#include "alarm.h"
#include <cwchar>

namespace awa {
namespace alarm {

// ------------------------------------------------------------------ calendar
// Hinnant's days-from-civil, shifted to the FILETIME epoch (1601-01-01 is a Monday,
// so day % 7 is the weekday with Monday = 0).
namespace {
constexpr long long kDaysTo1970 = 134774;

long long DayNumber(int y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468 + kDaysTo1970;
}

void CivilFromDay(long long z, int* y, int* m, int* d) {
    z += 719468 - kDaysTo1970;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

int Ymd(int y, int m, int d) { return y * 10000 + m * 100 + d; }
}

bool LeapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int DaysInMonth(int y, int m) {
    static const int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) return 30;
    return m == 2 && LeapYear(y) ? 29 : dim[m - 1];
}

bool ValidDate(int y, int m, int d) {
    return y >= 1601 && y <= 9999 && m >= 1 && m <= 12 && d >= 1 && d <= DaysInMonth(y, m);
}

Ticks FromCivil(int y, int mo, int d, int h, int mi) {
    return ((DayNumber(y, mo, d) * 1440 + h * 60 + mi) * 60) * kSecond;
}

LocalTime ToCivil(Ticks t) {
    LocalTime r;
    if (t < 0) t = 0;
    const long long minutes = t / kMinute;
    const long long day = minutes / 1440;
    CivilFromDay(day, &r.year, &r.month, &r.day);
    r.hour = (int)((minutes % 1440) / 60);
    r.minute = (int)(minutes % 60);
    return r;
}

int Weekday(int y, int m, int d) { return (int)(DayNumber(y, m, d) % 7); }

// ------------------------------------------------------------------ the zone
namespace {
class SysZone : public Zone {
public:
    LocalTime ToLocal(Ticks utc) const override {
        FILETIME ft;
        ft.dwLowDateTime = (DWORD)(utc & 0xFFFFFFFF);
        ft.dwHighDateTime = (DWORD)((uint64_t)utc >> 32);
        SYSTEMTIME su, sl;
        LocalTime r;
        if (!FileTimeToSystemTime(&ft, &su) || !SystemTimeToTzSpecificLocalTime(nullptr, &su, &sl))
            return ToCivil(utc);                        // no zone data: treat UTC as local
        r.year = sl.wYear; r.month = sl.wMonth; r.day = sl.wDay;
        r.hour = sl.wHour; r.minute = sl.wMinute;
        return r;
    }
    Ticks ToUtc(const LocalTime& lt) const override {
        SYSTEMTIME sl = {}, su;
        sl.wYear = (WORD)lt.year; sl.wMonth = (WORD)lt.month; sl.wDay = (WORD)lt.day;
        sl.wHour = (WORD)lt.hour; sl.wMinute = (WORD)lt.minute;
        FILETIME ft;
        if (!TzSpecificLocalTimeToSystemTime(nullptr, &sl, &su) || !SystemTimeToFileTime(&su, &ft))
            return 0;
        return ((Ticks)ft.dwHighDateTime << 32) | (Ticks)ft.dwLowDateTime;
    }
};
}

const Zone& SystemZone() {
    static const SysZone z;
    return z;
}

namespace {
// The instant a local wall-clock time happens, whatever the converter does with
// it. A time inside a spring-forward gap does not exist: it rings at the first
// minute after the gap. A time that repeats (fall back) rings at its first
// occurrence only, because the search is for instants strictly after the last
// ring and callers only ask for the earliest.
Ticks Resolve(const Zone& zone, LocalTime cur) {
    for (int step = 0; step < 180; ++step) {
        const Ticks u = zone.ToUtc(cur);
        if (u > 0 && zone.ToLocal(u) == cur) {
            for (Ticks back : { 3600 * kSecond, 1800 * kSecond })
                if (zone.ToLocal(u - back) == cur) return u - back;
            return u;
        }
        cur = ToCivil(FromCivil(cur.year, cur.month, cur.day, cur.hour, cur.minute) + kMinute);
    }
    return 0;
}

bool IsLastWeek(int day, int dim) { return day + 7 > dim; }

bool DayMatches(const Alarm& a, int y, int m, int d, int dow) {
    if (a.kind == Kind::Once) return y == a.year && m == a.month && d == a.day;
    if (a.months && !(a.months & (1u << (m - 1)))) return false;
    if (a.weeks) {
        const int dim = DaysInMonth(y, m);
        bool ok = (a.weeks & (1u << ((d - 1) / 7))) != 0;
        if (!ok && IsLastWeek(d, dim)) ok = (a.weeks & kLastWeekBit) != 0;
        if (!ok) return false;
    }
    switch (a.kind) {
    case Kind::Hourly:
    case Kind::Daily:
        return !a.weekdays || (a.weekdays & (1u << dow));
    case Kind::Weekly:
        return (a.weekdays & (1u << dow)) != 0;
    case Kind::Monthly: {
        if (a.monthDays & (1u << (d - 1)) & 0x7FFFFFFFu) return true;
        return (a.monthDays & kLastDayBit) && d == DaysInMonth(y, m);
    }
    default: return false;
    }
}

constexpr int kHorizonDays = 5 * 366 + 1;     // "never" past about five years
}

Ticks NextAfter(const Alarm& a, Ticks after, const Zone& zone) {
    if (!a.enabled) return 0;
    if (a.kind == Kind::Once && !ValidDate(a.year, a.month, a.day)) return 0;
    const LocalTime base = zone.ToLocal(after);
    const long long day0 = DayNumber(base.year, base.month, base.day);
    int lo = a.hour, hi = a.hour;
    if (a.kind == Kind::Hourly) {
        lo = a.hourFrom < a.hourTo ? a.hourFrom : a.hourTo;
        hi = a.hourFrom < a.hourTo ? a.hourTo : a.hourFrom;
    }
    if (lo < 0) lo = 0;
    if (hi > 23) hi = 23;
    const int minute = a.minute < 0 ? 0 : (a.minute > 59 ? 59 : a.minute);

    for (int i = 0; i < kHorizonDays; ++i) {
        int y, m, d;
        CivilFromDay(day0 + i, &y, &m, &d);
        if (a.until && Ymd(y, m, d) > a.until) return 0;
        if (!DayMatches(a, y, m, d, (int)((day0 + i) % 7))) continue;
        // Resolve never goes backwards as the hour grows (a gap moves forward, a
        // repeat takes its first instant), so the first hour past `after` is the
        // earliest: stop there. An hourly alarm walked over a week asleep would
        // otherwise convert all 24 hours of every day, on the UI thread.
        Ticks best = 0;
        for (int h = lo; h <= hi && !best; ++h) {
            LocalTime lt; lt.year = y; lt.month = m; lt.day = d; lt.hour = h; lt.minute = minute;
            const Ticks u = Resolve(zone, lt);
            if (u > after) best = u;
        }
        if (best) return best;
        if (a.kind == Kind::Once) return 0;       // its one day has passed
    }
    return 0;
}

Ticks NextFire(const Alarm& a, Ticks nowUtc, const Zone& zone) {
    return NextAfter(a, nowUtc > a.lastFiredUtc ? nowUtc : a.lastFiredUtc, zone);
}

Ticks NextWake(const Alarm& a, Ticks nowUtc, const Zone& zone) {
    Ticks t = NextFire(a, nowUtc, zone);
    if (a.snoozeUntilUtc > 0 && (t == 0 || a.snoozeUntilUtc < t)) t = a.snoozeUntilUtc;
    return t;
}

std::vector<Fired> TakeDue(std::vector<Alarm>& alarms, Ticks now, const Zone& zone) {
    std::vector<Fired> out;
    for (size_t i = 0; i < alarms.size(); ++i) {
        Alarm& a = alarms[i];
        Fired f;
        f.index = (int)i;
        bool any = false;

        // A snooze runs out even if the alarm was a Once that disabled itself.
        if (a.snoozeUntilUtc > 0 && a.snoozeUntilUtc <= now) {
            f.dueUtc = a.snoozeUntilUtc;
            f.snoozed = true;
            any = true;
            a.snoozeUntilUtc = 0;
        }
        // Never armed (a hand-written timers.ini): the walk would start in 1601 and
        // find nothing, so it would never ring. It rings from now on instead.
        if (a.enabled && a.lastFiredUtc <= 0) a.lastFiredUtc = now;
        if (a.enabled) {
            // Walk every ring since the last one; a week asleep is one report, not 168.
            Ticks cur = a.lastFiredUtc, last = 0;
            bool capped = true;
            for (int n = 0; n < 2000; ++n) {
                const Ticks d = NextAfter(a, cur, zone);
                if (!d || d > now) { capped = false; break; }
                last = cur = d;
                if (a.kind == Kind::Once) { capped = false; break; }
            }
            if (last) {
                a.lastFiredUtc = capped ? now : last;
                if (!any || last > f.dueUtc) { f.dueUtc = last; f.snoozed = false; }
                any = true;
                if (a.kind == Kind::Once) a.enabled = false;
            }
            if (a.enabled && a.until) {
                const LocalTime l = zone.ToLocal(now);
                if (Ymd(l.year, l.month, l.day) > a.until) a.enabled = false;
            }
        }
        if (any) {
            f.missed = now - f.dueUtc > kMissedAfter;
            out.push_back(f);
        }
    }
    return out;
}

void Arm(Alarm& a, Ticks nowUtc) {
    a.lastFiredUtc = nowUtc;
    a.snoozeUntilUtc = 0;
}

void Snooze(Alarm& a, Ticks nowUtc) {
    const int m = a.snoozeMin < 1 ? 1 : (a.snoozeMin > 120 ? 120 : a.snoozeMin);
    a.snoozeUntilUtc = nowUtc + m * kMinute;
}

void Dismiss(Alarm& a) { a.snoozeUntilUtc = 0; }

// ------------------------------------------------------------------ summary
namespace {
const wchar_t* const kDays[7]   = { L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun" };
const wchar_t* const kMonths[12] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                     L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };

std::wstring Ordinal(int n) {
    const int r = n % 100;
    const wchar_t* suf = L"th";
    if (r < 11 || r > 13) {
        if (n % 10 == 1) suf = L"st";
        else if (n % 10 == 2) suf = L"nd";
        else if (n % 10 == 3) suf = L"rd";
    }
    return std::to_wstring(n) + suf;
}

// "a", "a and b", "a, b and c".
std::wstring JoinAnd(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += (i + 1 == v.size()) ? L" and " : L", ";
        s += v[i];
    }
    return s;
}

std::wstring Join(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += L", "; s += v[i]; }
    return s;
}

std::wstring WeekdayText(uint32_t bits) {
    bits &= 0x7F;
    if (bits == 0x7F) return L"Every day";
    if (bits == 0x1F) return L"Weekdays";
    if (bits == 0x60) return L"Weekends";
    std::vector<std::wstring> v;
    for (int i = 0; i < 7; ++i) if (bits & (1u << i)) v.push_back(kDays[i]);
    return Join(v);
}
}

