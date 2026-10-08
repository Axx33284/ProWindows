// ProWindows - the Start page: Start menu styling (a port of the Windhawk
// "Windows 11 Start Menu Styler"). Rows over the edit copy, like every other
// page. The theme and layout are stored by name in config.ini, so the rows map
// the name to an index here.
#include "settings_internal.h"
#include "startmenustyler.h"

#include <shellapi.h>

namespace awa {

using ui::Row;

namespace {

bool On() { return Edit().startStyler; }
bool OwnTheme() { return On() && StartThemeIsProWindows(Edit().startTheme); }
bool GlassTheme() { return On() && Edit().startTheme == L"ProWindows Glass"; }

int ThemeIndex() {
    const int i = StartThemeIndexOf(Edit().startTheme);
    return i < 0 ? 0 : i;
}

int LayoutIndex() {
    const int i = StartLayoutIndexOf(Edit().startLayout);
    return i < 0 ? 0 : i;
}

std::wstring ThemeBlurb(int i) {
    const std::wstring id = StartThemeId(i);
    if (id == L"ProWindows Glass")
        return L"The ProWindows look with the Start menu see-through: the desktop blurred behind "
               L"a tint, a metal gradient under the pointer, hairline borders. How glassy it is, "
               L"and the tint, are on the Look page.";
    if (id == L"ProWindows")
        return L"Solid black Start menu and search, a dark-to-light metal gradient under the "
               L"pointer, light hairline borders and no accent colour. The tint, highlight, "
               L"corner radius and text colour are on the Look page.";
    if (id.empty())
        return L"No theme: the Start menu keeps its own look, apart from any custom styles you "
               L"add on the Custom styles page.";
    return L"One of the themes that come with the Windhawk Start Menu Styler. Some only suit the "
           L"redesigned Start menu or the classic one, and some change its layout, which "
           L"restarts the Start menu (see Layout).";
}

} // namespace

void BuildStartPage(std::vector<Row>& rows) {
    Config& e = Edit();
    const Config& s = Saved();
    auto add = [&](Row r) { r.enabled = On; rows.push_back(r); };
    auto addOwn = [&](Row r) { r.enabled = OwnTheme; rows.push_back(r); };

    rows.push_back(ui::Page(L"Styling"));
    rows.push_back(ui::Section(L"Start menu"));
    rows.push_back(ui::Toggle(L"styler", L"Start menu styling",
        L"Restyles the Start menu and the search flyout by loading a small DLL into the two "
        L"Windows processes that draw them. It is taken out again when you turn this off or quit "
        L"ProWindows. If the Start menu keeps restarting right after it goes in, ProWindows "
        L"turns this off by itself.", &e.startStyler, &s.startStyler));
    {
        Row r;
        r.kind  = ui::Kind::Choice;
        r.id    = L"theme";
        r.label = L"Theme";
        r.help  = ThemeBlurb(ThemeIndex());
        for (int i = 0; i < StartThemeCount(); ++i) r.options.push_back(StartThemeName(i));
        r.get = []() { return ThemeIndex(); };
        r.set = [](int v) { Edit().startTheme = StartThemeId(v); };
        r.modified = []() { return Edit().startTheme != Saved().startTheme; };
        r.fallback = []() { return std::wstring(StartThemeName(0)); };
        add(r);
    }
    {
        Row r;
        r.kind  = ui::Kind::Choice;
        r.id    = L"layout";
        r.label = L"Layout";
        r.help  = L"Which Start menu layout to use: the theme's own, the Windows default, the "
                  L"classic one, or the redesigned one with the pinned apps beside the list. "
                  L"Anything other than the default makes the Start menu process restart so the "
                  L"layout can take effect; if it keeps restarting ProWindows turns styling "
                  L"off. The classic layouts are gone from the newest Windows builds, where "
                  L"this choice is ignored.";
        for (int i = 0; i < StartLayoutCount(); ++i) r.options.push_back(StartLayoutName(i));
        r.get = []() { return LayoutIndex(); };
        r.set = [](int v) { Edit().startLayout = StartLayoutId(v); };
        r.modified = []() { return Edit().startLayout != Saved().startLayout; };
        r.fallback = []() { return std::wstring(StartLayoutName(0)); };
        add(r);
    }
    rows.push_back(ui::Info(L"status", L"Status",
        L"How many Start menu processes are styled right now, or why none is. The Start menu "
        L"and the search flyout are two processes; each is styled when it appears. Windows "
        L"may end them to save memory and start them again, which is normal.",
        []() { return StartMenuStylerStatus(); }));

    rows.push_back(ui::Page(L"Look"));
    rows.push_back(ui::Section(L"ProWindows themes"));
    {
        Row r;
        r.kind  = ui::Kind::Colour;
        r.id    = L"tint";
        r.label = L"Tint";
        r.help  = L"The colour of the Start menu's background. Only the two ProWindows themes "
                  L"use it; pick one of them on the Styling page to change it. Accent colour is "
                  L"the one Windows is set to now, read when you apply.";
        for (int i = 0; i < StartTintCount(); ++i) {
            r.options.push_back(StartTintName(i));
            r.colours.push_back(StartTintColor(i));
        }
        r.get = []() { return Edit().startTint; };
        r.set = [](int v) { Edit().startTint = v; };
        r.modified = []() { return Edit().startTint != Saved().startTint; };
        r.fallback = []() { return std::wstring(StartTintName(0)); };
        addOwn(r);
    }
    {
        Row r = ui::Slider(L"opacity", L"Tint opacity",
            L"How much of the tint covers the blur behind the Start menu. Lower shows more of "
            L"the desktop. Only the ProWindows Glass theme has it; ProWindows is solid.",
            &e.startTintOpacity, &s.startTintOpacity, 0, 100, 1, 5, L"%");
        r.enabled = GlassTheme;
        rows.push_back(r);
    }
    {
        Row r;
        r.kind  = ui::Kind::Choice;
        r.id    = L"highlight";
        r.label = L"Highlight";
        r.help  = L"What fills the item under the pointer: the metal gradient, the accent colour, "
                  L"or a lighter shade of the tint. Only the ProWindows themes use it.";
        r.options = { L"Metal gradient", L"Accent colour", L"Tint" };
        r.get = []() { return Edit().startHighlight; };
        r.set = [](int v) { Edit().startHighlight = v; };
        r.modified = []() { return Edit().startHighlight != Saved().startHighlight; };
        r.fallback = []() { return std::wstring(L"Metal gradient"); };
        addOwn(r);
    }
    addOwn(ui::Slider(L"radius", L"Corner radius",
        L"How round the Start menu's corners are, in pixels. Only the ProWindows themes use it.",
        &e.startRadius, &s.startRadius, 0, 12, 1, 2, L" px"));
    {
        Row r;
        r.kind  = ui::Kind::Choice;
        r.id    = L"text";
        r.label = L"Text";
        r.help  = L"The colour of the Start menu's text. Only the ProWindows themes use it.";
        r.options = { L"White", L"Light grey" };
        r.get = []() { return Edit().startText; };
        r.set = [](int v) { Edit().startText = v; };
        r.modified = []() { return Edit().startText != Saved().startText; };
        r.fallback = []() { return std::wstring(L"White"); };
        addOwn(r);
    }

    rows.push_back(ui::Page(L"Custom styles"));
    rows.push_back(ui::Section(L"Your own styles"));
    rows.push_back(ui::Action(L"edit", L"Custom styles",
        L"Opens startmenu-styler.ini. Below the part ProWindows writes you can add styles of "
        L"your own, one target per line pair: controlStyles[0].target=Border#AcrylicBorder "
        L"then controlStyles[0].styles[0]=Background=#8B0000 (a red Start menu). Targets are "
        L"XAML class names with an optional #Name, [Property=Value], @VisualState and "
        L"Parent > Child; numbers must run on without gaps; styleConstants (from [6] when a "
        L"ProWindows theme is chosen) and themeResourceVariables work as in the Windhawk mod. "
        L"Save the file, then press Reload.",
        L"Edit...", []() {
            StartMenuStylerEnsureIni();
            ShellExecuteW(nullptr, L"open", L"notepad.exe", StartMenuStylerIniPath().c_str(),
                          nullptr, SW_SHOWNORMAL);
        }));
    rows.push_back(ui::Action(L"reload", L"Reload",
        L"Makes the Start menu read startmenu-styler.ini again, with the settings as last "
        L"applied. Also tries again after a failure.",
        L"Reload", []() {
            // Reload acts on the applied config; say so rather than claim a reload.
            if (!Saved().startStyler) {
                SettingsToast(L"Start menu styling is off; turn it on and Apply first");
                return;
            }
            StartMenuStylerReload();
            SettingsToast(L"Asked the Start menu to reload its styles");
        }));
}

void ResetStartPage() {
    Config d;
    d.LoadDefaults();
    Config& e = Edit();
    e.startStyler      = d.startStyler;
    e.startTheme       = d.startTheme;
    e.startLayout      = d.startLayout;
    e.startTint        = d.startTint;
    e.startTintOpacity = d.startTintOpacity;
    e.startHighlight   = d.startHighlight;
    e.startRadius      = d.startRadius;
    e.startText        = d.startText;
}

} // namespace awa
