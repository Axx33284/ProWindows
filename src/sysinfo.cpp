#include "sysinfo.h"
#include "thermal.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <winternl.h>
#include <dxgi.h>
#include <unordered_map>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")

namespace awa {

namespace {

// One PDH query shared by every wildcard counter we use.
PDH_HQUERY   g_query    = nullptr;
PDH_HCOUNTER g_gpu      = nullptr;
PDH_HCOUNTER g_vram     = nullptr;
PDH_HCOUNTER g_disk     = nullptr;
PDH_HCOUNTER g_netRecv  = nullptr;
PDH_HCOUNTER g_netSend  = nullptr;
bool g_primed = false;      // PDH rate counters need two samples before they read

unsigned long long FileTimeToU64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

std::wstring FormatBytes(double bytes) {
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    int unit = 0;
    while (bytes >= 1024.0 && unit < 4) { bytes /= 1024.0; ++unit; }

    wchar_t buf[64];
    if (bytes >= 100.0 || unit == 0) swprintf_s(buf, L"%.0f %s", bytes, units[unit]);
    else                             swprintf_s(buf, L"%.1f %s", bytes, units[unit]);
    return buf;
}

std::wstring FormatRate(double bytesPerSecond) {
    return FormatBytes(bytesPerSecond) + L"/s";
}

// Adds a wildcard counter, tolerating the ones this machine does not have.
bool AddCounter(const wchar_t* path, PDH_HCOUNTER* out) {
    return PdhAddEnglishCounterW(g_query, path, 0, out) == ERROR_SUCCESS;
}

// Pulls every instance of a wildcard counter. The instance names matter for
// the per-process counters, so they come back too.
bool ReadArray(PDH_HCOUNTER counter, std::vector<BYTE>* buffer, DWORD* count) {
    if (!counter) return false;

    DWORD size = 0;
    *count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                                                     &size, count, nullptr);
    if (status != PDH_MORE_DATA || size == 0) return false;

    buffer->resize(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer->data());
    return PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, count, items)
           == ERROR_SUCCESS;
}

bool ValidItem(const PDH_FMT_COUNTERVALUE_ITEM_W& item) {
    return item.FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
           item.FmtValue.CStatus == PDH_CSTATUS_NEW_DATA;
}

// Wildcard counters return many instances; for rates and utilisation the
// useful figure is their sum.
bool ReadSum(PDH_HCOUNTER counter, double* total) {
    std::vector<BYTE> buffer;
    DWORD count = 0;
    if (!ReadArray(counter, &buffer, &count)) return false;

    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    double sum = 0.0;
    for (DWORD i = 0; i < count; ++i)
        if (ValidItem(items[i])) sum += items[i].FmtValue.doubleValue;

    *total = sum;
    return true;
}

// For memory held per adapter, summing would count every GPU in the machine.
// The busiest single instance is the one worth showing.
bool ReadMax(PDH_HCOUNTER counter, double* best) {
    std::vector<BYTE> buffer;
    DWORD count = 0;
    if (!ReadArray(counter, &buffer, &count)) return false;

    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    double top = 0.0;
    bool any = false;
    for (DWORD i = 0; i < count; ++i) {
        if (!ValidItem(items[i])) continue;
        any = true;
        if (items[i].FmtValue.doubleValue > top) top = items[i].FmtValue.doubleValue;
    }
    if (!any) return false;
    *best = top;
    return true;
}

// ------------------------------------------------------------------ VRAM size
// How much dedicated video memory the machine has, straight from DXGI. Asked
// once: it cannot change while the app is running.
double DedicatedVideoMemory() {
    static double cached = -1.0;
    if (cached >= 0.0) return cached;
    cached = 0.0;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) ||
        !factory)
        return cached;

    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            // Skip the software renderer; take the biggest real adapter.
            const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            const double bytes = (double)desc.DedicatedVideoMemory;
            if (!software && bytes > cached) cached = bytes;
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    return cached;
}

