// ProWindows - Start menu styling, the ProWindows side. See startmenustyler.h.
//
// The sibling of explorerstyler.cpp: the same worker-thread model and the same
// way into the process (LoadLibraryW by a remote thread, then the DLL's
// exported StylerStart by a second one). Kept as a second file rather than a
// shared injector so that the Explorer side stays exactly as reviewed; the two
// can be merged once both have had their live check.
//
// What the Start menu changes (PLAN-1.6 9.B, "9.T2 done - what differs"):
//
// 1. The hosts (StartMenuExperienceHost.exe, SearchHost.exe, SearchApp.exe on
//    older builds) are packaged AppContainer processes. They can read and
//    write only what has an ACE for their package, ALL APPLICATION PACKAGES
//    (S-1-15-2-1) or ALL RESTRICTED APPLICATION PACKAGES (S-1-15-2-2) - the
//    second is the one a lowered-privilege (LPAC) process is checked against.
//    So before the first injection ProWindows gives both SIDs
//      - read + execute on ProWindows_startmenu.dll (the file, in place), and
//      - read + write + delete on %APPDATA%\ProWindows\startmenu-styler\ with
//        an inheritable ACE, so everything made in it afterwards carries it.
//    That folder holds everything the DLL touches: the ini, the log, the values
//    the mod stores, and the signal files below. Nothing else of ProWindows'
//    data is opened up, and the path is passed to StylerStart because
//    SHGetKnownFolderPath inside a container answers with the package's own
//    virtualised %APPDATA%.
//
// 2. Named events are not used. "Local\" inside a container is the container's
//    own namespace (\Sessions\N\AppContainerNamedObjects\<package SID>), which
//    a normal process cannot open by name; a container cannot create events in
//    the session's global namespace; and creating them from here first would
//    mean guessing what each package may open. What both sides already agree
//    on is the folder above, so the signals are files in it (the Explorer DLL
//    still uses events - same shim, PW_STYLER_PACKAGED picks):
//      alive.<pid>   held open by the DLL, delete-on-close: the kernel removes
//                    it when the DLL stops or the host dies. Existing = styled.
//      reload.<pid>  made here after the ini changed; the DLL deletes it.
//      stop.<pid>    made here; the DLL deletes it and takes the styling out.
//    The DLL sleeps on a directory change notification; ProWindows never polls.
//
// 3. The crash guard cannot watch TaskbarCreated (the hosts restart on their
//    own), so the worker notices a styled or failed process gone within 90 s of
//    its injection. Two of those within three minutes turn styling off (the
//    config is changed on the UI thread: WM_AWA_STYLERCRASH). A host that exits
//    by itself right after injection is also what a non-default Start layout
//    does (the mod exits the process so the layout takes effect, and Windows
//    relaunches it), so the guard doubles as the loop breaker for that.
#include "startmenustyler.h"
#include "app.h"

#include <aclapi.h>
#include <dwmapi.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>

