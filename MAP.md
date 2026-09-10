# MAP.md — orientation for anyone (human or model) picking this project up

ProWindows is a tiling window manager for Windows 11: open windows and they arrange
themselves, Hyprland-style, driven from the keyboard. See [README.md](README.md) for the
user-facing description. This file covers how it is put together and what will bite you.

## The shape of it in one paragraph

A native C++ / Win32 app with no dependencies beyond what ships with Windows. It never polls: it
subscribes to `SetWinEventHook` and only recomputes a layout when the OS says something changed.
Each monitor owns a set of workspaces; each workspace owns an ordered window list plus a binary
space partition tree. Keyboard shortcuts go through `RegisterHotKey`, falling back to a low-level
keyboard hook for the chords the shell refuses to give up. Everything is stored as a commented text
file at `%APPDATA%\ProWindows\config.ini`, which the settings window reads and rewrites.

## Build

There is **no .vcxproj, no NuGet, no CMake**. `build.bat` locates `vcvars64.bat`, compiles every
source in one shot and writes `build\ProWindows.exe`.

```bash
build.bat
```

- Adding a source file **does** need registering — the file list in `build.bat` is explicit.
  The screenshot harnesses in `tests\` have their own file lists for the same reason.
- Statically linked (`/MT`) so the exe runs anywhere with nothing installed.
- `/utf-8` is not optional. The sources are UTF-8 without a BOM and a few literals are not ASCII —
  the degree sign on the temperature rows, the arrows on the network row. Without the flag MSVC
  reads them in the machine's ANSI code page and those characters reach the screen as mojibake.
- The exe must not be running when you build; the linker cannot overwrite a locked file. Either
  exit it from the tray first, or pass an output name — `build.bat PW_dev.exe` — and build a
  second copy beside the running one. Prefer exiting from the tray to `Stop-Process`: killing it
  skips `RestoreAllWindows`, so anything hidden for a workspace switch stays hidden and the user
  has no way to get it back.
- `res\gen_icon.py` regenerates `res\app.ico` from code; it is only needed if the icon changes.

## Where things live

| File | Responsibility |
| --- | --- |
| `src\common.*` | Paths, logging, `Rect`, `Dir`, shared string helpers. |
| `src\config.*` | The `Config` struct, INI parsing, and the keybinding/action grammar. |
| `src\defaults.cpp` | Writes `config.ini` back out. The only place that serialises settings. |
| `src\winutil.*` | Win32 helpers: window classification, DWM-accurate placement, app discovery. |
| `src\layout.*` | The BSP tree and the four layout algorithms, and the size-constraint solver. Pure geometry, no Win32 state - which is what makes `tests\` possible. |
| `src\wm.*` | Monitors, workspaces, event handling, animation, and every user action. |
| `src\hotkeys.*` | `RegisterHotKey` plus the `WH_KEYBOARD_LL` fallback. |
| `src\moddrag.*` | Hold the modifier and drag anywhere on a window. A `WH_MOUSE_LL` hook on a thread of its own; it starts the *system's* move loop rather than running one. |
| `src\sysinfo.*` | CPU / RAM / GPU / disk / network sampling for the overlay. |
| `src\thermal.*` | CPU and GPU temperature: the queue of sources Windows makes you try, on a thread of its own. |
| `src\launcher.*` | The search bar's window: query, ranking, painting and the row menu. |
| `src\appicon.*` | The shell icons the search bar draws, fetched on a thread of their own. |
| `src\search.*` | What it finds. The file index and its walk thread, the ms-settings table, the calculator, and the fuzzy scorer every source shares. No Win32 UI. |
| `src\montheme.*` | The overlay's colour schemes and layout styles. Tables of plain data. |
| `src\monpaint.*` | The overlay's geometry and painting. Knows nothing about the config. |
| `src\monitor.*` | The overlay's window: creation, drag, pin, z-order, context menu. |
| `src\dragguide.*` | The translucent rectangle shown while a tiled window is dragged, marking where it would land. |
| `src\theme.*` | Dark palette and the custom drawing that makes Win32 controls look like it. |
| `src\settings.*` | Settings shell (header, tabs, Apply) plus the Layout, Behaviour and General pages. |
| `src\settings_keys.cpp` | Window keys, Open apps and Monitor pages, and the key-capture editor. |
| `src\settings_search.cpp` | The Search page: which sources are on, and how the file index is built. |
| `src\settings_internal.h` | The contract every tab page follows: `Load()` in, `Save()` out. |
| `src\app.h` | The handful of services the settings pages need from the shell. |
| `src\main.cpp` | Entry point, tray UI, event hooks, and the `app.h` implementations. |
| `res\app.rc` | Icon, manifest, and every dialog template. The layout lives here, not in code. |
| `tests\` | `run.bat` asserts on layout geometry; `probe.bat` prints how each live window would be classified without moving any of it; `tempprobe.bat` prints which temperature source this machine can answer from, and self-tests the two shared-memory readers; `monshot.bat`, `launchshot.bat` and `uishot.bat` render the three pieces of UI to PNG without starting the tiler; `searchprobe.bat` runs the file and program index alone and prints what it found, per drive, which is the only way to see whether the walk reaches this machine's other disks; `analyze.bat` runs MSVC `/analyze` over whichever sources you name. None is part of the product build. |

## Data flow

```
a window opens / closes / moves
   → SetWinEventHook callback (main thread, out-of-context)
   → WindowManager::OnWinEvent      classifies, adds or drops the window
   → RequestRetile()                35 ms debounce timer, so a burst collapses into one pass
   → RetileNow()
        AnimBegin()                 clears the animation queue
        for each monitor: ComputeLayout() → ApplyPlacements()
        AnimCommit()                starts the 8 ms animation timer
   → AnimStep() until t reaches 1, then lands exactly on the target rects
```

Settings take a different path, and deliberately only one:

```
Apply → each page's Save() writes into the live Config
      → Config::SaveToFile()        the file now matches the UI exactly
      → ReloadConfig()              re-reads it, re-registers hotkeys, re-applies rules
      → each page's Load()          controls re-read the canonical values
