#include "core/vector/paint.hpp"
#include "ui/gradient_gesture.hpp"
#include "ui/shape_gesture.hpp" // shapeKindOf: the other half of the select-to-edit split

#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <optional>
#include <variant>
#include <vector>

using namespace mosaic::ui;
using mosaic::common::Affine2D;
using mosaic::common::Vec2;
namespace vec = mosaic::core::vec;
using doctest::Approx;

namespace {

std::vector<vec::GradientStop> blackWhite() {
    return {{0.0, vec::ColorF{0, 0, 0, 1}}, {1.0, vec::ColorF{1, 1, 1, 1}}};
}

const vec::Gradient& fillGrad(const vec::Object& o) {
    return std::get<vec::Gradient>(o.fill);
}

} // namespace

TEST_CASE("gradient shape <-> choice index round-trips") {
    CHECK(gradientShapeFromChoice(0) == GradientShape::Linear);
    CHECK(gradientShapeFromChoice(1) == GradientShape::Radial);
    CHECK(gradientShapeFromChoice(2) == GradientShape::Elliptical);
    CHECK(gradientShapeFromChoice(3) == GradientShape::Conic);
    for (auto s : {GradientShape::Linear, GradientShape::Radial, GradientShape::Elliptical,
                   GradientShape::Conic})
        CHECK(gradientShapeFromChoice(gradientChoiceForShape(s)) == s);
}

TEST_CASE("buildGradient linear maps unit endpoints to the drag") {
    const vec::Gradient g = buildGradient(GradientShape::Linear, {10, 20}, {60, 40}, 0.5,
                                          blackWhite(), vec::SpreadMethod::Pad, false);
    CHECK(g.type == vec::GradientType::Linear);
    const Vec2 a = g.transform.apply({0, 0});
    const Vec2 b = g.transform.apply({1, 0});
    CHECK(a.x == Approx(10));
    CHECK(a.y == Approx(20));
    CHECK(b.x == Approx(60));
    CHECK(b.y == Approx(40));
}

TEST_CASE("radial is isotropic, elliptical is anisotropic, conic is conic") {
    const auto r = buildGradient(GradientShape::Radial, {0, 0}, {30, 0}, 0.5, blackWhite(),
                                 vec::SpreadMethod::Pad, false);
    CHECK(r.type == vec::GradientType::Radial);
    CHECK(gradientShapeOf(vec::Object{.fill = r}) == GradientShape::Radial);

    const auto e = buildGradient(GradientShape::Elliptical, {0, 0}, {30, 0}, 0.4, blackWhite(),
                                 vec::SpreadMethod::Pad, false);
    CHECK(e.type == vec::GradientType::Radial); // stored as a radial with a squashed transform
    CHECK(gradientShapeOf(vec::Object{.fill = e}) == GradientShape::Elliptical);

    const auto c = buildGradient(GradientShape::Conic, {0, 0}, {30, 10}, 0.5, blackWhite(),
                                 vec::SpreadMethod::Pad, false);
    CHECK(c.type == vec::GradientType::Conic);
    CHECK(gradientShapeOf(vec::Object{.fill = c}) == GradientShape::Conic);
}

TEST_CASE("buildGradientDraft makes a full-bleed rect centred by its placement") {
    const auto d = buildGradientDraft(GradientShape::Linear, {10, 20}, {60, 40}, 100, 80,
                                      blackWhite(), vec::SpreadMethod::Pad, false);
    REQUIRE(d.has_value());
    const auto* ps = std::get_if<vec::ParametricShape>(&d->object.geometry);
    REQUIRE(ps != nullptr);
    const auto* rect = std::get_if<vec::RectShape>(ps);
    REQUIRE(rect != nullptr);
    CHECK(rect->size.x == Approx(100));
    CHECK(rect->size.y == Approx(80));
    CHECK(d->placement.apply({0, 0}).x == Approx(50)); // centre = (docW/2, docH/2)
    CHECK(d->placement.apply({0, 0}).y == Approx(40));
    CHECK_FALSE(d->object.stroke.enabled);
}

TEST_CASE("a sub-pixel drag authors nothing") {
    CHECK_FALSE(buildGradientDraft(GradientShape::Linear, {10, 20}, {10.2, 20.1}, 100, 80,
                                   blackWhite(), vec::SpreadMethod::Pad, false)
                    .has_value());
}

