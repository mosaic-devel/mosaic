// Texture Generator Vulkan-lane parity tests (S55-h; docs/texture-generator.md §8.4): the
// compute kernels against the CPU reference renderers on the same params. Gated on a usable
// Vulkan device (the test_extrude_gpu.cpp CI-safe pattern -- a machine without one WARNs and
// passes). TOLERANCE-BASED, deliberately: the two lanes share every formula but the CPU runs
// double and the GPU float, so smooth regions agree to ~1e-4 while a handful of pixels sitting
// exactly on a hash-lattice cell boundary, a coverage/print threshold, or the solar/lunar disc
// rim can land on the other side of a branch and differ by a lot. The bar is therefore
// "the same picture": a small mean error plus a small allowed fraction of outlier pixels --
// NEVER bit-equality, and never a substitute for the CPU-pinned byte goldens
// (test_texture_layer.cpp), which the GPU lane cannot touch (no override is installed here).
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/texture/paper_render.hpp"  // paperPreset (the §5.5 library)
#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"
#include "render/texture_gpu.hpp"

namespace texture = mosaic::core::texture;
using mosaic::common::Image;
using mosaic::common::ImageF;

namespace {

// Float-image difference (the sky lane): per-channel absolute error over all four channels.
// meanAbs bounds the global drift; outliers counts pixels where ANY channel differs by more
// than `eps` (branch flips concentrate the error in single pixels).
struct DiffF {
    double meanAbs = 0.0;
    double maxAbs = 0.0;
    std::size_t outliers = 0;
    std::size_t pixels = 0;
};

DiffF compareF(const ImageF& cpu, const ImageF& gpu, double eps) {
    DiffF d;
    REQUIRE(cpu.width == gpu.width);
    REQUIRE(cpu.height == gpu.height);
    d.pixels = cpu.pixelCount();
    double sum = 0.0;
    for (std::size_t i = 0; i < cpu.rgba.size(); i += 4) {
        double pixelMax = 0.0;
        for (std::size_t c = 0; c < 4; ++c) {
            const double diff = std::abs(static_cast<double>(cpu.rgba[i + c]) - gpu.rgba[i + c]);
            sum += diff;
            pixelMax = std::max(pixelMax, diff);
        }
        d.maxAbs = std::max(d.maxAbs, pixelMax);
        if (pixelMax > eps) ++d.outliers;
    }
    d.meanAbs = sum / static_cast<double>(cpu.rgba.size());
    return d;
}

// Byte-image difference (the paper lane): the CPU quantises double shades, the GPU lane
// quantises float shades through the SAME formula, so almost every byte matches exactly or by
// one step; print-tooth threshold flips can move single pixels by up to ~40%.
struct Diff8 {
    double meanAbs = 0.0;
    std::size_t outliers = 0;  // pixels where any channel differs by more than 2 steps
    std::size_t pixels = 0;
};

Diff8 compare8(const Image& cpu, const Image& gpu) {
    Diff8 d;
    REQUIRE(cpu.width == gpu.width);
    REQUIRE(cpu.height == gpu.height);
    d.pixels = cpu.pixelCount();
    double sum = 0.0;
    for (std::size_t i = 0; i < cpu.rgba.size(); i += 4) {
        int pixelMax = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            const int diff = std::abs(static_cast<int>(cpu.rgba[i + c]) -
                                      static_cast<int>(gpu.rgba[i + c]));
            sum += diff;
            pixelMax = std::max(pixelMax, diff);
        }
        if (pixelMax > 2) ++d.outliers;
    }
    d.meanAbs = sum / static_cast<double>(cpu.rgba.size());
    return d;
}

std::unique_ptr<mosaic::render::TextureGpu> makeLane(const char* who) {
    std::string err;
    auto gpu = mosaic::render::TextureGpu::create(/*enableValidation=*/true, err);
    if (!gpu) {
        const std::string why =
            std::string("no Vulkan device -- skipping ") + who + " (" + err + ")";
        WARN_MESSAGE(true, why);
    }
    return gpu;
}

