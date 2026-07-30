#include <doctest/doctest.h>

#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/brush/brush_tip.hpp"
#include "core/brush/texture.hpp"
#include "io/brush/library.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// THE TEXTURE OPTION (docs/brushes.md §6.6h): the bake, the document-locked tiling, the 8-bit
// composite and the engine's stencil, plus the corpus's own 21 textured presets.
//
// ⚠ WHAT EACH CASE CAN SEE, before it was written -- this arc's standing discipline:
//   * The BAKE cases read the mask BYTES of a hand-built image, so they see every step of the
//     reference's chain (the luma weights, the alpha-over-white rule, the two adjustments, the
//     neutral-point split, the cutoff bands) and nothing else. They cannot see the tiling or the
//     composite: the mask is the same whatever a dab does with it.
//   * The TILING case reads `textureValueAt` directly, so it sees the modulo -- including the
//     negative-coordinate branch that a truncating `%` gets wrong and that NO stroke-level test can
//     reach without painting off the document's left edge.
//   * The COMPOSITE cases are hand-computed integers, so they see the truncation. A metric (a mean,
//     a coverage sum) could not: rounding instead of truncating moves single bytes, which every
//     aggregate absorbs.
//   * The ENGINE cases paint a REAL stroke against a TWIN with texturing off. That comparison is
//     the only thing that can see the two facts a mask test cannot: that the pattern is read at the
//     DOCUMENT pixel (a half-period shift must change the mark; a whole-period shift must not) and
//     that an inert texture leaves the stroke byte-identical.
namespace cb = mosaic::core::brush;

using mosaic::common::Color8;
using mosaic::common::Image;

namespace {

// One RGBA pixel run, for the hand-built bake inputs.
[[nodiscard]] std::vector<std::uint8_t> rgbaOf(std::initializer_list<Color8> pixels) {
    std::vector<std::uint8_t> out;
    out.reserve(pixels.size() * 4);
    for (const Color8& c : pixels) {
        out.push_back(c.r);
        out.push_back(c.g);
        out.push_back(c.b);
        out.push_back(c.a);
    }
    return out;
}

[[nodiscard]] std::shared_ptr<const cb::TexturePattern> bake(std::vector<std::uint8_t> rgba,
                                                             std::uint32_t w, std::uint32_t h,
                                                             cb::TextureBake b = {}) {
    return cb::bakeTexturePattern(rgba.data(), w, h, b);
}

// A pattern of alternating COLUMNS -- 255, 0, 255, 0 ... -- built directly rather than baked, so a
// stroke test asserts against a grain it chose rather than one an adjustment produced.
[[nodiscard]] std::shared_ptr<const cb::TexturePattern> stripes(int period) {
    auto p = std::make_shared<cb::TexturePattern>();
    p->width = static_cast<std::uint32_t>(period);
    p->height = 1;
    p->mask.resize(static_cast<std::size_t>(period));
    for (int i = 0; i < period; ++i)
        p->mask[static_cast<std::size_t>(i)] = i < period / 2 ? std::uint8_t{255} : std::uint8_t{0};
    return p;
}

// A 1 x 1 pattern of one value: the grain is then a pure scale, so a test can read the composite's
// result off a single pixel without any tiling in the way.
[[nodiscard]] std::shared_ptr<const cb::TexturePattern> uniformPattern(std::uint8_t value) {
    auto p = std::make_shared<cb::TexturePattern>();
    p->width = 1;
    p->height = 1;
    p->mask = {value};
    return p;
}

[[nodiscard]] cb::StrokeInput at(double x, double y, double pressure = 1.0) {
    cb::StrokeInput in;
    in.pos = {x, y};
    in.pressure = pressure;
    return in;
}

[[nodiscard]] cb::BrushParams roundBrush() {
    cb::BrushParams p;
    p.diameter = 12.0;
    p.spacing = 0.2;
    p.color = Color8{0, 0, 0, 255};
    cb::MaskGeneratorParams g;
    g.diameter = 12.0;
    g.hFade = 1.0;
    g.vFade = 1.0;
    g.antialiasEdges = true;
    p.tip = cb::makeTip(g); // a REAL tip: the texture composites into the tip's mask
    return p;
}

// Paint a short horizontal stroke and hand back the whole image.
[[nodiscard]] Image paintStroke(const cb::BrushParams& params) {
    Image img(64, 32);
    for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i] = 255;
        img.rgba[i + 1] = 255;
        img.rgba[i + 2] = 255;
        img.rgba[i + 3] = 255;
    }
    cb::BrushEngine eng;
    eng.begin(64, 32, img, params, cb::BrushDynamics{}, at(10.0, 16.0));
    eng.extendTo(at(20.0, 16.0));
    eng.extendTo(at(30.0, 16.0));
    eng.extendTo(at(40.0, 16.0));
    eng.end();
    eng.composite();
    return img;
}