TEST_CASE("gradientHandles round-trips the drag endpoints through the placement") {
    const auto d = buildGradientDraft(GradientShape::Linear, {10, 20}, {60, 40}, 100, 80,
                                      blackWhite(), vec::SpreadMethod::Pad, false);
    REQUIRE(d.has_value());
    const GradientHandles h = gradientHandles(d->object, d->placement);
    REQUIRE(h.valid);
    CHECK(h.shape == GradientShape::Linear);
    CHECK(h.start.x == Approx(10));
    CHECK(h.start.y == Approx(20));
    CHECK(h.end.x == Approx(60));
    CHECK(h.end.y == Approx(40));
}

TEST_CASE("gradientHandles reports the true minor-axis edge, draggable only for Elliptical") {
    // A circular radial of radius 30 about the origin: unit (0,1) lands 30 px along +y, NOT on the
    // end handle (which is what the pre-outline code stored there).
    const vec::Object radial{.fill = buildGradient(GradientShape::Radial, {0, 0}, {30, 0}, 0.5,
                                                   blackWhite(), vec::SpreadMethod::Pad, false)};
    const GradientHandles rh = gradientHandles(radial, Affine2D::identity());
    REQUIRE(rh.valid);
    CHECK(rh.minor.x == Approx(0.0));
    CHECK(rh.minor.y == Approx(30.0));
    CHECK_FALSE(rh.hasMinor); // no fourth handle: a circle is retyped, not pulled, into an ellipse

    // The same drag as an Elliptical with ry/rx = 0.4 puts the minor edge at 12 px, and offers it.
    const vec::Object ell{.fill = buildGradient(GradientShape::Elliptical, {0, 0}, {30, 0}, 0.4,
                                                blackWhite(), vec::SpreadMethod::Pad, false)};
    const GradientHandles eh = gradientHandles(ell, Affine2D::identity());
    CHECK(eh.minor.x == Approx(0.0));
    CHECK(eh.minor.y == Approx(12.0));
    CHECK(eh.hasMinor);
}

TEST_CASE("hitGradientHandle picks the nearest handle and the linear body line") {
    const auto d = buildGradientDraft(GradientShape::Linear, {10, 20}, {60, 40}, 100, 80,
                                      blackWhite(), vec::SpreadMethod::Pad, false);
    const GradientHandles h = gradientHandles(d->object, d->placement);
    CHECK(hitGradientHandle(h, {60, 40}, 6.0) == 1);  // the end handle
    CHECK(hitGradientHandle(h, {10, 20}, 6.0) == 0);  // the start handle
    CHECK(hitGradientHandle(h, {35, 30}, 6.0) == 3);  // on the axis line -> body
    CHECK(hitGradientHandle(h, {35, 90}, 6.0) == -1); // far away -> nothing
}

TEST_CASE("the radial family's midpoint knob is grabbable, its minor edge only when elliptical") {
    const vec::Object radial{.fill = buildGradient(GradientShape::Radial, {100, 100}, {160, 100},
                                                   0.5, blackWhite(), vec::SpreadMethod::Pad,
                                                   false)};
    const GradientHandles rh = gradientHandles(radial, Affine2D::identity());
    CHECK(hitGradientHandle(rh, {160, 100}, 6.0) == 1); // the edge
    CHECK(hitGradientHandle(rh, {100, 100}, 6.0) == 0); // the centre
    CHECK(hitGradientHandle(rh, {130, 100}, 6.0) == 3); // the round midpoint knob -> rigid move
    CHECK(hitGradientHandle(rh, {100, 160}, 6.0) == -1); // the minor edge is NOT a handle here
    CHECK(hitGradientHandle(rh, {115, 100}, 6.0) == -1); // ... and there is no body line to grab

    const vec::Object ell{.fill = buildGradient(GradientShape::Elliptical, {100, 100}, {160, 100},
                                                0.5, blackWhite(), vec::SpreadMethod::Pad, false)};
    const GradientHandles eh = gradientHandles(ell, Affine2D::identity());
    CHECK(hitGradientHandle(eh, {100, 130}, 6.0) == 2); // ry = 0.5 * 60 = 30 px along +y
}

