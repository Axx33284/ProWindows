#include "timer.h"
#include "winutil.h"
#include "theme.h"
#include "app.h"
#include "clockpanel.h"
#include <mmsystem.h>
#include <cmath>
#include <cwchar>
#include <cwctype>

#pragma comment(lib, "winmm.lib")

namespace awa {

// =====================================================================
// The model: pure functions over timer::State, no window, no clock of its
// own. Everything takes `now` so a test can move time wherever it likes.
// =====================================================================
namespace timer {

Ticks NowUtc() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((Ticks)ft.dwHighDateTime << 32) | (Ticks)ft.dwLowDateTime;
}

Ticks Remaining(const Timer& t, Ticks now) {
    const Ticks r = t.running ? t.endUtc - now : t.remaining;
    return r < 0 ? 0 : r;
}

Ticks Elapsed(const Stopwatch& w, Ticks now) {
    Ticks e = w.banked < 0 ? 0 : w.banked;
    if (w.running) {
        // A clock set back past the start would make this negative.
        const Ticks run = now - w.startUtc;
        if (run > 0) e += run;
    }
    return e;
}

bool AnythingRunning(const State& s) {
    if (s.watch.running) return true;
    for (const Timer& t : s.timers) if (t.running) return true;
    return false;
}

std::vector<Finished> TakeFinished(State& s, Ticks now) {
    std::vector<Finished> out;
    for (size_t i = 0; i < s.timers.size(); ++i) {
        Timer& t = s.timers[i];
        if (!t.running || t.endUtc > now) continue;
        Finished f;
        f.index  = (int)i;
        f.label  = t.label;
        f.endUtc = t.endUtc;
        out.push_back(f);
        t.running   = false;     // no longer running, so it can never be reported again
        t.remaining = 0;
    }
    return out;
}

Ticks CeilSecond(Ticks t) {
    if (t <= 0) return 0;
    return ((t + kSecond - 1) / kSecond) * kSecond;
}

std::wstring FormatDuration(Ticks t, bool tenths) {
    if (t < 0) t = 0;
    const long long secs = t / kSecond;
    const int tenth = (int)((t % kSecond) / (kSecond / 10));
    const long long days = secs / 86400;
    const int h = (int)((secs / 3600) % 24), m = (int)((secs / 60) % 60), s = (int)(secs % 60);
    wchar_t buf[64];
    if (days > 0) swprintf_s(buf, L"%lldd %02d:%02d:%02d", days, h, m, s);
    else          swprintf_s(buf, L"%d:%02d:%02d", h, m, s);
    std::wstring out = buf;
    if (tenths) { swprintf_s(buf, L".%d", tenth); out += buf; }
    return out;
}

namespace {
// Reads up to 7 digits (so the product below cannot overflow) and advances.
bool Number(const wchar_t*& p, long long* out) {
    if (!iswdigit(*p)) return false;
    long long v = 0;
    int n = 0;
    while (iswdigit(*p)) {
        if (++n > 7) return false;
        v = v * 10 + (*p - L'0');
        ++p;
    }
    *out = v;
    return true;
}
void SkipSpace(const wchar_t*& p) { while (*p == L' ' || *p == L'\t') ++p; }
}

bool ParseDuration(const std::wstring& text, Ticks* out) {
    const wchar_t* p = text.c_str();
    SkipSpace(p);
    long long days = 0, parts[3] = {};
    int count = 0;

    long long first;
    if (!Number(p, &first)) return false;
    if (*p == L'd' || *p == L'D') {
        days = first;
        ++p;
        SkipSpace(p);
        if (*p) {
            if (!Number(p, &parts[0])) return false;
            count = 1;
        }
    } else {
        parts[0] = first;
        count = 1;
    }
    while (count > 0 && count < 3 && *p == L':') {
        ++p;
        if (!Number(p, &parts[count])) return false;
        ++count;
    }
    // A fraction (tenths) is accepted and dropped.
    if (*p == L'.') { ++p; while (iswdigit(*p)) ++p; }
    SkipSpace(p);
    if (*p) return false;

    long long secs = 0;
    if (count == 1)      secs = parts[0];
    else if (count == 2) { if (parts[1] > 59) return false; secs = parts[0] * 60 + parts[1]; }
    else if (count == 3) {
        if (parts[1] > 59 || parts[2] > 59) return false;
        secs = parts[0] * 3600 + parts[1] * 60 + parts[2];
    }
    if (days > 0 && count == 3 && parts[0] > 23) return false;
    secs += days * 86400;
    if (secs > kMaxDuration / kSecond) return false;
    *out = secs * kSecond;
    return true;
}

// ---------------------------------------------------------------- timers.ini
std::wstring Serialize(const State& s) {
    std::wstring out = L"# ProWindows timers - rewritten on every start, pause and lap. Times are\n"
                       L"# 100-ns ticks of the system clock (UTC), so they survive sleep and restart.\n"
                       L"version = 1\n";
    wchar_t buf[96];
    swprintf_s(buf, L"count = %d\n", (int)s.timers.size());
    out += buf;
    for (size_t i = 0; i < s.timers.size(); ++i) {
        const Timer& t = s.timers[i];
        std::wstring label = t.label;
        for (auto& c : label) if (c == L'\r' || c == L'\n') c = L' ';
        out += L"t" + std::to_wstring(i) + L".label = " + label + L"\n";
        out += L"t" + std::to_wstring(i) + L".duration = " + std::to_wstring(t.duration) + L"\n";
        out += L"t" + std::to_wstring(i) + L".running = " + (t.running ? L"1" : L"0") + L"\n";
        out += L"t" + std::to_wstring(i) + L".end = " + std::to_wstring(t.endUtc) + L"\n";
        out += L"t" + std::to_wstring(i) + L".remaining = " + std::to_wstring(t.remaining) + L"\n";
    }
    out += std::wstring(L"watch.running = ") + (s.watch.running ? L"1" : L"0") + L"\n";
    out += L"watch.start = " + std::to_wstring(s.watch.startUtc) + L"\n";
    out += L"watch.banked = " + std::to_wstring(s.watch.banked) + L"\n";
    out += L"watch.lapseq = " + std::to_wstring(s.watch.lapSeq) + L"\n";
    out += L"watch.laps = ";
    for (size_t i = 0; i < s.watch.laps.size(); ++i) {
        if (i) out += L",";
        out += std::to_wstring(s.watch.laps[i]);
    }
    out += L"\n";
    out += L"page = " + std::to_wstring(s.page) + L"\n";
    alarm::AppendIni(out, s.alarms);
    return out;
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

bool Deserialize(const std::wstring& text, State* out) {
    std::unordered_map<std::wstring, std::wstring> kv;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        size_t a = line.find_first_not_of(L" \t");
        if (a == std::wstring::npos || line[a] == L'#') continue;
        const size_t eq = line.find(L'=', a);
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(a, eq - a), val = line.substr(eq + 1);
        while (!key.empty() && key.back() == L' ') key.pop_back();
        const size_t v0 = val.find_first_not_of(L" \t");
        val = (v0 == std::wstring::npos) ? L"" : val.substr(v0);
        kv[key] = val;
    }
    if (kv.find(L"count") == kv.end()) return false;

    State s;
    const int count = (int)Int(kv, L"count", 0, 0, kMaxTimers);
    for (int i = 0; i < count; ++i) {
        const std::wstring p = L"t" + std::to_wstring(i) + L".";
        Timer t;
        auto lab = kv.find(p + L"label");
        t.label     = lab != kv.end() ? lab->second : L"Timer " + std::to_wstring(i + 1);
        t.duration  = Int(kv, p + L"duration", 0, 0, kMaxDuration);
        t.remaining = Int(kv, p + L"remaining", 0, 0, kMaxDuration);
        t.endUtc    = Int(kv, p + L"end", 0, 0, INT64_MAX / 2);
        t.running   = Int(kv, p + L"running", 0, 0, 1) != 0 && t.endUtc > 0;
        s.timers.push_back(t);
    }
    s.watch.startUtc = Int(kv, L"watch.start", 0, 0, INT64_MAX / 2);
    // Running without a start would read as 400 years elapsed (R4).
    s.watch.running  = Int(kv, L"watch.running", 0, 0, 1) != 0 && s.watch.startUtc > 0;
    s.watch.banked   = Int(kv, L"watch.banked", 0, 0, INT64_MAX / 2);
    s.watch.lapSeq   = (int)Int(kv, L"watch.lapseq", 0, 0, 1000000000);
    auto laps = kv.find(L"watch.laps");
    if (laps != kv.end()) {
        const wchar_t* p = laps->second.c_str();
        while (*p && (int)s.watch.laps.size() < kMaxLaps) {
            wchar_t* next = nullptr;
            const long long v = _wcstoi64(p, &next, 10);
            if (next == p) break;
            s.watch.laps.push_back(v < 0 ? 0 : v);
            p = next;
            if (*p == L',') ++p;
        }
    }
    s.page = (int)Int(kv, L"page", 0, 0, 3);
    alarm::ReadIni(kv, &s.alarms);       // absent keys = no alarms, so old files load
    *out = s;
    return true;
}

bool LoadFile(const std::wstring& path, State* out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") != 0 || !f) return false;
    std::wstring text;
    wchar_t line[8192];
    while (fgetws(line, (int)(sizeof(line) / sizeof(line[0])), f)) text += line;
    fclose(f);
    return Deserialize(text, out);
}

bool SaveFile(const std::wstring& path, const State& s) {
    // Beside the real file, then moved into place, as Config::SaveToFile does:
    // a power cut mid-write must not cost anyone a running timer.
    const std::wstring temp = path + L".new";
    FILE* f = nullptr;
    if (_wfopen_s(&f, temp.c_str(), L"w, ccs=UTF-8") != 0 || !f) return false;
    const std::wstring text = Serialize(s);
    const bool wrote = fputws(text.c_str(), f) >= 0;
    const bool closed = fclose(f) == 0;
    if (!wrote || !closed) { DeleteFileW(temp.c_str()); return false; }
    return MoveFileExW(temp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

} // namespace timer


// =====================================================================
// The panel: the window, the actions, the wake-up. What is drawn lives in
// clockpanel_paint.cpp; a click or a key arrives here as a Cmd.
// =====================================================================
namespace {

using namespace timer;
using namespace panel;

constexpr wchar_t kClass[] = L"ProWindows_Timer";

constexpr UINT_PTR kTimerTick  = 1;    // the one wake-up (see Reschedule)
// Set when the panel is hidden; when it fires the panel has been out of sight
// long enough that the shell is asked to give the memory back (inv. 67).
constexpr UINT_PTR kTimerIdle  = 2;
constexpr UINT     kIdleMs     = 3 * 60 * 1000;
constexpr UINT_PTR kTimerAlarm = 3;    // one-shot: the alarm gives up after a minute
constexpr UINT     kAlarmMs    = 60 * 1000;
constexpr UINT_PTR kTimerAway  = 4;    // one-shot: say what finished while we were off

HINSTANCE g_inst = nullptr;
Config*   g_cfg  = nullptr;
HWND      g_wnd  = nullptr;
float     g_scale = 1.0f;

State        g_state;
std::wstring g_store;
View         g_view;

bool g_alarming = false;               // a sound is looping (a timer's, or an alarm's with sound on)
long long g_lastTickIdx = -1;
std::wstring g_awayText;
bool g_awayHasAlarm = false;
std::vector<Ring> g_ringQueue;         // alarms that rang while another was still ringing
std::vector<BYTE> g_tickWav;

bool  g_dragging = false;              // unpinned: a drag by the rail or the top strip
POINT g_dragOrigin = {};
RECT  g_dragStart = {};

std::wstring StorePath() { return g_store.empty() ? ConfigDir() + L"\\timers.ini" : g_store; }

unsigned g_rev = 1;                    // bumped by every Save(): the settings page watches it

void Save() {
    ++g_rev;
    g_state.page = g_view.page;
    if (!SaveFile(StorePath(), g_state)) AWA_LOG(L"timer: could not save %s", StorePath().c_str());
}

bool Visible() { return g_wnd && IsWindowVisible(g_wnd); }
void Repaint() { if (Visible()) InvalidateRect(g_wnd, nullptr, FALSE); }
bool H24() { return g_cfg && g_cfg->clockHours24; }

// "14:32", or "7 Oct 14:32" when it is not today.
std::wstring LocalStamp(Ticks t) {
    FILETIME utc, local;
    utc.dwLowDateTime  = (DWORD)(t & 0xFFFFFFFF);
    utc.dwHighDateTime = (DWORD)((unsigned long long)t >> 32);
    SYSTEMTIME st = {}, today = {};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return L"";
    GetLocalTime(&today);
    wchar_t tm[40] = L"", date[40] = L"";
    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, tm, 40);
    if (st.wDay == today.wDay && st.wMonth == today.wMonth && st.wYear == today.wYear) return tm;
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, date, 40);
    return std::wstring(date) + L" " + tm;
}

// ------------------------------------------------------------------ sound
// A tick: 12 ms of a 2 kHz sine under a fast decay, 16-bit mono 44.1 kHz,
// built once in memory so there is no asset to ship or lose.
const std::vector<BYTE>& TickWav() {
    if (!g_tickWav.empty()) return g_tickWav;
    const int rate = 44100, n = rate * 12 / 1000;
    g_tickWav.resize(44 + (size_t)n * 2);
    BYTE* b = g_tickWav.data();
    auto put32 = [&](size_t at, DWORD v) { memcpy(b + at, &v, 4); };
    auto put16 = [&](size_t at, WORD v)  { memcpy(b + at, &v, 2); };
    memcpy(b, "RIFF", 4);          put32(4, (DWORD)(36 + n * 2));
    memcpy(b + 8, "WAVEfmt ", 8);  put32(16, 16);
    put16(20, 1); put16(22, 1);    put32(24, rate); put32(28, rate * 2);
    put16(32, 2); put16(34, 16);
    memcpy(b + 36, "data", 4);     put32(40, (DWORD)(n * 2));
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / rate;
        const double v = std::sin(2.0 * 3.14159265358979 * 2000.0 * t) * std::exp(-t / 0.0025) * 0.6;
        put16(44 + (size_t)i * 2, (WORD)(SHORT)(v * 32767.0));
    }
    return g_tickWav;
}

