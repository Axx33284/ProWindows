#include "clockpaint.h"
#include "monpaint.h"
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

using namespace Gdiplus;

constexpr float kPi = 3.14159265f;

Color Argb(COLORREF c, BYTE a) {
    return Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

void AddRoundedPath(GraphicsPath* path, const RectF& r, float radius) {
    if (radius * 2.0f <= 0.5f) { path->AddRectangle(r); return; }
    const float rr = (std::min)(radius, (std::min)(r.Width, r.Height) / 2.0f);
    const float dd = rr * 2.0f;
    path->AddArc(r.X, r.Y, dd, dd, 180.0f, 90.0f);
    path->AddArc(r.GetRight() - dd, r.Y, dd, dd, 270.0f, 90.0f);
    path->AddArc(r.GetRight() - dd, r.GetBottom() - dd, dd, dd, 0.0f, 90.0f);
    path->AddArc(r.X, r.GetBottom() - dd, dd, dd, 90.0f, 90.0f);
    path->CloseFigure();
}

// ---------------------------------------------------------------- fonts
// One family per ClockFace, resolved once. Every one of these falls back to
// Segoe UI on a machine that has had its fonts pruned; the look is then
// plainer, not broken. Leaked on purpose: a static would be destroyed after
// GdiplusShutdown.
struct FaceSpec { const wchar_t* family; INT style; };

const FontFamily* FamilyFor(ClockFace face) {
    static const FontFamily* cache[6] = {};
    const int i = (int)face;
    if (i < 0 || i >= 6) return FamilyFor(CLOCK_FACE_REGULAR);
    if (cache[i]) return cache[i];

    static const wchar_t* const kNames[6] = {
        L"Segoe UI Light", L"Segoe UI", L"Segoe UI Semibold",
        L"Consolas", L"Bahnschrift SemiBold Condensed", L"Georgia",
    };
    FontFamily* f = new FontFamily(kNames[i]);
    if (!f->IsAvailable()) {
        delete f;
        // The condensed face has a second choice before the plain one: a
        // narrow semibold keeps the game-menu look better than a light does.
        if (face == CLOCK_FACE_CONDENSED) {
            f = new FontFamily(L"Bahnschrift");
            if (!f->IsAvailable()) { delete f; f = new FontFamily(L"Segoe UI Semibold"); }
        } else {
            f = new FontFamily(L"Segoe UI");
        }
    }
    cache[i] = f;
    return f;
}

// GDI+ fonts are expensive to make and the clock draws a dozen strings a
// frame, so every (face, size) pair is built once and kept. Sizes are pixels,
// quantised to a quarter so the size slider does not mint a font per position.
const Font* CachedFont(ClockFace face, float px, bool bold = false) {
    struct Entry { int face; int key; bool bold; const Font* font; };
    static std::vector<Entry>* cache = new std::vector<Entry>();
    const int key = (int)((std::max)(4.0f, px) * 4.0f + 0.5f);
    for (const Entry& e : *cache)
        if (e.face == (int)face && e.key == key && e.bold == bold) return e.font;

    constexpr size_t kMaxFonts = 96;
    if (cache->size() >= kMaxFonts) {
        // Reuse the nearest, never leak: see monpaint.cpp for the reasoning.
        const Font* best = nullptr;
        int bestDelta = 0;
        for (const Entry& e : *cache) {
            if (e.face != (int)face || e.bold != bold) continue;
            const int delta = (e.key > key) ? e.key - key : key - e.key;
            if (!best || delta < bestDelta) { best = e.font; bestDelta = delta; }
        }
        if (best) return best;
    }
    // The tiles and the stacked digits ask for weight; a Light face has none
    // to give, so those take the Semibold family instead.
    const FontFamily* family = FamilyFor((bold && face == CLOCK_FACE_LIGHT)
                                         ? CLOCK_FACE_SEMIBOLD : face);
    INT style = FontStyleRegular;
    // A face that is already a weight of its own (Light, Semibold) is not
    // emboldened further: GDI+ would synthesise a smeared bold. Regular and
    // the monospace take a real bold.
    if (bold && (face == CLOCK_FACE_REGULAR || face == CLOCK_FACE_MONO ||
                 face == CLOCK_FACE_SERIF))
        style = FontStyleBold;
    const Font* font = new Font(family, (float)key / 4.0f, style, UnitPixel);
    cache->push_back({ (int)face, key, bold, font });
    return font;
}

const StringFormat* Typographic() {
    // Generic typographic: no padding round the glyphs, so a measured width
    // is the ink width and centred text is actually centred.
    static const StringFormat* fmt = [] {
        StringFormat* f = new StringFormat(StringFormat::GenericTypographic());
        f->SetFormatFlags(f->GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces |
                          StringFormatFlagsNoWrap);
        f->SetTrimming(StringTrimmingNone);
        return f;
    }();
    return fmt;
}

// Measuring needs a Graphics, and the caller may be measuring without one.
Graphics* Measurer() {
    static Graphics* g = [] {
        Bitmap* b = new Bitmap(4, 4, PixelFormat32bppPARGB);
        Graphics* mg = new Graphics(b);
        mg->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        return mg;
    }();
    return g;
}

SizeF TextSize(const wchar_t* text, const Font* font) {
    if (!text || !*text) return SizeF(0.0f, 0.0f);
    RectF box;
    Measurer()->MeasureString(text, -1, font, PointF(0, 0), Typographic(), &box);
    return SizeF(box.Width, box.Height);
}

// Set by ClockDraw for the frame: bare skins draw a dark shadow under every
// string so white digits stay readable on a white wallpaper.
bool g_textShadow = false;
BYTE g_glow = 0;
COLORREF g_glowColour = 0;
float g_scale = 1.0f;

// Draws one string with its top-left at (x, y), plus the skin's shadow or
// glow. Glow is the digits' own outline drawn wide and faint, three times,
// underneath - a real blur is a convolution over the whole panel per frame,
// and this is indistinguishable at these sizes.
void Text(Graphics* g, const wchar_t* text, const Font* font, COLORREF colour,
          BYTE alpha, float x, float y) {
    if (!text || !*text) return;

    if (g_glow > 0) {
        GraphicsPath path;
        FontFamily family;
        font->GetFamily(&family);
        path.AddString(text, -1, &family, font->GetStyle(), font->GetSize(),
                       PointF(x, y), Typographic());
        const float s = g_scale;
        const struct { float width; float strength; } rings[3] = {
            { 10.0f * s, 0.22f }, { 5.0f * s, 0.45f }, { 2.0f * s, 0.8f },
        };
        for (const auto& ring : rings) {
            Pen pen(Argb(g_glowColour, (BYTE)(g_glow * ring.strength * alpha / 255)),
                    ring.width);
            pen.SetLineJoin(LineJoinRound);
            g->DrawPath(&pen, &path);
        }
    } else if (g_textShadow) {
        const float off = (std::max)(1.0f, (float)(int)(1.5f * g_scale));
        SolidBrush shade(Color((BYTE)(alpha * 3 / 4), 0, 0, 0));
        g->DrawString(text, -1, font, PointF(x + off, y + off), Typographic(), &shade);
        // A soft halo as well as the offset copy: the offset alone reads as
        // a hard drop shadow on light wallpapers.
        GraphicsPath path;
        FontFamily family;
        font->GetFamily(&family);
        path.AddString(text, -1, &family, font->GetStyle(), font->GetSize(),
                       PointF(x, y), Typographic());
        Pen halo(Color((BYTE)(alpha / 5), 0, 0, 0), 4.0f * g_scale);
        halo.SetLineJoin(LineJoinRound);
        g->DrawPath(&halo, &path);
    }

    SolidBrush brush(Argb(colour, alpha));
    g->DrawString(text, -1, font, PointF(x, y), Typographic(), &brush);
}

// Small capitals with a little air between the letters. GDI+ has no
// tracking, so a hair space goes between each pair; at 12 px it reads as
// letter-spacing rather than as gaps.
std::wstring Tracked(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() * 2);
    for (size_t i = 0; i < s.size(); ++i) {
        out += (wchar_t)towupper(s[i]);
        if (i + 1 < s.size()) out += L'\x200A';
    }
    return out;
}