const mosaic::io::brush::PresetLibrary& shipped() {
    static const mosaic::io::brush::PresetLibrary lib = [] {
        mosaic::io::brush::PresetLibrary l;
        std::string error;
        const int n = l.addBundleFile(
            std::string(MOSAIC_SHIPPED_DATA_DIR) + "/brushes/Krita_4_Default_Resources.bundle",
            &error);
        REQUIRE_MESSAGE(n == 117, error);
        return l;
    }();
    return lib;
}

} // namespace

TEST_CASE("texture bake: the luma weights and the alpha-over-white rule") {
    // A mid-grey is the identity through the whole default chain: grey 128 -> 0.50196 -> the
    // neutral point's UPPER segment (0.5 + (v - 0.5) / 1) -> 0.50196 -> 128.
    const auto grey = bake(rgbaOf({Color8{128, 128, 128, 255}}), 1, 1);
    REQUIRE(grey != nullptr);
    CHECK(static_cast<int>(grey->mask[0]) == 128);

    // ⚠ THE WEIGHTS ARE NOT EQUAL: (r*11 + g*16 + b*5) / 32, INTEGER. Pure green reads far brighter
    // than pure blue, and a mutant that averaged the channels would give 85 for both.
    const auto green = bake(rgbaOf({Color8{0, 255, 0, 255}}), 1, 1);
    const auto blue = bake(rgbaOf({Color8{0, 0, 255, 255}}), 1, 1);
    REQUIRE(green != nullptr);
    REQUIRE(blue != nullptr);
    CHECK(static_cast<int>(green->mask[0]) == 127); // (255*16)/32 = 127
    CHECK(static_cast<int>(blue->mask[0]) == 39);   // (255*5)/32  = 39

    // A fully TRANSPARENT pixel reads WHITE, not black: the reference composites the pattern over
    // white before measuring it, and white is the identity under Multiply. Dropping the `(1 - a)`
    // term would make every transparent pixel erase the stroke instead of leaving it alone.
    const auto clear = bake(rgbaOf({Color8{0, 0, 0, 0}}), 1, 1);
    REQUIRE(clear != nullptr);
    CHECK(static_cast<int>(clear->mask[0]) == 255);

    // Half-transparent black: 0 * (128/255) + (1 - 128/255) = 0.498 -> the neutral split's LOWER
    // segment (v / (2*0.5)) -> 127. The alpha WEIGHTS the grey; it does not gate it.
    const auto half = bake(rgbaOf({Color8{0, 0, 0, 128}}), 1, 1);
    REQUIRE(half != nullptr);
    CHECK(static_cast<int>(half->mask[0]) == 127);
}

