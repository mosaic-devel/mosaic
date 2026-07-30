#include "io/brush/myb.hpp"

#include "core/brush/curve_option.hpp"
#include "core/brush/sensors.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// The MyPaint .myb reader (io/brush/myb.hpp, docs/brushes.md §6.7). Three SYNTHETIC presets
// serve as fixtures, each modeled on a structure the shipped Krita corpus carries (the real
// files stay out of the tree -- their per-file licensing is undeclared upstream; the
// system-corpus case below replays them on machines that have them): "pencil" folds a
// pressure-driven log-radius and a NEGATIVE-going scatter curve; "ink"'s opaque_multiply is a
// duplicate-x STEP (the knot union must not collapse it); "marker" drives the dab angle from
// the stroke direction, runs the airbrush, and its opaque x opaque_multiply product saturates
// the [0,1] clamp. Golden numbers are hand-derived from the producer's model (value = base +
// sum of per-input piecewise-linear curves; radius = clamp(exp(log_value))), never read back
// from the reader.
namespace {

using namespace mosaic::io::brush;
namespace cb = mosaic::core::brush;

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE_MESSAGE(f.good(), path);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    REQUIRE(f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))
                .good());
    return buf;
}

std::optional<BrushPreset> parse(std::string_view json, std::string* err = nullptr) {
    return readMyb(reinterpret_cast<const std::uint8_t*>(json.data()), json.size(), "syn", err);
}

bool hasDrop(const BrushPreset& p, std::string_view needle) {
    for (const std::string& d : p.provenance.droppedOptions)
        if (d.find(needle) != std::string::npos)
            return true;
    return false;
}

const cb::CurveOptionData& opt(const BrushPreset& p, std::string_view name) {
    for (const cb::CurveOptionData& o : p.options)
        if (o.name == name)
            return o;
    REQUIRE_MESSAGE(false, "no option " << std::string(name));
    return p.options.front();
}

