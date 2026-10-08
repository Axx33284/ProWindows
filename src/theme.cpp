// ProWindows - the look. See theme.h for what it is modelled on.
#include "theme.h"
#include "config.h"
#include "winutil.h"
#include <objidl.h>
#include <cmath>
// GDI+ headers use bare min/max, which NOMINMAX removes. Feed them the
// std:: versions rather than re-enabling the Windows macros.
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "msimg32.lib")

namespace awa {
namespace theme {

namespace {

ULONG_PTR g_gdiplusToken = 0;

// The DPI everything is currently drawn at. Each window points this at its own
// DPI before it paints, so a per-monitor-v2 window on a 150% laptop screen and
// the search bar on a 100% external one both come out the right size.
UINT g_dpi = 96;

inline int   Sc(int px)     { return MulDiv(px, (int)g_dpi, 96); }
inline float ScF(float px)  { return px * (float)g_dpi / 96.0f; }

Gdiplus::Color Argb(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

inline BYTE Lerp8(int a, int b, float t) {
    const float v = (float)a + ((float)b - (float)a) * t;
    return (BYTE)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v + 0.5f));
}

// ---------------------------------------------------------------- dark mode
// The one piece of system chrome left - the tray's popup menu and the odd
// system dialog - follows the process's app mode, and the only way to set that
// is by ordinal out of uxtheme. Best-effort: on a build without it the menus
// simply stay light. MAP.md invariant 39.
enum PreferredAppMode { AppModeDefault = 0, AllowDark = 1, ForceDark = 2 };
using SetPreferredAppModeFn = PreferredAppMode (WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn     = void (WINAPI*)();

void EnableProcessDarkMode() {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    // 135 SetPreferredAppMode (1903+; on 1809 the same ordinal takes a BOOL,
    // and 1 or 2 means "dark" to both).
    auto setMode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    if (setMode) setMode(ForceDark);
    auto flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (flush) flush();
}

// ---------------------------------------------------------------- faces
// Whether a face is installed. GDI silently substitutes for a name it does not
// know, and the substitute for a condensed face is a wide one.
bool FaceExists(const wchar_t* face) {
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    HDC dc = GetDC(nullptr);
    int found = 0;
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD,
                                   LPARAM param) -> int {
        *reinterpret_cast<int*>(param) = 1;
        return 0;
    }, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found != 0;
}

// Bahnschrift's named instances are already the weight they say, so they are
// asked for at normal; the plain family has to be asked for the weight.
struct Face { const wchar_t* name; int weight; };

Face Pick(std::initializer_list<Face> choices) {
    for (const Face& f : choices)
        if (FaceExists(f.name)) return f;
    return { L"Segoe UI", FW_NORMAL };
}

const Face& Plain() {       // SemiCondensed: the chosen word, captions
    static const Face f = Pick({ { L"Bahnschrift SemiCondensed", FW_NORMAL },
                                 { L"Bahnschrift",               FW_NORMAL },
                                 { L"Segoe UI",                  FW_NORMAL } });
    return f;
}

// The same family one weight lighter for the title and tab captions, where the
// install has the named instance.
const Face& Light() {
    static const Face f = Pick({ { L"Bahnschrift SemiLight SemiCondensed", FW_NORMAL },
                                 { L"Bahnschrift Light SemiCondensed",     FW_NORMAL },
                                 Plain() });
    return f;
}

// Segoe UI Variable Text where Windows 11 has it, plain Segoe UI otherwise.
const wchar_t* ReadingFace() {
    static const wchar_t* f = FaceExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text"
                                                                    : L"Segoe UI";
    return f;
}

HFONT MakeFont(int tenthsOfPoint, int weight, UINT dpi, const wchar_t* face) {
    LOGFONTW lf = {};
    lf.lfHeight  = -MulDiv(tenthsOfPoint, (int)dpi, 720);
    lf.lfWeight  = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    return CreateFontIndirectW(&lf);
}

struct FontSet { HFONT f[(int)Font::Count] = {}; };

// One set per DPI the process has been shown at. Kept rather than swapped, so a
// DC that still has one selected never ends up holding a deleted font.
std::unordered_map<UINT, FontSet> g_fontSets;
const FontSet* g_fonts = nullptr;

const FontSet& FontsFor(UINT dpi) {
    auto it = g_fontSets.find(dpi);
    if (it != g_fontSets.end()) return it->second;

    const Face& p = Plain();
    const Face& light = Light();
    FontSet set;
    auto put = [&](Font which, int size, int weight, const wchar_t* face) {
        set.f[(int)which] = MakeFont(size, weight, dpi, face);
    };
    // 1 DIP = 0.75 pt; sizes are in tenths of a point.
    put(Font::Heading,    195, light.weight, light.name);
    put(Font::Tab,        128, light.weight, light.name);
    put(Font::Row,        113, FW_NORMAL,   ReadingFace());
    put(Font::Desc,       120, FW_NORMAL,   ReadingFace());
    put(Font::Plate,      105, FW_NORMAL,   ReadingFace());
    put(Font::Prompt,     120, FW_NORMAL,   ReadingFace());
    put(Font::Keycap,     90,  p.weight,    p.name);
    put(Font::Body,       95,  FW_NORMAL,   L"Segoe UI");
    put(Font::Small,      85,  FW_NORMAL,   L"Segoe UI");
    put(Font::Query,      150, FW_NORMAL,   L"Segoe UI Semilight");
    return g_fontSets.emplace(dpi, set).first->second;
}

HBITMAP MakeDib(int w, int h, uint32_t** bits) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;              // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* p = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    *bits = static_cast<uint32_t*>(p);
    if (bmp && !p) { DeleteObject(bmp); bmp = nullptr; }
    return bmp;
}

// A premultiplied 32-bit layer the size of `r`, filled by `alphaAt`, blended
// onto `dc`. The glows are all this shape: a colour, and a per-pixel alpha.
//
// `hole` (layer coordinates, optional) is a span the caller knows is fully
// transparent; the DIB starts zeroed, so those pixels are skipped, not asked.
// A glow around a wide row is mostly such a span.
template <typename AlphaAt>
void BlendLayer(HDC dc, const RECT& r, COLORREF color, AlphaAt alphaAt,
                RECT hole = { 0, 0, 0, 0 }) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
    uint32_t* bits = nullptr;
    HBITMAP bmp = MakeDib(w, h, &bits);
    if (!bmp) return;
    const float cr = (float)GetRValue(color), cg = (float)GetGValue(color),
                cb = (float)GetBValue(color);
    bool any = false;
    for (int y = 0; y < h; ++y) {
        const bool inHole = y >= hole.top && y < hole.bottom;
        for (int x = 0; x < w; ++x) {
            if (inHole && x == hole.left) { x = hole.right - 1; continue; }
            float a = alphaAt(x, y);
            if (a <= 0.0f) { bits[(size_t)y * w + x] = 0; continue; }
            if (a > 255.0f) a = 255.0f;
            any = true;
            const float k = a / 255.0f;
            bits[(size_t)y * w + x] = ((uint32_t)(a + 0.5f) << 24) |
                                      ((uint32_t)(cr * k + 0.5f) << 16) |
                                      ((uint32_t)(cg * k + 0.5f) << 8) |
                                      (uint32_t)(cb * k + 0.5f);
        }
    }
    if (any) {
        HDC mem = CreateCompatibleDC(dc);
        if (mem) {
            HGDIOBJ old = SelectObject(mem, bmp);
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            AlphaBlend(dc, r.left, r.top, w, h, mem, 0, 0, w, h, bf);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
    }
    DeleteObject(bmp);
}

} // namespace

