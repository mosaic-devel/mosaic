#include "ui/transform_gesture.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>

// The S15 Move tool's pure half: handle geometry, hit-testing, and the gesture math (move with
// axis lock, scale with uniform/from-centre, rotate with 15-degree snap). The FLTK/Vulkan
// plumbing in VulkanCanvas stays thin and is exercised by the --gui-frames smoke run.
namespace {

using mosaic::common::Affine2D;
using mosaic::common::Rect;
using mosaic::common::Vec2;
using mosaic::ui::hitTextEditBox;
using mosaic::ui::hitTransformControls;
using mosaic::ui::rotateDotOpacity;
using mosaic::ui::TextBoxControl;
using mosaic::ui::TransformGesture;
using mosaic::ui::transformHandleCenters;
using mosaic::ui::TransformMode;
using mosaic::ui::transformQuadMismatch;
using mosaic::ui::transformQuadWackiness;

// An axis-aligned 100x60 quad at (10, 20) in TL,TR,BR,BL order.
const std::array<Vec2, 4> kQuad{{{10, 20}, {110, 20}, {110, 80}, {10, 80}}};

// The content rect most gestures below frame: a 10x10 box at the layer origin.
const Rect kBox10{0, 0, 10, 10};

bool near(Vec2 a, Vec2 b, double eps = 1e-9) {
    return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps;
}

} // namespace

TEST_CASE("transformHandleCenters: corners then edge midpoints") {
    const auto h = transformHandleCenters(kQuad);
    CHECK(near(h[0], {10, 20}));
    CHECK(near(h[4], {60, 20})); // top mid
    CHECK(near(h[5], {110, 50})); // right mid
    CHECK(near(h[6], {60, 80}));
    CHECK(near(h[7], {10, 50}));
}

TEST_CASE("hitTransformControls: handles, rotate band, body, miss") {
    const double r = 7.0;
    const double band = 18.0;
    // Dead on the BR corner handle.
    auto hit = hitTransformControls({110, 80}, kQuad, r, band);
    REQUIRE(hit.has_value());
    CHECK(hit->mode == TransformMode::Scale);
    CHECK(hit->handle == 2);
    // Near the top mid handle.
    hit = hitTransformControls({62, 22}, kQuad, r, band);
    REQUIRE(hit.has_value());
    CHECK(hit->mode == TransformMode::Scale);
    CHECK(hit->handle == 4);
    // Just outside a corner handle, inside the rotate band.
    hit = hitTransformControls({120, 90}, kQuad, r, band);
    REQUIRE(hit.has_value());
    CHECK(hit->mode == TransformMode::Rotate);
    // Inside the body.
    hit = hitTransformControls({60, 50}, kQuad, r, band);
    REQUIRE(hit.has_value());
    CHECK(hit->mode == TransformMode::Move);
    // Far away.
    CHECK_FALSE(hitTransformControls({200, 200}, kQuad, r, band).has_value());
}

TEST_CASE("hitTextEditBox: BR resizes, edge moves, interior is the caret, corner rotates") {
    const double r = 7.0;     // BR handle radius
    const double band = 18.0; // rotate band
    const double edge = 5.0;  // frame-edge move band
    // The bottom-right corner: resize (the only resize handle on the typographic box).
    CHECK(hitTextEditBox({110, 80}, kQuad, r, band, edge) == TextBoxControl::ResizeBR);
    // On the top edge (away from any corner): move.
    CHECK(hitTextEditBox({60, 20}, kQuad, r, band, edge) == TextBoxControl::Move);
    // On the left edge: move.
    CHECK(hitTextEditBox({10, 50}, kQuad, r, band, edge) == TextBoxControl::Move);
    // Deep interior: the caret (None) -- NOT Move, so clicking inside places the caret.
    CHECK(hitTextEditBox({60, 50}, kQuad, r, band, edge) == TextBoxControl::None);
    // Just outside the top-left corner: rotate.
    CHECK(hitTextEditBox({4, 14}, kQuad, r, band, edge) == TextBoxControl::Rotate);
    // The top-LEFT corner is NOT the resize handle (only BR is): on the edge there -> Move.
    CHECK(hitTextEditBox({10, 20}, kQuad, r, band, edge) == TextBoxControl::Move);
    // Far away: nothing.
    CHECK(hitTextEditBox({300, 300}, kQuad, r, band, edge) == TextBoxControl::None);
}

TEST_CASE("hitTextEditBox: resizeCorner=3 moves the handle to BL (vertical Point text)") {
    const double r = 7.0, band = 18.0, edge = 5.0;
    // The bottom-LEFT corner now carries the resize handle...
    CHECK(hitTextEditBox({10, 80}, kQuad, r, band, edge, 3) == TextBoxControl::ResizeBR);
    // ...and the bottom-right corner is just frame edge again (Move).
    CHECK(hitTextEditBox({110, 80}, kQuad, r, band, edge, 3) == TextBoxControl::Move);
}

