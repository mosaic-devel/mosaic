#include <doctest/doctest.h>

#include "core/brush/dab.hpp"

#include <cmath>
#include <limits>
#include <optional>

using mosaic::common::Vec2;
using mosaic::core::brush::BrushOptions;
using mosaic::core::brush::CurveOption;
using mosaic::core::brush::CurveOptionData;
using mosaic::core::brush::Dab;
using mosaic::core::brush::DabBase;
using mosaic::core::brush::evaluateDab;
using mosaic::core::brush::kRotationTurnRad;
using mosaic::core::brush::dabAngle;
using mosaic::core::brush::reticleDabAngle;
using mosaic::core::brush::Sensor;
using mosaic::core::brush::SensorId;
using mosaic::core::brush::StrokeInput;
using mosaic::core::brush::StrokeState;

namespace {

constexpr std::uint64_t kSeed = 0x0BADC0DE'F00DFACEULL;

[[nodiscard]] StrokeInput at(double x, double y, double pressure = 1.0) {
    StrokeInput in;
    in.pos = {x, y};
    in.pressure = pressure;
    return in;
}

// A stroke standing at one sample, so an option can be evaluated against a known pressure.
[[nodiscard]] StrokeState strokeAt(double pressure) {
    StrokeState s;
    s.begin(at(0.0, 0.0, pressure), kSeed);
    s.beginDab();
    return s;
}

// One option: `sensor` with an identity curve, at `strength`, checked unless told otherwise.
[[nodiscard]] CurveOption option(const char* name, SensorId sensor, double strength = 1.0,
                                 bool checked = true, bool checkable = true) {
    CurveOptionData d;
    d.name = name;
    d.checkable = checkable;
    d.checked = checked;
    d.strength = strength;
    d.sensors.sensors = {Sensor::withDefaults(sensor)};
    return CurveOption(d);
}

} // namespace

TEST_CASE("evaluateDab: no options is the identity, to the bit") {
    // The hard rule of §6.2, and the reason a mouse stroke and Uniform x Wash cannot move: an option
    // pipeline with nothing in it must hand back exactly the preset's static geometry. Not "to within
    // a rounding error" -- exactly, because `x * 1.0 == x` is what the engine's goldens rest on.
    DabBase base;
    base.diameter = 24.0;
    base.ratio = 1.0;
    base.angleRad = 0.0;
    base.flow = 0.35;

    StrokeState s = strokeAt(0.4); // a pressure that WOULD scale everything, if anything read it
    const Dab d = evaluateDab(BrushOptions{}, base, {12.5, 30.25}, s);

    CHECK(d.center.x == 12.5);
    CHECK(d.center.y == 30.25);
    CHECK(d.diameter == 24.0);
    CHECK(d.ratio == 1.0);
    CHECK(d.angleRad == 0.0);
    CHECK(d.flow == 0.35);
    CHECK_FALSE(d.mirrorH);
    CHECK_FALSE(d.mirrorV);

    // ... and so is a preset whose geometry is nothing like the default.
    DabBase odd;
    odd.diameter = 7.375;
    odd.ratio = 0.4;
    odd.angleRad = 0.75;
    odd.flow = 1.0;
    const Dab e = evaluateDab(BrushOptions{}, odd, {0.0, 0.0}, s);
    CHECK(e.diameter == 7.375);
    CHECK(e.ratio == 0.4);
    CHECK(e.angleRad == 0.75);
    CHECK(e.flow == 1.0);
}

TEST_CASE("evaluateDab: Size and Flow scale the preset's geometry from pressure") {
    DabBase base;
    base.diameter = 40.0;
    base.flow = 0.8;

    BrushOptions o;
    o.size = option("Size", SensorId::Pressure);
    o.flow = option("Flow", SensorId::Pressure, 1.0, false, /*checkable=*/false);

    StrokeState s = strokeAt(0.5);
    const Dab d = evaluateDab(o, base, {0.0, 0.0}, s);
    CHECK(d.diameter == doctest::Approx(20.0)); // 40 * 0.5
    CHECK(d.flow == doctest::Approx(0.4));      // 0.8 * 0.5

    // Flow is one of the two ALWAYS-ON options (§3.2): its `PressureFlow` bit is clear in every
    // shipped preset, and the reader forces it on regardless. It is not checkable, so `checked=false`
    // above must not disable it -- which the 0.4 just proved.
    StrokeState full = strokeAt(1.0);
    const Dab e = evaluateDab(o, base, {0.0, 0.0}, full);
    CHECK(e.diameter == doctest::Approx(40.0));
    CHECK(e.flow == doctest::Approx(0.8));
}

