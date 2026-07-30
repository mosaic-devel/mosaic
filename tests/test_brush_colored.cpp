#include "common/image.hpp"
#include "core/blend_mode.hpp"
#include "core/brush/bitmap_tip.hpp" // TipApplication's enumerators (forward-declared by the engine)
#include "core/brush/brush_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <memory>
#include <vector>

// The colour axis added in S19 step 5b (docs/brushes.md §6.1): `StrokeAccumulator {Uniform,
// Colored}`. `Colored` accumulates each dab's colour premultiplied by the dab's alpha beside the
// coverage, and composite() normalizes that accumulation back to a per-pixel colour -- which is
// what makes per-dab colour (colour dynamics, image-stamp tips) expressible at all. The per-dab
// colour enters either through the engine's own HSV colour dynamics (§6.6f, applyColorDynamics --
// exercised at the bottom of this file) or, for third-party colour packs and RGBA hoses, through
// the host `BrushDynamics::dabColor` seam.
//
// `Uniform` -- the auto-selected fast path -- is pinned byte-for-byte next door in
// test_brush_wash_golden.cpp; nothing here re-asserts it. The exact pins below are hand-derived
// (every quantity dyadic unless the comment derives its rounding), per the standing rule that
// recorded expectations only prove the code agrees with itself.
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::core::BlendMode;
using mosaic::core::brush::BrushDynamics;
using mosaic::core::brush::BrushEngine;
using mosaic::core::brush::BrushParams;
using mosaic::core::brush::chooseAccumulator;
using mosaic::core::brush::PaintMode;
using mosaic::core::brush::StrokeAccumulator;
using mosaic::core::brush::StrokeMode;
using mosaic::core::brush::StrokeInput;
using mosaic::core::brush::TipApplication;

Color8 pixel(const Image& img, int x, int y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

// A stroke that crosses itself at (48,48) -- the same path the accumulator tests use, so overlap
// (not pressure) drives the accumulation.
const std::vector<StrokeInput>& crossingPath() {
    static const std::vector<StrokeInput> path{
        StrokeInput{{20.0, 20.0}, 1.0}, StrokeInput{{76.0, 76.0}, 1.0},
        StrokeInput{{76.0, 20.0}, 1.0}, StrokeInput{{20.0, 76.0}, 1.0}};
    return path;
}

Image paint(BrushParams p, const BrushDynamics& d, const std::vector<StrokeInput>& path,
            Color8 fill) {
    Image img(96, 96);
    img.fill(fill);
    BrushEngine eng;
    eng.begin(96, 96, img, p, d, path.front());
    for (std::size_t i = 1; i < path.size(); ++i)
        eng.extendTo(path[i]);
    eng.flush(); // the walk lags one sample; lay the tail span before reading the pixels
    eng.composite();
    eng.end();
    return img;
}

BrushParams baseColored() {
    BrushParams p;
    p.diameter = 18.0;
    p.hardness = 0.9;
    p.flow = 1.0;
    p.opacity = 0.5;
    p.spacing = 0.05;
    p.color = Color8{0, 0, 0, 255};
    p.accumulator = StrokeAccumulator::Colored;
    return p;
}

// Exactly two dabs, both solid over pixel (48,48), with distinct colours and distinct alphas --
// every quantity dyadic so the premultiplied stacking is exact and derivable by hand:
//
//   spacingPx = 0.1 * 20 = 2.0 and the segment is 2.0 long -> exactly one dab beyond the press.
//   hardness 1, radius 10 -> coverage 1.0 at the pixel for both dabs.
//   `flowFromPressure` with end pressure 0.5 -> dab 0 lands at alpha 1.0, dab 1 at alpha 0.5.
//   The colour hook paints dab 0 RED and dab 1 GREEN, so the green must stack OVER the red:
//
//   Wash weights (da = a):            after dab 0: P = (1, 0, 0 | 1)
//     P = C*da + P*(1-da)             after dab 1: P = (0.5, 0.5, 0 | 1)     -> colour (1/2, 1/2, 0)
//   Buildup weights (da = a*cap=0.5): after dab 0: P = (0.5, 0, 0 | 0.5)
//     cap = opacity 0.5 * color.a 1   after dab 1: P = (0.375, 0.25, 0 | 0.625) -> (0.6, 0.4, 0)
//
// A stacking-order flip is caught two-sidedly: red-over-green washes to (1, 0, 0), not (1/2, 1/2, 0).
Image twoColorDabs(BrushParams p, Color8 fill) {
    p.diameter = 20.0;
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 0.5;
    p.spacing = 0.1;

    BrushDynamics d;
    d.flowFromPressure = true;
    d.dabColor = [](std::size_t dab, double) {
        return dab == 0 ? Color8{255, 0, 0, 255} : Color8{0, 255, 0, 255};
    };

    Image img(96, 96);
    img.fill(fill);
    BrushEngine eng;
    eng.begin(96, 96, img, p, d, StrokeInput{{48.0, 48.0}, 1.0});
    eng.extendTo(StrokeInput{{50.0, 48.0}, 0.5});
    eng.flush();
    eng.composite();
    eng.end();
    return img;
}

} // namespace