TEST_CASE("texture bake: brightness, contrast, invert and the neutral-point split") {
    // Brightness is SUBTRACTED. A white pixel at brightness 0.5 lands at 0.5 -> 128.
    cb::TextureBake b;
    b.brightness = 0.5;
    const auto dimmed = bake(rgbaOf({Color8{255, 255, 255, 255}}), 1, 1, b);
    REQUIRE(dimmed != nullptr);
    CHECK(static_cast<int>(dimmed->mask[0]) == 128);

    // Contrast pivots on 0.5, NOT on 0: a black pixel at contrast 0.5 lands on 0.25, not on 0.
    // (A pivot-on-zero implementation would leave black black and be invisible here.)
    cb::TextureBake c;
    c.contrast = 0.5;
    const auto pulled = bake(rgbaOf({Color8{0, 0, 0, 255}}), 1, 1, c); // (0 - 0.5)*0.5 + 0.5 = 0.25
    REQUIRE(pulled != nullptr);
    CHECK(static_cast<int>(pulled->mask[0]) == 64);
    // ... and the same pixel at contrast 1 is still black, so the case above measured the contrast
    // rather than the neutral split.
    const auto plainBlack = bake(rgbaOf({Color8{0, 0, 0, 255}}), 1, 1);
    REQUIRE(plainBlack != nullptr);
    CHECK(static_cast<int>(plainBlack->mask[0]) == 0);

    // Invert turns the white paper black -- which is what makes five shipped presets carve their
    // grain out of the stroke rather than leave it.
    cb::TextureBake inv;
    inv.invert = true;
    const auto flipped = bake(rgbaOf({Color8{255, 255, 255, 255}}), 1, 1, inv);
    REQUIRE(flipped != nullptr);
    CHECK(static_cast<int>(flipped->mask[0]) == 0);

    // The NEUTRAL POINT is two straight segments, and that is the whole reason it exists: at 0.25 a
    // grey below the point is stretched over [0, 0.5] and one above it compressed into [0.5, 1],
    // with NEITHER half clipping. A single-segment implementation (v/neutral, clamped) would send
    // the 0.5 pixel to 1.0 instead of 0.667.
    cb::TextureBake n;
    n.neutralPoint = 0.25;
    const auto low = bake(rgbaOf({Color8{64, 64, 64, 255}}), 1, 1, n); // grey 64 -> 0.251 <= 0.25?
    const auto mid = bake(rgbaOf({Color8{128, 128, 128, 255}}), 1, 1, n); // 0.502 -> upper segment
    REQUIRE(low != nullptr);
    REQUIRE(mid != nullptr);
    CHECK(static_cast<int>(mid->mask[0]) == 170); // 0.5 + (0.502 - 0.25) / 1.5 = 0.668 -> 170
    CHECK(static_cast<int>(low->mask[0]) == 128); // 0.251 -> upper segment by a hair: 0.5007 -> 128
}

TEST_CASE("texture bake: the two cutoff policies band the mask from opposite sides") {
    // Policy 1 makes everything outside the band TRANSPARENT (0); policy 2 makes it OPAQUE (255).
    // The two share one condition and differ only in what they write, which is exactly the pair a
    // single-branch implementation collapses -- so both are pinned.
    const auto pixels = rgbaOf({Color8{16, 16, 16, 255}, Color8{128, 128, 128, 255},
                                Color8{240, 240, 240, 255}});
    cb::TextureBake cut1;
    cut1.cutoffPolicy = 1;
    cut1.cutoffLeft = 64;
    cut1.cutoffRight = 192;
    const auto banded = bake(pixels, 3, 1, cut1);
    REQUIRE(banded != nullptr);
    CHECK(static_cast<int>(banded->mask[0]) == 0);   // below the band
    CHECK(static_cast<int>(banded->mask[1]) == 128); // inside it, untouched
    CHECK(static_cast<int>(banded->mask[2]) == 0);   // above the band

    cb::TextureBake cut2 = cut1;
    cut2.cutoffPolicy = 2;
    const auto filled = bake(pixels, 3, 1, cut2);
    REQUIRE(filled != nullptr);
    CHECK(static_cast<int>(filled->mask[0]) == 255);
    CHECK(static_cast<int>(filled->mask[1]) == 128);
    CHECK(static_cast<int>(filled->mask[2]) == 255);

    // Policy 0 is not "a band of everything": it is no band at all, and a preset outside 1/2 must
    // fall through untouched.
    cb::TextureBake none = cut1;
    none.cutoffPolicy = 0;
    const auto plain = bake(pixels, 3, 1, none);
    REQUIRE(plain != nullptr);
    CHECK(static_cast<int>(plain->mask[0]) != 0);
}

