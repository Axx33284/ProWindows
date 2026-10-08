// ProWindows - tests for the File Explorer styler's shim (src\explorer\
// windhawk_shim.cpp) and the ProWindows side that writes its settings
// (src\explorerstyler.cpp). All in this process: nothing is injected anywhere
// and Explorer is never touched. The "mod" here is a few stubs that look at
// what the shim hands them and hook a dummy function with MinHook.
//
// Usage: styler_test.exe <repo root>
#include "../src/explorer/windhawk_shim.h"
#include "../src/explorerstyler.h"
#include "../src/common.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

using namespace awa;

// ---------------------------------------------------------------- what the shim calls out to
namespace awa {
void AppTrayBalloon(const wchar_t*, const wchar_t*) {}
void AppSaveConfig() {}
void AppRefreshSettings() {}
}

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; \
    wprintf(L"  FAIL line %d: %S\n", __LINE__, #cond); } } while (0)

// A function worth hooking: long enough for a five byte patch, not inlined.
using DummyFn = int (*)(int);
__declspec(noinline) static int Dummy(int x) {
    volatile int acc = 0;
    for (int i = 0; i < x; ++i) acc = acc + i * 3 + 1;
    return acc + 7;
}
static DummyFn g_dummyOriginal;
static std::atomic<int> g_hookCalls{0};
static int DummyHook(int x) {
    ++g_hookCalls;
    return g_dummyOriginal(x) + 1000;
}

static std::atomic<int> g_inits{0}, g_afterInits{0}, g_uninits{0}, g_changes{0};
static std::wstring g_themeAtInit, g_themeAtChange;
static std::wstring g_style0AtInit, g_target1AtInit;
static int g_heightAtInit = -1;
static bool g_hooked = false;

BOOL Wh_ModInit() {
    ++g_inits;
    PCWSTR t = Wh_GetStringSetting(L"theme");
    g_themeAtInit = t;
    Wh_FreeStringSetting(t);
    PCWSTR a = Wh_GetStringSetting(L"controlStyles[%d].styles[%d]", 0, 0);
    g_style0AtInit = a;
    Wh_FreeStringSetting(a);
    PCWSTR b = Wh_GetStringSetting(L"controlStyles[%d].target", 1);
    g_target1AtInit = b;
    Wh_FreeStringSetting(b);
    g_heightAtInit = Wh_GetIntSetting(L"explorerFrameContainerHeight");
    g_hooked = WindhawkUtils::SetFunctionHook(Dummy, DummyHook, &g_dummyOriginal);
    return TRUE;
}
void Wh_ModAfterInit() { ++g_afterInits; }
void Wh_ModUninit() { ++g_uninits; }
void Wh_ModSettingsChanged() {
    PCWSTR t = Wh_GetStringSetting(L"theme");
    g_themeAtChange = t;
    Wh_FreeStringSetting(t);
    ++g_changes;
}

extern "C" DWORD WINAPI StylerStart(LPVOID);

// ---------------------------------------------------------------- helpers
static std::wstring ReadAll(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string bytes;
    char buf[4096];
    DWORD got;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) bytes.append(buf, got);
    CloseHandle(f);
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &out[0], n);
    return out;
}

static void WriteAll(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string bytes(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), &bytes[0], n, nullptr, nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote;
    WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr);
    CloseHandle(f);
}

static size_t Count(const std::wstring& hay, const std::wstring& needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::wstring::npos; p = hay.find(needle, p + needle.size())) ++n;
    return n;
}

template <class F>
static bool WaitFor(F cond, DWORD ms = 5000) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        if (cond()) return true;
        Sleep(10);
    }
    return cond();
}

static bool SetNamed(const wchar_t* what) {
    std::wstring name = std::wstring(L"Local\\ProWindows.Styler.") + what + L"." +
                        std::to_wstring(GetCurrentProcessId());
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
    if (!ev) return false;
    SetEvent(ev);
    CloseHandle(ev);
    return true;
}

