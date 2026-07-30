#include "core/vector/to_path.hpp"

#include "core/vector/boolean.hpp" // baking a live compound down to its resolved outline
#include "core/vector/flatten.hpp" // the callout's spliced outline converts through the flattener

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <variant>
#include <vector>

namespace mosaic::core::vec {
namespace {

using common::Vec2;

constexpr double kEps = 1e-9;

Node corner(Vec2 p) { return Node{p, p, p, Node::Type::Corner}; }

// Append the elliptical arc  P(t) = c + (rx cos t, ry sin t),  t from `a0` sweeping `sweep`, as
// cubic nodes. At most 90 degrees per cubic; the control offsets ride the parametric tangent
// P'(t) = (-rx sin t, ry cos t) scaled by k = 4/3 tan(dt/4) -- the classic circular-arc
// approximation, which stays right for an ellipse because an ellipse is an affine image of a
// circle and Bezier control points map affinely.
//
// The arc's FIRST anchor is appended only when `nodes` does not already end on it (so a corner can
// hand its tangent point straight in). The LAST node leaves outHandle == anchor, i.e. straight out;
// whoever continues the path overwrites it.
void appendArc(std::vector<Node>& nodes, Vec2 c, double rx, double ry, double a0, double sweep) {
    const auto P = [&](double t) { return Vec2{c.x + rx * std::cos(t), c.y + ry * std::sin(t)}; };
    const auto D = [&](double t) { return Vec2{-rx * std::sin(t), ry * std::cos(t)}; };

    const Vec2 start = P(a0);
    if (nodes.empty() || (nodes.back().anchor - start).length() > kEps)
        nodes.push_back(corner(start));

    const int seg = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / (M_PI * 0.5) - 1e-9)));
    const double dt = sweep / seg;
    const double k = (4.0 / 3.0) * std::tan(dt * 0.25);
    for (int i = 0; i < seg; ++i) {
        const double t0 = a0 + dt * i;
        const double t1 = t0 + dt;
        const Vec2 p1 = P(t1);
        nodes.back().outHandle = nodes.back().anchor + D(t0) * k;
        Node n;
        n.anchor = p1;
        n.inHandle = p1 - D(t1) * k;
        n.outHandle = p1; // straight out until someone continues
        n.type = Node::Type::Smooth;
        nodes.push_back(n);
    }
}

// The node-emitting twin of flatten.cpp's emitCorner: the corner at vertex V between neighbours A
// and B, shaped by `radius`/`style`. Every decision here -- the tangent inset t, its clamp to half
// the shorter edge, the fillet centre, the minor-arc choice, and every degenerate fallback -- is
// the same as the flattener's, because a converted path must trace the shape it came from.
void emitCornerNodes(std::vector<Node>& out, Vec2 A, Vec2 V, Vec2 B, double radius,
                     CornerStyle style) {
    if (style == CornerStyle::None || radius <= kEps) {
        out.push_back(corner(V));
        return;
    }
    const Vec2 e1 = A - V, e2 = B - V;
    const double l1 = e1.length(), l2 = e2.length();
    if (l1 < kEps || l2 < kEps) {
        out.push_back(corner(V));
        return;
    }
    const Vec2 n1 = e1 * (1.0 / l1), n2 = e2 * (1.0 / l2);
    const double alpha = std::acos(std::clamp(n1.dot(n2), -1.0, 1.0));
    if (alpha < 1e-6 || (M_PI - alpha) < 1e-6) { // collinear: nothing to round
        out.push_back(corner(V));
        return;
    }
    double t = radius / std::tan(alpha * 0.5);
    t = std::min(t, 0.5 * std::min(l1, l2));
    if (t <= kEps) {
        out.push_back(corner(V));
        return;
    }
    const Vec2 p0 = V + n1 * t; // tangent point on the A-side edge
    const Vec2 p1 = V + n2 * t; // tangent point on the B-side edge
    if (style == CornerStyle::Bevel) {
        out.push_back(corner(p0));
        out.push_back(corner(p1));
        return;
    }
    Vec2 center;
    double arcR = 0.0;
    if (style == CornerStyle::Inverse) { // concave scoop: a circle centred on V, biting inward
        center = V;
        arcR = t;
    } else { // Round: convex fillet, centre on the inward bisector
        arcR = t * std::tan(alpha * 0.5);
        const Vec2 bis = n1 + n2;
        const double bl = bis.length();
        if (bl < kEps) {
            out.push_back(corner(V));
            return;
        }
        center = V + bis * ((t / std::cos(alpha * 0.5)) / bl);
    }
    const double a0 = std::atan2(p0.y - center.y, p0.x - center.x);
    const double a1 = std::atan2(p1.y - center.y, p1.x - center.x);
    double sweep = a1 - a0; // the MINOR arc between the tangent points
    while (sweep <= -M_PI)
        sweep += 2.0 * M_PI;
    while (sweep > M_PI)
        sweep -= 2.0 * M_PI;

    out.push_back(corner(p0));
    appendArc(out, center, arcR, arcR, a0, sweep);
    out.back().type = Node::Type::Corner; // the arc ends where a straight edge resumes
}

