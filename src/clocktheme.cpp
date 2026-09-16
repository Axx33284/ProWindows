#include "clocktheme.h"

namespace awa {

namespace {

// Kept in one table so adding a skin is a single edit. Index 0 is the default
// and must stay first; everything else is only referenced by `id`.
//
// Field order: id, name, blurb,
//              panelTop, panelBottom, border, borderAlpha, gloss, radius,
//              time, date, accent, dim, tile, glow, face, [bare]
const ClockSkin kSkins[] = {
{
    L"midnight", L"Midnight",
    L"The default. Near-black glass, white digits, a blue second hand.",
    RGB(26, 27, 33), RGB(18, 19, 23), RGB(70, 74, 84), 180, 26, 14,
    RGB(244, 245, 248), RGB(150, 155, 165), RGB(88, 140, 255),
    RGB(58, 61, 70), RGB(34, 36, 42), 0, CLOCK_FACE_LIGHT
},
{
    L"glass", L"Glass",
    L"No panel: white digits with a soft shadow, straight on the wallpaper.",
    RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), 0, 0, 0,
    RGB(255, 255, 255), RGB(222, 226, 234), RGB(255, 255, 255),
    RGB(255, 255, 255), RGB(0, 0, 0), 0, CLOCK_FACE_LIGHT, true
},
{
    L"ink", L"Ink",
    L"The same, in black, for a light wallpaper.",
    RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), 0, 0, 0,
    RGB(24, 24, 28), RGB(70, 72, 80), RGB(24, 24, 28),
    RGB(24, 24, 28), RGB(255, 255, 255), 0, CLOCK_FACE_LIGHT, true
},
{
    L"slayer", L"Slayer",
    L"Gunmetal and hazard orange, condensed capitals. The DOOM one.",
    RGB(24, 26, 29), RGB(14, 15, 17), RGB(245, 146, 30), 150, 14, 3,
    RGB(245, 146, 30), RGB(196, 200, 206), RGB(214, 58, 42),
    RGB(62, 66, 72), RGB(32, 35, 39), 0, CLOCK_FACE_CONDENSED
},
{
    L"nixie", L"Nixie",
    L"Amber digits glowing in a dark tube, the way a nixie clock does.",
    RGB(22, 16, 12), RGB(10, 8, 6), RGB(120, 70, 30), 200, 12, 10,
    RGB(255, 150, 46), RGB(188, 128, 74), RGB(255, 196, 96),
    RGB(74, 46, 26), RGB(30, 22, 16), 120, CLOCK_FACE_REGULAR
},
{
    L"neon", L"Neon",
    L"Cyan tube light with a magenta second hand, big round corners.",
    RGB(14, 12, 24), RGB(8, 6, 16), RGB(64, 220, 255), 120, 0, 22,
    RGB(96, 236, 255), RGB(140, 170, 210), RGB(255, 96, 224),
    RGB(40, 44, 72), RGB(20, 18, 34), 140, CLOCK_FACE_SEMIBOLD
},
{
    L"terminal", L"Terminal",
    L"One green on black, square corners, monospace.",
    RGB(8, 14, 9), RGB(4, 8, 5), RGB(38, 92, 48), 210, 0, 4,
    RGB(120, 255, 140), RGB(78, 158, 92), RGB(120, 255, 140),
    RGB(24, 52, 30), RGB(10, 20, 12), 40, CLOCK_FACE_MONO
},
{
    L"amber", L"Amber",
    L"The other old terminal: amber on near-black.",
    RGB(20, 13, 4), RGB(12, 8, 2), RGB(112, 72, 20), 210, 0, 4,
    RGB(255, 183, 77), RGB(186, 128, 48), RGB(255, 183, 77),
    RGB(56, 38, 14), RGB(24, 16, 6), 40, CLOCK_FACE_MONO
},
{
    L"lcd", L"LCD",
    L"A grey-green LCD panel with dark segments, like a desk clock.",
    RGB(178, 196, 168), RGB(160, 180, 150), RGB(110, 126, 100), 220, 40, 6,
    RGB(28, 36, 30), RGB(60, 74, 60), RGB(28, 36, 30),
    RGB(150, 168, 142), RGB(168, 186, 158), 0, CLOCK_FACE_MONO
},
{
    L"paper", L"Paper",
    L"A white card with black ink and a red second hand, like a wall calendar.",
    RGB(252, 252, 250), RGB(240, 240, 236), RGB(196, 196, 190), 220, 0, 8,
    RGB(28, 28, 30), RGB(110, 112, 118), RGB(214, 58, 42),
    RGB(214, 214, 210), RGB(244, 244, 240), 0, CLOCK_FACE_SERIF
},
{
    L"frost", L"Frost",
    L"Light glass with dark text, for a light wallpaper.",
    RGB(236, 240, 246), RGB(220, 226, 236), RGB(170, 180, 196), 200, 60, 14,
    RGB(30, 34, 44), RGB(96, 104, 122), RGB(58, 110, 230),
    RGB(196, 204, 218), RGB(226, 231, 240), 0, CLOCK_FACE_LIGHT
},
{
    L"graphite", L"Graphite",
    L"No colour at all. Grey glass, white digits, grey seconds.",
    RGB(38, 39, 43), RGB(28, 29, 32), RGB(96, 100, 108), 170, 30, 10,
    RGB(238, 240, 244), RGB(158, 162, 170), RGB(186, 190, 198),
    RGB(66, 68, 74), RGB(46, 47, 52), 0, CLOCK_FACE_REGULAR
},
{
    L"nord", L"Nord",
    L"The cool blue-grey palette with an arctic blue second hand.",
    RGB(59, 66, 82), RGB(46, 52, 64), RGB(94, 106, 130), 190, 34, 10,
    RGB(236, 239, 244), RGB(180, 190, 206), RGB(136, 192, 208),
    RGB(76, 86, 106), RGB(52, 58, 72), 0, CLOCK_FACE_LIGHT
},
{
    L"dracula", L"Dracula",
    L"Deep violet with a pink second hand.",
    RGB(48, 50, 66), RGB(38, 40, 52), RGB(98, 114, 164), 190, 30, 12,
    RGB(248, 248, 242), RGB(160, 166, 192), RGB(255, 121, 198),
    RGB(68, 71, 90), RGB(42, 44, 58), 0, CLOCK_FACE_REGULAR
},
{
    L"solarized", L"Solarized",
    L"The low-contrast teal base, with Solarized orange for the seconds.",
    RGB(7, 54, 66), RGB(0, 43, 54), RGB(88, 110, 117), 200, 22, 8,
    RGB(238, 232, 213), RGB(131, 148, 150), RGB(203, 75, 22),
    RGB(30, 72, 84), RGB(4, 48, 60), 0, CLOCK_FACE_REGULAR
},
{
    L"gruvbox", L"Gruvbox",
    L"Warm retro browns with a faded orange second hand.",
    RGB(60, 56, 54), RGB(40, 40, 40), RGB(102, 92, 84), 200, 26, 8,
    RGB(235, 219, 178), RGB(168, 153, 132), RGB(254, 128, 25),
    RGB(80, 74, 70), RGB(50, 48, 46), 0, CLOCK_FACE_SEMIBOLD
},
{
    L"tokyonight", L"Tokyo Night",
    L"Indigo glass with a soft blue second hand.",
    RGB(36, 40, 59), RGB(26, 27, 38), RGB(65, 72, 104), 190, 28, 12,
    RGB(192, 202, 245), RGB(122, 132, 168), RGB(122, 162, 247),
    RGB(54, 60, 86), RGB(30, 33, 48), 0, CLOCK_FACE_LIGHT
},
{
    L"catppuccin", L"Catppuccin",
    L"Pastel on mocha, with a mauve second hand.",
    RGB(49, 50, 68), RGB(30, 30, 46), RGB(88, 91, 112), 190, 28, 14,
    RGB(205, 214, 244), RGB(166, 173, 200), RGB(203, 166, 247),
    RGB(69, 71, 90), RGB(36, 37, 52), 0, CLOCK_FACE_REGULAR
},
{
    L"rosepine", L"Rosé Pine",
    L"Muted rose on a dark base.",
    RGB(38, 35, 53), RGB(25, 23, 36), RGB(82, 79, 103), 190, 26, 12,
    RGB(224, 222, 244), RGB(144, 140, 170), RGB(235, 111, 146),
    RGB(58, 54, 78), RGB(31, 29, 46), 0, CLOCK_FACE_LIGHT
},
{
    L"ocean", L"Ocean",
    L"Deep navy with cyan seconds.",
    RGB(18, 34, 52), RGB(11, 22, 36), RGB(52, 86, 122), 190, 30, 12,
    RGB(198, 226, 245), RGB(139, 168, 196), RGB(56, 189, 248),
    RGB(36, 58, 84), RGB(14, 28, 44), 0, CLOCK_FACE_LIGHT
},
{
    L"ember", L"Ember",
    L"Charcoal warmed by orange.",
    RGB(38, 30, 30), RGB(26, 20, 21), RGB(96, 72, 68), 190, 26, 12,
    RGB(255, 236, 224), RGB(178, 152, 146), RGB(251, 146, 60),
    RGB(66, 52, 50), RGB(32, 25, 25), 0, CLOCK_FACE_SEMIBOLD
},
};

