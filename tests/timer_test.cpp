// ProWindows - tests for the timer model (src\timer.cpp). No window is made:
// every function takes the time it should use, so a three-day gap is one line.
#include "../src/timer.h"
#include <cstdio>

// timer.cpp reaches the tray and the memory trim through these.
namespace awa {
void AppTrayBalloon(const wchar_t*, const wchar_t*) {}
void AppScheduleTrim(UINT) {}
void AppSaveConfig() {}
void AppRefreshSettings() {}
void AppUpdateOverlays() {}
void AppOpenClockSettings() {}
}

using namespace awa::timer;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; \
    wprintf(L"  FAIL line %d: %S\n", __LINE__, #cond); } } while (0)

static void RoundTrip() {
    wprintf(L"parse / format round trip\n");
    const wchar_t* cases[] = { L"0:00:00", L"0:00:01", L"1:02:03", L"23:59:59",
                               L"1d 00:00:00", L"9999d 23:59:59", L"12d 04:05:06" };
    for (const wchar_t* c : cases) {
        Ticks t = -1;
        CHECK(ParseDuration(c, &t));
        CHECK(FormatDuration(t) == c);
    }
    Ticks t = 0;
    CHECK(ParseDuration(L"9999d 23:59:59", &t) && t == kMaxDuration);
    CHECK(!ParseDuration(L"10000d", &t));
    CHECK(!ParseDuration(L"9999d 23:59:60", &t));
    CHECK(!ParseDuration(L"abc", &t));
    CHECK(ParseDuration(L"5:30", &t) && t == (5 * 60 + 30) * kSecond);
    CHECK(ParseDuration(L"90", &t) && t == 90 * kSecond);
    CHECK(FormatDuration(kSecond * 3 + kSecond / 10 * 7, true) == L"0:00:03.7");
    CHECK(CeilSecond(kSecond + 1) == 2 * kSecond && CeilSecond(0) == 0);
    // And through the file text.
    State s;
    Timer a; a.label = L"Tea \x00E9"; a.duration = 5 * 60 * kSecond; a.running = true; a.endUtc = 123456789;
    Timer b; b.label = L"Bread"; b.duration = kMaxDuration; b.remaining = 77 * kSecond;
    s.timers = { a, b };
    s.watch.running = true; s.watch.startUtc = 999; s.watch.banked = 42; s.watch.lapSeq = 7;
    s.watch.laps = { 300, 200, 100 };
    State back;
    CHECK(Deserialize(Serialize(s), &back));
    CHECK(back.timers.size() == 2 && back.timers[0].label == a.label && back.timers[0].running);
    CHECK(back.timers[0].endUtc == 123456789 && back.timers[1].duration == kMaxDuration);
    CHECK(back.timers[1].remaining == 77 * kSecond && !back.timers[1].running);
    CHECK(back.watch.running && back.watch.startUtc == 999 && back.watch.banked == 42);
    CHECK(back.watch.laps.size() == 3 && back.watch.laps[0] == 300 && back.watch.lapSeq == 7);
    CHECK(!Deserialize(L"garbage", &back));
}

static void ThreeDayGap() {
    wprintf(L"remaining time across a 3-day gap\n");
    const Ticks t0 = 132000000000000000LL;          // some moment
    Timer t; t.duration = 10 * 86400 * kSecond; t.running = true; t.endUtc = t0 + t.duration;
    // Nothing runs while the PC is off: the wall clock is the only truth.
    const Ticks back = t0 + 3 * 86400 * kSecond;
    CHECK(Remaining(t, back) == 7 * 86400 * kSecond);
    CHECK(FormatDuration(Remaining(t, back)) == L"7d 00:00:00");
    // Paused timers do not move.
    t.running = false; t.remaining = 5 * kSecond;
    CHECK(Remaining(t, back) == 5 * kSecond);
}

static void LongStopwatch() {
    wprintf(L"stopwatch past 400 days\n");
    Stopwatch w; w.running = true; w.startUtc = 1000; w.banked = 5 * kSecond;
    const Ticks now = w.startUtc + 400LL * 86400 * kSecond + 3 * kSecond;
    CHECK(Elapsed(w, now) == 400LL * 86400 * kSecond + 8 * kSecond);
    CHECK(FormatDuration(Elapsed(w, now)) == L"400d 00:00:08");
    w.banked = Elapsed(w, now); w.running = false;       // a pause folds it in
    CHECK(Elapsed(w, now + 99 * kSecond) == w.banked);
}

static void ClockBackwards() {
    wprintf(L"clock set backwards\n");
    Stopwatch w; w.running = true; w.startUtc = 100 * kSecond;
    CHECK(Elapsed(w, 40 * kSecond) == 0);
    w.banked = 7 * kSecond;
    CHECK(Elapsed(w, 40 * kSecond) == 7 * kSecond);
    Timer t; t.running = true; t.endUtc = 50 * kSecond; t.duration = 60 * kSecond;
    CHECK(Remaining(t, 90 * kSecond) == 0);
    CHECK(Remaining(t, 40 * kSecond) == 10 * kSecond);
}

static void FiresOnce() {
    wprintf(L"a finished timer fires once on load\n");
    State s;
    Timer a; a.label = L"Done"; a.duration = 60 * kSecond; a.running = true; a.endUtc = 1000 * kSecond;
    Timer b; b.label = L"Later"; b.duration = 60 * kSecond; b.running = true; b.endUtc = 5000 * kSecond;
    s.timers = { a, b };
    State loaded;
    CHECK(Deserialize(Serialize(s), &loaded));
    const Ticks now = 3000 * kSecond;                 // the PC was off past a's end
    auto first = TakeFinished(loaded, now);
    CHECK(first.size() == 1 && first[0].label == L"Done" && first[0].endUtc == 1000 * kSecond);
    CHECK(TakeFinished(loaded, now).empty());          // not twice
    CHECK(loaded.timers[1].running && !loaded.timers[0].running && loaded.timers[0].remaining == 0);
    // Saved after firing, reloaded, still silent.
    State again;
    CHECK(Deserialize(Serialize(loaded), &again));
    CHECK(TakeFinished(again, 6000 * kSecond).size() == 1);   // only b, at its time
    CHECK(TakeFinished(again, 6000 * kSecond).empty());
}

static void RunningNeedsAnchor() {
    wprintf(L"a running entry needs its start or end time\n");
    // watch.running = 1 with no start would read as 400 years elapsed.
    State s;
    CHECK(Deserialize(L"count = 1\nt0.running = 1\nt0.end = 0\nt0.duration = 100\n"
                      L"watch.running = 1\nwatch.start = 0\nwatch.banked = 5\n", &s));
    CHECK(!s.watch.running && s.watch.banked == 5);
    CHECK(!s.timers[0].running);
    CHECK(Elapsed(s.watch, 132000000000000000LL) == 5);
    CHECK(Deserialize(L"count = 0\nwatch.running = 1\nwatch.start = 77\n", &s));
    CHECK(s.watch.running && s.watch.startUtc == 77);
}

static void Files() {
    wprintf(L"timers.ini on disk\n");
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"pw_timer_test.ini";
    State s; Timer a; a.label = L"x"; a.duration = kSecond; a.remaining = kSecond; s.timers.push_back(a);
    CHECK(SaveFile(path, s));
    State back;
    CHECK(LoadFile(path, &back) && back.timers.size() == 1 && back.timers[0].label == L"x");
    CHECK(GetFileAttributesW((path + L".new").c_str()) == INVALID_FILE_ATTRIBUTES);
    DeleteFileW(path.c_str());
    CHECK(!LoadFile(path, &back));
}

int wmain() {
    RoundTrip();
    ThreeDayGap();
    LongStopwatch();
    ClockBackwards();
    FiresOnce();
    RunningNeedsAnchor();
    Files();
    wprintf(L"\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
