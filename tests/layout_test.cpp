// ProWindows - geometry assertions for the layout engine.
//
// layout.cpp is pure: no Win32 state, no windows, just rectangles. That makes
// the one part of the tiler with real arithmetic in it testable without
// touching the user's desktop, which is worth having because the alternative -
// "put every running app in ignore_process first" - is slow and irreversible
// if you forget.
//
//   tests\run.bat
//
// Asserts on rects, never on screenshots, exactly as MAP.md says to.

#include "../src/layout.h"
#include <cstdio>
#include <algorithm>

using namespace awa;

static int g_fail = 0;
static int g_pass = 0;

static void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; return; }
    ++g_fail;
    std::printf("  FAIL  %s\n", what);
}

static HWND W(int i) { return (HWND)(intptr_t)(0x1000 + i); }

struct Plan {
    std::vector<std::pair<HWND, Rect>> v;
    Rect Of(HWND h) const {
        for (const auto& e : v) if (e.first == h) return e.second;
        return Rect();
    }
    bool Has(HWND h) const { return !Of(h).empty(); }
};

static Plan Run(LayoutKind kind, const Rect& work, const std::vector<HWND>& order,
                const ConsMap* cons, int gapInner = 0, int gapOuter = 0) {
    LayoutParams p;
    p.kind     = kind;
    p.work     = work;
    p.gapInner = gapInner;
    p.gapOuter = gapOuter;

    BspTree tree;
    for (HWND h : order) tree.Insert(h, nullptr, false);

    Plan plan;
    ComputeLayout(p, order, tree, &plan.v, cons);
    return plan;
}

// Nothing may overlap, and nothing may fall outside the work area.
static void CheckSane(const Plan& plan, const Rect& work, const char* label) {
    char buf[256];
    for (size_t i = 0; i < plan.v.size(); ++i) {
        const Rect& a = plan.v[i].second;
        std::snprintf(buf, sizeof(buf), "%s: window %zu inside the work area", label, i);
        Check(a.x >= work.x && a.y >= work.y &&
              a.right() <= work.right() && a.bottom() <= work.bottom(), buf);

        for (size_t j = i + 1; j < plan.v.size(); ++j) {
            const Rect& b = plan.v[j].second;
            const bool overlap = a.x < b.right() && b.x < a.right() &&
                                 a.y < b.bottom() && b.y < a.bottom();
            std::snprintf(buf, sizeof(buf), "%s: windows %zu and %zu do not overlap", label, i, j);
            Check(!overlap, buf);
        }
    }
}

// Every pixel of the work area belongs to some window. This is the assertion
// that catches "a window took a small space and left the rest empty".
static void CheckCovers(const Plan& plan, const Rect& work, const char* label) {
    long long covered = 0;
    for (const auto& e : plan.v) covered += (long long)e.second.w * e.second.h;
    const long long total = (long long)work.w * work.h;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: the layout fills the work area (%lld of %lld)",
                  label, covered, total);
    Check(covered == total, buf);
}