// ------------------------------------------------------------- temperature
// The readings themselves come from `thermal.*`, on its own thread. All that
// is left here is turning one into something the overlay can paint - and,
// when there is none, saying which of the two problems it is: a machine with
// no sensor can never work, where a denied read is fixed by elevating.
void FillTemperature(bool cpu, Metric* out) {
    ThermalReading reading;
    if (cpu ? ThermalReadCpu(&reading) : ThermalReadGpu(&reading)) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.0f°C", reading.celsius);
        out->available = true;
        // 30 C idle to 100 C throttling is the range that means anything.
        out->percent = (std::max)(0.0, (std::min)(100.0,
                       (reading.celsius - 30.0) / 70.0 * 100.0));
        out->value   = buf;
        out->detail  = reading.source;
        return;
    }
    switch (cpu ? ThermalCpuStatus() : ThermalGpuStatus()) {
        case ThermalStatus::Denied:  out->detail = L"needs admin"; break;
        case ThermalStatus::Missing: out->detail = L"no sensor";   break;
        default:                     out->detail = L"reading...";  break;
    }
}

// ------------------------------------------------------- process snapshot
// The documented APIs give one process per call; this gives the lot in one,
// with the CPU times, working set and I/O totals already in the buffer. It is
// what Task Manager itself reads.
struct AwaProcessInfo {
    ULONG          NextEntryOffset;
    ULONG          NumberOfThreads;
    LARGE_INTEGER  WorkingSetPrivateSize;
    ULONG          HardFaultCount;
    ULONG          NumberOfThreadsHighWatermark;
    ULONGLONG      CycleTime;
    LARGE_INTEGER  CreateTime;
    LARGE_INTEGER  UserTime;
    LARGE_INTEGER  KernelTime;
    UNICODE_STRING ImageName;
    LONG           BasePriority;
    HANDLE         UniqueProcessId;
    HANDLE         InheritedFromUniqueProcessId;
    ULONG          HandleCount;
    ULONG          SessionId;
    ULONG_PTR      UniqueProcessKey;
    SIZE_T         PeakVirtualSize;
    SIZE_T         VirtualSize;
    ULONG          PageFaultCount;
    SIZE_T         PeakWorkingSetSize;
    SIZE_T         WorkingSetSize;
    SIZE_T         QuotaPeakPagedPoolUsage;
    SIZE_T         QuotaPagedPoolUsage;
    SIZE_T         QuotaPeakNonPagedPoolUsage;
    SIZE_T         QuotaNonPagedPoolUsage;
    SIZE_T         PagefileUsage;
    SIZE_T         PeakPagefileUsage;
    SIZE_T         PrivatePageCount;
    LARGE_INTEGER  ReadOperationCount;
    LARGE_INTEGER  WriteOperationCount;
    LARGE_INTEGER  OtherOperationCount;
    LARGE_INTEGER  ReadTransferCount;
    LARGE_INTEGER  WriteTransferCount;
};

// One of these per logical processor, from SystemProcessorPerformanceInformation.
// Declared here rather than taken from winternl.h, which hides the fields this
// needs inside a Reserved array.
struct AwaProcessorPerf {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;      // includes IdleTime, as everywhere else
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG         InterruptCount;
};

using NtQuerySystemInformationFn =
    LONG (WINAPI*)(ULONG systemInformationClass, PVOID buffer,
                   ULONG length, PULONG returned);

NtQuerySystemInformationFn NtQuerySI() {
    static NtQuerySystemInformationFn fn = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? (NtQuerySystemInformationFn)GetProcAddress(
                           ntdll, "NtQuerySystemInformation")
                     : nullptr;
    }();
    return fn;
}

// Everything one sample needs to know about one process.
struct ProcSample {
    std::wstring       name;
    unsigned long long cpuTime = 0;    // kernel + user, 100 ns units
    unsigned long long io      = 0;    // bytes read + written
    unsigned long long workingSet = 0;
};

// Previous sample, so CPU and I/O can be turned into rates.
std::unordered_map<DWORD, ProcSample> g_prevProcs;
unsigned long long g_prevProcTime = 0;

std::wstring TrimExe(const std::wstring& name) {
    if (name.size() > 4) {
        const std::wstring tail = ToLower(name.substr(name.size() - 4));
        if (tail == L".exe") return name.substr(0, name.size() - 4);
    }
    return name;
}

