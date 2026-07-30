#pragma once

#include "formats/formats.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// BMP / DIB (Windows bitmap).
//
// The format is trivial and its history is not: five header versions, six compressions, four
// places a colour mask can live, and a 32-bit mode whose fourth byte means alpha in some files
// and nothing at all in others. This translation unit owns the whole of that, in both directions,
// including the headerless DIB that an ICO entry carries (ico.cpp calls decodeDib) -- one parser,
// so there is one hostile-input surface to harden instead of two.
namespace mosaicfmt {

struct BmpOptions {
    // What a pixel becomes on disk.
    //   Bgra32   8 bits per channel plus alpha -- the only mode that can carry transparency, and
    //            then only with a V4/V5 header (see writesAlpha below)
    //   Bgr24    the classic, universally readable mode; alpha is composited onto `matte`
    //   Rgb565   16-bit, 5-6-5 bit fields; smaller, visibly banded, and still no alpha
    //   Indexed8 256-entry palette; encodeBmpIndexed() only, because the PALETTE IS THE CALLER'S
    enum class Depth { Bgra32, Bgr24, Rgb565, Indexed8 };
    Depth depth = Depth::Bgra32;

    // 3 = BITMAPINFOHEADER (40 bytes, everything reads it), 4 = BITMAPV4HEADER (108, adds the
    // colour masks and endpoints), 5 = BITMAPV5HEADER (124, adds the embedded ICC profile).
    int headerVersion = 5;

    // BI_RLE8 run-length coding. Indexed8 only -- the RLE modes are defined over palette indices.
    // Forces bottom-up rows: a top-down RLE bitmap is illegal in the format.
    bool rle = false;

    // Negative biHeight: rows top-down, the order our own buffers are already in. Off by default
    // because bottom-up is what every reader has always seen, and some old ones only handle that.
    bool topDown = false;

    // Physical density, written to biXPelsPerMeter/biYPelsPerMeter. 0 (and 72, the "no opinion"
    // value every writer in Mosaic skips) writes zeroes, which is what "unspecified" looks like.
    double dpi = 0.0;

    Rgb8 matte;  // what transparency is composited onto in every mode that cannot carry it

    // A complete ICC profile, embedded when headerVersion == 5 (PROFILE_EMBEDDED). Ignored by V3
    // and V4, which have nowhere to put it.
    std::vector<std::uint8_t> icc;
};

// Whether these options actually preserve the alpha channel. Alpha survives ONLY as 32-bit with a
// V4/V5 header, because that is the only spelling that carries an explicit alpha MASK -- a V3
// 32-bit BI_RGB bitmap's fourth byte is formally undefined, and readers split roughly evenly on
// whether it is alpha or padding. The backend's help text says so in as many words; a caller that
// wants transparency must not talk the user out of the default.
[[nodiscard]] bool bmpWritesAlpha(const BmpOptions& opts) noexcept;

// Encode a truecolour BMP (Bgra32 / Bgr24 / Rgb565). Depth::Indexed8 is rejected here: an indexed
// encode needs a palette, and choosing one is the caller's policy (io/quantize.hpp).
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeBmp(const ImageView& image,
                                                                 const BmpOptions& opts = {},
                                                                 std::string* error = nullptr);

// Encode an 8-bit palettised BMP, optionally BI_RLE8-compressed. `opts.depth` is ignored (an
// IndexedView can only be written indexed); everything else -- header version, RLE, top-down,
// dpi, ICC -- applies as usual.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodeBmpIndexed(
    const IndexedView& image, const BmpOptions& opts = {}, std::string* error = nullptr);

// Decode a .bmp file (a BITMAPFILEHEADER followed by a DIB).
[[nodiscard]] std::optional<Bitmap> decodeBmp(const std::uint8_t* data, std::size_t size,
                                              std::string* error = nullptr);

// Decode a bare DIB -- no BITMAPFILEHEADER, pixels immediately after the header, its masks and
// its palette. `icoEntry` selects the ICO convention: biHeight is DOUBLED (the XOR image plus the
// AND mask), and the 1-bit mask that follows the pixels supplies transparency for the payload
// depths that have none. Getting that doubled height wrong is the classic ICO bug -- it yields a
// half-height image that only Explorer seems to notice.
[[nodiscard]] std::optional<Bitmap> decodeDib(const std::uint8_t* data, std::size_t size,
                                              bool icoEntry, std::string* error = nullptr);

// The bare DIB an ICO entry wants written: a BITMAPINFOHEADER with the doubled height, 32-bit
// BGRA bottom-up pixels, and the 1-bit AND mask derived from the alpha channel. Lives here
// because it is a DIB; the directory around it is ico.cpp's.
[[nodiscard]] std::vector<std::uint8_t> encodeIcoDib(const ImageView& image);

} // namespace mosaicfmt
