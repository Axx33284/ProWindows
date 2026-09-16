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

// The shadow margin lives in the header now, shared with the clock.
constexpr int kShadow = kPanelShadow;

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
enum class Face { Small, Body, Bold, Number, Mono, MonoBold };
enum class VAlign { Top, Middle };

const FontFamily* UiFamily() {
    // Leaked on purpose: a static would be destroyed after GdiplusShutdown.
    static const FontFamily* family = new FontFamily(L"Segoe UI");
    return family;
}

// The OSD style is columns of text, and columns of text only line up in a
// monospace face - which is also what an in-game overlay looks like.
// Consolas ships with Windows; the fallback is for a machine that has had its
// fonts pruned.
const FontFamily* MonoFamily() {
    static const FontFamily* family = [] {
        FontFamily* f = new FontFamily(L"Consolas");
        if (f->IsAvailable()) return (const FontFamily*)f;
        delete f;
        return UiFamily();
    }();
    return family;
}

// Bare skins draw text straight onto the desktop, and white text on a white
// window is invisible. A dark copy one pixel down and right - the in-game OSD
// trick - keeps it readable on anything. Set by MonDraw for the frame, read by
// DrawStr; painting is UI-thread only, like every other static here.
bool g_textShadow = false;

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
        {  9.5f, FontStyleRegular },   // Mono
        {  9.5f, FontStyleBold    },   // MonoBold
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

    const FontFamily* family =
        (face == Face::Mono || face == Face::MonoBold) ? MonoFamily() : UiFamily();
    const Font* font = new Font(family, (float)key / 4.0f, spec.style, UnitPixel);
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

    // The shadow first, offset by a scaled pixel, so the text lands on top.
    const float off = g_textShadow ? (std::max)(1.0f, (float)(int)(1.0f * scale)) : 0.0f;
    SolidBrush shade(Color((BYTE)(alpha * 3 / 4), 0, 0, 0));

    if (maxWidth <= 0.0f) {
        if (off > 0.0f)
            g->DrawString(text, -1, font, PointF(x + off, y + off),
                          CachedFormat(hAlign, vAlign, false), &shade);
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
    if (off > 0.0f)
        g->DrawString(text, -1, font, RectF(left + off, top + off, maxWidth, h),
                      CachedFormat(hAlign, vAlign, true), &shade);
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
        case MON_STYLE_OSD:
            // Monospace columns - label, reading, detail - and room for the
            // sparkline RTSS draws beside a "text, graph" item.
            cell.cx = (ctx.vertical ? 204 : 176) + (ctx.graphs ? 46 : 0);
            cell.cy = 15 + (ctx.topApps ? 12 : 0);
            break;
        case MON_STYLE_HUD:
            // A device tag, up to three readings across, and the detail.
            cell.cx = ctx.vertical ? 262 : 214;
            cell.cy = 24 + (ctx.topApps ? 12 : 0);
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
        case MON_STYLE_OSD:     return 1;
        case MON_STYLE_HUD:     return 5;
        default:                return 8;
    }
}

// How much clear space the panel keeps around its cells. Ticker is a strip, so
// it gets almost none; everything else gets the usual margin.
int PadFor(const MonPaintCtx& ctx) {
    if (ctx.style == MON_STYLE_TICKER) return 7;
    if (ctx.style == MON_STYLE_OSD || ctx.style == MON_STYLE_HUD) return 9;
    return 12;
}

// ------------------------------------------------------------------ grouping
// Which device a metric belongs to, for the style that folds a device's
// readouts onto one line. Anything not part of a family is its own line.
int DeviceOf(int id) {
    switch (id) {
        case MON_CPU: case MON_CPUTEMP:                return 0;
        case MON_GPU: case MON_GPUTEMP: case MON_VRAM: return 1;
        default:                                       return 16 + id;
    }
}