std::wstring RepeatSummary(const Alarm& a) {
    const wchar_t* dot = L" · ";
    std::wstring s;
    wchar_t buf[64];
    switch (a.kind) {
    case Kind::Once:
        s = L"Once";
        if (ValidDate(a.year, a.month, a.day)) {
            swprintf_s(buf, L"%s %d %s", kDays[Weekday(a.year, a.month, a.day)], a.day, kMonths[a.month - 1]);
            s += dot; s += buf;
        }
        return s;                                  // limits do not apply to a single date
    case Kind::Hourly: {
        swprintf_s(buf, L"Every hour at :%02d", a.minute);
        s = buf;
        const int lo = a.hourFrom < a.hourTo ? a.hourFrom : a.hourTo;
        const int hi = a.hourFrom < a.hourTo ? a.hourTo : a.hourFrom;
        if (lo != 0 || hi != 23) { swprintf_s(buf, L", %02d-%02d", lo, hi); s += buf; }
        if (a.weekdays & 0x7F) { s += dot; s += WeekdayText(a.weekdays); }
        break;
    }
    case Kind::Daily:
        s = (a.weekdays & 0x7F) ? WeekdayText(a.weekdays) : L"Every day";
        break;
    case Kind::Weekly:
        s = (a.weekdays & 0x7F) ? WeekdayText(a.weekdays) : L"No day chosen";
        break;
    case Kind::Monthly: {
        std::vector<std::wstring> v;
        for (int d = 1; d <= 31; ++d) if (a.monthDays & (1u << (d - 1))) v.push_back(Ordinal(d));
        const bool last = (a.monthDays & kLastDayBit) != 0;
        if (v.empty() && last) s = L"Last day of the month";
        else if (v.empty())    s = L"No day chosen";
        else {
            if (last) v.push_back(L"last day");
            s = JoinAnd(v);
        }
        break;
    }
    }
    if (a.weeks & 0x3F) {
        std::vector<std::wstring> v;
        for (int w = 0; w < 5; ++w) if (a.weeks & (1u << w)) v.push_back(Ordinal(w + 1));
        if (a.weeks & kLastWeekBit) v.push_back(L"last");
        s += dot; s += JoinAnd(v); s += L" week";
    }
    if (a.months & 0xFFF) {
        std::vector<std::wstring> v;
        for (int m = 0; m < 12; ++m) if (a.months & (1u << m)) v.push_back(kMonths[m]);
        s += dot; s += Join(v);
    }
    if (a.until) {
        const int y = a.until / 10000, m = a.until / 100 % 100, d = a.until % 100;
        if (m >= 1 && m <= 12) {
            swprintf_s(buf, L"until %d %s %d", d, kMonths[m - 1], y);
            s += dot; s += buf;
        }
    }
    return s;
}

