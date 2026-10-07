#include "appicon.h"
#include <shlobj.h>
#include <shlwapi.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "msimg32.lib")     // AlphaBlend

namespace awa {

namespace {

// A cache entry. `bitmap` null with `tried` set means the shell has no icon for
// this target - a settings URI, a moniker that no longer resolves - and asking
// again would cost the same and fail the same way.
struct Icon {
    HBITMAP bitmap = nullptr;
    bool    tried  = false;
    bool    fresh  = false;   // fetched this run, so worth writing to the cache
    ULONGLONG lastUsed = 0;   // seconds (UnixSeconds); drives the 30-day cull on save
};

// Keyed by "<lowercased target>|<pixels>": the same application at two sizes is
// two bitmaps, and the search bar asks for one size at a time but changes size
// when it moves to a monitor with a different scaling.
CRITICAL_SECTION g_lock;
bool g_lockReady = false;

std::unordered_map<std::wstring, Icon> g_cache;
// What somebody is looking at right now. Taken from the back, so the newest
// query is served before the one before it.
std::vector<std::wstring> g_queue;
// What nobody has asked for yet. Drained only when `g_queue` is empty, and one
// at a time, so a catalogue of four hundred applications can be worked through
// without ever delaying a row that is on screen.
std::vector<std::wstring> g_prefetch;
size_t    g_prefetchAt    = 0;
ULONGLONG g_prefetchAfter = 0;   // not before this tick; see kPrefetchDelayMs

// Logging in, every startup program is fighting over the disk at once, and a
// sweep of four hundred shell icons is not what the user is waiting for. The
// same hold-off the file indexer takes, for the same reason - and rows that are
// actually on screen jump the queue throughout, so nothing is delayed by it.
constexpr DWORD kPrefetchDelayMs = 15 * 1000;

// A machine with a lot of applications, searched a lot, would otherwise grow
// this without limit. Far more than the eight rows on screen, so scrolling back
// and forth through a result list never evicts anything being looked at.
constexpr size_t kMaxIcons = 512;

// The file is read once and written once, so its only real cost is disk space:
// a 28-pixel icon is about 3 KB, and the ceiling above puts the whole thing
// comfortably under two megabytes.
constexpr unsigned kCacheMagic   = 0x43494D50;   // "PMIC"
// 3: rows are top-down, and stay that way across a save. Version 1 files hold
// every icon inverted; version 2 files hold the icons inverted on every second
// save, because ReadPixels guessed the orientation from the handle - see
// TopDownCopy. Either is thrown away rather than read; one background sweep
// rebuilds it.
// 4: one size only (the one the search bar draws) and a last_used stamp per
// entry; entries unused for 30 days are dropped on save.
constexpr unsigned kCacheVersion = 4;
constexpr ULONGLONG kEntryMaxAgeSec = 30ull * 24 * 60 * 60;

ULONGLONG UnixSeconds() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10000000ull;
}

int PixelsOfKey(const std::wstring& key) {
    const size_t bar = key.find_last_of(L'|');
    return bar == std::wstring::npos ? 0 : _wtoi(key.c_str() + bar + 1);
}
// A shell icon does change - an application updates, a shortcut is repointed -
// just not often. Rebuilding the lot once a fortnight costs one background pass
// nobody is waiting on.
constexpr ULONGLONG kCacheMaxAgeMs = 14ull * 24 * 60 * 60 * 1000;

HWND   g_notify   = nullptr;
UINT   g_message  = 0;
HANDLE g_thread   = nullptr;
HANDLE g_wake     = nullptr;
HANDLE g_quit     = nullptr;
LONG   g_stopping = 0;
bool   g_dirty    = false;   // something was fetched that the cache does not hold
// Set by AppIconRelease: the table was emptied, and the cache file should be
// read back before anything is fetched from the shell again.
bool   g_reload   = false;

bool Stopping() { return InterlockedCompareExchange(&g_stopping, 0, 0) != 0; }

std::wstring KeyFor(const std::wstring& target, int pixels) {
    return ToLower(target) + L"|" + std::to_wstring(pixels);
}

// ---------------------------------------------------------------- pixels
// The bits of a 32-bit DIB section, so an icon can be written to the cache and
// read back without going anywhere near the shell again.
struct Pixels {
    int w = 0, h = 0;
    std::vector<BYTE> bgra;          // top-down, premultiplied
};

// Every bitmap in the table is top-down - TopDownCopy makes sure of it on the
// way in - so the rows can be copied in memory order.
//
// They have to be, because the handle cannot say which way up it is: GetObject
// reports dsBmih.biHeight positive for every DIB section, top-down or not
// (measured on Windows 11; the sign the bitmap was created with is not kept).
// An earlier version of this function tested that sign, took every bitmap to
// be bottom-up, and turned it over - which was right for the shell's bitmaps
// and wrong for the ones MakeBitmap had just restored from the cache. So each
// save after a load inverted every restored icon, and the search bar showed
// them upside down from the second launch on, and again three minutes after it
// was last used, once the table started being released and read back.
bool ReadPixels(HBITMAP bitmap, Pixels* out) {
    DIBSECTION dib = {};
    if (GetObjectW(bitmap, sizeof(dib), &dib) != sizeof(dib)) return false;
    if (dib.dsBm.bmBitsPixel != 32 || !dib.dsBm.bmBits) return false;

    const int w = dib.dsBm.bmWidth;
    const int h = dib.dsBm.bmHeight;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return false;

    out->w = w;
    out->h = h;
    out->bgra.resize((size_t)w * (size_t)h * 4);
    const BYTE* src = static_cast<const BYTE*>(dib.dsBm.bmBits);
    for (int y = 0; y < h; ++y)
        memcpy(out->bgra.data() + (size_t)y * w * 4,
               src + (size_t)y * (size_t)dib.dsBm.bmWidthBytes, (size_t)w * 4);
    return true;
}

HBITMAP MakeBitmap(const Pixels& px) {
    if (px.w <= 0 || px.h <= 0) return nullptr;
    if (px.bgra.size() != (size_t)px.w * (size_t)px.h * 4) return nullptr;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = px.w;
    bi.bmiHeader.biHeight      = -px.h;          // top-down, as read
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return nullptr;
    }
    memcpy(bits, px.bgra.data(), px.bgra.size());
    return bmp;
}

