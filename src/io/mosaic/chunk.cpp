#include "io/mosaic/chunk.hpp"

#include "io/mosaic/codec.hpp"
#include "io/mosaic/wire.hpp"

#include <blake3.h>

#define XXH_INLINE_ALL // vendored single-header usage (third_party/xxhash) -- no link dependency
#include <xxhash.h>

#include <algorithm>
#include <cassert>

namespace mosaic::io::native {

using detail::loadLe32;
using detail::loadLe64;
using detail::storeLe32;
using detail::storeLe64;

namespace {

[[nodiscard]] std::size_t checksumSizeFor(const ChunkTag& type) noexcept {
    return type == kTypeRoot ? kStrongChecksumSize : kFastChecksumSize;
}

// Checksum over one contiguous range (frames are packed contiguously, so the checked region --
// header-after-MAGIC [+ LINK] + payload -- is always a single span).
void computeChecksum(const ChunkTag& type, std::span<const std::uint8_t> checked,
                     std::uint8_t* out) noexcept {
    if (type == kTypeRoot) {
        blake3_hasher h;
        blake3_hasher_init(&h);
        blake3_hasher_update(&h, checked.data(), checked.size());
        blake3_hasher_finalize(&h, out, kStrongChecksumSize);
    } else {
        storeLe64(out, XXH3_64bits(checked.data(), checked.size()));
    }
}

} // namespace

void appendPreamble(std::vector<std::uint8_t>& out, std::uint8_t documentType,
                    std::uint8_t version) {
    const std::size_t base = out.size();
    out.resize(base + kPreambleSize, 0);
    std::copy(kPreambleMagic.begin(), kPreambleMagic.end(), out.begin() + static_cast<std::ptrdiff_t>(base));
    out[base + 8] = version;
    out[base + 9] = documentType;
}

std::optional<Preamble> parsePreamble(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size < kPreambleSize)
        return std::nullopt;
    if (!std::equal(kPreambleMagic.begin(), kPreambleMagic.end(), data))
        return std::nullopt;
    return Preamble{data[8], data[9]};
}

ChunkKey tileKey(std::uint64_t layerId, std::uint32_t tx, std::uint32_t ty) noexcept {
    ChunkKey k;
    storeLe64(k.bytes.data(), layerId);
    storeLe32(k.bytes.data() + 8, tx);
    storeLe32(k.bytes.data() + 12, ty);
    return k;
}

ChunkKey vectorKey(std::uint64_t layerId) noexcept {
    ChunkKey k;
    storeLe64(k.bytes.data(), layerId);
    return k;
}

ChunkKey histKey(std::uint64_t stateId) noexcept {
    ChunkKey k;
    storeLe64(k.bytes.data(), stateId);
    return k;
}

ChunkKey parityKey(std::uint64_t stripeIndex) noexcept {
    ChunkKey k;
    storeLe64(k.bytes.data(), stripeIndex);
    return k;
}

AppendedChunk appendChunk(std::vector<std::uint8_t>& out, ChunkTag type, const ChunkKey& key,
                          std::uint64_t generation, std::span<const std::uint8_t> payload,
                          Profile profile, std::uint8_t flags,
                          const std::array<std::uint8_t, kLinkSize>* link) {
    assert(payload.size() <= kMaxUncompressedLen && "chunk payloads are bounded");
    if (link != nullptr)
        flags |= kFlagLinked;
    else
        flags &= static_cast<std::uint8_t>(~kFlagLinked);

    const Encoded encoded = compressPayload(payload, profile);
    const std::span<const std::uint8_t> wire = encoded.bytes;

    const std::size_t start = out.size();
    const std::size_t linkBytes = (link != nullptr) ? kLinkSize : 0;
    const std::size_t suffix = checksumSizeFor(type);
    out.resize(start + kHeaderSize + linkBytes + wire.size() + suffix);
    std::uint8_t* p = out.data() + start;

    std::copy(kChunkMagic.begin(), kChunkMagic.end(), p);
    std::copy(type.begin(), type.end(), p + kOffType);
    p[kOffFlags] = flags;
    p[kOffProfile] = static_cast<std::uint8_t>(encoded.profile); // what ACTUALLY went on the wire
    std::copy(key.bytes.begin(), key.bytes.end(), p + kOffKey);
    storeLe64(p + kOffGeneration, generation);
    storeLe32(p + kOffUncompressedLen, static_cast<std::uint32_t>(payload.size()));
    storeLe32(p + kOffPayloadLen, static_cast<std::uint32_t>(wire.size()));
    if (link != nullptr)
        std::copy(link->begin(), link->end(), p + kHeaderSize);
    std::copy(wire.begin(), wire.end(), p + kHeaderSize + linkBytes);

    // The checked region: everything after MAGIC, through the payload.
    const std::size_t checkedLen = (kHeaderSize - 8) + linkBytes + wire.size();
    computeChecksum(type, {p + 8, checkedLen}, p + kHeaderSize + linkBytes + wire.size());

    AppendedChunk appended;
    appended.offset = start;
    appended.length = out.size() - start;
    appended.checksumSize = static_cast<std::uint8_t>(suffix);
    std::copy(p + kHeaderSize + linkBytes + wire.size(),
              p + kHeaderSize + linkBytes + wire.size() + suffix, appended.checksum.begin());
    return appended;
}