namespace awa {

namespace {

constexpr wchar_t kDllName[] = L"ProWindows_startmenu.dll";
constexpr wchar_t kHosts[][32] = { L"StartMenuExperienceHost.exe", L"SearchHost.exe", L"SearchApp.exe" };
constexpr DWORD   kLoadWaitMs  = 15000;
constexpr DWORD   kStartWaitMs = 30000;   // StylerStart talks to the host's windows
constexpr DWORD   kCrashWindowMs = 90 * 1000;
constexpr DWORD   kCrashMemoryMs = 180 * 1000;   // how long an early exit counts
constexpr DWORD   kRetryMs = 20 * 1000;          // before a failed process is tried again

// ---------------------------------------------------------------- choices
struct Choice { const wchar_t* id; const wchar_t* name; };

// The ids are the theme names the mod's GetSelectedTheme compares against.
const Choice kThemes[] = {
    { L"ProWindows Glass",                    L"ProWindows Glass" },
    { L"ProWindows",                          L"ProWindows" },
    { L"",                                    L"None (my own styles only)" },
    { L"TranslucentStartMenu",                L"TranslucentStartMenu" },
    { L"NoRecommendedSection",                L"NoRecommendedSection" },
    { L"SideBySide",                          L"SideBySide" },
    { L"SideBySide2",                         L"SideBySide2" },
    { L"SideBySideMinimal",                   L"SideBySideMinimal" },
    { L"Down Aero",                           L"Down Aero" },
    { L"Windows10",                           L"Windows10" },
    { L"Windows10_variant_Minimal",           L"Windows10 (Minimal)" },
    { L"Windows11_Metro10",                   L"Windows11_Metro10" },
    { L"Fluent2Inspired",                     L"Fluent2Inspired" },
    { L"RosePine",                            L"RosePine" },
    { L"Windows11_Metro10Minimal",            L"Windows11_Metro10Minimal" },
    { L"Everblush",                           L"Everblush" },
    { L"SunValley",                           L"SunValley" },
    { L"21996",                               L"SunValley (Legacy)" },
    { L"UniMenu",                             L"UniMenu" },
    { L"LegacyFluent",                        L"LegacyFluent" },
    { L"OnlySearch",                          L"OnlySearch" },
    { L"OnlySearch_variant_Minimal",          L"OnlySearch (Minimal)" },
    { L"WindowGlass",                         L"WindowGlass (redesigned Start menu)" },
    { L"Fluid",                               L"Fluid (redesigned Start menu)" },
    { L"Oversimplified&Accentuated",          L"Oversimplified&Accentuated" },
    { L"LiquidGlass2",                        L"LiquidGlass (redesigned Start menu)" },
    { L"LiquidGlass",                         L"LiquidGlass (Legacy)" },
    { L"Windows10X",                          L"Windows10X" },
    { L"TintedGlass",                         L"TintedGlass" },
    { L"LayerMicaUI",                         L"LayerMicaUI (redesigned Start menu)" },
    { L"Borderless",                          L"Borderless" },
    { L"Command Center",                      L"Command Center (redesigned Start menu)" },
    { L"FullScreen",                          L"FullScreen" },
    { L"FrostyGlass",                         L"FrostyGlass (redesigned Start menu)" },
};

// The mod's disableNewStartMenuLayout values.
const Choice kLayouts[] = {
    { L"",                              L"Default for the theme" },
    { L"default",                       L"Windows default" },
    { L"disableNewLayoutKeepPhoneLink", L"Classic layout (removed in 26100.8524)" },
    { L"legacyClassicLayout",           L"Legacy classic layout (removed in 26100.8328)" },
    { L"forceNewLayout",                L"Force new layout (if available)" },
    { L"newLayoutSideBySide",           L"New layout + side by side" },
};

// 0xRRGGBB. The last one is the system accent colour, read when it is used.
constexpr unsigned kTints[3] = { 0x000000, 0x1C1C1E, 0x26303A };
const wchar_t* const kTintNames[] = { L"Black", L"Graphite", L"Steel", L"Accent colour" };

unsigned AccentRgb() {
    DWORD argb = 0;
    BOOL opaque = FALSE;
    if (FAILED(DwmGetColorizationColor(&argb, &opaque))) return 0x0078D4;
    return argb & 0xFFFFFF;
}

unsigned TintRgb(int i) {
    return i >= 0 && i < 3 ? kTints[i] : AccentRgb();
}

// ---------------------------------------------------------------- state
Config* g_cfg = nullptr;       // UI thread only
HWND g_notify = nullptr;

// What the worker is told. Copied out of the config on the UI thread so the
// worker never reads it.
struct Wanted {
    bool enabled = false;
    std::wstring theme;
    std::wstring layout;
    int tint = 0, opacity = 55, highlight = 0, radius = 12, text = 0;
    bool debug = false;
};

std::mutex g_mutex;            // guards everything below
Wanted g_wanted;
std::wstring g_status = L"Not started";
std::set<DWORD> g_styled;      // host pids known to carry the styler
std::map<DWORD, ULONGLONG> g_failed;      // ...that could not be opened or started, and when
// Crash guard: when each pid was injected, and a handle to it. The handle
// tells when the host really exited (it is noticed only at the next Sync, which
// may be minutes later) and keeps its pid from being reused meanwhile.
struct Injected { ULONGLONG at; HANDLE proc; };
std::map<DWORD, Injected> g_injectedAt;
std::vector<ULONGLONG> g_earlyExits;      // crash guard: when a host went within the window

void ForgetInjected() {   // under g_mutex
    for (auto& e : g_injectedAt) if (e.second.proc) CloseHandle(e.second.proc);
    g_injectedAt.clear();
}

enum : unsigned { REQ_SYNC = 1, REQ_RELOAD = 2, REQ_STOP = 4, REQ_QUIT = 8, REQ_DELAYED = 16 };
std::atomic<unsigned> g_requests{0};
HANDLE g_wake = nullptr;
HANDLE g_thread = nullptr;
std::atomic<bool> g_quitting{false};

void Request(unsigned bits) {
    g_requests.fetch_or(bits);
    if (g_wake) SetEvent(g_wake);
}

// ---------------------------------------------------------------- access for packages
bool GrantPackages(const std::wstring& path, bool isDir) {
    PSID sids[2] = {};
    bool ok = ConvertStringSidToSidW(L"S-1-15-2-1", &sids[0]) &&
              ConvertStringSidToSidW(L"S-1-15-2-2", &sids[1]);
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL oldDacl = nullptr, newDacl = nullptr;
    if (ok) {
        ok = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                   nullptr, nullptr, &oldDacl, nullptr, &sd) == ERROR_SUCCESS;
    }
    if (ok) {
        EXPLICIT_ACCESSW ea[2] = {};
        for (int i = 0; i < 2; ++i) {
            // SET_ACCESS replaces what the SID had, so asking again changes nothing.
            ea[i].grfAccessPermissions = isDir
                ? (FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE)
                : (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
            ea[i].grfAccessMode = SET_ACCESS;
            ea[i].grfInheritance = isDir ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
            BuildTrusteeWithSidW(&ea[i].Trustee, sids[i]);
        }
        ok = SetEntriesInAclW(2, ea, oldDacl, &newDacl) == ERROR_SUCCESS;
    }
    if (ok) {
        // Not PROTECTED: the inherited entries the file or folder already has stay.
        ok = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION, nullptr, nullptr, newDacl,
                                   nullptr) == ERROR_SUCCESS;
    }
    if (ok && isDir) {
        // The hosts run at low integrity, and a file or folder without a label
        // counts as medium with no-write-up: whatever the DACL says, a host could
        // not make alive.<pid>, its log or its stored values, nor delete a signal.
        // A low label, inherited by everything made in here, opens this folder
        // and nothing above it.
        PSECURITY_DESCRIPTOR label = nullptr;
        PACL sacl = nullptr;
        BOOL present = FALSE, defaulted = FALSE;
        ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;OICI;NW;;;LW)", SDDL_REVISION_1,
                                                                  &label, nullptr) &&
             GetSecurityDescriptorSacl(label, &present, &sacl, &defaulted) && present && sacl &&
             SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                   LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr,
                                   sacl) == ERROR_SUCCESS;
        if (label) LocalFree(label);
    }
    if (!ok) AWA_LOG(L"start menu styler: cannot give the app packages access to %s (error %lu)",
                     path.c_str(), GetLastError());
    if (newDacl) LocalFree(newDacl);
    if (sd) LocalFree(sd);
    for (PSID s : sids) if (s) LocalFree(s);
    return ok;
}

