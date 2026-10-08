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
    X(clockScale) X(timerTick) X(timerAlarm)                                        \
    X(timerShown) X(timerPinned)                                                    \
    X(explorerStyler) X(explorerTheme) X(explorerEffect)                            \
    X(explorerTint) X(explorerTintOpacity) X(explorerHighlight) X(explorerRadius)   \
    X(explorerText) X(explorerRegion) X(explorerXamlDiag)                           \
    X(startStyler) X(startTheme) X(startLayout) X(startTint) X(startTintOpacity)    \
    X(startHighlight) X(startRadius) X(startText)                                   \
    X(startMinimized) X(debug)

bool ArraysEqual(const Config& a, const Config& b) {
    for (int i = 0; i < MON_METRIC_COUNT; ++i)
        if (a.monOrder[i] != b.monOrder[i] || a.monColor[i] != b.monColor[i]) return false;
    return true;
}

// Loads the edit copy from the live config.
void LoadEdit() {
    static Config defaults;
    static bool haveDefaults = false;
    if (!haveDefaults) { defaults.LoadDefaults(); haveDefaults = true; }
    ui::SetDefaultsSource(&g_edit, &defaults, sizeof(Config));
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
constexpr UINT_PTR kTimerMeters = 3;
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
    { L"Timer",
      L"The clock panel (Win+W): its settings, and the timers and stopwatch it runs, "
      L"which you can start, pause and reset from here.",
      BuildTimerPage, ResetTimerPage, nullptr, 0, nullptr },
    { L"Explorer",
      L"File Explorer's look: a theme for the address bar, command bar, tabs and "
      L"background, and styles of your own.",
      BuildExplorerPage, ResetExplorerPage, nullptr, 0, nullptr },
    { L"Start",
      L"The Start menu's look: a theme for the Start menu and the search flyout, "
      L"how glassy it is, and styles of your own.",
      BuildStartPage, ResetStartPage, nullptr, 0, nullptr },
    { L"General",
      L"How ProWindows starts, where its settings are kept, and what to reach for "
      L"when something on the desktop looks wrong.",
      BuildGeneralPage, ResetGeneralPage, nullptr, 0, nullptr },
    // Appended to match PAGE_WELCOME; the tab bar puts it first. Nothing to reset.
    { L"Welcome",
      L"What ProWindows does and the keys that matter. Come back here whenever a "
      L"shortcut slips your mind.",
      BuildWelcomePage, nullptr, nullptr, 0, nullptr },
};
static_assert(ARRAYSIZE(g_pages) == PAGE_COUNT, "PageIndex is out of step with g_pages");

enum class Zone { Nav, List };

// Things in the window that answer the mouse.
enum Hot {
    HOT_NONE = 0, HOT_MIN, HOT_CLOSE, HOT_KEYQ, HOT_KEYE, HOT_SUBKEY1, HOT_SUBKEY3,
    HOT_TAB = 100,           // + position in the tab bar
    HOT_SUB = 150,           // + sub-tab
    HOT_PROMPT = 200,        // + prompt
};

// The tab bar's order (PLAN-1.6 2.4). The categories themselves keep their
// PageIndex numbers, which callers outside the window use.
const int kTabOrder[PAGE_COUNT] = { PAGE_WELCOME, PAGE_LAYOUT, PAGE_BEHAVIOUR, PAGE_GENERAL, PAGE_SHORTCUTS,
                                    PAGE_APPS, PAGE_SEARCH, PAGE_MONITOR, PAGE_CLOCK, PAGE_TIMER, PAGE_EXPLORER, PAGE_START };
constexpr int kMaxSub = 8;

