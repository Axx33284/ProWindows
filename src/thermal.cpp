#include "thermal.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <wbemidl.h>
#include <oleauto.h>
#include <cstdlib>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace awa {

namespace {

// Silicon that reads below freezing or above 150 C is a unit mistake, not a
// temperature. Every source is filtered through this before it is believed.
bool Plausible(double celsius) { return celsius > -20.0 && celsius < 150.0; }

constexpr HRESULT kWbemAccessDenied = (HRESULT)0x80041003L;

// ======================================================== ACPI thermal zone
// The zone read through PDH rather than WMI. Same sensor, but the performance
// counter is readable by any user, where MSAcpi_ThermalZoneTemperature needs
// administrator - which is the whole reason temperature used to come up blank.
class ZoneCounter {
public:
    // False once means false for the life of the process: a machine either has
    // the counter set or it does not.
    bool Open() {
        if (tried_) return query_ != nullptr;
        tried_ = true;

        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
            query_ = nullptr;
            return false;
        }
        // Tenths of a Kelvin where it exists, whole Kelvin where it does not.
        PdhAddEnglishCounterW(query_,
            L"\\Thermal Zone Information(*)\\High Precision Temperature", 0, &fine_);
        PdhAddEnglishCounterW(query_,
            L"\\Thermal Zone Information(*)\\Temperature", 0, &coarse_);

        if (!fine_ && !coarse_) { Close(); return false; }
        PdhCollectQueryData(query_);
        return true;
    }

    void Close() {
        if (query_) PdhCloseQuery(query_);
        query_ = nullptr;
        fine_ = coarse_ = nullptr;
    }

    // The hottest zone. A machine can have several - CPU, mainboard, skin -
    // and the hottest is the one worth putting on screen.
    bool Read(double* celsius) {
        if (!query_) return false;
        if (PdhCollectQueryData(query_) != ERROR_SUCCESS) return false;

        double best = 0.0;
        if (fine_   && Hottest(fine_,   0.1, &best)) { *celsius = best; return true; }
        if (coarse_ && Hottest(coarse_, 1.0, &best)) { *celsius = best; return true; }
        return false;
    }

    ~ZoneCounter() { Close(); }

private:
    // `scale` turns one counter unit into Kelvin.
    static bool Hottest(PDH_HCOUNTER counter, double scale, double* best) {
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count,
                                         nullptr) != PDH_MORE_DATA || size == 0)
            return false;

        std::vector<BYTE> buffer(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count,
                                         items) != ERROR_SUCCESS)
            return false;

        double top = 0.0;
        bool any = false;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
                continue;
            const double c = items[i].FmtValue.doubleValue * scale - 273.15;
            if (!Plausible(c)) continue;
            if (!any || c > top) { top = c; any = true; }
        }
        if (!any) return false;
        *best = top;
        return true;
    }

    PDH_HQUERY   query_  = nullptr;
    PDH_HCOUNTER fine_   = nullptr;
    PDH_HCOUNTER coarse_ = nullptr;
    bool tried_ = false;
};

// ============================================================== NVIDIA / NvAPI
// The same die sensor NVML reports, through the driver's other library.
// Tried first, and for one reason: memory. On the development machine
// nvmlInit_v2 commits 19.3 MB of private memory in the calling process - the
// driver's user-mode state - and nvmlShutdown plus FreeLibrary give none of it
// back, so switching the temperature off later did not help either. NvAPI
// reads the same 47 degrees for 2.8 MB, and unloads (tests\gputemp.bat
// measures all three sources). NVML stays as the fallback for a driver old
// enough not to answer here.
//
// NvAPI has one export; everything else is fetched by id. The ids are the
// ones every open-source hardware monitor uses and have not changed in a
// decade.
class NvApiSensor {
public:
    bool Open() {
        if (tried_) return ready_;
        tried_ = true;
        dll_ = LoadLibraryW(L"nvapi64.dll");
        if (!dll_) return false;
        auto query = (QueryInterface)GetProcAddress(dll_, "nvapi_QueryInterface");
        if (!query) { Close(); return false; }
        initialize_ = (Initialize)query(0x0150E828);
        unload_     = (Unload)query(0xD22BDD7E);
        enumGpus_   = (EnumGpus)query(0xE5AC921F);
        thermal_    = (GetThermal)query(0xE3640A56);
        if (!initialize_ || !enumGpus_ || !thermal_) { Close(); return false; }
        if (initialize_() != 0) { Close(); return false; }
        started_ = true;
        count_ = 0;
        if (enumGpus_(gpus_, &count_) != 0 || count_ <= 0) { Close(); return false; }
        ready_ = true;
        return true;
    }

    bool Available() const { return ready_; }

    void Close() {
        if (started_ && unload_) unload_();
        started_ = false;
        ready_   = false;
        count_   = 0;
        if (dll_) FreeLibrary(dll_);
        dll_ = nullptr;
        initialize_ = nullptr; unload_ = nullptr; enumGpus_ = nullptr; thermal_ = nullptr;
    }

