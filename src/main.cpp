// ProWindows - entry point, tray UI and event plumbing.
#include "common.h"
#include "config.h"
#include "wm.h"
#include "winutil.h"
#include "hotkeys.h"
#include "moddrag.h"
#include "app.h"
#include "settings.h"
#include "monitor.h"
#include "clock.h"
#include "timer.h"
#include "alarm.h"
#include "launcher.h"
#include "dragguide.h"
#include "search.h"
#include "appicon.h"
#include "theme.h"
#include "ipc.h"
#include "resource.h"
#include <commctrl.h>
#include <psapi.h>
#include <cstdlib>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "psapi.lib")

namespace awa {

// ---------------------------------------------------------------- menu ids
enum : UINT {
    IDM_TILING = 100, IDM_GAPS, IDM_RETILE, IDM_RELOAD, IDM_EDITCFG,
    IDM_OPENDIR, IDM_AUTOSTART, IDM_RESTOREALL, IDM_EXIT,
    IDM_SETTINGS, IDM_WELCOME, IDM_SHORTCUTS, IDM_MONITOR, IDM_MONITOR_PIN, IDM_ELEVATE,
    IDM_ELEVAUTO, IDM_DIAG, IDM_CLOCK, IDM_CLOCK_PIN, IDM_MONITOR_SETTINGS,
    IDM_CLOCK_SETTINGS, IDM_TIMER, IDM_TIMER_PIN, IDM_TIMER_STOP, IDM_TIMER_SETTINGS,
    IDM_ALARM_SNOOZE, IDM_ALARM_DISMISS,
    IDM_LAYOUT_BASE = 200,
    IDM_WORKSPACE_BASE = 300,
};

// ---------------------------------------------------------------- globals
static HINSTANCE       g_inst      = nullptr;
static HWND            g_wnd       = nullptr;
static Config          g_cfg;
static WindowManager   g_wm;
static NOTIFYICONDATAW g_nid       = {};
static bool            g_trayAdded = false;
static UINT            g_taskbarCreatedMsg = 0;
static std::vector<HWINEVENTHOOK> g_hooks;
// Focus-follows-mouse: a LOCATIONCHANGE hook that only cares about the cursor
// and arms a one-shot TIMER_MOUSE. Installed only while the feature is on and
// game mode is off. (Not a low-level mouse hook: that stalls the pointer.)
static HWINEVENTHOOK g_mouseHook = nullptr;
static bool g_mouseTimerPending = false;
static constexpr UINT kMouseSettleMs = 60;
static void ApplyFocusFollows();
static bool            g_shuttingDown = false;
static HPOWERNOTIFY    g_powerNotify  = nullptr;

// ---------------------------------------------------------------- autostart
static const wchar_t kRunKey[]   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunValue[] = L"ProWindows";

static void SetAutostart(bool on);
// Defined with the rest of the overlay's state, below; used from here up.
static void UpdateOverlayVisibility();
// The screen is off (or the machine has gone to sleep). Nobody can see the
// overlays, so nothing about them is worth spending anything on.
static bool g_displayOff = false;

// The command line currently registered to run at logon, or empty.
static std::wstring AutostartCommand() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf), type = 0;
    const bool found =
        RegQueryValueExW(key, kRunValue, nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!found || type != REG_SZ) return L"";
    return buf;
}

static bool AutostartEnabled() { return !AutostartCommand().empty(); }

// Autostart records an absolute path. Copy the folder to another machine, move
// it to another drive, or unpack a new release beside the old one, and that
// path still names wherever the executable used to be - so the entry is there,
// looks enabled in the settings window, and silently starts nothing. Since we
// are the program it is supposed to be starting, we are also the only thing in
// a position to notice, so put it right on the way past.
static void RepairAutostartPath() {
    const std::wstring have = AutostartCommand();
    if (have.empty()) return;                    // not enabled; nothing to fix

    const std::wstring want = L"\"" + ExePath() + L"\" --tray";
    if (_wcsicmp(have.c_str(), want.c_str()) == 0) return;

    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)want.c_str(),
                   (DWORD)((want.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    AWA_LOG(L"autostart path repaired: %s", want.c_str());
}

// The Run entry was written under the old name and points at an executable
// that no longer exists. Clear it, and carry the user's choice over.
static void MigrateLegacyAutostart() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ | KEY_SET_VALUE, &key)
            != ERROR_SUCCESS)
        return;

    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf), type = 0;
    const bool had = RegQueryValueExW(key, L"AutoWindowsArrange", nullptr, &type,
                                      (LPBYTE)buf, &size) == ERROR_SUCCESS;
    if (had) RegDeleteValueW(key, L"AutoWindowsArrange");
    RegCloseKey(key);

    if (had && !AutostartEnabled()) SetAutostart(true);
}

static void SetAutostart(bool on) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\" --tray";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

// ---------------------------------------------------------------- tray
// The tooltip is NOTIFYICONDATAW::szTip, which is 128 characters. This used
// to be built with swprintf_s, which does not truncate: handed a string that
// does not fit it calls the invalid-parameter handler, and with no handler
// installed that is a fast-fail - the process is gone, tray icon and all,
// with nothing in the log. The one combination that did not fit was game mode
// ("paused for fullscreen app") together with at least one window running as
// administrator: 137 characters. So the tiler died at exactly the moment a
// game started while Task Manager, an installer or an elevated launcher was
// open, and the report read "it works at first and then just stops". Two
// Watson buckets on this machine say so. Truncate, and say less.
static void TrayTooltip(wchar_t* out, size_t cch) {
    const wchar_t* state = g_wm.GameMode()   ? L"paused: fullscreen app"
                         : g_wm.TilingEnabled() ? L"tiling on"
                                                : L"tiling paused";
    const int blocked = g_wm.BlockedCount();
    if (blocked > 0) {
        _snwprintf_s(out, cch, _TRUNCATE,
                     L"%s %s\nLayout: %s  |  Workspace %d  |  %s\n"
                     L"%d window%s need%s administrator rights",
                     kAppName, kVersion, LayoutName(g_wm.ActiveLayout()),
                     g_wm.ActiveWorkspace() + 1, state,
                     blocked, blocked == 1 ? L"" : L"s", blocked == 1 ? L"s" : L"");
    } else {
        _snwprintf_s(out, cch, _TRUNCATE, L"%s %s\nLayout: %s  |  Workspace %d  |  %s",
                     kAppName, kVersion, LayoutName(g_wm.ActiveLayout()),
                     g_wm.ActiveWorkspace() + 1, state);
    }
}

// Holds a notice raised before the tray icon exists. The first window scan
// happens during startup, and it is exactly that scan which discovers windows
// running as administrator - so the one message the user most needs to see was
// also the one guaranteed to be thrown away.
static std::wstring g_pendingBalloon;
static void TrayBalloon(const wchar_t* title, const wchar_t* text);

static void TrayAdd() {
    g_nid = {};
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_wnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_AWA_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    TrayTooltip(g_nid.szTip, ARRAYSIZE(g_nid.szTip));
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;

    if (g_trayAdded && !g_pendingBalloon.empty()) {
        const std::wstring held = std::move(g_pendingBalloon);
        g_pendingBalloon.clear();
        TrayBalloon(kAppName, held.c_str());
    }
}

