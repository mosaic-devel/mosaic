#include "common/image.hpp"
#include "core/blend_mode.hpp"
#include "core/brush/bitmap_tip.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/brush/brush_tip.hpp"
#include "core/brush/stroke_path.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <memory>
#include <vector>

// The masking brush (S19 Arc A step 6, docs/brushes.md §6.2): a SECOND dab walk along the same
// stroke path -- its own tip, spacing and pressure response -- accumulating a stroke-scoped
// grayscale mask, which composite() applies to the paint stroke's ACCUMULATED alpha per pixel:
// `alpha' = maskingOp(mask, alpha)`, before Wash's opacity ceiling. Not a per-dab mask multiply --
// the corrected §6.2 records the evidence.
//
// The three ops are the format default (`multiply`) plus everything the six shipped masking
// presets use (`subtract`, `linear_dodge`). Exact pins are hand-derived with dyadic quantities.
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::core::BlendMode;
using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::maskingOp;
using mosaic::core::brush::MaskingOp;
using mosaic::core::brush::PaintMode;
using mosaic::core::brush::StrokeAccumulator;
using mosaic::core::brush::StrokeMode;
using mosaic::core::brush::StrokeInput;

Color8 pixel(const Image& img, int x, int y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

const std::vector<StrokeInput>& crossingPath() {
    static const std::vector<StrokeInput> path{
        StrokeInput{{20.0, 20.0}, 1.0}, StrokeInput{{76.0, 76.0}, 1.0},
        StrokeInput{{76.0, 20.0}, 1.0}, StrokeInput{{20.0, 76.0}, 1.0}};
    return path;
}

Image paint(BrushParams p, const std::vector<StrokeInput>& path, Color8 fill) {
    Image img(96, 96);
    img.fill(fill);
    BrushEngine eng;
    eng.begin(96, 96, img, p, BrushDynamics{}, path.front());
    for (std::size_t i = 1; i < path.size(); ++i)
        eng.extendTo(path[i]);
    eng.flush(); // the walk lags one sample; lay the tail span before reading the pixels
    eng.composite();
    eng.end();
    return img;
}

// One primary dab at (48,48): hardness 1, radius 10, flow 1 -> coverage exactly 1.0 well inside
// the core, black at cap = opacity * 1.
BrushParams onePrimaryDab(double opacity) {
    BrushParams p;
    p.diameter = 20.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = opacity;
    p.spacing = 0.1;
    p.color = Color8{0, 0, 0, 255};
    return p;
}

// A masking dab landing at the same press point: radius 4 (core to 3.25), so pixel (48,48) sits
// under it at the given flow while (54,48) -- still deep inside the primary's core -- does not.
void smallMask(BrushParams& p, MaskingOp op, double flow) {
    p.masking.enabled = true;
    p.masking.op = op;
    p.masking.diameter = 8.0;
    p.masking.hardness = 1.0;
    p.masking.flow = flow;
    p.masking.spacing = 0.1;
}

} // namespace

TEST_CASE("maskingOp pins the three transcribed formulas") {
    // multiply: the format default. subtract / linear_dodge: everything the shipped set uses,
    // with their clamps at 0 and 1 respectively.
    CHECK(maskingOp(MaskingOp::Multiply, 0.5, 0.5) == doctest::Approx(0.25));
    CHECK(maskingOp(MaskingOp::Multiply, 0.0, 1.0) == 0.0);
    CHECK(maskingOp(MaskingOp::Subtract, 0.25, 1.0) == doctest::Approx(0.75));
    CHECK(maskingOp(MaskingOp::Subtract, 1.0, 0.5) == 0.0);  // clamped, not negative
    CHECK(maskingOp(MaskingOp::LinearDodge, 0.5, 0.25) == doctest::Approx(0.75));
    CHECK(maskingOp(MaskingOp::LinearDodge, 0.75, 0.5) == 1.0); // clamped at 1
}

