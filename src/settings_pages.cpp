// ProWindows - the Layout, Behaviour and General pages.
//
// Each page is a list of rows over the edit copy (settings_internal.h). The
// rows are rebuilt after every change, so a description can say what the
// current value does and a row that only applies while another is on can be
// greyed out by asking.
#include "settings_internal.h"
#include "modal.h"
#include "theme.h"
#include "winutil.h"

namespace awa {

using ui::Row;

// ================================================================ colours
namespace {

struct Swatch { const wchar_t* name; COLORREF colour; };
const Swatch kPalette[] = {
    { L"Amber",  RGB(255, 176, 0)   },
    { L"Gold",   RGB(232, 190, 96)  },
    { L"Orange", RGB(255, 122, 40)  },
    { L"Red",    RGB(236, 76, 60)   },
    { L"Rose",   RGB(244, 114, 182) },
    { L"Violet", RGB(167, 139, 250) },
    { L"Blue",   RGB(0x7A, 0xA2, 0xF7) },
    { L"Sky",    RGB(96, 205, 255)  },
    { L"Cyan",   RGB(0, 229, 255)   },
    { L"Teal",   RGB(45, 212, 191)  },
    { L"Green",  RGB(96, 210, 132)  },
    { L"Lime",   RGB(190, 242, 100) },
    { L"White",  RGB(236, 238, 240) },
    { L"Silver", RGB(160, 166, 174) },
    { L"Slate",  RGB(0x30, 0x34, 0x40) },
    { L"Black",  RGB(16, 16, 18)    },
};

std::wstring Hex(COLORREF c) {
    wchar_t buf[16];
    swprintf_s(buf, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

} // namespace

Row ColourRow(const std::wstring& id, const std::wstring& label, const std::wstring& help,
              COLORREF* field, const COLORREF* saved, COLORREF themeColour, COLORREF themeValue) {
    Row r;
    r.kind  = ui::Kind::Colour;
    r.id    = id;
    r.label = label;
    r.help  = help + L"\r\n\r\nLeft and Right step through the palette; Enter picks any colour.";

    // The choices: "Theme" first when there is one, the palette, and - when
    // the current colour is none of those - the colour itself, by its value.
    std::vector<COLORREF> values;
    const bool themed = themeColour != CLR_INVALID;
    if (themed) {
        r.options.push_back(L"Theme");
        r.colours.push_back(themeColour);
        values.push_back(themeValue);
    }
    for (const Swatch& s : kPalette) {
        r.options.push_back(s.name);
        r.colours.push_back(s.colour);
        values.push_back(s.colour);
    }
    if (std::find(values.begin(), values.end(), *field) == values.end()) {
        r.options.push_back(Hex(*field));
        r.colours.push_back(*field);
        values.push_back(*field);
    }
    r.get = [field, values]() {
        for (size_t i = 0; i < values.size(); ++i)
            if (values[i] == *field) return (int)i;
        return 0;
    };
    r.set = [field, values](int v) {
        if (v >= 0 && v < (int)values.size()) *field = values[(size_t)v];
    };
    if (saved) r.modified = [field, saved]() { return *field != *saved; };
    const COLORREF fallback = themed ? themeColour : *field;
    r.activate = [field, themed, themeValue, fallback]() {
        COLORREF c = (themed && *field == themeValue) ? fallback : *field;
        if (PickColor(SettingsHwnd(), &c)) *field = c;
    };
    return r;
}

// ================================================================ Layout
namespace {

struct LayoutInfo { const wchar_t* name; const wchar_t* description; };

const LayoutInfo kLayoutInfo[(int)LayoutKind::COUNT] = {
    { L"Dwindle",
      L"Every new window splits the one you are focused on, always cutting along "
      L"its longer side. The result spirals outwards, so the first window keeps the "
      L"most room and later ones fill in around it.\r\n\r\nBest when you keep opening "
      L"and closing windows and want a sensible arrangement without thinking about "
      L"it. This is Hyprland's default." },
    { L"Master",
      L"One big window on the left holds whatever you are working on, and everything "
      L"else stacks in a column on the right.\r\n\r\nBest when one window matters most "
      L"- an editor, a document, a game guide - and the rest are for reference. "
      L"$mod+Enter moves another window into the main area." },
    { L"Grid",
      L"Every window gets an equal cell, in as square a grid as the count allows. Two "
      L"windows split the screen in half, four make a 2x2, and so on.\r\n\r\nBest for "
      L"comparing things side by side, or for dashboards where no single window "
      L"matters more than the others." },
};

std::wstring WithMod(std::wstring text) {
    const wchar_t* mod = ModifierName(Edit().modMask);
    for (size_t p = text.find(L"$mod"); p != std::wstring::npos; p = text.find(L"$mod", p))
        text.replace(p, 4, mod);
    return text;
}

int g_ratioPct = 55;     // the Master ratio as a whole percentage, for its slider

} // namespace

void BuildLayoutPage(std::vector<Row>& rows) {
    Config& e = Edit();
    const Config& s = Saved();
    const auto isMaster = []() { return Edit().layout == LayoutKind::Master; };

    rows.push_back(ui::Section(L"Arrangement"));
    {
        Row r;
        r.kind  = ui::Kind::Choice;
        r.id    = L"layout";
        r.label = L"Arrangement";
        const int k = (int)e.layout;
        r.help  = WithMod(kLayoutInfo[(k >= 0 && k < (int)LayoutKind::COUNT) ? k : 0].description);
        for (const auto& info : kLayoutInfo) r.options.push_back(info.name);
        r.get = []() { const int v = (int)Edit().layout; return (v >= 0 && v < (int)LayoutKind::COUNT) ? v : 0; };
        r.set = [](int v) { Edit().layout = (LayoutKind)v; };
        r.modified = []() { return Edit().layout != Saved().layout; };
        r.fallback = []() -> std::wstring {
            Config d; d.LoadDefaults();
            const int v = (int)d.layout;
            return (v >= 0 && v < (int)LayoutKind::COUNT) ? kLayoutInfo[v].name : L"";
        };
        rows.push_back(r);
    }
    {
        Row r = ui::Slider(L"ratio", L"Main area size",
            L"How much of the screen the main window takes in the Master arrangement. "
            L"The rest is shared by the column beside it.",
            &g_ratioPct, nullptr, 15, 85, 1, 5, L"%");
        r.get = []() { return (int)(Edit().masterRatio * 100.0f + 0.5f); };
        r.set = [](int v) { Edit().masterRatio = (std::max)(15, (std::min)(85, v)) / 100.0f; };
        r.modified = []() { return Edit().masterRatio != Saved().masterRatio; };
        r.enabled = isMaster;
        rows.push_back(r);
    }
    {
        Row r = ui::Slider(L"mastercount", L"Windows in the main area",
            L"How many windows share the main area in the Master arrangement, one above "
            L"the other. Everything else goes in the column beside it.",
            &e.masterCount, &s.masterCount, 1, 4, 1, 1, nullptr);
        r.enabled = isMaster;
        rows.push_back(r);
    }

    rows.push_back(ui::Section(L"Spacing"));
    rows.push_back(ui::Slider(L"gapinner", L"Gap between windows",
        L"The space left between two windows side by side, in pixels.",
        &e.gapInner, &s.gapInner, 0, 100, 1, 4, L"px"));
    rows.push_back(ui::Slider(L"gapouter", L"Gap at the screen edge",
        L"The space kept between the windows and the edges of the screen, in pixels.",
        &e.gapOuter, &s.gapOuter, 0, 100, 1, 4, L"px"));
    rows.push_back(ui::Toggle(L"smartgaps", L"No gaps for a lone window",
        L"When a window is the only one on its workspace it fills the screen, with no "
        L"gap round it. The gaps are there to separate windows, and with nothing to "
        L"separate they are wasted screen.",
        &e.smartGaps, &s.smartGaps));

    rows.push_back(ui::Section(L"Workspaces"));
    {
        std::vector<int> values;
        std::vector<std::wstring> names;
        for (int i = 1; i <= 9; ++i) { values.push_back(i); names.push_back(std::to_wstring(i)); }
        const wchar_t* mod = ModifierName(e.modMask);
        rows.push_back(ui::ChoiceOf(L"workspaces", L"Workspaces",
            std::wstring(L"Separate sets of windows on the same screen. Switch between them with ") +
            mod + L"+1 to " + mod + L"+9, and send a window to one with " + mod + L"+Shift and the number.",
            &e.workspaceCount, &s.workspaceCount, values, names));
    }

    rows.push_back(ui::Section(L"Keep space clear for a bar"));
    const wchar_t* bar =
        L"Room kept free along this edge of every screen, in pixels, on top of what "
        L"Windows already keeps for the taskbar. Tiled windows never go into it - "
        L"for a bar or a dock that Windows does not know about.";
    rows.push_back(ui::Slider(L"margintop",    L"Top",    bar, &e.marginTop,    &s.marginTop,    0, 400, 1, 10, L"px"));
    rows.push_back(ui::Slider(L"marginbottom", L"Bottom", bar, &e.marginBottom, &s.marginBottom, 0, 400, 1, 10, L"px"));
    rows.push_back(ui::Slider(L"marginleft",   L"Left",   bar, &e.marginLeft,   &s.marginLeft,   0, 400, 1, 10, L"px"));
    rows.push_back(ui::Slider(L"marginright",  L"Right",  bar, &e.marginRight,  &s.marginRight,  0, 400, 1, 10, L"px"));

    rows.push_back(ui::Section(L"Resizing"));
    rows.push_back(ui::Slider(L"resizestep", L"Resize step",
        L"How far the dividing line between two windows moves each time a resize "
        L"shortcut is pressed, as a share of the space they share.",
        &e.resizeStep, &s.resizeStep, 1, 25, 1, 5, L"%"));
}

void ResetLayoutPage() {
    Config d;
    d.LoadDefaults();
    Config& e = Edit();
    e.layout = d.layout;               e.masterRatio = d.masterRatio;
    e.masterCount = d.masterCount;     e.gapInner = d.gapInner;
    e.gapOuter = d.gapOuter;           e.smartGaps = d.smartGaps;
    e.workspaceCount = d.workspaceCount;
    e.marginTop = d.marginTop;         e.marginBottom = d.marginBottom;
    e.marginLeft = d.marginLeft;       e.marginRight = d.marginRight;
    e.resizeStep = d.resizeStep;
}

// A miniature screen with the chosen arrangement on it: the focused window a
// pale bar, the rest frames - the page's own vocabulary.
void PreviewLayout(HDC dc, const RECT& box) {
    const Config& e = Edit();
    // Keep a screen's proportions inside the box.
    int w = box.right - box.left, h = box.bottom - box.top;
    if (w * 9 > h * 16) w = h * 16 / 9; else h = w * 9 / 16;
    RECT screen = { box.left + ((box.right - box.left) - w) / 2, box.top + ((box.bottom - box.top) - h) / 2, 0, 0 };
    screen.right = screen.left + w;
    screen.bottom = screen.top + h;
    theme::Wash(dc, screen, RGB(16, 18, 22), 255);
    theme::Frame(dc, screen, theme::Line, 255, 1);

    const float k = (float)w / 1920.0f;
    const int outer = (std::max)(1, (int)((float)e.gapOuter * k * 2.0f));
    const int gap   = (std::max)(1, (int)((float)e.gapInner * k * 2.0f));
    RECT area = { screen.left + outer + 2, screen.top + outer + 2, screen.right - outer - 2, screen.bottom - outer - 2 };
    const int aw = area.right - area.left, ah = area.bottom - area.top;
    if (aw < 12 || ah < 12) return;

    RECT panes[4] = {};
    switch (e.layout) {
        case LayoutKind::Master: {
            const int split = area.left + (int)((float)aw * e.masterRatio);
            const int third = ah / 3;
            panes[0] = { area.left, area.top, split, area.bottom };
            panes[1] = { split, area.top, area.right, area.top + third };
            panes[2] = { split, area.top + third, area.right, area.top + third * 2 };
            panes[3] = { split, area.top + third * 2, area.right, area.bottom };
            break;
        }
        case LayoutKind::Grid: {
            const int mx = area.left + aw / 2, my = area.top + ah / 2;
            panes[0] = { area.left, area.top, mx, my };
            panes[1] = { mx, area.top, area.right, my };
            panes[2] = { area.left, my, mx, area.bottom };
            panes[3] = { mx, my, area.right, area.bottom };
            break;
        }
        default: {
            const int half = area.left + aw / 2, halfH = area.top + ah / 2;
            panes[0] = { area.left, area.top, half, area.bottom };
            panes[1] = { half, area.top, area.right, halfH };
            panes[2] = { half, halfH, half + aw / 4, area.bottom };
            panes[3] = { half + aw / 4, halfH, area.right, area.bottom };
            break;
        }
    }
    for (int i = 3; i >= 0; --i) {
        RECT r = panes[i];
        // Every pane gives up half the gap on each side it shares.
        if (r.left > area.left)     r.left   += gap / 2;
        if (r.right < area.right)   r.right  -= (gap + 1) / 2;
        if (r.top > area.top)       r.top    += gap / 2;
        if (r.bottom < area.bottom) r.bottom -= (gap + 1) / 2;
        if (r.right - r.left < 3 || r.bottom - r.top < 3) continue;
        if (i == 0) {
            theme::Gradient(dc, r, theme::KeyFill, theme::Mix(theme::KeyFill, theme::Bg, 0.25f));
            RECT edge = { r.left, r.top, r.right, r.top + (std::max)(2, theme::Scale(2)) };
            theme::Wash(dc, edge, theme::TextHi, 255);
        } else {
            theme::Gradient(dc, r, theme::Plate, theme::PlateLow);
            theme::Frame(dc, r, theme::Rule, 255, 1);
        }
    }
}

// ================================================================ Behaviour
namespace {

struct RunningApp { std::wstring proc; std::wstring title; };

std::vector<RunningApp> EnumRunningApps() {
    std::vector<HWND> windows;
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));

    std::vector<RunningApp> apps;
    const DWORD self = GetCurrentProcessId();
    for (HWND h : windows) {
        if (!IsWindowVisible(h)) continue;
        if (GetAncestor(h, GA_ROOT) != h) continue;
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) continue;
        if (IsCloaked(h)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == self) continue;
        const std::wstring title = WindowTitle(h);
        if (title.empty()) continue;
        const std::wstring proc = ProcessName(h);
        if (proc.empty()) continue;
        bool skip = false;
        for (const auto& b : BuiltinIgnoreProcess())
            if (IEquals(b, proc)) { skip = true; break; }
        for (const auto& a : apps)
            if (IEquals(a.proc, proc)) { skip = true; break; }
        if (!skip) apps.push_back({ proc, title });
    }
    std::sort(apps.begin(), apps.end(), [](const RunningApp& a, const RunningApp& b) {
        return _wcsicmp(a.proc.c_str(), b.proc.c_str()) < 0;
    });
    return apps;
}

void Exclude(const std::wstring& name) {
    if (name.empty()) return;
    auto& list = Edit().ignoreProcess;
    for (const auto& have : list)
        if (IEquals(have, name)) { SettingsToast(name + L" is already on the list"); return; }
    list.push_back(name);
    SettingsRebuild();
    SettingsToast(name + L" will be left alone");
}

// Milliseconds of travel. Anything under ~120 ms is over before the eye can
// follow it, however many frames it is drawn in.
const int kAnimSpeeds[3] = { 120, 200, 320 };

} // namespace

