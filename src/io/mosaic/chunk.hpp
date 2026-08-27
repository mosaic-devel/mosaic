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

// The largest UNCOMPRESSED payload a chunk of this type may legitimately carry.
//
// codec.hpp's kMaxUncompressedLen is one ceiling for every type, and 256 MB is the right order for
// a directory or a history table. For a TILE it is 16,384 times too generous: a tile is one 64x64
// cell at no more than 4 bytes per pixel, so 16 KB is not a heuristic, it is the format's own
// arithmetic (docio.cpp builds them as tw * th * bpp).
//
// ⚠ THIS IS A MEMORY-EXHAUSTION BOUND, not tidiness. A frame's UNCOMPRESSED_LEN is checksum-
// covered, so it cannot be forged into an existing file -- but a file crafted from scratch carries
// whatever checksum its author computed, and zeros compress about 32,000:1. A 2 MB file holding 160
// valid TILE frames that each honestly expand to 256 MB drove openDocument to 23.6 GB of resident
// memory in 18 seconds, because the full-scan path RETAINS one decoded payload per (type, key).
// With the tile bound applied the same file costs 160 x 16 KB.
[[nodiscard]] std::size_t maxUncompressedFor(const ChunkTag& type) noexcept;

// The fields a 46-byte header carries, WITHOUT reading or verifying the payload -- everything
// needed to decide "do I want this frame?" and "where does the next one start?".
//
// ⚠ NOTHING HERE IS VERIFIED. The checksum covers the payload, so a header alone cannot say whether
// a frame is intact; `frameLength` is a claim by a length field that may itself be damaged. This
// exists so a reader after ONE chunk out of a 300 MB file can seek past the rest instead of
// hashing them -- it lands on the next header and, if the magic is not there, resyncs exactly as
// scanChunks does. Anything that cares whether a frame is GOOD still reads it and calls
// parseChunkAt.
struct ChunkHeaderView {
    ChunkTag type{};
    std::uint8_t flags = 0;
    std::uint64_t generation = 0;
    std::uint32_t payloadLen = 0;
    std::size_t frameLength = 0; // header [+ link] + payload + checksum, per the length field
};

// Parse a header from at least kHeaderSize bytes beginning with MAGIC. nullopt when the buffer is
// too short or the magic is absent.
[[nodiscard]] std::optional<ChunkHeaderView> parseChunkHeader(std::span<const std::uint8_t> buf);

// The checksum suffix a tag carries (ROOT is BLAKE3-32, everything else xxh3-8). Public because
// frame length is header + link + payload + THIS, and a streaming reader has to compute it.
[[nodiscard]] std::size_t chunkChecksumSize(const ChunkTag& type) noexcept;

// Linear magic-resync scan (spec 2.8, the full-scan fallback's engine): every chunk found, valid
// or not, in file order. After an invalid or incomplete record the scan advances one byte and
// hunts for the next MAGIC, so a corrupted length field cannot derail it; a valid chunk is
// consumed wholesale, so magic-lookalike bytes inside a valid payload are never even considered.
[[nodiscard]] std::vector<ChunkRecord> scanChunks(std::span<const std::uint8_t> buf,
                                                  std::size_t start = 0);

} // namespace mosaic::io::native
