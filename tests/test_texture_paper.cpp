#include "core/texture/noise.hpp"
#include "core/texture/paper_render.hpp"
#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// S55-d: the paper / material subsystem (docs/texture-generator.md §5) -- the Gabor fibre
// primitive, the laid/chain/wove/felt height-field structure, the Oren-Nayar raked shade, the
// deckle-edge alpha carry and the preset library. Property tests only; the byte-golden for the
// default render lives with the other generators in test_texture_layer.cpp.
namespace {

using namespace mosaic;
namespace texture = core::texture;

double meanLuma(const common::Image& img) {
    double sum = 0.0;
    const std::size_t n = static_cast<std::size_t>(img.width) * img.height;
    for (std::size_t i = 0; i < n; ++i) {
        sum += 0.2126 * img.rgba[i * 4 + 0] + 0.7152 * img.rgba[i * 4 + 1] +
               0.0722 * img.rgba[i * 4 + 2];
    }
    return sum / n;
}

// Variance of the per-column (axis=0) or per-row (axis=1) mean-luma profile -- a directional-
// structure probe: a ruled/streaked field has a large profile variance on the axis PERPENDICULAR
// to its lines and a small one along them; an isotropic field is low on both.
double profileVariance(const common::Image& img, int axis) {
    const std::uint32_t n = axis == 0 ? img.width : img.height;
    const std::uint32_t m = axis == 0 ? img.height : img.width;
    std::vector<double> prof(n, 0.0);
    for (std::uint32_t y = 0; y < img.height; ++y)
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * img.width + x) * 4;
            const double luma =
                0.2126 * img.rgba[o] + 0.7152 * img.rgba[o + 1] + 0.0722 * img.rgba[o + 2];
            prof[axis == 0 ? x : y] += luma;
        }
    double mean = 0.0;
    for (double& c : prof) {
        c /= m;
        mean += c;
    }
    mean /= n;
    double var = 0.0;
    for (double c : prof) var += (c - mean) * (c - mean);
    return var / n;
}

// The strongest directional profile variance over both axes.
double maxProfileVariance(const common::Image& img) {
    return std::max(profileVariance(img, 0), profileVariance(img, 1));
}

texture::TextureParams paperParams(const texture::PaperParams& pp, std::uint64_t seed = 7) {
    texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Paper);
    p.seed = seed;
    p.spec = pp;
    return p;
}

}  // namespace

// ---- the Gabor fibre primitive ---------------------------------------------------------------

TEST_CASE("gabor2 is deterministic and orientation-selective") {
    // Pure function of (seed, coords, params): identical calls give identical bits.
    CHECK(texture::gabor2(42, 3.5, 1.25, 0.6, 0.7, 1.0) ==
          texture::gabor2(42, 3.5, 1.25, 0.6, 0.7, 1.0));
    // A coherent (anisotropy 1) carrier makes a directional field: sampling ACROSS the carrier
    // (perpendicular to the stripes) swings more than sampling ALONG it. omega = 0 -> the carrier
    // varies along +x, so stripes run along y; stepping in x crosses them.
    double varAcross = 0.0, varAlong = 0.0;
    double mAcross = 0.0, mAlong = 0.0;
    constexpr int kN = 400;
    std::vector<double> across(kN), along(kN);
    for (int i = 0; i < kN; ++i) {
        across[i] = texture::gabor2(9, i * 0.05, 3.0, 1.0, 0.0, 1.0);  // stepping in x
        along[i] = texture::gabor2(9, 3.0, i * 0.05, 1.0, 0.0, 1.0);   // stepping in y
        mAcross += across[i];
        mAlong += along[i];
    }
    mAcross /= kN;
    mAlong /= kN;
    for (int i = 0; i < kN; ++i) {
        varAcross += (across[i] - mAcross) * (across[i] - mAcross);
        varAlong += (along[i] - mAlong) * (along[i] - mAlong);
    }
    CHECK(varAcross > varAlong * 1.5);  // the carrier direction carries the structure
}

// ---- paper kinds ------------------------------------------------------------------------------

TEST_CASE("paperKindName round-trips the enum") {
    CHECK(std::string(texture::paperKindName(texture::PaperKind::Wove)) == "Wove");
    CHECK(std::string(texture::paperKindName(texture::PaperKind::Laid)) == "Laid");
    CHECK(std::string(texture::paperKindName(texture::PaperKind::Felt)) == "Felt");
}

