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

// The capture box used to read WM_KEYDOWN off the edit control, which works for
// Alt+H and never for Win+E: the shell has already opened Explorer and eaten
// the keystroke before any window sees it, so the box sat there saying "press
// a key combination" while windows opened around it. Anything ProWindows
// itself had already registered went the same way - the chord fired the
// action instead of being recorded, which made rebinding a taken key
// impossible from the dialog that exists to do it.
//
// So while the dialog is open a low-level keyboard hook sits in front of both.
// It runs on this thread, is installed after the tiler's own hook (and so is
// consulted before it), and it swallows every keystroke aimed at the capture
// box, so the shell never sees Win+E, the tiler never sees Alt+H, and the
// user sees exactly what they pressed. It only reads what it is told: the
// hook posts each key to the dialog and everything else happens there.
HHOOK g_captureHook = nullptr;
HWND  g_captureDlg  = nullptr;      // the dialog the hook is serving
HWND  g_captureBox  = nullptr;      // the control that has to have the focus
UINT  g_captureHeld = 0;            // modifiers down right now, as the hook saw them

// wParam: the key, or 0 when only the modifiers changed. lParam: the modifiers.
constexpr UINT WM_AWA_CAPTUREKEY = WM_APP + 120;

UINT ModifierBitFor(UINT vk) {
    switch (vk) {
        case VK_LWIN: case VK_RWIN:                          return MOD_WIN;
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return MOD_CONTROL;
        case VK_MENU: case VK_LMENU: case VK_RMENU:          return MOD_ALT;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:       return MOD_SHIFT;
        default:                                             return 0;
    }
}

// Is the keystroke for us? Only while the capture box has the focus in the
// foreground dialog: the launcher page's "Program:" field, the OK button and
// every other window on the desktop must keep working exactly as before.
bool CaptureWantsKeys() {
    return g_captureDlg && g_captureBox &&
           GetForegroundWindow() == g_captureDlg && GetFocus() == g_captureBox;
}