// A top-down, 32-bit copy of whatever the shell handed back. GetDIBits knows
// the source's real orientation where GetObject does not, and asked for a
// negative height it turns a bottom-up source over itself; for a 32-bit source
// it copies the alpha byte through untouched (measured, pixel for pixel). The
// copy is made here, on the loader thread, before the bitmap is shared with
// anyone - GetDIBits must not run on a bitmap that is selected into a DC, and
// AppIconDraw selects every one it draws.
HBITMAP TopDownCopy(HBITMAP source) {
    BITMAP bm = {};
    if (GetObjectW(source, sizeof(bm), &bm) != sizeof(bm)) return nullptr;
    if (bm.bmWidth <= 0 || bm.bmHeight <= 0 || bm.bmWidth > 512 || bm.bmHeight > 512)
        return nullptr;

    Pixels px;
    px.w = bm.bmWidth;
    px.h = bm.bmHeight;
    px.bgra.resize((size_t)px.w * (size_t)px.h * 4);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = px.w;
    bi.bmiHeader.biHeight      = -px.h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;
    const int rows = GetDIBits(dc, source, 0, (UINT)px.h, px.bgra.data(), &bi, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (rows != px.h) return nullptr;

    return MakeBitmap(px);
}

// IShellItemImageFactory hands back a 32-bit DIB section with *straight* alpha.
// AlphaBlend wants it premultiplied, and the difference is not subtle - every
// icon comes out with a bright halo where it should be feathering into the
// background. Applied to the top-down copy, which is a DIB section of our
// own, so its bits can be corrected in place.
void Premultiply(HBITMAP bitmap) {
    DIBSECTION dib = {};
    if (GetObjectW(bitmap, sizeof(dib), &dib) != sizeof(dib)) return;
    if (dib.dsBm.bmBitsPixel != 32 || !dib.dsBm.bmBits) return;

    const int width  = dib.dsBm.bmWidth;
    const int height = dib.dsBm.bmHeight;
    BYTE* row = static_cast<BYTE*>(dib.dsBm.bmBits);

    for (int y = 0; y < height; ++y) {
        BYTE* p = row + (size_t)y * (size_t)dib.dsBm.bmWidthBytes;
        for (int x = 0; x < width; ++x, p += 4) {
            const unsigned a = p[3];
            if (a == 255) continue;
            if (a == 0) { p[0] = p[1] = p[2] = 0; continue; }
            p[0] = (BYTE)((p[0] * a + 127) / 255);
            p[1] = (BYTE)((p[1] * a + 127) / 255);
            p[2] = (BYTE)((p[2] * a + 127) / 255);
        }
    }
}

// The shell's own icon for anything it can parse: a path, a shortcut, a folder,
// or "shell:AppsFolder\<app id>" for a packaged app that has no file at all.
HBITMAP LoadIconBitmap(const std::wstring& target, int pixels) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(target.c_str(), nullptr,
                                           IID_PPV_ARGS(&item))) || !item)
        return nullptr;

    HBITMAP bitmap = nullptr;
    IShellItemImageFactory* factory = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&factory))) && factory) {
        const SIZE want = { pixels, pixels };
        // ICONONLY, so a folder of photographs does not turn the result list
        // into a thumbnail grid - and, more to the point, so this never waits
        // on a thumbnail being generated. BIGGERSIZEOK lets the shell hand back
        // the next size up rather than rescale a small one badly.
        if (FAILED(factory->GetImage(want, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK,
                                     &bitmap)))
            bitmap = nullptr;
        factory->Release();
    }
    item->Release();

    if (!bitmap) return nullptr;

    // Kept in our own orientation, not the shell's; see TopDownCopy. Nothing
    // is drawn from the shell's bitmap, so it goes as soon as it is copied.
    HBITMAP copy = TopDownCopy(bitmap);
    DeleteObject(bitmap);
    if (copy) Premultiply(copy);
    return copy;
}

