#include "montheme.h"

namespace awa {

namespace {

// Kept in one table so adding a skin is a single edit. Index 0 is the default
// and must stay first; everything else is only referenced by `id`.
// Metric colours are in MonMetric order: cpu, ram, gpu, vram, cpu temp,
// gpu temp, disk, net.
const MonitorSkin kSkins[] = {
{
    L"midnight", L"Midnight",
    L"The default. Near-black glass with a colour per metric.",
    RGB(26, 27, 33), RGB(18, 19, 23), RGB(70, 74, 84), 180, 26,
    RGB(150, 155, 165), RGB(132, 137, 148), kUseMetricColour,
    RGB(255, 255, 255), 70, 52, 225, 12,
    true,   // load tint
    { RGB(88, 140, 255), RGB(76, 200, 130), RGB(167, 139, 250),
      RGB(196, 167, 255), RGB(255, 138, 101), RGB(255, 183, 77),
      RGB(96, 205, 255), RGB(244, 143, 177) }
},
{
    L"graphite", L"Graphite",
    L"No colour at all - the readouts are told apart by weight, not hue.",
    RGB(38, 39, 43), RGB(28, 29, 32), RGB(96, 100, 108), 170, 30,
    RGB(158, 162, 170), RGB(136, 140, 148), RGB(238, 240, 244),
    RGB(255, 255, 255), 60, 40, 210, 10,
    false,   // load tint
    { RGB(238, 240, 244), RGB(212, 215, 221), RGB(186, 190, 198),
      RGB(164, 168, 177), RGB(226, 229, 234), RGB(198, 202, 210),
      RGB(206, 210, 217), RGB(178, 182, 191) }
},
{
    L"nord", L"Nord",
    L"The cool blue-grey palette, with muted arctic accents.",
    RGB(59, 66, 82), RGB(46, 52, 64), RGB(94, 106, 130), 190, 34,
    RGB(180, 190, 206), RGB(150, 161, 178), kUseMetricColour,
    RGB(216, 222, 233), 60, 58, 225, 10,
    true,   // load tint
    { RGB(136, 192, 208), RGB(163, 190, 140), RGB(180, 142, 173),
      RGB(94, 129, 172), RGB(191, 97, 106), RGB(208, 135, 112),
      RGB(129, 161, 193), RGB(235, 203, 139) }
},
{
    L"dracula", L"Dracula",
    L"Deep violet with the high-contrast Dracula accents.",
    RGB(48, 50, 66), RGB(38, 40, 52), RGB(98, 114, 164), 190, 30,
    RGB(160, 166, 192), RGB(140, 146, 172), kUseMetricColour,
    RGB(248, 248, 242), 55, 58, 230, 12,
    true,   // load tint
    { RGB(139, 233, 253), RGB(80, 250, 123), RGB(255, 121, 198),
      RGB(189, 147, 249), RGB(255, 85, 85), RGB(241, 250, 140),
      RGB(98, 214, 255), RGB(255, 184, 108) }
},
{
    L"solarized", L"Solarized",
    L"The low-contrast teal base with Solarized's accent ramp.",
    RGB(7, 54, 66), RGB(0, 43, 54), RGB(88, 110, 117), 200, 22,
    RGB(131, 148, 150), RGB(101, 123, 131), kUseMetricColour,
    RGB(147, 161, 161), 55, 60, 225, 8,
    true,   // load tint
    { RGB(38, 139, 210), RGB(133, 153, 0), RGB(211, 54, 130),
      RGB(108, 113, 196), RGB(220, 50, 47), RGB(203, 75, 22),
      RGB(42, 161, 152), RGB(181, 137, 0) }
},
{
    L"gruvbox", L"Gruvbox",
    L"Warm retro browns with earthy, slightly faded accents.",
    RGB(60, 56, 54), RGB(40, 40, 40), RGB(102, 92, 84), 200, 26,
    RGB(168, 153, 132), RGB(146, 131, 116), kUseMetricColour,
    RGB(235, 219, 178), 55, 58, 225, 8,
    true,   // load tint
    { RGB(131, 165, 152), RGB(184, 187, 38), RGB(211, 134, 155),
      RGB(232, 161, 180), RGB(251, 73, 52), RGB(254, 128, 25),
      RGB(142, 192, 124), RGB(250, 189, 47) }
},
{
    L"ocean", L"Ocean",
    L"Deep navy with cyan and aqua readouts.",
    RGB(18, 34, 52), RGB(11, 22, 36), RGB(52, 86, 122), 190, 30,
    RGB(139, 168, 196), RGB(116, 145, 175), kUseMetricColour,
    RGB(198, 226, 245), 60, 62, 230, 12,
    true,   // load tint
    { RGB(56, 189, 248), RGB(45, 212, 191), RGB(129, 140, 248),
      RGB(167, 139, 250), RGB(251, 146, 60), RGB(248, 113, 113),
      RGB(103, 232, 249), RGB(94, 234, 212) }
},
{
    L"ember", L"Ember",
    L"Charcoal warmed by orange and rose, easy on a dark desktop.",
    RGB(38, 30, 30), RGB(26, 20, 21), RGB(96, 72, 68), 190, 26,
    RGB(178, 152, 146), RGB(154, 130, 125), kUseMetricColour,
    RGB(255, 236, 224), 55, 56, 228, 12,
    true,   // load tint
    { RGB(251, 146, 60), RGB(248, 113, 113), RGB(244, 114, 182),
      RGB(251, 191, 36), RGB(239, 68, 68), RGB(255, 214, 102),
      RGB(253, 186, 116), RGB(252, 165, 165) }
},
{
    L"terminal", L"Terminal",
    L"One green on black, like a phosphor screen. No colour coding.",
    RGB(8, 14, 9), RGB(4, 8, 5), RGB(38, 92, 48), 210, 0,
    RGB(78, 158, 92), RGB(64, 132, 76), RGB(120, 255, 140),
    RGB(120, 255, 140), 45, 44, 235, 4,
    false,   // load tint
    { RGB(120, 255, 140), RGB(120, 255, 140), RGB(120, 255, 140),
      RGB(120, 255, 140), RGB(120, 255, 140), RGB(120, 255, 140),
      RGB(120, 255, 140), RGB(120, 255, 140) }
},
{
    L"amber", L"Amber",
    L"The other old terminal: amber on near-black, square corners.",
    RGB(20, 13, 4), RGB(12, 8, 2), RGB(112, 72, 20), 210, 0,
    RGB(186, 128, 48), RGB(158, 108, 42), RGB(255, 183, 77),
    RGB(255, 183, 77), 45, 44, 235, 4,
    false,   // load tint
    { RGB(255, 183, 77), RGB(255, 183, 77), RGB(255, 183, 77),
      RGB(255, 183, 77), RGB(255, 183, 77), RGB(255, 183, 77),
      RGB(255, 183, 77), RGB(255, 183, 77) }
},
{
    L"neon", L"Neon",
    L"Loud. Saturated cyan and magenta on near-black, big corners.",
    RGB(18, 10, 32), RGB(8, 6, 18), RGB(140, 48, 200), 200, 44,
    RGB(158, 128, 208), RGB(136, 110, 184), kUseMetricColour,
    RGB(255, 0, 193), 45, 74, 240, 14,
    true,   // load tint
    { RGB(0, 229, 255), RGB(0, 255, 163), RGB(255, 0, 193),
      RGB(124, 77, 255), RGB(255, 64, 129), RGB(255, 110, 64),
      RGB(64, 255, 218), RGB(255, 214, 0) }
},
{
    L"frost", L"Frost",
    L"A light panel for light wallpapers - dark text, saturated bars.",
    RGB(250, 252, 255), RGB(234, 240, 248), RGB(178, 191, 209), 210, 0,
    RGB(96, 106, 124), RGB(120, 130, 148), kUseMetricColour,
    RGB(30, 41, 59), 45, 46, 235, 12,
    true,   // load tint
    { RGB(37, 99, 235), RGB(22, 163, 74), RGB(124, 58, 237),
      RGB(147, 51, 234), RGB(220, 38, 38), RGB(234, 88, 12),
      RGB(8, 145, 178), RGB(219, 39, 119) }
},
{
    L"paper", L"Paper",
    L"Light and deliberately quiet: ink on off-white, muted accents.",
    RGB(253, 252, 249), RGB(243, 241, 235), RGB(210, 205, 194), 210, 0,
    RGB(112, 107, 98), RGB(140, 135, 125), kUseMetricColour,
    RGB(45, 42, 38), 38, 40, 220, 6,
    true,   // load tint
    { RGB(58, 72, 94), RGB(84, 108, 78), RGB(112, 82, 108),
      RGB(96, 86, 120), RGB(150, 70, 60), RGB(176, 96, 72),
      RGB(66, 96, 110), RGB(146, 106, 58) }
},
{
    L"tokyonight", L"Tokyo Night",
    L"Deep indigo with the electric blues and purples of the editor theme.",
    RGB(36, 40, 59), RGB(26, 27, 38), RGB(65, 72, 104), 195, 32,
    RGB(169, 177, 214), RGB(134, 142, 178), kUseMetricColour,
    RGB(192, 202, 245), 58, 60, 230, 12,
    true,   // load tint
    { RGB(122, 162, 247), RGB(158, 206, 106), RGB(187, 154, 247),
      RGB(157, 124, 216), RGB(247, 118, 142), RGB(255, 158, 100),
      RGB(125, 207, 255), RGB(224, 175, 104) }
},
{
    L"catppuccin", L"Catppuccin",
    L"Soft pastels on warm charcoal - the quietest of the colour schemes.",
    RGB(49, 50, 68), RGB(30, 30, 46), RGB(88, 91, 112), 195, 28,
    RGB(186, 194, 222), RGB(147, 153, 178), kUseMetricColour,
    RGB(205, 214, 244), 55, 54, 225, 14,
    true,   // load tint
    { RGB(137, 180, 250), RGB(166, 227, 161), RGB(203, 166, 247),
      RGB(245, 194, 231), RGB(243, 139, 168), RGB(250, 179, 135),
      RGB(148, 226, 213), RGB(249, 226, 175) }
},
{
    L"rosepine", L"Rose Pine",
    L"Muted rose and pine on a dusk background. Low contrast on purpose.",
    RGB(38, 35, 58), RGB(25, 23, 36), RGB(82, 79, 103), 195, 26,
    RGB(144, 140, 170), RGB(110, 106, 134), kUseMetricColour,
    RGB(224, 222, 244), 52, 52, 222, 12,
    true,   // load tint
    { RGB(156, 207, 216), RGB(49, 116, 143), RGB(196, 167, 231),
      RGB(192, 143, 190), RGB(235, 111, 146), RGB(246, 193, 119),
      RGB(156, 207, 216), RGB(235, 188, 186) }
},
// The two bare skins have no panel: they are the look of an in-game overlay,
// text straight on the screen with a shadow under it. They are meant for the
// OSD and HUD styles and work with every other style too, just without the
// glass. The panel colours are still filled in for the settings-page swatches
// and the fallback painter.
{
    L"afterburner", L"Overlay",
    L"No panel. Orange text straight on the screen, the in-game OSD look.",
    RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), 0, 0,
    RGB(255, 160, 0), RGB(214, 134, 0), RGB(255, 160, 0),
    RGB(255, 160, 0), 60, 40, 235, 0,
    false,   // load tint: one hue, like the real thing
    { RGB(255, 160, 0), RGB(255, 160, 0), RGB(255, 160, 0),
      RGB(255, 160, 0), RGB(255, 160, 0), RGB(255, 160, 0),
      RGB(255, 160, 0), RGB(255, 160, 0) },
    true,    // bare
},
{
    L"benchmark", L"Benchmark",
    L"No panel. Green GPU, blue CPU, white numbers - the benchmark-video look.",
    RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), 0, 0,
    RGB(225, 225, 225), RGB(170, 170, 170), RGB(255, 255, 255),
    RGB(255, 255, 255), 60, 44, 235, 0,
    true,    // load tint
    { RGB(0, 190, 255), RGB(235, 235, 235), RGB(118, 210, 0),
      RGB(118, 210, 0), RGB(0, 190, 255), RGB(118, 210, 0),
      RGB(255, 200, 60), RGB(255, 120, 200) },
    true,    // bare
},
};