TEST_CASE("move: translation, with Shift locking the dominant axis") {
    TransformGesture g;
    REQUIRE(g.begin(TransformMode::Move, -1, {5, 5}, Affine2D::identity(), kBox10));
    const Affine2D moved = g.transformFor({8, 7}, false, false);
    CHECK(near(moved.apply({0, 0}), {3, 2}));
    const Affine2D locked = g.transformFor({8, 7}, true, false);
    CHECK(near(locked.apply({0, 0}), {3, 0})); // |dx| >= |dy|: y collapses
}

TEST_CASE("move: translation snaps to whole document pixels") {
    TransformGesture g;
    REQUIRE(g.begin(TransformMode::Move, -1, {5, 5}, Affine2D::identity(), kBox10));
    // A sub-pixel drag (delta 3.4, 2.6) rounds to whole pixels (3, 3): a raster move is lossless,
    // keeping content + the transform box on the pixel grid.
    const Affine2D moved = g.transformFor({8.4, 7.6}, false, false);
    CHECK(near(moved.apply({0, 0}), {3, 3}));
    // Shift still locks the dominant axis; the surviving axis is whole-pixel too.
    const Affine2D locked = g.transformFor({8.4, 7.6}, true, false);
    CHECK(near(locked.apply({0, 0}), {3, 0}));
}

TEST_CASE("scale: opposite-corner anchor, uniform, from-centre, and mid handles") {
    TransformGesture g;
    // Grab the BR corner (handle 2) of an identity 10x10 layer at its corner point.
    REQUIRE(g.begin(TransformMode::Scale, 2, {10, 10}, Affine2D::identity(), kBox10));
    const Affine2D half = g.transformFor({5, 5}, false, false);
    CHECK(near(half.apply({10, 10}), {5, 5}));  // the dragged corner follows the cursor
    CHECK(near(half.apply({0, 0}), {0, 0}));    // the anchor (TL) stays put

    const Affine2D uniform = g.transformFor({5, 8}, true, false);
    CHECK(near(uniform.apply({10, 10}), {8, 8})); // dominant factor (0.8) drives both axes

    const Affine2D centred = g.transformFor({12.5, 12.5}, false, true);
    CHECK(near(centred.apply({5, 5}), {5, 5}));      // Alt: the centre is the anchor
    CHECK(near(centred.apply({10, 10}), {12.5, 12.5}));

    // The right-mid handle (5) scales x only; uniform there drives both axes from x.
    TransformGesture m;
    REQUIRE(m.begin(TransformMode::Scale, 5, {10, 5}, Affine2D::identity(), kBox10));
    const Affine2D xOnly = m.transformFor({12, 9}, false, false);
    CHECK(near(xOnly.apply({10, 5}), {12, 5}));
    CHECK(near(xOnly.apply({5, 10}), {6, 10})); // y untouched (sy = 1)
    const Affine2D uni = m.transformFor({12, 9}, true, false);
    CHECK(near(uni.apply({10, 10}), {12, 11})); // s = 1.2 on both axes, anchored at (0, 5)
}

TEST_CASE("scale: off-handle mid grabs scale one axis only (S15 regression)") {
    // Real grabs land within the hit radius, NOT on the handle point: the perpendicular
    // offset must not leak into the other axis (it used to be a near-zero denominator there,
    // exploding the layer). The handle index dictates the axes now.
    TransformGesture g;
    // Top mid (4) of an identity 10x10 layer, grabbed 0.5 right / 0.4 below the handle.
    REQUIRE(g.begin(TransformMode::Scale, 4, {5.5, 0.4}, Affine2D::identity(), kBox10));
    const Affine2D t = g.transformFor({7, 5.2}, false, false); // grab y 0.4 -> 5.2, anchor y 10
    CHECK(near(t.apply({5, 0}), {5, 5}));   // top edge followed: sy = 0.5
    CHECK(near(t.apply({0, 10}), {0, 10})); // bottom edge anchored
    CHECK(near(t.apply({10, 0}), {10, 5})); // x untouched despite the cursor's +1.5 x drift

    const Affine2D uni = g.transformFor({7, 5.2}, true, false); // Shift: y drives both axes
    CHECK(near(uni.apply({10, 10}), {7.5, 10})); // sx = sy = 0.5 about the anchor (5, 10)

    // Left mid (7), grabbed 0.3 right / 0.8 below its handle point: sy must stay exactly 1.
    TransformGesture m;
    REQUIRE(m.begin(TransformMode::Scale, 7, {0.3, 5.8}, Affine2D::identity(), kBox10));
    const Affine2D x = m.transformFor({-2.0, 9.0}, false, false);
    CHECK(near(x.apply({0.3, 5.8}), {-2.0, 5.8})); // the grab follows in x; its y holds
    CHECK(near(x.apply({10, 0}), {10, 0}));        // right edge anchored, y untouched
}