// ---------------------------------------------------------------- tests
static void Choices() {
    wprintf(L"the choices the settings page offers\n");
    CHECK(ExplorerThemeCount() == 18);
    CHECK(std::wstring(ExplorerThemeId(0)) == L"ProWindows");
    CHECK(ExplorerThemeIndexOf(L"ProWindows") == 0);
    CHECK(ExplorerThemeIndexOf(L"") == 1);
    CHECK(ExplorerThemeIndexOf(L"Matter") > 1);
    CHECK(ExplorerThemeIndexOf(L"no such theme") == -1);
    for (int i = 0; i < ExplorerThemeCount(); ++i)
        for (int j = i + 1; j < ExplorerThemeCount(); ++j)
            CHECK(std::wstring(ExplorerThemeId(i)) != ExplorerThemeId(j) &&
                  std::wstring(ExplorerThemeName(i)) != ExplorerThemeName(j));
    CHECK(ExplorerEffectCount() == 6);
    CHECK(std::wstring(ExplorerEffectId(0)).empty());
    CHECK(ExplorerEffectIndexOf(L"micaalt") == 4);
    CHECK(ExplorerEffectIndexOf(L"bogus") == -1);
}

// Every theme the page offers must be one the mod's GetSelectedTheme knows,
// and the ProWindows theme must be what the plan says it is.
static void ThemeTable(const std::wstring& root) {
    wprintf(L"the mod's theme table\n");
    const std::wstring src = ReadAll(root + L"\\src\\explorer\\styler.cpp");
    CHECK(!src.empty());
    for (int i = 0; i < ExplorerThemeCount(); ++i) {
        const std::wstring id = ExplorerThemeId(i);
        if (id.empty()) continue;
        const bool known = src.find(L"wcscmp(themeName, L\"" + id + L"\") == 0") != std::wstring::npos;
        if (!known) wprintf(L"  theme not selectable by the mod: %s\n", id.c_str());
        CHECK(known);
    }

    const size_t b = src.find(L"const Theme g_themeProWindows = {{");
    CHECK(b != std::wstring::npos);
    const size_t e = b == std::wstring::npos ? b : src.find(L"BackgroundTranslucentEffect::kNone};", b);
    CHECK(e != std::wstring::npos);
    if (b != std::wstring::npos && e != std::wstring::npos) {
        const std::wstring theme = src.substr(b, e - b);
        CHECK(theme.find(L"#000000") != std::wstring::npos);
        CHECK(theme.find(L"#1A1A1A") != std::wstring::npos);
        CHECK(theme.find(L"#3A3A3A") != std::wstring::npos);
        CHECK(theme.find(L"#E0E0E0") != std::wstring::npos);
        CHECK(theme.find(L"SystemAccentColor") == std::wstring::npos);   // no accent colour
        CHECK(theme.find(L"CommandBarControlRootGrid") != std::wstring::npos);
        CHECK(theme.find(L"NavigationBarControlGrid") != std::wstring::npos);
    }
    // No telemetry survives the port.
    CHECK(src.find(L"StatsTimer") == std::wstring::npos);
    CHECK(src.find(L"statsTimerLastTime") == std::wstring::npos);
}

static void IniFile(const std::wstring& dir) {
    wprintf(L"the ini ProWindows writes\n");
    const std::wstring path = ExplorerStylerIniPath();
    CHECK(path == dir + L"\\explorer-styler.ini");

    CHECK(ExplorerStylerWriteIni(L"Matter", L"mica", true));      // creates it
    std::wstring text = ReadAll(path);
    CHECK(text.find(L"theme=Matter") != std::wstring::npos);
    CHECK(text.find(L"backgroundTranslucentEffect=mica") != std::wstring::npos);
    CHECK(text.find(L"debug=1") != std::wstring::npos);
    CHECK(text.find(L"controlStyles[0].target") != std::wstring::npos);   // the commented template
    CHECK(!ExplorerStylerWriteIni(L"Matter", L"mica", true));     // nothing to change

    // The user's own lines survive a rewrite, and the managed block is replaced, not repeated.
    WriteAll(path, text + L"controlStyles[0].target=Grid#Mine\r\ncontrolStyles[0].styles[0]=Background=#112233\r\n");
    CHECK(ExplorerStylerWriteIni(L"TintedGlass", L"acrylic", false));
    text = ReadAll(path);
    CHECK(Count(text, L"theme=") == 1);
    CHECK(text.find(L"theme=TintedGlass") != std::wstring::npos);
    CHECK(text.find(L"backgroundTranslucentEffect=acrylic") != std::wstring::npos);
    CHECK(text.find(L"debug=0") != std::wstring::npos);
    CHECK(Count(text, L"controlStyles[0].target=Grid#Mine") == 1);
    CHECK(Count(text, L"Background=#112233") == 1);
    CHECK(Count(text, L"managed by ProWindows") == 1);

    // The effect names the mod's own setting uses.
    ExplorerStylerWriteIni(L"", L"blur", false);
    CHECK(ReadAll(path).find(L"backgroundTranslucentEffect=acrylicblur") != std::wstring::npos);
    ExplorerStylerWriteIni(L"", L"micaalt", false);
    CHECK(ReadAll(path).find(L"backgroundTranslucentEffect=micaAlt") != std::wstring::npos);
    ExplorerStylerWriteIni(L"ProWindows", L"", false);
    text = ReadAll(path);
    CHECK(text.find(L"theme=ProWindows") != std::wstring::npos);
    CHECK(text.find(L"backgroundTranslucentEffect=\r\n") != std::wstring::npos);
    CHECK(Count(text, L"Background=#112233") == 1);
}

