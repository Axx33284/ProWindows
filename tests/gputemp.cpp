// ProWindows - what a GPU temperature costs in memory, by API.
//
// Loads NVML, or NvAPI, or WMI, reads one temperature, and prints the
// process's private commit before and after. Written to decide which source
// thermal.cpp should prefer; the answer on the development machine is in
// thermal.cpp beside the sensor order.
//
//   gputemp.exe nvml | nvapi | wmi
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")

static double PrivateMB() {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
    return pmc.PrivateUsage / 1048576.0;
}

static int Nvml() {
    HMODULE dll = LoadLibraryW(L"nvml.dll");
    if (!dll) { wprintf(L"no nvml.dll\n"); return 1; }
    wprintf(L"  loaded nvml.dll:       %.1f MB\n", PrivateMB());
    typedef int (*Init)();
    typedef int (*Count)(unsigned*);
    typedef int (*Handle)(unsigned, void**);
    typedef int (*Temp)(void*, int, unsigned*);
    typedef int (*Shutdown)();
    auto init  = (Init)GetProcAddress(dll, "nvmlInit_v2");
    auto count = (Count)GetProcAddress(dll, "nvmlDeviceGetCount_v2");
    auto handle = (Handle)GetProcAddress(dll, "nvmlDeviceGetHandleByIndex_v2");
    auto temp  = (Temp)GetProcAddress(dll, "nvmlDeviceGetTemperature");
    auto shutdown = (Shutdown)GetProcAddress(dll, "nvmlShutdown");
    if (!init || !count || !handle || !temp) return 1;
    if (init() != 0) { wprintf(L"nvmlInit failed\n"); return 1; }
    wprintf(L"  nvmlInit:              %.1f MB\n", PrivateMB());
    unsigned n = 0; count(&n);
    void* dev = nullptr; unsigned t = 0;
    if (n && handle(0, &dev) == 0 && temp(dev, 0, &t) == 0)
        wprintf(L"  read %u C:             %.1f MB\n", t, PrivateMB());
    if (shutdown) shutdown();
    FreeLibrary(dll);
    wprintf(L"  shutdown + unload:     %.1f MB\n", PrivateMB());
    return 0;
}

// NvAPI is reached through one exported function that hands back the rest by
// id. The ids are stable and published by every open hardware monitor.
static int Nvapi() {
    HMODULE dll = LoadLibraryW(L"nvapi64.dll");
    if (!dll) { wprintf(L"no nvapi64.dll\n"); return 1; }
    wprintf(L"  loaded nvapi64.dll:    %.1f MB\n", PrivateMB());
    typedef void* (*Query)(unsigned);
    auto query = (Query)GetProcAddress(dll, "nvapi_QueryInterface");
    if (!query) return 1;
    typedef int (*Initialize)();
    typedef int (*EnumGpus)(void** handles, int* count);
    struct ThermalSettings {
        unsigned version;
        unsigned count;
        struct { int controller; int defaultMin; int defaultMax; int currentTemp; int target; } sensor[3];
    };
    typedef int (*GetThermal)(void* gpu, int sensorIndex, ThermalSettings* out);
    auto initialize = (Initialize)query(0x0150E828);
    auto enumGpus   = (EnumGpus)query(0xE5AC921F);
    auto getThermal = (GetThermal)query(0xE3640A56);
    if (!initialize || !enumGpus || !getThermal) { wprintf(L"nvapi entry points missing\n"); return 1; }
    if (initialize() != 0) { wprintf(L"NvAPI_Initialize failed\n"); return 1; }
    wprintf(L"  NvAPI_Initialize:      %.1f MB\n", PrivateMB());
    void* gpus[64] = {};
    int count = 0;
    if (enumGpus(gpus, &count) != 0 || count == 0) { wprintf(L"no gpus\n"); return 1; }
    ThermalSettings ts = {};
    ts.version = (unsigned)(sizeof(ThermalSettings) | (1u << 16));
    if (getThermal(gpus[0], 15 /* all */, &ts) == 0 && ts.count > 0)
        wprintf(L"  read %d C:             %.1f MB\n", ts.sensor[0].currentTemp, PrivateMB());
    else
        wprintf(L"  thermal read failed:   %.1f MB\n", PrivateMB());
    typedef int (*Unload)();
    if (auto unload = (Unload)query(0xD22BDD7E)) unload();
    FreeLibrary(dll);
    wprintf(L"  unload:                %.1f MB\n", PrivateMB());
    return 0;
}

#include <wbemidl.h>
static int Wmi() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWbemLocator* locator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator, (void**)&locator)))
        return 1;
    wprintf(L"  locator:               %.1f MB\n", PrivateMB());
    IWbemServices* svc = nullptr;
    BSTR ns = SysAllocString(L"root\\WMI");
    const HRESULT hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
    SysFreeString(ns);
    wprintf(L"  connected (%08x):    %.1f MB\n", (unsigned)hr, PrivateMB());
    if (svc) svc->Release();
    locator->Release();
    CoUninitialize();
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* which = argc > 1 ? argv[1] : L"nvml";
    wprintf(L"%s\n  start:                 %.1f MB\n", which, PrivateMB());
    if (wcscmp(which, L"nvml") == 0)  return Nvml();
    if (wcscmp(which, L"nvapi") == 0) return Nvapi();
    if (wcscmp(which, L"wmi") == 0)   return Wmi();
    return 1;
}
