#include "theme.h"
#include "winutil.h"
#include <objidl.h>
// GDI+ headers use bare min/max, which NOMINMAX removes. Feed them the
// std:: versions rather than re-enabling the Windows macros.
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <windowsx.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "uxtheme.lib")     // SetWindowTheme

namespace awa {
namespace theme {

// Defined further down, next to the card bookkeeping.
static bool OnCard(HWND dlg, HWND control, RECT* cardRect);
static bool InHeader(HWND dlg, HWND control);
// Whatever surface a control is sitting on, so its own painting can start
// from the same colour instead of punching a rectangle out of the page.
static HBRUSH Backdrop(HWND dlg, HWND control);

namespace {

ULONG_PTR g_gdiplusToken = 0;
HFONT  g_ui = nullptr, g_bold = nullptr, g_title = nullptr, g_small = nullptr,
       g_number = nullptr;
HBRUSH g_bgBrush = nullptr, g_panelBrush = nullptr, g_fieldBrush = nullptr;

// The DPI everything above was built for. A per-monitor-v2 process gets its
// dialogs laid out at the DPI of whichever monitor they open on, but a font
// built from the screen DC is always at the *system* DPI - so on a 150% laptop
// with a 100% external screen (or the other way round) every label was either
// clipped or half the size of the box drawn round it. Fonts and hand-drawn
// metrics now follow the window instead.
UINT g_dpi = 96;

// Which button the mouse is currently over, so hover can be drawn.
HWND g_hotButton = nullptr;
// Which tab the mouse is over, or -1. Tracked separately: a tab is an item
// inside one control, not a window of its own.
int  g_hotTab = -1;

// BS_DEFPUSHBUTTON is lost when a button becomes owner-drawn, so the accent
// ones are remembered here.
std::unordered_set<HWND> g_primaryButtons;

// ---------------------------------------------------------------- dark mode
// Win32 has no public switch for this. `SetWindowTheme(L"DarkMode_Explorer")`
// on its own does nothing to a list's scrollbars: the process has to have
// opted in first, and the only way to opt in is by ordinal out of uxtheme.
// Both are best-effort - on a build that does not have them the controls stay
// exactly as they were, which is what they did before this existed.
enum PreferredAppMode { AppModeDefault = 0, AllowDark = 1, ForceDark = 2 };

using SetPreferredAppModeFn   = PreferredAppMode (WINAPI*)(PreferredAppMode);
using AllowDarkModeForWindowFn = BOOL (WINAPI*)(HWND, BOOL);
using FlushMenuThemesFn        = void (WINAPI*)();

HMODULE UxTheme() {
    static HMODULE dll = LoadLibraryExW(L"uxtheme.dll", nullptr,
                                        LOAD_LIBRARY_SEARCH_SYSTEM32);
    return dll;
}

void EnableProcessDarkMode() {
    HMODULE ux = UxTheme();
    if (!ux) return;
    // 135 SetPreferredAppMode (1903+; on 1809 the same ordinal is the older
    // AllowDarkModeForApp, which takes a BOOL - and 1 means the same thing to
    // both, which is why this one call covers every build that has either).
    auto setMode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    // ForceDark, not AllowDark: AllowDark means "follow the system app-mode
    // setting", and this application is dark whatever that setting says. On a
    // machine set to light apps, AllowDark left every scrollbar white inside a
    // black dialog.
    if (setMode) setMode(ForceDark);
    // 136 FlushMenuThemes, so the change reaches anything already themed.
    auto flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (flush) flush();
}

void AllowDarkModeForWindow(HWND wnd) {
    HMODULE ux = UxTheme();
    if (!ux || !wnd) return;
    auto allow = (AllowDarkModeForWindowFn)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    if (allow) allow(wnd, TRUE);
}

Gdiplus::Color Argb(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

void AddRoundedPath(Gdiplus::GraphicsPath* path, const Gdiplus::Rect& r, int radius) {
    const int d = radius * 2;
    if (d <= 0) { path->AddRectangle(r); return; }
    path->AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path->AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    path->AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    path->AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    path->CloseFigure();
}

HFONT MakeFont(int points, int weight, UINT dpi) {
    const int height = -MulDiv(points, (int)dpi, 72);

    LOGFONTW lf = {};
    lf.lfHeight  = height;
    lf.lfWeight  = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

// A length measured at 96 dpi, in the units the current window actually uses.
// Every hand-drawn size in this file goes through here: the tick box, the combo
// chevron, the card insets. Left as raw pixels they stayed the same size while
// the dialog around them grew, which is what made the settings window look
// wrong on any machine not running at 100%.
inline int Sc(int px) { return MulDiv(px, (int)g_dpi, 96); }

bool IsClass(HWND wnd, const wchar_t* name) {
    wchar_t buf[64] = {};
    GetClassNameW(wnd, buf, 64);
    return _wcsicmp(buf, name) == 0;
}

// ---------------------------------------------------------------- controls
// Owner-drawn controls here all fill a background and then draw over it, which
// on a plain window DC is two visible steps. Painting into a memory DC and
// blitting once makes hover and focus changes silent.
class Buffered {
public:
    Buffered(HDC target, const RECT& r) : target_(target), rect_(r) {
        const int w = r.right - r.left, h = r.bottom - r.top;
        if (w <= 0 || h <= 0) return;
        mem_ = CreateCompatibleDC(target);
        if (!mem_) return;
        bmp_ = CreateCompatibleBitmap(target, w, h);
        if (!bmp_) { DeleteDC(mem_); mem_ = nullptr; return; }
        old_ = SelectObject(mem_, bmp_);
        // Draw in the caller's coordinates; the bitmap starts at the rect.
        SetViewportOrgEx(mem_, -r.left, -r.top, nullptr);
    }

    ~Buffered() {
        if (!mem_) return;
        SetViewportOrgEx(mem_, 0, 0, nullptr);
        BitBlt(target_, rect_.left, rect_.top, rect_.right - rect_.left,
               rect_.bottom - rect_.top, mem_, 0, 0, SRCCOPY);
        SelectObject(mem_, old_);
        DeleteObject(bmp_);
        DeleteDC(mem_);
    }

    Buffered(const Buffered&) = delete;
    Buffered& operator=(const Buffered&) = delete;

    HDC dc() const { return mem_ ? mem_ : target_; }

private:
    HDC     target_;
    RECT    rect_;
    HDC     mem_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
};

// A 1px accent ring just inside `r`. Owner-drawn controls lose the system's
// focus rectangle, and keyboard users need to see where they are.
void FocusRing(HDC dc, const RECT& r, int radius) {
    const int inset = (std::max)(1, Sc(2));
    RECT ring = { r.left + inset, r.top + inset,
                  r.right - inset, r.bottom - inset };
    if (ring.right <= ring.left || ring.bottom <= ring.top) return;
    RoundRectAlpha(dc, ring, radius, 0, 0, Accent, 190);
}

// Push buttons are owner-drawn: custom draw alone still lets the themed button
// paint its own text underneath, which shows through as a ghost.
void PaintButton(HWND button, HDC target, const RECT& rc, bool primary,
                 bool disabled, bool pressed, bool focused) {
    Buffered buffer(target, rc);
    HDC dc = buffer.dc();
    const bool hot = (button == g_hotButton);

    COLORREF fill = primary ? Accent : PanelAlt;
    if (disabled)     fill = RGB(46, 48, 53);
    else if (pressed) fill = primary ? RGB(70, 118, 224) : RGB(34, 36, 41);
    else if (hot)     fill = primary ? AccentHover : RGB(54, 57, 64);

    RECT r = rc;
    FillRect(dc, &r,
             Backdrop(GetParent(button), button));
    RoundRect(dc, r, Sc(5), fill, primary ? fill : Border, true);

    wchar_t text[256] = {};
    GetWindowTextW(button, text, 256);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? TextDim : (primary ? RGB(255, 255, 255) : Text));
    HGDIOBJ old = SelectObject(dc, g_ui);
    DrawTextW(dc, text, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old);

    if (focused && !disabled && !primary) FocusRing(dc, r, Sc(4));
}

void DrawCheckBox(HWND check, HDC target) {
    const bool checked  = SendMessageW(check, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool disabled = IsWindowEnabled(check) == FALSE;
    const bool hot      = (check == g_hotButton);

    RECT r;
    GetClientRect(check, &r);
    Buffered buffer(target, r);
    HDC dc = buffer.dc();
    FillRect(dc, &r,
             Backdrop(GetParent(check), check));

    const int side = Sc(15);
    RECT box = { r.left, r.top + (r.bottom - r.top - side) / 2,
                 r.left + side, r.top + (r.bottom - r.top - side) / 2 + side };

    COLORREF fill = checked ? Accent : Field;
    if (disabled)  fill = RGB(52, 54, 60);
    else if (hot)  fill = checked ? AccentHover : RGB(58, 62, 70);
    RoundRect(dc, box, Sc(4), fill, checked ? fill : Border, true);

    if (checked) {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // The tick is described against a 15 px box and then scaled with it,
        // so it stays centred instead of clinging to the top-left corner as
        // the box grows.
        const float k = (float)side / 15.0f;
        Gdiplus::Pen pen(Argb(RGB(255, 255, 255)), 2.0f * k);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        Gdiplus::PointF pts[3] = {
            { box.left + 3.5f * k, box.top + 7.5f * k },
            { box.left + 6.2f * k, box.top + 10.5f * k },
            { box.left + 11.5f * k, box.top + 4.5f * k },
        };
        g.DrawLines(&pen, pts, 3);
    }

    wchar_t text[256] = {};
    GetWindowTextW(check, text, 256);
    RECT tr = { r.left + side + Sc(7), r.top, r.right, r.bottom };

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? TextDim : Text);
    HGDIOBJ old = SelectObject(dc, g_ui);
    DrawTextW(dc, text, -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);

    if (!disabled && GetFocus() == check) {
        RECT ring = { r.left - 1, r.top, r.right, r.bottom };
        FocusRing(dc, ring, Sc(4));
    }
}

// Combo boxes are painted end to end. Owner-draw only covers the item text;
// the drop-down button beside it stays a light themed control otherwise, which
// is the one piece of chrome that gives the dark theme away.
void DrawCombo(HWND combo, HDC target) {
    RECT r;
    GetClientRect(combo, &r);
    Buffered buffer(target, r);
    HDC dc = buffer.dc();

    const bool disabled = IsWindowEnabled(combo) == FALSE;
    const bool hot      = (combo == g_hotButton);
    const bool focused  = (GetFocus() == combo);

    COLORREF fill = Field;
    if (disabled) fill = RGB(40, 42, 47);
    else if (hot) fill = RGB(56, 60, 69);

    FillRect(dc, &r,
             Backdrop(GetParent(combo), combo));
    RoundRect(dc, r, Sc(5), fill, focused ? Accent : Border, true);

    const int chevron = Sc(20);
    wchar_t text[256] = {};
    const int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && SendMessageW(combo, CB_GETLBTEXTLEN, sel, 0) < 256)
        SendMessageW(combo, CB_GETLBTEXT, sel, (LPARAM)text);

    RECT tr = { r.left + Sc(8), r.top, r.right - chevron, r.bottom };
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? TextDim : Text);
    HGDIOBJ old = SelectObject(dc, g_ui);
    DrawTextW(dc, text, -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float k = (float)g_dpi / 96.0f;
    Gdiplus::Pen pen(Argb(disabled ? TextDim : Text, 220), 1.6f * k);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    const float cx = (float)(r.right - chevron / 2 - Sc(4));
    const float cy = (float)((r.top + r.bottom) / 2);
    Gdiplus::PointF arrow[3] = {
        { cx - 3.5f * k, cy - 1.5f * k },
        { cx,            cy + 2.0f * k },
        { cx + 3.5f * k, cy - 1.5f * k },
    };
    g.DrawLines(&pen, arrow, 3);
}

// Edit fields keep their frame in the non-client area, where WM_CTLCOLOREDIT
// cannot reach it - so the border is redrawn here instead of being left as the
// light themed one.
LRESULT CALLBACK EditSubclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_NCPAINT: {
            HDC dc = GetWindowDC(wnd);
            if (dc) {
                RECT r;
                GetWindowRect(wnd, &r);
                OffsetRect(&r, -r.left, -r.top);

                RECT client = r;
                const int edge = GetSystemMetrics(SM_CXEDGE);
                InflateRect(&client, -edge, -GetSystemMetrics(SM_CYEDGE));
                // Leave the text alone; only the frame ring is ours.
                ExcludeClipRect(dc, client.left, client.top,
                                client.right, client.bottom);
                RoundRect(dc, r, Sc(4), Field,
                          GetFocus() == wnd ? Accent : Border, true);
                ReleaseDC(wnd, dc);
            }
            return 0;
        }

        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            const LRESULT result = DefSubclassProc(wnd, msg, wp, lp);
            RedrawWindow(wnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
            return result;
        }
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}

// A disabled list view paints its own background with the system window
// colour and ignores ListView_SetBkColor entirely, so switching file search
// off turned the folder list into a white rectangle in the middle of a dark
// page. The control still greys its text for us; all it needs is to be told
// what to erase to.
LRESULT CALLBACK ListViewSubclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp,
                                  UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND && !IsWindowEnabled(wnd)) {
        RECT r;
        GetClientRect(wnd, &r);
        FillRect((HDC)wp, &r, g_panelBrush ? g_panelBrush : GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    if (msg == WM_ENABLE) {
        // The whole control has to be repainted either way: coming back it has
        // to lose our fill, and going out it has to gain it.
        const LRESULT result = DefSubclassProc(wnd, msg, wp, lp);
        InvalidateRect(wnd, nullptr, TRUE);
        return result;
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}

LRESULT CALLBACK ComboSubclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp,
                               UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(wnd, &ps);
            DrawCombo(wnd, dc);
            EndPaint(wnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE:
            if (g_hotButton != wnd) {
                HWND previous = g_hotButton;
                g_hotButton = wnd;
                if (previous && IsWindow(previous))
                    InvalidateRect(previous, nullptr, FALSE);
                InvalidateRect(wnd, nullptr, FALSE);
                TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, wnd, 0 };
                TrackMouseEvent(&track);
            }
            break;

        case WM_MOUSELEAVE:
            if (g_hotButton == wnd) {
                g_hotButton = nullptr;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            break;

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case CB_SETCURSEL:
            InvalidateRect(wnd, nullptr, FALSE);
            break;
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}

// Buttons are subclassed for two reasons: a checkbox has to be painted
// entirely by us (custom draw still lets the themed label through as a
// ghost), and hover only reaches the control itself, never the dialog.
LRESULT CALLBACK ButtonSubclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp,
                                UINT_PTR, DWORD_PTR) {
    const LONG type = GetWindowLongW(wnd, GWL_STYLE) & BS_TYPEMASK;
    const bool isCheck = (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX);

    switch (msg) {
        case WM_MOUSEMOVE:
            if (g_hotButton != wnd) {
                HWND previous = g_hotButton;
                g_hotButton = wnd;
                if (previous && IsWindow(previous))
                    InvalidateRect(previous, nullptr, FALSE);
                InvalidateRect(wnd, nullptr, FALSE);

                TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, wnd, 0 };
                TrackMouseEvent(&track);
            }
            break;

        case WM_MOUSELEAVE:
            if (g_hotButton == wnd) {
                g_hotButton = nullptr;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            break;

        case WM_ERASEBKGND:
            // Every pixel is painted below, so erasing first only flickers.
            return 1;

        case WM_PAINT:
            if (isCheck) {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(wnd, &ps);
                DrawCheckBox(wnd, dc);
                EndPaint(wnd, &ps);
                return 0;
            }
            break;
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}

// TCS_OWNERDRAWFIXED only hands us the tab labels: the strip behind them and
// the frame around the page stay themed, which on Windows 11 means white. So
// the control is painted from scratch here and the items are dispatched by
// hand, exactly as the control would have done.
LRESULT CALLBACK TabSubclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp,
                             UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC target = BeginPaint(wnd, &ps);

            RECT client;
            GetClientRect(wnd, &client);
            {
                // Scoped: the buffer blits when it goes out of scope, and that
                // has to happen while EndPaint has not yet released the DC.
                Buffered buffer(target, client);
                HDC dc = buffer.dc();
                FillRect(dc, &client, g_bgBrush);

                const int count   = TabCtrl_GetItemCount(wnd);
                const int current = TabCtrl_GetCurSel(wnd);
                const int id      = GetDlgCtrlID(wnd);
                for (int i = 0; i < count; ++i) {
                    DRAWITEMSTRUCT dis = {};
                    if (!TabCtrl_GetItemRect(wnd, i, &dis.rcItem)) continue;
                    dis.CtlType    = ODT_TAB;
                    dis.CtlID      = (UINT)id;
                    dis.itemID     = (UINT)i;
                    dis.itemAction = ODA_DRAWENTIRE;
                    dis.itemState  = (i == current) ? ODS_SELECTED : 0;
                    dis.hwndItem   = wnd;
                    dis.hDC        = dc;
                    SendMessageW(GetParent(wnd), WM_DRAWITEM, (WPARAM)id,
                                 (LPARAM)&dis);
                }
            }

            EndPaint(wnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            // Repaint only when the tab under the pointer changes; otherwise
            // every pixel of movement would redraw the whole strip.
            TCHITTESTINFO hit = {};
            hit.pt.x = GET_X_LPARAM(lp);
            hit.pt.y = GET_Y_LPARAM(lp);
            const int over = TabCtrl_HitTest(wnd, &hit);
            if (over != g_hotTab) {
                g_hotTab = over;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, wnd, 0 };
            TrackMouseEvent(&track);
            break;
        }

        case WM_MOUSELEAVE:
            if (g_hotTab != -1) {
                g_hotTab = -1;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            break;
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}

void DrawTrackbar(HWND bar, NMCUSTOMDRAW* cd) {
    RECT client;
    GetClientRect(bar, &client);
    FillRect(cd->hdc, &client,
             Backdrop(GetParent(bar), bar));

    RECT channel;
    SendMessageW(bar, TBM_GETCHANNELRECT, 0, (LPARAM)&channel);
    RECT thumb;
    SendMessageW(bar, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);

    const bool enabled = IsWindowEnabled(bar) != FALSE;
    const int midY = (thumb.top + thumb.bottom) / 2;

    const int half   = (std::max)(1, Sc(2));
    const int knobW   = (std::max)(3, Sc(6));
    const int knobH   = (std::max)(4, Sc(7));
    const int centreX = (thumb.left + thumb.right) / 2;

    RECT track = { channel.left, midY - half, channel.right, midY + half };
    RoundRect(cd->hdc, track, half, Field, Field, false);

    RECT done = { channel.left, midY - half, centreX, midY + half };
    if (done.right > done.left)
        RoundRect(cd->hdc, done, half, enabled ? Accent : RGB(70, 73, 80),
                  Accent, false);

    RECT knob = { centreX - knobW, midY - knobH, centreX + knobW, midY + knobH };
    RoundRect(cd->hdc, knob, knobW, enabled ? RGB(226, 232, 245) : RGB(96, 99, 106),
              Border, true);
}

} // namespace

// ---------------------------------------------------------------- lifecycle
namespace {

struct FontSet { HFONT ui, bold, title, tiny, number; };

// One set per DPI the process has actually been shown at. Kept rather than
// swapped because controls hold their HFONT after WM_SETFONT: deleting the old
// set on a DPI change would hand every dialog a dangling font mid-repaint. In
// practice this is one or two entries - a machine only has so many scales.
std::unordered_map<UINT, FontSet> g_fontSets;

const FontSet& FontsFor(UINT dpi) {
    auto it = g_fontSets.find(dpi);
    if (it != g_fontSets.end()) return it->second;

    FontSet set;
    set.ui     = MakeFont(9,  FW_NORMAL,   dpi);
    set.bold   = MakeFont(9,  FW_SEMIBOLD, dpi);
    set.title  = MakeFont(11, FW_SEMIBOLD, dpi);
    set.tiny   = MakeFont(8,  FW_NORMAL,   dpi);
    set.number = MakeFont(14, FW_SEMIBOLD, dpi);
    return g_fontSets.emplace(dpi, set).first->second;
}

} // namespace

void SetDpi(UINT dpi) {
    if (dpi < 48 || dpi > 960) return;        // nothing real is outside this
    if (dpi == g_dpi && g_ui) return;

    const FontSet& set = FontsFor(dpi);
    g_dpi    = dpi;
    g_ui     = set.ui;
    g_bold   = set.bold;
    g_title  = set.title;
    g_small  = set.tiny;
    g_number = set.number;
}

UINT Dpi() { return g_dpi; }

void Init() {
    // Before any control exists: "DarkMode_Explorer" only reaches the
    // scrollbars of a list once the process itself has asked for dark mode, and
    // asking is not a documented call. Without it the folder list on the Search
    // page had a bright white scrollbar down one side of a black dialog.
    EnableProcessDarkMode();

    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);

    // A starting point only: every dialog and the launcher re-point this at
    // their own monitor's DPI before they draw anything.
    HDC screen = GetDC(nullptr);
    const int systemDpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    g_dpi = 0;                                 // force SetDpi to build the set
    SetDpi(systemDpi > 0 ? (UINT)systemDpi : 96u);

    g_bgBrush    = CreateSolidBrush(Bg);
    g_panelBrush = CreateSolidBrush(Panel);
    g_fieldBrush = CreateSolidBrush(Field);
}

void Shutdown() {
    for (auto& kv : g_fontSets) {
        for (HFONT f : { kv.second.ui, kv.second.bold, kv.second.title,
                         kv.second.tiny, kv.second.number })
            if (f) DeleteObject(f);
    }
    g_fontSets.clear();
    g_ui = g_bold = g_title = g_small = g_number = nullptr;

    for (HBRUSH* b : { &g_bgBrush, &g_panelBrush, &g_fieldBrush })
        if (*b) { DeleteObject(*b); *b = nullptr; }
    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

HFONT FontUI()     { return g_ui; }
HFONT FontBold()   { return g_bold; }
HFONT FontTitle()  { return g_title; }
HFONT FontSmall()  { return g_small; }
HFONT FontNumber() { return g_number; }

HBRUSH BrushBg()    { return g_bgBrush; }
HBRUSH BrushPanel() { return g_panelBrush; }
HBRUSH BrushField() { return g_fieldBrush; }

void DarkTitleBar(HWND wnd) {
    BOOL on = TRUE;
    // 20 on current builds, 19 on 1809-era ones.
    if (FAILED(DwmSetWindowAttribute(wnd, 20, &on, sizeof(on))))
        DwmSetWindowAttribute(wnd, 19, &on, sizeof(on));
}

// ---------------------------------------------------------------- shapes
void RoundRect(HDC dc, const RECT& r, int radius, COLORREF fill,
               COLORREF borderColor, bool drawBorder) {
    RoundRectAlpha(dc, r, radius, fill, 255, borderColor, drawBorder ? 255 : 0);
}

void RoundRectAlpha(HDC dc, const RECT& r, int radius, COLORREF fill, BYTE alpha,
                    COLORREF borderColor, BYTE borderAlpha) {
    if (r.right <= r.left || r.bottom <= r.top) return;

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    Gdiplus::Rect rect(r.left, r.top, r.right - r.left - 1, r.bottom - r.top - 1);
    Gdiplus::GraphicsPath path;
    AddRoundedPath(&path, rect, radius);

    if (alpha > 0) {
        Gdiplus::SolidBrush brush(Argb(fill, alpha));
        g.FillPath(&brush, &path);
    }
    if (borderAlpha > 0) {
        Gdiplus::Pen pen(Argb(borderColor, borderAlpha), 1.0f);
        g.DrawPath(&pen, &path);
    }
}

void FillRoundBar(HDC dc, const RECT& r, int radius, COLORREF color, BYTE alpha) {
    if (r.right <= r.left) return;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Rect rect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    Gdiplus::GraphicsPath path;
    AddRoundedPath(&path, rect, radius);
    Gdiplus::SolidBrush brush(Argb(color, alpha));
    g.FillPath(&brush, &path);
}

void DrawCard(HDC dc, const RECT& r, const wchar_t* title) {
    RoundRect(dc, r, Sc(8), Panel, Border, true);

    // A hairline of light along the top edge lifts the card off the window;
    // without it a Panel-on-Bg card is only a four-step difference in value.
    RECT gloss = { r.left + Sc(9), r.top + Sc(1),
                   r.right - Sc(9), r.top + Sc(2) };
    if (gloss.bottom <= gloss.top) gloss.bottom = gloss.top + 1;
    FillRoundBar(dc, gloss, 0, RGB(255, 255, 255), 14);

    if (!title || !*title) return;

    // A short accent tick in front of the title, so the eye can find the start
    // of each card down a long page.
    RECT tick = { r.left + Sc(12), r.top + Sc(11),
                  r.left + Sc(15), r.top + Sc(21) };
    FillRoundBar(dc, tick, (std::max)(1, Sc(1)), Accent, 235);

    RECT tr = { r.left + Sc(21), r.top + Sc(7),
                r.right - Sc(12), r.top + Sc(25) };
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, TextDim);
    HGDIOBJ old = SelectObject(dc, g_bold);
    DrawTextW(dc, title, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

// ---------------------------------------------------------------- dialogs
// Cards are the hidden groupboxes' rectangles, remembered per dialog.
struct CardInfo { RECT rect; std::wstring title; };
static std::unordered_map<HWND, std::vector<CardInfo>> g_cards;
static std::unordered_map<HWND, int> g_headers;

void SetHeaderHeight(HWND dlg, int height) { g_headers[dlg] = height; }

void ForgetDialog(HWND dlg) {
    g_cards.erase(dlg);
    g_headers.erase(dlg);
    for (auto it = g_primaryButtons.begin(); it != g_primaryButtons.end(); ) {
        if (!IsWindow(*it) || GetParent(*it) == dlg) it = g_primaryButtons.erase(it);
        else ++it;
    }
}

// True when a control sits in the dialog's header strip, which is Panel rather
// than Bg - so its text needs the matching brush behind it.
static bool InHeader(HWND dlg, HWND control) {
    auto it = g_headers.find(dlg);
    if (it == g_headers.end() || it->second <= 0) return false;

    RECT r;
    GetWindowRect(control, &r);
    MapWindowPoints(nullptr, dlg, (LPPOINT)&r, 2);
    return ((r.top + r.bottom) / 2) < it->second;
}

// True when a control sits inside one of the dialog's cards, which decides
// whether its background should be the card colour or the window colour.
static bool OnCard(HWND dlg, HWND control, RECT* cardRect) {
    auto it = g_cards.find(dlg);
    if (it == g_cards.end()) return false;

    RECT r;
    GetWindowRect(control, &r);
    MapWindowPoints(nullptr, dlg, (LPPOINT)&r, 2);
    const POINT centre = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };

    for (const auto& card : it->second) {
        if (PtInRect(&card.rect, centre)) {
            if (cardRect) *cardRect = card.rect;
            return true;
        }
    }
    return false;
}

static HBRUSH Backdrop(HWND dlg, HWND control) {
    if (OnCard(dlg, control, nullptr) || InHeader(dlg, control)) return g_panelBrush;
    return g_bgBrush;
}

void PrepareDialog(HWND dlg) {
    // Everything below - the fonts handed to the children, the card rectangles
    // measured from the hidden groupboxes - is in this window's units, so the
    // theme has to be pointed at this window's DPI first.
    SetDpi(DpiForWindow(dlg));

    std::vector<CardInfo> cards;

    struct Walker {
        static BOOL CALLBACK Proc(HWND child, LPARAM param) {
            auto* found = reinterpret_cast<std::vector<CardInfo>*>(param);
            SendMessageW(child, WM_SETFONT, (WPARAM)g_ui, TRUE);

            if (IsClass(child, L"Button") &&
                (GetWindowLongW(child, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX) {
                CardInfo card;
                GetWindowRect(child, &card.rect);
                MapWindowPoints(nullptr, GetParent(child), (LPPOINT)&card.rect, 2);

                wchar_t text[128] = {};
                GetWindowTextW(child, text, 128);
                card.title = text;

                found->push_back(card);
                ShowWindow(child, SW_HIDE);       // the card is painted instead
                return TRUE;
            }

            if (IsClass(child, L"Button")) {
                const LONG style = GetWindowLongW(child, GWL_STYLE);
                const LONG type  = style & BS_TYPEMASK;
                if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) {
                    if (type == BS_DEFPUSHBUTTON) g_primaryButtons.insert(child);
                    SetWindowLongW(child, GWL_STYLE,
                                   (style & ~BS_TYPEMASK) | BS_OWNERDRAW);
                }
                SetWindowSubclass(child, ButtonSubclass, 1, 0);
                return TRUE;
            }

            if (IsClass(child, L"ComboBox")) {
                // Owner-draw is the only way to get a dark list; the style has
                // to be there before the first paint. The closed control is
                // painted by ComboSubclass, drop-down button included.
                SetWindowLongW(child, GWL_STYLE,
                               GetWindowLongW(child, GWL_STYLE) | CBS_OWNERDRAWFIXED);
                SendMessageW(child, CB_SETITEMHEIGHT, 0, Sc(18));
                SetWindowTheme(child, L"DarkMode_CFD", nullptr);
                SetWindowSubclass(child, ComboSubclass, 1, 0);
            } else if (IsClass(child, L"Edit")) {
                SetWindowSubclass(child, EditSubclass, 1, 0);
            } else if (IsClass(child, L"SysListView32")) {
                ListView_SetBkColor(child, Panel);
                ListView_SetTextBkColor(child, Panel);
                ListView_SetTextColor(child, Text);
                AllowDarkModeForWindow(child);
                SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
                SetWindowSubclass(child, ListViewSubclass, 1, 0);
                // The column header is a control of its own and does not
                // inherit any of that. Left alone it stays pure white, which
                // on the Window keys page was a bright band across the top of
                // an otherwise black list.
                if (HWND header = ListView_GetHeader(child)) {
                    AllowDarkModeForWindow(header);
                    SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
                }
            } else if (IsClass(child, L"SysTabControl32")) {
                SetWindowLongW(child, GWL_STYLE,
                               GetWindowLongW(child, GWL_STYLE) |
                               TCS_OWNERDRAWFIXED);
                SetWindowTheme(child, L"", L"");     // no themed frame to fight
                SetWindowSubclass(child, TabSubclass, 1, 0);
            } else if (IsClass(child, L"ListBox")) {
                AllowDarkModeForWindow(child);
                SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
            }
            return TRUE;
        }
    };
    EnumChildWindows(dlg, Walker::Proc, reinterpret_cast<LPARAM>(&cards));
    g_cards[dlg] = std::move(cards);
}

// Buttons have no hover state of their own once we draw them, so track it.
static void TrackHover(HWND dlg) {
    POINT pt;
    GetCursorPos(&pt);
    HWND under = WindowFromPoint(pt);
    const bool hoverable = under && GetParent(under) == dlg &&
                           (IsClass(under, L"Button") || IsClass(under, L"ComboBox"));
    HWND hot = hoverable ? under : nullptr;
    if (hot != g_hotButton) {
        HWND previous = g_hotButton;
        g_hotButton = hot;
        if (previous && IsWindow(previous)) InvalidateRect(previous, nullptr, TRUE);
        if (hot) InvalidateRect(hot, nullptr, TRUE);
    }
}

#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

bool DialogMessage(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, INT_PTR* result) {
    switch (msg) {
        // Per-monitor-v2 resizes the dialog and its children for us, but it
        // knows nothing about the fonts we forced on them or the card
        // rectangles we measured off the hidden groupboxes. Both have to be
        // taken again at the new scale, or the window arrives on the second
        // monitor the right size with the wrong-sized text in it.
        case WM_DPICHANGED:
        case WM_DPICHANGED_AFTERPARENT:
            PrepareDialog(dlg);
            RedrawWindow(dlg, nullptr, nullptr,
                         RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
            return false;      // the dialog manager still has to do its part

        case WM_ERASEBKGND: {
            HDC dc = (HDC)wp;
            // Pages of the settings window are separate dialogs; whichever one
            // painted last left its own scale behind.
            SetDpi(DpiForWindow(dlg));
            RECT client;
            GetClientRect(dlg, &client);
            FillRect(dc, &client, g_bgBrush);

            auto header = g_headers.find(dlg);
            if (header != g_headers.end() && header->second > 0) {
                RECT strip = { client.left, client.top, client.right, header->second };
                FillRect(dc, &strip, g_panelBrush);

                RECT line = { client.left, header->second - 1, client.right,
                              header->second };
                HBRUSH b = CreateSolidBrush(Border);
                FillRect(dc, &line, b);
                DeleteObject(b);

                // The rule under the header fades from accent to nothing, so
                // the window has a single deliberate splash of colour at the top.
                Gdiplus::Graphics g(dc);
                Gdiplus::RectF fade(0.0f, (float)(header->second - 2),
                                    (float)(client.right - client.left) * 0.55f, 2.0f);
                if (fade.Width > 1.0f) {
                    Gdiplus::LinearGradientBrush accent(
                        fade, Argb(Accent, 190), Argb(Accent, 0),
                        Gdiplus::LinearGradientModeHorizontal);
                    g.FillRectangle(&accent, fade);
                }
            }

            auto cards = g_cards.find(dlg);
            if (cards != g_cards.end())
                for (const auto& card : cards->second)
                    DrawCard(dc, card.rect, card.title.c_str());

            *result = TRUE;
            return true;
        }

        case WM_NCDESTROY:
            // Page dialogs never called ForgetDialog, so their card lists piled
            // up every time the settings window was rebuilt.
            ForgetDialog(dlg);
            return false;      // the dialog still wants to see this

        case WM_CTLCOLORDLG:
            *result = (INT_PTR)g_bgBrush;
            return true;

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, Text);
            // Match whatever the control is sitting on, or the text gets a
            // mismatched rectangle behind it.
            *result = (INT_PTR)Backdrop(dlg, (HWND)lp);
            return true;
        }

        case WM_CTLCOLORBTN:
            *result = (INT_PTR)Backdrop(dlg, (HWND)lp);
            return true;

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wp;
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, Field);
            SetTextColor(dc, Text);
            *result = (INT_PTR)g_fieldBrush;
            return true;
        }

        case WM_MOUSEMOVE:
            TrackHover(dlg);
            return false;      // let the dialog see it too

        case WM_DRAWITEM: {
            auto* dis = (DRAWITEMSTRUCT*)lp;

            if (dis->CtlType == ODT_BUTTON) {
                PaintButton(dis->hwndItem, dis->hDC, dis->rcItem,
                            g_primaryButtons.count(dis->hwndItem) != 0,
                            (dis->itemState & ODS_DISABLED) != 0,
                            (dis->itemState & ODS_SELECTED) != 0,
                            (dis->itemState & ODS_FOCUS) != 0);
                *result = TRUE;
                return true;
            }

            if (dis->CtlType == ODT_COMBOBOX) {
                const bool inEdit = (dis->itemState & ODS_COMBOBOXEDIT) != 0;
                const bool sel = !inEdit && (dis->itemState & ODS_SELECTED) != 0;

                RECT r = dis->rcItem;
                HBRUSH back = CreateSolidBrush(sel ? Accent : (inEdit ? Field : PanelAlt));
                FillRect(dis->hDC, &r, back);
                DeleteObject(back);

                if ((int)dis->itemID >= 0) {
                    wchar_t text[256] = {};
                    SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, (LPARAM)text);
                    RECT tr = { r.left + 5, r.top, r.right - 4, r.bottom };
                    SetBkMode(dis->hDC, TRANSPARENT);
                    SetTextColor(dis->hDC, sel ? RGB(255, 255, 255) : Text);
                    HGDIOBJ old = SelectObject(dis->hDC, g_ui);
                    DrawTextW(dis->hDC, text, -1, &tr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    SelectObject(dis->hDC, old);
                }
                *result = TRUE;
                return true;
            }

            if (dis->CtlType == ODT_TAB) {
                const bool selected = (dis->itemState & ODS_SELECTED) != 0;
                const bool hot = !selected && (int)dis->itemID == g_hotTab;
                RECT r = dis->rcItem;

                RECT back = r;
                FillRect(dis->hDC, &back, g_bgBrush);

                // An underline rather than a raised lid: the page below is the
                // same colour as the strip, so a filled tab would float.
                if (selected) {
                    RECT lid = { r.left + 2, r.top + 2, r.right - 2, r.bottom };
                    RoundRectAlpha(dis->hDC, lid, 6, PanelAlt, 130, 0, 0);
                    RECT rule = { r.left + 8, r.bottom - 3, r.right - 8,
                                  r.bottom - 1 };
                    FillRoundBar(dis->hDC, rule, 1, Accent, 255);
                } else if (hot) {
                    RECT lid = { r.left + 2, r.top + 3, r.right - 2, r.bottom - 1 };
                    RoundRectAlpha(dis->hDC, lid, 6, PanelAlt, 80, 0, 0);
                }

                wchar_t text[64] = {};
                TCITEMW item = {};
                item.mask = TCIF_TEXT;
                item.pszText = text;
                item.cchTextMax = 64;
                TabCtrl_GetItem(dis->hwndItem, dis->itemID, &item);

                RECT tr = r;
                tr.bottom -= 3;             // sit above the accent rule
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, selected ? Text : (hot ? Text : TextDim));
                HGDIOBJ old = SelectObject(dis->hDC, selected ? g_bold : g_ui);
                DrawTextW(dis->hDC, text, -1, &tr,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dis->hDC, old);
                *result = TRUE;
                return true;
            }
            return false;
        }

