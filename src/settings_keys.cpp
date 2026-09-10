// ProWindows - the "Window keys", "Open apps" and "Taskbar" pages.
//
// The two shortcut pages are two views of one edit buffer: window actions on
// one, app launchers on the other, so a clash between them is still caught.
#include "settings_internal.h"
#include "hotkeys.h"
#include "winutil.h"
#include "monitor.h"
#include "montheme.h"
#include "theme.h"

namespace awa {

namespace {

// Nothing reaches the live Config until Apply.
std::vector<Keybind> g_editBinds;
UINT g_editMod = MOD_ALT;

bool IsLaunch(const Keybind& kb) { return kb.action == ACT_LAUNCH; }

const wchar_t* RouteText(BindRoute route) {
    switch (route) {
        case BindRoute::Hook:    return L"via keyboard hook";
        case BindRoute::Blocked: return L"blocked - key already taken";
        default:                 return L"";
    }
}

// Fills a report-mode list with the subset of bindings the page owns. Row
// lParam is the index into g_editBinds, so edits survive filtering.
void FillList(HWND list, bool launchers) {
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);

    int row = 0;
    for (size_t i = 0; i < g_editBinds.size(); ++i) {
        const Keybind& kb = g_editBinds[i];
        if (IsLaunch(kb) != launchers) continue;

        const std::wstring first = launchers ? FriendlyCommandName(kb.command)
                                             : DescribeAction(kb);
        const std::wstring chord = DescribeChord(kb.mods, kb.vk);

        LVITEMW item = {};
        item.mask    = LVIF_TEXT | LVIF_PARAM;
        item.iItem   = row;
        item.pszText = const_cast<wchar_t*>(first.c_str());
        item.lParam  = (LPARAM)i;
        const int inserted = ListView_InsertItem(list, &item);
        if (inserted < 0) continue;

        ListView_SetItemText(list, inserted, 1, const_cast<wchar_t*>(chord.c_str()));
        if (launchers) {
            ListView_SetItemText(list, inserted, 2,
                                 const_cast<wchar_t*>(kb.command.c_str()));
            ListView_SetItemText(list, inserted, 3,
                                 const_cast<wchar_t*>(RouteText(kb.route)));
        } else {
            ListView_SetItemText(list, inserted, 2,
                                 const_cast<wchar_t*>(RouteText(kb.route)));
        }
        ++row;
    }

    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

int SelectedIndex(HWND page, int listId) {
    HWND list = GetDlgItem(page, listId);
    const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (row < 0) return -1;
    LVITEMW item = {};
    item.mask  = LVIF_PARAM;
    item.iItem = row;
    if (!ListView_GetItem(list, &item)) return -1;
    const int index = (int)item.lParam;
    return (index >= 0 && index < (int)g_editBinds.size()) ? index : -1;
}

// ---------------------------------------------------------------- key capture
struct BindEditCtx {
    std::wstring actionLabel;
    bool     isLaunch  = false;
    std::wstring command;
    UINT     mods = 0;
    UINT     vk   = 0;
    const std::vector<Keybind>* existing = nullptr;
    int      selfIndex = -1;
};

WNDPROC g_captureOldProc = nullptr;

LRESULT CALLBACK CaptureProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    HWND dlg = GetParent(wnd);
    auto* ctx = reinterpret_cast<BindEditCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
        // Stop the dialog manager eating Tab / Enter / arrows so they can be bound.
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;

        case WM_CHAR:
        case WM_SYSCHAR:
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const UINT vk = (UINT)wp;
            const bool anyMod =
                (GetKeyState(VK_CONTROL) < 0) || (GetKeyState(VK_MENU) < 0) ||
                (GetKeyState(VK_SHIFT) < 0) ||
                (GetKeyState(VK_LWIN) < 0) || (GetKeyState(VK_RWIN) < 0);

            // Bare Escape still cancels; Escape with modifiers is a real chord.
            if (vk == VK_ESCAPE && !anyMod) {
                PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
                return 0;
            }
            if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT ||
                vk == VK_LWIN || vk == VK_RWIN ||
                vk == VK_LCONTROL || vk == VK_RCONTROL ||
                vk == VK_LMENU || vk == VK_RMENU ||
                vk == VK_LSHIFT || vk == VK_RSHIFT)
                return 0;

            UINT mods = 0;
            if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) < 0)    mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) < 0)   mods |= MOD_SHIFT;
            if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;

            if (ctx) {
                ctx->mods = mods;
                ctx->vk   = vk;
                SetWindowTextW(wnd, DescribeChord(mods, vk).c_str());

                std::wstring note;
                if (mods == 0) {
                    note = L"A shortcut with no modifier will fire while you type. "
                           L"Add Alt, Ctrl or Win.";
                } else if (ctx->existing) {
                    for (size_t i = 0; i < ctx->existing->size(); ++i) {
                        if ((int)i == ctx->selfIndex) continue;
                        const Keybind& other = (*ctx->existing)[i];
                        if (other.mods == mods && other.vk == vk) {
                            note = L"Already used by: " +
                                   (IsLaunch(other) ? FriendlyCommandName(other.command)
                                                    : DescribeAction(other)) +
                                   L". Saving will replace it.";
                            break;
                        }
                    }
                }
                if (note.empty() && (mods & MOD_WIN))
                    note = L"Windows reserves most Win shortcuts. This one will use "
                           L"the keyboard hook.";
                SetDlgItemTextW(dlg, IDC_BINDEDIT_HINT, note.c_str());
            }
            return 0;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP:
            return 0;
    }
    return CallWindowProcW(g_captureOldProc, wnd, msg, wp, lp);
}

