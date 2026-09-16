#include "winutil.h"
#include <shobjidl_core.h>
#include <shlobj.h>

// Declared here rather than left to whoever links this file: the layout test
// harness builds winutil.cpp without main.cpp, and DpiForWindow's fallback
// needs GetDeviceCaps.
#pragma comment(lib, "gdi32.lib")

namespace awa {

// ---------------------------------------------------------------- queries
std::wstring WindowTitle(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return L"";
    std::wstring s;
    s.resize((size_t)len + 1);
    int got = GetWindowTextW(h, &s[0], len + 1);
    s.resize((size_t)(got > 0 ? got : 0));
    return s;
}

std::wstring WindowClass(HWND h) {
    wchar_t buf[256] = {};
    int n = GetClassNameW(h, buf, 256);
    return std::wstring(buf, (size_t)(n > 0 ? n : 0));
}

// Answers about a process that cost a handle open and a token read, cached for
// a couple of seconds against the process id that produced them.
//
// Classify() runs on every window event in the session and asks for the image
// name and the integrity level of the owning process each time. Both are
// several kernel round trips, both give the same answer for every window of the
// same application, and neither changes over the life of a process. Opening
// three Explorer windows used to pay for six of these; now it pays for two.
//
// Entries time out rather than living forever because process ids are recycled.
// Two seconds is far longer than a burst of window events and far shorter than
// any plausible reuse.
namespace {

struct ProcFacts {
    std::wstring name;
    DWORD        integrity     = 0;
    bool         haveName      = false;
    bool         haveIntegrity = false;
    ULONGLONG    stamp         = 0;
};

constexpr ULONGLONG kProcCacheMs = 2000;

ProcFacts* ProcEntry(DWORD pid) {
    static std::unordered_map<DWORD, ProcFacts> cache;
    const ULONGLONG now = GetTickCount64();

    auto it = cache.find(pid);
    if (it != cache.end()) {
        if (now - it->second.stamp < kProcCacheMs) return &it->second;
        cache.erase(it);
    }

    // Swept on insert rather than on a timer: the map only grows when a window
    // event arrives, so that is exactly when it is worth tidying.
    if (cache.size() > 128) {
        for (auto e = cache.begin(); e != cache.end(); ) {
            if (now - e->second.stamp >= kProcCacheMs) e = cache.erase(e);
            else                                       ++e;
        }
    }

    ProcFacts fresh;
    fresh.stamp = now;
    return &cache.emplace(pid, std::move(fresh)).first->second;
}

} // namespace

std::wstring ProcessName(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return L"";

    ProcFacts* facts = ProcEntry(pid);
    if (facts->haveName) return facts->name;

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"";

    wchar_t path[MAX_PATH * 2] = {};
    DWORD size = (DWORD)(sizeof(path) / sizeof(path[0]));
    std::wstring& name = facts->name;
    facts->haveName = true;          // a failed lookup is worth caching too
    if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
        std::wstring full(path, size);
        size_t slash = full.find_last_of(L"\\/");
        name = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
    }
    CloseHandle(proc);
    return name;
}

bool IsCloaked(HWND h) {
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, AWA_DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return false;
}

bool IsMinimizedWnd(HWND h) { return IsIconic(h) != FALSE; }
bool IsMaximizedWnd(HWND h) { return IsZoomed(h) != FALSE; }

bool IsResizable(HWND h) {
    LONG style = GetWindowLongW(h, GWL_STYLE);
    return (style & WS_THICKFRAME) != 0;
}

bool IsOnCurrentVirtualDesktop(HWND h) {
    static IVirtualDesktopManager* vdm = nullptr;
    // When to ask for the object again after a failure. It used to be asked
    // for exactly once, and at logon - which is when this program normally
    // starts - the shell that serves it is often not up yet. One refusal then
    // switched virtual-desktop awareness off for the whole session: every
    // window on every desktop was tiled onto the one being looked at.
    static ULONGLONG retryAt = 0;
    if (!vdm) {
        const ULONGLONG now = GetTickCount64();
        if (now < retryAt) return true;
        if (FAILED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&vdm)))) {
            vdm = nullptr;
            retryAt = now + 5000;
        }
    }
    if (!vdm) return true;   // no API available (yet): assume yes

    BOOL onCurrent = TRUE;
    if (FAILED(vdm->IsWindowOnCurrentVirtualDesktop(h, &onCurrent))) return true;
    return onCurrent != FALSE;
}

Rect VisibleRect(HWND h) {
    RECT r{};
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        return Rect::FromRECT(r);
    if (GetWindowRect(h, &r)) return Rect::FromRECT(r);
    return Rect();
}

// ---------------------------------------------------------------- elevation
bool SelfIsElevated() {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 0;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elev{};
        DWORD size = sizeof(elev);
        if (GetTokenInformation(token, TokenElevation, &elev, size, &size))
            cached = elev.TokenIsElevated ? 1 : 0;
        CloseHandle(token);
    }
    return cached != 0;
}