TEST_CASE("a full multiply mask is byte-identical to no mask at all") {
    // The masking tip much larger than the primary (60 px vs 18, dabs every ~3 px along the same
    // path at hardness 1): every painted pixel sits deep inside some masking dab's solid core, so
    // the mask value is exactly 1.0 there, and multiply by 1.0 is exact in floats. Any plumbing
    // that perturbs the unmasked arithmetic -- or a mask that fails to reach 1 -- moves a byte.
    BrushParams plain;
    plain.diameter = 18.0;
    plain.hardness = 0.9;
    plain.flow = 1.0;
    plain.opacity = 0.5;
    plain.spacing = 0.05;
    plain.color = Color8{40, 80, 120, 255};
    BrushParams masked = plain;
    masked.masking.enabled = true;
    masked.masking.op = MaskingOp::Multiply;
    masked.masking.diameter = 60.0;
    masked.masking.hardness = 1.0;
    masked.masking.flow = 1.0;
    masked.masking.spacing = 0.05;

    const Image a = paint(plain, crossingPath(), Color8{200, 190, 180, 255});
    const Image b = paint(masked, crossingPath(), Color8{200, 190, 180, 255});
    CHECK(a.rgba == b.rgba);
}

TEST_CASE("subtract carves the mask out of the stroke, before the Wash ceiling") {
    // Coverage 1 under both dabs; mask 0.5 under the small masking dab only. All dyadic:
    //   masked pixel:   sa = (1.0 - 0.5) * cap 0.5 = 0.25 -> lround(63.75) = 64
    //   unmasked pixel: sa =  1.0        * cap 0.5 = 0.5  -> lround(127.5) = 128
    // Two mutations this catches: swapped operands give max(0, 0.5 - 1.0) = 0, and applying the
    // ceiling BEFORE the op gives max(0, 0.5 - 0.5) = 0 -- both land 0, not 64.
    BrushParams p = onePrimaryDab(0.5);
    smallMask(p, MaskingOp::Subtract, 0.5);

    const Image img = paint(p, {StrokeInput{{48.0, 48.0}, 1.0}}, Color8{0, 0, 0, 0});
    CHECK(pixel(img, 48, 48).a == 64);
    CHECK(pixel(img, 54, 48).a == 128);
}

TEST_CASE("linear_dodge adds the mask into the stroke, before the Wash ceiling") {
    // Primary flow 0.5 -> coverage 0.5 everywhere under the dab; mask 0.5 under the masking dab:
    //   masked pixel:   sa = min(1, 0.5 + 0.5) * cap 0.5 = 0.5  -> 128
    //   unmasked pixel: sa = 0.5               * cap 0.5 = 0.25 -> lround(63.75) = 64
    BrushParams p = onePrimaryDab(0.5);
    p.flow = 0.5;
    smallMask(p, MaskingOp::LinearDodge, 0.5);

    const Image img = paint(p, {StrokeInput{{48.0, 48.0}, 1.0}}, Color8{0, 0, 0, 0});
    CHECK(pixel(img, 48, 48).a == 128);
    CHECK(pixel(img, 54, 48).a == 64);
}

TEST_CASE("the mask never paints alone") {
    // A masking tip far larger than the primary: pixels only the MASKING stroke touched must stay
    // pristine for every op -- including linear_dodge, where the reference needs an explicit
    // zero-guard for exactly this (§6.2), and multiply, where a mask of 0.75 over nothing is
    // still nothing.
    for (const MaskingOp op : {MaskingOp::Multiply, MaskingOp::Subtract, MaskingOp::LinearDodge}) {
        BrushParams p = onePrimaryDab(1.0);
        p.diameter = 10.0; // primary radius 5
        p.masking.enabled = true;
        p.masking.op = op;
        p.masking.diameter = 60.0; // masking radius 30
        p.masking.hardness = 1.0;
        p.masking.flow = 0.75;
        p.masking.spacing = 0.1;

        const Image img = paint(p, {StrokeInput{{48.0, 48.0}, 1.0}}, Color8{10, 200, 90, 255});
        // (66,48): 17.5 px out -- outside the primary's radius 5, deep inside the mask's core.
        CHECK(pixel(img, 66, 48) == Color8{10, 200, 90, 255});
        // ... while the primary's own centre really did something (except full subtract).
        if (op != MaskingOp::Subtract)
            CHECK(pixel(img, 48, 48).a == 255);
    }
}