// The lone sensor curve of a single-sensor option.
const cb::Sensor& soleSensor(const cb::CurveOptionData& o) {
    REQUIRE(o.sensors.sensors.size() == 1);
    return o.sensors.sensors.front();
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Rejections: total, capped, and precise about the version-2 text format.

TEST_CASE("myb: rejections name their reason") {
    std::string err;
    CHECK_FALSE(readMyb(nullptr, 0, "x", &err).has_value());
    CHECK(err == "empty file");

    const std::vector<std::uint8_t> huge(kMaxMybBytes + 1, ' ');
    CHECK_FALSE(readMyb(huge.data(), huge.size(), "x", &err).has_value());
    CHECK(err.find("size cap") != std::string::npos);

    CHECK_FALSE(parse("  \n# mypaint brush file\nversion 2\n", &err).has_value());
    CHECK(err.find("version 2") != std::string::npos);
    CHECK(err.find("version 3 JSON") != std::string::npos);

    CHECK_FALSE(parse("{not json", &err).has_value());
    CHECK(err == "not a JSON object");
    CHECK_FALSE(parse("[1,2]", &err).has_value());

    CHECK_FALSE(parse(R"({"settings":{}})", &err).has_value());
    CHECK(err.find("version") != std::string::npos);
    CHECK_FALSE(parse(R"({"version":2,"settings":{}})", &err).has_value());
    CHECK_FALSE(parse(R"({"version":4,"settings":{}})", &err).has_value());

    CHECK_FALSE(parse(R"({"version":3})", &err).has_value());
    CHECK(err == "no settings object");
    CHECK_FALSE(parse(R"({"version":3,"settings":[]})", &err).has_value());
}

// ------------------------------------------------------------------------------------------------
// "pencil": pressure-driven log radius, dynamic hardness (dropped, noted), negative scatter
// curve, no anti-aliasing. diameter = 2*exp(0.8 + max_knot(0.3)) = 2*exp(1.1) = 6.0083320.

TEST_CASE("myb: Pencil_2b golden") {
    const auto parsed = parse(R"({"version":3,"settings":{
        "radius_logarithmic":{"base_value":0.8,"inputs":{
            "pressure":[[0.0,-0.6],[0.7,0.2],[1.0,0.3]]}},
        "opaque":{"base_value":0.2,"inputs":{}},
        "opaque_multiply":{"base_value":0.0,"inputs":{"pressure":[[0.0,0.0],[1.0,1.0]]}},
        "hardness":{"base_value":0.3,"inputs":{"pressure":[[0.0,0.0],[1.0,0.2]]}},
        "offset_by_random":{"base_value":0.4,"inputs":{"pressure":[[0.0,0.0],[1.0,-0.2]]}},
        "anti_aliasing":{"base_value":0.0,"inputs":{}},
        "opaque_linearize":{"base_value":0.0,"inputs":{}},
        "dabs_per_actual_radius":{"base_value":4.0,"inputs":{}}}})");
    REQUIRE(parsed.has_value());
    const BrushPreset& p = *parsed;

    CHECK(p.provenance.sourceFormat == "myb");
    CHECK(p.provenance.sourcePaintop == "mypaint");
    CHECK(p.provenance.fidelity == PresetFidelity::Approximated);
    CHECK(p.paintMode == cb::PaintMode::Buildup);
    CHECK(p.accumulator == cb::StrokeAccumulator::Uniform);
    CHECK_FALSE(p.eraserMode);

    // The tip: Soft circle, static-folded diameter, knee at hardness 0.3, AA off.
    CHECK(p.tip.kind == TipXml::Kind::Auto);
    const cb::MaskGeneratorParams& g = p.tip.autoTip.generator;
    CHECK(g.shape == cb::MaskShape::Circle);
    CHECK(g.falloff == cb::MaskFalloff::Soft);
    CHECK(g.diameter == doctest::Approx(6.0083320).epsilon(1e-6));
    CHECK(g.ratio == doctest::Approx(1.0));
    CHECK_FALSE(g.antialiasEdges);
    CHECK(g.softnessCurve.eval(0.3) == doctest::Approx(0.3).epsilon(1e-6));
    CHECK(g.softnessCurve.eval(0.0) == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(g.softnessCurve.eval(1.0) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(hasDrop(p, "Hardness: dynamic"));

    // Spacing: dabs_per_actual_radius 4 -> 1/(2*4).
    CHECK(p.tip.spacing == doctest::Approx(0.125));
    CHECK_FALSE(p.tip.useAutoSpacing);

    // Size: one pressure sensor, exp-folded so the curve tops out at 1.
    const cb::CurveOptionData& size = opt(p, "Size");
    CHECK(size.checked);
    CHECK(size.useCurve);
    CHECK(size.strength == doctest::Approx(1.0));
    const cb::Sensor& s = soleSensor(size);
    CHECK(s.id == cb::SensorId::Pressure);
    CHECK(s.curve.eval(1.0) == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(s.curve.eval(0.0) == doctest::Approx(0.4065697).epsilon(1e-3)); // exp(-0.6 - 0.3)

    // Flow: opaque 0.2 (constant) x pressure ramp -> a 0.2-scaled ramp; Opacity stays 1.
    const cb::CurveOptionData& flow = opt(p, "Flow");
    CHECK(flow.useCurve);
    CHECK(flow.strength == doctest::Approx(1.0));
    CHECK(soleSensor(flow).curve.eval(0.0) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(soleSensor(flow).curve.eval(1.0) == doctest::Approx(0.2).epsilon(1e-4));
    CHECK(soleSensor(flow).curve.eval(0.5) == doctest::Approx(0.1).epsilon(1e-3));
    const cb::CurveOptionData& opacity = opt(p, "Opacity");
    CHECK_FALSE(opacity.useCurve);
    CHECK(opacity.strength == doctest::Approx(1.0));
    CHECK_FALSE(hasDrop(p, "Flow: zero"));
    CHECK_FALSE(hasDrop(p, "Flow: dab-overlap")); // opaque_linearize is 0 in this file

    // Scatter: base 0.4, pressure curve descends to -0.2 -> strength 0.4, curve 1 -> 0.5.
    const cb::CurveOptionData& scatter = opt(p, "Scatter");
    CHECK(scatter.checked);
    CHECK(scatter.strength == doctest::Approx(0.4));
    CHECK(scatter.useCurve);
    CHECK(soleSensor(scatter).curve.eval(0.0) == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(soleSensor(scatter).curve.eval(1.0) == doctest::Approx(0.5).epsilon(1e-6));
    CHECK(hasDrop(p, "Scatter: gaussian offset approximated"));

    // Smudge 0 and lock_alpha 0 are at the producer's defaults: no notes for them.
    CHECK_FALSE(hasDrop(p, "Smudge"));
    CHECK_FALSE(hasDrop(p, "Lock alpha"));
}

// ------------------------------------------------------------------------------------------------
// "ink": the duplicate-x pressure STEP at 0.02 must survive the product resampling, and the
// speed1 size input arrives as an Approximated Speed sensor.

TEST_CASE("myb: Ink_pen pressure step survives the flow composition") {
    const auto parsed = parse(R"({"version":3,"settings":{
        "radius_logarithmic":{"base_value":1.0,"inputs":{
            "pressure":[[0.0,0.0],[1.0,0.4]],
            "speed1":[[0.0,0.0],[1.0,-0.3]]}},
        "opaque":{"base_value":1.0,"inputs":{}},
        "opaque_multiply":{"base_value":0.0,"inputs":{
            "pressure":[[0.0,0.0],[0.02,0.0],[0.02,1.0],[1.0,1.0]]}},
        "hardness":{"base_value":0.85,"inputs":{}},
        "opaque_linearize":{"base_value":0.9,"inputs":{}},
        "dabs_per_actual_radius":{"base_value":2.5,"inputs":{}}}})");
    REQUIRE(parsed.has_value());
    const BrushPreset& p = *parsed;

    // diameter = 2*exp(1.0 + 0.4 + 0) -- speed1's knots are all <= 0 so its max folds nothing.
    CHECK(p.tip.autoTip.generator.diameter == doctest::Approx(8.1103999).epsilon(1e-6));
    CHECK(p.tip.spacing == doctest::Approx(0.2).epsilon(1e-6));

    const cb::CurveOptionData& flow = opt(p, "Flow");
    CHECK(flow.useCurve);
    const cb::Curve& c = soleSensor(flow).curve;
    // Below the gate: nothing. Above it: full ink. A collapsed step would ramp for half the
    // pressure range instead.
    CHECK(c.eval(0.010) < 0.01);
    CHECK(c.eval(0.019) < 0.01);
    CHECK(c.eval(0.021) > 0.99);
    CHECK(c.eval(0.5) > 0.99);
    CHECK(c.eval(1.0) == doctest::Approx(1.0).epsilon(1e-6));

    // Size: pressure (exact) + speed1 (approximated, x/4).
    const cb::CurveOptionData& size = opt(p, "Size");
    REQUIRE(size.sensors.sensors.size() == 2);
    const cb::Sensor& pressure = size.sensors.sensors[0];
    const cb::Sensor& speed = size.sensors.sensors[1];
    CHECK(pressure.id == cb::SensorId::Pressure);
    CHECK(speed.id == cb::SensorId::Speed);
    CHECK(pressure.curve.eval(1.0) == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(speed.curve.eval(0.25) == doctest::Approx(0.7408182).epsilon(1e-3)); // exp(-0.3)
    CHECK(p.provenance.fidelity == PresetFidelity::Approximated);

    CHECK(hasDrop(p, "Flow: dab-overlap linearization"));
    CHECK_FALSE(hasDrop(p, "Hardness")); // static hardness is exact
}

// ------------------------------------------------------------------------------------------------
// "marker": direction-driven rotation, airbrush, ratio 8, and a flow product that saturates the
// clamp at full pressure.

TEST_CASE("myb: Marker_Medium rotation and airbrush golden") {
    const auto parsed = parse(R"({"version":3,"settings":{
        "radius_logarithmic":{"base_value":2.0,"inputs":{}},
        "opaque":{"base_value":1.5,"inputs":{"pressure":[[0.0,0.5],[1.0,0.5]]}},
        "opaque_multiply":{"base_value":0.0,"inputs":{
            "pressure":[[0.0,0.0],[0.1,0.3],[1.0,0.8]]}},
        "elliptical_dab_ratio":{"base_value":8.0,"inputs":{"speed1":[[0.0,-4.0],[1.0,0.0]]}},
        "elliptical_dab_angle":{"base_value":90.0,"inputs":{
            "direction":[[0.0,0.0],[180.0,180.0]]}},
        "dabs_per_actual_radius":{"base_value":0.0,"inputs":{}},
        "dabs_per_basic_radius":{"base_value":2.5,"inputs":{}},
        "dabs_per_second":{"base_value":15.0,"inputs":{}},
        "opaque_linearize":{"base_value":0.0,"inputs":{}},
        "smudge":{"base_value":0.5,"inputs":{}}}})");
    REQUIRE(parsed.has_value());
    const BrushPreset& p = *parsed;

    CHECK(p.tip.autoTip.generator.diameter == doctest::Approx(14.7781122).epsilon(1e-6));
    CHECK(p.tip.autoTip.generator.ratio == doctest::Approx(0.125));
    CHECK(p.tip.spacing == doctest::Approx(0.2).epsilon(1e-6)); // 1/(2*2.5), basic radius only
    CHECK(hasDrop(p, "Ratio: dynamic aspect"));
    CHECK_FALSE(hasDrop(p, "Spacing: base-radius")); // no size dynamics -> the sum is exact

    // Airbrush: dabs_per_second 15.
    CHECK(p.airbrush.enabled);
    CHECK(p.airbrush.rate == doctest::Approx(15.0));
    CHECK(hasDrop(p, "Airbrush"));

    // Rotation: DrawingAngle sensor; the direction input folds mod 180, so the curve repeats on
    // both half-turns: 90deg base + direction. At direction 0 the tip sits at 90deg (0.25 turn);
    // at 180 it reaches 270 (0.75), and the second half-turn repeats it.
    const cb::CurveOptionData& rot = opt(p, "Rotation");
    CHECK(rot.checked);
    const cb::Sensor& dir = soleSensor(rot);
    CHECK(dir.id == cb::SensorId::DrawingAngle);
    CHECK(dir.curve.eval(0.0) == doctest::Approx(0.25).epsilon(1e-3));
    CHECK(dir.curve.eval(0.499) == doctest::Approx(0.75).epsilon(1e-2));
    CHECK(dir.curve.eval(1.0) == doctest::Approx(0.75).epsilon(1e-3));
    CHECK(hasDrop(p, "Rotation: follows stroke direction"));

    // Flow: (1.5 + 0.5) x mul(p), clamped -- 2.0 * 0.8 saturates at full pressure.
    const cb::Curve& flow = soleSensor(opt(p, "Flow")).curve;
    CHECK(flow.eval(0.0) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(flow.eval(0.1) == doctest::Approx(0.6).epsilon(1e-3)); // 2.0 * 0.3
    CHECK(flow.eval(1.0) == doctest::Approx(1.0).epsilon(1e-6));

    CHECK(hasDrop(p, "Smudge"));
}

// ------------------------------------------------------------------------------------------------
// The opaque_multiply defaults ladder: absent -> libmypaint's surviving pressure ramp; an
// explicitly cleared pressure key -> the constant; base 0 cleared -> loud "paints nothing".

TEST_CASE("myb: opaque_multiply absent imports as the default pressure ramp") {
    const auto p = parse(R"({"version":3,"settings":{}})");
    REQUIRE(p.has_value());
    // Flow = opaque default 1.0 x ramp(p) = p.
    const cb::CurveOptionData& flow = opt(*p, "Flow");
    CHECK(flow.useCurve);
    CHECK(soleSensor(flow).curve.eval(0.0) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(soleSensor(flow).curve.eval(0.5) == doctest::Approx(0.5).epsilon(1e-6));
    CHECK(soleSensor(flow).curve.eval(1.0) == doctest::Approx(1.0).epsilon(1e-6));
    CHECK_FALSE(hasDrop(*p, "Flow: zero"));
    // And the rest of the defaults: 2*exp(2.0) diameter, 0.25 spacing from D=2, hardness 0.8.
    CHECK(p->tip.autoTip.generator.diameter == doctest::Approx(14.7781122).epsilon(1e-6));
    CHECK(p->tip.spacing == doctest::Approx(0.25));
    CHECK(p->tip.autoTip.generator.softnessCurve.eval(0.8) == doctest::Approx(0.8).epsilon(1e-6));
}

TEST_CASE("myb: opaque_multiply with a cleared pressure key stays constant") {
    // An empty pressure array is libmypaint's explicit clear (set_mapping_n(.., 0)): the default
    // ramp must NOT be injected over it.
    const auto half = parse(
        R"({"version":3,"settings":{"opaque_multiply":{"base_value":0.5,"inputs":{"pressure":[]}}}})");
    REQUIRE(half.has_value());
    CHECK_FALSE(opt(*half, "Flow").useCurve);
    CHECK(opt(*half, "Flow").strength == doctest::Approx(0.5));

    const auto zero = parse(
        R"({"version":3,"settings":{"opaque_multiply":{"base_value":0.0,"inputs":{"pressure":[]}}}})");
    REQUIRE(zero.has_value());
    CHECK_FALSE(opt(*zero, "Flow").useCurve);
    CHECK(opt(*zero, "Flow").strength == doctest::Approx(0.0));
    CHECK(hasDrop(*zero, "Flow: zero"));
}

// ------------------------------------------------------------------------------------------------
// Sensor remaps that no shipped file exercises: tilts, fuzzy, stroke, and the unknown-input note.

TEST_CASE("myb: tilt and foreign size inputs") {
    const auto p = parse(R"({"version":3,"settings":{"radius_logarithmic":{
        "base_value":1.0,"inputs":{
            "tilt_declination":[[0.0,0.0],[90.0,1.0]],
            "tilt_ascension":[[-180.0,0.0],[180.0,0.5]],
            "random":[[0.0,0.0],[1.0,0.25]],
            "viewzoom":[[0.0,0.0],[1.0,1.0]]}}}})");
    REQUIRE(p.has_value());
    const cb::CurveOptionData& size = opt(*p, "Size");
    REQUIRE(size.sensors.sensors.size() == 3); // viewzoom has no home -> dropped, noted
    CHECK(hasDrop(*p, "Size: input 'viewzoom'"));

    // Input order inside a setting is a serialization detail (the model SUMS them); look up by id.
    const auto byId = [&](cb::SensorId id) -> const cb::Sensor& {
        for (const cb::Sensor& s : size.sensors.sensors)
            if (s.id == id)
                return s;
        REQUIRE(false);
        return size.sensors.sensors.front();
    };
    const cb::Sensor& decl = byId(cb::SensorId::Declination);
    CHECK(decl.curve.eval(1.0) == doctest::Approx(1.0).epsilon(1e-4)); // 90deg -> x=1, exp(1-1)
    CHECK(decl.curve.eval(0.0) == doctest::Approx(std::exp(-1.0)).epsilon(1e-3));

    const cb::Sensor& asc = byId(cb::SensorId::Ascension);
    // -180deg maps to x=0, +180deg to x=1; the 0.5 max folds out.
    CHECK(asc.curve.eval(1.0) == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(asc.curve.eval(0.0) == doctest::Approx(std::exp(-0.5)).epsilon(1e-3));

    byId(cb::SensorId::Fuzzy); // random -> Fuzzy is present
    // diameter folds every input's max: 2*exp(1 + 1 + 0.5 + 0.25) [viewzoom dropped].
    CHECK(p->tip.autoTip.generator.diameter
          == doctest::Approx(2.0 * std::exp(2.75)).epsilon(1e-6));
}

TEST_CASE("myb: rotation wrap through the turn boundary") {
    // Base angle 350: direction 0 -> 350deg (0.972 turn), direction 90 -> 440 = 80deg (0.222).
    // Without the hugging pair the interpolation would sweep backwards through every angle.
    const auto p = parse(R"({"version":3,"settings":{
        "elliptical_dab_ratio":{"base_value":5.0,"inputs":{}},
        "elliptical_dab_angle":{"base_value":350.0,"inputs":{
            "direction":[[0.0,0.0],[90.0,90.0]]}}}})");
    REQUIRE(p.has_value());
    const cb::Curve& c = soleSensor(opt(*p, "Rotation")).curve;
    CHECK(c.eval(0.0) == doctest::Approx(350.0 / 360.0).epsilon(1e-3));
    CHECK(c.eval(0.25) == doctest::Approx(80.0 / 360.0).epsilon(1e-2));
    // The wrap midpoint (x = 0.125) carries the near-vertical pair.
    CHECK(c.eval(0.124) > 0.9);
    CHECK(c.eval(0.127) < 0.1);
}

