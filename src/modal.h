// ProWindows - modal screens in the settings window's own look.
//
// The game never shows a system message box; it puts a panel over the screen
// with a question, two buttons and the prompts for them. These are that: a
// question, a notice, and a searchable "pick one" list, each a window of its
// own drawn with theme.h, run modally over their owner. The owner is dimmed
// while one is up (ModalOpen()).
#pragma once
#include "common.h"

namespace awa {
namespace ui {

// True for yes. `danger` draws the confirming button in red.
bool Confirm(HWND owner, const std::wstring& title, const std::wstring& body,
             const std::wstring& yes = L"Yes", const std::wstring& no = L"Cancel",
             bool danger = false);

// Three ways out: 1 for `yes`, 0 for `no`, -1 for Esc (stay where you were).
int  Ask(HWND owner, const std::wstring& title, const std::wstring& body,
         const std::wstring& yes, const std::wstring& no);

void Notice(HWND owner, const std::wstring& title, const std::wstring& body);

struct PickEntry {
    std::wstring label;    // shown first, and searched
    std::wstring detail;   // shown under it, also searched
    std::wstring value;    // what the caller gets back
};
// Type to narrow, arrows to choose, Enter to take it.
bool Pick(HWND owner, const std::wstring& title, const std::wstring& prompt,
          const std::vector<PickEntry>& entries, std::wstring* chosen,
          const std::wstring& verb = L"Add");

// Whether one of these is up, so the owner can dim itself behind it.
bool ModalOpen();

} // namespace ui
} // namespace awa
