// ProWindows - a list of settings rows. See rowlist.h.
#include "rowlist.h"
#include "theme.h"
#include <cmath>
#include <cstring>

namespace awa {
namespace ui {

using theme::Font;

// ================================================================ row helpers
namespace {
const char* g_editBase = nullptr;
const char* g_defBase  = nullptr;
size_t      g_defSize  = 0;

// The default int / bool for a field of the edit struct, or false when the
// field is not in it.
bool DefaultOffset(const void* field, size_t* off) {
    const char* f = (const char*)field;
    if (!g_editBase || f < g_editBase || f >= g_editBase + g_defSize) return false;
    *off = (size_t)(f - g_editBase);
    return true;
}
int DefaultInt(size_t off)  { int v = 0; memcpy(&v, g_defBase + off, sizeof v); return v; }
bool DefaultBool(size_t off) { return *(const bool*)(g_defBase + off); }
} // namespace

void SetDefaultsSource(const void* edit, const void* defaults, size_t size) {
    g_editBase = (const char*)edit;
    g_defBase  = (const char*)defaults;
    g_defSize  = size;
}

Row Toggle(const std::wstring& id, const std::wstring& label, const std::wstring& help,
           bool* field, const bool* saved, const wchar_t* onWord, const wchar_t* offWord) {
    Row r;
    r.kind  = Kind::Toggle;
    r.id    = id;
    r.label = label;
    r.help  = help;
    r.options = { onWord, offWord };
    r.get = [field]() { return *field ? 0 : 1; };
    r.set = [field](int v) { *field = (v == 0); };
    if (saved) r.modified = [field, saved]() { return *field != *saved; };
    size_t off;
    if (DefaultOffset(field, &off)) {
        const std::wstring on = onWord, offw = offWord;
        r.fallback = [off, on, offw]() { return DefaultBool(off) ? on : offw; };
    }
    return r;
}

Row Slider(const std::wstring& id, const std::wstring& label, const std::wstring& help,
           int* field, const int* saved, int lo, int hi, int step, int bigStep,
           const wchar_t* unit) {
    Row r;
    r.kind  = Kind::Slider;
    r.id    = id;
    r.label = label;
    r.help  = help;
    r.lo = lo; r.hi = hi; r.step = step; r.bigStep = bigStep;
    r.get = [field, lo, hi]() { return (std::max)(lo, (std::min)(hi, *field)); };
    r.set = [field](int v) { *field = v; };
    if (saved) r.modified = [field, saved]() { return *field != *saved; };
    const std::wstring u = unit ? unit : L"";
    r.format = [u](int v) {
        if (u.empty()) return std::to_wstring(v);
        if (u == L"%") return std::to_wstring(v) + L"%";
        return std::to_wstring(v) + L" " + u;
    };
    size_t off;
    if (DefaultOffset(field, &off)) {
        auto fmt = r.format;
        r.fallback = [off, lo, hi, fmt]() { return fmt((std::max)(lo, (std::min)(hi, DefaultInt(off)))); };
    }
    return r;
}

Row ChoiceOf(const std::wstring& id, const std::wstring& label, const std::wstring& help,
             int* field, const int* saved, std::vector<int> values,
             std::vector<std::wstring> names) {
    Row r;
    r.kind    = Kind::Choice;
    r.id      = id;
    r.label   = label;
    r.help    = help;
    r.options = names;
    r.wrap    = false;
    r.get = [field, values]() {
        for (size_t i = 0; i < values.size(); ++i)
            if (values[i] == *field) return (int)i;
        // Not one of ours - a hand-edited file. Show the nearest at or above
        // it, so nothing reads smaller than what is really set.
        for (size_t i = 0; i < values.size(); ++i)
            if (values[i] >= *field) return (int)i;
        return values.empty() ? 0 : (int)values.size() - 1;
    };
    r.set = [field, values](int v) {
        if (v >= 0 && v < (int)values.size()) *field = values[(size_t)v];
    };
    if (saved) r.modified = [field, saved]() { return *field != *saved; };
    size_t off;
    if (DefaultOffset(field, &off)) {
        r.fallback = [off, values, names]() -> std::wstring {
            const int d = DefaultInt(off);
            for (size_t i = 0; i < values.size() && i < names.size(); ++i)
                if (values[i] >= d) return names[i];
            return names.empty() ? std::wstring() : names.back();
        };
    }
    return r;
}

Row Section(const std::wstring& label) {
    Row r;
    r.kind  = Kind::Section;
    r.id    = L"section:" + label;
    r.label = label;
    return r;
}

Row Page(const std::wstring& label) {
    Row r;
    r.kind  = Kind::Page;
    r.id    = L"page:" + label;
    r.label = label;
    return r;
}

Row Action(const std::wstring& id, const std::wstring& label, const std::wstring& help,
           const std::wstring& button, std::function<void()> run) {
    Row r;
    r.kind     = Kind::Action;
    r.id       = id;
    r.label    = label;
    r.help     = help;
    r.button   = button;
    r.activate = std::move(run);
    return r;
}

Row Info(const std::wstring& id, const std::wstring& label, const std::wstring& help,
         std::function<std::wstring()> value, bool rawValue) {
    Row r;
    r.kind     = Kind::Info;
    r.id       = id;
    r.label    = label;
    r.help     = help;
    r.value    = std::move(value);
    r.rawValue = rawValue;
    return r;
}

// ================================================================ metrics
namespace {

int RowHeight(const Row& r) {
    if (r.kind == Kind::Page) return 0;
    if (r.kind == Kind::Section) return theme::Scale(42);
    if (r.raw && !r.detail.empty()) return theme::Scale(50);
    return theme::Scale(46);
}
int PadLeft()      { return theme::Scale(24); }
int PadRight()     { return theme::Scale(20); }
int ControlHeight(){ return theme::Scale(30); }

} // namespace

int RowList::ControlWidth() const {
    const int w = bounds_.right - bounds_.left;
    return (std::max)(theme::Scale(180), (std::min)(theme::Scale(300), w * 44 / 100));
}

void RowList::Layout() {
    slots_.resize(rows_.size());
    int y = theme::Scale(2);
    for (size_t i = 0; i < rows_.size(); ++i) {
        slots_[i].top    = y;
        slots_[i].height = RowHeight(rows_[i]);
        y += slots_[i].height;
    }
    content_ = y + theme::Scale(6);
    lit_.resize(rows_.size(), 0.0f);
    scrollTarget_ = (std::max)(0.0f, (std::min)(scrollTarget_, (float)MaxScroll()));
    scroll_       = (std::max)(0.0f, (std::min)(scroll_, (float)MaxScroll()));
}

int RowList::MaxScroll() const {
    return (std::max)(0, content_ - (int)(bounds_.bottom - bounds_.top));
}

RECT RowList::RowRect(int i) const {
    RECT r = { bounds_.left, 0, bounds_.right, 0 };
    if (i < 0 || i >= (int)slots_.size()) return r;
    r.top    = bounds_.top + slots_[(size_t)i].top - (int)(scroll_ + 0.5f);
    r.bottom = r.top + slots_[(size_t)i].height;
    return r;
}

RECT RowList::ControlRect(int i) const {
    const RECT r = RowRect(i);
    const Row& row = rows_[(size_t)i];
    const int ch = ControlHeight();
    const int cy = (r.top + r.bottom) / 2;
    int cw = ControlWidth();
    if (row.kind == Kind::Item) cw = (std::min)(cw, theme::Scale(118));
    return { r.right - PadRight() - cw, cy - ch / 2, r.right - PadRight(), cy - ch / 2 + ch };
}

int RowList::RowAt(POINT pt) const {
    if (pt.x < bounds_.left || pt.x >= bounds_.right ||
        pt.y < bounds_.top  || pt.y >= bounds_.bottom) return -1;
    for (int i = 0; i < (int)rows_.size(); ++i) {
        const RECT r = RowRect(i);
        if (pt.y >= r.top && pt.y < r.bottom) return i;
    }
    return -1;
}

bool RowList::Contains(POINT pt) const {
    return PtInRect(&bounds_, pt) || PtInRect(&track_, pt);
}

// Which part of a row's control is under `pt`: for a pair, the word (0 / 1);
// for a selector, the left arrow (0), the value (1) or the right arrow (2);
// 0 for anything else that is a single target; -1 outside it. -2 is the grip
// of a row that can be dragged into a new place.
int RowList::PartAt(int i, POINT pt) const {
    if (i < 0 || i >= (int)rows_.size()) return -1;
    const Row& row = rows_[(size_t)i];
    const RECT r = RowRect(i);
    if (row.orderGroup && pt.x >= r.left && pt.x < r.left + PadLeft()) return -2;
    RECT c = ControlRect(i);
    InflateRect(&c, theme::Scale(4), theme::Scale(6));
    if (!PtInRect(&c, pt)) return -1;
    switch (row.kind) {
        case Kind::Toggle:
            return pt.x < (c.left + c.right) / 2 ? 0 : 1;
        case Kind::Choice:
        case Kind::Colour: {
            const int arrow = ControlHeight() + theme::Scale(6);
            if (pt.x < c.left + arrow)  return 0;
            if (pt.x >= c.right - arrow) return 2;
            return 1;
        }
        case Kind::Info:
            return -1;
        default:
            return 0;
    }
}

// ================================================================ rows
void RowList::SetRows(std::vector<Row> rows, bool keepFocus) {
    std::wstring focusId;
    int oldFocus = focus_;
    std::unordered_map<std::wstring, float> lit;
    if (keepFocus) {
        if (focus_ >= 0 && focus_ < (int)rows_.size()) focusId = rows_[(size_t)focus_].id;
        for (size_t i = 0; i < rows_.size() && i < lit_.size(); ++i)
            if (lit_[i] > 0.0f) lit[rows_[i].id] = lit_[i];
    } else {
        scroll_ = scrollTarget_ = 0.0f;
    }
    rows_ = std::move(rows);
    lit_.assign(rows_.size(), 0.0f);
    for (size_t i = 0; i < rows_.size(); ++i) {
        auto it = lit.find(rows_[i].id);
        if (it != lit.end()) lit_[i] = it->second;
    }
    Layout();
    hover_ = -1;
    hoverPart_ = -1;
    pressed_ = false;
    if (captureRow_ >= (int)rows_.size()) captureRow_ = -1;

    focus_ = -1;
    if (keepFocus && !focusId.empty()) {
        for (int i = 0; i < (int)rows_.size(); ++i)
            if (rows_[(size_t)i].id == focusId) { focus_ = i; break; }
        if (focus_ < 0 && oldFocus >= 0) {
            // Gone - removed from a list. Take whatever now sits where it was.
            const int from = (std::min)(oldFocus, (int)rows_.size() - 1);
            focus_ = NextFocusable(from - 1, +1);
            if (focus_ < 0) focus_ = NextFocusable(from + 1, -1);
        }
    }
    if (focus_ < 0) focus_ = NextFocusable(-1, +1);
    if (focus_ >= 0 && keepFocus) EnsureVisible(focus_, false);
    Invalidate();
}

void RowList::SetBounds(const RECT& rows, const RECT& scrollTrack) {
    const bool resized = !EqualRect(&rows, &bounds_);
    bounds_ = rows;
    track_  = scrollTrack;
    if (resized) {
        Layout();
        if (focus_ >= 0) EnsureVisible(focus_, false);
    }
}

void RowList::SetActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    Invalidate();
}

const Row* RowList::FocusedRow() const {
    return (focus_ >= 0 && focus_ < (int)rows_.size()) ? &rows_[(size_t)focus_] : nullptr;
}

int RowList::NextFocusable(int from, int dir) const {
    for (int i = from + dir; i >= 0 && i < (int)rows_.size(); i += dir)
        if (rows_[(size_t)i].Focusable()) return i;
    return -1;
}

void RowList::SetFocus(int index, bool scroll) {
    if (index < 0 || index >= (int)rows_.size() || !rows_[(size_t)index].Focusable()) return;
    if (index == focus_) { if (scroll) EnsureVisible(index, true); return; }
    focus_ = index;
    if (scroll) EnsureVisible(index, true);
    if (onFocus) onFocus();
    Invalidate();
}

void RowList::FocusFirst() {
    const int first = NextFocusable(-1, +1);
    if (first >= 0) SetFocus(first);
    scrollTarget_ = 0.0f;
}

void RowList::FocusById(const std::wstring& id) {
    for (int i = 0; i < (int)rows_.size(); ++i)
        if (rows_[(size_t)i].id == id) { SetFocus(i); return; }
}

void RowList::EnsureVisible(int i, bool animate) {
    if (i < 0 || i >= (int)slots_.size()) return;
    const int view = bounds_.bottom - bounds_.top;
    int top = slots_[(size_t)i].top;
    const int bottom = top + slots_[(size_t)i].height;
    // Bring the heading a row sits under into view with it, so the first row
    // of a group is never seen without the name of the group.
    if (i > 0 && rows_[(size_t)i - 1].kind == Kind::Section)
        top = slots_[(size_t)i - 1].top;
    const int margin = theme::Scale(6);
    float target = scrollTarget_;
    if (top - margin < target) target = (float)(top - margin);
    else if (bottom + margin > target + view) target = (float)(bottom + margin - view);
    target = (std::max)(0.0f, (std::min)(target, (float)MaxScroll()));
    scrollTarget_ = target;
    if (!animate) scroll_ = target;
    Invalidate();
}

void RowList::SetCapture(int row, UINT heldMods, const std::wstring& note) {
    captureRow_  = row;
    captureHeld_ = heldMods;
    captureNote_ = note;
    Invalidate();
}

// ================================================================ values
void RowList::Changed() {
    if (onChanged) onChanged();
    Invalidate();
}

void RowList::SetValue(int i, int v) {
    if (i < 0 || i >= (int)rows_.size()) return;
    Row& row = rows_[(size_t)i];
    if (!row.get || !row.set || !row.Enabled()) return;
    switch (row.kind) {
        case Kind::Toggle: v = v ? 1 : 0; break;
        case Kind::Slider: v = (std::max)(row.lo, (std::min)(row.hi, v)); break;
        case Kind::Choice:
        case Kind::Colour: {
            const int n = (int)row.options.size();
            if (n == 0) return;
            v = (std::max)(0, (std::min)(n - 1, v));
            break;
        }
        default: break;
    }
    if (v == row.get()) return;
    auto set = row.set;         // the callback may rebuild the rows
    set(v);
    Changed();
}

void RowList::Step(int i, int delta, bool big) {
    if (i < 0 || i >= (int)rows_.size()) return;
    const Row& row = rows_[(size_t)i];
    if (!row.get) return;
    const int cur = row.get();
    switch (row.kind) {
        case Kind::Toggle:
            SetValue(i, delta < 0 ? 0 : 1);
            break;
        case Kind::Choice:
        case Kind::Colour: {
            const int n = (int)row.options.size();
            if (n == 0) return;
            int v = cur + delta;
            if (row.wrap || row.kind == Kind::Colour) v = ((v % n) + n) % n;
            SetValue(i, v);
            break;
        }
        case Kind::Slider: {
            const int s = big ? row.bigStep : row.step;
            int v = cur + delta * s;
            // Land on the grid the step describes, so a value typed into the
            // file as 13 steps to 15 and 10 rather than 18 and 8.
            if (s > 1) v = (delta > 0) ? (cur / s + 1) * s : ((cur + s - 1) / s - 1) * s;
            SetValue(i, v);
            break;
        }
        default: break;
    }
}

void RowList::Activate(int i) {
    if (i < 0 || i >= (int)rows_.size()) return;
    const Row& row = rows_[(size_t)i];
    if (!row.Enabled()) return;
    switch (row.kind) {
        case Kind::Toggle:
            if (row.get) SetValue(i, 1 - row.get());
            break;
        case Kind::Choice:
            Step(i, +1, false);
            break;
        case Kind::Colour:
            if (row.activate) { auto fn = row.activate; fn(); Changed(); }
            else Step(i, +1, false);
            break;
        case Kind::Keys:
            if (onCapture) onCapture(i);
            break;
        case Kind::Action:
        case Kind::Info:
            if (row.activate) { auto fn = row.activate; fn(); Invalidate(); }
            break;
        case Kind::Item:
            if (row.activate)    { auto fn = row.activate; fn(); Invalidate(); }
            else if (row.remove) { auto fn = row.remove; fn(); Changed(); }
            break;
        default: break;
    }
}

void RowList::MoveInGroup(int i, int delta) {
    if (i < 0 || i >= (int)rows_.size()) return;
    const Row& row = rows_[(size_t)i];
    if (!row.orderGroup || !row.move || row.orderIndex < 0) return;
    int count = 0;
    for (const Row& r : rows_) if (r.orderGroup == row.orderGroup) ++count;
    const int to = row.orderIndex + delta;
    if (to < 0 || to >= count) return;
    auto move = row.move;
    move(row.orderIndex, to);
    Changed();
}

void RowList::SliderFromX(int i, int x) {
    const Row& row = rows_[(size_t)i];
    const RECT c = ControlRect(i);
    const int w = c.right - c.left;
    if (w <= 0) return;
    const float f = (float)(x - c.left) / (float)w;
    int v = row.lo + (int)std::lround(f * (float)(row.hi - row.lo));
    if (row.step > 1) v = (int)std::lround((double)v / row.step) * row.step;
    SetValue(i, v);
}

// ================================================================ input
bool RowList::Key(UINT vk, bool ctrl, bool shift) {
    if (rows_.empty()) return false;
    active_ = true;
    const int page = (std::max)(1, (int)((bounds_.bottom - bounds_.top) / theme::Scale(46)) - 1);
    const Row* row = FocusedRow();

    switch (vk) {
        case VK_UP:
        case VK_DOWN: {
            const int dir = (vk == VK_UP) ? -1 : +1;
            if (ctrl && row && row->orderGroup) { MoveInGroup(focus_, dir); return true; }
            const int next = NextFocusable(focus_, dir);
            if (next >= 0) SetFocus(next);
            else if (dir < 0) { scrollTarget_ = 0.0f; Invalidate(); }
            return true;
        }
        case VK_HOME:
            FocusFirst();
            return true;
        case VK_END: {
            const int last = NextFocusable((int)rows_.size(), -1);
            if (last >= 0) SetFocus(last);
            return true;
        }
        case VK_PRIOR:
        case VK_NEXT: {
            const int dir = (vk == VK_PRIOR) ? -1 : +1;
            int at = focus_;
            for (int n = 0; n < page; ++n) {
                const int next = NextFocusable(at, dir);
                if (next < 0) break;
                at = next;
            }
            if (at >= 0) SetFocus(at);
            return true;
        }
        case VK_LEFT:
        case VK_RIGHT: {
            if (!row || !row->Enabled()) return false;
            const int dir = (vk == VK_LEFT) ? -1 : +1;
            switch (row->kind) {
                case Kind::Toggle:
                case Kind::Choice:
                case Kind::Colour:
                case Kind::Slider:
                    Step(focus_, dir, shift);
                    return true;
                default:
                    return false;      // the host may use it to change column
            }
        }
        case VK_RETURN:
        case VK_SPACE:
            if (!row) return false;
            Activate(focus_);
            return true;
        case VK_DELETE:
        case VK_BACK:
            if (row && row->remove && row->Enabled()) {
                auto fn = row->remove;
                fn();
                Changed();
                return true;
            }
            return false;
        default:
            if (row && row->extraKey && vk == row->extraKey && row->extra && row->Enabled()) {
                auto fn = row->extra;
                fn();
                Changed();
                return true;
            }
            return false;
    }
}

void RowList::MouseMove(POINT pt, bool buttonDown) {
    if (drag_ == Drag::Slider && dragRow_ >= 0 && dragRow_ < (int)rows_.size()) {
        if (!buttonDown) { drag_ = Drag::None; return; }
        SliderFromX(dragRow_, pt.x);
        return;
    }
    if (drag_ == Drag::Thumb) {
        if (!buttonDown) { drag_ = Drag::None; return; }
        const RECT th = ThumbRect();
        const int travel = (track_.bottom - track_.top) - (th.bottom - th.top);
        if (travel > 0) {
            const float f = (float)(pt.y - thumbGrab_ - track_.top) / (float)travel;
            scrollTarget_ = scroll_ =
                (std::max)(0.0f, (std::min)(1.0f, f)) * (float)MaxScroll();
            Invalidate();
        }
        return;
    }
    if (drag_ == Drag::Order && dragRow_ >= 0) {
        if (!buttonDown) { drag_ = Drag::None; return; }
        const int over = RowAt(pt);
        if (over >= 0 && over != dragRow_ &&
            rows_[(size_t)over].orderGroup == rows_[(size_t)dragRow_].orderGroup) {
            const std::wstring id = rows_[(size_t)dragRow_].id;
            const int delta = rows_[(size_t)over].orderIndex - rows_[(size_t)dragRow_].orderIndex;
            MoveInGroup(dragRow_, delta);
            // The host rebuilt the rows; find the dragged one again.
            for (int i = 0; i < (int)rows_.size(); ++i)
                if (rows_[(size_t)i].id == id) { dragRow_ = i; focus_ = i; break; }
        }
        return;
    }

    const int over = RowAt(pt);
    const int part = PartAt(over, pt);
    if (over != hover_ || part != hoverPart_) {
        hover_ = over;
        hoverPart_ = part;
        Invalidate();
    }
    // The focus follows the pointer, as it does in the game: there is only
    // ever one lit row, and it is the one you are pointing at.
    if (over >= 0 && over != focus_ && rows_[(size_t)over].Focusable() && !buttonDown) {
        active_ = true;
        SetFocus(over, false);
    } else if (over >= 0 && !active_) {
        active_ = true;
        Invalidate();
    }
}

bool RowList::MouseDown(POINT pt) {
    if (MaxScroll() > 0 && PtInRect(&track_, pt)) {
        const RECT th = ThumbRect();
        if (PtInRect(&th, pt)) {
            drag_ = Drag::Thumb;
            thumbGrab_ = pt.y - th.top;
        } else {
            const float view = (float)(bounds_.bottom - bounds_.top);
            scrollTarget_ += (pt.y < th.top ? -view : view) * 0.9f;
            scrollTarget_ = (std::max)(0.0f, (std::min)(scrollTarget_, (float)MaxScroll()));
            Invalidate();
        }
        return true;
    }
    const int i = RowAt(pt);
    if (i < 0) return PtInRect(&bounds_, pt) != FALSE;
    const Row& row = rows_[(size_t)i];
    if (!row.Focusable()) return true;
    active_ = true;
    SetFocus(i, false);
    const int part = PartAt(i, pt);

    if (part == -2) {
        drag_ = Drag::Order;
        dragRow_ = i;
        return true;
    }
    switch (row.kind) {
        case Kind::Toggle:
            if (part >= 0) SetValue(i, part);
            break;
        case Kind::Choice:
            if (part == 0) Step(i, -1, false);
            else if (part >= 1) Step(i, +1, false);
            break;
        case Kind::Colour:
            if (part == 0) Step(i, -1, false);
            else if (part == 2) Step(i, +1, false);
            else if (part == 1) Activate(i);
            break;
        case Kind::Slider:
            if (part >= 0) {
                drag_ = Drag::Slider;
                dragRow_ = i;
                SliderFromX(i, pt.x);
            }
            break;
        case Kind::Keys:
            if (part >= 0) Activate(i);
            break;
        case Kind::Action:
        case Kind::Item:
            if (part >= 0) {
                pressed_ = true;
                pressRow_ = i;
                pressPart_ = part;
                Invalidate();
            }
            break;
        default:
            break;
    }
    return true;
}

void RowList::MouseUp(POINT pt) {
    const Drag was = drag_;
    drag_ = Drag::None;
    dragRow_ = -1;
    if (was != Drag::None) { Invalidate(); return; }
    if (pressed_) {
        pressed_ = false;
        const int i = RowAt(pt);
        if (i == pressRow_ && PartAt(i, pt) >= 0) Activate(i);
        Invalidate();
    }
}

void RowList::MouseLeave() {
    if (hover_ != -1 || pressed_) {
        hover_ = -1;
        hoverPart_ = -1;
        pressed_ = false;
        Invalidate();
    }
}

void RowList::Wheel(int delta) {
    if (MaxScroll() <= 0) return;
    scrollTarget_ -= (float)delta / 120.0f * (float)theme::Scale(46) * 2.5f;
    scrollTarget_ = (std::max)(0.0f, (std::min)(scrollTarget_, (float)MaxScroll()));
    Invalidate();
}

// ================================================================ animation
bool RowList::Tick() {
    const ULONGLONG now = GetTickCount64();
    const float dt = lastTick_ ? (float)(std::min<ULONGLONG>(now - lastTick_, 50)) : 16.0f;
    lastTick_ = now;
    bool moving = false;

    for (size_t i = 0; i < rows_.size() && i < lit_.size(); ++i) {
        const bool on = ((int)i == focus_) && (active_ || hover_ == (int)i) &&
                        rows_[i].Focusable();
        const float target = on ? 1.0f : 0.0f;
        float& v = lit_[i];
        if (v == target) continue;
        // In quickly, out a little slower - the lit frame trails the move.
        const float rate = on ? dt / 90.0f : dt / 150.0f;
        v = on ? (std::min)(1.0f, v + rate) : (std::max)(0.0f, v - rate);
        moving = true;
    }
    const float gap = scrollTarget_ - scroll_;
    if (std::fabs(gap) > 0.5f) {
        scroll_ += gap * (1.0f - std::exp(-dt / 65.0f));
        moving = true;
    } else if (gap != 0.0f) {
        scroll_ = scrollTarget_;
        moving = true;
    }
    if (captureRow_ >= 0) moving = true;          // the prompt pulses
    return moving;
}

// ================================================================ painting
RECT RowList::ThumbRect() const {
    const int view = bounds_.bottom - bounds_.top;
    const int max = MaxScroll();
    if (max <= 0 || content_ <= 0) return RECT{};
    const int trackH = track_.bottom - track_.top;
    const int h = (std::max)(theme::Scale(28), trackH * view / content_);
    const int y = track_.top + (int)((float)(trackH - h) * (scroll_ / (float)max));
    const int cx = (track_.left + track_.right) / 2;
    const int w = theme::Scale(3);              // 3 DIP wide (PLAN-1.6 2.4)
    return { cx - w / 2, y, cx - w / 2 + w, y + h };
}

void RowList::Paint(HDC dc, int offsetY) {
    if (rows_.empty()) return;
    const int saved = SaveDC(dc);
    // The glow of a lit row spills sideways past the panel, as it does in the
    // game; it is cut off at the top and bottom so it never climbs into the
    // frame's notches.
    IntersectClipRect(dc, bounds_.left - theme::Scale(24), bounds_.top,
                      bounds_.right + theme::Scale(24), bounds_.bottom);
    for (int i = 0; i < (int)rows_.size(); ++i) {
        RECT r = RowRect(i);
        OffsetRect(&r, 0, offsetY);
        if (r.bottom < bounds_.top - theme::Scale(20) || r.top > bounds_.bottom + theme::Scale(20))
            continue;
        PaintRow(dc, i, r, i < (int)lit_.size() ? lit_[(size_t)i] : 0.0f);
    }
    RestoreDC(dc, saved);

    // Where rows run on past the edge of the view they fade out rather than
    // being cut, so it is plain there is more.
    const int fade = theme::Scale(26);
    const COLORREF under = theme::Bg;
    const int inset = (std::max)(2, theme::Scale(2));
    if (scroll_ > 0.5f) {
        RECT top = { bounds_.left + inset, bounds_.top, bounds_.right - inset, bounds_.top + fade };
        theme::FadeV(dc, top, under, 255, 0);
    }
    if (scroll_ < (float)MaxScroll() - 0.5f) {
        RECT bottom = { bounds_.left + inset, bounds_.bottom - fade, bounds_.right - inset, bounds_.bottom };
        theme::FadeV(dc, bottom, under, 0, 255);
    }

    // The scrollbar: a 3 DIP track in Line, and a thumb in TextMute for where
    // the view is. Only drawn when the list overflows.
    if (MaxScroll() > 0) {
        const int w = theme::Scale(3);
        const int cx = (track_.left + track_.right) / 2;
        RECT line = { cx - w / 2, track_.top, cx - w / 2 + w, track_.bottom };
        theme::Wash(dc, line, theme::Line, 255);
        const RECT th = ThumbRect();
        theme::Wash(dc, th, drag_ == Drag::Thumb ? theme::TextDim : theme::TextMute, 255);
    }
}

void RowList::PaintRow(HDC dc, int i, const RECT& r, float lit) {
    const Row& row = rows_[(size_t)i];
    const int padL = PadLeft();
    const int inset = (std::max)(2, theme::Scale(2));

    if (row.kind == Kind::Page) return;          // shown in the sub-tab row, not here
    if (row.kind == Kind::Section) {
        RECT pr = { r.left + padL, r.top, r.right - PadRight(), r.bottom };
        theme::SectionPlate(dc, pr, row.label);
        return;
    }

    const bool enabled = row.Enabled();
    if (!enabled) lit = 0.0f;

    // The hairlines. Every row has one under it; the first of a group has one
    // over it too.
    RECT under = { r.left + inset, r.bottom - 1, r.right - inset, r.bottom };
    theme::Wash(dc, under, theme::Line, 255);
    if (i == 0 || rows_[(size_t)i - 1].kind == Kind::Section) {
        RECT over = { r.left + inset, r.top, r.right - inset, r.top + 1 };
        theme::Wash(dc, over, theme::Line, 255);
    }

    theme::RowFocus(dc, r, lit);

    theme::Look look;
    look.enabled = enabled;
    look.lit     = lit;
    look.hot     = (hover_ == i) ? hoverPart_ : -1;
    look.pressed = pressed_ && pressRow_ == i;

    const RECT c = ControlRect(i);
    const int cy = (r.top + r.bottom) / 2;
    const COLORREF labelInk = enabled ? theme::Mix(theme::Text, theme::TextHi, lit) : theme::TextMute;

    // A grip for rows that can be dragged into a new order.
    if (row.orderGroup) {
        const COLORREF g = enabled ? theme::Mix(theme::TextMute, theme::TextHi, lit) : theme::Line;
        for (int k = -1; k <= 1; ++k) {
            RECT bar = { r.left + theme::Scale(9), cy + k * theme::Scale(4),
                         r.left + theme::Scale(17), cy + k * theme::Scale(4) + (std::max)(1, theme::Scale(1)) };
            theme::Wash(dc, bar, g, 255);
        }
    }

    // ---- the name
    int labelRight = c.left - theme::Scale(16);
    if (row.kind == Kind::Info) labelRight = r.right - PadRight();
    RECT lr = { r.left + padL, r.top, labelRight, r.bottom };
    int labelEnd = lr.left;
    if (row.raw) {
        if (row.detail.empty()) {
            theme::Print(dc, Font::Row, row.label, lr, labelInk,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            RECT top = { lr.left, r.top + theme::Scale(6), lr.right, cy + theme::Scale(1) };
            theme::Print(dc, Font::Row, row.label, top, labelInk,
                         DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT sub = { lr.left, cy + theme::Scale(2), lr.right, r.bottom - theme::Scale(4) };
            theme::Print(dc, Font::Small, row.detail, sub,
                         enabled ? theme::Mix(theme::TextDim, theme::Text, lit) : theme::TextMute,
                         DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        }
        labelEnd = lr.left + (std::min)((int)(lr.right - lr.left),
                                        theme::Measure(dc, Font::Row, row.label));
    } else {
        theme::Print(dc, Font::Row, row.label, lr, labelInk,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        labelEnd = lr.left + (std::min)((int)(lr.right - lr.left),
                                        theme::Measure(dc, Font::Row, row.label));
    }

    // A tag after the name, then the mark for a value that differs from what
    // is saved.
    int x = labelEnd + theme::Scale(10);
    if (row.tag) {
        const std::wstring tag = row.tag();
        if (!tag.empty()) {
            const int tw = theme::Measure(dc, Font::Small, tag);
            RECT box = { x, cy - theme::Scale(9), x + tw + theme::Scale(12), cy + theme::Scale(9) };
            if (box.right < labelRight) {
                const COLORREF tc = enabled ? row.tagColor : theme::TextMute;
                theme::Wash(dc, box, tc, 28);
                theme::Frame(dc, box, tc, 170, 1);
                theme::Print(dc, Font::Small, tag, box, tc,
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                x = box.right + theme::Scale(10);
            }
        }
    }
    if (row.modified && row.modified() && x + theme::Scale(8) < labelRight)
        theme::Diamond(dc, (float)x + theme::ScaleF(3.0f), (float)cy, theme::ScaleF(2.6f),
                       theme::TextHi);

    // ---- the control
    switch (row.kind) {
        case Kind::Toggle: {
            // get() is 0 for the first word, 1 for the second. Use the row's own words.
            const int n = (int)row.options.size();
            const int v = row.get ? row.get() : 0;
            const std::wstring first = (n > 0) ? row.options[0] : L"Off";
            const std::wstring second = (n > 1) ? row.options[1] : L"On";
            theme::DrawToggle(dc, c, first, second, v, look);
            break;
        }
        case Kind::Choice:
        case Kind::Colour: {
            const int n = (int)row.options.size();
            const int v = row.get ? row.get() : 0;
            const std::wstring text = (v >= 0 && v < n) ? row.options[(size_t)v] : L"-";
            const bool wraps = row.wrap || row.kind == Kind::Colour;
            const COLORREF swatch = (row.kind == Kind::Colour && v >= 0 &&
                                     v < (int)row.colours.size()) ? row.colours[(size_t)v]
                                                                  : CLR_INVALID;
            theme::DrawSelector(dc, c, text, look, wraps || v > 0, wraps || v < n - 1, swatch, n, v);
            break;
        }
        case Kind::Slider: {
            const int v = row.get ? row.get() : row.lo;
            const float f = row.hi > row.lo ? (float)(v - row.lo) / (float)(row.hi - row.lo) : 0.0f;
            theme::Look l = look;
            if (drag_ == Drag::Slider && dragRow_ == i) l.pressed = true;
            theme::DrawSlider(dc, c, f, row.format ? row.format(v) : std::to_wstring(v), l);
            break;
        }
        case Kind::Keys: {
            const int packed = row.get ? row.get() : 0;
            const UINT vk = LOWORD(packed), mods = HIWORD(packed);
            const COLORREF ink = enabled ? theme::Mix(theme::Text, theme::TextHi, lit) : theme::TextMute;
            if (captureRow_ == i) {
                // Waiting for a chord: the box breathes, and whatever
                // modifiers are held so far are shown in it.
                const float t = (float)(GetTickCount64() % 1200) / 1200.0f;
                const float pulse = 0.5f + 0.5f * std::sin(t * 6.2831853f);
                theme::Wash(dc, c, theme::TextHi, (BYTE)(14 + 24 * pulse));
                theme::Frame(dc, c, theme::TextHi, (BYTE)(150 + 105 * pulse), (std::max)(1, theme::Scale(1)));
                if (captureHeld_) {
                    const int w = theme::Chord(dc, 0, cy, captureHeld_, 0, theme::TextHi, false, true, true);
                    const int dots = theme::Measure(dc, Font::Row, L"+ ...");
                    int cx = c.left + ((c.right - c.left) - (w + theme::Scale(6) + dots)) / 2;
                    theme::Chord(dc, cx, cy, captureHeld_, 0, theme::TextHi, false, false, true);
                    RECT tr = { cx + w + theme::Scale(6), c.top, c.right, c.bottom };
                    theme::Print(dc, Font::Row, L"+ ...", tr, theme::TextHi,
                                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                } else {
                    theme::Print(dc, Font::Row, captureNote_.empty() ? L"Press a shortcut"
                                                                      : captureNote_,
                                 c, theme::TextHi, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            } else if (!vk) {
                theme::Print(dc, Font::Row, L"Not set", c, enabled ? theme::TextDim : theme::TextMute,
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                const int w = theme::Chord(dc, 0, cy, mods, vk, ink, false, true, false);
                const int from = (std::max)((int)c.left, (int)c.right - w);
                const int saved = SaveDC(dc);
                IntersectClipRect(dc, c.left - theme::Scale(40), c.top - 2, c.right, c.bottom + 2);
                theme::Chord(dc, (std::min)(from, (int)c.right - w), cy, mods, vk, ink, false, false, false);
                RestoreDC(dc, saved);
            }
            break;
        }
        case Kind::Action:
            theme::DrawAction(dc, c, row.button.empty() ? L"Select" : row.button, look, row.danger);
            break;
        case Kind::Item:
            theme::DrawAction(dc, c, row.button.empty() ? L"Remove" : row.button, look, row.danger);
            break;
        case Kind::Info: {
            const std::wstring v = row.value ? row.value() : L"";
            if (!v.empty()) {
                RECT vr = { labelEnd + theme::Scale(24), r.top, r.right - PadRight(), r.bottom };
                const COLORREF ink = enabled ? theme::Mix(theme::TextDim, theme::TextHi, lit) : theme::TextMute;
                if (row.rawValue)
                    theme::Print(dc, Font::Body, v, vr, ink,
                                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
                else
                    theme::Print(dc, Font::Row, v, vr, ink,
                                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            break;
        }
        default:
            break;
    }
}

std::vector<std::pair<std::wstring, std::wstring>> RowList::Prompts() const {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    const Row* row = FocusedRow();
    if (!row || !row->Enabled()) return out;
    if (captureRow_ >= 0) {
        out.push_back({ L"Esc", L"Cancel" });
        return out;
    }
    switch (row->kind) {
        case Kind::Toggle:
        case Kind::Choice:
            out.push_back({ L"\x2190 \x2192", L"Change" });
            break;
        case Kind::Colour:
            out.push_back({ L"\x2190 \x2192", L"Change" });
            if (row->activate) out.push_back({ L"Enter", L"Custom" });
            break;
        case Kind::Slider:
            out.push_back({ L"\x2190 \x2192", L"Adjust" });
            break;
        case Kind::Keys:
            out.push_back({ L"Enter", L"Change key" });
            if (row->remove) out.push_back({ L"Del", L"Clear" });
            break;
        case Kind::Action:
            out.push_back({ L"Enter", row->button.empty() ? L"Select" : row->button });
            break;
        case Kind::Item:
            if (row->activate) out.push_back({ L"Enter", row->button.empty() ? L"Select" : row->button });
            if (row->remove) out.push_back({ L"Del", L"Remove" });
            break;
        case Kind::Info:
            if (row->activate) out.push_back({ L"Enter", L"Select" });
            break;
        default:
            break;
    }
    if (row->extraKey && row->extra) out.push_back({ theme::KeyName(row->extraKey), row->extraWord });
    if (row->orderGroup) out.push_back({ L"Ctrl \x2191 \x2193", L"Move" });
    return out;
}

} // namespace ui
} // namespace awa
