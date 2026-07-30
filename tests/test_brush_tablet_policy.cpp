#include <doctest/doctest.h>

#include "core/brush/curve.hpp"
#include "core/brush/stroke_state.hpp"
#include "core/brush/tablet_policy.hpp"

#include <cmath>
#include <iterator>

// The tablet policy layer (docs/tablet.md §7): pressure range remap + response curve LUT, the
// tilt-direction offset, and the SpeedSmoother component. All headless, canned streams only (§9).

using mosaic::core::brush::additiveToScaling;
using mosaic::core::brush::Curve;
using mosaic::core::brush::Sensor;
using mosaic::core::brush::SensorId;
using mosaic::core::brush::sensorValue;
using mosaic::core::brush::SpeedParams;
using mosaic::core::brush::SpeedSmoother;
using mosaic::core::brush::StrokeInput;
using mosaic::core::brush::StrokeState;
using mosaic::core::brush::TabletPolicy;
using mosaic::core::brush::wrapValue;

namespace {

// The ascension bearing in [0,1] that the sensors derive from a tilt pair, read through the real
// sensor path so the policy<->sensor contract (offset applied at ingest, never in the sensor) is
// what is actually tested.
[[nodiscard]] double ascension01(double xTilt, double yTilt) {
    StrokeInput in;
    in.xTilt = xTilt;
    in.yTilt = yTilt;
    StrokeState state;
    state.begin(in, 1);
    return additiveToScaling(sensorValue(Sensor::withDefaults(SensorId::Ascension), state, "Size"));
}

[[nodiscard]] double declination01(double xTilt, double yTilt) {
    StrokeInput in;
    in.xTilt = xTilt;
    in.yTilt = yTilt;
    StrokeState state;
    state.begin(in, 1);
    return sensorValue(Sensor::withDefaults(SensorId::Declination), state, "Size");
}

} // namespace

TEST_CASE("tablet policy: the default policy is a byte-exact pass-through") {
    const TabletPolicy policy;
    CHECK(policy.isIdentity());

    // == on purpose, not Approx: the default path must not even round-trip through the LUT.
    for (const double p : {0.0, 0.125, 0.3, 0.5, 0.7071067811865476, 1.0})
        CHECK(policy.applyPressure(p) == p);

    StrokeInput in;
    in.pos = {12.25, 7.5};
    in.pressure = 0.3;
    in.xTilt = 17.3;
    in.yTilt = -42.1;
    in.rotation = 33.0;
    in.tangentialPressure = 0.6;
    in.timeUs = 987654;
    const StrokeInput out = policy.apply(in);
    CHECK(out.pos.x == in.pos.x);
    CHECK(out.pos.y == in.pos.y);
    CHECK(out.pressure == in.pressure);
    CHECK(out.xTilt == in.xTilt);
    CHECK(out.yTilt == in.yTilt);
    CHECK(out.rotation == in.rotation);
    CHECK(out.tangentialPressure == in.tangentialPressure);
    CHECK(out.timeUs == in.timeUs);
}

TEST_CASE("tablet policy: raw pressure outside [0,1] is clamped before the pipeline") {
    const TabletPolicy policy;
    CHECK(policy.applyPressure(-0.5) == 0.0);
    CHECK(policy.applyPressure(1.5) == 1.0);
}

TEST_CASE("tablet policy: the range remap stretches [min,max] to the full [0,1]") {
    TabletPolicy policy;
    policy.setPressureRange(0.2, 0.8);
    CHECK_FALSE(policy.isIdentity());
    CHECK(policy.applyPressure(0.1) == 0.0);  // below min: a worn nib's floor reads as zero
    CHECK(policy.applyPressure(0.2) == 0.0);
    CHECK(policy.applyPressure(0.5) == doctest::Approx(0.5)); // midpoint stays the midpoint
    CHECK(policy.applyPressure(0.8) == 1.0);
    CHECK(policy.applyPressure(0.95) == 1.0); // above max: full pressure without bottoming out
}

TEST_CASE("tablet policy: a degenerate range is a threshold and never a division") {
    TabletPolicy policy;
    policy.setPressureRange(0.5, 0.5);
    CHECK(policy.applyPressure(0.49) == 0.0);
    CHECK(policy.applyPressure(0.5) == 1.0);
    CHECK(policy.applyPressure(0.51) == 1.0);
    CHECK(std::isfinite(policy.applyPressure(0.5)));
}

TEST_CASE("tablet policy: the range setter normalizes hostile bounds") {
    TabletPolicy policy;
    policy.setPressureRange(0.8, 0.2); // inverted: swapped, not honored upside down
    CHECK(policy.pressureMin() == 0.2);
    CHECK(policy.pressureMax() == 0.8);

    policy.setPressureRange(-3.0, 42.0); // out of range: clamped back to the identity span
    CHECK(policy.pressureMin() == 0.0);
    CHECK(policy.pressureMax() == 1.0);
    CHECK(policy.isIdentity());
}

