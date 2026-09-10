// ProWindows - thin Win32 helpers
#pragma once
#include "common.h"
#include "config.h"

namespace awa {

// ---------------------------------------------------------------- queries
std::wstring WindowTitle(HWND h);
std::wstring WindowClass(HWND h);
std::wstring ProcessName(HWND h);      // "notepad.exe" (empty if unavailable)

bool IsCloaked(HWND h);
bool IsMinimizedWnd(HWND h);
bool IsMaximizedWnd(HWND h);
bool IsResizable(HWND h);
bool IsOnCurrentVirtualDesktop(HWND h);

// Real on-screen bounds (DWM extended frame), falling back to GetWindowRect.
Rect VisibleRect(HWND h);

// ---------------------------------------------------------------- size limits
// What a window will actually accept, expressed in *visible frame* pixels so it
// can be compared against a layout slot directly.
//
// This exists because a tiling manager that ignores a window's own limits
// produces exactly the two failures users report: Steam refuses to shrink below
// its minimum and spills over its neighbours, and a file-copy dialog refuses to
// grow and leaves most of its slot empty.
constexpr int kNoLimit = 1 << 24;

struct SizeLimits {
    int minW = 0;
    int minH = 0;
    int maxW = kNoLimit;
    int maxH = kNoLimit;

