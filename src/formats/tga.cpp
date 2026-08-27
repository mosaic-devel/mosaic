#include "formats/tga.hpp"

#include <cstring>

namespace mosaicfmt {
namespace {

constexpr std::size_t kExtensionAreaSize = 495;
constexpr std::size_t kFooterSize = 26;
constexpr std::size_t kAttributesTypeInExtension = 494;  // the last byte of the extension area

// The attributes-type values that matter. 1 and 2 mean "there are alpha bits but do not use them".
constexpr std::uint8_t kAttrNone = 0;
constexpr std::uint8_t kAttrStraight = 3;
constexpr std::uint8_t kAttrPremultiplied = 4;

[[nodiscard]] std::uint16_t pack5551(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                     bool opaque) noexcept {
    return static_cast<std::uint16_t>(((opaque ? 1u : 0u) << 15) | ((r >> 3) << 10) |
                                      ((g >> 3) << 5) | (b >> 3));
}

// 5 bits -> 8 by exact scaling (0x1F must land on 0xFF, not 0xF8).
[[nodiscard]] std::uint8_t from5(std::uint32_t v) noexcept {
    return static_cast<std::uint8_t>((v * 255u + 15u) / 31u);
}

} // namespace

std::optional<std::vector<std::uint8_t>> encodeTga(const ImageView& image, const TgaOptions& opts,
                                                   std::string* error) {
    if (!image.valid()) {
        fail(error, "TGA: nothing to encode");
        return std::nullopt;
    }
    if (image.width > 0xFFFFu || image.height > 0xFFFFu) {
        // The header's dimensions are 16-bit. Rejecting is the only honest answer -- the
        // alternative is a file that silently claims a different size than it holds.
        fail(error, "TGA: the format cannot describe an image larger than 65535 pixels a side");
        return std::nullopt;
    }

    const bool carriesAlpha =
        opts.depth != TgaOptions::Depth::Bgr24 && opts.alpha != TgaOptions::AlphaAttributes::Ignored;
    const bool premultiply = carriesAlpha && opts.alpha == TgaOptions::AlphaAttributes::Premultiplied;
    const std::uint32_t bytesPerPixel =
        opts.depth == TgaOptions::Depth::Bgra32 ? 4u : (opts.depth == TgaOptions::Depth::Bgr24 ? 3u : 2u);
    const std::uint8_t pixelDepth = static_cast<std::uint8_t>(bytesPerPixel * 8u);
    const std::uint8_t alphaBits =
        !carriesAlpha ? 0u : (opts.depth == TgaOptions::Depth::Bgra32 ? 8u : 1u);

    ByteWriter w;
    w.u8(0);                                       // id field length
    w.u8(0);                                       // colour-map type: none
    w.u8(opts.rle ? std::uint8_t{10} : std::uint8_t{2});  // truecolour, RLE or plain
    w.u16le(0);                                    // colour-map first entry
    w.u16le(0);                                    // colour-map length
    w.u8(0);                                       // colour-map entry size
    w.u16le(0);                                    // x origin
    w.u16le(0);                                    // y origin
    w.u16le(static_cast<std::uint16_t>(image.width));
    w.u16le(static_cast<std::uint16_t>(image.height));
    w.u8(pixelDepth);
    // The descriptor's low nibble is the attribute-bit count; bit 5 puts the origin at the top.
    w.u8(static_cast<std::uint8_t>(alphaBits | (opts.topDown ? 0x20u : 0x00u)));

    // One row's worth of packed pixels, then either raw or run-length coded. RLE packets never
    // cross a scanline boundary -- the v2 spec requires that, and some readers rely on it.
    std::vector<std::uint8_t> row(static_cast<std::size_t>(image.width) * bytesPerPixel);
    for (std::uint32_t i = 0; i < image.height; ++i) {
        const std::uint32_t y = opts.topDown ? i : image.height - 1 - i;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::uint8_t* px = image.at(x, y);
            std::uint8_t rgb[3] = {px[0], px[1], px[2]};
            std::uint8_t a = px[3];
            if (!carriesAlpha) {
                compositeOverMatte(px, opts.matte, rgb);
                a = 255;
            } else if (premultiply) {
                rgb[0] = static_cast<std::uint8_t>((px[0] * a + 127u) / 255u);
                rgb[1] = static_cast<std::uint8_t>((px[1] * a + 127u) / 255u);
                rgb[2] = static_cast<std::uint8_t>((px[2] * a + 127u) / 255u);
            }
            std::uint8_t* dst = row.data() + static_cast<std::size_t>(x) * bytesPerPixel;
            if (bytesPerPixel == 2) {
                const std::uint16_t v = pack5551(rgb[0], rgb[1], rgb[2], a >= 128u);
                dst[0] = static_cast<std::uint8_t>(v & 0xFFu);
                dst[1] = static_cast<std::uint8_t>(v >> 8);
            } else {
                dst[0] = rgb[2];
                dst[1] = rgb[1];
                dst[2] = rgb[0];
                if (bytesPerPixel == 4)
                    dst[3] = a;
            }
        }
        if (!opts.rle) {
            w.raw(row.data(), row.size());
            continue;
        }
        const auto same = [&](std::uint32_t a, std::uint32_t b) {
            return std::memcmp(row.data() + static_cast<std::size_t>(a) * bytesPerPixel,
                               row.data() + static_cast<std::size_t>(b) * bytesPerPixel,
                               bytesPerPixel) == 0;
        };
        std::uint32_t x = 0;
        while (x < image.width) {
            std::uint32_t run = 1;
            while (x + run < image.width && run < 128 && same(x, x + run))
                ++run;
            if (run >= 2) {
                w.u8(static_cast<std::uint8_t>(0x80u | (run - 1)));
                w.raw(row.data() + static_cast<std::size_t>(x) * bytesPerPixel, bytesPerPixel);
                x += run;
                continue;
            }
            std::uint32_t n = 0;
            while (x + n < image.width && n < 128) {
                if (x + n + 1 < image.width && same(x + n, x + n + 1))
                    break;  // a pair starts here: let the repetition packet have it
                ++n;
            }
            if (n == 0)
                n = 1;
            w.u8(static_cast<std::uint8_t>(n - 1));
            w.raw(row.data() + static_cast<std::size_t>(x) * bytesPerPixel,
                  static_cast<std::size_t>(n) * bytesPerPixel);
            x += n;
        }
    }

