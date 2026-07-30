#include "core/vector/corner.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>
#include <variant>

namespace mosaic::core::vec {
namespace {

// The trigonometric frame shared by cornerPointAt() and cornerRadiusForPoint(). Exactly the
// quantities flatten()'s emitCorner() derives, so the two can never disagree about where a corner
// is: the two edge unit vectors, the angle between them, and the tangent-inset ceiling.
struct Frame {
    bool ok = false;
    Vec2 vertex;
    Vec2 n1, n2;      // unit vectors from V toward the previous / next vertex
    Vec2 axis;        // unit bisector (n1 + n2 normalised) -- points INTO the corner's fillet
    double half = 0.0;   // alpha/2, in (0, pi/2)
    double tanHalf = 0.0;
    double cosHalf = 1.0;
    double sinHalf = 0.0;
    double tMax = 0.0;   // half the shorter adjacent edge: emitCorner's clamp on the inset
};

Frame frameAt(const std::vector<Vec2>& verts, std::size_t index) {
    Frame f;
    const std::size_t n = verts.size();
    if (n == 0) return f;
    const std::size_t i = index % n;
    f.vertex = verts[i];
    if (n < 3) return f;
    const Vec2 V = verts[i];
    const Vec2 e1 = verts[(i + n - 1) % n] - V;
    const Vec2 e2 = verts[(i + 1) % n] - V;
    const double l1 = e1.length(), l2 = e2.length();
    if (l1 < 1e-9 || l2 < 1e-9) return f;
    f.n1 = e1 * (1.0 / l1);
    f.n2 = e2 * (1.0 / l2);
    const double alpha = std::acos(std::clamp(f.n1.dot(f.n2), -1.0, 1.0));
    if (alpha < 1e-6 || (M_PI - alpha) < 1e-6) return f;  // collinear edges: nothing to round
    const Vec2 bis = f.n1 + f.n2;
    const double bl = bis.length();
    if (bl < 1e-9) return f;
    f.axis = bis * (1.0 / bl);
    f.half = alpha * 0.5;
    f.tanHalf = std::tan(f.half);
    f.cosHalf = std::cos(f.half);
    f.sinHalf = std::sin(f.half);
    f.tMax = 0.5 * std::min(l1, l2);
    f.ok = f.tanHalf > 1e-9 && f.cosHalf > 1e-9;
    return f;
}

// How far along the bisector the emitted corner's MIDPOINT sits, per tangent-inset unit:
//   Round   -- centre is t/cos(half) out, the arc bulges back by its radius t*tan(half);
//   Inverse -- the scoop is centred ON the vertex, so its deepest point is exactly t out;
//   Bevel   -- the chamfer's midpoint is the average of the two tangent points, t*cos(half) out.
double apexFactor(const Frame& f, CornerStyle style) {
    switch (style) {
    case CornerStyle::Bevel: return f.cosHalf;
    case CornerStyle::Inverse: return 1.0;
    case CornerStyle::None: return 0.0;
    default: return (1.0 - f.sinHalf) / f.cosHalf;  // Round
    }
}

}  // namespace

// ---- the vertex rings ------------------------------------------------------------------------
// Each one mirrors the ring its flatten*() counterpart builds; flatten() calls THESE, so there is
// one definition and a designer handle derived from them cannot drift off the drawn outline.

CorneredPolygon rectPolygon(const RectShape& r) {
    CorneredPolygon out;
    const double hw = std::abs(r.size.x) * 0.5, hh = std::abs(r.size.y) * 0.5;
    out.verts = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};  // TL, TR, BR, BL (y-down)
    out.radii.assign(r.cornerRadius.begin(), r.cornerRadius.end());
    out.styles.assign(r.cornerStyle.begin(), r.cornerStyle.end());
    return out;
}

CorneredPolygon polygonPolygon(const PolygonShape& p) {
    CorneredPolygon out;
    const int n = std::max(3, p.sides);
    const double r = std::abs(p.radius);
    out.verts.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {  // first vertex points "up" (y-down -> -pi/2)
        const double t = -M_PI / 2.0 + 2.0 * M_PI * static_cast<double>(i) / n;
        out.verts.push_back({r * std::cos(t), r * std::sin(t)});
    }
    out.radii.assign(static_cast<std::size_t>(n), std::abs(p.cornerRadius));
    out.styles.assign(static_cast<std::size_t>(n), p.cornerStyle);
    return out;
}

CorneredPolygon starPolygon(const StarShape& s) {
    CorneredPolygon out;
    const int p = std::max(2, s.points);
    const double ro = std::abs(s.outerRadius), ri = std::abs(s.innerRadius);
    const int n = p * 2;
    out.verts.reserve(static_cast<std::size_t>(n));
    out.radii.reserve(static_cast<std::size_t>(n));
    out.styles.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {  // alternate outer tip / inner valley, step = pi/p
        const bool outer = (i % 2 == 0);
        const double r = outer ? ro : ri;
        const double t = -M_PI / 2.0 + M_PI * static_cast<double>(i) / p;
        out.verts.push_back({r * std::cos(t), r * std::sin(t)});
        out.radii.push_back(outer ? std::abs(s.pointRadius) : std::abs(s.valleyRadius));
        out.styles.push_back(CornerStyle::Round);
    }
    return out;
}