// ---------------------------------------------------------------- the cache
std::wstring CachePath() { return ConfigDir() + L"\\icons.cache"; }

ULONGLONG CacheAgeMs() {
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExW(CachePath().c_str(), GetFileExInfoStandard, &info))
        return ~0ull;
    ULARGE_INTEGER written;
    written.LowPart  = info.ftLastWriteTime.dwLowDateTime;
    written.HighPart = info.ftLastWriteTime.dwHighDateTime;

    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart  = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    if (now.QuadPart <= written.QuadPart) return 0;
    return (now.QuadPart - written.QuadPart) / 10000ull;
}

template <typename T>
bool ReadPod(FILE* f, T* out) {
    return fread(out, sizeof(T), 1, f) == 1;
}

// One sequential pass over one file, on the loader thread, before it serves
// anything. Everything it restores is an icon the shell never has to be asked
// for again.
void LoadCache() {
    if (CacheAgeMs() > kCacheMaxAgeMs) return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, CachePath().c_str(), L"rb") != 0 || !f) return;

    unsigned magic = 0, version = 0, count = 0;
    if (!ReadPod(f, &magic) || !ReadPod(f, &version) || !ReadPod(f, &count) ||
        magic != kCacheMagic || version != kCacheVersion || count > kMaxIcons) {
        fclose(f);
        return;
    }

    int restored = 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned keyLen = 0, bytes = 0;
        int w = 0, h = 0;
        ULONGLONG used = 0;
        if (!ReadPod(f, &used) || !ReadPod(f, &keyLen) || keyLen == 0 || keyLen > 1024) break;

        std::wstring key(keyLen, L'\0');
        if (fread(&key[0], sizeof(wchar_t), keyLen, f) != keyLen) break;
        if (!ReadPod(f, &w) || !ReadPod(f, &h) || !ReadPod(f, &bytes)) break;
        // A truncated or tampered file must not turn into a huge allocation.
        if (w <= 0 || h <= 0 || w > 512 || h > 512 ||
            bytes != (unsigned)w * (unsigned)h * 4) break;

        Pixels px;
        px.w = w;
        px.h = h;
        px.bgra.resize(bytes);
        if (fread(px.bgra.data(), 1, bytes, f) != bytes) break;

        HBITMAP bmp = MakeBitmap(px);
        if (!bmp) continue;

        EnterCriticalSection(&g_lock);
        Icon& slot = g_cache[key];
        if (slot.bitmap) DeleteObject(bmp);       // a live fetch beat us to it
        else { slot.bitmap = bmp; ++restored; }
        if (used > slot.lastUsed) slot.lastUsed = used;
        slot.tried = true;
        LeaveCriticalSection(&g_lock);
    }
    fclose(f);

    AWA_LOG(L"icons: %d restored from the cache", restored);
    if (restored > 0 && g_notify && g_message)
        PostMessageW(g_notify, g_message, 0, 0);
}

