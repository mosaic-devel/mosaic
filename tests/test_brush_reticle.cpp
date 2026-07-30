#include <doctest/doctest.h>

#include "ui/brush_reticle.hpp"

#include <cmath>
#include <initializer_list>

// The brush reticle's screen geometry (docs/brushes.md §6.3): the ring traces the TIP'S SHAPE at its
// configured size. The shader that draws the ellipse cannot be tested; this half can, and it is the
// half that goes wrong quietly.
namespace {

using mosaic::ui::reticleShape;
using mosaic::ui::reticleShapeFromExtent;
using mosaic::ui::ReticleShape;
using mosaic::ui::reticleTracesTip;

constexpr double kPi = 3.14159265358979323846;

// The shader's tipDist(), mirrored (shaders/canvas_present.comp). A parity lane, exactly as
// extrude_render mirrors extrude_raster.comp: the GLSL is unreachable from a test, so the formula is
// pinned here and any divergence is a divergence from something that HAS a test.
[[nodiscard]] double tipDist(double dx, double dy, double a, double b, double theta) {
    const double c = std::cos(theta);
    const double s = std::sin(theta);
    const double qx = dx * c + dy * s;
    const double qy = -dx * s + dy * c;
    const double k = a / b;
    const double le = std::fmax(std::sqrt(qx * qx + (qy * k) * (qy * k)), 1e-6);
    const double grad = std::fmax(std::sqrt(qx * qx + (qy * k * k) * (qy * k * k)) / le, 1e-6);
    return (le - a) / grad;
}

} // namespace

TEST_CASE("reticle: a round tip is a circle of exactly the brush's radius") {
    const ReticleShape s = reticleShape(/*diameter=*/24.0, /*ratio=*/1.0, /*angleRad=*/0.0,
                                        /*zoom=*/1.0, /*viewRotation=*/0.0);
    CHECK(s.semiX == 12.0);
    CHECK(s.semiY == 12.0);
    CHECK(s.angleRad == 0.0);
}

TEST_CASE("reticle: the zoom scales both axes, and cannot shear a circle into an ellipse") {
    // The view's zoom is ISOTROPIC (canvas_view.hpp: docToScreen = Rc * (T * S * T)), so a round tip
    // stays round at every zoom. A reticle that applied it to one axis would be an ellipse the dab
    // never lays.
    const ReticleShape s = reticleShape(24.0, 1.0, 0.0, 2.5, 0.0);
    CHECK(s.semiX == 30.0);
    CHECK(s.semiY == 30.0);

    // ... and a shaped tip keeps its aspect through the zoom.
    const ReticleShape n = reticleShape(40.0, 0.25, 0.0, 2.0, 0.0);
    CHECK(n.semiX == 40.0); // 0.5 * 40 * 2
    CHECK(n.semiY == 10.0); // ... * 0.25
    CHECK(n.semiY / n.semiX == doctest::Approx(0.25));
}

TEST_CASE("reticle: a bitmap tip's box is its FRAME's -- not diameter x diameter*ratio") {
    // ⚠ `diameter` sets a bitmap tip's LONG axis and the frame's own aspect fills in the rest
    // (core::brush::tipDabShape). A 300x80 stamp at diameter 300 paints a dab 300 x 80 -- so the
    // diameter-and-ratio form, which would call it 300 x 300, rings it with a box nearly four times
    // too tall. The extent form is the one a real tip must go through.
    const ReticleShape s = reticleShapeFromExtent(/*extentW=*/300.0, /*extentH=*/80.0,
                                                  /*angleRad=*/0.0, /*zoom=*/1.0,
                                                  /*viewRotation=*/0.0);
    CHECK(s.semiX == 150.0);
    CHECK(s.semiY == 40.0);

    // The diameter/ratio form is exactly the extent form of a tip that fills its ellipse -- which is
    // what a NULL tip and a procedural generator do, and what every brush that predates the preset
    // library is.
    const ReticleShape n = reticleShape(24.0, 0.5, 0.3, 2.0, 0.1);
    const ReticleShape e = reticleShapeFromExtent(24.0, 12.0, 0.3, 2.0, 0.1);
    CHECK(n.semiX == e.semiX);
    CHECK(n.semiY == e.semiY);
    CHECK(n.angleRad == e.angleRad);
}

