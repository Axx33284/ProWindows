# ProWindows 1.1 — review and fixes

Reviewed all 15,854 lines of `src/`. Every finding below was traced through the real
control flow, and every one has been fixed. Verification at the bottom.

Priority key: **P1** = caused the reported symptom or corrupted state · **P2** = real
bug, lower frequency · **P3** = robustness.

---

## A. "Sometimes it doesn't arrange until I move the window"

Five independent causes, all fixed. A1/A2 are the same root problem and accounted for
most of it.

### A1 — P1 — A window rejected at creation time was never looked at again ✔
`winutil.cpp` · `wm.cpp`

`EVENT_OBJECT_SHOW` was the only chance most windows got. It ran `AddWindow` → `Classify`,
and `Classify` rejects on conditions that are **transient during window creation**: still
cloaked by DWM, no title yet, not visible yet, `WS_THICKFRAME` not applied yet, briefly
borderless-fullscreen. `AddWindow` returned false and nothing ever scheduled another look.
Electron (VS Code, Discord, Slack), Chrome, Steam, Qt and JetBrains all create the HWND
first and set the title afterwards.

**Fix.** `Classify` now reports *why* it refused — `IgnoreReason::Transient` vs
`Permanent` — written out at each rejection so a new rule cannot be added without
deciding which kind it is. A transient refusal puts the window on a `pending_` list for
up to 10 second looks at 200 ms (`TIMER_PENDING`), capped at 64 entries. The timer runs
only while the list is occupied, so an idle desktop stays idle.

### A2 — P1 — `EVENT_OBJECT_NAMECHANGE` was not subscribed ✔
`main.cpp`

That is exactly the event Windows sends when an application finally sets its title — the
moment the most common A1 rejection stops applying.

**Fix.** Subscribed (`0x800C`). The handler answers from a single hash lookup unless the
window is one already on the retry list, which matters: a desktop produces a lot of title
changes.

### A3 — P1 — Windows opened during "game mode" were lost permanently ✔
`wm.cpp`

While `gameMode_` was set, `OnWinEvent` dropped everything except DESTROY and
`OnForeground` returned before `AddWindow`. On exit, `UpdateGameMode` called only
`RequestRetile()` — never a rescan. Not rare: `QUNS_BUSY` and any borderless full-monitor
window trip game mode, so fullscreen video does it too.

**Fix.** `UpdateGameMode` runs a full `ScanExistingWindows()` when the screen is released.

### A4 — P2 — `OnMoveSizeEnd` did not adopt an unknown window ✔
`wm.cpp`

Moving a window did not in fact add it. What rescued you was `EVENT_SYSTEM_FOREGROUND`
firing when you grabbed the title bar.

**Fix.** It now adopts a window it has never seen — picking one up and putting it down is
the most direct way somebody says "this one".

### A5 — P1 — Real events were being thrown away during animations ✔
`wm.cpp` · found while fixing B2

`suppress_` was set around `EndDeferWindowPos`, which **pumps messages** while it talks to
other processes, and it runs on every animation frame. Any window that opened during an
animation — and an animation follows every retile — had its `EVENT_OBJECT_SHOW` dropped.
This was a second, independent path to the reported symptom.

**Fix.** See B2: `suppress_` is gone.

---

## B. Correctness and stability

### B1 — P1 — Permanent 320 ms retile loop ✔
`wm.cpp`

`LearnFromLastPass` only retired an attempt once a window reported the same rect twice
running. A window that never holds still — a media player, an app still loading — never
did, so `RetileNow` re-armed its verification timer **three times a second for the life of
the process**, each pass a full layout computation across every monitor. This contradicted
the README's idle-CPU claim outright.

**Fix.** Two bounds. `kSettleLooks` (6) retires a single window that will not settle;
`kMaxVerifyChain` (8) bounds how many passes the loop may ask for without an outside
event. `RequestRetile`, and any `RetileNow` that is not the loop calling itself back
(tracked by `verifyPass_`), reset the budget — so responsiveness is untouched and the cap
only ever stops the loop talking to itself.

### B2 — P1 — `suppress_` did not suppress anything ✔
`wm.cpp` · `MAP.md` invariant 1

