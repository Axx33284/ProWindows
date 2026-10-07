// ProWindows - modal screens. See modal.h.
#include "modal.h"
#include "rowlist.h"
#include "theme.h"
#include "winutil.h"
#include <windowsx.h>

namespace awa {
namespace ui {

using theme::Font;

namespace {

constexpr wchar_t kClass[] = L"ProWindowsModal";
constexpr UINT_PTR kTimerAnim  = 1;
constexpr UINT_PTR kTimerCaret = 2;
constexpr int kMaxShown = 300;          // a list longer than this wants more typing

enum class Type { Confirm, Notice, Pick };

struct Modal {
    Type type = Type::Notice;
    HWND owner = nullptr;
    HWND wnd = nullptr;
    std::wstring title, body, yes, no, prompt, verb;
    bool danger = false;

    int  buttonCount = 1;
    RECT buttons[2] = {};
    int  focusButton = 0;
    int  hotButton = -1, pressedButton = -1;

    // Pick.
    const std::vector<PickEntry>* entries = nullptr;
    std::wstring filter;
    RowList list;
    std::wstring chosen;
    bool caretOn = true;
    RECT field = {};

    bool done = false;
    bool accepted = false;
    int  answer = -1;            // Ask: 1 yes, 0 no, -1 neither
    ULONGLONG shownAt = 0;
    bool animating = false;
};

int g_open = 0;

Modal* Of(HWND wnd) { return reinterpret_cast<Modal*>(GetWindowLongPtrW(wnd, GWLP_USERDATA)); }

int Pad() { return theme::Scale(28); }

void Finish(Modal* m, bool ok) {
    m->accepted = ok;
    m->done = true;
}

void Answer(Modal* m, int answer) {
    m->answer = answer;
    Finish(m, answer == 1);
}

void Kick(Modal* m) {
    if (!m->animating) {
        m->animating = true;
        SetTimer(m->wnd, kTimerAnim, 15, nullptr);
    }
}

void Refilter(Modal* m) {
    std::vector<Row> rows;
    const std::wstring needle = ToLower(Trim(m->filter));
    int shown = 0;
    for (size_t i = 0; i < m->entries->size() && shown < kMaxShown; ++i) {
        const PickEntry& e = (*m->entries)[i];
        if (!needle.empty() &&
            ToLower(e.label).find(needle) == std::wstring::npos &&
            ToLower(e.detail).find(needle) == std::wstring::npos)
            continue;
        Row r;
        r.kind   = Kind::Item;
        r.id     = L"pick:" + std::to_wstring(i);
        r.label  = e.label;
        r.detail = e.detail;
        r.raw    = true;
        r.button = m->verb;
        const std::wstring value = e.value;
        r.activate = [m, value]() { m->chosen = value; Finish(m, true); };
        rows.push_back(std::move(r));
        ++shown;
    }
    if (rows.empty()) {
        rows.push_back(Info(L"none", needle.empty() ? L"Nothing to choose from"
                                                    : L"Nothing matches", L"", nullptr));
    }
    m->list.SetRows(std::move(rows), false);
    m->list.SetActive(true);
    InvalidateRect(m->wnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- layout
SIZE Measure(Modal* m) {
    HDC dc = GetDC(nullptr);
    const int w = theme::Scale(m->type == Type::Pick ? 660 : 560);
    int h = 0;
    if (m->type == Type::Pick) {
        h = theme::Scale(620);
    } else {
        RECT body = { 0, 0, w - 2 * Pad(), 2000 };
        const int bh = theme::PrintWrapped(dc, Font::Body, m->body, body, 0, true);
        h = Pad() + theme::Scale(34) + theme::Scale(22) + bh + theme::Scale(34) +
            theme::Scale(40) + Pad();
    }
    ReleaseDC(nullptr, dc);
    return { w, h };
}

void Layout(Modal* m) {
    RECT c;
    GetClientRect(m->wnd, &c);
    const int pad = Pad();
    const int bh = theme::Scale(40);
    HDC dc = GetDC(m->wnd);
    int x = pad;
    const int y = c.bottom - pad - bh;
    const std::wstring labels[2] = { m->yes, m->no };
    for (int i = 0; i < m->buttonCount; ++i) {
        const int w = (std::max)(theme::Scale(130), theme::ButtonWidth(dc, labels[i], nullptr));
        m->buttons[i] = { x, y, x + w, y + bh };
        x += w + theme::Scale(12);
    }
    ReleaseDC(m->wnd, dc);

    if (m->type == Type::Pick) {
        const int top = pad + theme::Scale(34) + theme::Scale(18);
        m->field = { pad, top + theme::Scale(22), c.right - pad, top + theme::Scale(22) + theme::Scale(40) };
        RECT rows = { pad, m->field.bottom + theme::Scale(14), c.right - pad - theme::Scale(14),
                      y - theme::Scale(20) };
        RECT track = { rows.right + theme::Scale(4), rows.top, rows.right + theme::Scale(14), rows.bottom };
        m->list.SetBounds(rows, track);
    }
}

// ---------------------------------------------------------------- painting
void PaintInto(Modal* m, HDC dc, const RECT& c) {
    theme::PaintBackdrop(dc, c, { c.right, c.bottom });
    theme::Frame(dc, c, theme::Edge, 120, 1);
    const int pad = Pad();

    // The heading: a short amber rail and the title in capitals, a hairline
    // under both.
    RECT rail = { pad, pad + theme::Scale(6), pad + theme::Scale(3), pad + theme::Scale(28) };
    theme::Wash(dc, rail, m->danger ? theme::Danger : theme::Amber, 255);
    RECT tr = { pad + theme::Scale(14), pad, c.right - pad, pad + theme::Scale(34) };
    theme::Print(dc, Font::Title, theme::Caps(m->title), tr, theme::Text,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, theme::Scale(1));
    RECT rule = { pad, pad + theme::Scale(44), c.right - pad, pad + theme::Scale(45) };
    theme::Wash(dc, rule, theme::Line, 255);

    if (m->type == Type::Pick) {
        RECT pr = { pad, rule.bottom + theme::Scale(12), c.right - pad, m->field.top - theme::Scale(4) };
        theme::Print(dc, Font::Caption, theme::Caps(m->prompt), pr, theme::TextDim,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, theme::Scale(1));

        // The filter: a field with a magnifier, what has been typed, a caret.
        const RECT& f = m->field;
        theme::Wash(dc, f, RGB(0, 0, 0), 120);
        theme::Frame(dc, f, theme::Amber, 200, (std::max)(1, theme::Scale(1)));
        const float cx = (float)f.left + theme::ScaleF(20.0f), cy = (float)(f.top + f.bottom) / 2.0f;
        {
            HPEN pen = CreatePen(PS_SOLID, (std::max)(1, theme::Scale(2)), theme::TextDim);
            HGDIOBJ old = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            const int r = theme::Scale(6);
            Ellipse(dc, (int)cx - r, (int)cy - r - theme::Scale(1), (int)cx + r, (int)cy + r - theme::Scale(1));
            MoveToEx(dc, (int)cx + r - theme::Scale(1), (int)cy + r - theme::Scale(2), nullptr);
            LineTo(dc, (int)cx + r + theme::Scale(4), (int)cy + r + theme::Scale(3));
            SelectObject(dc, oldBrush);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        RECT text = { f.left + theme::Scale(40), f.top, f.right - theme::Scale(12), f.bottom };
        if (m->filter.empty()) {
            theme::Print(dc, Font::Caption, L"TYPE TO SEARCH", text, theme::TextMute,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE, theme::Scale(2));
        } else {
            theme::Print(dc, Font::Body, m->filter, text, theme::Text,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (m->caretOn) {
            const int tw = m->filter.empty() ? 0 : theme::Measure(dc, Font::Body, m->filter);
            const int x = (std::min)((int)text.right, (int)text.left + tw + theme::Scale(1));
            RECT caret = { x, (int)cy - theme::Scale(9), x + (std::max)(1, theme::Scale(2)),
                           (int)cy + theme::Scale(9) };
            theme::Wash(dc, caret, theme::Amber, 255);
        }
        m->list.Paint(dc);
    } else {
        RECT body = { pad, rule.bottom + theme::Scale(20), c.right - pad, m->buttons[0].top - theme::Scale(20) };
        theme::PrintWrapped(dc, Font::Body, m->body, body, RGB(206, 210, 216));
    }

    // Buttons, and the prompts that say which keys press them.
    const std::wstring labels[2] = { m->yes, m->no };
    for (int i = 0; i < m->buttonCount; ++i) {
        theme::ButtonLook look;
        look.hot     = (m->hotButton == i);
        look.pressed = (m->pressedButton == i && m->hotButton == i);
        look.primary = (i == 0) && !m->danger;
        look.focused = (m->type != Type::Pick) && (m->focusButton == i);
        if (i == 0 && m->danger && !look.hot && !look.pressed) {
            // A destructive answer is red until it is pointed at.
            const RECT& b = m->buttons[i];
            theme::CutBox(dc, b, (b.bottom - b.top) * 2 / 5, theme::Danger, 22, theme::Danger,
                          look.focused ? 255 : 200, (float)(std::max)(2, theme::Scale(2)));
            if (look.focused) theme::Glow(dc, b, theme::Danger, theme::Scale(10), 70);
            theme::Print(dc, Font::Button, theme::Caps(labels[i]), b, RGB(255, 132, 116),
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE, theme::Scale(1));
            continue;
        }
        theme::DrawButton(dc, m->buttons[i], labels[i], nullptr, look);
    }

    std::vector<std::pair<std::wstring, std::wstring>> prompts;
    if (m->type == Type::Pick) {
        prompts = { { L"\x2191 \x2193", L"Choose" }, { L"Enter", m->verb }, { L"Esc", L"Cancel" } };
    } else if (m->type == Type::Confirm) {
        prompts = { { L"Enter", L"Select" }, { L"Esc", L"Cancel" } };
    } else {
        prompts = { { L"Enter", L"OK" } };
    }
    const int cy = (m->buttons[0].top + m->buttons[0].bottom) / 2;
    const int lastButton = (std::max)(0, (std::min)(1, m->buttonCount - 1));
    int x = c.right - pad;
    for (int i = (int)prompts.size() - 1; i >= 0; --i) {
        const int w = theme::Prompt(dc, 0, cy, prompts[(size_t)i].first, prompts[(size_t)i].second,
                                    theme::TextDim, true);
        x -= w;
        if (x < m->buttons[lastButton].right + theme::Scale(16)) break;
        theme::Prompt(dc, x, cy, prompts[(size_t)i].first, prompts[(size_t)i].second, theme::TextDim);
        x -= theme::Scale(18);
    }
}

void Paint(Modal* m) {
    PAINTSTRUCT ps;
    HDC target = BeginPaint(m->wnd, &ps);
    theme::SetDpi(DpiForWindow(m->wnd));
    RECT c;
    GetClientRect(m->wnd, &c);
    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(target, c.right, c.bottom) : nullptr;
    if (mem && bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintInto(m, mem, c);
        BitBlt(target, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
    } else {
        PaintInto(m, target, c);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    EndPaint(m->wnd, &ps);
}

int ButtonAt(Modal* m, POINT pt) {
    for (int i = 0; i < m->buttonCount; ++i)
        if (PtInRect(&m->buttons[i], pt)) return i;
    return -1;
}

void Press(Modal* m, int button) {
    if (m->type == Type::Pick && button == 0) {
        const Row* row = m->list.FocusedRow();
        if (row && row->activate) { auto fn = row->activate; fn(); }
        return;
    }
    Answer(m, button == 0 ? 1 : 0);
}

void Paste(Modal* m) {
    if (!OpenClipboard(m->wnd)) return;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(h))) {
            for (const wchar_t* p = text; *p && *p != L'\r' && *p != L'\n'; ++p)
                m->filter += *p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    Refilter(m);
}

LRESULT CALLBACK ModalProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    Modal* m = Of(wnd);
    if (m) theme::SetDpi(DpiForWindow(wnd));      // shared; see SettingsProc
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (m) { Paint(m); return 0; }
            break;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

        case WM_TIMER:
            if (!m) break;
            if (wp == kTimerCaret) {
                m->caretOn = !m->caretOn;
                InvalidateRect(wnd, &m->field, FALSE);
                return 0;
            }
            if (wp == kTimerAnim) {
                bool more = false;
                const ULONGLONG age = GetTickCount64() - m->shownAt;
                const BYTE a = age >= 130 ? 255 : (BYTE)(255 * age / 130);
                SetLayeredWindowAttributes(wnd, 0, a, LWA_ALPHA);
                if (a < 255) more = true;
                if (m->type == Type::Pick && m->list.Tick()) more = true;
                InvalidateRect(wnd, nullptr, FALSE);
                if (!more) { KillTimer(wnd, kTimerAnim); m->animating = false; }
                return 0;
            }
            break;

        case WM_CHAR:
            if (m && m->type == Type::Pick && wp >= 32 && wp != 127) {
                m->filter += (wchar_t)wp;
                m->caretOn = true;
                Refilter(m);
                return 0;
            }
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (!m) break;
            const bool ctrl = GetKeyState(VK_CONTROL) < 0;
            const bool shift = GetKeyState(VK_SHIFT) < 0;
            if (wp == VK_ESCAPE) {
                if (m->type == Type::Pick && !m->filter.empty()) { m->filter.clear(); Refilter(m); }
                else if (m->type == Type::Notice) Finish(m, true);
                else Answer(m, -1);
                return 0;
            }
            if (m->type == Type::Pick) {
                if (wp == VK_BACK) {
                    if (ctrl) {
                        while (!m->filter.empty() && m->filter.back() == L' ') m->filter.pop_back();
                        while (!m->filter.empty() && m->filter.back() != L' ') m->filter.pop_back();
                    } else if (!m->filter.empty()) {
                        m->filter.pop_back();
                    }
                    Refilter(m);
                    return 0;
                }
                if (ctrl && wp == 'V') { Paste(m); return 0; }
                if (wp == VK_RETURN) { Press(m, 0); return 0; }
                if (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT) {
                    m->list.Key((UINT)wp, false, shift);
                    Kick(m);
                    return 0;
                }
                return 0;
            }
            if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_TAB) {
                if (m->buttonCount > 1) {
                    m->focusButton = (wp == VK_LEFT) ? 0 : (wp == VK_RIGHT ? 1 : 1 - m->focusButton);
                    InvalidateRect(wnd, nullptr, FALSE);
                }
                return 0;
            }
            if (wp == VK_RETURN || wp == VK_SPACE) { Press(m, m->focusButton); return 0; }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!m) break;
            const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const int hot = ButtonAt(m, pt);
            if (hot != m->hotButton) { m->hotButton = hot; InvalidateRect(wnd, nullptr, FALSE); }
            if (m->type == Type::Pick) { m->list.MouseMove(pt, (wp & MK_LBUTTON) != 0); Kick(m); }
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, wnd, 0 };
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (m) {
                m->hotButton = -1;
                if (m->type == Type::Pick) m->list.MouseLeave();
                InvalidateRect(wnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            if (!m) break;
            const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            SetCapture(wnd);
            m->pressedButton = ButtonAt(m, pt);
            if (m->pressedButton < 0 && m->type == Type::Pick) m->list.MouseDown(pt);
            InvalidateRect(wnd, nullptr, FALSE);
            Kick(m);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!m) break;
            const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ReleaseCapture();
            const int was = m->pressedButton;
            m->pressedButton = -1;
            if (was >= 0 && ButtonAt(m, pt) == was) Press(m, was);
            else if (m->type == Type::Pick) m->list.MouseUp(pt);
            InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (m && m->type == Type::Pick) {
                m->list.Wheel(GET_WHEEL_DELTA_WPARAM(wp));
                Kick(m);
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
            break;
        case WM_CLOSE:
            if (m) { if (m->type == Type::Notice) Finish(m, true); else Answer(m, -1); }
            return 0;
        case WM_ACTIVATE:
            // A modal screen that loses the foreground to another application
            // stays where it is; clicking the dimmed owner brings it back.
            break;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

void Register() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style         = CS_DROPSHADOW;
    wc.lpfnWndProc   = ModalProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    done = true;
}

bool Run(Modal& m) {
    Register();
    HWND owner = m.owner ? GetAncestor(m.owner, GA_ROOT) : nullptr;
    m.owner = owner;
    theme::SetDpi(owner ? DpiForWindow(owner) : theme::Dpi());

    const SIZE size = Measure(&m);
    RECT area;
    if (owner && IsWindowVisible(owner)) {
        GetWindowRect(owner, &area);
    } else {
        POINT pt;
        GetCursorPos(&pt);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
        area = mi.rcWork;
    }
    const int x = area.left + ((area.right - area.left) - size.cx) / 2;
    const int y = area.top + ((area.bottom - area.top) - size.cy) / 2;

    m.wnd = CreateWindowExW(WS_EX_LAYERED | (owner ? 0 : WS_EX_TOPMOST), kClass, m.title.c_str(),
                            WS_POPUP, x, y, size.cx, size.cy, owner, nullptr,
                            GetModuleHandleW(nullptr), &m);
    if (!m.wnd) return false;
    SetLayeredWindowAttributes(m.wnd, 0, 0, LWA_ALPHA);

    // A per-monitor-v2 window can land on a monitor at another scale; size it
    // again at that one.
    const UINT dpi = DpiForWindow(m.wnd);
    if (dpi != theme::Dpi()) {
        theme::SetDpi(dpi);
        const SIZE again = Measure(&m);
        SetWindowPos(m.wnd, nullptr, 0, 0, again.cx, again.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Layout(&m);
    if (m.type == Type::Pick) {
        Refilter(&m);
        SetTimer(m.wnd, kTimerCaret, 530, nullptr);
    }

    ++g_open;
    if (owner) {
        EnableWindow(owner, FALSE);
        RedrawWindow(owner, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    m.shownAt = GetTickCount64();
    ShowWindow(m.wnd, SW_SHOW);
    SetForegroundWindow(m.wnd);
    SetFocus(m.wnd);
    Kick(&m);

    MSG msg;
    while (!m.done) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) { PostQuitMessage((int)msg.wParam); break; }
        if (got < 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    --g_open;
    if (owner) EnableWindow(owner, TRUE);      // before the modal goes, so the owner is activated
    DestroyWindow(m.wnd);
    if (owner) {
        SetForegroundWindow(owner);
        RedrawWindow(owner, nullptr, nullptr, RDW_INVALIDATE);
    }
    return m.accepted;
}

} // namespace

bool Confirm(HWND owner, const std::wstring& title, const std::wstring& body,
             const std::wstring& yes, const std::wstring& no, bool danger) {
    Modal m;
    m.type = Type::Confirm;
    m.owner = owner;
    m.title = title;
    m.body = body;
    m.yes = yes;
    m.no = no;
    m.danger = danger;
    m.buttonCount = 2;
    m.focusButton = danger ? 1 : 0;       // the safe answer is the one under Enter
    return Run(m);
}

int Ask(HWND owner, const std::wstring& title, const std::wstring& body,
        const std::wstring& yes, const std::wstring& no) {
    Modal m;
    m.type = Type::Confirm;
    m.owner = owner;
    m.title = title;
    m.body = body;
    m.yes = yes;
    m.no = no;
    m.buttonCount = 2;
    Run(m);
    return m.answer;
}

void Notice(HWND owner, const std::wstring& title, const std::wstring& body) {
    Modal m;
    m.type = Type::Notice;
    m.owner = owner;
    m.title = title;
    m.body = body;
    m.yes = L"OK";
    m.buttonCount = 1;
    Run(m);
}

bool Pick(HWND owner, const std::wstring& title, const std::wstring& prompt,
          const std::vector<PickEntry>& entries, std::wstring* chosen, const std::wstring& verb) {
    Modal m;
    m.type = Type::Pick;
    m.owner = owner;
    m.title = title;
    m.prompt = prompt;
    m.entries = &entries;
    m.verb = verb;
    m.yes = verb;
    m.no = L"Cancel";
    m.buttonCount = 2;
    m.list.onInvalidate = [&m]() { if (m.wnd) { InvalidateRect(m.wnd, nullptr, FALSE); Kick(&m); } };
    if (!Run(m)) return false;
    if (chosen) *chosen = m.chosen;
    return true;
}

bool ModalOpen() { return g_open > 0; }

} // namespace ui
} // namespace awa
