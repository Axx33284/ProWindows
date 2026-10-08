// ProWindows - File Explorer styling, the ProWindows side. See explorerstyler.h.
//
// How it is put into Explorer: LoadLibraryW is run in explorer.exe by a remote
// thread, then the DLL's exported StylerStart is run by a second one. The DLL
// then owns itself: it waits for two named events, "Local\ProWindows.Styler.
// Reload.<pid>" (re-read the ini) and "...Stop.<pid>" (undo everything; the module
// stays loaded, idle - MAP 93). The Stop event existing is how this side knows a process is styled.
#include "explorerstyler.h"
#include "app.h"

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>

namespace awa {

namespace {

constexpr wchar_t kDllName[] = L"ProWindows_explorer.dll";
constexpr DWORD   kLoadWaitMs  = 15000;
constexpr DWORD   kStartWaitMs = 30000;   // StylerStart talks to Explorer's windows
constexpr DWORD   kCrashWindowMs = 90 * 1000;

// ---------------------------------------------------------------- choices
struct Choice { const wchar_t* id; const wchar_t* name; };

// The ids are the theme names the mod's GetSelectedTheme compares against.
const Choice kThemes[] = {
    { L"ProWindows",                         L"ProWindows" },
    { L"ProWindows Glass",                   L"ProWindows Glass" },
    { L"",                                   L"None (my own styles only)" },
    { L"Translucent Explorer11",             L"Translucent Explorer11" },
    { L"MicaBar",                            L"MicaBar" },
    { L"NoCommandBar",                       L"NoCommandBar" },
    { L"Minimal Explorer11",                 L"Minimal Explorer11" },
    { L"Tabless",                            L"Tabless" },
    { L"Matter",                             L"Matter" },
    { L"WindowGlass",                        L"WindowGlass" },
    { L"AddressSearchOnly",                  L"AddressSearchOnly" },
    { L"TintedGlass",                        L"TintedGlass" },
    { L"LiquidGlass",                        L"LiquidGlass" },
    { L"MicaTabless",                        L"MicaTabless" },
    { L"OS26 Liquid Glass",                  L"OS26 Liquid Glass" },
    { L"OS26 Liquid Glass_variant_Compact",  L"OS26 Liquid Glass (Compact)" },
    { L"ZEUSosX_044",                        L"ZEUSosX_044" },
    { L"Compact Explorer11",                 L"Compact Explorer11" },
    { L"Float",                              L"Float" },
};

// config.ini value, display name, the mod's own setting value.
struct EffectChoice { const wchar_t* id; const wchar_t* name; const wchar_t* mod; };
const EffectChoice kEffects[] = {
    { L"",        L"Theme default", L"" },
    { L"blur",    L"Blur",          L"acrylicblur" },
    { L"acrylic", L"Acrylic",       L"acrylic" },
    { L"mica",    L"Mica",          L"mica" },
    { L"micaalt", L"Mica Alt",      L"micaAlt" },
    { L"none",    L"None",          L"none" },
};

// ---------------------------------------------------------------- state
Config* g_cfg = nullptr;       // UI thread only

// What the worker is told. Copied out of the config on the UI thread so the
// worker never reads it.
struct Wanted {
    bool enabled = false;
    std::wstring theme;
    std::wstring effect;
    bool debug = false;
    ExplorerLook look;
};

std::mutex g_mutex;            // guards everything below
Wanted g_wanted;
std::wstring g_status = L"Not started";
std::set<DWORD> g_styled;      // explorer pids known to carry the styler
std::set<DWORD> g_failed;      // ...that could not be opened or started

enum : unsigned { REQ_SYNC = 1, REQ_RELOAD = 2, REQ_STOP = 4, REQ_QUIT = 8, REQ_DELAYED = 16 };
std::atomic<unsigned> g_requests{0};
HANDLE g_wake = nullptr;
HANDLE g_thread = nullptr;
std::atomic<bool> g_quitting{false};

// Crash guard. Written by the worker (an injection happened), read on the UI
// thread (Explorer restarted).
std::atomic<ULONGLONG> g_lastInjectTick{0};
int g_restartsSoon = 0;        // UI thread only

void Request(unsigned bits) {
    g_requests.fetch_or(bits);
    if (g_wake) SetEvent(g_wake);
}

// ---------------------------------------------------------------- paths
std::wstring DllPath() {
    std::wstring exe = ExePath();
    size_t slash = exe.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash + 1)) + kDllName;
}

