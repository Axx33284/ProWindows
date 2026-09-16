// ProWindows - the floating system-load overlay.
//
// A layered, always-on-top window drawn with per-pixel alpha, so it has real
// rounded corners and translucency rather than a grey box. Drag it anywhere;
// pin it and it stops moving and lets clicks through to whatever is underneath;
// or send it to the desktop, where it lives behind every ordinary window.
#pragma once
#include "common.h"
#include "config.h"

namespace awa {

void MonitorInit(HINSTANCE inst, Config* cfg);
void MonitorShutdown();

void MonitorSetVisible(bool visible);
bool MonitorVisible();

void MonitorSetPinned(bool pinned);
bool MonitorPinned();

// Re-reads the config: metrics, style, theme, opacity, scale, interval, and
// whether the panel floats on top or sits down on the desktop.
void MonitorApplyConfig();

// Explorer restarting destroys the desktop window the overlay is owned by when
// it is in desktop mode, taking the overlay with it. Call this from the
// TaskbarCreated handler to put it back.
void MonitorReattach();

// What a preview should draw. Deliberately not the live Config: the settings
// page previews the selection currently in its controls, before Apply.
struct MonitorPreview {
    int  theme    = 0;
    int  style    = 0;
    int  opacity  = 92;
    bool graphs   = true;
    bool topApps  = false;
    bool vertical = true;
    // MON_METRIC_COUNT per-metric colour overrides, or null for "theme only".
    const COLORREF* colors = nullptr;
};

// Draws a sample into `area` on an ordinary opaque DC. Uses the overlay's own
// painter, so the preview cannot drift from the real thing; the readings are
// invented but fixed, so the picture never flickers.
void MonitorDrawPreview(HDC dc, const RECT& area, const MonitorPreview& look);

} // namespace awa
