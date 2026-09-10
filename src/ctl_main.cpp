// prowindowsctl - the command-line front end for a running ProWindows.
//
// The same job hyprctl does for Hyprland and i3-msg does for i3, and it exists
// as its own executable for the same practical reason both of those do: the
// window manager is a /SUBSYSTEM:WINDOWS program and therefore has no stdout,
// so anything it prints cannot be piped, redirected or captured. A console
// program can be. That is the whole difference - the transport, the command
// grammar and the replies are shared code in ipc.cpp.
//
//   prowindowsctl workspace 3
//   prowindowsctl get windows
//   prowindowsctl get state > state.json
//
// Exit codes: 0 the command was accepted, 1 it was rejected, 2 nothing is
// listening. Worth having, because a script wants to know which.
#include "ipc.h"

int wmain(int argc, wchar_t** argv) {
    std::wstring command;
    for (int i = 1; i < argc; ++i) {
        if (!command.empty()) command += L' ';
        command += argv[i];
    }
    return awa::IpcClientMain(command);
}