INT_PTR CALLBACK BindEditProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<BindEditCtx*>(GetWindowLongPtrW(dlg, DWLP_USER));
    INT_PTR themed = 0;
    if (theme::DialogMessage(dlg, msg, wp, lp, &themed)) return themed;

    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)lp);
            ctx = reinterpret_cast<BindEditCtx*>(lp);

            theme::PrepareDialog(dlg);
            theme::DarkTitleBar(dlg);
            SetDlgItemTextW(dlg, IDC_BINDEDIT_LABEL, ctx->actionLabel.c_str());
            SetDlgItemTextW(dlg, IDC_BINDEDIT_COMMAND, ctx->command.c_str());

            const int show = ctx->isLaunch ? SW_SHOW : SW_HIDE;
            ShowWindow(GetDlgItem(dlg, IDC_BINDEDIT_PROGLABEL), show);
            ShowWindow(GetDlgItem(dlg, IDC_BINDEDIT_COMMAND), show);
            ShowWindow(GetDlgItem(dlg, IDC_BINDEDIT_BROWSE), show);

            HWND capture = GetDlgItem(dlg, IDC_BINDEDIT_CAPTURE);
            g_captureOldProc = (WNDPROC)SetWindowLongPtrW(
                capture, GWLP_WNDPROC, (LONG_PTR)CaptureProc);

            SetWindowTextW(capture, ctx->vk
                ? DescribeChord(ctx->mods, ctx->vk).c_str()
                : L"press a key combination...");

            CenterOn(dlg, GetParent(dlg));
            SetFocus(capture);
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_BINDEDIT_BROWSE: {
                    std::wstring path;
                    if (BrowseForExePath(dlg, &path))
                        SetDlgItemTextW(dlg, IDC_BINDEDIT_COMMAND, path.c_str());
                    return TRUE;
                }
                case IDOK:
                    if (!ctx) return TRUE;
                    if (!ctx->vk) {
                        MessageBoxW(dlg, L"Press the key combination you want first.",
                                    L"Shortcut", MB_OK | MB_ICONINFORMATION);
                        return TRUE;
                    }
                    if (ctx->isLaunch) {
                        ctx->command = Trim(GetItemText(dlg, IDC_BINDEDIT_COMMAND));
                        if (ctx->command.empty()) {
                            MessageBoxW(dlg, L"Choose a program to open.",
                                        L"Shortcut", MB_OK | MB_ICONINFORMATION);
                            return TRUE;
                        }
                    }
                    EndDialog(dlg, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            return FALSE;

        case WM_DESTROY:
            if (g_captureOldProc) {
                SetWindowLongPtrW(GetDlgItem(dlg, IDC_BINDEDIT_CAPTURE),
                                  GWLP_WNDPROC, (LONG_PTR)g_captureOldProc);
                g_captureOldProc = nullptr;
            }
            return TRUE;
    }
    return FALSE;
}

// Removes any other binding on the same chord. Returns how many were dropped
// before `keepIndex`, so the caller can fix up its index.
int RemoveClashes(UINT mods, UINT vk, int keepIndex) {
    int removedBefore = 0;
    for (int i = (int)g_editBinds.size() - 1; i >= 0; --i) {
        if (i == keepIndex) continue;
        if (g_editBinds[(size_t)i].mods == mods && g_editBinds[(size_t)i].vk == vk) {
            g_editBinds.erase(g_editBinds.begin() + i);
            if (i < keepIndex) ++removedBefore;
        }
    }
    return removedBefore;
}

// Shared by both pages: re-key the selected binding.
void ChangeKeyFor(HWND page, int listId) {
    const int index = SelectedIndex(page, listId);
    if (index < 0) {
        MessageBoxW(page, L"Pick a shortcut from the list first.", kAppName,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    Keybind& kb = g_editBinds[(size_t)index];

    BindEditCtx ctx;
    ctx.isLaunch    = IsLaunch(kb);
    ctx.actionLabel = ctx.isLaunch ? FriendlyCommandName(kb.command)
                                   : DescribeAction(kb);
    ctx.command     = kb.command;
    ctx.mods        = kb.mods;
    ctx.vk          = kb.vk;
    ctx.existing    = &g_editBinds;
    ctx.selfIndex   = index;

    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_BINDEDIT),
                        page, BindEditProc, (LPARAM)&ctx) != IDOK)
        return;

    const int shift = RemoveClashes(ctx.mods, ctx.vk, index);
    Keybind& target = g_editBinds[(size_t)(index - shift)];
    target.mods    = ctx.mods;
    target.vk      = ctx.vk;
    target.command = ctx.command;
    target.spec    = KeySpecText(ctx.mods, ctx.vk);
    target.route   = BindRoute::Unregistered;
}

