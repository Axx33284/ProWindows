// ProWindows - settings shell, Layout page and Behaviour page.
//
// The window owns no state: pages read the live Config when they load and write
// it back on Apply, which then saves + reloads through the normal config path so
// hotkeys, rules and layout all stay in sync.
#include "settings.h"
#include "settings_internal.h"
#include "winutil.h"
#include "theme.h"
#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")

namespace awa {

const ModChoice kModChoices[6] = {
    { L"Alt",          MOD_ALT },
    { L"Ctrl",         MOD_CONTROL },
    { L"Win",          MOD_WIN },
    { L"Alt + Shift",  MOD_ALT | MOD_SHIFT },
    { L"Ctrl + Alt",   MOD_CONTROL | MOD_ALT },
    { L"Win + Alt",    MOD_WIN | MOD_ALT },
};

static HWND  g_dlg           = nullptr;
static HFONT g_titleFont     = nullptr;
static bool  g_toldAboutTray = false;

struct Page { int templateId; const wchar_t* caption; DLGPROC proc; HWND hwnd; };

static Page g_pages[] = {
    { IDD_PAGE_LAYOUT,    L"Layout",         PageLayoutProc,    nullptr },
    { IDD_PAGE_BEHAVIOUR, L"Behaviour",      PageBehaviourProc, nullptr },
    { IDD_PAGE_SHORTCUTS, L"Window keys",    PageShortcutsProc, nullptr },
    { IDD_PAGE_APPS,      L"Open apps",      PageAppsProc,      nullptr },
    { IDD_PAGE_SEARCH,    L"Search",         PageSearchProc,    nullptr },
    { IDD_PAGE_MONITOR,   L"Monitor",        PageMonitorProc,   nullptr },
    { IDD_PAGE_GENERAL,   L"General",        PageGeneralProc,   nullptr },
};
// Keep the table and the PageIndex names in step.
static_assert(ARRAYSIZE(g_pages) == PAGE_COUNT, "PageIndex is out of step with g_pages");
static int g_activePage = 0;

// ================================================================ helpers
void SetCheck(HWND dlg, int id, bool on) {
    CheckDlgButton(dlg, id, on ? BST_CHECKED : BST_UNCHECKED);
}

bool GetCheck(HWND dlg, int id) {
    return IsDlgButtonChecked(dlg, id) == BST_CHECKED;
}

std::wstring GetText(HWND wnd) {
    int len = GetWindowTextLengthW(wnd);
    if (len <= 0) return L"";
    std::wstring s;
    s.resize((size_t)len + 1);
    int got = GetWindowTextW(wnd, &s[0], len + 1);
    s.resize((size_t)(got > 0 ? got : 0));
    return s;
}

std::wstring GetItemText(HWND dlg, int id) { return GetText(GetDlgItem(dlg, id)); }

void CenterOn(HWND wnd, HWND parent) {
    RECT r;
    if (!GetWindowRect(wnd, &r)) return;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;

    RECT area;
    if (parent && IsWindowVisible(parent)) {
        GetWindowRect(parent, &area);
    } else {
        POINT pt;
        GetCursorPos(&pt);
        MONITORINFO mi{ sizeof(MONITORINFO) };
        if (!GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
        area = mi.rcWork;
    }

    int x = area.left + ((area.right - area.left) - w) / 2;
    int y = area.top + ((area.bottom - area.top) - h) / 2;

    // Centring on its own is not enough. This dialog is laid out in dialog
    // units, so at 150% scaling on a 768-pixel-high laptop it is taller than
    // the work area, and centring it then puts Apply and Hide below the bottom
    // edge where they cannot be clicked - the window looks broken on that
    // machine and fine on every other one. Keep the top-left corner inside the
    // work area, which at worst loses the bottom of a window that was never
    // going to fit rather than the part with the buttons on it.
    MONITORINFO target{ sizeof(MONITORINFO) };
    const POINT centre = { x + w / 2, y + h / 2 };
    if (GetMonitorInfoW(MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST), &target)) {
        x = (std::min)(x, (int)target.rcWork.right - w);
        y = (std::min)(y, (int)target.rcWork.bottom - h);
        x = (std::max)(x, (int)target.rcWork.left);
        y = (std::max)(y, (int)target.rcWork.top);
    }

    SetWindowPos(wnd, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static bool BrowseForExe(HWND parent, std::wstring* result, bool fullPath) {
    wchar_t file[MAX_PATH * 2] = {};
    static const wchar_t filter[] = L"Programs (*.exe)\0*.exe\0All files (*.*)\0*.*\0";

    wchar_t initialDir[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramFiles", initialDir, MAX_PATH);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = parent;
    ofn.lpstrFilter     = filter;
    ofn.lpstrFile       = file;
    ofn.nMaxFile        = (DWORD)(sizeof(file) / sizeof(file[0]));
    ofn.lpstrTitle      = L"Choose a program";
    ofn.lpstrInitialDir = initialDir[0] ? initialDir : nullptr;
    ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                          OFN_EXPLORER;

    if (!GetOpenFileNameW(&ofn)) return false;

    std::wstring path = file;
    if (fullPath) { *result = path; return true; }

    const size_t slash = path.find_last_of(L"\\/");
    *result = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    return true;
}

bool BrowseForExeName(HWND parent, std::wstring* exeName) {
    return BrowseForExe(parent, exeName, false);
}

bool PickColor(HWND parent, COLORREF* color) {
    // Kept across calls so a palette built for one metric is there for the
    // next, which is the whole point of the custom slots.
    static COLORREF custom[16] = {
        RGB(88, 140, 255), RGB(76, 200, 130), RGB(167, 139, 250),
        RGB(255, 138, 101), RGB(96, 205, 255), RGB(244, 143, 177),
        RGB(255, 255, 255), RGB(120, 255, 140), RGB(255, 183, 77),
        RGB(0, 229, 255), RGB(255, 0, 193), RGB(255, 214, 0),
        RGB(180, 180, 180), RGB(120, 120, 120), RGB(60, 60, 60), RGB(0, 0, 0),
    };

    CHOOSECOLORW cc = {};
    cc.lStructSize  = sizeof(cc);
    cc.hwndOwner    = parent;
    cc.rgbResult    = *color;
    cc.lpCustColors = custom;
    cc.Flags        = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) return false;
    *color = cc.rgbResult;
    return true;
}

bool BrowseForExePath(HWND parent, std::wstring* fullPath) {
    return BrowseForExe(parent, fullPath, true);
}

std::wstring FriendlyCommandName(const std::wstring& command) {
    std::wstring path = Trim(command);
    if (!path.empty() && path[0] == L'"') {
        const size_t close = path.find(L'"', 1);
        if (close != std::wstring::npos) path = path.substr(1, close - 1);
    }
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path = path.substr(slash + 1);

    const size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        const std::wstring ext = ToLower(path.substr(dot));
        if (ext == L".lnk" || ext == L".exe") path = path.substr(0, dot);
    }
    return path.empty() ? command : path;
}

// ---------------------------------------------------------------- picker
namespace {

struct PickCtx {
    const std::vector<PickEntry>* entries = nullptr;
    const wchar_t* caption = nullptr;
    const wchar_t* prompt  = nullptr;
    std::wstring chosen;
};

void PickPopulate(HWND dlg, PickCtx* ctx) {
    const std::wstring filter = ToLower(GetItemText(dlg, IDC_PICK_FILTER));
    HWND list = GetDlgItem(dlg, IDC_PICK_LIST);

    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);

    for (size_t i = 0; i < ctx->entries->size(); ++i) {
        const PickEntry& e = (*ctx->entries)[i];
        if (!filter.empty()) {
            const bool hit = ToLower(e.label).find(filter) != std::wstring::npos ||
                             ToLower(e.detail).find(filter) != std::wstring::npos;
            if (!hit) continue;
        }
        std::wstring text = e.label;
        if (!e.detail.empty()) text += L"      " + e.detail;
        const int row = (int)SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)text.c_str());
        if (row >= 0) SendMessageW(list, LB_SETITEMDATA, row, (LPARAM)i);
    }

    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
    if (SendMessageW(list, LB_GETCOUNT, 0, 0) > 0)
        SendMessageW(list, LB_SETCURSEL, 0, 0);
}