// ------------------------------------------------------------------ timers.ini
void AppendIni(std::wstring& out, const std::vector<Alarm>& alarms) {
    out += L"acount = " + std::to_wstring(alarms.size()) + L"\n";
    for (size_t i = 0; i < alarms.size(); ++i) {
        const Alarm& a = alarms[i];
        const std::wstring p = L"a" + std::to_wstring(i) + L".";
        std::wstring label = a.label;
        for (auto& c : label) if (c == L'\r' || c == L'\n') c = L' ';
        auto num = [&](const wchar_t* k, long long v) { out += p + k + L" = " + std::to_wstring(v) + L"\n"; };
        out += p + L"label = " + label + L"\n";
        num(L"enabled", a.enabled);
        num(L"hour", a.hour);
        num(L"minute", a.minute);
        num(L"kind", (int)a.kind);
        num(L"year", a.year);
        num(L"month", a.month);
        num(L"day", a.day);
        num(L"from", a.hourFrom);
        num(L"to", a.hourTo);
        num(L"weekdays", a.weekdays);
        num(L"monthdays", a.monthDays);
        num(L"weeks", a.weeks);
        num(L"months", a.months);
        num(L"until", a.until);
        num(L"sound", a.sound);
        num(L"snooze", a.snoozeMin);
        num(L"snoozeuntil", a.snoozeUntilUtc);
        num(L"lastfired", a.lastFiredUtc);
    }
}

