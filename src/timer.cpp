#include "timer.h"
#include "winutil.h"
#include "theme.h"
#include "app.h"
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
// The panel.
// =====================================================================
namespace {

using namespace timer;

constexpr wchar_t kClass[] = L"ProWindows_Timer";

constexpr UINT_PTR kTimerTick  = 1;    // the one wake-up (see Reschedule)
// Set when the panel is hidden; when it fires the panel has been out of sight
// long enough that the shell is asked to give the memory back (inv. 67).
constexpr UINT_PTR kTimerIdle  = 2;
constexpr UINT     kIdleMs     = 3 * 60 * 1000;
constexpr UINT_PTR kTimerAlarm = 3;    // one-shot: the alarm gives up after a minute
constexpr UINT     kAlarmMs    = 60 * 1000;
constexpr UINT_PTR kTimerAway  = 4;    // one-shot: say what finished while we were off

// Unscaled layout, DIP.
constexpr int kWidth   = 640;
constexpr int kHeaderH = 56;
constexpr int kRowH    = 64;
constexpr int kPad     = 14;
constexpr int kFootH   = 34;           // one line of prompts
constexpr int kLapRows = 6;
constexpr int kLapH    = 28;
constexpr int kWatchH  = 150;          // the big stopwatch reading and its status line

HINSTANCE g_inst = nullptr;
Config*   g_cfg  = nullptr;
HWND      g_wnd  = nullptr;
float     g_scale = 1.0f;

State        g_state;
std::wstring g_store;

int  g_page = 0;                       // 0 timers, 1 stopwatch
int  g_sel  = 0;

// The duration editor: days : hh : mm : ss.
bool g_edit = false;
int  g_field = 2;
int  g_fv[4] = {};                     // the values
int  g_typed[4] = {};                  // digits typed into each since it got focus
bool g_editIsNew = false;              // Esc on a brand-new timer takes it back out

bool g_alarming = false;
long long g_lastTickIdx = -1;
std::wstring g_awayText;
std::vector<BYTE> g_tickWav;

struct PromptHit { RECT r; UINT vk; };
std::vector<PromptHit> g_promptHits;
int g_hotPrompt = -1;
RECT g_tabRects[2] = {};

bool  g_dragging = false;              // unpinned: a drag by the header
POINT g_dragOrigin = {};
RECT  g_dragStart = {};

std::wstring StorePath() { return g_store.empty() ? ConfigDir() + L"\\timers.ini" : g_store; }

void Save() {
    if (!SaveFile(StorePath(), g_state)) AWA_LOG(L"timer: could not save %s", StorePath().c_str());
}

bool Visible() { return g_wnd && IsWindowVisible(g_wnd); }
void Repaint() { if (Visible()) InvalidateRect(g_wnd, nullptr, FALSE); }

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

void StopAlarm() {
    if (!g_alarming) return;
    g_alarming = false;
    PlaySoundW(nullptr, nullptr, 0);
    if (g_wnd) KillTimer(g_wnd, kTimerAlarm);
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
}

// ------------------------------------------------------------------ layout
int Rows() { return (std::max)(1, (int)g_state.timers.size()); }

int FootLines() { return (g_page == 0 && !g_edit) ? 2 : 1; }

SIZE WindowSize() {
    SIZE s;
    s.cx = (int)(kWidth * g_scale);
    int body;
    if (g_page == 0) body = Rows() * kRowH + kPad;
    else             body = kWatchH + kLapRows * kLapH + kPad;
    s.cy = (int)((kHeaderH + body + FootLines() * kFootH) * g_scale);
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

// Same sizes after a page change, a new timer, a removed one; the top-left
// corner stays where it is unless that would push the panel off the screen.
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
// Nothing polls. With nothing running there is no OS timer at all; otherwise
// exactly one SetTimer sleeps until the earliest of: a timer's end, the next
// change the panel would show (only while it is up), the next tick sound, and
// a minute (a ceiling that also covers the clock being moved under us).
void Reschedule() {
    if (!g_wnd) return;
    if (!AnythingRunning(g_state)) { KillTimer(g_wnd, kTimerTick); return; }

    const Ticks now = NowUtc();
    const bool shown = Visible();
    Ticks delay = 60 * kSecond;
    auto sooner = [&](Ticks d) { if (d < delay) delay = d; };

    for (const Timer& t : g_state.timers) {
        if (!t.running) continue;
        const Ticks left = Remaining(t, now);
        sooner(left);
        if (shown && g_page == 0) {
            const Ticks part = left % kSecond;       // the shown (rounded-up) second changes here
            sooner(part == 0 ? kSecond : part);
        }
    }
    if (shown && g_page == 1 && g_state.watch.running) {
        const Ticks step = kSecond / 10;             // the stopwatch shows tenths
        sooner(step - Elapsed(g_state.watch, now) % step);
    }
    const int mode = g_cfg ? g_cfg->timerTick : 0;
    if (mode == 1)      sooner(kSecond - now % kSecond);
    else if (mode == 2) sooner(60 * kSecond - now % (60 * kSecond));

    // A few ms past the boundary, so the timer does not land just before it.
    long long ms = delay / 10000 + 8;
    if (ms < 10) ms = 10;
    if (ms > 60000) ms = 60000;       // SetTimer's own cap is ~24.8 days
    SetTimer(g_wnd, kTimerTick, (UINT)ms, nullptr);
}

void Announce(const std::vector<Finished>& done, Ticks now) {
    for (const Finished& f : done) {
        const bool away = now - f.endUtc > 10 * kSecond;
        if (away) {
            // Finished while the PC was off or asleep: said once, a little later,
            // so a balloon at start-up finds the tray icon in place.
            if (!g_awayText.empty()) g_awayText += L"\n";
            g_awayText += f.label + L" - ended " + LocalStamp(f.endUtc);
            if (g_wnd) SetTimer(g_wnd, kTimerAway, 1500, nullptr);
            continue;
        }
        AppTrayBalloon(L"Timer finished", f.label.c_str());
        if (g_cfg && g_cfg->timerAlarm) {
            StartAlarm();
            g_page = 0;
            g_sel = f.index;
            g_edit = false;
            // Shown, not toggled, and not activated (R3): the user may be typing in
            // something else. Any key or click in the panel, or the tray, stops it.
            if (!g_cfg->timerShown) {
                g_cfg->timerShown = true;
                AppSaveConfig();
                AppRefreshSettings();
            }
            if (!Visible()) AppUpdateOverlays();     // game mode and a dark display still win
            else            Relayout();
        }
    }
}

// What a wake-up does: fire what finished, tick, repaint, plan the next.
void Tick(bool fromTimer) {
    const Ticks now = NowUtc();
    const std::vector<Finished> done = TakeFinished(g_state, now);
    if (!done.empty()) { Save(); Announce(done, now); }

    const int mode = g_cfg ? g_cfg->timerTick : 0;
    if (fromTimer && mode && !g_alarming && AnythingRunning(g_state)) {
        const long long idx = now / (mode == 2 ? 60 * kSecond : kSecond);
        if (idx != g_lastTickIdx) { g_lastTickIdx = idx; PlayTick(); }
    }
    Repaint();
    Reschedule();
}

// ------------------------------------------------------------------ actions
Timer* Selected() {
    return (g_sel >= 0 && g_sel < (int)g_state.timers.size()) ? &g_state.timers[(size_t)g_sel] : nullptr;
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

void SplitFields(Ticks d) {
    const long long secs = d / kSecond;
    g_fv[0] = (int)(secs / 86400);
    g_fv[1] = (int)((secs / 3600) % 24);
    g_fv[2] = (int)((secs / 60) % 60);
    g_fv[3] = (int)(secs % 60);
}

void BeginEdit(bool isNew) {
    Timer* t = Selected();
    if (!t) return;
    SplitFields(t->duration);
    g_edit = true;
    g_editIsNew = isNew;
    g_field = 2;
    for (int& n : g_typed) n = 0;
    Relayout();
}

void EndEdit() {
    g_edit = false;
    if (g_editIsNew) {
        // Cancelled before it was ever set: it was never wanted.
        if (Selected()) g_state.timers.erase(g_state.timers.begin() + g_sel);
        if (g_sel >= (int)g_state.timers.size()) g_sel = (int)g_state.timers.size() - 1;
        if (g_sel < 0) g_sel = 0;
        g_editIsNew = false;
        Save();
    }
    Relayout();
}

void CommitEdit() {
    Timer* t = Selected();
    if (!t) { g_edit = false; return; }
    Ticks d = ((Ticks)g_fv[0] * 86400 + g_fv[1] * 3600 + g_fv[2] * 60 + g_fv[3]) * kSecond;
    if (d > kMaxDuration) d = kMaxDuration;
    if (d <= 0) return;                  // a zero timer is nothing; keep editing
    t->duration  = d;
    t->remaining = d;                    // a new length restarts it, paused
    t->running   = false;
    g_edit = false;
    g_editIsNew = false;
    Save();
    Reschedule();
    Relayout();
}

void NewTimer() {
    if ((int)g_state.timers.size() >= kMaxTimers) return;
    int n = 1;
    for (;; ++n) {
        bool used = false;
        for (const Timer& t : g_state.timers) if (t.label == L"Timer " + std::to_wstring(n)) used = true;
        if (!used) break;
    }
    Timer t;
    t.label = L"Timer " + std::to_wstring(n);
    t.duration = t.remaining = 5 * 60 * kSecond;
    g_state.timers.push_back(t);
    g_sel = (int)g_state.timers.size() - 1;
    BeginEdit(true);
}

void TypeDigit(int d) {
    const int f = g_field;
    const int maxDigits = f == 0 ? 4 : 2;
    if (g_typed[f] == 0 || g_typed[f] >= maxDigits) { g_fv[f] = d; g_typed[f] = 1; }
    else { g_fv[f] = g_fv[f] * 10 + d; ++g_typed[f]; }
    const int cap = f == 0 ? 9999 : (f == 1 ? 23 : 59);
    if (g_fv[f] > cap) g_fv[f] = cap;
}

// One key, from the keyboard or from clicking its prompt (inv. 74).
void HandleKey(UINT vk) {
    if (g_edit) {
        if (vk >= '0' && vk <= '9')                 TypeDigit((int)(vk - '0'));
        else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) TypeDigit((int)(vk - VK_NUMPAD0));
        else if (vk == VK_BACK)  { g_fv[g_field] /= 10; g_typed[g_field] = 1; }
        else if (vk == VK_LEFT)  { g_field = (g_field + 3) % 4; g_typed[g_field] = 0; }
        else if (vk == VK_RIGHT) { g_field = (g_field + 1) % 4; g_typed[g_field] = 0; }
        else if (vk == VK_RETURN) { CommitEdit(); return; }
        else if (vk == VK_ESCAPE) { EndEdit(); return; }
        Repaint();
        return;
    }

    switch (vk) {
        case VK_ESCAPE: TimerHide(); return;
        case VK_TAB:
            g_page ^= 1;
            Relayout();
            Reschedule();
            return;
    }

    if (g_page == 0) {
        const int n = (int)g_state.timers.size();
        switch (vk) {
            case VK_UP:   if (n) { g_sel = (g_sel + n - 1) % n; Repaint(); } return;
            case VK_DOWN: if (n) { g_sel = (g_sel + 1) % n; Repaint(); } return;
            case VK_RETURN: BeginEdit(false); return;
            case VK_SPACE:
                if (Timer* t = Selected()) { StartPauseTimer(*t); Save(); Reschedule(); Repaint(); }
                return;
            case 'R':
                if (Timer* t = Selected()) {
                    t->running = false;
                    t->remaining = t->duration;
                    Save(); Reschedule(); Repaint();
                }
                return;
            case 'N': NewTimer(); return;
            case VK_DELETE:
                if (Selected()) {
                    g_state.timers.erase(g_state.timers.begin() + g_sel);
                    if (g_sel >= (int)g_state.timers.size()) g_sel = (int)g_state.timers.size() - 1;
                    if (g_sel < 0) g_sel = 0;
                    Save(); Reschedule(); Relayout();
                }
                return;
        }
    } else {
        switch (vk) {
            case VK_SPACE: StartPauseWatch(); Save(); Reschedule(); Repaint(); return;
            case 'R':      ResetWatch(); Save(); Reschedule(); Repaint(); return;
            case 'L':      Lap(); Save(); Repaint(); return;
        }
    }
}

// ------------------------------------------------------------------ painting
struct PromptText { const wchar_t* key; std::wstring word; UINT vk; };

std::vector<PromptText> Prompts() {
    std::vector<PromptText> p;
    if (g_edit) {
        p.push_back({ L"\x2190 \x2192", L"Field", VK_RIGHT });
        p.push_back({ L"Enter", L"Set", VK_RETURN });
        p.push_back({ L"Esc", L"Cancel", VK_ESCAPE });
        return p;
    }
    if (g_page == 0) {
        const Timer* t = Selected();
        p.push_back({ L"Tab", L"Stopwatch", VK_TAB });
        if (t) {
            p.push_back({ L"\x2191 \x2193", L"Select", VK_DOWN });
            p.push_back({ L"Space", t->running ? L"Pause" : L"Start", VK_SPACE });
            p.push_back({ L"Enter", L"Edit", VK_RETURN });
            p.push_back({ L"R", L"Reset", 'R' });
        }
        if ((int)g_state.timers.size() < kMaxTimers) p.push_back({ L"N", L"New", 'N' });
        if (t) p.push_back({ L"Del", L"Remove", VK_DELETE });
        p.push_back({ L"Esc", L"Hide", VK_ESCAPE });
    } else {
        p.push_back({ L"Tab", L"Timer", VK_TAB });
        p.push_back({ L"Space", g_state.watch.running ? L"Pause" : L"Start", VK_SPACE });
        p.push_back({ L"L", L"Lap", 'L' });
        p.push_back({ L"R", L"Reset", 'R' });
        p.push_back({ L"Esc", L"Hide", VK_ESCAPE });
    }
    return p;
}

// A reading drawn digit by digit in equal cells, so the figures do not shuffle
// sideways as 1 and 0 change width. `align`: -1 left of x, 0 centred, +1 right.
int CellsWidth(HDC dc, const std::wstring& text) {
    SIZE zero = {};
    GetTextExtentPoint32W(dc, L"0", 1, &zero);
    int w = 0;
    for (wchar_t c : text) {
        if (c >= L'0' && c <= L'9') { w += zero.cx; continue; }
        SIZE sz = {};
        GetTextExtentPoint32W(dc, &c, 1, &sz);
        w += sz.cx;
    }
    return w;
}

void DrawCells(HDC dc, HFONT font, const std::wstring& text, int x, int top, int bottom,
               int align, COLORREF ink) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ink);
    const int total = CellsWidth(dc, text);
    int left = align < 0 ? x : (align == 0 ? x - total / 2 : x - total);
    SIZE zero = {};
    GetTextExtentPoint32W(dc, L"0", 1, &zero);
    for (wchar_t c : text) {
        SIZE sz = {};
        GetTextExtentPoint32W(dc, &c, 1, &sz);
        const bool digit = c >= L'0' && c <= L'9';
        const int cell = digit ? zero.cx : sz.cx;
        RECT r = { left + (cell - sz.cx) / 2, top, left + cell + sz.cx, bottom };
        DrawTextW(dc, &c, 1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        left += cell;
    }
    SelectObject(dc, old);
}

std::wstring RowStatus(const Timer& t, Ticks now) {
    const std::wstring len = FormatDuration(t.duration) + L" timer";
    const std::wstring dot = L"  \x00B7  ";
    if (t.running) return L"Running" + dot + L"ends " + LocalStamp(now + Remaining(t, now));
    if (t.remaining <= 0) return L"Finished" + dot + len;
    if (t.remaining < t.duration) return L"Paused" + dot + len;
    return L"Ready" + dot + len;
}

void PaintEditor(HDC dc, const RECT& row, float s) {
    using theme::Font;
    static const wchar_t* kUnit[4] = { L"d", L"h", L"m", L"s" };
    const int cy = (row.top + row.bottom) / 2;
    int x = row.right - (int)(kPad * s) - (int)(8 * s);
    for (int i = 3; i >= 0; --i) {
        wchar_t buf[16];
        swprintf_s(buf, i == 0 ? L"%d" : L"%02d", g_fv[i]);
        const int unitW = theme::Measure(dc, Font::Small, kUnit[i]);
        RECT ur = { x - unitW, cy - (int)(10 * s), x, cy + (int)(14 * s) };
        theme::Print(dc, Font::Small, kUnit[i], ur, theme::TextDim, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
        x -= unitW + (int)(12 * s);

        HGDIOBJ old = SelectObject(dc, theme::Get(Font::Heading));
        const int w = CellsWidth(dc, buf);
        SelectObject(dc, old);
        RECT box = { x - w - (int)(8 * s), cy - (int)(20 * s), x + (int)(8 * s), cy + (int)(20 * s) };
        if (i == g_field) {
            theme::Gradient(dc, box, theme::Metal1, theme::Metal3);
            theme::Frame(dc, box, theme::MetalEdge, 255);
        } else {
            theme::Frame(dc, box, theme::Rule, 255);
        }
        DrawCells(dc, theme::Get(Font::Heading), buf, x, box.top, box.bottom, 1,
                  i == g_field ? theme::TextHi : theme::Text);
        x = box.left - (int)(10 * s);
    }
}

void PaintTimers(HDC dc, int top, int width, float s) {
    using theme::Font;
    const int pad = (int)(kPad * s);
    const Ticks now = NowUtc();
    if (g_state.timers.empty()) {
        RECT r = { pad, top, width - pad, top + (int)(kRowH * s) };
        theme::Print(dc, Font::Row, L"No timers. Press N to add one.", r, theme::TextDim,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    for (int i = 0; i < (int)g_state.timers.size(); ++i) {
        const Timer& t = g_state.timers[(size_t)i];
        const bool sel = (i == g_sel);
        RECT row = { pad, top + (int)(i * kRowH * s), width - pad, top + (int)((i + 1) * kRowH * s) };
        if (sel) theme::RowFocus(dc, row, 1.0f);
        else { RECT line = { row.left, row.bottom - 1, row.right, row.bottom }; theme::Wash(dc, line, theme::Line, 255); }

        RECT name = { row.left + pad, row.top + (int)(8 * s), row.left + (int)(240 * s), row.top + (int)(32 * s) };
        theme::Print(dc, Font::Row, t.label, name, sel ? theme::TextHi : theme::Text,
                     DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT sub = { row.left + pad, row.top + (int)(33 * s), row.left + (int)(300 * s), row.bottom - (int)(6 * s) };
        const bool editing = sel && g_edit;
        theme::Print(dc, Font::Small, editing ? std::wstring(L"Type digits; arrows change field") : RowStatus(t, now),
                     sub, sel ? theme::Text : theme::TextDim,
                     DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (editing) { PaintEditor(dc, row, s); continue; }
        const bool finished = !t.running && t.remaining <= 0;
        const Ticks shown = CeilSecond(Remaining(t, now));
        COLORREF ink = t.running ? theme::TextHi : (finished ? theme::Warn : theme::TextDim);
        if (sel && !t.running && !finished) ink = theme::Text;
        DrawCells(dc, theme::Get(Font::Heading), FormatDuration(shown), row.right - pad, row.top, row.bottom, 1, ink);
    }
}

void PaintWatch(HDC dc, int top, int width, float s) {
    using theme::Font;
    const int pad = (int)(kPad * s);
    const Ticks now = NowUtc();
    const Stopwatch& w = g_state.watch;

    LOGFONTW lf = {};
    GetObjectW(theme::Get(Font::Heading), sizeof(lf), &lf);
    lf.lfHeight = -(int)(60 * s);
    lf.lfWidth = 0;
    HFONT big = CreateFontIndirectW(&lf);
    const int bandBottom = top + (int)(kWatchH * s);
    DrawCells(dc, big ? big : theme::Get(Font::Heading), FormatDuration(Elapsed(w, now), true),
              width / 2, top + (int)(14 * s), bandBottom - (int)(40 * s), 0,
              w.running ? theme::TextHi : theme::Text);
    if (big) DeleteObject(big);
    RECT st = { pad, bandBottom - (int)(40 * s), width - pad, bandBottom - (int)(12 * s) };
    theme::Print(dc, Font::Small, w.running ? L"Running" : (Elapsed(w, now) > 0 ? L"Paused" : L"Stopped"),
                 st, theme::TextDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT line = { pad, bandBottom, width - pad, bandBottom + 1 };
    theme::Wash(dc, line, theme::Line, 255);
    const int shown = (std::min)((int)w.laps.size(), kLapRows);
    if (shown == 0) {
        RECT r = { pad, bandBottom, width - pad, bandBottom + (int)(kLapH * s) * 2 };
        theme::Print(dc, Font::Small, L"Press L to mark a lap.", r, theme::TextMute,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    for (int i = 0; i < shown; ++i) {
        const Ticks total = w.laps[(size_t)i];
        const Ticks prev = (i + 1 < (int)w.laps.size()) ? w.laps[(size_t)i + 1] : 0;
        RECT row = { pad, bandBottom + (int)(i * kLapH * s), width - pad, bandBottom + (int)((i + 1) * kLapH * s) };
        RECT a = { row.left + pad, row.top, row.left + (int)(120 * s), row.bottom };
        theme::Print(dc, Font::Row, L"Lap " + std::to_wstring(w.lapSeq - i), a, theme::TextDim,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT c = { row.left, row.top, row.right - pad - (int)(150 * s), row.bottom };
        theme::Print(dc, Font::Row, FormatDuration(total, true), c, theme::Text,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        RECT d = { row.right - pad - (int)(140 * s), row.top, row.right - pad, row.bottom };
        theme::Print(dc, Font::Row, L"+" + FormatDuration(total - prev, true), d, theme::TextDim,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    if ((int)w.laps.size() > shown) {
        RECT r = { pad, bandBottom + (int)(shown * kLapH * s), width - pad, bandBottom + (int)((shown + 1) * kLapH * s) };
        theme::Print(dc, Font::Small, L"+" + std::to_wstring((int)w.laps.size() - shown) + L" earlier",
                     r, theme::TextMute, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void PaintInto(HDC dc, const SIZE& size) {
    using theme::Font;
    const float s = g_scale;
    RECT all = { 0, 0, size.cx, size.cy };
    theme::Wash(dc, all, theme::Bg, 255);
    theme::Frame(dc, all, theme::Rule, 255, (std::max)(1, (int)s));

    const int pad = (int)(kPad * s);
    const int headerH = (int)(kHeaderH * s);

    // ---- the two page names ----
    static const wchar_t* kPages[2] = { L"Timer", L"Stopwatch" };
    int x = pad + (int)(8 * s);
    const int track = (int)(2 * s);
    for (int i = 0; i < 2; ++i) {
        const std::wstring name = theme::Caps(kPages[i]);
        const int w = theme::Measure(dc, Font::Tab, name, track);
        RECT r = { x, 0, x + w, headerH };
        theme::Print(dc, Font::Tab, name, r, i == g_page ? theme::TextHi : theme::TextDim,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE, track);
        g_tabRects[i] = { x - (int)(8 * s), 0, x + w + (int)(8 * s), headerH };
        if (i == g_page) {
            RECT u = { x, headerH - (int)(8 * s), x + w, headerH - (int)(8 * s) + (std::max)(2, (int)(2 * s)) };
            theme::Wash(dc, u, theme::TextHi, 255);
        }
        x += w + (int)(32 * s);
    }
    RECT hair = { pad, headerH - 1, size.cx - pad, headerH };
    theme::Wash(dc, hair, theme::Line, 255);

    const int footLines = FootLines();
    const int footTop = size.cy - (int)(footLines * kFootH * s);
    if (g_page == 0) PaintTimers(dc, headerH, size.cx, s);
    else             PaintWatch(dc, headerH, size.cx, s);

    // ---- prompts: every one of these is a key that works, and clicking sends it ----
    g_promptHits.clear();
    RECT line = { pad, footTop, size.cx - pad, footTop + 1 };
    theme::Wash(dc, line, theme::Line, 255);
    const std::vector<PromptText> prompts = Prompts();
    const int gap = (int)(22 * s);
    const int avail = size.cx - 2 * pad;
    size_t i = 0;
    for (int ln = 0; ln < footLines && i < prompts.size(); ++ln) {
        // Greedy: as many as fit on the line, centred.
        size_t j = i;
        int total = 0;
        std::vector<int> widths;
        while (j < prompts.size()) {
            const int w = theme::Prompt(dc, 0, 0, prompts[j].key, prompts[j].word, theme::TextDim, true);
            const int next = total + (widths.empty() ? 0 : gap) + w;
            if (next > avail && !widths.empty()) break;
            widths.push_back(w);
            total = next;
            ++j;
        }
        // Spread evenly over the lines so the second is not a lone word.
        const int cy = footTop + (int)((ln + 0.5f) * kFootH * s);
        int px = (std::max)(pad, (int)(size.cx - total) / 2);
        for (size_t k = i; k < j; ++k) {
            const bool hot = g_hotPrompt == (int)k;
            theme::Prompt(dc, px, cy, prompts[k].key, prompts[k].word, hot ? theme::TextHi : theme::TextDim);
            PromptHit h = { { px, cy - (int)(14 * s), px + widths[k - i], cy + (int)(14 * s) }, prompts[k].vk };
            g_promptHits.push_back(h);
            px += widths[k - i] + gap;
        }
        i = j;
    }
}

void Paint(HWND wnd) {
    PAINTSTRUCT ps;
    HDC target = BeginPaint(wnd, &ps);
    RECT client;
    GetClientRect(wnd, &client);
    const SIZE size = { client.right, client.bottom };
    theme::SetDpi((UINT)(g_scale * 96.0f + 0.5f));

    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(target, size.cx, size.cy) : nullptr;
    if (mem && bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintInto(mem, size);
        BitBlt(target, 0, 0, size.cx, size.cy, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
    } else {
        PaintInto(target, size);
    }
    if (bmp) DeleteObject(bmp);     // nothing stays resident while hidden (inv. 67)
    if (mem) DeleteDC(mem);
    EndPaint(wnd, &ps);
}

int PromptAt(POINT pt) {
    for (size_t i = 0; i < g_promptHits.size(); ++i)
        if (PtInRect(&g_promptHits[i].r, pt)) return (int)i;
    return -1;
}

int RowAt(int y) {
    const int top = (int)(kHeaderH * g_scale);
    if (y < top || g_state.timers.empty()) return -1;
    const int row = (y - top) / (int)(kRowH * g_scale);
    return row < (int)g_state.timers.size() ? row : -1;
}

void ShowContextMenu() {
    enum { kPin = 1, kHide, kSettings };
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kPin, L"Pin in place (click-through)");
    AppendMenuW(menu, MF_STRING, kHide, L"Hide");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kSettings, L"Timer settings...");
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
            if (wp == kTimerAlarm) { StopAlarm(); return 0; }
            if (wp == kTimerAway) {
                KillTimer(wnd, kTimerAway);
                if (!g_awayText.empty()) {
                    AppTrayBalloon(L"Finished while you were away", g_awayText.c_str());
                    g_awayText.clear();
                }
                return 0;
            }
            return 0;

        case WM_TIMECHANGE:       // the user moved the clock: timers move with it
            Repaint();
            Reschedule();
            return 0;

        case WM_DPICHANGED:        // dragged to a monitor with another scale (R2)
            SetScale(HIWORD(wp));
            Relayout();
            return 0;

        case WM_KEYDOWN:
            if (g_alarming) { StopAlarm(); return 0; }       // any key silences it
            if (wp == VK_ESCAPE || wp == VK_TAB || wp == VK_UP || wp == VK_DOWN || wp == VK_LEFT ||
                wp == VK_RIGHT || wp == VK_RETURN || wp == VK_SPACE || wp == VK_BACK ||
                wp == VK_DELETE || (wp >= '0' && wp <= '9') || (wp >= VK_NUMPAD0 && wp <= VK_NUMPAD9) ||
                wp == 'R' || wp == 'N' || wp == 'L')
                HandleKey((UINT)wp);
            return 0;

        case WM_LBUTTONDOWN: {
            // Unpinned, the header carries the panel, except the two page names.
            if (g_alarming || !g_cfg || g_cfg->timerPinned) return 0;
            const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.y >= (int)(kHeaderH * g_scale)) return 0;
            for (const RECT& tab : g_tabRects) if (PtInRect(&tab, pt)) return 0;
            g_dragging = true;
            GetCursorPos(&g_dragOrigin);
            GetWindowRect(wnd, &g_dragStart);
            SetCapture(wnd);
            return 0;
        }

        case WM_CAPTURECHANGED:
            g_dragging = false;
            return 0;

        case WM_RBUTTONUP:
            if (g_alarming) { StopAlarm(); return 0; }
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
            if (g_alarming) { StopAlarm(); return 0; }
            const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const int prompt = PromptAt(pt);
            if (prompt >= 0) { HandleKey(g_promptHits[(size_t)prompt].vk); return 0; }
            if (!g_edit) {
                for (int i = 0; i < 2; ++i)
                    if (PtInRect(&g_tabRects[i], pt)) {
                        if (g_page != i) HandleKey(VK_TAB);
                        return 0;
                    }
                const int row = g_page == 0 ? RowAt(pt.y) : -1;
                if (row >= 0) { g_sel = row; Repaint(); }
            }
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
            const int hot = PromptAt(pt);
            if (hot != g_hotPrompt) { g_hotPrompt = hot; Repaint(); }
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, wnd, 0 };
            TrackMouseEvent(&track);
            return 0;
        }

        case WM_MOUSELEAVE:
            if (g_hotPrompt != -1) { g_hotPrompt = -1; Repaint(); }
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
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"ProWindows timer",
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
    if (pinned && g_edit) EndEdit();         // a pinned panel can take no key to finish with
    if (pinned) g_dragging = false;
    LONG ex = GetWindowLongW(g_wnd, GWL_EXSTYLE);
    const LONG wanted = pinned ? (ex | bits) : (ex & ~bits);
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
    g_edit = false;
    if (g_sel >= (int)g_state.timers.size()) g_sel = (int)g_state.timers.size() - 1;
    if (g_sel < 0) g_sel = 0;
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
    if (g_edit) EndEdit();       // a brand-new timer cancelled by closing is taken back out (R1)
    ShowWindow(g_wnd, SW_HIDE);
    SetTimer(g_wnd, kTimerIdle, kIdleMs, nullptr);
    g_hotPrompt = -1;
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

    // The window stays (hidden, a few hundred bytes) because the one OS timer
    // needs somewhere to land; it has no bitmap and no thread while hidden.
    EnsureWindow();
    Tick(false);      // anything that ended while the PC was off fires, once
}

void TimerShutdown() {
    StopAlarm();
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
    if (g_alarming) StopAlarm();
    if (!g_cfg) return;
    g_cfg->timerShown = !g_cfg->timerShown;
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

void TimerApplyConfig() {
    g_lastTickIdx = -1;
    if (g_wnd) {
        ApplyPin();
        if (Visible()) { Place(); Repaint(); }
    }
    Reschedule();
}

void TimerCheckNow() { if (g_wnd) Tick(false); }

} // namespace awa
