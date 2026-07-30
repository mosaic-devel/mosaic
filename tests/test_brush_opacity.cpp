#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <memory>
#include <vector>

// The per-dab opacity CEILING (docs/brushes.md §6.2, S19 Arc D): a live-sensor Opacity option rides
// every dab as the ceiling the Wash accumulation strives toward, through washAlphaDarkenAlpha --
// transcribed from the reference's indirect-painting composite in its default parameterization --
// while the option's static STRENGTH stays the whole stroke's cap (BrushParams::opacity), exactly
// where it has been since S14. The two must never fold together: that squares the opacity.
//
// The formula pins are hand-derived with dyadic quantities. The engine pins drive REAL strokes and
// choose their spacing so the dab count and each dab's pressure are exact -- a spacing of
// 0.6 x diameter on a 12 px span is precisely two dabs, one per endpoint.
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::core::brush::blendAverageOpacity;
using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushOptions;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::CurveOption;
using mosaic::core::brush::CurveOptionData;
using mosaic::core::brush::PaintMode;
using mosaic::core::brush::Sensor;
using mosaic::core::brush::SensorId;
using mosaic::core::brush::StrokeInput;
using mosaic::core::brush::washAlphaDarkenAlpha;

Color8 pixel(const Image& img, int x, int y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

// The always-on Opacity option with a live identity-curve pressure sensor: per dab, the ceiling IS
// the pressure. `strength` is carried but must never reach the per-dab value.
[[nodiscard]] std::shared_ptr<BrushOptions> pressureOpacity(double strength) {
    CurveOptionData d;
    d.name = "Opacity";
    d.checkable = false;
    d.strength = strength;
    d.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
    auto o = std::make_shared<BrushOptions>();
    o->opacity.emplace(std::move(d));
    return o;
}

[[nodiscard]] StrokeInput at(double x, double y, double pressure) {
    StrokeInput in;
    in.pos = {x, y};
    in.pressure = pressure;
    return in;
}

Image paint(const BrushParams& p, const std::vector<StrokeInput>& path) {
    Image img(96, 96); // transparent: what the stroke lays is exactly what it deposited
    BrushEngine eng;
    eng.begin(96, 96, img, p, BrushDynamics{}, path.front());
    for (std::size_t i = 1; i < path.size(); ++i)
        eng.extendTo(path[i]);
    eng.flush();
    eng.composite();
    eng.end();
    return img;
}

[[nodiscard]] BrushParams hardBrush() {
    BrushParams p;
    p.diameter = 20.0;
    p.hardness = 1.0; // coverage exactly 1.0 well inside the core
    p.flow = 1.0;
    p.opacity = 1.0;
    p.spacing = 0.1;
    p.color = Color8{0, 0, 0, 255};
    return p;
}

} // namespace

// ---- The two pure functions -------------------------------------------------------------------

TEST_CASE("blendAverageOpacity rises instantly and decays at 0.1 per dab") {
    // The one-sided EMA: a louder dab IS the new average; a quieter one pulls it down by a tenth.
    CHECK(blendAverageOpacity(0.8, 0.2) == 0.8);   // rise: instant
    CHECK(blendAverageOpacity(0.8, 0.0) == 0.8);   // the first dab: its own opacity
    CHECK(blendAverageOpacity(0.4, 0.8) == doctest::Approx(0.1 * 0.4 + 0.9 * 0.8)); // decay
    CHECK(blendAverageOpacity(0.5, 0.5) == doctest::Approx(0.5)); // equal: unmoved
}

