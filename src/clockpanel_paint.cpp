// ProWindows - the clock panel, painted.
//
// A Windows 11 Clock-style app in the black / metal Requiem theme: a left rail of
// pages, big readings, round buttons, progress rings, cards. All of it is GDI+
// (anti-aliased shapes) and GDI text into the DC the caller hands over; nothing
// here owns a window, a timer or a state. See clockpanel.h for the click model.
#include "clockpanel.h"
#include "alarm.h"
#include "theme.h"
#include <objidl.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <cmath>
#include <cwchar>

namespace awa {
namespace panel {

namespace {

using theme::Font;
using timer::Ticks;
using timer::kSecond;

constexpr Ticks kMinute = 60 * kSecond;
constexpr Ticks kDay = 24 * 60 * kMinute;

int S(int dip) { return theme::Scale(dip); }

struct Ctx {
    HDC dc;
    const Model& m;
    View& v;
    RECT clip;          // hits are cut to this (a scrolling list's viewport)
    bool live;          // register hits at all (a sheet over the page disables the page)
};

// ---------------------------------------------------------------- fonts
struct FontEntry { int h; int kind; UINT dpi; HFONT f; };
std::vector<FontEntry> g_fonts;

// A panel-sized font made from the theme's: kind 0 Bahnschrift (readings, titles),
// 1 Segoe UI (words). `dip` is the height at 96 dpi.
HFONT Fnt(int dip, int kind) {
    const UINT dpi = theme::Dpi();
    for (const FontEntry& e : g_fonts)
        if (e.h == dip && e.kind == kind && e.dpi == dpi) return e.f;
    LOGFONTW lf = {};
    GetObjectW(theme::Get(kind == 0 ? Font::Heading : Font::Row), sizeof(lf), &lf);
    lf.lfHeight = -S(dip);
    lf.lfWidth = 0;
    HFONT f = CreateFontIndirectW(&lf);
    if (!f) return theme::Get(Font::Row);
    g_fonts.push_back({ dip, kind, dpi, f });
    return f;
}

// ---------------------------------------------------------------- text
void Txt(HDC dc, HFONT font, const std::wstring& text, RECT r, COLORREF ink, UINT fmt) {
    if (text.empty()) return;
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ink);
    DrawTextW(dc, text.c_str(), (int)text.size(), &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(dc, old);
}

int TxtWidth(HDC dc, HFONT font, const std::wstring& text) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

// A small spaced caps label (inv. 77: Print and Measure do the spacing).
void Label(Ctx& c, const std::wstring& text, int x, int y, COLORREF ink = theme::TextDim) {
    const std::wstring caps = theme::Caps(text);
    const int track = S(2);
    RECT r = { x, y, x + theme::Measure(c.dc, Font::Small, caps, track) + S(4), y + S(18) };
    theme::Print(c.dc, Font::Small, caps, r, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE, track);
}

// A reading drawn digit by digit in equal cells, so the figures do not shuffle
// sideways as 1 and 0 change width. `align`: -1 left of x, 0 centred, +1 right.
int CellsWidth(HDC dc, const std::wstring& text) {
    SIZE zero = {};
    GetTextExtentPoint32W(dc, L"0", 1, &zero);
    int w = 0;
    for (wchar_t ch : text) {
        if (ch >= L'0' && ch <= L'9') { w += zero.cx; continue; }
        SIZE sz = {};
        GetTextExtentPoint32W(dc, &ch, 1, &sz);
        w += sz.cx;
    }
    return w;
}

int Cells(HDC dc, HFONT font, const std::wstring& text, int x, int top, int bottom, int align,
          COLORREF ink) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ink);
    const int total = CellsWidth(dc, text);
    int left = align < 0 ? x : (align == 0 ? x - total / 2 : x - total);
    const int start = left;
    SIZE zero = {};
    GetTextExtentPoint32W(dc, L"0", 1, &zero);
    for (wchar_t ch : text) {
        SIZE sz = {};
        GetTextExtentPoint32W(dc, &ch, 1, &sz);
        const bool digit = ch >= L'0' && ch <= L'9';
        const int cell = digit ? zero.cx : sz.cx;
        RECT r = { left + (cell - sz.cx) / 2, top, left + cell + sz.cx, bottom };
        DrawTextW(dc, &ch, 1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        left += cell;
    }
    SelectObject(dc, old);
    return start;
}

// ---------------------------------------------------------------- GDI+ bits
Gdiplus::Color Argb(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

struct Gfx {
    Gdiplus::Graphics g;
    explicit Gfx(HDC dc) : g(dc) {
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    }
};

void RoundPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    r = (std::min)(r, (std::min)(w, h) / 2.0f);
    const float d = r * 2;
    if (d <= 0) { p.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

void FillRound(HDC dc, const RECT& r, int radius, COLORREF fill, BYTE alpha = 255) {
    Gfx gf(dc);
    Gdiplus::GraphicsPath p;
    RoundPath(p, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), (float)radius);
    Gdiplus::SolidBrush b(Argb(fill, alpha));
    gf.g.FillPath(&b, &p);
}

void GradRound(HDC dc, const RECT& r, int radius, COLORREF top, COLORREF bottom) {
    Gfx gf(dc);
    Gdiplus::GraphicsPath p;
    RoundPath(p, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), (float)radius);
    Gdiplus::LinearGradientBrush b(Gdiplus::PointF(0, (float)r.top - 1), Gdiplus::PointF(0, (float)r.bottom + 1),
                                   Argb(top), Argb(bottom));
    gf.g.FillPath(&b, &p);
}

void StrokeRound(HDC dc, const RECT& r, int radius, COLORREF color, BYTE alpha = 255, float width = 1.0f) {
    Gfx gf(dc);
    Gdiplus::GraphicsPath p;
    const float h = width / 2.0f;
    RoundPath(p, (float)r.left + h, (float)r.top + h, (float)(r.right - r.left) - width,
              (float)(r.bottom - r.top) - width, (float)radius - h);
    Gdiplus::Pen pen(Argb(color, alpha), width);
    gf.g.DrawPath(&pen, &p);
}

int Hair() { return (std::max)(1, S(1)); }

// ---------------------------------------------------------------- hits and looks
void AddHit(Ctx& c, RECT r, int cmd, int arg = 0, bool wheel = false) {
    if (!c.live) return;
    RECT cut;
    if (!IntersectRect(&cut, &r, &c.clip)) return;
    Hit h;
    h.r = cut;
    h.cmd = cmd;
    h.arg = arg;
    h.wheel = wheel;
    c.v.hits.push_back(h);
}

bool Hot(const Ctx& c, int cmd, int arg) { return c.live && c.v.hotCmd == cmd && c.v.hotArg == arg; }
bool Down(const Ctx& c, int cmd, int arg) { return Hot(c, cmd, arg) && c.v.downCmd == cmd && c.v.downArg == arg; }

// ---------------------------------------------------------------- icons
enum Icon {
    IcPlay, IcPause, IcReset, IcFlag, IcPlus, IcPencil, IcBin, IcPin, IcClose, IcCheck, IcBell,
    IcClock, IcHourglass, IcStopwatch, IcUp, IcDown, IcChevronRight,
};

// A glyph in a box of half-size `r` around (cx, cy), in unit coordinates scaled up.
void DrawIcon(HDC dc, int icon, float cx, float cy, float r, COLORREF color, bool filled = false) {
    Gfx gf(dc);
    Gdiplus::Graphics& g = gf.g;
    g.TranslateTransform(cx, cy);
    g.ScaleTransform(r, r);
    Gdiplus::SolidBrush brush(Argb(color));
    const float w = (std::max)(0.13f, 1.7f / r);       // never thinner than ~1.7 px
    Gdiplus::Pen pen(Argb(color), w);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    using Gdiplus::PointF;
    switch (icon) {
        case IcPlay: {
            const PointF t[3] = { { -0.42f, -0.7f }, { 0.7f, 0.0f }, { -0.42f, 0.7f } };
            g.FillPolygon(&brush, t, 3);
            g.DrawPolygon(&pen, t, 3);
            break;
        }
        case IcPause:
            g.FillRectangle(&brush, -0.55f, -0.6f, 0.36f, 1.2f);
            g.FillRectangle(&brush, 0.19f, -0.6f, 0.36f, 1.2f);
            break;
        case IcReset: {
            g.ScaleTransform(-1.0f, 1.0f);      // anticlockwise
            const float R = 0.62f;
            g.DrawArc(&pen, -R, -R, 2 * R, 2 * R, 25.0f, 285.0f);
            const float a = -50.0f * 3.14159265f / 180.0f;
            const PointF e(R * std::cos(a), R * std::sin(a));
            const PointF t(-std::sin(a), std::cos(a)), n(-t.Y, t.X);
            const PointF tri[3] = { PointF(e.X + t.X * 0.5f, e.Y + t.Y * 0.5f),
                                    PointF(e.X - t.X * 0.12f + n.X * 0.42f, e.Y - t.Y * 0.12f + n.Y * 0.42f),
                                    PointF(e.X - t.X * 0.12f - n.X * 0.42f, e.Y - t.Y * 0.12f - n.Y * 0.42f) };
            g.FillPolygon(&brush, tri, 3);
            break;
        }
        case IcFlag: {
            g.DrawLine(&pen, -0.5f, -0.75f, -0.5f, 0.8f);
            const PointF t[4] = { { -0.5f, -0.7f }, { 0.65f, -0.45f }, { -0.5f, 0.05f }, { -0.5f, -0.7f } };
            g.FillPolygon(&brush, t, 3);
            break;
        }
        case IcPlus:
            g.DrawLine(&pen, -0.62f, 0.0f, 0.62f, 0.0f);
            g.DrawLine(&pen, 0.0f, -0.62f, 0.0f, 0.62f);
            break;
        case IcPencil: {
            g.RotateTransform(45.0f);
            const PointF body[5] = { { -0.17f, -0.62f }, { 0.17f, -0.62f }, { 0.17f, 0.3f }, { 0.0f, 0.66f }, { -0.17f, 0.3f } };
            g.DrawPolygon(&pen, body, 5);
            g.DrawLine(&pen, -0.17f, 0.3f, 0.17f, 0.3f);
            g.DrawLine(&pen, -0.17f, -0.4f, 0.17f, -0.4f);
            break;
        }
        case IcBin: {
            g.DrawLine(&pen, -0.6f, -0.4f, 0.6f, -0.4f);
            g.DrawLine(&pen, -0.2f, -0.4f, -0.2f, -0.62f);
            g.DrawLine(&pen, 0.2f, -0.4f, 0.2f, -0.62f);
            g.DrawLine(&pen, -0.2f, -0.62f, 0.2f, -0.62f);
            const PointF body[4] = { { -0.42f, -0.4f }, { -0.34f, 0.68f }, { 0.34f, 0.68f }, { 0.42f, -0.4f } };
            g.DrawPolygon(&pen, body, 4);
            g.DrawLine(&pen, -0.12f, -0.15f, -0.12f, 0.45f);
            g.DrawLine(&pen, 0.12f, -0.15f, 0.12f, 0.45f);
            break;
        }
        case IcPin: {
            g.RotateTransform(35.0f);
            const PointF head[4] = { { -0.22f, -0.78f }, { 0.22f, -0.78f }, { 0.28f, -0.1f }, { -0.28f, -0.1f } };
            if (filled) g.FillPolygon(&brush, head, 4);
            g.DrawPolygon(&pen, head, 4);
            g.DrawLine(&pen, -0.5f, -0.1f, 0.5f, -0.1f);
            g.DrawLine(&pen, 0.0f, -0.1f, 0.0f, 0.82f);
            break;
        }
        case IcClose:
            g.DrawLine(&pen, -0.5f, -0.5f, 0.5f, 0.5f);
            g.DrawLine(&pen, 0.5f, -0.5f, -0.5f, 0.5f);
            break;
        case IcCheck: {
            const PointF t[3] = { { -0.6f, 0.05f }, { -0.18f, 0.48f }, { 0.62f, -0.42f } };
            g.DrawLines(&pen, t, 3);
            break;
        }
        case IcBell: {
            Gdiplus::GraphicsPath p;
            p.AddArc(-0.5f, -0.72f, 1.0f, 1.0f, 180.0f, 180.0f);
            p.AddLine(0.5f, -0.22f, 0.5f, 0.14f);
            p.AddLine(0.5f, 0.14f, 0.72f, 0.4f);
            p.AddLine(0.72f, 0.4f, -0.72f, 0.4f);
            p.AddLine(-0.72f, 0.4f, -0.5f, 0.14f);
            p.CloseFigure();
            if (filled) g.FillPath(&brush, &p);
            g.DrawPath(&pen, &p);
            g.DrawArc(&pen, -0.2f, 0.42f, 0.4f, 0.36f, 0.0f, 180.0f);
            g.DrawLine(&pen, 0.0f, -0.72f, 0.0f, -0.84f);
            break;
        }
        case IcClock:
            g.DrawEllipse(&pen, -0.78f, -0.78f, 1.56f, 1.56f);
            g.DrawLine(&pen, 0.0f, 0.0f, 0.0f, -0.46f);
            g.DrawLine(&pen, 0.0f, 0.0f, 0.34f, 0.22f);
            break;
        case IcHourglass: {
            g.DrawLine(&pen, -0.55f, -0.74f, 0.55f, -0.74f);
            g.DrawLine(&pen, -0.55f, 0.74f, 0.55f, 0.74f);
            const PointF a[4] = { { -0.42f, -0.74f }, { 0.42f, -0.74f }, { 0.0f, 0.0f }, { -0.42f, -0.74f } };
            const PointF b[4] = { { 0.0f, 0.0f }, { 0.42f, 0.74f }, { -0.42f, 0.74f }, { 0.0f, 0.0f } };
            g.DrawLines(&pen, a, 4);
            g.DrawLines(&pen, b, 4);
            break;
        }
        case IcStopwatch:
            g.DrawEllipse(&pen, -0.66f, -0.52f, 1.32f, 1.32f);
            g.DrawLine(&pen, 0.0f, -0.52f, 0.0f, -0.8f);
            g.DrawLine(&pen, -0.2f, -0.8f, 0.2f, -0.8f);
            g.DrawLine(&pen, 0.0f, 0.14f, 0.3f, -0.2f);
            break;
        case IcUp: {
            const PointF t[3] = { { -0.55f, 0.28f }, { 0.0f, -0.28f }, { 0.55f, 0.28f } };
            g.DrawLines(&pen, t, 3);
            break;
        }
        case IcDown: {
            const PointF t[3] = { { -0.55f, -0.28f }, { 0.0f, 0.28f }, { 0.55f, -0.28f } };
            g.DrawLines(&pen, t, 3);
            break;
        }
        case IcChevronRight: {
            const PointF t[3] = { { -0.28f, -0.55f }, { 0.28f, 0.0f }, { -0.28f, 0.55f } };
            g.DrawLines(&pen, t, 3);
            break;
        }
    }
}

// ---------------------------------------------------------------- widgets
// A round button with a glyph. `primary` is the metal one; the others sit on the plate.
void RoundBtn(Ctx& c, int cx, int cy, int dia, int cmd, int arg, int icon, bool primary,
              bool enabled = true, bool filled = false, float glyph = 0.40f) {
    RECT r = { cx - dia / 2, cy - dia / 2, cx - dia / 2 + dia, cy - dia / 2 + dia };
    if (enabled) AddHit(c, r, cmd, arg);
    const bool hot = enabled && Hot(c, cmd, arg), down = enabled && Down(c, cmd, arg);
    COLORREF top, bottom, edge, ink;
    if (!enabled) {
        top = theme::Plate; bottom = theme::PlateLow; edge = theme::Line; ink = theme::TextMute;
    } else if (primary) {
        top = down ? theme::Metal3 : (hot ? theme::Mix(theme::Metal0, theme::TextHi, 0.12f) : theme::Metal0);
        bottom = down ? theme::Metal2 : (hot ? theme::Mix(theme::Metal2, theme::TextHi, 0.10f) : theme::Metal2);
        edge = hot ? theme::TextHi : theme::MetalEdge;
        ink = theme::TextHi;
    } else {
        top = down ? theme::Bg : (hot ? theme::Metal1 : theme::Plate);
        bottom = down ? theme::PlateLow : (hot ? theme::Metal3 : theme::PlateLow);
        edge = hot ? theme::MetalEdge : theme::Rule;
        ink = hot ? theme::TextHi : theme::Text;
    }
    Gfx gf(c.dc);
    {
        Gdiplus::LinearGradientBrush b(Gdiplus::PointF(0, (float)r.top - 1), Gdiplus::PointF(0, (float)r.bottom + 1),
                                       Argb(top), Argb(bottom));
        gf.g.FillEllipse(&b, (float)r.left, (float)r.top, (float)dia, (float)dia);
        Gdiplus::Pen pen(Argb(edge), (float)Hair());
        gf.g.DrawEllipse(&pen, r.left + 0.5f, r.top + 0.5f, dia - 1.0f, dia - 1.0f);
    }
    DrawIcon(c.dc, icon, cx + (down ? 0.5f : 0.0f), cy + (down ? 1.0f : 0.0f) + (icon == IcPlay ? 0.0f : 0.0f),
             dia * glyph, ink, filled);
}

// A rounded-square icon button (the corner buttons, a card's edit / delete).
void SquareBtn(Ctx& c, RECT r, int cmd, int arg, int icon, bool lit = false, bool dangerHot = false,
               float glyph = 0.30f) {
    AddHit(c, r, cmd, arg);
    const bool hot = Hot(c, cmd, arg), down = Down(c, cmd, arg);
    if (hot || lit) {
        COLORREF fill = dangerHot && hot ? theme::Mix(theme::Bg, theme::Danger, down ? 0.85f : 0.65f)
                                         : (down ? theme::Metal3 : (lit && !hot ? theme::Metal2 : theme::Metal1));
        FillRound(c.dc, r, S(6), fill);
        if (lit || hot) StrokeRound(c.dc, r, S(6), (lit ? theme::MetalEdge : theme::Rule), 255, (float)Hair());
    }
    const float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f;
    DrawIcon(c.dc, icon, cx, cy + (down ? 1.0f : 0.0f), (float)(r.right - r.left) * glyph,
             hot || lit ? theme::TextHi : theme::TextDim, lit);
}

// A text button: primary = metal, otherwise the plate; danger turns the text red.
void Button(Ctx& c, RECT r, const std::wstring& text, bool primary, int cmd, int arg = 0, bool danger = false) {
    AddHit(c, r, cmd, arg);
    const bool hot = Hot(c, cmd, arg), down = Down(c, cmd, arg);
    const int rad = S(8);
    if (primary) {
        GradRound(c.dc, r, rad, down ? theme::Metal3 : (hot ? theme::Mix(theme::Metal0, theme::TextHi, 0.12f) : theme::Metal0),
                  down ? theme::Metal2 : theme::Metal2);
        StrokeRound(c.dc, r, rad, hot ? theme::TextHi : theme::MetalEdge, 255, (float)Hair());
    } else {
        GradRound(c.dc, r, rad, down ? theme::Bg : (hot ? theme::Metal1 : theme::Plate),
                  down ? theme::PlateLow : (hot ? theme::Metal3 : theme::PlateLow));
        StrokeRound(c.dc, r, rad, hot ? theme::MetalEdge : theme::Rule, 255, (float)Hair());
    }
    RECT t = r;
    if (down) OffsetRect(&t, 0, 1);
    const COLORREF ink = danger ? theme::Mix(theme::Danger, theme::TextHi, hot ? 0.35f : 0.0f)
                                : (primary || hot ? theme::TextHi : theme::Text);
    Txt(c.dc, Fnt(15, 1), text, t, ink, DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
}

// A selectable chip (a weekday, a month, "Today"). Returns nothing; the caller handles the click.
void Chip(Ctx& c, RECT r, const std::wstring& text, bool on, int cmd, int arg, int fontDip = 15) {
    AddHit(c, r, cmd, arg);
    const bool hot = Hot(c, cmd, arg), down = Down(c, cmd, arg);
    const int rad = S(7);
    if (on) {
        GradRound(c.dc, r, rad, down ? theme::Metal3 : theme::Metal1, down ? theme::PlateLow : theme::Metal3);
        StrokeRound(c.dc, r, rad, hot ? theme::TextHi : theme::MetalEdge, 255, (float)Hair());
    } else {
        FillRound(c.dc, r, rad, down ? theme::Bg : (hot ? theme::Metal2 : theme::PlateLow));
        StrokeRound(c.dc, r, rad, hot ? theme::Rule : theme::Line, 255, (float)Hair());
    }
    RECT t = r;
    if (down) OffsetRect(&t, 0, 1);
    Txt(c.dc, Fnt(fontDip, 1), text, t, on || hot ? theme::TextHi : theme::TextDim, DT_CENTER | DT_VCENTER);
}

// The on / off switch.
void Switch(Ctx& c, RECT r, bool on, int cmd, int arg, bool dim = false) {
    AddHit(c, r, cmd, arg);
    const bool hot = Hot(c, cmd, arg);
    const int h = r.bottom - r.top;
    if (on) {
        FillRound(c.dc, r, h / 2, dim ? theme::SegOff : (hot ? theme::TextHi : theme::SegOn));
    } else {
        FillRound(c.dc, r, h / 2, hot ? theme::Metal2 : theme::PlateLow);
        StrokeRound(c.dc, r, h / 2, hot ? theme::TextDim : theme::Rule, 255, (float)Hair() * 1.5f);
    }
    const int knob = h - S(8);
    const int kx = on ? r.right - S(4) - knob : r.left + S(4);
    Gfx gf(c.dc);
    Gdiplus::SolidBrush b(Argb(on ? theme::Bg : (hot ? theme::TextHi : theme::TextDim)));
    gf.g.FillEllipse(&b, (float)kx, (float)(r.top + S(4)), (float)knob, (float)knob);
}

// ---------------------------------------------------------------- time and date text
struct Civil { int y, mo, d, h, mi, s; };

Civil LocalNow(Ticks now) {
    const alarm::LocalTime l = alarm::SystemZone().ToLocal(now);
    return { l.year, l.month, l.day, l.hour, l.minute, (int)((now / kSecond) % 60) };
}

// "07:00" or "7:00"; the AM / PM goes to *suffix for a 12-hour clock.
std::wstring Hm(bool h24, int h, int mi, std::wstring* suffix) {
    wchar_t buf[24];
    if (h24) {
        swprintf_s(buf, L"%02d:%02d", h, mi);
        if (suffix) suffix->clear();
    } else {
        const int h12 = h % 12 == 0 ? 12 : h % 12;
        swprintf_s(buf, L"%d:%02d", h12, mi);
        if (suffix) *suffix = h >= 12 ? L"PM" : L"AM";
    }
    return buf;
}

const wchar_t* const kDayShort[7] = { L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun" };
const wchar_t* const kMonShort[12] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                       L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };

// "Today 07:00", "Tomorrow 07:00", "Thu 9 Oct 07:00".
std::wstring When(const Model& m, Ticks wake) {
    const alarm::LocalTime w = alarm::SystemZone().ToLocal(wake);
    const alarm::LocalTime n = alarm::SystemZone().ToLocal(m.now);
    const Ticks dw = alarm::FromCivil(w.year, w.month, w.day, 0, 0);
    const Ticks dn = alarm::FromCivil(n.year, n.month, n.day, 0, 0);
    const long long diff = (long long)((dw - dn) / kDay);
    std::wstring sfx;
    std::wstring t = Hm(m.hours24, w.hour, w.minute, &sfx);
    if (!sfx.empty()) t += L" " + sfx;
    if (diff == 0) return L"Today " + t;
    if (diff == 1) return L"Tomorrow " + t;
    wchar_t buf[48];
    swprintf_s(buf, L"%s %d %s ", kDayShort[alarm::Weekday(w.year, w.month, w.day)], w.day, kMonShort[w.month - 1]);
    return buf + t;
}

// ---------------------------------------------------------------- the rail and the corner
const wchar_t* const kPageName[PageCount] = { L"Clock", L"Alarm", L"Timer", L"Stopwatch" };
const int kPageIcon[PageCount] = { IcClock, IcBell, IcHourglass, IcStopwatch };

void PaintRail(Ctx& c) {
    RECT rail = { 0, 0, S(kRail), S(kHeight) };
    theme::Wash(c.dc, rail, RGB(9, 9, 9), 255);
    RECT edge = { S(kRail) - Hair(), 0, S(kRail), S(kHeight) };
    theme::Wash(c.dc, edge, theme::Line, 255);

    // The mark and the name, which is also where the window is carried from.
    RECT mark = { S(22), S(14), S(22) + S(18), S(14) + S(14) };
    theme::Mark(c.dc, mark, theme::TextDim, theme::TextHi);
    const std::wstring brand = theme::Caps(L"ProWindows");
    RECT br = { S(48), S(10), S(kRail) - S(8), S(32) };
    theme::Print(c.dc, Font::Small, brand, br, theme::TextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(2));

    for (int i = 0; i < PageCount; ++i) {
        RECT r = { S(10), S(64) + i * S(48), S(kRail) - S(10), S(64) + i * S(48) + S(42) };
        const bool sel = c.v.page == i;
        AddHit(c, r, CmdPage, i);
        const bool hot = Hot(c, CmdPage, i), down = Down(c, CmdPage, i);
        if (sel) {
            GradRound(c.dc, r, S(8), theme::Metal1, theme::Metal3);
            StrokeRound(c.dc, r, S(8), theme::MetalEdge, 255, (float)Hair());
        } else if (hot) {
            FillRound(c.dc, r, S(8), down ? theme::Plate : theme::Metal2);
        }
        const COLORREF ink = sel ? theme::TextHi : (hot ? theme::Text : theme::TextDim);
        DrawIcon(c.dc, kPageIcon[i], (float)(r.left + S(26)), (float)((r.top + r.bottom) / 2), (float)S(9), ink);
        const std::wstring name = theme::Caps(kPageName[i]);
        RECT t = { r.left + S(50), r.top, r.right - S(6), r.bottom };
        theme::Print(c.dc, Font::Tab, name, t, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(2));
    }
}

void PaintCorner(Ctx& c) {
    const int sz = S(32), gap = S(4), y = S(8);
    RECT close = { S(kWidth) - S(12) - sz, y, S(kWidth) - S(12), y + sz };
    RECT pin = { close.left - gap - sz, y, close.left - gap, y + sz };
    SquareBtn(c, pin, CmdPin, 0, IcPin, c.v.pinned, false, 0.30f);
    SquareBtn(c, close, CmdClose, 0, IcClose, false, true, 0.28f);
}

void Title(Ctx& c, const wchar_t* text) {
    const std::wstring caps = theme::Caps(text);
    RECT r = { S(kRail + 32), S(46), S(kWidth - 32), S(86) };
    theme::Print(c.dc, Font::Heading, caps, r, theme::TextHi, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(3));
}

// ---------------------------------------------------------------- the ringing banner
constexpr int kBannerH = 88;

void PaintBanner(Ctx& c, int top) {
    const View& v = c.v;
    RECT r = { S(kRail + 32), top, S(kWidth - 32), top + S(kBannerH) };
    FillRound(c.dc, r, S(10), theme::Mix(theme::Bg, theme::Warn, 0.12f));
    StrokeRound(c.dc, r, S(10), theme::Warn, 200, (float)Hair() * 1.5f);
    const int cy = (r.top + r.bottom) / 2;
    // The bell in a ring of amber.
    const int d = S(44);
    Gfx gf(c.dc);
    {
        Gdiplus::SolidBrush b(Argb(theme::Warn));
        gf.g.FillEllipse(&b, (float)(r.left + S(18)), (float)(cy - d / 2), (float)d, (float)d);
    }
    DrawIcon(c.dc, IcBell, (float)(r.left + S(18) + d / 2), (float)cy + 1, (float)(d * 0.30f), theme::Bg, true);

    std::wstring sfx;
    std::wstring when = Hm(c.m.hours24, v.ring.hour, v.ring.minute, &sfx);
    if (!sfx.empty()) when += L" " + sfx;
    RECT nm = { r.left + S(78), cy - S(28), r.left + S(78) + S(210), cy + S(2) };
    Txt(c.dc, Fnt(22, 1), v.ring.name.empty() ? std::wstring(L"Alarm") : v.ring.name, nm, theme::TextHi,
        DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    RECT tm = { nm.left, cy + S(2), nm.right, cy + S(28) };
    Txt(c.dc, Fnt(15, 1), when, tm, theme::Warn, DT_LEFT | DT_VCENTER);

    RECT dismiss = { r.right - S(16) - S(104), cy - S(21), r.right - S(16), cy + S(21) };
    RECT snooze = { dismiss.left - S(10) - S(168), cy - S(21), dismiss.left - S(10), cy + S(21) };
    Button(c, snooze, L"Snooze (" + std::to_wstring(v.ring.snoozeMin) + L" min)", true, CmdRingSnooze);
    Button(c, dismiss, L"Dismiss", false, CmdRingDismiss);
}

// ---------------------------------------------------------------- cards
void Card(Ctx& c, RECT r, bool hot, bool selected = false, bool dim = false) {
    FillRound(c.dc, r, S(10), hot ? theme::Mix(theme::PlateLow, theme::Metal1, 0.5f)
                                  : (dim ? RGB(14, 14, 14) : RGB(18, 18, 18)));
    StrokeRound(c.dc, r, S(10), selected ? theme::MetalEdge : (hot ? theme::Rule : theme::Line), 255, (float)Hair());
}

// A thin scrollbar at the right edge of a scrolling area.
void ScrollBar(Ctx& c, int top, int bottom, int pos, int maxPos) {
    if (maxPos <= 0) return;
    const int track = bottom - top - S(8);
    const int total = track + maxPos;
    int thumb = (std::max)(S(28), (int)((long long)track * track / total));
    const int ty = top + S(4) + (int)((long long)(track - thumb) * pos / maxPos);
    RECT r = { S(kWidth) - S(10), ty, S(kWidth) - S(6), ty + thumb };
    FillRound(c.dc, r, S(2), theme::Rule);
}

// ---------------------------------------------------------------- Clock page
void PaintClockPage(Ctx& c, int y0) {
    const Model& m = c.m;
    Title(c, L"Clock");
    const int left = S(kRail + 32), right = S(kWidth - 32), mid = (left + right) / 2;
    const int bottom = S(kHeight - 20);
    const int blockH = S(104 + 34 + 24 + 150);
    int y = y0 + (std::max)(0, (bottom - y0 - blockH) / 3);

    const Civil n = LocalNow(m.now);
    std::wstring sfx;
    std::wstring t = Hm(m.hours24, n.h, n.mi, &sfx);
    wchar_t sec[8] = L"";
    if (m.seconds) swprintf_s(sec, L":%02d", n.s);
    t += sec;
    HFONT big = Fnt(96, 0);
    HFONT ampmFont = Fnt(22, 1);
    HDC dc = c.dc;
    HGDIOBJ old = SelectObject(dc, big);
    const int w = CellsWidth(dc, t);
    SelectObject(dc, old);
    const int ampmW = sfx.empty() ? 0 : TxtWidth(dc, ampmFont, sfx) + S(12);
    const int x0 = mid - (w + ampmW) / 2;
    Cells(dc, big, t, x0, y, y + S(104), -1, theme::TextHi);
    if (!sfx.empty()) {
        RECT ar = { x0 + w + S(12), y, x0 + w + ampmW + S(40), y + S(88) };
        Txt(dc, ampmFont, sfx, ar, theme::TextDim, DT_LEFT | DT_BOTTOM);
    }
    y += S(104);

    wchar_t date[96] = L"";
    SYSTEMTIME st = {};
    st.wYear = (WORD)n.y; st.wMonth = (WORD)n.mo; st.wDay = (WORD)n.d;
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &st, nullptr, date, 96);
    RECT dr = { left, y, right, y + S(34) };
    Txt(dc, Fnt(20, 1), date, dr, theme::TextDim, DT_CENTER | DT_VCENTER);
    y += S(34) + S(24);

    // ---- the two cards
    const int cardH = (std::min)(S(150), bottom - y);
    const int gap = S(16);
    const int cw = (right - left - gap) / 2;
    RECT next = { left, y, left + cw, y + cardH };
    RECT run = { left + cw + gap, y, right, y + cardH };

    AddHit(c, next, CmdCard, PageAlarm);
    Card(c, next, Hot(c, CmdCard, PageAlarm));
    Label(c, L"Next alarm", next.left + S(18), next.top + S(14));
    DrawIcon(dc, IcBell, (float)(next.right - S(26)), (float)(next.top + S(24)), (float)S(8), theme::TextMute);
    if (m.nextAlarm >= 0 && m.nextAlarm < (int)m.s->alarms.size() && m.wake[(size_t)m.nextAlarm]) {
        const alarm::Alarm& a = m.s->alarms[(size_t)m.nextAlarm];
        const Ticks wake = m.wake[(size_t)m.nextAlarm];
        RECT nm = { next.left + S(18), next.top + S(42), next.right - S(18), next.top + S(74) };
        Txt(dc, Fnt(22, 1), a.label.empty() ? std::wstring(L"Alarm") : a.label, nm, theme::TextHi, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        RECT wr = { nm.left, next.top + S(78), nm.right, next.top + S(102) };
        Txt(dc, Fnt(15, 1), When(m, wake), wr, theme::Text, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        RECT ir = { nm.left, next.top + S(102), nm.right, next.top + S(126) };
        Txt(dc, Fnt(15, 1), FormatIn(wake - m.now), ir, theme::TextDim, DT_LEFT | DT_VCENTER);
    } else {
        RECT nm = { next.left + S(18), next.top + S(46), next.right - S(18), next.top + S(110) };
        Txt(dc, Fnt(15, 1), L"No alarm is set.", nm, theme::TextDim, DT_LEFT | DT_TOP);
        RECT hr = { nm.left, next.top + S(76), nm.right, next.top + S(104) };
        Txt(dc, Fnt(15, 1), L"Click to add one.", hr, theme::TextMute, DT_LEFT | DT_TOP);
    }

    // The running card sends you to the page of its first line.
    const timer::State& s = *m.s;
    int target = PageTimer;
    bool anyTimer = false;
    for (const timer::Timer& tm : s.timers) if (tm.running) { anyTimer = true; break; }
    const bool watchOn = s.watch.running || timer::Elapsed(s.watch, m.now) > 0;
    if (!anyTimer && watchOn) target = PageWatch;
    AddHit(c, run, CmdCard, 10 + target);
    Card(c, run, Hot(c, CmdCard, 10 + target));
    Label(c, L"Running", run.left + S(18), run.top + S(14));
    DrawIcon(dc, IcHourglass, (float)(run.right - S(26)), (float)(run.top + S(24)), (float)S(8), theme::TextMute);
    int row = 0;
    HFONT cellFont = Fnt(18, 0);
    auto line = [&](const std::wstring& name, const std::wstring& reading) {
        if (row >= 3) return;
        const int ry = run.top + S(42) + row * S(30);
        RECT nr = { run.left + S(18), ry, run.right - S(124), ry + S(28) };
        Txt(dc, Fnt(15, 1), name, nr, theme::Text, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        Cells(dc, cellFont, reading, run.right - S(18), ry, ry + S(28), 1, theme::TextHi);
        ++row;
    };
    for (const timer::Timer& tm : s.timers)
        if (tm.running) line(tm.label, timer::FormatDuration(timer::CeilSecond(timer::Remaining(tm, m.now))));
    if (watchOn) line(L"Stopwatch", timer::FormatDuration(timer::Elapsed(s.watch, m.now)));
    if (row == 0) {
        RECT nr = { run.left + S(18), run.top + S(46), run.right - S(18), run.top + S(80) };
        Txt(dc, Fnt(15, 1), L"Nothing is running.", nr, theme::TextDim, DT_LEFT | DT_TOP);
    }
}

// ---------------------------------------------------------------- Alarm page
constexpr int kAlarmCardH = 88;
constexpr int kAlarmGap = 10;

void AlarmCard(Ctx& c, int i, RECT r) {
    const Model& m = c.m;
    const alarm::Alarm& a = m.s->alarms[(size_t)i];
    AddHit(c, r, CmdAlarmEdit, i);
    const bool over = Hot(c, CmdAlarmEdit, i) || Hot(c, CmdAlarmDelete, i) || Hot(c, CmdAlarmToggle, i);
    Card(c, r, over, false, !a.enabled);
    const COLORREF hi = a.enabled ? theme::TextHi : theme::TextMute;
    const COLORREF mid = a.enabled ? theme::Text : theme::TextMute;
    const COLORREF lo = a.enabled ? theme::TextDim : theme::TextMute;

    std::wstring sfx;
    // An hourly alarm has no hour of its own: it shows the minute it rings at.
    std::wstring t = Hm(m.hours24, a.hour, a.minute, &sfx);
    if (a.kind == alarm::Kind::Hourly) {
        wchar_t mm[8];
        swprintf_s(mm, L":%02d", a.minute);
        t = mm;
        sfx.clear();
    }
    HFONT big = Fnt(40, 0);
    HDC dc = c.dc;
    HGDIOBJ old = SelectObject(dc, big);
    const int tw = CellsWidth(dc, t);
    SelectObject(dc, old);
    const int cy = (r.top + r.bottom) / 2;
    Cells(dc, big, t, r.left + S(22), cy - S(30), cy + S(30), -1, hi);
    if (!sfx.empty()) {
        RECT ar = { r.left + S(22) + tw + S(6), cy - S(30), r.left + S(22) + tw + S(50), cy + S(22) };
        Txt(dc, Fnt(15, 1), sfx, ar, lo, DT_LEFT | DT_BOTTOM);
    }

    const int tx = r.left + S(196);
    const int textR = r.right - S(130);
    const bool isNext = m.nextAlarm == i && a.enabled && m.wake[(size_t)i];
    RECT nm = { tx, r.top + S(12), textR, r.top + S(38) };
    Txt(dc, Fnt(18, 1), a.label.empty() ? std::wstring(L"Alarm") : a.label, nm, mid, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    RECT sm = { tx, r.top + S(38), textR, r.top + S(60) };
    Txt(dc, Fnt(14, 1), alarm::RepeatSummary(a), sm, lo, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    if (isNext) {
        RECT rr = { tx, r.top + S(60), textR, r.top + S(80) };
        Txt(dc, Fnt(14, 1), L"Rings " + FormatIn(m.wake[(size_t)i] - m.now), rr, theme::TextHi, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    } else if (a.enabled && a.snoozeUntilUtc > m.now) {
        RECT rr = { tx, r.top + S(60), textR, r.top + S(80) };
        Txt(dc, Fnt(14, 1), L"Snoozed until " + When(m, a.snoozeUntilUtc), rr, theme::Warn, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }

    RECT sw = { r.right - S(22) - S(46), cy - S(12), r.right - S(22), cy + S(12) };
    Switch(c, sw, a.enabled, CmdAlarmToggle, i);
    if (over) {
        RECT del = { sw.left - S(12) - S(34), cy - S(17), sw.left - S(12), cy + S(17) };
        SquareBtn(c, del, CmdAlarmDelete, i, IcBin, false, true, 0.32f);
    }
}

void PaintAlarmPage(Ctx& c, int y0) {
    Title(c, L"Alarm");
    const Model& m = c.m;
    View& v = c.v;
    const int n = (int)m.s->alarms.size();
    const int top = y0, bottom = S(kHeight) - Hair();
    const int viewH = bottom - top;
    const int contentH = n ? n * S(kAlarmCardH + kAlarmGap) + S(80) : 0;
    v.scrollMax = (std::max)(0, contentH - viewH);
    if (v.scroll[PageAlarm] > v.scrollMax) v.scroll[PageAlarm] = v.scrollMax;
    if (v.scroll[PageAlarm] < 0) v.scroll[PageAlarm] = 0;
    v.scrollRect = { S(kRail), top, S(kWidth), bottom };

    const int left = S(kRail + 32), right = S(kWidth - 32);
    const int saved = SaveDC(c.dc);
    IntersectClipRect(c.dc, S(kRail) + Hair(), top, S(kWidth) - Hair(), bottom);
    const RECT keepClip = c.clip;
    c.clip = { S(kRail), top, S(kWidth), bottom };
    if (n == 0) {
        const int cx = (left + right) / 2, cy = top + (bottom - top) / 2 - S(30);
        DrawIcon(c.dc, IcBell, (float)cx, (float)cy - S(30), (float)S(26), theme::TextMute);
        RECT t = { left, cy + S(10), right, cy + S(38) };
        Txt(c.dc, Fnt(18, 1), L"No alarms yet", t, theme::TextDim, DT_CENTER | DT_VCENTER);
        RECT b = { cx - S(80), cy + S(52), cx + S(80), cy + S(94) };
        Button(c, b, L"New alarm", true, CmdAlarmAdd);
    }
    for (int i = 0; i < n; ++i) {
        const int ry = top + i * S(kAlarmCardH + kAlarmGap) - v.scroll[PageAlarm];
        if (ry + S(kAlarmCardH) < top || ry > bottom) continue;
        RECT r = { left, ry, right, ry + S(kAlarmCardH) };
        AlarmCard(c, i, r);
    }
    c.clip = keepClip;
    RestoreDC(c.dc, saved);
    ScrollBar(c, top, bottom, v.scroll[PageAlarm], v.scrollMax);
    // In the title row: a button over the list would sit on the last card's switch.
    if (n < alarm::kMaxAlarms)
        RoundBtn(c, S(kWidth - 32 - 20), S(66), S(40), CmdAlarmAdd, 0, IcPlus, true, true, false, 0.36f);
}

// ---------------------------------------------------------------- Timer page
void Ring(HDC dc, int cx, int cy, int radius, int thick, double frac, COLORREF track, COLORREF ink) {
    Gfx gf(dc);
    const float rr = (float)radius - thick / 2.0f;
    Gdiplus::Pen tp(Argb(track), (float)thick);
    gf.g.DrawEllipse(&tp, cx - rr, cy - rr, rr * 2, rr * 2);
    if (frac <= 0.002) return;
    Gdiplus::Pen pen(Argb(ink), (float)thick);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    if (frac >= 0.999) gf.g.DrawEllipse(&pen, cx - rr, cy - rr, rr * 2, rr * 2);
    else gf.g.DrawArc(&pen, cx - rr, cy - rr, rr * 2, rr * 2, -90.0f, (float)(360.0 * frac));
}

constexpr int kCardW = 200, kCardH = 238, kCardGap = 10;

void TimerCard(Ctx& c, int i, RECT r) {
    const Model& m = c.m;
    const timer::Timer& t = m.s->timers[(size_t)i];
    const bool sel = c.v.sel == i;
    const bool over = Hot(c, CmdTimerSel, i) || Hot(c, CmdTimerPlay, i) || Hot(c, CmdTimerReset, i) ||
                      Hot(c, CmdTimerEdit, i) || Hot(c, CmdTimerDelete, i);
    AddHit(c, r, CmdTimerSel, i);
    Card(c, r, over, sel);
    HDC dc = c.dc;

    RECT nm = { r.left + S(16), r.top + S(12), r.right - S(70), r.top + S(36) };
    Txt(dc, Fnt(15, 1), t.label, nm, theme::Text, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    if (over) {
        const int sz = S(26);
        RECT del = { r.right - S(10) - sz, r.top + S(11), r.right - S(10), r.top + S(11) + sz };
        RECT ed = { del.left - S(4) - sz, del.top, del.left - S(4), del.bottom };
        SquareBtn(c, ed, CmdTimerEdit, i, IcPencil, false, false, 0.34f);
        SquareBtn(c, del, CmdTimerDelete, i, IcBin, false, true, 0.34f);
    }

    const Ticks left = timer::Remaining(t, m.now);
    const bool done = !t.running && t.remaining <= 0;
    const double frac = done ? 1.0 : (t.duration > 0 ? (double)left / (double)t.duration : 0.0);
    const COLORREF ink = done ? theme::Warn : (t.running ? theme::TextHi : theme::TextDim);
    const int cx = (r.left + r.right) / 2, cy = r.top + S(44) + S(60);
    const int rad = S(62);
    Ring(dc, cx, cy, rad, S(7), frac, done ? theme::Mix(theme::Bg, theme::Warn, 0.25f) : RGB(38, 38, 38), ink);

    // The reading, as large as fits inside the ring.
    const std::wstring txt = timer::FormatDuration(timer::CeilSecond(left));
    int dip = 26;
    HFONT f = Fnt(dip, 0);
    const int room = rad * 2 - S(34);
    for (;;) {
        f = Fnt(dip, 0);
        HGDIOBJ old = SelectObject(dc, f);
        const int w = CellsWidth(dc, txt);
        SelectObject(dc, old);
        if (w <= room || dip <= 11) break;
        dip -= 2;
    }
    Cells(dc, f, txt, cx, cy - S(22), cy + S(6), 0, done ? theme::Warn : theme::TextHi);
    RECT sr = { cx - rad + S(10), cy + S(8), cx + rad - S(10), cy + S(28) };
    const wchar_t* status = done ? L"Finished" : (t.running ? L"Running" : (t.remaining < t.duration ? L"Paused" : L"Ready"));
    Txt(dc, Fnt(13, 1), status, sr, done ? theme::Warn : theme::TextDim, DT_CENTER | DT_VCENTER);

    const int by = r.bottom - S(16) - S(21);
    RoundBtn(c, cx - S(29), by, S(42), CmdTimerPlay, i, t.running ? IcPause : IcPlay, true, true, false, 0.36f);
    RoundBtn(c, cx + S(29), by, S(42), CmdTimerReset, i, IcReset, false, t.running || t.remaining != t.duration, false, 0.36f);
}

void PaintTimerPage(Ctx& c, int y0) {
    Title(c, L"Timer");
    const Model& m = c.m;
    View& v = c.v;
    const int left = S(kRail + 32), right = S(kWidth - 32);
    const int n = (int)m.s->timers.size();

    // ---- quick start chips
    const int chipY = y0;
    Label(c, L"Quick start", left, chipY + S(8));
    static const int kMins[7] = { 1, 3, 5, 10, 15, 30, 60 };
    int x = left + S(112);
    for (int k = 0; k < 7; ++k) {
        const std::wstring txt = kMins[k] == 60 ? std::wstring(L"1 h") : std::to_wstring(kMins[k]) + L" min";
        const int w = kMins[k] == 60 ? S(48) : (kMins[k] < 10 ? S(56) : S(64));
        RECT r = { x, chipY, x + w, chipY + S(34) };
        Chip(c, r, txt, false, CmdQuick, kMins[k], 14);
        x += w + S(8);
    }

    // ---- the grid
    const int top = y0 + S(48), bottom = S(kHeight) - Hair();
    const int viewH = bottom - top;
    const int rows = (n + 2) / 3;
    const int contentH = rows ? rows * S(kCardH + kCardGap) + S(70) : 0;
    v.scrollMax = (std::max)(0, contentH - viewH);
    if (v.scroll[PageTimer] > v.scrollMax) v.scroll[PageTimer] = v.scrollMax;
    if (v.scroll[PageTimer] < 0) v.scroll[PageTimer] = 0;
    v.scrollRect = { S(kRail), top, S(kWidth), bottom };

    const int saved = SaveDC(c.dc);
    IntersectClipRect(c.dc, S(kRail) + Hair(), top, S(kWidth) - Hair(), bottom);
    const RECT keepClip = c.clip;
    c.clip = { S(kRail), top, S(kWidth), bottom };
    if (n == 0) {
        RECT t = { left, top + S(60), right, top + S(100) };
        Txt(c.dc, Fnt(18, 1), L"No timers yet", t, theme::TextDim, DT_CENTER | DT_VCENTER);
        RECT t2 = { left, top + S(100), right, top + S(130) };
        Txt(c.dc, Fnt(14, 1), L"Pick a length above, or use the + button.", t2, theme::TextMute, DT_CENTER | DT_VCENTER);
    }
    for (int i = 0; i < n; ++i) {
        const int col = i % 3, row = i / 3;
        const int cx = left + col * S(kCardW + kCardGap);
        const int cy = top + row * S(kCardH + kCardGap) - v.scroll[PageTimer];
        if (cy + S(kCardH) < top || cy > bottom) continue;
        TimerCard(c, i, { cx, cy, cx + S(kCardW), cy + S(kCardH) });
    }
    c.clip = keepClip;
    RestoreDC(c.dc, saved);
    ScrollBar(c, top, bottom, v.scroll[PageTimer], v.scrollMax);
    if (n < timer::kMaxTimers)
        RoundBtn(c, S(kWidth - 32 - 26), S(kHeight - 30 - 26), S(52), CmdTimerAdd, 0, IcPlus, true, true, false, 0.36f);
}

// ---------------------------------------------------------------- Stopwatch page
void PaintWatchPage(Ctx& c, int y0) {
    Title(c, L"Stopwatch");
    const Model& m = c.m;
    View& v = c.v;
    const timer::Stopwatch& w = m.s->watch;
    const int left = S(kRail + 32), right = S(kWidth - 32), mid = (left + right) / 2;
    HDC dc = c.dc;

    const Ticks el = timer::Elapsed(w, m.now);
    Cells(dc, Fnt(84, 0), timer::FormatDuration(el, true), mid, y0, y0 + S(104), 0,
          w.running ? theme::TextHi : (el > 0 ? theme::Text : theme::TextDim));
    const int by = y0 + S(104) + S(46);
    RoundBtn(c, mid - S(96), by, S(52), CmdWatchLap, 0, IcFlag, false, w.running || el > 0, false, 0.36f);
    RoundBtn(c, mid, by, S(72), CmdWatchPlay, 0, w.running ? IcPause : IcPlay, true, true, false, 0.36f);
    RoundBtn(c, mid + S(96), by, S(52), CmdWatchReset, 0, IcReset, false, el > 0, false, 0.38f);

    // ---- laps
    const int hy = by + S(52);
    const int c1 = left + S(18), c2 = left + S(300), c3 = right - S(18);
    Label(c, L"Lap", c1, hy);
    {
        const std::wstring cap = theme::Caps(L"Time");
        RECT r = { c2 - S(120), hy, c2, hy + S(18) };
        theme::Print(dc, Font::Small, cap, r, theme::TextDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, S(2));
        const std::wstring cap2 = theme::Caps(L"Total");
        RECT r2 = { c3 - S(140), hy, c3, hy + S(18) };
        theme::Print(dc, Font::Small, cap2, r2, theme::TextDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, S(2));
    }
    const int top = hy + S(24), bottom = S(kHeight) - Hair();
    RECT hair = { left, top - Hair(), right, top };
    theme::Wash(dc, hair, theme::Line, 255);

    const int n = (int)w.laps.size();
    const int rowH = S(34);
    v.scrollMax = (std::max)(0, n * rowH + S(8) - (bottom - top));
    if (v.scroll[PageWatch] > v.scrollMax) v.scroll[PageWatch] = v.scrollMax;
    v.scrollRect = { S(kRail), top, S(kWidth), bottom };
    if (n == 0) {
        RECT t = { left, top + S(14), right, top + S(46) };
        Txt(dc, Fnt(14, 1), L"Laps appear here.", t, theme::TextMute, DT_CENTER | DT_VCENTER);
        return;
    }
    // Fastest and slowest split, once there are two to compare.
    int fast = -1, slow = -1;
    Ticks fastV = 0, slowV = 0;
    for (int i = 0; i < n; ++i) {
        const Ticks prev = (i + 1 < n) ? w.laps[(size_t)i + 1] : 0;
        const Ticks split = w.laps[(size_t)i] - prev;
        if (fast < 0 || split < fastV) { fast = i; fastV = split; }
        if (slow < 0 || split > slowV) { slow = i; slowV = split; }
    }
    if (n < 2 || fastV == slowV) fast = slow = -1;

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, S(kRail) + Hair(), top, S(kWidth) - Hair(), bottom);
    HFONT cellFont = Fnt(17, 0);
    for (int i = 0; i < n; ++i) {
        const int ry = top + i * rowH - v.scroll[PageWatch];
        if (ry + rowH < top || ry > bottom) continue;
        const Ticks prev = (i + 1 < n) ? w.laps[(size_t)i + 1] : 0;
        const Ticks split = w.laps[(size_t)i] - prev;
        const COLORREF ink = i == fast ? theme::TextHi : (i == slow ? theme::TextDim : theme::Text);
        RECT a = { c1, ry, c1 + S(80), ry + rowH };
        Txt(dc, Fnt(15, 1), L"Lap " + std::to_wstring(w.lapSeq - i), a, ink, DT_LEFT | DT_VCENTER);
        if (i == fast || i == slow) {
            RECT tag = { c1 + S(86), ry + S(8), c1 + S(86) + S(58), ry + rowH - S(8) };
            StrokeRound(dc, tag, S(8), i == fast ? theme::MetalEdge : theme::Rule, 255, (float)Hair());
            Txt(dc, Fnt(12, 1), i == fast ? L"fastest" : L"slowest", tag, i == fast ? theme::TextHi : theme::TextDim,
                DT_CENTER | DT_VCENTER);
        }
        Cells(dc, cellFont, timer::FormatDuration(split, true), c2, ry, ry + rowH, 1, ink);
        Cells(dc, cellFont, timer::FormatDuration(w.laps[(size_t)i], true), c3, ry, ry + rowH, 1, ink);
        RECT line = { left, ry + rowH - Hair(), right, ry + rowH };
        theme::Wash(dc, line, theme::Line, 255);
    }
    RestoreDC(dc, saved);
    ScrollBar(c, top, bottom, v.scroll[PageWatch], v.scrollMax);
}

// ---------------------------------------------------------------- editors
std::wstring TwoDigits(int v) { wchar_t b[16]; swprintf_s(b, L"%02d", v); return b; }

std::wstring HourText(bool h24, int h) {
    if (h24) return TwoDigits(h);
    return std::to_wstring(h % 12 == 0 ? 12 : h % 12) + (h >= 12 ? L" PM" : L" AM");
}

// A value with an up and a down arrow, the way the Clock app's time pickers go.
void Spinner(Ctx& c, int cx, int top, int field, const std::wstring& text, int widthDip = 72, int fontDip = 30) {
    const int w = S(widthDip), x = cx - w / 2;
    RECT up = { x, top, x + w, top + S(20) };
    RECT val = { x, top + S(22), x + w, top + S(22) + S(46) };
    RECT dn = { x, val.bottom + S(2), x + w, val.bottom + S(2) + S(20) };
    const bool focus = c.v.focus == field;
    AddHit(c, up, CmdSpinUp, field, true);
    AddHit(c, dn, CmdSpinDown, field, true);
    AddHit(c, val, CmdSpinFocus, field, true);
    for (int k = 0; k < 2; ++k) {
        const RECT& b = k == 0 ? up : dn;
        const int cmd = k == 0 ? CmdSpinUp : CmdSpinDown;
        const bool hot = Hot(c, cmd, field), down = Down(c, cmd, field);
        if (hot) FillRound(c.dc, b, S(6), down ? theme::Plate : theme::Metal2);
        DrawIcon(c.dc, k == 0 ? IcUp : IcDown, (b.left + b.right) / 2.0f, (b.top + b.bottom) / 2.0f + (down ? 1.0f : 0.0f),
                 (float)S(8), hot ? theme::TextHi : theme::TextDim);
    }
    const bool hot = Hot(c, CmdSpinFocus, field);
    if (focus) {
        GradRound(c.dc, val, S(8), theme::Metal1, theme::Metal3);
        StrokeRound(c.dc, val, S(8), theme::MetalEdge, 255, (float)Hair());
    } else {
        FillRound(c.dc, val, S(8), hot ? theme::Metal2 : theme::PlateLow);
        StrokeRound(c.dc, val, S(8), hot ? theme::Rule : theme::Line, 255, (float)Hair());
    }
    Cells(c.dc, Fnt(fontDip, 0), text, (val.left + val.right) / 2, val.top, val.bottom, 0,
          focus || hot ? theme::TextHi : theme::Text);
}

void Colon(Ctx& c, int cx, int top) {
    RECT r = { cx - S(8), top + S(22), cx + S(8), top + S(68) };
    Txt(c.dc, Fnt(30, 0), L":", r, theme::TextDim, DT_CENTER | DT_VCENTER);
}

void NameField(Ctx& c, RECT r, const TextField& f, bool focused) {
    AddHit(c, r, CmdName, 0);
    const bool hot = Hot(c, CmdName, 0);
    FillRound(c.dc, r, S(8), RGB(14, 14, 14));
    StrokeRound(c.dc, r, S(8), focused ? theme::MetalEdge : (hot ? theme::Rule : theme::Line), 255, (float)Hair() * (focused ? 1.5f : 1.0f));
    RECT t = { r.left + S(14), r.top, r.right - S(14), r.bottom };
    c.v.nameRect = t;
    HFONT font = Fnt(16, 1);
    HDC dc = c.dc;
    if (f.s.empty() && !focused) {
        Txt(dc, font, L"Name", t, theme::TextMute, DT_LEFT | DT_VCENTER);
        return;
    }
    HGDIOBJ old = SelectObject(dc, font);
    auto prefix = [&](int n) {
        SIZE sz = {};
        GetTextExtentPoint32W(dc, f.s.c_str(), n, &sz);
        return sz.cx;
    };
    const int lo = (std::min)(f.caret, f.anchor), hi = (std::max)(f.caret, f.anchor);
    const int ty = (t.top + t.bottom) / 2;
    const int lineH = S(22);
    if (focused && hi > lo) {
        RECT sel = { t.left + prefix(lo), ty - lineH / 2, t.left + prefix(hi), ty + lineH / 2 };
        theme::Wash(dc, sel, theme::Mix(theme::Metal1, theme::MetalEdge, 0.4f), 255);
    }
    SelectObject(dc, old);
    Txt(dc, font, f.s, t, theme::TextHi, DT_LEFT | DT_VCENTER);
    if (focused && hi == lo) {
        old = SelectObject(dc, font);
        const int cx = t.left + prefix(f.caret);
        SelectObject(dc, old);
        RECT caret = { cx, ty - lineH / 2, cx + (std::max)(1, S(2)), ty + lineH / 2 };
        theme::Wash(dc, caret, theme::TextHi, 255);
    }
}

void Dim(Ctx& c) {
    RECT all = { Hair(), Hair(), S(kWidth) - Hair(), S(kHeight) - Hair() };
    theme::Wash(c.dc, all, theme::Bg, 206);
}

RECT SheetFrame(Ctx& c, RECT r, const wchar_t* title) {
    FillRound(c.dc, r, S(12), RGB(10, 10, 10));
    StrokeRound(c.dc, r, S(12), theme::MetalEdge, 255, (float)Hair());
    const std::wstring caps = theme::Caps(title);
    RECT t = { r.left + S(28), r.top + S(14), r.right - S(28), r.top + S(54) };
    theme::Print(c.dc, Font::Heading, caps, t, theme::TextHi, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(3));
    return t;
}

void PaintTimerSheet(Ctx& c) {
    View& v = c.v;
    const int w = S(540), h = S(372);
    const int cx = (S(kRail) + S(kWidth)) / 2;
    RECT r = { cx - w / 2, S(kHeight - 372) / 2 + S(6), cx - w / 2 + w, S(kHeight - 372) / 2 + S(6) + h };
    SheetFrame(c, r, v.td.index < 0 ? L"New timer" : L"Edit timer");
    const int pad = S(28);
    Label(c, L"Name", r.left + pad, r.top + S(72));
    RECT nf = { r.left + pad, r.top + S(94), r.right - pad, r.top + S(94) + S(42) };
    NameField(c, nf, v.td.name, v.focus == FieldName);

    Label(c, L"Length", r.left + pad, r.top + S(156));
    static const wchar_t* const kUnit[4] = { L"days", L"hours", L"minutes", L"seconds" };
    const int colW = S(86);
    const int x0 = cx - (4 * colW) / 2;
    const int sy = r.top + S(180);
    for (int i = 0; i < 4; ++i) {
        const int sx = x0 + i * colW + colW / 2;
        Spinner(c, sx, sy, i, i == 0 ? std::to_wstring(v.td.v[0]) : TwoDigits(v.td.v[i]), 72, 28);
        RECT u = { sx - colW / 2, sy + S(92), sx + colW / 2, sy + S(112) };
        Txt(c.dc, Fnt(13, 1), kUnit[i], u, theme::TextDim, DT_CENTER | DT_VCENTER);
        if (i == 2 || i == 3) Colon(c, x0 + i * colW, sy);
    }

    RECT line = { r.left + pad, r.bottom - S(64), r.right - pad, r.bottom - S(64) + Hair() };
    theme::Wash(c.dc, line, theme::Line, 255);
    RECT save = { r.right - pad - S(110), r.bottom - S(52), r.right - pad, r.bottom - S(14) };
    RECT cancel = { save.left - S(10) - S(100), save.top, save.left - S(10), save.bottom };
    Button(c, save, L"Save", true, CmdSave);
    Button(c, cancel, L"Cancel", false, CmdCancel);
}

// ---- the alarm editor
const wchar_t* const kKindName[5] = { L"Once", L"Hourly", L"Daily", L"Weekly", L"Monthly" };

void PaintAlarmSheet(Ctx& c) {
    View& v = c.v;
    alarm::Alarm& a = v.ad.a;
    HDC dc = c.dc;
    const int w = S(644), h = S(508);
    RECT r = { S(kRail) + S(20), S(kTop) + S(4), S(kRail) + S(20) + w, S(kTop) + S(4) + h };
    SheetFrame(c, r, v.ad.index < 0 ? L"New alarm" : L"Edit alarm");

    const int pad = S(28);
    const int footH = S(64);
    RECT view = { r.left + Hair(), r.top + S(58), r.right - Hair(), r.bottom - footH };
    RECT foot = { r.left + pad, view.bottom, r.right - pad, view.bottom + Hair() };
    theme::Wash(dc, foot, theme::Line, 255);

    // ---- the footer buttons are not scrolled
    {
        RECT save = { r.right - pad - S(110), r.bottom - S(52), r.right - pad, r.bottom - S(14) };
        RECT cancel = { save.left - S(10) - S(100), save.top, save.left - S(10), save.bottom };
        Button(c, save, L"Save", true, CmdSave);
        Button(c, cancel, L"Cancel", false, CmdCancel);
        if (v.ad.index >= 0) {
            RECT del = { r.left + pad, save.top, r.left + pad + S(100), save.bottom };
            Button(c, del, L"Delete", false, CmdDelete, 0, true);
        }
    }

    // ---- the scrolling body
    const RECT keepClip = c.clip;
    c.clip = view;
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, view.left, view.top, view.right, view.bottom);
    int y = view.top - v.sheetScroll + S(6);
    const int x0 = r.left + pad, x1 = r.right - pad;
    const bool h24 = c.m.hours24;
    auto lbl = [&](const wchar_t* t) { Label(c, t, x0, y); y += S(20); };

    const bool repeating = a.kind != alarm::Kind::Once;

    // name
    lbl(L"Name");
    NameField(c, { x0, y, x1, y + S(42) }, v.ad.name, v.focus == FieldName);
    y += S(42) + S(16);

    // time: hour : minute (+ AM/PM), or the minute and the hour window for Hourly
    const bool hourly = a.kind == alarm::Kind::Hourly;
    Label(c, hourly ? L"Minute past the hour" : L"Time", x0, y);
    if (hourly) Label(c, L"Between", x0 + S(238), y);
    y += S(20);
    const int sp = S(72);
    int sx = x0 + sp / 2;
    if (!hourly) {
        Spinner(c, sx, y, FieldHour, h24 ? TwoDigits(a.hour) : std::to_wstring(a.hour % 12 == 0 ? 12 : a.hour % 12));
        Colon(c, sx + sp / 2 + S(10), y);
        sx += sp + S(20);
    }
    Spinner(c, sx, y, FieldMinute, TwoDigits(a.minute));
    if (!hourly && !h24) {
        const int ax = sx + sp / 2 + S(16);
        Chip(c, { ax, y + S(12), ax + S(60), y + S(12) + S(34) }, L"AM", a.hour < 12, CmdAmPm, 0);
        Chip(c, { ax, y + S(12) + S(40), ax + S(60), y + S(12) + S(40) + S(34) }, L"PM", a.hour >= 12, CmdAmPm, 1);
    }
    if (hourly) {
        const int fx = x0 + S(238) + sp / 2;
        Spinner(c, fx, y, FieldFrom, HourText(h24, a.hourFrom), 84, 22);
        RECT to = { fx + S(42) + S(6), y + S(22), fx + S(42) + S(40), y + S(68) };
        Txt(dc, Fnt(15, 1), L"to", to, theme::TextDim, DT_CENTER | DT_VCENTER);
        Spinner(c, fx + S(84) + S(40), y, FieldTo, HourText(h24, a.hourTo), 84, 22);
    }
    y += S(90) + S(16);

    // repeat
    Label(c, L"Repeat", x0, y);
    y += S(20);
    {
        RECT seg = { x0, y, x1, y + S(42) };
        FillRound(dc, seg, S(9), RGB(14, 14, 14));
        StrokeRound(dc, seg, S(9), theme::Line, 255, (float)Hair());
        const int sw = (seg.right - seg.left - S(8)) / 5;
        for (int k = 0; k < 5; ++k) {
            RECT cell = { seg.left + S(4) + k * sw, seg.top + S(4), seg.left + S(4) + (k + 1) * sw, seg.bottom - S(4) };
            Chip(c, cell, kKindName[k], (int)a.kind == k, CmdKind, k, 15);
        }
        y += S(42) + S(16);
    }

    // by kind
    switch (a.kind) {
        case alarm::Kind::Once: {
            Label(c, L"Date", x0, y);
            y += S(20);
            const int dx = x0 + sp / 2;
            Spinner(c, dx, y, FieldDay, TwoDigits(a.day));
            Spinner(c, dx + sp + S(14) + S(6), y, FieldMonth, kMonShort[(a.month > 0 ? a.month : 1) - 1], 84, 26);
            Spinner(c, dx + sp + S(14) + S(6) + S(42) + S(14) + S(50), y, FieldYear, std::to_wstring(a.year), 100, 26);
            const int cx0 = x0 + S(380);
            const alarm::LocalTime today = alarm::SystemZone().ToLocal(c.m.now);
            const alarm::LocalTime tmr = alarm::SystemZone().ToLocal(c.m.now + alarm::kSecond * 86400);
            Chip(c, { cx0, y + S(12), cx0 + S(110), y + S(12) + S(34) }, L"Today",
                 a.year == today.year && a.month == today.month && a.day == today.day, CmdDay, 0);
            Chip(c, { cx0, y + S(12) + S(40), cx0 + S(110), y + S(12) + S(40) + S(34) }, L"Tomorrow",
                 a.year == tmr.year && a.month == tmr.month && a.day == tmr.day, CmdDay, 1);
            if (alarm::OnceIsPast(a, c.m.now, alarm::SystemZone())) {
                // Save moves it on (RollPastOnce), so say so before, as Windows Clock does.
                RECT t = { x0 + S(80), y - S(20), x1, y };     // on the DATE label's row, in view without scrolling
                Txt(dc, Fnt(13, 1), L"That time has passed - it will ring tomorrow", t, theme::TextDim, DT_LEFT | DT_VCENTER);
            }
            y += S(90) + S(16);
            break;
        }
        case alarm::Kind::Hourly: {
            RECT t = { x0, y - S(8), x1, y + S(14) };
            Txt(dc, Fnt(14, 1), L"Rings every hour at the minute above, between the hours chosen (inclusive).", t, theme::TextDim, DT_LEFT | DT_VCENTER);
            y += S(36);
            break;
        }
        case alarm::Kind::Daily: {
            RECT t = { x0, y - S(8), x1, y + S(14) };
            Txt(dc, Fnt(14, 1), L"Rings every day at the time above.", t, theme::TextDim, DT_LEFT | DT_VCENTER);
            y += S(36);
            break;
        }
        case alarm::Kind::Weekly: {
            Label(c, L"Days", x0, y);
            y += S(24);
            const int cw = (x1 - x0 - 6 * S(8)) / 7;
            for (int d = 0; d < 7; ++d)
                Chip(c, { x0 + d * (cw + S(8)), y, x0 + d * (cw + S(8)) + cw, y + S(38) }, kDayShort[d],
                     (a.weekdays >> d) & 1, CmdWeekday, d);
            y += S(38);
            if (!a.weekdays) {
                RECT t = { x0, y + S(6), x1, y + S(26) };
                Txt(dc, Fnt(13, 1), L"Choose at least one day.", t, theme::Warn, DT_LEFT | DT_VCENTER);
            }
            y += S(34);
            break;
        }
        case alarm::Kind::Monthly: {
            Label(c, L"Days of the month", x0, y);
            y += S(24);
            const int cw = (x1 - x0 - 6 * S(6)) / 7;
            for (int d = 0; d < 31; ++d) {
                const int col = d % 7, row = d / 7;
                RECT cell = { x0 + col * (cw + S(6)), y + row * S(38), x0 + col * (cw + S(6)) + cw, y + row * S(38) + S(32) };
                Chip(c, cell, std::to_wstring(d + 1), (a.monthDays >> d) & 1, CmdMonthDay, d, 14);
            }
            RECT last = { x0 + 3 * (cw + S(6)), y + 4 * S(38), x1, y + 4 * S(38) + S(32) };
            Chip(c, last, L"Last day", (a.monthDays & alarm::kLastDayBit) != 0, CmdLastDay, 0, 14);
            y += 5 * S(38);
            if (!a.monthDays) {
                RECT t = { x0, y + S(2), x1, y + S(22) };
                Txt(dc, Fnt(13, 1), L"Choose at least one day.", t, theme::Warn, DT_LEFT | DT_VCENTER);
            }
            y += S(30);
            break;
        }
    }

    // limit to
    if (repeating) {
        RECT head = { x0, y, x1, y + S(36) };
        AddHit(c, head, CmdLimitOpen, 0);
        const bool hot = Hot(c, CmdLimitOpen, 0);
        if (hot) FillRound(dc, head, S(8), theme::Metal2);
        DrawIcon(dc, v.ad.limitOpen ? IcDown : IcChevronRight, (float)(head.left + S(14)), (float)(y + S(18)), (float)S(7),
                 hot ? theme::TextHi : theme::TextDim);
        Label(c, L"Limit to", head.left + S(34), y + S(9), hot ? theme::TextHi : theme::TextDim);
        const bool anyLimit = a.weeks || a.months || a.until || (a.weekdays && (hourly || a.kind == alarm::Kind::Daily));
        if (!v.ad.limitOpen) {
            RECT t = { head.left + S(130), y, x1, y + S(36) };
            Txt(dc, Fnt(13, 1), anyLimit ? L"A limit is set" : L"Weeks, months, weekdays or an end date",
                t, anyLimit ? theme::TextDim : theme::TextMute, DT_LEFT | DT_VCENTER);
        }
        y += S(36) + S(10);
        if (v.ad.limitOpen) {
            const int chipH = S(34);
            // weeks of the month
            Label(c, L"Weeks of the month", x0, y);
            y += S(24);
            {
                static const wchar_t* const kW[6] = { L"1st", L"2nd", L"3rd", L"4th", L"5th", L"Last" };
                const int cw = (x1 - x0 - 5 * S(8)) / 6;
                for (int k = 0; k < 6; ++k)
                    Chip(c, { x0 + k * (cw + S(8)), y, x0 + k * (cw + S(8)) + cw, y + chipH }, kW[k], (a.weeks >> k) & 1, CmdWeek, k);
                y += chipH + S(18);
            }
            Label(c, L"Months", x0, y);
            y += S(24);
            {
                const int cw = (x1 - x0 - 5 * S(8)) / 6;
                for (int k = 0; k < 12; ++k) {
                    const int col = k % 6, row = k / 6;
                    Chip(c, { x0 + col * (cw + S(8)), y + row * (chipH + S(8)), x0 + col * (cw + S(8)) + cw, y + row * (chipH + S(8)) + chipH },
                         kMonShort[k], (a.months >> k) & 1, CmdMonth, k);
                }
                y += 2 * chipH + S(8) + S(18);
            }
            if (hourly || a.kind == alarm::Kind::Daily) {
                Label(c, L"Weekdays", x0, y);
                y += S(24);
                const int cw = (x1 - x0 - 6 * S(8)) / 7;
                for (int d = 0; d < 7; ++d)
                    Chip(c, { x0 + d * (cw + S(8)), y, x0 + d * (cw + S(8)) + cw, y + chipH }, kDayShort[d], (a.weekdays >> d) & 1, CmdLimitWd, d);
                y += chipH + S(18);
            }
            Label(c, L"Until", x0, y);
            y += S(24);
            Chip(c, { x0, y, x0 + S(110), y + chipH }, a.until ? L"Ends on" : L"No end date", a.until != 0, CmdUntil, 0);
            if (a.until) {
                const int ux = x0 + S(110) + S(24) + sp / 2;
                y += chipH + S(10);
                Spinner(c, x0 + sp / 2, y, FieldUntilDay, TwoDigits(a.until % 100));
                Spinner(c, x0 + sp / 2 + sp + S(14) + S(6), y, FieldUntilMonth, kMonShort[((a.until / 100) % 100 > 0 ? (a.until / 100) % 100 : 1) - 1], 84, 26);
                Spinner(c, x0 + sp / 2 + sp + S(14) + S(6) + S(42) + S(14) + S(50), y, FieldUntilYear, std::to_wstring(a.until / 10000), 100, 26);
                (void)ux;
                y += S(90);
            } else {
                y += chipH;
            }
            y += S(22);
        }
    }

    // sound and snooze
    {
        Label(c, L"Sound", x0, y + S(8));
        Switch(c, { x0 + S(70), y + S(4), x0 + S(70) + S(46), y + S(28) }, a.sound, CmdSound, 0);
        Label(c, L"Snooze", x0 + S(200), y + S(8));
        static const int kS[4] = { 5, 10, 15, 30 };
        for (int k = 0; k < 4; ++k) {
            const int cx0 = x0 + S(280) + k * S(76);
            Chip(c, { cx0, y, cx0 + S(68), y + S(34) }, std::to_wstring(kS[k]) + L" min", a.snoozeMin == kS[k], CmdSnoozeLen, kS[k], 14);
        }
        y += S(34) + S(24);
    }

    RestoreDC(dc, saved);
    c.clip = keepClip;
    // A soft edge where the body runs under the title and the footer says "there is more".
    if (v.sheetScroll > 0) theme::FadeV(dc, { view.left + S(8), view.top, view.right - S(8), view.top + S(22) }, RGB(10, 10, 10), 255, 0);
    const int contentH = y + v.sheetScroll - view.top;
    v.scrollMax = (std::max)(0, contentH - (int)(view.bottom - view.top));
    v.scrollRect = view;
    if (v.sheetScroll > v.scrollMax) { v.sheetScroll = v.scrollMax; v.again = true; }
    if (v.sheetScroll < v.scrollMax)
        theme::FadeV(dc, { view.left + S(8), view.bottom - S(30), view.right - S(8), view.bottom }, RGB(10, 10, 10), 0, 255);
    ScrollBar(c, view.top, view.bottom, v.sheetScroll, v.scrollMax);
}

} // namespace

// ---------------------------------------------------------------- public
std::wstring FormatIn(Ticks span) {
    if (span < 0) span = 0;
    const long long mins = (long long)((span + kMinute - 1) / kMinute);     // rounded up
    if (mins <= 0) return L"in less than a minute";
    const long long d = mins / 1440, h = (mins / 60) % 24, mi = mins % 60;
    wchar_t buf[64];
    if (d > 0)      swprintf_s(buf, L"in %lld d %lld h", d, h);
    else if (h > 0) swprintf_s(buf, L"in %lld h %lld min", h, mi);
    else            swprintf_s(buf, L"in %lld min", mi);
    return buf;
}

int CaretAt(HDC dc, const std::wstring& text, int relX) {
    HGDIOBJ old = SelectObject(dc, Fnt(16, 1));
    int best = 0;
    int bestDist = relX < 0 ? -relX : relX;
    for (int i = 1; i <= (int)text.size(); ++i) {
        SIZE sz = {};
        GetTextExtentPoint32W(dc, text.c_str(), i, &sz);
        const int d = sz.cx > relX ? sz.cx - relX : relX - sz.cx;
        if (d < bestDist) { bestDist = d; best = i; }
    }
    SelectObject(dc, old);
    return best;
}

void ReleaseFonts() {
    for (FontEntry& e : g_fonts) if (e.f) DeleteObject(e.f);
    g_fonts.clear();
}

void Paint(HDC dc, const SIZE& size, const Model& m, View& v) {
    v.hits.clear();
    v.scrollRect = {};
    v.scrollMax = 0;
    v.again = false;
    Ctx c{ dc, m, v, { 0, 0, size.cx, size.cy }, true };

    RECT all = { 0, 0, size.cx, size.cy };
    theme::Wash(dc, all, theme::Bg, 255);

    const bool sheet = v.sheet != SheetNone;
    c.live = !sheet;
    PaintRail(c);

    int y0 = S(92);
    if (v.ringing) {
        PaintBanner(c, y0);
        y0 += S(kBannerH) + S(14);
    }
    switch (v.page) {
        case PageClock: PaintClockPage(c, y0); break;
        case PageAlarm: PaintAlarmPage(c, y0); break;
        case PageTimer: PaintTimerPage(c, y0); break;
        default:        PaintWatchPage(c, y0); break;
    }
    if (sheet) Dim(c);
    c.live = true;
    PaintCorner(c);
    if (sheet) {
        if (v.sheet == SheetTimer) PaintTimerSheet(c);
        else                       PaintAlarmSheet(c);
    }
    RECT frame = { 0, 0, size.cx, size.cy };
    theme::Frame(dc, frame, theme::Rule, 255, Hair());
}

} // namespace panel
} // namespace awa
