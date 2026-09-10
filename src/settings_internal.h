// Shared plumbing between the settings shell and its tab pages.
// Each page follows the same contract: Load() pulls the live Config into the
// controls, Save() pushes the controls back into it.
#pragma once
#include "common.h"
#include "config.h"
#include "app.h"
#include "resource.h"
#include <commctrl.h>

namespace awa {

// ---------------------------------------------------------------- shared bits
struct ModChoice { const wchar_t* label; UINT mask; };
extern const ModChoice kModChoices[6];

void SetCheck(HWND dlg, int id, bool on);
bool GetCheck(HWND dlg, int id);
std::wstring GetText(HWND wnd);
std::wstring GetItemText(HWND dlg, int id);
void CenterOn(HWND wnd, HWND parent);

// A file-open dialog filtered to programs; returns just the file name.
bool BrowseForExeName(HWND parent, std::wstring* exeName);

// The system colour picker, seeded with `color` and writing back into it.
bool PickColor(HWND parent, COLORREF* color);
// Same, but yields the full path (for launch commands).
bool BrowseForExePath(HWND parent, std::wstring* fullPath);

// A searchable "pick one of these" dialog, shared by the exclusion list and the
// app-launcher list.
struct PickEntry {
    std::wstring label;    // shown first, and searched
    std::wstring detail;   // shown after it, also searched
    std::wstring value;    // what the caller gets back
};
bool PickFromList(HWND parent, const wchar_t* caption, const wchar_t* prompt,
                  const std::vector<PickEntry>& entries, std::wstring* chosen);

// "C:\...\Brave Browser.lnk" -> "Brave Browser"
std::wstring FriendlyCommandName(const std::wstring& command);

// ---------------------------------------------------------------- pages
INT_PTR CALLBACK PageLayoutProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK PageBehaviourProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK PageShortcutsProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK PageMonitorProc(HWND, UINT, WPARAM, LPARAM);

void PageLayoutLoad(HWND page);
void PageLayoutSave(HWND page, bool* layoutChanged);

void PageBehaviourLoad(HWND page);
void PageBehaviourSave(HWND page);

INT_PTR CALLBACK PageAppsProc(HWND, UINT, WPARAM, LPARAM);

void PageShortcutsLoad(HWND page);
void PageShortcutsSave(HWND page);

void PageAppsLoad(HWND page);
void PageAppsSave(HWND page);

void PageMonitorLoad(HWND page);
void PageMonitorSave(HWND page);

INT_PTR CALLBACK PageSearchProc(HWND, UINT, WPARAM, LPARAM);
void PageSearchLoad(HWND page);
void PageSearchSave(HWND page);

INT_PTR CALLBACK PageGeneralProc(HWND, UINT, WPARAM, LPARAM);
void PageGeneralLoad(HWND page);
void PageGeneralSave(HWND page);

} // namespace awa
