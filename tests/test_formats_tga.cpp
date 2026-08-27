#include "formats/formats.hpp"
#include "formats/tga.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// libmosaicformats' TGA codec (docs/export-system-plan.md §10 item 5, docs/formats-curated.md),
// standalone -- no Mosaic dependency in the include list.
//
// The two things worth testing hard: TGA has NO MAGIC NUMBER, so identification is a plausibility
// judgement that must not claim other people's files; and the MEANING of its alpha channel lives in
// the v2 extension area, not in the pixels, so a premultiplied file has to come back straight.
namespace {

using mosaicfmt::Bitmap;
using mosaicfmt::TgaOptions;

Bitmap pattern(std::uint32_t w, std::uint32_t h) {
    Bitmap b(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = b.at(x, y);
            p[0] = static_cast<std::uint8_t>(x * 29 + 5);
            p[1] = static_cast<std::uint8_t>(y * 41 + 17);
            p[2] = static_cast<std::uint8_t>((x * 7 + y * 11) & 0xFF);
            // Long horizontal runs in every fourth row, so the RLE coder has both cases to code.
            p[3] = y % 4 == 0 ? std::uint8_t{255} : static_cast<std::uint8_t>((x * 23) & 0xFF);
            if (y % 4 == 0) {
                p[0] = 200;
                p[1] = 100;
                p[2] = 50;
            }
        }
    return b;
}

int maxDelta(const Bitmap& a, const Bitmap& b) {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size())
        return 1000;
    int worst = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        const int d = static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i]);
        const int m = d < 0 ? -d : d;
        worst = worst > m ? worst : m;
    }
    return worst;
}

} // namespace

TEST_CASE("TGA round-trips 32-bit colour and transparency exactly, coded or not") {
    const Bitmap src = pattern(19, 8);
    TgaOptions plain;
    plain.rle = false;
    TgaOptions coded;
    const auto flat = mosaicfmt::encodeTga(src.view(), plain);
    const auto rle = mosaicfmt::encodeTga(src.view(), coded);
    REQUIRE(flat.has_value());
    REQUIRE(rle.has_value());
    CHECK((*flat)[2] == 2);   // uncompressed truecolour
    CHECK((*rle)[2] == 10);   // run-length encoded truecolour
    CHECK(rle->size() < flat->size());

    for (const auto* file : {&flat, &rle}) {
        const auto back = mosaicfmt::decodeTga((*file)->data(), (*file)->size());
        REQUIRE(back.has_value());
        CHECK(back->width == src.width);
        CHECK(back->height == src.height);
        CHECK(back->rgba == src.rgba);
    }
}

TEST_CASE("TGA writes a v2 footer and states what its alpha means") {
    const Bitmap src = pattern(8, 4);
    const auto bytes = mosaicfmt::encodeTga(src.view());
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() > 26);
    CHECK(std::memcmp(bytes->data() + bytes->size() - 18, "TRUEVISION-XFILE.", 17) == 0);
    // The attributes type is the extension area's last byte: 3 = straight (unassociated) alpha.
    const std::size_t footer = bytes->size() - 26;
    const std::uint32_t extension = static_cast<std::uint32_t>((*bytes)[footer]) |
                                    (static_cast<std::uint32_t>((*bytes)[footer + 1]) << 8) |
                                    (static_cast<std::uint32_t>((*bytes)[footer + 2]) << 16) |
                                    (static_cast<std::uint32_t>((*bytes)[footer + 3]) << 24);
    REQUIRE(extension + 495u <= bytes->size());
    CHECK((*bytes)[extension + 494u] == 3);
    CHECK(((*bytes)[17] & 0x0Fu) == 8u);   // eight attribute bits
    CHECK(((*bytes)[17] & 0x20u) != 0u);   // origin at the top by default
}

