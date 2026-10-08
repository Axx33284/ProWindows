// ProWindows - screenshots of the settings window, without the tiler.
//
// The settings window only ever asks the shell for a handful of things, so it
// can be stood up on its own with the rest of `app.h` stubbed out. Nothing
// here starts the window manager, installs a hook or registers a hotkey - the
// point is to look at the UI on a live desktop without every window on it
// being rearranged.
//
// Everything is driven by messages sent to the window - keys, characters,
// clicks - never by the real keyboard or mouse, so the harness can run while
// somebody is using the machine.
//
//   uishot.exe <output folder>
//
// Build it with tests\uishot.bat.
#include "../src/settings.h"
#include "../src/settings_internal.h"
#include "../src/app.h"
#include "../src/theme.h"
#include "../src/monitor.h"
#include "../src/search.h"
#include "../src/resource.h"
#include <commctrl.h>
#include <objidl.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <cstdio>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace awa;

// ---------------------------------------------------------------- app.h stubs
namespace awa {

static Config        g_cfg;
static WindowManager g_wm;
static int           g_applied = 0;

Config&        AppConfig() { return g_cfg; }
WindowManager& AppWm()     { return g_wm; }

bool AppApplySettings()   { ++g_applied; SettingsRefresh(); return true; }
void AppShowShortcuts()   {}
void AppOpenConfigFile()  {}
void AppRetileNow()       {}
void AppUpdateTray()      {}
void AppTrayBalloon(const wchar_t*, const wchar_t*) {}
void AppSaveConfig()      {}
void AppRefreshSettings() {}
void AppOpenMonitorSettings() {}
void AppOpenClockSettings()   {}
void AppUpdateOverlays()    {}
void AppScheduleTrim(UINT)    {}
bool AppAutostartEnabled()    { return false; }
void AppSetAutostart(bool)    {}
void AppGameModeChanged(bool) {}
int  AppHotkeyConflicts()     { return 0; }
int  AppMemoryMB()            { return 38; }
int  AppManagedWindows()      { return 7; }
int  AppIndexEntries()        { return 12400; }
int  AppIconCacheCount()      { return 140; }
int  AppSamplerCostTenths()   { return 12; }
void AppOpenConfigFolder()    {}
void AppWriteDiagnostics()    {}
void AppReloadFromDisk()      {}
void AppRestoreHiddenWindows() {}
bool AppRestoreDefaults()     { return true; }
// Fixed text rather than the real thing: the About rows would otherwise put
// this machine's user name and install path into every capture.
std::wstring AppAboutText() {
    return L"ProWindows 1.5.0  -  running as a normal user\r\n"
           L"C:\\Tools\\ProWindows\\ProWindows.exe\r\n"
           L"C:\\Users\\you\\AppData\\Roaming\\ProWindows\\config.ini";
}

} // namespace awa