TEST_CASE("dragging the end handle moves the end, keeps the start, preserves stops") {
    const auto d = buildGradientDraft(GradientShape::Linear, {10, 20}, {60, 40}, 100, 80,
                                      blackWhite(), vec::SpreadMethod::Pad, false);
    const vec::Object edited =
        dragGradientHandle(d->object, d->placement, /*handle=*/1, {60, 40}, {70, 40}, false);
    const GradientHandles h = gradientHandles(edited, d->placement);
    CHECK(h.start.x == Approx(10)); // start unchanged
    CHECK(h.end.x == Approx(70));   // end moved by +10
    CHECK(h.end.y == Approx(40));
    CHECK(fillGrad(edited).stops.size() == 2); // ramp preserved
}

TEST_CASE("dragging the body translates the whole gradient") {
    const auto d = buildGradientDraft(GradientShape::Linear, {10, 20}, {60, 40}, 100, 80,
                                      blackWhite(), vec::SpreadMethod::Pad, false);
    const vec::Object edited =
        dragGradientHandle(d->object, d->placement, /*handle=*/3, {35, 30}, {45, 35}, false);
    const GradientHandles h = gradientHandles(edited, d->placement);
    CHECK(h.start.x == Approx(20)); // +10
    CHECK(h.start.y == Approx(25)); // +5
    CHECK(h.end.x == Approx(70));
    CHECK(h.end.y == Approx(45));
}

TEST_CASE("shift snaps the linear axis angle to 45 degrees") {
    // A 30 deg-ish drag snaps to 45 deg: the end sits on the y = x diagonal from the start.
    const vec::Gradient g = buildGradient(GradientShape::Linear, {0, 0}, {50, 25}, 0.5,
                                          blackWhite(), vec::SpreadMethod::Pad, /*shift=*/true);
    const Vec2 b = g.transform.apply({1, 0});
    CHECK(b.x == Approx(b.y).epsilon(0.01)); // 45 deg -> equal components
}

TEST_CASE("dragging the minor handle sets ry alone") {
    const vec::Object ell{.fill = buildGradient(GradientShape::Elliptical, {0, 0}, {60, 0}, 0.5,
                                                blackWhite(), vec::SpreadMethod::Pad, false)};
    // Pull the minor edge (0,30) out to (0,45): ry becomes 45, the major axis is untouched.
    const vec::Object taller =
        dragGradientHandle(ell, Affine2D::identity(), /*handle=*/2, {0, 30}, {0, 45}, false);
    const GradientHandles th = gradientHandles(taller, Affine2D::identity());
    CHECK(th.end.x == Approx(60.0)); // major axis unchanged
    CHECK(th.end.y == Approx(0.0));
    CHECK(th.minor.y == Approx(45.0));
    CHECK(gradientShapeOf(taller) == GradientShape::Elliptical);

    // Sliding it ALONG the major axis contributes nothing: the handle owns ry and only ry.
    const vec::Object sideways =
        dragGradientHandle(ell, Affine2D::identity(), /*handle=*/2, {0, 30}, {25, 30}, false);
    const GradientHandles sh = gradientHandles(sideways, Affine2D::identity());
    CHECK(sh.minor.y == Approx(30.0));
    CHECK(sh.end.x == Approx(60.0));
}

// ---- Retyping an existing gradient (the bar's "Type" choice on the bound layer) -----------------

TEST_CASE("retypeGradient keeps the dragged centre and axis across every kind") {
    const vec::Object linear{.fill = buildGradient(GradientShape::Linear, {20, 30}, {80, 70}, 0.5,
                                                   blackWhite(), vec::SpreadMethod::Reflect,
                                                   false)};
    for (const auto want : {GradientShape::Radial, GradientShape::Conic, GradientShape::Elliptical,
                            GradientShape::Linear}) {
        const vec::Object out = retypeGradient(linear, want);
        const GradientHandles h = gradientHandles(out, Affine2D::identity());
        REQUIRE(h.valid);
        CHECK(h.shape == want); // the shape actually took, incl. Elliptical (a real squash)
        CHECK(h.start.x == Approx(20.0));
        CHECK(h.start.y == Approx(30.0));
        CHECK(h.end.x == Approx(80.0)); // the axis endpoint survives the switch untouched
        CHECK(h.end.y == Approx(70.0));
        CHECK(fillGrad(out).spread == vec::SpreadMethod::Reflect); // ramp + spread preserved
        CHECK(fillGrad(out).stops == blackWhite());
    }
}