TEST_CASE("washAlphaDarkenAlpha: the plain branch pins, all dyadic") {
    // avg <= opacity: strive toward THIS dab's ceiling, as hard as the mask covers -- never past
    // it, never down.
    CHECK(washAlphaDarkenAlpha(0.0, 1.0, 1.0, 0.5, 0.5) == 0.5);   // first touch lays the ceiling
    CHECK(washAlphaDarkenAlpha(0.0, 0.5, 1.0, 0.5, 0.5) == 0.25);  // half mask: half way there
    CHECK(washAlphaDarkenAlpha(0.0, 1.0, 0.5, 0.5, 0.5) == 0.25);  // half flow: half the step
    CHECK(washAlphaDarkenAlpha(0.25, 1.0, 1.0, 0.5, 0.5) == 0.5);  // full mask+flow: at the ceiling
    // ⚠ NEVER DOWN: a dab whose ceiling sits below the paint leaves the paint standing.
    CHECK(washAlphaDarkenAlpha(0.75, 1.0, 1.0, 0.5, 0.5) == 0.75);
}

TEST_CASE("washAlphaDarkenAlpha: the ceiling is a CEILING, not a flow") {
    // Overlapping dabs at flow 0.5 build toward opacity 0.5 and stop there -- the distinction six
    // sessions of docs kept alive: flow builds WITHIN a stroke, opacity CAPS it.
    double a = 0.0;
    for (int i = 0; i < 64; ++i)
        a = washAlphaDarkenAlpha(a, 1.0, 0.5, 0.5, 0.5);
    CHECK(a <= 0.5);
    CHECK(a > 0.499);
}

TEST_CASE("washAlphaDarkenAlpha: the average branch strives toward what the stroke earned") {
    // avg 0.76 > opacity 0.4 (a stroke that pressed 0.8 and eased to 0.4). Hand-derived:
    //   fresh pixel (dst 0):      lerp(src 0.4, avg 0.76, 0/0.76)      = 0.4
    //   half-way pixel (dst .38): lerp(0.4, 0.76, 0.38/0.76 = 0.5)     = 0.58  -- ABOVE the dab's
    //                             own 0.4 ceiling: consecutive quiet dabs aim at the average.
    //   pixel past the avg (0.8): stays -- never down.
    CHECK(washAlphaDarkenAlpha(0.0, 1.0, 1.0, 0.4, 0.76) == doctest::Approx(0.4));
    CHECK(washAlphaDarkenAlpha(0.38, 1.0, 1.0, 0.4, 0.76) == doctest::Approx(0.58));
    CHECK(washAlphaDarkenAlpha(0.8, 1.0, 1.0, 0.4, 0.76) == 0.8);
}

TEST_CASE("washAlphaDarkenAlpha at opacity 1 is the static wash step, in real arithmetic") {
    // dst + flow*cov*(1 - dst), to within an ulp of grouping -- the reason the static path stays a
    // SEPARATE branch is bit-exactness, not behaviour.
    for (const double dst : {0.0, 0.25, 0.6}) {
        for (const double cov : {0.3, 1.0}) {
            for (const double flow : {0.45, 1.0}) {
                CHECK(washAlphaDarkenAlpha(dst, cov, flow, 1.0, 1.0) ==
                      doctest::Approx(dst + flow * cov * (1.0 - dst)).epsilon(1e-12));
            }
        }
    }
}

// ---- The engine wiring ------------------------------------------------------------------------

TEST_CASE("a per-dab ceiling: pressure caps the dab, the strength caps the stroke -- not twice") {
    // One press at pressure 0.5 under a live pressure->opacity option authored at strength 0.8.
    // The per-dab ceiling is the SENSOR value alone (0.5); the strength is the stroke cap
    // (BrushParams::opacity = 0.8, as presetBrushParams seeds it). Core pixel:
    //   coverage = lerp(0, 0.5, 1) = 0.5;  alpha = 0.5 * 0.8 * 255 -> 102.
    // The mutants this number kills, each landing somewhere else:
    //   * the dynamic path not taken at all      -> 1.0 * 0.8 * 255 = 204
    //   * the strength folded into the dab too   -> 0.5*0.8 * 0.8 * 255 = 82
    //   * the cap dropped in favour of the dab   -> 0.5 * 255 = 128
    BrushParams p = hardBrush();
    p.opacity = 0.8;
    p.options = pressureOpacity(0.8);

    const Image img = paint(p, {at(48, 48, 0.5)});
    CHECK(pixel(img, 48, 48).a == 102);
    CHECK(pixel(img, 48, 48).r == 0); // and it is the stroke's colour under that alpha
}

