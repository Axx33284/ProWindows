#include "monpaint.h"
#include <objidl.h>
// GDI+ headers use bare min/max, which NOMINMAX removes. Feed them the
// std:: versions rather than re-enabling the Windows macros.
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>

namespace awa {

namespace {

using namespace Gdiplus;

// The soft drop shadow is drawn inside the panel's own bitmap, so the bitmap
// has to be bigger than the panel by this much on every side. Unscaled, like
// every other number here.
constexpr int kShadow = 9;

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

// A rounded rectangle at a radius the caller chooses, for the places a pill
// would be too round to read.
void FillBar(Graphics* g, float x, float y, float w, float h,
             const Color& color, float radius) {
    if (w <= 0.4f || h <= 0.4f) return;
    SolidBrush brush(color);
    if (radius <= 0.5f) {
        g->FillRectangle(&brush, x, y, w, h);
        return;
    }
    GraphicsPath path;
    AddRoundedPath(&path, RectF(x, y, w, h), radius);
    g->FillPath(&brush, &path);
}

// A pill, or a plain rectangle once it gets too small to round.
void FillPill(Graphics* g, float x, float y, float w, float h, const Color& color) {
    if (w <= 0.4f || h <= 0.4f) return;
    SolidBrush brush(color);
    if (w <= h + 0.5f && h <= w + 0.5f && w < 3.0f) {
        g->FillRectangle(&brush, x, y, w, h);
        return;
    }
    GraphicsPath path;
    AddRoundedPath(&path, RectF(x, y, w, h), (std::min)(w, h) / 2.0f);
    g->FillPath(&brush, &path);
}

// Text is built at a size derived from the same scale as the geometry. The
// settings window's own fonts are fixed-size, so borrowing them here would
// leave the "Size" slider stretching the panel while the numbers stayed put.
enum class Face { Small, Body, Bold, Number };
enum class VAlign { Top, Middle };

const FontFamily* UiFamily() {
    // Leaked on purpose: a static would be destroyed after GdiplusShutdown.
    static const FontFamily* family = new FontFamily(L"Segoe UI");
    return family;
}

// ---------------------------------------------------------------- object cache
// Constructing a Gdiplus::Font is a font-family lookup and a device-context
// round trip, and the panel draws roughly thirty strings a frame at thirty
// frames a second while a reading glides. Building them fresh each time was by
// a wide margin the most expensive thing this file did - it dwarfed the actual
// rasterising. There are only ever a handful of distinct (face, size) pairs, so
// they are made once and kept.
//
// Everything here is leaked deliberately, for the same reason UiFamily is: GDI+
// objects must not be destroyed after GdiplusShutdown has run, and a function
// static would be.
const Font* CachedFont(Face face, float scale) {
    struct Spec { float points; INT style; };
    static const Spec kSpecs[] = {
        {  8.0f, FontStyleRegular },   // Small
        {  9.0f, FontStyleRegular },   // Body
        {  9.0f, FontStyleBold    },   // Bold
        { 14.0f, FontStyleBold    },   // Number
    };
    const Spec& spec = kSpecs[(int)face];

    // Points at 96 dpi, then scaled - UnitPixel keeps it independent of
    // whatever DPI the surface happens to claim. Quantised to a quarter of a
    // pixel so the slider does not mint a new font for every mouse position.
    const float size = spec.points * 1.3333f * (std::max)(0.35f, scale);
    const int   key  = (int)(size * 4.0f + 0.5f);

    struct Entry { int face; int key; const Font* font; };
    static std::vector<Entry>* cache = new std::vector<Entry>();

    for (const Entry& e : *cache)
        if (e.face == (int)face && e.key == key) return e.font;

    // The cache has a ceiling, because a caller can mint keys faster than they
    // are reused: every style, orientation and toggle on the settings page
    // produces a differently-scaled preview, which is already more combinations
    // than fit here.
    //
    // Past that ceiling the answer is to reuse the nearest size already held,
    // NOT to build one we cannot keep. Building it was a Gdiplus::Font leaked on
    // every call - thirty strings a frame, thirty frames a second, for the rest
    // of the session - and GDI objects run out. A quarter of a pixel of size
    // difference is invisible; that is not.
    constexpr size_t kMaxFonts = 64;
    if (cache->size() >= kMaxFonts) {
        const Font* best = nullptr;
        int bestDelta = 0;
        for (const Entry& e : *cache) {
            if (e.face != (int)face) continue;
            const int delta = (e.key > key) ? e.key - key : key - e.key;
            if (!best || delta < bestDelta) { best = e.font; bestDelta = delta; }
        }
        // Only when this face has nothing cached at all does a new one get
        // made, which can happen at most once per face.
        if (best) return best;
    }

    const Font* font = new Font(UiFamily(), (float)key / 4.0f, spec.style, UnitPixel);
    cache->push_back({ (int)face, key, font });
    return font;
}

// The same argument as the fonts, for rather less money but on the same path.
const StringFormat* CachedFormat(StringAlignment hAlign, VAlign vAlign, bool ellipsis) {
    int index = (int)hAlign * 4 + (int)(vAlign == VAlign::Middle) * 2 +
                (int)ellipsis;
    static const StringFormat* cache[12] = {};
    if (index < 0 || index >= 12) index = 0;
    if (!cache[index]) {
        StringFormat* fmt = new StringFormat();
        fmt->SetAlignment(hAlign);
        fmt->SetLineAlignment(vAlign == VAlign::Middle ? StringAlignmentCenter
                                                       : StringAlignmentNear);
        fmt->SetFormatFlags(StringFormatFlagsNoWrap);
        if (ellipsis) fmt->SetTrimming(StringTrimmingEllipsisCharacter);
        cache[index] = fmt;
    }
    return cache[index];
}

void DrawStr(Graphics* g, const wchar_t* text, Face face, float scale,
             COLORREF color, BYTE alpha, float x, float y,
             StringAlignment hAlign, VAlign vAlign = VAlign::Top,
             float maxWidth = 0.0f) {
    if (!text || !*text) return;

    const Font* font = CachedFont(face, scale);
    SolidBrush brush(Argb(color, alpha));

    if (maxWidth <= 0.0f) {
        g->DrawString(text, -1, font, PointF(x, y),
                      CachedFormat(hAlign, vAlign, false), &brush);
        return;
    }

    // Laid out in a box instead of at a point, so anything too long is cut
    // with an ellipsis rather than running into the next cell.
    const float h = font->GetSize() * 2.0f;
    float left = x;
    if (hAlign == StringAlignmentCenter)   left = x - maxWidth / 2.0f;
    else if (hAlign == StringAlignmentFar) left = x - maxWidth;
    const float top = (vAlign == VAlign::Middle) ? y - h / 2.0f : y;
    g->DrawString(text, -1, font, RectF(left, top, maxWidth, h),
                  CachedFormat(hAlign, vAlign, true), &brush);
}

// Gauge cells are too narrow for "12.4 GB"; the metric offers a compact form.
const wchar_t* Reading(const Metric& m, bool compact) {
    if (!m.available) return L"n/a";
    if (compact && !m.shortValue.empty()) return m.shortValue.c_str();
    return m.value.c_str();
}

float Clamp01(double v) {
    return (float)(std::max)(0.0, (std::min)(1.0, v / 100.0));
}

// What this row's meter paints with right now: its own colour until the reading
// climbs, then warmer. Text deliberately keeps the flat colour - see
// MonitorLoadColour.
Color MeterColour(const MonRow& row, const MonPaintCtx& ctx, BYTE alpha = 255) {
    const COLORREF c = MonitorLoadColour(*ctx.skin, row.color, row.percent);
    return Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

// ------------------------------------------------------------------ pieces
// The history curve, filled underneath and fading out downwards so stacked
// rows do not turn into blocks of colour.
void DrawCurve(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
               float x, float top, float w, float bottom, bool withHead) {
    if (!row.history || bottom <= top + 2.0f || w <= 2.0f) return;

    const MonitorSkin& skin = *ctx.skin;
    const Color tint(255, GetRValue(row.color), GetGValue(row.color),
                     GetBValue(row.color));

    const int count = (std::max)(2, (std::min)(kMonHistory, ctx.historyLen));
    const float* start = row.history + (kMonHistory - count);

    // Kept between calls rather than allocated per row per frame: this runs
    // eight times a frame, thirty times a second, for the life of the process.
    // Painting is UI-thread only, so one buffer is enough.
    static std::vector<PointF> line;
    static std::vector<PointF> area;
    line.clear();
    area.clear();
    line.reserve((size_t)count);
    area.reserve((size_t)count + 2);

    for (int p = 0; p < count; ++p) {
        const float t = (float)p / (float)(count - 1);
        // The newest point follows the eased display value, so the head of the
        // curve slides up to the new reading instead of jumping to it.
        const float raw = (p == count - 1) ? (float)(row.percent / 100.0) : start[p];
        const float v = (std::max)(0.0f, (std::min)(1.0f, raw));
        line.push_back(PointF(x + t * w, bottom - v * (bottom - top)));
    }

    area = line;
    area.push_back(PointF(x + w, bottom));
    area.push_back(PointF(x, bottom));

    LinearGradientBrush under(
        PointF(0.0f, top), PointF(0.0f, bottom + 1.0f),
        Color(skin.graphFill, tint.GetR(), tint.GetG(), tint.GetB()),
        Color(0, tint.GetR(), tint.GetG(), tint.GetB()));
    g->FillPolygon(&under, area.data(), (INT)area.size());

    // Where the highest point of this window was. Without it a graph that has
    // been flat for a minute and one that spiked thirty seconds ago look the
    // same once the spike has scrolled into the middle distance.
    {
        float peak = line[0].Y;
        for (int p = 1; p < count; ++p) peak = (std::min)(peak, line[(size_t)p].Y);
        if (peak > top + 1.0f && peak < bottom - 2.0f) {
            Pen mark(Color((BYTE)(skin.graphLine / 3), tint.GetR(), tint.GetG(),
                           tint.GetB()), 1.0f);
            mark.SetDashStyle(DashStyleDot);
            g->DrawLine(&mark, x, peak, x + w, peak);
        }
    }

    Pen pen(Color(skin.graphLine, tint.GetR(), tint.GetG(), tint.GetB()),
            (std::max)(1.0f, 1.5f * ctx.scale));
    pen.SetLineJoin(LineJoinRound);
    g->DrawLines(&pen, line.data(), count);

    if (withHead) {
        // A dot on the newest sample, so the eye finds "now" at a glance.
        const float dot = (std::max)(1.6f, 2.0f * ctx.scale);
        const PointF last = line[(size_t)count - 1];
        SolidBrush head(MeterColour(row, ctx));
        g->FillEllipse(&head, last.X - dot, last.Y - dot, dot * 2, dot * 2);
    }
}

// One segment per logical processor, each filled to its own level. At eight
// cores the segments are wide enough to read individually; past about thirty
// they stop being distinguishable, so cores are folded together in pairs (or
// fours, or eights) until they fit. A 128-thread machine still gets a bar that
// means something rather than a grey smear.
void DrawCoreBar(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
                 float x, float y, float w, float h,
                 const std::vector<double>& parts) {
    const MonitorSkin& skin = *ctx.skin;

    // Wide enough that the *level* inside a segment is readable, not just
    // whether the segment is there. Below about five pixels a partly-filled
    // segment and a full one look identical and the whole thing reads as a
    // dashed line.
    const float minSegment = (std::max)(5.0f, 6.0f * ctx.scale);
    const float gap        = (std::max)(1.0f, 1.0f * ctx.scale);

    size_t fold = 1;
    while (parts.size() / fold > 1 &&
           (w - gap * (float)(parts.size() / fold - 1)) / (float)(parts.size() / fold)
               < minSegment)
        fold *= 2;

    const size_t count = (parts.size() + fold - 1) / fold;
    if (count == 0) return;

    const float segment = (w - gap * (float)(count - 1)) / (float)count;
    if (segment < 1.0f) {                       // no room even folded: one bar
        FillPill(g, x, y, w * Clamp01(row.percent), h, MeterColour(row, ctx));
        return;
    }

    // Barely rounded rather than a pill: a segment this short becomes a circle
    // at the pill radius, and sixteen circles do not read as a bar.
    const float radius = (std::min)(1.5f, h / 3.0f);

    for (size_t i = 0; i < count; ++i) {
        // The busiest of the cores folded into this segment, not their mean:
        // the point of the display is to make a pinned thread visible, and
        // averaging it away is exactly what the single bar already did.
        double value = 0.0;
        for (size_t j = i * fold; j < (i + 1) * fold && j < parts.size(); ++j)
            value = (std::max)(value, parts[j]);

        // Each segment is filled whole, and its *brightness* is the level.
        // Filling part of a segment was the obvious first try and it does not
        // survive the size: at six pixels tall a rounded segment two pixels
        // into its fill is a dot, and sixteen dots is a dashed line. Brightness
        // stays readable all the way down, and it is what makes one pinned
        // core jump out of a row of idle ones - which is the whole point.
        const float left = x + (float)i * (segment + gap);
        const float level = (float)Clamp01(value);
        FillBar(g, left, y, segment, h, Argb(skin.track, skin.trackAlpha), radius);

        const COLORREF tint = MonitorLoadColour(skin, row.color, value);
        const BYTE alpha = (BYTE)(45.0f + 210.0f * level);
        FillBar(g, left, y, segment, h,
                Color(alpha, GetRValue(tint), GetGValue(tint), GetBValue(tint)),
                radius);
    }
}

// Whether this row's bar is going to be the segmented one, which is taller:
// a per-core bar has a level inside every segment and needs the height to
// show it, where a single bar only has to be visible.
bool WantsCoreBar(const MonRow& row, const MonPaintCtx& ctx) {
    return ctx.cores && row.metric && !row.metric->parts.empty();
}

void DrawTrackBar(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
                  float x, float y, float w, float h) {
    if (WantsCoreBar(row, ctx) && w > 24.0f) {
        DrawCoreBar(g, row, ctx, x, y, w, h, row.metric->parts);
        return;
    }

    const MonitorSkin& skin = *ctx.skin;
    FillPill(g, x, y, w, h, Argb(skin.track, skin.trackAlpha));

    const float filled = w * Clamp01(row.percent);
    FillPill(g, x, y, filled, h, MeterColour(row, ctx));
}

// A ring gauge: full circle track, arc drawn clockwise from twelve o'clock.
void DrawRing(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
              float cx, float cy, float radius, float thickness) {
    const MonitorSkin& skin = *ctx.skin;
    const RectF box(cx - radius, cy - radius, radius * 2, radius * 2);

    Pen track(Argb(skin.track, skin.trackAlpha), thickness);
    g->DrawArc(&track, box, 0.0f, 360.0f);

    const float sweep = 360.0f * Clamp01(row.percent);
    if (sweep <= 0.4f) return;

    Pen fill(MeterColour(row, ctx), thickness);
    fill.SetStartCap(LineCapRound);
    fill.SetEndCap(LineCapRound);
    g->DrawArc(&fill, box, -90.0f, sweep);
}

// A dial: 240 degrees of sweep with the opening at the bottom.
void DrawArcGauge(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
                  float cx, float cy, float radius, float thickness) {
    const MonitorSkin& skin = *ctx.skin;
    const RectF box(cx - radius, cy - radius, radius * 2, radius * 2);
    const float begin = 150.0f, span = 240.0f;

    Pen track(Argb(skin.track, skin.trackAlpha), thickness);
    track.SetStartCap(LineCapRound);
    track.SetEndCap(LineCapRound);
    g->DrawArc(&track, box, begin, span);

    const float sweep = span * Clamp01(row.percent);
    if (sweep <= 0.4f) return;

    Pen fill(MeterColour(row, ctx), thickness);
    fill.SetStartCap(LineCapRound);
    fill.SetEndCap(LineCapRound);
    g->DrawArc(&fill, box, begin, sweep);
}

// ------------------------------------------------------------------ layout
// Unscaled size of one metric's cell, before the panel padding is added.
SIZE CellSize(const MonPaintCtx& ctx) {
    SIZE cell = { 196, 52 };
    switch (ctx.style) {
        case MON_STYLE_COMPACT:
            cell.cx = ctx.vertical ? 190 : 150;
            cell.cy = 20;
            break;
        case MON_STYLE_RINGS:
            cell.cx = 84;
            cell.cy = ctx.topApps ? 104 : 90;
            break;
        case MON_STYLE_ARCS:
            cell.cx = 98;
            cell.cy = ctx.topApps ? 100 : 86;
            break;
        case MON_STYLE_BARS:
            cell.cx = 44;
            cell.cy = ctx.topApps ? 148 : 134;
            break;
        case MON_STYLE_GRAPH:
            cell.cx = ctx.vertical ? 228 : 176;
            cell.cy = ctx.topApps ? 84 : 70;
            break;
        case MON_STYLE_CARDS:
            cell.cx = ctx.vertical ? 208 : 156;
            cell.cy = (ctx.graphs ? 60 : 44) + (ctx.topApps ? 13 : 0);
            break;
        case MON_STYLE_TICKER:
            cell.cx = ctx.vertical ? 150 : 112;
            cell.cy = 18;
            break;
        default:                       // MON_STYLE_ROWS
            cell.cx = ctx.vertical ? 196 : 128;
            cell.cy = (ctx.graphs ? 52 : 30) + (ctx.topApps ? 13 : 0);
            break;
    }
    return cell;
}

int GapFor(const MonPaintCtx& ctx) {
    switch (ctx.style) {
        case MON_STYLE_COMPACT: return 4;
        case MON_STYLE_BARS:    return 2;
        // Laid out across, the gap is the only thing between one reading and
        // the next one's dot; two pixels ran them together.
        case MON_STYLE_TICKER:  return 16;
        case MON_STYLE_CARDS:   return 6;
        case MON_STYLE_RINGS:
        case MON_STYLE_ARCS:    return 4;
        default:                return 8;
    }
}

// How much clear space the panel keeps around its cells. Ticker is a strip, so
// it gets almost none; everything else gets the usual margin.
int PadFor(const MonPaintCtx& ctx) {
    return ctx.style == MON_STYLE_TICKER ? 7 : 12;
}

// ------------------------------------------------------------------ styles
void DrawRowsCell(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
                  float x, float y, float w, float h) {
    const MonitorSkin& skin = *ctx.skin;
    const float s = ctx.scale;
    const bool have = row.metric->available;

    DrawStr(g, row.label, Face::Small, s, skin.label, 235, x, y,
            StringAlignmentNear);

    const COLORREF valueColor =
        (skin.value == kUseMetricColour) ? row.color : skin.value;
    DrawStr(g, Reading(*row.metric, false), Face::Number, s,
            valueColor, have ? 255 : 120, x + w, y - 4 * s, StringAlignmentFar);

    if (!row.metric->detail.empty())
        DrawStr(g, row.metric->detail.c_str(), Face::Small, s, skin.detail,
                210, x, y + 13 * s, StringAlignmentNear, VAlign::Top, w * 0.62f);

    float bottom = y + h;
    if (ctx.topApps) {
        bottom -= 15 * s;
        const std::wstring& app = row.metric->topApp;
        DrawStr(g, app.empty() ? L"-" : app.c_str(), Face::Small, s,
                app.empty() ? skin.detail : row.color, app.empty() ? 120 : 225,
                x, bottom + 3 * s, StringAlignmentNear, VAlign::Top,
                w - 34 * s);
        DrawStr(g, app.empty() ? L"" : L"busiest", Face::Small, s,
                skin.detail, 150, x + w, bottom + 3 * s, StringAlignmentFar);
    }

    const float barH = WantsCoreBar(row, ctx) ? (std::max)(5.0f, 6.0f * s)
                                              : (std::max)(3.0f, 3.5f * s);
    const float barY = bottom - barH - 1 * s;

    if (ctx.graphs)
        DrawCurve(g, row, ctx, x, y + 20 * s, w, barY - 4 * s, true);

    DrawTrackBar(g, row, ctx, x, barY, w, barH);
}

void DrawCompactCell(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
                     float x, float y, float w, float h) {
    const MonitorSkin& skin = *ctx.skin;
    const float s = ctx.scale;
    const bool have = row.metric->available;

    const float labelW = 40 * s;
    const float valueW = 54 * s;
    const float barX   = x + labelW;
    const float barW   = (std::max)(10.0f, w - labelW - valueW);

    DrawStr(g, row.shortLabel ? row.shortLabel : row.label, Face::Small, s,
            skin.label, 235, x, y + 3 * s, StringAlignmentNear,
            VAlign::Top, labelW - 4 * s);

    DrawTrackBar(g, row, ctx, barX, y + h * 0.42f, barW,
                 (std::max)(4.0f, 5.0f * s));

    const COLORREF valueColor =
        (skin.value == kUseMetricColour) ? row.color : skin.value;
    DrawStr(g, Reading(*row.metric, true), Face::Bold, s,
            valueColor, have ? 255 : 120, x + w, y + 2 * s, StringAlignmentFar,
            VAlign::Top, valueW);
}

void DrawRingCell(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
                  float x, float y, float w, float h) {
    const MonitorSkin& skin = *ctx.skin;
    const float s = ctx.scale;
    const bool have = row.metric->available;

    const float radius = 29 * s;
    const float cx = x + w / 2.0f;
    const float cy = y + radius + 4 * s;

    DrawRing(g, row, ctx, cx, cy, radius, (std::max)(3.0f, 7.0f * s));

    const COLORREF valueColor =
        (skin.value == kUseMetricColour) ? row.color : skin.value;
    DrawStr(g, Reading(*row.metric, true), Face::Bold, s,
            valueColor, have ? 255 : 120, cx, cy, StringAlignmentCenter,
            VAlign::Middle, radius * 1.75f);

    DrawStr(g, row.shortLabel ? row.shortLabel : row.label, Face::Small, s,
            skin.label, 235, cx, cy + radius + 6 * s, StringAlignmentCenter,
            VAlign::Top, w - 4 * s);

    if (ctx.topApps) {
        const std::wstring& app = row.metric->topApp;
        DrawStr(g, app.empty() ? L"-" : app.c_str(), Face::Small, s,
                app.empty() ? skin.detail : row.color, app.empty() ? 120 : 220,
                cx, cy + radius + 19 * s, StringAlignmentCenter,
                VAlign::Top, w - 4 * s);
    }
    (void)h;
}

void DrawArcCell(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
                 float x, float y, float w, float h) {
    const MonitorSkin& skin = *ctx.skin;
    const float s = ctx.scale;
    const bool have = row.metric->available;

    const float radius = 32 * s;
    const float cx = x + w / 2.0f;
    const float cy = y + radius + 6 * s;

    DrawArcGauge(g, row, ctx, cx, cy, radius, (std::max)(3.0f, 8.0f * s));

    const COLORREF valueColor =
        (skin.value == kUseMetricColour) ? row.color : skin.value;
    DrawStr(g, Reading(*row.metric, true), Face::Number, s,
            valueColor, have ? 255 : 120, cx, cy - 2 * s, StringAlignmentCenter,
            VAlign::Middle, radius * 1.8f);

    DrawStr(g, row.shortLabel ? row.shortLabel : row.label, Face::Small, s,
            skin.label, 235, cx, cy + radius - 2 * s, StringAlignmentCenter,
            VAlign::Top, w - 4 * s);

    if (ctx.topApps) {
        const std::wstring& app = row.metric->topApp;
        DrawStr(g, app.empty() ? L"-" : app.c_str(), Face::Small, s,
                app.empty() ? skin.detail : row.color, app.empty() ? 120 : 220,
                cx, cy + radius + 11 * s, StringAlignmentCenter,
                VAlign::Top, w - 4 * s);
    }
    (void)h;
}

void DrawBarCell(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
                 float x, float y, float w, float h) {
    const MonitorSkin& skin = *ctx.skin;
    const float s = ctx.scale;
    const bool have = row.metric->available;

    const float labelH = 12 * s;
    const float appH   = ctx.topApps ? 12 * s : 0.0f;
    const float valueH = 14 * s;

    const float top    = y + valueH;
    const float bottom = y + h - labelH - appH;
    const float colW   = (std::max)(6.0f, 14.0f * s);
    const float cx     = x + w / 2.0f;

    const COLORREF valueColor =
        (skin.value == kUseMetricColour) ? row.color : skin.value;
    DrawStr(g, Reading(*row.metric, true), Face::Small, s,
            valueColor, have ? 255 : 120, cx, y, StringAlignmentCenter,
            VAlign::Top, w - 2 * s);

    if (bottom > top + 4.0f) {
        FillPill(g, cx - colW / 2.0f, top, colW, bottom - top,
                 Argb(skin.track, skin.trackAlpha));
        const float filled = (bottom - top) * Clamp01(row.percent);
        if (filled > 0.5f)
            FillPill(g, cx - colW / 2.0f, bottom - filled, colW, filled,
                     MeterColour(row, ctx));
    }

    DrawStr(g, row.shortLabel ? row.shortLabel : row.label, Face::Small, s,
            skin.label, 235, cx, bottom + 2 * s, StringAlignmentCenter,
            VAlign::Top, w - 2 * s);

    if (ctx.topApps) {
        const std::wstring& app = row.metric->topApp;
        DrawStr(g, app.empty() ? L"-" : app.c_str(), Face::Small, s,
                app.empty() ? skin.detail : row.color, app.empty() ? 120 : 220,
                cx, bottom + 2 * s + labelH, StringAlignmentCenter,
                VAlign::Top, w - 2 * s);
    }
}

void DrawGraphCell(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
                   float x, float y, float w, float h) {
    const MonitorSkin& skin = *ctx.skin;
    const float s = ctx.scale;
    const bool have = row.metric->available;

    float bottom = y + h;
    if (ctx.topApps) {
        bottom -= 12 * s;
        const std::wstring& app = row.metric->topApp;
        DrawStr(g, app.empty() ? L"-" : app.c_str(), Face::Small, s,
                app.empty() ? skin.detail : row.color, app.empty() ? 120 : 220,
                x, bottom, StringAlignmentNear, VAlign::Top, w * 0.6f);
    }

    // The graph fills the cell; the readings sit on top of it.
    DrawCurve(g, row, ctx, x, y + 16 * s, w, bottom - 1, true);

    Pen base(Argb(skin.track, skin.trackAlpha), (std::max)(1.0f, 1.0f * s));
    g->DrawLine(&base, x, bottom - 1, x + w, bottom - 1);

    DrawStr(g, row.label, Face::Small, s, skin.label, 235, x, y,
            StringAlignmentNear);

    const COLORREF valueColor =
        (skin.value == kUseMetricColour) ? row.color : skin.value;
    DrawStr(g, Reading(*row.metric, false), Face::Number, s,
            valueColor, have ? 255 : 120, x + w, y - 4 * s, StringAlignmentFar);
}

// Each metric on a raised card of its own, with a coloured rail down the left
// edge. The rail is what makes a column of these readable at a glance: the eye
// finds the colour first and the number second, which is the opposite of the
// row style and is the right way round when there are six or eight of them.
void DrawCardsCell(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
                   float x, float y, float w, float h) {
    const MonitorSkin& skin = *ctx.skin;
    const float s = ctx.scale;
    const bool have = row.metric->available;

    const float radius = (std::max)(3.0f, 7.0f * s);
    const RectF card(x, y, w, h);

    // The card sits a little lighter than the panel it is on. Derived from the
    // panel's own top colour rather than a fixed grey, so it works on the light
    // skins as well as the dark ones.
    const bool light = (GetRValue(skin.panelTop) + GetGValue(skin.panelTop) +
                        GetBValue(skin.panelTop)) > 384;
    {
        GraphicsPath path;
        AddRoundedPath(&path, card, radius);
        SolidBrush fill(Color(light ? 26 : 30, light ? 0 : 255,
                              light ? 0 : 255, light ? 0 : 255));
        g->FillPath(&fill, &path);
        Pen edge(Argb(skin.border, (BYTE)(skin.borderAlpha / 2)), 1.0f);
        g->DrawPath(&edge, &path);
    }

    // The rail. Clipped to the card so its rounded ends match the corner.
    {
        const float railW = (std::max)(2.0f, 3.0f * s);
        Region saved;
        g->GetClip(&saved);
        GraphicsPath clip;
        AddRoundedPath(&clip, card, radius);
        g->SetClip(&clip, CombineModeIntersect);
        SolidBrush rail(MeterColour(row, ctx, have ? 255 : 90));
        g->FillRectangle(&rail, x, y, railW, h);
        g->SetClip(&saved, CombineModeReplace);
    }

    const float left  = x + 11 * s;
    const float right = x + w - 9 * s;

    float bottom = y + h;
    if (ctx.topApps) {
        bottom -= 13 * s;
        const std::wstring& app = row.metric->topApp;
        DrawStr(g, app.empty() ? L"-" : app.c_str(), Face::Small, s,
                app.empty() ? skin.detail : row.color, app.empty() ? 110 : 215,
                left, bottom + 1 * s, StringAlignmentNear, VAlign::Top,
                right - left);
    }

    if (ctx.graphs)
        DrawCurve(g, row, ctx, left, y + 20 * s, right - left, bottom - 9 * s, true);

    DrawStr(g, row.label, Face::Small, s, skin.label, 230, left, y + 5 * s,
            StringAlignmentNear, VAlign::Top, (right - left) * 0.55f);

    const COLORREF valueColor =
        (skin.value == kUseMetricColour) ? row.color : skin.value;
    DrawStr(g, Reading(*row.metric, false), Face::Number, s,
            valueColor, have ? 255 : 120, right, y + 2 * s, StringAlignmentFar);

    if (!row.metric->detail.empty() && !ctx.graphs)
        DrawStr(g, row.metric->detail.c_str(), Face::Small, s, skin.detail, 200,
                left, y + 19 * s, StringAlignmentNear, VAlign::Top,
                right - left);

    const float barH = WantsCoreBar(row, ctx) ? (std::max)(5.0f, 6.0f * s)
                                              : (std::max)(2.5f, 3.0f * s);
    DrawTrackBar(g, row, ctx, left, bottom - barH - 4 * s, right - left, barH);
}

// One line, one metric: a filled dot that doubles as the gauge, the short name,
// and the number. Meant to be laid out across rather than down, parked along
// the top or bottom edge of a screen where a panel would be in the way.
void DrawTickerCell(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
                    float x, float y, float w, float h) {
    const MonitorSkin& skin = *ctx.skin;
    const float s = ctx.scale;
    const bool have = row.metric->available;

    const float mid = y + h / 2.0f;
    const float dot = (std::max)(2.0f, 3.5f * s);
    const float cx  = x + dot;              // fully inside the cell, not on its edge

    // The dot is a miniature gauge: hollow when idle, solid when the metric is
    // at its ceiling. At this size a bar would be unreadable and a ring would
    // be a smudge, but "how full is that dot" survives being three pixels wide.
    {
        SolidBrush track(Argb(skin.track, skin.trackAlpha));
        g->FillEllipse(&track, cx - dot, mid - dot, dot * 2, dot * 2);
        const float r = dot * (0.45f + 0.55f * Clamp01(row.percent));
        SolidBrush fill(MeterColour(row, ctx, have ? 255 : 90));
        g->FillEllipse(&fill, cx - r, mid - r, r * 2, r * 2);
    }

    const float labelX = cx + dot + 5 * s;
    const float labelW = (w - (labelX - x)) * 0.42f;
    DrawStr(g, row.shortLabel ? row.shortLabel : row.label, Face::Small, s,
            skin.label, 225, labelX, mid, StringAlignmentNear, VAlign::Middle,
            labelW);

    const COLORREF valueColor =
        (skin.value == kUseMetricColour) ? row.color : skin.value;
    DrawStr(g, Reading(*row.metric, true), Face::Bold, s,
            valueColor, have ? 255 : 120, x + w, mid, StringAlignmentFar,
            VAlign::Middle, w - (labelX - x) - labelW - 6 * s);
}

void DrawCell(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
              float x, float y, float w, float h) {
    switch (ctx.style) {
        case MON_STYLE_COMPACT: DrawCompactCell(g, row, ctx, x, y, w, h); break;
        case MON_STYLE_RINGS:   DrawRingCell(g, row, ctx, x, y, w, h);    break;
        case MON_STYLE_ARCS:    DrawArcCell(g, row, ctx, x, y, w, h);     break;
        case MON_STYLE_BARS:    DrawBarCell(g, row, ctx, x, y, w, h);     break;
        case MON_STYLE_GRAPH:   DrawGraphCell(g, row, ctx, x, y, w, h);   break;
        case MON_STYLE_CARDS:   DrawCardsCell(g, row, ctx, x, y, w, h);   break;
        case MON_STYLE_TICKER:  DrawTickerCell(g, row, ctx, x, y, w, h);  break;
        default:                DrawRowsCell(g, row, ctx, x, y, w, h);    break;
    }
}

// A soft shadow under the panel, drawn as a handful of concentric rounded
// strokes rather than a real blur: at this radius the two are indistinguishable
// and one of them costs a convolution over the whole bitmap every frame.
//
// It is what stops the panel reading as a rectangle painted onto the wallpaper.
void PaintPanelShadow(Graphics* g, const RectF& panel, float radius, BYTE alpha,
                      float spread) {
    if (spread < 1.0f || alpha < 24) return;

    const int steps = (std::max)(3, (std::min)(9, (int)(spread + 0.5f)));
    for (int i = steps; i >= 1; --i) {
        const float grow = spread * (float)i / (float)steps;
        // Quadratic falloff: dense against the panel, gone by the outer ring.
        const float t = 1.0f - (float)i / (float)(steps + 1);
        const BYTE  a = (BYTE)((std::min)(255.0f, 30.0f * t * t * (alpha / 255.0f)
                                          + 1.0f));
        if (a < 2) continue;

        GraphicsPath path;
        AddRoundedPath(&path, RectF(panel.X - grow, panel.Y - grow + grow * 0.35f,
                                    panel.Width + grow * 2, panel.Height + grow * 2),
                       radius + grow);
        SolidBrush brush(Color(a, 0, 0, 0));
        g->FillPath(&brush, &path);
    }
}

// The shadow, the panel fill, the gloss and the border, painted into a surface
// of their own.
void PaintPanelChrome(Graphics* g, const MonitorSkin& skin, const RectF& panel,
                      float radius, BYTE alpha, float spread) {
    g->SetSmoothingMode(SmoothingModeAntiAlias);
    PaintPanelShadow(g, panel, radius, alpha, spread);

    GraphicsPath path;
    AddRoundedPath(&path, panel, radius);

    // A vertical gradient rather than a flat fill, with a hairline of light
    // along the top edge so it reads as a raised piece of glass.
    LinearGradientBrush fill(
        PointF(0.0f, panel.Y), PointF(0.0f, panel.GetBottom() + 1.0f),
        Argb(skin.panelTop, alpha), Argb(skin.panelBottom, alpha));
    g->FillPath(&fill, &path);

    if (skin.gloss > 0) {
        Pen gloss(Color((BYTE)(skin.gloss * alpha / 255), 255, 255, 255), 1.0f);
        g->DrawArc(&gloss, panel.X + 1.0f, panel.Y + 1.0f, radius * 2, radius * 2,
                   200.0f, 70.0f);
        g->DrawLine(&gloss, panel.X + 1.0f + radius, panel.Y + 1.0f,
                    panel.GetRight() - 1.0f - radius, panel.Y + 1.0f);
        g->DrawArc(&gloss, panel.GetRight() - 1.0f - radius * 2, panel.Y + 1.0f,
                   radius * 2, radius * 2, 290.0f, 70.0f);
    }

    Pen pen(Argb(skin.border, (BYTE)(alpha * skin.borderAlpha / 255)), 1.0f);
    g->DrawPath(&pen, &path);
}

// ---------------------------------------------------------------- chrome cache
// None of the chrome changes between one frame and the next: the same size, the
// same skin, the same opacity produce the same pixels every time. Only the
// readings move. Painting it fresh each frame made the shadow alone - nine
// stacked antialiased rounded rectangles the size of the whole panel - two
// thirds of the cost of a frame, and a frame is drawn thirty times a second for
// the length of every glide.
//
// So it is rendered once into a bitmap and blitted afterwards. Two entries,
// because the settings page's live preview draws through this same painter at a
// different size and must not evict the overlay's copy on every repaint.
struct ChromeEntry {
    int    w = 0, h = 0;
    int    radius = 0;             // quarter-pixels
    int    spread = 0;
    BYTE   alpha  = 0;
    const  MonitorSkin* skin = nullptr;
    Bitmap* bitmap = nullptr;
    unsigned long long used = 0;   // for the two-entry eviction below
};

Bitmap* PanelChrome(const MonitorSkin& skin, int width, int height,
                    const RectF& panel, float radius, BYTE alpha, float spread) {
    static ChromeEntry cache[2];
    static unsigned long long tick = 0;
    ++tick;

    const int rKey = (int)(radius * 4.0f + 0.5f);
    const int sKey = (int)(spread * 4.0f + 0.5f);

    ChromeEntry* victim = &cache[0];
    for (ChromeEntry& e : cache) {
        if (e.bitmap && e.w == width && e.h == height && e.radius == rKey &&
            e.spread == sKey && e.alpha == alpha && e.skin == &skin) {
            e.used = tick;
            return e.bitmap;
        }
        if (e.used < victim->used) victim = &e;
    }

    Bitmap* fresh = new Bitmap(width, height, PixelFormat32bppPARGB);
    if (fresh->GetLastStatus() != Ok) { delete fresh; return nullptr; }
    {
        Graphics cg(fresh);
        cg.Clear(Color(0, 0, 0, 0));
        PaintPanelChrome(&cg, skin, panel, radius, alpha, spread);
    }

    delete victim->bitmap;
    victim->w      = width;
    victim->h      = height;
    victim->radius = rKey;
    victim->spread = sKey;
    victim->alpha  = alpha;
    victim->skin   = &skin;
    victim->bitmap = fresh;
    victim->used   = tick;
    return fresh;
}

} // namespace

// ------------------------------------------------------------------ public
SIZE MonMeasure(int rowCount, const MonPaintCtx& ctx) {
    const float s = ctx.scale;
    const int pad    = (int)(PadFor(ctx) * s);
    const int shadow = (int)(kShadow * s);

    SIZE size;
    if (rowCount <= 0) {
        size.cx = (int)(150 * s) + shadow * 2;
        size.cy = (int)(56 * s) + shadow * 2;
        return size;
    }

    const SIZE cell = CellSize(ctx);
    const int gap   = (int)(GapFor(ctx) * s);
    const int cellW = (int)(cell.cx * s);
    const int cellH = (int)(cell.cy * s);

    if (ctx.vertical) {
        size.cx = cellW + pad * 2;
        size.cy = pad * 2 + cellH * rowCount + gap * (rowCount - 1);
    } else {
        size.cx = pad * 2 + cellW * rowCount + gap * (rowCount - 1);
        size.cy = cellH + pad * 2;
    }
    size.cx += shadow * 2;
    size.cy += shadow * 2;
    return size;
}

void MonDraw(Gdiplus::Graphics* g, int width, int height,
             const std::vector<MonRow>& rows, const MonPaintCtx& ctx) {
    const MonitorSkin& skin = ctx.skin ? *ctx.skin : MonitorSkinAt(0);
    MonPaintCtx local = ctx;
    local.skin = &skin;

    g->SetSmoothingMode(SmoothingModeAntiAlias);
    g->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    const float s = local.scale;
    const float radius = skin.radius * s;
    const int   pad    = (int)(PadFor(local) * s);
    const float shadow = (float)(int)(kShadow * s);

    // Everything below is drawn relative to the panel, which is inset from the
    // bitmap by the shadow margin.
    const float panelW = (float)width - shadow * 2;
    const float panelH = (float)height - shadow * 2;
    if (panelW <= 2.0f || panelH <= 2.0f) return;
    const RectF panel(shadow + 0.5f, shadow + 0.5f, panelW - 1.0f, panelH - 1.0f);

    if (Bitmap* chrome = PanelChrome(skin, width, height, panel, radius,
                                     local.alpha, shadow)) {
        // Source-over rather than a straight copy: the settings page draws this
        // same panel onto an opaque device context under a transform, and a
        // copy would stamp the transparent margin over what is behind it.
        g->DrawImage(chrome, 0, 0, width, height);
    } else {
        PaintPanelChrome(g, skin, panel, radius, local.alpha, shadow);
    }

    if (rows.empty()) {
        DrawStr(g, L"Nothing selected", Face::Body, s, skin.detail, 255,
                width / 2.0f, height / 2.0f - 8 * s, StringAlignmentCenter);
        return;
    }

    const SIZE cell = CellSize(local);
    const int gap   = (int)(GapFor(local) * s);
    const int cellH = (int)(cell.cy * s);
    // Down a column every cell shares the panel's width; across a row they keep
    // their natural width so the panel grows instead of the cells stretching.
    const int cellW = local.vertical ? ((int)panelW - pad * 2) : (int)(cell.cx * s);

    for (size_t i = 0; i < rows.size(); ++i) {
        const float x = shadow + (local.vertical
                            ? (float)pad
                            : (float)(pad + (int)i * (cellW + gap)));
        const float y = shadow + (local.vertical
                            ? (float)(pad + (int)i * (cellH + gap))
                            : (float)pad);
        DrawCell(g, rows[i], local, x, y, (float)cellW, (float)cellH);
    }
}

} // namespace awa
