// ProWindows - drawing the desktop clock.
//
// The same split as the monitor's monpaint.*: this file is only geometry and
// paint, knows nothing about the config, and takes everything it needs in a
// ClockPaintCtx - which is what lets the settings page preview a look that is
// not the live one, through the very same painter. The panel behind the time
// is the monitor's own chrome (PaintPanel in monpaint.h), so the two overlays
// are visibly the same family.
#pragma once
#include "common.h"
#include "clocktheme.h"

namespace Gdiplus { class Graphics; }

namespace awa {

struct ClockPaintCtx {
    float scale   = 1.0f;
    BYTE  alpha   = 255;                 // panel opacity
    int   style   = CLOCK_STYLE_DIGITAL;
    const ClockSkin* skin = nullptr;
    bool  hours24 = false;               // 14:05 rather than 2:05 PM
    bool  seconds = false;
    bool  date    = true;                // the day and month
    bool  weekday = true;                // and its name
    SYSTEMTIME time = {};                // what to show; local time
};

// The strings a frame is made of. Exposed so the window can compare two
// frames without painting either: a clock that shows no seconds only changes
// once a minute, and the other fifty-nine ticks are not worth a repaint.
struct ClockText {
    std::wstring hours;        // "14" or "2"
    std::wstring minutes;      // "05"
    std::wstring seconds;      // "09"
    std::wstring ampm;         // "PM", or empty in 24-hour mode
    std::wstring dateLong;     // "Monday, 16 September" - as the locale writes it
    std::wstring dateShort;    // "Mon 16 Sep"
    std::wstring weekdayLong;  // "Monday"
    std::wstring weekdayShort; // "Mon"
    std::wstring day;          // "16"
    std::wstring monthShort;   // "Sep"
};
ClockText ClockBuildText(const ClockPaintCtx& ctx);

// Panel size for this look, shadow margin included, like MonMeasure.
SIZE ClockMeasure(const ClockPaintCtx& ctx);

// Paints the whole panel, background included, into a `width` x `height`
// area at the origin. The caller clears the surface first if it needs
// transparency.
void ClockDraw(Gdiplus::Graphics* g, int width, int height, const ClockPaintCtx& ctx);

// Does this style ever change between one second and the next? Decides how
// often the window has to wake up.
bool ClockStyleShowsSeconds(int style, bool secondsOn);

} // namespace awa
