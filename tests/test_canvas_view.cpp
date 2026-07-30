#include "ui/canvas_view.hpp"

#include <cmath>
#include <doctest/doctest.h>

using doctest::Approx;
using mosaic::common::Vec2;
using mosaic::ui::CanvasView;

namespace {
constexpr double kPi = 3.14159265358979323846;

// A view of a 400x200 document in an 800x600 viewport (logical pixels).
CanvasView makeView() {
    CanvasView v;
    v.setDocumentSize({400.0, 200.0});
    v.setViewportSize({800.0, 600.0});
    return v;
}

bool near(Vec2 a, Vec2 b, double eps = 1e-6) {
    return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps;
}
} // namespace

TEST_CASE("docToScreen and screenToDoc round-trip") {
    CanvasView v = makeView();
    v.zoomAround({400.0, 300.0}, 2.5);
    v.panByScreen({37.0, -19.0});
    v.rotateBy(0.7);
    const Vec2 p{123.0, 88.0};
    CHECK(near(v.screenToDoc().apply(v.docToScreen().apply(p)), p));
}

TEST_CASE("default places the document centre at the viewport centre") {
    CanvasView v = makeView();
    // zoom 1, no pan/rotation: doc centre (200,100) -> view centre (400,300).
    CHECK(near(v.toScreen({200.0, 100.0}), {400.0, 300.0}));
    CHECK(v.zoom() == Approx(1.0));
}

TEST_CASE("fit centres and scales the document to the viewport") {
    CanvasView v = makeView();
    v.fit();
    // 800/400 = 2.0, 600/200 = 3.0 -> min = 2.0 so the whole doc is visible.
    CHECK(v.zoom() == Approx(2.0));
    CHECK(near(v.toScreen({200.0, 100.0}), {400.0, 300.0})); // centre stays centred
    // Document corners land inside the viewport.
    const Vec2 tl = v.toScreen({0.0, 0.0});
    CHECK(tl.x == Approx(0.0));
    CHECK(tl.y == Approx(300.0 - 200.0)); // 200px tall * 2.0 = 400 -> spans y in [100,500]
}

TEST_CASE("zoomAround keeps the doc point under the cursor fixed") {
    CanvasView v = makeView();
    const Vec2 cursor{650.0, 120.0};
    const Vec2 docUnder = v.toDoc(cursor);
    v.zoomAround(cursor, 4.0);
    CHECK(v.zoom() == Approx(4.0));
    CHECK(near(v.toScreen(docUnder), cursor, 1e-5)); // same doc point still under the cursor
}

TEST_CASE("zoom is clamped to the configured range") {
    CanvasView v = makeView();
    v.zoomAround({400.0, 300.0}, 1.0e9);
    CHECK(v.zoom() == Approx(CanvasView::kMaxZoom));
    v.zoomAround({400.0, 300.0}, 1.0e-9);
    CHECK(v.zoom() == Approx(CanvasView::kMinZoom));
}

TEST_CASE("panByScreen moves a doc point by the screen delta (unrotated)") {
    CanvasView v = makeView();
    const Vec2 before = v.toScreen({200.0, 100.0});
    v.panByScreen({30.0, -40.0});
    CHECK(near(v.toScreen({200.0, 100.0}), before + Vec2{30.0, -40.0}));
}

TEST_CASE("panByScreen accounts for rotation (screen-space drag)") {
    CanvasView v = makeView();
    v.setRotation(kPi / 2.0); // 90 degrees
    const Vec2 before = v.toScreen({200.0, 100.0});
    v.panByScreen({50.0, 0.0});
    // Dragging right on screen should move the content right on screen regardless of rotation.
    CHECK(near(v.toScreen({200.0, 100.0}), before + Vec2{50.0, 0.0}, 1e-5));
}

TEST_CASE("actualPixels sets 100% and re-centres") {
    CanvasView v = makeView();
    v.zoomAround({10.0, 10.0}, 8.0);
    v.panByScreen({100.0, 100.0});
    v.actualPixels();
    CHECK(v.zoom() == Approx(1.0));
    CHECK(near(v.toScreen({200.0, 100.0}), {400.0, 300.0}));
}

TEST_CASE("rotationDegrees normalises to (-180, 180]") {
    CanvasView v = makeView();
    v.setRotation(kPi / 4.0);
    CHECK(v.rotationDegrees() == Approx(45.0));
    v.setRotation(3.0 * kPi); // 540 deg -> 180
    CHECK(v.rotationDegrees() == Approx(180.0));
    v.setRotation(-kPi / 2.0);
    CHECK(v.rotationDegrees() == Approx(-90.0));
    v.resetRotation();
    CHECK(v.rotationDegrees() == Approx(0.0));
}

TEST_CASE("fit accounts for rotation so the rotated page still fits") {
    CanvasView v = makeView();
    v.setRotation(kPi / 2.0); // 90 deg: doc bounds become 200 wide x 400 tall
    v.fit();
    // 800/200 = 4.0, 600/400 = 1.5 -> min 1.5.
    CHECK(v.zoom() == Approx(1.5));
    // All four corners remain within the viewport after rotation.
    for (Vec2 corner : {Vec2{0, 0}, Vec2{400, 0}, Vec2{0, 200}, Vec2{400, 200}}) {
        const Vec2 s = v.toScreen(corner);
        CHECK(s.x >= -1e-6);
        CHECK(s.x <= 800.0 + 1e-6);
        CHECK(s.y >= -1e-6);
        CHECK(s.y <= 600.0 + 1e-6);
    }
}