    // The hottest board, as the others do; within a board the hottest
    // sensor it reports, which is the GPU itself on every card seen.
    bool Read(double* celsius) {
        if (!ready_) return false;
        double top = 0.0;
        bool any = false;
        for (int i = 0; i < count_ && i < kMaxGpus; ++i) {
            ThermalSettings ts = {};
            ts.version = (unsigned)(sizeof(ThermalSettings) | (1u << 16));
            if (thermal_(gpus_[i], 15 /* every sensor */, &ts) != 0) continue;
            for (unsigned k = 0; k < ts.count && k < 3; ++k) {
                const double c = (double)ts.sensor[k].currentTemp;
                if (!Plausible(c)) continue;
                if (!any || c > top) { top = c; any = true; }
            }
        }
        if (!any) return false;
        *celsius = top;
        return true;
    }

    ~NvApiSensor() { Close(); }

private:
    static constexpr int kMaxGpus = 64;
    struct ThermalSettings {
        unsigned version;
        unsigned count;
        struct { int controller; int defaultMin; int defaultMax; int currentTemp; int target; } sensor[3];
    };
    typedef void* (*QueryInterface)(unsigned id);
    typedef int (*Initialize)();
    typedef int (*Unload)();
    typedef int (*EnumGpus)(void** handles, int* count);
    typedef int (*GetThermal)(void* gpu, int sensorIndex, ThermalSettings* out);

    HMODULE    dll_        = nullptr;
    Initialize initialize_ = nullptr;
    Unload     unload_     = nullptr;
    EnumGpus   enumGpus_   = nullptr;
    GetThermal thermal_    = nullptr;
    void*      gpus_[kMaxGpus] = {};
    int        count_      = 0;
    bool       tried_ = false, started_ = false, ready_ = false;
};

// ============================================================== NVIDIA / NVML
// nvml.dll ships with the driver, is installed into System32 by every recent
// one, and reports the GPU's own die sensor without any privilege at all.
// The fallback behind NvApiSensor, for the memory reason given there.
class NvidiaSensor {
public:
    bool Open() {
        if (tried_) return ready_;
        tried_ = true;

        dll_ = LoadLibraryW(L"nvml.dll");
        if (!dll_) {
            // Older driver packages only dropped it beside nvidia-smi.
            wchar_t programFiles[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH)) {
                const std::wstring path = std::wstring(programFiles) +
                    L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
                dll_ = LoadLibraryW(path.c_str());
            }
        }
        if (!dll_) return false;

        // The _v2 entry points are the current ones; the unsuffixed names are
        // kept by NVML for compatibility and are all an ancient driver has.
        init_     = (Init)    Pick("nvmlInit_v2", "nvmlInit");
        shutdown_ = (Shutdown)Pick("nvmlShutdown", nullptr);
        count_    = (Count)   Pick("nvmlDeviceGetCount_v2", "nvmlDeviceGetCount");
        handle_   = (Handle)  Pick("nvmlDeviceGetHandleByIndex_v2",
                                   "nvmlDeviceGetHandleByIndex");
        temp_     = (Temp)    Pick("nvmlDeviceGetTemperature", nullptr);

        if (!init_ || !count_ || !handle_ || !temp_) { Close(); return false; }
        if (init_() != 0) { Close(); return false; }

        started_ = true;
        unsigned int devices = 0;
        if (count_(&devices) != 0 || devices == 0) { Close(); return false; }
        devices_ = devices;
        ready_   = true;
        return true;
    }

    void Close() {
        if (started_ && shutdown_) shutdown_();
        started_ = false;
        ready_   = false;
        if (dll_) FreeLibrary(dll_);
        dll_ = nullptr;
        init_ = nullptr; shutdown_ = nullptr;
        count_ = nullptr; handle_ = nullptr; temp_ = nullptr;
    }

    // The hottest board in the machine, for the same reason as the zones.
    bool Read(double* celsius) {
        if (!ready_) return false;
        double top = 0.0;
        bool any = false;
        for (unsigned int i = 0; i < devices_; ++i) {
            void* device = nullptr;
            if (handle_(i, &device) != 0 || !device) continue;
            unsigned int value = 0;
            if (temp_(device, 0 /* NVML_TEMPERATURE_GPU */, &value) != 0) continue;
            const double c = (double)value;
            if (!Plausible(c)) continue;
            if (!any || c > top) { top = c; any = true; }
        }
        if (!any) return false;
        *celsius = top;
        return true;
    }

    ~NvidiaSensor() { Close(); }

private:
    using Init     = int (*)(void);
    using Shutdown = int (*)(void);
    using Count    = int (*)(unsigned int*);
    using Handle   = int (*)(unsigned int, void**);
    using Temp     = int (*)(void*, int, unsigned int*);

    FARPROC Pick(const char* name, const char* older) {
        FARPROC fn = GetProcAddress(dll_, name);
        if (!fn && older) fn = GetProcAddress(dll_, older);
        return fn;
    }

    HMODULE dll_ = nullptr;
    Init init_ = nullptr; Shutdown shutdown_ = nullptr;
    Count count_ = nullptr; Handle handle_ = nullptr; Temp temp_ = nullptr;
    unsigned int devices_ = 0;
    bool tried_ = false, started_ = false, ready_ = false;
};

// ================================================================= AMD / ADL
// atiadlxx.dll is AMD's equivalent, and equally unprivileged. Which of the
// three Overdrive generations answers depends on the card, so all three are
// tried and whichever gives a sane number wins.
void* __stdcall AdlAlloc(int bytes) {
    return bytes > 0 ? malloc((size_t)bytes) : nullptr;
}