TEST_CASE("a fading stroke never carves the paint it already laid") {
    // Press at full pressure, ease off to 0.1 across one straight stroke. The dabs laid at low
    // pressure OVERLAP the full-pressure core -- and must leave it standing. ⚠ On a FADING stroke
    // it is the AVERAGE branch that holds the line (avg decays slowly, so avg > o at every quiet
    // dab, and its own `avg > dst` guard is what refuses the step down) -- the plain branch's
    // guard never even runs here. The return-stroke case below is the one that exercises THAT.
    BrushParams p = hardBrush();
    p.options = pressureOpacity(1.0);

    const Image img = paint(p, {at(30, 48, 1.0), at(66, 48, 0.1)});
    CHECK(pixel(img, 30, 48).a == 255); // the press core: laid at ceiling 1, still there
}

TEST_CASE("a stroke returning over its own loud start at medium pressure leaves it standing") {
    // Loud press (dst -> 1 at the start), a long quiet leg away (the running average DECAYS well
    // below 0.5 over ~18 dabs), then a MEDIUM return over the start. At the returning dab the
    // average has decayed below the dab's own 0.5, so the PLAIN branch runs -- and its
    // `opacity > dst` guard is the only thing between the 0.5-ceiling dab and the 1.0 paint under
    // it. Drop that guard and the return leg drags the start core toward 0.5: this is the case the
    // fading stroke above cannot see, because a fade keeps the average ABOVE the dab and runs the
    // other branch.
    BrushParams p = hardBrush();
    p.options = pressureOpacity(1.0);

    const Image img =
        paint(p, {at(30, 48, 1.0), at(66, 48, 0.1), at(66, 60, 0.1), at(30, 60, 0.3),
                  at(30, 48, 0.5)});
    CHECK(pixel(img, 30, 48).a == 255); // laid at ceiling 1; the 0.5 return must not touch it
}

TEST_CASE("the running average is wired per dab: a quieter dab still aims above its own ceiling") {
    // Exactly two dabs -- spacing 0.6 x 20 px = 12 px, the span is 12 px -- at pressures 0.8 then
    // 0.4, hardness 0.5 so the midpoint pixel sits in BOTH dabs' soft shoulders at fractional
    // coverage m. After dab 1 it holds A = 0.8m; dab 2 arrives with avg = 0.1*0.4 + 0.9*0.8 = 0.76
    // > its own 0.4, so the pixel must rise TOWARD THE AVERAGE: lerp(0.4m, 0.76, A/0.76), which
    // sits well above the 0.4 its own ceiling could ever give it. Drop the average from the wiring
    // (avg = 0, or read once per stroke) and the pixel pins at ~0.4m..0.4.
    BrushParams p = hardBrush();
    p.hardness = 0.5;
    p.spacing = 0.6;
    p.options = pressureOpacity(1.0);

    // The probe sits 7 px from dab 1 (mid shoulder) and 5 px from dab 2 (near core). BOTH premises
    // are measured, not assumed -- a geometry retune must fail HERE, loudly, not let the check
    // below pass by discriminating nothing (the discrimination needs A1 in the mid-band and m2
    // high; outside those bands the two branches' outputs cross):
    //   dab 1 alone leaves the probe at A1 = 0.8*m1 in (0.25, 0.45);
    //   dab 2 alone (its own avg = its own 0.4) leaves it at 0.4*m2 with m2 > 0.8.
    const Image one = paint(p, {at(40, 48, 0.8)});
    const double a1 = pixel(one, 47, 48).a / 255.0;
    REQUIRE(a1 > 0.25);
    REQUIRE(a1 < 0.45);
    const Image two = paint(p, {at(52, 48, 0.4)});
    const double m2 = (pixel(two, 47, 48).a / 255.0) / 0.4;
    REQUIRE(m2 > 0.8);

    const Image img = paint(p, {at(40, 48, 0.8), at(52, 48, 0.4)});
    const double a2 = pixel(img, 47, 48).a / 255.0;
    // With the average wired: lerp(0.4*m2, 0.76, A1/0.76) > 0.46 over the whole premise band.
    // Without it (avg dropped, or read once per stroke), dab 2 can only aim at its own 0.4:
    // lerp(A1, 0.4, m2) <= 0.4. The gap is the mechanism.
    CHECK(a2 > 0.45);
}

