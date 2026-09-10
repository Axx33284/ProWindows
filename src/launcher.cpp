#include "launcher.h"
#include "winutil.h"
#include "search.h"
#include "theme.h"
#include "appicon.h"
#include <objidl.h>
#include <shlobj.h>      // IShellLinkW, for resolving what a .lnk points at
#include <cwchar>        // _wcsnicmp
#include <cstring>       // memcpy
// GDI+ headers use bare min/max, which NOMINMAX removes. Feed them the
// std:: versions rather than re-enabling the Windows macros.
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>

#pragma comment(lib, "shell32.lib")

namespace awa {

namespace {

constexpr wchar_t kClass[]    = L"ProWindows_Launcher";
constexpr UINT_PTR kTimerCaret = 1;
constexpr int kMaxRows = 8;        // more than this and you should type more

// Posted by the icon loader when one has arrived and the list is worth
// redrawing. WM_APP + 1 is the app catalogue finishing its scan.
constexpr UINT WM_AWA_APPSREADY = WM_APP + 1;
constexpr UINT WM_AWA_ICONREADY = WM_APP + 2;

// Unscaled layout, in the same spirit as the overlay: one place to change it.
constexpr int kWidth    = 640;
constexpr int kInputH   = 56;
constexpr int kRowH     = 46;
constexpr int kPad      = 14;
constexpr int kIconPx   = 28;      // the shell icon beside each result
// The key hints along the bottom. Present only when there is a list to act on,
// so an empty bar stays an empty bar.
constexpr int kFooterH  = 26;

HINSTANCE g_inst = nullptr;
Config*   g_cfg  = nullptr;
HWND      g_wnd  = nullptr;
float     g_scale = 1.0f;

std::wstring g_query;
int  g_selected = 0;
bool g_caretOn  = true;

// Right-click menu commands.
enum : int {
    IDM_OPEN = 1, IDM_RUNAS, IDM_LOCATION, IDM_COPYPATH, IDM_FORGET,
};

// ------------------------------------------------------------------ catalogue
// Filled by a background thread, read by the UI thread.
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
std::vector<InstalledApp> g_apps;
// Lower-cased names, one per entry of g_apps. Matching is the only thing that
// happens on every keystroke, so it must not allocate a string per app per
// letter typed.
std::vector<std::wstring> g_lowNames;
bool   g_appsReady = false;
HANDLE g_loadThread = nullptr;

// How often each app has been launched from here, so the things you actually
// use rise to the top of a short query.
std::unordered_map<std::wstring, int> g_uses;

// A row on screen. Everything is *copied* out of its source rather than
// pointed at: the scan and index threads replace their catalogues wholesale at
// any moment, and painting must never have to reason about that.
using Hit = SearchHit;
std::vector<Hit> g_hits;

// True while the right-click menu is up, so losing activation to it is not
// mistaken for the user clicking away.
bool g_menuOpen = false;

// Where the pointer was when the bar was last shown. Windows delivers a
// WM_MOUSEMOVE to a window that appears underneath a stationary cursor, and
// acting on it moved the selection off the first row before a single key had
// been pressed - so the pointer has to genuinely move before it gets a say.
POINT g_mouseAnchor = {};
bool  g_mouseIdle   = true;

// Call with g_lock held.
void BuildLowNames() {
    g_lowNames.clear();
    g_lowNames.reserve(g_apps.size());
    for (const auto& app : g_apps) g_lowNames.push_back(ToLower(app.name));
}

std::wstring UsagePath() { return ConfigDir() + L"\\launcher.txt"; }

void LoadUsage() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, UsagePath().c_str(), L"r, ccs=UTF-8") != 0 || !f) return;

    wchar_t line[1024];
    while (fgetws(line, 1024, f)) {
        std::wstring s = Trim(line);
        const size_t bar = s.find(L'|');
        if (bar == std::wstring::npos || bar == 0) continue;
        const int count = _wtoi(s.substr(0, bar).c_str());
        const std::wstring name = s.substr(bar + 1);
        if (count > 0 && !name.empty()) g_uses[ToLower(name)] = count;
    }
    fclose(f);
}

void SaveUsage() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, UsagePath().c_str(), L"w, ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"# ProWindows launcher - how often each entry has been opened.\n");
    for (const auto& e : g_uses)
        if (e.second > 0) fwprintf(f, L"%d|%s\n", e.second, e.first.c_str());
    fclose(f);
}

// Set at shutdown. A scan that outlives LauncherShutdown must not write to
// globals the process is about to tear down, so it checks this under the lock
// and drops its results on the floor.
bool g_abandonLoad = false;

// What size the background sweep should fetch. The bar itself asks for
// `kIconPx * g_scale`, and g_scale follows whichever monitor it opens on - but
// the sweep happens long before that, so it goes with the window's own monitor.
// Guessing wrong costs one live fetch on the other screen, not a wrong picture.
int PrefetchIconSize() {
    const UINT dpi = g_wnd ? DpiForWindow(g_wnd) : 96;
    const float scale = dpi ? (float)dpi / 96.0f : 1.0f;
    return (int)(kIconPx * scale);
}