TEST_CASE("retyping a circle to Elliptical squashes it; retyping back restores the circle") {
    const vec::Object radial{.fill = buildGradient(GradientShape::Radial, {0, 0}, {40, 0}, 0.5,
                                                   blackWhite(), vec::SpreadMethod::Pad, false)};
    const vec::Object ell = retypeGradient(radial, GradientShape::Elliptical);
    // An ellipse with ry == rx would read straight back as a circular Radial, so the retype has to
    // adopt the default aspect for the choice to stick.
    CHECK(gradientShapeOf(ell) == GradientShape::Elliptical);
    const GradientHandles eh = gradientHandles(ell, Affine2D::identity());
    CHECK(eh.minor.y == Approx(40.0 * kGradientDefaultAspect));

    const vec::Object back = retypeGradient(ell, GradientShape::Radial);
    CHECK(gradientShapeOf(back) == GradientShape::Radial);
    const GradientHandles bh = gradientHandles(back, Affine2D::identity());
    CHECK(bh.minor.y == Approx(40.0)); // isotropic again
    CHECK(bh.end.x == Approx(40.0));
}

TEST_CASE("retypeGradient leaves a non-gradient object alone") {
    vec::Object solid;
    solid.fill = vec::SolidPaint{vec::ColorF{1, 0, 0, 1}};
    const vec::Object out = retypeGradient(solid, GradientShape::Conic);
    CHECK(std::holds_alternative<vec::SolidPaint>(out.fill));
}

// ---- The shape-outline overlay (the ring the gizmo draws round the radial family) ---------------
// The ring is a DISTANCE FIELD now, not a polyline: gradientRingDistance is the exact expression
// canvas_present.comp's gradientRing() evaluates per screen pixel, so what these tests measure is
// literally what the shader draws.

namespace {

// The ring's two basis vectors, read off the handles exactly as the canvas (and the shader) do.
Vec2 ringUx(const GradientHandles& h) { return {h.end.x - h.start.x, h.end.y - h.start.y}; }
Vec2 ringUy(const GradientHandles& h) { return {h.minor.x - h.start.x, h.minor.y - h.start.y}; }

// A point on the exact ellipse at parameter t, scaled radially in the ellipse's own frame (s == 1
// is on the curve; s != 1 steps off it).
Vec2 ringPoint(const GradientHandles& h, double t, double s = 1.0) {
    const Vec2 ux = ringUx(h), uy = ringUy(h);
    const double c = s * std::cos(t), n = s * std::sin(t);
    return {h.start.x + ux.x * c + uy.x * n, h.start.y + ux.y * c + uy.y * n};
}

// Brute-force truth: the real distance from `p` to the ellipse, by dense parametric search.
double trueRingDistance(const GradientHandles& h, Vec2 p, int samples = 20000) {
    double best = 1e300;
    for (int i = 0; i < samples; ++i) {
        const Vec2 q = ringPoint(h, 2.0 * M_PI * static_cast<double>(i) / samples);
        best = std::min(best, std::hypot(p.x - q.x, p.y - q.y));
    }
    return best;
}

GradientHandles radialHandles(Vec2 centre, Vec2 edge) {
    const vec::Object o{.fill = buildGradient(GradientShape::Radial, centre, edge, 0.5, blackWhite(),
                                              vec::SpreadMethod::Pad, false)};
    return gradientHandles(o, Affine2D::identity());
}

} // namespace

TEST_CASE("gradientHasRing rings the radial family and a conic, never a linear") {
    const vec::Object radial{.fill = buildGradient(GradientShape::Radial, {0, 0}, {25, 0}, 0.5,
                                                   blackWhite(), vec::SpreadMethod::Pad, false)};
    const vec::Object conic{.fill = buildGradient(GradientShape::Conic, {0, 0}, {25, 0}, 0.5,
                                                  blackWhite(), vec::SpreadMethod::Pad, false)};
    const vec::Object ell{.fill = buildGradient(GradientShape::Elliptical, {0, 0}, {25, 0}, 0.5,
                                                blackWhite(), vec::SpreadMethod::Pad, false)};
    const vec::Object linear{.fill = buildGradient(GradientShape::Linear, {0, 0}, {25, 0}, 0.5,
                                                   blackWhite(), vec::SpreadMethod::Pad, false)};
    CHECK(gradientHasRing(gradientHandles(radial, Affine2D::identity())));
    CHECK(gradientHasRing(gradientHandles(conic, Affine2D::identity())));
    CHECK(gradientHasRing(gradientHandles(ell, Affine2D::identity())));
    // A linear gradient's extent IS its axis line -- ringing it would be noise.
    CHECK_FALSE(gradientHasRing(gradientHandles(linear, Affine2D::identity())));
    CHECK_FALSE(gradientHasRing(GradientHandles{})); // invalid handles -> nothing
}

