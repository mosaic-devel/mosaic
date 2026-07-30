#include <doctest/doctest.h>

#include "core/brush/curve_option.hpp"

#include <cmath>
#include <map>
#include <string>

using mosaic::core::brush::CombineMode;
using mosaic::core::brush::combineModeFromInt;
using mosaic::core::brush::Curve;
using mosaic::core::brush::CurveOption;
using mosaic::core::brush::CurveOptionData;
using mosaic::core::brush::CurveOptionKeys;
using mosaic::core::brush::PropertyLookup;
using mosaic::core::brush::readCurveOption;
using mosaic::core::brush::scalingToAdditive;
using mosaic::core::brush::Sensor;
using mosaic::core::brush::SensorId;
using mosaic::core::brush::SensorList;
using mosaic::core::brush::StrokeInput;
using mosaic::core::brush::StrokeState;
using mosaic::core::brush::ValueComponents;

namespace {

constexpr std::uint64_t kSeed = 0xC0FFEE1234ULL;

// A stroke with a known pressure and a settled eastward heading.
[[nodiscard]] StrokeState strokeAt(double pressure) {
    StrokeState s;
    StrokeInput in;
    in.pressure = pressure;
    s.begin(in, kSeed);
    in.pos = {10.0, 0.0};
    in.timeUs = 1000;
    s.extendTo(in);
    s.beginDab();
    return s;
}

[[nodiscard]] PropertyLookup lookup(const std::map<std::string, std::string>& m) {
    return [m](std::string_view key) -> std::optional<std::string> {
        const auto it = m.find(std::string(key));
        if (it == m.end())
            return std::nullopt;
        return it->second;
    };
}

// An option whose only sensor is `id`, with an identity curve and full strength.
[[nodiscard]] CurveOptionData optionWith(SensorId id, std::string name = "Size") {
    CurveOptionData d;
    d.name = std::move(name);
    d.checkable = false;
    d.sensors.sensors = {Sensor::withDefaults(id)};
    return d;
}

} // namespace

TEST_CASE("brush curve option: combineModeFromInt maps the five modes and defaults the rest") {
    CHECK(combineModeFromInt(0) == CombineMode::Multiply);
    CHECK(combineModeFromInt(1) == CombineMode::Add);
    CHECK(combineModeFromInt(2) == CombineMode::Max);
    CHECK(combineModeFromInt(3) == CombineMode::Min);
    CHECK(combineModeFromInt(4) == CombineMode::Difference);
    // A preset from a newer version must still paint.
    CHECK(combineModeFromInt(5) == CombineMode::Multiply);
    CHECK(combineModeFromInt(-1) == CombineMode::Multiply);
}

TEST_CASE("brush curve option: a lone scaling sensor is taken verbatim, whatever the combine mode") {
    // combineMode needs two things to combine. This is why it is inert on almost every real preset.
    for (const int mode : {0, 1, 2, 3, 4}) {
        CAPTURE(mode);
        CurveOptionData d = optionWith(SensorId::Pressure);
        d.combineMode = combineModeFromInt(mode);
        const CurveOption opt(d);
        StrokeState s = strokeAt(0.4);
        CHECK(opt.sizeLikeValue(s) == doctest::Approx(0.4));
    }
}

TEST_CASE("brush curve option: the five combine modes fold two scaling sensors") {
    // pressure = 0.25 and tangentialpressure = 0.75, both scaling, both identity curves.
    const auto build = [](CombineMode mode) {
        CurveOptionData d = optionWith(SensorId::Pressure);
        d.sensors.sensors.push_back(Sensor::withDefaults(SensorId::TangentialPressure));
        d.combineMode = mode;
        return CurveOption(d);
    };
    const auto run = [](const CurveOption& opt) {
        StrokeState s;
        StrokeInput in;
        in.pressure = 0.25;
        in.tangentialPressure = 0.75;
        s.begin(in, kSeed);
        return opt.sizeLikeValue(s);
    };

    CHECK(run(build(CombineMode::Multiply)) == doctest::Approx(0.25 * 0.75));
    CHECK(run(build(CombineMode::Add)) == doctest::Approx(1.0)); // 1.0, then clamped by strengthMax
    CHECK(run(build(CombineMode::Max)) == doctest::Approx(0.75));
    CHECK(run(build(CombineMode::Min)) == doctest::Approx(0.25));
    CHECK(run(build(CombineMode::Difference)) == doctest::Approx(0.5));
}

