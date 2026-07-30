#include "io/mosaic/blob.hpp"

#include <blake3.h>

#include <algorithm>

namespace mosaic::io::native {

BlobHash blobHashOf(std::span<const std::uint8_t> content) {
    BlobHash out{};
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, content.data(), content.size());
    blake3_hasher_finalize(&h, out.data(), out.size());
    return out;
}

ChunkKey blobKeyOf(const BlobHash& hash) noexcept {
    ChunkKey k;
    std::copy(hash.begin(), hash.begin() + 16, k.bytes.begin());
    return k;
}

std::vector<std::uint8_t> makeBlobPayload(const BlobHash& hash,
                                          std::span<const std::uint8_t> content) {
    std::vector<std::uint8_t> out;
    out.reserve(hash.size() + content.size());
    out.insert(out.end(), hash.begin(), hash.end());
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

std::optional<std::span<const std::uint8_t>> blobContentOf(
    std::span<const std::uint8_t> payload) {
    if (payload.size() < kBlobHashSize)
        return std::nullopt;
    const auto content = payload.subspan(kBlobHashSize);
    const BlobHash actual = blobHashOf(content);
    if (!std::equal(actual.begin(), actual.end(), payload.begin()))
        return std::nullopt;
    return content;
}

} // namespace mosaic::io::native