template <typename T>
void WritePod(FILE* f, const T& v) { fwrite(&v, sizeof(T), 1, f); }

void SaveCache() {
    // Read out under the lock and written outside it: the file is a couple of
    // megabytes and holding the section across the write would stall the next
    // keystroke in the search bar for as long as it took.
    struct Entry { std::wstring key; ULONGLONG used; Pixels px; };
    std::vector<Entry> entries;
    const ULONGLONG now = UnixSeconds();

    EnterCriticalSection(&g_lock);
    // Only the size the search bar draws is worth keeping: the one most
    // entries have. Another size (a second monitor's scaling) is refetched.
    std::unordered_map<int, int> sizes;
    for (const auto& e : g_cache)
        if (e.second.bitmap) ++sizes[PixelsOfKey(e.first)];
    int keep = 0, keepN = 0;
    for (const auto& s : sizes)
        if (s.second > keepN || (s.second == keepN && s.first > keep)) { keep = s.first; keepN = s.second; }

    entries.reserve(g_cache.size());
    for (const auto& e : g_cache) {
        if (!e.second.bitmap) continue;
        if (PixelsOfKey(e.first) != keep) continue;
        const ULONGLONG used = e.second.lastUsed ? e.second.lastUsed : now;
        if (now > used && now - used > kEntryMaxAgeSec) continue;
        Entry entry;
        entry.key = e.first;
        entry.used = used;
        if (!ReadPixels(e.second.bitmap, &entry.px)) continue;
        entries.push_back(std::move(entry));
        if (entries.size() >= kMaxIcons) break;
    }
    LeaveCriticalSection(&g_lock);

    if (entries.empty()) return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, CachePath().c_str(), L"wb") != 0 || !f) return;

    WritePod(f, kCacheMagic);
    WritePod(f, kCacheVersion);
    WritePod(f, (unsigned)entries.size());
    for (const Entry& e : entries) {
        WritePod(f, e.used);
        WritePod(f, (unsigned)e.key.size());
        fwrite(e.key.data(), sizeof(wchar_t), e.key.size(), f);
        WritePod(f, e.px.w);
        WritePod(f, e.px.h);
        WritePod(f, (unsigned)e.px.bgra.size());
        fwrite(e.px.bgra.data(), 1, e.px.bgra.size(), f);
    }
    fclose(f);
    AWA_LOG(L"icons: %d written to the cache", (int)entries.size());
}

// ---------------------------------------------------------------- the loader
// Takes the next thing to fetch. Anything on screen first, and only then one
// item of the background sweep - so a four-hundred-entry prefetch never gets
// between a row and its icon.
// Nothing to do now, but there is a sweep waiting for its hold-off to expire:
// the loader parks on a timeout rather than for ever.
constexpr DWORD kNothingToDo = INFINITE;