LRESULT CALLBACK CaptureHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);

    if (!CaptureWantsKeys()) {
        // Anything held when the focus left is not held any more as far as the
        // recorder is concerned; the release will go to whoever has it now.
        if (g_captureHeld) {
            g_captureHeld = 0;
            if (g_captureDlg) PostMessageW(g_captureDlg, WM_AWA_CAPTUREKEY, 0, 0);
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
    const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
    const bool up   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);
    if (!down && !up) return CallNextHookEx(nullptr, code, wp, lp);

    const UINT vk  = (UINT)kb->vkCode;
    const UINT bit = ModifierBitFor(vk);

    if (bit) {
        if (down) g_captureHeld |= bit;
        else      g_captureHeld &= ~bit;
        PostMessageW(g_captureDlg, WM_AWA_CAPTUREKEY, 0, g_captureHeld);
        // The Win key is the one that must not get through: pressed and
        // released on its own it opens Start, and we have just eaten whatever
        // was pressed in between. The others are harmless to let pass.
        if (bit == MOD_WIN) return 1;
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    // A real key. Record it on the way down, and swallow the release too so no
    // application ever sees half a keystroke.
    if (down) PostMessageW(g_captureDlg, WM_AWA_CAPTUREKEY, vk, g_captureHeld);
    return 1;
}

void CaptureHookInstall(HWND dlg, HWND box) {
    g_captureDlg  = dlg;
    g_captureBox  = box;
    g_captureHeld = 0;
    if (!g_captureHook) {
        g_captureHook = SetWindowsHookExW(WH_KEYBOARD_LL, CaptureHookProc,
                                          GetModuleHandleW(nullptr), 0);
        if (!g_captureHook)
            AWA_LOG(L"shortcut recorder: keyboard hook could not be installed; "
                    L"Win chords will not be captured");
    }
}

void CaptureHookRemove() {
    if (g_captureHook) {
        UnhookWindowsHookEx(g_captureHook);
        g_captureHook = nullptr;
    }
    g_captureDlg  = nullptr;
    g_captureBox  = nullptr;
    g_captureHeld = 0;
}

// What the box shows while modifiers are held and nothing else has been
// pressed yet: "Win + Ctrl + ..." - so the user can see the Win key registered
// before they commit to the rest of the chord.
std::wstring HeldText(UINT mods) {
    std::wstring s;
    if (mods & MOD_WIN)     s += L"Win + ";
    if (mods & MOD_CONTROL) s += L"Ctrl + ";
    if (mods & MOD_ALT)     s += L"Alt + ";
    if (mods & MOD_SHIFT)   s += L"Shift + ";
    return s + L"...";
}

// The recorder's whole job, in one place: take a chord, show it, and explain
// anything the user should know about it before they press OK.
void CaptureAccept(HWND dlg, BindEditCtx* ctx, UINT mods, UINT vk) {
    HWND box = GetDlgItem(dlg, IDC_BINDEDIT_CAPTURE);
    ctx->mods = mods;
    ctx->vk   = vk;
    SetWindowTextW(box, DescribeChord(mods, vk).c_str());

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

// The edit control itself. With the hook in front of it this sees almost
// nothing, but it still has to refuse the dialog manager's interest in Tab and
// Enter (so they can be bound), keep its caret and text to itself, and carry
// on working as a plain recorder if the hook could not be installed.
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
            // The hook has already handled and swallowed it. What reaches here
            // is the modifier keys it lets through, and everything when the
            // hook is missing.
            if (g_captureHook) return 0;

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
            if (ModifierBitFor(vk)) return 0;

            UINT mods = 0;
            if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) < 0)    mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) < 0)   mods |= MOD_SHIFT;
            if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;

            if (ctx) CaptureAccept(dlg, ctx, mods, vk);
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
            CaptureHookInstall(dlg, capture);

            SetWindowTextW(capture, ctx->vk
                ? DescribeChord(ctx->mods, ctx->vk).c_str()
                : L"press a key combination...");

            CenterOn(dlg, GetParent(dlg));
            SetFocus(capture);
            return FALSE;
        }

        case WM_AWA_CAPTUREKEY: {
            if (!ctx) return TRUE;
            const UINT vk   = (UINT)wp;
            const UINT mods = (UINT)lp;
            HWND box = GetDlgItem(dlg, IDC_BINDEDIT_CAPTURE);

            if (vk == 0) {
                // Modifiers only. Show what is held; when the last one goes
                // back up with nothing else pressed, show the chord recorded
                // so far (or the prompt) again.
                if (mods) SetWindowTextW(box, HeldText(mods).c_str());
                else      SetWindowTextW(box, ctx->vk
                              ? DescribeChord(ctx->mods, ctx->vk).c_str()
                              : L"press a key combination...");
                return TRUE;
            }
            // Bare Escape still cancels; Escape with modifiers is a real chord.
            if (vk == VK_ESCAPE && mods == 0) {
                PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
                return TRUE;
            }
            // Bare Tab keeps the dialog usable from the keyboard: nobody binds
            // a window action to the Tab key alone, and without this the OK
            // button cannot be reached without a mouse.
            if (vk == VK_TAB && mods == 0) {
                SetFocus(GetNextDlgTabItem(dlg, box, FALSE));
                return TRUE;
            }
            CaptureAccept(dlg, ctx, mods, vk);
            return TRUE;
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
            CaptureHookRemove();
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

void UpdateMonitorLook(HWND page);

// Moves the metric in row `from` to row `to`, sliding the rows between them
// along by one - a lift and reinsert, not a swap, so dragging the top row to
// the bottom does not leave the middle in a different order from where it
// started. Both buttons and the drag come through here.
void MoveEditRow(int from, int to) {
    if (from < 0 || to < 0 || from >= MON_METRIC_COUNT || to >= MON_METRIC_COUNT ||
        from == to)
        return;
    const int metric = g_editOrder[from];
    const int step = (to > from) ? 1 : -1;
    for (int i = from; i != to; i += step) g_editOrder[i] = g_editOrder[i + step];
    g_editOrder[to] = metric;
}

// The eight rows can be dragged into a new order as well as walked with the
// buttons. Each row's tick box is subclassed: a press that then moves up or
// down by more than a few pixels becomes a drag, and from there the row
// follows the pointer - the list re-sorts live as the pointer crosses each
// row, so what is on screen is always what letting go would give. A press that
// does not move is still a click on the tick.
struct RowDrag {
    HWND  page     = nullptr;
    HWND  ctl      = nullptr;
    bool  pressed  = false;
    bool  dragging = false;
    int   row      = -1;       // where the metric being dragged is now
    POINT origin   = {};       // screen, where the press landed
};
RowDrag g_rowDrag;
WNDPROC g_rowOldProc[MON_METRIC_COUNT] = {};

constexpr int kRowDragSlackPx = 4;

// The row whose tick box spans the pointer's height, or the nearest end.
int RowAtCursor(HWND page) {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(page, &pt);
    int nearest = -1, best = 1 << 30;
    for (int slot = 0; slot < MON_METRIC_COUNT; ++slot) {
        RECT r;
        GetWindowRect(GetDlgItem(page, IDC_MON_SHOW_FIRST + slot), &r);
        MapWindowPoints(nullptr, page, reinterpret_cast<POINT*>(&r), 2);
        if (pt.y >= r.top && pt.y < r.bottom) return slot;
        const int d = (pt.y < r.top) ? r.top - pt.y : pt.y - r.bottom;
        if (d < best) { best = d; nearest = slot; }
    }
    return nearest;
}

void EndRowDrag() {
    if (g_rowDrag.dragging && GetCapture() == g_rowDrag.ctl) ReleaseCapture();
    g_rowDrag.dragging = false;
    g_rowDrag.pressed  = false;
    g_rowDrag.ctl      = nullptr;
}

LRESULT CALLBACK RowDragProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    const int slot = (int)GetWindowLongPtrW(wnd, GWLP_ID) - IDC_MON_SHOW_FIRST;
    WNDPROC old = (slot >= 0 && slot < MON_METRIC_COUNT) ? g_rowOldProc[slot] : nullptr;
    if (!old) return DefWindowProcW(wnd, msg, wp, lp);
    HWND page = GetParent(wnd);

    switch (msg) {
        case WM_LBUTTONDOWN:
            g_rowDrag.page     = page;
            g_rowDrag.ctl      = wnd;
            g_rowDrag.pressed  = true;
            g_rowDrag.dragging = false;
            g_rowDrag.row      = slot;
            GetCursorPos(&g_rowDrag.origin);
            break;                              // the tick still takes the press

        case WM_MOUSEMOVE: {
            if (!g_rowDrag.pressed || g_rowDrag.ctl != wnd) break;
            if (!(wp & MK_LBUTTON)) { g_rowDrag.pressed = false; break; }
            POINT now;
            GetCursorPos(&now);
            if (!g_rowDrag.dragging) {
                if (abs(now.y - g_rowDrag.origin.y) <= kRowDragSlackPx) break;
                // It is a drag. The button has the capture and is holding
                // itself pushed; taking the capture away makes it let go
                // without a click, and then it is ours. `dragging` is set
                // only once the capture is back, because ReleaseCapture
                // delivers WM_CAPTURECHANGED to this very window, and the
                // handler below would otherwise read that as the drag being
                // taken away before it had begun.
                ReleaseCapture();
                SetCapture(wnd);
                g_rowDrag.dragging = true;
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            }
            const int target = RowAtCursor(page);
            if (target >= 0 && target != g_rowDrag.row) {
                MoveEditRow(g_rowDrag.row, target);
                g_rowDrag.row = target;
                g_orderRow    = target;
                RefreshMetricRows(page);
                UpdateMonitorLook(page);
            }
            return 0;
        }

        case WM_SETCURSOR:
            if (g_rowDrag.dragging && g_rowDrag.ctl == wnd) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
            break;

        case WM_LBUTTONUP:
            if (g_rowDrag.dragging && g_rowDrag.ctl == wnd) {
                const int landed = g_rowDrag.row;
                EndRowDrag();
                // The moved row keeps the highlight, as the buttons do.
                SetFocus(GetDlgItem(page, IDC_MON_SHOW_FIRST + landed));
                return 0;                       // not a click: the tick stays
            }
            g_rowDrag.pressed = false;
            break;

        case WM_CAPTURECHANGED:
            if (g_rowDrag.dragging && g_rowDrag.ctl == wnd && (HWND)lp != wnd)
                EndRowDrag();
            break;

        case WM_DESTROY:
            SetWindowLongPtrW(wnd, GWLP_WNDPROC, (LONG_PTR)old);
            g_rowOldProc[slot] = nullptr;
            if (g_rowDrag.ctl == wnd) EndRowDrag();
            break;
    }
    return CallWindowProcW(old, wnd, msg, wp, lp);
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
    hint += L"\r\nDrag the rows on the left into the order you want - or hold "
            L"a readout on the panel itself and drag it.";
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
        theme::Chamfer(dis->hDC, r, 3, theme::PanelAlt, 255, theme::Border, 255);
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
    theme::Chamfer(dis->hDC, r, 8, theme::Bg, 255, theme::Border, 255);

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

            for (int slot = 0; slot < MON_METRIC_COUNT; ++slot) {
                HWND row = GetDlgItem(page, IDC_MON_SHOW_FIRST + slot);
                if (row && !g_rowOldProc[slot])
                    g_rowOldProc[slot] = (WNDPROC)SetWindowLongPtrW(
                        row, GWLP_WNDPROC, (LONG_PTR)RowDragProc);
            }
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
                    MoveEditRow(g_orderRow, to);
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