// ================================================================ lifecycle
COLORREF Mix(COLORREF a, COLORREF b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return RGB(Lerp8(GetRValue(a), GetRValue(b), t),
               Lerp8(GetGValue(a), GetGValue(b), t),
               Lerp8(GetBValue(a), GetBValue(b), t));
}

void SetDpi(UINT dpi) {
    if (dpi < 48 || dpi > 960) return;        // nothing real is outside this
    if (dpi == g_dpi && g_fonts) return;
    g_fonts = &FontsFor(dpi);
    g_dpi   = dpi;
}

UINT  Dpi()              { return g_dpi; }
int   Scale(int px)      { return Sc(px); }
float ScaleF(float px)   { return ScF(px); }

void Init() {
    EnableProcessDarkMode();
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);

    // A starting point only: every window re-points this at its own DPI.
    HDC screen = GetDC(nullptr);
    const int systemDpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    g_fonts = nullptr;
    SetDpi(systemDpi > 0 ? (UINT)systemDpi : 96u);
}

void Shutdown() {
    for (auto& kv : g_fontSets)
        for (HFONT f : kv.second.f)
            if (f) DeleteObject(f);
    g_fontSets.clear();
    g_fonts = nullptr;
    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

HFONT Get(Font f) {
    if (!g_fonts) SetDpi(g_dpi ? g_dpi : 96);
    return g_fonts ? g_fonts->f[(int)f] : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

// ================================================================ type
std::wstring Caps(const std::wstring& text) {
    std::wstring out = text;
    for (auto& c : out) c = (wchar_t)towupper(c);
    return out;
}

int SpacedWidth(HDC dc, const std::wstring& text, int tracking) {
    if (text.empty()) return 0;
    // Measured unspaced and the spacing added by hand: GetTextExtentPoint32
    // does not count SetTextCharacterExtra, though DrawText draws with it.
    const int old = SetTextCharacterExtra(dc, 0);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    SetTextCharacterExtra(dc, old);
    return sz.cx + tracking * ((int)text.size() - 1);
}

void DrawSpaced(HDC dc, const std::wstring& text, RECT* r, UINT format, int tracking) {
    RECT box = *r;
    if (tracking && (format & (DT_CENTER | DT_RIGHT)) && !(format & DT_WORDBREAK)) {
        // DrawText centres and right-aligns by the width of the letters
        // without the space added between them, so spaced text ran past the
        // right edge and lost its last letter. Placed by hand instead, from
        // the width it really has - and still clipped on the left, never the
        // right, when the box is too small for it.
        const int w = SpacedWidth(dc, text, tracking);
        const int room = box.right - box.left;
        if (w <= room) {
            if (format & DT_CENTER) box.left += (room - w) / 2;
            else                    box.left = box.right - w;
            box.right = box.left + w + tracking;
        }
        format &= ~(UINT)(DT_CENTER | DT_RIGHT);
    }
    const int old = SetTextCharacterExtra(dc, tracking);
    DrawTextW(dc, text.c_str(), (int)text.size(), &box, format);
    SetTextCharacterExtra(dc, old);
}

void Print(HDC dc, Font font, const std::wstring& text, RECT r, COLORREF color,
           UINT format, int tracking) {
    if (text.empty()) return;
    HGDIOBJ old = SelectObject(dc, Get(font));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawSpaced(dc, text, &r, format | DT_NOPREFIX, tracking);
    SelectObject(dc, old);
}

int Measure(HDC dc, Font font, const std::wstring& text, int tracking) {
    HGDIOBJ old = SelectObject(dc, Get(font));
    const int w = SpacedWidth(dc, text, tracking);
    SelectObject(dc, old);
    return w;
}

int PrintWrapped(HDC dc, Font font, const std::wstring& text, RECT r, COLORREF color,
                 bool measureOnly) {
    if (text.empty()) return 0;
    HGDIOBJ old = SelectObject(dc, Get(font));
    RECT calc = r;
    DrawTextW(dc, text.c_str(), (int)text.size(), &calc,
              DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    if (!measureOnly) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        DrawTextW(dc, text.c_str(), (int)text.size(), &r,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, old);
    return calc.bottom - calc.top;
}

void DarkTitleBar(HWND wnd) {
    BOOL on = TRUE;
    // 20 on current builds, 19 on 1809-era ones.
    if (FAILED(DwmSetWindowAttribute(wnd, 20, &on, sizeof(on))))
        DwmSetWindowAttribute(wnd, 19, &on, sizeof(on));
    const COLORREF caption = Bg;
    DwmSetWindowAttribute(wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    const COLORREF border = RGB(52, 56, 62);
    DwmSetWindowAttribute(wnd, AWA_DWMWA_BORDER_COLOR, &border, sizeof(border));
}

// ================================================================ shapes
void Wash(HDC dc, const RECT& r, COLORREF color, BYTE alpha) {
    if (r.right <= r.left || r.bottom <= r.top || alpha == 0) return;
    if (alpha == 255) {
        HBRUSH b = CreateSolidBrush(color);
        FillRect(dc, &r, b);
        DeleteObject(b);
        return;
    }
    Gdiplus::Graphics g(dc);
    Gdiplus::SolidBrush brush(Argb(color, alpha));
    g.FillRectangle(&brush, (INT)r.left, (INT)r.top, (INT)(r.right - r.left),
                    (INT)(r.bottom - r.top));
}

void Sweep(HDC dc, const RECT& r, COLORREF color, BYTE alphaLeft, BYTE alphaRight) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    Gdiplus::Graphics g(dc);
    // One pixel wider than the rectangle, so the gradient's own wrap-around
    // does not put a sliver of the far colour down the first column.
    Gdiplus::LinearGradientBrush brush(
        Gdiplus::Point(r.left - 1, 0), Gdiplus::Point(r.right + 1, 0),
        Argb(color, alphaLeft), Argb(color, alphaRight));
    g.FillRectangle(&brush, (INT)r.left, (INT)r.top, (INT)(r.right - r.left),
                    (INT)(r.bottom - r.top));
}

void Gradient(HDC dc, const RECT& r, COLORREF top, COLORREF bottom) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    TRIVERTEX v[2] = {
        { r.left,  r.top,    (COLOR16)(GetRValue(top) << 8), (COLOR16)(GetGValue(top) << 8),
          (COLOR16)(GetBValue(top) << 8), 0xFF00 },
        { r.right, r.bottom, (COLOR16)(GetRValue(bottom) << 8), (COLOR16)(GetGValue(bottom) << 8),
          (COLOR16)(GetBValue(bottom) << 8), 0xFF00 },
    };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

void FadeV(HDC dc, const RECT& r, COLORREF color, BYTE alphaTop, BYTE alphaBottom) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    Gdiplus::Graphics g(dc);
    Gdiplus::LinearGradientBrush brush(
        Gdiplus::Point(0, r.top - 1), Gdiplus::Point(0, r.bottom + 1),
        Argb(color, alphaTop), Argb(color, alphaBottom));
    g.FillRectangle(&brush, (INT)r.left, (INT)r.top, (INT)(r.right - r.left),
                    (INT)(r.bottom - r.top));
}

void Frame(HDC dc, const RECT& r, COLORREF color, BYTE alpha, int width) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w < 2 || h < 2 || alpha == 0 || width <= 0) return;
    const int t = (std::min)(width, (std::min)(w, h) / 2);
    // Four axis-aligned fills, no antialiasing: a hairline that is exactly one
    // device pixel, never two half-lit ones.
    const RECT parts[4] = {
        { r.left, r.top, r.right, r.top + t },
        { r.left, r.bottom - t, r.right, r.bottom },
        { r.left, r.top + t, r.left + t, r.bottom - t },
        { r.right - t, r.top + t, r.right, r.bottom - t },
    };
    if (alpha == 255) {
        HBRUSH b = CreateSolidBrush(color);
        for (const RECT& p : parts) FillRect(dc, &p, b);
        DeleteObject(b);
        return;
    }
    Gdiplus::Graphics g(dc);
    Gdiplus::SolidBrush brush(Argb(color, alpha));
    for (const RECT& p : parts)
        g.FillRectangle(&brush, (INT)p.left, (INT)p.top, (INT)(p.right - p.left),
                        (INT)(p.bottom - p.top));
}

