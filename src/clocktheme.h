// ProWindows - colour schemes and layouts for the desktop clock.
//
// Pure data, laid out the way montheme.h lays out the monitor's: a table of
// skins and a table of styles, each entry found by a name that the config
// file stores. `clockpaint.cpp` does the drawing, `clock.cpp` owns the window,
// and the Clock settings page previews the pair.
#pragma once
#include "common.h"

namespace awa {

// How the clock is laid out. Independent of the colours: any style can wear
// any skin, exactly as the monitor's can.
enum ClockStyle : int {
    CLOCK_STYLE_DIGITAL = 0,   // big time, the date underneath, on a panel
    CLOCK_STYLE_MINIMAL,       // thin type straight on the wallpaper, no panel
    CLOCK_STYLE_ANALOG,        // a round dial with hands
    CLOCK_STYLE_FLIP,          // split-flap tiles, one digit each
    CLOCK_STYLE_SEGMENTS,      // seven-segment digits with ghost segments, like an LED alarm clock
    CLOCK_STYLE_STACKED,       // hours over minutes, huge, the date down the side
    CLOCK_STYLE_WIDE,          // one thin line: weekday, date and time, for a screen edge
    CLOCK_STYLE_RING,          // the time inside a ring that fills as the minute passes
    // Appended after the first eight so the config tokens above keep their
    // meaning; the settings list shows them in this order too.
    CLOCK_STYLE_ROMAN,         // a dial with Roman numerals and slim hands
    CLOCK_STYLE_STATION,       // a dial with bold numerals and heavy hands, like a railway clock
    CLOCK_STYLE_DIAL,          // a bare dial: four markers, no rim, thin hands
    CLOCK_STYLE_CLASSIC,       // a framed readout: rules above and below the time, small capitals
    CLOCK_STYLE_COUNT
};

struct ClockStyleInfo {
    const wchar_t* id;      // config token
    const wchar_t* name;    // label in the UI and the context menu
    const wchar_t* blurb;   // one line for the settings page
};

int  ClockStyleCount();
const ClockStyleInfo& ClockStyleAt(int index);     // clamped
int  ClockStyleIndexById(const std::wstring& id);  // -1 when unknown

// Which typeface family the digits and the date are set in. A skin chooses,
// because a phosphor terminal wants a monospace and a paper calendar wants a
// serif, and the family is more of the look than the colours are.
enum ClockFace : int {
    CLOCK_FACE_LIGHT = 0,   // Segoe UI Light: the wallpaper-clock look
    CLOCK_FACE_REGULAR,     // Segoe UI
    CLOCK_FACE_SEMIBOLD,    // Segoe UI Semibold
    CLOCK_FACE_MONO,        // Consolas
    CLOCK_FACE_CONDENSED,   // Bahnschrift SemiBold Condensed: the game-menu look
    CLOCK_FACE_SERIF,       // Georgia
};

struct ClockSkin {
    const wchar_t* id;        // config token, e.g. "midnight"
    const wchar_t* name;      // label in the UI and the context menu
    const wchar_t* blurb;     // one line of description for the settings page

    COLORREF panelTop;        // panel fill, top of the vertical gradient
    COLORREF panelBottom;     // ... and the bottom. Equal values = flat fill.
    COLORREF border;
    BYTE     borderAlpha;     // relative to the panel's own opacity
    BYTE     gloss;           // alpha of the hairline highlight along the top
    int      radius;          // panel corner radius, in unscaled pixels

    COLORREF time;            // the digits, and the hour and minute hands
    COLORREF date;            // the smaller line
    COLORREF accent;          // the seconds, the colon, the seconds hand, the ring
    COLORREF dim;             // ghost segments, dial ticks, tile split lines
    COLORREF tile;            // a flip tile's face, the dial's face
    BYTE     glow;            // 0 = none; otherwise the strength of the halo round the digits
    ClockFace face;

    // No panel at all: no shadow, no fill, no border - the time is drawn
    // straight onto whatever is behind it with a shadow under the text, which
    // is what a wallpaper clock looks like. Last, so the skins that do not set
    // it are simply false.
    bool     bare = false;
};

int  ClockSkinCount();
// Clamped: any out-of-range index gives the default skin rather than a crash.
const ClockSkin& ClockSkinAt(int index);
// -1 when the id is unknown, so callers can fall back without guessing.
int  ClockSkinIndexById(const std::wstring& id);

} // namespace awa