class AmdSensor {
public:
    bool Open() {
        if (tried_) return context_ != nullptr;
        tried_ = true;

        dll_ = LoadLibraryW(L"atiadlxx.dll");
        if (!dll_) return false;

        create_  = (Create) GetProcAddress(dll_, "ADL2_Main_Control_Create");
        destroy_ = (Destroy)GetProcAddress(dll_, "ADL2_Main_Control_Destroy");
        counted_ = (Counted)GetProcAddress(dll_, "ADL2_Adapter_NumberOfAdapters_Get");
        active_  = (Active) GetProcAddress(dll_, "ADL2_Adapter_Active_Get");
        odn_     = (OdN)    GetProcAddress(dll_, "ADL2_OverdriveN_Temperature_Get");
        od6_     = (Od6)    GetProcAddress(dll_, "ADL2_Overdrive6_Temperature_Get");
        od5_     = (Od5)    GetProcAddress(dll_, "ADL2_Overdrive5_Temperature_Get");

        if (!create_ || !counted_ || (!odn_ && !od6_ && !od5_)) { Close(); return false; }
        // 1 = only enumerate adapters that are actually connected.
        if (create_(AdlAlloc, 1, &context_) != 0 || !context_) { Close(); return false; }
        if (counted_(context_, &adapters_) != 0 || adapters_ <= 0) { Close(); return false; }
        return true;
    }

    void Close() {
        if (context_ && destroy_) destroy_(context_);
        context_ = nullptr;
        if (dll_) FreeLibrary(dll_);
        dll_ = nullptr;
        create_ = nullptr; destroy_ = nullptr; counted_ = nullptr; active_ = nullptr;
        odn_ = nullptr; od6_ = nullptr; od5_ = nullptr;
        adapters_ = 0;
    }

    bool Read(double* celsius) {
        if (!context_) return false;
        double top = 0.0;
        bool any = false;

        for (int i = 0; i < adapters_; ++i) {
            if (active_) {
                int on = 0;
                if (active_(context_, i, &on) != 0 || !on) continue;
            }
            double c = 0.0;
            if (!AdapterTemperature(i, &c)) continue;
            if (!any || c > top) { top = c; any = true; }
        }
        if (!any) return false;
        *celsius = top;
        return true;
    }

    ~AmdSensor() { Close(); }

private:
    // Both Overdrive N and 6 answer in thousandths of a degree; 5 fills in a
    // struct with the same unit.
    struct AdlTemperature { int size; int milliCelsius; };

    using Context = void*;
    using Malloc  = void* (__stdcall*)(int);
    using Create  = int (__stdcall*)(Malloc, int, Context*);
    using Destroy = int (__stdcall*)(Context);
    using Counted = int (__stdcall*)(Context, int*);
    using Active  = int (__stdcall*)(Context, int, int*);
    using OdN     = int (__stdcall*)(Context, int, int, int*);
    using Od6     = int (__stdcall*)(Context, int, int*);
    using Od5     = int (__stdcall*)(Context, int, int, AdlTemperature*);

    bool AdapterTemperature(int adapter, double* celsius) {
        int milli = 0;
        if (odn_ && odn_(context_, adapter, 1 /* core */, &milli) == 0) {
            const double c = milli / 1000.0;
            if (Plausible(c)) { *celsius = c; return true; }
        }
        if (od6_ && od6_(context_, adapter, &milli) == 0) {
            const double c = milli / 1000.0;
            if (Plausible(c)) { *celsius = c; return true; }
        }
        if (od5_) {
            AdlTemperature reading = { (int)sizeof(AdlTemperature), 0 };
            if (od5_(context_, adapter, 0, &reading) == 0) {
                const double c = reading.milliCelsius / 1000.0;
                if (Plausible(c)) { *celsius = c; return true; }
            }
        }
        return false;
    }

    HMODULE dll_ = nullptr;
    Context context_ = nullptr;
    Create create_ = nullptr; Destroy destroy_ = nullptr;
    Counted counted_ = nullptr; Active active_ = nullptr;
    OdN odn_ = nullptr; Od6 od6_ = nullptr; Od5 od5_ = nullptr;
    int  adapters_ = 0;
    bool tried_ = false;
};

// ------------------------------------------------------------- WMI helpers
std::wstring PropString(IWbemClassObject* row, const wchar_t* name) {
    std::wstring out;
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(row->Get(name, 0, &v, nullptr, nullptr)) &&
        v.vt == VT_BSTR && v.bstrVal)
        out.assign(v.bstrVal, SysStringLen(v.bstrVal));
    VariantClear(&v);
    return out;
}

// Sensor values come back as VT_R4, thermal zones as VT_I4. Let the variant
// machinery deal with the difference rather than testing for each type.
bool PropNumber(IWbemClassObject* row, const wchar_t* name, double* out) {
    VARIANT v;
    VariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(row->Get(name, 0, &v, nullptr, nullptr))) {
        VARIANT number;
        VariantInit(&number);
        if (SUCCEEDED(VariantChangeType(&number, &v, 0, VT_R8))) {
            *out = number.dblVal;
            ok = true;
        }
        VariantClear(&number);
    }
    VariantClear(&v);
    return ok;
}

