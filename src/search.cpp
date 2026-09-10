#include "search.h"
#include <shlobj.h>
#include <cwchar>
#include <cwctype>
#include <cmath>
#include <cfloat>

namespace awa {

namespace {

Config* g_cfg = nullptr;

// Case folding on the hot path. Names are overwhelmingly ASCII, so the common
// case is a compare and a subtract; towlower is a function call into the CRT's
// locale tables and is reserved for the characters that actually need it.
inline wchar_t LowerFast(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return (wchar_t)(c - L'A' + L'a');
    if (c < 128) return c;
    return (wchar_t)towlower(c);
}

// What one index walk should do. Copied off the config on the UI thread before
// the walk starts, because ReloadConfig can rewrite the live Config at any
// moment and the indexer must not be reading it while that happens.
struct IndexWants {
    std::vector<std::wstring> roots;
    int    maxDepth      = 5;
    size_t maxEntries    = 20000;
    bool   includeHidden = false;

    // Where to go looking for executables that never made a Start-menu
    // shortcut, and how many of them to keep. Counted separately from
    // `maxEntries` so a home folder full of documents cannot crowd out the
    // programs, and vice versa.
    bool   programs      = true;
    std::vector<std::wstring> progRoots;
    size_t maxPrograms   = 6000;
    // Every other fixed drive, walked from its root. Kept apart from
    // `progRoots` because they are walked differently: a drive root is one
    // level further out than Program Files and its top level is full of
    // things - Windows, Users, ProgramData - that a program walk must not
    // descend into at all.
    std::vector<std::wstring> driveRoots;
    // Keep the executables an application ships to serve itself.
    bool   deepExe       = false;
};
IndexWants g_wants;         // guarded by g_lock

bool SameWants(const IndexWants& a, const IndexWants& b) {
    return a.maxDepth == b.maxDepth && a.maxEntries == b.maxEntries &&
           a.includeHidden == b.includeHidden && a.roots == b.roots &&
           a.programs == b.programs && a.progRoots == b.progRoots &&
           a.maxPrograms == b.maxPrograms && a.driveRoots == b.driveRoots &&
           a.deepExe == b.deepExe;
}

// Program Files, Program Files (x86), and the per-user folder modern installers
// prefer. Resolved through the environment rather than hard-coded, so this is
// right on a machine whose Windows is not on C:.
std::vector<std::wstring> ProgramRoots() {
    const wchar_t* vars[] = { L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432" };
    std::vector<std::wstring> roots;
    wchar_t buf[MAX_PATH * 2];

    for (const wchar_t* var : vars) {
        if (!GetEnvironmentVariableW(var, buf, (DWORD)ARRAYSIZE(buf))) continue;
        std::wstring path = buf;
        if (path.empty()) continue;
        // ProgramW6432 and ProgramFiles are the same folder on a 64-bit build.
        if (std::find(roots.begin(), roots.end(), path) == roots.end())
            roots.push_back(std::move(path));
    }
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, (DWORD)ARRAYSIZE(buf))) {
        std::wstring path = std::wstring(buf) + L"\\Programs";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            roots.push_back(std::move(path));
    }
    return roots;
}

// Every fixed drive except the one Windows is on, whose interesting parts are
// already covered by ProgramRoots and the home folders.
//
// Fixed only: walking a removable disk means spinning up whatever is plugged
// in, walking a network drive means a recursive listing over the wire, and
// walking a mounted phone or camera means neither of those things finishes.
// GetDriveType answers all three without touching the disk.
std::vector<std::wstring> DriveRoots() {
    std::vector<std::wstring> roots;

    wchar_t systemDrive[8] = {};
    GetEnvironmentVariableW(L"SystemDrive", systemDrive, (DWORD)ARRAYSIZE(systemDrive));

    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t letter = (wchar_t)(L'A' + i);
        const std::wstring root = std::wstring(1, letter) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        // "C:" from the environment, "C:\" here.
        if (systemDrive[0] && towupper(systemDrive[0]) == towupper(letter)) continue;
        roots.push_back(root);
    }
    return roots;
}

// A folder that only ever *holds* programs rather than being part of one.
//
// These do not count against the walk's depth budget, which is what makes
// walking a whole disk affordable. An application's executable is two or three
// folders below wherever it was installed - but "wherever it was installed"
// can itself be four folders of pure filing:
//
//     D:\SteamLibrary\steamapps\common\Game\Binaries\Win64\game.exe
//
// Spending the budget on `SteamLibrary\steamapps\common` means either missing
// that executable or raising the limit for the entire disk, and raising it for
// the entire disk is how a walk of a media drive comes to enumerate every
// folder six levels down. Charging nothing for the three containers reaches
// exactly the thing that was wanted and nothing else.
bool ProgramContainerDirectory(const std::wstring& lowName) {
    static const wchar_t* kContainers[] = {
        L"program files", L"program files (x86)", L"programs",
        L"games", L"game files", L"gamefiles", L"my games",
        L"steam", L"steamlibrary", L"steamapps", L"common",
        L"epic games", L"gog galaxy", L"gog games", L"xboxgames",
        L"riot games", L"battle.net", L"blizzard", L"origin games",
        L"ea games", L"ubisoft", L"ubisoft game launcher",
        L"applications", L"apps", L"portable", L"portableapps", L"tools",
    };
    for (const wchar_t* n : kContainers)
        if (lowName == n) return true;
    return false;
}

