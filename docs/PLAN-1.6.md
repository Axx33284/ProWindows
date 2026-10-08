# ProWindows 1.6 — release plan

Four goals: **(1)** a pre-release review of the code, **(2)** the settings look rebuilt after the
options screens of *Resident Evil Requiem*, carrying ProWindows' own content, **(3)** storage
cleaned up, **(4)** the app optimised. Every task names its owner agent (see `CLAUDE.md`):
**H** = `janitor` (Haiku 4.5), **S** = `builder` (Sonnet 5.5), **O** = `reviewer` (Opus 5.5).

Order: Phase 0 → Phase 1 runs in parallel with Phase 2 → Phase 3 → Phase 4. Phase 2 is the bulk.

---

## Phase 0 — housekeeping and storage (H)

- [ ] **0.1 (ask the user first)** Commit the uncommitted 1.5 work as its own commit before 1.6
  begins, so the reskin diff is reviewable on its own. Message: `ProWindows 1.5: options-screen
  rework, one window and no controls, modal screens, rows, three-way Apply`.
- [x] **0.2** Delete regenerable build output: `tests/build/` (40 MB, includes stale
  `rowdragshot.exe` whose source is gone), `tests/shots/` (11 MB), `build/app.res`. All gitignored.
- [x] **0.3** Check `git ls-files` for anything that should not be tracked (binaries, PNGs, `.res`).
  Report; delete only build output.
- [x] **0.4** Move `docs/REVIEW-1.1.md` … `REVIEW-1.3.md`, `REVIEW-icons-and-idle.md`,
  `REVIEW-startup-and-input.md` to `docs/archive/` with `git mv`; fix links (`grep -rn REVIEW-1.[123]`).
  Keeps `docs/` to the current pass and stops agents from opening old reviews.
- [ ] **0.5** Stale references: `grep -rn "Battlefront\|DOOM" src res README.md MAP.md` (9 hits).
  Leave them for now — task 4.2 rewrites them to Requiem once the reskin lands.
- [ ] **0.6** Runtime data (`%APPDATA%\ProWindows`): `icons.cache` 780 KB (capped at 512 icons),
  `index.cache` 510 KB, `log.txt` truncated when debug turns on. Nothing to delete; see 3.3.

## Phase 1 — pre-release review (O)

- [x] **1.1** Review the 1.5 diff (`git diff HEAD --stat`, then by file) against the invariants.
  Focus: `modal.cpp` (own message loop, inv. 82), `rowlist.cpp` (inv. 79), `settings.cpp`
  capture hook (inv. 81, 85), the merge (inv. 78), GDI handle balance in `theme.cpp` painters.
  Output: `docs/REVIEW-1.6.md` §A, findings P1–P3 with file:line.
- [x] **1.2** S fixes every P1/P2 from 1.1; each fix ticked in REVIEW-1.6 with ✔.
- [ ] **1.3** H runs `tests\analyze.bat` (all sources) and `tests\run.bat`; report new warnings.

---

## Phase 2 — the Requiem look (S, decisions by O)

### 2.1 What the reference screens are

Pure black screen. Top-left a large plain title (`OPTIONS`). Under it a **horizontal tab bar** of
eight categories in squared capitals, separated by thin vertical rules, a keycap at each end (the
keys that step tabs), a hairline under the whole bar, the active tab white with a short soft light
under it. Optionally a **sub-tab row** (keycap `1`, `Display`, `Image`, keycap `3`) with its own
hairline. Below, two columns: on the left a **list of rows** separated by hairlines; on the right
the **description** of the focused row, an optional *(Default: …)* line, a **16:9 preview
picture**, and under it **meters** (labelled bars). Centred at the bottom, **button prompts**:
a light keycap and a word. Nothing is coloured except one muted green in a meter. The focused row
is a **brushed-metal bar**: a grey gradient with a sheen and a light 1 px outline. ProWindows'
eight categories (Layout, Behaviour, General, Shortcuts, Apps, Search, Monitor, Clock) map one to
one onto the game's eight tabs.