TEST_CASE("gesture anchors on the content rect, not the layer origin (S15 content bounds)") {
    // A document-sized layer whose alpha occupies (20,30)..(60,80): the handles frame that
    // box, so scaling anchors on ITS corners and rotation pivots about ITS centre.
    const Rect content{20, 30, 40, 50};
    TransformGesture g;
    REQUIRE(g.begin(TransformMode::Scale, 2, {60, 80}, Affine2D::identity(), content));
    const Affine2D t = g.transformFor({40, 55}, false, false); // halve both axes
    CHECK(near(t.apply({20, 30}), {20, 30})); // the content's TL is the anchor
    CHECK(near(t.apply({60, 80}), {40, 55})); // the dragged corner follows

    TransformGesture r;
    REQUIRE(r.begin(TransformMode::Rotate, -1, {60, 55}, Affine2D::identity(), content));
    const Affine2D q = r.transformFor({40, 75}, false, false); // +90 degrees
    CHECK(near(q.apply({40, 55}), {40, 55}, 1e-6)); // the content centre is the pivot
    CHECK(near(q.apply({60, 55}), {40, 75}, 1e-6));
}

TEST_CASE("rotate: about the layer centre, Shift snapping to 5 degrees") {
    constexpr double kPi = 3.14159265358979323846;
    TransformGesture g;
    // Identity 10x10 layer: centre (5,5). Grab at (10,5) = angle 0.
    REQUIRE(g.begin(TransformMode::Rotate, -1, {10, 5}, Affine2D::identity(), kBox10));
    const Affine2D quarter = g.transformFor({5, 10}, false, false); // cursor at +90 degrees
    CHECK(near(quarter.apply({10, 5}), {5, 10}, 1e-6));
    CHECK(near(quarter.apply({5, 5}), {5, 5}, 1e-6)); // the pivot holds

    // ~92 degrees with Shift snaps to exactly 90.
    const double a = 92.0 * kPi / 180.0;
    const Vec2 cursor{5.0 + 5.0 * std::cos(a), 5.0 + 5.0 * std::sin(a)};
    CHECK(near(g.transformFor(cursor, true, false).apply({10, 5}), {5, 10}, 1e-6));

    // 5-degree granularity: ~7 deg with Shift snaps to exactly 5 deg (not 0, as a 15 deg step would).
    const auto at = [](double deg) {
        const double r = deg * kPi / 180.0;
        return Vec2{5.0 + 5.0 * std::cos(r), 5.0 + 5.0 * std::sin(r)};
    };
    const Affine2D snap7 = g.transformFor(at(7.0), true, false);
    const Affine2D exact5 = g.transformFor(at(5.0), false, false);
    CHECK(near(snap7.apply({10, 5}), exact5.apply({10, 5}), 1e-6));
}

TEST_CASE("begin rejects degenerate setups") {
    TransformGesture g;
    CHECK_FALSE(g.begin(TransformMode::Move, -1, {0, 0}, Affine2D::identity(), Rect{0, 0, 0, 10}));
    CHECK_FALSE(g.begin(TransformMode::Scale, 0, {0, 0}, Affine2D::scaling(0, 0), kBox10));
    CHECK_FALSE(g.begin(TransformMode::Rotate, -1, {5, 5}, Affine2D::identity(), kBox10));
    CHECK_FALSE(g.active());
    REQUIRE(g.begin(TransformMode::Move, -1, {0, 0}, Affine2D::identity(), kBox10));
    CHECK(g.active());
    g.cancel();
    CHECK_FALSE(g.active());
}

// ---- The rotate-affordance dots (user 2026-07-14) ---------------------------------------------
// Invisible corner rings are findable on a plain rectangle and unfindable on a sheared,
// foreshortened, or displaced one -- so the dots' alpha must be exactly ZERO for every plain
// rotated/scaled rectangle (nothing changes on the boxes everyone knows) and must GROW with shear,
// with thinness, and with the rotate quad floating off the visible chrome.