// Snapshots every process. Returns false when the call is unavailable.
bool SnapshotProcesses(std::unordered_map<DWORD, ProcSample>* out) {
    NtQuerySystemInformationFn query = NtQuerySI();
    if (!query) return false;

    // Kept between samples. This runs once a second while the overlay is up,
    // and a fresh half-megabyte allocation each time is a pointless round trip
    // through the heap - on an old machine, a measurable one.
    static std::vector<BYTE> buffer(512 * 1024);
    for (int attempt = 0; attempt < 6; ++attempt) {
        ULONG needed = 0;
        const LONG status = query(5 /* SystemProcessInformation */, buffer.data(),
                                  (ULONG)buffer.size(), &needed);
        if (status == 0) break;
        if (status != (LONG)0xC0000004 /* STATUS_INFO_LENGTH_MISMATCH */)
            return false;
        buffer.resize((needed ? needed : (ULONG)buffer.size()) + 64 * 1024);
        if (attempt == 5) return false;
    }

    const BYTE* at  = buffer.data();
    const BYTE* end = buffer.data() + buffer.size();
    for (;;) {
        // The layout of SYSTEM_PROCESS_INFORMATION is not contractual, so the
        // entry is bounds-checked before it is read rather than after.
        if (at + sizeof(AwaProcessInfo) > end) break;
        auto* info = reinterpret_cast<const AwaProcessInfo*>(at);
        const DWORD pid = (DWORD)(ULONG_PTR)info->UniqueProcessId;

        if (pid != 0) {
            ProcSample sample;
            if (info->ImageName.Buffer && info->ImageName.Length)
                sample.name.assign(info->ImageName.Buffer,
                                   info->ImageName.Length / sizeof(wchar_t));
            sample.cpuTime = (unsigned long long)info->KernelTime.QuadPart +
                             (unsigned long long)info->UserTime.QuadPart;
            sample.io = (unsigned long long)info->ReadTransferCount.QuadPart +
                        (unsigned long long)info->WriteTransferCount.QuadPart;
            sample.workingSet = (unsigned long long)info->WorkingSetSize;
            (*out)[pid] = std::move(sample);
        }

        if (info->NextEntryOffset == 0) break;
        at += info->NextEntryOffset;
        if (at >= end) break;
    }
    return true;
}

// "pid_1234_luid_..." - the GPU engine counters name their instances after the
// process that owns the work, which is the only way to attribute GPU load.
DWORD PidFromGpuInstance(const wchar_t* name) {
    if (!name) return 0;
    const wchar_t* p = wcsstr(name, L"pid_");
    if (!p) return 0;
    return (DWORD)_wtoi(p + 4);
}

} // namespace

SystemSampler::~SystemSampler() { Close(); }

void SystemSampler::Configure(const SampleWants& want) {
    const bool pdhChanged = (want.gpu  != want_.gpu)  ||
                            (want.vram != want_.vram) ||
                            (want.disk != want_.disk) ||
                            (want.net  != want_.net);
    want_ = want;

    if (pdhChanged) {
        ClosePdh();                    // reopen with just the counters now wanted
        haveCpuBaseline_ = false;
    }

    ThermalWant(want_.cpuTemp, want_.gpuTemp);
}