DWORD WINAPI LoadAppsThread(LPVOID) {
    // This runs at startup, walking the Start menu folders and the shell's
    // AppsFolder - thousands of small reads. Background mode drops the thread's
    // I/O priority as well as its CPU priority, so on a mechanical disk the
    // scan yields to whatever the user is actually waiting for instead of
    // racing it. The file indexer has done this from the start; the app scan
    // was the one walk still competing at full speed.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);

    // The shell enumeration needs an apartment; this thread owns its own.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<InstalledApp> found = EnumAllApps();
    if (SUCCEEDED(hr)) CoUninitialize();

    EnterCriticalSection(&g_lock);
    const bool abandoned = g_abandonLoad;
    if (!abandoned) {
        g_apps = std::move(found);
        BuildLowNames();
        g_appsReady = true;
    }
    LeaveCriticalSection(&g_lock);

    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    if (abandoned) return 0;

    // Every icon the bar could ever need, fetched in the background at lowered
    // I/O priority and written to `icons.cache` on the way out - so the first
    // launch is the only one that ever draws a lettered tile. Anything on
    // screen overtakes this however long it is; see appicon.h.
    {
        std::vector<std::wstring> targets;
        EnterCriticalSection(&g_lock);
        targets.reserve(g_apps.size());
        for (const auto& app : g_apps) targets.push_back(app.launchPath);
        LeaveCriticalSection(&g_lock);
        AppIconPrefetch(targets, PrefetchIconSize());
    }

    // The window may be open already, waiting on exactly this.
    if (g_wnd) PostMessageW(g_wnd, WM_AWA_APPSREADY, 0, 0);
    return 0;
}

void StartLoad() {
    if (g_loadThread) {
        // A previous scan may still be running; let it finish on its own.
        if (WaitForSingleObject(g_loadThread, 0) != WAIT_OBJECT_0) return;
        CloseHandle(g_loadThread);
        g_loadThread = nullptr;
    }
    EnterCriticalSection(&g_lock);
    g_appsReady = false;
    LeaveCriticalSection(&g_lock);
    g_loadThread = CreateThread(nullptr, 0, LoadAppsThread, nullptr, 0, nullptr);
}

// ------------------------------------------------------------------ matching
// Ranks the installed applications. Scoring is SearchScore, shared with every
// other source so one query cannot mean two different things.
void CollectApps(const std::wstring& lowQuery, std::vector<Hit>* out) {
    struct Ranked { int score; size_t index; };
    std::vector<Ranked> ranked;

    EnterCriticalSection(&g_lock);
    const size_t count = (std::min)(g_apps.size(), g_lowNames.size());
    ranked.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        int score = 0;
        if (!SearchScore(g_lowNames[i], lowQuery, &score)) continue;

        auto used = g_uses.find(g_lowNames[i]);
        const int uses = (used != g_uses.end()) ? used->second : 0;

        // Nothing typed yet. An empty query matches every application equally,
        // so ranking it produced whatever came first alphabetically - a list
        // headed "About Java, Access, Administrative Tools" that nobody has
        // ever wanted. Show the ones actually opened from here instead, and if
        // there are none yet show nothing and say so.
        if (lowQuery.empty() && uses <= 0) continue;

        if (uses > 0) score += (std::min)(30, uses * 6);   // frecency, capped
        // With no query the count is the only ordering there is, so it has to
        // carry further than the ceiling a real query wants.
        if (lowQuery.empty()) score += (std::min)(4000, uses);

        // Apps are what people come here for; a file has to be a clearly
        // better match before it outranks one.
        score += 10;
        ranked.push_back({ score, i });
    }

    const size_t keep = (std::min)(ranked.size(), (size_t)kMaxRows);
    std::partial_sort(ranked.begin(), ranked.begin() + (ptrdiff_t)keep, ranked.end(),
                      [](const Ranked& a, const Ranked& b) {
                          if (a.score != b.score) return a.score > b.score;
                          return g_lowNames[a.index] < g_lowNames[b.index];
                      });

    for (size_t i = 0; i < keep; ++i) {
        Hit hit;
        hit.kind   = HitKind::App;
        hit.name   = g_apps[ranked[i].index].name;
        hit.target = g_apps[ranked[i].index].launchPath;
        hit.score  = ranked[i].score;
        out->push_back(std::move(hit));
    }
    LeaveCriticalSection(&g_lock);
}

// Does the query look like something to hand to the shell rather than search
// for? A path, a URL, or anything with a switch or an extension in it.
bool LooksRunnable(const std::wstring& query) {
    if (query.empty()) return false;
    if (query.find(L'\\') != std::wstring::npos) return true;
    if (query.find(L'/') != std::wstring::npos) return true;
    if (query.find(L':') != std::wstring::npos) return true;   // http:, C:, ms-settings:
    if (query.find(L'.') != std::wstring::npos) return true;
    return false;
}