bool PickCommit(HWND dlg, PickCtx* ctx) {
    HWND list = GetDlgItem(dlg, IDC_PICK_LIST);
    const int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return false;
    const int index = (int)SendMessageW(list, LB_GETITEMDATA, sel, 0);
    if (index < 0 || index >= (int)ctx->entries->size()) return false;
    ctx->chosen = (*ctx->entries)[(size_t)index].value;
    return true;
}

INT_PTR CALLBACK PickProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<PickCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));
    INT_PTR themed = 0;
    if (theme::DialogMessage(dlg, msg, wp, lp, &themed)) return themed;

    switch (msg) {
        case WM_INITDIALOG:
            SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)lp);
            ctx = reinterpret_cast<PickCtx*>(lp);
            theme::PrepareDialog(dlg);
            theme::DarkTitleBar(dlg);
            SetWindowTextW(dlg, ctx->caption);
            SetDlgItemTextW(dlg, IDC_PICK_PROMPT, ctx->prompt);
            SendDlgItemMessageW(dlg, IDC_PICK_FILTER, EM_SETCUEBANNER, TRUE,
                                (LPARAM)L"search");
            PickPopulate(dlg, ctx);
            CenterOn(dlg, GetParent(dlg));
            SetFocus(GetDlgItem(dlg, IDC_PICK_FILTER));
            return FALSE;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_PICK_FILTER:
                    if (HIWORD(wp) == EN_CHANGE && ctx) PickPopulate(dlg, ctx);
                    return TRUE;
                case IDC_PICK_LIST:
                    if (HIWORD(wp) == LBN_DBLCLK && ctx && PickCommit(dlg, ctx))
                        EndDialog(dlg, IDOK);
                    return TRUE;
                case IDOK:
                    if (ctx && PickCommit(dlg, ctx)) EndDialog(dlg, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            return FALSE;
    }
    return FALSE;
}

} // namespace

bool PickFromList(HWND parent, const wchar_t* caption, const wchar_t* prompt,
                  const std::vector<PickEntry>& entries, std::wstring* chosen) {
    if (entries.empty()) {
        MessageBoxW(parent, L"Nothing to choose from here.", kAppName,
                    MB_OK | MB_ICONINFORMATION);
        return false;
    }
    PickCtx ctx;
    ctx.entries = &entries;
    ctx.caption = caption;
    ctx.prompt  = prompt;

    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PICKAPP),
                        parent, PickProc, (LPARAM)&ctx) != IDOK)
        return false;
    *chosen = ctx.chosen;
    return true;
}

