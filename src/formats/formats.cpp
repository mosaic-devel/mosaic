#include "formats/formats.hpp"

#include <algorithm>
#include <cstring>

namespace mosaicfmt {
namespace {

[[nodiscard]] bool pnmSpace(std::uint8_t c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// The DIB header sizes a BMP can legally open with: BITMAPCOREHEADER, BITMAPINFOHEADER and its
// three grown spellings (the +12 and +16 Windows CE variants, OS/2 2.x's 64), V4 and V5. Checking
// this -- rather than trusting the two-byte "BM" -- is what makes the BMP sniff worth having:
// "BM" alone matches an enormous amount of text.
[[nodiscard]] bool dibHeaderSizeKnown(std::uint32_t size) noexcept {
    return size == 12 || size == 40 || size == 52 || size == 56 || size == 64 || size == 108 ||
           size == 124;
}

[[nodiscard]] std::uint32_t u16At(const std::uint8_t* d, std::size_t off) noexcept {
    return static_cast<std::uint32_t>(d[off]) | (static_cast<std::uint32_t>(d[off + 1]) << 8);
}

[[nodiscard]] std::uint32_t u32At(const std::uint8_t* d, std::size_t off) noexcept {
    return u16At(d, off) | (u16At(d, off + 2) << 16);
}

// TGA HAS NO MAGIC NUMBER -- its 18-byte header is nothing but small integers, which is exactly
// why this is the last thing sniff() tries. Acceptance therefore demands that the whole header be
// self-consistent AND that everything it declares actually fit the buffer: an id field, a colour
// map of the declared entry size, and (for the uncompressed types, whose length is known exactly)
// the pixels themselves. A file that fails this is simply not opened as a TGA -- deliberately
// choosing to reject a few unusual-but-legal files over guessing at somebody else's format.
[[nodiscard]] bool tgaHeaderPlausible(const std::uint8_t* d, std::size_t size) noexcept {
    if (size < 18)
        return false;
    const std::uint32_t idLength = d[0];
    const std::uint32_t colourMapType = d[1];
    const std::uint32_t imageType = d[2];
    const std::uint32_t colourMapLength = u16At(d, 5);
    const std::uint32_t colourMapBits = d[7];
    const std::uint32_t width = u16At(d, 12);
    const std::uint32_t height = u16At(d, 14);
    const std::uint32_t depth = d[16];
    const std::uint32_t descriptor = d[17];

    if (colourMapType > 1)
        return false;
    const bool indexed = imageType == 1 || imageType == 9;
    if (imageType != 1 && imageType != 2 && imageType != 3 && imageType != 9 && imageType != 10 &&
        imageType != 11)
        return false;  // 0 = no image data; 32/33 = the Huffman/Delta types nobody writes
    if (indexed != (colourMapType == 1))
        return false;
    if (indexed && (colourMapLength == 0 || colourMapLength > 256))
        return false;
    if (colourMapType == 1 && colourMapBits != 15 && colourMapBits != 16 && colourMapBits != 24 &&
        colourMapBits != 32)
        return false;
    if (depth != 8 && depth != 15 && depth != 16 && depth != 24 && depth != 32)
        return false;
    if (!dimensionsPlausible(width, height))
        return false;
    if ((descriptor & 0xC0u) != 0)
        return false;  // TGA 2.0 reserves the top two descriptor bits as zero

    const std::uint64_t colourMapBytes =
        colourMapType == 1 ? std::uint64_t{colourMapLength} * ((colourMapBits + 7) / 8) : 0;
    const std::uint64_t headerBytes = 18u + idLength + colourMapBytes;
    if (headerBytes > size)
        return false;
    if (imageType == 1 || imageType == 2 || imageType == 3) {
        const std::uint64_t payload = std::uint64_t{width} * height * ((depth + 7) / 8);
        if (headerBytes + payload > size)
            return false;
    }
    return true;
}

} // namespace

Codec sniff(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size == 0)
        return Codec::None;
    // BMP: "BM" plus a DIB header whose declared size is one of the real ones.
    if (size >= 18 && data[0] == 'B' && data[1] == 'M' && dibHeaderSizeKnown(u32At(data, 14)))
        return Codec::Bmp;
    if (size >= 4 && std::memcmp(data, "qoif", 4) == 0)
        return Codec::Qoi;
    // Radiance's magic is "#?" followed by the writing program's name; the two spellings in the
    // wild are the format's own and Greg Ward's original. A bare "#?" is not enough -- it is also
    // a perfectly ordinary comment opener.
    if (size >= 10 && std::memcmp(data, "#?RADIANCE", 10) == 0)
        return Codec::RadianceHdr;
    if (size >= 6 && std::memcmp(data, "#?RGBE", 6) == 0)
        return Codec::RadianceHdr;
    // PNM: 'P', a variant digit, then whitespace. The whitespace matters: "P3D" is not a PPM.
    if (size >= 3 && data[0] == 'P' && data[1] >= '1' && data[1] <= '7' && pnmSpace(data[2]))
        return Codec::Pnm;
    // ICO: a two-byte zero reserved field, type 1 (icon) or 2 (cursor), and at least one entry.
    if (size >= 6 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || data[2] == 2) &&
        data[3] == 0 && u16At(data, 4) != 0)
        return Codec::Ico;
    if (tgaHeaderPlausible(data, size))
        return Codec::Tga;
    return Codec::None;
}

