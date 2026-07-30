#pragma once

#include "common/exif.hpp"
#include "common/image.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// io/exif -- the EXIF READ slice of the S41/S42 metadata story, landed early because the sky
// generator's "Estimate from layer" needs FocalLengthIn35mmFilm (plus DateTimeOriginal + GPS for
// its date & place). A hand-rolled TIFF/IFD walk over exactly the handful of tags
// common::ExifData carries -- deliberately NOT a vendored library: the tag set is tiny, and a
// parser this small can be exhaustively hostile-tested (tests/test_exif.cpp).
//
// Hostile-input discipline (the src/io/mosaic pattern): every offset/count is bounds-checked
// against the payload before any read, IFD entry counts and the payload size are capped, the
// walk visits at most three IFDs (IFD0 -> Exif IFD, IFD0 -> GPS IFD; no recursion, next-IFD
// pointers never followed, so offset loops cannot hang it), and nothing allocates in proportion
// to an attacker-controlled count (only the two capped strings are ever copied). Structural
// lies (an offset or count pointing outside the payload) reject the whole payload; a merely
// absurd VALUE (zero-denominator rational, out-of-range GPS, a date that is not a date) drops
// that field and keeps the rest -- absent is honest, a guessed value is not.
namespace mosaic::io {

// Parse a raw EXIF payload: the TIFF header + IFDs, i.e. the bytes AFTER a JPEG APP1 segment's
// "Exif\0\0" prefix, or a PNG eXIf chunk's whole payload. nullopt = structurally malformed (or
// over the size cap); a well-formed payload with none of our tags returns a value with every
// field nullopt (ExifData::hasAny() == false).
[[nodiscard]] std::optional<common::ExifData> parseExif(const std::uint8_t* data,
                                                        std::size_t size);

// Find and parse the EXIF payload inside a whole PNG or JPEG file buffer (JPEG: the first APP1
// segment with the "Exif\0\0" prefix; PNG: the standardized eXIf chunk, 2017). nullopt when the
// container carries none, the container walk hits malformed framing, or parseExif rejects the
// payload -- metadata is best-effort by design and never fails a pixel decode.
[[nodiscard]] std::optional<common::ExifData> extractExif(const std::vector<std::uint8_t>& file);

// Bake EXIF orientation `orientation` (2..8: mirrors, 180, and the transposed/rotated quartet,
// which swaps width and height) into the pixels, so a sideways-shot photo becomes upright. 1 --
// and, defensively, anything out of range -- is a no-op. Callers that keep the metadata must
// then record orientation = 1: the pixels ARE upright now, and a second application would be
// a bug (loadImageWithMetadata does both).
void applyExifOrientation(common::Image& img, int orientation);

}  // namespace mosaic::io