// The top level of a drive that is Windows' own business rather than a place
// anybody installs anything. Only consulted at depth 0 of a drive walk, so a
// folder genuinely called "recovery" inside an application is unaffected.
bool SkipDriveTopLevel(const std::wstring& lowName) {
    static const wchar_t* kNoise[] = {
        L"windows", L"windows.old", L"winsxs", L"programdata", L"users",
        L"$recycle.bin", L"system volume information", L"recovery", L"boot",
        L"perflogs", L"msocache", L"config.msi", L"$windows.~bt",
        L"$windows.~ws", L"$getcurrent", L"$sysreset", L"efi", L"intel",
        L"amd", L"nvidia", L"drivers", L"temp", L"tmp", L"documents and settings",
    };
    for (const wchar_t* n : kNoise)
        if (lowName == n) return true;
    return false;
}

// Read on the UI thread only. The walk gets a copy of the result, never the
// Config itself, which ReloadConfig can rewrite at any moment.
IndexWants WantsFromConfig() {
    IndexWants wants;
    if (g_cfg) {
        wants.roots         = g_cfg->searchFolders;
        wants.maxDepth      = (std::max)(1, (std::min)(12, g_cfg->searchDepth));
        wants.maxEntries    = (size_t)(std::max)(100, g_cfg->searchMaxEntries);
        wants.includeHidden = g_cfg->searchHidden;
        wants.programs      = g_cfg->searchPrograms;
        wants.deepExe       = g_cfg->searchDeepExe;
        wants.maxPrograms   = (size_t)(std::max)(100, g_cfg->searchMaxPrograms);
    }
    // Resolved here rather than stored, so the default set keeps following the
    // user's folders if they move them - and so SameWants compares like for like.
    if (wants.roots.empty()) wants.roots = SearchDefaultFolders();
    if (wants.programs) {
        wants.progRoots = ProgramRoots();
        if (!g_cfg || g_cfg->searchDrives) wants.driveRoots = DriveRoots();
    }
    return wants;
}

// ---------------------------------------------------------------- the index
// One string per entry, not two. Matching folds case as it scans (SearchScore),
// so there is no lower-cased copy to hold: on a 20,000 entry index that is
// several megabytes that simply are not allocated. `name` is a view into
// `path`, so it costs nothing either.
struct FileEntry {
    std::wstring path;
    int          nameAt = 0;
    bool         folder = false;
    bool         exe    = false;   // ranked and shown as a program, not a file

    std::wstring Name() const { return path.substr((size_t)nameAt); }
    std::wstring Folder() const {
        return nameAt ? path.substr(0, (size_t)nameAt - 1) : std::wstring();
    }
};

// Scores an entry's file name without building a string for it: the name is
// already sitting in `path`, so point at it and give the length.
bool ScoreEntryName(const FileEntry& e, const std::wstring& lowQuery, int* score) {
    return SearchScore(e.path.c_str() + e.nameAt,
                       e.path.size() - (size_t)e.nameAt, lowQuery, score);
}

// Written by the indexer thread, read by the UI thread on every keystroke.
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
std::vector<FileEntry> g_index;
bool   g_ready       = false;
bool   g_abandon     = false;   // set at shutdown; see MAP.md invariant 12

// The same fact as g_abandon, published so the walk itself can check it.
// The walk visits tens of thousands of directories and cannot take a critical
// section at each one, but without any check at all it runs to completion no
// matter what - which is what left SearchShutdown waiting five seconds and then
// abandoning a live thread into ExitProcess, where it can be terminated holding
// the CRT heap lock. An interlocked read is free by comparison.
LONG   g_abandonFlag = 0;
HANDLE g_thread      = nullptr;
HANDLE g_stop        = nullptr; // signalled at shutdown, so a delay is skippable
DWORD  g_delayMs     = 0;       // how long the pending walk should hold off

// Logging in, every startup program fights over the disk at once. Joining that
// queue with a recursive walk is the difference between a machine that feels
// slow to boot and one that does not - so the first walk waits for the rush to
// pass. Nothing else is blocked meanwhile: apps are already searchable.
constexpr DWORD kColdStartDelayMs = 20 * 1000;
// A cache is already loaded and usable; refreshing it is in no hurry at all.
constexpr DWORD kRefreshDelayMs   = 120 * 1000;
// How stale a cache may be before it is quietly rebuilt in the background.
constexpr ULONGLONG kCacheMaxAgeMs = 24ull * 60 * 60 * 1000;

// Is this an executable, by name? Only .exe: a .bat or a .cmd is a script
// somebody wrote, not an application, and offering to run one from a search bar
// on a single keypress is not a favour.
bool IsExecutableName(const wchar_t* name, size_t len) {
    if (len < 5) return false;
    const wchar_t* ext = name + len - 4;
    return (ext[0] == L'.') &&
           (ext[1] == L'e' || ext[1] == L'E') &&
           (ext[2] == L'x' || ext[2] == L'X') &&
           (ext[3] == L'e' || ext[3] == L'E');
}

// Executables that exist to serve another executable. Program Files is full of
// them and not one is something anybody searches for by name, so they are left
// out rather than left in to be scrolled past.
bool NoiseExecutable(const std::wstring& lowStem) {
    static const wchar_t* kNoise[] = {
        L"unins", L"setup", L"install", L"update", L"upgrade", L"patch",
        L"crashpad", L"crashreport", L"crashhandler", L"bugreport",
        L"vcredist", L"redist", L"dotnet", L"vc_redist",
        L"helper", L"reporter", L"elevat", L"watchdog", L"daemon",
        L"service", L"squirrel", L"maintenance", L"repair", L"cleanup",
        L"launcher_helper", L"notification", L"telemetry", L"diagnos",
    };
    for (const wchar_t* n : kNoise)
        if (lowStem.find(n) != std::wstring::npos) return true;
    return false;
}

