// ProWindows - screenshots and click-driven checks of the clock panel, without
// the window manager. The panel never touches another window, so this does not
// rearrange the desktop. State lives in a scratch timers.ini, never the real one.
// Clicks are real window messages sent to the panel at the rectangle it painted
// the button in (TimerHitRect), so a button that is drawn but not clickable fails.
//
//   timershot.exe <output folder>
//
// Build it with tests\timershot.bat. Exit code 1 if a check fails.
#include "../src/timer.h"
#include "../src/clockpanel.h"
#include "../src/theme.h"
#include "../src/config.h"
#include <objidl.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <cstdio>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace awa;
using namespace awa::timer;

static awa::Config* g_cfgp = nullptr;
static std::wstring g_lastBalloon;

// timer.cpp reaches the shell through these. The overlay rule is the real one
// without game mode: shown when timerShown says so.
namespace awa {
void AppTrayBalloon(const wchar_t* title, const wchar_t* text) {
    g_lastBalloon = std::wstring(title) + L": " + text;
}
void AppScheduleTrim(UINT) {}
void AppSaveConfig() {}
void AppRefreshSettings() {}
void AppOpenClockSettings() {}
void AppUpdateOverlays() { if (g_cfgp) TimerSetVisible(g_cfgp->timerShown); }
}

namespace {

int g_failed = 0;
void Check(bool ok, const wchar_t* what) {
    wprintf(L"  %-62s %s\n", what, ok ? L"ok" : L"FAILED");
    if (!ok) ++g_failed;
}

CLSID PngEncoder() {
    UINT count = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    CLSID id = {};
    if (!bytes) return id;
    auto* info = (Gdiplus::ImageCodecInfo*)malloc(bytes);
    if (!info) return id;
    Gdiplus::GetImageEncoders(count, bytes, info);
    for (UINT i = 0; i < count; ++i)
        if (wcscmp(info[i].MimeType, L"image/png") == 0) { id = info[i].Clsid; break; }
    free(info);
    return id;
}

void Pump(DWORD ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetTickCount64() >= until) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 15, QS_ALLINPUT);
    }
}

HWND PanelWindow() { return FindWindowW(L"ProWindows_Timer", nullptr); }

void Capture(HWND wnd, const std::wstring& path, const CLSID& png) {
    RECT r{};
    if (!GetWindowRect(wnd, &r)) return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (!PrintWindow(wnd, mem, 0x00000002 /* PW_RENDERFULLCONTENT */))
        BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);
    Gdiplus::Bitmap image(bmp, nullptr);
    image.Save(path.c_str(), &png, nullptr);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    wprintf(L"  %s  (%dx%d)\n", path.c_str(), w, h);
}

void Key(HWND wnd, UINT vk) {
    SendMessageW(wnd, WM_KEYDOWN, vk, 0);
    Pump(30);
}

// Paints now, without pumping: the panel asks Windows for a WM_MOUSELEAVE the moment
// it tracks the mouse, and with no real pointer over the window that arrives at
// once and would undo a hover the harness has just sent.
void Settle(HWND wnd) {
    InvalidateRect(wnd, nullptr, FALSE);
    UpdateWindow(wnd);
}

void Shot(HWND wnd, const std::wstring& out, const wchar_t* name, const CLSID& png) {
    Pump(150);
    Settle(wnd);
    Capture(wnd, out + L"\\" + name, png);
}

void ShotNow(HWND wnd, const std::wstring& out, const wchar_t* name, const CLSID& png) {
    Settle(wnd);
    Capture(wnd, out + L"\\" + name, png);
}

// A click on the button the panel painted for (cmd, arg): move, press, release.
bool Click(HWND wnd, int cmd, int arg) {
    Settle(wnd);
    RECT r;
    if (!TimerHitRect(cmd, arg, &r)) {
        wprintf(L"  (no button %d/%d on screen)\n", cmd, arg);
        return false;
    }
    const LPARAM at = MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    SendMessageW(wnd, WM_MOUSEMOVE, 0, at);
    SendMessageW(wnd, WM_LBUTTONDOWN, MK_LBUTTON, at);
    SendMessageW(wnd, WM_LBUTTONUP, 0, at);
    Settle(wnd);
    return true;
}

// The wheel at a client point (a spot in the editor body with no control under it).
void Wheel(HWND wnd, int x, int y, int notches) {
    POINT pt = { x, y };
    ClientToScreen(wnd, &pt);
    SendMessageW(wnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)(notches * WHEEL_DELTA)), MAKELPARAM(pt.x, pt.y));
    Settle(wnd);
}

