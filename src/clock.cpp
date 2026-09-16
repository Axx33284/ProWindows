#include "clock.h"
#include "clockpaint.h"
#include "clocktheme.h"
#include "monpaint.h"
#include "app.h"
#include "winutil.h"
#include <objidl.h>
// GDI+ headers use bare min/max, which NOMINMAX removes. Feed them the
// std:: versions rather than re-enabling the Windows macros.
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>

namespace awa {

namespace {

constexpr wchar_t kClass[]     = L"ProWindows_Clock";
constexpr UINT_PTR kTimerTick  = 1;    // the next second (or minute) has arrived

enum : UINT {
    IDM_PIN = 1, IDM_DESKTOP, IDM_24H, IDM_SECONDS, IDM_DATE, IDM_WEEKDAY,
    IDM_SETTINGS, IDM_HIDE,
    IDM_STYLE_BASE = 100,       // + style index
    IDM_THEME_BASE = 200,       // + skin index
    IDM_SNAP_BASE  = 500,       // + one of the nine snap points
};

HINSTANCE g_inst = nullptr;
Config*   g_cfg  = nullptr;
HWND      g_wnd  = nullptr;
bool      g_wantVisible = false;

bool  g_dragging = false;
POINT g_dragOrigin = {};
RECT  g_dragStart  = {};

// What the last frame put on screen, so a tick that changes nothing visible
// - fifty-nine of every sixty with the seconds off - is not painted.
std::wstring g_lastSig;
bool         g_sigValid = false;

// The layered-window surface, kept across frames.
HDC     g_surfaceDc   = nullptr;
HBITMAP g_surfaceBmp  = nullptr;
HGDIOBJ g_surfaceOld  = nullptr;
void*   g_surfaceBits = nullptr;
SIZE    g_surfaceSize = { 0, 0 };

void InvalidatePanel() { g_sigValid = false; }

const ClockSkin& Skin() {
    return ClockSkinAt(g_cfg ? g_cfg->clockTheme : 0);
}

float Scale() {
    float scale = (g_cfg ? g_cfg->clockScale : 100) / 100.0f;
    if (g_wnd) {
        const UINT dpi = DpiForWindow(g_wnd);
        if (dpi > 0) scale *= (float)dpi / 96.0f;
    }
    return scale;
}

ClockPaintCtx LiveCtx() {
    ClockPaintCtx ctx;
    ctx.scale   = Scale();
    ctx.alpha   = (BYTE)(255 * (g_cfg ? g_cfg->clockOpacity : 92) / 100);
    ctx.style   = g_cfg ? g_cfg->clockStyle : CLOCK_STYLE_DIGITAL;
    ctx.skin    = &Skin();
    ctx.hours24 = g_cfg && g_cfg->clockHours24;
    ctx.seconds = g_cfg && g_cfg->clockSeconds;
    ctx.date    = !g_cfg || g_cfg->clockDate;
    ctx.weekday = !g_cfg || g_cfg->clockWeekday;
    GetLocalTime(&ctx.time);
    return ctx;
}

// Everything a frame depends on, as text. Cheap to build, cheap to compare,
// and it changes exactly when the picture would.
std::wstring FrameSignature(const ClockPaintCtx& ctx, const ClockText& t, const SIZE& size) {
    std::wstring sig;
    sig.reserve(160);
    sig += std::to_wstring(size.cx) + L"x" + std::to_wstring(size.cy) + L"|";
    sig += std::to_wstring(ctx.style) + L"|" + std::to_wstring((int)ctx.alpha) + L"|";
    sig += std::to_wstring((int)(ctx.scale * 100)) + L"|";
    sig += std::to_wstring((uintptr_t)ctx.skin) + L"|";
    sig += t.hours + L":" + t.minutes + L"|" + t.ampm + L"|" + t.dateLong + L"|";
    if (ClockStyleShowsSeconds(ctx.style, ctx.seconds)) sig += t.seconds;
    sig += ctx.date ? L"d" : L"-";
    sig += ctx.weekday ? L"w" : L"-";
    return sig;
}

void ReleaseSurface() {
    if (g_surfaceDc) {
        if (g_surfaceBmp) SelectObject(g_surfaceDc, g_surfaceOld);
        DeleteDC(g_surfaceDc);
        g_surfaceDc = nullptr;
    }
    if (g_surfaceBmp) { DeleteObject(g_surfaceBmp); g_surfaceBmp = nullptr; }
    g_surfaceBits = nullptr;
    g_surfaceOld  = nullptr;
    g_surfaceSize = SIZE{ 0, 0 };
    g_sigValid    = false;
}

void ClampOnScreen(int* x, int* y, const SIZE& size) {
    POINT probe = { *x + size.cx / 2, *y + size.cy / 2 };
    HMONITOR mon = MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(mon, &mi)) return;
    const int maxX = (int)mi.rcWork.right - (int)size.cx;
    const int maxY = (int)mi.rcWork.bottom - (int)size.cy;
    *x = (std::max)((int)mi.rcWork.left, (std::min)(*x, maxX));
    *y = (std::max)((int)mi.rcWork.top,  (std::min)(*y, maxY));
}

// Paints one frame into the layered window, unless nothing would change.
// The panel's size follows the text: "9:59" is narrower than "10:00", and
// the window is resized in the same UpdateLayeredWindow that paints it, with
// the panel kept anchored to whichever edge it is nearer.
void Redraw() {
    if (!g_wnd || !g_cfg) return;

    const ClockPaintCtx ctx = LiveCtx();
    const ClockText text = ClockBuildText(ctx);
    SIZE size = ClockMeasure(ctx);

    const std::wstring sig = FrameSignature(ctx, text, size);
    if (g_sigValid && sig == g_lastSig) return;
    g_lastSig  = sig;
    g_sigValid = true;

    HDC screen = GetDC(nullptr);
    if (!g_surfaceDc) g_surfaceDc = CreateCompatibleDC(screen);
    if (g_surfaceDc && (!g_surfaceBmp || g_surfaceSize.cx != size.cx ||
                        g_surfaceSize.cy != size.cy)) {
        if (g_surfaceBmp) {
            SelectObject(g_surfaceDc, g_surfaceOld);
            DeleteObject(g_surfaceBmp);
            g_surfaceBmp = nullptr;
        }
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = size.cx;
        bi.bmiHeader.biHeight      = -size.cy;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g_surfaceBits = nullptr;
        g_surfaceBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_surfaceBits, nullptr, 0);
        if (g_surfaceBmp) {
            g_surfaceOld  = SelectObject(g_surfaceDc, g_surfaceBmp);
            g_surfaceSize = size;
        }
    }
    if (!g_surfaceDc || !g_surfaceBmp || !g_surfaceBits) { ReleaseDC(nullptr, screen); return; }