TEST_CASE("texture bake: scale exactly 1 resamples NOTHING, and other scales change the size") {
    // ⚠ The reference's own `qFuzzyCompare` pair: at scale 1 (and at 0) it skips the resample
    // entirely. Four shipped presets author scale 1, and their mask is the image's own luminance --
    // so a resampler that ran anyway (a bilinear "identity" is not one at a half-pixel offset)
    // would move every one of their bytes.
    const auto pixels = rgbaOf({Color8{0, 0, 0, 255}, Color8{255, 255, 255, 255},
                                Color8{255, 255, 255, 255}, Color8{0, 0, 0, 255}});
    cb::TextureBake one;
    one.scale = 1.0;
    const auto exact = bake(pixels, 2, 2, one);
    REQUIRE(exact != nullptr);
    CHECK(exact->width == 2u);
    CHECK(exact->height == 2u);
    CHECK(static_cast<int>(exact->mask[0]) == 0);
    CHECK(static_cast<int>(exact->mask[1]) == 255);

    cb::TextureBake up;
    up.scale = 4.0;
    const auto bigger = bake(pixels, 2, 2, up);
    REQUIRE(bigger != nullptr);
    CHECK(bigger->width == 8u);
    CHECK(bigger->height == 8u);

    // A scale that would round the pattern away is floored at 2 x 2 rather than producing nothing.
    cb::TextureBake tiny;
    tiny.scale = 0.01;
    const auto floored = bake(pixels, 2, 2, tiny);
    REQUIRE(floored != nullptr);
    CHECK(floored->width == 2u);
    CHECK(floored->height == 2u);
}

TEST_CASE("texture tiling: the mask is read at the DOCUMENT pixel, and it wraps both ways") {
    auto p = std::make_shared<cb::TexturePattern>();
    p->width = 3;
    p->height = 2;
    p->mask = {10, 20, 30, 40, 50, 60};

    CHECK(static_cast<int>(cb::textureValueAt(*p, 0, 0, 0, 0)) == 10);
    CHECK(static_cast<int>(cb::textureValueAt(*p, 2, 1, 0, 0)) == 60);
    // Wrapping forward is the easy half.
    CHECK(static_cast<int>(cb::textureValueAt(*p, 3, 2, 0, 0)) == 10);
    // ⚠ AND BACKWARD. C's `%` truncates toward zero, so `-1 % 3` is -1 and a naive fetch indexes
    // out of the buffer (or, with an unsigned cast, into the far end of it). A stroke that crosses
    // x = 0 must see the grain continue, not fold.
    CHECK(static_cast<int>(cb::textureValueAt(*p, -1, -1, 0, 0)) == 60);
    CHECK(static_cast<int>(cb::textureValueAt(*p, -3, -2, 0, 0)) == 10);

    // The offsets SUBTRACT from the document coordinate.
    CHECK(static_cast<int>(cb::textureValueAt(*p, 1, 0, 1, 0)) == 10);
    CHECK(static_cast<int>(cb::textureValueAt(*p, 0, 1, 0, 1)) == 10);
}