void UpdateAppHint(HWND page) {
    const bool override_ = GetCheck(page, IDC_CHK_OVERRIDE);

    int reserved = 0;
    for (const auto& kb : g_editBinds)
        if (kb.route == BindRoute::Hook || kb.route == BindRoute::Blocked) ++reserved;

    wchar_t text[500];
    if (!override_) {
        swprintf_s(text,
            L"Windows keeps nearly every Win+<key> for itself, so %d of these "
            L"cannot work until this box is ticked. Untick it and use Alt or Ctrl "
            L"shortcuts instead if you would rather not intercept anything.",
            reserved);
    } else {
        swprintf_s(text,
            L"%d shortcut%s currently need this. Only those exact key combinations "
            L"are intercepted - everything else you type goes straight through, and "
            L"it costs nothing while idle.",
            reserved, reserved == 1 ? L"" : L"s");
    }
    SetDlgItemTextW(page, IDC_APP_HINT, text);
}

void UpdateShortcutHint(HWND page) {
    int blocked = 0;
    for (const auto& kb : g_editBinds)
        if (!IsLaunch(kb) && kb.route == BindRoute::Blocked) ++blocked;

    if (blocked > 0) {
        wchar_t text[300];
        swprintf_s(text,
            L"%d shortcut%s could not be registered - another program already owns "
            L"the key. Select it and press Change key to pick a different one.",
            blocked, blocked == 1 ? L"" : L"s");
        SetDlgItemTextW(page, IDC_BIND_HINT, text);
    } else {
        SetDlgItemTextW(page, IDC_BIND_HINT,
            L"These control windows and workspaces. App launchers live on the "
            L"\"Open apps\" tab. Double-click a row to change its key.");
    }
}

} // namespace

// ================================================================ Window keys
void PageShortcutsLoad(HWND page) {
    const Config& cfg = AppConfig();
    g_editBinds = cfg.binds;
    g_editMod   = cfg.modMask;

    int modIndex = 0;
    for (int i = 0; i < 6; ++i)
        if (kModChoices[i].mask == cfg.modMask) { modIndex = i; break; }
    SendDlgItemMessageW(page, IDC_MODIFIER, CB_SETCURSEL, (WPARAM)modIndex, 0);

    FillList(GetDlgItem(page, IDC_BINDLIST), false);
    UpdateShortcutHint(page);
}

void PageShortcutsSave(HWND page) {
    Config& cfg = AppConfig();
    const int modSel = (int)SendDlgItemMessageW(page, IDC_MODIFIER, CB_GETCURSEL, 0, 0);
    if (modSel >= 0 && modSel < 6) cfg.modMask = kModChoices[modSel].mask;
    cfg.binds = g_editBinds;
}

INT_PTR CALLBACK PageShortcutsProc(HWND page, UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR themed = 0;
    if (theme::DialogMessage(page, msg, wp, lp, &themed)) return themed;

    switch (msg) {
        case WM_INITDIALOG: {
            theme::PrepareDialog(page);
            for (const auto& m : kModChoices)
                SendDlgItemMessageW(page, IDC_MODIFIER, CB_ADDSTRING, 0, (LPARAM)m.label);

            HWND list = GetDlgItem(page, IDC_BINDLIST);
            ListView_SetExtendedListViewStyle(list,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            RECT r;
            GetClientRect(list, &r);
            const int total = r.right - r.left;

            LVCOLUMNW col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<wchar_t*>(L"What it does");
            col.cx = (int)(total * 0.46);
            ListView_InsertColumn(list, 0, &col);
            col.pszText = const_cast<wchar_t*>(L"Shortcut");
            col.cx = (int)(total * 0.27);
            ListView_InsertColumn(list, 1, &col);
            col.pszText = const_cast<wchar_t*>(L"Status");
            col.cx = total - (int)(total * 0.73) - 4;
            ListView_InsertColumn(list, 2, &col);
            return TRUE;
        }

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<const NMHDR*>(lp);
            if (hdr->idFrom == IDC_BINDLIST && hdr->code == NM_DBLCLK) {
                ChangeKeyFor(page, IDC_BINDLIST);
                FillList(GetDlgItem(page, IDC_BINDLIST), false);
                UpdateShortcutHint(page);
                return TRUE;
            }
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_MODIFIER:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        const int sel = (int)SendDlgItemMessageW(
                            page, IDC_MODIFIER, CB_GETCURSEL, 0, 0);
                        if (sel >= 0 && sel < 6 && kModChoices[sel].mask != g_editMod) {
                            // Move every shortcut that used the old modifier.
                            const UINT oldMod = g_editMod;
                            const UINT newMod = kModChoices[sel].mask;
                            for (auto& kb : g_editBinds) {
                                if ((kb.mods & oldMod) == oldMod) {
                                    kb.mods = (kb.mods & ~oldMod) | newMod;
                                    kb.spec = KeySpecText(kb.mods, kb.vk);
                                }
                            }
                            g_editMod = newMod;
                            FillList(GetDlgItem(page, IDC_BINDLIST), false);
                        }
                    }
                    return TRUE;

                case IDC_BIND_EDIT:
                    ChangeKeyFor(page, IDC_BINDLIST);
                    FillList(GetDlgItem(page, IDC_BINDLIST), false);
                    UpdateShortcutHint(page);
                    return TRUE;

                case IDC_BIND_REMOVE: {
                    const int index = SelectedIndex(page, IDC_BINDLIST);
                    if (index >= 0) g_editBinds.erase(g_editBinds.begin() + index);
                    FillList(GetDlgItem(page, IDC_BINDLIST), false);
                    return TRUE;
                }

                case IDC_BIND_RESET: {
                    if (MessageBoxW(page,
                            L"Put every window shortcut back to its default key?\n\n"
                            L"Your app shortcuts are not affected.",
                            kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES)
                        return TRUE;

                    // Keep the launchers, replace the rest with fresh defaults.
                    std::vector<Keybind> launchers;
                    for (const auto& kb : g_editBinds)
                        if (IsLaunch(kb)) launchers.push_back(kb);

                    Config fresh;
                    fresh.modMask = g_editMod;
                    fresh.LoadDefaults();

                    g_editBinds.clear();
                    for (const auto& kb : fresh.binds)
                        if (!IsLaunch(kb)) g_editBinds.push_back(kb);
                    for (const auto& kb : launchers) g_editBinds.push_back(kb);

                    FillList(GetDlgItem(page, IDC_BINDLIST), false);
                    UpdateShortcutHint(page);
                    return TRUE;
                }
            }
            return FALSE;
    }
    return FALSE;
}