TEST_CASE("a masking dab arriving after a composite carves the pixel back to its base bytes") {
    // The pixel is painted and composited FIRST; only then does the masking walk lay a dab over
    // it, driving subtract's result to exactly 0. The recomposite must write the base bytes back
    // -- an implementation that skips sa == 0 pixels (as the unmasked path rightly does) leaves
    // the stale black paint standing, which is precisely the bug this pins.
    //
    // Choreography: the press is at pressure 0, so the masking walk's first dab (flowFromPressure)
    // deposits nothing while the primary (flow fixed at 1) paints. The primary's spacing is huge
    // (no further primary dabs); the masking walk then lays dabs at pressure 0.75 and 1.0 along
    // two short segments, accumulating mask = 0.75 + 1*(1-0.75) = 1.0 exactly at (48,48).
    BrushParams p = onePrimaryDab(1.0);
    p.spacing = 10.0; // primary: the press dab and nothing else
    p.masking.enabled = true;
    p.masking.op = MaskingOp::Subtract;
    p.masking.diameter = 30.0;
    p.masking.hardness = 1.0;
    p.masking.flow = 1.0;
    p.masking.spacing = 0.05; // masking: dabs every 1.5 px
    p.masking.flowFromPressure = true;

    Image img(96, 96);
    img.fill(Color8{10, 200, 90, 255});
    BrushEngine eng;
    eng.begin(96, 96, img, p, BrushDynamics{}, StrokeInput{{48.0, 48.0}, 0.0});
    eng.composite();
    REQUIRE(pixel(img, 48, 48) == Color8{0, 0, 0, 255}); // painted, no mask yet

    eng.extendTo(StrokeInput{{50.0, 48.0}, 1.0});   // masking dab at t=0.75 -> flow 0.75
    eng.extendTo(StrokeInput{{51.5, 48.0}, 1.0});   // masking dab at pressure 1 -> mask reaches 1
    eng.flush();
    eng.composite();
    eng.end();
    CHECK(pixel(img, 48, 48) == Color8{10, 200, 90, 255}); // carved back to the pristine base
}

TEST_CASE("a fully carved pixel keeps the stashed RGB of a transparent base") {
    // Erase deliberately leaves a transparent pixel's colour bytes standing (un-premultiplied), so
    // "write the base bytes back" must mean VERBATIM bytes: routing sa = 0 through the general
    // source-over arithmetic instead lands in its oa <= 1e-6 branch when the base alpha is 0 and
    // stomps that stashed RGB with zeros. Full subtract mask over the primary's core carves sa to
    // exactly 0 at (48,48); the bytes there must be untouched, not merely "still transparent".
    BrushParams p = onePrimaryDab(1.0);
    smallMask(p, MaskingOp::Subtract, 1.0);

    const Image img = paint(p, {StrokeInput{{48.0, 48.0}, 1.0}}, Color8{10, 200, 90, 0});
    CHECK(pixel(img, 48, 48) == Color8{10, 200, 90, 0});
    CHECK(pixel(img, 54, 48) == Color8{0, 0, 0, 255}); // ... while the unmasked ring painted
}

TEST_CASE("Wash masks before its ceiling and Buildup after its per-dab one") {
    // The same numbers, both modes: primary one dab at flow 1, opacity 0.5; mask 0.25, subtract.
    //   Wash:    sa = (coverage 1.0 - 0.25) * cap 0.5      = 0.375 -> lround(95.625) = 96
    //   Buildup: sa =  max(0, build 0.5 - 0.25)            = 0.25  -> lround(63.75)  = 64
    // Buildup x masking is unreachable in the reference (§6.2); this pins Mosaic's definition --
    // the op applies to whatever accumulated alpha the mode produces.
    BrushParams wash = onePrimaryDab(0.5);
    smallMask(wash, MaskingOp::Subtract, 0.25);
    BrushParams build = wash;
    build.paintMode = PaintMode::Buildup;

    const Image w = paint(wash, {StrokeInput{{48.0, 48.0}, 1.0}}, Color8{0, 0, 0, 0});
    const Image b = paint(build, {StrokeInput{{48.0, 48.0}, 1.0}}, Color8{0, 0, 0, 0});
    CHECK(pixel(w, 48, 48).a == 96);
    CHECK(pixel(b, 48, 48).a == 64);
}

