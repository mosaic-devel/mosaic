#pragma once

#include "io/mosaic/format.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// mosaic/blob -- the BLOB chunk of cas-mode history (spec 2.2, 3.9; S48 Build 2). Content-
// addressed storage for retained undo states: a state's after-image is stored ONCE per unique
// content, keyed by its BLAKE3 hash, and HIST records reference hashes instead of carrying a
// frame per (key, generation). KEY(16) is the first 128 bits of the hash -- the chunk's
// file-level identity -- while every equality decision (dedup on write, reference resolution on
// read) is made on the full 32-byte hash, which rides at the head of the payload.
//
// The hash covers whole, fixed-size tile/vector payloads exactly as they would be framed --
// chunk boundaries are set in advance by the document's own tiling grid, never derived from the
// content (spec 4's standing rule for this design).
namespace mosaic::io::native {

inline constexpr std::size_t kBlobHashSize = 32; // full BLAKE3; the KEY takes the first half

using BlobHash = std::array<std::uint8_t, kBlobHashSize>;

[[nodiscard]] BlobHash blobHashOf(std::span<const std::uint8_t> content);

[[nodiscard]] ChunkKey blobKeyOf(const BlobHash& hash) noexcept;

// The BLOB payload spelling: the full 32-byte hash, then the content bytes verbatim.
[[nodiscard]] std::vector<std::uint8_t> makeBlobPayload(const BlobHash& hash,
                                                        std::span<const std::uint8_t> content);

// The content view of a decoded BLOB payload. nullopt when the payload is too short or the
// stored head hash does not match the content's actual BLAKE3 -- the frame checksum already
// verified the bytes, so a mismatch means a writer bug, and trusting its claimed identity would
// resolve some state's reference to content it never held.
[[nodiscard]] std::optional<std::span<const std::uint8_t>> blobContentOf(
    std::span<const std::uint8_t> payload);

} // namespace mosaic::io::native
