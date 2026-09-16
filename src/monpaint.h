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

// The panel's chrome - the soft shadow, the gradient fill, the gloss along
// the top and the border - described on its own, so the clock overlay draws
// exactly the glass the monitor draws rather than a second version of it.
// MonDraw builds one from a MonitorSkin; the clock builds one from its own.
struct PanelLook {
    COLORREF panelTop    = 0;
    COLORREF panelBottom = 0;
    COLORREF border      = 0;
    BYTE     borderAlpha = 0;
    BYTE     gloss       = 0;
    int      radius      = 0;      // corner radius, unscaled pixels
    bool     bare        = false;  // no chrome at all: text straight on the desktop
};

// The soft drop shadow is drawn inside the panel's own bitmap, so every
// overlay bitmap is bigger than its panel by this much on each side, before
// scaling. Invariant 37.
constexpr int kPanelShadow = 9;

// Paints the chrome into a `width` x `height` surface, the panel itself inset
// by the scaled shadow margin. Cached: the same look at the same size is the
// same pixels every frame, and painting the shadow inline was most of the
// cost of a frame (invariant 10). A bare look paints one count of alpha over
// the panel so the layered window still takes the mouse.
void PaintPanel(Gdiplus::Graphics* g, const PanelLook& look, int width, int height,
                float scale, BYTE alpha);

struct MonRow {
    int            id         = -1;        // MonMetric, so a style can group by device
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

// A readout being dragged to a new place in the panel. The painter lays the
// others out as if the lifted one were already at `slot`, leaving a gap there,
// and draws the lifted one itself at `pos` - so the panel is the drag feedback
// and nothing has to be drawn over it.
struct MonDrag {
    int cell = -1;       // index into what MonLayout returned, -1 = nothing lifted
    int slot = -1;       // where it would land if released now
    int pos  = 0;        // its leading edge along the stacking axis, bitmap px
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
    MonDrag drag;
};

// One thing the panel draws and the user can pick up: a rectangle in the
// bitmap, and the rows it holds. In most styles that is one row; the HUD style
// folds a device's readouts onto one line, and then a line is the thing that
// moves, with every row on it.
struct MonCell {
    RECT rect = {};
    std::vector<int> rows;      // indices into the rows given to MonLayout
};

// Where each cell sits, ignoring any drag in progress. This is what the
// overlay hit-tests a click against, and the only place that knows.
void MonLayout(const std::vector<MonRow>& rows, const MonPaintCtx& ctx,
               std::vector<MonCell>* out);

// Panel size for these rows. No rows still gives a small placeholder.
SIZE MonMeasure(const std::vector<MonRow>& rows, const MonPaintCtx& ctx);

// Paints the whole panel, background included, into a `width` x `height` area
// at the origin. The caller clears the surface first if it needs transparency.
void MonDraw(Gdiplus::Graphics* g, int width, int height,
             const std::vector<MonRow>& rows, const MonPaintCtx& ctx);

} // namespace awa