TEST_CASE("brush curve option: additive sensors sum and bypass the combine mode") {
    // rotation + fuzzy are both additive. Whatever combineMode says, they sum into their own slot.
    CurveOptionData d = optionWith(SensorId::Rotation);
    d.sensors.sensors.push_back(Sensor::withDefaults(SensorId::FuzzyStroke));
    d.combineMode = CombineMode::Min; // would pick the smaller if these were scaling sensors
    const CurveOption opt(d);

    StrokeState s;
    StrokeInput in;
    in.rotation = 90.0; // additive value +0.5
    s.begin(in, kSeed);

    const ValueComponents c = opt.compute(s);
    CHECK_FALSE(c.hasScaling);
    CHECK(c.hasAdditive);
    CHECK_FALSE(c.hasAbsoluteOffset);
    CHECK(c.scaling == doctest::Approx(1.0)); // untouched

    const double fuzzy = scalingToAdditive(s.strokeRandom("Size"));
    CHECK(c.additive == doctest::Approx(0.5 + fuzzy)); // a SUM, not a min
}

TEST_CASE("brush curve option: drawingangle overwrites rather than sums, and bypasses combine") {
    CurveOptionData d = optionWith(SensorId::DrawingAngle, "Rotation");
    const CurveOption opt(d);

    StrokeState s = strokeAt(1.0); // heading east -> 0.5
    const ValueComponents c = opt.compute(s);
    CHECK(c.hasAbsoluteOffset);
    CHECK_FALSE(c.hasScaling);
    CHECK_FALSE(c.hasAdditive);
    CHECK(c.absoluteOffset == doctest::Approx(0.5));

    // Two absolute-rotation sensors cannot both exist (there is only one), but a repeated entry
    // proves the slot is assigned rather than accumulated.
    CurveOptionData twice = d;
    twice.sensors.sensors.push_back(Sensor::withDefaults(SensorId::DrawingAngle));
    StrokeState s2 = strokeAt(1.0);
    const ValueComponents c2 = CurveOption(twice).compute(s2);
    CHECK(c2.absoluteOffset == doctest::Approx(0.5)); // not 1.0
}

TEST_CASE("brush curve option: sizeLikeValue multiplies the three components") {
    ValueComponents c;
    c.constant = 0.8;
    c.maxSizeLike = 1.0;

    // Nothing active: the option is its constant.
    CHECK(c.sizeLikeValue() == doctest::Approx(0.8));

    // An absent additive slot contributes 1, NOT additiveToScaling(0) == 0.5.
    c.hasScaling = true;
    c.scaling = 0.5;
    CHECK(c.sizeLikeValue() == doctest::Approx(0.4));

    // ...whereas an additive sensor reading 0 does halve it.
    c.hasAdditive = true;
    c.additive = 0.0;
    CHECK(c.sizeLikeValue() == doctest::Approx(0.2));

    // Clamped into the option's own range, not into [0,1].
    c.minSizeLike = 0.3;
    CHECK(c.sizeLikeValue() == doctest::Approx(0.3));

    // A non-finite product degrades to the floor rather than escaping into a dab.
    c.constant = std::numeric_limits<double>::infinity();
    CHECK(c.sizeLikeValue() == doctest::Approx(0.3));
}