        case WM_NOTIFY: {
            auto* hdr = (NMHDR*)lp;
            if (hdr->code != NM_CUSTOMDRAW) return false;

            if (IsClass(hdr->hwndFrom, L"msctls_trackbar32")) {
                auto* cd = (NMCUSTOMDRAW*)lp;
                if (cd->dwDrawStage == CDDS_PREPAINT) {
                    DrawTrackbar(hdr->hwndFrom, cd);
                    *result = CDRF_SKIPDEFAULT;
                    return true;
                }
                *result = CDRF_DODEFAULT;
                return true;
            }

            if (IsClass(hdr->hwndFrom, L"SysListView32")) {
                auto* cd = (NMLVCUSTOMDRAW*)lp;
                switch (cd->nmcd.dwDrawStage) {
                    case CDDS_PREPAINT:
                        *result = CDRF_NOTIFYITEMDRAW;
                        return true;
                    case CDDS_ITEMPREPAINT: {
                        const bool selected =
                            ListView_GetItemState(hdr->hwndFrom, (int)cd->nmcd.dwItemSpec,
                                                  LVIS_SELECTED) != 0;
                        cd->clrTextBk = selected ? RowSel
                                       : ((cd->nmcd.dwItemSpec % 2) ? RowAlt : Panel);
                        cd->clrText = Text;
                        *result = CDRF_DODEFAULT;
                        return true;
                    }
                }
                *result = CDRF_DODEFAULT;
                return true;
            }
            return false;
        }
    }
    return false;
}

} // namespace theme
} // namespace awa