TEST_CASE("evaluateDab: an unchecked option contributes the identity, not its strength") {
    // The trap. An option that is present but switched off must vanish -- if the strength leaked
    // through, a preset carrying a Size option with `PressureSize` clear and `SizeValue=0.3` would
    // paint at 30 % of the size it was authored at, forever.
    DabBase base;
    base.diameter = 40.0;

    BrushOptions o;
    o.size = option("Size", SensorId::Pressure, /*strength=*/0.3, /*checked=*/false);

    StrokeState s = strokeAt(1.0);
    CHECK(evaluateDab(o, base, {0.0, 0.0}, s).diameter == 40.0);

    // Check it and the strength (and the sensor) finally bite.
    o.size = option("Size", SensorId::Pressure, 0.3, true);
    StrokeState t = strokeAt(1.0);
    CHECK(evaluateDab(o, base, {0.0, 0.0}, t).diameter == doctest::Approx(12.0)); // 40 * 0.3 * 1.0

    // Rotation needs its own gate, and needs it MORE: a rotation-like value is re-centred on zero, so
    // an unchecked Rotation option that leaked through would not merely scale the dab -- it would spin
    // it a HALF TURN at full pressure, on all 35 shipped presets that carry the option switched off.
    DabBase flat;
    flat.angleRad = 0.0;
    BrushOptions r;
    r.rotation = option("Rotation", SensorId::Pressure, 1.0, /*checked=*/false);
    StrokeState u = strokeAt(1.0);
    CHECK(evaluateDab(r, flat, {0.0, 0.0}, u).angleRad == 0.0);
}

TEST_CASE("evaluateDab: Ratio scales the tip's aspect") {
    DabBase base;
    base.diameter = 30.0;
    base.ratio = 0.5; // an authored nib, already twice as wide as it is tall

    BrushOptions o;
    o.ratio = option("Ratio", SensorId::Pressure);

    StrokeState s = strokeAt(0.5);
    const Dab d = evaluateDab(o, base, {0.0, 0.0}, s);
    CHECK(d.diameter == doctest::Approx(30.0)); // Ratio does not touch the size
    CHECK(d.ratio == doctest::Approx(0.25));    // 0.5 * 0.5 -- it SCALES the authored aspect
}

TEST_CASE("evaluateDab: Rotation adds half-turns to the tip's authored angle") {
    // rotationLikeValue works in HALF TURNS: it doubles its inputs into a [-1,1) space, so 1.0 there
    // is pi radians, not 2pi. An ellipse has a period of pi, so a full-strength pressure sweep turning
    // the dab a half turn each way covers every distinct orientation it has.
    DabBase base;
    base.angleRad = 0.25; // the tip was authored slanted

    BrushOptions o;
    o.rotation = option("Rotation", SensorId::Pressure);

    // A scaling sensor reaches an ANGLE only after being re-centred on zero: pressure 0.5 means "no
    // rotation", which is what keeps a mid-pressure dab from being flung a quarter turn round.
    StrokeState half = strokeAt(0.5);
    CHECK(evaluateDab(o, base, {0.0, 0.0}, half).angleRad == doctest::Approx(0.25));

    // Full pressure re-centres to +1 half-turn ... which wraps to -1, the same orientation.
    StrokeState full = strokeAt(1.0);
    const double a = evaluateDab(o, base, {0.0, 0.0}, full).angleRad;
    CHECK(a == doctest::Approx(0.25 - kRotationTurnRad));

    // Zero pressure re-centres to -1 half-turn.
    StrokeState none = strokeAt(0.0);
    CHECK(evaluateDab(o, base, {0.0, 0.0}, none).angleRad ==
          doctest::Approx(0.25 - kRotationTurnRad));

    // It ADDS to the authored angle rather than replacing it: a preset that slants its nib AND drives
    // rotation from pressure means both.
    DabBase flat;
    flat.angleRad = 0.0;
    StrokeState full2 = strokeAt(1.0);
    CHECK(evaluateDab(o, flat, {0.0, 0.0}, full2).angleRad == doctest::Approx(-kRotationTurnRad));
}

TEST_CASE("evaluateDab: a drawingangle sensor makes the dab follow the stroke") {
    // The 3rd-most-used sensor in the shipped set (14 presets), and the only ABSOLUTE-rotation one:
    // it does not add to the angle, it IS the angle. A nib that follows the stroke's heading.
    DabBase base;
    base.angleRad = 0.0;

    BrushOptions o;
    o.rotation = option("Rotation", SensorId::DrawingAngle);

    StrokeState s;
    s.begin(at(0.0, 0.0), kSeed);
    s.extendTo(at(0.0, 40.0)); // heading due south: atan2(+40, 0) = +pi/2
    s.beginDab();
    const double south = evaluateDab(o, base, {0.0, 40.0}, s).angleRad;

    StrokeState e;
    e.begin(at(0.0, 0.0), kSeed);
    e.extendTo(at(40.0, 0.0)); // heading due east
    e.beginDab();
    const double east = evaluateDab(o, base, {40.0, 0.0}, e).angleRad;

    // A quarter turn of heading turns the dab a quarter turn -- and no further, which a naive
    // full-turn carrier would have doubled.
    const double turn = std::fabs(south - east);
    CHECK(turn == doctest::Approx(kRotationTurnRad * 0.5));
}

