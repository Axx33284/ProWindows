// Writing config.ini back out. The settings window edits values in memory and
// then calls SaveToFile, so the file on disk always matches the UI.
#include "config.h"
#include "montheme.h"
#include "clocktheme.h"

namespace awa {

// Trailing reference block. Keybindings stay a text-file feature - the settings
// window covers the modifier key, and this documents the rest.
static const wchar_t* kBindingReference = LR"AWA(
# ---------------------------------------------------------------------------
#  Custom key bindings
# ---------------------------------------------------------------------------
#  Syntax:  bind = <keys>, <action>[, <argument>]
#
#  $mod stands for the modifier chosen above. Combine with + :
#      $mod+shift+return, alt+f4, ctrl+win+left, f9
#
#  Actions:
#      focus left|right|up|down        swap left|right|up|down
#      resize left|right|up|down       focusnext / focusprev
#      workspace <1-9>                 movetoworkspace <1-9>
#      cyclelayout                     layout dwindle|master|grid
#      togglefloat                     togglefullscreen
#      close                           minimize
#      launcher                        promote
#                                      toggletiling
#      togglegaps                      reload
#      retile                          quit
#      focusmonitor prev|next          movetomonitor prev|next
#      togglesplit                     swapsplit
#      focuslast                       workspacerel prev|next|e-1|e+1
#
#  togglesplit flips the split the focused window sits under between side by
#  side and stacked; swapsplit exchanges its two halves. focuslast goes back to
#  the window you were on before, and again to come back. workspacerel steps
#  through workspaces, and the e-1 / e+1 forms skip the empty ones.
#
#  Examples:
#      bind   = $mod+shift+return, promote
#      bind   = f9, toggletiling
#      unbind = $mod+q
#
#  Add "clear_binds = true" to drop every built-in binding first.
#  Lines you add below are kept when the settings window saves this file.
# ---------------------------------------------------------------------------
)AWA";

static const wchar_t* ModMaskText(UINT m) {
    // Written in the same order ParseModMask accepts.
    static wchar_t buf[64];
    buf[0] = 0;
    if (m & MOD_WIN)     wcscat_s(buf, L"+win");
    if (m & MOD_CONTROL) wcscat_s(buf, L"+ctrl");
    if (m & MOD_ALT)     wcscat_s(buf, L"+alt");
    if (m & MOD_SHIFT)   wcscat_s(buf, L"+shift");
    return buf[0] ? buf + 1 : L"alt";
}

static const wchar_t* Bool(bool b) { return b ? L"true" : L"false"; }

static std::wstring JoinList(const std::vector<std::wstring>& v) {
    std::wstring out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += L", ";
        out += v[i];
    }
    return out;
}