TEST_CASE("masking weakens an eraser the same way it weakens paint") {
    // Erase carves sa out of an opaque base; a subtract mask of 0.5 halves the carve:
    //   sa = (coverage 1.0 - 0.5) * cap 1.0 = 0.5 -> surviving alpha 128, colour untouched.
    BrushParams p = onePrimaryDab(1.0);
    p.strokeMode = StrokeMode::Erase;
    smallMask(p, MaskingOp::Subtract, 0.5);

    const Image img = paint(p, {StrokeInput{{48.0, 48.0}, 1.0}}, Color8{10, 200, 90, 255});
    CHECK(pixel(img, 48, 48) == Color8{10, 200, 90, 128});
    CHECK(pixel(img, 54, 48).a == 0); // unmasked: erased completely
}

TEST_CASE("masking does not touch the coverage buffer the Inpaint brush reads") {
    BrushParams plain = onePrimaryDab(0.5);
    BrushParams masked = plain;
    masked.masking.enabled = true;
    masked.masking.op = MaskingOp::Subtract;
    masked.masking.diameter = 40.0;
    masked.masking.flow = 1.0;

    const auto run = [](const BrushParams& p) {
        Image img(96, 96);
        BrushEngine eng;
        eng.begin(96, 96, img, p, BrushDynamics{}, crossingPath().front());
        for (std::size_t i = 1; i < crossingPath().size(); ++i)
            eng.extendTo(crossingPath()[i]);
        std::vector<float> cov = eng.coverage();
        eng.end();
        return cov;
    };
    const std::vector<float> cp = run(plain);
    const std::vector<float> cm = run(masked);
    REQUIRE(!cp.empty());
    CHECK(cp == cm);
}

TEST_CASE("the document size does not change any pixel a masked stroke paints") {
    // The mask is the FIFTH per-pixel buffer ensureCovers() re-homes. Buildup x Colored x masking
    // exercises all five at once; as always, the stroke must come BACK over an early pixel after
    // the excursion that grew the rect, because composite() only rewrites the pending region.
    BrushParams p;
    p.diameter = 24.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 0.15;
    p.spacing = 1.0;
    p.color = Color8{0, 0, 0, 255};
    p.paintMode = PaintMode::Buildup;
    p.accumulator = StrokeAccumulator::Colored;
    p.masking.enabled = true;
    p.masking.op = MaskingOp::Subtract;
    p.masking.diameter = 30.0; // wider than the primary: the mask rim exercises the inert marks
    p.masking.hardness = 0.5;
    // Both walks step 24 px (1.0 x 24 and 0.8 x 30) from the same press, so mask dabs land exactly
    // on primary dabs. The mask accumulates wash-style (1 - 0.9^n at flow 0.1) and must stay BELOW
    // Buildup's per-dab-capped accumulation (1 - 0.85^n at cap 0.15) or subtract clamps the probe
    // to nothing -- at flow 0.3 it overtakes from the first dab and the probe stayed white.
    p.masking.flow = 0.1;
    p.masking.spacing = 0.8;
    BrushDynamics d;
    d.dabColor = [](std::size_t dab, double) {
        return Color8{static_cast<std::uint8_t>(40 + (dab * 53) % 200),
                      static_cast<std::uint8_t>(40 + (dab * 101) % 200),
                      static_cast<std::uint8_t>(40 + (dab * 29) % 200), 255};
    };

    const std::vector<StrokeInput> path{StrokeInput{{60.0, 60.0}, 1.0},
                                         StrokeInput{{120.0, 60.0}, 1.0},
                                         StrokeInput{{60.0, 60.0}, 1.0},
                                         StrokeInput{{800.0, 700.0}, 1.0}, // grows the big doc
                                         StrokeInput{{120.0, 60.0}, 1.0},
                                         StrokeInput{{60.0, 60.0}, 1.0}}; // re-stamps the early px

    const auto run = [&](std::uint32_t dim, std::uint32_t* finalCw) {
        Image img(dim, dim);
        img.fill(Color8{255, 255, 255, 255});
        BrushEngine eng;
        eng.begin(dim, dim, img, p, d, path.front());
        eng.composite();
        for (std::size_t i = 1; i < path.size(); ++i) {
            eng.extendTo(path[i]);
            eng.composite();
        }
        eng.flush(); // the tail span, which no lookahead sample is coming for
        eng.composite();
        *finalCw = eng.coverageWidth();
        eng.end();
        return img;
    };

    std::uint32_t smallCw = 0, bigCw = 0;
    const Image small = run(200, &smallCw);
    const Image big = run(1024, &bigCw);

    REQUIRE(smallCw == 200u);
    REQUIRE(bigCw > 256u);

    // The early pixel accumulated, its colour axis engaged, and the mask really modulated it --
    // if the mask buffer were dropped on growth the modulation would differ between the runs.
    const Color8 early = pixel(big, 90, 60);
    REQUIRE(early.a == 255);
    REQUIRE(early.r < 255);
    REQUIRE(!(early.r == early.g && early.g == early.b));

    // ... and the mask must be LIVE at the probe: painted, but not carved to nothing. Without this
    // the identity check keeps passing after a calibration drift (a cadence change can move every
    // mask dab off the probe, or a stronger mask can subtract the probe back to white) -- the test
    // would then no longer prove the re-homed mask still modulates anything.
    p.masking.enabled = false;
    std::uint32_t plainCw = 0;
    const Image plain = run(1024, &plainCw);
    p.masking.enabled = true;
    REQUIRE(pixel(plain, 90, 60) != early);

    int badX = -1, badY = -1;
    std::size_t differing = 0;
    for (int y = 0; y < 200 && differing == 0; ++y)
        for (int x = 0; x < 200; ++x)
            if (pixel(small, x, y) != pixel(big, x, y)) {
                badX = x;
                badY = y;
                ++differing;
                break;
            }
    INFO("first differing pixel: (" << badX << ", " << badY << ")");
    CHECK(differing == 0);
}