constexpr int kSkinCount = (int)(sizeof(kSkins) / sizeof(kSkins[0]));

// Order must match the MonStyle enum.
const MonitorStyleInfo kStyles[] = {
    { L"rows", L"Rows",
      L"The default. A row each: name, reading, history graph and a bar." },
    { L"compact", L"Compact",
      L"One dense line per metric - name, bar, reading. The smallest panel." },
    { L"rings", L"Rings",
      L"A circular gauge per metric with the reading in the middle." },
    { L"arcs", L"Arcs",
      L"Dial gauges with a needle-style sweep and the name underneath." },
    { L"bars", L"Bars",
      L"Vertical column meters standing side by side, like a mixing desk." },
    { L"graph", L"Graph",
      L"Chart first: a large history graph per metric, reading overlaid." },
    { L"cards", L"Cards",
      L"Each metric on its own raised card, with a coloured rail and a graph." },
    { L"ticker", L"Ticker",
      L"A single thin line - a dot, a name and a number each. Sits on an edge." },
    { L"osd", L"OSD",
      L"An in-game on-screen display: monospace text, one line each." },
    { L"hud", L"HUD",
      L"The benchmark-video HUD: one line per device, all its readings across." },
};

constexpr int kStyleCount = (int)(sizeof(kStyles) / sizeof(kStyles[0]));
static_assert(kStyleCount == MON_STYLE_COUNT, "style table must match MonStyle");

} // namespace