TEST_CASE("chooseAccumulator picks Uniform only for a plain alpha-mask preset") {
    // §6.1's rule, both halves: any tip application that stamps colour forces Colored, and so does
    // an active colour-dynamics option on a plain mask. Only the mask-without-dynamics preset --
    // which is every preset in the shipped default set -- earns the fast path.
    CHECK(chooseAccumulator(TipApplication::AlphaMask, false) == StrokeAccumulator::Uniform);
    CHECK(chooseAccumulator(TipApplication::AlphaMask, true) == StrokeAccumulator::Colored);
    CHECK(chooseAccumulator(TipApplication::ImageStamp, false) == StrokeAccumulator::Colored);
    CHECK(chooseAccumulator(TipApplication::LightnessMap, false) == StrokeAccumulator::Colored);
    CHECK(chooseAccumulator(TipApplication::GradientMap, false) == StrokeAccumulator::Colored);
}

TEST_CASE("two dabs of different colours pin the premultiplied Wash stacking exactly") {
    // Derivation in twoColorDabs(). Composite over a transparent base at cap 0.5:
    //   sa = coverage(1.0) * 0.5, colour = (1/2, 1/2, 0) -> lround(127.5) = 128 per lit channel.
    const Image w = twoColorDabs(baseColored(), Color8{0, 0, 0, 0});
    CHECK(pixel(w, 48, 48) == Color8{128, 128, 0, 128});
}

TEST_CASE("Buildup weighs each dab's colour by the same capped alpha it composites at") {
    // Derivation in twoColorDabs(): the Buildup deposit weight is a*cap -- matching m_build, NOT
    // the Wash weight a -- so the normalized colour is (0.375/0.625, 0.25/0.625, 0) = (0.6, 0.4, 0)
    // and sa = m_build = 0.625. The two quotients are within 1e-15 of 0.6 and 0.4 (their doubles
    // are not exact, but 153.0 and 102.0 are ~1e-13 from the nearest lround boundary):
    //   alpha = lround(0.625 * 255) = lround(159.375) = 159
    //   r = lround(0.6 * 255) = 153,  g = lround(0.4 * 255) = 102.
    // Two mutations this catches, both of which the Wash pin above cannot: depositing colour at
    // the UNCAPPED alpha gives (0.5, 0.5, 0) -> r = 128; normalizing by the coverage (1.0, not
    // 0.625) gives r = lround(0.375*255) = 96.
    BrushParams p = baseColored();
    p.paintMode = PaintMode::Buildup;
    const Image b = twoColorDabs(p, Color8{0, 0, 0, 0});
    CHECK(pixel(b, 48, 48) == Color8{153, 102, 0, 159});
}

TEST_CASE("a Colored stroke of one pure colour is byte-identical to Uniform") {
    // Every channel of the constant colour is 0 or 255, so each premultiplied row is either
    // exactly 0 or exactly the alpha row (da*1 == da, da*0 == 0 in floats), and the per-channel
    // division x/x == 1.0 exactly (which is why composite() divides instead of multiplying by a
    // reciprocal). The normalized colour is then bit-for-bit the params colour, and every
    // downstream expression is the pinned Uniform arithmetic. This holds across paint modes and
    // blend modes, over any base -- so the colour axis costs the Uniform semantics nothing.
    for (const PaintMode mode : {PaintMode::Wash, PaintMode::Buildup}) {
        for (const Color8 fill : {Color8{40, 90, 160, 200}, Color8{0, 0, 0, 0}}) {
            BrushParams uni = baseColored();
            uni.accumulator = StrokeAccumulator::Uniform;
            uni.color = Color8{255, 0, 0, 255};
            uni.paintMode = mode;
            BrushParams col = uni;
            col.accumulator = StrokeAccumulator::Colored;

            const Image u = paint(uni, BrushDynamics{}, crossingPath(), fill);
            const Image c = paint(col, BrushDynamics{}, crossingPath(), fill);
            CHECK(u.rgba == c.rgba);
        }
    }
    // ... including through the blend path (cs is cast from the same double).
    BrushParams uni = baseColored();
    uni.accumulator = StrokeAccumulator::Uniform;
    uni.color = Color8{0, 255, 255, 255};
    uni.blendMode = BlendMode::Multiply;
    BrushParams col = uni;
    col.accumulator = StrokeAccumulator::Colored;
    const Image u = paint(uni, BrushDynamics{}, crossingPath(), Color8{200, 60, 60, 255});
    const Image c = paint(col, BrushDynamics{}, crossingPath(), Color8{200, 60, 60, 255});
    CHECK(u.rgba == c.rgba);
}

