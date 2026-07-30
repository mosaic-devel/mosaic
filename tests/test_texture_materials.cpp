#include "core/texture/material_render.hpp"
#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

// S55-g: the follow-on materials (docs/texture-generator.md §1.1) -- wood, marble, stone, canvas,
// metal, all height-field recipes over the §5 paper engine. Property tests only (structure,
// determinism, window crop equality, preset libraries); the byte-goldens for the default renders
// live with the other generators in test_texture_layer.cpp.
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

double lumaVariance(const common::Image& img) {
    const double mean = meanLuma(img);
    double var = 0.0;
    const std::size_t n = static_cast<std::size_t>(img.width) * img.height;
    for (std::size_t i = 0; i < n; ++i) {
        const double luma = 0.2126 * img.rgba[i * 4 + 0] + 0.7152 * img.rgba[i * 4 + 1] +
                            0.0722 * img.rgba[i * 4 + 2];
        var += (luma - mean) * (luma - mean);
    }
    return var / n;
}

// Variance of the per-column (axis=0) or per-row (axis=1) mean-luma profile -- the directional-
// structure probe test_texture_paper.cpp established: a banded/streaked field has a large profile
// variance on the axis PERPENDICULAR to its bands and a small one along them.
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

template <class Spec>
texture::TextureParams materialParams(texture::Generator g, const Spec& spec,
                                      std::uint64_t seed = 7) {
    texture::TextureParams p = texture::defaultTextureParams(g);
    p.seed = seed;
    p.spec = spec;
    return p;
}

// The window result must equal the same crop of the full render, byte for byte (§8.2).
void checkCrop8(const common::Image& full, const common::Image& win, long x0, long y0) {
    REQUIRE(!win.rgba.empty());
    for (std::uint32_t y = 0; y < win.height; ++y) {
        const std::size_t fo =
            ((static_cast<std::size_t>(y0) + y) * full.width + static_cast<std::size_t>(x0)) * 4;
        const std::size_t wo = static_cast<std::size_t>(y) * win.width * 4;
        if (std::memcmp(&full.rgba[fo], &win.rgba[wo], static_cast<std::size_t>(win.width) * 4) !=
            0) {
            CAPTURE(y);
            FAIL_CHECK("window row differs from the full-frame crop");
            return;
        }
    }
}

constexpr texture::Generator kMaterials[] = {
    texture::Generator::Wood, texture::Generator::Marble, texture::Generator::Stone,
    texture::Generator::Canvas, texture::Generator::Metal};

}  // namespace

// ---- registry + lanes ---------------------------------------------------------------------------

TEST_CASE("the material generators register, seed their arms and render the 8-bit lane opaque") {
    CHECK(texture::kGeneratorCount == 8);
    for (const auto g : kMaterials) {
        CAPTURE(texture::generatorName(g));
        const texture::TextureParams p = texture::defaultTextureParams(g);
        CHECK(p.generator == g);
        const auto r = texture::renderTexture(p, 40, 30);
        REQUIRE(r.image8.has_value());  // materials are the 8-bit lane, like paper
        CHECK(!r.imageF.has_value());
        CHECK(r.image8->width == 40);
        CHECK(r.image8->height == 30);
        for (std::size_t i = 3; i < r.image8->rgba.size(); i += 4)
            CHECK(r.image8->rgba[i] == 255);  // opaque sheets (no alpha carry)
        // A real, non-degenerate surface: some ink, not pure black or blown white.
        const double luma = meanLuma(*r.image8);
        CHECK(luma > 20.0);
        CHECK(luma < 250.0);
        // Scale semantics are paper's: pixel-sized features.
        CHECK(texture::generatorTraits(g).pixelScaledFeatures);
    }
    CHECK(std::string(texture::generatorName(texture::Generator::Wood)) == "Wood");
    CHECK(std::string(texture::generatorName(texture::Generator::Marble)) == "Marble");
    CHECK(std::string(texture::generatorName(texture::Generator::Stone)) == "Stone");
    CHECK(std::string(texture::generatorName(texture::Generator::Canvas)) == "Canvas");
    CHECK(std::string(texture::generatorName(texture::Generator::Metal)) == "Metal");
}

