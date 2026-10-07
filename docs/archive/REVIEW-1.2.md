# ProWindows 1.2 — review, fixes and the three things that were asked for

A second pass over `src/`, after the 1.1 review in [REVIEW-1.1.md](REVIEW-1.1.md). That pass was about
correctness; this one was asked to look at the overlay, the search bar, the look of the app, and
what it costs to run. Everything below is either a defect that was traced through the real control
flow, or a change with a measurement next to it.

Verification is at the bottom. Nothing here has been run on a live desktop by me — see the note.

---

## A. Defects

### A1 — P2 — The thermal probe could end up running twice ✔
`thermal.cpp`

`Probe` keeps one live run at a time behind a `running_` flag. `Stop()` waits five seconds and, if
the probe is parked inside a WMI call, **abandons** it and clears the flag so a replacement can
start — which is the whole point of abandoning it, and is [MAP.md](MAP.md) invariant 12.

The abandoned thread then cleared the same flag *again* when it eventually finished. If a
replacement had started in between, that release handed away the slot the replacement was using,
and the next `ThermalWant()` started a **second** probe alongside it: two threads, two WMI
connections and two ACPI polls for one temperature reading. Nothing visible went wrong — the
generation counter still stopped the older one publishing — which is exactly why it would have
stayed there.

**Fix.** `running_` holds the *id* of the run that owns the slot rather than a bare flag, and every
release is a compare-exchange against that id. An abandoned run finding the slot reassigned leaves
it alone. Recorded as invariant 41, because any other abandonable worker needs the same shape.

### A2 — P2 — Two kernel handles leaked per window event, if the ticker ever failed to start ✔
`wm.cpp`

`AnimCommit` creates `animQuit_` and `animActive_` and then the animation thread. If `CreateThread`
fails it falls back to `SetTimer`, correctly — but `animThread_` stays null, so the *next* retile
runs the same block again and overwrites both event handles. Whatever stopped the thread starting
is unlikely to have gone away by the next window event, so this leaks two handles per event for the
life of the process.

**Fix.** The events are created only if they are not already there.

### A3 — P2 — The overlay could be left following the mouse with no button held ✔
`monitor.cpp`

The drag is `SetCapture` plus `WM_MOUSEMOVE`, ended by `WM_LBUTTONUP`. Capture can be taken away
without a button-up ever arriving — a system dialog, the task switcher, anything that grabs the
mouse — and `g_dragging` then stayed set, so the panel followed the pointer around the screen until
the next click.

**Fix.** `WM_CAPTURECHANGED` ends the drag.

### A4 — P3 — The version string had not followed the release ✔
`common.h` said `1.1.0` while `res\app.rc` said `1,2,0,0`. The tray tooltip, the diagnostics report
and the log header all print the former. Now `1.2.0`.

### A5 — P3 — The app catalogue could finish before there was a window to tell ✔
`launcher.cpp`

`LauncherInit` started the scan and the scan posts `WM_APP+1` to `g_wnd` when it finishes. With the
window now created during `LauncherInit` (see C1), the two are ordered: the window first.

### A6 — P3 — A dead branch, and one that could not be reached ✔
`monpaint.cpp` — `AddRoundedPath` opened with `if (d <= 0.5f || d > r.Width || d > r.Height)` whose
body only ever tested `d <= 0.5f` again. Folded.
`settings_keys.cpp` — the Monitor page decided which styles draw a history curve from its own
hard-coded list of two, which is precisely the thing that goes stale the moment a style is added.
It now asks `MonitorStyleUsesGraphs`, and adding a style is one edit again.

---

## B. The system monitor

### B1 — The panel is three and a half times cheaper to draw, *and* it has a drop shadow now

The overlay repaints thirty times a second for the length of every glide, so this is the one piece
of drawing in the project whose per-frame cost is worth measuring. It was not being measured, so
the first thing added was a way to: `tests\monshot.bat` renders every style and every skin to PNG,
and `monshot.exe --bench` times the real painter, per style, in ms/frame.