// ================================================================ Layout page
namespace {

struct LayoutInfo {
    const wchar_t* name;
    const wchar_t* description;
};

const LayoutInfo kLayoutInfo[(int)LayoutKind::COUNT] = {
    { L"Dwindle",
      L"Every new window splits the one you are focused on, always cutting "
      L"along its longer side. The result spirals outwards, so the first "
      L"window keeps the most room and later ones fill in around it.\r\n\r\n"
      L"Best when you keep opening and closing windows and want a sensible "
      L"arrangement without thinking about it. This is Hyprland's default." },

    { L"Master",
      L"One big window on the left holds whatever you are working on, and "
      L"everything else stacks in a column on the right.\r\n\r\n"
      L"Best when one window matters most - an editor, a document, a game "
      L"guide - and the rest are for reference. Use the slider below to set "
      L"how much room the main window gets, and $mod+Enter to move another "
      L"window into it." },

    { L"Grid",
      L"Every window gets an equal cell, arranged in as square a grid as the "
      L"count allows. Two windows split the screen in half, four make a 2x2, "
      L"and so on.\r\n\r\n"
      L"Best for comparing things side by side, or for dashboards where no "
      L"single window is more important than the others." },

    { L"Monocle",
      L"One window at a time, filling the whole screen. The others stay "
      L"stacked behind it and you bring them forward with the focus keys.\r\n\r\n"
      L"Best on a laptop or a small display, where splitting the screen just "
      L"makes everything too cramped to use." },
};

// A small painted sample of what the selected layout does, drawn from the same
// rules the real layouts use.
void DrawLayoutPreview(const DRAWITEMSTRUCT* dis, LayoutKind kind) {
    HDC dc = dis->hDC;
    RECT box = dis->rcItem;

    // The preview is a miniature desktop, so it uses the app's own palette
    // rather than the system one - system colours are light here.
    FillRect(dc, &box, theme::BrushBg());
    theme::RoundRect(dc, box, 6, theme::Bg, theme::Border, true);

    const int pad = 6;
    RECT area = { box.left + pad, box.top + pad, box.right - pad, box.bottom - pad };
    const int w = area.right - area.left;
    const int h = area.bottom - area.top;
    if (w <= 8 || h <= 8) return;

    const int gap = 3;
    RECT panes[4] = {};
    int count = 0;

    switch (kind) {
        case LayoutKind::Dwindle: {
            const int half = w / 2;
            const int halfH = h / 2;
            const int quarterH = halfH / 2;
            panes[0] = { area.left, area.top, area.left + half, area.bottom };
            panes[1] = { area.left + half, area.top, area.right, area.top + halfH };
            panes[2] = { area.left + half, area.top + halfH, area.left + half + w / 4,
                         area.bottom };
            panes[3] = { area.left + half + w / 4, area.top + halfH, area.right,
                         area.bottom };
            (void)quarterH;
            count = 4;
            break;
        }
        case LayoutKind::Master: {
            const int split = (int)(w * 0.55);
            const int third = h / 3;
            panes[0] = { area.left, area.top, area.left + split, area.bottom };
            panes[1] = { area.left + split, area.top, area.right, area.top + third };
            panes[2] = { area.left + split, area.top + third, area.right,
                         area.top + third * 2 };
            panes[3] = { area.left + split, area.top + third * 2, area.right,
                         area.bottom };
            count = 4;
            break;
        }
        case LayoutKind::Grid: {
            const int mx = area.left + w / 2;
            const int my = area.top + h / 2;
            panes[0] = { area.left, area.top, mx, my };
            panes[1] = { mx, area.top, area.right, my };
            panes[2] = { area.left, my, mx, area.bottom };
            panes[3] = { mx, my, area.right, area.bottom };
            count = 4;
            break;
        }
        default: {   // Monocle - one window in front, the rest hinted behind
            panes[0] = { area.left + 8, area.top + 8, area.right, area.bottom };
            panes[1] = { area.left + 4, area.top + 4, area.right - 4, area.bottom - 4 };
            panes[2] = { area.left, area.top, area.right - 8, area.bottom - 8 };
            count = 3;
            break;
        }
    }

    // For monocle the front-most pane is drawn last so it sits on top.
    for (int i = count - 1; i >= 0; --i) {
        RECT r = panes[i];
        if (kind != LayoutKind::Monocle) {
            r.right  -= gap;
            r.bottom -= gap;
        }
        const bool focused = (kind == LayoutKind::Monocle) ? (i == count - 1) : (i == 0);
        theme::RoundRect(dc, r, 3,
                         focused ? theme::Accent : theme::PanelAlt,
                         focused ? theme::AccentHover : theme::Border, true);
    }
}

LayoutKind SelectedLayout(HWND page) {
    const int sel = (int)SendDlgItemMessageW(page, IDC_LAYOUT, CB_GETCURSEL, 0, 0);
    return (sel >= 0 && sel < (int)LayoutKind::COUNT) ? (LayoutKind)sel
                                                      : LayoutKind::Dwindle;
}

void UpdateLayoutExplanation(HWND page) {
    const LayoutKind kind = SelectedLayout(page);

    // Spell out the modifier so the text matches the user's actual keys.
    std::wstring text = kLayoutInfo[(int)kind].description;
    const UINT mod = AppConfig().modMask;
    const wchar_t* modName = (mod & MOD_WIN) ? L"Win"
                           : (mod & MOD_CONTROL) ? L"Ctrl"
                           : (mod & MOD_ALT) ? L"Alt" : L"Shift";
    for (size_t p = text.find(L"$mod"); p != std::wstring::npos;
         p = text.find(L"$mod", p))
        text.replace(p, 4, modName);

    SetDlgItemTextW(page, IDC_LAYOUT_DESC, text.c_str());
    InvalidateRect(GetDlgItem(page, IDC_LAYOUT_PREVIEW), nullptr, TRUE);

    const bool usesMaster = (kind == LayoutKind::Master);
    EnableWindow(GetDlgItem(page, IDC_MASTER_SLIDER), usesMaster);
    SetDlgItemTextW(page, IDC_MASTER_HINT, usesMaster
        ? L"How much of the screen the big window on the left takes."
        : L"Only the Master layout uses this.");
}

} // namespace