TEST_CASE("restore reverts a masked stroke bytes-exactly") {
    BrushParams p = onePrimaryDab(0.9);
    p.paintMode = PaintMode::Buildup;
    p.blendMode = BlendMode::Multiply;
    p.masking.enabled = true;
    p.masking.op = MaskingOp::LinearDodge;
    p.masking.diameter = 40.0;
    p.masking.flow = 0.5;

    Image img(96, 96);
    img.fill(Color8{33, 44, 55, 200});
    const Image pristine = img;

    BrushEngine eng;
    eng.begin(96, 96, img, p, BrushDynamics{}, crossingPath().front());
    for (std::size_t i = 1; i < crossingPath().size(); ++i) {
        eng.extendTo(crossingPath()[i]);
        eng.composite();
    }
    REQUIRE(img.rgba != pristine.rgba);
    eng.restore();
    eng.end();
    CHECK(img.rgba == pristine.rgba);
}

// ---- The masking TIP (docs/brushes.md §6.2: the nested brush_definition's own shape) -----------

namespace {

using mosaic::core::brush::BitmapTip;
using mosaic::core::brush::makeTip;
using mosaic::core::brush::MaskGeneratorParams;
using mosaic::core::brush::MaskShape;
using mosaic::core::brush::TipApplication;
using mosaic::core::brush::TipFrame;
using mosaic::core::brush::TipSourceKind;

// A bitmap masking tip whose LEFT half paints and whose right half is empty -- a shape no analytic
// disc can imitate. Grey is the TIP IMAGE convention: 0 = full paint, 255 = none.
[[nodiscard]] std::shared_ptr<const BitmapTip> halfFrameTip(std::uint32_t w, std::uint32_t h) {
    TipFrame f;
    f.width = w;
    f.height = h;
    f.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint8_t grey = x < w / 2 ? 0 : 255;
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            f.rgba[i] = grey;
            f.rgba[i + 1] = grey;
            f.rgba[i + 2] = grey;
            f.rgba[i + 3] = 255;
        }
    return std::make_shared<BitmapTip>(std::vector<TipFrame>{f}, TipApplication::AlphaMask,
                                       TipSourceKind::Mask);
}

// An all-paint w x h bitmap masking tip, for cadence cases where only the FOOTPRINT matters.
[[nodiscard]] std::shared_ptr<const BitmapTip> solidFrameTip(std::uint32_t w, std::uint32_t h) {
    TipFrame f;
    f.width = w;
    f.height = h;
    f.rgba.resize(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::size_t i = 3; i < f.rgba.size(); i += 4)
        f.rgba[i] = 255;
    return std::make_shared<BitmapTip>(std::vector<TipFrame>{f}, TipApplication::AlphaMask,
                                       TipSourceKind::Mask);
}

} // namespace

