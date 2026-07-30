#include "formats/bmp.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// libmosaicformats' BMP codec, encode and decode (docs/export-system-plan.md §10 item 5,
// docs/formats-curated.md). Standalone: this file includes formats/bmp.hpp and nothing else of
// Mosaic's, which is what §11's "tested standalone, no Mosaic deps" means in practice.
//
// BMP's traps, all of which have a case below: rows padded to four bytes, bottom-up by default,
// 16-bit BI_RGB being 5-5-5 rather than 5-6-5, colour masks living in two different places
// depending on the header version, and a 32-bit fourth byte that means alpha in some files and
// padding in others.
namespace {

using mosaicfmt::Bitmap;
using mosaicfmt::BmpOptions;

Bitmap pattern(std::uint32_t w, std::uint32_t h) {
    Bitmap b(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = b.at(x, y);
            p[0] = static_cast<std::uint8_t>(x * 31 + 7);
            p[1] = static_cast<std::uint8_t>(y * 47 + 13);
            p[2] = static_cast<std::uint8_t>((x * 3 + y * 5) & 0xFF);
            p[3] = static_cast<std::uint8_t>((x + y) % 2 == 0 ? 255 : (x * 19) & 0xFF);
        }
    return b;
}

Bitmap opaquePattern(std::uint32_t w, std::uint32_t h) {
    Bitmap b = pattern(w, h);
    for (std::size_t i = 3; i < b.rgba.size(); i += 4)
        b.rgba[i] = 255;
    return b;
}

int maxDelta(const Bitmap& a, const Bitmap& b) {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size())
        return 1000;
    int worst = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        const int d = static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i]);
        worst = worst > (d < 0 ? -d : d) ? worst : (d < 0 ? -d : d);
    }
    return worst;
}

[[nodiscard]] std::uint32_t u32At(const std::vector<std::uint8_t>& bytes, std::size_t off) {
    return static_cast<std::uint32_t>(bytes[off]) |
           (static_cast<std::uint32_t>(bytes[off + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[off + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[off + 3]) << 24);
}

} // namespace

TEST_CASE("BMP round-trips 32-bit transparency exactly at V4 and V5") {
    for (const int version : {4, 5}) {
        // 13 x 5 on purpose: 13 * 4 bytes is not a multiple of four for the narrower depths, so the
        // row padding is exercised rather than accidentally satisfied.
        const Bitmap src = pattern(13, 5);
        BmpOptions opts;
        opts.headerVersion = version;
        CHECK(mosaicfmt::bmpWritesAlpha(opts));
        const auto bytes = mosaicfmt::encodeBmp(src.view(), opts);
        REQUIRE(bytes.has_value());
        CHECK((*bytes)[0] == 'B');
        CHECK((*bytes)[1] == 'M');
        CHECK(u32At(*bytes, 2) == bytes->size());  // the file-size field must be the file's size
        CHECK(u32At(*bytes, 14) == (version == 4 ? 108u : 124u));

        const auto back = mosaicfmt::decodeBmp(bytes->data(), bytes->size());
        REQUIRE(back.has_value());
        CHECK(back->width == src.width);
        CHECK(back->height == src.height);
        CHECK(back->rgba == src.rgba);
    }
}

TEST_CASE("BMP's row order does not change the picture") {
    const Bitmap src = pattern(6, 4);
    BmpOptions bottomUp;
    BmpOptions topDown;
    topDown.topDown = true;
    const auto a = mosaicfmt::encodeBmp(src.view(), bottomUp);
    const auto b = mosaicfmt::encodeBmp(src.view(), topDown);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a != *b);  // the bytes differ...
    const auto backA = mosaicfmt::decodeBmp(a->data(), a->size());
    const auto backB = mosaicfmt::decodeBmp(b->data(), b->size());
    REQUIRE(backA.has_value());
    REQUIRE(backB.has_value());
    CHECK(backA->rgba == backB->rgba);  // ... and the pictures do not
    CHECK(backA->rgba == src.rgba);
}