std::wstring Upper(const std::wstring& s) {
    std::wstring out = s;
    for (auto& c : out) c = (wchar_t)towupper(c);
    return out;
}

// ---------------------------------------------------------------- pieces
void FillCircle(Graphics* g, float cx, float cy, float r, const Color& c) {
    SolidBrush b(c);
    g->FillEllipse(&b, cx - r, cy - r, r * 2, r * 2);
}

void FillRound(Graphics* g, const RectF& r, float radius, const Color& c) {
    GraphicsPath path;
    AddRoundedPath(&path, r, radius);
    SolidBrush b(c);
    g->FillPath(&b, &path);
}

// A hand from the centre, drawn as a rounded line with a short tail.
void Hand(Graphics* g, float cx, float cy, float angleDeg, float length,
          float tail, float width, const Color& c) {
    const float a = (angleDeg - 90.0f) * kPi / 180.0f;
    Pen pen(c, width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g->DrawLine(&pen, cx - cosf(a) * tail, cy - sinf(a) * tail,
                cx + cosf(a) * length, cy + sinf(a) * length);
}

// One seven-segment digit. Segments are the classic a..g, each a hexagon so
// the corners meet at 45 degrees the way an LED display's do; `lit` says which
// are on and the rest are drawn in the ghost colour.
void SegmentDigit(Graphics* g, int digit, float x, float y, float w, float h,
                  float thick, const Color& on, const Color& off) {
    static const unsigned char kMap[10] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
    };
    const unsigned mask = (digit >= 0 && digit <= 9) ? kMap[digit] : 0;
    const float t = thick, half = t / 2.0f, gap = t * 0.18f;
    const float midY = y + h / 2.0f;

    // Horizontal and vertical segment shapes, as hexagons.
    auto horizontal = [&](float sx, float sy, bool lit) {
        PointF pts[6] = {
            { sx + gap,               sy },
            { sx + gap + half,        sy - half },
            { sx + w - gap - half,    sy - half },
            { sx + w - gap,           sy },
            { sx + w - gap - half,    sy + half },
            { sx + gap + half,        sy + half },
        };
        SolidBrush b(lit ? on : off);
        g->FillPolygon(&b, pts, 6);
    };
    auto vertical = [&](float sx, float sy, float len, bool lit) {
        PointF pts[6] = {
            { sx,        sy + gap },
            { sx + half, sy + gap + half },
            { sx + half, sy + len - gap - half },
            { sx,        sy + len - gap },
            { sx - half, sy + len - gap - half },
            { sx - half, sy + gap + half },
        };
        SolidBrush b(lit ? on : off);
        g->FillPolygon(&b, pts, 6);
    };

    horizontal(x, y,        (mask & 0x01) != 0);      // a  top
    vertical(x + w, y, h / 2.0f, (mask & 0x02) != 0); // b  top right
    vertical(x + w, midY, h / 2.0f, (mask & 0x04) != 0); // c bottom right
    horizontal(x, y + h,    (mask & 0x08) != 0);      // d  bottom
    vertical(x, midY, h / 2.0f, (mask & 0x10) != 0);  // e  bottom left
    vertical(x, y, h / 2.0f, (mask & 0x20) != 0);     // f  top left
    horizontal(x, midY,     (mask & 0x40) != 0);      // g  middle
}