// The sky tolerance: the cache holds sRGB-ENCODED floats (~[0, 2] body, HDR at the disc), so
// one 8-bit display step is 1/255 ~= 0.0039. Measured on RADV (RX 6600 XT) every case below
// sits at meanAbs <= 5e-7 with ZERO pixels over eps -- the double->float drift is invisible at
// display precision. The budgets are deliberately looser than that: eps of ~3 display steps
// only trips on genuine branch flips (a hash-lattice cell, a coverage threshold, a disc rim
// landing on the other side of a float-rounded compare), the 0.5% outlier fraction plus the
// mean cap leave headroom for OTHER drivers' transcendental/rounding differences without ever
// letting a real regression (a wrong formula, a lost dispatch) pass as "tolerance".
constexpr double kSkyEps = 0.012;
constexpr double kSkyMeanMax = 0.0015;
constexpr double kSkyOutlierFrac = 0.005;

void checkSkyParity(const texture::TextureParams& p, std::uint32_t w, std::uint32_t h,
                    mosaic::render::TextureGpu& gpu, const char* label,
                    double outlierFrac = kSkyOutlierFrac) {
    const texture::TextureRenderResult cpu = texture::renderTexture(p, w, h);
    REQUIRE(cpu.imageF.has_value());
    texture::TextureRenderResult via;
    REQUIRE(gpu.render(p, w, h, {}, nullptr, via));
    REQUIRE(via.imageF.has_value());
    const DiffF d = compareF(*cpu.imageF, *via.imageF, kSkyEps);
    INFO(label << ": meanAbs " << d.meanAbs << ", maxAbs " << d.maxAbs << ", outliers "
               << d.outliers << "/" << d.pixels);
    CHECK(d.meanAbs < kSkyMeanMax);
    CHECK(static_cast<double>(d.outliers) <=
          outlierFrac * static_cast<double>(d.pixels) + 4.0);
}

}  // namespace

TEST_CASE("GPU sky lane draws the CPU lane's picture (representative params)") {
    auto gpu = makeLane("sky parity");
    if (!gpu) return;

    // The default frame: a volumetric cumulus deck under 2D cirrus -- both cloud lanes at once.
    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
        p.seed = 42;
        checkSkyParity(p, 96, 64, *gpu, "default (volumetric + 2D)");
    }
    // The pure 2D lane (volumetric off) at another seed and heavier coverage.
    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
        p.seed = 7;
        auto& sky = std::get<texture::SkyParams>(p.spec);
        sky.volumetricClouds = false;
        sky.cloudCoverage = 0.7;
        checkSkyParity(p, 96, 64, *gpu, "2D decks");
    }
    // Clear day: dome + haze + the sun's disc/aureole/glare (HDR values near the disc).
    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
        p.seed = 3;
        auto& sky = std::get<texture::SkyParams>(p.spec);
        sky.cloudLayers.clear();
        sky.turbidity = 1.8;
        sky.sunElevationDeg = 20.0;  // the disc sits inside the default framing
        checkSkyParity(p, 96, 64, *gpu, "clear day + sun disc");
    }
    // Golden hour: low sun, heavy haze, Beer-Lambert reddening.
    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
        p.seed = 11;
        auto& sky = std::get<texture::SkyParams>(p.spec);
        sky.sunElevationDeg = 6.0;
        sky.turbidity = 4.5;
        sky.cloudCoverage = 0.35;
        sky.cloudLayers = {
            texture::CloudLayerParams{true, texture::CloudType::Altocumulus, 1.0, 1.0, 0.0},
            texture::CloudLayerParams{true, texture::CloudType::Cirrus, 0.8, 1.0, 0.0}};
        checkSkyParity(p, 96, 64, *gpu, "golden hour");
    }
    // Civil twilight: the physical single-scattering integrator + Psi_ms tables carry the dome.
    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
        p.seed = 5;
        auto& sky = std::get<texture::SkyParams>(p.spec);
        sky.sunElevationDeg = -4.0;
        sky.volumetricClouds = false;
        checkSkyParity(p, 96, 64, *gpu, "civil twilight");
    }
    // Deep night: stars (Yale BSC splats) + the moon (LRO albedo, LOLA relief, manual phase).
    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
        p.seed = 13;
        auto& sky = std::get<texture::SkyParams>(p.spec);
        sky.sunAzimuthDeg = 15.0;
        sky.sunElevationDeg = -30.0;
        sky.turbidity = 2.0;
        sky.cloudCoverage = 0.25;
        sky.enableMoon = true;
        sky.moonAzimuthDeg = 200.0;
        sky.moonElevationDeg = 30.0;
        sky.moonScale = 8.0;  // a big disc so the surface/terminator really gets exercised
        sky.moonPhaseMode = 1;
        sky.moonIlluminatedFraction = 0.62;
        sky.starsAmount = 0.9;
        sky.cloudLayers = {
            texture::CloudLayerParams{true, texture::CloudType::Cirrus, 0.9, 1.0, 0.0}};
        checkSkyParity(p, 160, 120, *gpu, "moonlit night");
    }
    // Transparent-elements sky: dome off, clouds only (the §3.4 alpha carry).
    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
        p.seed = 21;
        auto& sky = std::get<texture::SkyParams>(p.spec);
        sky.enableDome = false;
        sky.enableHaze = false;
        sky.volumetricClouds = false;
        checkSkyParity(p, 96, 64, *gpu, "dome off (alpha carry)");
    }
    // Lens flare: the screen-space ghost train / halo / starburst (off by default; this turns
    // it on hard so the ghosts land across the frame).
    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
        p.seed = 29;
        auto& sky = std::get<texture::SkyParams>(p.spec);
        sky.cloudLayers.clear();
        sky.sunElevationDeg = 24.0;
        sky.enableLensFlare = true;
        sky.flareStrength = 0.9;
        checkSkyParity(p, 96, 64, *gpu, "lens flare");
    }
}