// A closed subpath through `verts`, each sharp vertex replaced by its corner. The shared engine
// behind rect / polygon / star, exactly as flattenCornered is for the flattener.
SubPath corneredSubPath(const std::vector<Vec2>& verts, const std::vector<double>& radii,
                        const std::vector<CornerStyle>& styles) {
    SubPath sp;
    sp.closed = true;
    const std::size_t n = verts.size();
    if (n < 3) {
        for (const Vec2 v : verts)
            sp.nodes.push_back(corner(v));
        return sp;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2 A = verts[(i + n - 1) % n];
        const Vec2 B = verts[(i + 1) % n];
        emitCornerNodes(sp.nodes, A, verts[i], B, radii[i], styles[i]);
    }
    return sp;
}

Path fromRect(const RectShape& r) {
    Path p;
    const double w = std::abs(r.size.x), h = std::abs(r.size.y);
    if (w < kEps || h < kEps)
        return p;
    const double hw = w * 0.5, hh = h * 0.5;
    // y-down vertices in the cornerRadius/cornerStyle index order: TL, TR, BR, BL.
    const std::vector<Vec2> verts = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    const std::vector<double> radii(r.cornerRadius.begin(), r.cornerRadius.end());
    const std::vector<CornerStyle> styles(r.cornerStyle.begin(), r.cornerStyle.end());
    p.subpaths.push_back(corneredSubPath(verts, radii, styles));
    return p;
}

Path fromEllipse(const EllipseShape& e) {
    Path p;
    const double rx = std::abs(e.radii.x), ry = std::abs(e.radii.y);
    if (rx < kEps || ry < kEps)
        return p;
    SubPath sp;
    const double sweep = e.endAngle - e.startAngle;
    if (std::abs(sweep) >= 2.0 * M_PI - 1e-6) { // a full ellipse: four smooth quarter-arcs
        appendArc(sp.nodes, {0.0, 0.0}, rx, ry, e.startAngle, 2.0 * M_PI);
        sp.closed = true;
        // The sweep came back to the first anchor. Fold that duplicate into the seam so the closing
        // cubic carries the arc's own handles rather than a straight line across the ellipse.
        if (sp.nodes.size() > 1) {
            sp.nodes.front().inHandle = sp.nodes.back().inHandle;
            sp.nodes.front().type = Node::Type::Smooth;
            sp.nodes.pop_back();
        }
        p.subpaths.push_back(std::move(sp));
        return p;
    }
    appendArc(sp.nodes, {0.0, 0.0}, rx, ry, e.startAngle, sweep);
    switch (e.arcMode) {
    case EllipseShape::ArcMode::Open:
        sp.closed = false; // the bare arc (a stroke)
        break;
    case EllipseShape::ArcMode::Chord:
        sp.closed = true; // straight chord, end -> start
        break;
    case EllipseShape::ArcMode::Pie: // via the centre (a slice)
        sp.nodes.back().type = Node::Type::Corner;
        sp.nodes.push_back(corner({0.0, 0.0}));
        sp.closed = true;
        break;
    }
    if (!sp.nodes.empty())
        p.subpaths.push_back(std::move(sp));
    return p;
}