// ---------------------------------------------------------------- styles
// Each style lays itself out and, when `draw` is set, paints at (ox, oy).
// Called once without drawing to measure and once with, so the two cannot
// disagree. Sizes are unscaled and multiplied by `s` at the point of use.
struct Style {
    const ClockPaintCtx& ctx;
    const ClockText& t;
    const ClockSkin& skin;
    Graphics* g;
    bool draw;
    float s;
    BYTE a;   // text alpha: opaque, the panel opacity is the panel's business

    Style(const ClockPaintCtx& c, const ClockText& text, Graphics* gr, bool d)
        : ctx(c), t(text), skin(*c.skin), g(gr), draw(d), s(c.scale), a(255) {}

    const Font* F(float px, bool bold = false) const {
        return CachedFont(skin.face, px * s, bold);
    }
    // The date face: the same family, except the condensed and serif faces
    // keep their character and the rest set the date in plain Segoe UI so
    // a light 60 px time is not paired with a light 12 px date nobody can read.
    const Font* D(float px) const {
        const ClockFace face = (skin.face == CLOCK_FACE_CONDENSED || skin.face == CLOCK_FACE_SERIF ||
                                skin.face == CLOCK_FACE_MONO) ? skin.face : CLOCK_FACE_REGULAR;
        return CachedFont(face, px * s, false);
    }
    void Put(const std::wstring& text, const Font* f, COLORREF c, float x, float y) const {
        if (draw) Text(g, text.c_str(), f, c, a, x, y);
    }

    std::wstring TimeMain() const { return t.hours + L":" + t.minutes; }

    // What the date line says, given which parts are on.
    std::wstring DateLine(bool longForm) const {
        if (!ctx.date && !ctx.weekday) return L"";
        if (ctx.date && ctx.weekday) return longForm ? t.dateLong : t.dateShort;
        if (ctx.weekday) return longForm ? t.weekdayLong : t.weekdayShort;
        return longForm ? (t.day + L" " + t.monthShort) : (t.day + L" " + t.monthShort);
    }

    SizeF Digital(float ox, float oy) const;
    SizeF Minimal(float ox, float oy) const;
    SizeF Analog(float ox, float oy) const;
    SizeF Flip(float ox, float oy) const;
    SizeF Segments(float ox, float oy) const;
    SizeF Stacked(float ox, float oy) const;
    SizeF Wide(float ox, float oy) const;
    SizeF Ring(float ox, float oy) const;

    SizeF Run(float ox, float oy) const {
        switch (ctx.style) {
            case CLOCK_STYLE_MINIMAL:  return Minimal(ox, oy);
            case CLOCK_STYLE_ANALOG:   return Analog(ox, oy);
            case CLOCK_STYLE_FLIP:     return Flip(ox, oy);
            case CLOCK_STYLE_SEGMENTS: return Segments(ox, oy);
            case CLOCK_STYLE_STACKED:  return Stacked(ox, oy);
            case CLOCK_STYLE_WIDE:     return Wide(ox, oy);
            case CLOCK_STYLE_RING:     return Ring(ox, oy);
            default:                   return Digital(ox, oy);
        }
    }
};

// Digital: the time, large and centred; AM/PM tucked against its baseline;
// the seconds smaller in the accent colour; the date underneath.
SizeF Style::Digital(float ox, float oy) const {
    const float padX = 22 * s, padY = 14 * s;
    const Font* big   = F(46);
    const Font* smallF = F(15);
    const Font* dateF = D(12.5f);

    const std::wstring main = TimeMain();
    const SizeF mainSz  = TextSize(main.c_str(), big);
    const SizeF secSz   = ctx.seconds ? TextSize(t.seconds.c_str(), smallF) : SizeF();
    const SizeF ampmSz  = t.ampm.empty() ? SizeF() : TextSize(t.ampm.c_str(), smallF);
    const std::wstring dateLine = DateLine(true);
    const SizeF dateSz  = TextSize(dateLine.c_str(), dateF);

    float lineW = mainSz.Width;
    if (ctx.seconds)     lineW += 6 * s + secSz.Width;
    if (!t.ampm.empty()) lineW += 6 * s + ampmSz.Width;
    const float contentW = (std::max)(lineW, dateSz.Width);
    float contentH = mainSz.Height;
    if (!dateLine.empty()) contentH += dateSz.Height + 1 * s;

    if (draw) {
        float x = ox + padX + (contentW - lineW) / 2.0f;
        const float y = oy + padY;
        Put(main, big, skin.time, x, y);
        x += mainSz.Width;
        // Smaller text sits on the same baseline: the big face's descent is
        // roughly a fifth of its line, and the smallF face's likewise.
        const float baseY = y + mainSz.Height * 0.80f;
        if (ctx.seconds) {
            x += 6 * s;
            Put(t.seconds, smallF, skin.accent, x, baseY - secSz.Height * 0.80f);
            x += secSz.Width;
        }
        if (!t.ampm.empty()) {
            x += 6 * s;
            Put(t.ampm, smallF, skin.date, x, baseY - ampmSz.Height * 0.80f);
        }
        if (!dateLine.empty())
            Put(dateLine, dateF, skin.date, ox + padX + (contentW - dateSz.Width) / 2.0f,
                y + mainSz.Height + 1 * s);
    }
    return SizeF(contentW + padX * 2, contentH + padY * 2);
}

