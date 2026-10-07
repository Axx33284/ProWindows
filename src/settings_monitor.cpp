// ProWindows - the Monitor page: the floating system monitor's switches, its
// look, which readouts it shows and in what order, and their colours. The
// preview beside the rows is drawn by the overlay's own painter from the edit
// copy, so it shows what Apply is about to do.
#include "settings_internal.h"
#include "monitor.h"
#include "montheme.h"

namespace awa {

using ui::Row;

namespace {

bool* ShowField(Config& c, int metric) {
    switch (metric) {
        case MON_CPU:     return &c.monShowCpu;
        case MON_RAM:     return &c.monShowRam;
        case MON_GPU:     return &c.monShowGpu;
        case MON_VRAM:    return &c.monShowVram;
        case MON_CPUTEMP: return &c.monShowCpuTemp;
        case MON_GPUTEMP: return &c.monShowGpuTemp;
        case MON_DISK:    return &c.monShowDisk;
        default:          return &c.monShowNet;
    }
}

// Lifts the metric at `from` and puts it back in at `to`, sliding the ones
// between along - not a swap, so dragging the top readout to the bottom does
// not reshuffle the middle.
void MoveReadout(int from, int to) {
    int* order = Edit().monOrder;
    if (from < 0 || to < 0 || from >= MON_METRIC_COUNT || to >= MON_METRIC_COUNT || from == to) return;
    const int metric = order[from];
    const int step = (to > from) ? 1 : -1;
    for (int i = from; i != to; i += step) order[i] = order[i + step];
    order[to] = metric;
}

bool On() { return Edit().monitorEnabled; }

} // namespace

void BuildMonitorPage(std::vector<Row>& rows) {
    Config& e = Edit();
    const Config& s = Saved();
    MonitorNormaliseOrder(e.monOrder);

    rows.push_back(ui::Section(L"Overlay"));
    rows.push_back(ui::Toggle(L"enabled", L"System monitor",
        L"A small panel with live readings - processor, memory, graphics, temperatures, "
        L"disk and network - that floats over your windows. Drag it anywhere; right-click "
        L"it for all of this.",
        &e.monitorEnabled, &s.monitorEnabled));
    {
        Row r = ui::Toggle(L"pinned", L"Pin in place",
            L"Locks it where it is: it cannot be dragged, and clicks go straight through to "
            L"whatever is underneath.", &e.monitorPinned, &s.monitorPinned);
        r.enabled = On;
        rows.push_back(r);
        Row d = ui::Toggle(L"desktop", L"On the desktop",
            L"Sits on the desktop behind every window instead of over them: visible on a "
            L"clear desktop, covered as soon as you open something.",
            &e.monitorOnDesktop, &s.monitorOnDesktop);
        d.enabled = On;
        rows.push_back(d);
        Row p = ui::Action(L"resetpos", L"Position",
            L"Puts the panel back in its corner. Takes effect at once.",
            L"Reset", []() {
                Config& cfg = AppConfig();
                cfg.monitorX = INT_MIN;
                cfg.monitorY = INT_MIN;
                MonitorApplyConfig();
                AppSaveConfig();
                SettingsToast(L"The monitor is back in its corner");
            });
        p.enabled = On;
        rows.push_back(p);
    }

    rows.push_back(ui::Section(L"Look"));
    {
        Row r;
        r.kind  = ui::Kind::Choice;
        r.id    = L"style";
        r.label = L"Style";
        r.help  = MonitorStyleAt(e.monitorStyle).blurb;
        for (int i = 0; i < MonitorStyleCount(); ++i) r.options.push_back(MonitorStyleAt(i).name);
        r.get = []() { return (std::max)(0, (std::min)(MonitorStyleCount() - 1, Edit().monitorStyle)); };
        r.set = [](int v) { Edit().monitorStyle = v; };
        r.modified = []() { return Edit().monitorStyle != Saved().monitorStyle; };
        r.enabled = On;
        rows.push_back(r);
    }
    {
        Row r;
        r.kind  = ui::Kind::Choice;
        r.id    = L"theme";
        r.label = L"Theme";
        r.help  = MonitorSkinAt(e.monitorTheme).blurb;
        for (int i = 0; i < MonitorSkinCount(); ++i) r.options.push_back(MonitorSkinAt(i).name);
        r.get = []() { return (std::max)(0, (std::min)(MonitorSkinCount() - 1, Edit().monitorTheme)); };
        r.set = [](int v) { Edit().monitorTheme = v; };
        r.modified = []() { return Edit().monitorTheme != Saved().monitorTheme; };
        r.enabled = On;
        rows.push_back(r);
    }
    auto add = [&](Row r, std::function<bool()> when = nullptr) {
        r.enabled = when ? when : std::function<bool()>(On);
        rows.push_back(r);
    };
    add(ui::Toggle(L"vertical", L"Arrangement",
        L"The readings one above the other, or side by side in a strip.",
        &e.monitorVertical, &s.monitorVertical, L"Stacked", L"In a row"));
    add(ui::Toggle(L"graphs", L"History graphs",
        L"A line under each reading showing the last minute or so. Not every style has "
        L"room for one.", &e.monitorGraphs, &s.monitorGraphs),
        []() { return Edit().monitorEnabled && MonitorStyleUsesGraphs(Edit().monitorStyle); });
    add(ui::Toggle(L"topapp", L"Busiest app",
        L"Names the program using the most of each resource under its reading.",
        &e.monitorTopApps, &s.monitorTopApps));
    add(ui::Toggle(L"cores", L"Every processor core",
        L"Draws the processor as one bar per core rather than one average, so a single "
        L"busy core is not hidden by seven idle ones.", &e.monitorCores, &s.monitorCores));
    add(ui::Toggle(L"smooth", L"Smooth readings",
        L"Eases each reading towards the next instead of jumping.",
        &e.monitorSmooth, &s.monitorSmooth));
    add(ui::Slider(L"opacity", L"Opacity",
        L"How solid the panel is. Lower lets more of what is behind it show through.",
        &e.monitorOpacity, &s.monitorOpacity, 20, 100, 1, 5, L"%"));
    add(ui::Slider(L"scale", L"Size",
        L"The panel's size, as a share of its normal size.",
        &e.monitorScale, &s.monitorScale, 75, 175, 1, 5, L"%"));
    add(ui::ChoiceOf(L"interval", L"Update every",
        L"How often the readings are taken. Faster costs a little more processor time.",
        &e.monitorInterval, &s.monitorInterval, { 500, 1000, 2000, 3000 },
        { L"0.5 seconds", L"1 second", L"2 seconds", L"3 seconds" }));

    rows.push_back(ui::Section(L"Readouts, in order"));
    for (int slot = 0; slot < MON_METRIC_COUNT; ++slot) {
        const int metric = e.monOrder[slot];
        Row r = ui::Toggle(L"show:" + std::to_wstring(metric), MonitorMetricAt(metric).menu,
            L"Whether the panel shows this reading. The order here is the order on the "
            L"panel: drag a row by its grip, or hold Ctrl and press Up or Down.",
            ShowField(e, metric), ShowField(const_cast<Config&>(s), metric), L"Show", L"Hide");
        r.orderGroup = 1;
        r.orderIndex = slot;
        r.move = [](int from, int to) { MoveReadout(from, to); };
        r.enabled = On;
        if (slot == 0) {
            // One modified mark for the order as a whole, on the first row.
            bool* show = ShowField(e, metric);
            const bool* saved = ShowField(const_cast<Config&>(s), metric);
            r.modified = [show, saved]() {
                if (*show != *saved) return true;
                for (int i = 0; i < MON_METRIC_COUNT; ++i)
                    if (Edit().monOrder[i] != Saved().monOrder[i]) return true;
                return false;
            };
        }
        rows.push_back(r);
    }

    rows.push_back(ui::Section(L"Colours"));
    const MonitorSkin& skin = MonitorSkinAt(e.monitorTheme);
    for (int slot = 0; slot < MON_METRIC_COUNT; ++slot) {
        const int metric = e.monOrder[slot];
        Row r = ColourRow(L"colour:" + std::to_wstring(metric), MonitorMetricAt(metric).menu,
            L"The colour of this reading. Theme follows whichever theme is chosen above.",
            &e.monColor[metric], &s.monColor[metric],
            MonitorMetricColour(skin, metric, kMonColourFromTheme), kMonColourFromTheme);
        r.enabled = On;
        rows.push_back(r);
    }
    {
        Row r = ui::Action(L"themecolours", L"Every colour from the theme",
            L"Clears every colour chosen above, so the whole panel follows the theme.",
            L"Reset", []() {
                for (int i = 0; i < MON_METRIC_COUNT; ++i) Edit().monColor[i] = kMonColourFromTheme;
                SettingsRebuild();
            });
        r.enabled = On;
        rows.push_back(r);
    }
}

void ResetMonitorPage() {
    Config d;
    d.LoadDefaults();
    Config& e = Edit();
    e.monitorEnabled = d.monitorEnabled;   e.monitorPinned = d.monitorPinned;
    e.monitorOnDesktop = d.monitorOnDesktop;
    e.monitorGraphs = d.monitorGraphs;     e.monitorTopApps = d.monitorTopApps;
    e.monitorVertical = d.monitorVertical; e.monitorCores = d.monitorCores;
    e.monitorSmooth = d.monitorSmooth;     e.monitorTheme = d.monitorTheme;
    e.monitorStyle = d.monitorStyle;       e.monitorOpacity = d.monitorOpacity;
    e.monitorScale = d.monitorScale;       e.monitorInterval = d.monitorInterval;
    e.monShowCpu = d.monShowCpu;           e.monShowRam = d.monShowRam;
    e.monShowGpu = d.monShowGpu;           e.monShowVram = d.monShowVram;
    e.monShowCpuTemp = d.monShowCpuTemp;   e.monShowGpuTemp = d.monShowGpuTemp;
    e.monShowDisk = d.monShowDisk;         e.monShowNet = d.monShowNet;
    for (int i = 0; i < MON_METRIC_COUNT; ++i) {
        e.monOrder[i] = d.monOrder[i];
        e.monColor[i] = d.monColor[i];
    }
}

void PreviewMonitor(HDC dc, const RECT& area) {
    const Config& e = Edit();
    MonitorPreview look;
    look.theme    = e.monitorTheme;
    look.style    = e.monitorStyle;
    look.opacity  = e.monitorOpacity;
    look.graphs   = e.monitorGraphs;
    look.topApps  = e.monitorTopApps;
    look.vertical = e.monitorVertical;
    look.colors   = e.monColor;
    MonitorDrawPreview(dc, area, look);
}

} // namespace awa
