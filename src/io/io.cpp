#include "io/io.hpp"

#include "common/fs_path.hpp"

#include "io/detail.hpp"
#include "io/exif.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <utility>

namespace mosaic::io {
namespace {

[[nodiscard]] bool readFile(const std::string& path, std::vector<std::uint8_t>& out,
                            std::string* error) {
    std::ifstream f(common::pathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!f) {
        if (error)
            *error = "could not open the file";
        return false;
    }
    const std::streamoff size = f.tellg();
    if (size <= 0) {
        if (error)
            *error = "the file is empty";
        return false;
    }
    f.seekg(0);
    out.resize(static_cast<std::size_t>(size));
    if (!f.read(reinterpret_cast<char*>(out.data()), size)) {
        if (error)
            *error = "could not read the file";
        return false;
    }
    return true;
}

// The head of a file, up to `cap` bytes -- all a dimension probe needs. Short files simply
// yield what they have; empty/unreadable yields an empty vector.
[[nodiscard]] std::vector<std::uint8_t> readFileHead(const std::string& path, std::size_t cap) {
    std::vector<std::uint8_t> out;
    std::ifstream f(common::pathFromUtf8(path), std::ios::binary);
    if (!f)
        return out;
    out.resize(cap);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(cap));
    out.resize(static_cast<std::size_t>(f.gcount()));
    return out;
}

[[nodiscard]] std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

[[nodiscard]] std::uint32_t be16(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 8) | p[1];
}

} // namespace

std::string_view moduleName() noexcept { return "io"; }

std::vector<std::uint8_t> readIccProfile(const std::string& path) {
    if (path.empty())
        return {};
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};
    const std::streamoff size = file.tellg();
    // An ICC profile is at least the 128-byte header plus a 4-byte tag count. The upper bound is
    // a sanity limit, not a spec one: real display and press profiles run to a couple of MB, and
    // an export must not staple an arbitrary file of any size into every image it writes.
    if (size < 132 || size > 16 * 1024 * 1024)
        return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
        return {};

    // Two structural checks, because "the user picked a file" is not evidence that it IS a
    // profile: the header's big-endian size field must match the file exactly, and the 'acsp'
    // signature must sit at offset 36. Deeper validation belongs to whoever consumes the
    // profile -- libpng rejects a mismatched colour space itself, and gracefully.
    if (be32(bytes.data()) != bytes.size())
        return {};
    if (std::memcmp(bytes.data() + 36, "acsp", 4) != 0)
        return {};
    return bytes;
}

