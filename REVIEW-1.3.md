# ProWindows 1.3 — why it "just stopped", why every version looked the same, and what was added

A fourth pass over `src/`, after [REVIEW-startup-and-input.md](REVIEW-startup-and-input.md).
Asked four things: why the program works for a while and then stops until it is started again;
why 1.0, 1.1, 1.2 and 1.3 all appear to run as the same program; to add a desktop clock built the
way the monitor is built, with themes; and to redraw the settings window after DOOM Eternal's
menus. Verification is at the bottom.

---

## A. "It works at first and then stops"

### A1 — P0 — The process was being killed by its own tray tooltip ✔
`main.cpp`

The Windows Application event log on this machine has two entries for `ProWindows.exe 1.2.0`,
both exception `0xC0000409`, both at the same offset. Rebuilding the same sources with a linker
map puts that offset inside `_invoke_watson` — the CRT's invalid-parameter fast-fail. That is
what a `_s` function does when its buffer is too small, and no handler was installed to catch it:
no exception filter runs, the log stays silent, and the tray icon is simply gone.

`TrayTooltip` filled `NOTIFYICONDATAW::szTip` (128 characters) with `swprintf_s`. Every ordinary
state fit. The one that did not — **137 characters** — was game mode ("paused for fullscreen
app") together with at least one window running as administrator. So the tiler died at exactly
the moment a game went fullscreen while Task Manager, an installer or an elevated launcher was
open, which is a common evening, and the report reads "it worked and then stopped".

**Fix.** `_snwprintf_s` with `_TRUNCATE`, and shorter text. And, because invariant 14 already
forbade this and it happened anyway, `wWinMain` now installs an invalid-parameter handler that
logs the call and returns, which turns the same mistake anywhere else into an error code from the
function that made it. MAP.md invariant 57.

### A2 — P1 — Alt+Tab, Task View, the lock screen and the snipping overlay were "games" ✔
`winutil.cpp`, `wm.cpp`

`FullscreenAppActive` measures the foreground window, and on Windows 11 the shell's own surfaces
measure as full-screen. The log showed the consequence several times an hour: "fullscreen
application detected: pausing" on every Alt+Tab, with the overlay torn down and rebuilt, every
window event for the next 1.5 s dropped, and at logon (where an autostarted copy comes up behind
the lock screen) nothing arranged until the user unlocked. Shell processes are excused by name; an
empty `ApplicationFrameWindow` is not a game; and a window *we* put fullscreen with Alt+F no longer
pauses the tiler against itself. Invariant 58.

### A3 — P1 — A hung application froze the tiler on every workspace switch ✔
`wm.cpp`

`SetHidden` used `ShowWindow`, which waits for the other process. `ShowWindowAsync` now, with a
short grace period so the dead sweep does not mistake a window whose show is still queued for one
that has gone. Invariant 59.

### A4 — P2 — Windows written off as immovable stayed written off ✔
`wm.cpp`

Two refused placements is also what a busy application looks like. The verdict now expires after
a minute, and a window the user moves by hand has it cleared at once. Invariant 60.

### A5 — P2 — A dropped low-level hook stayed dropped ✔
`hotkeys.cpp`, `moddrag.cpp`

Windows silently removes a hook whose callback is late too often. Both hook threads re-install
their hook once a minute. Invariant 61.

## B. "All the versions run the same"

`build\ProWindows.exe` in the 1.3 folder was a byte-for-byte copy of 1.2's (same size, same
timestamp, same MD5) — 1.3 had never been built. And every version shares the single-instance
mutex, the window class and `%APPDATA%\ProWindows`, so starting any copy while another was running
just opened the *running* copy's settings. The autostart entry pointed at the 1.2 folder. Together
that is exactly "they all run the same".

**Fix.** The version string is 1.3.0 and is shown in the settings title bar. A different
executable that finds the mutex taken now offers to stop the running copy and take over, through
that copy's ordinary exit path, and the Run entry follows. Invariant 64.

## C. Added

- **The desktop clock** — `clock.*`, `clockpaint.*`, `clocktheme.*`, `settings_clock.cpp`, a
  Clock tab, two tray items, a context menu with the same snap points, pin and desktop mode as the
  monitor. Eight styles, twenty-one themes, the monitor's own panel chrome through a shared
  `PaintPanel` (invariant 62). Wakes on the second or minute boundary and skips any frame whose
  text has not changed.
- **The DOOM Eternal look** for the settings window — `theme.*`: the palette, Bahnschrift
  condensed capitals for headings, tabs and buttons, cut-corner plates, slanted tab plates with
  the active one filled orange, an orange rule with hazard stripes under the header, and a
  diagonal grain drawn as a screen-phased pattern brush so it runs unbroken under every control
  (invariant 63). The monitor and clock overlays keep their own themes.

---

## Verification

| | |
|---|---|
| `build.bat` | clean, no warnings from `src/` |
| `tests\run.bat` | **212 passed, 0 failed** |
| `tests\clockshot.bat` | every style and theme rendered to `tests\shots\clock-*.png` |
| `tests\clocklive.bat` | the real clock window, on the real desktop, one capture per style |
| `tests\uishot.bat` | every settings tab, Clock included, in the new look |
| Crash offset | `0x6bf9c` → `_invoke_watson`, from a map-file rebuild of the identical sources |
| Tooltip length | 137 > 128 in the failing state, 125 otherwise; truncated now |
