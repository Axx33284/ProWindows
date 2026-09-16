// ProWindows - offline renderer for the desktop clock.
//
// Draws the real clock, through the real painter, into PNG files - one per
// style on the default skin, one per skin on a style that suits it - on a
// wallpaper gradient so translucency and the shadow are visible. Nothing here
// is part of the product build.
//
//   clockshot.exe <output folder>
#include "../src/clockpaint.h"
#include "../src/clocktheme.h"
#include <objidl.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <cstdio>

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

void Wallpaper(Gdiplus::Graphics* g, int w, int h, bool light) {
    Gdiplus::LinearGradientBrush sky(
        Gdiplus::PointF(0, 0), Gdiplus::PointF((float)w, (float)h),
        light ? Gdiplus::Color(255, 214, 222, 232) : Gdiplus::Color(255, 32, 44, 68),
        light ? Gdiplus::Color(255, 240, 232, 220) : Gdiplus::Color(255, 96, 72, 104));
    g->FillRectangle(&sky, 0, 0, w, h);
    Gdiplus::SolidBrush tint(light ? Gdiplus::Color(18, 0, 0, 0) : Gdiplus::Color(26, 255, 255, 255));
    for (int y = 0; y < h; y += 32)
        for (int x = ((y / 32) % 2) * 32; x < w; x += 64)
            g->FillRectangle(&tint, x, y, 32, 32);
}

void Shot(const std::wstring& path, const ClockPaintCtx& ctx, const CLSID& png) {
    const SIZE size = ClockMeasure(ctx);
    const int pad = 26;
    // Light themes are shown on a light wallpaper, which is what they are for.
    const bool light = ctx.skin && (wcscmp(ctx.skin->id, L"ink") == 0 ||
                                    wcscmp(ctx.skin->id, L"paper") == 0 ||
                                    wcscmp(ctx.skin->id, L"frost") == 0 ||
                                    wcscmp(ctx.skin->id, L"lcd") == 0);

    Gdiplus::Bitmap canvas(size.cx + pad * 2, size.cy + pad * 2, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&canvas);
    Wallpaper(&g, size.cx + pad * 2, size.cy + pad * 2, light);

    Gdiplus::Bitmap panel(size.cx, size.cy, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics pg(&panel);
        pg.Clear(Gdiplus::Color(0, 0, 0, 0));
        ClockDraw(&pg, size.cx, size.cy, ctx);
    }
    g.DrawImage(&panel, pad, pad);
    canvas.Save(path.c_str(), &png, nullptr);
    wprintf(L"  %s  (%ldx%ld)\n", path.c_str(), size.cx, size.cy);
}

ClockPaintCtx Base() {
    ClockPaintCtx ctx;
    ctx.scale   = 1.0f;
    ctx.alpha   = (BYTE)(255 * 92 / 100);
    ctx.hours24 = false;
    ctx.seconds = true;
    ctx.date    = true;
    ctx.weekday = true;
    ctx.time = {};
    ctx.time.wYear = 2026; ctx.time.wMonth = 9; ctx.time.wDay = 18;
    ctx.time.wHour = 10; ctx.time.wMinute = 8; ctx.time.wSecond = 42;
    return ctx;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring out = (argc > 1) ? argv[1] : L".";

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);
    const CLSID png = PngEncoder();

    wprintf(L"clock styles:\n");
    for (int i = 0; i < ClockStyleCount(); ++i) {
        ClockPaintCtx ctx = Base();
        ctx.style = i;
        ctx.skin  = &ClockSkinAt(0);
        Shot(out + L"\\clock-style-" + ClockStyleAt(i).id + L".png", ctx, png);
        // And in 24-hour mode without seconds, which is the other common look.
        ctx.hours24 = true;
        ctx.seconds = false;
        Shot(out + L"\\clock-style-" + ClockStyleAt(i).id + L"-24h.png", ctx, png);
    }

    // Every skin, on the style it reads best in.
    wprintf(L"clock themes:\n");
    for (int i = 0; i < ClockSkinCount(); ++i) {
        const ClockSkin& skin = ClockSkinAt(i);
        ClockPaintCtx ctx = Base();
        ctx.skin = &skin;
        ctx.style = CLOCK_STYLE_DIGITAL;
        if (skin.bare) ctx.style = CLOCK_STYLE_MINIMAL;
        if (wcscmp(skin.id, L"lcd") == 0 || wcscmp(skin.id, L"terminal") == 0 ||
            wcscmp(skin.id, L"amber") == 0)
            ctx.style = CLOCK_STYLE_SEGMENTS;
        if (wcscmp(skin.id, L"nixie") == 0) ctx.style = CLOCK_STYLE_FLIP;
        if (wcscmp(skin.id, L"slayer") == 0) ctx.style = CLOCK_STYLE_STACKED;
        if (wcscmp(skin.id, L"paper") == 0) ctx.style = CLOCK_STYLE_ANALOG;
        if (wcscmp(skin.id, L"neon") == 0) ctx.style = CLOCK_STYLE_RING;
        Shot(out + L"\\clock-skin-" + skin.id + L".png", ctx, png);
    }

    Gdiplus::GdiplusShutdown(token);
    return 0;
}