// ---------------------------------------------------------------- paths
std::wstring DllPath() {
    std::wstring exe = ExePath();
    size_t slash = exe.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash + 1)) + kDllName;
}

std::wstring SignalPath(const wchar_t* what, DWORD pid) {
    return StartMenuStylerDir() + L"\\" + what + L"." + std::to_wstring(pid);
}

// The folder exists and the app packages may use it. Done once per run, and
// again if the folder went away.
std::mutex g_dirMutex;
bool g_dirReady = false;

bool EnsureDir() {
    std::lock_guard<std::mutex> lock(g_dirMutex);
    const std::wstring dir = StartMenuStylerDir();
    if (g_dirReady && GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    CreateDirectoryW(ConfigDir().c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    g_dirReady = GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES && GrantPackages(dir, true);
    return g_dirReady;
}

// True when the DLL in `pid` is running: it holds alive.<pid> open.
bool IsStyled(DWORD pid) {
    return GetFileAttributesW(SignalPath(L"alive", pid).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Asks the DLL in `pid` to reload or stop by making the signal file; false when
// it is not running.
bool SignalStyler(const wchar_t* what, DWORD pid) {
    if (!IsStyled(pid)) return false;
    HANDLE f = CreateFileW(SignalPath(what, pid).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    CloseHandle(f);
    return true;
}

// Signals left for a process that is not styled (it ended first) must not be
// found by a later process with the same id.
void CleanSignals(DWORD pid) {
    DeleteFileW(SignalPath(L"stop", pid).c_str());
    DeleteFileW(SignalPath(L"reload", pid).c_str());
}

// ---------------------------------------------------------------- the ini
// The part of the ini ProWindows owns is a marked block at the top; everything
// after it (custom styles, anything the user adds) is theirs and is kept. A
// later line wins in the DLL's reader, so a user line can override a managed one.
const wchar_t kBlockBegin[] = L"; >>> managed by ProWindows (Settings > Start). Rewritten on every Apply.";
const wchar_t kBlockEnd[]   = L"; <<< end of the managed part. Your own lines go below.";

const wchar_t kTemplate[] =
    L"; Custom Start menu styles for ProWindows. Each style is a target (a XAML\r\n"
    L"; control, optionally with #Name, [Property=Value], @VisualState or\r\n"
    L"; Parent > Child) and the properties to set on it. Remove the leading ';' to\r\n"
    L"; switch an example on, then press Reload on the Start page. Numbers must run\r\n"
    L"; on without gaps. styleConstants[0] to [5] belong to the managed part above\r\n"
    L"; when a ProWindows theme is chosen: start your own at styleConstants[6].\r\n"
    L";\r\n"
    L"; Example: a red Start menu background.\r\n"
    L"; controlStyles[0].target=Border#AcrylicBorder\r\n"
    L"; controlStyles[0].styles[0]=Background=#8B0000\r\n"
    L";\r\n"
    L"; Constants and resource variables work too:\r\n"
    L"; styleConstants[6]=myColor=#202020\r\n"
    L"; themeResourceVariables[0]=MyKey@Dark=#202020\r\n";

std::wstring ReadFileUtf8(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string bytes;
    char buf[8192];
    DWORD got;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) {
        bytes.append(buf, got);
        if (bytes.size() > (4u << 20)) break;
    }
    CloseHandle(f);
    if (bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
    if (bytes.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &out[0], n);
    return out;
}

bool WriteFileUtf8(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string bytes(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), &bytes[0], n, nullptr, nullptr);
    const std::wstring temp = path + L".tmp";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) &&
                    wrote == bytes.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

std::wstring Hex(unsigned v, int digits) {
    wchar_t buf[16];
    swprintf_s(buf, L"#%0*X", digits, v);
    return buf;
}

// Mixes `pct` percent of b into a (both 0xRRGGBB).
unsigned Mix(unsigned a, unsigned b, int pct) {
    unsigned out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const int x = (a >> shift) & 0xFF, y = (b >> shift) & 0xFF;
        out |= (unsigned)((x * (100 - pct) + y * pct) / 100) << shift;
    }
    return out;
}

// The style constants the two ProWindows themes read (see the theme in
// startmenu\styler.cpp). Whole values, never built from each other inside the
// mod: a constant is expanded when it is read, before later ones override it.
std::vector<std::wstring> LookConstants(const Wanted& w) {
    const bool glass = w.theme == L"ProWindows Glass";
    const unsigned tint = TintRgb(w.tint);
    const int alpha = glass ? std::clamp(w.opacity, 0, 100) * 255 / 100 : 255;
    const wchar_t* opacity = glass ? L"0.85" : L"1";

    std::wstring bg, soft, hover;
    if (glass) {
        bg   = L"<WindhawkBlur BlurAmount=\"18\" TintColor=\"" + Hex((unsigned)alpha << 24 | tint, 8) + L"\"/>";
        soft = L"<WindhawkBlur BlurAmount=\"18\" TintColor=\"" + Hex((unsigned)(alpha * 65 / 100) << 24 | tint, 8) + L"\"/>";
    } else {
        bg   = L"<SolidColorBrush Color=\"" + Hex(0xFF000000u | tint, 8) + L"\"/>";
        soft = L"<SolidColorBrush Color=\"" + Hex(0xFF000000u | Mix(tint, 0xFFFFFF, 6), 8) + L"\"/>";
    }
    if (w.highlight == 1) {
        hover = L"<SolidColorBrush Color=\"" + Hex(0xFF000000u | AccentRgb(), 8) + L"\" Opacity=\"" + opacity + L"\"/>";
    } else if (w.highlight == 2) {
        hover = L"<SolidColorBrush Color=\"" + Hex(0xFF000000u | Mix(tint, 0xFFFFFF, 22), 8) + L"\" Opacity=\"" + opacity + L"\"/>";
    } else {
        hover = std::wstring(L"<LinearGradientBrush StartPoint=\"0,0\" EndPoint=\"0,1\" Opacity=\"") + opacity +
                L"\"><GradientStop Color=\"#1A1A1A\" Offset=\"0\"/><GradientStop Color=\"#3A3A3A\" Offset=\"1\"/>"
                L"</LinearGradientBrush>";
    }
    return {
        L"pwBg=" + bg,
        L"pwBgSoft=" + soft,
        L"pwHover=" + hover,
        L"pwEdge=#59E0E0E0",
        L"pwRadius=" + std::to_wstring(std::clamp(w.radius, 0, 12)),
        std::wstring(L"pwText=") + (w.text == 1 ? L"#D0D0D0" : L"#FFFFFF"),
    };
}

// Rewrites the managed block. True when the file's content changed (so styled
// hosts need telling).
bool WriteIni(const Wanted& w) {
    const std::wstring path = StartMenuStylerIniPath();
    std::wstring old = ReadFileUtf8(path);

    std::wstring block = kBlockBegin;
    block += L"\r\ntheme=" + w.theme;
    block += L"\r\ndisableNewStartMenuLayout=" + w.layout;
    block += w.debug ? L"\r\ndebug=1" : L"\r\ndebug=0";
    if (StartThemeIsProWindows(w.theme)) {
        const std::vector<std::wstring> constants = LookConstants(w);
        for (size_t i = 0; i < constants.size(); ++i)
            block += L"\r\nstyleConstants[" + std::to_wstring(i) + L"]=" + constants[i];
    }
    block += L"\r\n";
    block += kBlockEnd;
    block += L"\r\n";

    std::wstring rest;
    const size_t b = old.find(kBlockBegin);
    const size_t e = b == std::wstring::npos ? b : old.find(kBlockEnd, b);
    if (b != std::wstring::npos && e != std::wstring::npos) {
        size_t after = e + wcslen(kBlockEnd);
        while (after < old.size() && (old[after] == L'\r' || old[after] == L'\n')) ++after;
        rest = old.substr(0, b) + old.substr(after);
    } else if (old.empty()) {
        rest = kTemplate;
    } else {
        rest = old;
    }

    const std::wstring fresh = block + rest;
    if (fresh == old) return false;
    EnsureDir();   // before the file: it then carries the folder's inherited ACE
    if (!WriteFileUtf8(path, fresh)) {
        AWA_LOG(L"start menu styler: could not write %s", path.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- processes
bool IsHostName(const wchar_t* exe) {
    for (const auto& h : kHosts) if (_wcsicmp(exe, h) == 0) return true;
    return false;
}

std::vector<DWORD> HostPids() {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (!IsHostName(pe.szExeFile)) continue;
        DWORD s = 0;
        if (ProcessIdToSessionId(pe.th32ProcessID, &s) && s == session)
            out.push_back(pe.th32ProcessID);
    }
    CloseHandle(snap);
    return out;
}

// Is `pid` one of the hosts? For the foreground check, on the UI thread: one
// OpenProcess and one query, no snapshot.
bool IsHostPid(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    wchar_t path[MAX_PATH];
    DWORD n = ARRAYSIZE(path);
    const bool ok = QueryFullProcessImageNameW(proc, 0, path, &n) != 0;
    CloseHandle(proc);
    if (!ok) return false;
    const wchar_t* slash = wcsrchr(path, L'\\');
    return IsHostName(slash ? slash + 1 : path);
}

// Where the DLL at `path` sits in `proc`, 0 if it is not loaded. By full path:
// a ProWindows_startmenu.dll from another ProWindows folder can be loaded (and,
// since the DLL is never unloaded, stay loaded) in the same host.
ULONG_PTR RemoteModuleBase(DWORD pid, const std::wstring& path) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) {
            // ERROR_BAD_LENGTH / PARTIAL_COPY: the module list moved while it was copied.
            Sleep(50);
            continue;
        }
        MODULEENTRY32W me = {};
        me.dwSize = sizeof(me);
        ULONG_PTR base = 0;
        for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
            if (_wcsicmp(me.szExePath, path.c_str()) == 0) { base = (ULONG_PTR)me.modBaseAddr; break; }
        }
        CloseHandle(snap);
        return base;
    }
    return 0;
}

// Runs `routine(param)` in the process and waits. Returns the thread's exit
// code; false when the thread could not be made or did not finish in time.
bool RunRemote(HANDLE proc, LPTHREAD_START_ROUTINE routine, LPVOID param,
               DWORD waitMs, DWORD* exitCode) {
    HANDLE t = CreateRemoteThread(proc, nullptr, 0, routine, param, 0, nullptr);
    if (!t) return false;
    const DWORD w = WaitForSingleObject(t, waitMs);
    bool done = false;
    if (w == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeThread(t, &code);
        if (exitCode) *exitCode = code;
        done = true;
    }
    CloseHandle(t);
    return done;
}

LPVOID PutRemote(HANDLE proc, const std::wstring& s) {
    const SIZE_T bytes = (s.size() + 1) * sizeof(wchar_t);
    LPVOID p = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return nullptr;
    if (!WriteProcessMemory(proc, p, s.c_str(), bytes, nullptr)) {
        VirtualFreeEx(proc, p, 0, MEM_RELEASE);
        return nullptr;
    }
    return p;
}

enum class Inject { Ok, NoAccess, Failed };

Inject InjectInto(DWORD pid, const std::wstring& dll) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        AWA_LOG(L"start menu styler: cannot open process %lu (error %lu), probably elevated",
                pid, GetLastError());
        return Inject::NoAccess;
    }
    // x64 on x64 Windows only. On ARM64 Windows this x64 ProWindows is emulated
    // and its LoadLibraryW address means nothing in a native host; a remote
    // thread started there would take it down.
    USHORT procMachine = 0, nativeMachine = 0;
    if (!IsWow64Process2(proc, &procMachine, &nativeMachine) ||
        procMachine != IMAGE_FILE_MACHINE_UNKNOWN || nativeMachine != IMAGE_FILE_MACHINE_AMD64) {
        CloseHandle(proc);
        return Inject::Failed;
    }

    // The offset of StylerStart inside the DLL, from a copy mapped here with no
    // code run (no DllMain, no imports resolved). The link stamp says the copy
    // in the host is this file: a DLL renamed aside and replaced while a host
    // still had it would put StylerStart somewhere else.
    ULONG_PTR startOffset = 0;
    DWORD stamp = 0;
    if (HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES)) {
        if (FARPROC f = GetProcAddress(local, "StylerStart"))
            startOffset = (ULONG_PTR)f - (ULONG_PTR)local;
        const auto* dos = (const IMAGE_DOS_HEADER*)local;
        stamp = ((const IMAGE_NT_HEADERS*)((const BYTE*)local + dos->e_lfanew))->FileHeader.TimeDateStamp;
        FreeLibrary(local);
    }
    if (!startOffset) {
        AWA_LOG(L"start menu styler: %s has no StylerStart", dll.c_str());
        CloseHandle(proc);
        return Inject::Failed;
    }

    Inject result = Inject::Failed;
    LPVOID remotePath = PutRemote(proc, dll);
    LPVOID remoteDir = PutRemote(proc, StartMenuStylerDir());
    auto loadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");

    bool loadDone = false;
    DWORD code = 0;
    if (remotePath && remoteDir && loadLibrary)
        loadDone = RunRemote(proc, loadLibrary, remotePath, kLoadWaitMs, &code);

    if (loadDone && code != 0) {
        const ULONG_PTR base = RemoteModuleBase(pid, dll);
        IMAGE_DOS_HEADER dos = {};
        IMAGE_NT_HEADERS nt = {};
        const bool same = base &&
            ReadProcessMemory(proc, (LPCVOID)base, &dos, sizeof(dos), nullptr) &&
            ReadProcessMemory(proc, (LPCVOID)(base + dos.e_lfanew), &nt, sizeof(nt), nullptr) &&
            nt.FileHeader.TimeDateStamp == stamp;
        if (base && !same)
            AWA_LOG(L"start menu styler: process %lu holds an older %s; sign out or end it", pid, kDllName);
        if (same) {
            DWORD started = 0;
            const bool startDone = RunRemote(proc, (LPTHREAD_START_ROUTINE)(base + startOffset),
                                             remoteDir, kStartWaitMs, &started);
            if (startDone && started == 1) result = Inject::Ok;
            else AWA_LOG(L"start menu styler: StylerStart in %lu %s (%lu)", pid,
                         startDone ? L"failed" : L"did not finish", started);
            if (!startDone) remoteDir = nullptr;   // may still be running: do not free under it
        }
    } else {
        // Most often a package that lost the race for the ACL, or a suspended host.
        AWA_LOG(L"start menu styler: LoadLibrary in %lu %s", pid,
                loadDone ? L"failed (is the DLL readable by the app packages?)" : L"did not finish");
        if (!loadDone) remotePath = nullptr;
    }

    if (remotePath) VirtualFreeEx(proc, remotePath, 0, MEM_RELEASE);
    if (remoteDir) VirtualFreeEx(proc, remoteDir, 0, MEM_RELEASE);
    CloseHandle(proc);
    return result;
}

