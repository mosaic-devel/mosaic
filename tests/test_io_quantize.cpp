#include "common/image.hpp"
#include "io/quantize.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <vector>

// io/quantize -- the palette + dither behind the GIF backend (and, later, M5's BMP/PCX/ICO
// writers). Tested as a pure function, with no GIF in sight: the interesting failure modes are
// all here (a palette that overruns its budget, a transparent slot that eats a colour it should
// not, a median cut that never terminates, an exactness claim that is not true).
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::io::quantize;
using mosaic::io::QuantizedImage;
using mosaic::io::QuantizeOptions;

void put(Image& img, std::uint32_t x, std::uint32_t y, Color8 c) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    img.rgba[p + 0] = c.r;
    img.rgba[p + 1] = c.g;
    img.rgba[p + 2] = c.b;
    img.rgba[p + 3] = c.a;
}

Color8 get(const Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return Color8{img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

// The quantized image rebuilt as RGBA, so a test can compare against the source directly.
Image expand(const QuantizedImage& q) {
    Image out(q.width, q.height);
    for (std::size_t i = 0; i < q.indices.size(); ++i) {
        const Color8 c = q.palette[q.indices[i]];
        const bool transparent = q.transparentIndex >= 0 &&
                                 q.indices[i] == static_cast<std::uint8_t>(q.transparentIndex);
        out.rgba[i * 4 + 0] = transparent ? 0 : c.r;
        out.rgba[i * 4 + 1] = transparent ? 0 : c.g;
        out.rgba[i * 4 + 2] = transparent ? 0 : c.b;
        out.rgba[i * 4 + 3] = transparent ? 0 : 255;
    }
    return out;
}

// A few flat colour blocks: distinct, and few enough to fit any palette.
Image blocks(std::uint32_t w, std::uint32_t h, const std::vector<Color8>& colors) {
    Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            put(img, x, y, colors[(static_cast<std::size_t>(y) * w + x) % colors.size()]);
    return img;
}

// A smooth two-axis ramp: far more distinct colours than any palette, so the median cut runs.
Image ramp(std::uint32_t w, std::uint32_t h) {
    Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            put(img, x, y,
                Color8{static_cast<std::uint8_t>(x * 255 / (w - 1)),
                       static_cast<std::uint8_t>(y * 255 / (h - 1)),
                       static_cast<std::uint8_t>((x + y) * 127 / (w + h - 2)), 255});
    return img;
}

std::size_t distinctIndices(const QuantizedImage& q) {
    return std::set<std::uint8_t>(q.indices.begin(), q.indices.end()).size();
}

} // namespace

TEST_CASE("an empty image quantizes to an empty result rather than failing") {
    const QuantizedImage q = quantize(Image{});
    CHECK(q.empty());
    CHECK(q.indices.empty());
    CHECK(q.palette.empty());
    CHECK(q.transparentIndex == -1);
}

TEST_CASE("an image whose colours already fit the palette round-trips exactly") {
    const std::vector<Color8> colors = {Color8{255, 0, 0, 255}, Color8{0, 255, 0, 255},
                                        Color8{0, 0, 255, 255}, Color8{17, 19, 23, 255}};
    const Image src = blocks(8, 6, colors);

    const QuantizedImage q = quantize(src, QuantizeOptions{});
    CHECK(q.exact);
    CHECK(q.palette.size() == colors.size());
    CHECK(q.transparentIndex == -1);
    CHECK(expand(q) == src);

    // Two colours that differ only in the low three bits would collapse into one 5-5-5 histogram
    // cell; the exactness carve-out is precisely what keeps them apart.
    const Image close = blocks(4, 4, {Color8{0, 0, 0, 255}, Color8{7, 0, 0, 255}});
    const QuantizedImage tight = quantize(close, QuantizeOptions{});
    CHECK(tight.exact);
    CHECK(tight.palette.size() == 2);
    CHECK(expand(tight) == close);
}

TEST_CASE("transparency takes exactly one palette slot, and only when it is used") {
    Image src = blocks(4, 4, {Color8{200, 100, 50, 255}, Color8{10, 20, 30, 255}});
    put(src, 0, 0, Color8{0, 0, 0, 0});
    put(src, 3, 3, Color8{9, 9, 9, 12});  // below the default threshold: also transparent

    const QuantizedImage q = quantize(src, QuantizeOptions{});
    REQUIRE(q.transparentIndex >= 0);
    CHECK(q.palette[static_cast<std::size_t>(q.transparentIndex)] == Color8{0, 0, 0, 0});
    CHECK(q.indices[0] == static_cast<std::uint8_t>(q.transparentIndex));
    CHECK(q.indices[15] == static_cast<std::uint8_t>(q.transparentIndex));
    CHECK(q.indices[1] != static_cast<std::uint8_t>(q.transparentIndex));

    // A threshold of 0 means "no transparency at all": both pixels are composited onto the matte.
    QuantizeOptions opaque;
    opaque.alphaThreshold = 0;
    opaque.matte = Color8{255, 255, 255, 255};
    const QuantizedImage flat = quantize(src, opaque);
    CHECK(flat.transparentIndex == -1);
    CHECK(expand(flat).rgba[3] == 255);
    CHECK(get(expand(flat), 0, 0) == Color8{255, 255, 255, 255});  // over the white matte
}

TEST_CASE("the palette never exceeds the requested budget") {
    const Image src = ramp(48, 48);
    for (const int budget : {2, 3, 16, 100, 256}) {
        QuantizeOptions opts;
        opts.maxColors = budget;
        opts.dither = false;
        const QuantizedImage q = quantize(src, opts);
        CAPTURE(budget);
        CHECK(q.palette.size() <= static_cast<std::size_t>(budget));
        CHECK_FALSE(q.palette.empty());
        CHECK(q.indices.size() == src.pixelCount());
        for (const std::uint8_t index : q.indices)
            REQUIRE(index < q.palette.size());
    }

    // ... and a budget below the format's floor is clamped, not honoured into nonsense.
    QuantizeOptions silly;
    silly.maxColors = -5;
    const QuantizedImage q = quantize(src, silly);
    CHECK(q.palette.size() <= 2);
    CHECK_FALSE(q.palette.empty());
}

TEST_CASE("a transparent image still leaves a colour slot at the smallest budget") {
    Image src(4, 4);  // every pixel {0,0,0,0}
    QuantizeOptions opts;
    opts.maxColors = 2;
    const QuantizedImage q = quantize(src, opts);
    REQUIRE(q.transparentIndex >= 0);
    CHECK(q.palette.size() == 2);  // one unused colour + the transparent slot
    for (const std::uint8_t index : q.indices)
        CHECK(index == static_cast<std::uint8_t>(q.transparentIndex));
}

TEST_CASE("the median cut follows the picture, and dithering spends more of the palette") {
    const Image src = ramp(64, 64);

    QuantizeOptions plain;
    plain.maxColors = 8;
    plain.dither = false;
    const QuantizedImage flat = quantize(src, plain);
    CHECK_FALSE(flat.exact);  // a ramp cannot fit eight colours
    CHECK(flat.palette.size() == 8);

    QuantizeOptions dithered = plain;
    dithered.dither = true;
    const QuantizedImage shaken = quantize(src, dithered);
    CHECK(shaken.palette == flat.palette);  // dithering changes the mapping, not the palette
    // Error diffusion mixes neighbouring entries, so a given ROW uses more of the palette than
    // the hard-threshold mapping does. Comparing whole-image counts would be too weak: both use
    // most of eight colours somewhere.
    const auto rowSpread = [&](const QuantizedImage& q, std::uint32_t y) {
        std::set<std::uint8_t> seen;
        for (std::uint32_t x = 0; x < q.width; ++x)
            seen.insert(q.indices[static_cast<std::size_t>(y) * q.width + x]);
        return seen.size();
    };
    CHECK(rowSpread(shaken, 32) >= rowSpread(flat, 32));
    CHECK(distinctIndices(shaken) > 1);
}

TEST_CASE("quantization is deterministic") {
    const Image src = ramp(40, 33);
    QuantizeOptions opts;
    opts.maxColors = 32;
    const QuantizedImage a = quantize(src, opts);
    const QuantizedImage b = quantize(src, opts);
    CHECK(a.palette == b.palette);
    CHECK(a.indices == b.indices);
    CHECK(a.transparentIndex == b.transparentIndex);
}

TEST_CASE("a quantized ramp stays recognisably the same picture") {
    const Image src = ramp(64, 64);
    QuantizeOptions opts;
    opts.maxColors = 64;
    opts.dither = false;
    const Image got = expand(quantize(src, opts));
    REQUIRE(got.width == src.width);

    // Mean absolute error per channel: a 64-entry palette over a smooth ramp should sit far
    // below this. The point is to catch a palette that is built from the wrong pixels (or an
    // index map that is off by a row), not to pin an exact number.
    long total = 0;
    for (std::size_t i = 0; i < src.rgba.size(); i += 4)
        for (int c = 0; c < 3; ++c)
            total += std::abs(static_cast<int>(src.rgba[i + c]) - static_cast<int>(got.rgba[i + c]));
    const double mae = static_cast<double>(total) / static_cast<double>(src.pixelCount() * 3);
    CHECK(mae < 16.0);
}