TEST_CASE("tablet policy: the response curve is applied through the baked LUT") {
    TabletPolicy policy;
    const Curve curve = Curve::fromString("0,0;0.5,0.25;1,1;");
    policy.setPressureCurve(curve);
    CHECK_FALSE(policy.isIdentity());

    // Endpoints land on exact LUT entries; interior values go through one lerp of a 256-entry
    // table, so they track the spline to well under a device's pressure quantum.
    CHECK(policy.applyPressure(0.0) == doctest::Approx(curve.eval(0.0)));
    CHECK(policy.applyPressure(1.0) == doctest::Approx(curve.eval(1.0)));
    for (const double p : {0.1, 0.25, 0.5, 0.6180339887, 0.9})
        CHECK(policy.applyPressure(p) == doctest::Approx(curve.eval(p)).epsilon(1e-3));
}

TEST_CASE("tablet policy: an identity curve short-circuits the LUT") {
    TabletPolicy policy;
    policy.setPressureCurve(Curve::fromString("0,0;1,1;"));
    CHECK(policy.isIdentity());
    // Byte-exact, not merely close: 0.3 is not representable and a float LUT round-trip would
    // perturb it.
    CHECK(policy.applyPressure(0.3) == 0.3);
}

TEST_CASE("tablet policy: the range remap runs BEFORE the response curve") {
    TabletPolicy policy;
    policy.setPressureRange(0.0, 0.5);
    policy.setPressureCurve(Curve::fromString("0,0;0.5,0.25;1,1;"));
    // Raw 0.25 remaps to 0.5, and the curve has a knot at (0.5, 0.25) -- so clamp-then-curve
    // reads 0.25. The reverse order would curve first (the spline dips to ~0.078 at raw 0.25)
    // and then remap that to ~0.156: the remap exists to re-normalize the DEVICE before the
    // user's feel curve applies.
    CHECK(policy.applyPressure(0.25) == doctest::Approx(0.25).epsilon(1e-3));
}

TEST_CASE("tablet policy: a zero tilt offset is a byte-exact pass-through") {
    const TabletPolicy policy;
    double x = 17.30000000000001;
    double y = -42.09999999999999;
    policy.applyTilt(x, y);
    CHECK(x == 17.30000000000001);
    CHECK(y == -42.09999999999999);
}

TEST_CASE("tablet policy: the tilt offset shifts ascension by exactly the offset") {
    TabletPolicy policy;
    policy.setTiltOffsetDegrees(90.0);
    CHECK_FALSE(policy.isIdentity());

    double x = 30.0;
    double y = 0.0;
    policy.applyTilt(x, y);
    // Plain 2D rotation of the tilt vector: (30, 0) by +90 degrees -> (0, 30).
    CHECK(x == doctest::Approx(0.0));
    CHECK(y == doctest::Approx(30.0));

    // And through the real sensor path: the bearing moves by 90/360, wrapped.
    const double before = ascension01(30.0, 0.0);
    const double after = ascension01(x, y);
    CHECK(after == doctest::Approx(wrapValue(before + 0.25, 0.0, 1.0)));
}

TEST_CASE("tablet policy: the tilt offset never changes the lean magnitude") {
    TabletPolicy policy;
    policy.setTiltOffsetDegrees(37.0); // deliberately not a right angle
    double x = 21.0;
    double y = -13.0;
    const double magBefore = std::hypot(x, y);
    const double declBefore = declination01(x, y);
    policy.applyTilt(x, y);
    CHECK(std::hypot(x, y) == doctest::Approx(magBefore));
    // The declination READING is allowed a small drift: the reference elevation formula
    // normalizes by whichever axis dominates, so it is mildly direction-dependent even at a
    // constant lean (tablet_policy.hpp). Bounded here so a garbled rotation still fails.
    CHECK(std::fabs(declination01(x, y) - declBefore) < 0.02);
}

TEST_CASE("tablet policy: an upright pen stays upright under any offset") {
    TabletPolicy policy;
    policy.setTiltOffsetDegrees(123.0);
    double x = 0.0;
    double y = 0.0;
    policy.applyTilt(x, y);
    CHECK(x == 0.0);
    CHECK(y == 0.0);
}

TEST_CASE("tablet policy: apply touches only pressure and the tilt pair") {
    TabletPolicy policy;
    policy.setPressureRange(0.1, 0.9);
    policy.setPressureCurve(Curve::fromString("0,0;0.5,0.25;1,1;"));
    policy.setTiltOffsetDegrees(45.0);

    StrokeInput in;
    in.pos = {95.9375, 42.0625}; // the spike's sub-pixel motion, kept sub-pixel
    in.pressure = 0.5;
    in.xTilt = 10.0;
    in.yTilt = 20.0;
    in.rotation = -77.0;
    in.tangentialPressure = 0.25;
    in.timeUs = 123456789;

    const StrokeInput out = policy.apply(in);
    CHECK(out.pos.x == in.pos.x);
    CHECK(out.pos.y == in.pos.y);
    CHECK(out.rotation == in.rotation);
    CHECK(out.tangentialPressure == in.tangentialPressure);
    CHECK(out.timeUs == in.timeUs);
    CHECK(out.pressure == doctest::Approx(policy.applyPressure(0.5)));
    CHECK(out.pressure != in.pressure);
    CHECK(out.xTilt != in.xTilt); // the rotation really ran
}

