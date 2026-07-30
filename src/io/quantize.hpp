#pragma once

#include "common/image.hpp"

#include <cstdint>
#include <vector>

// io/quantize -- truecolour RGBA down to an indexed palette, with optional error diffusion.
//
// GIF is the first consumer (giflib gives us the LZW layer and nothing else -- the palette and
// the dithering are ours), and the curated-tier BMP/PCX/ICO writers of M5/M7 will want exactly
// the same thing. It lives here rather than inside gif.cpp so it can be unit-tested with no GIF
// in sight, and so the loss banner's "quantised to N colours" claim is backed by one algorithm
// instead of one per format.
//
// The algorithm is Heckbert's median cut (SIGGRAPH 1982) over a 5-5-5 histogram whose buckets
// accumulate the TRUE 8-bit sums, so a box's representative colour is the real average of the
// pixels in it rather than a 5-bit centroid; error diffusion is Floyd-Steinberg (1976). There is a
// deliberate exactness carve-out: an image whose distinct colour count already fits the palette is
// mapped one-to-one and comes back BIT-EXACT, which is what makes a lossless round-trip test of an
// indexed format meaningful at all.
namespace mosaic::io {

struct QuantizeOptions {
    // Palette entries available, 2..256 (clamped). When the image needs transparency, one entry
    // is spent on it and the picture itself gets maxColors - 1.
    int maxColors = 256;
    // Floyd-Steinberg error diffusion. Ignored -- because there is no error to diffuse -- when
    // the image's own colours already fit the palette.
    bool dither = true;
    // Pixels with alpha strictly below this become the single transparent index; everything else
    // is composited over `matte` first, so a soft edge lands on a known colour rather than on
    // whatever the palette happened to leave behind it. 0 disables transparency entirely.
    int alphaThreshold = 128;
    common::Color8 matte{255, 255, 255, 255};
};

struct QuantizedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> indices;      // width * height, one palette index per pixel
    std::vector<common::Color8> palette;    // 1..256 entries; opaque, except the transparent one
    // The palette slot that means "transparent", or -1 when the image is fully opaque. Its
    // palette colour is {0,0,0,0} and is never used for a visible pixel.
    int transparentIndex = -1;

    // True when every pixel's palette colour equals its source colour (the carve-out above), so
    // a caller may advertise the encode as lossless.
    bool exact = false;

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
};

// Quantize `image` (8-bit straight-alpha RGBA). An empty image yields an empty result with an
// empty palette. Never fails: a degenerate request (maxColors below 2, an all-transparent image)
// is clamped into a valid palette rather than rejected.
[[nodiscard]] QuantizedImage quantize(const common::Image& image, const QuantizeOptions& opts = {});

}  // namespace mosaic::io
