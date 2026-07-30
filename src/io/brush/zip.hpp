#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A read-only ZIP walker (docs/brushes.md §3.7) -- the container under `.bundle`. Hand-rolled
// over zlib, like the PNG chunk walker (png_text.cpp): zlib is already a direct dependency, the
// two compression methods real bundles use (stored, deflate) are all it takes, and owning the
// walk keeps the hostile-input caps and honesty counters in one auditable place.
//
// The input is a third-party file, so the shape is the io/brush standard: parsing is total (a
// listing or an error, never a crash), every read is bounds-checked, damage is skipped and
// COUNTED rather than silently dropped, and per-entry output is an EXACT-size buffer.
//
// Format facts the implementation rests on (each verified against the shipped bundles):
//   * Sizes and CRCs come from the CENTRAL directory. RGBA_brushes.bundle sets the
//     data-descriptor flag on every entry, so its local headers say 0/0/0 -- a reader trusting
//     them loses the whole archive. The local header is consulted only for the data offset
//     (its own name/extra lengths may legally differ from the central copy's).
//   * The mimetype entry is NOT always stored-first: Krita_4 stores it, RGBA_brushes deflates
//     it. Enforcing the ODF "stored first" rule would reject a bundle Krita itself ships, so
//     this layer does not care and the bundle layer checks only the VALUE.
//   * Directory entries (name ending '/') are listed verbatim; skipping them is the caller's
//     policy, not the container's.
//   * zip64 is not supported: an archive whose end-of-central-directory carries the sentinel
//     values fails to open; a lone entry with sentinel sizes is skipped and counted.
namespace mosaic::io::brush {

struct ZipEntry {
    std::string name; // verbatim central-directory name; '/' separators, no normalization
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint32_t crc32 = 0;
    std::uint16_t method = 0; // 0 = stored, 8 = deflate; anything else fails at read()
    std::uint32_t localHeaderOffset = 0;
    bool encrypted = false; // general-purpose bit 0; read() refuses
};

inline constexpr std::uint32_t kMaxZipEntries = 16384;      // Krita_4 has 285
inline constexpr std::size_t kMaxZipNameBytes = 4096;
inline constexpr std::size_t kMaxZipEntryBytes = 64u << 20; // one uncompressed entry

class ZipReader {
public:
    // A non-owning view: `data` must outlive the reader and every read() through it.
    [[nodiscard]] static std::optional<ZipReader> open(const std::uint8_t* data, std::size_t size,
                                                       std::string* error = nullptr);

    [[nodiscard]] const std::vector<ZipEntry>& entries() const noexcept { return m_entries; }

    // First entry with this exact name, or nullptr. ZIP names are case-sensitive byte strings.
    [[nodiscard]] const ZipEntry* find(std::string_view name) const noexcept;

    // Decompress one entry, CRC-verified, into an exact-size buffer. Fails (nullopt + reason)
    // on an encrypted entry, an unknown method, a size over kMaxZipEntryBytes, a local header
    // that contradicts the archive bounds, a corrupt stream, or a CRC mismatch.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> read(const ZipEntry& entry,
                                                                std::string* error = nullptr) const;

    // Central-directory records dropped at open(): a name over the cap or containing NUL, or
    // zip64 sentinel fields. The honesty counter -- a damaged archive must not look intact.
    [[nodiscard]] int skippedEntries() const noexcept { return m_skipped; }

private:
    const std::uint8_t* m_data = nullptr;
    std::size_t m_size = 0;
    std::vector<ZipEntry> m_entries;
    int m_skipped = 0;
};

} // namespace mosaic::io::brush
