// ProWindows - configuration model + INI parsing
#pragma once
#include "common.h"
#include "montheme.h"

namespace awa {

// ---------------------------------------------------------------- layouts
enum class LayoutKind : int {
    Dwindle = 0,   // Hyprland-style binary space partitioning
    Master  = 1,   // master area + stack
    Grid    = 2,   // even grid
    // Monocle (one full-size window at a time) was removed in 1.3: Alt+F
    // covers the one case it was used for. "monocle" in a config file is
    // read as dwindle.
    COUNT   = 3
};

const wchar_t* LayoutName(LayoutKind k);
bool ParseLayout(const std::wstring& s, LayoutKind* out);

// ---------------------------------------------------------------- actions
enum Action : int {
    ACT_NONE = 0,
    ACT_FOCUS_DIR,          // arg = (int)Dir
    ACT_SWAP_DIR,           // arg = (int)Dir
    ACT_RESIZE_DIR,         // arg = (int)Dir
    ACT_FOCUS_NEXT,
    ACT_FOCUS_PREV,
    ACT_WORKSPACE,          // arg = workspace index (0-based)
    ACT_MOVE_TO_WORKSPACE,  // arg = workspace index (0-based)
    ACT_CYCLE_LAYOUT,
    ACT_SET_LAYOUT,         // arg = (int)LayoutKind
    ACT_TOGGLE_FLOAT,
    ACT_TOGGLE_FULLSCREEN,
    ACT_CLOSE_WINDOW,
    ACT_TOGGLE_TILING,
    ACT_RELOAD_CONFIG,
    ACT_TOGGLE_GAPS,
    ACT_PROMOTE,            // move focused window to the master / root slot
    ACT_FOCUS_MONITOR,      // arg = -1 previous, +1 next
    ACT_MOVE_TO_MONITOR,    // arg = -1 previous, +1 next
    ACT_MINIMIZE,
    ACT_RETILE,
    ACT_QUIT,
    ACT_LAUNCH,             // run a program; command line in Keybind::command
    ACT_LAUNCHER,           // the type-to-find app launcher
    // Flip the split the focused window sits under between side-by-side and
    // stacked. Hyprland's `togglesplit`: the automatic choice is right nearly
    // always and wrong for the one pair in front of you.
    ACT_TOGGLE_SPLIT,
    // Exchange the two halves of that split, subtrees and all. Hyprland's
    // `swapsplit`, and not expressible by swapping one window for another.
    ACT_SWAP_SPLIT,
    // Back to the window that had focus before this one, and then back again.
    // Alt+Tab that knows about the tiling: it never wanders into a window on
    // another workspace and never has to be held down.
    ACT_FOCUS_LAST,
    // Workspace by relative step rather than by number. arg = -1 / +1, and
    // ACT_WORKSPACE_USED skips the empty ones the way Hyprland's `e+1` does.
    ACT_WORKSPACE_REL,
    ACT_WORKSPACE_USED,

    // Visible on every workspace instead of just its own. i3 calls it sticky,
    // Hyprland calls it pin; the same idea either way, and the obvious home for
    // a music player or a chat window you want to keep an eye on.
    ACT_TOGGLE_STICKY,

    // i3's scratchpad. Send a window away to a holding area that belongs to no
    // workspace, then summon it over whatever you are looking at and dismiss it
    // again with the same key. The useful half of a "quake terminal" without
    // needing a terminal that supports one.
    ACT_SCRATCHPAD_MOVE,
    ACT_SCRATCHPAD_TOGGLE,

    // The clock panel (clock, alarms, timers, stopwatch): shows or hides it. Win+W.
    ACT_TIMER,
};

// How a binding actually reached us.
enum class BindRoute { Unregistered, Hotkey, Hook, Blocked };

struct Keybind {
    UINT mods = 0;          // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN
    UINT vk   = 0;
    Action action = ACT_NONE;
    int  arg  = 0;
    std::wstring command;   // ACT_LAUNCH only
    int  id   = 0;          // assigned at registration time
    std::wstring spec;      // original text, for the UI and for errors
    BindRoute route = BindRoute::Unregistered;
};

// Human-readable description of what a binding does, for the shortcuts list.
std::wstring DescribeAction(const Keybind& kb);

// "Alt + Shift + H" - for display, built from mods/vk rather than the spec text.
std::wstring DescribeChord(UINT mods, UINT vk);

// "alt+shift+h" - the parseable form, for writing back to config.ini.
std::wstring KeySpecText(UINT mods, UINT vk);

// "focus, left" / "launch, explorer.exe" - the parseable action form.
std::wstring ActionSpecText(const Keybind& kb);

// ---------------------------------------------------------------- config
struct Config {
    // gaps & look
    int   gapInner       = 8;
    int   gapOuter       = 8;
    bool  accentBorder   = true;
    COLORREF activeColor   = RGB(0x7A, 0xA2, 0xF7);
    COLORREF inactiveColor = RGB(0x30, 0x34, 0x40);
    int   cornerPref     = 0;      // 0 = leave alone, 1 = round, 2 = don't round

