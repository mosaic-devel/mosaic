#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/blend_mode.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/fill.hpp"
#include "core/layer.hpp"
#include "core/vector/paint.hpp"
#include "render/region_fill.hpp"

#include <cmath>
#include <doctest/doctest.h>
#include <vector>

using namespace mosaic;
using core::BlendMode;
using core::Document;
using core::FillCommand;
using core::LayerId;
using core::RasterLayer;

namespace {

common::Image solid(std::uint32_t w, std::uint32_t h, common::Color8 c) {
    common::Image img(w, h);
    img.fill(c);
    return img;
}

common::Color8 pixel(const common::Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

// Byte-equality within ±1 to absorb float-rounding at the 8-bit boundary.
void checkNear(common::Color8 got, common::Color8 want, int tol = 1) {
    CHECK(std::abs(int(got.r) - int(want.r)) <= tol);
    CHECK(std::abs(int(got.g) - int(want.g)) <= tol);
    CHECK(std::abs(int(got.b) - int(want.b)) <= tol);
    CHECK(std::abs(int(got.a) - int(want.a)) <= tol);
}

} // namespace

TEST_CASE("computeFill: opaque fill with no selection covers the whole region") {
    const common::Image region = solid(3, 2, {0, 0, 0, 255});
    const common::Image out =
        render::computeFill(region, /*coverage=*/{}, {200, 50, 25, 255}, BlendMode::Normal, 1.0f,
                            /*protectAlpha=*/false);
    REQUIRE(out.width == 3);
    REQUIRE(out.height == 2);
    for (std::uint32_t y = 0; y < 2; ++y)
        for (std::uint32_t x = 0; x < 3; ++x)
            checkNear(pixel(out, x, y), {200, 50, 25, 255}, 0);
}

TEST_CASE("computeFill: opacity blends the fill toward the backdrop") {
    const common::Image region = solid(1, 1, {0, 0, 0, 255}); // opaque black
    const common::Image out =
        render::computeFill(region, {}, {255, 0, 0, 255}, BlendMode::Normal, 0.5f, false);
    checkNear(pixel(out, 0, 0), {128, 0, 0, 255}); // half-red over black, still opaque
}

TEST_CASE("computeFill: coverage masks which pixels are filled") {
    const common::Image region = solid(2, 1, {0, 0, 0, 255});
    const std::vector<std::uint8_t> coverage = {255, 0}; // only the left pixel selected
    const common::Image out =
        render::computeFill(region, coverage, {255, 0, 0, 255}, BlendMode::Normal, 1.0f, false);
    checkNear(pixel(out, 0, 0), {255, 0, 0, 255}, 0); // filled
    checkNear(pixel(out, 1, 0), {0, 0, 0, 255}, 0);   // untouched
}

TEST_CASE("computeFill: Protect Alpha leaves transparent pixels alone and preserves alpha") {
    common::Image region(2, 1);
    region.rgba = {0, 0, 0, 0, 255, 255, 255, 128}; // px0 transparent, px1 half-alpha white
    const common::Image out =
        render::computeFill(region, {}, {255, 0, 0, 255}, BlendMode::Normal, 1.0f,
                            /*protectAlpha=*/true);
    checkNear(pixel(out, 0, 0), {0, 0, 0, 0}, 0);  // transparent pixel untouched
    checkNear(pixel(out, 1, 0), {255, 0, 0, 128}); // recoloured, original alpha kept
}

TEST_CASE("computeFill: blend mode is applied (Multiply red over white = red)") {
    const common::Image region = solid(1, 1, {255, 255, 255, 255});
    const common::Image out =
        render::computeFill(region, {}, {255, 0, 0, 255}, BlendMode::Multiply, 1.0f, false);
    checkNear(pixel(out, 0, 0), {255, 0, 0, 255});
}

TEST_CASE("FillCommand patches just its region and undoes byte-exact, named \"Fill\"") {
    Document doc(4, 4);
    core::Layer& l = doc.root().addOnTop(doc.makeRaster("L"));
    const LayerId id = l.id();
    auto* raster = l.as<RasterLayer>();
    REQUIRE(raster != nullptr);
    raster->image() = solid(4, 4, {0, 0, 0, 255}); // opaque black

    // A 2x2 red patch dropped at layer-local (1,1).
    const common::Image patch = solid(2, 2, {255, 0, 0, 255});
    auto cmd = std::make_unique<FillCommand>(id, patch, 1, 1);
    CHECK(cmd->name() == "Fill");
    doc.commands().push(std::move(cmd));

    checkNear(pixel(raster->image(), 1, 1), {255, 0, 0, 255}, 0); // inside the patch
    checkNear(pixel(raster->image(), 2, 2), {255, 0, 0, 255}, 0);
    checkNear(pixel(raster->image(), 0, 0), {0, 0, 0, 255}, 0); // outside untouched
    checkNear(pixel(raster->image(), 3, 3), {0, 0, 0, 255}, 0);

    doc.commands().undo();
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 4; ++x)
            checkNear(pixel(raster->image(), x, y), {0, 0, 0, 255}, 0);
}

