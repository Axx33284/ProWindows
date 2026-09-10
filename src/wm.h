// ProWindows - window manager core
#pragma once
#include "common.h"
#include "config.h"
#include "layout.h"
#include "winutil.h"

namespace awa {

struct ManagedWindow {
    HWND hwnd       = nullptr;
    int  monitor    = 0;        // index into monitors_
    int  workspace  = 0;        // 0-based
    bool floating   = false;
    bool fullscreen = false;
    bool minimized  = false;
    bool hidden     = false;    // hidden by us because its workspace is inactive

    // Follows the user from workspace to workspace instead of staying on one.
    // i3 calls it sticky, Hyprland calls it pin. Implemented by moving the
    // window to whichever workspace is being switched to, rather than by
    // special-casing it in the layout - so a sticky window is an ordinary tile
    // everywhere it appears, and nothing downstream needs to know.
    bool sticky     = false;

    // Parked in the scratchpad: on no workspace at all, hidden, waiting to be
    // summoned. See WindowManager::ActScratchpadToggle.
    bool scratch    = false;
    Rect savedRect;             // geometry to restore when floating / un-fullscreening

    // What this window will actually accept. Seeded from WM_GETMINMAXINFO and
    // then corrected by watching what it does with the rect we hand it, because
    // plenty of applications enforce limits they never declare.
    SizeLimits limits;
    bool       limitsAsked = false;

    // Consecutive placements this window ignored completely. Windows at a
    // higher integrity level are the usual cause: UIPI refuses our SetWindowPos
    // and returns success-shaped silence. After a few, we stop reserving a tile
    // for a window we cannot move, which is what leaves a hole in the layout.
    int  refusals   = 0;
    bool immovable  = false;

    // Times this window has been handed a slot and used almost none of it. A
    // file-copy dialog does this: it is top-level and has a resize frame, so it
    // looks tileable, but it keeps its own small size and the rest of the slot
    // stays bare desktop. Some of those cases the layout can absorb by giving
    // the surplus to a neighbour; the ones it cannot are the windows whose
    // limit is on the axis their split does not run along, and the only real
    // answer for those is to stop tiling them.
    int  wastes     = 0;
    bool tooSmall   = false;

    // The opposite failure, and the one people actually notice: a window whose
    // minimum size is larger than the slot the layout can offer it. Steam is
    // the standard example - it will not go below roughly 1000x600 whatever it
    // is told - so it keeps its own size, overhangs its tile and covers its
    // neighbours. The layout can absorb a minimum that merely squeezes the
    // board; what it cannot absorb is one that does not fit on the board at
    // all, and for those the only honest answer is to stop tiling the window.
    bool tooLarge   = false;

    // Set for this pass only, by the feasibility check in RetileMonitor: the
    // window is fine in principle but there is no room for it alongside
    // everything else on this workspace right now. Unlike tooLarge this is
    // recomputed every pass, so closing a neighbour brings the window straight
    // back into the tiling.
    bool crowdedOut = false;

    // "chrome.exe|Chrome_WidgetWin_1" - what learned limits are stored against,
    // so the next window of the same kind starts out already knowing them
    // instead of discovering them by overlapping its neighbours again.
    std::wstring limitKey;

    // Where this window was the last time we looked. A limit is only believed
    // once the window has held the same size across two consecutive looks: an
    // application that is still starting up, or still processing our resize,
    // is indistinguishable from one enforcing a limit, and a false limit is
    // considerably worse than no limit at all.
    Rect lastSeen;
    bool haveSeen = false;
};

struct Workspace {
    std::vector<HWND> tiled;
    std::vector<HWND> floats;
    BspTree    tree;
    LayoutKind layout      = LayoutKind::Dwindle;
    float      masterRatio = 0.55f;
    int        masterCount = 1;
    HWND       lastFocused = nullptr;

    bool Empty() const { return tiled.empty() && floats.empty(); }
};

struct Monitor {
    MonitorInfo info;
    std::vector<Workspace> workspaces;
    int active = 0;
};

class WindowManager {
public:
    bool Init(HWND msgWnd, Config* cfg);
    void Shutdown();                    // un-hides everything it hid