std::optional<std::vector<std::uint8_t>> decodeChunkPayload(const ChunkRecord& rec,
                                                            std::span<const std::uint8_t> buf) {
    if (!rec.valid)
        return std::nullopt;
    return decompressPayload(rec.payload(buf), static_cast<Profile>(rec.profile),
                             rec.uncompressedLen);
}

std::optional<ChunkRecord> parseChunkAt(std::span<const std::uint8_t> buf, std::size_t offset) {
    if (offset >= buf.size() || buf.size() - offset < kChunkMagic.size())
        return std::nullopt;
    if (!std::equal(kChunkMagic.begin(), kChunkMagic.end(), buf.data() + offset))
        return std::nullopt;

    ChunkRecord rec;
    rec.offset = offset;
    if (buf.size() - offset < kHeaderSize)
        return rec; // magic present, header torn off: incomplete

    const std::uint8_t* p = buf.data() + offset;
    std::copy(p + kOffType, p + kOffType + 4, rec.type.begin());
    rec.flags = p[kOffFlags];
    rec.profile = p[kOffProfile];
    std::copy(p + kOffKey, p + kOffKey + 16, rec.key.bytes.begin());
    rec.generation = loadLe64(p + kOffGeneration);
    rec.uncompressedLen = loadLe32(p + kOffUncompressedLen);
    rec.payloadLen = loadLe32(p + kOffPayloadLen);

    const std::size_t linkBytes = rec.linked() ? kLinkSize : 0;
    const std::size_t suffix = checksumSizeFor(rec.type);
    rec.checksumSize = static_cast<std::uint8_t>(suffix);
    rec.payloadOffset = offset + kHeaderSize + linkBytes;

    // Overflow-safe frame extent check: every operand below is bounded by u32 + small constants.
    const std::size_t frameLen =
        kHeaderSize + linkBytes + static_cast<std::size_t>(rec.payloadLen) + suffix;
    rec.consumed = frameLen;
    if (frameLen > buf.size() - offset)
        return rec; // truncated (or a corrupted length field claims more than exists): incomplete

    rec.complete = true;
    if (rec.linked())
        std::copy(p + kHeaderSize, p + kHeaderSize + kLinkSize, rec.link.begin());
    std::copy(p + kHeaderSize + linkBytes + rec.payloadLen,
              p + kHeaderSize + linkBytes + rec.payloadLen + suffix, rec.checksum.begin());

    std::array<std::uint8_t, kStrongChecksumSize> expected{};
    const std::size_t checkedLen = (kHeaderSize - 8) + linkBytes + rec.payloadLen;
    computeChecksum(rec.type, {p + 8, checkedLen}, expected.data());
    rec.valid = std::equal(expected.begin(), expected.begin() + static_cast<std::ptrdiff_t>(suffix),
                           rec.checksum.begin());
    return rec;
}

std::vector<ChunkRecord> scanChunks(std::span<const std::uint8_t> buf, std::size_t start) {
    std::vector<ChunkRecord> out;
    auto it = buf.begin() + static_cast<std::ptrdiff_t>(std::min(start, buf.size()));
    while (true) {
        it = std::search(it, buf.end(), kChunkMagic.begin(), kChunkMagic.end());
        if (it == buf.end())
            break;
        const std::size_t offset = static_cast<std::size_t>(it - buf.begin());
        std::optional<ChunkRecord> rec = parseChunkAt(buf, offset);
        if (!rec.has_value()) {
            ++it; // cannot happen (search matched), but never loop forever
            continue;
        }
        out.push_back(*rec);
        // A valid chunk is consumed wholesale (payload bytes that merely look like MAGIC are
        // never considered); anything else advances one byte and resyncs.
        it += static_cast<std::ptrdiff_t>(rec->valid ? rec->consumed : 1);
    }
    return out;
}

} // namespace mosaic::io::native
