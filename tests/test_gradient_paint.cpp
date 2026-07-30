#include "core/vector/paint.hpp"
#include "ui/gradient_gesture.hpp"

#include <cmath>
#include <doctest/doctest.h>
#include <variant>
#include <vector>

// The GRADIENT EVALUATION half of S22 (docs/gradient-tool.md §3): every model type -- Linear,
// Radial and Conic -- under every SpreadMethod, pinned to hand-computed values, plus the proof
// obligation the four-shapes-to-three-types design carries: an ELLIPTICAL authoring is a Radial
// whose transform maps the unit circle onto the intended ellipse. A kind that authors correctly but
// evaluates as another kind looks exactly like "radial doesn't work", so these probe the numbers,
// never just "a colour came out".
//
// The probe: a black(0) -> white(1) two-stop ramp with default (0.5, linear) blend curves makes
// sampleAt's red channel read back the spread-mapped gradient PARAMETER exactly -- sampleStops
// lerps black to white by t, so r == t. Every expectation below is therefore a parameter.

using mosaic::common::Affine2D;
using mosaic::common::Vec2;
namespace vec = mosaic::core::vec;
namespace ui = mosaic::ui;
using doctest::Approx;

namespace {

std::vector<vec::GradientStop> blackWhite() {
    return {{0.0, vec::ColorF{0, 0, 0, 1}}, {1.0, vec::ColorF{1, 1, 1, 1}}};
}

vec::Gradient grad(vec::GradientType type, const Affine2D& t, vec::SpreadMethod spread) {
    vec::Gradient g;
    g.type = type;
    g.stops = blackWhite();
    g.transform = t;
    g.spread = spread;
    return g;
}

// The spread-mapped gradient parameter at a layer-local point (see the probe note above).
double param(const vec::Gradient& g, Vec2 localPt) {
    return static_cast<double>(vec::sampleAt(vec::Paint{g}, localPt).r);
}

} // namespace

// ---- The three types are three genuinely different functions ------------------------------------

TEST_CASE("one transform, one point, three types, three different parameters") {
    // Unit space -> local: no rotation, scale 100. The probe point is unit (0,1) -- straight down
    // the gradient's y axis, where the three kinds disagree by construction.
    const Affine2D t = Affine2D::trs({0, 0}, 0.0, {100, 100});
    const Vec2 p{0, 100};
    CHECK(param(grad(vec::GradientType::Linear, t, vec::SpreadMethod::Pad), p) == Approx(0.0));
    CHECK(param(grad(vec::GradientType::Radial, t, vec::SpreadMethod::Pad), p) == Approx(1.0));
    // atan2(1, 0) = pi/2 -> a quarter of the way round the sweep.
    CHECK(param(grad(vec::GradientType::Conic, t, vec::SpreadMethod::Pad), p) == Approx(0.25));
}

// ---- Linear -------------------------------------------------------------------------------------

TEST_CASE("linear projects onto its axis and ignores the perpendicular") {
    const auto g = grad(vec::GradientType::Linear, Affine2D::trs({10, 20}, 0.0, {50, 50}),
                        vec::SpreadMethod::Pad);
    CHECK(param(g, {10, 20}) == Approx(0.0));  // the start handle
    CHECK(param(g, {35, 20}) == Approx(0.5));  // halfway
    CHECK(param(g, {60, 20}) == Approx(1.0));  // the end handle
    CHECK(param(g, {35, 999}) == Approx(0.5)); // ... at any distance off the axis
}

TEST_CASE("a rotated linear axis follows the drag") {
    // A quarter turn: the axis runs down +y, so x becomes the ignored perpendicular.
    const auto g = grad(vec::GradientType::Linear, Affine2D::trs({0, 0}, M_PI / 2.0, {100, 100}),
                        vec::SpreadMethod::Pad);
    CHECK(param(g, {0, 50}) == Approx(0.5));
    CHECK(param(g, {50, 0}) == Approx(0.0)); // straight along the perpendicular -> parameter 0
}

TEST_CASE("linear honours every spread method") {
    const Affine2D t = Affine2D::trs({0, 0}, 0.0, {100, 100});
    // x = 125 is parameter 1.25; x = -25 is -0.25.
    CHECK(param(grad(vec::GradientType::Linear, t, vec::SpreadMethod::Pad), {125, 0}) ==
          Approx(1.0));
    CHECK(param(grad(vec::GradientType::Linear, t, vec::SpreadMethod::Repeat), {125, 0}) ==
          Approx(0.25));
    CHECK(param(grad(vec::GradientType::Linear, t, vec::SpreadMethod::Reflect), {125, 0}) ==
          Approx(0.75));
    CHECK(param(grad(vec::GradientType::Linear, t, vec::SpreadMethod::Pad), {-25, 0}) ==
          Approx(0.0));
    CHECK(param(grad(vec::GradientType::Linear, t, vec::SpreadMethod::Repeat), {-25, 0}) ==
          Approx(0.75));
    CHECK(param(grad(vec::GradientType::Linear, t, vec::SpreadMethod::Reflect), {-25, 0}) ==
          Approx(0.25));
}

