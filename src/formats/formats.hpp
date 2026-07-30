#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// libmosaicformats -- the hand-rolled, dependency-free codecs (Export & I/O plan §2.2).
//
// The whole point of this library is that it depends on NOTHING: not on common::Image, not on
// mosaic::io, not on a system codec, only on the standard library. Its currency is a pixel-buffer
// view (ImageView / Bitmap below), so it can be built and tested on its own -- which is what
// makes §11's "libmosaicformats tested standalone, no Mosaic deps" a property of the code rather
// than a promise. The adapter that turns a common::Image into an ImageView lives on the far side
// of the fence, in src/io/backends/mosaicformats_backend.cpp.
//
// Pixels are always 8-bit STRAIGHT-alpha RGBA, tightly packed, top row first -- the one layout
// the rest of Mosaic speaks. A format that stores something else (BGR, bottom-up, palette,
// RGBE, bilevel) converts on the way in and on the way out; the seams never leak.
//
// Every decoder here is a hostile-input surface: these files arrive from the internet. The house
// discipline is io/exif.cpp's, and it is not negotiable:
//   * hard caps on declared dimensions and payload size, checked BEFORE the first allocation;
//   * every offset and length validated against the ACTUAL buffer, never against another
//     declared field;
//   * a STRUCTURAL lie (an offset outside the file, a row that cannot fit, a palette index with
//     no palette) rejects the file whole; an ABSURD VALUE in a field that is only decoration
//     drops that field alone;
//   * nothing allocates in proportion to a declared count.
namespace mosaicfmt {

// ---------------------------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------------------------

// Deliberately the same pair as io::detail::kMaxDim / kMaxPixels: a decoder that accepted a
// larger image than the rest of the application can hold would only move the failure later.
// The area cap is the one that matters -- 30000 x 30000 passes both per-side checks and asks for
// 3.6 GB, which is an out-of-memory kill rather than an error message.
inline constexpr std::uint32_t kMaxDim = 30000;
inline constexpr std::uint64_t kMaxPixels = std::uint64_t{1} << 28;

// A ceiling on an encoded file we are willing to walk. Real BMP/TGA/PNM files of legal
// dimensions stay far below it; it exists so a decoder cannot be handed an arbitrarily long
// buffer to scan (the PNM ASCII reader is the one that would care).
inline constexpr std::size_t kMaxFileBytes = std::size_t{1} << 30;

[[nodiscard]] constexpr bool dimensionsPlausible(std::uint64_t width,
                                                 std::uint64_t height) noexcept {
    return width != 0 && height != 0 && width <= kMaxDim && height <= kMaxDim &&
           width * height <= kMaxPixels;
}

// ---------------------------------------------------------------------------------------------
// Pixel buffers
// ---------------------------------------------------------------------------------------------

struct Rgba8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
    bool operator==(const Rgba8&) const = default;
};

// An opaque colour, for the formats that cannot store transparency at all (PNM, Radiance HDR,
// 24-bit BMP/TGA). The caller's "Matte" choice; alpha is composited onto it rather than dropped.
struct Rgb8 {
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
};

// A BORROWED, read-only view of 8-bit straight-alpha RGBA. This is the entire input surface of
// every encoder here -- no ownership, no allocation, no Mosaic type.
struct ImageView {
    const std::uint8_t* rgba = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool valid() const noexcept {
        return rgba != nullptr && dimensionsPlausible(width, height);
    }
    [[nodiscard]] const std::uint8_t* at(std::uint32_t x, std::uint32_t y) const noexcept {
        return rgba + (static_cast<std::size_t>(y) * width + x) * 4;
    }
};

// An OWNED decoded image, same layout. `consistent()` is what a caller (and every hostile-input
// test) checks: the buffer really is width * height * 4 bytes.
struct Bitmap {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;

    Bitmap() = default;
    Bitmap(std::uint32_t w, std::uint32_t h)
        : width(w), height(h), rgba(static_cast<std::size_t>(w) * h * 4, 0) {}

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
    [[nodiscard]] bool consistent() const noexcept {
        return rgba.size() == static_cast<std::size_t>(width) * height * 4;
    }
    [[nodiscard]] ImageView view() const noexcept { return ImageView{rgba.data(), width, height}; }
    [[nodiscard]] std::uint8_t* at(std::uint32_t x, std::uint32_t y) noexcept {
        return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
    }
    [[nodiscard]] const std::uint8_t* at(std::uint32_t x, std::uint32_t y) const noexcept {
        return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
    }
};

