#include "core/texture/render_worker.hpp"
#include "core/texture/sky_render.hpp"
#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <variant>

// S55-f core surface: the TextureWindow (a byte-exact sub-rect evaluation of the full frame --
// the dialog's 1:1 / pan preview), the TextureRenderProgress channel (Create's progress bar +
// cancellation), the background TextureRenderWorker, and the sky preset library
// (docs/texture-generator.md §7.4/§8.2).
namespace {

using namespace mosaic;
namespace texture = core::texture;

texture::TextureParams paramsFor(texture::Generator g) {
    texture::TextureParams p = texture::defaultTextureParams(g);
    p.seed = 42;
    return p;
}

// Exercise the alpha-carrying paths too: deckled paper and turf-off grass write real coverage.
texture::TextureParams paperDeckled() {
    texture::TextureParams p = paramsFor(texture::Generator::Paper);
    auto& paper = std::get<texture::PaperParams>(p.spec);
    paper.deckleEdge = true;
    paper.printTooth = true;
    paper.kind = texture::PaperKind::Laid;
    return p;
}

// The window result must equal the same crop of the full render, byte for byte.
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

void checkCropF(const common::ImageF& full, const common::ImageF& win, long x0, long y0) {
    REQUIRE(!win.rgba.empty());
    for (std::uint32_t y = 0; y < win.height; ++y) {
        const std::size_t fo =
            ((static_cast<std::size_t>(y0) + y) * full.width + static_cast<std::size_t>(x0)) * 4;
        const std::size_t wo = static_cast<std::size_t>(y) * win.width * 4;
        if (std::memcmp(&full.rgba[fo], &win.rgba[wo],
                        static_cast<std::size_t>(win.width) * 4 * sizeof(float)) != 0) {
            CAPTURE(y);
            FAIL_CHECK("window row differs from the full-frame crop (float lane)");
            return;
        }
    }
}

}  // namespace