    // ---- lifecycle -------------------------------------------------------
    void ReloadMonitors();
    void ScanExistingWindows();
    void RequestRetile();               // debounced; safe to call from events
    void RetileNow();
    // True while a pass is still owed. Lets the posted WM_AWA_RETILE drop
    // itself when the timer got there first, rather than running a second
    // pass that has nothing left to do.
    bool RetilePending() const { return retilePending_; }
    void ApplyConfigChanged();

    // ---- events ----------------------------------------------------------
    void OnWinEvent(DWORD event, HWND hwnd);
    void OnDisplayChange();
    void OnMoveSizeStart(HWND hwnd);
    void OnMoveSizeEnd(HWND hwnd);
    void OnForeground(HWND hwnd);

    // Called while a drag is in progress, to keep the drop indicator under the
    // pointer. Does nothing unless a tiled window is actually being dragged.
    void OnDragTick();
    bool Dragging() const { return dragging_ != nullptr; }

    // Where a window dropped at `pt` would land: the tile it would share a
    // split with, and which side of that tile it would take. `target` comes
    // back null when the drop would put the window against an edge of the
    // whole screen instead. Exposed so the drop indicator and the drop itself
    // are answering the same question with the same code, rather than two
    // implementations that agree until one of them is edited.
    struct DropPlan {
        bool valid  = false;
        HWND target = nullptr;      // null = against the edge of the work area
        Dir  side   = Dir::Right;
        Rect where;                 // the region the window would occupy
    };
    DropPlan PlanDrop(HWND moving, POINT pt);

    // ---- actions ---------------------------------------------------------
    void ActFocusDir(Dir d);
    void ActSwapDir(Dir d);
    void ActResizeDir(Dir d);
    void ActFocusCycle(int delta);
    void ActSwitchWorkspace(int index);
    void ActMoveToWorkspace(int index);
    void ActCycleLayout();
    void ActSetLayout(LayoutKind k);

    // Used by the settings window: pushes one layout choice to every workspace
    // on every monitor, rather than just the active one.
    void SetLayoutEverywhere(LayoutKind k, float masterRatio, int masterCount);
    void ActToggleFloat();
    void ActToggleFullscreen();
    void ActCloseFocused();
    void ActMinimizeFocused();
    void ActPromote();
    void ActFocusMonitor(int delta);
    void ActMoveToMonitor(int delta);
    void ActToggleTiling();
    void ActToggleGaps();

    // Focus one particular window. There is no keyboard shortcut for this and
    // there could not be a useful one - it exists for the control channel, so a
    // script can query `get windows` and then act on the one it wanted. i3 does
    // the same thing through criteria like [con_id=...] focus.
    // False when that window is not one we manage.
    bool ActFocusWindowById(HWND h);

    // Keep the focused window on every workspace, or stop.
    void ActToggleSticky();

    // i3's scratchpad, in two halves: send the focused window to the holding
    // area, and summon or dismiss whatever is waiting there.
    void ActScratchpadMove();
    void ActScratchpadToggle();

    // Flip the split the focused window sits under, or exchange its two
    // halves. Dwindle only - Master, Grid and Monocle do not have a tree to
    // flip, and pretending otherwise would silently do nothing.
    void ActToggleSplit();
    void ActSwapSplit();

    // Back to whichever window had focus before this one, and pressing it
    // again comes back. `lastFocused_` is the whole of it.
    void ActFocusLast();

    // Workspace by step rather than by number. `onlyUsed` skips the empty
    // ones, which is Hyprland's `workspace, e+1`.
    void ActWorkspaceRelative(int delta, bool onlyUsed);

