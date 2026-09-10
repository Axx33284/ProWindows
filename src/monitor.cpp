#include "monitor.h"
#include "monpaint.h"
#include "montheme.h"
#include "sysinfo.h"
#include "app.h"
#include <objidl.h>
// GDI+ headers use bare min/max, which NOMINMAX removes. Feed them the
// std:: versions rather than re-enabling the Windows macros.
#include <algorithm>
#include <cmath>
using std::min;
using std::max;
#include <gdiplus.h>

namespace awa {

namespace {

constexpr wchar_t kClass[]    = L"ProWindows_Monitor";
constexpr UINT_PTR kTimerSample = 1;   // take a reading
constexpr UINT_PTR kTimerEase   = 2;   // slide the gauges towards it

// How long a gauge takes to travel to a new reading, and how often it is
// redrawn while it does. Long enough to read as motion, short enough that the
// panel is idle again well before the next sample. 30 fps is indistinguishable
// from 60 on a gauge this size and costs half as much.
constexpr int kEaseMs    = 300;
constexpr int kEaseFrame = 32;

// A glide is only worth starting, or drawing another frame of, when the result
// would actually differ on screen. On an idle machine the readings barely move
// between samples, and repainting the whole panel to shift a bar by a third of
// a pixel is the difference between this costing nothing and costing a percent
// of a core all day.
constexpr double kEaseWorthStarting = 0.4;   // percentage points
constexpr double kEaseWorthDrawing  = 0.2;

enum : UINT {
    IDM_PIN = 1, IDM_DESKTOP, IDM_GRAPHS, IDM_VERTICAL, IDM_TOPAPPS,
    IDM_SMOOTH, IDM_SETTINGS, IDM_HIDE, IDM_ORDER_RESET,
    IDM_STYLE_BASE  = 100,      // + style index
    IDM_THEME_BASE  = 200,      // + skin index
    // + metric index. The eight show/hide ticks.
    IDM_METRIC_BASE = 300,
    // + metric * kOrderOps + one of the four moves below. Encoded rather than
    // enumerated because "move the GPU row up" is one command per metric per
    // direction, and writing thirty-two of them out by hand is how a table and
    // a switch drift apart.
    IDM_ORDER_BASE  = 400,
    // + one of the nine snap points.
    IDM_SNAP_BASE   = 500,
};

// The moves offered for one readout, in the order they appear in its submenu.
enum : int { ORDER_UP = 0, ORDER_DOWN, ORDER_TOP, ORDER_BOTTOM, kOrderOps };

HINSTANCE g_inst = nullptr;
Config*   g_cfg  = nullptr;
HWND      g_wnd  = nullptr;
// What MonitorSetVisible was last told. The window now outlives a hide, so
// "there is a window" and "the panel should be on screen" are two different
// questions and the timers have to follow the second one.
bool      g_wantVisible = false;
SystemSampler g_sampler;
SystemLoad    g_load;

// One history ring per metric, kept across config changes.
float g_history[MON_METRIC_COUNT][kMonHistory] = {};
int   g_historyLen = 0;

// Gauges are eased from where they were drawn towards the newest reading, so
// a once-a-second sample does not look like a once-a-second jump.
double g_shownPct[MON_METRIC_COUNT]   = {};   // what the next frame will show
double g_paintedPct[MON_METRIC_COUNT] = {};   // what the last frame did show
double g_fromPct[MON_METRIC_COUNT]    = {};   // where this glide started
double g_toPct[MON_METRIC_COUNT]      = {};   // where it is heading
LARGE_INTEGER g_easeStart = {};
LARGE_INTEGER g_easeFreq  = {};
bool  g_easing = false;

bool  g_dragging = false;
POINT g_dragOrigin = {};
RECT  g_dragStart  = {};

// What the last frame put on screen, as one number. A glide repaints thirty
// times a second and a sample once a second, and on an idle machine most of
// those frames are pixel-for-pixel identical to the one before - the readings
// have not moved and neither has any of the text. Comparing a signature is a
// few hundred nanoseconds; UpdateLayeredWindow on a panel this size is not, so
// the frames that would change nothing are simply not drawn.
unsigned long long g_lastSig  = 0;
bool               g_sigValid = false;

// The layered-window surface, kept across frames rather than rebuilt per frame.
HDC     g_surfaceDc   = nullptr;
HBITMAP g_surfaceBmp  = nullptr;
HGDIOBJ g_surfaceOld  = nullptr;
void*   g_surfaceBits = nullptr;
SIZE    g_surfaceSize = { 0, 0 };
void ReleaseSurface();

// Called from every path that changes what the panel should look like without
// changing a reading: a new theme, a new size, a fresh window.
void InvalidatePanel() { g_sigValid = false; }

const MonitorSkin& Skin() {
    return MonitorSkinAt(g_cfg ? g_cfg->monitorTheme : 0);
}

float Scale() {
    float scale = (g_cfg ? g_cfg->monitorScale : 100) / 100.0f;
    if (g_wnd) {
        const UINT dpi = DpiForWindow(g_wnd);
        if (dpi > 0) scale *= (float)dpi / 96.0f;
    }
    return scale;
}

// Whether each metric is switched on, indexed by MonMetric.
bool MetricEnabled(int metric) {
    if (!g_cfg) return false;
    switch (metric) {
        case MON_CPU:     return g_cfg->monShowCpu;
        case MON_RAM:     return g_cfg->monShowRam;
        case MON_GPU:     return g_cfg->monShowGpu;
        case MON_VRAM:    return g_cfg->monShowVram;
        case MON_CPUTEMP: return g_cfg->monShowCpuTemp;
        case MON_GPUTEMP: return g_cfg->monShowGpuTemp;
        case MON_DISK:    return g_cfg->monShowDisk;
        case MON_NET:     return g_cfg->monShowNet;
        default:          return false;
    }
}

// The live reading for each metric, indexed by MonMetric. Three separate
// functions used to write this array out longhand and they had to agree; now
// they call this, and adding a metric is one line here.
const Metric* MetricReading(int metric) {
    static const Metric kNothing;
    switch (metric) {
        case MON_CPU:     return &g_load.cpu;
        case MON_RAM:     return &g_load.ram;
        case MON_GPU:     return &g_load.gpu;
        case MON_VRAM:    return &g_load.vram;
        case MON_CPUTEMP: return &g_load.cpuTemp;
        case MON_GPUTEMP: return &g_load.gpuTemp;
        case MON_DISK:    return &g_load.disk;
        case MON_NET:     return &g_load.net;
        default:          return &kNothing;
    }
}

// Which metrics are switched on, in the order the user has put them, with the
// colours the current skin gives them.
//
// Everything else here - the history rings, the eased percentages, the colour
// overrides - stays indexed by MonMetric rather than by row, so reordering the
// panel does not move a metric's history onto another metric's graph.
std::vector<MonRow> BuildRows() {
    std::vector<MonRow> rows;
    if (!g_cfg) return rows;
    rows.reserve(MON_METRIC_COUNT);

    const MonitorSkin& skin = Skin();
    for (int slot = 0; slot < MON_METRIC_COUNT; ++slot) {
        const int m = g_cfg->monOrder[slot];
        if (m < 0 || m >= MON_METRIC_COUNT) continue;   // a config we repaired
        if (!MetricEnabled(m)) continue;

        const MonitorMetricInfo& info = MonitorMetricAt(m);
        MonRow row;
        row.label      = info.label;
        row.shortLabel = info.shortLabel;
        row.color      = MonitorMetricColour(skin, m, g_cfg->monColor[m]);
        row.metric     = MetricReading(m);
        row.history    = g_history[m];
        row.percent    = g_shownPct[m];
        rows.push_back(row);
    }
    return rows;
}

void PushHistory() {
    for (int m = 0; m < MON_METRIC_COUNT; ++m) {
        const Metric* reading = MetricReading(m);
        for (int i = 0; i < kMonHistory - 1; ++i)
            g_history[m][i] = g_history[m][i + 1];
        g_history[m][kMonHistory - 1] = reading->available
            ? (float)(reading->percent / 100.0) : 0.0f;
    }
    if (g_historyLen < kMonHistory) ++g_historyLen;
}

// Starts a glide from whatever is currently on screen to the new readings.
void SnapToTargets() {
    for (int i = 0; i < MON_METRIC_COUNT; ++i) g_shownPct[i] = g_toPct[i];
    g_easing = false;
    if (g_wnd) KillTimer(g_wnd, kTimerEase);
}

void BeginEase() {
    double furthest = 0.0;
    for (int i = 0; i < MON_METRIC_COUNT; ++i) {
        const Metric* reading = MetricReading(i);
        g_fromPct[i] = g_shownPct[i];
        g_toPct[i]   = reading->available ? reading->percent : 0.0;
        furthest = (std::max)(furthest, fabs(g_toPct[i] - g_fromPct[i]));
    }

    if (!g_cfg || !g_cfg->monitorSmooth || furthest < kEaseWorthStarting) {
        SnapToTargets();
        return;
    }

    QueryPerformanceFrequency(&g_easeFreq);
    QueryPerformanceCounter(&g_easeStart);
    g_easing = true;
    if (g_wnd) SetTimer(g_wnd, kTimerEase, kEaseFrame, nullptr);
}

// Returns true while there is still movement left to draw.
bool StepEase() {
    if (!g_easing) return false;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double ms = g_easeFreq.QuadPart
        ? (double)(now.QuadPart - g_easeStart.QuadPart) * 1000.0 /
          (double)g_easeFreq.QuadPart
        : (double)kEaseMs;

    double t = ms / (double)kEaseMs;
    if (t >= 1.0) t = 1.0;

    // Ease-out cubic, the same curve the window animation uses.
    const double inv = 1.0 - t;
    const double e = 1.0 - inv * inv * inv;

    for (int i = 0; i < MON_METRIC_COUNT; ++i)
        g_shownPct[i] = g_fromPct[i] + (g_toPct[i] - g_fromPct[i]) * e;

    if (t >= 1.0) {
        g_easing = false;
        if (g_wnd) KillTimer(g_wnd, kTimerEase);
        return true;                 // the last frame always gets drawn
    }

    // Otherwise only bother when the move is big enough to see.
    for (int i = 0; i < MON_METRIC_COUNT; ++i)
        if (fabs(g_shownPct[i] - g_paintedPct[i]) >= kEaseWorthDrawing)
            return true;
    return false;
}

// The live paint context, built from the config and the window's DPI.
MonPaintCtx LiveCtx() {
    MonPaintCtx ctx;
    ctx.scale      = Scale();
    ctx.alpha      = (BYTE)(255 * (g_cfg ? g_cfg->monitorOpacity : 92) / 100);
    ctx.vertical   = !g_cfg || g_cfg->monitorVertical;
    ctx.graphs     = g_cfg && g_cfg->monitorGraphs;
    ctx.topApps    = g_cfg && g_cfg->monitorTopApps;
    ctx.cores      = !g_cfg || g_cfg->monitorCores;
    ctx.historyLen = g_historyLen;
    ctx.style      = g_cfg ? g_cfg->monitorStyle : MON_STYLE_ROWS;
    ctx.skin       = &Skin();
    return ctx;
}

void ApplySampler() {
    if (!g_cfg) return;
    SampleWants want;
    want.cpu     = g_cfg->monShowCpu;
    want.ram     = g_cfg->monShowRam;
    want.gpu     = g_cfg->monShowGpu;
    want.vram    = g_cfg->monShowVram;
    want.cpuTemp = g_cfg->monShowCpuTemp;
    want.gpuTemp = g_cfg->monShowGpuTemp;
    want.disk    = g_cfg->monShowDisk;
    want.net     = g_cfg->monShowNet;
    want.topApps = g_cfg->monitorTopApps;
    g_sampler.Configure(want);
}

// Everything that can change what a frame looks like, folded into one 64-bit
// value. Deliberately includes the strings: a reading can change from "9.7 GB"
// to "9.8 GB" without moving the bar enough to notice, and that still has to
// be drawn.
unsigned long long FrameSignature(const MonPaintCtx& ctx,
                                  const std::vector<MonRow>& rows,
                                  const SIZE& size) {
    unsigned long long h = 1469598103934665603ull;          // FNV-1a
    const auto mix = [&h](unsigned long long v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    const auto mixText = [&](const std::wstring& t) {
        for (wchar_t c : t) mix((unsigned long long)c);
        mix(0x5bf03u);
    };

    mix((unsigned long long)size.cx);
    mix((unsigned long long)size.cy);
    mix((unsigned long long)ctx.style);
    mix((unsigned long long)ctx.alpha);
    mix((unsigned long long)((ctx.vertical ? 1u : 0u) | (ctx.graphs ? 2u : 0u) |
                             (ctx.topApps ? 4u : 0u) | (ctx.cores ? 8u : 0u)));
    mix((unsigned long long)ctx.historyLen);
    mix((unsigned long long)(uintptr_t)ctx.skin);
    mix((unsigned long long)(long long)(ctx.scale * 100.0f));

    for (const MonRow& row : rows) {
        mix((unsigned long long)row.color);
        // A twentieth of a percentage point: finer than any pixel on any of the
        // gauges, coarse enough that floating-point dust does not force a frame.
        mix((unsigned long long)(long long)(row.percent * 20.0));
        if (!row.metric) continue;
        mix(row.metric->available ? 1u : 0u);
        for (double part : row.metric->parts)
            mix((unsigned long long)(long long)(part * 4.0));
        mixText(row.metric->value);
        mixText(row.metric->detail);
        mixText(row.metric->topApp);
    }

    // The curve reads the history ring, which the signature above does not
    // cover. Fold in the newest sample of each row - that is the only one a new
    // reading can have changed.
    for (const MonRow& row : rows)
        if (row.history)
            mix((unsigned long long)(long long)(row.history[kMonHistory - 1] * 4096.0f));
    return h;
}

void Redraw() {
    if (!g_wnd || !g_cfg) return;

    const MonPaintCtx ctx = LiveCtx();
    const std::vector<MonRow> rows = BuildRows();
    SIZE size = MonMeasure((int)rows.size(), ctx);  // UpdateLayeredWindow wants it writable

    const unsigned long long sig = FrameSignature(ctx, rows, size);
    if (g_sigValid && sig == g_lastSig) {
        // Nothing on screen would move. Still record what the gauges are
        // showing, or StepEase would keep measuring against a stale frame.
        for (int i = 0; i < MON_METRIC_COUNT; ++i) g_paintedPct[i] = g_shownPct[i];
        return;
    }
    g_lastSig  = sig;
    g_sigValid = true;

    for (int i = 0; i < MON_METRIC_COUNT; ++i) g_paintedPct[i] = g_shownPct[i];

    HDC screen = GetDC(nullptr);

    // The surface is kept between frames. A glide repaints thirty times a
    // second, and building a DC and a quarter-megabyte DIB section each time
    // was allocating and freeing GDI objects at that rate for no reason - the
    // panel is the same size on all but the frame where something changed.
    if (!g_surfaceDc) g_surfaceDc = CreateCompatibleDC(screen);
    if (g_surfaceDc && (!g_surfaceBmp || g_surfaceSize.cx != size.cx ||
                        g_surfaceSize.cy != size.cy)) {
        if (g_surfaceBmp) {
            SelectObject(g_surfaceDc, g_surfaceOld);
            DeleteObject(g_surfaceBmp);
            g_surfaceBmp = nullptr;
        }
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = size.cx;
        bi.bmiHeader.biHeight      = -size.cy;      // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        g_surfaceBits = nullptr;
        g_surfaceBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS,
                                        &g_surfaceBits, nullptr, 0);
        if (g_surfaceBmp) {
            g_surfaceOld  = SelectObject(g_surfaceDc, g_surfaceBmp);
            g_surfaceSize = size;
        }
    }

    HDC   dc   = g_surfaceDc;
    void* bits = g_surfaceBits;
    if (!dc || !g_surfaceBmp || !bits) { ReleaseDC(nullptr, screen); return; }

    {
        // UpdateLayeredWindow expects premultiplied alpha. Wrapping the DIB bits
        // in a PARGB bitmap makes GDI+ write them that way; drawing through the
        // HDC instead would give straight alpha and a washed-out, too-
        // transparent panel.
        Gdiplus::Bitmap surface(size.cx, size.cy, size.cx * 4,
                                PixelFormat32bppPARGB, (BYTE*)bits);
        Gdiplus::Graphics g(&surface);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));    // the corners stay see-through
        MonDraw(&g, size.cx, size.cy, rows, ctx);
    }