TEST_CASE("the paper kinds render distinct height structure") {
    // Laid ruling runs along a fixed grain, so a Laid sheet has far stronger directional structure
    // than an isotropic Wove one at the same knobs. Grain angle 0 => laid corrugation across y,
    // chain ridges across x -> a strong per-column profile.
    texture::PaperParams wove;
    wove.kind = texture::PaperKind::Wove;
    wove.grainAnisotropy = 0.0;  // isotropic
    wove.fiber = 0.0;            // isolate the kind structure from the fibre streaks

    texture::PaperParams laid = wove;
    laid.kind = texture::PaperKind::Laid;
    laid.laidDepth = 0.9;
    laid.laidSpacing = 5.0;
    laid.chainSpacing = 24.0;

    const auto woveImg = texture::renderTexture(paperParams(wove), 96, 96);
    const auto laidImg = texture::renderTexture(paperParams(laid), 96, 96);
    REQUIRE(woveImg.image8.has_value());
    REQUIRE(laidImg.image8.has_value());
    // The laid ruling is a strong periodic signal on one axis; wove is isotropic-flat on both.
    CHECK(maxProfileVariance(*laidImg.image8) > maxProfileVariance(*woveImg.image8) * 3.0);

    // Felt adds a coarse low-frequency relief that Wove lacks -- distinct pixels, same seed.
    texture::PaperParams felt = wove;
    felt.kind = texture::PaperKind::Felt;
    const auto feltImg = texture::renderTexture(paperParams(felt), 96, 96);
    REQUIRE(feltImg.image8.has_value());
    CHECK(feltImg.image8->rgba != woveImg.image8->rgba);
}

TEST_CASE("determinism: same params -> identical pixels; seed and roughness move them") {
    texture::PaperParams pp;
    pp.kind = texture::PaperKind::Laid;
    const auto a = texture::renderTexture(paperParams(pp, 3), 48, 40);
    const auto b = texture::renderTexture(paperParams(pp, 3), 48, 40);
    REQUIRE(a.image8.has_value());
    REQUIRE(b.image8.has_value());
    CHECK(a.image8->rgba == b.image8->rgba);

    const auto c = texture::renderTexture(paperParams(pp, 99), 48, 40);
    REQUIRE(c.image8.has_value());
    CHECK(c.image8->rgba != a.image8->rgba);  // reseed changes the field

    texture::PaperParams rougher = pp;
    rougher.roughness = 0.95;
    const auto d = texture::renderTexture(paperParams(rougher, 3), 48, 40);
    REQUIRE(d.image8.has_value());
    CHECK(d.image8->rgba != a.image8->rgba);  // deeper tooth -> more contrast
}

// ---- deckle edge (alpha carry) ---------------------------------------------------------------

TEST_CASE("deckle edge carries transparency; default paper is opaque") {
    texture::PaperParams solid;  // deckle off
    const auto solidImg = texture::renderTexture(paperParams(solid), 80, 80);
    REQUIRE(solidImg.image8.has_value());
    // Every texel opaque.
    for (std::size_t i = 3; i < solidImg.image8->rgba.size(); i += 4)
        CHECK(solidImg.image8->rgba[i] == 255);

    texture::PaperParams deckled = solid;
    deckled.deckleEdge = true;
    deckled.deckleAmount = 0.6;
    deckled.deckleInset = 0.12;
    const auto deckImg = texture::renderTexture(paperParams(deckled), 80, 80);
    REQUIRE(deckImg.image8.has_value());
    const auto alphaAt = [&](std::uint32_t x, std::uint32_t y) {
        return deckImg.image8->rgba[(static_cast<std::size_t>(y) * 80 + x) * 4 + 3];
    };
    CHECK(alphaAt(40, 40) == 255);  // the interior stays fully opaque...
    CHECK(alphaAt(0, 0) < 128);     // ...the torn corner fades toward transparent
    // Alpha is graded, not binary: some edge texel sits strictly between.
    bool sawPartial = false;
    for (std::uint32_t x = 0; x < 80 && !sawPartial; ++x) {
        const std::uint8_t a = alphaAt(x, 0);
        if (a > 0 && a < 255) sawPartial = true;
    }
    CHECK(sawPartial);
}

// ---- print tooth ------------------------------------------------------------------------------

TEST_CASE("print tooth darkens the sheet with a stochastic speckle") {
    texture::PaperParams plain;
    plain.printTooth = false;
    texture::PaperParams inked = plain;
    inked.printTooth = true;
    inked.printAmount = 0.6;
    const auto plainImg = texture::renderTexture(paperParams(plain), 64, 64);
    const auto inkedImg = texture::renderTexture(paperParams(inked), 64, 64);
    REQUIRE(plainImg.image8.has_value());
    REQUIRE(inkedImg.image8.has_value());
    CHECK(meanLuma(*inkedImg.image8) < meanLuma(*plainImg.image8));  // speckle only darkens
}

// ---- presets ----------------------------------------------------------------------------------

TEST_CASE("the paper preset library is populated, named and renders") {
    REQUIRE(texture::paperPresetCount() == 7);
    for (std::size_t i = 0; i < texture::paperPresetCount(); ++i) {
        const auto& pre = texture::paperPreset(i);
        CHECK(std::string(pre.name).size() > 0);
        const auto img = texture::renderTexture(paperParams(pre.params), 40, 32);
        REQUIRE(img.image8.has_value());
        // A real, non-degenerate paper: some ink, not pure black or pure white.
        const double luma = meanLuma(*img.image8);
        CHECK(luma > 20.0);
        CHECK(luma < 250.0);
    }
    // The cold-press watercolour preset is the deckled one -> it carries transparency.
    const auto& cp = texture::paperPreset(3);
    CHECK(std::string(cp.name) == "Cold-press watercolour");
    CHECK(cp.params.deckleEdge);
}