    // behaviour
    LayoutKind layout    = LayoutKind::Dwindle;
    float masterRatio    = 0.55f;
    int   masterCount    = 1;
    bool  tilingEnabled  = true;
    bool  focusFollowsMouse = false;
    bool  newWindowOnTop = true;   // insert new windows before the focused one
    // Dragging a tiled window rearranges the layout instead of the window
    // snapping back where it was. Named `swapOnDrag` when all it could do was
    // exchange two windows; it now places the dragged window on whichever side
    // of the tile you dropped it on.
    bool  dragToRearrange = true;
    // Hold $mod and drag anywhere on a window to move it, or right-drag to
    // resize it from the nearest corner. Windows gives a window exactly one
    // drag handle and a tiled window's is a few pixels tall; this is the
    // Hyprland gesture that makes that stop mattering. Only ever acts on
    // windows ProWindows is arranging, so an excluded app keeps its own
    // mod+drag - which is the point of having excluded it.
    bool  modDrag         = true;

    // One window on a workspace does not need to be held away from the screen
    // edges by anything: the gap is there to separate tiles from each other,
    // and with nothing to separate it is just wasted screen. Hyprland calls
    // this no_gaps_when_only and it is on by default there for good reason.
    bool  smartGaps       = true;

    // Put the pointer on a window when focus moves to it by keyboard. Without
    // it the pointer stays wherever it was, which matters as soon as
    // focus-follows-mouse is on as well - the two fight, and the window under
    // the stationary pointer takes focus straight back.
    bool  cursorWarp      = false;

    int   workspaceCount = 9;
    bool  animations     = true;
    int   animationMs    = 200;
    int   resizeStep     = 4;      // percent per resize keypress
    bool  manageMinimized = false;
    bool  startMinimized = false;  // launch straight to the tray, no window
    bool  debug          = false;

    // rules
    std::vector<std::wstring> ignoreProcess;   // never touched at all
    std::vector<std::wstring> ignoreClass;
    std::vector<std::wstring> ignoreTitle;     // substring match
    std::vector<std::wstring> floatProcess;    // managed, but always floating
    std::vector<std::wstring> floatClass;
    std::vector<std::wstring> floatTitle;

    // Size limits discovered by watching what an application actually does
    // with the rect it is handed, remembered between runs.
    //
    // Without this the discovery starts from scratch every time: the first
    // few seconds after Steam opens, it is handed a slot it will not accept,
    // overhangs its neighbours, and only then is the limit learned. Keyed by
    // "process|class" in lower case, because that is the granularity at which
    // the limit actually belongs - every Steam window has the same minimum,
    // and it is not a property of any one HWND.
    struct RememberedLimits {
        int minW = 0, minH = 0;
        int maxW = 0, maxH = 0;         // 0 means "no maximum was observed"
        bool tooLarge = false;          // proved it cannot be tiled at all
    };
    std::unordered_map<std::wstring, RememberedLimits> learnedLimits;

    // Cap on how many of those are kept, so config.ini cannot grow without
    // bound on a machine that opens a lot of different applications.
    static constexpr size_t kMaxLearnedLimits = 200;

    // Park the tiler entirely while a fullscreen application (a game, in
    // practice) owns the screen. On by default: arranging windows behind a
    // game costs frames at best, and on some drivers drops it out of
    // exclusive fullscreen at worst.
    bool  pauseForFullscreen = true;

    // screen space kept clear of tiles, on top of the normal work area. Lets
    // the layout make room for a bar that Windows doesn't report.
    int   marginTop    = 0;
    int   marginBottom = 0;
    int   marginLeft   = 0;
    int   marginRight  = 0;

