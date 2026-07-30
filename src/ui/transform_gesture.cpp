#include "ui/transform_gesture.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mosaic::ui {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRotateSnap = kPi / 36.0; // Shift snaps rotation to 5 degrees
constexpr double kMinScale = 1e-3;         // a gesture can't collapse the layer to singular

// The layer-local position of handle `i` on the framed content rect (same indexing as the
// screen-space centres: 0-3 corners TL,TR,BR,BL; 4-7 edge mids T,R,B,L).
common::Vec2 localHandlePoint(int i, const common::Rect& r) {
    switch (i) {
    case 0: return {r.x, r.y};
    case 1: return {r.right(), r.y};
    case 2: return {r.right(), r.bottom()};
    case 3: return {r.x, r.bottom()};
    case 4: return {r.x + r.w * 0.5, r.y};
    case 5: return {r.right(), r.y + r.h * 0.5};
    case 6: return {r.x + r.w * 0.5, r.bottom()};
    default: return {r.x, r.y + r.h * 0.5};
    }
}

double clampedScale(double s) {
    if (std::abs(s) < kMinScale)
        return std::copysign(kMinScale, s == 0.0 ? 1.0 : s);
    return s;
}

// True when `p` is inside the convex (possibly flipped/rotated) quad: every edge cross product
// shares a sign. Same test hitTransformControls uses for the box body.
bool pointInQuad(common::Vec2 p, const std::array<common::Vec2, 4>& c) {
    bool anyPos = false, anyNeg = false;
    for (int i = 0; i < 4; ++i) {
        const common::Vec2 a = c[i];
        const common::Vec2 b = c[(i + 1) % 4];
        const double cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        anyPos = anyPos || cross > 0.0;
        anyNeg = anyNeg || cross < 0.0;
    }
    return !(anyPos && anyNeg);
}

// Distance from `p` to the segment [a,b].
double distToSegment(common::Vec2 p, common::Vec2 a, common::Vec2 b) {
    const common::Vec2 ab = b - a;
    const double len2 = ab.x * ab.x + ab.y * ab.y;
    double t = len2 > 0.0 ? ((p - a).x * ab.x + (p - a).y * ab.y) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return (p - (a + ab * t)).length();
}

} // namespace

std::array<common::Vec2, 8> transformHandleCenters(const std::array<common::Vec2, 4>& c) {
    return {c[0],
            c[1],
            c[2],
            c[3],
            (c[0] + c[1]) * 0.5,
            (c[1] + c[2]) * 0.5,
            (c[2] + c[3]) * 0.5,
            (c[3] + c[0]) * 0.5};
}

double transformQuadWackiness(const std::array<common::Vec2, 4>& c) {
    std::array<common::Vec2, 4> e{};
    std::array<double, 4> len{};
    double maxLen = 0.0;
    for (int i = 0; i < 4; ++i) {
        e[i] = c[(i + 1) % 4] - c[i];
        len[i] = e[i].length();
        maxLen = std::max(maxLen, len[i]);
    }
    if (maxLen <= 1e-9)
        return 1.0; // collapsed to a point: maximally unfindable
    for (const double l : len)
        if (l <= 1e-6 * maxLen)
            return 1.0; // a zero-length edge: the quad is a line or a triangle
    // Corner-angle deviation. A rectangle's corners are 90 deg WHATEVER its rotation or size, so
    // this term is rotation- and scale-invariant; shear and perspective foreshortening are exactly
    // what bend it away. Mean deviation of 45 deg (a fully collapsed parallelogram is 90) = 1.
    double dev = 0.0;
    for (int i = 0; i < 4; ++i) {
        const common::Vec2 in = e[(i + 3) % 4]; // edge arriving at corner i
        const common::Vec2 out = e[i];          // edge leaving it
        const double ax = -in.x, ay = -in.y;    // interior angle = angle(-in, out)
        const double cross = ax * out.y - ay * out.x;
        const double dot = ax * out.x + ay * out.y;
        const double interior = std::atan2(std::abs(cross), dot); // [0, pi]
        dev += std::abs(interior - kPi / 2.0);
    }
    const double angleTerm = std::clamp((dev / 4.0) / (kPi / 4.0), 0.0, 1.0);
    // Degeneracy: a sliver hides its corners along a line even with perfect right angles (a 3D
    // solid seen nearly edge-on). Aspect from opposite-edge averages; bites below 15%.
    const double w = 0.5 * (len[0] + len[2]);
    const double h = 0.5 * (len[1] + len[3]);
    const double aspect = std::min(w, h) / std::max(w, h);
    const double thinTerm = std::clamp((0.15 - aspect) / 0.15, 0.0, 1.0);
    return std::max(angleTerm, thinTerm);
}