void PlayTick() {
    const std::vector<BYTE>& wav = TickWav();
    PlaySoundW((LPCWSTR)wav.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void ApplyPin();

void StopAlarm() {
    if (!g_alarming) return;
    g_alarming = false;
    PlaySoundW(nullptr, nullptr, 0);
    if (g_wnd && !g_view.ringing) KillTimer(g_wnd, kTimerAlarm);
    ApplyPin();                      // the ring is over: a pinned panel goes click-through again
}

void StartAlarm() {
    StopAlarm();
    g_alarming = true;
    const wchar_t kFile[] = L"C:\\Windows\\Media\\Alarm01.wav";
    if (GetFileAttributesW(kFile) != INVALID_FILE_ATTRIBUTES)
        PlaySoundW(kFile, nullptr, SND_FILENAME | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
    else
        PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
    if (g_wnd) SetTimer(g_wnd, kTimerAlarm, kAlarmMs, nullptr);
    ApplyPin();
}

// ------------------------------------------------------------------ window geometry
SIZE WindowSize() {
    SIZE s;
    s.cx = (int)(kWidth * g_scale);
    s.cy = (int)(kHeight * g_scale);
    return s;
}

// The DPI of the monitor a point is on. The window's own DPI is where it
// *was*, which is the wrong scale the first time it opens elsewhere (R2).
UINT DpiAt(HMONITOR mon) {
    using GetDpiForMonitorFn = HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForMonitorFn fn = [] {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        return shcore ? (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor") : nullptr;
    }();
    UINT x = 0, y = 0;
    if (fn && mon && SUCCEEDED(fn(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && x) return x;
    return DpiForWindow(g_wnd);
}

void SetScale(UINT dpi) {
    if (!dpi) dpi = 96;
    g_scale = (float)dpi / 96.0f;
    theme::SetDpi(dpi);
}

// Keeps a window of `size` whole inside the work area of the monitor under its centre.
void ClampOnScreen(int* x, int* y, const SIZE& size) {
    POINT probe = { *x + size.cx / 2, *y + size.cy / 2 };
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST), &mi)) return;
    *x = (std::max)((int)mi.rcWork.left, (std::min)(*x, (int)mi.rcWork.right - (int)size.cx));
    *y = (std::max)((int)mi.rcWork.top,  (std::min)(*y, (int)mi.rcWork.bottom - (int)size.cy));
}

// Where timerX/Y say, on screen; never placed means centred in the primary work
// area, 22 % down. The scale is that of the monitor it lands on.
void Place() {
    if (!g_wnd || !g_cfg) return;
    const bool placed = g_cfg->timerX != INT_MIN && g_cfg->timerY != INT_MIN;
    const POINT at = placed ? POINT{ g_cfg->timerX, g_cfg->timerY } : POINT{ 0, 0 };
    HMONITOR mon = MonitorFromPoint(at, placed ? MONITOR_DEFAULTTONEAREST : MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(mon, &mi)) return;
    SetScale(DpiAt(mon));
    const SIZE size = WindowSize();
    int x, y;
    if (placed) {
        x = g_cfg->timerX;
        y = g_cfg->timerY;
        x = (std::max)((int)mi.rcWork.left, (std::min)(x, (int)mi.rcWork.right - (int)size.cx));
        y = (std::max)((int)mi.rcWork.top,  (std::min)(y, (int)mi.rcWork.bottom - (int)size.cy));
    } else {
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - size.cx) / 2;
        y = mi.rcWork.top + (int)((mi.rcWork.bottom - mi.rcWork.top) * 0.22);
    }
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE);
}

// The size only changes with the DPI; the top-left corner stays unless that
// would push the panel off the screen.
void Relayout() {
    if (!g_wnd) return;
    if (Visible()) {
        const SIZE size = WindowSize();
        RECT r;
        GetWindowRect(g_wnd, &r);
        int x = r.left, y = r.top;
        ClampOnScreen(&x, &y, size);
        SetWindowPos(g_wnd, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE);
    }
    Repaint();
}

// ------------------------------------------------------------------ waking
// Nothing polls. With nothing to wake for there is no OS timer at all; otherwise
// exactly one SetTimer sleeps until the earliest of: a timer's end, an alarm's
// next ring or snooze, the next change the panel would show (only while it is
// up), the next tick sound, and an hour. A moved clock or zone is WM_TIMECHANGE
// and sleep is the resume call, so the ceiling is not what catches those; it
// bounds the drift between SetTimer's tick clock and the wall clock (NTP slews
// the wall clock) to well under the 10 s after which a ring counts as missed.
void Reschedule() {
    if (!g_wnd) return;

    const Ticks now = NowUtc();
    const bool shown = Visible();
    constexpr Ticks kCeiling = 3600 * kSecond;
    Ticks delay = kCeiling;
    bool need = false;
    auto sooner = [&](Ticks d) { need = true; if (d < delay) delay = d; };
    auto toSecond = [&] { sooner(kSecond - now % kSecond); };
    auto toMinute = [&] { sooner(60 * kSecond - now % (60 * kSecond)); };

    bool timerRunning = false;
    for (const Timer& t : g_state.timers) {
        if (!t.running) continue;
        timerRunning = true;
        const Ticks left = Remaining(t, now);
        sooner(left);
        if (shown && (g_view.page == PageTimer || g_view.page == PageClock)) {
            const Ticks part = left % kSecond;       // the shown (rounded-up) second changes here
            sooner(part == 0 ? kSecond : part);
        }
    }
    if (shown && g_state.watch.running) {
        if (g_view.page == PageWatch) {
            const Ticks step = kSecond / 10;         // the stopwatch shows tenths
            sooner(step - Elapsed(g_state.watch, now) % step);
        } else if (g_view.page == PageClock) {
            toSecond();
        }
    }

    const alarm::Zone& zone = alarm::SystemZone();
    for (const alarm::Alarm& a : g_state.alarms) {
        const Ticks w = alarm::NextWake(a, now, zone);
        if (w) sooner(w - now);
    }

    if (shown) {
        if (g_view.page == PageClock) { if (g_cfg && g_cfg->clockSeconds) toSecond(); else toMinute(); }
        else if (g_view.page == PageAlarm) toMinute();       // "rings in 12 min" counts down
    }

    const int mode = g_cfg ? g_cfg->timerTick : 0;
    if (AnythingRunning(g_state)) {
        if (mode == 1)      toSecond();
        else if (mode == 2) toMinute();
    }
    (void)timerRunning;

    if (!need) { KillTimer(g_wnd, kTimerTick); return; }

    // A few ms past the boundary, so the timer does not land just before it.
    long long ms = delay / 10000 + 8;
    if (ms < 10) ms = 10;
    if (ms > kCeiling / 10000) ms = kCeiling / 10000;    // also under SetTimer's ~24.8-day cap
    SetTimer(g_wnd, kTimerTick, (UINT)ms, nullptr);
}

// Shown, not toggled, and not activated (R3): the user may be typing in
// something else. Game mode and a dark display still win.
void ShowForAlarm() {
    if (g_cfg && !g_cfg->timerShown) {
        g_cfg->timerShown = true;
        AppSaveConfig();
        AppRefreshSettings();
    }
    if (!Visible()) AppUpdateOverlays();
    else            Relayout();
}

void NoteAway(const std::wstring& line, bool alarmLine) {
    if (!g_awayText.empty()) g_awayText += L"\n";
    g_awayText += line;
    if (alarmLine) g_awayHasAlarm = true;
    if (g_wnd) SetTimer(g_wnd, kTimerAway, 1500, nullptr);
}

void Announce(const std::vector<Finished>& done, Ticks now) {
    for (const Finished& f : done) {
        const bool away = now - f.endUtc > 10 * kSecond;
        if (away) {
            // Finished while the PC was off or asleep: said once, a little later,
            // so a balloon at start-up finds the tray icon in place.
            NoteAway(f.label + L" - ended " + LocalStamp(f.endUtc), false);
            continue;
        }
        AppTrayBalloon(L"Timer finished", f.label.c_str());
        if (g_cfg && g_cfg->timerAlarm) {
            StartAlarm();
            g_view.page = PageTimer;
            g_view.sel = f.index;
            g_view.sheet = SheetNone;
            // Any key or click in the panel, or the tray, stops it.
            ShowForAlarm();
        }
    }
}

// ------------------------------------------------------------------ alarms ringing
void BeginRing(const Ring& ring, bool sound) {
    g_view.ringing = true;
    g_view.ring = ring;
    g_view.page = PageAlarm;
    if (sound) StartAlarm(); else StopAlarm();
    if (g_wnd) SetTimer(g_wnd, kTimerAlarm, kAlarmMs, nullptr);      // 60 s unanswered = dismissed
    ShowForAlarm();
    Repaint();
}

void StartRing(int index, Ticks dueUtc) {
    if (index < 0 || index >= (int)g_state.alarms.size()) return;
    const alarm::Alarm& a = g_state.alarms[(size_t)index];
    const alarm::LocalTime l = alarm::SystemZone().ToLocal(dueUtc);
    Ring r;
    r.index = index;
    r.name = a.label;
    r.hour = l.hour;
    r.minute = l.minute;
    r.snoozeMin = a.snoozeMin;
    std::wstring body = (a.label.empty() ? std::wstring(L"Alarm") : a.label);
    wchar_t t[16];
    swprintf_s(t, L" - %02d:%02d", l.hour, l.minute);
    AppTrayBalloon(L"Alarm", (body + t).c_str());
    if (g_view.ringing) { g_ringQueue.push_back(r); return; }
    BeginRing(r, a.sound);
}

// Snooze or dismiss the one on screen, then the next that was waiting.
void EndRing(bool snooze) {
    if (!g_view.ringing) return;
    const Ring done = g_view.ring;
    if (done.index >= 0 && done.index < (int)g_state.alarms.size()) {
        alarm::Alarm& a = g_state.alarms[(size_t)done.index];
        if (snooze) alarm::Snooze(a, NowUtc());
        else        alarm::Dismiss(a);
        Save();
    }
    g_view.ringing = false;
    g_alarming = true;               // StopAlarm only acts on a looping sound
    StopAlarm();
    if (g_wnd) KillTimer(g_wnd, kTimerAlarm);
    ApplyPin();
    if (!g_ringQueue.empty()) {
        const Ring next = g_ringQueue.front();
        g_ringQueue.erase(g_ringQueue.begin());
        const bool sound = next.index >= 0 && next.index < (int)g_state.alarms.size() &&
                           g_state.alarms[(size_t)next.index].sound;
        BeginRing(next, sound);
    }
    Reschedule();
    Repaint();
}

void AnnounceAlarms(const std::vector<alarm::Fired>& fired) {
    for (const alarm::Fired& f : fired) {
        if (f.index < 0 || f.index >= (int)g_state.alarms.size()) continue;
        const alarm::Alarm& a = g_state.alarms[(size_t)f.index];
        if (f.missed) {
            NoteAway((a.label.empty() ? std::wstring(L"Alarm") : a.label) + L" - rang " + LocalStamp(f.dueUtc), true);
            continue;
        }
        StartRing(f.index, f.dueUtc);
    }
}

// What a wake-up does: fire what finished, tick, repaint, plan the next.
void Tick(bool fromTimer) {
    const Ticks now = NowUtc();
    const std::vector<Finished> done = TakeFinished(g_state, now);
    const std::vector<alarm::Fired> fired = alarm::TakeDue(g_state.alarms, now, alarm::SystemZone());
    if (!done.empty() || !fired.empty()) Save();
    if (!done.empty()) Announce(done, now);
    if (!fired.empty()) AnnounceAlarms(fired);

    const int mode = g_cfg ? g_cfg->timerTick : 0;
    if (fromTimer && mode && !g_alarming && AnythingRunning(g_state)) {
        const long long idx = now / (mode == 2 ? 60 * kSecond : kSecond);
        if (idx != g_lastTickIdx) { g_lastTickIdx = idx; PlayTick(); }
    }
    Repaint();
    Reschedule();
}

// ------------------------------------------------------------------ timer actions
Timer* Selected() {
    return (g_view.sel >= 0 && g_view.sel < (int)g_state.timers.size()) ? &g_state.timers[(size_t)g_view.sel] : nullptr;
}

void StartPauseTimer(Timer& t) {
    const Ticks now = NowUtc();
    if (t.running) {
        t.remaining = Remaining(t, now);
        t.running = false;
    } else {
        if (t.duration <= 0) return;
        if (t.remaining <= 0) t.remaining = t.duration;       // a finished one starts over
        t.endUtc = now + t.remaining;
        t.running = true;
    }
}

void StartPauseWatch() {
    Stopwatch& w = g_state.watch;
    const Ticks now = NowUtc();
    if (w.running) { w.banked = Elapsed(w, now); w.running = false; }
    else           { w.startUtc = now; w.running = true; }
}

void ResetWatch() {
    Stopwatch& w = g_state.watch;
    w.running = false;
    w.banked = 0;
    w.startUtc = 0;
    w.laps.clear();
    w.lapSeq = 0;
}

void Lap() {
    Stopwatch& w = g_state.watch;
    const Ticks total = Elapsed(w, NowUtc());
    if (total <= 0) return;
    w.laps.insert(w.laps.begin(), total);
    ++w.lapSeq;
    if ((int)w.laps.size() > kMaxLaps) w.laps.pop_back();
}

std::wstring NewTimerName() {
    int n = 1;
    for (;; ++n) {
        bool used = false;
        for (const Timer& t : g_state.timers) if (t.label == L"Timer " + std::to_wstring(n)) used = true;
        if (!used) break;
    }
    return L"Timer " + std::to_wstring(n);
}

void AddQuickTimer(int minutes) {
    if ((int)g_state.timers.size() >= kMaxTimers) return;
    Timer t;
    t.label = NewTimerName();
    t.duration = t.remaining = (Ticks)minutes * 60 * kSecond;
    g_state.timers.push_back(t);
    g_view.sel = (int)g_state.timers.size() - 1;
    StartPauseTimer(g_state.timers.back());
}

// ------------------------------------------------------------------ the sheets
void SelectAll(TextField& f) { f.anchor = 0; f.caret = (int)f.s.size(); }

void FocusField(int field) {
    g_view.focus = field;
    g_view.typed = 0;
    if (field == FieldName) SelectAll(g_view.sheet == SheetTimer ? g_view.td.name : g_view.ad.name);
}

TextField& NameFieldOf() { return g_view.sheet == SheetTimer ? g_view.td.name : g_view.ad.name; }

void OpenTimerSheet(int index) {
    TimerDraft d;
    d.index = index;
    Ticks len = 5 * 60 * kSecond;
    if (index >= 0 && index < (int)g_state.timers.size()) {
        const Timer& t = g_state.timers[(size_t)index];
        len = t.duration;
        d.name.s = t.label;
    } else {
        d.index = -1;
        d.name.s = NewTimerName();
    }
    const long long secs = len / kSecond;
    d.v[0] = (int)(secs / 86400);
    d.v[1] = (int)((secs / 3600) % 24);
    d.v[2] = (int)((secs / 60) % 60);
    d.v[3] = (int)(secs % 60);
    d.name.caret = d.name.anchor = (int)d.name.s.size();
    g_view.td = d;
    g_view.sheet = SheetTimer;
    g_view.sheetScroll = 0;
    g_view.focus = 2;
    g_view.typed = 0;
    Repaint();
}

void SetDate(alarm::Alarm& a, Ticks utc) {
    const alarm::LocalTime l = alarm::SystemZone().ToLocal(utc);
    a.year = l.year; a.month = l.month; a.day = l.day;
}

void OpenAlarmSheet(int index) {
    AlarmDraft d;
    if (index >= 0 && index < (int)g_state.alarms.size()) {
        d.index = index;
        d.a = g_state.alarms[(size_t)index];
        const alarm::Alarm& a = d.a;
        d.limitOpen = a.weeks || a.months || a.until ||
                      (a.weekdays && (a.kind == alarm::Kind::Hourly || a.kind == alarm::Kind::Daily));
    } else {
        // A new alarm: once, at the next whole hour.
        const Ticks now = NowUtc();
        const alarm::LocalTime l = alarm::SystemZone().ToLocal(now);
        const Ticks next = alarm::FromCivil(l.year, l.month, l.day, l.hour, 0) + 60 * 60 * kSecond;
        const alarm::LocalTime n = alarm::ToCivil(next);
        d.index = -1;
        d.a.kind = alarm::Kind::Once;
        d.a.hour = n.hour;
        d.a.minute = 0;
        d.a.year = n.year; d.a.month = n.month; d.a.day = n.day;
        d.a.sound = true;
        d.a.snoozeMin = 10;
        d.a.enabled = true;
    }
    if (d.a.year < 2000) SetDate(d.a, NowUtc());
    d.name.s = d.a.label;
    d.name.caret = d.name.anchor = (int)d.name.s.size();
    g_view.ad = d;
    g_view.sheet = SheetAlarm;
    g_view.sheetScroll = 0;
    g_view.focus = d.a.kind == alarm::Kind::Hourly ? FieldMinute : FieldHour;
    g_view.typed = 0;
    Repaint();
}

void CloseSheet() {
    if (g_view.sheet == SheetNone) return;
    g_view.sheet = SheetNone;
    g_view.typed = 0;
    Repaint();
}

int Wrap(int v, int lo, int hi) {
    const int n = hi - lo + 1;
    return lo + (((v - lo) % n) + n) % n;
}

void FixOnceDay(alarm::Alarm& a) {
    if (a.month < 1) a.month = 1;
    if (a.month > 12) a.month = 12;
    const int dim = alarm::DaysInMonth(a.year, a.month);
    if (a.day > dim) a.day = dim;
    if (a.day < 1) a.day = 1;
}

void SetUntil(alarm::Alarm& a, int y, int mo, int d) {
    y = (std::max)(2000, (std::min)(2100, y));
    mo = Wrap(mo, 1, 12);
    const int dim = alarm::DaysInMonth(y, mo);
    d = (std::max)(1, (std::min)(dim, d));
    a.until = y * 10000 + mo * 100 + d;
}

// One step of a spinner (the arrows, the wheel, the Up and Down keys).
void SpinField(int field, int delta) {
    if (g_view.sheet == SheetTimer) {
        if (field < 0 || field > 3) return;
        int& v = g_view.td.v[field];
        if (field == 0) v = (std::max)(0, (std::min)(9999, v + delta));
        else            v = Wrap(v + delta, 0, field == 1 ? 23 : 59);
    } else if (g_view.sheet == SheetAlarm) {
        alarm::Alarm& a = g_view.ad.a;
        switch (field) {
            case FieldHour:
                if (H24()) a.hour = Wrap(a.hour + delta, 0, 23);
                else       a.hour = Wrap(a.hour % 12 + delta, 0, 11) + (a.hour >= 12 ? 12 : 0);
                break;
            case FieldMinute: a.minute = Wrap(a.minute + delta, 0, 59); break;
            case FieldDay:    a.day = Wrap(a.day + delta, 1, alarm::DaysInMonth(a.year, a.month)); break;
            case FieldMonth:  a.month = Wrap(a.month + delta, 1, 12); FixOnceDay(a); break;
            case FieldYear:   a.year = (std::max)(2000, (std::min)(2100, a.year + delta)); FixOnceDay(a); break;
            case FieldFrom:   a.hourFrom = Wrap(a.hourFrom + delta, 0, 23); break;
            case FieldTo:     a.hourTo = Wrap(a.hourTo + delta, 0, 23); break;
            case FieldUntilDay:   SetUntil(a, a.until / 10000, (a.until / 100) % 100,
                                           Wrap(a.until % 100 + delta, 1, alarm::DaysInMonth(a.until / 10000, (a.until / 100) % 100))); break;
            case FieldUntilMonth: SetUntil(a, a.until / 10000, (a.until / 100) % 100 + delta, a.until % 100); break;
            case FieldUntilYear:  SetUntil(a, a.until / 10000 + delta, (a.until / 100) % 100, a.until % 100); break;
            default: return;
        }
    }
    g_view.focus = field;
    g_view.typed = 0;
}

void TypeDigit(int d) {
    const int f = g_view.focus;
    if (f == FieldName) return;
    // `acc` is what was typed so far, not what is shown: a 12-hour "0" is shown as 12.
    auto accumulate = [&](int /*shown*/, int maxDigits, int lo, int hi) {
        const bool fresh = g_view.typed == 0 || g_view.typed >= maxDigits;
        const int v = (std::max)(lo, (std::min)(hi, fresh ? d : g_view.acc * 10 + d));
        g_view.acc = v;
        g_view.typed = fresh ? 1 : g_view.typed + 1;
        return v;
    };
    if (g_view.sheet == SheetTimer) {
        if (f < 0 || f > 3) return;
        g_view.td.v[f] = accumulate(g_view.td.v[f], f == 0 ? 4 : 2, 0, f == 0 ? 9999 : (f == 1 ? 23 : 59));
    } else if (g_view.sheet == SheetAlarm) {
        alarm::Alarm& a = g_view.ad.a;
        switch (f) {
            case FieldHour:
                if (H24()) a.hour = accumulate(a.hour, 2, 0, 23);
                else {
                    const int v = accumulate(a.hour % 12 == 0 ? 12 : a.hour % 12, 2, 0, 12);
                    a.hour = (v % 12) + (a.hour >= 12 ? 12 : 0);
                }
                break;
            case FieldMinute: a.minute = accumulate(a.minute, 2, 0, 59); break;
            case FieldDay:    a.day = accumulate(a.day, 2, 1, alarm::DaysInMonth(a.year, a.month)); break;
            case FieldMonth:  a.month = accumulate(a.month, 2, 1, 12); FixOnceDay(a); break;
            case FieldFrom:   a.hourFrom = accumulate(a.hourFrom, 2, 0, 23); break;
            case FieldTo:     a.hourTo = accumulate(a.hourTo, 2, 0, 23); break;
            case FieldUntilDay:
                SetUntil(a, a.until / 10000, (a.until / 100) % 100, accumulate(a.until % 100, 2, 1, 31)); break;
            case FieldUntilMonth:
                SetUntil(a, a.until / 10000, accumulate((a.until / 100) % 100, 2, 1, 12), a.until % 100); break;
            default: break;      // the years are changed with the arrows and the wheel
        }
    }
}

// Tab order of the open sheet.
std::vector<int> FocusOrder() {
    std::vector<int> o;
    o.push_back(FieldName);
    if (g_view.sheet == SheetTimer) { o.push_back(0); o.push_back(1); o.push_back(2); o.push_back(3); return o; }
    const alarm::Alarm& a = g_view.ad.a;
    if (a.kind != alarm::Kind::Hourly) o.push_back(FieldHour);
    o.push_back(FieldMinute);
    if (a.kind == alarm::Kind::Hourly) { o.push_back(FieldFrom); o.push_back(FieldTo); }
    if (a.kind == alarm::Kind::Once) { o.push_back(FieldDay); o.push_back(FieldMonth); o.push_back(FieldYear); }
    if (a.kind != alarm::Kind::Once && g_view.ad.limitOpen && a.until) {
        o.push_back(FieldUntilDay); o.push_back(FieldUntilMonth); o.push_back(FieldUntilYear);
    }
    return o;
}

void MoveFocus(int step) {
    const std::vector<int> o = FocusOrder();
    size_t i = 0;
    for (size_t k = 0; k < o.size(); ++k) if (o[k] == g_view.focus) i = k;
    i = (i + o.size() + (size_t)(step < 0 ? o.size() - 1 : 1)) % o.size();
    FocusField(o[i]);
    Repaint();
}

void SaveSheet() {
    const Ticks now = NowUtc();
    if (g_view.sheet == SheetTimer) {
        TimerDraft& d = g_view.td;
        Ticks len = ((Ticks)d.v[0] * 86400 + d.v[1] * 3600 + d.v[2] * 60 + d.v[3]) * kSecond;
        if (len > kMaxDuration) len = kMaxDuration;
        if (len <= 0) return;                       // a zero timer is nothing; keep editing
        std::wstring name = d.name.s;
        if (name.empty()) name = NewTimerName();
        if (d.index >= 0 && d.index < (int)g_state.timers.size()) {
            Timer& t = g_state.timers[(size_t)d.index];
            t.label = name;
            t.duration = len;
            t.remaining = len;                      // a new length restarts it, paused
            t.running = false;
        } else {
            if ((int)g_state.timers.size() >= kMaxTimers) return;
            Timer t;
            t.label = name;
            t.duration = t.remaining = len;
            g_state.timers.push_back(t);
            g_view.sel = (int)g_state.timers.size() - 1;
        }
    } else if (g_view.sheet == SheetAlarm) {
        AlarmDraft& d = g_view.ad;
        alarm::Alarm a = d.a;
        if (a.kind == alarm::Kind::Weekly && !a.weekdays) return;
        if (a.kind == alarm::Kind::Monthly && !a.monthDays) return;
        a.label = d.name.s.substr(0, (size_t)alarm::kMaxLabel);
        FixOnceDay(a);
        alarm::RollPastOnce(a, now, alarm::SystemZone());   // a Once for a moment gone rings tomorrow, as Windows Clock does
        a.enabled = true;
        alarm::Arm(a, now);                         // the baseline: rings only for the future
        if (d.index >= 0 && d.index < (int)g_state.alarms.size()) {
            g_state.alarms[(size_t)d.index] = a;
        } else {
            if ((int)g_state.alarms.size() >= alarm::kMaxAlarms) return;
            g_state.alarms.push_back(a);
        }
    }
    g_view.sheet = SheetNone;
    Save();
    Reschedule();
    Repaint();
}

// ------------------------------------------------------------------ text editing
void DeleteSel(TextField& f) {
    const int lo = (std::min)(f.caret, f.anchor), hi = (std::max)(f.caret, f.anchor);
    if (hi > lo) f.s.erase((size_t)lo, (size_t)(hi - lo));
    f.caret = f.anchor = lo;
}

void InsertText(TextField& f, const std::wstring& text) {
    DeleteSel(f);
    std::wstring clean;
    for (wchar_t ch : text) if (ch >= 32 && ch != 127) clean += ch;
    const int room = alarm::kMaxLabel - (int)f.s.size();
    if (room <= 0) return;
    if ((int)clean.size() > room) clean.resize((size_t)room);
    f.s.insert((size_t)f.caret, clean);
    f.caret = f.anchor = f.caret + (int)clean.size();
}

std::wstring ClipboardText() {
    std::wstring out;
    if (!OpenClipboard(g_wnd)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

// A caret never rests between the halves of a surrogate pair.
int PrevPos(const std::wstring& s, int p) {
    if (p <= 0) return 0;
    --p;
    if (p > 0 && s[(size_t)p] >= 0xDC00 && s[(size_t)p] <= 0xDFFF && s[(size_t)p - 1] >= 0xD800 && s[(size_t)p - 1] <= 0xDBFF) --p;
    return p;
}
int NextPos(const std::wstring& s, int p) {
    const int n = (int)s.size();
    if (p >= n) return n;
    if (s[(size_t)p] >= 0xD800 && s[(size_t)p] <= 0xDBFF && p + 1 < n && s[(size_t)p + 1] >= 0xDC00 && s[(size_t)p + 1] <= 0xDFFF) return p + 2;
    return p + 1;
}

void EditKey(TextField& f, UINT vk, bool ctrl, bool shift) {
    const int n = (int)f.s.size();
    auto moveTo = [&](int pos) {
        f.caret = (std::max)(0, (std::min)(n, pos));
        if (!shift) f.anchor = f.caret;
    };
    switch (vk) {
        case VK_LEFT:
            if (!shift && f.HasSel()) moveTo((std::min)(f.caret, f.anchor));
            else moveTo(PrevPos(f.s, f.caret));
            break;
        case VK_RIGHT:
            if (!shift && f.HasSel()) moveTo((std::max)(f.caret, f.anchor));
            else moveTo(NextPos(f.s, f.caret));
            break;
        case VK_HOME: moveTo(0); break;
        case VK_END:  moveTo(n); break;
        case VK_BACK:
            if (f.HasSel()) DeleteSel(f);
            else if (ctrl) {
                int p = f.caret;
                while (p > 0 && f.s[(size_t)p - 1] == L' ') --p;
                while (p > 0 && f.s[(size_t)p - 1] != L' ') --p;
                f.s.erase((size_t)p, (size_t)(f.caret - p));
                f.caret = f.anchor = p;
            } else if (f.caret > 0) {
                const int p = PrevPos(f.s, f.caret);
                f.s.erase((size_t)p, (size_t)(f.caret - p));
                f.caret = f.anchor = p;
            }
            break;
        case VK_DELETE:
            if (f.HasSel()) DeleteSel(f);
            else if (f.caret < n) f.s.erase((size_t)f.caret, (size_t)(NextPos(f.s, f.caret) - f.caret));
            break;
        case 'A': if (ctrl) SelectAll(f); break;
        case 'V': if (ctrl) InsertText(f, ClipboardText()); break;
        default: break;
    }
}

// ------------------------------------------------------------------ commands
void GoPage(int p) {
    if (p < 0 || p >= PageCount) return;
    g_view.page = p;
    Save();
    Reschedule();
    Repaint();
}

void ToggleBit(uint32_t& mask, int bit) { mask ^= (1u << bit); }

void SetKind(int k) {
    alarm::Alarm& a = g_view.ad.a;
    const alarm::Kind old = a.kind;
    a.kind = (alarm::Kind)k;
    const alarm::LocalTime today = alarm::SystemZone().ToLocal(NowUtc());
    if (a.kind == alarm::Kind::Once && a.year < 2000) SetDate(a, NowUtc());
    if (a.kind == alarm::Kind::Weekly) {
        if (!a.weekdays) a.weekdays = 1u << alarm::Weekday(today.year, today.month, today.day);
    } else if (old == alarm::Kind::Weekly && (a.kind == alarm::Kind::Hourly || a.kind == alarm::Kind::Daily)) {
        a.weekdays = 0;            // a weekly set of days would silently become a limit
    }
    if (a.kind == alarm::Kind::Monthly && !a.monthDays) a.monthDays = 1u << (today.day - 1);
    if (g_view.focus == FieldHour && a.kind == alarm::Kind::Hourly) g_view.focus = FieldMinute;
    g_view.sheetScroll = 0;
}

void Command(int cmd, int arg, int clickX = 0);

void DeleteAlarm(int i) {
    if (i < 0 || i >= (int)g_state.alarms.size()) return;
    g_state.alarms.erase(g_state.alarms.begin() + i);
    auto fix = [&](Ring& r) { if (r.index == i) r.index = -1; else if (r.index > i) --r.index; };
    fix(g_view.ring);
    for (Ring& r : g_ringQueue) fix(r);
    Save();
    Reschedule();
}

void Command(int cmd, int arg, int clickX) {
    alarm::Alarm* ea = g_view.sheet == SheetAlarm ? &g_view.ad.a : nullptr;
    const Ticks now = NowUtc();
    switch (cmd) {
        case CmdPage: GoPage(arg); return;
        case CmdCard: GoPage(arg >= 10 ? arg - 10 : arg); return;
        case CmdPin:  TimerSetPinned(!(g_cfg && g_cfg->timerPinned)); return;
        case CmdClose: if (g_view.ringing) EndRing(false); TimerHide(); return;

        case CmdQuick:
            AddQuickTimer(arg);
            Save(); Reschedule(); Repaint();
            return;
        case CmdTimerAdd:
            if ((int)g_state.timers.size() < kMaxTimers) OpenTimerSheet(-1);
            return;
        case CmdTimerSel: g_view.sel = arg; Repaint(); return;
        case CmdTimerPlay:
            if (arg >= 0 && arg < (int)g_state.timers.size()) {
                g_view.sel = arg;
                StartPauseTimer(g_state.timers[(size_t)arg]);
                Save(); Reschedule(); Repaint();
            }
            return;
        case CmdTimerReset:
            if (arg >= 0 && arg < (int)g_state.timers.size()) {
                Timer& t = g_state.timers[(size_t)arg];
                t.running = false;
                t.remaining = t.duration;
                Save(); Reschedule(); Repaint();
            }
            return;
        case CmdTimerEdit:
            if (arg >= 0 && arg < (int)g_state.timers.size()) { g_view.sel = arg; OpenTimerSheet(arg); }
            return;
        case CmdTimerDelete:
            if (arg >= 0 && arg < (int)g_state.timers.size()) {
                g_state.timers.erase(g_state.timers.begin() + arg);
                if (g_view.sel >= (int)g_state.timers.size()) g_view.sel = (int)g_state.timers.size() - 1;
                if (g_view.sel < 0) g_view.sel = 0;
                Save(); Reschedule(); Repaint();
            }
            return;

        case CmdWatchPlay:  StartPauseWatch(); Save(); Reschedule(); Repaint(); return;
        case CmdWatchLap:   Lap(); Save(); Repaint(); return;
        case CmdWatchReset: ResetWatch(); Save(); Reschedule(); Repaint(); return;

        case CmdAlarmAdd:
            if ((int)g_state.alarms.size() < alarm::kMaxAlarms) OpenAlarmSheet(-1);
            return;
        case CmdAlarmEdit: OpenAlarmSheet(arg); return;
        case CmdAlarmToggle:
            if (arg >= 0 && arg < (int)g_state.alarms.size()) {
                alarm::Alarm& a = g_state.alarms[(size_t)arg];
                a.enabled = !a.enabled;
                if (a.enabled) alarm::Arm(a, now);       // turning one on rings only for the future
                else           a.snoozeUntilUtc = 0;
                Save(); Reschedule(); Repaint();
            }
            return;
        case CmdAlarmDelete: DeleteAlarm(arg); Repaint(); return;
        case CmdRingSnooze:  EndRing(true); return;
        case CmdRingDismiss: EndRing(false); return;

        case CmdSave:   SaveSheet(); return;
        case CmdCancel: CloseSheet(); return;
        case CmdDelete:
            if (g_view.sheet == SheetAlarm) { DeleteAlarm(g_view.ad.index); CloseSheet(); }
            return;
        case CmdName: {
            const bool was = g_view.focus == FieldName;
            TextField& f = NameFieldOf();
            if (!was) { FocusField(FieldName); }
            else if (g_wnd) {
                HDC dc = GetDC(g_wnd);
                if (dc) {
                    theme::SetDpi((UINT)(g_scale * 96.0f + 0.5f));
                    f.caret = f.anchor = CaretAt(dc, f.s, clickX - g_view.nameRect.left);
                    ReleaseDC(g_wnd, dc);
                }
            }
            Repaint();
            return;
        }
        case CmdSpinFocus: FocusField(arg); Repaint(); return;
        case CmdSpinUp:    SpinField(arg, +1); Repaint(); return;
        case CmdSpinDown:  SpinField(arg, -1); Repaint(); return;
    }
    if (!ea) return;
    switch (cmd) {
        case CmdKind:     SetKind(arg); break;
        case CmdWeekday:  ToggleBit(ea->weekdays, arg); break;
        case CmdLimitWd:  ToggleBit(ea->weekdays, arg); break;
        case CmdMonthDay: ToggleBit(ea->monthDays, arg); break;
        case CmdLastDay:  ea->monthDays ^= alarm::kLastDayBit; break;
        case CmdWeek:     ToggleBit(ea->weeks, arg); break;
        case CmdMonth:    ToggleBit(ea->months, arg); break;
        case CmdLimitOpen: g_view.ad.limitOpen = !g_view.ad.limitOpen; break;
        case CmdUntil:
            if (ea->until) ea->until = 0;
            else {
                const alarm::LocalTime l = alarm::SystemZone().ToLocal(now + 31 * alarm::kSecond * 86400);
                SetUntil(*ea, l.year, l.month, l.day);
            }
            break;
        case CmdDay:      SetDate(*ea, now + (Ticks)arg * 86400 * kSecond); break;
        case CmdSound:    ea->sound = !ea->sound; break;
        case CmdSnoozeLen: ea->snoozeMin = arg; break;
        case CmdAmPm:
            if (arg == 0 && ea->hour >= 12) ea->hour -= 12;
            else if (arg == 1 && ea->hour < 12) ea->hour += 12;
            break;
        default: return;
    }
    Repaint();
}

// ------------------------------------------------------------------ keys
void HandleKey(UINT vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;

    if (g_view.sheet != SheetNone) {
        if (vk == VK_ESCAPE) { CloseSheet(); return; }
        if (vk == VK_RETURN) { SaveSheet(); return; }
        if (vk == VK_TAB)    { MoveFocus(shift ? -1 : 1); return; }
        if (g_view.focus == FieldName) {
            if (vk == VK_DOWN) { MoveFocus(1); return; }
            if (vk == VK_UP)   { MoveFocus(-1); return; }
            EditKey(NameFieldOf(), vk, ctrl, shift);
            Repaint();
            return;
        }
        if (vk >= '0' && vk <= '9' && !ctrl)                 TypeDigit((int)(vk - '0'));
        else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)      TypeDigit((int)(vk - VK_NUMPAD0));
        else if (vk == VK_UP)    SpinField(g_view.focus, +1);
        else if (vk == VK_DOWN)  SpinField(g_view.focus, -1);
        else if (vk == VK_LEFT)  MoveFocus(-1);
        else if (vk == VK_RIGHT) MoveFocus(1);
        Repaint();
        return;
    }

    if (g_view.ringing) {
        if (vk == VK_RETURN) { EndRing(true); return; }
        if (vk == VK_ESCAPE || vk == VK_SPACE) { EndRing(false); return; }
    }

    switch (vk) {
        case VK_ESCAPE: TimerHide(); return;
        case VK_TAB:
            GoPage((g_view.page + (shift ? PageCount - 1 : 1)) % PageCount);
            return;
    }

    if (g_view.page == PageTimer) {
        const int n = (int)g_state.timers.size();
        switch (vk) {
            case VK_LEFT:  if (n) { g_view.sel = (g_view.sel + n - 1) % n; Repaint(); } return;
            case VK_RIGHT: if (n) { g_view.sel = (g_view.sel + 1) % n; Repaint(); } return;
            case VK_UP:    if (g_view.sel >= 3) { g_view.sel -= 3; Repaint(); } return;
            case VK_DOWN:  if (g_view.sel + 3 < n) { g_view.sel += 3; Repaint(); } return;
            case VK_RETURN: if (Selected()) Command(CmdTimerEdit, g_view.sel); return;
            case VK_SPACE:  if (Selected()) Command(CmdTimerPlay, g_view.sel); return;
            case 'R':       if (Selected()) Command(CmdTimerReset, g_view.sel); return;
            case 'N':       Command(CmdTimerAdd, 0); return;
            case VK_DELETE: if (Selected()) Command(CmdTimerDelete, g_view.sel); return;
        }
    } else if (g_view.page == PageWatch) {
        switch (vk) {
            case VK_SPACE: Command(CmdWatchPlay, 0); return;
            case 'R':      Command(CmdWatchReset, 0); return;
            case 'L':      Command(CmdWatchLap, 0); return;
        }
    } else if (g_view.page == PageAlarm) {
        if (vk == 'N') Command(CmdAlarmAdd, 0);
    }
}

// ------------------------------------------------------------------ painting
Model BuildModel() {
    Model m;
    m.s = &g_state;
    m.hours24 = H24();
    m.seconds = g_cfg && g_cfg->clockSeconds;
    m.now = NowUtc();
    const alarm::Zone& zone = alarm::SystemZone();
    Ticks best = 0;
    for (size_t i = 0; i < g_state.alarms.size(); ++i) {
        const alarm::Alarm& a = g_state.alarms[i];
        const Ticks w = a.enabled || a.snoozeUntilUtc ? alarm::NextWake(a, m.now, zone) : 0;
        m.wake.push_back(w);
        if (w && (!best || w < best)) { best = w; m.nextAlarm = (int)i; }
    }
    return m;
}

void Paint(HWND wnd) {
    PAINTSTRUCT ps;
    HDC target = BeginPaint(wnd, &ps);
    RECT client;
    GetClientRect(wnd, &client);
    const SIZE size = { client.right, client.bottom };
    theme::SetDpi((UINT)(g_scale * 96.0f + 0.5f));
    g_view.pinned = g_cfg && g_cfg->timerPinned;
    const Model model = BuildModel();

    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(target, size.cx, size.cy) : nullptr;
    if (mem && bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        panel::Paint(mem, size, model, g_view);
        BitBlt(target, 0, 0, size.cx, size.cy, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
    } else {
        panel::Paint(target, size, model, g_view);
    }
    if (bmp) DeleteObject(bmp);     // nothing stays resident while hidden (inv. 67)
    if (mem) DeleteDC(mem);
    EndPaint(wnd, &ps);
    if (g_view.again) InvalidateRect(wnd, nullptr, FALSE);     // a scroll position was clamped
}

const Hit* HitAt(POINT pt) {
    for (size_t i = g_view.hits.size(); i-- > 0;)
        if (PtInRect(&g_view.hits[i].r, pt)) return &g_view.hits[i];
    return nullptr;
}

void SetHot(const Hit* h) {
    const int cmd = h ? h->cmd : CmdNone, arg = h ? h->arg : 0;
    if (cmd != g_view.hotCmd || arg != g_view.hotArg) {
        g_view.hotCmd = cmd;
        g_view.hotArg = arg;
        Repaint();
    }
}

bool InDragZone(POINT pt) {
    return pt.x < (int)(kRail * g_scale) || pt.y < (int)(kTop * g_scale);
}

void ShowContextMenu() {
    enum { kPin = 1, kHide, kSettings };
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kPin, L"Pin in place (click-through)");
    AppendMenuW(menu, MF_STRING, kHide, L"Hide");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kSettings, L"Clock panel settings...");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    const int cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    switch (cmd) {
        case kPin:      TimerSetPinned(true); break;
        case kHide:     TimerHide(); break;
        case kSettings: AppOpenClockSettings(); break;
    }
}

// A timer's alarm (not an alarm clock's ringing) is silenced by any key or click.
bool TimerRingActive() { return g_alarming && !g_view.ringing; }

LRESULT CALLBACK TimerProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:      Paint(wnd); return 0;
        case WM_ERASEBKGND: return 1;

        case WM_TIMER:
            if (wp == kTimerTick) { Tick(true); return 0; }
            if (wp == kTimerIdle) {
                KillTimer(wnd, kTimerIdle);
                if (!IsWindowVisible(wnd)) AppScheduleTrim(2 * 1000);
                return 0;
            }
            if (wp == kTimerAlarm) {
                if (g_view.ringing) EndRing(false);       // 60 s unanswered = dismissed
                else StopAlarm();
                return 0;
            }
            if (wp == kTimerAway) {
                KillTimer(wnd, kTimerAway);
                if (!g_awayText.empty()) {
                    AppTrayBalloon(g_awayHasAlarm ? L"Missed while you were away" : L"Finished while you were away",
                                   g_awayText.c_str());
                    g_awayText.clear();
                    g_awayHasAlarm = false;
                }
                return 0;
            }
            return 0;

        case WM_TIMECHANGE:       // the user moved the clock: timers move with it, alarms follow the local clock
            alarm::InvalidateSystemZone();
            Tick(false);
            return 0;

        case WM_SETTINGCHANGE:    // a new time zone or DST rule: the per-year tables are stale
            if (lp && wcscmp((const wchar_t*)lp, L"intl") == 0) alarm::InvalidateSystemZone();
            return 0;

        case WM_DPICHANGED:        // dragged to a monitor with another scale (R2)
            ReleaseFonts();
            SetScale(HIWORD(wp));
            Relayout();
            return 0;

        case WM_KEYDOWN:
            if (TimerRingActive()) { StopAlarm(); return 0; }       // any key silences it
            HandleKey((UINT)wp);
            return 0;

        case WM_CHAR:
            if (g_view.sheet != SheetNone && g_view.focus == FieldName && wp >= 32 && wp != 127) {
                InsertText(NameFieldOf(), std::wstring(1, (wchar_t)wp));
                Repaint();
            }
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(wnd, &pt);
            const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            if (!steps) return 0;
            const Hit* h = HitAt(pt);
            if (h && h->wheel) { SpinField(h->arg, steps > 0 ? +1 : -1); Repaint(); return 0; }
            if (PtInRect(&g_view.scrollRect, pt) && g_view.scrollMax > 0) {
                int* pos = g_view.sheet != SheetNone ? &g_view.sheetScroll : &g_view.scroll[g_view.page];
                *pos = (std::max)(0, (std::min)(g_view.scrollMax, *pos - steps * (int)(60 * g_scale)));
                Repaint();
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (TimerRingActive()) return 0;                 // the click that silences it comes up
            const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const Hit* h = HitAt(pt);
            if (h) {
                g_view.downCmd = h->cmd;
                g_view.downArg = h->arg;
                SetCapture(wnd);
                // The name field takes the caret on press, as any text box does.
                if (h->cmd == CmdName) { Command(CmdName, 0, pt.x); }
                Repaint();
                return 0;
            }
            if (g_view.sheet != SheetNone || !g_cfg || g_cfg->timerPinned) return 0;
            if (!InDragZone(pt)) return 0;
            g_dragging = true;
            GetCursorPos(&g_dragOrigin);
            GetWindowRect(wnd, &g_dragStart);
            SetCapture(wnd);
            return 0;
        }

        case WM_CAPTURECHANGED:
            g_dragging = false;
            if (g_view.downCmd != CmdNone) { g_view.downCmd = CmdNone; Repaint(); }
            return 0;

        case WM_RBUTTONUP:
            if (TimerRingActive()) { StopAlarm(); return 0; }
            if (g_cfg && !g_cfg->timerPinned) ShowContextMenu();
            return 0;

        case WM_LBUTTONUP: {
            if (g_dragging) {
                g_dragging = false;
                ReleaseCapture();
                RECT r;
                GetWindowRect(wnd, &r);
                if (g_cfg) {
                    g_cfg->timerX = r.left;
                    g_cfg->timerY = r.top;
                    AppSaveConfig();
                }
                return 0;
            }
            if (TimerRingActive()) { StopAlarm(); return 0; }
            const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const int dc = g_view.downCmd, da = g_view.downArg;
            g_view.downCmd = CmdNone;
            if (GetCapture() == wnd) ReleaseCapture();
            if (dc == CmdNone) return 0;
            const Hit* h = HitAt(pt);
            // The command must be copied: running it repaints, and the hit list is rebuilt.
            if (h && h->cmd == dc && h->arg == da && dc != CmdName) Command(dc, da, pt.x);
            else Repaint();
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (g_dragging) {
                POINT now;
                GetCursorPos(&now);
                RECT r;
                GetWindowRect(wnd, &r);
                const SIZE size = { r.right - r.left, r.bottom - r.top };
                int x = g_dragStart.left + (now.x - g_dragOrigin.x);
                int y = g_dragStart.top + (now.y - g_dragOrigin.y);
                ClampOnScreen(&x, &y, size);
                SetWindowPos(wnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            SetHot(HitAt(pt));
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, wnd, 0 };
            TrackMouseEvent(&track);
            return 0;
        }

        case WM_MOUSELEAVE:
            SetHot(nullptr);
            return 0;

        case WM_DESTROY:
            g_wnd = nullptr;
            return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

bool EnsureWindow() {
    if (g_wnd) return true;
    if (!g_inst) return false;
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"ProWindows clock panel",
                            WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, g_inst, nullptr);
    if (!g_wnd) return false;
    theme::DarkTitleBar(g_wnd);
    SetCornerPreference(g_wnd, 2);
    return true;
}

// Pinned: layered + transparent, so clicks fall through (WS_EX_TRANSPARENT only
// does that on a layered window), and never activated. Unpinned takes all three off.
void ApplyPin() {
    if (!g_wnd || !g_cfg) return;
    const LONG bits = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    const bool pinned = g_cfg->timerPinned;
    // While anything rings the click-through comes off so Snooze / Dismiss / any
    // click work; the position stays locked (the drag test reads timerPinned).
    const bool ringing = g_alarming || g_view.ringing;
    g_view.pinned = pinned;
    if (pinned) { g_view.sheet = SheetNone; g_dragging = false; }   // a pinned panel can take no key to finish an edit
    LONG ex = GetWindowLongW(g_wnd, GWL_EXSTYLE);
    const LONG wanted = pinned ? (ringing ? ((ex | WS_EX_LAYERED) & ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE))
                                          : (ex | bits))
                               : (ex & ~bits);
    if (wanted != ex) {
        SetWindowLongW(g_wnd, GWL_EXSTYLE, wanted);
        if (pinned) SetLayeredWindowAttributes(g_wnd, 0, 255, LWA_ALPHA);
        else        RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
    }
    Repaint();
}

