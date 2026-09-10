#include "wm.h"
#include "moddrag.h"
#include "app.h"
#include "dragguide.h"
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

namespace awa {

// ================================================================ crash safety
// Every window we have hidden, in a plain array with a plain count. Kept in
// step with ManagedWindow::hidden by SetHidden, and read by nothing except the
// unhandled-exception filter - see EmergencyUnhideAll at the bottom of this
// file for why it cannot use the ordinary tables.
namespace {

constexpr int kMaxHiddenTracked = 512;
HWND g_hiddenWindows[kMaxHiddenTracked] = {};

// Only ever written by the UI thread, but read by whichever thread happened to
// fault - so every access goes through an interlocked read, not just the
// writes. Mixing the two is exactly the pattern that lets a compiler cache the
// value in a register across the loop below.
LONG g_hiddenCount = 0;

LONG HiddenCount() {
    LONG n = InterlockedCompareExchange(&g_hiddenCount, 0, 0);
    if (n < 0) n = 0;
    if (n > kMaxHiddenTracked) n = kMaxHiddenTracked;
    return n;
}

void TrackHidden(HWND h) {
    const LONG n = HiddenCount();
    for (LONG i = 0; i < n; ++i)
        if (g_hiddenWindows[i] == h) return;
    if (n >= kMaxHiddenTracked) return;
    g_hiddenWindows[n] = h;
    // Published after the slot is filled, so a crash between the two leaves the
    // array short by one entry rather than holding an uninitialised handle.
    InterlockedExchange(&g_hiddenCount, n + 1);
}

void UntrackHidden(HWND h) {
    const LONG n = HiddenCount();
    for (LONG i = 0; i < n; ++i) {
        if (g_hiddenWindows[i] != h) continue;
        g_hiddenWindows[i] = g_hiddenWindows[n - 1];
        InterlockedExchange(&g_hiddenCount, n - 1);
        return;
    }
}

} // namespace

void EmergencyUnhideAll() {
    const LONG n = HiddenCount();
    for (LONG i = 0; i < n; ++i)
        if (g_hiddenWindows[i] && IsWindow(g_hiddenWindows[i]))
            ShowWindow(g_hiddenWindows[i], SW_SHOWNA);
    InterlockedExchange(&g_hiddenCount, 0);
}

// ================================================================ lifecycle
bool WindowManager::Init(HWND msgWnd, Config* cfg) {
    msgWnd_ = msgWnd;
    cfg_    = cfg;
    tilingEnabled_ = cfg->tilingEnabled;
    gapsEnabled_   = true;

    ReloadMonitors();
    // Before anything is scanned or moved: starting while a game is already
    // running must not rearrange the desktop behind it.
    UpdateGameMode(true);
    ScanExistingWindows();
    RetileNow();
    UpdateBorders();
    return true;
}

void WindowManager::Shutdown() {
    // Reached twice on a session end - once from WM_QUERYENDSESSION, once from
    // WM_ENDSESSION - and once more from the ordinary exit path if the user
    // closes the app while Windows is logging out. Doing any of this twice
    // means writing the config twice and tearing down a ticker thread that is
    // already gone.
    if (shutdown_) return;
    shutdown_ = true;

    if (msgWnd_) {
        KillTimer(msgWnd_, TIMER_RETILE);
        KillTimer(msgWnd_, TIMER_PENDING);
        KillTimer(msgWnd_, TIMER_DRAG);
    }
    pending_.clear();

    AnimShutdown();             // also releases the raised timer resolution

    // Everything still open has had a chance to prove what sizes it accepts.
    // Write that down once, here, rather than touching the config file every
    // time a single number changes.
    for (const auto& kv : managed_) RememberLimits(kv.second);
    if (limitsDirty_) {
        limitsDirty_ = false;
        AppSaveConfig();
    }

    RestoreAllWindows();
    if (cfg_ && cfg_->accentBorder)
        for (auto& kv : managed_) SetBorderColor(kv.first, 0, false);
}

void WindowManager::ReloadMonitors() {
    Busy guard(this);
    auto infos = EnumMonitors();
    if (infos.empty()) return;

    // EnumMonitors sorts left-to-right, and ManagedWindow::monitor is an index
    // into that order - so rearranging displays in Settings, or a monitor
    // waking in a different position, renumbers every monitor under us. Work
    // out where each old index has moved to before the old vector is replaced;
    // -1 means that display is gone.
    std::vector<int> remap(monitors_.size(), -1);
    for (size_t oldIdx = 0; oldIdx < monitors_.size(); ++oldIdx) {
        for (size_t newIdx = 0; newIdx < infos.size(); ++newIdx) {
            if (monitors_[oldIdx].info.handle == infos[newIdx].handle) {
                remap[oldIdx] = (int)newIdx;
                break;
            }
        }
    }

    std::vector<Monitor> fresh;
    fresh.reserve(infos.size());

    for (const auto& info : infos) {
        Monitor m;
        m.info = info;

        // Carry over the workspaces of a monitor we already knew about.
        int old = -1;
        for (size_t i = 0; i < monitors_.size(); ++i)
            if (monitors_[i].info.handle == info.handle) { old = (int)i; break; }

        if (old >= 0) {
            m.workspaces = std::move(monitors_[old].workspaces);
            m.active     = monitors_[old].active;
        }
        while ((int)m.workspaces.size() < cfg_->workspaceCount) {
            Workspace ws;
            ws.layout      = cfg_->layout;
            ws.masterRatio = cfg_->masterRatio;
            ws.masterCount = cfg_->masterCount;
            m.workspaces.push_back(std::move(ws));
        }
        if (m.active >= (int)m.workspaces.size()) m.active = 0;
        fresh.push_back(std::move(m));
    }

    monitors_ = std::move(fresh);

    auto remapped = [&](int oldIndex) {
        return (oldIndex >= 0 && oldIndex < (int)remap.size()) ? remap[oldIndex] : -1;
    };

    activeMonitor_ = remapped(activeMonitor_);
    if (activeMonitor_ < 0 || activeMonitor_ >= (int)monitors_.size()) activeMonitor_ = 0;

    // Follow every window to wherever its display ended up. The workspace lists
    // travelled with the handle, so a remapped index still points at the very
    // same Workspace the window is listed in.
    for (auto& kv : managed_) kv.second.monitor = remapped(kv.second.monitor);

    // Whatever is left was on a display that has gone; it lands on the first.
    for (auto& kv : managed_) {
        if (kv.second.monitor < 0 || kv.second.monitor >= (int)monitors_.size()) {
            kv.second.monitor = 0;
            Monitor& m = monitors_[0];
            if (kv.second.workspace >= (int)m.workspaces.size()) kv.second.workspace = 0;
            Workspace& ws = m.workspaces[kv.second.workspace];
            auto& list = kv.second.floating ? ws.floats : ws.tiled;
            if (std::find(list.begin(), list.end(), kv.first) == list.end())
                list.push_back(kv.first);
            if (!kv.second.floating) ws.tree.Insert(kv.first, nullptr, false);
        }
    }

    // Windows have just been moved between displays, and each display has its
    // own active workspace - so the invariant "visible exactly when on the
    // active workspace" no longer holds for any of them. Without this, a window
    // rehomed from a display that was unplugged could stay hidden on the
    // workspace the user is looking at: still managed, still counted, and
    // completely unreachable. Collected before it is applied, because
    // ShowWindow pumps.
    std::vector<std::pair<HWND, bool>> flips;
    flips.reserve(managed_.size());
    for (const auto& kv : managed_) {
        const Monitor* m = (kv.second.monitor >= 0 &&
                            kv.second.monitor < (int)monitors_.size())
                           ? &monitors_[kv.second.monitor] : nullptr;
        if (!m) continue;
        flips.push_back({ kv.first, kv.second.workspace != m->active });
    }
    for (const auto& f : flips)
        if (ManagedWindow* mw = Find(f.first)) SetHidden(mw, f.second);

    AWA_LOG(L"monitors: %d", (int)monitors_.size());
}

void WindowManager::ScanExistingWindows() {
    Busy guard(this);
    // Collect first, then classify: AddWindow can call into Win32 in ways that
    // are unsafe to interleave with an active EnumWindows walk.
    std::vector<HWND> found;
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));

    for (HWND h : found) AddWindow(h, false);
    AWA_LOG(L"scan: managing %d windows", (int)managed_.size());
}

void WindowManager::ApplyConfigChanged() {
    Busy guard(this);
    tilingEnabled_ = cfg_->tilingEnabled;

    for (int mi = 0; mi < (int)monitors_.size(); ++mi) {
        Monitor& m = monitors_[mi];

        while ((int)m.workspaces.size() < cfg_->workspaceCount) {
            Workspace ws;
            ws.layout      = cfg_->layout;
            ws.masterRatio = cfg_->masterRatio;
            ws.masterCount = cfg_->masterCount;
            m.workspaces.push_back(std::move(ws));
        }

        // Shrinking the workspace count has to rescue anything parked on the
        // workspaces that are about to disappear - those windows are hidden, so
        // leaving them behind would make them unreachable.
        if ((int)m.workspaces.size() > cfg_->workspaceCount) {
            const int last = cfg_->workspaceCount - 1;
            std::vector<HWND> strays;
            for (auto& kv : managed_)
                if (kv.second.monitor == mi && kv.second.workspace > last)
                    strays.push_back(kv.first);
            for (HWND h : strays) MoveWindowToWorkspace(h, mi, last, false);

            if (m.active > last) m.active = last;
            m.workspaces.resize((size_t)cfg_->workspaceCount);
        }
        if (m.active >= (int)m.workspaces.size()) m.active = 0;

        // Restore the invariant: visible exactly when on the active workspace.
        // Collected first, for the reason ActSwitchWorkspace gives.
        std::vector<std::pair<HWND, bool>> flips;
        for (const auto& kv : managed_) {
            if (kv.second.monitor != mi) continue;
            flips.push_back({ kv.first, kv.second.workspace != m.active });
        }
        for (const auto& f : flips)
            if (ManagedWindow* mw = Find(f.first)) SetHidden(mw, f.second);
    }

    // Turning the focus border off has to clear the ones already painted.
    if (!cfg_->accentBorder) {
        for (auto& kv : managed_) SetBorderColor(kv.first, 0, false);
        lastBordered_ = nullptr;
    }

    RetileNow();
    UpdateBorders();
}

// ================================================================ lookups
ManagedWindow* WindowManager::Find(HWND h) {
    auto it = managed_.find(h);
    return it == managed_.end() ? nullptr : &it->second;
}

Workspace* WindowManager::WorkspaceFor(const ManagedWindow& mw) {
    if (mw.monitor < 0 || mw.monitor >= (int)monitors_.size()) return nullptr;
    Monitor& m = monitors_[mw.monitor];
    if (mw.workspace < 0 || mw.workspace >= (int)m.workspaces.size()) return nullptr;
    return &m.workspaces[mw.workspace];
}

Workspace* WindowManager::ActiveWorkspaceOf(int monitorIndex) {
    if (monitorIndex < 0 || monitorIndex >= (int)monitors_.size()) return nullptr;
    Monitor& m = monitors_[monitorIndex];
    if (m.workspaces.empty()) return nullptr;
    return &m.workspaces[m.active];
}

Monitor* WindowManager::MonitorAt(int index) {
    if (index < 0 || index >= (int)monitors_.size()) return nullptr;
    return &monitors_[index];
}

Monitor* WindowManager::ActiveMonitor() {
    if (monitors_.empty()) return nullptr;
    if (activeMonitor_ < 0 || activeMonitor_ >= (int)monitors_.size()) activeMonitor_ = 0;
    return &monitors_[activeMonitor_];
}

int WindowManager::MonitorIndexFor(HMONITOR mon) const {
    for (size_t i = 0; i < monitors_.size(); ++i)
        if (monitors_[i].info.handle == mon) return (int)i;
    return 0;
}

int WindowManager::MonitorIndexForWindow(HWND h) const {
    return MonitorIndexFor(MonitorOf(h));
}

LayoutKind WindowManager::ActiveLayout() const {
    // Callable before Init() (the tray tooltip is built early), so tolerate a
    // manager that has no config and no monitors yet.
    const LayoutKind fallback = cfg_ ? cfg_->layout : LayoutKind::Dwindle;
    if (monitors_.empty()) return fallback;
    const size_t mi = (activeMonitor_ >= 0 && activeMonitor_ < (int)monitors_.size())
                      ? (size_t)activeMonitor_ : 0;
    const Monitor& m = monitors_[mi];
    if (m.workspaces.empty()) return fallback;
    // m.active is normally kept in range, but this is read by the tray tooltip
    // during a monitor change, when it briefly is not.
    if (m.active < 0 || m.active >= (int)m.workspaces.size()) return fallback;
    return m.workspaces[m.active].layout;
}

int WindowManager::ActiveWorkspace() const {
    if (monitors_.empty()) return 0;
    const size_t mi = (activeMonitor_ >= 0 && activeMonitor_ < (int)monitors_.size())
                      ? (size_t)activeMonitor_ : 0;
    const Monitor& m = monitors_[mi];
    return (m.active >= 0 && m.active < (int)m.workspaces.size()) ? m.active : 0;
}