static void TrayUpdate() {
    if (!g_trayAdded) return;
    g_nid.uFlags = NIF_TIP;
    TrayTooltip(g_nid.szTip, ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void TrayRemove() {
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = false;
}

static void TrayBalloon(const wchar_t* title, const wchar_t* text) {
    if (!g_trayAdded) {
        if (g_pendingBalloon.empty() && text) g_pendingBalloon = text;
        return;
    }
    NOTIFYICONDATAW n = {};
    n.cbSize = sizeof(n);
    n.hWnd   = g_wnd;
    n.uID    = 1;
    n.uFlags = NIF_INFO;
    // Truncate rather than overflow: the balloon carries user-supplied text
    // (a launch command), and the _s variants abort the process on overflow.
    wcsncpy_s(n.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(n.szInfo, text, _TRUNCATE);
    n.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ---------------------------------------------------------------- actions
static void RunAction(const Keybind& kb) {
    switch (kb.action) {
        case ACT_FOCUS_DIR:         g_wm.ActFocusDir((Dir)kb.arg); break;
        case ACT_SWAP_DIR:          g_wm.ActSwapDir((Dir)kb.arg); break;
        case ACT_RESIZE_DIR:        g_wm.ActResizeDir((Dir)kb.arg); break;
        case ACT_FOCUS_NEXT:        g_wm.ActFocusCycle(+1); break;
        case ACT_FOCUS_PREV:        g_wm.ActFocusCycle(-1); break;
        case ACT_WORKSPACE:         g_wm.ActSwitchWorkspace(kb.arg); break;
        case ACT_MOVE_TO_WORKSPACE: g_wm.ActMoveToWorkspace(kb.arg); break;
        case ACT_CYCLE_LAYOUT:      g_wm.ActCycleLayout(); break;
        case ACT_SET_LAYOUT:        g_wm.ActSetLayout((LayoutKind)kb.arg); break;
        case ACT_TOGGLE_FLOAT:      g_wm.ActToggleFloat(); break;
        case ACT_TOGGLE_FULLSCREEN: g_wm.ActToggleFullscreen(); break;
        case ACT_CLOSE_WINDOW:      g_wm.ActCloseFocused(); break;
        case ACT_MINIMIZE:          g_wm.ActMinimizeFocused(); break;
        case ACT_PROMOTE:           g_wm.ActPromote(); break;
        case ACT_TOGGLE_SPLIT:      g_wm.ActToggleSplit(); break;
        case ACT_SWAP_SPLIT:        g_wm.ActSwapSplit(); break;
        case ACT_TOGGLE_STICKY:     g_wm.ActToggleSticky(); break;
        case ACT_SCRATCHPAD_MOVE:   g_wm.ActScratchpadMove(); break;
        case ACT_SCRATCHPAD_TOGGLE: g_wm.ActScratchpadToggle(); break;
        case ACT_FOCUS_LAST:        g_wm.ActFocusLast(); break;
        case ACT_WORKSPACE_REL:     g_wm.ActWorkspaceRelative(kb.arg, false); break;
        case ACT_WORKSPACE_USED:    g_wm.ActWorkspaceRelative(kb.arg, true); break;
        case ACT_TOGGLE_TILING:     g_wm.ActToggleTiling(); break;
        case ACT_TOGGLE_GAPS:       g_wm.ActToggleGaps(); break;
        case ACT_LAUNCHER:          LauncherToggle(); return;
        case ACT_TIMER:             TimerToggle(); return;
        case ACT_FOCUS_MONITOR:     g_wm.ActFocusMonitor(kb.arg); break;
        case ACT_MOVE_TO_MONITOR:   g_wm.ActMoveToMonitor(kb.arg); break;
        case ACT_RETILE:            AppRetileNow(); break;
        case ACT_RELOAD_CONFIG:     PostMessageW(g_wnd, WM_COMMAND, IDM_RELOAD, 0); return;
        case ACT_QUIT:              PostMessageW(g_wnd, WM_COMMAND, IDM_EXIT, 0); return;
        case ACT_LAUNCH:
            if (!LaunchCommand(kb.command)) {
                const std::wstring text = L"Could not open:\n" + kb.command;
                TrayBalloon(kAppName, text.c_str());
            }
            return;
        default: return;
    }
    TrayUpdate();
    SettingsRefreshStatus();
}

// ---------------------------------------------------------------- control channel
// Turns one `--msg` command into its reply. Runs on the UI thread, posted from
// the pipe thread - see ipc.h for why it is posted and not sent.
namespace {

std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            default:
                if (c < 0x20) {
                    wchar_t esc[8];
                    swprintf_s(esc, L"\\u%04x", (unsigned)c);
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::wstring JsonStr(const std::wstring& s) { return L"\"" + JsonEscape(s) + L"\""; }
std::wstring JsonBool(bool b) { return b ? L"true" : L"false"; }
std::wstring JsonNum(long long v) { return std::to_wstring(v); }

std::wstring JsonRect(const Rect& r) {
    return L"{\"x\":" + JsonNum(r.x) + L",\"y\":" + JsonNum(r.y) +
           L",\"w\":" + JsonNum(r.w) + L",\"h\":" + JsonNum(r.h) + L"}";
}

std::wstring JsonWindows(const WindowManager::Snapshot& s) {
    std::wstring out = L"[";
    for (size_t i = 0; i < s.windows.size(); ++i) {
        const auto& w = s.windows[i];
        if (i) out += L",";
        out += L"{\"id\":" + JsonNum((long long)(uintptr_t)w.hwnd);
        out += L",\"title\":" + JsonStr(w.title);
        out += L",\"class\":" + JsonStr(w.cls);
        out += L",\"process\":" + JsonStr(w.proc);
        out += L",\"monitor\":" + JsonNum(w.monitor);
        out += L",\"workspace\":" + JsonNum(w.workspace + 1);
        out += L",\"floating\":" + JsonBool(w.floating);
        out += L",\"minimized\":" + JsonBool(w.minimized);
        out += L",\"hidden\":" + JsonBool(w.hidden);
        out += L",\"fullscreen\":" + JsonBool(w.fullscreen);
        out += L",\"immovable\":" + JsonBool(w.immovable);
        out += L",\"sticky\":" + JsonBool(w.sticky);
        out += L",\"scratchpad\":" + JsonBool(w.scratch);
        out += L",\"focused\":" + JsonBool(w.focused);
        out += L",\"rect\":" + JsonRect(w.rect);
        out += L"}";
    }
    return out + L"]";
}

std::wstring JsonWorkspaces(const WindowManager::Snapshot& s) {
    std::wstring out = L"[";
    for (size_t i = 0; i < s.workspaces.size(); ++i) {
        const auto& w = s.workspaces[i];
        if (i) out += L",";
        out += L"{\"number\":" + JsonNum(w.index + 1);
        out += L",\"monitor\":" + JsonNum(w.monitor);
        out += L",\"active\":" + JsonBool(w.active);
        out += L",\"windows\":" + JsonNum(w.windows);
        out += L",\"layout\":" + JsonStr(LayoutName(w.layout));
        out += L"}";
    }
    return out + L"]";
}

std::wstring JsonMonitors(const WindowManager::Snapshot& s) {
    std::wstring out = L"[";
    for (size_t i = 0; i < s.monitors.size(); ++i) {
        const auto& m = s.monitors[i];
        if (i) out += L",";
        out += L"{\"index\":" + JsonNum(m.index);
        out += L",\"primary\":" + JsonBool(m.primary);
        out += L",\"activeWorkspace\":" + JsonNum(m.activeWorkspace + 1);
        out += L",\"bounds\":" + JsonRect(m.full);
        out += L",\"workArea\":" + JsonRect(m.work);
        out += L"}";
    }
    return out + L"]";
}

// Where the memory is. Every heap in the process - the CRT's, GDI+'s, COM's
// - walked and summed, beside the process-wide counters, so "it uses 30 MB"
// can be answered with which part does. Diagnostics and `get memory`.
std::wstring MemoryReport() {
    std::wstring out;
    wchar_t line[256];

    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        swprintf_s(line, L"private commit %5.1f MB   working set %5.1f MB   peak %5.1f MB\r\n",
                   pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0,
                   pmc.PeakWorkingSetSize / 1048576.0);
        out += line;
    }
    swprintf_s(line, L"gdi objects %u   user objects %u\r\n",
               GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
               GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
    out += line;

    HANDLE heaps[64];
    const DWORD n = GetProcessHeaps((DWORD)ARRAYSIZE(heaps), heaps);
    SIZE_T totalUsed = 0, totalCommitted = 0;
    for (DWORD i = 0; i < n && i < ARRAYSIZE(heaps); ++i) {
        SIZE_T used = 0, committed = 0;
        if (!HeapLock(heaps[i])) continue;
        PROCESS_HEAP_ENTRY e = {};
        while (HeapWalk(heaps[i], &e)) {
            if (e.wFlags & PROCESS_HEAP_REGION) committed += e.Region.dwCommittedSize;
            else if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) used += e.cbData;
        }
        HeapUnlock(heaps[i]);
        totalUsed += used;
        totalCommitted += committed;
        if (committed >= 65536 || used >= 65536) {
            swprintf_s(line, L"heap %2u%s  used %6.2f MB  committed %6.2f MB\r\n", i,
                       heaps[i] == GetProcessHeap() ? L" (crt)" : L"      ",
                       used / 1048576.0, committed / 1048576.0);
            out += line;
        }
    }
    swprintf_s(line, L"all heaps: used %.2f MB, committed %.2f MB in %u heaps\r\n",
               totalUsed / 1048576.0, totalCommitted / 1048576.0, n);
    out += line;
    return out;
}

std::wstring IpcQuery(const std::wstring& what) {
    if (what == L"memory")  return MemoryReport();
    if (what == L"monitor") return MonitorReadingsText();
    if (what == L"version")
        return L"{\"name\":" + JsonStr(kAppName) + L",\"version\":" + JsonStr(kVersion) +
               L",\"elevated\":" + JsonBool(SelfIsElevated()) + L"}";

    const WindowManager::Snapshot s = g_wm.TakeSnapshot();
    if (what == L"windows")    return JsonWindows(s);
    if (what == L"workspaces") return JsonWorkspaces(s);
    if (what == L"monitors")   return JsonMonitors(s);
    if (what == L"state") {
        return L"{\"version\":" + JsonStr(kVersion) +
               L",\"tiling\":" + JsonBool(s.tiling) +
               L",\"gaps\":" + JsonBool(s.gaps) +
               L",\"gameMode\":" + JsonBool(s.gameMode) +
               L",\"displayOff\":" + JsonBool(g_displayOff) +
               L",\"monitorShown\":" + JsonBool(MonitorVisible()) +
               L",\"clockShown\":" + JsonBool(ClockVisible()) +
               L",\"blocked\":" + JsonNum(s.blocked) +
               L",\"activeMonitor\":" + JsonNum(s.activeMonitor) +
               L",\"activeWorkspace\":" + JsonNum(s.activeWorkspace + 1) +
               L",\"activeLayout\":" + JsonStr(LayoutName(s.activeLayout)) +
               L",\"monitors\":" + JsonMonitors(s) +
               L",\"workspaces\":" + JsonWorkspaces(s) +
               L",\"windows\":" + JsonWindows(s) + L"}";
    }
    return L"error: unknown query '" + what + L"'. Try: windows, workspaces, "
           L"monitors, state, version, memory, monitor";
}

std::wstring IpcCommandHandler(const std::wstring& line) {
    // Accept both "workspace 3" and the config file's own "workspace, 3", so
    // a line can be pasted straight from config.ini into --msg and back.
    std::wstring text = Trim(line);
    if (text.empty()) return IpcHelpText();
    if (text == L"help") return IpcHelpText();

    std::wstring verb = text, rest;
    const size_t cut = text.find_first_of(L" ,\t");
    if (cut != std::wstring::npos) {
        verb = Trim(text.substr(0, cut));
        rest = Trim(text.substr(cut + 1));
        if (!rest.empty() && rest[0] == L',') rest = Trim(rest.substr(1));
    }
    const std::wstring lower = ToLower(verb);

    if (lower == L"get") return IpcQuery(ToLower(rest));

    // `focus id <n>` is a control-channel command rather than a bindable
    // action: there is no useful keyboard shortcut for "focus window 918274",
    // but a script that has just read `get windows` wants exactly that.
    if (lower == L"focus") {
        std::wstring what = ToLower(rest);
        if (what.compare(0, 3, L"id ") == 0 || what.compare(0, 3, L"id	") == 0) {
            const std::wstring idText = Trim(rest.substr(3));
            const unsigned long long id = _wcstoui64(idText.c_str(), nullptr, 10);
            if (!id) return L"error: focus id needs a window id from 'get windows'";
            if (!g_wm.ActFocusWindowById((HWND)(uintptr_t)id))
                return L"error: no managed window with id " + idText;
            TrayUpdate();
            return L"ok";
        }
    }

    Keybind kb;
    if (!ParseAction(verb, rest, &kb.action, &kb.arg, &kb.command))
        return L"error: unknown command '" + text + L"'. Try: help";

    // Straight through the same dispatch the keyboard uses, so a command and a
    // shortcut cannot end up doing subtly different things.
    RunAction(kb);
    return L"ok";
}

} // namespace

// ---------------------------------------------------------------- config
static void LoadConfig() {
    const std::wstring path = ConfigPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        Config::WriteDefaultFile(path);

    g_cfg = Config();
    g_cfg.LoadDefaults();
    g_cfg.LoadFromFile(path);
    LogEnable(g_cfg.debug);
    AWA_LOG(L"config loaded (%d bindings)", (int)g_cfg.binds.size());
}

// Defined further down, with the rest of the diagnostics. Declared here so the
// app.h implementations below can hand the settings window the same code the
// tray menu runs rather than a second copy of it.
static void WriteDiagnostics();

// Puts whatever is in g_cfg into effect: keys, rules, overlays, search.
static void ApplyLiveConfig() {
    HotkeysUnregister();
    HotkeysRegister(g_wnd, &g_cfg);
    ModDragApplyConfig(&g_cfg);   // the gesture follows the modifier
    g_wm.ApplyConfigChanged();

    UpdateOverlayVisibility();
    MonitorApplyConfig();
    ClockApplyConfig();
    TimerApplyConfig();
    SearchApplyConfig();

    ApplyFocusFollows();

    TrayUpdate();
    SettingsRefresh();
}

static void ReloadConfig(bool announce) {
    LoadConfig();
    ApplyLiveConfig();
    if (announce) TrayBalloon(kAppName, L"Settings reloaded.");
}

// Saves g_cfg and puts it into effect. Applying used to save and then reload
// from disk, ignoring whether the save worked - so when config.ini could not
// be written (held open by an editor or a sync client), the reload read the
// old file back and every change the user had just applied quietly vanished.
// On a failed save the settings are applied from memory instead, and the
// caller is told they will not survive a restart.
static bool SaveAndApply() {
    if (g_cfg.SaveToFile(ConfigPath())) {
        ReloadConfig(false);
        return true;
    }
    LogEnable(g_cfg.debug);
    ApplyLiveConfig();
    return false;
}

// ---------------------------------------------------------------- app.h impl
Config&        AppConfig() { return g_cfg; }
WindowManager& AppWm()     { return g_wm; }

bool AppApplySettings() { return SaveAndApply(); }

void AppShowShortcuts()  { SettingsOpenTab(PAGE_SHORTCUTS); }
void AppUpdateTray()     { TrayUpdate(); }
bool AppAutostartEnabled()    { return AutostartEnabled(); }
void AppSetAutostart(bool on) { SetAutostart(on); }
int  AppHotkeyConflicts()     { return HotkeysBlockedCount(); }
int  AppManagedWindows()      { return g_wm.ManagedCount(); }
int  AppIndexEntries()        { return SearchIndexCount(); }
int  AppIconCacheCount()      { return AppIconCount(); }
int  AppSamplerCostTenths()   { return MonitorSampleCostTenths(); }
int  AppMemoryMB() {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc)) return 0;
    return (int)(pmc.PrivateUsage / (1024 * 1024));
}

void AppOpenConfigFile() {
    ShellExecuteW(nullptr, L"open", L"notepad.exe", ConfigPath().c_str(),
                  nullptr, SW_SHOWNORMAL);
}

void AppOpenConfigFolder() {
    ShellExecuteW(nullptr, L"open", ConfigDir().c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

void AppWriteDiagnostics()     { WriteDiagnostics(); }
void AppReloadFromDisk()       { ReloadConfig(true); }

void AppRestoreHiddenWindows() {
    g_wm.RestoreAllWindows();
    g_wm.RetileNow();
    TrayUpdate();
}

bool AppRestoreDefaults() {
    // Built from a fresh Config rather than by resetting fields on the live
    // one: a member added later is then defaulted by the compiler instead of
    // being quietly left at whatever the user had.
    g_cfg = Config();
    g_cfg.LoadDefaults();
    g_wm.ForgetLearnedLimits();
    return SaveAndApply();
}

std::wstring AppAboutText() {
    std::wstring text = std::wstring(kAppName) + L" " + kVersion;
    text += SelfIsElevated() ? L"  -  running as administrator"
                             : L"  -  running as a normal user";
    text += L"\r\n";

    wchar_t exe[MAX_PATH * 2] = {};
    if (GetModuleFileNameW(nullptr, exe, (DWORD)ARRAYSIZE(exe))) {
        text += exe;
        text += L"\r\n";
    }
    text += ConfigPath();

    const int blocked = g_wm.BlockedCount();
    if (blocked > 0) {
        text += L"\r\n";
        text += std::to_wstring(blocked);
        text += (blocked == 1)
            ? L" window runs as administrator and cannot be arranged from here."
            : L" windows run as administrator and cannot be arranged from here.";
    }
    return text;
}

void AppRetileNow() {
    g_wm.ScanExistingWindows();
    g_wm.RetileNow();
    TrayUpdate();
}

void AppTrayBalloon(const wchar_t* title, const wchar_t* text) {
    TrayBalloon(title, text);
}

// The overlays save on every drag, pin and skin change, and learned window
// sizes are saved as they are found; a failure is said once, not every time.
void AppSaveConfig() {
    static bool warned = false;
    if (g_cfg.SaveToFile(ConfigPath()) || warned) return;
    warned = true;
    TrayBalloon(kAppName, L"Could not save config.ini. Something may have it open; "
                          L"changes will be lost when ProWindows restarts.");
}

// Memory a tray application does not need to hold while it waits. Three
// things, each cheap and each honest about what it does:
//
//  - CoFreeUnusedLibraries: the shell's AppsFolder enumeration and the COM
//    objects behind it drag several large DLLs in (windows.storage,
//    StateRepository, CoreUI). Once the scan is over they have no objects
//    left and unload on request; they are loaded again the next time they
//    are needed.
//  - HeapCompact: returns freed pages at the end of the process heap to the
//    system, after a settings window or a result list has been freed.
//  - SetProcessWorkingSetSizeEx(-1, -1): moves every page the process is not
//    actively touching from its working set to the standby list. The pages
//    are not written anywhere - a page that is touched again comes back with
//    a soft fault, a few microseconds - but they no longer count against
//    the process, and Task Manager's memory column shows what the process
//    is actually using rather than everything it has ever touched.
//
// Never on the way to doing something: only after the windows have been
// put away and the timer has run out with nothing else having happened.
static constexpr UINT kTrimIdleMs     = 15 * 60 * 1000;   // and then again, every so often
static void TrimMemory() {
    // The default delay, not zero. A zero delay also unloads DLLs whose
    // objects live in the multithreaded apartment the instant they report
    // no objects left, and the thermal probe creates WMI objects from an
    // MTA thread of its own: a DLL could be unloaded between that thread
    // entering DllGetClassObject and the new object being counted. The shell
    // DLLs this call is here for were loaded from single-threaded apartments
    // and still go at once; the WMI ones follow on a later trim.
    CoFreeUnusedLibrariesEx(INFINITE, 0);
    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
    AWA_LOG(L"memory trimmed");
}

void AppScheduleTrim(UINT delayMs) {
    if (g_wnd) SetTimer(g_wnd, TIMER_TRIM, delayMs, nullptr);
}


// One place that decides whether the overlay exists at all, because three
// separate things now have an opinion about it and each of them used to set it
// directly - so whichever ran last won, and switching away from a game with
// the screen off brought the panel back to repaint at nobody.
static void UpdateOverlayVisibility() {
    const bool allowed = !g_wm.GameMode() && !g_displayOff;
    MonitorSetVisible(g_cfg.monitorEnabled && allowed);
    ClockSetVisible(g_cfg.clockEnabled && allowed);
    TimerSetVisible(g_cfg.timerShown && allowed);
}

// For the timer's alarm, which sets timerShown itself and needs the overlay
// rules (game mode, display off) applied to it.
void AppUpdateOverlays() { UpdateOverlayVisibility(); }

void AppGameModeChanged(bool on) {
    if (on) {
        // Everything of ours that wakes up on a timer, stopped for the
        // duration. The overlay in particular is a topmost layered window that
        // repaints several times a second and holds a PDH query open; tearing
        // it down costs a game nothing and gives it back a composition layer.
        ApplyFocusFollows();
        // Nothing else will tell us the game has gone.
        SetTimer(g_wnd, TIMER_GAMECHK, 1500, nullptr);
    } else {
        KillTimer(g_wnd, TIMER_GAMECHK);
        ApplyFocusFollows();
    }
    UpdateOverlayVisibility();
    TrayUpdate();
}

// GUID_CONSOLE_DISPLAY_STATE. Written out rather than included: the symbol
// lives behind INITGUID in one SDK header and is an extern in another, and
// this is sixteen bytes.
static const GUID kConsoleDisplayState =
    { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

// Milliseconds since the user last pressed or moved anything.
static ULONGLONG IdleMs() {
    LASTINPUTINFO lii = { sizeof(lii), 0 };
    if (!GetLastInputInfo(&lii)) return 0;
    return (ULONGLONG)(GetTickCount() - lii.dwTime);
}

// The console-display-state notification is the only thing that says the
// screen has gone dark, and it is not always right: on this machine the
// notification delivered at registration reported the display as off while
// it was being looked at, and both overlays stayed hidden - sampler parked,
// clock stopped - for the rest of the session, with nothing in the settings
// to explain it. A display does not switch itself off under a hand on the
// mouse, so "off" is only believed after a stretch of no input, and while it
// is believed a slow timer watches for input and clears it, in case the
// matching "on" never arrives either.
static constexpr ULONGLONG kDisplayOffNeedsIdleMs = 45 * 1000;
static void SetDisplayOff(bool off) {
    if (off && IdleMs() < kDisplayOffNeedsIdleMs) {
        AWA_LOG(L"display reported off with input %llu ms ago - not believed",
                IdleMs());
        return;
    }
    if (g_displayOff == off) return;
    g_displayOff = off;
    AWA_LOG(L"display %s", off ? L"off - suspending the overlays" : L"on");
    if (off) SetTimer(g_wnd, TIMER_DISPLAY, 5 * 1000, nullptr);
    else     KillTimer(g_wnd, TIMER_DISPLAY);
    UpdateOverlayVisibility();
    // Modern standby can end without a resume broadcast: what came due meanwhile fires now.
    if (!off) TimerCheckNow();
}
void AppRefreshSettings() { SettingsRefresh(); }
void AppOpenMonitorSettings() { SettingsOpenTab(PAGE_MONITOR); }
void AppOpenClockSettings()   { SettingsOpenTab(PAGE_CLOCK); }

// ---------------------------------------------------------------- win events
// Everything but the cursor is discarded at once; the cursor arms one timer if
// none is pending. Never blocks, never touches a window.
static void CALLBACK MouseEventProc(HWINEVENTHOOK, DWORD, HWND, LONG idObject,
                                    LONG, DWORD, DWORD) {
    if (idObject != OBJID_CURSOR) return;
    if (g_shuttingDown || g_mouseTimerPending || !g_wnd) return;
    g_mouseTimerPending = true;
    SetTimer(g_wnd, TIMER_MOUSE, kMouseSettleMs, nullptr);
}

static void ApplyFocusFollows() {
    const bool want = g_cfg.focusFollowsMouse && !g_wm.GameMode();
    if (want && !g_mouseHook) {
        g_mouseHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                      nullptr, MouseEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    } else if (!want) {
        if (g_mouseHook) { UnhookWinEvent(g_mouseHook); g_mouseHook = nullptr; }
        if (g_wnd) KillTimer(g_wnd, TIMER_MOUSE);
        g_mouseTimerPending = false;
    }
}

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                  LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (g_shuttingDown) return;
    g_wm.OnWinEvent(event, hwnd);
}

static void InstallHooks() {
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    struct Range { DWORD lo, hi; };
    // Exactly the events OnWinEvent acts on, and no others. Every event in a
    // subscribed range is a cross-process callback the OS has to marshal to
    // this thread, so a range that spans events we drop on the floor is pure
    // cost - and 0x0003..0x0017 swept up menus, scrolling, capture changes,
    // drag starts and alert sounds to be discarded on arrival.
    const Range ranges[] = {
        { EVENT_SYSTEM_FOREGROUND,     EVENT_SYSTEM_FOREGROUND    },  // 0x0003
        { EVENT_SYSTEM_MOVESIZESTART,  EVENT_SYSTEM_MOVESIZEEND   },  // 0x000A .. 0x000B
        { EVENT_SYSTEM_MINIMIZESTART,  EVENT_SYSTEM_MINIMIZEEND   },  // 0x0016 .. 0x0017
        { EVENT_OBJECT_DESTROY,        EVENT_OBJECT_HIDE          },  // 0x8001 .. 0x8003
        // A window being given its title. Applications create the window
        // first and name it afterwards, so this is the moment a great many of
        // them stop looking like something to ignore and start looking like
        // something to arrange. Without it that window waits until the user
        // clicks on it. OnWinEvent answers this one from a single hash lookup
        // unless it is a window we are already waiting on, which matters -
        // a desktop produces a lot of title changes.
        { EVENT_OBJECT_NAMECHANGE,     EVENT_OBJECT_NAMECHANGE    },  // 0x800C
        { EVENT_OBJECT_CLOAKED,        EVENT_OBJECT_UNCLOAKED     },  // 0x8017 .. 0x8018
    };
    for (const auto& r : ranges) {
        HWINEVENTHOOK h = SetWinEventHook(r.lo, r.hi, nullptr, WinEventProc, 0, 0, flags);
        if (h) g_hooks.push_back(h);
    }
}

static void RemoveHooks() {
    if (g_mouseHook) { UnhookWinEvent(g_mouseHook); g_mouseHook = nullptr; }
    for (HWINEVENTHOOK h : g_hooks) UnhookWinEvent(h);
    g_hooks.clear();
}

// ---------------------------------------------------------------- diagnostics
// "It does not work on my other machine" is not a report anyone can act on, and
// the things that actually differ between two Windows installs - the build, the
// scaling, which shortcuts another program already owns, whether the tiler is
// running at the same integrity level as the windows it is being asked to move -
// are all invisible from the desktop. This writes them all down in one file.
static std::wstring DiagnosticsText() {
    std::wstring out;
    wchar_t line[1024];

    // Truncating, never swprintf_s (invariant 57): a launch binding's command
    // is user text of any length, and it is printed below.
    auto add = [&](const wchar_t* fmt, auto... args) {
        _snwprintf_s(line, _TRUNCATE, fmt, args...);
        out += line;
        out += L"\r\n";
    };

    add(L"%s %s diagnostics", kAppName, kVersion);
    add(L"executable      : %s", ExePath().c_str());
    add(L"settings        : %s", ConfigPath().c_str());

    // The real build number. GetVersionEx lies to unmanifested callers, and the
    // manifest only ever admits to the versions it lists; the registry does not.
    {
        wchar_t build[64] = L"?", name[256] = L"?";
        DWORD size = sizeof(build);
        RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     L"CurrentBuild", RRF_RT_REG_SZ, nullptr, build, &size);
        size = sizeof(name);
        RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     L"ProductName", RRF_RT_REG_SZ, nullptr, name, &size);
        add(L"windows         : %s (build %s)", name, build);
    }

    add(L"running elevated: %s", SelfIsElevated() ? L"yes" : L"no");
    add(L"integrity level : 0x%04X", OwnProcessIntegrity());
    add(L"elevated logon  : %s", ElevatedAutostartInstalled() ? L"task installed"
                                                              : L"not installed");
    add(L"autostart entry : %s", AutostartCommand().empty()
                                     ? L"(none)" : AutostartCommand().c_str());
    add(L"process dpi     : %u", DpiForWindow(g_wnd));

    out += L"\r\nmonitors:\r\n";
    for (const auto& m : EnumMonitors()) {
        add(L"  %s %d,%d %dx%d   work %d,%d %dx%d",
            m.primary ? L"*" : L" ",
            m.full.x, m.full.y, m.full.w, m.full.h,
            m.work.x, m.work.y, m.work.w, m.work.h);
    }

    out += L"\r\nmemory:\r\n";
    out += MemoryReport();

    add(L"\nwindows managed : %d", g_wm.ManagedCount());
    add(L"blocked by uipi : %d  (running as administrator; cannot be moved)",
        g_wm.BlockedCount());
    add(L"fullscreen pause: %s%s", g_cfg.pauseForFullscreen ? L"on" : L"off",
        g_wm.GameMode() ? L" (active right now)" : L"");

    // The single most common difference between two machines: another program
    // already owns one of these chords, so the shortcut silently does nothing.
    out += L"\r\nshortcuts:\r\n";
    for (const auto& kb : g_cfg.binds) {
        const wchar_t* how =
            kb.route == BindRoute::Hotkey  ? L"ok" :
            kb.route == BindRoute::Hook    ? L"claimed via keyboard hook" :
            kb.route == BindRoute::Blocked ? L"BLOCKED - another program owns it"
                                           : L"not registered";
        add(L"  %-28s %-34s %s", DescribeChord(kb.mods, kb.vk).c_str(),
            DescribeAction(kb).c_str(), how);
    }

    out += L"\r\nlearned window sizes:\r\n";
    if (g_cfg.learnedLimits.empty()) out += L"  (none yet)\r\n";
    for (const auto& e : g_cfg.learnedLimits) {
        add(L"  %-40s min %dx%d max %dx%d%s", e.first.c_str(),
            e.second.minW, e.second.minH, e.second.maxW, e.second.maxH,
            e.second.tooLarge ? L"  [never tiled: will not fit]" : L"");
    }
    return out;
}

static void WriteDiagnostics() {
    const std::wstring path = ConfigDir() + L"\\diagnostics.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8") != 0 || !f) {
        TrayBalloon(kAppName, L"Could not write the diagnostics file.");
        return;
    }
    fputws(DiagnosticsText().c_str(), f);
    fclose(f);
    ShellExecuteW(nullptr, L"open", L"notepad.exe", path.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- tray menu
static void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    wchar_t header[128];
    swprintf_s(header, L"%s %s", kAppName, kVersion);
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, header);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Five groups, top to bottom: the windows, the tiling, the overlays,
    // maintenance, and leaving. Everything that used to be a top-level item
    // with a related item beside it is a submenu now, so the menu reads as
    // a short list of things rather than a long list of switches.
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings...");
    SetMenuDefaultItem(menu, IDM_SETTINGS, FALSE);
    AppendMenuW(menu, MF_STRING, IDM_WELCOME, L"Welcome and shortcuts...");
    AppendMenuW(menu, MF_STRING, IDM_SHORTCUTS, L"Keyboard shortcuts...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // ---- tiling
    AppendMenuW(menu, MF_STRING | (g_wm.TilingEnabled() ? MF_CHECKED : 0),
                IDM_TILING, L"Tiling active");
    AppendMenuW(menu, MF_STRING | (g_wm.GapsEnabled() ? MF_CHECKED : 0),
                IDM_GAPS, L"Gaps");

    HMENU layouts = CreatePopupMenu();
    const LayoutKind cur = g_wm.ActiveLayout();
    for (int i = 0; i < (int)LayoutKind::COUNT; ++i)
        AppendMenuW(layouts, MF_STRING | (cur == (LayoutKind)i ? MF_CHECKED : 0),
                    IDM_LAYOUT_BASE + i, LayoutName((LayoutKind)i));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)layouts, L"Layout");

    HMENU spaces = CreatePopupMenu();
    for (int i = 0; i < g_cfg.workspaceCount; ++i) {
        wchar_t label[32];
        swprintf_s(label, L"Workspace %d", i + 1);
        AppendMenuW(spaces, MF_STRING | (g_wm.ActiveWorkspace() == i ? MF_CHECKED : 0),
                    IDM_WORKSPACE_BASE + i, label);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)spaces, L"Workspace");
    AppendMenuW(menu, MF_STRING, IDM_RETILE, L"Re-arrange now");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // ---- overlays: each one a submenu of show / pin / its settings page
    HMENU monitor = CreatePopupMenu();
    AppendMenuW(monitor, MF_STRING | (g_cfg.monitorEnabled ? MF_CHECKED : 0),
                IDM_MONITOR, L"Show");
    AppendMenuW(monitor, MF_STRING | (g_cfg.monitorPinned ? MF_CHECKED : 0) |
                         (g_cfg.monitorEnabled ? 0 : MF_GRAYED),
                IDM_MONITOR_PIN, L"Pin in place");
    AppendMenuW(monitor, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(monitor, MF_STRING, IDM_MONITOR_SETTINGS, L"Monitor settings...");
    AppendMenuW(menu, MF_POPUP | (g_cfg.monitorEnabled ? MF_CHECKED : 0),
                (UINT_PTR)monitor, L"System monitor");

    HMENU clock = CreatePopupMenu();
    AppendMenuW(clock, MF_STRING | (g_cfg.clockEnabled ? MF_CHECKED : 0),
                IDM_CLOCK, L"Show");
    AppendMenuW(clock, MF_STRING | (g_cfg.clockPinned ? MF_CHECKED : 0) |
                       (g_cfg.clockEnabled ? 0 : MF_GRAYED),
                IDM_CLOCK_PIN, L"Pin in place");
    AppendMenuW(clock, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(clock, MF_STRING, IDM_CLOCK_SETTINGS, L"Clock settings...");
    AppendMenuW(menu, MF_POPUP | (g_cfg.clockEnabled ? MF_CHECKED : 0),
                (UINT_PTR)clock, L"Clock");

    HMENU timerMenu = CreatePopupMenu();
    AppendMenuW(timerMenu, MF_STRING | (g_cfg.timerShown ? MF_CHECKED : 0),
                IDM_TIMER, L"Show");
    AppendMenuW(timerMenu, MF_STRING | (g_cfg.timerPinned ? MF_CHECKED : 0) |
                           (g_cfg.timerShown ? 0 : MF_GRAYED),
                IDM_TIMER_PIN, L"Pin in place");
    // A pinned panel cannot take a key, so this is how a ringing alarm is stopped.
    if (TimerAlarmRinging()) {
        AppendMenuW(timerMenu, MF_STRING, IDM_ALARM_SNOOZE, L"Snooze alarm");
        AppendMenuW(timerMenu, MF_STRING, IDM_ALARM_DISMISS, L"Dismiss alarm");
    } else if (TimerAlarming()) {
        AppendMenuW(timerMenu, MF_STRING, IDM_TIMER_STOP, L"Stop alarm");
    }
    AppendMenuW(timerMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(timerMenu, MF_STRING, IDM_TIMER_SETTINGS, L"Clock panel settings...");
    AppendMenuW(menu, MF_POPUP | (g_cfg.timerShown ? MF_CHECKED : 0),
                (UINT_PTR)timerMenu, L"Clock panel");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // ---- maintenance
    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, IDM_RELOAD,     L"Reload settings from disk");
    AppendMenuW(tools, MF_STRING, IDM_EDITCFG,    L"Edit the config file...");
    AppendMenuW(tools, MF_STRING, IDM_OPENDIR,    L"Open the settings folder");
    AppendMenuW(tools, MF_STRING, IDM_DIAG,       L"Diagnostics report...");
    AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(tools, MF_STRING, IDM_RESTOREALL, L"Show all hidden windows");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)tools, L"Tools");

    HMENU startup = CreatePopupMenu();
    AppendMenuW(startup, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"Start with Windows");
    // Running elevated is the only way to arrange windows that are themselves
    // elevated; the item is only offered when it would change anything.
    if (!SelfIsElevated())
        AppendMenuW(startup, MF_STRING, IDM_ELEVATE, L"Restart as administrator");
    {
        // Checking the state costs a schtasks call, so it is only asked for
        // when the menu is actually being built. Creating or removing the
        // task needs administrator rights, so the item is greyed rather than
        // hidden: the user can see the option exists and what it would take.
        const bool haveTask = ElevatedAutostartInstalled();
        UINT flags = MF_STRING | (haveTask ? MF_CHECKED : 0);
        if (!SelfIsElevated() && !haveTask) flags |= MF_GRAYED;
        AppendMenuW(startup, flags, IDM_ELEVAUTO,
                    SelfIsElevated() || haveTask
                        ? L"Always start as administrator"
                        : L"Always start as administrator (restart as admin first)");
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)startup, L"Startup");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------- window proc
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg) {
        g_trayAdded = false;
        TrayAdd();
        // Explorer restarting destroys the desktop window, and with it any
        // overlay owned by it.
        MonitorReattach();
        ClockReattach();
        return 0;
    }

    switch (msg) {
        case WM_AWA_ANIMTICK:
            // Posted by the animation's own high-resolution ticker thread.
            g_wm.AnimTickHandled();
            g_wm.AnimStep();
            return 0;

        case WM_AWA_TRAY:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
                ShowTrayMenu();
            else if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK)
                SettingsOpen(g_inst);
            return 0;

        case WM_HOTKEY:
            if (const Keybind* kb = HotkeyForId((int)wp)) RunAction(*kb);
            return 0;

        case WM_AWA_HOOKKEY:
            if (const Keybind* kb = HotkeyForHookIndex(wp)) RunAction(*kb);
            return 0;

        // A retile that skipped the debounce because the desktop had been
        // still. Identical to the timer's arm of it, minus the wait.
        case WM_AWA_MODDRAG:
            ModDragBegin(wp);
            return 0;

        // A `--msg` command, posted by the control channel's pipe thread. It
        // runs here, from the main loop, for the same reason every other action
        // does: between passes, never inside one.
        case WM_AWA_IPC:
            IpcExecute(wp);
            return 0;

        case WM_AWA_RETILE:
            if (!g_wm.RetilePending()) return 0;    // the timer beat us to it
            g_wm.RetileNow();
            TrayUpdate();
            return 0;

        case WM_TIMER:
            if (wp == TIMER_RETILE) {
                KillTimer(hwnd, TIMER_RETILE);
                g_wm.RetileNow();
                TrayUpdate();
            } else if (wp == TIMER_ANIM) {
                g_wm.AnimStep();
            } else if (wp == TIMER_MOUSE) {
                KillTimer(hwnd, TIMER_MOUSE);       // one-shot; the hook re-arms it
                g_mouseTimerPending = false;
                if (g_wm.GameMode()) return 0;
                // Only a change of window is worth reporting. The pointer
                // rests on the same window for seconds at a time, and handing
                // it over on every tick meant a forced fullscreen check and a
                // DWM border write eight times a second for nothing.
                static HWND lastUnder = nullptr;
                POINT pt;
                GetCursorPos(&pt);
                HWND under = WindowFromPoint(pt);
                if (under) under = GetAncestor(under, GA_ROOT);
                if (under == lastUnder) return 0;
                lastUnder = under;
                if (under && under != GetForegroundWindow())
                    g_wm.OnForeground(under);
            } else if (wp == TIMER_GAMECHK) {
                g_wm.UpdateGameMode(true);
            } else if (wp == TIMER_DRAG) {
                g_wm.OnDragTick();
            } else if (wp == TIMER_PENDING) {
                g_wm.RetryPending();
            } else if (wp == TIMER_DISPLAY) {
                // Input while the display is "off" means it is not.
                if (g_displayOff && IdleMs() < 5 * 1000) SetDisplayOff(false);
            } else if (wp == TIMER_TRIM) {
                // Not while something is on screen that is about to be used:
                // the trim would only be undone by the next repaint.
                if (!SettingsVisible() && !LauncherVisible() && !g_wm.Dragging()) {
                    TrimMemory();
                    SetTimer(hwnd, TIMER_TRIM, kTrimIdleMs, nullptr);
                } else {
                    SetTimer(hwnd, TIMER_TRIM, 30 * 1000, nullptr);
                }
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_SETTINGS:  SettingsOpen(g_inst); break;
                case IDM_WELCOME:   SettingsOpenTab(PAGE_WELCOME); break;
                case IDM_SHORTCUTS: SettingsOpenTab(PAGE_SHORTCUTS); break;
                case IDM_MONITOR:
                    g_cfg.monitorEnabled = !g_cfg.monitorEnabled;
                    UpdateOverlayVisibility();
                    AppSaveConfig();
                    SettingsRefresh();
                    break;
                case IDM_MONITOR_PIN:
                    MonitorSetPinned(!g_cfg.monitorPinned);
                    break;
                case IDM_CLOCK:
                    g_cfg.clockEnabled = !g_cfg.clockEnabled;
                    UpdateOverlayVisibility();
                    AppSaveConfig();
                    SettingsRefresh();
                    break;
                case IDM_CLOCK_PIN:
                    ClockSetPinned(!g_cfg.clockPinned);
                    break;
                case IDM_TIMER:
                    g_cfg.timerShown = !g_cfg.timerShown;
                    UpdateOverlayVisibility();
                    AppSaveConfig();
                    SettingsRefresh();
                    break;
                case IDM_TIMER_PIN:      TimerSetPinned(!g_cfg.timerPinned); break;
                case IDM_TIMER_STOP:     TimerStopAlarm(); break;
                case IDM_ALARM_SNOOZE:   TimerSnoozeAlarm(); break;
                case IDM_ALARM_DISMISS:  TimerDismissAlarm(); break;
                case IDM_TIMER_SETTINGS: SettingsOpenTab(PAGE_CLOCK); break;
                case IDM_MONITOR_SETTINGS: SettingsOpenTab(PAGE_MONITOR); break;
                case IDM_CLOCK_SETTINGS:   SettingsOpenTab(PAGE_CLOCK); break;
                case IDM_TILING:    g_wm.ActToggleTiling(); TrayUpdate();
                                    SettingsRefreshStatus(); break;
                case IDM_GAPS:      g_wm.ActToggleGaps(); TrayUpdate(); break;
                case IDM_RETILE:    AppRetileNow(); break;
                case IDM_RELOAD:    ReloadConfig(true); break;
                case IDM_EDITCFG:   AppOpenConfigFile(); break;
                case IDM_OPENDIR:
                    ShellExecuteW(nullptr, L"open", ConfigDir().c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                case IDM_AUTOSTART:
                    SetAutostart(!AutostartEnabled());
                    SettingsRefresh();
                    break;
                case IDM_RESTOREALL: g_wm.RestoreAllWindows(); g_wm.RetileNow(); break;
                case IDM_DIAG:      WriteDiagnostics(); break;
                case IDM_ELEVAUTO:
                    if (ElevatedAutostartInstalled()) {
                        if (RemoveElevatedAutostart())
                            TrayBalloon(kAppName,
                                L"ProWindows will no longer start as administrator.");
                        else
                            TrayBalloon(kAppName,
                                L"Could not remove the scheduled task. Restart "
                                L"ProWindows as administrator and try again.");
                    } else if (InstallElevatedAutostart()) {
                        // The scheduled task now starts it at logon, so the Run
                        // entry would only start a second, unelevated copy.
                        SetAutostart(false);
                        TrayBalloon(kAppName,
                            L"ProWindows will start as administrator at every "
                            L"logon, with no prompt, and will be able to arrange "
                            L"windows that run as administrator.");
                    } else {
                        TrayBalloon(kAppName,
                            L"That needs administrator rights. Use \"Restart as "
                            L"administrator\" first, then try again.");
                    }
                    SettingsRefresh();
                    break;
                case IDM_ELEVATE:
                    // Put every hidden window back before handing over: the new
                    // instance knows nothing about what this one hid.
                    g_wm.RestoreAllWindows();
                    if (RestartElevated()) DestroyWindow(hwnd);
                    else TrayBalloon(kAppName, L"Restart as administrator was declined.");
                    break;
                case IDM_EXIT:       DestroyWindow(hwnd); break;
                default: {
                    const UINT id = LOWORD(wp);
                    if (id >= IDM_LAYOUT_BASE &&
                        id < IDM_LAYOUT_BASE + (UINT)LayoutKind::COUNT) {
                        g_wm.ActSetLayout((LayoutKind)(id - IDM_LAYOUT_BASE));
                        TrayUpdate();
                    } else if (id >= IDM_WORKSPACE_BASE && id < IDM_WORKSPACE_BASE + 9) {
                        g_wm.ActSwitchWorkspace((int)(id - IDM_WORKSPACE_BASE));
                        TrayUpdate();
                    }
                    break;
                }
            }
            return 0;

        case WM_DISPLAYCHANGE:
            g_wm.OnDisplayChange();
            return 0;

        case WM_POWERBROADCAST:
            // A machine left on overnight was still sampling CPU, GPU, disk and
            // network once a second and repainting a layered window nobody was
            // looking at. Nothing here changes what the user sees; it only
            // stops when there is no user to see it.
            if (wp == PBT_POWERSETTINGCHANGE) {
                const auto* s = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
                if (s && IsEqualGUID(s->PowerSetting, kConsoleDisplayState) &&
                    s->DataLength >= sizeof(DWORD)) {
                    // 0 off, 1 on, 2 dimmed.
                    SetDisplayOff(*reinterpret_cast<const DWORD*>(s->Data) == 0);
                }
            } else if (wp == PBT_APMSUSPEND) {
                SetDisplayOff(true);
            } else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                SetDisplayOff(false);
                TimerCheckNow();      // anything that ended during sleep fires now
                // Waking can bring displays back in a different arrangement.
                g_wm.OnDisplayChange();
            }
            return TRUE;

        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA) g_wm.OnDisplayChange();
            // A zone or DST change: the clock panel's per-year rule tables are stale
            // (the panel window may not exist yet to hear it).
            if (!lp || wcscmp((const wchar_t*)lp, L"intl") == 0) alarm::InvalidateSystemZone();
            return 0;

        case WM_TIMECHANGE:
            alarm::InvalidateSystemZone();
            return 0;

        case WM_DPICHANGED:
            g_wm.OnDisplayChange();
            return 0;

        case WM_QUERYENDSESSION:
            // Answer the question and nothing more. Any other application may
            // still veto the session ending, and shutting the manager down
            // here left it torn down - no ticker thread, every hidden window
            // put back, the config already written - in a process that then
            // carried on running.
            return TRUE;

        case WM_ENDSESSION:
            // wParam is FALSE when the session is not ending after all.
            if (wp) {
                g_shuttingDown = true;
                g_wm.Shutdown();
                TrayRemove();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- crash safety
// Nothing here may touch a container.
//
// The manager mutates an unordered_map on every window event, so an unhandled
// exception is most likely to arrive part way through one of those mutations -
// and walking a half-updated map from here faults again, which leaves the user
// with exactly the vanished windows this handler exists to prevent.
// EmergencyUnhideAll reads a flat array of handles instead. Registered hotkeys
// and the keyboard hook need no attention: Windows releases both when the
// process dies, which is where this is going.
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS*) {
    EmergencyUnhideAll();
    TrayRemove();
    return EXCEPTION_CONTINUE_SEARCH;
}