void PageLayoutLoad(HWND page) {
    const Config& cfg = AppConfig();

    SendDlgItemMessageW(page, IDC_LAYOUT, CB_SETCURSEL, (WPARAM)cfg.layout, 0);

    const int pct = (int)(cfg.masterRatio * 100.0f + 0.5f);
    SendDlgItemMessageW(page, IDC_MASTER_SLIDER, TBM_SETPOS, TRUE, pct);
    wchar_t buf[32];
    swprintf_s(buf, L"%d%%", pct);
    SetDlgItemTextW(page, IDC_MASTER_VALUE, buf);

    SetDlgItemInt(page, IDC_GAP_INNER, (UINT)cfg.gapInner, FALSE);
    SetDlgItemInt(page, IDC_GAP_OUTER, (UINT)cfg.gapOuter, FALSE);
    SetDlgItemInt(page, IDC_WORKSPACES, (UINT)cfg.workspaceCount, FALSE);
    SetDlgItemInt(page, IDC_MARGIN_TOP,    (UINT)cfg.marginTop, FALSE);
    SetDlgItemInt(page, IDC_MARGIN_BOTTOM, (UINT)cfg.marginBottom, FALSE);
    SetDlgItemInt(page, IDC_MARGIN_LEFT,   (UINT)cfg.marginLeft, FALSE);
    SetDlgItemInt(page, IDC_MARGIN_RIGHT,  (UINT)cfg.marginRight, FALSE);

    const wchar_t* modName = (cfg.modMask & MOD_WIN) ? L"Win"
                           : (cfg.modMask & MOD_CONTROL) ? L"Ctrl"
                           : (cfg.modMask & MOD_ALT) ? L"Alt" : L"Shift";
    swprintf_s(buf, L"%s", modName);
    wchar_t hint[200];
    swprintf_s(hint, L"Separate sets of windows on the same screen. "
                     L"Switch with %s+1 ... %s+9.", modName, modName);
    SetDlgItemTextW(page, IDC_WORKSPACE_HINT, hint);

    UpdateLayoutExplanation(page);
}

void PageLayoutSave(HWND page, bool* layoutChanged) {
    Config& cfg = AppConfig();

    const LayoutKind newLayout = SelectedLayout(page);
    const int pct = (int)SendDlgItemMessageW(page, IDC_MASTER_SLIDER, TBM_GETPOS, 0, 0);
    const float newRatio = (std::max)(0.15f, (std::min)(0.85f, pct / 100.0f));

    *layoutChanged = (newLayout != cfg.layout) || (newRatio != cfg.masterRatio);
    cfg.layout      = newLayout;
    cfg.masterRatio = newRatio;

    BOOL ok = FALSE;
    UINT v = GetDlgItemInt(page, IDC_GAP_INNER, &ok, FALSE);
    if (ok) cfg.gapInner = (std::min)(200, (int)v);
    v = GetDlgItemInt(page, IDC_GAP_OUTER, &ok, FALSE);
    if (ok) cfg.gapOuter = (std::min)(200, (int)v);
    v = GetDlgItemInt(page, IDC_WORKSPACES, &ok, FALSE);
    if (ok) cfg.workspaceCount = (std::max)(1, (std::min)(9, (int)v));

    v = GetDlgItemInt(page, IDC_MARGIN_TOP, &ok, FALSE);
    if (ok) cfg.marginTop = (std::min)(400, (int)v);
    v = GetDlgItemInt(page, IDC_MARGIN_BOTTOM, &ok, FALSE);
    if (ok) cfg.marginBottom = (std::min)(400, (int)v);
    v = GetDlgItemInt(page, IDC_MARGIN_LEFT, &ok, FALSE);
    if (ok) cfg.marginLeft = (std::min)(400, (int)v);
    v = GetDlgItemInt(page, IDC_MARGIN_RIGHT, &ok, FALSE);
    if (ok) cfg.marginRight = (std::min)(400, (int)v);
}

INT_PTR CALLBACK PageLayoutProc(HWND page, UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR themed = 0;
    if (theme::DialogMessage(page, msg, wp, lp, &themed)) return themed;

    switch (msg) {
        case WM_INITDIALOG:
            for (int i = 0; i < (int)LayoutKind::COUNT; ++i)
                SendDlgItemMessageW(page, IDC_LAYOUT, CB_ADDSTRING, 0,
                                    (LPARAM)kLayoutInfo[i].name);

            theme::PrepareDialog(page);
            SendDlgItemMessageW(page, IDC_MASTER_SLIDER, TBM_SETRANGE, TRUE,
                                MAKELPARAM(15, 85));
            SendDlgItemMessageW(page, IDC_MASTER_SLIDER, TBM_SETPAGESIZE, 0, 5);
            return TRUE;

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (dis->CtlID == IDC_LAYOUT_PREVIEW) {
                DrawLayoutPreview(dis, SelectedLayout(page));
                return TRUE;
            }
            return FALSE;
        }

        case WM_HSCROLL:
            if ((HWND)lp == GetDlgItem(page, IDC_MASTER_SLIDER)) {
                const int pct = (int)SendDlgItemMessageW(page, IDC_MASTER_SLIDER,
                                                         TBM_GETPOS, 0, 0);
                wchar_t buf[16];
                swprintf_s(buf, L"%d%%", pct);
                SetDlgItemTextW(page, IDC_MASTER_VALUE, buf);
            }
            return TRUE;

        case WM_COMMAND:
            if (LOWORD(wp) == IDC_LAYOUT && HIWORD(wp) == CBN_SELCHANGE) {
                UpdateLayoutExplanation(page);
                return TRUE;
            }
            return FALSE;
    }
    return FALSE;
}