    // ---- state for the tray UI -------------------------------------------
    bool  TilingEnabled() const { return tilingEnabled_; }
    void  SetTilingEnabled(bool on);
    bool  GapsEnabled() const { return gapsEnabled_; }
    LayoutKind ActiveLayout() const;
    int   ActiveWorkspace() const;
    int   ManagedCount() const { return (int)managed_.size(); }
    // Is this window one we are arranging, and in a state where a gesture on
    // it means anything? Asked by the mod-drag hook's handler, which must not
    // pick up a window ProWindows has been told to leave alone.
    bool  Manages(HWND h) const;
    void  RestoreAllWindows();          // emergency un-hide / un-tile

    // Windows we are not permitted to move because they run at a higher
    // integrity level than we do. Shown in the settings status line so the
    // situation is visible rather than mysterious.
    int   BlockedCount() const { return (int)blocked_.size(); }

    // True while a fullscreen application (a game, nearly always) owns the
    // screen. Everything that touches other windows is parked for the
    // duration: no tiling, no animation, no DWM borders, no focus polling.
    bool  GameMode() const { return gameMode_; }

    // Re-checks whether a fullscreen application is running and enters or
    // leaves that parked state. Cheap, and rate-limited internally, so it is
    // safe to call from the event path.
    void  UpdateGameMode(bool force = false);

    // ---- state, for anything that needs to report it ---------------------
    // One flat picture of what the manager currently believes, taken in one
    // go. The control channel and the settings window both want this, and
    // handing either of them the live tables would mean handing them pointers
    // that the next window event invalidates.
    struct Snapshot {
        struct Win {
            HWND hwnd = nullptr;
            std::wstring title, cls, proc;
            int  monitor = 0, workspace = 0;
            bool floating = false, minimized = false, hidden = false;
            bool fullscreen = false, focused = false, immovable = false;
            bool sticky = false, scratch = false;
            Rect rect;
        };
        struct Ws {
            int  monitor = 0, index = 0, windows = 0;
            bool active = false;
            LayoutKind layout = LayoutKind::Dwindle;
        };
        struct Mon {
            int  index = 0, activeWorkspace = 0;
            bool primary = false;
            Rect full, work;
        };
        std::vector<Win> windows;
        std::vector<Ws>  workspaces;
        std::vector<Mon> monitors;
        LayoutKind activeLayout   = LayoutKind::Dwindle;
        int        activeMonitor  = 0;
        int        activeWorkspace = 0;
        bool       tiling = true, gaps = true, gameMode = false;
        int        blocked = 0;
    };
    Snapshot TakeSnapshot();

private:
    // -- helpers
    ManagedWindow* Find(HWND h);
    Workspace* WorkspaceFor(const ManagedWindow& mw);
    Workspace* ActiveWorkspaceOf(int monitorIndex);
    Monitor*   ActiveMonitor();
    Monitor*   MonitorAt(int index);       // bounds-checked, may return nullptr

    int  MonitorIndexFor(HMONITOR mon) const;
    int  MonitorIndexForWindow(HWND h) const;

    bool AddWindow(HWND h, bool focusIt);
    void RemoveWindow(HWND h);
    void MoveWindowToWorkspace(HWND h, int monitorIndex, int workspaceIndex, bool follow);

    // Adds `h`, taking focus only if it really is the foreground window.
    // EVENT_OBJECT_SHOW arrives for windows that opened in the background too,
    // and treating those as focused pointed every directional action at a
    // window the user had never looked at.
    bool AdoptWindow(HWND h);

    // ---- windows we have seen but cannot classify yet --------------------
    // Classify has to say no to a window with no title, a window DWM still has
    // cloaked, and a window whose styles have not been applied - and almost
    // every application produces all three in the first few milliseconds of
    // opening one. Looking exactly once and never again is what leaves a
    // window untiled until it is touched, so a rejection that might not last
    // buys the window a place here and a handful of second looks.
    struct Pending {
        int       tries   = 0;
        ULONGLONG firstAt = 0;
    };
    std::unordered_map<HWND, Pending> pending_;

    // A window is watched for about two seconds. That is far longer than any
    // application takes to finish presenting a window and far shorter than the
    // user would notice, and the cap is what stops a desktop full of windows
    // we will never manage from being re-examined forever.
    static constexpr int  kPendingTries      = 10;
    static constexpr UINT kPendingIntervalMs = 200;
    static constexpr size_t kMaxPending      = 64;