// ================================================================ membership
bool WindowManager::AddWindow(HWND h, bool focusIt) {
    if (!h || Find(h) || monitors_.empty()) return false;

    IgnoreReason why = IgnoreReason::None;
    ManageVerdict verdict = Classify(h, *cfg_, &why);

    // A window we are not allowed to move. It takes no part in the layout -
    // reserving a tile for one leaves a rectangle of bare desktop - but it is
    // remembered so the count can be shown and explained.
    if (verdict == ManageVerdict::Blocked) {
        ForgetPending(h);
        if (blocked_.insert(h).second && !blockedAnnounced_ && !SelfIsElevated()) {
            blockedAnnounced_ = true;
            AppTrayBalloon(kAppName,
                L"A window is running as administrator, so Windows will not let "
                L"ProWindows arrange it. Use \"Restart as administrator\" in the "
                L"tray menu to include windows like it.");
        }
        return false;
    }
    if (verdict == ManageVerdict::Ignore) {
        // The whole of "it did not arrange until I moved it": an application
        // creates its window and only then gives it a title, applies its
        // styles and lets DWM un-cloak it, and every one of those states makes
        // Classify say no. Come back and ask again rather than writing the
        // window off on the strength of its first millisecond.
        if (why == IgnoreReason::Transient) WatchForLater(h);
        else                                ForgetPending(h);
        return false;
    }
    if (!IsOnCurrentVirtualDesktop(h)) {
        // The desktop association is assigned asynchronously as a window
        // opens, so this too is worth asking about again.
        WatchForLater(h);
        return false;
    }

    ForgetPending(h);

    ManagedWindow mw;
    mw.hwnd      = h;
    mw.monitor   = MonitorIndexForWindow(h);
    mw.floating  = (verdict == ManageVerdict::Float);
    mw.savedRect = VisibleRect(h);
    mw.minimized = IsMinimizedWnd(h);
    mw.limitKey  = LimitKeyFor(h);

    // Start from what this kind of window turned out to insist on last time.
    // Without this, every run rediscovers it the only way it can be
    // discovered - by handing the window a size it refuses and watching it
    // overhang its neighbours until the next pass corrects the layout.
    auto known = cfg_->learnedLimits.find(mw.limitKey);
    if (known != cfg_->learnedLimits.end()) {
        const Config::RememberedLimits& r = known->second;
        mw.limits.minW = r.minW;
        mw.limits.minH = r.minH;
        if (r.maxW > 0) mw.limits.maxW = r.maxW;
        if (r.maxH > 0) mw.limits.maxH = r.maxH;
        mw.tooLarge = r.tooLarge;
    }

    Monitor& mon = monitors_[mw.monitor];
    mw.workspace = mon.active;
    Workspace& ws = mon.workspaces[mw.workspace];

    if (mw.floating) {
        ws.floats.push_back(h);
    } else {
        HWND beside = (focused_ && Find(focused_) &&
                       std::find(ws.tiled.begin(), ws.tiled.end(), focused_) != ws.tiled.end())
                      ? focused_ : nullptr;
        auto pos = beside ? std::find(ws.tiled.begin(), ws.tiled.end(), beside) : ws.tiled.end();
        if (cfg_->newWindowOnTop && pos != ws.tiled.end()) ws.tiled.insert(pos, h);
        else                                               ws.tiled.push_back(h);

        // A window adopted while it is minimised belongs on the workspace's
        // list exactly like any other - it is simply skipped by every pass
        // until it comes back up. Leaving it off the list, which is what used
        // to happen, put it in `managed_` and in no workspace at all: restoring
        // it then inserted a leaf into the tree that the very next pass pruned
        // straight back out, because the pass builds its world from `tiled`.
        // The window was never arranged again for the rest of the session, and
        // "some windows just do not tile" is what that looked like.
        //
        // The tree is left alone while it is down: a minimised window takes no
        // part in the layout, and the pass re-inserts it on the way back.
        if (!mw.minimized) ws.tree.Insert(h, beside, cfg_->newWindowOnTop);
    }

    managed_[h] = mw;
    PublishManagedWindows();

    if (cfg_->accentBorder) SetBorderColor(h, cfg_->inactiveColor, true);
    if (cfg_->cornerPref)   SetCornerPreference(h, cfg_->cornerPref);

    AWA_LOG(L"add %p [%s] %s", (void*)h, WindowClass(h).c_str(),
            mw.floating ? L"float" : L"tile");

    if (focusIt) {
        focused_ = h;
        activeMonitor_ = mw.monitor;
        ws.lastFocused = h;
    }
    return true;
}

// Adds a window, taking focus only if it genuinely has it. EVENT_OBJECT_SHOW
// arrives for windows that open in the background just as readily as for ones
// the user asked for, and treating those as focused quietly moved the active
// monitor and pointed every directional action at a window nobody had looked
// at yet.
bool WindowManager::AdoptWindow(HWND h) {
    return AddWindow(h, h != nullptr && h == GetForegroundWindow());
}

// ================================================================ second looks
void WindowManager::WatchForLater(HWND h) {
    if (!h || !msgWnd_ || gameMode_) return;
    if (pending_.find(h) != pending_.end()) return;   // already being watched
    // A ceiling, because this list is fed by window events and a desktop can
    // produce a great many of those. Losing the sixty-fifth candidate in a
    // burst is better than growing without bound.
    if (pending_.size() >= kMaxPending) return;

    const bool wasEmpty = pending_.empty();
    pending_.emplace(h, Pending{ 0, GetTickCount64() });
    // The timer runs exactly while the list is occupied, which is what keeps
    // an idle desktop genuinely idle.
    if (wasEmpty) SetTimer(msgWnd_, TIMER_PENDING, kPendingIntervalMs, nullptr);
}

void WindowManager::ForgetPending(HWND h) {
    if (pending_.empty()) return;
    pending_.erase(h);
    if (pending_.empty() && msgWnd_) KillTimer(msgWnd_, TIMER_PENDING);
}

void WindowManager::RetryPending() {
    Busy guard(this);
    if (!msgWnd_) return;
    if (pending_.empty() || gameMode_) {
        // Nothing is arranged while a fullscreen application owns the screen,
        // and UpdateGameMode rescans from scratch when it lets go.
        pending_.clear();
        KillTimer(msgWnd_, TIMER_PENDING);
        return;
    }

    // Collected first: AddWindow edits pending_, and iterating a map while it
    // is being erased from underneath is how this kind of code goes wrong.
    std::vector<HWND> ready;
    ready.reserve(pending_.size());

    for (auto it = pending_.begin(); it != pending_.end(); ) {
        if (!IsWindow(it->first)) { it = pending_.erase(it); continue; }
        if (++it->second.tries > kPendingTries) {
            // Two seconds of asking. Whatever this window is, it is not one
            // that was merely half-built when we first saw it.
            it = pending_.erase(it);
            continue;
        }
        ready.push_back(it->first);
        ++it;
    }

    bool added = false;
    for (HWND h : ready) if (AdoptWindow(h)) added = true;
    if (added) RequestRetile();

    if (pending_.empty()) KillTimer(msgWnd_, TIMER_PENDING);
}

void WindowManager::RemoveWindow(HWND h) {
    blocked_.erase(h);
    ForgetPending(h);
    scratch_.erase(std::remove(scratch_.begin(), scratch_.end(), h), scratch_.end());
    if (scratchShown_ == h) scratchShown_ = nullptr;

    auto it = managed_.find(h);
    if (it == managed_.end()) return;

    ManagedWindow mw = it->second;
    managed_.erase(it);
    PublishManagedWindows();

    // Last chance to keep what this window taught us: it is about to go, and
    // the next one of its kind should not have to learn it again.
    RememberLimits(mw);

    if (Workspace* ws = WorkspaceFor(mw)) {
        ws->tiled.erase(std::remove(ws->tiled.begin(), ws->tiled.end(), h), ws->tiled.end());
        ws->floats.erase(std::remove(ws->floats.begin(), ws->floats.end(), h), ws->floats.end());
        ws->tree.Remove(h);
        if (ws->lastFocused == h) ws->lastFocused = ws->tiled.empty() ? nullptr : ws->tiled.front();
    }
    if (focused_ == h)      focused_ = nullptr;
    if (lastFocused_ == h)  lastFocused_ = nullptr;
    if (lastBordered_ == h) lastBordered_ = nullptr;

    anim_.erase(std::remove_if(anim_.begin(), anim_.end(),
        [&](const AnimEntry& e) { return e.hwnd == h; }), anim_.end());
    // The carry list too. A handle is reused the moment the window is gone, and
    // AnimCommit's IsWindow check passes for the *new* owner of that handle -
    // so an entry left behind here does not do nothing, it moves somebody
    // else's window to where this one was going.
    animCarry_.erase(std::remove_if(animCarry_.begin(), animCarry_.end(),
        [&](const AnimEntry& e) { return e.hwnd == h; }), animCarry_.end());

    AWA_LOG(L"remove %p", (void*)h);
}

void WindowManager::SetHidden(ManagedWindow* mw, bool hide) {
    if (!mw || mw->hidden == hide) return;

    // The flag is set BEFORE the call, and that ordering is what makes the
    // echo harmless: EVENT_OBJECT_HIDE for a window we hid ourselves arrives
    // some time later, and OnWinEvent recognises it by finding hidden already
    // true. Reversing these two lines would make every workspace switch look
    // like every window on it had just been closed.
    mw->hidden = hide;
    if (hide) TrackHidden(mw->hwnd);
    else      UntrackHidden(mw->hwnd);
    ShowWindow(mw->hwnd, hide ? SW_HIDE : SW_SHOWNA);
}

void WindowManager::MoveWindowToWorkspace(HWND h, int monitorIndex, int workspaceIndex,
                                          bool follow) {
    Busy guard(this);
    ManagedWindow* mw = Find(h);
    if (!mw) return;
    if (monitorIndex < 0 || monitorIndex >= (int)monitors_.size()) return;
    Monitor& dstMon = monitors_[monitorIndex];
    if (workspaceIndex < 0 || workspaceIndex >= (int)dstMon.workspaces.size()) return;

    // Being sent to a workspace is how a window leaves the scratchpad - the
    // same escape route i3 has, and the only one. Without this a parked window
    // could be moved onto a workspace and still be skipped by every pass, which
    // is a window that exists, is visible, and is arranged by nothing.
    const bool leavingScratchpad = mw->scratch;
    if (leavingScratchpad) {
        mw->scratch = false;
        scratch_.erase(std::remove(scratch_.begin(), scratch_.end(), h), scratch_.end());
        if (scratchShown_ == h) scratchShown_ = nullptr;
        AWA_LOG(L"scratchpad: released %p onto workspace %d", (void*)h, workspaceIndex + 1);
    }

    if (!leavingScratchpad &&
        mw->monitor == monitorIndex && mw->workspace == workspaceIndex) return;

    if (Workspace* src = WorkspaceFor(*mw)) {
        src->tiled.erase(std::remove(src->tiled.begin(), src->tiled.end(), h), src->tiled.end());
        src->floats.erase(std::remove(src->floats.begin(), src->floats.end(), h), src->floats.end());
        src->tree.Remove(h);
        if (src->lastFocused == h) src->lastFocused = src->tiled.empty() ? nullptr : src->tiled.front();
    }

    const int srcMonitor = mw->monitor;
    mw->monitor   = monitorIndex;
    mw->workspace = workspaceIndex;

    Workspace& dst = dstMon.workspaces[workspaceIndex];
    if (mw->floating) {
        dst.floats.push_back(h);
        // Keep floating windows on-screen when they change monitor.
        Monitor* srcMon = MonitorAt(srcMonitor);
        if (srcMonitor != monitorIndex && srcMon) {
            Rect r = VisibleRect(h);
            const Rect& from = srcMon->info.work;
            const Rect& to   = dstMon.info.work;
            r.x = to.x + (r.x - from.x);
            r.y = to.y + (r.y - from.y);
            PlaceWindow(h, r, nullptr);
        }
    } else {
        dst.tiled.push_back(h);
        dst.tree.Insert(h, dst.lastFocused, false);
    }

    const bool visible = (dstMon.active == workspaceIndex);
    SetHidden(mw, !visible);

    if (follow && visible) {
        activeMonitor_ = monitorIndex;
        dst.lastFocused = h;
        FocusAndRemember(h);
    }
    RequestRetile();
}

// ================================================================ fullscreen apps
// Everything this manager does is a cross-process SetWindowPos, a DWM attribute
// write or a foreground change, and every one of those costs a game frames -
// a DWM attribute write on some drivers costs it exclusive fullscreen outright.
// So while a fullscreen application owns the screen the manager does nothing at
// all: no tiling, no animation, no borders, no focus polling. That is the whole
// of the "it breaks when a game is running" report.
void WindowManager::UpdateGameMode(bool force) {
    Busy guard(this);
    if (!cfg_ || !cfg_->pauseForFullscreen) {
        if (gameMode_) {
            gameMode_ = false;
            AppGameModeChanged(false);
            RequestRetile();
        }
        return;
    }

    // Asked on every foreground change and on every retile, which during a
    // burst of window events is a great many times a second. The shell call
    // behind it is not free, and the answer cannot change meaningfully inside
    // half a second.
    const ULONGLONG now = GetTickCount64();
    if (!force && now - gameCheckedAt_ < 500) return;
    gameCheckedAt_ = now;

    const bool active = FullscreenAppActive();
    if (active == gameMode_) return;
    gameMode_ = active;

    if (gameMode_) {
        AWA_LOG(L"fullscreen application detected: pausing");
        AnimStop();
        pending_.clear();
        if (msgWnd_) {
            KillTimer(msgWnd_, TIMER_RETILE);
            KillTimer(msgWnd_, TIMER_ANIM);
            KillTimer(msgWnd_, TIMER_PENDING);
        }
        retilePending_ = false;
        // Anything still holding an accent border would keep a DWM attribute
        // set on a window the game may be compositing with.
        if (cfg_->accentBorder && lastBordered_ && IsWindow(lastBordered_)) {
            SetBorderColor(lastBordered_, 0, false);
            lastBordered_ = nullptr;
        }
    } else {
        AWA_LOG(L"fullscreen application gone: resuming");
    }

    // Lets the shell stop its own timers and hide the monitor overlay, which
    // is topmost and would otherwise be drawn over the game every second.
    AppGameModeChanged(gameMode_);
    if (!gameMode_) {
        // Everything that opened while the game had the screen was waved past:
        // OnWinEvent does nothing but bookkeeping in game mode, and
        // OnForeground turns back before it can adopt anything. Without a
        // rescan here every one of those windows stays unmanaged for the rest
        // of the session - and since a fullscreen video counts as a game, that
        // is not an exotic case.
        ScanExistingWindows();
        RequestRetile();
        UpdateBorders();
    }
}