TEST_CASE("evaluateDab: evaluating a dab draws from the stroke's random stream") {
    // `fuzzy` is the 2nd-most-used sensor (23 presets). It makes evaluateDab a mutation of the stroke,
    // not a read of it -- which is why the walk must evaluate each dab exactly once, and in order.
    DabBase base;
    base.diameter = 40.0;

    BrushOptions o;
    o.size = option("Size", SensorId::Fuzzy);

    StrokeState s = strokeAt(1.0);
    const double a = evaluateDab(o, base, {0.0, 0.0}, s).diameter;
    const double b = evaluateDab(o, base, {0.0, 0.0}, s).diameter;
    CHECK(a != b); // the same dab, twice, is two different draws

    // ... and the stream is seeded, so the stroke replays. That is what golden images, the editor's
    // preview and undo/redo of a fuzzy stroke all rest on.
    StrokeState t = strokeAt(1.0);
    CHECK(evaluateDab(o, base, {0.0, 0.0}, t).diameter == a);
}

// ---- the RETICLE's angle -----------------------------------------------------------------------
//
// The ring has to draw the tip the next dab will lay, before that dab exists. §6.3's ruling: it
// follows every sensor that is a DIRECTION and none that is a MAGNITUDE.

TEST_CASE("reticleDabAngle: the ring turns with the stroke, and it turns EXACTLY as the dab does") {
    // ⚠ THE LOAD-BEARING CASE. The bug the user reported was a DRIFT: the dab followed the heading
    // and the ring showed the authored slant. The two now share one rule, and this is the pin -- for
    // any heading, what the ring draws IS what the dab takes. If someone re-implements either side,
    // this fails.
    DabBase base;
    base.angleRad = 0.0;

    BrushOptions o;
    o.rotation = option("Rotation", SensorId::DrawingAngle);

    for (const double heading : {0.0, 0.3, 1.0, kRotationTurnRad / 2.0, -2.2, 3.0}) {
        // What the DAB takes: a real stroke, driven to that heading.
        StrokeState s;
        s.begin(at(0.0, 0.0), kSeed);
        s.extendTo(at(std::cos(heading) * 40.0, std::sin(heading) * 40.0));
        s.beginDab();
        const double dab = evaluateDab(o, base, {0.0, 0.0}, s).angleRad;

        // What the RING draws, told only the heading.
        const double ring = reticleDabAngle(o, base.angleRad, StrokeInput{}, heading);

        CHECK(ring == doctest::Approx(dab));
        CHECK(dabAngle(o, base.angleRad, s) == doctest::Approx(dab)); // ... and both go through one fn
    }
}

TEST_CASE("reticleDabAngle: no rotation option is the authored angle, to the bit") {
    // A brush that predates the preset library has no option table at all, and the ring it draws must
    // not move by an ulp.
    BrushOptions none;
    CHECK(reticleDabAngle(none, 0.25, StrokeInput{}, 1.0) == 0.25);
    CHECK(reticleDabAngle(none, 0.0, StrokeInput{}, 1.0) == 0.0);

    // ... and an option that is present but UNCHECKED contributes nothing either (the §6.2 rule).
    BrushOptions off;
    off.rotation = option("Rotation", SensorId::DrawingAngle, 1.0, /*checked=*/false);
    CHECK(reticleDabAngle(off, 0.25, StrokeInput{}, 1.0) == 0.25);
}

TEST_CASE("reticleDabAngle: no heading yet is the authored angle -- a still pointer has no direction") {
    BrushOptions o;
    o.rotation = option("Rotation", SensorId::DrawingAngle);
    const double none = std::numeric_limits<double>::quiet_NaN();
    CHECK(reticleDabAngle(o, 0.25, StrokeInput{}, none) == 0.25);
    // A tip must not snap to "pointing east" the instant the cursor crosses the canvas edge.
    CHECK(reticleDabAngle(o, 0.25, StrokeInput{}, none) != reticleDabAngle(o, 0.25, StrokeInput{}, 0.0));
}