TEST_CASE("myb: static angle tilts the tip only when the dab is not round") {
    const auto flat = parse(R"({"version":3,"settings":{
        "elliptical_dab_ratio":{"base_value":4.0,"inputs":{}},
        "elliptical_dab_angle":{"base_value":30.0,"inputs":{}}}})");
    REQUIRE(flat.has_value());
    CHECK(flat->tip.autoTip.generator.ratio == doctest::Approx(0.25));
    CHECK(flat->tip.angle == doctest::Approx(30.0 * 3.14159265358979 / 180.0).epsilon(1e-6));

    // On a round dab the same angle is invisible upstream too: no tilt, no note.
    const auto round = parse(R"({"version":3,"settings":{
        "elliptical_dab_angle":{"base_value":30.0,"inputs":{
            "direction":[[0.0,0.0],[180.0,180.0]]}}}})");
    REQUIRE(round.has_value());
    CHECK(round->tip.angle == doctest::Approx(0.0));
    CHECK_FALSE(opt(*round, "Rotation").checked);
    CHECK_FALSE(hasDrop(*round, "Rotation"));
}

// ------------------------------------------------------------------------------------------------
// Honesty: everything the engine has no home for is a note, never silence.

TEST_CASE("myb: active foreign features are noted, defaults stay silent") {
    const auto p = parse(R"({"version":3,"settings":{
        "eraser":{"base_value":0.7,"inputs":{"pressure":[[0.0,0.0],[1.0,1.0]]}},
        "anti_aliasing":{"base_value":1.0,"inputs":{"pressure":[[0.0,0.0],[1.0,1.0]]}},
        "smudge":{"base_value":0.4,"inputs":{}},
        "lock_alpha":{"base_value":1.0,"inputs":{}},
        "change_color_h":{"base_value":0.2,"inputs":{}},
        "change_color_v":{"base_value":0.2,"inputs":{}},
        "stroke_holdtime":{"base_value":9.0,"inputs":{"pressure":[[0.0,0.0],[1.0,1.0]]}}}})");
    REQUIRE(p.has_value());
    CHECK(p->eraserMode); // 0.7 >= 0.5
    CHECK(hasDrop(*p, "Eraser: partial strength"));
    CHECK(hasDrop(*p, "Eraser: dynamic"));
    CHECK(hasDrop(*p, "Anti-aliasing: dynamic"));
    CHECK(hasDrop(*p, "Smudge"));
    CHECK(hasDrop(*p, "Lock alpha"));
    CHECK(hasDrop(*p, "Color dynamics"));
    // One note for the whole change_color_* family, not one per member.
    int colorNotes = 0;
    for (const std::string& d : p->provenance.droppedOptions)
        colorNotes += d == "Color dynamics" ? 1 : 0;
    CHECK(colorNotes == 1);
    // stroke_holdtime is not a consumed setting; its dynamic mapping is behaviour we dropped.
    CHECK(hasDrop(*p, "Dynamic setting 'stroke_holdtime'"));

    // opaque_linearize DEFAULTS to 0.9: even an all-defaults file carries the linearization
    // note, because the producer's default behaviour includes a correction we don't have.
    const auto defaults = parse(R"({"version":3,"settings":{}})");
    REQUIRE(defaults.has_value());
    CHECK(defaults->provenance.droppedOptions
          == std::vector<std::string>{"Flow: dab-overlap linearization"});

    // With it zeroed and everything else at (or explicitly on) the defaults: fully silent.
    const auto quiet = parse(R"({"version":3,"settings":{
        "opaque_linearize":{"base_value":0.0,"inputs":{}},
        "smudge":{"base_value":0.0,"inputs":{}},
        "slow_tracking":{"base_value":2.0,"inputs":{}}}})");
    REQUIRE(quiet.has_value());
    CHECK(quiet->provenance.droppedOptions.empty());
    CHECK(quiet->provenance.fidelity == PresetFidelity::Exact);
}