// ---- Radial -------------------------------------------------------------------------------------

TEST_CASE("radial is the distance from its centre, in every direction alike") {
    const auto g = grad(vec::GradientType::Radial, Affine2D::trs({100, 100}, 0.0, {40, 40}),
                        vec::SpreadMethod::Pad);
    CHECK(param(g, {100, 100}) == Approx(0.0)); // the centre handle
    CHECK(param(g, {140, 100}) == Approx(1.0)); // the edge handle
    CHECK(param(g, {120, 100}) == Approx(0.5));
    CHECK(param(g, {100, 120}) == Approx(0.5)); // isotropy: a LINEAR gradient would read 0 here
    CHECK(param(g, {80, 100}) == Approx(0.5));
    const double d = 40.0 / std::sqrt(2.0);
    CHECK(param(g, {100 + d, 100 + d}) == Approx(1.0)); // 45 deg out is still exactly the edge
}

TEST_CASE("a rotated radial is the same circle (an isotropic scale swallows the rotation)") {
    const auto a = grad(vec::GradientType::Radial, Affine2D::trs({0, 0}, 0.0, {30, 30}),
                        vec::SpreadMethod::Pad);
    const auto b = grad(vec::GradientType::Radial, Affine2D::trs({0, 0}, 1.1, {30, 30}),
                        vec::SpreadMethod::Pad);
    for (const Vec2 p : {Vec2{15, 0}, Vec2{0, 15}, Vec2{-9, 12}, Vec2{21, 21}})
        CHECK(param(a, p) == Approx(param(b, p)));
}

TEST_CASE("radial honours every spread method") {
    const Affine2D t = Affine2D::trs({0, 0}, 0.0, {40, 40});
    // r = 50 px is parameter 1.25; r = 130 px is 3.25.
    CHECK(param(grad(vec::GradientType::Radial, t, vec::SpreadMethod::Pad), {50, 0}) ==
          Approx(1.0));
    CHECK(param(grad(vec::GradientType::Radial, t, vec::SpreadMethod::Repeat), {50, 0}) ==
          Approx(0.25));
    CHECK(param(grad(vec::GradientType::Radial, t, vec::SpreadMethod::Reflect), {50, 0}) ==
          Approx(0.75));
    CHECK(param(grad(vec::GradientType::Radial, t, vec::SpreadMethod::Pad), {0, 130}) ==
          Approx(1.0));
    CHECK(param(grad(vec::GradientType::Radial, t, vec::SpreadMethod::Repeat), {0, 130}) ==
          Approx(0.25));
    // fmod(3.25, 2) = 1.25 > 1 -> 2 - 1.25 = 0.75 (the second ring runs backwards).
    CHECK(param(grad(vec::GradientType::Radial, t, vec::SpreadMethod::Reflect), {0, 130}) ==
          Approx(0.75));
}

// ---- Conic --------------------------------------------------------------------------------------

TEST_CASE("conic sweeps the angle from the axis, and ignores the radius") {
    const auto g = grad(vec::GradientType::Conic, Affine2D::trs({50, 50}, 0.0, {30, 30}),
                        vec::SpreadMethod::Pad);
    CHECK(param(g, {80, 50}) == Approx(0.0));  // +x: the first stop
    CHECK(param(g, {50, 80}) == Approx(0.25)); // +y (screen down): a quarter turn
    CHECK(param(g, {20, 50}) == Approx(0.5));  // -x: half
    CHECK(param(g, {50, 20}) == Approx(0.75)); // -y: three quarters (atan2 is wrapped into [0,1))
    // Radius-independent: the same angles at a fifth and at ten times the radius.
    CHECK(param(g, {56, 50}) == Approx(0.0));
    CHECK(param(g, {50, 350}) == Approx(0.25));
}

TEST_CASE("a rotated conic starts its sweep where the drag pointed") {
    // A quarter-turn drag: unit (1,0) -- the first stop -- now lands straight down +y.
    const auto g = grad(vec::GradientType::Conic, Affine2D::trs({50, 50}, M_PI / 2.0, {30, 30}),
                        vec::SpreadMethod::Pad);
    CHECK(param(g, {50, 80}) == Approx(0.0));
    CHECK(param(g, {20, 50}) == Approx(0.25));
}

