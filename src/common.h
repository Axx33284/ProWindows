// ProWindows - common definitions
#pragma once

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define OEMRESOURCE

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <shobjidl.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdio>
#include <cstdint>

namespace awa {

// ---------------------------------------------------------------- constants
constexpr wchar_t kAppName[]      = L"ProWindows";
constexpr wchar_t kAppShort[]     = L"ProWindows";
constexpr wchar_t kWndClass[]     = L"ProWindows_MsgWnd";
constexpr wchar_t kMutexName[]    = L"Local\\ProWindows_SingleInstance";
constexpr wchar_t kVersion[]      = L"1.5.0";

// Private window messages
enum : UINT {
    WM_AWA_TRAY      = WM_APP + 1,
    WM_AWA_EVENT     = WM_APP + 2,   // coalesced win-event notification
    WM_AWA_TASKBARCREATED = WM_APP + 3,
    WM_AWA_HOOKKEY   = WM_APP + 4,   // wParam = index into the hooked bind list
    WM_AWA_ANIMTICK  = WM_APP + 5,   // one animation frame is due
    // "Arrange as soon as this event has been dealt with." Posted instead of
    // arming the debounce timer when the desktop has been quiet, so opening a
    // window on an idle machine rearranges on the very next trip through the
    // message loop rather than 35 ms later. See WindowManager::RequestRetile.
    WM_AWA_RETILE    = WM_APP + 6,
    // The mod-drag mouse hook saw its chord. wParam: 1 move, 2 resize. The
    // hook does nothing else - see moddrag.h.
    WM_AWA_MODDRAG   = WM_APP + 7,
    // A control-channel command is waiting. wParam is the request token; see
    // ipc.h. Deliberately POSTED rather than sent: a sent message would be
    // dispatched while the manager is parked inside a cross-process call, and
    // running a command there is the reentrancy this codebase just stopped
    // doing. Posted means it runs from the main loop, between passes.
    WM_AWA_IPC       = WM_APP + 8,
    // The overlay's sampling thread has a fresh reading waiting. Posted to
    // the overlay window; see monitor.cpp. The reading itself travels under
    // a lock, not in the message, so a late one cannot outlive its data.
    WM_AWA_SAMPLED   = WM_APP + 9,
};

// Timers
enum : UINT_PTR {
    TIMER_RETILE   = 1,   // debounce retile
    TIMER_ANIM     = 2,   // animation stepper
    TIMER_MOUSE    = 3,   // focus-follows-mouse: one-shot armed by the cursor hook
    // Runs only while a fullscreen application is on screen. No window events
    // arrive from a game, so without this there would be nothing to notice
    // when it closes and the desktop should come back.
    TIMER_GAMECHK  = 4,
    // Ticks only while a tiled window is being dragged, to keep the drop
    // indicator under the pointer. Windows runs the drag in a modal loop
    // inside the other process, so no other message would arrive meanwhile.
    TIMER_DRAG     = 5,
    // Ticks only while some window has been seen but could not yet be
    // classified - it has no title yet, it is still cloaked, its styles are
    // not applied. Stops as soon as the last one is resolved or given up on.
    TIMER_PENDING  = 6,
    // One-shot: give memory back once the process has been idle for a while.
    // See TrimMemory in main.cpp.
    TIMER_TRIM     = 7,
    // Runs only while the display is believed to be off, and clears that
    // belief the moment the user touches anything. See SetDisplayOff.
    TIMER_DISPLAY  = 8,
};

// DWM attributes that are missing from the 19041 SDK headers.
enum : DWORD {
    AWA_DWMWA_CLOAKED                = 14,
    AWA_DWMWA_WINDOW_CORNER_PREF     = 33,
    AWA_DWMWA_BORDER_COLOR           = 34,
};

// ---------------------------------------------------------------- small utils
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    Rect() = default;
    Rect(int x_, int y_, int w_, int h_) : x(x_), y(y_), w(w_), h(h_) {}
    static Rect FromRECT(const RECT& r) {
        return Rect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    }
    RECT ToRECT() const { RECT r{ x, y, x + w, y + h }; return r; }
    int  right()  const { return x + w; }
    int  bottom() const { return y + h; }
    int  cx()     const { return x + w / 2; }
    int  cy()     const { return y + h / 2; }
    bool empty()  const { return w <= 0 || h <= 0; }
    bool contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    Rect shrink(int g) const { return Rect(x + g, y + g, w - 2 * g, h - 2 * g); }
    bool operator==(const Rect& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
    bool operator!=(const Rect& o) const { return !(*this == o); }
};

enum class Dir { Left, Right, Up, Down };

inline bool DirHorizontal(Dir d) { return d == Dir::Left || d == Dir::Right; }

// ---------------------------------------------------------------- string utils
std::wstring Trim(const std::wstring& s);
std::wstring ToLower(std::wstring s);
bool IEquals(const std::wstring& a, const std::wstring& b);
bool IContains(const std::wstring& hay, const std::wstring& needle);
std::vector<std::wstring> SplitList(const std::wstring& s, wchar_t sep = L',');

// ---------------------------------------------------------------- paths / log
// %APPDATA%\ProWindows (created on first use). Returned by reference: it is
// resolved once, thread-safely, and never changes for the life of the process.
const std::wstring& ConfigDir();

// Points the settings folder somewhere else. Must be called before anything
// asks for ConfigDir, which in practice means the first few lines of wWinMain -
// after that the answer is already cached and this does nothing.
//
// This exists so the tests can run against a throwaway folder instead of the
// real settings. %APPDATA% cannot be used for that: the folder is resolved with
// SHGetKnownFolderPath, which reads the user's profile rather than the
// environment, so overriding the variable looks like it works and does not.
// It is a reasonable thing to want anyway - a portable install on a stick, or
// two configurations side by side.
void SetConfigDirOverride(const std::wstring& dir);
std::wstring ConfigPath();         // ...\config.ini
std::wstring LogPath();            // ...\log.txt
std::wstring ExePath();

void LogEnable(bool on);
void LogLine(const wchar_t* fmt, ...);

#define AWA_LOG(...) ::awa::LogLine(__VA_ARGS__)

} // namespace awa