// ---- computeFillPaint (gradient / pattern) ---------------------------------------------------

TEST_CASE("computeFillPaint: a SolidPaint matches the solid computeFill") {
    const common::Image region = solid(3, 2, {10, 20, 30, 255});
    const core::vec::Paint paint =
        core::vec::SolidPaint{{200 / 255.0f, 50 / 255.0f, 25 / 255.0f, 1.0f}};
    const common::Image out = render::computeFillPaint(region, /*coverage=*/{}, paint, /*ox=*/0,
                                                       /*oy=*/0, BlendMode::Normal, 1.0f,
                                                       /*protectAlpha=*/false, /*antialias=*/true);
    const common::Image ref =
        render::computeFill(region, {}, {200, 50, 25, 255}, BlendMode::Normal, 1.0f, false);
    for (std::uint32_t y = 0; y < 2; ++y)
        for (std::uint32_t x = 0; x < 3; ++x)
            checkNear(pixel(out, x, y), pixel(ref, x, y), 0);
}

TEST_CASE("computeFillPaint: a linear gradient runs across the region (dark left -> light right)") {
    core::vec::Gradient g;
    g.type = core::vec::GradientType::Linear;
    g.stops = {{0.0, {0, 0, 0, 1}}, {1.0, {1, 1, 1, 1}}}; // black -> white
    g.transform = common::Affine2D::identity();           // unit-space == the normalised region
    const common::Image region = solid(4, 1, {128, 128, 128, 255});
    const common::Image out =
        render::computeFillPaint(region, {}, g, 0, 0, BlendMode::Normal, 1.0f, false, true);
    // The gradient spans the region: leftmost pixel is dark, rightmost is light, monotonic, opaque.
    CHECK(pixel(out, 0, 0).r < 64);
    CHECK(pixel(out, 3, 0).r > 192);
    CHECK(pixel(out, 0, 0).r < pixel(out, 1, 0).r);
    CHECK(pixel(out, 1, 0).r < pixel(out, 2, 0).r);
    CHECK(pixel(out, 2, 0).r < pixel(out, 3, 0).r);
    for (std::uint32_t x = 0; x < 4; ++x)
        CHECK(pixel(out, x, 0).a == 255);
}

TEST_CASE("computeFillPaint: a rotated linear gradient changes direction (90 deg -> vertical)") {
    // Rotate the default (identity) linear transform 90 deg about the content-box centre -- the
    // same R_center * default the gradient flyout's Direction dial builds.
    using common::Affine2D;
    const Affine2D rot = Affine2D::translation(0.5, 0.5) * Affine2D::rotation(M_PI / 2.0) *
                         Affine2D::translation(-0.5, -0.5);
    core::vec::Gradient g;
    g.type = core::vec::GradientType::Linear;
    g.stops = {{0.0, {0, 0, 0, 1}}, {1.0, {1, 1, 1, 1}}}; // black -> white
    g.transform = rot;                                    // 90 deg from the left->right default
    const common::Image region = solid(2, 2, {128, 128, 128, 255});
    const common::Image out =
        render::computeFillPaint(region, {}, g, 0, 0, BlendMode::Normal, 1.0f, false, true);
    // The ramp now runs along Y: a row is constant, and it varies (and is monotonic) down the
    // columns.
    CHECK(pixel(out, 0, 0).r == pixel(out, 1, 0).r); // same row -> same colour
    CHECK(pixel(out, 0, 1).r == pixel(out, 1, 1).r);
    CHECK(pixel(out, 0, 0).r < pixel(out, 0, 1).r); // top darker, bottom lighter (vertical ramp)
}

