// ProWindows - Start menu styling.
//
// A port of m417z's Windhawk "Windows 11 Start Menu Styler" (GPL-3.0) lives in
// ProWindows_startmenu.dll. This file is the ProWindows side, the sibling of
// explorerstyler.h: it writes the styler's settings (startmenu-styler.ini),
// loads the DLL into StartMenuExperienceHost.exe and SearchHost.exe of this
// session, tells them when the settings changed, and takes the DLL out again
// when styling is turned off or ProWindows quits.
//
// What differs from Explorer: those two are packaged AppContainer processes.
// Everything the DLL touches - the DLL itself, the ini, the log, the files used
// as signals - lives in %APPDATA%\ProWindows\startmenu-styler\ and is made
// readable (and, for the folder, writable) for ALL APPLICATION PACKAGES and
// ALL RESTRICTED APPLICATION PACKAGES first. See startmenustyler.cpp for how
// ProWindows and the DLL talk to each other from outside the container.
//
// Everything that can block - walking processes, writing into another process,
// waiting for a remote thread - runs on one worker thread; the calls below only
// record what is wanted and wake it (MAP "Start menu styling").
#pragma once
#include "common.h"
#include "config.h"

namespace awa {

// `notify` is the main window: the worker posts WM_AWA_STYLERCRASH to it when
// the crash guard trips (the config is only ever changed on the UI thread).
void StartMenuStylerInit(Config* cfg, HWND notify);

// The config changed (or was just loaded): inject, reload or stop to match.
void StartMenuStylerApplyConfig();

// A window became the foreground window. When it is the Start menu or the
// search flyout of a process that is not styled yet, asks the worker to style
// it. Cheap enough to call for every event.
void StartMenuStylerForeground(HWND hwnd);

// The shell was restarted (TaskbarCreated): the hosts may have been too.
void StartMenuStylerTaskbarCreated();

// UI thread, from WM_AWA_STYLERCRASH: the hosts kept exiting right after an
// injection. Turns styling off (saved), balloon, log.
void StartMenuStylerCrashTripped();

// Makes every styled host re-read startmenu-styler.ini.
void StartMenuStylerReload();

// Takes the styler out of every host. Does not wait for them to finish.
void StartMenuStylerShutdown();

// "2 Start menu processes styled" / the reason it is not.
std::wstring StartMenuStylerStatus();

// %APPDATA%\ProWindows\startmenu-styler and the ini in it. EnsureIni creates
// the folder (with the package ACL) and the ini, with its explanatory
// comments, if they do not exist yet.
std::wstring StartMenuStylerDir();
std::wstring StartMenuStylerIniPath();
void StartMenuStylerEnsureIni();
// Rewrites the part of the ini ProWindows owns from the Start fields of `c`
// and leaves the rest alone; true when the file changed. The worker's own
// step, exposed for tests\styler_test.
bool StartMenuStylerWriteIni(const Config& c);
// Gives ALL APPLICATION PACKAGES and ALL RESTRICTED APPLICATION PACKAGES read
// and execute on a file, or (isDir) read, write and delete on a folder and
// everything made in it later. Keeps the existing ACL. Exposed for tests.
bool StartMenuStylerGrantPackages(const std::wstring& path, bool isDir);

// The choices the settings page offers. Index 0 of the themes is the default.
int StartThemeCount();
const wchar_t* StartThemeId(int i);        // the mod's name for it ("" = none)
const wchar_t* StartThemeName(int i);      // what the page calls it
int StartThemeIndexOf(const std::wstring& id);   // -1 if unknown
bool StartThemeIsProWindows(const std::wstring& id);   // the two themes the Look rows drive

int StartLayoutCount();
const wchar_t* StartLayoutId(int i);       // config.ini value ("" = the theme's)
const wchar_t* StartLayoutName(int i);
int StartLayoutIndexOf(const std::wstring& id);  // -1 if unknown

// Tint swatches: black, graphite, steel, the system accent colour.
int StartTintCount();
const wchar_t* StartTintName(int i);
COLORREF StartTintColor(int i);

} // namespace awa