    void WatchForLater(HWND h);   // remember a window worth a second look
    void ForgetPending(HWND h);
public:
    void RetryPending();          // driven by TIMER_PENDING
private:

    // Hands the mod-drag mouse hook the set of windows it may pick up. Called
    // from the two places `managed_` changes, which is the only way that set
    // can move; see moddrag.h for why the hook needs it rather than asking.
    void PublishManagedWindows() const;

    void RetileMonitor(int monitorIndex);
    void ApplyPlacements(const std::vector<std::pair<HWND, Rect>>& plan);

    // What we asked for last pass, so the next one can see what happened.
    // `looks` counts how many times we have come back to a window that had not
    // finished moving: a window that never holds still - a video player, an
    // application still loading its content - would otherwise keep its entry
    // on the books forever and hold the whole verification pass open with it.
    struct Attempt { Rect before; Rect target; int looks = 0; };
    std::unordered_map<HWND, Attempt> attempts_;

    // Verification passes run back to back without any outside event. Learning
    // converges in two or three; anything past that is two limits contradicting
    // each other, and re-measuring three times a second for the rest of the
    // session helps nobody. Reset by RequestRetile, so a real window event
    // always buys a fresh budget.
    static constexpr int kMaxVerifyChain = 8;
    static constexpr int kSettleLooks    = 6;
    int  verifyChain_ = 0;
    // True only for a pass that this verification loop asked for itself. Any
    // other pass - a window event, a layout change, a workspace switch - is
    // the desktop moving on, and starts the budget again.
    bool verifyPass_  = false;

    // Compares the last pass's requests against where the windows actually
    // ended up and records what they refused. Returns true if anything was
    // learned, in which case the layout is worth recomputing.
    bool LearnFromLastPass();

    // Limits for every window taking part, for ComputeLayout.
    ConsMap ConstraintsFor(const std::vector<HWND>& order);

    // Set when LearnFromLastPass changed something, so RetileNow runs a second
    // pass with the new knowledge - and only ever a second one.
    bool relayoutPending_ = false;

    // Reported once per run, not once per pass.
    bool immovableAnnounced_ = false;

    // Windows classified as Blocked: real windows at an integrity level we
    // cannot reach. Held only so they can be counted and reported; nothing is
    // ever done to them.
    std::unordered_set<HWND> blocked_;
    bool blockedAnnounced_ = false;

    // Drops handles from blocked_ that are no longer windows. Called from the
    // retile path, where the rest of the bookkeeping happens.
    void PruneBlocked();

    // Removes from `order` any window whose own minimum size cannot be met on
    // this workspace, marking it crowdedOut so it floats for this pass instead
    // of overhanging its neighbours. Returns the windows it took out.
    std::vector<HWND> DropWindowsThatCannotFit(const Rect& area,
                                               std::vector<HWND>* order,
                                               const ConsMap& cons);

    // Parks a window outside the tiling with a sensible free-floating rect,
    // used for the windows the two rules above take out.
    void ParkAsFloating(HWND h, const Rect& area);

    // Learned size limits, keyed by "process|class", read from and written
    // back to the config so a limit is discovered once per application rather
    // than once per window.
    std::wstring LimitKeyFor(HWND h) const;
    void RememberLimits(const ManagedWindow& mw);
    bool limitsDirty_ = false;

    // ---- fullscreen application ("game mode") ----
    bool      gameMode_      = false;
    ULONGLONG gameCheckedAt_ = 0;

    // ---- drag ----
    // The tiled window currently being dragged, or null. Held so the indicator
    // knows what it is previewing and so the drop knows what to place even if
    // the pointer has wandered off the window by the time the button is let go.
    HWND dragging_ = nullptr;

    // Where it was when the drag started. EVENT_SYSTEM_MOVESIZESTART does not
    // say whether the user grabbed the title bar or a border, and the two mean
    // completely different things: dragging a border resizes the window, and
    // rearranging the layout because somebody widened a tile by ten pixels
    // would be indefensible. Comparing the size against this is the only way
    // to tell them apart.
    Rect dragStartRect_;
    bool dragIsResize_ = false;

