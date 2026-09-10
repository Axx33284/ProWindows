// The Search tab: which sources the search bar draws on, and how the file
// index is built. Follows the same Load()/Save() contract as every other page
// (settings_internal.h) - nothing here writes to the Config until Apply.
#include "settings_internal.h"
#include "search.h"
#include "theme.h"
#include <shlobj.h>

namespace awa {

namespace {

// Offered ceilings for the index. Bigger is not better: each entry costs
// roughly 300 bytes held for the life of the process, and past a point you are
// scrolling rather than searching.
const int kEntryCaps[] = { 5000, 10000, 20000, 50000, 100000 };

// How far below each root to walk. Deeper finds more and costs more; past
// about eight levels you are indexing build output, not documents.
const int kDepths[] = { 2, 3, 4, 5, 6, 8, 12 };

// The folder list being edited. Empty means "the defaults", which is stored as
// an empty list rather than expanded - see Config::searchFolders.
std::vector<std::wstring> g_editFolders;

void FillFolderList(HWND page) {
    HWND list = GetDlgItem(page, IDC_SRCH_FOLDERS);
    if (!list) return;

    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);

    // An empty list means the defaults, so show what those actually resolve to
    // rather than an empty box the user cannot make sense of.
    const std::vector<std::wstring> show =
        g_editFolders.empty() ? SearchDefaultFolders() : g_editFolders;

    int row = 0;
    for (const auto& folder : show) {
        LVITEMW item = {};
        item.mask     = LVIF_TEXT;
        item.iItem    = row++;
        item.pszText  = const_cast<wchar_t*>(folder.c_str());
        ListView_InsertItem(list, &item);
    }

    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);

    // "Remove" only means something for a list the user actually chose.
    EnableWindow(GetDlgItem(page, IDC_SRCH_REMOVE), !g_editFolders.empty());
    EnableWindow(GetDlgItem(page, IDC_SRCH_DEFAULTS), !g_editFolders.empty());
}

// Names the drives the program walk will actually visit, so ticking the box
// says something concrete rather than making a promise the machine may not
// keep. Fixed disks only - a USB stick or a mapped share is not walked, and
// the row saying so is cheaper than a support question about it.
void UpdateDriveList(HWND page) {
    if (!GetCheck(page, IDC_SRCH_DRIVES)) {
        SetDlgItemTextW(page, IDC_SRCH_DRIVELIST,
                        L"Program Files and %LOCALAPPDATA%\\Programs only.");
        return;
    }

    std::wstring drives;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const std::wstring root = std::wstring(1, (wchar_t)(L'A' + i)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        if (!drives.empty()) drives += L"  ";
        drives += root.substr(0, 2);
    }
    SetDlgItemTextW(page, IDC_SRCH_DRIVELIST,
                    drives.empty()
                        ? L"No fixed drives found besides the system one."
                        : (L"Fixed drives found: " + drives).c_str());
}

void UpdateStatus(HWND page) {
    const bool on = GetCheck(page, IDC_SRCH_FILES);

    wchar_t text[200] = L"";
    if (!on) {
        wcscpy_s(text, L"File search is off, so no index is held.");
    } else if (!SearchIndexReady()) {
        wcscpy_s(text, L"Indexing your files...");
    } else {
        const int n = SearchIndexCount();
        // The memory figure is the honest reason there is a ceiling at all.
        swprintf_s(text, L"%d files and folders indexed, about %d MB.",
                   n, (std::max)(1, n * 300 / (1024 * 1024)));
    }
    SetDlgItemTextW(page, IDC_SRCH_STATUS, text);
}

// Everything below the "Files, folders and programs" tick only matters when it
// is on.
//
// The folder list itself is deliberately *not* in that set. A disabled list
// view paints its own background with the system window colour and ignores
// every colour we have set on it, so switching file search off - the default -
// put a bright white rectangle in the middle of a black page. It is also the
// one control here worth reading while the feature is off, since it is what
// the feature would index if it were on. The buttons around it still grey out,
// which is what says the list is not in use.
void UpdateEnabling(HWND page) {
    const bool on = GetCheck(page, IDC_SRCH_FILES);
    const int gated[] = {
        IDC_SRCH_ADD, IDC_SRCH_DEPTH,
        IDC_SRCH_MAXENTRIES, IDC_SRCH_HIDDEN, IDC_SRCH_REINDEX,
        IDC_SRCH_DRIVES, IDC_SRCH_DEEPEXE,
    };
    for (int id : gated) EnableWindow(GetDlgItem(page, id), on);
    UpdateDriveList(page);

    EnableWindow(GetDlgItem(page, IDC_SRCH_REMOVE), on && !g_editFolders.empty());
    EnableWindow(GetDlgItem(page, IDC_SRCH_DEFAULTS), on && !g_editFolders.empty());
    UpdateStatus(page);
}