// Mandatory integrity level of a token, or 0 when it cannot be read.
static DWORD IntegrityOfToken(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    if (!size) return 0;
    std::vector<BYTE> buf(size);
    if (!GetTokenInformation(token, TokenIntegrityLevel, buf.data(), size, &size)) return 0;
    auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
    if (!label->Label.Sid) return 0;
    const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (count == 0) return 0;
    return *GetSidSubAuthority(label->Label.Sid, count - 1);
}

static DWORD OwnIntegrity() {
    static DWORD cached = (DWORD)-1;
    if (cached != (DWORD)-1) return cached;
    cached = SECURITY_MANDATORY_MEDIUM_RID;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (DWORD il = IntegrityOfToken(token)) cached = il;
        CloseHandle(token);
    }
    return cached;
}

// Naming this "elevated" is a simplification kept from the original API. What
// it really answers is "would UIPI refuse to let us move this window", which is
// a question about integrity levels.
//
// The previous implementation asked whether OpenProcess(QUERY_LIMITED) failed.
// It nearly always SUCCEEDS against an elevated process of the same user - that
// access right is deliberately grantable across integrity levels - so elevated
// windows were reported as ordinary ones, got tiled, silently refused every
// SetWindowPos, and left a hole in the layout. Compare the levels instead.
DWORD ProcessIntegrity(DWORD pid) {
    if (!pid) return 0;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return 0;
    DWORD level = 0;
    HANDLE token = nullptr;
    if (OpenProcessToken(proc, TOKEN_QUERY, &token)) {
        level = IntegrityOfToken(token);
        CloseHandle(token);
    }
    CloseHandle(proc);
    return level;
}

DWORD OwnProcessIntegrity() { return OwnIntegrity(); }

bool ProcessOutOfReach(DWORD pid) {
    if (!pid) return false;

    // Same reasoning as ProcessName: asked once per window event, and the
    // answer cannot change while the process lives.
    ProcFacts* facts = ProcEntry(pid);
    if (facts->haveIntegrity) return facts->integrity > OwnIntegrity();

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return true;               // cannot even look: assume out of reach

    bool higher = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(proc, TOKEN_QUERY, &token)) {
        const DWORD theirs = IntegrityOfToken(token);
        if (theirs) {
            higher = theirs > OwnIntegrity();
            facts->integrity     = theirs;
            facts->haveIntegrity = true;
        } else {
            TOKEN_ELEVATION elev{};
            DWORD size = sizeof(elev);
            if (GetTokenInformation(token, TokenElevation, &elev, size, &size))
                higher = elev.TokenIsElevated != 0;
        }
        CloseHandle(token);
    } else {
        higher = true;                    // token unreadable: out of reach
    }
    CloseHandle(proc);

    // Cache the verdict even when the level itself could not be read, by
    // recording a level that reproduces it.
    if (!facts->haveIntegrity) {
        facts->haveIntegrity = true;
        facts->integrity = higher ? OwnIntegrity() + 1 : OwnIntegrity();
    }
    return higher;
}

bool IsElevatedProcess(HWND h) {
    if (SelfIsElevated()) return false;   // we can touch anything anyway
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return ProcessOutOfReach(pid);
}

bool RestartElevated() {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;

    SHELLEXECUTEINFOW info{ sizeof(SHELLEXECUTEINFOW) };
    info.lpVerb = L"runas";               // the UAC prompt
    info.lpFile = path;
    info.nShow  = SW_SHOWNORMAL;
    info.fMask  = SEE_MASK_NOASYNC;
    return ShellExecuteExW(&info) != FALSE;
}