Path fromPolygon(const PolygonShape& poly) {
    const int n = std::max(3, poly.sides);
    const double r = std::abs(poly.radius);
    std::vector<Vec2> verts;
    verts.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) { // the first vertex points "up" (y-down -> -pi/2)
        const double t = -M_PI / 2.0 + 2.0 * M_PI * static_cast<double>(i) / n;
        verts.push_back({r * std::cos(t), r * std::sin(t)});
    }
    Path p;
    p.subpaths.push_back(corneredSubPath(
        verts, std::vector<double>(static_cast<std::size_t>(n), std::abs(poly.cornerRadius)),
        std::vector<CornerStyle>(static_cast<std::size_t>(n), poly.cornerStyle)));
    return p;
}

Path fromStar(const StarShape& s) {
    const int pts = std::max(2, s.points);
    const double ro = std::abs(s.outerRadius), ri = std::abs(s.innerRadius);
    const int n = pts * 2;
    std::vector<Vec2> verts;
    std::vector<double> radii;
    std::vector<CornerStyle> styles;
    verts.reserve(static_cast<std::size_t>(n));
    radii.reserve(static_cast<std::size_t>(n));
    styles.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) { // alternate outer tip / inner valley, step = pi/points
        const bool outer = (i % 2 == 0);
        const double r = outer ? ro : ri;
        const double t = -M_PI / 2.0 + M_PI * static_cast<double>(i) / pts;
        verts.push_back({r * std::cos(t), r * std::sin(t)});
        radii.push_back(outer ? std::abs(s.pointRadius) : std::abs(s.valleyRadius));
        styles.push_back(CornerStyle::Round); // tips/valleys round (no chamfer/scoop for stars)
    }
    Path p;
    p.subpaths.push_back(corneredSubPath(verts, radii, styles));
    return p;
}

Path fromLine(const LineShape& l) {
    Path p;
    SubPath sp;
    sp.closed = false;
    // The path IS the centreline; the Object's Stroke gives it weight, and the Solid/Hollow/Outlined
    // paint mode is a stroke style, not geometry.
    if (l.bend.x * l.bend.x + l.bend.y * l.bend.y < 1e-12) {
        sp.nodes.push_back(corner(l.a));
        sp.nodes.push_back(corner(l.b));
        p.subpaths.push_back(std::move(sp));
        return p;
    }
    // A quadratic through a, (midpoint + bend), b has control C = midpoint + 2*bend. Elevating a
    // quadratic to a cubic is EXACT: c1 = a + 2/3 (C-a), c2 = b + 2/3 (C-b).
    const Vec2 mid = (l.a + l.b) * 0.5;
    const Vec2 C = mid + l.bend * 2.0;
    Node n0 = corner(l.a);
    n0.outHandle = l.a + (C - l.a) * (2.0 / 3.0);
    Node n1 = corner(l.b);
    n1.inHandle = l.b + (C - l.b) * (2.0 / 3.0);
    sp.nodes.push_back(n0);
    sp.nodes.push_back(n1);
    p.subpaths.push_back(std::move(sp));
    return p;
}

// ---- The widened shape library (S26-c) -------------------------------------------------------
// Same contract as above: a converted path must trace the shape it came from, so each of these
// mirrors its flattener decision for decision (the same clamps, the same vertex order).

Path fromCross(const CrossShape& x) {
    Path p;
    const double w = std::abs(x.size.x), h = std::abs(x.size.y);
    if (w < kEps || h < kEps)
        return p;
    const double hw = w * 0.5, hh = h * 0.5;
    const double ht = std::clamp(std::abs(x.armRatio), 0.02, 0.98) * std::min(w, h) * 0.5;
    const std::vector<Vec2> verts = {{-ht, -hh}, {ht, -hh},  {ht, -ht},  {hw, -ht},
                                     {hw, ht},   {ht, ht},   {ht, hh},   {-ht, hh},
                                     {-ht, ht},  {-hw, ht},  {-hw, -ht}, {-ht, -ht}};
    p.subpaths.push_back(corneredSubPath(verts,
                                         std::vector<double>(verts.size(), std::abs(x.cornerRadius)),
                                         std::vector<CornerStyle>(verts.size(), x.cornerStyle)));
    return p;
}

