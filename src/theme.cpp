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

const Face& Strong() {      // SemiBold SemiCondensed: labels, values, buttons
    static const Face f = Pick({ { L"Bahnschrift SemiBold SemiConden", FW_NORMAL },
                                 { L"Bahnschrift SemiBold",            FW_NORMAL },
                                 { L"Bahnschrift",                     FW_SEMIBOLD },
                                 { L"Segoe UI Semibold",               FW_NORMAL } });
    return f;
}
const Face& Plain() {       // SemiCondensed: the chosen word, captions
    static const Face f = Pick({ { L"Bahnschrift SemiCondensed", FW_NORMAL },
                                 { L"Bahnschrift",               FW_NORMAL },
                                 { L"Segoe UI",                  FW_NORMAL } });
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

    const Face& s = Strong();
    const Face& p = Plain();
    FontSet set;
    auto put = [&](Font which, int size, int weight, const wchar_t* face) {
        set.f[(int)which] = MakeFont(size, weight, dpi, face);
    };
    put(Font::Body,       95,  FW_NORMAL,   L"Segoe UI");
    put(Font::BodyBold,   95,  FW_SEMIBOLD, L"Segoe UI");
    put(Font::Small,      85,  FW_NORMAL,   L"Segoe UI");
    put(Font::Label,      120, s.weight,    s.name);
    put(Font::Value,      112, s.weight,    s.name);
    put(Font::ValueLight, 112, p.weight,    p.name);
    put(Font::Crumb,      200, p.weight,    p.name);
    put(Font::CrumbBold,  200, s.weight,    s.name);
    put(Font::Nav,        112, s.weight,    s.name);
    put(Font::Section,    85,  s.weight,    s.name);
    put(Font::Caption,    88,  p.weight,    p.name);
    put(Font::Title,      150, s.weight,    s.name);
    put(Font::Button,     105, s.weight,    s.name);
    put(Font::Key,        80,  s.weight,    s.name);
    put(Font::Query,      150, FW_NORMAL,   L"Segoe UI Semilight");
    return g_fontSets.emplace(dpi, set).first->second;
}

// ---------------------------------------------------------------- backdrop
struct Surface {
    int       w = 0, h = 0;
    HBITMAP   bmp = nullptr;
    ULONGLONG used = 0;
};
std::vector<Surface> g_surfaces;
constexpr size_t kMaxSurfaces = 3;

inline uint32_t Hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline float Smooth(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

inline uint32_t Pack(float r, float g, float b) {
    auto c = [](float v) -> uint32_t {
        return (uint32_t)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v + 0.5f));
    };
    return (c(r) << 16) | (c(g) << 8) | c(b);
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

// The screen behind everything: black, with the faintest cold light from the
// top left, a trace of warmth low on the right, and the corners pulled down.
// A function of the pixel, so it is the same picture every time; the noise is
// there because a gradient this dark shows each of its few steps as a band.
void RenderBackdrop(uint32_t* px, int w, int h) {
    const float W = (float)(std::max)(w, h);
    for (int y = 0; y < h; ++y) {
        const float fy = (float)y / (float)h;
        for (int x = 0; x < w; ++x) {
            const float fx = (float)x / (float)w;
            float r = 7.0f, g = 8.0f, b = 10.0f;

            const float nx = ((float)x - 0.10f * (float)w) / (0.95f * W);
            const float ny = ((float)y + 0.05f * (float)h) / (0.70f * W);
            const float cool = std::exp(-(nx * nx + ny * ny) * 2.6f);
            r += 10.0f * cool; g += 12.0f * cool; b += 16.0f * cool;

            const float wx = ((float)x - 0.98f * (float)w) / (0.55f * W);
            const float wy = ((float)y - 1.02f * (float)h) / (0.40f * W);
            const float warm = std::exp(-(wx * wx + wy * wy) * 2.2f);
            r += 9.0f * warm; g += 5.0f * warm; b += 1.0f * warm;

            const float vx = (fx - 0.5f) / 0.64f, vy = (fy - 0.45f) / 0.72f;
            const float vig = 1.0f - 0.40f * Smooth(0.50f, 1.30f, std::sqrt(vx * vx + vy * vy));
            const float n = (float)(Hash((uint32_t)x * 73856093U ^ (uint32_t)y * 19349663U) & 255)
                            / 255.0f - 0.5f;
            px[(size_t)y * (size_t)w + (size_t)x] = Pack(r * vig + n, g * vig + n, b * vig + n);
        }
    }
}

