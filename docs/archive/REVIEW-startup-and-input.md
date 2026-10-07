# ProWindows 1.2 — the startup stall, the vanishing pointer, and what it costs to run in the background

A third pass over `src/`, after [REVIEW-1.2.md](REVIEW-1.2.md). This one was asked three things:
why the machine glitches when the program starts, why the mouse pointer sometimes disappears
while typing, and what it costs to leave running. Everything below was traced through the real
control flow, and every change has either a measurement or a live test next to it.

Verification is at the bottom.

---

## A. The two reported symptoms

### A1 — P1 — "It glitches the PC at startup": three things stalled the UI thread ✔
`monitor.cpp`, `sysinfo.*`, `search.cpp`, `wm.cpp`

The UI thread is the thread that answers every window event, every shortcut, every animation
frame and the tray icon. Three things ran on it at startup, and one of them kept running:

1. **The overlay's readings.** `SystemSampler::Sample()` — `PdhCollectQueryData` on the
   `GPU Engine(*)` wildcard, `NtQuerySystemInformation`, the lot — ran from a `WM_TIMER` on the
   UI thread once a second, and once synchronously inside `MonitorSetVisible` during startup.
   Opening the GPU counter is **84 ms warm on this machine** (`scratchpad\pdhbench`); after a
   cold logon, while the counter provider starts, it is seconds. Every animation that overlapped
   a reading hitched; at startup the whole application sat frozen until the counters opened.

   **Fix.** A sampling thread (`SampleThread` in `monitor.cpp`, below-normal priority) takes the
   readings and posts `WM_AWA_SAMPLED`; the UI thread only ever moves a finished reading into
   `g_load` and paints. The thread parks on an event while the panel is hidden and lets the
   counters go, so a hidden panel now genuinely costs nothing. `Configure()` is cross-thread
   safe (wants are handed over under a lock, applied at the next reading); `Sample()` and
   `Close()` belong to the thread. The thermal probe stays under the UI thread's control — it has
   single-controller assumptions of its own — and the sampler only reads what it published.
   `Redraw` draws nothing until the first reading lands: a layered window with no frame is
   invisible, which beats a frame of "--" replaced a few milliseconds later. MAP.md invariant 51.

2. **The search index cache.** `SearchInit` parsed `index.cache` — half a megabyte through
   `fgetws`, six thousand strings — on the UI thread before the message loop had started.

   **Fix.** `StartIndex(0, true)` hands it to the index thread, which reads the cache at ordinary
   priority and only drops into background mode if it then has to walk. Invariant 55.

3. **Asking every window for its size limits.** `WM_GETMINMAXINFO` is a `SendMessageTimeout`
   with a 60 ms ceiling, and at logon every application takes the full 60 ms because it is busy
   starting. Ten familiar windows was over half a second of the tiler frozen, on every boot, to
   learn what `learnedLimits` already held.

   **Fix.** A window seeded from `learnedLimits` has `limitsAsked` set and is not asked. The
   remembered numbers are the merged declared-and-observed ones, so nothing is lost. Invariant 54.

Measured, warm, on the development machine: **config loaded → message loop running went from
317 ms to 68 ms**, with the cache landing on its thread 13 ms *after* the loop was up.

### A2 — P1 — "Sometimes hides the mouse when typing": `AttachThreadInput` ✔
`winutil.cpp`

`FocusWindow` attached this thread's input queue to the foreground thread's and the target's,
called `SetForegroundWindow` and `SetFocus`, and detached. Attached threads pool their entire
input state — keyboard state, capture, the caret, and the cursor, *including whether it is
shown*. Windows hides the pointer while the user types ("Hide pointer while typing", on by
default) and shows it again on the next movement. Attach in the middle of that, detach a moment
later, and the hidden pointer can be left hidden — in the target's queue or in ours. That is the
report, exactly. Attaching also turned `SetFocus` into a synchronous call into the other
process, so a hung application hung the tiler with it.

The fallback was worse: a synthetic **Alt press-and-release**. Alt alone moves keyboard focus
to the menu bar in Explorer and every Office application, and Alt arriving while Shift is held
is the keyboard-layout switch chord on most machines. Injected while somebody is typing, both
are visible.

**Fix.** Neither. One `SendInput` of a mouse event with no flags set — no movement, no button,
nothing any application sees — satisfies the documented "this process delivered the last input
event" condition, and `SetForegroundWindow` is then allowed. komorebi and GlazeWM arrived at
the same call for the same reasons. `SetForegroundWindow` is tried plainly first, because a
registered hotkey already grants the receiving thread foreground rights. Invariant 50.

Live test: two Notepad windows plus this Claude window; `focus left/right`, `focusnext`,
`focusprev` through the control channel each moved the *OS* foreground window (checked with
`GetForegroundWindow` from an unrelated process), from a tray process with no foreground rights
of its own.

### A3 — P2 — The hook threads ran at normal priority ✔
`hotkeys.cpp`, `moddrag.cpp`

Every keystroke and every pointer movement on the machine passes through the `WH_KEYBOARD_LL`
and `WH_MOUSE_LL` callbacks before the application it was meant for sees it. Both already had
threads of their own (the 1.2 fix), but at normal priority those threads queue behind the file
indexer, the icon sweep, a retile, and every other program on a busy machine — which at logon is
all of them. A keystroke then waits for a timeslice; typing feels sticky exactly when the
machine is under load, and a hook that is late often enough is silently removed.