    RECT current;
    GetWindowRect(g_wnd, &current);
    POINT position = { current.left, current.top };
    POINT origin = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    UpdateLayeredWindow(g_wnd, screen, &position, &size, dc, &origin, 0,
                        &blend, ULW_ALPHA);

    // dc and the bitmap deliberately outlive the frame; see above.
    ReleaseDC(nullptr, screen);
}

// Frees the cached surface. Called when the overlay is hidden or destroyed, so
// a panel nobody is looking at is not holding a bitmap.
void ReleaseSurface() {
    if (g_surfaceDc) {
        if (g_surfaceBmp) SelectObject(g_surfaceDc, g_surfaceOld);
        DeleteDC(g_surfaceDc);
        g_surfaceDc = nullptr;
    }
    if (g_surfaceBmp) { DeleteObject(g_surfaceBmp); g_surfaceBmp = nullptr; }
    g_surfaceBits = nullptr;
    g_surfaceOld  = nullptr;
    g_surfaceSize = SIZE{ 0, 0 };
    g_sigValid    = false;
}

void ClampOnScreen(int* x, int* y, const SIZE& size) {
    POINT probe = { *x + size.cx / 2, *y + size.cy / 2 };
    HMONITOR mon = MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(mon, &mi)) return;

    const int maxX = (int)mi.rcWork.right - (int)size.cx;
    const int maxY = (int)mi.rcWork.bottom - (int)size.cy;
    *x = (std::max)((int)mi.rcWork.left, (std::min)(*x, maxX));
    *y = (std::max)((int)mi.rcWork.top,  (std::min)(*y, maxY));
}

