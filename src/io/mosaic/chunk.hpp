#pragma once

#include "io/mosaic/format.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// mosaic/chunk -- self-describing chunk framing for the .mosaic container (spec 2.2, 2.8): pack,
// parse-at-offset, and the linear magic-resync scan that is recovery's ground truth.
//
// The checksum covers every header field after MAGIC, the LINK field when present, and the
// payload. That coverage is load-bearing, not stylistic: a bit flip isolated to KEY or GENERATION
// must fail verification exactly like payload damage, or an intact, checksum-passing payload gets
// silently attributed to the wrong logical tile (found empirically in the research prototype).
// ROOT chunks carry a 32-byte BLAKE3 checksum (the ZFS importance-weighted idea); everything else
// 8-byte xxh3-64.
namespace mosaic::io::native {

struct ChunkRecord {
    std::size_t offset = 0; // absolute offset of MAGIC in the buffer
    ChunkTag type{};
    std::uint8_t flags = 0;
    std::uint8_t profile = 0;
    ChunkKey key{};
    std::uint64_t generation = 0;
    std::uint32_t uncompressedLen = 0;
    std::uint32_t payloadLen = 0;
    std::size_t payloadOffset = 0; // absolute; sits after LINK when the frame is linked
    std::size_t consumed = 0;      // whole frame: header [+ link] + payload + checksum
    bool complete = false;         // the whole frame lies inside the buffer
    bool valid = false;            // complete AND the checksum verified
    std::array<std::uint8_t, kLinkSize> link{}; // meaningful iff linked()
    std::array<std::uint8_t, kStrongChecksumSize> checksum{}; // stored bytes; first checksumSize used
    std::uint8_t checksumSize = 0;

    [[nodiscard]] bool linked() const noexcept { return (flags & kFlagLinked) != 0; }
    [[nodiscard]] std::span<const std::uint8_t> payload(
        std::span<const std::uint8_t> buf) const noexcept {
        return buf.subspan(payloadOffset, payloadLen);
    }
    // The stored checksum, sized -- what a linked successor's LINK field must equal (truncated to
    // kLinkSize for ROOT-strong checksums).
    [[nodiscard]] std::span<const std::uint8_t> checksumBytes() const noexcept {
        return {checksum.data(), checksumSize};
    }
    [[nodiscard]] std::array<std::uint8_t, kLinkSize> linkValue() const noexcept {
        std::array<std::uint8_t, kLinkSize> v{};
        for (std::size_t i = 0; i < kLinkSize; ++i)
            v[i] = checksum[i];
        return v;
    }
};

// What appendChunk just wrote: enough for a writer to thread an explicit-link chain in memory
// (spec 2.6: neither the journal's nor the file's running link may be re-derived by scanning on
// every append -- the chain-reset bug that only a many-sequential-writes test can see).
struct AppendedChunk {
    std::size_t offset = 0; // frame start in `out`
    std::size_t length = 0; // whole frame
    std::array<std::uint8_t, kStrongChecksumSize> checksum{};
    std::uint8_t checksumSize = 0;
    [[nodiscard]] std::array<std::uint8_t, kLinkSize> linkValue() const noexcept {
        std::array<std::uint8_t, kLinkSize> v{};
        for (std::size_t i = 0; i < kLinkSize; ++i)
            v[i] = checksum[i];
        return v;
    }
};

// Append one framed chunk to `out`, compressing `payload` at `profile` (store-if-larger applies
// automatically -- the wire PROFILE byte records what actually happened, codec.hpp). `link`
// non-null makes this a linked frame (sets kFlagLinked; the 8 bytes ride between header and
// payload, covered by the checksum).
AppendedChunk appendChunk(std::vector<std::uint8_t>& out, ChunkTag type, const ChunkKey& key,
                          std::uint64_t generation, std::span<const std::uint8_t> payload,
                          Profile profile = Profile::Store, std::uint8_t flags = kFlagCritical,
                          const std::array<std::uint8_t, kLinkSize>* link = nullptr);

// Decompress a VALID record's payload back to its uncompressed bytes (per the record's own wire
// profile). nullopt on an unknown profile byte, an off-contract length, or a corrupt stream --
// possible even though the checksum passed, e.g. a bug in a foreign writer; never trust, never
// crash. (Spatial unfiltering, kFlagFiltered, is the caller's business -- the codec layer is
// bytes-only.)
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decodeChunkPayload(
    const ChunkRecord& rec, std::span<const std::uint8_t> buf);

// Parse the chunk whose MAGIC begins exactly at `offset`. nullopt when the magic bytes are not
// there at all; otherwise a record whose complete/valid flags say how far it got. Length fields
// are never trusted past a failed verification -- on !valid, resync by hunting for the next
// MAGIC (PNG's one structural weakness, closed the EBML way).
[[nodiscard]] std::optional<ChunkRecord> parseChunkAt(std::span<const std::uint8_t> buf,
                                                      std::size_t offset);

// Linear magic-resync scan (spec 2.8, the full-scan fallback's engine): every chunk found, valid
// or not, in file order. After an invalid or incomplete record the scan advances one byte and
// hunts for the next MAGIC, so a corrupted length field cannot derail it; a valid chunk is
// consumed wholesale, so magic-lookalike bytes inside a valid payload are never even considered.
[[nodiscard]] std::vector<ChunkRecord> scanChunks(std::span<const std::uint8_t> buf,
                                                  std::size_t start = 0);

} // namespace mosaic::io::native