TEST_CASE("GPU sky window crop is byte-exact against the GPU full frame") {
    auto gpu = makeLane("sky window parity");
    if (!gpu) return;
    // Same lane, same device, same code path: the §8.2 TextureWindow contract (a window is a
    // faithful evaluation of the same frame pixels) holds EXACTLY on the GPU -- no tolerance.
    texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
    p.seed = 42;
    texture::TextureRenderResult full;
    REQUIRE(gpu->render(p, 96, 64, {}, nullptr, full));
    REQUIRE(full.imageF.has_value());
    const texture::TextureWindow winSpec{17, 9, 40, 32};
    texture::TextureRenderResult win;
    REQUIRE(gpu->render(p, 96, 64, winSpec, nullptr, win));
    REQUIRE(win.imageF.has_value());
    REQUIRE(win.imageF->width == 40);
    REQUIRE(win.imageF->height == 32);
    for (std::uint32_t y = 0; y < 32; ++y)
        for (std::uint32_t x = 0; x < 40; ++x) {
            const auto a = full.imageF->at(17 + x, 9 + y);
            const auto b = win.imageF->at(x, y);
            REQUIRE(a.r == b.r);
            REQUIRE(a.g == b.g);
            REQUIRE(a.b == b.b);
            REQUIRE(a.a == b.a);
        }

    // And per-device determinism: the same dispatch twice gives the same bytes.
    texture::TextureRenderResult again;
    REQUIRE(gpu->render(p, 96, 64, {}, nullptr, again));
    REQUIRE(again.imageF.has_value());
    CHECK(again.imageF->rgba == full.imageF->rgba);
}

TEST_CASE("GPU paper lane matches the CPU lane's bytes (representative presets)") {
    auto gpu = makeLane("paper parity");
    if (!gpu) return;

    // The paper tolerance: the GPU quantises through the CPU's own q() formula, so smooth
    // regions agree exactly or by one 8-bit step (float vs double shading); a pixel on a
    // print-tooth threshold or a deckle fringe boundary may flip whole-hog, hence the small
    // outlier allowance. Measured on RADV: meanAbs <= 1.3e-4 bytes, zero outliers (the print
    // cells match EXACTLY thanks to the shader's printCell correction -- see
    // texture_paper.comp's note on reciprocal-lowered division).
    const auto check = [&](const texture::TextureParams& p, const char* label) {
        const texture::TextureRenderResult cpu = texture::renderTexture(p, 96, 64);
        REQUIRE(cpu.image8.has_value());
        texture::TextureRenderResult via;
        REQUIRE(gpu->render(p, 96, 64, {}, nullptr, via));
        REQUIRE(via.image8.has_value());
        const Diff8 d = compare8(*cpu.image8, *via.image8);
        INFO(label << ": meanAbs " << d.meanAbs << " bytes, outliers " << d.outliers << "/"
                   << d.pixels);
        CHECK(d.meanAbs < 0.5);
        CHECK(static_cast<double>(d.outliers) <= 0.005 * static_cast<double>(d.pixels) + 4.0);
    };

    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Paper);
        p.seed = 42;
        check(p, "default wove");
    }
    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Paper);
        p.seed = 9;
        p.spec = texture::paperPreset(2).params;  // Laid bond: both ruled-line systems
        check(p, "laid bond");
    }
    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Paper);
        p.seed = 4;
        p.spec = texture::paperPreset(3).params;  // Cold-press watercolour: deckled alpha edge
        check(p, "cold-press deckle");
    }
    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Paper);
        p.seed = 17;
        p.spec = texture::paperPreset(4).params;  // Newsprint: print tooth threshold speckle
        check(p, "newsprint print tooth");
    }
}

