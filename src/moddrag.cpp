#include "moddrag.h"
#include "hotkeys.h"
#include "winutil.h"
#include "wm.h"
#include "app.h"

namespace awa {

namespace {

// Posted to the hook thread when the wanted state changes. Same shape as the
// keyboard hook's: a hook has to be installed and removed by the thread that
// owns it, and that thread is not this one.
constexpr UINT WM_AWA_APPLYMOUSEHOOK = WM_APP + 101;

HWND g_target = nullptr;

// The modifier the hook is watching for, as a MOD_* mask. Zero means the
// gesture is off, which is also how the hook is asked to come out again.
// Interlocked because the hook thread reads it on every button press and the
// UI thread writes it on every settings reload.
LONG g_mods = 0;

// A button-down we swallowed, so its up can be let through deliberately rather
// than by accident. Left is 1, right is 2.
LONG g_swallowed = 0;

// Whether every key in g_mods is down right now, as last reported by the
// keyboard hook. The mouse hook is only installed while this is set: a
// WH_MOUSE_LL hook is called for every pointer movement on the machine, a
// thousand times a second on a gaming mouse, and all but a handful of those
// calls used to be a round trip into this process to decide "not a button,
// carry on". Keystrokes are a hundred times rarer, so watching the modifier
// from the keyboard hook and holding the mouse hook only while it is down
// costs almost nothing when the gesture is not being used - which is nearly
// always.
LONG g_modsHeld = 0;

// What the window manager is arranging, for the hook to test against. Sorted,
// so the hook's look-up is a binary search over a few dozen pointers rather
// than a scan; guarded like the keyboard hook's chord list, and for the same
// reason - held for comparisons only, never across a post.
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
std::vector<HWND> g_windows;    // guarded by g_lock

HANDLE g_thread   = nullptr;
DWORD  g_threadId = 0;
HANDLE g_ready    = nullptr;
HHOOK  g_hook     = nullptr;    // hook thread only
LONG   g_hookOn   = 0;          // the same fact, published for the UI thread

// Are exactly the modifier keys we want held right now?
//
// Exactly, not merely: Alt+drag and Alt+Shift+drag are different gestures, and
// a check that only asked "is Alt down" would claim both. GetAsyncKeyState is
// a read of a per-thread key state array - no cross-process call - which is
// what makes it safe to do inside a hook.
bool ModsHeld(UINT want) {
    const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool alt   = down(VK_MENU);
    const bool ctrl  = down(VK_CONTROL);
    const bool shift = down(VK_SHIFT);
    const bool win   = down(VK_LWIN) || down(VK_RWIN);

    UINT have = 0;
    if (alt)   have |= MOD_ALT;
    if (ctrl)  have |= MOD_CONTROL;
    if (shift) have |= MOD_SHIFT;
    if (win)   have |= MOD_WIN;
    return have == want;
}

bool IsManagedWindow(HWND h) {
    if (!h || !g_lockReady) return false;
    EnterCriticalSection(&g_lock);
    const bool found = std::binary_search(g_windows.begin(), g_windows.end(), h);
    LeaveCriticalSection(&g_lock);
    return found;
}

LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(g_hook, code, wp, lp);

    // Everything that is not a button press falls straight through, which is
    // every WM_MOUSEMOVE - by far the most of them. A hook that is slow here
    // is a pointer that stutters, and Windows removes a hook that is slow
    // enough for long enough.
    const bool isDown = (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN);
    const bool isUp   = (wp == WM_LBUTTONUP   || wp == WM_RBUTTONUP);
    if (!isDown && !isUp) return CallNextHookEx(g_hook, code, wp, lp);

    const LONG which = (wp == WM_LBUTTONDOWN || wp == WM_LBUTTONUP) ? 1 : 2;

    if (isUp) {
        // The up is deliberately *not* swallowed, even when its down was.
        // Windows' own move loop is waiting for it, and a low-level hook that
        // returns non-zero keeps the message from every application including
        // that loop - so swallowing it would leave the window stuck to the
        // pointer until something else broke the capture.
        if (InterlockedCompareExchange(&g_swallowed, 0, which) == which) {
            // Nothing to do beyond clearing the record.
        }
        return CallNextHookEx(g_hook, code, wp, lp);
    }

    const UINT want = (UINT)InterlockedCompareExchange(&g_mods, 0, 0);
    if (!want || !ModsHeld(want)) return CallNextHookEx(g_hook, code, wp, lp);

