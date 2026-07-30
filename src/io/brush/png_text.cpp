#include "io/brush/png_text.hpp"

#include <zlib.h>

#include <cstring>

namespace mosaic::io::brush {
namespace {

[[nodiscard]] std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// Inflate `n` bytes at `p` into `out`, refusing to grow past `cap`. False on a truncated or
// corrupt stream, on any non-zlib framing, and on the cap -- the caller treats all of those as
// "this chunk is undecodable", so the distinction is not surfaced.
[[nodiscard]] bool inflateBounded(const std::uint8_t* p, std::size_t n, std::size_t cap,
                                  std::string& out) {
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK)
        return false;
    out.clear();
    // A chunk payload is at most 2^31-1 bytes (checked by the walker), so it fits avail_in whole.
    zs.next_in = const_cast<Bytef*>(p);
    zs.avail_in = static_cast<uInt>(n);
    unsigned char buf[16384];
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        zs.next_out = buf;
        zs.avail_out = sizeof buf;
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            return false;
        }
        const std::size_t got = sizeof buf - zs.avail_out;
        if (out.size() + got > cap) {
            inflateEnd(&zs);
            return false;
        }
        out.append(reinterpret_cast<const char*>(buf), got);
        // Input exhausted without the stream ending: truncated. (zlib otherwise reports the
        // stall as Z_BUF_ERROR on the next call, but catching it here keeps the loop total by
        // inspection rather than by trust.)
        if (ret == Z_OK && zs.avail_in == 0 && zs.avail_out != 0) {
            inflateEnd(&zs);
            return false;
        }
    }
    inflateEnd(&zs);
    return true;
}

// Split off the NUL-terminated keyword that opens every text chunk. The spec bounds it to 1..79
// bytes; the 80-byte scan window IS that bound (a terminator any further out is not found, so an
// over-long keyword and a missing terminator fail the same way), and it caps the scan on hostile
// input too. Returns the keyword length, or npos when there is no terminator in range.
[[nodiscard]] std::size_t keywordLength(const std::uint8_t* p, std::size_t n) noexcept {
    const std::size_t scan = n < 80 ? n : 80;
    const void* nul = std::memchr(p, 0, scan);
    if (nul == nullptr)
        return std::string::npos;
    const std::size_t k = static_cast<std::size_t>(static_cast<const std::uint8_t*>(nul) - p);
    return k >= 1 ? k : std::string::npos;
}

// Decode one tEXt / zTXt / iTXt payload into `out`. False means "skip and count": a malformed
// keyword, an unknown compression method, a broken stream, or a payload over the cap.
[[nodiscard]] bool decodeTextChunk(const std::uint8_t type[4], const std::uint8_t* p,
                                   std::size_t n, PngText& out) {
    const std::size_t k = keywordLength(p, n);
    if (k == std::string::npos)
        return false;
    out.keyword.assign(reinterpret_cast<const char*>(p), k);

    if (std::memcmp(type, "tEXt", 4) == 0) {
        out.kind = PngTextKind::Text;
        const std::size_t textLen = n - k - 1;
        if (textLen > kMaxTextBytes)
            return false;
        out.text.assign(reinterpret_cast<const char*>(p + k + 1), textLen);
        return true;
    }

    if (std::memcmp(type, "zTXt", 4) == 0) {
        out.kind = PngTextKind::ZText;
        // keyword NUL method(1) deflate-stream. Method 0 (zlib deflate) is the only one defined.
        if (n < k + 2 || p[k + 1] != 0)
            return false;
        return inflateBounded(p + k + 2, n - k - 2, kMaxTextBytes, out.text);
    }

    // iTXt: keyword NUL flag(1) method(1) language NUL translated-keyword NUL text. The language
    // tag and translated keyword are skipped -- no consumer of ours reads them.
    out.kind = PngTextKind::IText;
    if (n < k + 3)
        return false;
    const std::uint8_t flag = p[k + 1];
    const std::uint8_t method = p[k + 2];
    std::size_t pos = k + 3;
    for (int field = 0; field < 2; ++field) {
        const void* nul = std::memchr(p + pos, 0, n - pos);
        if (nul == nullptr)
            return false;
        pos = static_cast<std::size_t>(static_cast<const std::uint8_t*>(nul) - p) + 1;
    }
    if (flag == 0) {
        const std::size_t textLen = n - pos;
        if (textLen > kMaxTextBytes)
            return false;
        out.text.assign(reinterpret_cast<const char*>(p + pos), textLen);
        return true;
    }
    if (flag == 1 && method == 0)
        return inflateBounded(p + pos, n - pos, kMaxTextBytes, out.text);
    return false;
}

} // namespace

const PngText* PngTextScan::find(std::string_view keyword) const noexcept {
    for (const PngText& c : chunks)
        if (c.keyword == keyword)
            return &c;
    return nullptr;
}

std::optional<PngTextScan> scanPngText(const std::uint8_t* data, std::size_t size,
                                       std::string* error) {
    const auto fail = [&](const char* why) -> std::optional<PngTextScan> {
        if (error != nullptr)
            *error = why;
        return std::nullopt;
    };

    static constexpr std::uint8_t kSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (data == nullptr || size < 8 || std::memcmp(data, kSig, 8) != 0)
        return fail("not a PNG");

    PngTextScan scan;
    std::size_t totalText = 0;
    std::size_t pos = 8;
    while (true) {
        // length(4) type(4) data(length) crc(4)
        if (size - pos < 12)
            return fail("truncated PNG: chunk header runs past the end of the file");
        const std::uint32_t len = be32(data + pos);
        if (len > 0x7FFFFFFFu)
            return fail("malformed PNG: chunk length out of range");
        if (size - pos - 12 < len)
            return fail("truncated PNG: chunk data runs past the end of the file");
        const std::uint8_t* type = data + pos + 4;
        const std::uint8_t* payload = type + 4;

        const bool isText = std::memcmp(type, "tEXt", 4) == 0 ||
                            std::memcmp(type, "zTXt", 4) == 0 ||
                            std::memcmp(type, "iTXt", 4) == 0;
        if (isText) {
            // CRC covers type + data. Verified for the chunks we consume only; the pixel chunks
            // are the image decoder's business.
            const std::uint32_t stored = be32(payload + len);
            const std::uint32_t computed = static_cast<std::uint32_t>(
                crc32_z(crc32_z(0, nullptr, 0), type, static_cast<z_size_t>(len) + 4));
            PngText text;
            if (stored == computed &&
                static_cast<int>(scan.chunks.size()) < kMaxTextChunks &&
                decodeTextChunk(type, payload, len, text) &&
                text.text.size() <= kMaxTotalTextBytes - totalText) {
                totalText += text.text.size();
                scan.chunks.push_back(std::move(text));
            } else {
                ++scan.undecodable;
            }
        }

        if (std::memcmp(type, "IEND", 4) == 0)
            return scan;
        pos += 12 + static_cast<std::size_t>(len);
    }
}

} // namespace mosaic::io::brush