```

## Invariants — break these and things get ugly

1. **Never react to our own window moves — and never do it with a global flag.** There used to be
   a `WindowManager::Suppressor` that set `suppress_` around every call that moved a window, on the
   theory that `OnWinEvent` would then ignore the echo. It could not work: the hook is installed
   `WINEVENT_OUTOFCONTEXT`, so the callback is delivered asynchronously, long after any scope guard
   has gone. It never suppressed what it was aimed at, and it *did* suppress real events —
   `EndDeferWindowPos` pumps messages, and it runs on every animation frame, so a window that
   opened during an animation had its `EVENT_OBJECT_SHOW` thrown away.

   The protection is per-window instead, and each case carries its own: `SetHidden` sets
   `ManagedWindow::hidden` **before** calling `ShowWindow`, so the echo of our own hide is
   recognised by that flag; a show for a window already in `managed_` is a no-op however it was
   caused. Placement raises no subscribed event at all — `EVENT_OBJECT_LOCATIONCHANGE` is not
   hooked, and `MOVESIZESTART`/`END` come only from a user dragging. Anything new that moves a
   window must say, in the event handler, how its own echo is recognised.

2. **Hidden windows must always be reachable again.** Workspace switching hides windows with
   `SW_HIDE`. If the app dies with windows hidden, they are gone from the user's point of view.
   Hence: `RestoreAllWindows()` runs on exit, on `WM_ENDSESSION`, from the crash handler, and from
   the tray's "Show all hidden windows". Anything that reduces the workspace count must first
   migrate windows off the workspaces being removed.

3. **The low-level keyboard hook must return fast.** It runs on the UI thread and Windows silently
   drops a hook that exceeds `LowLevelHooksTimeout`. It does a lookup and a `PostMessage`, nothing
   else. The actual work happens later on `WM_AWA_HOOKKEY`.

4. **Only ignore our own synthetic keystrokes.** `MaskWinKey` stamps `dwExtraInfo` with
   `kSelfInjected` and the hook skips exactly those. It deliberately does *not* ignore all
   `LLKHF_INJECTED` input, so macro keyboards, on-screen keyboards and test harnesses still work.

5. **Place windows by their visible frame, not `GetWindowRect`.** Windows 11 windows carry an
   invisible resize border several pixels wide. `PlaceWindow` corrects for the difference between
   `GetWindowRect` and `DWMWA_EXTENDED_FRAME_BOUNDS`; skip that and every gap is subtly wrong.

6. **The config file is the whole truth about bindings.** `SaveToFile` writes `clear_binds = true`
   followed by every binding. There is no "defaults plus overrides" merge to get out of step.

   The one deliberate exception is `config_version`. Because the file is authoritative, a binding
   added in a new release would otherwise never reach anybody who already had a config — the
   launcher shipped unreachable until this was noticed. `LoadFromFile` therefore adds newly
   introduced default bindings once, only when the file predates them, and only if the chord is
   still free. The next save writes the result out explicitly like everything else. Add to
   `kAdded` and bump `kConfigVersion` together.

   `kAdded` entries carry an `onlyIfUnbound` flag. `true` is the normal case: do not impose a
   second key on an action that already has one. `false` is for a chord that is worth having
   *as well* — `win+s` for the search bar, which most existing configs already reach on `$mod+r`.
   Either way a chord already spoken for is never stolen.

7. **`res\app.rc` owns the layout.** Pages read control positions from the template; the theme even
   turns each `GROUPBOX` into a painted card using that control's rectangle. Move things in the
   `.rc`, not in code.

8. **The monitor is painted once, from `MonDraw`.** The settings page's preview calls the same
   function through `MonitorDrawPreview`, with a `MonPaintCtx` built from the page's controls
   instead of from the live config. Never draw a second, simplified version of the panel: it
   will drift from the real one within a release.

9. **`monitor_theme` and `monitor_style` are stored by name, not by index.** Adding or reordering
   entries in `kSkins` or `kStyles` must not silently change somebody's look, so the file holds
   `nord` and `rings`, not `2` and `2`.

10. **An overlay frame is only drawn when it would look different, and its background is drawn
    once.** Three guards, at three different levels, and they are cumulative:

    - `BeginEase` refuses to start a glide unless something moved by `kEaseWorthStarting`, and
      `StepEase` returns false for any frame within `kEaseWorthDrawing` of what is on screen.
    - `Redraw` compares a signature of everything the frame would contain — the size, the skin,
      the eased percentages, and the *text* of every reading — against the last frame, and returns
      without touching `UpdateLayeredWindow` when they match. The text matters: a reading can go
      from "9.7 GB" to "9.8 GB" without moving a bar by a pixel, and that still has to be drawn.
    - `MonDraw` keeps the panel's chrome — shadow, gradient, gloss, border — in a bitmap keyed by
      size, skin, opacity and radius, and blits it. It is identical every frame, and painting it
      inline made the shadow alone two thirds of the cost of a frame: nine stacked antialiased
      rounded rectangles the size of the whole panel, thirty times a second.

    Measured with `tests\monshot.bat --bench`, eight metrics with the busiest-app line on: Rows
    2.27 ms/frame before the cache, 0.66 ms after — and the *after* number includes a drop shadow
    the *before* number did not have. Keep all three if you touch that code.

    Note what the chrome cache implies: anything that varies per frame must not be drawn inside
    `PaintPanelChrome`, and anything added to `MonPaintCtx` that changes the chrome must be added
    to the cache key as well.

11. **The overlay stays a top-level window, even on the desktop.** Desktop mode works by making the
    shell window its *owner* and dropping it to `HWND_BOTTOM`; an owned window is always above its
    owner, so it lands exactly one step above the wallpaper. The obvious alternative — reparenting
    into the `WorkerW` behind the desktop icons — breaks `UpdateLayeredWindow`, which only accepts
    top-level windows, and the panel would lose the per-pixel alpha that makes it look like glass.
    Note that Explorer restarting destroys the shell window and every window it owns, which is why
    `MonitorReattach()` hangs off the `TaskbarCreated` message.

12. **A worker thread that misses its deadline is abandoned, never freed out from under — and it
    is asked to stop first.** The thermal probe (`thermal.cpp`), the launcher's app scan
    (`launcher.cpp`) and the file indexer (`search.cpp`) are all waited on with a timeout, and all
    three can genuinely exceed it — WMI parks inside `Next()`, enumerating `shell:AppsFolder` on a
    machine full of Store apps is slow, and a deep tree on a cold disk is slower still. Critical
    sections are never `DeleteCriticalSection`'d; that costs three sections for the life of the
    process and removes the whole class of bug.

    The wait is a fallback, not the plan. Each of the long walks checks a cancellation flag it can
    read without taking a lock — `AbandonRequested()` in the indexer, `LongScansCancelled()` in
    `winutil.cpp` for the app scan — so shutdown normally completes well inside the timeout rather
    than leaving a live thread to be terminated by `ExitProcess`, possibly holding the CRT heap
    lock. Results are still discarded under the lock (`g_abandon`, `g_abandonLoad`) so a late
    finisher never writes to globals the process is tearing down.

    The thermal probe goes one step further, because it can be restarted. Each run owns its own
    stop event and closes it on the way out, and publishes only under the generation it started
    with, so an abandoned probe can be **forgotten immediately**. Leaving its handles in place made
    the next `Start()` decide a probe was already running and return without starting one — which
    is how switching a temperature off and on again could leave the readings dead for the session.

13. **`ManagedWindow::monitor` is an index into a list that gets re-sorted.** `EnumMonitors` orders
    displays left-to-right, so rearranging them in Settings renumbers every monitor. `ReloadMonitors`
    therefore builds an old-index → new-index map by `HMONITOR` *before* replacing `monitors_`, and
    remaps every window and `activeMonitor_` through it. Workspaces travel with the handle, so a
    remapped index still points at the same `Workspace` the window is listed in. Only windows whose
    display actually vanished fall through to the salvage path.

14. **User text never goes into a fixed buffer via the `_s` printf family.** `swprintf_s` and
    `wcscpy_s` invoke the invalid-parameter handler on overflow, and no handler is installed, so
    the default one terminates the process. A `launch` command is arbitrary user text and is long
    in practice. Build those strings with `std::wstring` concatenation, or use `_TRUNCATE`
    (`LogLine` and `TrayBalloon` show both). Ints and table lookups are fine as they are.

15. **Settings tabs are addressed by index from outside the settings window.** `PageIndex` in
    `settings.h` names them, `g_pages` in `settings.cpp` is asserted to match, and both
    `LoadAllPages` and `ApplyNow` list every page explicitly. Inserting the Search tab in the
    middle silently renumbered Monitor once already, which is why the numbers are gone.

16. **Every source the search bar draws on is capped before the merge.** `Refilter` takes at most
    `kMaxRows` from each of apps, settings pages and files, ranks the pool, and only then trims to
    the rows on screen. Without the per-source cap the file index - tens of thousands of entries
    against a handful of apps - would fill all eight rows on any short query. The calculator is
    inserted at the front rather than ranked (if what you typed is a sum, that is the answer), and
    "run what I typed" is appended at the back (it is a fallback, not a match).

17. **The file index is a cache, not a scan.** Walking is the expensive part and its result
    barely differs between launches, so `search.cpp` writes `index.cache` and loads that instead.
    `CacheSignature` ties a cache to the folders, depth, ceiling and hidden-file setting it was
    built under — change any of them and the cache is correctly ignored. A walk only happens with
    no cache, a cache over a day old, or an explicit rebuild. The walk itself runs under
    `THREAD_MODE_BACKGROUND_BEGIN` (lowered **I/O** priority, not just CPU) and, on a cold start,
    holds off for twenty seconds on an interruptible wait so it is not competing with every other
    login program for the disk. Keep all three if you touch that code: they are what makes this
    usable on a mechanical disk.

18. **Only `lowQuery` is pre-folded; candidate names are folded as they are scanned.** `SearchScore`
    takes a pointer and a length and calls `LowerFast` per character, so the file index stores one
    string per entry rather than a name plus a lower-cased copy of it. The pointer form exists so
    the index can score the tail of a path in place - a keystroke across the whole index allocates
    nothing at all. Do not "simplify" this back into taking two `std::wstring`s.

19. **The animation ticker parks; it is not respawned.** A retile happens on every window event, so
    creating and joining a thread per animation was a kernel round trip per event. The thread now
    waits on `animActive_`, which `AnimCommit` sets and `AnimStop` resets; `AnimShutdown` is the
    only thing that ends it. `AnimStop` is called on every animation end, so it must stay cheap and
    must not touch the thread.

20. **Every settings page must call `theme::PrepareDialog(page)` in `WM_INITDIALOG`.** It hides each
    `GROUPBOX` and records its rectangle so the theme can repaint it as a card, switches combo
    boxes to owner-draw, and subclasses the edits and lists. A page that forgets it still works but
    renders as a light-mode Win32 dialog inside a dark one — which is exactly how the Search page
    shipped until it was noticed by comparing it against Layout.

21. **A window's own size limits are part of the layout, and are learned, not assumed.**
    Three separate complaints turned out to be the same missing idea: Steam has a minimum size
    and spilled over its neighbour, a file-copy dialog has a maximum and left most of its tile
    bare, and an elevated window silently refused to move at all and left a hole.

    `ComputeLayout` therefore takes a `ConsMap` of per-window `SizeLimits` and every algorithm
    honours it - a split moves off its ratio only as far as the limits demand, and space one
    window cannot use goes to its neighbours instead of being abandoned. The numbers come from
    `WM_GETMINMAXINFO` once per window (`SMTO_ABORTIFHUNG`, 60 ms - a hung app must not take the
    UI thread with it) and are then corrected by `WindowManager::LearnFromLastPass`, which
    compares where each window was asked to go against where it actually is. Applications
    enforce limits they never declare, so observation is the authority and the message is only
    a first guess.

    Two escape hatches matter as much as the solver. A window that ignores placement twice is
    marked `immovable`, and one that keeps using less than `kUsesEnough` of its tile is marked
    `tooSmall`; both then drop out of `order` entirely. A tile reserved for a window that will
    not fill it *is* the empty rectangle the user is complaining about. Note that a maximum on
    the axis a window's split does not run along cannot be satisfied by geometry at all - see
    test 9 in `tests\layout_test.cpp` - which is why the escape hatch is not optional.

    The adaptive pass runs at most twice per retile (`relayoutPending_`). Two windows with
    incompatible limits would otherwise ping-pong forever.

    Two details the first live run turned up, both of which look optional and are not:

    *Something has to ask the question again.* A window only reveals its limits by ignoring a
    placement, and on an idle desktop nothing else happens to prompt a second look - so the
    first live run tiled the board once, learned nothing, and left a window sitting in a quarter
    of its tile. `RetileNow` now re-arms `TIMER_RETILE` when it left any attempt unjudged.
    `ApplyPlacements` deliberately records an attempt only for a window it is actually asking to
    move, which is what makes that terminate: once the board has settled there is nothing to
    verify and the timer is not re-armed.

    *A window that is still opening looks exactly like a window enforcing a limit.* The first
    unrestricted run on a real desktop opened three File Explorer windows and immediately
    concluded that Explorer has a 413-pixel maximum width and WhatsApp a 902-pixel one. Neither
    is true; they were simply measured before they had finished starting. A false limit is far
    worse than no limit, because it is sticky and it distorts every later pass. `ManagedWindow`
    therefore keeps `lastSeen`, and a limit is believed only once two consecutive looks agree;
    until then the attempt stays on the books. After the fix the same board reported no maxima
    at all and only real minima - Explorer 147x236, WhatsApp 486x393, Claude 602x401.

    *Clamping is not the same as sharing.* When the limits cannot all be met, clamping each
    split in turn pins whichever subtree is deepest to its bare minimum and leaves every spare
    pixel with the shallowest one. The first live board came out as 122 / 886 / 122 / 750 px -
    every limit respected, and unusable. `ConstrainedSplit` therefore falls back to dividing the
    slack in proportion to how many windows sit on each side, but *only* on the path where the
    ratio has already proved impossible, so an ordinary board still splits exactly 50/50 as
    dwindle always has. The same board now comes out 374 / 1139 / 375 with the fourth window
    floated. Test 10 in `tests\layout_test.cpp` is that board.

22. **`IsElevatedProcess` is a question about integrity levels, not about elevation.**
    It used to ask whether `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` failed. That access
    right is deliberately grantable across integrity levels, so it *succeeds* against an elevated
    process of the same user: elevated windows were reported as ordinary, got tiled, refused
    every `SetWindowPos` in silence, and left a hole. It now compares the target's mandatory
    integrity level against ours. `tests\probe.bat` checks that rule against every process on
    the machine; on the development box 10 were above us and openable - every one of them a
    window the old test got wrong.

23. **Nothing happens while a fullscreen application owns the screen.** Every single thing this
    program does is a cross-process `SetWindowPos`, a DWM attribute write or a foreground change,
    and each of those costs a game frames — a DWM attribute write on some drivers costs it
    exclusive fullscreen outright. So `WindowManager` has a parked state: `UpdateGameMode()`
    asks `FullscreenAppActive()`, and while the answer is yes there is no tiling, no animation,
    no accent border, no focus-follows-mouse poll and no system-monitor overlay. `Classify` also
    returns `Ignore` for any window that covers a whole monitor without a title bar, so a game is
    never managed in the first place.

    `FullscreenAppActive` asks the shell first (`SHQueryUserNotificationState`), because that is
    the only way to see *exclusive* fullscreen Direct3D from outside the process, and falls back
    to measuring the foreground window for borderless-windowed games and fullscreen video. The
    measurement deliberately rejects a maximised window (`IsZoomed` — that stops at the work
    area, and the taskbar is still there) and any window that still has a title bar.

    Leaving the state is the awkward half: no window events arrive from a game, so nothing would
    ever prompt a second look. `AppGameModeChanged(true)` starts `TIMER_GAMECHK` for exactly that
    reason, and stops it again on the way out.

24. **Three separate things have an opinion about whether the overlay exists.** The user's
    `monitor_enabled` setting, game mode, and whether the display is switched off. They used to
    call `MonitorSetVisible` directly, so whichever ran last won — alt-tabbing out of a game with
    the screen off brought the panel back to repaint at nobody. `UpdateOverlayVisibility()` in
    `main.cpp` is now the only caller, and it takes all three into account. The display-off case
    comes from `RegisterPowerSettingNotification(GUID_CONSOLE_DISPLAY_STATE)`: a machine left on
    overnight was sampling CPU, GPU, disk and network once a second and repainting a layered
    window nobody could see.

25. **What a window will accept is remembered between runs.** The learning loop in invariant 21
    can only discover a limit by handing a window a size it refuses and watching it overhang its
    neighbours — so paying that price on every launch, for the same applications, was pure waste.
    `Config::learnedLimits` keys `SizeLimits` by `"process|class"` (the granularity the limit
    actually belongs to: every Steam window has the same minimum, and it is not a property of any
    one `HWND`), and `AddWindow` seeds a new window from it. Written back through
    `RememberLimits`, which only marks the config dirty when the numbers actually changed, and
    flushed once in `Shutdown` rather than on every discovery.

26. **A minimum that does not fit is not a layout problem.** Invariant 21's solver moves splits
    off their ratio as far as the limits demand, but there is nothing it can do when the minimums
    genuinely do not fit — it produces slots the windows refuse, and they overhang. Two rules
    catch that before `ComputeLayout` ever sees them, and the difference between them matters:

    - `tooLarge` is **permanent and remembered**: the window's own minimum is larger than the
      whole usable area of the monitor. Nothing will ever satisfy it there. It is re-checked
      every pass against the *current* work area, though, so a larger display or a lower scaling
      brings the window straight back into the tiling — the flag survives in `config.ini`, and
      without the re-check a window would stay floating forever because of a monitor that is no
      longer attached.
    - `crowdedOut` is **recomputed every pass**: it would fit on its own, but not beside
      everything else open right now. `DropWindowsThatCannotFit` considers windows
      smallest-minimum first, so the small ones keep their tiles and the one oversized
      application is what gets left out — rather than whichever happened to be added last.

    Either way the window is positioned exactly once, by `ParkAsFloating`, and then left alone:
    from that point it behaves like any other floating window and the user owns its position.
    `LearnFromLastPass` skips both, because a window nobody is placing is neither obeying nor
    disobeying anything.

27. **An elevated window is reported, not silently dropped.** `Classify` returns
    `ManageVerdict::Blocked` rather than `Ignore` for a window at a higher integrity level, so
    the count is visible in the tray tooltip and the settings status line. A window sitting on
    top of the tiling with no explanation is indistinguishable from the program being broken,
    which is how it was reported.

    The fix offered alongside it is a scheduled task, not a manifest. `requestedExecutionLevel`
    of `requireAdministrator` would put a UAC prompt in front of the user at every single launch,
    and `uiAccess="true"` needs a signed binary in a protected directory. A logon task registered
    with `RunLevel=HighestAvailable` (`InstallElevatedAutostart`, via `schtasks /Create /XML`)
    starts elevated with no prompt at all, which is the only arrangement that both works and is
    not user-hostile. Registering it needs administrator rights once — hence "Restart as
    administrator" first. Note `ExecutionTimeLimit` of `PT0S`: the default three-day limit would
    silently kill a window manager.

28. **Process facts are cached per pid, for two seconds.** `Classify` runs on every window event
    in the session and asks for the owning process's image name and integrity level; both are
    several kernel round trips and both give the same answer for every window of the same
    application. `ProcEntry` in `winutil.cpp` caches them. Two seconds, not forever, because pids
    are recycled — far longer than a burst of window events and far shorter than any plausible
    reuse.

29. **A drag places a window on the side it was dropped on, and `Insert` cannot do that.**
    `BspTree::Insert` splits the target leaf along its *longer* axis, which is exactly right for
    a window appearing out of nowhere and exactly wrong for one the user has just aimed at an
    edge: drop a window on the right of a tall tile and dwindle's rule puts it underneath.
    Reported, reasonably, as "sometimes it goes right and sometimes it goes down". Before this
    the drop did not even choose a side — it swapped the dragged window with whatever was under
    the pointer, so the result always kept whatever split the tree already had.

    `MoveBeside(moving, target, side)` and `MoveToEdge(moving, side)` take the orientation from
    the direction instead: left/right always produce a side-by-side split, up/down always a
    stacked one. Both `Remove` first, so a window dropped next to its own neighbour is not
    counted twice, and both look the target leaf up *after* that — `Remove` splices the sibling
    into the parent's slot and frees the parent, so any `BspNode*` held across it is suspect.

    `WindowManager::PlaceBeside` moves the window in `ws->tiled` as well as in the tree. Master
    and Grid have no tree at all and lay windows out straight from that list, so updating only
    one of the two would make the same drag do different things depending on the layout.

30. **The drop side is decided by the rectangle's diagonals, not by an edge zone.** `DropSide`
    cuts the target into four triangles meeting at its centre and takes the one the pointer is
    standing in, measuring each axis as a fraction of that half-dimension. The obvious
    alternative — "the outer third of each side is that side" — leaves the middle of the window
    undefined and the corners arbitrary, which is what made the gesture feel random. Ties go to
    the horizontal, which is the axis people reach for. A drop that lands on no window at all
    (a gap, the outer margin, an empty workspace) falls through to the same function applied to
    the monitor's work area, and `MoveToEdge` gives the window that whole side of the screen.

31. **The drop indicator and the drop itself must be one function.** `PlanDrop` returns both the
    tree operation and the rectangle to highlight, and `OnDragTick` and `OnMoveSizeEnd` both go
    through it. Two implementations of "where would this land" agree right up until one of them
    is edited, and the whole point of showing a preview is that it is never wrong.

    The indicator (`dragguide.cpp`) is layered, `WS_EX_TRANSPARENT` and `WS_EX_NOACTIVATE`:
    Windows runs the drag in a modal loop inside the *other* process, and anything that takes
    the mouse or the foreground would end that loop. It is only repainted when its rectangle
    actually changes, which during a drag is a handful of times rather than 25 a second — the
    highlight can be half of a 4K screen and walking four million pixels per tick is visible.

32. **`EVENT_SYSTEM_MOVESIZESTART` does not say whether a border or the title bar was grabbed.**
    Both a move and a resize arrive as the same pair of events, and rearranging the layout
    because somebody widened a tile by ten pixels would be indefensible. `dragStartRect_` is
    recorded at the start and the size compared against it: `OnDragTick` hides the indicator as
    soon as the size moves, `dragIsResize_` latches so a border drag that passes back through
    its original size does not start offering to rearrange half way through, and
    `OnMoveSizeEnd` checks the final size itself because a quick drag can finish between two
    ticks. Note that a mouse resize of a tiled window still only snaps back; adjusting the split
    ratio from a border drag is a separate feature that does not exist yet.

33. **A window is never written off on the strength of its first millisecond.** `Classify` has to
    say no to a window with no title, one DWM still has cloaked, one not shown yet and one whose
    styles have not been applied — and almost every application produces all four in the first few
    milliseconds of opening a window. Electron, Chrome, Qt and JetBrains all create the HWND and
    name it afterwards. Looking exactly once, on `EVENT_OBJECT_SHOW`, and never again is the whole
    of "it did not get arranged until I moved it".

    So `Classify` reports **why** it refused (`IgnoreReason::Transient` vs `Permanent`), and a
    transient refusal puts the window on `pending_` for a handful of second looks over about two
    seconds (`TIMER_PENDING`, `kPendingTries`). Three things shorten that wait or make it
    unnecessary: `EVENT_OBJECT_NAMECHANGE` is subscribed, so a title arriving is acted on at once
    for windows already on the list; `OnMoveSizeEnd` adopts a window it has never seen, because
    picking one up and putting it down is the most direct way somebody says "this one"; and
    `UpdateGameMode` runs a full `ScanExistingWindows()` when a fullscreen application lets go,
    since everything that opened behind it was waved past.

    A new rejection rule must classify itself. Getting it wrong in the permanent direction brings
    the original bug back; wrong in the transient direction only costs a few hash lookups, and
    `kMaxPending` caps even that.

34. **The verification pass must be able to stop.** After moving windows, `RetileNow` comes back
    320 ms later to see what they did with the space — a window only reveals its limits by
    ignoring us. But an entry stays on `attempts_` until the window reports the same rect twice
    running, and a window that never holds still (a video player, an application still loading)
    never does. That kept the timer re-arming three times a second for the life of the process,
    on a desktop nobody was touching. Two caps now bound it: `kSettleLooks` retires a single
    window that will not settle, and `kMaxVerifyChain` bounds how many passes the loop may ask
    for without an outside event. `RequestRetile` and any `RetileNow` that is not the loop calling
    itself back (`verifyPass_`) reset the budget, so responsiveness is untouched — the cap only
    ever stops the loop talking to itself.

35. **The crash handler may not touch a container.** `managed_` is mutated on every window event,
    so an unhandled exception is most likely to arrive part way through one. Walking a half-updated
    `unordered_map` from the exception filter faults again, and the user is left with exactly the
    hidden windows the handler exists to put back. `EmergencyUnhideAll` therefore reads a flat
    fixed array of `HWND` maintained alongside by `SetHidden`. Registered hotkeys and the keyboard
    hook need no attention there: Windows releases both when the process dies.

36. **`WM_QUERYENDSESSION` answers the question and does nothing else.** Any other application may
    still veto the session ending. Shutting the manager down there left it torn down — no ticker
    thread, every hidden window put back, the config already written — inside a process that then
    carried on running. The work belongs in `WM_ENDSESSION`, and only when its `wParam` is true.
    `WindowManager::Shutdown()` is idempotent regardless, because the ordinary exit path can still
    reach it afterwards.

37. **The overlay's bitmap is bigger than the overlay.** `MonMeasure` adds `kShadow` on every
    side and `MonDraw` insets the panel by the same amount, so the panel's visible edge is not
    the window's edge. Anything that reasons about where the panel *looks* like it is — the
    drag, `ClampOnScreen`, the settings preview's scale-to-fit — goes through those two
    functions and stays correct; anything that measures the window rect directly does not.

    The margin is transparent, and a layered window does not hit-test transparent pixels, so it
    costs nothing in clicks: dragging and the click-through pin behave exactly as before.

38. **A disabled list view paints itself white and cannot be talked out of it.** `ListView_SetBkColor`
    is ignored while the control is disabled — comctl32 uses the system window colour — and it does
    not go through `WM_ERASEBKGND`, so a subclass cannot intercept it either. That is why switching
    file search off used to put a bright white rectangle in the middle of a black settings page.
    The fix is not to disable it: `UpdateEnabling` on the Search page greys the buttons around the
    list and leaves the list alone, which also happens to be more useful, since the list is what
    the feature *would* index. Do not add a list view to the `gated` array of any page.

39. **Dark scrollbars and dark list headers need an undocumented opt-in.** `SetWindowTheme(control,
    L"DarkMode_Explorer")` does nothing to a scrollbar until the process itself has asked for dark
    mode, and the only way to ask is `SetPreferredAppMode` — ordinal 135 in `uxtheme.dll`, no
    name, no header. `theme::Init` calls it with **ForceDark**, not `AllowDark`: `AllowDark` means
    "follow the system app-mode setting", and this application is dark whatever that setting says,
    so on a machine set to light apps every scrollbar stayed white. The column header of a list
    view is a separate control again and needs `DarkMode_ItemsView` of its own — without it there
    is a pure white band across the top of the list. All of it is best-effort by ordinal: on a
    build that does not have these the controls look exactly as they did before.

40. **The search bar's icons are never fetched on the way to the screen.** Asking the shell for an
    icon resolves a shortcut, or looks a packaged app up in its manifest, and either can touch the
    disk. `appicon.cpp` answers from a cache or returns null and queues the work on a thread of its
    own; the row draws its lettered tile meanwhile and the window is posted to when the icon lands.
    `AppIconFor` reserves the cache slot at the moment it queues, so eight rows redrawn on every
    keystroke queue each icon once rather than once per keystroke.

    `IShellItemImageFactory::GetImage` hands back **straight** alpha and `AlphaBlend` wants it
    premultiplied — skip that correction and every icon gets a bright halo. The bitmap is a DIB
    section, so it is corrected in place rather than copied through `GetDIBits`.

41. **A worker that can be abandoned must not release a slot it no longer owns.** The thermal probe
    keeps one live run at a time. That used to be a plain flag, and the abandon path cleared it so
    a replacement could start — but the abandoned thread then cleared it *again* when it eventually
    finished, releasing the slot its replacement was using, and the next `Want()` started a second
    probe alongside it. `running_` now holds the *id* of the run that owns the slot, and every
    release is a compare-exchange against that id. Any other "one at a time" worker that can be
    abandoned needs the same shape.

42. **The search bar never fetches an icon on the way to the screen, and after
    the first run it never fetches one at all.** Three things in `appicon.cpp`,
    and all three are needed:

    - `AppIconFor` answers from memory or returns null and queues. It reserves
      the cache slot *at the moment it queues*, so eight rows redrawn on every
      keystroke queue each icon once rather than once per keystroke.
    - What was fetched is written to `icons.cache` as raw premultiplied pixels
      and read back in one sequential pass on the loader thread before it serves
      anything. Two weeks' expiry, because an icon does change - an application
      updates - just not often.
    - Everything in the catalogue that the cache does not hold is fetched anyway,
      in the background, at `THREAD_MODE_BACKGROUND_BEGIN`, after a fifteen
      second hold-off. Urgent requests always overtake the sweep, and the thread
      drops back out of background mode while it serves one.

    Note what that costs if it is got wrong: `IShellItemImageFactory::GetImage`
    on a cold mechanical disk is tens of milliseconds, and eight of those is
    most of a second - long enough that the icons look like they are never
    coming rather than like they are loading.

43. **A shell icon is straight alpha and `AlphaBlend` wants premultiplied.**
    Skip the correction and every icon gets a bright halo where it should feather
    into the row behind it. The bitmap `GetImage` hands back is a DIB section, so
    it is corrected in place rather than copied through `GetDIBits` and back -
    and the same raw bits are what goes into the cache file.

44. **Programs are a separate source from files, and capped separately.** They
    come out of the same index - `FileEntry::exe` - but through
    `SearchPrograms` rather than `SearchFiles`, because a query matching a
    folder full of documents would otherwise fill all eight rows before the
    executable anybody was reaching for was considered. MAP invariant 16 applied
    to a source that did not exist when it was written.

    The walk of Program Files is deliberately not the general walk with a filter
    on the end: `WalkPrograms` keeps only `.exe`, skips the folders that hold an
    application's parts rather than the application (`SkipProgramDirectory`),
    skips the executables that serve another executable (`NoiseExecutable`), and
    skips two-letter binaries outright - a developer toolchain ships a whole
    POSIX userland, and `ex`, `sh` and `ls` match almost any short query while
    being almost never what was meant.

    Every index hit is also penalised by how deep it is buried.
    `Vendor\Product\product.exe` is the application; the same name four folders
    further in is one of its parts.

45. **The CPU bar is one segment per logical processor, and brightness is the
    level.** Filling part of each segment was the obvious first try and it does
    not survive the size: at six pixels tall, a rounded segment two pixels into
    its fill is a dot, and sixteen dots is a dashed line. Brightness stays
    readable all the way down.

    The point of the display is that an averaged bar cannot tell one pinned
    thread from a machine that is evenly busy, and those mean opposite things -
    so where several cores are folded into one segment (past about thirty, which
    is where segments stop being distinguishable) it is the **busiest** of them
    that is shown, not their mean. Averaging is what the single bar already did.

    `SystemProcessorPerformanceInformation` reports the current processor group
    only, so the first call is sized from `GetActiveProcessorCount` and there is
    one retry at whatever size it asks for.

46. **A sensor block published by another process is bounds-checked before it is
    believed.** `CoreTempSensor` and `HwInfoSensor` read shared memory somebody
    else wrote: the signature, the element size, the element count and every
    value go through a range check first, because a block from a version whose
    layout does not match is not an error - it is another program's memory read
    as if it were a struct. `tempprobe.bat --selftest` publishes deliberately
    wrong blocks and checks they are refused.

47. **A retile pass measures each window once.** `VisibleRect` and
    `WindowFramePad` both ask DWM for the extended frame bounds, and
    `ApplyPlacements` wanted an answer from each of them for every window it
    moved - the attempt record, the animation's starting rect, and its frame
    padding - which was four cross-process round trips per window per window
    event to answer one question four times. `MeasureWindow` returns both
    facts from one pair of calls, and the whole plan is measured into a local
    vector before anything is decided. Anything added to that function reads
    the measurement; nothing asks again.

    The one exception is deliberate and marked: un-maximising a window moves
    it, so a window that had to be restored is re-measured afterwards.

48. **Mod-drag starts the system's move loop; it does not run one.**
    `moddrag.cpp` posts the target `WM_NCLBUTTONDOWN` with `HTCAPTION`, which
    is exactly the message a real click on a title bar delivers - so Windows
    runs its own modal move loop and the tiler sees the ordinary
    `EVENT_SYSTEM_MOVESIZESTART` / `MOVESIZEEND` pair it has always handled.
    The drop indicator, the drop side, the cross-monitor hand-off and the
    learned size limits all come for free, and there is exactly one drag
    implementation to keep correct rather than two.

    Three things that follow from it, and are load-bearing:

    - **The button-up is never swallowed.** A `WH_MOUSE_LL` hook returning
      non-zero keeps the message from every application including the move
      loop, so eating the release would leave the window stuck to the pointer.
      Only the press is eaten, so the application underneath does not also see
      a click.
    - **`ModDragBegin` checks the button is still down.** The hook posts and
      returns; if the UI thread was busy the release can arrive first, and
      then the loop waits for a button-up that has already happened. Same
      symptom, reached the other way round.
    - **The hook decides on membership itself, from a published set.** It has
      to answer "swallow or not" before anything can look at the window, so
      `ModDragPublishWindows` hands it the windows the manager is arranging.
      Without it the gesture would eat mod+click everywhere on the machine -
      including on the applications the user excluded, which is the one place
      the exclusion list was supposed to protect.

49. **The overlay's readout order is a permutation, and the config file may
    lie about it.** `Config::monOrder` is eight metrics in the order they are
    drawn, stored by name (`monitor_order = cpu, ram, gpu`) for the same
    reason `monitor_theme` is - invariant 9. A hand-edited file can name a
    metric twice, leave three out, or name one that no longer exists, and the
    overlay still has to index eight arrays by it. `MonitorNormaliseOrder`
    repairs rather than rejects: unknown and duplicate entries are dropped and
    whatever is missing is appended in the declared order.

    Which metrics are *shown* is separate (`monShow*`), deliberately: hiding a
    readout and bringing it back must not lose its place. The eight rows on
    the Monitor page are addressed by **slot**, not by metric - `IDC_MON_SHOW_FIRST + n`
    is the nth row on screen, and which metric is in it comes from the order.
    The ids they replaced were per-metric and not in metric order, so
    `IDC_MON_CPU + 3` was Disk; that is the trap the renaming closes.

## Things that surprised us, recorded so they surprise nobody twice

- **The shell owns almost every `Win`+letter chord.** `RegisterHotKey` was refused for all but
  `Win+J` and `Win+Y` on the test machine. That is why the keyboard hook exists; it is not
  gold-plating.
- **Windows 11 cannot move its taskbar.** `StuckRects3` is ignored and `TaskbarSi` (small icons) no
  longer does anything — both verified on build 26200. Only Explorer patchers can do it, and this
  app deliberately will not. The Layout page's reserved margins are the supported way to make room
  for a bar.
- **Win32 has no usable dark mode.** `theme.cpp` builds it from three mechanisms: `WM_CTLCOLOR*`
  for backgrounds, owner-draw for push buttons / combo boxes / tabs, and subclassing for
  checkboxes. Custom draw alone is not enough for buttons — the themed control still paints its
  own label underneath, which shows through as a ghost.
- **`UpdateLayeredWindow` wants premultiplied alpha.** The overlay draws into a
  `PixelFormat32bppPARGB` GDI+ bitmap over the DIB bits. Drawing through the HDC instead gives
  straight alpha and a washed-out panel.
- **GDI+ headers need bare `min`/`max`**, which `NOMINMAX` removes. `theme.cpp` and `monitor.cpp`
  pull in `<algorithm>` and `using std::min/max` before including `gdiplus.h`.
- **Elevated windows cannot be moved by a non-elevated process.** They are classified `Ignore`
  rather than fought with on every pass, and the tray offers "Restart as administrator" because
  that is the only actual fix. Detecting them is invariant 21's problem, not a one-liner.
- **`SetWindowPos` on a window above our integrity level returns success.** UIPI drops it
  silently. There is no error to check, which is why placement is verified by looking at where
  the window ended up.
- **`small` is a `typedef` for `char`** in the RPC headers that `windows.h` drags in. Naming a
  local `small` produces "'SizeLimits' followed by 'char' is illegal", which reads like anything
  but the real cause.
- **A suspended UWP app is DWM-cloaked, not hidden.** Cloaked windows are skipped; they come back
  through `EVENT_OBJECT_UNCLOAKED` when the user returns to them.
- **`FindWindow(L"Progman", nullptr)` can return null** on this build even though a window of that
  class is enumerable. `GetShellWindow()` is asked first for that reason.
- **Per-process CPU, memory and I/O come from one `NtQuerySystemInformation` call.** The documented
  APIs need a handle per process and several calls each; this returns the lot in one buffer, which
  is what Task Manager does. `sysinfo.cpp` declares the full `SYSTEM_PROCESS_INFORMATION` rather
  than the abbreviated one in `winternl.h`, because the times and I/O counters live in the fields
  that header hides inside `Reserved1`.
- **`MSAcpi_ThermalZoneTemperature` needs administrator, and that is why temperature used to be
  blank.** Unelevated it is not `ExecQuery` that refuses but the enumeration afterwards, so the
  failure looks like an empty result set rather than an error. The same ACPI zone is published as
  the `Thermal Zone Information` performance counter set, which any user can read
  (`\Thermal Zone Information(*)\High Precision Temperature`, in tenths of a Kelvin), so that is
  what `thermal.cpp` asks first. The WMI class is kept only for machines that publish it but not
  the counter.
- **The ACPI zone is not the CPU package.** On a desktop it is usually a mainboard sensor and it
  barely moves. Reading the real package sensor means reading an MSR, which means a kernel driver;
  the only honest way to have one without shipping one is to use somebody else's, so `thermal.cpp`
  looks for LibreHardwareMonitor's (or OpenHardwareMonitor's) WMI namespace first and falls back to
  the zone. The overlay prints which source answered — `package` or `thermal zone` — rather than
  presenting them as the same claim.
- **GPU temperature, unlike CPU, is exact and unprivileged.** `nvml.dll` ships with every NVIDIA
  driver and lands in System32; `atiadlxx.dll` does the same for AMD. Both are loaded by name at
  run time, so a machine without either simply reports no sensor and nothing has to link against
  anything. Which ADL Overdrive generation answers depends on the card, so all three are tried.
- **Every source runs on one background thread.** A WMI round trip costs tens of milliseconds — far
  too much for the timer that redraws the overlay. The probe backs off to once a minute after three
  failures, retries the LibreHardwareMonitor namespace about once a minute so starting that tool
  upgrades the reading without a restart, and the sampler only ever reads the last answer it left
  behind.
- **`SetTimer` cannot animate.** Its floor is the ~15.6 ms system tick however small an interval
   you ask for, and `timeBeginPeriod` does not change that. A 140 ms animation was therefore nine
   frames, on a 180 Hz screen. The animation is driven by a thread waiting on a
   `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer that posts `WM_AWA_ANIMTICK`, which measures at
   ~6 ms. Only one tick is ever in flight (`animPending_`), or a slow frame would build a backlog
   the UI thread then has to drain.