// ================================================================ Open apps
namespace {

void AddLauncher(HWND page, const std::wstring& command) {
    if (command.empty()) return;

    BindEditCtx ctx;
    ctx.isLaunch    = true;
    ctx.actionLabel = FriendlyCommandName(command);
    ctx.command     = command;
    ctx.existing    = &g_editBinds;

    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_BINDEDIT),
                        page, BindEditProc, (LPARAM)&ctx) != IDOK)
        return;

    RemoveClashes(ctx.mods, ctx.vk, -1);

    Keybind kb;
    kb.mods    = ctx.mods;
    kb.vk      = ctx.vk;
    kb.action  = ACT_LAUNCH;
    kb.command = ctx.command;
    kb.spec    = KeySpecText(ctx.mods, ctx.vk);
    g_editBinds.push_back(kb);

    FillList(GetDlgItem(page, IDC_APPLIST), true);
    UpdateAppHint(page);
}

} // namespace

void PageAppsLoad(HWND page) {
    const Config& cfg = AppConfig();
    // The Window keys page loads first and owns the buffer; only refresh here if
    // it somehow has not run yet.
    if (g_editBinds.empty()) {
        g_editBinds = cfg.binds;
        g_editMod   = cfg.modMask;
    }
    SetCheck(page, IDC_CHK_OVERRIDE, cfg.overrideReserved);
    FillList(GetDlgItem(page, IDC_APPLIST), true);
    UpdateAppHint(page);
}

void PageAppsSave(HWND page) {
    Config& cfg = AppConfig();
    cfg.overrideReserved = GetCheck(page, IDC_CHK_OVERRIDE);
    cfg.binds = g_editBinds;
}

INT_PTR CALLBACK PageAppsProc(HWND page, UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR themed = 0;
    if (theme::DialogMessage(page, msg, wp, lp, &themed)) return themed;

    switch (msg) {
        case WM_INITDIALOG: {
            theme::PrepareDialog(page);
            HWND list = GetDlgItem(page, IDC_APPLIST);
            ListView_SetExtendedListViewStyle(list,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            RECT r;
            GetClientRect(list, &r);
            const int total = r.right - r.left;

            LVCOLUMNW col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<wchar_t*>(L"Opens");
            col.cx = (int)(total * 0.26);
            ListView_InsertColumn(list, 0, &col);
            col.pszText = const_cast<wchar_t*>(L"Shortcut");
            col.cx = (int)(total * 0.20);
            ListView_InsertColumn(list, 1, &col);
            col.pszText = const_cast<wchar_t*>(L"Program");
            col.cx = (int)(total * 0.32);
            ListView_InsertColumn(list, 2, &col);
            col.pszText = const_cast<wchar_t*>(L"Status");
            col.cx = total - (int)(total * 0.78) - 4;
            ListView_InsertColumn(list, 3, &col);
            return TRUE;
        }

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<const NMHDR*>(lp);
            if (hdr->idFrom == IDC_APPLIST && hdr->code == NM_DBLCLK) {
                ChangeKeyFor(page, IDC_APPLIST);
                FillList(GetDlgItem(page, IDC_APPLIST), true);
                UpdateAppHint(page);
                return TRUE;
            }
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_APP_ADD: {
                    std::vector<PickEntry> entries;
                    for (const auto& a : EnumStartMenuApps())
                        entries.push_back({ a.name, L"", a.launchPath });

                    std::wstring chosen;
                    if (PickFromList(page, L"Choose an app to open",
                                     L"Apps installed on this PC - type to search:",
                                     entries, &chosen))
                        AddLauncher(page, chosen);
                    return TRUE;
                }

                case IDC_APP_BROWSE: {
                    std::wstring path;
                    if (BrowseForExePath(page, &path)) AddLauncher(page, path);
                    return TRUE;
                }

                case IDC_APP_EDITKEY:
                    ChangeKeyFor(page, IDC_APPLIST);
                    FillList(GetDlgItem(page, IDC_APPLIST), true);
                    UpdateAppHint(page);
                    return TRUE;

                case IDC_APP_REMOVE: {
                    const int index = SelectedIndex(page, IDC_APPLIST);
                    if (index >= 0) g_editBinds.erase(g_editBinds.begin() + index);
                    FillList(GetDlgItem(page, IDC_APPLIST), true);
                    UpdateAppHint(page);
                    return TRUE;
                }

                case IDC_CHK_OVERRIDE:
                    UpdateAppHint(page);
                    return TRUE;
            }
            return FALSE;
    }
    return FALSE;
}

