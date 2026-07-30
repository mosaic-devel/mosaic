#include "formats/bmp.hpp"

#include <algorithm>
#include <cstring>

namespace mosaicfmt {
namespace {

constexpr std::uint32_t kBiRgb = 0;
constexpr std::uint32_t kBiRle8 = 1;
constexpr std::uint32_t kBiRle4 = 2;
constexpr std::uint32_t kBiBitfields = 3;

constexpr std::uint32_t kLcsSrgb = 0x73524742u;          // 'sRGB'
constexpr std::uint32_t kProfileEmbedded = 0x4D424544u;  // 'MBED'
constexpr std::uint32_t kLcsGmImages = 4;                // rendering intent: perceptual/images

// A colour channel described by a bit mask, which is how BMP spells 16- and 32-bit pixels.
struct MaskChannel {
    std::uint32_t mask = 0;
    int shift = 0;
    int bits = 0;

    [[nodiscard]] static MaskChannel of(std::uint32_t mask) noexcept {
        MaskChannel c;
        c.mask = mask;
        if (mask == 0)
            return c;
        while (((mask >> c.shift) & 1u) == 0u && c.shift < 31)
            ++c.shift;
        for (std::uint32_t m = mask >> c.shift; m != 0; m >>= 1)
            c.bits += static_cast<int>(m & 1u);
        return c;
    }
    // Scale the field to 0..255 exactly: a 5-bit 0x1F must become 0xFF, not 0xF8.
    [[nodiscard]] std::uint8_t sample(std::uint32_t v) const noexcept {
        if (mask == 0 || bits <= 0)
            return 0;
        const std::uint32_t raw = (v & mask) >> shift;
        const std::uint32_t maxValue = bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        return static_cast<std::uint8_t>(
            (static_cast<std::uint64_t>(raw) * 255u + maxValue / 2u) / maxValue);
    }
};

[[nodiscard]] std::size_t rowStride(std::uint32_t bitCount, std::uint32_t width) noexcept {
    // Rows are padded to a 4-byte boundary -- the one BMP detail every hand-rolled reader gets
    // wrong first.
    return ((static_cast<std::size_t>(bitCount) * width + 31u) / 32u) * 4u;
}

[[nodiscard]] std::int32_t pelsPerMetre(double dpi) noexcept {
    // 72 dpi is the "no opinion" value the rest of io skips, so it writes a zero here too.
    if (!(dpi > 0.0) || dpi == 72.0)
        return 0;
    return static_cast<std::int32_t>(dpi / 0.0254 + 0.5);
}

struct DibDesc {
    int version = 5;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool topDown = false;
    std::uint16_t bitCount = 32;
    std::uint32_t compression = kBiRgb;
    std::uint32_t imageBytes = 0;
    std::uint32_t rMask = 0, gMask = 0, bMask = 0, aMask = 0;
    std::uint32_t paletteEntries = 0;
    double dpi = 0.0;
    std::uint32_t iccOffsetFromHeader = 0;  // V5 only
    std::uint32_t iccSize = 0;
};

[[nodiscard]] std::uint32_t dibHeaderSize(int version) noexcept {
    return version <= 3 ? 40u : (version == 4 ? 108u : 124u);
}

void writeDibHeader(ByteWriter& w, const DibDesc& d) {
    w.u32le(dibHeaderSize(d.version));
    w.i32le(static_cast<std::int32_t>(d.width));
    w.i32le(d.topDown ? -static_cast<std::int32_t>(d.height)
                      : static_cast<std::int32_t>(d.height));
    w.u16le(1);  // biPlanes is always 1; the field is a fossil
    w.u16le(d.bitCount);
    w.u32le(d.compression);
    w.u32le(d.imageBytes);
    w.i32le(pelsPerMetre(d.dpi));
    w.i32le(pelsPerMetre(d.dpi));
    w.u32le(d.paletteEntries);
    w.u32le(0);  // biClrImportant: "all of them"
    if (d.version >= 4) {
        w.u32le(d.rMask);
        w.u32le(d.gMask);
        w.u32le(d.bMask);
        w.u32le(d.aMask);
        // Only V5 has anywhere to put a profile; a V4 file says sRGB and means it.
        w.u32le(d.iccSize != 0 && d.version >= 5 ? kProfileEmbedded : kLcsSrgb);
        w.zeros(36);  // CIEXYZTRIPLE endpoints: meaningful only for CSType == LCS_CALIBRATED_RGB
        w.u32le(0);   // gamma red
        w.u32le(0);   // gamma green
        w.u32le(0);   // gamma blue
    }
    if (d.version >= 5) {
        w.u32le(kLcsGmImages);
        w.u32le(d.iccSize != 0 ? d.iccOffsetFromHeader : 0u);
        w.u32le(d.iccSize);
        w.u32le(0);  // reserved
    }
}

// Assemble a complete .bmp around an already-strided pixel payload. Shared by the truecolour and
// the indexed encoders so the file header, the offsets and the ICC back-reference are computed in
// exactly one place -- three fields that all have to agree or the file is subtly broken.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> assembleBmp(
    DibDesc desc, const std::vector<Rgba8>& palette, const std::vector<std::uint8_t>& pixels,
    const std::vector<std::uint8_t>& icc, std::string* error) {
    const std::uint32_t headerSize = dibHeaderSize(desc.version);
    // A 40-byte header keeps its colour masks in the 12 bytes AFTER the header; a V4/V5 header has
    // fields of its own for them. Same information, two places, and offBits has to know which.
    const std::uint32_t trailingMasks =
        (desc.version <= 3 && desc.compression == kBiBitfields) ? 12u : 0u;
    const std::uint32_t paletteBytes = static_cast<std::uint32_t>(palette.size()) * 4u;
    const std::uint64_t offBits = 14ull + headerSize + trailingMasks + paletteBytes;
    const std::uint64_t total = offBits + pixels.size() + icc.size();
    if (total > 0xFFFFFFFFull) {
        fail(error, "BMP: the file would exceed the format's 4 GB size field");
        return std::nullopt;
    }
    desc.imageBytes = static_cast<std::uint32_t>(pixels.size());
    desc.paletteEntries = static_cast<std::uint32_t>(palette.size());
    desc.iccSize = static_cast<std::uint32_t>(icc.size());
    // ProfileData counts from the START OF THE DIB HEADER, not the start of the file.
    desc.iccOffsetFromHeader =
        static_cast<std::uint32_t>(offBits + pixels.size() - 14ull);

    ByteWriter w;
    w.text("BM");
    w.u32le(static_cast<std::uint32_t>(total));
    w.u16le(0);
    w.u16le(0);
    w.u32le(static_cast<std::uint32_t>(offBits));
    writeDibHeader(w, desc);
    if (trailingMasks != 0) {
        w.u32le(desc.rMask);
        w.u32le(desc.gMask);
        w.u32le(desc.bMask);
    }
    for (const Rgba8& c : palette) {
        w.u8(c.b);
        w.u8(c.g);
        w.u8(c.r);
        w.u8(0);  // RGBQUAD's reserved byte -- NOT an alpha channel, whatever it looks like
    }
    w.raw(pixels.data(), pixels.size());
    if (!icc.empty())
        w.raw(icc.data(), icc.size());
    return w.take();
}

// BI_RLE8 over one bottom-up run of rows. Encoded runs for 3 or more equal indices, absolute
// blocks for anything else, [0,0] at each row's end and [0,1] at the end of the bitmap. No deltas:
// they only pay off for sprite data with runs of untouched pixels, which an export never has.
[[nodiscard]] std::vector<std::uint8_t> encodeRle8(const IndexedView& image) {
    ByteWriter w;
    for (std::uint32_t i = 0; i < image.height; ++i) {
        const std::uint32_t y = image.height - 1 - i;  // an RLE bitmap is always bottom-up
        const std::uint8_t* row = image.indices + static_cast<std::size_t>(y) * image.width;
        std::uint32_t x = 0;
        while (x < image.width) {
            std::uint32_t run = 1;
            while (x + run < image.width && run < 255 && row[x + run] == row[x])
                ++run;
            if (run >= 3) {
                w.u8(static_cast<std::uint8_t>(run));
                w.u8(row[x]);
                x += run;
                continue;
            }
            // Gather literals up to the next run of three (which pays for its own two bytes).
            std::uint32_t n = 0;
            while (x + n < image.width && n < 255) {
                if (x + n + 2 < image.width && row[x + n] == row[x + n + 1] &&
                    row[x + n] == row[x + n + 2])
                    break;
                ++n;
            }
            if (n == 0)
                n = 1;
            if (n < 3) {
                for (std::uint32_t k = 0; k < n; ++k) {
                    w.u8(1);
                    w.u8(row[x + k]);
                }
            } else {
                w.u8(0);
                w.u8(static_cast<std::uint8_t>(n));
                for (std::uint32_t k = 0; k < n; ++k)
                    w.u8(row[x + k]);
                if ((n & 1u) != 0u)
                    w.u8(0);  // absolute blocks are padded to a 16-bit boundary
            }
            x += n;
        }
        w.u8(0);
        w.u8(0);  // end of line
    }
    w.u8(0);
    w.u8(1);  // end of bitmap
    return w.take();
}

// ---------------------------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------------------------

struct DibInfo {
    std::uint32_t headerSize = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;  // absolute, and already halved for an ICO entry
    bool topDown = false;
    std::uint16_t bitCount = 0;
    std::uint32_t compression = kBiRgb;
    std::uint32_t paletteEntries = 0;
    std::uint32_t paletteEntrySize = 4;
    MaskChannel r, g, b, a;
    bool explicitMasks = false;
    std::size_t paletteOffset = 0;  // absolute offsets into the buffer
    std::size_t pixelOffset = 0;
};

// Read a DIB header at `base`. Every rejection here is a STRUCTURAL problem -- a size that cannot
// be a header, a dimension that cannot be allocated, a compression whose payload we would have to
// invent. The decorative fields (biSizeImage, biClrImportant, the density) are read past and never
// trusted: a declared length is not evidence of anything.
[[nodiscard]] std::optional<DibInfo> parseDib(const std::uint8_t* data, std::size_t size,
                                              std::size_t base, bool icoEntry,
                                              std::string* error) {
    ByteReader r(data, size);
    if (!r.seek(base) || !r.has(12)) {
        fail(error, "BMP: the header is truncated");
        return std::nullopt;
    }
    DibInfo d;
    d.headerSize = r.u32le();
    // A DIB header size is an ENUMERATED set, not a range: 12 (CORE), 40 (INFO), 52/56 (V2/V3, the
    // mask extensions) and 108/124 (V4/V5). A range check passes sizes no encoder can emit -- 99
    // bytes, say -- and every such size then lands in the >=40 branch below and shifts where the
    // palette and the pixels are believed to start, so the file decodes to *something* instead of
    // being refused. Two sizes are deliberately NOT here: 16 and 64, the OS/2 v2 headers. 64 is the
    // one that matters -- it is >= 52, so the old range check let it read its OS/2 fields (units,
    // recording, rendering, colour encoding) as though they were colour masks. Refusing both is the
    // documented CORE-to-V5 subset (docs/formats-curated.md §3), not a regression.
    switch (d.headerSize) {
    case 12:
    case 40:
    case 52:
    case 56:
    case 108:
    case 124: break;
    default:
        fail(error, "BMP: unsupported or truncated DIB header");
        return std::nullopt;
    }
    if (!r.has(d.headerSize - 4u)) {
        fail(error, "BMP: unsupported or truncated DIB header");
        return std::nullopt;
    }
    if (d.headerSize == 12) {
        // BITMAPCOREHEADER: unsigned dimensions, always bottom-up, 3-byte palette entries.
        d.width = r.u16le();
        d.height = r.u16le();
        (void)r.u16le();  // planes
        d.bitCount = r.u16le();
        d.paletteEntrySize = 3;
    } else {
        // Necessarily one of 40 / 52 / 56 / 108 / 124 -- the switch above is the only way in.
        const std::int32_t w = r.i32le();
        const std::int32_t h = r.i32le();
        (void)r.u16le();  // planes
        d.bitCount = r.u16le();
        d.compression = r.u32le();
        (void)r.u32le();  // biSizeImage -- never trusted
        (void)r.i32le();  // density, x
        (void)r.i32le();  // density, y
        const std::uint32_t clrUsed = r.u32le();
        (void)r.u32le();  // biClrImportant
        if (w <= 0 || h == 0 || h == INT32_MIN) {
            fail(error, "BMP: implausible dimensions");
            return std::nullopt;
        }
        d.width = static_cast<std::uint32_t>(w);
        d.topDown = h < 0;
        d.height = static_cast<std::uint32_t>(h < 0 ? -static_cast<std::int64_t>(h) : h);
        d.paletteEntries = clrUsed;
        if (d.headerSize >= 52) {
            d.r = MaskChannel::of(r.u32le());
            d.g = MaskChannel::of(r.u32le());
            d.b = MaskChannel::of(r.u32le());
            if (d.headerSize >= 56)
                d.a = MaskChannel::of(r.u32le());
            d.explicitMasks = true;
        }
    }

    std::size_t masksAfterHeader = 0;
    if (d.headerSize == 40 && d.compression == kBiBitfields) {
        // The V3 spelling: three masks in the bytes that would otherwise hold a palette.
        if (!r.seek(base + 40u) || !r.has(12)) {
            fail(error, "BMP: the colour masks are missing");
            return std::nullopt;
        }
        d.r = MaskChannel::of(r.u32le());
        d.g = MaskChannel::of(r.u32le());
        d.b = MaskChannel::of(r.u32le());
        d.explicitMasks = true;
        masksAfterHeader = 12;
    }

    if (icoEntry) {
        // The ICO convention, and the classic bug: biHeight counts the XOR image AND the AND mask.
        if (d.topDown) {
            fail(error, "ICO: an icon's bitmap must be bottom-up");
            return std::nullopt;
        }
        if ((d.height & 1u) != 0u) {
            fail(error, "ICO: the entry's height is not twice a whole number of rows");
            return std::nullopt;
        }
        d.height /= 2;
    }

    if (d.bitCount != 1 && d.bitCount != 4 && d.bitCount != 8 && d.bitCount != 16 &&
        d.bitCount != 24 && d.bitCount != 32) {
        fail(error, "BMP: unsupported bit depth");
        return std::nullopt;
    }
    if (d.compression != kBiRgb && d.compression != kBiRle8 && d.compression != kBiRle4 &&
        d.compression != kBiBitfields) {
        // BI_JPEG and BI_PNG wrap a whole other codec; nothing here decodes one.
        fail(error, "BMP: unsupported compression");
        return std::nullopt;
    }
    if ((d.compression == kBiRle8 && d.bitCount != 8) ||
        (d.compression == kBiRle4 && d.bitCount != 4)) {
        fail(error, "BMP: the RLE compression does not match the bit depth");
        return std::nullopt;
    }
    if (d.compression == kBiBitfields && d.bitCount != 16 && d.bitCount != 32) {
        fail(error, "BMP: bit fields are only defined for 16- and 32-bit pixels");
        return std::nullopt;
    }
    if ((d.compression == kBiRle8 || d.compression == kBiRle4) && d.topDown) {
        fail(error, "BMP: a top-down RLE bitmap is not legal");
        return std::nullopt;
    }
    if (!dimensionsPlausible(d.width, d.height)) {
        fail(error, "BMP: implausible image dimensions");
        return std::nullopt;
    }

    const bool indexed = d.bitCount <= 8;
    if (indexed) {
        const std::uint32_t full = 1u << d.bitCount;
        if (d.paletteEntries == 0 || d.paletteEntries > full)
            d.paletteEntries = full;  // 0 means "the whole table"; anything larger is nonsense
    } else {
        // A truecolour DIB may declare an "optimal colours" table. It is advisory, we never use
        // it, and trusting a garbage count here would only move the pixel data.
        d.paletteEntries = 0;
    }
    if (d.explicitMasks &&
        (d.compression != kBiBitfields || (d.r.mask | d.g.mask | d.b.mask) == 0u)) {
        // Colour masks are only meaningful under BI_BITFIELDS. A V4/V5 header carries the fields
        // unconditionally and plenty of writers leave them zero -- or stale -- under BI_RGB, and
        // reading those would sample every channel as 0 and hand back a uniformly black image,
        // which is the worst kind of wrong: it looks like a decode, not like a failure.
        d.explicitMasks = false;
        d.r = MaskChannel{};
        d.g = MaskChannel{};
        d.b = MaskChannel{};
        d.a = MaskChannel{};
    }
    if (!d.explicitMasks) {
        // The default fields for the two mask-less truecolour depths. 16-bit BI_RGB is 5-5-5, NOT
        // 5-6-5 -- the one that is always guessed wrong.
        if (d.bitCount == 16) {
            d.r = MaskChannel::of(0x7C00u);
            d.g = MaskChannel::of(0x03E0u);
            d.b = MaskChannel::of(0x001Fu);
        } else if (d.bitCount == 32 || d.bitCount == 24) {
            d.r = MaskChannel::of(0x00FF0000u);
            d.g = MaskChannel::of(0x0000FF00u);
            d.b = MaskChannel::of(0x000000FFu);
        }
    }

    d.paletteOffset = base + d.headerSize + masksAfterHeader;
    d.pixelOffset =
        d.paletteOffset + static_cast<std::size_t>(d.paletteEntries) * d.paletteEntrySize;
    if (d.pixelOffset > size) {
        fail(error, "BMP: the palette does not fit the file");
        return std::nullopt;
    }
    return d;
}

[[nodiscard]] std::vector<Rgba8> readPalette(const std::uint8_t* data, const DibInfo& d) {
    std::vector<Rgba8> palette;
    palette.reserve(d.paletteEntries);
    for (std::uint32_t i = 0; i < d.paletteEntries; ++i) {
        const std::uint8_t* p = data + d.paletteOffset + static_cast<std::size_t>(i) * d.paletteEntrySize;
        palette.push_back(Rgba8{p[2], p[1], p[0], 255});
    }
    return palette;
}

[[nodiscard]] bool decodeUncompressed(const std::uint8_t* data, std::size_t size,
                                      const DibInfo& d, const std::vector<Rgba8>& palette,
                                      Bitmap& out, std::string* error) {
    const std::size_t stride = rowStride(d.bitCount, d.width);
    if (stride == 0 || d.pixelOffset > size ||
        (size - d.pixelOffset) / stride < static_cast<std::size_t>(d.height))
        return fail(error, "BMP: the pixel data is truncated");

    // A 32-bit BI_RGB bitmap's fourth byte is formally undefined. The pragmatic reading -- the one
    // every decoder converged on -- is: if it is zero everywhere, it is padding and the image is
    // opaque; if anything is non-zero, it is alpha and was meant.
    bool fourthByteIsAlpha = d.a.mask != 0;
    if (d.bitCount == 32 && d.a.mask == 0) {
        for (std::uint32_t i = 0; i < d.height && !fourthByteIsAlpha; ++i) {
            const std::uint8_t* row = data + d.pixelOffset + static_cast<std::size_t>(i) * stride;
            for (std::uint32_t x = 0; x < d.width; ++x)
                if (row[x * 4u + 3u] != 0u) {
                    fourthByteIsAlpha = true;
                    break;
                }
        }
    }

    for (std::uint32_t i = 0; i < d.height; ++i) {
        const std::uint8_t* row = data + d.pixelOffset + static_cast<std::size_t>(i) * stride;
        const std::uint32_t dstY = d.topDown ? i : d.height - 1 - i;
        for (std::uint32_t x = 0; x < d.width; ++x) {
            std::uint8_t* p = out.at(x, dstY);
            switch (d.bitCount) {
            case 1:
            case 4:
            case 8: {
                std::uint32_t index = 0;
                if (d.bitCount == 8)
                    index = row[x];
                else if (d.bitCount == 4)
                    index = (x & 1u) != 0u ? (row[x >> 1] & 0x0Fu) : (row[x >> 1] >> 4);
                else
                    index = (row[x >> 3] >> (7u - (x & 7u))) & 1u;
                if (index >= palette.size())
                    return fail(error, "BMP: a pixel indexes past the end of the palette");
                p[0] = palette[index].r;
                p[1] = palette[index].g;
                p[2] = palette[index].b;
                p[3] = 255;
                break;
            }
            case 16: {
                const std::uint32_t v =
                    static_cast<std::uint32_t>(row[x * 2u]) |
                    (static_cast<std::uint32_t>(row[x * 2u + 1u]) << 8);
                p[0] = d.r.sample(v);
                p[1] = d.g.sample(v);
                p[2] = d.b.sample(v);
                p[3] = d.a.mask != 0 ? d.a.sample(v) : std::uint8_t{255};
                break;
            }
            case 24:
                p[0] = row[x * 3u + 2u];
                p[1] = row[x * 3u + 1u];
                p[2] = row[x * 3u + 0u];
                p[3] = 255;
                break;
            default: {  // 32
                const std::uint8_t* q = row + x * 4u;
                const std::uint32_t v = static_cast<std::uint32_t>(q[0]) |
                                        (static_cast<std::uint32_t>(q[1]) << 8) |
                                        (static_cast<std::uint32_t>(q[2]) << 16) |
                                        (static_cast<std::uint32_t>(q[3]) << 24);
                p[0] = d.r.sample(v);
                p[1] = d.g.sample(v);
                p[2] = d.b.sample(v);
                p[3] = fourthByteIsAlpha
                           ? (d.a.mask != 0 ? d.a.sample(v) : q[3])
                           : std::uint8_t{255};
                break;
            }
            }
        }
    }
    return true;
}

// BI_RLE8 / BI_RLE4. Pixels the stream never writes (a delta jump, a short row) stay FULLY
// TRANSPARENT rather than becoming black: the format says they are undefined, and "nothing was
// stored here" is the only honest thing an RGBA buffer can say about them.
[[nodiscard]] bool decodeRle(const std::uint8_t* data, std::size_t size, const DibInfo& d,
                             const std::vector<Rgba8>& palette, Bitmap& out, std::string* error) {
    ByteReader r(data, size);
    if (!r.seek(d.pixelOffset))
        return fail(error, "BMP: the pixel data is truncated");
    const bool four = d.compression == kBiRle4;
    std::uint32_t x = 0, y = 0;
    const auto put = [&](std::uint32_t index) {
        if (x >= d.width || y >= d.height)
            return false;
        if (index >= palette.size())
            return false;
        std::uint8_t* p = out.at(x, d.height - 1 - y);
        p[0] = palette[index].r;
        p[1] = palette[index].g;
        p[2] = palette[index].b;
        p[3] = 255;
        ++x;
        return true;
    };

    for (;;) {
        if (!r.has(2))
            return fail(error, "BMP: the RLE stream ends without an end-of-bitmap marker");
        const std::uint8_t count = r.u8();
        const std::uint8_t value = r.u8();
        if (count > 0) {
            for (std::uint32_t k = 0; k < count; ++k) {
                const std::uint32_t index =
                    four ? ((k & 1u) != 0u ? (value & 0x0Fu) : std::uint32_t{value} >> 4)
                         : std::uint32_t{value};
                if (!put(index))
                    return fail(error, "BMP: an RLE run runs past the end of its row");
            }
            continue;
        }
        if (value == 0) {  // end of line
            x = 0;
            ++y;
            if (y > d.height)
                return fail(error, "BMP: the RLE stream declares more rows than the header");
            continue;
        }
        if (value == 1)  // end of bitmap
            break;
        if (value == 2) {  // delta
            if (!r.has(2))
                return fail(error, "BMP: a truncated RLE delta");
            const std::uint32_t dx = r.u8();
            const std::uint32_t dy = r.u8();
            x += dx;
            y += dy;
            if (x > d.width || y > d.height)
                return fail(error, "BMP: an RLE delta jumps outside the bitmap");
            continue;
        }
        // Absolute mode: `value` literal pixels, padded to a 16-bit boundary.
        const std::uint32_t n = value;
        const std::size_t bytes = four ? (n + 1u) / 2u : n;
        const std::size_t padded = bytes + (bytes & 1u);
        if (!r.has(padded))
            return fail(error, "BMP: a truncated RLE absolute block");
        std::uint8_t packed = 0;
        for (std::uint32_t k = 0; k < n; ++k) {
            std::uint32_t index = 0;
            if (four) {
                if ((k & 1u) == 0u)
                    packed = r.u8();
                index = (k & 1u) != 0u ? (packed & 0x0Fu) : std::uint32_t{packed} >> 4;
            } else {
                index = r.u8();
            }
            if (!put(index))
                return fail(error, "BMP: an RLE block runs past the end of its row");
        }
        if ((bytes & 1u) != 0u)
            (void)r.u8();  // the pad byte
    }
    return true;
}

// The 1-bit AND mask that follows an ICO entry's XOR image: a set bit means transparent. A mask
// that is missing or does not fit is DROPPED (the picture itself is intact and complete), which is
// the "absurd value loses its own field" half of the house rule.
void applyIcoMask(const std::uint8_t* data, std::size_t size, std::size_t offset,
                  const DibInfo& d, Bitmap& out) {
    const std::size_t stride = ((static_cast<std::size_t>(d.width) + 31u) / 32u) * 4u;
    if (stride == 0 || offset > size || (size - offset) / stride < static_cast<std::size_t>(d.height))
        return;
    for (std::uint32_t i = 0; i < d.height; ++i) {
        const std::uint8_t* row = data + offset + static_cast<std::size_t>(i) * stride;
        const std::uint32_t dstY = d.height - 1 - i;
        for (std::uint32_t x = 0; x < d.width; ++x)
            if (((row[x >> 3] >> (7u - (x & 7u))) & 1u) != 0u)
                out.at(x, dstY)[3] = 0;
    }
}

[[nodiscard]] std::optional<Bitmap> decodeDibAt(const std::uint8_t* data, std::size_t size,
                                                std::size_t base, bool icoEntry,
                                                std::size_t explicitPixelOffset,
                                                std::string* error) {
    std::optional<DibInfo> info = parseDib(data, size, base, icoEntry, error);
    if (!info)
        return std::nullopt;
    if (explicitPixelOffset != 0) {
        if (explicitPixelOffset > size || explicitPixelOffset < info->paletteOffset) {
            fail(error, "BMP: the pixel-data offset points outside the file");
            return std::nullopt;
        }
        // The file header's offset wins over our computed one, and the palette is however many
        // entries fit in between -- a smaller palette than biClrUsed claims is common enough in
        // the wild that trusting the offset recovers those files instead of refusing them.
        const std::size_t fits =
            (explicitPixelOffset - info->paletteOffset) / info->paletteEntrySize;
        if (fits < info->paletteEntries)
            info->paletteEntries = static_cast<std::uint32_t>(fits);
        info->pixelOffset = explicitPixelOffset;
    }

    const std::vector<Rgba8> palette = readPalette(data, *info);
    Bitmap out(info->width, info->height);
    const bool rle = info->compression == kBiRle8 || info->compression == kBiRle4;
    if (rle) {
        if (!decodeRle(data, size, *info, palette, out, error))
            return std::nullopt;
    } else if (!decodeUncompressed(data, size, *info, palette, out, error)) {
        return std::nullopt;
    }

    if (icoEntry && !rle) {
        // Windows ignores the mask when the XOR image carries real alpha, and so do we: some
        // writers leave the legacy mask filled with ones beside a perfectly good alpha channel,
        // and honouring it there would erase the icon.
        bool hasAlpha = false;
        if (info->bitCount == 32)
            for (std::size_t i = 3; i < out.rgba.size() && !hasAlpha; i += 4)
                hasAlpha = out.rgba[i] != 255;
        if (!hasAlpha) {
            const std::size_t xorBytes =
                rowStride(info->bitCount, info->width) * info->height;
            applyIcoMask(data, size, info->pixelOffset + xorBytes, *info, out);
        }
    }
    return out;
}

} // namespace

