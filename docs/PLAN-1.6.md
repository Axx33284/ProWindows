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
- [ ] **0.2** Delete regenerable build output: `tests/build/` (40 MB, includes stale
  `rowdragshot.exe` whose source is gone), `tests/shots/` (11 MB), `build/app.res`. All gitignored.
- [ ] **0.3** Check `git ls-files` for anything that should not be tracked (binaries, PNGs, `.res`).
  Report; delete only build output.
- [ ] **0.4** Move `docs/REVIEW-1.1.md` … `REVIEW-1.3.md`, `REVIEW-icons-and-idle.md`,
  `REVIEW-startup-and-input.md` to `docs/archive/` with `git mv`; fix links (`grep -rn REVIEW-1.[123]`).
  Keeps `docs/` to the current pass and stops agents from opening old reviews.
- [ ] **0.5** Stale references: `grep -rn "Battlefront\|DOOM" src res README.md MAP.md` (9 hits).
  Leave them for now — task 4.2 rewrites them to Requiem once the reskin lands.
- [ ] **0.6** Runtime data (`%APPDATA%\ProWindows`): `icons.cache` 780 KB (capped at 512 icons),
  `index.cache` 510 KB, `log.txt` truncated when debug turns on. Nothing to delete; see 3.3.

## Phase 1 — pre-release review (O)

- [ ] **1.1** Review the 1.5 diff (`git diff HEAD --stat`, then by file) against the invariants.
  Focus: `modal.cpp` (own message loop, inv. 82), `rowlist.cpp` (inv. 79), `settings.cpp`
  capture hook (inv. 81, 85), the merge (inv. 78), GDI handle balance in `theme.cpp` painters.
  Output: `docs/REVIEW-1.6.md` §A, findings P1–P3 with file:line.
- [ ] **1.2** S fixes every P1/P2 from 1.1; each fix ticked in REVIEW-1.6 with ✔.
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
- [ ] **2.T1 (S)** Palette and fonts in `theme.h/.cpp` (2.2, 2.3). Old tokens aliased until 2.T6.
- [ ] **2.T2 (S)** Painters: Choice-with-segments, Toggle-as-Choice, ruler Slider, open-in icon,
  section plate, metal `RowFocus`, keycap (2.5). Each painter answers `WM_PRINTCLIENT` paths as now.
- [ ] **2.T3 (S)** `settings.cpp` layout: title, tab bar with keycaps and active-tab light,
  sub-tab row, two columns, thin scrollbar, prompts in place of footer buttons (2.4, 2.8).
- [ ] **2.T4 (S)** `Kind::Page` and the page split; keys per the decision (2.6, 2.8).
- [ ] **2.T5 (S)** Right column: description, (Default: …), preview or mark, meters + `app.h`
  getters + harness stubs (2.7).
- [ ] **2.T6 (S)** Modal screens and search bar in the new look; delete dead amber code and the
  backdrop renderer (2.9, 3.1).
- [ ] **2.T7 (H)** Icon: change `gen_icon.py` colours as specified, regenerate `app.ico`,
  update `theme::Mark` colours.
- [ ] **2.T8 (S)** Update `tests\uishot.cpp`: the "only the focused row is amber" pixel check
  becomes "only the focused row has a metal bar" (luma of the row band ≥ 30 vs ≤ 8 elsewhere);
  add captures for a page switch and the meters. Then `tests\clickprobe.bat` (warn the user: it
  moves the mouse) — the X, minimise and the clickable prompts must work.
- [ ] **2.T9 (S, optional)** `requiem` skin for the monitor and the clock.
- [ ] **2.T10 (O)** Look at `ui-*.png` beside `docs/design/ref/*.webp` and list what still
  reads wrong (≤ 10 items, each with the fix). S applies them.

### Decisions

*(O writes here.)*

---

## Phase 3 — optimisation (S, measured; O reviews)

Measure before and after each change: private bytes and GDI/USER objects of the running app
(Task Manager columns or `GetProcessMemoryInfo` / `GetGuiResources` in a probe), and `monshot
--bench` for paint cost. Record the numbers in REVIEW-1.6 §C.

- [ ] **3.1** **Drop the backdrop picture.** `RenderBackdrop` renders a DIB at the *monitor's*
  size (a 4K screen is ~33 MB) and caches up to three (inv. 72). The Requiem screen is flat black:
  fill with `Bg`, draw at most a cheap gradient vignette. Removes the biggest allocation the
  settings window and the search bar make. Retire inv. 72 in MAP.
- [ ] **3.2** **Cheaper focus.** The amber `Glow` is per-pixel alpha work every animation frame
  (15 ms timer while fading); the metal bar is gradients. Confirm the fade timer stops when no
  row is fading.
- [ ] **3.3** **Icon cache.** 780 KB for ≤ 512 icons. Store only the size the search bar draws,
  and drop entries not seen for 30 days on save. Bump `kCacheVersion`; run `tests\iconcache.bat`.
- [ ] **3.4** **Focus-follows-mouse polls** every 120 ms (`main.cpp` `TIMER_MOUSE`), against the
  "never polls" rule. Replace with the `WM_MOUSE`-hook-only-while-needed pattern `moddrag.cpp`
  already uses, or `EVENT_OBJECT_LOCATIONCHANGE` on the cursor — O to choose; S implements.
- [ ] **3.5** **Meters must not cost.** The 1 s meter timer runs only while the settings window
  is visible and in front; getters read cached numbers, never walk anything.
- [ ] **3.6** **Binary size.** `dumpbin /headers` and a map file (`/MAP`) on `ProWindows.exe`
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