void SystemSampler::EnsurePdh() {
    if (openedPdh_) return;
    if (!want_.gpu && !want_.vram && !want_.disk && !want_.net) return;

    if (PdhOpenQueryW(nullptr, 0, &g_query) != ERROR_SUCCESS) {
        g_query = nullptr;
        openedPdh_ = true;             // do not retry every tick
        return;
    }

    if (want_.gpu) {
        // The 3D engine is what people mean by "GPU usage"; summing every engine
        // double-counts video decode and copy work. With per-app attribution on
        // we need the instance names, which the wildcard keeps.
        if (!AddCounter(L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", &g_gpu))
            AddCounter(L"\\GPU Engine(*)\\Utilization Percentage", &g_gpu);
    }
    if (want_.vram) {
        if (!AddCounter(L"\\GPU Adapter Memory(*)\\Dedicated Usage", &g_vram))
            AddCounter(L"\\GPU Process Memory(*)\\Dedicated Usage", &g_vram);
    }
    if (want_.disk)
        AddCounter(L"\\PhysicalDisk(_Total)\\% Disk Time", &g_disk);
    if (want_.net) {
        AddCounter(L"\\Network Interface(*)\\Bytes Received/sec", &g_netRecv);
        AddCounter(L"\\Network Interface(*)\\Bytes Sent/sec", &g_netSend);
    }

    PdhCollectQueryData(g_query);      // first collection primes the rates
    g_primed = false;
    openedPdh_ = true;
}

void SystemSampler::ClosePdh() {
    if (g_query) {
        PdhCloseQuery(g_query);
        g_query = nullptr;
    }
    g_gpu = g_vram = g_disk = g_netRecv = g_netSend = nullptr;
    g_primed = false;
    openedPdh_ = false;
    g_prevProcs.clear();
    g_prevProcTime = 0;
}

void SystemSampler::Close() {
    ClosePdh();
    ThermalStop();
}

// Per-processor busy time, as a fraction of the wall clock since the last
// sample. Same arithmetic as the aggregate above, once per core.
void SystemSampler::SampleCores(Metric* cpu) {
    NtQuerySystemInformationFn query = NtQuerySI();
    if (!query) return;

    DWORD cores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (cores == 0) return;
    // NtQuerySystemInformation reports the *current group* only, which is at
    // most 64 processors; asking for more would read past what it filled in.
    if (cores > 64) cores = 64;

    static std::vector<AwaProcessorPerf> buffer;
    buffer.resize(cores);
    ULONG needed = 0;
    LONG status = query(8 /* SystemProcessorPerformanceInformation */,
                        buffer.data(),
                        (ULONG)(buffer.size() * sizeof(AwaProcessorPerf)), &needed);
    if (status == (LONG)0xC0000004 /* STATUS_INFO_LENGTH_MISMATCH */ && needed) {
        // A machine with more logical processors than the guess above. One
        // retry at the size it asked for; the count does not change again.
        buffer.resize(needed / sizeof(AwaProcessorPerf) + 1);
        status = query(8, buffer.data(),
                       (ULONG)(buffer.size() * sizeof(AwaProcessorPerf)), &needed);
    }
    if (status != 0) return;

    const size_t got = needed / sizeof(AwaProcessorPerf);
    if (got == 0 || got > buffer.size()) return;

    if (prevCores_.size() != got) {
        // First sample, or the machine's processor count changed under us.
        prevCores_.assign(got, CoreTimes{});
        for (size_t i = 0; i < got; ++i) {
            prevCores_[i].idle   = (unsigned long long)buffer[i].IdleTime.QuadPart;
            prevCores_[i].kernel = (unsigned long long)buffer[i].KernelTime.QuadPart;
            prevCores_[i].user   = (unsigned long long)buffer[i].UserTime.QuadPart;
        }
        return;                     // nothing to compare against yet
    }

    cpu->parts.resize(got);
    for (size_t i = 0; i < got; ++i) {
        const unsigned long long idle   = (unsigned long long)buffer[i].IdleTime.QuadPart;
        const unsigned long long kernel = (unsigned long long)buffer[i].KernelTime.QuadPart;
        const unsigned long long user   = (unsigned long long)buffer[i].UserTime.QuadPart;

        const unsigned long long dIdle   = idle   - prevCores_[i].idle;
        const unsigned long long dKernel = kernel - prevCores_[i].kernel;
        const unsigned long long dUser   = user   - prevCores_[i].user;

        // Kernel time already includes idle time, exactly as GetSystemTimes.
        const unsigned long long busy  = (dKernel - dIdle) + dUser;
        const unsigned long long total = dKernel + dUser;

        double pct = total > 0 ? (double)busy * 100.0 / (double)total : 0.0;
        cpu->parts[i] = (std::max)(0.0, (std::min)(100.0, pct));

        prevCores_[i].idle   = idle;
        prevCores_[i].kernel = kernel;
        prevCores_[i].user   = user;
    }
}

void SystemSampler::Sample(SystemLoad* out) {
    *out = SystemLoad();

    // ---- CPU: ratio of non-idle time across the whole system ----
    if (want_.cpu) {
        FILETIME idleFt, kernelFt, userFt;
        if (GetSystemTimes(&idleFt, &kernelFt, &userFt)) {
            const unsigned long long idle   = FileTimeToU64(idleFt);
            const unsigned long long kernel = FileTimeToU64(kernelFt);
            const unsigned long long user   = FileTimeToU64(userFt);

            if (haveCpuBaseline_) {
                const unsigned long long dIdle   = idle - prevIdle_;
                const unsigned long long dKernel = kernel - prevKernel_;
                const unsigned long long dUser   = user - prevUser_;
                // Kernel time already includes idle time.
                const unsigned long long busy  = (dKernel - dIdle) + dUser;
                const unsigned long long total = dKernel + dUser;

                if (total > 0) {
                    double pct = (double)busy * 100.0 / (double)total;
                    pct = (std::max)(0.0, (std::min)(100.0, pct));

                    wchar_t buf[32];
                    swprintf_s(buf, L"%.0f%%", pct);
                    out->cpu.available = true;
                    out->cpu.percent   = pct;
                    out->cpu.value     = buf;
                }
            }
            prevIdle_ = idle; prevKernel_ = kernel; prevUser_ = user;
            haveCpuBaseline_ = true;
        }
        SampleCores(&out->cpu);
        if (!out->cpu.parts.empty() && out->cpu.detail.empty()) {
            wchar_t buf[48];
            swprintf_s(buf, L"%d threads", (int)out->cpu.parts.size());
            out->cpu.detail = buf;
        }
    }

    // ---- RAM ----
    if (want_.ram) {
        MEMORYSTATUSEX mem = {};
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem)) {
            const double used  = (double)(mem.ullTotalPhys - mem.ullAvailPhys);
            const double total = (double)mem.ullTotalPhys;

            out->ram.available = true;
            out->ram.percent   = total > 0 ? used * 100.0 / total : 0.0;
            out->ram.value     = FormatBytes(used);
            out->ram.detail    = L"of " + FormatBytes(total);

            wchar_t pct[16];
            swprintf_s(pct, L"%.0f%%", out->ram.percent);
            out->ram.shortValue = pct;
        }
    }

    // ---- Temperatures: whatever the probe thread last managed to read ----
    if (want_.cpuTemp)
        FillTemperature(true, &out->cpuTemp);
    if (want_.gpuTemp)
        FillTemperature(false, &out->gpuTemp);

    // ---- the busiest process per resource ----
    std::unordered_map<DWORD, ProcSample> procs;
    std::wstring topCpuName, topRamName, topDiskName;
    bool haveProcs = false;

    if (want_.topApps && SnapshotProcesses(&procs)) {
        haveProcs = true;

        const unsigned long long now = GetTickCount64();
        const double elapsedMs = g_prevProcTime ? (double)(now - g_prevProcTime) : 0.0;
        DWORD cores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (cores == 0) cores = 1;

        double bestCpu = 0.0, bestIo = 0.0;
        unsigned long long bestRam = 0;

        for (const auto& entry : procs) {
            const ProcSample& current = entry.second;

            if (current.workingSet > bestRam) {
                bestRam = current.workingSet;
                topRamName = current.name;
            }

            auto previous = g_prevProcs.find(entry.first);
            if (previous == g_prevProcs.end() || elapsedMs <= 0.0) continue;

            if (current.cpuTime > previous->second.cpuTime) {
                // 100 ns units of CPU over milliseconds of wall clock.
                const double busyMs =
                    (double)(current.cpuTime - previous->second.cpuTime) / 10000.0;
                const double pct = busyMs / (elapsedMs * cores) * 100.0;
                if (pct > bestCpu) { bestCpu = pct; topCpuName = current.name; }
            }
            if (current.io > previous->second.io) {
                const double rate =
                    (double)(current.io - previous->second.io) / (elapsedMs / 1000.0);
                if (rate > bestIo) { bestIo = rate; topDiskName = current.name; }
            }
        }

        // A process below a percent of one core is noise, not "the busiest app".
        if (bestCpu < 0.5) topCpuName.clear();
        if (bestIo < 64.0 * 1024.0) topDiskName.clear();

        // `procs` is handed to g_prevProcs at the very end of this function,
        // not here: the GPU block below still needs it to name the process
        // behind the busiest engine instance.
        g_prevProcTime = now;
    }

    out->cpu.topApp  = TrimExe(topCpuName);
    out->ram.topApp  = TrimExe(topRamName);
    out->disk.topApp = TrimExe(topDiskName);

    // ---- PDH-backed counters ----
    EnsurePdh();
    if (g_query && PdhCollectQueryData(g_query) == ERROR_SUCCESS) {
        // The first collection after opening only establishes a baseline.
        if (!g_primed) {
            g_primed = true;
        } else {
            double v = 0.0;
            if (want_.gpu) {
                std::vector<BYTE> buffer;
                DWORD count = 0;
                if (ReadArray(g_gpu, &buffer, &count)) {
                    auto* items =
                        reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
                    double total = 0.0, bestPid = 0.0;
                    DWORD  topPid = 0;
                    std::unordered_map<DWORD, double> perPid;

                    for (DWORD i = 0; i < count; ++i) {
                        if (!ValidItem(items[i])) continue;
                        const double value = items[i].FmtValue.doubleValue;
                        total += value;
                        if (!haveProcs) continue;
                        const DWORD pid = PidFromGpuInstance(items[i].szName);
                        if (pid) perPid[pid] += value;
                    }
                    for (const auto& e : perPid)
                        if (e.second > bestPid) { bestPid = e.second; topPid = e.first; }

                    total = (std::max)(0.0, (std::min)(100.0, total));
                    wchar_t buf[32];
                    swprintf_s(buf, L"%.0f%%", total);
                    out->gpu.available = true;
                    out->gpu.percent   = total;
                    out->gpu.value     = buf;

                    if (topPid && bestPid >= 0.5) {
                        auto it = procs.find(topPid);
                        if (it != procs.end()) out->gpu.topApp = TrimExe(it->second.name);
                    }
                }
            }
            if (want_.vram && ReadMax(g_vram, &v)) {
                const double total = DedicatedVideoMemory();
                out->vram.available = true;
                out->vram.percent   = total > 0 ? (std::min)(100.0, v * 100.0 / total)
                                                : 0.0;
                out->vram.value     = FormatBytes(v);
                out->vram.detail    = total > 0 ? (L"of " + FormatBytes(total))
                                                : std::wstring(L"in use");
                if (total > 0) {
                    wchar_t pct[16];
                    swprintf_s(pct, L"%.0f%%", out->vram.percent);
                    out->vram.shortValue = pct;
                }
            }
            if (want_.disk && ReadSum(g_disk, &v)) {
                v = (std::max)(0.0, (std::min)(100.0, v));
                wchar_t buf[32];
                swprintf_s(buf, L"%.0f%%", v);
                out->disk.available = true;
                out->disk.percent   = v;
                out->disk.value     = buf;
            }
            if (want_.net) {
                double down = 0.0, up = 0.0;
                const bool haveDown = ReadSum(g_netRecv, &down);
                const bool haveUp   = ReadSum(g_netSend, &up);
                if (haveDown || haveUp) {
                    // No meaningful ceiling for a network, so scale the bar
                    // against 12.5 MB/s (a saturated 100 Mbit link).
                    const double busiest = (std::max)(down, up);
                    out->net.available = true;
                    out->net.percent   = (std::min)(100.0, busiest / 12500000.0 * 100.0);
                    out->net.value      = L"↓ " + FormatRate(down);
                    out->net.detail     = L"↑ " + FormatRate(up);
                    out->net.shortValue = FormatRate(busiest);
                }
            }
        }
    }

    // Last use of `procs` is above, so hand the storage over rather than
    // copying it: several hundred entries, each with a heap-allocated name,
    // rebuilt once a second. Copying that was pure waste.
    if (haveProcs) g_prevProcs = std::move(procs);
}

} // namespace awa
