// The Windhawk API over ProWindows, and the DLL's life inside the process it
// styles (explorer.exe, or the Start menu's StartMenuExperienceHost.exe and
// SearchHost.exe - one source, two DLLs, see windhawk_shim.h).
//
// Everything here runs in somebody else's process, so it is deliberately plain:
// no ProWindows headers, no static constructors that do work, and DllMain does
// nothing. ProWindows injects the DLL with LoadLibraryW and then runs
// StylerStart on a remote thread; a worker thread started there waits for the
// Reload and Stop signals and is the only thing that ever tears the mod down.
// The signals are named events in Explorer; in the Start menu build
// (PW_STYLER_PACKAGED) they are files - see "lifecycle" below for why.

#include "windhawk_shim.h"

#include <shlobj.h>
#include <winhttp.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"

namespace {

std::wstring g_configDir;  // where the ini and the log live: ...\ProWindows for
                           // Explorer, ...\ProWindows\startmenu-styler for the Start menu
std::mutex g_settingsMutex;
std::unordered_map<std::wstring, std::wstring> g_settings;
std::atomic<bool> g_debug;

// ---------------------------------------------------------------- paths
std::wstring DefaultConfigDir() {
    PWSTR roaming = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr,
                                       &roaming)) &&
        roaming) {
        dir = roaming;
        dir += L"\\ProWindows";
#ifdef PW_STYLER_PACKAGED
        // Only a fallback: inside an AppContainer this is the package's own
        // copy of %APPDATA%, so ProWindows always passes the real path.
        dir += L"\\" PW_STYLER L"-styler";
#endif
    }
    CoTaskMemFree(roaming);
    return dir;
}

std::wstring IniPath() {
    return g_configDir + L"\\" PW_STYLER L"-styler.ini";
}

// ---------------------------------------------------------------- the ini
// key=value lines, the mod's own setting names as keys ("theme",
// "controlStyles[0].target", "controlStyles[0].styles[1]"...). Values are
// taken verbatim after the first '=' (styles contain '=' and quotes, so
// GetPrivateProfileString, which strips quotes, is not usable). Lines that
// start with ';' or '#' are comments, and so are lines without an '='.
std::wstring ReadWholeFile(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ |
                           FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string bytes;
    char buf[8192];
    DWORD got;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) {
        bytes.append(buf, got);
        if (bytes.size() > (4u << 20)) {
            break;  // not a settings file
        }
    }
    CloseHandle(f);
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        bytes.erase(0, 3);
    }
    if (bytes.empty()) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(),
                                nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &out[0], n);
    return out;
}

void LoadIni() {
    std::unordered_map<std::wstring, std::wstring> parsed;
    std::wstring text = ReadWholeFile(IniPath());

    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) {
            end = text.size();
        }
        std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) {
            line.pop_back();
        }
        size_t first = line.find_first_not_of(L" \t");
        if (first == std::wstring::npos || line[first] == L';' ||
            line[first] == L'#') {
            continue;
        }
        size_t eq = line.find(L'=', first);
        if (eq == std::wstring::npos) {
            continue;
        }
        std::wstring key = line.substr(first, eq - first);
        while (!key.empty() && (key.back() == L' ' || key.back() == L'\t')) {
            key.pop_back();
        }
        std::wstring value = line.substr(eq + 1);
        // One space after '=' is formatting; the rest of the value is content.
        if (!value.empty() && value[0] == L' ') {
            value.erase(0, 1);
        }
        parsed[key] = value;
    }

    bool debug = false;
    auto it = parsed.find(L"debug");
    if (it != parsed.end()) {
        debug = it->second == L"1" || it->second == L"true";
    }

    std::lock_guard<std::mutex> lock(g_settingsMutex);
    g_settings.swap(parsed);
    g_debug = debug;
}

