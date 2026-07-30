#include <doctest/doctest.h>

#include "core/brush/stroke_state.hpp"

#include <cmath>
#include <set>
#include <string>
#include <vector>

using mosaic::common::Vec2;
using mosaic::core::brush::additiveToScaling;
using mosaic::core::brush::kMaxTiltDegrees;
using mosaic::core::brush::scalingToAdditive;
using mosaic::core::brush::Sensor;
using mosaic::core::brush::SensorId;
using mosaic::core::brush::sensorValue;
using mosaic::core::brush::SpeedParams;
using mosaic::core::brush::lerpSnapshot;
using mosaic::core::brush::StrokeInput;
using mosaic::core::brush::StrokeSnapshot;
using mosaic::core::brush::StrokeState;
using mosaic::core::brush::wrapValue;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint64_t kSeed = 0x1234'5678'9ABC'DEF0ULL;

[[nodiscard]] StrokeInput at(double x, double y, std::uint64_t timeUs = 0) {
    StrokeInput in;
    in.pos = {x, y};
    in.timeUs = timeUs;
    return in;
}

// Evaluate a sensor with its default attributes.
[[nodiscard]] double read(SensorId id, StrokeState& s, std::string_view key = "Size") {
    return sensorValue(Sensor::withDefaults(id), s, key);
}

} // namespace

TEST_CASE("brush stroke state: wrapValue is cyclic, not clamping") {
    CHECK(wrapValue(0.25, 0.0, 1.0) == doctest::Approx(0.25));
    CHECK(wrapValue(1.25, 0.0, 1.0) == doctest::Approx(0.25));
    CHECK(wrapValue(-0.25, 0.0, 1.0) == doctest::Approx(0.75)); // negative wraps up, not to 0
    CHECK(wrapValue(-1.5, -1.0, 1.0) == doctest::Approx(0.5));
    CHECK(wrapValue(1.0, 0.0, 1.0) == doctest::Approx(0.0)); // half-open at the top

    // A degenerate or non-finite input yields the low bound rather than a NaN that would spread.
    CHECK(wrapValue(0.5, 1.0, 1.0) == doctest::Approx(1.0));
    CHECK(wrapValue(std::nan(""), 0.0, 1.0) == doctest::Approx(0.0));
}

TEST_CASE("brush stroke state: the additive maps are exact inverses") {
    for (int i = -10; i <= 10; ++i) {
        const double v = i / 10.0;
        CAPTURE(v);
        CHECK(scalingToAdditive(additiveToScaling(v)) == doctest::Approx(v));
    }
    CHECK(additiveToScaling(-1.0) == doctest::Approx(0.0));
    CHECK(additiveToScaling(1.0) == doctest::Approx(1.0));
}

