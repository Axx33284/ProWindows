#include "dragguide.h"
#include "winutil.h"

namespace awa {

namespace {

const wchar_t kClass[] = L"ProWindows_DragGuide";

HINSTANCE g_inst = nullptr;
Config*   g_cfg  = nullptr;
HWND      g_wnd  = nullptr;
Rect      g_shown;                 // what is currently on screen
bool      g_visible = false;

LRESULT CALLBACK GuideProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // Nothing to handle: it never receives input (WS_EX_TRANSPARENT) and it
    // never paints through WM_PAINT (UpdateLayeredWindow owns its pixels).
    return DefWindowProcW(h, msg, wp, lp);
}

// Per-pixel alpha wants premultiplied colour: an alpha of a means each channel
// is scaled by a/255 before it is stored, or the edges come out haloed.
inline DWORD Premultiplied(COLORREF c, BYTE a) {
    const DWORD r = (GetRValue(c) * a) / 255;
    const DWORD g = (GetGValue(c) * a) / 255;
    const DWORD b = (GetBValue(c) * a) / 255;
    return ((DWORD)a << 24) | (r << 16) | (g << 8) | b;
}

// Paints the indicator into a fresh 32-bit surface and hands it to the
// compositor. Only called when the rectangle actually changes - see the guard
// in DragGuideShow - so the cost of walking the pixels is paid a handful of
// times per drag rather than on every timer tick.
bool Redraw(const Rect& r) {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = r.w;
    bi.bmiHeader.biHeight      = -r.h;          // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        ReleaseDC(nullptr, screen);
        return false;
    }

    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bmp);

    const COLORREF accent = g_cfg ? g_cfg->activeColor : RGB(0x7A, 0xA2, 0xF7);
    const DWORD body   = Premultiplied(accent, 0x38);
    const DWORD edge   = Premultiplied(accent, 0xD0);
    const int   border = (std::max)(2, ScaleDpi(3, DpiForWindow(g_wnd)));

    DWORD* px = static_cast<DWORD*>(bits);

    // One body row, then copied down: filling several million pixels one at a
    // time is visible as a hitch on a 4K display, and every row of the middle
    // of the rectangle is identical.
    for (int x = 0; x < r.w; ++x)
        px[x] = (x < border || x >= r.w - border) ? edge : body;
    for (int y = 1; y < r.h; ++y)
        memcpy(px + (size_t)y * r.w, px, (size_t)r.w * sizeof(DWORD));

    // The top and bottom edges go over the top of that.
    for (int y = 0; y < border && y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) px[(size_t)y * r.w + x] = edge;
    for (int y = (std::max)(0, r.h - border); y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) px[(size_t)y * r.w + x] = edge;

    POINT dst   = { r.x, r.y };
    SIZE  size  = { r.w, r.h };
    POINT src   = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    const BOOL ok = UpdateLayeredWindow(g_wnd, screen, &dst, &size, mem, &src, 0,
                                        &blend, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(bmp);
    ReleaseDC(nullptr, screen);
    return ok != FALSE;
}

bool Ensure() {
    if (g_wnd) return true;
    if (!g_inst) return false;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = GuideProc;
    wc.hInstance     = g_inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);          // harmless if it is already registered

    // WS_EX_TRANSPARENT so it cannot swallow the mouse, WS_EX_NOACTIVATE so it
    // cannot take the foreground from the window being dragged, and topmost so
    // it is drawn over that window rather than under it.
    g_wnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
        WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    return g_wnd != nullptr;
}

} // namespace

void DragGuideInit(HINSTANCE inst, Config* cfg) {
    g_inst = inst;
    g_cfg  = cfg;
}

void DragGuideShutdown() {
    if (g_wnd) {
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
    }
    g_visible = false;
    g_shown = Rect();
}

void DragGuideShow(const Rect& r) {
    if (r.empty()) { DragGuideHide(); return; }
    if (g_visible && r == g_shown) return;      // the common case, per tick
    if (!Ensure()) return;

    if (!Redraw(r)) return;
    g_shown = r;

    if (!g_visible) {
        // SWP_NOACTIVATE and SW_SHOWNA both matter: showing this must not
        // disturb the modal move loop running in the dragged window's process.
        SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_visible = true;
    }
}

void DragGuideHide() {
    if (!g_visible || !g_wnd) return;
    ShowWindow(g_wnd, SW_HIDE);
    g_visible = false;
    g_shown = Rect();
}

} // namespace awa
