# ProWindows 1.4 — the Battlefront II rework, and what the review turned up

A fifth pass over `src/`, after [REVIEW-1.3.md](REVIEW-1.3.md) and
[REVIEW-icons-and-idle.md](REVIEW-icons-and-idle.md). Asked three things: review the app; rework
its look, structure and icons after the menus of *Star Wars Battlefront II* (2017); and renew
[MAP.md](../MAP.md). Findings first, then the rework, then verification.

---

## A. Review findings

### A1 — P2 — The diagnostics report used `swprintf_s` on user text ✔
`main.cpp`

`DiagnosticsText` builds every line through a small `add` lambda into a 1024-character buffer
with `swprintf_s` — and one of the lines it prints is `DescribeAction(kb)` for every binding,
which for a launch binding is `"Open  " + command`: user text of any length. This is exactly the
class of bug invariant 57 (then 14) exists for. Since 1.3 installed an invalid-parameter handler
that returns, a long command no longer kills the process; it silently blanks that line of the
report instead, which is the one place somebody looks when a binding misbehaves.

**Fix.** `_snwprintf_s(line, _TRUNCATE, ...)`. MAP.md invariant 57.

### A2 — P2 — On a DPI change the shell collected the pages' group boxes as its own cards ✔
`theme.cpp`

`PrepareDialog` walks the dialog with `EnumChildWindows`, which visits *every* descendant. On
first creation that was harmless — the shell is prepared before its pages exist — but on
`WM_DPICHANGED` the shell's walk reached into all eight pages, re-fonted their controls a second
time, and appended every page's group boxes to the *shell's* card list, measured in the page's
coordinates. Those phantom cards were painted under the pages, mostly hidden, and grew with every
DPI change for the life of the window.

**Fix.** The walker only touches `GetParent(child) == dlg`. MAP.md invariant 75.

### A3 — P3 — The file version said 1.2 while the program said 1.3 ✔
`res/app.rc`, `src/common.h`

`VERSIONINFO` was never bumped for 1.3, so Explorer's Properties → Details reported 1.2.0.0 for a
1.3.0 binary — and invariant 64 exists precisely because telling copies apart matters. Both now
say **1.4.0**.

### A4 — P3 — Two pages ran past the page area ✔
`res/app.rc`

The page area is 352 DLU tall; the Clock page's Preview panel ended at 358 and the Monitor page's
hint at 358. Invisible under the old tab frame, obvious once the prompt bar had a rule above it.
Both trimmed to fit. MAP.md invariant 76.

### A5 — P3 — The map had drifted ✔
`MAP.md`

- Invariants **40**, **42** and **43** said the same thing three times (icons never fetched on the
  way to the screen; straight versus premultiplied alpha), and **14** and **57** twice.
- "A tray process has no foreground rights … `FocusWindow` already knows the attach-thread-input
  dance" contradicted invariant 50, which exists because the attach was removed.
- "`ReadPixels` turns bottom-up sources over" contradicted invariant 71 (`TopDownCopy`).
- Elevated windows were said to be "classified `Ignore`"; they are `Blocked` (27).
- "Three harnesses render the UI" preceded a list of five; "61 assertions" is now **212**; the
  clock has twelve layouts, not eight; `ipc.*` and `ctl_main.cpp` were missing from the file table.

Rewritten: grouped by subsystem, every number kept as a stable ID (code and older notes cite
them), duplicates merged by pointer, stale claims corrected, a threads table added.

### A6 — P3 — The settings harness photographed the real profile ✔
`tests/uishot.cpp`

`AppAboutText` is stubbed so captures carry no user name, but the Search page lists the default
indexed folders, which expand to `C:\Users\<name>\…`. `tests/shots` is git-ignored, so nothing was
committed; captures do get shared, though. The harness now sets placeholder folders.

### A7 — Notes on the working-tree change to `Classify` (not changed)
`winutil.cpp`, `wm.cpp`

The uncommitted skip-reason logging — `Classify` naming the rule that turned a window away,
`ExplainSkip` logging it once per window, and `IsOnCurrentVirtualDesktop` re-asking a fresh
`IVirtualDesktopManager` before believing "elsewhere" about a window DWM is drawing — reads
correctly: every rule name is a string literal, so storing the pointer in `Pending` is safe, and
the fresh-object path only runs in the rare disagreeing case. Two small things, left for whoever
finishes that change:

- When the *fresh* `CoCreateInstance` fails, `retryAt` is not set, so the next call tries to
  create the object again immediately rather than five seconds later as the first path does.
- `explained_` is cleared only at 512 entries, so a new window that reuses a logged `HWND` is not
  explained. Diagnostic output only.

---

## B. The rework: after Star Wars Battlefront II

Battlefront II's menus are a thin layer of white type and hairlines over a dark, cinematic scene.
The DOOM Eternal look this replaces was the opposite idea — solid gunmetal plates, hazard orange,
slanted tabs — so this is a rework of the theme engine, not a recolour.

**The backdrop** (`theme.cpp`, MAP.md invariant 72). The window's background is now a picture: a
cold sky darker towards the floor, a blue nebula glow upper left, the atmosphere-lit limb of a
planet crossing the bottom right, a few hundred stars (mostly faint; a few with a little spread),
and a vignette — all a pure function of the pixel, with dithering noise so the dark gradients do
not band. It is rendered once per window size into two DIBs, bare and seen-through-a-panel, and
handed out as pattern brushes phased to the root window, so every label, check box and preview
shows its own piece of the same sky. The 8 px grain (old invariant 63) is retired.

