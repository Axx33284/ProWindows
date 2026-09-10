// Runs the search bar's *index* on its own and prints what it found.
//
// This exists because the walk is the one part of the search bar whose
// correctness depends entirely on the machine it is running on: which drives
// are fixed, what is installed on them, and how deeply a game buries its
// executable. None of that can be asserted in a unit test, and running the
// whole application to find out rearranges every window on the desktop.
//
// No window is created and nothing is launched. It walks, reports, and stops.
//
//   searchprobe                 walk with the shipped defaults, then report
//   searchprobe --no-drives     only Program Files, for comparison
//   searchprobe --deep          include an application's own helper .exe files
//   searchprobe <query> ...     also show what those queries would return
#include "../src/search.h"
#include "../src/config.h"
#include <cstdio>
#include <clocale>

using namespace awa;

namespace {

void Report(const wchar_t* what, const std::vector<SearchHit>& hits) {
    wprintf(L"\n  %s -> %d\n", what, (int)hits.size());
    for (const auto& h : hits) {
        const wchar_t* kind = (h.kind == HitKind::Program) ? L"program"
                            : (h.kind == HitKind::Folder)  ? L"folder"
                                                           : L"file";
        wprintf(L"    %-7s %-32s  %s\n", kind, h.name.c_str(), h.detail.c_str());
    }
}

// Which drive each program came off, so "it walked D:" is something the output
// actually shows rather than something the reader has to take on trust.
void ByDrive(const std::vector<std::wstring>& queries) {
    int counts[26] = {};
    int total = 0;

    // There is no "give me the whole index" call, and adding one for a probe
    // would be the wrong shape. Every path starts with a drive letter, so a
    // handful of single-letter queries reaches a broad, unbiased sample.
    static const wchar_t* kProbes[] = { L"a", L"e", L"i", L"o", L"s", L"t", L"r", L"n" };
    for (const wchar_t* p : kProbes) {
        std::vector<SearchHit> hits;
        SearchPrograms(p, 400, &hits);
        for (const auto& h : hits) {
            if (h.target.size() < 2 || h.target[1] != L':') continue;
            const wchar_t c = (wchar_t)towupper(h.target[0]);
            if (c < L'A' || c > L'Z') continue;
            ++counts[c - L'A'];
            ++total;
        }
    }

    wprintf(L"\n  programs seen by drive (sampled, duplicates included):\n");
    for (int i = 0; i < 26; ++i)
        if (counts[i])
            wprintf(L"    %c:  %d\n", (wchar_t)(L'A' + i), counts[i]);
    if (!total) wprintf(L"    (none)\n");
    (void)queries;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Not _O_U16TEXT: it is right for a console and wrong for a pipe, and the
    // first thing anybody does with this is pipe it somewhere.
    setlocale(LC_ALL, "");

    Config cfg;
    cfg.LoadDefaults();
    cfg.searchFiles    = true;
    cfg.searchPrograms = true;

    std::vector<std::wstring> queries;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if      (a == L"--no-drives") cfg.searchDrives  = false;
        else if (a == L"--drives")    cfg.searchDrives  = true;
        else if (a == L"--deep")      cfg.searchDeepExe = true;
        else queries.push_back(ToLower(a));
    }

    wprintf(L"ProWindows search index probe\n");
    wprintf(L"  every drive : %s\n", cfg.searchDrives  ? L"yes" : L"no");
    wprintf(L"  helper exes : %s\n", cfg.searchDeepExe ? L"yes" : L"no");
    wprintf(L"  ceiling     : %d files, %d programs\n",
            cfg.searchMaxEntries, cfg.searchMaxPrograms);

    SearchInit(&cfg);
    SearchReindex();          // no hold-off: this is what was asked for

    wprintf(L"\n  walking...");
    fflush(stdout);
    const ULONGLONG began = GetTickCount64();
    // The walk is on its own thread. Nothing here is time-critical, so waiting
    // on a poll is simpler than exposing a handle the application never needs.
    for (int i = 0; i < 1200 && !SearchIndexReady(); ++i) Sleep(250);
    const ULONGLONG took = GetTickCount64() - began;

    if (!SearchIndexReady()) {
        wprintf(L"\n  the walk did not finish within five minutes.\n");
        SearchShutdown();
        return 1;
    }
    wprintf(L" %d entries in %llu ms\n", SearchIndexCount(), took);

    ByDrive(queries);

    for (const auto& q : queries) {
        std::vector<SearchHit> hits;
        SearchPrograms(q, 8, &hits);
        Report((L"programs matching \"" + q + L"\"").c_str(), hits);
        hits.clear();
        SearchFiles(q, 5, &hits);
        Report((L"files matching \"" + q + L"\"").c_str(), hits);
    }

    SearchShutdown();
    return 0;
}