TEST_CASE("reticle: a tip too small on screen to have a shape does not trace one") {
    // Below a couple of pixels a traced contour is a blob: it says nothing about the tip's shape and
    // less about its size than a plain ring does. The analytic ellipse -- which the shader floors at
    // a half pixel -- is the better answer, and it still says WHERE the brush is.
    CHECK_FALSE(reticleTracesTip(reticleShape(2.0, 1.0, 0.0, /*zoom=*/1.0, 0.0)));  // 1 px semi-axis
    CHECK(reticleTracesTip(reticleShape(24.0, 1.0, 0.0, 1.0, 0.0)));                // 12 px
    // A zoomed-out big brush stops tracing; the SAME brush zoomed in traces again -- and the field
    // itself is not rebuilt either way (it lives in the tip's own frame).
    CHECK_FALSE(reticleTracesTip(reticleShape(60.0, 1.0, 0.0, /*zoom=*/0.02, 0.0)));
    CHECK(reticleTracesTip(reticleShape(60.0, 1.0, 0.0, /*zoom=*/1.0, 0.0)));
    // A thin nib is judged on its SHORT axis: a hair-thin ellipse has no traceable shape either.
    CHECK_FALSE(reticleTracesTip(reticleShape(80.0, 0.01, 0.0, 1.0, 0.0)));
}

TEST_CASE("reticle: the view's rotation ADDS to the tip's own angle") {
    // Rotating the CANVAS turns everything drawn on it, the nib included -- so the ring has to turn
    // with it or it stops tracing the tip the moment the user rotates the view. The transform has no
    // mirror term, so nothing flips the sense.
    const ReticleShape s = reticleShape(20.0, 0.5, /*angleRad=*/0.25, /*zoom=*/1.0,
                                        /*viewRotation=*/kPi * 0.5);
    CHECK(s.angleRad == doctest::Approx(0.25 + kPi * 0.5));

    // A tip with no angle of its own still turns with the canvas.
    CHECK(reticleShape(20.0, 0.5, 0.0, 1.0, 1.0).angleRad == doctest::Approx(1.0));
    // ... and an unrotated canvas leaves the tip's own angle alone, exactly.
    CHECK(reticleShape(20.0, 0.5, 0.75, 1.0, 0.0).angleRad == 0.75);
}

TEST_CASE("reticle: a degenerate tip still shows where the brush is") {
    // A preset is an untrusted file: a ratio of 0 is legal and paints nothing, and a size of 0 or a
    // NaN zoom must not produce a ring with no position. Locating the cursor is the more useful of
    // the reticle's two jobs once the other has broken down.
    for (const ReticleShape s :
         {reticleShape(0.0, 1.0, 0.0, 1.0, 0.0), reticleShape(24.0, 0.0, 0.0, 1.0, 0.0),
          reticleShape(24.0, 1.0, 0.0, 0.0, 0.0),
          reticleShape(std::nan(""), std::nan(""), std::nan(""), std::nan(""), std::nan(""))}) {
        CHECK(s.semiX >= 0.5);
        CHECK(s.semiY >= 0.5);
        CHECK(std::isfinite(s.semiX));
        CHECK(std::isfinite(s.semiY));
        CHECK(std::isfinite(s.angleRad));
    }
}

TEST_CASE("reticle: the shader's tip distance is EXACT for a circle") {
    // ⚠ The claim the whole shader change rests on. Every brush that ships today is round, so the
    // ring's pixel-tuned antialiasing must come out bit-for-bit what it was -- which it does only if
    // the ellipse formula degenerates to `length(d) - R` EXACTLY, not merely closely. It does,
    // because at a == b the squash factor is exactly 1.0, the metric correction divides by exactly
    // 1.0, and a rotation by 0 is the identity (cos 0 == 1, sin 0 == 0).
    for (const double R : {0.5, 3.0, 12.0, 47.5, 300.0}) {
        for (const double dx : {-31.0, -7.25, 0.0, 1.5, 19.0}) {
            for (const double dy : {-22.0, -0.5, 0.0, 4.75, 28.0}) {
                if (dx == 0.0 && dy == 0.0)
                    continue; // the centre is the one point with no direction to a rim
                const double exact = std::sqrt(dx * dx + dy * dy) - R;
                CHECK(tipDist(dx, dy, R, R, 0.0) == exact); // bit-for-bit, not Approx
            }
        }
    }
}