void Glow(HDC dc, const RECT& r, COLORREF color, int spread, BYTE peak) {
    if (spread <= 0 || peak == 0) return;
    RECT o = r;
    InflateRect(&o, spread, spread);
    const float fs = (float)spread;
    const float inner = (std::max)(2.0f, fs * 0.45f);
    const float left = (float)(r.left - o.left), top = (float)(r.top - o.top);
    const float right = left + (float)(r.right - r.left) - 1.0f;
    const float bottom = top + (float)(r.bottom - r.top) - 1.0f;
    const float p = (float)peak;
    // Deeper than `inner` from every edge the alpha is zero: skip it.
    RECT hole = { (int)std::ceil(left + inner), (int)std::ceil(top + inner),
                  (int)std::floor(right - inner) + 1, (int)std::floor(bottom - inner) + 1 };
    if (hole.right <= hole.left || hole.bottom <= hole.top) hole = { 0, 0, 0, 0 };
    BlendLayer(dc, o, color, [&](int x, int y) -> float {
        const float fx = (float)x, fy = (float)y;
        const float dx = fx < left ? left - fx : (fx > right ? fx - right : 0.0f);
        const float dy = fy < top ? top - fy : (fy > bottom ? fy - bottom : 0.0f);
        if (dx > 0.0f || dy > 0.0f) {
            const float d = std::sqrt(dx * dx + dy * dy) / fs;
            if (d >= 1.0f) return 0.0f;
            const float k = 1.0f - d;
            return p * k * k;
        }
        // Inside: a little light carried in from the edge.
        const float d = (std::min)((std::min)(fx - left, right - fx),
                                   (std::min)(fy - top, bottom - fy)) / inner;
        if (d >= 1.0f) return 0.0f;
        const float k = 1.0f - d;
        return p * 0.55f * k * k;
    }, hole);
}

