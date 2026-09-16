// ProWindows - the Monitor tab's row drag, with the mouse actually moved.
//
// The eight readout rows on the Monitor tab can be dragged into a new order.
// A subclassed button that has to tell a click from a drag is exactly the kind
// of thing that works in the debugger and not with a mouse, so this harness
// stands the settings window up the way uishot does, presses on the first row
// with SendInput, drags it down three rows, lets go, and prints the rows
// before and after - and once more with a press that does not move, which
// must still toggle the tick.
//
//   rowdragshot.exe <output folder>
//
// Build it with tests\rowdragshot.bat.
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
bool AppAutostartEnabled()    { return false; }
void AppSetAutostart(bool)    {}
void AppGameModeChanged(bool) {}
int  AppHotkeyConflicts()     { return 0; }
void AppOpenConfigFolder()    {}
void AppWriteDiagnostics()    {}
void AppReloadFromDisk()      {}
void AppRestoreHiddenWindows() {}
void AppRestoreDefaults(HWND) {}
std::wstring AppAboutText() { return L"ProWindows (rowdragshot harness)"; }

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

void Press(bool down) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
}

std::wstring RowText(HWND page, int slot) {
    wchar_t buf[64] = {};
    GetDlgItemTextW(page, IDC_MON_SHOW_FIRST + slot, buf, 64);
    return buf;
}

void PrintRows(HWND page, const wchar_t* when) {
    wprintf(L"  %s:", when);
    for (int slot = 0; slot < MON_METRIC_COUNT; ++slot)
        wprintf(L" %s%s", slot ? L"| " : L"", RowText(page, slot).c_str());
    wprintf(L"\n");
}

// Runs on a thread of its own so the page can pump while the mouse moves.
struct Drive { HWND page; bool done = false; bool ok = false; };

DWORD WINAPI DriveThread(LPVOID param) {
    auto* d = static_cast<Drive*>(param);
    HWND row0 = GetDlgItem(d->page, IDC_MON_SHOW_FIRST + 0);
    HWND row3 = GetDlgItem(d->page, IDC_MON_SHOW_FIRST + 3);
    RECT a{}, b{};
    GetWindowRect(row0, &a);
    GetWindowRect(row3, &b);

    // On the label text, a little in from the tick box.
    const int x  = a.left + 40;
    const int y0 = (a.top + a.bottom) / 2;
    const int y3 = (b.top + b.bottom) / 2;

    SetCursorPos(x, y0);
    Sleep(120);
    Press(true);
    for (int i = 1; i <= 12; ++i) {
        SetCursorPos(x, y0 + (y3 - y0) * i / 12);
        Sleep(25);
    }
    Sleep(120);
    Press(false);
    Sleep(250);
    d->ok = true;

    // And a plain click on what is now row 1, which must toggle its tick and
    // not move anything.
    HWND row1 = GetDlgItem(d->page, IDC_MON_SHOW_FIRST + 1);
    RECT c{};
    GetWindowRect(row1, &c);
    SetCursorPos(c.left + 6, (c.top + c.bottom) / 2);
    Sleep(120);
    Press(true);
    Sleep(60);
    Press(false);
    Sleep(250);
    d->done = true;
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
    g_cfg.monitorEnabled = true;    // or the rows are greyed out and ignore the mouse
    SearchInit(&g_cfg);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    SettingsOpen(inst);
    Pump(700);

    HWND dlg = SettingsWindow();
    if (!dlg) { wprintf(L"the settings window never appeared\n"); return 1; }
    SettingsOpenTab(PAGE_MONITOR);
    Pump(400);

    HWND page = nullptr;
    for (HWND child = GetWindow(dlg, GW_CHILD); child && !page;
         child = GetWindow(child, GW_HWNDNEXT))
        if (GetDlgItem(child, IDC_MON_SHOW_FIRST)) page = child;
    if (!page) { wprintf(L"the monitor page was not found\n"); return 1; }

    SetForegroundWindow(dlg);
    // The real ProWindows, if it is running, tiles this window the moment it
    // appears and animates it there; the rows have to be measured after that
    // has finished, not while they are still on their way.
    Pump(1200);
    if (GetForegroundWindow() != dlg) {
        wprintf(L"the settings window is not in the foreground; not moving the mouse\n");
        return 1;
    }

    const bool tickBefore = IsDlgButtonChecked(page, IDC_MON_SHOW_FIRST + 0) == BST_CHECKED;
    PrintRows(page, L"before");

    Drive drive{ page };
    HANDLE thread = CreateThread(nullptr, 0, DriveThread, &drive, 0, nullptr);
    while (!drive.done) Pump(50);
    WaitForSingleObject(thread, 2000);
    CloseHandle(thread);

    PrintRows(page, L"after ");
    // The first row's metric went to row 3; the plain click then toggled row
    // 1's tick.
    const bool moved = RowText(page, 3) == L"CPU";
    const bool tickAfter = IsDlgButtonChecked(page, IDC_MON_SHOW_FIRST + 1) == BST_CHECKED;
    wprintf(L"  CPU landed in row 3: %s\n", moved ? L"yes" : L"NO");
    wprintf(L"  plain click toggled a tick: %s\n",
            (tickAfter != tickBefore) ? L"yes" : L"NO");

    RedrawWindow(dlg, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    Pump(200);
    Capture(dlg, out + L"\\rowdrag-after.png", PngEncoder());

    SettingsDestroy();
    SearchShutdown();
    theme::Shutdown();
    Gdiplus::GdiplusShutdown(token);
    CoUninitialize();
    return (moved && tickAfter != tickBefore) ? 0 : 1;
}