TEST_CASE("a Colored stroke of one arbitrary colour stays within one level of Uniform") {
    // For channels that are neither 0 nor 255 the premultiplied row and the alpha row round at
    // different moments, so normalization recovers the colour to within a few ulps rather than
    // exactly -- at most one 8-bit level after quantization. More than that would mean the
    // accumulation is wrong, not merely rounded.
    for (const Color8 colour : {Color8{200, 100, 50, 255}, Color8{13, 240, 7, 255}}) {
        for (const double opacity : {0.5, 0.123456}) {
            BrushParams uni = baseColored();
            uni.accumulator = StrokeAccumulator::Uniform;
            uni.color = colour;
            uni.opacity = opacity;
            BrushParams col = uni;
            col.accumulator = StrokeAccumulator::Colored;

            const Image u = paint(uni, BrushDynamics{}, crossingPath(), Color8{30, 40, 50, 255});
            const Image c = paint(col, BrushDynamics{}, crossingPath(), Color8{30, 40, 50, 255});
            REQUIRE(u.rgba.size() == c.rgba.size());
            int maxDelta = 0;
            for (std::size_t i = 0; i < u.rgba.size(); ++i)
                maxDelta = std::max(maxDelta, std::abs(int(u.rgba[i]) - int(c.rgba[i])));
            INFO("colour=(" << int(colour.r) << "," << int(colour.g) << "," << int(colour.b)
                            << ") opacity=" << opacity);
            CHECK(maxDelta <= 1);
        }
    }
}

TEST_CASE("the blend mode sees the per-pixel colour, not the stroke colour") {
    // The two-colour construction under Multiply over an opaque white base. The normalized paint
    // colour at (48,48) is (1/2, 1/2, 0); Multiply against white returns it unchanged and the
    // blue channel becomes 0, all in exact float arithmetic:
    //   sa = 0.5, oa = 1:  r = g = 0.5*0.5 + 1*0.5 = 0.75 -> lround(191.25) = 191
    //                      b =   0*0.5 + 1*0.5 = 0.5  -> lround(127.5)  = 128.
    // The params colour is left black, so an implementation that fed the blend the stroke colour
    // would produce 0.0*white = black limbs instead.
    BrushParams p = baseColored();
    p.blendMode = BlendMode::Multiply;
    const Image m = twoColorDabs(p, Color8{255, 255, 255, 255});
    CHECK(pixel(m, 48, 48) == Color8{191, 191, 128, 255});
}

TEST_CASE("Erase ignores the accumulator and the colour hook entirely") {
    // Destination-out reads no colour, so `Colored` on an erase stroke must change nothing --
    // neither the carved alpha nor a single byte elsewhere. The hook feeding it loud colours is
    // the point: they must go nowhere.
    BrushParams uni = baseColored();
    uni.accumulator = StrokeAccumulator::Uniform;
    uni.strokeMode = StrokeMode::Erase;
    BrushParams col = uni;
    col.accumulator = StrokeAccumulator::Colored;
    BrushDynamics hooked;
    hooked.dabColor = [](std::size_t dab, double) {
        return dab % 2 == 0 ? Color8{255, 0, 255, 255} : Color8{0, 255, 0, 255};
    };

    const Image u = paint(uni, BrushDynamics{}, crossingPath(), Color8{10, 200, 90, 255});
    const Image c = paint(col, hooked, crossingPath(), Color8{10, 200, 90, 255});
    CHECK(u.rgba == c.rgba);
    CHECK(pixel(c, 48, 48).a < 255); // it really erased something
}

TEST_CASE("the colour hook is consulted only by the Colored accumulator") {
    // On Uniform the hook must be dead weight -- not applied, not sampled into anything. A caller
    // who wants per-dab colour asks for the accumulator that can express it.
    BrushParams p = baseColored();
    p.accumulator = StrokeAccumulator::Uniform;
    BrushDynamics hooked;
    hooked.dabColor = [](std::size_t, double) { return Color8{255, 255, 0, 255}; };

    const Image plain = paint(p, BrushDynamics{}, crossingPath(), Color8{200, 200, 200, 255});
    const Image withHook = paint(p, hooked, crossingPath(), Color8{200, 200, 200, 255});
    CHECK(plain.rgba == withHook.rgba);
}