TEST_CASE("tablet policy: SpeedSmoother converges to a held speed") {
    SpeedSmoother smoother;
    smoother.setParams({3.0, 30.0}); // maxSpeed 3 px/ms, window 30 ms
    // 2 px/ms held for 300 ms = 10 time constants: the EMA must have landed.
    for (int i = 0; i < 60; ++i)
        smoother.extend(10.0, 5.0);
    CHECK(smoother.pxPerMs() == doctest::Approx(2.0).epsilon(1e-4));
    CHECK(smoother.normalized() == doctest::Approx(2.0 / 3.0).epsilon(1e-4));
}

TEST_CASE("tablet policy: SpeedSmoother steps by exactly 1-exp(-dt/tau)") {
    // The convergence tests cannot tell alpha = 1-exp(-dt/tau) from, say, exp(-dt/tau) -- every
    // alpha in (0,1) converges to the held speed eventually. One step of exactly one time
    // constant pins the FORMULA: from rest, the EMA must land at (1 - 1/e) of the input.
    SpeedSmoother smoother;
    smoother.setParams({10.0, 30.0});
    smoother.extend(60.0, 30.0); // 2 px/ms for one full time constant
    CHECK(smoother.pxPerMs() == doctest::Approx(2.0 * (1.0 - std::exp(-1.0))));
}

TEST_CASE("tablet policy: SpeedSmoother is sampling-rate independent") {
    // The same physical motion -- 2 px/ms for 300 ms -- delivered at 200 Hz and at 40 Hz must
    // smooth to the same speed. This is the time-constant form's whole point (docs/tablet.md §7):
    // a 200 Hz tablet and a 60 Hz mouse read the same.
    SpeedSmoother fast;
    fast.setParams({3.0, 30.0});
    for (int i = 0; i < 60; ++i)
        fast.extend(10.0, 5.0);
    SpeedSmoother slow;
    slow.setParams({3.0, 30.0});
    for (int i = 0; i < 12; ++i)
        slow.extend(50.0, 25.0);
    CHECK(fast.pxPerMs() == doctest::Approx(slow.pxPerMs()).epsilon(1e-3));
}

TEST_CASE("tablet policy: SpeedSmoother normalization clamps and refuses a bad ceiling") {
    SpeedSmoother smoother;
    smoother.setParams({1.0, 30.0});
    for (int i = 0; i < 60; ++i)
        smoother.extend(25.0, 5.0); // 5 px/ms against a 1 px/ms ceiling
    CHECK(smoother.normalized() == 1.0);

    smoother.setParams({0.0, 30.0}); // a non-positive ceiling reads 0, never divides
    CHECK(smoother.normalized() == 0.0);
    smoother.setParams({-1.0, 30.0});
    CHECK(smoother.normalized() == 0.0);
}

TEST_CASE("tablet policy: SpeedSmoother ignores degenerate steps") {
    SpeedSmoother smoother;
    smoother.setParams({3.0, 30.0});
    smoother.extend(10.0, 5.0);
    const double after = smoother.pxPerMs();
    smoother.extend(10.0, 0.0);  // zero dt: a replayed event contributes nothing
    smoother.extend(10.0, -5.0); // negative dt: a stepped clock contributes nothing
    CHECK(smoother.pxPerMs() == after);

    SpeedSmoother zeroWindow;
    zeroWindow.setParams({3.0, 0.0}); // a zero window never smooths (and never divides)
    zeroWindow.extend(10.0, 5.0);
    CHECK(zeroWindow.pxPerMs() == 0.0);
}

TEST_CASE("tablet policy: StrokeState speed is bit-identical to a bare SpeedSmoother") {
    // The S19 Arc C refactor moved the stroke's speed EMA into SpeedSmoother. The Uniform x Wash
    // golden pins the whole-engine result; this pins the seam directly: same steps, same dts,
    // identical BITS.
    const double steps[] = {0.0, 3.5, 10.0, 0.25, 7.0, 7.0, 1.0};
    const double dts[] = {0.0, 4.0, 5.5, 1.0, 16.0, 16.0, 2.0};

    StrokeState state;
    state.setSpeedParams({2.5, 25.0});
    StrokeInput in;
    in.pos = {0.0, 0.0};
    in.timeUs = 0;
    state.begin(in, 1);

    SpeedSmoother smoother;
    smoother.setParams({2.5, 25.0});

    double x = 0.0;
    std::uint64_t t = 0;
    for (std::size_t i = 0; i < std::size(steps); ++i) {
        x += steps[i];
        t += static_cast<std::uint64_t>(dts[i] * 1000.0);
        StrokeInput next;
        next.pos = {x, 0.0};
        next.timeUs = t;
        state.extendTo(next);
        smoother.extend(steps[i], dts[i]);
        CHECK(state.speed() == smoother.normalized()); // ==, not Approx
    }
}
