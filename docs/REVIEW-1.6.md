# ProWindows 1.6 — pre-release review

The seventh pass over `src/`, after [REVIEW-1.5.md](REVIEW-1.5.md). §A is the release review of
the 1.5 rework as committed (`90e3f3f`), done for PLAN-1.6 task 1.1: `modal.cpp`, `rowlist.cpp`,
`settings.cpp` (capture hook, close and Apply paths, the merge), GDI balance in `theme.cpp`,
`main.cpp`'s idle trim, `launcher.cpp`, `appicon.cpp`. Nothing REVIEW-1.5 settled is repeated.
§B (the Requiem reskin) and §C (optimisation numbers) follow as those phases land.

Items marked ✔ were fixed in place (each a few lines); the rest are tasks for `builder` (1.2).

---

## A. Review findings

### A1 — P2 — "Reset to defaults" could reset a different category from the one it named ✔
`settings.cpp:1037` (`ResetPage`)

`ResetPage` read `g_page` again *after* `ui::Confirm` returned. The modal's loop keeps the tray and
the overlays live, and *Monitor settings…* / *Clock settings…* / *Shortcuts…* from either call
`SettingsOpenTab`, which switches category behind the question. Trigger: Layout, *Reset to
defaults*, leave the question up, right-click the monitor overlay → *Settings*, then *Reset*: the
Monitor category's unapplied edits are thrown away and the toast says "Layout reset".

**Fix.** The page is captured before the question and that one is reset (`const int page = g_page`).

### A2 — P2 — A taken chord, confirmed after an outside refresh, can erase the wrong binding or index past the end ✔
`settings_keys.cpp:109` (`Assign`), via `settings.cpp ConfirmChordFree`

`Assign` receives `index` and `ConfirmChordFree` computes `clashes` *before* the "That key is
taken" question; both are used after it to `erase` from `Edit().binds`. While the question is up,
`SettingsRefresh` can run (tray *Reload settings*, `prowindowsctl reload`, an overlay menu change -
all reach `ReloadConfig` / `AppRefreshSettings`), and when the user had not yet touched any
binding `Rebase` replaces `g_edit.binds` with the live list. A config file edited by hand to have
fewer bindings then makes `binds.erase(begin() + j)` run past the end (crash); an equal-length but
reordered list erases and overwrites the wrong bindings. Rare, but it is memory corruption.

**Fix (S).** Snapshot `Edit().binds` before `ConfirmChordFree`; after a yes, if the list is no longer
equal (`BindsEqual`, expose it from `settings.cpp`), recompute `clashes` and the target index from
`(action, arg, nth)` / the launcher ordinal, or simply bail and `SettingsRebuild()`. A few lines.

### A3 — P3 — The capture is held by row *index*, so a rebuild during it records onto another row
`settings.cpp:1143` (`BeginCapture`), `:1188` (`CaptureKey`)

`g_captureRow` is an index into `g_list.Rows()`. Losing the foreground or clicking cancels it, so
the only rebuild that can land mid-capture is one from outside with the window still in front -
`prowindowsctl reload` from a script, or a `SettingsRefresh` from IPC. If that adds or removes a
row above the listening one, the chord is `set` on whatever row now sits at that index. Inv. 79
says identity is the row's `id`.

