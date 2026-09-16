// ProWindows - the settings window's look.
//
// Modelled on DOOM Eternal's menus: near-black surfaces with a faint diagonal
// grain, one hot orange for everything selected or important, condensed
// capitals for headings, and panels with their corners cut rather than
// rounded. The tab row is a run of slanted plates, the active one filled.
// Win32 common controls have no dark mode we can rely on, so the look is
// built from three pieces: WM_CTLCOLOR* for backgrounds and text,
// NM_CUSTOMDRAW for lists and sliders, and owner-draw for buttons, check
// boxes, combo boxes and tabs. The overlays (monitor, clock) have themes of
// their own and are not touched by any of this.
#pragma once
#include "common.h"
#include <commctrl.h>

namespace awa {
namespace theme {

// ---------------------------------------------------------------- palette
constexpr COLORREF Bg          = RGB(13,  14,  16);
constexpr COLORREF Panel       = RGB(22,  24,  27);
constexpr COLORREF PanelAlt    = RGB(33,  36,  40);
constexpr COLORREF Border      = RGB(58,  62,  68);
constexpr COLORREF Field       = RGB(27,  29,  33);
constexpr COLORREF Text        = RGB(232, 228, 220);   // warm off-white
constexpr COLORREF TextDim     = RGB(142, 148, 154);
constexpr COLORREF Accent      = RGB(245, 146, 30);    // the orange
constexpr COLORREF AccentHover = RGB(255, 170, 62);
// Text set on top of the accent: the selected menu item in the game is dark
// on orange, not white on orange.
constexpr COLORREF AccentText  = RGB(20,  16,  10);
constexpr COLORREF Good        = RGB(122, 200, 108);
constexpr COLORREF Warn        = RGB(245, 197, 66);
constexpr COLORREF Danger      = RGB(214, 58,  42);
constexpr COLORREF RowAlt      = RGB(26,  28,  32);
constexpr COLORREF RowSel      = RGB(82,  50,  14);    // orange, dimmed to a row

void Init();
void Shutdown();

// Points the fonts and every hand-drawn size at one DPI. Called for you by
// PrepareDialog and on WM_DPICHANGED; the launcher, which is not a dialog,
// calls it itself before it lays out. Font sets are cached per DPI and only
// freed by Shutdown, so a control that is still holding one stays valid.
void SetDpi(UINT dpi);
UINT Dpi();

HFONT FontUI();
HFONT FontBold();
HFONT FontTitle();
HFONT FontSmall();
HFONT FontNumber();       // larger semibold, for the monitor readouts
// The condensed capitals: card titles, tabs and buttons in Heading, the
// window's own name in Display. Bahnschrift ships with Windows 10 and 11;
// on a machine without it these are Segoe UI Semibold, which is plainer but
// reads the same.
HFONT FontHeading();
HFONT FontDisplay();

// Capitals, for the places the design sets everything in them.
std::wstring Caps(const std::wstring& text);

HBRUSH BrushBg();
HBRUSH BrushPanel();
HBRUSH BrushField();

void DarkTitleBar(HWND wnd);

// ---------------------------------------------------------------- shapes
// A rectangle with its top-left and bottom-right corners cut at 45 degrees,
// which is the panel shape the whole look is built from. `cut` is the size
// of the cut in pixels; 0 is a plain rectangle.
void Chamfer(HDC dc, const RECT& r, int cut, COLORREF fill, BYTE alpha,
             COLORREF borderColor, BYTE borderAlpha);
// A parallelogram leaning right by `slant` pixels - the tab plate.
void Slant(HDC dc, const RECT& r, int slant, COLORREF fill, BYTE alpha,
           COLORREF borderColor, BYTE borderAlpha);

// Antialiased rounded rectangle. `borderColor` is skipped when fully clear.
void RoundRect(HDC dc, const RECT& r, int radius, COLORREF fill,
               COLORREF borderColor, bool drawBorder);
void RoundRectAlpha(HDC dc, const RECT& r, int radius, COLORREF fill, BYTE alpha,
                    COLORREF borderColor, BYTE borderAlpha);
void FillRoundBar(HDC dc, const RECT& r, int radius, COLORREF color, BYTE alpha);

// ---------------------------------------------------------------- dialogs
// Call once from WM_INITDIALOG: applies fonts and the styles the drawing needs.
// Every GROUPBOX in the template is turned into a painted card - the control
// itself is hidden and its rectangle becomes the card, so the .rc stays the
// single source of layout truth.
void PrepareDialog(HWND dlg);

// Paints a Panel-coloured strip across the top of a dialog (the shell header).
void SetHeaderHeight(HWND dlg, int height);

void ForgetDialog(HWND dlg);   // call from WM_DESTROY

// Call at the top of every themed dialog proc. Returns true when it handled the
// message, in which case *result is what the proc should return.
bool DialogMessage(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, INT_PTR* result);

// Cards are painted by the page itself, under its controls.
void DrawCard(HDC dc, const RECT& r, const wchar_t* title);

} // namespace theme
} // namespace awa
