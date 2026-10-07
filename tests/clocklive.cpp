// ProWindows - the desktop clock, live, without the tiler.
//
// Creates the real clock window through clock.cpp with a Config of its own,
// lets it tick, and captures it off the screen - a layered window cannot be
// PrintWindow'd, so this reads the composed desktop under its rectangle.
// One PNG per style, to tests\shots\clock-live-<style>.png.
#include "../src/clock.h"
#include "../src/clocktheme.h"
#include "../src/theme.h"
#include "../src/app.h"
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
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")

using namespace awa;

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
void AppRestoreDefaults()     {}
std::wstring AppAboutText()   { return L""; }
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
    while (GetTickCount64() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

bool CaptureScreen(const RECT& r, const std::wstring& path, const CLSID& png) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    Gdiplus::Bitmap image(bmp, nullptr);
    const bool ok = image.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring out = (argc > 1) ? argv[1] : L".";
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    theme::Init();
    const CLSID png = PngEncoder();

    g_cfg.LoadDefaults();
    g_cfg.clockEnabled = true;
    g_cfg.clockSeconds = true;
    g_cfg.clockX = 80;
    g_cfg.clockY = 80;
    ClockInit(GetModuleHandleW(nullptr), &g_cfg);

    for (int style = 0; style < ClockStyleCount(); ++style) {
        g_cfg.clockStyle = style;
        g_cfg.clockTheme = (style % 2) ? ClockSkinIndexById(L"slayer") : 0;
        ClockApplyConfig();
        Pump(1300);                       // at least one tick lands
        HWND wnd = FindWindowW(L"ProWindows_Clock", nullptr);
        if (!wnd || !IsWindowVisible(wnd)) { wprintf(L"no clock window\n"); return 1; }
        RECT r;
        GetWindowRect(wnd, &r);
        const std::wstring path = out + L"\\clock-live-" + ClockStyleAt(style).id + L".png";
        wprintf(L"  %s  (%ldx%ld at %ld,%ld)\n", path.c_str(),
                r.right - r.left, r.bottom - r.top, r.left, r.top);
        CaptureScreen(r, path, png);
    }

    ClockShutdown();
    theme::Shutdown();
    return 0;
}
