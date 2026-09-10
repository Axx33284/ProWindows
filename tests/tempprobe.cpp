// ProWindows - what this machine can actually tell us about temperature.
//
// Runs the real probe and prints what came back and which source answered.
// Read-only: it opens sensors and mapped views, and touches nothing else. The
// fastest way to answer "why is the CPU temperature blank on my machine", and
// the only way to check a new source without running the whole application.
//
//   tempprobe.exe            one pass
//   tempprobe.exe 20         keep printing for twenty seconds
//   tempprobe.exe --selftest publish synthetic Core Temp and HWiNFO blocks and
//                            check the probe reads them back correctly
//
// Build it with tests\tempprobe.bat.
#include "../src/thermal.h"
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

using namespace awa;

namespace {

const wchar_t* StatusName(ThermalStatus s) {
    switch (s) {
        case ThermalStatus::Ok:      return L"ok";
        case ThermalStatus::Denied:  return L"denied (needs administrator)";
        case ThermalStatus::Missing: return L"no sensor this machine can read";
        default:                     return L"not answered yet";
    }
}

void Line(const wchar_t* what, bool have, const ThermalReading& r, ThermalStatus s) {
    if (have)
        wprintf(L"  %-4s %6.1f C   source: %-12s\n", what, r.celsius, r.source);
    else
        wprintf(L"  %-4s      -     %s\n", what, StatusName(s));
}


// ---------------------------------------------------------------- self-test
// Neither Core Temp nor HWiNFO is installed on every machine, and a reader for
// a struct nobody has published is a reader nobody has ever run. These publish
// blocks of exactly the documented shape, with values chosen so a mistake in
// the layout cannot produce the right answer by accident, and then ask the real
// probe what it sees.
//
// The names are the "Local\" variants, which need no privilege - the probe
// tries Global first and falls through to these.
namespace selftest {

#pragma pack(push, 4)
struct CoreTempBlock {
    unsigned int  uiLoad[256];
    unsigned int  uiTjMax[128];
    unsigned int  uiCoreCnt;
    unsigned int  uiCPUCnt;
    float         fTemp[256];
    float         fVID;
    float         fCPUSpeed;
    float         fFSBSpeed;
    float         fMultiplier;
    char          sCPUName[100];
    unsigned char ucFahrenheit;
    unsigned char ucDeltaToTjMax;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct HwInfoHead {
    DWORD     signature;
    DWORD     version;
    DWORD     revision;
    long long pollTime;
    DWORD     sensorSectionOffset;
    DWORD     sensorElementSize;
    DWORD     sensorElementCount;
    DWORD     readingSectionOffset;
    DWORD     readingElementSize;
    DWORD     readingElementCount;
};
struct HwInfoReading {
    DWORD  type;
    DWORD  sensorIndex;
    DWORD  readingId;
    char   labelOrig[128];
    char   labelUser[128];
    char   unit[16];
    double value;
    double valueMin;
    double valueMax;
    double valueAvg;
};
#pragma pack(pop)

// Holds a mapping open for as long as it is alive.
struct Block {
    HANDLE map  = nullptr;
    void*  view = nullptr;