Surface* SurfaceFor(int w, int h) {
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return nullptr;
    const ULONGLONG now = GetTickCount64();
    for (Surface& s : g_surfaces)
        if (s.w == w && s.h == h) { s.used = now; return &s; }

    if (g_surfaces.size() >= kMaxSurfaces) {
        auto oldest = std::min_element(g_surfaces.begin(), g_surfaces.end(),
            [](const Surface& a, const Surface& b) { return a.used < b.used; });
        if (oldest->bmp) DeleteObject(oldest->bmp);
        g_surfaces.erase(oldest);
    }
    Surface s;
    s.w = w; s.h = h; s.used = now;
    uint32_t* bits = nullptr;
    s.bmp = MakeDib(w, h, &bits);
    if (!s.bmp) return nullptr;
    RenderBackdrop(bits, w, h);
    g_surfaces.push_back(s);
    return &g_surfaces.back();
}

// A premultiplied 32-bit layer the size of `r`, filled by `alphaAt`, blended
// onto `dc`. The glows are all this shape: a colour, and a per-pixel alpha.
template <typename AlphaAt>
void BlendLayer(HDC dc, const RECT& r, COLORREF color, AlphaAt alphaAt) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
    uint32_t* bits = nullptr;
    HBITMAP bmp = MakeDib(w, h, &bits);
    if (!bmp) return;
    const float cr = (float)GetRValue(color), cg = (float)GetGValue(color),
                cb = (float)GetBValue(color);
    bool any = false;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
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
    TrimSurfaces();
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

// ================================================================ the screen
void PaintBackdrop(HDC dc, const RECT& r, SIZE canvas) {
    Surface* s = SurfaceFor(canvas.cx, canvas.cy);
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (!s) {
        HBRUSH b = CreateSolidBrush(Bg);
        FillRect(dc, &r, b);
        DeleteObject(b);
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    if (!mem) return;
    HGDIOBJ old = SelectObject(mem, s->bmp);
    BitBlt(dc, r.left, r.top, (std::min)(w, s->w), (std::min)(h, s->h), mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
}

void TrimSurfaces() {
    for (Surface& s : g_surfaces)
        if (s.bmp) DeleteObject(s.bmp);
    g_surfaces.clear();
    g_surfaces.shrink_to_fit();
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
    });
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
    // Warm light filling the row from the right, where the control is.
    Sweep(dc, row, AmberGlow, (BYTE)(10.0f * t), (BYTE)(46.0f * t));
    Glow(dc, row, AmberGlow, Sc(16), (BYTE)(118.0f * t));
    Frame(dc, row, Mix(RGB(90, 60, 10), Amber, t), (BYTE)(255.0f * (std::min)(1.0f, t * 1.4f)),
          (std::max)(2, Sc(2)));
}

namespace {

// The colour of a word that is not on a bar.
COLORREF LooseInk(const Look& look) {
    if (!look.enabled) return TextMute;
    return Mix(Text, Amber, look.lit);
}

void Bar(HDC dc, const RECT& r, const Look& look) {
    if (!look.enabled) { Wash(dc, r, Slate, 255); return; }
    Gradient(dc, r, Mix(Fill, AmberHot, look.lit), Mix(FillLow, Amber, look.lit));
}

COLORREF BarInk(const Look& look) {
    if (!look.enabled) return SlateText;
    return Mix(FillText, AmberText, look.lit);
}

} // namespace

