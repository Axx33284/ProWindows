#include "hotkeys.h"

namespace awa {

namespace {

constexpr int kHotkeyBase = 1000;

HWND     g_target = nullptr;
Config*  g_cfg    = nullptr;

// Bindings served by the keyboard hook, in the order the hook reports them.
// UI thread only: the hook thread never touches these. It works from the chord
// snapshot below and reports back an index into it.
//
// These hold INDICES into cfg->binds, not pointers into it. The settings window
// applies its changes with `cfg.binds = g_editBinds`, which reallocates the
// vector and moves every element - so a Keybind* taken before that assignment
// dangles until HotkeysRegister runs again. Today nothing pumps a message in
// between and so nothing ever dereferences one, but that is an accident of
// where the calls happen to sit, not a property anybody could rely on.
std::vector<int> g_hooked;
std::vector<int> g_byId;               // index = hotkey id - kHotkeyBase, -1 = burnt

int g_blocked = 0;

// Resolves an index recorded above against the binding list as it stands now.
const Keybind* BindAt(int index) {
    if (!g_cfg || index < 0 || index >= (int)g_cfg->binds.size()) return nullptr;
    return &g_cfg->binds[(size_t)index];
}

Keybind* MutableBindAt(int index) {
    if (!g_cfg || index < 0 || index >= (int)g_cfg->binds.size()) return nullptr;
    return &g_cfg->binds[(size_t)index];
}

// ---------------------------------------------------------------- hook thread
// The hook used to live on the main thread. That thread also arranges windows,
// runs the animation, talks to the shell and waits on WM_GETMINMAXINFO replies
// from other processes - and a WH_KEYBOARD_LL callback is delivered by pumping
// the installing thread's message queue. So every keystroke in the session had
// to wait for whatever the tiler happened to be doing, and Windows silently
// removes a hook that answers too slowly (LowLevelHooksTimeout). That is the
// whole of "it types fine on this machine and stutters on the other one": a
// slower disk or a busier desktop makes the tiler's pauses longer than the
// timeout, and then the shortcuts stop working too.
//
// The hook now owns a thread that does nothing else. Its callback compares the
// keystroke against a small snapshot and posts an index; every bit of real work
// still happens on the UI thread, where it has to.
struct Chord { UINT mods; UINT vk; };

CRITICAL_SECTION g_lock;
bool   g_lockReady = false;
std::vector<Chord> g_chords;           // guarded by g_lock

// Bumped every time g_chords is replaced. The hook posts it alongside the index
// it matched, so a keystroke that was already in the message queue when the
// bindings changed is recognised as stale and dropped rather than running
// whichever action has since inherited that slot. Guarded by g_lock.
LONG g_chordGen = 0;

HANDLE g_thread   = nullptr;
DWORD  g_threadId = 0;
HANDLE g_ready    = nullptr;           // set once the thread has a message queue
HHOOK  g_hook     = nullptr;           // hook thread only
// The same fact as g_hook, published for the UI thread. A plain read of g_hook
// from another thread is something the optimiser is free to hoist out of a
// wait loop; this one it is not.
LONG   g_hookOn   = 0;

// Posted to the hook thread: install or remove the hook to match g_chords.
constexpr UINT WM_AWA_APPLYHOOK = WM_APP + 100;

// Keys we swallowed on the way down, so the matching key-up can be swallowed
// too and applications never see half a keystroke. Hook thread only.
//
// Each entry carries the moment it was added, because the matching key-up does
// not always arrive: the release can land while an elevated window has focus,
// or after the hook has been taken out and put back over a settings reload. An
// entry left behind would then eat the NEXT release of that key, which reaches
// the user as a modifier that has stuck down.
struct Swallowed { UINT vk; ULONGLONG at; };
std::vector<Swallowed> g_swallowed;

// Comfortably longer than anyone holds a shortcut, far shorter than the gap
// before they would press that key again for something else.
constexpr ULONGLONG kSwallowExpiryMs = 4000;

void ExpireSwallowed(ULONGLONG now) {
    g_swallowed.erase(
        std::remove_if(g_swallowed.begin(), g_swallowed.end(),
                       [&](const Swallowed& s) { return now - s.at > kSwallowExpiryMs; }),
        g_swallowed.end());
}

UINT CurrentModifiers() {
    UINT mods = 0;
    if (GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
    if (GetAsyncKeyState(VK_CONTROL) < 0)                               mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU) < 0)                                  mods |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT) < 0)                                 mods |= MOD_SHIFT;
    return mods;
}

// Stamped on the keystrokes we synthesise, so the hook can recognise its own
// work. Only these are skipped - input from macro keyboards, on-screen
// keyboards or remote sessions still triggers bindings normally.
constexpr ULONG_PTR kSelfInjected = 0x41574121;   // AWA!