TEST_CASE("reticleDabAngle: a MAGNITUDE does not turn the ring, a DIRECTION does") {
    // The whole ruling, in one case. §6.3 already forbids the ring from breathing with the pen; the
    // same argument says nothing about an orientation, which is a statement about where paint lands.
    //
    // The sensor CLASSIFICATION (sensors.cpp) draws this line already and we do not re-draw it:
    // Scaling = magnitude, Additive/AbsoluteRotation = direction.
    StrokeInput soft;
    soft.pressure = 0.1;
    StrokeInput hard;
    hard.pressure = 1.0;

    SUBCASE("pressure is a magnitude: the ring does not move") {
        BrushOptions o;
        o.rotation = option("Rotation", SensorId::Pressure);
        CHECK(reticleDabAngle(o, 0.0, soft, 0.5) == doctest::Approx(reticleDabAngle(o, 0.0, hard, 0.5)));
        // ... and the DAB still does, which is the point: the two are allowed to differ HERE.
        StrokeState a = strokeAt(0.1);
        StrokeState b = strokeAt(1.0);
        DabBase base;
        CHECK(evaluateDab(o, base, {0.0, 0.0}, a).angleRad !=
              doctest::Approx(evaluateDab(o, base, {0.0, 0.0}, b).angleRad));
    }

    SUBCASE("declination -- the tilt ANGLE -- is a magnitude too") {
        BrushOptions o;
        o.rotation = option("Rotation", SensorId::Declination);
        StrokeInput upright; // no lean
        StrokeInput leaning;
        leaning.xTilt = 45.0;
        CHECK(reticleDabAngle(o, 0.0, upright, 0.5) ==
              doctest::Approx(reticleDabAngle(o, 0.0, leaning, 0.5)));
    }

    SUBCASE("ascension -- the tilt BEARING -- is a direction: the ring leans with the pen") {
        BrushOptions o;
        o.rotation = option("Rotation", SensorId::Ascension);
        StrokeInput east;
        east.xTilt = 40.0;
        StrokeInput north;
        north.yTilt = 40.0;
        CHECK(reticleDabAngle(o, 0.0, east, 0.5) != doctest::Approx(reticleDabAngle(o, 0.0, north, 0.5)));
    }

    SUBCASE("barrel rotation is a direction") {
        BrushOptions o;
        o.rotation = option("Rotation", SensorId::Rotation);
        StrokeInput flat;
        StrokeInput twisted;
        twisted.rotation = 90.0;
        CHECK(reticleDabAngle(o, 0.0, flat, 0.5) != doctest::Approx(reticleDabAngle(o, 0.0, twisted, 0.5)));
    }
}

TEST_CASE("reticleDabAngle: a RANDOM rotation is not a direction -- the ring holds still") {
    // 13 of the 82 shipped presets drive rotation from `fuzzy`. A ring that re-rolled the dice on
    // every motion event would be telling the user something true and useless -- and it would also
    // have to draw from a random stream to do it, which a const hover has no business doing.
    for (const SensorId noise : {SensorId::Fuzzy, SensorId::FuzzyStroke}) {
        BrushOptions o;
        o.rotation = option("Rotation", noise);
        CHECK(reticleDabAngle(o, 0.25, StrokeInput{}, 0.5) == 0.25);
        CHECK(reticleDabAngle(o, 0.25, StrokeInput{}, 2.0) == 0.25); // and the heading cannot shake it
    }

    // ⚠ ... even when a real direction sensor is in the SAME option. Fuzzy would still have to be
    // drawn to evaluate it, so the whole option is refused. (A preset like this exists: 2 of the 82
    // carry `fuzzy` alongside another sensor on Rotation.)
    CurveOptionData d;
    d.name = "Rotation";
    d.checkable = true;
    d.checked = true;
    d.strength = 1.0;
    d.sensors.sensors = {Sensor::withDefaults(SensorId::DrawingAngle),
                         Sensor::withDefaults(SensorId::Fuzzy)};
    BrushOptions mixed;
    mixed.rotation = CurveOption(d);
    CHECK(mixed.rotation->isRandom());
    CHECK(reticleDabAngle(mixed, 0.25, StrokeInput{}, 0.5) == 0.25);
}

TEST_CASE("reticleDabAngle: drawing the ring cannot perturb the stroke") {
    // It builds its own StrokeState. If it ever reached for the engine's, a hover would advance the
    // random stream and a `fuzzy`-driven stroke would stop replaying -- which is what golden images,
    // the editor preview and undo/redo of that stroke all rest on.
    BrushOptions o;
    o.rotation = option("Rotation", SensorId::DrawingAngle);
    o.size = option("Size", SensorId::Fuzzy);

    DabBase base;
    base.diameter = 40.0;

    StrokeState s = strokeAt(1.0);
    const double first = evaluateDab(o, base, {0.0, 0.0}, s).diameter;

    StrokeState t = strokeAt(1.0);
    for (int i = 0; i < 50; ++i)
        (void)reticleDabAngle(o, 0.0, StrokeInput{}, 0.1 * i); // 50 hover frames
    CHECK(evaluateDab(o, base, {0.0, 0.0}, t).diameter == first);
}

// ------------------------------------------------------------------------------------------------
// §6.6d: the two positional options. Transcribed formulas, so the assertions are EXACT (==) where
// the arithmetic is a straight replay of predictable draws -- an Approx here would let a reordered
// draw or a dropped factor hide inside the tolerance.

