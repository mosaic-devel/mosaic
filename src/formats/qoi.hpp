#pragma once

#include "formats/formats.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// QOI -- the "Quite OK Image" format, implemented clean-room from the published specification
// (Dominic Szablewski, 2021; the spec document is MIT-licensed and one page long, which is the
// point of the format).
//
// The whole codec is a running one-pixel predictor plus a 64-entry hash index, so there is no
// entropy coder, no block structure and nothing to tune -- which is why the option surface is two
// header fields and nothing else, and why an encode -> decode round trip is BIT-EXACT by
// construction rather than by care.
namespace mosaicfmt {

struct QoiOptions {
    // The header's channel count. 4 stores alpha; 3 declares an opaque image, so transparency is
    // composited onto `matte` before encoding (the container has nowhere to put it).
    int channels = 4;
    // The header's colorspace byte: false = 0 (sRGB with a linear alpha channel), true = 1 (all
    // channels linear). It is a TAG ONLY -- QOI never converts anything, and neither do we; the
    // pixels are written unchanged either way.
    bool linearColorspace = false;
    Rgb8 matte;  // used only when channels == 3
};

// Encode `image` as a QOI file. nullopt only for an invalid view or a nonsensical option bag.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeQoi(const ImageView& image,
                                                                 const QoiOptions& opts = {},
                                                                 std::string* error = nullptr);

// Decode a QOI file. A 3-channel file yields alpha 255 everywhere.
[[nodiscard]] std::optional<Bitmap> decodeQoi(const std::uint8_t* data, std::size_t size,
                                              std::string* error = nullptr);

} // namespace mosaicfmt