TEST_CASE("materials are deterministic; seed and scale move the pixels") {
    for (const auto g : kMaterials) {
        CAPTURE(texture::generatorName(g));
        texture::TextureParams p = texture::defaultTextureParams(g);
        p.seed = 3;
        const auto a = texture::renderTexture(p, 48, 40);
        const auto b = texture::renderTexture(p, 48, 40);
        REQUIRE(a.image8.has_value());
        REQUIRE(b.image8.has_value());
        CHECK(a.image8->rgba == b.image8->rgba);  // §8.3: same inputs, same pixels

        texture::TextureParams reseeded = p;
        reseeded.seed = 99;
        const auto c = texture::renderTexture(reseeded, 48, 40);
        REQUIRE(c.image8.has_value());
        CHECK(c.image8->rgba != a.image8->rgba);

        texture::TextureParams rescaled = p;
        rescaled.scale = 3.0;
        const auto d = texture::renderTexture(rescaled, 48, 40);
        REQUIRE(d.image8.has_value());
        CHECK(d.image8->rgba != a.image8->rgba);
    }
}

TEST_CASE("material windows are byte-exact crops of the full frame") {
    constexpr std::uint32_t kW = 64, kH = 48;
    const texture::TextureWindow wins[] = {
        {0, 0, 16, 16}, {17, 9, 21, 13}, {kW - 16, kH - 16, 16, 16}, {5, kH - 8, kW - 5, 8}};
    for (const auto g : kMaterials) {
        CAPTURE(texture::generatorName(g));
        texture::TextureParams p = texture::defaultTextureParams(g);
        p.seed = 42;
        const auto full = texture::renderTexture(p, kW, kH);
        REQUIRE(full.image8.has_value());
        for (const texture::TextureWindow& w : wins) {
            const auto part = texture::renderTexture(p, kW, kH, w);
            REQUIRE(part.image8.has_value());
            CHECK(part.image8->width == w.w);
            CHECK(part.image8->height == w.h);
            checkCrop8(*full.image8, *part.image8, w.x, w.y);
        }
    }
}

// ---- per-material structure ---------------------------------------------------------------------

TEST_CASE("wood: rings band across the grain; knots change the field") {
    texture::WoodParams plain;  // isolate the ring system from streaks and knots
    plain.fiber = 0.0;
    plain.knots = 0.0;
    plain.grainAngleDeg = 0.0;  // grain along x -> ring bands vary with y
    plain.ringSpacing = 12.0;
    const auto img = texture::renderTexture(
        materialParams(texture::Generator::Wood, plain), 96, 96);
    REQUIRE(img.image8.has_value());
    // Bands across y: the per-row profile carries the rings, the per-column one averages them out.
    CHECK(profileVariance(*img.image8, 1) > 3.0 * profileVariance(*img.image8, 0));

    texture::WoodParams knotty = plain;
    knotty.knots = 0.9;
    const auto knotImg = texture::renderTexture(
        materialParams(texture::Generator::Wood, knotty), 96, 96);
    REQUIRE(knotImg.image8.has_value());
    CHECK(knotImg.image8->rgba != img.image8->rgba);
}

TEST_CASE("marble: contrast strengthens the veining") {
    texture::MarbleParams faint;
    faint.contrast = 0.05;
    texture::MarbleParams bold = faint;
    bold.contrast = 0.95;
    const auto a = texture::renderTexture(
        materialParams(texture::Generator::Marble, faint), 96, 96);
    const auto b = texture::renderTexture(
        materialParams(texture::Generator::Marble, bold), 96, 96);
    REQUIRE(a.image8.has_value());
    REQUIRE(b.image8.has_value());
    CHECK(lumaVariance(*b.image8) > 2.0 * lumaVariance(*a.image8));
}

TEST_CASE("stone: cracks darken the aggregate; per-cell variation moves the pixels") {
    texture::StoneParams smooth;
    smooth.crackDepth = 0.0;
    smooth.cellSize = 24.0;
    texture::StoneParams cracked = smooth;
    cracked.crackDepth = 0.95;
    const auto a = texture::renderTexture(
        materialParams(texture::Generator::Stone, smooth), 96, 96);
    const auto b = texture::renderTexture(
        materialParams(texture::Generator::Stone, cracked), 96, 96);
    REQUIRE(a.image8.has_value());
    REQUIRE(b.image8.has_value());
    CHECK(meanLuma(*b.image8) < meanLuma(*a.image8));  // crack channels only darken

    texture::StoneParams uniform = smooth;
    uniform.variation = 0.0;
    texture::StoneParams varied = smooth;
    varied.variation = 0.9;
    const auto c = texture::renderTexture(
        materialParams(texture::Generator::Stone, uniform), 96, 96);
    const auto d = texture::renderTexture(
        materialParams(texture::Generator::Stone, varied), 96, 96);
    REQUIRE(c.image8.has_value());
    REQUIRE(d.image8.has_value());
    CHECK(lumaVariance(*d.image8) > lumaVariance(*c.image8));
}

