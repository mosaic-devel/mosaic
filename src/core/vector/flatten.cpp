#include "core/vector/flatten.hpp"

#include "core/vector/boolean.hpp" // the S28 compound arm: flatten the children, then resolve the op
#include "core/vector/corner.hpp" // the shared vertex rings (one definition of "where the corners are")

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace mosaic::core::vec {
namespace {

using common::Affine2D;
using common::Rect;
using common::Vec2;

constexpr int kMaxSubdivDepth = 18;

// Upper bound on (16 x squared chord deviation) of a cubic's control points from its chord,
// evaluated in DEVICE space so smoothness tracks zoom (classic ux/uy/vx/vy flatness form).
double flatnessDeviceSq(const Affine2D& m, Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3) {
    const Vec2 d0 = m.apply(p0), d1 = m.apply(p1), d2 = m.apply(p2), d3 = m.apply(p3);
    const double ux = 3.0 * d1.x - 2.0 * d0.x - d3.x;
    const double uy = 3.0 * d1.y - 2.0 * d0.y - d3.y;
    const double vx = 3.0 * d2.x - d0.x - 2.0 * d3.x;
    const double vy = 3.0 * d2.y - d0.y - 2.0 * d3.y;
    return std::max(ux * ux, vx * vx) + std::max(uy * uy, vy * vy);
}

// Adaptive de Casteljau subdivision: emits LOCAL-space points (p0 assumed already emitted),
// but decides flatness in device space against 16*tol^2.
void subdivideCubic(std::vector<Vec2>& out, const Affine2D& toDevice, Vec2 p0, Vec2 p1, Vec2 p2,
                    Vec2 p3, double tolSq16, int depth) {
    if (depth >= kMaxSubdivDepth || flatnessDeviceSq(toDevice, p0, p1, p2, p3) <= tolSq16) {
        out.push_back(p3);
        return;
    }
    const Vec2 p01 = (p0 + p1) * 0.5;
    const Vec2 p12 = (p1 + p2) * 0.5;
    const Vec2 p23 = (p2 + p3) * 0.5;
    const Vec2 p012 = (p01 + p12) * 0.5;
    const Vec2 p123 = (p12 + p23) * 0.5;
    const Vec2 p0123 = (p012 + p123) * 0.5;
    subdivideCubic(out, toDevice, p0, p01, p012, p0123, tolSq16, depth + 1);
    subdivideCubic(out, toDevice, p0123, p123, p23, p3, tolSq16, depth + 1);
}

Contour flattenSubPath(const Affine2D& toDevice, const SubPath& sp, double tolSq16) {
    Contour c;
    c.closed = sp.closed;
    const auto& nodes = sp.nodes;
    if (nodes.empty()) return c;
    c.points.push_back(nodes[0].anchor);
    if (nodes.size() == 1) return c;  // degenerate single-point subpath

    const std::size_t segCount = sp.closed ? nodes.size() : nodes.size() - 1;
    for (std::size_t i = 0; i < segCount; ++i) {
        const Node& a = nodes[i];
        const Node& b = nodes[(i + 1) % nodes.size()];
        // Both handles coincident with their anchors == a straight segment (the line convention).
        if (a.outHandle == a.anchor && b.inHandle == b.anchor)
            c.points.push_back(b.anchor);
        else
            subdivideCubic(c.points, toDevice, a.anchor, a.outHandle, b.inHandle, b.anchor, tolSq16, 0);
    }
    // A closed contour's final segment lands back on points[0]; drop the duplicate so the ring is
    // clean (the `closed` flag carries the wrap). Exact compare is valid: the base case re-emits
    // the original endpoint, not a recomputed midpoint.
    if (sp.closed && c.points.size() > 1 && c.points.front() == c.points.back())
        c.points.pop_back();
    return c;
}

Contours flattenPath(const Affine2D& toDevice, const Path& path, double tolSq16) {
    Contours out;
    out.reserve(path.subpaths.size());
    for (const auto& sp : path.subpaths) {
        Contour c = flattenSubPath(toDevice, sp, tolSq16);
        if (!c.points.empty()) out.push_back(std::move(c));
    }
    return out;
}

// Segment count to keep an arc's chord error under `tolPx`, given the arc's device-space radius.
int arcSegments(double radiusDevice, double sweepRad, double tolPx) {
    radiusDevice = std::max(radiusDevice, 1e-6);
    const double maxAngle = 2.0 * std::acos(std::clamp(1.0 - tolPx / radiusDevice, -1.0, 1.0));
    const int n = (maxAngle > 1e-6) ? static_cast<int>(std::ceil(std::abs(sweepRad) / maxAngle)) : 1;
    return std::max(n, 1);
}

// Emit the corner at vertex V (between previous vertex A and next vertex B) into `out`, shaped by
// `radius`/`style` (docs/vector-model.md §7.6). All styles share tangent points an inset `t` along
// each edge from V, where t = radius / tan(alpha/2) for the half-angle between the edges; `t` is
// clamped to half the shorter adjacent edge so neighbouring corners never overlap. Round/Inverse
// arcs are tessellated to `tolPx` device px. Falls back to the sharp vertex when there is nothing
// to round (style None, ~zero radius, or near-collinear edges).
void emitCorner(std::vector<Vec2>& out, const Affine2D& toDevice, Vec2 A, Vec2 V, Vec2 B,
                double radius, CornerStyle style, double tolPx) {
    if (style == CornerStyle::None || radius <= 1e-9) {
        out.push_back(V);
        return;
    }
    const Vec2 e1 = A - V, e2 = B - V;
    const double l1 = e1.length(), l2 = e2.length();
    if (l1 < 1e-9 || l2 < 1e-9) {
        out.push_back(V);
        return;
    }
    const Vec2 n1 = e1 * (1.0 / l1), n2 = e2 * (1.0 / l2);
    const double alpha = std::acos(std::clamp(n1.dot(n2), -1.0, 1.0));  // angle between edges, (0,pi)
    if (alpha < 1e-6 || (M_PI - alpha) < 1e-6) {  // collinear edges: nothing to round
        out.push_back(V);
        return;
    }
    double t = radius / std::tan(alpha * 0.5);
    t = std::min(t, 0.5 * std::min(l1, l2));
    if (t <= 1e-9) {
        out.push_back(V);
        return;
    }
    const Vec2 p0 = V + n1 * t;  // tangent point on the A-side edge
    const Vec2 p1 = V + n2 * t;  // tangent point on the B-side edge
    if (style == CornerStyle::Bevel) {
        out.push_back(p0);
        out.push_back(p1);
        return;
    }
    Vec2 center;
    double arcR = 0.0;
    if (style == CornerStyle::Inverse) {  // concave scoop: a circle centred on V, biting inward
        center = V;
        arcR = t;
    } else {  // Round: a convex fillet, centre on the inward bisector at distance t/cos(alpha/2)
        arcR = t * std::tan(alpha * 0.5);  // effective fillet radius after the clamp
        const Vec2 bis = n1 + n2;
        const double bl = bis.length();
        if (bl < 1e-9) {
            out.push_back(V);
            return;
        }
        center = V + bis * ((t / std::cos(alpha * 0.5)) / bl);
    }
    const double a0 = std::atan2(p0.y - center.y, p0.x - center.x);
    const double a1 = std::atan2(p1.y - center.y, p1.x - center.x);
    double sweep = a1 - a0;  // walk the MINOR arc between the tangent points (central angle < pi)
    while (sweep <= -M_PI) sweep += 2.0 * M_PI;
    while (sweep > M_PI) sweep -= 2.0 * M_PI;
    const double rdev = toDevice.applyVector({arcR, 0}).length();
    const int seg = std::max(1, arcSegments(rdev, sweep, tolPx));
    for (int i = 0; i <= seg; ++i) {
        const double a = a0 + sweep * static_cast<double>(i) / seg;
        out.push_back({center.x + arcR * std::cos(a), center.y + arcR * std::sin(a)});
    }
}

// Flatten a closed polygon whose sharp vertices are `verts`, replacing each vertex with its
// per-vertex corner (radius/style). The shared corner engine behind rect/polygon/star.
Contour flattenCornered(const Affine2D& toDevice, const std::vector<Vec2>& verts,
                        const std::vector<double>& radii, const std::vector<CornerStyle>& styles,
                        double tolPx) {
    Contour c;
    c.closed = true;
    const std::size_t n = verts.size();
    if (n < 3) {
        c.points = verts;
        return c;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2 A = verts[(i + n - 1) % n];
        const Vec2 B = verts[(i + 1) % n];
        emitCorner(c.points, toDevice, A, verts[i], B, radii[i], styles[i], tolPx);
    }
    return c;
}

Contour flattenRect(const Affine2D& toDevice, const RectShape& r, double tolPx) {
    Contour c;
    c.closed = true;
    const double w = std::abs(r.size.x), h = std::abs(r.size.y);
    if (w < 1e-9 || h < 1e-9) return c;
    // y-down vertices in the cornerRadius/cornerStyle index order: TL, TR, BR, BL. The corner engine
    // emits each (rounded/inverse/bevel/sharp) join; straight edges are the gaps between them.
    const CorneredPolygon poly = rectPolygon(r);
    return flattenCornered(toDevice, poly.verts, poly.radii, poly.styles, tolPx);
}

Contour flattenEllipse(const Affine2D& toDevice, const EllipseShape& e, double tolPx) {
    Contour c;
    const double rx = std::abs(e.radii.x), ry = std::abs(e.radii.y);
    if (rx < 1e-9 || ry < 1e-9) {
        c.closed = true;
        return c;
    }
    const double rdev = std::max(toDevice.applyVector({rx, 0}).length(),
                                 toDevice.applyVector({0, ry}).length());
    const double sweep = e.endAngle - e.startAngle;
    if (std::abs(sweep) >= 2.0 * M_PI - 1e-6) {  // a full ellipse (arcMode irrelevant)
        c.closed = true;
        const int n = std::max(8, arcSegments(rdev, 2.0 * M_PI, tolPx));
        c.points.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double t = 2.0 * M_PI * static_cast<double>(i) / n;
            c.points.push_back({rx * std::cos(t), ry * std::sin(t)});
        }
        return c;
    }
    // A partial sweep is an arc: sample start..end inclusive, then close per the arc mode.
    const int n = std::max(2, arcSegments(rdev, sweep, tolPx));
    c.points.reserve(static_cast<std::size_t>(n) + 2);
    for (int i = 0; i <= n; ++i) {
        const double t = e.startAngle + sweep * static_cast<double>(i) / n;
        c.points.push_back({rx * std::cos(t), ry * std::sin(t)});
    }
    switch (e.arcMode) {
        case EllipseShape::ArcMode::Open:   c.closed = false; break;  // the bare arc (a stroke)
        case EllipseShape::ArcMode::Chord:  c.closed = true;  break;  // straight chord end->start
        case EllipseShape::ArcMode::Pie:                              // via the centre (a slice)
            c.points.push_back({0.0, 0.0});
            c.closed = true;
            break;
    }
    return c;
}

Contour flattenPolygon(const Affine2D& toDevice, const PolygonShape& p, double tolPx) {
    const CorneredPolygon poly = polygonPolygon(p);  // first vertex points "up" (y-down -> -pi/2)
    return flattenCornered(toDevice, poly.verts, poly.radii, poly.styles, tolPx);
}

Contour flattenStar(const Affine2D& toDevice, const StarShape& s, double tolPx) {
    // Alternating outer tip / inner valley, step = pi/p; tips and valleys round (no chamfer/scoop
    // for stars) with their own independent radii.
    const CorneredPolygon poly = starPolygon(s);
    return flattenCornered(toDevice, poly.verts, poly.radii, poly.styles, tolPx);
}

Contour flattenLine(const Affine2D& toDevice, const LineShape& l, double tolPx) {
    Contour c;
    c.closed = false;
    if (l.bend.x * l.bend.x + l.bend.y * l.bend.y < 1e-12) { // straight segment
        c.points = {l.a, l.b};
        return c;
    }
    // A quadratic curve through a, (midpoint + bend) at t=0.5, b: the control point that makes the
    // curve pass through midpoint+bend is C = midpoint + 2*bend. Flatten it via the cubic subdivider
    // (quadratic -> cubic: c1 = a + 2/3(C-a), c2 = b + 2/3(C-b)).
    const Vec2 mid = (l.a + l.b) * 0.5;
    const Vec2 C = mid + l.bend * 2.0;
    const Vec2 c1 = l.a + (C - l.a) * (2.0 / 3.0);
    const Vec2 c2 = l.b + (C - l.b) * (2.0 / 3.0);
    const double tol = std::max(tolPx, 1e-4);
    c.points.push_back(l.a);
    subdivideCubic(c.points, toDevice, l.a, c1, c2, l.b, 16.0 * tol * tol, 0);
    return c;
}

// ---- The widened shape library (S26-c) -------------------------------------------------------
// Everything below follows the same two rules as the five originals: the figure is centred on the
// local origin and tight in its own size parameters, and it is expressed as ordinary polyline
// contours -- no new seam, no renderer change. Where a corner can be rounded the shared
// flattenCornered engine does it, so the four CornerStyles behave identically everywhere.

Contour flattenCross(const Affine2D& toDevice, const CrossShape& x, double tolPx) {
    Contour c;
    c.closed = true;
    const double w = std::abs(x.size.x), h = std::abs(x.size.y);
    if (w < 1e-9 || h < 1e-9) return c;
    // Twelve vertices, clockwise from the top-left of the vertical bar (y-down); the arm thickness
    // is a fraction of the SHORTER side, so the figure stays a cross at any aspect.
    const CorneredPolygon poly = crossPolygon(x);
    return flattenCornered(toDevice, poly.verts, poly.radii, poly.styles, tolPx);
}

// The arrow points along the local +x axis; a rotated arrow is the layer transform's job, exactly
// as a rotated rectangle is. Both proportions are ratios of `size`, so a resize keeps the look.
Contour flattenArrow(const ArrowShape& a) {
    Contour c;
    c.closed = true;
    const double w = std::abs(a.size.x), h = std::abs(a.size.y);
    if (w < 1e-9 || h < 1e-9) return c;
    const double hw = w * 0.5, hh = h * 0.5;
    const double head = std::clamp(std::abs(a.headRatio), 0.02, a.doubleHeaded ? 0.49 : 0.98) * w;
    const double s = std::clamp(std::abs(a.shaftRatio), 0.02, 1.0) * hh;  // shaft HALF thickness
    const double hx = hw - head;                                          // the head's base
    const double nx = hx + std::clamp(std::abs(a.notchRatio), 0.0, 0.95) * head;  // shaft junction
    if (a.doubleHeaded)
        c.points = {{-hw, 0.0}, {-hx, -hh}, {-nx, -s}, {nx, -s}, {hx, -hh},
                    {hw, 0.0},  {hx, hh},   {nx, s},   {-nx, s}, {-hx, hh}};
    else
        c.points = {{-hw, -s}, {nx, -s}, {hx, -hh}, {hw, 0.0}, {hx, hh}, {nx, s}, {-hw, s}};
    return c;
}

Contour flattenBanner(const Affine2D& toDevice, const BannerShape& b, double tolPx) {
    Contour c;
    c.closed = true;
    const double w = std::abs(b.size.x), h = std::abs(b.size.y);
    if (w < 1e-9 || h < 1e-9) return c;
    // The right edge is pushed OUT into a point (Chevron) or cut IN as a swallow-tail (Banner),
    // optionally with the matching notch in the left edge.
    const CorneredPolygon poly = bannerPolygon(b);
    return flattenCornered(toDevice, poly.verts, poly.radii, poly.styles, tolPx);
}

Contours flattenRing(const Affine2D& toDevice, const RingShape& r, double tolPx) {
    Contours out;
    const double rx = std::abs(r.radii.x), ry = std::abs(r.radii.y);
    if (rx < 1e-9 || ry < 1e-9) return out;
    const double k = std::clamp(r.innerRatio, 0.0, 0.98);
    const double rdev = std::max(toDevice.applyVector({rx, 0}).length(),
                                 toDevice.applyVector({0, ry}).length());
    const double sweep = r.endAngle - r.startAngle;
    if (std::abs(sweep) >= 2.0 * M_PI - 1e-6) {  // a full annulus
        const int n = std::max(8, arcSegments(rdev, 2.0 * M_PI, tolPx));
        Contour outer;
        outer.closed = true;
        outer.points.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double t = 2.0 * M_PI * static_cast<double>(i) / n;
            outer.points.push_back({rx * std::cos(t), ry * std::sin(t)});
        }
        out.push_back(std::move(outer));
        if (k > 1e-6) {  // the hole runs the OTHER way, so the NonZero windings cancel inside it
            Contour inner;
            inner.closed = true;
            inner.points.reserve(static_cast<std::size_t>(n));
            for (int i = n - 1; i >= 0; --i) {
                const double t = 2.0 * M_PI * static_cast<double>(i) / n;
                inner.points.push_back({rx * k * std::cos(t), ry * k * std::sin(t)});
            }
            out.push_back(std::move(inner));
        }
        return out;
    }
    // A ring SEGMENT: out along the outer arc and back along the inner one -- or, with no hole,
    // back via the centre, which is exactly a pie slice. One closed contour, no fill-rule subtlety.
    const int n = std::max(2, arcSegments(rdev, sweep, tolPx));
    Contour c;
    c.closed = true;
    c.points.reserve(static_cast<std::size_t>(n) * 2 + 2);
    for (int i = 0; i <= n; ++i) {
        const double t = r.startAngle + sweep * static_cast<double>(i) / n;
        c.points.push_back({rx * std::cos(t), ry * std::sin(t)});
    }
    if (k > 1e-6)
        for (int i = n; i >= 0; --i) {
            const double t = r.startAngle + sweep * static_cast<double>(i) / n;
            c.points.push_back({rx * k * std::cos(t), ry * k * std::sin(t)});
        }
    else
        c.points.push_back({0.0, 0.0});
    out.push_back(std::move(c));
    return out;
}