    // Rearranges the tree and the workspace order so `moving` sits `side` of
    // `target`. Both have to move together: the tree drives Dwindle, the order
    // list drives Master and Grid, and a drag that only updated one of them
    // would do two different things depending on the layout.
    void PlaceBeside(Workspace* ws, HWND moving, HWND target, Dir side);
    void PlaceAtEdge(Workspace* ws, HWND moving, Dir side);

    // True once the window being dragged has changed size, i.e. the user
    // grabbed a border rather than the title bar.
    bool DragChangedSize(HWND hwnd) const;

    // The tiled window under `pt` on `ws`, skipping `except` and anything that
    // is not taking part in the layout.
    HWND TiledWindowAt(Workspace* ws, POINT pt, HWND except);
public:
    void AnimStep();                    // driven by WM_AWA_ANIMTICK
    // Clears the "a frame is queued" flag so the ticker may post the next one.
    void AnimTickHandled();
private:
    void UpdateBorders();
    void SetHidden(ManagedWindow* mw, bool hidden);

    HWND FocusedManaged();
    void FocusAndRemember(HWND h);
    HWND FindNeighbour(HWND from, Dir d);

    LayoutParams ParamsFor(const Monitor& mon, const Workspace& ws) const;

    // ---- reentrancy ------------------------------------------------------
    // Nearly everything in here ends in a cross-process call - ShowWindow,
    // SetWindowPlacement, the SendMessageTimeout behind WM_GETMINMAXINFO - and
    // every one of those pumps this thread's sent-message queue while it waits
    // for the other process to answer. A win-event callback or a broadcast
    // WM_SETTINGCHANGE delivered inside one of them re-enters the manager part
    // way through a pass: `managed_` is erased from under a ManagedWindow* the
    // outer frame is still holding, or `monitors_` is rebuilt under a
    // Workspace& it is still using. Both are use-after-free, and both present
    // as the application vanishing at random.
    //
    // Rather than auditing every call site for ever, the two entry points that
    // can mutate those containers queue themselves while a pass is in flight
    // and are replayed the moment the last one unwinds. Nothing is lost and
    // nothing runs on top of itself.
    int  busy_     = 0;
    bool draining_ = false;

    struct Busy {
        WindowManager* wm;
        explicit Busy(WindowManager* w) : wm(w) { ++wm->busy_; }
        // Draining from here is what guarantees it happens at all: every path
        // out of a pass, including an early return, goes through this.
        ~Busy() { if (--wm->busy_ == 0) wm->DrainDeferred(); }
        Busy(const Busy&) = delete;
        Busy& operator=(const Busy&) = delete;
    };
    friend struct Busy;

    struct DeferredEvent { DWORD event; HWND hwnd; };
    std::vector<DeferredEvent> deferred_;
    bool deferredDisplayChange_ = false;
    // A ceiling, because this queue is fed by window events and a busy desktop
    // produces a great many. Dropping the far end of a burst is better than
    // growing without bound; the retile that follows re-reads the world anyway.
    static constexpr size_t kMaxDeferred = 512;

    void HandleWinEvent(DWORD event, HWND hwnd);   // the body of OnWinEvent
    void DrainDeferred();

    // -- state
    HWND     msgWnd_       = nullptr;
    Config*  cfg_          = nullptr;
    bool     tilingEnabled_ = true;
    bool     gapsEnabled_   = true;
    bool     shutdown_      = false;   // Shutdown() has already run
    int      activeMonitor_ = 0;
    HWND     focused_       = nullptr;
    HWND     lastBordered_  = nullptr;
    // The window focus was on before `focused_`, for ActFocusLast. Updated in
    // one place - FocusAndRemember - so it cannot drift out of step with
    // focused_, and cleared by RemoveWindow when that window goes away.
    HWND     lastFocused_   = nullptr;