TEST_CASE("a dab colour's alpha does not weaken the stroke") {
    // BrushDynamics::dabColor documents its returned alpha as ignored: the stroke's ceiling froze
    // at begin() from the params colour, and a per-dab alpha would have to leak into the coverage
    // buffer, which stays the Inpaint brush's plain geometric mask.
    BrushParams p = baseColored();
    BrushDynamics opaque;
    opaque.dabColor = [](std::size_t, double) { return Color8{255, 0, 0, 255}; };
    BrushDynamics thin;
    thin.dabColor = [](std::size_t, double) { return Color8{255, 0, 0, 7}; };

    const Image a = paint(p, opaque, crossingPath(), Color8{0, 0, 0, 0});
    const Image b = paint(p, thin, crossingPath(), Color8{0, 0, 0, 0});
    CHECK(a.rgba == b.rgba);
    CHECK(pixel(a, 48, 48).a == 128); // the ceiling is still the params opacity 0.5
}

TEST_CASE("the accumulator does not touch the coverage buffer the Inpaint brush reads") {
    BrushParams uni = baseColored();
    uni.accumulator = StrokeAccumulator::Uniform;
    BrushParams col = baseColored();
    BrushDynamics hooked;
    hooked.dabColor = [](std::size_t dab, double) {
        return dab % 2 == 0 ? Color8{255, 0, 0, 255} : Color8{0, 0, 255, 255};
    };

    const auto run = [](const BrushParams& p, const BrushDynamics& d) {
        Image img(96, 96);
        BrushEngine eng;
        eng.begin(96, 96, img, p, d, crossingPath().front());
        for (std::size_t i = 1; i < crossingPath().size(); ++i)
            eng.extendTo(crossingPath()[i]);
        std::vector<float> cov = eng.coverage();
        eng.end();
        return cov;
    };
    const std::vector<float> cu = run(uni, BrushDynamics{});
    const std::vector<float> cc = run(col, hooked);
    REQUIRE(!cu.empty());
    CHECK(cu == cc);
}