// Four cubics: from the top cleft up over each shoulder, then down to the bottom tip. The shoulder
// controls are solved so the lobes peak EXACTLY at -hh, which is what keeps the heart tight in its
// box (a heart that floats inside its own bounds makes every alignment and resize feel wrong).
Contour flattenHeart(const Affine2D& toDevice, const HeartShape& hs, double tolPx) {
    Contour c;
    c.closed = true;
    const double w = std::abs(hs.size.x), h = std::abs(hs.size.y);
    if (w < 1e-9 || h < 1e-9) return c;
    const double hw = w * 0.5, hh = h * 0.5;
    const double dy = -hh + std::clamp(hs.cleft, 0.02, 0.60) * h;          // the top cleft
    const double sy = -hh + (0.20 + 0.19 * std::clamp(hs.lobe, 0.0, 1.0)) * h;  // the shoulders
    // A cubic with equal middle control y's peaks at t = 0.5, where y = (dy + 6*ty + sy)/8; solve
    // that for the ty which puts the peak on the box's top edge.
    const double ty = (8.0 * -hh - dy - sy) / 6.0;
    const double b1 = sy + (hh - sy) * 0.416;  // the lower cubic's control heights, from the
    const double b2 = sy + (hh - sy) * 0.667;  // classic heart outline
    const double tol = std::max(tolPx, 1e-4);
    const double tolSq16 = 16.0 * tol * tol;
    const Vec2 dip{0.0, dy}, rs{hw, sy}, tip{0.0, hh}, ls{-hw, sy};
    c.points.push_back(dip);
    subdivideCubic(c.points, toDevice, dip, {0.0, ty}, {hw, ty}, rs, tolSq16, 0);
    subdivideCubic(c.points, toDevice, rs, {hw, b1}, {hw * 0.5, b2}, tip, tolSq16, 0);
    subdivideCubic(c.points, toDevice, tip, {-hw * 0.5, b2}, {-hw, b1}, ls, tolSq16, 0);
    subdivideCubic(c.points, toDevice, ls, {-hw, ty}, {0.0, ty}, dip, tolSq16, 0);
    if (c.points.size() > 1 && c.points.front() == c.points.back()) c.points.pop_back();
    return c;
}