// Minimal: the wallpaper clock. Very large, very thin, with the date as
// smallF tracked capitals - and, on the bare skins it was drawn for, no panel.
SizeF Style::Minimal(float ox, float oy) const {
    const float padX = 16 * s, padY = 8 * s;
    const Font* big   = F(78);
    const Font* smallF = F(20);
    const Font* dateF = D(12);

    const std::wstring main = TimeMain();
    const SizeF mainSz = TextSize(main.c_str(), big);
    const SizeF secSz  = ctx.seconds ? TextSize(t.seconds.c_str(), smallF) : SizeF();
    const SizeF ampmSz = t.ampm.empty() ? SizeF() : TextSize(t.ampm.c_str(), smallF);
    const std::wstring dateLine = Tracked(DateLine(true));
    const SizeF dateSz = TextSize(dateLine.c_str(), dateF);

    float lineW = mainSz.Width;
    if (ctx.seconds)     lineW += 8 * s + secSz.Width;
    if (!t.ampm.empty()) lineW += 8 * s + ampmSz.Width;
    const float contentW = (std::max)(lineW, dateSz.Width);
    float contentH = mainSz.Height * 0.92f;
    if (!dateLine.empty()) contentH += dateSz.Height + 2 * s;

    if (draw) {
        float x = ox + padX + (contentW - lineW) / 2.0f;
        const float y = oy + padY - mainSz.Height * 0.06f;
        Put(main, big, skin.time, x, y);
        x += mainSz.Width;
        const float baseY = y + mainSz.Height * 0.80f;
        if (ctx.seconds) {
            x += 8 * s;
            Put(t.seconds, smallF, skin.accent, x, baseY - secSz.Height * 0.80f);
            x += secSz.Width;
        }
        if (!t.ampm.empty()) {
            x += 8 * s;
            Put(t.ampm, smallF, skin.date, x, baseY - ampmSz.Height * 0.80f);
        }
        if (!dateLine.empty())
            Put(dateLine, dateF, skin.date, ox + padX + (contentW - dateSz.Width) / 2.0f,
                oy + padY + mainSz.Height * 0.92f + 2 * s);
    }
    return SizeF(contentW + padX * 2, contentH + padY * 2);
}

// Analog: a dial. Sixty ticks with the twelve hours heavier, three hands,
// a cap over the pivot, and the date in a smallF pill in the lower half.
SizeF Style::Analog(float ox, float oy) const {
    const float pad = 10 * s;
    const float d   = 168 * s;
    const float r   = d / 2.0f;
    const float cx  = ox + pad + r, cy = oy + pad + r;

    if (draw) {
        // The face: a shade lighter than the panel, with a fine rim. On a
        // bare skin only the rim is drawn, so the wallpaper is the face.
        if (!skin.bare) FillCircle(g, cx, cy, r, Argb(skin.tile, 255));
        Pen rim(Argb(skin.dim, 200), (std::max)(1.0f, 1.2f * s));
        g->DrawEllipse(&rim, cx - r, cy - r, d, d);

        for (int i = 0; i < 60; ++i) {
            const bool hour = (i % 5) == 0;
            const float ang = (float)i * 6.0f * kPi / 180.0f - kPi / 2.0f;
            const float len = hour ? 9 * s : 4 * s;
            const float in  = r - 6 * s;
            Pen tick(Argb(hour ? skin.time : skin.dim, hour ? 230 : 200),
                     hour ? 2.2f * s : 1.0f * s);
            tick.SetStartCap(LineCapRound);
            tick.SetEndCap(LineCapRound);
            g->DrawLine(&tick, cx + cosf(ang) * (in - len), cy + sinf(ang) * (in - len),
                        cx + cosf(ang) * in, cy + sinf(ang) * in);
        }

        // The date, below the pivot, as smallF capitals in a faint pill.
        const std::wstring dateLine = DateLine(false);
        if (!dateLine.empty()) {
            const Font* f = D(10.5f);
            const std::wstring txt = Upper(dateLine);
            const SizeF sz = TextSize(txt.c_str(), f);
            const float px = cx - sz.Width / 2.0f - 6 * s, py = cy + r * 0.38f;
            FillRound(g, RectF(px, py, sz.Width + 12 * s, sz.Height + 3 * s),
                      3 * s, Argb(skin.dim, skin.bare ? 90 : 120));
            Put(txt, f, skin.date, px + 6 * s, py + 1.5f * s);
        }

        const float sec = (float)ctx.time.wSecond;
        const float mnt = (float)ctx.time.wMinute + sec / 60.0f;
        const float hr  = (float)(ctx.time.wHour % 12) + mnt / 60.0f;
        Hand(g, cx, cy, hr * 30.0f,  r * 0.50f, 8 * s,  5.0f * s, Argb(skin.time, 255));
        Hand(g, cx, cy, mnt * 6.0f,  r * 0.74f, 8 * s,  3.4f * s, Argb(skin.time, 255));
        if (ctx.seconds)
            Hand(g, cx, cy, sec * 6.0f, r * 0.84f, 14 * s, 1.4f * s, Argb(skin.accent, 255));
        FillCircle(g, cx, cy, 4.2f * s, Argb(skin.accent, 255));
        FillCircle(g, cx, cy, 1.6f * s, Argb(skin.tile, 255));
    }
    return SizeF(d + pad * 2, d + pad * 2);
}

