// ProWindows - drawing the system monitor overlay.
//
// Split out from monitor.* so that file stays about the window - creation,
// dragging, z-order, the context menu - while this one is only geometry and
// paint. It knows nothing about the config: everything it needs arrives in a
// MonPaintCtx, which is what lets the settings page render a preview of a look
// that is not the live one.
#pragma once
#include "common.h"
#include "montheme.h"
#include "sysinfo.h"

namespace Gdiplus { class Graphics; }

namespace awa {

// Samples kept per metric for the history graphs.
constexpr int kMonHistory = 60;

struct MonRow {
    const wchar_t* label      = nullptr;   // "MEMORY"
    const wchar_t* shortLabel = nullptr;   // "RAM", for the narrow styles
    COLORREF       color      = 0;
    const Metric*  metric     = nullptr;
    const float*   history    = nullptr;   // kMonHistory samples, newest last
    // What the bars, rings and the newest graph point should show, 0..100.
    // The caller supplies it rather than the painter reading metric->percent,
    // so it can be eased between samples instead of stepping once a second.
    double         percent    = 0.0;
};

struct MonPaintCtx {
    float scale      = 1.0f;
    BYTE  alpha      = 255;                // panel opacity
    bool  vertical   = true;               // stack metrics down, not across
    bool  graphs     = true;
    bool  topApps    = false;              // room for the busiest-app line
    // Draw the CPU bar divided into one segment per logical processor, where
    // the style has a bar at all. Strictly more information in the same
    // pixels: an averaged bar cannot tell one pinned thread from a machine
    // that is evenly busy, and those mean opposite things.
    bool  cores      = true;
    int   historyLen = kMonHistory;
    int   style      = MON_STYLE_ROWS;
    const MonitorSkin* skin = nullptr;
};

// Panel size for `rowCount` metrics. Zero rows still gives a small placeholder.
SIZE MonMeasure(int rowCount, const MonPaintCtx& ctx);

// Paints the whole panel, background included, into a `width` x `height` area
// at the origin. The caller clears the surface first if it needs transparency.
void MonDraw(Gdiplus::Graphics* g, int width, int height,
             const std::vector<MonRow>& rows, const MonPaintCtx& ctx);

} // namespace awa