void Haze(HDC dc, const RECT& r, COLORREF color, BYTE peak) {
    const float cx = (float)(r.right - r.left) / 2.0f, cy = (float)(r.bottom - r.top) / 2.0f;
    if (cx < 1.0f || cy < 1.0f) return;
    const float p = (float)peak;
    BlendLayer(dc, r, color, [&](int x, int y) -> float {
        const float nx = ((float)x + 0.5f - cx) / cx, ny = ((float)y + 0.5f - cy) / cy;
        const float d = 1.0f - (nx * nx + ny * ny);
        if (d <= 0.0f) return 0.0f;
        return p * d * d;
    });
}

void Arrowhead(HDC dc, float cx, float cy, float size, int dir, COLORREF color, BYTE alpha) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush brush(Argb(color, alpha));
    const float s = size, w = size * 0.80f;
    Gdiplus::PointF pts[3];
    switch (dir) {
        case -1: pts[0] = { cx - w, cy }; pts[1] = { cx + w, cy - s }; pts[2] = { cx + w, cy + s }; break;
        case +1: pts[0] = { cx + w, cy }; pts[1] = { cx - w, cy - s }; pts[2] = { cx - w, cy + s }; break;
        case -2: pts[0] = { cx, cy - w }; pts[1] = { cx + s, cy + w }; pts[2] = { cx - s, cy + w }; break;
        default: pts[0] = { cx, cy + w }; pts[1] = { cx - s, cy - w }; pts[2] = { cx + s, cy - w }; break;
    }
    g.FillPolygon(&brush, pts, 3);
}

void Chevron(HDC dc, float cx, float cy, float size, int dir, COLORREF color, float width) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Argb(color), width);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    const float s = size, h = size * 0.5f;
    Gdiplus::PointF pts[3];
    switch (dir) {
        case -1: pts[0] = { cx + h, cy - s }; pts[1] = { cx - h, cy }; pts[2] = { cx + h, cy + s }; break;
        case +1: pts[0] = { cx - h, cy - s }; pts[1] = { cx + h, cy }; pts[2] = { cx - h, cy + s }; break;
        case -2: pts[0] = { cx - s, cy + h }; pts[1] = { cx, cy - h }; pts[2] = { cx + s, cy + h }; break;
        default: pts[0] = { cx - s, cy - h }; pts[1] = { cx, cy + h }; pts[2] = { cx + s, cy - h }; break;
    }
    g.DrawLines(&pen, pts, 3);
}

void Diamond(HDC dc, float cx, float cy, float r, COLORREF color) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush halo(Argb(color, 60));
    Gdiplus::PointF outer[4] = { { cx, cy - r * 2.0f }, { cx + r * 2.0f, cy },
                                 { cx, cy + r * 2.0f }, { cx - r * 2.0f, cy } };
    g.FillPolygon(&halo, outer, 4);
    Gdiplus::SolidBrush fill(Argb(color));
    Gdiplus::PointF d[4] = { { cx, cy - r }, { cx + r, cy }, { cx, cy + r }, { cx - r, cy } };
    g.FillPolygon(&fill, d, 4);
}

int PanelNotch() { return Sc(9); }