TEST_CASE("a bitmap masking tip carves ITS shape, not an analytic disc") {
    // One primary press (hard 20 px black dab, coverage exactly 1 in the core) under one masking
    // dab whose 16x16 bitmap paints ONLY its left half, at subtract flow 1. The mask must carve
    // the paint back to the paper's own bytes on its painted side and leave the other side black
    // -- an asymmetry across the dab's centre that the null-tip analytic disc (which this routes
    // to if the tip is ignored) can never produce: a disc carves BOTH probes or neither.
    const Color8 paper{10, 200, 90, 255};
    BrushParams p = onePrimaryDab(1.0);
    p.masking.enabled = true;
    p.masking.op = MaskingOp::Subtract;
    p.masking.diameter = 16.0;
    p.masking.flow = 1.0;
    p.masking.spacing = 0.1;
    p.masking.tip = makeTip(halfFrameTip(16, 16));

    const Image img = paint(p, {StrokeInput{{48.0, 48.0}, 1.0}}, paper);
    CHECK(pixel(img, 44, 48) == paper);              // under the painted half: carved to the base
    CHECK(pixel(img, 52, 48) == Color8{0, 0, 0, 255}); // under the empty half: the paint stands
}

TEST_CASE("a procedural masking tip stamps its generator -- a rect reaches where no disc can") {
    // The masking tip is a hard 16x16 RECT generator. Its corner (42, 42) is 6*sqrt(2) ~ 8.49 px
    // from the dab's centre -- INSIDE the rect, but OUTSIDE the radius-8 disc the analytic
    // fallback would stamp at the same diameter. A build that quietly drops the procedural
    // masking tip leaves that corner black.
    const Color8 paper{10, 200, 90, 255};
    MaskGeneratorParams gen;
    gen.shape = MaskShape::Rect;
    gen.diameter = 16.0;
    gen.hFade = 1.0;
    gen.vFade = 1.0;

    BrushParams p = onePrimaryDab(1.0);
    p.diameter = 30.0; // the primary well past the mask's corners
    p.masking.enabled = true;
    p.masking.op = MaskingOp::Subtract;
    p.masking.diameter = 16.0;
    p.masking.flow = 1.0;
    p.masking.spacing = 0.1;
    p.masking.tip = makeTip(gen);

    const Image img = paint(p, {StrokeInput{{48.0, 48.0}, 1.0}}, paper);
    CHECK(pixel(img, 42, 42) == paper);              // the rect's corner: carved
    CHECK(pixel(img, 48, 48) == paper);              // its centre too
    CHECK(pixel(img, 48, 58) == Color8{0, 0, 0, 255}); // 10 px out: past the mask, still painted
}