// Folders inside an installed application that hold its parts rather than the
// application. Skipping them is most of what makes a walk of Program Files
// cheap enough to do at all.
bool SkipProgramDirectory(const std::wstring& lowName) {
    static const wchar_t* kNoise[] = {
        L"redist", L"crashpad", L"locales", L"resources", L"plugins",
        L"runtime", L"runtimes", L"jre", L"jdk", L"node_modules", L"cache",
        L"logs", L"temp", L"tmp", L"swiftshader", L"installer", L"drivers",
        L"lib", L"libs", L"include", L"licenses", L"docs", L"samples",
        L"windowsapps",
        // A developer toolchain ships a whole POSIX userland - Git alone puts
        // sixty single-purpose binaries under usr\bin. Every one of them
        // matches a short query and not one of them is what anybody meant.
        L"usr", L"sbin", L"libexec", L"share", L"etc", L"mingw32", L"mingw64",
        L"site-packages", L"scripts", L"__pycache__",
    };
    for (const wchar_t* n : kNoise)
        if (lowName == n) return true;
    return false;
}

// Directories that are never worth indexing: they are enormous, they are full
// of files nobody searches for by name, or both.
bool SkipDirectory(const std::wstring& lowName) {
    static const wchar_t* kNoise[] = {
        L"appdata", L"node_modules", L".git", L".svn", L".hg", L"__pycache__",
        L".venv", L"venv", L".cache", L".gradle", L".nuget", L".conda",
        L"$recycle.bin", L"system volume information", L".vs", L".idea",
    };
    for (const wchar_t* n : kNoise)
        if (lowName == n) return true;
    return false;
}

// How long ago the cache on disk was written, or "forever" if there is none.
ULONGLONG IndexCacheAgeMs();
void StartWalk(DWORD delayMs);

bool AbandonRequested() {
    return InterlockedCompareExchange(&g_abandonFlag, 0, 0) != 0;
}

// ---------------------------------------------------------------- index cache
// The walk is the expensive part, and its result barely changes between one
// launch and the next. Writing it out means a cold start reads one file
// sequentially instead of seeking over a whole home folder.
std::wstring IndexCachePath() { return ConfigDir() + L"\\index.cache"; }

// Identifies the settings a cache was built under. A cache built for different
// folders, depth or ceiling is not a cache for the settings in force now.
std::wstring CacheSignature(const IndexWants& w) {
    // v2 added the executable flag to every row and the program roots to the
    // signature; v3 added the other drives, the program ceiling and deep-exe
    // mode. A cache written under different settings is not a cache for these.
    //
    // The version is also how a change to the *walk* reaches an existing
    // cache. How deep a drive is walked and which folders are free of that
    // budget are constants in this file, not settings, so nothing else in the
    // signature moves when they are edited - and an old cache would then be
    // served forever as though it were current. Bump this when the walk
    // changes shape; v4 is the containers being free of the depth budget.
    std::wstring sig = L"v4|" + std::to_wstring(w.maxDepth) + L"|" +
                       std::to_wstring((unsigned long long)w.maxEntries) + L"|" +
                       std::to_wstring((unsigned long long)w.maxPrograms) + L"|" +
                       (w.includeHidden ? L"h" : L"-") + L"|" +
                       (w.programs ? L"p" : L"-") + L"|" +
                       (w.deepExe ? L"d" : L"-") + L"|";
    for (const auto& r : w.roots) sig += r + L"*";
    sig += L"|";
    for (const auto& r : w.progRoots) sig += r + L"*";
    sig += L"|";
    for (const auto& r : w.driveRoots) sig += r + L"*";
    return sig;
}

// Takes the entries directly rather than reading g_index, so the lock is not
// held across a few megabytes of file writing - that would stall every
// keystroke in the search bar for as long as the write took.
void SaveIndexCache(const IndexWants& wants, const std::vector<FileEntry>& entries) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, IndexCachePath().c_str(), L"w, ccs=UTF-8") != 0 || !f) return;

    fwprintf(f, L"%s\n", CacheSignature(wants).c_str());
    for (const auto& e : entries)
        fwprintf(f, L"%d\t%d\t%s\n",
                 (e.folder ? 1 : 0) | (e.exe ? 2 : 0), e.nameAt, e.path.c_str());

    fclose(f);
}

// Returns false when there is no usable cache, in which case a walk is needed.
bool LoadIndexCache(const IndexWants& wants) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, IndexCachePath().c_str(), L"r, ccs=UTF-8") != 0 || !f)
        return false;

    std::vector<FileEntry> loaded;
    bool ok = false;
    // Long enough for any real path; a line that overflows is simply skipped.
    std::vector<wchar_t> line(4096);

    if (fgetws(line.data(), (int)line.size(), f)) {
        if (Trim(line.data()) == CacheSignature(wants)) {
            ok = true;
            loaded.reserve(4096);
            while (loaded.size() < wants.maxEntries &&
                   fgetws(line.data(), (int)line.size(), f)) {
                const wchar_t* p = line.data();
                wchar_t* end = nullptr;
                const long folder = wcstol(p, &end, 10);
                if (end == p || *end != L'\t') continue;
                const wchar_t* q = end + 1;
                const long nameAt = wcstol(q, &end, 10);
                if (end == q || *end != L'\t' || nameAt < 0) continue;

                std::wstring path = Trim(end + 1);
                if (path.empty() || (size_t)nameAt >= path.size()) continue;

                FileEntry entry;
                entry.path   = std::move(path);
                entry.nameAt = (int)nameAt;
                entry.folder = (folder & 1) != 0;
                entry.exe    = (folder & 2) != 0;
                loaded.push_back(std::move(entry));
            }
        }
    }
    fclose(f);
    if (!ok) return false;

    loaded.shrink_to_fit();
    const int count = (int)loaded.size();

    EnterCriticalSection(&g_lock);
    g_index = std::move(loaded);
    g_ready = true;
    LeaveCriticalSection(&g_lock);

    AWA_LOG(L"search: %d entries restored from the cache", count);
    return true;
}