std::wstring FormatV(PCWSTR format, va_list args) {
    wchar_t stackBuf[512];
    va_list copy;
    va_copy(copy, args);
    int n = _vsnwprintf_s(stackBuf, _countof(stackBuf), _TRUNCATE, format, copy);
    va_end(copy);
    if (n >= 0) {
        return std::wstring(stackBuf, n);
    }
    std::vector<wchar_t> big(16384);
    n = _vsnwprintf_s(big.data(), big.size(), _TRUNCATE, format, args);
    return std::wstring(big.data(), n >= 0 ? n : big.size() - 1);
}

PCWSTR CopyString(const std::wstring& s) {
    wchar_t* out = new wchar_t[s.size() + 1];
    wmemcpy(out, s.c_str(), s.size() + 1);
    return out;
}

}  // namespace

// ================================================================ Windhawk API
void Wh_Log(PCWSTR format, ...) {
    if (!g_debug) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::wstring msg = FormatV(format, args);
    va_end(args);

    wchar_t head[64];
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(head, L"%02d:%02d:%02d.%03d [%lu:%lu] ", st.wHour, st.wMinute,
               st.wSecond, st.wMilliseconds, GetCurrentProcessId(),
               GetCurrentThreadId());
    std::wstring line = head + msg + L"\r\n";

    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string bytes(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &bytes[0], n,
                        nullptr, nullptr);

    HANDLE f = CreateFileW((g_configDir + L"\\" PW_STYLER L"-styler.log").c_str(),
                           FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wrote;
        WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr);
        CloseHandle(f);
    }
}

PCWSTR Wh_GetStringSetting(PCWSTR valueName, ...) {
    va_list args;
    va_start(args, valueName);
    std::wstring key = FormatV(valueName, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_settingsMutex);
    auto it = g_settings.find(key);
    return CopyString(it == g_settings.end() ? std::wstring() : it->second);
}

void Wh_FreeStringSetting(PCWSTR string) {
    delete[] string;
}

int Wh_GetIntSetting(PCWSTR valueName, ...) {
    va_list args;
    va_start(args, valueName);
    std::wstring key = FormatV(valueName, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_settingsMutex);
    auto it = g_settings.find(key);
    return it == g_settings.end() ? 0 : _wtoi(it->second.c_str());
}

namespace {

// The mod's private folder. Explorer: a subfolder of the config dir. Start
// menu: the config dir itself, which is already the one folder the packaged
// hosts may write to (ProWindows gave it the ACL).
std::wstring StorageDir() {
#ifdef PW_STYLER_PACKAGED
    return g_configDir;
#else
    return g_configDir + L"\\" PW_STYLER L"-styler";
#endif
}

// One file per stored value, "value_<key>.bin" in the storage folder (the key
// cut down to letters, digits and '_'). Raw bytes; an int is its four bytes.
// Separate files so that two host processes never rewrite each other's keys.
std::wstring ValuePath(PCWSTR name) {
    std::wstring file = L"\\value_";
    for (PCWSTR c = name; *c; ++c) {
        file += (iswalnum(*c) || *c == L'_') ? *c : L'_';
    }
    return StorageDir() + file + L".bin";
}

}  // namespace

BOOL Wh_GetModStoragePath(PWSTR pathBuffer, UINT bufferChars) {
    std::wstring dir = StorageDir();
    if (dir.size() + 1 > bufferChars) {
        return FALSE;
    }
#ifndef PW_STYLER_PACKAGED
    CreateDirectoryW(g_configDir.c_str(), nullptr);
#endif
    CreateDirectoryW(dir.c_str(), nullptr);
    wmemcpy(pathBuffer, dir.c_str(), dir.size() + 1);
    return TRUE;
}

