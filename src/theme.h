// ProWindows - the look, and the pieces it is drawn with.
//
// Modelled on the options screens of Star Wars Battlefront II (2017). Nothing
// there is a box on a box: the screen is black, a single hairline frame with
// two shallow notches in it holds a list of settings, and each setting is one
// full-width row - its name on the left in condensed capitals, its control on
// the right. The controls are few and always the same:
//
//   ON | OFF       two words side by side; the chosen one sits on a pale bar
//   <  VALUE  >    a value between two solid arrowheads
//   [####    65 ]  a bar filled from the left, the number in the middle
//
// The row under the pointer or the keyboard is the only colour on the screen:
// amber, a lit frame round the whole row with a glow bleeding out of it, the
// name turned amber and the chosen word on an amber bar. Disabled rows fall
// back to grey and their chosen word to a slate bar. Buttons are frames with
// their top-right corner cut off; prompts are a keycap and a word.
//
// Everything here is plain GDI and GDI+ drawing into whatever DC the caller
// hands over - the settings window, its modal screens and the search bar are
// all single custom-drawn windows with no child controls, so there is nothing
// to theme and nothing that can come out in the system's colours. The
// overlays (monitor, clock) have skins of their own and do not use any of it.
#pragma once
#include "common.h"

namespace awa {
namespace theme {

// ---------------------------------------------------------------- palette
constexpr COLORREF Bg        = RGB(7,   8,   10);    // the screen
constexpr COLORREF Panel     = RGB(10,  11,  13);    // inside the frame
constexpr COLORREF Raised    = RGB(20,  22,  26);    // a modal screen's plate
constexpr COLORREF Line      = RGB(46,  50,  56);    // the hairline between rows
constexpr COLORREF Edge      = RGB(150, 156, 164);   // the panel frame
constexpr COLORREF Text      = RGB(236, 238, 240);
constexpr COLORREF TextDim   = RGB(148, 154, 162);
constexpr COLORREF TextMute  = RGB(92,  97,  104);   // a disabled row
// The chosen word of a pair sits on a pale bar with dark type on it; on a
// disabled row the bar is slate. The slider's groove and its travelled part
// are the same two greys.
constexpr COLORREF Fill      = RGB(228, 231, 234);
constexpr COLORREF FillLow   = RGB(200, 205, 210);   // bottom of the bar's gradient
constexpr COLORREF FillText  = RGB(74,  78,  84);
constexpr COLORREF Slate     = RGB(64,  74,  88);
constexpr COLORREF SlateText = RGB(132, 140, 150);
constexpr COLORREF Track     = RGB(82,  88,  96);
// Focus. The one warm colour, and only ever where the user is.
constexpr COLORREF Amber     = RGB(255, 176, 0);
constexpr COLORREF AmberHot  = RGB(255, 206, 52);    // the chosen word's bar, lit
constexpr COLORREF AmberDeep = RGB(214, 128, 0);
constexpr COLORREF AmberText = RGB(126, 72,  0);     // type on an amber bar
constexpr COLORREF AmberGlow = RGB(255, 146, 0);     // what bleeds out of a lit frame
constexpr COLORREF AmberTrack= RGB(96,  58,  6);     // a slider's groove, lit
// Status marks.
constexpr COLORREF Good      = RGB(96,  210, 132);
constexpr COLORREF Warn      = RGB(255, 176, 0);
constexpr COLORREF Danger    = RGB(236, 76,  60);

// `a` moved `t` of the way towards `b`.
COLORREF Mix(COLORREF a, COLORREF b, float t);

void Init();
void Shutdown();

// Points the fonts and every hand-drawn size at one DPI. Font sets are cached
// per DPI and only freed by Shutdown. Each window calls this with its own DPI
// before it draws.
void SetDpi(UINT dpi);
UINT Dpi();
int  Scale(int px);            // a length at 96 dpi, in the current DPI's pixels
float ScaleF(float px);

// ---------------------------------------------------------------- type
// Bahnschrift is a DIN, which is what the game's menus are set in; it ships
// with Windows 10 and 11. Condensed capitals for everything that is a label,
// Segoe UI for text that has to be read rather than recognised. On a machine
// without Bahnschrift the capitals fall back to Segoe UI.
enum class Font {
    Body,        // Segoe UI - descriptions, paths, anything the user typed
    BodyBold,
    Small,       // Segoe UI, a size down - the second line of a result
    Label,       // a row's name                     SemiBold SemiCondensed
    Value,       // an unchosen word, a value         SemiBold SemiCondensed
    ValueLight,  // the chosen word on its pale bar   SemiCondensed
    Crumb,       // "PROWINDOWS /"                    SemiCondensed, large
    CrumbBold,   // "LAYOUT"                          SemiBold SemiCondensed, large
    Nav,         // the category column
    Section,     // a group's heading in the list     SemiBold SemiCondensed, small
    Caption,     // status lines, prompts             SemiCondensed, small
    Title,       // the description panel's heading
    Button,
    Key,         // the letters in a keycap
    Query,       // what is typed into the search bar
    Count
};
HFONT Get(Font f);

std::wstring Caps(const std::wstring& text);

// Text with extra space between the letters. `tracking` is in pixels at the
// current DPI; `format` is DrawText's.
void DrawSpaced(HDC dc, const std::wstring& text, RECT* r, UINT format, int tracking);
int  SpacedWidth(HDC dc, const std::wstring& text, int tracking);

// One call for the common case: select `font`, set the colour, draw, restore.
void Print(HDC dc, Font font, const std::wstring& text, RECT r, COLORREF color,
           UINT format, int tracking = 0);
int  Measure(HDC dc, Font font, const std::wstring& text, int tracking = 0);
// Word-wrapped body text; returns the height it took. `measureOnly` draws nothing.
int  PrintWrapped(HDC dc, Font font, const std::wstring& text, RECT r, COLORREF color,
                  bool measureOnly = false);

// ---------------------------------------------------------------- the screen
// The backdrop: near-black, a little lighter towards the top left, with a
// vignette and enough noise that the gradient does not band. Rendered once per
// canvas size and blitted; `r` shows the top-left part of a `canvas`-sized
// picture, so a window that grows and shrinks keeps the same one.
void PaintBackdrop(HDC dc, const RECT& r, SIZE canvas);
// Lets the rendered pictures go (called when a window closes and on idle trim).
void TrimSurfaces();

void DarkTitleBar(HWND wnd);

// ---------------------------------------------------------------- shapes
// A translucent fill over whatever is already on `dc`.
void Wash(HDC dc, const RECT& r, COLORREF color, BYTE alpha);
// A horizontal wash fading from `alphaLeft` to `alphaRight`.
void Sweep(HDC dc, const RECT& r, COLORREF color, BYTE alphaLeft, BYTE alphaRight);
// A vertical gradient, opaque.
void Gradient(HDC dc, const RECT& r, COLORREF top, COLORREF bottom);
// A vertical wash fading from `alphaTop` to `alphaBottom` - the soft edge
// where a scrolled list runs under the frame.
void FadeV(HDC dc, const RECT& r, COLORREF color, BYTE alphaTop, BYTE alphaBottom);
// A square frame `width` pixels thick, drawn inside `r`.
void Frame(HDC dc, const RECT& r, COLORREF color, BYTE alpha, int width = 1);
// Light bleeding out of (and a little into) the edges of `r`, fading to
// nothing over `spread` pixels.
void Glow(HDC dc, const RECT& r, COLORREF color, int spread, BYTE peak);
// An elliptical pool of light filling `r`, brightest in the middle.
void Haze(HDC dc, const RECT& r, COLORREF color, BYTE peak);
// A solid arrowhead pointing left (-1) or right (+1), up (-2) or down (+2).
void Arrowhead(HDC dc, float cx, float cy, float size, int dir, COLORREF color, BYTE alpha = 255);
// A stroked chevron, the same directions.
void Chevron(HDC dc, float cx, float cy, float size, int dir, COLORREF color, float width);
// A small diamond - the status mark.
void Diamond(HDC dc, float cx, float cy, float r, COLORREF color);

// The panel's outline: a frame whose top edge dips in a shallow notch to the
// right of centre and whose bottom edge rises in one to the left, the way the
// game draws its options panel. `inset` is how far the notches go in.
void PanelFrame(HDC dc, const RECT& r, COLORREF color, BYTE alpha, float width, int inset);
// How much of the panel's top and bottom the notches take, so content can be
// kept clear of them.
int  PanelNotch();

// A rectangle with its top-right corner cut at 45 degrees - the game's
// buttons. `fill` is skipped when fillAlpha is 0.
void CutBox(HDC dc, const RECT& r, int cut, COLORREF fill, BYTE fillAlpha,
            COLORREF edge, BYTE edgeAlpha, float width);

// ---------------------------------------------------------------- widgets
// Shared by everything that shows settings-style rows: the settings list, the
// modal screens and the search bar.

// How a control is being shown.
struct Look {
    bool  enabled = true;
    float lit     = 0.0f;   // 0 = at rest, 1 = the row has focus (animated between)
    int   hot     = -1;     // which part is under the pointer (control-specific)
    bool  pressed = false;
};

// The amber frame, glow and wash over a row that has focus. `t` fades it in.
void RowFocus(HDC dc, const RECT& row, float t);

// Two words side by side, `chosen` (0 or 1) on a bar.
void DrawPair(HDC dc, const RECT& r, const std::wstring& first, const std::wstring& second,
              int chosen, const Look& look);
// A value between two arrowheads. `hot`: 0 the left arrow, 2 the right one.
void DrawSelector(HDC dc, const RECT& r, const std::wstring& text, const Look& look,
                  bool canBack = true, bool canForward = true, COLORREF swatch = CLR_INVALID);
// A bar filled `fraction` of the way, `text` in the middle.
void DrawSlider(HDC dc, const RECT& r, float fraction, const std::wstring& text,
                const Look& look);
// A button in the control column: a cut frame with a word in it.
void DrawAction(HDC dc, const RECT& r, const std::wstring& text, const Look& look,
                bool danger = false);

// A key, as the game draws a button prompt: the name in a small outlined box.
// Returns its width. `ink` is the outline and the letters; `solid` fills the
// box with `ink` and prints the letters dark.
int  Keycap(HDC dc, int x, int centreY, const std::wstring& key, COLORREF ink,
            bool measureOnly = false, bool solid = false, bool large = false);
// A whole chord - "Win + Shift + H" - as a row of keycaps, laid out from `x`
// (or ending at `x` when `alignRight`). Returns the width.
// `large` is the size a settings row shows a shortcut at; prompts use the small one.
int  Chord(HDC dc, int x, int centreY, UINT mods, UINT vk, COLORREF ink,
           bool alignRight, bool measureOnly = false, bool large = false);
// The name of a key the way a keycap prints it.
std::wstring KeyName(UINT vk);

// A footer button: a cut frame, the label in capitals, an optional keycap in
// front of it. `primary` is the action that commits something.
struct ButtonLook {
    bool hot = false, pressed = false, enabled = true, primary = false, focused = false;
};
void DrawButton(HDC dc, const RECT& r, const std::wstring& label, const wchar_t* key,
                const ButtonLook& look);
int  ButtonWidth(HDC dc, const std::wstring& label, const wchar_t* key);

// A prompt in the footer - keycap then word - drawn from `x`. Returns its width.
int  Prompt(HDC dc, int x, int centreY, const std::wstring& key, const std::wstring& word,
            COLORREF ink, bool measureOnly = false);

// The mark: a tiled-window arrangement, master pane amber. Drawn in `r`,
// `ink` for the other two panes.
void Mark(HDC dc, const RECT& r, COLORREF ink, COLORREF master);

} // namespace theme
} // namespace awa
