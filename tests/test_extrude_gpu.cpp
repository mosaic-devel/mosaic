// Vulkan-lane parity tests (S30-c, docs/type-tool.md §10.5): the compute rasterizer against the
// CPU lane on the same solids. Gated on a usable Vulkan device (the CI-safe pattern); tolerance-
// based -- the two lanes share every formula but differ in float precision (double CPU vs float
// GPU) and in benign same-depth tie-breaking, so the assert is "the same picture", not same bits.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "common/geometry3d.hpp"
#include "core/layer_effects.hpp"
#include "core/text/extrude_mesh.hpp"
#include "core/text/extrude_overlay.hpp"
#include "core/text/extrude_render.hpp"
#include "render/extrude_gpu.hpp"

using namespace mosaic::core::text;
namespace vec = mosaic::core::vec;
using mosaic::common::Affine2D;
using mosaic::common::ImageF;
using mosaic::common::Quat;
using mosaic::common::Vec2;

namespace {

GlyphSolidInput squareWithHole() {
    vec::Contour outer, hole;
    outer.points = {{0, 0}, {20, 0}, {20, 20}, {0, 20}};
    outer.closed = true;
    hole.points = {{6, 6}, {14, 6}, {14, 14}, {6, 14}};
    hole.closed = true;
    return {{outer, hole}, 0};
}

struct Diff {
    int cpuCovered = 0, gpuCovered = 0, coverDiff = 0;
    double meanAbs = 0.0;  // mean |channel delta| over pixels both lanes covered
};
Diff compare(const ImageF& cpu, const ImageF& gpu) {
    Diff d;
    double sum = 0.0;
    long n = 0;
    for (std::uint32_t y = 0; y < cpu.height; ++y)
        for (std::uint32_t x = 0; x < cpu.width; ++x) {
            const auto a = cpu.at(x, y);
            const auto b = gpu.at(x, y);
            const bool ca = a.a > 0.5f, cb = b.a > 0.5f;
            d.cpuCovered += ca ? 1 : 0;
            d.gpuCovered += cb ? 1 : 0;
            if (ca != cb) ++d.coverDiff;
            if (ca && cb) {
                sum += std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b);
                n += 3;
            }
        }
    d.meanAbs = n > 0 ? sum / static_cast<double>(n) : 0.0;
    return d;
}

}  // namespace

TEST_CASE("the Vulkan lane draws the same picture as the CPU lane") {
    std::string err;
    auto gpu = mosaic::render::ExtrudeGpu::create(/*enableValidation=*/true, err);
    if (!gpu) {
        const std::string why = "no Vulkan device -- skipping GPU parity (" + err + ")";
        WARN_MESSAGE(true, why);
        return;
    }

    Extrude e;
    e.depth = 14.0f;
    e.perspective = 22.0f;
    e.bevelFront.size = 1.5f;
    e.bevelFront.profile = Bevel::Profile::Round;
    e.orientation = Quat::fromAxisAngle({0.5, 0.8, 0.2}, 0.7);
    e.material.albedo = {0.85f, 0.55f, 0.2f, 1.0f};
    e.material.roughness = 0.35f;
    e.lights = {Light{{0.4, 0.5, -0.75}, {1, 1, 1, 1}, 1.0f}};

    const ExtrudeMesh mesh = buildExtrudeMesh({squareWithHole()}, e);
    REQUIRE_FALSE(mesh.empty());

    const Affine2D place = Affine2D::translation(30.0, 30.0);
    ImageF cpu(90, 90), viaGpu(90, 90);
    renderExtrudeMeshF(cpu, mesh, e, place, /*antialias=*/true);  // no override set in tests
    REQUIRE(gpu->render(viaGpu, mesh, e, place, /*antialias=*/true));

    const Diff d = compare(cpu, viaGpu);
    REQUIRE(d.cpuCovered > 200);
    // Coverage: the silhouettes agree to a sliver (float rounding along edges).
    CHECK(d.coverDiff <= d.cpuCovered / 50 + 8);
    // Shading: same formulas, different precision -- a couple of 8-bit steps of slack.
    CHECK(d.meanAbs < 0.02);

    // A second render (cached mesh buffers) is just as right.
    ImageF again(90, 90);
    REQUIRE(gpu->render(again, mesh, e, place, true));
    const Diff d2 = compare(viaGpu, again);
    CHECK(d2.coverDiff == 0);

    // Lighting toggle parity: flat lane vs flat lane.
    Extrude flat = e;
    flat.lightingEnabled = false;
    ImageF cpuFlat(90, 90), gpuFlat(90, 90);
    renderExtrudeMeshF(cpuFlat, buildExtrudeMesh({squareWithHole()}, flat), flat, place, true);
    REQUIRE(gpu->render(gpuFlat, buildExtrudeMesh({squareWithHole()}, flat), flat, place, true));
    const Diff df = compare(cpuFlat, gpuFlat);
    CHECK(df.meanAbs < 0.005);  // flat albedo: no shading math to diverge

    CHECK(err.empty());
}