std::string_view codecName(Codec codec) noexcept {
    switch (codec) {
    case Codec::Bmp: return "BMP";
    case Codec::Ico: return "ICO";
    case Codec::Pnm: return "PNM";
    case Codec::Qoi: return "QOI";
    case Codec::RadianceHdr: return "Radiance HDR";
    case Codec::Tga: return "TGA";
    case Codec::None: break;
    }
    return "unknown";
}

Bitmap downscaleBox(const ImageView& src, std::uint32_t outW, std::uint32_t outH) {
    if (!src.valid() || outW == 0 || outH == 0 || outW > src.width || outH > src.height)
        return {};
    Bitmap out(outW, outH);
    for (std::uint32_t y = 0; y < outH; ++y) {
        const std::uint32_t y0 =
            static_cast<std::uint32_t>(std::uint64_t{y} * src.height / outH);
        std::uint32_t y1 =
            static_cast<std::uint32_t>(std::uint64_t{y + 1} * src.height / outH);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (std::uint32_t x = 0; x < outW; ++x) {
            const std::uint32_t x0 =
                static_cast<std::uint32_t>(std::uint64_t{x} * src.width / outW);
            std::uint32_t x1 =
                static_cast<std::uint32_t>(std::uint64_t{x + 1} * src.width / outW);
            if (x1 <= x0)
                x1 = x0 + 1;
            std::uint64_t sumA = 0, sumR = 0, sumG = 0, sumB = 0;
            std::uint32_t n = 0;
            for (std::uint32_t sy = y0; sy < y1; ++sy)
                for (std::uint32_t sx = x0; sx < x1; ++sx) {
                    const std::uint8_t* p = src.at(sx, sy);
                    sumA += p[3];
                    sumR += std::uint64_t{p[0]} * p[3];  // premultiplied, so a soft edge cannot
                    sumG += std::uint64_t{p[1]} * p[3];  // drag the colour hidden behind it
                    sumB += std::uint64_t{p[2]} * p[3];
                    ++n;
                }
            std::uint8_t* d = out.at(x, y);
            if (sumA == 0) {
                d[0] = d[1] = d[2] = d[3] = 0;  // wholly transparent: no colour to recover
                continue;
            }
            d[0] = static_cast<std::uint8_t>((sumR + sumA / 2) / sumA);
            d[1] = static_cast<std::uint8_t>((sumG + sumA / 2) / sumA);
            d[2] = static_cast<std::uint8_t>((sumB + sumA / 2) / sumA);
            d[3] = static_cast<std::uint8_t>((sumA + n / 2) / n);
        }
    }
    return out;
}

Bitmap fitSquare(const ImageView& src, std::uint32_t side) {
    if (!src.valid() || side == 0 || side > kMaxDim)
        return {};

    // The largest aspect-preserving box that fits the square, then clamped so we never upscale:
    // an 8 px source in a 32 px entry stays 8 px and is centred, which is what a designer would
    // do by hand and is far better than four times the pixels and none of the detail.
    std::uint32_t w = side, h = side;
    if (src.width >= src.height)
        h = static_cast<std::uint32_t>(
            std::max<std::uint64_t>(1, std::uint64_t{side} * src.height / src.width));
    else
        w = static_cast<std::uint32_t>(
            std::max<std::uint64_t>(1, std::uint64_t{side} * src.width / src.height));
    if (w > src.width)
        w = src.width;
    if (h > src.height)
        h = src.height;

    Bitmap scaled;
    if (w == src.width && h == src.height) {
        scaled.width = src.width;
        scaled.height = src.height;
        scaled.rgba.assign(src.rgba, src.rgba + static_cast<std::size_t>(w) * h * 4);
    } else {
        scaled = downscaleBox(src, w, h);
    }
    if (scaled.empty())
        return {};
    if (scaled.width == side && scaled.height == side)
        return scaled;

    Bitmap out(side, side);  // transparent ground
    const std::uint32_t ox = (side - scaled.width) / 2;
    const std::uint32_t oy = (side - scaled.height) / 2;
    for (std::uint32_t y = 0; y < scaled.height; ++y)
        std::memcpy(out.at(ox, oy + y), scaled.at(0, y),
                    static_cast<std::size_t>(scaled.width) * 4);
    return out;
}

} // namespace mosaicfmt
