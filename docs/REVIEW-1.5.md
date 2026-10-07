# ProWindows 1.5 — the options-screen rework, and what the review turned up

A sixth pass over `src/`, after [REVIEW-1.4.md](REVIEW-1.4.md). Asked two things: review the app,
and rework its design so it looks like the options screens of *Star Wars Battlefront II* (2017) -
specifically the *OPTIONS / VIDEO* screen: a notched panel of full-width rows, `ON | OFF` pairs with
the chosen word on a pale bar, `◀ VALUE ▶` selectors, filled bars, and an amber glowing frame
round the row with focus. "A high quality professional app, used daily."

1.4 had already been drawn "after Battlefront II", but as a themed Win32 dialog: native check
boxes, combo boxes and edit fields restyled one by one over a starfield. It did not look like the
reference and could not be made to, because the reference is not made of those controls. So this
pass replaced the settings window rather than restyling it. Findings first, then the rework, then
verification.

---

## A. Review findings

### A1 — P2 — Changing the monitor or clock from its own menu threw away unapplied settings ✔
`monitor.cpp`, `clock.cpp`, `settings.cpp`

Every change made from an overlay's right-click menu ends in `AppRefreshSettings()`, which called
`LoadAllPages()`: every page re-read the live config into its controls. Anything the user had
changed and not yet applied - on any page - was silently put back. Re-skinning the clock while
halfway through rebinding shortcuts lost the shortcuts.

**Fix.** The window edits a copy, and an outside change *rebases* it: fields the user has not
touched take the new live value, fields they have keep theirs (`Rebase`). `uishot` checks it.
MAP.md invariant 78.

### A2 — P2 — Apply wrote back every field, including ones changed elsewhere meanwhile ✔
`settings.cpp`

`ApplyNow` had every page `Save()` everything it showed into the live config. Where the live value
had changed since the page loaded - an overlay pinned or dragged from its own menu without a
refresh - Apply put the stale value back.

**Fix.** Apply is a three-way merge (`MergeEdits`): a field reaches the live config only when the
edit differs from what the window started from. Every edited field is listed once, in
`AWA_EDITED_FIELDS`. MAP.md invariant 78.

### A3 — P3 — Closing the window discarded unapplied changes without a word ✔
`settings.cpp`

Hiding destroys the window (invariant 66), and nothing asked first. Now closing with anything
unapplied asks *Apply* / *Discard*, and `Esc` goes back to the settings. Changes are also visible
while they are pending: a mark on the row, a line in its description, and a count along the foot.

### A4 — P3 — Right-aligned spaced capitals lost their last letter ✔
`theme.cpp`

The search bar's kind tags read `PROGRA` and `ANSWE` in 1.4's own screenshots. `DrawText`
centres and right-aligns by the width of the text *without* `SetTextCharacterExtra`, while drawing
it with the extra; the shift `DrawSpaced` applied could not make up for it. Spaced text that is
centred or right-aligned is now placed from its measured width and drawn left-aligned. MAP.md
invariant 77 said the opposite and has been corrected.

### A5 — P3 — A cleared window shortcut could not be given a key again ✔
`settings_keys.cpp`

*Remove* on the Keys page deleted the binding, and with it the action's only row; the only way
back was *Reset all keys*, which also undid every other change. The Shortcuts category now lists
every action that has a default, whatever is bound, so a cleared one stays as **NOT SET** and takes
a new key with Enter.

### A6 — P3 — The virtual-desktop manager was re-created on every event while it could not be ✔
`winutil.cpp`

Left open by REVIEW-1.4 A7. When the *fresh* `CoCreateInstance` in `IsOnCurrentVirtualDesktop`
failed, `retryAt` was not set, so every window event paid for another activation until the shell
answered. It now backs off five seconds, like the first attempt.

### A7 — P3 — Settings that existed only in the config file ✔
`config.h`, the new categories