// ---------------------------------------------------------------- worker
void SetStatus(const std::wstring& s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status = s;
}

void StopAll() {
    std::vector<DWORD> pids;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        pids.assign(g_styled.begin(), g_styled.end());
        g_styled.clear();
        g_failed.clear();
        ForgetInjected();   // a host that goes after this was not hurt by us
    }
    // Also any process styled by an earlier ProWindows that left it behind.
    for (DWORD pid : HostPids())
        if (std::find(pids.begin(), pids.end(), pid) == pids.end()) pids.push_back(pid);
    for (DWORD pid : pids) SignalStyler(L"stop", pid);
}

// Hosts that went away soon after we injected them. Two within the memory
// window trip the guard. Under g_mutex.
bool NoteGoneHosts(const std::vector<DWORD>& pids) {
    const ULONGLONG now = GetTickCount64();
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    for (auto it = g_injectedAt.begin(); it != g_injectedAt.end();) {
        HANDLE proc = it->second.proc;
        const bool gone = proc ? WaitForSingleObject(proc, 0) == WAIT_OBJECT_0
                               : std::find(pids.begin(), pids.end(), it->first) == pids.end();
        if (!gone) { ++it; continue; }
        ULONGLONG exitedAt = now;   // without a handle: when it was noticed
        FILETIME created, exited, kernel, user;
        if (proc && GetProcessTimes(proc, &created, &exited, &kernel, &user)) {
            const ULONGLONG a = ((ULONGLONG)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
            const ULONGLONG b = ((ULONGLONG)exited.dwHighDateTime << 32) | exited.dwLowDateTime;
            const ULONGLONG ago = a > b ? (a - b) / 10000 : 0;
            exitedAt = now - std::min(ago, now);
        }
        if (proc) CloseHandle(proc);
        if (exitedAt - std::min(exitedAt, it->second.at) < kCrashWindowMs) g_earlyExits.push_back(exitedAt);
        g_styled.erase(it->first);
        g_failed.erase(it->first);
        it = g_injectedAt.erase(it);
    }
    for (auto it = g_styled.begin(); it != g_styled.end();)
        it = std::find(pids.begin(), pids.end(), *it) == pids.end() ? g_styled.erase(it) : std::next(it);
    for (auto it = g_failed.begin(); it != g_failed.end();)
        it = std::find(pids.begin(), pids.end(), it->first) == pids.end() ? g_failed.erase(it) : std::next(it);
    // Two early exits count when they were close to each other, however late
    // they were noticed.
    std::sort(g_earlyExits.begin(), g_earlyExits.end());
    if (!g_earlyExits.empty()) {
        const ULONGLONG last = g_earlyExits.back();
        g_earlyExits.erase(std::remove_if(g_earlyExits.begin(), g_earlyExits.end(),
                                          [&](ULONGLONG t) { return last - t > kCrashMemoryMs; }),
                           g_earlyExits.end());
    }
    return g_earlyExits.size() >= 2;
}

void Sync(bool reload) {
    Wanted w;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        w = g_wanted;
    }
    if (!w.enabled) {
        StopAll();
        SetStatus(L"Off");
        return;
    }

    const bool changed = WriteIni(w);
    const std::wstring dll = DllPath();
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        AWA_LOG(L"start menu styler: %s is missing", dll.c_str());
        SetStatus(L"ProWindows_startmenu.dll is missing; rebuild with build.bat");
        return;
    }

    const std::vector<DWORD> pids = HostPids();
    bool tripped;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        tripped = NoteGoneHosts(pids);
        if (tripped) {
            g_earlyExits.clear();
            g_status = L"Turned off: the Start menu kept restarting";
        } else if (reload) {
            g_failed.clear();   // an explicit Reload is also "try again"
        }
    }
    if (tripped) {
        AWA_LOG(L"start menu styler: the Start menu hosts exited twice right after an injection");
        if (g_notify && !g_quitting) PostMessageW(g_notify, WM_AWA_STYLERCRASH, 0, 0);
        return;
    }

    int denied = 0, failed = 0;
    bool granted = false;
    for (DWORD pid : pids) {
        if (g_quitting) break;
        if (IsStyled(pid)) {
            if (changed || reload) SignalStyler(L"reload", pid);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_styled.insert(pid);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_styled.erase(pid);
            const auto f = g_failed.find(pid);
            if (f != g_failed.end() && GetTickCount64() - f->second < kRetryMs) { ++failed; continue; }
        }
        if (!granted) {
            // The package reads the DLL and the folder; without these the host
            // cannot even load it. Done again for every injection, because a
            // rebuilt DLL is a new file.
            granted = EnsureDir() && GrantPackages(dll, false);
            if (!granted) { SetStatus(L"Could not give the Start menu access to ProWindows' files; see the log"); return; }
        }
        CleanSignals(pid);
        // Stamped before, not after: the crash the guard is for happens while
        // StylerStart runs, and then the injection never reports Ok.
        {
            HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            std::lock_guard<std::mutex> lock(g_mutex);
            Injected& e = g_injectedAt[pid];
            if (e.proc) CloseHandle(e.proc);
            e = { GetTickCount64(), proc };
        }
        const Inject r = InjectInto(pid, dll);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (r == Inject::Ok) {
            g_styled.insert(pid);
            g_failed.erase(pid);
        } else {
            g_failed[pid] = GetTickCount64();
            if (r == Inject::NoAccess) ++denied; else ++failed;
            // Not opened, so nothing ran in it: its exit is not ours.
            if (r == Inject::NoAccess) {
                if (HANDLE p = g_injectedAt[pid].proc) CloseHandle(p);
                g_injectedAt.erase(pid);
            }
        }
    }

    if (g_quitting) {
        // ProWindows is on its way out and has already told the others to stop.
        std::lock_guard<std::mutex> lock(g_mutex);
        for (DWORD pid : g_styled) SignalStyler(L"stop", pid);
        g_styled.clear();
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    const size_t n = g_styled.size();
    if (n) {
        g_status = std::to_wstring(n) + (n == 1 ? L" Start menu process styled" : L" Start menu processes styled");
        if (denied) g_status += L" (" + std::to_wstring(denied) + L" could not be opened)";
    } else if (denied) {
        g_status = L"ProWindows cannot open the Start menu (is it running as administrator?)";
    } else if (failed) {
        g_status = L"Could not style the Start menu; set debug = true in config.ini and read startmenu-styler\\startmenu-styler.log";
    } else {
        g_status = L"Waiting for the Start menu";
    }
}

