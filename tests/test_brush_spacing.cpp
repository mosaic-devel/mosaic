#include "common/image.hpp"
#include "core/brush/bitmap_tip.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/brush/brush_tip.hpp"
#include "core/brush/mask_generator.hpp"

#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <memory>
#include <vector>

// Spacing (S19 Arc A step 7, docs/brushes.md §6.2): the dab interval keys off the EFFECTIVE
// (pressure-scaled) size and is re-resolved after every dab from that dab's own pressure -- not
// from the nominal diameter once per segment. `useAutoSpacing` (XML default "0") switches to the
// format's absolute step of `autoSpacingCoeff * sqrt(diameter)` px for tips >= 1 px, linear below.
//
// Dab counts are pinned exactly: every quantity is dyadic (and every sqrt of a perfect square), so
// each dab position is an exact double and the counts cannot wobble.
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::MaskingOp;
using mosaic::core::brush::StrokeAccumulator;
using mosaic::core::brush::StrokeInput;

Color8 pixel(const Image& img, int x, int y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

// Runs the stroke and returns how many dabs the primary walk laid, counted through the Colored
// accumulator's per-dab colour hook -- the only seam that sees every deposited dab.
std::size_t countDabs(BrushParams p, BrushDynamics d, const std::vector<StrokeInput>& path,
                      std::uint32_t dim = 128) {
    std::size_t count = 0;
    p.accumulator = StrokeAccumulator::Colored;
    d.dabColor = [&count](std::size_t, double) {
        ++count;
        return Color8{0, 0, 0, 255};
    };
    Image img(dim, dim);
    BrushEngine eng;
    eng.begin(dim, dim, img, p, d, path.front());
    for (std::size_t i = 1; i < path.size(); ++i)
        eng.extendTo(path[i]);
    eng.flush(); // the walk lags one sample; lay the tail span before reading the pixels
    eng.composite();
    eng.end();
    return count;
}

constexpr double kPi = 3.14159265358979323846;

// A hard procedural tip. Its AUTHORED ratio is irrelevant to the cadence -- the DAB's ratio is what
// squashes it, and that is what the ellipse reads -- so this is just "a tip that is not null", which
// is what puts the engine on the tip path at all.
[[nodiscard]] mosaic::core::brush::MaskGeneratorParams knifeGen() {
    mosaic::core::brush::MaskGeneratorParams g;
    g.shape = mosaic::core::brush::MaskShape::Circle;
    g.falloff = mosaic::core::brush::MaskFalloff::Default;
    g.hFade = g.vFade = 1.0;
    return g;
}

// A solid opaque bitmap frame, `w` x `h`. The tip-image convention is white = no paint, so BLACK is
// full coverage.
[[nodiscard]] std::shared_ptr<const mosaic::core::brush::BitmapTip> solidFrame(int w, int h) {
    mosaic::core::brush::TipFrame f;
    f.width = w;
    f.height = h;
    f.rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 255);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * static_cast<std::size_t>(h); ++i)
        f.rgba[i * 4] = f.rgba[i * 4 + 1] = f.rgba[i * 4 + 2] = 0;
    return std::make_shared<const mosaic::core::brush::BitmapTip>(
        std::vector<mosaic::core::brush::TipFrame>{std::move(f)},
        mosaic::core::brush::TipApplication::AlphaMask, mosaic::core::brush::TipSourceKind::Mask,
        mosaic::core::brush::TipAdjustments{}, mosaic::core::brush::HoseParams{});
}

} // namespace

TEST_CASE("auto spacing steps at coeff * sqrt(diameter) pixels") {
    // Diameter 100 over a 100 px segment. Nominal spacing 0.25 steps 25 px: dabs at 25/50/75/100
    // plus the press. Auto spacing steps sqrt(100) = 10 px: ten dabs plus the press -- and if the
    // sqrt were dropped (step = coeff * diameter = 100), exactly one dab beyond the press.
    BrushParams p;
    p.diameter = 100.0;
    p.flow = 1.0;
    p.spacing = 0.25;
    const std::vector<StrokeInput> path{StrokeInput{{10.0, 64.0}, 1.0},
                                         StrokeInput{{110.0, 64.0}, 1.0}};

    CHECK(countDabs(p, BrushDynamics{}, path) == 5);
    p.useAutoSpacing = true;
    p.autoSpacingCoeff = 1.0;
    CHECK(countDabs(p, BrushDynamics{}, path) == 11);
    p.autoSpacingCoeff = 2.0;
    CHECK(countDabs(p, BrushDynamics{}, path) == 6);
}