IWbemServices* ConnectNamespace(IWbemLocator* locator, const wchar_t* space) {
    if (!locator) return nullptr;
    IWbemServices* services = nullptr;
    BSTR path = SysAllocString(space);
    const HRESULT hr = locator->ConnectServer(path, nullptr, nullptr, nullptr, 0,
                                              nullptr, nullptr, &services);
    SysFreeString(path);
    if (FAILED(hr) || !services) return nullptr;

    // Deliberately no CoInitializeSecurity: it is process-wide and can only be
    // called once, so a library has no business calling it. Setting the blanket
    // on this proxy achieves the same thing locally.
    // Best effort: if the blanket cannot be set the calls below will simply be
    // refused, which is a case this class already reports as "denied".
    (void)CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                            nullptr, EOAC_NONE);
    return services;
}

// ================================================ LibreHardwareMonitor / OHM
// Only worth having because it is the one source that can see the CPU package
// sensor: reading that needs a kernel driver, and theirs is signed and already
// installed if the user runs either tool. Absent, this simply never connects.
class HardwareMonitorSensors {
public:
    void Attach(IWbemLocator* locator) {
        if (services_) return;
        // Cheap to retry - an absent namespace fails immediately - so a tool
        // started after the overlay still gets picked up.
        if (locator) {
            services_ = ConnectNamespace(locator, L"ROOT\\LibreHardwareMonitor");
            if (!services_)
                services_ = ConnectNamespace(locator, L"ROOT\\OpenHardwareMonitor");
        }
    }

    void Close() {
        if (services_) services_->Release();
        services_ = nullptr;
    }

    bool Connected() const { return services_ != nullptr; }

    // Reads both readings in one query, since one round trip returns every
    // sensor in the machine anyway.
    void Read(bool* haveCpu, double* cpu, bool* haveGpu, double* gpu) {
        *haveCpu = *haveGpu = false;
        if (!services_) return;

        BSTR language = SysAllocString(L"WQL");
        BSTR query    = SysAllocString(
            L"SELECT Identifier, Name, Value FROM Sensor "
            L"WHERE SensorType = 'Temperature'");
        IEnumWbemClassObject* results = nullptr;
        const HRESULT hr = services_->ExecQuery(
            language, query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &results);
        SysFreeString(language);
        SysFreeString(query);
        if (FAILED(hr) || !results) return;

        // A package or core reading is what people mean by "CPU temperature";
        // anything else on the die is a fallback, so preferred readings win
        // regardless of which is hotter.
        double bestCpu = 0.0, bestGpu = 0.0;
        bool namedCpu = false, namedGpu = false;

        for (;;) {
            IWbemClassObject* row = nullptr;
            ULONG got = 0;
            if (results->Next(2000, 1, &row, &got) != S_OK || got == 0) break;

            const std::wstring id   = ToLower(PropString(row, L"Identifier"));
            const std::wstring name = ToLower(PropString(row, L"Name"));
            double value = 0.0;
            const bool number = PropNumber(row, L"Value", &value);
            row->Release();
            if (!number || !Plausible(value)) continue;

            // "/intelcpu/0/temperature/0", "/gpu-nvidia/0/temperature/0".
            if (id.find(L"gpu") != std::wstring::npos) {
                const bool preferred = name.find(L"core") != std::wstring::npos;
                Consider(preferred, value, &namedGpu, &bestGpu, haveGpu);
            } else if (id.find(L"cpu") != std::wstring::npos) {
                const bool preferred = name.find(L"package") != std::wstring::npos;
                Consider(preferred, value, &namedCpu, &bestCpu, haveCpu);
            }
        }
        results->Release();

        if (*haveCpu) *cpu = bestCpu;
        if (*haveGpu) *gpu = bestGpu;
    }

    ~HardwareMonitorSensors() { Close(); }

private:
    static void Consider(bool preferred, double value, bool* havePreferred,
                         double* best, bool* have) {
        if (*havePreferred && !preferred) return;
        if (preferred && !*havePreferred) { *best = value; *havePreferred = true; }
        else if (!*have || value > *best)  { *best = value; }
        *have = true;
    }

    IWbemServices* services_ = nullptr;
};

// ============================================== MSAcpi_ThermalZoneTemperature
// The original source, kept as a last resort: it reads the same zone the
// performance counter does, but needs administrator, so it only ever answers
// on the machines where the counter set is missing and ProWindows is elevated.
class AcpiZoneWmi {
public:
    void Attach(IWbemLocator* locator) {
        if (services_ || tried_) return;
        tried_ = true;
        services_ = ConnectNamespace(locator, L"ROOT\\WMI");
    }

    void Close() {
        if (services_) services_->Release();
        services_ = nullptr;
    }