// The rows of each cell, in cell order. One row per cell for every style but
// the HUD, which groups by device in the order the devices first appear - so
// the user's order still decides which line is first, and where a device's
// readouts sit within its line.
void GroupRows(const std::vector<MonRow>& rows, const MonPaintCtx& ctx,
               std::vector<std::vector<int>>* cells) {
    cells->clear();
    if (ctx.style != MON_STYLE_HUD) {
        for (size_t i = 0; i < rows.size(); ++i) cells->push_back({ (int)i });
        return;
    }
    std::vector<int> device;
    for (size_t i = 0; i < rows.size(); ++i) {
        const int d = DeviceOf(rows[i].id);
        size_t at = 0;
        while (at < device.size() && device[at] != d) ++at;
        if (at == device.size()) {
            device.push_back(d);
            cells->push_back({});
        }
        (*cells)[at].push_back((int)i);
    }
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

// The in-game on-screen display: one monospace line per
// readout, label in the readout's colour, the number beside it, the detail
// dimmer, everything in columns that line up down the panel because the face
// is fixed-pitch. With graphs on, a sparkline sits at the right of the line
// the way RTSS draws a "text, graph" item. Nothing else - no bars, no rings -
// because the whole point of the look is that it is text.
void DrawOsdCell(Graphics* g, const MonRow& row, const MonPaintCtx& ctx,
                 float x, float y, float w, float h) {
    const MonitorSkin& skin = *ctx.skin;
    const float s = ctx.scale;
    const bool have = row.metric->available;

    const float graphW = ctx.graphs ? 42 * s : 0.0f;
    const float labelW = 40 * s;
    const float valueW = 62 * s;
    const float textW  = w - graphW;

    DrawStr(g, row.shortLabel ? row.shortLabel : row.label, Face::MonoBold, s,
            row.color, 245, x, y, StringAlignmentNear, VAlign::Top, labelW);

    const COLORREF valueColor =
        (skin.value == kUseMetricColour) ? row.color : skin.value;
    DrawStr(g, Reading(*row.metric, false), Face::Mono, s,
            valueColor, have ? 255 : 120, x + labelW, y, StringAlignmentNear,
            VAlign::Top, valueW + 8 * s);

    if (!row.metric->detail.empty())
        DrawStr(g, row.metric->detail.c_str(), Face::Mono, s, skin.detail, 215,
                x + textW, y, StringAlignmentFar, VAlign::Top,
                (std::max)(10.0f, textW - labelW - valueW - 6 * s));

    if (ctx.topApps) {
        const std::wstring& app = row.metric->topApp;
        DrawStr(g, app.empty() ? L"-" : app.c_str(), Face::Mono, s,
                app.empty() ? skin.detail : row.color, app.empty() ? 120 : 200,
                x + labelW, y + 12 * s, StringAlignmentNear, VAlign::Top,
                textW - labelW);
    }

    if (ctx.graphs)
        DrawCurve(g, row, ctx, x + w - graphW + 4 * s, y + 2 * s, graphW - 4 * s,
                  y + 13 * s, false);
    (void)h;
}

// The HUD the benchmark videos use: one line per
// device, with everything known about it across the line - "GPU 24% 71°C
// 3.1 GB" - the device tag in its colour, the numbers big and white. The
// readouts on a line are the ones the user has switched on, in the order they
// have put them, so "GPU temperature first" is a drag away. With graphs on,
// a faint history of the first readout sits behind the line.
void DrawHudCell(Graphics* g, const std::vector<MonRow>& rows,
                 const std::vector<int>& members, const MonPaintCtx& ctx,
                 float x, float y, float w, float h) {
    if (members.empty()) return;
    const MonitorSkin& skin = *ctx.skin;
    const float s = ctx.scale;
    const MonRow& lead = rows[(size_t)members[0]];

    const float lineH = 24 * s;
    const float mid   = y + lineH / 2.0f;

    if (ctx.graphs) {
        // Under everything, and quieter than a real graph: it is a backdrop
        // to the numbers, not a chart to read.
        MonPaintCtx faint = ctx;
        MonitorSkin quiet = skin;
        quiet.graphFill = (BYTE)(skin.graphFill / 2);
        quiet.graphLine = (BYTE)(skin.graphLine / 3);
        faint.skin = &quiet;
        DrawCurve(g, lead, faint, x, y + 2 * s, w, y + lineH - 2 * s, false);
    }

    // The device: the family's name, not the first readout's, so a line that
    // starts with the GPU temperature still says GPU.
    const wchar_t* tag = lead.shortLabel ? lead.shortLabel : lead.label;
    switch (DeviceOf(lead.id)) {
        case 0: tag = L"CPU"; break;
        case 1: tag = L"GPU"; break;
        default: break;
    }
    const float tagW = 38 * s;
    DrawStr(g, tag, Face::Bold, s, lead.color, 255, x, mid,
            StringAlignmentNear, VAlign::Middle, tagW);

    // The readings share what is left after the tag and the detail. Three is
    // the most a device has, and three "24%  47%  69°" fit as columns; a line
    // with one reading has the room for the whole of it - "9.8 GB" rather
    // than "61%", "↓ 1.4 MB/s" rather than a number that means nothing alone.
    const float detailW = 64 * s;
    const size_t n      = (std::min)((size_t)3, members.size());
    const float avail   = w - tagW - detailW;
    const float colW    = (n >= 3) ? avail / 3.0f
                        : (n == 2) ? (std::min)(80.0f * s, avail / 2.0f)
                        :            avail;
    const bool compact  = n >= 2;
    float cx = x + tagW;
    for (size_t i = 0; i < n; ++i) {
        const MonRow& row = rows[(size_t)members[i]];
        const bool have = row.metric->available;
        const COLORREF valueColor =
            (skin.value == kUseMetricColour) ? row.color : skin.value;
        DrawStr(g, Reading(*row.metric, compact), Face::Number, s, valueColor,
                have ? 255 : 120, cx, mid, StringAlignmentNear, VAlign::Middle,
                colW - 4 * s);
        cx += colW;
    }

    // What the lead readout has to add - "of 31.6 GB", "4.2 GHz" - at the
    // right, dim, where it does not compete with the numbers.
    if (!lead.metric->detail.empty())
        DrawStr(g, lead.metric->detail.c_str(), Face::Small, s, skin.detail, 210,
                x + w, mid, StringAlignmentFar, VAlign::Middle,
                (std::max)(10.0f, x + w - cx));

    if (ctx.topApps) {
        const std::wstring& app = lead.metric->topApp;
        DrawStr(g, app.empty() ? L"-" : app.c_str(), Face::Small, s,
                app.empty() ? skin.detail : lead.color, app.empty() ? 120 : 210,
                x + tagW, y + lineH - 1 * s, StringAlignmentNear, VAlign::Top,
                w - tagW);
    }
    (void)h;
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
        case MON_STYLE_OSD:     DrawOsdCell(g, row, ctx, x, y, w, h);     break;
        default:                DrawRowsCell(g, row, ctx, x, y, w, h);    break;
    }
}