TEST_CASE("brush curve option: rotationLikeValue re-centres scaling and wraps") {
    ValueComponents c;
    c.constant = 1.0;

    // No absolute sensor: the dab inherits the canvas angle, doubled into the [-1,1) turn space.
    CHECK(c.rotationLikeValue(0.25, false, 1.0, false) == doctest::Approx(0.5));
    CHECK(c.rotationLikeValue(0.75, false, 1.0, false) == doctest::Approx(-0.5)); // wrap(1.5)

    // An absolute sensor IS the angle, and the canvas angle is then ignored entirely.
    c.hasAbsoluteOffset = true;
    c.absoluteOffset = 0.1;
    CHECK(c.rotationLikeValue(0.9, false, 1.0, false) == doctest::Approx(0.2));
    // A single-axis mirror reverses its sense about the half turn: 0.5 - 0.1 = 0.4, doubled.
    CHECK(c.rotationLikeValue(0.9, true, 1.0, false) == doctest::Approx(0.8));

    // A scaling sensor contributes to an angle only after re-centring: 0.5 must mean "no rotation".
    ValueComponents s;
    s.hasScaling = true;
    s.scaling = 0.5;
    CHECK(s.rotationLikeValue(0.0, false, 1.0, false) == doctest::Approx(0.0));
    s.scaling = 1.0;
    CHECK(s.rotationLikeValue(0.0, false, 1.0, false) == doctest::Approx(1.0 - 2.0)); // wrapped
    // ...and disableScalingPart drops it entirely.
    CHECK(s.rotationLikeValue(0.0, false, 1.0, true) == doctest::Approx(0.0));

    // The result always lands in [-1,1).
    ValueComponents big;
    big.hasAdditive = true;
    big.additive = 37.5;
    const double v = big.rotationLikeValue(0.3, false, 1.0, false);
    CHECK(v >= -1.0);
    CHECK(v < 1.0);
}

TEST_CASE("brush curve option: the response curve is applied in each sensor's own class space") {
    // A curve that inverts: y = 1 - x.
    const Curve invert = Curve::fromString("0,1;1,0;");

    // Scaling: applied directly.
    {
        CurveOptionData d = optionWith(SensorId::Pressure);
        d.commonCurve = invert;
        StrokeState s = strokeAt(0.25);
        CHECK(CurveOption(d).sizeLikeValue(s) == doctest::Approx(0.75).epsilon(0.01));
    }

    // Additive: carried into [0,1], mapped, carried back. rotation 90deg -> +0.5 -> 0.75 -> 0.25
    // -> inverted -> -0.5.
    {
        CurveOptionData d = optionWith(SensorId::Rotation);
        d.commonCurve = invert;
        StrokeState s;
        StrokeInput in;
        in.rotation = 90.0;
        s.begin(in, kSeed);
        const ValueComponents c = CurveOption(d).compute(s);
        CHECK(c.additive == doctest::Approx(-0.5).epsilon(0.02));
    }

    // Absolute rotation: carried through a HALF TURN either side, so the curve's ends meet where the
    // angle wraps rather than at the heading a stroke most often has. East (0.5) -> wrap -> 0.0
    // -> inverted -> 1.0 -> wrap -> 0.5. An inverting curve leaves due east exactly where it was.
    {
        CurveOptionData d = optionWith(SensorId::DrawingAngle, "Rotation");
        d.commonCurve = invert;
        StrokeState s = strokeAt(1.0); // east
        const ValueComponents c = CurveOption(d).compute(s);
        CHECK(c.absoluteOffset == doctest::Approx(0.5).epsilon(0.02));
    }

    // An identity curve is skipped entirely, and the class round trip is a no-op, so the two paths
    // must agree. (If they did not, a preset would change behaviour merely by spelling out its
    // identity curve -- which shipped files do.)
    {
        CurveOptionData spelled = optionWith(SensorId::DrawingAngle, "Rotation");
        spelled.commonCurve = Curve::fromString("0,0;1,1;");
        CurveOptionData omitted = optionWith(SensorId::DrawingAngle, "Rotation");
        StrokeState a = strokeAt(1.0);
        StrokeState b = strokeAt(1.0);
        CHECK(CurveOption(spelled).compute(a).absoluteOffset ==
              doctest::Approx(CurveOption(omitted).compute(b).absoluteOffset));
    }
}

TEST_CASE("brush curve option: useSameCurve overrides every sensor's own curve") {
    CurveOptionData d = optionWith(SensorId::Pressure);
    d.sensors.sensors[0].curve = Curve::fromString("0,1;1,0;"); // the sensor's own: inverting
    d.commonCurve = Curve();                                    // the shared one: identity

    d.useSameCurve = true; // the default -- the shared curve wins, so pressure passes through
    StrokeState s1 = strokeAt(0.25);
    CHECK(CurveOption(d).sizeLikeValue(s1) == doctest::Approx(0.25));

    d.useSameCurve = false; // now each sensor answers to its own
    StrokeState s2 = strokeAt(0.25);
    CHECK(CurveOption(d).sizeLikeValue(s2) == doctest::Approx(0.75).epsilon(0.01));
}

