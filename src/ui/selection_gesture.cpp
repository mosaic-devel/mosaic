#include "ui/selection_gesture.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace mosaic::ui {
namespace {

constexpr double kLassoDecimateDocPx = 0.5; // skip free-lasso samples closer than this to the last
constexpr double kClickAreaPx2 = 1.0;       // marquees smaller than this commit as a plain click
// Smoothed-lasso sampling: emit a point roughly every this-many doc px ALONG the curve, so a fast
// drag's long segments get subdivided enough to stay smooth (a fixed per-segment count faceted them),
// while dense slow strokes don't explode. Capped per segment so a huge segment can't run away.
constexpr double kLassoSmoothSpacingDocPx = 1.5;
constexpr int kLassoMaxSamplesPerSeg = 96;
// De-jitter the freehand control points BEFORE the spline fit, so the curve doesn't wave through
// every hand-tremor sample (worst on fast drags, where the few samples each carry direction noise).
constexpr int kLassoSmoothPasses = 2;      // Laplacian neighbour-average passes
constexpr double kLassoSmoothLambda = 0.5; // per-pass pull toward the neighbour midpoint

// Draw a `width`-px-thick line segment into `mask` (doc-space DDA, clamped): the lasso
// rubber-band stroke. Steps are <= 1 px apart, so the per-step width x width stamps overlap.
void strokeSegment(core::Selection& mask, common::Vec2 a, common::Vec2 b, int width) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const int steps =
        std::max(1, static_cast<int>(std::ceil(std::max(std::abs(dx), std::abs(dy)))));
    const long lo = -(width - 1) / 2; // stamp offsets, centred on the step point
    const long hi = lo + width;
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        const long cx = static_cast<long>(std::floor(a.x + dx * t));
        const long cy = static_cast<long>(std::floor(a.y + dy * t));
        for (long y = cy + lo; y < cy + hi; ++y) {
            if (y < 0 || y >= static_cast<long>(mask.height()))
                continue;
            for (long x = cx + lo; x < cx + hi; ++x) {
                if (x < 0 || x >= static_cast<long>(mask.width()))
                    continue;
                mask.data()[static_cast<std::size_t>(y) * mask.width() + x] = 255;
            }
        }
    }
}

} // namespace

core::SelectOp selectOpForModifiers(bool shift, bool ctrl, bool alt) {
    if ((shift && ctrl) || alt)
        return core::SelectOp::Intersect;
    if (shift)
        return core::SelectOp::Add;
    if (ctrl)
        return core::SelectOp::Subtract;
    return core::SelectOp::Replace;
}

namespace {
// One point on the CENTRIPETAL Catmull-Rom segment p1->p2 (neighbour controls p0,p3), at u in [0,1].
// Knots are spaced by sqrt(chord) (alpha = 0.5): unlike the uniform version this can't loop or
// overshoot between unevenly spaced samples -- the wiggle that made a hand-drawn path read as "wavy".
// Barry-Goldman recursion, with epsilon-guarded lerps so coincident points don't divide by zero.
common::Vec2 centripetalAt(common::Vec2 p0, common::Vec2 p1, common::Vec2 p2, common::Vec2 p3,
                           double u) {
    const auto knot = [](double t, common::Vec2 a, common::Vec2 b) {
        return t + std::sqrt(std::max((b - a).length(), 1e-9)); // sqrt(chord): centripetal
    };
    const double t0 = 0.0;
    const double t1 = knot(t0, p0, p1);
    const double t2 = knot(t1, p1, p2);
    const double t3 = knot(t2, p2, p3);
    const double t = t1 + u * (t2 - t1);
    const auto lerp = [](common::Vec2 a, common::Vec2 b, double ta, double tb, double tt) {
        const double d = tb - ta;
        if (std::abs(d) < 1e-9)
            return a;
        const double w = (tt - ta) / d;
        return a * (1.0 - w) + b * w;
    };
    const common::Vec2 a1 = lerp(p0, p1, t0, t1, t);
    const common::Vec2 a2 = lerp(p1, p2, t1, t2, t);
    const common::Vec2 a3 = lerp(p2, p3, t2, t3, t);
    const common::Vec2 b1 = lerp(a1, a2, t0, t2, t);
    const common::Vec2 b2 = lerp(a2, a3, t1, t3, t);
    return lerp(b1, b2, t1, t2, t);
}
} // namespace