// Flip: one tile per digit, split across the middle like a split-flap board;
// hours and minutes as pairs with the colon between, the seconds smaller.
SizeF Style::Flip(float ox, float oy) const {
    const float padX = 16 * s, padY = 14 * s;
    const float tileW = 44 * s, tileH = 62 * s, tileGap = 4 * s, groupGap = 14 * s;
    const float secScale = 0.62f;
    const Font* digitF = F(44, true);
    const Font* secF   = F(44 * secScale, true);

    // Hours are always two tiles: "0" and "9" rather than a lone "9", because
    // a board has a fixed number of flaps.
    std::wstring hh = t.hours;
    if (hh.size() < 2) hh = L"0" + hh;

    float lineW = tileW * 4 + tileGap * 2 + groupGap;
    if (ctx.seconds) lineW += groupGap + tileW * secScale * 2 + tileGap;
    if (!t.ampm.empty()) lineW += 8 * s + TextSize(t.ampm.c_str(), D(12)).Width;
    const std::wstring dateLine = DateLine(true);
    const SizeF dateSz = TextSize(dateLine.c_str(), D(12));
    const float contentW = (std::max)(lineW, dateSz.Width);
    float contentH = tileH;
    if (!dateLine.empty()) contentH += 8 * s + dateSz.Height;

    if (draw) {
        auto tile = [&](wchar_t ch, float x, float y, float w, float h, const Font* f) {
            const RectF r(x, y, w, h);
            FillRound(g, r, 5 * s, Argb(skin.tile, 255));
            // The lower half a touch darker, the way a real flap catches
            // less light, then the hinge line across the middle.
            GraphicsPath lower;
            AddRoundedPath(&lower, r, 5 * s);
            Region below(&lower);
            below.Intersect(RectF(x, y + h / 2.0f, w, h / 2.0f));
            SolidBrush shade(Color(28, 0, 0, 0));
            g->FillRegion(&shade, &below);
            Pen edge(Argb(skin.dim, 160), (std::max)(1.0f, 1.0f * s));
            g->DrawPath(&edge, &lower);

            const wchar_t str[2] = { ch, 0 };
            const SizeF sz = TextSize(str, f);
            Put(str, f, skin.time, x + (w - sz.Width) / 2.0f, y + (h - sz.Height) / 2.0f);
            Pen hinge(Argb(skin.panelBottom, 200), (std::max)(1.0f, 1.5f * s));
            g->DrawLine(&hinge, x, y + h / 2.0f, x + w, y + h / 2.0f);
        };
        auto colon = [&](float x, float h) {
            FillCircle(g, x, oy + padY + h * 0.36f, 3 * s, Argb(skin.accent, 255));
            FillCircle(g, x, oy + padY + h * 0.64f, 3 * s, Argb(skin.accent, 255));
        };

        float x = ox + padX + (contentW - lineW) / 2.0f;
        const float y = oy + padY;
        tile(hh[0], x, y, tileW, tileH, digitF); x += tileW + tileGap;
        tile(hh[1], x, y, tileW, tileH, digitF); x += tileW + groupGap / 2.0f;
        colon(x, tileH);                          x += groupGap / 2.0f;
        tile(t.minutes[0], x, y, tileW, tileH, digitF); x += tileW + tileGap;
        tile(t.minutes[1], x, y, tileW, tileH, digitF); x += tileW;
        if (ctx.seconds) {
            x += groupGap;
            const float sw = tileW * secScale, sh = tileH * secScale;
            const float sy = y + tileH - sh;
            tile(t.seconds[0], x, sy, sw, sh, secF); x += sw + tileGap;
            tile(t.seconds[1], x, sy, sw, sh, secF); x += sw;
        }
        if (!t.ampm.empty()) {
            x += 8 * s;
            const SizeF sz = TextSize(t.ampm.c_str(), D(12));
            Put(t.ampm, D(12), skin.date, x, y + tileH - sz.Height - 2 * s);
        }
        if (!dateLine.empty())
            Put(dateLine, D(12), skin.date, ox + padX + (contentW - dateSz.Width) / 2.0f,
                y + tileH + 8 * s);
    }
    return SizeF(contentW + padX * 2, contentH + padY * 2);
}