// ================================================================ monitor page
namespace {

const int kIntervals[] = { 500, 1000, 2000, 3000 };

// The page's own copy of the colour overrides. Like every other control here,
// nothing reaches the live Config until Apply. Initialised to "no override"
// because a swatch can be asked to paint between WM_INITDIALOG and the first
// Load, and a zeroed array would read as an override to black.
COLORREF g_editColors[MON_METRIC_COUNT] = {
    kMonColourFromTheme, kMonColourFromTheme, kMonColourFromTheme,
    kMonColourFromTheme, kMonColourFromTheme, kMonColourFromTheme,
    kMonColourFromTheme, kMonColourFromTheme,
};

// The page's own copy of the readout order, for the same reason: nothing
// reaches the live Config until Apply. Slot n of the eight rows shows metric
// g_editOrder[n], so a row's control id says where it is and the order says
// what is in it.
int g_editOrder[MON_METRIC_COUNT] = { 0, 1, 2, 3, 4, 5, 6, 7 };

// Which row Move up / Move down act on. Follows the focus - both the tick and
// the swatch of a row set it - so "highlighted" means what it looks like it
// means. Kept when focus leaves the group entirely, so tabbing to the buttons
// does not lose the row that was being moved.
int g_orderRow = 0;

// The metric shown in row `slot`, or -1 for a slot that is out of range.
int MetricInRow(int slot) {
    if (slot < 0 || slot >= MON_METRIC_COUNT) return -1;
    const int m = g_editOrder[slot];
    return (m >= 0 && m < MON_METRIC_COUNT) ? m : -1;
}

// Row `slot` of the eight, or -1 when the id is not one of them.
int RowForControl(int id) {
    if (id >= IDC_MON_SHOW_FIRST && id <= IDC_MON_SHOW_LAST)
        return id - IDC_MON_SHOW_FIRST;
    if (id >= IDC_MON_COL_FIRST && id <= IDC_MON_COL_LAST)
        return id - IDC_MON_COL_FIRST;
    return -1;
}

// Which readouts are switched on, by metric. The eight ticks are a *view* of
// this rather than the state itself: a row is a position, and moving a metric
// to a different position must take its tick and its colour with it.
bool g_editShow[MON_METRIC_COUNT] = {};

// Puts each row's label, tick and swatch in step with the order. Called after
// anything that changes either what is shown or where it sits.
void RefreshMetricRows(HWND page) {
    for (int slot = 0; slot < MON_METRIC_COUNT; ++slot) {
        const int m = MetricInRow(slot);
        SetDlgItemTextW(page, IDC_MON_SHOW_FIRST + slot,
                        m >= 0 ? MonitorMetricAt(m).menu : L"");
        SetCheck(page, IDC_MON_SHOW_FIRST + slot, m >= 0 && g_editShow[m]);
        InvalidateRect(GetDlgItem(page, IDC_MON_COL_FIRST + slot), nullptr, FALSE);
    }
    // The ends of the list have nowhere to go, and a button that cannot do
    // anything should say so rather than doing nothing when pressed.
    const bool enabled = GetCheck(page, IDC_MON_ENABLED);
    EnableWindow(GetDlgItem(page, IDC_MON_ORDER_UP), enabled && g_orderRow > 0);
    EnableWindow(GetDlgItem(page, IDC_MON_ORDER_DOWN),
                 enabled && g_orderRow < MON_METRIC_COUNT - 1);
}

int SelectedSkin(HWND page) {
    const int sel = (int)SendDlgItemMessageW(page, IDC_MON_THEME, CB_GETCURSEL, 0, 0);
    return (sel >= 0 && sel < MonitorSkinCount()) ? sel : 0;
}

int SelectedStyle(HWND page) {
    const int sel = (int)SendDlgItemMessageW(page, IDC_MON_STYLE, CB_GETCURSEL, 0, 0);
    return (sel >= 0 && sel < MonitorStyleCount()) ? sel : 0;
}

// What the preview should draw: the controls as they stand, never the live
// config, so the page always shows what Apply is about to do.
MonitorPreview PreviewFromPage(HWND page) {
    MonitorPreview look;
    look.colors   = g_editColors;
    look.theme    = SelectedSkin(page);
    look.style    = SelectedStyle(page);
    look.graphs   = GetCheck(page, IDC_MON_GRAPHS);
    look.topApps  = GetCheck(page, IDC_MON_TOPAPP);
    look.vertical = GetCheck(page, IDC_MON_VERTICAL);
    look.opacity  = (int)SendDlgItemMessageW(page, IDC_MON_OPACITY, TBM_GETPOS, 0, 0);
    if (look.opacity <= 0) look.opacity = 92;
    return look;
}

void UpdateMonitorEnabling(HWND page) {
    const bool on = GetCheck(page, IDC_MON_ENABLED);
    const int gated[] = { IDC_MON_PINNED, IDC_MON_DESKTOP, IDC_MON_RESET_POS,
                          IDC_MON_STYLE, IDC_MON_GRAPHS, IDC_MON_TOPAPP,
                          IDC_MON_VERTICAL, IDC_MON_OPACITY, IDC_MON_SCALE,
                          IDC_MON_INTERVAL, IDC_MON_THEME, IDC_MON_PREVIEW,
                          IDC_MON_COL_RESET };
    for (int id : gated) EnableWindow(GetDlgItem(page, id), on);
    for (int id = IDC_MON_COL_FIRST; id <= IDC_MON_COL_LAST; ++id)
        EnableWindow(GetDlgItem(page, id), on);
    for (int id = IDC_MON_SHOW_FIRST; id <= IDC_MON_SHOW_LAST; ++id)
        EnableWindow(GetDlgItem(page, id), on);
    EnableWindow(GetDlgItem(page, IDC_MON_ORDER_UP), on && g_orderRow > 0);
    EnableWindow(GetDlgItem(page, IDC_MON_ORDER_DOWN),
                 on && g_orderRow < MON_METRIC_COUNT - 1);

    // Not every style draws a history curve. Which ones do is montheme's
    // business, not this page's - keeping a second list here is how the box
    // ended up greyed out for a style that does use graphs.
    const int style = SelectedStyle(page);
    EnableWindow(GetDlgItem(page, IDC_MON_GRAPHS),
                 on && MonitorStyleUsesGraphs(style));

    std::wstring hint = MonitorStyleAt(style).blurb;
    hint += L"\r\n";
    if (GetCheck(page, IDC_MON_DESKTOP))
        hint += L"On the desktop: behind every window - visible on a clear "
                L"desktop, covered as soon as you open something.";
    else if (GetCheck(page, IDC_MON_PINNED))
        hint += L"Pinned: it cannot be dragged, and clicks pass through to "
                L"whatever is behind it.";
    else
        hint += L"Drag it anywhere, then pin it so it cannot be moved by "
                L"accident. Right-click it for all of this.";
    SetDlgItemTextW(page, IDC_MON_HINT, hint.c_str());
}

void RedrawSwatches(HWND page) {
    for (int id = IDC_MON_COL_FIRST; id <= IDC_MON_COL_LAST; ++id)
        InvalidateRect(GetDlgItem(page, id), nullptr, FALSE);
}

void UpdateMonitorLook(HWND page) {
    SetDlgItemTextW(page, IDC_MON_THEME_DESC, MonitorSkinAt(SelectedSkin(page)).blurb);
    InvalidateRect(GetDlgItem(page, IDC_MON_PREVIEW), nullptr, TRUE);
}

// The sample sits on a scrap of "desktop" so a translucent panel reads as
// translucent rather than as a slightly different flat colour. The picture
// comes from the overlay's own painter, so it cannot drift from the real thing.
// A swatch shows the colour the metric will actually paint with, so a metric
// left on "theme" still shows the theme's colour rather than a blank.
void DrawColourSwatch(const DRAWITEMSTRUCT* dis, HWND page) {
    // The swatch id is a row, and the row's metric comes from the order.
    const int metric = MetricInRow((int)dis->CtlID - IDC_MON_COL_FIRST);
    if (metric < 0) return;

    const MonitorSkin& skin = MonitorSkinAt(SelectedSkin(page));
    const COLORREF colour = MonitorMetricColour(skin, metric, g_editColors[metric]);
    const bool custom = (g_editColors[metric] != kMonColourFromTheme);
    const bool focused = (GetFocus() == dis->hwndItem);

    RECT r = dis->rcItem;
    FillRect(dis->hDC, &r, theme::BrushPanel());
    if (!IsWindowEnabled(dis->hwndItem)) {
        theme::RoundRect(dis->hDC, r, 4, theme::PanelAlt, theme::Border, true);
        return;
    }

    // A custom colour gets a brighter rim, so "overridden" is visible without
    // having to remember what the theme's own colour looked like.
    theme::RoundRect(dis->hDC, r, 4, colour,
                     custom ? RGB(235, 238, 245) : theme::Border, true);
    if (focused || (dis->itemState & ODS_SELECTED))
        theme::RoundRectAlpha(dis->hDC, r, 4, 0, 0, RGB(255, 255, 255), 210);
}

void DrawMonitorPreview(const DRAWITEMSTRUCT* dis, HWND page) {
    RECT r = dis->rcItem;
    FillRect(dis->hDC, &r, theme::BrushPanel());
    theme::RoundRect(dis->hDC, r, 6, theme::Bg, theme::Border, true);

    RECT inner = { r.left + 6, r.top + 5, r.right - 6, r.bottom - 5 };
    MonitorDrawPreview(dis->hDC, inner, PreviewFromPage(page));
}

} // namespace