double transformQuadMismatch(const std::array<common::Vec2, 4>& quad,
                             const std::array<common::Vec2, 4>& visible) {
    const double diag =
        std::max((quad[2] - quad[0]).length(), (quad[3] - quad[1]).length());
    if (diag <= 1e-9)
        return 1.0; // a collapsed rotate quad is nowhere near anything
    double mean = 0.0;
    for (int i = 0; i < 4; ++i)
        mean += (quad[i] - visible[i]).length();
    mean *= 0.25;
    // Displacement of half the quad's own diagonal reads as fully lost.
    return std::clamp(2.0 * mean / diag, 0.0, 1.0);
}

double rotateDotOpacity(double wackiness) {
    return 0.5 * std::clamp(wackiness / 0.5, 0.0, 1.0);
}

std::optional<TransformHit> hitTransformControls(common::Vec2 p,
                                                 const std::array<common::Vec2, 4>& corners,
                                                 double handleRadius, double rotateBand) {
    const std::array<common::Vec2, 8> handles = transformHandleCenters(corners);
    int best = -1;
    double bestDist = handleRadius;
    for (int i = 0; i < 8; ++i) {
        const double d = (p - handles[i]).length();
        if (d <= bestDist) {
            bestDist = d;
            best = i;
        }
    }
    if (best >= 0)
        return TransformHit{TransformMode::Scale, best};

    // Inside the (convex, possibly flipped) quad: every edge cross product has the same sign.
    // Computed BEFORE the rotate band so the rotate affordance is restricted to OUTSIDE the box
    // (user 2026-06-17): a hover inside the body is always Move, never Rotate.
    bool anyPos = false;
    bool anyNeg = false;
    for (int i = 0; i < 4; ++i) {
        const common::Vec2 a = corners[i];
        const common::Vec2 b = corners[(i + 1) % 4];
        const double cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        anyPos = anyPos || cross > 0.0;
        anyNeg = anyNeg || cross < 0.0;
    }
    if (!(anyPos && anyNeg))
        return TransformHit{TransformMode::Move, -1};

    // Outside the box, just beyond a corner handle: the rotate band.
    for (const int i : {0, 1, 2, 3})
        if ((p - corners[i]).length() <= rotateBand)
            return TransformHit{TransformMode::Rotate, -1};
    return std::nullopt;
}

TextBoxControl hitTextEditBox(common::Vec2 p, const std::array<common::Vec2, 4>& c,
                              double handleRadius, double rotateBand, double edgeBand,
                              int resizeCorner) {
    // The resize handle wins wherever it sits (it straddles its corner, atop edge/band).
    if ((p - c[static_cast<std::size_t>(resizeCorner & 3)]).length() <= handleRadius)
        return TextBoxControl::ResizeBR;
    double edgeDist = std::numeric_limits<double>::infinity();
    for (int i = 0; i < 4; ++i)
        edgeDist = std::min(edgeDist, distToSegment(p, c[i], c[(i + 1) % 4]));
    if (pointInQuad(p, c))
        // Inside: the frame border (within edgeBand of an edge) moves; deeper interior is the caret.
        return edgeDist <= edgeBand ? TextBoxControl::Move : TextBoxControl::None;
    // Outside the box: just beyond a corner rotates; a thin sliver off an edge still moves.
    for (int i = 0; i < 4; ++i)
        if ((p - c[i]).length() <= rotateBand)
            return TextBoxControl::Rotate;
    return edgeDist <= edgeBand ? TextBoxControl::Move : TextBoxControl::None;
}