CorneredPolygon crossPolygon(const CrossShape& x) {
    CorneredPolygon out;
    const double w = std::abs(x.size.x), h = std::abs(x.size.y);
    if (w < 1e-9 || h < 1e-9) return out;
    const double hw = w * 0.5, hh = h * 0.5;
    const double ht = std::clamp(std::abs(x.armRatio), 0.02, 0.98) * std::min(w, h) * 0.5;
    // Twelve vertices, clockwise from the top-left of the vertical bar (y-down). Indices 2, 5, 8
    // and 11 are the REFLEX inner corners -- the concave fillets.
    out.verts = {{-ht, -hh}, {ht, -hh},  {ht, -ht},  {hw, -ht},
                 {hw, ht},   {ht, ht},   {ht, hh},   {-ht, hh},
                 {-ht, ht},  {-hw, ht},  {-hw, -ht}, {-ht, -ht}};
    out.radii.assign(out.verts.size(), std::abs(x.cornerRadius));
    out.styles.assign(out.verts.size(), x.cornerStyle);
    return out;
}

CorneredPolygon bannerPolygon(const BannerShape& b) {
    CorneredPolygon out;
    const double w = std::abs(b.size.x), h = std::abs(b.size.y);
    if (w < 1e-9 || h < 1e-9) return out;
    const double hw = w * 0.5, hh = h * 0.5;
    const double d = std::clamp(std::abs(b.pointRatio), 0.0, 0.45) * w;
    out.verts.reserve(6);
    out.verts.push_back({-hw, -hh});
    if (b.style == BannerShape::Style::Chevron) {  // the right edge is pushed OUT into a point
        out.verts.push_back({hw - d, -hh});
        out.verts.push_back({hw, 0.0});
        out.verts.push_back({hw - d, hh});
    } else {                                       // ...or cut IN as a swallow-tail (a ribbon)
        out.verts.push_back({hw, -hh});
        out.verts.push_back({hw - d, 0.0});
        out.verts.push_back({hw, hh});
    }
    out.verts.push_back({-hw, hh});
    if (b.notchTail) out.verts.push_back({-hw + d, 0.0});  // the matching notch in the left edge
    out.radii.assign(out.verts.size(), std::abs(b.cornerRadius));
    out.styles.assign(out.verts.size(), CornerStyle::Round);
    return out;
}

CorneredPolygon calloutBodyPolygon(const CalloutShape& c) {
    if (c.body != CalloutShape::Body::RoundedRect) return {};
    const double w = std::abs(c.size.x), h = std::abs(c.size.y);
    if (w < 1e-9 || h < 1e-9) return {};
    return rectPolygon(RectShape::uniform({w, h}, std::abs(c.cornerRadius)));
}

std::optional<CorneredPolygon> corneredPolygonOf(const ParametricShape& s) {
    std::optional<CorneredPolygon> out;
    std::visit(
        [&](const auto& shape) {
            using T = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<T, RectShape>)
                out = rectPolygon(shape);
            else if constexpr (std::is_same_v<T, PolygonShape>)
                out = polygonPolygon(shape);
            else if constexpr (std::is_same_v<T, StarShape>)
                out = starPolygon(shape);
            else if constexpr (std::is_same_v<T, CrossShape>)
                out = crossPolygon(shape);
            else if constexpr (std::is_same_v<T, BannerShape>)
                out = bannerPolygon(shape);
            else if constexpr (std::is_same_v<T, CalloutShape>) {
                CorneredPolygon body = calloutBodyPolygon(shape);
                if (!body.empty()) out = std::move(body);
            } else
                static_cast<void>(shape);  // ellipse / line / arrow / ring / heart: no cornered ring
        },
        s);
    if (out && out->empty()) out.reset();
    return out;
}

// ---- the resolved corner ---------------------------------------------------------------------

CornerPoint cornerPointAt(const std::vector<Vec2>& verts, std::size_t index, double radius,
                          CornerStyle style) {
    CornerPoint out;
    const Frame f = frameAt(verts, index);
    out.vertex = f.vertex;
    out.apex = f.vertex;
    out.p0 = f.vertex;
    out.p1 = f.vertex;
    if (!f.ok) return out;
    out.axis = f.axis;
    out.maxRadius = f.tMax * f.tanHalf;
    if (style == CornerStyle::None || radius <= 1e-9) return out;  // the sharp vertex
    const double t = std::min(radius / f.tanHalf, f.tMax);
    if (t <= 1e-9) return out;
    out.rounded = true;
    out.radius = t * f.tanHalf;
    out.p0 = f.vertex + f.n1 * t;
    out.p1 = f.vertex + f.n2 * t;
    out.apex = f.vertex + f.axis * (t * apexFactor(f, style));
    return out;
}

CornerPoint cornerPointAt(const CorneredPolygon& poly, std::size_t index) {
    if (poly.empty()) return {};
    const std::size_t i = index % poly.size();
    return cornerPointAt(poly.verts, i, poly.radii[i], poly.styles[i]);
}

double cornerRadiusForPoint(const std::vector<Vec2>& verts, std::size_t index, CornerStyle style,
                            Vec2 p) {
    const Frame f = frameAt(verts, index);
    if (!f.ok) return 0.0;
    const double maxR = f.tMax * f.tanHalf;
    if (style == CornerStyle::None) return 0.0;
    const double factor = apexFactor(f, style);
    // Distance from the vertex along the bisector -- negative (the cursor is on the far side of the
    // corner) reads as "no rounding". A near-flat corner has a vanishing factor, so its apex barely
    // moves: rather than divide by ~0 and explode, saturate.
    const double d = std::max(0.0, (p - f.vertex).dot(f.axis));
    if (factor < 1e-6) return d > 0.0 ? maxR : 0.0;
    return std::clamp((d / factor) * f.tanHalf, 0.0, maxR);
}

double maxCornerRadius(const CorneredPolygon& poly) {
    double best = 0.0;
    bool any = false;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const Frame f = frameAt(poly.verts, i);
        if (!f.ok) continue;
        const double m = f.tMax * f.tanHalf;
        best = any ? std::min(best, m) : m;
        any = true;
    }
    return best;
}

}  // namespace mosaic::core::vec