// Arc-length parameterisation of a closed point ring -- the machinery a callout tail needs to be
// SPLICED INTO the body outline. (Overlaying the tail as a second contour would fill identically
// but leave a seam right across the body edge under any stroke, which is the tell of a fake
// speech bubble.) Non-owning: it borrows the body contour's points.
class RingWalk {
public:
    explicit RingWalk(const std::vector<Vec2>& pts) : m_p(&pts) {
        m_cum.reserve(pts.size() + 1);
        m_cum.push_back(0.0);
        double s = 0.0;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            s += (pts[(i + 1) % pts.size()] - pts[i]).length();
            m_cum.push_back(s);
        }
    }
    [[nodiscard]] double length() const { return m_cum.back(); }
    [[nodiscard]] double cumAt(std::size_t i) const { return m_cum[i]; }
    [[nodiscard]] Vec2 at(double s) const {
        const std::size_t n = m_p->size();
        const double L = length();
        if (n == 0) return {0.0, 0.0};
        if (L <= 1e-12) return (*m_p)[0];
        s = std::fmod(s, L);
        if (s < 0.0) s += L;
        std::size_t lo = 0, hi = n - 1;
        while (lo < hi) {  // the last vertex whose cumulative length is <= s
            const std::size_t mid = (lo + hi + 1) / 2;
            if (m_cum[mid] <= s)
                lo = mid;
            else
                hi = mid - 1;
        }
        const double seg = m_cum[lo + 1] - m_cum[lo];
        const double t = seg > 1e-12 ? (s - m_cum[lo]) / seg : 0.0;
        const Vec2 a = (*m_p)[lo], b = (*m_p)[(lo + 1) % n];
        return a + (b - a) * t;
    }
    // Arc distance of the ring's crossing with the ray from the ORIGIN at `angle`. Callout bodies
    // are convex and centred on the origin, so exactly one segment carries the forward hit.
    [[nodiscard]] double arcAtAngle(double angle) const {
        const Vec2 d{std::cos(angle), std::sin(angle)};
        const std::size_t n = m_p->size();
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2 a = (*m_p)[i], b = (*m_p)[(i + 1) % n];
            const double ca = d.x * a.y - d.y * a.x;  // which side of the ray each endpoint is on
            const double cb = d.x * b.y - d.y * b.x;
            if ((ca > 0.0 && cb > 0.0) || (ca < 0.0 && cb < 0.0)) continue;
            const double den = ca - cb;
            const double t = std::abs(den) > 1e-12 ? std::clamp(ca / den, 0.0, 1.0) : 0.0;
            if ((a + (b - a) * t).dot(d) <= 0.0) continue;  // the antipodal crossing, not ours
            return m_cum[i] + (m_cum[i + 1] - m_cum[i]) * t;
        }
        return 0.0;
    }

