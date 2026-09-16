#include "ipc.h"
#include <cstdio>

// Declared here rather than left to whoever links this in. ipc.cpp is built
// into two different executables and the second one has no other reason to
// know it needs user32.
#pragma comment(lib, "user32.lib")

namespace awa {

namespace {

// One pipe per logon session, so two users on the same machine each drive their
// own copy rather than fighting over one name.
std::wstring PipeName() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    wchar_t buf[128];
    swprintf_s(buf, L"\\\\.\\pipe\\ProWindows.control.%lu", (unsigned long)session);
    return buf;
}

// The command as it travels, and the answer coming back. Allocated by the pipe
// thread and handed to the UI thread by pointer; see Abandon() below for who
// frees it, which is the only subtle thing in this file.
struct Request {
    std::wstring command;
    std::wstring reply;
    HANDLE       done      = nullptr;
    LONG         abandoned = 0;   // the pipe thread gave up waiting
};

HWND       g_msgWnd  = nullptr;
IpcHandler g_handler = nullptr;
HANDLE     g_thread  = nullptr;
HANDLE     g_stop    = nullptr;
LONG       g_running = 0;

// UTF-8 on the wire. Windows is the only thing on either end, but a pipe that
// carries window titles carries every alphabet there is.
std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const char* p, int bytes) {
    if (bytes <= 0) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, p, bytes, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, bytes, &out[0], n);
    return out;
}

// How long the pipe thread will wait for the UI thread to answer. Long enough
// that a command arriving during a retile still gets served; short enough that
// a wedged UI thread does not hold a client forever.
constexpr DWORD kReplyTimeoutMs = 5000;

// Runs one command by handing it to the UI thread and waiting.
//
// On timeout the request is NOT freed here. The UI thread may still be about to
// write into it, and freeing memory another thread is holding is the exact
// failure this project has spent its time removing. The request is marked
// abandoned instead and the UI thread frees it when it finally gets there - so
// a timeout costs a few hundred bytes until then, and never a crash.
std::wstring RunOnUiThread(const std::wstring& command) {
    if (!g_msgWnd || !IsWindow(g_msgWnd)) return L"error: not running";

    Request* req = new (std::nothrow) Request();
    if (!req) return L"error: out of memory";
    req->command = command;
    req->done    = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!req->done) { delete req; return L"error: out of memory"; }

    if (!PostMessageW(g_msgWnd, WM_AWA_IPC, (WPARAM)req, 0)) {
        CloseHandle(req->done);
        delete req;
        return L"error: not running";
    }

    const DWORD hit = WaitForSingleObject(req->done, kReplyTimeoutMs);
    if (hit != WAIT_OBJECT_0) {
        InterlockedExchange(&req->abandoned, 1);
        AWA_LOG(L"ipc: '%s' timed out waiting for the UI thread", command.c_str());
        return L"error: timed out";
    }

    std::wstring reply = req->reply;
    CloseHandle(req->done);
    delete req;
    return reply;
}

void ServeOneClient(HANDLE pipe) {
    char buf[8192];
    DWORD got = 0;
    if (!ReadFile(pipe, buf, (DWORD)sizeof(buf), &got, nullptr) || got == 0) return;

    const std::wstring command = FromUtf8(buf, (int)got);
    const std::wstring reply   = RunOnUiThread(command);

    const std::string out = ToUtf8(reply);
    DWORD wrote = 0;
    WriteFile(pipe, out.data(), (DWORD)out.size(), &wrote, nullptr);
    FlushFileBuffers(pipe);
}

DWORD WINAPI ServerThread(LPVOID) {
    const std::wstring name = PipeName();

    while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
        HANDLE pipe = CreateNamedPipeW(
            name.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            8192, 8192, 0,
            nullptr);                       // default DACL: this user, SYSTEM, admins
        if (pipe == INVALID_HANDLE_VALUE) {
            AWA_LOG(L"ipc: could not create the control pipe (%lu)", GetLastError());
            break;
        }

        // Blocks until somebody connects. IpcStop unblocks it by connecting to
        // the pipe itself - simpler and more reliable than overlapped I/O for a
        // channel that handles one short command at a time.
        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                             ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) {
            if (connected) DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }
        if (connected) {
            ServeOneClient(pipe);
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------- server
void IpcStart(HWND msgWnd, IpcHandler handler) {
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return;

    g_msgWnd  = msgWnd;
    g_handler = handler;
    g_stop    = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop) { InterlockedExchange(&g_running, 0); return; }

    g_thread = CreateThread(nullptr, 0, ServerThread, nullptr, 0, nullptr);
    if (!g_thread) {
        CloseHandle(g_stop);
        g_stop = nullptr;
        InterlockedExchange(&g_running, 0);
        AWA_LOG(L"ipc: control channel could not start");
        return;
    }
    AWA_LOG(L"ipc: listening on %s", PipeName().c_str());
}