void WalkFolder(const std::wstring& dir, int depth, int maxDepth,
                size_t maxEntries, bool includeHidden,
                std::vector<FileEntry>* out) {
    if (depth > maxDepth || out->size() >= maxEntries) return;
    // Checked once per directory rather than once per file: often enough that
    // shutdown never has to wait, cheap enough not to show up in the walk.
    if (AbandonRequested()) return;

    WIN32_FIND_DATAW find = {};
    // FindExInfoBasic skips the 8.3 name, LARGE_FETCH batches the reads. Both
    // matter over tens of thousands of files.
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &find,
                                FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> subdirs;

    do {
        if (out->size() >= maxEntries) break;

        const std::wstring name = find.cFileName;
        if (name == L"." || name == L"..") continue;

        const DWORD attr = find.dwFileAttributes;
        if (attr & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // no symlink loops
        if (!includeHidden &&
            (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;

        const bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir && SkipDirectory(ToLower(name))) continue;

        FileEntry entry;
        entry.path   = dir + L"\\" + name;
        entry.nameAt = (int)(dir.size() + 1);
        entry.folder = isDir;
        entry.exe    = !isDir && IsExecutableName(name.c_str(), name.size());

        // Recurse afterwards, not here: holding a find handle open all the way
        // down a deep tree pins one per level for the whole walk.
        if (isDir) subdirs.push_back(entry.path);

        out->push_back(std::move(entry));
    } while (FindNextFileW(h, &find));

    FindClose(h);

    for (const auto& sub : subdirs) {
        if (AbandonRequested()) return;
        WalkFolder(sub, depth + 1, maxDepth, maxEntries, includeHidden, out);
    }
}

// How a program walk is being run: what it may descend into, and what counts
// as an executable worth keeping. Passed down rather than read from the config
// for the usual reason - the walk is on its own thread and the config is not.
struct ProgramWalk {
    int    maxDepth  = 4;
    size_t ceiling   = 0;
    bool   deepExe   = false;   // keep an application's own helper executables
    bool   driveRoot = false;   // depth 0 is the root of a disk, not a folder
};

// Executables only, and only the ones that look like something a person would
// open. Deliberately not the general walk with a filter on the end: a drive is
// deep and wide, and the point is to touch as little of it as possible.
void WalkPrograms(const std::wstring& dir, int depth, const ProgramWalk& how,
                  std::vector<FileEntry>* out) {
    if (depth > how.maxDepth || out->size() >= how.ceiling) return;
    if (AbandonRequested()) return;

    // "C:\" already ends in a separator; "C:\Games" does not.
    const bool hasSlash = !dir.empty() && dir.back() == L'\\';
    const std::wstring prefix = hasSlash ? dir : dir + L"\\";

    WIN32_FIND_DATAW find = {};
    HANDLE h = FindFirstFileExW((prefix + L"*").c_str(), FindExInfoBasic, &find,
                                FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> subdirs;
    // Parallel to `subdirs`: true where descending costs nothing.
    std::vector<bool> free;

    do {
        if (out->size() >= how.ceiling) break;

        const std::wstring name = find.cFileName;
        if (name == L"." || name == L"..") continue;

        const DWORD attr = find.dwFileAttributes;
        if (attr & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // no symlink loops
        if (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;

        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            const std::wstring low = ToLower(name);
            // The top level of a disk is Windows, Users, ProgramData and a
            // handful of installer leftovers. Descending into any of them is
            // the whole cost of walking a drive and none of the benefit.
            if (how.driveRoot && depth == 0 && SkipDriveTopLevel(low)) continue;
            if (SkipDirectory(low)) continue;
            // The folders that hold an application's parts are still skipped
            // in deep mode; what deep mode keeps is the executables inside
            // the folders it does walk. Skipping node_modules is not an
            // opinion about which exe you meant.
            if (!how.deepExe && SkipProgramDirectory(low)) continue;
            // A pure container costs nothing to pass through. See
            // ProgramContainerDirectory - it is what lets the budget be small
            // enough to walk a whole disk and still deep enough to find a game.
            subdirs.push_back(prefix + name);
            if (ProgramContainerDirectory(low)) free.push_back(true);
            else                                free.push_back(false);
            continue;
        }

        if (!IsExecutableName(name.c_str(), name.size())) continue;
        const std::wstring stem = ToLower(name.substr(0, name.size() - 4));
        if (!how.deepExe) {
            // "ex", "sh", "ls": a two-letter binary is a command-line utility,
            // and it matches almost any short query while being almost never
            // the thing that was meant.
            if (stem.size() < 3) continue;
            if (NoiseExecutable(stem)) continue;
        }

        FileEntry entry;
        entry.path   = prefix + name;
        entry.nameAt = (int)prefix.size();
        entry.folder = false;
        entry.exe    = true;
        out->push_back(std::move(entry));
    } while (FindNextFileW(h, &find));

    FindClose(h);

    for (size_t i = 0; i < subdirs.size(); ++i) {
        if (AbandonRequested()) return;
        WalkPrograms(subdirs[i], free[i] ? depth : depth + 1, how, out);
    }
}

DWORD WINAPI IndexThread(LPVOID) {
    // Background mode drops this thread's *I/O* priority as well as its CPU
    // priority, which is the whole game on a spinning disk: the walk is pure
    // random seeks, and without this it competes with whatever the user is
    // actually doing. Vista and later; the failure is harmless if it is not.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);

    EnterCriticalSection(&g_lock);
    const IndexWants wants = g_wants;      // whole-config snapshot, taken safely
    const DWORD delay = g_delayMs;
    LeaveCriticalSection(&g_lock);

    // Interruptible: shutting the app down during the hold-off must not make
    // the user wait out the rest of it.
    if (delay && g_stop && WaitForSingleObject(g_stop, delay) == WAIT_OBJECT_0)
        return 0;
    if (AbandonRequested()) return 0;

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const ULONGLONG began = GetTickCount64();

    std::vector<FileEntry> built;
    built.reserve(4096);
    for (const auto& root : wants.roots) {
        if (root.empty()) continue;
        if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        WalkFolder(root, 0, wants.maxDepth, wants.maxEntries,
                   wants.includeHidden, &built);
        if (built.size() >= wants.maxEntries) break;
        // Bail out early if the app is shutting down mid-walk; a deep tree on a
        // slow disk is exactly when this matters.
        if (AbandonRequested()) break;
    }
    // Programs second, with a ceiling of their own, so a home folder full of
    // documents cannot use up the budget before Program Files is reached.
    if (wants.programs && !AbandonRequested()) {
        const size_t ceiling = built.size() + wants.maxPrograms;

        ProgramWalk how;
        how.ceiling = ceiling;
        how.deepExe = wants.deepExe;

        for (const auto& root : wants.progRoots) {
            if (root.empty()) continue;
            if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            // Four levels reaches Program Files\Vendor\Product\bin\thing.exe,
            // which is as deep as an application's own executable ever sits.
            how.maxDepth  = 4;
            how.driveRoot = false;
            WalkPrograms(root, 0, how, &built);
            if (built.size() >= ceiling || AbandonRequested()) break;
        }

        // Then every other fixed drive, from the root. Two levels deeper than
        // Program Files, because a drive root is one level further out and a
        // game sits one further in than an ordinary application:
        // D:\SteamLibrary\steamapps\common\Game\Binaries\Win64\game.exe is
        // exactly six, and it is the shape this exists to find.
        if (!AbandonRequested()) {
            const size_t before = built.size();
            const ULONGLONG drivesBegan = GetTickCount64();
            for (const auto& root : wants.driveRoots) {
                if (root.empty()) continue;
                if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                how.maxDepth  = 4;
                how.driveRoot = true;
                WalkPrograms(root, 0, how, &built);
                if (built.size() >= ceiling || AbandonRequested()) break;
            }
            if (!wants.driveRoots.empty())
                AWA_LOG(L"search: %d programs from %d other drive(s) in %llu ms",
                        (int)(built.size() - before), (int)wants.driveRoots.size(),
                        GetTickCount64() - drivesBegan);
        }
    }

    built.shrink_to_fit();               // reserve(4096) usually overshoots

    if (SUCCEEDED(hr)) CoUninitialize();

    if (AbandonRequested()) {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        return 0;
    }

    const int indexed = (int)built.size();
    AWA_LOG(L"search: indexed %d entries in %llu ms", indexed,
            GetTickCount64() - began);

    // Write the cache before handing the entries over, while they are still
    // this thread's own: the next launch then skips the walk entirely, which on
    // a mechanical disk is the difference between a moment and a minute.
    SaveIndexCache(wants, built);

    EnterCriticalSection(&g_lock);
    if (!g_abandon) {
        g_index = std::move(built);
        g_ready = true;
    }
    LeaveCriticalSection(&g_lock);

    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    return 0;
}

ULONGLONG IndexCacheAgeMs() {
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExW(IndexCachePath().c_str(), GetFileExInfoStandard, &info))
        return ~0ull;

    ULARGE_INTEGER written;
    written.LowPart  = info.ftLastWriteTime.dwLowDateTime;
    written.HighPart = info.ftLastWriteTime.dwHighDateTime;

    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart  = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;

    if (now.QuadPart <= written.QuadPart) return 0;      // clock moved backwards
    return (now.QuadPart - written.QuadPart) / 10000ull; // 100 ns -> ms
}

// Schedules the walk. `delayMs` is how long the thread holds off before it
// touches the disk at all; 0 means the user asked for it, so do it now.
void StartWalk(DWORD delayMs) {
    if (!g_lockReady) return;
    if (g_thread) {
        // Still walking (or still holding off). One walk at a time.
        if (WaitForSingleObject(g_thread, 0) != WAIT_OBJECT_0) return;
        CloseHandle(g_thread);
        g_thread = nullptr;
    }

    IndexWants wants = WantsFromConfig();

    EnterCriticalSection(&g_lock);
    g_wants   = std::move(wants);
    g_delayMs = delayMs;
    // A walk started from scratch invalidates nothing that is already loaded:
    // keep serving the old index until the new one lands, so an explicit
    // rebuild does not blank the results while it runs.
    LeaveCriticalSection(&g_lock);

    g_thread = CreateThread(nullptr, 0, IndexThread, nullptr, 0, nullptr);
}

// ---------------------------------------------------------------- settings pages
struct SettingsPage { const wchar_t* name; const wchar_t* uri; };

// The pages people actually go looking for. Deliberately not the full list:
// a hundred near-duplicate rows would bury the app you were after.
const SettingsPage kSettingsPages[] = {
    { L"Display settings",          L"ms-settings:display" },
    { L"Night light",               L"ms-settings:nightlight" },
    { L"Graphics settings",         L"ms-settings:display-advancedgraphics" },
    { L"Sound settings",            L"ms-settings:sound" },
    { L"Volume mixer",              L"ms-settings:apps-volume" },
    { L"Notifications",             L"ms-settings:notifications" },
    { L"Focus assist",              L"ms-settings:quiethours" },
    { L"Power and battery",         L"ms-settings:powersleep" },
    { L"Storage",                   L"ms-settings:storagesense" },
    { L"Multitasking",              L"ms-settings:multitasking" },
    { L"Clipboard",                 L"ms-settings:clipboard" },
    { L"Remote Desktop",            L"ms-settings:remotedesktop" },
    { L"About this PC",             L"ms-settings:about" },
    { L"Bluetooth and devices",     L"ms-settings:bluetooth" },
    { L"Printers and scanners",     L"ms-settings:printers" },
    { L"Mouse settings",            L"ms-settings:mousetouchpad" },
    { L"Touchpad settings",         L"ms-settings:devices-touchpad" },
    { L"Typing settings",           L"ms-settings:typing" },
    { L"AutoPlay",                  L"ms-settings:autoplay" },
    { L"USB settings",              L"ms-settings:usb" },
    { L"Network and internet",      L"ms-settings:network" },
    { L"Wi-Fi settings",            L"ms-settings:network-wifi" },
    { L"VPN settings",              L"ms-settings:network-vpn" },
    { L"Mobile hotspot",            L"ms-settings:network-mobilehotspot" },
    { L"Proxy settings",            L"ms-settings:network-proxy" },
    { L"Personalisation",           L"ms-settings:personalization" },
    { L"Background wallpaper",      L"ms-settings:personalization-background" },
    { L"Colours and accent",        L"ms-settings:colors" },
    { L"Themes",                    L"ms-settings:themes" },
    { L"Lock screen",               L"ms-settings:lockscreen" },
    { L"Start menu settings",       L"ms-settings:personalization-start" },
    { L"Taskbar settings",          L"ms-settings:taskbar" },
    { L"Fonts",                     L"ms-settings:fonts" },
    { L"Installed apps",            L"ms-settings:appsfeatures" },
    { L"Default apps",              L"ms-settings:defaultapps" },
    { L"Startup apps",              L"ms-settings:startupapps" },
    { L"Optional features",         L"ms-settings:optionalfeatures" },
    { L"Accounts",                  L"ms-settings:yourinfo" },
    { L"Sign-in options",           L"ms-settings:signinoptions" },
    { L"Windows Update",            L"ms-settings:windowsupdate" },
    { L"Date and time",             L"ms-settings:dateandtime" },
    { L"Language and region",       L"ms-settings:regionlanguage" },
    { L"Accessibility",             L"ms-settings:easeofaccess-display" },
    { L"Privacy and security",      L"ms-settings:privacy" },
    { L"Windows Security",          L"ms-settings:windowsdefender" },
    { L"Camera privacy",            L"ms-settings:privacy-webcam" },
    { L"Microphone privacy",        L"ms-settings:privacy-microphone" },
    { L"Developer settings",        L"ms-settings:developers" },
    { L"Troubleshoot",              L"ms-settings:troubleshoot" },
    { L"Recovery",                  L"ms-settings:recovery" },
};

// ---------------------------------------------------------------- calculator
// A plain recursive-descent parser over a wide string. No allocation, no
// locale surprises, and it either consumes the whole expression or fails.
struct Calc {
    const wchar_t* p;
    bool ok = true;

    void Skip() { while (*p == L' ' || *p == L'\t') ++p; }

    bool Eat(wchar_t c) {
        Skip();
        if (*p != c) return false;
        ++p;
        return true;
    }

    double Expr() {
        double v = Term();
        for (;;) {
            Skip();
            if (*p == L'+')      { ++p; v += Term(); }
            else if (*p == L'-') { ++p; v -= Term(); }
            else return v;
        }
    }

    double Term() {
        double v = Unary();
        for (;;) {
            Skip();
            if (*p == L'*') {
                ++p;
                v *= Unary();
            } else if (*p == L'/') {
                ++p;
                const double d = Unary();
                if (d == 0.0) { ok = false; return 0.0; }
                v /= d;
            } else if (*p == L'%') {
                ++p;
                const double d = Unary();
                if (d == 0.0) { ok = false; return 0.0; }
                v = fmod(v, d);
            } else {
                return v;
            }
        }
    }

    double Unary() {
        Skip();
        if (*p == L'-') { ++p; return -Unary(); }
        if (*p == L'+') { ++p; return  Unary(); }
        return Power();
    }

    double Power() {
        const double base = Atom();
        Skip();
        // Right-associative, so 2^3^2 is 2^9 the way a calculator means it.
        if (*p == L'^') { ++p; return pow(base, Unary()); }
        return base;
    }

    double Atom() {
        Skip();
        if (*p == L'(') {
            ++p;
            const double v = Expr();
            if (!Eat(L')')) ok = false;
            return v;
        }

        if (iswalpha(*p)) {
            const wchar_t* start = p;
            while (iswalpha(*p)) ++p;
            const std::wstring fn = ToLower(std::wstring(start, p));

            if (fn == L"pi") return 3.14159265358979323846;
            if (fn == L"e")  return 2.71828182845904523536;

            if (!Eat(L'(')) { ok = false; return 0.0; }
            const double a = Expr();
            if (!Eat(L')')) { ok = false; return 0.0; }

            if (fn == L"sqrt")  { if (a < 0) { ok = false; return 0.0; } return sqrt(a); }
            if (fn == L"abs")   return fabs(a);
            if (fn == L"floor") return floor(a);
            if (fn == L"ceil")  return ceil(a);
            if (fn == L"round") return floor(a + 0.5);
            if (fn == L"sin")   return sin(a);
            if (fn == L"cos")   return cos(a);
            if (fn == L"tan")   return tan(a);
            if (fn == L"ln")    { if (a <= 0) { ok = false; return 0.0; } return log(a); }
            if (fn == L"log")   { if (a <= 0) { ok = false; return 0.0; } return log10(a); }
            ok = false;
            return 0.0;
        }

        wchar_t* end = nullptr;
        const double v = wcstod(p, &end);
        if (end == p) { ok = false; return 0.0; }
        p = end;
        return v;
    }
};

std::wstring FormatNumber(double v) {
    wchar_t buf[64];
    // Whole numbers should read as whole numbers: "8", not "8.0000000".
    if (fabs(v) < 1e15 && v == floor(v))
        swprintf_s(buf, L"%lld", (long long)v);
    else
        swprintf_s(buf, L"%.10g", v);
    return buf;
}

} // namespace

// ---------------------------------------------------------------- scoring
bool SearchScore(const wchar_t* name, size_t n, const std::wstring& lowQuery, int* out) {
    if (lowQuery.empty()) { *out = 0; return true; }
    if (!name) return false;

    int score = 0;
    size_t at = 0;
    bool  previousMatched = false;

    for (wchar_t want : lowQuery) {
        size_t found = std::wstring::npos;
        for (size_t j = at; j < n; ++j) {
            if (LowerFast(name[j]) == want) { found = j; break; }
        }
        if (found == std::wstring::npos) return false;

        const wchar_t before = found ? LowerFast(name[found - 1]) : L'\0';
        if (found == 0)                                  score += 20;  // first letter
        else if (before == L' ')                         score += 12;  // start of a word
        else if (before == L'-' || before == L'_' ||
                 before == L'.')                         score += 10;  // or of a dotted part
        else if (previousMatched)                        score += 8;   // run of letters
        else                                             score += 1;

        previousMatched = (found == at);
        at = found + 1;
    }

    // Prefix and exact-match bonuses, folded the same way.
    const size_t q = lowQuery.size();
    if (q <= n) {
        bool prefix = true;
        for (size_t i = 0; i < q; ++i)
            if (LowerFast(name[i]) != lowQuery[i]) { prefix = false; break; }
        if (prefix) {
            score += 40;
            if (q == n) score += 60;
        }
    }

    // A short name matching the same letters is the more likely target.
    score -= (int)(n / 8);
    *out = score;
    return true;
}

// ---------------------------------------------------------------- file index
std::vector<std::wstring> SearchDefaultFolders() {
    const KNOWNFOLDERID* ids[] = {
        &FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads,
        &FOLDERID_Pictures, &FOLDERID_Music, &FOLDERID_Videos,
    };
    std::vector<std::wstring> out;
    for (const KNOWNFOLDERID* id : ids) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &path)) && path)
            out.push_back(path);
        if (path) CoTaskMemFree(path);
    }
    return out;
}

