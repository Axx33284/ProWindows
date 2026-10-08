// ProWindows - the Explorer page: File Explorer styling (a port of the Windhawk
// "Windows 11 File Explorer Styler"). Rows over the edit copy, like every other
// page. The theme and effect are stored by name in config.ini, so the rows map
// the name to an index here.
#include "settings_internal.h"
#include "explorerstyler.h"

#include <shellapi.h>

namespace awa {

using ui::Row;

namespace {

bool On() { return Edit().explorerStyler; }

int ThemeIndex() {
    const int i = ExplorerThemeIndexOf(Edit().explorerTheme);
    return i < 0 ? 0 : i;
}

int EffectIndex() {
    const int i = ExplorerEffectIndexOf(Edit().explorerEffect);
    return i < 0 ? 0 : i;
}

std::wstring ThemeBlurb(int i) {
    const std::wstring id = ExplorerThemeId(i);
    if (id == L"ProWindows")
        return L"Black window, navigation pane and command bar, a dark-to-light metal gradient "
               L"on the item under the pointer and on the selected tab, light hairline borders "
               L"and no accent colour. The same look as the rest of ProWindows.";
    if (id == L"ProWindows Glass")
        return L"The ProWindows look with a translucent tint over a blurred background: you see "
               L"the desktop and the windows behind through the Explorer window. Set the tint and "
               L"its opacity on the Look page. The background effect defaults to Acrylic.";
    if (id.empty())
        return L"No theme: File Explorer keeps its own look, apart from any custom styles you "
               L"add on the Custom styles page.";
    return L"One of the themes that come with the Windhawk File Explorer Styler. Some use the "
           L"accent colour or blur what is behind the window. A few also change the height of "
           L"the Explorer frame, which is not supported here, so they can leave a gap.";
}

} // namespace

void BuildExplorerPage(std::vector<Row>& rows) {
    Config& e = Edit();
    const Config& s = Saved();
    auto add = [&](Row r) { r.enabled = On; rows.push_back(r); };

    rows.push_back(ui::Page(L"Styling"));
    rows.push_back(ui::Section(L"File Explorer"));
    rows.push_back(ui::Toggle(L"styler", L"File Explorer styling",
        L"Restyles File Explorer's address bar, command bar, tabs and background by loading a "
        L"small DLL into Explorer. It is taken out again when you turn this off or quit "
        L"ProWindows. If Explorer keeps restarting right after it goes in, ProWindows turns "
        L"this off by itself.", &e.explorerStyler, &s.explorerStyler));
    {
        Row r;
        r.kind  = ui::Kind::Choice;
        r.id    = L"theme";
        r.label = L"Theme";
        r.help  = ThemeBlurb(ThemeIndex());
        for (int i = 0; i < ExplorerThemeCount(); ++i) r.options.push_back(ExplorerThemeName(i));
        r.get = []() { return ThemeIndex(); };
        r.set = [](int v) { Edit().explorerTheme = ExplorerThemeId(v); };
        r.modified = []() { return Edit().explorerTheme != Saved().explorerTheme; };
        r.fallback = []() { return std::wstring(ExplorerThemeName(0)); };
        add(r);
    }
    {
        Row r;
        r.kind  = ui::Kind::Choice;
        r.id    = L"effect";
        r.label = L"Background effect";
        r.help  = L"What is drawn behind a translucent Explorer window. Theme default uses what "
                  L"the theme asks for. Mica and Mica Alt show a tint of your wallpaper, Acrylic "
                  L"and Blur blur what is behind the window, None is a plain colour.";
        for (int i = 0; i < ExplorerEffectCount(); ++i) r.options.push_back(ExplorerEffectName(i));
        r.get = []() { return EffectIndex(); };
        r.set = [](int v) { Edit().explorerEffect = ExplorerEffectId(v); };
        r.modified = []() { return Edit().explorerEffect != Saved().explorerEffect; };
        r.fallback = []() { return std::wstring(ExplorerEffectName(0)); };
        add(r);
    }
    add(ui::ChoiceOf(L"region", L"Effect region",
        L"How far the background effect reaches. Entire window puts it behind everything, the "
        L"title bar included; Frame only keeps it to Explorer's own frame (the address bar, "
        L"command bar and navigation pane).",
        &e.explorerRegion, &s.explorerRegion, { 0, 1 },
        { L"Entire window", L"Frame only" }));
    rows.push_back(ui::Info(L"status", L"Status",
        L"How many File Explorer processes are styled right now, or why none is. A folder "
        L"window that opens in its own process is styled when it appears. Explorer running as "
        L"administrator cannot be styled.",
        []() { return ExplorerStylerStatus(); }));

    // The Look page drives the two ProWindows themes; the rows say so and sit
    // idle under any other theme.
    rows.push_back(ui::Page(L"Look"));
    rows.push_back(ui::Section(L"ProWindows themes"));
    auto look = [&](Row r, bool glassOnly) {
        r.enabled = [glassOnly]() {
            if (!On() || !ExplorerThemeIsProWindows(Edit().explorerTheme)) return false;
            return !glassOnly || Edit().explorerTheme == L"ProWindows Glass";
        };
        rows.push_back(r);
    };
    const std::wstring only = L"\r\n\r\nUsed only while the theme is ProWindows or ProWindows Glass.";
    look(ColourRow(L"tint", L"Tint",
        L"The colour of the window, navigation pane and command bar. Black by default." + only,
        &e.explorerTint, &s.explorerTint, CLR_INVALID, 0), false);
    look(ui::Slider(L"tintopacity", L"Tint opacity",
        L"How much of the tint covers the background effect. 0 shows the effect alone, 100 is "
        L"a solid colour. ProWindows Glass only - the plain ProWindows theme is always solid.",
        &e.explorerTintOpacity, &s.explorerTintOpacity, 0, 100, 5, 10, L"%"), true);
    look(ui::ChoiceOf(L"highlight", L"Highlight",
        L"What the item under the pointer and the selected tab are filled with: the dark-to-light "
        L"metal gradient, the Windows accent colour, or a lighter shade of the tint." + only,
        &e.explorerHighlight, &s.explorerHighlight, { 0, 1, 2 },
        { L"Metal gradient", L"Accent colour", L"Tint" }), false);
    look(ui::Slider(L"radius", L"Corner radius",
        L"How round the corners of the address bar and of the highlighted buttons are." + only,
        &e.explorerRadius, &s.explorerRadius, 0, 12, 1, 2, L" px"), false);
    look(ui::ChoiceOf(L"text", L"Text",
        L"The colour of the text in File Explorer's bars." + only,
        &e.explorerText, &s.explorerText, { 0, 1 }, { L"White", L"Light grey" }), false);

    rows.push_back(ui::Page(L"Custom styles"));
    rows.push_back(ui::Section(L"Your own styles"));
    add(ui::ChoiceOf(L"xaml", L"Other XAML diagnostics users",
        L"Styling uses the XAML diagnostics channel, which only one program can hold. Block "
        L"keeps it for ProWindows (and stops other tools that restyle Explorer, like Windhawk "
        L"mods, from taking it). Allow lets them in, which can make styling fail. Ask shows "
        L"a prompt inside File Explorer when another program wants it.",
        &e.explorerXamlDiag, &s.explorerXamlDiag, { 1, 2, 0 },
        { L"Block", L"Allow", L"Ask" }));
    rows.push_back(ui::Action(L"edit", L"Custom styles",
        L"Opens explorer-styler.ini. Below the part ProWindows writes you can add styles of "
        L"your own, one target per line pair: controlStyles[0].target=Grid#CommandBarControlRootGrid "
        L"then controlStyles[0].styles[0]=Background=#8B0000 (a red command bar). Targets are "
        L"XAML class names with an optional #Name, [Property=Value] and Parent > Child; numbers "
        L"must run on without gaps; styleConstants[N] and themeResourceVariables[N] work as in "
        L"the Windhawk mod. Save the file, then press Reload.",
        L"Edit...", []() {
            ExplorerStylerEnsureIni();
            ShellExecuteW(nullptr, L"open", L"notepad.exe", ExplorerStylerIniPath().c_str(),
                          nullptr, SW_SHOWNORMAL);
        }));
    rows.push_back(ui::Action(L"reload", L"Reload",
        L"Makes File Explorer read explorer-styler.ini again, with the theme and effect as last "
        L"applied. Also tries again after a failure.",
        L"Reload", []() {
            // Reload acts on the applied config; say so rather than claim a reload.
            if (!Saved().explorerStyler) {
                SettingsToast(L"File Explorer styling is off; turn it on and Apply first");
                return;
            }
            ExplorerStylerReload();
            SettingsToast(L"Asked File Explorer to reload its styles");
        }));
}

void ResetExplorerPage() {
    Config d;
    d.LoadDefaults();
    Config& e = Edit();
    e.explorerStyler = d.explorerStyler;
    e.explorerTheme  = d.explorerTheme;
    e.explorerEffect = d.explorerEffect;
    e.explorerTint        = d.explorerTint;
    e.explorerTintOpacity = d.explorerTintOpacity;
    e.explorerHighlight   = d.explorerHighlight;
    e.explorerRadius      = d.explorerRadius;
    e.explorerText        = d.explorerText;
    e.explorerRegion      = d.explorerRegion;
    e.explorerXamlDiag    = d.explorerXamlDiag;
}

} // namespace awa
