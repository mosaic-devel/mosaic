#include "io/mosaic/codec.hpp"

#include <lz4.h>
#include <zstd.h>

#include <cassert>

namespace mosaic::io::native {
namespace {

// Spec 2.4's "~3 / ~19". Frozen here rather than exposed: changing them never breaks reading
// (the wire records the profile, and zstd/LZ4 decode any level's output), but a deliberate
// retune should be a measured, committed decision, not a call-site drift.
constexpr int kZstdBalancedLevel = 3;
constexpr int kZstdMaxLevel = 19;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> tryCompress(
    std::span<const std::uint8_t> raw, Profile profile) {
    if (raw.empty())
        return std::nullopt; // nothing compresses below zero bytes; Store wins by definition
    if (profile == Profile::Fast) {
        if (raw.size() > LZ4_MAX_INPUT_SIZE)
            return std::nullopt;
        std::vector<std::uint8_t> out(static_cast<std::size_t>(
            LZ4_compressBound(static_cast<int>(raw.size()))));
        const int n = LZ4_compress_default(reinterpret_cast<const char*>(raw.data()),
                                           reinterpret_cast<char*>(out.data()),
                                           static_cast<int>(raw.size()),
                                           static_cast<int>(out.size()));
        if (n <= 0)
            return std::nullopt;
        out.resize(static_cast<std::size_t>(n));
        return out;
    }
    const int level = (profile == Profile::Max) ? kZstdMaxLevel : kZstdBalancedLevel;
    std::vector<std::uint8_t> out(ZSTD_compressBound(raw.size()));
    const std::size_t n =
        ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), level);
    if (ZSTD_isError(n))
        return std::nullopt;
    out.resize(n);
    return out;
}

} // namespace

Encoded compressPayload(std::span<const std::uint8_t> raw, Profile profile) {
    assert(raw.size() <= kMaxUncompressedLen && "chunk payloads are bounded (kMaxUncompressedLen)");
    if (profile != Profile::Store) {
        if (auto compressed = tryCompress(raw, profile);
            compressed.has_value() && compressed->size() < raw.size())
            return {profile, std::move(*compressed)};
    }
    return {Profile::Store, {raw.begin(), raw.end()}};
}

std::optional<std::vector<std::uint8_t>> decompressPayload(std::span<const std::uint8_t> payload,
                                                           Profile profile,
                                                           std::uint32_t uncompressedLen) {
    if (uncompressedLen > kMaxUncompressedLen)
        return std::nullopt;
    switch (profile) {
    case Profile::Store: {
        if (payload.size() != uncompressedLen)
            return std::nullopt;
        return std::vector<std::uint8_t>(payload.begin(), payload.end());
    }
    case Profile::Fast: {
        if (payload.size() > LZ4_MAX_INPUT_SIZE)
            return std::nullopt;
        std::vector<std::uint8_t> out(uncompressedLen);
        // The _safe decoder: bounded writes whatever the (untrusted) stream claims.
        const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(payload.data()),
                                          reinterpret_cast<char*>(out.data()),
                                          static_cast<int>(payload.size()),
                                          static_cast<int>(out.size()));
        if (n < 0 || static_cast<std::uint32_t>(n) != uncompressedLen)
            return std::nullopt;
        return out;
    }
    case Profile::Balanced:
    case Profile::Max: {
        std::vector<std::uint8_t> out(uncompressedLen);
        const std::size_t n =
            ZSTD_decompress(out.data(), out.size(), payload.data(), payload.size());
        if (ZSTD_isError(n) || n != uncompressedLen)
            return std::nullopt;
        return out;
    }
    }
    return std::nullopt; // unknown profile byte (corrupted, or a future format revision)
}

} // namespace mosaic::io::native