// The CRT's answer to a bad argument - a swprintf_s whose buffer is too
// small, a _wcsicmp handed a null - is to call this, and with no handler
// installed the default one is a fast-fail: the process is terminated on the
// spot with exception 0xC0000409, no exception filter runs, and the log stays
// silent. That is how the tray tooltip took the whole application down (see
// TrayTooltip). A handler that returns turns the same mistake into an error
// code from the function that made it, which every caller here already copes
// with, and a line in the log saying which one it was.
static void __cdecl InvalidParameterHandler(const wchar_t* expression,
                                            const wchar_t* function,
                                            const wchar_t* file, unsigned line,
                                            uintptr_t) {
    // Release builds pass nulls for all four; the log line still says it
    // happened, which is the part that matters.
    AWA_LOG(L"CRT invalid parameter: %s in %s (%s:%u) - continuing",
            expression ? expression : L"?", function ? function : L"?",
            file ? file : L"?", line);
}

// Another copy of ProWindows already holds the single-instance mutex. Several
// versions of this program live side by side on a development machine, and
// they all share the mutex, the window class and the config folder - so
// starting 1.3 while 1.2 was still running from another folder simply poked
// the running 1.2 and opened *its* settings window, which looked exactly like
// every version being the same program. Now: the same executable opens the
// running copy's settings as before; a different one is offered the choice of
// replacing what is running.
static bool ReplaceRunningInstance(HWND existing) {
    std::wstring theirs;
    DWORD pid = 0;
    GetWindowThreadProcessId(existing, &pid);
    if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2] = {};
        DWORD len = (DWORD)ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(proc, 0, path, &len)) theirs = path;
        CloseHandle(proc);
    }
    const std::wstring ours = ExePath();
    if (theirs.empty() || _wcsicmp(theirs.c_str(), ours.c_str()) == 0) {
        PostMessageW(existing, WM_COMMAND, IDM_SETTINGS, 0);
        return false;
    }

    std::wstring text = L"ProWindows is already running from:\n" + theirs +
                        L"\n\nThis copy is:\n" + ours +
                        L"\n\nStop the running copy and use this one instead?\n"
                        L"(Its hidden windows are put back first. If it starts "
                        L"with Windows, the startup entry is moved to this copy.)";
    const int answer = MessageBoxW(nullptr, text.c_str(),
                                   L"ProWindows is already running",
                                   MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1 |
                                   MB_SETFOREGROUND | MB_TOPMOST);
    if (answer != IDYES) return false;

    // WM_CLOSE on the hidden main window is DefWindowProc -> DestroyWindow ->
    // the ordinary exit path, in every version that has ever shipped. Not
    // TerminateProcess: that skips RestoreAllWindows.
    PostMessageW(existing, WM_CLOSE, 0, 0);
    if (HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(proc, 8000);
        CloseHandle(proc);
    }
    return true;
}

} // namespace awa

