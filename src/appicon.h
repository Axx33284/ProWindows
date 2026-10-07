// ProWindows - the icons the search bar draws beside each result.
//
// Getting an icon out of the shell is not quick: a Start-menu shortcut has to
// be resolved, a packaged app has to be looked up in its manifest, and either
// can touch the disk. On a mechanical disk a cold fetch is tens of
// milliseconds, and eight rows of them is most of a second - long enough that
// the icons look like they are never coming. The search bar redraws on every
// keystroke and must not wait for any of that, so nothing here ever blocks the
// caller.
//
// Three things between them make it feel instant:
//
//   - `AppIconFor` answers from memory or returns null and queues the work.
//     Until the icon lands the row draws the lettered tile it always drew,
//     which is why a cold cache looks like the old behaviour rather than a gap.
//   - What was fetched last time is written to `icons.cache` and read back in
//     one sequential pass, so the second launch never asks the shell at all.
//   - Everything not in that file is fetched anyway, in the background, at
//     lowered I/O priority, while nobody is waiting - so the *first* launch is
//     the only one that ever sees a lettered tile.
#pragma once
#include "common.h"

namespace awa {

// `notify` is posted `message` (wParam and lParam both zero) whenever an icon
// finishes loading. Safe to call more than once; a second call only updates
// where the notifications go.
void AppIconInit(HWND notify, UINT message);

// Stops the loader, writes the cache out and frees every bitmap. After this
// `AppIconFor` answers null for everything.
void AppIconShutdown();

// A 32-bit premultiplied bitmap for whatever `target` names - a file, a
// shortcut, a folder, or a `shell:AppsFolder\...` moniker - at `pixels` square.
//
// Null means "not now": either it is still being fetched, or the shell has no
// icon for this target and never will. Either way the caller should draw its
// own placeholder rather than wait.
HBITMAP AppIconFor(const std::wstring& target, int pixels);

// Lets every bitmap go. The search bar calls this once it has been hidden
// for a few minutes: the icons are in icons.cache, and the next time a row
// needs one the loader reads the cache back - one sequential read, a lettered
// tile for the frame or two before it lands - rather than the bitmaps sitting
// in memory all day for a window nobody is looking at.
void AppIconRelease();

// How many icons the table holds (for the settings window's meter).
int AppIconCount();

// Fetch these in the background, at `pixels` square, so that by the time
// anybody searches for one it is already there. Requests made through
// `AppIconFor` always overtake these, however long the list is.
//
// Meant to be handed the whole application catalogue once it has been built.
// Anything already cached costs one hash lookup and is dropped.
void AppIconPrefetch(const std::vector<std::wstring>& targets, int pixels);

// Blits a bitmap from `AppIconFor` into `box`, centred and aspect-preserved,
// blending against whatever is already there.
void AppIconDraw(HDC dc, HBITMAP icon, const RECT& box);

} // namespace awa