TEST_CASE("BUILDUP takes the per-dab opacity as a per-dab SHARE, not as a ceiling") {
    // ⚠⚠ THIS CASE USED TO PIN THE UNTRANSCRIBED HALF AS THE CONTRACT ("Buildup keeps the static
    // ceiling: the dynamic option changes NOTHING there"), and six shipped presets were badged for
    // it. Both halves of the option are transcribed now (§6.6i): WASH is the reference's INDIRECT
    // painting, where the per-dab value is the ceiling the accumulation strives toward; BUILDUP is
    // its DIRECT painting, where the same value rides each dab's own composite instead. Here that
    // is the per-dab share of the accumulation -- `a * opacity * cap` in place of `a * cap`.
    //
    // ⚠ The IMAGE alone cannot witness this: Buildup composites out of its own capped accumulation,
    // so the COVERAGE buffer -- the Inpaint brush's hole mask -- must stay the plain geometric
    // accumulation in EITHER mode, and only the accumulation may move. Both are checked, and they
    // are checked in opposite directions.
    BrushParams with = hardBrush();
    with.paintMode = PaintMode::Buildup;
    with.opacity = 0.7;
    with.flow = 0.6;
    with.options = pressureOpacity(0.7);

    BrushParams without = with;
    without.options = nullptr;

    // A stroke that PRESSES LIGHTLY the whole way: every dab's opacity is 0.3, so every dab lays
    // less than the static ceiling would have and the mark must come out LIGHTER. (A stroke that
    // ramped from 0.9 to 0.3 would cross 0.7 somewhere and could pass while the sign was wrong.)
    const std::vector<StrokeInput> path{at(30, 48, 0.3), at(66, 48, 0.3)};
    const auto lay = [&path](const BrushParams& params) {
        Image img(96, 96);
        BrushEngine eng;
        eng.begin(96, 96, img, params, BrushDynamics{}, path.front());
        for (std::size_t i = 1; i < path.size(); ++i)
            eng.extendTo(path[i]);
        eng.flush();
        eng.composite();
        std::vector<float> cov = eng.coverage();
        eng.end();
        return std::make_pair(std::move(img), std::move(cov));
    };
    const auto [imgA, covA] = lay(with);
    const auto [imgB, covB] = lay(without);

    // The mark MOVED, and it moved the only way a per-dab opacity of 0.3 under a 0.7 ceiling can.
    CHECK_FALSE(imgA.rgba == imgB.rgba);
    CHECK(pixel(imgA, 48, 48).a < pixel(imgB, 48, 48).a);
    CHECK(pixel(imgA, 48, 48).a > 0); // ... and it still painted: this is a scale, not a gate

    // ⚠ AND THE COVERAGE DID NOT MOVE. It is the geometry of the stroke and nothing else; an
    // implementation that scaled it by the opacity would corrupt the Inpaint mask while producing
    // exactly the image above.
    REQUIRE(!covA.empty());
    CHECK(covA == covB);
}

TEST_CASE("BUILDUP: a dab at full opacity is BYTE-IDENTICAL to the static accumulation") {
    // The identity that keeps every Buildup golden where it is: at a per-dab value of exactly 1 the
    // new share is `1.0 * m_cap`, which IS `m_cap` in IEEE doubles -- so the accumulation's
    // expression is unchanged to the bit, not merely close. A stroke pressed at full pressure the
    // whole way through an identity curve is that case, dab for dab.
    BrushParams with = hardBrush();
    with.paintMode = PaintMode::Buildup;
    with.opacity = 0.7;
    with.flow = 0.6;
    with.options = pressureOpacity(0.7);

    BrushParams without = with;
    without.options = nullptr;

    const std::vector<StrokeInput> path{at(30, 48, 1.0), at(66, 48, 1.0)};
    CHECK(paint(with, path).rgba == paint(without, path).rgba);
}