That immediately contradicted the assumption I had started with. Caching the GDI+ `Font` objects —
the obvious suspect, one constructed per string per frame — made **no measurable difference at
all**; the fonts are thin wrappers and the cost is inside `DrawString`. The caching is still there,
because it removes allocations for nothing, but it is not why the number moved. What actually
dominated was the drop shadow I had just added: nine stacked antialiased rounded rectangles the
size of the whole panel, every frame, at two thirds of the total.

The shadow, the gradient, the gloss and the border are identical every frame. They are now painted
once into a bitmap keyed by size, skin, opacity and radius, and blitted.

| eight metrics, busiest-app line on | before | after | |
| --- | --- | --- | --- |
| Rows | 2.27 ms/frame | **0.66** | and now with a shadow |
| Cards | — | **0.96** | new style |
| Graph | 3.03 ms/frame | **0.78** | |
| Compact | 0.77 ms/frame | **0.16** | |
| Ticker | — | **0.13** | new style |

At 30 fps that is roughly 7% of a core down to 2% while a reading is actually gliding — and the
guard below means most frames do not happen at all.

### B2 — A frame that would change nothing is not drawn

`BeginEase` already refused to start a glide for a movement too small to see. But a *sample* forced
a repaint once a second whatever happened, and on an idle machine the readings do not move.

`Redraw` now folds everything the frame would contain — the size, the skin, the eased percentages,
and the text of every reading — into one signature and skips `UpdateLayeredWindow` when it matches
the last frame. Including the text matters: a reading can go from "9.7 GB" to "9.8 GB" without
moving a bar by a pixel, and that still has to be drawn.

### B3 — Two new styles, three new themes, and a shadow

**Cards** gives each metric its own raised card with a coloured rail down the left edge. With six or
eight metrics on, the eye finds the colour before the number, which is the right way round at that
density and the opposite of what Rows does.

**Ticker** is one thin line: a dot that doubles as the gauge, a short name, a number. Meant to be
laid out across rather than down and parked along a screen edge, where a panel would be in the way.

**Tokyo Night**, **Catppuccin** and **Rosé Pine** join the palette list. Sixteen themes, eight
styles, any combination.

The **drop shadow** is what stops the panel reading as a rectangle painted onto the wallpaper. It
lives inside the panel's own bitmap, so `MonMeasure` and `MonDraw` both account for it and the
window is now larger than the panel it shows. The margin is transparent and layered windows do not
hit-test transparent pixels, so dragging and click-through are unaffected. Invariant 37.

**A reading close to its ceiling warms up**: above about 72% a bar, ring or column slides towards
amber and then red. The text keeps its flat colour — moving both made the panel look like it was
flickering — and the themes whose whole point is a single hue (Graphite, Terminal, Amber) declare
themselves out of it in the skin table rather than being special-cased in the painter.

---

## C. The search bar

### C1 — It opens with nothing left to do

The window was created on the first chord: a `CreateWindowEx`, a DWM dark-title-bar call and a DWM
corner-preference call, all on the path between pressing the shortcut and seeing anything. It is
now created hidden during `LauncherInit`, along with the theme's fonts, so the chord shows a window
that already exists.

### C2 — Real icons, fetched off the hot path

Each row now shows the icon the shell itself would show, packaged apps included. Getting one
resolves a shortcut or reads a packaged app's manifest and can touch the disk, so it is never done
on the way to the screen: `appicon.cpp` answers from a cache or returns null and queues the work on
a thread of its own, the row draws the lettered tile it always drew until the icon arrives, and the
window is posted to when it does. The slot is reserved at the moment it is queued, so eight rows
redrawn on every keystroke queue each icon once rather than once per keystroke.

`IShellItemImageFactory::GetImage` returns straight alpha and `AlphaBlend` wants it premultiplied;
without correcting that every icon has a bright halo. The returned bitmap is a DIB section, so it is
corrected in place. Invariant 40.

### C3 — An empty query shows what you open, not the alphabet

An empty query matched every application equally, so the list came out in whatever order the
tiebreak produced: `About Java`, `Access`, `Administrative Tools`. It now shows only the
applications actually opened from here, most-used first, and says so plainly when there are none
yet rather than padding the list out.