TEST_CASE("GPU lane refuses what it does not carry (the CPU fallback contract)") {
    auto gpu = makeLane("fallback contract");
    if (!gpu) return;

    // Grass is not a per-pixel kernel: the lane must decline so the CPU reference serves.
    texture::TextureParams grass = texture::defaultTextureParams(texture::Generator::Grass);
    grass.seed = 42;
    texture::TextureRenderResult out;
    CHECK_FALSE(gpu->render(grass, 64, 48, {}, nullptr, out));
    CHECK_FALSE(out.image8.has_value());
    CHECK_FALSE(out.imageF.has_value());

    // The S55-g materials are CPU-fallback entries for now (they carry their own param structs
    // this build does not cook) -- every registry row past Grass must decline too.
    for (int gi = static_cast<int>(texture::Generator::Wood);
         gi < texture::kGeneratorCount; ++gi) {
        texture::TextureParams mat =
            texture::defaultTextureParams(static_cast<texture::Generator>(gi));
        mat.seed = 42;
        texture::TextureRenderResult matOut;
        CHECK_FALSE(gpu->render(mat, 64, 48, {}, nullptr, matOut));
        CHECK_FALSE(matOut.image8.has_value());
    }

    // A cancelled render is handled (true) but hands back the all-or-nothing EMPTY result.
    texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
    p.seed = 42;
    texture::TextureRenderProgress progress;
    progress.cancel.store(true);
    texture::TextureRenderResult cancelled;
    CHECK(gpu->render(p, 96, 64, {}, &progress, cancelled));
    CHECK_FALSE(cancelled.imageF.has_value());

    // An uncancelled progress channel reports the window's rows.
    texture::TextureRenderProgress live;
    texture::TextureRenderResult ok;
    REQUIRE(gpu->render(p, 96, 64, {}, &live, ok));
    CHECK(live.rowsTotal.load() == 64);
    CHECK(live.rowsDone.load() == 64);
    CHECK(ok.imageF.has_value());
}

// A lane-cost probe, not a test (doctest::skip -- run with -tc="*texture lane costs*" -ns):
// prints CPU vs GPU wall time for the default sky and paper at a dialog-proxy-ish size.
TEST_CASE("bench: texture lane costs" * doctest::skip()) {
    auto gpu = makeLane("bench");
    if (!gpu) return;
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const auto bench = [&](const texture::TextureParams& p, std::uint32_t w, std::uint32_t h,
                           const char* label) {
        auto t0 = Clock::now();
        const texture::TextureRenderResult cpu = texture::renderTexture(p, w, h);
        auto t1 = Clock::now();
        texture::TextureRenderResult warm;
        REQUIRE(gpu->render(p, w, h, {}, nullptr, warm));  // pipeline/table warm-up
        auto t2 = Clock::now();
        texture::TextureRenderResult via;
        REQUIRE(gpu->render(p, w, h, {}, nullptr, via));
        auto t3 = Clock::now();
        std::printf("%-28s %4ux%-4u  CPU %8.2f ms   GPU %8.2f ms (warm %8.2f)\n", label, w, h,
                    ms(t0, t1), ms(t2, t3), ms(t1, t2));
        (void)cpu;
    };
    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
        p.seed = 42;
        bench(p, 480, 320, "sky default (volumetric)");
        auto& sky = std::get<texture::SkyParams>(p.spec);
        sky.volumetricClouds = false;
        bench(p, 480, 320, "sky 2D decks");
        sky.sunElevationDeg = -30.0;
        sky.enableMoon = true;
        sky.starsAmount = 0.9;
        bench(p, 480, 320, "sky night (stars + moon)");
    }
    {
        texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Paper);
        p.seed = 42;
        bench(p, 480, 320, "paper default");
    }
}