void ApplyClickThrough() {
    if (!g_wnd || !g_cfg) return;
    LONG ex = GetWindowLongW(g_wnd, GWL_EXSTYLE);
    if (g_cfg->monitorPinned) ex |= WS_EX_TRANSPARENT;
    else                      ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongW(g_wnd, GWL_EXSTYLE, ex);
}

// Desktop mode. Giving the overlay the desktop window as its *owner* is what
// makes it behave like wallpaper: an owned window is always above its owner and
// a plain popup is always below the ordinary windows above it, so the panel
// sits on the desktop and every app covers it. Parenting it into the WorkerW
// behind the icons would look the same but breaks UpdateLayeredWindow, which
// only works for top-level windows - so the panel would lose its per-pixel
// alpha, which is the whole reason it looks like glass.
void ApplyZOrder() {
    if (!g_wnd || !g_cfg) return;

    if (g_cfg->monitorOnDesktop) {
        // GetShellWindow is the reliable one: FindWindow("Progman") comes back
        // null on some desktop-composition setups even though the window is
        // there and enumerates under that class.
        HWND desktop = GetShellWindow();
        if (!desktop) desktop = FindWindowW(L"Progman", nullptr);
        SetWindowPos(g_wnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowLongPtrW(g_wnd, GWLP_HWNDPARENT, (LONG_PTR)desktop);
        SetWindowPos(g_wnd, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        SetWindowLongPtrW(g_wnd, GWLP_HWNDPARENT, 0);
        SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void SaveAndRefresh() {
    MonitorApplyConfig();
    AppSaveConfig();
    AppRefreshSettings();
}

// ---------------------------------------------------------------- readouts
void SetMetricEnabled(int metric, bool on) {
    if (!g_cfg) return;
    switch (metric) {
        case MON_CPU:     g_cfg->monShowCpu     = on; break;
        case MON_RAM:     g_cfg->monShowRam     = on; break;
        case MON_GPU:     g_cfg->monShowGpu     = on; break;
        case MON_VRAM:    g_cfg->monShowVram    = on; break;
        case MON_CPUTEMP: g_cfg->monShowCpuTemp = on; break;
        case MON_GPUTEMP: g_cfg->monShowGpuTemp = on; break;
        case MON_DISK:    g_cfg->monShowDisk    = on; break;
        case MON_NET:     g_cfg->monShowNet     = on; break;
        default: break;
    }
}

// Moves one metric within the order. Works on the whole eight-entry
// permutation rather than on the visible rows, so a hidden readout keeps its
// place in the list and comes back where it was rather than at the end.
void MoveMetric(int metric, int op) {
    if (!g_cfg || metric < 0 || metric >= MON_METRIC_COUNT) return;

    int at = -1;
    for (int i = 0; i < MON_METRIC_COUNT; ++i)
        if (g_cfg->monOrder[i] == metric) { at = i; break; }
    if (at < 0) return;

    int to = at;
    switch (op) {
        case ORDER_UP:     to = at - 1; break;
        case ORDER_DOWN:   to = at + 1; break;
        case ORDER_TOP:    to = 0; break;
        case ORDER_BOTTOM: to = MON_METRIC_COUNT - 1; break;
        default: return;
    }
    if (to < 0 || to >= MON_METRIC_COUNT || to == at) return;

    // Lift and reinsert rather than swap: "move to the top" has to slide the
    // rows above it down by one, not exchange with whatever happens to be
    // sitting there.
    const int step = (to > at) ? 1 : -1;
    for (int i = at; i != to; i += step)
        g_cfg->monOrder[i] = g_cfg->monOrder[i + step];
    g_cfg->monOrder[to] = metric;
}

// ---------------------------------------------------------------- snapping
// Where "Move to" can put the panel, as a fraction of the free space along
// each axis: 0 is against the leading edge, 1 against the trailing one.
struct SnapPoint { const wchar_t* name; float fx; float fy; };
const SnapPoint kSnapPoints[] = {
    { L"Top left",      0.0f, 0.0f },
    { L"Top centre",    0.5f, 0.0f },
    { L"Top right",     1.0f, 0.0f },
    { L"Left",          0.0f, 0.5f },
    { L"Centre",        0.5f, 0.5f },
    { L"Right",         1.0f, 0.5f },
    { L"Bottom left",   0.0f, 1.0f },
    { L"Bottom centre", 0.5f, 1.0f },
    { L"Bottom right",  1.0f, 1.0f },
};
constexpr int kSnapCount = (int)(sizeof(kSnapPoints) / sizeof(kSnapPoints[0]));

// The margin a corner snap leaves, so the panel sits *near* the corner rather
// than jammed into it. Scaled with the panel, so it stays proportionate at
// 150%. The shadow already occupies part of the window (invariant 37), which
// is why this is smaller than it looks.
constexpr int kSnapMargin = 16;

void SnapPanelTo(int index) {
    if (!g_wnd || !g_cfg || index < 0 || index >= kSnapCount) return;

    RECT r{};
    if (!GetWindowRect(g_wnd, &r)) return;
    const SIZE size = { r.right - r.left, r.bottom - r.top };

    // Whichever monitor the panel is on now, not the primary: snapping should
    // move it into a corner of the screen it is already on.
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi))
        return;

    const SnapPoint& p = kSnapPoints[index];
    const int margin = (int)(kSnapMargin * Scale());
    const int left   = (int)mi.rcWork.left + margin;
    const int top    = (int)mi.rcWork.top + margin;
    const int spanX  = (int)(mi.rcWork.right - mi.rcWork.left) - size.cx - margin * 2;
    const int spanY  = (int)(mi.rcWork.bottom - mi.rcWork.top) - size.cy - margin * 2;

    g_cfg->monitorX = left + (int)((std::max)(0, spanX) * p.fx);
    g_cfg->monitorY = top  + (int)((std::max)(0, spanY) * p.fy);
    SaveAndRefresh();
}

void ShowContextMenu() {
    if (!g_cfg) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING | (g_cfg->monitorPinned ? MF_CHECKED : 0),
                IDM_PIN, L"Pin in place (click-through)");
    AppendMenuW(menu, MF_STRING | (g_cfg->monitorOnDesktop ? MF_CHECKED : 0),
                IDM_DESKTOP, L"Sit on the desktop (behind windows)");

    // Where the panel itself sits. Dragging is exact and fiddly; nine snap
    // points get it into a corner in one click, which is where it wants to be
    // nearly every time. Disabled while pinned, because a pinned panel is one
    // the user has deliberately fixed - the pin has to be lifted first, which
    // is one item above.
    HMENU snap = CreatePopupMenu();
    if (snap) {
        for (int i = 0; i < kSnapCount; ++i)
            AppendMenuW(snap, MF_STRING, IDM_SNAP_BASE + i, kSnapPoints[i].name);
        AppendMenuW(menu, MF_POPUP | (g_cfg->monitorPinned ? MF_GRAYED : 0),
                    (UINT_PTR)snap, L"Move to");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // The readouts, in the order they are actually drawn - so this list reads
    // as the panel does, and "the third one down" is the third one here. Each
    // opens onto its own tick and the four moves.
    for (int slot = 0; slot < MON_METRIC_COUNT; ++slot) {
        const int m = g_cfg->monOrder[slot];
        if (m < 0 || m >= MON_METRIC_COUNT) continue;
        const bool on = MetricEnabled(m);

        HMENU one = CreatePopupMenu();
        if (!one) continue;
        AppendMenuW(one, MF_STRING | (on ? MF_CHECKED : 0),
                    IDM_METRIC_BASE + m, L"Show this readout");
        AppendMenuW(one, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(one, MF_STRING | (slot == 0 ? MF_GRAYED : 0),
                    IDM_ORDER_BASE + m * kOrderOps + ORDER_UP,     L"Move up");
        AppendMenuW(one, MF_STRING | (slot == MON_METRIC_COUNT - 1 ? MF_GRAYED : 0),
                    IDM_ORDER_BASE + m * kOrderOps + ORDER_DOWN,   L"Move down");
        AppendMenuW(one, MF_STRING | (slot == 0 ? MF_GRAYED : 0),
                    IDM_ORDER_BASE + m * kOrderOps + ORDER_TOP,    L"Move to the top");
        AppendMenuW(one, MF_STRING | (slot == MON_METRIC_COUNT - 1 ? MF_GRAYED : 0),
                    IDM_ORDER_BASE + m * kOrderOps + ORDER_BOTTOM, L"Move to the bottom");

        // The tick on the parent says at a glance which readouts are on
        // without having to open all eight submenus.
        const std::wstring label = std::to_wstring(slot + 1) + L".  " +
                                   MonitorMetricAt(m).menu;
        AppendMenuW(menu, MF_POPUP | (on ? MF_CHECKED : 0), (UINT_PTR)one,
                    label.c_str());
    }
    AppendMenuW(menu, MF_STRING, IDM_ORDER_RESET, L"Reset the order");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU styles = CreatePopupMenu();
    if (styles) {
        for (int i = 0; i < MonitorStyleCount(); ++i)
            AppendMenuW(styles, MF_STRING, IDM_STYLE_BASE + i, MonitorStyleAt(i).name);
        CheckMenuRadioItem(styles, IDM_STYLE_BASE,
                           IDM_STYLE_BASE + MonitorStyleCount() - 1,
                           IDM_STYLE_BASE + g_cfg->monitorStyle, MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)styles, L"Style");
    }

    HMENU themes = CreatePopupMenu();
    if (themes) {
        for (int i = 0; i < MonitorSkinCount(); ++i)
            AppendMenuW(themes, MF_STRING, IDM_THEME_BASE + i, MonitorSkinAt(i).name);
        // Radio marks read better than ticks for a one-of-many list.
        CheckMenuRadioItem(themes, IDM_THEME_BASE,
                           IDM_THEME_BASE + MonitorSkinCount() - 1,
                           IDM_THEME_BASE + g_cfg->monitorTheme, MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)themes, L"Theme");
    }

    AppendMenuW(menu, MF_STRING | (g_cfg->monitorGraphs ? MF_CHECKED : 0),
                IDM_GRAPHS, L"Show graphs");
    AppendMenuW(menu, MF_STRING | (g_cfg->monitorTopApps ? MF_CHECKED : 0),
                IDM_TOPAPPS, L"Show the busiest app");
    AppendMenuW(menu, MF_STRING | (g_cfg->monitorVertical ? MF_CHECKED : 0),
                IDM_VERTICAL, L"Stack vertically");
    AppendMenuW(menu, MF_STRING | (g_cfg->monitorSmooth ? MF_CHECKED : 0),
                IDM_SMOOTH, L"Glide between readings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Monitor settings...");
    AppendMenuW(menu, MF_STRING, IDM_HIDE, L"Hide");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    const int cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                        pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(menu);
    // The overlay is WS_EX_NOACTIVATE, so without this the menu can stay up
    // after a click elsewhere.
    PostMessageW(g_wnd, WM_NULL, 0, 0);

    if (cmd >= (int)IDM_THEME_BASE &&
        cmd < (int)IDM_THEME_BASE + MonitorSkinCount()) {
        g_cfg->monitorTheme = cmd - (int)IDM_THEME_BASE;
        SaveAndRefresh();
        return;
    }
    if (cmd >= (int)IDM_STYLE_BASE &&
        cmd < (int)IDM_STYLE_BASE + MonitorStyleCount()) {
        g_cfg->monitorStyle = cmd - (int)IDM_STYLE_BASE;
        SaveAndRefresh();
        return;
    }
    if (cmd >= (int)IDM_SNAP_BASE && cmd < (int)IDM_SNAP_BASE + kSnapCount) {
        SnapPanelTo(cmd - (int)IDM_SNAP_BASE);
        return;
    }
    if (cmd >= (int)IDM_ORDER_BASE &&
        cmd < (int)IDM_ORDER_BASE + MON_METRIC_COUNT * kOrderOps) {
        const int rel = cmd - (int)IDM_ORDER_BASE;
        MoveMetric(rel / kOrderOps, rel % kOrderOps);
        SaveAndRefresh();
        return;
    }
    if (cmd >= (int)IDM_METRIC_BASE &&
        cmd < (int)IDM_METRIC_BASE + MON_METRIC_COUNT) {
        SetMetricEnabled(cmd - (int)IDM_METRIC_BASE,
                         !MetricEnabled(cmd - (int)IDM_METRIC_BASE));
        SaveAndRefresh();
        return;
    }

    switch (cmd) {
        case IDM_PIN:      MonitorSetPinned(!g_cfg->monitorPinned); return;
        case IDM_DESKTOP:  g_cfg->monitorOnDesktop = !g_cfg->monitorOnDesktop; break;
        case IDM_ORDER_RESET:
            for (int i = 0; i < MON_METRIC_COUNT; ++i) g_cfg->monOrder[i] = i;
            break;
        case IDM_GRAPHS:   g_cfg->monitorGraphs   = !g_cfg->monitorGraphs;   break;
        case IDM_TOPAPPS:  g_cfg->monitorTopApps  = !g_cfg->monitorTopApps;  break;
        case IDM_VERTICAL: g_cfg->monitorVertical = !g_cfg->monitorVertical; break;
        case IDM_SMOOTH:   g_cfg->monitorSmooth   = !g_cfg->monitorSmooth;   break;
        case IDM_SETTINGS: AppOpenMonitorSettings(); return;
        case IDM_HIDE:
            g_cfg->monitorEnabled = false;
            MonitorSetVisible(false);
            AppSaveConfig();
            AppRefreshSettings();
            return;
        default: return;
    }

    SaveAndRefresh();
}

LRESULT CALLBACK MonitorProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kTimerSample) {
                // Explorer builds a *new* desktop window when the wallpaper
                // changes - a slideshow, Spotlight, a theme - and the panel,
                // which is owned by the old one and sitting at the bottom of
                // the z-order, ends up behind it. Nothing broadcasts
                // TaskbarCreated for that, so the overlay simply vanished and
                // stayed vanished. One handle comparison a second is enough to
                // notice, and re-owning it puts it back.
                if (g_cfg && g_cfg->monitorOnDesktop) {
                    HWND shell = GetShellWindow();
                    if (shell && (HWND)GetWindowLongPtrW(wnd, GWLP_HWNDPARENT) != shell)
                        ApplyZOrder();
                }
                g_sampler.Sample(&g_load);
                PushHistory();
                BeginEase();
                Redraw();
            } else if (wp == kTimerEase) {
                if (StepEase()) Redraw();
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (g_cfg && !g_cfg->monitorPinned) {
                g_dragging = true;
                GetCursorPos(&g_dragOrigin);
                GetWindowRect(wnd, &g_dragStart);
                SetCapture(wnd);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (g_dragging) {
                POINT now;
                GetCursorPos(&now);
                RECT r;
                GetWindowRect(wnd, &r);
                const SIZE size = { r.right - r.left, r.bottom - r.top };

                int x = g_dragStart.left + (now.x - g_dragOrigin.x);
                int y = g_dragStart.top + (now.y - g_dragOrigin.y);
                ClampOnScreen(&x, &y, size);
                SetWindowPos(wnd, nullptr, x, y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;

        // Capture can be taken away without a button-up ever arriving: a
        // system dialog, the task switcher, or anything else that grabs the
        // mouse. Without this the panel stayed in "dragging" and then followed
        // the pointer around the screen with no button held down.
        case WM_CAPTURECHANGED:
            g_dragging = false;
            return 0;

        case WM_LBUTTONUP:
            if (g_dragging) {
                g_dragging = false;
                ReleaseCapture();
                RECT r;
                GetWindowRect(wnd, &r);
                if (g_cfg) {
                    g_cfg->monitorX = r.left;
                    g_cfg->monitorY = r.top;
                    AppSaveConfig();      // survive a crash in the new spot
                }
            }
            return 0;

        case WM_RBUTTONUP:
            ShowContextMenu();
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            MonitorApplyConfig();
            return 0;

        // Changing the wallpaper is the common way the desktop window gets
        // replaced under a panel that is sitting on it; this is the moment it
        // happens, rather than up to a second later.
        case WM_SETTINGCHANGE:
            if (g_cfg && g_cfg->monitorOnDesktop) ApplyZOrder();
            return 0;

        case WM_DESTROY:
            // Reached without going through MonitorSetVisible when the desktop
            // window this panel is owned by is destroyed - Explorer restarting.
            // MonitorReattach builds it again; the cached surface must not
            // outlive the window it was drawn for.
            g_wnd = nullptr;
            ReleaseSurface();
            return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace

// ---------------------------------------------------------------- public
void MonitorInit(HINSTANCE inst, Config* cfg) {
    g_inst = inst;
    g_cfg  = cfg;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MonitorProc;
    wc.hInstance     = inst;
    wc.lpszClassName = kClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_SIZEALL);
    RegisterClassExW(&wc);

    if (cfg->monitorEnabled) MonitorSetVisible(true);
}

void MonitorShutdown() {
    g_wantVisible = false;
    if (g_wnd) {
        KillTimer(g_wnd, kTimerSample);
        KillTimer(g_wnd, kTimerEase);
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
    }
    ReleaseSurface();
    g_sampler.Close();
}

bool MonitorVisible() { return g_wnd != nullptr && IsWindowVisible(g_wnd); }

void MonitorSetVisible(bool visible) {
    if (!g_cfg) return;
    g_wantVisible = visible;

    if (!visible) {
        // Hidden, not destroyed. This is reached every time a fullscreen
        // application starts or stops and every time the screen blanks, which
        // on an ordinary day is several times an hour - and tearing the window
        // down each time threw away every graph, reset the sampler, and left
        // the panel gone for good if anything went wrong on the way back up.
        // Hiding costs nothing and cannot fail.
        if (g_wnd) {
            KillTimer(g_wnd, kTimerSample);
            KillTimer(g_wnd, kTimerEase);
            g_easing = false;
            ShowWindow(g_wnd, SW_HIDE);
        }
        ReleaseSurface();       // a panel nobody is looking at holds no bitmap
        g_sampler.Close();
        return;
    }

    if (g_wnd) {
        if (IsWindowVisible(g_wnd)) return;      // already up
        ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
        MonitorApplyConfig();                    // re-arms the sample timer
        // Something on screen straight away rather than a blank panel for an
        // interval: the surface was released on the way down.
        g_sampler.Sample(&g_load);
        PushHistory();
        BeginEase();
        Redraw();
        return;
    }

    g_wnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClass, L"System monitor", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, g_inst, nullptr);
    if (!g_wnd) return;

    g_historyLen = 0;
    memset(g_history, 0, sizeof(g_history));
    memset(g_shownPct, 0, sizeof(g_shownPct));
    memset(g_paintedPct, 0, sizeof(g_paintedPct));
    g_easing = false;

    MonitorApplyConfig();
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);

    // Fill the first frame immediately rather than after a whole interval.
    // No glide for this one: there is nothing to glide from.
    g_sampler.Sample(&g_load);
    PushHistory();
    for (int i = 0; i < MON_METRIC_COUNT; ++i) g_shownPct[i] = 0.0;
    BeginEase();
    Redraw();
}

void MonitorSetPinned(bool pinned) {
    if (!g_cfg) return;
    g_cfg->monitorPinned = pinned;
    ApplyClickThrough();
    AppSaveConfig();
    AppRefreshSettings();
}

bool MonitorPinned() { return g_cfg && g_cfg->monitorPinned; }

void MonitorReattach() {
    if (!g_cfg || !g_cfg->monitorEnabled) return;
    // Explorer restarting destroys the desktop window, and any overlay owned by
    // it goes too - so there may genuinely be nothing left to reattach. Only
    // rebuild it if the panel is supposed to be on screen: a hidden one is
    // hidden for a reason (a game, a blank screen) and must stay that way.
    if (!g_wnd) { if (g_wantVisible) MonitorSetVisible(true); return; }
    if (g_cfg->monitorOnDesktop) ApplyZOrder();
}

void MonitorDrawPreview(HDC dc, const RECT& area, const MonitorPreview& look) {
    const int w = area.right - area.left;
    const int h = area.bottom - area.top;
    if (w <= 16 || h <= 16) return;

    Metric cpu;
    cpu.available = true; cpu.percent = 38.0; cpu.value = L"38%";
    cpu.detail = L"8 threads"; cpu.topApp = L"chrome";
    // Enough of a spread that the segmented bar is recognisable as one in the
    // preview, rather than looking like a plain bar that happens to be striped.
    const double cores[8] = { 84.0, 21.0, 12.0, 47.0, 9.0, 33.0, 15.0, 58.0 };
    cpu.parts.assign(cores, cores + 8);
    Metric ram;
    ram.available = true; ram.percent = 61.0; ram.value = L"61%";
    ram.detail = L"9.8 / 16 GB"; ram.topApp = L"Code";
    Metric gpu;
    gpu.available = true; gpu.percent = 24.0; gpu.value = L"24%";
    gpu.detail = L"3D engine"; gpu.topApp = L"dwm";

    // Deterministic waves, so the preview never flickers between repaints.
    float history[3][kMonHistory];
    for (int i = 0; i < kMonHistory; ++i) {
        const float t = (float)i / (float)(kMonHistory - 1);
        history[0][i] = 0.34f + 0.20f * sinf(t * 7.0f) + 0.08f * sinf(t * 19.0f);
        history[1][i] = 0.56f + 0.09f * sinf(t * 4.0f + 1.2f);
        history[2][i] = 0.24f + 0.16f * sinf(t * 11.0f + 0.6f);
    }

    const MonitorSkin& skin = MonitorSkinAt(look.theme);
    std::vector<MonRow> rows(3);
    const Metric* metrics[3] = { &cpu, &ram, &gpu };
    const wchar_t* labels[3]  = { L"CPU", L"MEMORY", L"GPU" };
    const wchar_t* shorts[3]  = { L"CPU", L"RAM", L"GPU" };
    for (int i = 0; i < 3; ++i) {
        rows[(size_t)i].label      = labels[i];
        rows[(size_t)i].shortLabel = shorts[i];
        rows[(size_t)i].color      = look.colors
            ? MonitorMetricColour(skin, i, look.colors[i]) : skin.metric[i];
        rows[(size_t)i].metric     = metrics[i];
        rows[(size_t)i].history    = history[i];
        rows[(size_t)i].percent    = metrics[i]->percent;
    }

    MonPaintCtx ctx;
    ctx.alpha      = (BYTE)(255 * (std::max)(20, (std::min)(100, look.opacity)) / 100);
    ctx.vertical   = look.vertical;
    ctx.graphs     = look.graphs;
    ctx.topApps    = look.topApps;
    ctx.historyLen = kMonHistory;
    ctx.style      = look.style;
    ctx.skin       = &skin;

    // Largest scale that fits the space we were given, measured at 1:1 first.
    ctx.scale = 1.0f;
    const SIZE unit = MonMeasure(3, ctx);
    ctx.scale = (std::min)((float)w / (float)unit.cx, (float)h / (float)unit.cy);
    ctx.scale = (std::max)(0.4f, (std::min)(1.6f, ctx.scale));
    const SIZE size = MonMeasure(3, ctx);

    Gdiplus::Graphics g(dc);
    g.SetClip(Gdiplus::Rect(area.left, area.top, w, h));
    g.TranslateTransform((float)(area.left + (w - size.cx) / 2),
                         (float)(area.top + (h - size.cy) / 2));
    MonDraw(&g, size.cx, size.cy, rows, ctx);
}

void MonitorApplyConfig() {
    if (!g_wnd || !g_cfg) return;

    // The sampler follows the panel rather than the config. The window now
    // survives a hide, so a settings reload while a game is running would
    // otherwise reopen the performance counters and restart the temperature
    // probe behind it - which is the whole thing game mode exists to stop.
    if (g_wantVisible) ApplySampler();
    InvalidatePanel();      // theme, style, scale or opacity may all have moved

    const MonPaintCtx ctx = LiveCtx();
    const std::vector<MonRow> rows = BuildRows();
    const SIZE size = MonMeasure((int)rows.size(), ctx);

    int x = g_cfg->monitorX;
    int y = g_cfg->monitorY;
    if (x == INT_MIN || y == INT_MIN) {
        // First run: top-right of the primary monitor's work area.
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            x = mi.rcWork.right - size.cx - 24;
            y = mi.rcWork.top + 24;
        } else {
            x = 40; y = 40;
        }
    }
    ClampOnScreen(&x, &y, size);

    SetWindowPos(g_wnd, nullptr, x, y, size.cx, size.cy,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    ApplyZOrder();
    ApplyClickThrough();

    // Only while the panel is actually on screen. The window now survives a
    // hide, so without this a reload while a game is running would quietly put
    // the sampler back to work behind it - which is the whole thing game mode
    // exists to stop.
    KillTimer(g_wnd, kTimerSample);
    if (g_wantVisible) {
        SetTimer(g_wnd, kTimerSample, (UINT)g_cfg->monitorInterval, nullptr);
        Redraw();
    }
}

} // namespace awa
