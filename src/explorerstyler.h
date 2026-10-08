// ProWindows - File Explorer styling.
//
// A port of m417z's Windhawk "Windows 11 File Explorer Styler" (GPL-3.0) lives in
// ProWindows_explorer.dll. This file is the ProWindows side: it writes the
// styler's settings (explorer-styler.ini), loads the DLL into every explorer.exe
// of this session, tells it when the settings changed, and takes it out again
// when styling is turned off or ProWindows quits.
//
// Everything that can block - walking processes, writing into another process,
// waiting for a remote thread - runs on one worker thread; the calls below only
// record what is wanted and wake it (MAP "Explorer styling").
#pragma once
#include "common.h"
#include "config.h"

namespace awa {

void ExplorerStylerInit(Config* cfg);

// The config changed (or was just loaded): inject, reload or stop to match.
void ExplorerStylerApplyConfig();

// The shell was restarted (TaskbarCreated). Counts restarts shortly after an
// injection and, if Explorer keeps restarting, turns styling off. Then styles
// the new Explorer.
void ExplorerStylerTaskbarCreated();

// A window became the foreground window. When it is a File Explorer window of
// a process that is not styled yet (folder windows in their own process), asks
// the worker to style that process. Cheap enough to call for every event.
void ExplorerStylerForeground(HWND hwnd);

// Makes every styled Explorer re-read explorer-styler.ini.
void ExplorerStylerReload();

// Takes the styler out of every Explorer. Does not wait for them to finish.
void ExplorerStylerShutdown();

// "2 Explorer processes styled" / the reason it is not.
std::wstring ExplorerStylerStatus();

// %APPDATA%\ProWindows\explorer-styler.ini; created with its explanatory
// comments if it does not exist yet.
std::wstring ExplorerStylerIniPath();
void ExplorerStylerEnsureIni();
// Rewrites the part of the ini ProWindows owns and leaves the rest alone; true
// when the file changed. The worker's own step, exposed for tests\styler_test.
bool ExplorerStylerWriteIni(const std::wstring& theme, const std::wstring& effect, bool debug);

// The choices the settings page offers. Index 0 of the themes is the default.
int ExplorerThemeCount();
const wchar_t* ExplorerThemeId(int i);     // the mod's name for it ("" = none)
const wchar_t* ExplorerThemeName(int i);   // what the page calls it
int ExplorerThemeIndexOf(const std::wstring& id);   // -1 if unknown

int ExplorerEffectCount();
const wchar_t* ExplorerEffectId(int i);    // config.ini value ("" = the theme's)
const wchar_t* ExplorerEffectName(int i);
int ExplorerEffectIndexOf(const std::wstring& id);  // -1 if unknown

} // namespace awa
