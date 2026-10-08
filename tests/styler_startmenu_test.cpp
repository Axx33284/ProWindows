// ProWindows - tests for the Start menu styler: the shim built the way the
// Start menu DLL gets it (PW_STYLER=L"startmenu", PW_STYLER_PACKAGED: stored
// values, signals as files) and the ProWindows side in src\startmenustyler.cpp.
// All in this process: nothing is injected anywhere and the real Start menu is
// never touched. The "mod" is a few stubs that look at what the shim hands them
// and hook a dummy function with MinHook.
//
// Usage: styler_startmenu_test.exe <repo root>
#include "../src/explorer/windhawk_shim.h"
#include "../src/startmenustyler.h"
#include "../src/common.h"

#include <aclapi.h>
#include <sddl.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

using namespace awa;

// ---------------------------------------------------------------- what startmenustyler.cpp calls out to
namespace awa {
void AppTrayBalloon(const wchar_t*, const wchar_t*) {}
void AppSaveConfig() {}
void AppRefreshSettings() {}
}

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; \
    wprintf(L"  FAIL line %d: %S\n", __LINE__, #cond); } } while (0)

__declspec(noinline) static int Dummy(int x) {
    volatile int acc = 0;
    for (int i = 0; i < x; ++i) acc = acc + i * 3 + 1;
    return acc + 7;
}
using DummyFn = int (*)(int);
static DummyFn g_dummyOriginal;
static int DummyHook(int x) { return g_dummyOriginal(x) + 1000; }

static std::atomic<int> g_inits{0}, g_afterInits{0}, g_uninits{0}, g_changes{0};
static std::wstring g_themeAtInit, g_themeAtChange, g_layoutAtInit;
static bool g_hooked = false;
static int g_intAtInit = -1, g_missingInt = -1;
static size_t g_binSizeAtInit = 0;

BOOL Wh_ModInit() {
    ++g_inits;
    PCWSTR t = Wh_GetStringSetting(L"theme");
    g_themeAtInit = t;
    Wh_FreeStringSetting(t);
    PCWSTR l = Wh_GetStringSetting(L"disableNewStartMenuLayout");
    g_layoutAtInit = l;
    Wh_FreeStringSetting(l);

    // The throttle the mod keeps between runs (lastExitTickCount_<target>).
    g_missingInt = Wh_GetIntValue(L"lastExitTickCount_0", 77);
    Wh_SetIntValue(L"lastExitTickCount_0", 123456);
    g_intAtInit = Wh_GetIntValue(L"lastExitTickCount_0", 77);
    const unsigned char blob[5] = {1, 2, 3, 4, 5};
    Wh_SetBinaryValue(L"statsTimerLastTime", blob, sizeof(blob));
    unsigned char two[2] = {};
    g_binSizeAtInit = Wh_GetBinaryValue(L"statsTimerLastTime", two, sizeof(two));

    g_hooked = Wh_SetFunctionHook((void*)Dummy, (void*)DummyHook, (void**)&g_dummyOriginal);
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

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

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

static std::wstring Signal(const wchar_t* what, const std::wstring& dir) {
    return dir + L"\\" + what + L"." + std::to_wstring(GetCurrentProcessId());
}

static void MakeSignal(const wchar_t* what, const std::wstring& dir) {
    HANDLE f = CreateFileW(Signal(what, dir).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
}

// Does `path` have an allow entry for the SID with at least these rights?
static bool HasAce(const std::wstring& path, PCWSTR sidText, DWORD rights) {
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(sidText, &sid)) return false;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    bool found = false;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                              nullptr, &dacl, nullptr, &sd) == ERROR_SUCCESS && dacl) {
        for (DWORD i = 0; i < dacl->AceCount; ++i) {
            void* ace = nullptr;
            if (!GetAce(dacl, i, &ace)) continue;
            auto* a = (ACCESS_ALLOWED_ACE*)ace;
            if (a->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
            if (EqualSid((PSID)&a->SidStart, sid) && (a->Mask & rights) == rights) found = true;
        }
    }
    if (sd) LocalFree(sd);
    LocalFree(sid);
    return found;
}

// ---------------------------------------------------------------- tests
static void Choices() {
    wprintf(L"the choices the settings page offers\n");
    CHECK(StartThemeCount() == 34);
    CHECK(std::wstring(StartThemeId(0)) == L"ProWindows Glass");   // the default
    CHECK(std::wstring(StartThemeId(1)) == L"ProWindows");
    CHECK(StartThemeIndexOf(L"ProWindows Glass") == 0);
    CHECK(StartThemeIndexOf(L"") == 2);
    CHECK(StartThemeIndexOf(L"TintedGlass") > 2);
    CHECK(StartThemeIndexOf(L"no such theme") == -1);
    CHECK(StartThemeIsProWindows(L"ProWindows") && StartThemeIsProWindows(L"ProWindows Glass"));
    CHECK(!StartThemeIsProWindows(L"TintedGlass") && !StartThemeIsProWindows(L""));
    for (int i = 0; i < StartThemeCount(); ++i)
        for (int j = i + 1; j < StartThemeCount(); ++j)
            CHECK(std::wstring(StartThemeId(i)) != StartThemeId(j) &&
                  std::wstring(StartThemeName(i)) != StartThemeName(j));

    CHECK(StartLayoutCount() == 6);
    CHECK(std::wstring(StartLayoutId(0)).empty());
    CHECK(StartLayoutIndexOf(L"forceNewLayout") == 4);
    CHECK(StartLayoutIndexOf(L"bogus") == -1);

    CHECK(StartTintCount() == 4);
    CHECK(StartTintColor(0) == RGB(0, 0, 0));
    CHECK(StartTintColor(1) == RGB(0x1C, 0x1C, 0x1E));
    CHECK(StartTintColor(3) != CLR_INVALID);
}

// Every theme the page offers must be one the mod's GetSelectedTheme knows,
// the ProWindows themes must be what the plan says, and no telemetry survives.
static void ThemeTable(const std::wstring& root) {
    wprintf(L"the mod's theme table\n");
    const std::wstring src = ReadAll(root + L"\\src\\startmenu\\styler.cpp");
    CHECK(!src.empty());
    for (int i = 0; i < StartThemeCount(); ++i) {
        const std::wstring id = StartThemeId(i);
        if (id.empty()) continue;
        const bool known = src.find(L"wcscmp(themeName, L\"" + id + L"\") == 0") != std::wstring::npos;
        if (!known) wprintf(L"  theme not selectable by the mod: %s\n", id.c_str());
        CHECK(known);
    }

    // The targets both ProWindows themes share, and each theme's constants.
    const size_t t0 = src.find(L"const std::vector<ThemeTargetStyles> g_pwStartTargets = {");
    const size_t t1 = src.find(L"const Theme g_themeProWindows = {g_pwStartTargets", t0);
    const size_t t2 = src.find(L"const Theme g_themeProWindowsGlass = {g_pwStartTargets", t1);
    const size_t t3 = src.find(L"#undef PW_METAL", t2);
    CHECK(t0 != std::wstring::npos && t1 != std::wstring::npos && t2 != std::wstring::npos &&
          t3 != std::wstring::npos);
    if (t3 != std::wstring::npos) {
        const std::wstring targets = src.substr(t0, t1 - t0);
        const std::wstring solid = src.substr(t1, t2 - t1);
        const std::wstring glass = src.substr(t2, t3 - t2);
        CHECK(targets.find(L"Border#AcrylicBorder") != std::wstring::npos);
        CHECK(targets.find(L"StartDocked.AppListViewItem") != std::wstring::npos);
        CHECK(targets.find(L"$pwHover") != std::wstring::npos);
        CHECK(targets.find(L"SystemAccentColor") == std::wstring::npos);   // no accent colour
        CHECK(src.find(L"#1A1A1A") != std::wstring::npos && src.find(L"#3A3A3A") != std::wstring::npos);
        CHECK(solid.find(L"WindhawkBlur") == std::wstring::npos);         // solid black
        CHECK(solid.find(L"#FF000000") != std::wstring::npos);
        CHECK(glass.find(L"WindhawkBlur") != std::wstring::npos);          // translucent
        for (const wchar_t* c : { L"pwBg=", L"pwBgSoft=", L"pwHover=", L"pwEdge=", L"pwRadius=", L"pwText=" }) {
            CHECK(solid.find(c) != std::wstring::npos || solid.find(L"PW_METAL") != std::wstring::npos);
            CHECK(glass.find(c) != std::wstring::npos || glass.find(L"PW_METAL") != std::wstring::npos);
        }
    }

    // No telemetry survives the port.
    CHECK(src.find(L"StatsTimer") == std::wstring::npos);
    CHECK(src.find(L"statsTimerLastTime") == std::wstring::npos);
    CHECK(src.find(L"Wh_GetUrlContent") == std::wstring::npos);
    CHECK(src.find(L"releases/download/stats") == std::wstring::npos);
}

static void IniFile(const std::wstring& dir) {
    wprintf(L"the ini ProWindows writes\n");
    const std::wstring path = StartMenuStylerIniPath();
    CHECK(StartMenuStylerDir() == dir + L"\\startmenu-styler");
    CHECK(path == dir + L"\\startmenu-styler\\startmenu-styler.ini");

    Config c;   // the defaults: ProWindows Glass, black, 55 %, metal, 12 px, white
    CHECK(StartMenuStylerWriteIni(c));                           // creates folder and file
    CHECK(Exists(path));
    std::wstring text = ReadAll(path);
    CHECK(text.find(L"theme=ProWindows Glass\r\n") != std::wstring::npos);
    CHECK(text.find(L"disableNewStartMenuLayout=\r\n") != std::wstring::npos);
    CHECK(text.find(L"debug=0") != std::wstring::npos);
    CHECK(text.find(L"styleConstants[0]=pwBg=<WindhawkBlur BlurAmount=\"18\" TintColor=\"#8C000000\"/>") != std::wstring::npos);
    CHECK(text.find(L"styleConstants[1]=pwBgSoft=<WindhawkBlur BlurAmount=\"18\" TintColor=\"#5B000000\"/>") != std::wstring::npos);
    CHECK(text.find(L"styleConstants[2]=pwHover=<LinearGradientBrush") != std::wstring::npos);
    CHECK(text.find(L"#1A1A1A") != std::wstring::npos && text.find(L"#3A3A3A") != std::wstring::npos);
    CHECK(text.find(L"styleConstants[3]=pwEdge=#59E0E0E0") != std::wstring::npos);
    CHECK(text.find(L"styleConstants[4]=pwRadius=12") != std::wstring::npos);
    CHECK(text.find(L"styleConstants[5]=pwText=#FFFFFF") != std::wstring::npos);
    CHECK(text.find(L"controlStyles[0].target") != std::wstring::npos);    // the commented template
    CHECK(!StartMenuStylerWriteIni(c));                          // nothing to change

    // Solid theme, steel tint, accent highlight, round 4, grey text, layout.
    c.startTheme = L"ProWindows";
    c.startTint = 2; c.startHighlight = 1; c.startRadius = 4; c.startText = 1;
    c.startLayout = L"forceNewLayout"; c.debug = true;
    WriteAll(path, text + L"controlStyles[0].target=Border#Mine\r\ncontrolStyles[0].styles[0]=Background=#112233\r\n");
    CHECK(StartMenuStylerWriteIni(c));
    text = ReadAll(path);
    CHECK(Count(text, L"theme=") == 1);
    CHECK(text.find(L"theme=ProWindows\r\n") != std::wstring::npos);
    CHECK(text.find(L"disableNewStartMenuLayout=forceNewLayout") != std::wstring::npos);
    CHECK(text.find(L"debug=1") != std::wstring::npos);
    CHECK(text.find(L"pwBg=<SolidColorBrush Color=\"#FF26303A\"/>") != std::wstring::npos);
    CHECK(text.find(L"WindhawkBlur") == std::wstring::npos);
    CHECK(text.find(L"pwHover=<SolidColorBrush Color=\"#FF") != std::wstring::npos);
    CHECK(text.find(L"pwRadius=4") != std::wstring::npos);
    CHECK(text.find(L"pwText=#D0D0D0") != std::wstring::npos);
    CHECK(Count(text, L"controlStyles[0].target=Border#Mine") == 1);      // the user's lines survive
    CHECK(Count(text, L"Background=#112233") == 1);
    CHECK(Count(text, L"managed by ProWindows") == 1);

    // Glass opacity and the inner tint's share of it.
    c.startTheme = L"ProWindows Glass"; c.startTint = 0; c.startTintOpacity = 100; c.startHighlight = 0;
    StartMenuStylerWriteIni(c);
    text = ReadAll(path);
    CHECK(text.find(L"TintColor=\"#FF000000\"") != std::wstring::npos);
    CHECK(text.find(L"TintColor=\"#A5000000\"") != std::wstring::npos);   // 255 * 65 / 100
    c.startTintOpacity = 0;
    StartMenuStylerWriteIni(c);
    CHECK(ReadAll(path).find(L"TintColor=\"#00000000\"") != std::wstring::npos);

    // A theme of the mod's own: no constants of ours, the user's lines still there.
    c.startTheme = L"TintedGlass"; c.startLayout.clear(); c.debug = false;
    StartMenuStylerWriteIni(c);
    text = ReadAll(path);
    CHECK(text.find(L"theme=TintedGlass") != std::wstring::npos);
    CHECK(text.find(L"styleConstants[0]=") == std::wstring::npos);
    CHECK(Count(text, L"Background=#112233") == 1);
}

static void Access(const std::wstring& dir) {
    wprintf(L"the app packages' access\n");
    const std::wstring folder = StartMenuStylerDir();
    // The folder was made with the ACL when the ini was first written.
    const DWORD dirRights = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
    CHECK(HasAce(folder, L"S-1-15-2-1", dirRights));
    CHECK(HasAce(folder, L"S-1-15-2-2", dirRights));
    CHECK(HasAce(StartMenuStylerIniPath(), L"S-1-15-2-1", FILE_GENERIC_READ));    // inherited
    CHECK(HasAce(StartMenuStylerIniPath(), L"S-1-15-2-2", FILE_GENERIC_READ));

    // A file made in it afterwards carries the entries too (the log, signals, values).
    const std::wstring later = folder + L"\\later.bin";
    WriteAll(later, L"x");
    CHECK(HasAce(later, L"S-1-15-2-1", FILE_GENERIC_READ | FILE_GENERIC_WRITE));
    CHECK(HasAce(later, L"S-1-15-2-2", FILE_GENERIC_READ | FILE_GENERIC_WRITE));

    // The DLL: read and execute only, on the file itself, and asking twice is fine.
    const std::wstring dll = dir + L"\\fake.dll";
    WriteAll(dll, L"MZ");
    CHECK(!HasAce(dll, L"S-1-15-2-1", FILE_GENERIC_READ | FILE_GENERIC_EXECUTE));
    CHECK(StartMenuStylerGrantPackages(dll, false));
    CHECK(StartMenuStylerGrantPackages(dll, false));
    CHECK(HasAce(dll, L"S-1-15-2-1", FILE_GENERIC_READ | FILE_GENERIC_EXECUTE));
    CHECK(HasAce(dll, L"S-1-15-2-2", FILE_GENERIC_READ | FILE_GENERIC_EXECUTE));
    CHECK(!HasAce(dll, L"S-1-15-2-1", FILE_WRITE_DATA));
    CHECK(!StartMenuStylerGrantPackages(dir + L"\\no such file", false));
}

static void Lifecycle() {
    wprintf(L"the shim as the Start menu DLL gets it: values, file signals, hooks\n");
    const std::wstring dir = StartMenuStylerDir();
    const std::wstring path = StartMenuStylerIniPath();

    WriteAll(path, L"theme=ProWindows Glass\r\ndisableNewStartMenuLayout=default\r\ndebug=1\r\n");
    const int plain = Dummy(3);
    CHECK(plain == 19);
    CHECK(!Exists(Signal(L"alive", dir)));

    CHECK(StylerStart((LPVOID)dir.c_str()) == 1);
    CHECK(g_inits == 1 && g_afterInits == 1);
    CHECK(g_hooked);
    CHECK(g_themeAtInit == L"ProWindows Glass");
    CHECK(g_layoutAtInit == L"default");
    CHECK(Dummy(3) == plain + 1000);

    // Alive: the file ProWindows looks for, held open and deleted when it goes.
    CHECK(Exists(Signal(L"alive", dir)));
    CHECK(!Exists(Signal(L"stop", dir)) && !Exists(Signal(L"reload", dir)));

    // The names: the ini and the log are the Start menu's own, in the one folder.
    CHECK(Exists(dir + L"\\startmenu-styler.log"));
    CHECK(ReadAll(dir + L"\\startmenu-styler.log").find(L"StylerStart") != std::wstring::npos);
    wchar_t store[MAX_PATH];
    CHECK(Wh_GetModStoragePath(store, ARRAYSIZE(store)));
    CHECK(std::wstring(store) == dir);                 // no subfolder: this one is the package's
    CHECK(!Exists(dir + L"\\explorer-styler.ini"));

    // Stored values: Wh_Get/SetIntValue, Wh_Get/SetBinaryValue.
    CHECK(g_missingInt == 77);                         // the default for a key never set
    CHECK(g_intAtInit == 123456);
    CHECK(g_binSizeAtInit == 5);                       // the stored size, 2 bytes copied
    CHECK(Exists(dir + L"\\value_lastExitTickCount_0.bin"));
    CHECK(Wh_GetIntValue(L"lastExitTickCount_0", 0) == 123456);
    unsigned char got[8] = {};
    CHECK(Wh_GetBinaryValue(L"statsTimerLastTime", got, sizeof(got)) == 5);
    CHECK(got[0] == 1 && got[4] == 5);
    CHECK(Wh_SetIntValue(L"a b/c", 5));                // a key with odd characters
    CHECK(Wh_GetIntValue(L"a b/c", 0) == 5);
    CHECK(Wh_GetBinaryValue(L"never set", got, sizeof(got)) == 0);
    CHECK(Wh_GetIntValue(L"never set", -9) == -9);

    // Reload: the file is taken, the ini read again, the mod told.
    WriteAll(path, L"theme=Float\r\ndebug=0\r\n");
    MakeSignal(L"reload", dir);
    CHECK(WaitFor([] { return g_changes.load() == 1; }));
    CHECK(g_themeAtChange == L"Float");
    CHECK(WaitFor([&] { return !Exists(Signal(L"reload", dir)); }));
    CHECK(Exists(Signal(L"alive", dir)));

    // Stop: the mod is uninitialised, the hook comes off, the file is taken and
    // alive is gone - that is how ProWindows sees the process is not styled.
    MakeSignal(L"stop", dir);
    CHECK(WaitFor([] { return g_uninits.load() == 1; }));
    CHECK(WaitFor([&] { return !Exists(Signal(L"alive", dir)); }));
    CHECK(WaitFor([&] { return !Exists(Signal(L"stop", dir)); }));
    CHECK(WaitFor([] { return Dummy(3) == 19; }));
    CHECK(g_changes == 1 && g_inits == 1);

    // A stale stop (made for a run that already ended) is not for the next run.
    MakeSignal(L"stop", dir);
    CHECK(StylerStart((LPVOID)dir.c_str()) == 1);
    CHECK(g_inits == 2 && g_afterInits == 2);
    CHECK(Exists(Signal(L"alive", dir)) && !Exists(Signal(L"stop", dir)));
    CHECK(Dummy(3) == plain + 1000);
    CHECK(StylerStart((LPVOID)dir.c_str()) == 1);      // running: a reload, no second init
    CHECK(g_inits == 2);
    CHECK(WaitFor([] { return g_changes.load() == 2; }));
    MakeSignal(L"stop", dir);
    CHECK(WaitFor([] { return g_uninits.load() == 2; }));
    CHECK(WaitFor([&] { return !Exists(Signal(L"alive", dir)); }));
    CHECK(WaitFor([] { return Dummy(3) == 19; }));
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: styler_startmenu_test <repo root>\n");
        return 2;
    }
    const std::wstring root = argv[1];
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"pw_startmenu_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    awa::SetConfigDirOverride(dir);

    Choices();
    ThemeTable(root);
    IniFile(dir);
    Access(dir);
    Lifecycle();

    wprintf(L"\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
