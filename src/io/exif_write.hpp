#pragma once

#include "common/exif.hpp"

#include <cstdint>
#include <vector>

// io/exif_write -- the WRITE half of the EXIF slice (docs/export-system-plan.md §7b left it owed).
// io/exif.hpp reads a camera's metadata when a photo is opened; this serialises the typed
// common::ExifData back into the one wire format every container wants: a little-endian TIFF
// header followed by IFD0, an Exif sub-IFD and a GPS sub-IFD.
//
// The same payload feeds every backend that can carry metadata -- a PNG eXIf chunk, a WebP "EXIF"
// mux chunk, an AVIF Exif item -- because all three store exactly these bytes (a JPEG APP1 would
// additionally need the "Exif\0\0" prefix, which is the container's job, not this one's).
//
// The contract that makes it testable: parseExif(buildExifPayload(d)) == d for every `d` whose
// fields are in range, up to the resolution the wire format itself has (focal length is written
// as thousandths of a millimetre; GPS seconds as ten-thousandths of a second). A field that is
// absent, or present but out of the range io/exif.cpp would accept on the way back in, is simply
// not written -- an export must not invent metadata, and must not write metadata its own reader
// would then reject.
namespace mosaic::io {

// Serialise `data` as a raw EXIF payload. Returns an EMPTY vector when there is nothing to say
// (no fields, or every field out of range), which every caller reads as "write no metadata".
[[nodiscard]] std::vector<std::uint8_t> buildExifPayload(const common::ExifData& data);

// The record an EXPORT may write, from the record a LOAD produced: `data` with **Orientation
// forced to 1**.
//
// This is not a nicety, it is a correctness rule, and it is the kind of bug that only ever shows
// up in someone else's viewer. io::loadImageWithMetadata BAKES the shot rotation into the pixels
// (io::applyExifOrientation) and then records 1 -- but a document's metadata can also arrive from
// a .mosaic manifest, from a layer stamped before that bake existed, or from a future importer,
// and writing an original 6 back out would tell every reader to rotate an already-upright picture
// a second time. There is no toggle for this (the strictly-better rule): a value of 1 is the only
// one that is true of the pixels an export actually writes.
//
// An ABSENT orientation stays absent -- an export must not invent metadata, and absent already
// means 1 to every reader. Every other field is copied through untouched; buildExifPayload's own
// range tests still decide what survives serialisation.
[[nodiscard]] common::ExifData exifForExport(const common::ExifData& data);

}  // namespace mosaic::io
