// ProWindows - the settings window's buttons, pressed with the real mouse.
//
// uishot drives the window with messages, which proves the logic behind a
// button and nothing about whether a click on it ever arrives: hit-testing,
// capture, activation and the modal screens' own loops are all skipped. This
// harness moves the real pointer and clicks, so it takes the mouse for a few
// seconds - do not touch it while it runs. Nothing else is started: the
// window manager, hooks and hotkeys are stubbed out exactly as in uishot.
//
//   clickprobe.exe
//
// Build and run it with tests\clickprobe.bat.
#include "../src/settings.h"
#include "../src/settings_internal.h"
#include "../src/app.h"
#include "../src/theme.h"
#include "../src/search.h"
#include <cstdio>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")

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
std::wstring AppAboutText()   { return L"ProWindows"; }

} // namespace awa

namespace {

int g_failures = 0;

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

void Check(bool ok, const wchar_t* what) {
    wprintf(L"  %-64s %s\n", what, ok ? L"ok" : L"FAILED");
    if (!ok) ++g_failures;
}

// A real click at a point in `wnd`'s client area.
void ClickAt(HWND wnd, int x, int y) {
    POINT pt = { x, y };
    ClientToScreen(wnd, &pt);
    SetCursorPos(pt.x, pt.y);
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(40);
    SendInput(1, &in[1], sizeof(INPUT));
}

void PressKey(WORD vk) {
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = vk; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

int Sc(int px) { return theme::Scale(px); }

// The footer prompt for a key: where the window drew it.
RECT FooterPrompt(HWND wnd, UINT key) {
    theme::SetDpi(DpiForWindow(wnd));
    return SettingsPromptRect(key);
}

void ClickCentre(HWND wnd, const RECT& r) {
    ClickAt(wnd, (r.left + r.right) / 2, (r.top + r.bottom) / 2);
}

HWND Reopen(int page) {
    SettingsOpenTab(page);
    Pump(500);
    HWND wnd = SettingsWindow();
    if (wnd) SetForegroundWindow(wnd);
    Pump(200);
    return wnd;
}

// A modal screen runs its own loop, inside the click that raised it; this
// timer answers it from in there, with the real mouse, on the given button.
int g_modalButton = 0;
bool g_modalSeen = false;
void CALLBACK AnswerModal(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    HWND modal = FindWindowW(L"ProWindowsModal", nullptr);
    g_modalSeen = modal != nullptr;
    if (!modal) return;
    RECT c;
    GetClientRect(modal, &c);
    // The answers are two rows above the footer (modal.cpp's Layout): 12 DIP
    // over a 52 DIP footer, 46 DIP a row.
    const int rowH = Sc(46);
    const int bottom = c.bottom - Sc(52) - Sc(12);
    const int top = bottom - (2 - g_modalButton) * rowH;
    ClickAt(modal, c.right / 2, top + rowH / 2);
}

} // namespace

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    theme::Init();
    g_cfg.LoadDefaults();
    g_cfg.searchFiles = false;
    SearchInit(&g_cfg);

    // ---- the X in the corner
    HWND wnd = Reopen(PAGE_LAYOUT);
    if (!wnd) { wprintf(L"the settings window never appeared\n"); return 1; }
    {
        RECT c; GetClientRect(wnd, &c);
        ClickAt(wnd, c.right - Sc(46), Sc(27));
        Pump(600);
        Check(SettingsWindow() == nullptr, L"the X closes the window");
    }

    // ---- the minimise button
    wnd = Reopen(PAGE_LAYOUT);
    {
        RECT c; GetClientRect(wnd, &c);
        ClickAt(wnd, c.right - Sc(86), Sc(27));
        Pump(600);
        Check(SettingsWindow() && IsIconic(SettingsWindow()), L"the minimise button minimises it");
    }

    // ---- the Esc prompt in the footer: Back from the list, then Close
    wnd = Reopen(PAGE_LAYOUT);
    ClickCentre(wnd, FooterPrompt(wnd, VK_ESCAPE));
    Pump(300);
    ClickCentre(wnd, FooterPrompt(wnd, VK_ESCAPE));
    Pump(600);
    Check(SettingsWindow() == nullptr, L"the Esc prompt in the footer closes the window");

    // ---- Esc, from the categories column, closes
    wnd = Reopen(PAGE_LAYOUT);
    PressKey(VK_ESCAPE);   // the list -> the categories
    Pump(250);
    PressKey(VK_ESCAPE);   // the categories -> closed
    Pump(600);
    Check(SettingsWindow() == nullptr, L"Esc from the categories closes the window");

    // ---- Alt+F4
    wnd = Reopen(PAGE_LAYOUT);
    {
        INPUT in[4] = {};
        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_MENU;
        in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = VK_F4;
        in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = VK_F4;   in[2].ki.dwFlags = KEYEVENTF_KEYUP;
        in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_MENU; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, in, sizeof(INPUT));
        Pump(600);
        Check(SettingsWindow() == nullptr, L"Alt+F4 closes the window");
    }

    // ---- the close and minimise shortcuts, with the settings window in front.
    // It is not a window the tiler arranges, and these used to act on the last
    // arranged window that had the focus instead.
    wnd = Reopen(PAGE_LAYOUT);
    Check(GetForegroundWindow() == wnd, L"(the settings window is in front)");
    g_wm.ActMinimizeFocused();
    Pump(600);
    Check(SettingsWindow() && IsIconic(SettingsWindow()), L"the minimise shortcut minimises the window in front");
    wnd = Reopen(PAGE_LAYOUT);
    g_wm.ActCloseFocused();
    Pump(600);
    Check(SettingsWindow() == nullptr, L"the close shortcut closes the window in front");

    // ---- Reset (the R prompt), confirmed with the mouse
    wnd = Reopen(PAGE_LAYOUT);
    Config d; d.LoadDefaults();
    Edit().gapInner = d.gapInner + 6;
    Edit().gapOuter = d.gapOuter + 6;
    SettingsRefresh();                       // the rows show the change
    Pump(200);
    g_modalButton = 0;
    g_modalSeen = false;
    SetTimer(nullptr, 0, 700, AnswerModal);
    ClickCentre(wnd, FooterPrompt(wnd, 'R'));
    Pump(1600);
    Check(g_modalSeen, L"the Reset prompt asks first");
    Check(Edit().gapInner == d.gapInner && Edit().gapOuter == d.gapOuter,
          L"Reset puts the page back to its defaults");

    // ---- and a close with that unapplied: Discard
    g_modalButton = 1;
    g_modalSeen = false;
    SetTimer(nullptr, 0, 700, AnswerModal);
    PressKey(VK_ESCAPE);                      // the list -> the categories
    Pump(250);
    ClickCentre(wnd, FooterPrompt(wnd, VK_ESCAPE));
    Pump(1600);
    Check(!g_modalSeen || SettingsWindow() == nullptr,
          L"closing with unapplied changes, then Discard, closes the window");

    SettingsDestroy();
    SearchShutdown();
    theme::Shutdown();
    CoUninitialize();
    wprintf(g_failures ? L"%d check(s) FAILED\n" : L"all checks passed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