Path fromArrow(const ArrowShape& a) {
    Path p;
    const double w = std::abs(a.size.x), h = std::abs(a.size.y);
    if (w < kEps || h < kEps)
        return p;
    const double hw = w * 0.5, hh = h * 0.5;
    const double head = std::clamp(std::abs(a.headRatio), 0.02, a.doubleHeaded ? 0.49 : 0.98) * w;
    const double s = std::clamp(std::abs(a.shaftRatio), 0.02, 1.0) * hh;
    const double hx = hw - head;
    const double nx = hx + std::clamp(std::abs(a.notchRatio), 0.0, 0.95) * head;
    const std::vector<Vec2> verts =
        a.doubleHeaded ? std::vector<Vec2>{{-hw, 0.0}, {-hx, -hh}, {-nx, -s}, {nx, -s}, {hx, -hh},
                                           {hw, 0.0},  {hx, hh},   {nx, s},   {-nx, s}, {-hx, hh}}
                       : std::vector<Vec2>{{-hw, -s}, {nx, -s}, {hx, -hh}, {hw, 0.0},
                                           {hx, hh},  {nx, s},  {-hw, s}};
    SubPath sp;
    sp.closed = true;
    for (const Vec2 v : verts)
        sp.nodes.push_back(corner(v));
    p.subpaths.push_back(std::move(sp));
    return p;
}

Path fromBanner(const BannerShape& b) {
    Path p;
    const double w = std::abs(b.size.x), h = std::abs(b.size.y);
    if (w < kEps || h < kEps)
        return p;
    const double hw = w * 0.5, hh = h * 0.5;
    const double d = std::clamp(std::abs(b.pointRatio), 0.0, 0.45) * w;
    std::vector<Vec2> verts;
    verts.reserve(6);
    verts.push_back({-hw, -hh});
    if (b.style == BannerShape::Style::Chevron) {
        verts.push_back({hw - d, -hh});
        verts.push_back({hw, 0.0});
        verts.push_back({hw - d, hh});
    } else {
        verts.push_back({hw, -hh});
        verts.push_back({hw - d, 0.0});
        verts.push_back({hw, hh});
    }
    verts.push_back({-hw, hh});
    if (b.notchTail)
        verts.push_back({-hw + d, 0.0});
    p.subpaths.push_back(corneredSubPath(verts,
                                         std::vector<double>(verts.size(), std::abs(b.cornerRadius)),
                                         std::vector<CornerStyle>(verts.size(), CornerStyle::Round)));
    return p;
}

Path fromRing(const RingShape& r) {
    Path p;
    const double rx = std::abs(r.radii.x), ry = std::abs(r.radii.y);
    if (rx < kEps || ry < kEps)
        return p;
    const double k = std::clamp(r.innerRatio, 0.0, 0.98);
    const double sweep = r.endAngle - r.startAngle;
    if (std::abs(sweep) >= 2.0 * M_PI - 1e-6) { // a full annulus: two closed rings
        const auto full = [&](double sx, double sy, double sw) {
            SubPath sp;
            appendArc(sp.nodes, {0.0, 0.0}, sx, sy, r.startAngle, sw);
            sp.closed = true;
            if (sp.nodes.size() > 1) { // fold the duplicate seam anchor (fromEllipse's trick)
                sp.nodes.front().inHandle = sp.nodes.back().inHandle;
                sp.nodes.front().type = Node::Type::Smooth;
                sp.nodes.pop_back();
            }
            return sp;
        };
        p.subpaths.push_back(full(rx, ry, 2.0 * M_PI));
        if (k > 1e-6) // the hole runs the other way, so NonZero cancels inside it
            p.subpaths.push_back(full(rx * k, ry * k, -2.0 * M_PI));
        return p;
    }
    SubPath sp; // a segment: out along the outer arc, back along the inner one (or via the centre)
    appendArc(sp.nodes, {0.0, 0.0}, rx, ry, r.startAngle, sweep);
    sp.nodes.back().type = Node::Type::Corner;
    if (k > 1e-6)
        appendArc(sp.nodes, {0.0, 0.0}, rx * k, ry * k, r.startAngle + sweep, -sweep);
    else
        sp.nodes.push_back(corner({0.0, 0.0}));
    sp.closed = true;
    p.subpaths.push_back(std::move(sp));
    return p;
}