TEST_CASE("texture composite: the reference's 8-bit arithmetic, truncation included") {
    using cb::textureComposite;
    using M = cb::TexturingMode;

    // Multiply, HARD (the default): mul(src, dst, strength) = (s*d*k) / (255*255), truncating.
    CHECK(static_cast<int>(textureComposite(M::Multiply, 255, 200, 255, false)) == 200);
    CHECK(static_cast<int>(textureComposite(M::Multiply, 128, 200, 255, false)) == 100); // 100.39
    CHECK(static_cast<int>(textureComposite(M::Multiply, 128, 200, 128, false)) == 50);  // 50.39
    CHECK(static_cast<int>(textureComposite(M::Multiply, 0, 200, 255, false)) == 0);
    // Strength 0 under the HARD form kills the dab entirely -- the strength scales the DAB there.
    CHECK(static_cast<int>(textureComposite(M::Multiply, 255, 200, 0, false)) == 0);

    // Multiply, SOFT: mul(union(src, inv(strength)), dst) -- the strength scales the PATTERN toward
    // white instead, so strength 0 leaves the dab alone. That inversion of meaning is the whole
    // point of the flag, and a soft/hard mix-up shows here and nowhere else.
    CHECK(static_cast<int>(textureComposite(M::Multiply, 255, 200, 0, true)) == 200);
    CHECK(static_cast<int>(textureComposite(M::Multiply, 128, 200, 128, true)) == 150);
    CHECK(static_cast<int>(textureComposite(M::Multiply, 128, 200, 255, true)) == 100);

    // Subtract, HARD: max(0, dst - (src + inv(strength))). At full strength it is a plain
    // difference; below it the INVERTED strength is added to the pattern, which is what makes a
    // weak subtract texture eat less rather than eat differently.
    CHECK(static_cast<int>(textureComposite(M::Subtract, 64, 200, 255, false)) == 136);
    CHECK(static_cast<int>(textureComposite(M::Subtract, 64, 200, 128, false)) == 9);
    CHECK(static_cast<int>(textureComposite(M::Subtract, 255, 200, 255, false)) == 0);

    // Subtract, SOFT: max(0, dst - mul(src, strength)).
    CHECK(static_cast<int>(textureComposite(M::Subtract, 64, 200, 128, true)) == 168);
    CHECK(static_cast<int>(textureComposite(M::Subtract, 64, 200, 255, true)) == 136);

    // The per-dab strength's 8-bit quantization is round-to-nearest, clamped.
    CHECK(cb::textureStrength8(1.0) == 255);
    CHECK(cb::textureStrength8(0.0) == 0);
    CHECK(cb::textureStrength8(0.5) == 128);
    CHECK(cb::textureStrength8(0.2) == 51);
    CHECK(cb::textureStrength8(-1.0) == 0);
    CHECK(cb::textureStrength8(2.0) == 255);
}

TEST_CASE("texture engine: an inert texture leaves the stroke BYTE-IDENTICAL") {
    // The hard rule (§6.2): an option that is not driving anything draws nothing and changes
    // nothing. Three ways to be inert -- not enabled, enabled with no pattern, and enabled on a
    // brush with no tip -- and all three must land on the untextured bytes.
    const cb::BrushParams plain = roundBrush();
    const Image reference = paintStroke(plain);

    cb::BrushParams off = plain;
    off.texture.pattern = stripes(4);
    off.texture.enabled = false;
    CHECK(paintStroke(off).rgba == reference.rgba);

    cb::BrushParams noPattern = plain;
    noPattern.texture.enabled = true;
    CHECK(paintStroke(noPattern).rgba == reference.rgba);

    // A tipless brush lays the engine's analytic circle, which is not a preset's dab: the gate
    // requires a real tip, exactly as hatching's does.
    cb::BrushParams noTip = plain;
    noTip.tip.reset();
    noTip.texture.enabled = true;
    noTip.texture.pattern = stripes(4);
    const Image tipless = paintStroke(noTip);
    cb::BrushParams noTipPlain = plain;
    noTipPlain.tip.reset();
    CHECK(tipless.rgba == paintStroke(noTipPlain).rgba);
}

TEST_CASE("texture engine: a full-strength multiply grain prints the pattern into the mark") {
    // A 4-wide stripe pattern: columns 0,1 are 255 (leave the dab alone) and 2,3 are 0 (erase it).
    // Under Multiply at full strength the result must be EXACTLY the untextured stroke on the first
    // pair and EXACTLY the untouched canvas on the second -- so this sees both that the composite
    // is a multiply and that the columns are DOCUMENT columns.
    cb::BrushParams p = roundBrush();
    const Image reference = paintStroke(p);
    p.texture.enabled = true;
    p.texture.mode = cb::TexturingMode::Multiply;
    p.texture.pattern = stripes(4);
    const Image textured = paintStroke(p);

    int keptPixels = 0;
    int clearedPixels = 0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * 64 + x) * 4;
            const bool grainOn = (x % 4) < 2;
            for (int c = 0; c < 4; ++c) {
                CAPTURE(x);
                CAPTURE(y);
                CAPTURE(c);
                if (grainOn)
                    CHECK(textured.rgba[i + c] == reference.rgba[i + c]);
                else
                    CHECK(textured.rgba[i + c] == 255); // the white canvas, verbatim
            }
            if (reference.rgba[i + 3] == 255 && reference.rgba[i] != 255) {
                if (grainOn)
                    ++keptPixels;
                else
                    ++clearedPixels;
            }
        }
    }
    // ... and the stroke really did cover both kinds of column, so the loop above asserted
    // something. (A stroke that missed every grain-off column would pass it vacuously.)
    CHECK(keptPixels > 50);
    CHECK(clearedPixels > 50);
}