TEST_CASE("conic's parameter never leaves [0,1), so all three spreads agree") {
    const Affine2D t = Affine2D::trs({0, 0}, 0.0, {10, 10});
    for (const Vec2 p : {Vec2{5, 0}, Vec2{0, 5}, Vec2{-5, 0}, Vec2{0, -5}, Vec2{3, -4},
                         Vec2{-7, -2}}) {
        const double pad = param(grad(vec::GradientType::Conic, t, vec::SpreadMethod::Pad), p);
        CHECK(param(grad(vec::GradientType::Conic, t, vec::SpreadMethod::Repeat), p) ==
              Approx(pad));
        CHECK(param(grad(vec::GradientType::Conic, t, vec::SpreadMethod::Reflect), p) ==
              Approx(pad));
        CHECK(pad >= 0.0);
        CHECK(pad < 1.0);
    }
}

// ---- Elliptical == a Radial under an anisotropic transform (the design's proof obligation) ------
// The model has no Elliptical type (docs/gradient-tool.md §2). What makes the tool's fourth shape
// real is that its transform maps the unit CIRCLE onto the intended ellipse -- so these tests check
// the transform geometrically AND check that the evaluated iso-parameter contours are that ellipse.

TEST_CASE("an elliptical authoring is a Radial whose transform makes the intended ellipse") {
    const Vec2 centre{20, 40};
    const Vec2 edge{20 + 60, 40}; // a 60 px major axis along +x
    constexpr double kAspect = 0.25;
    const vec::Gradient g = ui::buildGradient(ui::GradientShape::Elliptical, centre, edge, kAspect,
                                              blackWhite(), vec::SpreadMethod::Pad, false);
    // No new enum: the object carries a plain Radial.
    CHECK(g.type == vec::GradientType::Radial);
    CHECK(ui::gradientShapeOf(vec::Object{.fill = g}) == ui::GradientShape::Elliptical);

    // The unit circle's image IS the ellipse rx = 60, ry = 15 about the centre. Checked pointwise,
    // which is the whole "elliptical is a radial with an anisotropic transform" claim.
    const double rx = 60.0;
    const double ry = rx * kAspect;
    for (int i = 0; i < 16; ++i) {
        const double th = 2.0 * M_PI * i / 16.0;
        const Vec2 mapped = g.transform.apply({std::cos(th), std::sin(th)});
        CHECK(mapped.x == Approx(centre.x + rx * std::cos(th)));
        CHECK(mapped.y == Approx(centre.y + ry * std::sin(th)));
    }
}

TEST_CASE("an elliptical gradient's iso-parameter contours are concentric ellipses") {
    const Vec2 centre{0, 0};
    constexpr double kRx = 80.0;
    constexpr double kRy = 40.0; // aspect 0.5
    const vec::Gradient g = ui::buildGradient(ui::GradientShape::Elliptical, centre, {kRx, 0}, 0.5,
                                              blackWhite(), vec::SpreadMethod::Pad, false);
    for (int i = 0; i < 16; ++i) {
        const double th = 2.0 * M_PI * i / 16.0;
        // Half the ellipse -> parameter 0.5 exactly, all the way round. A value strictly inside
        // (0,1) means no clamp can be hiding a wrong contour.
        CHECK(param(g, {0.5 * kRx * std::cos(th), 0.5 * kRy * std::sin(th)}) == Approx(0.5));
        // ... and the ellipse itself -> exactly the last stop.
        CHECK(param(g, {kRx * std::cos(th), kRy * std::sin(th)}) == Approx(1.0));
    }
    // The discriminator against a circular radial of the same rx: on the minor axis, half of ry is
    // parameter 0.5 for the ellipse but only ry/2/rx = 0.25 for a circle.
    const vec::Gradient circle = ui::buildGradient(ui::GradientShape::Radial, centre, {kRx, 0}, 1.0,
                                                   blackWhite(), vec::SpreadMethod::Pad, false);
    CHECK(param(g, {0, 0.5 * kRy}) == Approx(0.5));
    CHECK(param(circle, {0, 0.5 * kRy}) == Approx(0.25));
}

