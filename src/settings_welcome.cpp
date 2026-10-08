// ProWindows - the Welcome page. What the app is, things to try, and the keys
// that matter, read live from the edit copy's binds so it never goes stale.
// Built only from the existing row kinds; nothing here is edited, so nothing
// joins AWA_EDITED_FIELDS.
#include "settings.h"
#include "settings_internal.h"
#include "app.h"
#include "launcher.h"

namespace awa {

using ui::Row;

namespace {

// Every chord bound to it, not just the first: search is both Alt+R and Win+S,
// and the one someone remembers may be either.
std::wstring ChordOf(Action a, int arg) {
    std::wstring out;
    for (const auto& kb : Edit().binds) {
        if (kb.action != a || kb.arg != arg || !kb.vk) continue;
        if (!out.empty()) out += L"  or  ";
        out += DescribeChord(kb.mods, kb.vk);
    }
    return out.empty() ? L"Not set" : out;
}

// "Alt + 1 ... 9" for a run of workspace binds sharing a modifier, else the first chord.
std::wstring RunOf(Action a) {
    const Keybind* first = nullptr;
    const Keybind* last = nullptr;
    for (const auto& kb : Edit().binds) {
        if (kb.action != a || !kb.vk) continue;
        if (kb.arg == 0) first = &kb;
        if (kb.arg == 8) last = &kb;
    }
    if (!first) return L"Not set";
    if (last && last->mods == first->mods && last->vk >= '1' && last->vk <= '9')
        return DescribeChord(first->mods, first->vk) + L" ... " + (wchar_t)last->vk;
    return DescribeChord(first->mods, first->vk);
}

// A keys row: label from the action, value live, help from the Shortcuts page.
void KeyRow(std::vector<Row>& rows, Action a, int arg) {
    Keybind probe;
    probe.action = a;
    probe.arg = arg;
    rows.push_back(ui::Info(L"wk:" + std::to_wstring((int)a) + L":" + std::to_wstring(arg),
                            DescribeAction(probe), ActionHelp(a),
                            [a, arg]() { return ChordOf(a, arg); }));
}

// What the tray does for a "Show": switch the overlay on in the live config,
// save, and let the settings window catch up. The Row& is not touched after.
void ShowOverlay(bool Config::*field, const wchar_t* toast) {
    AppConfig().*field = true;
    AppUpdateOverlays();
    AppSaveConfig();
    SettingsRefresh();
    SettingsToast(toast);
}

} // namespace

void BuildWelcomePage(std::vector<Row>& rows) {
    rows.push_back(ui::Page(L"Start here"));
    rows.push_back(ui::Section(L"ProWindows"));
    rows.push_back(ui::Info(L"what", L"Tiles every window",
        L"ProWindows arranges every window on your screen into a layout so nothing overlaps "
        L"and nothing is lost behind something else. Add workspaces to keep projects apart, "
        L"move around from the keyboard, and put a search bar, a system monitor, a clock and "
        L"a timer on the desktop. Everything is in the tabs above.",
        []() { return std::wstring(L"Layouts, workspaces, overlays"); }));

    rows.push_back(ui::Section(L"Try these"));
    rows.push_back(ui::Action(L"trysearch",
        L"Search  (" + ChordOf(ACT_LAUNCHER, 0) + L")",
        L"Opens the search bar: type to find apps, files, settings and sums. Press the "
        L"shortcut shown to open it any time.",
        L"Open", []() { LauncherToggle(); }));
    rows.push_back(ui::Action(L"trytimer", L"Timer",
        L"Shows the timer and stopwatch panel on your screen. Drag it anywhere; right-click "
        L"it for more. Timers keep running while it is hidden.",
        L"Show", []() { ShowOverlay(&Config::timerShown, L"The timer is on screen"); }));
    rows.push_back(ui::Action(L"trymonitor", L"System monitor",
        L"Shows a small floating panel with processor, memory and network use, always up to "
        L"date. Choose what it shows on the Monitor tab.",
        L"Show", []() { ShowOverlay(&Config::monitorEnabled, L"The system monitor is on screen"); }));
    rows.push_back(ui::Action(L"tryclock", L"Clock",
        L"Shows a desktop clock in one of a dozen styles. Change its look on the Clock tab.",
        L"Show", []() { ShowOverlay(&Config::clockEnabled, L"The clock is on screen"); }));
    rows.push_back(ui::Action(L"tryfolder", L"Settings folder",
        L"Opens the folder that holds config.ini and the other files ProWindows keeps. "
        L"Copy it to move your setup to another machine.",
        L"Open", []() { AppOpenConfigFolder(); }));

    rows.push_back(ui::Section(L"Mouse"));
    {
        const std::wstring mod = ModifierName(Edit().modMask);
        rows.push_back(ui::Info(L"mousemove", mod + L" + drag to move",
            L"Hold " + mod + L" and drag anywhere on a window to move it; no need to find the "
            L"title bar. Switch it off on the Behaviour tab if you would rather not.",
            []() { return std::wstring(L"Left button"); }));
        rows.push_back(ui::Info(L"mouseresize", mod + L" + drag to resize",
            L"Hold " + mod + L" and drag with the right button to resize the window under "
            L"the pointer.",
            []() { return std::wstring(L"Right button"); }));
        rows.push_back(ui::Info(L"mouseswap", L"Drag to swap",
            L"Drag a window by its title bar onto one side of another window and it moves "
            L"there. A highlight shows where it will land. Turn it off on the Behaviour tab.",
            []() { return std::wstring(L"Title bar onto a window"); }));
    }

    rows.push_back(ui::Page(L"Keys"));
    rows.push_back(ui::Section(L"Search and apps"));
    KeyRow(rows, ACT_LAUNCHER, 0);
    KeyRow(rows, ACT_TIMER, 0);
    rows.push_back(ui::Section(L"Focus"));
    for (int d = 0; d < 4; ++d) KeyRow(rows, ACT_FOCUS_DIR, d);
    KeyRow(rows, ACT_FOCUS_LAST, 0);
    rows.push_back(ui::Section(L"Move"));
    for (int d = 0; d < 4; ++d) KeyRow(rows, ACT_SWAP_DIR, d);
    rows.push_back(ui::Section(L"Workspaces"));
    // One row for each run of numbers rather than nine.
    rows.push_back(ui::Info(L"wk:ws", L"Switch to workspace", ActionHelp(ACT_WORKSPACE),
                            []() { return RunOf(ACT_WORKSPACE); }));
    rows.push_back(ui::Info(L"wk:send", L"Send window to workspace", ActionHelp(ACT_MOVE_TO_WORKSPACE),
                            []() { return RunOf(ACT_MOVE_TO_WORKSPACE); }));
    rows.push_back(ui::Section(L"Windows"));
    KeyRow(rows, ACT_CLOSE_WINDOW, 0);
    KeyRow(rows, ACT_TOGGLE_FLOAT, 0);
    KeyRow(rows, ACT_TOGGLE_FULLSCREEN, 0);
    KeyRow(rows, ACT_TOGGLE_STICKY, 0);
    rows.push_back(ui::Section(L"ProWindows"));
    KeyRow(rows, ACT_TOGGLE_TILING, 0);
    KeyRow(rows, ACT_RELOAD_CONFIG, 0);
    KeyRow(rows, ACT_QUIT, 0);
    rows.push_back(ui::Action(L"changekeys", L"Change shortcuts",
        L"Every shortcut, with a key you can change: select one, press Enter, then press the "
        L"keys you want.",
        L"Open", []() { SettingsOpenTab(PAGE_SHORTCUTS); }));

    rows.push_back(ui::Page(L"Overlays"));
    rows.push_back(ui::Section(L"Monitor"));
    rows.push_back(ui::Action(L"ovmonitor", L"System monitor",
        L"A small panel with processor, memory, disk and network use. Drag it anywhere; "
        L"right-click it to choose what it shows. \"Pin in place\" in the tray menu locks it "
        L"and lets clicks pass through.",
        L"Show", []() { ShowOverlay(&Config::monitorEnabled, L"The system monitor is on screen"); }));
    rows.push_back(ui::Section(L"Clock"));
    rows.push_back(ui::Action(L"ovclock", L"Clock",
        L"A clock on your screen in one of a dozen styles. Drag it anywhere; right-click it "
        L"for style, theme and size. \"Pin in place\" locks it and lets clicks pass through.",
        L"Show", []() { ShowOverlay(&Config::clockEnabled, L"The clock is on screen"); }));
    rows.push_back(ui::Section(L"Timer"));
    rows.push_back(ui::Action(L"ovtimer", L"Timer and stopwatch",
        L"Countdown timers and a stopwatch that keep running by the system clock, even while "
        L"hidden. Drag the panel anywhere; right-click it for more. \"Pin in place\" locks it "
        L"and lets clicks pass through. The Timer item in the tray menu shows, pins and stops "
        L"the alarm.",
        L"Show", []() { ShowOverlay(&Config::timerShown, L"The timer is on screen"); }));
    rows.push_back(ui::Action(L"ovsettings", L"Look and behaviour",
        L"Styles, colours, size and what each overlay shows are on the Monitor and Clock tabs.",
        L"Clock tab", []() { SettingsOpenTab(PAGE_CLOCK); }));
}

} // namespace awa