TEST_CASE("texture engine: the grain is locked to the DOCUMENT, not to the dab") {
    // ⚠ THE PROPERTY THE WHOLE OPTION RESTS ON, and the only test that can see it. A stroke moved
    // by a WHOLE pattern period must lay the same mark translated; one moved by HALF a period must
    // NOT -- the grain slides under it. A dab-locked implementation passes the first half and fails
    // the second, which is exactly why the second half exists (the same shape as §6.6g's
    // phase-lock test for the hatching lattice).
    cb::BrushParams p = roundBrush();
    p.texture.enabled = true;
    p.texture.pattern = stripes(8); // 4 opaque columns, then 4 clear ones

    const auto paintAtX = [&](double x0) {
        Image img(96, 32);
        for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
            img.rgba[i] = img.rgba[i + 1] = img.rgba[i + 2] = img.rgba[i + 3] = 255;
        }
        cb::BrushEngine eng;
        eng.begin(96, 32, img, p, cb::BrushDynamics{}, at(x0, 16.0));
        eng.extendTo(at(x0 + 10.0, 16.0));
        eng.extendTo(at(x0 + 20.0, 16.0));
        eng.end();
        eng.composite();
        return img;
    };
    const Image base = paintAtX(20.0);
    const Image shiftedWhole = paintAtX(28.0); // one period
    const Image shiftedHalf = paintAtX(24.0);  // half a period

    const auto sameUnderShift = [&](const Image& other, int shift) {
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x + shift < 96; ++x) {
                const std::size_t a = (static_cast<std::size_t>(y) * 96 + x) * 4;
                const std::size_t b = (static_cast<std::size_t>(y) * 96 + (x + shift)) * 4;
                for (int c = 0; c < 4; ++c)
                    if (base.rgba[a + c] != other.rgba[b + c])
                        return false;
            }
        }
        return true;
    };
    CHECK(sameUnderShift(shiftedWhole, 8));
    CHECK_FALSE(sameUnderShift(shiftedHalf, 4));
}

TEST_CASE("texture engine: the offset slides the grain, and a random offset is per STROKE") {
    cb::BrushParams p = roundBrush();
    p.texture.enabled = true;
    p.texture.pattern = stripes(8);
    const Image plain = paintStroke(p);

    // A static offset of one whole period is the identity; half a period is not.
    cb::BrushParams whole = p;
    whole.texture.offsetX = 8;
    CHECK(paintStroke(whole).rgba == plain.rgba);
    cb::BrushParams half = p;
    half.texture.offsetX = 4;
    CHECK_FALSE(paintStroke(half).rgba == plain.rgba);

    // A RANDOM offset is drawn once per stroke from the stroke's SEED, so the same seed replays
    // exactly and a different one moves the grain. (It is drawn from `strokeRandom`, not from the
    // per-dab stream, which is why it cannot disturb any option's pinned draw order -- the mark
    // stays a function of the seed and the samples.)
    cb::BrushParams rnd = p;
    rnd.texture.randomOffsetX = true;
    rnd.seed = 1234;
    const Image a = paintStroke(rnd);
    CHECK(paintStroke(rnd).rgba == a.rgba); // same seed, same grain
    int differing = 0;
    for (std::uint64_t seed = 1; seed < 12; ++seed) {
        cb::BrushParams other = rnd;
        other.seed = seed;
        if (!(paintStroke(other).rgba == a.rgba))
            ++differing;
    }
    CHECK(differing > 0);
}