DWORD WINAPI WorkerProc(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        for (;;) {
            const unsigned bits = g_requests.exchange(0);
            if (!bits) break;
            if (bits & REQ_QUIT) return 0;
            if (bits & REQ_STOP) {
                StopAll();
                SetStatus(L"Off");
            }
            if (bits & (REQ_SYNC | REQ_RELOAD)) {
                // A host that has only just restarted is not ready for hooks.
                if (bits & REQ_DELAYED) Sleep(1500);
                Sync((bits & REQ_RELOAD) != 0);
            }
        }
    }
}

Wanted Snapshot(const Config& c) {
    Wanted w;
    w.enabled   = c.startStyler;
    w.theme     = c.startTheme;
    w.layout    = c.startLayout;
    w.tint      = c.startTint;
    w.opacity   = c.startTintOpacity;
    w.highlight = c.startHighlight;
    w.radius    = c.startRadius;
    w.text      = c.startText;
    w.debug     = c.debug;
    return w;
}

} // namespace

// ================================================================ public
void StartMenuStylerInit(Config* cfg, HWND notify) {
    g_cfg = cfg;
    g_notify = notify;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    StartMenuStylerApplyConfig();
}

void StartMenuStylerApplyConfig() {
    if (!g_cfg || !g_thread) return;
    const Wanted w = Snapshot(*g_cfg);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_wanted = w;
        if (!w.enabled) g_status = L"Off";
    }
    Request(w.enabled ? REQ_SYNC : REQ_STOP);
}