TEST_CASE("myb: producer clamps hold at the extremes") {
    // radius in [0.2, 1000] px -- the producer's own dab-time clamp, folded statically.
    const auto big = parse(R"({"version":3,"settings":{
        "radius_logarithmic":{"base_value":50.0,"inputs":{}}}})");
    REQUIRE(big.has_value());
    CHECK(big->tip.autoTip.generator.diameter == doctest::Approx(2000.0));
    const auto tiny = parse(R"({"version":3,"settings":{
        "radius_logarithmic":{"base_value":-50.0,"inputs":{}}}})");
    REQUIRE(tiny.has_value());
    CHECK(tiny->tip.autoTip.generator.diameter == doctest::Approx(0.4));

    // hardness 0 draws nothing upstream: floored to a usable knee, loudly.
    const auto soft = parse(R"({"version":3,"settings":{
        "hardness":{"base_value":0.0,"inputs":{}}}})");
    REQUIRE(soft.has_value());
    CHECK(soft->tip.autoTip.generator.softnessCurve.eval(0.02)
          == doctest::Approx(0.02).epsilon(1e-6));
    CHECK(hasDrop(*soft, "Hardness: zero"));
    // And 1.0 is pinned to a knee strictly inside the profile.
    const auto hard = parse(R"({"version":3,"settings":{
        "hardness":{"base_value":1.0,"inputs":{}}}})");
    REQUIRE(hard.has_value());
    CHECK(hard->tip.autoTip.generator.softnessCurve.eval(0.98)
          == doctest::Approx(0.98).epsilon(1e-6));
}