// ---------------------------------------------------------------- fullscreen
bool IsFullscreenWindow(HWND h) {
    if (!h || !IsWindow(h) || !IsWindowVisible(h)) return false;

    // A maximised window is not fullscreen: it stops at the work area and the
    // taskbar is still there. Games never go through the maximise path at all.
    if (IsZoomed(h)) return false;

    // A titled, resizable window that happens to have been dragged over the
    // whole screen is still an ordinary window and must keep being tiled.
    const LONG style = GetWindowLongW(h, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false;

    MONITORINFO mi{ sizeof(MONITORINFO) };
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return false;

    RECT r{};
    if (!GetWindowRect(h, &r)) return false;

    // Covers the monitor, taskbar included. Two pixels of slack: some engines
    // are a hair off, and some report the client area rather than the frame.
    const int slack = 2;
    return r.left   <= mi.rcMonitor.left   + slack &&
           r.top    <= mi.rcMonitor.top    + slack &&
           r.right  >= mi.rcMonitor.right  - slack &&
           r.bottom >= mi.rcMonitor.bottom - slack;
}

bool FullscreenAppActive() {
    // The shell already tracks this for its own notification suppression, and
    // its answer covers exclusive-fullscreen Direct3D, which no amount of
    // rectangle measuring can detect from outside the process.
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        // Only the two that genuinely mean "the screen is not ours". QUNS_BUSY
        // is deliberately not one of them: the shell returns it for any
        // full-screen window at all - a full-screen video, a slideshow, the
        // magnifier - and transiently around lock and logon, so trusting it
        // paused the tiler and tore the overlay down several times an hour for
        // no reason the user could see. Anything QUNS_BUSY would have caught
        // that matters is caught by measuring the foreground window below.
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN ||
            state == QUNS_PRESENTATION_MODE)
            return true;
    }

    // Borderless-windowed games and fullscreen video leave the shell state
    // alone, so measure the foreground window as well.
    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false;

    const std::wstring cls = WindowClass(fg);
    if (cls == L"Progman" || cls == L"WorkerW" ||
        cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd")
        return false;

    // A Store app is hosted in an ApplicationFrameWindow, and the frame is
    // what takes the foreground. A fullscreen UWP game is one of those, so the
    // frame cannot simply be excused - but an empty frame (the app is still
    // starting, or suspended) has no CoreWindow inside it and is never a game.
    if (cls == L"ApplicationFrameWindow") {
        HWND core = FindWindowExW(fg, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
        return core != nullptr && IsFullscreenWindow(fg);
    }

    // The shell's own full-screen surfaces are not games, and Windows 11 has a
    // lot of them: the Alt+Tab switcher and Task View cover the whole screen
    // with a translucent backdrop, so does the lock screen, the snipping
    // overlay (Win+Shift+S), and the logon screen while an autostarted copy
    // is coming up behind it. Every one of them took the foreground, measured
    // as full-screen, and put the tiler into game mode - which tore the
    // overlay down and back up on every Alt+Tab, dropped every window event
    // for the next second and a half, and at logon left nothing arranged
    // until the user unlocked. The log on this machine showed it several
    // times an hour. Known by process, because their classes change between
    // builds and the process names do not.
    static const wchar_t* const kShellProcesses[] = {
        L"explorer.exe", L"lockapp.exe", L"logonui.exe",
        L"shellexperiencehost.exe", L"startmenuexperiencehost.exe",
        L"searchhost.exe", L"searchapp.exe", L"searchui.exe",
        L"screenclippinghost.exe", L"snippingtool.exe",
        L"textinputhost.exe", L"widgets.exe", L"dwm.exe",
    };
    const std::wstring proc = ToLower(ProcessName(fg));
    for (const wchar_t* shell : kShellProcesses)
        if (proc == shell) return false;

    return IsFullscreenWindow(fg);
}

// ---------------------------------------------------------------- elevated autostart
namespace {

const wchar_t kTaskName[] = L"ProWindows Elevated Autostart";

// Runs a console tool with no window and waits for it. Returns its exit code,
// or -1 if it could not be started.
int RunToCompletion(std::wstring commandLine) {
    STARTUPINFOW si{ sizeof(STARTUPINFOW) };
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    commandLine.push_back(L'\0');           // CreateProcessW may write to it
    if (!CreateProcessW(nullptr, &commandLine[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;

    // schtasks either answers immediately or is wedged; it must never hold the
    // UI thread indefinitely.
    DWORD code = (DWORD)-1;
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

std::wstring XmlEscape(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (wchar_t c : in) {
        switch (c) {
            case L'&':  out += L"&amp;";  break;
            case L'<':  out += L"&lt;";   break;
            case L'>':  out += L"&gt;";   break;
            case L'"':  out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            default:    out += c;         break;
        }
    }
    return out;
}

std::wstring CurrentUserSam() {
    wchar_t domain[256] = {}, user[256] = {};
    if (!GetEnvironmentVariableW(L"USERDOMAIN", domain, ARRAYSIZE(domain))) domain[0] = 0;
    if (!GetEnvironmentVariableW(L"USERNAME", user, ARRAYSIZE(user)))       user[0]   = 0;
    if (!user[0]) return L"";
    if (!domain[0]) return user;
    return std::wstring(domain) + L"\\" + user;
}

} // namespace

// Asking Windows costs a schtasks process, about a third of a second. The tray
// menu and the Behaviour page both want the answer every time they are built,
// and a third of a second between right-clicking the tray icon and the menu
// appearing is very obvious. Since the only things that change it are the two
// functions below, the answer is worked out once and then kept. Someone who
// deletes the task by hand in Task Scheduler sees the stale state until the
// next restart, which is a fair trade for a menu that opens instantly.
namespace { int g_taskState = -1; }   // -1 unknown, 0 absent, 1 present

bool ElevatedAutostartInstalled() {
    if (g_taskState >= 0) return g_taskState != 0;

    std::wstring cmd = L"schtasks.exe /Query /TN \"";
    cmd += kTaskName;
    cmd += L"\"";
    g_taskState = (RunToCompletion(cmd) == 0) ? 1 : 0;
    return g_taskState != 0;
}

bool InstallElevatedAutostart() {
    // Registering a task that runs at the highest available level is itself a
    // privileged operation. Refusing here, rather than letting schtasks fail
    // with an opaque code, keeps the message the user sees honest.
    if (!SelfIsElevated()) return false;

    const std::wstring user = CurrentUserSam();
    if (user.empty()) return false;

    wchar_t tempDir[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempDir)) return false;
    const std::wstring xmlPath = std::wstring(tempDir) + L"ProWindows_task.xml";

    const std::wstring exe = XmlEscape(ExePath());
    const std::wstring who = XmlEscape(user);

    const std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" "
        L"xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo>\r\n"
        L"    <Description>Starts ProWindows at logon with the rights it needs to "
        L"arrange windows that run as administrator.</Description>\r\n"
        L"  </RegistrationInfo>\r\n"
        L"  <Triggers>\r\n"
        L"    <LogonTrigger><Enabled>true</Enabled><UserId>" + who +
        L"</UserId></LogonTrigger>\r\n"
        L"  </Triggers>\r\n"
        L"  <Principals>\r\n"
        L"    <Principal id=\"Author\">\r\n"
        L"      <UserId>" + who + L"</UserId>\r\n"
        L"      <LogonType>InteractiveToken</LogonType>\r\n"
        L"      <RunLevel>HighestAvailable</RunLevel>\r\n"
        L"    </Principal>\r\n"
        L"  </Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
        L"    <StartWhenAvailable>false</StartWhenAvailable>\r\n"
        L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n"
        L"    <IdleSettings>\r\n"
        L"      <StopOnIdleEnd>false</StopOnIdleEnd>\r\n"
        L"      <RestartOnIdle>false</RestartOnIdle>\r\n"
        L"    </IdleSettings>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Enabled>true</Enabled>\r\n"
        L"    <Hidden>false</Hidden>\r\n"
        L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
        L"    <WakeToRun>false</WakeToRun>\r\n"
        // A window manager runs for the whole session; the default three-day
        // execution limit would silently kill it.
        L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
        L"    <Priority>6</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\">\r\n"
        L"    <Exec>\r\n"
        L"      <Command>" + exe + L"</Command>\r\n"
        L"      <Arguments>--tray</Arguments>\r\n"
        L"    </Exec>\r\n"
        L"  </Actions>\r\n"
        L"</Task>\r\n";

    // schtasks only accepts UTF-16 with a byte order mark.
    HANDLE f = CreateFileW(xmlPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const wchar_t bom = 0xFEFF;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(f, &bom, sizeof(bom), &wrote, nullptr) &&
                    WriteFile(f, xml.data(), (DWORD)(xml.size() * sizeof(wchar_t)),
                              &wrote, nullptr);
    CloseHandle(f);
    if (!ok) { DeleteFileW(xmlPath.c_str()); return false; }

    std::wstring cmd = L"schtasks.exe /Create /F /TN \"";
    cmd += kTaskName;
    cmd += L"\" /XML \"" + xmlPath + L"\"";
    const int rc = RunToCompletion(cmd);
    DeleteFileW(xmlPath.c_str());

    if (rc != 0) AWA_LOG(L"schtasks /Create failed (%d)", rc);
    g_taskState = (rc == 0) ? 1 : 0;
    return rc == 0;
}

bool RemoveElevatedAutostart() {
    std::wstring cmd = L"schtasks.exe /Delete /F /TN \"";
    cmd += kTaskName;
    cmd += L"\"";
    const bool gone = RunToCompletion(cmd) == 0;
    if (gone) g_taskState = 0;
    return gone;
}

// ---------------------------------------------------------------- size limits
SizeLimits QuerySizeLimits(HWND h) {
    SizeLimits lim;
    if (!IsWindow(h)) return lim;

    // Seed with what the system would have supplied before sending the message,
    // so a window that does not handle it leaves sane numbers behind rather
    // than zeroes.
    MINMAXINFO mmi{};
    mmi.ptMinTrackSize.x = GetSystemMetrics(SM_CXMINTRACK);
    mmi.ptMinTrackSize.y = GetSystemMetrics(SM_CYMINTRACK);
    mmi.ptMaxTrackSize.x = GetSystemMetrics(SM_CXMAXTRACK);
    mmi.ptMaxTrackSize.y = GetSystemMetrics(SM_CYMAXTRACK);

    DWORD_PTR result = 0;
    // SMTO_ABORTIFHUNG, and a short timeout: this runs on the thread that draws
    // everything, and a window that is not pumping messages must cost us 60 ms
    // once, not a freeze.
    if (!SendMessageTimeoutW(h, WM_GETMINMAXINFO, 0, (LPARAM)&mmi,
                             SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 60, &result))
        return lim;

    // Track sizes are GetWindowRect sizes. We place by the visible DWM frame,
    // so the invisible resize border has to come off both numbers or every
    // limit is several pixels too generous.
    const FramePad pad = WindowFramePad(h);
    const int padW = pad.l + pad.r;
    const int padH = pad.t + pad.b;

    lim.minW = (std::max)(0, (int)mmi.ptMinTrackSize.x - padW);
    lim.minH = (std::max)(0, (int)mmi.ptMinTrackSize.y - padH);

    const int maxW = (int)mmi.ptMaxTrackSize.x - padW;
    const int maxH = (int)mmi.ptMaxTrackSize.y - padH;
    lim.maxW = (maxW > 0 && maxW < kNoLimit) ? maxW : kNoLimit;
    lim.maxH = (maxH > 0 && maxH < kNoLimit) ? maxH : kNoLimit;

    // A window whose maximum is below its minimum tells us nothing usable.
    if (lim.maxW < lim.minW) lim.maxW = kNoLimit;
    if (lim.maxH < lim.minH) lim.maxH = kNoLimit;
    return lim;
}

// ---------------------------------------------------------------- classification
static bool MatchesAny(const std::vector<std::wstring>& list, const std::wstring& value) {
    if (value.empty()) return false;
    for (const auto& e : list)
        if (IEquals(e, value)) return true;
    return false;
}

static bool ContainsAny(const std::vector<std::wstring>& list, const std::wstring& value) {
    for (const auto& e : list)
        if (IContains(value, e)) return true;
    return false;
}

ManageVerdict Classify(HWND h, const Config& cfg, IgnoreReason* why) {
    // Written out at every rejection rather than set once at the top, so a new
    // rule cannot be added without deciding which kind of "no" it is.
    const auto ignore = [&](IgnoreReason reason) {
        if (why) *why = reason;
        return ManageVerdict::Ignore;
    };
    if (why) *why = IgnoreReason::None;

    if (!h || !IsWindow(h))                    return ignore(IgnoreReason::Permanent);
    // Asked before visibility, deliberately. A child window is never going to
    // become a top-level one, and EVENT_OBJECT_SHOW arrives for every child
    // an application shows - a control, a tooltip, a tab strip - usually
    // while its parent is still hidden. Testing visibility first put every one
    // of those on the pending list for two seconds of second looks, which on a
    // busy desktop kept TIMER_PENDING ticking for nothing.
    if (GetAncestor(h, GA_ROOT) != h)          return ignore(IgnoreReason::Permanent);
    // A window that has been created but not shown yet. Ordinary during
    // startup of almost any application.
    if (!IsWindowVisible(h))                   return ignore(IgnoreReason::Transient);

    // Never manage our own settings window.
    DWORD ownPid = 0;
    GetWindowThreadProcessId(h, &ownPid);
    if (ownPid == GetCurrentProcessId())       return ignore(IgnoreReason::Permanent);

    const LONG style = GetWindowLongW(h, GWL_STYLE);
    const LONG ex    = GetWindowLongW(h, GWL_EXSTYLE);

    if (style & WS_CHILD)                      return ignore(IgnoreReason::Permanent);
    if (ex & WS_EX_TOOLWINDOW)                 return ignore(IgnoreReason::Permanent);
    if (ex & WS_EX_NOACTIVATE)                 return ignore(IgnoreReason::Permanent);
    // DWM cloaks a window while it opens, and for the whole time it sits on
    // another virtual desktop. Both end.
    if (IsCloaked(h))                          return ignore(IgnoreReason::Transient);

    const std::wstring cls = WindowClass(h);
    if (MatchesAny(BuiltinIgnoreClass(), cls)) return ignore(IgnoreReason::Permanent);
    if (MatchesAny(cfg.ignoreClass, cls))      return ignore(IgnoreReason::Permanent);

    const std::wstring title = WindowTitle(h);
    // The single most common reason a real window is turned away: Electron,
    // Chrome, Qt and JetBrains applications all create the window first and
    // set its title on a later turn of their message pump.
    if (title.empty())                         return ignore(IgnoreReason::Transient);
    // A title the user asked to ignore. Titles change, so this is not final -
    // but EVENT_OBJECT_NAMECHANGE re-examines the window when it does, which
    // is cheaper than keeping it on a retry list.
    if (ContainsAny(cfg.ignoreTitle, title))   return ignore(IgnoreReason::Permanent);

    const std::wstring proc = ProcessName(h);
    if (MatchesAny(BuiltinIgnoreProcess(), proc)) return ignore(IgnoreReason::Permanent);
    if (MatchesAny(cfg.ignoreProcess, proc))   return ignore(IgnoreReason::Permanent);

    // Anything covering a whole monitor with no title bar is a game, a video
    // player or a presentation. Touching one of those is at best pointless and
    // at worst throws it out of exclusive fullscreen, so it is never managed -
    // not tiled, not floated, not even given a DWM border. Transient because a
    // window created borderless-maximised briefly looks exactly like one.
    if (IsFullscreenWindow(h))                 return ignore(IgnoreReason::Transient);

    // A window at a higher integrity level cannot be moved by us at all: UIPI
    // refuses the SetWindowPos and reports nothing. Reserving a tile for one
    // leaves a rectangle of bare desktop, so it takes no part in the layout -
    // but it is reported rather than ignored, so the user can be told why the
    // window they ran as administrator is sitting on top of everything.
    if (IsElevatedProcess(h))                  return ManageVerdict::Blocked;

    if (MatchesAny(BuiltinFloatClass(), cls))  return ManageVerdict::Float;
    if (MatchesAny(BuiltinFloatProcess(), proc)) return ManageVerdict::Float;
    if (ContainsAny(BuiltinFloatTitle(), title)) return ManageVerdict::Float;
    if (MatchesAny(cfg.floatProcess, proc))    return ManageVerdict::Float;
    if (MatchesAny(cfg.floatClass, cls))       return ManageVerdict::Float;
    if (ContainsAny(cfg.floatTitle, title))    return ManageVerdict::Float;

    // Dialogs and fixed-size windows behave badly when tiled.
    if (!IsResizable(h))                       return ManageVerdict::Float;
    if (GetWindow(h, GW_OWNER) != nullptr)     return ManageVerdict::Float;

    return ManageVerdict::Tile;
}

// ---------------------------------------------------------------- mutation
void PlaceWindow(HWND h, const Rect& r, HDWP* dwp) {
    if (!IsWindow(h) || r.empty()) return;

    if (IsMaximizedWnd(h)) {
        WINDOWPLACEMENT wp{ sizeof(WINDOWPLACEMENT) };
        if (GetWindowPlacement(h, &wp)) {
            wp.showCmd = SW_RESTORE;
            SetWindowPlacement(h, &wp);
        }
    }

    // Compensate for the invisible resize border so the *visible* edges land on r.
    RECT wr{};
    if (!GetWindowRect(h, &wr)) return;
    RECT fr = wr;
    DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr));

    const int padL = fr.left   - wr.left;
    const int padT = fr.top    - wr.top;
    const int padR = wr.right  - fr.right;
    const int padB = wr.bottom - fr.bottom;

    const int x = r.x - padL;
    const int y = r.y - padT;
    const int w = r.w + padL + padR;
    const int hgt = r.h + padT + padB;

    const UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS |
                       SWP_ASYNCWINDOWPOS;

    if (dwp && *dwp) {
        HDWP next = DeferWindowPos(*dwp, h, nullptr, x, y, w, hgt, flags);
        if (next) *dwp = next;
        else      SetWindowPos(h, nullptr, x, y, w, hgt, flags);
    } else {
        SetWindowPos(h, nullptr, x, y, w, hgt, flags);
    }
}

FramePad WindowFramePad(HWND h) {
    return MeasureWindow(h).pad;
}

WindowGeom MeasureWindow(HWND h) {
    WindowGeom geom;
    RECT wr{};
    if (!GetWindowRect(h, &wr)) return geom;

    // One question, one answer, two facts derived from it. The fallback when
    // DWM declines is the window rect itself, which is what VisibleRect has
    // always done and gives a zero pad - correct for a window with no frame.
    RECT fr = wr;
    DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr));

    geom.visible = Rect::FromRECT(fr);
    geom.pad.l   = fr.left   - wr.left;
    geom.pad.t   = fr.top    - wr.top;
    geom.pad.r   = wr.right  - fr.right;
    geom.pad.b   = wr.bottom - fr.bottom;
    geom.ok      = true;
    return geom;
}

void PlaceWindowFast(HWND h, const Rect& target, const FramePad& pad, HDWP* dwp) {
    if (!target.w || !target.h) return;

    const int x = target.x - pad.l;
    const int y = target.y - pad.t;
    const int w = target.w + pad.l + pad.r;
    const int ht = target.h + pad.t + pad.b;

    const UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS |
                       SWP_ASYNCWINDOWPOS;

    if (dwp && *dwp) {
        HDWP next = DeferWindowPos(*dwp, h, nullptr, x, y, w, ht, flags);
        if (next) *dwp = next;
        else      SetWindowPos(h, nullptr, x, y, w, ht, flags);
    } else {
        SetWindowPos(h, nullptr, x, y, w, ht, flags);
    }
}

// Brings a window to the front from a process that does not own the
// foreground, which SetForegroundWindow alone refuses.
//
// This used to do two things that are both well known to go wrong, and both
// were reported from the desktop:
//
//   AttachThreadInput to the foreground and target threads. Attached threads
//   pool their whole input state - keyboard state, capture, the caret and the
//   cursor, including whether it is shown. Windows hides the pointer while
//   the user types ("Hide pointer while typing" is on by default) and shows it
//   again on the next mouse movement; pool that state with ours in the middle
//   of a keystroke and split it again a moment later, and the hidden pointer
//   can be left hidden, or ours can. That is "the mouse disappears when I
//   type". It also makes SetFocus a synchronous call into the other process,
//   so a hung application hung the tiler with it.
//
//   A synthetic Alt press-and-release as a fallback. Alt on its own moves
//   keyboard focus to the menu bar in Explorer and every Office application,
//   and Alt arriving while Shift is held is the keyboard-layout switch chord
//   on most machines. Injected while somebody is typing, both were visible.
//
// What replaces them is a mouse input event with no flags set - no movement,
// no button, nothing for any application to see - which the input system
// still records as "this process delivered the last input event". That is the
// documented condition under which SetForegroundWindow is allowed, and it is
// the same approach komorebi and GlazeWM settled on for the same reasons.
// Nothing here calls into the other process synchronously, and nothing here
// shares state with it.
void FocusWindow(HWND h) {
    if (!h || !IsWindow(h)) return;

    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    if (GetForegroundWindow() == h) return;

    // A registered hotkey already grants the receiving thread foreground
    // rights, so this often succeeds outright. Try before injecting anything.
    if (SetForegroundWindow(h) && GetForegroundWindow() == h) return;

    INPUT in = {};
    in.type = INPUT_MOUSE;
    SendInput(1, &in, sizeof(INPUT));

    // HWND_TOP with no move or size: the same raise a real click would do,
    // so a window behind another one is brought forward as it is focused.
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    SetForegroundWindow(h);
}

void SetBorderColor(HWND h, COLORREF c, bool enabled) {
    COLORREF value = enabled ? c : 0xFFFFFFFF /* DWMWA_COLOR_DEFAULT */;
    DwmSetWindowAttribute(h, AWA_DWMWA_BORDER_COLOR, &value, sizeof(value));
}

void SetCornerPreference(HWND h, int pref) {
    if (pref <= 0) return;                 // 0 = leave the system default alone
    DWORD value = (pref == 1) ? 1u : 2u;   // 1 = DONOTROUND, 2 = ROUND
    DwmSetWindowAttribute(h, AWA_DWMWA_WINDOW_CORNER_PREF, &value, sizeof(value));
}

void CloseWindow(HWND h) {
    if (h && IsWindow(h)) PostMessageW(h, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------- launching
bool LaunchCommand(const std::wstring& commandLine) {
    std::wstring cmd = Trim(commandLine);
    if (cmd.empty()) return false;

    std::wstring file, params;

    if (cmd[0] == L'"') {
        const size_t close = cmd.find(L'"', 1);
        if (close == std::wstring::npos) return false;
        file   = cmd.substr(1, close - 1);
        params = Trim(cmd.substr(close + 1));
    } else {
        // Prefer treating the whole string as a program name; only split on the
        // first space when that fails, so unquoted paths with spaces still work
        // when they name a real file.
        wchar_t resolved[MAX_PATH];
        const bool whole =
            SearchPathW(nullptr, cmd.c_str(), L".exe", MAX_PATH, resolved, nullptr) > 0 ||
            GetFileAttributesW(cmd.c_str()) != INVALID_FILE_ATTRIBUTES;

        if (whole) {
            file = cmd;
        } else {
            const size_t space = cmd.find(L' ');
            file   = (space == std::wstring::npos) ? cmd : cmd.substr(0, space);
            params = (space == std::wstring::npos) ? L"" : Trim(cmd.substr(space + 1));
        }
    }

    SHELLEXECUTEINFOW info = {};
    info.cbSize       = sizeof(info);
    // Deliberately NOT SEE_MASK_NOASYNC. That flag makes ShellExecuteEx block
    // until the shell has finished, and this runs on the thread that owns the
    // tiler, the animation and - when a binding needs a chord the shell already
    // claims - the low-level keyboard hook. Launching a large application off a
    // mechanical disk held that thread for seconds, during which every
    // keystroke on the machine was queued behind us and Windows was free to
    // drop the hook for being slow. NOASYNC exists for callers that are about
    // to exit; this one is not.
    info.fMask        = SEE_MASK_FLAG_NO_UI;
    info.lpVerb       = L"open";
    info.lpFile       = file.c_str();
    info.lpParameters = params.empty() ? nullptr : params.c_str();
    info.nShow        = SW_SHOWNORMAL;

    if (ShellExecuteExW(&info)) return true;
    AWA_LOG(L"launch failed (%lu): %s", GetLastError(), cmd.c_str());
    return false;
}

// ---------------------------------------------------------------- taskbar
static const wchar_t kAdvancedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

int GetTaskbarAlignment() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAdvancedKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 1;
    DWORD value = 1, size = sizeof(value), type = 0;
    if (RegQueryValueExW(key, L"TaskbarAl", nullptr, &type, (LPBYTE)&value, &size)
            != ERROR_SUCCESS || type != REG_DWORD) {
        value = 1;
    }
    RegCloseKey(key);
    return value ? 1 : 0;
}

bool SetTaskbarAlignment(int align) {
    if (align != 0 && align != 1) return true;      // "leave it alone"
    if (GetTaskbarAlignment() == align) return true;

    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAdvancedKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    DWORD value = (DWORD)align;
    const bool ok = RegSetValueExW(key, L"TaskbarAl", 0, REG_DWORD,
                                   (const BYTE*)&value, sizeof(value)) == ERROR_SUCCESS;
    RegCloseKey(key);

    // Explorer watches this key, but nudging it makes the change immediate.
    if (ok) {
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                            (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 200, nullptr);
    }
    return ok;
}

// ---------------------------------------------------------------- app discovery
static std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    return buf;
}

std::wstring DefaultBrowserPath() {
    // The shell records the user's pick as a ProgId; that ProgId's open command
    // is the actual browser.
    const std::wstring progId = RegString(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\http\\UserChoice",
        L"ProgId");

    std::wstring command;
    if (!progId.empty())
        command = RegString(HKEY_CLASSES_ROOT, (progId + L"\\shell\\open\\command").c_str(),
                            nullptr);
    if (command.empty())
        command = RegString(HKEY_CLASSES_ROOT, L"http\\shell\\open\\command", nullptr);
    if (command.empty()) return L"";

    // Strip the arguments: the command looks like  "C:\...\brave.exe" -- "%1"
    std::wstring path = Trim(command);
    if (!path.empty() && path[0] == L'"') {
        const size_t close = path.find(L'"', 1);
        if (close == std::wstring::npos) return L"";
        path = path.substr(1, close - 1);
    } else {
        const size_t space = path.find(L' ');
        if (space != std::wstring::npos) path = path.substr(0, space);
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return L"";
    return path;
}

// ---------------------------------------------------------------- scan cancel
static LONG g_scansCancelled = 0;

void CancelLongScans()   { InterlockedExchange(&g_scansCancelled, 1); }
bool LongScansCancelled() {
    return InterlockedCompareExchange(&g_scansCancelled, 0, 0) != 0;
}

static void ScanShortcuts(const std::wstring& dir, int depth,
                          std::vector<InstalledApp>* out) {
    if (depth > 4 || out->size() > 500) return;
    if (LongScansCancelled()) return;

    WIN32_FIND_DATAW find = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        const std::wstring name = find.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring full = dir + L"\\" + name;

        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanShortcuts(full, depth + 1, out);
            continue;
        }
        if (name.size() < 5) continue;
        if (_wcsicmp(name.c_str() + name.size() - 4, L".lnk") != 0) continue;

        const std::wstring stem = name.substr(0, name.size() - 4);
        // Installers leave a lot of these around; they are not apps.
        if (IContains(stem, L"uninstall") || IContains(stem, L"readme") ||
            IContains(stem, L"help") || IContains(stem, L"website") ||
            IContains(stem, L"release notes")) continue;

        bool seen = false;
        for (const auto& a : *out)
            if (IEquals(a.name, stem)) { seen = true; break; }
        if (!seen) out->push_back({ stem, full });
    } while (FindNextFileW(h, &find));

    FindClose(h);
}