TEST_CASE("the Vulkan lane matches the CPU lane with canvas reflections on") {
    std::string err;
    auto gpu = mosaic::render::ExtrudeGpu::create(/*enableValidation=*/true, err);
    if (!gpu) {
        WARN_MESSAGE(true, "no Vulkan device -- skipping GPU env parity");
        return;
    }

    Extrude e;
    e.depth = 14.0f;
    e.perspective = 15.0f;
    e.orientation = Quat::fromAxisAngle({1.0, 0.2, 0.0}, 0.9);  // tipped: rays reach the canvas
    e.material.albedo = {0.9f, 0.9f, 0.9f, 1.0f};
    e.material.metalness = 1.0f;
    e.material.roughness = 0.1f;
    e.reflectCanvas = true;

    // A gradient snapshot so a mapping bug (not just a missing sample) breaks parity.
    ImageF envImg(16, 16);
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) {
            const std::size_t at = (y * 16 + x) * 4;
            envImg.rgba[at + 0] = static_cast<float>(x) / 15.0f;
            envImg.rgba[at + 1] = static_cast<float>(y) / 15.0f;
            envImg.rgba[at + 2] = 0.5f;
            envImg.rgba[at + 3] = 1.0f;
        }
    const mosaic::core::text::ExtrudeEnv env{&envImg,
                                             Affine2D::scaling(0.5, 0.5)};

    const ExtrudeMesh mesh = buildExtrudeMesh({squareWithHole()}, e);
    REQUIRE_FALSE(mesh.empty());
    const Affine2D place = Affine2D::translation(30.0, 30.0);
    ImageF cpu(90, 90), viaGpu(90, 90);
    renderExtrudeMeshF(cpu, mesh, e, place, true, &env);
    REQUIRE(gpu->render(viaGpu, mesh, e, place, true, &env));
    const Diff d = compare(cpu, viaGpu);
    REQUIRE(d.cpuCovered > 200);
    CHECK(d.coverDiff <= d.cpuCovered / 50 + 8);
    CHECK(d.meanAbs < 0.02);

    // Sides-only parity: the per-vertex cap flag must survive the vertex packing (position.w).
    Extrude sides = e;
    sides.reflectSidesOnly = true;
    ImageF cpuSides(90, 90), gpuSides(90, 90);
    renderExtrudeMeshF(cpuSides, mesh, sides, place, true, &env);
    REQUIRE(gpu->render(gpuSides, mesh, sides, place, true, &env));
    const Diff ds = compare(cpuSides, gpuSides);
    CHECK(ds.coverDiff <= ds.cpuCovered / 50 + 8);
    CHECK(ds.meanAbs < 0.02);
    CHECK(err.empty());
}

TEST_CASE("the Vulkan lane matches the CPU lane with S30-e overlay maps on") {
    std::string err;
    auto gpu = mosaic::render::ExtrudeGpu::create(/*enableValidation=*/true, err);
    if (!gpu) {
        WARN_MESSAGE(true, "no Vulkan device -- skipping GPU overlay parity");
        return;
    }

    Extrude e;
    e.depth = 14.0f;
    e.perspective = 18.0f;
    e.bevelFront.size = 1.0f;
    e.orientation = Quat::fromAxisAngle({0.3, 0.8, 0.1}, 0.6);  // front cap AND walls visible
    e.material.albedo = {0.9f, 0.2f, 0.15f, 1.0f};
    e.material.roughness = 0.4f;
    e.lights = {Light{{0.4, 0.5, -0.75}, {1, 1, 1, 1}, 1.0f}};
    const ExtrudeMesh mesh = buildExtrudeMesh({squareWithHole()}, e);
    REQUIRE_FALSE(mesh.empty());

    // A gradient overlay so the map's UV mapping (not just its presence) is what parity tests.
    mosaic::core::LayerEffects fx;
    fx.gradientOverlay.enabled = true;
    vec::Gradient g;
    g.stops = {{0.0, mosaic::common::ColorF{0, 1, 0, 1}}, {1.0, mosaic::common::ColorF{0, 0, 1, 1}}};
    fx.gradientOverlay.paint = g;
    const ExtrudeOverlay ov = buildExtrudeOverlay(fx, mesh, e, mesh.designBounds, 2.0, true);
    REQUIRE_FALSE(ov.empty());

    const Affine2D place = Affine2D::translation(30.0, 30.0);
    ImageF cpu(90, 90), viaGpu(90, 90);
    renderExtrudeMeshF(cpu, mesh, e, place, true, nullptr, &ov);
    REQUIRE(gpu->render(viaGpu, mesh, e, place, true, nullptr, &ov));
    const Diff d = compare(cpu, viaGpu);
    REQUIRE(d.cpuCovered > 200);
    CHECK(d.coverDiff <= d.cpuCovered / 50 + 8);
    CHECK(d.meanAbs < 0.02);

    // Wrap-to-sides parity: the walls sample the map through the same interpolated UVs.
    Extrude wrap = e;
    wrap.overlayWrapSides = true;
    const ExtrudeOverlay ovWrap = buildExtrudeOverlay(fx, mesh, wrap, mesh.designBounds, 2.0, true);
    ImageF cpuWrap(90, 90), gpuWrap(90, 90);
    renderExtrudeMeshF(cpuWrap, mesh, wrap, place, true, nullptr, &ovWrap);
    REQUIRE(gpu->render(gpuWrap, mesh, wrap, place, true, nullptr, &ovWrap));
    const Diff dw = compare(cpuWrap, gpuWrap);
    CHECK(dw.coverDiff <= dw.cpuCovered / 50 + 8);
    CHECK(dw.meanAbs < 0.02);
    CHECK(err.empty());
}

