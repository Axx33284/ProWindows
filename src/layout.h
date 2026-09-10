// ProWindows - tiling layout engine
#pragma once
#include "common.h"
#include "config.h"
#include "winutil.h"

namespace awa {

// Per-window size limits the layout has to work within, keyed by window.
// Everything here is in visible-frame pixels, matching SizeLimits.
using ConsMap = std::unordered_map<HWND, SizeLimits>;

// ---------------------------------------------------------------- BSP tree
// Hyprland's "dwindle": every insertion splits the focused leaf along its longer
// axis, producing the familiar spiralling partition.
struct BspNode {
    HWND     hwnd     = nullptr;   // meaningful only for leaves
    bool     vertical = true;      // internal node: true = side-by-side split
    float    ratio    = 0.5f;      // share of the first child
    BspNode* a        = nullptr;
    BspNode* b        = nullptr;
    BspNode* parent   = nullptr;
    Rect     rect;                 // rect from the most recent layout pass

    // Filled by the measure pass that runs before every compute, never stored
    // between passes: a window's limits change when it is themed, DPI-moved or
    // switched between windowed modes.
    int minW = 0, minH = 0, maxW = kNoLimit, maxH = kNoLimit;
    int leaves = 1;                // windows in this subtree

    bool IsLeaf() const { return a == nullptr && b == nullptr; }
};

class BspTree {
public:
    BspTree() = default;
    ~BspTree() { Clear(); }
    BspTree(const BspTree&) = delete;
    BspTree& operator=(const BspTree&) = delete;

    BspTree(BspTree&& o) noexcept : root_(o.root_) { o.root_ = nullptr; }
    BspTree& operator=(BspTree&& o) noexcept {
        if (this != &o) { Clear(); root_ = o.root_; o.root_ = nullptr; }
        return *this;
    }

    void Clear();
    bool Empty() const { return root_ == nullptr; }

    // `beside` selects the leaf to split; nullptr splits the first leaf found.
    void Insert(HWND h, HWND beside, bool newWindowFirst);
    void Remove(HWND h);

    // Put `moving` immediately `side` of `target`, whatever shape the tree
    // currently has. This is what a drag has to call, and it is deliberately
    // NOT Insert(): Insert splits the target along its *longer* axis, which is
    // right for a window appearing out of nowhere and wrong for one the user
    // has just dropped on a particular edge. Dropping a window on the right of
    // a tall tile has to produce a side-by-side split even though the tile is
    // taller than it is wide - otherwise the window lands underneath, which is
    // exactly the complaint.
    //
    // `moving` need not already be in the tree. If `target` is not, the call
    // degrades to a plain Insert rather than doing nothing.
    void MoveBeside(HWND moving, HWND target, Dir side);

    // Put `moving` against one edge of the whole tree: a full-height column
    // down that side, or a full-width row across it. For a drop that landed on
    // bare desktop rather than on any particular window.
    void MoveToEdge(HWND moving, Dir side);
    bool Contains(HWND h) const { return FindLeaf(h) != nullptr; }
    void Swap(HWND x, HWND y);
    void Promote(HWND h);                    // swap with the tree's first leaf

    // Flip the split `h` sits under from side-by-side to stacked, or back.
    // Hyprland's `togglesplit`, and the reason it exists there: the automatic
    // choice - split the longer edge - is right nearly always and wrong for
    // the one pair you happen to be looking at, and the alternative to
    // toggling is dragging windows around until the shape comes out.
    //
    // False when `h` is not in the tree or is the only window in it, which is
    // what lets the caller skip the retile rather than run one that does
    // nothing.
    bool ToggleSplit(HWND h);

    // Exchange the two halves of the split `h` sits under, so the window
    // moves to the other side of its sibling *subtree* rather than trading
    // places with one window. Hyprland's `swapsplit`: with one window on the
    // left and three stacked on the right, this is the way to get the three
    // on the left, and swapping window for window cannot express it at all.
    bool SwapSplit(HWND h);
    bool Resize(HWND h, Dir d, float delta); // adjust the nearest matching split

    void Collect(std::vector<HWND>* out) const;

    // The tree as text, e.g. "V(H(a,b),c)" with each leaf's measured minimum.
    // The layout is only ever as good as the tree it is given, and the shape is
    // otherwise invisible: worth being able to print when a result surprises.
    std::wstring Describe() const;
    void Compute(const Rect& area, std::vector<std::pair<HWND, Rect>>* out);

    // Drop leaves whose HWND is no longer in `alive`.
    void Prune(const std::unordered_set<HWND>& alive);

    // Same, but honouring what each window will actually accept. A split is
    // moved off its ratio only as far as the constraints demand, so a window
    // that cannot shrink takes the space from its sibling instead of spilling
    // over it, and one that cannot grow hands its surplus back.
    void Compute(const Rect& area, const ConsMap* cons,
                 std::vector<std::pair<HWND, Rect>>* out);

private:
    BspNode* root_ = nullptr;

    BspNode* FindLeaf(HWND h) const;
    static BspNode* FirstLeaf(BspNode* n);
    static void DestroyTree(BspNode* n);
    static void CollectRec(BspNode* n, std::vector<HWND>* out);
    static void MeasureRec(BspNode* n, const ConsMap* cons);
    void ComputeRec(BspNode* n, const Rect& area, std::vector<std::pair<HWND, Rect>>* out);
};

// ---------------------------------------------------------------- layouts
struct LayoutParams {
    LayoutKind kind        = LayoutKind::Dwindle;
    Rect       work;                 // monitor work area
    int        gapInner    = 8;
    int        gapOuter    = 8;
    float      masterRatio = 0.55f;
    int        masterCount = 1;
};

// Produces the final on-screen rect for every tiled window, gaps applied.
//
// `cons` is optional. When present, every algorithm respects each window's
// minimum and maximum size instead of handing out slots it knows will be
// refused. Space a window cannot use goes to its neighbours rather than being
// left blank, which is the whole point.
void ComputeLayout(const LayoutParams& p,
                   const std::vector<HWND>& order,
                   BspTree& tree,
                   std::vector<std::pair<HWND, Rect>>* out,
                   const ConsMap* cons = nullptr);

} // namespace awa