private:
    const std::vector<Vec2>* m_p;
    std::vector<double> m_cum;
};

Contours flattenCallout(const Affine2D& toDevice, const CalloutShape& cs, double tolPx) {
    Contours out;
    const double w = std::abs(cs.size.x), h = std::abs(cs.size.y);
    if (w < 1e-9 || h < 1e-9) return out;
    Contour body =
        cs.body == CalloutShape::Body::Ellipse
            ? flattenEllipse(toDevice, EllipseShape{{w * 0.5, h * 0.5}}, tolPx)
            : flattenRect(toDevice, RectShape::uniform({w, h}, std::abs(cs.cornerRadius)), tolPx);
    const double tail = std::max(0.0, cs.tailLength);
    if (body.points.size() < 3 || tail <= 1e-9) {  // no tail: the body IS the shape
        out.push_back(std::move(body));
        return out;
    }
    const RingWalk ring(body.points);
    const double L = ring.length();
    if (L <= 1e-9) {
        out.push_back(std::move(body));
        return out;
    }
    const double sMid = ring.arcAtAngle(cs.tailAngle);
    const Vec2 base = ring.at(sMid);
    const Vec2 dir{std::cos(cs.tailAngle), std::sin(cs.tailAngle)};
    const double e = std::max(1e-6, L * 1e-3);
    const Vec2 tv = ring.at(sMid + e) - ring.at(sMid - e);
    const double tvl = tv.length();
    const Vec2 tangent = tvl > 1e-12 ? tv * (1.0 / tvl) : Vec2{-dir.y, dir.x};

    if (cs.tail == CalloutShape::Tail::Bubbles) {  // a thought balloon: body + a trail of puffs
        out.push_back(std::move(body));
        const int count = std::clamp(cs.bubbleCount, 1, 8);
        const double step = tail / count;
        double r0 = std::abs(cs.tailWidth) * 0.5;
        if (r0 < 1e-6) r0 = std::min(w, h) * 0.12;
        r0 = std::min(r0, step * 0.48);  // never let neighbouring puffs merge
        for (int i = 0; i < count; ++i) {
            const double shrink =
                count > 1 ? 1.0 - 0.55 * static_cast<double>(i) / (count - 1) : 1.0;
            const double r = r0 * shrink;
            if (r < 1e-6) continue;
            const Vec2 ctr = base + dir * (step * (static_cast<double>(i) + 0.5));
            const int n = std::max(8, arcSegments(toDevice.applyVector({r, 0}).length(),
                                                  2.0 * M_PI, tolPx));
            Contour puff;
            puff.closed = true;
            puff.points.reserve(static_cast<std::size_t>(n));
            for (int k = 0; k < n; ++k) {
                const double t = 2.0 * M_PI * static_cast<double>(k) / n;
                puff.points.push_back({ctr.x + r * std::cos(t), ctr.y + r * std::sin(t)});
            }
            out.push_back(std::move(puff));
        }
        return out;
    }

    // Pointer: A -> tip -> B, then every body vertex from B forward around to A. That keeps the
    // body's winding and yields ONE closed ring, so a stroke traces the balloon without a seam.
    double half = std::abs(cs.tailWidth) * 0.5;  // half the base width, along the perimeter
    if (half < 1e-6) half = std::min(w, h) * 0.10;
    half = std::clamp(half, L * 0.004, L * 0.30);
    const Vec2 tip = base + dir * tail + tangent * (std::clamp(cs.tailSkew, -1.0, 1.0) * tail);
    Contour c;
    c.closed = true;
    c.points.reserve(body.points.size() + 3);
    c.points.push_back(ring.at(sMid - half));
    c.points.push_back(tip);
    c.points.push_back(ring.at(sMid + half));
    const double span = L - 2.0 * half;  // the arc that survives, from B forward to A
    std::vector<std::pair<double, Vec2>> rest;
    rest.reserve(body.points.size());
    for (std::size_t i = 0; i < body.points.size(); ++i) {
        double f = std::fmod(ring.cumAt(i) - (sMid + half), L);  // arc measured from B
        if (f < 0.0) f += L;
        if (f > 1e-9 && f < span - 1e-9) rest.emplace_back(f, body.points[i]);
    }
    std::sort(rest.begin(), rest.end(),
              [](const std::pair<double, Vec2>& a, const std::pair<double, Vec2>& b) {
                  return a.first < b.first;
              });
    for (const std::pair<double, Vec2>& fp : rest) c.points.push_back(fp.second);
    out.push_back(std::move(c));
    return out;
}

