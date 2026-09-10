// ProWindows - offline renderer for the system-monitor overlay.
//
// Draws the real panel, through the real painter, into PNG files - one per
// style and one per skin - on a checkerboard so the translucency and the
// shadow are visible. Nothing here is part of the product build; it exists so
// a change to monpaint.cpp can be looked at without putting the tiler on a
// live desktop and rearranging every window on it.
//
//   cl /nologo /std:c++17 /utf-8 /EHsc /DUNICODE /D_UNICODE /MT ^
//      tests\monshot.cpp src\monpaint.cpp src\montheme.cpp ^
//      /Fe:tests\build\monshot.exe /link gdiplus.lib
//
//   monshot.exe <output folder>          write the PNGs
//   monshot.exe --bench                   time the painter instead
#include "../src/monpaint.h"
#include "../src/montheme.h"
#include <objidl.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <cstdio>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")

using namespace awa;

namespace {



CLSID PngEncoder() {
    UINT count = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    CLSID id = {};
    if (!bytes) return id;
    auto* info = (Gdiplus::ImageCodecInfo*)malloc(bytes);
    if (!info) return id;
    Gdiplus::GetImageEncoders(count, bytes, info);
    for (UINT i = 0; i < count; ++i)
        if (wcscmp(info[i].MimeType, L"image/png") == 0) { id = info[i].Clsid; break; }
    free(info);
    return id;
}

float history[MON_METRIC_COUNT][kMonHistory];

void BuildHistory() {
    for (int m = 0; m < MON_METRIC_COUNT; ++m)
        for (int i = 0; i < kMonHistory; ++i) {
            const float t = (float)i / (float)(kMonHistory - 1);
            history[m][i] = 0.30f + 0.22f * sinf(t * (5.0f + m * 1.7f) + m) +
                            0.10f * sinf(t * (17.0f + m) + m * 0.5f);
            if (history[m][i] < 0.02f) history[m][i] = 0.02f;
            if (history[m][i] > 0.98f) history[m][i] = 0.98f;
        }
}

struct Fake { Metric metric; };
Fake fakes[MON_METRIC_COUNT];

void BuildMetrics() {
    struct Spec { double pct; const wchar_t* v; const wchar_t* sv;
                  const wchar_t* d; const wchar_t* app; };
    const Spec specs[MON_METRIC_COUNT] = {
        { 38.0, L"38%",     L"38%", L"16 threads",   L"chrome"   },
        { 61.0, L"9.8 GB",  L"61%", L"of 16 GB",     L"Code"     },
        { 24.0, L"24%",     L"24%", L"3D engine",    L"dwm"      },
        { 47.0, L"3.8 GB",  L"47%", L"of 8 GB",      L"chrome"   },
        { 74.0, L"82°C", L"82°", L"package", L""       },
        { 55.0, L"69°C", L"69°", L"nvml",    L""       },
        { 94.0, L"94%",     L"94%", L"",             L"explorer" },
        { 12.0, L"↓ 1.4 MB/s", L"1.4 MB/s", L"↑ 210 KB/s", L"" },
    };
    // Sixteen threads with one of them pinned - the case a single averaged bar
    // cannot show and the segmented one exists for.
    const double cores[16] = { 97, 12, 8, 41, 6, 9, 55, 7,
                               11, 5, 63, 4, 8, 22, 6, 9 };
    fakes[0].metric.parts.assign(cores, cores + 16);

    for (int i = 0; i < MON_METRIC_COUNT; ++i) {
        Metric& m = fakes[i].metric;
        m.available  = true;
        m.percent    = specs[i].pct;
        m.value      = specs[i].v;
        m.shortValue = specs[i].sv;
        m.detail     = specs[i].d;
        m.topApp     = specs[i].app;
    }
}

std::vector<MonRow> Rows(const MonitorSkin& skin, int count) {
    const wchar_t* labels[MON_METRIC_COUNT] = {
        L"CPU", L"MEMORY", L"GPU", L"GPU MEMORY",
        L"CPU TEMP", L"GPU TEMP", L"DISK", L"NETWORK" };
    const wchar_t* shorts[MON_METRIC_COUNT] = {
        L"CPU", L"RAM", L"GPU", L"VRAM", L"CPU°", L"GPU°", L"DISK", L"NET" };

    std::vector<MonRow> rows;
    for (int i = 0; i < count && i < MON_METRIC_COUNT; ++i) {
        MonRow r;
        r.label      = labels[i];
        r.shortLabel = shorts[i];
        r.color      = MonitorMetricColour(skin, i, kMonColourFromTheme);
        r.metric     = &fakes[i].metric;
        r.history    = history[i];
        r.percent    = fakes[i].metric.percent;
        rows.push_back(r);
    }
    return rows;
}

// A desktop to sit on, so alpha and the shadow are visible rather than assumed.
void Wallpaper(Gdiplus::Graphics* g, int w, int h) {
    Gdiplus::LinearGradientBrush sky(Gdiplus::PointF(0, 0), Gdiplus::PointF((float)w, (float)h),
                                     Gdiplus::Color(255, 32, 44, 68),
                                     Gdiplus::Color(255, 96, 72, 104));
    g->FillRectangle(&sky, 0, 0, w, h);
    Gdiplus::SolidBrush light(Gdiplus::Color(26, 255, 255, 255));
    for (int y = 0; y < h; y += 32)
        for (int x = ((y / 32) % 2) * 32; x < w; x += 64)
            g->FillRectangle(&light, x, y, 32, 32);
}

void Shot(const std::wstring& path, const MonPaintCtx& base,
          const std::vector<MonRow>& rows, const CLSID& png) {
    MonPaintCtx ctx = base;
    const SIZE size = MonMeasure((int)rows.size(), ctx);

    const int pad = 26;
    Gdiplus::Bitmap canvas(size.cx + pad * 2, size.cy + pad * 2, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&canvas);
    Wallpaper(&g, size.cx + pad * 2, size.cy + pad * 2);

    // The panel is drawn into its own premultiplied surface exactly as the
    // overlay does it, then composited - so what comes out is what the screen
    // would show, not an approximation of it.
    Gdiplus::Bitmap panel(size.cx, size.cy, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics pg(&panel);
        pg.Clear(Gdiplus::Color(0, 0, 0, 0));
        MonDraw(&pg, size.cx, size.cy, rows, ctx);
    }
    g.DrawImage(&panel, pad, pad);

    canvas.Save(path.c_str(), &png, nullptr);
    wprintf(L"  %s  (%ldx%ld)\n", path.c_str(), size.cx, size.cy);
}

} // namespace

