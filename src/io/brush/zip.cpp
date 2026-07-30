#include "io/brush/zip.hpp"

#include "io/brush/bytes.hpp"

#include <zlib.h>

#include <cstring>

namespace mosaic::io::brush {
namespace {

[[maybe_unused]] constexpr std::uint32_t kEocdSignature = 0x06054b50; // "PK\x05\x06" (clang: unused)
constexpr std::uint32_t kCentralSignature = 0x02014b50; // "PK\x01\x02"
constexpr std::uint32_t kLocalSignature = 0x04034b50;   // "PK\x03\x04"

constexpr std::size_t kEocdFixedSize = 22;
[[maybe_unused]] constexpr std::size_t kCentralFixedSize = 46; // (documentation; clang: unused)
[[maybe_unused]] constexpr std::size_t kLocalFixedSize = 30;   // (documentation; clang: unused)
// The comment length field is 16-bit, so the EOCD record starts within the last 22 + 65535 bytes.
constexpr std::size_t kEocdScanWindow = kEocdFixedSize + 0xFFFF;

void setError(std::string* error, const char* what) {
    if (error != nullptr)
        *error = what;
}

// Scan backwards for the end-of-central-directory record. The LAST signature in the file wins --
// an embedded signature inside some entry's data can only appear EARLIER than the real record.
[[nodiscard]] std::size_t findEocd(const std::uint8_t* data, std::size_t size) noexcept {
    if (size < kEocdFixedSize)
        return std::string::npos;
    const std::size_t lowest = size >= kEocdScanWindow ? size - kEocdScanWindow : 0;
    for (std::size_t pos = size - kEocdFixedSize;; --pos) {
        if (data[pos] == 0x50 && data[pos + 1] == 0x4b && data[pos + 2] == 0x05 &&
            data[pos + 3] == 0x06)
            return pos;
        if (pos == lowest)
            break;
    }
    return std::string::npos;
}

} // namespace

std::optional<ZipReader> ZipReader::open(const std::uint8_t* data, std::size_t size,
                                         std::string* error) {
    const std::size_t eocdPos = findEocd(data, size);
    if (eocdPos == std::string::npos) {
        setError(error, "not a ZIP archive (no end-of-central-directory record)");
        return std::nullopt;
    }

    detail::ByteReader eocd(data, size);
    eocd.seek(eocdPos + 4);
    const std::uint16_t diskNumber = eocd.u16le();
    const std::uint16_t centralStartDisk = eocd.u16le();
    const std::uint16_t entriesThisDisk = eocd.u16le();
    const std::uint16_t entriesTotal = eocd.u16le();
    const std::uint32_t centralSize = eocd.u32le();
    const std::uint32_t centralOffset = eocd.u32le();

    if (diskNumber != 0 || centralStartDisk != 0 || entriesThisDisk != entriesTotal) {
        setError(error, "multi-disk ZIP archives are unsupported");
        return std::nullopt;
    }
    if (entriesTotal == 0xFFFF || centralOffset == 0xFFFFFFFF || centralSize == 0xFFFFFFFF) {
        setError(error, "zip64 archives are unsupported");
        return std::nullopt;
    }
    if (centralOffset > eocdPos || centralSize > eocdPos - centralOffset) {
        setError(error, "central directory outside the archive");
        return std::nullopt;
    }
    if (entriesTotal > kMaxZipEntries) {
        setError(error, "central directory over the entry cap");
        return std::nullopt;
    }

    ZipReader zip;
    zip.m_data = data;
    zip.m_size = size;
    zip.m_entries.reserve(entriesTotal);

    // Walk the central directory. Its extent is [centralOffset, centralOffset + centralSize);
    // a record leaking past that is structural damage, not a skippable entry.
    detail::ByteReader cd(data, centralOffset + centralSize);
    cd.seek(centralOffset);
    for (std::uint32_t i = 0; i < entriesTotal; ++i) {
        if (cd.u32le() != kCentralSignature || !cd.ok()) {
            setError(error, "malformed central directory record");
            return std::nullopt;
        }
        cd.skip(4); // version made by, version needed
        const std::uint16_t flags = cd.u16le();
        const std::uint16_t method = cd.u16le();
        cd.skip(4); // dos mod time/date
        const std::uint32_t crc = cd.u32le();
        const std::uint32_t compressedSize = cd.u32le();
        const std::uint32_t uncompressedSize = cd.u32le();
        const std::uint16_t nameLen = cd.u16le();
        const std::uint16_t extraLen = cd.u16le();
        const std::uint16_t commentLen = cd.u16le();
        cd.skip(8); // disk start, internal attrs, external attrs
        const std::uint32_t localOffset = cd.u32le();
        const std::uint8_t* name = cd.bytes(nameLen);
        cd.skip(static_cast<std::size_t>(extraLen) + commentLen);
        if (!cd.ok() || name == nullptr) {
            setError(error, "truncated central directory record");
            return std::nullopt;
        }

        // Individually unusable records are skipped and counted; the rest of the archive stays
        // readable. zip64 sentinel sizes mean the real values live in an extra field this
        // reader does not parse -- treating the sentinel as a literal size would mis-bound.
        if (nameLen > kMaxZipNameBytes || std::memchr(name, 0, nameLen) != nullptr ||
            compressedSize == 0xFFFFFFFF || uncompressedSize == 0xFFFFFFFF ||
            localOffset == 0xFFFFFFFF) {
            ++zip.m_skipped;
            continue;
        }

        ZipEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(name), nameLen);
        entry.compressedSize = compressedSize;
        entry.uncompressedSize = uncompressedSize;
        entry.crc32 = crc;
        entry.method = method;
        entry.localHeaderOffset = localOffset;
        entry.encrypted = (flags & 0x1) != 0;
        zip.m_entries.push_back(std::move(entry));
    }

