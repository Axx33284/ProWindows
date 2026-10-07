// Checks that a shell icon survives the search bar's icon cache the right way
// up: fetched, written to icons.cache, released, read back, written again.
//
// This exists because the orientation of a bitmap cannot be read off its
// handle - GetObject reports every DIB section as bottom-up - so a mistake
// here is invisible to the code and only shows on screen, and only from the
// second launch (or the second save) on. The check compares the alpha channel
// row by row against a fresh top-down copy taken straight from the shell,
// which premultiplication does not touch.
//
// No window is created. It uses the real %APPDATA%\ProWindows\icons.cache,
// like the search bar does, and adds one or two entries to it.
//
//   iconcache            the check, PASS or FAIL per stage
//
// Build it with tests\iconcache.bat.
#include "../src/appicon.h"
#include <shobjidl.h>
#include <cstdio>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

using namespace awa;

namespace {

const wchar_t* kTarget = L"C:\\Windows\\System32\\notepad.exe";
constexpr int  kPixels = 28;

// One byte per pixel: alpha, in top-down row order.
using AlphaMap = std::vector<BYTE>;

// The reference: the shell's bitmap, read through GetDIBits as top-down.
bool Reference(AlphaMap* out, int* w, int* h) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(kTarget, nullptr, IID_PPV_ARGS(&item))) || !item)
        return false;
    HBITMAP bmp = nullptr;
    IShellItemImageFactory* factory = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&factory))) && factory) {
        const SIZE want = { kPixels, kPixels };
        if (FAILED(factory->GetImage(want, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bmp)))
            bmp = nullptr;
        factory->Release();
    }
    item->Release();
    if (!bmp) return false;

    BITMAP bm = {};
    GetObjectW(bmp, sizeof(bm), &bm);
    std::vector<BYTE> px((size_t)bm.bmWidth * bm.bmHeight * 4);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = bm.bmWidth;
    bi.bmiHeader.biHeight      = -bm.bmHeight;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    const int rows = GetDIBits(dc, bmp, 0, (UINT)bm.bmHeight, px.data(), &bi, DIB_RGB_COLORS);
    DeleteDC(dc);
    DeleteObject(bmp);
    if (rows != bm.bmHeight) return false;

    *w = bm.bmWidth;
    *h = bm.bmHeight;
    out->resize((size_t)bm.bmWidth * bm.bmHeight);
    for (size_t i = 0; i < out->size(); ++i) (*out)[i] = px[i * 4 + 3];
    return true;
}

// The bits of a bitmap from AppIconFor, in memory order - which the contract
// says is top-down.
bool AlphaOf(HBITMAP bmp, AlphaMap* out, int* w, int* h) {
    DIBSECTION dib = {};
    if (GetObjectW(bmp, sizeof(dib), &dib) != sizeof(dib)) return false;
    if (dib.dsBm.bmBitsPixel != 32 || !dib.dsBm.bmBits) return false;
    *w = dib.dsBm.bmWidth;
    *h = dib.dsBm.bmHeight;
    out->resize((size_t)*w * *h);
    const BYTE* bits = static_cast<const BYTE*>(dib.dsBm.bmBits);
    for (int y = 0; y < *h; ++y)
        for (int x = 0; x < *w; ++x)
            (*out)[(size_t)y * *w + x] = bits[(size_t)y * dib.dsBm.bmWidthBytes + x * 4 + 3];
    return true;
}

// Polls until the loader has answered, up to `ms`.
HBITMAP WaitFor(const std::wstring& target, int pixels, DWORD ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    for (;;) {
        HBITMAP b = AppIconFor(target, pixels);
        if (b || GetTickCount64() >= until) return b;
        Sleep(20);
    }
}

bool Same(const AlphaMap& a, const AlphaMap& b) { return a == b; }
bool Flipped(const AlphaMap& a, const AlphaMap& b, int w, int h) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (a[(size_t)y * w + x] != b[(size_t)(h - 1 - y) * w + x]) return false;
    return true;
}

int g_failed = 0;

void Check(const wchar_t* stage, const AlphaMap& ref, int rw, int rh) {
    HBITMAP bmp = WaitFor(kTarget, kPixels, 8000);
    AlphaMap got;
    int w = 0, h = 0;
    if (!bmp || !AlphaOf(bmp, &got, &w, &h)) {
        wprintf(L"  FAIL  %-34s no bitmap\n", stage);
        ++g_failed;
        return;
    }
    if (w != rw || h != rh) {
        wprintf(L"  FAIL  %-34s %dx%d, reference is %dx%d\n", stage, w, h, rw, rh);
        ++g_failed;
        return;
    }
    if (Same(got, ref))             wprintf(L"  PASS  %s\n", stage);
    else if (Flipped(got, ref, w, h)) { wprintf(L"  FAIL  %-34s upside down\n", stage); ++g_failed; }
    else                            { wprintf(L"  FAIL  %-34s pixels differ\n", stage); ++g_failed; }
}

} // namespace

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LogEnable(true);

    AlphaMap ref;
    int rw = 0, rh = 0;
    if (!Reference(&ref, &rw, &rh)) {
        wprintf(L"the shell has no icon for %s\n", kTarget);
        return 2;
    }
    wprintf(L"reference: %s at %dx%d\n\n", kTarget, rw, rh);

    AppIconInit(nullptr, 0);

    // 1. Straight from the loader, or from a cache a previous run wrote.
    Check(L"fetched or restored", ref, rw, rh);

    // 2. Something that is not in the cache, so the release below has a
    //    reason to write the file; then release and read back.
    const wchar_t* kOther = L"C:\\Windows\\explorer.exe";
    const wchar_t* kOther2 = L"C:\\Windows\\System32\\cmd.exe";
    WaitFor(kOther, kPixels, 8000);
    AppIconRelease();
    Check(L"after a save and a reload", ref, rw, rh);

    // 3. The case that bit: a save whose entries came from a load, not the
    //    shell, and a reload of that.
    WaitFor(kOther2, kPixels, 8000);
    AppIconRelease();
    Check(L"after a second save and reload", ref, rw, rh);

    AppIconShutdown();
    CoUninitialize();

    wprintf(L"\n%s\n", g_failed ? L"FAILED" : L"all passed");
    return g_failed ? 1 : 0;
}
