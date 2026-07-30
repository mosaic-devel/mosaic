#include "core/texture/grass_render.hpp"
#include "core/texture/sky_camera.hpp"
#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>

// S55-e: the grass subsystem (docs/texture-generator.md §6) -- the ground-plane homography, the
// procedural turf base, the depth-graded Bezier-blade instancing, the element toggles and the
// preset library. Property tests only; the byte-golden for the default render lives with the other
// generators in test_texture_layer.cpp.
namespace {

using namespace mosaic;
namespace texture = core::texture;

double meanLuma(const common::Image& img) {
    double sum = 0.0;
    const std::size_t n = static_cast<std::size_t>(img.width) * img.height;
    for (std::size_t i = 0; i < n; ++i)
        sum += 0.2126 * img.rgba[i * 4 + 0] + 0.7152 * img.rgba[i * 4 + 1] +
               0.0722 * img.rgba[i * 4 + 2];
    return sum / n;
}

// Mean absolute horizontal gradient over the bottom `frac` of the frame -- a "how bladey is the
// near field" probe: individual blades cut sharp vertical edges the smooth turf lacks.
double nearFieldDetail(const common::Image& img, double frac = 0.35) {
    const std::uint32_t y0 = static_cast<std::uint32_t>(img.height * (1.0 - frac));
    double sum = 0.0;
    std::size_t n = 0;
    for (std::uint32_t y = y0; y < img.height; ++y)
        for (std::uint32_t x = 1; x < img.width; ++x) {
            const std::size_t a = (static_cast<std::size_t>(y) * img.width + x) * 4;
            const std::size_t b = a - 4;
            const double la = 0.2126 * img.rgba[a] + 0.7152 * img.rgba[a + 1] + 0.0722 * img.rgba[a + 2];
            const double lb = 0.2126 * img.rgba[b] + 0.7152 * img.rgba[b + 1] + 0.0722 * img.rgba[b + 2];
            sum += std::abs(la - lb);
            ++n;
        }
    return n ? sum / static_cast<double>(n) : 0.0;
}

texture::TextureParams grassParams(const texture::GrassParams& g, std::uint64_t seed = 7,
                                   double scale = 1.0) {
    texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Grass);
    p.seed = seed;
    p.scale = scale;
    p.spec = g;
    return p;
}

}  // namespace

// ---- the ground-plane homography (§6.1) ------------------------------------------------------

TEST_CASE("GrassCamera: groundAt and project are inverse below the horizon") {
    texture::GrassParams g;
    const texture::GrassCamera cam = texture::GrassCamera::fromParams(g, 320, 240, 1.5);
    // A pixel low in the frame sees the ground; projecting that ground point back lands on it.
    for (const auto py : {180.0, 200.0, 235.0}) {
        texture::SkyVec3 ground;
        double dist = 0.0;
        REQUIRE(cam.groundAt(160.0, py, ground, dist));
        CHECK(ground.z == doctest::Approx(0.0));
        CHECK(dist > 0.0);
        double sx = 0.0, sy = 0.0, camZ = 0.0;
        REQUIRE(cam.project(ground, sx, sy, camZ));
        CHECK(sx == doctest::Approx(160.0).epsilon(1e-6));
        CHECK(sy == doctest::Approx(py).epsilon(1e-6));
        CHECK(camZ > 0.0);
    }
    // A pixel high in the frame points at/above the horizon: no ground there.
    texture::SkyVec3 sky;
    double d = 0.0;
    CHECK_FALSE(cam.groundAt(160.0, 2.0, sky, d));
    // Depth grows downward in world Y (farther = larger camZ): a nearer ground point is closer.
    texture::SkyVec3 gn, gf;
    double dn = 0.0, df = 0.0;
    REQUIRE(cam.groundAt(160.0, 238.0, gn, dn));  // very bottom = near
    REQUIRE(cam.groundAt(160.0, 150.0, gf, df));  // higher up = far
    CHECK(gf.y > gn.y);
    CHECK(df > dn);
}

// ---- determinism -----------------------------------------------------------------------------

