// ProWindows - the alarm model.
//
// Pure functions over a list of alarms, no window and no clock of its own: every
// call takes `now` and the time zone it should use, so a test can move time and
// DST wherever it likes. Alarms are local wall time (a 07:00 alarm stays 07:00
// across a clock change); timers stay UTC. Instants are 100-ns ticks of the
// system clock, as FILETIME, the same unit as timer::Ticks.
#pragma once
#include "common.h"

namespace awa {
namespace alarm {

using Ticks = int64_t;
constexpr Ticks kSecond = 10000000LL;
constexpr Ticks kMinute = 60 * kSecond;
constexpr int   kMaxAlarms = 32;
constexpr int   kMaxLabel  = 40;
constexpr Ticks kMissedAfter = 10 * kSecond;   // due longer ago than this = missed, not rung

enum class Kind { Once = 0, Hourly, Daily, Weekly, Monthly };

// Bit layouts (0 in a limit mask = no limit).
constexpr uint32_t kLastDayBit  = 1u << 31;    // monthDays: bits 0-30 = days 1-31
constexpr uint32_t kLastWeekBit = 1u << 5;     // weeks: bits 0-4 = 1st-5th, bit 5 = last

struct Alarm {
    std::wstring label;
    bool  enabled = true;
    int   hour = 7, minute = 0;
    Kind  kind = Kind::Once;
    int   year = 0, month = 0, day = 0;        // Once: the date
    int   hourFrom = 0, hourTo = 23;           // Hourly: inclusive; from > to wraps midnight
    uint32_t weekdays = 0;                     // 7 bits, Monday = bit 0
    uint32_t monthDays = 0;                    // Monthly
    uint32_t weeks = 0;                        // weeks of the month
    uint32_t months = 0;                       // 12 bits, January = bit 0
    int   until = 0;                           // yyyymmdd, 0 = none
    bool  sound = true;
    int   snoozeMin = 10;
    Ticks snoozeUntilUtc = 0;                  // a snoozed alarm rings again then
    Ticks lastFiredUtc = 0;                    // also the "armed at" baseline, see Arm
};

// A civil date and time with minute resolution.
struct LocalTime {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    bool operator==(const LocalTime& o) const {
        return year == o.year && month == o.month && day == o.day && hour == o.hour && minute == o.minute;
    }
};

// The local <-> UTC conversion, injected. ToUtc may answer anything for a local
// time that does not exist (spring-forward gap) or exists twice (fall back);
// the model checks with ToLocal and normalises, so a converter only has to be
// right for ordinary times. ToUtc returns 0 if it cannot convert.
class Zone {
public:
    virtual ~Zone() {}
    virtual LocalTime ToLocal(Ticks utc) const = 0;
    virtual Ticks     ToUtc(const LocalTime& local) const = 0;
};
// The machine's zone, on SystemTimeToTzSpecificLocalTime / TzSpecificLocalTimeToSystemTime.
const Zone& SystemZone();
// Drop the cached per-year rules (the clock or the time zone changed).
void InvalidateSystemZone();
// Once alarm whose date+time is not after now (local); RollPastOnce moves it to the
// next occurrence of that time (today if still ahead, else tomorrow). True if moved.
bool OnceIsPast(const Alarm& a, Ticks nowUtc, const Zone& zone);
bool RollPastOnce(Alarm& a, Ticks nowUtc, const Zone& zone);

// Calendar helpers (proleptic Gregorian, FILETIME epoch 1601-01-01 = a Monday).
bool  LeapYear(int y);
int   DaysInMonth(int y, int m);
Ticks FromCivil(int y, int mo, int d, int h, int mi);     // civil fields -> ticks, no zone
LocalTime ToCivil(Ticks t);                               // the inverse
int   Weekday(int y, int m, int d);                       // 0 = Monday
bool  ValidDate(int y, int m, int d);

// The next UTC instant strictly after max(now, lastFiredUtc) at which the alarm
// rings by its schedule (snooze ignored); 0 = never (disabled, past until, Once in
// the past, nothing selected, or nothing within about five years).
Ticks NextFire(const Alarm& a, Ticks nowUtc, const Zone& zone);
// The same from an explicit instant: strictly after `afterUtc`.
Ticks NextAfter(const Alarm& a, Ticks afterUtc, const Zone& zone);
// The earliest wake-up of this alarm: its schedule or its snooze. 0 = none.
Ticks NextWake(const Alarm& a, Ticks nowUtc, const Zone& zone);

struct Fired {
    int   index = -1;
    Ticks dueUtc = 0;           // when it should have rung
    bool  missed = false;       // due more than kMissedAfter ago: report, do not ring
    bool  snoozed = false;      // a snooze ending, not the schedule
};
// Everything due by `now` is reported once (several missed rings of one alarm
// collapse into one entry) and moved on: lastFiredUtc advances, a snooze clears,
// a Once alarm disables itself, an alarm past its until date disables itself.
std::vector<Fired> TakeDue(std::vector<Alarm>& alarms, Ticks nowUtc, const Zone& zone);

// A new or re-enabled or edited alarm only rings for the future: sets the baseline.
void Arm(Alarm& a, Ticks nowUtc);
void Snooze(Alarm& a, Ticks nowUtc);     // rings again snoozeMin minutes from now
void Dismiss(Alarm& a);                  // cancels a pending snooze

// "Mon, Wed, Fri", "Every hour at :15, 09-17", "1st, 15th and last day · Jan, Jun",
// "Once · Thu 9 Oct" - what the cards show.
std::wstring RepeatSummary(const Alarm& a);

// timers.ini: `acount` and `a<i>.*` keys, appended to / read from the same file
// as the timers. A file without them loads as no alarms.
void AppendIni(std::wstring& out, const std::vector<Alarm>& alarms);
void ReadIni(const std::unordered_map<std::wstring, std::wstring>& kv, std::vector<Alarm>* out);

} // namespace alarm
} // namespace awa