The hook is `WINEVENT_OUTOFCONTEXT`, so callbacks arrive asynchronously, long after the
stack-scoped `Suppressor` has gone. It never suppressed what it aimed at — and it *did*
suppress genuine events (A5).

**Fix.** Removed entirely, along with the `Suppressor` type. The real protection is
per-window and was already there, undocumented: `SetHidden` sets `ManagedWindow::hidden`
*before* calling `ShowWindow`, so our own hide is recognised by that flag; a show for an
already-managed window is a no-op however caused; placement raises no subscribed event at
all. That reasoning is now written down at `OnWinEvent` and in invariant 1, so the next
person to add a window-moving call knows what they owe.

### B3 — P1 — Dangling `Keybind*` across a settings Apply ✔
`hotkeys.cpp`

`g_byId`/`g_hooked` held raw pointers into `cfg->binds`; `cfg.binds = g_editBinds`
reallocates it. Safe only because nothing happened to pump a message between that
assignment and the re-registration — one added dialog and it becomes a use-after-free.

**Fix.** Both lists hold **indices**, resolved against the live vector at call time with
bounds checks.

### B4 — P2 — A stale hook index ran the wrong action ✔
`hotkeys.cpp`

The hook posted an index into `g_chords`; if the bindings were rebuilt before the UI
thread dispatched, it resolved against the new list. Bounds-checked, so no crash — it just
silently fired a different action.

**Fix.** A generation counter is bumped whenever the chord list is replaced and packed
into the message alongside the index. A stale post is dropped.

### B5 — P2 — Swallowed keys could leak into a stuck modifier ✔
`hotkeys.cpp`

If the key-up for a swallowed chord never reached the hook — release landing while an
elevated window has focus, or after the hook was taken out and put back over a reload —
the entry stayed and ate the **next** release of that key.

**Fix.** Each entry carries a timestamp and expires after 4 s.

### B6 — P2 — The thermal probe could die for the rest of the session ✔
`thermal.cpp`

If `Stop()`'s 5 s wait expired, the thread was abandoned but `thread_` left non-null — so
the next `Start()` decided a probe was already running and returned without starting one.
Temperatures stopped until something called `Start()` again, and nothing did.

**Fix.** Each run owns its own stop event and closes it on the way out, and publishes only
under the generation it started with — so an abandoned probe can be forgotten immediately
and cannot overwrite a newer one's readings. A `running_` flag, cleared by the thread
itself, keeps it to one live probe.

### B7 — P2 — Data race in `ConfigDir()` ✔
`common.cpp`

Unguarded check-then-assign on a `std::wstring`, called from the file indexer, the app
scanner, the thermal probe (via `LogLine`) and the UI thread. Concurrent construction is
undefined behaviour; it worked only because the UI thread happened to get there first.

**Fix.** A function-local static initialised once — the language guarantees every other
thread waits. Returns by `const&`, which also drops a string copy per call.

### B8 — P2 — `LogLine` was not thread-safe ✔
`common.cpp`

`g_logOn` was a plain `bool` read from four threads, and four threads appending through
separate `FILE*` handles interleaved mid-line — corrupting the one artefact you would use
to diagnose a threading bug.

**Fix.** Interlocked flag; a critical section held across the whole write. Also:
`LogEnable` no longer truncates on every settings reload, only on an actual off→on
transition — it was wiping the log out from under whoever was reading it.

### B9 — P2 — Shutdown ran twice on session end ✔
`main.cpp` · `wm.cpp`

`WM_QUERYENDSESSION` ran the full shutdown and `WM_ENDSESSION` ran it again — and if
another application vetoed the shutdown, the process carried on with the manager already
torn down.

**Fix.** `WM_QUERYENDSESSION` only answers the question. The work moved to
`WM_ENDSESSION`, gated on its `wParam`. `Shutdown()` is idempotent regardless.

### B10 — P2 — The crash handler was not crash-safe ✔
`main.cpp` · `wm.cpp`

It walked a `std::unordered_map` from an unhandled-exception filter. Since that map is
mutated on every window event, a crash part way through one would fault the handler too —
leaving the user with exactly the hidden windows it exists to restore.

**Fix.** `EmergencyUnhideAll()` reads a flat fixed `HWND` array maintained by `SetHidden`,
touching no container. Hotkey/hook teardown was dropped from the handler: Windows releases
both when the process dies.