void StartMenuStylerTaskbarCreated() {
    if (!g_cfg || !g_cfg->startStyler || !g_thread) return;
    Request(REQ_SYNC | REQ_DELAYED);
}

void StartMenuStylerCrashTripped() {
    if (!g_cfg || !g_cfg->startStyler) return;
    AWA_LOG(L"start menu styler: turning Start menu styling off");
    g_cfg->startStyler = false;
    AppSaveConfig();
    AppRefreshSettings();
    StartMenuStylerApplyConfig();
    AppTrayBalloon(L"ProWindows",
                   L"Start menu styling was turned off because the Start menu kept restarting.");
}

void StartMenuStylerForeground(HWND hwnd) {
    if (!g_cfg || !g_cfg->startStyler || !g_thread || !hwnd) return;
    // The Start menu and the search flyout are CoreWindows of their hosts. Every
    // UWP app has one, so the process is looked at only if it is not known yet.
    wchar_t cls[40];
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) == 0 || wcscmp(cls, L"Windows.UI.Core.CoreWindow") != 0) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_styled.count(pid)) return;
        const auto f = g_failed.find(pid);
        if (f != g_failed.end() && GetTickCount64() - f->second < kRetryMs) return;
    }
    if (!IsHostPid(pid)) return;
    Request(REQ_SYNC);
}

