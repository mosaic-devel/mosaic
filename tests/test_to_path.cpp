#include "core/vector/flatten.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/to_path.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <vector>

// "Convert to Path" on a parametric shape (S26's promise, wired into the layer context menu).
//
// The load-bearing property is not "it produces some nodes" -- it is that the converted path traces
// the SAME OUTLINE as the shape it came from. So the battery below flattens both and compares the
// polylines geometrically, family by family and corner style by corner style. A conversion that
// dropped a fillet, took the major arc instead of the minor one, or forgot the inset clamp would
// pass a node-count check and fail this one.

using namespace mosaic;
using common::Vec2;
using core::vec::Contour;
using core::vec::Contours;
using core::vec::CornerStyle;
using core::vec::EllipseShape;
using core::vec::Geometry;
using core::vec::LineShape;
using core::vec::Path;
using core::vec::pathFromShape;
using core::vec::PolygonShape;
using core::vec::RectShape;
using core::vec::StarShape;

namespace {

// Flatten both sides tightly, so the comparison measures the CONVERSION error rather than two
// coarse tessellations of the same curve talking past each other.
constexpr double kTol = 0.005;

// What the two outlines may legitimately differ by, for a shape whose largest curvature radius is
// `radius`. Two terms, both real:
//   * The cubic arc approximation. A <= 90-degree arc emitted with k = 4/3 tan(dt/4) control offsets
//     sits within ~2.9e-4 * R of the true arc -- MEASURED here as tolerance -> 0 (r=100 -> 0.0254,
//     r=24 -> 0.0070, r=10 -> 0.0029: dead linear in R, and independent of the flatten tolerance).
//     This is the entire reason a converted path is not bit-identical to a flattened shape, and it
//     is 400x tighter than a screen pixel.
//   * Two polylines, each within kTol of its own curve, can sit 2*kTol apart. A little slack on top.
// Anything that is actually WRONG -- the major arc instead of the minor one, a dropped fillet, a
// forgotten inset clamp -- is off by a fraction of R, not by 3e-4 of it.
double allowedDeviation(double radius) { return 3.0e-4 * radius + 2.5 * kTol; }

double distToSegment(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const double len2 = ab.dot(ab);
    if (len2 < 1e-18)
        return (p - a).length();
    double t = (p - a).dot(ab) / len2;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return (p - (a + ab * t)).length();
}

// Farthest a point of `from` sits from the polyline `to` (a one-sided Hausdorff distance).
double maxDeviation(const Contour& from, const Contour& to) {
    REQUIRE(to.points.size() >= 2);
    double worst = 0.0;
    for (const Vec2 p : from.points) {
        double best = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i + 1 < to.points.size(); ++i)
            best = std::min(best, distToSegment(p, to.points[i], to.points[i + 1]));
        if (to.closed)
            best = std::min(best, distToSegment(p, to.points.back(), to.points.front()));
        worst = std::max(worst, best);
    }
    return worst;
}

// Flatten the shape directly, and flatten the path it converts to; assert the two outlines agree
// from both sides (so neither can be a subset of the other) and that closedness survived.
// `radius` is the shape's largest curvature radius -- see allowedDeviation.
void checkTracesTheSameOutline(const core::vec::ParametricShape& shape, double radius) {
    const double allowed = allowedDeviation(radius);
    const Contours direct = core::vec::flatten(Geometry{shape}, kTol);
    const Contours viaPath = core::vec::flatten(Geometry{pathFromShape(shape)}, kTol);
    REQUIRE(direct.size() == viaPath.size());
    for (std::size_t i = 0; i < direct.size(); ++i) {
        CAPTURE(i);
        CHECK(direct[i].closed == viaPath[i].closed);
        CHECK(maxDeviation(direct[i], viaPath[i]) < allowed);
        CHECK(maxDeviation(viaPath[i], direct[i]) < allowed);
    }
}

std::size_t nodeCount(const Path& p) {
    std::size_t n = 0;
    for (const auto& sp : p.subpaths)
        n += sp.nodes.size();
    return n;
}

} // namespace

TEST_CASE("a converted ellipse is four smooth quarter-arcs, not a sampled polygon") {
    const EllipseShape e{{100.0, 60.0}, 0.0, 2.0 * M_PI, EllipseShape::ArcMode::Open};
    const Path p = pathFromShape(e);
    REQUIRE(p.subpaths.size() == 1);
    CHECK(p.subpaths[0].closed);
    // The whole point of converting rather than flattening: four nodes, every one of them smooth.
    CHECK(p.subpaths[0].nodes.size() == 4);
    for (const auto& n : p.subpaths[0].nodes) {
        CHECK(n.type == core::vec::Node::Type::Smooth);
        CHECK((n.inHandle - n.anchor).length() > 1.0);  // real handles, not a corner node
        CHECK((n.outHandle - n.anchor).length() > 1.0);
    }
    // ... and flattening it traces the same ellipse the shape does.
    checkTracesTheSameOutline(e, 100.0);

    // For comparison: the direct flatten of that ellipse is a hundred-odd points.
    CHECK(core::vec::flatten(Geometry{e}, kTol)[0].points.size() > 40);
}