namespace {

using mosaic::core::brush::applyMirror;
using mosaic::core::brush::applyScatter;
using mosaic::core::brush::applySharpnessSnap;
using mosaic::core::brush::MirrorOption;
using mosaic::core::brush::ScatterOption;

// The Scatter consumer's spec: strength spans [0,5], not the default [0,1].
[[nodiscard]] ScatterOption scatterOption(double strength, bool axisX, bool axisY,
                                          bool checked = true) {
    CurveOptionData d;
    d.name = "Scatter";
    d.checkable = true;
    d.checked = checked;
    d.strength = strength;
    d.strengthMax = 5.0;
    d.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
    return ScatterOption{CurveOption(d), axisX, axisY};
}

[[nodiscard]] MirrorOption mirrorOption(SensorId sensor, bool horizontal, bool vertical,
                                        bool checked = true) {
    CurveOptionData d;
    d.name = "Mirror";
    d.checkable = true;
    d.checked = checked;
    d.strength = 1.0;
    d.sensors.sensors = {Sensor::withDefaults(sensor)};
    return MirrorOption{CurveOption(d), horizontal, vertical};
}

// A stroke that has MOVED, so it has a drawing angle. Ends standing at (10*dx, 10*dy).
[[nodiscard]] StrokeState movedStroke(double dx, double dy, double pressure = 1.0) {
    StrokeState s;
    s.begin(at(0.0, 0.0, pressure), kSeed);
    s.extendTo(at(10.0 * dx, 10.0 * dy, pressure));
    s.beginDab();
    return s;
}

[[nodiscard]] Dab dabAtOrigin() {
    Dab d;
    d.center = {0.0, 0.0};
    return d;
}

} // namespace

TEST_CASE("applyScatter: both axes are two independent draws, X first, at extent x value") {
    // Predict the draws on a twin stroke: same seed, same history, so the same stream.
    StrokeState twin = movedStroke(1.0, 0.0);
    const double r1 = twin.nextRandom();
    const double r2 = twin.nextRandom();
    REQUIRE(r1 != r2); // or "independent draws" below tests nothing

    // Pressure 1 through an identity curve at strength 5: the sensor value is exactly 5.
    StrokeState s = movedStroke(1.0, 0.0);
    Dab d = dabAtOrigin();
    applyScatter(scatterOption(5.0, true, true), 40.0, 20.0, s, d);

    // jitter = (2r - 1) * max(40, 20) * 5 -- the same expression, the same doubles.
    CHECK(d.center.x == (2.0 * r1 - 1.0) * 40.0 * 5.0);
    CHECK(d.center.y == (2.0 * r2 - 1.0) * 40.0 * 5.0);
}

TEST_CASE("applyScatter: the strength clamps into the consumer's [0,5] span") {
    StrokeState twin = movedStroke(1.0, 0.0);
    const double r1 = twin.nextRandom();

    StrokeState s = movedStroke(1.0, 0.0);
    Dab d = dabAtOrigin();
    ScatterOption sc = scatterOption(7.0, true, false); // authored past the span
    applyScatter(sc, 10.0, 10.0, s, d);
    CHECK(d.center.x == (2.0 * r1 - 1.0) * 10.0 * 5.0); // evaluated at 5, not 7
}

TEST_CASE("applyScatter: one axis lies ALONG the stroke, the other ACROSS it") {
    // Due east (drawing angle 0): X-only jitter moves x alone; Y-only moves y alone. The zero leg
    // is exact -- sin(0) is 0.0 to the bit, and adding it must not move the centre.
    {
        StrokeState twin = movedStroke(1.0, 0.0);
        const double j = (2.0 * twin.nextRandom() - 1.0) * 30.0;
        StrokeState s = movedStroke(1.0, 0.0);
        Dab d = dabAtOrigin();
        applyScatter(scatterOption(1.0, true, false), 30.0, 30.0, s, d);
        CHECK(d.center.x == j);
        CHECK(d.center.y == 0.0);
    }
    {
        StrokeState twin = movedStroke(1.0, 0.0);
        const double j = (2.0 * twin.nextRandom() - 1.0) * 30.0;
        StrokeState s = movedStroke(1.0, 0.0);
        Dab d = dabAtOrigin();
        applyScatter(scatterOption(1.0, false, true), 30.0, 30.0, s, d);
        CHECK(d.center.x == 0.0); // -sin(0) * j
        CHECK(d.center.y == j);   //  cos(0) * j
    }
    // A slanted stroke (atan2(5, 10)): the one draw lands on BOTH document axes, in the stroke's
    // frame -- cos on x and sin on y for the along-axis, -sin on x and cos on y for the across.
    {
        StrokeState twin = movedStroke(1.0, 0.5);
        const double j = (2.0 * twin.nextRandom() - 1.0) * 30.0;
        const double a = std::atan2(5.0, 10.0);
        StrokeState s = movedStroke(1.0, 0.5);
        Dab d = dabAtOrigin();
        applyScatter(scatterOption(1.0, true, false), 30.0, 30.0, s, d);
        CHECK(d.center.x == std::cos(a) * j);
        CHECK(d.center.y == std::sin(a) * j);

        StrokeState s2 = movedStroke(1.0, 0.5);
        Dab d2 = dabAtOrigin();
        applyScatter(scatterOption(1.0, false, true), 30.0, 30.0, s2, d2);
        CHECK(d2.center.x == -std::sin(a) * j);
        CHECK(d2.center.y == std::cos(a) * j);
    }
}

