#include "common/image.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "io/io.hpp"
#include "core/layer.hpp"
#include "render/compositor.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

// The S18-b image readers (io::loadImage): magic-byte format sniffing + PNG/JPEG decode into 8-bit
// straight-alpha RGBA. Fixtures live in tests/fixtures/ (a 4x4 RGBA PNG with a transparent corner;
// a 4x4 flat-colour JPEG). MOSAIC_FIXTURE_DIR is the absolute path, injected by CMake.
namespace {

using mosaic::common::Image;
using mosaic::io::ImageFormat;
using mosaic::io::loadImage;
using mosaic::io::sniffImageFormat;

std::string fixture(const char* name) { return std::string(MOSAIC_FIXTURE_DIR) + "/" + name; }

mosaic::common::Color8 px(const Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

} // namespace

TEST_CASE("sniffImageFormat keys off the magic bytes, not the extension") {
    const std::uint8_t png[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const std::uint8_t jpg[3] = {0xFF, 0xD8, 0xFF};
    const std::uint8_t junk[4] = {'h', 'i', '!', '\n'};
    CHECK(sniffImageFormat(png, sizeof png) == ImageFormat::Png);
    CHECK(sniffImageFormat(jpg, sizeof jpg) == ImageFormat::Jpeg);
    CHECK(sniffImageFormat(junk, sizeof junk) == ImageFormat::Unknown);
    CHECK(sniffImageFormat(nullptr, 0) == ImageFormat::Unknown);
    CHECK(sniffImageFormat(png, 4) == ImageFormat::Unknown); // too short for the full signature
}

TEST_CASE("loadImage decodes a PNG, preserving alpha") {
    std::string err;
    auto img = loadImage(fixture("sample.png"), &err);
    REQUIRE_MESSAGE(img.has_value(), err);
    CHECK(img->width == 4);
    CHECK(img->height == 4);
    CHECK(px(*img, 0, 0).a == 0);            // the transparent corner survives
    CHECK(px(*img, 1, 1) == mosaic::common::Color8{200, 100, 50, 255}); // exact (lossless)
}

TEST_CASE("loadImage decodes a JPEG as opaque RGBA") {
    std::string err;
    auto img = loadImage(fixture("sample.jpg"), &err);
    REQUIRE_MESSAGE(img.has_value(), err);
    CHECK(img->width == 4);
    CHECK(img->height == 4);
    const auto c = px(*img, 2, 2);
    CHECK(c.a == 255); // JPEG has no alpha -> fully opaque
    // Lossy, but a flat colour decodes close to the source.
    CHECK(std::abs(static_cast<int>(c.r) - 200) <= 6);
    CHECK(std::abs(static_cast<int>(c.g) - 100) <= 6);
    CHECK(std::abs(static_cast<int>(c.b) - 50) <= 6);
}

TEST_CASE("loadImage rejects a missing file and a non-image") {
    std::string err;
    CHECK_FALSE(loadImage(fixture("does-not-exist.png"), &err).has_value());
    CHECK_FALSE(err.empty());
    err.clear();
    CHECK_FALSE(loadImage(fixture("../CMakeLists.txt"), &err).has_value()); // a real, non-image file
    CHECK_FALSE(err.empty());
}

// ---- Milestone 1: the first ENCODER (savePng), for Quick Export -> PNG --------------------------

namespace {
using mosaic::io::PngSaveOptions;
using mosaic::io::savePng;

// A per-test scratch PNG path under the OS temp dir, removed when the test ends.
struct TempPng {
    std::filesystem::path path;
    explicit TempPng(const char* stem)
        : path(std::filesystem::temp_directory_path() / (std::string("mosaic_test_") + stem + ".png")) {}
    ~TempPng() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    [[nodiscard]] std::string str() const { return path.string(); }
};

// A small RGBA image with a varied colour + alpha pattern (every channel exercised).
Image makePattern(std::uint32_t w, std::uint32_t h) {
    Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            img.rgba[p + 0] = static_cast<std::uint8_t>(x * 37 + 3);
            img.rgba[p + 1] = static_cast<std::uint8_t>(y * 53 + 11);
            img.rgba[p + 2] = static_cast<std::uint8_t>((x + y) * 17);
            img.rgba[p + 3] = static_cast<std::uint8_t>((x * y) % 256); // includes 0 (transparent)
        }
    return img;
}
} // namespace

TEST_CASE("savePng writes a PNG that reloads bit-exact (lossless round-trip)") {
    const Image src = makePattern(13, 7);
    TempPng out("savepng_roundtrip");

    std::string err;
    REQUIRE_MESSAGE(savePng(src, out.str(), {}, &err), err);

    auto reloaded = loadImage(out.str(), &err);
    REQUIRE_MESSAGE(reloaded.has_value(), err);
    CHECK(reloaded->width == src.width);
    CHECK(reloaded->height == src.height);
    CHECK(*reloaded == src); // PNG is lossless: straight-alpha RGBA survives byte-for-byte
}

TEST_CASE("savePng honours the compression + interlace options (still lossless)") {
    const Image src = makePattern(20, 20);
    std::string err;

    // By reference: PngSaveOptions carries an EmbeddedMetadata (heap-owning) since M4, so a
    // by-value loop variable is a copy -Wrange-loop-construct rightly objects to.
    for (const PngSaveOptions& o : {PngSaveOptions{.compression = 0, .interlace = false},
                                    PngSaveOptions{.compression = 9, .interlace = true}}) {
        TempPng out(o.interlace ? "opts_interlaced" : "opts_store");
        REQUIRE_MESSAGE(savePng(src, out.str(), o, &err), err);
        auto reloaded = loadImage(out.str(), &err);
        REQUIRE_MESSAGE(reloaded.has_value(), err);
        CHECK(*reloaded == src); // options change the bytes on disk, never the decoded pixels
    }
}

TEST_CASE("savePng rejects an empty image and an unwritable path") {
    std::string err;
    CHECK_FALSE(savePng(Image{}, "/tmp/should-not-be-written.png", {}, &err));
    CHECK_FALSE(err.empty());
    err.clear();
    CHECK_FALSE(savePng(makePattern(2, 2), "/no/such/directory/x.png", {}, &err));
    CHECK_FALSE(err.empty());
}

// The Milestone-1 pipeline end to end: open a real image, edit it through the command stack,
// flatten it with the compositor, export the flatten as PNG, then reload -- the reload must match
// the composited bytes exactly (a lossless encoder can't be what loses anything).
TEST_CASE("open -> edit -> composite -> export PNG round-trips") {
    namespace core = mosaic::core;
    namespace render = mosaic::render;

    std::string err;
    auto opened = loadImage(fixture("sample.png"), &err);
    REQUIRE_MESSAGE(opened.has_value(), err);

    core::Document doc(opened->width, opened->height);
    auto bg = doc.makeRaster("sample", opened->width, opened->height);
    bg->image() = *opened;
    doc.root().addOnTop(std::move(bg));

    // An edit through the stack (the "edit" in open->edit->export): drop an opaque 2x2 patch on top.
    auto patch = doc.makeRaster("patch", opened->width, opened->height);
    patch->image().rgba[0] = 255; // (0,0) fully opaque red
    patch->image().rgba[3] = 255;
    doc.commands().push(
        std::make_unique<core::AddLayerCommand>(doc.root().id(), 1, std::move(patch)));
    CHECK(doc.dirty());

    const render::CompositeResult flat = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(flat.ok);

    TempPng out("pipeline");
    REQUIRE_MESSAGE(savePng(flat.image, out.str(), {}, &err), err);
    auto reloaded = loadImage(out.str(), &err);
    REQUIRE_MESSAGE(reloaded.has_value(), err);
    CHECK(*reloaded == flat.image);
}

TEST_CASE("probeImageDimensions reads PNG/JPEG headers without decoding") {
    auto png = mosaic::io::probeImageDimensions(fixture("sample.png"));
    REQUIRE(png.has_value());
    CHECK(png->width == 4);
    CHECK(png->height == 4);

    auto jpg = mosaic::io::probeImageDimensions(fixture("sample.jpg"));
    REQUIRE(jpg.has_value());
    CHECK(jpg->width == 4);
    CHECK(jpg->height == 4);

    CHECK(!mosaic::io::probeImageDimensions(fixture("no_such_file.png")).has_value());
    // A non-image (this very source file) sniffs Unknown.
    CHECK(!mosaic::io::probeImageDimensions(__FILE__).has_value());
}