TEST_CASE("reticle: the shader's tip distance vanishes ON the ellipse and grows off it") {
    // The zero level set IS the tip's outline: walk the ellipse's own parameterization and the
    // distance must read zero all the way round -- at every angle, including the two where a naive
    // "squash it to a circle and measure there" formula is worst (the ends of the SHORT axis, where
    // that map stretches distance by a/b and would thin the ring out).
    const double a = 40.0;
    const double b = 10.0;
    for (const double theta : {0.0, 0.6, kPi * 0.5, 2.5, -1.2}) {
        for (int i = 0; i < 32; ++i) {
            const double t = kPi * 2.0 * i / 32.0;
            // A point on the ellipse, in the tip's frame, turned into the screen frame.
            const double ex = a * std::cos(t);
            const double ey = b * std::sin(t);
            const double dx = ex * std::cos(theta) - ey * std::sin(theta);
            const double dy = ex * std::sin(theta) + ey * std::cos(theta);
            CHECK(std::fabs(tipDist(dx, dy, a, b, theta)) < 0.02);
        }
    }

    // Just outside the SHORT axis' end, the distance is ~the offset -- not the offset stretched by
    // a/b (which is 4x here, and would have drawn a ring a quarter of its proper width there).
    CHECK(tipDist(0.0, b + 1.0, a, b, 0.0) == doctest::Approx(1.0).epsilon(0.12));
    // ... and just outside the LONG axis' end, likewise.
    CHECK(tipDist(a + 1.0, 0.0, a, b, 0.0) == doctest::Approx(1.0).epsilon(0.12));
    // Inside is negative, outside positive.
    CHECK(tipDist(0.0, 0.0, a, b, 0.0) < 0.0);
    CHECK(tipDist(a * 3.0, 0.0, a, b, 0.0) > 0.0);
}

// ---- the hover heading -------------------------------------------------------------------------
//
// The direction a direction-following tip turns to, when there is no stroke to read one from.

TEST_CASE("HoverHeading: a pointer that has not moved has NO direction, not a direction of zero") {
    mosaic::ui::HoverHeading h;
    CHECK(std::isnan(h.headingRad()));

    // Entering the canvas arms it; it does not invent a heading out of one position.
    h.moveTo(100.0, 100.0, 10.0, 10.0);
    CHECK(std::isnan(h.headingRad()));

    // Nor does a wobble under the travel threshold. This is the case that matters: a mouse reports
    // INTEGER positions at 60 Hz and a hand resting on a stylus jitters by a pixel. A raw per-event
    // atan2 would spin the tip on the spot.
    h.moveTo(101.0, 100.0, 10.1, 10.0);
    h.moveTo(100.0, 101.0, 10.0, 10.1);
    h.moveTo(101.0, 101.0, 10.1, 10.1);
    CHECK(std::isnan(h.headingRad()));
}

TEST_CASE("HoverHeading: the heading is measured in the DOCUMENT, the travel on the SCREEN") {
    // ⚠ The two frames are not an accident and they are not interchangeable -- see the header. This
    // case drives them APART deliberately: a view zoomed 10x and rotated a quarter turn, so a screen
    // delta and a document delta point in different directions AND have different lengths. A single
    // -frame implementation cannot pass both halves.
    mosaic::ui::HoverHeading h;

    // Travel 10 px EAST on screen; the document, under a quarter-turn view, went SOUTH.
    h.moveTo(0.0, 0.0, 0.0, 0.0);
    h.moveTo(10.0, 0.0, 0.0, 1.0);
    CHECK(h.headingRad() == doctest::Approx(kPi / 2.0)); // atan2(+1, 0): the DOCUMENT's heading

    // Now a big DOCUMENT move that is a tiny SCREEN one (zoomed far out): it must NOT register --
    // the pointer barely moved, so whatever it did was noise.
    mosaic::ui::HoverHeading z;
    z.moveTo(0.0, 0.0, 0.0, 0.0);
    z.moveTo(1.0, 0.0, 500.0, 0.0); // 1 screen px, 500 document px
    CHECK(std::isnan(z.headingRad()));
}

TEST_CASE("HoverHeading: it holds its last direction between updates, and re-anchors when it moves") {
    mosaic::ui::HoverHeading h;
    h.moveTo(0.0, 0.0, 0.0, 0.0);
    h.moveTo(10.0, 0.0, 10.0, 0.0);
    CHECK(h.headingRad() == doctest::Approx(0.0)); // due east

    // A sub-threshold wobble does not blank the heading -- it keeps the last good one.
    h.moveTo(11.0, 0.0, 11.0, 0.0);
    CHECK(h.headingRad() == doctest::Approx(0.0));

    // A real move re-aims it. ⚠ And it re-anchors from where it LAST FIRED, not from the wobble:
    // if the anchor crept along with every sub-threshold event, the threshold would be defeated by
    // a slow drift, one pixel at a time.
    h.moveTo(10.0, 10.0, 10.0, 10.0); // 10 px south of the anchor at (10, 0)
    CHECK(h.headingRad() == doctest::Approx(kPi / 2.0));

    // Leaving the canvas forgets it: coming back must not resume a direction the pointer is no
    // longer travelling in.
    h.reset();
    CHECK(std::isnan(h.headingRad()));
}