void BuildBehaviourPage(std::vector<Row>& rows) {
    Config& e = Edit();
    const Config& s = Saved();

    rows.push_back(ui::Section(L"Focus"));
    rows.push_back(ui::Toggle(L"ffm", L"Focus follows the mouse",
        L"The window under the pointer takes the focus as soon as the pointer rests on "
        L"it, without a click.",
        &e.focusFollowsMouse, &s.focusFollowsMouse));
    rows.push_back(ui::Toggle(L"warp", L"Pointer follows the focus",
        L"When a shortcut moves the focus to another window, the pointer jumps to the "
        L"middle of it. Worth turning on with Focus follows the mouse, or the window "
        L"under the resting pointer takes the focus straight back.",
        &e.cursorWarp, &s.cursorWarp));
    rows.push_back(ui::Toggle(L"newtop", L"New windows go",
        L"Where a newly opened window is placed: before the window you are focused on, "
        L"or after it.",
        &e.newWindowOnTop, &s.newWindowOnTop, L"Before", L"After"));

    rows.push_back(ui::Section(L"Mouse"));
    rows.push_back(ui::Toggle(L"dragswap", L"Drag to rearrange",
        L"Drag a window by its title bar onto one side of another window and it moves "
        L"there. A highlight shows where it will land.",
        &e.dragToRearrange, &s.dragToRearrange));
    rows.push_back(ui::Toggle(L"moddrag",
        std::wstring(ModifierName(e.modMask)) + L" + drag to move",
        std::wstring(L"Hold ") + ModifierName(e.modMask) +
        L" and drag anywhere on a window to move it - no need to find the title bar. "
        L"Only for windows ProWindows is arranging; an excluded app keeps its own "
        L"behaviour.",
        &e.modDrag, &s.modDrag));

    rows.push_back(ui::Section(L"Motion"));
    rows.push_back(ui::Toggle(L"anim", L"Animations",
        L"Windows slide into their new places instead of jumping there.",
        &e.animations, &s.animations));
    {
        Row r = ui::ChoiceOf(L"animspeed", L"Animation speed",
            L"How long a window takes to slide into place.",
            &e.animationMs, &s.animationMs, { kAnimSpeeds[0], kAnimSpeeds[1], kAnimSpeeds[2] },
            { L"Fast", L"Normal", L"Relaxed" });
        r.enabled = []() { return Edit().animations; };
        rows.push_back(r);
    }

    rows.push_back(ui::Section(L"Focus border"));
    rows.push_back(ui::Toggle(L"border", L"Highlight the focused window",
        L"Colours the border of the window that has the focus, so you can always see "
        L"where your typing will go. Windows 11 only.",
        &e.accentBorder, &s.accentBorder));
    {
        Row r = ColourRow(L"activecolor", L"Focused window",
            L"The colour of the focused window's border.", &e.activeColor, &s.activeColor);
        r.enabled = []() { return Edit().accentBorder; };
        rows.push_back(r);
        Row q = ColourRow(L"inactivecolor", L"Other windows",
            L"The colour of every other arranged window's border.", &e.inactiveColor, &s.inactiveColor);
        q.enabled = []() { return Edit().accentBorder; };
        rows.push_back(q);
    }
    rows.push_back(ui::ChoiceOf(L"corners", L"Window corners",
        L"Whether arranged windows keep the rounded corners Windows 11 gives them. "
        L"Square corners let neighbouring windows meet cleanly. Windows 11 only.",
        &e.cornerPref, &s.cornerPref, { 0, 1, 2 }, { L"Default", L"Square", L"Round" }));

    rows.push_back(ui::Section(L"Games"));
    rows.push_back(ui::Toggle(L"gamepause", L"Pause for fullscreen apps",
        L"Stops completely - no arranging, no animation, no overlays - while a game or "
        L"any other fullscreen app has the screen, and starts again when it goes. "
        L"Arranging windows behind a game costs it frames.",
        &e.pauseForFullscreen, &s.pauseForFullscreen));

    rows.push_back(ui::Section(L"Never arrange these apps"));
    auto& list = e.ignoreProcess;
    for (size_t i = 0; i < list.size(); ++i) {
        Row r;
        r.kind   = ui::Kind::Item;
        r.id     = L"exclude:" + ToLower(list[i]);
        r.label  = list[i];
        r.raw    = true;
        r.help   = L"ProWindows leaves every window of " + list[i] + L" exactly where it is. "
                   L"Delete takes it off the list.";
        r.button = L"Remove";
        const std::wstring name = list[i];
        r.remove = [name]() {
            auto& l = Edit().ignoreProcess;
            for (size_t k = 0; k < l.size(); ++k)
                if (IEquals(l[k], name)) { l.erase(l.begin() + (ptrdiff_t)k); break; }
            SettingsRebuild();
        };
        r.modified = [name]() {
            for (const auto& have : Saved().ignoreProcess) if (IEquals(have, name)) return false;
            return true;
        };
        rows.push_back(r);
    }
    rows.push_back(ui::Action(L"exclude-running", L"Add a running app",
        L"Pick one of the apps running right now. Its windows will never be arranged.",
        L"Choose", []() {
            std::vector<ui::PickEntry> entries;
            for (const auto& a : EnumRunningApps()) entries.push_back({ a.proc, a.title, a.proc });
            std::wstring chosen;
            if (ui::Pick(SettingsHwnd(), L"Leave an app alone", L"Apps running right now",
                         entries, &chosen, L"Add"))
                Exclude(chosen);
        }));
    rows.push_back(ui::Action(L"exclude-browse", L"Add a program by file",
        L"Choose the program's .exe. Useful for an app that is not running now.",
        L"Browse", []() {
            std::wstring name;
            if (BrowseForExeName(SettingsHwnd(), &name)) Exclude(name);
        }));
}

