#include "ui/pen_gesture.hpp"

#include "core/vector/flatten.hpp" // contentBounds (re-centring the finished path) + flatten

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <variant>

namespace mosaic::ui {
namespace {

using common::Affine2D;
using common::Vec2;
namespace vec = core::vec;

// How finely a segment is sampled when picking / locating a point on it. Two passes: a coarse
// sweep, then the same count again inside the winning interval -- so the reported `t` is good to
// ~1/1024 of a segment, which is what an inserted node needs to land under the cursor.
constexpr int kSegSamples = 32;

Vec2 lerp(Vec2 a, Vec2 b, double t) { return a + (b - a) * t; }

// Squared distance from p to the segment ab (avoids a sqrt per sample in the segment sweep).
double segDistSq(Vec2 a, Vec2 b, Vec2 p) {
    const Vec2 ab = b - a;
    const double len2 = ab.dot(ab);
    if (len2 <= 1e-20)
        return (p - a).dot(p - a);
    const double t = std::clamp((p - a).dot(ab) / len2, 0.0, 1.0);
    const Vec2 d = p - (a + ab * t);
    return d.dot(d);
}

// The node addresses of a path, in draw order -- used only to keep the loops below readable.
bool addressValid(const vec::Path& p, PenSelection n) {
    return n.valid && n.subpath < p.subpaths.size() &&
           n.node < p.subpaths[n.subpath].nodes.size();
}

// Distance from a node to its neighbour along the subpath (prev when `back`), or 0 when it has none.
double neighbourDistance(const vec::SubPath& sp, std::size_t i, bool back) {
    const std::size_t n = sp.nodes.size();
    if (n < 2)
        return 0.0;
    if (back) {
        if (i == 0 && !sp.closed)
            return 0.0;
        const std::size_t j = i == 0 ? n - 1 : i - 1;
        return (sp.nodes[j].anchor - sp.nodes[i].anchor).length();
    }
    if (i + 1 >= n && !sp.closed)
        return 0.0;
    const std::size_t j = (i + 1) % n;
    return (sp.nodes[j].anchor - sp.nodes[i].anchor).length();
}

// The unit chord direction through node i (prev -> next), the axis a smoothed node's handles lie on.
Vec2 chordDirection(const vec::SubPath& sp, std::size_t i) {
    const std::size_t n = sp.nodes.size();
    if (n < 2)
        return {1.0, 0.0};
    const bool hasPrev = sp.closed || i > 0;
    const bool hasNext = sp.closed || i + 1 < n;
    const Vec2 prev = sp.nodes[i == 0 ? n - 1 : i - 1].anchor;
    const Vec2 next = sp.nodes[(i + 1) % n].anchor;
    Vec2 d{0.0, 0.0};
    if (hasPrev && hasNext)
        d = next - prev;
    else if (hasNext)
        d = next - sp.nodes[i].anchor;
    else if (hasPrev)
        d = sp.nodes[i].anchor - prev;
    const double len = d.length();
    return len > 1e-12 ? d * (1.0 / len) : Vec2{1.0, 0.0};
}

// The knob shape a node's editing hint asks for: a cusp is a square (the two sides are independent,
// so the mark has corners), anything smoothed is round.
PenChromeMark::Kind anchorKind(const vec::Node& n) {
    return n.type == vec::Node::Type::Corner ? PenChromeMark::Kind::AnchorCusp
                                             : PenChromeMark::Kind::AnchorSmooth;
}

// Does `hover` name this node's anchor / this side's handle?
bool hoversAnchor(const PenHit& hover, std::size_t s, std::size_t i) {
    return hover.kind == PenHit::Kind::Anchor && hover.subpath == s && hover.node == i;
}
bool hoversHandle(const PenHit& hover, std::size_t s, std::size_t i, bool outSide) {
    const PenHit::Kind want = outSide ? PenHit::Kind::OutHandle : PenHit::Kind::InHandle;
    return hover.kind == want && hover.subpath == s && hover.node == i;
}

// One handle of `n`: its stem and its tip knob, appended only when the handle is actually off its
// anchor (a collapsed handle means "straight on that side" -- there is nothing to draw or grab).
void appendHandle(PenChrome& out, const vec::Node& n, bool outSide, bool selected, bool hovered,
                  std::size_t maxMarks, std::size_t maxStems) {
    const Vec2 tip = outSide ? n.outHandle : n.inHandle;
    if ((tip - n.anchor).length() <= kPenHandleMinPx)
        return;
    // Both or neither: a stem running out to a tip knob that the mark budget dropped would be a
    // line pointing at nothing, which is worse than the handle simply not being shown.
    if (out.marks.size() >= maxMarks || out.stems.size() >= maxStems)
        return;
    out.stems.push_back(PenChromeStem{n.anchor, tip});
    out.marks.push_back(PenChromeMark{tip, PenChromeMark::Kind::HandleTip, selected, hovered});
}

} // namespace

// ---- Options / paint ---------------------------------------------------------------------------

std::vector<double> penDashArray(int style, double width) {
    // The shape designer's dashPatternFor at spacing 1.0 -- kept identical on purpose (a dashed pen
    // path and a dashed shape outline are the same feature wearing two bars).
    const double w = std::max(1.0, width);
    switch (style) {
    case 1:
        return {3.0 * w, 2.0 * w};
    case 2:
        return {0.6 * w, 1.6 * w};
    case 3:
        return {3.0 * w, 1.6 * w, 0.6 * w, 1.6 * w};
    default:
        return {};
    }
}

int penDashStyleOf(const std::vector<double>& dashArray) {
    if (dashArray.empty())
        return 0;
    if (dashArray.size() >= 4)
        return 3;
    return (dashArray.size() == 2 && dashArray[0] < dashArray[1] * 0.5) ? 2 : 1;
}

vec::Object penPaintedObject(const vec::Object& base, const PenOptions& opts) {
    vec::Object o = base;
    // A path that paints nothing at all would be an invisible layer you cannot find again; when the
    // user has switched both off, the stroke wins (it is the one that can express an open path).
    const bool wantFill = opts.fill;
    const bool wantStroke = opts.strokeEnabled || !wantFill;
    o.fill = wantFill ? vec::Paint{vec::SolidPaint{opts.foreground}} : vec::Paint{vec::NoPaint{}};
    o.stroke.enabled = wantStroke;
    o.stroke.width = std::max(0.0, opts.strokeWidth);
    o.stroke.cap = opts.cap;
    o.stroke.join = opts.join;
    o.stroke.dashArray = penDashArray(opts.dashStyle, o.stroke.width);
    if (wantStroke) {
        // Same convention as ui::recoloredObject: the fill is the primary element (fg), so an
        // outline drawn OVER a fill takes the secondary accent (bg); a lone stroke is primary.
        o.stroke.paint = vec::SolidPaint{wantFill ? opts.background : opts.foreground};
    }
    return o;
}

void readPenOptions(const vec::Object& obj, PenOptions& io) {
    io.fill = !std::holds_alternative<vec::NoPaint>(obj.fill);
    io.strokeEnabled = obj.stroke.enabled;
    io.strokeWidth = obj.stroke.width;
    io.cap = obj.stroke.cap;
    io.join = obj.stroke.join;
    io.dashStyle = penDashStyleOf(obj.stroke.dashArray);
}

vec::Object penRecoloredObject(const vec::Object& base, common::ColorF fg, common::ColorF bg) {
    vec::Object o = base;
    const bool hasFill = !std::holds_alternative<vec::NoPaint>(o.fill);
    const bool hasStroke = o.stroke.enabled;
    if (hasFill)
        o.fill = vec::SolidPaint{fg};
    if (hasStroke)
        o.stroke.paint = vec::SolidPaint{hasFill ? bg : fg};
    return o;
}

bool penToolBinds(const vec::Object& obj) {
    return std::holds_alternative<vec::Path>(obj.geometry);
}

// ---- Hit-testing and the pure edits ------------------------------------------------------------

std::optional<std::array<Vec2, 4>> penSegmentCubic(const vec::SubPath& sp, std::size_t i) {
    const std::size_t n = sp.nodes.size();
    if (n < 2 || i >= n)
        return std::nullopt;
    if (i + 1 >= n && !sp.closed)
        return std::nullopt; // the last node of an open subpath leaves no segment
    const vec::Node& a = sp.nodes[i];
    const vec::Node& b = sp.nodes[(i + 1) % n];
    return std::array<Vec2, 4>{a.anchor, a.outHandle, b.inHandle, b.anchor};
}

Vec2 penCubicAt(const std::array<Vec2, 4>& c, double t) {
    const Vec2 a0 = lerp(c[0], c[1], t);
    const Vec2 a1 = lerp(c[1], c[2], t);
    const Vec2 a2 = lerp(c[2], c[3], t);
    const Vec2 b0 = lerp(a0, a1, t);
    const Vec2 b1 = lerp(a1, a2, t);
    return lerp(b0, b1, t);
}

PenHit penHitTest(const vec::Path& path, Vec2 pLocal, double tolLocal, PenSelection handlesOf) {
    const double tol = std::max(tolLocal, 0.0);
    const double tolSq = tol * tol;

    // 1. The selected node's handles -- drawn on top, so grabbable first (a handle pulled only a
    //    little way out otherwise sits inside its own anchor's pick disc and could never be taken).
    if (addressValid(path, handlesOf)) {
        const vec::Node& n = path.subpaths[handlesOf.subpath].nodes[handlesOf.node];
        const bool outOff = (n.outHandle - n.anchor).length() > 1e-9;
        const bool inOff = (n.inHandle - n.anchor).length() > 1e-9;
        const double dOut = (n.outHandle - pLocal).dot(n.outHandle - pLocal);
        const double dIn = (n.inHandle - pLocal).dot(n.inHandle - pLocal);
        if (outOff && dOut <= tolSq && (!inOff || dOut <= dIn))
            return PenHit{PenHit::Kind::OutHandle, handlesOf.subpath, handlesOf.node, 0.0,
                          n.outHandle};
        if (inOff && dIn <= tolSq)
            return PenHit{PenHit::Kind::InHandle, handlesOf.subpath, handlesOf.node, 0.0,
                          n.inHandle};
    }

    // 2. Any anchor (nearest wins).
    PenHit best;
    double bestSq = std::numeric_limits<double>::infinity();
    for (std::size_t s = 0; s < path.subpaths.size(); ++s) {
        const vec::SubPath& sp = path.subpaths[s];
        for (std::size_t i = 0; i < sp.nodes.size(); ++i) {
            const Vec2 d = sp.nodes[i].anchor - pLocal;
            const double dsq = d.dot(d);
            if (dsq <= tolSq && dsq < bestSq) {
                bestSq = dsq;
                best = PenHit{PenHit::Kind::Anchor, s, i, 0.0, sp.nodes[i].anchor};
            }
        }
    }
    if (best.hit())
        return best;

    // 3. ANY node's handles (nearest wins), below the anchors so a handle lying over a neighbouring
    //    anchor never steals it. Every one of these is DRAWN now -- the chrome has its own overlay
    //    lane -- and drawn-but-not-grabbable is exactly the kind of quiet lie this tool cannot
    //    afford: the tip you can see is the tip you can pull.
    for (std::size_t s = 0; s < path.subpaths.size(); ++s) {
        const vec::SubPath& sp = path.subpaths[s];
        for (std::size_t i = 0; i < sp.nodes.size(); ++i) {
            const vec::Node& n = sp.nodes[i];
            const Vec2 tips[2] = {n.outHandle, n.inHandle};
            const PenHit::Kind kinds[2] = {PenHit::Kind::OutHandle, PenHit::Kind::InHandle};
            for (int k = 0; k < 2; ++k) {
                // "Off its anchor at all", NOT kPenHandleMinPx: that threshold is in SCREEN px (it
                // decides what is worth drawing) and these are layer-local units, which at a deep
                // zoom are a very different size. A handle that really is on its anchor loses to
                // the anchor above anyway, since anchors are tried first.
                if ((tips[k] - n.anchor).length() <= 1e-9)
                    continue; // collapsed onto its anchor: nothing to grab
                const Vec2 d = tips[k] - pLocal;
                const double dsq = d.dot(d);
                if (dsq <= tolSq && dsq < bestSq) {
                    bestSq = dsq;
                    best = PenHit{kinds[k], s, i, 0.0, tips[k]};
                }
            }
        }
    }
    if (best.hit())
        return best;

    // 4. The segments: a coarse sweep, then a refinement inside the winning interval so the
    //    reported `t` is precise enough to insert a node exactly under the cursor.
    for (std::size_t s = 0; s < path.subpaths.size(); ++s) {
        const vec::SubPath& sp = path.subpaths[s];
        const std::size_t n = sp.nodes.size();
        if (n < 2)
            continue;
        const std::size_t segCount = sp.closed ? n : n - 1;
        for (std::size_t i = 0; i < segCount; ++i) {
            const std::optional<std::array<Vec2, 4>> cub = penSegmentCubic(sp, i);
            if (!cub)
                continue;
            Vec2 prev = penCubicAt(*cub, 0.0);
            int bestK = 0;
            double bestSegSq = std::numeric_limits<double>::infinity();
            for (int k = 1; k <= kSegSamples; ++k) {
                const Vec2 cur = penCubicAt(*cub, static_cast<double>(k) / kSegSamples);
                const double dsq = segDistSq(prev, cur, pLocal);
                if (dsq < bestSegSq) {
                    bestSegSq = dsq;
                    bestK = k;
                }
                prev = cur;
            }
            if (bestSegSq > tolSq || bestSegSq >= bestSq)
                continue;
            const double lo = std::max(0.0, static_cast<double>(bestK - 1) / kSegSamples);
            const double hi = std::min(1.0, static_cast<double>(bestK) / kSegSamples);
            double bestT = lo;
            double refinedSq = std::numeric_limits<double>::infinity();
            for (int k = 0; k <= kSegSamples; ++k) {
                const double t = lo + (hi - lo) * (static_cast<double>(k) / kSegSamples);
                const Vec2 q = penCubicAt(*cub, t);
                const double dsq = (q - pLocal).dot(q - pLocal);
                if (dsq < refinedSq) {
                    refinedSq = dsq;
                    bestT = t;
                }
            }
            bestSq = std::min(bestSegSq, refinedSq);
            best = PenHit{PenHit::Kind::Segment, s, i, bestT, penCubicAt(*cub, bestT)};
        }
    }
    return best;
}

vec::Path penMoveAnchor(const vec::Path& base, PenSelection n, Vec2 deltaLocal) {
    if (!addressValid(base, n))
        return base;
    vec::Path out = base;
    vec::Node& node = out.subpaths[n.subpath].nodes[n.node];
    node.anchor = node.anchor + deltaLocal;
    node.inHandle = node.inHandle + deltaLocal;   // the handles ride with the anchor: moving a node
    node.outHandle = node.outHandle + deltaLocal; // must not reshape the curve, only re-place it
    return out;
}

vec::Path penMoveHandle(const vec::Path& base, PenSelection n, bool outSide, Vec2 toLocal,
                        bool breakPair) {
    if (!addressValid(base, n))
        return base;
    vec::Path out = base;
    vec::Node& node = out.subpaths[n.subpath].nodes[n.node];
    Vec2& moved = outSide ? node.outHandle : node.inHandle;
    Vec2& other = outSide ? node.inHandle : node.outHandle;
    moved = toLocal;
    if (breakPair) {
        node.type = vec::Node::Type::Corner; // Alt: the pair is broken -- this is a cusp now
        return out;
    }
    if (node.type == vec::Node::Type::Corner)
        return out; // an existing cusp keeps its independence until you ask for smoothness back
    const Vec2 arm = toLocal - node.anchor;
    const double armLen = arm.length();
    if (armLen <= 1e-12)
        return out;
    if (node.type == vec::Node::Type::Symmetric) {
        other = node.anchor - arm; // exact mirror
        return out;
    }
    const double otherLen = (other - node.anchor).length(); // Smooth: collinear, own length kept
    other = node.anchor - arm * (otherLen / armLen);
    return out;
}

vec::Path penInsertNode(const vec::Path& base, PenSelection seg, double t,
                        PenSelection* outSelection) {
    if (outSelection != nullptr)
        *outSelection = PenSelection{};
    if (!addressValid(base, seg))
        return base;
    const vec::SubPath& spRef = base.subpaths[seg.subpath];
    const std::optional<std::array<Vec2, 4>> cub = penSegmentCubic(spRef, seg.node);
    if (!cub)
        return base;
    const double u = std::clamp(t, 0.0, 1.0);
    // de Casteljau split: the two halves reproduce the original curve exactly, so inserting a node
    // changes the model without changing a single drawn pixel.
    const Vec2 a0 = lerp((*cub)[0], (*cub)[1], u);
    const Vec2 a1 = lerp((*cub)[1], (*cub)[2], u);
    const Vec2 a2 = lerp((*cub)[2], (*cub)[3], u);
    const Vec2 b0 = lerp(a0, a1, u);
    const Vec2 b1 = lerp(a1, a2, u);
    const Vec2 mid = lerp(b0, b1, u);

    vec::Path out = base;
    vec::SubPath& sp = out.subpaths[seg.subpath];
    const std::size_t n = sp.nodes.size();
    sp.nodes[seg.node].outHandle = a0;
    sp.nodes[(seg.node + 1) % n].inHandle = a2;
    vec::Node fresh;
    fresh.anchor = mid;
    fresh.inHandle = b0;
    fresh.outHandle = b1;
    fresh.type = vec::Node::Type::Smooth;
    const std::size_t at = seg.node + 1; // == n for the closing segment, i.e. a push_back
    sp.nodes.insert(sp.nodes.begin() + static_cast<std::ptrdiff_t>(at), fresh);
    if (outSelection != nullptr)
        *outSelection = PenSelection{true, seg.subpath, at};
    return out;
}

vec::Path penDeleteNode(const vec::Path& base, PenSelection n) {
    if (!addressValid(base, n))
        return base;
    vec::Path out = base;
    vec::SubPath& sp = out.subpaths[n.subpath];
    sp.nodes.erase(sp.nodes.begin() + static_cast<std::ptrdiff_t>(n.node));
    if (sp.nodes.size() < 2) // one lone anchor draws nothing and fills nothing: drop the subpath
        out.subpaths.erase(out.subpaths.begin() + static_cast<std::ptrdiff_t>(n.subpath));
    return out;
}

vec::Path penToggleNodeType(const vec::Path& base, PenSelection n) {
    if (!addressValid(base, n))
        return base;
    vec::Path out = base;
    vec::SubPath& sp = out.subpaths[n.subpath];
    vec::Node& node = sp.nodes[n.node];
    const bool hasHandles = (node.inHandle - node.anchor).length() > 1e-9 ||
                            (node.outHandle - node.anchor).length() > 1e-9;
    if (hasHandles) { // -> a cusp: collapse both handles onto the anchor
        node.inHandle = node.anchor;
        node.outHandle = node.anchor;
        node.type = vec::Node::Type::Corner;
        return out;
    }
    // -> smooth: pull a symmetric pair out along the chord between the neighbours, a third of the
    // way to the nearer one (the classic "round this corner off" construction).
    const Vec2 dir = chordDirection(sp, n.node);
    const double back = neighbourDistance(sp, n.node, /*back=*/true);
    const double fwd = neighbourDistance(sp, n.node, /*back=*/false);
    double reach = 0.0;
    if (back > 0.0 && fwd > 0.0)
        reach = std::min(back, fwd) / 3.0;
    else
        reach = std::max(back, fwd) / 3.0;
    if (reach <= 1e-9)
        return out; // no neighbours to take a direction from: leave it a corner
    node.outHandle = node.anchor + dir * reach;
    node.inHandle = node.anchor - dir * reach;
    node.type = vec::Node::Type::Symmetric;
    return out;
}

Vec2 penConstrainAngle(Vec2 from, Vec2 to, double stepRad) {
    const Vec2 d = to - from;
    const double len = d.length();
    if (len <= 1e-9 || stepRad <= 0.0)
        return to;
    const double a = std::round(std::atan2(d.y, d.x) / stepRad) * stepRad;
    return {from.x + len * std::cos(a), from.y + len * std::sin(a)};
}

// ---- The authoring state machine ---------------------------------------------------------------

bool PenGesture::press(Vec2 doc, double closeRadiusDoc, bool shift) {
    if (m_closed)
        return true; // already closed: the caller owes a finish, not another node
    // Clicking the FIRST anchor closes the path. Tested against the RAW pointer, not the
    // Shift-constrained point, so the close never depends on where a constraint happened to land.
    if (m_nodes.size() >= 2 && (doc - m_nodes.front().anchor).length() <= closeRadiusDoc) {
        m_closed = true;
        m_dragging = true;
        m_dragIndex = 0; // a closing drag shapes the FIRST node's handles (the closing curve)
        m_hasHover = false;
        return true;
    }
    const Vec2 p = (shift && !m_nodes.empty()) ? penConstrainAngle(m_nodes.back().anchor, doc) : doc;
    vec::Node n;
    n.anchor = p;
    n.inHandle = p; // a bare click is a CORNER: both handles sit on the anchor (straight both sides)
    n.outHandle = p;
    n.type = vec::Node::Type::Corner;
    m_nodes.push_back(n);
    m_active = true;
    m_dragging = true;
    m_dragIndex = m_nodes.size() - 1;
    m_hasHover = false;
    return false;
}

void PenGesture::dragHandle(Vec2 doc, bool shift, bool alt) {
    if (!m_dragging || m_dragIndex >= m_nodes.size())
        return;
    vec::Node& n = m_nodes[m_dragIndex];
    const Vec2 p = shift ? penConstrainAngle(n.anchor, doc) : doc;
    n.outHandle = p;
    if (alt) { // Alt breaks the pair while you pull: the incoming side stays flat -> a cusp
        n.inHandle = n.anchor;
        n.type = vec::Node::Type::Corner;
        return;
    }
    n.inHandle = n.anchor + (n.anchor - p); // symmetric: the handle you see plus its mirror
    n.type = vec::Node::Type::Symmetric;
}

void PenGesture::moveTo(Vec2 doc, bool shift) {
    if (m_nodes.empty()) {
        m_hasHover = false;
        return;
    }
    m_hover = shift ? penConstrainAngle(m_nodes.back().anchor, doc) : doc;
    m_hasHover = true;
}

bool PenGesture::backspace() {
    if (m_nodes.empty())
        return false;
    m_nodes.pop_back();
    m_dragging = false;
    m_closed = false; // undoing a node re-opens the path (you are back to authoring it)
    if (m_nodes.empty())
        m_active = false;
    m_dragIndex = m_nodes.empty() ? 0 : m_nodes.size() - 1;
    return true;
}

void PenGesture::reset() noexcept {
    m_nodes.clear();
    m_active = false;
    m_closed = false;
    m_dragging = false;
    m_dragIndex = 0;
    m_hasHover = false;
}

vec::Path PenGesture::path() const {
    vec::Path p;
    if (m_nodes.size() < 2)
        return p; // one anchor is not a path
    vec::SubPath sp;
    sp.nodes = m_nodes;
    sp.closed = m_closed;
    p.subpaths.push_back(std::move(sp));
    return p;
}

vec::Path PenGesture::pathWithRubberBand() const {
    vec::Path p;
    if (m_nodes.empty())
        return p;
    vec::SubPath sp;
    sp.nodes = m_nodes;
    sp.closed = m_closed;
    if (!m_closed && !m_dragging && m_hasHover) {
        vec::Node n;
        n.anchor = m_hover;
        n.inHandle = m_hover;
        n.outHandle = m_hover;
        n.type = vec::Node::Type::Corner;
        sp.nodes.push_back(n);
    }
    p.subpaths.push_back(std::move(sp));
    return p;
}

PenSelection PenGesture::liveNode() const {
    if (m_nodes.empty())
        return {};
    return PenSelection{true, 0, m_dragging ? m_dragIndex : m_nodes.size() - 1};
}

// ---- Landing the path ----------------------------------------------------------------------------

std::optional<PenDraft> buildPenDraft(const vec::Path& docPath, const PenOptions& opts) {
    std::size_t nodes = 0;
    for (const vec::SubPath& sp : docPath.subpaths)
        nodes += sp.nodes.size();
    if (nodes < 2)
        return std::nullopt;
    const std::optional<common::Rect> box = vec::contentBounds(vec::Geometry{docPath});
    if (!box)
        return std::nullopt;
    // A degenerate path (every node on one point) authors nothing -- the same "a click authors no
    // shape" rule buildShapeDraft applies. A straight horizontal/vertical run has a zero-height or
    // zero-width box and is perfectly legitimate, so BOTH extents must be sub-pixel to refuse.
    if (box->w < kPenMinExtentDoc && box->h < kPenMinExtentDoc)
        return std::nullopt;

    const Vec2 c = box->center();
    vec::Path local = docPath;
    for (vec::SubPath& sp : local.subpaths)
        for (vec::Node& n : sp.nodes) {
            n.anchor = n.anchor - c;
            n.inHandle = n.inHandle - c;
            n.outHandle = n.outHandle - c;
        }
    vec::Object o;
    o.geometry = std::move(local);
    o = penPaintedObject(o, opts);
    return PenDraft{std::move(o), Affine2D::translation(c.x, c.y)};
}

// ---- Overlay geometry -----------------------------------------------------------------------------

std::vector<Vec2> penPathPolyline(const vec::Path& path, const Affine2D& toDevice) {
    const vec::Contours contours = vec::flatten(vec::Geometry{path}, 0.25, toDevice);
    std::vector<Vec2> out;
    for (const vec::Contour& c : contours) {
        if (c.points.size() < 2)
            continue;
        if (!out.empty())
            out.push_back(kPolylineBreak); // ... so the lane does not chord one contour to the next
        out.insert(out.end(), c.points.begin(), c.points.end());
        if (c.closed && c.points.size() > 2)
            out.push_back(c.points.front()); // each contour is drawn as one OPEN polyline
    }
    return out;
}

PenChrome penChromeMarks(const vec::Path& screenPath, PenSelection sel, const PenHit& hover,
                         std::size_t maxMarks, std::size_t maxStems) {
    PenChrome out;
    if (maxMarks == 0 && maxStems == 0)
        return out;
    out.marks.reserve(std::min<std::size_t>(maxMarks, 64));

    // Pass 1 -- the SELECTED node, whole: its anchor knob, then a stem + tip per live handle. First
    // so no clamp below can cost you the node you are actually working on (see the header).
    if (addressValid(screenPath, sel)) {
        const vec::Node& n = screenPath.subpaths[sel.subpath].nodes[sel.node];
        if (out.marks.size() < maxMarks)
            out.marks.push_back(PenChromeMark{n.anchor, anchorKind(n), /*selected=*/true,
                                              hoversAnchor(hover, sel.subpath, sel.node)});
        appendHandle(out, n, /*outSide=*/true, /*selected=*/true,
                     hoversHandle(hover, sel.subpath, sel.node, true), maxMarks, maxStems);
        appendHandle(out, n, /*outSide=*/false, /*selected=*/true,
                     hoversHandle(hover, sel.subpath, sel.node, false), maxMarks, maxStems);
    }

    // Pass 2 -- every other anchor. Anchors before ANY other node's handles, so a path too big for
    // the lane loses handles rather than nodes: an anchor you cannot see is a node you did not know
    // was there, while a missing handle is only a curve you have to select before you can shape.
    for (std::size_t s = 0; s < screenPath.subpaths.size(); ++s) {
        const vec::SubPath& sp = screenPath.subpaths[s];
        for (std::size_t i = 0; i < sp.nodes.size(); ++i) {
            if (sel.valid && sel.subpath == s && sel.node == i)
                continue; // already emitted, and selected
            if (out.marks.size() >= maxMarks)
                break;
            out.marks.push_back(PenChromeMark{sp.nodes[i].anchor, anchorKind(sp.nodes[i]),
                                              /*selected=*/false, hoversAnchor(hover, s, i)});
        }
    }

    // Pass 3 -- every other node's handles. This is what the old guide-lane borrowing could not
    // afford at all, and it is why a handle of an unselected node used to be invisible AND
    // ungrabbable; penHitTest's third tier is the matching half.
    for (std::size_t s = 0; s < screenPath.subpaths.size(); ++s) {
        const vec::SubPath& sp = screenPath.subpaths[s];
        for (std::size_t i = 0; i < sp.nodes.size(); ++i) {
            if (sel.valid && sel.subpath == s && sel.node == i)
                continue;
            appendHandle(out, sp.nodes[i], /*outSide=*/true, /*selected=*/false,
                         hoversHandle(hover, s, i, true), maxMarks, maxStems);
            appendHandle(out, sp.nodes[i], /*outSide=*/false, /*selected=*/false,
                         hoversHandle(hover, s, i, false), maxMarks, maxStems);
        }
    }
    return out;
}

bool penCloseTarget(const vec::Path& screenPath, Vec2 screenPt, double radiusPx, Vec2& outCenter) {
    if (screenPath.subpaths.empty() || screenPath.subpaths.front().nodes.empty() || radiusPx <= 0.0)
        return false;
    const Vec2 first = screenPath.subpaths.front().nodes.front().anchor;
    if ((screenPt - first).length() > radiusPx)
        return false;
    outCenter = first;
    return true;
}

} // namespace mosaic::ui
