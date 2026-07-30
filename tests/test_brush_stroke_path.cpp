#include <doctest/doctest.h>

#include "core/brush/stroke_path.hpp"

#include <cmath>
#include <vector>

// The stroke's PATH between samples (core/brush/stroke_path.hpp).
//
// The engine used to lay dabs along a STRAIGHT CHORD from one sample to the next, which made a 60 Hz
// mouse stroke a literal 60-gon (a tablet hid it by sampling at ~200 Hz whatever the display does).
// The path is now a centripetal Catmull-Rom curve THROUGH the samples.
//
// ⚠ Two properties are load-bearing and are pinned here, because either one could be lost silently:
//
//   1. It INTERPOLATES. The curve passes exactly through every sample the user made. It never moves
//      an input point. That is the line between this and the deferred rope/pulled-string stabilizer
//      (docs/tablet.md §7) -- interpolate, do not filter.
//   2. A STRAIGHT stroke stays STRAIGHT, and the flattener emits nothing for it, so the dab walk
//      falls back to the single chord and lays bit-identical dabs. That is what keeps the
//      `Uniform x Wash` and mouse goldens honest.

using mosaic::common::Vec2;
using mosaic::core::brush::catmullRom;
using mosaic::core::brush::flattenCatmullRom;
using mosaic::core::brush::kMaxFlattenSteps;

namespace {

constexpr double kTol = 0.05; // the engine's flattening tolerance

[[nodiscard]] double dist(Vec2 a, Vec2 b) { return (a - b).length(); }

// Perpendicular distance from c to the infinite line through p1 and p2.
[[nodiscard]] double toLine(Vec2 c, Vec2 p1, Vec2 p2) {
    const Vec2 d = p2 - p1;
    const double len = d.length();
    if (len < 1e-12)
        return dist(c, p1);
    const Vec2 v = c - p1;
    return std::abs(d.x * v.y - d.y * v.x) / len;
}

} // namespace

TEST_CASE("stroke path: the curve passes exactly THROUGH the samples -- it interpolates them") {
    const Vec2 p0{0.0, 0.0}, p1{10.0, 4.0}, p2{22.0, 1.0}, p3{30.0, 9.0};

    // u=0 IS p1 and u=1 IS p2. Not "close to": the whole design rests on the curve going through the
    // user's own points rather than near them, so this is an equality, not an Approx of
    // convenience.
    // (Not doctest::Approx(0).epsilon(0) -- its tolerance test is a STRICT <, so that form fails
    // even on an exact match. A ULP of slack is what "exactly" means for a double here.)
    CHECK(dist(catmullRom(p0, p1, p2, p3, 0.0), p1) < 1e-12);
    CHECK(dist(catmullRom(p0, p1, p2, p3, 1.0), p2) < 1e-12);
}

TEST_CASE("stroke path: a straight stroke stays straight, and flattens to nothing") {
    std::vector<Vec2> out;

    SUBCASE("evenly spaced collinear samples") {
        const Vec2 p0{0.0, 5.0}, p1{10.0, 5.0}, p2{20.0, 5.0}, p3{30.0, 5.0};
        for (const double u : {0.0, 0.25, 0.5, 0.75, 1.0})
            CHECK(catmullRom(p0, p1, p2, p3, u).y == doctest::Approx(5.0));
        flattenCatmullRom(p0, p1, p2, p3, kTol, out);
        CHECK(out.empty()); // -> the walk runs the old single-chord arithmetic, bit for bit
    }

    SUBCASE("UNEVENLY spaced collinear samples") {
        // ⚠ The case that caught a real bug. Uneven spacing on a straight line produces a curve that
        // lies exactly ON the line but is *parameterized* differently along it. A flattener that
        // measured "distance from p1 + chord*u" would read that harmless reparameterization as
        // curvature and chop a straight stroke into pieces -- moving dabs, and breaking the goldens,
        // for a line that never bent. The deviation must be measured PERPENDICULAR to the chord.
        const Vec2 p0{0.0, 5.0}, p1{1.0, 5.0}, p2{40.0, 5.0}, p3{43.0, 5.0};
        for (const double u : {0.0, 0.25, 0.5, 0.75, 1.0})
            CHECK(toLine(catmullRom(p0, p1, p2, p3, u), p1, p2) == doctest::Approx(0.0));
        flattenCatmullRom(p0, p1, p2, p3, kTol, out);
        CHECK(out.empty());
    }
}