TEST_CASE("computeFillPaint: a checker pattern tiles the region (both colours appear, crisp)") {
    core::vec::ProceduralPattern pp;
    pp.kind = core::vec::ProceduralPattern::Kind::Checker;
    pp.fg = {1, 0, 0, 1}; // red
    pp.bg = {0, 0, 1, 1}; // blue
    pp.scale = 2.0f;      // 2px cells over a 4x4 region -> alternating blocks
    const common::Image region = solid(4, 4, {0, 0, 0, 255});
    const common::Image out = render::computeFillPaint(region, {}, core::vec::Pattern{pp}, 0, 0,
                                                       BlendMode::Normal, 1.0f, false,
                                                       /*antialias=*/false);
    // Crisp (AA off): every pixel is one of the two opaque pattern colours, and BOTH appear (it
    // tiled).
    bool sawRed = false, sawBlue = false;
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 4; ++x) {
            const common::Color8 c = pixel(out, x, y);
            CHECK(c.a == 255);
            const bool red = std::abs(int(c.r) - 255) <= 1 && c.g <= 1 && c.b <= 1;
            const bool blue = c.r <= 1 && c.g <= 1 && std::abs(int(c.b) - 255) <= 1;
            CHECK((red || blue));
            sawRed = sawRed || red;
            sawBlue = sawBlue || blue;
        }
    CHECK(sawRed);
    CHECK(sawBlue);
}

TEST_CASE("computeFillPaint: pattern tiling is keyed to the layer origin, not the region") {
    core::vec::ProceduralPattern pp;
    pp.kind = core::vec::ProceduralPattern::Kind::Checker;
    pp.fg = {1, 0, 0, 1};
    pp.bg = {0, 0, 1, 1};
    pp.scale = 2.0f;
    const common::Image region = solid(2, 2, {0, 0, 0, 255});
    // The same region filled one whole cell-pair (2*scale) further along x lands on the same
    // phase...
    const common::Image a = render::computeFillPaint(region, {}, core::vec::Pattern{pp}, 0, 0,
                                                     BlendMode::Normal, 1.0f, false, false);
    const common::Image same = render::computeFillPaint(region, {}, core::vec::Pattern{pp}, 4, 0,
                                                        BlendMode::Normal, 1.0f, false, false);
    // ...while one cell (scale) over flips the phase (the tiling moved under the region).
    const common::Image shifted = render::computeFillPaint(region, {}, core::vec::Pattern{pp}, 2, 0,
                                                           BlendMode::Normal, 1.0f, false, false);
    checkNear(pixel(same, 0, 0), pixel(a, 0, 0), 0);
    CHECK(
        (pixel(shifted, 0, 0).r != pixel(a, 0, 0).r || pixel(shifted, 0, 0).b != pixel(a, 0, 0).b));
}

TEST_CASE("computeFillPaint: coverage + Protect Alpha behave as in computeFill") {
    common::Image region(2, 1);
    region.rgba = {0, 0, 0, 0, 255, 255, 255, 128}; // px0 transparent, px1 half-alpha white
    const core::vec::Paint red = core::vec::SolidPaint{{1, 0, 0, 1}};
    const common::Image out =
        render::computeFillPaint(region, {}, red, 0, 0, BlendMode::Normal, 1.0f,
                                 /*protectAlpha=*/true, true);
    checkNear(pixel(out, 0, 0), {0, 0, 0, 0}, 0);  // transparent pixel untouched
    checkNear(pixel(out, 1, 0), {255, 0, 0, 128}); // recoloured, original alpha kept
}

// ---- core::bucketFillCoverage (the S21 flood engine) -----------------------------------------

namespace {

// A 1-row image from a list of colours (one per column) -- handy for reasoning about the flood.
common::Image row(std::initializer_list<common::Color8> cols) {
    common::Image img(static_cast<std::uint32_t>(cols.size()), 1);
    std::size_t i = 0;
    for (common::Color8 c : cols) {
        img.rgba[i * 4 + 0] = c.r;
        img.rgba[i * 4 + 1] = c.g;
        img.rgba[i * 4 + 2] = c.b;
        img.rgba[i * 4 + 3] = c.a;
        ++i;
    }
    return img;
}

} // namespace