static void Lifecycle(const std::wstring& dir) {
    wprintf(L"the shim in this process: settings, hooks, reload, stop\n");
    const std::wstring path = ExplorerStylerIniPath();

    // Indexed keys as the mod reads them, a user line overriding a managed one.
    WriteAll(path, L"; managed by hand for the test\r\ntheme=Matter\r\ndebug=1\r\nexplorerFrameContainerHeight=87\r\n"
                   L"controlStyles[0].target=Grid#A\r\n"
                   L"controlStyles[0].styles[0]=Background:=<SolidColorBrush Color=\"#FF0000\"/>\r\n"
                   L"controlStyles[1].target=Grid#B\r\n");

    const int plain = Dummy(3);
    CHECK(plain == 19);

    CHECK(StylerStart((LPVOID)dir.c_str()) == 1);
    CHECK(g_inits == 1 && g_afterInits == 1);
    CHECK(g_hooked);
    CHECK(g_themeAtInit == L"Matter");
    CHECK(g_style0AtInit == L"Background:=<SolidColorBrush Color=\"#FF0000\"/>");   // '=' and quotes intact
    CHECK(g_target1AtInit == L"Grid#B");
    CHECK(g_heightAtInit == 87);

    CHECK(Dummy(3) == plain + 1000);             // hooked, original still reached
    CHECK(g_hookCalls == 1);

    // The log is written because debug=1.
    CHECK(GetFileAttributesW((dir + L"\\explorer-styler.log").c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(ReadAll(dir + L"\\explorer-styler.log").find(L"StylerStart") != std::wstring::npos);

    // Storage path for the image cache.
    wchar_t store[MAX_PATH];
    CHECK(Wh_GetModStoragePath(store, ARRAYSIZE(store)));
    CHECK(std::wstring(store) == dir + L"\\explorer-styler");
    CHECK(GetFileAttributesW(store) != INVALID_FILE_ATTRIBUTES);

    // Reload: the ini is read again, then the mod is told.
    WriteAll(path, L"theme=Float\r\ndebug=0\r\n");
    CHECK(SetNamed(L"Reload"));
    CHECK(WaitFor([] { return g_changes.load() == 1; }));
    CHECK(g_themeAtChange == L"Float");
    PCWSTR gone = Wh_GetStringSetting(L"controlStyles[%d].target", 0);
    CHECK(gone && !*gone);                       // a key that left the file reads as empty
    Wh_FreeStringSetting(gone);

    // Stop: the mod is uninitialised and the hook comes off again.
    CHECK(SetNamed(L"Stop"));
    CHECK(WaitFor([] { return g_uninits.load() == 1; }));
    CHECK(WaitFor([] { return Dummy(3) == 19; }, 5000));
    CHECK(g_changes == 1 && g_inits == 1);

    // Styling on again in the same process: the DLL stays loaded after a stop
    // (MAP 93), so the mod's init runs again over the hooks MinHook kept.
    CHECK(StylerStart((LPVOID)dir.c_str()) == 1);
    CHECK(g_inits == 2 && g_afterInits == 2);
    CHECK(Dummy(3) == plain + 1000);
    CHECK(StylerStart((LPVOID)dir.c_str()) == 1);   // running: no second init
    CHECK(g_inits == 2);
    CHECK(SetNamed(L"Stop"));
    CHECK(WaitFor([] { return g_uninits.load() == 2; }));
    CHECK(WaitFor([] { return Dummy(3) == 19; }, 5000));
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: styler_test <repo root>\n");
        return 2;
    }
    const std::wstring root = argv[1];
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"pw_styler_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    awa::SetConfigDirOverride(dir);

    Choices();
    ThemeTable(root);
    IniFile(dir);
    Lifecycle(dir);

    wprintf(L"\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
