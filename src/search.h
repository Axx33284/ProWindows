// ProWindows - what the search bar can find.
//
// The search bar itself (launcher.cpp) owns the window, the query and the
// ranking. This file owns the *sources*: the file index, the Windows settings
// pages, the calculator, and the fuzzy scorer they all share. Nothing here
// touches a window, so it can be reasoned about - and the indexer thread can
// run - without any of the UI in the way.
#pragma once
#include "common.h"
#include "config.h"

namespace awa {

enum class HitKind {
    App,        // an installed application
    Program,    // an .exe found by walking, with no Start-menu entry of its own
    File,       // an indexed file
    Folder,     // an indexed directory
    Setting,    // an ms-settings: page
    Calc,       // the answer to an arithmetic query
    Command,    // run the query as typed
};

struct SearchHit {
    HitKind      kind   = HitKind::App;
    std::wstring name;      // the bright first line
    std::wstring detail;    // the dim second line: a folder, a hint, a result
    std::wstring target;    // what to open; for Calc, the answer to copy
    int          score  = 0;
};

// ---------------------------------------------------------------- scoring
// A subsequence match. `lowQuery` must already be lower-cased; `name` may be
// any case and is folded a character at a time as it is scanned. That asymmetry
// is deliberate: it means the file index does not have to store a second,
// lower-cased copy of every name, which was the single largest thing this
// process held. Returns false when the query's letters do not appear in order.
// The pointer form is the real one: the file index scores the tail of a path
// in place, so a keystroke over 20,000 entries allocates nothing at all.
bool SearchScore(const wchar_t* name, size_t nameLen,
                 const std::wstring& lowQuery, int* out);

inline bool SearchScore(const std::wstring& name, const std::wstring& lowQuery, int* out) {
    return SearchScore(name.c_str(), name.size(), lowQuery, out);
}

// ---------------------------------------------------------------- file index
// The folders indexed when the config names none: Desktop, Documents,
// Downloads, Pictures, Music, Videos. Resolved through the shell, so they are
// right even when the user has moved them.
std::vector<std::wstring> SearchDefaultFolders();

void SearchInit(Config* cfg);
void SearchShutdown();

// Rebuilds the index on a background thread. Safe to call while one is running
// - it declines rather than starting a second.
void SearchReindex();

// Called after the config is reloaded. Re-walks only if the settings that
// shape the index actually changed, so pressing Apply does not cost a full
// walk of your home folder every time. Frees the index when file search is
// switched off.
void SearchApplyConfig();

bool SearchIndexReady();
int  SearchIndexCount();

// Appends up to `limit` file and folder matches for an already-lowercased query.
// Executables are not included; they come back from SearchPrograms instead.
void SearchFiles(const std::wstring& lowQuery, int limit, std::vector<SearchHit>* out);

// Appends up to `limit` executables. Separate from SearchFiles, and capped
// separately, because a query that matches a lot of documents would otherwise
// push the one program you were after off the end of the list - which is the
// whole of MAP.md invariant 16 applied to a source that did not exist when it
// was written.
//
// The pool is every .exe the walk found: under the indexed folders, and under
// Program Files, Program Files (x86) and %LOCALAPPDATA%\Programs, which is
// where an application that never made a Start-menu shortcut actually lives.
void SearchPrograms(const std::wstring& lowQuery, int limit,
                    std::vector<SearchHit>* out);

// ---------------------------------------------------------------- other sources
// Appends matching Windows settings pages (Display, Bluetooth, Sound, ...).
void SearchSettingsPages(const std::wstring& lowQuery, int limit,
                         std::vector<SearchHit>* out);

// Evaluates the query as arithmetic. False unless the whole string parses and
// it actually looks like a sum, so "cal" does not become a calculation.
bool SearchCalc(const std::wstring& query, std::wstring* result);

} // namespace awa