// A drag-frame budget probe, not a test (doctest::skip -- run with `-tc="*drag-frame*" -ns`): the
// S30-d gizmos re-render a headline-sized solid per pointer event, so this prints where that
// frame's milliseconds actually go (mesh rebuild vs CPU raster vs GPU raster + readback).
TEST_CASE("bench: extrude drag-frame costs" * doctest::skip()) {
    // A headline-ish block: 8 "glyphs", each a 64-pt rounded outer + 48-pt hole, ~90px tall,
    // laid out across ~700px -- about "Chrome!" at 120pt worth of geometry.
    std::vector<GlyphSolidInput> solids;
    for (int g = 0; g < 8; ++g) {
        vec::Contour outer, hole;
        const double cx = 20.0 + g * 90.0, cy = 50.0;
        for (int i = 0; i < 64; ++i) {
            const double a = 2.0 * M_PI * i / 64.0;
            outer.points.push_back({cx + 40.0 * std::cos(a) * (1.0 + 0.15 * std::cos(3 * a)),
                                    cy + 45.0 * std::sin(a) * (1.0 + 0.1 * std::sin(2 * a))});
        }
        outer.closed = true;
        for (int i = 0; i < 48; ++i) {
            const double a = 2.0 * M_PI * i / 48.0;
            hole.points.push_back({cx + 18.0 * std::cos(a), cy + 20.0 * std::sin(a)});
        }
        hole.closed = true;
        solids.push_back({{outer, hole}, 0});
    }
    Extrude e;
    e.depth = 30.0f;
    e.perspective = 10.0f;
    e.bevelFront.size = 3.0f;
    e.bevelFront.profile = Bevel::Profile::Round;
    e.orientation = Quat::fromAxisAngle({0.3, 0.9, 0.1}, 0.5);

    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    constexpr int N = 20;
    auto t0 = Clock::now();
    ExtrudeMesh mesh;
    for (int i = 0; i < N; ++i) mesh = buildExtrudeMesh(solids, e);
    auto t1 = Clock::now();
    std::printf("mesh build:        %7.2f ms  (%zu verts, %zu tris)\n", ms(t0, t1) / N,
                mesh.vertices.size(), mesh.indices.size() / 3);

    const Affine2D place = Affine2D::translation(20.0, 20.0);
    {
        auto ta = Clock::now();
        for (int i = 0; i < N; ++i) {
            ImageF cpu(780, 140);
            renderExtrudeMeshF(cpu, mesh, e, place, true);
        }
        auto tb = Clock::now();
        std::printf("CPU raster (2x2):  %7.2f ms  (780x140)\n", ms(ta, tb) / N);
    }
    {
        // Canvas-ish scale: the same solid seen at 200%% zoom.
        const Affine2D big = Affine2D::scaling(2.0, 2.0) * place;
        auto ta = Clock::now();
        for (int i = 0; i < N; ++i) {
            ImageF cpu(1560, 280);
            renderExtrudeMeshF(cpu, mesh, e, big, true);
        }
        auto tb = Clock::now();
        std::printf("CPU raster @200%%:  %7.2f ms  (1560x280)\n", ms(ta, tb) / N);
    }

    std::string err;
    auto gpu = mosaic::render::ExtrudeGpu::create(false, err);
    if (!gpu) {
        std::printf("GPU: unavailable (%s)\n", err.c_str());
        return;
    }
    {
        ImageF warm(780, 140);
        REQUIRE(gpu->render(warm, mesh, e, place, true));  // pipeline + mesh upload warm-up
        auto ta = Clock::now();
        for (int i = 0; i < N; ++i) {
            ImageF img(780, 140);
            REQUIRE(gpu->render(img, mesh, e, place, true));
        }
        auto tb = Clock::now();
        std::printf("GPU raster+readbk: %7.2f ms  (780x140, cached mesh)\n", ms(ta, tb) / N);
        const Affine2D big = Affine2D::scaling(2.0, 2.0) * place;
        ta = Clock::now();
        for (int i = 0; i < N; ++i) {
            ImageF img(1560, 280);
            REQUIRE(gpu->render(img, mesh, e, big, true));
        }
        tb = Clock::now();
        std::printf("GPU raster @200%%:  %7.2f ms  (1560x280, cached mesh)\n", ms(ta, tb) / N);
    }
}