TEST_CASE("the masking cadence reads the heading AT EACH DAB, not the span's first edge") {
    // ⚠ A STRAIGHT STROKE CANNOT SEE THIS: on a straight span the curve's tangent and the span's
    // first edge are bit-identical (the oldest trap in this arc), so the case above passes with
    // the per-dab heading update deleted. This one drives a BEND: three samples on an arc, so the
    // walked span turns from diagonal through horizontal to diagonal WITHIN the spans. The wide-
    // flat 64x8 masking tip must tighten its cadence as the path bends across its thin axis --
    // read the heading once per span and the step stays ~32 px through a bend that needs ~5, and
    // whole stretches of the stroke keep their paint.
    const Color8 paper{10, 200, 90, 255};
    const mosaic::common::Vec2 a{30.0, 80.0};
    const mosaic::common::Vec2 b{80.0, 30.0};
    const mosaic::common::Vec2 c{130.0, 80.0};

    BrushParams p = onePrimaryDab(1.0);
    p.diameter = 12.0;
    p.spacing = 0.1;
    p.masking.enabled = true;
    p.masking.op = MaskingOp::Subtract;
    p.masking.diameter = 64.0;
    p.masking.flow = 1.0;
    p.masking.spacing = 0.5;
    p.masking.tip = makeTip(solidFrameTip(64, 8));

    const auto lay = [&](const BrushParams& params) {
        Image img(160, 160);
        img.fill(paper);
        BrushEngine eng;
        eng.begin(160, 160, img, params, BrushDynamics{}, StrokeInput{a, 1.0});
        eng.extendTo(StrokeInput{b, 1.0});
        eng.extendTo(StrokeInput{c, 1.0});
        eng.flush();
        eng.composite();
        eng.end();
        return img;
    };

    // Probe along the path the ENGINE actually walks: the same spans, flattened at the same
    // tolerance (begin duplicates the first sample as its own predecessor; flush the last).
    std::vector<mosaic::common::Vec2> poly;
    std::vector<mosaic::common::Vec2> flat;
    mosaic::core::brush::flattenCatmullRom(a, a, b, c, 0.05, flat);
    poly.push_back(a);
    poly.insert(poly.end(), flat.begin(), flat.end());
    poly.push_back(b);
    mosaic::core::brush::flattenCatmullRom(a, b, c, c, 0.05, flat);
    poly.insert(poly.end(), flat.begin(), flat.end());
    poly.push_back(c);

    std::vector<std::pair<int, int>> probes; // every ~4 px of arc, clear of the two stroke caps
    double arc = 0.0;
    double next = 10.0;
    double total = 0.0;
    for (std::size_t i = 0; i + 1 < poly.size(); ++i)
        total += (poly[i + 1] - poly[i]).length();
    for (std::size_t i = 0; i + 1 < poly.size(); ++i) {
        const double el = (poly[i + 1] - poly[i]).length();
        while (next <= arc + el && next <= total - 10.0) {
            const double t = el > 0.0 ? (next - arc) / el : 0.0;
            const mosaic::common::Vec2 q = poly[i] + (poly[i + 1] - poly[i]) * t;
            probes.emplace_back(static_cast<int>(std::floor(q.x)),
                                static_cast<int>(std::floor(q.y)));
            next += 4.0;
        }
        arc += el;
    }
    REQUIRE(probes.size() > 20); // the premise of a premise: the arc is long enough to mean it

    // Premise: unmasked, every probe is solid paint -- a carved probe can only be the mask's work.
    BrushParams unmasked = p;
    unmasked.masking.enabled = false;
    const Image plain = lay(unmasked);
    for (const auto& [x, y] : probes)
        REQUIRE(pixel(plain, x, y) == Color8{0, 0, 0, 255});

    const Image img = lay(p);
    for (const auto& [x, y] : probes) {
        CAPTURE(x);
        CAPTURE(y);
        CHECK(pixel(img, x, y) == paper); // continuously carved, through both bends
    }
}

TEST_CASE("the masking cadence follows the masking tip's own extents, not one round diameter") {
    // The masking tip is a 64x8 all-paint bitmap -- the same wide-flat geometry as the shipped
    // eroded-debris masking tip (153x64: NOT square). Dragged DOWN its thin axis at spacing 0.5
    // it must step 4 px a dab (0.5 x its 8 px height), so consecutive 8 px-tall footprints
    // overlap and the subtract mask carves one continuous band out of the stroke. A cadence keyed
    // off the one 64 px diameter steps 32 px a dab and leaves 24 px of UNCARVED paint between
    // masking dabs -- which is exactly the five-times-too-sparse bug the primary walk had before
    // its spacing became an ellipse.
    const Color8 paper{10, 200, 90, 255};
    BrushParams p = onePrimaryDab(1.0);
    p.diameter = 12.0;
    p.spacing = 0.1;
    p.masking.enabled = true;
    p.masking.op = MaskingOp::Subtract;
    p.masking.diameter = 64.0;
    p.masking.flow = 1.0;
    p.masking.spacing = 0.5;
    p.masking.tip = makeTip(solidFrameTip(64, 8));

    const std::vector<StrokeInput> down{StrokeInput{{48.0, 20.0}, 1.0},
                                        StrokeInput{{48.0, 76.0}, 1.0}};

    // Premise first: with masking off, this stroke paints the probe rows solid black -- so a
    // carved probe below can only mean the mask reached it.
    BrushParams unmasked = p;
    unmasked.masking.enabled = false;
    const Image plain = paint(unmasked, down, paper);
    for (int y = 24; y <= 72; y += 4)
        REQUIRE(pixel(plain, 48, y) == Color8{0, 0, 0, 255});

    const Image img = paint(p, down, paper);
    for (int y = 24; y <= 72; y += 4) {
        CAPTURE(y);
        CHECK(pixel(img, 48, y) == paper); // continuously carved: no 32 px cadence gaps
    }
}