TEST_CASE("brush stroke state: distance, time and the dab counter") {
    StrokeState s;
    s.begin(at(0, 0, 0), kSeed);
    CHECK(s.distance() == doctest::Approx(0.0));
    CHECK(s.elapsedMs() == doctest::Approx(0.0));
    CHECK(s.dabIndex() == -1); // no dab laid yet

    s.extendTo(at(3, 4, 10'000)); // 5 px, 10 ms
    CHECK(s.distance() == doctest::Approx(5.0));
    CHECK(s.elapsedMs() == doctest::Approx(10.0));

    s.extendTo(at(3, 4, 20'000)); // no movement, 10 more ms
    CHECK(s.distance() == doctest::Approx(5.0));
    CHECK(s.elapsedMs() == doctest::Approx(20.0));

    s.beginDab();
    CHECK(s.dabIndex() == 0);
    s.beginDab();
    CHECK(s.dabIndex() == 1);

    // begin() resets everything, so a second stroke never inherits the first's history.
    s.begin(at(100, 100, 999'000), kSeed);
    CHECK(s.distance() == doctest::Approx(0.0));
    CHECK(s.elapsedMs() == doctest::Approx(0.0));
    CHECK(s.dabIndex() == -1);
}

TEST_CASE("brush stroke state: a clock that goes backwards does not poison the state") {
    StrokeState s;
    s.begin(at(0, 0, 1'000'000), kSeed);
    s.extendTo(at(10, 0, 1'010'000));
    const double elapsed = s.elapsedMs();
    const double speed = s.speed();
    REQUIRE(elapsed > 0.0);

    // A replayed or stale sample: distance still accrues, but time and speed hold rather than
    // becoming negative and driving the EMA to nonsense.
    s.extendTo(at(20, 0, 1'005'000));
    CHECK(s.distance() == doctest::Approx(20.0));
    CHECK(s.elapsedMs() == doctest::Approx(elapsed));
    CHECK(s.speed() == doctest::Approx(speed));
    CHECK(s.speed() >= 0.0);
    CHECK(s.speed() <= 1.0);
}

TEST_CASE("brush stroke state: speed is a time-constant EMA, so the sample rate drops out") {
    // The same motion -- 1 px/ms for 300 ms -- sampled at 200 Hz and at 20 Hz must converge to the
    // same speed. That is the whole point of driving the EMA by elapsed time rather than by count.
    const auto run = [](std::uint64_t stepUs) {
        StrokeState s;
        SpeedParams p;
        p.maxSpeed = 2.0; // px/ms
        p.windowMs = 30.0;
        s.setSpeedParams(p);
        s.begin(at(0, 0, 0), kSeed);
        const double pxPerUs = 1.0 / 1000.0; // 1 px per ms
        for (std::uint64_t t = stepUs; t <= 300'000; t += stepUs)
            s.extendTo(at(static_cast<double>(t) * pxPerUs, 0, t));
        return s.speed();
    };

    const double fast = run(5'000);  // 200 Hz
    const double slow = run(50'000); // 20 Hz
    CHECK(fast == doctest::Approx(0.5).epsilon(0.02)); // 1 px/ms of a 2 px/ms full scale
    CHECK(slow == doctest::Approx(0.5).epsilon(0.02));
    CHECK(fast == doctest::Approx(slow).epsilon(0.05));

    // Faster than full scale saturates rather than exceeding the sensor's domain.
    StrokeState s;
    SpeedParams p;
    p.maxSpeed = 0.5;
    s.setSpeedParams(p);
    s.begin(at(0, 0, 0), kSeed);
    for (std::uint64_t t = 1'000; t <= 200'000; t += 1'000)
        s.extendTo(at(static_cast<double>(t) / 100.0, 0, t)); // 10 px/ms
    CHECK(s.speed() == doctest::Approx(1.0));
}

TEST_CASE("brush stroke state: the drawing angle holds through a repeated position") {
    StrokeState s;
    s.begin(at(0, 0, 0), kSeed);
    CHECK(s.drawingAngle() == doctest::Approx(0.0)); // no direction before any motion

    s.extendTo(at(10, 0, 1000)); // due east
    CHECK(s.drawingAngle() == doctest::Approx(0.0));

    s.extendTo(at(10, 10, 2000)); // due south in a y-down space
    CHECK(s.drawingAngle() == doctest::Approx(kPi / 2.0));

    // Tablets (and Android especially) re-report the same point constantly. The heading must not
    // snap back to zero when the stroke pauses.
    s.extendTo(at(10, 10, 3000));
    CHECK(s.drawingAngle() == doctest::Approx(kPi / 2.0));
}

TEST_CASE("brush stroke state: lockedAngleMode latches once, lazily") {
    StrokeState s;
    s.begin(at(0, 0, 0), kSeed);
    s.extendTo(at(10, 0, 1000)); // east

    Sensor locked = Sensor::withDefaults(SensorId::DrawingAngle);
    locked.fan.lockedAngleMode = true;
    const Sensor free = Sensor::withDefaults(SensorId::DrawingAngle);

    const double lockedFirst = sensorValue(locked, s, "Size");
    const double freeFirst = sensorValue(free, s, "Size");
    CHECK(lockedFirst == doctest::Approx(freeFirst)); // latched at the current heading

    s.extendTo(at(10, 10, 2000)); // turn south
    CHECK(sensorValue(free, s, "Size") != doctest::Approx(freeFirst));   // follows the turn
    CHECK(sensorValue(locked, s, "Size") == doctest::Approx(lockedFirst)); // does not

    // A new stroke unlatches it.
    s.begin(at(0, 0, 0), kSeed);
    s.extendTo(at(0, 10, 1000)); // south from the start
    CHECK(sensorValue(locked, s, "Size") == doctest::Approx(sensorValue(free, s, "Size")));
}

TEST_CASE("brush stroke state: drawingangle puts east at the middle of its domain") {
    StrokeState s;
    s.begin(at(0, 0, 0), kSeed);
    s.extendTo(at(10, 0, 1000)); // east, angle 0
    CHECK(read(SensorId::DrawingAngle, s) == doctest::Approx(0.5));

    // The half turn exists so the domain does not wrap at the most common heading.
    s.begin(at(0, 0, 0), kSeed);
    s.extendTo(at(-10, 0, 1000)); // west, angle pi
    CHECK(read(SensorId::DrawingAngle, s) == doctest::Approx(0.0));

    // angleOffset rotates the domain and stays inside it.
    s.begin(at(0, 0, 0), kSeed);
    s.extendTo(at(10, 0, 1000));
    Sensor offset = Sensor::withDefaults(SensorId::DrawingAngle);
    offset.fan.angleOffset = 180.0;
    const double v = sensorValue(offset, s, "Size");
    CHECK(v >= 0.0);
    CHECK(v < 1.0);
    CHECK(v == doctest::Approx(0.0)); // 0.5 + 0.5 wraps to 0

    offset.fan.angleOffset = -180.0;
    CHECK(sensorValue(offset, s, "Size") == doctest::Approx(0.0)); // and so does 0.5 - 0.5
}

TEST_CASE("brush stroke state: fuzzy draws per call, fuzzystroke holds per option") {
    StrokeState s;
    s.begin(at(0, 0, 0), kSeed);

    // fuzzy: a fresh draw every evaluation, so two options driven by it scatter independently.
    std::set<double> draws;
    for (int i = 0; i < 64; ++i)
        draws.insert(read(SensorId::Fuzzy, s));
    CHECK(draws.size() == 64);
    for (const double d : draws) {
        CHECK(d >= -1.0);
        CHECK(d <= 1.0);
    }

    // fuzzystroke: constant for the whole stroke, and constant no matter how often it is read --
    // evaluating an option twice within one dab must not move the brush.
    const double a1 = read(SensorId::FuzzyStroke, s, "Size");
    const double a2 = read(SensorId::FuzzyStroke, s, "Size");
    CHECK(a1 == a2);
    // ...but different per option, which is why it is keyed rather than stored.
    CHECK(read(SensorId::FuzzyStroke, s, "Rotation") != doctest::Approx(a1));

    // A different seed gives a different stroke; the same seed replays it exactly. Golden images,
    // the editor preview and undo of a fuzzy stroke all rest on this.
    s.begin(at(0, 0, 0), kSeed + 1);
    CHECK(read(SensorId::FuzzyStroke, s, "Size") != doctest::Approx(a1));

    s.begin(at(0, 0, 0), kSeed);
    CHECK(read(SensorId::FuzzyStroke, s, "Size") == doctest::Approx(a1));
    StrokeState replay;
    replay.begin(at(0, 0, 0), kSeed);
    std::vector<double> first;
    std::vector<double> second;
    for (int i = 0; i < 16; ++i)
        first.push_back(read(SensorId::Fuzzy, s));
    for (int i = 0; i < 16; ++i)
        second.push_back(read(SensorId::Fuzzy, replay));
    CHECK(first == second);
}

TEST_CASE("brush stroke state: pressure sensors") {
    StrokeState s;
    StrokeInput in = at(0, 0, 0);
    in.pressure = 0.25;
    s.begin(in, kSeed);
    CHECK(read(SensorId::Pressure, s) == doctest::Approx(0.25));
    CHECK(read(SensorId::PressureIn, s) == doctest::Approx(0.25));

    in.pressure = 0.9;
    in.timeUs = 1000;
    s.extendTo(in);
    CHECK(read(SensorId::Pressure, s) == doctest::Approx(0.9));

    // pressurein is the stroke's high-water mark: it does not fall when the pen eases off.
    in.pressure = 0.1;
    in.timeUs = 2000;
    s.extendTo(in);
    CHECK(read(SensorId::Pressure, s) == doctest::Approx(0.1));
    CHECK(read(SensorId::PressureIn, s) == doctest::Approx(0.9));

    // Out-of-range pressure from a miscalibrated device is clamped, not propagated.
    in.pressure = 3.0;
    in.timeUs = 3000;
    s.extendTo(in);
    CHECK(read(SensorId::Pressure, s) == doctest::Approx(1.0));
}

TEST_CASE("brush stroke state: tilt sensors") {
    StrokeState s;
    StrokeInput in = at(0, 0, 0);

    // Upright: both tilt axes read 1, elevation is 1, and the direction is pinned to the neutral
    // bearing rather than to whatever atan2(0,0) happens to return.
    s.begin(in, kSeed);
    CHECK(read(SensorId::XTilt, s) == doctest::Approx(1.0));
    CHECK(read(SensorId::YTilt, s) == doctest::Approx(1.0));
    CHECK(read(SensorId::Declination, s) == doctest::Approx(1.0));
    CHECK(read(SensorId::Ascension, s) == doctest::Approx(scalingToAdditive(0.25)));

    // Laid flat along one axis: that axis reads 0, elevation reads 0.
    in.xTilt = kMaxTiltDegrees;
    s.begin(in, kSeed);
    CHECK(read(SensorId::XTilt, s) == doctest::Approx(0.0));
    CHECK(read(SensorId::Declination, s) == doctest::Approx(0.0));

    // Tilt is symmetric in sign for the x/y sensors -- they report lean, not direction.
    in.xTilt = -kMaxTiltDegrees;
    s.begin(in, kSeed);
    CHECK(read(SensorId::XTilt, s) == doctest::Approx(0.0));

    // A device reporting beyond full scale saturates instead of inverting.
    in.xTilt = 200.0;
    s.begin(in, kSeed);
    CHECK(read(SensorId::XTilt, s) == doctest::Approx(0.0));
    CHECK(read(SensorId::Declination, s) == doctest::Approx(0.0));

    // Ascension is additive, so it spans [-1,1] and opposite leans land on opposite ends.
    in.xTilt = 0.0;
    in.yTilt = 30.0;
    s.begin(in, kSeed);
    const double north = read(SensorId::Ascension, s);
    in.yTilt = -30.0;
    s.begin(in, kSeed);
    const double south = read(SensorId::Ascension, s);
    CHECK(north != doctest::Approx(south));
    for (const double v : {north, south}) {
        CHECK(v >= -1.0);
        CHECK(v <= 1.0);
    }
}

TEST_CASE("brush stroke state: rotation and tangential pressure") {
    StrokeState s;
    StrokeInput in = at(0, 0, 0);
    in.rotation = 90.0;
    in.tangentialPressure = 0.4;
    s.begin(in, kSeed);
    CHECK(read(SensorId::Rotation, s) == doctest::Approx(0.5)); // additive: [-180,180] -> [-1,1]
    CHECK(read(SensorId::TangentialPressure, s) == doctest::Approx(0.4));

    in.rotation = -180.0;
    s.begin(in, kSeed);
    CHECK(read(SensorId::Rotation, s) == doctest::Approx(-1.0));

    in.rotation = 5000.0; // a device that reports turns rather than a bearing
    s.begin(in, kSeed);
    CHECK(read(SensorId::Rotation, s) == doctest::Approx(1.0));
}

TEST_CASE("brush stroke state: the three ramp sensors count in their own units") {
    StrokeState s;
    s.begin(at(0, 0, 0), kSeed);

    // distance: document px.
    Sensor dist = Sensor::withDefaults(SensorId::Distance);
    dist.range.length = 100;
    s.extendTo(at(25, 0, 1000));
    CHECK(sensorValue(dist, s, "Size") == doctest::Approx(0.25));
    s.extendTo(at(200, 0, 2000)); // past the length: saturates
    CHECK(sensorValue(dist, s, "Size") == doctest::Approx(1.0));

    // ...and sawtooths instead when periodic.
    dist.range.periodic = true;
    CHECK(sensorValue(dist, s, "Size") == doctest::Approx(0.0)); // 200 px, 100 px period

    // time: milliseconds, NOT seconds. A default duration of 30 is a 30 ms opening ramp.
    Sensor time = Sensor::withDefaults(SensorId::Time);
    CHECK(time.range.length == 30);
    s.begin(at(0, 0, 0), kSeed);
    s.extendTo(at(0, 0, 15'000)); // 15 ms
    CHECK(sensorValue(time, s, "Size") == doctest::Approx(0.5));
    s.extendTo(at(0, 0, 60'000)); // 60 ms, past the ramp
    CHECK(sensorValue(time, s, "Size") == doctest::Approx(1.0));

    // fade: dabs.
    Sensor fade = Sensor::withDefaults(SensorId::Fade);
    CHECK(fade.range.length == 1000); // fade alone defaults to 1000
    fade.range.length = 4;
    s.begin(at(0, 0, 0), kSeed);
    CHECK(sensorValue(fade, s, "Size") == doctest::Approx(0.0)); // before the first dab
    s.beginDab();                                               // dab 0
    CHECK(sensorValue(fade, s, "Size") == doctest::Approx(0.0));
    s.beginDab();
    s.beginDab(); // dab 2
    CHECK(sensorValue(fade, s, "Size") == doctest::Approx(0.5));
    for (int i = 0; i < 10; ++i)
        s.beginDab();
    CHECK(sensorValue(fade, s, "Size") == doctest::Approx(1.0)); // saturates
}

TEST_CASE("brush stroke state: perspective is an honest constant") {
    // Mosaic has no perspective grid. A preset driving an option from it still imports and paints;
    // the option simply reads a constant, and the importer records the loss.
    StrokeState s;
    s.begin(at(0, 0, 0), kSeed);
    CHECK(read(SensorId::Perspective, s) == doctest::Approx(1.0));
}

TEST_CASE("brush stroke state: every sensor stays inside its declared range") {
    // A sensor's raw value feeds a curve LUT whose domain is [0,1] (after the class-specific map),
    // so a sensor that escapes its range silently saturates the curve instead of erroring.
    StrokeState s;
    StrokeInput in = at(0, 0, 0);
    in.pressure = 2.0;
    in.xTilt = -300.0;
    in.yTilt = 400.0;
    in.rotation = -9999.0;
    in.tangentialPressure = -5.0;
    s.begin(in, kSeed);
    s.extendTo(at(-1e6, 1e6, 1));
    for (int i = 0; i < 4; ++i)
        s.beginDab();

    for (std::size_t i = 0; i < mosaic::core::brush::kSensorCount; ++i) {
        const auto id = static_cast<SensorId>(i);
        CAPTURE(mosaic::core::brush::sensorName(id));
        const double v = read(id, s);
        CHECK(std::isfinite(v));
        CHECK(v >= -1.0);
        CHECK(v <= 1.0);
    }
}

TEST_CASE("brush stroke state: a hand-built zero-length range sensor cannot emit NaN") {
    // The XML parser clamps `length` to >= 1, but Sensor is an aggregate anyone may fill in, and the
    // struct's own default does not stop a direct field write. A zero here divides by zero: fmod
    // returns NaN for the periodic ramp and the plain divide returns infinity. Either would travel
    // through a curve LUT into a dab. The floor lives where the division is, not only at the parser.
    StrokeState s;
    s.begin(at(0, 0, 0), kSeed);
    s.extendTo(at(500, 0, 500'000));
    for (int i = 0; i < 8; ++i)
        s.beginDab();

    for (const SensorId id : {SensorId::Fade, SensorId::Distance, SensorId::Time}) {
        for (const bool periodic : {false, true}) {
            CAPTURE(mosaic::core::brush::sensorName(id));
            CAPTURE(periodic);
            Sensor sensor = Sensor::withDefaults(id);
            sensor.range.length = 0;
            sensor.range.periodic = periodic;
            const double v = sensorValue(sensor, s, "Size");
            CHECK(std::isfinite(v));
            CHECK(v >= 0.0);
            CHECK(v <= 1.0);
        }
    }
}

TEST_CASE("brush stroke state: wrapValue never returns its upper bound") {
    // A tiny negative fmod remainder plus `range` can round up to exactly `range`, which would make
    // the half-open contract a lie -- and the drawingangle sensor's domain wrap with it.
    CHECK(wrapValue(-1e-18, 0.0, 1.0) < 1.0);
    CHECK(wrapValue(-1e-300, 0.0, 1.0) < 1.0);
    CHECK(wrapValue(3.0 - 1e-17, 0.0, 1.0) < 1.0);
    CHECK(wrapValue(-2.0 - 1e-18, -1.0, 1.0) < 1.0);

    // Exhaustively: nothing in a wide sweep escapes [lo, hi).
    for (int i = -2000; i <= 2000; ++i) {
        const double x = i * 0.0013;
        CAPTURE(x);
        const double a = wrapValue(x, 0.0, 1.0);
        CHECK(a >= 0.0);
        CHECK(a < 1.0);
        const double b = wrapValue(x, -1.0, 1.0);
        CHECK(b >= -1.0);
        CHECK(b < 1.0);
    }
}

// ---------------------------------------------------------------------------------------------
// StrokeSnapshot -- the derived state as a value the dab walk can re-install (docs/brushes.md §6.2).
//
// The walk lags the sample stream by one sample, so a dab is stamped after the state has already
// moved past its span. These are the properties that let it read the state that belonged to IT.

TEST_CASE("a snapshot round-trips the derived state") {
    StrokeState s;
    s.begin(at(10.0, 10.0, 0), kSeed);
    s.extendTo(at(40.0, 10.0, 20'000));
    s.extendTo(at(40.0, 50.0, 50'000));

    const StrokeSnapshot snap = s.snapshot();
    CHECK(snap.sample.pos.x == doctest::Approx(40.0));
    CHECK(snap.distance == doctest::Approx(70.0));
    CHECK(snap.elapsedMs == doctest::Approx(50.0));
    CHECK(snap.drawingAngle == doctest::Approx(kPi * 0.5)); // due south
    CHECK(snap.speedPxPerMs > 0.0);

    // Move the stroke on, then put it back. Every field the snapshot carries returns.
    const double speedThen = s.speed();
    s.extendTo(at(200.0, 50.0, 60'000));
    CHECK(s.distance() == doctest::Approx(230.0));
    s.rewindTo(snap);
    CHECK(s.sample().pos.x == doctest::Approx(40.0));
    CHECK(s.distance() == doctest::Approx(70.0));
    CHECK(s.elapsedMs() == doctest::Approx(50.0));
    CHECK(s.drawingAngle() == doctest::Approx(kPi * 0.5));
    CHECK(s.speed() == doctest::Approx(speedThen));
    CHECK(s.maxPressure() == doctest::Approx(snap.maxPressure));
}

TEST_CASE("a rewind re-reads the stroke -- it does not rewind the stroke-scoped streams") {
    // The whole point of the split. `fuzzy`, the dab counter and the latched angle are properties of
    // the STROKE, not of a point on it: a dab that re-reads an earlier point must not also re-draw an
    // earlier random number, or fuzzy would repeat itself once per span.
    StrokeState s;
    s.begin(at(0.0, 0.0, 0), kSeed);
    s.extendTo(at(10.0, 0.0, 10'000));
    const StrokeSnapshot snap = s.snapshot();

    s.beginDab();
    const double r0 = s.nextRandom();
    const double locked = s.lockedDrawingAngle(); // latches HERE, heading east
    CHECK(locked == doctest::Approx(0.0));
    CHECK(s.dabIndex() == 0);

    // Head north, then rewind to the eastbound snapshot.
    s.extendTo(at(10.0, 40.0, 20'000));
    s.beginDab();
    const double r1 = s.nextRandom();
    s.rewindTo(snap);

    CHECK(s.drawingAngle() == doctest::Approx(0.0)); // the DERIVED angle came back ...
    CHECK(s.lockedDrawingAngle() == doctest::Approx(locked)); // ... the LATCHED one never left
    CHECK(s.dabIndex() == 1);                                 // the counter kept counting
    const double r2 = s.nextRandom();                         // and the stream kept advancing
    CHECK(r2 != r0);
    CHECK(r2 != r1);
}

TEST_CASE("lerpSnapshot: a dab between two samples reads a state between them") {
    StrokeSnapshot a;
    a.sample = at(0.0, 0.0, 1'000);
    a.sample.pressure = 0.2;
    a.sample.xTilt = -30.0;
    a.distance = 10.0;
    a.elapsedMs = 5.0;
    a.speedPxPerMs = 1.0;
    a.maxPressure = 0.4; // the stroke pressed harder EARLIER than this span

    StrokeSnapshot b;
    b.sample = at(20.0, 0.0, 3'000);
    b.sample.pressure = 0.6;
    b.sample.xTilt = 30.0;
    b.distance = 30.0;
    b.elapsedMs = 7.0;
    b.speedPxPerMs = 3.0;
    b.maxPressure = 0.6;

    const StrokeSnapshot m = lerpSnapshot(a, b, 0.5);
    CHECK(m.sample.pos.x == doctest::Approx(10.0));
    CHECK(m.sample.pressure == doctest::Approx(0.4));
    CHECK(m.sample.xTilt == doctest::Approx(0.0));
    CHECK(m.sample.timeUs == 2'000);
    CHECK(m.distance == doctest::Approx(20.0));
    CHECK(m.elapsedMs == doctest::Approx(6.0));
    CHECK(m.speedPxPerMs == doctest::Approx(2.0));
    // A running max does not interpolate: it is a's high-water mark against THIS dab's pressure.
    // Interpolating it would have given 0.5, a pressure the stroke never reached.
    CHECK(m.maxPressure == doctest::Approx(0.4));

    // The endpoints are the samples themselves.
    CHECK(lerpSnapshot(a, b, 0.0).sample.pressure == doctest::Approx(0.2));
    CHECK(lerpSnapshot(a, b, 1.0).sample.pressure == doctest::Approx(0.6));
    CHECK(lerpSnapshot(a, b, 1.0).maxPressure == doctest::Approx(0.6));
    // t off the span is clamped to it -- a dab is never off its own span.
    CHECK(lerpSnapshot(a, b, 2.0).sample.pressure == doctest::Approx(0.6));
    CHECK(lerpSnapshot(a, b, -1.0).sample.pressure == doctest::Approx(0.2));
}

TEST_CASE("lerpSnapshot blends angles along the shorter arc") {
    StrokeSnapshot a;
    StrokeSnapshot b;

    // Barrel rotation across the +/-180 seam: 170 -> -170 is a 20-degree turn, not a 340-degree one.
    a.sample.rotation = 170.0;
    b.sample.rotation = -170.0;
    const double mid = lerpSnapshot(a, b, 0.5).sample.rotation;
    CHECK(std::fabs(mid) > 179.0); // it went the SHORT way, through the seam
    CHECK(std::fabs(mid) <= 180.0);
    // The endpoints are verbatim: a dab landing on a sample reads that sample's angle, and at the
    // seam +180 and -180 are the same bearing but OPPOSITE readings of the `rotation` sensor.
    CHECK(lerpSnapshot(a, b, 1.0).sample.rotation == -170.0);
    CHECK(lerpSnapshot(a, b, 0.0).sample.rotation == 170.0);

    // The drawing angle is radians and wraps at +/-pi, on the same rule.
    a.drawingAngle = 3.0;
    b.drawingAngle = -3.0;
    const double da = lerpSnapshot(a, b, 0.5).drawingAngle;
    CHECK(std::fabs(da) > kPi - 0.15);
    CHECK(lerpSnapshot(a, b, 1.0).drawingAngle == -3.0);

    // ... and an ordinary (non-wrapping) pair still blends the plain way.
    a.drawingAngle = 0.0;
    b.drawingAngle = 1.0;
    CHECK(lerpSnapshot(a, b, 0.25).drawingAngle == doctest::Approx(0.25));
}

TEST_CASE("lerpSnapshot: a dab ON the seam keeps the reading its sample had") {
    // The canonical angle range is half-open, so +180 and -180 are the same bearing but land on
    // OPPOSITE ends of the `rotation` sensor's domain (+1 against -1). Deriving an endpoint through
    // the wrap -- rather than handing the sample's own angle straight back -- flips it.
    //
    // This is the case that tells the two code paths apart. At 170 -> -170 the arithmetic happens to
    // reproduce the endpoint anyway, so a test that only CROSSES the seam cannot see the difference:
    // it has to land ON it.
    StrokeSnapshot a;
    StrokeSnapshot b;
    a.sample.rotation = 180.0;
    b.sample.rotation = 180.0;
    a.drawingAngle = kPi;
    b.drawingAngle = kPi;

    CHECK(lerpSnapshot(a, b, 0.0).sample.rotation == 180.0);
    CHECK(lerpSnapshot(a, b, 1.0).sample.rotation == 180.0);
    CHECK(lerpSnapshot(a, b, 0.0).drawingAngle == kPi);
    CHECK(lerpSnapshot(a, b, 1.0).drawingAngle == kPi);

    // The sensor is what a flip would actually break: a full clockwise barrel turn reading as a full
    // ANTICLOCKWISE one.
    StrokeState s;
    s.begin(at(0.0, 0.0, 0), kSeed);
    s.rewindTo(lerpSnapshot(a, b, 1.0));
    CHECK(sensorValue(Sensor::withDefaults(SensorId::Rotation), s, "Rotation") ==
          doctest::Approx(1.0));
}

TEST_CASE("lerpSnapshot holds the clock on a non-advancing pair") {
    // StrokeState::extendTo holds the clock on a replayed event, so two snapshots can carry the same
    // (or a backwards) timestamp. Blending one backwards would hand `time` a dab that happened before
    // its own span began.
    StrokeSnapshot a;
    a.sample = at(0.0, 0.0, 5'000);
    StrokeSnapshot b;
    b.sample = at(10.0, 0.0, 2'000);
    CHECK(lerpSnapshot(a, b, 0.5).sample.timeUs == 5'000);
    b.sample.timeUs = 5'000;
    CHECK(lerpSnapshot(a, b, 0.5).sample.timeUs == 5'000);
}