TEST_CASE("texture window: a sub-rect render is a byte-exact crop of the full frame") {
    constexpr std::uint32_t kW = 64, kH = 48;
    // Interior, corner and edge-hugging windows; the last touches the bottom-right frame corner.
    const texture::TextureWindow wins[] = {
        {0, 0, 16, 16}, {17, 9, 21, 13}, {kW - 16, kH - 16, 16, 16}, {5, kH - 8, kW - 5, 8}};

    SUBCASE("sky (float lane, volumetric + 2D decks)") {
        const texture::TextureParams p = paramsFor(texture::Generator::Sky);
        const auto full = texture::renderTexture(p, kW, kH);
        REQUIRE(full.imageF.has_value());
        for (const texture::TextureWindow& w : wins) {
            const auto part = texture::renderTexture(p, kW, kH, w);
            REQUIRE(part.imageF.has_value());
            CHECK(part.imageF->width == w.w);
            CHECK(part.imageF->height == w.h);
            checkCropF(*full.imageF, *part.imageF, w.x, w.y);
        }
    }
    SUBCASE("night sky (projected star field splats across window seams)") {
        // The S55 star field projects the catalogue in FRAME space, then splats per pixel -- a
        // crop must reproduce the same bytes even where a star's PSF straddles the window edge.
        texture::TextureParams p = paramsFor(texture::Generator::Sky);
        auto& sky = std::get<texture::SkyParams>(p.spec);
        sky.enableClouds = false;
        sky.sunElevationDeg = -30.0;  // deep night -> stars on
        sky.starsAmount = 1.0;
        sky.fovDeg = 90.0;
        sky.pitchDeg = 45.0;
        const auto full = texture::renderTexture(p, kW, kH);
        REQUIRE(full.imageF.has_value());
        for (const texture::TextureWindow& w : wins) {
            const auto part = texture::renderTexture(p, kW, kH, w);
            REQUIRE(part.imageF.has_value());
            checkCropF(*full.imageF, *part.imageF, w.x, w.y);
        }
    }
    SUBCASE("physical twilight (phase-4 integrator + relit clouds across window seams)") {
        // A sub-horizon sun routes the dome through the single-scattering integrator AND relights the
        // cloud decks from the scattered twilight; a crop must still be byte-exact (the integrator is
        // a pure function of the ray, cooked once from the full frame).
        texture::TextureParams p = paramsFor(texture::Generator::Sky);
        auto& sky = std::get<texture::SkyParams>(p.spec);
        sky.sunElevationDeg = -4.0;  // civil twilight: HW<->integrator blend + relit clouds
        sky.cloudCoverage = 0.5;
        sky.enableMoon = true;
        sky.moonElevationDeg = 20.0;
        const auto full = texture::renderTexture(p, kW, kH);
        REQUIRE(full.imageF.has_value());
        for (const texture::TextureWindow& w : wins) {
            const auto part = texture::renderTexture(p, kW, kH, w);
            REQUIRE(part.imageF.has_value());
            checkCropF(*full.imageF, *part.imageF, w.x, w.y);
        }
    }
    SUBCASE("lens flare (screen-space ghosts/halo/starburst across window seams)") {
        // The flare's sprites are placed in FRAME space off the full frame's centre and sun
        // projection; a crop must reproduce the same bytes even where a ghost or a starburst
        // streak straddles the window edge.
        texture::TextureParams p = paramsFor(texture::Generator::Sky);
        auto& sky = std::get<texture::SkyParams>(p.spec);
        sky.enableClouds = false;
        sky.sunAzimuthDeg = 168.0;  // off-centre so the ghost train crosses the frame
        sky.sunElevationDeg = 38.0;
        sky.enableLensFlare = true;
        sky.flareStrength = 1.0;
        const auto full = texture::renderTexture(p, kW, kH);
        REQUIRE(full.imageF.has_value());
        for (const texture::TextureWindow& w : wins) {
            const auto part = texture::renderTexture(p, kW, kH, w);
            REQUIRE(part.imageF.has_value());
            checkCropF(*full.imageF, *part.imageF, w.x, w.y);
        }
    }
    SUBCASE("paper (deckle alpha + print tooth + laid ruling)") {
        const texture::TextureParams p = paperDeckled();
        const auto full = texture::renderTexture(p, kW, kH);
        REQUIRE(full.image8.has_value());
        for (const texture::TextureWindow& w : wins) {
            const auto part = texture::renderTexture(p, kW, kH, w);
            REQUIRE(part.image8.has_value());
            checkCrop8(*full.image8, *part.image8, w.x, w.y);
        }
    }
    SUBCASE("grass (blade raster across window seams)") {
        const texture::TextureParams p = paramsFor(texture::Generator::Grass);
        const auto full = texture::renderTexture(p, kW, kH);
        REQUIRE(full.image8.has_value());
        for (const texture::TextureWindow& w : wins) {
            const auto part = texture::renderTexture(p, kW, kH, w);
            REQUIRE(part.image8.has_value());
            checkCrop8(*full.image8, *part.image8, w.x, w.y);
        }
    }
    SUBCASE("a default window is the whole frame; an oversized one clamps inside") {
        const texture::TextureParams p = paramsFor(texture::Generator::Paper);
        const auto full = texture::renderTexture(p, kW, kH);
        const auto dflt = texture::renderTexture(p, kW, kH, texture::TextureWindow{});
        REQUIRE(full.image8.has_value());
        REQUIRE(dflt.image8.has_value());
        CHECK(full.image8->rgba == dflt.image8->rgba);
        const auto big = texture::renderTexture(p, kW, kH, {-5, -5, 999, 999});
        REQUIRE(big.image8.has_value());
        CHECK(big.image8->width == kW);
        CHECK(big.image8->height == kH);
        CHECK(big.image8->rgba == full.image8->rgba);
    }
}

TEST_CASE("texture progress: a completed render fills its row budget; cancel yields nothing") {
    constexpr std::uint32_t kW = 48, kH = 32;
    for (const texture::Generator g :
         {texture::Generator::Sky, texture::Generator::Paper, texture::Generator::Grass}) {
        CAPTURE(static_cast<int>(g));
        texture::TextureRenderProgress prog;
        const auto r = texture::renderTexture(paramsFor(g), kW, kH, {}, &prog);
        CHECK((r.image8.has_value() || r.imageF.has_value()));
        CHECK(prog.rowsTotal.load() > 0);
        CHECK(prog.rowsDone.load() == prog.rowsTotal.load());
        CHECK_FALSE(prog.cancel.load());

        // Pre-cancelled: the all-or-nothing contract -- no partial pixels ever escape.
        texture::TextureRenderProgress dead;
        dead.cancel.store(true);
        const auto rc = texture::renderTexture(paramsFor(g), kW, kH, {}, &dead);
        CHECK_FALSE(rc.image8.has_value());
        CHECK_FALSE(rc.imageF.has_value());
    }
    // Progress plumbing is observational: same bytes with and without a channel attached.
    texture::TextureRenderProgress prog;
    const auto with = texture::renderTexture(paramsFor(texture::Generator::Grass), kW, kH, {}, &prog);
    const auto without = texture::renderTexture(paramsFor(texture::Generator::Grass), kW, kH);
    REQUIRE(with.image8.has_value());
    REQUIRE(without.image8.has_value());
    CHECK(with.image8->rgba == without.image8->rgba);
}