constexpr int kSkinCount = (int)(sizeof(kSkins) / sizeof(kSkins[0]));

const ClockStyleInfo kStyles[CLOCK_STYLE_COUNT] = {
    { L"digital",  L"Digital",
      L"The default. The time, large, with the date underneath." },
    { L"minimal",  L"Minimal",
      L"Thin type and small capitals, the wallpaper-clock look. Best on Glass or Ink." },
    { L"analog",   L"Analog",
      L"A round dial with hour, minute and second hands." },
    { L"flip",     L"Flip",
      L"Split-flap tiles, one digit each, like a station board." },
    { L"segments", L"Segments",
      L"Seven-segment digits with the unlit segments faintly showing, like an LED clock." },
    { L"stacked",  L"Stacked",
      L"Hours over minutes, very large, with the date down the side." },
    { L"wide",     L"Wide",
      L"A single thin line - weekday, date, time - for the edge of a screen." },
    { L"ring",     L"Ring",
      L"The time inside a ring that fills as the minute goes by." },
};

} // namespace

int ClockSkinCount() { return kSkinCount; }

const ClockSkin& ClockSkinAt(int index) {
    if (index < 0 || index >= kSkinCount) index = 0;
    return kSkins[index];
}

int ClockSkinIndexById(const std::wstring& id) {
    const std::wstring want = ToLower(id);
    for (int i = 0; i < kSkinCount; ++i)
        if (want == kSkins[i].id) return i;
    return -1;
}

int ClockStyleCount() { return CLOCK_STYLE_COUNT; }

const ClockStyleInfo& ClockStyleAt(int index) {
    if (index < 0 || index >= CLOCK_STYLE_COUNT) index = 0;
    return kStyles[index];
}

int ClockStyleIndexById(const std::wstring& id) {
    const std::wstring want = ToLower(id);
    for (int i = 0; i < CLOCK_STYLE_COUNT; ++i)
        if (want == kStyles[i].id) return i;
    return -1;
}

} // namespace awa