namespace {

// In MonMetric order, which is also the order the overlay uses until the user
// says otherwise. The `id` column is what reaches config.ini, so these strings
// are as load-bearing as the skin ids: renaming one silently drops a setting.
const MonitorMetricInfo kMetrics[MON_METRIC_COUNT] = {
    { L"cpu",      L"CPU",        L"CPU",  L"CPU"             },
    { L"ram",      L"MEMORY",     L"RAM",  L"Memory"          },
    { L"gpu",      L"GPU",        L"GPU",  L"GPU"             },
    { L"vram",     L"GPU MEMORY", L"VRAM", L"GPU memory"      },
    { L"cpu_temp", L"CPU TEMP",   L"CPU°", L"CPU temperature" },
    { L"gpu_temp", L"GPU TEMP",   L"GPU°", L"GPU temperature" },
    { L"disk",     L"DISK",       L"DISK", L"Disk"            },
    { L"net",      L"NETWORK",    L"NET",  L"Network"         },
};

} // namespace

const MonitorMetricInfo& MonitorMetricAt(int metric) {
    if (metric < 0 || metric >= MON_METRIC_COUNT) metric = 0;
    return kMetrics[metric];
}

int MonitorMetricIndexById(const std::wstring& id) {
    for (int i = 0; i < MON_METRIC_COUNT; ++i)
        if (_wcsicmp(kMetrics[i].id, id.c_str()) == 0) return i;
    // There used to be one temperature row; it is the CPU one now, so a config
    // written before the split keeps its position rather than losing it.
    if (_wcsicmp(id.c_str(), L"temp") == 0) return MON_CPUTEMP;
    return -1;
}