    // Whether this is a window we may pick up has to be settled here, before
    // the click is either swallowed or let through - a low-level hook does not
    // get to change its mind. WindowFromPoint and GetAncestor are local
    // look-ups in the window manager's own tables, not cross-process calls.
    const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
    HWND under = WindowFromPoint(ms->pt);
    if (under) under = GetAncestor(under, GA_ROOT);
    if (!IsManagedWindow(under)) return CallNextHookEx(g_hook, code, wp, lp);

    // Everything that has to *look* at the window still happens on the UI
    // thread; the hook has only decided that there is something to look at.
    if (g_target) PostMessageW(g_target, WM_AWA_MODDRAG, (WPARAM)which, 0);

    // The press itself is swallowed so the application underneath never sees
    // a click. Without this, mod+drag on a browser follows a link on the way
    // to moving the window.
    InterlockedExchange(&g_swallowed, which);
    return 1;
}

void ApplyHookState() {
    const bool wanted = InterlockedCompareExchange(&g_mods, 0, 0) != 0 &&
                        InterlockedCompareExchange(&g_modsHeld, 0, 0) != 0;

    if (wanted && !g_hook) {
        g_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                                   GetModuleHandleW(nullptr), 0);
        if (!g_hook) AWA_LOG(L"mod-drag: the mouse hook could not be installed");
    } else if (!wanted && g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        InterlockedExchange(&g_swallowed, 0);
    }
    InterlockedExchange(&g_hookOn, g_hook ? 1 : 0);
}

DWORD WINAPI HookThread(LPVOID) {
    // Every pointer movement on the machine passes through here. See the
    // keyboard hook's thread for why it runs ahead of ordinary work: a mouse
    // hook that waits for a timeslice is a pointer that stutters, and one
    // that waits too long is removed by the system.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    if (g_ready) SetEvent(g_ready);

    // Re-installed once a minute, for the reason the keyboard hook's thread
    // gives: a hook the system has quietly dropped comes back by itself.
    const UINT_PTR rehook = SetTimer(nullptr, 0, 60000, nullptr);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_AWA_APPLYMOUSEHOOK) {
            ApplyHookState();
        } else if (msg.message == WM_TIMER && msg.wParam == rehook && g_hook) {
            UnhookWindowsHookEx(g_hook);
            g_hook = nullptr;
            ApplyHookState();
        }
    }
    if (rehook) KillTimer(nullptr, rehook);

    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    InterlockedExchange(&g_hookOn, 0);
    return 0;
}

bool EnsureHookThread() {
    if (g_thread) return true;
    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }

    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_ready) return false;

    g_thread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_threadId);
    if (!g_thread) {
        CloseHandle(g_ready);
        g_ready = nullptr;
        return false;
    }
    // The first PostThreadMessage would be dropped if the queue did not exist.
    WaitForSingleObject(g_ready, 5000);
    return true;
}

// Which corner a resize should grab: the one the pointer is nearest, so the
// edge that moves is the edge under the hand. HT* codes are what
// WM_NCLBUTTONDOWN expects.
WPARAM ResizeCornerFor(const RECT& r, POINT pt) {
    const bool left = pt.x < (r.left + r.right) / 2;
    const bool top  = pt.y < (r.top + r.bottom) / 2;
    if (top)  return left ? HTTOPLEFT    : HTTOPRIGHT;
    return           left ? HTBOTTOMLEFT : HTBOTTOMRIGHT;
}

} // namespace

void ModDragInit(HWND target) {
    g_target = target;
    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
}

void ModDragPublishWindows(const std::vector<HWND>& windows) {
    if (!g_lockReady) return;
    // Sorted here, on the caller's thread, so the hook never does more than
    // compare. The manager hands this over on every window open and close,
    // which on a busy desktop is a few times a second at most.
    std::vector<HWND> sorted = windows;
    std::sort(sorted.begin(), sorted.end());

    EnterCriticalSection(&g_lock);
    g_windows = std::move(sorted);
    LeaveCriticalSection(&g_lock);
}

void ModDragApplyConfig(const Config* cfg) {
    const LONG want = (cfg && cfg->modDrag) ? (LONG)cfg->modMask : 0;
    InterlockedExchange(&g_mods, want);
    InterlockedExchange(&g_modsHeld, 0);
    // The keyboard hook watches the modifier for us; the mouse hook thread is
    // started now, idle, so the first press of the modifier only has to post
    // to it rather than create it.
    HotkeysWatchModifiers(want != 0);
    if (!want && !g_thread) return;              // nothing on, nothing running
    if (!EnsureHookThread()) return;
    PostThreadMessageW(g_threadId, WM_AWA_APPLYMOUSEHOOK, 0, 0);
}