TEST_CASE("brush curve option: useCurve gates the sensors, not merely their curves") {
    // The key name says "curve"; the behaviour is "dynamics off". A pressure-driven size option with
    // useCurve=false paints at its constant strength, not at the pressure.
    CurveOptionData d = optionWith(SensorId::Pressure);
    d.strength = 0.6;
    d.useCurve = false;

    StrokeState s = strokeAt(0.1);
    const CurveOption opt(d);
    const ValueComponents c = opt.compute(s);
    CHECK_FALSE(c.hasScaling);
    CHECK(opt.sizeLikeValue(s) == doctest::Approx(0.6));

    // ...and the strength can be ignored by the caller that applies it itself.
    CHECK(opt.sizeLikeValue(s, false) == doctest::Approx(1.0));
}

TEST_CASE("brush curve option: isChecked and isRandom") {
    CurveOptionData off = optionWith(SensorId::Pressure);
    off.checkable = true;
    off.checked = false;
    CHECK_FALSE(CurveOption(off).isChecked());
    off.checked = true;
    CHECK(CurveOption(off).isChecked());

    // Opacity and Flow are always on; the gate does not apply to them.
    CurveOptionData always = optionWith(SensorId::Pressure, "Opacity");
    always.checkable = false;
    always.checked = false;
    CHECK(CurveOption(always).isChecked());

    CHECK_FALSE(CurveOption(optionWith(SensorId::Pressure)).isRandom());
    CHECK(CurveOption(optionWith(SensorId::Fuzzy)).isRandom());
    CHECK(CurveOption(optionWith(SensorId::FuzzyStroke)).isRandom());
}

TEST_CASE("brush curve option: fuzzystroke is keyed on the option, so options scatter apart") {
    const CurveOption size(optionWith(SensorId::FuzzyStroke, "Size"));
    const CurveOption rot(optionWith(SensorId::FuzzyStroke, "Rotation"));

    StrokeState s = strokeAt(1.0);
    const double a = size.compute(s).additive;
    const double b = rot.compute(s).additive;
    CHECK(a != doctest::Approx(b));

    // Both are constant across the stroke, however many dabs and in whatever order they are read.
    for (int i = 0; i < 8; ++i) {
        s.beginDab();
        CHECK(rot.compute(s).additive == doctest::Approx(b));
        CHECK(size.compute(s).additive == doctest::Approx(a));
    }
}

TEST_CASE("brush curve option: the seven keys are built with the historical Pressure prefix") {
    const CurveOptionKeys k = CurveOptionKeys::forBase("Size");
    CHECK(k.enabled == "PressureSize"); // a PREFIX, and nothing to do with the pressure sensor
    CHECK(k.sensor == "SizeSensor");
    CHECK(k.value == "SizeValue");
    CHECK(k.useCurve == "SizeUseCurve");
    CHECK(k.useSameCurve == "SizeUseSameCurve");
    CHECK(k.curveMode == "SizecurveMode"); // lowercase 'c', as the format spells it
    CHECK(k.commonCurve == "SizecommonCurve");
    CHECK(k.legacyCustom == "CustomSize");
    CHECK(k.legacyCurve == "CurveSize");
}

TEST_CASE("brush curve option: a base name may contain slashes, and Pressure still goes in front") {
    // `Texture/Strength/` is a base name, not a prefix. Verified in d)_Ink-7_Brush_Rough.kpp, which
    // carries `PressureTexture/Strength/` -- trailing slash and all.
    const CurveOptionKeys k = CurveOptionKeys::forBase("Texture/Strength/");
    CHECK(k.enabled == "PressureTexture/Strength/");
    CHECK(k.sensor == "Texture/Strength/Sensor");
    CHECK(k.value == "Texture/Strength/Value");
    CHECK(k.useCurve == "Texture/Strength/UseCurve");
    CHECK(k.curveMode == "Texture/Strength/curveMode");

    // The nested masking brush is the OTHER rule: a prefixed property table whose option names are
    // plain, so its enable key reads `MaskingBrush/Preset/PressureSize`. A reader handles that by
    // prefix-stripping the lookup, never by mangling the base name -- which is why this function
    // knows nothing about it.
    CHECK(CurveOptionKeys::forBase("Size").enabled == "PressureSize");
}

