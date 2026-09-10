// ProWindows - the control channel.
//
// `ProWindows.exe --msg "workspace 3"` talks to the copy that is already
// running, exactly the way `i3-msg` and `hyprctl` talk to theirs. Two reasons
// it is worth having, and the second is the bigger one:
//
//   1. Everything the tiler does becomes scriptable - a Stream Deck button, an
//      AutoHotkey line, a scheduled task, a shell alias.
//   2. Everything the tiler does becomes *testable* from outside the process.
//      Until now the only way to check that a workspace switch really hid the
//      right windows was to drive the real desktop by hand.
//
// The command grammar is not a second language: it is the same
// `ParseAction` the config file's `bind =` lines go through, so a command
// that works in one works in the other and neither can drift.
//
// Transport is one named pipe per session, message-mode, default DACL - which
// is to say the user who started ProWindows, LocalSystem and Administrators.
// It carries no secrets, but it can move windows, so it is deliberately not
// widened beyond that.
//
// Threading: the pipe has a thread of its own and that thread never touches
// the window manager. It POSTS the command to the UI thread and waits for the
// answer. Posting rather than sending is the whole point - a sent message is
// dispatched while the manager is parked inside a cross-process call, and
// running a command in there is precisely the reentrancy the rest of this
// codebase works to avoid. A posted one runs from the main loop, between
// passes, where every other action already runs.
#pragma once
#include "common.h"

namespace awa {

// Turns one command into its reply. Runs on the UI thread. Supplied by main.cpp,
// which is the only place that can see both the manager and the config.
using IpcHandler = std::wstring (*)(const std::wstring& command);

// Starts the pipe server. Safe to call when it is already running.
void IpcStart(HWND msgWnd, IpcHandler handler);

// Stops it and waits for the thread. Safe when it was never started.
void IpcStop();

// Called on the UI thread when WM_AWA_IPC arrives; `token` is the wParam.
void IpcExecute(WPARAM token);

// The `--msg` side. Connects to the running instance, prints the reply, and
// returns the exit code the process should use: 0 for a reply, 1 for a command
// the manager rejected, 2 when nothing is listening.
int IpcClientMain(const std::wstring& command);

// Every command the channel understands, as text. Printed by `--msg help` and
// by `--help`, so the list cannot go stale in a document nobody updates.
std::wstring IpcHelpText();

} // namespace awa