**Fix.** `THREAD_PRIORITY_HIGHEST` for both. Not time-critical: the callbacks are bounded
(invariant 3), but a runaway at time-critical would starve the UI thread that acts on what they
post. Confirmed live: two threads at base priority 10 in the process. Invariant 52.

---

## B. Background cost

### B1 — The animation ticked at 250 Hz whatever the display ✔
`wm.cpp`, `winutil.*`

A flat 4 ms tick. A frame the display cannot show is still a `SetWindowPos` into every
application on the board and a relayout in each — a full one for anything Chromium-based — so
on a 60 Hz screen three frames in four were pure cost, paid by every animated application.

**Fix.** `MonitorInfo::refreshHz` from `EnumDisplaySettings`; `ReloadMonitors` folds the fastest
display into `animFrameMs_` (4..16 ms); the ticker reads it when an animation starts. Measured
on the 180 Hz development display: 31–32 frames per 200 ms animation, worst gap 6.5 ms. On a
60 Hz display this is a 75% cut in placement traffic per animation. Invariant 53.

### B2 — Focus-follows-mouse re-reported the same window eight times a second ✔
`main.cpp`

`TIMER_MOUSE` handed the window under the pointer to `OnForeground` on every 120 ms tick for as
long as it was not the foreground window — a forced fullscreen check (`SHQueryUserNotificationState`)
and a DWM border write each time, for a pointer that had not moved. It now reports a *change*
of window and nothing else. (Off in the reviewed config; fixed because it is cheap.)

### B3 — Every Settings → Apply started a full disk walk ✔
`search.cpp`

`SearchApplyConfig` decides whether a settings change needs a new index by comparing the live
config against `g_wants`. Only `StartWalk` set `g_wants`, and on a cache hit `StartWalk` was
never called — so the comparison was against an empty struct, and every Apply, of anything, on
any tab, started a walk of every configured folder. Found while moving the cache load;
`StartIndex` records `g_wants` on every path. Invariant 55.

### B4 — A child window's `EVENT_OBJECT_SHOW` bought it two seconds of second looks ✔
`winutil.cpp`

`Classify` tested visibility before it tested for a root window. A child window shown while its
parent is still hidden — a control, a tooltip, a tab strip, which is most of them — was
"transient" and went on `pending_` for ten retries. The root test comes first now; a child is a
permanent no. Invariant 56.

---

## C. Startup correctness

### C1 — Windows that opened during the first scan were nobody's ✔
`main.cpp`

`InstallHooks()` ran after `g_wm.Init()`. A window that opened during the scan and first pass —
at logon, a long window — was too late for the scan and too early for the hooks, and stayed
unmanaged until something prompted a rescan. Hooks go in first now; events raised before the
message loop are queued, and any delivered inside the pass are deferred by `Busy` as usual.
Invariant 56.

### C2 — One failed `CoCreateInstance` switched virtual desktops off for the session ✔
`winutil.cpp`

`IsOnCurrentVirtualDesktop` asked for `CLSID_VirtualDesktopManager` once. At logon — when an
autostarted copy first asks — the shell that serves it is often not up, and the failure was
remembered: every window on every virtual desktop was tiled onto the one being looked at, for
the rest of the session. It retries every five seconds until it has the object.

---

## D. Looked at, deliberately left alone

- **Focus-follows-mouse does not move the OS foreground.** `OnForeground(under)` moves the
  accent border and the target of the next keyboard action; keyboard input still goes to
  whatever Windows has focused. The setting's label says "Focus follows the mouse pointer",
  which reads as more than that. Whether it *should* steal real focus on hover is a product
  decision, not a bug fix, so it is unchanged and noted here.
- **`SetHidden` calls `ShowWindow`, which is synchronous.** A hung application on the workspace
  being switched away from holds the UI thread until the hung-window timeout. `ShowWindowAsync`
  would fix that but would break `RetileMonitor`'s dead sweep, which reads `IsWindowVisible`
  straight after an un-hide and would now see the old value. Needs its own design.
- **`MaskWinKey` injects a Ctrl press on every Win+chord**, and injected keys hide the pointer
  exactly as typed ones do. Cosmetic, and the injection is what keeps Start closed.
- **`ThermalProbe::Stop()` waits up to 250 ms on the UI thread** every time the overlay hides,
  when a temperature is on. Already documented as an accepted cost in `thermal.cpp`.
- **Startup tiles every open window at once, with animation.** That is the product; a
  "grace period" would be a feature, not a fix.

---

## Verification

| | |
|---|---|
| `build.bat` | clean, no warnings from `src/` |
| `tests\run.bat` | **212 passed, 0 failed** |
| `tests\monshot.bat --bench` | builds and renders every skin (it links `monitor.cpp` and `sysinfo.cpp`) |
| Overlay, live | first reading painted through the thread; hide → show → reload all leave it live; readings change between frames |
| Focus, live | `focus left/right`, `focusnext`, `focusprev` move the OS foreground window from a tray process |
| Hook threads | two threads at base priority 10 (`Get-Process ... .Threads`) |
| Sampling thread | base priority 7, 109 ms of CPU in total including the counter open |
| Animation cadence | 31–32 frames per 200 ms on a 180 Hz display, worst gap 6.5 ms |
| Startup, UI thread | config loaded → loop running: **317 ms → 68 ms**, warm |
| Idle, overlay on | **0.08% of one core** over 20 s, 13.2 MB private, 11 threads |

Not verified here: a real cold logon. The startup numbers above are warm; the cold-logon case
is the one the fixes are for, and the only way to see it is to boot with it.