### C4 — The keys are written down, and the row says what Enter will do

Every shortcut the bar supports already worked and none of them was discoverable. There is now a
dim line along the bottom: select, open, open as administrator, close. Each row carries a quiet tag
on the right — `file`, `folder`, `settings`, `answer`, `run` — because "Run" and "Open" are not the
same promise. Applications get no tag: their icon has already said so, and eight repetitions of
"app" is noise.

### C5 — The pointer no longer steals the selection before you touch it

Windows delivers a `WM_MOUSEMOVE` to a window that appears underneath a stationary cursor, and the
bar acted on it — so opening it with the pointer in the middle of the screen moved the selection off
the first row before a key was pressed. The pointer now has to actually move first.

### C6 — Fifty-one heap allocations per keystroke, removed

`SearchSettingsPages` lower-cased every page name into a fresh `std::wstring` on every keystroke, to
answer a question that needs no allocation at all. It uses the pointer form of `SearchScore` now,
which folds case as it scans — the same reasoning the file index has followed from the start, and
the reason that form exists.

---

## D. The settings window

Two real defects, both of them things Win32 does on its own and will not be argued out of.

### D1 — A disabled list view is white, and nothing you set on it changes that ✔

`ListView_SetBkColor` is ignored while the control is disabled — comctl32 uses the system window
colour — and the fill does not go through `WM_ERASEBKGND`, so a subclass cannot intercept it
either. With file search off, which is the **default**, the Search page had a bright white
rectangle in the middle of a black dialog.

**Fix.** Do not disable it. `UpdateEnabling` greys the buttons around the list and leaves the list
alone, which is also more useful: the list is what the feature would index, and that is worth
reading whether or not it is switched on. Invariant 38.

### D2 — Dark scrollbars and dark list headers need an undocumented opt-in ✔

`SetWindowTheme(control, L"DarkMode_Explorer")` was already being called, and does nothing to a
scrollbar on its own: the process has to have asked for dark mode first, and there is no documented
way to ask. `theme::Init` now calls `SetPreferredAppMode` — ordinal 135 of `uxtheme.dll`, no name,
no header — with **ForceDark** rather than `AllowDark`, because `AllowDark` means "follow the system
app-mode setting" and this application is dark whatever that setting says.

The column header of a list view is a separate control again and needs `DarkMode_ItemsView` of its
own; without it there was a pure white band across the top of the shortcut list. Both are
best-effort by ordinal, so a build without them looks exactly as it did before. Invariant 39.

Measured from the captures: the list header went from `255,255,255` to `25,25,25`, and the
scrollbar track is `23,23,23` with a `149,149,149` thumb.

---

## E. Being able to see any of this

The reason the settings window shipped with a white rectangle on its default page, and the reason
"the shadow costs two thirds of a frame" was a surprise, is the same reason: the only way to look at
this UI was to run the real application, and running it rearranges every window on the desktop
without remembering where they were. So nobody looks.

Three harnesses now render the UI to `tests\shots\*.png` without starting the window manager:

- **`tests\monshot.bat`** — every style and every skin, through the overlay's real painter, over a
  checkerboard so translucency and the shadow are visible rather than assumed. `--bench` times the
  painter per style instead.
- **`tests\launchshot.bat`** — stands the search bar up alone, types into it, waits for the shell
  icons and captures it. Safe because the launcher touches no window but its own.
- **`tests\uishot.bat`** — opens the settings window with `app.h` stubbed out and captures every
  tab. Owner-drawn controls do not always survive `PrintWindow`; a control missing from one of
  those PNGs is worth confirming before it is believed.

---

## Verification

- `build.bat` — clean at `/W4`; no warnings in project code.
- MSVC `/analyze` over every file this pass touched (`monpaint`, `montheme`, `monitor`, `appicon`,
  `launcher`, `thermal`, `wm`) — **zero** warnings in project code.