// Segments: an LED clock. The unlit segments show faintly, which is what
// makes it read as a display rather than as a font.
SizeF Style::Segments(float ox, float oy) const {
    const float padX = 18 * s, padY = 14 * s;
    const float digW = 26 * s, digH = 48 * s, thick = 5.2f * s;
    const float digGap = 12 * s, groupGap = 18 * s;
    const float secScale = 0.62f;

    std::wstring hh = t.hours;
    const bool leadingBlank = (hh.size() < 2);   // a 12-hour clock leaves the tens digit dark
    if (leadingBlank) hh = L" " + hh;

    float lineW = digW * 4 + digGap * 2 + groupGap;
    if (ctx.seconds) lineW += groupGap + digW * secScale * 2 + digGap * secScale;
    const Font* smallF = D(11);
    if (!t.ampm.empty()) lineW += 8 * s + TextSize(t.ampm.c_str(), smallF).Width;
    const std::wstring dateLine = Upper(DateLine(false));
    const SizeF dateSz = TextSize(dateLine.c_str(), smallF);
    const float contentW = (std::max)(lineW, dateSz.Width) + thick;   // the slant
    float contentH = digH + thick;
    if (!dateLine.empty()) contentH += 8 * s + dateSz.Height;

    if (draw) {
        const Color on  = Argb(skin.time, 255);
        const Color off = Argb(skin.dim, skin.bare ? 60 : 255);
        // A slight italic lean, as the real ones have: everything is drawn in
        // a sheared transform about the baseline.
        Matrix old;
        g->GetTransform(&old);
        const float y = oy + padY + thick / 2.0f;
        const float baseline = y + digH;
        Matrix shear;
        shear.Translate(0.0f, baseline);
        shear.Shear(-0.09f, 0.0f);
        shear.Translate(0.0f, -baseline);
        g->MultiplyTransform(&shear);

        auto digit = [&](wchar_t ch, float x, float yy, float w, float h, float th) {
            if (ch == L' ') {
                SegmentDigit(g, -1, x, yy, w, h, th, on, off);
                return;
            }
            SegmentDigit(g, ch - L'0', x, yy, w, h, th, on, off);
        };
        auto colon = [&](float x, float h, float th) {
            const float dot = th * 0.9f;
            SolidBrush b(Argb(skin.accent, 255));
            g->FillRectangle(&b, x - dot / 2, y + h * 0.30f - dot / 2, dot, dot);
            g->FillRectangle(&b, x - dot / 2, y + h * 0.70f - dot / 2, dot, dot);
        };

        float x = ox + padX + thick / 2.0f + (contentW - thick - lineW) / 2.0f;
        digit(hh[0], x, y, digW, digH, thick); x += digW + digGap;
        digit(hh[1], x, y, digW, digH, thick); x += digW + groupGap / 2.0f;
        colon(x, digH, thick);                 x += groupGap / 2.0f;
        digit(t.minutes[0], x, y, digW, digH, thick); x += digW + digGap;
        digit(t.minutes[1], x, y, digW, digH, thick); x += digW;
        if (ctx.seconds) {
            x += groupGap;
            const float sw = digW * secScale, sh = digH * secScale, st = thick * secScale;
            const float sy = y + digH - sh;
            digit(t.seconds[0], x, sy, sw, sh, st); x += sw + digGap * secScale;
            digit(t.seconds[1], x, sy, sw, sh, st); x += sw;
        }
        g->SetTransform(&old);

        if (!t.ampm.empty()) {
            x += 8 * s;
            const SizeF sz = TextSize(t.ampm.c_str(), smallF);
            Put(t.ampm, smallF, skin.date, x, baseline - sz.Height + 2 * s);
        }
        if (!dateLine.empty())
            Put(dateLine, smallF, skin.date,
                ox + padX + (contentW - dateSz.Width) / 2.0f, baseline + thick / 2.0f + 8 * s);
    }
    return SizeF(contentW + padX * 2, contentH + padY * 2);
}

// Stacked: hours above minutes at a size that fills a corner of the screen,
// with the weekday, day and month in a column beside them.
SizeF Style::Stacked(float ox, float oy) const {
    const float padX = 18 * s, padY = 10 * s;
    const Font* big = F(84, true);
    const Font* colF = D(13);
    const Font* dayF = F(28, true);

    std::wstring hh = t.hours;
    const SizeF hhSz = TextSize(hh.c_str(), big);
    const SizeF mmSz = TextSize(t.minutes.c_str(), big);
    const float lineH = hhSz.Height * 0.78f;         // the faces have generous leading
    const float digitsW = (std::max)(hhSz.Width, mmSz.Width);
    const float digitsH = lineH * 2 + hhSz.Height * 0.18f;

    // The side column.
    const bool column = ctx.date || ctx.weekday || ctx.seconds || !t.ampm.empty();
    const std::wstring wd = Upper(t.weekdayShort);
    const std::wstring mo = Upper(t.monthShort);
    const SizeF wdSz  = ctx.weekday ? TextSize(wd.c_str(), colF) : SizeF();
    const SizeF daySz = ctx.date ? TextSize(t.day.c_str(), dayF) : SizeF();
    const SizeF moSz  = ctx.date ? TextSize(mo.c_str(), colF) : SizeF();
    const SizeF secSz = ctx.seconds ? TextSize(t.seconds.c_str(), colF) : SizeF();
    const SizeF apSz  = t.ampm.empty() ? SizeF() : TextSize(t.ampm.c_str(), colF);
    float colW = 0.0f, colH = 0.0f;
    if (column) {
        colW = (std::max)((std::max)(wdSz.Width, daySz.Width),
                          (std::max)((std::max)(moSz.Width, secSz.Width), apSz.Width));
        if (ctx.weekday)     colH += wdSz.Height;
        if (ctx.date)        colH += daySz.Height * 0.82f + moSz.Height;
        if (ctx.seconds)     colH += secSz.Height + 4 * s;
        if (!t.ampm.empty()) colH += apSz.Height;
    }
    const float gap = column ? 14 * s : 0.0f;
    const float contentW = digitsW + gap + colW;
    const float contentH = (std::max)(digitsH, colH);

    if (draw) {
        const float x = ox + padX;
        const float y = oy + padY + (contentH - digitsH) / 2.0f - hhSz.Height * 0.12f;
        Put(hh, big, skin.time, x + (digitsW - hhSz.Width), y);
        Put(t.minutes, big, skin.time, x + (digitsW - mmSz.Width), y + lineH);

        // A hairline between the two rows, in the accent, which is what
        // stops "12" over "45" reading as "1245".
        Pen rule(Argb(skin.accent, 200), (std::max)(1.0f, 1.5f * s));
        const float ry = y + lineH + hhSz.Height * 0.11f;
        g->DrawLine(&rule, x, ry, x + digitsW, ry);

        if (column) {
            const float cx = x + digitsW + gap;
            float cy = oy + padY + (contentH - colH) / 2.0f;
            if (ctx.weekday) { Put(wd, colF, skin.date, cx, cy); cy += wdSz.Height; }
            if (ctx.date) {
                Put(t.day, dayF, skin.time, cx, cy - daySz.Height * 0.10f);
                cy += daySz.Height * 0.82f;
                Put(mo, colF, skin.date, cx, cy);
                cy += moSz.Height;
            }
            if (!t.ampm.empty()) { Put(t.ampm, colF, skin.date, cx, cy); cy += apSz.Height; }
            if (ctx.seconds) { cy += 4 * s; Put(t.seconds, colF, skin.accent, cx, cy); }
        }
    }
    return SizeF(contentW + padX * 2, contentH + padY * 2);
}