TEST_CASE("texture engine: the per-dab strength option scales the grain") {
    // The `Texture/Strength/` option rides the pipeline like every other one. At strength 0 the
    // HARD multiply kills the dab entirely (the strength scales the dab, per the composite above),
    // which is the coarsest visible consequence and the one a metric can see.
    cb::BrushParams p = roundBrush();
    p.texture.enabled = true;
    p.texture.pattern = stripes(2); // half the columns fully opaque

    const auto ink = [](const Image& img) {
        long long sum = 0;
        for (std::size_t i = 0; i < img.rgba.size(); i += 4)
            sum += 255 - img.rgba[i];
        return sum;
    };
    const long long full = ink(paintStroke(p));

    auto options = std::make_shared<cb::BrushOptions>();
    cb::CurveOptionData d;
    d.name = "Texture/Strength/";
    d.checked = true;
    d.strength = 0.0;
    d.sensors.sensors = {cb::Sensor::withDefaults(cb::SensorId::Pressure)};
    options->textureStrength.emplace(d);
    cb::BrushParams zero = p;
    zero.options = options;
    CHECK(ink(paintStroke(zero)) == 0);
    CHECK(full > 0);
}

TEST_CASE("texture engine: the grain composites AFTER the sharpness threshold") {
    // ⚠ THE ORDER IS THE REFERENCE'S POST-PROCESSING ORDER AND IT IS OBSERVABLE. A Sharpness at
    // full value collapses the mask to 1 bit; if the texture went first, its grey would be
    // thresholded straight back to 255 and vanish. Applied second, it scales the hardened mask, so
    // a uniform mid-grey pattern caps the mark's alpha at roughly that grey.
    cb::BrushParams p = roundBrush();
    // ⚠ A WIDE spacing, deliberately: at the default cadence the dabs overlap and the WASH
    // accumulation climbs toward 1 however weak each dab is, which would hide the grain's ceiling
    // behind the build-up. At 2.0 the dabs are 24 px apart on a 12 px tip and none touches another,
    // so each pixel's alpha is exactly one dab's.
    p.spacing = 2.0;
    p.texture.enabled = true;
    p.texture.pattern = uniformPattern(128);

    auto options = std::make_shared<cb::BrushOptions>();
    cb::CurveOptionData sharp;
    sharp.name = "Sharpness";
    sharp.checked = true;
    sharp.strength = 1.0;
    sharp.sensors.sensors = {cb::Sensor::withDefaults(cb::SensorId::Pressure)};
    options->sharpness.emplace(cb::SharpnessOption{cb::CurveOption(sharp), false, 0});
    p.options = options;

    const Image img = paintStroke(p);
    int darkest = 255;
    for (std::size_t i = 0; i < img.rgba.size(); i += 4)
        darkest = std::min(darkest, static_cast<int>(img.rgba[i]));
    // The stroke paints BLACK on WHITE, so "as dark as the grain allows" is 255 - 128 = 127-ish.
    // Texture-then-threshold would have driven it to 0 (a solid black 1-bit mark).
    CHECK(darkest > 100);
    CHECK(darkest < 160);

    // ... and without the texture the same sharpened stroke really does reach solid black, so the
    // bound above is measuring the grain and not the tip.
    cb::BrushParams bare = p;
    bare.texture.enabled = false;
    const Image plain = paintStroke(bare);
    int plainDarkest = 255;
    for (std::size_t i = 0; i < plain.rgba.size(); i += 4)
        plainDarkest = std::min(plainDarkest, static_cast<int>(plain.rgba[i]));
    CHECK(plainDarkest == 0);
}