std::wstring EventName(const wchar_t* what, DWORD pid) {
    return std::wstring(L"Local\\ProWindows.Styler.") + what + L"." + std::to_wstring(pid);
}

// Sets one of the DLL's events; false when it does not exist (not styled).
bool SignalStyler(const wchar_t* what, DWORD pid) {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, EventName(what, pid).c_str());
    if (!ev) return false;
    SetEvent(ev);
    CloseHandle(ev);
    return true;
}

// ---------------------------------------------------------------- the ini
// The part of the ini ProWindows owns is a marked block at the top; everything
// after it (custom styles, anything the user adds) is theirs and is kept. A
// later line wins in the DLL's reader, so a user line can override a managed one.
const wchar_t kBlockBegin[] = L"; >>> managed by ProWindows (Settings > Explorer). Rewritten on every Apply.";
const wchar_t kBlockEnd[]   = L"; <<< end of the managed part. Your own lines go below.";

const wchar_t kTemplate[] =
    L"; Custom File Explorer styles for ProWindows. Each style is a target (a XAML\r\n"
    L"; control, optionally with #Name, [Property=Value] or Parent > Child) and the\r\n"
    L"; properties to set on it. Remove the leading ';' to switch an example on, then\r\n"
    L"; press Reload on the Explorer page. Numbers must run on without gaps.\r\n"
    L";\r\n"
    L"; Example: a red command bar.\r\n"
    L"; controlStyles[0].target=Grid#CommandBarControlRootGrid\r\n"
    L"; controlStyles[0].styles[0]=Background=#8B0000\r\n"
    L";\r\n"
    L"; Constants and resource variables work too:\r\n"
    L"; styleConstants[4]=myColor=#202020\r\n"
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

const wchar_t* ModEffect(const std::wstring& id) {
    for (const auto& e : kEffects) if (id == e.id) return e.mod;
    return L"";
}

// The system accent colour, as the shell stores it (ABGR); blue if unreadable.
COLORREF AccentColour() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                     RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return RGB(0, 120, 215);
    return RGB(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
}

COLORREF Lighten(COLORREF c, int percent) {
    auto f = [&](int x) { return x + (255 - x) * percent / 100; };
    return RGB(f(GetRValue(c)), f(GetGValue(c)), f(GetBValue(c)));
}

