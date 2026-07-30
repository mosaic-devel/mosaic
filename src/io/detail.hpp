#pragma once

#include "common/image.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// io/detail -- internal seams between io.cpp (the dispatch facade) and the per-format codec
// translation units (png.cpp, jpeg.cpp, mosaic/). Nothing here is part of io's public surface;
// dependents include io/io.hpp only.
namespace mosaic::io::detail {

// A sane upper bound on a decoded dimension, so a malformed or hostile header can't ask us to
// allocate gigabytes. Real per-format limits + a progress/cancel path are S41/S42.
inline constexpr std::uint32_t kMaxDim = 30000;

// ... and on the AREA, which the per-side limit alone does not bound: 30000 x 30000 passes both
// dimension checks and asks for 3.6 GB. 2^28 pixels is a 16384-square canvas -- comfortably past
// anything a camera produces, and about 1 GB once unpacked to RGBA8. The M4 decoders check this
// before the first allocation; a corrupt header is otherwise an out-of-memory kill, not an error
// message.
inline constexpr std::uint64_t kMaxPixels = std::uint64_t{1} << 28;

// Both limits at once, over values that may be arbitrary garbage from a header.
[[nodiscard]] constexpr bool dimensionsPlausible(std::uint64_t width,
                                                 std::uint64_t height) noexcept {
    return width != 0 && height != 0 && width <= kMaxDim && height <= kMaxDim &&
           width * height <= kMaxPixels;
}

// png.cpp -- decode any libpng-readable PNG into 8-bit straight-alpha RGBA.
[[nodiscard]] std::optional<common::Image> decodePng(const std::vector<std::uint8_t>& buf,
                                                     std::string* error);

// jpeg.cpp -- decode a JPEG (no alpha; filled with 255) via libjpeg-turbo.
[[nodiscard]] std::optional<common::Image> decodeJpeg(const std::vector<std::uint8_t>& buf,
                                                      std::string* error);

// The M4 codecs (webp.cpp / avif.cpp / tiff.cpp / gif.cpp). Each is compiled unconditionally but
// only DOES anything when its library was found: the stub half reports "support was not compiled
// in" rather than "unrecognised format", which is a materially different thing to tell a user
// holding a perfectly valid file.
//
// All four normalise to the same 8-bit straight-alpha RGBA the rest of io speaks:
//   * WebP  -- native RGBA, straight alpha, single (still) frame; the first frame of an animation
//              is what the still decoder yields.
//   * AVIF  -- YUV(+alpha) converted through avifImageYUVToRGB at depth 8; a premultiplied file
//              is un-premultiplied by libavif on the way out.
//   * TIFF  -- via TIFFReadRGBAImageOriented, which normalises photometric, planar config, bit
//              depth, orientation and palette for us; associated alpha is un-premultiplied here.
//   * GIF   -- the first frame, palette expanded, the transparent index expanded to alpha 0.
[[nodiscard]] std::optional<common::Image> decodeWebp(const std::vector<std::uint8_t>& buf,
                                                      std::string* error);
[[nodiscard]] std::optional<common::Image> decodeAvif(const std::vector<std::uint8_t>& buf,
                                                      std::string* error);
[[nodiscard]] std::optional<common::Image> decodeTiff(const std::vector<std::uint8_t>& buf,
                                                      std::string* error);
[[nodiscard]] std::optional<common::Image> decodeGif(const std::vector<std::uint8_t>& buf,
                                                     std::string* error);

// M5 -- the curated-pro formats (libmosaicformats, src/formats). ONE pair of calls reaches all six:
// the library sniffs its own signatures (mosaicfmt::sniff), and the adapter
// (backends/mosaicformats_backend.cpp) converts to common::Image -- and hands an icon's PNG payload
// to decodePng above, which is why this seam lives on THIS side of the fence. Deliberately NOT new
// ImageFormat members: io.hpp's sniff would then duplicate six signature checks that already exist
// one layer down, and TGA's "signature" is a plausibility judgement that belongs with its parser.
[[nodiscard]] bool sniffCuratedFormat(const std::vector<std::uint8_t>& buf) noexcept;
[[nodiscard]] std::optional<common::Image> decodeCuratedFormat(const std::vector<std::uint8_t>& buf,
                                                               std::string* error);

// webp.cpp -- the dimensions from a WebP's header alone (io::probeImageDimensions' fast path).
// `head` may be a PREFIX of the file: libwebp's WebPGetInfo reads only the RIFF/VP8X header.
// false when this build has no libwebp, or the head is not a readable WebP header.
[[nodiscard]] bool probeWebpDimensions(const std::vector<std::uint8_t>& head, std::uint32_t& width,
                                       std::uint32_t& height);

} // namespace mosaic::io::detail