TEST_CASE("a rotated elliptical keeps its axes glued to the drag") {
    // A drag straight down: rx = 60 runs +y, ry = 30 runs -x (the +90 deg perpendicular).
    const vec::Gradient g = ui::buildGradient(ui::GradientShape::Elliptical, {0, 0}, {0, 60}, 0.5,
                                              blackWhite(), vec::SpreadMethod::Pad, false);
    CHECK(param(g, {0, 60}) == Approx(1.0));  // the major-axis edge
    CHECK(param(g, {-30, 0}) == Approx(1.0)); // the minor-axis edge
    CHECK(param(g, {30, 0}) == Approx(1.0));  // ... symmetric about the centre
    CHECK(param(g, {0, 30}) == Approx(0.5));
    CHECK(param(g, {-15, 0}) == Approx(0.5));
    CHECK(ui::gradientShapeOf(vec::Object{.fill = g}) == ui::GradientShape::Elliptical);
}

TEST_CASE("elliptical honours every spread method") {
    const vec::Gradient base =
        ui::buildGradient(ui::GradientShape::Elliptical, {0, 0}, {80, 0}, 0.5, blackWhite(),
                          vec::SpreadMethod::Pad, false);
    // (100, 0) is 1.25 major radii out; (0, 50) is 1.25 MINOR radii out -- the same parameter,
    // which is the point: the spread rings are ellipses too.
    for (const Vec2 p : {Vec2{100, 0}, Vec2{0, 50}}) {
        vec::Gradient g = base;
        g.spread = vec::SpreadMethod::Pad;
        CHECK(param(g, p) == Approx(1.0));
        g.spread = vec::SpreadMethod::Repeat;
        CHECK(param(g, p) == Approx(0.25));
        g.spread = vec::SpreadMethod::Reflect;
        CHECK(param(g, p) == Approx(0.75));
    }
}

// ---- The authoring -> evaluation chain, end to end ----------------------------------------------

TEST_CASE("a drag authored by the tool evaluates as the kind the user picked") {
    // A 200x100 document; the drag runs from its centre out 40 px along +x. buildGradientDraft
    // centres the full-bleed rect on the local origin, so a doc point's local coordinate is
    // (doc - (100, 50)).
    const Vec2 pressDoc{100, 50};
    const Vec2 currentDoc{140, 50};
    const auto local = [](Vec2 doc) { return Vec2{doc.x - 100.0, doc.y - 50.0}; };

    const auto draftFor = [&](ui::GradientShape s) {
        const auto d = ui::buildGradientDraft(s, pressDoc, currentDoc, 200, 100, blackWhite(),
                                              vec::SpreadMethod::Pad, false);
        REQUIRE(d.has_value());
        return std::get<vec::Gradient>(d->object.fill);
    };

    const vec::Gradient lin = draftFor(ui::GradientShape::Linear);
    CHECK(param(lin, local({120, 50})) == Approx(0.5));
    CHECK(param(lin, local({100, 90})) == Approx(0.0)); // perpendicular: still the first stop

    const vec::Gradient rad = draftFor(ui::GradientShape::Radial);
    CHECK(param(rad, local({120, 50})) == Approx(0.5));
    CHECK(param(rad, local({100, 70})) == Approx(0.5)); // ... a circle, 20 px in ANY direction
    CHECK(param(rad, local({140, 50})) == Approx(1.0));

    const vec::Gradient ell = draftFor(ui::GradientShape::Elliptical);
    CHECK(param(ell, local({140, 50})) == Approx(1.0)); // rx = 40 along +x
    CHECK(param(ell, local({100, 70})) == Approx(1.0)); // ry = 20 along +y (the default aspect)
    CHECK(param(ell, local({120, 50})) == Approx(0.5));

    const vec::Gradient con = draftFor(ui::GradientShape::Conic);
    CHECK(param(con, local({140, 50})) == Approx(0.0));
    CHECK(param(con, local({100, 90})) == Approx(0.25));
    CHECK(param(con, local({60, 50})) == Approx(0.5));
}

// ---- Blend curves are a property of the RAMP, so they apply to every kind ----------------------

TEST_CASE("a blend-curve midpoint biases a radial ramp the same way it biases a linear one") {
    vec::Gradient g = grad(vec::GradientType::Radial, Affine2D::trs({0, 0}, 0.0, {100, 100}),
                           vec::SpreadMethod::Pad);
    g.stops[0].midpoint = 0.25; // reach 50% grey a quarter of the way out
    CHECK(param(g, {25, 0}) == Approx(0.5));
    CHECK(param(g, {0, 25}) == Approx(0.5)); // ... on the whole ring, not just the axis
    CHECK(param(g, {50, 0}) > 0.5);
    CHECK(param(g, {0, 0}) == Approx(0.0));
    CHECK(param(g, {100, 0}) == Approx(1.0));
}