TEST_CASE("brush curve option: the one multi-sensor option in the whole default set") {
    // Verbatim from d)_Ink-7_Brush_Rough.kpp: a sensorslist of fade + pressure, combineMode 1 (add),
    // useSameCurve false, no commonCurve property at all. Note `id` is the THIRD attribute on the
    // fade child, and that the option's base name carries slashes.
    const CurveOptionData d = readCurveOption(
        "Texture/Strength/",
        lookup({
            {"PressureTexture/Strength/", "true"},
            {"Texture/Strength/Sensor",
             R"(<!DOCTYPE params> <params id="sensorslist"> <ChildSensor periodic="0" length="1000" id="fade"> <curve>0,1;1,0;</curve> </ChildSensor> <ChildSensor id="pressure"> <curve>0,0;1,1;</curve> </ChildSensor> </params> )"},
            {"Texture/Strength/UseSameCurve", "false"},
            {"Texture/Strength/Value", "1"},
            {"Texture/Strength/curveMode", "1"},
        }));

    CHECK(d.combineMode == CombineMode::Add);
    CHECK_FALSE(d.useSameCurve);
    REQUIRE(d.sensors.sensors.size() == 2);
    CHECK(d.sensors.sensors[0].id == SensorId::Fade);
    CHECK(d.sensors.sensors[0].range.length == 1000);
    CHECK_FALSE(d.sensors.sensors[0].range.periodic);
    CHECK(d.sensors.sensors[1].id == SensorId::Pressure);

    const CurveOption opt(d);
    CHECK(opt.isChecked());

    // Both sensors are scaling, so combineMode is live here -- one of the few presets where it is.
    // At the stroke's first dab, fade reads 0 and its inverting curve turns that into 1; pressure
    // passes through its identity curve. Add: 1 + p, clamped to the option's max of 1.
    StrokeState s = strokeAt(0.3);
    const ValueComponents c = opt.compute(s);
    CHECK(c.hasScaling);
    CHECK(c.scaling == doctest::Approx(1.0 + 0.3).epsilon(0.02));
    CHECK(opt.sizeLikeValue(s) == doctest::Approx(1.0)); // clamped

    // Under Multiply the same two sensors would give 1 * 0.3 instead -- proving the mode is read.
    CurveOptionData mult = d;
    mult.combineMode = CombineMode::Multiply;
    StrokeState s2 = strokeAt(0.3);
    CHECK(CurveOption(mult).sizeLikeValue(s2) == doctest::Approx(0.3).epsilon(0.02));
}

TEST_CASE("brush curve option: a shipped periodic time sensor, duration and all") {
    // Verbatim from j)_Waterpaint_Hard_Edges.kpp -- the only `time` sensor in the default set, and
    // proof in a real file that Time spells its length `duration`. `id` is again the third attribute.
    const CurveOptionData d = readCurveOption(
        "Softness", lookup({{"PressureSoftness", "true"},
                            {"SoftnessSensor",
                             R"(<!DOCTYPE params> <params periodic="1" duration="1470" id="time"/> )"},
                            {"SoftnesscurveMode", "0"}}),
        true, 0.1, 1.0); // Softness's own strength range

    REQUIRE(d.sensors.sensors.size() == 1);
    CHECK(d.sensors.sensors[0].id == SensorId::Time);
    CHECK(d.sensors.sensors[0].range.periodic);
    CHECK(d.sensors.sensors[0].range.length == 1470); // ms
    CHECK(d.strength == doctest::Approx(1.0));        // absent {X}Value -> the top of the range

    const CurveOption opt(d);
    CHECK(opt.isChecked());

    // Softness never falls below 0.1, whatever the sensor says -- the range is the consumer's.
    StrokeState s;
    StrokeInput in;
    s.begin(in, kSeed);
    CHECK(opt.sizeLikeValue(s) == doctest::Approx(0.1)); // time 0 at the stroke's start

    in.timeUs = 735'000; // half of 1470 ms
    s.extendTo(in);
    CHECK(opt.sizeLikeValue(s) == doctest::Approx(0.5).epsilon(0.02));

    // Periodic: at exactly one period the sawtooth is back at 0, not saturated at 1.
    in.timeUs = 1'470'000;
    s.extendTo(in);
    CHECK(opt.sizeLikeValue(s) == doctest::Approx(0.1));
}

