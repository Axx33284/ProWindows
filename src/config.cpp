#include "config.h"
#include "winutil.h"
#include "montheme.h"
#include "clocktheme.h"

namespace awa {

// ---------------------------------------------------------------- layouts
const wchar_t* LayoutName(LayoutKind k) {
    switch (k) {
        case LayoutKind::Dwindle: return L"Dwindle";
        case LayoutKind::Master:  return L"Master";
        case LayoutKind::Grid:    return L"Grid";
        case LayoutKind::Monocle: return L"Monocle";
        default:                  return L"?";
    }
}

bool ParseLayout(const std::wstring& s, LayoutKind* out) {
    std::wstring v = ToLower(Trim(s));
    if (v == L"dwindle" || v == L"bsp")  { *out = LayoutKind::Dwindle; return true; }
    if (v == L"master"  || v == L"tall") { *out = LayoutKind::Master;  return true; }
    if (v == L"grid")                    { *out = LayoutKind::Grid;    return true; }
    if (v == L"monocle" || v == L"full") { *out = LayoutKind::Monocle; return true; }
    return false;
}

// ---------------------------------------------------------------- key names
struct KeyName { const wchar_t* name; UINT vk; };

static const KeyName kKeyNames[] = {
    { L"left", VK_LEFT }, { L"right", VK_RIGHT }, { L"up", VK_UP }, { L"down", VK_DOWN },
    { L"return", VK_RETURN }, { L"enter", VK_RETURN }, { L"space", VK_SPACE },
    { L"tab", VK_TAB }, { L"escape", VK_ESCAPE }, { L"esc", VK_ESCAPE },
    { L"backspace", VK_BACK }, { L"delete", VK_DELETE }, { L"del", VK_DELETE },
    { L"insert", VK_INSERT }, { L"home", VK_HOME }, { L"end", VK_END },
    { L"pageup", VK_PRIOR }, { L"pgup", VK_PRIOR },
    { L"pagedown", VK_NEXT }, { L"pgdn", VK_NEXT },
    { L"comma", VK_OEM_COMMA }, { L"period", VK_OEM_PERIOD }, { L"dot", VK_OEM_PERIOD },
    { L"minus", VK_OEM_MINUS }, { L"plus", VK_OEM_PLUS }, { L"equal", VK_OEM_PLUS },
    { L"semicolon", VK_OEM_1 }, { L"slash", VK_OEM_2 }, { L"backslash", VK_OEM_5 },
    { L"grave", VK_OEM_3 }, { L"lbracket", VK_OEM_4 }, { L"rbracket", VK_OEM_6 },
    { L"quote", VK_OEM_7 },
};

static bool KeyNameToVk(const std::wstring& raw, UINT* vk) {
    std::wstring k = ToLower(Trim(raw));
    if (k.empty()) return false;

    if (k.size() == 1) {
        wchar_t c = k[0];
        if (c >= L'a' && c <= L'z') { *vk = (UINT)(c - L'a' + 'A'); return true; }
        if (c >= L'0' && c <= L'9') { *vk = (UINT)(c - L'0' + '0'); return true; }
    }
    if (k.size() >= 2 && k[0] == L'f') {
        int n = _wtoi(k.c_str() + 1);
        if (n >= 1 && n <= 24) { *vk = (UINT)(VK_F1 + n - 1); return true; }
    }
    if (k.size() >= 4 && k.compare(0, 3, L"num") == 0) {
        wchar_t c = k[3];
        if (c >= L'0' && c <= L'9') { *vk = (UINT)(VK_NUMPAD0 + (c - L'0')); return true; }
    }
    for (const auto& e : kKeyNames)
        if (k == e.name) { *vk = e.vk; return true; }
    return false;
}

bool ParseKeySpec(const std::wstring& spec, UINT defaultMod, UINT* mods, UINT* vk) {
    *mods = 0;
    *vk = 0;

    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : spec) {
        if (c == L'+') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    if (parts.empty()) return false;

    for (size_t i = 0; i < parts.size(); ++i) {
        std::wstring p = ToLower(Trim(parts[i]));
        if (p.empty()) continue;
        bool last = (i + 1 == parts.size());

        if (!last) {
            if (p == L"$mod" || p == L"mod")                        *mods |= defaultMod;
            else if (p == L"alt")                                   *mods |= MOD_ALT;
            else if (p == L"ctrl" || p == L"control")               *mods |= MOD_CONTROL;
            else if (p == L"shift")                                 *mods |= MOD_SHIFT;
            else if (p == L"win" || p == L"super" || p == L"meta")  *mods |= MOD_WIN;
            else return false;
        } else {
            if (!KeyNameToVk(p, vk)) return false;
        }
    }
    return *vk != 0;
}

static bool ParseModMask(const std::wstring& s, UINT* out) {
    UINT m = 0;
    for (const auto& tok : SplitList(ToLower(Trim(s)), L'+')) {
        if (tok == L"alt")                                     m |= MOD_ALT;
        else if (tok == L"ctrl" || tok == L"control")          m |= MOD_CONTROL;
        else if (tok == L"shift")                              m |= MOD_SHIFT;
        else if (tok == L"win" || tok == L"super" || tok == L"meta") m |= MOD_WIN;
        else return false;
    }
    if (!m) return false;
    *out = m;
    return true;
}

// ---------------------------------------------------------------- action names
bool ParseAction(const std::wstring& name, const std::wstring& argText,
                 Action* act, int* arg, std::wstring* command) {
    std::wstring a = ToLower(Trim(name));
    std::wstring v = ToLower(Trim(argText));
    *arg = 0;
    command->clear();

    if (a == L"launch" || a == L"run" || a == L"exec") {
        // The command keeps its original case and may itself contain commas.
        *command = Trim(argText);
        *act = ACT_LAUNCH;
        return !command->empty();
    }

    auto dirArg = [&](int* out) -> bool {
        if (v == L"left")  { *out = (int)Dir::Left;  return true; }
        if (v == L"right") { *out = (int)Dir::Right; return true; }
        if (v == L"up")    { *out = (int)Dir::Up;    return true; }
        if (v == L"down")  { *out = (int)Dir::Down;  return true; }
        return false;
    };
    auto relArg = [&](int* out) -> bool {
        if (v == L"prev" || v == L"previous" || v == L"-1") { *out = -1; return true; }
        if (v == L"next" || v == L"+1" || v == L"1")        { *out = +1; return true; }
        return false;
    };

    if (a == L"focus")                { *act = ACT_FOCUS_DIR;  return dirArg(arg); }
    if (a == L"swap" || a == L"move") { *act = ACT_SWAP_DIR;   return dirArg(arg); }
    if (a == L"resize")               { *act = ACT_RESIZE_DIR; return dirArg(arg); }
    if (a == L"focusnext")            { *act = ACT_FOCUS_NEXT; return true; }
    if (a == L"focusprev")            { *act = ACT_FOCUS_PREV; return true; }
    if (a == L"workspace")            { *act = ACT_WORKSPACE; *arg = _wtoi(v.c_str()) - 1; return *arg >= 0; }
    if (a == L"movetoworkspace")      { *act = ACT_MOVE_TO_WORKSPACE; *arg = _wtoi(v.c_str()) - 1; return *arg >= 0; }
    if (a == L"cyclelayout")          { *act = ACT_CYCLE_LAYOUT; return true; }
    if (a == L"layout") {
        LayoutKind kind;
        if (!ParseLayout(v, &kind)) return false;
        *act = ACT_SET_LAYOUT;
        *arg = (int)kind;
        return true;
    }
    if (a == L"togglefloat")          { *act = ACT_TOGGLE_FLOAT; return true; }
    if (a == L"togglefullscreen" || a == L"fullscreen") { *act = ACT_TOGGLE_FULLSCREEN; return true; }
    if (a == L"close")                { *act = ACT_CLOSE_WINDOW; return true; }
    if (a == L"toggletiling")         { *act = ACT_TOGGLE_TILING; return true; }
    if (a == L"reload")               { *act = ACT_RELOAD_CONFIG; return true; }
    if (a == L"togglegaps")           { *act = ACT_TOGGLE_GAPS; return true; }
    if (a == L"launcher" || a == L"run") { *act = ACT_LAUNCHER; return true; }
    if (a == L"promote")              { *act = ACT_PROMOTE; return true; }
    if (a == L"focusmonitor")         { *act = ACT_FOCUS_MONITOR; return relArg(arg); }
    if (a == L"movetomonitor")        { *act = ACT_MOVE_TO_MONITOR; return relArg(arg); }
    if (a == L"minimize")             { *act = ACT_MINIMIZE; return true; }
    if (a == L"retile")               { *act = ACT_RETILE; return true; }
    if (a == L"quit" || a == L"exit") { *act = ACT_QUIT; return true; }
    if (a == L"togglesplit")          { *act = ACT_TOGGLE_SPLIT; return true; }
    if (a == L"swapsplit")            { *act = ACT_SWAP_SPLIT; return true; }
    if (a == L"togglesticky" || a == L"sticky" || a == L"pin") {
        *act = ACT_TOGGLE_STICKY; return true;
    }
    if (a == L"movetoscratchpad")     { *act = ACT_SCRATCHPAD_MOVE; return true; }
    if (a == L"scratchpad")           { *act = ACT_SCRATCHPAD_TOGGLE; return true; }
    if (a == L"focuslast" || a == L"focuscurrentorlast") {
        *act = ACT_FOCUS_LAST;
        return true;
    }
    if (a == L"workspacerel" || a == L"nextworkspace") {
        // "workspace, e+1" is how Hyprland spells "the next one that has
        // something on it", so the same spelling means the same thing here.
        *act = ACT_WORKSPACE_REL;
        if (v == L"e+1" || v == L"e-1") *act = ACT_WORKSPACE_USED;
        if (v == L"prev" || v == L"previous" || v == L"-1" || v == L"e-1") *arg = -1;
        else                                                               *arg = +1;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- descriptions
std::wstring DescribeChord(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_WIN)     s += L"Win + ";
    if (mods & MOD_CONTROL) s += L"Ctrl + ";
    if (mods & MOD_ALT)     s += L"Alt + ";
    if (mods & MOD_SHIFT)   s += L"Shift + ";

    // Prefer the friendly name where we have one.
    for (const auto& e : kKeyNames) {
        if (e.vk == vk) {
            std::wstring name = e.name;
            if (!name.empty()) name[0] = (wchar_t)towupper(name[0]);
            return s + name;
        }
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        wchar_t buf[8];
        swprintf_s(buf, L"F%d", (int)(vk - VK_F1 + 1));
        return s + buf;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return s + (wchar_t)vk;

    wchar_t buf[16];
    swprintf_s(buf, L"key %u", vk);
    return s + buf;
}

std::wstring KeySpecText(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_WIN)     s += L"win+";
    if (mods & MOD_CONTROL) s += L"ctrl+";
    if (mods & MOD_ALT)     s += L"alt+";
    if (mods & MOD_SHIFT)   s += L"shift+";

    for (const auto& e : kKeyNames)
        if (e.vk == vk) return s + e.name;

    if (vk >= VK_F1 && vk <= VK_F24) {
        wchar_t buf[8];
        swprintf_s(buf, L"f%d", (int)(vk - VK_F1 + 1));
        return s + buf;
    }
    if (vk >= 'A' && vk <= 'Z') return s + (wchar_t)towlower((wchar_t)vk);
    if (vk >= '0' && vk <= '9') return s + (wchar_t)vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        wchar_t buf[8];
        swprintf_s(buf, L"num%d", (int)(vk - VK_NUMPAD0));
        return s + buf;
    }
    return L"";     // not representable; caller skips it
}

std::wstring ActionSpecText(const Keybind& kb) {
    auto dirName = [&]() -> const wchar_t* {
        switch ((Dir)kb.arg) {
            case Dir::Left:  return L"left";
            case Dir::Right: return L"right";
            case Dir::Up:    return L"up";
            default:         return L"down";
        }
    };
    wchar_t buf[512];

    switch (kb.action) {
        case ACT_FOCUS_DIR:  swprintf_s(buf, L"focus, %s", dirName());  return buf;
        case ACT_SWAP_DIR:   swprintf_s(buf, L"swap, %s", dirName());   return buf;
        case ACT_RESIZE_DIR: swprintf_s(buf, L"resize, %s", dirName()); return buf;
        case ACT_FOCUS_NEXT: return L"focusnext";
        case ACT_FOCUS_PREV: return L"focusprev";
        case ACT_WORKSPACE:
            swprintf_s(buf, L"workspace, %d", kb.arg + 1); return buf;
        case ACT_MOVE_TO_WORKSPACE:
            swprintf_s(buf, L"movetoworkspace, %d", kb.arg + 1); return buf;
        case ACT_CYCLE_LAYOUT: return L"cyclelayout";
        case ACT_SET_LAYOUT:
            swprintf_s(buf, L"layout, %s",
                       ToLower(LayoutName((LayoutKind)kb.arg)).c_str()); return buf;
        case ACT_TOGGLE_FLOAT:      return L"togglefloat";
        case ACT_TOGGLE_FULLSCREEN: return L"togglefullscreen";
        case ACT_CLOSE_WINDOW:      return L"close";
        case ACT_MINIMIZE:          return L"minimize";
        case ACT_PROMOTE:           return L"promote";
        case ACT_TOGGLE_TILING:     return L"toggletiling";
        case ACT_TOGGLE_GAPS:       return L"togglegaps";
        case ACT_LAUNCHER:          return L"launcher";
        case ACT_TOGGLE_SPLIT:      return L"togglesplit";
        case ACT_SWAP_SPLIT:        return L"swapsplit";
        case ACT_TOGGLE_STICKY:     return L"togglesticky";
        case ACT_SCRATCHPAD_MOVE:   return L"movetoscratchpad";
        case ACT_SCRATCHPAD_TOGGLE: return L"scratchpad";
        case ACT_FOCUS_LAST:        return L"focuslast";
        case ACT_WORKSPACE_REL:
            swprintf_s(buf, L"workspacerel, %s", kb.arg < 0 ? L"prev" : L"next"); return buf;
        case ACT_WORKSPACE_USED:
            swprintf_s(buf, L"workspacerel, %s", kb.arg < 0 ? L"e-1" : L"e+1"); return buf;
        case ACT_RELOAD_CONFIG:     return L"reload";
        case ACT_RETILE:            return L"retile";
        case ACT_QUIT:              return L"quit";
        case ACT_FOCUS_MONITOR:
            swprintf_s(buf, L"focusmonitor, %s", kb.arg < 0 ? L"prev" : L"next"); return buf;
        case ACT_MOVE_TO_MONITOR:
            swprintf_s(buf, L"movetomonitor, %s", kb.arg < 0 ? L"prev" : L"next"); return buf;
        case ACT_LAUNCH:
            // Built by concatenation, not swprintf_s: the command is arbitrary
            // user text and overflowing a fixed buffer would abort the process.
            return L"launch, " + kb.command;
        default: return L"";
    }
}

std::wstring DescribeAction(const Keybind& kb) {
    // Written out per direction: "the window to the down" reads badly.
    auto place = [&]() -> const wchar_t* {
        switch ((Dir)kb.arg) {
            case Dir::Left:  return L"on the left";
            case Dir::Right: return L"on the right";
            case Dir::Up:    return L"above";
            default:         return L"below";
        }
    };
    auto dir = [&]() -> const wchar_t* {
        switch ((Dir)kb.arg) {
            case Dir::Left:  return L"left";
            case Dir::Right: return L"right";
            case Dir::Up:    return L"up";
            default:         return L"down";
        }
    };
    wchar_t buf[160];

    switch (kb.action) {
        case ACT_FOCUS_DIR:
            swprintf_s(buf, L"Focus the window %s", place());        return buf;
        case ACT_SWAP_DIR:
            swprintf_s(buf, L"Move this window %s", dir());          return buf;
        case ACT_RESIZE_DIR:
            swprintf_s(buf, L"Move the dividing line %s", dir());    return buf;
        case ACT_FOCUS_NEXT:        return L"Focus the next window";
        case ACT_FOCUS_PREV:        return L"Focus the previous window";
        case ACT_WORKSPACE:
            swprintf_s(buf, L"Switch to workspace %d", kb.arg + 1);  return buf;
        case ACT_MOVE_TO_WORKSPACE:
            swprintf_s(buf, L"Send this window to workspace %d", kb.arg + 1); return buf;
        case ACT_CYCLE_LAYOUT:      return L"Switch to the next layout";
        case ACT_SET_LAYOUT:
            swprintf_s(buf, L"Use the %s layout", LayoutName((LayoutKind)kb.arg)); return buf;
        case ACT_TOGGLE_FLOAT:      return L"Float / unfloat this window";
        case ACT_TOGGLE_FULLSCREEN: return L"Make this window fullscreen";
        case ACT_CLOSE_WINDOW:      return L"Close this window";
        case ACT_MINIMIZE:          return L"Minimise this window";
        case ACT_PROMOTE:           return L"Promote to the main slot";
        case ACT_TOGGLE_TILING:     return L"Pause / resume arranging";
        case ACT_TOGGLE_GAPS:       return L"Show / hide the gaps";
        case ACT_LAUNCHER:          return L"Open the app launcher";
        case ACT_FOCUS_MONITOR:
            return kb.arg < 0 ? L"Focus the previous monitor" : L"Focus the next monitor";
        case ACT_MOVE_TO_MONITOR:
            return kb.arg < 0 ? L"Send window to the previous monitor"
                              : L"Send window to the next monitor";
        case ACT_TOGGLE_SPLIT:      return L"Flip this split: side by side / stacked";
        case ACT_SWAP_SPLIT:        return L"Swap the two halves of this split";
        case ACT_TOGGLE_STICKY:     return L"Keep this window on every workspace";
        case ACT_SCRATCHPAD_MOVE:   return L"Send this window to the scratchpad";
        case ACT_SCRATCHPAD_TOGGLE: return L"Show or hide the scratchpad window";
        case ACT_FOCUS_LAST:        return L"Back to the last window (and back again)";
        case ACT_WORKSPACE_REL:
            return kb.arg < 0 ? L"Switch to the previous workspace"
                              : L"Switch to the next workspace";
        case ACT_WORKSPACE_USED:
            return kb.arg < 0 ? L"Previous workspace that has windows on it"
                              : L"Next workspace that has windows on it";
        case ACT_RELOAD_CONFIG:     return L"Reload settings from disk";
        case ACT_RETILE:            return L"Rescan windows and re-arrange";
        case ACT_QUIT:              return L"Quit ProWindows";
        case ACT_LAUNCH:
            return L"Open  " + kb.command;   // see ActionSpecText: never a fixed buffer
        default:                    return L"-";
    }
}

// ---------------------------------------------------------------- defaults
struct DefaultBind { const wchar_t* spec; Action act; int arg; };

static const DefaultBind kDefaultBinds[] = {
    // focus
    { L"$mod+h",            ACT_FOCUS_DIR, (int)Dir::Left  },
    { L"$mod+j",            ACT_FOCUS_DIR, (int)Dir::Down  },
    { L"$mod+k",            ACT_FOCUS_DIR, (int)Dir::Up    },
    { L"$mod+l",            ACT_FOCUS_DIR, (int)Dir::Right },
    { L"$mod+left",         ACT_FOCUS_DIR, (int)Dir::Left  },
    { L"$mod+down",         ACT_FOCUS_DIR, (int)Dir::Down  },
    { L"$mod+up",           ACT_FOCUS_DIR, (int)Dir::Up    },
    { L"$mod+right",        ACT_FOCUS_DIR, (int)Dir::Right },
    { L"$mod+o",            ACT_FOCUS_NEXT, 0 },
    { L"$mod+i",            ACT_FOCUS_PREV, 0 },
    // move / swap
    { L"$mod+shift+h",      ACT_SWAP_DIR, (int)Dir::Left  },
    { L"$mod+shift+j",      ACT_SWAP_DIR, (int)Dir::Down  },
    { L"$mod+shift+k",      ACT_SWAP_DIR, (int)Dir::Up    },
    { L"$mod+shift+l",      ACT_SWAP_DIR, (int)Dir::Right },
    { L"$mod+shift+left",   ACT_SWAP_DIR, (int)Dir::Left  },
    { L"$mod+shift+down",   ACT_SWAP_DIR, (int)Dir::Down  },
    { L"$mod+shift+up",     ACT_SWAP_DIR, (int)Dir::Up    },
    { L"$mod+shift+right",  ACT_SWAP_DIR, (int)Dir::Right },
    // resize
    { L"$mod+ctrl+h",       ACT_RESIZE_DIR, (int)Dir::Left  },
    { L"$mod+ctrl+j",       ACT_RESIZE_DIR, (int)Dir::Down  },
    { L"$mod+ctrl+k",       ACT_RESIZE_DIR, (int)Dir::Up    },
    { L"$mod+ctrl+l",       ACT_RESIZE_DIR, (int)Dir::Right },
    { L"$mod+ctrl+left",    ACT_RESIZE_DIR, (int)Dir::Left  },
    { L"$mod+ctrl+down",    ACT_RESIZE_DIR, (int)Dir::Down  },
    { L"$mod+ctrl+up",      ACT_RESIZE_DIR, (int)Dir::Up    },
    { L"$mod+ctrl+right",   ACT_RESIZE_DIR, (int)Dir::Right },
    // window state
    { L"$mod+v",            ACT_TOGGLE_FLOAT, 0 },
    { L"$mod+f",            ACT_TOGGLE_FULLSCREEN, 0 },
    { L"$mod+q",            ACT_CLOSE_WINDOW, 0 },
    { L"$mod+n",            ACT_MINIMIZE, 0 },
    { L"$mod+return",       ACT_PROMOTE, 0 },
    // layout
    { L"$mod+space",        ACT_CYCLE_LAYOUT, 0 },
    { L"$mod+g",            ACT_TOGGLE_GAPS, 0 },
    // Reshaping the split in front of you, rather than the whole layout.
    // Hyprland puts togglesplit on $mod+J, which is taken here by "focus
    // down"; $mod+E and $mod+shift+E are free on both this default modifier
    // and Win, and sit next to each other on the keyboard.
    { L"$mod+e",            ACT_TOGGLE_SPLIT, 0 },
    { L"$mod+shift+e",      ACT_SWAP_SPLIT, 0 },
    // Alt+Tab that stays inside the tiling. Tab itself belongs to the shell
    // and is refused by RegisterHotKey; grave is the other key everybody's
    // hand already knows for "the last one".
    { L"$mod+grave",        ACT_FOCUS_LAST, 0 },
    // Workspace by step. Hyprland's e+1 / e-1 - skip the empty ones, so this
    // walks what you are actually using rather than counting to nine.
    { L"$mod+ctrl+period",  ACT_WORKSPACE_USED, +1 },
    { L"$mod+ctrl+comma",   ACT_WORKSPACE_USED, -1 },
    // The launcher, on two chords deliberately. R for "run", which is what
    // Hyprland users reach for; Win+S because that is where Windows itself
    // puts search, so it is already in everybody's fingers. Win+S is a shell
    // chord, so it only works through the keyboard hook - see overrideReserved.
    { L"$mod+r",            ACT_LAUNCHER, 0 },
    { L"win+s",             ACT_LAUNCHER, 0 },
    { L"$mod+p",            ACT_TOGGLE_TILING, 0 },
    // F5 rather than R: R is commonly taken by other software, and F5 already
    // means "refresh" everywhere else.
    { L"$mod+f5",           ACT_RELOAD_CONFIG, 0 },
    { L"$mod+shift+f5",     ACT_RETILE, 0 },
    // monitors
    { L"$mod+comma",        ACT_FOCUS_MONITOR, -1 },
    { L"$mod+period",       ACT_FOCUS_MONITOR, +1 },
    { L"$mod+shift+comma",  ACT_MOVE_TO_MONITOR, -1 },
    { L"$mod+shift+period", ACT_MOVE_TO_MONITOR, +1 },
};

void Config::LoadDefaults() {
    binds.clear();
    for (const auto& d : kDefaultBinds) {
        Keybind kb;
        if (!ParseKeySpec(d.spec, modMask, &kb.mods, &kb.vk)) continue;
        kb.action = d.act;
        kb.arg    = d.arg;
        kb.spec   = d.spec;
        binds.push_back(kb);
    }

    for (int i = 0; i < workspaceCount && i < 9; ++i) {
        wchar_t num[8];
        swprintf_s(num, L"%d", i + 1);

        Keybind go;
        go.mods   = modMask;
        go.vk     = (UINT)(L'0' + i + 1);
        go.action = ACT_WORKSPACE;
        go.arg    = i;
        go.spec   = std::wstring(L"$mod+") + num;
        binds.push_back(go);

        Keybind mv = go;
        mv.mods  |= MOD_SHIFT;
        mv.action = ACT_MOVE_TO_WORKSPACE;
        mv.spec   = std::wstring(L"$mod+shift+") + num;
        binds.push_back(mv);
    }

    // App launchers. The Windows shell owns these chords, so they only take
    // effect while "override_windows_shortcuts" is on.
    {
        wchar_t found[MAX_PATH];
        const bool haveTerminal =
            SearchPathW(nullptr, L"wt.exe", nullptr, MAX_PATH, found, nullptr) > 0;

        // Whatever the user actually browses with, rather than a guess.
        std::wstring browser = DefaultBrowserPath();
        if (browser.empty()) {
            const wchar_t* candidates[] = { L"brave.exe", L"chrome.exe",
                                            L"firefox.exe", L"msedge.exe" };
            for (const wchar_t* c : candidates) {
                if (SearchPathW(nullptr, c, nullptr, MAX_PATH, found, nullptr) > 0) {
                    browser = c;
                    break;
                }
            }
        }

        struct LaunchDefault { const wchar_t* spec; std::wstring command; };
        std::vector<LaunchDefault> launchers = {
            { L"win+e", L"explorer.exe" },
            { L"win+f", L"explorer.exe" },
            { L"win+q", haveTerminal ? L"wt.exe" : L"powershell.exe" },
        };
        if (!browser.empty()) launchers.push_back({ L"win+b", browser });

        for (const auto& l : launchers) {
            Keybind kb;
            if (!ParseKeySpec(l.spec, modMask, &kb.mods, &kb.vk)) continue;
            kb.action  = ACT_LAUNCH;
            kb.command = l.command;
            kb.spec    = l.spec;
            binds.push_back(kb);
        }
    }

    // The built-in exclusion lists are separate (see BuiltinIgnore*) so these
    // stay purely what the user asked for.
    ignoreProcess.clear();
    ignoreClass.clear();
    ignoreTitle.clear();
    floatProcess.clear();
    floatClass.clear();
    floatTitle.clear();
    // Empty means "the shell's own Desktop, Documents, Downloads..." - see the
    // comment on Config::searchFolders.
    searchFolders.clear();
}

const std::vector<std::wstring>& BuiltinIgnoreClass() {
    static const std::vector<std::wstring> v = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"Windows.UI.Core.CoreWindow", L"TaskListThumbnailWnd", L"ForegroundStaging",
        L"MultitaskingViewFrame", L"XamlExplorerHostIslandWindow",
        L"Xaml_WindowedPopupClass", L"TopLevelWindowForOverflowXamlIsland",
        L"Windows.Internal.Shell.TabProxyWindow", L"tooltips_class32",
        L"SysShadow", L"Button", L"EdgeUiInputTopWndClass", L"NarratorHelperWindow",
        L"ApplicationManager_ImmersiveShellWindow",
        L"Windows.UI.Composition.DesktopWindowContentBridge",
    };
    return v;
}