    {
        Gdiplus::Bitmap surface(size.cx, size.cy, size.cx * 4,
                                PixelFormat32bppPARGB, (BYTE*)g_surfaceBits);
        Gdiplus::Graphics g(&surface);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        ClockDraw(&g, size.cx, size.cy, ctx);
    }

    // A panel that has grown or shrunk keeps the edge nearest the screen's
    // edge where it was, so a clock in the top-right corner grows leftwards
    // rather than walking off the screen at ten o'clock.
    RECT current;
    GetWindowRect(g_wnd, &current);
    POINT position = { current.left, current.top };
    const int oldW = current.right - current.left, oldH = current.bottom - current.top;
    if (oldW > 0 && (oldW != size.cx || oldH != size.cy)) {
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            const int centreX = current.left + oldW / 2;
            const int centreY = current.top + oldH / 2;
            const int midX = (mi.rcWork.left + mi.rcWork.right) / 2;
            const int midY = (mi.rcWork.top + mi.rcWork.bottom) / 2;
            if (centreX > midX) position.x = current.right - size.cx;
            if (centreY > midY) position.y = current.bottom - size.cy;
        }
        int x = position.x, y = position.y;
        ClampOnScreen(&x, &y, size);
        position.x = x;
        position.y = y;
        g_cfg->clockX = x;
        g_cfg->clockY = y;
    }

    POINT origin = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_wnd, screen, &position, &size, g_surfaceDc, &origin, 0,
                        &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// Arms the timer for the next boundary the face will change on: the next
// second when seconds are shown, otherwise the next minute. Re-armed on
// every tick, so a clock with no seconds wakes sixty times an hour.
void ArmTick() {
    if (!g_wnd || !g_wantVisible) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    const bool perSecond = g_cfg && ClockStyleShowsSeconds(g_cfg->clockStyle, g_cfg->clockSeconds);
    UINT wait = perSecond ? (1000 - st.wMilliseconds)
                          : ((60 - st.wSecond) * 1000 - st.wMilliseconds);
    // A few milliseconds late rather than a few early: an early tick draws
    // the old second and then waits a whole interval to correct it.
    wait += 15;
    if (wait < 30) wait = 30;
    SetTimer(g_wnd, kTimerTick, wait, nullptr);
}