// ================================================================ Behaviour page
namespace {

struct RunningApp { std::wstring proc; std::wstring title; };

std::vector<RunningApp> EnumRunningApps() {
    std::vector<HWND> windows;
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));

    std::vector<RunningApp> apps;
    const DWORD self = GetCurrentProcessId();

    for (HWND h : windows) {
        if (!IsWindowVisible(h)) continue;
        if (GetAncestor(h, GA_ROOT) != h) continue;
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) continue;
        if (IsCloaked(h)) continue;

        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == self) continue;

        const std::wstring title = WindowTitle(h);
        if (title.empty()) continue;
        const std::wstring proc = ProcessName(h);
        if (proc.empty()) continue;

        bool skip = false;
        for (const auto& b : BuiltinIgnoreProcess())
            if (IEquals(b, proc)) { skip = true; break; }
        if (skip) continue;
        for (const auto& a : apps)
            if (IEquals(a.proc, proc)) { skip = true; break; }
        if (skip) continue;

        apps.push_back({ proc, title });
    }

    std::sort(apps.begin(), apps.end(), [](const RunningApp& a, const RunningApp& b) {
        return _wcsicmp(a.proc.c_str(), b.proc.c_str()) < 0;
    });
    return apps;
}

void ExclusionAdd(HWND page, const std::wstring& name) {
    if (name.empty()) return;
    HWND list = GetDlgItem(page, IDC_EXCLUDE_LIST);
    if (SendMessageW(list, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)name.c_str()) != LB_ERR)
        return;
    SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)name.c_str());
}

void ExclusionRemoveSelected(HWND page) {
    HWND list = GetDlgItem(page, IDC_EXCLUDE_LIST);
    const int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return;
    SendMessageW(list, LB_DELETESTRING, sel, 0);
    const int n = (int)SendMessageW(list, LB_GETCOUNT, 0, 0);
    if (n > 0) SendMessageW(list, LB_SETCURSEL, (WPARAM)(std::min)(sel, n - 1), 0);
}

// Milliseconds of travel. Anything under ~120 ms is over before the eye can
// follow it, however many frames it is drawn in.
const int kAnimSpeeds[3] = { 120, 200, 320 };

} // namespace

void PageBehaviourLoad(HWND page) {
    const Config& cfg = AppConfig();

    SetCheck(page, IDC_CHK_BORDER,   cfg.accentBorder);
    SetCheck(page, IDC_CHK_FFM,      cfg.focusFollowsMouse);
    SetCheck(page, IDC_CHK_DRAGSWAP, cfg.dragToRearrange);
    SetCheck(page, IDC_CHK_MODDRAG,  cfg.modDrag);
    SetCheck(page, IDC_CHK_NEWTOP,   cfg.newWindowOnTop);
    SetCheck(page, IDC_CHK_ANIM,     cfg.animations);
    SetCheck(page, IDC_CHK_GAMEPAUSE, cfg.pauseForFullscreen);

    int speed = 1;
    for (int i = 0; i < 3; ++i)
        if (cfg.animationMs <= kAnimSpeeds[i]) { speed = i; break; }
    SendDlgItemMessageW(page, IDC_ANIM_SPEED, CB_SETCURSEL, (WPARAM)speed, 0);
    EnableWindow(GetDlgItem(page, IDC_ANIM_SPEED), cfg.animations);

    SendDlgItemMessageW(page, IDC_EXCLUDE_LIST, LB_RESETCONTENT, 0, 0);
    for (const auto& name : cfg.ignoreProcess) ExclusionAdd(page, name);
}

void PageBehaviourSave(HWND page) {
    Config& cfg = AppConfig();

    cfg.accentBorder      = GetCheck(page, IDC_CHK_BORDER);
    cfg.focusFollowsMouse = GetCheck(page, IDC_CHK_FFM);
    cfg.dragToRearrange   = GetCheck(page, IDC_CHK_DRAGSWAP);
    cfg.modDrag           = GetCheck(page, IDC_CHK_MODDRAG);
    cfg.newWindowOnTop    = GetCheck(page, IDC_CHK_NEWTOP);
    cfg.animations        = GetCheck(page, IDC_CHK_ANIM);
    cfg.pauseForFullscreen = GetCheck(page, IDC_CHK_GAMEPAUSE);

    const int speed = (int)SendDlgItemMessageW(page, IDC_ANIM_SPEED, CB_GETCURSEL, 0, 0);
    if (speed >= 0 && speed < 3) cfg.animationMs = kAnimSpeeds[speed];

    std::vector<std::wstring> excluded;
    HWND list = GetDlgItem(page, IDC_EXCLUDE_LIST);
    const int n = (int)SendMessageW(list, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; ++i) {
        const int len = (int)SendMessageW(list, LB_GETTEXTLEN, i, 0);
        if (len <= 0) continue;
        std::wstring s;
        s.resize((size_t)len + 1);
        const int got = (int)SendMessageW(list, LB_GETTEXT, i, (LPARAM)&s[0]);
        s.resize((size_t)(got > 0 ? got : 0));
        if (!s.empty()) excluded.push_back(s);
    }
    cfg.ignoreProcess = excluded;
}