// Shown without taking the keyboard from whatever the user is typing in (R3);
// clicking the panel activates it, and then its keys work.
void ShowPanel() {
    if (!EnsureWindow()) return;
    g_view.sheet = SheetNone;
    if (g_view.sel >= (int)g_state.timers.size()) g_view.sel = (int)g_state.timers.size() - 1;
    if (g_view.sel < 0) g_view.sel = 0;
    ApplyPin();
    Place();
    KillTimer(g_wnd, kTimerIdle);
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    Repaint();
    Reschedule();
}

// Out of sight, config untouched: game mode and a dark display come this way.
void HidePanel() {
    if (!g_wnd) return;
    g_dragging = false;
    g_view.sheet = SheetNone;      // an unsaved edit is dropped; nothing was changed yet
    ShowWindow(g_wnd, SW_HIDE);
    SetTimer(g_wnd, kTimerIdle, kIdleMs, nullptr);
    g_view.hotCmd = g_view.downCmd = CmdNone;
    ReleaseFonts();
    Reschedule();       // hidden: no per-second wake-ups for the display
}

} // namespace

// ---------------------------------------------------------------- public
void TimerSetStorePath(const std::wstring& path) { g_store = path; }

void TimerInit(HINSTANCE inst, Config* cfg) {
    g_inst = inst;
    g_cfg  = cfg;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = TimerProc;
    wc.hInstance     = inst;
    wc.lpszClassName = kClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_state = State();
    if (!LoadFile(StorePath(), &g_state)) g_state = State();
    g_view = View();
    g_view.page = (std::max)(0, (std::min)((int)PageCount - 1, g_state.page));
    g_ringQueue.clear();

    // The window stays (hidden, a few hundred bytes) because the one OS timer
    // needs somewhere to land; it has no bitmap and no thread while hidden.
    EnsureWindow();
    Tick(false);      // anything that ended while the PC was off fires, once
}