DWORD NextKey(std::wstring* key, bool* background) {
    *background = false;
    EnterCriticalSection(&g_lock);
    while (!g_queue.empty()) {
        *key = std::move(g_queue.back());
        g_queue.pop_back();
        // Queued while the table was empty after a release, and then restored
        // by the reload that ran before this call: nothing to fetch.
        auto it = g_cache.find(*key);
        if (it != g_cache.end() && it->second.tried) continue;
        LeaveCriticalSection(&g_lock);
        return 0;
    }

    const ULONGLONG now = GetTickCount64();
    if (g_prefetchAt < g_prefetch.size() && now < g_prefetchAfter) {
        const DWORD wait = (DWORD)(g_prefetchAfter - now);
        LeaveCriticalSection(&g_lock);
        return wait;
    }

    while (g_prefetchAt < g_prefetch.size()) {
        // Copied, not moved: the sweep starts over from the same list after a
        // release, and a moved-from entry would come back as an empty key.
        std::wstring candidate = g_prefetch[g_prefetchAt++];
        auto it = g_cache.find(candidate);
        if (it != g_cache.end() && it->second.tried) continue;   // already have it
        g_cache[candidate];                    // reserve, so it is queued once
        *key = std::move(candidate);
        *background = true;
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    LeaveCriticalSection(&g_lock);
    return kNothingToDo;
}

DWORD WINAPI LoaderThread(LPVOID) {
    // Its own apartment: the shell wants one, and the UI thread's must not be
    // borrowed for work that can take a while.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    LoadCache();

    HANDLE waits[2] = { g_quit, g_wake };
    bool lowered = false;
    for (;;) {
        bool reload = false;
        EnterCriticalSection(&g_lock);
        reload = g_reload;
        g_reload = false;
        LeaveCriticalSection(&g_lock);
        if (reload) {
            // Released while hidden; somebody is looking again. The cache
            // first, so the sweep below finds most of what it wants already
            // there and asks the shell for nothing.
            LoadCache();
            EnterCriticalSection(&g_lock);
            g_prefetchAt    = 0;
            g_prefetchAfter = GetTickCount64() + kPrefetchDelayMs;
            LeaveCriticalSection(&g_lock);
        }

        std::wstring key;
        bool background = false;
        const DWORD idle = NextKey(&key, &background);
        if (idle != 0) {
            if (WaitForMultipleObjects(2, waits, FALSE, idle) == WAIT_OBJECT_0)
                break;                                   // told to quit
            continue;                                    // woken, or the wait expired
        }
        if (Stopping()) break;

        // Background mode drops this thread's *I/O* priority as well as its
        // CPU priority, which is the whole game on a mechanical disk: the sweep
        // is hundreds of small reads nobody is waiting for, and it must yield
        // to a row that somebody is looking at right now.
        if (background != lowered) {
            SetThreadPriority(GetCurrentThread(),
                              background ? THREAD_MODE_BACKGROUND_BEGIN
                                         : THREAD_MODE_BACKGROUND_END);
            lowered = background;
        }

        // The key carries the size; the target is everything before it.
        const size_t bar = key.find_last_of(L'|');
        if (bar == std::wstring::npos) continue;
        const std::wstring target = key.substr(0, bar);
        const int pixels = _wtoi(key.c_str() + bar + 1);
        if (pixels <= 0) continue;

        const ULONGLONG began = GetTickCount64();
        HBITMAP bitmap = LoadIconBitmap(target, pixels);
        const ULONGLONG took = GetTickCount64() - began;
        // Only interesting when it is slow; a cheap one is noise in the log.
        if (took >= 40)
            AWA_LOG(L"icon %llu ms%s: %s", took, bitmap ? L"" : L" (none)",
                    target.c_str());

        bool announce = false;
        EnterCriticalSection(&g_lock);
        if (Stopping()) {
            LeaveCriticalSection(&g_lock);
            if (bitmap) DeleteObject(bitmap);
            break;
        }
        Icon& slot = g_cache[key];
        // Somebody may have raced us to it; keep the first answer so the
        // handle the UI is already drawing with stays valid.
        if (slot.bitmap) {
            if (bitmap) DeleteObject(bitmap);
        } else if (bitmap) {
            slot.bitmap = bitmap;
            slot.fresh  = true;
            slot.lastUsed = UnixSeconds();
            g_dirty     = true;
            announce    = true;
        }
        slot.tried = true;
        LeaveCriticalSection(&g_lock);

        if (announce && g_notify && g_message)
            PostMessageW(g_notify, g_message, 0, 0);
    }

    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}

} // namespace

void AppIconInit(HWND notify, UINT message) {
    g_notify  = notify;
    g_message = message;
    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
    if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    if (!g_quit) g_quit = CreateEventW(nullptr, TRUE,  FALSE, nullptr);
    if (!g_thread && g_wake && g_quit)
        g_thread = CreateThread(nullptr, 0, LoaderThread, nullptr, 0, nullptr);
}

void AppIconShutdown() {
    // Written before the thread is told to stop, while everything it fetched
    // this run is still in the table.
    bool dirty = false;
    if (g_lockReady) {
        EnterCriticalSection(&g_lock);
        dirty = g_dirty;
        LeaveCriticalSection(&g_lock);
    }
    if (dirty) SaveCache();

    InterlockedExchange(&g_stopping, 1);
    if (g_quit) SetEvent(g_quit);

    if (g_thread) {
        // A shell call can sit for a while on a cold disk. If it does, abandon
        // the handle rather than free what the thread is standing on - the same
        // bargain the file indexer and the app scan make.
        if (WaitForSingleObject(g_thread, 3000) == WAIT_OBJECT_0) {
            CloseHandle(g_thread);
            g_thread = nullptr;
        } else {
            AWA_LOG(L"icon loader did not finish in time; abandoning it");
            g_thread = nullptr;
            return;      // its handles and the cache stay put; the process is going
        }
    }

    if (g_lockReady) {
        EnterCriticalSection(&g_lock);
        for (auto& e : g_cache) if (e.second.bitmap) DeleteObject(e.second.bitmap);
        g_cache.clear();
        g_queue.clear();
        g_prefetch.clear();
        LeaveCriticalSection(&g_lock);
    }
    if (g_wake) { CloseHandle(g_wake); g_wake = nullptr; }
    if (g_quit) { CloseHandle(g_quit); g_quit = nullptr; }
    // g_lock is deliberately never deleted - see MAP.md invariant 12.
}

