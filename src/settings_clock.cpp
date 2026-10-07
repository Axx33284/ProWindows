// ProWindows - the Clock page. Rows over the edit copy, like every other page,
// and a preview drawn through the clock's own painter.
#include "settings_internal.h"
#include "clock.h"
#include "clocktheme.h"

namespace awa {

using ui::Row;

namespace {
bool On() { return Edit().clockEnabled; }
}

void BuildClockPage(std::vector<Row>& rows) {
    Config& e = Edit();
    const Config& s = Saved();
    auto add = [&](Row r) { r.enabled = On; rows.push_back(r); };

    rows.push_back(ui::Section(L"Clock"));
    rows.push_back(ui::Toggle(L"enabled", L"Desktop clock",
        L"A clock that sits on your screen in one of a dozen styles. Drag it anywhere; "
        L"right-click it for all of this.", &e.clockEnabled, &s.clockEnabled));
    add(ui::Toggle(L"pinned", L"Pin in place",
        L"Locks it where it is: it cannot be dragged, and clicks go straight through to "
        L"whatever is underneath.", &e.clockPinned, &s.clockPinned));
    add(ui::Toggle(L"desktop", L"On the desktop",
        L"Sits on the desktop behind every window instead of over them.",
        &e.clockOnDesktop, &s.clockOnDesktop));
    add(ui::Action(L"resetpos", L"Position", L"Puts the clock back in its corner. Takes effect at once.",
        L"Reset", []() {
            Config& cfg = AppConfig();
            cfg.clockX = INT_MIN;
            cfg.clockY = INT_MIN;
            ClockApplyConfig();
            AppSaveConfig();
            SettingsToast(L"The clock is back in its corner");
        }));

    rows.push_back(ui::Section(L"Look"));
    {
        Row r;
        r.kind  = ui::Kind::Choice;
        r.id    = L"style";
        r.label = L"Style";
        r.help  = ClockStyleAt(e.clockStyle).blurb;
        for (int i = 0; i < ClockStyleCount(); ++i) r.options.push_back(ClockStyleAt(i).name);
        r.get = []() { return (std::max)(0, (std::min)(ClockStyleCount() - 1, Edit().clockStyle)); };
        r.set = [](int v) { Edit().clockStyle = v; };
        r.modified = []() { return Edit().clockStyle != Saved().clockStyle; };
        add(r);
    }
    {
        Row r;
        r.kind  = ui::Kind::Choice;
        r.id    = L"theme";
        r.label = L"Theme";
        r.help  = ClockSkinAt(e.clockTheme).blurb;
        for (int i = 0; i < ClockSkinCount(); ++i) r.options.push_back(ClockSkinAt(i).name);
        r.get = []() { return (std::max)(0, (std::min)(ClockSkinCount() - 1, Edit().clockTheme)); };
        r.set = [](int v) { Edit().clockTheme = v; };
        r.modified = []() { return Edit().clockTheme != Saved().clockTheme; };
        add(r);
    }
    add(ui::Slider(L"opacity", L"Opacity",
        L"How solid the clock's panel is. Lower lets more of what is behind it show through.",
        &e.clockOpacity, &s.clockOpacity, 20, 100, 1, 5, L"%"));
    add(ui::Slider(L"scale", L"Size", L"The clock's size, as a share of its normal size.",
        &e.clockScale, &s.clockScale, 50, 250, 1, 10, L"%"));

    rows.push_back(ui::Section(L"What it shows"));
    add(ui::Toggle(L"hours24", L"Hours", L"A 24-hour clock, or 12 hours with AM and PM.",
        &e.clockHours24, &s.clockHours24, L"24-hour", L"12-hour"));
    add(ui::Toggle(L"seconds", L"Seconds", L"Shows the seconds as well. The clock then wakes "
        L"every second instead of every minute.", &e.clockSeconds, &s.clockSeconds));
    add(ui::Toggle(L"date", L"Date", L"Shows the date under the time.", &e.clockDate, &s.clockDate));
    add(ui::Toggle(L"weekday", L"Day of the week", L"Shows the day's name with the date.",
        &e.clockWeekday, &s.clockWeekday));
}

void ResetClockPage() {
    Config d;
    d.LoadDefaults();
    Config& e = Edit();
    e.clockEnabled = d.clockEnabled;   e.clockPinned = d.clockPinned;
    e.clockOnDesktop = d.clockOnDesktop;
    e.clockTheme = d.clockTheme;       e.clockStyle = d.clockStyle;
    e.clockHours24 = d.clockHours24;   e.clockSeconds = d.clockSeconds;
    e.clockDate = d.clockDate;         e.clockWeekday = d.clockWeekday;
    e.clockOpacity = d.clockOpacity;   e.clockScale = d.clockScale;
}

void PreviewClock(HDC dc, const RECT& area) {
    const Config& e = Edit();
    ClockPreview look;
    look.theme   = e.clockTheme;
    look.style   = e.clockStyle;
    look.opacity = e.clockOpacity;
    look.hours24 = e.clockHours24;
    look.seconds = e.clockSeconds;
    look.date    = e.clockDate;
    look.weekday = e.clockWeekday;
    ClockDrawPreview(dc, area, look);
}

} // namespace awa