    return zip;
}

const ZipEntry* ZipReader::find(std::string_view name) const noexcept {
    for (const ZipEntry& entry : m_entries) {
        if (entry.name == name)
            return &entry;
    }
    return nullptr;
}

std::optional<std::vector<std::uint8_t>> ZipReader::read(const ZipEntry& entry,
                                                          std::string* error) const {
    if (entry.encrypted) {
        setError(error, "encrypted entry");
        return std::nullopt;
    }
    if (entry.method != 0 && entry.method != 8) {
        setError(error, "unsupported compression method");
        return std::nullopt;
    }
    if (entry.uncompressedSize > kMaxZipEntryBytes) {
        setError(error, "entry over the size cap");
        return std::nullopt;
    }

    // The local header is consulted only for the data offset: its name/extra lengths may differ
    // from the central copy's, and with the data-descriptor flag set (RGBA_brushes.bundle, every
    // entry) its size fields are legitimately zero. Sizes and CRC always come from the central
    // directory.
    detail::ByteReader local(m_data, m_size);
    local.seek(entry.localHeaderOffset);
    if (local.u32le() != kLocalSignature || !local.ok()) {
        setError(error, "bad local header");
        return std::nullopt;
    }
    local.skip(22); // version, flags, method, time, date, crc, sizes
    const std::uint16_t nameLen = local.u16le();
    const std::uint16_t extraLen = local.u16le();
    local.skip(static_cast<std::size_t>(nameLen) + extraLen);
    const std::uint8_t* src = local.bytes(entry.compressedSize);
    if (!local.ok() || src == nullptr) {
        setError(error, "entry data outside the archive");
        return std::nullopt;
    }

    // EXACT-size output: a decoder overrun past the declared size must trip ASan, not land in
    // vector slack (the png_text 2-byte-overread lesson).
    std::vector<std::uint8_t> out(entry.uncompressedSize);

    // Directory placeholders and empty files: nothing to decode (zlib refuses a null next_out,
    // and memcpy to a null destination is UB even at zero length). The CRC of nothing is 0.
    if (entry.uncompressedSize == 0) {
        if (entry.crc32 != 0) {
            setError(error, "CRC mismatch");
            return std::nullopt;
        }
        return out;
    }

    if (entry.method == 0) {
        if (entry.compressedSize != entry.uncompressedSize) {
            setError(error, "stored entry with mismatched sizes");
            return std::nullopt;
        }
        std::memcpy(out.data(), src, entry.uncompressedSize);
    } else {
        z_stream zs{};
        // Raw deflate: ZIP entries carry no zlib header. -15 selects raw with the full window.
        if (inflateInit2(&zs, -15) != Z_OK) {
            setError(error, "inflate init failed");
            return std::nullopt;
        }
        zs.next_in = const_cast<Bytef*>(src);
        zs.avail_in = entry.compressedSize;
        zs.next_out = out.data();
        zs.avail_out = entry.uncompressedSize;
        const int ret = inflate(&zs, Z_FINISH);
        const bool complete = ret == Z_STREAM_END && zs.avail_out == 0;
        inflateEnd(&zs);
        if (!complete) {
            // Either corrupt, truncated, or the stream wants to write past the declared size --
            // all "this entry is undecodable" to the caller.
            setError(error, "corrupt deflate stream or size mismatch");
            return std::nullopt;
        }
    }

    if (::crc32(0, out.data(), static_cast<uInt>(out.size())) != entry.crc32) {
        setError(error, "CRC mismatch");
        return std::nullopt;
    }
    return out;
}

} // namespace mosaic::io::brush