void TimerShutdown() {
    g_view.ringing = false;
    StopAlarm();
    ReleaseFonts();
    if (g_wnd) {
        KillTimer(g_wnd, kTimerTick);
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
    }
}

bool TimerVisible() { return Visible(); }

void TimerSetVisible(bool visible) {
    if (visible) { if (!Visible()) ShowPanel(); }
    else if (Visible()) HidePanel();
}

// The user puts it away (Esc, the menu): remembered, so it stays away.
void TimerHide() {
    if (g_cfg) g_cfg->timerShown = false;
    HidePanel();
    if (g_cfg) { AppSaveConfig(); AppRefreshSettings(); }
}

void TimerToggle() {
    if (TimerRingActive()) StopAlarm();
    if (!g_cfg) return;
    g_cfg->timerShown = !g_cfg->timerShown;
    if (g_cfg->timerShown) g_view.page = PageClock;      // Win+W opens on the clock
    AppSaveConfig();
    AppRefreshSettings();
    AppUpdateOverlays();         // game mode and a dark display still win
}

void TimerSetPinned(bool pinned) {
    if (!g_cfg) return;
    g_cfg->timerPinned = pinned;
    ApplyPin();
    AppSaveConfig();
    AppRefreshSettings();
}

bool TimerPinned() { return g_cfg && g_cfg->timerPinned; }
bool TimerAlarming() { return g_alarming; }
void TimerStopAlarm() { StopAlarm(); }
bool TimerAlarmRinging() { return g_view.ringing; }
void TimerSnoozeAlarm() { EndRing(true); }
void TimerDismissAlarm() { EndRing(false); }

