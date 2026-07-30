#include <doctest/doctest.h>

#include <cstddef>
#include <fstream>
#include <string>

#include "common/image.hpp"
#include "render/render.hpp"
#include "render/window_renderer.hpp"

using namespace mosaic;

TEST_CASE("CPU solid render fills every pixel exactly") {
    const common::Color8 c{10, 20, 30, 40};
    const render::RenderResult r = render::renderSolid(5, 7, c, render::Backend::Cpu);
    REQUIRE(r.ok);
    CHECK(r.usedBackend == render::Backend::Cpu);
    CHECK(r.image.width == 5);
    CHECK(r.image.height == 7);
    REQUIRE(r.image.rgba.size() == std::size_t{5} * 7 * 4);

    bool allMatch = true;
    for (std::size_t i = 0; i < r.image.pixelCount(); ++i) {
        const std::size_t p = i * 4;
        if (r.image.rgba[p] != c.r || r.image.rgba[p + 1] != c.g || r.image.rgba[p + 2] != c.b ||
            r.image.rgba[p + 3] != c.a) {
            allMatch = false;
            break;
        }
    }
    CHECK(allMatch);
}

TEST_CASE("Auto backend always succeeds") {
    const render::RenderResult r = render::renderSolid(16, 16, {0, 0, 0, 255}, render::Backend::Auto);
    CHECK(r.ok);
}

TEST_CASE("GPU solid render matches CPU and is validation-clean") {
    const common::Color8 c{200, 100, 50, 255};
    const render::RenderResult gpu = render::renderSolid(8, 8, c, render::Backend::Gpu);
    if (!gpu.ok) {
        MESSAGE("GPU unavailable, skipping GPU checks: " << gpu.error);
        return;
    }
    CHECK(gpu.usedBackend == render::Backend::Gpu);
    CHECK(gpu.validationErrors == 0);

    const render::RenderResult cpu = render::renderSolid(8, 8, c, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    CHECK(gpu.image == cpu.image);  // GPU clear is bit-exact with the CPU fill
}

TEST_CASE("GPU compute fill matches CPU and is validation-clean") {
    const common::Color8 c{12, 240, 77, 255};  // 13x9 is not a multiple of the 8x8 workgroup
    const render::RenderResult gpu = render::renderSolid(13, 9, c, render::Backend::GpuCompute);
    if (!gpu.ok) {
        MESSAGE("GPU compute unavailable, skipping: " << gpu.error);
        return;
    }
    CHECK(gpu.usedBackend == render::Backend::GpuCompute);
    CHECK(gpu.validationErrors == 0);

    const render::RenderResult cpu = render::renderSolid(13, 9, c, render::Backend::Cpu);
    REQUIRE(cpu.ok);
    CHECK(gpu.image == cpu.image);  // compute shader output is bit-exact with the CPU fill
}

TEST_CASE("fitCentered letterboxes a document onto the canvas") {
    using render::BlitRect;
    // Exact fit, upscaling 2x.
    CHECK(render::fitCentered(100, 100, 200, 200) == BlitRect{0, 0, 200, 200});
    // Wider canvas: limited by width-vs-height min scale (2x), centered horizontally.
    CHECK(render::fitCentered(100, 100, 400, 200) == BlitRect{100, 0, 200, 200});
    // Landscape doc in a square canvas: centered vertically.
    CHECK(render::fitCentered(200, 100, 200, 200) == BlitRect{0, 50, 200, 100});
    // Downscale a large doc to fit.
    CHECK(render::fitCentered(400, 400, 200, 200) == BlitRect{0, 0, 200, 200});
    // Degenerate inputs yield an empty rect.
    CHECK(render::fitCentered(0, 100, 200, 200) == BlitRect{});
    CHECK(render::fitCentered(100, 100, 0, 200) == BlitRect{});
}

TEST_CASE("writePpm produces a P6 file") {
    const render::RenderResult r = render::renderSolid(4, 4, {1, 2, 3, 255}, render::Backend::Cpu);
    REQUIRE(r.ok);
    const std::string path = "test_render_out.ppm";
    std::string err;
    REQUIRE(common::writePpm(r.image, path, &err));

    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    char hdr[2] = {0, 0};
    in.read(hdr, 2);
    CHECK(hdr[0] == 'P');
    CHECK(hdr[1] == '6');
}