void PanelFrame(HDC dc, const RECT& r, COLORREF color, BYTE alpha, float width, int inset) {
    const float half = width / 2.0f;
    const float x0 = (float)r.left + half, x1 = (float)r.right - half;
    const float y0 = (float)r.top + half,  y1 = (float)r.bottom - half;
    const float W = x1 - x0;
    const float d = (float)inset;
    // The run of each slope, and a curve through it rather than a straight
    // cut: the game's frame bends into its notches.
    const float s = d * 2.6f;
    if (W < s * 6.0f) {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(Argb(color, alpha), width);
        g.DrawRectangle(&pen, x0, y0, W, y1 - y0);
        return;
    }
    const float tA = x0 + W * 0.57f, tB = x0 + W * 0.89f;   // top notch, dipping in
    const float bA = x0 + W * 0.18f, bB = x0 + W * 0.79f;   // bottom notch, rising in

    Gdiplus::GraphicsPath path;
    path.StartFigure();
    path.AddLine(x0, y1, x0, y0);
    path.AddLine(x0, y0, tA, y0);
    path.AddBezier(tA, y0, tA + s * 0.5f, y0, tA + s * 0.5f, y0 + d, tA + s, y0 + d);
    path.AddLine(tA + s, y0 + d, tB - s, y0 + d);
    path.AddBezier(tB - s, y0 + d, tB - s * 0.5f, y0 + d, tB - s * 0.5f, y0, tB, y0);
    path.AddLine(tB, y0, x1, y0);
    path.AddLine(x1, y0, x1, y1);
    path.AddLine(x1, y1, bB, y1);
    path.AddBezier(bB, y1, bB - s * 0.5f, y1, bB - s * 0.5f, y1 - d, bB - s, y1 - d);
    path.AddLine(bB - s, y1 - d, bA + s, y1 - d);
    path.AddBezier(bA + s, y1 - d, bA + s * 0.5f, y1 - d, bA + s * 0.5f, y1, bA, y1);
    path.AddLine(bA, y1, x0, y1);
    path.CloseFigure();

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::Pen pen(Argb(color, alpha), width);
    pen.SetLineJoin(Gdiplus::LineJoinMiter);
    g.DrawPath(&pen, &path);
}

void CutBox(HDC dc, const RECT& r, int cut, COLORREF fill, BYTE fillAlpha,
            COLORREF edge, BYTE edgeAlpha, float width) {
    if (r.right - r.left < 4 || r.bottom - r.top < 4) return;
    const float half = width / 2.0f;
    const float x0 = (float)r.left + half, x1 = (float)r.right - half;
    const float y0 = (float)r.top + half,  y1 = (float)r.bottom - half;
    const float c = (std::min)((float)cut, (y1 - y0) * 0.6f);
    Gdiplus::PointF pts[5] = { { x0, y0 }, { x1 - c, y0 }, { x1, y0 + c }, { x1, y1 }, { x0, y1 } };

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    if (fillAlpha) {
        // The fill covers the stroke's own footprint too, so the edge sits on it.
        Gdiplus::PointF f[5] = { { x0 - half, y0 - half }, { x1 - c + half * 0.4f, y0 - half },
                                 { x1 + half, y0 + c - half * 0.4f }, { x1 + half, y1 + half },
                                 { x0 - half, y1 + half } };
        Gdiplus::SolidBrush brush(Argb(fill, fillAlpha));
        g.FillPolygon(&brush, f, 5);
    }
    if (edgeAlpha) {
        Gdiplus::Pen pen(Argb(edge, edgeAlpha), width);
        pen.SetLineJoin(Gdiplus::LineJoinMiter);
        g.DrawPolygon(&pen, pts, 5);
    }
}

// ================================================================ widgets
void RowFocus(HDC dc, const RECT& row, float t) {
    if (t <= 0.01f) return;
    if (t > 1.0f) t = 1.0f;
    const int w = row.right - row.left, h = row.bottom - row.top;
    if (w < 4 || h < 4) return;
    auto a8 = [&](float v) { return (BYTE)(v * t + 0.5f); };

    // Soft white bleed a few pixels outside the bar.
    Glow(dc, row, RGB(255, 255, 255), Sc(4), a8(44.0f));

    // The metal: four stops of vertical gradient, faded in from the screen.
    const COLORREF stop[4] = { Mix(Bg, Metal0, t), Mix(Bg, Metal1, t),
                               Mix(Bg, Metal2, t), Mix(Bg, Metal3, t) };
    const float at[4] = { 0.0f, 0.18f, 0.60f, 1.0f };
    for (int i = 0; i < 3; ++i) {
        RECT band = { row.left, row.top + (int)(h * at[i] + 0.5f), row.right,
                      i == 2 ? row.bottom : row.top + (int)(h * at[i + 1] + 0.5f) };
        Gradient(dc, band, stop[i], stop[i + 1]);
    }

    // A sheen from the left, gone by 45 % of the width.
    RECT sheen = { row.left, row.top, row.left + (int)(w * 0.45f), row.bottom };
    Sweep(dc, sheen, RGB(255, 255, 255), a8(28.0f), 0);

    // A faint highlight where the value sits (the middle of the right half).
    const int hw = (int)(w * 0.36f);
    const int hc = row.left + (int)(w * 0.78f);
    RECT haze = { hc - hw / 2, row.top, hc + hw / 2, row.bottom };
    Haze(dc, haze, RGB(255, 255, 255), a8(18.0f));

    // The outline and the lit top edge just inside it.
    Frame(dc, row, MetalEdge, a8(255.0f), 1);
    RECT top = { row.left + 1, row.top + 1, row.right - 1, row.top + 2 };
    Wash(dc, top, RGB(255, 255, 255), a8(130.0f));
    RECT low = { row.left + 1, row.bottom - 2, row.right - 1, row.bottom - 1 };
    Wash(dc, low, RGB(255, 255, 255), a8(70.0f));
}