bool bmpWritesAlpha(const BmpOptions& opts) noexcept {
    return opts.depth == BmpOptions::Depth::Bgra32 && opts.headerVersion >= 4;
}

std::optional<std::vector<std::uint8_t>> encodeBmp(const ImageView& image, const BmpOptions& opts,
                                                   std::string* error) {
    if (!image.valid()) {
        fail(error, "BMP: nothing to encode");
        return std::nullopt;
    }
    if (opts.depth == BmpOptions::Depth::Indexed8) {
        fail(error, "BMP: an indexed encode needs a palette (use encodeBmpIndexed)");
        return std::nullopt;
    }
    DibDesc desc;
    desc.version = std::clamp(opts.headerVersion, 3, 5);
    desc.width = image.width;
    desc.height = image.height;
    desc.topDown = opts.topDown;
    desc.dpi = opts.dpi;

    const bool alpha = bmpWritesAlpha(opts);
    switch (opts.depth) {
    case BmpOptions::Depth::Bgra32:
        desc.bitCount = 32;
        if (alpha) {
            desc.compression = kBiBitfields;
            desc.rMask = 0x00FF0000u;
            desc.gMask = 0x0000FF00u;
            desc.bMask = 0x000000FFu;
            desc.aMask = 0xFF000000u;
        }
        break;
    case BmpOptions::Depth::Bgr24:
        desc.bitCount = 24;
        break;
    case BmpOptions::Depth::Rgb565:
        desc.bitCount = 16;
        desc.compression = kBiBitfields;
        desc.rMask = 0xF800u;
        desc.gMask = 0x07E0u;
        desc.bMask = 0x001Fu;
        break;
    case BmpOptions::Depth::Indexed8:
        break;  // rejected above
    }

    const std::size_t stride = rowStride(desc.bitCount, image.width);
    std::vector<std::uint8_t> pixels(stride * image.height, 0);
    for (std::uint32_t i = 0; i < image.height; ++i) {
        const std::uint32_t y = opts.topDown ? i : image.height - 1 - i;
        std::uint8_t* dst = pixels.data() + static_cast<std::size_t>(i) * stride;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::uint8_t* px = image.at(x, y);
            std::uint8_t rgb[3] = {px[0], px[1], px[2]};
            if (!alpha)
                compositeOverMatte(px, opts.matte, rgb);
            if (desc.bitCount == 32) {
                dst[x * 4u + 0u] = rgb[2];
                dst[x * 4u + 1u] = rgb[1];
                dst[x * 4u + 2u] = rgb[0];
                dst[x * 4u + 3u] = alpha ? px[3] : std::uint8_t{255};
            } else if (desc.bitCount == 24) {
                dst[x * 3u + 0u] = rgb[2];
                dst[x * 3u + 1u] = rgb[1];
                dst[x * 3u + 2u] = rgb[0];
            } else {
                const std::uint16_t v = static_cast<std::uint16_t>(
                    ((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
                dst[x * 2u + 0u] = static_cast<std::uint8_t>(v & 0xFFu);
                dst[x * 2u + 1u] = static_cast<std::uint8_t>(v >> 8);
            }
        }
    }
    const std::vector<std::uint8_t> icc = desc.version >= 5 ? opts.icc : std::vector<std::uint8_t>{};
    return assembleBmp(desc, {}, pixels, icc, error);
}

std::optional<std::vector<std::uint8_t>> encodeBmpIndexed(const IndexedView& image,
                                                          const BmpOptions& opts,
                                                          std::string* error) {
    if (!image.valid()) {
        fail(error, "BMP: nothing to encode");
        return std::nullopt;
    }
    DibDesc desc;
    desc.version = std::clamp(opts.headerVersion, 3, 5);
    desc.width = image.width;
    desc.height = image.height;
    desc.bitCount = 8;
    desc.dpi = opts.dpi;
    // A top-down RLE bitmap is illegal, so RLE wins and the rows go bottom-up.
    desc.topDown = opts.rle ? false : opts.topDown;
    desc.compression = opts.rle ? kBiRle8 : kBiRgb;

    std::vector<Rgba8> palette(image.palette, image.palette + image.paletteSize);
    // Every index has to have an entry: the picture is described by the palette, so a short one is
    // a broken file rather than a missing decoration.
    const std::size_t total = static_cast<std::size_t>(image.width) * image.height;
    for (std::size_t i = 0; i < total; ++i)
        if (image.indices[i] >= palette.size()) {
            fail(error, "BMP: a pixel indexes past the end of the palette");
            return std::nullopt;
        }

    std::vector<std::uint8_t> pixels;
    if (opts.rle) {
        pixels = encodeRle8(image);
    } else {
        const std::size_t stride = rowStride(8, image.width);
        pixels.assign(stride * image.height, 0);
        for (std::uint32_t i = 0; i < image.height; ++i) {
            const std::uint32_t y = desc.topDown ? i : image.height - 1 - i;
            std::memcpy(pixels.data() + static_cast<std::size_t>(i) * stride,
                        image.indices + static_cast<std::size_t>(y) * image.width, image.width);
        }
    }
    const std::vector<std::uint8_t> icc = desc.version >= 5 ? opts.icc : std::vector<std::uint8_t>{};
    return assembleBmp(desc, palette, pixels, icc, error);
}

std::optional<Bitmap> decodeBmp(const std::uint8_t* data, std::size_t size, std::string* error) {
    if (data == nullptr || size < 14 || size > kMaxFileBytes) {
        fail(error, "BMP: not a BMP file (too short)");
        return std::nullopt;
    }
    if (data[0] != 'B' || data[1] != 'M') {
        fail(error, "BMP: bad signature");
        return std::nullopt;
    }
    ByteReader r(data, size);
    r.skip(2);
    (void)r.u32le();  // bfSize: writers get this wrong often enough that it cannot be a check
    (void)r.u16le();
    (void)r.u16le();
    const std::uint32_t offBits = r.u32le();
    return decodeDibAt(data, size, 14, /*icoEntry=*/false, offBits, error);
}

std::optional<Bitmap> decodeDib(const std::uint8_t* data, std::size_t size, bool icoEntry,
                                std::string* error) {
    if (data == nullptr || size < 12 || size > kMaxFileBytes) {
        fail(error, "BMP: the bitmap header is truncated");
        return std::nullopt;
    }
    return decodeDibAt(data, size, 0, icoEntry, /*explicitPixelOffset=*/0, error);
}

std::vector<std::uint8_t> encodeIcoDib(const ImageView& image) {
    if (!image.valid() || image.height > kMaxDim / 2)
        return {};
    const std::size_t xorStride = static_cast<std::size_t>(image.width) * 4u;
    const std::size_t maskStride = ((static_cast<std::size_t>(image.width) + 31u) / 32u) * 4u;

    ByteWriter w;
    w.u32le(40);
    w.i32le(static_cast<std::int32_t>(image.width));
    // THE ICO TRAP: biHeight spans the XOR image and the AND mask together. A writer that puts the
    // real height here produces icons that look right everywhere except in Explorer, which reads
    // the top half of a half-height image.
    w.i32le(static_cast<std::int32_t>(image.height) * 2);
    w.u16le(1);
    w.u16le(32);
    w.u32le(kBiRgb);
    w.u32le(static_cast<std::uint32_t>((xorStride + maskStride) * image.height));
    w.i32le(0);
    w.i32le(0);
    w.u32le(0);
    w.u32le(0);
    for (std::uint32_t i = 0; i < image.height; ++i) {
        const std::uint32_t y = image.height - 1 - i;  // bottom-up
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::uint8_t* px = image.at(x, y);
            w.u8(px[2]);
            w.u8(px[1]);
            w.u8(px[0]);
            w.u8(px[3]);
        }
    }
    // The AND mask, bottom-up, a set bit meaning transparent. Modern Windows reads the alpha
    // channel instead, but the mask is not optional: its bytes are counted in the entry's size and
    // an older shell really does use it.
    std::vector<std::uint8_t> maskRow(maskStride, 0);
    for (std::uint32_t i = 0; i < image.height; ++i) {
        const std::uint32_t y = image.height - 1 - i;
        std::fill(maskRow.begin(), maskRow.end(), std::uint8_t{0});
        for (std::uint32_t x = 0; x < image.width; ++x)
            if (image.at(x, y)[3] == 0)
                maskRow[x >> 3] = static_cast<std::uint8_t>(maskRow[x >> 3] |
                                                            (0x80u >> (x & 7u)));
        w.raw(maskRow.data(), maskRow.size());
    }
    return w.take();
}

} // namespace mosaicfmt