// ================================================================ learned limits
std::wstring WindowManager::LimitKeyFor(HWND h) const {
    std::wstring key = ToLower(ProcessName(h));
    key += L'|';
    key += ToLower(WindowClass(h));
    return key;
}

void WindowManager::RememberLimits(const ManagedWindow& mw) {
    if (!cfg_ || mw.limitKey.empty() || mw.limitKey == L"|") return;

    Config::RememberedLimits rec;
    rec.minW     = mw.limits.minW;
    rec.minH     = mw.limits.minH;
    rec.maxW     = (mw.limits.maxW >= kNoLimit) ? 0 : mw.limits.maxW;
    rec.maxH     = (mw.limits.maxH >= kNoLimit) ? 0 : mw.limits.maxH;
    rec.tooLarge = mw.tooLarge;

    if (!rec.minW && !rec.minH && !rec.maxW && !rec.maxH && !rec.tooLarge) return;

    auto it = cfg_->learnedLimits.find(mw.limitKey);
    if (it != cfg_->learnedLimits.end()) {
        // Nothing new: do not mark the config dirty and do not rewrite it.
        if (it->second.minW == rec.minW && it->second.minH == rec.minH &&
            it->second.maxW == rec.maxW && it->second.maxH == rec.maxH &&
            it->second.tooLarge == rec.tooLarge)
            return;
        it->second = rec;
    } else {
        if (cfg_->learnedLimits.size() >= Config::kMaxLearnedLimits) return;
        cfg_->learnedLimits.emplace(mw.limitKey, rec);
    }
    limitsDirty_ = true;
    AWA_LOG(L"remember %s min %dx%d max %dx%d%s", mw.limitKey.c_str(),
            rec.minW, rec.minH, rec.maxW, rec.maxH, rec.tooLarge ? L" (nofit)" : L"");
}

// ================================================================ blocked windows
void WindowManager::PruneBlocked() {
    for (auto it = blocked_.begin(); it != blocked_.end(); ) {
        if (IsWindow(*it) && IsWindowVisible(*it)) ++it;
        else                                       it = blocked_.erase(it);
    }
}

// ================================================================ crowding
void WindowManager::ParkAsFloating(HWND h, const Rect& area) {
    ManagedWindow* mw = Find(h);
    if (!mw) return;

    // Give it the size it actually wants, centred, and then never touch it
    // again: from here on it behaves like any other floating window, which is
    // to say the user owns its position.
    Rect r = VisibleRect(h);
    r.w = (std::min)(area.w, (std::max)(r.w, mw->limits.minW));
    r.h = (std::min)(area.h, (std::max)(r.h, mw->limits.minH));
    r.x = area.x + (area.w - r.w) / 2;
    r.y = area.y + (area.h - r.h) / 2;

    PlaceWindow(h, r, nullptr);
    mw->savedRect = r;
}

// Takes out of `order` any window whose own minimum cannot be satisfied
// alongside everything else here. The alternative - handing it a slot it will
// refuse - is what makes a large application overhang and cover its
// neighbours, which is the visible symptom users report.
//
// Windows are considered smallest-minimum first, so the ones that fit
// comfortably keep their tiles and the one oversized application is the one
// that gets left out, rather than whichever happened to be added last.
std::vector<HWND> WindowManager::DropWindowsThatCannotFit(const Rect& area,
                                                          std::vector<HWND>* order,
                                                          const ConsMap& cons) {
    std::vector<HWND> dropped;
    if (!order || order->size() < 2) return dropped;   // one window gets the board

    struct Item { HWND h; long long need; size_t seq; };
    std::vector<Item> items;
    items.reserve(order->size());
    for (size_t i = 0; i < order->size(); ++i) {
        HWND h = (*order)[i];
        long long need = 0;
        auto it = cons.find(h);
        if (it != cons.end())
            need = (long long)(std::max)(1, it->second.minW) *
                   (long long)(std::max)(1, it->second.minH);
        items.push_back({ h, need, i });
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.need != b.need) return a.need < b.need;
        return a.seq < b.seq;
    });

    // Area is an approximation of what a two-dimensional packing can hold, but
    // it is a conservative one and it is the only measure that does not depend
    // on which layout happens to be selected.
    const long long capacity = (long long)area.w * (long long)area.h;
    long long used = 0;
    std::vector<Item> kept;

    for (const Item& it : items) {
        if (it.need > 0 && !kept.empty() && used + it.need > capacity) {
            dropped.push_back(it.h);
            continue;
        }
        used += it.need;
        kept.push_back(it);
    }
    if (dropped.empty()) return dropped;

    // Put the survivors back in the order the workspace listed them, so the
    // layout itself is unchanged by having gone through here.
    std::sort(kept.begin(), kept.end(),
              [](const Item& a, const Item& b) { return a.seq < b.seq; });
    order->clear();
    for (const Item& it : kept) order->push_back(it.h);
    return dropped;
}

// ================================================================ tiling
LayoutParams WindowManager::ParamsFor(const Monitor& mon, const Workspace& ws) const {
    LayoutParams p;
    p.kind        = ws.layout;

    // Reserved margins keep tiles clear of bars Windows does not report in the
    // work area (a custom bar on any edge, for instance).
    p.work        = mon.info.work;
    p.work.x     += cfg_->marginLeft;
    p.work.y     += cfg_->marginTop;
    p.work.w     -= cfg_->marginLeft + cfg_->marginRight;
    p.work.h     -= cfg_->marginTop + cfg_->marginBottom;
    if (p.work.w < 100 || p.work.h < 100) p.work = mon.info.work;   // ignore silly values

    p.gapInner    = gapsEnabled_ ? cfg_->gapInner : 0;
    p.gapOuter    = gapsEnabled_ ? cfg_->gapOuter : 0;
    // Filled in by RetileMonitor, which is the only place that knows how many
    // windows are actually taking part.
    p.smartGaps   = cfg_->smartGaps;
    p.masterRatio = ws.masterRatio;
    p.masterCount = ws.masterCount;
    return p;
}

void WindowManager::RequestRetile() {
    if (gameMode_ || !msgWnd_) return;

    // Something outside the manager happened, so the verification pass gets a
    // fresh budget: the cap on it is there to stop it looping on its own, not
    // to stop it responding to the desktop.
    verifyChain_ = 0;
    verifyPass_  = false;

    const ULONGLONG now = GetTickCount64();

    // The debounce exists to coalesce a burst - an application opening four
    // windows at once, a workspace's worth of windows being un-hidden. It is
    // not there to slow down the ordinary case, which is one event on a
    // desktop that has been still for seconds, and paying 35 ms for it is the
    // single largest delay between pressing a key and seeing the layout move.
    //
    // So a request that arrives out of the quiet is posted rather than timed:
    // it runs on the very next trip through the message loop, once this event
    // has been dealt with. Posting rather than calling matters - RetileNow
    // ends in EndDeferWindowPos, which pumps messages while it talks to other
    // processes, and running that inside a win-event callback is how
    // reentrancy bugs are made.
    if (!retilePending_ && now - lastRetileAt_ >= kRetileQuietMs) {
        retilePending_ = true;
        retileDueAt_   = now + kRetileMaxDelayMs;
        // The timer is armed as well, as the backstop for the one case the
        // post cannot cover: a message queue busy enough that the post is
        // still sitting in it when the next burst arrives.
        SetTimer(msgWnd_, TIMER_RETILE, kRetileMaxDelayMs, nullptr);
        PostMessageW(msgWnd_, WM_AWA_RETILE, 0, 0);
        return;
    }

    if (!retilePending_) {
        retilePending_ = true;
        retileDueAt_   = now + kRetileMaxDelayMs;
    }

    // SetTimer restarts a timer that is already running, so debouncing by
    // calling it again on every event can postpone the retile for as long as
    // the events keep coming. The deadline set by the first request is the
    // ceiling; later requests may only bring the moment forward.
    UINT delay = kRetileDebounceMs;
    if (now + kRetileDebounceMs >= retileDueAt_)
        delay = (retileDueAt_ > now) ? (UINT)(retileDueAt_ - now) : 1;
    SetTimer(msgWnd_, TIMER_RETILE, delay, nullptr);
}

void WindowManager::RetileNow() {
    // A pass holds Monitor* and Workspace& across calls that pump, so nothing
    // that could rebuild those containers may run inside one. Window events and
    // display changes queue themselves for the moment this unwinds.
    Busy guard(this);

    if (msgWnd_) KillTimer(msgWnd_, TIMER_RETILE);
    retilePending_ = false;
    lastRetileAt_  = GetTickCount64();

    // Anything that is not the verification loop calling itself back means the
    // desktop has moved on, so the budget starts again. RequestRetile does the
    // same for the paths that go through it; this covers the ones that call
    // RetileNow outright - a layout change, a workspace switch, a display
    // arriving.
    const bool fromVerify = verifyPass_;
    verifyPass_ = false;
    if (!fromVerify) verifyChain_ = 0;

    if (!tilingEnabled_) return;

    // Cheap, rate-limited, and the last chance to notice a game that went
    // fullscreen without ever changing the foreground window (alt+enter).
    UpdateGameMode();
    if (gameMode_) return;

    PruneBlocked();

    // Everything we learned from the last pass, before planning this one.
    const bool learned = LearnFromLastPass();

    AnimBegin();
    // Only drop the record once it has been judged. LearnFromLastPass declines
    // to judge while an animation is in flight, and keeps the entries for the
    // pass that follows it.
    if (attempts_.empty() || learned || !cfg_->animations) attempts_.clear();
    for (int i = 0; i < (int)monitors_.size(); ++i) RetileMonitor(i);
    AnimCommit();   // also lands anything the new plan left in flight

    // A window that turned out to have a minimum size, a maximum size, or no
    // interest in being moved at all changes what the layout should be. Run
    // exactly one more pass with that knowledge - never a third, or two windows
    // with incompatible limits would ping-pong forever.
    if (learned && !relayoutPending_) {
        relayoutPending_ = true;
        AnimBegin();
        attempts_.clear();
        for (int i = 0; i < (int)monitors_.size(); ++i) RetileMonitor(i);
        AnimCommit();
    }
    relayoutPending_ = false;

    // Something was asked to move. Come back and look at what it did with the
    // space - a window only reveals its limits by ignoring us, and nothing
    // else is going to happen on an idle desktop to prompt the check.
    //
    // Attempts are only recorded for windows actually being moved, so on an
    // ordinary board this stops rescheduling itself after a pass or two. The
    // chain cap is for the boards that are not ordinary: a window that never
    // holds still, or two limits that contradict each other, would otherwise
    // keep an attempt on the books forever and have this timer re-arm itself
    // three times a second for the life of the process - which is a permanent
    // background cost on a desktop nobody is touching.
    if (!attempts_.empty() && msgWnd_) {
        if (++verifyChain_ <= kMaxVerifyChain) {
            verifyPass_ = true;
            SetTimer(msgWnd_, TIMER_RETILE, 320, nullptr);
        } else {
            AWA_LOG(L"verify: %d passes without settling, leaving %d window(s) alone",
                    verifyChain_ - 1, (int)attempts_.size());
            attempts_.clear();
            verifyChain_ = 0;
        }
    }
}