TEST_CASE("auto spacing goes linear below one pixel") {
    // Diameter 0.5 < 1: the step is coeff * diameter = 0.5 px, exactly the loop's own floor. Over
    // a 5 px segment that is ten dabs plus the press; via sqrt(0.5) it would be seven plus one.
    BrushParams p;
    p.diameter = 0.5;
    p.flow = 1.0;
    p.useAutoSpacing = true;
    p.autoSpacingCoeff = 1.0;
    const std::vector<StrokeInput> path{StrokeInput{{40.0, 64.0}, 1.0},
                                         StrokeInput{{45.0, 64.0}, 1.0}};
    CHECK(countDabs(p, BrushDynamics{}, path) == 11);
}

TEST_CASE("spacing follows the effective size, re-resolved at every dab") {
    // Diameter 32, spacing 0.5, sizeFromPressure on. Segment one holds pressure 1 (step 16): dabs
    // at 16 and 32. Segment two falls linearly to 0 over 32 px, and each dab's step comes from ITS
    // pressure: 16 -> x=48 (pr 1/2), 8 -> x=56 (pr 1/4), 4 -> x=60 (pr 1/8), 2 -> x=62 (pr 1/16),
    // 1 -> x=63 (pr 1/32, effective dia 1), 0.5 -> x=63.5, then the sub-px floor keeps the step at
    // 0.5 -> x=64 (pr 0) -- seven dabs, every position dyadic. Segment three holds pressure 0, and
    // its cadence must key off the LAST DAB'S pressure (0, floored dia 0.1 -> 0.5 px steps, 64
    // dabs over 32 px), not the press's: 1 + 2 + 7 + 64 = 74.
    //
    // The regressions this pins: keying off the nominal diameter (step 16 everywhere -> 7);
    // resolving spacing once per segment instead of per dab (segment two collapses to 2 dabs ->
    // 69); and a last-dab pressure frozen at the press's (segment three opens with a step-16
    // stride before adapting -> 43).
    BrushParams p;
    p.diameter = 32.0;
    p.flow = 1.0;
    p.spacing = 0.5;
    BrushDynamics d;
    d.sizeFromPressure = true;
    const std::vector<StrokeInput> path{
        StrokeInput{{0.0, 64.0}, 1.0}, StrokeInput{{32.0, 64.0}, 1.0},
        StrokeInput{{64.0, 64.0}, 0.0}, StrokeInput{{96.0, 64.0}, 0.0}};
    CHECK(countDabs(p, d, path) == 74);
}

