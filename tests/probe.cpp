// ProWindows - read-only window probe.
//
// Prints how every top-level window on this desktop would be classified, what
// it says its size limits are, and whether it is out of our reach. It moves
// nothing, so it is safe to run on a live desktop - which the tiler itself is
// not, and which is why this exists.
//
//   tests\probe.bat
//
// Use it to answer "why is this window not being arranged?" without turning
// the tiler loose on somebody's real session.

#include "../src/winutil.h"
#include <tlhelp32.h>
#include <cstdio>

using namespace awa;

static const char* VerdictName(ManageVerdict v) {
    switch (v) {
        case ManageVerdict::Tile:  return "tile";
        case ManageVerdict::Float: return "float";
        default:                   return "ignore";
    }
}

static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

struct Counters { int tile = 0, floated = 0, ignored = 0, elevated = 0, limited = 0; };

static BOOL CALLBACK OnWindow(HWND h, LPARAM lp) {
    auto* c = (Counters*)lp;

    if (!IsWindowVisible(h)) return TRUE;
    if (GetAncestor(h, GA_ROOT) != h) return TRUE;
    const std::wstring title = WindowTitle(h);
    if (title.empty()) return TRUE;

    Config cfg;                       // defaults only: no user rules involved
    const ManageVerdict v = Classify(h, cfg);
    const bool elevated = IsElevatedProcess(h);

    switch (v) {
        case ManageVerdict::Tile:  ++c->tile; break;
        case ManageVerdict::Float: ++c->floated; break;
        default:                   ++c->ignored; break;
    }
    if (elevated) ++c->elevated;

    // Only interesting for windows we would actually try to arrange.
    SizeLimits lim;
    if (v == ManageVerdict::Tile) {
        lim = QuerySizeLimits(h);
        if (lim.constrained()) ++c->limited;
    }

    const Rect r = VisibleRect(h);
    std::printf("%-7s %-5s  %4dx%-4d  min %4dx%-4d  max %5s x%-5s  %-22.22s  %s\n",
                VerdictName(v),
                elevated ? "ELEV" : "",
                r.w, r.h,
                lim.minW, lim.minH,
                lim.maxW >= kNoLimit ? "-" : std::to_string(lim.maxW).c_str(),
                lim.maxH >= kNoLimit ? "-" : std::to_string(lim.maxH).c_str(),
                Narrow(ProcessName(h)).c_str(),
                Narrow(title).c_str());
    return TRUE;
}

// Proves the branch the window walk above cannot reach on a machine with
// nothing running elevated: a System-integrity process must come back as out of
// reach, and an ordinary one must not.
//
// This is the case the old implementation got wrong. It asked only whether
// OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) failed - and that access right
// is granted across integrity levels, so it succeeded and the process was
// reported as reachable. The line marked "opens: yes" below is that bug, still
// visible; what changed is the verdict beside it.
// Walks every process on the machine, reads its integrity level, and checks
// that ProcessOutOfReach agrees with the only rule that matters: a process
// above us is out of reach, one at or below us is not.
//
// This covers the branch the named cases below cannot, because on a machine
// with nothing running elevated the interesting comparison is the one against
// browser sandbox processes, which sit BELOW us and must stay reachable.
static void CheckIntegrityRule() {
    const DWORD ours = OwnProcessIntegrity();
    std::printf("\nintegrity rule (ours = 0x%04lX)\n------------------------------\n", ours);

    int readable = 0, above = 0, below = 0, same = 0, wrong = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { std::printf("  skip  no process snapshot\n"); return; }

    PROCESSENTRY32W pe{ sizeof(PROCESSENTRY32W) };
    if (Process32FirstW(snap, &pe)) {
        do {
            const DWORD il = ProcessIntegrity(pe.th32ProcessID);
            if (!il) continue;                       // could not read it: not this test
            ++readable;
            if (il > ours) ++above; else if (il < ours) ++below; else ++same;

            if (ProcessOutOfReach(pe.th32ProcessID) != (il > ours)) {
                ++wrong;
                std::printf("  FAIL  %-22S il 0x%04lX\n", pe.szExeFile, il);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    std::printf("  read %d processes: %d above us, %d below, %d the same\n",
                readable, above, below, same);
    std::printf("  %s\n", wrong == 0 ? "PASS  every verdict matches the integrity comparison"
                                     : "FAIL  see above");
}

static void CheckReach() {
    struct Case { const wchar_t* name; bool expectOutOfReach; };
    const Case cases[] = {
        // Protected: OpenProcess itself fails. Exercises the "cannot even look"
        // branch.
        { L"wininit.exe",  true  },
        { L"winlogon.exe", true  },
        // The one that matters. A service runs at System integrity but is NOT
        // protected, so OpenProcess succeeds - "opens: yes, out of reach: yes"
        // below is exactly the combination the old check could not represent,
        // and exactly what an elevated app looks like to us.
        { L"svchost.exe",  true  },
        // Ours, same integrity level. Must stay reachable, or the tiler would
        // stop arranging ordinary windows.
        { L"explorer.exe", false },
    };

    std::printf("\nreachability\n------------\n");
    for (const auto& c : cases) {
        DWORD pid = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{ sizeof(PROCESSENTRY32W) };
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, c.name) == 0) { pid = pe.th32ProcessID; break; }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        if (!pid) { std::printf("  skip  %-16S not running\n", c.name); continue; }

        HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        const bool opens = probe != nullptr;
        if (probe) CloseHandle(probe);

        const bool out = ProcessOutOfReach(pid);
        const bool ok  = (out == c.expectOutOfReach);
        std::printf("  %-4s  %-16S opens: %-3s  out of reach: %-3s (expected %s)\n",
                    ok ? "PASS" : "FAIL", c.name,
                    opens ? "yes" : "no",
                    out ? "yes" : "no",
                    c.expectOutOfReach ? "yes" : "no");
    }
}

int main() {
    std::printf("verdict elev   size        minimum        maximum         process"
                "                 title\n");
    std::printf("--------------------------------------------------------------"
                "-------------------------------\n");
    Counters c;
    EnumWindows(OnWindow, (LPARAM)&c);

    std::printf("\n%d would be tiled, %d floated, %d ignored.\n", c.tile, c.floated, c.ignored);
    std::printf("%d out of reach (higher integrity level).\n", c.elevated);
    std::printf("%d of the tiled ones declare a size limit.\n", c.limited);
    CheckReach();
    CheckIntegrityRule();
    std::printf("\nRunning %s.\n", SelfIsElevated() ? "elevated" : "unelevated");
    return 0;
}