void TimerApplyConfig() {
    g_lastTickIdx = -1;
    if (g_wnd) {
        ApplyPin();
        if (Visible()) { Place(); Repaint(); }
    }
    Reschedule();
}

void TimerCheckNow() { if (g_wnd) Tick(false); }

bool TimerHitRect(int cmd, int arg, RECT* out) {
    for (size_t i = g_view.hits.size(); i-- > 0;) {
        const Hit& h = g_view.hits[i];
        if (h.cmd == cmd && h.arg == arg) { if (out) *out = h.r; return true; }
    }
    return false;
}

void TimerRingAlarm(int index) {
    if (index < 0 || index >= (int)g_state.alarms.size()) return;
    StartRing(index, NowUtc());
}

// ---------------------------------------------------- the settings page's view
const timer::State& TimerModel() { return g_state; }
unsigned TimerRevision() { return g_rev; }

namespace {
void Changed() { Save(); Reschedule(); Repaint(); }
}

bool TimerAddPaused(timer::Ticks duration) {
    if ((int)g_state.timers.size() >= timer::kMaxTimers || duration <= 0) return false;
    timer::Timer t;
    t.label = NewTimerName();
    t.duration = t.remaining = (std::min)(duration, timer::kMaxDuration);
    g_state.timers.push_back(t);
    Changed();
    return true;
}

void TimerStartPause(int i) {
    if (i < 0 || i >= (int)g_state.timers.size()) return;
    StartPauseTimer(g_state.timers[(size_t)i]);
    Changed();
}

void TimerResetAt(int i) {
    if (i < 0 || i >= (int)g_state.timers.size()) return;
    timer::Timer& t = g_state.timers[(size_t)i];
    t.running = false;
    t.remaining = t.duration;
    Changed();
}

void TimerDeleteAt(int i) {
    if (i < 0 || i >= (int)g_state.timers.size()) return;
    g_state.timers.erase(g_state.timers.begin() + i);
    if (g_view.sel >= (int)g_state.timers.size()) g_view.sel = (int)g_state.timers.size() - 1;
    if (g_view.sel < 0) g_view.sel = 0;
    Changed();
}

void StopwatchStartStop() { StartPauseWatch(); Changed(); }
void StopwatchLap()       { Lap(); Changed(); }
void StopwatchReset()     { ResetWatch(); Changed(); }

} // namespace awa