std::wstring Hex6(COLORREF c) {
    wchar_t b[16];
    swprintf_s(b, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return b;
}

std::wstring Gradient(COLORREF top, COLORREF bottom) {
    return L"<LinearGradientBrush StartPoint=\"0,0\" EndPoint=\"0,1\"><GradientStop Color=\"" +
           Hex6(top) + L"\" Offset=\"0\"/><GradientStop Color=\"" + Hex6(bottom) +
           L"\" Offset=\"1\"/></LinearGradientBrush>";
}

// The constants the ProWindows themes read (see styler.cpp), as styleConstants[N]
// lines, always the same four so the numbers a user continues from do not move.
std::wstring LookConstants(const Wanted& w) {
    const ExplorerLook& l = w.look;
    std::wstring fill = Hex6(l.tint);
    if (w.theme == L"ProWindows Glass") {
        const int a = std::clamp(l.tintOpacity, 0, 100) * 255 / 100;
        wchar_t b[16];
        swprintf_s(b, L"#%02X", a);
        fill = b + fill.substr(1);
    }
    std::wstring highlight;
    if (l.highlight == 1)      highlight = L"<SolidColorBrush Color=\"" + Hex6(AccentColour()) + L"\"/>";
    else if (l.highlight == 2) highlight = Gradient(Lighten(l.tint, 12), Lighten(l.tint, 28));
    else                       highlight = Gradient(RGB(0x1A, 0x1A, 0x1A), RGB(0x3A, 0x3A, 0x3A));
    const wchar_t* text = l.text == 1 ? L"#D0D0D0" : L"#FFFFFF";

    std::wstring out;
    out += L"\r\nstyleConstants[0]=pwFill=" + fill;
    out += L"\r\nstyleConstants[1]=pwHighlight=" + highlight;
    out += L"\r\nstyleConstants[2]=pwRadius=" + std::to_wstring(std::clamp(l.radius, 0, 12));
    out += L"\r\nstyleConstants[3]=pwText=" + std::wstring(text);
    return out;
}

// Rewrites the managed block. True when the file's content changed (so styled
// Explorers need telling).
bool WriteIni(const Wanted& w) {
    const std::wstring path = ExplorerStylerIniPath();
    std::wstring old = ReadFileUtf8(path);

    std::wstring block = kBlockBegin;
    block += L"\r\ntheme=" + w.theme;
    block += L"\r\nbackgroundTranslucentEffect=" + std::wstring(ModEffect(w.effect));
    block += w.look.region == 1 ? L"\r\nbackgroundTranslucentEffectRegion=explorerFrame"
                                : L"\r\nbackgroundTranslucentEffectRegion=entireWindow";
    // Other programs using the XAML diagnostics channel the styling needs: the
    // default is to block them, as a prompt inside Explorer is not something
    // ProWindows can answer.
    block += w.look.xaml == 0 ? L"\r\nxamlDiagnosticsHandling=alert"
           : w.look.xaml == 2 ? L"\r\nxamlDiagnosticsHandling=allow"
                              : L"\r\nxamlDiagnosticsHandling=block";
    block += LookConstants(w);
    block += w.debug ? L"\r\ndebug=1" : L"\r\ndebug=0";
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
    CreateDirectoryW(ConfigDir().c_str(), nullptr);
    if (!WriteFileUtf8(path, fresh)) {
        AWA_LOG(L"explorer styler: could not write %s", path.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- processes
std::vector<DWORD> ExplorerPids() {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"explorer.exe") != 0) continue;
        DWORD s = 0;
        if (ProcessIdToSessionId(pe.th32ProcessID, &s) && s == session)
            out.push_back(pe.th32ProcessID);
    }
    CloseHandle(snap);
    return out;
}

// Where the DLL at `path` sits in `proc`, 0 if it is not loaded. By full path:
// a ProWindows_explorer.dll from another ProWindows folder can be loaded (and,
// since the DLL is never unloaded, stay loaded) in the same Explorer.
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
        AWA_LOG(L"explorer styler: cannot open explorer.exe %lu (error %lu), probably elevated",
                pid, GetLastError());
        return Inject::NoAccess;
    }
    // x64 Explorer on x64 Windows only. On ARM64 Windows this x64 ProWindows
    // is emulated and its LoadLibraryW address means nothing in a native
    // Explorer; a remote thread started there would take Explorer down.
    USHORT procMachine = 0, nativeMachine = 0;
    if (!IsWow64Process2(proc, &procMachine, &nativeMachine) ||
        procMachine != IMAGE_FILE_MACHINE_UNKNOWN || nativeMachine != IMAGE_FILE_MACHINE_AMD64) {
        CloseHandle(proc);
        return Inject::Failed;
    }

    // The offset of StylerStart inside the DLL, from a copy mapped here with no
    // code run (no DllMain, no imports resolved).
    // The link stamp says the copy in Explorer is this file: a DLL renamed
    // aside and replaced while Explorer still had it would put StylerStart
    // somewhere else.
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
        AWA_LOG(L"explorer styler: %s has no StylerStart", dll.c_str());
        CloseHandle(proc);
        return Inject::Failed;
    }

    Inject result = Inject::Failed;
    LPVOID remotePath = PutRemote(proc, dll);
    LPVOID remoteDir = PutRemote(proc, ConfigDir());
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
            AWA_LOG(L"explorer styler: Explorer %lu holds an older %s; restart Explorer", pid, kDllName);
        if (same) {
            DWORD started = 0;
            const bool startDone = RunRemote(proc, (LPTHREAD_START_ROUTINE)(base + startOffset),
                                             remoteDir, kStartWaitMs, &started);
            if (startDone && started == 1) result = Inject::Ok;
            else AWA_LOG(L"explorer styler: StylerStart in %lu %s (%lu)", pid,
                         startDone ? L"failed" : L"did not finish", started);
            if (!startDone) remoteDir = nullptr;   // may still be running: do not free under it
        }
    } else {
        AWA_LOG(L"explorer styler: LoadLibrary in %lu %s", pid,
                loadDone ? L"failed" : L"did not finish");
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
    }
    // Also any process styled by an earlier ProWindows that left it behind.
    for (DWORD pid : ExplorerPids())
        if (std::find(pids.begin(), pids.end(), pid) == pids.end()) pids.push_back(pid);
    for (DWORD pid : pids) SignalStyler(L"Stop", pid);
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

    if (reload) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_failed.clear();   // an explicit Reload is also "try again"
    }
    const bool changed = WriteIni(w);
    const std::wstring dll = DllPath();
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        AWA_LOG(L"explorer styler: %s is missing", dll.c_str());
        SetStatus(L"ProWindows_explorer.dll is missing; rebuild with build.bat");
        return;
    }

    const std::vector<DWORD> pids = ExplorerPids();
    {   // forget processes that are gone
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_styled.begin(); it != g_styled.end();)
            it = std::find(pids.begin(), pids.end(), *it) == pids.end() ? g_styled.erase(it) : std::next(it);
        for (auto it = g_failed.begin(); it != g_failed.end();)
            it = std::find(pids.begin(), pids.end(), *it) == pids.end() ? g_failed.erase(it) : std::next(it);
    }

    int denied = 0, failed = 0;
    for (DWORD pid : pids) {
        if (g_quitting) break;
        if (changed || reload) {
            if (SignalStyler(L"Reload", pid)) {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_styled.insert(pid);
                continue;
            }
        } else {
            // Already carrying the styler (this run or an earlier one)?
            HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, EventName(L"Stop", pid).c_str());
            if (ev) {
                CloseHandle(ev);
                std::lock_guard<std::mutex> lock(g_mutex);
                g_styled.insert(pid);
                continue;
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_failed.count(pid)) { ++failed; continue; }
        }
        // Stamped before, not after: the crash the guard is for happens while
        // StylerStart runs, and then the injection never reports Ok.
        g_lastInjectTick = GetTickCount64();
        const Inject r = InjectInto(pid, dll);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (r == Inject::Ok) {
            g_styled.insert(pid);
        } else {
            g_failed.insert(pid);
            if (r == Inject::NoAccess) ++denied; else ++failed;
        }
    }

    if (g_quitting) {
        // ProWindows is on its way out and has already told the others to stop.
        std::lock_guard<std::mutex> lock(g_mutex);
        for (DWORD pid : g_styled) SignalStyler(L"Stop", pid);
        g_styled.clear();
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    const size_t n = g_styled.size();
    if (n) {
        g_status = std::to_wstring(n) + (n == 1 ? L" Explorer process styled" : L" Explorer processes styled");
        if (denied) g_status += L" (" + std::to_wstring(denied) + L" elevated, skipped)";
    } else if (denied) {
        g_status = L"Explorer runs elevated; ProWindows cannot style it";
    } else if (failed) {
        g_status = L"Could not style Explorer; set debug = true in config.ini and read explorer-styler.log";
    } else {
        g_status = L"Waiting for Explorer";
    }
}