    // The v2 extension area. Its last byte -- the attributes type -- is the only standard place a
    // TGA states whether its alpha is straight or premultiplied, which is why it is always written.
    const std::uint32_t extensionOffset = static_cast<std::uint32_t>(w.size());
    w.u16le(static_cast<std::uint16_t>(kExtensionAreaSize));
    w.zeros(41);   // author name
    w.zeros(324);  // author comments
    w.zeros(12);   // date/time -- deliberately zero: an encode must be reproducible byte for byte
    w.zeros(41);   // job name
    w.zeros(6);    // job time
    w.zeros(41);   // software id
    w.zeros(3);    // software version
    w.u32le(0);    // key colour
    w.u16le(0);    // pixel aspect ratio numerator (0 = unspecified)
    w.u16le(0);    // ... denominator
    w.u16le(0);    // gamma numerator (0 = unspecified)
    w.u16le(0);    // ... denominator
    w.u32le(0);    // colour correction table offset
    w.u32le(0);    // postage stamp (thumbnail) offset
    w.u32le(0);    // scanline table offset
    w.u8(!carriesAlpha ? kAttrNone
                       : (premultiply ? kAttrPremultiplied : kAttrStraight));

    w.u32le(extensionOffset);
    w.u32le(0);  // developer directory: none
    w.text("TRUEVISION-XFILE.");
    w.u8(0);
    return w.take();
}

