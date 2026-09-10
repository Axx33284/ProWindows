#include "common.h"
#include <shlobj.h>
#include <cstdarg>

namespace awa {

// ---------------------------------------------------------------- string utils
std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

bool IEquals(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// Case-insensitive substring search without building lowercase copies of both
// strings. This is on the hot path: every window event runs the whole ignore /
// float rule set through here, and the old version allocated two std::wstrings
// per rule per event.
bool IContains(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return false;
    if (needle.size() > hay.size()) return false;

    const size_t last = hay.size() - needle.size();
    const wchar_t first = (wchar_t)towlower(needle[0]);

    for (size_t i = 0; i <= last; ++i) {
        if ((wchar_t)towlower(hay[i]) != first) continue;
        size_t j = 1;
        for (; j < needle.size(); ++j)
            if ((wchar_t)towlower(hay[i + j]) != (wchar_t)towlower(needle[j])) break;
        if (j == needle.size()) return true;
    }
    return false;
}

std::vector<std::wstring> SplitList(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find(sep, start);
        if (p == std::wstring::npos) p = s.size();
        std::wstring item = Trim(s.substr(start, p - start));
        if (!item.empty()) out.push_back(item);
        if (p == s.size()) break;
        start = p + 1;
    }
    return out;
}

// ---------------------------------------------------------------- paths
std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, n);
}

// The app was called Auto Windows Arrange before it was called ProWindows,
// and renaming it moved the settings folder out from under anyone who had
// been using it. Anything found under the old name is copied across once.
static void MigrateLegacySettings(const std::wstring& roaming,
                                  const std::wstring& dir) {
    if (roaming.empty()) return;

    const std::wstring legacy = roaming + L"\\AutoWindowsArrange";
    if (GetFileAttributesW(legacy.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    // Only ever on a fresh folder: never overwrite settings made since.
    const std::wstring config = dir + L"\\config.ini";
    if (GetFileAttributesW(config.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    const wchar_t* carry[] = { L"config.ini", L"launcher.txt" };
    for (const wchar_t* name : carry) {
        const std::wstring from = legacy + L"\\" + name;
        const std::wstring to   = dir + L"\\" + name;
        // Copied rather than moved: if this turns out badly the old install
        // is still sitting there untouched.
        CopyFileW(from.c_str(), to.c_str(), TRUE);
    }
}

static std::wstring ComputeConfigDir() {
    PWSTR roaming = nullptr;
    std::wstring dir;
    std::wstring roamingRoot;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        dir = roaming;
        roamingRoot = roaming;
        CoTaskMemFree(roaming);
    }
    if (dir.empty()) dir = L".";
    dir += L"\\";
    dir += kAppShort;
    CreateDirectoryW(dir.c_str(), nullptr);
    MigrateLegacySettings(roamingRoot, dir);
    return dir;
}

// Asked for by the file indexer, the app scanner and the temperature probe as
// well as by the UI thread. The old "if (cached.empty()) cached = ..." was a
// plain check-then-assign on a std::wstring from four threads at once, which is
// undefined behaviour and only ever worked because the UI thread happened to
// get here first during startup. A function-local static is initialised exactly
// once, and the language guarantees every other thread waits for it.
const std::wstring& ConfigDir() {
    static const std::wstring cached = ComputeConfigDir();
    return cached;
}

std::wstring ConfigPath() { return ConfigDir() + L"\\config.ini"; }
std::wstring LogPath()    { return ConfigDir() + L"\\log.txt"; }

// ---------------------------------------------------------------- logging
// Written from the UI thread, the file indexer, the app scanner and the
// temperature probe. A plain bool read across threads is a data race, and four
// threads each opening, appending to and closing the same file produced
// interleaved half-lines - which corrupts the one artefact you would use to
// diagnose a threading problem in the first place.
static LONG g_logOn = 0;

namespace {

struct LogLock {
    CRITICAL_SECTION cs;
    LogLock() { InitializeCriticalSection(&cs); }
    // Deliberately not deleted: a background thread can still be logging while
    // the process tears down, and entering a deleted section is a crash.
    ~LogLock() = default;
};

// C++11 magic static: constructed once, thread-safely, on first use.
CRITICAL_SECTION& LogSection() {
    static LogLock lock;
    return lock.cs;
}

} // namespace

void LogEnable(bool on) {
    const LONG want = on ? 1 : 0;
    // Only truncate when logging is actually being switched on. LoadConfig
    // calls this on every settings reload, and truncating there wiped the log
    // out from under whoever was reading it.
    const LONG had = InterlockedExchange(&g_logOn, want);
    if (!on || had == want) return;

    EnterCriticalSection(&LogSection());
    FILE* f = nullptr;
    if (_wfopen_s(&f, LogPath().c_str(), L"w, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"=== %s %s log start ===\n", kAppName, kVersion);
        fclose(f);
    }
    LeaveCriticalSection(&LogSection());
}

void LogLine(const wchar_t* fmt, ...) {
    if (InterlockedCompareExchange(&g_logOn, 0, 0) == 0) return;

    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    // Held across the whole write, not just the open: two threads appending to
    // the same file through separate FILE* handles interleave mid-line.
    EnterCriticalSection(&LogSection());
    FILE* f = nullptr;
    if (_wfopen_s(&f, LogPath().c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"[%02d:%02d:%02d.%03d] %s\n",
                 (int)st.wHour, (int)st.wMinute, (int)st.wSecond,
                 (int)st.wMilliseconds, buf);
        fclose(f);
    }
    LeaveCriticalSection(&LogSection());
}

} // namespace awa