Contours flattenShape(const Affine2D& toDevice, const ParametricShape& s, double tolPx) {
    Contours out;
    const auto add = [&out](Contour c) {
        if (!c.points.empty()) out.push_back(std::move(c));
    };
    const auto addAll = [&add](Contours cs) {
        for (Contour& c : cs) add(std::move(c));
    };
    std::visit(
        [&](const auto& shape) {
            using T = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<T, RectShape>)
                add(flattenRect(toDevice, shape, tolPx));
            else if constexpr (std::is_same_v<T, EllipseShape>)
                add(flattenEllipse(toDevice, shape, tolPx));
            else if constexpr (std::is_same_v<T, PolygonShape>)
                add(flattenPolygon(toDevice, shape, tolPx));
            else if constexpr (std::is_same_v<T, StarShape>)
                add(flattenStar(toDevice, shape, tolPx));
            else if constexpr (std::is_same_v<T, LineShape>)
                add(flattenLine(toDevice, shape, tolPx));
            else if constexpr (std::is_same_v<T, CalloutShape>)
                addAll(flattenCallout(toDevice, shape, tolPx));
            else if constexpr (std::is_same_v<T, ArrowShape>)
                add(flattenArrow(shape));
            else if constexpr (std::is_same_v<T, RingShape>)
                addAll(flattenRing(toDevice, shape, tolPx));
            else if constexpr (std::is_same_v<T, CrossShape>)
                add(flattenCross(toDevice, shape, tolPx));
            else if constexpr (std::is_same_v<T, HeartShape>)
                add(flattenHeart(toDevice, shape, tolPx));
            else if constexpr (std::is_same_v<T, BannerShape>)
                add(flattenBanner(toDevice, shape, tolPx));
        },
        s);
    return out;
}

}  // namespace