void DrawPair(HDC dc, const RECT& r, const std::wstring& first, const std::wstring& second,
              int chosen, const Look& look) {
    const int mid = (r.left + r.right) / 2;
    const RECT half[2] = { { r.left, r.top, mid, r.bottom }, { mid, r.top, r.right, r.bottom } };
    const std::wstring* words[2] = { &first, &second };

    // The glow of a lit row pools behind the chosen word, as the game's does.
    if (look.enabled && look.lit > 0.01f && (chosen == 0 || chosen == 1)) {
        RECT pool = half[chosen];
        InflateRect(&pool, Sc(34), Sc(18));
        Haze(dc, pool, AmberGlow, (BYTE)(70.0f * look.lit));
    }
    for (int i = 0; i < 2; ++i) {
        const bool on = (i == chosen);
        if (on) {
            Bar(dc, half[i], look);
            Print(dc, Font::ValueLight, Caps(*words[i]), half[i], BarInk(look),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            if (look.enabled && look.hot == i) {
                RECT box = half[i];
                InflateRect(&box, -Sc(1), -Sc(1));
                Frame(dc, box, LooseInk(look), 90, 1);
            }
            Print(dc, Font::Value, Caps(*words[i]), half[i], LooseInk(look),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
}

void DrawSelector(HDC dc, const RECT& r, const std::wstring& text, const Look& look,
                  bool canBack, bool canForward, COLORREF swatch) {
    const int h = r.bottom - r.top;
    const float cy = (float)(r.top + r.bottom) / 2.0f;
    const float size = (float)h * 0.30f;
    const float lx = (float)r.left + size + (float)Sc(4);
    const float rx = (float)r.right - size - (float)Sc(4);

    if (look.enabled && look.lit > 0.01f) {
        RECT pool = r;
        InflateRect(&pool, Sc(10), Sc(14));
        Haze(dc, pool, AmberGlow, (BYTE)(46.0f * look.lit));
    }
    const COLORREF ink = LooseInk(look);
    auto arrow = [&](float x, int dir, bool can, bool hot) {
        COLORREF c = ink;
        BYTE a = 255;
        if (!can) a = 70;
        else if (hot && look.enabled) c = look.lit > 0.5f ? AmberHot : RGB(255, 255, 255);
        Arrowhead(dc, x, cy, size * (hot && can ? 1.12f : 1.0f), dir, c, a);
    };
    arrow(lx, -1, canBack, look.hot == 0);
    arrow(rx, +1, canForward, look.hot == 2);

    RECT tr = { (int)(lx + size * 1.4f), r.top, (int)(rx - size * 1.4f), r.bottom };
    std::wstring caps = Caps(text);
    if (swatch != CLR_INVALID) {
        // A chip of the colour, then its name.
        const int chip = (int)((float)h * 0.46f);
        const int tw = Measure(dc, Font::Value, caps);
        const int total = chip + Sc(9) + tw;
        int x = (int)tr.left + (std::max)(0, (int)((tr.right - tr.left) - total) / 2);
        RECT sw = { x, (int)cy - chip / 2, x + chip, (int)cy - chip / 2 + chip };
        Wash(dc, sw, swatch, 255);
        Frame(dc, sw, look.enabled ? Mix(Text, Amber, look.lit) : TextMute, 200, 1);
        tr.left = sw.right + Sc(9);
        Print(dc, Font::Value, caps, tr, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    Print(dc, Font::Value, caps, tr, ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void DrawSlider(HDC dc, const RECT& r, float fraction, const std::wstring& text,
                const Look& look) {
    fraction = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
    const COLORREF groove = !look.enabled ? RGB(34, 38, 44) : Mix(Track, AmberTrack, look.lit);
    Wash(dc, r, groove, 255);

    const int split = r.left + (int)((float)(r.right - r.left) * fraction + 0.5f);
    RECT done = { r.left, r.top, split, r.bottom };
    if (done.right > done.left) Bar(dc, done, look);

    // The number sits across the edge of the fill, so it is printed twice and
    // each copy clipped to its own side: dark on the bar, light on the groove.
    const std::wstring caps = Caps(text);
    const COLORREF light = !look.enabled ? SlateText : RGB(250, 251, 252);
    int saved = SaveDC(dc);
    IntersectClipRect(dc, split, r.top, r.right, r.bottom);
    Print(dc, Font::Value, caps, r, light, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RestoreDC(dc, saved);
    saved = SaveDC(dc);
    IntersectClipRect(dc, r.left, r.top, split, r.bottom);
    Print(dc, Font::Value, caps, r, BarInk(look), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RestoreDC(dc, saved);

    if (look.enabled && (look.hot >= 0 || look.pressed)) {
        // Where a click would put it.
        RECT tick = { split - (std::max)(1, Sc(1)), r.top - Sc(3), split + (std::max)(1, Sc(1)),
                      r.bottom + Sc(3) };
        Wash(dc, tick, look.lit > 0.5f ? AmberHot : Text, 255);
    }
}

void DrawAction(HDC dc, const RECT& r, const std::wstring& text, const Look& look, bool danger) {
    const int cut = Sc(9);
    const float w = ScF(1.5f);
    COLORREF ink = danger ? RGB(255, 120, 104) : Text;
    if (!look.enabled) {
        CutBox(dc, r, cut, 0, 0, TextMute, 150, w);
        Print(dc, Font::Value, Caps(text), r, TextMute, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    if (look.pressed) {
        CutBox(dc, r, cut, AmberHot, 255, AmberHot, 255, w);
        Print(dc, Font::Value, Caps(text), r, AmberText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    const float lit = (std::max)(look.lit, look.hot >= 0 ? 0.6f : 0.0f);
    CutBox(dc, r, cut, AmberGlow, (BYTE)(40.0f * lit), Mix(ink, Amber, lit),
           (BYTE)(150.0f + 105.0f * lit), w);
    Print(dc, Font::Value, Caps(text), r, Mix(ink, Amber, lit),
          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
    if (key.empty()) return 0;
    const std::wstring caps = Caps(key);
    const Font font = large ? Font::Button : Font::Key;
    const int track = large ? 0 : (std::max)(1, Sc(1)) - 1;
    const int tw = Measure(dc, font, caps, track);
    const int h  = large ? Sc(26) : Sc(18);
    const int w  = (std::max)(h, tw + (large ? Sc(18) : Sc(12)));
    if (!measureOnly) {
        RECT box = { x, centreY - h / 2, x + w, centreY - h / 2 + h };
        if (solid) {
            Wash(dc, box, ink, 255);
        } else {
            Wash(dc, box, ink, 20);
            Frame(dc, box, ink, 200, 1);
        }
        Print(dc, font, caps, box, solid ? RGB(18, 18, 20) : ink,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE, track);
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

int ButtonWidth(HDC dc, const std::wstring& label, const wchar_t* key) {
    int w = Measure(dc, Font::Button, Caps(label), Sc(1)) + Sc(40);
    if (key && *key) w += Keycap(dc, 0, 0, key, Text, true) + Sc(9);
    return w;
}

void DrawButton(HDC dc, const RECT& r, const std::wstring& label, const wchar_t* key,
                const ButtonLook& look) {
    const int cut = (r.bottom - r.top) * 2 / 5;
    const float w = (float)(std::max)(2, Sc(2));
    const bool lit = look.enabled && (look.hot || look.focused);
    COLORREF ink;
    if (!look.enabled) {
        ink = TextMute;
        CutBox(dc, r, cut, 0, 0, TextMute, 140, w);
    } else if (look.pressed) {
        ink = AmberText;
        CutBox(dc, r, cut, AmberHot, 255, AmberHot, 255, w);
    } else if (lit) {
        ink = Amber;
        Glow(dc, r, AmberGlow, Sc(12), 90);
        CutBox(dc, r, cut, AmberGlow, 34, Amber, 255, w);
    } else if (look.primary) {
        ink = Amber;
        CutBox(dc, r, cut, AmberGlow, 16, Amber, 210, w);
    } else {
        ink = Text;
        CutBox(dc, r, cut, 0, 0, RGB(196, 200, 206), 190, w);
    }

    const std::wstring caps = Caps(label);
    const int track = Sc(1);
    const int tw = Measure(dc, Font::Button, caps, track);
    const int kw = (key && *key) ? Keycap(dc, 0, 0, key, ink, true) + Sc(9) : 0;
    int x = r.left + ((r.right - r.left) - (tw + kw)) / 2;
    const int cy = (r.top + r.bottom) / 2;
    if (kw) {
        Keycap(dc, x, cy, key, ink, false, look.pressed);
        x += kw;
    }
    RECT tr = { x, r.top, x + tw + track, r.bottom };
    Print(dc, Font::Button, caps, tr, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE, track);
}

int Prompt(HDC dc, int x, int centreY, const std::wstring& key, const std::wstring& word,
           COLORREF ink, bool measureOnly) {
    const std::wstring caps = Caps(word);
    const int track = Sc(1);
    const int kw = Keycap(dc, 0, 0, key, ink, true);
    const int ww = Measure(dc, Font::Caption, caps, track);
    const int total = kw + Sc(7) + ww;
    if (measureOnly) return total;
    Keycap(dc, x, centreY, key, ink);
    RECT tr = { x + kw + Sc(7), centreY - Sc(12), x + total + track, centreY + Sc(12) };
    Print(dc, Font::Caption, caps, tr, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE, track);
    return total;
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

} // namespace theme
} // namespace awa