void ResetBehaviourPage() {
    Config d;
    d.LoadDefaults();
    Config& e = Edit();
    e.focusFollowsMouse = d.focusFollowsMouse; e.cursorWarp = d.cursorWarp;
    e.newWindowOnTop = d.newWindowOnTop;       e.dragToRearrange = d.dragToRearrange;
    e.modDrag = d.modDrag;                     e.animations = d.animations;
    e.animationMs = d.animationMs;             e.pauseForFullscreen = d.pauseForFullscreen;
    e.accentBorder = d.accentBorder;           e.activeColor = d.activeColor;
    e.inactiveColor = d.inactiveColor;         e.cornerPref = d.cornerPref;
    e.ignoreProcess = d.ignoreProcess;
}

// ================================================================ General
namespace {

// The About block's lines, for rows of their own.
std::vector<std::wstring> AboutLines() {
    std::vector<std::wstring> lines;
    const std::wstring text = AppAboutText();
    size_t from = 0;
    while (from <= text.size()) {
        size_t at = text.find(L"\r\n", from);
        if (at == std::wstring::npos) at = text.size();
        lines.push_back(text.substr(from, at - from));
        from = at + 2;
    }
    return lines;
}

// Reloading throws away whatever has not been applied, so it asks - and then
// really does throw it away (DiscardEdits).
bool OkToDiscard(const wchar_t* what) {
    if (!EditsPending()) return true;
    if (!ui::Confirm(SettingsHwnd(), what,
            L"Changes you have not applied will be lost.", L"Continue", L"Cancel"))
        return false;
    DiscardEdits();
    return true;
}

} // namespace

