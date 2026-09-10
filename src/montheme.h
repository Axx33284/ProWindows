// ProWindows - colour schemes for the floating system monitor.
//
// Pure data. `monitor.cpp` does the drawing, the Monitor settings page shows a
// live preview of the selected one, and `config.cpp` stores the `id` string so
// the config file stays readable and survives themes being reordered.
#pragma once
#include "common.h"

namespace awa {

// A metric index, used to pick a colour out of MonitorSkin::metric.
// The order is the order metrics appear in the overlay and in the settings.
enum MonMetric : int { MON_CPU = 0, MON_RAM, MON_GPU, MON_VRAM,
                       MON_CPUTEMP, MON_GPUTEMP, MON_DISK, MON_NET,
                       MON_METRIC_COUNT };

// One metric, as everything outside the sampler needs to talk about it. The
// three names used to be written out separately in five files - the parser in
// config.cpp, the writer in defaults.cpp, the row builder in monitor.cpp, the
// context menu, and the settings page - and adding the two temperature rows
// meant finding all five. One table now, in the file that already holds this
// project's tables of data.
struct MonitorMetricInfo {
    const wchar_t* id;      // config token, e.g. "cpu_temp"
    const wchar_t* label;   // "CPU TEMP", as the overlay paints it
    const wchar_t* shortLabel;   // "CPU°", for the narrow styles
    const wchar_t* menu;    // "CPU temperature", as a menu or a settings row
};

const MonitorMetricInfo& MonitorMetricAt(int metric);   // clamped
// -1 when the token is unknown, so a caller can ignore it rather than guess.
int MonitorMetricIndexById(const std::wstring& id);

// Puts `order` into a state the overlay can use: every metric exactly once.
// A config file can say anything at all - a name twice, a name that no longer
// exists, six of the eight - and the overlay still has to have all eight rows
// to index by. Unknown and duplicate entries are dropped and whatever is
// missing is appended in the declared order, so a hand-edited file that names
// three metrics gets those three first and the rest behind them.
void MonitorNormaliseOrder(int order[MON_METRIC_COUNT]);

// How the overlay is laid out. Independent of the colour scheme: any style can
// wear any skin.
enum MonStyle : int {
    MON_STYLE_ROWS = 0,   // stacked rows: label, reading, sparkline, bar
    MON_STYLE_COMPACT,    // one dense line each: label, bar, reading
    MON_STYLE_RINGS,      // a circular gauge per metric
    MON_STYLE_ARCS,       // a 240-degree dial per metric
    MON_STYLE_BARS,       // vertical column meters
    MON_STYLE_GRAPH,      // chart first: a big area graph per metric
    MON_STYLE_CARDS,      // each metric on its own raised card with an accent rail
    MON_STYLE_TICKER,     // one thin line: a dot, a name and a number per metric
    MON_STYLE_COUNT
};

struct MonitorStyleInfo {
    const wchar_t* id;      // config token
    const wchar_t* name;    // label in the UI and the context menu
    const wchar_t* blurb;   // one line for the settings page
};

int  MonitorStyleCount();
const MonitorStyleInfo& MonitorStyleAt(int index);   // clamped
int  MonitorStyleIndexById(const std::wstring& id);  // -1 when unknown

// Whether this style draws a history curve at all. The settings page greys the
// "Show graphs" box out for the ones that do not, and it used to do that from
// its own hard-coded list of two - which is exactly the kind of thing that goes
// stale the moment a style is added. One answer, one place.
bool MonitorStyleUsesGraphs(int style);

struct MonitorSkin {
    const wchar_t* id;        // config token, e.g. "midnight"
    const wchar_t* name;      // label in the UI and the context menu
    const wchar_t* blurb;     // one line of description for the settings page

    COLORREF panelTop;        // panel fill, top of the vertical gradient
    COLORREF panelBottom;     // ... and the bottom. Equal values = flat fill.
    COLORREF border;
    BYTE     borderAlpha;     // relative to the panel's own opacity
    BYTE     gloss;           // alpha of the hairline highlight along the top

    COLORREF label;           // metric name
    COLORREF detail;          // the smaller second line
    COLORREF value;           // headline number, or kUseMetricColour
    COLORREF track;           // progress-bar groove
    BYTE     trackAlpha;
    BYTE     graphFill;       // alpha of the wash under a sparkline
    BYTE     graphLine;       // alpha of the sparkline itself
    int      radius;          // panel corner radius, in unscaled pixels

    // Whether a reading close to its ceiling is allowed to warm towards amber
    // and then red. It is genuinely useful on the colour-coded skins - a glance
    // tells you something is pinned without reading a number - and wrong on the
    // ones whose whole point is a single hue, so each skin says for itself.
    bool     alerts;

    COLORREF metric[MON_METRIC_COUNT];   // in MonMetric order
};

// Sentinel for MonitorSkin::value: colour each readout like its own metric.
constexpr COLORREF kUseMetricColour = 0xFF000000;

// Sentinel for a per-metric override: this metric has none, so the theme
// decides. A real COLORREF never has the high byte set, so neither sentinel
// can collide with a colour a user actually picked.
constexpr COLORREF kMonColourFromTheme = 0xFF000001;

// What a metric actually paints with: the user's override when they set one,
// otherwise whatever the skin says. `metric` is a MonMetric.
COLORREF MonitorMetricColour(const MonitorSkin& skin, int metric,
                             COLORREF override);

// The colour a bar, ring or column actually paints with at `percent` full.
// Identical to `base` below the warm threshold, and on a skin that has asked
// not to have one; above it the hue slides towards amber and then red, so a
// pinned CPU reads as pinned from across the room. Text keeps `base`: moving
// the label colour as well made the panel look like it was flickering.
COLORREF MonitorLoadColour(const MonitorSkin& skin, COLORREF base, double percent);

int  MonitorSkinCount();
// Clamped: any out-of-range index gives the default skin rather than a crash.
const MonitorSkin& MonitorSkinAt(int index);
// -1 when the id is unknown, so callers can fall back without guessing.
int  MonitorSkinIndexById(const std::wstring& id);

} // namespace awa
