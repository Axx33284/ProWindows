// ProWindows - a list of settings rows, the way the game's options screens are
// built: one setting per full-width row, its name on the left and its control
// on the right, the row with focus lit amber.
//
// A row is data: what it is called, what it says in the description panel,
// and a get/set pair onto whatever it edits. The list owns layout, painting,
// scrolling, the focus and its animation, and every keystroke and click on a
// row. It is not a window - the settings window and the modal screens host it
// inside their own paint and input handling and give it a rectangle.
//
// Keys, the way the game takes them: Up/Down move the focus, Left/Right change
// the value, Enter selects, Delete clears or removes, Ctrl+Up/Down moves a
// row that can be reordered. The mouse lights whatever it is over.
#pragma once
#include "common.h"
#include <functional>

namespace awa {
namespace ui {

enum class Kind {
    Section,    // a group's heading; not focusable
    Toggle,     // two words, one chosen: get() is 0 for the first, 1 for the second
    Choice,     // < value >: get() is an index into `options`
    Slider,     // a bar: get() is the value between lo and hi
    Keys,       // a chord: get() is MAKELONG(vk, mods); Enter asks for a new one
    Action,     // a button in the control column
    Colour,     // < swatch name >: get() is an index into `options` / `colours`
    Item,       // an entry in a list the user builds (an excluded app, a folder)
    Info,       // a label and a value, read-only
    Page,       // marks where a page (sub-tab) of a category starts; never drawn in the list
};

struct Row {
    Kind kind = Kind::Info;
    std::wstring id;             // stable identity: the focus survives a rebuild by it
    std::wstring label;          // drawn in capitals unless `raw`
    bool raw = false;            // user text - a program, a path: drawn as typed
    std::wstring detail;         // a second line under a raw label
    std::wstring help;           // what the description panel says about it
    std::wstring page;           // which category it belongs to (set by the shell)

    std::function<bool()>    enabled;    // null = always
    std::function<int()>     get;
    std::function<void(int)> set;
    std::function<bool()>    modified;   // differs from what is saved; null = never
    // The value the row has in a default Config, formatted as the row shows
    // it; the right column prints "(Default: ...)". Null = omitted. The
    // Toggle / Slider / ChoiceOf helpers fill it when their field lies inside
    // the source given to SetDefaultsSource.
    std::function<std::wstring()> fallback;

    // Toggle: the two words. Choice: the values' names. Colour: the swatches' names.
    std::vector<std::wstring> options;
    std::vector<COLORREF> colours;       // Colour: one per option
    bool wrap = true;                    // Choice: past the end comes back round

    // Slider.
    int lo = 0, hi = 100, step = 1, bigStep = 10;
    std::function<std::wstring(int)> format;

    // Action / Item: the word on the button, and what it does.
    std::wstring button;
    bool danger = false;
    std::function<void()> activate;      // Enter, Space, a click on the button
    std::function<void()> remove;        // Delete
    // One more key the row answers to, shown in the prompt bar ("F2 PROGRAM").
    UINT extraKey = 0;
    std::wstring extraWord;
    std::function<void()> extra;

    // Info: the value shown on the right.
    std::function<std::wstring()> value;
    bool rawValue = false;

    // A short word after the label - "BLOCKED", "HOOK" - and its colour.
    std::function<std::wstring()> tag;
    COLORREF tagColor = RGB(236, 76, 60);

    // Rows that can be reordered: the same non-zero group, and a move that
    // lifts the row at `from` and reinserts it at `to` (both in-group indices).
    int orderGroup = 0;
    int orderIndex = -1;
    std::function<void(int from, int to)> move;