// A reload makes the styler drop its XAML watcher and attach a new one, which
// it does from a thread of its own. A second reload while that thread is still
// attaching crashed Explorer inside Microsoft.UI.Xaml.dll - clicking through
// the theme choice in Settings does exactly that, an Apply every half second.
// So a reload waits until the requests have been quiet for this long and then
// goes out once. Stop and quit are not held up.
constexpr DWORD kSettleMs = 1500;

void WaitForQuiet() {
    while ((g_requests.load() & (REQ_SYNC | REQ_RELOAD)) &&
           !(g_requests.load() & (REQ_STOP | REQ_QUIT)) &&
           WaitForSingleObject(g_wake, kSettleMs) == WAIT_OBJECT_0) {
    }
}

DWORD WINAPI WorkerProc(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        for (;;) {
            WaitForQuiet();
            const unsigned bits = g_requests.exchange(0);
            if (!bits) break;
            if (bits & REQ_QUIT) return 0;
            if (bits & REQ_STOP) {
                StopAll();
                SetStatus(L"Off");
            }
            if (bits & (REQ_SYNC | REQ_RELOAD)) {
                // Explorer that has only just started is not ready for hooks.
                if (bits & REQ_DELAYED) Sleep(1500);
                Sync((bits & REQ_RELOAD) != 0);
            }
        }
    }
}

Wanted Snapshot(const Config& c) {
    Wanted w;
    w.enabled = c.explorerStyler;
    w.theme   = c.explorerTheme;
    w.effect  = c.explorerEffect;
    w.debug   = c.debug;
    w.look.tint        = c.explorerTint;
    w.look.tintOpacity = c.explorerTintOpacity;
    w.look.highlight   = c.explorerHighlight;
    w.look.radius      = c.explorerRadius;
    w.look.text        = c.explorerText;
    w.look.region      = c.explorerRegion;
    w.look.xaml        = c.explorerXamlDiag;
    return w;
}

} // namespace

// ================================================================ public
void ExplorerStylerInit(Config* cfg) {
    g_cfg = cfg;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    ExplorerStylerApplyConfig();
}

void ExplorerStylerApplyConfig() {
    if (!g_cfg || !g_thread) return;
    const Wanted w = Snapshot(*g_cfg);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_wanted = w;
        if (!w.enabled) g_status = L"Off";
    }
    Request(w.enabled ? REQ_SYNC : REQ_STOP);
}