TEST_CASE("stroke path: a coarsely sampled arc is painted as an ARC, not as a polygon") {
    // THE BUG, in one test. A mouse at 60 Hz delivers a fast curve as a handful of far-apart points.
    // Chords between them cut INSIDE the true curve by the sagitta -- and that shortfall IS the
    // staircase the user reported.
    const Vec2 centre{0.0, 0.0};
    constexpr double kR = 40.0;
    constexpr double kStepDeg = 30.0; // a coarse mouse sample -- 12 points around the circle
    const auto onCircle = [&](double deg) {
        const double r = deg * std::acos(-1.0) / 180.0;
        return Vec2{centre.x + kR * std::cos(r), centre.y + kR * std::sin(r)};
    };
    const Vec2 p0 = onCircle(0.0), p1 = onCircle(kStepDeg), p2 = onCircle(2 * kStepDeg),
               p3 = onCircle(3 * kStepDeg);

    // How far the straight chord falls inside the true arc: the sagitta, R(1 - cos(step/2)).
    const double half = kStepDeg * 0.5 * std::acos(-1.0) / 180.0;
    const double sagitta = kR * (1.0 - std::cos(half));
    REQUIRE(sagitta > 1.3); // ~1.36 px at these numbers -- plainly visible in a stroke

    double chordWorst = 0.0;
    double curveWorst = 0.0;
    for (int i = 1; i < 16; ++i) {
        const double u = static_cast<double>(i) / 16.0;
        // where the OLD walk would have put a dab: on the straight chord
        const Vec2 onChord = p1 + (p2 - p1) * u;
        chordWorst = std::max(chordWorst, std::abs(dist(onChord, centre) - kR));
        // ... and where the curve puts it
        const Vec2 onCurve = catmullRom(p0, p1, p2, p3, u);
        curveWorst = std::max(curveWorst, std::abs(dist(onCurve, centre) - kR));
    }
    CHECK(chordWorst == doctest::Approx(sagitta).epsilon(0.02)); // the chord IS off by the sagitta
    // The curve is not a circle -- no cubic is -- but it hugs one to well under a tenth of a pixel,
    // against the chord's 1.36. Measured: 0.069 px, ~20x closer. Invisible against a 1.4 px cut.
    CHECK(curveWorst < 0.1);
    CHECK(curveWorst < chordWorst / 15.0);
}

TEST_CASE("stroke path: the flattener honours its tolerance, and its cap") {
    const Vec2 p0{0.0, 0.0}, p1{10.0, 0.0}, p2{20.0, 20.0}, p3{30.0, 0.0};
    std::vector<Vec2> out;
    flattenCatmullRom(p0, p1, p2, p3, kTol, out);
    REQUIRE(!out.empty()); // this one genuinely bends

    // Every emitted point is ON the curve (the flattener samples it; it does not approximate it),
    // and they are in order from p1 to p2.
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double u = static_cast<double>(i + 1) / static_cast<double>(out.size() + 1);
        CHECK(dist(out[i], catmullRom(p0, p1, p2, p3, u)) == doctest::Approx(0.0));
    }

    // The polyline through them tracks the curve to within the tolerance: probe the midpoint of each
    // sub-chord against the true curve there.
    std::vector<Vec2> poly;
    poly.push_back(p1);
    poly.insert(poly.end(), out.begin(), out.end());
    poly.push_back(p2);
    const auto k = static_cast<double>(poly.size() - 1);
    for (std::size_t e = 0; e + 1 < poly.size(); ++e) {
        const double u = (static_cast<double>(e) + 0.5) / k;
        const Vec2 mid = (poly[e] + poly[e + 1]) * 0.5;
        CHECK(dist(mid, catmullRom(p0, p1, p2, p3, u)) < 4.0 * kTol);
    }

    SUBCASE("a violent bend is capped, not unbounded") {
        std::vector<Vec2> big;
        flattenCatmullRom({0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}, {1.5, 900.0}, 1e-6, big);
        CHECK(big.size() <= static_cast<std::size_t>(kMaxFlattenSteps));
    }
}

