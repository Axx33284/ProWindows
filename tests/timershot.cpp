// ProWindows - screenshots and key-driven checks of the timer panel, without
// the window manager. The panel never touches another window, so this does not
// rearrange the desktop. State lives in a scratch timers.ini, never the real one.
//
//   timershot.exe <output folder>
//
// Build it with tests\timershot.bat. Exit code 1 if a check fails.
#include "../src/timer.h"
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

// timer.cpp reaches the shell through these. The overlay rule is the real one
// without game mode: shown when timerShown says so.
namespace awa {
void AppTrayBalloon(const wchar_t*, const wchar_t*) {}
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

void Shot(HWND wnd, const std::wstring& out, const wchar_t* name, const CLSID& png) {
    Pump(150);
    InvalidateRect(wnd, nullptr, FALSE);
    UpdateWindow(wnd);
    Capture(wnd, out + L"\\" + name, png);
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

    // A timer showing days, one counting down, one paused, one finished; and a
    // stopwatch that has been going for a while with laps.
    const Ticks now = NowUtc();
    const Ticks maxLen = 9999 * 86400 * kSecond + 86399 * kSecond;
    const Ticks twoDays = 2 * 86400 * kSecond + 3 * 3600 * kSecond;
    State seed;
    seed.timers.push_back(Make(L"Timer 1", maxLen, true, maxLen, now));
    seed.timers.push_back(Make(L"Timer 2", 10 * 60 * kSecond, false, 4 * 60 * kSecond + 30 * kSecond, now));
    seed.timers.push_back(Make(L"Timer 3", twoDays, true, twoDays + 4 * 60 * kSecond + 5 * kSecond, now));
    seed.timers.push_back(Make(L"Timer 4", 60 * kSecond, false, 0, now));
    seed.watch.running = true;
    seed.watch.startUtc = now - (3723 * kSecond + 4 * kSecond / 10);
    seed.watch.banked = 12 * kSecond;
    seed.watch.laps = { 3700 * kSecond, 2400 * kSecond + kSecond / 2, 900 * kSecond, 61 * kSecond + kSecond / 3 };
    seed.watch.lapSeq = 4;
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
    Check(cfg.timerShown && wnd && IsWindowVisible(wnd), L"the overlay appears");
    if (!wnd || !IsWindowVisible(wnd)) { TimerShutdown(); return 1; }
    Check(GetForegroundWindow() != wnd, L"showing it does not take the keyboard");
    Check(!(GetWindowLongW(wnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT), L"unpinned: not click-through");

    // ---- the timer page; selection on the 9999-day timer ----
    Shot(wnd, out, L"timer-list.png", png);

    Key(wnd, VK_DOWN);
    Key(wnd, VK_DOWN);                       // Timer 3: 2d 03:04:05, running
    Shot(wnd, out, L"timer-days.png", png);

    // ---- space pauses the running one, and that reaches the file ----
    Key(wnd, VK_SPACE);
    {
        State s = Reload(store);
        Check(s.timers.size() == 4 && !s.timers[2].running && s.timers[2].remaining > 0,
              L"Space pauses the selected timer and saves it");
    }
    Key(wnd, VK_SPACE);
    Check(Reload(store).timers[2].running, L"Space starts it again");

    // ---- the editor on Timer 2 ----
    Key(wnd, VK_UP);
    Key(wnd, VK_RETURN);
    Key(wnd, '1'); Key(wnd, '5');            // minutes field: 15
    Shot(wnd, out, L"timer-editor.png", png);
    Key(wnd, VK_RETURN);
    {
        State s = Reload(store);
        Check(s.timers[1].duration == 15 * 60 * kSecond && !s.timers[1].running,
              L"Enter commits the typed duration (0d 00:15:00)");
    }

    // ---- N makes a timer, Esc in its editor takes it back out ----
    Key(wnd, 'N');
    Check(Reload(store).timers.size() == 4, L"a new timer is not kept until it is set");
    // R1: putting the panel away mid-edit takes the new timer back out.
    TimerSetVisible(false);
    Check(Reload(store).timers.size() == 4 && !IsWindowVisible(wnd),
          L"hiding with an unset new timer drops it");
    TimerSetVisible(true);
    Check(IsWindowVisible(wnd), L"shown again");
    Key(wnd, 'N');
    Key(wnd, VK_ESCAPE);
    Check(IsWindowVisible(wnd) && Reload(store).timers.size() == 4,
          L"Esc in the editor cancels; the panel stays up");
    Key(wnd, 'N');
    Key(wnd, VK_RETURN);
    Check(Reload(store).timers.size() == 5, L"N then Enter keeps the new timer");
    Key(wnd, VK_DELETE);
    Check(Reload(store).timers.size() == 4, L"Del removes the selected timer");

    // ---- the stopwatch page ----
    Key(wnd, VK_TAB);
    Shot(wnd, out, L"timer-stopwatch.png", png);
    Key(wnd, 'L');
    Check(Reload(store).watch.laps.size() == 5, L"L adds a lap");
    Key(wnd, VK_SPACE);
    Check(!Reload(store).watch.running && Reload(store).watch.banked > 3700 * kSecond,
          L"Space pauses the stopwatch and folds the time in");
    Key(wnd, 'R');
    Check(Reload(store).watch.laps.empty() && Reload(store).watch.banked == 0,
          L"R resets the stopwatch");
    Shot(wnd, out, L"timer-stopwatch-reset.png", png);

    // ---- placement: where timerX/Y say, remembered across a hide ----
    Key(wnd, VK_TAB);                        // back to the timers
    cfg.timerX = 120; cfg.timerY = 90;
    TimerApplyConfig();
    {
        RECT r{};
        GetWindowRect(wnd, &r);
        Check(r.left == 120 && r.top == 90, L"the panel sits at timerX/timerY");
    }

    // ---- pinned: click-through, never activated ----
    TimerSetPinned(true);
    Check(cfg.timerPinned, L"pinning is recorded");
    {
        const LONG ex = GetWindowLongW(wnd, GWL_EXSTYLE);
        const LONG want = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
        Check((ex & want) == want, L"pinned: layered, transparent and no-activate");
    }
    Shot(wnd, out, L"timer-pinned.png", png);
    TimerSetPinned(false);
    Check(!(GetWindowLongW(wnd, GWL_EXSTYLE) & (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)),
          L"unpinned: all three taken off again");
    Shot(wnd, out, L"timer-unpinned.png", png);

    Key(wnd, VK_ESCAPE);
    Check(!IsWindowVisible(wnd) && !cfg.timerShown, L"Esc hides the overlay and remembers it");

    TimerShutdown();
    DeleteFileW(store.c_str());
    theme::Shutdown();
    Gdiplus::GdiplusShutdown(token);
    CoUninitialize();
    wprintf(g_failed ? L"\n%d checks failed\n" : L"\nall checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