    // `denied` distinguishes "this machine has no sensor" from "elevate and it
    // would work", which are very different things to tell the user.
    bool Read(double* celsius, bool* denied) {
        *denied = false;
        if (!services_) return false;

        BSTR language = SysAllocString(L"WQL");
        BSTR query    = SysAllocString(
            L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
        IEnumWbemClassObject* results = nullptr;
        const HRESULT hr = services_->ExecQuery(
            language, query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &results);
        SysFreeString(language);
        SysFreeString(query);
        if (FAILED(hr) || !results) {
            if (hr == kWbemAccessDenied || hr == E_ACCESSDENIED) *denied = true;
            return false;
        }

        double top = 0.0;
        bool any = false;
        for (;;) {
            IWbemClassObject* row = nullptr;
            ULONG got = 0;
            // Unelevated it is the enumeration, not ExecQuery, that refuses.
            const HRESULT next = results->Next(2000, 1, &row, &got);
            if (next == kWbemAccessDenied || next == E_ACCESSDENIED) *denied = true;
            if (next != S_OK || got == 0) break;

            double tenthsKelvin = 0.0;
            const bool number = PropNumber(row, L"CurrentTemperature", &tenthsKelvin);
            row->Release();
            if (!number) continue;

            const double c = tenthsKelvin / 10.0 - 273.15;
            if (!Plausible(c)) continue;
            if (!any || c > top) { top = c; any = true; }
        }
        results->Release();

        if (!any) return false;
        *celsius = top;
        return true;
    }

    ~AcpiZoneWmi() { Close(); }

private:
    IWbemServices* services_ = nullptr;
    bool tried_ = false;
};


// ================================================ shared-memory monitors
// Two of the tools people already run publish everything they read into a
// shared memory block that any process can open. No driver, no elevation, no
// WMI round trip - a memcpy out of a mapped view, which is cheap enough to do
// on every pass rather than once a minute like the WMI sources.
//
// This is the honest way to get a real CPU *package* temperature. Reading the
// sensor itself means reading an MSR, which means a kernel driver; the only
// way to have one without shipping one is to use the driver somebody else has
// already installed and signed. LibreHardwareMonitor over WMI was the first
// such source here; these two are the same idea with less overhead and a far
// wider install base.

// ---------------------------------------------------------------- Core Temp
// CoreTemp publishes this structure under a well-known name. The layout is
// fixed and documented by its author; it has not changed in many years, and
// the size check below is what catches it if it ever does.
#pragma pack(push, 4)
struct CoreTempSharedData {
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

class CoreTempSensor {
public:
    // Opening a mapping that is not there fails immediately, so this is
    // retried on every pass and picks CoreTemp up if it is started later.
    bool Read(double* celsius) {
        if (!map_) {
            static const wchar_t* kNames[] = {
                L"CoreTempMappingObjectEx", L"Local\\CoreTempMappingObjectEx",
                L"CoreTempMappingObject",   L"Local\\CoreTempMappingObject",
            };
            for (const wchar_t* name : kNames) {
                map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
                if (map_) break;
            }
            if (!map_) return false;
        }

        const auto* data = static_cast<const CoreTempSharedData*>(
            MapViewOfFile(map_, FILE_MAP_READ, 0, 0, sizeof(CoreTempSharedData)));
        if (!data) { Close(); return false; }

        const unsigned cores = data->uiCoreCnt;
        const unsigned cpus  = (std::max)(1u, data->uiCPUCnt);
        bool   any  = false;
        double best = 0.0;

        // Sane bounds first: a stale or half-written view is the one thing a
        // shared memory block will hand you without any error at all.
        if (cores > 0 && cores <= 128 && cores * cpus <= 256) {
            for (unsigned i = 0; i < cores * cpus; ++i) {
                double c = (double)data->fTemp[i];
                if (data->ucFahrenheit) c = (c - 32.0) * 5.0 / 9.0;
                // The option that reports how far *below* the throttle point
                // each core is, rather than its temperature.
                if (data->ucDeltaToTjMax) {
                    const unsigned tj = data->uiTjMax[(i / (std::max)(1u, cores)) % 128];
                    if (tj == 0 || tj > 200) continue;
                    c = (double)tj - c;
                }
                if (!Plausible(c)) continue;
                if (!any || c > best) { best = c; any = true; }
            }
        }
        UnmapViewOfFile(data);

        if (!any) return false;
        *celsius = best;      // the hottest core is the one worth showing
        return true;
    }