size_t Wh_GetBinaryValue(PCWSTR valueName, void* buffer, size_t bufferSize) {
    HANDLE f = CreateFileW(ValuePath(valueName).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    if (buffer && bufferSize) {
        DWORD got = 0;
        ReadFile(f, buffer, (DWORD)(bufferSize < (1u << 28) ? bufferSize : (1u << 28)),
                 &got, nullptr);
    }
    CloseHandle(f);
    return (size_t)size.QuadPart;
}

BOOL Wh_SetBinaryValue(PCWSTR valueName, const void* buffer, size_t bufferSize) {
    CreateDirectoryW(StorageDir().c_str(), nullptr);
    HANDLE f = CreateFileW(ValuePath(valueName).c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    DWORD wrote = 0;
    BOOL ok = !bufferSize ||
              (WriteFile(f, buffer, (DWORD)bufferSize, &wrote, nullptr) && wrote == bufferSize);
    CloseHandle(f);
    return ok;
}

int Wh_GetIntValue(PCWSTR valueName, int defaultValue) {
    int value = 0;
    if (Wh_GetBinaryValue(valueName, &value, sizeof(value)) != sizeof(value)) {
        return defaultValue;
    }
    return value;
}

BOOL Wh_SetIntValue(PCWSTR valueName, int value) {
    return Wh_SetBinaryValue(valueName, &value, sizeof(value));
}

// ---------------------------------------------------------------- download
const WH_URL_CONTENT* Wh_GetUrlContent(PCWSTR url,
                                       const WH_GET_URL_CONTENT_OPTIONS* options) {
    PCWSTR targetFile = options && options->optionsSize >= sizeof(*options)
                            ? options->targetFilePath
                            : nullptr;

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        return nullptr;
    }

    HINTERNET session = WinHttpOpen(L"ProWindows", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return nullptr;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);

    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    HINTERNET request =
        connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr,
                                     WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     uc.nScheme == INTERNET_SCHEME_HTTPS
                                         ? WINHTTP_FLAG_SECURE
                                         : 0)
                : nullptr;

    WH_URL_CONTENT* result = nullptr;
    if (request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);

        HANDLE file = INVALID_HANDLE_VALUE;
        if (targetFile) {
            file = CreateFileW(targetFile, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }

        std::string body;
        bool ok = !targetFile || file != INVALID_HANDLE_VALUE;
        char buf[16384];
        DWORD got;
        while (ok && WinHttpReadData(request, buf, sizeof(buf), &got) && got) {
            if (file != INVALID_HANDLE_VALUE) {
                DWORD wrote;
                ok = WriteFile(file, buf, got, &wrote, nullptr) && wrote == got;
            } else {
                body.append(buf, got);
                ok = body.size() < (64u << 20);
            }
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }

        if (ok) {
            result = new WH_URL_CONTENT{};
            result->statusCode = (int)status;
            if (!body.empty()) {
                char* copy = new char[body.size() + 1];
                memcpy(copy, body.data(), body.size());
                copy[body.size()] = 0;
                result->data = copy;
                result->length = body.size();
            }
        }
    }

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return result;
}

void Wh_FreeUrlContent(const WH_URL_CONTENT* content) {
    if (content) {
        delete[] content->data;
        delete content;
    }
}

// ---------------------------------------------------------------- hooks
// Guards turning the hooks on and off, so that a Wh_ApplyHookOperations from an
// Explorer thread (the mod's LoadLibraryExW hook calls it) cannot switch them
// back on after Stop has taken them off.
std::mutex g_hookMutex;
bool g_hooksAllowed;

bool WhSetFunctionHook(void* target, void* hook, void** original) {
    if (!target) {
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, hook, original);
    if (st == MH_ERROR_ALREADY_CREATED) {
        // A restart in the same Explorer (styling off, then on): the hook and
        // its trampoline outlived the stop, and *original still points at it.
        return true;
    }
    if (st != MH_OK) {
        Wh_Log(L"MH_CreateHook(%p) failed: %S", target, MH_StatusToString(st));
        return false;
    }
    return true;
}

// Windhawk queues hook operations until this is called; MinHook does the same
// with CreateHook / EnableHook. Hooks that are already on are skipped.
BOOL Wh_ApplyHookOperations() {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (!g_hooksAllowed) {
        return FALSE;
    }
    MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        Wh_Log(L"MH_EnableHook failed: %S", MH_StatusToString(st));
        return FALSE;
    }
    return TRUE;
}

namespace {

void DisableAllHooks() {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    g_hooksAllowed = false;
    MH_DisableHook(MH_ALL_HOOKS);
}

}  // namespace