Path fromHeart(const HeartShape& hs) {
    Path p;
    const double w = std::abs(hs.size.x), h = std::abs(hs.size.y);
    if (w < kEps || h < kEps)
        return p;
    const double hw = w * 0.5, hh = h * 0.5;
    const double dy = -hh + std::clamp(hs.cleft, 0.02, 0.60) * h;
    const double sy = -hh + (0.20 + 0.19 * std::clamp(hs.lobe, 0.0, 1.0)) * h;
    const double ty = (8.0 * -hh - dy - sy) / 6.0; // peaks the lobes exactly on the box's top edge
    const double b1 = sy + (hh - sy) * 0.416, b2 = sy + (hh - sy) * 0.667;
    const auto node = [](Vec2 a, Vec2 in, Vec2 out, Node::Type t) { return Node{a, in, out, t}; };
    SubPath sp;
    sp.closed = true;
    sp.nodes.push_back(node({0.0, dy}, {0.0, ty}, {0.0, ty}, Node::Type::Corner)); // the cleft cusp
    sp.nodes.push_back(node({hw, sy}, {hw, ty}, {hw, b1}, Node::Type::Smooth));    // right shoulder
    sp.nodes.push_back(
        node({0.0, hh}, {hw * 0.5, b2}, {-hw * 0.5, b2}, Node::Type::Corner));     // the bottom tip
    sp.nodes.push_back(node({-hw, sy}, {-hw, b1}, {-hw, ty}, Node::Type::Smooth)); // left shoulder
    p.subpaths.push_back(std::move(sp));
    return p;
}

Path fromCallout(const CalloutShape& cs) {
    // The tail is spliced INTO the body ring at flatten time (one closed outline a stroke can
    // trace with no seam), so the honest conversion is that same outline as a polyline. It is the
    // one shape here whose path is denser than its ideal curve -- worth it for the seamless ring.
    Path p;
    for (const Contour& c : flatten(Geometry{ParametricShape{cs}})) {
        SubPath sp;
        sp.closed = c.closed;
        for (const Vec2 v : c.points)
            sp.nodes.push_back(corner(v));
        if (!sp.nodes.empty())
            p.subpaths.push_back(std::move(sp));
    }
    return p;
}

} // namespace

Path pathFromShape(const ParametricShape& shape) {
    Path p = std::visit(
        [](const auto& s) -> Path {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, RectShape>)
                return fromRect(s);
            else if constexpr (std::is_same_v<T, EllipseShape>)
                return fromEllipse(s);
            else if constexpr (std::is_same_v<T, PolygonShape>)
                return fromPolygon(s);
            else if constexpr (std::is_same_v<T, StarShape>)
                return fromStar(s);
            else if constexpr (std::is_same_v<T, LineShape>)
                return fromLine(s);
            else if constexpr (std::is_same_v<T, CalloutShape>)
                return fromCallout(s);
            else if constexpr (std::is_same_v<T, ArrowShape>)
                return fromArrow(s);
            else if constexpr (std::is_same_v<T, RingShape>)
                return fromRing(s);
            else if constexpr (std::is_same_v<T, CrossShape>)
                return fromCross(s);
            else if constexpr (std::is_same_v<T, HeartShape>)
                return fromHeart(s);
            else
                return fromBanner(s);
        },
        shape);
    p.fillRule = FillRule::NonZero; // what the shape rasteriser assumes for a parametric primitive
    return p;
}

Path pathFromGeometry(const Geometry& geometry) {
    if (const auto* path = std::get_if<Path>(&geometry))
        return *path; // already editable
    // A live boolean (S28) has no parametric form to convert: its outline is COMPUTED, so it bakes
    // to the resolved region as closed polyline subpaths, exactly the concession CalloutShape's
    // spliced outline already makes (docs/vector-model.md §7.7, §9). NonZero by construction --
    // the kernel normalizes outers/holes -- so no fill-rule negotiation follows the bake.
    if (const auto* compound = std::get_if<BooleanCompound>(&geometry))
        return bakedBooleanPath(*compound);
    return pathFromShape(std::get<ParametricShape>(geometry));
}

Path transformedPath(const Path& path, const common::Affine2D& t) {
    Path out = path; // fillRule and closedness ride along untouched
    for (SubPath& sp : out.subpaths) {
        for (Node& n : sp.nodes) {
            n.anchor = t.apply(n.anchor);
            n.inHandle = t.apply(n.inHandle);   // handles are stored ABSOLUTE (geometry.hpp),
            n.outHandle = t.apply(n.outHandle); // so they map exactly like the anchor
        }
    }
    return out;
}

} // namespace mosaic::core::vec