HBITMAP AppIconFor(const std::wstring& target, int pixels) {
    if (!g_lockReady || target.empty() || pixels <= 0 || Stopping()) return nullptr;

    const std::wstring key = KeyFor(target, pixels);

    bool queue = false;
    HBITMAP found = nullptr;

    EnterCriticalSection(&g_lock);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
        found = it->second.bitmap;
        if (found) it->second.lastUsed = UnixSeconds();
    } else if (g_cache.size() < kMaxIcons) {
        // Reserved right away, so eight rows redrawn on every keystroke queue
        // each icon once rather than once per keystroke.
        g_cache.emplace(key, Icon{});
        g_queue.push_back(key);
        queue = true;
    }
    LeaveCriticalSection(&g_lock);

    if (queue && g_wake) SetEvent(g_wake);
    return found;
}

void AppIconRelease() {
    if (!g_lockReady || Stopping()) return;
    bool dirty = false;
    EnterCriticalSection(&g_lock);
    dirty = g_dirty;
    LeaveCriticalSection(&g_lock);
    // Anything fetched this run goes to the file first, or it would be
    // fetched from the shell all over again.
    if (dirty) SaveCache();

    int freed = 0;
    EnterCriticalSection(&g_lock);
    for (auto& e : g_cache)
        if (e.second.bitmap) { DeleteObject(e.second.bitmap); ++freed; }
    g_cache.clear();
    g_queue.clear();
    g_dirty  = false;
    g_reload = true;
    LeaveCriticalSection(&g_lock);
    AWA_LOG(L"icons: %d released; the cache will be read back on demand", freed);
}

void AppIconPrefetch(const std::vector<std::wstring>& targets, int pixels) {
    if (!g_lockReady || pixels <= 0 || Stopping() || targets.empty()) return;

    EnterCriticalSection(&g_lock);
    // Anything left of a previous sweep is superseded: the catalogue it was
    // built from has just been replaced.
    g_prefetch.clear();
    g_prefetchAt    = 0;
    g_prefetchAfter = GetTickCount64() + kPrefetchDelayMs;
    g_prefetch.reserve(targets.size());
    for (const std::wstring& target : targets) {
        if (target.empty()) continue;
        g_prefetch.push_back(KeyFor(target, pixels));
    }
    LeaveCriticalSection(&g_lock);

    if (g_wake) SetEvent(g_wake);
}

void AppIconDraw(HDC dc, HBITMAP icon, const RECT& box) {
    if (!dc || !icon) return;

    BITMAP info = {};
    if (GetObjectW(icon, sizeof(info), &info) != sizeof(info)) return;
    if (info.bmWidth <= 0 || info.bmHeight <= 0) return;

    const int boxW = box.right - box.left;
    const int boxH = box.bottom - box.top;
    if (boxW <= 0 || boxH <= 0) return;

    // Contain, never stretch: shell icons are square, but a bitmap handed back
    // at the next size up is not always the size that was asked for.
    const int side = (std::min)(boxW, boxH);
    const int w = (info.bmWidth >= info.bmHeight)
                  ? side : side * info.bmWidth / info.bmHeight;
    const int h = (info.bmHeight >= info.bmWidth)
                  ? side : side * info.bmHeight / info.bmWidth;

    HDC mem = CreateCompatibleDC(dc);
    if (!mem) return;
    HGDIOBJ old = SelectObject(mem, icon);

    SetStretchBltMode(dc, HALFTONE);
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(dc, box.left + (boxW - w) / 2, box.top + (boxH - h) / 2, w, h,
               mem, 0, 0, info.bmWidth, info.bmHeight, blend);

    SelectObject(mem, old);
    DeleteDC(mem);
}

} // namespace awa

namespace awa {
int AppIconCount() {
    if (!g_lockReady) return 0;
    EnterCriticalSection(&g_lock);
    const int n = (int)g_cache.size();
    LeaveCriticalSection(&g_lock);
    return n;
}
} // namespace awa