TEST_CASE("BMP without an alpha-capable spelling flattens onto the matte") {
    Bitmap src(2, 1);
    std::uint8_t* clear = src.at(0, 0);
    clear[0] = 250;  // colour hiding under a fully transparent pixel
    clear[1] = 250;
    clear[2] = 250;
    clear[3] = 0;
    std::uint8_t* solid = src.at(1, 0);
    solid[0] = 12;
    solid[1] = 34;
    solid[2] = 56;
    solid[3] = 255;

    // 24-bit, and a V3 header at 32-bit: neither can carry an alpha mask, so both must composite.
    BmpOptions bgr24;
    bgr24.depth = BmpOptions::Depth::Bgr24;
    bgr24.matte = mosaicfmt::Rgb8{9, 8, 7};
    BmpOptions v3;
    v3.headerVersion = 3;
    v3.matte = mosaicfmt::Rgb8{9, 8, 7};
    CHECK_FALSE(mosaicfmt::bmpWritesAlpha(bgr24));
    CHECK_FALSE(mosaicfmt::bmpWritesAlpha(v3));

    for (const BmpOptions& opts : {bgr24, v3}) {
        const auto bytes = mosaicfmt::encodeBmp(src.view(), opts);
        REQUIRE(bytes.has_value());
        const auto back = mosaicfmt::decodeBmp(bytes->data(), bytes->size());
        REQUIRE(back.has_value());
        CHECK(back->at(0, 0)[0] == 9);
        CHECK(back->at(0, 0)[1] == 8);
        CHECK(back->at(0, 0)[2] == 7);
        CHECK(back->at(0, 0)[3] == 255);
        CHECK(back->at(1, 0)[0] == 12);
        CHECK(back->at(1, 0)[3] == 255);
    }
}