TEST_CASE("texture render worker: coalescing, epochs, cancellation") {
    texture::TextureRenderWorker worker;

    const auto waitResult = [&](std::uint64_t wantEpoch,
                                int maxMs) -> std::optional<texture::TextureRenderWorker::Result> {
        for (int i = 0; i < maxMs; ++i) {
            if (auto r = worker.takeResult()) {
                if (r->epoch == wantEpoch) return r;
                // Stale epoch: an older job completed before ours was picked up -- keep polling.
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::nullopt;
    };

    SUBCASE("a single job completes and carries its epoch") {
        worker.request({7, paramsFor(texture::Generator::Paper), 32, 24, {}});
        const auto r = waitResult(7, 5000);
        REQUIRE(r.has_value());
        REQUIRE(r->render.image8.has_value());
        CHECK(r->render.image8->width == 32);
        CHECK(r->render.image8->height == 24);
    }
    SUBCASE("a burst of requests coalesces to the newest epoch") {
        for (std::uint64_t e = 1; e <= 24; ++e)
            worker.request({e, paramsFor(texture::Generator::Paper), 48, 32, {}});
        const auto r = waitResult(24, 5000);
        REQUIRE(r.has_value());
        REQUIRE(r->render.image8.has_value());
        // Anything still queued behind is at most one stale, already-superseded result.
        int extra = 0;
        while (worker.takeResult()) ++extra;
        CHECK(extra == 0);
    }
    SUBCASE("cancelAll silences the worker") {
        worker.request({99, paramsFor(texture::Generator::Grass), 640, 480, {}});
        worker.cancelAll();
        // Give a cancelled render time to unwind; no result may surface afterwards.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (int i = 0; i < 200 && worker.busy(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK_FALSE(worker.busy());
        CHECK_FALSE(worker.takeResult().has_value());
    }
}

TEST_CASE("sky presets: pinned names, valid values, distinct renders") {
    REQUIRE(texture::skyPresetCount() == 14);
    const char* expected[] = {"Clear day",     "Fair-weather cumulus", "Golden hour",
                              "High cirrus",   "Sunset streaks",       "Overcast",
                              "Storm brewing", "Moonlit night",        "Cloudy moonlit night",
                              "Crescent & scattered cloud",
                              "Overcast night", "Stormy night", "Misty moonrise",
                              "Starry high cirrus"};
    for (std::size_t i = 0; i < texture::skyPresetCount(); ++i) {
        CAPTURE(i);
        const texture::SkyPreset& pre = texture::skyPreset(i);
        CHECK(std::string(pre.name) == expected[i]);
        CHECK(pre.params.turbidity >= 1.0);
        CHECK(pre.params.turbidity <= 10.0);
        // Every preset must actually render (tiny frame; the point is "no preset is broken").
        texture::TextureParams p = paramsFor(texture::Generator::Sky);
        p.spec = pre.params;
        const auto r = texture::renderTexture(p, 24, 16);
        REQUIRE(r.imageF.has_value());
        CHECK(r.imageF->rgba.size() == 24u * 16u * 4u);
    }
    // "Clear day" carries no decks; the storm carries a cumulonimbus; the night carries a moon.
    CHECK(texture::skyPreset(0).params.cloudLayers.empty());
    REQUIRE_FALSE(texture::skyPreset(6).params.cloudLayers.empty());
    CHECK(texture::skyPreset(6).params.cloudLayers[0].type == texture::CloudType::Cumulonimbus);
    CHECK(texture::skyPreset(7).params.enableMoon);
    CHECK(texture::skyPreset(7).params.sunElevationDeg < 0.0);
}

TEST_CASE("night sky (S55-f): darkness, stars, and the moon's phase geometry") {
    constexpr std::uint32_t kW = 96, kH = 64;
    const auto mean = [](const common::ImageF& img) {
        double s = 0.0;
        for (std::size_t i = 0; i < img.rgba.size(); i += 4)
            s += img.rgba[i] + img.rgba[i + 1] + img.rgba[i + 2];
        return s / (static_cast<double>(img.rgba.size() / 4) * 3.0);
    };
    const auto maxCh = [](const common::ImageF& img) {
        float m = 0.0f;
        for (std::size_t i = 0; i < img.rgba.size(); i += 4)
            m = std::max({m, img.rgba[i], img.rgba[i + 1], img.rgba[i + 2]});
        return m;
    };
    texture::TextureParams p = paramsFor(texture::Generator::Sky);
    auto& sky = std::get<texture::SkyParams>(p.spec);
    sky.enableClouds = false;
    sky.enableSun = false;

    SUBCASE("deep night is dark; twilight sits between; the day path ignores the night knobs") {
        sky.starsAmount = 0.0;
        sky.sunElevationDeg = 30.0;
        const auto day = texture::renderTexture(p, kW, kH);
        sky.sunElevationDeg = -8.0;
        const auto twilight = texture::renderTexture(p, kW, kH);
        sky.sunElevationDeg = -25.0;
        const auto night = texture::renderTexture(p, kW, kH);
        REQUIRE(day.imageF.has_value());
        REQUIRE(twilight.imageF.has_value());
        REQUIRE(night.imageF.has_value());
        // Means are sRGB-encoded (the cache convention), which lifts dark values a lot -- the
        // ratios stay meaningful, the absolute gaps look smaller than the scene is.
        CHECK(mean(*night.imageF) < 0.5 * mean(*twilight.imageF));
        CHECK(mean(*twilight.imageF) < 0.8 * mean(*day.imageF));
        // Above the horizon the night fields are inert: stars/moon knobs change no byte.
        sky.sunElevationDeg = 30.0;
        sky.starsAmount = 1.0;
        sky.moonScale = 3.0;  // moon still DISABLED -- the day sky must not move
        const auto day2 = texture::renderTexture(p, kW, kH);
        REQUIRE(day2.imageF.has_value());
        CHECK(day2.imageF->rgba == day.imageF->rgba);
    }
    SUBCASE("stars appear at night, scale with the amount, and vanish by day") {
        sky.sunElevationDeg = -25.0;
        sky.starsAmount = 0.0;
        const auto bare = texture::renderTexture(p, kW, kH);
        sky.starsAmount = 1.0;
        const auto starry = texture::renderTexture(p, kW, kH);
        REQUIRE(bare.imageF.has_value());
        REQUIRE(starry.imageF.has_value());
        CHECK(maxCh(*starry.imageF) > maxCh(*bare.imageF) + 0.08f);
    }
    SUBCASE("the moon renders bright at night and follows the phase geometry") {
        sky.sunAzimuthDeg = 15.0;  // parked opposite the moon, well below the horizon
        sky.sunElevationDeg = -30.0;
        sky.starsAmount = 0.0;
        sky.enableMoon = true;
        sky.moonAzimuthDeg = 200.0;
        sky.moonElevationDeg = 30.0;
        sky.moonScale = 3.0;
        const auto full = texture::renderTexture(p, kW, kH);
        REQUIRE(full.imageF.has_value());
        CHECK(maxCh(*full.imageF) > 0.7f);  // a near-full moon blazes against the night
        // Sun swung under the moon's own azimuth (same depth -> the sky itself is unchanged):
        // the phase closes toward a quarter and the frame carries less moonlight.
        sky.sunAzimuthDeg = 200.0;
        const auto quarter = texture::renderTexture(p, kW, kH);
        REQUIRE(quarter.imageF.has_value());
        CHECK(mean(*quarter.imageF) < mean(*full.imageF));
        // Disabled moon: none of it.
        sky.enableMoon = false;
        const auto off = texture::renderTexture(p, kW, kH);
        REQUIRE(off.imageF.has_value());
        CHECK(maxCh(*off.imageF) < maxCh(*full.imageF));
    }
}