`smart_gaps`, `cursor_warp`, `active_color`, `inactive_color`, `corners`,
`resize_step`, `master_count`, `monitor_cores`, `monitor_smooth`, `search_programs` and `debug`
were all live settings with no way to change them except by hand. Each is a row now.

### A8 — P3 — Stale numbers in the README ✔
The executable is 1.0 MB, not 850 KB. Every "tab", "tick the box" and dialog button name in the
README has been brought up to date with the new window.

### Not changed
- `explained_` in `wm.cpp` is still cleared only at 512 entries (REVIEW-1.4 A7). Diagnostic output
  only.
- The tray menu is still the system's popup menu, dark through the app-mode opt-in (invariant 39),
  not drawn in the new look. Owner-drawn menus keep a system-drawn frame and submenu arrow; doing
  it properly means a menu window of our own, which is a change of its own.

---

## B. The rework

### One window, no controls
`settings.cpp`, `rowlist.*`, `theme.*`, `modal.*`

The settings window is a single custom-drawn window with no child controls and no dialog template
(`res\app.rc` keeps only the icon, manifest and version). That is what lets every pixel be the
game's, lets the keys work the way the game's do, and makes it cheap to build from nothing when the
tray icon is clicked. MAP.md invariants 72, 80.

- **Frame.** It draws its own: the whole window is client area, the header drags it, every edge
  resizes it, the corners are square, a one-pixel DWM margin keeps the shadow. Minimum 980 × 620
  DIPs; per-monitor DPI throughout.