    bool Enabled() const { return !enabled || enabled(); }
    bool Focusable() const { return kind != Kind::Section && kind != Kind::Page && Enabled(); }
};

// Helpers for the common shapes. `field` points into an edit buffer that
// outlives the rows; `saved` is the same field in the saved config, for the
// modified mark. Both are captured by pointer.
Row Toggle(const std::wstring& id, const std::wstring& label, const std::wstring& help,
           bool* field, const bool* saved,
           const wchar_t* onWord = L"On", const wchar_t* offWord = L"Off");
Row Slider(const std::wstring& id, const std::wstring& label, const std::wstring& help,
           int* field, const int* saved, int lo, int hi, int step, int bigStep,
           const wchar_t* unit);
// A choice over a list of values: shows names[i] while *field == values[i]; a
// value not in the list shows as the nearest one at or above it.
Row ChoiceOf(const std::wstring& id, const std::wstring& label, const std::wstring& help,
             int* field, const int* saved, std::vector<int> values,
             std::vector<std::wstring> names);
// Where the helpers find a default for a field: `edit` is the struct the rows'
// fields point into, `defaults` a default-constructed copy of the same type.
void SetDefaultsSource(const void* edit, const void* defaults, size_t size);
Row Section(const std::wstring& label);
// Starts a page of the category; its id is "page:<label>", stable across rebuilds.
Row Page(const std::wstring& label);
Row Action(const std::wstring& id, const std::wstring& label, const std::wstring& help,
           const std::wstring& button, std::function<void()> run);
Row Info(const std::wstring& id, const std::wstring& label, const std::wstring& help,
         std::function<std::wstring()> value, bool rawValue = false);

class RowList {
public:
    // The host is told when to repaint, when a value changed, when the focus
    // moved and when a Keys row wants a new chord. All optional.
    std::function<void()>    onInvalidate;
    std::function<void()>    onChanged;
    std::function<void()>    onFocus;
    std::function<void(int)> onCapture;

    // Replaces the rows. With `keepFocus` the focus stays on the row with the
    // same id (or the nearest one after it) and the scroll stays put.
    void SetRows(std::vector<Row> rows, bool keepFocus);
    const std::vector<Row>& Rows() const { return rows_; }
    std::vector<Row>& MutableRows() { return rows_; }

    // Where the rows go, and the strip to the right of it for the scrollbar.
    void SetBounds(const RECT& rows, const RECT& scrollTrack);
    const RECT& Bounds() const { return bounds_; }

    // Whether the list has the keyboard. A list without it shows no lit row
    // unless the pointer is over one.
    void SetActive(bool active);
    bool Active() const { return active_; }

    void Paint(HDC dc, int offsetY = 0);

    bool Key(UINT vk, bool ctrl, bool shift);
    void MouseMove(POINT pt, bool buttonDown);
    bool MouseDown(POINT pt);          // true when the list took the press
    void MouseUp(POINT pt);
    void MouseLeave();
    void Wheel(int delta);
    bool Dragging() const { return drag_ != Drag::None; }
    bool Contains(POINT pt) const;

    // Advances the animations; true while anything is still moving.
    bool Tick();

    int  Focus() const { return focus_; }
    const Row* FocusedRow() const;
    void SetFocus(int index, bool scroll = true);
    void FocusFirst();
    void FocusById(const std::wstring& id);

    // A Keys row waiting for a chord: what to show in it meanwhile.
    void SetCapture(int row, UINT heldMods, const std::wstring& note);
    int  CaptureRow() const { return captureRow_; }

    // The prompt bar for the focused row: pairs of key and word.
    std::vector<std::pair<std::wstring, std::wstring>> Prompts() const;

private:
    enum class Drag { None, Slider, Order, Thumb };

    struct Slot { int top = 0, height = 0; };
    void Layout();
    int  RowAt(POINT pt) const;             // -1 when none
    RECT RowRect(int i) const;              // screen space, scroll applied
    RECT ControlRect(int i) const;
    int  ControlWidth() const;
    int  PartAt(int i, POINT pt) const;     // which part of the control
    void Step(int i, int delta, bool big);
    void SetValue(int i, int v);
    void Changed();
    void Invalidate() const { if (onInvalidate) onInvalidate(); }
    void EnsureVisible(int i, bool animate);
    int  MaxScroll() const;
    void Activate(int i);
    void MoveInGroup(int i, int delta);
    int  NextFocusable(int from, int dir) const;
    void SliderFromX(int i, int x);
    RECT ThumbRect() const;
    void PaintRow(HDC dc, int i, const RECT& r, float lit);

    std::vector<Row>   rows_;
    std::vector<Slot>  slots_;
    std::vector<float> lit_;          // per row, animated towards 1 for the focus
    RECT bounds_ = {}, track_ = {};
    int  content_ = 0;                // total height of the rows
    float scroll_ = 0.0f;             // current, animated
    float scrollTarget_ = 0.0f;
    int  focus_ = -1;
    int  hover_ = -1, hoverPart_ = -1;
    bool active_ = true;
    bool pressed_ = false;
    int  pressRow_ = -1, pressPart_ = -1;
    Drag drag_ = Drag::None;
    int  dragRow_ = -1;
    int  thumbGrab_ = 0;
    int  captureRow_ = -1;
    UINT captureHeld_ = 0;
    std::wstring captureNote_;
    ULONGLONG lastTick_ = 0;
};

} // namespace ui
} // namespace awa