void MonitorNormaliseOrder(int order[MON_METRIC_COUNT]) {
    bool seen[MON_METRIC_COUNT] = {};
    int  clean[MON_METRIC_COUNT];
    int  n = 0;

    for (int i = 0; i < MON_METRIC_COUNT; ++i) {
        const int m = order[i];
        if (m < 0 || m >= MON_METRIC_COUNT || seen[m]) continue;
        seen[m] = true;
        clean[n++] = m;
    }
    for (int m = 0; m < MON_METRIC_COUNT; ++m)
        if (!seen[m]) clean[n++] = m;

    for (int i = 0; i < MON_METRIC_COUNT; ++i) order[i] = clean[i];
}

int MonitorSkinCount() { return kSkinCount; }

const MonitorSkin& MonitorSkinAt(int index) {
    if (index < 0 || index >= kSkinCount) index = 0;
    return kSkins[index];
}

int MonitorSkinIndexById(const std::wstring& id) {
    for (int i = 0; i < kSkinCount; ++i)
        if (_wcsicmp(kSkins[i].id, id.c_str()) == 0) return i;
    return -1;
}

COLORREF MonitorMetricColour(const MonitorSkin& skin, int metric,
                             COLORREF override) {
    if (metric < 0 || metric >= MON_METRIC_COUNT) metric = 0;
    if (override != kMonColourFromTheme) return override;
    return skin.metric[metric];
}

namespace {

// Straight-line blend between two colours, 0..1.
COLORREF Mix(COLORREF a, COLORREF b, double t) {
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;
    const auto ch = [&](int av, int bv) {
        return (int)(av + (bv - av) * t + 0.5);
    };
    return RGB(ch(GetRValue(a), GetRValue(b)),
               ch(GetGValue(a), GetGValue(b)),
               ch(GetBValue(a), GetBValue(b)));
}

// Where the warming starts and where it is complete. 72% is high enough that an
// ordinary desktop never trips it and low enough to give some warning before a
// metric is actually pinned.
constexpr double kWarmAt = 72.0;
constexpr double kHotAt  = 92.0;

} // namespace

COLORREF MonitorLoadColour(const MonitorSkin& skin, COLORREF base, double percent) {
    if (!skin.alerts || percent <= kWarmAt) return base;

    constexpr COLORREF kWarm = RGB(245, 176, 66);
    constexpr COLORREF kHot  = RGB(240, 90,  86);

    if (percent >= kHotAt)
        return Mix(kWarm, kHot, (percent - kHotAt) / (100.0 - kHotAt));
    return Mix(base, kWarm, (percent - kWarmAt) / (kHotAt - kWarmAt));
}

int MonitorStyleCount() { return kStyleCount; }

const MonitorStyleInfo& MonitorStyleAt(int index) {
    if (index < 0 || index >= kStyleCount) index = 0;
    return kStyles[index];
}

int MonitorStyleIndexById(const std::wstring& id) {
    for (int i = 0; i < kStyleCount; ++i)
        if (_wcsicmp(kStyles[i].id, id.c_str()) == 0) return i;
    return -1;
}

bool MonitorStyleUsesGraphs(int style) {
    return style == MON_STYLE_ROWS || style == MON_STYLE_GRAPH ||
           style == MON_STYLE_CARDS || style == MON_STYLE_OSD ||
           style == MON_STYLE_HUD;
}

} // namespace awa