TEST_CASE("BUILDUP: a STATIC Opacity option leaves the stroke byte-identical") {
    // `optionIsDynamic` is the shared gate on BOTH halves of the option (the importer's honesty
    // contract reads the same predicate), so `{X}UseCurve` off collapses to the constant strength
    // and the accumulation must take the untouched static path -- the same rule the Wash side has
    // carried since the ceiling landed, now asserted on the Buildup side too.
    BrushParams with = hardBrush();
    with.paintMode = PaintMode::Buildup;
    with.opacity = 0.7;
    with.flow = 0.6;
    CurveOptionData d;
    d.name = "Opacity";
    d.checkable = false;
    d.strength = 0.7;
    d.useCurve = false;
    d.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
    auto o = std::make_shared<BrushOptions>();
    o->opacity.emplace(std::move(d));
    with.options = std::move(o);

    BrushParams without = with;
    without.options = nullptr;

    const std::vector<StrokeInput> path{at(30, 48, 0.3), at(66, 48, 0.3)};
    CHECK(paint(with, path).rgba == paint(without, path).rgba);
}

TEST_CASE("BUILDUP: the per-dab share is the SENSOR times the cap, never the strength twice") {
    // ⚠ THE SQUARING TRAP, from the Buildup side. The reference reads its opacity option WITH the
    // strength under direct painting -- and in Mosaic the strength IS the stroke's cap (it is also
    // where the context bar's Opacity lands), so the per-dab value must be the sensors ALONE and the
    // cap supplies the rest. Folding the option's own `strength` in as well would square it.
    //
    // Hand-derived, on ONE dab: cap 0.5, flow 1, coverage 1 in the core, pressure 0.5 through an
    // identity curve -> share = 0.5 * 0.5 = 0.25, so alpha = 64 (round(255 * 0.25) = 63.75 -> 64).
    // A squared strength would give 0.5 * 0.5 * 0.5 = 0.125 and alpha 32.
    BrushParams p = hardBrush();
    p.paintMode = PaintMode::Buildup;
    p.opacity = 0.5;
    p.flow = 1.0;
    p.options = pressureOpacity(0.5);

    Image img(96, 96);
    BrushEngine eng;
    eng.begin(96, 96, img, p, BrushDynamics{}, at(48, 48, 0.5));
    eng.end(); // the press dab is the whole stroke
    eng.composite();
    // The dab's CORE, read as the peak rather than as one named pixel: the core's coverage is
    // exactly 1 well inside a hardness-1 nib, and which pixel that is depends on a centring
    // convention this case has no business pinning.
    int peak = 0;
    for (std::size_t i = 3; i < img.rgba.size(); i += 4)
        peak = std::max(peak, static_cast<int>(img.rgba[i]));
    CHECK(peak == 64);
}

TEST_CASE("a STATIC Opacity option (useCurve off) leaves the stroke byte-identical") {
    // `{X}UseCurve` false collapses the option to its constant strength (§3.2) -- which is already
    // the stroke cap. The dynamic gate must not open (optionIsDynamic is the shared predicate), so
    // the stroke runs the static accumulation path: the same bytes as no option at all.
    BrushParams with = hardBrush();
    with.opacity = 0.7;
    with.flow = 0.6;
    with.hardness = 0.6;
    CurveOptionData d;
    d.name = "Opacity";
    d.checkable = false;
    d.strength = 0.7;
    d.useCurve = false;
    d.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
    auto o = std::make_shared<BrushOptions>();
    o->opacity.emplace(std::move(d));
    with.options = std::move(o);

    BrushParams without = with;
    without.options = nullptr;

    const std::vector<StrokeInput> path{at(30, 48, 0.9), at(66, 48, 0.3)};
    const Image a = paint(with, path);
    const Image b = paint(without, path);
    CHECK(a.rgba == b.rgba);
}