Reference sizes below are in pixels of the 2000 × 1125 screenshots; **DIP = ref × 0.64**
(the settings window's design size is 1280 × 720 DIPs). Colours are sRGB.

### 2.2 Palette (`theme.h` — replaces the amber set)

| Token | RGB | Use |
| --- | --- | --- |
| `Bg` | 0,0,0 | the screen; no backdrop picture any more (see 3.1) |
| `Line` | 40,40,40 | hairline between rows, under the tab and sub-tab bars |
| `Rule` | 92,92,92 | vertical separators between tabs |
| `Text` | 232,232,232 | row labels, values |
| `TextHi` | 255,255,255 | focused row's label, active tab |
| `TextDim` | 168,168,168 | inactive tabs, section labels, chevrons |
| `TextBody` | 205,205,205 | description panel |
| `TextMute` | 96,96,96 | disabled rows |
| `SegOn` / `SegOff` | 225,225,225 / 78,78,78 | value segments |
| `Plate` | 30,30,30 → 22,22,22 | section plate gradient |
| `Metal0..3` | 88,88,88 / 58,58,58 / 38,38,38 / 30,30,30 | focus bar gradient stops |
| `MetalEdge` | 150,150,150 | focus bar outline |
| `KeyFill` / `KeyInk` | 205,205,205 / 20,20,20 | keycap |
| `MeterFill` | 140,220,160 | meter bar (the one colour) |
| `MeterEdge` | 130,130,130 | meter frame |
| `Good` / `Danger` | keep | status marks, destructive actions |

Delete `Amber*`, `Fill*`, `Slate*`, `Track`, `AmberTrack` once nothing references them.

### 2.3 Type

- **Title, tabs, keycap letters**: Bahnschrift SemiCondensed (squared like the game's tabs). Title
  `SETTINGS` regular, cap height ≈ 30 ref → font ≈ 26 DIP, no tracking. Tabs ≈ 17 DIP regular,
  tracking 1 DIP, capitals.
- **Rows, values, description, sub-tabs, prompts**: Segoe UI (Segoe UI Variable Text when
  present), **sentence case — not capitals**. Row label / value ≈ 15 DIP regular; description
  ≈ 16 DIP; section plate ≈ 14 DIP; prompts ≈ 16 DIP. `Row::label` strings are already written
  for capitals — S checks each reads right in sentence case (`Caps()` is no longer applied to rows).

### 2.4 Layout (`settings.cpp DoLayout`, all in DIPs at 1280 × 720, scale with the window)

| Element | Geometry |
| --- | --- |
| Margins | left 70, right 70 |
| Title | baseline at y ≈ 52 |
| Tab bar | y 64–98; tabs evenly spaced across the full width between the two keycaps; hairline at y 99 spanning the margins |
| Sub-tab row | y 140–168, only when the category has pages (2.6); keycaps at x 104 and after the last page; hairline at y 171 across the **left column only** |
| Left column (list) | x 70 → 670 (≈ 52 % of width) |
| Scrollbar | 3 wide, at x ≈ 694, from the list top to the list bottom; track `Line`, thumb `TextMute` |
| Right column | x 742 → 1210 |
| Row height | 45; label inset 20 from the column's left |
| Footer prompts | centred, baseline y ≈ 700 |

The window keeps its own frame (inv. 80): the close and minimise glyphs stay at the top right,
drawn as thin 1 px strokes in `TextDim`, white on hover. The cut-corner footer **buttons go**;
Apply / Reset / Close become prompts, which are clickable already (inv. 74).

### 2.5 Controls (`theme.cpp`, drawn by `rowlist.cpp`)

- **Choice** (`< value >`): thin stroked chevrons (≈ 5 × 9 DIP, 1.3 px, `TextDim`) at 61 % and
  96 % of the row's width; the value centred between them; under the value a **segment
  indicator**: one segment per option, each 24 × 2 DIP with a 2 DIP gap, the chosen one `SegOn`,
  the rest `SegOff`. More than 8 options: a 2-DIP track with a thumb sized 1/n instead.
- **Toggle**: drawn as a two-option Choice (`Off` / `On`, two segments), exactly as the game draws
  Motion Blur. Left/right or a click on either half flips it. `DrawPair` goes.
- **Slider**: `< - ├┼┼┼┼…┤ + >` then the number. A 1 px `TextDim` ruler with 20 ticks (3 DIP; every
  5th 6 DIP), a 2 × 16 DIP white marker at the value, `-` and `+` glyphs at the ends, the
  chevrons outside them, the value right-aligned at the row's end in `Text`.
- **Action**: the label, and at the right edge an **open-in icon** (a square missing its top-right
  corner with a diagonal arrow out of it, 14 DIP, 1.3 px, `Text`). A Choice row that also opens a
  screen draws its chevrons *and* the icon (the game's *Grace's Camera*).
- **Keys** (a chord): keycaps right-aligned as now, in the new keycap style.
- **Colour**: a Choice with a 10 DIP square swatch before the name.
- **Item / Info**: label left, value right in `TextDim`, no chevrons.
- **Section**: a **plate**, not a heading: fill `Plate` gradient, left at the column's edge,
  45 % of the column wide, 26 DIP tall, its right end cut by a slant leaning out to the bottom
  right (22 DIP run); label inset 10, `TextDim`. A `Line` hairline continues from the plate's
  bottom across the whole column.
- **Fold** *(new kind, optional — see decisions)*: `^` / `v` chevron in a 40 DIP cell, a 1 px
  vertical `Line` after it, then the label. Collapses the rows after it up to the next Section.
- **Disabled**: label, value and chevrons `TextMute`; segments `SegOff` / 110,110,110.
- **Focus** (replaces amber `RowFocus`): the whole row becomes a **brushed-metal bar**:
  a vertical gradient through `Metal0..3` (top 0 %, 18 %, 60 %, 100 %); a horizontal sheen,
  white at α 28 at the left fading to 0 by 45 %, and a faint elliptical highlight (white α 18)
  centred under the value; a 1 px `MetalEdge` outline plus a 1 px inner top line white α 60.
  No coloured glow, at most a 3 DIP soft white outer bleed at α 24. Label turns `TextHi`.
  Fades in with the existing `lit` value. Cheap: gradients and one `Haze`, no per-pixel work.
- **Keycap**: a solid `KeyFill` rounded-2 box, letters in `KeyInk`, min 20 × 20 DIP; mouse
  buttons drawn as a small mouse outline (the game's *Back*).

### 2.6 Pages (the sub-tab row)

A category can split into **pages**, each a sub-tab; categories with one page show no sub-tab row
(the game's CAMERA screen has none). Pages are explicit: a new `Kind::Page` row marks where one
starts and names it. Proposed split (O signs it off): Shortcuts → *Windows · Workspaces ·
Launchers*; Monitor → *Display · Readouts · Colours*; Clock → *Display · Look*; Search →
*Sources · Index*. Others: one page. Search results ignore pages.

### 2.7 The right column, in ProWindows' terms

1. **Description** of the focused row (`Row::help`), `TextBody`, wrapped to the column.
2. **(Default: …)** — the value the row has in a default `Config`, formatted as the row would
   show it. Rows get a `std::function<std::wstring()> fallback` (or the category computes it from
   `Defaults()`); omitted for lists and actions.
3. **Preview**, 16:9, 468 × 263 DIP, under the text: the existing layout / monitor / clock
   previews, framed by nothing, on black. A category with no preview shows the ProWindows mark,
   large and dim (`TextMute`), centred — never an empty box.
4. **Meters** (the game's *VRAM Usage* / *Processing Load*), label left, bar right (220 × 10 DIP,
   1 px `MeterEdge` frame, `MeterFill` gradient, hatched part = "other"). Content per category:
   - General: *Memory usage* (ProWindows' private bytes vs 64 MB; hatched = the settings window's
     share, if measurable), *Windows arranged* (managed / total top-level).
   - Search: *File index* (entries vs `searchMaxEntries`), *Icon cache* (count / 512).
   - Monitor: *Sampling cost* (sampler ms per tick vs its interval).
   - Others: none.
   Meters are read when the category opens and on a 1 s timer only while the window is visible.
   Each value comes from a cheap getter in `app.h` (stubbed in the harnesses).

### 2.8 Keys and prompts (inv. 74 still rules)

- The game steps tabs with **Q / E** and sub-tabs with **1 / 3**. Today `WM_CHAR` starts the
  cross-category search when you type, so bare letters are taken. **Decision needed (O + user):**
  either (a) Q/E/1/3 step tabs and pages, search moves to **Ctrl+F** or **/** (shown as a
  prompt), or (b) keep type-to-search, end keycaps show **Ctrl+PgUp / Ctrl+PgDn** and pages use
  **Ctrl+1..9**. Recommendation: (a) — it is the game's feel and the search box is still one key
  away.
- Footer prompts: `Ctrl S  Apply` (only when something is unapplied), `R  Reset category`,
  `Tab  Reset all` *(asks first, `ui::Ask`)*, `Esc  Back` / `Close`, plus the focused row's own
  prompts from `RowList::Prompts()`.

### 2.9 Everything else that uses the theme

- **Modal screens** (`modal.cpp`): black plate with a `Line` frame, title in Bahnschrift, the
  options as rows with the metal focus, prompts underneath.
- **Search bar** (`launcher.cpp`): black, the query in Segoe UI, results as rows with the metal
  focus and the open-in icon on the focused result; no backdrop picture.
- **Icon** (`res/gen_icon.py` → `app.ico`, and `theme::Mark`): the same tiled arrangement, master
  pane **white / brushed silver** instead of amber, on black. Tray, taskbar and header match.
- **Overlays** (monitor, clock) keep their own skins. Optional: add a `requiem` skin to each
  (`montheme.cpp`, `clocktheme.cpp`: black, grey hairlines, white type, green accent) — task 2.T9.

### 2.10 Tasks

- [ ] **2.T0 (O)** Settle the open decisions: keys (2.8), pages split (2.6), Fold kind yes/no,
  whether the window title reads `SETTINGS` or `OPTIONS`. Write them under *Decisions* below.
- [x] **2.T1 (S)** Palette and fonts in `theme.h/.cpp` (2.2, 2.3). Old tokens aliased until 2.T6.
- [x] **2.T2 (S)** Painters: Choice-with-segments, Toggle-as-Choice, ruler Slider, open-in icon,
  section plate, metal `RowFocus`, keycap (2.5). Each painter answers `WM_PRINTCLIENT` paths as now.
- [x] **2.T3 (S)** `settings.cpp` layout: title, tab bar with keycaps and active-tab light,
  sub-tab row, two columns, thin scrollbar, prompts in place of footer buttons (2.4, 2.8).
- [x] **2.T4 (S)** `Kind::Page` and the page split; keys per the decision (2.6, 2.8).
- [x] **2.T5 (S)** Right column: description, (Default: …), preview or mark, meters + `app.h`
  getters + harness stubs (2.7).
- [x] **2.T6 (S)** Modal screens and search bar in the new look; delete dead amber code and the
  backdrop renderer (2.9, 3.1).
- [ ] **2.T7 (H)** Icon: change `gen_icon.py` colours as specified, regenerate `app.ico`,
  update `theme::Mark` colours.
- [ ] **2.T8 (S)** Update `tests\uishot.cpp`: the "only the focused row is amber" pixel check
  becomes "only the focused row has a metal bar" (luma of the row band ≥ 30 vs ≤ 8 elsewhere);
  add captures for a page switch and the meters. Then `tests\clickprobe.bat` (warn the user: it
  moves the mouse) — the X, minimise and the clickable prompts must work.
- [ ] **2.T9 (S, optional)** `requiem` skin for the monitor and the clock.
- [x] **2.T10 (O)** Look at `ui-*.png` beside `docs/design/ref/*.webp` and list what still
  reads wrong (≤ 10 items, each with the fix). S applies them.

### Decisions

- **Keys (user, 2026-10-07): option (a).** Q/E step tabs, 1/3 step pages, search moves to
  Ctrl+F or `/`. 1.5 is committed (0.1 done).
- **Pages, 2.6 (O, 2026-10-07): split by the existing section headings, with one change.**
  Shortcuts → *Windows* (Modifier, Focus, Move windows, Resize, Windows) · *Workspaces*
  (Workspaces, Layout) · *ProWindows* (ProWindows, All shortcuts). The proposed *Launchers* page
  is dropped: launchers live in the **Apps** category, which stays one page. Monitor → *Display*
  (Overlay, Look) · *Readouts* (Readouts, in order) · *Colours*. Clock → *Display* (Clock, What it
  shows) · *Look*. Search → *Sources* (What to search, Programs) · *Index* (File index, Folders to
  index). Layout, Behaviour, General, Apps: one page. Why: each page is a run of whole sections
  the builders already emit, so `Kind::Page` rows go in front of existing `Section`s and no row
  moves. The current page is held by the Page row's **id**, not its index (inv. 79 - a rebuild
  must not change page), and `ResetPage` still resets the whole category.
- **Focus follows the mouse, 3.4 (O, 2026-10-07): `EVENT_OBJECT_LOCATIONCHANGE`, not a mouse
  hook.** A hook instance of its own (`SetWinEventHook(LOCATIONCHANGE, LOCATIONCHANGE, …,
  WINEVENT_OUTOFCONTEXT)`, with its own callback - never `WinEventProc`, whose placement logic
  relies on that event *not* being hooked, `wm.cpp:1800`), installed only while
  `focusFollowsMouse` is on and game mode is off, removed otherwise. The callback ignores
  everything but `idObject == OBJID_CURSOR` and only arms a one-shot `TIMER_MOUSE` (~60 ms) if
  none is pending; the timer does today's `WindowFromPoint` / `lastUnder` check once and kills
  itself. Why: with the feature on, a `WH_MOUSE_LL` hook would have to stay installed the whole
  time - every pointer move on the machine a synchronous round trip into the UI thread, stalling
  the pointer whenever that thread is busy, the very cost `moddrag.cpp` was rebuilt to avoid.
  Out-of-context WinEvents are queued, never block input, and a still pointer costs nothing.
  Check with the probe in Phase 3 that the event's chatter (window and caret moves) stays cheap.

---

## Phase 3 — optimisation (S, measured; O reviews)

Measure before and after each change: private bytes and GDI/USER objects of the running app
(Task Manager columns or `GetProcessMemoryInfo` / `GetGuiResources` in a probe), and `monshot
--bench` for paint cost. Record the numbers in REVIEW-1.6 §C.

- [x] **3.1** **Drop the backdrop picture.** `RenderBackdrop` renders a DIB at the *monitor's*
  size (a 4K screen is ~33 MB) and caches up to three (inv. 72). The Requiem screen is flat black:
  fill with `Bg`, draw at most a cheap gradient vignette. Removes the biggest allocation the
  settings window and the search bar make. Retire inv. 72 in MAP.
- [x] **3.2** **Cheaper focus.** The amber `Glow` is per-pixel alpha work every animation frame
  (15 ms timer while fading); the metal bar is gradients. Confirm the fade timer stops when no
  row is fading.
- [x] **3.3** **Icon cache.** 780 KB for ≤ 512 icons. Store only the size the search bar draws,
  and drop entries not seen for 30 days on save. Bump `kCacheVersion`; run `tests\iconcache.bat`.
- [x] **3.4** **Focus-follows-mouse polls** every 120 ms (`main.cpp` `TIMER_MOUSE`), against the
  "never polls" rule. Replace with the `WM_MOUSE`-hook-only-while-needed pattern `moddrag.cpp`
  already uses, or `EVENT_OBJECT_LOCATIONCHANGE` on the cursor — O to choose; S implements.
  **O chose (2026-10-08):** `EVENT_OBJECT_LOCATIONCHANGE` / `OBJID_CURSOR` through an
  out-of-context `SetWinEventHook`, installed only while focus-follows-mouse is on; each event
  arms the one coalescing timer. No `WH_MOUSE_LL`: it adds latency to every mouse move on the
  machine. If `main.cpp` already does this (grep "the cursor arms one timer"), only remove
  what still polls.
- [x] **3.5** **Meters must not cost.** The 1 s meter timer runs only while the settings window
  is visible and in front; getters read cached numbers, never walk anything.
- [x] **3.6** **Binary size.** `dumpbin /headers` and a map file (`/MAP`) on `ProWindows.exe`
  (1.0 MB, already `/O1 /GL /LTCG /OPT:REF,ICF`). Report the top 10 contributors; act only on
  something obviously dead.
- [ ] **3.7 (O)** Review 3.1–3.6 for regressions (idle trim inv. 67, thread rules).

## Phase 4 — release (H, then O gate)

- [ ] **4.1 (H)** Version 1.6.0: `kVersion` in `src/common.h`, `VERSIONINFO` in `res/app.rc`.
- [ ] **4.2 (H)** Docs: replace the Battlefront / DOOM wording with Resident Evil Requiem in
  `theme.h`, `MAP.md` (§ "Where things live", inv. 72/80, the theme row), `README.md`; README
  "What's new in 1.6" from REVIEW-1.6. Update the numbers README quotes (assertion counts etc.).
- [ ] **4.3 (H)** Clean build, then run: `tests\run.bat`, `tests\uishot.bat`,
  `tests\launchshot.bat`, `tests\monshot.bat`, `tests\clockshot.bat`, `tests\iconcache.bat`,
  `tests\analyze.bat`. All green or report.
- [ ] **4.4 (O)** Final diff review against this plan; anything P1/P2 blocks the release.
- [ ] **4.5 (user)** Run the real app for a day; then commit, tag `v1.6.0`, zip
  `build\ProWindows.exe` + `build\prowindowsctl.exe` + `README.md`.

## Phase 5 — bug fixes and the Timer (added 2026-10-08)

### 5.A Fixes that landed (O), verify only

- [x] **5.A1** Brave stops being tiled after a few hours or a restart. Cause: `tooSmall`
  ("uses too little of its tile") and `tooLarge` ("too large for this monitor") were decided
  from a few looks at a window that may have been mid-restore or playing fullscreen video, and
  they were never looked at again. `tooLarge` was also saved to `config.ini` as `nofit` and
  reloaded on every run. The fix is in the `wm.cpp` layout pass (`misfitAt` in `wm.h`,
  `kMisfitRetryMs` = 5 min). Both verdicts now expire, the observed limits are forgotten, the
  window is asked again, and the stale `learned` record is erased. A window that really does not
  fit is caught again on the same pass.
- [x] **5.A2** "Run as administrator" in the search bar (menu, footer prompt, Ctrl+Shift+Enter)
  seemed to do nothing. The bar hid itself *before* `ShellExecuteEx("runas")`, so the UAC prompt
  came up as a flashing taskbar button. Now `RunElevated` runs first, owned by the bar
  (`info.hwnd`), and any failure other than a refused prompt is logged (`launcher.cpp`).
- [ ] **5.A3 (user)** Restart the tiler on the new build and use it for a day with Brave. Then
  search `%APPDATA%\ProWindows\log.txt` for "trying to tile it again" and "sits out". Also try
  elevating from the search bar.

### 5.B The Timer: spec (decided by O, built by S)

**What it is.** A panel (an overlay since 5.D; first built as a Win+T popup) with two pages: **Timer** (countdowns)
and **Stopwatch**. Everything is anchored to the **system clock** (UTC wall time), never to tick
counts. A running timer or stopwatch keeps going through sleep, hibernate, shutdown and restart,
and on the next start it is exactly where the wall clock says it should be.

**Files.** New `src/timer.h` / `src/timer.cpp` for the window, state, input and sound. Split out
`src/timerpaint.cpp/.h` only if painting grows past ~400 lines. Register each `.cpp` in
`build.bat` **and** in `tests\uishot.bat`, `clickprobe.bat`, `launchshot.bat` and `analyze.bat`.
`timer.cpp` pulls in `winmm.lib` with a `#pragma comment`.

**State and storage.** All times are `int64` 100-ns units from `GetSystemTimeAsFileTime`.
- Timer *i* (up to **8**) holds `label`, `duration`, and either `endUtc` (running) or
  `remaining` (paused or not started). Remaining = `endUtc − nowUtc`, clamped at 0.
- Stopwatch (one) holds `startUtc` and `banked`. While running, elapsed =
  `banked + now − startUtc`, and a pause folds that into `banked`. Laps: up to 99, newest first.
- Store all of this in **`%APPDATA%\ProWindows\timers.ini`** (`ConfigDir()`), **not** in
  `config.ini`, which is rewritten whole and would churn on every start, pause and lap. Write it
  to `.new` first and then `MoveFileExW`, as `Config::SaveToFile` does (`defaults.cpp`). Load it
  once in `TimerInit`, and save on every state change only. Nothing is written while time
  simply passes.
- Durations go up to **9999 days 23:59:59**: 8.6e15 in 100-ns units, safe in `int64`. The
  stopwatch has no limit worth caring about (int64 covers 29,000 years).

**Clock rules.**
- `WM_TIMECHANGE`: the user moved the clock, so the timers move with it, as the user asked.
  Only repaint.
- On resume (`WM_POWERBROADCAST` / `PBT_APMRESUMEAUTOMATIC`, already handled near
  `main.cpp:1220`), call `TimerCheckNow()`.
- A timer whose `endUtc` passed while the PC was off or asleep fires **once**, at load or on
  resume. Title: "Finished while you were away". The text gives the label and the local time it
  ended.
- If the clock went back past `startUtc`, elapsed would be negative: show 0, never wrap or
  crash.

**Waking up.** The UI thread never waits, and nothing polls while idle.
- When nothing is running, there is no timer at all.
- When the panel is visible and something runs, use `SetTimer` aligned to the next whole
  second, or 100 ms while the stopwatch shows tenths.
- When the panel is hidden, use one `SetTimer` for `min(next deadline, 60 s)`, or 1 s only when
  the every-second tick is on. `SetTimer` caps at about 24.8 days, so always clamp.

**Display.** `H:MM:SS` under a day and `Nd HH:MM:SS` from a day up (`9999d 23:59:59`). The
stopwatch adds `.t` (tenths) while visible. Use the Requiem tokens and fonts from `theme.h`, and
for spaced text `theme::SpacedWidth` / `DrawSpaced` (inv. 77). Build the window the way
`launcher.cpp` builds the search bar:
- centred on the monitor under the pointer and DPI-scaled;
- hidden on deactivate;
- `FocusWindow` to take the foreground;
- resources freed on idle (`kTimerIdle`).

**Keys in the panel.** The footer prompts list only these, and clicking a prompt sends its key
(inv. 74).
- `Tab`: switch page.
- `Up` / `Down`: select a timer.
- `Enter`: edit the selected timer's duration in the fields `days : hh : mm : ss`. Digits type
  into the focused field, `Left` / `Right` move between fields, and `Enter` commits.
- `Space`: start or pause. `R`: reset.
- `N`: new timer. `Del`: remove the timer.
- `L`: lap (stopwatch).
- `Esc`: close.

**When a timer ends.**
- If the alarm setting is on, an alarm loops for up to 60 s: `C:\Windows\Media\Alarm01.wav` if
  it exists, otherwise `SystemExclamation` through `PlaySoundW(..., SND_ALIAS)`. Any key in the
  panel stops it.
- A tray balloon (`AppTrayBalloon`) names the timer, and the panel opens on it.
- With the alarm off, only the balloon shows.

**Tick sound.** The user asked for "a tick clock sound … able to enable or disable". Setting
`timer_tick = off | second | minute`, default `off`:
- `second`: a tick every whole second while any timer or the stopwatch runs.
- `minute`: a tick each time the seconds come round to :00 (the "full clock").

Build the tick once as an in-memory WAV: 16-bit mono 44.1 kHz, about 12 ms, a decaying 2 kHz
burst. Play it with `PlaySoundW(buf, nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT)`. Ship no
asset file, and never tick while the alarm loops.

**Config and settings.**
- `config.ini` gets `timer_tick = off|second|minute` and `timer_alarm = true|false`. Parse them
  in `config.cpp`, write them in `SaveToFile` (`defaults.cpp`), and list both in
  **`AWA_EDITED_FIELDS`** (`settings.cpp:160`, inv. 78), or Apply drops them.
- In Settings, add a "Timer" group on the best-fitting existing page (`settings_clock.cpp` is
  the natural home): a three-way Choice for the tick and a Toggle for the alarm, each with a
  right-column description (2.T5). Row callbacks must not touch a `Row&` after calling out
  (inv. 79).

**Binding.**
- Add `ACT_TIMER` (word `timer`) to `config.h`. In `config.cpp`, add its parse (near :163),
  word (near :279) and description (near :348), and add its hint at `settings_keys.cpp:79`.
- Dispatch it at `main.cpp:264` → `TimerToggle()`.
- No default key (5.D removed the Win+T default and its migration; `kConfigVersion` stays 4).
  The action is bindable by the word `timer`.
- The ctl channel gets `timer` for free, since it uses the bind words. Update its help text in
  `ctl_main.cpp`.

**Lifecycle.**
- Call `TimerInit(inst, cfg)` after `ClockInit` in `main.cpp`, `TimerShutdown()` beside
  `ClockShutdown`, and `TimerApplyConfig()` wherever `ClockApplyConfig()` runs.
- Respect the idle trim (inv. 67): no thread and no resident DIB while the panel is hidden.

### 5.C Tasks

- [x] **5.C1 (S)** `timer.h/.cpp`: state, `timers.ini` load and save, the time maths and the
  formatting. Add `tests\timer_test.cpp` (asserts, run from `tests\run.bat` like
  `layout_test`). It must cover:
  - parse/format round-trip, including `9999d 23:59:59`;
  - remaining time across a simulated 3-day "off" gap;
  - a stopwatch running past 400 days;
  - a clock set backwards gives 0, not a negative time;
  - a finished timer fires once on load.
- [x] **5.C2 (S)** The panel: window, paint, keys, footer prompts, both pages, the editor.
- [x] **5.C3 (S)** Alarm and tick sound, the settings rows, the config fields and
  `AWA_EDITED_FIELDS`.
- [x] **5.C4 (S)** `ACT_TIMER`, the Win+T default and migration, dispatch, ctl help text.
- [x] **5.C5 (S)** `tests\timershot.bat`, modelled on `clockshot.bat`: PNGs of both pages, a
  running timer showing days, and the editor. Exit code 1 on failure.
- [x] **5.C6 (H)** Register the sources in every `.bat`. Add Timer to README's "What's new in
  1.6" (`grep -n "What's new" README.md`, edit only that range). Add a "Timer" heading to MAP
  under "The overlays", with this feature's invariants: wall-clock anchoring, `timers.ini` kept
  apart from `config.ini`, and nothing waking while idle.
- [x] **5.C7 (O)** Review 5.C1–5.C4 against inv. 67/74/77/78/79 and the "UI thread never
  waits" rule. **Passes:** 67 (paint-time DIB freed every paint, no thread, idle trim armed on
  hide), 74 (every prompt maps to a handled key; click sends it through `HandleKey`), 77 (tabs
  go through `Measure`/`Print` → `SpacedWidth`/`DrawSpaced`; `DrawCells` is unspaced), 78
  (`timerTick`/`timerAlarm` in `AWA_EDITED_FIELDS`), 79 (stock `Toggle`/`ChoiceOf` rows). The
  UI thread does a small synchronous `timers.ini` write per state change, the same as
  `AppSaveConfig`; accepted. **Bugs, fixed in 5.D:**
  - R1. `TimerHide` clears `g_editIsNew` *before* calling `EndEdit`, so a new timer cancelled by
    closing the panel is kept instead of taken back out.
  - R2. `Reposition` takes the DPI of where the window *was*, not of the monitor it is moved
    to: wrong scale the first time it opens on a monitor with another DPI. No `WM_DPICHANGED`.
  - R3. A finished timer with the alarm on calls `FocusWindow`: it steals the keyboard from
    whatever the user is typing in, and their next key only silences the alarm.
  - R4. `Deserialize` accepts `watch.running = 1` with `watch.start = 0`: elapsed = 400 years.
    Running needs `startUtc > 0`, as timers need `endUtc > 0`.

### 5.D The Timer becomes an overlay (user, 2026-10-08)

The user asked for two changes: **no Win+T** - the timer is opened from the app - and it can be
**pinned like the monitor and clock**. So the panel stops being a search-bar-style popup and
becomes an overlay that stays where it was put.

**Config.** `timer_shown` (bool, default false), `timer_pinned` (bool, false), `timer_x` /
`timer_y` (int, `INT_MIN` = never placed: centred in the primary work area, 22 % down). Parse in
`config.cpp`, write in `SaveToFile`, defaults in `config.h`. `timerShown` and `timerPinned` go in
`AWA_EDITED_FIELDS` (inv. 78) and in `ResetClockPage`.

**Window.** `WS_POPUP`, `WS_EX_TOOLWINDOW | WS_EX_TOPMOST`. Pinned adds `WS_EX_LAYERED |
WS_EX_TRANSPARENT | WS_EX_NOACTIVATE` (then `SetLayeredWindowAttributes(255, LWA_ALPHA)`;
`WS_EX_TRANSPARENT` is only click-through on a layered window); unpinning removes all three.
Painting stays `WM_PAINT` with the paint-time DIB (inv. 67).
- Shown with `SW_SHOWNOACTIVATE` and **never** `FocusWindow` - clicking it activates it and then
  the keys work. Deactivating no longer hides it.
- Placed at `timerX/Y`, clamped on screen; DPI from the monitor at that point
  (`MonitorFromPoint` + `GetDpiForMonitor`), and `WM_DPICHANGED` rescales (R2). A size change
  keeps the top-left corner.
- Unpinned, a left drag on the header outside the two tab names moves it, as the clock drags
  (`SetCapture`, clamp, save `timerX/Y` + `AppSaveConfig()` on button-up).
- Right-click (unpinned): Pin in place / Hide / Timer settings... (`SettingsOpenTab(PAGE_CLOCK)`).
- `Esc` prompt becomes **Hide**: `timerShown = false`, hide, `AppSaveConfig()`,
  `AppRefreshSettings()`. Inv. 74 still holds.
- Hidden by game mode and display-off like the others: `UpdateOverlayVisibility` in `main.cpp`
  calls `TimerSetVisible(g_cfg.timerShown && allowed)`. Timers keep running while hidden.

**Public API** (`timer.h`): `TimerSetVisible(bool)`, `TimerSetPinned(bool)`, `TimerPinned()`,
`TimerAlarming()`, `TimerStopAlarm()`; `TimerToggle()` stays for the `timer` action and flips
`timerShown`. `TimerApplyConfig()` also re-applies pin and position.

**Tray.** A third overlay submenu, **Timer**, beside Clock: Show (checked = `timerShown`), Pin in
place (greyed unless shown), **Stop alarm** (only while alarming - a pinned panel cannot take a
key), separator, Timer settings... - mirroring the monitor and clock submenus.

**Alarm** (R3). A finished timer with the alarm on sets `timerShown = true` (saved), selects it
on page 0 and shows the panel without activating it. Any key or click in the panel, or the tray
item, stops it; it still gives up after 60 s.

**Settings** (Clock page, "Timer" section): add "Show the timer" (toggle, `timerShown`), "Pin in
place" (toggle, `timerPinned`, enabled only while shown), "Position" Reset action (as the
clock's). Take Win+T out of every description.

**Win+T goes.** Remove the `win+t` default and its `kAdded` entry, put `kConfigVersion` back to
4 and drop "5 adds the timer panel's win+t". `ACT_TIMER` stays as a bindable word (`timer`, no
default, ctl `timer` shows/hides). Sweep "Win+T" out of `config.h`, `defaults.cpp`,
`settings_keys.cpp`, `settings_clock.cpp`, `timer.h`, MAP inv. 88–90 / Timer heading, README
"What's new in 1.6", and §5.B above.

- [x] **5.D1 (S)** Everything above, plus R1 and R4. `timer_test` gets an R4 case.
  `tests\timershot.bat` shoots the overlay (unpinned and pinned) instead of the popup. Green:
  `build.bat PW_dev.exe`, `tests\run.bat`, `tests\timershot.bat`, `tests\uishot.bat`.
- [x] **5.D2 (O)** Review the 5.D1 diff (done with 6.2).

## Phase 6 — Welcome (user, 2026-10-08)

The user wants opening the app to show what it is and what it can do - Win+S search first -
for someone who just installed it and for someone who forgot a shortcut. In the app's design.

**Where.** A new settings category, **Welcome**, first in `kTabOrder` (`PAGE_WELCOME`; keep the
other `PageIndex` numbers stable by appending it to the enum, as the tab order is separate). It
is built only from existing row kinds (`Section`, `Info`, `Action`, `Page`), so it inherits the
Requiem look, the right-column description (2.T5) and the footer prompts (inv. 74) for free. No
Reset (nothing to reset; hide the Reset prompt if the `PageDef` allows a null reset, otherwise a
no-op). Nothing edited, so nothing joins `AWA_EDITED_FIELDS`.

**When it shows.**
- Launching the app (the `SettingsOpen(inst)` at start-up in `wWinMain`) lands on Welcome.
- First run (`firstRun`) opens the settings window on Welcome even when starting hidden.
- Tray: a new item **"Welcome and shortcuts..."** above "Keyboard shortcuts..." →
  `SettingsOpenTab(PAGE_WELCOME)`. Tray "Settings..." keeps its current landing tab.

**Pages (sub-tabs).**
1. **Start here.** Section "ProWindows": an `Info` row saying in one line what it does (tiles
   every window into a layout, workspaces, overlays), its help a short paragraph. Section
   "Try these": `Action` rows that *do* the thing - **Search** (value shows the live chord, e.g.
   Win+S; button "Open" → `LauncherToggle`), **Timer** ("Show" → set `timerShown`, as the tray
   does), **System monitor**, **Clock** (same, via the existing tray paths), **Settings
   folder**. Each with help text explaining it. Callbacks copy nothing from the `Row&` after
   calling out (inv. 79) and refresh with `SettingsRefresh`/`AppRefreshSettings` as the tray does.
   Section "Mouse": the mod+drag move / resize and drag-to-swap, as `Info` rows.
2. **Keys.** The shortcuts that matter, grouped by Section - Search and apps; Focus; Move;
   Workspaces (switch, send - one row each, "Win+1 ... 9" style if the binds are a run);
   Windows (close, float, fullscreen, sticky); ProWindows (pause tiling, reload, quit). Each an
   `Info` row: label = `DescribeAction`, value = the **live** chord from `Edit().binds`
   (`DescribeChord`), "Not set" when unbound; help = `HelpFor(action)` from
   `settings_keys.cpp`. Last row: `Action` "Change shortcuts" → the Shortcuts tab.
3. **Overlays.** Monitor, Clock, Timer: what each is, how to show / pin / move it (tray submenu,
   right-click, drag, Pin in place = click-through), each with a Show action.

Category blurb (the `PageDef` text): "What ProWindows does and the keys that matter. Come back
here whenever a shortcut slips your mind."

- [x] **6.1 (S)** Build it: `src/settings_welcome.cpp` (register in `build.bat` and every UI
  `tests\*.bat`), the enum, `g_pages`, `kTabOrder`, start-up and tray changes. `uishot` shoots
  each Welcome page. README "What's new in 1.6" gets one line; MAP a one-line note under the
  settings window heading.
- [x] **6.2 (O)** Review 6.1 and 5.D1 together. Passes inv. 67/74/78/79; Show actions rebase
  through `SettingsRefresh` (78). Fixed: a stale comment, and the first-run balloon that said
  "click the tray icon to set it up" over a settings window already open.

## Phase 7 — the Clock panel: Win+W, a mouse-first design, alarms (user, 2026-10-08)

The user: open the timer/clock with **Win+W**; the panel is "too keyboard focused" - model it
on the most popular desktop clock app, in the app's theme; and add **alarms** - once (this
hour, this day), every hour, daily, weekly, monthly, limited to chosen weeks and months, named,
"and everything else".

**Model: the Windows 11 Clock app**, in Requiem tokens (`theme.h`: `Bg` black, `Metal1..3`
gradients for the selected/pressed thing, `MetalEdge` frames, `TextHi/Text/TextDim/TextMute`,
`Warn` for ringing; `Font::Heading/Row/Small/Tab`; spaced caps via `Caps` + `SpacedWidth` /
`DrawSpaced`, inv. 77). Everything is clickable; every button has a hover and pressed state;
the mouse wheel changes any number under the pointer. Keys keep working (Tab/Ctrl+Tab pages,
Space, Esc, Enter, Del, digits) but the two-line footer of key prompts goes - no prompt is
shown that is not a real key (inv. 74 still holds trivially).

### 7.A Window and navigation
- One overlay window (the 5.D overlay: drag, pin = layered + click-through, `timerX/Y`,
  `timerShown`, game mode / display-off hiding). The tray "Timer" submenu is renamed
  **"Clock panel"**. Default size 880 × 560 DIP; DPI as 5.D (R2).
- **Left rail** (196 DIP): four pages with a drawn glyph + label: **Clock**, **Alarm**,
  **Timer**, **Stopwatch**. Selected = metal gradient pill + `TextHi`; hover = `Metal1` wash.
  The page is remembered in `timers.ini`. The empty rail area and the top 40 DIP strip drag the
  window (unpinned).
- **Top-right buttons** (drawn, 32 DIP square): a pushpin (toggles `timerPinned`) and × (hide,
  as Esc). A pinned panel is click-through, so unpinning stays in the tray / Settings, as for
  the monitor.
- Split painting out of `timer.cpp` (e.g. `src/clockpanel_paint.cpp`) once it passes ~400
  lines.

### 7.B Pages
- **Clock.** The local time very large (follows `clockHours24` / `clockSeconds`), the date
  under it, then two cards: "Next alarm" (name, when, "in 3 h 12 min") and "Running" (each
  running timer and the stopwatch with its live reading). Clicking a card opens its page.
- **Timer.** A grid of cards (2-3 columns by width, wheel scrolls). Each card: name, a
  **progress ring** (GDI+ anti-aliased arc, bright while running, `Warn` when done) with the
  remaining time large in its centre, and round **Play/Pause** and **Reset** buttons; hovering
  shows **Edit** (pencil) and **Delete** (bin). A round **+** bottom-right adds one.
  Quick-start chips above the grid: 1, 3, 5, 10, 15, 30 min, 1 h (click = new timer of that
  length, started). Max 8 timers stays.
- **Stopwatch.** The reading very large, centred (tenths while visible); three round buttons
  under it: **Start/Pause** (largest), **Lap** (flag), **Reset**. Laps table: Lap, Time,
  Total; the fastest lap `TextHi` with a "fastest" tag, the slowest `TextDim` with "slowest".
- **Alarm.** A scrolling list of alarm cards: time large, name, repeat summary ("Mon, Wed,
  Fri" / "Every hour at :15, 09-17" / "1st, 15th and last day · Jan, Jun" / "Once · Thu 9
  Oct"), "Rings in 3 h 12 min" on the next one, and a **toggle switch** on the right
  (enabled). Click a card to edit; hover shows Delete. **+** adds. Max 32 alarms.

### 7.C Editors
Drawn in-panel as a sheet over the page (`Bg`, `MetalEdge` frame); Esc / Cancel close, Enter /
Save commit.
- **Number spinners** for every time field: the value large, ▲ above and ▼ below; wheel and
  digits work; minutes and hours wrap.
- **Name**: a single-line text field (caret, select-all on focus, Backspace, Ctrl+A,
  Ctrl+Backspace, Ctrl+V, max 40 chars). Reuse the search bar's edit handling in
  `launcher.cpp` if it factors out cleanly; otherwise a small local one.
- **Timer editor**: name, days / hours / minutes / seconds spinners (days 0-9999), Save, Cancel.
- **Alarm editor**: name; time (hour, minute; AM/PM chip when 12-hour); a **Repeat** segmented
  control **Once · Hourly · Daily · Weekly · Monthly**; then, by repeat:
  - Once: the date (day / month / year spinners) with **Today** and **Tomorrow** chips. "Just
    this hour" = Once today at the chosen minute. A new alarm defaults to Once at the next
    whole hour (today, or tomorrow if that has passed).
  - Hourly: minute of the hour; optional **between** hour-from and hour-to (inclusive;
    default all day).
  - Daily: nothing more.
  - Weekly: seven weekday chips (Monday first), at least one.
  - Monthly: a 31-day grid + a **Last day** chip, at least one.
  - Every repeating kind has a **Limit to** section, collapsed by default: **Weeks of the
    month** chips (1st-5th, Last; none = all), **Months** chips (Jan-Dec; none = all),
    **Weekdays** chips for Hourly and Daily (none = all), and an optional **Until** date.
  - **Sound** toggle (off: banner + balloon only); **Snooze** 5 / 10 / 15 / 30 min; **Delete**
    (existing alarms only), Cancel, Save.

### 7.D The alarm model (`src/alarm.h/.cpp`, pure functions, tested)
- `struct Alarm { label; enabled; hour, minute; kind {Once,Hourly,Daily,Weekly,Monthly};
  date y/m/d (Once); hourFrom, hourTo (Hourly); weekdays (7 bits, Mon = bit 0); monthDays
  (bits 0-30 = days 1-31, bit 31 = last day); weeks (bits 0-4 = 1st-5th, bit 5 = last;
  0 = all); months (12 bits; 0 = all); until date (0 = none); sound; snoozeMin;
  snoozeUntilUtc; lastFiredUtc }`.
- Alarms are **local wall time** (timers stay UTC, inv. 88). `NextFire(alarm, nowUtc, conv)`
  returns the next UTC instant strictly after `max(nowUtc, lastFiredUtc)`, walking days (give up
  after ~5 years = never). `conv` is an injected local<->UTC converter: the app passes one built
  on `TzSpecificLocalTimeToSystemTime` / `SystemTimeToTzSpecificLocalTime`; tests pass fakes
  with a DST gap and a repeated hour, so they do not depend on the machine's zone. A local time
  inside a spring-forward gap rings at the first minute after the gap; a repeated hour rings
  once. Week of the month = `(day - 1) / 7 + 1`; "last" = `day + 7 > daysInMonth`. Days 29-31
  skip months without them; Last day always matches.
- A Once alarm disables itself after it fires (kept, so it can be re-armed). An alarm past its
  until date disables itself.
- **Waking** joins 5.B's single `SetTimer`: the next deadline is the earliest of timers, alarms
  and snoozes, clamped as before (inv. 90). `WM_TIMECHANGE` and resume recompute (alarms follow
  the local clock, so a moved clock matters to them).
- **Missed** (due more than 10 s ago when looked at: the PC was off or asleep): no ringing; one
  balloon "Missed while you were away" listing them, as the timers do.
- **Ringing**: the `StartAlarm` loop if `sound`; a tray balloon with the name; the panel shows
  (not activated, R3) on the Alarm page with a **ringing banner**: name, time, large **Snooze
  (N min)** and **Dismiss** buttons. The tray "Clock panel" submenu shows Snooze / Dismiss
  while an alarm rings (a timer's ring keeps "Stop alarm"). 60 s unanswered = dismissed.
- Storage: `timers.ini`, `a<i>.*` keys, the same `.new` + `MoveFileExW` rule (inv. 89).

### 7.E Win+W
- Default `{ L"win+w", ACT_TIMER, 0 }`; `DescribeAction` "Open the clock panel". Win+W is the
  shell's **Widgets** chord, so it only works through the keyboard hook, as Win+S does
  (`overrideReserved`); check the log says `chord busy, hooking: win+w`.
- Migration: a config written by a dev build may already be at version 5 (5.D note), so
  `kConfigVersion` → **6** and `kAdded` gets `{ L"win+w", ACT_TIMER, true }` "added in
  version 6".
- Win+W toggles: shows the panel (on the Clock page) if hidden, hides it if shown.
- Wording sweep: Welcome page (Keys → the clock panel row; Start here → "Clock panel" try-row),
  settings_keys help, ipc help, README What's new, MAP "Timer" heading → "Clock panel", tray.

### 7.F Tasks
- [x] **7.1 (S)** `alarm.h/.cpp` model, alarms in `timers.ini`, `tests\alarm_test.cpp` (run by
  `tests\run.bat`): every kind; weekdays, weeks of the month (incl. last), months, until; day 31
  in short months; last day of a leap February; DST gap and repeated hour with the fake
  converter; missed alarms reported once; Once disables after firing; snooze.
- [x] **7.2 (S)** The panel redesign (7.A-7.C), alarm firing / banner / tray (7.D), Win+W
  (7.E). `tests\timershot` shoots every page, both editors (the alarm editor once per repeat
  kind), the ringing banner, pinned and unpinned. Green: build, run, timershot, uishot.
- [x] **7.3 (O)** Review 7.1-7.2 against inv. 67/74/77/78/79/88-90 and "the UI thread never
  waits".

### 7.G Decisions
- **Idle wake ceiling: 1 h, not the deadline (7.3).** A moved clock or zone arrives as
  `WM_TIMECHANGE` (the hidden panel is a top-level popup, so the broadcast reaches it) and sleep
  as the resume call, plus display-on now calls `TimerCheckNow` for modern standby; so the
  ceiling is only there for drift between SetTimer's tick clock and the NTP-slewed wall clock.
  Over 24 days that drift can pass the 10 s "missed" line and turn a ring into a balloon; over
  an hour it is a fraction of a second. 24 wakes a day instead of 1440 (inv. 90).
- **Pinned panel + ringing.** `ApplyPin` drops WS_EX_TRANSPARENT/NOACTIVATE (keeps LAYERED) while an
  alarm or timer rings and restores them when it ends; dragging stays locked (reads `timerPinned`);
  show is still SW_SHOWNOACTIVATE (R3).
- **Past Once alarm.** `alarm::RollPastOnce` on Save moves a Once whose date+time is not after now to
  the next occurrence of that time (today if still ahead, else tomorrow); the editor shows a TextDim
  note under the date meanwhile (`OnceIsPast`). Saving still re-enables the alarm (decided).
- **Hourly window across midnight.** hourFrom > hourTo wraps (22-06 = 22,23,0..6); no swap on save;
  `NextAfter` tests `InHourWindow`, `RepeatSummary` prints the window as entered.
- **Name field.** Left/Right/Backspace/Delete step over a UTF-16 surrogate pair as one unit.
- **SystemZone per year.** `GetTimeZoneInformationForYear` + the `...Ex` converters, one rule table
  per year (16 cached, guarded), dropped by `InvalidateSystemZone` on WM_TIMECHANGE and a
  WM_SETTINGCHANGE "intl"/null (main window and panel both).