void PageMonitorLoad(HWND page) {
    const Config& cfg = AppConfig();

    SetCheck(page, IDC_MON_ENABLED,  cfg.monitorEnabled);
    SetCheck(page, IDC_MON_PINNED,   cfg.monitorPinned);
    SetCheck(page, IDC_MON_DESKTOP,  cfg.monitorOnDesktop);
    SetCheck(page, IDC_MON_GRAPHS,   cfg.monitorGraphs);
    SetCheck(page, IDC_MON_TOPAPP,   cfg.monitorTopApps);
    SetCheck(page, IDC_MON_VERTICAL, cfg.monitorVertical);
    g_editShow[MON_CPU]     = cfg.monShowCpu;
    g_editShow[MON_RAM]     = cfg.monShowRam;
    g_editShow[MON_GPU]     = cfg.monShowGpu;
    g_editShow[MON_VRAM]    = cfg.monShowVram;
    g_editShow[MON_CPUTEMP] = cfg.monShowCpuTemp;
    g_editShow[MON_GPUTEMP] = cfg.monShowGpuTemp;
    g_editShow[MON_DISK]    = cfg.monShowDisk;
    g_editShow[MON_NET]     = cfg.monShowNet;

    for (int i = 0; i < MON_METRIC_COUNT; ++i) {
        g_editColors[i] = cfg.monColor[i];
        g_editOrder[i]  = cfg.monOrder[i];
    }
    // A config file can name a metric twice or leave one out; the eight rows
    // cannot. Repair a copy rather than trusting one.
    MonitorNormaliseOrder(g_editOrder);
    RefreshMetricRows(page);

    SendDlgItemMessageW(page, IDC_MON_THEME, CB_SETCURSEL,
                        (WPARAM)cfg.monitorTheme, 0);
    SendDlgItemMessageW(page, IDC_MON_STYLE, CB_SETCURSEL,
                        (WPARAM)cfg.monitorStyle, 0);

    SendDlgItemMessageW(page, IDC_MON_OPACITY, TBM_SETPOS, TRUE, cfg.monitorOpacity);
    SendDlgItemMessageW(page, IDC_MON_SCALE, TBM_SETPOS, TRUE, cfg.monitorScale);

    wchar_t buf[16];
    swprintf_s(buf, L"%d%%", cfg.monitorOpacity);
    SetDlgItemTextW(page, IDC_MON_OPACITY_VAL, buf);
    swprintf_s(buf, L"%d%%", cfg.monitorScale);
    SetDlgItemTextW(page, IDC_MON_SCALE_VAL, buf);

    int sel = 1;
    for (int i = 0; i < 4; ++i)
        if (kIntervals[i] == cfg.monitorInterval) { sel = i; break; }
    SendDlgItemMessageW(page, IDC_MON_INTERVAL, CB_SETCURSEL, (WPARAM)sel, 0);

    UpdateMonitorLook(page);
    UpdateMonitorEnabling(page);
}