void Refilter() {
    g_hits.clear();
    const std::wstring lowQuery = ToLower(g_query);
    const Config* cfg = g_cfg;

    std::vector<Hit> pool;
    pool.reserve(kMaxRows * 3);

    CollectApps(lowQuery, &pool);

    // Each source is capped before the merge, so one prolific source - the file
    // index, in practice - cannot crowd everything else off the list.
    if (cfg && cfg->searchSettings)
        SearchSettingsPages(lowQuery, kMaxRows, &pool);
    if (cfg && cfg->searchFiles) {
        // Programs are capped apart from documents. Both come out of the same
        // index, and a query matching a folder full of notes would otherwise
        // fill the list before the executable you were reaching for was
        // considered at all.
        if (cfg->searchPrograms) SearchPrograms(lowQuery, kMaxRows, &pool);
        SearchFiles(lowQuery, kMaxRows, &pool);
    }

    // An application and the executable behind it are the same row twice:
    // "Excel" from the Start menu and "EXCEL" out of Program Files. The
    // catalogue entry is the better of the two - it has the display name the
    // Start menu uses - so the program yields to it.
    if (!pool.empty()) {
        std::vector<std::wstring> appNames;
        for (const Hit& hit : pool)
            if (hit.kind == HitKind::App) appNames.push_back(ToLower(hit.name));

        if (!appNames.empty()) {
            pool.erase(std::remove_if(pool.begin(), pool.end(),
                [&](const Hit& hit) {
                    if (hit.kind != HitKind::Program) return false;
                    const std::wstring low = ToLower(hit.name);
                    return std::find(appNames.begin(), appNames.end(), low) !=
                           appNames.end();
                }), pool.end());
        }
    }

    std::stable_sort(pool.begin(), pool.end(),
                     [](const Hit& a, const Hit& b) { return a.score > b.score; });

    // The calculator goes straight to the top when it fires at all: if what you
    // typed is a sum, the answer is the only thing you wanted.
    std::wstring answer;
    if (cfg && cfg->searchCalc && SearchCalc(g_query, &answer)) {
        Hit hit;
        hit.kind   = HitKind::Calc;
        hit.name   = answer;
        hit.detail = g_query + L"  -  Enter copies the answer";
        hit.target = answer;
        pool.insert(pool.begin(), std::move(hit));
    }

    if ((int)pool.size() > kMaxRows) pool.resize(kMaxRows);

    // "Run what I typed" is a fallback, so it takes the last slot rather than
    // competing for a good one - unless nothing else matched at all, in which
    // case it is the only useful thing on screen.
    if (cfg && cfg->searchCommands && !g_query.empty() &&
        (pool.empty() || LooksRunnable(g_query))) {
        Hit hit;
        hit.kind   = HitKind::Command;
        hit.name   = g_query;
        hit.detail = L"Run this command";
        hit.target = g_query;
        if ((int)pool.size() >= kMaxRows) pool.pop_back();
        pool.push_back(std::move(hit));
    }

    g_hits = std::move(pool);
    g_selected = 0;
}

// ------------------------------------------------------------------ geometry
int Rows() { return (int)g_hits.size(); }

SIZE WindowSize() {
    SIZE s;
    s.cx = (int)(kWidth * g_scale);
    if (Rows() == 0) {
        s.cy = (int)(kInputH * g_scale) + (int)(kPad * g_scale);
        return s;
    }
    s.cy = (int)((kInputH + kPad) * g_scale) + Rows() * (int)(kRowH * g_scale) +
           (int)(kFooterH * g_scale);
    return s;
}

void Reposition() {
    if (!g_wnd) return;

    // On the monitor the mouse is on, a third of the way down: high enough to
    // read without the eye having to travel to a corner.
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) return;

    const UINT dpi = DpiForWindow(g_wnd);
    g_scale = dpi ? (float)dpi / 96.0f : 1.0f;
    // The launcher is a plain popup, not a dialog, so nothing else points the
    // theme at its monitor. Without this its rows were laid out at the right
    // scale and then drawn with fonts built for a different one.
    theme::SetDpi(dpi);

    const SIZE size = WindowSize();
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - size.cx) / 2;
    const int y = mi.rcWork.top + (int)((mi.rcWork.bottom - mi.rcWork.top) * 0.22);
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE);
}

// The word for a kind of result, shown dim at the right of its row. It is
// there to say what Enter is about to do: "Run" and "Open" are not the same
// promise, and a settings page and a folder called the same thing are told
// apart by this and nothing else.
const wchar_t* KindTag(HitKind kind) {
    switch (kind) {
        case HitKind::Program: return L"program";
        case HitKind::Folder:  return L"folder";
        case HitKind::File:    return L"file";
        case HitKind::Setting: return L"settings";
        case HitKind::Calc:    return L"answer";
        case HitKind::Command: return L"run";
        default:               return nullptr;   // an app; its icon says so
    }
}

// Only these are backed by something the shell can draw an icon for. A sum and
// a command line are not, and asking would be one failed shell round trip per
// keystroke for a result that keeps its glyph either way.
bool WantsIcon(HitKind kind) {
    return kind == HitKind::App || kind == HitKind::Program ||
           kind == HitKind::File || kind == HitKind::Folder;
}

