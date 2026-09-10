// ProWindows - keyboard binding registration.
//
// Two routes exist. RegisterHotKey is the good one: cheap, scoped, and the OS
// does the matching. But the Windows shell has already claimed almost every
// Win+<letter> chord, and it will not give them up. For those, and only for
// those, we fall back to a low-level keyboard hook that intercepts the chord
// before the shell sees it.
#pragma once
#include "common.h"
#include "config.h"

namespace awa {

// Registers every binding in cfg->binds, filling in each one's `route`.
void HotkeysRegister(HWND target, Config* cfg);
void HotkeysUnregister();

// Unregisters, then tears the hook's thread down. Once, at exit - unlike
// HotkeysUnregister, which runs on every settings reload and leaves the thread
// parked so a reload does not cost a thread create and join.
void HotkeysShutdown();

// Look-ups for the two dispatch paths.
//
// Both resolve to a Keybind inside cfg->binds at the moment they are called,
// rather than handing out a pointer that was taken earlier. The settings window
// replaces that whole vector on Apply (`cfg.binds = g_editBinds`), which moves
// every element; anything holding a Keybind* across that is pointing at freed
// memory, and the only reason it has never crashed is that nothing happens to
// pump messages between the assignment and the re-registration that follows it.
const Keybind* HotkeyForId(int hotkeyId);        // WM_HOTKEY

// `packed` is the wParam of WM_AWA_HOOKKEY: the index the hook matched in the
// low half and the generation of the chord list it matched against in the high
// half. The generation is what stops a keystroke that was posted just before a
// settings reload from running whatever action has since taken its slot.
const Keybind* HotkeyForHookIndex(WPARAM packed);

int  HotkeysBlockedCount();     // chords nothing could claim
int  HotkeysHookedCount();      // chords served by the keyboard hook
bool HotkeysHookInstalled();

} // namespace awa