TEST_CASE("texture engine: the grain rides the SMUDGE walk too") {
    // ⚠ The opposite of Mirror and Sharpness, and the reason is structural: the reference installs
    // its texture option on the brush-BASED paintop base that colorsmudge derives from, so the
    // smudge dab's mask goes through the very same post-processing step. Five shipped presets
    // depend on it. A fully BLACK pattern under Multiply zeroes every mask pixel, so a textured
    // smudge stroke must leave the canvas untouched where an untextured one smears it.
    const auto paintSmudge = [](bool textured, std::uint8_t grain) {
        Image img(64, 32);
        for (std::uint32_t y = 0; y < 32; ++y) {
            for (std::uint32_t x = 0; x < 64; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * 64 + x) * 4;
                const std::uint8_t v = x < 32 ? std::uint8_t{0} : std::uint8_t{255};
                img.rgba[i] = img.rgba[i + 1] = img.rgba[i + 2] = v;
                img.rgba[i + 3] = 255;
            }
        }
        cb::BrushParams p = roundBrush();
        p.smudge.enabled = true;
        p.texture.enabled = textured;
        p.texture.pattern = uniformPattern(grain);
        cb::BrushEngine eng;
        eng.begin(64, 32, img, p, cb::BrushDynamics{}, at(24.0, 16.0));
        eng.extendTo(at(34.0, 16.0));
        eng.extendTo(at(44.0, 16.0));
        eng.end();
        eng.composite();
        return img;
    };
    const Image smeared = paintSmudge(false, 255);
    const Image blocked = paintSmudge(true, 0);
    const Image passed = paintSmudge(true, 255);

    // A black grain blocks the smear entirely: the canvas is the two flat halves it started as.
    for (std::uint32_t y = 0; y < 32; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * 64 + x) * 4;
            CAPTURE(x);
            CAPTURE(y);
            CHECK(blocked.rgba[i] == (x < 32 ? 0 : 255));
        }
    }
    // ... and a WHITE grain is the identity under Multiply at full strength, so it must be exactly
    // the untextured smear. (Together these two say the composite really runs on this walk and
    // really is a multiply, which neither says alone.)
    CHECK(passed.rgba == smeared.rgba);
    CHECK_FALSE(blocked.rgba == smeared.rgba);
}

TEST_CASE("texture corpus: all 21 shipped patterns decode, bake and reach the engine") {
    const mosaic::io::brush::PresetLibrary& lib = shipped();
    int textured = 0;
    for (const mosaic::io::brush::LibraryPreset& p : lib.presets()) {
        if (!p.preset.texture.enabled)
            continue;
        ++textured;
        CAPTURE(p.preset.name);
        REQUIRE(p.texture.enabled);
        REQUIRE(p.texture.pattern != nullptr);
        CHECK_FALSE(p.texture.pattern->empty());
    }
    CHECK(textured == 21);
    CHECK(lib.counters().texturesResolved == 21);
    CHECK(lib.counters().texturesFallback == 0);

    // Two spot checks against the RAW files, hand-derived rather than echoed: the dithering
    // preset's pattern is an 8 x 8 PNG at scale 1 (so no resample at all), and the screentone's is
    // a 25 x 25 PNG at scale 0.35 -> round(8.75) = 9.
    const auto find = [&](std::string_view name) -> const mosaic::io::brush::LibraryPreset* {
        for (const mosaic::io::brush::LibraryPreset& p : lib.presets())
            if (p.preset.name == name)
                return &p;
        return nullptr;
    };
    const mosaic::io::brush::LibraryPreset* dither = find("u)_Pixel_Art_Dithering");
    REQUIRE(dither != nullptr);
    CHECK(dither->texture.pattern->width == 8u);
    CHECK(dither->texture.pattern->height == 8u);
    CHECK(dither->texture.mode == cb::TexturingMode::Subtract); // TexturingMode = 1

    const mosaic::io::brush::LibraryPreset* screen = find("y)_Screentone_Pressure");
    REQUIRE(screen != nullptr);
    CHECK(screen->texture.pattern->width == 9u);
    CHECK(screen->texture.pattern->height == 9u);

    // ⚠ The five presets that INVERT their grain, and the one that authors a cutoff band, are the
    // ones whose bake is not the identity -- read back through the whole chain rather than from the
    // reader's defaults.
    const mosaic::io::brush::LibraryPreset* charcoal = find("h)_Charcoal_Pencil_Thin");
    REQUIRE(charcoal != nullptr);
    CHECK(charcoal->preset.texture.bake.invert);
    CHECK(charcoal->preset.texture.randomOffsetX);
    CHECK(charcoal->preset.texture.mode == cb::TexturingMode::Multiply);

    const mosaic::io::brush::LibraryPreset* wood = find("y)_Texture_Wood_Fiber");
    REQUIRE(wood != nullptr);
    CHECK(wood->preset.texture.bake.cutoffPolicy == 2);
    CHECK(wood->preset.texture.bake.cutoffLeft == 70);
}