void UpdateHint(HWND page) {
    const bool calc = GetCheck(page, IDC_SRCH_CALC);
    const bool cmd  = GetCheck(page, IDC_SRCH_COMMANDS);

    const wchar_t* text =
        (calc && cmd) ? L"A sum jumps to the top of the list; anything that looks "
                        L"like a path, a URL or a command line offers to run as typed."
      : calc          ? L"Type something like 1920*0.75 and the answer is the first result."
      : cmd           ? L"Anything that looks like a path, a URL or a command line "
                        L"offers to run as typed."
                      : L"Only apps, and whatever else is ticked above, are searched.";
    SetDlgItemTextW(page, IDC_SRCH_HINT, text);
}

// The shell's folder picker.
bool BrowseForFolder(HWND parent, std::wstring* chosen) {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))) || !dialog)
        return false;

    bool picked = false;
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Choose a folder to index");

    if (SUCCEEDED(dialog->Show(parent))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                *chosen = path;
                picked = true;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return picked;
}

void AddFolder(HWND page) {
    std::wstring folder;
    if (!BrowseForFolder(page, &folder) || folder.empty()) return;

    for (const auto& have : g_editFolders)
        if (IEquals(have, folder)) return;      // already listed

    // Note that the first folder added replaces the defaults rather than
    // joining them, because an empty list is what *means* "the defaults".
    // "Use the default folders" puts them back.
    g_editFolders.push_back(folder);
    FillFolderList(page);
    UpdateEnabling(page);
}