TEST_CASE("transformQuadWackiness: zero for every plain rectangle, whatever its rotation") {
    CHECK(transformQuadWackiness(kQuad) == doctest::Approx(0.0).epsilon(1e-9));
    // Rotated by an arbitrary angle about its centre: still a rectangle, still zero.
    const double a = 0.6;
    const Vec2 c{60, 50};
    std::array<Vec2, 4> rot{};
    for (int i = 0; i < 4; ++i) {
        const Vec2 d = kQuad[static_cast<std::size_t>(i)] - c;
        rot[static_cast<std::size_t>(i)] = {c.x + d.x * std::cos(a) - d.y * std::sin(a),
                                            c.y + d.x * std::sin(a) + d.y * std::cos(a)};
    }
    CHECK(transformQuadWackiness(rot) < 1e-9);
    // ... and scaling changes nothing either (the metric is about SHAPE, not size).
    std::array<Vec2, 4> big{};
    for (int i = 0; i < 4; ++i)
        big[static_cast<std::size_t>(i)] = kQuad[static_cast<std::size_t>(i)] * 7.0;
    CHECK(transformQuadWackiness(big) < 1e-9);
}

TEST_CASE("transformQuadWackiness: shear, slivers and collapse all register") {
    // A 45-degree shear: corner angles are 45/135, a full 45-degree mean deviation -> 1.0.
    const std::array<Vec2, 4> sheared{{{0, 0}, {100, 0}, {160, 60}, {60, 60}}};
    CHECK(transformQuadWackiness(sheared) == doctest::Approx(1.0).epsilon(1e-6));
    // A mild shear registers, smaller -- and monotonically less than a strong one.
    const std::array<Vec2, 4> mild{{{0, 0}, {100, 0}, {110, 60}, {10, 60}}};
    const double m = transformQuadWackiness(mild);
    CHECK(m > 0.05);
    CHECK(m < transformQuadWackiness(sheared));
    // A sliver: perfect right angles, but the corners hide along a line (a solid seen edge-on).
    const std::array<Vec2, 4> sliver{{{0, 0}, {300, 0}, {300, 6}, {0, 6}}};
    CHECK(transformQuadWackiness(sliver) > 0.5);
    // Collapse: a point, and a quad with a zero edge, are maximally unfindable.
    const std::array<Vec2, 4> point{{{5, 5}, {5, 5}, {5, 5}, {5, 5}}};
    CHECK(transformQuadWackiness(point) == doctest::Approx(1.0));
    const std::array<Vec2, 4> zeroEdge{{{0, 0}, {0, 0}, {100, 60}, {0, 60}}};
    CHECK(transformQuadWackiness(zeroEdge) == doctest::Approx(1.0));
}

TEST_CASE("transformQuadMismatch: zero when coincident, saturating as the quads separate") {
    CHECK(transformQuadMismatch(kQuad, kQuad) == doctest::Approx(0.0));
    // Every corner displaced by d: mismatch = 2d / diagonal (kQuad's diagonal ~116.6).
    const Vec2 d{20, 0};
    std::array<Vec2, 4> moved{};
    for (int i = 0; i < 4; ++i)
        moved[static_cast<std::size_t>(i)] = kQuad[static_cast<std::size_t>(i)] + d;
    const double diag = (kQuad[2] - kQuad[0]).length();
    CHECK(transformQuadMismatch(kQuad, moved) == doctest::Approx(2.0 * 20.0 / diag).epsilon(1e-6));
    // Far apart: clamped to 1, never past it.
    std::array<Vec2, 4> far{};
    for (int i = 0; i < 4; ++i)
        far[static_cast<std::size_t>(i)] = kQuad[static_cast<std::size_t>(i)] + Vec2{5000, 0};
    CHECK(transformQuadMismatch(kQuad, far) == doctest::Approx(1.0));
    // The 3D-text shape of the problem: the rotate quad BALLOONS around the visible box (the
    // projected solid extent vs the cap) -- corners displaced outward register even though the
    // centres coincide.
    std::array<Vec2, 4> ballooned{};
    const Vec2 ctr{60, 50};
    for (int i = 0; i < 4; ++i)
        ballooned[static_cast<std::size_t>(i)] =
            ctr + (kQuad[static_cast<std::size_t>(i)] - ctr) * 3.0;
    CHECK(transformQuadMismatch(ballooned, kQuad) > 0.3);
}

TEST_CASE("rotateDotOpacity: invisible at zero, 50% ceiling, monotone between") {
    CHECK(rotateDotOpacity(0.0) == doctest::Approx(0.0));
    CHECK(rotateDotOpacity(0.25) == doctest::Approx(0.25));
    CHECK(rotateDotOpacity(0.5) == doctest::Approx(0.5));
    CHECK(rotateDotOpacity(1.0) == doctest::Approx(0.5)); // the ceiling: never louder than 50%
                                                          // (the user's tuning, up from 25%)
    double prev = -1.0;
    for (double w = 0.0; w <= 1.0; w += 0.05) {
        const double o = rotateDotOpacity(w);
        CHECK(o >= prev);
        prev = o;
    }
}