TEST_CASE("gradientRingDistance is zero on the ring and signed off it") {
    const GradientHandles h = radialHandles({100, 50}, {130, 50}); // centre (100,50), radius 30
    const Vec2 ux = ringUx(h), uy = ringUy(h);
    for (int i = 0; i < 64; ++i) // every point of the exact circle is ON the ring
        CHECK(std::abs(gradientRingDistance(h.start, ux, uy,
                                            ringPoint(h, 2.0 * M_PI * i / 64.0))) < 1e-9);
    // For a circle the estimate reduces in closed form to (rho^2 - R^2) / (2 rho): positive
    // outside, negative inside, and equal to the true distance to first order in (rho - R).
    CHECK(gradientRingDistance(h.start, ux, uy, {133, 50}) ==
          Approx((33.0 * 33.0 - 900.0) / (2.0 * 33.0))); // 3 px outside -> +2.864
    CHECK(gradientRingDistance(h.start, ux, uy, {100, 23}) ==
          Approx((27.0 * 27.0 - 900.0) / (2.0 * 27.0))); // 3 px inside  -> -3.167
    // Two "no ring here" cases the shader guards identically (it leaves the pixel untouched): a
    // degenerate/collinear basis, and the exact centre, where grad F vanishes.
    CHECK(std::isinf(gradientRingDistance({0, 0}, {10, 0}, {20, 0}, {5, 5})));
    CHECK(std::isinf(gradientRingDistance(h.start, ux, uy, h.start)));
}

TEST_CASE("gradientRingDistance follows a rotated, squashed ellipse") {
    // A drag straight down: the major axis (60) runs +y, the minor (0.5 * 60 = 30) runs -x.
    const vec::Object ell{.fill = buildGradient(GradientShape::Elliptical, {10, 10}, {10, 70}, 0.5,
                                                blackWhite(), vec::SpreadMethod::Pad, false)};
    const GradientHandles h = gradientHandles(ell, Affine2D::identity());
    CHECK(h.end.x == Approx(10.0));
    CHECK(h.end.y == Approx(70.0));
    CHECK(h.minor.x == Approx(-20.0)); // 10 - 30
    CHECK(h.minor.y == Approx(10.0));
    const Vec2 ux = ringUx(h), uy = ringUy(h);
    for (int i = 0; i < 128; ++i)
        CHECK(std::abs(gradientRingDistance(h.start, ux, uy,
                                            ringPoint(h, 2.0 * M_PI * i / 128.0))) < 1e-9);
    // Inside is negative, outside positive -- in the ring's own frame, not the screen's.
    CHECK(std::isinf(gradientRingDistance(h.start, ux, uy, {10, 10}))); // the centre: "no ring"
    CHECK(gradientRingDistance(h.start, ux, uy, {10, 80}) > 0.0);       // past the major tip
    CHECK(gradientRingDistance(h.start, ux, uy, {10, 60}) < 0.0);       // inside it
}

// The estimate must measure REAL pixels, not the unit-space parameter -- otherwise a squashed
// ellipse would draw a ring of varying thickness. Checked against a brute-force distance-to-ellipse
// over the band a 1 px hairline actually covers.
TEST_CASE("the analytic ring measures pixels on an eccentric ellipse") {
    // 2:1, and rotated 30 deg, so neither axis is aligned with anything.
    const vec::Object ell{
        .fill = buildGradient(GradientShape::Elliptical, {40, -15},
                              {40 + 200 * std::cos(M_PI / 6.0), -15 + 200 * std::sin(M_PI / 6.0)},
                              0.5, blackWhite(), vec::SpreadMethod::Pad, false)};
    const GradientHandles h = gradientHandles(ell, Affine2D::identity());
    const Vec2 ux = ringUx(h), uy = ringUy(h);
    double worst = 0.0;
    for (int i = 0; i < 48; ++i) {
        const double t = 2.0 * M_PI * (static_cast<double>(i) + 0.5) / 48.0;
        for (const double s : {0.995, 1.0, 1.005}) { // ~ +/- 1 px, the hairline's own reach
            const Vec2 probe = ringPoint(h, t, s);
            worst = std::max(worst, std::abs(std::abs(gradientRingDistance(h.start, ux, uy, probe)) -
                                             trueRingDistance(h, probe)));
        }
    }
    CHECK(worst < 0.15);
}

