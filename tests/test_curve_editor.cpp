// The response-curve editor (docs/brushes.md §8.3's net-new widget), driven headlessly.
//
// An Fl_Widget needs no display to exist or to handle an event -- only draw() would -- and Fl::e_x /
// e_y / e_keysym are public, so the whole interaction surface is testable without a window. Which
// matters: the bug-prone part of a direct-manipulation editor is not the drawing, it is the index
// bookkeeping around a point list that Curve is free to SORT and DE-DUPLICATE underneath you.
#include "core/brush/curve.hpp"
#include "ui/curve_editor.hpp"

#include <FL/Fl.H>
#include <FL/Enumerations.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <doctest/doctest.h>

#include <cstdlib>

using mosaic::core::brush::Curve;
using mosaic::ui::CurveEditor;

namespace {

// The widget's own geometry, so a test can aim at a curve coordinate. Mirrors curve_editor.cpp's
// kPad; the editor is 8 px inset on every side, y is flipped (1.0 at the top).
constexpr int kW = 216; // -> a 200 px plot
constexpr int kH = 216;
constexpr int kPad = 8;

int pixelX(double cx) { return kPad + static_cast<int>(cx * (kW - 2 * kPad)); }
int pixelY(double cy) { return kPad + static_cast<int>((1.0 - cy) * (kH - 2 * kPad)); }

// Push / drag / release at a curve coordinate. `button` is FL_LEFT_MOUSE or FL_RIGHT_MOUSE.
// Dispatched through Fl_Widget, which is how FLTK itself delivers an event: the override is
// protected (the house widget idiom), the base's handle() is public.
void send(CurveEditor& e, int event) { static_cast<Fl_Widget&>(e).handle(event); }

void press(CurveEditor& e, double cx, double cy, int button = FL_LEFT_MOUSE, int clicks = 0) {
    Fl::e_x = pixelX(cx);
    Fl::e_y = pixelY(cy);
    Fl::e_keysym = FL_Button + button;
    Fl::e_clicks = clicks;
    send(e, FL_PUSH);
}
void dragTo(CurveEditor& e, double cx, double cy) {
    Fl::e_x = pixelX(cx);
    Fl::e_y = pixelY(cy);
    send(e, FL_DRAG);
}
void release(CurveEditor& e) { send(e, FL_RELEASE); }

} // namespace

TEST_CASE("CurveEditor: starts at the identity and round-trips a curve verbatim") {
    CurveEditor e(0, 0, kW, kH);
    CHECK(e.curve().isIdentity());
    CHECK(e.curve().toString() == "0,0;1,1;");

    // An imported preset's curve must survive a visit to the editor byte-exactly -- that is the
    // whole reason the widget edits a Curve rather than its own point type.
    const std::string imported = "0,0;0.25,0.1;0.75,0.9;1,1;";
    e.setCurve(Curve::fromString(imported));
    CHECK(e.curve().toString() == imported);
    CHECK_FALSE(e.curve().isIdentity());
}

TEST_CASE("CurveEditor: seeding does NOT fire the change callback") {
    // Settings seeds on every open. If that wrote back, opening the dialog would persist a curve the
    // user never touched -- and worse, would do it through Curve's own re-serialization.
    CurveEditor e(0, 0, kW, kH);
    int fired = 0;
    e.onChanged([&](const Curve&) { ++fired; });
    e.setCurve(Curve::fromString("0,0;0.5,0.9;1,1;"));
    CHECK(fired == 0);
}

TEST_CASE("CurveEditor: clicking empty space adds a point, and the drag moves THAT point") {
    CurveEditor e(0, 0, kW, kH);
    int fired = 0;
    e.onChanged([&](const Curve&) { ++fired; });

    press(e, 0.5, 0.8); // well off the identity diagonal, so it is not a grab
    REQUIRE(e.curve().points().size() == 3);
    CHECK(fired > 0);
    CHECK(e.curve().points()[1].x == doctest::Approx(0.5).epsilon(0.02));
    CHECK(e.curve().points()[1].y == doctest::Approx(0.8).epsilon(0.02));

    // The gesture continues into a drag without a second click: the new point is the dragged one.
    // (commit() rebuilds the point vector THROUGH Curve, which sorts and de-duplicates, so a drag
    // index computed against the pre-commit vector is exactly how you end up dragging a stranger.)
    dragTo(e, 0.6, 0.2);
    release(e);
    REQUIRE(e.curve().points().size() == 3);
    CHECK(e.curve().points()[1].x == doctest::Approx(0.6).epsilon(0.02));
    CHECK(e.curve().points()[1].y == doctest::Approx(0.2).epsilon(0.02));
}

TEST_CASE("CurveEditor: a dragged point cannot cross or land on its neighbours") {
    // Curve DROPS a point that shares an x with an earlier one (a zero-width interval has no
    // spline). Without the clamp, dragging a point onto its neighbour would delete the point under
    // the cursor, mid-gesture.
    CurveEditor e(0, 0, kW, kH);
    e.setCurve(Curve::fromString("0,0;0.3,0.3;0.7,0.7;1,1;"));
    REQUIRE(e.curve().points().size() == 4);

    press(e, 0.3, 0.3);  // grab the first interior point ...
    dragTo(e, 0.95, 0.5); // ... and try to shove it past the SECOND interior point (x = 0.7)
    release(e);

    REQUIRE(e.curve().points().size() == 4); // nothing was swallowed
    const auto& pts = e.curve().points();
    CHECK(pts[1].x < pts[2].x);              // still on its own side of its neighbour
    CHECK(pts[1].x == doctest::Approx(0.7).epsilon(0.01)); // clamped hard against it
    CHECK(pts[2].x == doctest::Approx(0.7).epsilon(0.001)); // ... which did not move
}

