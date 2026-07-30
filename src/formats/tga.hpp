#pragma once

#include "formats/formats.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// TGA / Truevision Targa, version 2.
//
// Two things make TGA interesting rather than trivial. It has NO MAGIC NUMBER -- identification is
// a plausibility judgement on its 18-byte header (see mosaicfmt::sniff) -- and the MEANING of its
// alpha channel is not in the pixels but in the v2 extension area's "attributes type" field, which
// is the only place a Targa says whether its alpha is straight or premultiplied. Writing that
// field is the difference between a file that composites correctly and one that darkens every
// soft edge, so this encoder always writes the extension area and the v2 footer.
namespace mosaicfmt {

struct TgaOptions {
    // Bgra16 is TGA's 5-5-5 mode with one alpha bit: transparency is all-or-nothing there.
    enum class Depth { Bgra32, Bgr24, Bgra16 };
    Depth depth = Depth::Bgra32;

    // Run-length coding (image type 10). On by default: it is lossless, universally supported, and
    // an artwork export with flat regions is where TGA earns the extra code.
    bool rle = true;

    // Rows top-down (descriptor bit 5). On by default -- the order our buffers already have, and
    // what every modern writer produces; bottom-up is the format's older habit.
    bool topDown = true;

    // What the alpha channel MEANS, recorded in the extension area's attributes type:
    //   Straight       (3) unassociated alpha -- our pipeline's own convention
    //   Premultiplied  (4) associated alpha; the pixels ARE premultiplied on the way out, because
    //                      relabelling them without multiplying would be a lie about the file
    //   Ignored        (0) no useful alpha; transparency is composited onto `matte` instead
    enum class AlphaAttributes { Straight, Premultiplied, Ignored };
    AlphaAttributes alpha = AlphaAttributes::Straight;

    Rgb8 matte;  // what transparency lands on for Bgr24 and for AlphaAttributes::Ignored
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeTga(const ImageView& image,
                                                                 const TgaOptions& opts = {},
                                                                 std::string* error = nullptr);

// Decode a TGA: image types 1, 2, 3, 9, 10 and 11 (colour-mapped, truecolour and greyscale, each
// plain or RLE) at 8, 15, 16, 24 and 32 bits per pixel, either origin, either scan direction. A
// premultiplied file (attributes type 4) is un-premultiplied on the way out, so callers only ever
// see straight alpha.
[[nodiscard]] std::optional<Bitmap> decodeTga(const std::uint8_t* data, std::size_t size,
                                              std::string* error = nullptr);

} // namespace mosaicfmt
