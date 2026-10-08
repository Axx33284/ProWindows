// ProWindows - the Timer page: the clock panel's settings, and its live timers and
// stopwatch as rows. The rows read and drive the model in timer.cpp through the
// accessors in timer.h; nothing here keeps a copy of a timer. Starting, pausing and
// the rest act at once and are saved by the timer itself - they are not config
// fields and Apply has nothing to do with them.
#include "settings_internal.h"
#include "timer.h"

namespace awa {

using ui::Row;

namespace {

bool TimerOn() { return Edit().timerShown; }

// The length a "New timer" gets. Page state only, not saved.
const int kPresetSeconds[] = { 30, 60, 120, 180, 300, 600, 900, 1200, 1800, 2700, 3600, 5400, 7200 };
int g_preset = 4;                       // 5 minutes

unsigned g_builtRev = 0;                // TimerRevision() when the rows were last built

const wchar_t kLive[] = L" This acts at once and is saved by the timer itself; Apply does not apply to it.";

std::wstring PresetName(int seconds) {
    if (seconds < 60)    return std::to_wstring(seconds) + L" sec";
    if (seconds < 3600)  return std::to_wstring(seconds / 60) + L" min";
    if (seconds % 3600 == 0) return std::to_wstring(seconds / 3600) + (seconds == 3600 ? L" hour" : L" hours");
    return std::to_wstring(seconds / 60) + L" min";
}

// The panel can change the list between a build and a click; the index is
// checked against the live model every time it is used.
const timer::Timer* TimerAt(int i) {
    const timer::State& s = TimerModel();
    return (i >= 0 && i < (int)s.timers.size()) ? &s.timers[(size_t)i] : nullptr;
}

std::wstring TimerValue(int i) {
    const timer::Timer* t = TimerAt(i);
    if (!t) return L"";
    const timer::Ticks left = timer::Remaining(*t, timer::NowUtc());
    if (t->running) return timer::FormatDuration(timer::CeilSecond(left)) + L"  running";
    if (t->duration > 0 && t->remaining <= 0) return L"Finished";
    if (t->remaining == t->duration) return timer::FormatDuration(t->duration) + L"  ready";
    return timer::FormatDuration(timer::CeilSecond(t->remaining)) + L"  paused";
}

std::wstring WatchValue() {
    const timer::Stopwatch& w = TimerModel().watch;
    return timer::FormatDuration(timer::Elapsed(w, timer::NowUtc()), true) + (w.running ? L"  running" : L"");
}

} // namespace

bool TimerPageStale() { return g_builtRev != TimerRevision(); }

void BuildTimerPage(std::vector<Row>& rows) {
    Config& e = Edit();
    const Config& s = Saved();
    g_builtRev = TimerRevision();
    const timer::State& st = TimerModel();

    rows.push_back(ui::Page(L"Timers"));
    rows.push_back(ui::Section(L"New timer"));
    {
        Row r;
        r.kind  = ui::Kind::Choice;
        r.id    = L"newlen";
        r.label = L"Length";
        r.help  = L"How long the next new timer runs. Choose it, then press Add. The clock panel "
                  L"(Win+W) can set any length down to the second.";
        for (int sec : kPresetSeconds) r.options.push_back(PresetName(sec));
        r.get = []() { return g_preset; };
        r.set = [](int v) { g_preset = (std::max)(0, (std::min)((int)ARRAYSIZE(kPresetSeconds) - 1, v)); };
        rows.push_back(r);
    }
    rows.push_back(ui::Action(L"newtimer", L"New timer",
        L"Adds a timer of the chosen length, stopped. Start it below or in the clock panel. Up to eight." + std::wstring(kLive),
        L"Add", []() {
            if (!TimerAddPaused((timer::Ticks)kPresetSeconds[g_preset] * timer::kSecond))
                SettingsToast(L"There are already eight timers");
            SettingsRebuild();
        }));

    rows.push_back(ui::Section(L"Timers"));
    if (st.timers.empty())
        rows.push_back(ui::Info(L"notimers", L"No timers", L"Add one above, or in the clock panel (Win+W).", nullptr));
    for (int i = 0; i < (int)st.timers.size(); ++i) {
        const std::wstring id = L"t" + std::to_wstring(i) + L":";
        const timer::Timer& t = st.timers[(size_t)i];
        Row info = ui::Info(id + L"time", t.label,
            L"This timer's remaining time. It counts down by the system clock, so it stays right across sleep and restart."
            L" Ringing and the tick sound follow the Alarm and Clock tick settings on the Panel page.",
            [i]() { return TimerValue(i); });
        info.raw = true;
        rows.push_back(info);
        rows.push_back(ui::Action(id + L"play", L"Start or pause",
            L"Starts this timer, or pauses it where it is. A finished timer starts over." + std::wstring(kLive),
            t.running ? L"Pause" : L"Start", [i]() { TimerStartPause(i); SettingsRebuild(); }));
        rows.push_back(ui::Action(id + L"reset", L"Reset",
            L"Stops this timer and sets it back to its full length." + std::wstring(kLive),
            L"Reset", [i]() { TimerResetAt(i); SettingsRebuild(); }));
        Row del = ui::Action(id + L"del", L"Delete",
            L"Removes this timer." + std::wstring(kLive),
            L"Delete", [i]() { TimerDeleteAt(i); SettingsRebuild(); });
        del.danger = true;
        rows.push_back(del);
    }

    rows.push_back(ui::Section(L"Stopwatch"));
    rows.push_back(ui::Info(L"watch", L"Elapsed",
        L"The stopwatch, shared with the clock panel. It keeps running while this window is closed.",
        []() { return WatchValue(); }));
    rows.push_back(ui::Action(L"watchplay", L"Start or stop",
        L"Starts the stopwatch, or stops it where it is." + std::wstring(kLive),
        st.watch.running ? L"Stop" : L"Start", []() { StopwatchStartStop(); SettingsRebuild(); }));
    rows.push_back(ui::Action(L"watchlap", L"Lap",
        L"Notes the time so far as a lap; the newest is listed first. Up to 99." + std::wstring(kLive),
        L"Lap", []() { StopwatchLap(); SettingsRebuild(); }));
    {
        Row r = ui::Action(L"watchreset", L"Reset",
            L"Stops the stopwatch, sets it to zero and clears the laps." + std::wstring(kLive),
            L"Reset", []() { StopwatchReset(); SettingsRebuild(); });
        r.danger = true;
        rows.push_back(r);
    }
    const int shown = (std::min)((int)st.watch.laps.size(), 10);
    for (int i = 0; i < shown; ++i) {
        Row r = ui::Info(L"lap" + std::to_wstring(i), L"Lap " + std::to_wstring(st.watch.lapSeq - i),
            L"The stopwatch's total when this lap was taken. The clock panel lists them all.",
            [i]() {
                const timer::Stopwatch& w = TimerModel().watch;
                return i < (int)w.laps.size() ? timer::FormatDuration(w.laps[(size_t)i], true) : std::wstring();
            });
        rows.push_back(r);
    }

    // The panel's own settings used to sit on the Clock page; they are config
    // fields like any other and go through Apply.
    rows.push_back(ui::Page(L"Panel"));
    rows.push_back(ui::Section(L"Clock panel"));
    rows.push_back(ui::Toggle(L"timershown", L"Show the clock panel",
        L"The clock, alarms, timers and stopwatch in one panel that stays on your screen. Drag it "
        L"anywhere; alarms and timers keep running while it is hidden.",
        &e.timerShown, &s.timerShown));
    {
        Row r = ui::Toggle(L"timerpinned", L"Pin in place",
            L"Locks it where it is: it cannot be dragged, and clicks go straight through to "
            L"whatever is underneath. Stop a ringing alarm from the tray menu.",
            &e.timerPinned, &s.timerPinned);
        r.enabled = TimerOn;
        rows.push_back(r);
    }
    rows.push_back(ui::Action(L"timerpos", L"Position",
        L"Puts the clock panel back where it first opens. Takes effect at once.",
        L"Reset", []() {
            Config& cfg = AppConfig();
            cfg.timerX = INT_MIN;
            cfg.timerY = INT_MIN;
            TimerApplyConfig();
            AppSaveConfig();
            SettingsToast(L"The clock panel is back where it starts");
        }));
    rows.push_back(ui::ChoiceOf(L"timertick", L"Clock tick",
        L"A soft tick while a timer or the stopwatch runs: every second, or once a minute "
        L"as the seconds come round to zero. Never while the alarm is sounding.",
        &e.timerTick, &s.timerTick, { 0, 1, 2 }, { L"Off", L"Every second", L"Every minute" }));
    rows.push_back(ui::Toggle(L"timeralarm", L"Alarm",
        L"Plays a sound for up to a minute when a timer ends (alarms have their own sound switch), and shows the panel without "
        L"taking the keyboard. Off shows only a notification. Any key or click in the panel, or "
        L"the tray menu, stops the sound.",
        &e.timerAlarm, &s.timerAlarm));
}

void ResetTimerPage() {
    Config d;
    d.LoadDefaults();
    Config& e = Edit();
    e.timerShown = d.timerShown;       e.timerPinned = d.timerPinned;
    e.timerTick = d.timerTick;         e.timerAlarm = d.timerAlarm;
}

} // namespace awa