    // ---- system monitor overlay ----
    bool  monitorEnabled  = false;
    int   monitorX        = INT_MIN;   // INT_MIN = never placed, pick a corner
    int   monitorY        = INT_MIN;
    bool  monitorPinned   = false;     // locked in place and click-through
    int   monitorOpacity  = 92;        // percent
    int   monitorScale    = 100;       // percent
    int   monitorInterval = 1000;      // sampling period, ms
    bool  monitorVertical = true;      // stacked rows, or one wide strip
    bool  monitorGraphs   = true;      // draw the history sparklines
    int   monitorTheme    = 0;         // index into the MonitorSkin table
    int   monitorStyle    = 0;         // MonStyle: rows, rings, arcs, bars...
    bool  monitorOnDesktop = false;    // live on the desktop, under every window
    bool  monitorTopApps  = false;     // name the busiest process per metric
    bool  monShowCpu      = true;
    bool  monShowRam      = true;
    bool  monShowGpu      = true;
    bool  monShowVram     = false;
    bool  monShowCpuTemp  = false;
    bool  monShowGpuTemp  = false;
    bool  monShowDisk     = false;
    bool  monShowNet      = false;
    bool  monitorSmooth   = true;      // ease readouts between samples
    // Draw the CPU bar as one segment per logical processor rather than as one
    // averaged bar. See MonPaintCtx::cores for why it is worth the pixels.
    bool  monitorCores    = true;

    // The order the readouts appear in the panel: `monOrder[0]` is the metric
    // drawn first. A permutation of MonMetric, never a set of flags - which
    // one is shown is monShow*, and the two are deliberately separate so
    // hiding a readout and bringing it back does not lose its place.
    //
    // Stored in config.ini by name, for the same reason monitor_theme is:
    // `monitor_order = cpu, ram, gpu` survives the enum being reordered, and
    // reads as what it is. MonitorNormaliseOrder repairs anything a
    // hand-edited file gets wrong.
    int monOrder[MON_METRIC_COUNT] = {
        MON_CPU, MON_RAM, MON_GPU, MON_VRAM,
        MON_CPUTEMP, MON_GPUTEMP, MON_DISK, MON_NET,
    };

    // Per-metric colour overrides, in MonMetric order. kMonColourFromTheme
    // means the metric has no override and follows the theme.
    COLORREF monColor[MON_METRIC_COUNT] = {
        kMonColourFromTheme, kMonColourFromTheme, kMonColourFromTheme,
        kMonColourFromTheme, kMonColourFromTheme, kMonColourFromTheme,
        kMonColourFromTheme, kMonColourFromTheme,
    };

    // ---- desktop clock ----
    // The monitor's sibling: same window, same chrome, its own styles and
    // themes. Stored by name like monitor_theme, for the same reason.
    bool  clockEnabled   = false;
    int   clockX         = INT_MIN;     // INT_MIN = never placed, pick a corner
    int   clockY         = INT_MIN;
    bool  clockPinned    = false;
    bool  clockOnDesktop = false;
    int   clockOpacity   = 92;          // percent
    int   clockScale     = 100;         // percent
    int   clockTheme     = 0;           // index into the ClockSkin table
    int   clockStyle     = 0;           // ClockStyle
    bool  clockHours24   = false;
    bool  clockSeconds   = false;
    bool  clockDate      = true;
    bool  clockWeekday   = true;

    // timer overlay. Shown / pinned / position work as the clock's; INT_MIN =
    // never placed. Tick: 0 off, 1 every second, 2 every minute while
    // something runs. Alarm: loop a sound when a timer ends.
    bool  timerShown     = false;
    bool  timerPinned    = false;
    int   timerX         = INT_MIN;
    int   timerY         = INT_MIN;
    int   timerTick      = 0;
    bool  timerAlarm     = true;

    // ---- File Explorer styling ----
    // Restyles File Explorer by loading a DLL into explorer.exe (see
    // explorerstyler.h). On by default. The theme is the name the styler mod
    // knows it by ("" = no theme, only the custom styles in
    // explorer-styler.ini); the effect is "" (the theme's own), blur,
    // acrylic, mica, micaalt or none.
    bool         explorerStyler = true;
    std::wstring explorerTheme  = L"ProWindows";
    std::wstring explorerEffect;
    // The Look page: read by the two ProWindows themes only. Tint is the
    // fill colour, its opacity counts for ProWindows Glass only; highlight is
    // 0 metal gradient, 1 accent colour, 2 tint; text is 0 white, 1 light grey.
    COLORREF     explorerTint        = RGB(0, 0, 0);
    int          explorerTintOpacity = 60;
    int          explorerHighlight   = 0;
    int          explorerRadius      = 4;
    int          explorerText        = 0;
    // Where the background effect reaches: 0 the whole window, 1 the frame
    // only. What to do about other XAML diagnostics users: 0 ask, 1 block,
    // 2 allow (block: a prompt inside Explorer is not something to answer).
    int          explorerRegion      = 0;
    int          explorerXamlDiag    = 1;