bool Config::SaveToFile(const std::wstring& path) const {
    // Written beside the real file and moved into place at the end. Opening the
    // config itself for writing truncates it first, so anything that stopped
    // the process midway - a crash, a power cut, the user ending the task -
    // left a half-written file, and the next launch quietly fell back to the
    // defaults with every binding gone. The move is the only step that can be
    // observed from outside, and it either happened or it did not.
    const std::wstring temp = path + L".new";

    FILE* f = nullptr;
    if (_wfopen_s(&f, temp.c_str(), L"w, ccs=UTF-8") != 0 || !f) {
        AWA_LOG(L"config: could not open %s for writing (error %lu)", temp.c_str(), GetLastError());
        return false;
    }

    fwprintf(f,
        L"# ===========================================================================\n"
        L"#  ProWindows - configuration\n"
        L"#\n"
        L"#  Most of this is editable from the settings window (tray icon -> Settings).\n"
        L"#  Saving there rewrites this file, keeping any custom bindings at the end.\n"
        L"#  After editing by hand, press %s+F5 or use Reload in the tray menu.\n"
        L"# ===========================================================================\n\n",
        (modMask & MOD_WIN) ? L"Win" : (modMask & MOD_CONTROL) ? L"Ctrl" : L"Alt");

    fwprintf(f, L"# ------------------------------------------------------------ keys\n");
    fwprintf(f, L"# The modifier every default binding is built on: alt, ctrl, shift, win.\n");
    fwprintf(f, L"# Note that Windows reserves several Win+<key> chords (Win+L especially)\n");
    fwprintf(f, L"# and those bindings will be skipped.\n");
    fwprintf(f, L"# Written by this version; do not edit. Lets a later release add a\n");
    fwprintf(f, L"# binding for an action that did not exist when you made this file.\n");
    fwprintf(f, L"config_version = %d\n\n", Config::kConfigVersion);
    fwprintf(f, L"mod        = %s\n", ModMaskText(modMask));
    fwprintf(f, L"workspaces = %d\n\n", workspaceCount);

    fwprintf(f, L"# ------------------------------------------------------------ look\n");
    fwprintf(f, L"gap_inner = %d\n", gapInner);
    fwprintf(f, L"gap_outer = %d\n\n", gapOuter);
    fwprintf(f, L"# Coloured DWM border around the focused window (Windows 11 only).\n");
    fwprintf(f, L"accent_border  = %s\n", Bool(accentBorder));
    fwprintf(f, L"active_color   = #%02X%02X%02X\n",
             GetRValue(activeColor), GetGValue(activeColor), GetBValue(activeColor));
    fwprintf(f, L"inactive_color = #%02X%02X%02X\n\n",
             GetRValue(inactiveColor), GetGValue(inactiveColor), GetBValue(inactiveColor));
    fwprintf(f, L"# Window corners: default | round | square\n");
    fwprintf(f, L"corners = %s\n\n",
             cornerPref == 1 ? L"square" : cornerPref == 2 ? L"round" : L"default");

    fwprintf(f, L"# ------------------------------------------------------------ layout\n");
    fwprintf(f, L"# dwindle | master | grid\n");
    fwprintf(f, L"layout       = %s\n", ToLower(LayoutName(layout)).c_str());
    fwprintf(f, L"master_ratio = %.2f\n", masterRatio);
    fwprintf(f, L"master_count = %d\n\n", masterCount);

    fwprintf(f, L"# ------------------------------------------------------------ behaviour\n");
    fwprintf(f, L"tiling_enabled      = %s\n", Bool(tilingEnabled));
    fwprintf(f, L"new_window_on_top   = %s\n", Bool(newWindowOnTop));
    fwprintf(f, L"# Drag a tiled window and drop it on the left, right, top or bottom\n");
    fwprintf(f, L"# half of another one to put it on that side. Off means a dragged\n");
    fwprintf(f, L"# window snaps straight back where it was.\n");
    fwprintf(f, L"drag_to_rearrange   = %s\n", Bool(dragToRearrange));
    fwprintf(f, L"# Hold the modifier and drag anywhere on a window to move it, or\n"
                L"# right-drag to resize it from the nearest corner. Only ever acts on\n"
                L"# windows ProWindows arranges, so an excluded app keeps its own.\n"
                L"mod_drag            = %s\n", Bool(modDrag));
    fwprintf(f, L"# Drop the gaps when a workspace holds a single window. The gap\n"
                L"# exists to separate tiles from one another; with nothing to\n"
                L"# separate it is only wasted screen.\n"
                L"smart_gaps          = %s\n", Bool(smartGaps));
    fwprintf(f, L"focus_follows_mouse = %s\n", Bool(focusFollowsMouse));
    fwprintf(f, L"# Move the pointer onto a window when the keyboard focuses it.\n"
                L"# Worth turning on with focus_follows_mouse, which otherwise\n"
                L"# hands focus straight back to whatever the pointer is resting on.\n"
                L"cursor_warp         = %s\n", Bool(cursorWarp));
    fwprintf(f, L"start_minimized     = %s\n\n", Bool(startMinimized));

    fwprintf(f, L"# Smooth movement when windows are re-arranged. Costs a little CPU\n");
    fwprintf(f, L"# while the animation runs; off is the lightest setting.\n");
    fwprintf(f, L"animations   = %s\n", Bool(animations));
    fwprintf(f, L"animation_ms = %d\n\n", animationMs);

    fwprintf(f, L"# Percent of the screen one resize keypress moves a split.\n");
    fwprintf(f, L"resize_step = %d\n\n", resizeStep);

    fwprintf(f, L"# Stop arranging windows entirely while a game or any other\n");
    fwprintf(f, L"# fullscreen application is on screen. Strongly recommended: moving\n");
    fwprintf(f, L"# windows behind a game costs frames, and on some drivers drops it\n");
    fwprintf(f, L"# out of exclusive fullscreen.\n");
    fwprintf(f, L"pause_for_fullscreen = %s\n\n", Bool(pauseForFullscreen));

    fwprintf(f, L"# Write a diagnostic log next to this file.\n");
    fwprintf(f, L"debug = %s\n\n", Bool(debug));

    fwprintf(f, L"# ------------------------------------------------------------ screen space\n");
    fwprintf(f, L"# Extra pixels kept clear of tiles on each edge, on top of whatever\n");
    fwprintf(f, L"# Windows already reserves. Use these to make room for a custom bar.\n");
    fwprintf(f, L"margin_top    = %d\n", marginTop);
    fwprintf(f, L"margin_bottom = %d\n", marginBottom);
    fwprintf(f, L"margin_left   = %d\n", marginLeft);
    fwprintf(f, L"margin_right  = %d\n\n", marginRight);

    fwprintf(f, L"# ------------------------------------------------------------ monitor\n");
    fwprintf(f, L"# The floating CPU / RAM / GPU readout.\n");
    fwprintf(f, L"monitor_enabled  = %s\n", Bool(monitorEnabled));
    fwprintf(f, L"monitor_pinned   = %s\n", Bool(monitorPinned));
    fwprintf(f, L"monitor_x        = %d\n", monitorX);
    fwprintf(f, L"monitor_y        = %d\n", monitorY);
    fwprintf(f, L"monitor_opacity  = %d\n", monitorOpacity);
    fwprintf(f, L"monitor_scale    = %d\n", monitorScale);
    fwprintf(f, L"monitor_interval = %d\n", monitorInterval);
    fwprintf(f, L"monitor_vertical = %s\n", Bool(monitorVertical));
    fwprintf(f, L"monitor_graphs   = %s\n", Bool(monitorGraphs));

    fwprintf(f, L"# monitor_theme: one of");
    for (int i = 0; i < MonitorSkinCount(); ++i)
        fwprintf(f, L"%s %s", i ? L"," : L"", MonitorSkinAt(i).id);
    fwprintf(f, L"\nmonitor_theme    = %s\n", MonitorSkinAt(monitorTheme).id);

    fwprintf(f, L"# monitor_style: one of");
    for (int i = 0; i < MonitorStyleCount(); ++i)
        fwprintf(f, L"%s %s", i ? L"," : L"", MonitorStyleAt(i).id);
    fwprintf(f, L"\nmonitor_style    = %s\n", MonitorStyleAt(monitorStyle).id);

    fwprintf(f, L"# on_desktop puts the panel behind every window, like wallpaper.\n");
    fwprintf(f, L"monitor_on_desktop = %s\n", Bool(monitorOnDesktop));
    fwprintf(f, L"monitor_top_apps   = %s\n", Bool(monitorTopApps));
    fwprintf(f, L"monitor_smooth     = %s\n", Bool(monitorSmooth));
    fwprintf(f, L"# Divide the CPU bar into one segment per logical processor.\n"
                L"# An averaged bar cannot tell one pinned thread from a machine\n"
                L"# that is evenly busy, and those mean opposite things.\n"
                L"monitor_cores      = %s\n", Bool(monitorCores));

    fwprintf(f, L"# The order the readouts are drawn in, top to bottom. Any metric\n"
                L"# left off the end is appended in the order above.\n"
                L"monitor_order      = ");
    for (int i = 0; i < MON_METRIC_COUNT; ++i)
        fwprintf(f, L"%s%s", i ? L", " : L"", MonitorMetricAt(monOrder[i]).id);
    fwprintf(f, L"\n");

    fwprintf(f, L"# Per-metric colours. \"theme\" follows whichever theme is chosen.\n");
    for (int i = 0; i < MON_METRIC_COUNT; ++i) {
        const wchar_t* id = MonitorMetricAt(i).id;
        if (monColor[i] == kMonColourFromTheme) {
            fwprintf(f, L"monitor_color_%-8s = theme\n", id);
        } else {
            fwprintf(f, L"monitor_color_%-8s = #%02X%02X%02X\n", id,
                     GetRValue(monColor[i]), GetGValue(monColor[i]),
                     GetBValue(monColor[i]));
        }
    }

    fwprintf(f, L"monitor_cpu      = %s\n", Bool(monShowCpu));
    fwprintf(f, L"monitor_ram      = %s\n", Bool(monShowRam));
    fwprintf(f, L"monitor_gpu      = %s\n", Bool(monShowGpu));
    fwprintf(f, L"monitor_vram     = %s\n", Bool(monShowVram));
    fwprintf(f, L"# cpu_temp falls back to the mainboard's thermal zone unless\n");
    fwprintf(f, L"# LibreHardwareMonitor is running; gpu_temp needs an NVIDIA or\n");
    fwprintf(f, L"# AMD driver, or LibreHardwareMonitor for anything else.\n");
    fwprintf(f, L"monitor_cpu_temp = %s\n", Bool(monShowCpuTemp));
    fwprintf(f, L"monitor_gpu_temp = %s\n", Bool(monShowGpuTemp));
    fwprintf(f, L"monitor_disk     = %s\n", Bool(monShowDisk));
    fwprintf(f, L"monitor_net      = %s\n\n", Bool(monShowNet));

    fwprintf(f, L"# ------------------------------------------------------------ clock\n");
    fwprintf(f, L"# The desktop clock. Same window as the monitor: drag it, pin it,\n");
    fwprintf(f, L"# or send it to the desktop behind every window.\n");
    fwprintf(f, L"clock_enabled    = %s\n", Bool(clockEnabled));
    fwprintf(f, L"clock_pinned     = %s\n", Bool(clockPinned));
    fwprintf(f, L"clock_on_desktop = %s\n", Bool(clockOnDesktop));
    fwprintf(f, L"clock_x          = %d\n", clockX);
    fwprintf(f, L"clock_y          = %d\n", clockY);
    fwprintf(f, L"clock_opacity    = %d\n", clockOpacity);
    fwprintf(f, L"clock_scale      = %d\n", clockScale);
    fwprintf(f, L"# clock_theme: one of");
    for (int i = 0; i < ClockSkinCount(); ++i)
        fwprintf(f, L"%s %s", i ? L"," : L"", ClockSkinAt(i).id);
    fwprintf(f, L"\nclock_theme      = %s\n", ClockSkinAt(clockTheme).id);
    fwprintf(f, L"# clock_style: one of");
    for (int i = 0; i < ClockStyleCount(); ++i)
        fwprintf(f, L"%s %s", i ? L"," : L"", ClockStyleAt(i).id);
    fwprintf(f, L"\nclock_style      = %s\n", ClockStyleAt(clockStyle).id);
    fwprintf(f, L"clock_24h        = %s\n", Bool(clockHours24));
    fwprintf(f, L"clock_seconds    = %s\n", Bool(clockSeconds));
    fwprintf(f, L"clock_date       = %s\n", Bool(clockDate));
    fwprintf(f, L"clock_weekday    = %s\n\n", Bool(clockWeekday));

    fwprintf(f, L"# ------------------------------------------------------------ timer\n");
    fwprintf(f, L"# The clock panel (clock, alarms, timers, stopwatch): drag it, pin it. Win+W\n");
    fwprintf(f, L"# shows or hides it (the `timer` action).\n");
    fwprintf(f, L"timer_shown      = %s\n", Bool(timerShown));
    fwprintf(f, L"timer_pinned     = %s\n", Bool(timerPinned));
    fwprintf(f, L"timer_x          = %d\n", timerX);
    fwprintf(f, L"timer_y          = %d\n", timerY);
    fwprintf(f, L"# timer_tick: off, second or minute - a clock tick while a timer runs.\n");
    fwprintf(f, L"timer_tick       = %s\n", timerTick == 1 ? L"second" : (timerTick == 2 ? L"minute" : L"off"));
    fwprintf(f, L"# timer_alarm: loop a sound (up to a minute) when a timer ends.\n");
    fwprintf(f, L"timer_alarm      = %s\n\n", Bool(timerAlarm));

    fwprintf(f, L"# ------------------------------------------------------------ File Explorer\n");
    fwprintf(f, L"# Styles File Explorer by loading ProWindows_explorer.dll into explorer.exe\n");
    fwprintf(f, L"# (a port of the Windhawk 'Windows 11 File Explorer Styler' mod). It is\n");
    fwprintf(f, L"# taken out again when ProWindows quits. Your own styles go in\n");
    fwprintf(f, L"# explorer-styler.ini beside this file.\n");
    fwprintf(f, L"explorer_styler  = %s\n", Bool(explorerStyler));
    fwprintf(f, L"# explorer_theme: ProWindows, or the name of one of the mod's themes\n");
    fwprintf(f, L"# (Matter, TintedGlass, Minimal Explorer11 ...); empty for none.\n");
    fwprintf(f, L"explorer_theme   = %s\n", explorerTheme.c_str());
    fwprintf(f, L"# explorer_effect: empty (the theme's own), blur, acrylic, mica, micaalt, none.\n");
    fwprintf(f, L"explorer_effect  = %s\n\n", explorerEffect.c_str());

    fwprintf(f, L"# ------------------------------------------------------------ search bar\n");
    fwprintf(f, L"# What the search bar looks through besides installed apps.\n");
    fwprintf(f, L"search_files    = %s\n", Bool(searchFiles));
    fwprintf(f, L"# Also walk Program Files and %%LOCALAPPDATA%%\\Programs for .exe\n"
                L"# files, so a program with no Start-menu shortcut is still findable.\n"
                L"search_programs = %s\n", Bool(searchPrograms));
    fwprintf(f, L"# Look on every fixed drive, not only the one Windows is on. A second\n"
                L"# disk is where games and anything large actually live, and none of it\n"
                L"# is under Program Files or listed in the Start menu.\n"
                L"search_drives   = %s\n", Bool(searchDrives));
    fwprintf(f, L"# Include the executables an application ships to serve itself -\n"
                L"# helpers, updaters, crash handlers, a toolchain's POSIX userland.\n"
                L"# Many more results, and nearly all of them noise.\n"
                L"search_deep_exe = %s\n", Bool(searchDeepExe));
    fwprintf(f, L"search_commands = %s\n", Bool(searchCommands));
    fwprintf(f, L"search_calc     = %s\n", Bool(searchCalc));
    fwprintf(f, L"search_settings = %s\n\n", Bool(searchSettings));

    fwprintf(f, L"# The file index. It is built once on a background thread and held in\n");
    fwprintf(f, L"# memory, so max_entries is what it costs you - roughly 300 bytes each.\n");
    fwprintf(f, L"# Leave search_folders empty for Desktop, Documents, Downloads,\n");
    fwprintf(f, L"# Pictures, Music and Videos, which follows those folders if you move them.\n");
    fwprintf(f, L"search_folders     = %s\n", JoinList(searchFolders).c_str());
    fwprintf(f, L"search_depth       = %d\n", searchDepth);
    fwprintf(f, L"search_max_entries = %d\n", searchMaxEntries);
    fwprintf(f, L"# Executables are counted separately, so a folder full of documents\n"
                L"# cannot use the budget up before the programs are reached.\n"
                L"search_max_programs = %d\n", searchMaxPrograms);
    fwprintf(f, L"search_hidden      = %s\n\n", Bool(searchHidden));

    fwprintf(f, L"# Claim shortcuts the Windows shell reserves (Win+E, Win+F, Win+Q...).\n");
    fwprintf(f, L"# Needs a low-level keyboard hook, installed only if a binding wants one.\n");
    fwprintf(f, L"override_windows_shortcuts = %s\n\n", Bool(overrideReserved));

    fwprintf(f, L"# ------------------------------------------------------------ rules\n");
    fwprintf(f, L"# ignore_* = never touched at all.  float_* = managed, but never tiled.\n");
    fwprintf(f, L"# Comma separated. Titles match on any part of the title.\n");
    fwprintf(f, L"ignore_process = %s\n", JoinList(ignoreProcess).c_str());
    fwprintf(f, L"ignore_class   = %s\n", JoinList(ignoreClass).c_str());
    fwprintf(f, L"ignore_title   = %s\n", JoinList(ignoreTitle).c_str());
    fwprintf(f, L"float_process  = %s\n", JoinList(floatProcess).c_str());
    fwprintf(f, L"float_class    = %s\n", JoinList(floatClass).c_str());
    fwprintf(f, L"float_title    = %s\n", JoinList(floatTitle).c_str());

    if (!learnedLimits.empty()) {
        fwprintf(f, L"\n# ------------------------------------------------- learned sizes\n");
        fwprintf(f, L"# Written by ProWindows, not meant to be edited. These are the\n");
        fwprintf(f, L"# minimum and maximum sizes applications turned out to insist on,\n");
        fwprintf(f, L"# remembered so they do not have to be rediscovered - which can only\n");
        fwprintf(f, L"# be done by handing the window a size it refuses and watching.\n");
        fwprintf(f, L"# \"nofit\" means the window will not fit any tile and is left alone.\n");
        fwprintf(f, L"# Delete a line to make ProWindows forget and measure again.\n");
        for (const auto& e : learnedLimits) {
            const RememberedLimits& l = e.second;
            fwprintf(f, L"learned = %s, %d, %d, %d, %d%s\n", e.first.c_str(),
                     l.minW, l.minH, l.maxW, l.maxH, l.tooLarge ? L", nofit" : L"");
        }
    }

    fwprintf(f, L"%s\n", kBindingReference);

    // The whole binding set is written out explicitly, so this file is an exact
    // description of what the Shortcuts tab shows - no hidden defaults to
    // reconcile, and editing here round-trips through the settings window.
    fwprintf(f, L"clear_binds = true\n\n");
    for (const auto& kb : binds) {
        const std::wstring keys = KeySpecText(kb.mods, kb.vk);
        const std::wstring act  = ActionSpecText(kb);
        if (keys.empty() || act.empty()) continue;
        fwprintf(f, L"bind = %-18s %s\n", (keys + L",").c_str(), act.c_str());
    }

    // Push everything out of the CRT's buffer before anything is swapped: a
    // buffer still sitting in memory is exactly what this is guarding against.
    fflush(f);
    const bool wrote = (ferror(f) == 0);
    fclose(f);

    if (!wrote) {
        AWA_LOG(L"config: writing %s failed", temp.c_str());
        DeleteFileW(temp.c_str());
        return false;
    }

    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        // Usually something holding config.ini open without sharing: an
        // editor, a sync client, an antivirus scan.
        AWA_LOG(L"config: could not replace %s (error %lu)", path.c_str(), GetLastError());
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool Config::WriteDefaultFile(const std::wstring& path) {
    Config fresh;
    fresh.LoadDefaults();
    return fresh.SaveToFile(path);
}

} // namespace awa
