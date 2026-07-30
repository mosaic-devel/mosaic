#include "ui/scrub_slider.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <string>

// The pure value model behind the options-bar ScrubSlider (ui::scrub_detail). GUI-free, so the
// curve mapping, local sensitivity, precision ramp, snap and formatting are all exercised here
// without an X server; the widget gesture + ruler are verified interactively by the user.
namespace {

using namespace mosaic::ui;
using namespace mosaic::ui::scrub_detail;

constexpr double kEps = 1e-9;

} // namespace

TEST_CASE("curveToValue/valueToTrack round-trip across curves") {
    struct Case {
        ScrubCurve curve;
        double k, mn, mx;
    };
    const Case cases[] = {
        {ScrubCurve::Linear, 1.0, 1.0, 1000.0},
        {ScrubCurve::Gamma, 2.0, 1.0, 1000.0},
        {ScrubCurve::Gamma, 3.0, 0.5, 400.0},
        {ScrubCurve::Log, 1.0, 1.0, 1024.0},
    };
    for (const Case& c : cases) {
        for (double t = 0.0; t <= 1.0 + kEps; t += 0.05) {
            const double v = curveToValue(t, c.curve, c.k, c.mn, c.mx);
            CHECK(v >= c.mn - 1e-6);
            CHECK(v <= c.mx + 1e-6);
            const double back = valueToTrack(v, c.curve, c.k, c.mn, c.mx);
            CHECK(back == doctest::Approx(t).epsilon(1e-6));
        }
    }
}

TEST_CASE("curve endpoints map to the range ends") {
    CHECK(curveToValue(0.0, ScrubCurve::Gamma, 2.0, 1.0, 1000.0) == doctest::Approx(1.0));
    CHECK(curveToValue(1.0, ScrubCurve::Gamma, 2.0, 1.0, 1000.0) == doctest::Approx(1000.0));
    CHECK(curveToValue(0.0, ScrubCurve::Log, 1.0, 2.0, 512.0) == doctest::Approx(2.0));
    CHECK(curveToValue(1.0, ScrubCurve::Log, 1.0, 2.0, 512.0) == doctest::Approx(512.0));
}

TEST_CASE("Gamma front-loads the low end of the track") {
    // The midpoint of the track sits well below the linear midpoint, so small values get more room.
    const double mid = curveToValue(0.5, ScrubCurve::Gamma, 2.0, 1.0, 1000.0);
    CHECK(mid < 500.0);
    CHECK(mid == doctest::Approx(1.0 + 999.0 * 0.25)); // 1 + (mx-mn)*0.5^2
}

TEST_CASE("Log falls back to Linear for a non-positive range") {
    // Log needs mn>0; a 0..100 range must not produce NaN -- it degrades to linear.
    const double v = curveToValue(0.5, ScrubCurve::Log, 1.0, 0.0, 100.0);
    CHECK(std::isfinite(v));
    CHECK(v == doctest::Approx(50.0));
}

TEST_CASE("unitsPerPixel: finer at the low end of a Gamma curve, always positive") {
    const double mn = 1.0, mx = 1000.0;
    const int track = 150;
    const double low = unitsPerPixel(2.0, ScrubCurve::Gamma, 2.0, mn, mx, track);
    const double high = unitsPerPixel(900.0, ScrubCurve::Gamma, 2.0, mn, mx, track);
    CHECK(low > 0.0);
    CHECK(high > low); // coarser (more units per pixel) up high
    // Even pinned at the very bottom (derivative -> 0) it never returns zero, so the drag can move.
    const double atFloor = unitsPerPixel(mn, ScrubCurve::Gamma, 2.0, mn, mx, track);
    CHECK(atFloor > 0.0);
}

TEST_CASE("unitsPerPixel: linear curve ~ range/width") {
    const double upp = unitsPerPixel(500.0, ScrubCurve::Linear, 1.0, 0.0, 1000.0, 100);
    CHECK(upp == doctest::Approx(10.0).epsilon(0.05)); // 1000 units over 100 px
}