int main() {
    const Rect work(0, 0, 1920, 1080);

    // ---------------------------------------------------------------- 1
    {
        std::vector<HWND> order{ W(0), W(1) };
        Plan p = Run(LayoutKind::Dwindle, work, order, nullptr);
        CheckSane(p, work, "dwindle/plain");
        CheckCovers(p, work, "dwindle/plain");
        Check(p.Of(W(0)).w == 960 && p.Of(W(1)).w == 960, "dwindle/plain: an even split");
    }

    // ---------------------------------------------------------------- 2
    // A window that cannot shrink - Steam is the reported case. It must get its
    // minimum, and the space must come out of its neighbour rather than out of
    // the screen.
    {
        std::vector<HWND> order{ W(0), W(1) };
        ConsMap cons;
        SizeLimits steam; steam.minW = 1400;
        cons[W(0)] = steam;

        Plan p = Run(LayoutKind::Dwindle, work, order, &cons);
        CheckSane(p, work, "dwindle/min");
        CheckCovers(p, work, "dwindle/min");
        Check(p.Of(W(0)).w >= 1400, "dwindle/min: the window gets its minimum width");
        Check(p.Of(W(1)).w == 1920 - p.Of(W(0)).w, "dwindle/min: the neighbour takes the rest");
        Check(p.Of(W(1)).x == p.Of(W(0)).right(), "dwindle/min: they stay adjacent");
    }

    // ---------------------------------------------------------------- 3
    // A window that cannot grow - the file-copy dialog. Its surplus has to go
    // to the neighbour, or the screen ends up half empty.
    {
        std::vector<HWND> order{ W(0), W(1) };
        ConsMap cons;
        SizeLimits dialog; dialog.maxW = 300;
        cons[W(1)] = dialog;

        Plan p = Run(LayoutKind::Dwindle, work, order, &cons);
        CheckSane(p, work, "dwindle/max");
        CheckCovers(p, work, "dwindle/max");
        Check(p.Of(W(1)).w <= 300, "dwindle/max: the window is not forced wider than it allows");
        Check(p.Of(W(0)).w >= 1620, "dwindle/max: the surplus goes to the neighbour");
    }

    // ---------------------------------------------------------------- 4
    // Both at once, three windows, and gaps switched on so the slot-versus-
    // window arithmetic is exercised too.
    {
        std::vector<HWND> order{ W(0), W(1), W(2) };
        ConsMap cons;
        // The limits are on the axis these windows are actually split along.
        // See test 9 for the case that is on the other axis.
        // ('small' is a typedef for char in the RPC headers, hence 'capped'.)
        SizeLimits big;    big.minW    = 1000;  cons[W(0)] = big;
        SizeLimits capped; capped.maxW = 200;   cons[W(2)] = capped;

        Plan p = Run(LayoutKind::Dwindle, work, order, &cons, 8, 8);
        CheckSane(p, work, "dwindle/both");
        Check(p.Of(W(0)).w >= 1000, "dwindle/both: the minimum is met with gaps applied");
        Check(p.Of(W(2)).w <= 200,  "dwindle/both: the maximum is respected with gaps applied");
    }

    // ---------------------------------------------------------------- 5
    {
        std::vector<HWND> order{ W(0), W(1), W(2), W(3) };
        ConsMap cons;
        SizeLimits shortOne; shortOne.maxH = 240;
        cons[W(0)] = shortOne;

        Plan p = Run(LayoutKind::Grid, work, order, &cons);
        CheckSane(p, work, "grid");
        CheckCovers(p, work, "grid");
        Check(p.Of(W(0)).h <= 240, "grid: the row shrinks to what its member allows");
        Check(p.Of(W(2)).h >= 1080 - 240, "grid: the other row takes the height back");
    }

    // ---------------------------------------------------------------- 6
    {
        std::vector<HWND> order{ W(0), W(1), W(2) };
        ConsMap cons;
        SizeLimits wide; wide.minW = 1500;
        cons[W(0)] = wide;

        Plan p = Run(LayoutKind::Master, work, order, &cons);
        CheckSane(p, work, "master");
        CheckCovers(p, work, "master");
        Check(p.Of(W(0)).w >= 1500, "master: the master column widens to fit its window");
        Check(p.Of(W(1)).x == p.Of(W(0)).right(), "master: the stack starts where master ends");
    }

    // ---------------------------------------------------------------- 7
    // An impossible board: two windows that each demand more than half. Nobody
    // can be satisfied, but the result must still be a tiling - no overlap, no
    // window pushed off the screen.
    {
        std::vector<HWND> order{ W(0), W(1) };
        ConsMap cons;
        SizeLimits a; a.minW = 1500; cons[W(0)] = a;
        SizeLimits b; b.minW = 1500; cons[W(1)] = b;

        Plan p = Run(LayoutKind::Dwindle, work, order, &cons);
        CheckSane(p, work, "dwindle/impossible");
        CheckCovers(p, work, "dwindle/impossible");
        Check(p.Of(W(0)).w > 0 && p.Of(W(1)).w > 0, "dwindle/impossible: both are still placed");
    }

    // ---------------------------------------------------------------- 8
    // Constraints that cannot apply must change nothing at all.
    {
        std::vector<HWND> order{ W(0), W(1), W(2) };
        ConsMap cons;
        cons[W(0)] = SizeLimits{};            // wide open

        Plan withCons = Run(LayoutKind::Dwindle, work, order, &cons);
        Plan without   = Run(LayoutKind::Dwindle, work, order, nullptr);
        bool same = withCons.v.size() == without.v.size();
        for (size_t i = 0; same && i < withCons.v.size(); ++i)
            same = withCons.v[i].second == without.v[i].second;
        Check(same, "unconstrained windows lay out exactly as before");
    }

    // ---------------------------------------------------------------- 9
    // The case that used to be recorded here as "geometry cannot fix".
    //
    // Three windows in dwindle sit in nested VERTICAL splits, so every one of
    // them spans the full height. A window with a maximum HEIGHT - a file-copy
    // dialog is the real example - could not be satisfied by moving a split,
    // because there was no horizontal split to move: it sat in a tall slot
    // with empty desktop under it.
    //
    // The split it sits under is now turned to run the other way, so the
    // height it cannot use goes to its sibling instead of to nobody.
    {
        std::vector<HWND> order{ W(0), W(1), W(2) };
        ConsMap cons;
        SizeLimits shortDialog; shortDialog.maxH = 200;
        cons[W(2)] = shortDialog;

        Plan p = Run(LayoutKind::Dwindle, work, order, &cons);
        CheckSane(p, work, "dwindle/cross-axis");
        CheckCovers(p, work, "dwindle/cross-axis");
        Check(p.Of(W(2)).h <= 200,
              "dwindle/cross-axis: the short window is not given height it cannot use");
        Check(p.Of(W(1)).h >= 1080 - 200,
              "dwindle/cross-axis: its sibling takes the height back");
        Check(p.Of(W(1)).w == p.Of(W(2)).w,
              "dwindle/cross-axis: the pair is stacked, not side by side");
    }

    // ---------------------------------------------------------------- 9b
    // Turning a split is only ever done to gain something. A wide window with
    // a maximum WIDTH already sits in the right kind of split; nothing may
    // move. And a board with no limits at all is never touched (see 8).
    {
        std::vector<HWND> order{ W(0), W(1) };
        ConsMap cons;
        SizeLimits narrow; narrow.maxW = 300;
        cons[W(1)] = narrow;

        Plan p = Run(LayoutKind::Dwindle, work, order, &cons);
        CheckSane(p, work, "dwindle/keep");
        Check(p.Of(W(0)).y == p.Of(W(1)).y && p.Of(W(1)).w <= 300,
              "dwindle/keep: a split already running the right way is left alone");
    }

    // ---------------------------------------------------------------- 9c
    // Two windows that cannot stand side by side but can stack: the split is
    // turned so that both get their minimum rather than both overflowing.
    {
        std::vector<HWND> order{ W(0), W(1) };
        ConsMap cons;
        SizeLimits wide; wide.minW = 1200;
        cons[W(0)] = wide;
        cons[W(1)] = wide;

        Plan p = Run(LayoutKind::Dwindle, work, order, &cons);
        CheckSane(p, work, "dwindle/turn-to-fit");
        CheckCovers(p, work, "dwindle/turn-to-fit");
        Check(p.Of(W(0)).w >= 1200 && p.Of(W(1)).w >= 1200,
              "dwindle/turn-to-fit: both get their minimum width by stacking");
        Check(SqueezedWindows(order, p.v, cons).empty(),
              "dwindle/turn-to-fit: nothing is reported squeezed");
    }

    // ---------------------------------------------------------------- 9d
    // And when neither way round fits, the plan says so, so the caller can
    // take a window out rather than let two of them overlap on screen.
    {
        std::vector<HWND> order{ W(0), W(1) };
        ConsMap cons;
        SizeLimits huge; huge.minW = 1200; huge.minH = 700;
        cons[W(0)] = huge;
        cons[W(1)] = huge;

        Plan p = Run(LayoutKind::Dwindle, work, order, &cons);
        CheckSane(p, work, "dwindle/squeezed");
        const std::vector<HWND> squeezed = SqueezedWindows(order, p.v, cons);
        Check(!squeezed.empty(), "dwindle/squeezed: an impossible pair is reported");

        // Without one of them, the other has the board and is no longer squeezed.
        std::vector<HWND> alone{ W(0) };
        Plan q = Run(LayoutKind::Dwindle, work, alone, &cons);
        Check(SqueezedWindows(alone, q.v, cons).empty(),
              "dwindle/squeezed: on its own, it fits");

        // A window the plan left out entirely counts as squeezed too.
        std::vector<HWND> ghost{ W(0), W(9) };
        Check(SqueezedWindows(ghost, q.v, cons).size() == 1,
              "squeezed: an unplaced window is reported");
    }

    // ---------------------------------------------------------------- 10
    // The board that came out of the first live run, kept because it is the
    // case that showed the constraint solver was not enough on its own.
    //
    // Tree: V(V(V(A, min900), max420), B) - the shape dwindle actually built.
    // Every window's minimum can be met, but not everyone's fair share, so
    // something has to give. Clamping alone gave the deepest windows their bare
    // minimum (122px) while B kept 750px, which is a useless layout even though
    // every limit was technically respected. The shortfall is shared now.
    {
        const Rect screen(0, 0, 1920, 1032);
        std::vector<HWND> order{ W(3), W(2), W(1), W(0) };   // B, max, min, A

        BspTree tree;
        tree.Insert(W(3), nullptr, true);
        tree.Insert(W(2), W(3), true);
        tree.Insert(W(1), W(2), true);
        tree.Insert(W(0), W(1), true);

        ConsMap cons;
        SizeLimits plainA;  plainA.minW = 122; plainA.minH = 32;   cons[W(0)] = plainA;
        SizeLimits big;     big.minW    = 886; big.minH    = 493;  cons[W(1)] = big;
        SizeLimits dialog;  dialog.minW = 122; dialog.minH = 32;
                            dialog.maxW = 406; dialog.maxH = 253;  cons[W(2)] = dialog;
        SizeLimits plainB;  plainB.minW = 122; plainB.minH = 32;   cons[W(3)] = plainB;

        LayoutParams p;
        p.kind = LayoutKind::Dwindle;
        p.work = screen;
        p.gapInner = 8;
        p.gapOuter = 12;

        Plan plan;
        ComputeLayout(p, order, tree, &plan.v, &cons);

        CheckSane(plan, screen, "live board");
        Check(plan.Of(W(1)).w >= 886, "live board: the big window still gets its minimum");
        Check(plan.Of(W(2)).w <= 406, "live board: the capped window is still capped");

        // The point of the case: nobody is left on the floor while a sibling
        // hoards the slack. Every ordinary window gets comfortably more than
        // its minimum on both axes.
        int narrowest = 1 << 20;
        bool roomy = true;
        for (const auto& e : plan.v) {
            if (e.first == W(1)) continue;          // the big one is meant to be big
            narrowest = (std::min)(narrowest, e.second.w);
            const SizeLimits& l = cons[e.first];
            if (e.second.w < l.minW * 2 || e.second.h < l.minH * 2) roomy = false;
        }
        Check(narrowest >= 200, "live board: no window is squeezed to its bare minimum");
        Check(roomy, "live board: the slack is shared, not hoarded");

        // The capped dialog used to stand in a full-height column and leave
        // 406 x 755 pixels of bare desktop under it. Turning the split it sits
        // under is what this board was kept to show could not be done; now
        // its unusable share is a fraction of that.
        const Rect d = plan.Of(W(2));
        const long long unusable = (long long)d.w * d.h -
            (long long)(std::min)(d.w, 406) * (std::min)(d.h, 253);
        Check(unusable < 100000, "live board: the capped dialog wastes far less of its slot");
    }

    // ------------------------------------------------------------------ 11
    // Dropping a window on a side puts it on that side.
    //
    // This is the bug the whole gesture existed to have: the old code swapped
    // the dragged window with whatever was under the pointer, so the result
    // kept the split the tree already had. Drop a window on the right of a
    // tile that happened to be part of a stacked split and it landed
    // underneath - "sometimes it goes right, sometimes it goes down", from
    // what felt to the user like the same gesture both times.
    {
        const Rect screen(0, 0, 1600, 900);

        // Two windows, stacked: A on top of B. This is the shape that used to
        // produce the wrong answer.
        BspTree tree;
        tree.Insert(W(0), nullptr, false);
        tree.Insert(W(1), W(0), false);

        // Force the stack, whatever Insert's longer-axis rule chose.
        {
            std::vector<std::pair<HWND, Rect>> probe;
            tree.Compute(screen, &probe);
        }
        tree.MoveBeside(W(1), W(0), Dir::Down);       // B below A, explicitly

        LayoutParams p;
        p.kind = LayoutKind::Dwindle;
        p.work = screen;
        p.gapInner = 0;
        p.gapOuter = 0;

        Plan stacked;
        std::vector<HWND> order = { W(0), W(1) };
        ComputeLayout(p, order, tree, &stacked.v, nullptr);
        Check(stacked.Of(W(0)).y < stacked.Of(W(1)).y,
              "drop: the board starts out stacked");

        // Now drag A and drop it on the right of B.
        tree.MoveBeside(W(0), W(1), Dir::Right);
        order = { W(1), W(0) };

        Plan after;
        ComputeLayout(p, order, tree, &after.v, nullptr);
        CheckSane(after, screen, "drop right");

        const Rect a = after.Of(W(0)), b = after.Of(W(1));
        Check(a.x >= b.right() - 1, "drop right: the dropped window is on the right");
        Check(b.x < a.x,            "drop right: the window already there is on the left");
        Check(a.h == screen.h && b.h == screen.h,
              "drop right: the split is side by side, not stacked");
    }

    // ------------------------------------------------------------------ 12
    // The same drop, from every direction, on a board deep enough that the
    // tree has a shape to get wrong.
    {
        const Rect screen(0, 0, 1920, 1080);
        LayoutParams p;
        p.kind = LayoutKind::Dwindle;
        p.work = screen;
        p.gapInner = 0;
        p.gapOuter = 0;

        struct Case { Dir side; const char* label; };
        const Case cases[] = {
            { Dir::Right, "right" }, { Dir::Left,  "left"  },
            { Dir::Down,  "down"  }, { Dir::Up,    "up"    },
        };

        for (const Case& c : cases) {
            BspTree tree;
            std::vector<HWND> order;
            for (int i = 0; i < 4; ++i) { tree.Insert(W(i), nullptr, false); order.push_back(W(i)); }

            // W(3) is dragged onto W(1).
            tree.MoveBeside(W(3), W(1), c.side);
            order = { W(0), W(1), W(2) };
            const bool first = (c.side == Dir::Left || c.side == Dir::Up);
            order.insert(order.begin() + (first ? 1 : 2), W(3));

            Plan plan;
            ComputeLayout(p, order, tree, &plan.v, nullptr);

            char buf[128];
            std::snprintf(buf, sizeof(buf), "drop %s: every window is still placed", c.label);
            Check(plan.v.size() == 4, buf);
            std::snprintf(buf, sizeof(buf), "drop %s: nothing overlaps", c.label);
            CheckSane(plan, screen, buf);

            const Rect moved  = plan.Of(W(3));
            const Rect target = plan.Of(W(1));

            std::snprintf(buf, sizeof(buf), "drop %s: it lands on that side of the target", c.label);
            switch (c.side) {
                case Dir::Right: Check(moved.x >= target.x, buf); break;
                case Dir::Left:  Check(moved.x <= target.x, buf); break;
                case Dir::Down:  Check(moved.y >= target.y, buf); break;
                default:         Check(moved.y <= target.y, buf); break;
            }

            // The two of them share one split, so they line up exactly on the
            // axis the split does not run along.
            std::snprintf(buf, sizeof(buf), "drop %s: it shares a split with the target", c.label);
            if (DirHorizontal(c.side))
                Check(moved.y == target.y && moved.h == target.h, buf);
            else
                Check(moved.x == target.x && moved.w == target.w, buf);
        }
    }

    // ------------------------------------------------------------------ 13
    // A drop that landed on bare desktop rather than on any window: the
    // dragged window takes that whole edge of the screen, and everything else
    // keeps its arrangement in the other half.
    {
        const Rect screen(0, 0, 1600, 900);
        LayoutParams p;
        p.kind = LayoutKind::Dwindle;
        p.work = screen;
        p.gapInner = 0;
        p.gapOuter = 0;

        BspTree tree;
        std::vector<HWND> order;
        for (int i = 0; i < 3; ++i) { tree.Insert(W(i), nullptr, false); order.push_back(W(i)); }

        tree.MoveToEdge(W(2), Dir::Left);
        order = { W(2), W(0), W(1) };

        Plan plan;
        ComputeLayout(p, order, tree, &plan.v, nullptr);
        CheckSane(plan, screen, "drop on the edge");

        const Rect moved = plan.Of(W(2));
        Check(moved.x == screen.x, "edge drop: it sits against that edge");
        Check(moved.h == screen.h, "edge drop: it runs the full height");
        Check(plan.Of(W(0)).x >= moved.right() - 1 && plan.Of(W(1)).x >= moved.right() - 1,
              "edge drop: everything else is in the other half");
    }

    // ------------------------------------------------------------------ 14
    // Dropping a window next to itself, or next to a window that is not in
    // the tree, must not lose it or corrupt the tree.
    {
        BspTree tree;
        tree.Insert(W(0), nullptr, false);
        tree.Insert(W(1), nullptr, false);

        tree.MoveBeside(W(0), W(0), Dir::Right);       // onto itself
        std::vector<HWND> seen;
        tree.Collect(&seen);
        Check(seen.size() == 2, "drop onto itself: nothing is lost");

        tree.MoveBeside(W(1), W(9), Dir::Left);        // onto a stranger
        seen.clear();
        tree.Collect(&seen);
        Check(seen.size() == 2, "drop onto an unknown window: nothing is lost");
        Check(tree.Contains(W(0)) && tree.Contains(W(1)),
              "drop onto an unknown window: both windows are still there");
    }

    // ------------------------------------------------------------------ 15
    // config.ini round-trip.
    //
    // The learned size limits are written by the program and read back by it,
    // and nobody ever looks at those lines - so a formatting mistake would
    // show up as "Steam started overlapping again after a restart" rather than
    // as anything obviously wrong with the file. Write one, read it back,
    // compare. Uses a temporary file, never the user's real config.
    {
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring path = std::wstring(tempDir) + L"prowindows_roundtrip.ini";

        Config out;
        out.LoadDefaults();
        out.pauseForFullscreen = false;
        out.gapInner = 13;

        // The three settings added since this test was written. Each is stored
        // in a different shape - a plain flag, a number, and a permutation
        // written out by name - and the last of those is the one with room to
        // go wrong.
        out.modDrag         = false;
        out.searchDrives    = false;
        out.searchDeepExe   = true;
        out.searchMaxPrograms = 1234;
        out.monOrder[0] = MON_NET;
        out.monOrder[1] = MON_GPU;
        out.monOrder[2] = MON_CPU;
        out.monOrder[3] = MON_RAM;
        out.monOrder[4] = MON_VRAM;
        out.monOrder[5] = MON_CPUTEMP;
        out.monOrder[6] = MON_GPUTEMP;
        out.monOrder[7] = MON_DISK;

        Config::RememberedLimits steam;
        steam.minW = 1024; steam.minH = 600; steam.tooLarge = true;
        out.learnedLimits[L"steam.exe|sdl_app"] = steam;

        Config::RememberedLimits dialog;
        dialog.minW = 120; dialog.minH = 90; dialog.maxW = 406; dialog.maxH = 253;
        out.learnedLimits[L"explorer.exe|operationstatuswindow"] = dialog;

        Check(out.SaveToFile(path), "config: the file is written");

        Config in;
        in.LoadDefaults();
        Check(in.LoadFromFile(path), "config: the file is read back");

        Check(in.gapInner == 13, "config: an ordinary value survives the round trip");
        Check(in.pauseForFullscreen == false,
              "config: pause_for_fullscreen survives the round trip");
        Check(in.learnedLimits.size() == 2, "config: both learned limits come back");

        Check(in.modDrag == false, "config: mod_drag survives the round trip");
        Check(in.searchDrives == false, "config: search_drives survives the round trip");
        Check(in.searchDeepExe == true, "config: search_deep_exe survives the round trip");
        Check(in.searchMaxPrograms == 1234,
              "config: search_max_programs survives the round trip");

        // A file held open without sharing - an editor, a sync client - must
        // make the save say it failed and leave what was there alone. Apply
        // used to ignore this answer and reload the old file over the user's
        // changes.
        {
            HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Config other = out;
            other.gapInner = 21;
            Check(held != INVALID_HANDLE_VALUE && !other.SaveToFile(path),
                  "config: a save onto a locked file reports failure");
            if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
            Config back;
            back.LoadDefaults();
            Check(back.LoadFromFile(path) && back.gapInner == 13,
                  "config: a failed save leaves the old file intact");
            Check(GetFileAttributesW((path + L".new").c_str()) == INVALID_FILE_ATTRIBUTES,
                  "config: a failed save leaves no temporary file behind");
        }

        bool orderKept = true;
        for (int i = 0; i < MON_METRIC_COUNT; ++i)
            if (in.monOrder[i] != out.monOrder[i]) orderKept = false;
        Check(orderKept, "config: the readout order survives the round trip");

        // A hand-edited file naming a metric twice, leaving one out, and
        // inventing another. It must come back as a permutation - every metric
        // exactly once - or the overlay indexes past the end of its arrays.
        {
            int broken[MON_METRIC_COUNT] = { MON_GPU, MON_GPU, -1, MON_NET, 99, 0, 0, 0 };
            MonitorNormaliseOrder(broken);
            bool seen[MON_METRIC_COUNT] = {};
            bool onceEach = true;
            for (int i = 0; i < MON_METRIC_COUNT; ++i) {
                if (broken[i] < 0 || broken[i] >= MON_METRIC_COUNT) { onceEach = false; break; }
                if (seen[broken[i]]) { onceEach = false; break; }
                seen[broken[i]] = true;
            }
            Check(onceEach, "config: a broken readout order is repaired to a permutation");
            Check(broken[0] == MON_GPU,
                  "config: and what the file did say keeps its place");
        }

        auto s = in.learnedLimits.find(L"steam.exe|sdl_app");
        Check(s != in.learnedLimits.end(), "config: the minimum-size entry is found");
        if (s != in.learnedLimits.end()) {
            Check(s->second.minW == 1024 && s->second.minH == 600,
                  "config: a learned minimum comes back unchanged");
            Check(s->second.tooLarge, "config: the \"will not fit\" flag comes back");
        }

        auto d = in.learnedLimits.find(L"explorer.exe|operationstatuswindow");
        Check(d != in.learnedLimits.end(), "config: the maximum-size entry is found");
        if (d != in.learnedLimits.end()) {
            Check(d->second.maxW == 406 && d->second.maxH == 253,
                  "config: a learned maximum comes back unchanged");
            Check(!d->second.tooLarge,
                  "config: an entry without the flag does not gain one");
        }

        // Bindings are the part the file is authoritative about; a change to
        // how the learned block is written must not disturb them.
        Check(in.binds.size() == out.binds.size(),
              "config: every binding survives the round trip");

        DeleteFileW(path.c_str());
    }

    // ---------------------------------------------------------- togglesplit
    // Two windows on a wide screen come out side by side, because Insert
    // splits the longer axis. ToggleSplit has to stack them, and put them back.
    {
        const Rect screen(0, 0, 1920, 1080);

        LayoutParams p;
        p.kind = LayoutKind::Dwindle;
        p.work = screen;
        p.gapInner = 0;
        p.gapOuter = 0;

        std::vector<HWND> order{ W(0), W(1) };
        BspTree tree;
        tree.Insert(W(0), nullptr, false);
        tree.Insert(W(1), W(0), false);

        Plan plan;
        ComputeLayout(p, order, tree, &plan.v, nullptr);
        Check(plan.Of(W(0)).y == plan.Of(W(1)).y,
              "togglesplit: a wide screen starts side by side");

        Check(tree.ToggleSplit(W(1)), "togglesplit: the flip is accepted");
        plan.v.clear();
        ComputeLayout(p, order, tree, &plan.v, nullptr);
        Check(plan.Of(W(0)).x == plan.Of(W(1)).x,
              "togglesplit: they are stacked afterwards");
        Check(plan.Of(W(0)).y != plan.Of(W(1)).y,
              "togglesplit: and no longer share a row");
        CheckSane(plan, screen, "togglesplit stacked");
        CheckCovers(plan, screen, "togglesplit stacked");

        Check(tree.ToggleSplit(W(1)), "togglesplit: flipping again is accepted");
        plan.v.clear();
        ComputeLayout(p, order, tree, &plan.v, nullptr);
        Check(plan.Of(W(0)).y == plan.Of(W(1)).y,
              "togglesplit: the second press puts them back");

        // One window has no split to flip, and saying so is what lets the
        // caller skip a retile that would do nothing.
        BspTree lone;
        lone.Insert(W(0), nullptr, false);
        Check(!lone.ToggleSplit(W(0)), "togglesplit: a lone window declines");
        Check(!lone.ToggleSplit(W(7)), "togglesplit: an unknown window declines");
    }

    // ---------------------------------------------------------- swapsplit
    // The point of swapping a split rather than two windows: the stack of two
    // has to cross to the other side *as a stack*, keeping its own shape.
    {
        const Rect screen(0, 0, 1920, 1080);

        LayoutParams p;
        p.kind = LayoutKind::Dwindle;
        p.work = screen;
        p.gapInner = 0;
        p.gapOuter = 0;

        // Laid out between inserts, exactly as the tiler does it: Insert splits
        // the *longer* edge of the target's most recent rect, so a tree built
        // without a layout pass in between has every split the same way round
        // and cannot produce the shape this test is about.
        BspTree tree;
        std::vector<HWND> pair{ W(0), W(1) };
        tree.Insert(W(0), nullptr, false);
        tree.Insert(W(1), W(0), false);          // 1920 wide: side by side
        Plan seed;
        ComputeLayout(p, pair, tree, &seed.v, nullptr);

        std::vector<HWND> order{ W(0), W(1), W(2) };
        tree.Insert(W(2), W(1), false);          // W(1) is now tall: stacked

        // An uneven root split, so mirroring the ratio is actually tested
        // rather than being hidden by two equal halves.
        Check(tree.Resize(W(0), Dir::Right, 0.1f), "swapsplit: the root split moves");

        Plan before;
        ComputeLayout(p, order, tree, &before.v, nullptr);
        Check(before.Of(W(0)).x < before.Of(W(1)).x,
              "swapsplit: the first window starts on the left");
        Check(before.Of(W(1)).x == before.Of(W(2)).x,
              "swapsplit: the other two start as a stack");
        const int stackHeight = before.Of(W(1)).h;

        Check(tree.SwapSplit(W(0)), "swapsplit: the swap is accepted");

        Plan after;
        ComputeLayout(p, order, tree, &after.v, nullptr);
        Check(after.Of(W(0)).x > after.Of(W(1)).x,
              "swapsplit: the single window crossed to the right");
        Check(after.Of(W(1)).x == after.Of(W(2)).x,
              "swapsplit: the pair stayed a stack");
        Check(after.Of(W(1)).h == stackHeight,
              "swapsplit: and kept its own proportions");
        // The ratio is mirrored, so each half keeps the width it had rather
        // than swapping sides and inheriting the other's.
        Check(after.Of(W(0)).w == before.Of(W(0)).w,
              "swapsplit: the mirrored ratio keeps each half its own width");
        CheckSane(after, screen, "swapsplit");
        CheckCovers(after, screen, "swapsplit");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