**Fix (S).** Keep `g_captureId` (the row's id) beside the index and resolve it again in
`CaptureKey` / `CancelCapture` (and in `RowList::SetRows`, which already clears an out-of-range
`captureRow_`); drop the capture when the id is gone.

### A4 — P3 — Clicking a new launcher's own row while it waits for a key activates a different row ✔
`settings.cpp:1345` (`MouseDown`)

Adding a launcher starts a capture on its keyless row. A click anywhere cancels first, and
cancelling a keyless launcher removes its row (inv. 83) - synchronously, so the rows below move
up. The same click then went on to `g_list.MouseDown` at the old position and pressed whatever had
moved under the pointer: usually *Add an installed app* (the picker opens on mouse-up) or the next
launcher's key (a capture starts on it). The same fall-through made a click on the footer's
`Esc Cancel` prompt also run Esc, which then left the list for the categories.

**Fix.** After cancelling, the click stops there if the rows changed or it was on a prompt.

### A5 — P3 — A bare Alt tap put the settings window into an invisible menu loop ✔
`settings.cpp:1586` (`SettingsProc`)

The window has `WS_SYSMENU | WS_CAPTION` (for the taskbar and Alt+Space) but draws no caption.
Alt pressed and released with nothing between reaches `DefWindowProc` as `SC_KEYMENU` with
`lParam` 0, which enters the system menu's keyboard loop: nothing is drawn, the next ↓ opens the
window menu at the top-left, and letters beep. It also follows every Alt chord *recorded* by the
capture hook - the hook swallows the key, so the window sees a bare Alt down and up.

**Fix.** `WM_SYSCOMMAND`: `SC_KEYMENU` with `lParam == 0` is swallowed. Alt+Space and Alt+F4 are
unaffected. (Reasoned from `DefWindowProc` behaviour; `clickprobe` cannot press Alt - try it once
by hand.)

### A6 — P3 — Prompts that promised the wrong thing (inv. 74) ✔ / S
- `settings.cpp:834` ✔ - in the footer zone the prompt read `Esc Close`, but Esc there goes back
  to the categories. Now *Close* only from the categories, *Back* elsewhere.
- `modal.cpp:228` ✔ - the picker's `Esc Cancel` cleared the filter instead while anything was
  typed. It now reads *Clear* until the filter is empty.
- `settings.cpp:823` (S) - a click on `← → Change` sends `VK_RIGHT`; on a Toggle that is `Step(+1)`,
  which sets the second word rather than flipping (`rowlist.cpp:349`), so a second click does
  nothing. Send `VK_RETURN` when the focused row is a Toggle. Moot once 2.5 draws toggles as
  two-option Choices, if those wrap.

### A7 — P3 — A listening Keys row repaints the whole window at 66 Hz until a key comes
`rowlist.cpp:676` (`Tick`), `settings.cpp` `kTimerAnim`

`Tick` reports "moving" for as long as `captureRow_ >= 0`, so the 15 ms timer invalidates the
entire client - backdrop blit, the per-pixel amber `Glow` - for the whole wait, at any window size.
Fold into 3.2: invalidate only the capture row's rectangle, at the pulse's own rate (~30 Hz).

### Checked and found sound
- **`modal.cpp`** (inv. 82): owner disabled and re-enabled before destroy; `WM_QUIT` re-posted; no
  path destroys the owner while a modal runs (`SettingsHide` only after `SettleBeforeClose`,
  `SettingsDestroy` only after the main loop); no nested modal is reachable (nothing in `main.cpp`
  or the search bar opens one); Alt+N on a modal is refused (no `WS_MINIMIZEBOX`), Alt+Q answers it
  as Esc. `Pick` row callbacks capture `Modal*` on the caller's stack, which outlives the loop.
- **`rowlist.cpp`** (inv. 79): every `activate` / `set` / `remove` / `extra` / `move` is copied before
  it runs and no `Row&` is read afterwards.
- **Inv. 78**: every `Edit()` field the categories write is in `AWA_EDITED_FIELDS` (scripted
  comparison); `binds`, `monOrder`, `monColor`, autostart and the elevated task are merged by hand.
- **Capture hook** (inv. 81, 85): installed only while listening, removed before `set`, `MouseUp`
  reads `g_pressed` before `ReleaseCapture`.
- **GDI**: every `CreatePen` / `CreateSolidBrush` / `CreateCompatibleDC` / bitmap in `theme.cpp`,
  `settings.cpp`, `modal.cpp` and `launcher.cpp` is selected out and deleted on every path; GDI+
  objects are scoped; `SaveDC`/`RestoreDC` pair up. Fonts are one set per DPI seen, freed at exit.
- **Idle trim** (`main.cpp:1052`, inv. 67): skipped while settings, the search bar or a drag is up;
  `SettingsHide` / the search bar's idle release re-arm it.
- **`appicon.cpp`**: unchanged since `87a8084`; no new findings.

---

## C. Optimisation numbers (Phase 3)

The running tiler was not started for these (it rearranges the desktop), so there are no
private-bytes / GDI-object columns for the live app; the figures are from harnesses and a
`RowFocus` micro-bench linking only `theme.cpp`.

- **3.1 backdrop.** Already gone before this pass: no `RenderBackdrop` or backdrop cache remains
  in `src/`, inv. 72 is marked retired in MAP. Nothing changed; the ~33 MB (4K) allocation no
  longer exists.
- **3.2 focus.** The 15 ms `kTimerAnim` already kills itself when `g_list.Tick()` reports nothing
  fading (and the page-in/toast windows are over). New: `Glow` tells `BlendLayer` which span of
  its layer is fully transparent (deeper than `inner` from every edge) and the per-pixel loop
  skips it. `RowFocus` at 840x60, 96 dpi, t = 0.7, 300 frames: **0.558 ms to 0.463 ms** per
  frame; the pixels are byte-identical (same checksum before and after).
- **3.3 icon cache.** Already landed: `kCacheVersion` 4, one size (the majority size) per save,
  `last_used` stamp per entry, entries older than 30 days dropped on save. `tests\iconcache.bat`
  green.
- **3.4 mouse.** Already landed: `main.cpp` installs an out-of-context `SetWinEventHook`
  (`OBJID_CURSOR`) only while focus-follows-mouse is on; each event arms the one-shot
  `TIMER_MOUSE`. No 120 ms poll remains.
- **3.5 meters.** The 1 s `kTimerMeters` ran for the whole life of the settings window, with an
  `EnumWindows` on every tick. Now it is started by `WM_ACTIVATE` (active, not minimised) and
  killed on deactivate and on destroy; a tick reads the cached getters only (`ReadMeters(false)`);
  the top-level window count is taken once per activation. An unfocused settings window runs no
  meter timer at all.
- **3.6 binary.** `PW_dev.exe` 1,132,032 bytes: `.text` 805 KB, `.rdata` 267 KB, `.data` 35 KB,
  `.pdata` 23 KB, `.rsrc` 6 KB. Top `.text` contributors from the `/MAP` (symbol-gap sizes, so
  approximate): `wm` 66 KB, `settings` 56 KB, `main` 39 KB, `config` 39 KB (`Config::LoadFromFile`
  alone 12.6 KB), `timer` 38 KB, `rowlist` 35 KB, `launcher` 33 KB, `clockpaint` 32 KB,
  `settings_pages` 31 KB, `monpaint` 29 KB; CRT float parse/format (`atof` 7.8 KB, `pow` 5.8 KB,
  `cfout`) is the largest foreign block. Nothing obviously dead; no change made.