// Windows lies by omission: SetWindowPos on a window we are not allowed to
// touch returns success, and a window enforcing its own minimum simply keeps
// the size it wanted. Neither is reported, so the only way to know is to look
// at where the window actually ended up. (This is the same rule the rest of the
// project follows: verify by observing, not by assuming the call worked.)
bool WindowManager::LearnFromLastPass() {
    if (attempts_.empty()) return false;

    // Mid-animation a window is somewhere between its old rect and its target,
    // and reading that would teach us a minimum and a maximum that are both
    // nonsense. A retile fires on every window event and the animation runs for
    // ~140 ms, so this happens constantly. Wait for the windows to land; the
    // attempts stay on the books until they can be judged fairly.
    if (animActive_ && WaitForSingleObject(animActive_, 0) == WAIT_OBJECT_0)
        return false;

    const int kSlack = 4;            // rounding and per-monitor DPI dust
    const int kGiveUpAfter = 2;      // consecutive refusals before we stop trying
    const int kUsesEnough  = 55;     // percent of its slot a tiled window must use
    bool learned = false;
    int  newlyImmovable = 0;
    std::unordered_map<HWND, Attempt> unsettled;

    for (const auto& e : attempts_) {
        ManagedWindow* mw = Find(e.first);
        if (!mw || !IsWindow(e.first)) continue;
        if (mw->hidden || mw->minimized || mw->floating || mw->fullscreen) continue;
        // A window that has been taken out of the tiling is not obeying or
        // disobeying anything: whatever it does with its size from here is the
        // user's business, and reading it would teach us a limit it does not
        // have.
        if (mw->tooLarge || mw->crowdedOut) continue;

        const Rect now = VisibleRect(e.first);

        // Has it stopped moving? A window still opening - three fresh Explorer
        // windows was the case that showed this - is nowhere near its final
        // size, and reading it then taught us a maximum width Explorer does not
        // have. Wait until two consecutive looks agree before believing
        // anything, and keep the attempt on the books until then.
        const bool settled = mw->haveSeen && mw->lastSeen == now;
        mw->lastSeen = now;
        mw->haveSeen = true;
        if (!settled) {
            // Keep it on the books - but not indefinitely. A window that is
            // never the same size twice running (a video player, an
            // application still streaming its content in) would otherwise hold
            // this entry open forever, and with it the verification timer.
            Attempt again = e.second;
            if (++again.looks < kSettleLooks) unsettled[e.first] = again;
            continue;
        }

        const Rect target = e.second.target;
        const Rect before = e.second.before;

        const bool movedAtAll = (now != before);
        const bool wanted     = (target != before);

        // Nothing happened. Two very different causes look identical from here,
        // so tell them apart by what we already know about the window: if a
        // limit we have already learned explains why it stayed put, this is a
        // window that will not use the space, not one we are forbidden to
        // touch. Reporting the wrong one sends whoever reads the log looking
        // for a permissions problem that does not exist.
        if (wanted && !movedAtAll) {
            const bool explained =
                (mw->limits.maxW < target.w) || (mw->limits.maxH < target.h) ||
                (mw->limits.minW > target.w) || (mw->limits.minH > target.h);

            if (!explained) {
                if (++mw->refusals >= kGiveUpAfter && !mw->immovable) {
                    mw->immovable = true;
                    learned = true;
                    ++newlyImmovable;
                    AWA_LOG(L"window %p will not accept any placement; leaving it alone "
                            L"(usually a higher integrity level)", (void*)e.first);
                }
                continue;
            }
            // Fall through: the size checks below record what it did accept.
        }
        if (movedAtAll) mw->refusals = 0;

        // It moved, but not to the size we asked for. That is the window's own
        // minimum or maximum talking, and it is the number we want.
        if (now.w > target.w + kSlack && now.w > mw->limits.minW) {
            mw->limits.minW = now.w;
            if (mw->limits.maxW < mw->limits.minW) mw->limits.maxW = kNoLimit;
            learned = true;
            if (cfg_ && cfg_->debug)
                LogLine(L"learn %p minW=%d (asked %d, got %d)",
                        (void*)e.first, mw->limits.minW, target.w, now.w);
        } else if (now.w < target.w - kSlack && now.w < mw->limits.maxW) {
            mw->limits.maxW = now.w;
            if (mw->limits.minW > mw->limits.maxW) mw->limits.minW = 0;
            learned = true;
            if (cfg_ && cfg_->debug)
                LogLine(L"learn %p maxW=%d (asked %d, got %d)",
                        (void*)e.first, mw->limits.maxW, target.w, now.w);
        }

        if (now.h > target.h + kSlack && now.h > mw->limits.minH) {
            mw->limits.minH = now.h;
            if (mw->limits.maxH < mw->limits.minH) mw->limits.maxH = kNoLimit;
            learned = true;
            if (cfg_ && cfg_->debug)
                LogLine(L"learn %p minH=%d (asked %d, got %d)",
                        (void*)e.first, mw->limits.minH, target.h, now.h);
        } else if (now.h < target.h - kSlack && now.h < mw->limits.maxH) {
            mw->limits.maxH = now.h;
            if (mw->limits.minH > mw->limits.maxH) mw->limits.minH = 0;
            learned = true;
            if (cfg_ && cfg_->debug)
                LogLine(L"learn %p maxH=%d (asked %d, got %d)",
                        (void*)e.first, mw->limits.maxH, target.h, now.h);
        }

        // Whatever it accepted, did it use the slot? A window that keeps taking
        // a corner of what it is given is leaving the rest as bare desktop, and
        // no amount of moving splits fixes a limit on the axis this window's
        // split does not run along. Stop tiling it and let it float.
        const long long slot = (long long)target.w * target.h;
        const long long used = (long long)now.w * now.h;
        if (slot > 0 && used * 100 < slot * kUsesEnough) {
            if (++mw->wastes >= kGiveUpAfter && !mw->tooSmall) {
                mw->tooSmall = true;
                learned = true;
                AWA_LOG(L"window %p uses %lld%% of its tile (%dx%d of %dx%d); "
                        L"floating it instead",
                        (void*)e.first, used * 100 / slot,
                        now.w, now.h, target.w, target.h);
            }
        } else {
            mw->wastes = 0;
        }

        // Whatever this window just taught us, keep it for next time. Cheap
        // when nothing changed: one map lookup and a comparison, and the
        // config is only marked dirty if the numbers are actually new.
        RememberLimits(*mw);
    }
    attempts_ = std::move(unsettled);

    // Say it once, and only when we can actually offer the fix.
    if (newlyImmovable > 0 && !immovableAnnounced_ && !SelfIsElevated()) {
        immovableAnnounced_ = true;
        AppTrayBalloon(L"ProWindows",
                       L"Some windows cannot be arranged because they run as "
                       L"administrator. Restart ProWindows as administrator from "
                       L"the tray menu to include them.");
    }
    return learned;
}

ConsMap WindowManager::ConstraintsFor(const std::vector<HWND>& order) {
    ConsMap cons;
    cons.reserve(order.size());
    for (HWND h : order) {
        ManagedWindow* mw = Find(h);
        if (!mw) continue;

        // Ask the window once. It is a cross-process SendMessage, so it is done
        // on first sight and then corrected by observation rather than repeated
        // on every pass.
        if (!mw->limitsAsked) {
            mw->limitsAsked = true;
            const SizeLimits declared = QuerySizeLimits(h);
            // That was a SendMessageTimeout, which pumps this thread's sent
            // messages while it waits. The window can be gone by the time it
            // returns, and `mw` with it - so look it up again rather than
            // writing through a pointer into a node that may have been freed.
            mw = Find(h);
            if (!mw) continue;
            mw->limits.minW = (std::max)(mw->limits.minW, declared.minW);
            mw->limits.minH = (std::max)(mw->limits.minH, declared.minH);
            mw->limits.maxW = (std::min)(mw->limits.maxW, declared.maxW);
            mw->limits.maxH = (std::min)(mw->limits.maxH, declared.maxH);
            if (mw->limits.maxW < mw->limits.minW) mw->limits.maxW = kNoLimit;
            if (mw->limits.maxH < mw->limits.minH) mw->limits.maxH = kNoLimit;
        }
        if (mw->limits.constrained()) cons.emplace(h, mw->limits);
    }
    return cons;
}

void WindowManager::RetileMonitor(int monitorIndex) {
    Monitor* mon = (monitorIndex >= 0 && monitorIndex < (int)monitors_.size())
                   ? &monitors_[monitorIndex] : nullptr;
    if (!mon || mon->workspaces.empty()) return;
    Workspace& ws = mon->workspaces[mon->active];

    // Drop windows that died or stopped qualifying since the last pass.
    std::vector<HWND> dead;
    for (HWND h : ws.tiled) {
        ManagedWindow* mw = Find(h);
        if (!mw) { dead.push_back(h); continue; }
        if (!IsWindow(h)) { dead.push_back(h); continue; }
        // "Is it still on screen" is not a question that can be asked of a
        // minimised window: minimising is itself a hide, and some applications
        // (Store apps especially) come back cloaked as well. Judging one by
        // those two would drop it from the workspace it is parked on and make
        // it a stranger again the moment it was restored. A minimised window
        // that has genuinely gone is caught by IsWindow above, and by
        // EVENT_OBJECT_DESTROY well before this.
        if (mw->minimized) continue;
        if (!mw->hidden && !IsWindowVisible(h)) { dead.push_back(h); continue; }
        if (!mw->hidden && IsCloaked(h)) { dead.push_back(h); continue; }
    }
    for (HWND h : dead) RemoveWindow(h);
    dead.clear();                       // or every tiled window is removed twice
    for (HWND h : ws.floats) if (!IsWindow(h)) dead.push_back(h);
    for (HWND h : dead) RemoveWindow(h);

    const LayoutParams params = ParamsFor(*mon, ws);

    // Only visible, non-minimised, non-fullscreen windows take part in tiling.
    std::vector<HWND> order;
    HWND fullscreen = nullptr;
    for (HWND h : ws.tiled) {
        ManagedWindow* mw = Find(h);
        if (!mw || mw->hidden || mw->minimized || mw->scratch) continue;
        if (mw->fullscreen) { fullscreen = h; continue; }
        // A window we have proved we cannot move, or that will not use the
        // space it is given, must not be given a tile: reserving one is
        // exactly what leaves a rectangle of empty desktop behind.
        if (mw->immovable || mw->tooSmall) continue;

        if (mw->tooLarge) {
            // "Will not fit" was decided against a particular screen. Plug in
            // a larger monitor, drop the scaling, or move the window to a
            // different display and it may fit perfectly well - and the flag
            // is remembered between runs, so without this re-check a window
            // could stay floating forever because of a monitor that is no
            // longer attached.
            if (mw->limits.minW > params.work.w || mw->limits.minH > params.work.h)
                continue;
            mw->tooLarge = false;
            RememberLimits(*mw);
            AWA_LOG(L"window %p fits again (%dx%d needed, %dx%d available)",
                    (void*)h, mw->limits.minW, mw->limits.minH,
                    params.work.w, params.work.h);
        }
        order.push_back(h);
    }

    const ConsMap cons = ConstraintsFor(order);

    // Windows to move out of the tiling and leave floating. Two reasons, and
    // they are worth keeping apart: one is permanent, one is only true of the
    // board as it stands right now.
    std::vector<HWND> park;

    // Permanent: the window's own minimum is larger than the whole usable area
    // of this monitor. No layout, no ratio and no amount of squeezing its
    // neighbours can produce a slot it will accept.
    for (size_t i = 0; i < order.size(); ) {
        ManagedWindow* mw = Find(order[i]);
        auto lim = cons.find(order[i]);
        const bool hopeless = mw && lim != cons.end() &&
                              (lim->second.minW > params.work.w ||
                               lim->second.minH > params.work.h);
        if (!hopeless) { ++i; continue; }

        mw->tooLarge = true;
        RememberLimits(*mw);
        AWA_LOG(L"window %p needs at least %dx%d, the screen offers %dx%d; "
                L"leaving it floating", (void*)order[i],
                lim->second.minW, lim->second.minH, params.work.w, params.work.h);
        park.push_back(order[i]);
        order.erase(order.begin() + (ptrdiff_t)i);
    }

    // Temporary: it would fit on its own, but not beside everything else that
    // is open. Recomputed every pass, so closing a neighbour brings it back.
    const std::vector<HWND> crowded =
        DropWindowsThatCannotFit(params.work, &order, cons);
    for (HWND h : crowded) {
        ManagedWindow* mw = Find(h);
        if (!mw) continue;
        const bool wasOut = mw->crowdedOut;
        mw->crowdedOut = true;
        if (!wasOut) park.push_back(h);      // position it once, then leave it
    }
    for (HWND h : order)
        if (ManagedWindow* mw = Find(h)) mw->crowdedOut = false;

    std::unordered_set<HWND> alive(order.begin(), order.end());
    ws.tree.Prune(alive);
    for (HWND h : order) if (!ws.tree.Contains(h)) ws.tree.Insert(h, nullptr, false);

    std::vector<std::pair<HWND, Rect>> plan;
    ComputeLayout(params, order, ws.tree, &plan, &cons);

    for (HWND h : park) ParkAsFloating(h, params.work);

    if (cfg_->debug) {
        LogLine(L"tree %s", ws.tree.Describe().c_str());
        for (const auto& e : plan) {
            auto it = cons.find(e.first);
            if (it == cons.end())
                LogLine(L"plan %p -> %d,%d %dx%d", (void*)e.first,
                        e.second.x, e.second.y, e.second.w, e.second.h);
            else
                LogLine(L"plan %p -> %d,%d %dx%d  [min %dx%d max %dx%d]", (void*)e.first,
                        e.second.x, e.second.y, e.second.w, e.second.h,
                        it->second.minW, it->second.minH,
                        it->second.maxW >= kNoLimit ? -1 : it->second.maxW,
                        it->second.maxH >= kNoLimit ? -1 : it->second.maxH);
        }
    }
    ApplyPlacements(plan);

    if (fullscreen) {
        PlaceWindow(fullscreen, mon->info.full, nullptr);
        BringWindowToTop(fullscreen);
    } else if (ws.layout == LayoutKind::Monocle && focused_ && Find(focused_)) {
        ManagedWindow* mw = Find(focused_);
        if (mw && mw->monitor == monitorIndex && !mw->floating) BringWindowToTop(focused_);
    }
}

void WindowManager::ApplyPlacements(const std::vector<std::pair<HWND, Rect>>& plan) {
    if (plan.empty()) return;

    // Every window in the plan is measured exactly once, here, and the three
    // things that follow all read that measurement instead of asking again.
    // Each of them used to make its own DWM round trip - the attempt record,
    // the animation's "from", and the animation's frame padding - which on a
    // board of ten windows was forty cross-process calls per window event to
    // answer one question ten times. See MAP.md invariant 47.
    struct Placement { HWND hwnd; Rect to; WindowGeom geom; };
    std::vector<Placement> live;
    live.reserve(plan.size());
    for (const auto& e : plan) {
        const WindowGeom geom = MeasureWindow(e.first);
        if (!geom.ok) continue;                      // the window has gone
        live.push_back({ e.first, e.second, geom });
    }
    if (live.empty()) return;

    // Remember what we asked for, and where the window was when we asked, so
    // the next pass can tell "it refused" from "the user moved it". An entry
    // already on the books is one we have not been able to judge yet, so its
    // original "before" is kept - overwriting it with a rect we ourselves just
    // moved would make every window look like it had accepted the placement.
    //
    // A window already sitting on its target is not an attempt. Recording those
    // too would mean there was always something left to verify, and the
    // verification pass below would never stop rescheduling itself.
    for (const auto& p : live) {
        auto it = attempts_.find(p.hwnd);
        if (it != attempts_.end())      { it->second.target = p.to; continue; }
        if (p.geom.visible == p.to)     continue;
        attempts_[p.hwnd] = Attempt{ p.geom.visible, p.to };
    }

    if (cfg_->animations) {
        // Append, never clear: one retile pass covers every monitor, and all of
        // them have to animate together.
        for (const auto& p : live) {
            if (p.geom.visible == p.to) continue;

            AnimEntry entry;
            entry.hwnd = p.hwnd;
            entry.from = p.geom.visible;
            entry.to   = p.to;
            entry.pad  = p.geom.pad;

            // Un-maximise once, up front, rather than on every frame - and
            // re-measure, because restoring moves the window and the rect
            // taken a moment ago is now the maximised one.
            if (IsMaximizedWnd(p.hwnd)) {
                WINDOWPLACEMENT wp{ sizeof(WINDOWPLACEMENT) };
                if (GetWindowPlacement(p.hwnd, &wp)) {
                    wp.showCmd = SW_RESTORE;
                    SetWindowPlacement(p.hwnd, &wp);
                }
                const WindowGeom after = MeasureWindow(p.hwnd);
                if (!after.ok) continue;
                entry.from = after.visible;
                entry.pad  = after.pad;
            }

            if (entry.from != entry.to) anim_.push_back(entry);
        }
        return;
    }

    HDWP dwp = BeginDeferWindowPos((int)live.size());
    for (const auto& p : live) {
        if (p.geom.visible == p.to) continue;             // already in place
        // PlaceWindow would measure the frame again; it has just been measured.
        if (IsMaximizedWnd(p.hwnd)) {
            WINDOWPLACEMENT wp{ sizeof(WINDOWPLACEMENT) };
            if (GetWindowPlacement(p.hwnd, &wp)) {
                wp.showCmd = SW_RESTORE;
                SetWindowPlacement(p.hwnd, &wp);
            }
            PlaceWindow(p.hwnd, p.to, dwp ? &dwp : nullptr);
            continue;
        }
        PlaceWindowFast(p.hwnd, p.to, p.geom.pad, dwp ? &dwp : nullptr);
    }
    if (dwp) EndDeferWindowPos(dwp);
}

