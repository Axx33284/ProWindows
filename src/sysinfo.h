// ProWindows - machine load sampling for the monitor overlay.
//
// CPU and RAM come from plain Win32. GPU, VRAM, disk and network come from PDH
// performance counters, which is the same data Task Manager shows. The two
// temperatures come from `thermal.*`, which runs its own thread because no
// source for them is fast enough for the UI thread. Each of these is optional
// and simply reports "unavailable" if the machine does not expose it.
#pragma once
#include "common.h"

namespace awa {

struct Metric {
    bool   available = false;
    double percent   = 0.0;    // 0..100, for the bar and the graph
    std::wstring value;        // headline text, e.g. "38%" or "12.4 GB"
    // Same reading in as few characters as possible, for the gauge styles
    // where "12.4 GB" will not fit inside a ring. Empty means "use value".
    std::wstring shortValue;
    std::wstring detail;       // smaller second line, may be empty
    std::wstring topApp;       // busiest process for this resource, may be empty

    // The individual parts behind the headline number: one entry per logical
    // processor for the CPU, 0..100 each. Empty for every other metric.
    //
    // A single averaged bar cannot tell "one thread pinned" from "everything
    // at a quarter", and those are completely different situations - the first
    // is a program stuck in a loop and the second is a machine working. The
    // overlay draws the CPU bar divided into these instead.
    std::vector<double> parts;
};

struct SystemLoad {
    Metric cpu;
    Metric ram;
    Metric gpu;
    Metric vram;               // dedicated video memory in use
    Metric cpuTemp;            // CPU package or thermal zone, degrees C
    Metric gpuTemp;            // GPU die, degrees C
    Metric disk;
    Metric net;
};

// Which counters to keep open. Anything switched off costs nothing.
struct SampleWants {
    bool cpu = true, ram = true, gpu = true;
    bool vram = false, cpuTemp = false, gpuTemp = false;
    bool disk = false, net = false;
    // Name the process using the most of each resource. Costs one process
    // snapshot per sample, so it is off unless asked for.
    bool topApps = false;
};

// Threading. Sample() and Close() belong to one thread - the overlay's
// sampling thread, see monitor.cpp - and are never called from anywhere else.
// Configure() may be called from any thread: it hands the new wants over
// under a lock and they take effect at the start of the next Sample().
//
// The reason there is a sampling thread at all: PdhCollectQueryData on the
// GPU Engine wildcard counter enumerates every process's GPU engines and is
// not bounded - tens of milliseconds is normal, and opening the counter for
// the first time after logon can take seconds while the counter provider
// starts up. Doing that on the UI thread meant a stall in every animation
// that happened to overlap a sample, and a frozen tray icon at startup.
//
// Temperatures are not this class's business: thermal.* runs its own thread
// and is controlled from the UI thread by monitor.cpp. Sample() only reads
// what that thread last published.
class SystemSampler {
public:
    SystemSampler();
    // Deliberately does not close anything. This is a static object, so the
    // destructor runs at process exit, possibly while an abandoned sampling
    // thread is still inside a PDH call; closing the query under it would
    // fault. The thread closes what it opened on its own way out, and the OS
    // reclaims anything that thread never got to.
    ~SystemSampler() = default;

    // Any thread. Takes effect on the next Sample().
    void Configure(const SampleWants& want);

    // Sampling thread only. Reads every configured counter.
    void Sample(SystemLoad* out);

    // Sampling thread only. Releases the counters; the next Sample() reopens
    // whatever is still wanted.
    void Close();

private:
    // Handed over by Configure(), picked up by Sample().
    CRITICAL_SECTION lock_;
    SampleWants pending_;
    bool        pendingDirty_ = false;
    void ApplyPending();

    SampleWants want_;
    bool openedPdh_ = false;

    // CPU is derived from deltas of the system times, so the previous reading
    // has to be kept.
    unsigned long long prevIdle_ = 0, prevKernel_ = 0, prevUser_ = 0;
    bool  haveCpuBaseline_ = false;

    // The same, per logical processor. One extra kernel call per sample for a
    // few kilobytes of counters, which is what Task Manager's own graph grid
    // costs it too.
    struct CoreTimes { unsigned long long idle = 0, kernel = 0, user = 0; };
    std::vector<CoreTimes> prevCores_;
    void SampleCores(Metric* cpu);

    void EnsurePdh();
    void ClosePdh();     // counters only; leaves the temperature thread alone
};

} // namespace awa
