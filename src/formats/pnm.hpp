#pragma once

#include "formats/formats.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// PNM -- the Netpbm family: PBM (bilevel), PGM (greyscale), PPM (colour) and PAM (the arbitrary-
// tuple generalisation, which is the one that carries alpha). Exporting PAM is one of the four
// formats §0 records us as EXCEEDING GIMP on.
//
// The family is six magic numbers over one idea -- a text header, then samples -- and its subtlety
// is entirely in the header: comments may appear anywhere whitespace may, exactly one whitespace
// byte separates the header from binary samples, PBM stores 1 = BLACK while every other variant
// stores 0 = black, and PAM's own bilevel tuple type inverts PBM again (1 = white).
namespace mosaicfmt {

struct PnmOptions {
    // Which member of the family to write. Only Pam can carry transparency; the other three
    // composite it onto `matte`.
    enum class Variant { Ppm, Pgm, Pbm, Pam };
    Variant variant = Variant::Ppm;

    // Plain (ASCII) rather than raw (binary) samples: P3/P2/P1 instead of P6/P5/P4. Far larger and
    // occasionally useful for hand inspection. PAM has no plain form, so this is ignored there.
    bool ascii = false;

    // PAM only: the tuple the file declares, which is also how many samples a pixel holds.
    enum class PamTuple { RgbAlpha, Rgb, GrayscaleAlpha, Grayscale };
    PamTuple pamTuple = PamTuple::RgbAlpha;

    // PBM only: the luminance below which a pixel becomes black. The conversion is a plain
    // threshold, deliberately not a dither -- a bilevel export of a photograph wants an explicit
    // dithering step, and inventing one inside a codec would hide it.
    int bwThreshold = 128;

    Rgb8 matte;
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encodePnm(const ImageView& image,
                                                                 const PnmOptions& opts = {},
                                                                 std::string* error = nullptr);

// Decode any of P1..P7, 8- or 16-bit samples. PAM depths 1, 2, 3 and 4 map to grey, grey+alpha,
// RGB and RGBA; every other variant yields opaque pixels.
[[nodiscard]] std::optional<Bitmap> decodePnm(const std::uint8_t* data, std::size_t size,
                                              std::string* error = nullptr);

} // namespace mosaicfmt