struct Geometry {
    RECT client = {};
    RECT winMin = {}, winClose = {};
    RECT search = {}, status = {};           // header: the search readout (left), status (right)
    RECT keyQ = {}, keyE = {};
    RECT tab[PAGE_COUNT] = {};
    bool subRow = false;
    RECT key1 = {}, key3 = {};
    RECT sub[kMaxSub] = {};
    int  subCount = 0;
    RECT rows = {}, track = {};              // the left column's list and its scrollbar
    RECT right = {};                         // the right column
    int  tabRuleY = 0, subRuleY = 0;
    int  footerCy = 0;
    int  headerBottom = 0;
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
bool        g_searching = false;       // / or Ctrl+F: typing goes into the query
std::wstring g_pageId;                 // the current page of the category: a Kind::Page row's id
struct PageTab { std::wstring id, label; };
std::vector<PageTab> g_pageTabs;       // the pages of the category, in order (empty while searching)
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

// ---------------------------------------------------------------- pages
// A category can split into pages, each a sub-tab (PLAN-1.6 2.6). A page is
// the run of rows after a Kind::Page row; the row list is only ever given the
// current page's rows, so the Page rows themselves are never drawn there.
void DoLayout();

bool HasPages() { return g_pageTabs.size() > 1; }

int PageNames(std::wstring* out, int max) {
    int n = 0;
    for (const auto& t : g_pageTabs) { if (n >= max) break; out[n++] = t.label; }
    return n;
}

int PageIndex(const std::wstring& id) {
    for (int i = 0; i < (int)g_pageTabs.size(); ++i) if (g_pageTabs[i].id == id) return i;
    return -1;
}

// The id of the page being shown: the one holding the focused row, which with
// one page's rows in the list is the current page; the first when unset.
std::wstring CurrentPage() {
    if (PageIndex(g_pageId) >= 0) return g_pageId;
    return g_pageTabs.empty() ? std::wstring() : g_pageTabs[0].id;
}

void BuildRows(bool keepFocus);

// Shows the page with this id and puts the focus on its first row.
void SetPage(const std::wstring& id) {
    if (!g_query.empty() || PageIndex(id) < 0) return;
    const bool changed = (id != CurrentPage());
    g_pageId = id;
    BuildRows(false);
    if (changed) {
        g_pageShownAt = GetTickCount64();
        Animate();
    }
    if (g_zone == Zone::List) g_list.FocusFirst();
    Invalidate();
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
        if (p == PAGE_WELCOME) continue;      // a guide to the other pages; its rows would only echo them
        std::vector<ui::Row> rows;
        g_pages[p].build(rows);
        std::wstring section;
        bool headed = false;
        for (auto& r : rows) {
            if (r.kind == ui::Kind::Page) continue;
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
    const std::vector<PageTab> hadTabs = g_pageTabs;
    g_pageTabs.clear();
    if (!g_query.empty()) {
        BuildSearchResults(rows);
    } else {
        std::vector<ui::Row> all;
        g_pages[g_page].build(all);
        for (const auto& r : all)
            if (r.kind == ui::Kind::Page) g_pageTabs.push_back({ r.id, r.label });
        if (PageIndex(g_pageId) < 0) g_pageId = g_pageTabs.empty() ? std::wstring() : g_pageTabs[0].id;
        // Rows before the first Page row (a category that starts without one)
        // belong to the first page.
        bool inPage = g_pageTabs.empty() || g_pageTabs[0].id == g_pageId;
        for (auto& r : all) {
            if (r.kind == ui::Kind::Page) { inPage = (r.id == g_pageId); continue; }
            if (!inPage) continue;
            r.page = g_pages[g_page].caption;
            rows.push_back(std::move(r));
        }
    }
    g_list.SetRows(std::move(rows), keepFocus);
    bool sameTabs = hadTabs.size() == g_pageTabs.size();
    for (size_t i = 0; sameTabs && i < hadTabs.size(); ++i) sameTabs = hadTabs[i].id == g_pageTabs[i].id;
    if (g_wnd && !sameTabs) DoLayout();        // the sub-tab row's size follows the page names
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
// The layout is written in DIPs for a 1280 x 720 window (PLAN-1.6 2.4): the
// header, the tab bar and the footer keep their size and the two columns take
// whatever width is left, in the same proportions.

int TabOf(int page) {
    for (int t = 0; t < PAGE_COUNT; ++t) if (kTabOrder[t] == page) return t;
    return 0;
}

void DoLayout() {
    Geometry& g = g_geo;
    GetClientRect(g_wnd, &g.client);
    const int W = g.client.right, H = g.client.bottom;
    const int dpi = (int)theme::Dpi();
    const int wd = MulDiv(W, 96, dpi), hd = MulDiv(H, 96, dpi);
    const int M = 70;

    g.winClose = { W - Sc(66), Sc(10), W - Sc(26), Sc(44) };
    g.winMin   = { W - Sc(106), Sc(10), W - Sc(66), Sc(44) };
    g.search   = { Sc(M), Sc(14), W / 2 - Sc(120), Sc(60) };
    g.status   = { W / 2 + Sc(120), Sc(14), g.winMin.left - Sc(8), Sc(60) };
    g.headerBottom = Sc(60);

    HDC dc = GetDC(g_wnd);

    // The tab bar: a keycap at each end, eight cells between them.
    const int tabTop = 64, tabBottom = 98;
    const int cyTab = Sc((tabTop + tabBottom) / 2);
    const int kw = theme::Keycap(dc, 0, 0, L"Q", 0, true);
    g.keyQ = { Sc(M), cyTab - Sc(14), Sc(M) + kw, cyTab + Sc(14) };
    g.keyE = { W - Sc(M) - kw, cyTab - Sc(14), W - Sc(M), cyTab + Sc(14) };
    const int cellsL = g.keyQ.right + Sc(18), cellsR = g.keyE.left - Sc(18);
    for (int t = 0; t < PAGE_COUNT; ++t)
        g.tab[t] = { cellsL + (cellsR - cellsL) * t / PAGE_COUNT, Sc(tabTop),
                     cellsL + (cellsR - cellsL) * (t + 1) / PAGE_COUNT, Sc(tabBottom) };
    g.tabRuleY = Sc(99);

    // The columns.
    const int span = wd - 2 * M;
    const int leftW = span * 600 / 1140;
    g.subRow = HasPages();
    g.subCount = 0;
    int listTop = 118;
    if (g.subRow) {
        listTop = 184;
        g.subRuleY = Sc(171);
        std::wstring names[kMaxSub];
        const int n = PageNames(names, kMaxSub);
        const int cy = Sc(154);
        g.key1 = { Sc(104), cy - Sc(14), Sc(104) + theme::Keycap(dc, 0, 0, L"1", 0, true), cy + Sc(14) };
        int x = g.key1.right + Sc(22);
        for (int i = 0; i < n; ++i) {
            const int w = theme::Measure(dc, Font::Row, names[i]);
            g.sub[i] = { x, Sc(140), x + w, Sc(168) };
            x += w + Sc(30);
            g.subCount = i + 1;
        }
        g.key3 = { x - Sc(8), cy - Sc(14), x - Sc(8) + theme::Keycap(dc, 0, 0, L"3", 0, true), cy + Sc(14) };
    }
    ReleaseDC(g_wnd, dc);

    const int listBottom = hd - 64;
    g.rows  = { Sc(M), Sc(listTop), Sc(M + leftW), Sc(listBottom) };
    g.track = { Sc(M + leftW + 23), Sc(listTop), Sc(M + leftW + 23) + Sc(3), Sc(listBottom) };
    g.right = { Sc(M + leftW + 72), Sc(listTop), W - Sc(M), Sc(listBottom) };
    g_list.SetBounds(g.rows, g.track);
    g.footerCy = Sc(hd - 26);
}

// ---------------------------------------------------------------- painting
void PaintHeader(HDC dc) {
    const Geometry& g = g_geo;
    const int W = g.client.right;

    // The title, centred, its baseline near y 52.
    RECT title = { Sc(70), Sc(10), W, Sc(58) };
    theme::Print(dc, Font::Heading, L"SETTINGS", title, theme::TextHi,
                 DT_LEFT | DT_BOTTOM | DT_SINGLELINE);

    // Left: what is being searched for, while a search is open.
    if (g_searching) {
        RECT s = g.search;
        const int cy = (s.top + s.bottom) / 2 + Sc(6);
        RECT t = { s.left, cy - Sc(14), s.right, cy + Sc(14) };
        if (g_query.empty()) {
            theme::Print(dc, Font::Body, L"Type to search the settings", t, theme::TextMute,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else {
            theme::Print(dc, Font::Body, g_query, t, theme::TextHi,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        const int w = (std::min)((int)(s.right - s.left),
                                 g_query.empty() ? 0 : theme::Measure(dc, Font::Body, g_query));
        if ((GetTickCount64() / 530) % 2 == 0) {
            RECT caret = { s.left + w + Sc(1), cy - Sc(9), s.left + w + Sc(1) + (std::max)(1, Sc(1)), cy + Sc(9) };
            theme::Wash(dc, caret, theme::TextHi, 255);
        }
    }

    // Right, left of the window's own buttons: a message, the reminder that
    // something is not applied, or how the tiler is doing.
    {
        RECT s = g.status;
        const int cy = (s.top + s.bottom) / 2 + Sc(6);
        RECT t = { s.left, cy - Sc(12), s.right, cy + Sc(12) };
        const ULONGLONG age = GetTickCount64() - g_toastAt;
        const UINT fmt = DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
        if (!g_toast.empty() && age < 2600) {
            const float fade = age < 2100 ? 1.0f : 1.0f - (float)(age - 2100) / 500.0f;
            theme::Print(dc, Font::Small, g_toast, t, theme::Mix(theme::Bg, theme::Good, fade), fmt);
        } else if (g_dirty) {
            const int n = ChangeCount();
            theme::Print(dc, Font::Small,
                         std::to_wstring(n) + (n == 1 ? L" change not applied yet" : L" changes not applied yet"),
                         t, theme::TextBody, fmt);
        } else if (!g_status.alert.empty()) {
            theme::Print(dc, Font::Small, g_status.alert, t, theme::Danger, fmt);
        } else {
            theme::Print(dc, Font::Small, g_status.chip, t, theme::TextMute, fmt);
        }
    }

    // The window's own two buttons: thin strokes, white under the pointer.
    {
        const int t = (std::max)(1, Sc(1));
        const bool hm = (g_hot == HOT_MIN), hc = (g_hot == HOT_CLOSE);
        const int mx = (g.winMin.left + g.winMin.right) / 2, my = (g.winMin.top + g.winMin.bottom) / 2;
        RECT dash = { mx - Sc(6), my, mx + Sc(6), my + t };
        theme::Wash(dc, dash, hm ? theme::TextHi : theme::TextDim, 255);
        const float cx = (float)(g.winClose.left + g.winClose.right) / 2.0f;
        const float cyy = (float)(g.winClose.top + g.winClose.bottom) / 2.0f;
        const float s = theme::ScaleF(5.5f);
        HPEN pen = CreatePen(PS_SOLID, t, hc ? theme::TextHi : theme::TextDim);
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, (int)(cx - s), (int)(cyy - s), nullptr); LineTo(dc, (int)(cx + s) + 1, (int)(cyy + s) + 1);
        MoveToEx(dc, (int)(cx + s), (int)(cyy - s), nullptr); LineTo(dc, (int)(cx - s) - 1, (int)(cyy + s) + 1);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
}

void PaintTabs(HDC dc) {
    const Geometry& g = g_geo;
    const int W = g.client.right;
    const int cy = (g.tab[0].top + g.tab[0].bottom) / 2;
    const int one = (std::max)(1, Sc(1));

    theme::Keycap(dc, g.keyQ.left, cy, L"Q", 0, false, g_hot == HOT_KEYQ);
    theme::Keycap(dc, g.keyE.left, cy, L"E", 0, false, g_hot == HOT_KEYE);

    for (int t = 0; t < PAGE_COUNT; ++t) {
        const RECT& r = g.tab[t];
        const int page = kTabOrder[t];
        const bool active = g_query.empty() && page == g_page;
        const bool hot = (g_hot == HOT_TAB + t);
        const std::wstring caps = theme::Caps(g_pages[page].caption);
        // A caption that will not fit the cell at tab size falls back to the row font.
        const bool narrow = theme::Measure(dc, Font::Tab, caps, Sc(1)) > (r.right - r.left) - Sc(14);
        const Font tabFont = narrow ? Font::Row : Font::Tab;
        const int track = narrow ? 0 : Sc(1);
        {
            RECT rule = { r.left, cy - Sc(9), r.left + one, cy + Sc(9) };
            theme::Wash(dc, rule, theme::Rule, 255);
        }
        if (t == PAGE_COUNT - 1) {
            RECT rule = { r.right - one, cy - Sc(9), r.right, cy + Sc(9) };
            theme::Wash(dc, rule, theme::Rule, 255);
        }
        theme::Print(dc, tabFont, caps, r, active ? theme::TextHi : hot ? theme::Text : theme::TextDim,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE, track);
        const int tw = theme::Measure(dc, tabFont, caps, track);
        const int mid = (r.left + r.right) / 2;
        if (active) {
            // A short soft light under the word, sitting on the hairline.
            RECT light = { mid - tw / 2 - Sc(12), g.tabRuleY - Sc(8), mid + tw / 2 + Sc(12), g.tabRuleY + Sc(8) };
            theme::Haze(dc, light, theme::TextHi, 150);
            RECT bar = { mid - tw / 2, g.tabRuleY - one, mid + tw / 2, g.tabRuleY + one };
            theme::Wash(dc, bar, theme::TextHi, 255);
        }
        if (g_pages[page].issues && g_pages[page].issues() > 0)
            theme::Diamond(dc, (float)(mid + tw / 2 + Sc(9)), (float)(cy - Sc(6)), theme::ScaleF(3.0f), theme::Danger);
    }
    RECT hair = { Sc(70), g.tabRuleY, W - Sc(70), g.tabRuleY + one };
    theme::Wash(dc, hair, theme::Line, 255);
}

void PaintSubTabs(HDC dc) {
    const Geometry& g = g_geo;
    if (!g.subRow || g.subCount == 0) return;
    std::wstring names[kMaxSub];
    PageNames(names, kMaxSub);
    const int cy = (g.sub[0].top + g.sub[0].bottom) / 2;
    theme::Keycap(dc, g.key1.left, cy, L"1", 0, false, g_hot == HOT_SUBKEY1);
    theme::Keycap(dc, g.key3.left, cy, L"3", 0, false, g_hot == HOT_SUBKEY3);
    for (int i = 0; i < g.subCount; ++i) {
        const bool active = (i == PageIndex(CurrentPage())), hot = (g_hot == HOT_SUB + i);
        theme::Print(dc, Font::Row, names[i], g.sub[i], active ? theme::TextHi : hot ? theme::Text : theme::TextDim,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (active) {
            RECT light = { g.sub[i].left - Sc(6), g.sub[i].bottom - Sc(6), g.sub[i].right + Sc(6), g.sub[i].bottom + Sc(4) };
            theme::Haze(dc, light, theme::TextHi, 130);
        }
    }
    RECT hair = { g.rows.left, g.subRuleY, g.rows.right, g.subRuleY + (std::max)(1, Sc(1)) };
    theme::Wash(dc, hair, theme::Line, 255);
}

// Figures behind the right column's meters: read when a category opens and
// once a second while the window is visible and in front.
struct Meters { int memMB = 0, managed = 0, topLevel = 0, index = 0, icons = 0, cost = -1; };
Meters g_meters;

BOOL CALLBACK CountTop(HWND h, LPARAM lp) {
    if (IsWindowVisible(h) && !IsIconic(h) && GetWindow(h, GW_OWNER) == nullptr &&
        !(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) && GetWindowTextLengthW(h) > 0)
        ++*(int*)lp;
    return TRUE;
}

void ReadMeters(bool all) {
    g_meters.memMB = AppMemoryMB();
    g_meters.managed = AppManagedWindows();
    g_meters.index = AppIndexEntries();
    g_meters.icons = AppIconCacheCount();
    g_meters.cost = AppSamplerCostTenths();
    if (all) { int n = 0; EnumWindows(CountTop, (LPARAM)&n); g_meters.topLevel = n; }
}

// The right column: what the focused row says about itself, then a picture.
void PaintRight(HDC dc) {
    const Geometry& g = g_geo;
    RECT d = g.right;
    if (d.right - d.left < Sc(120)) return;
    const ui::Row* row = (g_zone == Zone::Nav && g_query.empty()) ? nullptr : g_list.FocusedRow();
    const int page = PageOfRow(row);

    std::wstring body;
    if (!row) {
        body = g_pages[g_page].blurb;
    } else {
        body = row->help;
        if (g_captureRow >= 0)
            body = L"Hold the modifiers you want - Win, Ctrl, Alt, Shift - and press the "
                   L"key. The shortcut is taken the moment the key goes down.\r\n\r\n"
                   L"Esc on its own cancels. A key with no modifier only works for F1 to "
                   L"F24 and the media keys, which nobody types with.";
    }

    int y = d.top;
    if (!g_query.empty() && row) {
        RECT in = { d.left, y, d.right, y + Sc(18) };
        theme::Print(dc, Font::Small, L"In " + std::wstring(g_pages[page].caption), in, theme::TextMute,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += Sc(24);
    }
    if (row && row->modified && row->modified()) {
        RECT m = { d.left, y, d.right, y + Sc(18) };
        theme::Print(dc, Font::Small, L"Changed, not applied yet", m, theme::TextDim,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += Sc(24);
    }
    if (row && !row->Enabled()) {
        RECT m = { d.left, y, d.right, y + Sc(18) };
        theme::Print(dc, Font::Small, L"Not available right now", m, theme::TextMute,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += Sc(24);
    }
    // What the right column carries below the text: the picture, 16:9, and
    // the category's meters under it.
    struct MeterRow { std::wstring label; std::wstring value; float used, other; };
    std::vector<MeterRow> meters;
    if (page == PAGE_GENERAL) {
        meters.push_back({ L"Memory usage", std::to_wstring(g_meters.memMB) + L" / 64 MB",
                           g_meters.memMB / 64.0f, 0.0f });
        const int tot = (std::max)(g_meters.managed, g_meters.topLevel);
        meters.push_back({ L"Windows arranged", std::to_wstring(g_meters.managed) + L" of " + std::to_wstring(tot),
                           tot ? (float)g_meters.managed / tot : 0.0f,
                           tot ? (float)(tot - g_meters.managed) / tot : 0.0f });
    } else if (page == PAGE_SEARCH) {
        const int cap = (std::max)(1, Saved().searchMaxEntries);
        meters.push_back({ L"File index", std::to_wstring(g_meters.index) + L" / " + std::to_wstring(cap),
                           (float)g_meters.index / cap, 0.0f });
        meters.push_back({ L"Icon cache", std::to_wstring(g_meters.icons) + L" / 512",
                           g_meters.icons / 512.0f, 0.0f });
    } else if (page == PAGE_MONITOR) {
        const int every = (std::max)(100, Saved().monitorInterval);
        if (g_meters.cost < 0)
            meters.push_back({ L"Sampling cost", L"not sampling", 0.0f, 0.0f });
        else
            meters.push_back({ L"Sampling cost",
                               std::to_wstring(g_meters.cost / 10) + L"." + std::to_wstring(g_meters.cost % 10) +
                               L" ms / " + std::to_wstring(every) + L" ms",
                               g_meters.cost / (every * 10.0f), 0.0f });
    }
    const int meterPitch = Sc(32);
    const int metersH = (int)meters.size() * meterPitch;

    const int w = d.right - d.left;
    const int pw = (std::min)(w, (int)Sc(468)), ph = pw * 9 / 16;
    int top = d.top + Sc(186);
    top = (std::min)(top, (int)d.bottom - metersH - (meters.empty() ? 0 : Sc(14)) - ph);
    top = (std::max)(top, y + Sc(40));

    // Description, then (Default: ...) under it.
    const int avail = top - Sc(10) - y;
    int used = 0;
    {
        const int saved = SaveDC(dc);
        RECT br = { d.left, y, d.right, y + (std::max)(0, avail) };
        used = (std::min)((int)avail, (int)theme::PrintWrapped(dc, Font::Desc, body, br, theme::TextBody, true));
        IntersectClipRect(dc, br.left, br.top, br.right, br.bottom);
        theme::PrintWrapped(dc, Font::Desc, body, br, theme::TextBody);
        RestoreDC(dc, saved);
    }
    if (row && row->fallback && row->kind != ui::Kind::Action && row->kind != ui::Kind::Item &&
        used + Sc(34) <= avail + Sc(8)) {
        const std::wstring def = row->fallback();
        if (!def.empty()) {
            RECT fr = { d.left, y + used + Sc(8), d.right, y + used + Sc(34) };
            theme::Print(dc, Font::Desc, L"(Default: " + def + L")", fr, theme::TextBody,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    // The picture: the category's preview, or the mark, large and dim.
    {
        RECT box = { d.left, top, d.left + pw, top + ph };
        theme::Wash(dc, box, RGB(0, 0, 0), 255);
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, box.left, box.top, box.right, box.bottom);
        if (g_pages[page].preview) {
            g_pages[page].preview(dc, box);
        } else {
            const int s = (std::min)(Sc(200), ph - Sc(8));
            const int cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
            RECT mark = { cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2 };
            theme::Mark(dc, mark, theme::TextMute, theme::TextMute);
        }
        RestoreDC(dc, saved);
    }

    // The meters: label and figure on the left, the bar on the right.
    int my = top + ph + Sc(14);
    for (const MeterRow& m : meters) {
        const int barW = Sc(180), barH = Sc(10);
        RECT lr = { d.left, my, d.right - barW - Sc(14), my + meterPitch - Sc(6) };
        RECT bar = { d.right - barW, my + (lr.bottom - lr.top - barH) / 2, d.right,
                     my + (lr.bottom - lr.top - barH) / 2 + barH };
        theme::Print(dc, Font::Desc, m.label, lr, theme::TextBody, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT vr = lr;
        vr.left += Sc(140);
        theme::Print(dc, Font::Desc, m.value, vr, theme::TextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        theme::Meter(dc, bar, m.used, m.other);
        my += meterPitch;
    }
}

void PaintFooter(HDC dc) {
    const Geometry& g = g_geo;

    // What the keys do right now: the row's own first, then the window's.
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
        } else {
            prompts.push_back({ L"Enter", L"Select" });
            keys.push_back(VK_RETURN);
        }
        if (g_dirty) { prompts.push_back({ L"Ctrl+S", L"Apply" }); keys.push_back('S' | 0x10000); }
        if (!g_searching && g_pages[g_page].reset) { prompts.push_back({ L"R", L"Reset category" }); keys.push_back('R'); }
        prompts.push_back({ L"Tab", L"Reset all" });
        keys.push_back(VK_TAB);
        prompts.push_back({ L"Esc", (g_zone == Zone::Nav && !g_searching) ? L"Close" : L"Back" });
        keys.push_back(VK_ESCAPE);
    }

    const int cy = g.footerCy;
    const int gap = Sc(28);
    int total = 0;
    std::vector<int> widths;
    for (const auto& p : prompts) {
        widths.push_back(theme::Prompt(dc, 0, cy, p.first, p.second, theme::Text, true));
        total += widths.back() + (total ? gap : 0);
    }
    g_promptHits.clear();
    int x = (std::max)(Sc(20), (int)(g.client.right - total) / 2);
    for (size_t i = 0; i < prompts.size(); ++i) {
        const bool hot = (g_hot == HOT_PROMPT + (int)g_promptHits.size());
        theme::Prompt(dc, x, cy, prompts[i].first, prompts[i].second, hot ? theme::TextHi : theme::Text);
        g_promptHits.push_back({ RECT{ x, cy - Sc(14), x + widths[i], cy + Sc(14) }, keys[i] });
        x += widths[i] + gap;
    }
}

void PaintAll(HDC dc) {
    const Geometry& g = g_geo;
    theme::SetDpi(DpiForWindow(g_wnd));

    // The screen is flat black.
    {
        HBRUSH black = CreateSolidBrush(theme::Bg);
        FillRect(dc, &g.client, black);
        DeleteObject(black);
    }

    PaintHeader(dc);
    PaintTabs(dc);
    PaintSubTabs(dc);

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
        RECT area = { g.rows.left - Sc(30), g.rows.top, g.track.right + Sc(4), g.rows.bottom };
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

    PaintRight(dc);
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
    g_searching = false;
    if (changed) g_pageId.clear();
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
    const bool saved = AppApplySettings();   // save to disk, reload, re-register keys
    if (layoutChanged)
        AppWm().SetLayoutEverywhere(snapshot.layout, snapshot.masterRatio, snapshot.masterCount);
    AppUpdateTray();

    LoadEdit();
    BuildRows(true);
    UpdateDirty();
    if (saved) {
        Toast(L"Settings applied and saved");
    } else {
        Toast(L"Settings applied, but not saved");
        ui::Notice(g_wnd, L"Could not save",
                   L"Your changes are in effect now, but config.ini could not be written, "
                   L"so they will be lost when ProWindows restarts. Something may have the "
                   L"file open - an editor or a sync client. Apply again once it is closed.");
    }
}

void ResetPage() {
    if (!g_query.empty() || !g_pages[g_page].reset) return;
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

// Everything in every category back to how it shipped; asks first.
void ResetAll() {
    if (!ui::Confirm(g_wnd, L"Reset everything?",
                     L"Every setting in every category goes back to how it shipped. "
                     L"Nothing is saved until you apply.",
                     L"Reset all", L"Cancel"))
        return;
    for (int p = 0; p < PAGE_COUNT; ++p) if (g_pages[p].reset) g_pages[p].reset();
    BuildRows(true);
    UpdateDirty();
    Toast(L"Everything reset to defaults - apply to keep it");
}

// Steps the tab bar, wrapping at the ends; the keyboard stays where it was.
void StepTab(int delta) {
    const int t = ((TabOf(g_page) + delta) % PAGE_COUNT + PAGE_COUNT) % PAGE_COUNT;
    ShowPage(kTabOrder[t], g_zone == Zone::List);
}

// Steps the pages of the category (PLAN-1.6 2.6); stops at the ends.
void StepSub(int delta) {
    if (!HasPages() || !g_query.empty()) return;
    const int n = (int)g_pageTabs.size();
    const int i = (std::max)(0, (std::min)(n - 1, PageIndex(CurrentPage()) + delta));
    SetPage(g_pageTabs[(size_t)i].id);
}

void PressButton(int hot) {
    switch (hot) {
        case HOT_MIN:     ShowWindow(g_wnd, SW_MINIMIZE); break;
        case HOT_CLOSE:   Close(); break;
        case HOT_KEYQ:    StepTab(-1); break;
        case HOT_KEYE:    StepTab(+1); break;
        case HOT_SUBKEY1: StepSub(-1); break;
        case HOT_SUBKEY3: StepSub(+1); break;
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
    if (PtInRect(&g.keyQ, pt))     return HOT_KEYQ;
    if (PtInRect(&g.keyE, pt))     return HOT_KEYE;
    for (int t = 0; t < PAGE_COUNT; ++t)
        if (PtInRect(&g.tab[t], pt)) return HOT_TAB + t;
    if (g.subRow) {
        if (PtInRect(&g.key1, pt)) return HOT_SUBKEY1;
        if (PtInRect(&g.key3, pt)) return HOT_SUBKEY3;
        for (int i = 0; i < g.subCount; ++i)
            if (PtInRect(&g.sub[i], pt)) return HOT_SUB + i;
    }
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

void StartSearch() {
    g_searching = true;
    Invalidate();
}

void StopSearch() {
    const bool had = !g_query.empty();
    g_searching = false;
    g_query.clear();
    if (had) BuildRows(false);
    SetZone(Zone::Nav);
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
        StepTab((vk == VK_PRIOR || (vk == VK_TAB && shift)) ? -1 : +1);
        return;
    }
    if (ctrl && vk == 'S') { ApplyNow(); return; }
    if (ctrl && vk == 'F') { StartSearch(); return; }

    if (vk == VK_ESCAPE) {
        if (g_searching) { StopSearch(); return; }
        if (g_zone == Zone::List) { SetZone(Zone::Nav); return; }
        Close();
        return;
    }
    if (vk == VK_TAB) { ResetAll(); return; }
    if (vk == VK_BACK && g_searching) {
        if (g_query.empty()) { StopSearch(); return; }
        g_query.pop_back();
        SearchChanged();
        return;
    }

    // Bare letters step and reset only when they are not being typed into a search.
    if (!ctrl && !g_searching) {
        switch (vk) {
            case 'Q': StepTab(-1); return;
            case 'E': StepTab(+1); return;
            case 'R': ResetPage(); return;
            case '1': StepSub(-1); return;
            case '3': StepSub(+1); return;
            default: break;
        }
    }

    switch (g_zone) {
        case Zone::Nav:
            if (vk == VK_LEFT || vk == VK_RIGHT) {
                StepTab(vk == VK_LEFT ? -1 : +1);
            } else if (vk == VK_HOME) {
                ShowPage(kTabOrder[0], false);
            } else if (vk == VK_END) {
                ShowPage(kTabOrder[PAGE_COUNT - 1], false);
            } else if (vk == VK_DOWN || vk == VK_RETURN || vk == VK_SPACE) {
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
    }
}

void Char(wchar_t ch) {
    if (g_captureRow >= 0) return;
    if (ch < 32 || ch == 127) return;
    if (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) return;
    // Letters are the game's own keys now (Q, E, R, 1, 3); a search is opened
    // with / or Ctrl+F and only then does typing go into it.
    if (!g_searching) {
        if (ch == L'/') StartSearch();
        return;
    }
    // A space only continues a query, since on its own it presses the row.
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
    if (hot >= HOT_TAB && hot < HOT_TAB + PAGE_COUNT) {
        SetZone(Zone::Nav);
        ShowPage(kTabOrder[hot - HOT_TAB], false);
        return;
    }
    if (hot >= HOT_SUB && hot < HOT_SUB + kMaxSub) {
        const int i = hot - HOT_SUB;
        if (i < (int)g_pageTabs.size()) SetPage(g_pageTabs[(size_t)i].id);
        return;
    }
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
    const int w = (std::min)(MulDiv(1280, (int)dpi, 96), waW * 96 / 100);
    const int h = (std::min)(MulDiv(720, (int)dpi, 96), waH * 94 / 100);
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
            // Restored from the taskbar or Alt+Tab, the activation can arrive while
            // still iconic, and WM_ACTIVATE then left the meter tick off.
            if (wp == SIZE_MINIMIZED) KillTimer(wnd, kTimerMeters);
            else if (GetForegroundWindow() == wnd) SetTimer(wnd, kTimerMeters, 1000, nullptr);
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
                // The Timer page counts down on this tick, and rebuilds when a timer
                // changed elsewhere (the panel, a timer ending).
                if (IsWindowVisible(wnd) && g_page == PAGE_TIMER && g_query.empty() &&
                    g_captureRow < 0 && !ui::ModalOpen()) {
                    if (TimerPageStale()) { BuildRows(true); Invalidate(); }
                    else InvalidateRect(wnd, &g_geo.right, FALSE);
                }
                // The search caret blinks.
                if (g_searching) InvalidateRect(wnd, &g_geo.search, FALSE);
                return 0;
            }
            if (wp == kTimerMeters) {
                // Runs only while the window is active (WM_ACTIVATE): cached
                // getters, no window walk - that one is taken on activation.
                ReadMeters(false);
                InvalidateRect(wnd, &g_geo.right, FALSE);
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
            // The meter tick exists only while the window is in front and shown.
            if (LOWORD(wp) == WA_INACTIVE || IsIconic(wnd)) {
                KillTimer(wnd, kTimerMeters);
            } else {
                ReadMeters(true);
                SetTimer(wnd, kTimerMeters, 1000, nullptr);
            }
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
            KillTimer(wnd, kTimerMeters);
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

RECT SettingsPromptRect(UINT key) {
    for (const auto& p : g_promptHits)
        if (p.second == key) return p.first;
    return RECT{};
}

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
        g_searching = false;
        g_pageId.clear();
        g_pageTabs.clear();
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
        ReadMeters(true);       // the meter timer starts with WM_ACTIVATE
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