namespace {

// What a control's ink does as the row gains focus: grey to white.
COLORREF Ink(const Look& look, COLORREF rest = Text) {
    if (!look.enabled) return TextMute;
    return Mix(rest, TextHi, look.lit);
}

// An opaque rectangle on the pixel grid.
void Px(HDC dc, int x0, int y0, int x1, int y1, COLORREF c) {
    RECT r = { x0, y0, x1, y1 };
    Wash(dc, r, c, 255);
}

// The segment indicator under a Choice's value, centred on `cx`: a segment per
// option, or a track and a thumb when there are too many (or too little room).
void Segments(HDC dc, int cx, int y, int count, int index, int room, const Look& look) {
    if (count <= 0) return;
    const COLORREF on  = look.enabled ? SegOn : SegOffDis;
    const COLORREF off = look.enabled ? SegOff : Mix(Bg, SegOffDis, 0.55f);
    const int sw = Sc(24), gap = Sc(2), th = (std::max)(1, Sc(2));
    const int total = count * sw + (count - 1) * gap;
    if (count <= 8 && total <= room) {
        int x = cx - total / 2;
        for (int i = 0; i < count; ++i, x += sw + gap)
            Px(dc, x, y, x + sw, y + th, i == index ? on : off);
        return;
    }
    const int tw = (std::min)(room, 8 * sw + 7 * gap);
    const int x0 = cx - tw / 2;
    Px(dc, x0, y, x0 + tw, y + th, off);
    const int thumb = (std::max)(Sc(4), tw / count);
    const int tx = x0 + (count > 1 ? (int)((long long)(tw - thumb) * index / (count - 1)) : 0);
    Px(dc, tx, y, tx + thumb, y + th, on);
}

// The - and + glyphs of a slider.
void Sign(HDC dc, int cx, int cy, bool plus, COLORREF c) {
    const int arm = Sc(4);
    const int t = (std::max)(1, Sc(1));
    Px(dc, cx - arm, cy, cx + arm + 1, cy + t, c);
    if (plus) Px(dc, cx, cy - arm, cx + t, cy + arm + 1, c);
}

} // namespace