void StartMenuStylerReload() {
    if (!g_cfg || !g_thread) return;
    Request(g_cfg->startStyler ? (REQ_SYNC | REQ_RELOAD) : 0);
}

void StartMenuStylerShutdown() {
    g_quitting = true;
    if (!g_thread) return;
    // Straight to the signal files: making one does not block, and the host
    // undoes the styling on its own thread from there.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (DWORD pid : g_styled) SignalStyler(L"stop", pid);
    }
    for (DWORD pid : HostPids()) SignalStyler(L"stop", pid);
    Request(REQ_QUIT);
    // An injection in flight finishes and is stopped by the worker itself
    // (Sync's g_quitting branch) - but only if this process is still there.
    // Bounded: StylerStart normally returns well inside a second.
    WaitForSingleObject(g_thread, 5000);
}

std::wstring StartMenuStylerStatus() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

std::wstring StartMenuStylerDir() {
    return ConfigDir() + L"\\startmenu-styler";
}

std::wstring StartMenuStylerIniPath() {
    return StartMenuStylerDir() + L"\\startmenu-styler.ini";
}

bool StartMenuStylerWriteIni(const Config& c) {
    Wanted w = Snapshot(c);
    w.enabled = true;
    return WriteIni(w);
}

bool StartMenuStylerGrantPackages(const std::wstring& path, bool isDir) {
    return GrantPackages(path, isDir);
}

