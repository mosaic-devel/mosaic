#pragma once

#include "formats/formats.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Radiance HDR (.hdr / .pic) -- RGBE, the shared-exponent format Greg Ward published in 1991.
//
// ⚠ THE PIPELINE THIS SITS ON IS STILL 8-BIT. `common::Image` is 8-bit RGBA and
// `render::composite()` collapses to it at `toImage8Parallel` (plan §5's high-bit note), so an
// export here cannot contain high-dynamic-range information that the document never produced. What
// it CAN do is be a correct, round-trippable RGBE file, and that is what this is:
//
//   * on ENCODE, each 8-bit sRGB value is decoded through the sRGB transfer function to LINEAR
//     light and that linear value is what the file stores. Radiance files are linear-light by
//     convention, so writing the encoded value verbatim would make every HDR viewer show a
//     washed-out picture. There is no tone mapping and no invented headroom: the values simply
//     land in [0,1];
//   * on DECODE, the linear value is clamped to [0,1] (an exposure-1 clamp -- no auto-exposure, no
//     tone mapper, because guessing at one is worse than a documented clamp) and re-encoded to
//     8-bit sRGB.
//
// So the pair is self-consistent, and a real HDR file loses only what an 8-bit buffer cannot hold.
// Genuine HDR output waits on the "tap the ImageF accumulator before the 8-bit conversion" slice
// (plan §5 / S43-a); when it lands, this encoder needs a float entry point and nothing else.
namespace mosaicfmt {

struct HdrOptions {
    // Radiance's adaptive run-length coding: four separately coded component streams per scanline.
    // On by default -- it is lossless, it is what every writer produces, and a flat file is
    // roughly four bytes per pixel with no upside.
    bool rle = true;
    Rgb8 matte;  // the format carries no alpha, so transparency is composited onto this
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeHdr(const ImageView& image,
                                                                 const HdrOptions& opts = {},
                                                                 std::string* error = nullptr);

// Decode a Radiance RGBE file: flat scanlines, the old run-length spelling and the adaptive one,
// with the standard "-Y h +X w" resolution line (and its vertically flipped "+Y" variant). An
// EXPOSURE header is honoured -- the values in the file were multiplied by it, so they are divided
// back out. XYZE files are refused rather than guessed at (see docs/formats-curated.md).
[[nodiscard]] std::optional<Bitmap> decodeHdr(const std::uint8_t* data, std::size_t size,
                                              std::string* error = nullptr);

} // namespace mosaicfmt