- `tests\run.bat` — **158 passed, 0 failed**.
- `tests\monshot.bat`, `tests\launchshot.bat`, `tests\uishot.bat` — all three build and run, and
  every capture in `tests\shots\` was looked at.

**Not verified by me: live behaviour on your desktop.** Running the tiler rearranges every window
you have open and cannot put them back, so I did not start it — which is exactly why the three
harnesses above exist. The things to watch on the first real run are the overlay's drag and its
click-through pin, since the window is now larger than the panel it draws (invariant 37), and the
first press of the search chord, which should now be immediate.

## What is worth keeping

- The guards in `BeginEase`/`StepEase` are still right and still necessary; the signature check in
  `Redraw` sits on top of them rather than replacing them.
- Storing `monitor_theme` and `monitor_style` by name was what made adding two styles and three
  themes a table edit with nothing else to think about. Invariant 9 earned its keep.
- `SearchScore`'s pointer form was already the right design; C6 was a caller that had not used it.
- The 1.1 review's per-window echo recognition (no global suppression flag) held up under
  everything here — nothing in this pass needed a new exception to it.

---

# Second round — what was asked for after the first build

Three requests, and one of them turned out to have a much simpler explanation
than expected.

## F. "The icons don't appear when I search"

**The build that was running was `ProWindows 1.1\build\ProWindows.exe`** — a
different folder, dated two days before any of this work, with no `appicon.cpp`
in it at all. There were no icons to appear; the 1.2 build has had them since
the first round.

That is the whole of the bug report, but it pointed at a real risk worth fixing
anyway: on a mechanical disk `IShellItemImageFactory::GetImage` is tens of
milliseconds cold, and eight rows of that is most of a second — long enough that
icons look like they are never coming rather than like they are loading. So:

- **`icons.cache`.** What was fetched is written out as raw premultiplied pixels
  and read back in one sequential pass on the loader thread before it serves
  anything. Two weeks' expiry; about 3 KB an icon, capped at 512.
- **A background sweep.** Everything in the application catalogue that the cache
  does not hold is fetched anyway, fifteen seconds after login, at
  `THREAD_MODE_BACKGROUND_BEGIN` — lowered *I/O* priority, not just CPU. Rows
  actually on screen overtake the sweep however long it is, and the thread drops
  out of background mode while it serves one.

So the first run is the only one that ever draws a lettered tile, and even then
only for the seconds before the sweep reaches that entry.

Measured here, warm cache, eight rows: **no icon fetch over 40 ms**, which is the
threshold the loader bothers to log at all.

## G. "I want it to see exe files inside folders"

A `.exe` sitting in a folder — a portable tool, a game unpacked somewhere,
anything that never made a Start-menu shortcut — is now a result in its own
right, tagged `program`.

`FileEntry` gained an `exe` flag, and the indexer gained a second pass over
Program Files, Program Files (x86) and `%LOCALAPPDATA%\Programs`. It is
deliberately **not** the general walk with a filter on the end:

- `SkipProgramDirectory` skips the folders that hold an application's parts
  rather than the application.
- `NoiseExecutable` skips the executables that exist to serve another
  executable — `unins*`, `*crashpad*`, `vcredist`, and a dozen more.
- Two-letter binaries are skipped outright. Git alone puts sixty single-purpose
  POSIX tools under `usr\bin`; `ex` and `ls` match almost any short query and are
  almost never what was meant. The first live run returned `ex`, `expr` and
  `expand` at the top of the list, which is what prompted the rule.
- Every index hit is now penalised by how deep it is buried.
  `Vendor\Product\product.exe` is the application; the same name four folders
  further in is one of its parts.
- An application that already has a Start-menu entry shows up **once**: a
  program whose name matches an app already in the pool yields to it, so "Excel"
  is not listed beside "EXCEL".

Programs are capped **separately** from files, because a query matching a folder
full of documents would otherwise fill all eight rows before the executable
anybody was reaching for was considered. That is MAP invariant 16 applied to a
source that did not exist when it was written.

Cached with the rest of the index — signature bumped to `v2`, so an old cache is
correctly ignored — so it costs part of one background walk and nothing after
that. Measured here: **5,710 entries including Program Files, 20.5 s cold, 0 ms
warm.**

`search_programs = false` in the config file turns it off. If your programs live
somewhere the index does not reach — `D:\Games`, say — add that folder on the
**Search** tab and they come in with everything else.

## H. The monitor: how it shows load, and how it reads temperature

### H1 — The CPU bar is one segment per thread

An averaged bar cannot tell one pinned thread from a machine that is evenly
busy, and those mean opposite things: the first is a program stuck in a loop,
the second is a machine working. Per-processor times come from
`NtQuerySystemInformation(SystemProcessorPerformanceInformation)` — one extra
kernel call per sample — and the CPU bar is divided into one segment per logical
processor, in exactly the pixels the single bar used to occupy.

**Brightness is the level, not length.** Filling part of each segment was the
obvious first try and it does not survive the size: at six pixels tall a rounded
segment two pixels into its fill is a dot, and sixteen dots is a dashed line. I
only caught that by cropping the render at 8× — at 1:1 it was easy to write the
whole thing off as broken when it was drawing exactly the right data.

Past about thirty threads the segments stop being distinguishable, so cores fold
together in pairs and then fours — and a folded segment shows the **busiest** of
its cores, not their mean. Averaging is precisely what the plain bar already did.

`monitor_cores = false` puts the single bar back.

### H2 — Graphs mark their peak

A faint dotted line at the highest point of the visible window. Without it a
graph that has been flat for a minute and one that spiked thirty seconds ago look
identical once the spike has scrolled into the middle distance.

### H3 — Two more temperature sources, and both are tested

Reading the CPU package sensor means reading an MSR, which means a kernel driver.
The only honest way to have one is to use the driver the user already installed,
so the chain now asks three tools rather than one:

| Source | Row says | How |
|---|---|---|
| LibreHardwareMonitor / OpenHardwareMonitor | `package` | WMI — was already there |
| **HWiNFO** | `hwinfo` | shared memory — **new** |
| **Core Temp** | `core temp` | shared memory — **new** |

Then the ACPI thermal zone as before. The two new ones are a `memcpy` out of a
mapped view rather than a WMI round trip, so they are re-checked on every pass
and starting either tool mid-session simply upgrades the reading. GPU gained
HWiNFO behind NVML and ADL.

**Neither tool is installed on this machine**, so a reader for a struct nobody
here publishes would have shipped having never run. `tempprobe.bat --selftest`
publishes synthetic blocks of exactly the documented shape and asks the real
probe what it sees:

```
  core temp : ok    (read 51.5 from core temp, wanted 51.5)
  delta form: ok    (read 59.0, wanted 59.0)
  hwinfo    : ok    (read 63.5 from hwinfo, wanted 63.5)
  bad magic : ok    (fell through to thermal zone)