    void Close() {
        if (map_) { CloseHandle(map_); map_ = nullptr; }
    }
    ~CoreTempSensor() { Close(); }

private:
    HANDLE map_ = nullptr;
};

// ---------------------------------------------------------------- HWiNFO
// HWiNFO publishes every sensor it reads, with names, into one block. It has
// to be switched on - Settings, "Shared Memory Support" - which is why a
// failure here is not worth reporting: most people simply do not run it.
namespace hwinfo {

constexpr DWORD kSignature = 0x53695748;    // "HWiS"

#pragma pack(push, 1)
struct SharedMem {
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

// Only the head of the reading element is needed; the rest is min/max/average.
struct Reading {
    DWORD  type;          // 1 = temperature
    DWORD  sensorIndex;
    DWORD  readingId;
    char   labelOrig[128];
    char   labelUser[128];
    char   unit[16];
    double value;
};
#pragma pack(pop)

constexpr DWORD kTypeTemperature = 1;

// Case-insensitive substring, over the fixed ASCII buffers HWiNFO uses.
bool LabelHas(const char* label, const char* needle) {
    if (!label || !needle || !*needle) return false;
    for (size_t i = 0; label[i]; ++i) {
        size_t j = 0;
        while (needle[j] && label[i + j] &&
               tolower((unsigned char)label[i + j]) == tolower((unsigned char)needle[j]))
            ++j;
        if (!needle[j]) return true;
        if (!label[i + j]) break;
    }
    return false;
}

} // namespace hwinfo

class HwInfoSensor {
public:
    // Both readings come out of one pass over the block, because walking it is
    // the cost and there is no reason to do it twice.
    bool Read(bool* haveCpu, double* cpu, bool* haveGpu, double* gpu) {
        *haveCpu = *haveGpu = false;

        if (!map_) {
            static const wchar_t* kNames[] = {
                L"Global\\HWiNFO_SENS_SM2", L"Local\\HWiNFO_SENS_SM2",
            };
            for (const wchar_t* name : kNames) {
                map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
                if (map_) break;
            }
            if (!map_) return false;
        }

        const auto* head = static_cast<const hwinfo::SharedMem*>(
            MapViewOfFile(map_, FILE_MAP_READ, 0, 0, sizeof(hwinfo::SharedMem)));
        if (!head) { Close(); return false; }

        const DWORD signature = head->signature;
        const DWORD offset    = head->readingSectionOffset;
        const DWORD stride    = head->readingElementSize;
        const DWORD count     = head->readingElementCount;
        UnmapViewOfFile(head);

        // Anything outside these is a block from a version that does not match
        // the layout above, and reading it would be reading somebody else's
        // memory as if it were a struct.
        if (signature != hwinfo::kSignature) { Close(); return false; }
        if (stride < sizeof(hwinfo::Reading) || stride > 4096) return false;
        if (count == 0 || count > 8192) return false;

        const size_t bytes = (size_t)offset + (size_t)stride * count;
        const auto* base = static_cast<const BYTE*>(
            MapViewOfFile(map_, FILE_MAP_READ, 0, 0, bytes));
        if (!base) return false;

        double bestCpu = 0.0, bestGpu = 0.0;
        for (DWORD i = 0; i < count; ++i) {
            const auto* r = reinterpret_cast<const hwinfo::Reading*>(
                base + offset + (size_t)stride * i);
            if (r->type != hwinfo::kTypeTemperature) continue;
            if (!Plausible(r->value)) continue;

            // The labels HWiNFO uses for the sensor that actually matters, on
            // Intel and on AMD respectively. "Core Max" is the fallback for a
            // board that does not publish a package reading.
            const char* label = r->labelUser[0] ? r->labelUser : r->labelOrig;
            if (hwinfo::LabelHas(label, "CPU Package") ||
                hwinfo::LabelHas(label, "CPU (Tctl") ||
                hwinfo::LabelHas(label, "Core Max")) {
                if (!*haveCpu || r->value > bestCpu) { bestCpu = r->value; *haveCpu = true; }
            } else if (hwinfo::LabelHas(label, "GPU Temperature") ||
                       hwinfo::LabelHas(label, "GPU Hot Spot")) {
                if (!*haveGpu || r->value > bestGpu) { bestGpu = r->value; *haveGpu = true; }
            }
        }
        UnmapViewOfFile(base);

        if (*haveCpu) *cpu = bestCpu;
        if (*haveGpu) *gpu = bestGpu;
        return *haveCpu || *haveGpu;
    }

    void Close() {
        if (map_) { CloseHandle(map_); map_ = nullptr; }
    }
    ~HwInfoSensor() { Close(); }

private:
    HANDLE map_ = nullptr;
};

// ==================================================================== probe
class Probe {
public:
    void Want(bool cpu, bool gpu) {
        // Nothing wanted costs nothing: not a thread, not even the critical
        // section, for the many people who never switch a temperature on.
        if (!cpu && !gpu) { Stop(); return; }

        EnsureLock();
        EnterCriticalSection(&lock_);
        wantCpu_ = cpu;
        wantGpu_ = gpu;
        LeaveCriticalSection(&lock_);
        Start();
    }

    void Stop() {
        // Everything the outgoing probe still publishes is stale from here on.
        EnsureLock();
        EnterCriticalSection(&lock_);
        ++gen_;
        LeaveCriticalSection(&lock_);

        if (stop_) SetEvent(stop_);
        if (thread_) {
            // The probe can be parked inside a WMI call - Next() alone is given
            // two seconds a row - so this wait genuinely can expire.
            //
            // It is deliberately short. Stop() is not only reached at exit: it
            // runs whenever the overlay is taken down, which is every time a
            // fullscreen application starts or stops and every time the screen
            // blanks. Waiting seconds for it froze the whole application - tray,
            // shortcuts, tiling and all - for as long as WMI took to answer,
            // which reads as the app having hung. The abandon path below is
            // already correct and costs nothing, so take it sooner.
            if (WaitForSingleObject(thread_, 250) == WAIT_OBJECT_0) {
                Reap();
            } else {
                // Disowned rather than waited on. The thread owns its own stop
                // event and closes it when it finally lets go, and it clears
                // running_ on the way out, so forgetting it here is safe.
                //
                // Forgetting it is also the point: leaving thread_ set made
                // the next Start() decide a probe was already running and
                // return without starting one, which is how switching a
                // temperature off and on again could leave the readings dead
                // for the rest of the session.
                AWA_LOG(L"thermal probe did not stop in time; abandoning it");
                CloseHandle(thread_);
                thread_ = nullptr;
                stop_   = nullptr;
                // Taken back here rather than left to the abandoned thread,
                // which is the whole point of abandoning it. That thread's own
                // release is a compare-exchange against its id, so it will find
                // the slot reassigned and leave it alone.
                InterlockedExchange(&running_, 0);
            }
        }
        // lock_ is never deleted. It costs one critical section for the life of
        // the process and removes any chance of an abandoned probe entering a
        // section that has already been freed.
        if (haveLock_) {
            EnterCriticalSection(&lock_);
            cpu_ = ThermalReading();
            gpu_ = ThermalReading();
            cpuStatus_ = gpuStatus_ = ThermalStatus::Unknown;
            LeaveCriticalSection(&lock_);
        }
    }

