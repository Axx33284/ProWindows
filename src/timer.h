// ProWindows - the timer and stopwatch overlay.
//
// A panel with two pages, countdown timers and a stopwatch, that stays where it
// was put like the monitor and the clock: drag it, pin it, hide it.
// Everything is anchored to the system clock (UTC), never to tick counts, so a
// running timer survives sleep, hibernate, shutdown and restart and is exactly
// where the wall clock says it should be the next time anyone looks.
//
// While nothing runs there is no OS timer at all; while the panel is hidden a
// single SetTimer sleeps until the next deadline (or the next tick sound).
#pragma once
#include "common.h"
#include "config.h"

namespace awa {

void TimerInit(HINSTANCE inst, Config* cfg);
void TimerShutdown();

// On screen or not, without touching the config: UpdateOverlayVisibility calls
// this with timerShown && (not game mode, display on).
void TimerSetVisible(bool visible);
bool TimerVisible();

// The `timer` action: flips timerShown (saved).
void TimerToggle();
// The user puts it away (Esc, its menu): timerShown = false, saved.
void TimerHide();

// Pinned = click-through and never activated; saves the config.
void TimerSetPinned(bool pinned);
bool TimerPinned();

// A finished timer's alarm is sounding; a pinned panel cannot take a key to
// stop it, so the tray menu offers this.
bool TimerAlarming();
void TimerStopAlarm();

// Re-reads the config (pin, position, tick mode, alarm) and re-plans the wake-up.
void TimerApplyConfig();

// Where timers.ini lives; the default is ConfigDir(). Call before TimerInit.
// Tests point it at a scratch file so they never touch the real one.
void TimerSetStorePath(const std::wstring& path);

// Looks at the clock now: anything that finished while the PC slept fires.
// Called on resume from sleep.
void TimerCheckNow();

// ---------------------------------------------------------------- the model
// Public so tests\timer_test.cpp can drive it without a window.
namespace timer {

using Ticks = int64_t;                           // 100 ns units, as FILETIME
constexpr Ticks kSecond = 10000000LL;
constexpr Ticks kMaxDuration = (9999LL * 86400LL + 86399LL) * kSecond;   // 9999d 23:59:59
constexpr int   kMaxTimers = 8;
constexpr int   kMaxLaps   = 99;

struct Timer {
    std::wstring label;
    Ticks duration  = 0;
    bool  running   = false;
    Ticks endUtc    = 0;       // while running
    Ticks remaining = 0;       // while paused, not started or finished
};

struct Stopwatch {
    bool  running  = false;
    Ticks startUtc = 0;        // while running
    Ticks banked   = 0;        // what earlier runs added up to
    std::vector<Ticks> laps;   // totals at each lap, newest first
    int   lapSeq   = 0;        // how many laps were ever taken (numbering survives the cap)
};

struct State {
    std::vector<Timer> timers;
    Stopwatch watch;
};

struct Finished {
    int   index = -1;          // which timer
    std::wstring label;
    Ticks endUtc = 0;
};

Ticks NowUtc();
Ticks Remaining(const Timer& t, Ticks now);      // clamped at 0
Ticks Elapsed(const Stopwatch& w, Ticks now);    // never negative, even if the clock went back
bool  AnythingRunning(const State& s);

// Every running timer whose end has passed becomes stopped at 0 and is
// reported - once, because it is no longer running afterwards.
std::vector<Finished> TakeFinished(State& s, Ticks now);

// "H:MM:SS", or "Nd HH:MM:SS" from a day up; ".t" is added for tenths. Rounds
// down; callers showing a countdown pass CeilSecond(remaining).
std::wstring FormatDuration(Ticks t, bool tenths = false);
Ticks CeilSecond(Ticks t);
// Accepts what FormatDuration prints, and "M:SS" or a bare number of seconds.
bool  ParseDuration(const std::wstring& text, Ticks* out);

// timers.ini: UTF-8 key = value lines. Parse tolerates anything it does not know.
std::wstring Serialize(const State& s);
bool  Deserialize(const std::wstring& text, State* out);
bool  LoadFile(const std::wstring& path, State* out);
bool  SaveFile(const std::wstring& path, const State& s);   // writes .new, then renames

} // namespace timer
} // namespace awa