TEST_CASE("applyScatter: inert forms draw NOTHING from the stream") {
    // Unchecked, or no axis: the centre holds AND the random stream holds. A preset that authors
    // its scatter off must replay byte-for-byte as one that never mentioned it -- the stream is
    // the contract, not just the centre.
    StrokeState control = movedStroke(1.0, 0.0);

    StrokeState unchecked = movedStroke(1.0, 0.0);
    Dab d = dabAtOrigin();
    applyScatter(scatterOption(5.0, true, true, /*checked=*/false), 40.0, 40.0, unchecked, d);
    CHECK(d.center.x == 0.0);
    CHECK(d.center.y == 0.0);
    CHECK(unchecked.nextRandom() == control.nextRandom());

    StrokeState axisless = movedStroke(1.0, 0.0);
    StrokeState control2 = movedStroke(1.0, 0.0);
    Dab d2 = dabAtOrigin();
    applyScatter(scatterOption(5.0, false, false), 40.0, 40.0, axisless, d2);
    CHECK(d2.center.x == 0.0);
    CHECK(d2.center.y == 0.0);
    CHECK(axisless.nextRandom() == control2.nextRandom());
}

TEST_CASE("applyMirror: the coin is 'value >= 0.5', per ENABLED axis") {
    // Pressure through an identity curve IS the value: 0.7 flips, 0.3 does not, and the boundary
    // 0.5 flips -- the reference's comparison is >=, and a mutation to > dies here.
    {
        StrokeState s = strokeAt(0.7);
        Dab d = dabAtOrigin();
        applyMirror(mirrorOption(SensorId::Pressure, true, true), s, d);
        CHECK(d.mirrorH);
        CHECK(d.mirrorV);
    }
    {
        StrokeState s = strokeAt(0.3);
        Dab d = dabAtOrigin();
        applyMirror(mirrorOption(SensorId::Pressure, true, true), s, d);
        CHECK(!d.mirrorH);
        CHECK(!d.mirrorV);
    }
    {
        StrokeState s = strokeAt(0.5);
        Dab d = dabAtOrigin();
        applyMirror(mirrorOption(SensorId::Pressure, true, true), s, d);
        CHECK(d.mirrorH);
    }
    // Axis gates: a flip decision only lands on the enabled axes.
    {
        StrokeState s = strokeAt(1.0);
        Dab d = dabAtOrigin();
        applyMirror(mirrorOption(SensorId::Pressure, true, false), s, d);
        CHECK(d.mirrorH);
        CHECK(!d.mirrorV);
    }
    {
        StrokeState s = strokeAt(1.0);
        Dab d = dabAtOrigin();
        applyMirror(mirrorOption(SensorId::Pressure, false, true), s, d);
        CHECK(!d.mirrorH);
        CHECK(d.mirrorV);
    }
}

TEST_CASE("applyMirror: a fuzzy mirror draws exactly ONE number; inert forms draw none") {
    // One draw for the whole decision -- both axes flip TOGETHER on one coin, which is what makes
    // a both-axes mirror a 180-degree turn rather than two independent flips.
    StrokeState twin = strokeAt(1.0);
    (void)twin.nextRandom(); // the coin the option will consume
    const double next = twin.nextRandom();

    StrokeState s = strokeAt(1.0);
    Dab d = dabAtOrigin();
    applyMirror(mirrorOption(SensorId::Fuzzy, true, true), s, d);
    CHECK(d.mirrorH == d.mirrorV); // one coin, both axes
    CHECK(s.nextRandom() == next); // exactly one draw happened

    // Unchecked or axis-less: the stream holds.
    StrokeState control = strokeAt(1.0);
    StrokeState unchecked = strokeAt(1.0);
    Dab d2 = dabAtOrigin();
    applyMirror(mirrorOption(SensorId::Fuzzy, true, true, /*checked=*/false), unchecked, d2);
    CHECK(!d2.mirrorH);
    CHECK(!d2.mirrorV);
    CHECK(unchecked.nextRandom() == control.nextRandom());

    StrokeState axisless = strokeAt(1.0);
    StrokeState control2 = strokeAt(1.0);
    Dab d3 = dabAtOrigin();
    applyMirror(mirrorOption(SensorId::Fuzzy, false, false), axisless, d3);
    CHECK(!d3.mirrorH);
    CHECK(!d3.mirrorV);
    CHECK(axisless.nextRandom() == control2.nextRandom());
}

// ------------------------------------------------------------------------------------------------
// §6.6e: the Sharpness coordinate snap. Pure geometry (no draw), so exact-equality predictions --
// the centre delta is the reference's own `sharpness*(round(pt) - pt)` on the mask top-left.