std::vector<InstalledApp> EnumStartMenuApps() {
    std::vector<InstalledApp> apps;

    const KNOWNFOLDERID* roots[] = { &FOLDERID_CommonPrograms, &FOLDERID_Programs };
    for (const KNOWNFOLDERID* id : roots) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &path)) && path) {
            ScanShortcuts(path, 0, &apps);
            CoTaskMemFree(path);
        }
    }

    std::sort(apps.begin(), apps.end(), [](const InstalledApp& a, const InstalledApp& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return apps;
}

// The shell's own list of launchable apps. Covers Store and packaged apps,
// which have no shortcut on disk to find.
static void ScanAppsFolder(std::vector<InstalledApp>* out) {
    IShellItem* folder = nullptr;
    if (FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr,
                                           IID_PPV_ARGS(&folder))) || !folder)
        return;

    IEnumShellItems* items = nullptr;
    if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems,
                                        IID_PPV_ARGS(&items))) && items) {
        IShellItem* item = nullptr;
        while (items->Next(1, &item, nullptr) == S_OK && item) {
            PWSTR display = nullptr;
            PWSTR parsing = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display)) &&
                SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &parsing)) &&
                display && parsing) {
                // ShellExecute opens this exactly as the Start menu would,
                // whether it is a packaged app or a plain exe.
                out->push_back({ display, std::wstring(L"shell:AppsFolder\\") + parsing });
            }
            CoTaskMemFree(display);
            CoTaskMemFree(parsing);
            item->Release();
            item = nullptr;
            if (LongScansCancelled()) break;
        }
        items->Release();
    }
    folder->Release();
}