Contours flatten(const Geometry& geometry, double tolerancePx, const Affine2D& toDevice) {
    const double tol = std::max(tolerancePx, 1e-4);
    const double tolSq16 = 16.0 * tol * tol;
    Contours out;
    std::visit(
        [&](const auto& g) {
            using T = std::decay_t<decltype(g)>;
            if constexpr (std::is_same_v<T, Path>)
                out = flattenPath(toDevice, g, tolSq16);
            else if constexpr (std::is_same_v<T, ParametricShape>)
                out = flattenShape(toDevice, g, tol);
            // S28 live booleans: recurse into the children at the SAME tolerance and device
            // transform, then resolve the op on the polylines (core/vector/boolean.cpp). Every
            // consumer downstream of this seam -- fill, stroke, hit-test, bounds, thumbnails,
            // rasterize, text-on-path -- becomes correct with no Bezier clipping anywhere.
            else if constexpr (std::is_same_v<T, BooleanCompound>)
                out = flattenCompound(g, tol, toDevice);
        },
        geometry);
    return out;
}

std::optional<Rect> contentBounds(const Geometry& geometry) {
    const Contours cs = flatten(geometry);  // identity transform, default tolerance
    bool any = false;
    double minx = 0, miny = 0, maxx = 0, maxy = 0;
    for (const auto& c : cs)
        for (const Vec2& p : c.points) {
            if (!any) {
                minx = maxx = p.x;
                miny = maxy = p.y;
                any = true;
            } else {
                minx = std::min(minx, p.x);
                miny = std::min(miny, p.y);
                maxx = std::max(maxx, p.x);
                maxy = std::max(maxy, p.y);
            }
        }
    if (!any) return std::nullopt;
    return Rect{minx, miny, maxx - minx, maxy - miny};
}

