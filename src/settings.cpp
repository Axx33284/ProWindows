// ProWindows - the settings window.
//
// Laid out the way Battlefront II lays out an options screen: the screen's
// name as a breadcrumb at the top left ("PROWINDOWS / LAYOUT"), the settings
// in a single notched panel of full-width rows, a description of whatever row
// has focus beside it, and a row of buttons and key prompts along the foot.
// The categories run down the left, where the game's own options menu lists
// them.
//
// It is one window with no child controls - everything is drawn here or by
// rowlist.cpp - which is what lets every pixel be the game's, lets the keys
// work the way the game's do (arrows to move and change, Enter to select, Esc
// to go back), and makes the window cheap enough to build from nothing every
// time it is opened and throw away when it is closed.
//
// The window owns no settings. It edits a copy of the live Config; Apply
// copies back only what the user actually changed - see settings_internal.h.
#include "settings.h"
#include "settings_internal.h"
#include "modal.h"
#include "rowlist.h"
#include "theme.h"
#include "winutil.h"
#include "resource.h"
#include <commdlg.h>
#include <shlobj.h>
#include <windowsx.h>
#include <cmath>

#pragma comment(lib, "comdlg32.lib")

namespace awa {

using theme::Font;

// ================================================================ shared bits
const ModChoice kModChoices[6] = {
    { L"Alt",         MOD_ALT },
    { L"Ctrl",        MOD_CONTROL },
    { L"Win",         MOD_WIN },
    { L"Alt + Shift", MOD_ALT | MOD_SHIFT },
    { L"Ctrl + Alt",  MOD_CONTROL | MOD_ALT },
    { L"Win + Alt",   MOD_WIN | MOD_ALT },
};

const wchar_t* ModifierName(UINT mod) {
    return (mod & MOD_WIN) ? L"Win" : (mod & MOD_CONTROL) ? L"Ctrl"
         : (mod & MOD_ALT) ? L"Alt" : L"Shift";
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
    ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return false;

    std::wstring path = file;
    if (fullPath) { *result = path; return true; }
    const size_t slash = path.find_last_of(L"\\/");
    *result = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    return true;
}

bool BrowseForExeName(HWND parent, std::wstring* exeName) { return BrowseForExe(parent, exeName, false); }
bool BrowseForExePath(HWND parent, std::wstring* fullPath) { return BrowseForExe(parent, fullPath, true); }

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

bool PickColor(HWND parent, COLORREF* color) {
    // Kept across calls so a palette built for one thing is there for the next.
    static COLORREF custom[16] = {
        RGB(255, 176, 0), RGB(122, 162, 247), RGB(96, 210, 132), RGB(236, 76, 60),
        RGB(96, 205, 255), RGB(167, 139, 250), RGB(255, 255, 255), RGB(160, 166, 174),
        RGB(48, 52, 64), RGB(0, 229, 255), RGB(255, 0, 193), RGB(255, 214, 0),
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

// ================================================================ the edit copy
namespace {

Config     g_edit;          // what the rows change
Config     g_base;          // the live config as it was when the edit began
EditExtras g_extra, g_extraBase;

bool BindsEqual(const std::vector<Keybind>& a, const std::vector<Keybind>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const Keybind& x = a[i];
        const Keybind& y = b[i];
        if (x.mods != y.mods || x.vk != y.vk || x.action != y.action || x.arg != y.arg ||
            x.command != y.command)
            return false;
    }
    return true;
}

// Every field the settings window edits. Anything added to a page has to be
// added here too, or Apply will not carry it.
#define AWA_EDITED_FIELDS(X)                                                        \
    X(layout) X(masterRatio) X(masterCount) X(gapInner) X(gapOuter) X(smartGaps)    \
    X(workspaceCount) X(marginTop) X(marginBottom) X(marginLeft) X(marginRight)     \
    X(resizeStep)                                                                   \
    X(focusFollowsMouse) X(cursorWarp) X(newWindowOnTop) X(dragToRearrange)         \
    X(modDrag) X(animations) X(animationMs) X(pauseForFullscreen) X(accentBorder)   \
    X(activeColor) X(inactiveColor) X(cornerPref) X(ignoreProcess)                  \
    X(modMask) X(overrideReserved)                                                  \
    X(searchFiles) X(searchSettings) X(searchCalc) X(searchCommands)                \
    X(searchHidden) X(searchDepth) X(searchMaxEntries) X(searchFolders)             \
    X(searchPrograms) X(searchDrives) X(searchDeepExe)                              \
    X(monitorEnabled) X(monitorPinned) X(monitorOnDesktop) X(monitorGraphs)         \
    X(monitorTopApps) X(monitorVertical) X(monitorCores) X(monitorSmooth)           \
    X(monitorTheme) X(monitorStyle) X(monitorOpacity) X(monitorScale)               \
    X(monitorInterval) X(monShowCpu) X(monShowRam) X(monShowGpu) X(monShowVram)     \
    X(monShowCpuTemp) X(monShowGpuTemp) X(monShowDisk) X(monShowNet)                \
    X(clockEnabled) X(clockPinned) X(clockOnDesktop) X(clockTheme) X(clockStyle)    \
    X(clockHours24) X(clockSeconds) X(clockDate) X(clockWeekday) X(clockOpacity)    \
    X(clockScale)                                                                   \
    X(startMinimized) X(debug)

bool ArraysEqual(const Config& a, const Config& b) {
    for (int i = 0; i < MON_METRIC_COUNT; ++i)
        if (a.monOrder[i] != b.monOrder[i] || a.monColor[i] != b.monColor[i]) return false;
    return true;
}

// Loads the edit copy from the live config.
void LoadEdit() {
    g_edit = AppConfig();
    g_base = AppConfig();
    g_extra.autostart = AppAutostartEnabled();
    g_extra.elevated  = ElevatedAutostartInstalled();
    g_extraBase = g_extra;
}

bool Dirty() {
    return !EditedFieldsEqual(g_edit, g_base) ||
           g_extra.autostart != g_extraBase.autostart ||
           g_extra.elevated  != g_extraBase.elevated;
}

int ChangeCount() {
    int n = 0;
#define AWA_COUNT(f) if (!(g_edit.f == g_base.f)) ++n;
    AWA_EDITED_FIELDS(AWA_COUNT)
#undef AWA_COUNT
    if (!BindsEqual(g_edit.binds, g_base.binds)) ++n;
    for (int i = 0; i < MON_METRIC_COUNT; ++i) {
        if (g_edit.monOrder[i] != g_base.monOrder[i]) { ++n; break; }
    }
    for (int i = 0; i < MON_METRIC_COUNT; ++i)
        if (g_edit.monColor[i] != g_base.monColor[i]) ++n;
    if (g_extra.autostart != g_extraBase.autostart) ++n;
    if (g_extra.elevated  != g_extraBase.elevated) ++n;
    return n;
}

} // namespace

Config&           Edit()       { return g_edit; }
const Config&     Saved()      { return g_base; }
EditExtras&       EditExtra()  { return g_extra; }
const EditExtras& SavedExtra() { return g_extraBase; }

bool EditsPending() { return Dirty(); }

void DiscardEdits() {
    g_edit  = g_base;
    g_extra = g_extraBase;
}

bool EditedFieldsEqual(const Config& a, const Config& b) {
#define AWA_EQ(f) if (!(a.f == b.f)) return false;
    AWA_EDITED_FIELDS(AWA_EQ)
#undef AWA_EQ
    return BindsEqual(a.binds, b.binds) && ArraysEqual(a, b);
}

// Copies `from` into `to`, but only the fields that differ from `base` - what
// the user changed. Everything else in `to` is left as it is now, so a change
// made meanwhile from outside the window survives an Apply.
static void MergeEdits(const Config& from, const Config& base, Config& to) {
#define AWA_MERGE(f) if (!(from.f == base.f)) to.f = from.f;
    AWA_EDITED_FIELDS(AWA_MERGE)
#undef AWA_MERGE
    if (!BindsEqual(from.binds, base.binds)) to.binds = from.binds;
    bool order = false;
    for (int i = 0; i < MON_METRIC_COUNT; ++i) order |= from.monOrder[i] != base.monOrder[i];
    for (int i = 0; i < MON_METRIC_COUNT; ++i) {
        if (order) to.monOrder[i] = from.monOrder[i];
        if (from.monColor[i] != base.monColor[i]) to.monColor[i] = from.monColor[i];
    }
}

// The live config changed underneath an edit - the monitor re-skinned from
// its own menu, a reload from disk. Every field the user has not touched takes
// the new value; every field they have keeps theirs; and what counts as saved
// moves on to the live config. 1.4 reloaded every page instead, which threw
// away everything that had not been applied yet.
static void Rebase(const Config& live) {
#define AWA_REBASE(f) if (g_edit.f == g_base.f) g_edit.f = live.f; g_base.f = live.f;
    AWA_EDITED_FIELDS(AWA_REBASE)
#undef AWA_REBASE
    if (BindsEqual(g_edit.binds, g_base.binds)) g_edit.binds = live.binds;
    g_base.binds = live.binds;
    bool order = false, colours = false;
    for (int i = 0; i < MON_METRIC_COUNT; ++i) {
        order   |= g_edit.monOrder[i] != g_base.monOrder[i];
        colours |= g_edit.monColor[i] != g_base.monColor[i];
    }
    for (int i = 0; i < MON_METRIC_COUNT; ++i) {
        if (!order)   g_edit.monOrder[i] = live.monOrder[i];
        if (!colours) g_edit.monColor[i] = live.monColor[i];
        g_base.monOrder[i] = live.monOrder[i];
        g_base.monColor[i] = live.monColor[i];
    }
    // Everything the window does not edit simply follows the live config.
    const Config edited = g_edit;
    g_edit = live;
    MergeEdits(edited, live, g_edit);
    g_base = live;

    EditExtras now;
    now.autostart = AppAutostartEnabled();
    now.elevated  = ElevatedAutostartInstalled();
    if (g_extra.autostart == g_extraBase.autostart) g_extra.autostart = now.autostart;
    if (g_extra.elevated  == g_extraBase.elevated)  g_extra.elevated  = now.elevated;
    g_extraBase = now;
}

bool ConfirmChordFree(UINT mods, UINT vk, int selfIndex, std::vector<int>* clashes) {
    clashes->clear();
    for (int i = 0; i < (int)g_edit.binds.size(); ++i) {
        if (i == selfIndex) continue;
        const Keybind& kb = g_edit.binds[(size_t)i];
        if (kb.vk && kb.mods == mods && kb.vk == vk) clashes->push_back(i);
    }
    if (clashes->empty()) return true;
    const Keybind& other = g_edit.binds[(size_t)clashes->front()];
    const std::wstring who = other.action == ACT_LAUNCH ? L"opens " + FriendlyCommandName(other.command)
                                                        : DescribeAction(other);
    const std::wstring body = DescribeChord(mods, vk) + L" already " +
                              (other.action == ACT_LAUNCH ? who : L"does this: " + who) +
                              L".\r\n\r\nUse it here instead? The other shortcut will be left "
                              L"without a key.";
    return ui::Confirm(SettingsHwnd(), L"That key is taken", body, L"Use it here", L"Cancel");
}

// ================================================================ the window
namespace {

constexpr wchar_t kClass[] = L"ProWindowsSettings";
constexpr UINT_PTR kTimerStatus = 1;
constexpr UINT_PTR kTimerAnim   = 2;
constexpr UINT WM_AWA_CAPTUREKEY = WM_APP + 120;

const PageDef g_pages[] = {
    { L"Layout",
      L"How windows are arranged on the screen: the pattern new windows are placed "
      L"in, the space between them, how many workspaces there are, and room kept "
      L"clear for a bar.",
      BuildLayoutPage, ResetLayoutPage, PreviewLayout, 150, nullptr },
    { L"Behaviour",
      L"How ProWindows reacts while you work: focus, the mouse, animation, the "
      L"border round the focused window, games, and apps it should leave alone.",
      BuildBehaviourPage, ResetBehaviourPage, nullptr, 0, nullptr },
    { L"Shortcuts",
      L"Every keyboard shortcut that moves, focuses and arranges windows. Select "
      L"one and press Enter, then press the keys you want.",
      BuildShortcutsPage, ResetShortcutsPage, nullptr, 0, ShortcutIssues },
    { L"Apps",
      L"Shortcuts that open an app, a file or a folder. Add one, then press the "
      L"keys that should open it.",
      BuildAppsPage, ResetAppsPage, nullptr, 0, nullptr },
    { L"Search",
      L"The search bar (Win+S): what it finds besides your installed apps, and "
      L"which folders it indexes.",
      BuildSearchPage, ResetSearchPage, nullptr, 0, nullptr },
    { L"Monitor",
      L"The floating system monitor: what it shows, in what order, and how it looks.",
      BuildMonitorPage, ResetMonitorPage, PreviewMonitor, 190, nullptr },
    { L"Clock",
      L"The desktop clock: its style, its colours and what it shows.",
      BuildClockPage, ResetClockPage, PreviewClock, 150, nullptr },
    { L"General",
      L"How ProWindows starts, where its settings are kept, and what to reach for "
      L"when something on the desktop looks wrong.",
      BuildGeneralPage, ResetGeneralPage, nullptr, 0, nullptr },
};
static_assert(ARRAYSIZE(g_pages) == PAGE_COUNT, "PageIndex is out of step with g_pages");

enum class Zone { Nav, List, Footer };

// Things in the window that answer the mouse.
enum Hot {
    HOT_NONE = 0, HOT_MIN, HOT_CLOSE, HOT_RETILE, HOT_PAUSE, HOT_SEARCH,
    HOT_APPLY, HOT_RESET, HOT_BACK,
    HOT_NAV = 100,           // + category
    HOT_PROMPT = 200,        // + prompt
};

struct Geometry {
    RECT client = {};
    RECT badge = {}, chip = {}, crumb = {};
    RECT retile = {}, pause = {};
    RECT winMin = {}, winClose = {};
    RECT search = {};
    RECT nav[PAGE_COUNT] = {};
    RECT frame = {}, rows = {}, track = {};
    RECT desc = {};
    RECT apply = {}, reset = {}, back = {};
    int  headerBottom = 0;
    int  footerY = 0;
};

struct Status {
    std::wstring chip;
    std::wstring alert;
    COLORREF mark = theme::Good;
    bool tiling = true;
    int  windows = 0;
};

HWND        g_wnd = nullptr;
Geometry    g_geo;
ui::RowList g_list;
int         g_page = 0;
Zone        g_zone = Zone::Nav;
int         g_hot = HOT_NONE;
int         g_pressed = HOT_NONE;
int         g_footFocus = 0;
std::wstring g_query;
bool        g_dirty = false;
Status      g_status;
std::vector<std::pair<RECT, UINT>> g_promptHits;   // footer prompts, and the key each stands for

// Animation.
ULONGLONG g_pageShownAt = 0;       // the rows fade and slide in after a category change
bool      g_animating = false;
std::wstring g_toast;
ULONGLONG g_toastAt = 0;

// Shortcut capture.
HHOOK g_captureHook = nullptr;
int   g_captureRow = -1;
UINT  g_captureHeld = 0;

bool g_toldAboutTray = false;

// Where the pointer was when the window appeared. Windows sends a mouse move
// to a window that opens under a pointer that has not moved, and acting on it
// took the keyboard's place in the list before a key had been pressed; the
// pointer has to really move before hovering means anything.
POINT g_mouseAnchor = {};
bool  g_mouseIdle   = true;

int Sc(int px) { return theme::Scale(px); }

void Invalidate() { if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE); }

void Animate() {
    if (!g_wnd || g_animating) return;
    g_animating = true;
    SetTimer(g_wnd, kTimerAnim, 15, nullptr);
}

// ---------------------------------------------------------------- rows
void BuildSearchResults(std::vector<ui::Row>& out) {
    // Every word of the query has to appear somewhere in the row's name, its
    // description or the heading it sits under.
    std::vector<std::wstring> words;
    {
        std::wstring w;
        for (wchar_t c : ToLower(g_query) + L" ") {
            if (c == L' ') { if (!w.empty()) words.push_back(w); w.clear(); }
            else w += c;
        }
    }
    for (int p = 0; p < PAGE_COUNT; ++p) {
        std::vector<ui::Row> rows;
        g_pages[p].build(rows);
        std::wstring section;
        bool headed = false;
        for (auto& r : rows) {
            if (r.kind == ui::Kind::Section) { section = r.label; continue; }
            const std::wstring hay = ToLower(r.label + L" " + r.help + L" " + r.detail + L" " +
                                             section + L" " + g_pages[p].caption);
            bool all = true;
            for (const auto& w : words)
                if (hay.find(w) == std::wstring::npos) { all = false; break; }
            if (!all) continue;
            if (!headed) {
                out.push_back(ui::Section(g_pages[p].caption));
                headed = true;
            }
            r.id   = std::to_wstring(p) + L":" + r.id;
            r.page = g_pages[p].caption;
            r.orderGroup = 0;           // reordering belongs on its own page
            out.push_back(std::move(r));
        }
    }
    if (out.empty())
        out.push_back(ui::Info(L"nothing", L"No setting matches \"" + g_query + L"\"",
                               L"Try fewer or shorter words. Esc clears the search.", nullptr));
}

void BuildRows(bool keepFocus) {
    std::vector<ui::Row> rows;
    if (!g_query.empty()) {
        BuildSearchResults(rows);
    } else {
        g_pages[g_page].build(rows);
        for (auto& r : rows) r.page = g_pages[g_page].caption;
    }
    g_list.SetRows(std::move(rows), keepFocus);
}

int PageOfRow(const ui::Row* row) {
    if (!row) return g_page;
    for (int p = 0; p < PAGE_COUNT; ++p)
        if (row->page == g_pages[p].caption) return p;
    return g_page;
}

void UpdateDirty() {
    const bool was = g_dirty;
    g_dirty = Dirty();
    if (was != g_dirty) Invalidate();
}

// ---------------------------------------------------------------- status
void ReadStatus(Status* s) {
    WindowManager& wm = AppWm();
    const int chords  = AppHotkeyConflicts();
    const int refused = wm.BlockedCount();
    s->windows = wm.ManagedCount();
    s->tiling  = wm.TilingEnabled();
    const wchar_t* state = wm.GameMode()      ? L"Game mode"
                         : wm.TilingEnabled() ? L"Tiling"
                                              : L"Paused";
    s->chip = std::wstring(state) + L"  \x00B7  " + std::to_wstring(s->windows) +
              (s->windows == 1 ? L" window" : L" windows");
    s->mark = (refused > 0 || chords > 0)            ? theme::Danger
            : (wm.GameMode() || !wm.TilingEnabled()) ? theme::Warn
                                                     : theme::Good;
    s->alert.clear();
    if (refused > 0)
        s->alert = std::to_wstring(refused) + (refused == 1 ? L" window needs" : L" windows need") +
                   L" administrator rights";
    else if (chords > 0)
        s->alert = std::to_wstring(chords) + (chords == 1 ? L" shortcut is" : L" shortcuts are") +
                   L" blocked by another program";
}

// ---------------------------------------------------------------- geometry
void DoLayout() {
    Geometry& g = g_geo;
    GetClientRect(g_wnd, &g.client);
    const int W = g.client.right, H = g.client.bottom;
    const int M = Sc(36);

    g.winClose = { W - Sc(48), 0, W, Sc(34) };
    g.winMin   = { W - Sc(96), 0, W - Sc(48), Sc(34) };

    const int y0 = Sc(24);
    g.badge = { M, y0, M + Sc(34), y0 + Sc(34) };
    g.chip  = { g.badge.right + Sc(10), y0, g.badge.right + Sc(10), y0 + Sc(34) };   // width at paint
    g.crumb = { M, y0 + Sc(34) + Sc(16), W - M, y0 + Sc(34) + Sc(16) + Sc(40) };
    g.headerBottom = g.crumb.bottom + Sc(22);

    HDC dc = GetDC(g_wnd);
    const std::wstring pauseLabel = g_status.tiling ? L"Pause tiling" : L"Resume tiling";
    const int pw = (std::max)(Sc(150), theme::ButtonWidth(dc, L"Resume tiling", nullptr));
    const int rw = (std::max)(Sc(130), theme::ButtonWidth(dc, L"Re-arrange", nullptr));
    const int by = g.crumb.top + Sc(3);
    g.pause  = { W - M - pw, by, W - M, by + Sc(34) };
    g.retile = { g.pause.left - Sc(12) - rw, by, g.pause.left - Sc(12), by + Sc(34) };

    // Footer.
    g.footerY = H - Sc(34) - Sc(40);
    const int fb = Sc(40);
    int x = M;
    auto place = [&](RECT& r, const wchar_t* label) {
        const int w = (std::max)(Sc(120), theme::ButtonWidth(dc, label, nullptr));
        r = { x, g.footerY, x + w, g.footerY + fb };
        x += w + Sc(12);
    };
    place(g.apply, L"Apply");
    place(g.reset, L"Reset to defaults");
    place(g.back, L"Close");
    ReleaseDC(g_wnd, dc);

    // Body: the categories, the panel, the description.
    const int top = g.headerBottom;
    const int bottom = g.footerY - Sc(26);
    const int navW = Sc(200);
    const int descW = (std::max)(Sc(250), (std::min)(Sc(340), W * 26 / 100));
    g.desc  = { W - M - descW, top, W - M, bottom };
    g.frame = { M + navW + Sc(30), top, g.desc.left - Sc(40), bottom };
    const int notch = theme::PanelNotch();
    g.rows  = { g.frame.left, g.frame.top + notch + Sc(14), g.frame.right,
                g.frame.bottom - notch - Sc(14) };
    g.track = { g.frame.right + Sc(10), g.frame.top + Sc(4), g.frame.right + Sc(22),
                g.frame.bottom - Sc(4) };
    g_list.SetBounds(g.rows, g.track);

    g.search = { M, top, M + navW, top + Sc(38) };
    const int itemH = Sc(42);
    int y = g.search.bottom + Sc(18);
    for (int i = 0; i < PAGE_COUNT; ++i) {
        g.nav[i] = { M, y, M + navW, y + itemH };
        y += itemH;
    }
}

// ---------------------------------------------------------------- painting
void PaintHeader(HDC dc) {
    const Geometry& g = g_geo;

    // The badge where the game shows the player's: the app's own mark on its
    // cut plate, as the icon draws it.
    theme::CutBox(dc, g.badge, Sc(9), theme::Raised, 255, theme::Edge, 230,
                  (float)(std::max)(1, Sc(1)) * 1.5f);
    RECT mark = g.badge;
    InflateRect(&mark, -Sc(9), -Sc(9));
    theme::Mark(dc, mark, theme::Text, theme::Amber);

    // Beside it, between two bars, what is going on.
    const int cy = (g.chip.top + g.chip.bottom) / 2;
    const int bar = (std::max)(2, Sc(2));
    const int tw = theme::Measure(dc, Font::Caption, theme::Caps(g_status.chip), Sc(1));
    RECT chip = g.chip;
    chip.right = chip.left + Sc(12) + Sc(14) + tw + Sc(14);
    RECT l = { chip.left, chip.top + Sc(4), chip.left + bar, chip.bottom - Sc(4) };
    RECT r = { chip.right - bar, chip.top + Sc(4), chip.right, chip.bottom - Sc(4) };
    theme::Wash(dc, l, theme::Edge, 200);
    theme::Wash(dc, r, theme::Edge, 200);
    theme::Diamond(dc, (float)(chip.left + Sc(15)), (float)cy, theme::ScaleF(3.0f), g_status.mark);
    RECT ct = { chip.left + Sc(26), chip.top, chip.right - Sc(8), chip.bottom };
    theme::Print(dc, Font::Caption, theme::Caps(g_status.chip), ct, theme::Text,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE, Sc(1));
    if (!g_status.alert.empty()) {
        RECT at = { chip.right + Sc(16), chip.top, g.winMin.left - Sc(100), chip.bottom };
        theme::Print(dc, Font::Caption, theme::Caps(g_status.alert), at, theme::Danger,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, Sc(1));
    }

    // The breadcrumb, between its bars: "PROWINDOWS / LAYOUT".
    const std::wstring lead = L"PROWINDOWS / ";
    const std::wstring here = g_query.empty() ? theme::Caps(g_pages[g_page].caption) : L"SEARCH";
    const int lw = theme::Measure(dc, Font::Crumb, lead);
    const int hw = theme::Measure(dc, Font::CrumbBold, here);
    const RECT& c = g.crumb;
    RECT lb = { c.left, c.top - Sc(4), c.left + bar, c.bottom + Sc(10) };
    theme::Wash(dc, lb, theme::Edge, 220);
    RECT t1 = { c.left + Sc(18), c.top, c.left + Sc(18) + lw + Sc(4), c.bottom };
    theme::Print(dc, Font::Crumb, lead, t1, RGB(176, 180, 186), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT t2 = { t1.right - Sc(2), c.top, t1.right + hw + Sc(4), c.bottom };
    theme::Print(dc, Font::CrumbBold, here, t2, theme::Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT rb = { t2.right + Sc(16), c.top - Sc(6), t2.right + Sc(16) + bar, c.bottom + Sc(4) };
    theme::Wash(dc, rb, theme::Edge, 220);

    // Header buttons.
    theme::ButtonLook bl;
    bl.hot = (g_hot == HOT_RETILE);
    bl.pressed = (g_pressed == HOT_RETILE && bl.hot);
    theme::DrawButton(dc, g.retile, L"Re-arrange", nullptr, bl);
    bl.hot = (g_hot == HOT_PAUSE);
    bl.pressed = (g_pressed == HOT_PAUSE && bl.hot);
    theme::DrawButton(dc, g.pause, g_status.tiling ? L"Pause tiling" : L"Resume tiling", nullptr, bl);

    // The version, and the window's own two buttons.
    {
        const std::wstring v = std::wstring(L"V") + kVersion;
        const int w = theme::Measure(dc, Font::Caption, v, Sc(1));
        RECT ver = { g.winMin.left - Sc(12) - w, g.winMin.top, g.winMin.left, g.winMin.bottom };
        theme::Print(dc, Font::Caption, v, ver, theme::TextMute, DT_LEFT | DT_VCENTER | DT_SINGLELINE, Sc(1));
    }
    if (g_hot == HOT_MIN)   theme::Wash(dc, g.winMin, theme::Text, 30);
    if (g_hot == HOT_CLOSE) theme::Wash(dc, g.winClose, RGB(196, 43, 28), 255);
    {
        const int mx = (g.winMin.left + g.winMin.right) / 2, my = (g.winMin.top + g.winMin.bottom) / 2;
        RECT dash = { mx - Sc(6), my, mx + Sc(6), my + (std::max)(1, Sc(1)) };
        theme::Wash(dc, dash, theme::TextDim, 255);
        const float cx = (float)(g.winClose.left + g.winClose.right) / 2.0f;
        const float cyy = (float)(g.winClose.top + g.winClose.bottom) / 2.0f;
        const float s = theme::ScaleF(5.5f);
        HPEN pen = CreatePen(PS_SOLID, (std::max)(1, Sc(1)), g_hot == HOT_CLOSE ? RGB(255, 255, 255) : theme::TextDim);
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, (int)(cx - s), (int)(cyy - s), nullptr); LineTo(dc, (int)(cx + s) + 1, (int)(cyy + s) + 1);
        MoveToEx(dc, (int)(cx + s), (int)(cyy - s), nullptr); LineTo(dc, (int)(cx - s) - 1, (int)(cyy + s) + 1);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
}

void PaintNav(HDC dc) {
    const Geometry& g = g_geo;

    // The search field.
    {
        const RECT& s = g.search;
        const bool lit = !g_query.empty() || g_hot == HOT_SEARCH;
        theme::Wash(dc, s, RGB(0, 0, 0), 110);
        theme::Frame(dc, s, lit ? theme::Amber : theme::Line, 255, (std::max)(1, Sc(1)));
        const int cx = s.left + Sc(18), cy = (s.top + s.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, (std::max)(1, Sc(2)), lit ? theme::Amber : theme::TextDim);
        HGDIOBJ old = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        const int r = Sc(5);
        Ellipse(dc, cx - r, cy - r - Sc(1), cx + r, cy + r - Sc(1));
        MoveToEx(dc, cx + r - Sc(1), cy + r - Sc(2), nullptr);
        LineTo(dc, cx + r + Sc(4), cy + r + Sc(3));
        SelectObject(dc, oldBrush);
        SelectObject(dc, old);
        DeleteObject(pen);
        RECT tr = { s.left + Sc(34), s.top, s.right - Sc(10), s.bottom };
        if (g_query.empty()) {
            theme::Print(dc, Font::Caption, L"SEARCH SETTINGS", tr, theme::TextMute,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE, Sc(1));
        } else {
            theme::Print(dc, Font::Body, g_query, tr, theme::Text,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            const int w = (std::min)((int)(tr.right - tr.left), theme::Measure(dc, Font::Body, g_query));
            if ((GetTickCount64() / 530) % 2 == 0) {
                RECT caret = { tr.left + w + Sc(1), cy - Sc(8), tr.left + w + Sc(1) + (std::max)(1, Sc(2)), cy + Sc(8) };
                theme::Wash(dc, caret, theme::Amber, 255);
            }
        }
    }

    // The categories. The current one sits on a pale bar, as a chosen word
    // does; with the keyboard on this column it is lit amber instead.
    for (int i = 0; i < PAGE_COUNT; ++i) {
        const RECT& r = g.nav[i];
        const bool current = g_query.empty() && i == g_page;
        const bool hot = (g_hot == HOT_NAV + i);
        RECT line = { r.left, r.bottom - 1, r.right, r.bottom };
        theme::Wash(dc, line, theme::Line, 255);
        RECT text = { r.left + Sc(16), r.top, r.right - Sc(24), r.bottom };
        const std::wstring caps = theme::Caps(g_pages[i].caption);
        if (current) {
            RECT bar = { r.left, r.top + Sc(3), r.right, r.bottom - Sc(3) };
            const bool lit = (g_zone == Zone::Nav);
            if (lit) theme::Glow(dc, bar, theme::AmberGlow, Sc(12), 100);
            theme::Gradient(dc, bar, lit ? theme::AmberHot : theme::Fill, lit ? theme::Amber : theme::FillLow);
            theme::Print(dc, Font::Nav, caps, text, lit ? theme::AmberText : theme::FillText,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE, Sc(1));
        } else {
            if (hot) {
                RECT box = { r.left, r.top + Sc(3), r.right, r.bottom - Sc(3) };
                theme::Wash(dc, box, theme::Text, 14);
            }
            theme::Print(dc, Font::Nav, caps, text, hot ? theme::Text : theme::TextDim,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE, Sc(1));
        }
        if (g_pages[i].issues && g_pages[i].issues() > 0)
            theme::Diamond(dc, (float)(r.right - Sc(14)), (float)(r.top + r.bottom) / 2.0f,
                           theme::ScaleF(3.0f), theme::Danger);
    }
}

void PaintDescription(HDC dc) {
    const Geometry& g = g_geo;
    RECT d = g.desc;
    const ui::Row* row = (g_zone == Zone::Nav && g_query.empty()) ? nullptr : g_list.FocusedRow();
    const int page = PageOfRow(row);

    // The preview for the category, if it has one: a miniature of the thing
    // being set, drawn by the same code that draws the real one.
    if (g_pages[page].preview) {
        RECT box = { d.left, d.top, d.right, d.top + Sc(g_pages[page].previewHeight) };
        theme::Wash(dc, box, RGB(0, 0, 0), 150);
        theme::Frame(dc, box, theme::Line, 255, 1);
        RECT inner = box;
        InflateRect(&inner, -Sc(8), -Sc(8));
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, box.left + 1, box.top + 1, box.right - 1, box.bottom - 1);
        g_pages[page].preview(dc, inner);
        RestoreDC(dc, saved);
        d.top = box.bottom + Sc(26);
    }

    std::wstring title, body;
    if (!row) {
        title = theme::Caps(g_pages[g_page].caption);
        body  = g_pages[g_page].blurb;
    } else {
        title = row->raw ? row->label : theme::Caps(row->label);
        body  = row->help;
        if (g_captureRow >= 0)
            body = L"Hold the modifiers you want - Win, Ctrl, Alt, Shift - and press the "
                   L"key. The shortcut is taken the moment the key goes down.\r\n\r\n"
                   L"Esc on its own cancels. A key with no modifier only works for F1 to "
                   L"F24 and the media keys, which nobody types with.";
    }

    if (!g_query.empty() && row) {
        RECT in = { d.left, d.top, d.right, d.top + Sc(18) };
        theme::Print(dc, Font::Caption, L"IN " + theme::Caps(g_pages[page].caption), in,
                     theme::TextMute, DT_LEFT | DT_VCENTER | DT_SINGLELINE, Sc(2));
        d.top += Sc(22);
    }

    RECT tr = { d.left, d.top, d.right, d.top + Sc(64) };
    const int th = theme::PrintWrapped(dc, row && row->raw ? Font::BodyBold : Font::Title, title,
                                       tr, 0, true);
    tr.bottom = tr.top + (std::min)(th, Sc(64));
    theme::PrintWrapped(dc, row && row->raw ? Font::BodyBold : Font::Title, title, tr, theme::Text);
    RECT rule = { d.left, tr.bottom + Sc(10), d.left + Sc(44), tr.bottom + Sc(10) + (std::max)(2, Sc(2)) };
    theme::Wash(dc, rule, theme::Amber, 255);

    int y = rule.bottom + Sc(16);
    if (row && row->modified && row->modified()) {
        RECT m = { d.left, y, d.right, y + Sc(18) };
        theme::Diamond(dc, (float)d.left + theme::ScaleF(3.0f), (float)(y + Sc(9)), theme::ScaleF(2.6f), theme::Amber);
        m.left += Sc(14);
        theme::Print(dc, Font::Caption, L"CHANGED  \x00B7  NOT APPLIED YET", m, theme::Amber,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE, Sc(1));
        y += Sc(28);
    }
    if (row && !row->Enabled()) {
        RECT m = { d.left, y, d.right, y + Sc(18) };
        theme::Print(dc, Font::Caption, L"NOT AVAILABLE RIGHT NOW", m, theme::TextMute,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE, Sc(1));
        y += Sc(28);
    }
    RECT br = { d.left, y, d.right, d.bottom };
    theme::PrintWrapped(dc, Font::Body, body, br, RGB(190, 195, 202));
}

void PaintFooter(HDC dc) {
    const Geometry& g = g_geo;
    RECT rule = { Sc(36), g.footerY - Sc(18), g.client.right - Sc(36), g.footerY - Sc(17) };
    theme::Wash(dc, rule, theme::Line, 255);

    auto button = [&](const RECT& r, int hot, int foot, const wchar_t* label, bool enabled, bool primary) {
        theme::ButtonLook look;
        look.enabled = enabled;
        look.primary = primary;
        look.hot     = (g_hot == hot);
        look.pressed = (g_pressed == hot && look.hot);
        look.focused = (g_zone == Zone::Footer && g_footFocus == foot);
        theme::DrawButton(dc, r, label, nullptr, look);
    };
    button(g.apply, HOT_APPLY, 0, L"Apply", g_dirty, g_dirty);
    button(g.reset, HOT_RESET, 1, L"Reset to defaults", g_query.empty(), false);
    button(g.back,  HOT_BACK,  2, L"Close", true, false);

    // Prompts for whatever has focus, from the right.
    std::vector<std::pair<std::wstring, std::wstring>> prompts;
    std::vector<UINT> keys;
    if (g_captureRow >= 0) {
        prompts.push_back({ L"Esc", L"Cancel" });
        keys.push_back(VK_ESCAPE);
    } else {
        if (g_zone == Zone::List) {
            for (const auto& p : g_list.Prompts()) {
                prompts.push_back(p);
                UINT vk = 0;
                if (p.first == L"Enter") vk = VK_RETURN;
                else if (p.first == L"Del") vk = VK_DELETE;
                else if (p.first == L"\x2190 \x2192") vk = VK_RIGHT;   // a click steps forward
                else if (p.first.size() == 2 && p.first[0] == L'F') vk = VK_F1 + (p.first[1] - L'1');
                keys.push_back(vk);
            }
        } else if (g_zone == Zone::Nav) {
            prompts.push_back({ L"\x2191 \x2193", L"Category" });
            keys.push_back(0);
            prompts.push_back({ L"Enter", L"Select" });
            keys.push_back(VK_RETURN);
        }
        if (g_dirty) { prompts.push_back({ L"Ctrl+S", L"Apply" }); keys.push_back('S' | 0x10000); }
        prompts.push_back({ L"Esc", g_zone == Zone::Nav ? L"Close" : L"Back" });
        keys.push_back(VK_ESCAPE);
    }
    g_promptHits.clear();
    const int cy = (g.apply.top + g.apply.bottom) / 2;
    int x = g.client.right - Sc(36);
    int promptsLeft = x;
    const int floor = g.back.right + Sc(24);
    for (int i = (int)prompts.size() - 1; i >= 0; --i) {
        const int w = theme::Prompt(dc, 0, cy, prompts[(size_t)i].first, prompts[(size_t)i].second,
                                    theme::TextDim, true);
        if (x - w < floor) break;
        x -= w;
        const bool hot = (g_hot == HOT_PROMPT + (int)g_promptHits.size());
        theme::Prompt(dc, x, cy, prompts[(size_t)i].first, prompts[(size_t)i].second,
                      hot ? theme::Amber : theme::TextDim);
        g_promptHits.push_back({ RECT{ x, cy - Sc(14), x + w, cy + Sc(14) }, keys[(size_t)i] });
        promptsLeft = x;
        x -= Sc(22);
    }

    // A message, or the reminder that there is something to apply.
    const ULONGLONG age = GetTickCount64() - g_toastAt;
    const RECT msg = { g.back.right + Sc(28), g.apply.top, promptsLeft - Sc(24), g.apply.bottom };
    if (!g_toast.empty() && age < 2600) {
        const float fade = age < 2100 ? 1.0f : 1.0f - (float)(age - 2100) / 500.0f;
        theme::Diamond(dc, (float)msg.left + theme::ScaleF(4.0f), (float)(msg.top + msg.bottom) / 2.0f,
                       theme::ScaleF(2.6f), theme::Mix(theme::Bg, theme::Good, fade));
        RECT t = { msg.left + Sc(16), msg.top, msg.right, msg.bottom };
        theme::Print(dc, Font::Caption, theme::Caps(g_toast), t, theme::Mix(theme::Bg, theme::Text, fade),
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, Sc(1));
    } else if (g_dirty) {
        const int n = ChangeCount();
        theme::Diamond(dc, (float)msg.left + theme::ScaleF(4.0f), (float)(msg.top + msg.bottom) / 2.0f,
                       theme::ScaleF(2.6f), theme::Amber);
        RECT t = { msg.left + Sc(16), msg.top, msg.right, msg.bottom };
        std::wstring text = std::to_wstring(n) + (n == 1 ? L" CHANGE" : L" CHANGES") +
                            L" NOT APPLIED YET";
        theme::Print(dc, Font::Caption, text, t, theme::Amber,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, Sc(1));
    }
}

void PaintAll(HDC dc) {
    const Geometry& g = g_geo;
    theme::SetDpi(DpiForWindow(g_wnd));

    // The backdrop is rendered at the size of the monitor rather than of the
    // window, so resizing shows more or less of one picture instead of
    // rendering a new one for every frame of the drag.
    SIZE canvas = { g.client.right, g.client.bottom };
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        canvas.cx = (std::max)(canvas.cx, (LONG)(mi.rcMonitor.right - mi.rcMonitor.left));
        canvas.cy = (std::max)(canvas.cy, (LONG)(mi.rcMonitor.bottom - mi.rcMonitor.top));
    }
    theme::PaintBackdrop(dc, g.client, canvas);

    PaintHeader(dc);
    PaintNav(dc);

    // The panel.
    theme::Wash(dc, g.frame, theme::Panel, 140);
    theme::PanelFrame(dc, g.frame, theme::Edge, 235, theme::ScaleF(2.0f), theme::PanelNotch());

    // After a category change the rows fade in and settle from a little
    // below, as the game's do.
    const ULONGLONG age = GetTickCount64() - g_pageShownAt;
    const float t = age >= 200 ? 1.0f : (float)age / 200.0f;
    const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    if (e >= 1.0f) {
        g_list.Paint(dc);
    } else {
        // Draw the rows onto a copy of what is behind them and blend the copy
        // in, so they fade without anything under them fading too.
        const RECT& f = g.frame;
        RECT area = { f.left - Sc(30), f.top, g.track.right + Sc(4), f.bottom };
        const int w = area.right - area.left, h = area.bottom - area.top;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = mem ? CreateCompatibleBitmap(dc, w, h) : nullptr;
        if (mem && bmp) {
            HGDIOBJ old = SelectObject(mem, bmp);
            BitBlt(mem, 0, 0, w, h, dc, area.left, area.top, SRCCOPY);
            SetViewportOrgEx(mem, -area.left, -area.top, nullptr);
            g_list.Paint(mem, (int)((1.0f - e) * (float)Sc(18)));
            SetViewportOrgEx(mem, 0, 0, nullptr);
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(255.0f * e), 0 };
            AlphaBlend(dc, area.left, area.top, w, h, mem, 0, 0, w, h, bf);
            SelectObject(mem, old);
        }
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
    }

    PaintDescription(dc);
    PaintFooter(dc);

    // Behind a modal screen the window steps back.
    if (ui::ModalOpen()) theme::Wash(dc, g.client, RGB(0, 0, 0), 150);
}

void Paint() {
    PAINTSTRUCT ps;
    HDC target = BeginPaint(g_wnd, &ps);
    const RECT& c = g_geo.client;
    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(target, c.right, c.bottom) : nullptr;
    if (mem && bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintAll(mem);
        BitBlt(target, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
    } else {
        PaintAll(target);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    EndPaint(g_wnd, &ps);
}

// ---------------------------------------------------------------- actions
void ShowPage(int index, bool focusList) {
    if (index < 0 || index >= PAGE_COUNT) return;
    const bool changed = (index != g_page) || !g_query.empty();
    g_page = index;
    g_query.clear();
    if (changed) {
        BuildRows(false);
        g_pageShownAt = GetTickCount64();
        Animate();
    }
    if (focusList) {
        g_zone = Zone::List;
        g_list.SetActive(true);
        g_list.FocusFirst();
    } else {
        g_list.SetActive(g_zone == Zone::List);
    }
    Invalidate();
}

void SetZone(Zone z) {
    g_zone = z;
    g_list.SetActive(z == Zone::List);
    Animate();
    Invalidate();
}

void Toast(const std::wstring& text) {
    g_toast = text;
    g_toastAt = GetTickCount64();
    Animate();
    Invalidate();
}

// Everything changed goes to the live config, is saved and reloaded, and the
// window starts again from what came back.
void ApplyNow() {
    if (!Dirty()) return;
    Config& live = AppConfig();
    const bool layoutChanged = g_edit.layout != g_base.layout ||
                               g_edit.masterRatio != g_base.masterRatio ||
                               g_edit.masterCount != g_base.masterCount;

    // The elevated logon task is registered with Windows, not stored in the
    // config, so it is applied here. Only when it actually changed: creating
    // it costs a schtasks call.
    if (g_extra.elevated != g_extraBase.elevated && SelfIsElevated()) {
        if (g_extra.elevated) {
            if (InstallElevatedAutostart()) {
                // The task starts it at logon now; the Run entry would only
                // add a second, unelevated copy.
                g_extra.autostart = false;
            } else {
                g_extra.elevated = false;
                ui::Notice(g_wnd, L"Could not set that up",
                           L"Windows would not register the logon task that starts ProWindows "
                           L"as administrator.");
            }
        } else if (!RemoveElevatedAutostart()) {
            g_extra.elevated = true;
        }
    }
    if (g_extra.autostart != AppAutostartEnabled()) AppSetAutostart(g_extra.autostart);

    // A shortcut still waiting for its key cannot be registered or written out.
    auto& binds = g_edit.binds;
    binds.erase(std::remove_if(binds.begin(), binds.end(),
                               [](const Keybind& kb) { return kb.vk == 0; }), binds.end());

    MergeEdits(g_edit, g_base, live);
    const Config snapshot = live;
    AppApplySettings();                 // save to disk, reload, re-register keys
    if (layoutChanged)
        AppWm().SetLayoutEverywhere(snapshot.layout, snapshot.masterRatio, snapshot.masterCount);
    AppUpdateTray();

    LoadEdit();
    BuildRows(true);
    UpdateDirty();
    Toast(L"Settings applied and saved");
}

void ResetPage() {
    if (!g_query.empty()) return;
    // Held across the question: the tray or an overlay can switch category
    // while it is up, and the one that was named is the one to reset.
    const int page = g_page;
    const std::wstring name = g_pages[page].caption;
    if (!ui::Confirm(g_wnd, L"Reset " + name + L"?",
                     L"Every setting on the " + name + L" page goes back to how it shipped. "
                     L"Nothing is saved until you apply.",
                     L"Reset", L"Cancel"))
        return;
    g_pages[page].reset();
    BuildRows(true);
    UpdateDirty();
    Toast(name + L" reset to defaults - apply to keep it");
}

// Closing with changes that have not been applied asks what to do with them.
// False when the user chose to stay.
bool SettleBeforeClose() {
    if (!Dirty()) return true;
    const int answer = ui::Ask(g_wnd, L"Apply your changes?",
        L"Some settings were changed but not applied. Apply them now, or close "
        L"and throw them away? Esc goes back to the settings.",
        L"Apply", L"Discard");
    if (answer < 0) return false;
    if (answer == 1) ApplyNow();
    return true;
}

void Close() {
    if (!SettleBeforeClose()) return;
    SettingsHide();
}

void PressButton(int hot) {
    switch (hot) {
        case HOT_MIN:    ShowWindow(g_wnd, SW_MINIMIZE); break;
        case HOT_CLOSE:  Close(); break;
        case HOT_RETILE: AppRetileNow(); SettingsRefreshStatus(); Toast(L"Windows re-arranged"); break;
        case HOT_PAUSE:
            AppWm().ActToggleTiling();
            AppUpdateTray();
            SettingsRefreshStatus();
            DoLayout();
            break;
        case HOT_APPLY:  ApplyNow(); break;
        case HOT_RESET:  ResetPage(); break;
        case HOT_BACK:   Close(); break;
        default: break;
    }
    Invalidate();
}

// ---------------------------------------------------------------- shortcut capture
// Recording a shortcut used to read WM_KEYDOWN, which works for Alt+H and
// never for Win+E: the shell has opened Explorer and eaten the keystroke
// before any window sees it, and anything ProWindows itself had registered
// fired its action instead of being recorded. So while a row is waiting for a
// chord, a low-level keyboard hook sits in front of both, swallows every key
// while this window is in the foreground, and posts each one here.
bool CaptureWanted() { return g_captureRow >= 0 && g_wnd && GetForegroundWindow() == g_wnd; }

UINT ModifierBitFor(UINT vk) {
    switch (vk) {
        case VK_LWIN: case VK_RWIN:                          return MOD_WIN;
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return MOD_CONTROL;
        case VK_MENU: case VK_LMENU: case VK_RMENU:          return MOD_ALT;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:       return MOD_SHIFT;
        default:                                             return 0;
    }
}

LRESULT CALLBACK CaptureHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION || !CaptureWanted()) return CallNextHookEx(nullptr, code, wp, lp);
    const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
    const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
    const bool up   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);
    if (!down && !up) return CallNextHookEx(nullptr, code, wp, lp);

    const UINT vk  = (UINT)kb->vkCode;
    const UINT bit = ModifierBitFor(vk);
    if (bit) {
        if (down) g_captureHeld |= bit;
        else      g_captureHeld &= ~bit;
        PostMessageW(g_wnd, WM_AWA_CAPTUREKEY, 0, g_captureHeld);
        // The Win key is the one that must not get through: on its own it
        // opens Start. The others are harmless to let pass.
        if (bit == MOD_WIN) return 1;
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    if (down) PostMessageW(g_wnd, WM_AWA_CAPTUREKEY, vk, g_captureHeld);
    return 1;
}

void EndCapture() {
    if (g_captureHook) { UnhookWindowsHookEx(g_captureHook); g_captureHook = nullptr; }
    g_captureRow = -1;
    g_captureHeld = 0;
    g_list.SetCapture(-1, 0, L"");
    Invalidate();
}

void BeginCapture(int row) {
    if (row < 0 || row >= (int)g_list.Rows().size()) return;
    EndCapture();
    g_captureRow = row;
    g_captureHeld = 0;
    g_captureHook = SetWindowsHookExW(WH_KEYBOARD_LL, CaptureHookProc, GetModuleHandleW(nullptr), 0);
    if (!g_captureHook)
        AWA_LOG(L"shortcut recorder: keyboard hook could not be installed; Win chords will not be captured");
    g_list.SetCapture(row, 0, L"");
    SetZone(Zone::List);
    Animate();
}

// A bare key with no modifier fires while you type, so only the keys nobody
// types with are allowed on their own.
bool BareKeyAllowed(UINT vk) {
    return (vk >= VK_F1 && vk <= VK_F24) || vk == VK_PAUSE || vk == VK_SCROLL ||
           (vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2);
}

void CancelCapture() {
    const int row = g_captureRow;
    EndCapture();
    // A row that was added a moment ago and never got a key goes again.
    if (row >= 0 && row < (int)g_list.Rows().size()) {
        const ui::Row& r = g_list.Rows()[(size_t)row];
        if (r.get && LOWORD(r.get()) == 0 && r.remove) {
            auto fn = r.remove;
            fn();
            UpdateDirty();
        }
    }
}

void CaptureKey(UINT vk, UINT mods) {
    if (g_captureRow < 0) return;
    if (vk == 0) {
        g_list.SetCapture(g_captureRow, mods, L"");
        return;
    }
    if (vk == VK_ESCAPE && mods == 0) { CancelCapture(); return; }
    if (mods == 0 && !BareKeyAllowed(vk)) {
        g_list.SetCapture(g_captureRow, 0, L"Add Alt, Ctrl, Win or Shift");
        return;
    }
    const int row = g_captureRow;
    EndCapture();
    if (row < (int)g_list.Rows().size()) {
        auto set = g_list.Rows()[(size_t)row].set;
        if (set) set((int)MAKELONG(vk, mods));
    }
    UpdateDirty();
    Invalidate();
}

// ---------------------------------------------------------------- input
int HitTest(POINT pt) {
    const Geometry& g = g_geo;
    if (PtInRect(&g.winClose, pt)) return HOT_CLOSE;
    if (PtInRect(&g.winMin, pt))   return HOT_MIN;
    if (PtInRect(&g.retile, pt))   return HOT_RETILE;
    if (PtInRect(&g.pause, pt))    return HOT_PAUSE;
    if (PtInRect(&g.search, pt))   return HOT_SEARCH;
    for (int i = 0; i < PAGE_COUNT; ++i)
        if (PtInRect(&g.nav[i], pt)) return HOT_NAV + i;
    if (PtInRect(&g.apply, pt)) return HOT_APPLY;
    if (PtInRect(&g.reset, pt)) return HOT_RESET;
    if (PtInRect(&g.back, pt))  return HOT_BACK;
    for (size_t i = 0; i < g_promptHits.size(); ++i)
        if (PtInRect(&g_promptHits[i].first, pt)) return HOT_PROMPT + (int)i;
    return HOT_NONE;
}

void KeyDown(UINT vk);

void RunPrompt(int index) {
    if (index < 0 || index >= (int)g_promptHits.size()) return;
    const UINT key = g_promptHits[(size_t)index].second;
    if (key == ('S' | 0x10000)) { ApplyNow(); return; }
    if (key) KeyDown(key);
}

void SearchChanged() {
    BuildRows(false);
    SetZone(Zone::List);
    g_list.FocusFirst();
    g_pageShownAt = GetTickCount64() - 120;
    Animate();
    Invalidate();
}

void KeyDown(UINT vk) {
    const bool ctrl  = GetKeyState(VK_CONTROL) < 0;
    const bool shift = GetKeyState(VK_SHIFT) < 0;

    if (g_captureRow >= 0) {
        // The hook sees everything first; this is only reached when it could
        // not be installed, and then a plain recorder is better than none.
        if (g_captureHook) return;
        UINT mods = 0;
        if (ctrl) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
        if (shift) mods |= MOD_SHIFT;
        if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
        if (ModifierBitFor(vk)) { CaptureKey(0, mods); return; }
        CaptureKey(vk, mods);
        return;
    }

    if (ctrl && (vk == VK_TAB || vk == VK_NEXT || vk == VK_PRIOR)) {
        const int delta = (vk == VK_PRIOR || (vk == VK_TAB && shift)) ? -1 : +1;
        ShowPage(((g_page + delta) % PAGE_COUNT + PAGE_COUNT) % PAGE_COUNT, false);
        return;
    }
    if (ctrl && vk == 'S') { ApplyNow(); return; }
    if (ctrl && vk == 'F') { SetZone(Zone::Nav); return; }

    if (vk == VK_ESCAPE) {
        if (!g_query.empty()) { g_query.clear(); BuildRows(false); SetZone(Zone::Nav); return; }
        if (g_zone == Zone::List || g_zone == Zone::Footer) { SetZone(Zone::Nav); return; }
        Close();
        return;
    }
    if (vk == VK_TAB) {
        const Zone next = shift ? (g_zone == Zone::Nav ? Zone::Footer : g_zone == Zone::List ? Zone::Nav : Zone::List)
                                : (g_zone == Zone::Nav ? Zone::List : g_zone == Zone::List ? Zone::Footer : Zone::Nav);
        SetZone(next);
        return;
    }
    if (vk == VK_BACK && !g_query.empty()) {
        g_query.pop_back();
        if (g_query.empty()) { BuildRows(false); SetZone(Zone::Nav); }
        else SearchChanged();
        return;
    }

    switch (g_zone) {
        case Zone::Nav:
            if (vk == VK_UP || vk == VK_DOWN) {
                const int d = (vk == VK_UP) ? -1 : +1;
                ShowPage(((g_page + d) % PAGE_COUNT + PAGE_COUNT) % PAGE_COUNT, false);
            } else if (vk == VK_HOME) {
                ShowPage(0, false);
            } else if (vk == VK_END) {
                ShowPage(PAGE_COUNT - 1, false);
            } else if (vk == VK_RIGHT || vk == VK_RETURN || vk == VK_SPACE) {
                if (!g_query.empty()) { SetZone(Zone::List); break; }
                SetZone(Zone::List);
                if (!g_list.FocusedRow()) g_list.FocusFirst();
            }
            break;
        case Zone::List:
            if (g_list.Key(vk, ctrl, shift)) {
                UpdateDirty();
                Animate();
            } else if (vk == VK_LEFT) {
                SetZone(Zone::Nav);
            }
            break;
        case Zone::Footer:
            if (vk == VK_LEFT)  { g_footFocus = (std::max)(0, g_footFocus - 1); Invalidate(); }
            if (vk == VK_RIGHT) { g_footFocus = (std::min)(2, g_footFocus + 1); Invalidate(); }
            if (vk == VK_UP)    SetZone(Zone::List);
            if (vk == VK_RETURN || vk == VK_SPACE)
                PressButton(g_footFocus == 0 ? HOT_APPLY : g_footFocus == 1 ? HOT_RESET : HOT_BACK);
            break;
    }
}

void Char(wchar_t ch) {
    if (g_captureRow >= 0) return;
    if (ch < 32 || ch == 127) return;
    if (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) return;
    // Typing anywhere starts a search; a space only continues one, since on
    // its own it presses the row that has focus.
    if (ch == L' ' && g_query.empty()) return;
    if (g_query.size() >= 60) return;
    g_query += ch;
    SearchChanged();
}

void MouseMove(POINT pt, bool down) {
    if (g_mouseIdle) {
        POINT screen = pt;
        ClientToScreen(g_wnd, &screen);
        if (abs(screen.x - g_mouseAnchor.x) < 3 && abs(screen.y - g_mouseAnchor.y) < 3 && !down)
            return;
        g_mouseIdle = false;
    }
    const int hot = HitTest(pt);
    if (hot != g_hot) { g_hot = hot; Invalidate(); }
    if (g_captureRow < 0) {
        if (g_list.Contains(pt) || g_list.Dragging()) {
            if (g_zone != Zone::List && !down) SetZone(Zone::List);
            g_list.MouseMove(pt, down);
            UpdateDirty();
        } else {
            g_list.MouseLeave();
        }
    }
    TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, g_wnd, 0 };
    TrackMouseEvent(&t);
    Animate();
}

void MouseDown(POINT pt) {
    if (g_captureRow >= 0) {
        // A click gives up waiting. If that took a keyless launcher's row away,
        // or the click was on the Esc prompt, it has done its job: the rows
        // under the pointer have moved, and Esc would press again.
        const size_t before = g_list.Rows().size();
        CancelCapture();
        if (g_list.Rows().size() != before || HitTest(pt) >= HOT_PROMPT) { Invalidate(); return; }
    }
    SetCapture(g_wnd);
    const int hot = HitTest(pt);
    if (hot >= HOT_NAV && hot < HOT_NAV + PAGE_COUNT) {
        SetZone(Zone::Nav);
        ShowPage(hot - HOT_NAV, false);
        return;
    }
    if (hot == HOT_SEARCH) { SetZone(Zone::Nav); return; }
    if (hot != HOT_NONE) { g_pressed = hot; Invalidate(); return; }
    if (g_list.MouseDown(pt)) {
        SetZone(Zone::List);
        UpdateDirty();
        Animate();
    }
}

void MouseUp(POINT pt) {
    // Read before letting go of the capture: ReleaseCapture sends
    // WM_CAPTURECHANGED on the spot, and that clears g_pressed - which made
    // every button in the window dead to the mouse.
    const int pressed = g_pressed;
    g_pressed = HOT_NONE;
    if (GetCapture() == g_wnd) ReleaseCapture();
    if (pressed != HOT_NONE) {
        if (HitTest(pt) == pressed) {
            if (pressed >= HOT_PROMPT) RunPrompt(pressed - HOT_PROMPT);
            else PressButton(pressed);
        }
        Invalidate();
        return;
    }
    g_list.MouseUp(pt);
    UpdateDirty();
    Animate();
}

// ---------------------------------------------------------------- the frame
// The window draws its own title strip, so the system's is removed and the
// resize borders and the drag area are answered by hand.
LRESULT HitTestFrame(POINT screen) {
    POINT pt = screen;
    ScreenToClient(g_wnd, &pt);
    RECT c;
    GetClientRect(g_wnd, &c);
    const int b = Sc(6);
    if (!IsZoomed(g_wnd)) {
        const bool l = pt.x < b, r = pt.x >= c.right - b, t = pt.y < b, d = pt.y >= c.bottom - b;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (d && l) return HTBOTTOMLEFT;
        if (d && r) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (t) return HTTOP;
        if (d) return HTBOTTOM;
    }
    if (pt.y < g_geo.headerBottom && HitTest(pt) == HOT_NONE) return HTCAPTION;
    return HTCLIENT;
}

void PlaceInitially() {
    // At the DPI of the monitor the pointer is on, centred there, and never
    // bigger than its work area - on a 1366x768 laptop at 125% there is not
    // room for the size it would like.
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    const UINT dpi = DpiForWindow(g_wnd);
    const RECT& wa = mi.rcWork;
    const int waW = wa.right - wa.left, waH = wa.bottom - wa.top;
    const int w = (std::min)(MulDiv(1220, (int)dpi, 96), waW * 96 / 100);
    const int h = (std::min)(MulDiv(820, (int)dpi, 96), waH * 94 / 100);
    SetWindowPos(g_wnd, nullptr, wa.left + (waW - w) / 2, wa.top + (waH - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK SettingsProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    // The theme's scale is shared with the search bar and the modal screens,
    // and every length the rows and buttons are hit-tested with goes through
    // it. Point it at this window's monitor before handling anything, not
    // only before painting. Cheap when it is already right.
    if (msg != WM_NCCREATE && msg != WM_DPICHANGED) theme::SetDpi(DpiForWindow(wnd));
    switch (msg) {
        case WM_CREATE: {
            g_wnd = wnd;
            theme::SetDpi(DpiForWindow(wnd));
            // No rounded corners: the game's screens are square. A hairline of
            // DWM frame keeps the shadow.
            const DWORD square = 1;     // DWMWCP_DONOTROUND
            DwmSetWindowAttribute(wnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &square, sizeof(square));
            theme::DarkTitleBar(wnd);
            const MARGINS m = { 0, 0, 0, 1 };
            DwmExtendFrameIntoClientArea(wnd, &m);
            return 0;
        }

        case WM_NCCALCSIZE:
            if (wp) {
                // The whole window is client area. A maximised window's frame
                // hangs off the screen, so it is taken back in.
                if (IsZoomed(wnd)) {
                    auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                    const int f = GetSystemMetricsForDpi(SM_CXFRAME, DpiForWindow(wnd)) +
                                  GetSystemMetricsForDpi(SM_CXPADDEDBORDER, DpiForWindow(wnd));
                    InflateRect(&p->rgrc[0], -f, -f);
                }
                return 0;
            }
            break;

        case WM_NCHITTEST:
            return HitTestFrame({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });

        case WM_NCACTIVATE:
            // Nothing of the system's frame is drawn; stop it flashing one.
            return TRUE;

        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            const UINT dpi = DpiForWindow(wnd);
            mm->ptMinTrackSize.x = MulDiv(980, (int)dpi, 96);
            mm->ptMinTrackSize.y = MulDiv(620, (int)dpi, 96);
            return 0;
        }

        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            theme::SetDpi(HIWORD(wp));
            SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            DoLayout();
            Invalidate();
            return 0;
        }

        case WM_SIZE:
            theme::SetDpi(DpiForWindow(wnd));
            DoLayout();
            Invalidate();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            Paint();
            return 0;

        case WM_TIMER:
            if (wp == kTimerStatus) {
                if (IsWindowVisible(wnd)) SettingsRefreshStatus();
                // The search caret blinks.
                if (!g_query.empty()) InvalidateRect(wnd, &g_geo.search, FALSE);
                return 0;
            }
            if (wp == kTimerAnim) {
                bool more = g_list.Tick();
                const ULONGLONG now = GetTickCount64();
                if (now - g_pageShownAt < 220) more = true;
                if (!g_toast.empty() && now - g_toastAt < 2700) more = true;
                Invalidate();
                if (!more) { KillTimer(wnd, kTimerAnim); g_animating = false; }
                return 0;
            }
            break;

        case WM_AWA_CAPTUREKEY:
            CaptureKey((UINT)wp, (UINT)lp);
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && g_captureRow >= 0 && !ui::ModalOpen()) CancelCapture();
            Invalidate();
            break;

        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

        case WM_KEYDOWN:
            KeyDown((UINT)wp);
            return 0;

        case WM_SYSKEYDOWN:
            // Alt held while capturing a chord is part of the chord, not the
            // menu; otherwise Alt+F4 and friends go to the system as usual.
            if (g_captureRow >= 0 && !g_captureHook) { KeyDown((UINT)wp); return 0; }
            break;

        case WM_CHAR:
            Char((wchar_t)wp);
            return 0;

        case WM_MOUSEMOVE:
            MouseMove({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }, (wp & MK_LBUTTON) != 0);
            return 0;

        case WM_MOUSELEAVE:
            if (g_hot != HOT_NONE) { g_hot = HOT_NONE; Invalidate(); }
            g_list.MouseLeave();
            Animate();
            return 0;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            MouseDown({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            return 0;

        case WM_LBUTTONUP:
            MouseUp({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            return 0;

        case WM_CAPTURECHANGED:
            if (g_pressed != HOT_NONE) { g_pressed = HOT_NONE; Invalidate(); }
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(wnd, &pt);
            // The wheel always scrolls the settings, wherever the pointer is:
            // there is nothing else in the window that scrolls, and changing
            // category on a stray flick of the wheel lost people's place.
            (void)pt;
            g_list.Wheel(GET_WHEEL_DELTA_WPARAM(wp));
            Animate();
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
            break;

        case WM_SYSCOMMAND:
            // A bare Alt tap (or the Alt of a chord the capture hook swallowed
            // the key of) would start the system menu's keyboard loop, unseen,
            // and the next arrow key would open the window menu. Alt+Space still does.
            if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0) return 0;
            break;

        case WM_CLOSE:
            Close();
            return 0;

        case WM_DESTROY:
            EndCapture();
            KillTimer(wnd, kTimerStatus);
            KillTimer(wnd, kTimerAnim);
            g_animating = false;
            g_wnd = nullptr;
            return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

void Register(HINSTANCE inst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = SettingsProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm       = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    done = true;
}

} // namespace

// ================================================================ for the pages
HWND SettingsHwnd() { return g_wnd; }

void SettingsRebuild() {
    if (!g_wnd) return;
    BuildRows(true);
    UpdateDirty();
    Invalidate();
}

void SettingsCaptureRow(const std::wstring& rowId) {
    const auto& rows = g_list.Rows();
    for (int i = 0; i < (int)rows.size(); ++i) {
        if (rows[(size_t)i].id == rowId) {
            g_list.SetFocus(i);
            BeginCapture(i);
            return;
        }
    }
}

void SettingsToast(const std::wstring& text) { Toast(text); }

// ================================================================ public API
void SettingsRefreshStatus() {
    if (!g_wnd) return;
    Status s;
    ReadStatus(&s);
    const bool relayout = s.tiling != g_status.tiling;
    if (s.chip != g_status.chip || s.alert != g_status.alert || s.mark != g_status.mark || relayout) {
        g_status = s;
        if (relayout) DoLayout();
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
}

void SettingsRefresh() {
    if (!g_wnd) return;
    // Edits nobody has applied are the user's; something changing the live
    // config meanwhile does not get to throw them away (Rebase).
    if (!Dirty()) LoadEdit();
    else          Rebase(AppConfig());
    BuildRows(true);
    UpdateDirty();
    SettingsRefreshStatus();
    Invalidate();
}

HWND SettingsOpen(HINSTANCE inst) {
    if (!g_wnd) {
        Register(inst);
        LoadEdit();
        ReadStatus(&g_status);
        g_query.clear();
        g_zone = Zone::Nav;
        GetCursorPos(&g_mouseAnchor);
        g_mouseIdle = true;
        g_list = ui::RowList();
        g_list.onInvalidate = []() { Invalidate(); Animate(); };
        // Rows are rebuilt after every change, so a description that depends
        // on a value, or a row that only applies while another is on, is
        // always up to date. Not in search results, where a row whose words
        // changed could drop out from under the pointer.
        g_list.onChanged    = []() { if (g_query.empty()) BuildRows(true); UpdateDirty(); };
        g_list.onFocus      = []() { Invalidate(); };
        g_list.onCapture    = [](int row) { BeginCapture(row); };

        HWND wnd = CreateWindowExW(WS_EX_APPWINDOW, kClass, kAppName,
                                   WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU | WS_CAPTION,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800,
                                   nullptr, nullptr, inst, nullptr);
        if (!wnd) return nullptr;
        g_wnd = wnd;                    // WM_CREATE set it too; say so where it is used
        PlaceInitially();
        theme::SetDpi(DpiForWindow(wnd));
        DoLayout();
        BuildRows(false);
        g_list.SetActive(false);
        SetTimer(wnd, kTimerStatus, 700, nullptr);
        g_pageShownAt = GetTickCount64();
        Animate();
    } else {
        SettingsRefresh();
    }
    if (!g_wnd) return nullptr;
    ShowWindow(g_wnd, SW_SHOW);
    if (IsIconic(g_wnd)) ShowWindow(g_wnd, SW_RESTORE);
    SetForegroundWindow(g_wnd);
    return g_wnd;
}

void SettingsOpenTab(int index) {
    HWND wnd = SettingsOpen(GetModuleHandleW(nullptr));
    if (!wnd) return;
    ShowPage(index, true);
}

bool SettingsTranslateMessage(const MSG*) { return false; }

// Hiding is destroying. The window is a few megabytes of bitmaps and fonts
// held for something open a minute a week, and it is rebuilt from nothing in
// well under a hundred milliseconds when the tray icon is clicked. Edits that
// were never applied have already been settled by Close().
void SettingsHide() {
    if (!g_wnd) return;
    ShowWindow(g_wnd, SW_HIDE);
    SettingsDestroy();
    theme::TrimSurfaces();
    AppScheduleTrim(5 * 1000);
    if (!g_toldAboutTray) {
        g_toldAboutTray = true;
        AppTrayBalloon(kAppName,
                       L"Still running and still arranging windows. "
                       L"Click the tray icon to bring the settings back.");
    }
}

bool SettingsVisible() { return g_wnd && IsWindowVisible(g_wnd); }
HWND SettingsWindow()  { return g_wnd; }

void SettingsDestroy() {
    if (g_wnd) {
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
    }
}

} // namespace awa