// An indexed source, for BMP's 8-bit and RLE8 modes. The PALETTE IS THE CALLER'S: choosing
// colours is policy (a median cut, a dither, an exactness carve-out), not codec work, and
// Mosaic already owns one quantizer (io/quantize.hpp) that the loss banner quotes. A codec that
// grew its own would give the same picture two different answers.
struct IndexedView {
    const std::uint8_t* indices = nullptr;  // width * height, one entry per pixel
    const Rgba8* palette = nullptr;
    std::uint32_t paletteSize = 0;  // 1..256
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool valid() const noexcept {
        return indices != nullptr && palette != nullptr && paletteSize >= 1 &&
               paletteSize <= 256 && dimensionsPlausible(width, height);
    }
};

// Composite one straight-alpha pixel over an opaque matte. The +127 is round-to-nearest: a
// truncating blend darkens every soft edge by up to one code value, and an edge is exactly where
// anyone would notice.
inline void compositeOverMatte(const std::uint8_t* px, Rgb8 matte, std::uint8_t out[3]) noexcept {
    const unsigned a = px[3];
    const unsigned inv = 255u - a;
    out[0] = static_cast<std::uint8_t>((px[0] * a + matte.r * inv + 127u) / 255u);
    out[1] = static_cast<std::uint8_t>((px[1] * a + matte.g * inv + 127u) / 255u);
    out[2] = static_cast<std::uint8_t>((px[2] * a + matte.b * inv + 127u) / 255u);
}

// A box-average downscale, used by ONE caller: the ICO writer's size set (an icon is a set of
// mip levels, and the export pipeline hands us exactly one image -- see docs/formats-curated.md).
// Averaging runs in PREMULTIPLIED alpha and un-premultiplies on the way out, the compositor's own
// rule, so a soft edge cannot fringe toward whatever colour sat under the transparent pixels.
// Never UPscales: a request larger than the source on either axis returns an empty Bitmap, since
// inventing pixels is the caller's decision to make, not a codec's.
[[nodiscard]] Bitmap downscaleBox(const ImageView& src, std::uint32_t outW, std::uint32_t outH);

// Fit `src` into a `side` x `side` transparent square, preserving its aspect ratio (box-averaged
// down, centred, never upscaled past the source's own size). What an ICO entry needs from a
// non-square document: squashing the aspect ratio would be a silent lie about the artwork.
[[nodiscard]] Bitmap fitSquare(const ImageView& src, std::uint32_t side);

// ---------------------------------------------------------------------------------------------
// Byte plumbing
// ---------------------------------------------------------------------------------------------