    bool constrained() const {
        return minW > 0 || minH > 0 || maxW < kNoLimit || maxH < kNoLimit;
    }
};

// Asks the window itself, via WM_GETMINMAXINFO. Seeded with the system defaults
// so a window that ignores the message still answers sensibly, and sent with a
// timeout because a hung application must never take our UI thread with it.
SizeLimits QuerySizeLimits(HWND h);

// ---------------------------------------------------------------- classification
enum class ManageVerdict {
    Ignore,
    Tile,
    Float,
    // A perfectly ordinary window that we are simply not permitted to move: it
    // belongs to a process at a higher integrity level, so UIPI drops every
    // SetWindowPos we send it without reporting an error. It is kept as its own
    // answer rather than folded into Ignore so the UI can tell the user why
    // their elevated editor is not being arranged, and offer the fix.
    Blocked,
};

// Why Classify returned Ignore. Most rejections are about the state the window
// happens to be in at this instant rather than about the window itself: it has
// not been given a title yet, DWM has it cloaked for its opening fade, its
// styles have not been applied. Applications create the HWND first and do all
// of that afterwards, so a manager that looks exactly once and never again
// leaves those windows untiled for the rest of their life. Telling the two
// kinds of "no" apart is what makes a second look affordable - only the
// transient ones are worth keeping on a retry list.
enum class IgnoreReason {
    None,        // not an Ignore verdict at all
    Permanent,   // never going to qualify: a child, a tooltip class, our own
    Transient,   // might qualify shortly: no title yet, cloaked, not yet shown
};

ManageVerdict Classify(HWND h, const Config& cfg, IgnoreReason* why = nullptr);

// ---------------------------------------------------------------- fullscreen
// True when `h` covers the whole of the monitor it is on (within a couple of
// pixels) and carries no resizable frame - the shape every game takes in
// "fullscreen" and "borderless windowed" alike.
bool IsFullscreenWindow(HWND h);

// True when a fullscreen application currently owns the foreground. Games are
// the case that matters: moving other windows, animating, or setting DWM
// attributes while one is presenting makes it stutter, and on some drivers
// kicks it out of exclusive fullscreen entirely. Cheap enough to call on every
// foreground change - it asks the shell first and only measures rectangles if
// the shell has nothing to say.
bool FullscreenAppActive();

// ---------------------------------------------------------------- autostart
// Starting elevated at logon. A shortcut in Run cannot do this: Windows starts
// Run entries at the user's normal integrity level, so the tiler comes up
// unable to touch anything that is itself elevated, and asking for elevation
// from Run would put a UAC prompt in front of the user at every logon.
//
// A scheduled task registered to "run with highest privileges" starts elevated
// at logon with no prompt at all, which is the only arrangement that both
// works and is not user-hostile. Creating it needs administrator rights once.
bool ElevatedAutostartInstalled();
bool InstallElevatedAutostart();     // needs elevation; false if refused
bool RemoveElevatedAutostart();      // needs elevation

// ---------------------------------------------------------------- mutation
// Position a window so that its *visible* frame lands exactly on `r`,
// compensating for Windows' invisible resize borders. When `dwp` is non-null the
// move is queued into that DeferWindowPos batch instead of applied immediately.
void PlaceWindow(HWND h, const Rect& r, HDWP* dwp);

// The gap between GetWindowRect and the DWM frame the user actually sees.
struct FramePad { int l = 0, t = 0, r = 0, b = 0; };

FramePad WindowFramePad(HWND h);

// Both facts about a window's geometry, from one pair of calls.
//
// `VisibleRect` and `WindowFramePad` each ask DWM for the extended frame
// bounds, and a retile pass wanted both for every window it moved - so a board
// of ten windows spent forty cross-process round trips per window event asking
// four questions that have one answer. This asks once. `ok` is false when the
// window has gone, which the callers have to check anyway.
struct WindowGeom {
    Rect     visible;
    FramePad pad;
    bool     ok = false;
};
WindowGeom MeasureWindow(HWND h);

// PlaceWindow without the per-call DWM round-trip. Used for animation frames,
// where the padding is measured once and then reused every tick.
void PlaceWindowFast(HWND h, const Rect& target, const FramePad& pad, HDWP* dwp);

void FocusWindow(HWND h);              // reliable SetForegroundWindow
void SetBorderColor(HWND h, COLORREF c, bool enabled);
void SetCornerPreference(HWND h, int pref);   // 0 default, 1 square, 2 round
void CloseWindow(HWND h);

// ---------------------------------------------------------------- dpi
// GetDpiForWindow is Windows 10 1607 and later. Calling it directly puts a
// hard import in the binary, so a machine older than that cannot even start the
// process - the loader fails before wWinMain with "entry point not found".
// Resolved once at runtime instead, with the pre-1607 answer (the system DPI,
// via the window's own DC) as the fallback.
UINT DpiForWindow(HWND h);

// px measured at 96 dpi, scaled to `dpi`.
inline int ScaleDpi(int px, UINT dpi) { return MulDiv(px, (int)dpi, 96); }

// ---------------------------------------------------------------- monitors
struct MonitorInfo {
    HMONITOR handle = nullptr;
    Rect     work;                     // work area (taskbar excluded)
    Rect     full;
    bool     primary = false;
};

std::vector<MonitorInfo> EnumMonitors();
HMONITOR MonitorOf(HWND h);

// ---------------------------------------------------------------- misc
// True when the window belongs to a process at a HIGHER integrity level than
// ours, which is the thing that actually matters: UIPI then refuses our
// SetWindowPos and the window can neither be moved nor resized. Elevation is
// the common cause, a service or a protected process is the other.
bool IsElevatedProcess(HWND h);
bool SelfIsElevated();

// The same question asked about a process id, so it can be checked against a
// process that has no window - which is the only way to exercise the
// higher-integrity branch on a machine where nothing is running elevated.
bool ProcessOutOfReach(DWORD pid);

// Raw mandatory integrity level of a process, or 0 if it cannot be read.
// SECURITY_MANDATORY_*_RID: 0x1000 low, 0x2000 medium, 0x3000 high (what "run
// as administrator" produces), 0x4000 system. Exposed so the reachability rule
// can be checked against real numbers rather than taken on trust.
DWORD ProcessIntegrity(DWORD pid);
DWORD OwnProcessIntegrity();

// Relaunches ourselves through the UAC prompt and asks the current instance to
// quit. Returns false if the user declined the prompt.
bool RestartElevated();

// Runs a command line the way the Run box would. Splits off arguments so that
// "notepad.exe C:\notes.txt" works as well as a bare program name.
bool LaunchCommand(const std::wstring& commandLine);

// Applies the Windows taskbar alignment (0 = left, 1 = centre). Anything else
// leaves it alone. Returns false if the setting could not be written.
bool SetTaskbarAlignment(int align);
int  GetTaskbarAlignment();            // 0 or 1

// Full path of the user's default browser, or empty if it can't be determined.
std::wstring DefaultBrowserPath();

// Apps with a Start menu entry, for the "pick an app" list. `launchPath` is the
// shortcut itself, which ShellExecute opens exactly as the Start menu would.
struct InstalledApp {
    std::wstring name;
    std::wstring launchPath;
};
std::vector<InstalledApp> EnumStartMenuApps();

// Everything the Start menu can launch: the shortcuts above plus the shell's
// AppsFolder, which is where Store and packaged apps live. Deduplicated by
// display name and sorted. Costs a few hundred milliseconds, so the launcher
// builds it once on a background thread rather than on the way to the screen.
std::vector<InstalledApp> EnumAllApps();

// Asks any scan currently running on a background thread to stop at its next
// natural break. Called at shutdown: without it a scan runs to completion no
// matter what, and the only options left are to wait for it or to abandon a
// live thread into ExitProcess, where it can be terminated while it holds the
// CRT heap lock. One-way - nothing un-cancels.
void CancelLongScans();
bool LongScansCancelled();

} // namespace awa
