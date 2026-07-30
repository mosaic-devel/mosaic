#include "formats/qoi.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// libmosaicformats' QOI codec (docs/export-system-plan.md §10 item 5, docs/formats-curated.md).
//
// Tested STANDALONE, and the include list above is the assertion: §11 asks for the library to be
// exercised "with no Mosaic deps", so this file knows about formats/qoi.hpp and nothing else.
//
// QOI is the one format here worth pinning BYTE FOR BYTE against the published specification
// (Dominic Szablewski, 2021) rather than only round-tripping: the codec is a predictor plus a
// 64-entry hash index, so a bug in either shows up as a file that our own decoder reads back
// perfectly and no other decoder can open at all. A hand-computed chunk stream is the only test
// that catches that.
namespace {

using mosaicfmt::Bitmap;

Bitmap image(std::uint32_t w, std::uint32_t h,
             const std::vector<std::array<std::uint8_t, 4>>& pixels) {
    Bitmap b(w, h);
    for (std::size_t i = 0; i < pixels.size() && i * 4 + 3 < b.rgba.size(); ++i)
        for (std::size_t c = 0; c < 4; ++c)
            b.rgba[i * 4 + c] = pixels[i][c];
    return b;
}

// Varied colour AND alpha, including fully transparent pixels whose colour is not black -- the
// case that separates an exact codec from a nearly exact one.
Bitmap pattern(std::uint32_t w, std::uint32_t h) {
    Bitmap b(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = b.at(x, y);
            p[0] = static_cast<std::uint8_t>(x * 37 + 3);
            p[1] = static_cast<std::uint8_t>(y * 53 + 11);
            p[2] = static_cast<std::uint8_t>((x + y) * 17);
            p[3] = static_cast<std::uint8_t>((x * y) % 256);
        }
    return b;
}

std::vector<std::uint8_t> header(std::uint32_t w, std::uint32_t h, std::uint8_t channels,
                                 std::uint8_t colorspace) {
    return {'q',
            'o',
            'i',
            'f',
            static_cast<std::uint8_t>(w >> 24),
            static_cast<std::uint8_t>((w >> 16) & 0xFF),
            static_cast<std::uint8_t>((w >> 8) & 0xFF),
            static_cast<std::uint8_t>(w & 0xFF),
            static_cast<std::uint8_t>(h >> 24),
            static_cast<std::uint8_t>((h >> 16) & 0xFF),
            static_cast<std::uint8_t>((h >> 8) & 0xFF),
            static_cast<std::uint8_t>(h & 0xFF),
            channels,
            colorspace};
}

void appendEndMarker(std::vector<std::uint8_t>& bytes) {
    bytes.insert(bytes.end(), {0, 0, 0, 0, 0, 0, 0, 1});
}

} // namespace

TEST_CASE("QOI writes the specification's own chunks for a hand-computed pattern") {
    // Four pixels: two opaque blacks (which equal the codec's initial running pixel, so they are a
    // RUN), then (1,1,1,255) -- a +1 step in each channel, i.e. a DIFF -- then a repeat of it,
    // which the last-pixel flush has to emit as a one-long RUN.
    const Bitmap img = image(4, 1,
                             {{{0, 0, 0, 255}}, {{0, 0, 0, 255}}, {{1, 1, 1, 255}},
                              {{1, 1, 1, 255}}});
    const auto bytes = mosaicfmt::encodeQoi(img.view());
    REQUIRE(bytes.has_value());

    std::vector<std::uint8_t> expected = header(4, 1, 4, 0);
    expected.push_back(0xC1);  // QOI_OP_RUN, length 2 (stored as length - 1)
    expected.push_back(0x7F);  // QOI_OP_DIFF, +1/+1/+1 -> 0x40 | 3<<4 | 3<<2 | 3
    expected.push_back(0xC0);  // QOI_OP_RUN, length 1
    appendEndMarker(expected);
    CHECK(*bytes == expected);
}

TEST_CASE("QOI reaches back into its hash index rather than re-sending a colour") {
    // (1,1,1,255) hashes to slot 4 and (9,9,9,255) to slot 60, so the fourth pixel is an INDEX
    // chunk -- the one chunk kind a round-trip test cannot distinguish from an RGB chunk.
    const Bitmap img = image(4, 1,
                             {{{0, 0, 0, 255}}, {{1, 1, 1, 255}}, {{9, 9, 9, 255}},
                              {{1, 1, 1, 255}}});
    const auto bytes = mosaicfmt::encodeQoi(img.view());
    REQUIRE(bytes.has_value());

    std::vector<std::uint8_t> expected = header(4, 1, 4, 0);
    expected.push_back(0xC0);  // RUN of 1: the first pixel equals the initial opaque black
    expected.push_back(0x7F);  // DIFF +1/+1/+1
    expected.push_back(0xA8);  // LUMA, green +8 -> 0x80 | (8 + 32)
    expected.push_back(0x88);  // ... with both differences zero -> (0+8)<<4 | (0+8)
    expected.push_back(0x04);  // INDEX, slot 4
    appendEndMarker(expected);
    CHECK(*bytes == expected);
}

TEST_CASE("QOI's hash index starts at transparent black, not at opaque black") {
    // The spec initialises the index to {0,0,0,0} while the running pixel starts at {0,0,0,255}.
    // Getting that pair wrong produces files only our own decoder can read, and no round-trip test
    // notices -- so this reads an INDEX chunk that points at a slot nothing has written yet.
    std::vector<std::uint8_t> file = header(1, 1, 4, 0);
    file.push_back(0x00);  // INDEX, slot 0
    appendEndMarker(file);
    const auto decoded = mosaicfmt::decodeQoi(file.data(), file.size());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->consistent());
    CHECK(decoded->rgba[0] == 0);
    CHECK(decoded->rgba[1] == 0);
    CHECK(decoded->rgba[2] == 0);
    CHECK(decoded->rgba[3] == 0);
}