// ------------------------------------------------------------------ painting
void PaintInto(HDC dc, const SIZE& size) {
    using namespace Gdiplus;

    const float s = g_scale;
    RECT all = { 0, 0, size.cx, size.cy };
    FillRect(dc, &all, theme::BrushBg());
    theme::RoundRect(dc, all, (int)(12 * s), theme::Panel, theme::Border, true);

    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    const int pad    = (int)(kPad * s);
    const int inputH = (int)(kInputH * s);

    // ---- the query line ----
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, theme::FontTitle());

    RECT prompt = { pad + (int)(4 * s), 0, pad + (int)(26 * s), inputH };
    SetTextColor(dc, theme::Accent);
    DrawTextW(dc, L">", -1, &prompt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT text = { pad + (int)(28 * s), 0, size.cx - pad, inputH };
    if (g_query.empty()) {
        SetTextColor(dc, theme::TextDim);
        DrawTextW(dc, L"Search for an app...", -1, &text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        SetTextColor(dc, theme::Text);
        DrawTextW(dc, g_query.c_str(), -1, &text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (g_caretOn) {
            RECT measure = text;
            DrawTextW(dc, g_query.c_str(), -1, &measure,
                      DT_LEFT | DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
            const int caretX = text.left + (measure.right - measure.left) + (int)(2 * s);
            RECT caret = { caretX, inputH / 2 - (int)(10 * s),
                           caretX + (int)(2 * s), inputH / 2 + (int)(10 * s) };
            theme::FillRoundBar(dc, caret, 1, theme::Accent, 235);
        }
    }
    SelectObject(dc, oldFont);

    // ---- the separator, only when there is something below it ----
    if (Rows() > 0) {
        RECT line = { pad, inputH - 1, size.cx - pad, inputH };
        theme::FillRoundBar(dc, line, 0, theme::Border, 200);
    } else {
        EnterCriticalSection(&g_lock);
        const bool appsReady = g_appsReady;
        LeaveCriticalSection(&g_lock);
        const bool indexing = g_cfg && g_cfg->searchFiles && !SearchIndexReady();

        const wchar_t* message =
            !appsReady      ? L"Finding your apps..." :
            g_query.empty() ? L"Type to search your apps, files and settings "
                              L"- or type a sum." :
            indexing        ? L"No match yet - still indexing your files..." :
                              L"No match";

        RECT empty = { pad + (int)(28 * s), inputH, size.cx - pad, size.cy };
        SetTextColor(dc, theme::TextDim);
        oldFont = SelectObject(dc, theme::FontUI());
        DrawTextW(dc, message, -1, &empty,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
    }

    // ---- results ----
    const int rowH = (int)(kRowH * s);
    for (int i = 0; i < Rows(); ++i) {
        const int top = inputH + (int)(pad * 0.5f) + i * rowH;
        RECT row = { pad, top, size.cx - pad, top + rowH - (int)(4 * s) };
        const bool active = (i == g_selected);
        const Hit& hit = g_hits[(size_t)i];
        const std::wstring& name = hit.name;

        if (active) {
            theme::RoundRect(dc, row, (int)(8 * s), theme::RowSel, 0, false);
            // A rail rather than an outline. An outline around a row this wide
            // draws the eye to the border instead of to the row, and it fights
            // with the rounded corners of the panel behind it.
            RECT rail = { row.left + (int)(2 * s), row.top + (int)(6 * s),
                          row.left + (int)(5 * s), row.bottom - (int)(6 * s) };
            theme::FillRoundBar(dc, rail, (int)(2 * s), theme::Accent, 255);
        }

        // The icon the shell would show for this entry, if it has one and has
        // had time to fetch it. Until then, the lettered tile - which is what
        // this always drew and is still what tells a list of applications
        // apart at a glance.
        const int tile = (int)(kIconPx * s);
        const int tileY = row.top + ((row.bottom - row.top) - tile) / 2;
        RECT badge = { row.left + (int)(12 * s), tileY,
                       row.left + (int)(12 * s) + tile, tileY + tile };

        HBITMAP icon = WantsIcon(hit.kind) ? AppIconFor(hit.target, tile) : nullptr;
        if (icon) {
            AppIconDraw(dc, icon, badge);
        } else {
            theme::RoundRect(dc, badge, (int)(7 * s),
                             active ? theme::Accent : theme::PanelAlt,
                             active ? theme::Accent : theme::Border, true);

            // The badge says what kind of thing this is at a glance: a symbol
            // for the sources that are not apps, and for apps the initial
            // letter, which is what tells one row of a list of apps from the
            // next. All BMP characters - a wchar_t holds no more than that,
            // and a surrogate pair is not worth it for a 28px tile.
            wchar_t glyph[2] = { L'?', 0 };
            switch (hit.kind) {
                case HitKind::Folder:  glyph[0] = L'\x25B8'; break;   // ▸
                case HitKind::File:    glyph[0] = L'\x25A4'; break;   // ▤
                case HitKind::Setting: glyph[0] = L'\x2699'; break;   // ⚙
                case HitKind::Calc:    glyph[0] = L'=';      break;
                case HitKind::Command: glyph[0] = L'>';      break;
                case HitKind::Program:
                case HitKind::App:
                default:
                    glyph[0] = name.empty() ? L'?' : (wchar_t)towupper(name[0]);
                    break;
            }

            SetTextColor(dc, active ? RGB(255, 255, 255) : theme::TextDim);
            oldFont = SelectObject(dc, theme::FontBold());
            DrawTextW(dc, glyph, -1, &badge,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, oldFont);
        }

        // The kind, dim, on the right - but only where it says something. An
        // "app" tag on every row of a list of applications is eight repetitions
        // of what the icon has already made obvious.
        const wchar_t* tag = KindTag(hit.kind);
        RECT tagBox = { row.right - (int)(12 * s), row.top,
                        row.right - (int)(12 * s), row.bottom };
        if (tag) {
            oldFont = SelectObject(dc, theme::FontSmall());
            RECT tagFit = { 0, 0, 0, 0 };
            DrawTextW(dc, tag, -1, &tagFit, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
            tagBox.left -= (tagFit.right - tagFit.left);
            SetTextColor(dc, active ? RGB(178, 196, 232) : RGB(112, 118, 130));
            DrawTextW(dc, tag, -1, &tagBox,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, oldFont);
        }

        const int labelLeft  = badge.right + (int)(13 * s);
        const int labelRight = tagBox.left - (int)(12 * s);
        if (labelRight <= labelLeft) continue;

        if (hit.detail.empty()) {
            RECT label = { labelLeft, row.top, labelRight, row.bottom };
            SetTextColor(dc, active ? theme::Text : RGB(206, 210, 218));
            oldFont = SelectObject(dc, active ? theme::FontBold() : theme::FontUI());
            DrawTextW(dc, name.c_str(), -1, &label, DT_LEFT | DT_VCENTER |
                      DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, oldFont);
        } else {
            // Two lines: the name, and beneath it the folder it lives in or
            // what pressing Enter will do. Split the row rather than centring.
            const int mid = (row.top + row.bottom) / 2;
            RECT label = { labelLeft, row.top + (int)(3 * s), labelRight, mid + (int)(1 * s) };
            SetTextColor(dc, active ? theme::Text : RGB(206, 210, 218));
            oldFont = SelectObject(dc, active ? theme::FontBold() : theme::FontUI());
            DrawTextW(dc, name.c_str(), -1, &label, DT_LEFT | DT_BOTTOM |
                      DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, oldFont);

            RECT sub = { labelLeft, mid, labelRight, row.bottom - (int)(2 * s) };
            SetTextColor(dc, theme::TextDim);
            oldFont = SelectObject(dc, theme::FontSmall());
            // Ellipsise a long path from the *left*: the end of it is the part
            // that tells you which of three "notes.txt" this one is.
            DrawTextW(dc, hit.detail.c_str(), -1, &sub, DT_LEFT | DT_TOP |
                      DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, oldFont);
        }
    }

    // ---- the key hints ----
    // Every one of these was already bound and none of them was discoverable.
    // The bar is the one place somebody is looking when they would want to
    // know, and it costs a line of dim text.
    if (Rows() > 0) {
        const int footerTop = size.cy - (int)(kFooterH * s);
        RECT line = { pad + (int)(4 * s), footerTop, size.cx - pad - (int)(4 * s),
                      footerTop + 1 };
        theme::FillRoundBar(dc, line, 0, theme::Border, 130);

        RECT hint = { pad + (int)(14 * s), footerTop, size.cx - pad - (int)(14 * s),
                      size.cy };
        SetTextColor(dc, RGB(104, 110, 122));
        oldFont = SelectObject(dc, theme::FontSmall());
        DrawTextW(dc, L"↑↓ select      Enter open      "
                      L"Ctrl+Shift+Enter as administrator      Esc close",
                  -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
    }
}

void Paint(HWND wnd) {
    PAINTSTRUCT ps;
    HDC target = BeginPaint(wnd, &ps);

    RECT client;
    GetClientRect(wnd, &client);
    const SIZE size = { client.right, client.bottom };

    // Double buffered: the panel is redrawn on every keystroke.
    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(target, size.cx, size.cy) : nullptr;
    if (mem && bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintInto(mem, size);
        BitBlt(target, 0, 0, size.cx, size.cy, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
    } else {
        PaintInto(target, size);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);

    EndPaint(wnd, &ps);
}

void Relayout() {
    Refilter();
    Reposition();
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

// Store and other packaged apps are reached through a shell moniker, not a
// file. There is nothing on disk to elevate or to show in Explorer.
bool IsPackaged(const std::wstring& path) {
    static const wchar_t kPrefix[] = L"shell:AppsFolder\\";
    const size_t n = ARRAYSIZE(kPrefix) - 1;
    return path.size() >= n && _wcsnicmp(path.c_str(), kPrefix, n) == 0;
}

// What a Start-menu .lnk actually points at. Empty for anything that is not a
// resolvable shortcut - including packaged apps, which have no target file.
std::wstring ResolveShortcut(const std::wstring& path) {
    if (path.size() < 5) return L"";
    if (_wcsicmp(path.c_str() + path.size() - 4, L".lnk") != 0) return L"";

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))) || !link)
        return L"";

    std::wstring resolved;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && file) {
        if (SUCCEEDED(file->Load(path.c_str(), STGM_READ))) {
            wchar_t target[MAX_PATH * 2] = {};
            // SLGP_RAWPATH, and no Resolve() call: chasing a stale shortcut can
            // put up a dialog or hit the network, and this runs on the UI thread.
            if (SUCCEEDED(link->GetPath(target, (int)ARRAYSIZE(target), nullptr,
                                        SLGP_RAWPATH)) && target[0]) {
                wchar_t expanded[MAX_PATH * 2] = {};
                resolved = ExpandEnvironmentStringsW(target, expanded,
                                                     (DWORD)ARRAYSIZE(expanded))
                           ? expanded : target;
            }
        }
        file->Release();
    }
    link->Release();
    return resolved;
}

void NoteUse(const std::wstring& name) {
    g_uses[ToLower(name)] += 1;
    SaveUsage();
}

void CopyToClipboard(const std::wstring& text) {
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            // The clipboard owns the block on success; on failure we still do.
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

void RunElevated(const std::wstring& path) {
    // Elevate the executable itself where we can find it: "runas" on a .lnk
    // elevates Explorer's handling of the shortcut, not reliably the target.
    const std::wstring target = ResolveShortcut(path);

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask  = SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = target.empty() ? path.c_str() : target.c_str();
    info.nShow  = SW_SHOWNORMAL;
    // A refused UAC prompt comes back as ERROR_CANCELLED; that is the user
    // saying no, not something to complain about.
    ShellExecuteExW(&info);
}

void RevealInExplorer(const std::wstring& path) {
    const std::wstring target = ResolveShortcut(path);
    const std::wstring& show = target.empty() ? path : target;
    // Quoted: half of these live under "Program Files".
    const std::wstring args = L"/select,\"" + show + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

// Only apps carry a usage count. Ranking a file or a settings page by how
// often it was opened would need a second store keyed on path, and the file
// index is rebuilt from scratch each run anyway.
bool Ranked(HitKind kind) { return kind == HitKind::App; }

void LaunchSelected() {
    if (g_selected < 0 || g_selected >= Rows()) return;

    const Hit hit = g_hits[(size_t)g_selected];   // copied: hiding can refilter
    LauncherHide();

    switch (hit.kind) {
        case HitKind::Calc:
            // There is nothing to open - the answer is the result, so put it
            // where it can be pasted.
            CopyToClipboard(hit.target);
            return;

        case HitKind::Command:
            // Straight to the shell, the same path the launch bindings use, so
            // arguments and quoting behave identically.
            if (!LaunchCommand(hit.target))
                AWA_LOG(L"search: could not run '%s'", hit.target.c_str());
            return;

        default:
            if (Ranked(hit.kind)) NoteUse(hit.name);
            ShellExecuteW(nullptr, L"open", hit.target.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
            return;
    }
}

// A row is backed by a real file when it is an indexed file or folder, or an
// app reached through a shortcut rather than a shell moniker. Those are the
// rows that can be elevated, revealed in Explorer, or have a path copied.
bool HasFile(const Hit& hit) {
    switch (hit.kind) {
        case HitKind::App:     return !IsPackaged(hit.target);
        case HitKind::Program:
        case HitKind::File:
        case HitKind::Folder:  return true;
        default:               return false;   // Setting, Calc, Command
    }
}

void RunRowCommand(int cmd) {
    if (g_selected < 0 || g_selected >= Rows()) return;
    const Hit hit = g_hits[(size_t)g_selected];   // copied: IDM_FORGET refilters
    const bool file = HasFile(hit);

    switch (cmd) {
        case IDM_OPEN:
            LaunchSelected();
            break;
        // The file-bound commands are greyed out in the menu for rows with no
        // file behind them; the keyboard route has to agree, or
        // Ctrl+Shift+Enter on a Store app would quietly do nothing at all.
        case IDM_RUNAS:
            if (!file) return;
            LauncherHide();
            if (Ranked(hit.kind)) NoteUse(hit.name);
            RunElevated(hit.target);
            break;
        case IDM_LOCATION:
            if (!file) return;
            LauncherHide();
            RevealInExplorer(hit.target);
            break;
        case IDM_COPYPATH: {
            // For an app that is a shortcut, the useful path is what it points
            // at. For an indexed file it is already the real thing.
            const std::wstring resolved = ResolveShortcut(hit.target);
            CopyToClipboard(resolved.empty() ? hit.target : resolved);
            LauncherHide();
            break;
        }
        case IDM_FORGET:
            if (!Ranked(hit.kind)) return;
            g_uses.erase(ToLower(hit.name));
            SaveUsage();
            Relayout();          // it may well drop down the list now
            break;
        default:
            break;
    }
}

void ShowRowMenu(HWND wnd, POINT screen) {
    if (g_selected < 0 || g_selected >= Rows()) return;
    const Hit& hit = g_hits[(size_t)g_selected];
    const UINT fileOnly = HasFile(hit) ? 0 : MF_GRAYED;
    const UINT appOnly  = Ranked(hit.kind) ? 0 : MF_GRAYED;

    // The first line changes with the row, because "Open" is a poor description
    // of copying a sum or running a command line.
    const wchar_t* openLabel =
        hit.kind == HitKind::Calc    ? L"Copy the answer\tEnter" :
        hit.kind == HitKind::Command ? L"Run it\tEnter" :
        hit.kind == HitKind::Folder  ? L"Open the folder\tEnter" :
        hit.kind == HitKind::Setting ? L"Open the settings page\tEnter" :
        hit.kind == HitKind::Program ? L"Run the program\tEnter" :
                                       L"Open\tEnter";

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, IDM_OPEN, openLabel);
    SetMenuDefaultItem(menu, IDM_OPEN, FALSE);
    AppendMenuW(menu, MF_STRING | fileOnly, IDM_RUNAS, L"Run as administrator");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | fileOnly, IDM_LOCATION, L"Open file location");
    AppendMenuW(menu, MF_STRING | fileOnly, IDM_COPYPATH, L"Copy path");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | appOnly, IDM_FORGET, L"Forget this app");

    // The menu takes activation with it; without this the panel would treat
    // that as "the user clicked away" and vanish underneath its own menu.
    g_menuOpen = true;
    SetForegroundWindow(wnd);
    const int cmd = (int)TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        screen.x, screen.y, 0, wnd, nullptr);
    g_menuOpen = false;
    DestroyMenu(menu);

    if (cmd) {
        RunRowCommand(cmd);
    } else if (GetForegroundWindow() != wnd) {
        // Dismissed by clicking somewhere else entirely: that *was* a click away.
        LauncherHide();
    }
}

// Which result row a client-area y lands on, or -1.
int RowAt(int y) {
    const int inputH = (int)(kInputH * g_scale);
    const int rowH   = (int)(kRowH * g_scale);
    if (y <= inputH || rowH <= 0) return -1;
    const int row = (y - inputH - (int)(kPad * g_scale / 2)) / rowH;
    return (row >= 0 && row < Rows()) ? row : -1;
}

std::wstring ClipboardText() {
    if (!OpenClipboard(g_wnd)) return L"";
    std::wstring out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    // A pasted newline would be meaningless in a one-line query.
    for (auto& c : out) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    return out;
}

LRESULT CALLBACK LauncherProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_AWA_APPSREADY:            // the catalogue finished loading
            if (IsWindowVisible(wnd)) Relayout();
            return 0;

        case WM_AWA_ICONREADY:            // one more shell icon has arrived
            if (IsWindowVisible(wnd)) InvalidateRect(wnd, nullptr, FALSE);
            return 0;

        case WM_PAINT:
            Paint(wnd);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_TIMER:
            if (wp == kTimerCaret) {
                g_caretOn = !g_caretOn;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            return 0;

        case WM_ACTIVATE:
            // Clicking away is the same as pressing Escape - but the row menu
            // taking activation is not clicking away.
            if (LOWORD(wp) == WA_INACTIVE && !g_menuOpen) LauncherHide();
            return 0;

        case WM_KEYDOWN:
            switch (wp) {
                case VK_ESCAPE: LauncherHide(); return 0;
                case VK_RETURN:
                    // Ctrl+Shift+Enter elevates, the same gesture Windows
                    // Search uses, so nobody has to learn a new one.
                    if (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0)
                        RunRowCommand(IDM_RUNAS);
                    else
                        LaunchSelected();
                    return 0;
                case VK_APPS: {                      // the Menu key
                    if (Rows() <= 0) return 0;
                    RECT frame;                      // screen coordinates
                    GetWindowRect(wnd, &frame);
                    const int inputH = (int)(kInputH * g_scale);
                    const int rowH   = (int)(kRowH * g_scale);
                    // Roughly where a right-click on that row would have landed.
                    POINT at = { frame.left + (int)(kPad * g_scale) + rowH,
                                 frame.top + inputH + g_selected * rowH + rowH / 2 };
                    ShowRowMenu(wnd, at);
                    return 0;
                }
                case VK_UP:
                    if (Rows() > 0) {
                        g_selected = (g_selected + Rows() - 1) % Rows();
                        InvalidateRect(wnd, nullptr, FALSE);
                    }
                    return 0;
                case VK_DOWN:
                case VK_TAB:
                    if (Rows() > 0) {
                        g_selected = (g_selected + 1) % Rows();
                        InvalidateRect(wnd, nullptr, FALSE);
                    }
                    return 0;
                case VK_BACK:
                    if (!g_query.empty()) {
                        if (GetKeyState(VK_CONTROL) < 0) {
                            // Ctrl+Backspace drops the last word.
                            size_t end = g_query.find_last_not_of(L' ');
                            const size_t space = (end == std::wstring::npos)
                                ? std::wstring::npos : g_query.find_last_of(L' ', end);
                            g_query.erase(space == std::wstring::npos ? 0 : space + 1);
                        } else {
                            g_query.pop_back();
                        }
                        Relayout();
                    }
                    return 0;
                case 'V':
                    if (GetKeyState(VK_CONTROL) < 0) {
                        g_query += ClipboardText();
                        Relayout();
                        return 0;
                    }
                    break;
                case 'U':
                    if (GetKeyState(VK_CONTROL) < 0) {   // the readline habit
                        g_query.clear();
                        Relayout();
                        return 0;
                    }
                    break;
            }
            return 0;

        case WM_CHAR:
            // Control characters arrive here too; only printable text is query.
            if (wp >= 32 && wp != 127 && g_query.size() < 120) {
                g_query.push_back((wchar_t)wp);
                Relayout();
            }
            return 0;

        case WM_LBUTTONUP: {
            const int row = RowAt(GET_Y_LPARAM(lp));
            if (row >= 0) {
                g_selected = row;
                LaunchSelected();
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            const int row = RowAt(GET_Y_LPARAM(lp));
            if (row >= 0) {
                g_selected = row;
                InvalidateRect(wnd, nullptr, FALSE);
                UpdateWindow(wnd);          // show the selection before the menu
                POINT screen;
                GetCursorPos(&screen);
                ShowRowMenu(wnd, screen);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (g_mouseIdle) {
                POINT now;
                GetCursorPos(&now);
                if (abs(now.x - g_mouseAnchor.x) < 3 &&
                    abs(now.y - g_mouseAnchor.y) < 3)
                    return 0;
                g_mouseIdle = false;
            }
            const int row = RowAt(GET_Y_LPARAM(lp));
            if (row >= 0 && row != g_selected) {
                g_selected = row;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_DESTROY:
            g_wnd = nullptr;
            return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

// Builds the window if it is not there yet. Called from LauncherInit so the
// first chord has nothing left to do but show it.
bool EnsureWindow() {
    if (g_wnd) return true;
    if (!g_inst) return false;

    g_wnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"ProWindows launcher",
        WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, g_inst, nullptr);
    if (!g_wnd) return false;

    theme::DarkTitleBar(g_wnd);
    // Rounded to match the overlay and the rest of Windows 11. The SDK this
    // builds against predates the constant, hence winutil's own.
    SetCornerPreference(g_wnd, 2);
    return true;
}

} // namespace

// ---------------------------------------------------------------- public
void LauncherInit(HINSTANCE inst, Config* cfg) {
    g_inst = inst;
    g_cfg  = cfg;

    InitializeCriticalSection(&g_lock);
    g_lockReady = true;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = LauncherProc;
    wc.hInstance     = inst;
    wc.lpszClassName = kClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    LoadUsage();

    // The window is built here, not on the first chord. Creating a popup,
    // giving it a dark title bar and asking DWM to round its corners are three
    // round trips, and paying for them at the moment somebody presses the
    // shortcut is exactly when they are most noticeable. Built hidden, it costs
    // nothing until it is shown.
    //
    // Before StartLoad, because the scan posts to this window when it finishes
    // and a scan that beat it into existence would have nowhere to post to.
    EnsureWindow();
    AppIconInit(g_wnd, WM_AWA_ICONREADY);

    StartLoad();          // ready long before the first chord
}

void LauncherShutdown() {
    AppIconShutdown();
    if (g_wnd) { DestroyWindow(g_wnd); g_wnd = nullptr; }

    // Ask a scan that is still walking to stop where it is, so the wait below
    // is nearly always satisfied rather than timing out and leaving a live
    // thread to be terminated by ExitProcess.
    CancelLongScans();

    // Tell a scan still in flight to discard its results before we stop waiting
    // for it. Set under the lock so it is either seen or the write already
    // happened - never half of each.
    if (g_lockReady) {
        EnterCriticalSection(&g_lock);
        g_abandonLoad = true;
        LeaveCriticalSection(&g_lock);
    }

    if (g_loadThread) {
        // Enumerating shell:AppsFolder on a machine with a lot of Store apps is
        // not quick. If it still has not finished, abandon the handle: the
        // thread is harmless now, but freeing what it is standing on is not.
        if (WaitForSingleObject(g_loadThread, 5000) == WAIT_OBJECT_0)
            CloseHandle(g_loadThread);
        else
            AWA_LOG(L"launcher scan did not finish in time; abandoning it");
        g_loadThread = nullptr;
    }
    // g_lock is deliberately never deleted - an abandoned scan still enters it.
}

bool LauncherVisible() { return g_wnd && IsWindowVisible(g_wnd); }

void LauncherHide() {
    if (!g_wnd) return;
    KillTimer(g_wnd, kTimerCaret);
    ShowWindow(g_wnd, SW_HIDE);
}

void LauncherRefresh() { StartLoad(); }

void LauncherToggle() {
    if (LauncherVisible()) { LauncherHide(); return; }
    if (!EnsureWindow()) return;

    g_query.clear();
    g_selected = 0;
    g_caretOn = true;
    Relayout();

    GetCursorPos(&g_mouseAnchor);
    g_mouseIdle = true;

    ShowWindow(g_wnd, SW_SHOW);
    // A tray process has no foreground rights of its own, so a plain
    // SetForegroundWindow leaves the window drawn but not typed into.
    // FocusWindow already knows how to get past that.
    FocusWindow(g_wnd);
    // GetCaretBlinkTime returns INFINITE when the user has turned blinking off
    // in the accessibility settings, and SetTimer would clamp that to ~25 days.
    const UINT blink = GetCaretBlinkTime();
    SetTimer(g_wnd, kTimerCaret,
             (blink == 0 || blink == INFINITE) ? 530 : blink, nullptr);
}

} // namespace awa
