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

class SystemSampler {
public:
    ~SystemSampler();

    void Configure(const SampleWants& want);

    // Reads every configured counter. Cheap enough for a once-a-second timer.
    void Sample(SystemLoad* out);

    void Close();

private:
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
