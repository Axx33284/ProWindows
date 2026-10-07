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
// The row under the pointer or the keyboard is the only thing lit: a bar of
// brushed metal over the whole row, its name turned white. Disabled rows fall
// back to grey. Prompts are a keycap and a word.
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
// The Requiem set (PLAN-1.6 2.2): a black screen, grey hairlines, white type and
// a brushed-metal focus bar. The one colour is the meter's green.
constexpr COLORREF Bg        = RGB(0,   0,   0);     // the screen
constexpr COLORREF Line      = RGB(40,  40,  40);    // hairline between rows, under the bars
constexpr COLORREF Rule      = RGB(92,  92,  92);    // vertical separators between tabs
constexpr COLORREF Text      = RGB(232, 232, 232);   // row labels, values
constexpr COLORREF TextHi    = RGB(255, 255, 255);   // focused row's label, active tab
constexpr COLORREF TextDim   = RGB(168, 168, 168);   // inactive tabs, section labels, chevrons
constexpr COLORREF TextBody  = RGB(205, 205, 205);   // the description panel
constexpr COLORREF TextMute  = RGB(96,  96,  96);    // a disabled row
constexpr COLORREF SegOn     = RGB(225, 225, 225);   // the chosen value segment
constexpr COLORREF SegOff    = RGB(78,  78,  78);
constexpr COLORREF SegOffDis = RGB(110, 110, 110);   // segments of a disabled row
constexpr COLORREF Plate     = RGB(30,  30,  30);    // section plate gradient, top ...
constexpr COLORREF PlateLow  = RGB(22,  22,  22);    // ... and bottom
// The focus bar's gradient stops, at 0 %, 18 %, 60 % and 100 % of its height.
constexpr COLORREF Metal0    = RGB(88,  88,  88);
constexpr COLORREF Metal1    = RGB(58,  58,  58);
constexpr COLORREF Metal2    = RGB(38,  38,  38);
constexpr COLORREF Metal3    = RGB(30,  30,  30);
constexpr COLORREF MetalEdge = RGB(150, 150, 150);   // the bar's outline
constexpr COLORREF KeyFill   = RGB(205, 205, 205);   // a keycap
constexpr COLORREF KeyInk    = RGB(20,  20,  20);
constexpr COLORREF MeterFill = RGB(140, 220, 160);   // the one colour
constexpr COLORREF MeterEdge = RGB(130, 130, 130);
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
    // Requiem (PLAN-1.6 2.3). Squared Bahnschrift for the title, the tabs and
    // keycap letters; Segoe UI in sentence case for everything else.
    Heading,     // the screen's title                  Bahnschrift SemiCondensed, 26 DIP
    Tab,         // a tab, capitals                      Bahnschrift SemiCondensed, 17 DIP
    Row,         // a row's label and value              Segoe UI (Variable), 15 DIP
    Desc,        // the description panel                Segoe UI (Variable), 16 DIP
    Plate,       // a section plate's label              Segoe UI (Variable), 14 DIP
    Prompt,      // a footer prompt's word               Segoe UI (Variable), 16 DIP
    Keycap,      // the letters in a keycap              Bahnschrift SemiCondensed
    Body,        // Segoe UI - paths, anything the user typed
    Small,       // Segoe UI, a size down - the second line of a result
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

// The brushed-metal bar over a row that has focus: gradient, sheen, highlight,
// light outline. `t` fades it in.
void RowFocus(HDC dc, const RECT& row, float t);

// A value between two thin chevrons, with one segment per option under it
// (`count` options, `index` chosen; a track and thumb above 8; nothing when
// `count` is 0). `hot`: 0 the left chevron, 2 the right one.
void DrawSelector(HDC dc, const RECT& r, const std::wstring& text, const Look& look,
                  bool canBack = true, bool canForward = true, COLORREF swatch = CLR_INVALID,
                  int count = 0, int index = 0);
// A Choice with two options, "Off" and "On".
void DrawToggle(HDC dc, const RECT& r, bool on, const Look& look);
// A ruler with a marker at `fraction`, - and + at its ends, `text` at the right.
// `hot`: 0 the left chevron, 1 the -, 3 the +, 2 the right chevron.
void DrawSlider(HDC dc, const RECT& r, float fraction, const std::wstring& text,
                const Look& look);
// A row that opens something: the word, and the open-in icon at the right edge.
void DrawAction(HDC dc, const RECT& r, const std::wstring& text, const Look& look,
                bool danger = false);

// A key, as the game draws a button prompt: the name in a solid light box with
// dark letters, at least 20 x 20 DIP. Returns its width. `ink` is only used by
// the legacy callers and is ignored; `solid` and `large` pick the pressed look
// and the bigger size.
int  Keycap(HDC dc, int x, int centreY, const std::wstring& key, COLORREF ink,
            bool measureOnly = false, bool solid = false, bool large = false);
// A whole chord - "Win + Shift + H" - as a row of keycaps, laid out from `x`
// (or ending at `x` when `alignRight`). Returns the width.
// `large` is the size a settings row shows a shortcut at; prompts use the small one.
int  Chord(HDC dc, int x, int centreY, UINT mods, UINT vk, COLORREF ink,
           bool alignRight, bool measureOnly = false, bool large = false);
// The name of a key the way a keycap prints it.
std::wstring KeyName(UINT vk);

// A prompt in the footer - keycap then word - drawn from `x`. Returns its width.
// A section's plate: a slanted dark slab with the label inset, and a hairline
// running on from it to the right edge of `r` (the row's full width).
void SectionPlate(HDC dc, const RECT& r, const std::wstring& label);
int  Prompt(HDC dc, int x, int centreY, const std::wstring& key, const std::wstring& word,
            COLORREF ink, bool measureOnly = false);

// The mark: a tiled-window arrangement, master pane silver. Drawn in `r`,
// `ink` for the other two panes.
void Mark(HDC dc, const RECT& r, COLORREF ink, COLORREF master);

// A meter bar in `r` (220 x 10 DIP in the settings window): a 1 px MeterEdge
// frame, a MeterFill gradient for `used` (0..1) and a hatched part for `other`
// (0..1, drawn after `used`).
void Meter(HDC dc, const RECT& r, float used, float other);

} // namespace theme
} // namespace awa
