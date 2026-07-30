#include "core/vector/boolean.hpp"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <utility>
#include <variant>
#include <vector>

#include "core/vector/flatten.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/hit.hpp"
#include "core/vector/object.hpp"
#include "core/vector/to_path.hpp"
#include "io/mosaic/docjson.hpp"

using namespace mosaic::core;
using mosaic::common::Affine2D;
using mosaic::common::Vec2;
using doctest::Approx;

namespace {

// ---- input builders --------------------------------------------------------------------------
// The kernel's input unit is a flattened contour, so the tests speak contours directly: that keeps
// each case about the BOOLEAN and not about how a rect happens to flatten.

vec::Contour ring(std::vector<Vec2> pts, bool closed = true) {
    vec::Contour c;
    c.points = std::move(pts);
    c.closed = closed;
    return c;
}

vec::Contour reversedRing(const vec::Contour& c) {
    vec::Contour out = c;
    for (std::size_t i = 0; i < out.points.size(); ++i)
        out.points[i] = c.points[c.points.size() - 1 - i];
    return out;
}

// An axis-aligned square wound POSITIVE (visually clockwise in this y-down space) -- the same
// orientation the kernel is contracted to emit for an outer, so a "square in, square out" case
// should come back with the same sign and not merely the same outline.
vec::Contour squareRing(double x, double y, double s) {
    return ring({{x, y}, {x + s, y}, {x + s, y + s}, {x, y + s}});
}

vec::Contours squareOp(double x, double y, double s) { return vec::Contours{squareRing(x, y, s)}; }

// A regular n-gon, circumradius r, first vertex on the +x axis, wound POSITIVE like squareRing.
//
// This is the operand family that really exercises the ARRANGEMENT. Two axis-aligned squares only
// ever meet at lattice-exact points, and two n-gons with n a multiple of six, radius 5 and centres
// 5 apart meet exactly on the shared vertices (2.5, +-4.330127) -- neither case ever needs an
// edge-edge intersection to be computed at all. Every other n does: the two boundaries cross at a
// GENERIC point that has to be computed, snap-rounded onto the lattice and re-stitched, and the
// kernel used to drop exactly those, so the sweep below is deliberately half multiples of six and
// half not.
vec::Contour ngonRing(int n, double r, double cx, double cy) {
    constexpr double kTwoPi = 6.28318530717958647692;
    std::vector<Vec2> pts;
    pts.reserve(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) {
        const double a = kTwoPi * static_cast<double>(k) / static_cast<double>(n);
        pts.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    return ring(std::move(pts));
}

vec::Contours ngonOp(int n, double r, double cx, double cy) {
    return vec::Contours{ngonRing(n, r, cx, cy)};
}

// Area tolerance for the polygon sweeps, as doctest's RELATIVE epsilon (the test is
// |a - b| < eps * (1 + max(|a|, |b|))): about 0.11 of a unit at the smallest expected value below
// and 0.63 at the largest. The kernel's own error is the 1/1024 lattice -- every vertex moves by
// at most 1/2048, worth a couple of hundredths of a unit of area on a 30-unit perimeter -- while
// the defect these cases pin was worth twenty to seventy. There is that much daylight between
// "snap-rounded" and "wrong", and none of these numbers is sensitive to where in it the bar sits.
constexpr double kNgonAreaTol = 0.005;

// ---- measurements ----------------------------------------------------------------------------
// Signed shoelace area. Positive == visually clockwise in y-down == an OUTER under the kernel's
// normalization; a hole comes back negative. Summing the signed areas of every ring therefore
// gives the FILLED area of a NonZero result directly -- which is how these tests check a boolean
// answered with the right region without re-implementing a rasterizer to do it.
double signedArea(const vec::Contour& c) {
    double a = 0.0;
    const std::size_t n = c.points.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2& p0 = c.points[i];
        const Vec2& p1 = c.points[(i + 1) % n];
        a += p0.x * p1.y - p1.x * p0.y;
    }
    return a * 0.5;
}

double filledArea(const vec::Contours& cs) {
    double a = 0.0;
    for (const vec::Contour& c : cs) a += signedArea(c);
    return a;
}

bool inside(const vec::Contours& cs, Vec2 p) {
    return vec::contains(cs, p, vec::FillRule::NonZero);
}

bool allClosed(const vec::Contours& cs) {
    for (const vec::Contour& c : cs)
        if (!c.closed || c.points.size() < 3) return false;
    return true;
}

bool sameContours(const vec::Contours& a, const vec::Contours& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].closed != b[i].closed || a[i].points.size() != b[i].points.size()) return false;
        // Exact, never Approx: determinism is a byte property, not a fuzzy one.
        for (std::size_t k = 0; k < a[i].points.size(); ++k)
            if (!(a[i].points[k] == b[i].points[k])) return false;
    }
    return true;
}

// The baked path read back as contours -- ANCHORS only, which is exactly right for a polyline bake
// and lets every measurement above (signedArea, filledArea, inside, allClosed) apply to it
// unchanged, without a second flatten and without re-deriving what "the region" means.
vec::Contours anchorContours(const vec::Path& p) {
    vec::Contours cs;
    for (const vec::SubPath& sp : p.subpaths) {
        vec::Contour c;
        c.closed = sp.closed;
        for (const vec::Node& n : sp.nodes) c.points.push_back(n.anchor);
        cs.push_back(std::move(c));
    }
    return cs;
}

// Two anchors at one point: the defect an editor renders as two draggable nodes stacked on each
// other. The WRAP pair counts -- a closed subpath's last node neighbours its first.
bool anyDuplicateAnchor(const vec::Path& p) {
    for (const vec::SubPath& sp : p.subpaths) {
        const std::size_t n = sp.nodes.size();
        for (std::size_t i = 0; i < n; ++i)
            if (sp.nodes[i].anchor == sp.nodes[(i + 1) % n].anchor) return true;
    }
    return false;
}

// Every node of a baked path is a polyline corner: handles ON the anchor, type Corner.
bool allPolylineCorners(const vec::Path& p) {
    for (const vec::SubPath& sp : p.subpaths)
        for (const vec::Node& n : sp.nodes) {
            // Exact, never Approx: a handle either sits on its anchor or it does not.
            if (!(n.inHandle == n.anchor) || !(n.outHandle == n.anchor)) return false;
            if (n.type != vec::Node::Type::Corner) return false;
        }
    return true;
}