std::vector<InstalledApp> EnumAllApps() {
    std::vector<InstalledApp> apps;
    ScanAppsFolder(&apps);

    // Shortcuts second: anything AppsFolder already listed wins, since its
    // display names are the ones the Start menu shows.
    std::vector<InstalledApp> shortcuts = EnumStartMenuApps();
    for (auto& s : shortcuts) {
        bool seen = false;
        for (const auto& a : apps)
            if (IEquals(a.name, s.name)) { seen = true; break; }
        if (!seen) apps.push_back(std::move(s));
    }

    std::sort(apps.begin(), apps.end(),
              [](const InstalledApp& a, const InstalledApp& b) {
                  return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
              });
    return apps;
}

// ---------------------------------------------------------------- dpi
UINT DpiForWindow(HWND h) {
    using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
    static GetDpiForWindowFn fn = [] {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow")
                      : nullptr;
    }();

    if (fn && h) {
        const UINT dpi = fn(h);
        if (dpi) return dpi;
    }
    // Pre-1607, or a handle the call would not accept. The screen DC reports
    // the system DPI, which on those releases is the only DPI there is.
    HDC dc = GetDC(h);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(h, dc);
    return dpi > 0 ? (UINT)dpi : 96u;
}

// ---------------------------------------------------------------- monitors
static BOOL CALLBACK MonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM data) {
    auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(data);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        MonitorInfo m;
        m.handle  = mon;
        m.work    = Rect::FromRECT(mi.rcWork);
        m.full    = Rect::FromRECT(mi.rcMonitor);
        m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        // The current mode of the device behind this monitor. A read of what
        // the driver already knows, not a mode query; and EnumMonitors runs
        // on display changes, not on window events.
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
            (dm.dmFields & DM_DISPLAYFREQUENCY) && dm.dmDisplayFrequency > 1)
            m.refreshHz = (int)dm.dmDisplayFrequency;
        out->push_back(m);
    }
    return TRUE;
}

std::vector<MonitorInfo> EnumMonitors() {
    std::vector<MonitorInfo> mons;
    EnumDisplayMonitors(nullptr, nullptr, MonitorProc, reinterpret_cast<LPARAM>(&mons));

    // Stable left-to-right, top-to-bottom ordering so "next monitor" is intuitive.
    std::sort(mons.begin(), mons.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.full.x != b.full.x) return a.full.x < b.full.x;
        return a.full.y < b.full.y;
    });
    return mons;
}

HMONITOR MonitorOf(HWND h) {
    return MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
}

} // namespace awa
