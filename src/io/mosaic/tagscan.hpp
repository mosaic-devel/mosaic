#pragma once

#include "io/mosaic/format.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// mosaic/tagscan -- find a few chunks in a file without reading the file.
//
// The container's ground-truth scan (chunk.hpp scanChunks) takes a span, so every caller that
// wanted ONE chunk out of a document first pulled the whole document into memory. For a recents
// card that meant reading 302 MB and hashing every tile to answer "how big is this canvas, and is
// there a thumbnail" -- two frames out of 33,664, both of them small, both of them near the end.
//
// This walks the FRAME CHAIN on disk instead. Each step reads a 46-byte header, and unless the tag
// is one being asked for, seeks past the payload rather than reading it. Bytes read go from "the
// file" to "the headers plus the frames you wanted", and the cost stops scaling with the document.
//
// ⚠ RECOVERY SEMANTICS ARE PRESERVED, not traded away. A header's length field is a claim, not a
// fact -- the checksum is what makes a frame true, and it covers the payload. So:
//
//   * a frame whose tag is WANTED is read in full and verified by parseChunkAt, exactly as the
//     in-memory scan would. An unverified frame is never returned.
//   * a frame that is skipped is skipped on the strength of its length field, and the walk lands
//     where that field says the next header is. If MAGIC is not there, it RESYNCS -- hunting
//     forward for the next magic from just past the bad header -- which is precisely what
//     scanChunks does after an invalid record. A corrupted length costs a resync, never a wrong
//     answer.
//
// The one thing it deliberately does NOT do is notice damage in frames nobody asked about. That is
// the full scan's job (openDocument), and a card reader has never reported it anyway.
namespace mosaic::io::native {

// The newest VALID frame's DECODED payload for each tag in `tags`, in the same order; nullopt for
// a tag the file does not carry (or carries only in damaged copies). "Newest" is the
// highest-generation frame, later-wins on a tie -- the replica rule readers apply everywhere
// (format.hpp).
//
// Returns a vector of `tags.size()` entries. An unreadable file yields all-nullopt rather than an
// error: every caller of this treats a missing chunk and an unreadable file the same way.
[[nodiscard]] std::vector<std::optional<std::vector<std::uint8_t>>> readNewestChunkPayloads(
    const std::string& path, std::span<const ChunkTag> tags);

} // namespace mosaic::io::native