void PageMonitorSave(HWND page) {
    Config& cfg = AppConfig();

    cfg.monitorEnabled   = GetCheck(page, IDC_MON_ENABLED);
    cfg.monitorPinned    = GetCheck(page, IDC_MON_PINNED);
    cfg.monitorOnDesktop = GetCheck(page, IDC_MON_DESKTOP);
    cfg.monitorGraphs    = GetCheck(page, IDC_MON_GRAPHS);
    cfg.monitorTopApps   = GetCheck(page, IDC_MON_TOPAPP);
    cfg.monitorVertical  = GetCheck(page, IDC_MON_VERTICAL);
    cfg.monShowCpu       = g_editShow[MON_CPU];
    cfg.monShowRam       = g_editShow[MON_RAM];
    cfg.monShowGpu       = g_editShow[MON_GPU];
    cfg.monShowVram      = g_editShow[MON_VRAM];
    cfg.monShowCpuTemp   = g_editShow[MON_CPUTEMP];
    cfg.monShowGpuTemp   = g_editShow[MON_GPUTEMP];
    cfg.monShowDisk      = g_editShow[MON_DISK];
    cfg.monShowNet       = g_editShow[MON_NET];
    cfg.monitorTheme     = SelectedSkin(page);
    cfg.monitorStyle     = SelectedStyle(page);
    for (int i = 0; i < MON_METRIC_COUNT; ++i) {
        cfg.monColor[i] = g_editColors[i];
        cfg.monOrder[i] = g_editOrder[i];
    }

    cfg.monitorOpacity = (int)SendDlgItemMessageW(page, IDC_MON_OPACITY, TBM_GETPOS, 0, 0);
    cfg.monitorScale   = (int)SendDlgItemMessageW(page, IDC_MON_SCALE, TBM_GETPOS, 0, 0);

    const int sel = (int)SendDlgItemMessageW(page, IDC_MON_INTERVAL, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < 4) cfg.monitorInterval = kIntervals[sel];
}