**Panels** are square, translucent (80% towards a cold near-black, stars just visible through),
framed by a hairline whose corners are picked out brighter. Headings are spaced capitals with a
short gold rail before them and a hairline after.

**Type.** Bahnschrift throughout the chrome: *Light* for the window's name, spaced very wide;
*SemiBold SemiCondensed* for headings and buttons; *SemiCondensed* for the category row and the
small captions; Segoe UI for body text, which has to be read rather than recognised.

**Selection is white**, as in the game. A button at rest is a frame; pointed at, it becomes a
solid white bar with dark type. The current category has a white bar under it with a glow rising
off it. The search bar's selected row is swept with white light from a solid bar on the left.
**Gold** is kept for what commits something (Apply) and for the rails. **Hologram blue** is
defined for information; green / gold / red mark the status line.

**Structure.**
- Header: the badge, **P R O W I N D O W S** in thin wide capitals, and a status caption with a
  coloured diamond.
- A category row replaces the tab control: `ProWindowsNav`, a window class of its own, words
  centred between chevron keycaps at the window's edges. Click, arrow keys, the keycaps, or
  **Ctrl+Tab / Ctrl+Shift+Tab / Ctrl+PgDn / Ctrl+PgUp** from anywhere (invariant 73). Captions
  shortened to fit eight across: *Window keys* → **Keys**, *Open apps* → **Apps**.
- The page area is an explicit hidden control in the template (`IDC_PAGEAREA`), so it is laid out
  in the `.rc` like everything else (invariant 7).
- A prompt bar along the foot, under a hairline: **[ENTER] APPLY** in gold and **[ESC] HIDE TO
  TRAY**, with keycaps that are true (invariant 74). The app picker gets the same prompts.
- The title bar's caption is coloured to match the top of the backdrop (Windows 11; older builds
  keep the dark caption).

**Controls.** Check boxes are outlined squares with a solid inner square when ticked; combo boxes
are dark fields with a hairline frame, a white bar when focused and a stroked chevron; sliders are
a hairline with the travelled part solid white and an upright handle; list headers are spaced
capitals; the Layout, Monitor and Clock previews are dimmed windows onto the backdrop.

**Search bar.** The same backdrop, frame and gold rule; a drawn magnifier; the placeholder in
spaced capitals; kind tags as small spaced capitals; and the key hints redrawn as a prompt bar —
keycap, then word — set from the right.

**Icon.** A round badge: deep navy disc, a thin light ring broken at the four compass points like
a reticle, and the tiled-window mark inside — the master pane gold, the others white and slate.
`res/gen_icon.py`.

**Overlays.** Both the monitor and the clock gain **Holonet** (square dark glass, white readings,
a restrained per-metric palette; for the clock, thin wide digits and a gold second hand) and
**Hologram** (everything in one projected cyan). Holonet is the default for new configs; existing
configs store skins by name and are untouched (invariant 9). The clock gains a typeface,
`CLOCK_FACE_WIDE` (Bahnschrift Light), whose date line uses the condensed sibling. The clock's
*Slayer* skin keeps its look; only its description lost the franchise name.

Nothing here uses Star Wars artwork, logos or insignia; the look is built from layout, type,
colour and procedural drawing.

---

## C. Verification

- `build.bat PW_dev.exe`: builds clean (the only warnings are the SDK's own GDI+ headers under
  `/W4`, as before).
- `tests\run.bat`: **212 passed, 0 failed**.
- `tests\uishot.bat`: every page captured and inspected — header, category row, cards, prompt bar,
  previews, list headers, disabled states; the Clock and Monitor pages now fit. The harness now
  also *checks* the shell by messages sent to the controls: a click on the category row moved
  from page 7 to page 4 with exactly one page showing; `DM_GETDEFID` answers `IDC_APPLY`; and
  Apply under the pointer reads back as solid gold (`DFB75D`) from the window's surface.
- Found on the way: `PrintWindow` returned the category row and footer blank after the synthetic
  mouse move while the window's surface had both — the capture, not the paint. `uishot` now takes
  the client area from the surface itself.
- `tests\launchshot.bat`: empty, app, calculator, settings and program queries captured; tags and
  prompt keycaps measured correctly after the fix in invariant 77.
- `tests\monshot.bat`, `tests\clockshot.bat`: Holonet and Hologram rendered for both overlays;
  Hologram's clock glow reduced after the first render smeared the date line.
- `res\gen_icon.py`: 16 – 256 px rendered and inspected at 32 and 256.

Not verified, and why:

- **The real tiler was not started.** Doing so arranges every window on the desktop; everything
  above runs the UI with the manager stubbed out. Ctrl+Tab through `SettingsTranslateMessage` in
  the real message loop is therefore reasoned rather than observed (it calls `NavStep`, which
  sends the same notification as the checked click), and so is the status mark under live state.
- `tests\bindshot.bat` and `tests\rowdragshot.bat` press real keys and move the real mouse; not run.
  Neither's code path was changed beyond the shared painters.
- The settings window was only captured at 100%; the surfaces and every hand-drawn size follow the
  window's DPI, but no other scale was looked at.