TEST_CASE("TGA's premultiplied option really premultiplies, and comes back straight") {
    // No fully transparent pixel here on purpose: premultiplying at alpha 0 legitimately destroys
    // the colour, so a whole-image comparison would be measuring the format, not the codec. The
    // alpha-0 case is asserted on its own below.
    Bitmap src(2, 1);
    const std::uint8_t colours[2][4] = {{200, 100, 50, 128}, {255, 255, 255, 255}};
    for (std::uint32_t x = 0; x < 2; ++x)
        for (std::size_t c = 0; c < 4; ++c)
            src.at(x, 0)[c] = colours[x][c];

    TgaOptions opts;
    opts.alpha = TgaOptions::AlphaAttributes::Premultiplied;
    opts.rle = false;
    const auto bytes = mosaicfmt::encodeTga(src.view(), opts);
    REQUIRE(bytes.has_value());
    const std::size_t footer = bytes->size() - 26;
    const std::uint32_t extension = static_cast<std::uint32_t>((*bytes)[footer]) |
                                    (static_cast<std::uint32_t>((*bytes)[footer + 1]) << 8) |
                                    (static_cast<std::uint32_t>((*bytes)[footer + 2]) << 16) |
                                    (static_cast<std::uint32_t>((*bytes)[footer + 3]) << 24);
    CHECK((*bytes)[extension + 494u] == 4);  // associated alpha, and the pixels agree

    // The stored blue byte of the half-transparent pixel is 50 * 128/255 ~= 25, not 50.
    const std::size_t firstPixel = 18;
    CHECK((*bytes)[firstPixel] < 30);

    const auto back = mosaicfmt::decodeTga(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    // Un-premultiplying at alpha 128 costs about one part in 128 -- two codes at the top of the
    // range. A decoder that forgot to un-premultiply would be out by a factor of two.
    CHECK(maxDelta(*back, src) <= 3);
    CHECK(back->at(0, 0)[3] == 128);

    // And a fully transparent pixel keeps its alpha, whatever premultiplication did to its colour.
    Bitmap hole(1, 1);
    hole.at(0, 0)[0] = 80;
    hole.at(0, 0)[1] = 90;
    hole.at(0, 0)[2] = 100;
    hole.at(0, 0)[3] = 0;
    const auto holeBytes = mosaicfmt::encodeTga(hole.view(), opts);
    REQUIRE(holeBytes.has_value());
    const auto holeBack = mosaicfmt::decodeTga(holeBytes->data(), holeBytes->size());
    REQUIRE(holeBack.has_value());
    CHECK(holeBack->at(0, 0)[3] == 0);
}

TEST_CASE("TGA's 24-bit and alpha-less modes flatten onto the matte") {
    Bitmap src(2, 1);
    std::uint8_t* clear = src.at(0, 0);
    clear[0] = clear[1] = clear[2] = 240;
    clear[3] = 0;
    std::uint8_t* solid = src.at(1, 0);
    solid[0] = 11;
    solid[1] = 22;
    solid[2] = 33;
    solid[3] = 255;

    TgaOptions bgr24;
    bgr24.depth = TgaOptions::Depth::Bgr24;
    bgr24.matte = mosaicfmt::Rgb8{1, 2, 3};
    TgaOptions ignored;
    ignored.alpha = TgaOptions::AlphaAttributes::Ignored;
    ignored.matte = mosaicfmt::Rgb8{1, 2, 3};

    for (const TgaOptions& opts : {bgr24, ignored}) {
        const auto bytes = mosaicfmt::encodeTga(src.view(), opts);
        REQUIRE(bytes.has_value());
        const auto back = mosaicfmt::decodeTga(bytes->data(), bytes->size());
        REQUIRE(back.has_value());
        CHECK(back->at(0, 0)[0] == 1);
        CHECK(back->at(0, 0)[1] == 2);
        CHECK(back->at(0, 0)[2] == 3);
        CHECK(back->at(0, 0)[3] == 255);
        CHECK(back->at(1, 0)[0] == 11);
        CHECK(back->at(1, 0)[3] == 255);
    }
}

TEST_CASE("TGA's 16-bit mode keeps one transparency bit and five bits a channel") {
    Bitmap src(4, 2);
    for (std::uint32_t y = 0; y < 2; ++y)
        for (std::uint32_t x = 0; x < 4; ++x) {
            std::uint8_t* p = src.at(x, y);
            p[0] = static_cast<std::uint8_t>(x * 60);
            p[1] = 128;
            p[2] = static_cast<std::uint8_t>(y * 200);
            p[3] = x == 0 ? std::uint8_t{0} : std::uint8_t{255};  // one hard hole
        }
    TgaOptions opts;
    opts.depth = TgaOptions::Depth::Bgra16;
    const auto bytes = mosaicfmt::encodeTga(src.view(), opts);
    REQUIRE(bytes.has_value());
    CHECK((*bytes)[16] == 16);           // pixel depth
    CHECK(((*bytes)[17] & 0x0Fu) == 1u); // one attribute bit

    const auto back = mosaicfmt::decodeTga(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    for (std::uint32_t y = 0; y < 2; ++y) {
        CHECK(back->at(0, y)[3] == 0);
        for (std::uint32_t x = 1; x < 4; ++x) {
            CHECK(back->at(x, y)[3] == 255);
            const int d = static_cast<int>(back->at(x, y)[0]) - static_cast<int>(src.at(x, y)[0]);
            CHECK((d < 0 ? -d : d) <= 8);  // five bits of red
        }
    }
}

TEST_CASE("TGA's stored row order does not change the picture") {
    const Bitmap src = pattern(7, 5);
    TgaOptions bottom;
    bottom.topDown = false;
    const auto bytes = mosaicfmt::encodeTga(src.view(), bottom);
    REQUIRE(bytes.has_value());
    CHECK(((*bytes)[17] & 0x20u) == 0u);
    const auto back = mosaicfmt::decodeTga(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    CHECK(back->rgba == src.rgba);
}

TEST_CASE("sniff() identifies a real TGA and does not claim other people's bytes") {
    const Bitmap src = pattern(9, 3);
    const auto bytes = mosaicfmt::encodeTga(src.view());
    REQUIRE(bytes.has_value());
    CHECK(mosaicfmt::sniff(bytes->data(), bytes->size()) == mosaicfmt::Codec::Tga);

    // Prose, a PNG, and a JPEG must not be mistaken for a header full of small integers.
    const std::uint8_t text[32] = "the quick brown fox jumped over";
    const std::uint8_t png[16] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                                  0,    0,   0,   13,  'I',  'H',  'D',  'R'};
    const std::uint8_t jpeg[8] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F'};
    CHECK(mosaicfmt::sniff(text, sizeof text) == mosaicfmt::Codec::None);
    CHECK(mosaicfmt::sniff(png, sizeof png) == mosaicfmt::Codec::None);
    CHECK(mosaicfmt::sniff(jpeg, sizeof jpeg) == mosaicfmt::Codec::None);
    CHECK(mosaicfmt::sniff(nullptr, 0) == mosaicfmt::Codec::None);
}

TEST_CASE("TGA rejects what it cannot decode and survives every truncation") {
    const Bitmap src = pattern(13, 6);
    const auto rle = mosaicfmt::encodeTga(src.view());
    REQUIRE(rle.has_value());
    std::string error;

    SUBCASE("an image type nobody writes") {
        std::vector<std::uint8_t> broken = *rle;
        broken[2] = 32;  // Huffman + delta + run-length colour-mapped
        CHECK_FALSE(mosaicfmt::decodeTga(broken.data(), broken.size(), &error).has_value());
        CHECK_FALSE(error.empty());
    }
    SUBCASE("a colour-mapped image with no colour map") {
        std::vector<std::uint8_t> broken = *rle;
        broken[2] = 1;
        CHECK_FALSE(mosaicfmt::decodeTga(broken.data(), broken.size(), &error).has_value());
    }
    SUBCASE("an id field longer than the file") {
        std::vector<std::uint8_t> broken = *rle;
        broken[0] = 255;
        broken.resize(60);
        CHECK_FALSE(mosaicfmt::decodeTga(broken.data(), broken.size(), &error).has_value());
    }
    SUBCASE("every truncation, on exact-size allocations") {
        for (std::size_t n = 0; n < rle->size(); ++n) {
            const std::vector<std::uint8_t> cut(rle->begin(),
                                                rle->begin() + static_cast<std::ptrdiff_t>(n));
            const auto decoded = mosaicfmt::decodeTga(cut.data(), cut.size(), &error);
            if (decoded.has_value())
                CHECK(decoded->consistent());
        }
    }
    SUBCASE("a scrambled payload") {
        std::vector<std::uint8_t> broken = *rle;
        for (std::size_t i = 18; i < broken.size(); ++i)
            broken[i] = static_cast<std::uint8_t>((i * 7919u) & 0xFFu);
        const auto decoded = mosaicfmt::decodeTga(broken.data(), broken.size(), &error);
        if (decoded.has_value())
            CHECK(decoded->consistent());
    }
}

TEST_CASE("TGA: a pixel depth its image type cannot have is refused, not read past") {
    // ⚠ REGRESSION, found by fuzzing (libFuzzer + ASan): a heap-buffer-overflow READ reachable by
    // opening a .tga file.
    //
    // The depth check accepted {8, 15, 16, 24, 32} for EVERY image type, while readPixel bounded
    // its reads by bytesPerPixel = (pixelDepth + 7) / 8. For a TRUE-COLOUR image those disagree: a
    // type-2 header declaring depth 8 gives bytesPerPixel == 1, so the reader checked that one byte
    // was available and then read three -- b, g, r -- off the end of the buffer. Seventeen distinct
    // fuzz witnesses, all of them this; the header now rejects the combination up front, which is
    // what the format says anyway (true-colour is 15/16/24/32, greyscale is 8/16).
    //
    // The assertion is "declined", not "did not crash": in a normal build the old code read out of
    // bounds and usually got away with it, which is exactly why it survived the hand-written
    // negative tests below.
    const auto header = [](std::uint8_t imageType, std::uint8_t depth) {
        std::vector<std::uint8_t> f(18 + 64, 0);
        f[2] = imageType;
        f[12] = 4; // width  = 4
        f[14] = 1; // height = 1
        f[16] = depth;
        return f;
    };
    std::string err;

    SUBCASE("true-colour cannot be 8-bit") {
        const auto f = header(2, 8);
        CHECK_FALSE(mosaicfmt::decodeTga(f.data(), f.size(), &err).has_value());
        CHECK(err.find("true-colour") != std::string::npos);
    }
    SUBCASE("nor can its RLE twin") {
        const auto f = header(10, 8);
        CHECK_FALSE(mosaicfmt::decodeTga(f.data(), f.size(), &err).has_value());
    }
    SUBCASE("greyscale cannot be 24- or 32-bit") {
        for (std::uint8_t depth : {std::uint8_t{24}, std::uint8_t{32}}) {
            const auto f = header(3, depth);
            CHECK_FALSE(mosaicfmt::decodeTga(f.data(), f.size(), &err).has_value());
        }
    }
    SUBCASE("and the legal combinations still decode") {
        // The fix must not have narrowed what a real file may be: round-trip each depth the format
        // does allow, through the encoder that writes it.
        const Bitmap src = pattern(4, 3);
        for (TgaOptions::Depth d :
             {TgaOptions::Depth::Bgra32, TgaOptions::Depth::Bgr24, TgaOptions::Depth::Bgra16}) {
            TgaOptions opt;
            opt.depth = d;
            const auto bytes = mosaicfmt::encodeTga(
                mosaicfmt::ImageView{src.rgba.data(), src.width, src.height}, opt, &err);
            REQUIRE(bytes.has_value());
            const auto back = mosaicfmt::decodeTga(bytes->data(), bytes->size(), &err);
            CHECK(back.has_value());
        }
    }
}