    // ---- retile debounce ----
    // SetTimer restarts a timer that is already running, so a steady stream of
    // window events used to be able to postpone the retile indefinitely. The
    // first request now fixes a deadline and later ones can only bring it
    // forward, never push it back.
    static constexpr UINT kRetileDebounceMs = 35;
    static constexpr UINT kRetileMaxDelayMs = 250;
    // How still the desktop has to have been for a request to skip the
    // debounce entirely and be posted instead. Long enough that a burst of
    // events from one application opening several windows still coalesces;
    // short enough that ordinary use - one window, then a pause, then another
    // - never waits at all.
    static constexpr UINT kRetileQuietMs    = 200;
    bool      retilePending_ = false;
    ULONGLONG retileDueAt_   = 0;
    ULONGLONG lastRetileAt_  = 0;

    std::vector<Monitor> monitors_;
    std::unordered_map<HWND, ManagedWindow> managed_;

    // Windows parked in the scratchpad, oldest first, so summoning takes the
    // one sent there most recently. Deliberately not per-monitor: the whole
    // point is that it follows you, and i3's is global for the same reason.
    std::vector<HWND> scratch_;
    HWND scratchShown_ = nullptr;      // the one currently on screen, or null

    // Takes a window out of whatever workspace lists and tree hold it, without
    // removing it from `managed_`. Shared by the scratchpad and by sticky.
    void DetachFromWorkspace(HWND h);

    // Puts the pointer on a window, if cursor_warp is on.
    void WarpCursorTo(HWND h);

    // Frame padding is measured once when the animation starts, so each tick is
    // a plain DeferWindowPos with no DWM round-trips.
    struct AnimEntry {
        HWND hwnd; Rect from; Rect to; FramePad pad;
        Rect last = {};      // last rect actually sent to this window
        bool sent = false;
    };
    std::vector<AnimEntry> anim_;

    // Windows that were part way through an animation when a new retile pass
    // replaced the plan. Those that appear in the new plan simply carry on
    // from where they are; those that do not - because this pass parked,
    // floated or crowded them out - were being left frozen at whatever
    // interpolated rect they had reached, having never arrived anywhere.
    // AnimCommit puts those on their target and clears the list.
    std::vector<AnimEntry> animCarry_;

    LARGE_INTEGER animStart_ = {};
    LARGE_INTEGER animFreq_  = {};
    bool  animTimerRaised_ = false;   // timeBeginPeriod is active

    void AnimBegin();                 // called once per retile pass
    void AnimCommit();                // starts the ticker if anything moved
    void AnimStop();
    void AnimShutdown();            // stops, then tears the ticker thread down

    // SetTimer cannot go below the ~15.6 ms system tick, which is only 64 fps
    // and looks stepped on a high-refresh screen. This thread waits on a
    // high-resolution timer and posts WM_AWA_ANIMTICK instead.
    // The thread is created once and parked between animations, rather than
    // spun up and torn down on every retile - which, since a retile happens
    // every time any window opens, closes or moves, was a kernel round trip
    // per window event for the life of the process.
    static DWORD WINAPI AnimThread(LPVOID self);
    void AnimTickLoop();
    LARGE_INTEGER animLastFrame_ = {};   // debug frame-cadence logging only
    int    animFrames_   = 0;            // summarised once per animation, not per frame
    double animWorstGap_ = 0.0;
    HANDLE animThread_ = nullptr;
    HANDLE animQuit_   = nullptr;
    HANDLE animActive_ = nullptr;     // set while an animation is running
    LONG   animPending_ = 0;          // a frame is already queued, do not pile up

};

// Un-hides every window the manager has hidden, working from a flat fixed-size
// array kept alongside the main tables rather than from those tables.
//
// This exists to be called from the unhandled-exception filter. The manager
// spends its life mutating an unordered_map on every window event, so a crash
// is most likely to happen part way through one - and walking a half-mutated
// map from the crash handler faults again, leaving the user with exactly the
// missing windows the handler was written to prevent.
void EmergencyUnhideAll();

} // namespace awa
