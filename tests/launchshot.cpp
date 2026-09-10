// ProWindows - screenshots of the search bar, without the window manager.
//
// The launcher is the one piece of UI that can be exercised on its own: it
// never touches another window, so running it does not rearrange the desktop
// the way starting the real application would. This harness stands it up with
// a default config, types a query into it, waits for the shell icons to land,
// and saves what it drew.
//
//   launchshot.exe <output folder> [query ...]
//
// Build it with tests\launchshot.bat.
#include "../src/launcher.h"
#include "../src/theme.h"
#include "../src/search.h"
#include "../src/config.h"
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

// Drains the queue for `ms`, so the app scan, the icon loader and the repaints
// they trigger all get a chance to happen.
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

HWND LauncherWindow() {
    return FindWindowW(L"ProWindows_Launcher", nullptr);
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

    // PW_RENDERFULLCONTENT, so a window drawn with anything the compositor
    // owns comes back complete rather than blank.
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

void Type(HWND wnd, const wchar_t* text) {
    for (const wchar_t* p = text; *p; ++p)
        SendMessageW(wnd, WM_CHAR, (WPARAM)*p, 0);
}

void ClearQuery(HWND wnd) {
    for (int i = 0; i < 200; ++i) SendMessageW(wnd, WM_KEYDOWN, VK_BACK, 0);
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

    static Config cfg;
    cfg.LoadDefaults();
    // The log is where the icon loader reports anything that took a while, and
    // the indexer reports what it found. Both are the point of this run.
    cfg.debug = true;
    LogEnable(true);
    SearchInit(&cfg);
    // SearchInit holds the walk off for twenty seconds so it does not compete
    // with the login rush. Nothing else is running here, so ask for it now.
    SearchReindex();

    LauncherInit(GetModuleHandleW(nullptr), &cfg);

    // The catalogue is a shell enumeration and takes a moment on a machine with
    // a lot of applications installed; the index walk is slower still.
    wprintf(L"waiting for the app catalogue and the file index...\n");
    const ULONGLONG waitFrom = GetTickCount64();
    while (!SearchIndexReady() && GetTickCount64() - waitFrom < 180000) Pump(500);
    wprintf(L"  index: %d entries after %llu ms\n", SearchIndexCount(),
            GetTickCount64() - waitFrom);
    Pump(1500);

    LauncherToggle();
    Pump(400);

    HWND wnd = LauncherWindow();
    if (!wnd) { wprintf(L"the launcher window never appeared\n"); return 1; }

    struct Shot { const wchar_t* name; const wchar_t* query; DWORD settle; };
    const Shot shots[] = {
        { L"launcher-empty.png", L"",          2500 },
        { L"launcher-apps.png",  L"e",         2500 },
        { L"launcher-calc.png",  L"12*(7+3)",  600  },
        { L"launcher-set.png",   L"display",   1500 },
        // An executable that is not an installed application, which is the
        // case the program walk exists for.
        { L"launcher-exe.png",   L"note",      2500 },
    };

    for (const Shot& shot : shots) {
        ClearQuery(wnd);
        if (*shot.query) Type(wnd, shot.query);
        // Long enough for the icon loader to answer for the rows on screen.
        Pump(shot.settle);
        InvalidateRect(wnd, nullptr, FALSE);
        UpdateWindow(wnd);
        Pump(120);
        Capture(wnd, out + L"\\" + shot.name, png);
    }

    // How long the icons actually took, straight out of the log the loader
    // writes. Anything under 40 ms is not reported, so an empty list here means
    // every icon came back immediately - which is what the cache is for.
    wprintf(L"\nslow icon fetches this run:\n");
    {
        FILE* log = nullptr;
        if (_wfopen_s(&log, LogPath().c_str(), L"r, ccs=UTF-8") == 0 && log) {
            wchar_t line[1024];
            int shown = 0;
            while (fgetws(line, 1024, log)) {
                if (wcsstr(line, L"icon") && shown < 24) { wprintf(L"  %s", line); ++shown; }
            }
            if (shown == 0) wprintf(L"  (none over 40 ms)\n");
            fclose(log);
        }
    }

    LauncherHide();
    LauncherShutdown();
    SearchShutdown();
    theme::Shutdown();
    Gdiplus::GdiplusShutdown(token);
    CoUninitialize();
    return 0;
}