std::optional<Rect> contentBounds(const Object& object) {
    std::optional<Rect> b = contentBounds(object.geometry);
    if (!b || !object.stroke.enabled || object.stroke.width <= 0.0) return b;
    double out = object.stroke.width * 0.5;  // Center
    if (object.stroke.align == StrokeAlign::Inside) out = 0.0;
    else if (object.stroke.align == StrokeAlign::Outside) out = object.stroke.width;
    // A Hollow / Outlined line's border reaches `borderWidth` past the weight edge (§7.5).
    if (const auto* ps = std::get_if<ParametricShape>(&object.geometry))
        if (const auto* line = std::get_if<LineShape>(ps);
            line != nullptr && line->paint != LineShape::Paint::Solid)
            out += std::max(0.0, line->borderWidth);
    if (out <= 0.0) return b;
    return Rect{b->x - out, b->y - out, b->w + 2.0 * out, b->h + 2.0 * out};
}

double contourLength(const Contours& contours) {
    double total = 0;
    for (const auto& c : contours) {
        const std::size_t n = c.points.size();
        if (n < 2) continue;
        for (std::size_t i = 0; i + 1 < n; ++i) total += (c.points[i + 1] - c.points[i]).length();
        if (c.closed) total += (c.points.front() - c.points.back()).length();
    }
    return total;
}

