// ProWindows - the desktop clock.
//
// The system monitor's sibling: the same layered, per-pixel-alpha window with
// the same chrome, dragged, pinned and sent to the desktop the same way, with
// its own styles and themes. It draws the time and nothing else, so it costs
// one repaint a minute - one a second with the seconds on - and nothing at all
// while it is hidden.
#pragma once
#include "common.h"
#include "config.h"

namespace awa {

void ClockInit(HINSTANCE inst, Config* cfg);
void ClockShutdown();

void ClockSetVisible(bool visible);
bool ClockVisible();

void ClockSetPinned(bool pinned);
bool ClockPinned();

// Re-reads the config: style, theme, opacity, scale, what to show, and
// whether the clock floats on top or sits down on the desktop.
void ClockApplyConfig();

// Explorer restarting destroys the desktop window the clock is owned by when
// it is in desktop mode. Called from the TaskbarCreated handler, with the
// monitor's equivalent.
void ClockReattach();

// What a preview should draw. Deliberately not the live Config: the settings
// page previews the selection currently in its controls, before Apply.
struct ClockPreview {
    int  theme   = 0;
    int  style   = 0;
    int  opacity = 92;
    bool hours24 = false;
    bool seconds = false;
    bool date    = true;
    bool weekday = true;
};

// Draws a sample into `area` on an ordinary opaque DC, through the clock's
// own painter, so the preview cannot drift from the real thing. The time
// shown is fixed, so the picture never flickers.
void ClockDrawPreview(HDC dc, const RECT& area, const ClockPreview& look);

} // namespace awa