INT_PTR CALLBACK PageMonitorProc(HWND page, UINT msg, WPARAM wp, LPARAM lp) {
    // The swatches are owner-drawn buttons, and the theme's generic button
    // painter would claim them first, so they are handled ahead of it.
    if (msg == WM_DRAWITEM) {
        auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (dis->CtlType == ODT_BUTTON &&
            (int)dis->CtlID >= IDC_MON_COL_FIRST &&
            (int)dis->CtlID <= IDC_MON_COL_LAST) {
            DrawColourSwatch(dis, page);
            return TRUE;
        }
    }

    INT_PTR themed = 0;
    if (theme::DialogMessage(page, msg, wp, lp, &themed)) return themed;

    switch (msg) {
        case WM_INITDIALOG: {
            theme::PrepareDialog(page);
            const wchar_t* rates[] = { L"half a second", L"1 second",
                                       L"2 seconds", L"3 seconds" };
            for (const wchar_t* r : rates)
                SendDlgItemMessageW(page, IDC_MON_INTERVAL, CB_ADDSTRING, 0, (LPARAM)r);

            for (int i = 0; i < MonitorSkinCount(); ++i)
                SendDlgItemMessageW(page, IDC_MON_THEME, CB_ADDSTRING, 0,
                                    (LPARAM)MonitorSkinAt(i).name);

            for (int i = 0; i < MonitorStyleCount(); ++i)
                SendDlgItemMessageW(page, IDC_MON_STYLE, CB_ADDSTRING, 0,
                                    (LPARAM)MonitorStyleAt(i).name);

            SendDlgItemMessageW(page, IDC_MON_OPACITY, TBM_SETRANGE, TRUE,
                                MAKELPARAM(20, 100));   // matches config.cpp
            SendDlgItemMessageW(page, IDC_MON_SCALE, TBM_SETRANGE, TRUE,
                                MAKELPARAM(75, 175));
            return TRUE;
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (dis->CtlID == IDC_MON_PREVIEW) {
                DrawMonitorPreview(dis, page);
                return TRUE;
            }
            return FALSE;
        }

        case WM_HSCROLL: {
            wchar_t buf[16];
            if ((HWND)lp == GetDlgItem(page, IDC_MON_OPACITY)) {
                swprintf_s(buf, L"%d%%", (int)SendDlgItemMessageW(
                    page, IDC_MON_OPACITY, TBM_GETPOS, 0, 0));
                SetDlgItemTextW(page, IDC_MON_OPACITY_VAL, buf);
                InvalidateRect(GetDlgItem(page, IDC_MON_PREVIEW), nullptr, TRUE);
            } else if ((HWND)lp == GetDlgItem(page, IDC_MON_SCALE)) {
                swprintf_s(buf, L"%d%%", (int)SendDlgItemMessageW(
                    page, IDC_MON_SCALE, TBM_GETPOS, 0, 0));
                SetDlgItemTextW(page, IDC_MON_SCALE_VAL, buf);
            }
            return TRUE;
        }

        case WM_COMMAND: {
            // The eight rows are ranges, not named cases: a tick, a swatch,
            // and either of them taking focus, all of which have to say which
            // row Move up / Move down should act on.
            const int id  = (int)LOWORD(wp);
            const int row = RowForControl(id);
            if (row >= 0) {
                const int metric = MetricInRow(row);
                if (HIWORD(wp) == BN_SETFOCUS) {
                    g_orderRow = row;
                    RefreshMetricRows(page);
                    return TRUE;
                }
                if (HIWORD(wp) == BN_CLICKED && metric >= 0) {
                    g_orderRow = row;
                    if (id <= IDC_MON_SHOW_LAST && id >= IDC_MON_SHOW_FIRST) {
                        g_editShow[metric] = GetCheck(page, id);
                    } else {
                        const MonitorSkin& skin = MonitorSkinAt(SelectedSkin(page));
                        COLORREF colour = MonitorMetricColour(
                            skin, metric, g_editColors[metric]);
                        if (PickColor(page, &colour)) g_editColors[metric] = colour;
                    }
                    RefreshMetricRows(page);
                    UpdateMonitorLook(page);
                    return TRUE;
                }
            }
            switch (LOWORD(wp)) {
                case IDC_MON_ENABLED:
                case IDC_MON_PINNED:
                case IDC_MON_DESKTOP:
                    UpdateMonitorEnabling(page);
                    return TRUE;
                case IDC_MON_GRAPHS:
                case IDC_MON_TOPAPP:
                case IDC_MON_VERTICAL:
                    UpdateMonitorLook(page);
                    return TRUE;
                case IDC_MON_THEME:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        RedrawSwatches(page);   // un-overridden ones follow it
                        UpdateMonitorLook(page);
                    }
                    return TRUE;
                case IDC_MON_STYLE:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        UpdateMonitorEnabling(page);   // graphs only apply to some
                        UpdateMonitorLook(page);
                    }
                    return TRUE;

                case IDC_MON_COL_RESET:
                    for (int i = 0; i < MON_METRIC_COUNT; ++i)
                        g_editColors[i] = kMonColourFromTheme;
                    RedrawSwatches(page);
                    UpdateMonitorLook(page);
                    return TRUE;

                case IDC_MON_ORDER_UP:
                case IDC_MON_ORDER_DOWN: {
                    const int to = g_orderRow +
                                   (LOWORD(wp) == IDC_MON_ORDER_UP ? -1 : 1);
                    if (to < 0 || to >= MON_METRIC_COUNT) return TRUE;
                    std::swap(g_editOrder[g_orderRow], g_editOrder[to]);
                    g_orderRow = to;   // the row follows the metric it moved
                    RefreshMetricRows(page);
                    UpdateMonitorLook(page);
                    // Keep the moved row highlighted, so a second press
                    // carries on rather than needing the mouse again.
                    SetFocus(GetDlgItem(page, IDC_MON_SHOW_FIRST + to));
                    return TRUE;
                }

                case IDC_MON_RESET_POS: {
                    Config& cfg = AppConfig();
                    cfg.monitorX = INT_MIN;
                    cfg.monitorY = INT_MIN;
                    MonitorApplyConfig();
                    AppSaveConfig();
                    return TRUE;
                }
            }
            return FALSE;
        }
    }
    return FALSE;
}

} // namespace awa