// The pointer over a button, so the things that show on hover appear.
void Hover(HWND wnd, int cmd, int arg) {
    Settle(wnd);
    RECT r;
    if (!TimerHitRect(cmd, arg, &r)) return;
    SendMessageW(wnd, WM_MOUSEMOVE, 0, MAKELPARAM((r.left + r.right) / 2, r.top + (r.bottom - r.top) / 3));
    Settle(wnd);
}

State Reload(const std::wstring& path) {
    State s;
    LoadFile(path, &s);
    return s;
}

Timer Make(const wchar_t* label, Ticks duration, bool running, Ticks left, Ticks now) {
    Timer t;
    t.label = label;
    t.duration = duration;
    t.running = running;
    if (running) t.endUtc = now + left; else t.remaining = left;
    return t;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring out = (argc > 1) ? argv[1] : L".";

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);
    const CLSID png = PngEncoder();
    theme::Init();

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring store = std::wstring(tmp) + L"pw_timershot.ini";

    // Timers: one counting down, one paused, one with days, one finished, one at
    // the 9999-day limit; a stopwatch that has been going for a while with laps;
    // five alarms of different kinds.
    const Ticks now = NowUtc();
    const Ticks maxLen = 9999 * 86400 * kSecond + 86399 * kSecond;
    const Ticks twoDays = 2 * 86400 * kSecond + 3 * 3600 * kSecond;
    State seed;
    seed.timers.push_back(Make(L"Pasta", 10 * 60 * kSecond, true, 6 * 60 * kSecond + 12 * kSecond, now));
    seed.timers.push_back(Make(L"Laundry", 45 * 60 * kSecond, false, 31 * 60 * kSecond + 30 * kSecond, now));
    seed.timers.push_back(Make(L"Long haul", twoDays, true, twoDays + 4 * 60 * kSecond + 5 * kSecond, now));
    seed.timers.push_back(Make(L"Eggs", 60 * kSecond, false, 0, now));
    seed.timers.push_back(Make(L"Max", maxLen, true, maxLen, now));
    seed.watch.running = true;
    seed.watch.startUtc = now - (3723 * kSecond + 4 * kSecond / 10);
    seed.watch.banked = 12 * kSecond;
    seed.watch.laps = { 3700 * kSecond, 2400 * kSecond + kSecond / 2, 900 * kSecond, 61 * kSecond + kSecond / 3 };
    seed.watch.lapSeq = 4;
    {
        alarm::Alarm a;
        a.label = L"Wake up";
        a.kind = alarm::Kind::Weekly; a.hour = 7; a.minute = 0;
        a.weekdays = 0x1F;                                       // Mon-Fri
        alarm::Arm(a, now); seed.alarms.push_back(a);
        alarm::Alarm b;
        b.label = L"Stand up and stretch";
        b.kind = alarm::Kind::Hourly; b.minute = 15; b.hourFrom = 9; b.hourTo = 17;
        alarm::Arm(b, now); seed.alarms.push_back(b);
        alarm::Alarm c;
        c.label = L"Pay the rent";
        c.kind = alarm::Kind::Monthly; c.hour = 9; c.minute = 30;
        c.monthDays = 1u | alarm::kLastDayBit; c.months = (1u << 0) | (1u << 5);
        alarm::Arm(c, now); seed.alarms.push_back(c);
        alarm::Alarm d;
        d.label = L"Dentist";
        d.kind = alarm::Kind::Once; d.hour = 9; d.minute = 30;
        const alarm::LocalTime t = alarm::SystemZone().ToLocal(now + 2 * 86400 * kSecond);
        d.year = t.year; d.month = t.month; d.day = t.day;
        alarm::Arm(d, now); seed.alarms.push_back(d);
        alarm::Alarm e;
        e.label = L"Gym";
        e.kind = alarm::Kind::Daily; e.hour = 18; e.minute = 0; e.enabled = false;
        alarm::Arm(e, now); seed.alarms.push_back(e);
    }
    Check(SaveFile(store, seed), L"scratch timers.ini written");

    static Config cfg;
    cfg.LoadDefaults();
    g_cfgp = &cfg;
    TimerSetStorePath(store);
    TimerInit(GetModuleHandleW(nullptr), &cfg);

    Check(!cfg.timerShown && !cfg.timerPinned && cfg.timerX == INT_MIN, L"defaults: hidden, unpinned, unplaced");
    TimerToggle();
    Pump(300);
    HWND wnd = PanelWindow();
    Check(cfg.timerShown && wnd && IsWindowVisible(wnd), L"Win+W (the toggle) shows the panel");
    if (!wnd || !IsWindowVisible(wnd)) { TimerShutdown(); return 1; }
    Check(GetForegroundWindow() != wnd, L"showing it does not take the keyboard");
    Check(!(GetWindowLongW(wnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT), L"unpinned: not click-through");
    {
        RECT r{};
        GetWindowRect(wnd, &r);
        Check(r.right - r.left == panel::kWidth && r.bottom - r.top == panel::kHeight, L"880 x 560 at 100 %");
    }

    // ---- the Clock page, and the rail ----
    Shot(wnd, out, L"clock-page.png", png);
    Check(Reload(store).page == 0, L"opens on the Clock page");
    Check(Click(wnd, panel::CmdPage, panel::PageAlarm), L"click Alarm in the rail");
    Check(Reload(store).page == panel::PageAlarm, L"the page is remembered in timers.ini");

    // ---- the Alarm page ----
    Shot(wnd, out, L"alarm-list.png", png);
    Check(Click(wnd, panel::CmdAlarmToggle, 4), L"switch Gym on");
    {
        State s = Reload(store);
        Check(s.alarms.size() == 5 && s.alarms[4].enabled && s.alarms[4].lastFiredUtc > 0,
              L"enabling re-arms from now (baseline set)");
    }
    Check(Click(wnd, panel::CmdAlarmToggle, 4), L"switch Gym off");
    Check(!Reload(store).alarms[4].enabled, L"and off again");

    // ---- the alarm editor, once per repeat kind ----
    Check(Click(wnd, panel::CmdAlarmEdit, 0), L"click the Wake up card");
    Shot(wnd, out, L"alarm-edit-weekly.png", png);
    Check(Click(wnd, panel::CmdKind, 1), L"Hourly");
    Wheel(wnd, 224, 300, -1);
    Shot(wnd, out, L"alarm-edit-hourly.png", png);
    Check(Click(wnd, panel::CmdKind, 2), L"Daily");
    Check(Click(wnd, panel::CmdLimitOpen, 0), L"open Limit to");
    Wheel(wnd, 224, 300, -4);
    Check(Click(wnd, panel::CmdMonth, 5), L"limit to June");
    Wheel(wnd, 224, 300, -8);
    Check(Click(wnd, panel::CmdUntil, 0), L"set an end date");
    Shot(wnd, out, L"alarm-edit-daily-limits-a.png", png);
    Wheel(wnd, 224, 300, -8);
    Shot(wnd, out, L"alarm-edit-daily-limits-b.png", png);
    Wheel(wnd, 224, 300, 20);
    Check(Click(wnd, panel::CmdKind, 4), L"Monthly");
    Wheel(wnd, 224, 300, -3);
    Shot(wnd, out, L"alarm-edit-monthly.png", png);
    Check(Click(wnd, panel::CmdKind, 0), L"Once");
    Wheel(wnd, 224, 300, -2);
    Shot(wnd, out, L"alarm-edit-once.png", png);
    Check(Click(wnd, panel::CmdKind, 3), L"Weekly (days kept: none, Daily cleared them)");
    Shot(wnd, out, L"alarm-edit-weekly-empty.png", png);
    // Weekly with no day selected cannot be saved; the editor opened the Daily
    // limit chips off, so switching to Weekly picks today.
    Check(Click(wnd, panel::CmdSave, 0), L"Save");
    {
        State s = Reload(store);
        Check(s.alarms[0].kind == alarm::Kind::Weekly && s.alarms[0].weekdays != 0 &&
              s.alarms[0].lastFiredUtc >= now, L"saved as Weekly with at least one day, re-armed");
    }

    // Name and time: type, then Save.
    Check(Click(wnd, panel::CmdAlarmEdit, 0), L"edit it again");
    Check(Click(wnd, panel::CmdName, 0), L"focus the name");
    for (wchar_t ch : std::wstring(L"Morning run")) SendMessageW(wnd, WM_CHAR, ch, 0);
    Check(Click(wnd, panel::CmdSpinUp, panel::FieldMinute), L"minute up");
    Check(Click(wnd, panel::CmdSpinFocus, panel::FieldHour), L"focus the hour");
    Key(wnd, '0'); Key(wnd, '6');
    Shot(wnd, out, L"alarm-edit-typed.png", png);
    for (int d = 0; d < 7; ++d) Click(wnd, panel::CmdWeekday, d);      // flip all seven
    Check(Click(wnd, panel::CmdSave, 0), L"Save");
    {
        State s = Reload(store);
        Check(s.alarms[0].label == L"Morning run", L"the name was typed in");
        Check(s.alarms[0].minute == 1, L"minute 01");
        Check(s.alarms[0].hour == 6 || s.alarms[0].hour == 18, L"hour 06 typed");
    }

    // a new alarm: the default is Once at the next whole hour
    Check(Click(wnd, panel::CmdAlarmAdd, 0), L"the + button");
    Shot(wnd, out, L"alarm-new.png", png);
    Key(wnd, VK_ESCAPE);
    Check(Reload(store).alarms.size() == 5 && IsWindowVisible(wnd), L"Esc cancels the editor, the panel stays");
    Click(wnd, panel::CmdAlarmAdd, 0);
    Key(wnd, VK_RETURN);
    {
        State s = Reload(store);
        Check(s.alarms.size() == 6 && s.alarms[5].kind == alarm::Kind::Once && s.alarms[5].minute == 0 && s.alarms[5].enabled,
              L"Enter saves the new alarm: Once, on the hour");
        Check(s.alarms.size() == 6 && alarm::NextFire(s.alarms[5], NowUtc(), alarm::SystemZone()) > NowUtc(),
              L"and it rings in the future");
    }
    Wheel(wnd, 400, 300, -10);
    Hover(wnd, panel::CmdAlarmEdit, 5);
    ShotNow(wnd, out, L"alarm-hover.png", png);
    Click(wnd, panel::CmdAlarmDelete, 5);
    Check(Reload(store).alarms.size() == 5, L"the bin deletes an alarm");

    // ---- the ringing banner ----
    g_lastBalloon.clear();
    TimerRingAlarm(0);
    Pump(200);
    Check(TimerAlarmRinging() && TimerAlarming(), L"ringing: banner and sound");
    Check(g_lastBalloon.find(L"Alarm:") == 0, L"a tray balloon names the alarm");
    Shot(wnd, out, L"alarm-ringing.png", png);
    Check(Click(wnd, panel::CmdRingSnooze, 0), L"Snooze");
    {
        State s = Reload(store);
        Check(!TimerAlarmRinging() && !TimerAlarming() && s.alarms[0].snoozeUntilUtc > NowUtc(),
              L"snoozed: silent, rings again later");
    }
    TimerRingAlarm(1);
    Pump(100);
    Check(Click(wnd, panel::CmdRingDismiss, 0) && !TimerAlarmRinging(), L"Dismiss");

    // ---- the Timer page ----
    Click(wnd, panel::CmdPage, panel::PageTimer);
    Pump(100);
    {
        RECT r;
        if (TimerHitRect(panel::CmdTimerSel, 1, &r))
            SendMessageW(wnd, WM_MOUSEMOVE, 0, MAKELPARAM((r.left + r.right) / 2, r.top + 30));
    }
    ShotNow(wnd, out, L"timer-grid.png", png);
    Check(Click(wnd, panel::CmdTimerPlay, 0), L"pause Pasta");
    Check(!Reload(store).timers[0].running && Reload(store).timers[0].remaining > 0, L"it paused and saved");
    Check(Click(wnd, panel::CmdTimerPlay, 0) && Reload(store).timers[0].running, L"and runs again");
    Check(Click(wnd, panel::CmdQuick, 3), L"quick start 3 min");
    {
        State s = Reload(store);
        Check(s.timers.size() == 6 && s.timers[5].running && s.timers[5].duration == 3 * 60 * kSecond,
              L"a new started 3 min timer");
    }
    Wheel(wnd, 400, 400, -10);
    Hover(wnd, panel::CmdTimerSel, 5);
    Check(Click(wnd, panel::CmdTimerDelete, 5) && Reload(store).timers.size() == 5, L"delete it again");
    Wheel(wnd, 400, 400, 10);
    Hover(wnd, panel::CmdTimerSel, 1);
    Check(Click(wnd, panel::CmdTimerEdit, 1), L"edit Laundry");
    Shot(wnd, out, L"timer-edit.png", png);
    Click(wnd, panel::CmdSpinFocus, 1);
    Key(wnd, '2'); Key(wnd, '3');
    Key(wnd, VK_RETURN);
    Check(Reload(store).timers[1].duration == (23 * 3600 + 45 * 60) * kSecond,
          L"hours 23 typed, minutes kept: 23:45:00");
    Click(wnd, panel::CmdTimerAdd, 0);
    Key(wnd, VK_ESCAPE);
    Check(Reload(store).timers.size() == 5, L"cancelling a new timer adds nothing");
    Click(wnd, panel::CmdTimerAdd, 0);
    Key(wnd, VK_RETURN);
    Check(Reload(store).timers.size() == 6, L"Enter in a new timer's editor keeps it");
    Wheel(wnd, 400, 400, -10);
    Hover(wnd, panel::CmdTimerSel, 5);
    Click(wnd, panel::CmdTimerDelete, 5);
    // the wheel changes a number under the pointer
    Wheel(wnd, 400, 400, 10);
    Hover(wnd, panel::CmdTimerSel, 1);
    Click(wnd, panel::CmdTimerEdit, 1);
    {
        RECT r;
        if (TimerHitRect(panel::CmdSpinFocus, 2, &r)) {
            POINT pt = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
            ClientToScreen(wnd, &pt);
            SendMessageW(wnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(pt.x, pt.y));
            SendMessageW(wnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(pt.x, pt.y));
        }
        Key(wnd, VK_RETURN);
        Check(Reload(store).timers[1].duration == (23 * 3600 + 47 * 60) * kSecond, L"the wheel over minutes adds two: 23:47:00");
    }

    // ---- the stopwatch ----
    Key(wnd, VK_TAB);
    Check(Reload(store).page == panel::PageWatch, L"Tab goes to the next page (Stopwatch)");
    Shot(wnd, out, L"stopwatch.png", png);
    Click(wnd, panel::CmdWatchLap, 0);
    Check(Reload(store).watch.laps.size() == 5, L"Lap adds a lap");
    Click(wnd, panel::CmdWatchPlay, 0);
    Check(!Reload(store).watch.running && Reload(store).watch.banked > 3700 * kSecond,
          L"Start/Pause pauses and folds the time in");
    Click(wnd, panel::CmdWatchReset, 0);
    Check(Reload(store).watch.laps.empty() && Reload(store).watch.banked == 0, L"Reset clears it");
    Shot(wnd, out, L"stopwatch-reset.png", png);

    // ---- placement, pin ----
    Key(wnd, VK_TAB);                        // wraps round to the Clock
    Check(Reload(store).page == panel::PageClock, L"Tab wraps to the Clock");
    cfg.timerX = 120; cfg.timerY = 90;
    TimerApplyConfig();
    {
        RECT r{};
        GetWindowRect(wnd, &r);
        Check(r.left == 120 && r.top == 90, L"the panel sits at timerX/timerY");
    }
    Click(wnd, panel::CmdPage, panel::PageTimer);
    Click(wnd, panel::CmdPin, 0);
    Check(cfg.timerPinned, L"the pin button pins");
    {
        const LONG ex = GetWindowLongW(wnd, GWL_EXSTYLE);
        const LONG want = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
        Check((ex & want) == want, L"pinned: layered, transparent and no-activate");
    }
    Shot(wnd, out, L"pinned.png", png);
    TimerSetPinned(false);
    Check(!(GetWindowLongW(wnd, GWL_EXSTYLE) & (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)),
          L"unpinned: all three taken off again");
    Shot(wnd, out, L"unpinned.png", png);

    Click(wnd, panel::CmdPage, panel::PageClock);
    Shot(wnd, out, L"clock-page-2.png", png);

    Click(wnd, panel::CmdClose, 0);
    Check(!IsWindowVisible(wnd) && !cfg.timerShown, L"the x button hides the overlay and remembers it");
    TimerToggle();
    Pump(100);
    Check(IsWindowVisible(wnd) && Reload(store).page == panel::PageClock, L"Win+W again: shown, on the Clock page");
    Key(wnd, VK_ESCAPE);
    Check(!IsWindowVisible(wnd), L"Esc hides it");

    TimerShutdown();
    DeleteFileW(store.c_str());
    theme::Shutdown();
    Gdiplus::GdiplusShutdown(token);
    CoUninitialize();
    wprintf(g_failed ? L"\n%d checks failed\n" : L"\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