std::optional<Bitmap> decodeTga(const std::uint8_t* data, std::size_t size, std::string* error) {
    if (data == nullptr || size < 18 || size > kMaxFileBytes) {
        fail(error, "TGA: the header is truncated");
        return std::nullopt;
    }
    ByteReader r(data, size);
    const std::uint32_t idLength = r.u8();
    const std::uint32_t colourMapType = r.u8();
    const std::uint32_t imageType = r.u8();
    const std::uint32_t colourMapFirst = r.u16le();
    const std::uint32_t colourMapLength = r.u16le();
    const std::uint32_t colourMapBits = r.u8();
    (void)r.u16le();  // x origin -- a display hint, never a buffer offset
    (void)r.u16le();  // y origin
    const std::uint32_t width = r.u16le();
    const std::uint32_t height = r.u16le();
    const std::uint32_t pixelDepth = r.u8();
    const std::uint32_t descriptor = r.u8();

    const bool rle = imageType == 9 || imageType == 10 || imageType == 11;
    const bool indexed = imageType == 1 || imageType == 9;
    const bool greyscale = imageType == 3 || imageType == 11;
    if (imageType != 1 && imageType != 2 && imageType != 3 && imageType != 9 && imageType != 10 &&
        imageType != 11) {
        fail(error, "TGA: unsupported image type");
        return std::nullopt;
    }
    if (!dimensionsPlausible(width, height)) {
        fail(error, "TGA: implausible image dimensions");
        return std::nullopt;
    }
    if (pixelDepth != 8 && pixelDepth != 15 && pixelDepth != 16 && pixelDepth != 24 &&
        pixelDepth != 32) {
        fail(error, "TGA: unsupported pixel depth");
        return std::nullopt;
    }
    if (indexed && (colourMapType != 1 || colourMapLength == 0)) {
        fail(error, "TGA: a colour-mapped image without a colour map");
        return std::nullopt;
    }
    if (indexed && pixelDepth != 8 && pixelDepth != 16) {
        fail(error, "TGA: unsupported colour-map index width");
        return std::nullopt;
    }
    // ⚠ THE DEPTH HAS TO BE LEGAL FOR THE IMAGE TYPE, not merely legal in the abstract.
    //
    // The check above accepts {8, 15, 16, 24, 32} for every type, and readPixel below bounds its
    // reads by bytesPerPixel = (pixelDepth + 7) / 8. For a truecolour image those two disagree: a
    // type-2 (or RLE type-10) header declaring depth 8 gives bytesPerPixel == 1, so readPixel
    // checks that ONE byte is available and then reads three -- b, g, r -- off the end of the
    // buffer. A heap-buffer-overflow READ of attacker-controlled length, from opening a .tga.
    //
    // Found by fuzzing (libFuzzer + ASan, 1.1 M executions); it produced seventeen witnesses and
    // all seventeen were this. The other five decoders came back clean over the same run.
    //
    // The honest fix is here rather than at the read: a truecolour TGA is 15/16/24/32 and a
    // greyscale one is 8/16, by the format, so a header claiming otherwise is not a picture this
    // decoder can read -- and saying so up front keeps bytesPerPixel and readPixel's appetite in
    // agreement by construction instead of by coincidence.
    if (!indexed && !greyscale && pixelDepth != 15 && pixelDepth != 16 && pixelDepth != 24 &&
        pixelDepth != 32) {
        fail(error, "TGA: unsupported pixel depth for a true-colour image");
        return std::nullopt;
    }
    if (greyscale && pixelDepth != 8 && pixelDepth != 16) {
        fail(error, "TGA: unsupported pixel depth for a greyscale image");
        return std::nullopt;
    }

    r.skip(idLength);  // the id field is free-form text; nothing here reads it
    if (r.pos() != 18u + idLength) {
        fail(error, "TGA: the id field runs past the end of the file");
        return std::nullopt;
    }

    // The colour map. Its entries are the only allocation proportional to a declared count, and
    // that count is a 16-bit field, so the ceiling is 64 Ki entries -- 256 KB, whatever the file
    // claims about anything else.
    std::vector<Rgba8> palette;
    if (colourMapType == 1 && colourMapLength != 0) {
        if (colourMapBits != 15 && colourMapBits != 16 && colourMapBits != 24 &&
            colourMapBits != 32) {
            fail(error, "TGA: unsupported colour-map entry size");
            return std::nullopt;
        }
        const std::uint32_t entryBytes = (colourMapBits + 7u) / 8u;
        if (!r.has(static_cast<std::uint64_t>(colourMapLength) * entryBytes)) {
            fail(error, "TGA: the colour map is truncated");
            return std::nullopt;
        }
        palette.resize(colourMapLength);
        for (std::uint32_t i = 0; i < colourMapLength; ++i) {
            if (entryBytes == 2) {
                const std::uint32_t v = r.u16le();
                palette[i] = Rgba8{from5((v >> 10) & 0x1Fu), from5((v >> 5) & 0x1Fu),
                                   from5(v & 0x1Fu),
                                   colourMapBits == 16 && (v & 0x8000u) == 0u ? std::uint8_t{0}
                                                                             : std::uint8_t{255}};
            } else {
                const std::uint8_t b = r.u8();
                const std::uint8_t g = r.u8();
                const std::uint8_t rr = r.u8();
                const std::uint8_t a = entryBytes == 4 ? r.u8() : std::uint8_t{255};
                palette[i] = Rgba8{rr, g, b, a};
            }
        }
    }

    // The v2 extension area, when the footer points at one: its attributes type is what tells us
    // whether the alpha channel is straight, premultiplied or meaningless. A footer that lies
    // simply leaves us with the descriptor's attribute-bit count, which is the v1 answer.
    std::uint8_t attributes = 0xFF;  // 0xFF = "the file did not say"
    if (size >= kFooterSize &&
        std::memcmp(data + size - 18, "TRUEVISION-XFILE.", 17) == 0) {
        const std::size_t footer = size - kFooterSize;
        const std::uint32_t extensionOffset = static_cast<std::uint32_t>(data[footer]) |
                                              (static_cast<std::uint32_t>(data[footer + 1]) << 8) |
                                              (static_cast<std::uint32_t>(data[footer + 2]) << 16) |
                                              (static_cast<std::uint32_t>(data[footer + 3]) << 24);
        // ⚠ The subtraction has to be guarded, not just compared: a 30-byte file with a footer
        // would make `size - 495` wrap to an enormous number and let any offset through.
        if (extensionOffset != 0 && size >= kExtensionAreaSize &&
            extensionOffset <= size - kExtensionAreaSize)
            attributes = data[extensionOffset + kAttributesTypeInExtension];
    }
    const std::uint32_t attributeBits = descriptor & 0x0Fu;
    // Whether to believe the fourth byte at all. TGA is full of 32-bit files whose descriptor
    // claims zero attribute bits and whose alpha channel is nevertheless meant, so the rule is:
    // an explicit attributes type wins; otherwise the attribute-bit count decides; and a
    // 32-bit file that says nothing gets the same "all zero means opaque" reading BMP needs.
    bool useAlpha = pixelDepth == 32 || (pixelDepth == 16 && attributeBits == 1);
    if (attributes == kAttrNone || attributes == 1 || attributes == 2)
        useAlpha = false;
    const bool premultiplied = attributes == kAttrPremultiplied;

    const bool rightToLeft = (descriptor & 0x10u) != 0u;
    const bool topToBottom = (descriptor & 0x20u) != 0u;
    Bitmap out(width, height);
    const std::size_t total = static_cast<std::size_t>(width) * height;
    const std::uint32_t bytesPerPixel = (pixelDepth + 7u) / 8u;

    // Write pixel number `i` of the file's own scan order into the right place.
    const auto put = [&](std::size_t i, Rgba8 c) {
        const std::uint32_t sx = static_cast<std::uint32_t>(i % width);
        const std::uint32_t sy = static_cast<std::uint32_t>(i / width);
        std::uint8_t* p = out.at(rightToLeft ? width - 1 - sx : sx,
                                 topToBottom ? sy : height - 1 - sy);
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
        p[3] = c.a;
    };

    const auto readPixel = [&](Rgba8& c) {
        if (!r.has(bytesPerPixel))
            return false;
        if (indexed) {
            const std::uint32_t raw = bytesPerPixel == 1 ? r.u8() : r.u16le();
            if (raw < colourMapFirst)
                return false;
            const std::uint32_t index = raw - colourMapFirst;
            if (index >= palette.size())
                return false;
            c = palette[index];
            return true;
        }
        if (greyscale) {
            const std::uint8_t v = r.u8();
            std::uint8_t a = 255;
            if (bytesPerPixel == 2)  // 16-bit greyscale: value + alpha
                a = r.u8();
            c = Rgba8{v, v, v, useAlpha ? a : std::uint8_t{255}};
            return true;
        }
        if (bytesPerPixel == 2) {
            const std::uint32_t v = r.u16le();
            c = Rgba8{from5((v >> 10) & 0x1Fu), from5((v >> 5) & 0x1Fu), from5(v & 0x1Fu),
                      !useAlpha || (v & 0x8000u) != 0u ? std::uint8_t{255} : std::uint8_t{0}};
            return true;
        }
        const std::uint8_t b = r.u8();
        const std::uint8_t g = r.u8();
        const std::uint8_t rr = r.u8();
        const std::uint8_t a = bytesPerPixel == 4 ? r.u8() : std::uint8_t{255};
        c = Rgba8{rr, g, b, useAlpha ? a : std::uint8_t{255}};
        return true;
    };

    if (!rle) {
        for (std::size_t i = 0; i < total; ++i) {
            Rgba8 c;
            if (!readPixel(c)) {
                fail(error, "TGA: the pixel data is truncated");
                return std::nullopt;
            }
            put(i, c);
        }
    } else {
        std::size_t i = 0;
        while (i < total) {
            if (!r.has(1)) {
                fail(error, "TGA: the run-length stream is truncated");
                return std::nullopt;
            }
            const std::uint8_t packet = r.u8();
            const std::uint32_t count = (packet & 0x7Fu) + 1u;
            if (i + count > total) {
                fail(error, "TGA: a run-length packet describes more pixels than the image has");
                return std::nullopt;
            }
            if ((packet & 0x80u) != 0u) {  // repetition packet: one pixel, `count` times
                Rgba8 c;
                if (!readPixel(c)) {
                    fail(error, "TGA: the run-length stream is truncated");
                    return std::nullopt;
                }
                for (std::uint32_t k = 0; k < count; ++k)
                    put(i + k, c);
            } else {  // raw packet
                for (std::uint32_t k = 0; k < count; ++k) {
                    Rgba8 c;
                    if (!readPixel(c)) {
                        fail(error, "TGA: the run-length stream is truncated");
                        return std::nullopt;
                    }
                    put(i + k, c);
                }
            }
            i += count;
        }
    }

    if (premultiplied) {
        // Hand back straight alpha, always: everything above this library composites that way, and
        // a premultiplied buffer wearing a straight label is exactly how soft edges go dark.
        for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
            const unsigned a = out.rgba[i + 3];
            if (a == 0 || a == 255)
                continue;
            for (int c = 0; c < 3; ++c) {
                const unsigned v = (out.rgba[i + static_cast<std::size_t>(c)] * 255u + a / 2u) / a;
                out.rgba[i + static_cast<std::size_t>(c)] =
                    static_cast<std::uint8_t>(v > 255u ? 255u : v);
            }
        }
    } else if (pixelDepth == 32 && useAlpha && attributes == 0xFF) {
        // No attributes type, 8 attribute bits or not: a 32-bit Targa whose alpha is zero
        // everywhere is the well-known "the fourth byte was never filled in" file, and reading it
        // literally yields an entirely invisible image.
        bool any = false;
        for (std::size_t i = 3; i < out.rgba.size() && !any; i += 4)
            any = out.rgba[i] != 0;
        if (!any)
            for (std::size_t i = 3; i < out.rgba.size(); i += 4)
                out.rgba[i] = 255;
    }
    return out;
}

} // namespace mosaicfmt