TEST_CASE("CurveEditor: the endpoints are pinned in x and move only in y") {
    // A curve whose domain does not span [0,1] has no meaning for a sensor whose reading is
    // normalized to it: everything past the last knot silently clamps flat.
    CurveEditor e(0, 0, kW, kH);
    press(e, 0.0, 0.0);   // grab the left endpoint
    dragTo(e, 0.5, 0.4);  // drag it right AND up
    release(e);

    const auto& pts = e.curve().points();
    REQUIRE(pts.size() == 2);
    CHECK(pts[0].x == doctest::Approx(0.0)); // x pinned ...
    CHECK(pts[0].y == doctest::Approx(0.4).epsilon(0.02)); // ... y followed
    CHECK(pts[1].x == doctest::Approx(1.0)); // the right endpoint is untouched
}

TEST_CASE("CurveEditor: right-click removes an interior point but never an endpoint") {
    CurveEditor e(0, 0, kW, kH);
    e.setCurve(Curve::fromString("0,0;0.5,0.8;1,1;"));
    REQUIRE(e.curve().points().size() == 3);

    press(e, 0.0, 0.0, FL_RIGHT_MOUSE); // an endpoint: removing it would break the domain
    CHECK(e.curve().points().size() == 3);
    press(e, 1.0, 1.0, FL_RIGHT_MOUSE);
    CHECK(e.curve().points().size() == 3);

    press(e, 0.5, 0.8, FL_RIGHT_MOUSE); // the interior point: gone
    CHECK(e.curve().points().size() == 2);
    CHECK(e.curve().isIdentity()); // ... and what is left is the identity again
}

TEST_CASE("CurveEditor: double-click toggles a point between smooth and corner") {
    CurveEditor e(0, 0, kW, kH);
    e.setCurve(Curve::fromString("0,0;0.5,0.8;1,1;"));
    REQUIRE_FALSE(e.curve().points()[1].corner);

    press(e, 0.5, 0.8, FL_LEFT_MOUSE, /*clicks=*/1); // FLTK reports the 2nd click as e_clicks == 1
    CHECK(e.curve().points()[1].corner);
    // The corner flag is part of the interchange format, so it has to survive serialization too.
    CHECK(e.curve().toString() == Curve::fromString(e.curve().toString()).toString());

    press(e, 0.5, 0.8, FL_LEFT_MOUSE, /*clicks=*/1);
    CHECK_FALSE(e.curve().points()[1].corner);
}

TEST_CASE("CurveEditor: reset returns to the identity and FIRES (it is an edit)") {
    CurveEditor e(0, 0, kW, kH);
    e.setCurve(Curve::fromString("0,0;0.5,0.9;1,1;"));
    int fired = 0;
    e.onChanged([&](const Curve&) { ++fired; });

    e.reset();
    CHECK(fired == 1); // a reset that did not persist would come back on the next open
    CHECK(e.curve().isIdentity());
}

// draw() must own EVERY pixel of its rect, not just the plot. The plot is inset by kPad, and the
// editor shipped painting only that -- so the ring around it kept whatever the pane had underneath,
// which inside a ScrollView is stale scrolled content. (Settings → Tablet showed exactly this.)
//
// Rendering needs a display, so this one is gated; everything above it is pure event bookkeeping.
TEST_CASE("CurveEditor: draw() erases its whole cell, not just the plot") {
#if defined(__SANITIZE_ADDRESS__) ||                                                                \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return; // FLTK/X11 internals leak on teardown under LeakSanitizer; this is not a memory test
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    constexpr mosaic::common::Color8 kGround{20, 30, 40, 255};   // what the editor must clear TO
    constexpr mosaic::common::Color8 kStale{255, 0, 255, 255};   // what is under it beforehand

    CurveEditor e(0, 0, kW, kH);
    e.setCellColor(kGround);

    auto* surf = new Fl_Image_Surface(kW, kH);
    Fl_Surface_Device::push_current(surf);
    fl_color(fl_rgb_color(kStale.r, kStale.g, kStale.b)); // the stale content the widget sits on
    fl_rectf(0, 0, kW, kH);
    surf->draw(&e, 0, 0);
    Fl_Surface_Device::pop_current();

    Fl_RGB_Image* img = surf->image();
    REQUIRE(img != nullptr);
    const auto* px = reinterpret_cast<const unsigned char*>(img->data()[0]);
    const int d = img->d();
    const auto at = [&](int x, int y) {
        const unsigned char* p = px + (static_cast<std::size_t>(y) * kW + x) * d;
        return mosaic::common::Color8{p[0], p[1], p[2], 255};
    };

    // The four corners of the pad ring: inside the widget, outside the plot. Every one of them is
    // the editor's to paint.
    CHECK(at(0, 0) == kGround);
    CHECK(at(kW - 1, 0) == kGround);
    CHECK(at(0, kH - 1) == kGround);
    CHECK(at(kW - 1, kH - 1) == kGround);
    CHECK(at(kPad / 2, kH / 2) == kGround); // mid-height, in the left margin

    delete img;
    delete surf;
}
