#include "formats/ico.hpp"

#include "formats/bmp.hpp"

#include <cstring>

namespace mosaicfmt {
namespace {

constexpr std::size_t kDirEntrySize = 16;
// A directory this long is not an icon set. The count field is 16 bits; nothing real uses more than
// a handful, and the cap keeps a hostile count from making us walk 64 Ki entries.
constexpr std::uint32_t kMaxEntries = 64;

constexpr std::uint8_t kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

[[nodiscard]] bool looksLikePng(const std::uint8_t* data, std::size_t size) noexcept {
    return size >= 8 && std::memcmp(data, kPngSignature, 8) == 0;
}

// The directory records a side of 256 as 0 -- there is one byte for it and 256 does not fit.
[[nodiscard]] std::uint32_t sideFromByte(std::uint8_t v) noexcept {
    return v == 0 ? kMaxIcoSide : v;
}

[[nodiscard]] std::uint8_t sideToByte(std::uint32_t v) noexcept {
    return v >= kMaxIcoSide ? std::uint8_t{0} : static_cast<std::uint8_t>(v);
}

} // namespace

std::optional<std::vector<std::uint8_t>> encodeIco(const std::vector<IcoEntry>& entries,
                                                   std::string* error) {
    if (entries.empty()) {
        fail(error, "ICO: an icon needs at least one image");
        return std::nullopt;
    }
    if (entries.size() > kMaxEntries) {
        fail(error, "ICO: too many entries");
        return std::nullopt;
    }
    for (const IcoEntry& e : entries) {
        if (!e.pixels.valid()) {
            fail(error, "ICO: an entry has no image");
            return std::nullopt;
        }
        if (e.pixels.width > kMaxIcoSide || e.pixels.height > kMaxIcoSide) {
            fail(error, "ICO: an entry is larger than 256 pixels a side");
            return std::nullopt;
        }
        if (e.png != nullptr && !looksLikePng(e.png->data(), e.png->size())) {
            fail(error, "ICO: the supplied payload is not a PNG");
            return std::nullopt;
        }
    }

    // Build every payload first: the directory has to state each one's length and offset, and
    // guessing at those before they exist is how a subtly broken icon happens.
    std::vector<std::vector<std::uint8_t>> payloads;
    payloads.reserve(entries.size());
    for (const IcoEntry& e : entries) {
        if (e.png != nullptr)
            payloads.push_back(*e.png);
        else
            payloads.push_back(encodeIcoDib(e.pixels));
        if (payloads.back().empty()) {
            fail(error, "ICO: an entry's image could not be encoded");
            return std::nullopt;
        }
    }

    ByteWriter w;
    w.u16le(0);  // reserved
    w.u16le(1);  // type: 1 = icon (2 would be a cursor, which needs a hotspot -- not this slice)
    w.u16le(static_cast<std::uint16_t>(entries.size()));
    std::size_t offset = 6u + kDirEntrySize * entries.size();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const IcoEntry& e = entries[i];
        w.u8(sideToByte(e.pixels.width));
        w.u8(sideToByte(e.pixels.height));
        w.u8(0);     // palette entry count: 0 for anything deeper than 8 bits
        w.u8(0);     // reserved
        w.u16le(1);  // colour planes
        w.u16le(32); // bits per pixel
        w.u32le(static_cast<std::uint32_t>(payloads[i].size()));
        w.u32le(static_cast<std::uint32_t>(offset));
        offset += payloads[i].size();
    }
    for (const std::vector<std::uint8_t>& p : payloads)
        w.raw(p.data(), p.size());
    return w.take();
}

std::optional<IcoPayload> selectIcoEntry(const std::uint8_t* data, std::size_t size,
                                         std::string* error) {
    if (data == nullptr || size < 6 + kDirEntrySize || size > kMaxFileBytes) {
        fail(error, "ICO: the directory is truncated");
        return std::nullopt;
    }
    ByteReader r(data, size);
    const std::uint32_t reserved = r.u16le();
    const std::uint32_t type = r.u16le();
    const std::uint32_t count = r.u16le();
    if (reserved != 0 || (type != 1 && type != 2) || count == 0) {
        fail(error, "ICO: not an icon directory");
        return std::nullopt;
    }
    if (count > kMaxEntries || !r.has(static_cast<std::uint64_t>(count) * kDirEntrySize)) {
        fail(error, "ICO: the directory declares more entries than the file holds");
        return std::nullopt;
    }

    std::optional<IcoPayload> best;
    std::uint32_t bestDepth = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        IcoPayload e;
        e.width = sideFromByte(r.u8());
        e.height = sideFromByte(r.u8());
        (void)r.u8();  // palette entry count -- advisory
        (void)r.u8();  // reserved
        (void)r.u16le();  // colour planes: 1, or 0 in plenty of real files
        const std::uint32_t depth = r.u16le();
        const std::uint32_t bytes = r.u32le();
        const std::uint32_t offset = r.u32le();
        // A slot whose payload is not inside the file is a broken SLOT, not a broken file: an icon
        // with four good sizes and one bad one still has four good sizes.
        if (bytes == 0 || offset < 6u || offset > size || bytes > size - offset)
            continue;
        e.offset = offset;
        e.size = bytes;
        e.isPng = looksLikePng(data + offset, bytes);
        const std::uint64_t area = std::uint64_t{e.width} * e.height;
        const std::uint64_t bestArea =
            best ? std::uint64_t{best->width} * best->height : 0u;
        if (!best || area > bestArea || (area == bestArea && depth > bestDepth) ||
            (area == bestArea && depth == bestDepth && e.size > best->size)) {
            best = e;
            bestDepth = depth;
        }
    }
    if (!best)
        fail(error, "ICO: no entry has a usable payload");
    return best;
}

std::optional<Bitmap> decodeIco(const std::uint8_t* data, std::size_t size, std::string* error) {
    const std::optional<IcoPayload> entry = selectIcoEntry(data, size, error);
    if (!entry)
        return std::nullopt;
    if (entry->isPng) {
        // Deliberate: no PNG decoder lives on this side of the dependency fence. The caller has
        // selectIcoEntry() and, presumably, libpng.
        fail(error, "ICO: this icon's image is stored as a PNG, which this decoder does not read");
        return std::nullopt;
    }
    return decodeDib(data + entry->offset, entry->size, /*icoEntry=*/true, error);
}

} // namespace mosaicfmt