TEST_CASE("brush curve option: readCurveOption applies the reference defaults") {
    // An empty table. Every default has to come out right, because most presets omit most keys.
    const CurveOptionData empty = readCurveOption("Size", lookup({}));
    CHECK(empty.name == "Size");
    CHECK_FALSE(empty.checked);        // PressureSize absent -> off
    CHECK(empty.useCurve);             // absent -> true
    CHECK(empty.useSameCurve);         // absent -> true
    CHECK(empty.combineMode == CombineMode::Multiply);
    CHECK(empty.strength == doctest::Approx(1.0)); // absent -> the TOP of the range
    // An absent {X}Sensor is not "no sensor": it floors to pressure with an identity curve.
    REQUIRE(empty.sensors.sensors.size() == 1);
    CHECK(empty.sensors.sensors[0].id == SensorId::Pressure);
    CHECK(empty.commonCurve.isIdentity());

    // The strength default follows the consumer's range, not the number 1.
    const CurveOptionData ranged = readCurveOption("Rotation", lookup({}), true, 0.0, 0.5);
    CHECK(ranged.strength == doctest::Approx(0.5));

    // Opacity and Flow are always on.
    CHECK(readCurveOption("Opacity", lookup({}), false).checkable == false);
}

TEST_CASE("brush curve option: readCurveOption reads a real preset's property shape") {
    const CurveOptionData d = readCurveOption(
        "Size", lookup({
                    {"PressureSize", "true"},
                    {"SizeSensor",
                     R"(<!DOCTYPE params> <params id="drawingangle" lockedAngleMode="0" angleOffset="0" fanCornersEnabled="1" fanCornersStep="90"> <curve>0,0;1,1;</curve> </params> )"},
                    {"SizeValue", "0.8"},
                    {"SizeUseCurve", "true"},
                    {"SizeUseSameCurve", "false"},
                    {"SizecurveMode", "2"},
                    {"SizecommonCurve", "0,1;1,0;"},
                }));

    CHECK(d.checked);
    CHECK(d.strength == doctest::Approx(0.8));
    CHECK(d.combineMode == CombineMode::Max);
    CHECK_FALSE(d.useSameCurve);
    REQUIRE(d.sensors.sensors.size() == 1);
    CHECK(d.sensors.sensors[0].id == SensorId::DrawingAngle);
    CHECK(d.sensors.sensors[0].fan.fanCornersStep == 90);

    // useSameCurve is off, so commonCurve is NOT taken from the property -- it seeds from the
    // sensor's own curve, which here is the identity the XML spelled out.
    CHECK(d.commonCurve.isIdentity());
}

TEST_CASE("brush curve option: the commonCurve seeds from the last sensor the XML defined") {
    const CurveOptionData d = readCurveOption(
        "Size", lookup({{"SizeSensor",
                         R"(<params id="sensorslist">)"
                         R"(<ChildSensor id="pressure"><curve>0,0.25;1,1;</curve></ChildSensor>)"
                         R"(<ChildSensor id="speed"><curve>0,1;1,0;</curve></ChildSensor>)"
                         R"(</params>)"}}));
    REQUIRE(d.sensors.sensors.size() == 2);
    CHECK(d.useSameCurve);
    // The LAST child's curve, not the first, not a merge. A legacy of one-curve-per-option.
    CHECK(d.commonCurve.toString() == "0,1;1,0;");

    // An explicit commonCurve property wins over the seed.
    const CurveOptionData explicitly =
        readCurveOption("Size", lookup({{"SizeSensor", R"(<params id="pressure"/>)"},
                                        {"SizecommonCurve", "0,0.5;1,1;"}}));
    CHECK(explicitly.commonCurve.toString() == "0,0.5;1,1;");
}