TEST_CASE("the document size does not change any pixel a Colored stroke paints") {
    // The colour accumulation is the FOURTH per-pixel buffer ensureCovers() re-homes (coverage,
    // base snapshot, Buildup, colour) -- Buildup x Colored here exercises all four at once. As
    // with the Buildup variant next door: growth alone cannot expose a dropped buffer, because
    // composite() only rewrites the pending region; the stroke must come BACK over an early pixel
    // after the excursion that grew the rect. Identical stroke, 200x200 (never grows: the first
    // dab clamps the rect to the whole layer) vs 1024x1024 (grows past the first 256 px tile) --
    // every shared pixel must match.
    //
    // The hook derives each channel arithmetically from the dab INDEX -- deliberately NOT a
    // cycling palette: a hook with period k is blind to any index shift that is a multiple of k,
    // and the first version of this test (period 3) survived exactly that mutation. This one also
    // pins the index rule: the index advances for dabs clipped off the 200x200 document (the
    // excursion to (800,700)), so both documents see the same colour sequence on the shared
    // pixels. An index that counted only deposited dabs recolours the small document's return
    // pass, by a different amount at every index, and fails here.
    BrushParams p = baseColored();
    p.paintMode = PaintMode::Buildup;
    p.diameter = 24.0;
    p.spacing = 1.0;  // sparse dabs, far from saturating
    p.opacity = 0.15; // each pass adds a visible share
    p.hardness = 1.0;
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

    REQUIRE(smallCw == 200u); // clamped to the layer on the first dab: this run never grows
    REQUIRE(bigCw > 256u);    // ... and this one did grow, past its first 256 px tile

    // The early pixel accumulated across the excursion AND its colour axis engaged -- a grey here
    // would mean every dab deposited the same colour and the hook never fired.
    const Color8 early = pixel(big, 90, 60);
    REQUIRE(early.a == 255);
    REQUIRE(early.r < 255);
    REQUIRE(!(early.r == early.g && early.g == early.b));

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

TEST_CASE("restore reverts a Colored stroke bytes-exactly") {
    // restore() reads only the coverage and the base snapshot; the colour axis must be invisible
    // to it, like the other three composite switches.
    BrushParams p = baseColored();
    p.paintMode = PaintMode::Buildup;
    p.blendMode = BlendMode::Multiply;
    BrushDynamics d;
    d.dabColor = [](std::size_t dab, double) {
        return dab % 2 == 0 ? Color8{250, 120, 30, 255} : Color8{30, 120, 250, 255};
    };

    Image img(96, 96);
    img.fill(Color8{33, 44, 55, 200});
    const Image pristine = img;

    BrushEngine eng;
    eng.begin(96, 96, img, p, d, crossingPath().front());
    for (std::size_t i = 1; i < crossingPath().size(); ++i) {
        eng.extendTo(crossingPath()[i]);
        eng.composite(); // frame by frame, as the canvas does
    }
    REQUIRE(img.rgba != pristine.rgba); // it really painted
    eng.restore();
    eng.end();
    CHECK(img.rgba == pristine.rgba);
}

// ------------------------------------------------------------------------------------------------
// Colour dynamics (§6.6f): the HSV options adjust the Colored accumulator's per-dab colour,
// end-to-end -- begin()'s gate, resolveDab (applyColorDynamics), beginDeposit, composite.

namespace {
using mosaic::core::brush::BrushOptions;
using mosaic::core::brush::CurveOptionData;
using mosaic::core::brush::Sensor;
using mosaic::core::brush::SensorId;

// A preset carrying a single `value` colour-dynamics option on a Pressure sensor at strength 1.
BrushParams coloredWithValue() {
    BrushParams p = baseColored();
    p.color = Color8{255, 0, 0, 255}; // pure red -- green and blue are 0 unless value lifts them
    p.opacity = 1.0;
    p.colorDynamicsActive = true;

    CurveOptionData vd;
    vd.name = "v";
    vd.checked = true;
    vd.strength = 1.0;
    vd.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
    auto opts = std::make_shared<BrushOptions>();
    opts->value.emplace(vd);
    p.options = opts;
    return p;
}
} // namespace

TEST_CASE("colour dynamics: a value option lightens the whole Colored stroke, end-to-end") {
    // The crossing path holds pressure 1.0, so every dab's value adjustment is dv = 2*1 - 1 = +1 --
    // the reference's push to white -- and the deposited colour is white, not the flat red. Over a
    // black ground the centre pixel's green and blue rise off zero, which pure red never does.
    const Image withDyn = paint(coloredWithValue(), BrushDynamics{}, crossingPath(),
                                Color8{0, 0, 0, 255});
    const Color8 c = pixel(withDyn, 48, 48);
    CHECK(c.r > 0);
    CHECK(c.g > 0);
    CHECK(c.b > 0);
    CHECK(c.g == c.r); // white is neutral: the three channels lift together
    CHECK(c.b == c.r);

    // Control: the SAME preset with colour dynamics OFF stays pure red -- green and blue never leave
    // zero. This is the two-sided pin that the value option, not some other path, did the lifting.
    BrushParams flat = coloredWithValue();
    flat.colorDynamicsActive = false;
    const Image noDyn = paint(flat, BrushDynamics{}, crossingPath(), Color8{0, 0, 0, 255});
    const Color8 f = pixel(noDyn, 48, 48);
    CHECK(f.r > 0);
    CHECK(f.g == 0);
    CHECK(f.b == 0);
}

TEST_CASE("colour dynamics: the gate needs an actually-checked h/s/v option, not just the flag") {
    // `colorDynamicsActive` set but with NO options table (or an unchecked channel) must leave the
    // flat colour: begin()'s gate requires a checked h/s/v, so a Colored-because-of-a-colour-tip
    // preset does not accidentally route through applyColorDynamics.
    BrushParams noOpt = baseColored();
    noOpt.color = Color8{255, 0, 0, 255};
    noOpt.opacity = 1.0;
    noOpt.colorDynamicsActive = true; // set, but options == nullptr -> gate stays shut
    const Color8 a = pixel(paint(noOpt, BrushDynamics{}, crossingPath(), Color8{0, 0, 0, 255}),
                           48, 48);
    CHECK(a.r > 0);
    CHECK(a.g == 0);
    CHECK(a.b == 0);

    // An unchecked value option is the same: present, but the gate reads it as off.
    BrushParams unchecked = coloredWithValue();
    {
        CurveOptionData vd;
        vd.name = "v";
        vd.checked = false; // <- the difference
        vd.strength = 1.0;
        vd.sensors.sensors = {Sensor::withDefaults(SensorId::Pressure)};
        auto opts = std::make_shared<BrushOptions>();
        opts->value.emplace(vd);
        unchecked.options = opts;
    }
    const Color8 b = pixel(paint(unchecked, BrushDynamics{}, crossingPath(), Color8{0, 0, 0, 255}),
                           48, 48);
    CHECK(b.g == 0);
    CHECK(b.b == 0);
}