    void* Create(const wchar_t* name, size_t bytes) {
        map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                 0, (DWORD)bytes, name);
        if (!map) return nullptr;
        view = MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, bytes);
        if (!view) { CloseHandle(map); map = nullptr; return nullptr; }
        memset(view, 0, bytes);
        return view;
    }
    ~Block() {
        if (view) UnmapViewOfFile(view);
        if (map) CloseHandle(map);
    }
};

// Asks the probe for a fresh reading and returns what it said.
bool Ask(bool gpu, ThermalReading* out) {
    ThermalStop();               // so the next run re-opens every source
    ThermalWant(true, true);
    for (int i = 0; i < 60; ++i) {
        Sleep(100);
        const bool have = gpu ? ThermalReadGpu(out) : ThermalReadCpu(out);
        if (have) return true;
    }
    return false;
}

bool Near(double a, double b) { return a > b - 0.6 && a < b + 0.6; }

int Run() {
    int failed = 0;

    // ---- Core Temp: four cores, plain Celsius. The hottest is the answer.
    {
        Block block;
        auto* data = static_cast<CoreTempBlock*>(
            block.Create(L"Local\\CoreTempMappingObjectEx", sizeof(CoreTempBlock)));
        if (!data) {
            wprintf(L"  core temp : could not publish a test block\n");
            ++failed;
        } else {
            data->uiCoreCnt = 4;
            data->uiCPUCnt  = 1;
            data->fTemp[0] = 45.0f; data->fTemp[1] = 51.5f;
            data->fTemp[2] = 44.0f; data->fTemp[3] = 46.0f;
            for (int i = 0; i < 4; ++i) data->uiTjMax[i] = 100;

            ThermalReading r;
            const bool have = Ask(false, &r);
            const bool ok = have && wcscmp(r.source, L"core temp") == 0 &&
                            Near(r.celsius, 51.5);
            wprintf(L"  core temp : %s  (read %.1f from %s, wanted 51.5)\n",
                    ok ? L"ok  " : L"FAIL", have ? r.celsius : 0.0,
                    have ? r.source : L"nothing");
            if (!ok) ++failed;
        }
    }

    // ---- Core Temp again, this time reporting how far below TjMax each core
    // ---- is, which is an option in its own settings and reads as a
    // ---- temperature of 41 degrees if it is not handled.
    {
        Block block;
        auto* data = static_cast<CoreTempBlock*>(
            block.Create(L"Local\\CoreTempMappingObjectEx", sizeof(CoreTempBlock)));
        if (!data) { ++failed; }
        else {
            data->uiCoreCnt = 2;
            data->uiCPUCnt  = 1;
            data->ucDeltaToTjMax = 1;
            data->uiTjMax[0] = 100;
            data->fTemp[0] = 41.0f;   // 100 - 41 = 59
            data->fTemp[1] = 45.0f;   // 100 - 45 = 55
            ThermalReading r;
            const bool have = Ask(false, &r);
            const bool ok = have && wcscmp(r.source, L"core temp") == 0 &&
                            Near(r.celsius, 59.0);
            wprintf(L"  delta form: %s  (read %.1f, wanted 59.0)\n",
                    ok ? L"ok  " : L"FAIL", have ? r.celsius : 0.0);
            if (!ok) ++failed;
        }
    }

    // ---- HWiNFO: a block with three readings, only one of which is the one
    // ---- being looked for, so a reader that takes the first temperature it
    // ---- sees fails here.
    {
        const size_t readings = 3;
        const size_t bytes = sizeof(HwInfoHead) + sizeof(HwInfoReading) * readings;
        Block block;
        auto* base = static_cast<BYTE*>(
            block.Create(L"Local\\HWiNFO_SENS_SM2", bytes));
        if (!base) {
            wprintf(L"  hwinfo    : could not publish a test block\n");
            ++failed;
        } else {
            auto* head = reinterpret_cast<HwInfoHead*>(base);
            head->signature            = 0x53695748;      // "HWiS"
            head->readingSectionOffset = (DWORD)sizeof(HwInfoHead);
            head->readingElementSize   = (DWORD)sizeof(HwInfoReading);
            head->readingElementCount  = (DWORD)readings;

            auto* r = reinterpret_cast<HwInfoReading*>(base + sizeof(HwInfoHead));
            // A voltage that would be a nonsense temperature if the type field
            // were ignored.
            r[0].type = 2; strcpy_s(r[0].labelOrig, "CPU Core Voltage"); r[0].value = 1.2;
            r[1].type = 1; strcpy_s(r[1].labelOrig, "CPU Package");      r[1].value = 63.5;
            r[2].type = 1; strcpy_s(r[2].labelOrig, "Drive Temperature"); r[2].value = 38.0;

            ThermalReading cpu;
            const bool have = Ask(false, &cpu);
            const bool ok = have && wcscmp(cpu.source, L"hwinfo") == 0 &&
                            Near(cpu.celsius, 63.5);
            wprintf(L"  hwinfo    : %s  (read %.1f from %s, wanted 63.5)\n",
                    ok ? L"ok  " : L"FAIL", have ? cpu.celsius : 0.0,
                    have ? cpu.source : L"nothing");
            if (!ok) ++failed;
        }
    }

    // ---- And a block whose signature does not match must be ignored outright
    // ---- rather than read as if the layout were right.
    {
        Block block;
        auto* base = static_cast<BYTE*>(
            block.Create(L"Local\\HWiNFO_SENS_SM2", 4096));
        if (base) {
            auto* head = reinterpret_cast<HwInfoHead*>(base);
            head->signature            = 0xDEADBEEF;
            head->readingSectionOffset = (DWORD)sizeof(HwInfoHead);
            head->readingElementSize   = (DWORD)sizeof(HwInfoReading);
            head->readingElementCount  = 1;
            auto* r = reinterpret_cast<HwInfoReading*>(base + sizeof(HwInfoHead));
            r->type = 1; strcpy_s(r->labelOrig, "CPU Package"); r->value = 99.0;

            ThermalReading cpu;
            const bool have = Ask(false, &cpu);
            const bool ok = !have || wcscmp(cpu.source, L"hwinfo") != 0;
            wprintf(L"  bad magic : %s  (fell through to %s)\n",
                    ok ? L"ok  " : L"FAIL", have ? cpu.source : L"nothing");
            if (!ok) ++failed;
        }
    }

    ThermalStop();
    wprintf(L"\n%s\n", failed == 0 ? L"self-test: all passed"
                                   : L"self-test: FAILURES above");
    return failed;
}

} // namespace selftest

} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool selfTest = (argc > 1) && wcscmp(argv[1], L"--selftest") == 0;
    const int seconds = (argc > 1 && !selfTest) ? _wtoi(argv[1]) : 0;

    // The probe's own thread does its own CoInitialize; this is for the main
    // thread, which does nothing but wait and print.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (selfTest) {
        wprintf(L"publishing test sensor blocks and asking the probe:\n\n");
        const int failed = selftest::Run();
        CoUninitialize();
        return failed == 0 ? 0 : 1;
    }

    wprintf(L"asking for both temperatures...\n\n");
    ThermalWant(true, true);

    const ULONGLONG until = GetTickCount64() + (ULONGLONG)(seconds > 0 ? seconds : 6) * 1000;
    bool everCpu = false, everGpu = false;

    for (;;) {
        Sleep(1000);

        ThermalReading cpu, gpu;
        const bool haveCpu = ThermalReadCpu(&cpu);
        const bool haveGpu = ThermalReadGpu(&gpu);
        everCpu = everCpu || haveCpu;
        everGpu = everGpu || haveGpu;

        Line(L"cpu", haveCpu, cpu, ThermalCpuStatus());
        Line(L"gpu", haveGpu, gpu, ThermalGpuStatus());
        wprintf(L"\n");

        if (GetTickCount64() >= until) break;
        // One pass is enough once both have answered, unless a run length was
        // asked for - the point of a longer run is watching a number move.
        if (seconds <= 0 && everCpu && everGpu) break;
    }

    if (!everCpu)
        wprintf(L"No CPU temperature. The package sensor needs a kernel driver, so the\n"
                L"sources that read it are other people's: HWiNFO (with Shared Memory\n"
                L"Support switched on), Core Temp, or LibreHardwareMonitor. Failing all\n"
                L"three it falls back to the ACPI thermal zone, which is a mainboard\n"
                L"sensor and is not the same number.\n");
    if (!everGpu)
        wprintf(L"No GPU temperature. NVIDIA needs nvml.dll and AMD atiadlxx.dll, both of\n"
                L"which ship with the driver; anything else needs HWiNFO or\n"
                L"LibreHardwareMonitor running.\n");

    ThermalStop();
    CoUninitialize();
    return 0;
}