std::vector<common::Vec2> catmullRomSmooth(const std::vector<common::Vec2>& pts,
                                           double sampleSpacing) {
    if (pts.size() < 3)
        return pts; // nothing to round (a point or a single segment is already "smooth")
    const double spacing = std::max(0.25, sampleSpacing);
    std::vector<common::Vec2> out;
    out.push_back(pts.front());
    // Phantom controls at the ends by reflecting the neighbour (a + (a - b)): gives a natural end
    // tangent and keeps every chord non-zero (duplicating the endpoint would zero a centripetal knot).
    const auto reflect = [](common::Vec2 a, common::Vec2 b) { return a + (a - b); };
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const common::Vec2 p1 = pts[i];
        const common::Vec2 p2 = pts[i + 1];
        const common::Vec2 p0 = (i == 0) ? reflect(p1, p2) : pts[i - 1];
        const common::Vec2 p3 = (i + 2 < pts.size()) ? pts[i + 2] : reflect(p2, p1);
        // Samples PROPORTIONAL to the segment length, so a fast drag's long segment is subdivided
        // (no facets) without a slow stroke's short segments multiplying needlessly.
        const int n = std::clamp(static_cast<int>(std::ceil((p2 - p1).length() / spacing)), 1,
                                 kLassoMaxSamplesPerSeg);
        for (int s = 1; s <= n; ++s)
            out.push_back(centripetalAt(p0, p1, p2, p3, static_cast<double>(s) / n)); // u=1 -> p2
    }
    return out;
}

std::vector<common::Vec2> laplacianSmooth(const std::vector<common::Vec2>& pts, int iterations,
                                          double lambda) {
    if (pts.size() < 3 || iterations <= 0)
        return pts;
    const double l = std::clamp(lambda, 0.0, 1.0);
    std::vector<common::Vec2> cur = pts;
    std::vector<common::Vec2> next = pts; // endpoints copied once and never rewritten -> pinned
    for (int it = 0; it < iterations; ++it) {
        for (std::size_t i = 1; i + 1 < cur.size(); ++i) {
            const common::Vec2 mid = (cur[i - 1] + cur[i + 1]) * 0.5;
            next[i] = cur[i] * (1.0 - l) + mid * l; // pull a fraction toward the neighbour midpoint
        }
        cur.swap(next);
    }
    return cur;
}

common::Rect marqueeRect(common::Vec2 anchor, common::Vec2 cursor, bool square, bool fromCenter) {
    common::Vec2 d = cursor - anchor;
    if (square) {
        const double s = std::max(std::abs(d.x), std::abs(d.y));
        d = {std::copysign(s, d.x), std::copysign(s, d.y)};
    }
    if (fromCenter)
        return common::Rect::fromCorners(anchor - d, anchor + d);
    return common::Rect::fromCorners(anchor, anchor + d);
}

void SelectionGesture::beginDrag(Kind kind, core::SelectOp op, common::Vec2 docPt,
                                 bool shiftAtPress, bool altAtPress) {
    m_phase = Phase::Dragging;
    m_kind = kind;
    m_op = op;
    m_anchor = m_cursor = docPt;
    m_constrain = m_fromCenter = false;
    m_shiftArmed = !shiftAtPress; // a modifier that chose the op is spent until re-pressed
    m_altArmed = !altAtPress;
    m_points.clear();
    if (kind == Kind::FreeLasso)
        m_points.push_back(docPt);
    m_previewDirty = true;
}