### B11 — P2 — Every shown window stole internal focus ✔
`wm.cpp`

`AddWindow(hwnd, true)` on `EVENT_OBJECT_SHOW` set `focused_` and moved `activeMonitor_`
for any newly-shown window, including background ones — pointing directional focus, swap
and move-to-monitor at a window nobody had looked at.

**Fix.** A new `AdoptWindow()` takes focus only when the window genuinely is
`GetForegroundWindow()`. `OnForeground` still takes it outright, which is correct there.

### B12 — P2 — Windows could be abandoned mid-animation ✔
`wm.cpp`

`AnimBegin()` cleared `anim_` unconditionally. A window that was in flight and not in the
new plan — because that pass parked, floated or crowded it out — was left frozen at
whatever interpolated rect the easing curve had reached.

**Fix.** In-flight entries are carried across and landed on their targets by `AnimCommit`
if the new plan does not claim them. `AnimStop` does the same for an animation cut short
by game mode or shutdown. `AnimCommit` is now called unconditionally so the carry always
drains, even if animations are switched off mid-flight.

### B13 — P3 — Retile debounce had no ceiling ✔
`wm.cpp`

`SetTimer` restarts a running timer, so a sustained event stream could postpone the retile
indefinitely.

**Fix.** The first request fixes a 250 ms deadline; later ones may only bring the moment
forward, never push it back.

---

## C. Robustness

### C1 — P3 — Background threads abandoned at process exit ✔
`search.cpp` · `winutil.cpp` · `launcher.cpp`

Both the index walk and the app scan waited 5 s and then abandoned the thread while
`wWinMain` returned — `ExitProcess` then terminates it wherever it is, possibly holding
the CRT heap lock.

**Fix.** Both walks now check a lock-free cancellation flag at each directory / each shell
item, so shutdown normally completes well inside the timeout instead of relying on it.

### C2 — P3 — Dead branch in `OnWinEvent` ✔ — folded to `if (Find(hwnd)) return;`.

### C3 — P3 — No bounds guard on the colour-swatch index ✔
`settings_keys.cpp` — correct today only because the switch enumerates exactly the eight
ids in range; guarded so a future control id added there cannot write past the array.

### C4 — P3 — Analyser warnings ✔
`CoInitializeEx` is now paired properly (an unmatched `CoUninitialize` tears the apartment
down under whatever is still using it); the two `CoCreateInstance` calls and
`CoSetProxyBlanket` check or explicitly discard their `HRESULT`.

### C5 — P3 — No tests for the layer where these bugs live ✔ (partly)
The event/lifecycle layer is deeply Win32-coupled and still has no unit tests — that would
need a stubbed Win32 seam, which is a larger change than this one and I have not made it.

What I did add is a runnable regression check in the style the project already uses.
`tests/testwin.cpp` gained three switches that reproduce the A1 rejection paths exactly:

```
testwin.exe --late-title 400        no name until 400 ms in, like Electron and Chrome
testwin.exe --late-show 400         created hidden, shown later
testwin.exe --late-resizable 400    WS_THICKFRAME applied after creation
```

Run two or three at once: they should take their tiles unaided. Before the fix they sat
where Windows put them until clicked.

---

## Verification

- `build.bat` — clean at `/W4`, no warnings in project code.
- MSVC `/analyze` across all 20 translation units — **zero** warnings in project code
  (was 11, of which 2 were introduced by these fixes and then removed).
- `tests\run.bat` — **158 passed, 0 failed**.
- `tests\testwin.cpp` compiles clean at `/W4` with the new switches.

Not verified by me: live behaviour on your desktop. Running the tiler rearranges every
window you have open, so I left that to you — `--late-title` above is the case to watch.

## What is worth keeping

- The layout engine (`layout.cpp`) is careful and correct: `ConstrainedSplit` and
  `FlexSizes` never produce overlapping or lost rects even on unsatisfiable boards, and
  the saturating arithmetic in `MeasureRec` is right. Untouched.
- Learning size limits by observation rather than trusting `WM_GETMINMAXINFO` is the right
  design; B1 was a missing cap, not a wrong idea.
- Moving the keyboard hook off the UI thread is right, and the comment explaining it is
  accurate.
- Monitor re-mapping by `HMONITOR` across display changes is handled properly, which most
  tiling managers get wrong.