// THE smoothness criterion, and the reason the ring stopped being a polyline. A chorded ring's
// worst deviation from the true circle is its chord SAG, R*(1 - cos(pi/n)) -- which grows without
// bound as you zoom in unless n does, and n was pinned at 64 by the shared 64-entry guide lane
// (0.72 px of sag at a 600 px radius, 1.2 px at 1000 px: a visible polygon). The analytic ring has
// no n at all: its error is second order in the DISTANCE from the curve and, as this shows,
// independent of the radius. On a circle the radial offset IS the exact distance, so this compares
// against ground truth with no approximation of its own.
TEST_CASE("the analytic ring stays sub-quarter-pixel at every zoom, where a 64-gon does not") {
    for (const double r : {10.0, 60.0, 207.0, 600.0, 2000.0, 10000.0}) {
        const GradientHandles h = radialHandles({0, 0}, {r, 0});
        const Vec2 ux = ringUx(h), uy = ringUy(h);
        // Probe midway between where 64 chords would have put their vertices -- the sag's worst
        // case -- and out to 1.5 px off the curve, which is where the hairline's coverage ends.
        double worst = 0.0;
        for (int i = 0; i < 64; ++i) {
            const double t = 2.0 * M_PI * (static_cast<double>(i) + 0.5) / 64.0;
            for (const double off : {-1.5, -0.5, 0.0, 0.5, 1.5}) {
                const Vec2 probe = ringPoint(h, t, (r + off) / r); // exactly |off| px off the ring
                worst = std::max(worst,
                                 std::abs(std::abs(gradientRingDistance(h.start, ux, uy, probe)) -
                                          std::abs(off)));
            }
        }
        CHECK(worst < 0.25); // reads as a smooth curve at any radius
        // The polyline it replaced: 64 chords sag R*(1 - cos(pi/64)), which blows that same budget
        // for anything past a ~200 px on-screen radius -- and 64 was the WHOLE lane, guides too.
        const double sag64 = r * (1.0 - std::cos(M_PI / 64.0));
        if (r >= 600.0)
            CHECK(sag64 > 0.25);
    }
}

// ---- Blend curve (the per-stop midpoint) -------------------------------------------------------
// sampleAt evaluates a Gradient at a layer-local point; for an identity-transform linear gradient
// the parameter is just the x coordinate, so we can probe the ramp directly.

TEST_CASE("a 0.5 midpoint is a plain linear blend") {
    vec::Gradient g;
    g.type = vec::GradientType::Linear;
    g.stops = blackWhite(); // midpoints default to 0.5
    const vec::ColorF mid = vec::sampleAt(vec::Paint{g}, {0.5, 0.0});
    CHECK(mid.r == Approx(0.5)); // halfway is 50% grey
}

TEST_CASE("a biased midpoint moves the 50% crossover") {
    vec::Gradient g;
    g.type = vec::GradientType::Linear;
    g.stops = blackWhite();
    g.stops[0].midpoint = 0.25; // reach 50% grey a quarter of the way along
    CHECK(vec::sampleAt(vec::Paint{g}, {0.25, 0.0}).r == Approx(0.5));
    // Past the midpoint the ramp is already brighter than the linear 50%.
    CHECK(vec::sampleAt(vec::Paint{g}, {0.5, 0.0}).r > 0.5);
    // Endpoints are still pinned.
    CHECK(vec::sampleAt(vec::Paint{g}, {0.0, 0.0}).r == Approx(0.0));
    CHECK(vec::sampleAt(vec::Paint{g}, {1.0, 0.0}).r == Approx(1.0));
}

// ---- Which tool binds which object (the select-to-edit split, S22) -----------------------------