ImageFormat sniffImageFormat(const std::uint8_t* data, std::size_t size) noexcept {
    static constexpr std::uint8_t kPng[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (data == nullptr)
        return ImageFormat::Unknown;
    if (size >= 8 && std::memcmp(data, kPng, 8) == 0)
        return ImageFormat::Png;
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return ImageFormat::Jpeg;
    // RIFF....WEBP -- the four size bytes between the two tags are not part of the signature.
    if (size >= 12 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WEBP", 4) == 0)
        return ImageFormat::WebP;
    // ISO-BMFF: a `ftyp` box at offset 4, whose major brand names AVIF ("avif") or an AVIF image
    // sequence ("avis"). Deliberately narrow -- "heic"/"mif1" are the parked HEVC family and must
    // NOT sniff as something we claim to decode.
    if (size >= 12 && std::memcmp(data + 4, "ftyp", 4) == 0 &&
        (std::memcmp(data + 8, "avif", 4) == 0 || std::memcmp(data + 8, "avis", 4) == 0))
        return ImageFormat::Avif;
    // TIFF's byte-order mark plus its version word: 42 for classic, 43 for BigTIFF.
    if (size >= 4 &&
        ((data[0] == 'I' && data[1] == 'I' && data[3] == 0x00 &&
          (data[2] == 0x2A || data[2] == 0x2B)) ||
         (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 &&
          (data[3] == 0x2A || data[3] == 0x2B))))
        return ImageFormat::Tiff;
    if (size >= 6 &&
        (std::memcmp(data, "GIF87a", 6) == 0 || std::memcmp(data, "GIF89a", 6) == 0))
        return ImageFormat::Gif;
    return ImageFormat::Unknown;
}

std::string_view imageFormatName(ImageFormat format) noexcept {
    switch (format) {
    case ImageFormat::Png: return "PNG";
    case ImageFormat::Jpeg: return "JPEG";
    case ImageFormat::WebP: return "WebP";
    case ImageFormat::Avif: return "AVIF";
    case ImageFormat::Tiff: return "TIFF";
    case ImageFormat::Gif: return "GIF";
    case ImageFormat::Unknown: break;
    }
    return "unknown";
}

namespace {

// The one decode dispatch, shared by the file and the buffer entry points. `error` carries the
// codec's own reason; the caller supplies the "nothing recognised this" case.
[[nodiscard]] std::optional<common::Image> decodeSniffed(ImageFormat format,
                                                         const std::vector<std::uint8_t>& buf,
                                                         std::string* error) {
    switch (format) {
    case ImageFormat::Png: return detail::decodePng(buf, error);
    case ImageFormat::Jpeg: return detail::decodeJpeg(buf, error);
    case ImageFormat::WebP: return detail::decodeWebp(buf, error);
    case ImageFormat::Avif: return detail::decodeAvif(buf, error);
    case ImageFormat::Tiff: return detail::decodeTiff(buf, error);
    case ImageFormat::Gif: return detail::decodeGif(buf, error);
    case ImageFormat::Unknown: break;
    }
    // M5: the curated-pro formats identify themselves (BMP, TGA, PNM/PAM, QOI, ICO, Radiance HDR --
    // libmosaicformats), so Unknown here only means "none of the containers sniffed above". One
    // call covers all six, and it has to come BEFORE the error: without it those formats would be
    // export-only, which is not support for a format, it is half of it.
    if (format == ImageFormat::Unknown && detail::sniffCuratedFormat(buf))
        return detail::decodeCuratedFormat(buf, error);
    if (error)
        *error = "unrecognised image format";
    return std::nullopt;
}

} // namespace

std::optional<common::Image> loadImage(const std::string& path, std::string* error) {
    auto loaded = loadImageWithMetadata(path, error);
    if (!loaded)
        return std::nullopt;
    return std::move(loaded->image);
}

std::optional<common::Image> decodeImageBytes(const std::uint8_t* data, std::size_t size,
                                              std::string* error) {
    if (data == nullptr || size == 0) {
        if (error)
            *error = "no data to decode";
        return std::nullopt;
    }
    // The codec entry points take a vector; one copy of an already-in-memory file is the price of
    // not duplicating the readers for a span.
    const std::vector<std::uint8_t> buf(data, data + size);
    return decodeSniffed(sniffImageFormat(buf.data(), buf.size()), buf, error);
}

std::optional<ImageDimensions> probeImageDimensions(const std::string& path) {
    // 1 MB of head covers PNG's fixed-offset IHDR trivially and, for JPEG, the marker walk to
    // SOF even when a fat embedded ICC profile pads the APP segments out by hundreds of KB.
    const std::vector<std::uint8_t> head = readFileHead(path, 1u << 20);
    ImageDimensions dims;
    switch (sniffImageFormat(head.data(), head.size())) {
    case ImageFormat::Png:
        // Signature (8) + IHDR length/type (8) + width/height at fixed offsets 16/20.
        if (head.size() < 24)
            return std::nullopt;
        dims.width = be32(head.data() + 16);
        dims.height = be32(head.data() + 20);
        break;
    case ImageFormat::Jpeg: {
        // Walk the marker stream to the first SOFn frame header (C0-CF minus C4/C8/CC, which
        // are DHT/JPG/DAC). Height sits at +5, width at +7 inside the segment payload.
        std::size_t i = 2;
        bool found = false;
        while (!found && i + 9 <= head.size()) {
            if (head[i] != 0xFF) // desynced: not a marker where one is due
                return std::nullopt;
            const std::uint8_t m = head[i + 1];
            if (m == 0xFF) { // fill byte before a marker
                ++i;
                continue;
            }
            if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) { // standalone: SOI / RSTn, no length
                i += 2;
                continue;
            }
            if (m == 0xD9 || m == 0xDA) // EOI / start-of-scan: no frame header seen
                return std::nullopt;
            const std::size_t len = be16(head.data() + i + 2);
            if (len < 2)
                return std::nullopt;
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                if (len < 7)
                    return std::nullopt;
                dims.height = be16(head.data() + i + 5);
                dims.width = be16(head.data() + i + 7);
                found = true;
                break;
            }
            i += 2 + len;
        }
        if (!found)
            return std::nullopt;
        break;
    }
    case ImageFormat::Gif:
        // The logical screen descriptor sits at a fixed offset right after the 6-byte signature,
        // little-endian: this is the whole GIF header, so no walk is needed.
        if (head.size() < 10)
            return std::nullopt;
        dims.width = static_cast<std::uint32_t>(head[6]) | (static_cast<std::uint32_t>(head[7]) << 8);
        dims.height =
            static_cast<std::uint32_t>(head[8]) | (static_cast<std::uint32_t>(head[9]) << 8);
        break;
    case ImageFormat::WebP:
        // libwebp's own header parse -- the VP8/VP8L/VP8X variants each store the size
        // differently, and re-deriving that by hand would be a second parser to harden.
        if (!detail::probeWebpDimensions(head, dims.width, dims.height))
            return std::nullopt;
        break;
    case ImageFormat::Tiff:
    case ImageFormat::Avif:
    case ImageFormat::Unknown:
        // TIFF hides its dimensions behind an IFD walk and AVIF behind a box tree; both are a
        // decode in all but name. A caller that genuinely needs the size opens the file.
        return std::nullopt;
    }
    if (dims.width == 0 || dims.height == 0)
        return std::nullopt;
    // Match loadImage's orientation bake: a transposed shot (EXIF 5..8) swaps the axes. The
    // metadata usually sits in the first APP1/eXIf bytes, so the head is enough; a miss just
    // reports the stored orientation, which is honest.
    if (const auto exif = extractExif(head); exif && exif->orientation.value_or(1) >= 5)
        std::swap(dims.width, dims.height);
    return dims;
}

std::optional<LoadedImage> loadImageWithMetadata(const std::string& path, std::string* error) {
    std::vector<std::uint8_t> buf;
    if (!readFile(path, buf, error))
        return std::nullopt;
    std::optional<common::Image> img =
        decodeSniffed(sniffImageFormat(buf.data(), buf.size()), buf, error);
    if (!img)
        return std::nullopt;
    LoadedImage out;
    out.exif = extractExif(buf);
    if (out.exif.has_value()) {
        // Bake the shot rotation into the pixels -- sideways photos load upright -- then record
        // orientation 1: the stored pixels ARE upright now, and applying it twice would be a bug.
        if (out.exif->orientation.has_value()) {
            applyExifOrientation(*img, *out.exif->orientation);
            out.exif->orientation = 1;
        }
        if (!out.exif->hasAny())
            out.exif.reset();  // a well-formed payload with none of our tags is "no metadata"
    }
    out.image = std::move(*img);
    return out;
}

} // namespace mosaic::io