const std::vector<std::wstring>& BuiltinIgnoreProcess() {
    static const std::vector<std::wstring> v = {
        L"SearchHost.exe", L"StartMenuExperienceHost.exe", L"ShellExperienceHost.exe",
        L"TextInputHost.exe", L"PeopleExperienceHost.exe", L"LockApp.exe",
        L"SearchUI.exe", L"WidgetBoard.exe", L"Widgets.exe",
    };
    return v;
}

const std::vector<std::wstring>& BuiltinFloatProcess() {
    static const std::vector<std::wstring> v = {
        L"Taskmgr.exe", L"SnippingTool.exe", L"ScreenSketch.exe",
    };
    return v;
}

// Transient windows that should never be given a tile. A file-copy progress
// dialog is the case that prompted this: it is top-level, titled and has a
// resize frame, so it looked tileable, but it keeps its own small size and the
// slot reserved for it stayed mostly empty until it went away again.
const std::vector<std::wstring>& BuiltinFloatClass() {
    static const std::vector<std::wstring> v = {
        L"#32770",                        // the standard Win32 dialog class
        L"OperationStatusWindow",         // copying / moving / deleting files
        L"Progress",
        L"ProgressWnd",
        L"MsoSplash",
        L"CredentialDialogXamlHost",      // the UAC-style credential prompt
        L"Windows.UI.Popups.PopupWindow",
    };
    return v;
}