    // search bar
    // Which sources the search bar draws on besides installed apps. Files are
    // the only one that costs anything: the index is built once on a background
    // thread and held in memory, so it is also the only one with a size cap.
    bool searchFiles     = true;
    // Walk Program Files and friends for executables as well, so an application
    // with no Start-menu shortcut - a portable tool, a game unpacked into a
    // folder - is still one query away. Costs part of one background walk and
    // is cached with the rest of the index.
    bool searchPrograms  = true;
    // Look for programs on every fixed drive, not only the one Windows is on.
    // A second disk is where games and anything large actually live -
    // D:\SteamLibrary, D:\Games - and none of it has a Start-menu entry or
    // sits under Program Files, so without this it is invisible to the search
    // bar. The walk is the program walk, not the file walk: only .exe, and
    // only where an application's own executable plausibly sits.
    bool searchDrives    = true;
    // Include the executables an application ships to serve itself - helpers,
    // crash handlers, updaters, a toolchain's POSIX userland. Off by default
    // because they are numerous and none of them is ever what was meant, and
    // available because "show me every exe in that folder" is a real thing to
    // want when the one you need is a helper.
    bool searchDeepExe   = false;
    int  searchMaxPrograms = 6000;  // ceiling on executables, counted apart
    bool searchCommands  = true;
    bool searchCalc      = true;
    bool searchSettings  = true;
    bool searchHidden    = false;   // index hidden and system files too
    int  searchDepth     = 5;       // folder levels below each root
    int  searchMaxEntries = 20000;  // hard ceiling on indexed files
    // Roots to index. Empty means the shell's own Desktop, Documents,
    // Downloads, Pictures, Music and Videos - stored empty rather than expanded
    // so the defaults keep following the user if they move those folders.
    std::vector<std::wstring> searchFolders;

    // keys
    UINT modMask = MOD_ALT;                    // value of $mod
    // Claim chords the Windows shell has already reserved (Win+E, Win+F...) by
    // installing a low-level keyboard hook. Only installed if a binding needs it.
    bool  overrideReserved = true;
    std::vector<Keybind> binds;

    void LoadDefaults();
    bool LoadFromFile(const std::wstring& path);

    // Bumped when a release adds a default binding. The file lists every
    // binding explicitly (see MAP.md invariant 6), so without this a newly
    // introduced action would never reach anyone who already had a config.
    // 4 adds togglesplit, swapsplit, focuslast and the two relative-workspace
    // bindings. 6 adds win+w for the clock panel.
    static constexpr int kConfigVersion = 6;

    // Rewrites `path` from the current values, every binding included - a bind
    // added by hand survives because it was parsed into `binds` on load, not
    // because the line itself was copied across. Written to a temporary file
    // and moved into place, so an interrupted save cannot lose the config.
    bool SaveToFile(const std::wstring& path) const;

    static bool WriteDefaultFile(const std::wstring& path);
};

bool ParseKeySpec(const std::wstring& spec, UINT defaultMod, UINT* mods, UINT* vk);

// Turns an action name and its argument - "workspace" and "3" - into something
// the manager can run. Shared by the config file's `bind =` lines and by the
// control channel, so `ProWindows.exe --msg "workspace 3"` and
// `bind = alt+3, workspace, 3` cannot drift into meaning different things.
// That is the same arrangement i3 has with i3-msg, and the reason it works.
bool ParseAction(const std::wstring& name, const std::wstring& argText,
                 Action* act, int* arg, std::wstring* command);

// Exclusions that are always in force (shell surfaces, overlays, and windows
// that simply cannot be tiled sensibly). Kept apart from the user's own lists
// so the settings window shows only what the user added, and so saving the
// config never duplicates them.
const std::vector<std::wstring>& BuiltinIgnoreClass();
const std::vector<std::wstring>& BuiltinFloatClass();
const std::vector<std::wstring>& BuiltinIgnoreProcess();
const std::vector<std::wstring>& BuiltinFloatProcess();
const std::vector<std::wstring>& BuiltinFloatTitle();

} // namespace awa