void RemoveFolder(HWND page) {
    if (g_editFolders.empty()) return;
    HWND list = GetDlgItem(page, IDC_SRCH_FOLDERS);
    const int sel = list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    if (sel < 0 || sel >= (int)g_editFolders.size()) {
        MessageBoxW(page, L"Pick a folder from the list first.", kAppName,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    g_editFolders.erase(g_editFolders.begin() + sel);
    FillFolderList(page);
    UpdateEnabling(page);
}

} // namespace

// ---------------------------------------------------------------- page proc
INT_PTR CALLBACK PageSearchProc(HWND page, UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR themed = 0;
    if (theme::DialogMessage(page, msg, wp, lp, &themed)) return themed;

    switch (msg) {
        case WM_INITDIALOG: {
            // Hides the group boxes so the theme can paint them as cards, and
            // makes the combos and the list dark. Every other page does this;
            // this one was missing it and rendered as a light-mode dialog.
            theme::PrepareDialog(page);

            HWND list = GetDlgItem(page, IDC_SRCH_FOLDERS);
            if (list) {
                ListView_SetExtendedListViewStyle(
                    list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
                RECT r;
                GetClientRect(list, &r);
                LVCOLUMNW col = {};
                col.mask = LVCF_WIDTH | LVCF_TEXT;
                col.cx   = r.right - GetSystemMetrics(SM_CXVSCROLL) - 4;
                col.pszText = const_cast<wchar_t*>(L"Folder");
                ListView_InsertColumn(list, 0, &col);
            }

            for (int depth : kDepths) {
                wchar_t label[32];
                swprintf_s(label, L"%d folders", depth);
                SendDlgItemMessageW(page, IDC_SRCH_DEPTH, CB_ADDSTRING, 0,
                                    (LPARAM)label);
            }
            for (int cap : kEntryCaps) {
                wchar_t label[32];
                swprintf_s(label, L"%d items", cap);
                SendDlgItemMessageW(page, IDC_SRCH_MAXENTRIES, CB_ADDSTRING, 0,
                                    (LPARAM)label);
            }
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_SRCH_ADD:      AddFolder(page); return TRUE;
                case IDC_SRCH_REMOVE:   RemoveFolder(page); return TRUE;
                case IDC_SRCH_DEFAULTS:
                    g_editFolders.clear();
                    FillFolderList(page);
                    UpdateEnabling(page);
                    return TRUE;
                case IDC_SRCH_REINDEX:
                    // Deliberately works on what is saved, not on what is on
                    // screen: re-walking for settings the user has not applied
                    // would build an index that does not match anything.
                    SearchReindex();
                    UpdateStatus(page);
                    return TRUE;
                case IDC_SRCH_DRIVES:
                    if (HIWORD(wp) == BN_CLICKED) UpdateDriveList(page);
                    return TRUE;
                case IDC_SRCH_FILES:
                    if (HIWORD(wp) == BN_CLICKED) UpdateEnabling(page);
                    return TRUE;
                case IDC_SRCH_CALC:
                case IDC_SRCH_COMMANDS:
                    if (HIWORD(wp) == BN_CLICKED) UpdateHint(page);
                    return TRUE;
                default:
                    break;
            }
            break;

        case WM_NOTIFY:
            if (((LPNMHDR)lp)->idFrom == IDC_SRCH_FOLDERS &&
                ((LPNMHDR)lp)->code == NM_DBLCLK) {
                RemoveFolder(page);
                return TRUE;
            }
            break;

        default:
            break;
    }
    return FALSE;
}

void PageSearchLoad(HWND page) {
    const Config& cfg = AppConfig();

    SetCheck(page, IDC_SRCH_FILES,    cfg.searchFiles);
    SetCheck(page, IDC_SRCH_SETTINGS, cfg.searchSettings);
    SetCheck(page, IDC_SRCH_CALC,     cfg.searchCalc);
    SetCheck(page, IDC_SRCH_COMMANDS, cfg.searchCommands);
    SetCheck(page, IDC_SRCH_HIDDEN,   cfg.searchHidden);
    SetCheck(page, IDC_SRCH_DRIVES,   cfg.searchDrives);
    SetCheck(page, IDC_SRCH_DEEPEXE,  cfg.searchDeepExe);

    g_editFolders = cfg.searchFolders;
    FillFolderList(page);

    // Nearest offered value at or above what is configured, so a hand-edited
    // number in the file never silently becomes something smaller.
    int depthSel = (int)ARRAYSIZE(kDepths) - 1;
    for (int i = 0; i < (int)ARRAYSIZE(kDepths); ++i)
        if (kDepths[i] >= cfg.searchDepth) { depthSel = i; break; }
    SendDlgItemMessageW(page, IDC_SRCH_DEPTH, CB_SETCURSEL, (WPARAM)depthSel, 0);

    int capSel = (int)ARRAYSIZE(kEntryCaps) - 1;
    for (int i = 0; i < (int)ARRAYSIZE(kEntryCaps); ++i)
        if (kEntryCaps[i] >= cfg.searchMaxEntries) { capSel = i; break; }
    SendDlgItemMessageW(page, IDC_SRCH_MAXENTRIES, CB_SETCURSEL, (WPARAM)capSel, 0);

    UpdateHint(page);
    UpdateEnabling(page);
}

void PageSearchSave(HWND page) {
    Config& cfg = AppConfig();

    cfg.searchFiles    = GetCheck(page, IDC_SRCH_FILES);
    cfg.searchSettings = GetCheck(page, IDC_SRCH_SETTINGS);
    cfg.searchCalc     = GetCheck(page, IDC_SRCH_CALC);
    cfg.searchCommands = GetCheck(page, IDC_SRCH_COMMANDS);
    cfg.searchHidden   = GetCheck(page, IDC_SRCH_HIDDEN);
    cfg.searchDrives   = GetCheck(page, IDC_SRCH_DRIVES);
    cfg.searchDeepExe  = GetCheck(page, IDC_SRCH_DEEPEXE);
    cfg.searchFolders  = g_editFolders;

    const int depthSel = (int)SendDlgItemMessageW(page, IDC_SRCH_DEPTH,
                                                  CB_GETCURSEL, 0, 0);
    if (depthSel >= 0 && depthSel < (int)ARRAYSIZE(kDepths))
        cfg.searchDepth = kDepths[depthSel];

    const int capSel = (int)SendDlgItemMessageW(page, IDC_SRCH_MAXENTRIES,
                                                CB_GETCURSEL, 0, 0);
    if (capSel >= 0 && capSel < (int)ARRAYSIZE(kEntryCaps))
        cfg.searchMaxEntries = kEntryCaps[capSel];
}

} // namespace awa