TEST_CASE("grass: same params -> identical pixels; seed and scale move them") {
    texture::GrassParams g;
    const auto a = texture::renderGrass(grassParams(g, 3), g, 96, 72);
    const auto b = texture::renderGrass(grassParams(g, 3), g, 96, 72);
    CHECK(a.rgba == b.rgba);

    const auto c = texture::renderGrass(grassParams(g, 99), g, 96, 72);
    CHECK(c.rgba != a.rgba);  // reseed re-scatters the blades and re-tints the turf

    const auto d = texture::renderGrass(grassParams(g, 3, 2.5), g, 96, 72);
    CHECK(d.rgba != a.rgba);  // Scale zooms the lawn (bigger blades/clumps)
}

// ---- element toggles + blade structure -------------------------------------------------------

TEST_CASE("grass element toggles: turf opacity, blades add near-field structure") {
    texture::GrassParams full;
    const auto fullImg = texture::renderGrass(grassParams(full), full, 200, 160);

    // Turf ON => every texel opaque (the ground fills behind the blades).
    for (std::size_t i = 3; i < fullImg.rgba.size(); i += 4) CHECK(fullImg.rgba[i] == 255);

    // Turf OFF => a transparent ground; only blade silhouettes (and none above the horizon) write
    // coverage, so many texels are fully transparent (§3.4 alpha carry).
    texture::GrassParams noTurf = full;
    noTurf.enableTurf = false;
    const auto noTurfImg = texture::renderGrass(grassParams(noTurf), noTurf, 200, 160);
    std::size_t transparent = 0;
    for (std::size_t i = 3; i < noTurfImg.rgba.size(); i += 4)
        if (noTurfImg.rgba[i] == 0) ++transparent;
    CHECK(transparent > 0);

    // Blades OFF => the smooth turf base alone. The full render's near field has more fine
    // horizontal structure (blade edges) than the turf-only one.
    texture::GrassParams noBlades = full;
    noBlades.enableBlades = false;
    const auto turfOnlyImg = texture::renderGrass(grassParams(noBlades), noBlades, 200, 160);
    CHECK(turfOnlyImg.rgba != fullImg.rgba);
    CHECK(nearFieldDetail(fullImg) > nearFieldDetail(turfOnlyImg) * 1.3);
}

TEST_CASE("grass: density and wind change the render") {
    texture::GrassParams base;
    const auto a = texture::renderGrass(grassParams(base), base, 160, 120);

    texture::GrassParams sparse = base;
    sparse.density = 0.2;
    const auto s = texture::renderGrass(grassParams(sparse), sparse, 160, 120);
    CHECK(s.rgba != a.rgba);
    // Fewer blades => less near-field blade detail.
    CHECK(nearFieldDetail(s) < nearFieldDetail(a));

    texture::GrassParams windy = base;
    windy.windStrength = 0.9;
    windy.curvature = 0.9;
    const auto w = texture::renderGrass(grassParams(windy), windy, 160, 120);
    CHECK(w.rgba != a.rgba);
}

// ---- presets ---------------------------------------------------------------------------------

TEST_CASE("the grass preset library is populated, named and renders") {
    REQUIRE(texture::grassPresetCount() == 6);
    for (std::size_t i = 0; i < texture::grassPresetCount(); ++i) {
        const auto& pre = texture::grassPreset(i);
        CHECK(std::string(pre.name).size() > 0);
        const auto img = texture::renderGrass(grassParams(pre.params), pre.params, 96, 72);
        const double luma = meanLuma(img);
        CHECK(luma > 10.0);
        CHECK(luma < 245.0);
    }
    // The dry/savanna preset is the strawy one -> a warmer (higher R than G) mean than the lawn.
    const auto& lawn = texture::grassPreset(0);
    const auto& dry = texture::grassPreset(2);
    CHECK(std::string(lawn.name) == "Lawn");
    CHECK(std::string(dry.name) == "Dry / savanna");
    CHECK(dry.params.dryAmount > lawn.params.dryAmount);
}