// How long one frame of the real painter takes, into the same premultiplied
// surface the overlay uses. This is the number that matters: the panel repaints
// thirty times a second for the length of every glide, so a millisecond here is
// three percent of a core while a reading moves.
void Bench() {
    const int kFrames = 400;
    for (int style = 0; style < MonitorStyleCount(); ++style) {
        const MonitorSkin& skin = MonitorSkinAt(0);
        MonPaintCtx ctx;
        ctx.scale      = 1.0f;
        ctx.alpha      = (BYTE)(255 * 92 / 100);
        ctx.vertical   = (style != MON_STYLE_BARS && style != MON_STYLE_TICKER);
        ctx.graphs     = true;
        ctx.topApps    = true;
        ctx.historyLen = kMonHistory;
        ctx.style      = style;
        ctx.skin       = &skin;

        std::vector<MonRow> rows = Rows(skin, MON_METRIC_COUNT);
        const SIZE size = MonMeasure((int)rows.size(), ctx);
        Gdiplus::Bitmap panel(size.cx, size.cy, PixelFormat32bppPARGB);

        LARGE_INTEGER freq, a, b;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&a);
        for (int i = 0; i < kFrames; ++i) {
            // Move the readings, or the caches would be measured under
            // conditions no real frame ever meets.
            for (size_t r = 0; r < rows.size(); ++r)
                rows[r].percent = 40.0 + 40.0 * sin((i + r * 7) * 0.05);
            Gdiplus::Graphics pg(&panel);
            pg.Clear(Gdiplus::Color(0, 0, 0, 0));
            MonDraw(&pg, size.cx, size.cy, rows, ctx);
        }
        QueryPerformanceCounter(&b);

        const double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 /
                          (double)freq.QuadPart / kFrames;
        wprintf(L"  %-10s %4ldx%-4ld  %6.3f ms/frame   %5.2f%% of a core at 30 fps\n",
                MonitorStyleAt(style).name, size.cx, size.cy, ms, ms * 30.0 / 10.0);
    }
}

int wmain(int argc, wchar_t** argv) {
    const std::wstring first = (argc > 1) ? argv[1] : L".";
    const bool bench = (first == L"--bench");
    const std::wstring out = bench ? L"." : first;

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);
    const CLSID png = PngEncoder();

    BuildHistory();
    BuildMetrics();

    if (bench) {
        wprintf(L"painter, eight metrics, busiest-app line on:\n");
        Bench();
        Gdiplus::GdiplusShutdown(token);
        return 0;
    }

    // Every style, on the default skin, with six metrics on.
    wprintf(L"styles:\n");
    for (int i = 0; i < MonitorStyleCount(); ++i) {
        const MonitorSkin& skin = MonitorSkinAt(0);
        MonPaintCtx ctx;
        ctx.scale      = 1.0f;
        ctx.alpha      = (BYTE)(255 * 92 / 100);
        ctx.vertical   = (i != MON_STYLE_BARS && i != MON_STYLE_TICKER);
        ctx.graphs     = true;
        ctx.topApps    = false;
        ctx.historyLen = kMonHistory;
        ctx.style      = i;
        ctx.skin       = &skin;
        Shot(out + L"\\style-" + MonitorStyleAt(i).id + L".png", ctx, Rows(skin, 6), png);
    }

    // Every skin, in the Cards style, so the palettes can be compared.
    wprintf(L"skins:\n");
    for (int i = 0; i < MonitorSkinCount(); ++i) {
        const MonitorSkin& skin = MonitorSkinAt(i);
        MonPaintCtx ctx;
        ctx.scale      = 1.0f;
        ctx.alpha      = (BYTE)(255 * 92 / 100);
        ctx.vertical   = true;
        ctx.graphs     = true;
        ctx.topApps    = false;
        ctx.historyLen = kMonHistory;
        ctx.style      = MON_STYLE_CARDS;
        ctx.skin       = &skin;
        Shot(out + L"\\skin-" + MonitorSkinAt(i).id + L".png", ctx, Rows(skin, 4), png);
    }

    Gdiplus::GdiplusShutdown(token);
    return 0;
}