void SearchInit(Config* cfg) {
    g_cfg = cfg;
    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
    if (!g_stop) g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!cfg || !cfg->searchFiles) return;

    const IndexWants wants = WantsFromConfig();

    // Reading the cache is one sequential pass over one file. Walking is tens
    // of thousands of seeks. Always try the cheap one first.
    if (LoadIndexCache(wants)) {
        if (IndexCacheAgeMs() > kCacheMaxAgeMs) {
            AWA_LOG(L"search: cache is stale, refreshing later");
            StartWalk(kRefreshDelayMs);
        }
        return;
    }
    StartWalk(kColdStartDelayMs);
}

void SearchShutdown() {
    // Set before the lock is taken, so the walk sees it at its very next
    // directory even if the UI thread is queued behind it for the section.
    InterlockedExchange(&g_abandonFlag, 1);
    if (g_lockReady) {
        EnterCriticalSection(&g_lock);
        g_abandon = true;
        LeaveCriticalSection(&g_lock);
    }
    if (g_stop) SetEvent(g_stop);      // cut short a walk that is still holding off

    bool exited = true;
    if (g_thread) {
        // A deep tree on a cold disk can take a while. If it has not finished,
        // abandon the handle rather than free what the thread still stands on.
        exited = (WaitForSingleObject(g_thread, 5000) == WAIT_OBJECT_0);
        if (exited) CloseHandle(g_thread);
        else AWA_LOG(L"search: index walk did not finish in time; abandoning it");
        g_thread = nullptr;
    }
    // Only once nothing can still be waiting on it. An abandoned walk is
    // parked inside WaitForSingleObject(g_stop, ...), so closing this handle
    // early would be the very thing invariant 12 exists to prevent.
    if (exited && g_stop) { CloseHandle(g_stop); g_stop = nullptr; }
    // g_lock is deliberately never deleted - see MAP.md invariant 12.
}

