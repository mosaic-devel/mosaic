#pragma once

#include "io/mosaic/format.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// mosaic/codec -- the per-chunk compression profiles (spec 2.4): store / fast (LZ4) /
// balanced (zstd ~3) / max (zstd ~19), recorded per chunk so one file freely mixes them.
// Store-if-larger is mandatory, not an optimization: compressing noisy or already-compressed
// content can grow it, and the wire profile honestly records what happened.
namespace mosaic::io::native {

// Decode-side sanity ceiling on UNCOMPRESSED_LEN (a hostile or corrupted header field must not
// be able to ask the reader to allocate gigabytes; same philosophy as io/detail.hpp's kMaxDim).
// Generous: the largest legitimate chunks are directory/history tables, far below this.
inline constexpr std::uint32_t kMaxUncompressedLen = 1u << 28; // 256MB

struct Encoded {
    Profile profile; // what actually goes on the wire (Store when compression didn't pay)
    std::vector<std::uint8_t> bytes;
};

// Compress `raw` at the requested profile, with the store-if-larger fallback applied.
[[nodiscard]] Encoded compressPayload(std::span<const std::uint8_t> raw, Profile profile);

// Decompress a wire payload back to exactly `uncompressedLen` bytes. nullopt on anything
// off-contract: unknown profile, a length past kMaxUncompressedLen, a corrupt stream, or a
// stream that decodes to the wrong size. Never trusts the inputs; never crashes on garbage.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decompressPayload(
    std::span<const std::uint8_t> payload, Profile profile, std::uint32_t uncompressedLen);

} // namespace mosaic::io::native