INT_PTR CALLBACK PageBehaviourProc(HWND page, UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR themed = 0;
    if (theme::DialogMessage(page, msg, wp, lp, &themed)) return themed;

    switch (msg) {
        case WM_INITDIALOG: {
            theme::PrepareDialog(page);
            const wchar_t* speeds[] = { L"Fast", L"Normal", L"Relaxed" };
            for (const wchar_t* s : speeds)
                SendDlgItemMessageW(page, IDC_ANIM_SPEED, CB_ADDSTRING, 0, (LPARAM)s);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_CHK_ANIM:
                    EnableWindow(GetDlgItem(page, IDC_ANIM_SPEED),
                                 GetCheck(page, IDC_CHK_ANIM));
                    return TRUE;

                case IDC_BTN_ADD_RUNNING: {
                    std::vector<PickEntry> entries;
                    for (const auto& a : EnumRunningApps())
                        entries.push_back({ a.proc, a.title, a.proc });

                    std::wstring chosen;
                    if (PickFromList(page, L"Choose an app to leave alone",
                                     L"Apps running right now - type to search:",
                                     entries, &chosen))
                        ExclusionAdd(page, chosen);
                    return TRUE;
                }

                case IDC_BTN_ADD_BROWSE: {
                    std::wstring name;
                    if (BrowseForExeName(page, &name)) ExclusionAdd(page, name);
                    return TRUE;
                }

                case IDC_BTN_REMOVE:
                    ExclusionRemoveSelected(page);
                    return TRUE;

                case IDC_EXCLUDE_LIST:
                    if (HIWORD(wp) == LBN_DBLCLK) ExclusionRemoveSelected(page);
                    return TRUE;
            }
            return FALSE;
    }
    return FALSE;
}


// ================================================================ general page
// Setup and maintenance: how ProWindows starts, where it keeps its settings,
// and what to reach for when something on the desktop looks wrong. The three
// startup boxes moved here from the Behaviour page, which is about how windows
// behave rather than about the application.
void PageGeneralLoad(HWND page) {
    const Config& cfg = AppConfig();

    SetCheck(page, IDC_CHK_STARTMIN,  cfg.startMinimized);
    SetCheck(page, IDC_CHK_AUTOSTART, AppAutostartEnabled());

    // Registering the scheduled task that does this needs administrator
    // rights, so when we do not have them the box shows the real state but
    // cannot be changed - with the reason on the label rather than in a
    // message box after the fact.
    SetCheck(page, IDC_CHK_ELEVAUTO, ElevatedAutostartInstalled());
    if (HWND box = GetDlgItem(page, IDC_CHK_ELEVAUTO)) {
        const bool changeable = SelfIsElevated();
        EnableWindow(box, changeable);
        SetWindowTextW(box, changeable
            ? L"Start as administrator, so windows that run as administrator "
              L"can be arranged too"
            : L"Start as administrator (needs \"Restart as administrator\" from "
              L"the tray menu first)");
    }

    SetDlgItemTextW(page, IDC_GEN_ABOUT, AppAboutText().c_str());
}

void PageGeneralSave(HWND page) {
    AppConfig().startMinimized = GetCheck(page, IDC_CHK_STARTMIN);

    // The elevated logon task is registered with Windows, not stored in the
    // config, so it is applied here rather than saved. Only touched when the
    // box actually changed: creating it costs a schtasks call.
    const bool wantElevated = GetCheck(page, IDC_CHK_ELEVAUTO);
    const bool haveElevated = ElevatedAutostartInstalled();
    if (wantElevated != haveElevated && SelfIsElevated()) {
        if (wantElevated) {
            if (InstallElevatedAutostart()) {
                // The task starts it at logon now; the Run entry would only
                // add a second, unelevated copy.
                AppSetAutostart(false);
                SetCheck(page, IDC_CHK_AUTOSTART, false);
                AppTrayBalloon(kAppName,
                    L"ProWindows will start as administrator at every logon, "
                    L"with no prompt.");
            } else {
                SetCheck(page, IDC_CHK_ELEVAUTO, false);
                AppTrayBalloon(kAppName,
                    L"Windows would not register the logon task.");
            }
        } else if (!RemoveElevatedAutostart()) {
            SetCheck(page, IDC_CHK_ELEVAUTO, true);
        }
    }

    AppSetAutostart(GetCheck(page, IDC_CHK_AUTOSTART));
}

INT_PTR CALLBACK PageGeneralProc(HWND page, UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR themed = 0;
    if (theme::DialogMessage(page, msg, wp, lp, &themed)) return themed;

    switch (msg) {
        case WM_INITDIALOG:
            theme::PrepareDialog(page);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                // These four act immediately rather than on Apply. Every one
                // of them is something you press *because* something looks
                // wrong, and making the fix wait for a second button is the
                // wrong shape for that.
                case IDC_GEN_OPENCFG:     AppOpenConfigFile();        return TRUE;
                case IDC_GEN_OPENDIR:     AppOpenConfigFolder();      return TRUE;
                case IDC_GEN_DIAG:        AppWriteDiagnostics();      return TRUE;
                case IDC_GEN_RESTOREWINS: AppRestoreHiddenWindows();  return TRUE;

                case IDC_GEN_RELOAD:
                    // Reloading throws away whatever is in these controls, so
                    // the pages have to be refilled from what came off disk.
                    AppReloadFromDisk();
                    return TRUE;

                case IDC_GEN_RESET:
                    AppRestoreDefaults(page);
                    return TRUE;
            }
            return FALSE;
    }
    return FALSE;
}

