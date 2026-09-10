// ProWindows - hold the modifier and drag anywhere on a window.
//
// The one gesture Hyprland has that Windows has no equivalent for. Windows
// gives a window exactly one drag handle - the title bar - and a tiled window
// on a laptop screen has a title bar about eight pixels tall to aim at, if it
// has one at all. Holding $mod turns the whole window into that handle: left
// to move it, right to resize it from the nearest corner.
//
// It does not run a drag loop of its own. It posts the window the same
// WM_NCLBUTTONDOWN Windows sends when a title bar is really clicked, so the
// move is the system's own modal loop, and the tiler sees the same
// EVENT_SYSTEM_MOVESIZESTART / END pair it already handles - drop indicator,
// drop side, cross-monitor and all. See MAP.md invariant 48.
#pragma once
#include "common.h"
#include "config.h"

namespace awa {

// `target` is the message window WM_AWA_MODDRAG is posted to.
void ModDragInit(HWND target);

// Installs or removes the mouse hook to match `mod_drag` and the current
// modifier. Called on every config reload, like the keyboard bindings.
void ModDragApplyConfig(const Config* cfg);

// Tears the hook thread down. Once, at exit.
void ModDragShutdown();

// Runs on the UI thread, from WM_AWA_MODDRAG: `wp` is 1 for a move and 2 for a
// resize. Everything that has to look at a window happens here, never in the
// hook - see MAP.md invariant 3, which a mouse hook has more reason to obey
// than a keyboard one because it is called for every pixel of pointer travel.
void ModDragBegin(WPARAM wp);

bool ModDragHookInstalled();

// The windows the gesture may pick up, published for the hook thread.
//
// The hook has to decide whether to swallow the click *before* anyone can look
// at the window, because a low-level hook's answer is the answer. Without this
// it would either eat every mod+click on the machine - including on the
// desktop and on the apps the user has excluded, which is the one place the
// exclusion was supposed to protect - or eat none and let the click through to
// an application that treats mod+click as "download this link".
//
// So the window manager publishes what it is arranging and the hook checks
// membership. Called whenever that set changes, which is twice in wm.cpp.
void ModDragPublishWindows(const std::vector<HWND>& windows);

} // namespace awa
