#include "formats/hdr.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// libmosaicformats' Radiance RGBE codec (docs/export-system-plan.md §10 item 5,
// docs/formats-curated.md). Standalone: formats/hdr.hpp and nothing else of Mosaic's.
//
// ⚠ What these cases can and cannot assert. The pipeline above is 8-bit (§5's high-bit note), so
// there is no high dynamic range to test round-tripping -- what is tested is that the file is a
// CORRECT RGBE file and that the pair of conversions is self-consistent. RGBE shares one exponent
// across a pixel's three channels, so a dark channel beside a bright one loses precision by
// construction; the tolerances below are the format's, not the code's, and the case that asserts
// EXACT equality does it between two encodings of the same pixels (run-length coded and flat),
// where the quantisation is common to both and only the coding differs.
namespace {

using mosaicfmt::Bitmap;
using mosaicfmt::HdrOptions;

// A grey ramp: all three channels equal, so the shared exponent costs nothing and the round trip
// is tight.
Bitmap greyRamp(std::uint32_t w, std::uint32_t h) {
    Bitmap b(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = b.at(x, y);
            const std::uint8_t v = static_cast<std::uint8_t>((x * 255u) / (w > 1 ? w - 1 : 1));
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    return b;
}

// Colour, with every channel kept in [64, 255]: a bounded dynamic range within each pixel, which
// is what keeps the shared exponent's cost inside a few code values.
Bitmap boundedColour(std::uint32_t w, std::uint32_t h) {
    Bitmap b(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = b.at(x, y);
            p[0] = static_cast<std::uint8_t>(64 + (x * 19) % 192);
            p[1] = static_cast<std::uint8_t>(64 + (y * 37) % 192);
            p[2] = static_cast<std::uint8_t>(64 + ((x + y) * 13) % 192);
            p[3] = 255;
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

std::string headOf(const std::vector<std::uint8_t>& file, std::size_t n) {
    return std::string(file.begin(),
                       file.begin() + static_cast<std::ptrdiff_t>(n < file.size() ? n : file.size()));
}

std::vector<std::uint8_t> bytesOf(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

} // namespace

TEST_CASE("Radiance HDR writes the header a reader looks for") {
    const Bitmap src = greyRamp(16, 3);
    const auto bytes = mosaicfmt::encodeHdr(src.view());
    REQUIRE(bytes.has_value());
    const std::string header = headOf(*bytes, 64);
    CHECK(header.rfind("#?RADIANCE\n", 0) == 0);
    CHECK(header.find("FORMAT=32-bit_rle_rgbe\n") != std::string::npos);
    // The resolution line's ORDER is the orientation: -Y first means rows run top to bottom.
    CHECK(header.find("-Y 3 +X 16\n") != std::string::npos);
}

TEST_CASE("Radiance HDR's run-length coding changes the bytes and not the picture") {
    const Bitmap src = boundedColour(40, 6);
    HdrOptions coded;
    HdrOptions flat;
    flat.rle = false;
    const auto a = mosaicfmt::encodeHdr(src.view(), coded);
    const auto b = mosaicfmt::encodeHdr(src.view(), flat);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->size() != b->size());

    const auto backA = mosaicfmt::decodeHdr(a->data(), a->size());
    const auto backB = mosaicfmt::decodeHdr(b->data(), b->size());
    REQUIRE(backA.has_value());
    REQUIRE(backB.has_value());
    // EXACT: both files hold the same RGBE bytes, so any difference is the coder's fault alone.
    CHECK(backA->rgba == backB->rgba);
}

TEST_CASE("Radiance HDR's flat and coded runs both survive a long uniform scanline") {
    // 300 identical pixels: one run packet per component per 127, which is the path where an
    // off-by-one in the packet length shows up as a shifted picture rather than a crash.
    Bitmap flat(300, 2);
    for (std::size_t i = 0; i < flat.rgba.size(); i += 4) {
        flat.rgba[i + 0] = 90;
        flat.rgba[i + 1] = 140;
        flat.rgba[i + 2] = 200;
        flat.rgba[i + 3] = 255;
    }
    const auto bytes = mosaicfmt::encodeHdr(flat.view());
    REQUIRE(bytes.has_value());
    const auto back = mosaicfmt::decodeHdr(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    CHECK(back->width == 300);
    CHECK(back->height == 2);
    // Every pixel came out the same, and the first one is a faithful round trip of the source.
    for (std::size_t i = 4; i < back->rgba.size(); i += 4)
        REQUIRE(back->rgba[i] == back->rgba[0]);
    CHECK(maxDelta(*back, flat) <= 4);
}

TEST_CASE("Radiance HDR round-trips a grey ramp within its own precision") {
    const Bitmap src = greyRamp(32, 4);
    const auto bytes = mosaicfmt::encodeHdr(src.view());
    REQUIRE(bytes.has_value());
    const auto back = mosaicfmt::decodeHdr(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    CHECK(back->width == src.width);
    CHECK(back->height == src.height);
    // Equal channels share their exponent perfectly, so the only error is the mantissa's rounding.
    CHECK(maxDelta(*back, src) <= 2);
}

TEST_CASE("Radiance HDR round-trips bounded colour within a few code values") {
    const Bitmap src = boundedColour(24, 5);
    const auto bytes = mosaicfmt::encodeHdr(src.view());
    REQUIRE(bytes.has_value());
    const auto back = mosaicfmt::decodeHdr(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    CHECK(maxDelta(*back, src) <= 4);
}

TEST_CASE("Radiance HDR has no alpha, so transparency lands on the matte") {
    Bitmap src(8, 1);
    for (std::uint32_t x = 0; x < 8; ++x) {
        std::uint8_t* p = src.at(x, 0);
        p[0] = p[1] = p[2] = 255;
        p[3] = x == 0 ? std::uint8_t{0} : std::uint8_t{255};
    }
    HdrOptions opts;
    opts.matte = mosaicfmt::Rgb8{0, 0, 0};
    const auto bytes = mosaicfmt::encodeHdr(src.view(), opts);
    REQUIRE(bytes.has_value());
    const auto back = mosaicfmt::decodeHdr(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    CHECK(back->at(0, 0)[0] == 0);
    CHECK(back->at(0, 0)[3] == 255);  // the format cannot say anything else
    CHECK(back->at(1, 0)[0] >= 250);
}

TEST_CASE("Radiance HDR reads the old run-length spelling") {
    // A width of 4 is below the adaptive coding's range, so the scanline is flat -- and a
    // (1,1,1,n) pixel there means "repeat the previous pixel n times", which is how the format
    // counted runs before 1991's rewrite.
    std::vector<std::uint8_t> file = bytesOf("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 4\n");
    const std::uint8_t first[4] = {100, 100, 100, 128};
    const std::uint8_t repeat[4] = {1, 1, 1, 3};
    file.insert(file.end(), first, first + 4);
    file.insert(file.end(), repeat, repeat + 4);
    const auto back = mosaicfmt::decodeHdr(file.data(), file.size());
    REQUIRE(back.has_value());
    REQUIRE(back->width == 4);
    for (std::uint32_t x = 1; x < 4; ++x)
        for (std::size_t c = 0; c < 4; ++c)
            CHECK(back->at(x, 0)[c] == back->at(0, 0)[c]);
    CHECK(back->at(0, 0)[0] > 100);  // and the RGBE arithmetic produced something plausible
    CHECK(back->at(0, 0)[3] == 255);
}

TEST_CASE("Radiance HDR refuses what it will not guess at") {
    std::string error;
    const auto bad = [&](std::string_view text) {
        error.clear();
        const auto file = bytesOf(text);
        CHECK_FALSE(mosaicfmt::decodeHdr(file.data(), file.size(), &error).has_value());
        CHECK_FALSE(error.empty());
    };
    bad("not a radiance file at all, not even close\n");
    // XYZE stores CIE XYZ with a shared exponent; converting it needs the file's primaries.
    bad("#?RADIANCE\nFORMAT=32-bit_rle_xyze\n\n-Y 2 +X 2\n");
    bad("#?RADIANCE\n\n-Y 2 +X 2\n");                       // no FORMAT line at all
    bad("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n+X 2 -Y 2\n"); // the transposed orientation
    bad("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 0 +X 2\n"); // a zero dimension
    bad("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 2\n"); // no pixels behind the header
}

TEST_CASE("Radiance HDR survives every truncation of a real file") {
    const Bitmap src = boundedColour(20, 4);
    HdrOptions coded;
    HdrOptions flat;
    flat.rle = false;
    for (const HdrOptions& opts : {coded, flat}) {
        const auto bytes = mosaicfmt::encodeHdr(src.view(), opts);
        REQUIRE(bytes.has_value());
        for (std::size_t n = 0; n < bytes->size(); ++n) {
            const std::vector<std::uint8_t> cut(bytes->begin(),
                                                bytes->begin() + static_cast<std::ptrdiff_t>(n));
            std::string error;
            const auto decoded = mosaicfmt::decodeHdr(cut.data(), cut.size(), &error);
            if (decoded.has_value())
                CHECK(decoded->consistent());
        }
    }
}

TEST_CASE("Radiance HDR survives a scrambled scanline stream") {
    const Bitmap src = boundedColour(64, 8);
    auto bytes = mosaicfmt::encodeHdr(src.view());
    REQUIRE(bytes.has_value());
    for (std::size_t i = bytes->size() / 2; i < bytes->size(); ++i)
        (*bytes)[i] = static_cast<std::uint8_t>((i * 7919u) & 0xFFu);
    std::string error;
    const auto decoded = mosaicfmt::decodeHdr(bytes->data(), bytes->size(), &error);
    if (decoded.has_value())
        CHECK(decoded->consistent());
}