void SearchReindex() { StartWalk(0); }

void SearchApplyConfig() {
    if (!g_lockReady || !g_cfg) return;

    if (!g_cfg->searchFiles) {
        // Hand the memory back: nothing is going to read this index, and it is
        // the single largest thing the app holds.
        EnterCriticalSection(&g_lock);
        g_index.clear();
        g_index.shrink_to_fit();
        g_ready = false;
        LeaveCriticalSection(&g_lock);
        AWA_LOG(L"search: file index dropped");
        return;
    }

    const IndexWants wants = WantsFromConfig();

    EnterCriticalSection(&g_lock);
    const bool unchanged = g_ready && SameWants(g_wants, wants);
    LeaveCriticalSection(&g_lock);

    if (!unchanged) SearchReindex();
}

bool SearchIndexReady() {
    if (!g_lockReady) return false;
    EnterCriticalSection(&g_lock);
    const bool ready = g_ready;
    LeaveCriticalSection(&g_lock);
    return ready;
}

int SearchIndexCount() {
    if (!g_lockReady) return 0;
    EnterCriticalSection(&g_lock);
    const int n = (int)g_index.size();
    LeaveCriticalSection(&g_lock);
    return n;
}

namespace {

// One scan of the index, for one kind of thing. `wantExe` picks which half of
// it is being asked about: the two sources are capped separately, so a query
// matching a hundred documents cannot push the one program off the list.
//
// The whole thing runs under the lock and allocates nothing per candidate -
// `ScoreEntryName` scores the tail of a path in place. See search.h.
void CollectFromIndex(const std::wstring& lowQuery, int limit, bool wantExe,
                      std::vector<SearchHit>* out) {
    if (!g_lockReady || limit <= 0) return;
    // An empty query would rank the entire index; the app list is a far better
    // thing to show somebody who has not typed anything yet.
    if (lowQuery.empty()) return;

    struct Ranked { int score; size_t index; };
    std::vector<Ranked> ranked;

    EnterCriticalSection(&g_lock);

    ranked.reserve(256);
    for (size_t i = 0; i < g_index.size(); ++i) {
        const FileEntry& e = g_index[i];
        if (e.exe != wantExe) continue;
        int score = 0;
        if (!ScoreEntryName(e, lowQuery, &score)) continue;
        // Folders are usually a route to something rather than the thing
        // itself, so they yield to an equally good file.
        if (e.folder) score -= 2;
        ranked.push_back({ score, i });
    }

    const size_t keep = (std::min)(ranked.size(), (size_t)limit);
    std::partial_sort(ranked.begin(), ranked.begin() + (ptrdiff_t)keep, ranked.end(),
                      [](const Ranked& a, const Ranked& b) {
                          if (a.score != b.score) return a.score > b.score;
                          // Shorter path first, as a stable, allocation-free
                          // tiebreak - the deeper copy is rarely the one meant.
                          return g_index[a.index].path.size() <
                                 g_index[b.index].path.size();
                      });

    for (size_t i = 0; i < keep; ++i) {
        const FileEntry& e = g_index[ranked[i].index];
        SearchHit hit;
        hit.kind   = e.exe    ? HitKind::Program
                   : e.folder ? HitKind::Folder
                              : HitKind::File;
        hit.name   = e.Name();
        hit.detail = e.Folder();
        hit.target = e.path;
        hit.score  = ranked[i].score;
        // A program is something you open; a file is something you find. When
        // both match the same letters the program is nearly always the answer,
        // and this is the same bonus CollectApps gives an installed app.
        // How far down it is buried. `Vendor\Product\product.exe` is the
        // application; the same name four folders further in is one of its
        // parts, and a document seven folders deep is rarely the one meant
        // either. Depth is the only signal that separates them, and it is a
        // reliable one.
        int depth = 0;
        for (wchar_t c : e.path) if (c == L'\\') ++depth;
        hit.score -= (std::min)(24, depth * 3);

        if (e.exe) {
            hit.score += 10;
            // ".exe" on every row is four characters of nothing. The path
            // underneath still shows it, for the one case where two programs
            // share a name.
            if (hit.name.size() > 4) hit.name.resize(hit.name.size() - 4);
        }
        out->push_back(std::move(hit));
    }

    LeaveCriticalSection(&g_lock);
}

} // namespace