- **Header.** The badge (the icon's plate) and a chip between two bars - *◆ TILING · 7 WINDOWS* -
  where the game shows the player's card and level; then the breadcrumb **PROWINDOWS / LAYOUT**
  between its own bars, as in the reference. *Re-arrange* and *Pause tiling* at the right.
- **Categories** down the left: a search field, then the eight categories. The current one sits on
  the same pale bar as a chosen word; with the keyboard in the column it is lit amber.
- **The panel**: a hairline frame with a notch dipping into the top right of centre and one rising
  from the bottom left, as the game's does. Inside, rows separated by hairlines, grouped under
  small spaced headings. It scrolls; rows fade where they run under the edge.
- **Rows** (`ui::Row`): *Toggle* (two words, the chosen one on a pale bar - `ON|OFF`,
  `STACKED|IN A ROW`, `24-HOUR|12-HOUR`), *Choice* (`◀ VALUE ▶`), *Slider* (a bar filled from the
  left, the value in the middle, dark over the fill and light over the groove), *Keys* (the chord as
  keycaps), *Colour* (`◀ ■ AMBER ▶`, Enter for any colour), *Action* (a cut-corner button in the
  control column), *Item* (an entry in a list the user builds - an excluded app, a folder, a
  launcher), *Info*.
- **Focus** is the only colour: an amber frame round the whole row with the glow bleeding out of
  it, a warm wash growing towards the control, the name amber, the chosen word on an amber bar and
  a pool of light behind it. It fades in over 90 ms and out over 150. Disabled rows are grey, their
  chosen word on slate - the reference's *FULLSCREEN MONITOR* and *ENABLE DYNAMIC RESOLUTION*.
- **Description** beside the panel: the focused row's name, an amber rule, *CHANGED · NOT APPLIED
  YET* when it is, and what it does in plain English. Layout, Monitor and Clock show a live preview
  above it, drawn by the real painters from the edit copy.
- **Footer**: *Apply*, *Reset to defaults* (this category only), *Close* as cut-corner buttons,
  the count of unapplied changes or the last message, and on the right the button prompts for
  whatever has focus - clickable. MAP.md invariant 74.

### Keys, the game's way
`↑↓` move, `←→` change (and `←` from a row with nothing to change goes back to the categories),
`Enter`/`Space` select, `Delete` clear or remove, `Ctrl+Tab` / `Ctrl+PgDn` between categories,
`Tab` between categories, settings and buttons, `Ctrl+S` apply, `Esc` back then close. Typing
anywhere searches every setting in every category; the results are the real rows. The pointer
lights whatever it is over, once it has actually moved (invariant 84).

### Shortcuts recorded in place
A Keys row takes its chord where it is shown: Enter, and the row breathes *PRESS A SHORTCUT*,
showing the modifiers as they are held. The low-level hook that made the 1.4 recorder work for
`Win` chords is the same one, now owned by the settings window and installed only while a row is
listening (invariant 81). A chord another binding has asks before moving; bare keys other than
F-keys and media keys are refused; a launcher added from the Apps category asks for its key at
once and goes again if the capture is cancelled (83).

### Modal screens
`ui::Confirm`, `ui::Ask`, `ui::Notice`, `ui::Pick` replace every `MessageBox` in the settings path
and the old app picker and shortcut editor dialogs: a black plate, a titled heading with an amber
rail, cut-corner buttons and prompts; the picker filters as you type and its entries are rows. The
owner dims behind them (invariant 82). *Restore every default* in `main.cpp` uses one too.

### The search bar
Black, a hairline frame, a short amber rule at the top left, hairlines between results, the
selected result lit exactly like a settings row, kind tags in spaced capitals, prompts along the
foot. Its behaviour is unchanged.

### The icon, and the overlays
The icon is the tile mark - master pane amber, the others white and grey - on a near-black plate
with its top-right corner cut off like the window's buttons (`res\gen_icon.py`); the header draws
the same plate. Both overlays gain **Frontline** (black glass inside a hairline, white type, amber
for the processor or the second hand), first in their tables and so the default for new configs;
existing configs name their skin and are untouched (invariant 9).

### What went
`IDD_*` templates and every `IDC_*` id; `PrepareDialog`, `DialogMessage`, the card bookkeeping,
the pattern-brush surfaces and their phasing, `ProWindowsNav`, owner-drawn buttons, combo boxes,
check boxes and list views. `tests\bindshot` and `tests\rowdragshot` drove those controls with the
real keyboard and mouse; what they proved is now checked through the row engine by `uishot`
(a chord posted exactly as the hook posts it; reordering by `Ctrl+↑↓` and the grip). Invariants 7,
20, 38, 73, 75 and 76 are retired with pointers.

Nothing here uses Star Wars artwork, logos or insignia: the look is layout, type, colour and
procedural drawing.

---

## C. Verification

- `build.bat PW_dev.exe`: builds clean at `/W4` (only the SDK's own GDI+ header warnings, as
  before). 1.0 MB.
- `tests\analyze.bat` over every new and rewritten source: clean after two fixes (a null-window
  path in `SettingsOpen`, an unguarded button index in the modal's prompt layout).
- `tests\run.bat`: **212 passed, 0 failed**.
- `tests\uishot.bat`: every category captured, then by messages sent to the window: only the row
  with the keyboard is lit amber (pixels read back), a changed switch is an unapplied edit, `Ctrl+S`
  applies it and leaves nothing unapplied, an outside change arrives without losing an unapplied
  edit (A1), undoing an edit by hand leaves nothing to apply, `→` steps a slider, a chord lands on
  the shortcut; captures of the categories column, `Ctrl+Tab`, a capture in progress, a recorded
  chord, search results, the Reset question and the window behind it, and the window at its
  smallest. All passed.
- `tests\launchshot.bat`, `monshot.bat`, `clockshot.bat`: search bar states, and Frontline for both
  overlays, inspected.
- `res\gen_icon.py`: 16 – 256 px; inspected at 32 and 256.

Not verified, and why:

- **The real tiler was not started**, as in 1.4: it rearranges every window on the desktop.
  `SettingsTranslateMessage` no longer does anything and `IsDialogMessage` is gone from the loop;
  that is reasoned from the code rather than observed in the running product.
- **The capture hook was not driven with real keystrokes.** `bindshot` did that for the old editor;
  the hook procedure is the same, and `uishot` posts exactly what it posts, but `Win`+`E` through
  the real hook into the new window has not been pressed.
- **Only 100% scaling was captured.** Every size goes through `theme::Scale` and the fonts are built
  per DPI, but 125%, 150% and a move between monitors of different scale have not been looked at.
- The tray menu is unchanged (A, *Not changed*).

---

## D. Follow-up: "I can't close it or hide it"

Reported against the 1.5 build: the settings window could not be closed or minimised, Brave showed
"the same problem", and Reset to defaults did nothing. A new harness, `tests\clickprobe.bat`,
presses the window's buttons with the **real** pointer. Its first run failed five of eight checks.
Everything that `uishot` covers had passed, because `uishot` drives the window with messages and
never goes through capture.

### D1 - P1 - Every button in the settings window was dead to the mouse ✔
`settings.cpp`

`MouseUp` called `ReleaseCapture()` before reading `g_pressed`. `ReleaseCapture` sends
`WM_CAPTURECHANGED` on the spot, and the handler for that clears `g_pressed`, so no header or
footer button ever fired from a click: not the X, not minimise, Apply, Reset to defaults, Close,
Re-arrange or Pause. Only the keyboard worked. It is now read first. MAP.md invariant 85.

### D2 - P1 - Alt+Q / Alt+N acted on a window the user was not looking at ✔
`wm.cpp`

`FocusedManaged()` returned `focused_`, the last *managed* window to have focus, and
`OnForeground` leaves that alone when an unmanaged window comes to the front. With the settings
window or an excluded app in front (Brave was on `ignore_process` in the reporter's log), the
close shortcut closed a different window and the minimise shortcut minimised one. The window in
front could be neither closed nor minimised from the keyboard. Close and minimise now act on the
window in front. Every other action that changes a window does nothing while an unmanaged one is
in front. MAP.md invariant 86.

### D3 - P2 - Exclusions did not apply to windows already open ✔
`wm.cpp`, `winutil.cpp`

Adding an app to *Never arrange these apps* left its open windows tiled until they closed. Taking
one off the list left its windows unarranged until they next took the focus. `ApplyConfigChanged`
now releases the windows the rules name (un-hiding any on another workspace first) and rescans for
the rest (`ExcludedByUser`).

### D4 - P2 - Reload and Restore every default kept the "lost" edits ✔
`settings_pages.cpp`, `settings.cpp`, `main.cpp`

Both asked "Changes you have not applied will be lost". The refresh that followed then rebased
those edits onto the reloaded config, so they survived. Restore also asked twice when anything was
unapplied. The edits are now discarded (`DiscardEdits`), Restore asks once, and the window sizes
learned about windows open right now are forgotten too (`ForgetLearnedLimits`). Before, they were
written straight back when those windows closed.

### D5 - P2 - Applying any setting un-paused tiling ✔
`wm.cpp`

Pause from the header, the tray or a shortcut flips only the runtime flag. Every Apply reloads the
config, and `ApplyConfigChanged` copied `tiling_enabled` from the file back over it. It now applies
the file's value only when that value changed. MAP.md invariant 87.

### D6 - P3 - Smaller things ✔
- *Put every key back* and the Shortcuts page's Reset made workspace keys for nine workspaces
  whatever the workspace count.
- The theme's DPI is shared between the settings window, its modal screens and the search bar,
  but was set only when painting. On two screens at different scales, clicks in one could be
  hit-tested at the other's scale. Every settings and modal message now sets it, and so does the
  search bar's paint.
- `ipc.cpp`: a command still running when the client's five-second wait ran out was never freed.
  Both threads now own the request, and the last one to let go frees it.

### Verification
- `tests\clickprobe.bat`: 11 of 11, with the real pointer and keyboard.
- `tests\run.bat`: 212 passed, 0 failed. `tests\uishot.bat`: all checks passed.
- `tests\analyze.bat` over every changed source: one pre-existing C28159 (`IdleMs`, whose 32-bit
  subtraction is wrap-safe).
- `build.bat PW_dev.exe`: clean.
- Not verified: the D2/D3/D5 paths inside the running tiler with Brave. They are reasoned from the
  code and from the reporter's log, not observed.
