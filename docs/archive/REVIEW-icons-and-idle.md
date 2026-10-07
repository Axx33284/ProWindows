# ProWindows 1.3 — the upside-down icons, and a pass over the memory work

A fifth pass over `src/`, after [REVIEW-1.3.md](REVIEW-1.3.md). Asked to review the application,
with one symptom named: the icons in the search bar are upside down. The symptom is new since the
memory commit (`8a0dd5a`), so that commit was read in full alongside the search bar and its icon
loader. Verification is at the bottom.

---

## A. The reported symptom

### A1 — P1 — Every icon read back from `icons.cache` was inverted, on every second save ✔
`appicon.cpp`

`ReadPixels` decided whether a bitmap's rows were stored bottom-up or top-down by looking at
`DIBSECTION::dsBmih.biHeight`. That field is **positive for every DIB section**, whichever way up
it was created — measured on this machine with a bitmap made through `CreateDIBSection` with
`biHeight = -28`, which `GetObject` reports back as `28`. So the test always said "bottom-up" and
the rows were always turned over on the way out.

That is right for a bitmap straight from the shell (`IShellItemImageFactory::GetImage` hands back
bottom-up DIBs; also measured) and wrong for one `MakeBitmap` has just restored from the cache,
which is top-down. The table holds both kinds and the code could not tell them apart. So:

- **Launch 1.** Icons fetched from the shell, drawn correctly, saved correctly.
- **Launch 2.** Icons restored (top-down), drawn correctly. Anything new is fetched, the table is
  dirty, and the save turns every *restored* icon over: the file is now inverted.
- **Launch 3.** Upside down. The next save turns them back. And so on.

Until the memory commit that needed a restart to happen. Since it, the search bar releases its
bitmaps after three minutes hidden — save, free, reload on demand — so the flip now happens
within a session, about three minutes after the bar was last used. That is why it appeared now.

**Fix.** The orientation is decided once, at fetch time, where the source is known and the bitmap
is not yet shared with anyone. `TopDownCopy` reads the shell's bitmap through `GetDIBits` with a
negative height — GDI knows the source's real orientation and turns it over itself, and for a
32-bit source it copies the alpha byte through untouched (measured, every pixel identical to the
mirrored memory) — into a fresh top-down DIB section, and deletes the shell's. Every bitmap in the
table is now top-down and `ReadPixels` copies rows in memory order. The cache format goes to
version 3 so an inverted version-2 file is thrown away rather than read; one background sweep
rebuilds it. MAP.md invariant 71.

**Test.** `tests\iconcache.bat` is new: it fetches Notepad's icon through the real loader,
compares its alpha channel row by row against a top-down copy taken straight from the shell, then
releases, reloads and compares again, twice — the second time after a save whose entries all came
from a load, which is the case that bit. Against the committed code it reports the alternation
(`FAIL upside down`, `PASS`, `FAIL upside down`); against the fix, three passes.

### A2 — P2 — `tests\launchshot.bat` did not link ✔
`tests/launchshot.cpp`

The memory commit made the launcher call `AppScheduleTrim` and gave four of the five harnesses
that link it a stub. The search bar's own screenshot harness — the one that would have shown this
— was the one without. Stub added; the harness runs again and its shots are refreshed.

### A3 — P3 — After a release, the loader fetched what the reload had just restored, and the prefetch list was hollow ✔
`appicon.cpp`

Two small things in `NextKey`, both only reachable since the release path exists:

- A row on screen after a release queues its key while the table is empty; the loader then reloads
  the cache, which restores that very key, and *then* pops the queue and asks the shell for it
  anyway. The queue now skips keys whose slot was filled in between.
- The background sweep moved entries out of `g_prefetch` as it went. A reload rewinds
  `g_prefetchAt` to zero and sweeps the same vector, so every entry already consumed came back as
  an empty key: an empty-string lookup, a failed `SHCreateItemFromParsingName`, a `tried` slot
  under `""`. Entries are now copied.

---

## B. The memory commit, read through

### B1 — P2 — `CoFreeUnusedLibrariesEx(0, 0)` races the thermal probe ✔
`main.cpp`

The delay argument exists for one reason: a DLL serving the multithreaded apartment can report
"no objects left" a moment before a thread in that apartment finishes creating one, and a zero
delay unloads it in that moment. The thermal probe runs on an MTA thread of its own and creates
WMI objects from it (`hwmon.Attach` every thirty passes). The window is microseconds wide and the
trim runs every fifteen minutes, so this would be a once-in-months crash — the class of failure
[REVIEW-1.3.md](REVIEW-1.3.md) A1 spent a section on. The call now passes `INFINITE`, which is the
system default delay for MTA DLLs; the shell DLLs the trim is there for were loaded from
single-threaded apartments and are still unloaded at once.

### Read and left alone

- **Settings destroyed on hide.** `DestroyWindow` from inside the dialog's own `WM_COMMAND` is
  legal; `WM_DESTROY` nulls `g_dlg` and every page handle, and each entry point that could be
  reached afterwards (`SettingsRefresh`, `SettingsRefreshStatus`, `SettingsVisible`) checks it.
- **"Display off" believed only after 45 s idle.** Sleep from the Start menu arrives with input a
  second old and is not believed; the overlays keep their timers through a sleep the machine
  cannot run them in anyway, and the matching "on" is a no-op. Correct, and cheaper than the
  alternative.
- **Mouse hook only while the modifier is held.** The keyboard hook's early returns
  (`code != HC_ACTION`, our own injected keystroke) come before the modifier report, which is
  right; `ModDragModifier` takes the key from the event and the rest from `GetAsyncKeyState`,
  since the event's key is not in the async state yet from a low-level hook. `g_watchMods` keeps
  the keyboard hook in when no chord needs it, and `EnsureHookThread` accounts for it.
- **NvAPI.** Interface ids (`Initialize`, `Unload`, `EnumPhysicalGPUs`, `GPU_GetThermalSettings`),
  the `NV_GPU_THERMAL_SETTINGS_V1` layout and version word, `NVAPI_THERMAL_TARGET_ALL = 15` and
  the 64-handle array all match the SDK. `Close` calls `Unload` only if `Initialize` succeeded.
- **`IdleMs`.** `GetTickCount() - dwTime` in `DWORD` arithmetic survives the 49-day wrap.

---

## Verification

| What | How | Result |
| --- | --- | --- |
| Orientation cannot be read from a handle | scratch probe: `CreateDIBSection(biHeight=-28)` then `GetObject` | `dsBmih.biHeight = 28` |
| Shell bitmaps are bottom-up; `GetDIBits(-h)` returns them top-down with alpha intact | scratch probe on `notepad.exe`, `explorer.exe` | 0 pixels differ from the mirrored memory; 262 / 70 partial-alpha pixels preserved |
| Round trip through the cache | `tests\iconcache.bat` | committed code: FAIL, PASS, FAIL (upside down); fixed: PASS ×3 |
| Restored icons on screen | `tests\launchshot.bat` with a version-3 file present | log `icons: 25 restored from the cache`; `launcher-exe.png` right way up |
| Nothing else moved | `build.bat PW_dev.exe`, `tests\run.bat` | built; 212 passed, 0 failed |

The running instance is the old build, so the fix is in effect after the next restart. Its first
save writes a version-3 file; until then a version-2 file, right way up or not, is ignored.