void BuildGeneralPage(std::vector<Row>& rows) {
    Config& e = Edit();
    const Config& s = Saved();
    EditExtras& x = EditExtra();
    const EditExtras& xs = SavedExtra();

    rows.push_back(ui::Section(L"Startup"));
    {
        Row r = ui::Toggle(L"autostart", L"Start with Windows",
            L"Starts ProWindows when you sign in. One registry entry under your own "
            L"account - no service, no scheduled task.",
            &x.autostart, &xs.autostart);
        r.fallback = []() -> std::wstring { return L"Off"; };
        rows.push_back(r);
    }
    rows.push_back(ui::Toggle(L"startmin", L"Start in the tray",
        L"Starts without opening this window; ProWindows waits in the notification "
        L"area, already arranging.",
        &e.startMinimized, &s.startMinimized));
    {
        const bool can = SelfIsElevated();
        Row r = ui::Toggle(L"elevauto", L"Start as administrator",
            can ? L"Starts ProWindows with administrator rights at every sign-in, with no "
                  L"prompt, through a scheduled task. Windows that run as administrator can "
                  L"only be arranged by a program that does too."
                : L"Starts ProWindows with administrator rights at every sign-in. Setting "
                  L"this up needs administrator rights itself: choose \"Restart as "
                  L"administrator\" from the tray menu first.",
            &x.elevated, &xs.elevated);
        r.enabled = [can]() { return can; };
        rows.push_back(r);
    }

    rows.push_back(ui::Section(L"Settings file"));
    rows.push_back(ui::Action(L"opencfg", L"The config file",
        L"Every setting here is kept in config.ini, a commented text file that can be "
        L"edited by hand. Opens it in Notepad.",
        L"Open", []() { AppOpenConfigFile(); }));
    rows.push_back(ui::Action(L"opendir", L"The settings folder",
        L"Opens the folder with config.ini, the search index and the logs.",
        L"Open", []() { AppOpenConfigFolder(); }));
    rows.push_back(ui::Action(L"reload", L"Reload from disk",
        L"Reads config.ini again - after editing it by hand - and puts every setting "
        L"here back in step with it.",
        L"Reload", []() {
            if (!OkToDiscard(L"Reload the settings?")) return;
            AppReloadFromDisk();
            SettingsToast(L"Settings reloaded from disk");
        }));

    rows.push_back(ui::Section(L"If something looks wrong"));
    rows.push_back(ui::Action(L"restorewins", L"Show all hidden windows",
        L"Brings back every window hidden by a workspace switch. A window that seems "
        L"to have vanished is usually on another workspace.",
        L"Show", []() { AppRestoreHiddenWindows(); SettingsToast(L"Every hidden window is back"); }));
    rows.push_back(ui::Action(L"diag", L"Diagnostics report",
        L"Writes a report of what ProWindows can see - monitors, windows, shortcuts and "
        L"why any window is being left alone - and opens it.",
        L"Create", []() { AppWriteDiagnostics(); }));
    rows.push_back(ui::Toggle(L"debug", L"Debug log",
        L"Writes a detailed log to the settings folder. Only worth turning on to report "
        L"a problem: it grows quickly.",
        &e.debug, &s.debug));
    {
        Row r = ui::Action(L"restoredefaults", L"Restore every default",
            L"Puts every setting back to how it shipped: shortcuts, excluded apps, the "
            L"monitor's colours, the search folders and the window sizes ProWindows has "
            L"learned. It cannot be undone.",
            L"Restore", []() {
                // One question, asked here: it used to be two when anything
                // was unapplied, and the edits survived both.
                std::wstring body =
                    L"Every setting goes back to how it shipped: keyboard shortcuts, the "
                    L"apps you excluded, the monitor's colours, the search folders, and "
                    L"the window sizes ProWindows has learned.";
                if (EditsPending()) body += L" Changes you have not applied are lost too.";
                body += L"\r\n\r\nThis cannot be undone.";
                if (!ui::Confirm(SettingsHwnd(), L"Restore every default?", body,
                                 L"Restore", L"Cancel", true))
                    return;
                DiscardEdits();
                if (AppRestoreDefaults())
                    SettingsToast(L"Every setting is back to its default");
                else
                    ui::Notice(SettingsHwnd(), L"Could not save",
                               L"The defaults are in effect, but config.ini could not be "
                               L"written, so your old settings come back when ProWindows "
                               L"restarts. Something may have the file open.");
            });
        r.danger = true;
        rows.push_back(r);
    }

    rows.push_back(ui::Section(L"About"));
    const std::vector<std::wstring> about = AboutLines();
    rows.push_back(ui::Info(L"version", L"Version",
        L"The version of this copy. Several copies can live side by side; this is how "
        L"to tell which one is running.",
        []() { return std::wstring(kVersion); }));
    {
        const bool admin = SelfIsElevated();
        rows.push_back(ui::Info(L"rights", L"Running as",
            admin ? L"This copy has administrator rights, so it can arrange every window."
                  : L"This copy runs with ordinary rights. Windows of programs running as "
                    L"administrator cannot be moved by it.",
            [admin]() { return std::wstring(admin ? L"Administrator" : L"Standard user"); }));
    }
    if (about.size() > 1) {
        const std::wstring exe = about[1];
        Row r = ui::Info(L"exe", L"Program", exe, [exe]() { return exe; }, true);
        rows.push_back(r);
    }
    if (about.size() > 2) {
        const std::wstring cfg = about[2];
        Row r = ui::Info(L"cfgpath", L"Settings", cfg, [cfg]() { return cfg; }, true);
        r.activate = []() { AppOpenConfigFolder(); };
        rows.push_back(r);
    }
}

void ResetGeneralPage() {
    Config d;
    d.LoadDefaults();
    Edit().startMinimized = d.startMinimized;
    Edit().debug = d.debug;
}

} // namespace awa