TEST_CASE("the masking walk keys off its own effective size and auto spacing") {
    // The primary lays exactly one dab (press, spacing 10 x diameter 20 outruns the 10 px path)
    // with coverage 1 and cap 1 at the probe; every alpha byte below is 255 * (1 - mask) with the
    // mask a product of 1/2-flow masking dabs whose count IS the cadence under test. Masking tip:
    // diameter 16, hardness 1 -> solid core to 7.25, rim to R. The stroke runs along y = 48.5 from
    // x = 48.5 -- ON the probe pixel's sampling centre (dabCoverage samples at x + 0.5), so every
    // dab-to-probe distance is exactly its path offset and lands squarely in core or beyond R,
    // never on the rim.
    BrushParams p;
    p.diameter = 20.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 1.0;
    p.spacing = 10.0;
    p.color = Color8{0, 0, 0, 255};
    p.masking.enabled = true;
    p.masking.op = MaskingOp::Subtract;
    p.masking.diameter = 16.0;
    p.masking.hardness = 1.0;
    p.masking.flow = 0.5;
    p.masking.spacing = 0.10;

    const auto probeAlpha = [&](double pressure) {
        Image img(96, 96);
        img.fill(Color8{0, 0, 0, 0});
        BrushEngine eng;
        eng.begin(96, 96, img, p, BrushDynamics{}, StrokeInput{{48.5, 48.5}, pressure});
        eng.extendTo(StrokeInput{{58.5, 48.5}, pressure});
        eng.flush();
        eng.composite();
        eng.end();
        return pixel(img, 48, 48).a;
    };

    // Nominal masking spacing 0.10 x 16 = 1.6 px: dabs at 0/1.6/3.2/4.8/6.4 all cover the probe
    // solidly (7.25 px core) -> mask 1 - (1/2)^5 = 31/32, alpha' = 1/32 -> 8.
    CHECK(probeAlpha(1.0) == 8);

    // Auto spacing sqrt(16) = 4 px: dabs at 0 and 4 cover, 8 lands on the rim's edge (cov 0) ->
    // mask 3/4, alpha' = 1/4 -> 64. A linear step (coeff * 16) would leave the press dab alone
    // (mask 1/2 -> 128).
    p.masking.useAutoSpacing = true;
    p.masking.autoSpacingCoeff = 1.0;
    CHECK(probeAlpha(1.0) == 64);

    // The masking walk's OWN PressureSize gate scales both its tip and its cadence. Diameter 64
    // gated at pressure 1/4: effective 16 -> R 8, auto step sqrt(16) = 4, press + x=4 cover the
    // probe -> mask 3/4 -> 64. A cadence keyed off the NOMINAL 64 would stride sqrt(64) = 8
    // straight past the shrunken core (d = 8 >= R) and leave mask 1/2 -> 128. (The primary is
    // untouched: its own sizeFromPressure stays off, so its press dab still covers at pressure
    // 1/4 with coverage 1.)
    p.masking.diameter = 64.0;
    p.masking.sizeFromPressure = true;
    CHECK(probeAlpha(0.25) == 64);

    // Control: gate off, the pressure is irrelevant -- nominal 64 drives both the radius (32, so
    // the core swallows the probe) and the auto step (8): press + x=8 -> mask 3/4 -> 64 again,
    // now via the WIDE tip's cadence.
    p.masking.sizeFromPressure = false;
    CHECK(probeAlpha(0.25) == 64);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// SPACING IS AN ELLIPSE (docs/brushes.md §6.2). The interval is a fraction of BOTH of the dab's
// extents, and the step actually taken is that ellipse's radius in the DIRECTION OF TRAVEL. Until
// this landed the cadence keyed off the dab's WIDTH alone, so `i)_Wet_Knife` -- 75 px along its
// blade, 15 px across it -- stamped FIVE TIMES too sparsely across the thin axis, which is what the
// user saw.
//
// ⚠ NOT ONE GOLDEN IN THE SUITE MOVED WHEN THIS LANDED, AND THAT IS NOT REASSURANCE -- it is the
// whole reason these cases had to be written. Every golden was laid by a ROUND tip, where the two
// extents are equal and the ellipse degenerates to the scalar step it always was.
//
// Every quantity below is DYADIC so the dab counts are exact doubles and cannot wobble: a 64 x 16
// tip (diameter 64, ratio 1/4) at spacing 1/8 gives sx = 8 and sy = 2 exactly.

TEST_CASE("the spacing step is the ellipse's radius along the heading") {
    using mosaic::core::brush::SpacingEllipse;
    using mosaic::core::brush::spacingStepAlong;

    // A ROUND cadence answers its scalar EXACTLY, at every heading -- not to within a rounding
    // error, but bit for bit. This is the check that keeps every golden alive: `1/sqrt(cos^2+sin^2)`
    // is not exactly 1.0 for an arbitrary angle, so a build that dropped the round branch and ran a
    // round tip through the ellipse arithmetic would shift its dabs by an ulp. Kill that mutant.
    const SpacingEllipse round{8.0, 8.0, 0.0};
    for (int i = 0; i < 64; ++i) {
        const double heading = -3.0 + 0.1 * static_cast<double>(i);
        CHECK(spacingStepAlong(round, heading) == 8.0); // `==`, deliberately
    }
    // ... and it ignores its rotation, because a circle has no orientation to carry.
    const SpacingEllipse turned{8.0, 8.0, 1.0};
    CHECK(spacingStepAlong(turned, 0.4) == 8.0);

    // The knife: 8 px along its own x, 2 px along its y.
    const SpacingEllipse knife{8.0, 2.0, 0.0};
    CHECK(spacingStepAlong(knife, 0.0) == doctest::Approx(8.0));          // due east: the long axis
    CHECK(spacingStepAlong(knife, kPi) == doctest::Approx(8.0));          // and due west
    CHECK(spacingStepAlong(knife, kPi / 2.0) == doctest::Approx(2.0));    // north: the thin axis
    CHECK(spacingStepAlong(knife, -kPi / 2.0) == doctest::Approx(2.0));   // and south

    // ⚠ THE DIAGONAL IS THE PRIMARY CHECK AND THE TWO AXES ARE THE BACKSTOP, not the other way
    // round. An implementation that merely PICKED sx or sy by whichever axis the heading is nearer
    // would pass all four probes above; only an off-axis heading evaluates the ellipse. At 45 deg
    // the answer is neither 8 nor 2, and it is much closer to the THIN axis -- an ellipse's radius
    // is dominated by its short semi-axis away from the long one's line.
    const double c = std::cos(kPi / 4.0) / 8.0;
    const double s = std::sin(kPi / 4.0) / 2.0;
    const double diag = 1.0 / std::sqrt(c * c + s * s);
    CHECK(diag == doctest::Approx(2.7442).epsilon(0.001));
    CHECK(spacingStepAlong(knife, kPi / 4.0) == doctest::Approx(diag));

    // The rotation turns the ellipse with the tip: a knife stood on end steps 2 px east and 8 north.
    const SpacingEllipse upright{8.0, 2.0, kPi / 2.0};
    CHECK(spacingStepAlong(upright, 0.0) == doctest::Approx(2.0));
    CHECK(spacingStepAlong(upright, kPi / 2.0) == doctest::Approx(8.0));
}

TEST_CASE("a shaped tip lays dabs along its blade and across it at different rates") {
    // A 64 x 16 knife (diameter 64, ratio 1/4), spacing 1/8 -> sx = 8, sy = 2. Over a 64 px stroke:
    // ALONG the blade, dabs at 8,16..64 = 8, plus the press = 9. ACROSS it, dabs at 2,4..64 = 32,
    // plus the press = 33 -- four times as many, exactly as the tip is four times as long as it is
    // wide.
    //
    // ⚠ BEFORE THIS FIX BOTH ANSWERED 9. That is the bug, and the whole of it.
    BrushParams p;
    p.diameter = 64.0;
    p.ratio = 0.25;
    p.flow = 1.0;
    p.spacing = 0.125;
    p.tip = mosaic::core::brush::makeTip(knifeGen());

    const std::vector<StrokeInput> along{StrokeInput{{96.0, 96.0}, 1.0},
                                         StrokeInput{{160.0, 96.0}, 1.0}};
    const std::vector<StrokeInput> across{StrokeInput{{96.0, 96.0}, 1.0},
                                          StrokeInput{{96.0, 160.0}, 1.0}};
    CHECK(countDabs(p, BrushDynamics{}, along, 256) == 9);
    CHECK(countDabs(p, BrushDynamics{}, across, 256) == 33);

    // A quarter turn stands the blade on end, and the two cadences SWAP. This is what pins that the
    // ellipse is carried in the tip's frame and not the document's -- an implementation that built
    // the right two intervals but never rotated them would answer 9 and 33 again.
    p.angleRad = kPi / 2.0;
    CHECK(countDabs(p, BrushDynamics{}, along, 256) == 33);
    CHECK(countDabs(p, BrushDynamics{}, across, 256) == 9);
}

TEST_CASE("Spacing Isotropic keys the cadence off the larger extent in every direction") {
    // The author's opt-out (§3.2; 2 shipped presets set it, both bitmap chalks). The same 64 x 16
    // knife, but the cadence now reads max(64, 16) = 64 -> a step of 8 whichever way it is dragged.
    // Both answers are the ALONG count, and the ACROSS one moves from 33 back to 9.
    BrushParams p;
    p.diameter = 64.0;
    p.ratio = 0.25;
    p.flow = 1.0;
    p.spacing = 0.125;
    p.isotropicSpacing = true;
    p.tip = mosaic::core::brush::makeTip(knifeGen());

    const std::vector<StrokeInput> along{StrokeInput{{96.0, 96.0}, 1.0},
                                         StrokeInput{{160.0, 96.0}, 1.0}};
    const std::vector<StrokeInput> across{StrokeInput{{96.0, 96.0}, 1.0},
                                          StrokeInput{{96.0, 160.0}, 1.0}};
    CHECK(countDabs(p, BrushDynamics{}, along, 256) == 9);
    CHECK(countDabs(p, BrushDynamics{}, across, 256) == 9);

    // ... and the angle cannot bring the anisotropy back: a circle has no orientation.
    p.angleRad = kPi / 2.0;
    CHECK(countDabs(p, BrushDynamics{}, across, 256) == 9);
}

TEST_CASE("a bitmap tip's FRAME aspect drives the cadence -- its ratio is 1 and says nothing") {
    // ⚠ THE TRAP. A bitmap tip's `ratio` is 1 by construction: its frame's own aspect lives INSIDE
    // the dab's envelope (io/brush/preset_brush.cpp). So `(diameter, diameter * ratio)` is a SQUARE
    // for every bitmap tip in the corpus, and a cadence derived from that pair is round for all of
    // them -- including the 22 shipped tips that are not circles. The extents have to come from the
    // TIP, which is the only thing that knows the frame.
    //
    // A 32 x 16 frame at diameter 64 paints 64 x 32 (the long axis IS the diameter, §3.5). Spacing
    // 1/8 -> sx = 8, sy = 4. Over 64 px: along = 8 + press = 9; across = 16 + press = 17.
    BrushParams p;
    p.diameter = 64.0;
    p.ratio = 1.0; // as every bitmap preset has it
    p.flow = 1.0;
    p.spacing = 0.125;
    p.tip = mosaic::core::brush::makeTip(solidFrame(32, 16));

    const std::vector<StrokeInput> along{StrokeInput{{96.0, 96.0}, 1.0},
                                         StrokeInput{{160.0, 96.0}, 1.0}};
    const std::vector<StrokeInput> across{StrokeInput{{96.0, 96.0}, 1.0},
                                          StrokeInput{{96.0, 160.0}, 1.0}};
    CHECK(countDabs(p, BrushDynamics{}, along, 256) == 9);
    CHECK(countDabs(p, BrushDynamics{}, across, 256) == 17);

    // A SQUARE frame is the control: same tip machinery, round cadence, and the two agree again.
    p.tip = mosaic::core::brush::makeTip(solidFrame(16, 16));
    CHECK(countDabs(p, BrushDynamics{}, along, 256) == 9);
    CHECK(countDabs(p, BrushDynamics{}, across, 256) == 9);
}

TEST_CASE("auto spacing is applied per axis, so it is an ellipse too") {
    // The 64 x 16 knife again, auto-spacing at coeff 1: the interval is sqrt(extent) PER AXIS, so
    // sx = sqrt(64) = 8 and sy = sqrt(16) = 4 -- both exact. Over 64 px: along = 8 + press = 9;
    // across = 16 + press = 17. Deriving both from the width would give 9 and 9.
    BrushParams p;
    p.diameter = 64.0;
    p.ratio = 0.25;
    p.flow = 1.0;
    p.useAutoSpacing = true;
    p.autoSpacingCoeff = 1.0;
    p.tip = mosaic::core::brush::makeTip(knifeGen());

    const std::vector<StrokeInput> along{StrokeInput{{96.0, 96.0}, 1.0},
                                         StrokeInput{{160.0, 96.0}, 1.0}};
    const std::vector<StrokeInput> across{StrokeInput{{96.0, 96.0}, 1.0},
                                          StrokeInput{{96.0, 160.0}, 1.0}};
    CHECK(countDabs(p, BrushDynamics{}, along, 256) == 9);
    CHECK(countDabs(p, BrushDynamics{}, across, 256) == 17);
}

TEST_CASE("the half-pixel spacing floor is per AXIS, and only a DIAGONAL can see that") {
    // The floor belongs on each AXIS as the ellipse is built (which is where the format puts it),
    // NOT on the step the walk finally takes. The two are easy to conflate, and a hair-thin nib is
    // where they part company.
    //
    // A 64 x 0.5 nib at spacing 1/8 wants sy = 0.0625, which floors to 0.5. Then:
    //
    //   heading    floor the AXIS (correct)   floor the STEP (the plausible mistake)
    //   ────────────────────────────────────────────────────────────────────────────
    //   along           8.0000                        8.0000     <- same
    //   across          0.5000                        0.5000     <- same
    //   DIAGONAL        0.7057                        0.5000     <- DIFFERENT
    //
    // ⚠ SO THE TWO ON-AXIS PROBES PASS ON BOTH, AND ONLY AN OFF-AXIS ONE TELLS THEM APART. This is
    // the same trap the ratio test fell into (docs/brushes.md §6.2): the axes are the backstop and
    // the diagonal is the primary check. The first cut of THIS case dragged only across the nib and
    // would have shipped the mistake.
    //
    // Every quantity is dyadic: ratio 1/128 makes the nib exactly 0.5 px tall.
    BrushParams p;
    p.diameter = 64.0;
    p.ratio = 0.0078125; // 0.5 / 64
    p.flow = 1.0;
    p.spacing = 0.125;
    p.tip = mosaic::core::brush::makeTip(knifeGen());

    // Across: 64 px at the floored 0.5 -> 128 dabs, plus the press. Both builds agree here.
    const std::vector<StrokeInput> across{StrokeInput{{96.0, 96.0}, 1.0},
                                          StrokeInput{{96.0, 160.0}, 1.0}};
    CHECK(countDabs(p, BrushDynamics{}, across, 256) == 129);

    // Diagonal: a 64 x 64 leg is 90.51 px of arc. The ellipse's radius at 45 deg with a FLOORED
    // minor axis is 0.7057 px -> 128 dabs, plus the press. Floor the step instead and it collapses
    // to 0.5 -> 181 dabs and 182 in all: the nib would be stamped half again as densely as it
    // should be, for no reason but where the max() went.
    const std::vector<StrokeInput> diagonal{StrokeInput{{96.0, 96.0}, 1.0},
                                            StrokeInput{{160.0, 160.0}, 1.0}};
    CHECK(countDabs(p, BrushDynamics{}, diagonal, 256) == 129);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// THE SPACING OPTION (docs/brushes.md §6.6e). The reference's KisSpacingOption::apply returns
// computeSizeLikeValue WITH strength over [0,1] and multiplies the WHOLE spacing interval -- both
// axes, in every branch -- so a value below 1 lays dabs DENSER. Transcribed as a per-dab cadence
// scale (brush_engine.cpp's m_dabSpacingScale), NOT a dab-shape option. Every quantity below is
// dyadic so the dab counts are exact doubles and cannot wobble.

namespace {

using mosaic::core::brush::BrushOptions;
using mosaic::core::brush::CurveOption;
using mosaic::core::brush::CurveOptionData;
using mosaic::core::brush::Sensor;
using mosaic::core::brush::SensorId;

// A Spacing option that resolves to a constant cadence scale `value`: a pressure sensor with an
// identity curve at strength = value, so at pressure 1 the value IS `value`, deterministically (no
// random draw -- pressure is not a random sensor).
[[nodiscard]] std::shared_ptr<BrushOptions> spacingOption(double value, bool checked = true) {
    CurveOptionData d;
    d.name = "Spacing";
    d.checkable = true;
    d.checked = checked;
    d.strength = value;
    d.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
    auto o = std::make_shared<BrushOptions>();
    o->spacing = CurveOption(d);
    return o;
}

// Paint a whole stroke and hand back the composited image (the spacing tests above count dabs; the
// byte-identity ones need the pixels). end() flushes the tail span.
[[nodiscard]] Image paintImg(const BrushParams& p, const std::vector<StrokeInput>& path,
                             std::uint32_t dim) {
    Image img(dim, dim);
    BrushEngine eng;
    eng.begin(dim, dim, img, p, BrushDynamics{}, path.front());
    for (std::size_t i = 1; i < path.size(); ++i)
        eng.extendTo(path[i]);
    eng.end();
    eng.composite();
    return img;
}

} // namespace

TEST_CASE("the Spacing option scales the whole cadence") {
    // Diameter 100, spacing 0.25 -> a 25 px step: dabs at 25/50/75/100 plus the press = 5 (the same
    // baseline the auto-spacing case opens with). The option multiplies the interval, so a scale of
    // 0.5 halves it (12.5 px -> 8 walked + press = 9) and 0.25 quarters it (6.25 px -> 16 + press =
    // 17). If the `* scale` term were dropped, all three answer 5.
    BrushParams p;
    p.diameter = 100.0;
    p.flow = 1.0;
    p.spacing = 0.25;
    const std::vector<StrokeInput> path{StrokeInput{{10.0, 128.0}, 1.0},
                                        StrokeInput{{110.0, 128.0}, 1.0}};
    CHECK(countDabs(p, BrushDynamics{}, path, 256) == 5);
    p.options = spacingOption(0.5);
    CHECK(countDabs(p, BrushDynamics{}, path, 256) == 9);
    p.options = spacingOption(0.25);
    CHECK(countDabs(p, BrushDynamics{}, path, 256) == 17);
}

TEST_CASE("the Spacing scale multiplies the interval BEFORE the half-pixel floor") {
    // A scale toward zero cannot spin the dab loop forever: the interval is floored at 0.5 px AFTER
    // the scale, exactly as the raw interval is. Diameter 8, spacing 0.25 -> a 2 px step; scale
    // 1/32 wants 0.0625 px, which floors to 0.5. Over a 32 px stroke that is 64 dabs plus the press.
    // (Floor the raw interval and THEN scale, and a scale of 0 would give a zero step and hang.)
    BrushParams p;
    p.diameter = 8.0;
    p.flow = 1.0;
    p.spacing = 0.25;
    p.options = spacingOption(0.03125); // 1/32, dyadic
    const std::vector<StrokeInput> path{StrokeInput{{16.0, 64.0}, 1.0},
                                        StrokeInput{{48.0, 64.0}, 1.0}};
    CHECK(countDabs(p, BrushDynamics{}, path, 128) == 65);
}

TEST_CASE("the Spacing scale rides the ellipse -- both axes, isotropic and anisotropic alike") {
    // The reference multiplies the whole QPointF, so a shaped tip's two cadences scale TOGETHER. A
    // 64 x 16 knife (ratio 1/4), spacing 1/8 -> sx = 8, sy = 2; a scale of 0.5 gives sx = 4, sy = 1.
    // Along its blade over 64 px: 16 + press = 17; across it: 64 + press = 65. Both doubled from the
    // no-option 9 and 33 -- the scale did not privilege one axis.
    BrushParams p;
    p.diameter = 64.0;
    p.ratio = 0.25;
    p.flow = 1.0;
    p.spacing = 0.125;
    p.tip = mosaic::core::brush::makeTip(knifeGen());
    p.options = spacingOption(0.5);
    const std::vector<StrokeInput> along{StrokeInput{{96.0, 96.0}, 1.0},
                                         StrokeInput{{160.0, 96.0}, 1.0}};
    const std::vector<StrokeInput> across{StrokeInput{{96.0, 96.0}, 1.0},
                                          StrokeInput{{96.0, 160.0}, 1.0}};
    CHECK(countDabs(p, BrushDynamics{}, along, 256) == 17);
    CHECK(countDabs(p, BrushDynamics{}, across, 256) == 65);

    // Isotropic keys off the larger extent (64) for both axes; the scale still applies. sx = sy =
    // 8 * 0.5 = 4 -> along AND across both 16 + press = 17.
    p.isotropicSpacing = true;
    CHECK(countDabs(p, BrushDynamics{}, along, 256) == 17);
    CHECK(countDabs(p, BrushDynamics{}, across, 256) == 17);
}

TEST_CASE("an inert Spacing option leaves the cadence and the bytes exactly as no option") {
    // Unchecked, the option contributes the identity 1.0 and draws NOTHING -- so a stroke carrying
    // it must be byte-for-byte the stroke with no option at all (the §6.2 rule, extended to the
    // stream). A hard black tip so the alpha reads the coverage directly.
    BrushParams p;
    p.diameter = 40.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.spacing = 0.2;
    p.color = Color8{0, 0, 0, 255};
    const std::vector<StrokeInput> path{StrokeInput{{30.0, 128.0}, 1.0},
                                        StrokeInput{{60.0, 100.0}, 1.0},
                                        StrokeInput{{120.0, 150.0}, 1.0},
                                        StrokeInput{{200.0, 128.0}, 1.0}};
    const Image plain = paintImg(p, path, 256);

    p.options = spacingOption(0.4, /*checked=*/false); // present but off
    const Image inert = paintImg(p, path, 256);
    CHECK(inert.rgba == plain.rgba);

    // ... and a CHECKED one really does change the stroke, or the equality above pins nothing.
    p.options = spacingOption(0.4, /*checked=*/true);
    CHECK(paintImg(p, path, 256).rgba != plain.rgba);
}