void ExplorerStylerTaskbarCreated() {
    if (!g_cfg || !g_cfg->explorerStyler) return;

    // Explorer restarting is normal (it is restarted from Task Manager, after a
    // shell crash). Restarting again and again right after the styler went in
    // is not: that is the styler's doing, and the way out is to leave Explorer
    // alone.
    const ULONGLONG injected = g_lastInjectTick.load();
    if (injected && GetTickCount64() - injected < kCrashWindowMs) ++g_restartsSoon;
    else g_restartsSoon = 0;

    if (g_restartsSoon >= 2) {
        AWA_LOG(L"explorer styler: Explorer restarted %d times within %d s of an injection; "
                L"turning File Explorer styling off", g_restartsSoon, (int)(kCrashWindowMs / 1000));
        g_cfg->explorerStyler = false;
        g_restartsSoon = 0;
        AppSaveConfig();
        AppRefreshSettings();
        ExplorerStylerApplyConfig();
        AppTrayBalloon(L"ProWindows",
                       L"File Explorer styling was turned off because Explorer kept restarting.");
        return;
    }
    Request(REQ_SYNC | REQ_DELAYED);
}

void ExplorerStylerForeground(HWND hwnd) {
    if (!g_cfg || !g_cfg->explorerStyler || !g_thread || !hwnd) return;
    wchar_t cls[24];
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) == 0 || wcscmp(cls, L"CabinetWClass") != 0) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_styled.count(pid) || g_failed.count(pid)) return;
    }
    Request(REQ_SYNC);
}

void ExplorerStylerReload() {
    if (!g_cfg || !g_thread) return;
    Request(g_cfg->explorerStyler ? (REQ_SYNC | REQ_RELOAD) : 0);
}

void ExplorerStylerShutdown() {
    g_quitting = true;
    if (!g_thread) return;
    // Straight to the events: opening and setting one does not block, and
    // Explorer undoes the styling on its own thread from there.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (DWORD pid : g_styled) SignalStyler(L"Stop", pid);
    }
    for (DWORD pid : ExplorerPids()) SignalStyler(L"Stop", pid);
    Request(REQ_QUIT);
    // An injection in flight finishes and is stopped by the worker itself
    // (Sync's g_quitting branch) - but only if this process is still there.
    // Bounded: StylerStart normally returns well inside a second.
    WaitForSingleObject(g_thread, 5000);
}

std::wstring ExplorerStylerStatus() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

std::wstring ExplorerStylerIniPath() {
    return ConfigDir() + L"\\explorer-styler.ini";
}

bool ExplorerStylerWriteIni(const std::wstring& theme, const std::wstring& effect, bool debug,
                            const ExplorerLook& look) {
    Wanted w;
    w.enabled = true;
    w.theme   = theme;
    w.effect  = effect;
    w.debug   = debug;
    w.look    = look;
    return WriteIni(w);
}

bool ExplorerThemeIsProWindows(const std::wstring& id) {
    return id == L"ProWindows" || id == L"ProWindows Glass";
}

void ExplorerStylerEnsureIni() {
    Wanted w;
    if (g_cfg) w = Snapshot(*g_cfg);
    if (GetFileAttributesW(ExplorerStylerIniPath().c_str()) == INVALID_FILE_ATTRIBUTES)
        WriteIni(w);
}

// ---------------------------------------------------------------- choices
int ExplorerThemeCount() { return (int)ARRAYSIZE(kThemes); }
const wchar_t* ExplorerThemeId(int i) { return kThemes[std::clamp(i, 0, ExplorerThemeCount() - 1)].id; }
const wchar_t* ExplorerThemeName(int i) { return kThemes[std::clamp(i, 0, ExplorerThemeCount() - 1)].name; }
int ExplorerThemeIndexOf(const std::wstring& id) {
    for (int i = 0; i < ExplorerThemeCount(); ++i) if (id == kThemes[i].id) return i;
    return -1;
}

int ExplorerEffectCount() { return (int)ARRAYSIZE(kEffects); }
const wchar_t* ExplorerEffectId(int i) { return kEffects[std::clamp(i, 0, ExplorerEffectCount() - 1)].id; }
const wchar_t* ExplorerEffectName(int i) { return kEffects[std::clamp(i, 0, ExplorerEffectCount() - 1)].name; }
int ExplorerEffectIndexOf(const std::wstring& id) {
    for (int i = 0; i < ExplorerEffectCount(); ++i) if (id == kEffects[i].id) return i;
    return -1;
}

} // namespace awa