TEST_CASE("applySharpnessSnap: the centre delta is sharpness * (round(topLeft) - topLeft), per axis") {
    const double cx = 10.3, cy = 20.8, ew = 8.0, eh = 8.0;
    const double tlx = cx - 0.5 * ew; // the mask top-left placeDab will recompute
    const double tly = cy - 0.5 * eh;

    // Sharpness 1: full snap. The top-left lands on the integer grid, so the sub-pixel phase is 0.
    Dab full = dabAtOrigin();
    full.center = {cx, cy};
    applySharpnessSnap(1.0, ew, eh, full);
    CHECK(full.center.x == cx + (std::round(tlx) - tlx));
    CHECK(full.center.y == cy + (std::round(tly) - tly));
    // ... and with an even extent the resulting top-left really is that integer, to the bit.
    CHECK(full.center.x - 0.5 * ew == std::round(tlx));
    CHECK(full.center.y - 0.5 * eh == std::round(tly));

    // Sharpness 0: nothing moves at all.
    Dab none = dabAtOrigin();
    none.center = {cx, cy};
    applySharpnessSnap(0.0, ew, eh, none);
    CHECK(none.center.x == cx);
    CHECK(none.center.y == cy);

    // Something in between: a partial pull toward the grid, the exact lerp.
    Dab half = dabAtOrigin();
    half.center = {cx, cy};
    applySharpnessSnap(0.5, ew, eh, half);
    CHECK(half.center.x == cx + 0.5 * (std::round(tlx) - tlx));
    CHECK(half.center.y == cy + 0.5 * (std::round(tly) - tly));
}

// ------------------------------------------------------------------------------------------------
// Colour dynamics (§6.6f): the HSV adjustments to the paint colour.

using mosaic::common::Color8;
using mosaic::core::brush::applyColorDynamics;
using mosaic::core::brush::hsvAdjust;

TEST_CASE("hsvAdjust: the identity leaves a saturated primary byte-for-byte") {
    // dh=ds=dv=0 with a channel notionally "on" round-trips RGB->HSV->RGB; a pure primary survives
    // the trip exactly (its reconstruction lands back on the same 8-bit value).
    const Color8 red{255, 0, 0, 255};
    CHECK(hsvAdjust(red, 0.0, 0.0, 0.0) == red);
    // A neutral grey has no chroma at all, so it is trivially unmoved -- and its alpha passes through.
    const Color8 grey{128, 128, 128, 200};
    CHECK(hsvAdjust(grey, 0.0, 0.0, 0.0) == grey);
}

TEST_CASE("hsvAdjust: hue is a HALF-turn rotation -- dh*180 degrees around the wheel") {
    const Color8 red{255, 0, 0, 255};
    // +120 deg: red -> green. dh*180 = 120 -> dh = 2/3.
    CHECK(hsvAdjust(red, 2.0 / 3.0, 0.0, 0.0) == Color8{0, 255, 0, 255});
    // -120 deg: red -> blue.
    CHECK(hsvAdjust(red, -2.0 / 3.0, 0.0, 0.0) == Color8{0, 0, 255, 255});
    // 180 deg: red -> cyan (dh = 1, the top of the range).
    CHECK(hsvAdjust(red, 1.0, 0.0, 0.0) == Color8{0, 255, 255, 255});
}

TEST_CASE("hsvAdjust: value drives toward black (dv<0) and white (dv>0)") {
    const Color8 c{200, 100, 50, 255};
    // Full value movement: dv = -1 pulls to black, dv = +1 pushes to white -- both base-independent.
    CHECK(hsvAdjust(c, 0.0, 0.0, -1.0) == Color8{0, 0, 0, 255});
    CHECK(hsvAdjust(c, 0.0, 0.0, 1.0) == Color8{255, 255, 255, 255});
}

TEST_CASE("hsvAdjust: saturation ds = -1 desaturates fully to the value grey") {
    // ds = -1 => chroma *= (ds + 1) = 0, so the colour collapses to its own value on the grey axis.
    // For (200,100,50) the value is max = 200/255, so every channel lands on 200.
    const Color8 c{200, 100, 50, 255};
    CHECK(hsvAdjust(c, 0.0, -1.0, 0.0) == Color8{200, 200, 200, 255});
}