- **A tray process has no foreground rights.** `SetForegroundWindow` on the launcher leaves it
   drawn but not typed into. `winutil::FocusWindow` already knows the attach-thread-input dance;
   use it for anything that has to take the keyboard.
- **A memory-DC buffer must blit before `EndPaint`.** `theme::Buffered` copies to the target DC in
   its destructor, so a buffer declared alongside a `PAINTSTRUCT` blits after the DC has been
   released and the control paints nothing at all. Give it its own scope.
- **The overlay builds its own fonts.** Borrowing `theme::FontUI()` and friends looked tidy but
  they are fixed-size, so the "Size" slider stretched the panel while the numbers stayed put.
  `monpaint.cpp` derives every font size from the same scale as the geometry.

## Testing

`tests\run.bat` compiles `layout.cpp` with a console harness and asserts on the rectangles it
produces - 61 assertions covering every layout, both kinds of size limit, gaps, and boards whose
constraints cannot all be met. It touches nothing on the desktop, so it is safe to run at any
time, and it is the right place to add a case for anything geometric.

`tests\probe.bat` prints how every window currently on screen would be classified, what size
limits it declares, and whether it is out of reach - read-only, and the fastest way to answer
"why is this window not being arranged?".

Three harnesses render the UI to `tests\shots\*.png` **without starting the window manager**,
which matters more than it sounds: the only other way to look at a change to any of this is to run
the real thing, and running the real thing rearranges every window on the desktop and does not
remember where they were.

