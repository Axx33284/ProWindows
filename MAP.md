# MAP.md — orientation for anyone (human or model) picking this project up

ProWindows is a tiling window manager for Windows 11: open windows and they arrange themselves,
Hyprland-style, driven from the keyboard. Around the tiler sit a search bar, a floating system
monitor, a desktop clock and a settings window laid out like the options screens of *Star Wars
Battlefront II*.
[README.md](README.md) is the user-facing description; this file is how it is put together and
what will bite you.

**Version 1.5.0.** Review notes for each pass live in [`docs/`](docs) — the latest is
[`docs/REVIEW-1.5.md`](docs/REVIEW-1.5.md).

## Contents

1. [The shape of it](#the-shape-of-it)
2. [Build](#build)
3. [Where things live](#where-things-live)
4. [How things flow](#how-things-flow)
5. [Threads](#threads)
6. [Invariants](#invariants) — grouped by subsystem; the numbers are stable IDs quoted in code and in `docs/`
7. [Things that surprised us](#things-that-surprised-us)
8. [Testing](#testing)

## The shape of it

A native C++17 / Win32 program with no dependencies beyond what ships with Windows. It never polls:
it subscribes to `SetWinEventHook` and only recomputes a layout when the OS says something changed.
Each monitor owns a set of workspaces; each workspace owns an ordered window list plus a binary
space partition tree. Keyboard shortcuts go through `RegisterHotKey`, falling back to a low-level
keyboard hook for the chords the shell refuses to give up. Everything is stored as a commented text
file at `%APPDATA%\ProWindows\config.ini`, which the settings window reads and rewrites. A named
pipe (`ProWindows.exe --msg "workspace 3"`, or `prowindowsctl.exe`) makes every action scriptable
and testable from outside the process.

The one rule that shapes everything else: **the UI thread never waits.** Anything that can block —
sampling hardware, walking the disk, asking the shell for an icon, talking to WMI — is on a thread
of its own, and anything cross-process on the UI thread has a timeout.

## Build

There is **no .vcxproj, no NuGet, no CMake**. `build.bat` locates `vcvars64.bat`, compiles every
source in one shot and writes `build\ProWindows.exe` and `build\prowindowsctl.exe`.

```bash
build.bat
```

- **Adding a source file needs registering** — the file list in `build.bat` is explicit, and so
  are the lists in the `tests\*.bat` harnesses that compile the UI.
- Statically linked (`/MT`), so the exe runs anywhere with nothing installed.
- **`/utf-8` is not optional.** The sources are UTF-8 *without a BOM*, LF line endings
  (`.gitattributes` says `-text`: git leaves them exactly as they are), and a few literals are not
  ASCII — the degree sign, the arrows on the network row and the search bar's prompt bar.
- **The exe must not be running when you build**; the linker cannot overwrite a locked file. Exit
  it from the tray, or build a second copy beside it: `build.bat PW_dev.exe`. Prefer exiting from
  the tray to `Stop-Process`: killing it skips `RestoreAllWindows`, so anything hidden for a
  workspace switch stays hidden (invariant 2).
- `res\gen_icon.py` regenerates `res\app.ico` from code (and drops `icon-32.png` / `icon-256.png`
  into `tests\shots` to look at). The mark is a tiled-window arrangement - master pane amber - on a
  near-black plate with its top-right corner cut off like the settings window's buttons, in
  `theme.h`'s palette; the settings header draws the same plate, so the tray, the taskbar and the
  window show the same object.

## Where things live

| File | Responsibility |
| --- | --- |
| `src\common.*` | Paths, logging, `Rect`, `Dir`, string helpers, private messages and timer ids, `kVersion`. |
| `src\config.*` | The `Config` struct, INI parsing, and the keybinding / action grammar. |
| `src\defaults.cpp` | Writes `config.ini` back out. The only place that serialises settings. |
| `src\winutil.*` | Win32 helpers: window classification (`Classify`, and *why* it said no), DWM-accurate placement, focus, process facts, app discovery. |
| `src\layout.*` | The BSP tree, the layout algorithms and the size-constraint solver. Pure geometry, no Win32 state — which is what makes `tests\run.bat` possible. |
| `src\wm.*` | Monitors, workspaces, event handling, animation, drag-to-rearrange, and every user action. |
| `src\hotkeys.*` | `RegisterHotKey` plus the `WH_KEYBOARD_LL` fallback, on a thread of its own. |
| `src\moddrag.*` | Hold the modifier and drag anywhere on a window. A `WH_MOUSE_LL` hook on a thread of its own, installed only while the modifier is held; it starts the *system's* move loop rather than running one. |
| `src\dragguide.*` | The translucent rectangle shown while a tiled window is dragged, marking where it would land. |
| `src\ipc.*` | The control channel: a named pipe, the command grammar and the replies. Commands are *posted* to the UI thread, never run on the pipe thread. |
| `src\ctl_main.cpp` | `prowindowsctl.exe`: the console front end to `ipc.*`, because a `/SUBSYSTEM:WINDOWS` program has no stdout to pipe. |
| `src\sysinfo.*` | CPU / RAM / GPU / disk / network sampling for the overlay. Called only from the sampling thread in `monitor.cpp`. |
| `src\thermal.*` | CPU and GPU temperature: the queue of sources Windows makes you try, on a thread of its own. |
| `src\montheme.*` | The overlay's colour schemes and layout styles. Tables of plain data, stored by name. |
| `src\monpaint.*` | The overlay's geometry and painting, one function per style. Knows nothing about the config. Also `PaintPanel`, which the clock borrows. |
| `src\monitor.*` | The overlay's window: creation, drag, pin, row reordering, z-order, context menu — and the thread that takes its readings. |
| `src\clocktheme.*` | The clock's colour schemes, typefaces and layouts. Tables of plain data; a skin names its face too. |
| `src\clockpaint.*` | The clock's twelve layouts. Each is one function that lays itself out and, when asked, paints — so the measure and the drawing cannot disagree. |
| `src\clock.*` | The clock window: the monitor's drag, pin, desktop mode, snap points and menu, with a timer that wakes on the next second or minute boundary. |
| `src\timer.*` | The clock panel: timers, stopwatch and alarms state, timers.ini, keys, commands, ringing and the one wake-up (inv. 88-91). |
| `src\clockpanel_paint.cpp`, `clockpanel.h` | The clock panel painted from a `Model` into a `View` of hit rects: rail, pages, sheets, ringing banner. |
| `src\alarm.*` | The alarm model: pure schedule maths over an injected time zone, summaries, the `a<i>.*` keys (inv. 91). |
| `src\launcher.*` | The search bar's window: query, ranking, painting, the row menu and the app catalogue scan. |
| `src\search.*` | What the search bar finds: the file and program index and its walk thread, the ms-settings table, the calculator, and the fuzzy scorer every source shares. No Win32 UI. |
| `src\appicon.*` | The shell icons the search bar draws, fetched on a thread of their own and cached in `icons.cache`. |
| `src\theme.*` | **The look**, modelled on Resident Evil Requiem's menus: pure black, grey hairlines, white type, Bahnschrift for headers and tabs, Segoe UI for content. A brushed-metal focus bar with bright edges and glow replaces the amber glow. Section plates with slants, segment indicators for choices, ruler sliders with markers, open-in icons for actions, and the painters for every row type (`DrawToggle`, `DrawSelector`, `DrawSlider`, `DrawAction`, `Keycap`/`Chord`, `Prompt`, `RowFocus`). No window, no state. The overlays are not themed by it. |
| `src\rowlist.*` | A list of settings rows (`ui::Row`: a kind, a label, a description, get/set onto the edit copy) and `ui::RowList`, which lays them out, paints them, owns the focus and its fade, scrolling, reordering, and every key and click on a row. Not a window: its host paints it and feeds it input. |
| `src\modal.*` | `ui::Confirm`, `ui::Ask`, `ui::Notice` and `ui::Pick`: modal screens in the same look, each a window of its own with its own message loop, the owner disabled and dimmed. |
| `src\settings.*` | The settings window: one custom-drawn window with its own frame - header, category column, the panel, the description and preview, footer buttons and prompts - plus the edit copy, Apply, Reset, search across every category, and shortcut capture. |
| `src\settings_pages.cpp` | The Layout, Behaviour and General categories, the colour row, and the layout preview. |
| `src\settings_keys.cpp` | The Shortcuts and Apps categories: every action's row, chords assigned in place, launchers. |
| `src\settings_search.cpp` | The Search category: which sources are on, and how the file index is built. |
| `src\settings_monitor.cpp` | The Monitor category: switches, look, readouts in order, colours, and the preview through the real painter. |
| `src\settings_clock.cpp` | The Clock category, and its preview through the real painter. |
| `src\settings_internal.h` | The contract every category follows: rows over `Edit()`, compared with `Saved()`; a `reset`; optionally a preview. |
| `src\app.h` | The handful of services the settings pages need from the shell — stubbed by the UI harnesses. |
| `src\main.cpp` | Entry point, tray menu, event hooks, idle trim, diagnostics, game mode and display-off plumbing, and the `app.h` implementations. |
| `res\app.rc` | Icon, manifest and version. There are no dialog templates: the settings window and its modal screens are laid out in code (invariant 80). |
| `res\gen_icon.py` | Regenerates `res\app.ico`. |
| `docs\` | The review notes: what was reported, what was found, what changed, one file per pass. Nothing in it is read by the build. |
| `tests\` | Harnesses; see [Testing](#testing). None is part of the product build. |

## How things flow

**A window opens, closes or moves:**

```
SetWinEventHook callback (main thread, out-of-context)
   → WindowManager::OnWinEvent      classifies, adds or drops the window
   → RequestRetile()                posts WM_AWA_RETILE on an idle desktop, else a 35 ms debounce
   → RetileNow()
        AnimBegin()                 clears the animation queue
        for each monitor: ComputeLayout() → ApplyPlacements()
        AnimCommit()                wakes the parked ticker thread
   → AnimStep() per WM_AWA_ANIMTICK - one per refresh of the fastest display - until
     t reaches 1, then lands exactly on the target rects
   → 320 ms later, a verification pass sees what each window did with its tile (21, 34)
```

**The overlay's readings** take their own path, and none of it is on the UI thread until the end:

```
monitor.cpp SampleThread (below normal priority, parked while the panel is hidden)
   → SystemSampler::Sample()        PDH, NtQuerySystemInformation, the thermal probe's last answer
   → g_freshLoad under g_loadLock, PostMessage(WM_AWA_SAMPLED)
   → MonitorProc (UI thread)        moves it into g_load; PushHistory, BeginEase, Redraw
```

**Settings** take one path, and deliberately only one:

```
open     → LoadEdit()                     Edit() and Saved() are both the live Config
a row    → set() writes into Edit()       every row is rebuilt from Edit() (79); Dirty()
                                          compares Edit() with Saved()
Apply    → MergeEdits(edit, saved, live)  only fields that differ from Saved() (78)
         → AppApplySettings()             SaveToFile, then ReloadConfig: hotkeys, rules,
                                          overlays, and SettingsRefresh
         → LoadEdit(), rows rebuilt       the window starts again from what came back
```

**A settings paint** — one bitmap, one blit, nothing that can come out in the system's colours:

```
WM_PAINT → a memory bitmap the size of the client
         → PaintBackdrop       rendered once at the monitor's size, so resizing shows more
                               or less of one picture instead of rendering one per frame
         → header, categories, the panel's fill and notched frame
         → RowList::Paint      rows, the lit row's glow, edge fades, the scrollbar - blended
                               in over 200 ms after a category change
         → description and preview, footer buttons, prompts, the unapplied-changes count
         → a veil over all of it while a modal screen is up
         → BitBlt
```

**A command from outside** (`prowindowsctl workspace 3`): the pipe thread parses it and posts
`WM_AWA_IPC` with a token; the UI thread runs it between passes and hands the reply back.

## Threads

Every one of these is a thread because the UI thread must not wait on what it does.

| Thread | Where | Priority | Notes |
| --- | --- | --- | --- |
| UI | `main.cpp` | normal | Message loop, every window, every retile. `SettingsTranslateMessage` then `IsDialogMessage` for the settings window. |
| Keyboard hook | `hotkeys.cpp` | highest | `WH_KEYBOARD_LL`; lookup and `PostMessage` only (3). Re-hooks once a minute (61). |
| Mouse hook | `moddrag.cpp` | highest | `WH_MOUSE_LL`, hook present only while the modifier is held (69). |
| Animation ticker | `wm.cpp` | normal | High-resolution waitable timer; parks between animations (19). |
| Overlay sampler | `monitor.cpp` | below normal | Parked while the panel is hidden (51). |
| Thermal probe | `thermal.cpp` | normal | Mostly asleep on its stop event; restartable and generation-checked (12, 41). |
| App catalogue scan | `launcher.cpp` | background mode | CPU *and* I/O priority lowered; abandonable at shutdown (12). |
| Icon loader | `appicon.cpp` | background mode while sweeping | Drops out of background mode to serve an urgent request (42). |
| File indexer | `search.cpp` | normal, then background mode | Reads `index.cache` at normal priority first; walks, if it must, in background mode (17, 55). |
| Pipe server | `ipc.cpp` | normal | Posts commands; never runs them. |

## Invariants

Break these and things get ugly. **The numbers are stable IDs**: code comments and older review
notes cite them ("MAP.md invariant 12"), so they are grouped here by subsystem rather than
renumbered, and a merged or retired rule keeps its number and says where it went. New rules are
appended at the end of the numbering.

### Window events and placement

1. **Never react to our own window moves — and never with a global flag.** A scope-guard
   `Suppressor` cannot work: the hook is `WINEVENT_OUTOFCONTEXT`, so the callback arrives long
   after any guard is gone, and `EndDeferWindowPos` pumps messages, so the flag swallowed real
   `EVENT_OBJECT_SHOW`s instead. Protection is per-window: `SetHidden` sets
   `ManagedWindow::hidden` **before** hiding, so the echo is recognised; a show for a window
   already in `managed_` is a no-op; placement raises no subscribed event
   (`EVENT_OBJECT_LOCATIONCHANGE` is not hooked, `MOVESIZESTART`/`END` come only from a user).
   Anything new that moves a window must say, in the event handler, how its echo is recognised.

5. **Place windows by their visible frame, not `GetWindowRect`.** Windows 11 windows carry an
   invisible resize border several pixels wide. `PlaceWindow` corrects for the difference between
   `GetWindowRect` and `DWMWA_EXTENDED_FRAME_BOUNDS`; skip it and every gap is subtly wrong.

13. **`ManagedWindow::monitor` is an index into a list that gets re-sorted.** `EnumMonitors`
    orders displays left to right, so rearranging them in Settings renumbers every monitor.
    `ReloadMonitors` builds an old → new index map by `HMONITOR` *before* replacing `monitors_`
    and remaps every window and `activeMonitor_` through it. Only windows whose display really
    vanished fall through to the salvage path.

21. **A window's own size limits are part of the layout, and are learned, not assumed.** Steam
    has a minimum and spilled over its neighbour; a copy dialog has a maximum and left most of its
    tile bare; an elevated window refused to move and left a hole. `ComputeLayout` takes a
    `ConsMap` of `SizeLimits`, and every algorithm honours it: a split moves off its ratio only as
    far as the limits demand, and space one window cannot use goes to its neighbours. The numbers
    come from `WM_GETMINMAXINFO` once (`SMTO_ABORTIFHUNG`, 60 ms) and are then corrected by
    `LearnFromLastPass`, which compares where each window was asked to go with where it is —
    applications enforce limits they never declare, so observation is the authority.
    - Escape hatches: a window that ignores placement twice is `immovable`; one that keeps using
      less than `kUsesEnough` of its tile is `tooSmall`; both drop out of `order` (a tile reserved
      for a window that will not fill it *is* the empty rectangle). A maximum across the axis a
      split does not run along cannot be met by geometry at all — test 9 in `layout_test.cpp`.
    - The adaptive pass runs at most twice per retile (`relayoutPending_`), or two windows with
      incompatible limits ping-pong forever.
    - *Something has to ask again*: `RetileNow` re-arms `TIMER_RETILE` when it left an attempt
      unjudged, and `ApplyPlacements` records an attempt only for a window it actually moves,
      which is what makes that terminate.
    - *A window still opening looks exactly like one enforcing a limit.* A limit is believed only
      once two consecutive looks agree (`lastSeen`); a false limit is sticky and distorts every
      later pass.
    - *Clamping is not sharing.* When the limits cannot all be met, `ConstrainedSplit` divides the
      slack in proportion to the windows on each side — but only on the path where the ratio has
      already proved impossible, so an ordinary board still splits exactly 50/50. Test 10.

22. **`IsElevatedProcess` is a question about integrity levels, not about elevation.**
    `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` succeeds across integrity levels, so it
    reported elevated windows as ordinary; they got tiled, refused every `SetWindowPos` silently,
    and left holes. It compares mandatory integrity levels. `tests\probe.bat` checks the rule
    against every process on the machine.

25. **What a window will accept is remembered between runs.** `Config::learnedLimits` keys
    `SizeLimits` by `"process|class"` — the granularity a limit belongs to — and `AddWindow` seeds
    a new window from it. `RememberLimits` marks the config dirty only when the numbers change;
    it is flushed once, in `Shutdown`.

26. **A minimum that does not fit is not a layout problem.** Two rules catch it before
    `ComputeLayout` sees it:
    - `tooLarge` is **permanent and remembered** — the window's minimum exceeds the monitor's
      usable area — but re-checked every pass against the *current* work area, so a bigger display
      brings it straight back.
    - `crowdedOut` is **recomputed every pass** — it fits alone but not beside everything else.
      `DropWindowsThatCannotFit` considers windows smallest-minimum first, so the one oversized
      application is what gets left out.
    Either way the window is positioned once, by `ParkAsFloating`, and then left alone.

27. **An elevated window is reported, not silently dropped.** `Classify` returns
    `ManageVerdict::Blocked` for a window at a higher integrity level, so the count shows in the
    tray tooltip and the settings status line (with a red mark). The fix offered is a logon task
    with `RunLevel=HighestAvailable` (`InstallElevatedAutostart`, via `schtasks /Create /XML`), not
    a manifest: `requireAdministrator` prompts at every launch and `uiAccess` needs a signed binary
    in a protected folder. `ExecutionTimeLimit` is `PT0S` — the default three days would kill a
    window manager.

28. **Process facts are cached per pid, for two seconds.** `Classify` asks for the image name and
    integrity level on every window event; `ProcEntry` caches them. Not forever: pids are recycled.

29. **A drag places a window on the side it was dropped on, and `Insert` cannot do that.**
    `BspTree::Insert` splits along the longer axis — right for a new window, wrong for one aimed
    at an edge. `MoveBeside(moving, target, side)` and `MoveToEdge(moving, side)` take the
    orientation from the direction. Both `Remove` first and look the target leaf up *after* —
    `Remove` frees the parent, so any `BspNode*` held across it is suspect. `PlaceBeside` updates
    `ws->tiled` too: Master and Grid lay out from that list, not the tree.

30. **The drop side is decided by the rectangle's diagonals, not by an edge zone.** `DropSide`
    cuts the target into four triangles meeting at its centre; ties go to the horizontal. A drop on
    no window (a gap, an empty workspace) applies the same function to the monitor's work area.

31. **The drop indicator and the drop itself are one function.** `PlanDrop` returns both the tree
    operation and the rectangle to highlight; `OnDragTick` and `OnMoveSizeEnd` both call it. The
    indicator (`dragguide.cpp`) is layered, `WS_EX_TRANSPARENT` and `WS_EX_NOACTIVATE` — the drag is
    a modal loop in the *other* process, and taking the mouse or the foreground would end it — and
    repaints only when its rectangle changes.

32. **`EVENT_SYSTEM_MOVESIZESTART` does not say whether a border or the title bar was grabbed.**
    `dragStartRect_` is recorded and the size compared: `OnDragTick` hides the indicator once the
    size moves, `dragIsResize_` latches, and `OnMoveSizeEnd` checks the final size itself because a
    quick drag can finish between ticks.

33. **A window is never written off on the strength of its first millisecond.** Electron, Chrome,
    Qt and JetBrains create the window and name, style and uncloak it afterwards. `Classify`
    reports **why** it refused (`IgnoreReason::Transient` / `Permanent`) and, since 1.4, **which
    rule** (a string literal, logged once per window by `ExplainSkip`); a transient refusal buys
    the window a place on `pending_` and a handful of second looks (`TIMER_PENDING`,
    `kPendingTries`). `EVENT_OBJECT_NAMECHANGE` is subscribed, `OnMoveSizeEnd` adopts a window it
    has never seen, and leaving game mode rescans. A new rejection rule must classify itself and
    name itself: wrong in the permanent direction brings the bug back.

34. **The verification pass must be able to stop.** A window that never holds still (a video, a
    loading app) never reports the same rect twice. `kSettleLooks` retires such a window and
    `kMaxVerifyChain` bounds how many passes the loop may request without an outside event; any
    real event resets the budget.

47. **A retile pass measures each window once.** `MeasureWindow` returns both the visible rect and
    the frame padding from one pair of DWM calls, and the plan is measured into a local vector
    before anything is decided. The one marked exception: un-maximising moves a window, so it is
    re-measured.

54. **A window whose limits are remembered is not asked for them again.** A window seeded from
    `learnedLimits` has `limitsAsked` set, so `ConstraintsFor` skips the 60 ms-ceiling message.
    Ten familiar windows at logon used to be over half a second of frozen UI.

56. **Win-event hooks go in before the first scan**, so a window that opens during it is not
    nobody's. `Classify` rejects a non-root window *before* testing visibility, or every child an
    application shows spends two seconds on `pending_`.

59. **Hiding and showing a managed window is asynchronous.** `SetHidden` uses `ShowWindowAsync`;
    `ShowWindow` waits on the other process, and a hung app froze the tiler. Because
    `IsWindowVisible` does not flip at once, the dead-window sweep excuses a window for
    `kShowGraceMs` after an un-hide (`shownAt`).

60. **"Immovable" and "too small" are verdicts with an expiry.** A hung application looks exactly
    like an immovable one. Immovable windows are asked again after `kImmovableRetryMs`, and a
    window the user moves or sizes by hand has both verdicts cleared.

### Safety and recovery

2. **Hidden windows must always be reachable again.** Workspace switching hides windows.
   `RestoreAllWindows()` runs on exit, on `WM_ENDSESSION`, from the crash handler, and from the
   tray's Tools → "Show all hidden windows". Anything that reduces the workspace count must first
   migrate windows off the workspaces being removed.

12. **A worker thread that misses its deadline is abandoned, never freed out from under — and it
    is asked to stop first.** The thermal probe, the app scan and the file indexer can all exceed
    their shutdown timeout. Each long walk checks a lock-free cancellation flag
    (`AbandonRequested()`, `LongScansCancelled()`); results are discarded under the lock
    (`g_abandon`, `g_abandonLoad`); critical sections are never deleted. The thermal probe, which
    can be restarted, owns a stop event per run and publishes only under its generation, so an
    abandoned run can be forgotten at once.

14. *Merged into 57.*

35. **The crash handler may not touch a container.** `EmergencyUnhideAll` reads a flat fixed
    array of `HWND` maintained by `SetHidden`, never `managed_`, which may be half-updated.

36. **`WM_QUERYENDSESSION` answers the question and does nothing else.** Another application may
    still veto. The teardown belongs in `WM_ENDSESSION` with `wParam` true; `Shutdown()` is
    idempotent anyway.

41. **A worker that can be abandoned must not release a slot it no longer owns.** `running_`
    holds the *id* of the run that owns the thermal probe's slot, and every release is a
    compare-exchange against it. Any other abandonable one-at-a-time worker needs the same shape.

57. **A fixed buffer is never filled with a `_s` function that cannot truncate.** (Was also 14.)
    `swprintf_s` and `wcscpy_s` call the invalid-parameter handler on overflow, and the default
    handler is a fast-fail: exception `0xC0000409`, no filter, nothing in the log, the tray icon
    simply gone. Launch commands, titles and paths are user text of any length: build those
    strings with `std::wstring`, or `_snwprintf_s` / `wcsncpy_s` with `_TRUNCATE`. `wWinMain`
    installs a handler that logs and returns — a net, not a licence. Ints and table lookups into a
    sized buffer are fine. (1.4 fixed one more: `DiagnosticsText`'s `add` printed a launch
    binding's command through `swprintf_s`.)

64. **A different executable that finds the mutex taken offers to replace what holds it.** Every
    version shares `kMutexName`, `kWndClass` and the config folder. `ReplaceRunningInstance`
    compares image paths: the same file behaves as before; a different one asks, posts `WM_CLOSE`
    to the hidden main window (the ordinary exit path, `RestoreAllWindows` included), waits, and
    takes the mutex; `RepairAutostartPath` moves the Run entry to the new copy.

### Input

3. **The low-level keyboard hook must return fast.** Windows silently drops a hook that exceeds
   `LowLevelHooksTimeout`. It does a lookup and a `PostMessage`; the work happens on
   `WM_AWA_HOOKKEY`.

4. **Only ignore our own synthetic keystrokes.** `MaskWinKey` stamps `dwExtraInfo` with
   `kSelfInjected` and the hook skips exactly those — not all `LLKHF_INJECTED` input, so macro
   keyboards, on-screen keyboards and test harnesses still work.

48. **Mod-drag starts the system's move loop; it does not run one.** `moddrag.cpp` posts
    `WM_NCLBUTTONDOWN` with `HTCAPTION`, so the tiler sees the same `MOVESIZESTART`/`END` pair as a
    real title-bar drag and there is one drag implementation. Load-bearing consequences: the
    button-up is never swallowed (the window would stick to the pointer); `ModDragBegin` checks the
    button is still down; the hook decides "swallow or not" from the published set of managed
    windows (`ModDragPublishWindows`), or it would eat mod+click on excluded apps too.

50. **Never take the other application's input state to give it focus.** `AttachThreadInput`
    pools keyboard state, capture, the caret and the cursor's show count — the whole of "the mouse
    disappears when I type" — and made `SetFocus` a synchronous call into a possibly hung process.
    `FocusWindow` sends one flagless mouse `SendInput` (nothing moves, nothing clicks), which makes
    this process the last input source, the documented condition for `SetForegroundWindow`.
    Anything that needs the foreground goes through `FocusWindow`; nothing may attach.

52. **The hook threads run at highest priority** (not time-critical). Every keystroke and pointer
    movement on the machine passes through them before its application sees it.

61. **The low-level hooks are re-installed once a minute.** Windows removes a hook that was late
    too often and tells nobody.

69. **The mouse hook is installed only while the mod-drag modifier is held.** A `WH_MOUSE_LL` hook
    sees every pointer movement — a thousand a second on a gaming mouse. The keyboard hook reports
    modifier transitions to `ModDragModifier`, which asks the idle mouse thread to hook or unhook.

### Fullscreen, power and visibility

23. **Nothing happens while a fullscreen application owns the screen.** Every action is a
    cross-process call that costs a game frames. `UpdateGameMode()` asks `FullscreenAppActive()`
    (the shell's `SHQueryUserNotificationState` first — the only view of exclusive fullscreen —
    then a measurement that rejects maximised and captioned windows); while yes: no tiling, no
    animation, no accent border, no focus-follows-mouse, no overlays. `Classify` ignores a
    borderless window covering a whole monitor. `TIMER_GAMECHK` runs only in game mode, because no
    events arrive to say it ended.

24. **Three things have an opinion about whether the overlays exist** — the user's setting, game
    mode, and display-off — and `UpdateOverlayVisibility()` in `main.cpp` is the only caller that
    shows or hides them.

58. **The shell's own full-screen surfaces are not games.** Alt+Tab, Task View, the lock and logon
    screens and the snipping overlay measure as fullscreen. Windows of `explorer.exe`, `LockApp`,
    `LogonUI`, the shell experience hosts and the snipping tools are excused by name; an
    `ApplicationFrameWindow` counts only when it hosts a `CoreWindow`; a window *we* made fullscreen
    is excused.

70. **"The display is off" is only believed after a stretch of no input.** The notification
    arrives at registration with the current value; while "off" is believed, `TIMER_DISPLAY`
    clears it on input. `get state` reports `displayOff`, `monitorShown`, `clockShown`.

### Configuration

6. **The config file is the whole truth about bindings.** `SaveToFile` writes `clear_binds = true`
   then every binding; there is no defaults-plus-overrides merge. The deliberate exception is
   `config_version`: `LoadFromFile` adds bindings introduced since the file was written, once, and
   only if the chord is free (`kAdded`, `onlyIfUnbound`). Add to `kAdded` and bump `kConfigVersion`
   together.

9. **Themes, styles and metric order are stored by name, not by index.** Adding or reordering
   entries in `kSkins` or `kStyles` must not change anybody's look, so the file holds `holonet`
   and `rings`, not `0` and `2`. 1.4 relied on this: it put **Holonet** and **Hologram** at the
   front of both skin tables, which makes Holonet the default for new configs and changes nothing
   for existing ones. 1.5 did it again with **Frontline**, the settings window's own look.

15. **Settings categories are addressed by index from outside the settings window.** `PageIndex`
    in `settings.h` names them, `g_pages` in `settings.cpp` is asserted to match, and
    `tests\uishot.cpp` asserts it covers every one.

49. **The overlay's readout order is a permutation, and the config file may lie about it.**
    `MonitorNormaliseOrder` repairs rather than rejects: unknown and duplicate names are dropped,
    missing ones appended. The Monitor category's readout rows are **positions**: row n shows
    `monOrder[n]`, carries `orderIndex = n`, and its id names the metric, so the focus follows a
    readout as it moves.

### The overlays: monitor and clock

8. **The monitor is painted once, from `MonDraw`.** The settings preview calls the same function
    through `MonitorDrawPreview` with a `MonPaintCtx` built from the edit copy, so it shows what
    Apply would do. Never draw a second, simplified version. The clock's preview is the same.

10. **An overlay frame is only drawn when it would look different, and its chrome is drawn once.**
    `BeginEase`/`StepEase` refuse glides and frames too small to see; `Redraw` compares a signature
    of the size, skin, eased values and the *text* of every reading; `MonDraw` caches the panel's
    shadow, gradient, gloss and border in a bitmap keyed by size, skin, opacity and radius. Rows
    went from 2.27 to 0.66 ms/frame (`tests\monshot.bat --bench`). Anything per-frame must stay out
    of `PaintPanelChrome`; anything that changes the chrome must join the cache key.

11. **The overlay stays a top-level window, even on the desktop.** Desktop mode makes the shell
    window its *owner* and drops it to `HWND_BOTTOM`. Reparenting into `WorkerW` breaks
    `UpdateLayeredWindow`. Explorer restarting destroys owned windows, hence `MonitorReattach()` on
    `TaskbarCreated`.

37. **The overlay's bitmap is bigger than the overlay.** `MonMeasure` adds `kShadow` on every side
    and `MonDraw` insets by it. Anything reasoning about where the panel *looks* goes through those
    two functions. The margin is transparent, so it costs nothing in clicks.

45. **The CPU bar is one segment per logical processor, and brightness is the level.** Past about
    thirty cores, a folded segment shows the **busiest** of its cores, not their mean.
    `SystemProcessorPerformanceInformation` is per processor group; the first call is sized from
    `GetActiveProcessorCount`, with one retry.

46. **A sensor block published by another process is bounds-checked before it is believed.** Core
    Temp and HWiNFO shared memory: signature, element size, count and every value are
    range-checked. `tempprobe.bat --selftest` publishes malformed blocks.

51. **Readings are taken on the sampling thread, never on the UI thread.** The GPU Engine PDH
    wildcard is unbounded (seconds at logon). `Sample()` and `Close()` belong to `SampleThread`;
    `Configure()` may be called from anywhere. The thread parks while the panel is hidden; `Redraw`
    draws nothing until the first reading lands.

62. **The clock draws the monitor's panel.** `PaintPanel` takes a `PanelLook`; one shadow, one
    gloss, one chrome cache (four entries). A radius larger than the panel is clamped to a circle;
    a radius of 0 is square, which is what Frontline, Holonet and Hologram use.

65. **NvAPI before NVML for the GPU temperature.** `nvmlInit_v2` commits 19.3 MB that
    `nvmlShutdown` never returns; `NvApiSensor` reads the same sensor for 2.8 MB and unloads. NVML
    is the fallback only when NvAPI could not be *opened*. `tests\gputemp.bat` measures all three.

### Clock panel (timer, alarms, stopwatch)

88. **Timers are anchored to the wall clock, never to tick counts.** Times are `int64` 100-ns
    units from `GetSystemTimeAsFileTime` (UTC). Remaining is `endUtc - nowUtc`, clamped at 0;
    stopwatch elapsed is `banked + now - startUtc`, clamped at 0 if the clock was set back. A
    tick count would stop during sleep and be wrong after a restart. `src\timer.*`.

89. **`timers.ini` is kept apart from `config.ini`, and written only on a state change.** It goes
    to `.new` and then `MoveFileExW`, as `Config::SaveToFile` does. Starting, pausing, lapping and
    time passing never write it; `config.ini` is rewritten whole and would churn on every one.

90. **Nothing wakes while idle.** With nothing running there is no timer at all. The panel is an
    overlay (`timer_shown`, `timer_pinned`, `timer_x/y`), shown without activation and hidden by
    game mode and display-off through `UpdateOverlayVisibility`; the alarm shows it the same way
    and never takes the keyboard. A visible panel
    aligns to the next whole second (100 ms while the stopwatch shows tenths). A hidden panel uses
    one `SetTimer` for `min(next deadline, 1 h)`, or 1 s only while the tick is on. The hour is
    not what catches a moved clock or zone (`WM_TIMECHANGE`, which the hidden top-level popup
    receives) nor sleep (resume and display-on call `TimerCheckNow`); it bounds the drift between
    SetTimer's tick clock and the NTP-slewed wall clock far below the 10 s "missed" threshold, so
    do not raise it to the deadline. `SetTimer` caps at about 24.8 days, so always clamp. No
    thread and no resident DIB while hidden (67).

91. **Alarms are local wall time, through an injected zone; timers stay UTC.** `src\alarm.*` is
    pure: `NextAfter` / `TakeDue` take `now` and an `alarm::Zone` (the app passes `SystemZone()`,
    tests pass fakes with a DST gap and a repeated hour). A gap time rings at the first minute
    after it, a repeated hour once. `lastFiredUtc` is also the baseline: saving, editing or
    re-enabling an alarm calls `Arm(now)`, so nothing rings for the past; `TakeDue` collapses
    every ring since the baseline into one report, and one due over 10 s ago is *missed*
    (balloon), not rung. A Once alarm disables itself after firing; a snooze still rings. Ring
    and ring-queue entries hold alarm indices, so `DeleteAlarm` renumbers them. Painting is
    `src\clockpanel_paint.cpp` (`clockpanel.h`: `Model`, `View`, hits); state, keys and waking
    stay in `src\timer.cpp`. Fonts are cached per DIP size and DPI and freed on hide.

### Explorer styling

92. **The styler runs inside explorer.exe and ProWindows never waits on it.** `src\explorerstyler.*`
    is the ProWindows side; the DLL (`src\explorer\`, built by `build_explorer.bat`) is a port of the
    Windhawk File Explorer Styler over a shim (`windhawk_shim.*`: ini settings, log, MinHook,
    WinHTTP). Every call that can block - walking processes, `VirtualAllocEx`, `CreateRemoteThread`
    plus the wait for it (up to 30 s: `StylerStart` talks to Explorer's windows) - is on the one
    worker thread; the UI thread only sets request bits and an event (`Request`). The foreground
    WinEvent calls `ExplorerStylerForeground`, which is a class-name compare and a set lookup.
93. **Stop on quit; the DLL is never unloaded.** `ExplorerStylerShutdown` runs first at exit, sets
    the DLL's named events (`Local\ProWindows.Styler.Stop.<pid>`, also `Reload`) and waits at most
    5 s for the worker, so an injection in flight is stopped by it (Sync's `g_quitting` branch)
    before the process goes. In Explorer the stop closes the events first (the process then reads
    as not styled), runs `Wh_ModUninit` and disables the hooks; the module stays **pinned** and
    MinHook stays initialised until Explorer exits, because hook bodies (`CreateWindowExW_Hook`
    waits through WM_CREATE), the TAP object XAML keeps, its delegates and the
    `RunFromWindowThread` hook proc are all code of the DLL. Styling again re-runs the mod's init
    in the same module (`MH_ERROR_ALREADY_CREATED` is success; `StylerStart` and the stop share a
    mutex). An event's existence is how "styled" is known, so a ProWindows started after a crashed
    one re-manages the old injection. The remote `StylerStart` is located by the module's full path
    and checked by link stamp; only x64 Explorer on x64 Windows is injected (`IsWow64Process2`).
94. **Crash guard.** `ExplorerStylerTaskbarCreated` counts Explorer restarts within 90 s of the last
    injection *attempt* (stamped before `InjectInto`: a crash inside `StylerStart` never reports
    Ok); the second one sets `explorer_styler = false`, saves, balloons and logs. A restart long after an injection only re-injects (delayed 1.5 s).
95. **The ini is shared with the user.** `explorer-styler.ini` has a managed block (theme, effect,
    `xamlDiagnosticsHandling=block`, `debug`) delimited by two marker lines at the top; ProWindows
    rewrites only that block, atomically, and the DLL reads the whole file, later lines winning.
    Anything under the block is the user's custom styles. The fields `explorer_styler`,
    `explorer_theme`, `explorer_effect` are in `AWA_EDITED_FIELDS` (78).
96. **No telemetry, and a build that may fail alone.** The mod's stats timer is deleted (the test
    greps for it). The DLL needs WinUI 3 headers generated at build time from the Windows App
    Runtime winmds in `C:\Windows\SystemApps\Microsoft.WindowsAppRuntime.CBS_*` (not the WinUI 2
    package) with the SDK's old cppwinrt, so the mod is patched in a few marked places
    (`// ProWindows:`) and `src\explorer\fix_winrt.ps1` qualifies `Windows::` in the generated
    headers. A failure in that step is a warning in `build.bat`, never an error. The DLL is
    locked while Explorer has it loaded; rebuild it with ProWindows closed (or styling off).

### The search bar

16. **Every source is capped before the merge.** `Refilter` takes at most `kMaxRows` from each of
    apps, programs, settings and files, ranks the pool, and trims. The calculator goes first
    (unranked), "run what I typed" last.

17. **The file index is a cache, not a scan.** `index.cache` is tied by `CacheSignature` to the
    folders, depth, ceiling and hidden-file setting; a walk happens only with no cache, a cache
    over a day old, or an explicit rebuild — under `THREAD_MODE_BACKGROUND_BEGIN`, after an
    interruptible 20 s hold-off on a cold start.

18. **Only `lowQuery` is pre-folded; candidate names are folded as they are scanned.**
    `SearchScore` takes a pointer and a length and scores the tail of a path in place; a keystroke
    across the whole index allocates nothing. Do not "simplify" it into two `std::wstring`s.

40. *Merged into 42.*

42. **The search bar never fetches an icon on the way to the screen, and after the first run it
    hardly fetches one at all.** (Was also 40 and 43.)
    - `AppIconFor` answers from memory or returns null and queues, reserving the slot at the
      moment it queues, so eight rows redrawn per keystroke queue each icon once.
    - Fetched icons go to `icons.cache` as raw premultiplied pixels and are read back in one
      sequential pass before anything is served; two weeks' expiry.
    - Everything in the catalogue the cache lacks is fetched in the background after a 15 s
      hold-off; urgent requests overtake the sweep.
    - `IShellItemImageFactory::GetImage` hands back *straight* alpha and `AlphaBlend` wants
      premultiplied: skip the correction and every icon gets a halo.

43. *Merged into 42.*

44. **Programs are a separate source from files, and capped separately.** `SearchPrograms` reads
    the same index (`FileEntry::exe`). `WalkPrograms` keeps only `.exe`, skips parts folders
    (`SkipProgramDirectory`), helper executables (`NoiseExecutable`) and two-letter binaries, and
    every hit is penalised by how deep it is buried.

55. **`index.cache` is read on the index thread, and `g_wants` is always recorded.** Parsing it on
    the UI thread cost 250 ms at startup; and because only `StartWalk` set `g_wants`, every Apply
    on any page used to start a full walk. `StartIndex` records it on every path.

68. **The search bar's icon bitmaps are not held for a hidden window.** `AppIconRelease` after
    three minutes out of sight: fresh ones written to `icons.cache`, every bitmap freed.

71. **Every icon bitmap is top-down, made so on the way in, and never asked which way up it is.**
    `GetObject` reports a positive height for every DIB section, so a handle cannot say how its
    rows are ordered. `TopDownCopy` copies the shell's bitmap through `GetDIBits` with a negative
    height and deletes the original; from then on `ReadPixels` copies rows in memory order.
    `tests\iconcache.bat` checks the round trip pixel for pixel.

### Settings window and the look

*Welcome* (`PAGE_WELCOME`, `settings_welcome.cpp`) is the first tab, appended to the enum and put first by `kTabOrder`; it has no reset (null `PageDef::reset`) and is skipped by the settings search.

7. *Retired in 1.5 — see 80.* `res\app.rc` owned the layout while the settings window was a
   dialog; there are no templates now.

20. *Retired in 1.5.* Every page had to call `theme::PrepareDialog` and `theme::DialogMessage`;
    there are no dialogs, no Win32 controls and nothing left to prepare.

38. *Retired in 1.5.* The disabled list view that painted itself white is gone with the list
    views; lists are rows (`Kind::Item`).

39. **The system's own chrome follows an undocumented opt-in.** `SetPreferredAppMode` (uxtheme
    ordinal 135) with **ForceDark**, not AllowDark, called once by `theme::Init`. Since 1.5 it only
    matters for what ProWindows does not draw itself: the tray's popup menu and the file, folder
    and colour dialogs. Best-effort by ordinal.

63. *Retired in 1.4 — superseded by 72.*

66. **The settings window is destroyed when hidden, not kept.** It is built from nothing in well
    under 100 ms when the tray icon is clicked. Unapplied edits are settled first: `Close` asks
    whether to apply or discard them (`ui::Ask`), and `SettingsHide` only ever runs after that.

67. **Memory is given back on idle, never on the way to doing something.** `TIMER_TRIM` fires with
    nothing on screen: `CoFreeUnusedLibrariesEx` with the *default* delay (zero races the thermal
    probe's MTA thread), `theme::TrimSurfaces()` (the backdrop bitmaps), `HeapCompact`, and
    `SetProcessWorkingSetSizeEx(-1, -1)`. Scheduled 45 s after start, a few seconds after the
    settings window or search bar is put away, and every fifteen minutes.

72. *Retired in 1.6 — the backdrop is gone.* The settings window and search bar now fill with flat
    black (`theme::Bg`) instead of a rendered picture. The screen is pure black, matching Resident
    Evil Requiem's menu design.

73. *Retired in 1.5 — see 80.* The category row was a window class of its own; categories are
    now part of the settings window, and Ctrl+Tab is handled by its own `WM_KEYDOWN`.
    `SettingsTranslateMessage` survives for the message loop and always answers false.

74. **A prompt is a promise: only show keys that really do the thing.** The footer's prompts come
    from `RowList::Prompts()` for the focused row, plus Ctrl+S (only while something is unapplied)
    and Esc (Back from the list, Close from the categories). Each maps to the key it names, and a
    click on one sends that key through the same `KeyDown`. The search bar's prompts are all bound.

75. *Retired in 1.5.* `PrepareDialog`, and its walk of a dialog's descendants, are gone.

76. *Retired in 1.5.* There is no page area to fit; the panel scrolls.

77. **Spaced text is measured unspaced, plus the spacing - and placed by hand.**
    `GetTextExtentPoint32` ignores `SetTextCharacterExtra`, and `DrawText` *centres and
    right-aligns by that unspaced width* while drawing the spaced one, so spaced text ran off the
    right of its box and lost its last letter (the search bar's `PROGRA`, the version's `V1.5.`).
    `theme::SpacedWidth` measures with the extra at 0 and adds `tracking × (length − 1)`;
    `DrawSpaced` lays centred and right-aligned spaced text out from that width and draws it
    left-aligned. Every width in the theme goes through these two.

78. **Apply writes only what was changed, and it is a three-way merge.** The window edits a copy
    (`Edit()`) and remembers what the live config was when the edit began (`Saved()`).
    `MergeEdits` copies a field into the live config only when the copy differs from `Saved()` -
    so the monitor dragged, pinned or re-skinned from its own menu while the window was open is
    not overwritten with the stale value from the copy. Every edited field is listed once, in
    `AWA_EDITED_FIELDS`; a setting added to a category and not to that list is silently never
    applied. `SettingsRefresh` from outside - the overlays call it after their own menus change
    something - reloads the copy when nothing is unapplied and otherwise **rebases** it (`Rebase`):
    fields the user has not touched take the live value, fields they have keep theirs, and
    `Saved()` moves on to the live config. 1.4 reloaded every page, which silently threw away
    every unapplied edit whenever the monitor or clock was changed from its own menu.

79. **Rows are rebuilt after every change, so a callback must not outlive its row.** A row is
    data - `std::function`s capturing pointers into `Edit()` - and `onChanged` rebuilds the whole
    list, which is what keeps descriptions and greyed-out rows true. Anything that runs a row's
    callback copies the `std::function` first and touches no `Row&` afterwards: the call may
    replace the vector it came from (`SetRows`), or open a modal screen whose message loop
    repaints and rebuilds underneath it. The focus, the scroll position and the lit fade survive
    a rebuild through the row's `id`, which is therefore stable and unique within a category.
    Search results prefix ids with the category number, and are not rebuilt on change - a row
    whose words changed would drop out from under the pointer.

80. **The settings window has no child windows, and its layout is code.** `DoLayout` places
    everything from the client size in `theme::Scale` units; `WM_DPICHANGED` resizes to the
    suggested rectangle and lays out again; `WM_GETMINMAXINFO` holds it at 980 × 620 DIPs. It
    draws its own frame: `WM_NCCALCSIZE` makes the whole window client, `WM_NCHITTEST` answers the
    resize edges and makes the header draggable where nothing in it is clickable, and a one-pixel
    DWM margin keeps the shadow. Corners are square (`DWMWCP_DONOTROUND`), matching Resident Evil
    Requiem's menu screens.

81. **A shortcut is recorded through a hook of its own, only while a row is listening.**
    `BeginCapture` installs a `WH_KEYBOARD_LL` hook on the UI thread that swallows every key while
    the settings window is in the foreground and posts it as `WM_AWA_CAPTUREKEY`, so the shell
    never sees `Win`+`E` and ProWindows' own hotkeys never fire. Only `Win` is swallowed among the
    modifiers (alone it opens Start). A bare key is refused unless nobody types with it (F1-F24,
    media keys); a bare Esc cancels; losing the foreground or a click elsewhere cancels. The hook
    is removed before the row's `set` runs, because that may open a modal screen (a taken chord
    asks first).

82. **A modal screen runs its own loop, and the owner is disabled for exactly that long.**
    `ui::Run` disables the owner, pumps until the answer, re-enables the owner *before* destroying
    the modal (so activation returns to it), then brings it forward. A `WM_QUIT` seen inside is
    re-posted. The settings window dims itself while `ui::ModalOpen()`, and does not treat the
    modal taking the foreground as a reason to cancel a capture.

83. **A binding without a key never reaches the config file.** A launcher is added with no chord
    and asks for one at once; if the capture is cancelled the row removes itself, and anything
    still keyless is dropped by `ApplyNow` before the merge.

84. **Hover follows the pointer only once the pointer has moved.** A window that opens under a
    stationary pointer is sent a mouse move; acting on it put the lit row wherever the pointer
    happened to be. `g_mouseAnchor` holds the position at open, as the search bar's does.

85. **Read what was pressed before releasing the capture.** `ReleaseCapture` sends
    `WM_CAPTURECHANGED` synchronously, and the settings window's handler for it clears
    `g_pressed`. `MouseUp` released first and read second, so every header and footer button -
    the X, minimise, Apply, Reset, Close - was dead to the mouse while the keyboard worked. Only
    `tests\clickprobe.bat`, which clicks with the real pointer, can catch this kind of thing.

86. **"The focused window" is the one in front, not the last one arranged.** `focused_` only ever
    names a managed window and is left alone when something unmanaged comes to the front, so with
    the settings window or an excluded app in front, Alt+Q closed whatever had focus before it.
    `ForeignForeground()` is that unmanaged window: close and minimise act on it, and every other
    action that changes a window (`ActionTarget()`) does nothing rather than touch one the user is
    not looking at.

87. **A config reload re-applies exclusions and nothing the user did at runtime.**
    `ApplyConfigChanged` releases managed windows the ignore rules now name (un-hiding them first,
    invariant 2) and rescans for ones they no longer name. It applies `tiling_enabled` only when
    the file's value changed (`cfgTiling_`), so a pause from the tray survives the next Apply.
    Reload and Restore every default throw unapplied edits away (`DiscardEdits`) before they run;
    the rebase of invariant 78 is for changes made elsewhere, not for "changes will be lost".

### Animation

19. **The animation ticker parks; it is not respawned.** It waits on `animActive_`; `AnimCommit`
    sets it, `AnimStop` resets it (and must stay cheap), `AnimShutdown` ends it.

53. **The animation ticks once per refresh of the fastest display** (`animFrameMs_`, 4..16 ms,
    from `MonitorInfo::refreshHz`). A frame the display cannot show is still a relayout in every
    application on the board.

## Things that surprised us

- **The shell owns almost every `Win`+letter chord.** `RegisterHotKey` was refused for all but
  `Win+J` and `Win+Y` on the test machine. That is why the keyboard hook exists.
- **Windows 11 cannot move its taskbar.** `StuckRects3` and `TaskbarSi` are ignored (build 26200).
  The Layout page's reserved margins are the supported way to make room for a bar.
- **Win32 has no usable dark mode.** 1.4 built one out of `WM_CTLCOLOR*`, owner-draw, subclassing
  and a window class of its own, and every control still had a corner that came out in the
  system's colours. 1.5 stopped fighting: the settings window has no controls at all.
- **`GetTextExtentPoint32` ignores `SetTextCharacterExtra`, and `DrawText` aligns by it.**
  Invariant 77.
- **`UpdateLayeredWindow` wants premultiplied alpha**; draw into a `PixelFormat32bppPARGB` GDI+
  bitmap, not through the HDC.
- **GDI+ headers need bare `min`/`max`**, which `NOMINMAX` removes: `#include <algorithm>` and
  `using std::min/max` before `gdiplus.h`. The SDK's own GDI+ headers then produce a page of C4458
  warnings under `/W4`; they are noise, filter them.
- **`SetWindowPos` on a window above our integrity level returns success.** UIPI drops it silently,
  which is why placement is verified by looking (21) and such windows are `Blocked` (27).
- **`small` is a `typedef` for `char`** in the RPC headers `windows.h` drags in. A local named
  `small` produces "'SizeLimits' followed by 'char' is illegal".
- **A suspended UWP app is DWM-cloaked, not hidden**; it comes back via `EVENT_OBJECT_UNCLOAKED`.
- **`CoCreateInstance(CLSID_VirtualDesktopManager)` fails at logon** while the shell is starting;
  `IsOnCurrentVirtualDesktop` retries every five seconds. And a kept instance can go stale and
  say "elsewhere" about a window DWM is plainly drawing, so that answer is re-asked of a fresh one
  before it is believed.
- **`FindWindow(L"Progman", nullptr)` can return null** on this build; ask `GetShellWindow()` first.
- **Per-process CPU, memory and I/O come from one `NtQuerySystemInformation` call**, with the full
  `SYSTEM_PROCESS_INFORMATION` declared locally — `winternl.h` hides the fields in `Reserved1`.
- **`MSAcpi_ThermalZoneTemperature` needs administrator**, and fails as an empty result rather than
  an error. The same zone is the `Thermal Zone Information` performance counter set, readable by
  anyone. **The ACPI zone is not the CPU package** either; the real sensor needs a kernel driver,
  so `thermal.cpp` reads LibreHardwareMonitor's (or OpenHardwareMonitor's) WMI namespace when one
  is running, and the overlay says which source answered.
- **GPU temperature, unlike CPU, is exact and unprivileged** — NvAPI / NVML for NVIDIA, ADL for
  AMD, all loaded by name at run time (65).
- **`SetTimer` cannot animate.** Its floor is the ~15.6 ms tick whatever you ask for; the ticker is
  a thread on a `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer, with one tick in flight at most.
- **A tray process has no foreground rights.** `SetForegroundWindow` on the search bar leaves it
  drawn but not typed into; use `FocusWindow` (the `SendInput` rule, invariant 50).
- **A memory-DC buffer must blit before `EndPaint`.** Every painter here blits explicitly, then
  calls `EndPaint`.
- **The overlays build their own fonts.** Borrowing `theme::FontUI()` looked tidy, but those are
  fixed-size and the Size slider stretched the panel around unchanging numbers.
- **Every `testwin.exe` shares one window class**, and learned limits are keyed by process and
  class — so the first test window to teach a minimum teaches it for all of them. Use Notepad for
  the unconstrained window in a mixed board, and strip `learned = testwin.exe` between sessions.
- **The shell's icons are bottom-up DIBs, and a DIB section's handle will not tell you.** The
  first cache inverted every icon on the second launch; see invariant 71 for the rule that ended it.
- **A layered window is click-through wherever its alpha is zero.** The bare skins paint one count
  of alpha over the panel rectangle to stay grabbable.
- **A press on the overlay is two gestures until time tells them apart**: press-and-go moves the
  panel, press-and-hold (320 ms, four pixels) lifts a readout, Ctrl lifts at once.
- **`ReleaseCapture()` tells you about itself** — `WM_CAPTURECHANGED` arrives at the window that
  asked.
- **A shortcut cannot be recorded from `WM_KEYDOWN`.** `Win+E` never reaches any window and our own
  hotkeys fire instead; the settings window installs its own `WH_KEYBOARD_LL` hook while a row is
  listening (81).
- **`SetKeyboardState` is how a harness holds Ctrl.** `GetKeyState` answers from the thread's key
  state, so a message-driven Ctrl+Tab or Ctrl+S needs the state set as well as the message sent.
- **Windows PowerShell 5.1's `Set-Content -Encoding utf8` writes a BOM.** The sources are UTF-8
  *without* one; write through `[IO.File]::WriteAllText` with `UTF8Encoding($false)` when scripting
  edits.

## Testing

Nothing here starts the tiler unless it says so — the only other way to look at a change is to run
the real thing, and the real thing rearranges every window on the desktop without remembering where
they were.

| Harness | What it does |
| --- | --- |
| `tests\run.bat` | Compiles `layout.cpp` with a console harness and asserts on the rectangles: **212 assertions** covering every layout, both kinds of size limit, gaps, and boards whose limits cannot all be met. Safe at any time; the place for anything geometric. |
| `tests\probe.bat` | Prints how every window on screen would be classified, its declared limits, and whether it is out of reach. Read-only; the fastest answer to "why is this window not arranged?". |
| `tests\uishot.bat` | Opens the settings window with `app.h` stubbed, captures every category to `tests\shots\ui-*.png`, then drives it by messages sent to the window - nothing pressed on the desktop - and checks: only the row with the keyboard is lit amber (pixels read back), a changed switch is an unapplied edit, Ctrl+S applies it to the live config and leaves nothing unapplied, Right steps a slider, a chord posted as the capture hook would lands on the shortcut; and captures the categories column, Ctrl+Tab, a capture in progress, search results, a modal screen and the window at its smallest. Exit code 1 on any failure. The About text and indexed folders are fixed so captures carry no profile paths. |
| `tests\clickprobe.bat` | Opens the settings window with `app.h` stubbed and **clicks with the real pointer and keyboard** (it takes the mouse for a few seconds): the X, minimise, footer Close, Esc, Alt+F4, the close and minimise shortcuts with the window in front, Reset to defaults answered on its modal screen, and Discard on close. The only harness that goes through hit-testing, capture and activation (85). |
| `tests\launchshot.bat` | Stands the search bar up alone, types into it, waits for icons, captures `launcher-*.png`. |
| `tests\monshot.bat` | Draws the overlay through its own painter, one PNG per style and per skin, over a checkerboard. `--bench` times the painter per style in ms/frame. |
| `tests\clockshot.bat` | The same for the clock: every style (12- and 24-hour) and every skin. |
| `tests\styler_test.bat` | The styler's shim and ProWindows' side of it, in this process: the theme table (every theme selectable by the mod, the ProWindows theme's colours, no stats timer), the ini round trip that keeps your own lines, settings and indexed keys as the mod reads them, a MinHook hook on a dummy function installed, reloaded and removed. Nothing is injected; Explorer is never touched. |
| `tests\clocklive.bat` | Runs the real clock window for a few seconds and captures it off the screen. |
| `tests\searchprobe.bat` | Runs the file and program index alone and prints what it found, per drive. |
| `tests\iconcache.bat` | Checks a shell icon comes back from `icons.cache` the right way up, through a save, a release and a reload, twice. Run after touching `appicon.cpp`. |
| `tests\tempprobe.bat` | Which temperature source this machine answers from; `--selftest` publishes synthetic (and malformed) Core Temp and HWiNFO blocks. |
| `tests\gputemp.bat` | What each GPU-temperature source costs in private memory here (65). |
| `tests\analyze.bat` | MSVC `/analyze` over the named sources (or all of them), compile only. |
| `tests\ctl_test.ps1` | **Starts a real tiler** (`--tray --config <temp folder>`, so your settings are untouched) and drives it through the control channel: workspace switches really hide windows, layout changes take, bad commands are rejected. The sandbox config is fresh, so it **arranges the live desktop's windows** — run it where that is acceptable. It refuses to start while ProWindows is running. |

Judging the PNGs: zoom in before believing them — the core-load strip was written off as "a dashed
line" from a downscaled view and was drawing exactly the right thing. **`PrintWindow` does not
always reproduce a window's client area**: after a synthetic mouse move it returned the category row
and the footer blank while `GetPixel` on the window showed both intact, and the old tab strip came
back blank from every capture. `uishot`'s `Capture` therefore takes only the frame from
`PrintWindow` and the client straight from the window's own surface (`GetDC` + `BitBlt`), which is
what DWM composites. The theme's painters also answer `WM_PRINTCLIENT`. Confirm a missing control
before believing it.

`tests\testwin.cpp` is a window that misbehaves on request: `--min 900x500`, `--max 420x260`, and
the three ways a real application arrives half-built (invariant 33):

```
testwin.exe --late-title 400        no name until 400 ms in, like Electron and Chrome
testwin.exe --late-show 400         created hidden, shown later
testwin.exe --late-resizable 400    WS_THICKFRAME applied after creation
```

Run two or three at once; they should take their tiles on their own. It is the regression check
worth running after anything that touches `Classify` or `OnWinEvent`.

With `debug = true`, `%APPDATA%\ProWindows\log.txt` records every decision: why each window was or
was not managed (the rule's name, since 1.4), and on each retile the BSP tree
(`BspTree::Describe`, leaf minimums included) with the rect planned for each window — which turns
"the layout looks wrong" into arithmetic.

Other techniques that have earned their keep:

- Drive the settings UI with `WM_KEYDOWN` / `WM_CHAR` sent to the window (and `SetKeyboardState`
  for Ctrl), then assert on `Edit()`, `Saved()` or `config.ini` — the real Apply → reload path.
  `WM_APP + 120` is what the capture hook posts: `(vk, mods)`, or `(0, mods)` while modifiers are
  held.
- Assert on geometry, not screenshots: enumerate windows, read `DWMWA_EXTENDED_FRAME_BOUNDS`, check
  for gaps and overlaps.
- Sample window rects in a tight loop during a layout change to prove the animation interpolates.

When testing on a live desktop, put every already-running app in `ignore_process` first.