void WindowManager::AnimBegin() {
    // Anything that has already been moved at least once is in flight, part
    // way between where it was and where it was going. The new plan will pick
    // most of them up again - but not one this pass decides to park, float or
    // crowd out, and that one would simply be abandoned wherever the easing
    // curve had left it. Hand them to AnimCommit, which lands the ones the new
    // plan does not claim.
    //
    // Accumulated rather than replaced, because RetileNow runs a second pass
    // when it has learned something and that pass must not lose the first
    // pass's carry.
    for (const auto& a : anim_) {
        if (!a.sent) continue;                 // never moved; nothing to land
        auto it = std::find_if(animCarry_.begin(), animCarry_.end(),
                               [&](const AnimEntry& c) { return c.hwnd == a.hwnd; });
        if (it != animCarry_.end()) *it = a;   // newest target wins
        else                        animCarry_.push_back(a);
    }
    anim_.clear();
}

DWORD WINAPI WindowManager::AnimThread(LPVOID self) {
    static_cast<WindowManager*>(self)->AnimTickLoop();
    return 0;
}

// Nothing is computed here: the thread only says "a frame is due". All the
// window moving stays on the UI thread, where it has to be.
void WindowManager::AnimTickLoop() {
    // 4 ms is comfortably inside a 180 Hz frame; the UI thread coalesces
    // anything it cannot keep up with.
    HANDLE tick = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
    if (!tick) tick = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (!tick) return;

    HANDLE idle[2]   = { animQuit_, animActive_ };
    HANDLE ticking[2] = { animQuit_, tick };

    for (;;) {
        // Parked: costs nothing at all until an animation actually starts.
        if (WaitForMultipleObjects(2, idle, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
            break;                                   // quit, or the wait failed

        LARGE_INTEGER due;
        due.QuadPart = -40000;                       // 4 ms, in 100 ns units
        SetWaitableTimer(tick, &due, 4, nullptr, nullptr, FALSE);

        for (;;) {
            const DWORD hit = WaitForMultipleObjects(2, ticking, FALSE, INFINITE);
            if (hit != WAIT_OBJECT_0 + 1) { CancelWaitableTimer(tick); goto done; }
            // The animation ended while we were waiting; go back to parking.
            if (WaitForSingleObject(animActive_, 0) != WAIT_OBJECT_0) break;
            // Only ever one frame in flight: if the UI thread has not caught
            // up, posting again would just build a backlog to drain later.
            if (InterlockedCompareExchange(&animPending_, 1, 0) == 0)
                PostMessageW(msgWnd_, WM_AWA_ANIMTICK, 0, 0);
        }
        CancelWaitableTimer(tick);
    }

done:
    CloseHandle(tick);
}

void WindowManager::AnimCommit() {
    // Land anything that was in flight and is not in the new plan, so no
    // window is ever left stranded at an interpolated rect.
    if (!animCarry_.empty()) {
        HDWP dwp = BeginDeferWindowPos((int)animCarry_.size());
        for (const auto& c : animCarry_) {
            if (!IsWindow(c.hwnd)) continue;
            bool stillMoving = false;
            for (const auto& a : anim_)
                if (a.hwnd == c.hwnd) { stillMoving = true; break; }
            if (stillMoving) continue;
            PlaceWindowFast(c.hwnd, c.to, c.pad, dwp ? &dwp : nullptr);
        }
        if (dwp) EndDeferWindowPos(dwp);
        animCarry_.clear();
    }

    if (anim_.empty()) { AnimStop(); return; }

    QueryPerformanceFrequency(&animFreq_);
    QueryPerformanceCounter(&animStart_);
    animLastFrame_.QuadPart = 0;     // gaps are measured within one animation

    // 1 ms scheduling resolution, released the moment the animation ends so
    // idle power is untouched.
    if (!animTimerRaised_) {
        timeBeginPeriod(1);
        animTimerRaised_ = true;
    }

    if (!animThread_) {
        // Created once, and only if they are not already there. If CreateThread
        // below fails - and then keeps failing, because whatever stopped it is
        // still true - this runs again on every retile, and overwriting the
        // handles each time leaked two kernel objects per window event.
        if (!animQuit_)   animQuit_   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!animActive_) animActive_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (animQuit_ && animActive_)
            animThread_ = CreateThread(nullptr, 0, AnimThread, this, 0, nullptr);
        // Without the thread the animation still runs, just at the old cadence.
        if (!animThread_) SetTimer(msgWnd_, TIMER_ANIM, 8, nullptr);
    }
    // Wakes the parked ticker. The thread itself lives until Shutdown.
    if (animActive_) SetEvent(animActive_);
}

// Ends the current animation. The ticker thread parks rather than exiting - see
// AnimShutdown for the one place it is actually torn down.
void WindowManager::AnimStop() {
    if (cfg_ && cfg_->debug && animFrames_ > 0) {
        LogLine(L"anim: %d frames, worst gap %.1f ms", animFrames_, animWorstGap_);
    }
    animFrames_   = 0;
    animWorstGap_ = 0.0;
    animLastFrame_.QuadPart = 0;

    // An animation cut short - by game mode, or by shutdown - must still leave
    // its windows somewhere deliberate rather than mid-glide.
    for (const auto& a : anim_) {
        if (!a.sent || a.last == a.to || !IsWindow(a.hwnd)) continue;
        PlaceWindowFast(a.hwnd, a.to, a.pad, nullptr);
    }
    anim_.clear();
    animCarry_.clear();
    if (msgWnd_) KillTimer(msgWnd_, TIMER_ANIM);
    if (animActive_) ResetEvent(animActive_);
    InterlockedExchange(&animPending_, 0);

    if (animTimerRaised_) {
        timeEndPeriod(1);
        animTimerRaised_ = false;
    }
}

void WindowManager::AnimShutdown() {
    AnimStop();

    // Whether the ticker actually stopped decides what may be closed. It parks
    // in WaitForMultipleObjects on animQuit_ and animActive_, so closing those
    // while it is still in there is a wait on freed handles - and handle values
    // are reused, so the thread can end up waiting on somebody else's object.
    // A thread that will not stop is abandoned with its handles instead, the
    // same way the thermal probe and the search walk already are.
    bool stopped = true;
    if (animThread_) {
        SetEvent(animQuit_);
        stopped = (WaitForSingleObject(animThread_, 2000) == WAIT_OBJECT_0);
        if (stopped) CloseHandle(animThread_);
        else AWA_LOG(L"animation ticker did not stop in time; abandoning it");
        animThread_ = nullptr;
    }
    if (stopped) {
        if (animQuit_)   CloseHandle(animQuit_);
        if (animActive_) CloseHandle(animActive_);
    }
    // Cleared either way, so nothing here touches a handle it no longer owns.
    animQuit_   = nullptr;
    animActive_ = nullptr;
}

void WindowManager::AnimTickHandled() {
    InterlockedExchange(&animPending_, 0);
}

void WindowManager::AnimStep() {
    Busy guard(this);
    if (anim_.empty()) { AnimStop(); return; }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // Frame cadence, so "it feels janky" can be measured rather than guessed
    // at. The worst gap is remembered and reported once when the animation
    // ends: logging every frame meant an open, write and close of the log file
    // 250 times a second, which on a mechanical disk measured the disk rather
    // than the animation.
    if (cfg_->debug && animFreq_.QuadPart) {
        if (animLastFrame_.QuadPart) {
            const double gap = (double)(now.QuadPart - animLastFrame_.QuadPart) *
                               1000.0 / (double)animFreq_.QuadPart;
            if (gap > animWorstGap_) animWorstGap_ = gap;
            ++animFrames_;
        }
        animLastFrame_ = now;
    }
    const double ms = animFreq_.QuadPart
        ? (double)(now.QuadPart - animStart_.QuadPart) * 1000.0 / (double)animFreq_.QuadPart
        : (double)cfg_->animationMs;

    float t = (float)(ms / (double)(std::max)(1, cfg_->animationMs));
    if (t >= 1.0f) t = 1.0f;

    // Ease-out cubic: quick off the mark, settling gently into place.
    const float inv = 1.0f - t;
    const float e = 1.0f - inv * inv * inv;

    {
        HDWP dwp = BeginDeferWindowPos((int)anim_.size());
        for (auto& a : anim_) {
            Rect r;
            if (t >= 1.0f) {
                r = a.to;                       // land exactly on target
            } else {
                r.x = a.from.x + (int)((a.to.x - a.from.x) * e + 0.5f);
                r.y = a.from.y + (int)((a.to.y - a.from.y) * e + 0.5f);
                r.w = a.from.w + (int)((a.to.w - a.from.w) * e + 0.5f);
                r.h = a.from.h + (int)((a.to.h - a.from.h) * e + 0.5f);
            }
            // Towards the end of an ease-out the steps fall below a pixel.
            // Asking an app to move to where it already is costs a full
            // repaint in that app and buys nothing.
            if (a.sent && r == a.last) continue;
            a.last = r;
            a.sent = true;
            PlaceWindowFast(a.hwnd, r, a.pad, dwp ? &dwp : nullptr);
        }
        if (dwp) EndDeferWindowPos(dwp);
    }

    if (t >= 1.0f) AnimStop();
}

void WindowManager::UpdateBorders() {
    if (!cfg_->accentBorder || gameMode_) return;
    if (lastBordered_ && lastBordered_ != focused_ && IsWindow(lastBordered_))
        SetBorderColor(lastBordered_, cfg_->inactiveColor, true);
    if (focused_ && IsWindow(focused_))
        SetBorderColor(focused_, cfg_->activeColor, true);
    lastBordered_ = focused_;
}

void WindowManager::RestoreAllWindows() {
    Busy guard(this);
    // The un-hiding is collected first and done afterwards: ShowWindow pumps,
    // and a window event delivered inside it erases from `managed_` under this
    // loop's iterator.
    std::vector<HWND> show;
    show.reserve(managed_.size());
    for (auto& kv : managed_) {
        ManagedWindow& mw = kv.second;
        if (mw.hidden) {
            mw.hidden = false;
            UntrackHidden(mw.hwnd);
            show.push_back(mw.hwnd);
        }
        mw.fullscreen = false;
    }
    for (HWND h : show) ShowWindow(h, SW_SHOWNA);
}

// ================================================================ events
// There is deliberately no "ignore our own changes" flag here any more.
//
// The hook is installed WINEVENT_OUTOFCONTEXT, which means the callback is
// delivered asynchronously - typically long after the SetWindowPos or
// ShowWindow that caused it has returned. A scope guard around the call
// therefore cannot possibly still be in scope when the echo arrives, so the
// flag never suppressed anything it was aimed at. What it did do was suppress
// genuine events: EndDeferWindowPos pumps messages while it talks to other
// processes, and it is called on every animation frame, so any window that
// opened during an animation had its EVENT_OBJECT_SHOW dropped on the floor -
// which is one of the ways a window ended up untiled until it was touched.
//
// The real protection is per-window and lives in the cases below: SetHidden
// sets ManagedWindow::hidden BEFORE calling ShowWindow, so the echo of our own
// hide is recognised by that flag, and a show for a window we already manage is
// a no-op whichever way it was caused. Placement calls raise no subscribed
// event at all - EVENT_OBJECT_LOCATIONCHANGE is not hooked, and
// MOVESIZESTART/END come only from a user dragging.
// The public entry point. Everything a window event does - adopting a window,
// dropping one, moving it between workspaces - mutates the very containers a
// pass in flight is holding references into, so while one is running the event
// is written down instead and replayed the moment the pass unwinds. See the
// reentrancy note in wm.h.
void WindowManager::OnWinEvent(DWORD event, HWND hwnd) {
    if (!hwnd || shutdown_) return;

    if (busy_) {
        if (deferred_.size() < kMaxDeferred) {
            deferred_.push_back(DeferredEvent{ event, hwnd });
        } else {
            // Nothing that is dropped here is lost for good: a window that has
            // gone is noticed by the dead sweep at the top of every pass, and a
            // window that has appeared is picked up by the next scan. Ask for
            // the pass rather than growing this queue without bound.
            RequestRetile();
        }
        return;
    }
    Busy guard(this);
    HandleWinEvent(event, hwnd);
}

// Replays everything that arrived while a pass was running. Called from the
// last Busy guard to unwind, which is the only moment it is safe.
void WindowManager::DrainDeferred() {
    if (busy_ || draining_) return;
    if (shutdown_) { deferred_.clear(); deferredDisplayChange_ = false; return; }
    if (deferred_.empty() && !deferredDisplayChange_) return;

    draining_ = true;
    // Replaying an event can queue another - a destroy leads to a retile which
    // notices a second window has gone. A handful of rounds settles every real
    // case; the cap is there so a pathological one cannot spin here.
    for (int round = 0; round < 8; ++round) {
        const bool display = deferredDisplayChange_;
        deferredDisplayChange_ = false;

        std::vector<DeferredEvent> batch;
        batch.swap(deferred_);
        if (!display && batch.empty()) break;

        if (display) { ReloadMonitors(); RetileNow(); }
        for (const DeferredEvent& e : batch) {
            Busy guard(this);
            HandleWinEvent(e.event, e.hwnd);
        }
    }

    // Still arriving after eight rounds. Whatever is left is dropped rather
    // than replayed for ever - but not in silence, and not without asking for
    // the one pass that re-reads the desktop from scratch and puts right
    // anything the dropped events would have said.
    if (!deferred_.empty() || deferredDisplayChange_) {
        AWA_LOG(L"deferred window events did not settle; dropping %d",
                (int)deferred_.size());
        deferred_.clear();
        deferredDisplayChange_ = false;
        RequestRetile();
    }
    draining_ = false;
}

void WindowManager::HandleWinEvent(DWORD event, HWND hwnd) {
    if (!hwnd) return;

    // A game entering or leaving fullscreen reaches us as a foreground change,
    // so that is where the check belongs; OnForeground does it and then bails
    // out on its own if a game has taken over.
    if (event == EVENT_SYSTEM_FOREGROUND) { OnForeground(hwnd); return; }

    // While a fullscreen application owns the screen, keep the books straight
    // but do nothing else: a destroyed window still has to leave the tables,
    // and every other event here would end in a cross-process call we have
    // just decided not to make.
    if (gameMode_) {
        if (event == EVENT_OBJECT_DESTROY) RemoveWindow(hwnd);
        return;
    }

    switch (event) {
        case EVENT_OBJECT_SHOW:
        case EVENT_OBJECT_UNCLOAKED: {
            // Already ours, whether it is on screen or hidden because its
            // workspace is not the active one. Either way there is nothing to
            // do: this is our own hide/show echoing back, or the application
            // showing a window we are already arranging.
            if (Find(hwnd)) return;
            if (AdoptWindow(hwnd)) RequestRetile();
            break;
        }

        // A window that has just been given a title. This is the moment the
        // most common reason for turning a window away stops applying, so it
        // is worth acting on immediately rather than waiting for the next
        // retry tick - but only for windows we are actually waiting on, which
        // keeps it to one hash lookup for the many title changes a desktop
        // produces every second.
        case EVENT_OBJECT_NAMECHANGE: {
            if (Find(hwnd)) return;
            if (pending_.find(hwnd) == pending_.end()) return;
            if (AdoptWindow(hwnd)) RequestRetile();
            break;
        }
        case EVENT_OBJECT_HIDE:
        case EVENT_OBJECT_CLOAKED:
        case EVENT_OBJECT_DESTROY: {
            ManagedWindow* mw = Find(hwnd);
            if (!mw) return;
            if (mw->hidden && event != EVENT_OBJECT_DESTROY) return;
            RemoveWindow(hwnd);
            RequestRetile();
            break;
        }
        case EVENT_SYSTEM_MINIMIZESTART: {
            ManagedWindow* mw = Find(hwnd);
            if (!mw || mw->minimized) return;
            mw->minimized = true;
            if (Workspace* ws = WorkspaceFor(*mw)) ws->tree.Remove(hwnd);
            RequestRetile();
            break;
        }
        case EVENT_SYSTEM_MINIMIZEEND: {
            ManagedWindow* mw = Find(hwnd);
            if (!mw) { if (AdoptWindow(hwnd)) RequestRetile(); return; }
            if (!mw->minimized) return;
            mw->minimized = false;
            if (!mw->floating) {
                if (Workspace* ws = WorkspaceFor(*mw)) {
                    // Belt and braces for a window that reached us by some
                    // path that did not list it - the tree alone is not enough,
                    // because the pass rebuilds the tree from this list.
                    if (std::find(ws->tiled.begin(), ws->tiled.end(), hwnd) ==
                        ws->tiled.end())
                        ws->tiled.push_back(hwnd);
                    ws->tree.Insert(hwnd, ws->lastFocused, false);
                }
            }
            RequestRetile();
            break;
        }
        case EVENT_SYSTEM_MOVESIZESTART:
            OnMoveSizeStart(hwnd);
            break;
        case EVENT_SYSTEM_MOVESIZEEND:
            OnMoveSizeEnd(hwnd);
            break;
        default:
            break;
    }
}

void WindowManager::OnForeground(HWND hwnd) {
    Busy guard(this);
    // The one place a game reliably announces itself. Forced rather than
    // rate-limited: a foreground change is exactly the moment the answer can
    // have changed, and there are few enough of them to afford it.
    UpdateGameMode(true);
    if (gameMode_) return;

    ManagedWindow* mw = Find(hwnd);
    if (!mw) {
        // This one really is the foreground window - that is what the event
        // says - so it may take focus outright.
        if (AddWindow(hwnd, true)) { RequestRetile(); mw = Find(hwnd); }
        if (!mw) return;
    }
    focused_ = hwnd;
    activeMonitor_ = mw->monitor;
    if (Workspace* ws = WorkspaceFor(*mw)) {
        ws->lastFocused = hwnd;
        if (ws->layout == LayoutKind::Monocle && !mw->floating) BringWindowToTop(hwnd);
    }
    UpdateBorders();
}

// ================================================================ drag and drop
namespace {

// Which side of `r` the point landed on.
//
// The rectangle is cut into four triangles meeting at its centre, and the
// pointer takes the one it is standing in. That is the whole rule, and the
// reason for it is that the obvious alternative - an "edge zone" a fixed
// fraction in from each side - leaves the middle of the window ambiguous and
// the corners arbitrary, which is what produced "sometimes it goes right and
// sometimes it goes down" from what felt like the same gesture.
//
// Distances are measured as a fraction of each half-dimension, so the split is
// along the rectangle's own diagonals: a wide tile gives left and right a
// generously wide catchment, a tall one gives top and bottom the same, and in
// both cases the pointer is always in the region nearest the edge it is
// nearest to. The tie goes to the horizontal, which is the axis people reach
// for and the one the complaint was about.
Dir DropSide(const Rect& r, POINT pt) {
    const double halfW = (std::max)(1, r.w) / 2.0;
    const double halfH = (std::max)(1, r.h) / 2.0;
    const double dx = (pt.x - (r.x + halfW)) / halfW;    // -1 .. 1
    const double dy = (pt.y - (r.y + halfH)) / halfH;

    if (std::abs(dx) >= std::abs(dy)) return dx >= 0 ? Dir::Right : Dir::Left;
    return dy >= 0 ? Dir::Down : Dir::Up;
}

// The half of `r` that a window dropped on `side` of it would occupy. Only
// used to draw the indicator, so it does not have to agree with the layout
// engine to the pixel - but being the same halves the layout will produce is
// what makes the preview worth showing at all.
Rect HalfTowards(const Rect& r, Dir side) {
    switch (side) {
        case Dir::Left:  return Rect(r.x, r.y, r.w / 2, r.h);
        case Dir::Right: return Rect(r.x + r.w - r.w / 2, r.y, r.w / 2, r.h);
        case Dir::Up:    return Rect(r.x, r.y, r.w, r.h / 2);
        default:         return Rect(r.x, r.y + r.h - r.h / 2, r.w, r.h / 2);
    }
}

} // namespace

HWND WindowManager::TiledWindowAt(Workspace* ws, POINT pt, HWND except) {
    if (!ws) return nullptr;
    for (HWND h : ws->tiled) {
        if (h == except) continue;
        ManagedWindow* mw = Find(h);
        if (!mw || mw->hidden || mw->minimized || mw->floating) continue;
        // Anything not taking part in the layout has no split to share.
        if (mw->immovable || mw->tooSmall || mw->tooLarge || mw->crowdedOut) continue;
        if (VisibleRect(h).contains(pt.x, pt.y)) return h;
    }
    return nullptr;
}

WindowManager::DropPlan WindowManager::PlanDrop(HWND moving, POINT pt) {
    DropPlan plan;

    ManagedWindow* mw = Find(moving);
    if (!mw || mw->floating || !cfg_ || !cfg_->dragToRearrange) return plan;
    if (!tilingEnabled_ || gameMode_) return plan;

    // Which display the pointer is over, not which one the window overlaps: a
    // half-dragged window straddles two screens and the pointer is what the
    // user is actually aiming with.
    const int monIndex = MonitorIndexFor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
    Monitor* mon = MonitorAt(monIndex);
    if (!mon) return plan;

    Workspace* ws = ActiveWorkspaceOf(monIndex);
    if (!ws) return plan;

    if (HWND target = TiledWindowAt(ws, pt, moving)) {
        plan.valid  = true;
        plan.target = target;
        plan.side   = DropSide(VisibleRect(target), pt);
        plan.where  = HalfTowards(VisibleRect(target), plan.side);
        return plan;
    }

    // Dropped in a gap, in the outer margin, or on bare desktop. Fall back to
    // the screen itself, which is also the only sensible answer on an empty
    // workspace.
    plan.valid  = true;
    plan.target = nullptr;
    plan.side   = DropSide(mon->info.work, pt);
    plan.where  = HalfTowards(mon->info.work, plan.side);
    return plan;
}

void WindowManager::PlaceBeside(Workspace* ws, HWND moving, HWND target, Dir side) {
    if (!ws || !moving || moving == target) return;

    ws->tree.MoveBeside(moving, target, side);

    // Master and Grid have no tree at all - they lay windows out straight from
    // this list - so it has to be moved in step, or the same drag would do two
    // different things depending on which layout happened to be selected.
    const bool movingFirst = (side == Dir::Left || side == Dir::Up);
    auto& list = ws->tiled;
    list.erase(std::remove(list.begin(), list.end(), moving), list.end());

    auto at = std::find(list.begin(), list.end(), target);
    if (at == list.end()) list.push_back(moving);
    else                  list.insert(movingFirst ? at : at + 1, moving);
}

void WindowManager::PlaceAtEdge(Workspace* ws, HWND moving, Dir side) {
    if (!ws || !moving) return;

    ws->tree.MoveToEdge(moving, side);

    const bool movingFirst = (side == Dir::Left || side == Dir::Up);
    auto& list = ws->tiled;
    list.erase(std::remove(list.begin(), list.end(), moving), list.end());
    if (movingFirst) list.insert(list.begin(), moving);
    else             list.push_back(moving);
}

void WindowManager::OnMoveSizeStart(HWND hwnd) {
    dragging_     = nullptr;
    dragIsResize_ = false;
    if (gameMode_ || !tilingEnabled_ || !cfg_ || !cfg_->dragToRearrange) return;

    ManagedWindow* mw = Find(hwnd);
    if (!mw || mw->floating || mw->hidden || mw->minimized) return;
    if (mw->immovable || mw->tooSmall || mw->tooLarge || mw->crowdedOut) return;

    dragging_       = hwnd;
    dragStartRect_  = VisibleRect(hwnd);
    if (msgWnd_) SetTimer(msgWnd_, TIMER_DRAG, 40, nullptr);
}

// Has this turned out to be a resize rather than a move? A few pixels of slack
// because a window that enforces its own size can end up a hair off the rect it
// was given, and that is not the user dragging a border.
bool WindowManager::DragChangedSize(HWND hwnd) const {
    const Rect now = VisibleRect(hwnd);
    return std::abs(now.w - dragStartRect_.w) > 4 ||
           std::abs(now.h - dragStartRect_.h) > 4;
}

void WindowManager::OnDragTick() {
    if (!dragging_) return;

    // The drag can end without EVENT_SYSTEM_MOVESIZEEND ever arriving - the
    // window closing mid-drag is the obvious case, and a modal loop that is
    // torn down rather than exited is the other. Notice it here rather than
    // leaving a timer and an indicator running for the rest of the session.
    if (!IsWindow(dragging_) || !Find(dragging_)) {
        DragGuideHide();
        dragging_ = nullptr;
        if (msgWnd_) KillTimer(msgWnd_, TIMER_DRAG);
        return;
    }

    // Once it has been a resize it stays one for the rest of the gesture: a
    // border drag that happens to pass back through its original size must not
    // start offering to rearrange the layout half way through.
    if (dragIsResize_ || DragChangedSize(dragging_)) {
        dragIsResize_ = true;
        DragGuideHide();
        return;
    }

    POINT pt;
    GetCursorPos(&pt);
    const DropPlan plan = PlanDrop(dragging_, pt);
    if (plan.valid) DragGuideShow(plan.where);
    else            DragGuideHide();
}

void WindowManager::OnMoveSizeEnd(HWND hwnd) {
    Busy guard(this);
    const HWND dragged = dragging_;
    dragging_ = nullptr;
    if (msgWnd_) KillTimer(msgWnd_, TIMER_DRAG);
    DragGuideHide();

    ManagedWindow* mw = Find(hwnd);
    if (!mw) {
        // A window we have never managed, which the user has just picked up
        // and put down. Moving a window is the most direct way somebody says
        // "this one, please" - and if it was turned away when it opened
        // because it had no title yet, this is the obvious second chance.
        if (AdoptWindow(hwnd)) RequestRetile();
        return;
    }

    if (mw->floating) {
        // A floating window that was dragged onto another display belongs to
        // that display now; a tiled one is handled below, by the drop, which
        // needs to know the destination workspace before it can place it.
        const int newMon = MonitorIndexForWindow(hwnd);
        if (newMon != mw->monitor && newMon < (int)monitors_.size()) {
            MoveWindowToWorkspace(hwnd, newMon, monitors_[newMon].active, true);
            return;
        }
        mw->savedRect = VisibleRect(hwnd);
        return;
    }

    POINT pt;
    GetCursorPos(&pt);

    // A resize is not a drop. OnDragTick usually notices first, but a quick
    // border drag can finish between two ticks, so the final size is checked
    // here as well - this is the authoritative one.
    const bool wasResize = dragIsResize_ || (dragged == hwnd && DragChangedSize(hwnd));
    dragIsResize_ = false;
    const bool rearrange = cfg_->dragToRearrange && dragged == hwnd && !wasResize;

    // Work out where it is going *before* moving it between workspaces, so the
    // answer is about where the pointer was let go rather than where the window
    // has just been put.
    const DropPlan plan = rearrange ? PlanDrop(hwnd, pt) : DropPlan{};

    // Followed the pointer onto another display: hand the window over first,
    // then place it within that display's active workspace.
    const int dropMon = MonitorIndexFor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
    if (dropMon != mw->monitor && dropMon >= 0 && dropMon < (int)monitors_.size()) {
        MoveWindowToWorkspace(hwnd, dropMon, monitors_[dropMon].active, true);
        mw = Find(hwnd);
        if (!mw) return;
    }

    if (plan.valid) {
        if (Workspace* ws = WorkspaceFor(*mw)) {
            if (plan.target && Find(plan.target)) PlaceBeside(ws, hwnd, plan.target, plan.side);
            else                                  PlaceAtEdge(ws, hwnd, plan.side);
        }
    }
    RequestRetile();   // snap back into the layout either way
}

void WindowManager::OnDisplayChange() {
    if (shutdown_) return;

    // WM_DISPLAYCHANGE and WM_SETTINGCHANGE are broadcast with
    // SendMessageTimeout, so they arrive as *sent* messages - which means they
    // can be delivered while this thread is parked inside a cross-process call
    // of its own. ReloadMonitors rebuilds `monitors_` wholesale, and doing that
    // under a Workspace& an outstanding pass is still using is a use-after-free.
    if (busy_) { deferredDisplayChange_ = true; return; }

    Busy guard(this);
    ReloadMonitors();
    RetileNow();
}

// ================================================================ focus helpers
HWND WindowManager::FocusedManaged() {
    if (focused_ && Find(focused_)) return focused_;
    HWND fg = GetForegroundWindow();
    if (fg && Find(fg)) { focused_ = fg; return fg; }
    if (Workspace* ws = ActiveWorkspaceOf(activeMonitor_)) {
        if (ws->lastFocused && Find(ws->lastFocused)) return ws->lastFocused;
        if (!ws->tiled.empty()) return ws->tiled.front();
        if (!ws->floats.empty()) return ws->floats.front();
    }
    return nullptr;
}

void WindowManager::FocusAndRemember(HWND h) {
    if (!h) return;
    // Only a real change counts as "the last window". Re-focusing what is
    // already focused - which happens on every retile that touches the
    // foreground window - would otherwise make ActFocusLast a no-op.
    if (focused_ && focused_ != h) lastFocused_ = focused_;
    FocusWindow(h);
    WarpCursorTo(h);
    focused_ = h;
    if (ManagedWindow* mw = Find(h)) {
        activeMonitor_ = mw->monitor;
        if (Workspace* ws = WorkspaceFor(*mw)) ws->lastFocused = h;
    }
    UpdateBorders();
}

HWND WindowManager::FindNeighbour(HWND from, Dir d) {
    if (!from) return nullptr;
    const Rect a = VisibleRect(from);

    HWND best = nullptr;
    long long bestScore = 0;

    for (auto& kv : managed_) {
        HWND h = kv.first;
        ManagedWindow& mw = kv.second;
        if (h == from || mw.hidden || mw.minimized) continue;
        if (!IsWindow(h) || !IsWindowVisible(h)) continue;

        const Rect b = VisibleRect(h);
        const long long dx = b.cx() - a.cx();
        const long long dy = b.cy() - a.cy();

        long long primary, secondary;
        switch (d) {
            case Dir::Left:  if (dx >= -2) continue; primary = -dx; secondary = (dy < 0 ? -dy : dy); break;
            case Dir::Right: if (dx <=  2) continue; primary =  dx; secondary = (dy < 0 ? -dy : dy); break;
            case Dir::Up:    if (dy >= -2) continue; primary = -dy; secondary = (dx < 0 ? -dx : dx); break;
            default:         if (dy <=  2) continue; primary =  dy; secondary = (dx < 0 ? -dx : dx); break;
        }
        // Strongly prefer windows that actually line up on the perpendicular axis.
        const long long score = primary + secondary * 3;
        if (!best || score < bestScore) { best = h; bestScore = score; }
    }
    return best;
}

// ================================================================ actions
void WindowManager::ActFocusDir(Dir d) {
    HWND cur = FocusedManaged();
    if (!cur) return;
    if (HWND next = FindNeighbour(cur, d)) FocusAndRemember(next);
}

void WindowManager::ActFocusCycle(int delta) {
    Workspace* ws = ActiveWorkspaceOf(activeMonitor_);
    if (!ws) return;

    std::vector<HWND> all;
    for (HWND h : ws->tiled) {
        ManagedWindow* mw = Find(h);
        if (mw && !mw->hidden && !mw->minimized) all.push_back(h);
    }
    for (HWND h : ws->floats) {
        ManagedWindow* mw = Find(h);
        if (mw && !mw->hidden && !mw->minimized) all.push_back(h);
    }
    if (all.empty()) return;

    HWND cur = FocusedManaged();
    auto it = std::find(all.begin(), all.end(), cur);
    int idx = (it == all.end()) ? 0 : (int)(it - all.begin()) + delta;
    const int n = (int)all.size();
    idx = ((idx % n) + n) % n;
    FocusAndRemember(all[idx]);
}

void WindowManager::ActSwapDir(Dir d) {
    HWND cur = FocusedManaged();
    if (!cur) return;
    ManagedWindow* mw = Find(cur);
    if (!mw || mw->floating) return;

    HWND other = FindNeighbour(cur, d);
    if (!other) return;
    ManagedWindow* om = Find(other);
    if (!om) return;

    if (om->monitor != mw->monitor) {
        MoveWindowToWorkspace(cur, om->monitor, om->workspace, true);
        return;
    }
    if (om->floating || om->workspace != mw->workspace) return;

    Workspace* ws = WorkspaceFor(*mw);
    if (!ws) return;
    ws->tree.Swap(cur, other);
    auto a = std::find(ws->tiled.begin(), ws->tiled.end(), cur);
    auto b = std::find(ws->tiled.begin(), ws->tiled.end(), other);
    if (a != ws->tiled.end() && b != ws->tiled.end()) std::iter_swap(a, b);
    RequestRetile();
}

void WindowManager::ActResizeDir(Dir d) {
    HWND cur = FocusedManaged();
    if (!cur) return;
    ManagedWindow* mw = Find(cur);
    if (!mw) return;
    Workspace* ws = WorkspaceFor(*mw);
    if (!ws) return;

    const float step = cfg_->resizeStep / 100.0f;

    if (mw->floating) {
        Monitor* mon = MonitorAt(mw->monitor);
        if (!mon) return;
        Rect r = VisibleRect(cur);
        const Rect& work = mon->info.work;
        const int dxp = (int)(work.w * step);
        const int dyp = (int)(work.h * step);
        switch (d) {
            case Dir::Left:  r.w -= dxp; break;
            case Dir::Right: r.w += dxp; break;
            case Dir::Up:    r.h -= dyp; break;
            default:         r.h += dyp; break;
        }
        r.w = (std::max)(120, r.w);
        r.h = (std::max)(80, r.h);
        PlaceWindow(cur, r, nullptr);
        mw->savedRect = r;
        return;
    }

    switch (ws->layout) {
        case LayoutKind::Dwindle:
            if (ws->tree.Resize(cur, d, step)) RequestRetile();
            break;
        case LayoutKind::Master: {
            const bool grow = (d == Dir::Right);
            const bool shrink = (d == Dir::Left);
            if (!grow && !shrink) return;
            ws->masterRatio += grow ? step : -step;
            ws->masterRatio = (std::max)(0.15f, (std::min)(0.85f, ws->masterRatio));
            RequestRetile();
            break;
        }
        default:
            break;   // Grid and Monocle have nothing to resize
    }
}

void WindowManager::ActSwitchWorkspace(int index) {
    Busy guard(this);
    Monitor* mon = ActiveMonitor();
    if (!mon || index < 0 || index >= (int)mon->workspaces.size()) return;
    if (mon->active == index) return;

    const int prev = mon->active;
    mon->active = index;

    // Collected before anything is shown or hidden. SetHidden ends in a
    // ShowWindow on another process's window, which pumps this thread's sent
    // messages - and a window event arriving in there erases from `managed_`
    // under the iterator this loop is holding.
    // Sticky windows travel rather than hide. Moving them onto the incoming
    // workspace - instead of teaching the layout about a third kind of window -
    // means they arrive as ordinary tiles and every pass downstream, tree
    // included, needs to know nothing about them.
    std::vector<HWND> travelling;
    std::vector<std::pair<HWND, bool>> flips;
    flips.reserve(managed_.size());
    for (const auto& kv : managed_) {
        const ManagedWindow& mw = kv.second;
        if (mw.monitor != activeMonitor_ || mw.scratch) continue;
        if (mw.sticky) { travelling.push_back(kv.first); continue; }
        if (mw.workspace == prev)  flips.push_back({ kv.first, true });
        if (mw.workspace == index) flips.push_back({ kv.first, false });
    }
    for (const auto& f : flips)
        if (ManagedWindow* mw = Find(f.first)) SetHidden(mw, f.second);

    for (HWND h : travelling) {
        ManagedWindow* mw = Find(h);
        if (!mw || mw->workspace == index) continue;
        DetachFromWorkspace(h);
        mw->workspace = index;
        Workspace& dst = mon->workspaces[index];
        if (mw->floating) dst.floats.push_back(h);
        else              dst.tiled.push_back(h);
        SetHidden(mw, false);
    }

    RetileNow();

    Workspace& ws = mon->workspaces[index];
    HWND target = ws.lastFocused;
    if (!target || !Find(target)) target = ws.tiled.empty()
        ? (ws.floats.empty() ? nullptr : ws.floats.front())
        : ws.tiled.front();
    if (target) FocusAndRemember(target);
    else { focused_ = nullptr; UpdateBorders(); }

    AWA_LOG(L"workspace -> %d", index + 1);
}

void WindowManager::ActMoveToWorkspace(int index) {
    HWND cur = FocusedManaged();
    if (!cur) return;
    Monitor* mon = ActiveMonitor();
    if (!mon || index < 0 || index >= (int)mon->workspaces.size()) return;
    const bool staying = (mon->active == index);
    MoveWindowToWorkspace(cur, activeMonitor_, index, false);

    // Focus moves on to whatever is left behind - but only when the window has
    // actually gone somewhere else. Sending it to the workspace it is already
    // on (which is how a window leaves the scratchpad) must not hand focus to
    // whichever window happens to be first in the list.
    if (staying) {
        FocusAndRemember(cur);
    } else if (Workspace* ws = ActiveWorkspaceOf(activeMonitor_)) {
        HWND next = ws->tiled.empty() ? nullptr : ws->tiled.front();
        if (next) FocusAndRemember(next);
    }
    RetileNow();
}

void WindowManager::ActCycleLayout() {
    Workspace* ws = ActiveWorkspaceOf(activeMonitor_);
    if (!ws) return;
    ws->layout = (LayoutKind)(((int)ws->layout + 1) % (int)LayoutKind::COUNT);
    AWA_LOG(L"layout -> %s", LayoutName(ws->layout));
    RetileNow();
}

void WindowManager::SetLayoutEverywhere(LayoutKind k, float ratio, int count) {
    for (auto& mon : monitors_) {
        for (auto& ws : mon.workspaces) {
            ws.layout      = k;
            ws.masterRatio = ratio;
            ws.masterCount = count;
        }
    }
    RetileNow();
}

void WindowManager::ActSetLayout(LayoutKind k) {
    Workspace* ws = ActiveWorkspaceOf(activeMonitor_);
    if (!ws) return;
    ws->layout = k;
    RetileNow();
}

void WindowManager::ActToggleFloat() {
    HWND cur = FocusedManaged();
    if (!cur) return;
    ManagedWindow* mw = Find(cur);
    if (!mw) return;
    Workspace* ws = WorkspaceFor(*mw);
    if (!ws) return;

    if (mw->floating) {
        mw->floating = false;
        ws->floats.erase(std::remove(ws->floats.begin(), ws->floats.end(), cur), ws->floats.end());
        ws->tiled.push_back(cur);
        ws->tree.Insert(cur, ws->lastFocused, false);
    } else {
        mw->floating = true;
        ws->tiled.erase(std::remove(ws->tiled.begin(), ws->tiled.end(), cur), ws->tiled.end());
        ws->tree.Remove(cur);
        ws->floats.push_back(cur);

        // Give it a sane centred size rather than leaving it filling a tile slot.
        Monitor* mon = MonitorAt(mw->monitor);
        if (!mon) { RequestRetile(); return; }
        const Rect& work = mon->info.work;
        Rect r = mw->savedRect;
        if (r.empty() || r.w > work.w || r.h > work.h) {
            r.w = (int)(work.w * 0.6);
            r.h = (int)(work.h * 0.6);
        }
        r.x = work.x + (work.w - r.w) / 2;
        r.y = work.y + (work.h - r.h) / 2;
        {
            PlaceWindow(cur, r, nullptr);
            BringWindowToTop(cur);
        }
        mw->savedRect = r;
    }
    RequestRetile();
}

void WindowManager::ActToggleFullscreen() {
    HWND cur = FocusedManaged();
    if (!cur) return;
    ManagedWindow* mw = Find(cur);
    if (!mw) return;

    Monitor* mon = MonitorAt(mw->monitor);
    if (!mon) return;

    mw->fullscreen = !mw->fullscreen;
    if (mw->fullscreen) {
        mw->savedRect = VisibleRect(cur);
        PlaceWindow(cur, mon->info.full, nullptr);
        BringWindowToTop(cur);
    } else if (mw->floating) {
        PlaceWindow(cur, mw->savedRect, nullptr);
    }
    RetileNow();
}

void WindowManager::ActCloseFocused() {
    HWND cur = FocusedManaged();
    if (cur) awa::CloseWindow(cur);
}

void WindowManager::ActMinimizeFocused() {
    HWND cur = FocusedManaged();
    if (cur) ShowWindow(cur, SW_MINIMIZE);
}

// The two split actions share everything except the tree call, and both have
// the same three reasons to decline: nothing focused, the window is floating,
// or this workspace is not on a layout that has a tree.
void WindowManager::ActToggleSplit() {
    HWND cur = FocusedManaged();
    if (!cur) return;
    ManagedWindow* mw = Find(cur);
    if (!mw || mw->floating) return;
    Workspace* ws = WorkspaceFor(*mw);
    if (!ws || ws->layout != LayoutKind::Dwindle) return;
    if (ws->tree.ToggleSplit(cur)) RetileNow();
}

void WindowManager::ActSwapSplit() {
    HWND cur = FocusedManaged();
    if (!cur) return;
    ManagedWindow* mw = Find(cur);
    if (!mw || mw->floating) return;
    Workspace* ws = WorkspaceFor(*mw);
    if (!ws || ws->layout != LayoutKind::Dwindle) return;
    if (ws->tree.SwapSplit(cur)) RetileNow();
}

void WindowManager::ActFocusLast() {
    HWND back = lastFocused_;
    // A window can go away, be minimised, or be hidden by a workspace switch
    // between the two presses. None of those is worth a message; the key
    // simply does nothing, exactly as it does with nothing to go back to.
    if (!back || !IsWindow(back)) return;
    ManagedWindow* mw = Find(back);
    if (!mw || mw->hidden || mw->minimized) return;

    FocusAndRemember(back);   // which makes the window we just left the new last
}

void WindowManager::ActWorkspaceRelative(int delta, bool onlyUsed) {
    Monitor* mon = ActiveMonitor();
    if (!mon || mon->workspaces.empty()) return;

    const int count = (int)mon->workspaces.size();
    const int step  = (delta < 0) ? -1 : 1;

    // Wraps, because nine workspaces in a ring is what the key is for; without
    // it, "next" on workspace nine does nothing and feels broken. At most one
    // full lap, so an empty set of workspaces cannot spin here forever.
    int at = mon->active;
    for (int i = 0; i < count; ++i) {
        at = (at + step + count) % count;
        if (at == mon->active) break;                 // all the way round
        if (!onlyUsed || !mon->workspaces[at].Empty()) {
            ActSwitchWorkspace(at);
            return;
        }
    }
}

WindowManager::Snapshot WindowManager::TakeSnapshot() {
    Snapshot s;
    // The same answer the actions use. `focused_` alone is not it: FocusedManaged
    // falls back to the real foreground window when nothing has been focused
    // through us yet, so reporting the raw field would say "nothing is focused"
    // about the very window the next command is going to act on.
    HWND focus = (focused_ && Find(focused_)) ? focused_ : GetForegroundWindow();
    if (focus && !Find(focus)) focus = nullptr;
    s.activeLayout    = ActiveLayout();
    s.activeMonitor   = activeMonitor_;
    s.activeWorkspace = ActiveWorkspace();
    s.tiling          = tilingEnabled_;
    s.gaps            = gapsEnabled_;
    s.gameMode        = gameMode_;
    s.blocked         = (int)blocked_.size();

    for (size_t mi = 0; mi < monitors_.size(); ++mi) {
        const Monitor& m = monitors_[mi];
        Snapshot::Mon mon;
        mon.index           = (int)mi;
        mon.activeWorkspace = m.active;
        mon.primary         = m.info.primary;
        mon.full            = m.info.full;
        mon.work            = m.info.work;
        s.monitors.push_back(mon);

        for (size_t wi = 0; wi < m.workspaces.size(); ++wi) {
            Snapshot::Ws ws;
            ws.monitor = (int)mi;
            ws.index   = (int)wi;
            ws.active  = ((int)wi == m.active);
            ws.layout  = m.workspaces[wi].layout;
            ws.windows = 0;      // counted below, from `managed_`
            s.workspaces.push_back(ws);
        }
    }

    for (const auto& kv : managed_) {
        const ManagedWindow& mw = kv.second;
        Snapshot::Win w;
        w.hwnd       = kv.first;
        w.title      = WindowTitle(kv.first);
        w.cls        = WindowClass(kv.first);
        w.proc       = ProcessName(kv.first);
        w.monitor    = mw.monitor;
        w.workspace  = mw.workspace;
        w.floating   = mw.floating;
        w.minimized  = mw.minimized;
        w.hidden     = mw.hidden;
        w.fullscreen = mw.fullscreen;
        w.immovable  = mw.immovable;
        w.sticky     = mw.sticky;
        w.scratch    = mw.scratch;
        w.focused    = (kv.first == focus);
        w.rect       = VisibleRect(kv.first);
        s.windows.push_back(w);

        for (auto& ws : s.workspaces)
            if (ws.monitor == mw.monitor && ws.index == mw.workspace) { ++ws.windows; break; }
    }

    // Stable output, so a caller diffing two snapshots sees real changes
    // rather than unordered_map iteration order.
    std::sort(s.windows.begin(), s.windows.end(),
              [](const Snapshot::Win& a, const Snapshot::Win& b) {
                  if (a.monitor   != b.monitor)   return a.monitor   < b.monitor;
                  if (a.workspace != b.workspace) return a.workspace < b.workspace;
                  return a.hwnd < b.hwnd;
              });
    return s;
}

void WindowManager::PublishManagedWindows() const {
    std::vector<HWND> live;
    live.reserve(managed_.size());
    for (const auto& kv : managed_) {
        const ManagedWindow& mw = kv.second;
        if (mw.hidden || mw.minimized || mw.immovable) continue;
        live.push_back(kv.first);
    }
    ModDragPublishWindows(live);
}

bool WindowManager::Manages(HWND h) const {
    auto it = managed_.find(h);
    if (it == managed_.end()) return false;
    const ManagedWindow& mw = it->second;
    // Hidden, minimised or immovable are all "there is nothing a drag could
    // usefully do", and the last one is a window Windows will not let us
    // touch at all.
    return !mw.hidden && !mw.minimized && !mw.immovable;
}

void WindowManager::ActPromote() {
    HWND cur = FocusedManaged();
    if (!cur) return;
    ManagedWindow* mw = Find(cur);
    if (!mw || mw->floating) return;
    Workspace* ws = WorkspaceFor(*mw);
    if (!ws || ws->tiled.empty()) return;

    ws->tree.Promote(cur);
    auto it = std::find(ws->tiled.begin(), ws->tiled.end(), cur);
    if (it != ws->tiled.end() && it != ws->tiled.begin())
        std::iter_swap(it, ws->tiled.begin());
    RequestRetile();
}

void WindowManager::ActFocusMonitor(int delta) {
    if (monitors_.size() < 2) return;
    const int n = (int)monitors_.size();
    activeMonitor_ = ((activeMonitor_ + delta) % n + n) % n;

    Workspace* ws = ActiveWorkspaceOf(activeMonitor_);
    if (!ws) return;
    HWND target = ws->lastFocused;
    if (!target || !Find(target))
        target = ws->tiled.empty() ? (ws->floats.empty() ? nullptr : ws->floats.front())
                                   : ws->tiled.front();
    if (target) FocusAndRemember(target);
}

void WindowManager::ActMoveToMonitor(int delta) {
    if (monitors_.size() < 2) return;
    HWND cur = FocusedManaged();
    if (!cur) return;
    const int n = (int)monitors_.size();
    const int dst = ((activeMonitor_ + delta) % n + n) % n;
    MoveWindowToWorkspace(cur, dst, monitors_[dst].active, true);
    RetileNow();
}

void WindowManager::SetTilingEnabled(bool on) {
    tilingEnabled_ = on;
    AWA_LOG(L"tiling %s", on ? L"on" : L"off");
    if (on) RetileNow();
}

// Lifts a window out of whatever workspace is holding it, leaving it in
// `managed_` but taking no part in any layout. Shared by the scratchpad and by
// sticky, which both need to move a window between workspaces without the
// bookkeeping that MoveWindowToWorkspace does around focus and visibility.
void WindowManager::DetachFromWorkspace(HWND h) {
    ManagedWindow* mw = Find(h);
    if (!mw) return;
    if (Workspace* ws = WorkspaceFor(*mw)) {
        ws->tiled.erase(std::remove(ws->tiled.begin(), ws->tiled.end(), h), ws->tiled.end());
        ws->floats.erase(std::remove(ws->floats.begin(), ws->floats.end(), h), ws->floats.end());
        ws->tree.Remove(h);
        if (ws->lastFocused == h)
            ws->lastFocused = ws->tiled.empty() ? nullptr : ws->tiled.front();
    }
}

void WindowManager::WarpCursorTo(HWND h) {
    if (!cfg_ || !cfg_->cursorWarp || !h || !IsWindow(h)) return;
    const Rect r = VisibleRect(h);
    if (r.empty()) return;
    SetCursorPos(r.cx(), r.cy());
}

bool WindowManager::ActFocusWindowById(HWND h) {
    Busy guard(this);
    ManagedWindow* mw = Find(h);
    if (!mw || !IsWindow(h)) return false;

    // Following it across a workspace boundary is the useful behaviour: a
    // script that has just found the window it wants does not also want to be
    // told it is on the wrong workspace.
    if (Monitor* mon = MonitorAt(mw->monitor)) {
        if (mon->active != mw->workspace && !mw->sticky && !mw->scratch) {
            activeMonitor_ = mw->monitor;
            ActSwitchWorkspace(mw->workspace);
        }
    }
    if (mw->minimized) ShowWindow(h, SW_RESTORE);
    FocusAndRemember(h);
    WarpCursorTo(h);
    return true;
}

void WindowManager::ActToggleSticky() {
    Busy guard(this);
    HWND cur = FocusedManaged();
    if (!cur) return;
    ManagedWindow* mw = Find(cur);
    if (!mw || mw->scratch) return;         // the scratchpad is its own thing

    mw->sticky = !mw->sticky;
    AWA_LOG(L"sticky %p %s", (void*)cur, mw->sticky ? L"on" : L"off");
    // Nothing else to do: a sticky window already sits on the workspace the
    // user is looking at, and ActSwitchWorkspace brings it along from here on.
    RequestRetile();
}

void WindowManager::ActScratchpadMove() {
    Busy guard(this);
    HWND cur = FocusedManaged();
    if (!cur) return;
    ManagedWindow* mw = Find(cur);
    if (!mw || mw->scratch) return;

    // A floating window keeps the size the user gave it. A tiled one has no
    // size worth keeping: its rect is a slot the layout chose, and savedRect
    // may be another one from before it was adopted. Clearing it hands the
    // summon path its default - centred, a comfortable fraction of the screen -
    // which is what i3 does the first time a window comes out of the
    // scratchpad, and for the same reason: restoring a window to a 294-pixel
    // column in the middle of the screen is not summoning it.
    if (mw->floating) mw->savedRect = VisibleRect(cur);
    else              mw->savedRect = Rect();

    DetachFromWorkspace(cur);
    mw->scratch = true;
    mw->sticky  = false;
    if (scratchShown_ == cur) scratchShown_ = nullptr;
    scratch_.erase(std::remove(scratch_.begin(), scratch_.end(), cur), scratch_.end());
    scratch_.push_back(cur);

    SetHidden(mw, true);
    if (focused_ == cur) focused_ = nullptr;
    AWA_LOG(L"scratchpad: parked %p (%d waiting)", (void*)cur, (int)scratch_.size());

    RequestRetile();
    UpdateBorders();
}

void WindowManager::ActScratchpadToggle() {
    Busy guard(this);

    // Something is on screen: put it away again. Same key both ways, which is
    // the whole ergonomic point of a scratchpad.
    if (scratchShown_) {
        HWND shown = scratchShown_;
        scratchShown_ = nullptr;
        if (ManagedWindow* mw = Find(shown)) {
            // It was floating while it was up, so wherever the user left it is
            // where it should come back.
            mw->savedRect = VisibleRect(shown);
            SetHidden(mw, true);
        }
        AWA_LOG(L"scratchpad: dismissed %p", (void*)shown);
        UpdateBorders();
        return;
    }

    // Drop anything that has closed since it was parked, then take the most
    // recently sent - which is nearly always the one being asked for.
    while (!scratch_.empty() && (!IsWindow(scratch_.back()) || !Find(scratch_.back())))
        scratch_.pop_back();
    if (scratch_.empty()) return;

    HWND h = scratch_.back();
    ManagedWindow* mw = Find(h);
    if (!mw) return;

    // Centred on the monitor the user is actually on, at a readable size, and
    // above everything. It is a summoned window, not a tile: it deliberately
    // takes no part in the layout while it is up.
    Monitor* mon = ActiveMonitor();
    if (mon) {
        const Rect& work = mon->info.work;
        Rect r = mw->savedRect;
        if (r.empty() || r.w > work.w || r.h > work.h) {
            r.w = (int)(work.w * 0.6);
            r.h = (int)(work.h * 0.6);
        }
        r.x = work.x + (work.w - r.w) / 2;
        r.y = work.y + (work.h - r.h) / 2;
        mw->monitor = (int)(mon - &monitors_[0]);
        SetHidden(mw, false);
        PlaceWindow(h, r, nullptr);
        mw->savedRect = r;
    } else {
        SetHidden(mw, false);
    }

    BringWindowToTop(h);
    scratchShown_ = h;
    FocusAndRemember(h);
    WarpCursorTo(h);
    AWA_LOG(L"scratchpad: summoned %p", (void*)h);
}

void WindowManager::ActToggleTiling() { SetTilingEnabled(!tilingEnabled_); }

void WindowManager::ActToggleGaps() {
    gapsEnabled_ = !gapsEnabled_;
    RetileNow();
}

} // namespace awa