// Pressing Win and releasing it without an intervening keystroke opens Start.
// We ate the intervening keystroke, so feed the shell a harmless one instead.
void MaskWinKey() {
    INPUT in[2] = {};
    in[0].type            = INPUT_KEYBOARD;
    in[0].ki.wVk          = VK_CONTROL;
    in[0].ki.dwFlags      = 0;
    in[0].ki.dwExtraInfo  = kSelfInjected;
    in[1]                 = in[0];
    in[1].ki.dwFlags      = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(g_hook, code, wp, lp);

    const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);

    // Our own masking keystroke comes back through here; never act on it.
    if (kb->dwExtraInfo == kSelfInjected) return CallNextHookEx(g_hook, code, wp, lp);

    const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
    const bool up   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);

    const ULONGLONG now = GetTickCount64();
    if (!g_swallowed.empty()) ExpireSwallowed(now);

    if (up) {
        auto it = std::find_if(g_swallowed.begin(), g_swallowed.end(),
                               [&](const Swallowed& s) { return s.vk == (UINT)kb->vkCode; });
        if (it != g_swallowed.end()) {
            g_swallowed.erase(it);
            return 1;                      // eat the release of a key we ate
        }
        return CallNextHookEx(g_hook, code, wp, lp);
    }

    if (down) {
        const UINT mods = CurrentModifiers();

        // The section is held for a handful of integer comparisons and never
        // across SendInput or a post: this callback is on the critical path of
        // every keystroke in the session.
        int  match = -1;
        LONG gen   = 0;
        EnterCriticalSection(&g_lock);
        gen = g_chordGen;
        for (size_t i = 0; i < g_chords.size(); ++i) {
            if (g_chords[i].vk == (UINT)kb->vkCode && g_chords[i].mods == mods) {
                match = (int)i;
                break;
            }
        }
        LeaveCriticalSection(&g_lock);

        if (match >= 0) {
            const bool known =
                std::any_of(g_swallowed.begin(), g_swallowed.end(),
                            [&](const Swallowed& s) { return s.vk == (UINT)kb->vkCode; });
            if (!known) g_swallowed.push_back({ (UINT)kb->vkCode, now });
            if (mods & MOD_WIN) MaskWinKey();

            // Index in the low half, chord generation in the high half.
            const WPARAM packed = ((WPARAM)(ULONG)gen << 32) | (WPARAM)(ULONG)match;
            PostMessageW(g_target, WM_AWA_HOOKKEY, packed, 0);
            return 1;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

// Installs or removes the hook so it matches whether there is anything to match.
// Hook thread only: a hook must be released by the thread that set it.
void ApplyHookState() {
    EnterCriticalSection(&g_lock);
    const bool wanted = !g_chords.empty();
    LeaveCriticalSection(&g_lock);

    if (wanted && !g_hook) {
        g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                   GetModuleHandleW(nullptr), 0);
        if (!g_hook) AWA_LOG(L"keyboard hook could not be installed");
    } else if (!wanted && g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        g_swallowed.clear();
    }
    InterlockedExchange(&g_hookOn, g_hook ? 1 : 0);
}

DWORD WINAPI HookThread(LPVOID) {
    // Every keystroke on the machine passes through this thread before the
    // application it was meant for sees it, so it must be scheduled ahead of
    // ordinary work. At normal priority it competes with everything else in
    // the process - the file indexer's first walk, the icon sweep, a retile -
    // and with every other program on a busy machine, which at logon is all
    // of them. A keystroke then waits for a timeslice, and typing feels
    // sticky exactly when the machine is under load. Highest rather than
    // time-critical: the callback is bounded, but a runaway at time-critical
    // would starve the UI thread that is supposed to act on what it posts.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // Force the queue into existence before anyone posts to this thread.
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    if (g_ready) SetEvent(g_ready);

    // Windows removes a low-level hook whose callback has been late too often
    // (LowLevelHooksTimeout), and it tells nobody: the hook simply stops
    // being called, and the Win+key bindings silently stop working for the
    // rest of the session. A thread of our own at high priority makes that
    // rare, not impossible - a machine paging at logon can starve anything.
    // So the hook is taken out and put back on a slow timer. It costs two
    // calls a minute, and a hook that was dropped is back within the minute.
    const UINT_PTR rehook = SetTimer(nullptr, 0, 60000, nullptr);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_AWA_APPLYHOOK) {
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

// Starts the hook thread if it is not already running. Returns false if it
// could not be started, in which case the hooked bindings simply do not work.
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

// Hands the hook thread the chords it should now be watching for. `wait` is for
// the one caller that asks whether the hook actually went in straight
// afterwards; removing it is not worth stalling a settings reload over.
void PublishChords(std::vector<Chord> chords, bool wait) {
    const bool empty = chords.empty();
    if (empty && !g_thread) return;        // nothing to do, nothing running
    if (!EnsureHookThread()) return;

    EnterCriticalSection(&g_lock);
    g_chords = std::move(chords);
    ++g_chordGen;          // anything already posted against the old list is stale
    LeaveCriticalSection(&g_lock);

    // There is no SendMessage equivalent for a thread queue, so the change is
    // posted and then, when it matters, waited for. It is one SetWindowsHookEx
    // call away and only ever happens on a settings reload.
    PostThreadMessageW(g_threadId, WM_AWA_APPLYHOOK, 0, 0);
    if (!wait) return;
    for (int i = 0; i < 200; ++i) {
        if ((InterlockedCompareExchange(&g_hookOn, 0, 0) != 0) != empty) break;
        Sleep(1);
    }
}

} // namespace

void HotkeysUnregister() {
    if (g_target) {
        for (size_t i = 0; i < g_byId.size(); ++i)
            UnregisterHotKey(g_target, kHotkeyBase + (int)i);
    }
    PublishChords({}, false);      // asks the hook thread to drop the hook
    g_byId.clear();
    g_hooked.clear();
    g_blocked = 0;
}

void HotkeysShutdown() {
    HotkeysUnregister();
    if (g_thread) {
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
        if (WaitForSingleObject(g_thread, 5000) != WAIT_OBJECT_0)
            AWA_LOG(L"keyboard hook thread did not stop in time");
        CloseHandle(g_thread);
        g_thread   = nullptr;
        g_threadId = 0;
    }
    if (g_ready) { CloseHandle(g_ready); g_ready = nullptr; }
    // g_lock is deliberately never deleted: a hook callback already in flight
    // when the thread was told to quit still enters it.
}

void HotkeysRegister(HWND target, Config* cfg) {
    HotkeysUnregister();
    g_target = target;
    g_cfg    = cfg;
    if (!target || !cfg) return;

    std::vector<Chord> chords;

    for (size_t i = 0; i < cfg->binds.size(); ++i) {
        Keybind& kb = cfg->binds[i];
        kb.id    = kHotkeyBase + (int)g_byId.size();
        kb.route = BindRoute::Unregistered;

        if (RegisterHotKey(target, kb.id, kb.mods | MOD_NOREPEAT, kb.vk)) {
            kb.route = BindRoute::Hotkey;
            g_byId.push_back((int)i);
            continue;
        }

        // The shell (or another app) already owns this chord.
        kb.id = 0;
        g_byId.push_back(-1);          // keep ids aligned with the slot we burned

        if (cfg->overrideReserved) {
            kb.route = BindRoute::Hook;
            g_hooked.push_back((int)i);
            chords.push_back({ kb.mods, kb.vk });
        } else {
            kb.route = BindRoute::Blocked;
            ++g_blocked;
        }
        AWA_LOG(L"chord busy, %s: %s", cfg->overrideReserved ? L"hooking" : L"blocked",
                kb.spec.c_str());
    }

    if (!chords.empty()) {
        PublishChords(std::move(chords), true);
        if (!HotkeysHookInstalled()) {
            // Without the hook those bindings simply do not work.
            for (int index : g_hooked)
                if (Keybind* kb = MutableBindAt(index)) kb->route = BindRoute::Blocked;
            g_blocked += (int)g_hooked.size();
            g_hooked.clear();
        }
    }
    AWA_LOG(L"bindings: %d direct, %d hooked, %d blocked",
            (int)g_byId.size() - (int)g_hooked.size() - g_blocked,
            (int)g_hooked.size(), g_blocked);
}

const Keybind* HotkeyForId(int hotkeyId) {
    const int slot = hotkeyId - kHotkeyBase;
    if (slot < 0 || slot >= (int)g_byId.size()) return nullptr;
    return BindAt(g_byId[(size_t)slot]);
}

const Keybind* HotkeyForHookIndex(WPARAM packed) {
    const int  index = (int)(ULONG)(packed & 0xFFFFFFFFu);
    const LONG gen   = (LONG)(ULONG)(packed >> 32);

    // Posted against a chord list that has since been replaced. The slot may
    // well still exist and hold a perfectly valid binding - just not the one
    // the user pressed.
    LONG current = 0;
    if (g_lockReady) {
        EnterCriticalSection(&g_lock);
        current = g_chordGen;
        LeaveCriticalSection(&g_lock);
    }
    if (gen != current) return nullptr;

    if (index < 0 || index >= (int)g_hooked.size()) return nullptr;
    return BindAt(g_hooked[(size_t)index]);
}

int  HotkeysBlockedCount()  { return g_blocked; }
int  HotkeysHookedCount()   { return (int)g_hooked.size(); }
bool HotkeysHookInstalled() {
    return InterlockedCompareExchange(&g_hookOn, 0, 0) != 0;
}

} // namespace awa