TEST_CASE("stroke path: a duplicated endpoint is a tangent, not a division by zero") {
    // begin() seeds the window with the first sample TWICE (a stroke has no sample before its
    // first), and flush() ends it the same way. Those duplicates make a knot span of exactly zero,
    // and the knot floor is what turns them into the natural chord-aligned end condition.
    const Vec2 a{10.0, 10.0}, b{20.0, 14.0}, c{30.0, 10.0};

    const Vec2 head = catmullRom(a, a, b, c, 0.5); // the stroke's FIRST span: p0 == p1
    CHECK(std::isfinite(head.x));
    CHECK(std::isfinite(head.y));
    CHECK(dist(catmullRom(a, a, b, c, 0.0), a) == doctest::Approx(0.0));
    CHECK(dist(catmullRom(a, a, b, c, 1.0), b) == doctest::Approx(0.0));

    const Vec2 tail = catmullRom(a, b, c, c, 0.5); // the stroke's LAST span: p2 == p3
    CHECK(std::isfinite(tail.x));
    CHECK(std::isfinite(tail.y));
    CHECK(dist(catmullRom(a, b, c, c, 1.0), c) == doctest::Approx(0.0));

    SUBCASE("every point duplicated -- a stroke that never moved") {
        const Vec2 p = catmullRom(a, a, a, a, 0.5);
        CHECK(std::isfinite(p.x));
        CHECK(dist(p, a) == doctest::Approx(0.0));
    }
}

TEST_CASE("stroke path: CENTRIPETAL -- wildly uneven samples do not cusp or loop") {
    // The reason the exponent is 0.5 and not 0. With the UNIFORM parameterization, a near-duplicate
    // point next to a far one makes the curve overshoot and loop back on itself -- putting a cusp in
    // a stroke the user drew smoothly. Mouse samples are nothing but unevenly spaced, so this is the
    // common case, not the pathological one.
    const Vec2 p0{0.0, 0.0}, p1{10.0, 0.0}, p2{10.6, 0.0}, p3{40.0, 0.0};

    // The span p1..p2 is short and sits between two long ones. The curve must stay inside it and
    // advance monotonically -- never turning back on itself.
    double prevX = catmullRom(p0, p1, p2, p3, 0.0).x;
    for (int i = 1; i <= 20; ++i) {
        const Vec2 c = catmullRom(p0, p1, p2, p3, static_cast<double>(i) / 20.0);
        CHECK(c.x >= prevX - 1e-9);              // monotone: no loop
        CHECK(c.x <= p2.x + 1e-6);               // no overshoot past the endpoint
        CHECK(c.x >= p1.x - 1e-6);               // ... nor behind the start
        CHECK(std::abs(c.y) < 1e-6);             // collinear input stays collinear
        prevX = c.x;
    }
}

// ---------------------------------------------------------------------------------------------
// The engine, end to end: the bug the user actually reported
// ---------------------------------------------------------------------------------------------
#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::StrokeInput;