// Wide: one strip. The date in the quiet colour, the time in the loud one,
// the seconds in the accent, for the top or bottom edge of a screen.
SizeF Style::Wide(float ox, float oy) const {
    const float padX = 16 * s, padY = 6 * s;
    const Font* timeF = F(22, true);
    const Font* dateF = D(12);
    const Font* secF  = F(13);

    const std::wstring dateLine = Tracked(DateLine(false));
    const std::wstring main = TimeMain();
    const SizeF dateSz = TextSize(dateLine.c_str(), dateF);
    const SizeF mainSz = TextSize(main.c_str(), timeF);
    const SizeF secSz  = ctx.seconds ? TextSize(t.seconds.c_str(), secF) : SizeF();
    const SizeF apSz   = t.ampm.empty() ? SizeF() : TextSize(t.ampm.c_str(), secF);

    float w = mainSz.Width;
    if (!dateLine.empty()) w += dateSz.Width + 16 * s;
    if (ctx.seconds)       w += 5 * s + secSz.Width;
    if (!t.ampm.empty())   w += 5 * s + apSz.Width;
    const float h = mainSz.Height;

    if (draw) {
        float x = ox + padX;
        const float y = oy + padY;
        const float mid = y + h / 2.0f;
        if (!dateLine.empty()) {
            Put(dateLine, dateF, skin.date, x, mid - dateSz.Height / 2.0f);
            x += dateSz.Width + 16 * s;
            // A dot between the two halves.
            FillCircle(g, x - 8 * s, mid + 1 * s, 1.6f * s, Argb(skin.accent, 255));
        }
        Put(main, timeF, skin.time, x, y);
        x += mainSz.Width;
        const float baseY = y + h * 0.80f;
        if (ctx.seconds) {
            x += 5 * s;
            Put(t.seconds, secF, skin.accent, x, baseY - secSz.Height * 0.80f);
            x += secSz.Width;
        }
        if (!t.ampm.empty()) {
            x += 5 * s;
            Put(t.ampm, secF, skin.date, x, baseY - apSz.Height * 0.80f);
        }
    }
    return SizeF(w + padX * 2, h + padY * 2);
}

// Ring: the time inside a ring that fills once a minute (or once an hour,
// with the seconds off), so the panel shows motion without a second hand.
SizeF Style::Ring(float ox, float oy) const {
    const float pad = 10 * s;
    const float d = 156 * s, r = d / 2.0f, band = 7 * s;
    const float cx = ox + pad + r, cy = oy + pad + r;
    const Font* timeF = F(34);
    const Font* smallF = D(11);

    if (draw) {
        // The track, then the progress arc on top of it.
        Pen track(Argb(skin.dim, skin.bare ? 110 : 255), band);
        g->DrawEllipse(&track, cx - r + band / 2, cy - r + band / 2, d - band, d - band);
        const float sec = (float)ctx.time.wSecond;
        const float frac = ctx.seconds ? sec / 60.0f
                                       : ((float)ctx.time.wMinute + sec / 60.0f) / 60.0f;
        Pen arc(Argb(skin.accent, 255), band);
        arc.SetStartCap(LineCapRound);
        arc.SetEndCap(LineCapRound);
        if (frac > 0.002f)
            g->DrawArc(&arc, cx - r + band / 2, cy - r + band / 2, d - band, d - band,
                       -90.0f, 360.0f * frac);
        // A faint disc inside, so the numbers have something to sit on when
        // the panel is bare.
        if (skin.bare) FillCircle(g, cx, cy, r - band * 1.5f, Color(40, 0, 0, 0));

        const std::wstring main = TimeMain();
        const SizeF mainSz = TextSize(main.c_str(), timeF);
        const std::wstring dateLine = Upper(DateLine(false));
        const SizeF dateSz = TextSize(dateLine.c_str(), smallF);
        const SizeF apSz = t.ampm.empty() ? SizeF() : TextSize(t.ampm.c_str(), smallF);

        float blockH = mainSz.Height * 0.9f;
        if (!dateLine.empty()) blockH += dateSz.Height;
        if (!t.ampm.empty())   blockH += apSz.Height;
        float y = cy - blockH / 2.0f - mainSz.Height * 0.05f;
        Put(main, timeF, skin.time, cx - mainSz.Width / 2.0f, y);
        y += mainSz.Height * 0.9f;
        if (!t.ampm.empty()) {
            Put(t.ampm, smallF, skin.date, cx - apSz.Width / 2.0f, y);
            y += apSz.Height;
        }
        if (!dateLine.empty())
            Put(dateLine, smallF, skin.date, cx - dateSz.Width / 2.0f, y);
        if (ctx.seconds) {
            // The count, smallF, at the top of the ring where the arc starts.
            const SizeF sz = TextSize(t.seconds.c_str(), smallF);
            FillRound(g, RectF(cx - sz.Width / 2 - 4 * s, cy - r + band + 5 * s,
                               sz.Width + 8 * s, sz.Height + 2 * s), 3 * s,
                      Argb(skin.tile, skin.bare ? 120 : 255));
            Put(t.seconds, smallF, skin.accent, cx - sz.Width / 2.0f, cy - r + band + 6 * s);
        }
    }
    return SizeF(d + pad * 2, d + pad * 2);
}