void DrawSelector(HDC dc, const RECT& r, const std::wstring& text, const Look& look,
                  bool canBack, bool canForward, COLORREF swatch, int count, int index) {
    const float cy = (float)(r.top + r.bottom) / 2.0f;
    const float size = ScF(4.5f);                    // half the chevron's height
    const float pen = ScF(1.3f);
    const float lx = (float)r.left + size + (float)Sc(4);
    const float rx = (float)r.right - size - (float)Sc(4);

    const COLORREF ink = Ink(look);
    auto chevron = [&](float x, int dir, bool can, bool hot) {
        COLORREF c = !look.enabled ? TextMute : Mix(TextDim, TextHi, look.lit);
        if (hot && can && look.enabled) c = TextHi;
        if (!can) c = Mix(Bg, c, 0.35f);
        Chevron(dc, x, cy, size, dir, c, pen);
    };
    chevron(lx, -1, canBack, look.hot == 0);
    chevron(rx, +1, canForward, look.hot == 2);

    // With a segment row the value rides a little above the centre line.
    const bool segs = count > 0;
    const int lift = segs ? Sc(4) : 0;
    RECT tr = { (int)(lx + size + Sc(8)), r.top - lift, (int)(rx - size - Sc(8)), r.bottom - lift };
    if (swatch != CLR_INVALID) {
        // A chip of the colour, then its name.
        const int chip = Sc(10);
        const int tw = Measure(dc, Font::Row, text);
        const int total = chip + Sc(8) + tw;
        const int x = tr.left + (std::max)(0, (int)(tr.right - tr.left) - total) / 2;
        const int cyi = (tr.top + tr.bottom) / 2;
        RECT sw = { x, cyi - chip / 2, x + chip, cyi - chip / 2 + chip };
        Wash(dc, sw, look.enabled ? swatch : Mix(Bg, swatch, 0.4f), 255);
        Frame(dc, sw, ink, 120, 1);
        tr.left = sw.right + Sc(8);
        Print(dc, Font::Row, text, tr, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        Print(dc, Font::Row, text, tr, ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (segs) {
        const int cx = (int)(lx + rx) / 2;
        const int y = (r.top + r.bottom) / 2 + Sc(9);
        Segments(dc, cx, y, count, index, (std::max)(0, (int)(rx - lx) - Sc(24)), look);
    }
}

void DrawToggle(HDC dc, const RECT& r, const std::wstring& first, const std::wstring& second, int chosen,
                const Look& look) {
    DrawSelector(dc, r, chosen == 0 ? first : second, look, true, true, CLR_INVALID, 2, chosen);
}

void DrawSlider(HDC dc, const RECT& r, float fraction, const std::wstring& text,
                const Look& look) {
    fraction = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
    const int cy = (r.top + r.bottom) / 2;
    const float size = ScF(4.5f), pen = ScF(1.3f);
    const COLORREF ink = Ink(look);
    const COLORREF dim = !look.enabled ? TextMute : Mix(TextDim, TextHi, look.lit);

    // The value at the right edge.
    const int valW = (std::max)(Sc(36), Measure(dc, Font::Row, text));
    RECT vr = { r.right - valW, r.top, r.right, r.bottom };
    Print(dc, Font::Row, text, vr, ink, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // < - ruler + >
    const int end = r.right - valW - Sc(14);
    const float lx = (float)r.left + size + (float)Sc(4);
    const float rx = (float)end - size - (float)Sc(4);
    Chevron(dc, lx, (float)cy, size, -1, (look.hot == 0 && look.enabled) ? TextHi : dim, pen);
    Chevron(dc, rx, (float)cy, size, +1, (look.hot == 2 && look.enabled) ? TextHi : dim, pen);

    const int minusX = (int)lx + Sc(20), plusX = (int)rx - Sc(20);
    Sign(dc, minusX, cy, false, (look.hot == 1 && look.enabled) ? TextHi : dim);
    Sign(dc, plusX,  cy, true,  (look.hot == 3 && look.enabled) ? TextHi : dim);

    const int x0 = minusX + Sc(14), x1 = plusX - Sc(14);
    if (x1 - x0 < Sc(40)) return;
    const int t = (std::max)(1, Sc(1));
    Px(dc, x0, cy, x1, cy + t, dim);                       // the ruler
    const int shortTick = Sc(3), longTick = Sc(6);
    for (int i = 0; i < 20; ++i) {
        const int x = x0 + (int)((long long)(x1 - x0 - t) * i / 19);
        const int len = (i % 5 == 0) ? longTick : shortTick;
        Px(dc, x, cy - len / 2, x + t, cy - len / 2 + len + t, dim);
    }
    // The marker.
    const int mw = (std::max)(2, Sc(2)), mh = Sc(16);
    const int mx = x0 + (int)((float)(x1 - x0 - mw) * fraction + 0.5f);
    Px(dc, mx, cy - mh / 2, mx + mw, cy - mh / 2 + mh, look.enabled ? TextHi : TextMute);
}

namespace {

// A square missing its top-right corner, with an arrow leaving through the gap.
void OpenInIcon(HDC dc, float cx, float cy, COLORREF c) {
    const float s = ScF(14.0f), w = ScF(1.3f);
    const float x0 = cx - s / 2, x1 = cx + s / 2, y0 = cy - s / 2, y1 = cy + s / 2;
    const float gap = s * 0.58f, head = s * 0.36f;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::Pen pen(Argb(c), w);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::PointF box[5] = { { x0 + gap, y0 }, { x0, y0 }, { x0, y1 }, { x1, y1 }, { x1, y0 + gap } };
    g.DrawLines(&pen, box, 5);
    g.DrawLine(&pen, x0 + s * 0.42f, y1 - s * 0.42f, x1, y0);
    Gdiplus::PointF tip[3] = { { x1 - head, y0 }, { x1, y0 }, { x1, y0 + head } };
    g.DrawLines(&pen, tip, 3);
}

} // namespace

void DrawAction(HDC dc, const RECT& r, const std::wstring& text, const Look& look, bool danger) {
    COLORREF ink = !look.enabled ? TextMute : (danger ? Danger : Ink(look));
    if (look.enabled && look.pressed) ink = TextDim;
    const int iconW = Sc(14);
    const int cy = (r.top + r.bottom) / 2;
    OpenInIcon(dc, (float)(r.right - iconW / 2 - Sc(2)), (float)cy, ink);
    RECT tr = { r.left, r.top, r.right - iconW - Sc(12), r.bottom };
    Print(dc, Font::Row, text, tr, ink, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

std::wstring KeyName(UINT vk) {
    switch (vk) {
        case VK_LEFT:  return L"\x2190";
        case VK_UP:    return L"\x2191";
        case VK_RIGHT: return L"\x2192";
        case VK_DOWN:  return L"\x2193";
        case VK_RETURN: return L"Enter";
        case VK_ESCAPE: return L"Esc";
        case VK_BACK:  return L"Bksp";
        case VK_DELETE: return L"Del";
        case VK_PRIOR: return L"PgUp";
        case VK_NEXT:  return L"PgDn";
        case VK_OEM_3: return L"`";
        default: break;
    }
    return DescribeChord(0, vk);
}

int Keycap(HDC dc, int x, int centreY, const std::wstring& key, COLORREF ink,
           bool measureOnly, bool solid, bool large) {
    (void)ink;
    if (key.empty()) return 0;
    const std::wstring caps = Caps(key);
    const bool arrows = !caps.empty() && caps[0] >= 0x2190 && caps[0] <= 0x2193;
    const Font font = arrows ? Font::Body : (large ? Font::Tab : Font::Keycap);
    const int tw = Measure(dc, font, caps);
    const int h  = large ? Sc(26) : Sc(20);
    const int w  = (std::max)(h, tw + (large ? Sc(16) : Sc(12)));
    if (!measureOnly) {
        const float fx = (float)x, fy = (float)(centreY - h / 2);
        const float rad = ScF(2.0f), d = rad * 2.0f;
        Gdiplus::GraphicsPath path;
        path.AddArc(fx, fy, d, d, 180, 90);
        path.AddArc(fx + w - d, fy, d, d, 270, 90);
        path.AddArc(fx + w - d, fy + h - d, d, d, 0, 90);
        path.AddArc(fx, fy + h - d, d, d, 90, 90);
        path.CloseFigure();
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::SolidBrush brush(Argb(solid ? Mix(KeyFill, TextHi, 0.5f) : KeyFill));
        g.FillPath(&brush, &path);
        RECT box = { x, centreY - h / 2, x + w, centreY - h / 2 + h };
        Print(dc, font, caps, box, KeyInk, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    return w;
}

int Chord(HDC dc, int x, int centreY, UINT mods, UINT vk, COLORREF ink,
          bool alignRight, bool measureOnly, bool large) {
    std::vector<std::wstring> keys;
    if (mods & MOD_WIN)     keys.push_back(L"Win");
    if (mods & MOD_CONTROL) keys.push_back(L"Ctrl");
    if (mods & MOD_ALT)     keys.push_back(L"Alt");
    if (mods & MOD_SHIFT)   keys.push_back(L"Shift");
    if (vk) keys.push_back(KeyName(vk));
    if (keys.empty()) return 0;

    const int gap = large ? Sc(6) : Sc(4);
    int total = 0;
    for (size_t i = 0; i < keys.size(); ++i)
        total += Keycap(dc, 0, 0, keys[i], ink, true, false, large) + (i ? gap : 0);
    if (measureOnly) return total;

    int cx = alignRight ? x - total : x;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) cx += gap;
        cx += Keycap(dc, cx, centreY, keys[i], ink, false, false, large);
    }
    return total;
}

int Prompt(HDC dc, int x, int centreY, const std::wstring& key, const std::wstring& word,
           COLORREF ink, bool measureOnly) {
    const int kw = Keycap(dc, 0, 0, key, ink, true);
    const int ww = Measure(dc, Font::Prompt, word);
    const int total = kw + Sc(8) + ww;
    if (measureOnly) return total;
    Keycap(dc, x, centreY, key, ink);
    RECT tr = { x + kw + Sc(8), centreY - Sc(12), x + total + Sc(2), centreY + Sc(12) };
    Print(dc, Font::Prompt, word, tr, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    return total;
}

void SectionPlate(HDC dc, const RECT& r, const std::wstring& label) {
    const int h = Sc(26);
    const int bottom = r.bottom - Sc(7), top = bottom - h;
    const int slant = Sc(9), pad = Sc(16);
    const int tw = Measure(dc, Font::Plate, label);
    const int w = tw + pad * 2;
    const int line = (std::max)(1, Sc(1));
    {
        // The hairline along the plate's foot, on to the right.
        RECT ln = { r.left + w, bottom - line, r.right, bottom };
        if (ln.right > ln.left) Wash(dc, ln, Line, 255);
    }
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float x = (float)r.left, t = (float)top, b = (float)bottom, s = (float)slant;
    Gdiplus::PointF pts[4] = { { x + s, t }, { x + w + s, t }, { x + w, b }, { x, b } };
    Gdiplus::LinearGradientBrush brush(Gdiplus::PointF(0, t), Gdiplus::PointF(0, b + 1),
                                       Argb(Plate), Argb(PlateLow));
    g.FillPolygon(&brush, pts, 4);
    Gdiplus::Pen edge(Argb(Rule, 160), (float)line);
    g.DrawLine(&edge, pts[0], pts[1]);
    g.DrawLine(&edge, pts[3], pts[0]);
    g.DrawLine(&edge, pts[1], pts[2]);
    RECT tr = { r.left + pad + slant / 2, top, r.left + w, bottom };
    Print(dc, Font::Plate, label, tr, TextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void Mark(HDC dc, const RECT& r, COLORREF ink, COLORREF master) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int gap = (std::max)(1, w / 12);
    const int split = r.left + w * 11 / 20;
    const int mid = r.top + h / 2;
    RECT a = { r.left, r.top, split - gap / 2, r.bottom };
    RECT b = { split + (gap + 1) / 2, r.top, r.right, mid - gap / 2 };
    RECT c = { split + (gap + 1) / 2, mid + (gap + 1) / 2, r.right, r.bottom };
    Wash(dc, a, master, 255);
    Wash(dc, b, ink, 255);
    Wash(dc, c, Mix(ink, Bg, 0.45f), 255);
}

void Meter(HDC dc, const RECT& r, float used, float other) {
    used  = (std::max)(0.0f, (std::min)(1.0f, used));
    other = (std::max)(0.0f, (std::min)(1.0f - used, other));
    const int line = (std::max)(1, Sc(1));
    RECT in = { r.left + line, r.top + line, r.right - line, r.bottom - line };
    const int w = in.right - in.left;
    Frame(dc, r, MeterEdge, 255, line);
    RECT u = { in.left, in.top, in.left + (int)(w * used + 0.5f), in.bottom };
    if (u.right > u.left) Gradient(dc, u, MeterFill, Mix(MeterFill, Bg, 0.45f));
    RECT o = { u.right, in.top, u.right + (int)(w * other + 0.5f), in.bottom };
    if (o.right > o.left) {
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, o.left, o.top, o.right, o.bottom);
        const int step = (std::max)(3, Sc(4));
        HPEN pen = CreatePen(PS_SOLID, 1, MeterEdge);
        HGDIOBJ old = SelectObject(dc, pen);
        for (int x = o.left - (o.bottom - o.top); x < o.right; x += step) {
            MoveToEx(dc, x, o.bottom, nullptr);
            LineTo(dc, x + (o.bottom - o.top), o.top - 1);
        }
        SelectObject(dc, old);
        DeleteObject(pen);
        RestoreDC(dc, saved);
    }
}

} // namespace theme
} // namespace awa