// ================================================================ shell
namespace {

void LayoutPages(HWND dlg) {
    HWND tabs = GetDlgItem(dlg, IDC_TABS);
    RECT r;
    GetWindowRect(tabs, &r);
    MapWindowPoints(nullptr, dlg, (LPPOINT)&r, 2);
    TabCtrl_AdjustRect(tabs, FALSE, &r);

    for (auto& p : g_pages) {
        if (!p.hwnd) continue;
        SetWindowPos(p.hwnd, HWND_TOP, r.left, r.top,
                     r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
    }
}

void ShowPage(int index) {
    // Show the incoming page first: hiding the outgoing one first uncovers the
    // shell for a frame, which reads as a flash on every tab change.
    if (index >= 0 && index < (int)ARRAYSIZE(g_pages) && g_pages[index].hwnd)
        ShowWindow(g_pages[index].hwnd, SW_SHOW);
    for (int i = 0; i < (int)ARRAYSIZE(g_pages); ++i)
        if (i != index && g_pages[i].hwnd)
            ShowWindow(g_pages[i].hwnd, SW_HIDE);
    g_activePage = index;
}

void LoadAllPages() {
    if (g_pages[PAGE_LAYOUT].hwnd)    PageLayoutLoad(g_pages[PAGE_LAYOUT].hwnd);
    if (g_pages[PAGE_BEHAVIOUR].hwnd) PageBehaviourLoad(g_pages[PAGE_BEHAVIOUR].hwnd);
    if (g_pages[PAGE_SHORTCUTS].hwnd) PageShortcutsLoad(g_pages[PAGE_SHORTCUTS].hwnd);
    if (g_pages[PAGE_APPS].hwnd)      PageAppsLoad(g_pages[PAGE_APPS].hwnd);
    if (g_pages[PAGE_SEARCH].hwnd)    PageSearchLoad(g_pages[PAGE_SEARCH].hwnd);
    if (g_pages[PAGE_MONITOR].hwnd)   PageMonitorLoad(g_pages[PAGE_MONITOR].hwnd);
    if (g_pages[PAGE_GENERAL].hwnd)   PageGeneralLoad(g_pages[PAGE_GENERAL].hwnd);
}

void ApplyNow() {
    bool layoutChanged = false;
    if (g_pages[PAGE_LAYOUT].hwnd)    PageLayoutSave(g_pages[PAGE_LAYOUT].hwnd, &layoutChanged);
    if (g_pages[PAGE_BEHAVIOUR].hwnd) PageBehaviourSave(g_pages[PAGE_BEHAVIOUR].hwnd);
    if (g_pages[PAGE_SHORTCUTS].hwnd) PageShortcutsSave(g_pages[PAGE_SHORTCUTS].hwnd);
    if (g_pages[PAGE_APPS].hwnd)      PageAppsSave(g_pages[PAGE_APPS].hwnd);
    if (g_pages[PAGE_SEARCH].hwnd)    PageSearchSave(g_pages[PAGE_SEARCH].hwnd);
    if (g_pages[PAGE_MONITOR].hwnd)   PageMonitorSave(g_pages[PAGE_MONITOR].hwnd);
    if (g_pages[PAGE_GENERAL].hwnd)   PageGeneralSave(g_pages[PAGE_GENERAL].hwnd);

    const Config snapshot = AppConfig();
    AppApplySettings();                 // save to disk + reload + re-register keys

    if (layoutChanged)
        AppWm().SetLayoutEverywhere(snapshot.layout, snapshot.masterRatio,
                                    snapshot.masterCount);

    AppUpdateTray();
    LoadAllPages();
}

} // namespace

void SettingsRefreshStatus() {
    if (!g_dlg) return;

    WindowManager& wm = AppWm();
    const int chords  = AppHotkeyConflicts();
    const int count   = wm.ManagedCount();
    const int refused = wm.BlockedCount();
    const wchar_t* state = wm.GameMode()      ? L"Paused - fullscreen app running"
                         : wm.TilingEnabled() ? L"Tiling active"
                                              : L"Tiling paused";

    wchar_t text[256];
    // Whichever problem there is, say so: a silently missing shortcut and a
    // window that will not move are the two things that otherwise look like
    // the program simply not working.
    if (refused > 0) {
        swprintf_s(text, L"%s  -  %d window%s  -  %d need%s administrator rights",
                   state, count, count == 1 ? L"" : L"s",
                   refused, refused == 1 ? L"s" : L"");
    } else if (chords > 0) {
        swprintf_s(text, L"%s  -  %d window%s  -  %d shortcut%s blocked",
                   state, count, count == 1 ? L"" : L"s",
                   chords, chords == 1 ? L"" : L"s");
    } else {
        swprintf_s(text, L"%s  -  %d window%s arranged",
                   state, count, count == 1 ? L"" : L"s");
    }
    if (GetItemText(g_dlg, IDC_STATUS_TEXT) != text)
        SetDlgItemTextW(g_dlg, IDC_STATUS_TEXT, text);

    const wchar_t* button = wm.TilingEnabled() ? L"&Pause tiling" : L"&Resume tiling";
    if (GetItemText(g_dlg, IDC_TOGGLE_TILING) != button)
        SetDlgItemTextW(g_dlg, IDC_TOGGLE_TILING, button);
}

void SettingsRefresh() {
    if (!g_dlg) return;
    LoadAllPages();
    SettingsRefreshStatus();
}

// The header strip and the oversized title, both of which are measured in the
// dialog's own units and so have to be redone whenever those units change.
// Called after PrepareDialog, never before: PrepareDialog pushes the body font
// onto every child, which used to wipe the title font out again a line after it
// was set.
static void ApplyShellChrome(HWND dlg) {
    if (HFONT base = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0)) {
        LOGFONTW lf{};
        if (GetObjectW(base, sizeof(lf), &lf)) {
            lf.lfWeight = FW_SEMIBOLD;
            lf.lfHeight = (LONG)(lf.lfHeight * 1.35);
            HFONT fresh = CreateFontIndirectW(&lf);
            if (fresh) {
                SendDlgItemMessageW(dlg, IDC_TITLE, WM_SETFONT, (WPARAM)fresh, TRUE);
                // Only once the control has let go of the old one.
                if (g_titleFont) DeleteObject(g_titleFont);
                g_titleFont = fresh;
            }
        }
    }

