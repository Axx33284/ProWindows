// ProWindows - the clock panel's view: what is on screen, what a click on it means.
//
// timer.cpp owns the window, the model and every action; clockpanel_paint.cpp
// draws. They meet here. The panel is immediate-mode: every paint rebuilds the
// list of Hit rectangles it drew, and the mouse looks things up in that list, so
// what can be clicked is always exactly what is visible (and a button has the
// same rectangle for painting, hovering and pressing).
#pragma once
#include "common.h"
#include "timer.h"

namespace awa {
namespace panel {

// Unscaled layout, DIP.
constexpr int kWidth  = 880;
constexpr int kHeight = 560;
constexpr int kRail   = 196;       // the left navigation rail
constexpr int kTop    = 40;        // the strip across the top that drags the window

enum Page { PageClock = 0, PageAlarm, PageTimer, PageWatch, PageCount };
enum Sheet { SheetNone = 0, SheetTimer, SheetAlarm };

// What a click means. `arg` says which one (page, card, field, day ...).
enum Cmd {
    CmdNone = 0,
    CmdPage, CmdPin, CmdClose, CmdCard,
    CmdQuick, CmdTimerAdd, CmdTimerSel, CmdTimerPlay, CmdTimerReset, CmdTimerEdit, CmdTimerDelete,
    CmdWatchPlay, CmdWatchLap, CmdWatchReset,
    CmdAlarmAdd, CmdAlarmEdit, CmdAlarmToggle, CmdAlarmDelete,
    CmdRingSnooze, CmdRingDismiss,
    // the editors
    CmdSave, CmdCancel, CmdDelete,
    CmdName, CmdSpinFocus, CmdSpinUp, CmdSpinDown,
    CmdKind, CmdWeekday, CmdMonthDay, CmdLastDay, CmdWeek, CmdMonth, CmdLimitWd,
    CmdLimitOpen, CmdUntil, CmdDay, CmdSound, CmdSnoozeLen, CmdAmPm,
};

// The spinner fields. A timer's four use the first four ids.
enum Field {
    FieldHour = 0, FieldMinute, FieldDay, FieldMonth, FieldYear,
    FieldFrom, FieldTo, FieldUntilDay, FieldUntilMonth, FieldUntilYear,
    FieldName = 100,
};

struct Hit {
    RECT r;
    int  cmd = CmdNone;
    int  arg = 0;
    bool wheel = false;            // the mouse wheel changes this one's number
};

// A single-line text field: the text, the caret and where the selection started.
struct TextField {
    std::wstring s;
    int caret = 0, anchor = 0;
    bool HasSel() const { return caret != anchor; }
};

struct TimerDraft {
    int index = -1;                // -1: a new timer
    TextField name;
    int v[4] = {};                 // days, hours, minutes, seconds
};

struct AlarmDraft {
    int index = -1;
    alarm::Alarm a;
    TextField name;
    bool limitOpen = false;
};

// An alarm that is ringing, for the banner.
struct Ring {
    int index = -1;
    std::wstring name;
    int hour = 0, minute = 0;
    int snoozeMin = 10;
};

struct View {
    int page = PageClock;
    int sheet = SheetNone;
    TimerDraft td;
    AlarmDraft ad;
    int  focus = FieldHour;        // the editor's keyboard focus: a Field
    int  typed = 0;                // digits typed into the focused spinner since it got focus
    int  acc = 0;                  // and what they add up to
    int  sel = 0;                  // the selected timer card
    int  scroll[PageCount] = {};
    int  sheetScroll = 0;
    int  hotCmd = CmdNone, hotArg = 0;      // under the pointer
    int  downCmd = CmdNone, downArg = 0;    // pressed (left button down on it)
    bool ringing = false;
    Ring ring;
    bool pinned = false;

    // Written by the paint, read by the mouse and the keys.
    std::vector<Hit> hits;
    RECT scrollRect = {};          // the scrolling list or sheet body under the wheel
    int  scrollMax = 0;            // how far it can scroll, pixels
    bool again = false;            // the scroll position was clamped: paint once more
    RECT nameRect = {};            // the name field's text area, for placing the caret
};

// Everything the paint needs that is not the view.
struct Model {
    const timer::State* s = nullptr;
    bool hours24 = false, seconds = false;
    timer::Ticks now = 0;
    std::vector<timer::Ticks> wake;     // per alarm: its next ring, 0 = none
    int nextAlarm = -1;                  // the soonest of them
};

void Paint(HDC dc, const SIZE& size, const Model& m, View& v);
// Which character boundary of `f` the pixel `relX` (from the text's left edge) is nearest.
int  CaretAt(HDC dc, const std::wstring& text, int relX);
// Fonts made for the panel are kept while it is up; the idle trim lets them go.
void ReleaseFonts();

// "in 3 h 12 min", "in 12 min", "in less than a minute".
std::wstring FormatIn(timer::Ticks span);

} // namespace panel
} // namespace awa