PathSample samplePathAt(const Contours& contours, double arcDistance) {
    double remaining = std::max(0.0, arcDistance);
    bool haveFirst = false;
    Vec2 firstPt{0, 0};
    bool any = false;
    Vec2 lastPos{0, 0}, lastTan{1, 0};
    for (const auto& c : contours) {
        const std::size_t n = c.points.size();
        if (n == 0) continue;
        if (!haveFirst) {
            firstPt = c.points[0];
            haveFirst = true;
        }
        const std::size_t segs = c.closed ? n : n - 1;
        for (std::size_t i = 0; i < segs; ++i) {
            const Vec2 a = c.points[i];
            const Vec2 b = c.points[(i + 1) % n];
            const Vec2 d = b - a;
            const double len = d.length();
            any = true;
            lastPos = b;
            if (len > 1e-12) lastTan = d * (1.0 / len);
            if (len >= remaining) {
                const double t = (len > 1e-12) ? remaining / len : 0.0;
                return {a + d * t, (len > 1e-12) ? d * (1.0 / len) : Vec2{1, 0}};
            }
            remaining -= len;
        }
    }
    if (!any) return {haveFirst ? firstPt : Vec2{0, 0}, {1, 0}};
    return {lastPos, lastTan};  // past the end -> clamp to the final point/tangent
}

double nearestArcDistance(const Contours& contours, Vec2 p, double* outDistance) {
    double best = std::numeric_limits<double>::infinity();
    double bestS = 0.0;
    double walked = 0.0;
    for (const auto& c : contours) {
        const std::size_t n = c.points.size();
        if (n == 0) continue;
        const std::size_t segs = c.closed ? n : (n > 0 ? n - 1 : 0);
        for (std::size_t i = 0; i < segs; ++i) {
            const Vec2 a = c.points[i];
            const Vec2 b = c.points[(i + 1) % n];
            const Vec2 d = b - a;
            const double len2 = d.x * d.x + d.y * d.y;
            const double t =
                len2 > 1e-18 ? std::clamp(((p - a).x * d.x + (p - a).y * d.y) / len2, 0.0, 1.0)
                             : 0.0;
            const Vec2 q = a + d * t;
            const double dist = (p - q).length();
            if (dist < best) {
                best = dist;
                bestS = walked + t * std::sqrt(len2);
            }
            walked += std::sqrt(len2);
        }
    }
    if (outDistance != nullptr) *outDistance = std::isfinite(best) ? best : 0.0;
    return std::isfinite(best) ? bestS : 0.0;
}

}  // namespace mosaic::core::vec