namespace {

// ⚠ THE HALF-PIXEL TRAP (and it caught this test, not the engine). stamp() measures a dab's coverage
// from a pixel's CENTRE, which sits at (x + 0.5, y + 0.5). So the pixel that CONTAINS a point p is
// floor(p) -- never lround(p), which picks the neighbour whenever p's fraction is above a half, and
// would report a perfectly painted arc as bare.
[[nodiscard]] int px(double v) { return static_cast<int>(std::floor(v)); }

[[nodiscard]] int alphaAt(const Image& img, int x, int y) {
    if (x < 0 || y < 0 || x >= static_cast<int>(img.width) || y >= static_cast<int>(img.height))
        return 0;
    return img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3];
}

} // namespace

TEST_CASE("brush engine: a coarsely sampled curve paints the CURVE, not the polygon") {
    // The user's bug, at the level they see it. A mouse delivers one position per motion event, so a
    // fast arc arrives as a handful of far-apart points. With straight chords between them the paint
    // lands on the POLYGON inscribed in the arc -- it cuts the corner, and the true arc is left bare.
    //
    // A thin, hard tip and a big radius make the shortfall unmissable: the chord falls ~1.4 px inside
    // the arc, which is wider than the tip. So the arc's own midpoint between two samples is painted
    // if and only if the path is a curve.
    constexpr double kR = 40.0;
    constexpr int kCx = 48, kCy = 48;
    const auto onCircle = [&](double deg) {
        const double r = deg * std::acos(-1.0) / 180.0;
        return Vec2{kCx + kR * std::cos(r), kCy + kR * std::sin(r)};
    };

    BrushParams p;
    p.diameter = 2.0; // thin: it cannot bridge the sagitta by sheer width
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 1.0;
    p.spacing = 0.05;
    p.color = Color8{255, 255, 255, 255};

    // A quarter arc, sampled every 30 degrees -- coarse, exactly as a fast mouse stroke arrives.
    std::vector<StrokeInput> path;
    for (double deg = 180.0; deg <= 300.0; deg += 30.0)
        path.push_back(StrokeInput{onCircle(deg), 1.0});
    REQUIRE(path.size() == 5);

    Image img(96, 96);
    img.fill(Color8{0, 0, 0, 0});
    BrushEngine eng;
    eng.begin(96, 96, img, p, BrushDynamics{}, path.front());
    for (std::size_t i = 1; i < path.size(); ++i)
        eng.extendTo(path[i]);
    eng.flush(); // the walk lags one sample; lay the tail span
    eng.composite();
    eng.end();

    // Every SAMPLE is painted either way -- the curve interpolates them, and so did the chords. This
    // is the part that must not regress.
    for (const StrokeInput& s : path)
        CHECK_MESSAGE(alphaAt(img, px(s.pos.x), px(s.pos.y)) > 0,
                      "a sample the user actually made is unpainted");

    // ... and now the point of the whole exercise: the arc BETWEEN two samples. The chord through
    // 210 deg and 240 deg passes ~1.4 px inside the circle, so with a 2 px tip the true arc at
    // 225 deg was left bare. It must be painted now.
    const Vec2 mid = onCircle(225.0);
    const int mx = px(mid.x);
    const int my = px(mid.y);
    CHECK_MESSAGE(alphaAt(img, mx, my) > 0,
                  "the arc between two samples is bare -- the stroke is still a polygon");

    // And the corner the polygon would have cut: the chord's own midpoint, a good pixel INSIDE the
    // true arc. The curve does not go there, so it must stay unpainted -- otherwise this test would
    // pass for a stroke that simply painted everything.
    const Vec2 chordMid = (onCircle(210.0) + onCircle(240.0)) * 0.5;
    const double inward = kR - (chordMid - Vec2{static_cast<double>(kCx), static_cast<double>(kCy)})
                                   .length();
    REQUIRE(inward > 1.3); // the chord really does cut this far in
    CHECK_MESSAGE(alphaAt(img, px(chordMid.x), px(chordMid.y)) == 0,
                  "paint landed on the CHORD -- the walk is still cutting the corner");
}