bool TransformGesture::begin(TransformMode mode, int handle, common::Vec2 docPt,
                             const common::Affine2D& base, const common::Rect& content,
                             std::optional<common::Vec2> pivotLocal) {
    m_mode = TransformMode::None;
    if (mode == TransformMode::None || content.empty())
        return false;
    const std::optional<common::Affine2D> inv = base.inverse();
    if (!inv)
        return false; // a singular transform has no local space to work in
    m_base = base;
    m_baseInv = *inv;
    m_content = content;
    m_pivotLocal = pivotLocal;
    m_startDoc = docPt;
    m_startLocal = m_baseInv.apply(docPt);
    // The rotate (and, with an explicit anchor, the scale) pivot: the reference point if the user
    // placed one, else the content centre. In document space, so a rotate delta turns the box about it.
    m_centerDoc = base.apply(pivotLocal ? *pivotLocal : content.center());
    m_handle = std::clamp(handle, 0, 7);
    if (mode == TransformMode::Rotate) {
        const common::Vec2 d = docPt - m_centerDoc;
        if (d.length() < 1e-9)
            return false; // grabbing the pivot itself: the angle is undefined
        m_startAngle = std::atan2(d.y, d.x);
    }
    m_mode = mode;
    return true;
}

common::Affine2D TransformGesture::transformFor(common::Vec2 docPt, bool shiftDown,
                                                bool altDown) const {
    using common::Affine2D;
    using common::Vec2;
    switch (m_mode) {
    case TransformMode::Move: {
        Vec2 d = docPt - m_startDoc;
        if (shiftDown) { // lock to the dominant axis
            if (std::abs(d.x) >= std::abs(d.y))
                d.y = 0.0;
            else
                d.x = 0.0;
        }
        // A raster Move is lossless: snap the translation to whole document pixels so moved content
        // stays pixel-aligned (no nearest-resample aliasing) and the transform box rides the pixel
        // grid (user 2026-06-17). Sub-pixel placement + resampling is Free Transform's job, not Move's.
        d.x = std::round(d.x);
        d.y = std::round(d.y);
        return Affine2D::translation(d.x, d.y) * m_base;
    }
    case TransformMode::Scale: {
        const Vec2 hp = localHandlePoint(m_handle, m_content);
        // Alt always scales around the centre. Otherwise the fixed point is the user's anchor
        // (reference point) when one was placed, else the handle opposite hp (the Affinity default).
        const Vec2 anchor = altDown ? m_content.center()
                            : m_pivotLocal
                                ? *m_pivotLocal
                                : Vec2{m_content.x + m_content.right() - hp.x,
                                       m_content.y + m_content.bottom() - hp.y};
        const Vec2 local = m_baseInv.apply(docPt);
        // The handle index dictates the axes: edge mids scale ONE axis. The grab never lands
        // exactly on the handle (hit radius), so the perpendicular delta is a near-zero
        // denominator that would explode the other factor if we let the geometry decide.
        const bool scalesX = m_handle != 4 && m_handle != 6; // not the top/bottom mids
        const bool scalesY = m_handle != 5 && m_handle != 7; // not the left/right mids
        double sx = 1.0;
        double sy = 1.0;
        const double dx = m_startLocal.x - anchor.x;
        const double dy = m_startLocal.y - anchor.y;
        if (scalesX && std::abs(dx) > 1e-9)
            sx = (local.x - anchor.x) / dx;
        if (scalesY && std::abs(dy) > 1e-9)
            sy = (local.y - anchor.y) / dy;
        if (shiftDown) { // uniform: the dominant (or only driven) factor drives both axes
            double s = std::abs(sx) >= std::abs(sy) ? sx : sy;
            if (!scalesX)
                s = sy; // top/bottom mid: only y is driven
            else if (!scalesY)
                s = sx; // left/right mid
            sx = sy = s;
        }
        sx = clampedScale(sx);
        sy = clampedScale(sy);
        return m_base * Affine2D::translation(anchor.x, anchor.y) * Affine2D::scaling(sx, sy) *
               Affine2D::translation(-anchor.x, -anchor.y);
    }
    case TransformMode::Rotate: {
        const Vec2 d = docPt - m_centerDoc;
        if (d.length() < 1e-9)
            return m_base;
        double delta = std::atan2(d.y, d.x) - m_startAngle;
        if (shiftDown)
            delta = std::round(delta / kRotateSnap) * kRotateSnap;
        return Affine2D::translation(m_centerDoc.x, m_centerDoc.y) * Affine2D::rotation(delta) *
               Affine2D::translation(-m_centerDoc.x, -m_centerDoc.y) * m_base;
    }
    case TransformMode::None:
        break;
    }
    return m_base;
}

} // namespace mosaic::ui