const std::vector<std::wstring>& BuiltinFloatTitle() {
    static const std::vector<std::wstring> v = { L"Picture-in-Picture" };
    return v;
}

// ---------------------------------------------------------------- ini parsing
static bool ParseBool(const std::wstring& v, bool def) {
    std::wstring s = ToLower(Trim(v));
    if (s == L"1" || s == L"true"  || s == L"yes" || s == L"on")  return true;
    if (s == L"0" || s == L"false" || s == L"no"  || s == L"off") return false;
    return def;
}

static bool ParseColor(const std::wstring& v, COLORREF* out) {
    std::wstring s = Trim(v);
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6) return false;
    wchar_t* end = nullptr;
    unsigned long rgb = wcstoul(s.c_str(), &end, 16);
    if (end && *end) return false;
    *out = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    return true;
}

static bool SplitKV(const std::wstring& line, std::wstring* k, std::wstring* v) {
    std::wstring l = Trim(line);
    if (l.empty() || l[0] == L'#' || l[0] == L';' || l[0] == L'[') return false;
    size_t eq = l.find(L'=');
    if (eq == std::wstring::npos) return false;
    *k = ToLower(Trim(l.substr(0, eq)));
    *v = Trim(l.substr(eq + 1));
    return true;
}

bool Config::LoadFromFile(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") != 0 || !f) return false;

    std::vector<std::wstring> lines;
    wchar_t buf[2048];
    while (fgetws(buf, (int)(sizeof(buf) / sizeof(buf[0])), f)) lines.push_back(buf);
    fclose(f);

    std::wstring k, v;

    // Pass 1: resolve "mod" and "workspaces" first, so $mod works regardless of
    // where those lines appear in the file.
    for (const auto& line : lines) {
        if (!SplitKV(line, &k, &v)) continue;
        if (k == L"mod") {
            UINT m;
            if (ParseModMask(v, &m)) modMask = m;
        } else if (k == L"workspaces") {
            int n = _wtoi(v.c_str());
            if (n >= 1 && n <= 9) workspaceCount = n;
        }
    }

    // Rebuild the default binds against the resolved mod / workspace count.
    LoadDefaults();

    bool clearedBinds = false;
    int  fileVersion = 1;          // absent means "before versioning"
    for (const auto& line : lines) {
        if (!SplitKV(line, &k, &v)) continue;

        if      (k == L"gap_inner" || k == L"inner_gap") gapInner = (std::max)(0, _wtoi(v.c_str()));
        else if (k == L"gap_outer" || k == L"outer_gap") gapOuter = (std::max)(0, _wtoi(v.c_str()));
        else if (k == L"accent_border")   accentBorder = ParseBool(v, accentBorder);
        else if (k == L"active_color")    ParseColor(v, &activeColor);
        else if (k == L"inactive_color")  ParseColor(v, &inactiveColor);
        else if (k == L"corners") {
            std::wstring s = ToLower(Trim(v));
            if (s == L"round")                       cornerPref = 2;
            else if (s == L"square" || s == L"sharp") cornerPref = 1;
            else                                      cornerPref = 0;
        }
        else if (k == L"layout")         ParseLayout(v, &layout);
        else if (k == L"master_ratio") {
            float r = (float)_wtof(v.c_str());
            if (r > 0.1f && r < 0.9f) masterRatio = r;
        }
        else if (k == L"master_count")   masterCount = (std::max)(1, _wtoi(v.c_str()));
        else if (k == L"tiling_enabled") tilingEnabled = ParseBool(v, tilingEnabled);
        else if (k == L"focus_follows_mouse") focusFollowsMouse = ParseBool(v, focusFollowsMouse);
        else if (k == L"new_window_on_top")   newWindowOnTop = ParseBool(v, newWindowOnTop);
        // `swap_on_drag` is what this was called when dragging could only
        // exchange two windows. Still read, so nobody's config loses the
        // setting; only the new name is written back.
        else if (k == L"drag_to_rearrange" || k == L"swap_on_drag")
            dragToRearrange = ParseBool(v, dragToRearrange);
        else if (k == L"animations")     animations = ParseBool(v, animations);
        else if (k == L"animation_ms")   animationMs = (std::max)(30, (std::min)(600, _wtoi(v.c_str())));
        else if (k == L"resize_step")    resizeStep = (std::max)(1, (std::min)(25, _wtoi(v.c_str())));
        else if (k == L"manage_minimized") manageMinimized = ParseBool(v, manageMinimized);
        else if (k == L"pause_for_fullscreen")
            pauseForFullscreen = ParseBool(v, pauseForFullscreen);
        else if (k == L"learned") {
            // learned = <process|class>, minW, minH, maxW, maxH[, nofit]
            //
            // Written by the tiler, not by hand: these are the sizes an
            // application turned out to insist on, recorded so the next run
            // does not have to rediscover them by overlapping windows first.
            const std::vector<std::wstring> parts = SplitList(v);
            if (parts.size() >= 5 && learnedLimits.size() < kMaxLearnedLimits) {
                RememberedLimits lim;
                lim.minW = (std::max)(0, _wtoi(parts[1].c_str()));
                lim.minH = (std::max)(0, _wtoi(parts[2].c_str()));
                lim.maxW = (std::max)(0, _wtoi(parts[3].c_str()));
                lim.maxH = (std::max)(0, _wtoi(parts[4].c_str()));
                lim.tooLarge = parts.size() >= 6 && IEquals(parts[5], L"nofit");
                // A recorded limit larger than any plausible display is a
                // corrupt line, not a limit; drop it rather than let it
                // dictate the layout.
                if (lim.minW < 30000 && lim.minH < 30000)
                    learnedLimits[ToLower(parts[0])] = lim;
            }
        }
        else if (k == L"start_minimized")  startMinimized = ParseBool(v, startMinimized);
        else if (k == L"margin_top")       marginTop    = (std::max)(0, _wtoi(v.c_str()));
        else if (k == L"margin_bottom")    marginBottom = (std::max)(0, _wtoi(v.c_str()));
        else if (k == L"margin_left")      marginLeft   = (std::max)(0, _wtoi(v.c_str()));
        else if (k == L"margin_right")     marginRight  = (std::max)(0, _wtoi(v.c_str()));
        else if (k == L"monitor_enabled")  monitorEnabled = ParseBool(v, monitorEnabled);
        else if (k == L"monitor_x")        monitorX = _wtoi(v.c_str());
        else if (k == L"monitor_y")        monitorY = _wtoi(v.c_str());
        else if (k == L"monitor_pinned")   monitorPinned = ParseBool(v, monitorPinned);
        else if (k == L"monitor_opacity")  monitorOpacity = (std::max)(20, (std::min)(100, _wtoi(v.c_str())));
        else if (k == L"monitor_scale")    monitorScale = (std::max)(70, (std::min)(200, _wtoi(v.c_str())));
        else if (k == L"monitor_interval") monitorInterval = (std::max)(250, (std::min)(5000, _wtoi(v.c_str())));
        else if (k == L"monitor_vertical") monitorVertical = ParseBool(v, monitorVertical);
        else if (k == L"monitor_graphs")   monitorGraphs = ParseBool(v, monitorGraphs);
        else if (k == L"monitor_theme") {
            // Stored by name so the file stays readable and re-ordering the
            // table cannot silently change somebody's colours.
            const int found = MonitorSkinIndexById(Trim(v));
            if (found >= 0) monitorTheme = found;
        }
        else if (k == L"monitor_style") {
            const int found = MonitorStyleIndexById(Trim(v));
            if (found >= 0) monitorStyle = found;
        }
        else if (k == L"monitor_on_desktop") monitorOnDesktop = ParseBool(v, monitorOnDesktop);
        else if (k == L"monitor_top_apps")   monitorTopApps = ParseBool(v, monitorTopApps);
        else if (k == L"config_version")     fileVersion = _wtoi(v.c_str());
        else if (k == L"monitor_cores")    monitorCores = ParseBool(v, monitorCores);
        else if (k == L"monitor_smooth")     monitorSmooth = ParseBool(v, monitorSmooth);
        else if (k == L"monitor_order") {
            // "cpu, gpu, ram" - by name, like monitor_theme, and repaired
            // rather than rejected: whatever the line leaves out is appended
            // in the declared order. See MonitorNormaliseOrder.
            int order[MON_METRIC_COUNT];
            int n = 0;
            for (const auto& tok : SplitList(v)) {
                const int m = MonitorMetricIndexById(tok);
                if (m >= 0 && n < MON_METRIC_COUNT) order[n++] = m;
            }
            while (n < MON_METRIC_COUNT) order[n++] = -1;   // filled in below
            MonitorNormaliseOrder(order);
            for (int i = 0; i < MON_METRIC_COUNT; ++i) monOrder[i] = order[i];
        }
        else if (k.compare(0, 14, L"monitor_color_") == 0) {
            // monitor_color_<metric> = #RRGGBB, or "theme" to drop the override.
            const int index = MonitorMetricIndexById(k.substr(14));
            if (index >= 0) {
                COLORREF c = 0;
                if (ParseColor(v, &c)) monColor[index] = c;
                else                   monColor[index] = kMonColourFromTheme;
            }
        }
        else if (k == L"clock_enabled")    clockEnabled = ParseBool(v, clockEnabled);
        else if (k == L"clock_x")          clockX = _wtoi(v.c_str());
        else if (k == L"clock_y")          clockY = _wtoi(v.c_str());
        else if (k == L"clock_pinned")     clockPinned = ParseBool(v, clockPinned);
        else if (k == L"clock_on_desktop") clockOnDesktop = ParseBool(v, clockOnDesktop);
        else if (k == L"clock_opacity")    clockOpacity = (std::max)(20, (std::min)(100, _wtoi(v.c_str())));
        else if (k == L"clock_scale")      clockScale = (std::max)(50, (std::min)(250, _wtoi(v.c_str())));
        else if (k == L"clock_theme") {
            const int found = ClockSkinIndexById(Trim(v));
            if (found >= 0) clockTheme = found;
        }
        else if (k == L"clock_style") {
            const int found = ClockStyleIndexById(Trim(v));
            if (found >= 0) clockStyle = found;
        }
        else if (k == L"clock_24h")        clockHours24 = ParseBool(v, clockHours24);
        else if (k == L"clock_seconds")    clockSeconds = ParseBool(v, clockSeconds);
        else if (k == L"clock_date")       clockDate = ParseBool(v, clockDate);
        else if (k == L"clock_weekday")    clockWeekday = ParseBool(v, clockWeekday);
        else if (k == L"monitor_cpu")      monShowCpu = ParseBool(v, monShowCpu);
        else if (k == L"monitor_ram")      monShowRam = ParseBool(v, monShowRam);
        else if (k == L"monitor_gpu")      monShowGpu = ParseBool(v, monShowGpu);
        else if (k == L"monitor_vram")     monShowVram = ParseBool(v, monShowVram);
        // `monitor_temp` is what the single temperature row was called before
        // it became one per chip. Still read, so nobody's overlay loses it.
        else if (k == L"monitor_temp" || k == L"monitor_cpu_temp")
            monShowCpuTemp = ParseBool(v, monShowCpuTemp);
        else if (k == L"monitor_gpu_temp") monShowGpuTemp = ParseBool(v, monShowGpuTemp);
        else if (k == L"monitor_disk")     monShowDisk = ParseBool(v, monShowDisk);
        else if (k == L"monitor_net")      monShowNet = ParseBool(v, monShowNet);
        else if (k == L"search_files")     searchFiles = ParseBool(v, searchFiles);
        else if (k == L"search_programs")  searchPrograms = ParseBool(v, searchPrograms);
        else if (k == L"search_drives")    searchDrives = ParseBool(v, searchDrives);
        else if (k == L"search_deep_exe")  searchDeepExe = ParseBool(v, searchDeepExe);
        else if (k == L"search_max_programs")
            searchMaxPrograms = (std::max)(100, (std::min)(100000, _wtoi(v.c_str())));
        else if (k == L"search_commands")  searchCommands = ParseBool(v, searchCommands);
        else if (k == L"search_calc")      searchCalc = ParseBool(v, searchCalc);
        else if (k == L"search_settings")  searchSettings = ParseBool(v, searchSettings);
        else if (k == L"search_hidden")    searchHidden = ParseBool(v, searchHidden);
        else if (k == L"search_depth")
            searchDepth = (std::max)(1, (std::min)(12, _wtoi(v.c_str())));
        else if (k == L"search_max_entries")
            searchMaxEntries = (std::max)(100, (std::min)(200000, _wtoi(v.c_str())));
        else if (k == L"search_folders") { for (auto& s : SplitList(v)) searchFolders.push_back(s); }
        else if (k == L"override_windows_shortcuts")
            overrideReserved = ParseBool(v, overrideReserved);
        else if (k == L"mod_drag")       modDrag = ParseBool(v, modDrag);
        else if (k == L"smart_gaps")     smartGaps = ParseBool(v, smartGaps);
        else if (k == L"cursor_warp")    cursorWarp = ParseBool(v, cursorWarp);
        else if (k == L"debug")          debug = ParseBool(v, debug);
        else if (k == L"ignore_process") { for (auto& s : SplitList(v)) ignoreProcess.push_back(s); }
        else if (k == L"ignore_class")   { for (auto& s : SplitList(v)) ignoreClass.push_back(s); }
        else if (k == L"ignore_title")   { for (auto& s : SplitList(v)) ignoreTitle.push_back(s); }
        else if (k == L"float_process")  { for (auto& s : SplitList(v)) floatProcess.push_back(s); }
        else if (k == L"float_class")    { for (auto& s : SplitList(v)) floatClass.push_back(s); }
        else if (k == L"float_title")    { for (auto& s : SplitList(v)) floatTitle.push_back(s); }
        else if (k == L"clear_binds") {
            if (ParseBool(v, false) && !clearedBinds) {
                binds.clear();
                clearedBinds = true;
            }
        }
        else if (k == L"unbind") {
            UINT m, vk;
            if (ParseKeySpec(v, modMask, &m, &vk)) {
                binds.erase(std::remove_if(binds.begin(), binds.end(),
                    [&](const Keybind& b) { return b.mods == m && b.vk == vk; }),
                    binds.end());
            }
        }
        else if (k == L"bind") {
            // Split into at most three fields: a launch command is free to
            // contain commas of its own, so it must not be split further.
            const size_t c1 = v.find(L',');
            if (c1 == std::wstring::npos) {
                AWA_LOG(L"config: incomplete bind '%s'", v.c_str());
                continue;
            }
            const size_t c2 = v.find(L',', c1 + 1);

            const std::wstring keyText = Trim(v.substr(0, c1));
            const std::wstring actText = Trim(c2 == std::wstring::npos
                                              ? v.substr(c1 + 1)
                                              : v.substr(c1 + 1, c2 - c1 - 1));
            const std::wstring argText = (c2 == std::wstring::npos)
                                         ? L"" : Trim(v.substr(c2 + 1));

            Keybind kb;
            if (!ParseKeySpec(keyText, modMask, &kb.mods, &kb.vk)) {
                AWA_LOG(L"config: unknown key '%s'", keyText.c_str());
                continue;
            }
            if (!ParseAction(actText, argText, &kb.action, &kb.arg, &kb.command)) {
                AWA_LOG(L"config: unknown action '%s'", actText.c_str());
                continue;
            }
            kb.spec = keyText;
            binds.erase(std::remove_if(binds.begin(), binds.end(),
                [&](const Keybind& b) { return b.mods == kb.mods && b.vk == kb.vk; }),
                binds.end());
            binds.push_back(kb);
        }
    }

    // Bindings added since this file was written. The file is otherwise the
    // whole truth about bindings, so this is the one deliberate exception:
    // it runs once, only for chords the file has not already claimed, and the
    // next save writes the result out explicitly like everything else.
    if (fileVersion < kConfigVersion) {
        // `onlyIfUnbound` is the usual rule: an action that already has a key
        // does not want a second one imposed on it. Win+S is the exception -
        // it is a second, more familiar way to reach a launcher that most
        // existing configs already have on $mod+r, so it is added regardless.
        struct AddedBind { const wchar_t* spec; Action act; bool onlyIfUnbound; };
        static const AddedBind kAdded[] = {
            { L"$mod+r", ACT_LAUNCHER, true  },   // added in version 2
            { L"win+s",  ACT_LAUNCHER, false },   // added in version 3
        };

        for (const auto& add : kAdded) {
            if (add.onlyIfUnbound) {
                bool haveAction = false;
                for (const auto& b : binds)
                    if (b.action == add.act) { haveAction = true; break; }
                if (haveAction) continue;
            }

            Keybind kb;
            if (!ParseKeySpec(add.spec, modMask, &kb.mods, &kb.vk)) continue;

            // Never steal a chord the user has already put to work.
            bool taken = false;
            for (const auto& b : binds)
                if (b.mods == kb.mods && b.vk == kb.vk) { taken = true; break; }
            if (taken) {
                AWA_LOG(L"config: '%s' is taken, leaving the new action unbound",
                        add.spec);
                continue;
            }

            kb.action = add.act;
            kb.spec   = add.spec;
            binds.push_back(kb);
            AWA_LOG(L"config: added the new default binding %s", add.spec);
        }
    }

    return true;
}

} // namespace awa
