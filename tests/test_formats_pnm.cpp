#include "formats/pnm.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// libmosaicformats' Netpbm codecs -- PBM, PGM, PPM and PAM (docs/export-system-plan.md §10 item 5,
// docs/formats-curated.md). Standalone: formats/pnm.hpp and nothing else of Mosaic's.
//
// The family's subtleties, each with a case below: PBM stores 1 = BLACK while everything else
// stores 0 = black; comments are legal anywhere whitespace is, including between the width and the
// height; exactly one whitespace byte separates the header from raw samples; and PAM is the only
// variant that carries transparency, which is what makes it the one §0 records us as exceeding
// GIMP on.
namespace {

using mosaicfmt::Bitmap;
using mosaicfmt::PnmOptions;

Bitmap pattern(std::uint32_t w, std::uint32_t h, bool opaque) {
    Bitmap b(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = b.at(x, y);
            p[0] = static_cast<std::uint8_t>(x * 17 + 9);
            p[1] = static_cast<std::uint8_t>(y * 61 + 3);
            p[2] = static_cast<std::uint8_t>((x * y * 7) & 0xFF);
            p[3] = opaque ? std::uint8_t{255} : static_cast<std::uint8_t>((x * 33 + y) & 0xFF);
        }
    return b;
}

std::vector<std::uint8_t> bytesOf(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string headOf(const std::vector<std::uint8_t>& file, std::size_t n) {
    return std::string(file.begin(),
                       file.begin() + static_cast<std::ptrdiff_t>(n < file.size() ? n : file.size()));
}

[[nodiscard]] std::uint8_t luma601(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return static_cast<std::uint8_t>((299u * r + 587u * g + 114u * b + 500u) / 1000u);
}

} // namespace

TEST_CASE("PPM round-trips colour exactly, raw or plain") {
    const Bitmap src = pattern(11, 4, /*opaque=*/true);
    for (const bool ascii : {false, true}) {
        PnmOptions opts;
        opts.ascii = ascii;
        const auto bytes = mosaicfmt::encodePnm(src.view(), opts);
        REQUIRE(bytes.has_value());
        CHECK(headOf(*bytes, 2) == (ascii ? "P3" : "P6"));
        const auto back = mosaicfmt::decodePnm(bytes->data(), bytes->size());
        REQUIRE(back.has_value());
        CHECK(back->width == src.width);
        CHECK(back->height == src.height);
        CHECK(back->rgba == src.rgba);
    }
}

TEST_CASE("PAM is the variant that keeps transparency") {
    const Bitmap src = pattern(9, 5, /*opaque=*/false);
    PnmOptions opts;
    opts.variant = PnmOptions::Variant::Pam;
    const auto bytes = mosaicfmt::encodePnm(src.view(), opts);
    REQUIRE(bytes.has_value());
    CHECK(headOf(*bytes, 2) == "P7");
    const std::string header = headOf(*bytes, 80);
    CHECK(header.find("DEPTH 4") != std::string::npos);
    CHECK(header.find("TUPLTYPE RGB_ALPHA") != std::string::npos);

    const auto back = mosaicfmt::decodePnm(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    CHECK(back->rgba == src.rgba);
}

TEST_CASE("PAM's greyscale tuples carry the right number of samples") {
    const Bitmap src = pattern(6, 3, /*opaque=*/false);
    PnmOptions opts;
    opts.variant = PnmOptions::Variant::Pam;
    opts.pamTuple = PnmOptions::PamTuple::GrayscaleAlpha;
    const auto bytes = mosaicfmt::encodePnm(src.view(), opts);
    REQUIRE(bytes.has_value());
    CHECK(headOf(*bytes, 80).find("DEPTH 2") != std::string::npos);

    const auto back = mosaicfmt::decodePnm(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    for (std::uint32_t y = 0; y < src.height; ++y)
        for (std::uint32_t x = 0; x < src.width; ++x) {
            const std::uint8_t* s = src.at(x, y);
            const std::uint8_t* d = back->at(x, y);
            const std::uint8_t grey = luma601(s[0], s[1], s[2]);
            CHECK(d[0] == grey);
            CHECK(d[1] == grey);
            CHECK(d[2] == grey);
            CHECK(d[3] == s[3]);
        }
}

TEST_CASE("PGM writes luminance and PPM's alpha lands on the matte") {
    Bitmap src(2, 1);
    std::uint8_t* clear = src.at(0, 0);
    clear[0] = clear[1] = clear[2] = 255;
    clear[3] = 0;
    std::uint8_t* solid = src.at(1, 0);
    solid[0] = 30;
    solid[1] = 60;
    solid[2] = 90;
    solid[3] = 255;

    PnmOptions opts;
    opts.variant = PnmOptions::Variant::Pgm;
    opts.matte = mosaicfmt::Rgb8{0, 0, 0};
    const auto bytes = mosaicfmt::encodePnm(src.view(), opts);
    REQUIRE(bytes.has_value());
    CHECK(headOf(*bytes, 2) == "P5");
    const auto back = mosaicfmt::decodePnm(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    CHECK(back->at(0, 0)[0] == 0);  // transparent over a black matte
    CHECK(back->at(1, 0)[0] == luma601(30, 60, 90));
    CHECK(back->at(1, 0)[3] == 255);
}

TEST_CASE("PBM thresholds, and stores 1 as black") {
    Bitmap src(3, 1);
    for (std::uint32_t x = 0; x < 3; ++x) {
        std::uint8_t* p = src.at(x, 0);
        p[0] = p[1] = p[2] = static_cast<std::uint8_t>(x * 100);  // 0, 100, 200
        p[3] = 255;
    }
    PnmOptions opts;
    opts.variant = PnmOptions::Variant::Pbm;
    opts.bwThreshold = 128;
    for (const bool ascii : {false, true}) {
        opts.ascii = ascii;
        const auto bytes = mosaicfmt::encodePnm(src.view(), opts);
        REQUIRE(bytes.has_value());
        CHECK(headOf(*bytes, 2) == (ascii ? "P1" : "P4"));
        const auto back = mosaicfmt::decodePnm(bytes->data(), bytes->size());
        REQUIRE(back.has_value());
        CHECK(back->at(0, 0)[0] == 0);    // 0 -> below the threshold -> black
        CHECK(back->at(1, 0)[0] == 0);    // 100 -> still below
        CHECK(back->at(2, 0)[0] == 255);  // 200 -> white
        CHECK(back->at(0, 0)[3] == 255);
    }
    // The raw form's first sample byte has its top bit set, because in PBM a 1 bit is BLACK.
    opts.ascii = false;
    const auto raw = mosaicfmt::encodePnm(src.view(), opts);
    REQUIRE(raw.has_value());
    CHECK((raw->back() & 0x80u) != 0u);
}

TEST_CASE("PNM reads a header with comments in the middle of it") {
    // Comments are legal anywhere whitespace is -- including between the width and the height,
    // which is the case a line-based header parser gets wrong.
    const auto file = bytesOf("P3\n# a comment\n2 # between the dimensions\n1\n255\n"
                              "255 0 0  0 255 0\n");
    const auto back = mosaicfmt::decodePnm(file.data(), file.size());
    REQUIRE(back.has_value());
    CHECK(back->width == 2);
    CHECK(back->height == 1);
    CHECK(back->at(0, 0)[0] == 255);
    CHECK(back->at(0, 0)[1] == 0);
    CHECK(back->at(1, 0)[1] == 255);
}

TEST_CASE("PNM scales 16-bit samples down to eight") {
    std::vector<std::uint8_t> file = bytesOf("P6\n2 1\n65535\n");
    const std::uint16_t samples[6] = {65535, 0, 32768, 0, 65535, 32768};
    for (const std::uint16_t s : samples) {
        file.push_back(static_cast<std::uint8_t>(s >> 8));  // big-endian, as the format says
        file.push_back(static_cast<std::uint8_t>(s & 0xFF));
    }
    const auto back = mosaicfmt::decodePnm(file.data(), file.size());
    REQUIRE(back.has_value());
    CHECK(back->at(0, 0)[0] == 255);
    CHECK(back->at(0, 0)[1] == 0);
    CHECK(back->at(0, 0)[2] == 128);
    CHECK(back->at(1, 0)[0] == 0);
    CHECK(back->at(1, 0)[1] == 255);
    CHECK(back->at(1, 0)[2] == 128);
}

TEST_CASE("PNM rejects the headers it cannot honour") {
    std::string error;
    const auto bad = [&](std::string_view text) {
        error.clear();
        const auto file = bytesOf(text);
        CHECK_FALSE(mosaicfmt::decodePnm(file.data(), file.size(), &error).has_value());
        CHECK_FALSE(error.empty());
    };
    bad("P8\n2 2\n255\n");                                  // no such variant
    bad("Q6\n2 2\n255\n");                                  // no such family
    bad("P6\n2 2\n0\n");                                     // an impossible maximum value
    bad("P6\n0 2\n255\n");                                   // a zero dimension
    bad("P6\n99999 99999\n255\n");                           // past the dimension cap
    bad("P7\nWIDTH 2\nHEIGHT 2\nDEPTH 3\nMAXVAL 255\n");     // no ENDHDR
    bad("P7\nWIDTH 2\nHEIGHT 2\nMAXVAL 255\nENDHDR\n");      // no DEPTH
    bad("P7\nWIDTH 2\nHEIGHT 2\nDEPTH 9\nMAXVAL 255\nENDHDR\n");  // a tuple depth we cannot map
    bad("P6\n2 2\n255\n\x01\x02\x03");                       // the samples are truncated
}

TEST_CASE("PNM survives every truncation of a real file") {
    const Bitmap src = pattern(7, 5, /*opaque=*/false);
    PnmOptions ppm;
    PnmOptions pam;
    pam.variant = PnmOptions::Variant::Pam;
    PnmOptions plain;
    plain.ascii = true;
    for (const PnmOptions& opts : {ppm, pam, plain}) {
        const auto bytes = mosaicfmt::encodePnm(src.view(), opts);
        REQUIRE(bytes.has_value());
        for (std::size_t n = 0; n < bytes->size(); ++n) {
            const std::vector<std::uint8_t> cut(bytes->begin(),
                                                bytes->begin() + static_cast<std::ptrdiff_t>(n));
            std::string error;
            const auto decoded = mosaicfmt::decodePnm(cut.data(), cut.size(), &error);
            if (decoded.has_value())
                CHECK(decoded->consistent());
        }
    }
}