void ApplyClickThrough() {
    if (!g_wnd || !g_cfg) return;
    LONG ex = GetWindowLongW(g_wnd, GWL_EXSTYLE);
    if (g_cfg->clockPinned) ex |= WS_EX_TRANSPARENT;
    else                    ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongW(g_wnd, GWL_EXSTYLE, ex);
}

// Desktop mode, exactly as the monitor does it: owned by the shell window
// and dropped to the bottom, which lands one step above the wallpaper while
// keeping per-pixel alpha (invariant 11).
void ApplyZOrder() {
    if (!g_wnd || !g_cfg) return;
    if (g_cfg->clockOnDesktop) {
        HWND desktop = GetShellWindow();
        if (!desktop) desktop = FindWindowW(L"Progman", nullptr);
        SetWindowPos(g_wnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowLongPtrW(g_wnd, GWLP_HWNDPARENT, (LONG_PTR)desktop);
        SetWindowPos(g_wnd, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        SetWindowLongPtrW(g_wnd, GWLP_HWNDPARENT, 0);
        SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void SaveAndRefresh() {
    ClockApplyConfig();
    AppSaveConfig();
    AppRefreshSettings();
}

// ---------------------------------------------------------------- snapping
struct SnapPoint { const wchar_t* name; float fx; float fy; };
const SnapPoint kSnapPoints[] = {
    { L"Top left",      0.0f, 0.0f },
    { L"Top centre",    0.5f, 0.0f },
    { L"Top right",     1.0f, 0.0f },
    { L"Left",          0.0f, 0.5f },
    { L"Centre",        0.5f, 0.5f },
    { L"Right",         1.0f, 0.5f },
    { L"Bottom left",   0.0f, 1.0f },
    { L"Bottom centre", 0.5f, 1.0f },
    { L"Bottom right",  1.0f, 1.0f },
};
constexpr int kSnapCount = (int)(sizeof(kSnapPoints) / sizeof(kSnapPoints[0]));
constexpr int kSnapMargin = 16;

void SnapTo(int index) {
    if (!g_wnd || !g_cfg || index < 0 || index >= kSnapCount) return;
    RECT r{};
    if (!GetWindowRect(g_wnd, &r)) return;
    const SIZE size = { r.right - r.left, r.bottom - r.top };
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi)) return;

    const SnapPoint& p = kSnapPoints[index];
    const int margin = (int)(kSnapMargin * Scale());
    const int left   = (int)mi.rcWork.left + margin;
    const int top    = (int)mi.rcWork.top + margin;
    const int spanX  = (int)(mi.rcWork.right - mi.rcWork.left) - size.cx - margin * 2;
    const int spanY  = (int)(mi.rcWork.bottom - mi.rcWork.top) - size.cy - margin * 2;
    g_cfg->clockX = left + (int)((std::max)(0, spanX) * p.fx);
    g_cfg->clockY = top  + (int)((std::max)(0, spanY) * p.fy);
    SaveAndRefresh();
}

void ShowContextMenu() {
    if (!g_cfg) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING | (g_cfg->clockPinned ? MF_CHECKED : 0),
                IDM_PIN, L"Pin in place (click-through)");
    AppendMenuW(menu, MF_STRING | (g_cfg->clockOnDesktop ? MF_CHECKED : 0),
                IDM_DESKTOP, L"Sit on the desktop (behind windows)");
    HMENU snap = CreatePopupMenu();
    if (snap) {
        for (int i = 0; i < kSnapCount; ++i)
            AppendMenuW(snap, MF_STRING, IDM_SNAP_BASE + i, kSnapPoints[i].name);
        AppendMenuW(menu, MF_POPUP | (g_cfg->clockPinned ? MF_GRAYED : 0),
                    (UINT_PTR)snap, L"Move to");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU styles = CreatePopupMenu();
    if (styles) {
        for (int i = 0; i < ClockStyleCount(); ++i)
            AppendMenuW(styles, MF_STRING, IDM_STYLE_BASE + i, ClockStyleAt(i).name);
        CheckMenuRadioItem(styles, IDM_STYLE_BASE, IDM_STYLE_BASE + ClockStyleCount() - 1,
                           IDM_STYLE_BASE + g_cfg->clockStyle, MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)styles, L"Style");
    }
    HMENU themes = CreatePopupMenu();
    if (themes) {
        for (int i = 0; i < ClockSkinCount(); ++i)
            AppendMenuW(themes, MF_STRING, IDM_THEME_BASE + i, ClockSkinAt(i).name);
        CheckMenuRadioItem(themes, IDM_THEME_BASE, IDM_THEME_BASE + ClockSkinCount() - 1,
                           IDM_THEME_BASE + g_cfg->clockTheme, MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)themes, L"Theme");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_cfg->clockHours24 ? MF_CHECKED : 0), IDM_24H, L"24-hour");
    AppendMenuW(menu, MF_STRING | (g_cfg->clockSeconds ? MF_CHECKED : 0), IDM_SECONDS, L"Show seconds");
    AppendMenuW(menu, MF_STRING | (g_cfg->clockDate ? MF_CHECKED : 0), IDM_DATE, L"Show the date");
    AppendMenuW(menu, MF_STRING | (g_cfg->clockWeekday ? MF_CHECKED : 0), IDM_WEEKDAY, L"Show the weekday");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Clock settings...");
    AppendMenuW(menu, MF_STRING, IDM_HIDE, L"Hide");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    const int cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                        pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_wnd, WM_NULL, 0, 0);

    if (cmd >= (int)IDM_THEME_BASE && cmd < (int)IDM_THEME_BASE + ClockSkinCount()) {
        g_cfg->clockTheme = cmd - (int)IDM_THEME_BASE;
        SaveAndRefresh();
        return;
    }
    if (cmd >= (int)IDM_STYLE_BASE && cmd < (int)IDM_STYLE_BASE + ClockStyleCount()) {
        g_cfg->clockStyle = cmd - (int)IDM_STYLE_BASE;
        SaveAndRefresh();
        return;
    }
    if (cmd >= (int)IDM_SNAP_BASE && cmd < (int)IDM_SNAP_BASE + kSnapCount) {
        SnapTo(cmd - (int)IDM_SNAP_BASE);
        return;
    }
    switch (cmd) {
        case IDM_PIN:      ClockSetPinned(!g_cfg->clockPinned); return;
        case IDM_DESKTOP:  g_cfg->clockOnDesktop = !g_cfg->clockOnDesktop; break;
        case IDM_24H:      g_cfg->clockHours24 = !g_cfg->clockHours24; break;
        case IDM_SECONDS:  g_cfg->clockSeconds = !g_cfg->clockSeconds; break;
        case IDM_DATE:     g_cfg->clockDate    = !g_cfg->clockDate; break;
        case IDM_WEEKDAY:  g_cfg->clockWeekday = !g_cfg->clockWeekday; break;
        case IDM_SETTINGS: AppOpenClockSettings(); return;
        case IDM_HIDE:
            g_cfg->clockEnabled = false;
            ClockSetVisible(false);
            AppSaveConfig();
            AppRefreshSettings();
            return;
        default: return;
    }
    SaveAndRefresh();
}

