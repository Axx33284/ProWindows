// ProWindows - CPU and GPU temperature.
//
// Split out of `sysinfo.*` because temperature is the one reading Windows has
// no straight answer for. Every other metric is a single documented call; this
// one is a queue of sources, each of which works on some machines and not on
// others, tried in order of how much the number can be trusted:
//
//   CPU   The real package sensor lives behind an MSR, and reading an MSR needs
//         a kernel driver. The only honest way to have one without shipping one
//         is to use the driver somebody else already installed and signed, so
//         the first three sources are all "ask a tool the user is already
//         running", in order of how cheap they are to ask:
//
//           1. LibreHardwareMonitor / OpenHardwareMonitor, over WMI  ("package")
//           2. HWiNFO, through its shared memory block               ("hwinfo")
//           3. Core Temp, through its shared memory block            ("core temp")
//
//         The two shared-memory ones are a memcpy out of a mapped view rather
//         than a WMI round trip, so they are retried on every pass and starting
//         either tool mid-session simply starts working. HWiNFO needs "Shared
//         Memory Support" switched on in its own settings.
//
//         Failing all three: the ACPI thermal zone, read through the "Thermal
//         Zone Information" performance counter. That counter matters because
//         it is the same zone `MSAcpi_ThermalZoneTemperature` exposes except it
//         can be read without administrator rights, which is why the WMI class
//         alone left this blank for most people. The class stays as a last
//         resort for machines that publish it but not the counter.
//
//         The zone is *not* the package: on a desktop it is usually a mainboard
//         sensor and it barely moves. Which source answered is published in
//         `ThermalReading::source` and printed by the overlay, because "62 on
//         the package" and "62 on the mainboard" are not the same claim.
//   GPU   NVML (`nvml.dll`, ships with every NVIDIA driver) or ADL
//         (`atiadlxx.dll`, ditto for AMD). Both are unprivileged and exact, so
//         they come first. Then HWiNFO, then LibreHardwareMonitor, which
//         between them cover Intel Arc and anything else.
//
// `tests\tempprobe.bat` prints what this machine can answer and which source
// did; `tempprobe.bat --selftest` publishes synthetic Core Temp and HWiNFO
// blocks and checks they are read back correctly, because a reader for a struct
// nobody on this machine publishes is a reader nobody has ever run.
//
// The whole thing lives on one background thread: a WMI round trip costs tens
// of milliseconds and the overlay redraws on a timer. Callers only ever read
// the last answer the thread left behind.
#pragma once
#include "common.h"

namespace awa {

struct ThermalReading {
    bool   have    = false;
    double celsius = 0.0;
    // Which source answered, so the overlay can say what the number actually
    // measures - a package sensor and a mainboard thermal zone are not the
    // same claim. Never null.
    const wchar_t* source = L"";
};

// Why there is no reading, when there is none.
enum class ThermalStatus {
    Unknown,   // the probe has not finished its first pass yet
    Ok,
    Denied,    // a source exists but refused to be read
    Missing,   // nothing on this machine can answer
};

// Which readings the overlay wants. Passing false for both stops the thread;
// this is called every time the monitor's configuration changes, so it is
// cheap and idempotent when nothing has moved.
void ThermalWant(bool cpu, bool gpu);

// The last reading the probe published. False when there is none yet.
bool ThermalReadCpu(ThermalReading* out);
bool ThermalReadGpu(ThermalReading* out);

ThermalStatus ThermalCpuStatus();
ThermalStatus ThermalGpuStatus();

// Stops the probe and releases every source. Safe to call when not running.
void ThermalStop();

} // namespace awa