// ---------------------------------------------------------------- entry point
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    using namespace awa;

    // `--msg` is a client, not a second copy of the application: it has to be
    // handled before the single-instance mutex, because the whole point is to
    // talk to the instance that already holds it.
    //
    // `--config <dir>` has to be read here too, and for a stricter reason: the
    // settings folder is resolved once, on first use, and half of startup uses
    // it. Anything later is too late.
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            std::wstring command;
            bool isClient = false;
            for (int i = 1; i < argc; ++i) {
                const std::wstring a = argv[i];
                if ((a == L"--config" || a == L"-c") && i + 1 < argc) {
                    SetConfigDirOverride(argv[i + 1]);
                    ++i;
                    continue;
                }
                if (a == L"--msg" || a == L"-m") {
                    isClient = true;
                    // Everything after it is the command, so both
                    // `--msg "workspace 3"` and `--msg workspace 3` work.
                    for (int j = i + 1; j < argc; ++j) {
                        if (!command.empty()) command += L' ';
                        command += argv[j];
                    }
                    break;
                }
                if (a == L"--help" || a == L"-h" || a == L"/?") {
                    isClient = true;
                    command  = L"help";
                    break;
                }
            }
            LocalFree(argv);
            if (isClient) return IpcClientMain(command);
        }
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kWndClass, nullptr);
        bool replaced = existing && ReplaceRunningInstance(existing);
        if (!replaced) {
            CloseHandle(mutex);
            return 0;
        }
        // The old copy has been asked to go. Take the mutex over once it has.
        CloseHandle(mutex);
        mutex = nullptr;
        for (int i = 0; i < 100 && !mutex; ++i) {
            mutex = CreateMutexW(nullptr, TRUE, kMutexName);
            if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
                CloseHandle(mutex);
                mutex = nullptr;
                Sleep(100);
            }
        }
        if (!mutex) {
            MessageBoxW(nullptr, L"The running copy did not exit. Exit it from its "
                                 L"tray icon and try again.", kAppName, MB_ICONWARNING);
            return 0;
        }
    }

    // Installed before anything else runs: see InvalidParameterHandler.
    _set_invalid_parameter_handler(InvalidParameterHandler);

    g_inst = inst;
    // DPI awareness comes from the embedded manifest (PerMonitorV2). Calling
    // SetProcessDpiAwarenessContext here would fail anyway - the manifest has
    // already fixed it - and it is a hard import that only exists from Windows
    // 10 1703 onwards, so it stopped the binary loading at all on anything
    // older, including the Windows 8.1 the manifest says is supported.
    SetUnhandledExceptionFilter(CrashHandler);
    // Paired with the CoUninitialize at the end, which must only run if this
    // succeeded: the calls are reference-counted, and an unmatched uninitialise
    // tears the apartment down under whatever is still using it.
    const HRESULT comInit =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc = { sizeof(icc),
                                 ICC_STANDARD_CLASSES | ICC_BAR_CLASSES |
                                 ICC_UPDOWN_CLASS | ICC_TAB_CLASSES |
                                 ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
    theme::Init();

    const bool firstRun =
        GetFileAttributesW(ConfigPath().c_str()) == INVALID_FILE_ATTRIBUTES;
    LoadConfig();

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = kWndClass;
    wc.hIcon         = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    if (!RegisterClassExW(&wc)) return 1;

    // A real (never shown) top-level window: message-only windows do not receive
    // WM_DISPLAYCHANGE / WM_SETTINGCHANGE broadcasts.
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWndClass, kAppName, WS_POPUP,
                            0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return 1;

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // Hooks before the first scan, not after. The scan and the first pass
    // take a while at logon - every application is starting at once - and a
    // window that opened during them used to be nobody's: too late for the
    // scan, too early for the hooks. Events raised before the message loop
    // runs are simply queued, and the manager's reentrancy guard already
    // handles any that are delivered inside the pass itself.
    InstallHooks();
    g_wm.Init(g_wnd, &g_cfg);
    TrayAdd();
    HotkeysRegister(g_wnd, &g_cfg);
    ModDragInit(g_wnd);
    ModDragApplyConfig(&g_cfg);
    DragGuideInit(inst, &g_cfg);
    MonitorInit(inst, &g_cfg);
    ClockInit(inst, &g_cfg);
    TimerInit(inst, &g_cfg);
    // MonitorInit and ClockInit show their overlays straight from the config;
    // if a game is already running they must not.
    UpdateOverlayVisibility();
    // Ask to be told when the screen goes off, so the overlay can stop. The
    // registration is held so it can be given back at the end rather than left
    // pointing at a window that is about to be destroyed.
    g_powerNotify = RegisterPowerSettingNotification(g_wnd, &kConsoleDisplayState,
                                                     DEVICE_NOTIFY_WINDOW_HANDLE);
    // Both start scanning immediately, so the first chord is instant. The file
    // index walk is the slower of the two and runs on its own thread.
    LauncherInit(inst, &g_cfg);
    SearchInit(&g_cfg);
    // Last, so a command arriving on the first millisecond finds a manager
    // that is actually ready to answer it.
    IpcStart(g_wnd, IpcCommandHandler);
    MigrateLegacyAutostart();
    RepairAutostartPath();
    ApplyFocusFollows();
    // Startup allocates and then forgets a great deal: the app scan, the
    // index and icon caches, the first pass over every window. Give it back
    // once the machine has settled.
    AppScheduleTrim(45 * 1000);

    const bool startHidden = g_cfg.startMinimized ||
                             (cmdLine && (wcsstr(cmdLine, L"--tray") ||
                                          wcsstr(cmdLine, L"/tray")));
    // Opening the app lands on Welcome; a first run shows it even when hidden.
    if (!startHidden || firstRun) SettingsOpenTab(PAGE_WELCOME);
    else TrayBalloon(kAppName, L"Running in the tray - click the icon for settings.");

    // The Welcome page is already open; this says where it lives afterwards.
    if (firstRun && startHidden)
        TrayBalloon(kAppName, L"Tiling is active. The tray icon brings this window back.");

    AWA_LOG(L"%s %s started%s", kAppName, kVersion, startHidden ? L" (tray)" : L"");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (SettingsTranslateMessage(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_shuttingDown = true;
    IpcStop();          // before the window goes; in-flight commands finish first
    if (g_powerNotify) {
        UnregisterPowerSettingNotification(g_powerNotify);
        g_powerNotify = nullptr;
    }
    MonitorShutdown();
    TimerShutdown();
    ClockShutdown();
    DragGuideShutdown();
    LauncherShutdown();
    SearchShutdown();
    SettingsDestroy();
    theme::Shutdown();
    RemoveHooks();
    ModDragShutdown();
    HotkeysShutdown();
    g_wm.Shutdown();
    TrayRemove();
    if (SUCCEEDED(comInit)) CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