TEST_CASE("QOI splits a long run into 62-pixel chunks") {
    Bitmap flat(200, 1);
    for (std::size_t i = 0; i < flat.rgba.size(); i += 4) {
        flat.rgba[i] = flat.rgba[i + 1] = flat.rgba[i + 2] = 7;
        flat.rgba[i + 3] = 255;
    }
    const auto bytes = mosaicfmt::encodeQoi(flat.view());
    REQUIRE(bytes.has_value());
    // One 2-byte LUMA for the first pixel, then 199 repeats as 62 + 62 + 62 + 13 = four RUN bytes.
    CHECK(bytes->size() == 14u + 2u + 4u + 8u);

    const auto back = mosaicfmt::decodeQoi(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    CHECK(back->rgba == flat.rgba);
}

TEST_CASE("QOI round-trips colour and transparency bit-exactly") {
    for (const std::uint32_t side : {1u, 2u, 7u, 61u}) {
        const Bitmap src = pattern(side, side + 3u);
        const auto bytes = mosaicfmt::encodeQoi(src.view());
        REQUIRE(bytes.has_value());
        const auto back = mosaicfmt::decodeQoi(bytes->data(), bytes->size());
        REQUIRE(back.has_value());
        CHECK(back->width == src.width);
        CHECK(back->height == src.height);
        CHECK(back->rgba == src.rgba);  // lossless means exactly this, so assert exactly this
    }
}

TEST_CASE("QOI in three channels flattens transparency onto the matte") {
    Bitmap src(2, 1);
    std::uint8_t* opaque = src.at(0, 0);
    opaque[0] = 10;
    opaque[1] = 20;
    opaque[2] = 30;
    opaque[3] = 255;
    std::uint8_t* clear = src.at(1, 0);
    clear[0] = 200;  // colour under a fully transparent pixel: it must not survive
    clear[1] = 200;
    clear[2] = 200;
    clear[3] = 0;

    mosaicfmt::QoiOptions opts;
    opts.channels = 3;
    opts.matte = mosaicfmt::Rgb8{4, 5, 6};
    const auto bytes = mosaicfmt::encodeQoi(src.view(), opts);
    REQUIRE(bytes.has_value());
    CHECK((*bytes)[12] == 3);  // the header's channel count

    const auto back = mosaicfmt::decodeQoi(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    CHECK(back->at(0, 0)[0] == 10);
    CHECK(back->at(0, 0)[3] == 255);
    CHECK(back->at(1, 0)[0] == 4);
    CHECK(back->at(1, 0)[1] == 5);
    CHECK(back->at(1, 0)[2] == 6);
    CHECK(back->at(1, 0)[3] == 255);
}

TEST_CASE("QOI refuses a header it cannot honour") {
    std::string error;
    const auto bad = [&](std::vector<std::uint8_t> file) {
        error.clear();
        appendEndMarker(file);
        const auto decoded = mosaicfmt::decodeQoi(file.data(), file.size(), &error);
        CHECK_FALSE(decoded.has_value());
        CHECK_FALSE(error.empty());
    };
    bad(header(1, 1, 5, 0));  // neither 3 nor 4 channels
    bad(header(1, 1, 4, 2));  // an unknown colourspace tag
    bad(header(0, 1, 4, 0));  // a zero dimension
    bad(header(1, 0, 4, 0));
    bad(header(0x40000000u, 0x40000000u, 4, 0));  // an area no allocation should be attempted for

    std::vector<std::uint8_t> wrongMagic = header(1, 1, 4, 0);
    wrongMagic[0] = 'x';
    bad(wrongMagic);

    // The end marker is mandatory, and it is the format's only integrity signal.
    std::vector<std::uint8_t> unterminated = header(1, 1, 4, 0);
    unterminated.push_back(0xC0);
    CHECK_FALSE(mosaicfmt::decodeQoi(unterminated.data(), unterminated.size(), &error).has_value());
}

TEST_CASE("QOI survives every truncation of a real file") {
    const Bitmap src = pattern(23, 9);
    const auto bytes = mosaicfmt::encodeQoi(src.view());
    REQUIRE(bytes.has_value());
    for (std::size_t n = 0; n < bytes->size(); ++n) {
        // A fresh exact-size allocation per length, so ASan sees any read past the end.
        const std::vector<std::uint8_t> cut(bytes->begin(),
                                            bytes->begin() + static_cast<std::ptrdiff_t>(n));
        std::string error;
        const auto decoded = mosaicfmt::decodeQoi(cut.data(), cut.size(), &error);
        if (decoded.has_value())
            CHECK(decoded->consistent());
        else
            CHECK_FALSE(error.empty());
    }
    // ... and the untruncated original still reads.
    const auto whole = mosaicfmt::decodeQoi(bytes->data(), bytes->size());
    REQUIRE(whole.has_value());
    CHECK(whole->rgba == src.rgba);
}

TEST_CASE("QOI survives a scrambled chunk stream") {
    const Bitmap src = pattern(31, 11);
    auto bytes = mosaicfmt::encodeQoi(src.view());
    REQUIRE(bytes.has_value());
    for (std::size_t i = 14; i < bytes->size(); ++i)
        (*bytes)[i] = static_cast<std::uint8_t>((i * 7919u) & 0xFFu);
    std::string error;
    const auto decoded = mosaicfmt::decodeQoi(bytes->data(), bytes->size(), &error);
    if (decoded.has_value())
        CHECK(decoded->consistent());
}
