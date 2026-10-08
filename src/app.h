// Services the settings window needs from the application shell.
// Implemented in main.cpp; keeps settings.cpp free of the tray/hotkey plumbing.
#pragma once
#include "common.h"
#include "config.h"
#include "wm.h"

namespace awa {

Config&        AppConfig();
WindowManager& AppWm();

// Writes the current Config to disk, then reloads it so hotkeys, rules and the
// layout all pick the new values up. False when the file could not be written:
// the settings are then in effect from memory but will not survive a restart.
bool AppApplySettings();

void AppShowShortcuts();
void AppOpenConfigFile();
void AppRetileNow();
void AppUpdateTray();
void AppTrayBalloon(const wchar_t* title, const wchar_t* text);

// Writes the current Config straight to disk without a reload. Used by the
// monitor overlay, which changes settings from its own context menu.
void AppSaveConfig();

// Asks the shell to give memory back after `delayMs` of nothing happening:
// unused COM libraries unloaded, the heap compacted, and the working set
// trimmed to what is actually being touched. Called after anything large has
// just been put away - the settings window, the search bar - and on a slow
// timer besides. Re-arming it postpones it, so a burst of activity ends in
// one trim, not several.
void AppScheduleTrim(UINT delayMs);

// Pulls the settings window's controls back into step with the live Config.
void AppRefreshSettings();

// Re-applies which overlays may be on screen (game mode, display off, and each
// one's own "shown" setting). The timer's alarm calls it after showing itself.
void AppUpdateOverlays();

// Opens the settings window on the Monitor tab.
void AppOpenMonitorSettings();
// ... and on the Clock tab.
void AppOpenClockSettings();

bool AppAutostartEnabled();
void AppSetAutostart(bool on);

// The maintenance actions the tray menu offers, so the General settings page
// can offer them too without a second implementation of any of them. Each one
// is the same code the menu item runs.
void AppOpenConfigFolder();
void AppWriteDiagnostics();     // writes diagnostics.txt and opens it
void AppReloadFromDisk();       // re-reads config.ini, re-registers hotkeys
void AppRestoreHiddenWindows(); // the workspace-hiding safety net
// Throws every setting away and starts again from the built-in defaults:
// keybindings, exclusions and learned window limits included. Does not ask -
// the caller has, together with whatever it has to say about unapplied edits.
// False when the defaults could not be written to disk (see AppApplySettings).
bool AppRestoreDefaults();
// A short account of what this install is: version, where it is, whether it is
// elevated, and how many windows it is not allowed to touch.
std::wstring AppAboutText();

// The window manager has entered or left the parked state it uses while a
// fullscreen application owns the screen. The shell responds by stopping its
// own periodic work - the focus-follows-mouse poll, the system monitor
// overlay's sampling and repaint - and by arranging for someone to notice when
// the game goes away, since no window events arrive while it is running.
void AppGameModeChanged(bool on);

// How many bindings the OS refused at registration time (shown in the status
// line, since a stolen chord is otherwise silently missing).
int  AppHotkeyConflicts();

// Figures for the settings window's meters. Each is cheap to call.
int  AppMemoryMB();             // ProWindows' private bytes, in MB
int  AppManagedWindows();       // windows the tiler is arranging
int  AppIndexEntries();         // entries in the search file index
int  AppIconCacheCount();       // icons held for the search bar
int  AppSamplerCostTenths();    // the monitor's last sample, in 0.1 ms; -1 when not sampling

} // namespace awa