// A growing little/big-endian byte buffer. Every encoder here writes through one, so the
// endianness of a header field is stated once, at the call site, in the order the file has it.
class ByteWriter {
public:
    void u8(std::uint8_t v) { m_bytes.push_back(v); }
    void u16le(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v & 0xFFu));
        u8(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    }
    void u32le(std::uint32_t v) {
        u16le(static_cast<std::uint16_t>(v & 0xFFFFu));
        u16le(static_cast<std::uint16_t>((v >> 16) & 0xFFFFu));
    }
    void i32le(std::int32_t v) { u32le(static_cast<std::uint32_t>(v)); }
    void u16be(std::uint16_t v) {
        u8(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
        u8(static_cast<std::uint8_t>(v & 0xFFu));
    }
    void u32be(std::uint32_t v) {
        u16be(static_cast<std::uint16_t>((v >> 16) & 0xFFFFu));
        u16be(static_cast<std::uint16_t>(v & 0xFFFFu));
    }
    void raw(const std::uint8_t* p, std::size_t n) { m_bytes.insert(m_bytes.end(), p, p + n); }
    void text(std::string_view s) { m_bytes.insert(m_bytes.end(), s.begin(), s.end()); }
    void zeros(std::size_t n) { m_bytes.insert(m_bytes.end(), n, std::uint8_t{0}); }

    [[nodiscard]] std::size_t size() const noexcept { return m_bytes.size(); }
    // Back-patch a 32-bit little-endian field written earlier (a file size, a payload offset).
    void patch32le(std::size_t off, std::uint32_t v) noexcept {
        if (off + 4 > m_bytes.size())
            return;
        m_bytes[off + 0] = static_cast<std::uint8_t>(v & 0xFFu);
        m_bytes[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
        m_bytes[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
        m_bytes[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(m_bytes); }

private:
    std::vector<std::uint8_t> m_bytes;
};

// A bounds-checked cursor over an encoded file. The read accessors are UNCHECKED on purpose and
// every one of them REQUIRES a preceding has() that covers it -- io/exif.cpp's discipline:
// validate a whole record's extent once, then read its fields without re-checking each.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) noexcept
        : m_data(data), m_size(data == nullptr ? 0u : size) {}

    [[nodiscard]] std::size_t pos() const noexcept { return m_pos; }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] std::size_t left() const noexcept { return m_size - m_pos; }  // m_pos <= m_size
    [[nodiscard]] bool has(std::uint64_t n) const noexcept { return n <= left(); }
    [[nodiscard]] bool done() const noexcept { return m_pos >= m_size; }
    [[nodiscard]] const std::uint8_t* cursor() const noexcept {
        return m_data == nullptr ? nullptr : m_data + m_pos;
    }
    [[nodiscard]] bool seek(std::uint64_t off) noexcept {
        if (off > m_size)
            return false;
        m_pos = static_cast<std::size_t>(off);
        return true;
    }
    void skip(std::size_t n) noexcept { m_pos += (n <= left() ? n : left()); }

    [[nodiscard]] std::uint8_t u8() noexcept { return m_data[m_pos++]; }
    [[nodiscard]] std::uint16_t u16le() noexcept {
        const std::uint16_t v =
            static_cast<std::uint16_t>(m_data[m_pos] | (m_data[m_pos + 1] << 8));
        m_pos += 2;
        return v;
    }
    [[nodiscard]] std::uint32_t u32le() noexcept {
        const std::uint32_t a = m_data[m_pos], b = m_data[m_pos + 1], c = m_data[m_pos + 2],
                            d = m_data[m_pos + 3];
        m_pos += 4;
        return a | (b << 8) | (c << 16) | (d << 24);
    }
    [[nodiscard]] std::int32_t i32le() noexcept { return static_cast<std::int32_t>(u32le()); }
    [[nodiscard]] std::uint16_t u16be() noexcept {
        const std::uint16_t v =
            static_cast<std::uint16_t>((m_data[m_pos] << 8) | m_data[m_pos + 1]);
        m_pos += 2;
        return v;
    }
    [[nodiscard]] std::uint32_t u32be() noexcept {
        const std::uint32_t a = m_data[m_pos], b = m_data[m_pos + 1], c = m_data[m_pos + 2],
                            d = m_data[m_pos + 3];
        m_pos += 4;
        return (a << 24) | (b << 16) | (c << 8) | d;
    }

private:
    const std::uint8_t* m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_pos = 0;
};

// The one place an error string is set, so a decoder's failure paths read as one line each and
// a null `error` costs nothing.
inline bool fail(std::string* error, std::string_view why) {
    if (error != nullptr)
        *error = std::string(why);
    return false;
}

// ---------------------------------------------------------------------------------------------
// Identification
// ---------------------------------------------------------------------------------------------

enum class Codec : std::uint8_t { None, Bmp, Ico, Pnm, Qoi, RadianceHdr, Tga };

// Which of ours -- if any -- these bytes are, from the leading signature. Order matters: every
// format with a real magic number is tested first, and TGA (which HAS NO MAGIC -- its 18-byte
// header is all small integers) is the deliberate last resort, accepted only when the whole
// header is self-consistent and its declared payload fits the buffer. So a TGA guess can never
// shadow a format that identified itself.
[[nodiscard]] Codec sniff(const std::uint8_t* data, std::size_t size) noexcept;

// "BMP", "ICO", "PNM", "QOI", "Radiance HDR", "TGA", "unknown" -- for error text.
[[nodiscard]] std::string_view codecName(Codec codec) noexcept;

} // namespace mosaicfmt
