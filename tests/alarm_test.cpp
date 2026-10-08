// ProWindows - tests for the alarm model (src\alarm.cpp). No window is made and
// the machine's time zone is never consulted except in the one sanity check:
// every function takes the zone, and the tests pass fakes (UTC, and New York
// with a spring-forward gap and a repeated autumn hour).
#include "../src/timer.h"
#include <cstdio>

namespace awa {
void AppTrayBalloon(const wchar_t*, const wchar_t*) {}
void AppScheduleTrim(UINT) {}
void AppSaveConfig() {}
void AppRefreshSettings() {}
void AppUpdateOverlays() {}
void AppOpenClockSettings() {}
}

using namespace awa::alarm;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; \
    wprintf(L"  FAIL line %d: %S\n", __LINE__, #cond); } } while (0)

// ------------------------------------------------------------------ fake zones
namespace {
// Local = UTC.
class UtcZone : public Zone {
public:
    LocalTime ToLocal(Ticks u) const override { return ToCivil(u); }
    Ticks ToUtc(const LocalTime& l) const override { return FromCivil(l.year, l.month, l.day, l.hour, l.minute); }
};

// New York in 2026: EST (-5) until 03-08 07:00Z, EDT (-4) until 11-01 06:00Z, then EST.
// ToUtc is deliberately naive about the two awkward cases, as a real API may be:
// in the gap it answers with a wrong instant, and for the repeated hour it
// answers with the later occurrence.
class NyZone : public Zone {
public:
    static int OffsetMin(Ticks u) {
        if (u < FromCivil(2026, 3, 8, 7, 0)) return -300;
        if (u < FromCivil(2026, 11, 1, 6, 0)) return -240;
        return -300;
    }
    LocalTime ToLocal(Ticks u) const override { return ToCivil(u + OffsetMin(u) * kMinute); }
    Ticks ToUtc(const LocalTime& l) const override {
        const Ticks local = FromCivil(l.year, l.month, l.day, l.hour, l.minute);
        const int offs[3] = { -300, -240, -300 };
        Ticks found = 0;
        for (int o : offs) {
            const Ticks u = local - o * kMinute;
            if (OffsetMin(u) == o) found = u;          // the last (later) match wins
        }
        return found ? found : local + 300 * kMinute;  // inside the gap: wrong on purpose
    }
};

Ticks U(int y, int mo, int d, int h = 0, int mi = 0) { return FromCivil(y, mo, d, h, mi); }

Alarm Daily(int h, int m) { Alarm a; a.kind = Kind::Daily; a.hour = h; a.minute = m; return a; }
Alarm OnceAt(int y, int mo, int d, int h, int m) {
    Alarm a; a.kind = Kind::Once; a.year = y; a.month = mo; a.day = d; a.hour = h; a.minute = m; return a;
}
constexpr uint32_t Mon = 1, Tue = 2, Wed = 4, Thu = 8, Fri = 16, Sat = 32, Sun = 64;
}

static const UtcZone utc;
static const NyZone ny;

static void Calendar() {
    wprintf(L"calendar\n");
    CHECK(Weekday(2026, 10, 8) == 3);                 // Thursday
    CHECK(Weekday(1601, 1, 1) == 0);
    CHECK(DaysInMonth(2028, 2) == 29 && DaysInMonth(2027, 2) == 28 && DaysInMonth(2100, 2) == 28);
    CHECK(DaysInMonth(2000, 2) == 29 && DaysInMonth(2026, 4) == 30 && DaysInMonth(2026, 12) == 31);
    const LocalTime t = ToCivil(U(2026, 12, 31, 23, 59));
    CHECK(t == (LocalTime{ 2026, 12, 31, 23, 59 }));
    CHECK(ToCivil(U(2028, 2, 28, 23, 59) + kMinute) == (LocalTime{ 2028, 2, 29, 0, 0 }));
    CHECK(ToCivil(U(2027, 2, 28, 23, 59) + kMinute) == (LocalTime{ 2027, 3, 1, 0, 0 }));
    CHECK(ValidDate(2028, 2, 29) && !ValidDate(2027, 2, 29) && !ValidDate(2026, 13, 1));
}

static void OnceKind() {
    wprintf(L"once\n");
    Alarm a = OnceAt(2026, 10, 9, 9, 0);
    CHECK(NextFire(a, U(2026, 10, 8, 12), utc) == U(2026, 10, 9, 9, 0));
    CHECK(NextFire(a, U(2026, 10, 9, 8, 59), utc) == U(2026, 10, 9, 9, 0));
    CHECK(NextFire(a, U(2026, 10, 9, 9, 0), utc) == 0);          // strictly after
    CHECK(NextFire(a, U(2026, 10, 10, 0), utc) == 0);
    a.lastFiredUtc = U(2026, 10, 9, 9, 0);
    CHECK(NextFire(a, U(2026, 10, 8, 12), utc) == 0);            // already rang
    a.lastFiredUtc = 0; a.enabled = false;
    CHECK(NextFire(a, U(2026, 10, 8, 12), utc) == 0);
    CHECK(NextFire(OnceAt(2026, 2, 30, 9, 0), U(2026, 1, 1), utc) == 0);   // not a date
    // "Just this hour": Once today at the chosen minute.
    CHECK(NextFire(OnceAt(2026, 10, 8, 14, 20), U(2026, 10, 8, 14, 0), utc) == U(2026, 10, 8, 14, 20));
}

static void DailyHourly() {
    wprintf(L"daily and hourly\n");
    Alarm d = Daily(7, 30);
    CHECK(NextFire(d, U(2026, 10, 8, 7, 29), utc) == U(2026, 10, 8, 7, 30));
    CHECK(NextFire(d, U(2026, 10, 8, 7, 30), utc) == U(2026, 10, 9, 7, 30));
    CHECK(NextFire(d, U(2026, 12, 31, 8), utc) == U(2027, 1, 1, 7, 30));
    d.weekdays = Sat | Sun;
    CHECK(NextFire(d, U(2026, 10, 8, 8), utc) == U(2026, 10, 10, 7, 30));   // Saturday

    Alarm h; h.kind = Kind::Hourly; h.minute = 15; h.hourFrom = 9; h.hourTo = 17;
    CHECK(NextFire(h, U(2026, 10, 8, 8, 0), utc) == U(2026, 10, 8, 9, 15));
    CHECK(NextFire(h, U(2026, 10, 8, 9, 15), utc) == U(2026, 10, 8, 10, 15));
    CHECK(NextFire(h, U(2026, 10, 8, 17, 15), utc) == U(2026, 10, 9, 9, 15));
    h.weekdays = Mon | Tue | Wed | Thu | Fri;
    CHECK(NextFire(h, U(2026, 10, 9, 17, 16), utc) == U(2026, 10, 12, 9, 15));   // Fri -> Mon
    Alarm all; all.kind = Kind::Hourly; all.minute = 0;
    CHECK(NextFire(all, U(2026, 10, 8, 23, 30), utc) == U(2026, 10, 9, 0, 0));
    Alarm wrap = h; wrap.weekdays = 0; wrap.hourFrom = 22; wrap.hourTo = 6;     // across midnight
    CHECK(NextFire(wrap, U(2026, 10, 8, 8, 0), utc) == U(2026, 10, 8, 22, 15));
    CHECK(NextFire(wrap, U(2026, 10, 8, 22, 15), utc) == U(2026, 10, 8, 23, 15));
    CHECK(NextFire(wrap, U(2026, 10, 8, 23, 15), utc) == U(2026, 10, 9, 0, 15));
    CHECK(NextFire(wrap, U(2026, 10, 9, 6, 15), utc) == U(2026, 10, 9, 22, 15));
    CHECK(RepeatSummary(wrap) == L"Every hour at :15, 22-06");
    // A Once alarm saved for a moment already past rolls to the next occurrence.
    Alarm past = OnceAt(2026, 10, 8, 9, 0);
    CHECK(OnceIsPast(past, U(2026, 10, 8, 12), utc));
    CHECK(!OnceIsPast(past, U(2026, 10, 8, 8), utc));
    CHECK(RollPastOnce(past, U(2026, 10, 8, 12), utc) && past.day == 9 && past.month == 10);
    Alarm old = OnceAt(2026, 9, 1, 15, 0);
    CHECK(RollPastOnce(old, U(2026, 10, 8, 12), utc) && old.day == 8 && old.month == 10);   // still ahead today
    Alarm fut = OnceAt(2026, 10, 9, 9, 0);
    CHECK(!RollPastOnce(fut, U(2026, 10, 8, 12), utc));
}

static void WeeklyMonthly() {
    wprintf(L"weekly, monthly, weeks, months, until\n");
    Alarm w; w.kind = Kind::Weekly; w.hour = 8; w.minute = 0; w.weekdays = Mon | Wed | Fri;
    CHECK(NextFire(w, U(2026, 10, 8, 12), utc) == U(2026, 10, 9, 8, 0));         // Thu -> Fri
    CHECK(NextFire(w, U(2026, 10, 9, 8, 0), utc) == U(2026, 10, 12, 8, 0));      // Fri -> Mon
    w.weekdays = 0;
    CHECK(NextFire(w, U(2026, 10, 8, 12), utc) == 0);                            // nothing chosen

    // Weeks of the month: first Monday, last Sunday.
    Alarm first = w; first.weekdays = Mon; first.weeks = 1;
    CHECK(NextFire(first, U(2026, 10, 8, 12), utc) == U(2026, 11, 2, 8, 0));
    Alarm last = w; last.weekdays = Sun; last.weeks = kLastWeekBit;
    CHECK(NextFire(last, U(2026, 10, 8, 12), utc) == U(2026, 10, 25, 8, 0));
    CHECK(NextFire(last, U(2026, 10, 25, 12), utc) == U(2026, 11, 29, 8, 0));
    Alarm fifth = w; fifth.weekdays = Thu; fifth.weeks = 1u << 4;                // a 5th Thursday
    CHECK(NextFire(fifth, U(2026, 10, 8, 12), utc) == U(2026, 10, 29, 8, 0));
    CHECK(NextFire(fifth, U(2026, 10, 29, 12), utc) == U(2026, 12, 31, 8, 0));   // Nov has only four

    // Monthly: day 31 skips months without it; Last day always matches.
    Alarm m; m.kind = Kind::Monthly; m.hour = 7; m.minute = 0; m.monthDays = 1u << 30;
    CHECK(NextFire(m, U(2026, 1, 31, 12), utc) == U(2026, 3, 31, 7, 0));
    CHECK(NextFire(m, U(2026, 4, 1, 0), utc) == U(2026, 5, 31, 7, 0));
    Alarm m30 = m; m30.monthDays = 1u << 29;
    CHECK(NextFire(m30, U(2026, 1, 30, 12), utc) == U(2026, 3, 30, 7, 0));
    Alarm ld = m; ld.monthDays = kLastDayBit;
    CHECK(NextFire(ld, U(2028, 2, 1, 0), utc) == U(2028, 2, 29, 7, 0));          // leap
    CHECK(NextFire(ld, U(2027, 2, 1, 0), utc) == U(2027, 2, 28, 7, 0));
    CHECK(NextFire(ld, U(2026, 4, 1, 0), utc) == U(2026, 4, 30, 7, 0));
    Alarm mix = m; mix.monthDays = 1u | (1u << 14) | kLastDayBit;                // 1st, 15th, last
    CHECK(NextFire(mix, U(2026, 10, 2, 0), utc) == U(2026, 10, 15, 7, 0));
    CHECK(NextFire(mix, U(2026, 10, 15, 8), utc) == U(2026, 10, 31, 7, 0));
    CHECK(NextFire(mix, U(2026, 10, 31, 8), utc) == U(2026, 11, 1, 7, 0));
    Alarm none = m; none.monthDays = 0;
    CHECK(NextFire(none, U(2026, 10, 2, 0), utc) == 0);
    Alarm feb31 = m; feb31.months = 1u << 1;                                     // 31st in February: never
    CHECK(NextFire(feb31, U(2026, 1, 1, 0), utc) == 0);
    Alarm feb29 = m; feb29.monthDays = 1u << 28; feb29.months = 1u << 1;         // Feb 29: within five years
    CHECK(NextFire(feb29, U(2026, 1, 1, 0), utc) == U(2028, 2, 29, 7, 0));

    // Months limit.
    Alarm d = Daily(7, 0); d.months = (1u << 11) | 1u;                           // Dec, Jan
    CHECK(NextFire(d, U(2026, 10, 8, 12), utc) == U(2026, 12, 1, 7, 0));
    CHECK(NextFire(d, U(2026, 12, 31, 12), utc) == U(2027, 1, 1, 7, 0));

    // Until.
    Alarm u = Daily(7, 0); u.until = 20261010;
    CHECK(NextFire(u, U(2026, 10, 8, 12), utc) == U(2026, 10, 9, 7, 0));
    CHECK(NextFire(u, U(2026, 10, 9, 12), utc) == U(2026, 10, 10, 7, 0));        // the until day itself rings
    CHECK(NextFire(u, U(2026, 10, 10, 12), utc) == 0);
}

static void Dst() {
    wprintf(L"daylight saving with a fake New York\n");
    CHECK(ny.ToLocal(U(2026, 3, 8, 7, 0)) == (LocalTime{ 2026, 3, 8, 3, 0 }));
    // 02:30 on 8 March does not exist: it rings at 03:00 EDT = 07:00Z.
    Alarm d = Daily(2, 30);
    CHECK(NextFire(d, U(2026, 3, 7, 12), ny) == U(2026, 3, 8, 7, 0));
    CHECK(NextFire(d, U(2026, 3, 8, 7, 0), ny) == U(2026, 3, 9, 6, 30));         // next day, normal
    Alarm one = OnceAt(2026, 3, 8, 2, 0);
    CHECK(NextFire(one, U(2026, 3, 7, 12), ny) == U(2026, 3, 8, 7, 0));
    // Hourly at :30 across the gap: 01:30 EST, then 03:00 (from 02:30), then 03:30.
    Alarm h; h.kind = Kind::Hourly; h.minute = 30; h.hourFrom = 0; h.hourTo = 5;
    CHECK(NextFire(h, U(2026, 3, 8, 6, 0), ny) == U(2026, 3, 8, 6, 30));
    CHECK(NextFire(h, U(2026, 3, 8, 6, 30), ny) == U(2026, 3, 8, 7, 0));
    CHECK(NextFire(h, U(2026, 3, 8, 7, 0), ny) == U(2026, 3, 8, 7, 30));
    // 01:30 on 1 November happens twice; it rings at the first.
    Alarm r = Daily(1, 30);
    CHECK(NextFire(r, U(2026, 10, 31, 12), ny) == U(2026, 11, 1, 5, 30));        // EDT
    CHECK(NextFire(r, U(2026, 11, 1, 5, 30), ny) == U(2026, 11, 2, 6, 30));      // not the EST repeat
    CHECK(NextFire(r, U(2026, 11, 1, 6, 0), ny) == U(2026, 11, 2, 6, 30));
    // Wall time holds across the change: 07:00 is 12:00Z before, 11:00Z after.
    Alarm m = Daily(7, 0);
    CHECK(NextFire(m, U(2026, 3, 7, 13), ny) == U(2026, 3, 8, 11, 0));
    CHECK(NextFire(m, U(2026, 10, 31, 13), ny) == U(2026, 11, 1, 12, 0));        // 07:00 EST
}

static void Firing() {
    wprintf(L"fired, missed, once, snooze\n");
    // Fires once, on time.
    std::vector<Alarm> v = { Daily(7, 0) };
    Arm(v[0], U(2026, 10, 7, 12));
    auto f = TakeDue(v, U(2026, 10, 8, 6, 59), utc);
    CHECK(f.empty());
    f = TakeDue(v, U(2026, 10, 8, 7, 0) + 5 * kSecond, utc);
    CHECK(f.size() == 1 && f[0].index == 0 && !f[0].missed && f[0].dueUtc == U(2026, 10, 8, 7, 0));
    CHECK(v[0].lastFiredUtc == U(2026, 10, 8, 7, 0));
    CHECK(TakeDue(v, U(2026, 10, 8, 7, 0) + 6 * kSecond, utc).empty());          // reported once
    CHECK(v[0].enabled);

    // Missed while away: one report for four days, flagged, then silence.
    f = TakeDue(v, U(2026, 10, 12, 9, 0), utc);
    CHECK(f.size() == 1 && f[0].missed && f[0].dueUtc == U(2026, 10, 12, 7, 0));
    CHECK(TakeDue(v, U(2026, 10, 12, 9, 0), utc).empty());
    CHECK(NextFire(v[0], U(2026, 10, 12, 9, 0), utc) == U(2026, 10, 13, 7, 0));
    // Just inside the 10 s grace is rung, not missed.
    v[0].lastFiredUtc = U(2026, 10, 12, 7, 0);
    f = TakeDue(v, U(2026, 10, 13, 7, 0) + 10 * kSecond, utc);
    CHECK(f.size() == 1 && !f[0].missed);
    f = TakeDue(v, U(2026, 10, 14, 7, 0) + 11 * kSecond, utc);
    CHECK(f.size() == 1 && f[0].missed);

    // A new alarm never reports earlier times today.
    std::vector<Alarm> n = { Daily(7, 0) };
    Arm(n[0], U(2026, 10, 8, 12));
    CHECK(TakeDue(n, U(2026, 10, 8, 13), utc).empty());

    // Once disables itself after it fires (kept), and can be re-armed.
    std::vector<Alarm> o = { OnceAt(2026, 10, 9, 9, 0) };
    Arm(o[0], U(2026, 10, 8, 12));
    f = TakeDue(o, U(2026, 10, 9, 9, 0), utc);
    CHECK(f.size() == 1 && !o[0].enabled);
    CHECK(TakeDue(o, U(2026, 10, 9, 9, 1), utc).empty());
    o[0].year = 2026; o[0].day = 10; o[0].enabled = true; Arm(o[0], U(2026, 10, 9, 12));
    CHECK(NextFire(o[0], U(2026, 10, 9, 12), utc) == U(2026, 10, 10, 9, 0));
    // A Once missed while away disables too, and is flagged.
    std::vector<Alarm> o2 = { OnceAt(2026, 10, 9, 9, 0) };
    Arm(o2[0], U(2026, 10, 8, 12));
    f = TakeDue(o2, U(2026, 10, 11, 8), utc);
    CHECK(f.size() == 1 && f[0].missed && !o2[0].enabled);

    // Past its until date, an alarm disables itself.
    std::vector<Alarm> un = { Daily(7, 0) };
    un[0].until = 20261009;
    Arm(un[0], U(2026, 10, 8, 12));
    f = TakeDue(un, U(2026, 10, 9, 7, 0), utc);
    CHECK(f.size() == 1 && un[0].enabled);                                       // the last day rings
    TakeDue(un, U(2026, 10, 10, 0, 1), utc);
    CHECK(!un[0].enabled);

    // Snooze: rings again N minutes later, even for a Once that disabled itself.
    std::vector<Alarm> s = { OnceAt(2026, 10, 9, 9, 0) };
    s[0].snoozeMin = 5;
    Arm(s[0], U(2026, 10, 8, 12));
    TakeDue(s, U(2026, 10, 9, 9, 0), utc);
    Snooze(s[0], U(2026, 10, 9, 9, 0));
    CHECK(s[0].snoozeUntilUtc == U(2026, 10, 9, 9, 5));
    CHECK(NextWake(s[0], U(2026, 10, 9, 9, 1), utc) == U(2026, 10, 9, 9, 5));
    CHECK(TakeDue(s, U(2026, 10, 9, 9, 4), utc).empty());
    f = TakeDue(s, U(2026, 10, 9, 9, 5), utc);
    CHECK(f.size() == 1 && f[0].snoozed && !f[0].missed && s[0].snoozeUntilUtc == 0);
    CHECK(TakeDue(s, U(2026, 10, 9, 9, 6), utc).empty());
    // Dismiss cancels a pending snooze; a repeating alarm's snooze beats its schedule.
    std::vector<Alarm> dd = { Daily(7, 0) };
    Arm(dd[0], U(2026, 10, 8, 6));
    TakeDue(dd, U(2026, 10, 8, 7, 0), utc);
    Snooze(dd[0], U(2026, 10, 8, 7, 0));
    CHECK(NextWake(dd[0], U(2026, 10, 8, 7, 1), utc) == U(2026, 10, 8, 7, 10));
    Dismiss(dd[0]);
    CHECK(NextWake(dd[0], U(2026, 10, 8, 7, 1), utc) == U(2026, 10, 9, 7, 0));
    // Disabled alarms do not wake anyone; the clock set back does not ring early.
    dd[0].enabled = false;
    CHECK(NextWake(dd[0], U(2026, 10, 8, 7, 1), utc) == 0);
    dd[0].enabled = true; dd[0].lastFiredUtc = U(2026, 10, 20, 7, 0);
    CHECK(NextFire(dd[0], U(2026, 10, 8, 7, 1), utc) == U(2026, 10, 21, 7, 0));
}

static void Summary() {
    wprintf(L"repeat summaries\n");
    const std::wstring dot = L" · ";
    Alarm w; w.kind = Kind::Weekly; w.weekdays = Mon | Wed | Fri;
    CHECK(RepeatSummary(w) == L"Mon, Wed, Fri");
    w.weekdays = Mon | Tue | Wed | Thu | Fri;
    CHECK(RepeatSummary(w) == L"Weekdays");
    w.weekdays = Sat | Sun;
    CHECK(RepeatSummary(w) == L"Weekends");
    Alarm h; h.kind = Kind::Hourly; h.minute = 15; h.hourFrom = 9; h.hourTo = 17;
    CHECK(RepeatSummary(h) == L"Every hour at :15, 09-17");
    h.hourFrom = 0; h.hourTo = 23; h.minute = 5;
    CHECK(RepeatSummary(h) == L"Every hour at :05");
    Alarm m; m.kind = Kind::Monthly; m.monthDays = 1u | (1u << 14) | kLastDayBit; m.months = 1u | (1u << 5);
    CHECK(RepeatSummary(m) == L"1st, 15th and last day" + dot + L"Jan, Jun");
    m.monthDays = kLastDayBit; m.months = 0;
    CHECK(RepeatSummary(m) == L"Last day of the month");
    m.monthDays = (1u << 1) | (1u << 10) | (1u << 20) | (1u << 22);
    CHECK(RepeatSummary(m) == L"2nd, 11th, 21st and 23rd");
    Alarm o = OnceAt(2026, 10, 9, 9, 0);
    CHECK(RepeatSummary(o) == L"Once" + dot + L"Fri 9 Oct");
    Alarm d = Daily(7, 0);
    CHECK(RepeatSummary(d) == L"Every day");
    d.weekdays = Mon | Tue; d.weeks = 1 | kLastWeekBit; d.until = 20261231;
    CHECK(RepeatSummary(d) == L"Mon, Tue" + dot + L"1st and last week" + dot + L"until 31 Dec 2026");
}

static void Storage() {
    wprintf(L"alarms in timers.ini\n");

    awa::timer::State s;
    awa::timer::Timer t; t.label = L"Tea"; t.duration = 5 * awa::timer::kSecond; t.remaining = 5 * awa::timer::kSecond;
    s.timers.push_back(t);
    Alarm a = OnceAt(2026, 10, 9, 9, 30);
    a.label = L"Dentist \x00E9"; a.sound = false; a.snoozeMin = 15; a.snoozeUntilUtc = 777; a.lastFiredUtc = 555;
    Alarm b; b.kind = Kind::Monthly; b.enabled = false; b.hour = 23; b.minute = 59;
    b.monthDays = 1u | kLastDayBit; b.weeks = 0x21; b.months = 0xFFF; b.until = 20271231;
    Alarm c; c.kind = Kind::Hourly; c.hourFrom = 9; c.hourTo = 17; c.minute = 15; c.weekdays = 0x1F;
    s.alarms = { a, b, c };
    awa::timer::State back;
    CHECK(awa::timer::Deserialize(awa::timer::Serialize(s), &back));
    CHECK(back.timers.size() == 1 && back.timers[0].label == L"Tea");
    CHECK(back.alarms.size() == 3);
    if (back.alarms.size() == 3) {
        const Alarm& x = back.alarms[0];
        CHECK(x.label == a.label && x.kind == Kind::Once && x.year == 2026 && x.month == 10 && x.day == 9);
        CHECK(x.hour == 9 && x.minute == 30 && !x.sound && x.snoozeMin == 15 && x.enabled);
        CHECK(x.snoozeUntilUtc == 777 && x.lastFiredUtc == 555);
        const Alarm& y = back.alarms[1];
        CHECK(y.kind == Kind::Monthly && !y.enabled && y.hour == 23 && y.minute == 59);
        CHECK(y.monthDays == (1u | kLastDayBit) && y.weeks == 0x21 && y.months == 0xFFF && y.until == 20271231);
        const Alarm& z = back.alarms[2];
        CHECK(z.kind == Kind::Hourly && z.hourFrom == 9 && z.hourTo == 17 && z.minute == 15 && z.weekdays == 0x1F);
    }
    // A file from before alarms still loads, with none.
    awa::timer::State old;
    old.alarms.push_back(a);
    CHECK(awa::timer::Deserialize(L"count = 1\nt0.label = x\nt0.duration = 10\nwatch.running = 0\n", &old));
    CHECK(old.timers.size() == 1 && old.alarms.empty());
    // Garbage values are clamped, a long label is cut, and the cap holds.
    CHECK(awa::timer::Deserialize(L"count = 0\nacount = 99\na0.kind = 9\na0.hour = 77\na0.label = 0123456789012345678901234567890123456789XYZ\n", &old));
    CHECK(old.alarms.size() == (size_t)kMaxAlarms);
    CHECK((int)old.alarms[0].kind == 4 && old.alarms[0].hour == 23 && old.alarms[0].label.size() == 40);
    // Through a file.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"pw_alarm_test.ini";
    CHECK(awa::timer::SaveFile(path, s));
    awa::timer::State disk;
    CHECK(awa::timer::LoadFile(path, &disk) && disk.alarms.size() == 3 && disk.alarms[0].label == a.label);
    DeleteFileW(path.c_str());
}

static void Real() {
    wprintf(L"the machine's zone (sanity only)\n");
    const Zone& z = SystemZone();
    const Ticks noon = z.ToUtc(LocalTime{ 2026, 6, 15, 12, 0 });
    CHECK(noon > 0 && z.ToLocal(noon) == (LocalTime{ 2026, 6, 15, 12, 0 }));
    const Ticks now = U(2026, 10, 8, 12);
    Alarm d = Daily(7, 0);
    const Ticks n = NextFire(d, now, z);
    CHECK(n > now && n <= now + 26 * 3600 * kSecond);
    CHECK(z.ToLocal(n).hour == 7 && z.ToLocal(n).minute == 0);
}

int wmain() {
    Calendar();
    OnceKind();
    DailyHourly();
    WeeklyMonthly();
    Dst();
    Firing();
    Summary();
    Storage();
    Real();
    wprintf(L"\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