    bool Read(bool gpu, ThermalReading* out) {
        if (!haveLock_) return false;
        EnterCriticalSection(&lock_);
        const ThermalReading& reading = gpu ? gpu_ : cpu_;
        const bool ok = reading.have;
        if (ok) *out = reading;
        LeaveCriticalSection(&lock_);
        return ok;
    }

    ThermalStatus Status(bool gpu) {
        if (!haveLock_) return ThermalStatus::Unknown;
        EnterCriticalSection(&lock_);
        const ThermalStatus s = gpu ? gpuStatus_ : cpuStatus_;
        LeaveCriticalSection(&lock_);
        return s;
    }

private:
    void EnsureLock() {
        // Initialised once and never deleted: see Stop().
        if (!haveLock_) {
            InitializeCriticalSection(&lock_);
            haveLock_ = true;
        }
    }

    // Each run owns its own stop event and closes it on the way out, so a probe
    // that had to be abandoned can be forgotten immediately instead of pinning
    // the handles - and the next Start() is then free to run a fresh one.
    struct RunCtx {
        Probe* owner = nullptr;
        HANDLE stop  = nullptr;
        LONG   gen   = 0;
        LONG   id    = 0;    // which run owns running_; see Start and Trampoline
    };

    void Start() {
        // Exactly one live probe at a time. running_ holds the id of the run
        // that owns the slot rather than a plain flag, because an abandoned
        // probe finishes at a time of its own choosing and must not release a
        // slot a newer probe has since taken. With a plain flag it did exactly
        // that, and a third Start() then ran a second probe alongside the
        // second - two threads and two WMI connections for one temperature.
        const LONG id = InterlockedIncrement(&nextRunId_);
        if (InterlockedCompareExchange(&running_, id, 0) != 0) return;

        EnsureLock();
        HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop) { InterlockedCompareExchange(&running_, 0, id); return; }

        // Assigned rather than brace-initialised so there is exactly one place
        // the null check has to cover, and the analyser can see it.
        RunCtx* ctx = new (std::nothrow) RunCtx();
        if (!ctx) {
            CloseHandle(stop);
            InterlockedCompareExchange(&running_, 0, id);
            return;
        }
        ctx->owner = this;
        ctx->stop  = stop;
        ctx->id    = id;
        // Start and Stop are both UI thread only, so this read needs no lock;
        // the probe thread only ever reads gen_ under lock_, in Publish.
        ctx->gen   = gen_;

        HANDLE thread = CreateThread(nullptr, 0, Trampoline, ctx, 0, nullptr);
        if (!thread) {
            CloseHandle(stop);
            delete ctx;
            InterlockedCompareExchange(&running_, 0, id);
            return;
        }
        thread_ = thread;
        stop_   = stop;
    }

    // Closes the handles of a thread that has definitely finished.
    void Reap() {
        if (thread_) { CloseHandle(thread_); thread_ = nullptr; }
        if (stop_)   { CloseHandle(stop_);   stop_   = nullptr; }
    }

    static DWORD WINAPI Trampoline(LPVOID p) {
        RunCtx* ctx = static_cast<RunCtx*>(p);
        Probe* self = ctx->owner;
        self->Run(ctx->stop, ctx->gen);
        // Released before the handle is closed, so a Start() that sees the slot
        // free cannot begin before this one has finished with it - and released
        // only if this run still owns it, so an abandoned probe finishing much
        // later does not hand away the slot its replacement is using.
        InterlockedCompareExchange(&self->running_, 0, ctx->id);
        CloseHandle(ctx->stop);
        delete ctx;
        return 0;
    }

    // Readings from a probe that Stop() has already disowned are thrown away
    // rather than overwriting the ones a newer probe is publishing.
    void Publish(LONG gen, bool gpu, const ThermalReading& reading, ThermalStatus status) {
        EnterCriticalSection(&lock_);
        if (gen == gen_) {
            (gpu ? gpu_ : cpu_) = reading;
            (gpu ? gpuStatus_ : cpuStatus_) = status;
        }
        LeaveCriticalSection(&lock_);
    }

    void Run(HANDLE stop, LONG gen) {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;

        IWbemLocator* locator = nullptr;
        // A machine without WMI simply has no locator; every user of it below
        // already copes with that, so the failure needs no separate handling -
        // only a guarantee that the pointer is null when it happens.
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWbemLocator, (void**)&locator)))
            locator = nullptr;

        ZoneCounter            zones;
        NvApiSensor            nvapi;
        NvidiaSensor           nvidia;
        AmdSensor              amd;
        HardwareMonitorSensors hwmon;
        AcpiZoneWmi            acpi;
        CoreTempSensor         coretemp;
        HwInfoSensor           hwinfoSensor;

        DWORD wait = 0;
        int   misses = 0;
        int   cycle  = 0;

