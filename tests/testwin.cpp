// ProWindows - a window that misbehaves exactly the way you ask it to.
//
// The first three bugs this was written for are all about what a window will
// accept: one that refuses to shrink (Steam), one that refuses to grow (a
// file-copy dialog), and one that refuses to move at all. Reproducing those
// with real applications means installing them and hoping they behave the same
// on the next machine. This does it exactly, repeatably, and closes when you
// are done.
//
// The later ones are about what a window looks like at the instant it is
// created. Almost every real application creates its top-level window and only
// then gives it a title, shows it, or finishes applying its styles - and a
// tiling manager that classifies the window once, on the way past, sees a
// nameless invisible thing and writes it off for good. That is "it did not get
// arranged until I moved it", and --late-title is the one that reproduces it
// most reliably, because it is what Electron, Chrome and Qt all do.
//
//   testwin.exe [--min WxH] [--max WxH] [--name TEXT]
//               [--late-title MS] [--late-show MS] [--late-resizable MS]
//
// It paints its own limits, so a screenshot of a tiled desktop is readable.
//
// To check the fix: run two or three with --late-title 400 and watch them take
// their tiles without being touched. Before the fix they sat where Windows put
// them until they were clicked on.

#include <windows.h>
#include <string>
#include <cstdio>

static int  g_minW = 0, g_minH = 0;
static int  g_maxW = 0, g_maxH = 0;      // 0 = no limit
static std::wstring g_name = L"PWTEST";

// Delays, in milliseconds, before the window becomes what it will finally be.
// 0 means "do it up front", i.e. behave like a well-mannered window.
static int g_lateTitle     = 0;
static int g_lateShow      = 0;
static int g_lateResizable = 0;

enum : UINT_PTR {
    TIMER_TITLE = 1,
    TIMER_SHOW,
    TIMER_RESIZABLE,
};

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETMINMAXINFO: {
            // The whole point of this program.
            auto* mmi = (MINMAXINFO*)lp;
            if (g_minW) mmi->ptMinTrackSize.x = g_minW;
            if (g_minH) mmi->ptMinTrackSize.y = g_minH;
            if (g_maxW) mmi->ptMaxTrackSize.x = g_maxW;
            if (g_maxH) mmi->ptMaxTrackSize.y = g_maxH;
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT r; GetClientRect(h, &r);
            FillRect(dc, &r, (HBRUSH)(COLOR_WINDOW + 1));

            RECT fr; GetWindowRect(h, &fr);
            wchar_t buf[256];
            _snwprintf_s(buf, _TRUNCATE,
                         L"%s\n\n%ld x %ld on screen\nmin %d x %d\nmax %d x %d",
                         g_name.c_str(),
                         fr.right - fr.left, fr.bottom - fr.top,
                         g_minW, g_minH, g_maxW, g_maxH);
            r.left += 16; r.top += 16;
            DrawTextW(dc, buf, -1, &r, DT_LEFT | DT_TOP | DT_NOCLIP);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(h, nullptr, TRUE);
            return 0;

        // Each of these is one application finishing a job it started when the
        // window was created. All three make Classify say no until they land.
        case WM_TIMER:
            switch (wp) {
                case TIMER_TITLE:
                    KillTimer(h, TIMER_TITLE);
                    SetWindowTextW(h, g_name.c_str());
                    break;
                case TIMER_SHOW:
                    KillTimer(h, TIMER_SHOW);
                    ShowWindow(h, SW_SHOW);
                    break;
                case TIMER_RESIZABLE:
                    KillTimer(h, TIMER_RESIZABLE);
                    SetWindowLongW(h, GWL_STYLE,
                                   GetWindowLongW(h, GWL_STYLE) | WS_THICKFRAME);
                    // Styles changed after creation do not take effect until
                    // the frame is recalculated.
                    SetWindowPos(h, nullptr, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
                    break;
                default:
                    break;
            }
            InvalidateRect(h, nullptr, TRUE);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ParseSize(const wchar_t* s, int* w, int* hgt) {
    int a = 0, b = 0;
    if (swscanf_s(s, L"%dx%d", &a, &b) == 2) { *w = a; *hgt = b; }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i + 1 < argc; ++i) {
        if      (!lstrcmpiW(argv[i], L"--min"))  ParseSize(argv[++i], &g_minW, &g_minH);
        else if (!lstrcmpiW(argv[i], L"--max"))  ParseSize(argv[++i], &g_maxW, &g_maxH);
        else if (!lstrcmpiW(argv[i], L"--name")) g_name = argv[++i];
        else if (!lstrcmpiW(argv[i], L"--late-title"))     g_lateTitle     = _wtoi(argv[++i]);
        else if (!lstrcmpiW(argv[i], L"--late-show"))      g_lateShow      = _wtoi(argv[++i]);
        else if (!lstrcmpiW(argv[i], L"--late-resizable")) g_lateResizable = _wtoi(argv[++i]);
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PWTestWindow";
    RegisterClassW(&wc);

    // A perfectly ordinary top-level window: resizable, titled, unowned. Exactly
    // what the tiler expects to be able to arrange - unless one of the --late
    // switches is on, in which case it starts out missing whichever piece was
    // asked for and acquires it a moment later, the way a real application
    // does.
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (g_lateResizable) style &= ~(DWORD)WS_THICKFRAME;

    HWND h = CreateWindowExW(0, wc.lpszClassName,
                             g_lateTitle ? L"" : g_name.c_str(),
                             style, CW_USEDEFAULT, CW_USEDEFAULT,
                             700, 480, nullptr, nullptr, inst, nullptr);
    if (!h) return 1;

    if (g_lateTitle)     SetTimer(h, TIMER_TITLE,     (UINT)g_lateTitle,     nullptr);
    if (g_lateResizable) SetTimer(h, TIMER_RESIZABLE, (UINT)g_lateResizable, nullptr);

    if (g_lateShow) SetTimer(h, TIMER_SHOW, (UINT)g_lateShow, nullptr);
    else            ShowWindow(h, SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