// A cell is one row in every style but the HUD, where it is a device.
void DrawCellRows(Graphics* g, const std::vector<MonRow>& rows,
                  const std::vector<int>& members, const MonPaintCtx& ctx,
                  float x, float y, float w, float h) {
    if (members.empty()) return;
    if (ctx.style == MON_STYLE_HUD) DrawHudCell(g, rows, members, ctx, x, y, w, h);
    else                            DrawCell(g, rows[(size_t)members[0]], ctx, x, y, w, h);
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
void PaintPanelChrome(Graphics* g, const PanelLook& look, const RectF& panel,
                      float radius, BYTE alpha, float spread) {
    g->SetSmoothingMode(SmoothingModeAntiAlias);
    PaintPanelShadow(g, panel, radius, alpha, spread);

    GraphicsPath path;
    AddRoundedPath(&path, panel, radius);

    // A vertical gradient rather than a flat fill, with a hairline of light
    // along the top edge so it reads as a raised piece of glass.
    LinearGradientBrush fill(
        PointF(0.0f, panel.Y), PointF(0.0f, panel.GetBottom() + 1.0f),
        Argb(look.panelTop, alpha), Argb(look.panelBottom, alpha));
    g->FillPath(&fill, &path);

    if (look.gloss > 0) {
        Pen gloss(Color((BYTE)(look.gloss * alpha / 255), 255, 255, 255), 1.0f);
        g->DrawArc(&gloss, panel.X + 1.0f, panel.Y + 1.0f, radius * 2, radius * 2,
                   200.0f, 70.0f);
        g->DrawLine(&gloss, panel.X + 1.0f + radius, panel.Y + 1.0f,
                    panel.GetRight() - 1.0f - radius, panel.Y + 1.0f);
        g->DrawArc(&gloss, panel.GetRight() - 1.0f - radius * 2, panel.Y + 1.0f,
                   radius * 2, radius * 2, 290.0f, 70.0f);
    }

    Pen pen(Argb(look.border, (BYTE)(alpha * look.borderAlpha / 255)), 1.0f);
    g->DrawPath(&pen, &path);
}

// ---------------------------------------------------------------- chrome cache
// None of the chrome changes between one frame and the next: the same size, the
// same look, the same opacity produce the same pixels every time. Only the
// readings move. Painting it fresh each frame made the shadow alone - nine
// stacked antialiased rounded rectangles the size of the whole panel - two
// thirds of the cost of a frame, and a frame is drawn thirty times a second for
// the length of every glide.
//
// So it is rendered once into a bitmap and blitted afterwards. Four entries:
// the monitor and the clock each have a live panel and a settings preview at a
// different size, and none of them should evict another on every repaint.
struct ChromeEntry {
    int    w = 0, h = 0;
    int    radius = 0;             // quarter-pixels
    int    spread = 0;
    BYTE   alpha  = 0;
    PanelLook look;
    Bitmap* bitmap = nullptr;
    unsigned long long used = 0;   // for the eviction below
};

bool SameLook(const PanelLook& a, const PanelLook& b) {
    return a.panelTop == b.panelTop && a.panelBottom == b.panelBottom &&
           a.border == b.border && a.borderAlpha == b.borderAlpha &&
           a.gloss == b.gloss && a.radius == b.radius && a.bare == b.bare;
}

Bitmap* PanelChrome(const PanelLook& look, int width, int height,
                    const RectF& panel, float radius, BYTE alpha, float spread) {
    static ChromeEntry cache[4];
    static unsigned long long tick = 0;
    ++tick;

    const int rKey = (int)(radius * 4.0f + 0.5f);
    const int sKey = (int)(spread * 4.0f + 0.5f);

    ChromeEntry* victim = &cache[0];
    for (ChromeEntry& e : cache) {
        if (e.bitmap && e.w == width && e.h == height && e.radius == rKey &&
            e.spread == sKey && e.alpha == alpha && SameLook(e.look, look)) {
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
        PaintPanelChrome(&cg, look, panel, radius, alpha, spread);
    }

    delete victim->bitmap;
    victim->w      = width;
    victim->h      = height;
    victim->radius = rKey;
    victim->spread = sKey;
    victim->alpha  = alpha;
    victim->look   = look;
    victim->bitmap = fresh;
    victim->used   = tick;
    return fresh;
}

PanelLook LookOf(const MonitorSkin& skin) {
    PanelLook look;
    look.panelTop    = skin.panelTop;
    look.panelBottom = skin.panelBottom;
    look.border      = skin.border;
    look.borderAlpha = skin.borderAlpha;
    look.gloss       = skin.gloss;
    look.radius      = skin.radius;
    look.bare        = skin.bare;
    return look;
}

} // namespace

// ------------------------------------------------------------------ public
void PaintPanel(Gdiplus::Graphics* g, const PanelLook& look, int width, int height,
                float scale, BYTE alpha) {
    const float radius = look.radius * scale;
    const float shadow = (float)(int)(kShadow * scale);
    const float panelW = (float)width - shadow * 2;
    const float panelH = (float)height - shadow * 2;
    if (panelW <= 2.0f || panelH <= 2.0f) return;
    const RectF panel(shadow + 0.5f, shadow + 0.5f, panelW - 1.0f, panelH - 1.0f);

    if (look.bare) {
        // No chrome - but not nothing. A layered window passes clicks through
        // any pixel whose alpha is zero, and a panel that can only be grabbed
        // by its letters cannot be dragged. One count of alpha over the whole
        // panel is invisible and enough to make it solid to the mouse.
        SolidBrush ghost(Color(1, 0, 0, 0));
        g->FillRectangle(&ghost, panel);
    } else if (Bitmap* chrome = PanelChrome(look, width, height, panel, radius,
                                            alpha, shadow)) {
        // Source-over rather than a straight copy: the settings page draws this
        // same panel onto an opaque device context under a transform, and a
        // copy would stamp the transparent margin over what is behind it.
        g->DrawImage(chrome, 0, 0, width, height);
    } else {
        PaintPanelChrome(g, look, panel, radius, alpha, shadow);
    }
}

namespace {

// Where cell `index` of `count` sits, before any drag is applied. The one
// formula the layout, the hit-test and the drag feedback all use, so they
// cannot disagree by a pixel.
RECT CellRectAt(int index, const MonPaintCtx& ctx, int panelW) {
    const float s   = ctx.scale;
    const int pad    = (int)(PadFor(ctx) * s);
    const int shadow = (int)(kShadow * s);
    const SIZE cell  = CellSize(ctx);
    const int gap    = (int)(GapFor(ctx) * s);
    const int cellH  = (int)(cell.cy * s);
    // Down a column every cell shares the panel's width; across a row they
    // keep their natural width so the panel grows instead of the cells
    // stretching.
    const int cellW  = ctx.vertical ? (panelW - pad * 2) : (int)(cell.cx * s);

    RECT r;
    if (ctx.vertical) {
        r.left = shadow + pad;
        r.top  = shadow + pad + index * (cellH + gap);
    } else {
        r.left = shadow + pad + index * (cellW + gap);
        r.top  = shadow + pad;
    }
    r.right  = r.left + cellW;
    r.bottom = r.top + cellH;
    return r;
}

SIZE MeasureCells(int cellCount, const MonPaintCtx& ctx) {
    const float s = ctx.scale;
    const int pad    = (int)(PadFor(ctx) * s);
    const int shadow = (int)(kShadow * s);

    SIZE size;
    if (cellCount <= 0) {
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
        size.cy = pad * 2 + cellH * cellCount + gap * (cellCount - 1);
    } else {
        size.cx = pad * 2 + cellW * cellCount + gap * (cellCount - 1);
        size.cy = cellH + pad * 2;
    }
    size.cx += shadow * 2;
    size.cy += shadow * 2;
    return size;
}

} // namespace

void MonLayout(const std::vector<MonRow>& rows, const MonPaintCtx& ctx,
               std::vector<MonCell>* out) {
    out->clear();
    std::vector<std::vector<int>> groups;
    GroupRows(rows, ctx, &groups);
    const SIZE size = MeasureCells((int)groups.size(), ctx);
    const int panelW = size.cx - (int)(kShadow * ctx.scale) * 2;
    for (size_t i = 0; i < groups.size(); ++i) {
        MonCell cell;
        cell.rect = CellRectAt((int)i, ctx, panelW);
        cell.rows = std::move(groups[i]);
        out->push_back(std::move(cell));
    }
}

SIZE MonMeasure(const std::vector<MonRow>& rows, const MonPaintCtx& ctx) {
    std::vector<std::vector<int>> groups;
    GroupRows(rows, ctx, &groups);
    return MeasureCells((int)groups.size(), ctx);
}

void MonDraw(Gdiplus::Graphics* g, int width, int height,
             const std::vector<MonRow>& rows, const MonPaintCtx& ctx) {
    const MonitorSkin& skin = ctx.skin ? *ctx.skin : MonitorSkinAt(0);
    MonPaintCtx local = ctx;
    local.skin = &skin;

    g->SetSmoothingMode(SmoothingModeAntiAlias);
    g->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g_textShadow = skin.bare;

    const float s = local.scale;
    const float shadow = (float)(int)(kShadow * s);

    // Everything below is drawn relative to the panel, which is inset from the
    // bitmap by the shadow margin.
    const float panelW = (float)width - shadow * 2;
    const float panelH = (float)height - shadow * 2;
    if (panelW <= 2.0f || panelH <= 2.0f) return;

    PaintPanel(g, LookOf(skin), width, height, s, local.alpha);

    if (rows.empty()) {
        DrawStr(g, L"Nothing selected", Face::Body, s, skin.detail, 255,
                width / 2.0f, height / 2.0f - 8 * s, StringAlignmentCenter);
        g_textShadow = false;
        return;
    }

    std::vector<std::vector<int>> groups;
    GroupRows(rows, local, &groups);
    const int count = (int)groups.size();

    const MonDrag& drag = local.drag;
    const bool lifted = drag.cell >= 0 && drag.cell < count && count > 1;

    // Which cell is drawn in each slot. With nothing lifted that is the
    // identity; with one lifted, the others close up around the slot it
    // would land in, which is left empty for it.
    std::vector<int> inSlot;
    inSlot.reserve((size_t)count);
    for (int i = 0; i < count; ++i) if (!lifted || i != drag.cell) inSlot.push_back(i);
    int hole = -1;
    if (lifted) {
        hole = (std::max)(0, (std::min)(count - 1, drag.slot));
        inSlot.insert(inSlot.begin() + hole, -1);
    }

    for (int slot = 0; slot < count; ++slot) {
        const RECT r = CellRectAt(slot, local, (int)panelW);
        const float x = (float)r.left, y = (float)r.top;
        const float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
        const int cell = inSlot[(size_t)slot];
        if (cell < 0) {
            // The hole: a dotted outline so the eye knows where the lifted
            // readout will drop, and nothing else, so it reads as empty.
            Pen dots(Argb(skin.label, 110), (std::max)(1.0f, 1.0f * s));
            dots.SetDashStyle(DashStyleDot);
            GraphicsPath path;
            AddRoundedPath(&path, RectF(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f),
                           (std::max)(2.0f, 5.0f * s));
            g->DrawPath(&dots, &path);
            continue;
        }
        DrawCellRows(g, rows, groups[(size_t)cell], local, x, y, w, h);
    }

    if (lifted) {
        // The lifted readout, last so it sits on top, where the pointer has
        // taken it. A backing in the panel's own colour lifts it off the
        // cells beneath, which is what makes it read as picked up rather
        // than painted over.
        RECT home = CellRectAt(0, local, (int)panelW);
        const int cellW = home.right - home.left, cellH = home.bottom - home.top;
        float x = (float)home.left, y = (float)home.top;
        if (local.vertical) y = (float)drag.pos; else x = (float)drag.pos;

        const RectF box(x - 3 * s, y - 3 * s, cellW + 6 * s, cellH + 6 * s);
        GraphicsPath path;
        AddRoundedPath(&path, box, (std::max)(3.0f, 6.0f * s));
        // Bare skins have no panel colour to lift with; a dark glass does the
        // same job on those.
        const COLORREF back = skin.bare ? RGB(20, 20, 24) : skin.panelTop;
        SolidBrush fill(Argb(back, skin.bare ? 200 : 245));
        g->FillPath(&fill, &path);
        const MonRow& lead = rows[(size_t)groups[(size_t)drag.cell][0]];
        Pen edge(Argb(lead.color, 200), (std::max)(1.0f, 1.5f * s));
        g->DrawPath(&edge, &path);

        DrawCellRows(g, rows, groups[(size_t)drag.cell], local, x, y,
                     (float)cellW, (float)cellH);
    }
    g_textShadow = false;
}

} // namespace awa