LRESULT CALLBACK ClockProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kTimerTick) {
                Redraw();
                ArmTick();
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (g_cfg && !g_cfg->clockPinned) {
                g_dragging = true;
                GetCursorPos(&g_dragOrigin);
                GetWindowRect(wnd, &g_dragStart);
                SetCapture(wnd);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (g_dragging) {
                POINT now;
                GetCursorPos(&now);
                RECT r;
                GetWindowRect(wnd, &r);
                const SIZE size = { r.right - r.left, r.bottom - r.top };
                int x = g_dragStart.left + (now.x - g_dragOrigin.x);
                int y = g_dragStart.top + (now.y - g_dragOrigin.y);
                ClampOnScreen(&x, &y, size);
                SetWindowPos(wnd, nullptr, x, y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;

        case WM_CAPTURECHANGED:
            g_dragging = false;
            return 0;

        case WM_LBUTTONUP:
            if (g_dragging) {
                g_dragging = false;
                ReleaseCapture();
                RECT r;
                GetWindowRect(wnd, &r);
                if (g_cfg) {
                    g_cfg->clockX = r.left;
                    g_cfg->clockY = r.top;
                    AppSaveConfig();
                }
            }
            return 0;

        case WM_RBUTTONUP:
            ShowContextMenu();
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            ClockApplyConfig();
            return 0;

        case WM_SETTINGCHANGE:
            // The wallpaper changing replaces the desktop window under a
            // clock that sits on it; and a change of date or time format
            // changes what the locale writes.
            if (g_cfg && g_cfg->clockOnDesktop) ApplyZOrder();
            InvalidatePanel();
            Redraw();
            return 0;

        case WM_TIMECHANGE:
            InvalidatePanel();
            Redraw();
            ArmTick();
            return 0;

        case WM_DESTROY:
            g_wnd = nullptr;
            ReleaseSurface();
            return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace

// ---------------------------------------------------------------- public
void ClockInit(HINSTANCE inst, Config* cfg) {
    g_inst = inst;
    g_cfg  = cfg;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ClockProc;
    wc.hInstance     = inst;
    wc.lpszClassName = kClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_SIZEALL);
    RegisterClassExW(&wc);

    if (cfg->clockEnabled) ClockSetVisible(true);
}

void ClockShutdown() {
    g_wantVisible = false;
    if (g_wnd) {
        KillTimer(g_wnd, kTimerTick);
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
    }
    ReleaseSurface();
}

bool ClockVisible() { return g_wnd != nullptr && IsWindowVisible(g_wnd); }

void ClockSetVisible(bool visible) {
    if (!g_cfg) return;
    g_wantVisible = visible;

    if (!visible) {
        // Hidden, not destroyed, for the monitor's reasons: this is reached
        // on every game and every screen blank, and hiding cannot fail.
        if (g_wnd) {
            KillTimer(g_wnd, kTimerTick);
            ShowWindow(g_wnd, SW_HIDE);
        }
        ReleaseSurface();
        return;
    }

    if (g_wnd) {
        if (IsWindowVisible(g_wnd)) return;
        ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
        ClockApplyConfig();
        return;
    }

    g_wnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClass, L"Clock", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, g_inst, nullptr);
    if (!g_wnd) return;
    ClockApplyConfig();
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
}

void ClockSetPinned(bool pinned) {
    if (!g_cfg) return;
    g_cfg->clockPinned = pinned;
    ApplyClickThrough();
    AppSaveConfig();
    AppRefreshSettings();
}

bool ClockPinned() { return g_cfg && g_cfg->clockPinned; }

void ClockReattach() {
    if (!g_cfg || !g_cfg->clockEnabled) return;
    if (!g_wnd) { if (g_wantVisible) ClockSetVisible(true); return; }
    if (g_cfg->clockOnDesktop) ApplyZOrder();
}

void ClockApplyConfig() {
    if (!g_wnd || !g_cfg) return;
    InvalidatePanel();

    const ClockPaintCtx ctx = LiveCtx();
    const SIZE size = ClockMeasure(ctx);

    int x = g_cfg->clockX;
    int y = g_cfg->clockY;
    if (x == INT_MIN || y == INT_MIN) {
        // First run: top-left of the primary monitor's work area, clear of
        // the monitor overlay's default corner on the right.
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            x = mi.rcWork.left + 24;
            y = mi.rcWork.top + 24;
        } else {
            x = 40; y = 40;
        }
    }
    ClampOnScreen(&x, &y, size);
    SetWindowPos(g_wnd, nullptr, x, y, size.cx, size.cy, SWP_NOACTIVATE | SWP_NOZORDER);
    ApplyZOrder();
    ApplyClickThrough();

    if (g_wantVisible) {
        Redraw();
        ArmTick();
    } else {
        KillTimer(g_wnd, kTimerTick);
    }
}

void ClockDrawPreview(HDC dc, const RECT& area, const ClockPreview& look) {
    const int w = area.right - area.left;
    const int h = area.bottom - area.top;
    if (w <= 16 || h <= 16) return;

    ClockPaintCtx ctx;
    ctx.alpha   = (BYTE)(255 * (std::max)(20, (std::min)(100, look.opacity)) / 100);
    ctx.style   = look.style;
    ctx.skin    = &ClockSkinAt(look.theme);
    ctx.hours24 = look.hours24;
    ctx.seconds = look.seconds;
    ctx.date    = look.date;
    ctx.weekday = look.weekday;
    // A fixed moment - a Friday afternoon - so every preview is the same
    // picture and the hands sit at an angle that shows all three.
    ctx.time = {};
    ctx.time.wYear = 2026; ctx.time.wMonth = 9; ctx.time.wDay = 18;
    ctx.time.wDayOfWeek = 5;
    ctx.time.wHour = 10; ctx.time.wMinute = 8; ctx.time.wSecond = 42;

    // Largest scale that fits the space we were given, measured at 1:1 first.
    ctx.scale = 1.0f;
    const SIZE unit = ClockMeasure(ctx);
    ctx.scale = (std::min)((float)w / (float)unit.cx, (float)h / (float)unit.cy);
    ctx.scale = (std::max)(0.3f, (std::min)(1.4f, ctx.scale));
    const SIZE size = ClockMeasure(ctx);

    Gdiplus::Graphics g(dc);
    g.SetClip(Gdiplus::Rect(area.left, area.top, w, h));
    g.TranslateTransform((float)(area.left + (w - size.cx) / 2),
                         (float)(area.top + (h - size.cy) / 2));
    ClockDraw(&g, size.cx, size.cy, ctx);
}

} // namespace awa