void IpcStop() {
    if (InterlockedCompareExchange(&g_running, 0, 1) != 1) return;

    if (g_stop) SetEvent(g_stop);

    // Unblock a ConnectNamedPipe that is waiting for a client by being one.
    // The server checks the stop event the moment it wakes and goes no further.
    HANDLE poke = CreateFileW(PipeName().c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (poke != INVALID_HANDLE_VALUE) CloseHandle(poke);

    if (g_thread) {
        if (WaitForSingleObject(g_thread, 3000) != WAIT_OBJECT_0)
            AWA_LOG(L"ipc: server thread did not stop in time; abandoning it");
        else
            CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stop) { CloseHandle(g_stop); g_stop = nullptr; }
    g_msgWnd  = nullptr;
    g_handler = nullptr;
}

void IpcExecute(WPARAM token) {
    Request* req = reinterpret_cast<Request*>(token);
    if (!req) return;

    // The pipe thread stopped waiting. It deliberately left the request behind
    // rather than freeing something this thread was about to write to, so
    // clearing it up is this thread's job.
    if (InterlockedCompareExchange(&req->abandoned, 0, 0) != 0) {
        CloseHandle(req->done);
        delete req;
        return;
    }

    req->reply = g_handler ? g_handler(req->command)
                           : std::wstring(L"error: no handler");
    SetEvent(req->done);
}

// ---------------------------------------------------------------- client
int IpcClientMain(const std::wstring& command) {
    // Two callers, two situations.
    //
    // prowindowsctl.exe is a console program: it already owns a real stdout, so
    // its output pipes, redirects and gets captured like any other tool's. That
    // is the one to script against.
    //
    // ProWindows.exe --msg is the same code inside a /SUBSYSTEM:WINDOWS binary,
    // which has no stdout at all. It borrows the console that launched it, so
    // typing the command in a terminal still shows an answer - but the answer
    // goes to the console buffer rather than down a pipe, so it cannot be
    // captured. Convenience, not automation.
    // GetFileType rather than comparing the handle: a process with no standard
    // handles gets NULL, a broken one gets INVALID_HANDLE_VALUE, and a closed
    // one gets a handle that fails on write. FILE_TYPE_UNKNOWN covers all three
    // and is the check the API is designed for.
    const HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    bool attached = false;
    bool haveOut  = (stdOut != INVALID_HANDLE_VALUE) &&
                    (GetFileType(stdOut) != FILE_TYPE_UNKNOWN);
    if (!haveOut && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* reopened = nullptr;
        if (freopen_s(&reopened, "CONOUT$", "w", stdout) == 0) {
            attached = true;
            haveOut  = true;
        }
    }
    const auto say = [&](const std::wstring& text) {
        if (!haveOut) return;
        fwprintf(stdout, L"%s\n", text.c_str());
        fflush(stdout);
    };
    const auto done = [&](int code) {
        if (attached) FreeConsole();
        return code;
    };

    // Answered here rather than over the pipe, so `--help` still works when
    // nothing is running - which is exactly when somebody is most likely to
    // ask what the commands are.
    if (command.empty() || command == L"help" || command == L"--help") {
        say(IpcHelpText());
        return done(0);
    }

    const std::wstring name = PipeName();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        // Every instance is busy serving somebody else; wait for a free one.
        if (GetLastError() != ERROR_PIPE_BUSY) break;
        if (!WaitNamedPipeW(name.c_str(), 2000)) break;
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        say(L"error: ProWindows is not running.");
        return done(2);
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    const std::string payload = ToUtf8(command);
    DWORD wrote = 0;
    if (!WriteFile(pipe, payload.data(), (DWORD)payload.size(), &wrote, nullptr)) {
        say(L"error: could not send the command");
        CloseHandle(pipe);
        return done(2);
    }

    std::string reply;
    char buf[8192];
    for (;;) {
        DWORD got = 0;
        const BOOL ok = ReadFile(pipe, buf, (DWORD)sizeof(buf), &got, nullptr);
        if (got > 0) reply.append(buf, buf + got);
        if (ok) break;                                   // whole message read
        if (GetLastError() != ERROR_MORE_DATA) break;     // truncated or gone
    }
    CloseHandle(pipe);

    const std::wstring text = FromUtf8(reply.data(), (int)reply.size());
    say(text);
    return done(text.compare(0, 6, L"error:") == 0 ? 1 : 0);
}

std::wstring IpcHelpText() {
    return
        L"ProWindows control channel - ProWindows.exe --msg \"<command>\"\n"
        L"\n"
        L"Commands take the same words as the config file's bind lines, so\n"
        L"anything you can bind you can also send.\n"
        L"\n"
        L"  focus left|right|up|down        move focus\n"
        L"  focus id <n>                    focus one window, by id from get windows\n"
        L"  focusnext | focusprev           cycle focus on this workspace\n"
        L"  focuslast                       back to the previous window\n"
        L"  swap left|right|up|down         exchange two tiles\n"
        L"  resize left|right|up|down       grow or shrink the focused tile\n"
        L"  promote                         send the focused window to the master slot\n"
        L"  workspace <1-9>                 switch workspace\n"
        L"  workspacerel next|prev|e+1|e-1  step workspaces (e = skip empty ones)\n"
        L"  movetoworkspace <1-9>           send the focused window there\n"
        L"  focusmonitor next|prev          switch monitor\n"
        L"  movetomonitor next|prev         send the focused window there\n"
        L"  layout dwindle|master|grid\n"
        L"  cyclelayout                     next layout\n"
        L"  togglesplit | swapsplit         flip or mirror the split (dwindle only)\n"
        L"  togglesticky                    keep this window on every workspace\n"
        L"  movetoscratchpad                send this window to the scratchpad\n"
        L"  scratchpad                      summon or dismiss the scratchpad\n"
        L"  togglefloat | togglefullscreen\n"
        L"  toggletiling | togglegaps\n"
        L"  minimize | close                the focused window\n"
        L"  retile                          rescan and re-arrange now\n"
        L"  reload                          re-read the config file\n"
        L"  launcher                        open the app launcher\n"
        L"  launch <command>                run something\n"
        L"  quit                            exit ProWindows\n"
        L"\n"
        L"Queries print JSON:\n"
        L"  get windows | get workspaces | get monitors | get state | get version\n"
        L"\n"
        L"  help                            this text\n";
}

} // namespace awa