// ================================================================ lifecycle
//
// The DLL is never unloaded. Stop undoes the styling and takes the hooks off,
// but the module stays (pinned) until Explorer exits: a thread can be inside a
// hook body - CreateWindowExW_Hook waits in the original for the whole of
// WM_CREATE - and XAML keeps the TAP object, the visual tree watcher's
// delegates and the RunFromWindowThread hook procedure, all code of this
// module, for as long as it likes. No in-flight count covers all of that, and
// a stopped DLL costs nothing. MinHook stays initialised for the same reason:
// its trampolines are where a hook body returns through. Styling the same
// Explorer again re-runs the mod's init over the hooks MinHook already has.
namespace {

std::mutex g_lifeMutex;  // StylerStart against the stop in WorkerProc
bool g_running;          // under g_lifeMutex
HANDLE g_worker;

// ---------------------------------------------------------------- signals
// How ProWindows says "reload" and "stop", and how it can tell this process is
// styled. Explorer: the named events Local\ProWindows.Styler.{Reload,Stop}.<pid>.
//
// The Start menu hosts are packaged AppContainer processes, and there the
// events do not work: "Local\" inside a container is the container's own
// namespace (\Sessions\N\AppContainerNamedObjects\<package SID>), which a
// normal ProWindows cannot open by name; creating the events in the session's
// global namespace from inside the container is denied, and creating them from
// ProWindows first means guessing what a package may open. What both sides
// already have is the folder ProWindows made readable and writable for ALL
// APPLICATION PACKAGES (startmenu-styler\), so the signals are files there:
//   alive.<pid>   held open by the DLL, deleted by the kernel when it closes
//                 (also when the host dies): "this process is styled"
//   reload.<pid>  made by ProWindows; the worker deletes it and re-reads the ini
//   stop.<pid>    made by ProWindows; the worker deletes it and stops
// The worker sleeps on a directory change notification, never polls.
#ifdef PW_STYLER_PACKAGED

HANDLE g_aliveFile = INVALID_HANDLE_VALUE;
HANDLE g_watch = INVALID_HANDLE_VALUE;

std::wstring SignalPath(PCWSTR what) {
    return g_configDir + L"\\" + what + L"." + std::to_wstring(GetCurrentProcessId());
}

// True when the signal file existed (and is gone now).
bool TakeSignal(PCWSTR what) {
    const std::wstring path = SignalPath(what);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    if (!DeleteFileW(path.c_str())) {
        Wh_Log(L"cannot delete %s (error %lu)", path.c_str(), GetLastError());
    }
    return true;
}

bool OpenSignals() {
    // Left over from a run that ended without taking it (ProWindows made it
    // after the worker had already gone): not for us.
    DeleteFileW(SignalPath(L"stop").c_str());
    DeleteFileW(SignalPath(L"reload").c_str());
    g_aliveFile = CreateFileW(SignalPath(L"alive").c_str(), GENERIC_WRITE | DELETE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    g_watch = FindFirstChangeNotificationW(
        g_configDir.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (g_aliveFile == INVALID_HANDLE_VALUE || g_watch == INVALID_HANDLE_VALUE) {
        Wh_Log(L"cannot open the signal files in %s (error %lu)", g_configDir.c_str(),
               GetLastError());
        return false;
    }
    return true;
}

void CloseSignals() {
    if (g_watch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(g_watch);
    if (g_aliveFile != INVALID_HANDLE_VALUE) CloseHandle(g_aliveFile);  // deletes alive.<pid>
    g_watch = g_aliveFile = INVALID_HANDLE_VALUE;
}

// StylerStart on an already styled process: wake the worker with a reload.
void PostReload() {
    HANDLE f = CreateFileW(SignalPath(L"reload").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
}

// true = reload, false = stop.
bool WaitForSignal() {
    for (;;) {
        WaitForSingleObject(g_watch, INFINITE);
        FindNextChangeNotification(g_watch);   // before looking, so none is missed
        if (TakeSignal(L"stop")) return false;
        if (TakeSignal(L"reload")) return true;
    }
}

#else

HANDLE g_reloadEvent;
HANDLE g_stopEvent;

std::wstring EventName(PCWSTR what) {
    return std::wstring(L"Local\\ProWindows.Styler.") + what + L"." +
           std::to_wstring(GetCurrentProcessId());
}

bool OpenSignals() {
    g_reloadEvent =
        CreateEventW(nullptr, FALSE, FALSE, EventName(L"Reload").c_str());
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, EventName(L"Stop").c_str());
    if (!g_reloadEvent || !g_stopEvent) {
        if (g_reloadEvent) CloseHandle(g_reloadEvent);
        if (g_stopEvent) CloseHandle(g_stopEvent);
        g_reloadEvent = g_stopEvent = nullptr;
        return false;
    }
    // A Stop meant for an earlier run that ended on its own is not for us.
    ResetEvent(g_stopEvent);
    return true;
}

void CloseSignals() {
    CloseHandle(g_reloadEvent);
    CloseHandle(g_stopEvent);
    g_reloadEvent = g_stopEvent = nullptr;
}

void PostReload() {
    SetEvent(g_reloadEvent);
}

bool WaitForSignal() {
    HANDLE events[2] = {g_stopEvent, g_reloadEvent};
    return WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1;
}

#endif

// Waits for ProWindows to say "reload" (the ini changed) or "stop" (styling
// off, or ProWindows quitting). Stop undoes everything the mod did and takes
// the hooks out; the module stays loaded (see above).
DWORD WINAPI WorkerProc(LPVOID) {
    while (WaitForSignal()) {
        try {
            LoadIni();
            Wh_ModSettingsChanged();
        } catch (...) {
            Wh_Log(L"exception in Wh_ModSettingsChanged");
        }
    }

    std::lock_guard<std::mutex> life(g_lifeMutex);
    // The signals go first: from here ProWindows sees this process as not
    // styled, and a new injection waits in StylerStart until the stop is done.
    CloseSignals();
    try {
        Wh_ModUninit();
    } catch (...) {
        Wh_Log(L"exception in Wh_ModUninit");
    }
    DisableAllHooks();
    CloseHandle(g_worker);
    g_worker = nullptr;
    g_running = false;
    Wh_Log(L"stopped");
    return 0;
}

}  // namespace

// Run by the injecting thread (CreateRemoteThread) right after LoadLibraryW.
// `configDir` is a wide string in this process, allocated by ProWindows, or
// null for %APPDATA%\ProWindows. Returns 1 when the styler is running.
extern "C" __declspec(dllexport) DWORD WINAPI StylerStart(LPVOID configDir) {
    std::lock_guard<std::mutex> life(g_lifeMutex);
    if (g_running) {
        PostReload();  // already running: just re-read the ini
        return 1;
    }

    // Pinned: see "lifecycle" above. Also covers the FreeLibrary the TAP does
    // to balance InitializeXamlDiagnosticsEx's load.
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&StylerStart), &self);

    // Set once: Wh_Log reads it unlocked, from any thread, and a straggler
    // inside a hook body may still be logging from the previous run.
    if (g_configDir.empty()) {
        g_configDir =
            configDir ? static_cast<PCWSTR>(configDir) : DefaultConfigDir();
    }
    LoadIni();
    Wh_Log(L"StylerStart, config dir %s", g_configDir.c_str());

    if (!OpenSignals()) {
        CloseSignals();
        return 0;
    }

    bool ok = false;
    bool inited = false;
    MH_STATUS mh = MH_Initialize();
    if (mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED) {
        {
            std::lock_guard<std::mutex> lock(g_hookMutex);
            g_hooksAllowed = true;
        }
        try {
            if (Wh_ModInit()) {
                inited = true;
                Wh_ApplyHookOperations();
                Wh_ModAfterInit();
                ok = true;
            }
        } catch (...) {
            Wh_Log(L"exception in Wh_ModInit");
        }
    }

    if (ok) {
        g_worker = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
        ok = g_worker != nullptr;
    }
    if (!ok) {
        if (inited) {
            try {
                Wh_ModUninit();
            } catch (...) {
            }
        }
        DisableAllHooks();
        CloseSignals();
        return 0;
    }
    g_running = true;
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
