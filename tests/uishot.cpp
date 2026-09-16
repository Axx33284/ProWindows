// ProWindows - screenshots of the settings window, without the tiler.
//
// The settings window only ever asks the shell for two things (a layout push
// and a tiling toggle, both from a button), so it can be stood up on its own
// with the rest of `app.h` stubbed out. Nothing here starts the window manager,
// installs a hook or registers a hotkey - the point is to be able to look at
// the UI on a live desktop without every window on it being rearranged.
//
//   uishot.exe <output folder>
//
// Build it with tests\uishot.bat.
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
// Everything the settings window expects the shell to provide. None of it does
// anything: the harness exists to draw the window, not to apply what it says.
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
// Fixed text rather than the real thing: the About block would otherwise put
// this machine's user name and install path into every capture.
std::wstring AppAboutText() {
    return L"ProWindows 1.3.0  -  running as a normal user\r\n"
           L"C:\\Tools\\ProWindows\\ProWindows.exe\r\n"
           L"C:\\Users\\you\\AppData\\Roaming\\ProWindows\\config.ini";
}

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

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring out = (argc > 1) ? argv[1] : L".";

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);
    const CLSID png = PngEncoder();

    INITCOMMONCONTROLSEX icc = { sizeof(icc),
                                 ICC_STANDARD_CLASSES | ICC_BAR_CLASSES |
                                 ICC_UPDOWN_CLASS | ICC_TAB_CLASSES |
                                 ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
    theme::Init();

    g_cfg.LoadDefaults();
    g_cfg.searchFiles = false;      // no index walk; not what is being looked at
    SearchInit(&g_cfg);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    SettingsOpen(inst);
    Pump(700);

    HWND dlg = SettingsWindow();
    if (!dlg) { wprintf(L"the settings window never appeared\n"); return 1; }

    struct Tab { PageIndex page; const wchar_t* name; };
    const Tab tabs[] = {
        { PAGE_LAYOUT,    L"ui-layout.png"    },
        { PAGE_BEHAVIOUR, L"ui-behaviour.png" },
        { PAGE_SHORTCUTS, L"ui-shortcuts.png" },
        { PAGE_APPS,      L"ui-apps.png"      },
        { PAGE_SEARCH,    L"ui-search.png"    },
        { PAGE_MONITOR,   L"ui-monitor.png"   },
        { PAGE_CLOCK,     L"ui-clock.png"     },
        { PAGE_GENERAL,   L"ui-general.png"   },
    };
    static_assert(ARRAYSIZE(tabs) == PAGE_COUNT,
                  "a tab was added and this harness stopped covering it");

    for (const Tab& tab : tabs) {
        SettingsOpenTab(tab.page);
        Pump(350);
        // The shell header and the footer buttons belong to the parent dialog,
        // and switching tabs does not invalidate them - so without this the
        // capture caught whatever they happened to be showing.
        RedrawWindow(dlg, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        Pump(250);
        Capture(dlg, out + L"\\" + tab.name, png);
    }

    SettingsDestroy();
    SearchShutdown();
    theme::Shutdown();
    Gdiplus::GdiplusShutdown(token);
    CoUninitialize();
    return 0;
}
