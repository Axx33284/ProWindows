#include "layout.h"

namespace awa {

// The smallest slot any window is ever given. Small enough that it never
// overrides a real preference, large enough that the window is still there to
// be grabbed.
static constexpr int kMinLeafPx = 48;

// ---------------------------------------------------------------- BspTree
void BspTree::DestroyTree(BspNode* n) {
    if (!n) return;
    DestroyTree(n->a);
    DestroyTree(n->b);
    delete n;
}

void BspTree::Clear() {
    DestroyTree(root_);
    root_ = nullptr;
}

BspNode* BspTree::FirstLeaf(BspNode* n) {
    while (n && !n->IsLeaf()) n = n->a;
    return n;
}

static BspNode* FindLeafRec(BspNode* n, HWND h) {
    if (!n) return nullptr;
    if (n->IsLeaf()) return n->hwnd == h ? n : nullptr;
    if (BspNode* r = FindLeafRec(n->a, h)) return r;
    return FindLeafRec(n->b, h);
}

BspNode* BspTree::FindLeaf(HWND h) const {
    return FindLeafRec(root_, h);
}

void BspTree::Insert(HWND h, HWND beside, bool newWindowFirst) {
    if (!h || FindLeaf(h)) return;

    if (!root_) {
        root_ = new BspNode();
        root_->hwnd = h;
        return;
    }

    BspNode* target = beside ? FindLeaf(beside) : nullptr;
    if (!target) target = FirstLeaf(root_);
    if (!target) { Clear(); root_ = new BspNode(); root_->hwnd = h; return; }

    // Split along the longer axis of the leaf being replaced, exactly like
    // dwindle does. A zero rect (never laid out yet) defaults to a vertical split.
    const bool vertical = (target->rect.w >= target->rect.h);

    BspNode* existing = new BspNode();
    existing->hwnd = target->hwnd;
    existing->rect = target->rect;

    BspNode* fresh = new BspNode();
    fresh->hwnd = h;

    target->hwnd     = nullptr;
    target->vertical = vertical;
    target->ratio    = 0.5f;
    target->a        = newWindowFirst ? fresh : existing;
    target->b        = newWindowFirst ? existing : fresh;
    target->a->parent = target;
    target->b->parent = target;
}

// Splits `leaf` in two along the axis `side` implies, putting `h` on that side
// of whatever was there. Shared by both directional operations below; the only
// difference between them is which node gets split.
static void SplitLeafTowards(BspNode* leaf, HWND h, Dir side) {
    BspNode* existing = new BspNode();
    existing->hwnd = leaf->hwnd;
    existing->rect = leaf->rect;

    BspNode* fresh = new BspNode();
    fresh->hwnd = h;

    const bool movingFirst = (side == Dir::Left || side == Dir::Up);

    leaf->hwnd     = nullptr;
    leaf->vertical = DirHorizontal(side);   // left/right = side by side
    leaf->ratio    = 0.5f;
    leaf->a        = movingFirst ? fresh : existing;
    leaf->b        = movingFirst ? existing : fresh;
    leaf->a->parent = leaf;
    leaf->b->parent = leaf;
}

void BspTree::MoveBeside(HWND moving, HWND target, Dir side) {
    if (!moving || moving == target) return;

    // Take it out first, so a window dropped next to its own neighbour does
    // not end up counted twice. Remove() splices the sibling into the parent's
    // slot and frees the parent, so any BspNode* held across this call is
    // suspect - which is why the target leaf is looked up afterwards.
    Remove(moving);

    BspNode* leaf = FindLeaf(target);
    if (!leaf) { Insert(moving, nullptr, false); return; }
    SplitLeafTowards(leaf, moving, side);
}

void BspTree::MoveToEdge(HWND moving, Dir side) {
    if (!moving) return;
    Remove(moving);

    if (!root_) {
        root_ = new BspNode();
        root_->hwnd = moving;
        return;
    }

    // Wrap the entire existing tree, rather than splitting any one leaf: the
    // point of this gesture is a column down the whole side of the screen.
    BspNode* fresh = new BspNode();
    fresh->hwnd = moving;

    BspNode* split = new BspNode();
    split->vertical = DirHorizontal(side);
    split->ratio    = 0.5f;

    const bool movingFirst = (side == Dir::Left || side == Dir::Up);
    split->a = movingFirst ? fresh : root_;
    split->b = movingFirst ? root_ : fresh;
    split->a->parent = split;
    split->b->parent = split;

    split->parent = nullptr;
    root_ = split;
}

void BspTree::Remove(HWND h) {
    BspNode* leaf = FindLeaf(h);
    if (!leaf) return;

    BspNode* parent = leaf->parent;
    if (!parent) {                      // the only window
        Clear();
        return;
    }

    BspNode* sibling = (parent->a == leaf) ? parent->b : parent->a;
    BspNode* grand   = parent->parent;

    // Splice the sibling into the parent's slot.
    sibling->parent = grand;
    if (!grand) root_ = sibling;
    else if (grand->a == parent) grand->a = sibling;
    else                         grand->b = sibling;

    parent->a = parent->b = nullptr;    // detach so DestroyTree spares the sibling
    delete parent;
    delete leaf;
}

void BspTree::Swap(HWND x, HWND y) {
    BspNode* nx = FindLeaf(x);
    BspNode* ny = FindLeaf(y);
    if (!nx || !ny || nx == ny) return;
    std::swap(nx->hwnd, ny->hwnd);
}

void BspTree::Promote(HWND h) {
    BspNode* leaf = FindLeaf(h);
    BspNode* first = FirstLeaf(root_);
    if (!leaf || !first || leaf == first) return;
    std::swap(leaf->hwnd, first->hwnd);
}

bool BspTree::ToggleSplit(HWND h) {
    BspNode* leaf = FindLeaf(h);
    if (!leaf || !leaf->parent) return false;   // the only window in the tree

    BspNode* split = leaf->parent;
    split->vertical = !split->vertical;

    // The ratio is a share of the first child along whichever axis the split
    // runs, so it carries over unchanged and the halves keep their relative
    // weight. What does not carry over is a ratio dragged to an extreme on a
    // wide split and then applied to a short one: 8% of a 1440-pixel width is
    // a usable sliver, and 8% of a 300-pixel height is a title bar. Anything
    // past the point of being usable goes back to even.
    if (split->ratio < 0.15f || split->ratio > 0.85f) split->ratio = 0.5f;
    return true;
}

bool BspTree::SwapSplit(HWND h) {
    BspNode* leaf = FindLeaf(h);
    if (!leaf || !leaf->parent) return false;

    BspNode* split = leaf->parent;
    std::swap(split->a, split->b);
    // The ratio names the *first* child, and the first child is now the other
    // subtree, so it has to be mirrored or the two halves would swap sides and
    // keep each other's widths.
    split->ratio = 1.0f - split->ratio;
    return true;
}

bool BspTree::Resize(HWND h, Dir d, float delta) {
    BspNode* leaf = FindLeaf(h);
    if (!leaf) return false;

    const bool wantVertical = DirHorizontal(d);

    // Walk up to the closest ancestor that splits along the requested axis.
    BspNode* node = leaf;
    while (node->parent) {
        BspNode* p = node->parent;
        if (p->vertical == wantVertical) {
            // Growing "right"/"down" means giving more room to whichever side the
            // window sits on.
            const bool firstChild = (p->a == node);
            const bool positive   = (d == Dir::Right || d == Dir::Down);
            float step = (firstChild == positive) ? delta : -delta;

            p->ratio += step;
            p->ratio = (std::max)(0.08f, (std::min)(0.92f, p->ratio));
            return true;
        }
        node = p;
    }
    return false;
}

void BspTree::CollectRec(BspNode* n, std::vector<HWND>* out) {
    if (!n) return;
    if (n->IsLeaf()) { if (n->hwnd) out->push_back(n->hwnd); return; }
    CollectRec(n->a, out);
    CollectRec(n->b, out);
}

void BspTree::Collect(std::vector<HWND>* out) const {
    CollectRec(root_, out);
}

static void DescribeRec(const BspNode* n, std::wstring* out) {
    if (!n) return;
    if (n->IsLeaf()) {
        wchar_t buf[80];
        _snwprintf_s(buf, _TRUNCATE, L"%p[min %d,%d max %d,%d]",
                     (void*)n->hwnd, n->minW, n->minH,
                     n->maxW >= kNoLimit ? -1 : n->maxW,
                     n->maxH >= kNoLimit ? -1 : n->maxH);
        *out += buf;
        return;
    }
    *out += n->vertical ? L"V(" : L"H(";
    DescribeRec(n->a, out);
    *out += L", ";
    DescribeRec(n->b, out);
    *out += L")";
}

std::wstring BspTree::Describe() const {
    std::wstring s;
    DescribeRec(root_, &s);
    return s;
}

void BspTree::Prune(const std::unordered_set<HWND>& alive) {
    std::vector<HWND> have;
    Collect(&have);
    for (HWND h : have)
        if (!alive.count(h)) Remove(h);
}

void BspTree::ComputeRec(BspNode* n, const Rect& area,
                         std::vector<std::pair<HWND, Rect>>* out) {
    if (!n) return;
    n->rect = area;

    if (n->IsLeaf()) {
        if (n->hwnd) out->push_back({ n->hwnd, area });
        return;
    }

    if (n->vertical) {
        int first = (int)(area.w * n->ratio + 0.5f);
        first = (std::max)(1, (std::min)(area.w - 1, first));
        ComputeRec(n->a, Rect(area.x, area.y, first, area.h), out);
        ComputeRec(n->b, Rect(area.x + first, area.y, area.w - first, area.h), out);
    } else {
        int first = (int)(area.h * n->ratio + 0.5f);
        first = (std::max)(1, (std::min)(area.h - 1, first));
        ComputeRec(n->a, Rect(area.x, area.y, area.w, first), out);
        ComputeRec(n->b, Rect(area.x, area.y + first, area.w, area.h - first), out);
    }
}

void BspTree::Compute(const Rect& area, std::vector<std::pair<HWND, Rect>>* out) {
    ComputeRec(root_, area, out);
}

// Bottom-up: what does this subtree need, and what is the most it can use?
// A side-by-side split needs the sum of its children's widths and the larger of
// their heights; a stacked one is the other way round. Maxima add along the
// split axis and take the smaller across it, saturating rather than overflowing.
void BspTree::MeasureRec(BspNode* n, const ConsMap* cons) {
    if (!n) return;

    if (n->IsLeaf()) {
        // Every window needs somewhere to be. Without this floor a neighbour
        // with a large minimum eats the whole subtree, the split lands one
        // pixel short of what it asked for, and the sibling is handed a rect so
        // thin that ComputeLayout drops it and the window is never placed at
        // all. The floor makes the arithmetic add up and keeps a squeezed
        // window visible enough to drag out of the way.
        n->minW = n->minH = kMinLeafPx;
        n->maxW = n->maxH = kNoLimit;
        n->constrained = false;
        if (cons && n->hwnd) {
            auto it = cons->find(n->hwnd);
            if (it != cons->end()) {
                n->minW = (std::max)(kMinLeafPx, it->second.minW);
                n->minH = (std::max)(kMinLeafPx, it->second.minH);
                n->maxW = it->second.maxW;
                n->maxH = it->second.maxH;
                n->constrained = it->second.constrained();
            }
        }
        if (n->maxW < n->minW) n->maxW = n->minW;
        if (n->maxH < n->minH) n->maxH = n->minH;
        n->leaves = 1;
        return;
    }

    MeasureRec(n->a, cons);
    MeasureRec(n->b, cons);
    n->leaves      = n->a->leaves + n->b->leaves;
    n->constrained = n->a->constrained || n->b->constrained;

    const auto addSat = [](int x, int y) {
        const long long sum = (long long)x + (long long)y;
        return sum >= kNoLimit ? kNoLimit : (int)sum;
    };

    if (n->vertical) {
        n->minW = addSat(n->a->minW, n->b->minW);
        n->minH = (std::max)(n->a->minH, n->b->minH);
        n->maxW = addSat(n->a->maxW, n->b->maxW);
        n->maxH = (std::min)(n->a->maxH, n->b->maxH);
    } else {
        n->minW = (std::max)(n->a->minW, n->b->minW);
        n->minH = addSat(n->a->minH, n->b->minH);
        n->maxW = (std::min)(n->a->maxW, n->b->maxW);
        n->maxH = addSat(n->a->maxH, n->b->maxH);
    }
    // A subtree can never be told it must be smaller than it must be.
    if (n->maxW < n->minW) n->maxW = n->minW;
    if (n->maxH < n->minH) n->maxH = n->minH;
}

namespace {

// Where to cut `total` between two children that each want at least `minA`/`minB`
// and at most `maxA`/`maxB`. The ratio is the preference; the limits win.
//
// `leavesA`/`leavesB` are how many windows sit on each side. They matter only
// when the ratio cannot be honoured - see below.
int ConstrainedSplit(int total, float ratio, int minA, int maxA, int minB, int maxB,
                     int leavesA = 1, int leavesB = 1) {
    int first = (int)(total * ratio + 0.5f);

    // The second child's needs are the first child's ceiling, and vice versa.
    const int hiFromB = total - minB;
    const int loFromB = total - (maxB >= kNoLimit ? total : maxB);

    int lo = (std::max)(minA, loFromB);
    int hi = (std::min)(maxA >= kNoLimit ? total : maxA, hiFromB);

    // Everything cannot fit. Give each side its minimum in proportion rather
    // than letting one of them win outright and push the other off the screen -
    // but never more than it can use. Handing a side more than its maximum does
    // not fill the space, it just moves the empty part of the screen somewhere
    // else, and on a real board that turned a 264-pixel window into a 448-pixel
    // slot with a 184-pixel hole beside it.
    if (lo > hi) {
        const long long needA = (std::max)(1, minA);
        const long long needB = (std::max)(1, minB);
        int share = (int)((needA * total) / (needA + needB));
        if (maxA < kNoLimit) share = (std::min)(share, maxA);
        if (maxB < kNoLimit) share = (std::max)(share, total - maxB);
        return (std::max)(1, (std::min)(total - 1, share));
    }

    // The ratio can be honoured: nothing else to decide, and the layout looks
    // exactly as it always has.
    if (first >= lo && first <= hi) return first;

    // It cannot. Clamping alone would pin the constrained side to its bare
    // MINIMUM and leave every spare pixel on the other side - which is how a
    // board with one large window ended up with a 122-pixel-wide neighbour
    // beside a 750-pixel one. Share what is left after both minimums instead,
    // in proportion to how many windows each side has to fit into it, so the
    // shortfall is spread rather than dumped on whoever sits deepest in the
    // tree. Only reachable when the ratio has already failed, so the ordinary
    // case is untouched.
    const int slack = total - minA - minB;
    if (slack > 0 && leavesA > 0 && leavesB > 0) {
        const long long share = (long long)slack * leavesA / (leavesA + leavesB);
        first = minA + (int)share;
    }
    return (std::max)(lo, (std::min)(hi, first));
}

// The two child rects a split of `n` produces inside `r`, the way the
// constrained walk would cut it if the split ran `vertical`. One place, so the
// walk and the judgement below cannot disagree about where the cut lands.
void CutNode(const BspNode* n, const Rect& r, bool vertical, Rect* ra, Rect* rb) {
    if (vertical) {
        int first = ConstrainedSplit(r.w, n->ratio,
                                     n->a->minW, n->a->maxW,
                                     n->b->minW, n->b->maxW,
                                     n->a->leaves, n->b->leaves);
        first = (std::max)(1, (std::min)(r.w - 1, first));
        *ra = Rect(r.x, r.y, first, r.h);
        *rb = Rect(r.x + first, r.y, r.w - first, r.h);
    } else {
        int first = ConstrainedSplit(r.h, n->ratio,
                                     n->a->minH, n->a->maxH,
                                     n->b->minH, n->b->maxH,
                                     n->a->leaves, n->b->leaves);
        first = (std::max)(1, (std::min)(r.h - 1, first));
        *ra = Rect(r.x, r.y, r.w, first);
        *rb = Rect(r.x, r.y + first, r.w, r.h - first);
    }
}

// ---------------------------------------------------------------- orientation
// What a subtree makes of a slot: does everything in it get its minimum, and
// how many pixels of the slot does it have no use for.
//
// A window with a maximum size uses min(slot, max) of each axis and leaves the
// rest as bare desktop. A split can only give that surplus to the sibling if
// it runs along the limited axis; across it, the surplus is simply lost. The
// only cure is to run the split the other way, and that is a decision the
// tree cannot make for itself - it was built by "split the longer edge",
// which knows nothing about limits. So the constrained walk judges each split
// both ways round, in the slot it actually has, and takes the better one.
struct Fit {
    bool      feasible = true;   // every minimum below is met
    long long waste    = 0;      // pixels of the slot nothing below can use
};

bool BetterFit(const Fit& x, const Fit& y) {       // strictly better
    if (x.feasible != y.feasible) return x.feasible;
    return x.waste < y.waste;
}

// The judgement looks a few levels down - a child is only as good as the
// choice *it* will get to make - and past that trusts the aggregate limits,
// which are a fair guess and keep the cost bounded on a deep dwindle spiral.
constexpr int kJudgeDepth = 3;

Fit JudgeSplit(const BspNode* n, const Rect& r, bool vertical, int depth);

Fit JudgeNode(const BspNode* n, const Rect& r, int depth) {
    if (n->IsLeaf() || depth <= 0 || !n->constrained) {
        Fit f;
        f.feasible = (n->minW <= r.w && n->minH <= r.h);
        const long long useW = (std::min)((long long)r.w, (long long)n->maxW);
        const long long useH = (std::min)((long long)r.h, (long long)n->maxH);
        f.waste = (long long)r.w * r.h - useW * useH;
        return f;
    }
    const Fit stored = JudgeSplit(n, r, n->vertical, depth);
    const Fit other  = JudgeSplit(n, r, !n->vertical, depth);
    return BetterFit(other, stored) ? other : stored;
}

Fit JudgeSplit(const BspNode* n, const Rect& r, bool vertical, int depth) {
    Rect ra, rb;
    CutNode(n, r, vertical, &ra, &rb);
    const Fit fa = JudgeNode(n->a, ra, depth - 1);
    const Fit fb = JudgeNode(n->b, rb, depth - 1);
    Fit f;
    f.feasible = fa.feasible && fb.feasible;
    f.waste    = fa.waste + fb.waste;
    return f;
}

} // namespace

void BspTree::Compute(const Rect& area, const ConsMap* cons,
                      std::vector<std::pair<HWND, Rect>>* out) {
    if (!cons || cons->empty()) { ComputeRec(root_, area, out); return; }

    // Top-down, using the measurements. Written as an explicit stack-free
    // recursion by lambda so the constrained path stays beside the plain one.
    struct Walk {
        std::vector<std::pair<HWND, Rect>>* out;
        bool turned = false;         // some split was set the other way round
        void Go(BspNode* n, const Rect& r) {
            if (!n) return;
            n->rect = r;
            if (n->IsLeaf()) {
                if (n->hwnd) out->push_back({ n->hwnd, r });
                return;
            }
            // A split with nothing limited beneath it is left exactly as it
            // is, so a board of ordinary windows lays out as it always has.
            // One with a limit beneath it is tried both ways and turned only
            // when the other way is strictly better: the same waste is a tie,
            // and a tie keeps whatever the user (or dwindle) chose.
            if (n->constrained) {
                const Fit stored = JudgeSplit(n, r, n->vertical, kJudgeDepth);
                const Fit other  = JudgeSplit(n, r, !n->vertical, kJudgeDepth);
                if (BetterFit(other, stored)) {
                    n->vertical = !n->vertical;
                    turned = true;
                }
            }
            Rect ra, rb;
            CutNode(n, r, n->vertical, &ra, &rb);
            Go(n->a, ra);
            Go(n->b, rb);
        }
    } walk{ out };

    // The measurements a split is cut by are summed under the orientation its
    // children had when they were measured. Turning a child changes what its
    // subtree needs and can use, so its parent is worth cutting again with the
    // new numbers. Twice is enough in practice; the cap is so a board with two
    // equally bad choices cannot keep the pass going round.
    for (int pass = 0; pass < 3; ++pass) {
        out->clear();
        walk.turned = false;
        MeasureRec(root_, cons);
        walk.Go(root_, area);
        if (!walk.turned) break;
    }
}

// ---------------------------------------------------------------- layouts
namespace {

SizeLimits LimitsOf(const ConsMap* cons, HWND h) {
    if (cons) {
        auto it = cons->find(h);
        if (it != cons->end()) return it->second;
    }
    return SizeLimits{};
}

// The limits of a group of windows stacked along one axis, seen as a single
// slice: it needs as much as its hungriest member, and can use no more than its
// most restricted one.
SizeLimits GroupLimits(const std::vector<HWND>& members, const ConsMap* cons) {
    SizeLimits g;
    g.minW = g.minH = 0;
    g.maxW = g.maxH = kNoLimit;
    for (HWND h : members) {
        const SizeLimits l = LimitsOf(cons, h);
        g.minW = (std::max)(g.minW, l.minW);
        g.minH = (std::max)(g.minH, l.minH);
        g.maxW = (std::min)(g.maxW, l.maxW);
        g.maxH = (std::min)(g.maxH, l.maxH);
    }
    if (g.maxW < g.minW) g.maxW = g.minW;
    if (g.maxH < g.minH) g.maxH = g.minH;
    return g;
}

// Divide `total` between `count` slices that each want at least `lo[i]` and at
// most `hi[i]`. Start from an even share, lock whatever hits a limit, and hand
// what that frees (or costs) to the slices still free to move. This is what
// stops a maximum-size window leaving a hole: its surplus is redistributed
// rather than abandoned.
void FlexSizes(int total, const std::vector<int>& loIn, const std::vector<int>& hi,
               std::vector<int>* out) {
    const int count = (int)loIn.size();
    out->assign(count, 0);
    if (count <= 0 || total <= 0) return;

    // The minimums may add up to more than there is. Scale them down together
    // rather than letting the earlier slices take everything and the later ones
    // overlap or vanish - an unsatisfiable board still has to be a tiling.
    std::vector<int> lo = loIn;
    long long need = 0;
    for (int v : lo) need += v;
    if (need > total) {
        long long used = 0;
        for (int i = 0; i < count; ++i) {
            lo[i] = (int)((long long)lo[i] * total / need);
            if (lo[i] < 1) lo[i] = 1;
            used += lo[i];
        }
        for (int i = count - 1; i >= 0 && used > total; --i) {
            const int cut = (std::min)((int)(used - total), (std::max)(0, lo[i] - 1));
            lo[i] -= cut;
            used  -= cut;
        }
    }

    const int base = total / count;
    int rem = total - base * count;
    for (int i = 0; i < count; ++i) (*out)[i] = base + (i < rem ? 1 : 0);

    std::vector<bool> locked(count, false);

    for (int pass = 0; pass <= count; ++pass) {
        int freed = 0;                 // pixels the locked slices gave back
        bool changed = false;
        for (int i = 0; i < count; ++i) {
            if (locked[i]) continue;
            if ((*out)[i] < lo[i]) {
                freed -= lo[i] - (*out)[i];
                (*out)[i] = lo[i];
                locked[i] = true;
                changed = true;
            } else if ((*out)[i] > hi[i]) {
                freed += (*out)[i] - hi[i];
                (*out)[i] = hi[i];
                locked[i] = true;
                changed = true;
            }
        }
        if (!changed) break;

        std::vector<int> free;
        for (int i = 0; i < count; ++i) if (!locked[i]) free.push_back(i);
        if (free.empty() || freed == 0) break;

        const int step = freed / (int)free.size();
        int spare = freed - step * (int)free.size();
        for (int i : free) {
            (*out)[i] += step;
            if (spare > 0)      { (*out)[i] += 1; --spare; }
            else if (spare < 0) { (*out)[i] -= 1; ++spare; }
        }
    }

    // Rounding, or space nothing is allowed to use. Give it to the last slice
    // that can still take it, so the tiling stays edge to edge.
    int sum = 0;
    for (int v : *out) sum += v;
    int diff = total - sum;
    for (int i = count - 1; i >= 0 && diff != 0; --i) {
        if (locked[i] && diff > 0) continue;
        // Never drive a slice to nothing to balance the books. Handing the
        // whole shortfall to one slice could take it to zero or below, and a
        // zero-width slot is a window ComputeLayout then drops from the plan
        // outright - the window is simply never placed.
        if ((*out)[i] + diff < 1) continue;
        (*out)[i] += diff;
        diff = 0;
    }
    // Still over budget, and no single slice could absorb it. Take it back a
    // pixel at a time from whoever has one to spare, so the slices still add up
    // to the area and none of them collapses.
    for (int guard = 0; diff < 0 && guard < count; ++guard) {
        bool took = false;
        for (int i = count - 1; i >= 0 && diff < 0; --i) {
            if ((*out)[i] <= 1) continue;
            --(*out)[i];
            ++diff;
            took = true;
        }
        if (!took) break;
    }
    if (diff > 0) (*out)[count - 1] += diff;
}

// Lay `count` slices out along one axis of `area`, honouring per-slice limits.
void SliceConstrained(const Rect& area, bool vertical,
                      const std::vector<SizeLimits>& lim, std::vector<Rect>* out) {
    const int count = (int)lim.size();
    if (count <= 0) return;

    const int total = vertical ? area.w : area.h;
    std::vector<int> lo(count), hi(count);
    for (int i = 0; i < count; ++i) {
        const int want = (std::max)(kMinLeafPx, vertical ? lim[i].minW : lim[i].minH);
        lo[i] = (std::max)(1, (std::min)(total, want));
        const int cap = vertical ? lim[i].maxW : lim[i].maxH;
        hi[i] = (std::max)(lo[i], cap >= kNoLimit ? total : cap);
    }

    std::vector<int> sizes;
    FlexSizes(total, lo, hi, &sizes);

    int pos = vertical ? area.x : area.y;
    for (int i = 0; i < count; ++i) {
        const int size = (std::max)(1, sizes[i]);
        out->push_back(vertical ? Rect(pos, area.y, size, area.h)
                                : Rect(area.x, pos, area.w, size));
        pos += size;
    }
}

// Split `area` into `count` slices along one axis, distributing the remainder so
// no pixel column is ever lost to rounding.
void SliceEvenly(const Rect& area, int count, bool vertical, std::vector<Rect>* out) {
    if (count <= 0) return;
    const int total = vertical ? area.w : area.h;
    const int base  = total / count;
    int rem = total - base * count;
    int pos = vertical ? area.x : area.y;

    for (int i = 0; i < count; ++i) {
        int size = base + (rem > 0 ? 1 : 0);
        if (rem > 0) --rem;
        out->push_back(vertical ? Rect(pos, area.y, size, area.h)
                                : Rect(area.x, pos, area.w, size));
        pos += size;
    }
}

void LayoutMaster(const LayoutParams& p, const std::vector<HWND>& order,
                  const Rect& area, std::vector<std::pair<HWND, Rect>>* out,
                  const ConsMap* cons) {
    const int n = (int)order.size();
    const int masters = (std::max)(1, (std::min)(p.masterCount, n));
    const int stack   = n - masters;

    const std::vector<HWND> masterWnds(order.begin(), order.begin() + masters);
    const std::vector<HWND> stackWnds(order.begin() + masters, order.end());

    Rect masterArea = area, stackArea;
    if (stack > 0) {
        int split = (int)(area.w * p.masterRatio + 0.5f);
        if (cons) {
            // The column each side needs is the widest window standing in it.
            const SizeLimits m = GroupLimits(masterWnds, cons);
            const SizeLimits t = GroupLimits(stackWnds, cons);
            split = ConstrainedSplit(area.w, p.masterRatio,
                                     m.minW, m.maxW, t.minW, t.maxW,
                                     (int)masterWnds.size(), (int)stackWnds.size());
        }
        split = (std::max)(1, (std::min)(area.w - 1, split));
        masterArea = Rect(area.x, area.y, split, area.h);
        stackArea  = Rect(area.x + split, area.y, area.w - split, area.h);
    }

    const auto limitsFor = [&](const std::vector<HWND>& list) {
        std::vector<SizeLimits> v;
        v.reserve(list.size());
        for (HWND h : list) v.push_back(LimitsOf(cons, h));
        return v;
    };

    std::vector<Rect> slots;
    if (cons) SliceConstrained(masterArea, false, limitsFor(masterWnds), &slots);
    else      SliceEvenly(masterArea, masters, false, &slots);
    for (int i = 0; i < masters; ++i) out->push_back({ order[i], slots[i] });

    if (stack > 0) {
        std::vector<Rect> stackSlots;
        if (cons) SliceConstrained(stackArea, false, limitsFor(stackWnds), &stackSlots);
        else      SliceEvenly(stackArea, stack, false, &stackSlots);
        for (int i = 0; i < stack; ++i)
            out->push_back({ order[masters + i], stackSlots[i] });
    }
}

void LayoutGrid(const std::vector<HWND>& order, const Rect& area,
                std::vector<std::pair<HWND, Rect>>* out, const ConsMap* cons) {
    const int n = (int)order.size();
    int cols = 1;
    while (cols * cols < n) ++cols;
    if (cols > 1 && (cols - 1) * cols >= n) --cols;   // prefer wider-than-tall
    const int rows = (n + cols - 1) / cols;

    // Group by row first: a row is only as short as its tallest member allows,
    // and only as tall as its most restricted one.
    std::vector<std::vector<HWND>> byRow;
    {
        int index = 0;
        for (int r = 0; r < rows && index < n; ++r) {
            const int inThisRow = (std::min)(cols, n - index);
            byRow.emplace_back(order.begin() + index, order.begin() + index + inThisRow);
            index += inThisRow;
        }
    }

    std::vector<Rect> rowRects;
    if (cons) {
        std::vector<SizeLimits> rowLim;
        rowLim.reserve(byRow.size());
        for (const auto& row : byRow) rowLim.push_back(GroupLimits(row, cons));
        SliceConstrained(area, false, rowLim, &rowRects);
    } else {
        SliceEvenly(area, rows, false, &rowRects);
    }

    for (int r = 0; r < (int)byRow.size(); ++r) {
        std::vector<Rect> cells;
        if (cons) {
            std::vector<SizeLimits> cellLim;
            cellLim.reserve(byRow[r].size());
            for (HWND h : byRow[r]) cellLim.push_back(LimitsOf(cons, h));
            SliceConstrained(rowRects[r], true, cellLim, &cells);
        } else {
            SliceEvenly(rowRects[r], (int)byRow[r].size(), true, &cells);
        }
        for (int c = 0; c < (int)byRow[r].size(); ++c)
            out->push_back({ byRow[r][c], cells[c] });
    }
}

} // namespace

void ComputeLayout(const LayoutParams& p, const std::vector<HWND>& order,
                   BspTree& tree, std::vector<std::pair<HWND, Rect>>* out,
                   const ConsMap* cons) {
    out->clear();
    if (order.empty() || p.work.empty()) return;

    // Windows tile `area` edge to edge; each one then shrinks by half the inner
    // gap, so neighbours end up `gapInner` apart and the screen edge `gapOuter`.
    //
    // Unless there is only one of them, in which case there is no neighbour to
    // be `gapInner` away from and no reason to hold it off the screen edge
    // either: it simply takes the work area.
    const bool alone = (p.smartGaps && order.size() == 1);
    const int half = alone ? 0 : p.gapInner / 2;
    const Rect area = alone ? p.work : p.work.shrink(p.gapOuter - half);
    if (area.empty()) return;

    // Limits describe the WINDOW; the algorithms below divide up SLOTS, which
    // are one inner gap larger. Inflate once, here, so nothing downstream has
    // to remember the difference.
    ConsMap slotCons;
    const ConsMap* useCons = nullptr;
    if (cons && !cons->empty()) {
        const int pad = half * 2;
        slotCons.reserve(cons->size());
        for (const auto& e : *cons) {
            SizeLimits l = e.second;
            if (!l.constrained()) continue;
            if (l.minW > 0)        l.minW += pad;
            if (l.minH > 0)        l.minH += pad;
            if (l.maxW < kNoLimit) l.maxW += pad;
            if (l.maxH < kNoLimit) l.maxH += pad;
            slotCons.emplace(e.first, l);
        }
        if (!slotCons.empty()) useCons = &slotCons;
    }

    switch (p.kind) {
        case LayoutKind::Dwindle:
            tree.Compute(area, useCons, out);
            break;
        case LayoutKind::Master:
            LayoutMaster(p, order, area, out, useCons);
            break;
        case LayoutKind::Grid:
            LayoutGrid(order, area, out, useCons);
            break;
        case LayoutKind::Monocle:
            for (HWND h : order) out->push_back({ h, area });
            break;
        default:
            break;
    }

    for (auto& e : *out) e.second = e.second.shrink(half);

    // A slot that collapsed to nothing cannot be handed to a window - but it
    // must not disappear in silence either. This is the layout quietly refusing
    // to place a window, which from the desktop looks exactly like the tiler
    // having skipped it for no reason, and it used to leave no trace at all.
    const size_t asked = out->size();
    out->erase(std::remove_if(out->begin(), out->end(),
        [](const std::pair<HWND, Rect>& e) { return e.second.empty(); }), out->end());
    if (out->size() != asked)
        AWA_LOG(L"layout: %d window(s) had no room in %dx%d and were left unplaced",
                (int)(asked - out->size()), area.w, area.h);
}

std::vector<HWND> SqueezedWindows(const std::vector<HWND>& order,
                                  const std::vector<std::pair<HWND, Rect>>& plan,
                                  const ConsMap& cons) {
    std::vector<HWND> squeezed;
    for (HWND h : order) {
        auto lim = cons.find(h);
        const int needW = (lim != cons.end()) ? lim->second.minW : 0;
        const int needH = (lim != cons.end()) ? lim->second.minH : 0;

        const Rect* got = nullptr;
        for (const auto& e : plan) if (e.first == h) { got = &e.second; break; }

        // Unplaced is the layout admitting it had nowhere to put it - the
        // same thing as a slot it cannot use, seen from the other side.
        if (!got || got->w < needW || got->h < needH) squeezed.push_back(h);
    }
    return squeezed;
}

} // namespace awa