TEST_CASE("converted ellipse arcs honour Open / Chord / Pie") {
    for (const auto mode : {EllipseShape::ArcMode::Open, EllipseShape::ArcMode::Chord,
                            EllipseShape::ArcMode::Pie}) {
        const EllipseShape e{{80.0, 80.0}, 0.2, 2.4, mode};
        CAPTURE(static_cast<int>(mode));
        checkTracesTheSameOutline(e, 80.0);
        const Path p = pathFromShape(e);
        REQUIRE(p.subpaths.size() == 1);
        CHECK(p.subpaths[0].closed == (mode != EllipseShape::ArcMode::Open));
    }
    // Pie adds the centre as a sharp node; Chord does not.
    const Path pie = pathFromShape(EllipseShape{{80, 80}, 0.2, 2.4, EllipseShape::ArcMode::Pie});
    const Path chord = pathFromShape(EllipseShape{{80, 80}, 0.2, 2.4, EllipseShape::ArcMode::Chord});
    CHECK(nodeCount(pie) == nodeCount(chord) + 1);
    CHECK(pie.subpaths[0].nodes.back().anchor.x == doctest::Approx(0.0));
    CHECK(pie.subpaths[0].nodes.back().anchor.y == doctest::Approx(0.0));
}

TEST_CASE("a converted rect traces the same outline under every corner style") {
    for (const auto style : {CornerStyle::None, CornerStyle::Round, CornerStyle::Bevel,
                             CornerStyle::Inverse}) {
        CAPTURE(static_cast<int>(style));
        checkTracesTheSameOutline(RectShape::uniform({200.0, 120.0}, 24.0, style), 24.0);
    }
    // A sharp rect is exactly four corner nodes.
    const Path sharp = pathFromShape(RectShape::uniform({200, 120}, 0.0, CornerStyle::Round));
    CHECK(nodeCount(sharp) == 4);

    // Mixed per-corner radii and styles (the shape designer's real output).
    RectShape mixed;
    mixed.size = {180.0, 90.0};
    mixed.cornerRadius = {30.0, 0.0, 12.0, 45.0};
    mixed.cornerStyle = {CornerStyle::Round, CornerStyle::None, CornerStyle::Bevel,
                         CornerStyle::Inverse};
    checkTracesTheSameOutline(mixed, 45.0);
}

TEST_CASE("an oversized corner radius clamps to half the shorter edge, exactly as flatten does") {
    // Radius far larger than the rect: the inset saturates and the outline must still agree.
    // The clamp caps the inset at half the short edge (20), so the fillet radius is 20 too.
    checkTracesTheSameOutline(RectShape::uniform({100.0, 40.0}, 500.0, CornerStyle::Round), 20.0);
    checkTracesTheSameOutline(RectShape::uniform({100.0, 40.0}, 500.0, CornerStyle::Inverse), 20.0);
}

TEST_CASE("converted polygons and stars trace their shapes, rounded or sharp") {
    for (const int sides : {3, 5, 8}) {
        CAPTURE(sides);
        checkTracesTheSameOutline(PolygonShape{sides, 90.0, 0.0, CornerStyle::Round}, 0.0);
        checkTracesTheSameOutline(PolygonShape{sides, 90.0, 14.0, CornerStyle::Round}, 14.0);
        checkTracesTheSameOutline(PolygonShape{sides, 90.0, 14.0, CornerStyle::Bevel}, 14.0);
    }
    checkTracesTheSameOutline(StarShape{5, 100.0, 45.0, 0.0, 0.0}, 0.0); // sharp tips
    checkTracesTheSameOutline(StarShape{5, 100.0, 45.0, 8.0, 6.0}, 8.0); // rounded tips + valleys
    checkTracesTheSameOutline(StarShape{7, 120.0, 30.0, 3.0, 12.0}, 12.0);
}

TEST_CASE("a converted line keeps its bend exactly -- a quadratic elevates to a cubic") {
    const LineShape straight{{0.0, 0.0}, {100.0, 40.0}};
    const Path sp = pathFromShape(straight);
    REQUIRE(sp.subpaths.size() == 1);
    CHECK_FALSE(sp.subpaths[0].closed);
    CHECK(sp.subpaths[0].nodes.size() == 2);
    checkTracesTheSameOutline(straight, 0.0);

    LineShape bent{{0.0, 0.0}, {100.0, 0.0}};
    bent.bend = {0.0, 30.0};
    const Path bp = pathFromShape(bent);
    REQUIRE(bp.subpaths.size() == 1);
    CHECK(bp.subpaths[0].nodes.size() == 2); // one cubic, exact -- no subdivision needed
    // The curve passes through midpoint + bend at t = 0.5. Evaluate the cubic there.
    const auto& n0 = bp.subpaths[0].nodes[0];
    const auto& n1 = bp.subpaths[0].nodes[1];
    const Vec2 at = n0.anchor * 0.125 + n0.outHandle * 0.375 + n1.inHandle * 0.375 + n1.anchor * 0.125;
    CHECK(at.x == doctest::Approx(50.0));
    CHECK(at.y == doctest::Approx(30.0)); // exactly the bent midpoint
    checkTracesTheSameOutline(bent, 0.0); // a quadratic elevates EXACTLY: no arc error at all
}

TEST_CASE("degenerate shapes convert to nothing rather than to garbage") {
    CHECK(pathFromShape(RectShape::uniform({0.0, 50.0}, 4.0)).subpaths.empty());
    CHECK(pathFromShape(EllipseShape{{0.0, 10.0}}).subpaths.empty());
}

TEST_CASE("pathFromGeometry passes an existing path through untouched") {
    Path p;
    core::vec::SubPath sp;
    sp.closed = true;
    sp.nodes.push_back({{0, 0}, {-5, 0}, {5, 0}, core::vec::Node::Type::Smooth});
    sp.nodes.push_back({{10, 10}, {5, 10}, {15, 10}, core::vec::Node::Type::Symmetric});
    p.subpaths.push_back(sp);
    p.fillRule = core::vec::FillRule::EvenOdd;
    CHECK(core::vec::pathFromGeometry(Geometry{p}) == p); // same nodes, same rule, same handles
}