        while (WaitForSingleObject(stop, wait) == WAIT_TIMEOUT) {
            bool wantCpu, wantGpu;
            EnterCriticalSection(&lock_);
            wantCpu = wantCpu_;
            wantGpu = wantGpu_;
            LeaveCriticalSection(&lock_);

            // Retried about once a minute so launching LibreHardwareMonitor
            // while the overlay is up upgrades the reading without a restart.
            if ((wantCpu || wantGpu) && (cycle % 30) == 0) hwmon.Attach(locator);
            ++cycle;

            bool hwCpuOk = false, hwGpuOk = false;
            double hwCpu = 0.0, hwGpu = 0.0;
            if (hwmon.Connected() && (wantCpu || wantGpu))
                hwmon.Read(&hwCpuOk, &hwCpu, &hwGpuOk, &hwGpu);

            // A shared memory read is a memcpy, not a WMI round trip, so both
            // of these are tried on every pass - which is also what makes
            // starting one of the tools mid-session just start working.
            bool hiCpuOk = false, hiGpuOk = false;
            double hiCpu = 0.0, hiGpu = 0.0;
            if (wantCpu || wantGpu)
                hwinfoSensor.Read(&hiCpuOk, &hiCpu, &hiGpuOk, &hiGpu);

            bool ctOk = false;
            double ctCpu = 0.0;
            if (wantCpu) ctOk = coretemp.Read(&ctCpu);

            bool answered = false;

            if (wantCpu) {
                ThermalReading reading;
                ThermalStatus  status = ThermalStatus::Missing;
                double celsius = 0.0;

                // Ordered by how directly the number comes from the die. The
                // first three are all the real package sensor, read through
                // whichever tool the user happens to have; the thermal zone is
                // a mainboard sensor and says so, because presenting the two
                // as the same claim is how "my CPU idles at 40" turns into an
                // argument.
                if (hwCpuOk) {
                    reading = { true, hwCpu, L"package" };
                } else if (hiCpuOk) {
                    reading = { true, hiCpu, L"hwinfo" };
                } else if (ctOk) {
                    reading = { true, ctCpu, L"core temp" };
                } else if (zones.Open() && zones.Read(&celsius)) {
                    reading = { true, celsius, L"thermal zone" };
                } else {
                    // Only worth a WMI connection once the counter has failed.
                    acpi.Attach(locator);
                    bool denied = false;
                    if (acpi.Read(&celsius, &denied))
                        reading = { true, celsius, L"thermal zone" };
                    else if (denied)
                        status = ThermalStatus::Denied;
                }
                if (reading.have) { status = ThermalStatus::Ok; answered = true; }
                Publish(gen, false, reading, status);
            } else {
                Publish(gen, false, ThermalReading(), ThermalStatus::Unknown);
            }

            if (wantGpu) {
                ThermalReading reading;
                double celsius = 0.0;

                // The vendor libraries first: both read the die sensor
                // directly, need no privilege and no third-party tool, and are
                // present on any machine with the driver installed.
                // NvAPI before NVML, and NVML only if NvAPI could not be
                // opened at all: a single failed read must not load 19 MB.
                if (nvapi.Open() && nvapi.Read(&celsius))
                    reading = { true, celsius, L"nvidia" };
                else if (!nvapi.Available() && nvidia.Open() && nvidia.Read(&celsius))
                    reading = { true, celsius, L"nvidia" };
                else if (amd.Open() && amd.Read(&celsius))
                    reading = { true, celsius, L"amd" };
                else if (hiGpuOk)
                    reading = { true, hiGpu, L"hwinfo" };
                else if (hwGpuOk)
                    reading = { true, hwGpu, L"gpu core" };

                if (reading.have) answered = true;
                Publish(gen, true, reading, reading.have ? ThermalStatus::Ok
                                                         : ThermalStatus::Missing);
            } else {
                Publish(gen, true, ThermalReading(), ThermalStatus::Unknown);
            }

            // A machine that cannot answer should not be asked every two
            // seconds for the rest of the session.
            if (answered) { misses = 0; wait = 2000; }
            else {
                if (misses < 10) ++misses;
                wait = (misses >= 3) ? 60000 : 5000;
            }
        }

        if (locator) locator->Release();
        CoUninitialize();
    }

    HANDLE thread_ = nullptr;
    HANDLE stop_   = nullptr;
    // Set while a probe thread is alive, so two Want() calls in a row cannot
    // start two of them - and, equally important, so a probe that had to be
    // abandoned does not permanently look like one that is still working.
    LONG   running_ = 0;
    // Ever-increasing, so no two runs share an id and a stale release cannot be
    // mistaken for the live one's.
    LONG   nextRunId_ = 0;
    // Bumped by every Stop(). A probe only publishes under the generation it
    // was started with, so an abandoned one cannot overwrite live readings.
    LONG   gen_    = 0;
    CRITICAL_SECTION lock_ = {};
    bool haveLock_ = false;
    bool wantCpu_  = false;
    bool wantGpu_  = false;
    ThermalReading cpu_, gpu_;
    ThermalStatus  cpuStatus_ = ThermalStatus::Unknown;
    ThermalStatus  gpuStatus_ = ThermalStatus::Unknown;
};

Probe g_probe;

} // namespace

void ThermalWant(bool cpu, bool gpu) { g_probe.Want(cpu, gpu); }

bool ThermalReadCpu(ThermalReading* out) { return g_probe.Read(false, out); }
bool ThermalReadGpu(ThermalReading* out) { return g_probe.Read(true,  out); }

ThermalStatus ThermalCpuStatus() { return g_probe.Status(false); }
ThermalStatus ThermalGpuStatus() { return g_probe.Status(true);  }

void ThermalStop() { g_probe.Stop(); }

} // namespace awa