// The panel chrome of this skin, as the shared painter wants it. Round styles
// ask for a corner radius the painter clamps to a circle.
PanelLook LookOf(const ClockSkin& skin, int style) {
    PanelLook look;
    look.panelTop    = skin.panelTop;
    look.panelBottom = skin.panelBottom;
    look.border      = skin.border;
    look.borderAlpha = skin.borderAlpha;
    look.gloss       = skin.gloss;
    look.radius      = skin.radius;
    look.bare        = skin.bare;
    if (style == CLOCK_STYLE_ANALOG || style == CLOCK_STYLE_RING) look.radius = 4096;
    return look;
}

std::wstring DateFormat(const SYSTEMTIME& st, const wchar_t* format) {
    wchar_t buf[128] = {};
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, format, buf, 128, nullptr))
        return L"";
    return buf;
}

} // namespace

// ---------------------------------------------------------------- public
ClockText ClockBuildText(const ClockPaintCtx& ctx) {
    ClockText t;
    const SYSTEMTIME& st = ctx.time;
    wchar_t buf[8];

    int h = st.wHour;
    if (!ctx.hours24) {
        t.ampm = (h >= 12) ? L"PM" : L"AM";
        h %= 12;
        if (h == 0) h = 12;
        swprintf_s(buf, L"%d", h);
    } else {
        swprintf_s(buf, L"%02d", h);
    }
    t.hours = buf;
    swprintf_s(buf, L"%02d", st.wMinute);
    t.minutes = buf;
    swprintf_s(buf, L"%02d", st.wSecond);
    t.seconds = buf;

    // The locale writes the date; the pieces are asked for separately so the
    // styles that stack or abbreviate can do it in the locale's words.
    t.weekdayLong  = DateFormat(st, L"dddd");
    t.weekdayShort = DateFormat(st, L"ddd");
    t.monthShort   = DateFormat(st, L"MMM");
    t.day          = DateFormat(st, L"d");
    t.dateLong     = DateFormat(st, L"dddd, d MMMM");
    t.dateShort    = DateFormat(st, L"ddd d MMM");
    return t;
}

bool ClockStyleShowsSeconds(int style, bool secondsOn) {
    // The ring fills with the seconds whether or not they are printed; every
    // other style only moves once a second when the seconds are on.
    if (style == CLOCK_STYLE_RING) return true;
    return secondsOn;
}

SIZE ClockMeasure(const ClockPaintCtx& ctx) {
    ClockPaintCtx local = ctx;
    if (!local.skin) local.skin = &ClockSkinAt(0);
    const ClockText text = ClockBuildText(local);
    Style style(local, text, Measurer(), false);
    const SizeF content = style.Run(0.0f, 0.0f);
    const int shadow = (int)(kPanelShadow * local.scale);
    SIZE size;
    size.cx = (int)(content.Width + 0.999f) + shadow * 2;
    size.cy = (int)(content.Height + 0.999f) + shadow * 2;
    return size;
}

void ClockDraw(Gdiplus::Graphics* g, int width, int height, const ClockPaintCtx& ctx) {
    ClockPaintCtx local = ctx;
    if (!local.skin) local.skin = &ClockSkinAt(0);
    const ClockSkin& skin = *local.skin;

    g->SetSmoothingMode(SmoothingModeAntiAlias);
    g->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g->SetPixelOffsetMode(PixelOffsetModeHalf);

    const float s = local.scale;
    const float shadow = (float)(int)(kPanelShadow * s);
    if ((float)width - shadow * 2 <= 2.0f || (float)height - shadow * 2 <= 2.0f) return;

    PaintPanel(g, LookOf(skin, local.style), width, height, s, local.alpha);

    g_textShadow = skin.bare;
    g_glow       = skin.glow;
    g_glowColour = skin.time;
    g_scale      = s;

    const ClockText text = ClockBuildText(local);
    // Centred in whatever the bitmap is: the window is sized from
    // ClockMeasure, so this is exact there, and the preview scales to fit.
    Style measure(local, text, g, false);
    const SizeF content = measure.Run(0.0f, 0.0f);
    Style painter(local, text, g, true);
    painter.Run(((float)width - content.Width) / 2.0f,
                ((float)height - content.Height) / 2.0f);

    g_textShadow = false;
    g_glow       = 0;
}

} // namespace awa
