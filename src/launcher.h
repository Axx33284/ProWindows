// ProWindows - the app launcher.
//
// The Hyprland habit this project is chasing: hit one chord, type three
// letters, hit Enter. No desktop shortcuts, no Start menu, no mouse.
//
// The list of apps is built once on a background thread, so the window is
// already populated the first time it is asked for. Everything after that is
// a substring scan over a few hundred strings, which is far too fast to be
// worth optimising.
#pragma once
#include "common.h"
#include "config.h"

namespace awa {

void LauncherInit(HINSTANCE inst, Config* cfg);
void LauncherShutdown();

// Shows it if hidden, hides it if already up: the same chord does both.
void LauncherToggle();
void LauncherHide();
bool LauncherVisible();

// Re-reads the theme colours and drops the cached app list, so a rescan picks
// up anything installed since start-up.
void LauncherRefresh();

} // namespace awa