void StartMenuStylerEnsureIni() {
    Wanted w;
    if (g_cfg) w = Snapshot(*g_cfg);
    EnsureDir();
    if (GetFileAttributesW(StartMenuStylerIniPath().c_str()) == INVALID_FILE_ATTRIBUTES)
        WriteIni(w);
}

// ---------------------------------------------------------------- choices
int StartThemeCount() { return (int)ARRAYSIZE(kThemes); }
const wchar_t* StartThemeId(int i) { return kThemes[std::clamp(i, 0, StartThemeCount() - 1)].id; }
const wchar_t* StartThemeName(int i) { return kThemes[std::clamp(i, 0, StartThemeCount() - 1)].name; }
int StartThemeIndexOf(const std::wstring& id) {
    for (int i = 0; i < StartThemeCount(); ++i) if (id == kThemes[i].id) return i;
    return -1;
}
bool StartThemeIsProWindows(const std::wstring& id) {
    return id == L"ProWindows" || id == L"ProWindows Glass";
}

int StartLayoutCount() { return (int)ARRAYSIZE(kLayouts); }
const wchar_t* StartLayoutId(int i) { return kLayouts[std::clamp(i, 0, StartLayoutCount() - 1)].id; }
const wchar_t* StartLayoutName(int i) { return kLayouts[std::clamp(i, 0, StartLayoutCount() - 1)].name; }
int StartLayoutIndexOf(const std::wstring& id) {
    for (int i = 0; i < StartLayoutCount(); ++i) if (id == kLayouts[i].id) return i;
    return -1;
}

int StartTintCount() { return (int)ARRAYSIZE(kTintNames); }
const wchar_t* StartTintName(int i) { return kTintNames[std::clamp(i, 0, StartTintCount() - 1)]; }
COLORREF StartTintColor(int i) {
    const unsigned c = TintRgb(std::clamp(i, 0, StartTintCount() - 1));
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

} // namespace awa
