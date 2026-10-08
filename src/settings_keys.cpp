// ProWindows - the Shortcuts and Apps pages.
//
// Both are views of one list, the edit copy's `binds`: window actions on one,
// app launchers on the other, so a clash between them is still caught. A
// shortcut is changed where it is shown - Enter on the row, then the keys -
// the way the game rebinds a control, rather than in a dialog.
#include "settings_internal.h"
#include "modal.h"
#include "winutil.h"
#include "theme.h"

namespace awa {

using ui::Row;

namespace {

bool IsLaunch(const Keybind& kb) { return kb.action == ACT_LAUNCH; }

int Pack(const Keybind& kb) { return (int)MAKELONG(kb.vk, kb.mods); }

// Which heading an action is listed under.
int GroupOf(Action a) {
    switch (a) {
        case ACT_FOCUS_DIR: case ACT_FOCUS_NEXT: case ACT_FOCUS_PREV:
        case ACT_FOCUS_LAST: case ACT_FOCUS_MONITOR:
            return 0;
        case ACT_SWAP_DIR: case ACT_MOVE_TO_MONITOR: case ACT_PROMOTE:
        case ACT_TOGGLE_SPLIT: case ACT_SWAP_SPLIT:
            return 1;
        case ACT_RESIZE_DIR:
            return 2;
        case ACT_WORKSPACE: case ACT_MOVE_TO_WORKSPACE: case ACT_WORKSPACE_REL:
        case ACT_WORKSPACE_USED: case ACT_SCRATCHPAD_MOVE: case ACT_SCRATCHPAD_TOGGLE:
        case ACT_TOGGLE_STICKY:
            return 3;
        case ACT_CYCLE_LAYOUT: case ACT_SET_LAYOUT: case ACT_TOGGLE_GAPS:
        case ACT_TOGGLE_TILING: case ACT_RETILE:
            return 4;
        case ACT_TOGGLE_FLOAT: case ACT_TOGGLE_FULLSCREEN: case ACT_CLOSE_WINDOW:
        case ACT_MINIMIZE:
            return 5;
        default:
            return 6;
    }
}
const wchar_t* kGroups[] = { L"Focus", L"Move windows", L"Resize", L"Workspaces",
                             L"Layout", L"Windows", L"ProWindows" };

const wchar_t* HelpFor(Action a) {
    switch (a) {
        case ACT_FOCUS_DIR:         return L"Moves the focus to the nearest window in that direction.";
        case ACT_FOCUS_NEXT:
        case ACT_FOCUS_PREV:        return L"Steps the focus through the windows on this workspace in order.";
        case ACT_FOCUS_LAST:        return L"Jumps back to the window that had the focus before this one; press it again to come back. Alt+Tab that stays on this workspace.";
        case ACT_FOCUS_MONITOR:     return L"Moves the focus to the next or previous screen.";
        case ACT_SWAP_DIR:          return L"Moves the focused window one place in that direction, trading places with what is there.";
        case ACT_MOVE_TO_MONITOR:   return L"Sends the focused window to the next or previous screen.";
        case ACT_PROMOTE:           return L"Puts the focused window in the main slot - the big one in the Master arrangement, the first split in Dwindle.";
        case ACT_TOGGLE_SPLIT:      return L"Turns the split the focused window sits in round: side by side becomes one above the other, and back. Dwindle only.";
        case ACT_SWAP_SPLIT:        return L"Swaps the two halves of the split the focused window sits in. Dwindle only.";
        case ACT_RESIZE_DIR:        return L"Moves the dividing line next to the focused window, by the resize step set on the Layout page.";
        case ACT_WORKSPACE:         return L"Shows that workspace. The windows on the one you leave are hidden until you come back.";
        case ACT_MOVE_TO_WORKSPACE: return L"Sends the focused window to that workspace without going there.";
        case ACT_WORKSPACE_REL:     return L"Steps to the neighbouring workspace.";
        case ACT_WORKSPACE_USED:    return L"Steps to the nearest workspace that has windows on it, skipping empty ones.";
        case ACT_SCRATCHPAD_MOVE:   return L"Sends the focused window to the scratchpad: a holding place that belongs to no workspace.";
        case ACT_SCRATCHPAD_TOGGLE: return L"Brings the scratchpad window over whatever you are looking at, or puts it away again.";
        case ACT_TOGGLE_STICKY:     return L"Keeps the focused window on every workspace - a music player, a chat - or stops.";
        case ACT_CYCLE_LAYOUT:      return L"Switches this workspace to the next arrangement: Dwindle, Master, Grid.";
        case ACT_SET_LAYOUT:        return L"Switches this workspace to that arrangement.";
        case ACT_TOGGLE_GAPS:       return L"Turns the gaps between windows off and on again.";
        case ACT_TOGGLE_TILING:     return L"Stops arranging windows, or starts again. Windows stay where they are while it is paused.";
        case ACT_RETILE:            return L"Puts every window back where the arrangement says it belongs.";
        case ACT_TOGGLE_FLOAT:      return L"Takes the focused window out of the arrangement so it can be moved and sized freely, or puts it back.";
        case ACT_TOGGLE_FULLSCREEN: return L"Makes the focused window fill the screen, or returns it to its tile.";
        case ACT_CLOSE_WINDOW:      return L"Closes the focused window, as its own close button would.";
        case ACT_MINIMIZE:          return L"Minimises the focused window.";
        case ACT_LAUNCHER:          return L"Opens the search bar: apps, files, settings and sums.";
        case ACT_TIMER:             return L"Shows the timer and stopwatch panel, or hides it. Timers keep running by the system clock.";
        case ACT_RELOAD_CONFIG:     return L"Reads config.ini again.";
        case ACT_QUIT:              return L"Quits ProWindows. Every hidden window is shown again first.";
        default:                    return L"";
    }
}

std::wstring RouteNote(const Keybind& kb, bool overrideReserved) {
    if (kb.route == BindRoute::Blocked)
        return L"\r\n\r\nThis key is blocked: another program already owns it. Pick a different one.";
    if ((kb.mods & MOD_WIN) && !overrideReserved)
        return L"\r\n\r\nWindows keeps most Win shortcuts for itself, so this one will not work "
               L"until \"Take over Windows shortcuts\" is on.";
    if (kb.route == BindRoute::Hook)
        return L"\r\n\r\nWindows reserves this key, so ProWindows takes it with its keyboard hook.";
    return L"";
}

std::function<std::wstring()> TagFor(const Keybind& kb) {
    const BindRoute route = kb.route;
    const UINT mods = kb.mods;
    return [route, mods]() -> std::wstring {
        if (route == BindRoute::Blocked) return L"Blocked";
        if ((mods & MOD_WIN) && !Edit().overrideReserved) return L"Needs takeover";
        return L"";
    };
}

// Gives the chord to binding `index` (or to a new binding for `action`/`arg`
// when index is -1), taking it from whatever else had it once the user agrees.
void Assign(int index, Action action, int arg, const std::wstring& command, UINT mods, UINT vk) {
    // The question below runs its own loop, and a reload while it is up (tray,
    // control channel, an overlay's menu) can replace the list the indices
    // point into; if it did, the answer is about a list that is gone.
    const std::vector<Keybind> before = Edit().binds;
    std::vector<int> clashes;
    if (!ConfirmChordFree(mods, vk, index, &clashes)) return;
    auto& binds = Edit().binds;
    bool same = binds.size() == before.size();
    for (size_t i = 0; same && i < binds.size(); ++i)
        same = binds[i].mods == before[i].mods && binds[i].vk == before[i].vk &&
               binds[i].action == before[i].action && binds[i].arg == before[i].arg;
    if (!same) return;
    std::sort(clashes.rbegin(), clashes.rend());
    for (int j : clashes) {
        binds.erase(binds.begin() + j);
        if (j < index) --index;
    }
    if (index >= 0 && index < (int)binds.size()) {
        Keybind& kb = binds[(size_t)index];
        kb.mods  = mods;
        kb.vk    = vk;
        kb.spec  = KeySpecText(mods, vk);
        kb.route = BindRoute::Unregistered;
    } else {
        Keybind kb;
        kb.mods    = mods;
        kb.vk      = vk;
        kb.action  = action;
        kb.arg     = arg;
        kb.command = command;
        kb.spec    = KeySpecText(mods, vk);
        binds.push_back(kb);
    }
    SettingsRebuild();
    if (!clashes.empty()) SettingsToast(DescribeChord(mods, vk) + L" moved here");
}

// The nth binding in `binds` for this action and argument, or -1.
int NthFor(const std::vector<Keybind>& binds, Action a, int arg, int nth) {
    int seen = 0;
    for (int i = 0; i < (int)binds.size(); ++i) {
        const Keybind& kb = binds[(size_t)i];
        if (IsLaunch(kb) || kb.action != a || kb.arg != arg) continue;
        if (seen++ == nth) return i;
    }
    return -1;
}

// The nth launcher, or -1.
int NthLauncher(const std::vector<Keybind>& binds, int nth) {
    int seen = 0;
    for (int i = 0; i < (int)binds.size(); ++i)
        if (IsLaunch(binds[(size_t)i]) && seen++ == nth) return i;
    return -1;
}

Row TakeoverRow() {
    Row r = ui::Toggle(L"takeover", L"Take over Windows shortcuts",
        L"Windows keeps nearly every Win+key shortcut for itself (Win+E, Win+F, Win+Q...). "
        L"With this on, ProWindows takes the ones you have given it with a small keyboard "
        L"hook. Only those exact chords are caught - everything else you type goes "
        L"straight through - and it costs nothing while idle.",
        &Edit().overrideReserved, &Saved().overrideReserved);
    return r;
}

} // namespace

const wchar_t* ActionHelp(Action a) { return HelpFor(a); }

// ================================================================ Shortcuts
int ShortcutIssues() {
    int n = 0;
    for (const auto& kb : Edit().binds)
        if (kb.route == BindRoute::Blocked) ++n;
    return n;
}

void BuildShortcutsPage(std::vector<Row>& rows) {
    Config& e = Edit();

    rows.push_back(ui::Page(L"Windows"));
    rows.push_back(ui::Section(L"Modifier"));
    {
        Row r;
        r.kind  = ui::Kind::Choice;
        r.id    = L"modifier";
        r.label = L"Modifier key";
        r.help  = L"The key held for every ProWindows shortcut marked with it. Changing it "
                  L"moves every shortcut that uses it: with Win, Alt+H becomes Win+H.";
        r.wrap  = false;
        for (const auto& m : kModChoices) r.options.push_back(m.label);
        r.get = []() {
            for (int i = 0; i < 6; ++i) if (kModChoices[i].mask == Edit().modMask) return i;
            return 0;
        };
        r.set = [](int v) {
            Config& c = Edit();
            const UINT oldMod = c.modMask, newMod = kModChoices[v].mask;
            if (oldMod == newMod) return;
            for (auto& kb : c.binds) {
                if (IsLaunch(kb)) continue;
                if ((kb.mods & oldMod) == oldMod) {
                    kb.mods = (kb.mods & ~oldMod) | newMod;
                    kb.spec = KeySpecText(kb.mods, kb.vk);
                    kb.route = BindRoute::Unregistered;
                }
            }
            c.modMask = newMod;
        };
        r.modified = []() { return Edit().modMask != Saved().modMask; };
        r.fallback = []() -> std::wstring {
            Config d; d.LoadDefaults();
            for (int i = 0; i < 6; ++i) if (kModChoices[i].mask == d.modMask) return kModChoices[i].label;
            return L"";
        };
        rows.push_back(r);
    }
    rows.push_back(TakeoverRow());

    // Every action there is a default for, in the defaults' order, then any
    // the config file binds that is not among them. An action with no key
    // still gets a row, so a cleared shortcut can be given one again. The
    // defaults are worked out once: building them looks up the default
    // browser and searches the PATH, and these rows are rebuilt after every
    // change and on every keystroke of a search.
    struct Slot { Action a; int arg; };
    static std::vector<Slot> defaultSlots;
    if (defaultSlots.empty()) {
        Config defaults;
        defaults.LoadDefaults();
        for (const auto& kb : defaults.binds) {
            if (IsLaunch(kb)) continue;
            bool seen = false;
            for (const Slot& s : defaultSlots) seen |= (s.a == kb.action && s.arg == kb.arg);
            if (!seen) defaultSlots.push_back({ kb.action, kb.arg });
        }
    }
    std::vector<Slot> catalogue = defaultSlots;
    auto known = [&](Action a, int arg) {
        for (const Slot& s : catalogue) if (s.a == a && s.arg == arg) return true;
        return false;
    };
    for (const auto& kb : e.binds)
        if (!IsLaunch(kb) && !known(kb.action, kb.arg)) catalogue.push_back({ kb.action, kb.arg });

    // Sections in page order: Windows before Workspaces (PLAN-1.6 2.6).
    static const int kGroupOrder[] = { 0, 1, 2, 5, 3, 4, 6 };
    static_assert(ARRAYSIZE(kGroupOrder) == ARRAYSIZE(kGroups), "every group needs a place");
    for (int group : kGroupOrder) {
        bool headed = false;
        for (const Slot& slot : catalogue) {
            if (GroupOf(slot.a) != group) continue;
            if (!headed) { if (group == 3) rows.push_back(ui::Page(L"Workspaces")); if (group == 6) rows.push_back(ui::Page(L"ProWindows")); rows.push_back(ui::Section(kGroups[group])); headed = true; }

            int count = 0;
            for (const auto& kb : e.binds)
                if (!IsLaunch(kb) && kb.action == slot.a && kb.arg == slot.arg) ++count;
            const int slots = (std::max)(1, count);
            for (int nth = 0; nth < slots; ++nth) {
                const int index = NthFor(e.binds, slot.a, slot.arg, nth);
                Keybind probe;
                probe.action = slot.a;
                probe.arg    = slot.arg;
                const Keybind& kb = index >= 0 ? e.binds[(size_t)index] : probe;

                Row r;
                r.kind  = ui::Kind::Keys;
                r.id    = L"key:" + std::to_wstring((int)slot.a) + L":" + std::to_wstring(slot.arg) +
                          L":" + std::to_wstring(nth);
                r.label = DescribeAction(kb);
                r.help  = std::wstring(HelpFor(slot.a)) +
                          (index >= 0 ? RouteNote(kb, e.overrideReserved) : L"") +
                          L"\r\n\r\nEnter records a new key. Delete clears it.";
                const Action a = slot.a;
                const int arg = slot.arg;
                r.get = [a, arg, nth]() {
                    const int i = NthFor(Edit().binds, a, arg, nth);
                    return i >= 0 ? Pack(Edit().binds[(size_t)i]) : 0;
                };
                r.set = [a, arg, nth](int packed) {
                    Assign(NthFor(Edit().binds, a, arg, nth), a, arg, L"", HIWORD(packed), LOWORD(packed));
                };
                if (index >= 0) {
                    r.remove = [a, arg, nth]() {
                        auto& b = Edit().binds;
                        const int i = NthFor(b, a, arg, nth);
                        if (i >= 0) b.erase(b.begin() + i);
                        SettingsRebuild();
                    };
                    r.tag = TagFor(kb);
                }
                r.modified = [a, arg, nth]() {
                    const int now = NthFor(Edit().binds, a, arg, nth);
                    const int was = NthFor(Saved().binds, a, arg, nth);
                    if ((now < 0) != (was < 0)) return true;
                    if (now < 0) return false;
                    return Pack(Edit().binds[(size_t)now]) != Pack(Saved().binds[(size_t)was]);
                };
                rows.push_back(r);
            }
        }
    }

    rows.push_back(ui::Section(L"All shortcuts"));
    rows.push_back(ui::Action(L"resetkeys", L"Put every key back",
        L"Every window shortcut goes back to its default key. The Apps page's shortcuts "
        L"are not touched. Nothing is saved until you apply.",
        L"Reset", []() {
            if (!ui::Confirm(SettingsHwnd(), L"Put every key back?",
                    L"Every window shortcut goes back to its default key. Your app shortcuts "
                    L"stay as they are.", L"Reset", L"Cancel"))
                return;
            Config& c = Edit();
            std::vector<Keybind> kept;
            for (const auto& kb : c.binds) if (IsLaunch(kb)) kept.push_back(kb);
            Config fresh;
            fresh.modMask = c.modMask;
            // Workspace keys are made for as many workspaces as there are.
            fresh.workspaceCount = c.workspaceCount;
            fresh.LoadDefaults();
            c.binds.clear();
            for (const auto& kb : fresh.binds) if (!IsLaunch(kb)) c.binds.push_back(kb);
            for (const auto& kb : kept) c.binds.push_back(kb);
            SettingsRebuild();
        }));
}

void ResetShortcutsPage() {
    Config& c = Edit();
    Config fresh;
    fresh.workspaceCount = c.workspaceCount;
    fresh.LoadDefaults();
    std::vector<Keybind> kept;
    for (const auto& kb : c.binds) if (IsLaunch(kb)) kept.push_back(kb);
    c.modMask = fresh.modMask;
    c.overrideReserved = fresh.overrideReserved;
    c.binds.clear();
    for (const auto& kb : fresh.binds) if (!IsLaunch(kb)) c.binds.push_back(kb);
    for (const auto& kb : kept) c.binds.push_back(kb);
}

// ================================================================ Apps
namespace {

// Adds a launcher with no key yet and asks for one at once. If the user backs
// out of choosing the key, the row goes again (settings.cpp, CancelCapture).
void AddLauncher(const std::wstring& command) {
    if (command.empty()) return;
    auto& binds = Edit().binds;
    Keybind kb;
    kb.action  = ACT_LAUNCH;
    kb.command = command;
    binds.push_back(kb);
    int ordinal = -1;
    for (const auto& b : binds) if (IsLaunch(b)) ++ordinal;
    SettingsRebuild();
    SettingsCaptureRow(L"app:" + std::to_wstring(ordinal));
}

} // namespace

void BuildAppsPage(std::vector<Row>& rows) {
    Config& e = Edit();

    rows.push_back(ui::Section(L"Windows shortcuts"));
    rows.push_back(TakeoverRow());

    rows.push_back(ui::Section(L"Open with a key"));
    int ordinal = 0;
    for (int i = 0; i < (int)e.binds.size(); ++i) {
        const Keybind& kb = e.binds[(size_t)i];
        if (!IsLaunch(kb)) continue;
        const int nth = ordinal++;
        Row r;
        r.kind   = ui::Kind::Keys;
        r.id     = L"app:" + std::to_wstring(nth);
        r.label  = FriendlyCommandName(kb.command);
        r.detail = kb.command;
        r.raw    = true;
        r.help   = L"Opens " + kb.command + L"." + RouteNote(kb, e.overrideReserved) +
                   L"\r\n\r\nEnter records a new key, F2 chooses what it opens, Delete "
                   L"removes it.";
        r.get = [nth]() {
            const int at = NthLauncher(Edit().binds, nth);
            return at >= 0 ? Pack(Edit().binds[(size_t)at]) : 0;
        };
        r.set = [nth](int packed) {
            const int at = NthLauncher(Edit().binds, nth);
            if (at < 0) return;
            Assign(at, ACT_LAUNCH, 0, Edit().binds[(size_t)at].command, HIWORD(packed), LOWORD(packed));
        };
        r.remove = [nth]() {
            auto& b = Edit().binds;
            const int at = NthLauncher(b, nth);
            if (at >= 0) b.erase(b.begin() + at);
            SettingsRebuild();
        };
        r.extraKey  = VK_F2;
        r.extraWord = L"Program";
        r.extra = [nth]() {
            std::wstring path;
            if (!BrowseForExePath(SettingsHwnd(), &path)) return;
            const int at = NthLauncher(Edit().binds, nth);
            if (at >= 0) Edit().binds[(size_t)at].command = path;
            SettingsRebuild();
        };
        r.tag = TagFor(kb);
        r.modified = [nth]() {
            const int now = NthLauncher(Edit().binds, nth);
            const int was = NthLauncher(Saved().binds, nth);
            if ((now < 0) != (was < 0)) return true;
            if (now < 0) return false;
            const Keybind& a = Edit().binds[(size_t)now];
            const Keybind& b = Saved().binds[(size_t)was];
            return Pack(a) != Pack(b) || a.command != b.command;
        };
        rows.push_back(r);
    }
    if (ordinal == 0)
        rows.push_back(ui::Info(L"noapps", L"No app shortcuts yet",
            L"Add an app below, then press the keys that should open it.", nullptr));

    rows.push_back(ui::Action(L"addapp", L"Add an installed app",
        L"Pick from the apps in your Start menu, then press the keys that should open it.",
        L"Choose", []() {
            std::vector<ui::PickEntry> entries;
            for (const auto& a : EnumStartMenuApps()) entries.push_back({ a.name, L"", a.launchPath });
            std::wstring chosen;
            if (ui::Pick(SettingsHwnd(), L"Open an app with a key", L"Apps installed on this PC",
                         entries, &chosen, L"Add"))
                AddLauncher(chosen);
        }));
    rows.push_back(ui::Action(L"addfile", L"Add a program or file",
        L"Choose any program, script or document, then press the keys that should open it.",
        L"Browse", []() {
            std::wstring path;
            if (BrowseForExePath(SettingsHwnd(), &path)) AddLauncher(path);
        }));
}

void ResetAppsPage() {
    Config& c = Edit();
    Config fresh;
    fresh.modMask = c.modMask;
    fresh.LoadDefaults();
    std::vector<Keybind> kept;
    for (const auto& kb : c.binds) if (!IsLaunch(kb)) kept.push_back(kb);
    for (const auto& kb : fresh.binds) if (IsLaunch(kb)) kept.push_back(kb);
    c.binds = kept;
    c.overrideReserved = fresh.overrideReserved;
}

} // namespace awa