    // The strip has to reach past the icon and the status line. Taking that
    // from the tab control's own position keeps it right at every DPI and font
    // size, which a fixed pixel count did not.
    if (HWND tabs = GetDlgItem(dlg, IDC_TABS)) {
        RECT tr;
        GetWindowRect(tabs, &tr);
        MapWindowPoints(nullptr, dlg, (LPPOINT)&tr, 2);
        theme::SetHeaderHeight(dlg, tr.top - ScaleDpi(5, DpiForWindow(dlg)));
    }
}

static INT_PTR CALLBACK ShellProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR themed = 0;
    if (theme::DialogMessage(dlg, msg, wp, lp, &themed)) return themed;

    switch (msg) {
        case WM_INITDIALOG: {
            g_dlg = dlg;
            HINSTANCE inst = GetModuleHandleW(nullptr);

            HICON big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON),
                                          IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                          GetSystemMetrics(SM_CYICON), 0);
            HICON small_ = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON),
                                             IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CYSMICON), 0);
            if (big)    SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)big);
            if (small_) SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)small_);

            theme::PrepareDialog(dlg);
            theme::DarkTitleBar(dlg);
            ApplyShellChrome(dlg);

            HWND tabs = GetDlgItem(dlg, IDC_TABS);

            for (int i = 0; i < (int)ARRAYSIZE(g_pages); ++i) {
                TCITEMW item = {};
                item.mask    = TCIF_TEXT;
                item.pszText = const_cast<wchar_t*>(g_pages[i].caption);
                TabCtrl_InsertItem(tabs, i, &item);

                g_pages[i].hwnd = CreateDialogParamW(
                    inst, MAKEINTRESOURCEW(g_pages[i].templateId), dlg,
                    g_pages[i].proc, 0);
            }
            LayoutPages(dlg);
            LoadAllPages();
            ShowPage(0);

            CenterOn(dlg, nullptr);
            SetTimer(dlg, 1, 700, nullptr);
            return TRUE;
        }

        case WM_DPICHANGED:
            // theme::DialogMessage has already re-fonted and re-measured; what
            // is left is the header strip and the pages, which are sized from
            // the tab control rather than from the template.
            ApplyShellChrome(dlg);
            LayoutPages(dlg);
            RedrawWindow(dlg, nullptr, nullptr,
                         RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
            return FALSE;      // let the dialog manager resize the frame

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<const NMHDR*>(lp);
            if (hdr->idFrom == IDC_TABS && hdr->code == TCN_SELCHANGE) {
                ShowPage(TabCtrl_GetCurSel(GetDlgItem(dlg, IDC_TABS)));
                return TRUE;
            }
            return FALSE;
        }

        case WM_TIMER:
            if (wp == 1 && IsWindowVisible(dlg)) SettingsRefreshStatus();
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_APPLY:
                    ApplyNow();
                    return TRUE;
                case IDC_TOGGLE_TILING:
                    AppWm().ActToggleTiling();
                    AppUpdateTray();
                    SettingsRefreshStatus();
                    return TRUE;
                case IDC_BTN_RETILE:
                    AppRetileNow();
                    SettingsRefreshStatus();
                    return TRUE;
                case IDC_BTN_EDITCFG:
                    ApplyNow();                 // keep the file and the UI in step
                    AppOpenConfigFile();
                    return TRUE;
                case IDC_HIDE:
                case IDCANCEL:
                    SettingsHide();
                    return TRUE;
            }
            return FALSE;

        case WM_CLOSE:
            SettingsHide();
            return TRUE;

        case WM_DESTROY:
            theme::ForgetDialog(dlg);
            KillTimer(dlg, 1);
            if (g_titleFont) { DeleteObject(g_titleFont); g_titleFont = nullptr; }
            for (auto& p : g_pages) p.hwnd = nullptr;
            g_dlg = nullptr;
            return TRUE;
    }
    return FALSE;
}

// ================================================================ public API
HWND SettingsOpen(HINSTANCE inst) {
    if (!g_dlg) {
        g_dlg = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_SETTINGS),
                                   nullptr, ShellProc, 0);
        if (!g_dlg) return nullptr;
    } else {
        SettingsRefresh();
    }

    ShowWindow(g_dlg, SW_SHOW);
    if (IsIconic(g_dlg)) ShowWindow(g_dlg, SW_RESTORE);
    SetForegroundWindow(g_dlg);
    return g_dlg;
}

void SettingsOpenTab(int index) {
    HWND dlg = SettingsOpen(GetModuleHandleW(nullptr));
    if (!dlg) return;
    if (index < 0 || index >= (int)ARRAYSIZE(g_pages)) return;
    TabCtrl_SetCurSel(GetDlgItem(dlg, IDC_TABS), index);
    ShowPage(index);
}

void SettingsHide() {
    if (!g_dlg) return;
    ShowWindow(g_dlg, SW_HIDE);
    if (!g_toldAboutTray) {
        g_toldAboutTray = true;
        AppTrayBalloon(kAppName,
                       L"Still running and still arranging windows. "
                       L"Click the tray icon to bring the settings back.");
    }
}

bool SettingsVisible() { return g_dlg && IsWindowVisible(g_dlg); }
HWND SettingsWindow()  { return g_dlg; }

void SettingsDestroy() {
    if (g_dlg) {
        DestroyWindow(g_dlg);
        g_dlg = nullptr;
    }
}

} // namespace awa