namespace {
long long Int(const std::unordered_map<std::wstring, std::wstring>& kv, const std::wstring& key,
              long long def, long long lo, long long hi) {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    const long long v = _wtoi64(it->second.c_str());
    return v < lo ? lo : (v > hi ? hi : v);
}
}

void ReadIni(const std::unordered_map<std::wstring, std::wstring>& kv, std::vector<Alarm>* out) {
    out->clear();
    const int count = (int)Int(kv, L"acount", 0, 0, kMaxAlarms);
    for (int i = 0; i < count; ++i) {
        const std::wstring p = L"a" + std::to_wstring(i) + L".";
        Alarm a;
        auto lab = kv.find(p + L"label");
        if (lab != kv.end()) a.label = lab->second.substr(0, kMaxLabel);
        a.enabled   = Int(kv, p + L"enabled", 1, 0, 1) != 0;
        a.hour      = (int)Int(kv, p + L"hour", 7, 0, 23);
        a.minute    = (int)Int(kv, p + L"minute", 0, 0, 59);
        a.kind      = (Kind)Int(kv, p + L"kind", 0, 0, 4);
        a.year      = (int)Int(kv, p + L"year", 0, 0, 9999);
        a.month     = (int)Int(kv, p + L"month", 0, 0, 12);
        a.day       = (int)Int(kv, p + L"day", 0, 0, 31);
        a.hourFrom  = (int)Int(kv, p + L"from", 0, 0, 23);
        a.hourTo    = (int)Int(kv, p + L"to", 23, 0, 23);
        a.weekdays  = (uint32_t)Int(kv, p + L"weekdays", 0, 0, 0x7F);
        a.monthDays = (uint32_t)Int(kv, p + L"monthdays", 0, 0, 0xFFFFFFFFLL);
        a.weeks     = (uint32_t)Int(kv, p + L"weeks", 0, 0, 0x3F);
        a.months    = (uint32_t)Int(kv, p + L"months", 0, 0, 0xFFF);
        a.until     = (int)Int(kv, p + L"until", 0, 0, 99991231);
        a.sound     = Int(kv, p + L"sound", 1, 0, 1) != 0;
        a.snoozeMin = (int)Int(kv, p + L"snooze", 10, 1, 120);
        a.snoozeUntilUtc = Int(kv, p + L"snoozeuntil", 0, 0, INT64_MAX / 2);
        a.lastFiredUtc   = Int(kv, p + L"lastfired", 0, 0, INT64_MAX / 2);
        out->push_back(a);
    }
}

} // namespace alarm
} // namespace awa
