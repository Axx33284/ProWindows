// ProWindows - dark theme.
//
// Same palette and card language as AutoKeys. Win32 common controls have no
// dark mode we can rely on, so the look is built from three pieces: WM_CTLCOLOR*
// for backgrounds and text, NM_CUSTOMDRAW for buttons / checkboxes / lists /
// sliders, and owner-draw for combo boxes and tabs.
#pragma once
#include "common.h"
#include <commctrl.h>

namespace awa {
namespace theme {

// ---------------------------------------------------------------- palette
constexpr COLORREF Bg          = RGB(24,  25,  28);
constexpr COLORREF Panel       = RGB(32,  34,  38);
constexpr COLORREF PanelAlt    = RGB(40,  42,  47);
constexpr COLORREF Border      = RGB(56,  59,  65);
constexpr COLORREF Field       = RGB(48,  51,  58);
constexpr COLORREF Text        = RGB(233, 234, 237);
constexpr COLORREF TextDim     = RGB(150, 155, 165);
constexpr COLORREF Accent      = RGB(88,  140, 255);
constexpr COLORREF AccentHover = RGB(110, 158, 255);
constexpr COLORREF Good        = RGB(76,  200, 130);
constexpr COLORREF Warn        = RGB(240, 175, 70);
constexpr COLORREF Danger      = RGB(232, 92,  92);
constexpr COLORREF RowAlt      = RGB(36,  38,  43);
constexpr COLORREF RowSel      = RGB(52,  74,  122);

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

HBRUSH BrushBg();
HBRUSH BrushPanel();
HBRUSH BrushField();

void DarkTitleBar(HWND wnd);

// ---------------------------------------------------------------- shapes
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