TEST_CASE("applyColorDynamics: an inert set draws NOTHING and returns the base, byte-for-byte") {
    const Color8 base{123, 45, 200, 255};
    // No colour-dynamics options at all: the reference builds no transformation, so the colour and
    // the random stream are both untouched.
    {
        StrokeState s = strokeAt(1.0);
        StrokeState control = strokeAt(1.0);
        CHECK(applyColorDynamics(BrushOptions{}, base, s) == base);
        CHECK(s.nextRandom() == control.nextRandom());
    }
    // All three present but UNCHECKED: same contract -- no draw, no change (and byte-exact, which a
    // round trip through hsvAdjust would not guarantee -- that is why the inert path returns base).
    {
        BrushOptions o;
        o.hue = option("h", SensorId::Fuzzy, 1.0, /*checked=*/false);
        o.saturation = option("s", SensorId::Fuzzy, 1.0, false);
        o.value = option("v", SensorId::Fuzzy, 1.0, false);
        StrokeState s = strokeAt(1.0);
        StrokeState control = strokeAt(1.0);
        CHECK(applyColorDynamics(o, base, s) == base);
        CHECK(s.nextRandom() == control.nextRandom());
    }
}

TEST_CASE("applyColorDynamics: value drives the paint colour, through the reference's remap") {
    // A `value` option on a deterministic Pressure sensor (no RNG), strength 1. The remap is
    //     raw = sizeLikeValue(WITH strength) = clamp(S*P, 0, 1);  dv = 2*(raw*S + (0.5 - 0.5*S)) - 1
    // so at S=1 it is dv = 2P - 1: P=1 -> white, P=0 -> black, P=0.5 -> the neutral midpoint.
    const Color8 red{255, 0, 0, 255};
    auto valueDab = [&](double pressure) {
        BrushOptions o;
        o.value = option("v", SensorId::Pressure, 1.0);
        StrokeState s = strokeAt(pressure);
        return applyColorDynamics(o, red, s);
    };
    CHECK(valueDab(1.0) == Color8{255, 255, 255, 255}); // dv = +1
    CHECK(valueDab(0.0) == Color8{0, 0, 0, 255});       // dv = -1
    CHECK(valueDab(0.5) == red);                         // dv = 0: pure primary round-trips exact
}

TEST_CASE("applyColorDynamics: hue rotates the paint colour off the Pressure sensor") {
    // Pressure 1, strength 1: rotationLikeValue = wrapValue(2P - 1) = 1, which wraps to -1 -- and
    // -180 deg and +180 deg are the SAME hue, so red -> cyan either way (the wrap is not observable
    // here, deliberately: the boundary is the safe place to pin the sensor path).
    const Color8 red{255, 0, 0, 255};
    BrushOptions o;
    o.hue = option("h", SensorId::Pressure, 1.0);
    StrokeState s = strokeAt(1.0);
    CHECK(applyColorDynamics(o, red, s) == Color8{0, 255, 255, 255});
}

TEST_CASE("applyColorDynamics: hue draws before value, and each fuzzy channel draws exactly once") {
    // The reference order is hue, saturation, value. With hue and value both fuzzy, re-derive the two
    // adjustments on a TWIN stroke -- hue first (rotationLikeValue), value second (sizeLikeValue +
    // the remap) -- and compose them exactly as applyColorDynamics does. Equality pins the order,
    // the composition and the per-channel draw count together.
    const Color8 base{200, 100, 50, 255};
    const CurveOption hue = option("h", SensorId::Fuzzy);
    const CurveOption val = option("v", SensorId::Fuzzy);

    StrokeState twin = strokeAt(1.0);
    const double dh = hue.rotationLikeValue(twin, 0.0, false, 1.0, false); // draw 1
    const double rawV = val.sizeLikeValue(twin, /*useStrength=*/true);     // draw 2
    const double S = val.data().strength;
    const double dv = 2.0 * (rawV * S + (0.5 - S * 0.5)) - 1.0;
    const Color8 expected = hsvAdjust(base, dh, 0.0, dv);
    const double afterTwo = twin.nextRandom(); // the stream position after the two draws

    BrushOptions o;
    o.hue = hue;
    o.value = val;
    StrokeState s = strokeAt(1.0);
    const Color8 got = applyColorDynamics(o, base, s);
    CHECK(got == expected);
    CHECK(s.nextRandom() == afterTwo); // exactly two draws, hue then value

    // With saturation ALSO checked, the full order is hue, saturation, value -- three draws. A
    // saturation draw slots BETWEEN the other two, so a swapped order (or a saturation that drew out
    // of place) would move the value channel's draw and the result with it.
    const CurveOption sat = option("s", SensorId::Fuzzy);
    const double sS = sat.data().strength;
    StrokeState twin3 = strokeAt(1.0);
    const double dh3 = hue.rotationLikeValue(twin3, 0.0, false, 1.0, false);
    const double ds3 = 2.0 * (sat.sizeLikeValue(twin3, true) * sS + (0.5 - sS * 0.5)) - 1.0;
    const double dv3 = 2.0 * (val.sizeLikeValue(twin3, true) * S + (0.5 - S * 0.5)) - 1.0;
    const Color8 expected3 = hsvAdjust(base, dh3, ds3, dv3);

    BrushOptions three;
    three.hue = hue;
    three.saturation = sat;
    three.value = val;
    StrokeState s3 = strokeAt(1.0);
    CHECK(applyColorDynamics(three, base, s3) == expected3);
}