- `tests\monshot.bat` draws the overlay through its own painter, one PNG per style and one per
  skin, composited over a checkerboard so the translucency and the shadow are visible rather than
  assumed. `monshot.exe --bench` times the painter instead, per style, in ms/frame - which is the
  number to quote when changing anything in `monpaint.cpp`.
- `tests\launchshot.bat` stands the search bar up on its own, types into it, waits for the shell
  icons, and captures it. Safe because the launcher touches no window but its own.
- `tests\uishot.bat` opens the settings window with `app.h` stubbed out and captures every tab.
  Note that owner-drawn controls do not always survive `PrintWindow` - the tab strip in particular
  can come back blank - so a missing control in one of those PNGs is worth confirming before it is
  believed.
- `tests\tempprobe.bat` prints what this machine can say about CPU and GPU temperature and which
  source answered, which is the whole of "why is the temperature blank on my machine".
  `tempprobe.bat --selftest` publishes synthetic Core Temp and HWiNFO blocks - including a
  deliberately malformed one - and checks the readers handle them. That matters because neither
  tool is installed on most machines, so without it those two readers would ship having never run.

A note on judging any of the PNGs: they are worth zooming into before believing. The core-load
strip was written off as "a dashed line" from a downscaled view and turned out to be drawing
exactly the right thing; a nearest-neighbour crop settled it in one look.