void SelectionGesture::dragTo(common::Vec2 docPt, bool shiftDown, bool altDown) {
    if (m_phase != Phase::Dragging)
        return;
    if (!shiftDown)
        m_shiftArmed = true; // released mid-drag: the next press means "shape", not "op"
    if (!altDown)
        m_altArmed = true;
    m_cursor = docPt;
    m_constrain = shiftDown && m_shiftArmed;
    m_fromCenter = altDown && m_altArmed;
    if (m_kind == Kind::FreeLasso) {
        if ((docPt - m_points.back()).length() < kLassoDecimateDocPx)
            return; // too close to the previous sample: the path did not change
        m_points.push_back(docPt);
    }
    m_previewDirty = true;
}

void SelectionGesture::beginPoly(core::SelectOp op, common::Vec2 docPt) {
    m_phase = Phase::Placing;
    m_kind = Kind::PolyLasso;
    m_op = op;
    m_anchor = m_cursor = docPt;
    m_constrain = m_fromCenter = false;
    m_points = {docPt};
    m_previewDirty = true;
}

namespace {
// Constrain the segment from `prev` to `pt` to a 15 degree multiple (keeping its length), for the
// poly-lasso's Shift snap. atan2 + round to the nearest step; a zero-length segment is left alone.
// (15 deg, not the Move-tool rotate's 5 deg: a lasso wants coarse straight-edge angles, user 2026-06-17.)
common::Vec2 snapSegmentAngle(common::Vec2 prev, common::Vec2 pt) {
    const common::Vec2 d = pt - prev;
    const double len = d.length();
    if (len < 1e-9)
        return pt;
    constexpr double kStep = 3.14159265358979323846 / 12.0; // 15 degrees
    const double ang = std::round(std::atan2(d.y, d.x) / kStep) * kStep;
    return {prev.x + len * std::cos(ang), prev.y + len * std::sin(ang)};
}
} // namespace

void SelectionGesture::addVertex(common::Vec2 docPt, bool shiftDown) {
    if (m_phase != Phase::Placing)
        return;
    if (shiftDown && !m_points.empty())
        docPt = snapSegmentAngle(m_points.back(), docPt);
    m_points.push_back(docPt);
    m_cursor = docPt;
    m_previewDirty = true;
}

void SelectionGesture::moveTo(common::Vec2 docPt, bool shiftDown) {
    if (m_phase != Phase::Placing)
        return;
    if (shiftDown && !m_points.empty())
        docPt = snapSegmentAngle(m_points.back(), docPt);
    m_cursor = docPt;
    m_previewDirty = true;
}

bool SelectionGesture::shouldClose(common::Vec2 docPt, double closeRadius,
                                   bool isDoubleClick) const {
    if (m_phase != Phase::Placing || m_points.size() < 3)
        return false; // can't enclose anything yet
    if ((docPt - m_points.front()).length() <= closeRadius)
        return true;
    return isDoubleClick && (docPt - m_points.back()).length() <= closeRadius;
}

bool SelectionGesture::degenerate() const {
    switch (m_kind) {
    case Kind::Rect:
    case Kind::Ellipse: {
        const common::Rect r = marqueeRect(m_anchor, m_cursor, m_constrain, m_fromCenter);
        return r.w * r.h < kClickAreaPx2;
    }
    case Kind::FreeLasso:
    case Kind::PolyLasso:
        return m_points.size() < 3;
    }
    return true;
}

std::vector<common::Vec2> SelectionGesture::pathPoints() const {
    if (m_smooth && m_kind == Kind::FreeLasso)
        // De-jitter first, then fit the smooth curve (both pass through verbatim under 3 points).
        return catmullRomSmooth(laplacianSmooth(m_points, kLassoSmoothPasses, kLassoSmoothLambda),
                                kLassoSmoothSpacingDocPx);
    return m_points;
}

core::Selection SelectionGesture::shapeMask(std::uint32_t docW, std::uint32_t docH) const {
    switch (m_kind) {
    case Kind::Rect:
        return core::Selection::rectangle(
            docW, docH, marqueeRect(m_anchor, m_cursor, m_constrain, m_fromCenter));
    case Kind::Ellipse:
        return core::Selection::ellipse(
            docW, docH, marqueeRect(m_anchor, m_cursor, m_constrain, m_fromCenter));
    case Kind::FreeLasso:
    case Kind::PolyLasso:
        return core::Selection::polygon(docW, docH, pathPoints());
    }
    return core::Selection(docW, docH);
}

