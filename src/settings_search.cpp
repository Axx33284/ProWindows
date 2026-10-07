// ProWindows - the Search page: which sources the search bar draws on, and how
// the file index is built. Rows over the edit copy, like every other page;
// nothing reaches the live config until Apply.
#include "settings_internal.h"
#include "search.h"

namespace awa {

using ui::Row;

namespace {

// Offered ceilings for the index. Bigger is not better: each entry costs
// roughly 300 bytes held for the life of the process, and past a point you
// are scrolling rather than searching.
const int kEntryCaps[] = { 5000, 10000, 20000, 50000, 100000 };

// How far below each root to walk. Deeper finds more and costs more; past
// about eight levels you are indexing build output, not documents.
const int kDepths[] = { 2, 3, 4, 5, 6, 8, 12 };

std::wstring Thousands(int n) {
    std::wstring s = std::to_wstring(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, L",");
    return s;
}

// The drives the program walk will visit, so the switch says something
// concrete. Fixed disks only - a USB stick or a share is not walked.
std::wstring FixedDrives() {
    std::wstring drives;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const std::wstring root = std::wstring(1, (wchar_t)(L'A' + i)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        if (!drives.empty()) drives += L"  ";
        drives += root.substr(0, 2);
    }
    return drives;
}

} // namespace

void BuildSearchPage(std::vector<Row>& rows) {
    Config& e = Edit();
    const Config& s = Saved();
    const auto files = []() { return Edit().searchFiles; };

    rows.push_back(ui::Section(L"What to search"));
    rows.push_back(ui::Toggle(L"files", L"Files and folders",
        L"Finds files and folders by name, from an index of the folders listed below. "
        L"The index is built once in the background and kept up to date after that.",
        &e.searchFiles, &s.searchFiles));
    rows.push_back(ui::Toggle(L"settingspages", L"Windows settings",
        L"Finds pages of the Windows Settings app by what they are about - \"display\", "
        L"\"bluetooth\", \"mouse speed\".",
        &e.searchSettings, &s.searchSettings));
    rows.push_back(ui::Toggle(L"calc", L"Calculator",
        L"Type a sum like 1920*0.75 and the answer is the first result; Enter copies it.",
        &e.searchCalc, &s.searchCalc));
    rows.push_back(ui::Toggle(L"commands", L"Run what I type",
        L"Anything that looks like a path, a web address or a command line is offered to "
        L"run exactly as typed.",
        &e.searchCommands, &s.searchCommands));

    rows.push_back(ui::Section(L"Programs"));
    rows.push_back(ui::Toggle(L"programs", L"Programs without a shortcut",
        L"Also finds programs that have no Start menu entry - a portable tool, a game "
        L"unpacked into a folder - by walking Program Files and the like for .exe files.",
        &e.searchPrograms, &s.searchPrograms));
    {
        const std::wstring drives = FixedDrives();
        Row r = ui::Toggle(L"drives", L"Every drive",
            L"Looks for programs on every fixed drive, not only the one Windows is on - "
            L"where games and anything large usually live." +
            (drives.empty() ? std::wstring() : L"\r\n\r\nFixed drives on this PC: " + drives + L"."),
            &e.searchDrives, &s.searchDrives);
        r.enabled = []() { return Edit().searchPrograms; };
        rows.push_back(r);
        Row h = ui::Toggle(L"deepexe", L"Helper programs too",
            L"Includes the helper .exe files that apps ship to serve themselves - updaters, "
            L"crash reporters, tools. Many more results, rarely what you meant.",
            &e.searchDeepExe, &s.searchDeepExe);
        h.enabled = []() { return Edit().searchPrograms; };
        rows.push_back(h);
    }

    rows.push_back(ui::Section(L"File index"));
    {
        std::vector<int> depths(std::begin(kDepths), std::end(kDepths));
        std::vector<std::wstring> names;
        for (int d : kDepths) names.push_back(std::to_wstring(d) + L" levels");
        Row r = ui::ChoiceOf(L"depth", L"Folder depth",
            L"How many folders deep below each indexed folder to look. Deeper finds more "
            L"and takes longer; past eight you are indexing build output, not documents.",
            &e.searchDepth, &s.searchDepth, depths, names);
        r.enabled = files;
        rows.push_back(r);
    }
    {
        std::vector<int> caps(std::begin(kEntryCaps), std::end(kEntryCaps));
        std::vector<std::wstring> names;
        for (int c : kEntryCaps) names.push_back(Thousands(c) + L" items");
        Row r = ui::ChoiceOf(L"maxentries", L"Index size limit",
            L"The most files and folders kept in the index. Each costs about 300 bytes of "
            L"memory for as long as ProWindows runs.",
            &e.searchMaxEntries, &s.searchMaxEntries, caps, names);
        r.enabled = files;
        rows.push_back(r);
    }
    {
        Row r = ui::Toggle(L"hidden", L"Hidden files",
            L"Indexes hidden and system files too.", &e.searchHidden, &s.searchHidden);
        r.enabled = files;
        rows.push_back(r);
    }
    {
        Row r = ui::Info(L"status", L"Index",
            L"What is in the index now. It reflects the saved settings, not changes that "
            L"have not been applied.",
            []() -> std::wstring {
                if (!Saved().searchFiles) return L"Off";
                if (!SearchIndexReady()) return L"Indexing...";
                const int n = SearchIndexCount();
                return Thousands(n) + L" items  \x00B7  " +
                       std::to_wstring((std::max)(1, n * 300 / (1024 * 1024))) + L" MB";
            });
        rows.push_back(r);
        Row a = ui::Action(L"reindex", L"Rebuild the index",
            L"Walks the indexed folders again from scratch. Uses the saved settings: apply "
            L"first if you have changed them.",
            L"Rebuild", []() { SearchReindex(); SettingsToast(L"Rebuilding the file index"); });
        a.enabled = []() { return Saved().searchFiles; };
        rows.push_back(a);
    }

    rows.push_back(ui::Section(L"Folders to index"));
    const bool defaults = e.searchFolders.empty();
    const std::vector<std::wstring> shown = defaults ? SearchDefaultFolders() : e.searchFolders;
    for (size_t i = 0; i < shown.size(); ++i) {
        const std::wstring folder = shown[i];
        Row r;
        r.kind   = ui::Kind::Item;
        r.id     = L"folder:" + ToLower(folder);
        const size_t slash = folder.find_last_of(L"\\/");
        r.label  = (slash != std::wstring::npos && slash + 1 < folder.size()) ? folder.substr(slash + 1) : folder;
        r.detail = folder;
        r.raw    = true;
        r.enabled = files;
        if (defaults) {
            r.kind  = ui::Kind::Info;
            r.value = []() { return std::wstring(L"Default"); };
            r.help  = folder + L"\r\n\r\nOne of the default folders. Add a folder of your own "
                      L"and the list becomes yours to edit.";
        } else {
            r.button = L"Remove";
            r.help   = folder + L"\r\n\r\nDelete takes it off the list.";
            r.remove = [folder]() {
                auto& list = Edit().searchFolders;
                for (size_t k = 0; k < list.size(); ++k)
                    if (IEquals(list[k], folder)) { list.erase(list.begin() + (ptrdiff_t)k); break; }
                SettingsRebuild();
            };
        }
        rows.push_back(r);
    }
    {
        Row r = ui::Action(L"addfolder", L"Add a folder",
            L"Adds a folder to index. The first one you add replaces the defaults; the "
            L"default folders can be put back below.",
            L"Browse", []() {
                std::wstring folder;
                if (!BrowseForFolder(SettingsHwnd(), &folder) || folder.empty()) return;
                auto& list = Edit().searchFolders;
                for (const auto& have : list) if (IEquals(have, folder)) return;
                list.push_back(folder);
                SettingsRebuild();
            });
        r.enabled = files;
        rows.push_back(r);
        Row d = ui::Action(L"defaultfolders", L"Use the default folders",
            L"Goes back to indexing Desktop, Documents, Downloads, Pictures, Music and "
            L"Videos - wherever Windows keeps them for you.",
            L"Restore", []() { Edit().searchFolders.clear(); SettingsRebuild(); });
        d.enabled = []() { return Edit().searchFiles && !Edit().searchFolders.empty(); };
        rows.push_back(d);
    }
}

void ResetSearchPage() {
    Config d;
    d.LoadDefaults();
    Config& e = Edit();
    e.searchFiles = d.searchFiles;       e.searchSettings = d.searchSettings;
    e.searchCalc = d.searchCalc;         e.searchCommands = d.searchCommands;
    e.searchHidden = d.searchHidden;     e.searchDepth = d.searchDepth;
    e.searchMaxEntries = d.searchMaxEntries;
    e.searchFolders = d.searchFolders;   e.searchPrograms = d.searchPrograms;
    e.searchDrives = d.searchDrives;     e.searchDeepExe = d.searchDeepExe;
}

} // namespace awa