TEST_CASE("brush curve option: the legacy Custom{X}/Curve{X} pair is honoured when no <curve> shipped") {
    // Pre-2.x: the sensor XML carries no <curve> at all, and the single shared curve lives in
    // Curve{X}, gated by Custom{X}. The discriminator is the SERIALIZED form -- a parsed identity
    // curve is indistinguishable from an absent one, and shipped files write both.
    const CurveOptionData legacy = readCurveOption("Size", lookup({
                                                                 {"SizeSensor", R"(<params id="pressure"/>)"},
                                                                 {"CustomSize", "true"},
                                                                 {"CurveSize", "0,1;1,0;"},
                                                             }));
    CHECK(legacy.commonCurve.toString() == "0,1;1,0;");
    REQUIRE(legacy.sensors.sensors.size() == 1);
    CHECK(legacy.sensors.sensors[0].curve.toString() == "0,1;1,0;"); // pushed onto every sensor

    // Custom{X} false: the legacy curve is ignored and the option is the identity.
    const CurveOptionData off = readCurveOption("Size", lookup({
                                                              {"SizeSensor", R"(<params id="pressure"/>)"},
                                                              {"CustomSize", "false"},
                                                              {"CurveSize", "0,1;1,0;"},
                                                          }));
    CHECK(off.commonCurve.isIdentity());

    // A sensor XML that DOES carry <curve> takes the modern path, and the legacy pair is dead --
    // even when the shipped curve is the identity, which is exactly the case that trips a reader
    // that discriminates on the parsed curve rather than on the serialized form.
    const CurveOptionData modern =
        readCurveOption("Size", lookup({
                                    {"SizeSensor", R"(<params id="pressure"><curve>0,0;1,1;</curve></params>)"},
                                    {"CustomSize", "true"},
                                    {"CurveSize", "0,1;1,0;"},
                                }));
    CHECK(modern.commonCurve.isIdentity()); // NOT the legacy inverting curve
}

TEST_CASE("brush curve option: a garbage property table still yields a paintable option") {
    const CurveOptionData d = readCurveOption("Size", lookup({
                                                          {"PressureSize", "banana"},
                                                          {"SizeSensor", "<not xml"},
                                                          {"SizeValue", "abc"},
                                                          {"SizeUseCurve", ""},
                                                          {"SizecurveMode", "99"},
                                                          {"SizecommonCurve", "###"},
                                                      }));
    CHECK_FALSE(d.checked);                        // unparsable bool -> the default
    CHECK(d.strength == doctest::Approx(1.0));     // unparsable double -> the default
    CHECK(d.useCurve);                             // empty -> the default
    CHECK(d.combineMode == CombineMode::Multiply); // out-of-range -> the default
    CHECK(d.commonCurve.isIdentity());             // unparsable curve -> the identity
    REQUIRE(d.sensors.sensors.size() == 1);
    CHECK(d.sensors.sensors[0].id == SensorId::Pressure);

    StrokeState s = strokeAt(0.5);
    CHECK(CurveOption(d).sizeLikeValue(s) == doctest::Approx(0.5));
}

TEST_CASE("brush curve option: the pressure-driven size option every preset actually uses") {
    // The single most common shape in the default set: Size, checked, one pressure sensor, a curve.
    const CurveOptionData d =
        readCurveOption("Size", lookup({{"PressureSize", "true"},
                                        {"SizeSensor", R"(<params id="pressure"/>)"},
                                        {"SizecommonCurve", "0,0;1,1;"}}));
    const CurveOption opt(d);
    CHECK(opt.isChecked());
    CHECK_FALSE(opt.isRandom());

    // Size tracks pressure exactly through the identity curve, and a zero-pressure sample yields a
    // zero-size dab rather than a full one.
    for (const double p : {0.0, 0.25, 0.5, 1.0}) {
        CAPTURE(p);
        StrokeState s = strokeAt(p);
        CHECK(opt.sizeLikeValue(s) == doctest::Approx(p));
    }
}