TEST_CASE("BMP's 16-bit mode keeps the picture within its own precision") {
    const Bitmap src = opaquePattern(9, 3);
    BmpOptions opts;
    opts.depth = BmpOptions::Depth::Rgb565;
    const auto bytes = mosaicfmt::encodeBmp(src.view(), opts);
    REQUIRE(bytes.has_value());
    const auto back = mosaicfmt::decodeBmp(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    // Five bits of red and blue is one part in 32: a channel lands on the nearest multiple of
    // 255/31, so it can move by up to 8 codes. The assertion is that it moves by no MORE than its
    // own quantisation -- a wrong mask or a wrong scale shows up as a far larger error, or as a
    // colour cast, which a per-channel bound catches.
    CHECK(maxDelta(*back, src) <= 8);
}

TEST_CASE("BMP's indexed mode round-trips an exact palette, compressed or not") {
    // Six flat colours: fewer than the palette holds, so the quantisation is exact and the
    // round trip can be asserted with ==. Long horizontal runs, so RLE has something to do.
    const mosaicfmt::Rgba8 palette[6] = {{220, 30, 40, 255},   {30, 220, 40, 255},
                                         {40, 30, 220, 255},   {250, 250, 250, 255},
                                         {12, 12, 12, 255},    {90, 90, 90, 255}};
    const std::uint32_t w = 17, h = 6;
    std::vector<std::uint8_t> indices(static_cast<std::size_t>(w) * h);
    Bitmap expected(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint8_t index = static_cast<std::uint8_t>((x / 5 + y) % 6);
            indices[static_cast<std::size_t>(y) * w + x] = index;
            std::uint8_t* p = expected.at(x, y);
            p[0] = palette[index].r;
            p[1] = palette[index].g;
            p[2] = palette[index].b;
            p[3] = 255;
        }

    mosaicfmt::IndexedView view;
    view.indices = indices.data();
    view.palette = palette;
    view.paletteSize = 6;
    view.width = w;
    view.height = h;
    REQUIRE(view.valid());

    BmpOptions plain;
    BmpOptions compressed;
    compressed.rle = true;
    const auto flat = mosaicfmt::encodeBmpIndexed(view, plain);
    const auto rle = mosaicfmt::encodeBmpIndexed(view, compressed);
    REQUIRE(flat.has_value());
    REQUIRE(rle.has_value());
    CHECK(rle->size() < flat->size());  // flat colour in long runs is what RLE is for

    for (const auto* file : {&flat, &rle}) {
        const auto back = mosaicfmt::decodeBmp((*file)->data(), (*file)->size());
        REQUIRE(back.has_value());
        CHECK(back->width == w);
        CHECK(back->height == h);
        CHECK(back->rgba == expected.rgba);
    }
}

TEST_CASE("BMP refuses an indexed source whose pixels outrun its palette") {
    const mosaicfmt::Rgba8 palette[2] = {{0, 0, 0, 255}, {255, 255, 255, 255}};
    const std::uint8_t indices[4] = {0, 1, 7, 0};  // 7 has no entry
    mosaicfmt::IndexedView view;
    view.indices = indices;
    view.palette = palette;
    view.paletteSize = 2;
    view.width = 2;
    view.height = 2;
    std::string error;
    CHECK_FALSE(mosaicfmt::encodeBmpIndexed(view, {}, &error).has_value());
    CHECK_FALSE(error.empty());
}

TEST_CASE("BMP embeds an ICC profile only where there is room for one") {
    const Bitmap src = opaquePattern(4, 4);
    std::vector<std::uint8_t> profile(300, 0xAB);
    BmpOptions v5;
    v5.icc = profile;
    BmpOptions v4;
    v4.headerVersion = 4;
    v4.icc = profile;

    const auto withProfile = mosaicfmt::encodeBmp(src.view(), v5);
    const auto without = mosaicfmt::encodeBmp(src.view(), v4);
    REQUIRE(withProfile.has_value());
    REQUIRE(without.has_value());
    CHECK(withProfile->size() > without->size() + 200u);
    // bV5CSType == 'MBED' and the profile sits where the header says it does.
    CHECK(u32At(*withProfile, 14 + 56) == 0x4D424544u);
    const std::uint32_t offsetFromHeader = u32At(*withProfile, 14 + 108 + 4);
    const std::uint32_t declaredSize = u32At(*withProfile, 14 + 108 + 8);
    CHECK(declaredSize == profile.size());
    REQUIRE(14u + offsetFromHeader + declaredSize == withProfile->size());
    CHECK((*withProfile)[14 + offsetFromHeader] == 0xAB);
    // ... and the picture still decodes with the profile stapled on the end.
    const auto back = mosaicfmt::decodeBmp(withProfile->data(), withProfile->size());
    REQUIRE(back.has_value());
    CHECK(back->rgba == src.rgba);
}

TEST_CASE("BMP rejects the structural lies and survives every truncation") {
    const Bitmap src = pattern(11, 7);
    const auto bytes = mosaicfmt::encodeBmp(src.view());
    REQUIRE(bytes.has_value());

    std::string error;
    SUBCASE("a header size that cannot be a header") {
        std::vector<std::uint8_t> broken = *bytes;
        broken[14] = 99;  // no DIB header is 99 bytes long
        CHECK_FALSE(mosaicfmt::decodeBmp(broken.data(), broken.size(), &error).has_value());
        CHECK_FALSE(error.empty());
    }
    SUBCASE("a compression whose payload is another codec entirely") {
        std::vector<std::uint8_t> broken = *bytes;
        broken[14 + 16] = 4;  // BI_JPEG
        CHECK_FALSE(mosaicfmt::decodeBmp(broken.data(), broken.size(), &error).has_value());
    }
    SUBCASE("a pixel-data offset outside the file") {
        std::vector<std::uint8_t> broken = *bytes;
        broken[10] = 0xFF;
        broken[11] = 0xFF;
        broken[12] = 0xFF;
        broken[13] = 0x7F;
        CHECK_FALSE(mosaicfmt::decodeBmp(broken.data(), broken.size(), &error).has_value());
    }
    SUBCASE("an enormous declared size") {
        std::vector<std::uint8_t> broken = *bytes;
        broken[14 + 4] = 0x00;  // width = 0x10000000
        broken[14 + 5] = 0x00;
        broken[14 + 6] = 0x00;
        broken[14 + 7] = 0x10;
        CHECK_FALSE(mosaicfmt::decodeBmp(broken.data(), broken.size(), &error).has_value());
    }
    SUBCASE("every truncation, on exact-size allocations") {
        for (std::size_t n = 0; n < bytes->size(); ++n) {
            const std::vector<std::uint8_t> cut(bytes->begin(),
                                                bytes->begin() + static_cast<std::ptrdiff_t>(n));
            const auto decoded = mosaicfmt::decodeBmp(cut.data(), cut.size(), &error);
            if (decoded.has_value())
                CHECK(decoded->consistent());
        }
        const auto whole = mosaicfmt::decodeBmp(bytes->data(), bytes->size());
        REQUIRE(whole.has_value());
        CHECK(whole->rgba == src.rgba);
    }
}

TEST_CASE("BMP survives every truncation of a run-length compressed file") {
    // The RLE path has its own row cursor and its own end conditions, so it needs its own sweep.
    const mosaicfmt::Rgba8 palette[3] = {{10, 20, 30, 255}, {40, 50, 60, 255}, {70, 80, 90, 255}};
    std::vector<std::uint8_t> indices(64 * 8);
    for (std::size_t i = 0; i < indices.size(); ++i)
        indices[i] = static_cast<std::uint8_t>((i / 9) % 3);
    mosaicfmt::IndexedView view;
    view.indices = indices.data();
    view.palette = palette;
    view.paletteSize = 3;
    view.width = 64;
    view.height = 8;
    BmpOptions opts;
    opts.rle = true;
    const auto bytes = mosaicfmt::encodeBmpIndexed(view, opts);
    REQUIRE(bytes.has_value());
    for (std::size_t n = 0; n < bytes->size(); ++n) {
        const std::vector<std::uint8_t> cut(bytes->begin(),
                                            bytes->begin() + static_cast<std::ptrdiff_t>(n));
        std::string error;
        const auto decoded = mosaicfmt::decodeBmp(cut.data(), cut.size(), &error);
        if (decoded.has_value())
            CHECK(decoded->consistent());
    }
}