namespace {

CLSID g_png = {};
std::wstring g_out;
int g_failures = 0;

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

// The window's own surface, which is what DWM composites.
void Capture(HWND wnd, const std::wstring& name) {
    RECT c{};
    GetClientRect(wnd, &c);
    const int w = c.right, h = c.bottom;
    if (w <= 0 || h <= 0) return;
    HDC own = GetDC(wnd);
    HDC mem = CreateCompatibleDC(own);
    HBITMAP bmp = CreateCompatibleBitmap(own, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (!PrintWindow(wnd, mem, 0x00000002 /* PW_RENDERFULLCONTENT */))
        BitBlt(mem, 0, 0, w, h, own, 0, 0, SRCCOPY);
    ReleaseDC(wnd, own);
    Gdiplus::Bitmap image(bmp, nullptr);
    const std::wstring path = g_out + L"\\" + name;
    image.Save(path.c_str(), &g_png, nullptr);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    wprintf(L"  %s  (%dx%d)\n", name.c_str(), w, h);
}

// Holds (or lets go of) a key as far as GetKeyState on this thread is
// concerned, so a message-driven Ctrl+Tab reads as one.
void Hold(int vk, bool down) {
    BYTE keys[256] = {};
    GetKeyboardState(keys);
    keys[vk] = down ? 0x80 : 0;
    SetKeyboardState(keys);
}

void Key(HWND wnd, UINT vk, int times = 1) {
    for (int i = 0; i < times; ++i) {
        SendMessageW(wnd, WM_KEYDOWN, vk, 1);
        SendMessageW(wnd, WM_KEYUP, vk, 0xC0000001);
        Pump(40);
    }
}

void Type(HWND wnd, const wchar_t* text) {
    for (const wchar_t* p = text; *p; ++p) { SendMessageW(wnd, WM_CHAR, *p, 1); Pump(20); }
}

void Check(bool ok, const wchar_t* what) {
    wprintf(L"  %-60s %s\n", what, ok ? L"ok" : L"FAILED");
    if (!ok) ++g_failures;
}

// The focused row is a brushed-metal bar (PLAN-1.6 2.5); nothing else on the
// screen is that bright. Reads a column of pixels just inside the list's left
// edge, where no label or control is drawn, and returns the length in pixels
// of the longest run with luma >= 30 and how many such runs there are. The
// section plates (luma <= 30 at their very top) and the 1 px hairlines
// (luma 40) are far too short to count as a run.
struct MetalBar { int longest = 0; int runs = 0; int minLumaInRun = 255; };

MetalBar FindMetalBar(HWND wnd, const RECT& client) {
    const int dpi = (int)GetDpiForWindow(wnd);
    const int x = MulDiv(70 + 8, dpi, 96);
    const int y0 = MulDiv(118, dpi, 96), y1 = client.bottom - MulDiv(64, dpi, 96);
    const int minRun = MulDiv(20, dpi, 96);
    MetalBar out;
    HDC dc = GetDC(wnd);
    int run = 0, runMin = 255;
    auto end = [&]() {
        if (run >= minRun) { ++out.runs; if (run > out.longest) { out.longest = run; out.minLumaInRun = runMin; } }
        run = 0; runMin = 255;
    };
    for (int y = y0; y < y1; ++y) {
        const COLORREF c = GetPixel(dc, x, y);
        const int luma = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
        if (luma >= 30) { ++run; runMin = min(runMin, luma); }
        else end();
    }
    end();
    ReleaseDC(wnd, dc);
    return out;
}

// A modal screen takes over the message loop; this timer photographs it and
// then answers it with Esc.
std::wstring g_modalShot;
void CALLBACK ShootModal(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    HWND modal = FindWindowW(L"ProWindowsModal", nullptr);
    if (!modal) { wprintf(L"  (no modal screen appeared)\n"); ++g_failures; return; }
    SetLayeredWindowAttributes(modal, 0, 255, LWA_ALPHA);
    RedrawWindow(modal, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    if (!g_modalShot.empty()) Capture(modal, g_modalShot);
    if (HWND owner = GetWindow(modal, GW_OWNER)) Capture(owner, L"ui-behind-modal.png");
    PostMessageW(modal, WM_KEYDOWN, VK_ESCAPE, 1);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    g_out = (argc > 1) ? argv[1] : L".";

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);
    g_png = PngEncoder();
    theme::Init();

    g_cfg.LoadDefaults();
    g_cfg.searchFiles = false;      // no index walk; not what is being looked at
    // Named folders rather than the defaults, which expand to this machine's
    // profile - the same reason AppAboutText above is fixed text.
    g_cfg.searchFolders = { L"C:\\Users\\you\\Desktop", L"C:\\Users\\you\\Documents",
                            L"C:\\Users\\you\\Downloads", L"D:\\Projects" };
    g_cfg.ignoreProcess = { L"Steam.exe", L"obs64.exe" };
    g_cfg.monitorEnabled = true;
    g_cfg.clockEnabled = true;
    SearchInit(&g_cfg);

    SettingsOpen(GetModuleHandleW(nullptr));
    Pump(600);
    HWND wnd = SettingsWindow();
    if (!wnd) { wprintf(L"the settings window never appeared\n"); return 1; }

    struct Tab { PageIndex page; const wchar_t* name; };
    const Tab tabs[] = {
        { PAGE_LAYOUT,    L"ui-layout.png"    },
        { PAGE_BEHAVIOUR, L"ui-behaviour.png" },
        { PAGE_SHORTCUTS, L"ui-shortcuts.png" },
        { PAGE_APPS,      L"ui-apps.png"      },
        { PAGE_SEARCH,    L"ui-search.png"    },
        { PAGE_MONITOR,   L"ui-monitor.png"   },
        { PAGE_CLOCK,     L"ui-clock.png"     },
        { PAGE_TIMER,     L"ui-timer.png"     },
        { PAGE_EXPLORER,  L"ui-explorer.png"  },
        { PAGE_START,     L"ui-start.png"     },
        { PAGE_GENERAL,   L"ui-general.png"   },
        { PAGE_WELCOME,   L"ui-welcome.png"   },
    };
    static_assert(ARRAYSIZE(tabs) == PAGE_COUNT, "a category was added and this harness stopped covering it");
    for (const Tab& tab : tabs) {
        SettingsOpenTab(tab.page);
        Pump(420);
        Capture(wnd, tab.name);
    }

    // The Welcome category's other pages: '3' steps to the next sub-tab.
    SettingsOpenTab(PAGE_WELCOME);
    Pump(300);
    Key(wnd, VK_ESCAPE);
    Key(wnd, '3');
    Pump(420);
    Capture(wnd, L"ui-welcome-keys.png");
    Key(wnd, '3');
    Pump(420);
    Capture(wnd, L"ui-welcome-overlays.png");

    RECT client{};
    GetClientRect(wnd, &client);

    // ---- the categories, from the keyboard
    SettingsOpenTab(PAGE_LAYOUT);
    Pump(300);
    Key(wnd, VK_ESCAPE);                      // back to the column
    Key(wnd, 'E');                            // Layout -> Behaviour
    Pump(300);
    Capture(wnd, L"ui-nav-focus.png");
    Hold(VK_CONTROL, true);
    Key(wnd, VK_TAB);                         // Behaviour -> General (the tab bar's order)
    Hold(VK_CONTROL, false);
    Pump(300);
    Capture(wnd, L"ui-ctrl-tab.png");

    // ---- a lit row, and a change that is not applied yet
    SettingsOpenTab(PAGE_BEHAVIOUR);
    Pump(300);
    Key(wnd, VK_ESCAPE);                      // the keyboard back on the categories
    Pump(300);
    const MetalBar idle = FindMetalBar(wnd, client);
    Key(wnd, VK_DOWN);                        // into the list: its first row lights
    Key(wnd, VK_DOWN, 1);                     // the second row
    Pump(300);
    const MetalBar lit = FindMetalBar(wnd, client);
    Check(idle.runs == 0, L"no row is a metal bar while the keyboard is on the tabs");
    Check(lit.runs == 1 && lit.longest >= MulDiv(30, (int)GetDpiForWindow(wnd), 96) &&
          lit.minLumaInRun >= 30,
          L"only the focused row is a metal bar (luma >= 30, the rest dark)");
    Key(wnd, VK_RIGHT);                       // Pointer follows the focus -> Off (it is off) ...
    Key(wnd, VK_LEFT);                        // ... -> On
    Pump(300);
    Capture(wnd, L"ui-changed.png");
    Check(!EditedFieldsEqual(Edit(), Saved()), L"a changed toggle is an unapplied edit");

    // ---- Ctrl+S applies it
    Hold(VK_CONTROL, true);
    Key(wnd, 'S');
    Hold(VK_CONTROL, false);
    Pump(300);
    Check(g_applied == 1 && g_cfg.cursorWarp, L"Ctrl+S applies, and the live config has the change");
    Check(EditedFieldsEqual(Edit(), Saved()), L"after Apply nothing is left unapplied");
    Capture(wnd, L"ui-applied.png");

    // ---- the live config changes underneath an unapplied edit, as it does when
    // the monitor is re-skinned from its own right-click menu
    Edit().smartGaps = !Edit().smartGaps;          // the user's change, not applied
    g_cfg.monitorTheme = 3;                         // somebody else's, already live
    SettingsRefresh();
    Check(Edit().smartGaps != g_cfg.smartGaps && Edit().monitorTheme == 3 &&
          !EditedFieldsEqual(Edit(), Saved()),
          L"an outside change arrives without losing an unapplied edit");
    Edit().smartGaps = g_cfg.smartGaps;
    SettingsRefresh();
    Check(EditedFieldsEqual(Edit(), Saved()), L"undoing the edit by hand leaves nothing to apply");

    // ---- a slider, from the keyboard
    SettingsOpenTab(PAGE_LAYOUT);
    Pump(250);
    const int gap = Edit().gapInner;
    Key(wnd, VK_DOWN, 1);                     // Arrangement -> (two Master-only rows skipped) -> gap between
    Key(wnd, VK_RIGHT, 4);
    Pump(300);
    Check(Edit().gapInner == gap + 4, L"Right on a slider moves it one step");
    Capture(wnd, L"ui-slider.png");

    // ---- recording a shortcut
    SettingsOpenTab(PAGE_SHORTCUTS);
    Pump(250);
    Key(wnd, VK_DOWN, 2);                     // modifier, takeover, the first shortcut
    Key(wnd, VK_RETURN);
    Pump(400);
    // What the keyboard hook would post while Win and Shift are held...
    constexpr UINT kCaptureKey = WM_APP + 120;
    SendMessageW(wnd, kCaptureKey, 0, MOD_WIN | MOD_SHIFT);
    Pump(300);
    Capture(wnd, L"ui-capture.png");
    // ... and a chord nobody uses, which is taken at once.
    SendMessageW(wnd, kCaptureKey, VK_F11, MOD_CONTROL | MOD_ALT);
    Pump(300);
    bool bound = false;
    for (const auto& kb : Edit().binds) bound |= (kb.vk == VK_F11 && kb.mods == (MOD_CONTROL | MOD_ALT));
    Check(bound, L"a recorded chord lands on the shortcut");
    Capture(wnd, L"ui-recorded.png");
    // Esc on its own gives up without changing anything.
    Key(wnd, VK_RETURN);
    SendMessageW(wnd, kCaptureKey, VK_ESCAPE, 0);
    Pump(200);

    // ---- search across every category
    Type(wnd, L"/gap");                       // / opens the search
    Pump(350);
    Capture(wnd, L"ui-search-results.png");
    Key(wnd, VK_ESCAPE);
    Pump(200);

    // ---- a modal screen: Reset to defaults asks first
    SettingsOpenTab(PAGE_CLOCK);
    Pump(250);
    g_modalShot = L"ui-modal.png";
    SetTimer(nullptr, 0, 700, ShootModal);
    Key(wnd, 'R');                            // reset this category: asks first, and runs the modal loop until the timer answers it
    Pump(300);

    // ---- the smallest the window goes
    SetWindowPos(wnd, nullptr, 0, 0, 100, 100, SWP_NOMOVE | SWP_NOZORDER);
    SettingsOpenTab(PAGE_MONITOR);
    Pump(450);
    Capture(wnd, L"ui-small.png");

    SettingsDestroy();
    SearchShutdown();
    theme::Shutdown();
    Gdiplus::GdiplusShutdown(token);
    CoUninitialize();
    wprintf(g_failures ? L"%d check(s) FAILED\n" : L"all checks passed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