std::optional<core::Selection> SelectionGesture::finish(const core::Selection& base,
                                                        std::uint32_t docW, std::uint32_t docH) {
    if (m_phase == Phase::Idle)
        return std::nullopt;
    std::optional<core::Selection> result;
    if (degenerate()) {
        // A plain click: Replace deselects (the Photoshop click-away); other ops change nothing.
        if (m_op == core::SelectOp::Replace && !base.isEmpty())
            result = core::Selection{};
    } else {
        core::Selection combined = core::Selection::combine(base, shapeMask(docW, docH), m_op);
        if (!combined.anySelected())
            combined = core::Selection{}; // deselect rather than land an all-zero active mask
        if (combined != base)
            result = std::move(combined); // a no-op combine isn't worth an undo step
    }
    cancel();
    return result;
}

void SelectionGesture::cancel() {
    m_phase = Phase::Idle;
    m_points.clear();
    m_previewDirty = false;
}

core::Selection SelectionGesture::preview(const core::Selection& base, std::uint32_t docW,
                                          std::uint32_t docH, double strokeDocPx) const {
    if (m_phase == Phase::Idle)
        return base;
    if (m_kind == Kind::Rect || m_kind == Kind::Ellipse)
        return core::Selection::combine(base, shapeMask(docW, docH), m_op);
    const int width = std::max(1, static_cast<int>(std::lround(strokeDocPx)));
    core::Selection line = base.isEmpty() ? core::Selection(docW, docH) : base;
    const std::vector<common::Vec2> path = pathPoints(); // smoothed for a FreeLasso, else verbatim
    for (std::size_t i = 0; i + 1 < path.size(); ++i)
        strokeSegment(line, path[i], path[i + 1], width);
    if (m_kind == Kind::PolyLasso && !m_points.empty())
        strokeSegment(line, m_points.back(), m_cursor, width); // the rubber segment to the cursor
    return line;
}

// ---- S16-i selection move / nudge -----------------------------------------------------------

void SelectionMoveGesture::begin(core::Selection base, common::Vec2 anchor) {
    m_base = std::move(base);
    m_anchor = anchor;
    m_active = true;
    m_hasAnchor = true;
    m_dx = m_dy = 0;
}

void SelectionMoveGesture::beginNudge(core::Selection base) {
    m_base = std::move(base);
    m_anchor = {0.0, 0.0};
    m_active = true;
    m_hasAnchor = false;
    m_dx = m_dy = 0;
}

bool SelectionMoveGesture::dragTo(common::Vec2 docPt) {
    if (!dragging())
        return false;
    const long dx = std::lround(docPt.x - m_anchor.x);
    const long dy = std::lround(docPt.y - m_anchor.y);
    if (dx == m_dx && dy == m_dy)
        return false; // sub-pixel motion: the integer mask is unchanged, skip the rebuild
    m_dx = dx;
    m_dy = dy;
    return true;
}

void SelectionMoveGesture::nudge(long dx, long dy) {
    if (!m_active)
        return;
    m_dx += dx;
    m_dy += dy;
}

core::Selection SelectionMoveGesture::current() const {
    if (!m_active)
        return {};
    return m_base.translated(m_dx, m_dy); // always from the base -- see the header's clipping note
}

std::optional<core::Selection> SelectionMoveGesture::finish() {
    if (!m_active)
        return std::nullopt;
    const bool travelled = moved();
    core::Selection out = current();
    cancel();
    if (!travelled)
        return std::nullopt; // a click that never moved: no undo step
    return out;
}

void SelectionMoveGesture::cancel() {
    m_active = false;
    m_hasAnchor = false;
    m_base = core::Selection{};
    m_dx = m_dy = 0;
}

} // namespace mosaic::ui