void ModDragModifier(UINT vk, bool down) {
    const UINT want = (UINT)InterlockedCompareExchange(&g_mods, 0, 0);
    if (!want || !g_thread) return;

    // The key that caused this event is not yet in the async key state when
    // a low-level hook sees it, so it is taken from the event and the rest
    // from the state.
    auto pressed = [&](int left, int right, int generic) {
        if (vk == (UINT)left || vk == (UINT)right || vk == (UINT)generic) return down;
        return (GetAsyncKeyState(left) & 0x8000) != 0 ||
               (GetAsyncKeyState(right) & 0x8000) != 0;
    };
    bool held = true;
    if (want & MOD_ALT)     held = held && pressed(VK_LMENU, VK_RMENU, VK_MENU);
    if (want & MOD_CONTROL) held = held && pressed(VK_LCONTROL, VK_RCONTROL, VK_CONTROL);
    if (want & MOD_SHIFT)   held = held && pressed(VK_LSHIFT, VK_RSHIFT, VK_SHIFT);
    if (want & MOD_WIN)     held = held && pressed(VK_LWIN, VK_RWIN, VK_LWIN);

    const LONG was = InterlockedExchange(&g_modsHeld, held ? 1 : 0);
    if ((was != 0) != held)
        PostThreadMessageW(g_threadId, WM_AWA_APPLYMOUSEHOOK, 0, 0);
}

void ModDragShutdown() {
    InterlockedExchange(&g_mods, 0);
    InterlockedExchange(&g_modsHeld, 0);
    if (g_thread) {
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_ready) { CloseHandle(g_ready); g_ready = nullptr; }
    g_target = nullptr;
}

bool ModDragHookInstalled() {
    return InterlockedCompareExchange(&g_hookOn, 0, 0) != 0;
}

void ModDragBegin(WPARAM wp) {
    const bool resize = (wp == 2);

    // Is the button still down?
    //
    // This is the failure worth guarding against by name. The hook posts and
    // returns; the loop it is asking for ends when the button is released. If
    // the UI thread was busy - a retile mid-animation, a settings save - the
    // release can happen before this runs, and then WM_NCLBUTTONDOWN starts a
    // modal loop waiting for a button-up that has already been and gone. The
    // window follows the pointer around the screen until the next click, which
    // is the single most alarming thing a window manager can do.
    //
    // Swapped buttons are a real setting, and GetAsyncKeyState reports the
    // physical button rather than the logical one, so the codes are swapped
    // back here to match what the hook actually saw.
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    const int  vk = resize ? (swapped ? VK_LBUTTON : VK_RBUTTON)
                           : (swapped ? VK_RBUTTON : VK_LBUTTON);
    if (!(GetAsyncKeyState(vk) & 0x8000)) return;

    POINT pt;
    GetCursorPos(&pt);

    HWND under = WindowFromPoint(pt);
    if (!under) return;
    under = GetAncestor(under, GA_ROOT);
    if (!under) return;

    // Only windows this application is already arranging. That is not merely a
    // safety check - it is what makes the excluded-apps list mean something
    // here too: an app on the never-arrange list keeps its own mod+drag, which
    // is the point of having excluded it.
    // Asked again here rather than trusted from the hook: the pointer can move
    // between the two, and this is the answer the action is actually taken on.
    WindowManager& wm = AppWm();
    if (wm.GameMode() || !wm.Manages(under)) return;

    // A window at a higher integrity level will not accept a posted message,
    // and UIPI reports that as success. Nothing to do about it; the gesture
    // simply does nothing on those windows, exactly as arranging them does.
    RECT r{};
    if (!GetWindowRect(under, &r)) return;

    // Bring it forward first. Windows does this itself for a real title-bar
    // click, and without it a mod+drag on a window that is behind another one
    // moves a window the user cannot see.
    FocusWindow(under);

    // The system's own modal move or size loop, started exactly as a real
    // click on the frame starts it - so the tiler's existing drag handling
    // sees the ordinary MOVESIZESTART / MOVESIZEEND pair and does the rest.
    // Posted rather than sent: the loop does not return until the button is
    // released, and this is the UI thread.
    const WPARAM hit = resize ? ResizeCornerFor(r, pt) : (WPARAM)HTCAPTION;
    PostMessageW(under, WM_NCLBUTTONDOWN, hit, MAKELPARAM(pt.x, pt.y));
}

} // namespace awa