void SearchFiles(const std::wstring& lowQuery, int limit, std::vector<SearchHit>* out) {
    CollectFromIndex(lowQuery, limit, false, out);
}

void SearchPrograms(const std::wstring& lowQuery, int limit,
                    std::vector<SearchHit>* out) {
    CollectFromIndex(lowQuery, limit, true, out);
}

// ---------------------------------------------------------------- settings pages
void SearchSettingsPages(const std::wstring& lowQuery, int limit,
                         std::vector<SearchHit>* out) {
    if (lowQuery.empty() || limit <= 0) return;

    struct Ranked { int score; int index; };
    std::vector<Ranked> ranked;

    for (int i = 0; i < (int)ARRAYSIZE(kSettingsPages); ++i) {
        int score = 0;
        // The pointer form, so the page names are folded as they are scanned.
        // ToLower built a std::wstring per page per keystroke - fifty-one heap
        // allocations for every letter typed, to answer a question that needs
        // none. Same reasoning as the file index; see search.h.
        const wchar_t* name = kSettingsPages[i].name;
        if (!SearchScore(name, wcslen(name), lowQuery, &score)) continue;
        ranked.push_back({ score, i });
    }

    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const Ranked& a, const Ranked& b) { return a.score > b.score; });

    const size_t keep = (std::min)(ranked.size(), (size_t)limit);
    for (size_t i = 0; i < keep; ++i) {
        const SettingsPage& page = kSettingsPages[ranked[i].index];
        SearchHit hit;
        hit.kind   = HitKind::Setting;
        hit.name   = page.name;
        hit.detail = L"Windows settings";
        hit.target = page.uri;
        hit.score  = ranked[i].score;
        out->push_back(std::move(hit));
    }
}

// ---------------------------------------------------------------- calculator
bool SearchCalc(const std::wstring& query, std::wstring* result) {
    const std::wstring s = Trim(query);
    if (s.size() < 2 || s.size() > 200) return false;

    // It has to look like a sum before we try. Without this, "cos" alone or a
    // bare "2" would each produce a calculator row nobody asked for.
    bool hasDigit = false, hasOperator = false;
    for (wchar_t c : s) {
        if (iswdigit(c)) hasDigit = true;
        if (c == L'+' || c == L'-' || c == L'*' || c == L'/' ||
            c == L'%' || c == L'^' || c == L'(')
            hasOperator = true;
    }
    if (!hasDigit || !hasOperator) return false;

    Calc calc;
    calc.p = s.c_str();
    const double value = calc.Expr();
    calc.Skip();

    // Anything left over means this was not an expression after all - which is
    // exactly what stops "7 zip" turning into a broken sum.
    if (!calc.ok || *calc.p != L'\0') return false;
    if (!_finite(value)) return false;

    *result = FormatNumber(value);
    return true;
}

} // namespace awa