`tests\testwin.cpp` is a window that misbehaves exactly the way you ask it to:
`testwin.exe --name X --min 900x500` or `--max 420x260`. Reproducing "Steam will not shrink" or
"the copy dialog will not grow" with the real applications means installing them and hoping they
behave the same next time; this reproduces both exactly, and closes when you are done. Build it
the same way the other two are built.

It also reproduces invariant 33 — the reason a window used to sit untiled until it was touched:

```
testwin.exe --late-title 400        no name until 400 ms in, like Electron and Chrome
testwin.exe --late-show 400         created hidden, shown later
testwin.exe --late-resizable 400    WS_THICKFRAME applied after creation
```

Run two or three of those at once and they should take their tiles on their own. Before the fix
they stayed where Windows put them until they were clicked on, which is what makes this the
regression check worth running after anything that touches `Classify` or `OnWinEvent`.

With `debug = true`, a retile logs the BSP tree (`BspTree::Describe`, leaf minimums included) and
the rect it planned for each window. That pair is what turns "the layout looks wrong" into an
arithmetic question, and it is how the 122px board above was diagnosed - the tree shape is
otherwise invisible.

Beyond those two, what has actually been used and is worth reaching for again:

- Drive the settings UI from PowerShell with `SendMessage`/`PostMessage` on control IDs from
  `src\resource.h`, then assert on `config.ini`. This exercises the real Save → reload path.
- Assert on geometry, not screenshots: enumerate windows, read
  `DwmGetWindowAttribute(..., DWMWA_EXTENDED_FRAME_BOUNDS)`, and check for gaps and overlaps.
- Sample window rects in a tight loop during a layout change to prove the animation is
  interpolating rather than jumping.
- `debug = true` in the config writes a decision log to `%APPDATA%\ProWindows\log.txt`,
  including why each window was or was not managed.

When testing on a live desktop, put every already-running app in `ignore_process` first. Otherwise
the first launch rearranges the user's real windows, and their original positions are not
recoverable.