TEST_CASE("bucketFillCoverage: a bad seed / empty image yields no coverage") {
    const common::Image img = solid(4, 4, {10, 20, 30, 255});
    CHECK(core::bucketFillCoverage(img, -1, 0, {}).empty());
    CHECK(core::bucketFillCoverage(img, 0, 4, {}).empty());
    CHECK(core::bucketFillCoverage(common::Image{}, 0, 0, {}).empty());
}

TEST_CASE("bucketFillCoverage: a uniform image floods solid everywhere") {
    const common::Image img = solid(4, 4, {10, 20, 30, 255});
    const std::vector<std::uint8_t> cov = core::bucketFillCoverage(img, 1, 1, {});
    REQUIRE(cov.size() == 16);
    for (std::uint8_t c : cov)
        CHECK(c == 255);
}

TEST_CASE("bucketFillCoverage: contiguous stops at a colour boundary; global crosses it") {
    // A, B, A along one row: the two A cells are the same colour but not connected.
    const common::Color8 a{255, 0, 0, 255}, b{0, 255, 0, 255};
    const common::Image img = row({a, b, a});

    core::FillParams contig;
    contig.tolerance = 0.1;
    contig.antialias = false;
    contig.contiguous = true;
    const std::vector<std::uint8_t> cc = core::bucketFillCoverage(img, 0, 0, contig);
    REQUIRE(cc.size() == 3);
    CHECK(cc[0] == 255); // the clicked A cell
    CHECK(cc[1] == 0);   // B is out of tolerance -> the flood stops
    CHECK(cc[2] == 0);   // the far A cell is disconnected

    core::FillParams global = contig;
    global.contiguous = false;
    const std::vector<std::uint8_t> gc = core::bucketFillCoverage(img, 0, 0, global);
    REQUIRE(gc.size() == 3);
    CHECK(gc[0] == 255);
    CHECK(gc[1] == 0);   // B still fails the colour test
    CHECK(gc[2] == 255); // ... but the disconnected A now fills (connectivity ignored)
}

TEST_CASE("bucketFillCoverage: tolerance widens what the flood accepts") {
    const common::Image img = row({{100, 100, 100, 255}, {130, 130, 130, 255}});
    core::FillParams p;
    p.antialias = false;
    p.sampleAlpha = false; // grayscale metric collapses to |delta|/255

    p.tolerance = 0.05; // |30|/255 = 0.1176 > 0.05 -> the neighbour is rejected
    const std::vector<std::uint8_t> tight = core::bucketFillCoverage(img, 0, 0, p);
    CHECK(tight[0] == 255);
    CHECK(tight[1] == 0);

    p.tolerance = 0.20; // now within tolerance -> both fill
    const std::vector<std::uint8_t> loose = core::bucketFillCoverage(img, 0, 0, p);
    CHECK(loose[0] == 255);
    CHECK(loose[1] == 255);
}

TEST_CASE("bucketFillCoverage: anti-alias feathers the outer boundary, hard mode does not") {
    // px1 sits just past tolerance (|28|/255 = 0.1098, T = 0.10, band = 0.02) -> it earns a sub-128
    // ramp with AA on, and nothing with AA off. Only 0/255 ever appear in the hard mask.
    const common::Image img = row({{100, 100, 100, 255}, {128, 128, 128, 255}});
    core::FillParams p;
    p.tolerance = 0.10;
    p.sampleAlpha = false;

    p.antialias = false;
    const std::vector<std::uint8_t> hard = core::bucketFillCoverage(img, 0, 0, p);
    CHECK(hard[0] == 255);
    CHECK(hard[1] == 0);

    p.antialias = true;
    const std::vector<std::uint8_t> aa = core::bucketFillCoverage(img, 0, 0, p);
    CHECK(aa[0] == 255);
    CHECK(aa[1] > 0);   // feathered
    CHECK(aa[1] < 128); // ... but never bright enough to read as a solid extra pixel
}

TEST_CASE("bucketFillCoverage: sampling alpha keeps a transparent click in its own region") {
    // px0 transparent, px1 opaque red: with alpha in the metric they are far apart, so a click on
    // the transparent cell does not bleed into the opaque one.
    const common::Image img = row({{0, 0, 0, 0}, {255, 0, 0, 255}});
    core::FillParams p;
    p.tolerance = 0.1;
    p.antialias = false;
    p.sampleAlpha = true;
    const std::vector<std::uint8_t> cov = core::bucketFillCoverage(img, 0, 0, p);
    CHECK(cov[0] == 255);
    CHECK(cov[1] == 0);
}
