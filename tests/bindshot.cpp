// ProWindows - screenshots of the shortcut recorder, with keys actually pressed.
//
// The recorder's whole difficulty is Win+<key>: the shell takes those before
// any window sees them, so nothing short of pressing the keys proves it works.
// This harness stands the settings window up the way uishot does, opens the
// editor for the first window shortcut, injects real keystrokes with SendInput
// from a second thread, and saves what the dialog showed after each one.
//
//   bindshot.exe <output folder>
//
// Build it with tests\bindshot.bat. The chords it presses are swallowed by the
// recorder's hook, so nothing else on the desktop reacts to them - but only
// while the dialog is in the foreground, so the harness checks that first and
// declines to press anything otherwise.
#include "../src/settings.h"
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

Config&        AppConfig() { return g_cfg; }
WindowManager& AppWm()     { return g_wm; }

void AppApplySettings()   {}
void AppShowShortcuts()   {}
void AppOpenConfigFile()  {}
void AppRetileNow()       {}
void AppUpdateTray()      {}
void AppTrayBalloon(const wchar_t*, const wchar_t*) {}
void AppSaveConfig()      {}
void AppRefreshSettings() {}
void AppOpenMonitorSettings() {}
void AppOpenClockSettings()   {}
void AppScheduleTrim(UINT)    {}
bool AppAutostartEnabled()    { return false; }
void AppSetAutostart(bool)    {}
void AppGameModeChanged(bool) {}
int  AppHotkeyConflicts()     { return 0; }
void AppOpenConfigFolder()    {}
void AppWriteDiagnostics()    {}
void AppReloadFromDisk()      {}
void AppRestoreHiddenWindows() {}
void AppRestoreDefaults(HWND) {}
std::wstring AppAboutText() { return L"ProWindows (bindshot harness)"; }

} // namespace awa

namespace {

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
            HWND dlg = SettingsWindow();
            if (dlg && IsWindow(dlg) && IsDialogMessageW(dlg, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetTickCount64() >= until) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 15, QS_ALLINPUT);
    }
}

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

// The editor is modal, so everything that happens while it is up has to come
// from another thread. This one waits for it, presses, looks, and cancels.
struct Driver {
    std::wstring out;
    CLSID png;
    HWND  dialog = nullptr;
    bool  pressed = false;
};

void Press(WORD vk, bool down) {
    INPUT in = {};
    in.type       = INPUT_KEYBOARD;
    in.ki.wVk     = vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

void Chord(std::initializer_list<WORD> keys) {
    for (WORD k : keys) { Press(k, true);  Sleep(30); }
    for (auto it = keys.end(); it != keys.begin(); ) { --it; Press(*it, false); Sleep(30); }
}

std::wstring BoxText(HWND dialog) {
    wchar_t buf[128] = {};
    GetDlgItemTextW(dialog, IDC_BINDEDIT_CAPTURE, buf, 128);
    return buf;
}

DWORD WINAPI DriveThread(LPVOID param) {
    auto* d = static_cast<Driver*>(param);

    // Wait for the editor to appear.
    for (int i = 0; i < 100 && !d->dialog; ++i) {
        Sleep(50);
        d->dialog = FindWindowW(L"#32770", L"Shortcut");
    }
    if (!d->dialog) { wprintf(L"the shortcut editor never appeared\n"); return 1; }
    Sleep(300);

    SetForegroundWindow(d->dialog);
    Sleep(200);
    if (GetForegroundWindow() != d->dialog) {
        wprintf(L"the editor is not in the foreground; not pressing anything\n");
        PostMessageW(d->dialog, WM_COMMAND, IDCANCEL, 0);
        return 1;
    }

    struct Step { const wchar_t* name; std::initializer_list<WORD> keys; };
    const Step steps[] = {
        { L"bind-win-e.png",         { VK_LWIN, 'E' } },
        { L"bind-win-shift-f.png",   { VK_LWIN, VK_SHIFT, 'F' } },
        { L"bind-ctrl-alt-t.png",    { VK_CONTROL, VK_MENU, 'T' } },
        { L"bind-alt-enter.png",     { VK_MENU, VK_RETURN } },
    };
    for (const Step& s : steps) {
        Chord(s.keys);
        Sleep(250);
        wprintf(L"  box: %s\n", BoxText(d->dialog).c_str());
        Capture(d->dialog, d->out + L"\\" + s.name, d->png);
    }

    // Held modifiers alone must show up while they are down, and go away
    // again when released without a key.
    Press(VK_LWIN, true);
    Press(VK_CONTROL, true);
    Sleep(250);
    wprintf(L"  held: %s\n", BoxText(d->dialog).c_str());
    Capture(d->dialog, d->out + L"\\bind-held.png", d->png);
    Press(VK_CONTROL, false);
    Press(VK_LWIN, false);
    Sleep(250);
    wprintf(L"  released: %s\n", BoxText(d->dialog).c_str());

    d->pressed = true;
    PostMessageW(d->dialog, WM_COMMAND, IDCANCEL, 0);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring out = (argc > 1) ? argv[1] : L".";

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);

    INITCOMMONCONTROLSEX icc = { sizeof(icc),
                                 ICC_STANDARD_CLASSES | ICC_BAR_CLASSES |
                                 ICC_UPDOWN_CLASS | ICC_TAB_CLASSES |
                                 ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
    theme::Init();

    g_cfg.LoadDefaults();
    g_cfg.searchFiles = false;
    SearchInit(&g_cfg);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    SettingsOpen(inst);
    Pump(700);

    HWND dlg = SettingsWindow();
    if (!dlg) { wprintf(L"the settings window never appeared\n"); return 1; }

    SettingsOpenTab(PAGE_SHORTCUTS);
    Pump(350);

    // The page is a child dialog of the settings window; the list is on it.
    HWND list = nullptr;
    for (HWND child = GetWindow(dlg, GW_CHILD); child && !list;
         child = GetWindow(child, GW_HWNDNEXT)) {
        HWND candidate = GetDlgItem(child, IDC_BINDLIST);
        if (candidate) list = candidate;
    }
    if (!list) { wprintf(L"the shortcut list was not found\n"); return 1; }
    HWND page = GetParent(list);

    ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    Pump(100);

    Driver driver;
    driver.out = out;
    driver.png = PngEncoder();
    HANDLE thread = CreateThread(nullptr, 0, DriveThread, &driver, 0, nullptr);

    // Runs the modal editor; returns when the driver cancels it.
    SendMessageW(page, WM_COMMAND, MAKEWPARAM(IDC_BIND_EDIT, BN_CLICKED), 0);

    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);

    SettingsDestroy();
    SearchShutdown();
    theme::Shutdown();
    Gdiplus::GdiplusShutdown(token);
    CoUninitialize();
    return driver.pressed ? 0 : 1;
}