TEST_CASE("precision01: zero in the deadzone, ramps then saturates, monotonic") {
    CHECK(precision01(0, 24, 360) == 0.0);
    CHECK(precision01(24, 24, 360) == 0.0);
    CHECK(precision01(204, 24, 360) == doctest::Approx(0.5)); // halfway up the ramp
    CHECK(precision01(384, 24, 360) == doctest::Approx(1.0));
    CHECK(precision01(900, 24, 360) == 1.0); // clamped
    double prev = -1.0;
    for (int dy = 0; dy < 500; dy += 10) {
        const double p = precision01(dy, 24, 360);
        CHECK(p >= prev);
        prev = p;
    }
}

TEST_CASE("gainDivisor: 1 with no precision, eases up to maxDiv, monotonic") {
    CHECK(gainDivisor(0.0, 24.0) == doctest::Approx(1.0));
    CHECK(gainDivisor(1.0, 24.0) == doctest::Approx(24.0));
    CHECK(gainDivisor(0.5, 24.0) > 1.0);
    CHECK(gainDivisor(0.5, 24.0) < 24.0);
    double prev = 0.0;
    for (double p = 0.0; p <= 1.0 + kEps; p += 0.05) {
        const double d = gainDivisor(p, 24.0);
        CHECK(d >= prev);
        prev = d;
    }
}

TEST_CASE("snapTo rounds to the grid; <=0 is identity") {
    CHECK(snapTo(2.7, 0.5) == doctest::Approx(2.5));
    CHECK(snapTo(2.8, 0.5) == doctest::Approx(3.0));
    CHECK(snapTo(2.0, 0.5) == doctest::Approx(2.0));
    CHECK(snapTo(2.7, 0.0) == doctest::Approx(2.7));
    CHECK(snapTo(2.7, -1.0) == doctest::Approx(2.7));
}

TEST_CASE("quantize rounds to step and clamps to range") {
    CHECK(quantize(2.74, 0.1, 0.5, 1000.0) == doctest::Approx(2.7));
    CHECK(quantize(2.76, 0.1, 0.5, 1000.0) == doctest::Approx(2.8));
    CHECK(quantize(-5.0, 0.1, 0.5, 1000.0) == doctest::Approx(0.5)); // clamp low
    CHECK(quantize(5000.0, 0.1, 0.5, 1000.0) == doctest::Approx(1000.0)); // clamp high
    CHECK(quantize(7.3, 0.0, 0.0, 100.0) == doctest::Approx(7.3)); // step<=0 -> just clamp
}

TEST_CASE("format: integer steps drop decimals, fine steps show minimal decimals") {
    CHECK(format(24.0, 1.0, "px") == "24px");
    CHECK(format(2.5, 0.1, "px") == "2.5px");   // trailing zero trimmed
    CHECK(format(80.0, 1.0, "%") == "80%");
    CHECK(format(2.0, 0.1, "px") == "2px");     // whole value: no stray ".0"
    CHECK(format(1000.0, 0.1, "px") == "1000px"); // large value stays compact
    CHECK(format(0.25, 0.05, "") == "0.25");
}

TEST_CASE("integrated relative drag reaches a fine target without jumping") {
    // Simulate a precision drag: integrate dx*gain into a continuous accumulator, the way
    // ScrubSlider::handle(FL_DRAG) does, and confirm we can land on a sub-step value smoothly.
    const ScrubCurve curve = ScrubCurve::Gamma;
    const double k = 2.0, mn = 0.5, mx = 1000.0, step = 0.1;
    const int track = 150;
    double accum = 24.0; // start value
    const double div = gainDivisor(precision01(200, 24, 360), 24.0); // well into precision
    double last = 0.0;
    // Drag left 30 px in 1-px steps.
    for (int i = 1; i <= 30; ++i) {
        const double upp = unitsPerPixel(accum, curve, k, mn, mx, track);
        accum += (-1.0) * (upp / div);
        last = static_cast<double>(i);
    }
    (void)last;
    CHECK(accum < 24.0);            // moved down
    CHECK(accum > mn);              // stayed in range
    const double shown = quantize(accum, step, mn, mx);
    CHECK(std::fabs(shown - accum) <= step); // display tracks the accumulator within one step
}