TEST_CASE("the Shape tool never binds a gradient layer, and the Gradient tool never binds a shape") {
    // A gradient layer's geometry is a plain full-bleed RECT, so shapeKindOf calls it a Rect and
    // the Shape bar would happily bind it -- and then flat-fill it from the colour swatch. That is
    // the defect: the exclusion has to be on the PAINT, not on the geometry.
    const std::optional<GradientDraft> draft =
        buildGradientDraft(GradientShape::Radial, {10, 10}, {90, 60}, 200, 120, blackWhite(),
                           vec::SpreadMethod::Pad, false);
    REQUIRE(draft.has_value());
    CHECK(shapeKindOf(draft->object).has_value()); // it really does look like a Rect...
    CHECK_FALSE(shapeToolBinds(draft->object));    // ... and is excluded anyway
    CHECK(gradientToolBinds(draft->object));

    // The mirror: an ordinary solid-filled shape. The Gradient bar's Type / Stops / Dithering have
    // nothing to drive on it, so the Gradient tool leaves it alone; the Shape tool takes it.
    vec::Object shape;
    shape.geometry = vec::ParametricShape{vec::StarShape{}};
    shape.fill = vec::SolidPaint{vec::ColorF{1, 0, 0, 1}};
    CHECK(shapeToolBinds(shape));
    CHECK_FALSE(gradientToolBinds(shape));

    // A shape with NO paint at all is still the Shape tool's; a gradient-filled star is not (the
    // predicate is about the paint the bar can express, not about the geometry's name).
    vec::Object unpainted;
    unpainted.geometry = vec::ParametricShape{vec::EllipseShape{{5, 5}}};
    CHECK(shapeToolBinds(unpainted));
    CHECK_FALSE(gradientToolBinds(unpainted));
    vec::Object gradientStar;
    gradientStar.geometry = vec::ParametricShape{vec::StarShape{}};
    gradientStar.fill = buildGradient(GradientShape::Linear, {0, 0}, {10, 0}, 0.5, blackWhite(),
                                      vec::SpreadMethod::Pad, false);
    CHECK_FALSE(shapeToolBinds(gradientStar));
    CHECK(gradientToolBinds(gradientStar));
}

// ---- Dithering: the bar <-> model bridge, and its survival through every edit (S22) -------------

TEST_CASE("dither choice index round-trips through the model kind") {
    CHECK(gradientDitherFromChoice(0) == vec::DitherKind::None);
    CHECK(gradientDitherFromChoice(1) == vec::DitherKind::Ordered);
    CHECK(gradientDitherFromChoice(2) == vec::DitherKind::BlueNoise);
    CHECK(gradientDitherFromChoice(3) == vec::DitherKind::Noise);
    CHECK(gradientDitherFromChoice(-1) == vec::DitherKind::None); // out of range -> the default
    CHECK(gradientDitherFromChoice(99) == vec::DitherKind::None);
    for (auto k : {vec::DitherKind::None, vec::DitherKind::Ordered, vec::DitherKind::BlueNoise,
                   vec::DitherKind::Noise})
        CHECK(gradientDitherFromChoice(gradientDitherChoice(k)) == k);
}

TEST_CASE("a gradient's dither kind survives authoring, retyping and every handle drag") {
    const std::optional<GradientDraft> draft = buildGradientDraft(
        GradientShape::Elliptical, {10, 10}, {90, 60}, 200, 120, blackWhite(),
        vec::SpreadMethod::Reflect, false, vec::DitherKind::BlueNoise);
    REQUIRE(draft.has_value());
    CHECK(fillGrad(draft->object).dither == vec::DitherKind::BlueNoise);

    // Retyping re-authors the gradient from its own two points; the ramp settings come with it.
    for (auto shape : {GradientShape::Linear, GradientShape::Radial, GradientShape::Conic,
                       GradientShape::Elliptical}) {
        const vec::Object out = retypeGradient(draft->object, shape);
        CHECK(fillGrad(out).dither == vec::DitherKind::BlueNoise);
        CHECK(fillGrad(out).spread == vec::SpreadMethod::Reflect); // the S22 precedent it follows
    }
    // ... and so does a handle re-drag, on every handle (0 centre, 1 edge, 2 minor, 3 body).
    for (int handle = 0; handle <= 3; ++handle) {
        const vec::Object out = dragGradientHandle(draft->object, draft->placement, handle,
                                                   {50, 50}, {70, 40}, false);
        CHECK(fillGrad(out).dither == vec::DitherKind::BlueNoise);
    }
    // The default stays None, so a gradient authored without one is exactly a pre-S22 gradient.
    const std::optional<GradientDraft> plain =
        buildGradientDraft(GradientShape::Linear, {10, 10}, {90, 60}, 200, 120, blackWhite(),
                           vec::SpreadMethod::Pad, false);
    REQUIRE(plain.has_value());
    CHECK(fillGrad(plain->object).dither == vec::DitherKind::None);
}
