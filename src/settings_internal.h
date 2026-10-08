// Shared plumbing between the settings window and its pages.
//
// The window edits a copy of the live Config (`Edit()`), and every row of
// every page points straight into that copy. Nothing reaches the live Config
// until Apply, which copies over exactly the fields the pages own - so a
// change made meanwhile from somewhere else (the monitor's own context menu
// moving it, a learned window limit) is never overwritten with a stale copy.
//
// A page is a function that builds its rows, one that puts its part of the
// edit copy back to the defaults, and optionally a preview drawn above the
// description of whatever row has focus.
#pragma once
#include "common.h"
#include "config.h"
#include "app.h"
#include "rowlist.h"
#include <functional>

namespace awa {

// ---------------------------------------------------------------- the edit copy
Config&       Edit();            // what the rows change
const Config& Saved();           // the live config, for "has this changed"

// Where the footer prompt for `key` (VK_ESCAPE, VK_TAB, 'R', ...) was last drawn, in client
// coordinates; empty when it is not shown. For the click probe.
RECT SettingsPromptRect(UINT key);

// Settings that live outside config.ini: the Run key and the elevated logon
// task. Edited like everything else and applied with it.
struct EditExtras {
    bool autostart = false;
    bool elevated  = false;
};
EditExtras&       EditExtra();
const EditExtras& SavedExtra();

// Whether two configs agree on every field the settings window edits.
bool EditedFieldsEqual(const Config& a, const Config& b);

// Whether anything in the edit copy has not been applied.
bool EditsPending();
// Throws the unapplied edits away. For an action that replaces the live
// config wholesale (reload, restore every default): without it, the refresh
// that follows would carry the edits over onto the new config, which is right
// for a change made from an overlay's menu and wrong after "changes you have
// not applied will be lost".
void DiscardEdits();

// ---------------------------------------------------------------- the shell
HWND SettingsHwnd();
// The rows of the current page changed shape - an entry added or removed.
void SettingsRebuild();
// Asks a Keys row for a new chord, as if Enter had been pressed on it.
void SettingsCaptureRow(const std::wstring& rowId);
// A short message along the foot of the window.
void SettingsToast(const std::wstring& text);

// ---------------------------------------------------------------- pages
struct PageDef {
    const wchar_t* caption;                         // the category's name
    const wchar_t* blurb;                           // said when the category has focus
    void (*build)(std::vector<ui::Row>& rows);
    void (*reset)();                                // RESET TO DEFAULTS, this page only
    // Drawn at the top of the description panel, `height` tall at 96 dpi.
    void (*preview)(HDC dc, const RECT& area) = nullptr;
    int  previewHeight = 0;
    // Something on this page wants attention (a blocked shortcut): shown as a
    // mark beside the category.
    int  (*issues)() = nullptr;
};

void BuildLayoutPage(std::vector<ui::Row>& rows);
void ResetLayoutPage();
void PreviewLayout(HDC dc, const RECT& area);

void BuildBehaviourPage(std::vector<ui::Row>& rows);
void ResetBehaviourPage();

void BuildShortcutsPage(std::vector<ui::Row>& rows);
void ResetShortcutsPage();
int  ShortcutIssues();

void BuildAppsPage(std::vector<ui::Row>& rows);
void ResetAppsPage();

void BuildSearchPage(std::vector<ui::Row>& rows);
void ResetSearchPage();

void BuildMonitorPage(std::vector<ui::Row>& rows);
void ResetMonitorPage();
void PreviewMonitor(HDC dc, const RECT& area);

void BuildClockPage(std::vector<ui::Row>& rows);
void ResetClockPage();
void PreviewClock(HDC dc, const RECT& area);

void BuildExplorerPage(std::vector<ui::Row>& rows);
void ResetExplorerPage();

void BuildGeneralPage(std::vector<ui::Row>& rows);
void ResetGeneralPage();

void BuildWelcomePage(std::vector<ui::Row>& rows);
// The description the Shortcuts page gives an action; the Welcome page reuses it.
const wchar_t* ActionHelp(Action a);

// ---------------------------------------------------------------- shared bits
struct ModChoice { const wchar_t* label; UINT mask; };
extern const ModChoice kModChoices[6];

// "C:\...\Brave Browser.lnk" -> "Brave Browser"
std::wstring FriendlyCommandName(const std::wstring& command);

// A file-open dialog filtered to programs: just the file name, or the full path.
bool BrowseForExeName(HWND parent, std::wstring* exeName);
bool BrowseForExePath(HWND parent, std::wstring* fullPath);
// The shell's folder picker.
bool BrowseForFolder(HWND parent, std::wstring* folder);
// The system colour picker, seeded with `color` and writing back into it.
bool PickColor(HWND parent, COLORREF* color);

// A row that picks a colour from a short palette, or any colour with Enter.
// With `themeColour` set (not CLR_INVALID), the first choice is "Theme" and
// stores `themeValue`: the colour follows whatever theme is chosen.
ui::Row ColourRow(const std::wstring& id, const std::wstring& label, const std::wstring& help,
                  COLORREF* field, const COLORREF* saved,
                  COLORREF themeColour = CLR_INVALID, COLORREF themeValue = 0);

// The name of the modifier as the user will press it: "Alt", "Win".
const wchar_t* ModifierName(UINT modMask);

// Whether a chord can be given to binding `selfIndex` (-1 for a new one):
// true when nothing else has it, or when the user agreed to take it from what
// does - in which case `clashes` lists the bindings that have to give it up.
bool ConfirmChordFree(UINT mods, UINT vk, int selfIndex, std::vector<int>* clashes);

} // namespace awa
