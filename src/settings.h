// ProWindows - the settings window
#pragma once
#include "common.h"

namespace awa {

// Creates the (modeless) settings window. Safe to call more than once; a second
// call just brings the existing window forward.
HWND SettingsOpen(HINSTANCE inst);

// Tab order. Named rather than written out as bare numbers because callers
// outside the settings window address tabs by index, and inserting a page in
// the middle renumbered them silently once already.
enum PageIndex {
    PAGE_LAYOUT = 0,
    PAGE_BEHAVIOUR,
    PAGE_SHORTCUTS,
    PAGE_APPS,
    PAGE_SEARCH,
    PAGE_MONITOR,
    PAGE_CLOCK,
    PAGE_GENERAL,
    PAGE_COUNT
};

// Opens the window with a particular tab selected.
void SettingsOpenTab(int index);

void SettingsHide();
bool SettingsVisible();
HWND SettingsWindow();               // nullptr until first opened

// Re-reads the live config/manager state into the controls.
void SettingsRefresh();

// Updates just the status line (cheap; called while the window is visible).
void SettingsRefreshStatus();

void SettingsDestroy();

} // namespace awa