TEST_CASE("canvas weaves both axes; metal streaks one") {
    texture::CanvasParams weave;  // a clean regular weave: periodic on BOTH axes
    weave.irregularity = 0.0;
    weave.fuzz = 0.0;
    weave.threadPitch = 8.0;
    weave.weaveAngleDeg = 0.0;
    const auto cImg = texture::renderTexture(
        materialParams(texture::Generator::Canvas, weave), 96, 96);
    REQUIRE(cImg.image8.has_value());

    texture::MetalParams brushed;  // streaks along x: rows streaky, columns averaged flat
    brushed.brushAngleDeg = 0.0;
    brushed.gradient = 0.0;  // isolate the anisotropy from the vertical ramp
    const auto mImg = texture::renderTexture(
        materialParams(texture::Generator::Metal, brushed), 96, 96);
    REQUIRE(mImg.image8.has_value());

    // The weave carries structure on BOTH axes; the brushed sheet is strongly one-sided.
    const double cMin = std::min(profileVariance(*cImg.image8, 0), profileVariance(*cImg.image8, 1));
    const double mMin = std::min(profileVariance(*mImg.image8, 0), profileVariance(*mImg.image8, 1));
    CHECK(cMin > 3.0 * mMin);
    CHECK(profileVariance(*mImg.image8, 1) > 5.0 * profileVariance(*mImg.image8, 0));
}

TEST_CASE("metal: the reflection gradient brightens the top of the sheet") {
    texture::MetalParams m;
    m.gradient = 0.8;
    const auto img = texture::renderTexture(
        materialParams(texture::Generator::Metal, m), 64, 96);
    REQUIRE(img.image8.has_value());
    // Mean luma of the top quarter vs the bottom quarter.
    const auto bandLuma = [&](std::uint32_t y0, std::uint32_t y1) {
        double sum = 0.0;
        std::size_t n = 0;
        for (std::uint32_t y = y0; y < y1; ++y)
            for (std::uint32_t x = 0; x < img.image8->width; ++x) {
                const std::size_t o = (static_cast<std::size_t>(y) * img.image8->width + x) * 4;
                sum += 0.2126 * img.image8->rgba[o] + 0.7152 * img.image8->rgba[o + 1] +
                       0.0722 * img.image8->rgba[o + 2];
                ++n;
            }
        return sum / static_cast<double>(n);
    };
    CHECK(bandLuma(0, 24) > bandLuma(72, 96) + 8.0);
}

// ---- presets -------------------------------------------------------------------------------------

TEST_CASE("the material preset libraries are populated, named and render") {
    REQUIRE(texture::woodPresetCount() == 4);
    REQUIRE(texture::marblePresetCount() == 3);
    REQUIRE(texture::stonePresetCount() == 4);
    REQUIRE(texture::canvasPresetCount() == 4);
    REQUIRE(texture::metalPresetCount() == 4);
    for (const auto g : kMaterials) {
        CAPTURE(texture::generatorName(g));
        const texture::GeneratorTraits& traits = texture::generatorTraits(g);
        for (std::size_t i = 0; i < traits.presetCount(); ++i) {
            CHECK(std::string(traits.presetName(i)).size() > 0);
            texture::TextureParams p = texture::defaultTextureParams(g);
            p.seed = 7;
            traits.applyPreset(p, i);
            CHECK(traits.matchPreset(p) == static_cast<int>(i) + 1);
            const auto img = texture::renderTexture(p, 40, 32);
            REQUIRE(img.image8.has_value());
            const double luma = meanLuma(*img.image8);
            CHECK(luma > 12.0);
            CHECK(luma < 250.0);
        }
    }
    // Spot-pin a name per library (ordering is part of the data contract).
    CHECK(std::string(texture::woodPreset(2).name) == "Knotty pine");
    CHECK(std::string(texture::marblePreset(1).name) == "Nero");
    CHECK(std::string(texture::stonePreset(1).name) == "Concrete");
    CHECK(std::string(texture::canvasPreset(0).name) == "Cotton duck");
    CHECK(std::string(texture::metalPreset(3).name) == "Gunmetal");
}
