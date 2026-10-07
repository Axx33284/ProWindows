// ProWindows - the settings window
#pragma once
#include "common.h"

namespace awa {

// Creates the settings window. Safe to call more than once; a second call just
// brings the existing window forward.
HWND SettingsOpen(HINSTANCE inst);

// Category order - the order of the column down the left. Named rather than
// written out as bare numbers because callers outside the settings window
// address categories by index, and inserting one in the middle renumbered
// them silently once already.
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

// Opens the window with a particular category selected.
void SettingsOpenTab(int index);

// The settings window is one custom-drawn window that takes its own keys, so
// the message loop has nothing to pre-translate for it. Kept so the loop does
// not have to know that; always false.
bool SettingsTranslateMessage(const MSG* msg);

void SettingsHide();
bool SettingsVisible();
HWND SettingsWindow();               // nullptr until first opened

// Re-reads the live config into the window - unless there are edits that have
// not been applied, which are kept.
void SettingsRefresh();

// Updates just the status in the header (cheap; called while the window is
// visible).
void SettingsRefreshStatus();

void SettingsDestroy();

} // namespace awa
