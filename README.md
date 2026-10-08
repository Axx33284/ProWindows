# ProWindows

A dynamic tiling window manager for Windows 11, in the spirit of Hyprland on
Arch. Windows you open arrange themselves automatically — no dragging, no
snapping, no overlap. Everything is driven from the keyboard.

Native C++ / Win32. One 1 MB executable, no runtime to install, no services,
no background polling. Everything is configurable from a settings window laid
out like the menus of *Resident Evil Requiem*, which opens when you launch it.

```
┌─────────────────┬───────────────┐
│                 │               │
│                 │   Terminal    │
│                 │               │
│     Browser     ├───────────────┤
│                 │               │
│                 │     Files     │
│                 │               │
└─────────────────┴───────────────┘
```

---

## What's new in 1.6

- **A Welcome page** opens with the app: what ProWindows does, things to try (search, timer,
  monitor, clock), and the keys that matter, read live from your own shortcuts. Tray: *Welcome
  and shortcuts...*.
- **A new look**, after the options screens of *Resident Evil Requiem*: black,
  categories as tabs across the top, pages within a category, a brushed-metal
  bar on the row you are on, and every setting's default shown beside it. See
  [Running it](#running-it).
- **The settings window's keys changed.** `Q` / `E` step categories and `1` /
  `3` step pages; search now opens with `/` or `Ctrl`+`F` rather than on any
  key. `R` resets a category and `Tab` resets everything, both asking first.
  Re-arrange is in the tray menu, and pausing is `Alt`+`P`.
- **Meters** for ProWindows' own memory, the windows it arranges, the file
  index, the icon cache and the monitor's sampling cost.
- **Focus follows the mouse without polling.** It used to check the pointer
  every 120 ms; it now waits for the pointer to move.
- **A smaller icon cache**: only the size the search bar draws is kept, and
  icons unused for 30 days are dropped - 813 KB down to 92 KB on the machine it
  was measured on.
- **A timer and stopwatch.** An overlay like the clock, shown from the tray's Timer
  menu or the Clock page of the settings (no default key; bind the `timer` action
  if you want one). Drag it, pin it in place. It has two pages, Timer and
  Stopwatch: up to 8 countdowns, and laps on the stopwatch. Both are anchored to
  the system clock, so they keep going through sleep, hibernation and restarts.
  An alarm can be turned on, and a tick sound (`timer_tick` = `off`, `second` or
  `minute`) can be heard while something runs.
- **Fixes:** Apply no longer loses your changes when `config.ini` cannot be
  written; a window that dropped out of the layout is put back; "Reset to
  defaults" could reset the wrong category; a taken shortcut confirmed during a
  config reload could remove the wrong binding; tapping `Alt` alone left the
  window in an invisible menu.

---

## Why it's light

| | |
|---|---|
| Executable | **1.0 MB**, statically linked — nothing else to install |
| Memory, tray only | **4.9 MB** private, with the file index off |
| CPU, tray only | **0 ms over 30 seconds**, three samples running — with animations *and* the keyboard hook active |
| Memory with the monitor and a 5,300-entry file index | **10.5 MB** private |
| CPU with the monitor | **31–47 ms over 30 seconds** on a quiet machine, **~94 ms** while it is busy — the glide only runs when a reading actually moves, so the panel costs most exactly when there is something to show |
| Threads | **4–6** at rest |

Measured on one machine, several samples each, not estimated. Your numbers will
differ — the file index in particular is proportional to how many files you
have, at roughly 300 bytes an entry.

**Where the memory goes, and what 1.3 did about it.** `prowindowsctl get memory`
prints the process's private commit, working set, GDI object count and every
heap's size, and the diagnostics report includes the same. Measured with it:

| | before | after |
|---|---|---|
| Base: tiler, search bar, index, icons, both hooks | 6.5 MB | 6.5 MB |
| + the monitor overlay (performance counters) | +4 MB | +4 MB |
| + GPU temperature on an NVIDIA card | **+19 MB** | **+2.8 MB** |
| Working set while idle | 65 MB | ~2 MB after the trim, growing back only to what is touched |

The 19 MB was NVML: `nvmlInit` commits that much driver state in the calling
process and never returns it, even after `nvmlShutdown`. The same sensor is
read through NvAPI now for 2.8 MB; NVML is only loaded on a driver too old to
answer NvAPI. `tests\gputemp.bat` measures both, and WMI, on your machine.

The rest is habits rather than one fix: the settings window is destroyed when
it is hidden rather than kept (it rebuilds in under 100 ms); the search bar
lets its icon bitmaps go after three minutes out of sight and reads them back
from `icons.cache` when next opened; the mouse hook behind `Alt`+drag is only
installed while the modifier is held, so the thousand-events-a-second stream
from a gaming mouse is not delivered into this process at all the rest of the
time; and once the process has been idle for a while it unloads COM libraries
nothing is using, compacts its heap and trims its working set - which is what
takes the number in Task Manager from 65 MB to a few.

The tray-only row is the honest floor: with the monitor closed and file search
off, the process does nothing at all between window events, and the CPU counter
does not move. Everything above that floor is something you switched on. The
monitor is the only part that costs anything *continuously*, and only while it
is on screen; the temperatures run a probe on a thread of its own, and
naming the busiest app snapshots every process once a second.

The glide is nearly free because it refuses to do pointless work: it does not
start at all unless a reading actually moved, and while it runs it skips any
frame that would land within a fifth of a percent of the one already on screen.
Without those two checks the same effect cost **1.0% of a core** — thirteen
times the idle figure — because an idle machine would still repaint the whole
panel thirty times a second to move nothing.

It never polls. The window layout is recalculated only when Windows tells it
something changed, through `SetWinEventHook` — the same out-of-process
notification mechanism screen readers use. No DLL injection, no hooks inside
other applications, no driver, no elevation.

**And it starts arranging the moment the window appears.** Every window event
used to wait out a 35 ms debounce before anything moved. That debounce exists
to coalesce a burst - one application opening four windows at once - and not to
slow down the ordinary case, which is one window on a desktop that has been
still for seconds. A request that arrives out of the quiet is now posted rather
than timed, so it runs on the very next trip through the message loop; a second
one within 200 ms goes back to being debounced, exactly as before.

The pass itself got cheaper too. Placing a window wanted three separate answers
from DWM - where the window is now, where it was when the animation started,
and how thick its invisible frame is - which are all the same question, asked
across a process boundary. They are asked once now: **four cross-process calls
per window per event down to one**, which on a board of ten windows is forty
round trips replaced by ten.

**Nothing slow runs on the thread that moves windows.** That thread is the one
that has to answer every window event, every shortcut and every animation
frame, and three things used to stall it:

- The monitor's readings. Opening the GPU performance counter takes **84 ms
  warm on this machine** and seconds cold, and every reading after that is a
  walk of every process's GPU engines. Both ran on the UI thread, the first one
  during startup, the rest once a second — so an animation that overlapped a
  reading hitched, and at logon the whole application sat frozen until the
  counters had opened. The readings come from a thread of their own now, at
  below-normal priority, and the UI thread only ever paints a finished one.
- The search bar's file index. Its cache — half a megabyte, six thousand
  entries — was parsed on the UI thread before the message loop had even
  started. That is on the index thread now. Startup to a running message loop
  went from **317 ms to 68 ms** on this machine, warm; at logon, with every
  other startup program on the same disk, the difference is whatever the disk
  felt like.
- Asking a window it already knows about for its size limits. Each ask is a
  cross-process message with a 60 ms ceiling, and at logon every application
  takes the whole 60 ms because it is busy starting. A window whose limits are
  already remembered from a previous run is not asked again.

**The keyboard and mouse hooks are scheduled ahead of everything.** Every
keystroke and every pointer movement on the machine passes through them before
the application it was meant for sees it. They already ran on threads of their
own; those threads now run at highest priority, so a keystroke never waits for
a timeslice behind the file indexer or a retile — which is exactly what typing
felt like on a busy machine.

**Focus is taken without borrowing the other application's input state.**
Bringing a window to the front from a process that does not own the
foreground used to attach this thread's input queue to the target's. Attached
queues share everything, including whether the pointer is shown — and Windows
hides the pointer while you type. Attach in the middle of a keystroke, detach
a moment later, and the hidden pointer stays hidden: that was "the mouse
disappears when I type". The fallback, a synthetic Alt press, was no better —
it lands in the menu bar in Explorer and Office, and Alt with Shift held is
the keyboard-layout switch. Both are gone. A mouse input event with nothing in
it satisfies the foreground rule on its own, and nothing is shared.

**The animation runs at the display's refresh rate, not faster.** It ticked
every 4 ms whatever the screen; on a 60 Hz display three frames in four were
moved and never shown, and each was still a `SetWindowPos` into every
application on the board — a relayout apiece. It reads the fastest attached
display's refresh rate and ticks once per refresh: 6 ms on the 180 Hz screen
it was tested on, 16 ms on a 60 Hz one.

---

## Build

Requires the **MSVC toolchain** ("Desktop development with C++" from Visual
Studio or the standalone Build Tools) and Python (only to regenerate the icon;
a pre-built `res/app.ico` is already committed).

```bash
build.bat
```

The result is `build\ProWindows.exe`. That single file is the whole
application — copy it anywhere you like.

---

## Running it

Launch the exe. The settings window opens and tiling starts immediately.

```
 SETTINGS                                        Tiling · 7 windows   —   ✕
 [Q] LAYOUT │ BEHAVIOUR │ GENERAL │ SHORTCUTS │ APPS │ SEARCH │ MONITOR │ CLOCK [E]
 ─────▀▀▀▀▀────────────────────────────────────────────────────────────────────
  ▛ Arrangement ▟━━━━━━━━━━━━━━━━━━━━━━━━━━━━   Every new window splits the one
  ▐▒▒ Arrangement      ‹     Dwindle      › ▒▒▌  you are focused on...
                             ▬ ─ ─                (Default: Dwindle)
    Main area size     ‹ - ┼┼┼┼┼┼│┼┼┼┼ + ›  55%   ┌────────────────────────┐
  ▛ Spacing ▟━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━   │  ██████  │  ░░░░░░░░░  │
    Gap between windows ‹ - ┼│┼┼┼┼┼┼┼┼┼ + ›  8   │  ██████  │  ░░░░░░░░░  │
    No gaps for a lone window ‹    On     ›      └────────────────────────┘
                             ▬ ─

            [R] Reset category   [Tab] Reset all   [Esc] Close
```

The window is laid out like the options screens of **Resident Evil Requiem**:
a pure black screen, a plain **SETTINGS** title, and the eight categories as a
row of tabs across the top, stepped with the keycaps at either end. Categories
with more to show split into pages - Monitor into *Display*, *Readouts* and
*Colours*, for example - with a second row of tabs under the first.

Below the tabs are two columns. On the left, every setting is one row: its name
in plain sentence case, its control on the right. Groups of rows sit under a
slanted grey plate. There are few kinds of control, and they always look the
same:

- a value between two thin chevrons, with a row of dashes under it - one dash
  per choice, the chosen one lit. Switches are drawn this way too, with their
  own two words: **On / Off**, **24-hour / 12-hour**, **Stacked / In a row**;
- a ruler with a marker for anything numeric, the number at the end;
- a shortcut, as keycaps;
- an action, its name with a small open-in icon.

The row you are on is the only thing lit: a bar of brushed metal across the
whole row, its name turned white. A row that does not apply right now - *Main
area size* when the arrangement is not Master - is greyed out.

The right column explains whatever has focus in plain English, then shows the
value it ships with - *(Default: Dwindle)* - so you always know what a reset
would give you. Under that, the Layout, Monitor and Clock categories show a
live preview, drawn by the same code that draws the real thing; the others show
the ProWindows mark. General, Search and Monitor end with meters, in the style
of the game's VRAM gauge: ProWindows' own memory use and how many windows it is
arranging, the size of the file index and the icon cache, and what the
monitor's sampling costs. They refresh once a second, and only while the window
is open and in front.

Along the foot are the game's button prompts - a keycap and a word - for
whatever you are on right now. Every prompt shown really does what it says, and
each one can be clicked.

It is driven the way the game is:

| | |
| --- | --- |
| `Q` / `E` | previous / next category (`Ctrl`+`Tab`, `Ctrl`+`PgUp` / `PgDn` too) |
| `1` / `3` | previous / next page, in a category that has pages |
| `↑` `↓` | move between rows; `↓` from the tabs goes into the rows |
| `←` `→` | change the value; on a row with nothing to change, `←` goes back to the tabs |
| `Enter` / `Space` | select: press an action, record a new shortcut |
| `Delete` | clear a shortcut, remove an entry from a list |
| `/` or `Ctrl`+`F` | search every setting in every category |
| `Ctrl`+`S` | apply |
| `R` | reset this category (asks first) |
| `Tab` | reset every category (asks first) |
| `Esc` | back: out of a search, from the rows to the tabs, then close |

With the mouse, the row under the pointer lights up; click a chevron or
anywhere along a ruler (and drag it); click a tab or a page; the wheel
scrolls.

**Search.** Press `/` or `Ctrl`+`F` and type: every category is searched at
once - *gap* finds both gap sliders, *No gaps for a lone window* and the
shortcut that hides the gaps. The results are the real rows, changed in place;
`Esc` leaves the search. Letters only go into the search once it is open, since
`Q`, `E`, `R`, `1` and `3` are the window's own keys.

**Nothing takes effect until you apply it.** A changed setting is marked as you
make it - a small diamond after its name - and the top right counts what is not
applied yet. `Ctrl`+`S` (or its prompt) makes it real and writes the config
file. If the file cannot be written - something else has it open - the settings
still take effect, and you are told they will not survive a restart. **Reset
category** and **Reset all** put settings back the way they shipped, still to
be applied. Closing with changes you have not applied asks whether to apply
them or throw them away. Apply only writes what *you* changed, so moving or
pinning the monitor from its own menu while the window is open is never undone
by it.

At the top right, the window says how the tiler is doing - *Tiling · 7
windows* - or, in red, when a shortcut is blocked or a window needs
administrator rights. Re-arranging now is in the tray menu, and pausing is
`Alt`+`P`. The window draws its own frame: drag it by the header, resize it
from any edge.

The search bar and the small question screens (*Apply your changes?*) are drawn
the same way. The monitor and the clock have their own themes and are not
touched by any of this.

The version is in **General**, under *About*. Several copies of this program
can live side by side on one machine, and they all share the single-instance
lock, so starting a newer copy while an older one is running used to silently
open the *older* copy's settings - which made every version look like the same
one. Starting a different executable now offers to stop the running copy and
take over, moving the start-with-Windows entry across with it.

`Esc` from the tabs, or the ✕, puts the window away while the tiler keeps
running; click the tray icon to bring it back.

Everything about setting the application *up*, as opposed to setting up how
windows behave, is in the **General** category: how it starts with Windows,
where it keeps its settings and how to open them, the diagnostics report, the
safety net that brings back every hidden window, and one button that puts every
setting back to how it shipped.

To start it with Windows, turn on **Start with Windows** there, and usually
**Start in the tray** with it. That writes a single
`HKCU\...\CurrentVersion\Run` entry — no scheduled task, no service, no admin
rights. The entry records where the executable is, so if you move the folder,
copy it to another machine or unpack a new release somewhere else, ProWindows
notices and corrects the path the next time it starts.

There is a third switch, **Start as administrator**, which is a different
mechanism and only worth using if you actually run applications as
administrator — see [Notes and limitations](#notes-and-limitations).

## Does it need a setup or installer?

No. `ProWindows.exe` is the whole program: statically linked, no runtime, no
service, no registry setup, nothing to install. Copy it anywhere and run it.

The first launch creates `%APPDATA%\ProWindows\config.ini` from built-in
defaults, so there is nothing you have to write by hand either. Everything else
it keeps — the file index cache, the log if you turn it on — lives in that same
folder. Deleting the folder resets it completely; deleting the executable
removes it, apart from the `Run` entry if you turned on *Start with Windows*.

If it behaves differently on one machine than another, the tray menu has, under **Tools**,
**Diagnostics report...**, which writes `%APPDATA%\ProWindows\diagnostics.txt`
and opens it: Windows build, DPI, every monitor, whether it is running elevated,
how many windows it is not allowed to move, and which of your keyboard shortcuts
another program has already claimed. That last one is the usual answer — the
shortcuts that work on one PC are silently taken on the other.

---

## Keyboard shortcuts

The modifier is **Alt** by default (`mod = alt` in the config). Alt is the
default rather than Win because Windows reserves a lot of `Win`+letter chords
for the shell — `Win+L` in particular can never be reassigned.

### Move around
| Keys | Action |
|---|---|
| `Alt` + `H` `J` `K` `L` — or arrow keys | Focus the window left / down / up / right |
| `Alt` + `O` / `Alt` + `I` | Focus next / previous window |
| `Alt` + `,` / `Alt` + `.` | Focus previous / next monitor |

### Rearrange
| Keys | Action |
|---|---|
| `Alt` + `Shift` + `H` `J` `K` `L` — or arrows | Move the window in that direction |
| `Alt` + `Ctrl` + `H` `J` `K` `L` — or arrows | Resize the split |
| `Alt` + `Enter` | Promote the window to the master slot |
| `Alt` + `Shift` + `,` / `.` | Send the window to the previous / next monitor |
| `Alt` + `E` | Flip this split: side by side, or stacked |
| `Alt` + `Shift` + `E` | Swap the two halves of this split |

You can also **drag a tiled window onto another one to swap them** - or **hold
`Alt` and drag anywhere on it**, which is the same gesture without having to
aim at a title bar. `Alt` + right-drag resizes it from the nearest corner.

### Windows
| Keys | Action |
|---|---|
| `Alt` + `V` | Float / unfloat |
| `Alt` + `F` | Fullscreen |
| `Alt` + `Q` | Close |
| `Alt` + `N` | Minimise |

### Layout and workspaces
| Keys | Action |
|---|---|
| `Win` + `S`, or `Alt` + `R` | Open the search bar |
| `Alt` + `Space` | Cycle layout |
| `Alt` + `G` | Toggle gaps |
| `Alt` + `1` … `9` | Switch workspace |
| `Alt` + `Shift` + `1` … `9` | Send the window to that workspace |
| `Alt` + `Ctrl` + `.` / `,` | Next / previous workspace that has windows on it |
| `Alt` + `` ` `` | Back to the last window, and again to come back |

### Control
| Keys | Action |
|---|---|
| `Alt` + `P` | Pause / resume tiling |
| `Alt` + `F5` | Reload settings from disk |
| `Alt` + `Shift` + `F5` | Rescan windows and re-arrange |

### Opening apps
| Keys | Opens |
|---|---|
| `Win` + `B` | Your default browser — detected, so Brave stays Brave |
| `Win` + `E` / `Win` + `F` | File Explorer |
| `Win` + `Q` | Windows Terminal (PowerShell if Terminal isn't installed) |

---

## Changing the shortcuts

These live in two categories, because they are two different things.

**Shortcuts** lists every window action - focus, move, resize, workspaces,
layout, windows, ProWindows itself - under those headings, each with its keys
drawn as keycaps. Select one and press `Enter`, the way the game rebinds a
control: the row says *PRESS A SHORTCUT* and shows the modifiers as you hold
them down; press the key and it is taken. `Win` chords work too, because while a
row is listening a keyboard hook holds every keystroke back from the shell and
from ProWindows' own shortcuts. If another shortcut already has that key you
are asked whether to move it here. `Esc` cancels, `Delete` clears. A key that
another program owns is tagged **BLOCKED**, and one that only works with the
Windows-shortcut takeover says so. An action whose key you cleared keeps its
row, *NOT SET*, so it can be given one again. **Put every key back** restores
the defaults without touching your app shortcuts.

Changing **Modifier key** moves every `$mod` shortcut at once, keeping any
custom keys you set.

**Apps** is for launchers — press a key, an app opens. **Add an installed app**
gives you a searchable list of everything in the Start menu, and **Add a
program or file** browses for anything else; either way the new row asks for
its key straight away. `F2` on a launcher changes what it opens. Add as many as
you like.

### Shortcuts Windows reserves

The Windows shell has already claimed nearly every `Win`+letter chord, and it
will not hand them over — on this machine `RegisterHotKey` was refused for all
but `Win+J` and `Win+Y`. So a binding on `Win+E`, `Win+F` or `Win+Q` is served
instead by a low-level keyboard hook that intercepts the chord before the shell
sees it, exactly as AutoHotkey and PowerToys do.

That hook is only installed when a binding actually needs one, and it only ever
intercepts those specific chords — everything else you type passes straight
through untouched, and it costs nothing measurable when idle. Turn it off with
**Take over Windows shortcuts** (in the Shortcuts and Apps categories); those
bindings are then marked rather than silently doing nothing.

Keystrokes the app synthesises are tagged so it ignores its own, which means
macro keyboards, on-screen keyboards and remote sessions still trigger your
bindings normally.

---

## Layouts

Press `Alt+Space` to cycle. Each workspace on each monitor remembers its own.

- **Dwindle** — Hyprland's default. A real binary space partition: every new
  window splits the focused pane along its longer edge, producing the familiar
  spiral. Splits are individually resizable with `Alt+Ctrl`+direction.
- **Master** — one large pane plus a stack, the classic dwm/tall layout.
  `Alt+Ctrl+Left/Right` grows and shrinks the master area.
- **Grid** — even rows and columns.

**Reshaping one split rather than the whole layout.** Dwindle splits the longer
edge of whatever you were focused on, which is right nearly every time and
wrong for the one pair in front of you. `Alt+E` flips that split between side
by side and stacked; `Alt+Shift+E` exchanges its two halves, and it exchanges
*subtrees* — one window on the left and three stacked on the right become three
on the left and one on the right, which swapping two windows cannot express.

## Workspaces

Nine per monitor, independent of Windows' own virtual desktops. Switching a
workspace hides the windows of the outgoing one and restores the incoming ones
exactly where they were.

Because inactive workspaces are hidden windows, there is a safety net: **tray
icon → *Tools* → *Show all hidden windows*** brings everything back at once, and the app
also un-hides everything when it exits, when Windows shuts down, and even if it
crashes.

---

## Configuration

Everything on the settings window is stored in
`%APPDATA%\ProWindows\config.ini`. The file is written by the settings
window, so the two never disagree — and it stays commented, so editing it by
hand is still pleasant. **Advanced (config file)…** opens it in Notepad; press
`Alt+R` afterwards to reload.

A few things live only in the file, because they are rarely changed: border
colours, corner style, animation duration, resize step, the master-window count,
and the float/ignore rules that match on window class or title rather than
program name.

```ini
mod       = alt          # alt | ctrl | shift | win (combine with +)
gap_inner = 8
gap_outer = 8
layout    = dwindle      # dwindle | master | grid

accent_border  = true    # coloured DWM border on the focused window
active_color   = #7AA2F7

animations   = true      # smooth movement instead of windows snapping
animation_ms = 140       # how long a rearrangement takes
```

### About the animation

Windows glide to their new positions on an ease-out curve rather than jumping.
While a transition runs the app raises the system timer resolution to 1 ms and
steps every 8 ms, so the motion is smooth rather than steppy; both are released
the moment it finishes, which is why idle cost stays at zero. Frame padding for
each window is measured once when the transition starts, so no frame pays for a
DWM round-trip.

Turn `animations` off (**Animations** in the Behaviour category) if you would rather have windows snap
into place instantly.

### Excluding applications

Use **Never arrange these apps** in the Behaviour category. Two ways to add
something, no typing required:

- **Add a running app** — a searchable list of everything currently open, shown
  as program name plus window title so you can tell two Chromium apps apart.
  Type to filter, `Enter` to add.
- **Add a program by file** — a normal file dialog, for apps that aren't running
  right now. Only the file name is stored, so it keeps matching wherever the
  program is installed.

Each excluded app is a row of its own; `Delete` (or its **Remove** button) takes
it off again. Nothing takes effect until you apply.

Behind the scenes this is the `ignore_process` line. The file adds two more
kinds of rule that the window doesn't expose:

```ini
ignore_process = obs64.exe, vmware.exe     # never touched at all
float_process  = Taskmgr.exe               # managed, but never tiled
float_title    = Picture-in-Picture        # substring match
```

Dialogs, fixed-size windows and owned pop-ups float automatically — they don't
need rules. Shell surfaces (Start, Search, widgets, the taskbar) are excluded by
a built-in list that is kept separate from yours, so it never clutters the
settings box and never gets duplicated when the file is rewritten.

### Rebinding

```ini
bind   = $mod+shift+return, promote
unbind = $mod+q
```

`clear_binds = true` drops every default first so you can start from scratch.
The full action list is in the generated config file.

---

## The search bar

`Win` + `S`. Type three letters. Press Enter.

`Alt` + `R` opens the same thing, for the Hyprland habit. `Win` + `S` is where
Windows itself puts search, so it is already in your fingers — it is a shell
chord, which means it arrives through the keyboard hook and needs **Claim
shortcuts the Windows shell reserves** left on (it is on by default).

The thing Hyprland gets right that Windows does not: you should never have to
keep a shortcut on the desktop, or hunt through a Start menu that wants to show
you web results. A search box appears in the middle of the screen, filters as
you type, and the first result is almost always the one you wanted.

```
╭──────────────────────────────────────────────╮
│ >  note                                      │
├──────────────────────────────────────────────┤
│ ▣    Notepad                            <--  │
│ ▣    Notes Board                             │
│ ▣    Node.js website                         │
│ ▣    NVIDIA Control Panel                    │
│ ▣    notes.txt                        file   │
│      C:\Users\you\Documents                  │
├──────────────────────────────────────────────┤
│ ↑↓ select   Enter open   Ctrl+Shift+Enter…   │
╰──────────────────────────────────────────────╯
```

**It knows about everything the Start menu does**, including Store and packaged
apps — it reads the shell's own `AppsFolder`, not just shortcuts on disk. The
list is built once on a background thread while the app starts, so the first
time you hit the chord it is already there.

**Real icons, and it still opens instantly.** Each row shows the icon the shell
itself would show, packaged apps included. Fetching one can touch the disk — on
a mechanical disk, tens of milliseconds each, and eight rows of that is most of
a second — so it is never done on the way to the screen. Three things between
them make it feel like it was always there:

- The window is created at startup rather than on the first chord, so pressing
  the shortcut has nothing left to do but show it.
- A background thread fetches icons; the row draws a lettered tile until its
  own arrives, and the panel repaints when it does.
- What was fetched is written to `icons.cache` and read back in one sequential
  pass next launch, and everything the cache does *not* hold is fetched anyway
  — in the background, at lowered disk priority, fifteen seconds after login.
  So the first run is the only one that ever draws a lettered tile.

**Programs, not just installed applications.** A `.exe` sitting in a folder — a
portable tool, a game unpacked somewhere, anything that never made a Start-menu
shortcut — is now a result in its own right, tagged `program`. Program Files,
Program Files (x86) and `%LOCALAPPDATA%\Programs` are walked for executables
alongside your own indexed folders, and the result is cached with the rest of
the index, so this costs part of one background walk and nothing thereafter.

It is deliberately not "every exe on the disk". An application's own executable
sits at the top of its folder; the things underneath it are its parts. So the
walk skips the folders that hold parts rather than programs, skips the
executables that exist to serve another executable (`unins*`, `*crashpad*`,
`vcredist`, and a dozen more), skips two-letter binaries — a developer
toolchain ships a whole POSIX userland, and `ex` and `ls` match almost any
short query while being almost never what you meant — and ranks what is left by
how deep it is buried. An application that already has a Start-menu entry shows
up once, not twice.

**Every drive, not just the one Windows is on.** A second disk is where games
and anything large actually live, and none of it is under Program Files or in
the Start menu — so `D:\SteamLibrary\steamapps\common\Game\Binaries\Win64\game.exe`
used to be invisible. Every **fixed** drive is now walked for programs.
Removable and network drives are not: `GetDriveType` answers that without
touching the disk, so a USB stick is never spun up and a mapped share is never
listed over the wire.

It costs nothing, and that is the part worth explaining. Walking a whole disk
six folders deep to reach a buried game executable took **143 seconds** here.
The folders that only ever *hold* programs — `Program Files`, `Games`,
`SteamLibrary`, `steamapps`, `common`, `Epic Games`, and a dozen more — are now
free of the depth budget, so the walk reaches that same executable while
spending four levels on the folders that are actually part of an application.
Measured on the same machine: **20.9 seconds**, against 20.5 for the old
Program-Files-only walk. Three drives, one cache, no extra cost.

If your programs live somewhere the walk does not reach, add that folder on the
**Search** category and it is indexed with everything else. `tests\searchprobe.bat`
runs the index on its own and prints what it found per drive, without starting
the tiler.

**And the helper executables inside apps, if you want them.** *Helper programs
too* in the Search category drops the rules that hide `unins000`,
`crashpad_handler`, `vcredist` and a toolchain's POSIX userland. Off by default,
because those are numerous and almost never what anybody meant — on when the
one you need is a helper.

**Nothing typed yet shows what you actually open**, most-used first, rather
than an alphabetical dump of everything installed. Until you have opened
something from here it says so, instead of offering you `About Java`.

**The keys are written along the bottom** — select, open, open as
administrator, close. Every one of them worked before; none of them was
discoverable.

**And four more things besides apps**, each of which can be switched off on the
**Search** category:

| | |
| --- | --- |
| Files and folders | Your Desktop, Documents, Downloads, Pictures, Music and Videos, indexed once on a background thread at startup. The row shows the containing folder underneath the name, so three files called `notes.txt` are still telling apart. |
| Windows settings pages | Type `blue` and jump straight to Bluetooth. About fifty pages — the ones people actually go looking for, not every leaf in Settings. |
| Calculator | Type `1920*0.75` and the answer is the first result; Enter copies it. Handles `+ - * / % ^`, brackets, and `sqrt floor ceil round sin cos tan ln log`, plus `pi` and `e`. |
| Run what you typed | Anything that looks like a path, a URL or a command line offers to run as typed — and if nothing else matched at all, that offer is the whole list. |

The file index is the only part with a running cost: it is held in memory, at
roughly 300 bytes an entry, so 20,000 files is about 6 MB. The **Search** category
shows the live figure and lets you change which folders are walked, how deep,
and where the ceiling sits — or turn file search off, which frees it entirely.

### On a mechanical disk

A recursive walk of your home folder is thousands of random seeks, which is the
one thing a spinning disk is worst at. Three things keep that off your back:

- **It is written down.** The index is saved to `%APPDATA%\ProWindows\index.cache`
  and reloaded on the next launch — one sequential read of a small file instead
  of walking anything. Measured here: **5,273 entries restored in about 110 ms**,
  versus a full walk. The cache is rebuilt only when it is a day old, when you
  change the folders or the ceiling, or when you press **Rebuild the index now**.
- **It waits for the login rush.** With no cache to load, the first walk holds
  off for twenty seconds rather than joining every other startup program in a
  fight over the disk. Apps are searchable the whole time; only files are late.
- **It yields.** The walk runs in `THREAD_MODE_BACKGROUND_BEGIN`, which lowers
  its **I/O** priority as well as its CPU priority, so Windows puts your own
  reads first. It is meant to be something you never notice running.

### On an old machine

The rest of the app was built for this and holds up: no polling, no injected
DLLs, no GPU work, and no runtime to load. `SetWinEventHook` is subscribed to
exactly the six events the tiler acts on, so the OS is not marshalling menu,
scrolling and capture events across a process boundary for us to discard. If
your machine is genuinely old, the two things worth turning off are the
**monitor** overlay (the only continuous cost) and **animations** in the
Behaviour category (which briefly raises the system timer resolution while they run).

**Matching is fuzzy but not silly.** A name that *starts* with what you typed
beats one that merely contains it, which beats one that only has the letters
scattered through it, and apps you open often drift to the top. Typing `note`
puts Notepad first, ahead of Notes Board and Release Notes.

| | |
| --- | --- |
| type | filter |
| `Enter` | launch the highlighted entry |
| `Ctrl` + `Shift` + `Enter` | launch it as administrator |
| `Up` / `Down` / `Tab` | move the highlight |
| right-click, or `Menu` | more, for the highlighted entry |
| `Esc`, or click away | close |
| `Ctrl` + `U` | clear the query |
| `Ctrl` + `Backspace` | delete the last word |
| `Ctrl` + `V` | paste |

**Right-click any result** for the things you would otherwise open a File
Explorer window to do:

| | |
| --- | --- |
| Open | same as Enter |
| Run as administrator | elevates the shortcut's *target*, not the shortcut |
| Open file location | Explorer, with the executable already selected |
| Copy path | the resolved target path, on the clipboard |
| Forget this app | drops it back down the ranking |

The two greyed-out entries are for Store and other packaged apps: they are
reached through a shell moniker rather than a file, so there is nothing on disk
to elevate or to show you.

Rebind it like anything else in the **Shortcuts** category, or in the config file:
`bind = win+s, launcher`.

---

## System monitor

A floating readout of what the machine is doing: **CPU, memory, GPU, disk and network**, each with
a live value, a bar, and a history graph. It is a layered window with real rounded corners and
adjustable translucency, not a grey box.

```
╭──────────────────────────────╮
│ CPU                      38% │
│ ▁▂▄▆▅▃▂▁▂▄▃▂▁▁▂▃▄▅▄▃▂▁▂▃▄▅▄ │
│ ████████░░░░░░░░░░░░░░░░░░░░ │
│ MEMORY              10.4 GB  │
│ of 31.6 GB                   │
│ ██████████░░░░░░░░░░░░░░░░░░ │
╰──────────────────────────────╯
```

**Eight readouts:** CPU, memory, GPU, **GPU memory (VRAM)**, **CPU temperature**,
**GPU temperature**, disk and network. VRAM is real dedicated video memory — used against the
adapter's actual size, read from DXGI.

**The two temperatures** are the one place Windows has no single answer, so each row also says
where its number came from.

*GPU temperature* is the easy one: NVIDIA's `nvml.dll` and AMD's `atiadlxx.dll` both ship with the
driver and both report the die sensor to any user, so on either card the row reads exactly what
the vendor's own tool would show, with no elevation and nothing to install. The row is labelled
`nvidia` or `amd` accordingly.

*CPU temperature* is harder, and worth being straight about. Reading the CPU package sensor means
reading a machine-specific register, which means a kernel driver — this app does not ship one and
will not ask you to install one. The only honest way to have one is to use the driver you have
already installed and trusted, so if any of these is running its reading is used, and the row says
which:

| Source | Row says | Needs |
|---|---|---|
| **LibreHardwareMonitor** / OpenHardwareMonitor | `package` | nothing — just leave it running |
| **HWiNFO** | `hwinfo` | *Shared Memory Support* switched on in its settings |
| **Core Temp** | `core temp` | nothing — just leave it running |

All three are picked up while ProWindows is already running, so starting one of them mid-session
simply upgrades the reading. The two shared-memory ones cost a `memcpy` to check, so they are
re-checked on every pass rather than once a minute.

Failing all of those, every machine publishes its ACPI thermal zone, and that is the fallback,
labelled `thermal zone`: on a laptop it tracks the CPU closely, on a desktop it is often a
mainboard sensor that barely moves. **The zone is not the package**, which is exactly why the row
tells you which one you are looking at rather than presenting them as the same claim.

Where a machine cannot answer at all the row says so rather than inventing a number: `no sensor`
when nothing on it exposes one, and `needs admin` in the one remaining case where a sensor exists
but is not readable unelevated.

To find out what *your* machine can answer without running the whole application,
`tests\tempprobe.bat` prints the reading and its source and stops.

> Earlier versions read the thermal zone through the `MSAcpi_ThermalZoneTemperature` WMI class,
> which needs administrator — so run normally, the row said `needs admin` and never showed a
> number. It now reads the same zone through the performance counter of the same name, which does
> not.

**The busiest app per meter.** Turn on *Name the busiest app* and each readout says which process
is responsible: the top process by CPU, by working set, by disk I/O, and — via the per-process GPU
engine counters — by GPU. Network has no cheap per-process source, so it stays blank. This is the
one option here that costs meaningfully more: it snapshots every process on the machine each tick,
so it is off by default.

**Put the readouts in whatever order you like — by dragging them.** Hold the
mouse on a readout for a third of a second and it lifts off the panel; drag it
up or down (or across, in a horizontal panel) and the others close up around
the slot it would land in, marked with a dotted outline; let go and it drops
there. **Ctrl+drag** lifts it at once, without the hold. A press that moves
straight away still drags the whole panel, exactly as before, so the two never
get in each other's way. In the HUD style, where a device's readouts share one
line, the line is what you pick up.

The **Monitor** category's readout rows reorder too — drag a row by the grip at
its left edge, or hold `Ctrl` and press `Up` or `Down` — and the right-click menu
still opens each readout onto its own switch and four moves. Which readouts are
*shown* is a separate thing from where they sit, so hiding one and bringing it
back puts it where it was rather than at the end. The order is stored by name
(`monitor_order = cpu, gpu, ram`), so it survives anything being added to the
panel later.

**Drag it anywhere.** Its position is saved the moment you let go, so it survives a restart — or a
crash. Or don't drag it: **Move to** in the right-click menu has the nine
places you were probably aiming for — the four corners, the four edges and the
middle of whichever screen it is already on.

**Pin it** and two things happen: it can no longer be dragged, and clicks pass straight through to
whatever is behind it. That is the setting to use once it is where you want it, so you never nudge
it by accident while reaching for something underneath. Unpin from the tray menu or the Monitor
category (a pinned monitor can't be right-clicked, by definition).

**Or send it to the desktop.** *Sit on the desktop, behind every window* drops the panel to the
bottom of the z-order, one step above the wallpaper: it is there when the desktop is clear and
every window covers it, so it stops being something you have to work around. It keeps its real
translucency and rounded corners either way.

**Ten styles**, independent of the colours — any style can wear any theme:

| | |
| --- | --- |
| **Rows** | the default: name, reading, history graph and a bar, one row each |
| **Cards** | each metric on its own raised card with a coloured rail down the left |
| **Compact** | one dense line per metric — name, bar, reading. The smallest panel |
| **Rings** | a circular gauge per metric with the reading in the middle |
| **Arcs** | dial gauges with a 240° sweep and the name underneath |
| **Bars** | vertical column meters side by side, like a mixing desk |
| **Graph** | chart first: a large history graph per metric, reading overlaid |
| **Ticker** | a single thin line — a dot, a name and a number each, for a screen edge |
| **OSD** | an in-game on-screen display: monospace text, one line per readout, label in its colour, sparkline beside it |
| **HUD** | the benchmark-video layout: one line per *device* — `GPU  24%  69°  3.8 GB` — with the device tag in colour and the numbers big and white |

The last two are the two looks nearly every in-game overlay converges on. The
in-game OSD is a column of monospace text with each item on its own line;
the benchmark-video HUD groups everything about the GPU on one line and
everything about the CPU on the next, so the eye reads a device at a time. Both
were made to go with the two **bare** themes below, which draw no panel at
all — just the text, with a shadow under it so it reads on any wallpaper — but
either works on any theme, glass and all.

**It reads as a panel, not a rectangle.** The whole thing now sits on a soft drop shadow, which is
what separates it from a busy wallpaper. It costs nothing per frame: the shadow, the gradient, the
gloss and the border are the same pixels every frame, so they are drawn once into a bitmap and
blitted — which is also why the panel now paints roughly three times faster than it did before the
shadow existed.

**A reading close to its ceiling warms up.** Above about 72% a bar, ring or column slides towards
amber and then red, so a pinned CPU is visible without reading the number. The text keeps its own
colour — moving both made the panel look like it was flickering — and the themes whose whole point
is a single hue (Graphite, Terminal, Amber) opt out entirely.

**The CPU bar is one segment per thread.** An averaged bar cannot tell one pinned thread from a
machine that is evenly busy, and those mean opposite things: the first is a program stuck in a loop
and the second is a machine working. Each logical processor gets a segment of the same bar, and its
brightness is its load — so one core at 97% in a row of idle ones is visible at a glance, in
exactly the pixels the single bar used to occupy.

Past about thirty threads the segments stop being distinguishable, so cores are folded together in
pairs, then fours, until they fit — and a folded segment shows the **busiest** of its cores, not
their mean. Averaging is what the plain bar already did. `monitor_cores = false` in the config file
puts the single bar back.

**Graphs mark their peak.** A faint dotted line sits at the highest point of the visible window,
because a graph that has been flat for a minute and one that spiked thirty seconds ago look the
same once the spike has scrolled into the middle distance.

**A frame nothing would change is not drawn.** The panel compares a signature of everything it is
about to paint — sizes, colours, eased percentages, and the text of every reading — against the
last frame, and skips the repaint when they match. On an idle machine that is most of them.

**Any colour you like, per metric.** The theme sets a colour for each readout, and the
**Colours** rows in the **Monitor** category override it — step through a palette with `←` `→`,
or press `Enter` for any colour at all. *Theme* is the first choice on each row, and *Every colour
from the theme* puts them all back. Metrics you have not touched keep following the theme, so
switching from Midnight to Nord still recolours everything except your own choices. The config
file stores them one per line (`monitor_color_gpu = #FF3B30`, or `theme`).

**It glides rather than jumps.** A once-a-second sample used to move the bars and gauges in
once-a-second steps. They now ease into each new reading over 300 ms, using the same curve the
window animation uses. *Glide between readings* in the right-click menu turns it off.

**Right-click it** for everything else without opening settings: which metrics to show and in what
order, where on the screen it sits, the style, the theme, graphs on or off, the busiest app, the
glide, vertical or horizontal, pin, desktop, hide.

**Twenty-one themes.** The panel, the border, the text, the bars and the graphs all come from the
theme, so each one is a genuinely different readout rather than a recoloured accent:

| | |
| --- | --- |
| **Frontline** | the default, and the settings window's look: black glass inside a hairline, white readings, amber for the processor |
| **Holonet** | square dark glass, white readings, a restrained colour per metric |
| **Hologram** | a blue projection: every readout in one cyan, like a holotable |
| **Midnight** | near-black glass, one colour per metric |
| **Graphite** | no colour at all; the readouts are told apart by weight |
| **Nord** · **Dracula** · **Solarized** · **Gruvbox** | the familiar editor palettes |
| **Tokyo Night** · **Catppuccin** · **Rosé Pine** | the newer ones — indigo, pastel, and muted rose |
| **Ocean** · **Ember** | deep navy with cyan, or charcoal warmed by orange |
| **Terminal** · **Amber** | one colour on black, square corners, like a phosphor screen |
| **Neon** | saturated cyan and magenta, big corners |
| **Frost** · **Paper** | light panels with dark text, for a light wallpaper |
| **Overlay** · **Benchmark** | *no panel*: text straight on the screen. Orange, or green GPU / blue CPU / white numbers |

Pick a theme and a style from the overlay's right-click menu, or from the **Monitor** category, which
previews the pair live — at the current opacity, with the style and the busiest-app lines you have
selected — before you Apply. The config file stores names (`monitor_theme = nord`,
`monitor_style = rings`), not numbers.

The **Monitor** category also has opacity, size, and how often it samples. It only samples while it is
on screen — hide it and the cost returns to zero.

Where the numbers come from: CPU from `GetSystemTimes`, memory from `GlobalMemoryStatusEx`,
GPU / VRAM / disk / network from the same performance counters Task Manager reads, total VRAM from
DXGI, per-process figures from a single `NtQuerySystemInformation` snapshot, GPU temperature from
NVML or ADL, and CPU temperature from LibreHardwareMonitor if it is running and the
`Thermal Zone Information` performance counters otherwise — the last two on a thread of their own.
A counter your machine does not expose shows `n/a` rather than a made-up zero.

---

## Desktop clock

The monitor's sibling: the same layered window with the same glass, dragged,
pinned and sent to the desktop the same way, showing the time instead of the
load. Turn it on from the **Clock** category or the tray menu. It repaints once a
minute - once a second with the seconds on - and costs nothing while it is
hidden.

```
╭──────────────────────────╮
│                          │
│   10:08  42 AM           │
│   Friday, 18 September   │
│                          │
╰──────────────────────────╯
```

**Twelve styles**, each a different kind of clock rather than a recolouring:

| | |
| --- | --- |
| **Digital** | the default: the time, large, the date underneath |
| **Minimal** | very thin type and small tracked capitals, the wallpaper-clock look - made for the two bare themes below |
| **Analog** | a round dial with sixty ticks, three hands and the date in a small pill |
| **Flip** | split-flap tiles, one digit each, with the hinge line across the middle |
| **Segments** | seven-segment LED digits with the unlit segments faintly showing, leaning slightly, like an alarm clock |
| **Stacked** | hours over minutes at a size that fills a corner, the weekday, day and month down the side |
| **Wide** | a single thin strip - weekday, date, time - for the top or bottom edge of a screen |
| **Ring** | the time inside a ring that fills as the minute goes by |
| **Roman** | a dial with serif Roman numerals, a dotted minute track and slim hands |
| **Station** | the railway clock: bold numerals, heavy bar markers, a red second hand with a disc on its tip |
| **Dial** | a bare dial - four markers, thin hands, no rim - for the Glass and Ink themes |
| **Classic** | a framed readout: a double rule above and below the time, the date in small capitals |

The Flip style animates: when a digit changes, the top half of the old digit
folds down about the hinge and the bottom half of the new one unfolds beneath
it, over 420 ms at 40 frames a second, then the board is still again.

**Twenty-four themes**, and the theme chooses the typeface as well as the colours,
because a phosphor terminal wants a monospace and a paper calendar wants a serif:

| | |
| --- | --- |
| **Frontline** | the default, matching the settings window: thin wide digits on black glass inside a hairline, an amber second hand |
| **Holonet** | thin wide digits on square dark glass, a gold second hand |
| **Hologram** | glowing cyan digits on deep blue, square |
| **Midnight** | near-black glass, white digits, a blue second hand |
| **Glass** · **Ink** | *no panel*: white or black digits with a soft shadow, straight on the wallpaper |
| **Slayer** | gunmetal and hazard orange in condensed capitals |
| **Nixie** · **Neon** | glowing amber in a dark tube; cyan tube light with a magenta second hand |
| **Terminal** · **Amber** · **LCD** | green or amber on black, or dark segments on a grey-green LCD |
| **Paper** · **Frost** | a white card with a red second hand; light glass for a light wallpaper |
| **Graphite** · **Nord** · **Dracula** · **Solarized** · **Gruvbox** · **Tokyo Night** · **Catppuccin** · **Rosé Pine** · **Ocean** · **Ember** | the palettes the monitor has, so the two can match |

The digits and the date come from the locale, so a machine set to German
writes `Freitag, 18. September`. **24-hour**, **seconds**, **the date** and
**the day of the week** are each their own switch, in the settings or in the
clock's right-click menu, which also has the nine snap positions, the styles
and the themes. The panel follows the text - `9:59` is narrower than `10:00` -
and a clock in a corner grows towards the middle of the screen rather than
off its edge.

`tests\clockshot.bat` renders every style and theme to `tests\shots`;
`tests\clocklive.bat` runs the real window for a few seconds and captures it
off the screen.

---

## Screen space for a bar

Windows 11 only supports its taskbar along the bottom edge — Microsoft removed the option to move
it, and the old registry tricks are ignored (verified on build 26200). Only a tool that patches
Explorer, such as ExplorerPatcher, can move it, and this app deliberately will not do that.

What it does offer is on the **Layout** category: reserve pixels on any edge, and tiled windows keep
clear of them. If you run a bar of your own, anywhere on screen, the tiling fits around it.

---

## Notes and limitations

- **Drag a window to move it, and it lands where you aimed it.** Pick a tiled
  window up by its title bar and drop it on the **left, right, top or bottom**
  half of another one, and it takes that side of it — the window that was there
  moves over to make room. Drop it on the right and it goes on the right; it
  will not decide to go underneath instead.

  While you drag, a translucent rectangle shows exactly which space the window
  is about to take, so a drop near a corner is never a guess. Each window is cut
  along its diagonals into four triangles, and the pointer takes the one it is
  standing in — so the whole right-hand wedge of a tile means "right", corners
  included, and there is no dead zone in the middle.

  Drop on bare desktop, in a gap, or on an empty workspace and the window takes
  that whole edge of the screen instead — a full-height column down the side,
  or a full-width row across the top or bottom. Drag it onto another monitor and
  it lands there, placed the same way. Dragging a window's *border* resizes it
  as usual and never rearranges anything.

  Turn the whole gesture off with **Drag to rearrange** on the Behaviour category (`drag_to_rearrange` in the config), and
  a dragged window just snaps back where it was.

- **Or hold `Alt` and drag anywhere on the window.** Windows gives a window
  exactly one drag handle, and a tiled window's title bar is a few pixels tall
  to aim at - if it has one at all. Holding the modifier makes the whole window
  that handle: left to move it, right to resize it from the nearest corner.

  It does not run a drag of its own. It hands the window the same message a
  real title-bar click delivers, so what follows is Windows' own move loop and
  everything above - the drop indicator, the drop side, dragging onto another
  monitor - happens exactly as it does from the title bar.

  It only ever picks up windows ProWindows is arranging, which means an app on
  the **Never arrange these apps** list keeps its own `Alt`+drag - that is
  rather the point of having excluded it. Turn it off entirely with **Alt + drag to
  move** on the Behaviour category
  (`mod_drag` in the config).

- **Games and fullscreen apps stop it completely.** While anything is running
  fullscreen — a game, a video, a presentation — ProWindows does nothing at all:
  no arranging, no animation, no focus borders, and the system monitor overlay
  is taken down. Everything it normally does is a cross-process call that costs a
  game frames, and on some drivers a window-attribute change is enough to drop it
  out of exclusive fullscreen. It notices borderless-windowed games as well as
  true fullscreen ones, and picks up again within a second or two of you leaving.
  Turn it off with **Pause for fullscreen apps** on the Behaviour category if you
  ever need to.

- **Windows that run as administrator need ProWindows to as well.** Windows will
  not let a normal program move a window belonging to an elevated one, and it
  fails silently rather than reporting anything. Those windows are left out of
  the layout — no tile is reserved for them, so the rest of the screen still
  fills up properly — and the count is shown in the tray tooltip and the status
  line, so a window sitting on top of everything is explained rather than
  mysterious.

  To include them, the tray menu has, under **Startup**, **Restart as administrator** for right now,
  and **Always start as administrator** to make it stick. The second one
  registers a logon task with Windows Task Scheduler, which is the only way to
  start elevated without a UAC prompt every single time; creating it asks for
  administrator rights once and never again. The same switch is in the General
  category, under Startup.

- **Windows that won't fit are given room, not squeezed.** Some applications
  refuse to go below a minimum size — Steam is the usual example. Instead of
  handing one a slot it will overflow, the layout asks each window what it will
  accept and moves the split: the window gets the width it needs and its
  neighbour gives it up. It works the other way too, so a window that refuses to
  grow hands its surplus to whoever can use it rather than leaving bare desktop.

  A split can only pass surplus along its own direction, so a window with a
  maximum *height* sitting in a side-by-side split used to leave the space
  under it empty. The layout now turns such a split the other way round when
  that lets the windows under it use more of the screen — the short window
  takes its row, its neighbour takes the rest — and does the same when two
  windows cannot stand beside each other but can stack. Splits with nothing
  limited beneath them are never touched.

  When even that is not enough — the window's minimum is larger than the screen,
  or no arrangement of the others leaves it what it needs — it is taken out of
  the tiling and left floating where you put it, rather than allowed to overhang
  and cover its neighbours. The check is exact: every window in the plan is
  compared with its own minimum before anything moves, so a window is never
  handed a slot it will refuse. It comes back into the tiling by itself as soon
  as there is room: close a window, or move it to a bigger screen.

  These minimums can only be discovered by handing a window a size and watching
  what it does with it, so they are remembered in `config.ini` (the `learned =`
  lines) and applied straight away next time. Delete a line to make ProWindows
  measure that application again.

- **Transient dialogs float instead of taking a tile.** A file-copy or delete
  progress window looks tileable — it is top-level, titled and has a resize
  frame — but it keeps its own small size, so tiling it left most of a slot
  empty until it went away. Those float now, along with anything else that turns
  out not to use the space it is given.
- **Workspaces are the app's own**, not Windows' virtual desktops. Windows on
  *other* Windows virtual desktops are ignored, so the two coexist safely.
- Store/UWP apps are handled through their `ApplicationFrameWindow` host and
  tile normally; suspended ones are ignored until they wake.
- Multi-monitor and mixed-DPI setups are supported (per-monitor DPI aware v2).
  Monitors are ordered left to right, so "next monitor" matches what you see.

---

## Project layout

```
tests/           layout geometry assertions, and a read-only window probe
src/common.*     paths, logging, shared types
src/config.*     INI parsing, keybind and action grammar
src/defaults.cpp writes config.ini back out, preserving custom bindings
src/winutil.*    Win32 helpers: classification, DWM frame-accurate placement
src/layout.*     BSP tree and the four layout algorithms
src/wm.*         monitors, workspaces, event handling, all the actions
src/hotkeys.*    RegisterHotKey, plus the keyboard-hook fallback
src/ipc.*        the control channel: named pipe, command grammar, replies
src/ctl_main.cpp prowindowsctl.exe, the console front end to the control channel
src/sysinfo.*    CPU / RAM / GPU / disk / network sampling
src/launcher.*   the search bar: its window, the app catalogue, the row menu
src/search.*     what the search bar finds: file index, settings pages, calculator
src/montheme.*   the overlay's colour schemes and styles, as tables of data
src/monpaint.*   the overlay's geometry and painting, one function per style
src/monitor.*    the floating system-load overlay: window, z-order, menu
src/clocktheme.* the clock's colour schemes, typefaces and styles, as tables of data
src/clockpaint.* the clock's twelve layouts, one function each, measured and drawn together
src/clock.*      the clock window: the same drag, pin, desktop and snap as the monitor
src/dragguide.*  the drop indicator shown while a tiled window is dragged
src/theme.*      the Requiem look: palette, type, the metal focus bar, plates, keycaps, the row controls
src/moddrag.*    hold the modifier and drag anywhere on a window
src/rowlist.*    a list of settings rows: layout, painting, focus, keys, mouse, scrolling
src/modal.*      the question, notice and pick-one screens, in the same look
src/settings.*   the settings window: header, categories, panel, description, footer, Apply
src/settings_pages.cpp   the Layout, Behaviour and General categories
src/settings_keys.cpp    the Shortcuts and Apps categories
src/settings_search.cpp  the Search category
src/settings_monitor.cpp the Monitor category
src/settings_clock.cpp   the Clock category
src/app.h        the few services the settings pages need from the shell
src/main.cpp     entry point, tray UI, event hooks
res/app.rc       icon, manifest and version (the settings window has no dialog templates)
res/gen_icon.py  regenerates res/app.ico from code: the tile mark on a cut plate, in the settings window's palette
docs/            the review notes, one per pass: what was reported, what was found, what changed
build.bat        one-step MSVC build
```

See [MAP.md](MAP.md) for how it all fits together, the invariants worth knowing, and the
things that surprised us along the way.

The settings window keeps no state of its own. It edits a copy of the live
`Config`, and Apply copies back only the settings that were changed, then saves
and reloads through the ordinary config path — so changing the modifier key
re-registers every hotkey, and changing a rule re-classifies every window, with
no separate code path to fall out of sync.

The one Win32 subtlety worth knowing about is in `PlaceWindow()`: Windows 11
windows have an invisible resize border, so `GetWindowRect` is several pixels
larger than what you actually see. Every placement is therefore corrected by the
difference between `GetWindowRect` and the DWM extended frame bounds, which is
why the gaps come out visually even instead of subtly wrong.