```

The second case is Core Temp's option to report how far *below* TjMax each core
is rather than its temperature — handled wrongly it reads as 41 °C when the CPU
is at 59. The third contains a *voltage* of 1.2 and a drive temperature of 38
alongside the CPU package value, so a reader that takes the first temperature it
sees, or ignores the type field, fails it. The fourth publishes a block with the
wrong signature and checks it is refused rather than read as though the layout
matched — a block somebody else wrote is another program's memory, and every
field goes through a range check before it is believed.

Live on this machine: `cpu 27.9 C source: thermal zone` — no tool running, so the
fallback, correctly labelled — and `gpu 59.0 C source: nvidia`.

## Verification, second round

- `build.bat` — clean at `/W4`, no warnings in project code.
- MSVC `/analyze` over `appicon`, `search`, `launcher`, `thermal`, `sysinfo`,
  `monpaint`, `monitor`, `theme` — **zero** warnings in project code.
- `tests\run.bat` — **158 passed, 0 failed**.
- `tests\tempprobe.bat --selftest` — **4 of 4 passed**.
- `tests\launchshot.bat`, `tests\monshot.bat`, `tests\uishot.bat` — all render,
  and every capture was looked at, the core strip at 8× as well as 1:1.

Still not verified by me: live behaviour on your desktop, for the same reason as
the first round. The new things to watch are the first search after a fresh
install — icons arrive over a few seconds and then never again — and the CPU
segment strip, which needs a second sample before it appears at all.

---

# Third round — arranging, the overlay's order, setup, and every drive

Five requests. Four of them are features; the first is a performance question,
and it turned out to have two separate answers with a measurement behind each.

## I. Arranging: where the time actually went

### I1 — Four cross-process calls per window per event, to ask one question

`ApplyPlacements` is the end of every retile pass. For each window in the plan
it wanted three things: where the window is now (for the attempt record), where
it is now (for the animation's starting rect), and how thick its invisible
resize frame is (for the animation's padding). Those are two functions,
`VisibleRect` and `WindowFramePad`, and **both of them ask DWM for the extended
frame bounds** — a cross-process call each. A window that had to be un-maximised
asked a fourth time.

So a board of ten windows spent forty round trips per window event answering one
question ten times.

**Fix.** `MeasureWindow` returns the visible rect and the frame padding from one
`GetWindowRect` and one `DwmGetWindowAttribute`, and `ApplyPlacements` measures
the whole plan into a local vector before it decides anything. Four calls per
window become one. The non-animated path also stopped calling `PlaceWindow`,
which measures the frame *again* internally, in favour of `PlaceWindowFast` with
the padding already in hand.

The one place that still measures twice is marked: un-maximising a window moves
it, so a window that had to be restored is re-measured afterwards. Recorded as
invariant 47.

### I2 — 35 ms of debounce on every single window event

`RequestRetile` armed a 35 ms timer. That debounce is there to coalesce a burst —
an application opening four windows at once, a workspace's worth of windows
being un-hidden — and it was also being paid by the ordinary case, which is one
window opening on a desktop that has been still for several seconds. It was the
largest single delay between the event and the layout moving.

**Fix.** A request that arrives after 200 ms of quiet is *posted* rather than
timed: `WM_AWA_RETILE` runs on the very next trip through the message loop, once
the current event has been dealt with. A second request inside that window falls
back to the debounce exactly as before, so bursts still coalesce.

Posted rather than called, deliberately: `RetileNow` ends in `EndDeferWindowPos`,
which pumps messages while it talks to other processes, and running that inside
a `SetWinEventHook` callback is how reentrancy bugs are made. The timer is still
armed alongside the post, as the backstop for a message queue busy enough that
the post is still sitting in it when the next burst arrives.

## J. The monitor: the readouts move

The panel drew its eight readouts in the order they happened to be declared, so
CPU was always first and Network always last however little you cared about
either.

`Config::monOrder` is now a permutation of the eight, stored **by name**
(`monitor_order = cpu, gpu, ram`) for the same reason `monitor_theme` is —
invariant 9. Which readouts are *shown* stays a separate set of flags, so hiding
one and bringing it back puts it where it was rather than at the end.

- **In the overlay's own menu**, each readout opens onto its own tick and four
  moves: up, down, to the top, to the bottom. The parent item carries the tick,
  so which readouts are on is still visible without opening all eight.
- **On the Monitor tab**, the eight rows are now addressed by *slot* rather than
  by metric — `IDC_MON_SHOW_FIRST + n` is the nth row on screen — with **Move
  up** and **Move down** under them. The ids they replaced were per-metric and
  not in metric order, so `IDC_MON_CPU + 3` was Disk. That is exactly the trap
  positional rows would have walked into, which is why they were renumbered
  rather than reused.
- **`MonitorNormaliseOrder` repairs rather than rejects.** A hand-edited file can
  name a metric twice, leave three out, or name one that no longer exists, and
  the overlay still has to index eight arrays by it. Unknown and duplicate
  entries are dropped and whatever is missing is appended in the declared order.
  Asserted in `tests\run.bat` against a deliberately broken order.

Five places — the config parser, the config writer, the row builder, the context
menu and the settings page — were writing out the same eight metric names
separately and had to agree. They are one table in `montheme.cpp` now, which is
where this project keeps its tables of data.

**And the panel itself moves.** *Move to* in the right-click menu offers the nine
places a panel is ever actually wanted: four corners, four edges, the middle — of
whichever screen it is already on, not the primary one.

## K. A General tab

Setting the *application* up was scattered: three startup boxes on the Behaviour
tab, the config file behind a button on the shell, the diagnostics report and the
hidden-window safety net in the tray menu only, and no way at all to start over.

**General** collects them: starting with Windows, opening the config file or its
folder, reloading from disk, the diagnostics report, showing every hidden window,
restoring defaults, and a short About block saying which version this is, where
it is installed, whether it is elevated, and how many windows it is not allowed
to touch.

The four buttons in the middle act immediately rather than on Apply. Every one of
them is something you press *because* something looks wrong, and making the fix
wait for a second button is the wrong shape for that. **Restore defaults** is the
one control in the settings window that asks first, because it discards
keybindings, exclusions and learned window limits with no undo.

The tray menu keeps every one of these. They are the same functions, exported
through `app.h`, not a second implementation.

## L. Search: every drive, and the executables inside apps

### L1 — It only ever looked at the Windows drive

The second round added Program Files, `Program Files (x86)` and
`%LOCALAPPDATA%\Programs`. All three are on C:. A game library on D:, a tools
folder on E:, anything unpacked onto a second disk — none of it was reachable,
and that is precisely where the large things live.

Every **fixed** drive is now walked for programs. Fixed only: `GetDriveType`
answers without touching the disk, so a USB stick is never spun up, a network
drive is never listed over the wire, and a mounted phone never hangs the walk.
The top level of a disk is skipped where it is Windows' own business — `Windows`,
`Users`, `ProgramData`, `$Recycle.Bin`, and a dozen more.

### L2 — Which cost 143 seconds, until it didn't

A game's executable is buried:
`D:\SteamLibrary\steamapps\common\Game\Binaries\Win64\game.exe` is six folders
down. Reaching it meant a depth of six, and a depth of six applied to a whole
disk means enumerating every folder six levels deep on a drive full of media.

Measured here: **143 seconds**, against 20.5 for the old C:-only walk, for 168
extra entries. Terrible value.

The fix is that three of those six folders are not *part of* anything — they are
filing. `ProgramContainerDirectory` names them — `Program Files`, `Games`,
`SteamLibrary`, `steamapps`, `common`, `Epic Games`, `Battle.net` and the rest —
and descending into one does not count against the budget. Four levels of real
depth then reaches that same executable while a media tree still stops after
four.

Measured on the same machine, same drives: **20.9 seconds**, against 20.5 for the
old walk that only looked at C:. Three drives for the price of one, and it is
cached with everything else so it costs that once.

`tests\searchprobe.bat` is new and is how those numbers were taken: it runs the
index alone, with no window and nothing launched, and prints what it found broken
down by drive. The walk is the one part of the search bar whose correctness
depends entirely on the machine it is running on, and there was no way to look at
it that did not involve starting the tiler.

### L3 — And the helper executables, when you want them

`NoiseExecutable` and `SkipProgramDirectory` hide `unins000`, `crashpad_handler`,
`vcredist` and a toolchain's whole POSIX userland, for the reasons the second
round set out. *Include an app's own helper .exe files* on the Search tab drops
those rules. Off by default — they are numerous and almost never what anybody
meant — and available, because "show me every exe in that folder" is a real thing
to want when the one you need is a helper.

The cache signature is `v4`. It gained the drives, the program ceiling and
deep-exe mode; and it is bumped by hand because the walk's *depth* and its list
of free folders are constants in the source rather than settings, so nothing else
in the signature moves when they are edited. An old cache would otherwise be
served forever as though it were current — which happened once during this work,
and is why the note is there.

## M. Five more things that make it feel like Hyprland

| | |
| --- | --- |
| **`togglesplit`** — `Alt+E` | Flip the split the focused window sits under between side by side and stacked. Dwindle splits the longer edge, which is right nearly always and wrong for the one pair in front of you; the alternative was dragging windows about until the shape came out. |
| **`swapsplit`** — `Alt+Shift+E` | Exchange the two halves of that split, *subtrees and all*. One window on the left and three stacked on the right become three on the left and one on the right — which swapping one window for another cannot express at all. |
| **`focuslast`** — `Alt+grave` | Back to the window that had focus before this one, and again to come back. Alt+Tab that stays inside the tiling, never wanders onto another workspace, and never has to be held down. |
| **`workspacerel`** — `Alt+Ctrl+.` / `,` | Next and previous workspace, skipping the empty ones. Hyprland's `e+1`, and it wraps, because nine workspaces in a ring is what the key is for. |
| **mod + drag** | Hold the modifier and drag anywhere on a window to move it; right-drag to resize it from the nearest corner. |

The last one is the one worth explaining, because the obvious implementation is
the wrong one. It does **not** run a drag loop: it posts the target the same
`WM_NCLBUTTONDOWN` / `HTCAPTION` that a real click on a title bar delivers, so
Windows runs its own modal move loop and the tiler sees the ordinary
`MOVESIZESTART` / `MOVESIZEEND` pair it has handled since 1.0. The drop
indicator, the drop side, the cross-monitor hand-off and the learned size limits
all come for free, and there is one drag implementation to keep correct rather
than two.

Three things follow from that and are load-bearing, all recorded as invariant 48:

- **The button-up is never swallowed.** A `WH_MOUSE_LL` hook returning non-zero
  keeps the message from every application *including* the move loop, so eating
  the release would leave the window stuck to the pointer. Only the press is
  eaten, so the application underneath does not also see a click — without which
  mod+drag on a browser follows a link on the way to moving the window.
- **`ModDragBegin` checks the button is still down.** The hook posts and returns.
  If the UI thread was busy the release can arrive first, and then the loop waits
  for a button-up that has already been and gone. Same symptom, reached the other
  way round.
- **The hook decides membership itself, from a published set.** It has to answer
  "swallow or not" before anything can look at the window, so the manager hands
  it the windows it is arranging. Without that the gesture would eat mod+click
  everywhere on the machine — including on the applications the user *excluded*,
  which is the one place the exclusion list was supposed to protect.

The hook lives on a thread of its own, like the keyboard one and for the same
reason, and its callback returns immediately for every message that is not a
button press — which is every `WM_MOUSEMOVE`, by far the most of them.

## Verification, third round

- `build.bat` — clean at `/W4`; no warnings in project code.
- `tests\analyze.bat` — new; runs MSVC `/analyze` over whichever sources you
  name. Run over `wm`, `moddrag`, `search`, `monitor`, `montheme`, `config`,
  `defaults`, `settings`, `settings_keys`, `settings_search`, `layout`, `winutil`
  and `main`: **zero** warnings in project code. (`main.cpp` reports C28251 about
  `wWinMain`'s SAL annotation, which predates this pass and is about the
  signature the SDK declares, not about anything here.)
- `tests\run.bat` — **192 passed, 0 failed**, up from 158. The new ones cover
  `ToggleSplit` and `SwapSplit` as geometry, the config round trip for
  `mod_drag`, `search_drives`, `search_deep_exe`, `search_max_programs` and the
  readout order, and `MonitorNormaliseOrder` against a deliberately broken one.
- `tests\uishot.bat` — all seven tabs render, General included, and every capture
  was looked at. The harness now asserts its own tab list against `PAGE_COUNT`,
  so a tab added later cannot quietly stop being captured.
- `tests\searchprobe.bat` — 5,845 entries in 20.9 s across three drives;
  `--no-drives` 5,710 in 20.5 s; `--deep` 6,572 in 20.6 s. Programs found on C:,
  D: and E:.
- `tests\monshot.bat` — every style and skin still renders.

**Not verified by me: live behaviour on your desktop**, for the same reason as
the first two rounds. The new things to watch on the first real run are the
mod+drag gesture — it is the only change here that takes a global hook, and the
Behaviour tab turns it off — and the first search after this build, which
rebuilds the index once because the cache signature moved to `v4`.

One test I could not write: whether the mod+drag hook interacts badly with an
application that has its own meaning for `Alt`+click. It only ever fires over
windows ProWindows is arranging, so putting such an application on the **Never
arrange these apps** list is the answer, and that is worth knowing before it
comes up rather than after.