TEST_CASE("myb: caps bound hostile input") {
    // 40 inputs on one setting (cap 32), 80 points on one input (cap 64): parse, don't die.
    std::string json = R"({"version":3,"settings":{"radius_logarithmic":{"base_value":1.0,"inputs":{)";
    for (int i = 0; i < 40; ++i)
        json += (i ? "," : "") + std::string("\"in") + std::to_string(i)
              + "\":[[0.0,0.0],[1.0,1.0]]";
    json += "}},\"opaque\":{\"base_value\":1.0,\"inputs\":{\"pressure\":[";
    for (int i = 0; i < 80; ++i)
        json += (i ? "," : "") + std::string("[") + std::to_string(i / 80.0) + ",0.5]";
    json += "]}}}}";
    const auto p = parse(json);
    REQUIRE(p.has_value());
    CHECK(p->provenance.fidelity == PresetFidelity::Approximated);
}

// ------------------------------------------------------------------------------------------------
// The system corpus, when this machine has it: every shipped Krita MyPaint preset loads and
// comes out VISIBLE (the ramp rule's payoff), in buildup, with sane geometry.

TEST_CASE("myb: system corpus replay") {
    const std::filesystem::path dir = "/usr/share/krita/paintoppresets";
    if (!std::filesystem::exists(dir))
        return; // machine without the corpus: covered by the three in-tree fixtures
    int seen = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".myb")
            continue;
        ++seen;
        const auto buf = readFile(entry.path().string());
        std::string err;
        const auto p = readMyb(buf.data(), buf.size(), entry.path().stem().string(), &err);
        REQUIRE_MESSAGE(p.has_value(), entry.path().string() << ": " << err);
        CHECK(p->paintMode == cb::PaintMode::Buildup);
        CHECK(p->accumulator == cb::StrokeAccumulator::Uniform);
        CHECK(p->tip.kind == TipXml::Kind::Auto);
        CHECK(p->tip.spacing >= 0.01);
        CHECK(p->tip.spacing <= 10.0);
        CHECK(p->tip.autoTip.generator.diameter >= 0.4);
        CHECK(p->tip.autoTip.generator.diameter <= 2000.0);
        // Visible: the flow channel must reach past zero somewhere.
        const cb::CurveOptionData& flow = opt(*p, "Flow");
        const double top = flow.useCurve ? soleSensor(flow).curve.eval(1.0) : flow.strength;
        CHECK_MESSAGE(top > 0.05, entry.path().string());
    }
    CHECK(seen >= 7);
}