// ---- object builders -------------------------------------------------------------------------

vec::Object rectObject(double x, double y, double s) {
    const vec::Contour r = squareRing(x, y, s);  // named: a range-for over a temporary's member
    vec::SubPath sp;                             // would dangle before C++23
    sp.closed = true;
    for (const Vec2& p : r.points) sp.nodes.push_back(vec::Node{p, p, p});
    vec::Object o;
    o.geometry = vec::Path{{sp}, vec::FillRule::NonZero};
    o.fill = vec::SolidPaint{{1, 0, 0, 1}};
    return o;
}

// A circle centred on the local origin -- a CURVED operand, so the baking tolerance is observable
// in the node count (a rect bakes to four nodes at any tolerance).
vec::Object circleObject(double r) {
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::EllipseShape{{r, r}}};
    o.fill = vec::SolidPaint{{0, 0, 1, 1}};
    return o;
}

vec::Object compoundObject(vec::BoolOp op, std::vector<vec::Object> children) {
    vec::Object o;
    vec::BooleanCompound c;
    c.op = op;
    c.children = std::move(children);
    o.geometry = std::move(c);
    o.fill = vec::SolidPaint{{0, 1, 0, 1}};
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The four ops on the plain overlapping case
// ---------------------------------------------------------------------------------------------

TEST_CASE("boolean union of two overlapping squares is one ring of the merged area") {
    const vec::Contours r =
        vec::booleanContours(vec::BoolOp::Union, squareOp(0, 0, 10), squareOp(5, 5, 10));
    REQUIRE(r.size() == 1);
    CHECK(allClosed(r));
    // 100 + 100 - 25 overlap.
    CHECK(filledArea(r) == Approx(175.0));
    CHECK(inside(r, {1, 1}));
    CHECK(inside(r, {14, 14}));
    CHECK(inside(r, {7, 7}));
    CHECK_FALSE(inside(r, {12, 1}));  // the notch the L leaves open
}

TEST_CASE("boolean intersect of two overlapping squares is exactly the overlap rectangle") {
    const vec::Contours r =
        vec::booleanContours(vec::BoolOp::Intersect, squareOp(0, 0, 10), squareOp(5, 5, 10));
    REQUIRE(r.size() == 1);
    CHECK(filledArea(r) == Approx(25.0));
    CHECK(inside(r, {7, 7}));
    CHECK_FALSE(inside(r, {1, 1}));
    CHECK_FALSE(inside(r, {14, 14}));
}

TEST_CASE("boolean subtract removes the second operand and keeps the first's remainder") {
    const vec::Contours r =
        vec::booleanContours(vec::BoolOp::Subtract, squareOp(0, 0, 10), squareOp(5, 5, 10));
    REQUIRE(r.size() == 1);
    CHECK(filledArea(r) == Approx(75.0));
    CHECK(inside(r, {1, 1}));
    CHECK_FALSE(inside(r, {7, 7}));
    CHECK_FALSE(inside(r, {14, 14}));
}

TEST_CASE("boolean exclude of two overlapping squares is the symmetric difference") {
    const vec::Contours r =
        vec::booleanContours(vec::BoolOp::Exclude, squareOp(0, 0, 10), squareOp(5, 5, 10));
    // The two L pieces meet at the overlap rect's two opposite corners -- a PINCH, which the
    // linker is contracted to resolve into separate simple rings rather than one self-touching one.
    CHECK(r.size() == 2);
    CHECK(allClosed(r));
    CHECK(filledArea(r) == Approx(150.0));
    CHECK(inside(r, {1, 1}));
    CHECK(inside(r, {14, 14}));
    CHECK_FALSE(inside(r, {7, 7}));
}

// ---------------------------------------------------------------------------------------------
// Degeneracies -- the whole reason this kernel snap-rounds before it sweeps
// ---------------------------------------------------------------------------------------------

TEST_CASE("boolean union of two squares sharing a whole edge is one seamless rectangle") {
    // The coincident-edge case. Both operands own the segment x == 10, in OPPOSITE directions;
    // getting this wrong leaves a hairline seam or an internal edge in the result.
    const vec::Contours r =
        vec::booleanContours(vec::BoolOp::Union, squareOp(0, 0, 10), squareOp(10, 0, 10));
    REQUIRE(r.size() == 1);
    CHECK(r[0].points.size() == 4);  // the shared edge is gone entirely, not merely invisible
    CHECK(filledArea(r) == Approx(200.0));
    CHECK(inside(r, {5, 5}));
    CHECK(inside(r, {15, 5}));
    CHECK(inside(r, {10, 5}));
}

TEST_CASE("boolean of two identical squares: union and intersect keep it, subtract and exclude "
          "empty it") {
    const vec::Contours a = squareOp(0, 0, 10);
    const vec::Contours b = squareOp(0, 0, 10);
    CHECK(filledArea(vec::booleanContours(vec::BoolOp::Union, a, b)) == Approx(100.0));
    CHECK(filledArea(vec::booleanContours(vec::BoolOp::Intersect, a, b)) == Approx(100.0));
    // A zero-area result is an EMPTY Contours, not a degenerate ring and not a crash.
    CHECK(vec::booleanContours(vec::BoolOp::Subtract, a, b).empty());
    CHECK(vec::booleanContours(vec::BoolOp::Exclude, a, b).empty());
}

TEST_CASE("boolean handles vertices on an edge and a partly shared edge") {
    // Two of the triangle's vertices sit in the INTERIOR of the square's lower edge, and the
    // segment between them is shared with it. No crossing anywhere -- just vertex-on-edge and a
    // partial collinear overlap, which is what a tolerance-based kernel classifies at random.
    const vec::Contours sq = squareOp(0, 0, 10);
    const vec::Contours tri{ring({{2, 10}, {8, 10}, {5, 16}})};
    const vec::Contours r = vec::booleanContours(vec::BoolOp::Union, sq, tri);
    REQUIRE(r.size() == 1);
    CHECK(filledArea(r) == Approx(100.0 + 18.0));  // triangle: base 6, height 6
    CHECK(inside(r, {5, 12}));
    CHECK(inside(r, {5, 5}));
    CHECK_FALSE(inside(r, {1, 12}));
}

TEST_CASE("boolean handles operands that only touch at a single corner") {
    const vec::Contours r =
        vec::booleanContours(vec::BoolOp::Union, squareOp(0, 0, 10), squareOp(10, 10, 10));
    CHECK(filledArea(r) == Approx(200.0));
    CHECK(inside(r, {5, 5}));
    CHECK(inside(r, {15, 15}));
    CHECK_FALSE(inside(r, {15, 5}));
    CHECK(vec::booleanContours(vec::BoolOp::Intersect, squareOp(0, 0, 10), squareOp(10, 10, 10))
              .empty());
}

TEST_CASE("boolean of disjoint operands keeps them apart, or yields nothing") {
    const vec::Contours u =
        vec::booleanContours(vec::BoolOp::Union, squareOp(0, 0, 10), squareOp(50, 50, 10));
    CHECK(u.size() == 2);
    CHECK(filledArea(u) == Approx(200.0));
    CHECK(vec::booleanContours(vec::BoolOp::Intersect, squareOp(0, 0, 10), squareOp(50, 50, 10))
              .empty());
    // Subtracting something that is nowhere near leaves the host untouched.
    CHECK(filledArea(vec::booleanContours(vec::BoolOp::Subtract, squareOp(0, 0, 10),
                                          squareOp(50, 50, 10))) == Approx(100.0));
}

TEST_CASE("boolean subtracting a square strictly inside another leaves an oriented hole") {
    const vec::Contours r =
        vec::booleanContours(vec::BoolOp::Subtract, squareOp(0, 0, 30), squareOp(10, 10, 10));
    REQUIRE(r.size() == 2);
    CHECK(allClosed(r));
    // Normalization is the assertion: exactly one positive outer and one negative hole, which is
    // what makes the result correct under NonZero with no fill-rule negotiation downstream.
    int outers = 0;
    int holes = 0;
    for (const vec::Contour& c : r) (signedArea(c) > 0.0 ? outers : holes) += 1;
    CHECK(outers == 1);
    CHECK(holes == 1);
    CHECK(filledArea(r) == Approx(800.0));
    CHECK(inside(r, {2, 2}));
    CHECK_FALSE(inside(r, {15, 15}));
}

TEST_CASE("boolean of an operand entirely inside another gives the containing or contained one") {
    const vec::Contours big = squareOp(0, 0, 30);
    const vec::Contours small = squareOp(10, 10, 10);
    CHECK(filledArea(vec::booleanContours(vec::BoolOp::Union, big, small)) == Approx(900.0));
    CHECK(filledArea(vec::booleanContours(vec::BoolOp::Intersect, big, small)) == Approx(100.0));
    // Host inside subtrahend: nothing survives.
    CHECK(vec::booleanContours(vec::BoolOp::Subtract, small, big).empty());
}

TEST_CASE("boolean normalizes nested holes to alternating orientation by containment depth") {
    // outer 30x30, a hole cut out of it, and an island inside the hole -- three depths, which is
    // where a kernel that only knows "outer vs hole" starts filling the island as a hole.
    vec::Contours donutIsland;
    donutIsland.push_back(squareRing(0, 0, 30));
    donutIsland.push_back(reversedRing(squareRing(5, 5, 20)));  // wound negative: a hole
    donutIsland.push_back(squareRing(10, 10, 10));
    const vec::Contours r = vec::normalizedContours(donutIsland, vec::FillRule::NonZero);
    REQUIRE(r.size() == 3);
    CHECK(filledArea(r) == Approx(900.0 - 400.0 + 100.0));
    CHECK(inside(r, {2, 2}));
    CHECK_FALSE(inside(r, {7, 7}));
    CHECK(inside(r, {15, 15}));
}

TEST_CASE("boolean resolves a self-intersecting operand under the NonZero rule") {
    // The bowtie: one contour crossing itself. Under NonZero both lobes are filled (windings +1
    // and -1), and they meet at the crossing -- another pinch, resolved into two simple rings.
    const vec::Contours bowtie{ring({{0, 0}, {10, 10}, {10, 0}, {0, 10}})};
    const vec::Contours r = vec::normalizedContours(bowtie, vec::FillRule::NonZero);
    CHECK(r.size() == 2);
    CHECK(allClosed(r));
    CHECK(filledArea(r) == Approx(50.0));  // two triangles of 25, meeting at (5,5)
    CHECK(inside(r, {1, 5}));
    CHECK(inside(r, {9, 5}));
    CHECK_FALSE(inside(r, {5, 1}));
    CHECK_FALSE(inside(r, {5, 9}));
}

TEST_CASE("boolean reads an EvenOdd operand as EvenOdd") {
    // Two concentric squares wound the SAME way: NonZero fills the middle solid, EvenOdd leaves a
    // hole. Same points, different rule, different region -- so the rule really is being honoured.
    vec::Contours nested;
    nested.push_back(squareRing(0, 0, 30));
    nested.push_back(squareRing(10, 10, 10));
    CHECK(filledArea(vec::normalizedContours(nested, vec::FillRule::NonZero)) == Approx(900.0));
    const vec::Contours odd = vec::normalizedContours(nested, vec::FillRule::EvenOdd);
    CHECK(filledArea(odd) == Approx(800.0));
    CHECK_FALSE(inside(odd, {15, 15}));
}

TEST_CASE("boolean treats an open contour as implicitly closed for the area test") {
    // The documented rule (boolean.hpp): open contours close for the area test rather than being
    // dropped, matching raster.cpp's scanline and hit.cpp's contains().
    const vec::Contours openTri{ring({{0, 0}, {10, 0}, {0, 10}}, /*closed=*/false)};
    const vec::Contours closedTri{ring({{0, 0}, {10, 0}, {0, 10}}, /*closed=*/true)};
    const vec::Contours a = vec::booleanContours(vec::BoolOp::Union, openTri, squareOp(20, 20, 5));
    const vec::Contours b = vec::booleanContours(vec::BoolOp::Union, closedTri, squareOp(20, 20, 5));
    CHECK(sameContours(a, b));
    CHECK(filledArea(a) == Approx(50.0 + 25.0));
    // ...and it comes back CLOSED, whatever went in.
    CHECK(allClosed(a));
}

TEST_CASE("boolean with empty and degenerate operands yields empty rather than misbehaving") {
    const vec::Contours none;
    CHECK(vec::booleanContours(vec::BoolOp::Union, none, none).empty());
    CHECK(vec::booleanContours(vec::BoolOp::Intersect, squareOp(0, 0, 10), none).empty());
    CHECK(filledArea(vec::booleanContours(vec::BoolOp::Subtract, squareOp(0, 0, 10), none)) ==
          Approx(100.0));
    CHECK(vec::booleanContours(vec::BoolOp::Union, std::vector<vec::Contours>{}).empty());
    // A contour with no area contributes nothing and takes nothing down with it.
    const vec::Contours sliver{ring({{0, 0}, {5, 0}})};
    CHECK(vec::booleanContours(vec::BoolOp::Union, sliver, sliver).empty());
    CHECK(filledArea(vec::booleanContours(vec::BoolOp::Union, sliver, squareOp(0, 0, 10))) ==
          Approx(100.0));
}

// ---------------------------------------------------------------------------------------------
// Generic edge-edge crossings -- the arrangement, not the classification
// ---------------------------------------------------------------------------------------------
// The other half of this story is already above: the four square cases pin the crossings that land
// EXACTLY on the lattice, and n = 6/12/24/48 in the sweep below pin the ones that land on a shared
// vertex. Those two families are what the kernel used to get right, so they are also the guard that
// honouring the rounded crossings did not move anything that was not broken.

TEST_CASE("boolean resolves a generic edge-edge crossing: the n-gon sweep") {
    // THE REGRESSION. Two radius-5 n-gons with centres 5 apart genuinely overlap, so for every n
    // the union is ONE region and the intersection is ONE region. The kernel used to answer this
    // correctly only for n a multiple of six -- the case where both operands already carry a vertex
    // on each true crossing point, so no intersection is ever computed. Every other n needs a
    // computed crossing, and those came back as two rings with area missing: n = 8 unioned to 58.20
    // (LESS than one operand) and intersected to nothing at all; n = 64 unioned to 72.85.
    //
    // The expected areas are the exact convex-clip areas of the two POLYGONS, not of the two discs:
    // union == 2 * oneArea - intersectArea, both rising toward the discs' 126.3704 / 30.7092 as the
    // polygon approximation tightens. Comparing a hexagon to the disc figure would be asserting
    // that a six-sided approximation is a circle.
    struct Case {
        int n;
        double oneArea;    // 0.5 * n * r^2 * sin(2*pi/n)
        double unionArea;
        double intersectArea;
    };
    const std::vector<Case> cases{
        {6, 64.95191, 108.25318, 21.65064},   {8, 70.71068, 115.53301, 25.88835},
        {12, 75.00000, 121.65064, 28.34936},  {16, 76.53669, 123.72735, 29.34602},
        {24, 77.64571, 125.17825, 30.11317},  {32, 78.03613, 125.69364, 30.37862},
        {48, 78.31572, 126.07159, 30.55984},  {64, 78.41371, 126.20280, 30.62462},
        {128, 78.50828, 126.32827, 30.68829},
    };
    for (const Case& c : cases) {
        CAPTURE(c.n);
        const vec::Contours a = ngonOp(c.n, 5.0, 0.0, 0.0);
        const vec::Contours b = ngonOp(c.n, 5.0, 5.0, 0.0);

        const vec::Contours u = vec::booleanContours(vec::BoolOp::Union, a, b);
        // The ring COUNT is exact: the failure signature was two rings, never area drift.
        CHECK(u.size() == 1);
        CHECK(allClosed(u));
        CHECK(filledArea(u) == Approx(c.unionArea).epsilon(kNgonAreaTol));
        CHECK(filledArea(u) > c.oneArea);  // a union can never be smaller than either operand
        CHECK(inside(u, {-4.0, 0.0}));     // in A only
        CHECK(inside(u, {9.0, 0.0}));      // in B only
        CHECK(inside(u, {2.5, 0.0}));      // in both
        CHECK_FALSE(inside(u, {2.5, 6.0}));  // in neither: 6.5 from each centre

        const vec::Contours x = vec::booleanContours(vec::BoolOp::Intersect, a, b);
        CHECK(x.size() == 1);
        CHECK(allClosed(x));
        CHECK(filledArea(x) == Approx(c.intersectArea).epsilon(kNgonAreaTol));
        CHECK(inside(x, {2.5, 0.0}));
        CHECK_FALSE(inside(x, {-4.0, 0.0}));
        CHECK_FALSE(inside(x, {9.0, 0.0}));
    }
    // ...and the tail of the sweep really is converging on the two discs: 2*pi*r^2 minus the lens
    // 2r^2 acos(d/2r) - (d/2) sqrt(4r^2 - d^2) == 126.3704, lens == 30.7092.
    CHECK(std::abs(cases.back().unionArea - 126.3704) < 0.05);
    CHECK(std::abs(cases.back().intersectArea - 30.7092) < 0.03);
}

TEST_CASE("boolean crosses generically for subtract and exclude too") {
    // The fault was in the arrangement, upstream of the op predicate, so all four ops carried it.
    const vec::Contours a = ngonOp(16, 5.0, 0.0, 0.0);
    const vec::Contours b = ngonOp(16, 5.0, 5.0, 0.0);
    const double crescent = 76.53669 - 29.34602;  // oneArea - intersectArea, from the sweep's table

    const vec::Contours s = vec::booleanContours(vec::BoolOp::Subtract, a, b);
    CHECK(s.size() == 1);
    CHECK(allClosed(s));
    CHECK(filledArea(s) == Approx(crescent).epsilon(kNgonAreaTol));
    CHECK(inside(s, {-4.0, 0.0}));
    CHECK_FALSE(inside(s, {2.5, 0.0}));
    CHECK_FALSE(inside(s, {9.0, 0.0}));

    const vec::Contours x = vec::booleanContours(vec::BoolOp::Exclude, a, b);
    // The two crescents meet at the two crossing points -- pinches, which the linker is contracted
    // to resolve into separate simple rings (the same rule the two-squares Exclude case pins).
    CHECK(x.size() == 2);
    CHECK(allClosed(x));
    CHECK(filledArea(x) == Approx(2.0 * crescent).epsilon(kNgonAreaTol));
    CHECK(inside(x, {-4.0, 0.0}));
    CHECK(inside(x, {9.0, 0.0}));
    CHECK_FALSE(inside(x, {2.5, 0.0}));
}

TEST_CASE("boolean of two near-coincident operands is one region, not a cloud of slivers") {
    // Centres 0.001 apart -- ONE lattice unit at the kernel's 1/1024 resolution. Every crossing is
    // degenerate: the two boundaries have identical y sequences and x sequences a single lattice
    // step apart, so the rounded crossings land on existing vertices and the two chains have to be
    // stitched through them. This came back as SEVEN rings totalling 30.56 while computed crossings
    // were being dropped. The answer is one ring of essentially one operand's area: the offset
    // operand contributes a sliver 0.001 wide down the side, worth 0.01.
    const vec::Contours a = ngonOp(64, 5.0, 0.0, 0.0);
    const vec::Contours b = ngonOp(64, 5.0, 0.001, 0.0);
    const vec::Contours u = vec::booleanContours(vec::BoolOp::Union, a, b);
    CHECK(u.size() == 1);
    CHECK(allClosed(u));
    CHECK(filledArea(u) == Approx(78.42371).epsilon(kNgonAreaTol));  // 78.41371 + the sliver
    CHECK(inside(u, {0.0, 0.0}));
    CHECK(inside(u, {-4.0, 0.0}));
    CHECK_FALSE(inside(u, {6.0, 0.0}));
}

TEST_CASE("boolean of two disjoint curved operands keeps exactly two rings") {
    // The control for the sweep above: no crossings anywhere, so this case was already right and
    // has to stay right -- a fix that resolved crossings by merging everything in sight would show
    // up here as one ring.
    const vec::Contours a = ngonOp(64, 5.0, 0.0, 0.0);
    const vec::Contours b = ngonOp(64, 5.0, 20.0, 0.0);
    const vec::Contours u = vec::booleanContours(vec::BoolOp::Union, a, b);
    CHECK(u.size() == 2);
    CHECK(allClosed(u));
    CHECK(filledArea(u) == Approx(2.0 * 78.41371).epsilon(kNgonAreaTol));
    CHECK(inside(u, {0.0, 0.0}));
    CHECK(inside(u, {20.0, 0.0}));
    CHECK_FALSE(inside(u, {10.0, 0.0}));
    CHECK(vec::booleanContours(vec::BoolOp::Intersect, a, b).empty());
}

// ---------------------------------------------------------------------------------------------
// The n-ary fold, and determinism
// ---------------------------------------------------------------------------------------------

TEST_CASE("boolean n-ary fold treats the first operand as the host") {
    const std::vector<vec::Contours> ops{squareOp(0, 0, 30), squareOp(0, 0, 10),
                                         squareOp(20, 20, 10)};
    CHECK(filledArea(vec::booleanContours(vec::BoolOp::Subtract, ops)) == Approx(700.0));
    CHECK(filledArea(vec::booleanContours(vec::BoolOp::Union, ops)) == Approx(900.0));
    CHECK(vec::booleanContours(vec::BoolOp::Intersect, ops).empty());
    // Exclude over three operands is odd-coverage parity: the two small squares are covered twice
    // (by themselves and by the big one) so they drop out.
    CHECK(filledArea(vec::booleanContours(vec::BoolOp::Exclude, ops)) == Approx(700.0));
}

TEST_CASE("boolean output is deterministic: two runs are byte-identical") {
    const vec::Contours a = squareOp(0, 0, 13);
    const vec::Contours b = squareOp(7, 3, 11);
    const vec::Contours r1 = vec::booleanContours(vec::BoolOp::Exclude, a, b);
    const vec::Contours r2 = vec::booleanContours(vec::BoolOp::Exclude, a, b);
    CHECK(sameContours(r1, r2));
    CHECK_FALSE(r1.empty());
}

// ---------------------------------------------------------------------------------------------
// The model: BooleanCompound through the flatten seam
// ---------------------------------------------------------------------------------------------

TEST_CASE("flatten of a BooleanCompound resolves the op through the one seam") {
    const vec::Object o =
        compoundObject(vec::BoolOp::Subtract, {rectObject(0, 0, 30), rectObject(10, 10, 10)});
    const vec::Contours cs = vec::flatten(o.geometry);
    REQUIRE(cs.size() == 2);
    CHECK(filledArea(cs) == Approx(800.0));
    // Everything downstream of the seam therefore just works.
    CHECK(vec::fillRuleOf(o.geometry) == vec::FillRule::NonZero);
    const auto box = vec::contentBounds(o.geometry);
    REQUIRE(box.has_value());
    CHECK(box->x == Approx(0.0));
    CHECK(box->y == Approx(0.0));
    CHECK(box->w == Approx(30.0));
    CHECK(box->h == Approx(30.0));
    CHECK(vec::hitTest(o, {2, 2}));
    CHECK_FALSE(vec::hitTest(o, {15, 15}));
}

TEST_CASE("a compound nested inside a compound resolves innermost-first") {
    // (A u B) - C. The inner union is a live child, not a baked one.
    const vec::Object inner =
        compoundObject(vec::BoolOp::Union, {rectObject(0, 0, 20), rectObject(20, 0, 20)});
    const vec::Object outer = compoundObject(vec::BoolOp::Subtract, {inner, rectObject(10, 5, 10)});
    const vec::Contours cs = vec::flatten(outer.geometry);
    CHECK_FALSE(cs.empty());
    CHECK(filledArea(cs) == Approx(800.0 - 100.0));
    CHECK(inside(cs, {5, 2}));
    CHECK(inside(cs, {30, 10}));
    CHECK_FALSE(inside(cs, {15, 10}));
}

TEST_CASE("pathFromGeometry bakes a compound to a closed polyline path instead of throwing") {
    // The std::get site that a third Geometry alternative would otherwise turn into a
    // std::bad_variant_access. Baking is the CalloutShape precedent: computed outline -> polyline.
    const vec::Object o =
        compoundObject(vec::BoolOp::Union, {rectObject(0, 0, 10), rectObject(10, 0, 10)});
    const vec::Path p = vec::pathFromGeometry(o.geometry);
    REQUIRE(p.subpaths.size() == 1);
    CHECK(p.fillRule == vec::FillRule::NonZero);
    CHECK(p.subpaths[0].closed);
    CHECK(p.subpaths[0].nodes.size() == 4);
    for (const vec::Node& n : p.subpaths[0].nodes) {
        CHECK(n.inHandle == n.anchor);  // handles == anchors: a polyline, exactly
        CHECK(n.outHandle == n.anchor);
    }
    // The bake draws the same region it came from.
    CHECK(filledArea(vec::flatten(vec::Geometry{p})) == Approx(200.0));
}

TEST_CASE("BooleanCompound equality compares the op and the children") {
    // The hand-written operator== (geometry.hpp cannot default it: Object is incomplete there).
    // Several call sites compare whole objects to suppress no-op commands, so this has to work.
    const vec::Object a =
        compoundObject(vec::BoolOp::Union, {rectObject(0, 0, 10), rectObject(5, 5, 10)});
    const vec::Object same =
        compoundObject(vec::BoolOp::Union, {rectObject(0, 0, 10), rectObject(5, 5, 10)});
    const vec::Object otherOp =
        compoundObject(vec::BoolOp::Intersect, {rectObject(0, 0, 10), rectObject(5, 5, 10)});
    const vec::Object otherKids =
        compoundObject(vec::BoolOp::Union, {rectObject(0, 0, 10), rectObject(6, 5, 10)});
    CHECK(a == same);
    CHECK(a != otherOp);
    CHECK(a != otherKids);
    CHECK_FALSE(a == rectObject(0, 0, 10));  // across variant alternatives too
}

// ---------------------------------------------------------------------------------------------
// makeLiveBooleanObject -- the fold, kept live for a future non-destructive mode
// ---------------------------------------------------------------------------------------------

TEST_CASE("makeLiveBooleanObject rebases every child into the host layer's local frame") {
    const vec::Object a = rectObject(0, 0, 10);        // local, on a layer at (100, 100)
    const vec::Object b = rectObject(0, 0, 10);        // local, on a layer at (105, 105)
    const Affine2D hostWorld = Affine2D::translation(100, 100);
    const std::vector<std::pair<vec::Object, Affine2D>> ops{
        {a, hostWorld}, {b, Affine2D::translation(105, 105)}};
    const auto made = vec::makeLiveBooleanObject(vec::BoolOp::Union, ops, hostWorld);
    REQUIRE(made.has_value());
    const auto* compound = std::get_if<vec::BooleanCompound>(&made->geometry);
    REQUIRE(compound != nullptr);
    REQUIRE(compound->children.size() == 2);
    // The host's own child is unmoved; the other lands 5,5 away in the host's frame.
    CHECK(filledArea(vec::flatten(compound->children[0].geometry)) == Approx(100.0));
    const auto box = vec::contentBounds(compound->children[1].geometry);
    REQUIRE(box.has_value());
    CHECK(box->x == Approx(5.0));
    CHECK(box->y == Approx(5.0));
    // ...so the resolved union is the two overlapping squares, in host-local coordinates.
    CHECK(filledArea(vec::flatten(made->geometry)) == Approx(175.0));
    // The host's appearance wins.
    CHECK(made->fill == a.fill);
}

// ---------------------------------------------------------------------------------------------
// makeBooleanObject -- what Layer > Combine Paths COMMITS: a baked, editable path
// ---------------------------------------------------------------------------------------------

TEST_CASE("makeBooleanObject commits a baked Path, never a live compound") {
    // The user-visible bug this pins: a committed compound is a layer NO tool binds -- the Pen
    // wants std::holds_alternative<vec::Path> (ui::penToolBinds) and the Shape tool wants a
    // ParametricShape, so a third alternative is selectable by neither and badges wrong.
    const vec::Object a = rectObject(0, 0, 10);
    const Affine2D hostWorld = Affine2D::translation(100, 100);
    const std::vector<std::pair<vec::Object, Affine2D>> ops{
        {a, hostWorld}, {rectObject(0, 0, 10), Affine2D::translation(105, 105)}};
    const auto made = vec::makeBooleanObject(vec::BoolOp::Union, ops, hostWorld);
    REQUIRE(made.has_value());
    // penToolBinds' predicate, spelled out here so the geometry side owns the guarantee.
    REQUIRE(std::holds_alternative<vec::Path>(made->geometry));
    CHECK_FALSE(std::holds_alternative<vec::BooleanCompound>(made->geometry));
    const vec::Path& p = std::get<vec::Path>(made->geometry);
    // The rule the normalization actually produced, and the one fillRuleOf therefore reports.
    CHECK(p.fillRule == vec::FillRule::NonZero);
    CHECK(vec::fillRuleOf(made->geometry) == vec::FillRule::NonZero);
    CHECK(allPolylineCorners(p));
    // Same region as the live fold drew, in host-local coordinates; host's appearance preserved.
    CHECK(filledArea(vec::flatten(made->geometry)) == Approx(175.0));
    CHECK(made->fill == a.fill);
    CHECK(made->stroke.enabled == a.stroke.enabled);
    CHECK(made->paintOrder == a.paintOrder);
}

TEST_CASE("a baked subtract comes back as two subpaths of opposite orientation") {
    const Affine2D id = Affine2D::identity();
    const std::vector<std::pair<vec::Object, Affine2D>> ops{{rectObject(0, 0, 30), id},
                                                            {rectObject(10, 10, 10), id}};
    const auto made = vec::makeBooleanObject(vec::BoolOp::Subtract, ops, id);
    REQUIRE(made.has_value());
    const auto* p = std::get_if<vec::Path>(&made->geometry);
    REQUIRE(p != nullptr);
    REQUIRE(p->subpaths.size() == 2);
    const vec::Contours cs = anchorContours(*p);
    // The hole keeps the negative winding the NonZero normalization gave it -- which is the only
    // reason a NonZero path with a hole in it is a hole and not a solid square.
    int outers = 0;
    int holes = 0;
    for (const vec::Contour& c : cs) (signedArea(c) > 0.0 ? outers : holes) += 1;
    CHECK(outers == 1);
    CHECK(holes == 1);
    CHECK(filledArea(cs) == Approx(800.0));
    // ...and it reads that way through the ordinary Path pipeline too, not just as anchors.
    CHECK(vec::hitTest(*made, {2, 2}));
    CHECK_FALSE(vec::hitTest(*made, {15, 15}));
}

TEST_CASE("a baked result is well formed for editing: closed, >= 3 nodes, no duplicate anchors") {
    const Affine2D id = Affine2D::identity();
    // Exclude is the pinch case (the two L pieces meet at the overlap's opposite corners), which is
    // where a linker that walked one self-touching ring would leave two anchors on one point.
    const std::vector<std::pair<vec::Object, Affine2D>> lobes{{rectObject(0, 0, 10), id},
                                                              {rectObject(5, 5, 10), id}};
    const auto excluded = vec::makeBooleanObject(vec::BoolOp::Exclude, lobes, id);
    REQUIRE(excluded.has_value());
    const auto* xp = std::get_if<vec::Path>(&excluded->geometry);
    REQUIRE(xp != nullptr);
    CHECK(xp->subpaths.size() == 2);
    CHECK(allClosed(anchorContours(*xp)));  // closed, and >= 3 points each
    CHECK_FALSE(anyDuplicateAnchor(*xp));
    CHECK(allPolylineCorners(*xp));

    // The curved case, where the flattener -- not the linker -- is what could emit a repeat.
    const std::vector<std::pair<vec::Object, Affine2D>> discs{
        {circleObject(5.0), id}, {circleObject(5.0), Affine2D::translation(4, 0)}};
    const auto united = vec::makeBooleanObject(vec::BoolOp::Union, discs, id);
    REQUIRE(united.has_value());
    const auto* up = std::get_if<vec::Path>(&united->geometry);
    REQUIRE(up != nullptr);
    CHECK(allClosed(anchorContours(*up)));
    CHECK_FALSE(anyDuplicateAnchor(*up));
    CHECK(allPolylineCorners(*up));
}

TEST_CASE("makeBooleanObject refuses fewer than two operands, a singular host and an empty region") {
    const std::vector<std::pair<vec::Object, Affine2D>> one{
        {rectObject(0, 0, 10), Affine2D::identity()}};
    CHECK_FALSE(
        vec::makeBooleanObject(vec::BoolOp::Union, one, Affine2D::identity()).has_value());
    const std::vector<std::pair<vec::Object, Affine2D>> two{
        {rectObject(0, 0, 10), Affine2D::identity()},
        {rectObject(5, 5, 10), Affine2D::identity()}};
    CHECK_FALSE(
        vec::makeBooleanObject(vec::BoolOp::Union, two, Affine2D::scaling(0.0, 0.0)).has_value());
    CHECK(vec::makeBooleanObject(vec::BoolOp::Union, two, Affine2D::identity()).has_value());
    // A region with no area: committing it would delete the consumed operand layers and leave the
    // host holding an invisible, unpickable path. Refused, so the caller can say so instead.
    const std::vector<std::pair<vec::Object, Affine2D>> same{
        {rectObject(0, 0, 10), Affine2D::identity()},
        {rectObject(0, 0, 10), Affine2D::identity()}};
    CHECK_FALSE(vec::makeBooleanObject(vec::BoolOp::Subtract, same, Affine2D::identity())
                    .has_value());
    CHECK_FALSE(
        vec::makeBooleanObject(vec::BoolOp::Exclude, same, Affine2D::identity()).has_value());
    // The live fold has no such refusal -- an empty compound is still a live, editable object.
    CHECK(vec::makeLiveBooleanObject(vec::BoolOp::Subtract, same, Affine2D::identity())
              .has_value());
}

TEST_CASE("the bake tolerance follows the shape's own size instead of the fixed default") {
    const Affine2D id = Affine2D::identity();
    const std::vector<std::pair<vec::Object, Affine2D>> discs{
        {circleObject(5.0), id}, {circleObject(5.0), Affine2D::translation(5, 0)}};
    const auto live = vec::makeLiveBooleanObject(vec::BoolOp::Union, discs, id);
    REQUIRE(live.has_value());
    const auto* compound = std::get_if<vec::BooleanCompound>(&live->geometry);
    REQUIRE(compound != nullptr);
    const vec::Path coarse = vec::bakedBooleanPath(*compound);  // the fixed 0.25 default
    const auto made = vec::makeBooleanObject(vec::BoolOp::Union, discs, id);
    REQUIRE(made.has_value());
    const auto* fine = std::get_if<vec::Path>(&made->geometry);
    REQUIRE(fine != nullptr);
    // Both bakes are ONE region: two discs that truly overlap have a single-region union, at any
    // tolerance. This pair of assertions is load-bearing history -- while the arrangement dropped
    // every rounded edge-edge crossing, this case came back as two rings totalling 76.9, which is
    // less area than ONE of the operands.
    INFO("coarse subpaths=" << coarse.subpaths.size() << " fine subpaths=" << fine->subpaths.size());
    REQUIRE(coarse.subpaths.size() == 1);
    REQUIRE(fine->subpaths.size() == 1);
    // A 10-unit circle flattened to 0.25 LOCAL units is a visibly faceted polygon; the size-relative
    // reading (1/4000 of the operands' extent) takes it to a curve. The exact counts are the
    // flattener's business -- what is pinned is that the commit is markedly finer than the default.
    CHECK(fine->subpaths[0].nodes.size() > coarse.subpaths[0].nodes.size() * 3);
    // ...and finer means CLOSER, not merely different: two discs of radius 5 with centres 5 apart
    // cover 2*pi*r^2 minus the lens 2r^2 acos(d/2r) - (d/2) sqrt(4r^2 - d^2) = 126.3703.
    const double exactUnion = 126.3703;
    const double fineErr = std::abs(filledArea(anchorContours(*fine)) - exactUnion);
    const double coarseErr = std::abs(filledArea(anchorContours(coarse)) - exactUnion);
    CHECK(fineErr < coarseErr);
    CHECK(filledArea(anchorContours(*fine)) == Approx(exactUnion).epsilon(0.01));
}

TEST_CASE("a baked Combine Paths result round-trips through docjson as an ordinary path") {
    namespace detail = mosaic::io::native::detail;
    const Affine2D id = Affine2D::identity();
    const std::vector<std::pair<vec::Object, Affine2D>> ops{{rectObject(0, 0, 30), id},
                                                            {rectObject(10, 10, 10), id}};
    const auto made = vec::makeBooleanObject(vec::BoolOp::Subtract, ops, id);
    REQUIRE(made.has_value());
    const nlohmann::json j = detail::vectorObjectToJson(*made);
    REQUIRE(j.contains("geometry"));
    const nlohmann::json& g = j["geometry"];
    CHECK(g["type"] == "path");
    CHECK(g["fill_rule"] == "nonzero");
    CHECK(g["subpaths"].size() == 2);
    // No side-car: a committed boolean is not a live compound, so nothing has to be forward-
    // compatible about it and an older build reads it as the plain path it is.
    CHECK_FALSE(g.contains("boolean"));
    const auto back = detail::vectorObjectFromJson(j);
    REQUIRE(back.has_value());
    REQUIRE(std::holds_alternative<vec::Path>(back->geometry));
    CHECK(*back == *made);
    CHECK(filledArea(vec::flatten(back->geometry)) == Approx(800.0));
}

TEST_CASE("rebasedObject keeps a nested compound live instead of baking it") {
    const vec::Object inner =
        compoundObject(vec::BoolOp::Union, {rectObject(0, 0, 10), rectObject(10, 0, 10)});
    const vec::Object moved = vec::rebasedObject(inner, Affine2D::translation(7, 3));
    const auto* compound = std::get_if<vec::BooleanCompound>(&moved.geometry);
    REQUIRE(compound != nullptr);  // still a compound, not a baked path
    CHECK(compound->children.size() == 2);
    const auto box = vec::contentBounds(moved.geometry);
    REQUIRE(box.has_value());
    CHECK(box->x == Approx(7.0));
    CHECK(box->y == Approx(3.0));
    CHECK(box->w == Approx(20.0));
}

// ---------------------------------------------------------------------------------------------
// .mosaic persistence -- live compound plus the baked fallback an older build reads
// ---------------------------------------------------------------------------------------------

TEST_CASE("docjson round-trips a boolean compound and leaves an old build a baked path") {
    namespace detail = mosaic::io::native::detail;
    const vec::Object o =
        compoundObject(vec::BoolOp::Subtract, {rectObject(0, 0, 30), rectObject(10, 10, 10)});
    const nlohmann::json j = detail::vectorObjectToJson(o);

    // The forward-compatibility contract: the wire type is "path" (so a build that predates
    // booleans reads a shape it understands rather than emptying the layer), with the live
    // compound alongside it under "boolean".
    REQUIRE(j.contains("geometry"));
    const nlohmann::json& g = j["geometry"];
    CHECK(g["type"] == "path");
    CHECK(g["fill_rule"] == "nonzero");
    REQUIRE(g.contains("subpaths"));
    CHECK(g["subpaths"].size() == 2);  // the baked outer + hole
    REQUIRE(g.contains("boolean"));
    CHECK(g["boolean"]["op"] == "subtract");
    CHECK(g["boolean"]["children"].size() == 2);

    // A current build prefers the compound and gets the live operands back.
    const auto back = detail::vectorObjectFromJson(j);
    REQUIRE(back.has_value());
    const auto* compound = std::get_if<vec::BooleanCompound>(&back->geometry);
    REQUIRE(compound != nullptr);
    CHECK(compound->op == vec::BoolOp::Subtract);
    CHECK(compound->children.size() == 2);
    CHECK(*back == o);
    CHECK(filledArea(vec::flatten(back->geometry)) == Approx(800.0));
}

TEST_CASE("docjson round-trips a compound nested inside a compound") {
    namespace detail = mosaic::io::native::detail;
    const vec::Object inner =
        compoundObject(vec::BoolOp::Union, {rectObject(0, 0, 20), rectObject(20, 0, 20)});
    const vec::Object outer = compoundObject(vec::BoolOp::Exclude, {inner, rectObject(10, 5, 10)});
    const auto back = detail::vectorObjectFromJson(detail::vectorObjectToJson(outer));
    REQUIRE(back.has_value());
    CHECK(*back == outer);
    const auto* compound = std::get_if<vec::BooleanCompound>(&back->geometry);
    REQUIRE(compound != nullptr);
    REQUIRE(compound->children.size() == 2);
    CHECK(std::holds_alternative<vec::BooleanCompound>(compound->children[0].geometry));
}

TEST_CASE("docjson rejects a malformed boolean side-car rather than quietly downgrading it") {
    namespace detail = mosaic::io::native::detail;
    const vec::Object o =
        compoundObject(vec::BoolOp::Union, {rectObject(0, 0, 10), rectObject(5, 5, 10)});
    nlohmann::json j = detail::vectorObjectToJson(o);
    SUBCASE("unknown op token") {
        j["geometry"]["boolean"]["op"] = "carve";
        CHECK_FALSE(detail::vectorObjectFromJson(j).has_value());
    }
    SUBCASE("children is not an array") {
        j["geometry"]["boolean"]["children"] = 7;
        CHECK_FALSE(detail::vectorObjectFromJson(j).has_value());
    }
    SUBCASE("a child is malformed") {
        j["geometry"]["boolean"]["children"][0].erase("stroke");
        CHECK_FALSE(detail::vectorObjectFromJson(j).has_value());
    }
    SUBCASE("no side-car at all: the baked path is read as an ordinary path") {
        j["geometry"].erase("boolean");
        const auto back = detail::vectorObjectFromJson(j);
        REQUIRE(back.has_value());
        const auto* path = std::get_if<vec::Path>(&back->geometry);
        REQUIRE(path != nullptr);
        CHECK(filledArea(vec::flatten(back->geometry)) == Approx(175.0));
    }
}
