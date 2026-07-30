#include "io/exif.hpp"

#include "io/io.hpp"

#include <cstring>
#include <string_view>

namespace mosaic::io {
namespace {

// A sane payload ceiling: real APP1 EXIF is under 64KB by JPEG's segment framing, and even a
// PNG eXIf carrying a preview thumbnail stays far below this. Anything larger is not a camera.
constexpr std::size_t kMaxExifPayload = std::size_t{1} << 20;  // 1 MiB

// An IFD is a camera's tag directory; real ones hold a few dozen entries. The cap only exists
// so a hostile count cannot make the walk arbitrarily long -- entries are never allocated.
constexpr std::uint32_t kMaxIfdEntries = 512;

// TIFF field types we consume (of the spec's 12; entries of any other type are skipped whole).
constexpr std::uint16_t kAscii = 2;     // byte string, count includes the trailing NUL
constexpr std::uint16_t kShort = 3;     // u16
constexpr std::uint16_t kLong = 4;      // u32
constexpr std::uint16_t kRational = 5;  // u32 numerator / u32 denominator

constexpr std::size_t typeSize(std::uint16_t type) noexcept {
    switch (type) {
    case kAscii: return 1;
    case kShort: return 2;
    case kLong: return 4;
    case kRational: return 8;
    default: return 0;  // a type we never read
    }
}

// The tags this slice reads (everything else is skipped unread -- see the header note).
constexpr std::uint16_t kTagMake = 271;
constexpr std::uint16_t kTagModel = 272;
constexpr std::uint16_t kTagOrientation = 274;
constexpr std::uint16_t kTagExifIfd = 34665;  // IFD0 -> Exif IFD pointer
constexpr std::uint16_t kTagGpsIfd = 34853;   // IFD0 -> GPS IFD pointer
constexpr std::uint16_t kTagDateTimeOriginal = 36867;
constexpr std::uint16_t kTagFocalLength = 37386;
constexpr std::uint16_t kTagFocalLength35mm = 41989;
constexpr std::uint16_t kTagGpsLatitudeRef = 1;  // GPS IFD tag space
constexpr std::uint16_t kTagGpsLatitude = 2;
constexpr std::uint16_t kTagGpsLongitudeRef = 3;
constexpr std::uint16_t kTagGpsLongitude = 4;

// A bounds-checked, byte-order-aware window over the payload. Every u16/u32 read is preceded by
// a has() check (walkIfd validates whole entry tables up front; value reads check per value).
struct View {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    bool littleEndian = true;

    [[nodiscard]] bool has(std::size_t off, std::uint64_t len) const noexcept {
        return off <= size && len <= size - off;
    }
    [[nodiscard]] std::uint16_t u16(std::size_t off) const noexcept {
        const std::uint16_t a = data[off], b = data[off + 1];
        return littleEndian ? static_cast<std::uint16_t>(a | b << 8)
                            : static_cast<std::uint16_t>(a << 8 | b);
    }
    [[nodiscard]] std::uint32_t u32(std::size_t off) const noexcept {
        const std::uint32_t a = data[off], b = data[off + 1], c = data[off + 2],
                            d = data[off + 3];
        return littleEndian ? (a | b << 8 | c << 16 | d << 24)
                            : (a << 24 | b << 16 | c << 8 | d);
    }
};

// One decoded IFD entry whose value bytes are proven in-bounds. `valueOff` is absolute.
struct Entry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::size_t valueOff = 0;
};

// Walk the IFD at `off`, handing every consumable entry to `perEntry`. false = a structural
// lie (offset/count outside the payload), which rejects the whole parse. Entries of types we
// never read are skipped without validating their value offsets: unknown and vendor types have
// sizes this parser does not know, and bytes it never reads cannot hurt it.
template <typename PerEntry>
[[nodiscard]] bool walkIfd(const View& v, std::uint32_t off, PerEntry&& perEntry) {
    if (off < 8 || !v.has(off, 2))  // an IFD inside the 8-byte TIFF header is nonsense
        return false;
    const std::uint32_t n = v.u16(off);
    if (n > kMaxIfdEntries || !v.has(off + 2, std::uint64_t{n} * 12))
        return false;  // the entry table itself must fit (the trailing next-IFD is never read)
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t e = off + 2 + std::size_t{i} * 12;
        Entry entry;
        entry.tag = v.u16(e);
        entry.type = v.u16(e + 2);
        entry.count = v.u32(e + 4);
        const std::size_t ts = typeSize(entry.type);
        if (ts == 0)
            continue;  // a type this slice never reads
        const std::uint64_t bytes = std::uint64_t{ts} * entry.count;
        if (entry.count == 0)
            continue;  // an empty value; nothing to read
        if (bytes <= 4) {
            entry.valueOff = e + 8;  // inline in the entry's own value field
        } else {
            entry.valueOff = v.u32(e + 8);
            if (!v.has(entry.valueOff, bytes))
                return false;  // a declared length pointing outside the payload: reject
        }
        perEntry(entry);
    }
    return true;
}

// A single unsigned rational -> double, or nullopt on the classic zero denominator.
[[nodiscard]] std::optional<double> rationalAt(const View& v, std::size_t off) {
    const std::uint32_t num = v.u32(off);
    const std::uint32_t den = v.u32(off + 4);
    if (den == 0)
        return std::nullopt;
    return static_cast<double>(num) / static_cast<double>(den);
}

// An ASCII value -> string_view over the payload, cut at the first NUL, cap + printable-ASCII
// enforced (common/exif.hpp's shared contract). nullopt = drop the field.
[[nodiscard]] std::optional<std::string_view> asciiAt(const View& v, const Entry& e) {
    if (e.type != kAscii || e.count > common::kMaxExifString + 1)  // +1: the trailing NUL
        return std::nullopt;
    std::string_view s(reinterpret_cast<const char*>(v.data + e.valueOff), e.count);
    if (const std::size_t nul = s.find('\0'); nul != std::string_view::npos)
        s = s.substr(0, nul);
    if (!common::isValidExifString(s))
        return std::nullopt;
    return s;
}

// "YYYY:MM:DD HH:MM:SS" (EXIF's one date spelling) -> validated ExifDateTime.
[[nodiscard]] std::optional<common::ExifDateTime> dateTimeFrom(std::string_view s) {
    if (s.size() != 19)
        return std::nullopt;
    for (std::size_t i = 0; i < 19; ++i) {
        const bool sep = i == 4 || i == 7 || i == 10 || i == 13 || i == 16;
        const char expect = i == 10 ? ' ' : ':';
        if (sep ? s[i] != expect : (s[i] < '0' || s[i] > '9'))
            return std::nullopt;
    }
    const auto num = [&](std::size_t at, std::size_t digits) {
        int value = 0;
        for (std::size_t i = 0; i < digits; ++i)
            value = value * 10 + (s[at + i] - '0');
        return value;
    };
    const common::ExifDateTime dt{num(0, 4), num(5, 2), num(8, 2),
                                  num(11, 2), num(14, 2), num(17, 2)};
    if (!common::isValidExifDateTime(dt))
        return std::nullopt;
    return dt;
}

// A GPS coordinate: three unsigned rationals (deg, min, sec) + a one-letter hemisphere ref ->
// signed decimal degrees, range-validated. nullopt = drop the coordinate.
[[nodiscard]] std::optional<double> gpsCoordinate(const View& v, const Entry& value,
                                                  const Entry& ref, char negative,
                                                  char positive, double range) {
    if (value.type != kRational || value.count != 3)
        return std::nullopt;
    const auto deg = rationalAt(v, value.valueOff);
    const auto min = rationalAt(v, value.valueOff + 8);
    const auto sec = rationalAt(v, value.valueOff + 16);
    if (!deg || !min || !sec)
        return std::nullopt;
    const auto hemisphere = asciiAt(v, ref);
    if (!hemisphere || hemisphere->size() != 1)
        return std::nullopt;
    const char h = (*hemisphere)[0];
    if (h != negative && h != positive)
        return std::nullopt;
    const double degrees = *deg + *min / 60.0 + *sec / 3600.0;
    if (!(degrees >= 0.0 && degrees <= range))  // NaN-proof spelling
        return std::nullopt;
    return h == negative ? -degrees : degrees;
}

// ---- containers ---------------------------------------------------------------------------

// JPEG: scan the segment chain from SOI for the first APP1 whose payload opens "Exif\0\0".
// Entropy-coded data never precedes SOS, so the scan stops there (and at EOI, and at any framing
// that lies about itself). Every iteration advances >= 2 bytes: termination is structural.
[[nodiscard]] std::optional<common::ExifData> exifFromJpeg(const std::vector<std::uint8_t>& f) {
    std::size_t pos = 2;  // past SOI (sniffed by the caller)
    while (pos + 1 < f.size()) {
        if (f[pos] != 0xFF)
            return std::nullopt;  // not a marker where a marker must be
        while (pos < f.size() && f[pos] == 0xFF)
            ++pos;  // fill bytes before a marker are legal padding
        if (pos >= f.size())
            return std::nullopt;
        const std::uint8_t marker = f[pos++];
        if (marker == 0x00)
            return std::nullopt;  // a stuffed byte outside entropy data: corrupt framing
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
            continue;  // standalone markers carry no length
        if (marker == 0xD9 || marker == 0xDA)
            return std::nullopt;  // EOI / SOS: APP1 lives before the scan data
        if (pos + 2 > f.size())
            return std::nullopt;
        const std::size_t len = std::size_t{f[pos]} << 8 | f[pos + 1];  // includes these 2 bytes
        if (len < 2 || len > f.size() - pos)
            return std::nullopt;
        if (marker == 0xE1 && len >= 8 && std::memcmp(f.data() + pos + 2, "Exif\0\0", 6) == 0)
            return parseExif(f.data() + pos + 8, len - 8);
        pos += len;
    }
    return std::nullopt;
}

// PNG: walk the chunk chain from past the signature to the eXIf chunk (standardized 2017).
// Every iteration advances >= 12 bytes (length + type + CRC): termination is structural.
[[nodiscard]] std::optional<common::ExifData> exifFromPng(const std::vector<std::uint8_t>& f) {
    std::size_t pos = 8;  // past the signature (sniffed by the caller)
    while (pos + 8 <= f.size()) {
        const std::size_t len = std::size_t{f[pos]} << 24 | std::size_t{f[pos + 1]} << 16 |
                                std::size_t{f[pos + 2]} << 8 | f[pos + 3];
        if (len > 0x7FFFFFFF || len + 4 > f.size() - pos - 8)
            return std::nullopt;  // the spec's length cap; payload + CRC must fit the file
        if (std::memcmp(f.data() + pos + 4, "eXIf", 4) == 0)
            return parseExif(f.data() + pos + 8, len);
        if (std::memcmp(f.data() + pos + 4, "IEND", 4) == 0)
            return std::nullopt;
        pos += 8 + len + 4;
    }
    return std::nullopt;
}

}  // namespace

std::optional<common::ExifData> parseExif(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 8 || size > kMaxExifPayload)
        return std::nullopt;
    View v{data, size, /*littleEndian=*/true};
    if (data[0] == 'I' && data[1] == 'I')
        v.littleEndian = true;
    else if (data[0] == 'M' && data[1] == 'M')
        v.littleEndian = false;
    else
        return std::nullopt;
    if (v.u16(2) != 42)
        return std::nullopt;

    common::ExifData out;
    std::uint32_t exifIfdOff = 0;  // 0 = not present (a real IFD can never sit at 0)
    std::uint32_t gpsIfdOff = 0;

    // IFD0: Make/Model/Orientation + the two sub-IFD pointers. First occurrence of a tag wins;
    // per-value oddities drop the field (the header's reject-vs-drop rule).
    const auto ifd0Entry = [&](const Entry& e) {
        switch (e.tag) {
        case kTagMake:
            if (!out.make)
                if (const auto s = asciiAt(v, e))
                    out.make = std::string(*s);
            break;
        case kTagModel:
            if (!out.model)
                if (const auto s = asciiAt(v, e))
                    out.model = std::string(*s);
            break;
        case kTagOrientation:
            if (!out.orientation && e.type == kShort && e.count == 1)
                if (const int o = v.u16(e.valueOff); o >= 1 && o <= 8)
                    out.orientation = o;
            break;
        case kTagExifIfd:
            if (exifIfdOff == 0 && e.type == kLong && e.count == 1)
                exifIfdOff = v.u32(e.valueOff);
            break;
        case kTagGpsIfd:
            if (gpsIfdOff == 0 && e.type == kLong && e.count == 1)
                gpsIfdOff = v.u32(e.valueOff);
            break;
        default: break;
        }
    };
    if (!walkIfd(v, v.u32(4), ifd0Entry))
        return std::nullopt;

    // The Exif sub-IFD: DateTimeOriginal + the two focal lengths. Pointer tags inside it are
    // ignored, so the walk can never loop -- at most these three IFDs are ever visited.
    if (exifIfdOff != 0) {
        const auto exifEntry = [&](const Entry& e) {
            switch (e.tag) {
            case kTagDateTimeOriginal:
                if (!out.dateTimeOriginal)
                    if (const auto s = asciiAt(v, e))
                        out.dateTimeOriginal = dateTimeFrom(*s);
                break;
            case kTagFocalLength:
                if (!out.focalLengthMm && e.type == kRational && e.count == 1)
                    if (const auto f = rationalAt(v, e.valueOff);
                        f && *f > 0.0 && *f <= 10000.0)
                        out.focalLengthMm = *f;
                break;
            case kTagFocalLength35mm:
                if (!out.focalLength35mm && e.type == kShort && e.count == 1)
                    if (const int f = v.u16(e.valueOff); f >= 1 && f <= 10000)
                        out.focalLength35mm = f;  // 0 is EXIF's spelling of "unknown"
                break;
            default: break;
            }
        };
        if (!walkIfd(v, exifIfdOff, exifEntry))
            return std::nullopt;
    }

    // The GPS sub-IFD: a coordinate needs its value AND its hemisphere ref, so collect both
    // first and combine after the walk.
    if (gpsIfdOff != 0) {
        std::optional<Entry> lat, latRef, lon, lonRef;
        const auto gpsEntry = [&](const Entry& e) {
            switch (e.tag) {
            case kTagGpsLatitudeRef:
                if (!latRef)
                    latRef = e;
                break;
            case kTagGpsLatitude:
                if (!lat)
                    lat = e;
                break;
            case kTagGpsLongitudeRef:
                if (!lonRef)
                    lonRef = e;
                break;
            case kTagGpsLongitude:
                if (!lon)
                    lon = e;
                break;
            default: break;
            }
        };
        if (!walkIfd(v, gpsIfdOff, gpsEntry))
            return std::nullopt;
        if (lat && latRef)
            out.gpsLatitude = gpsCoordinate(v, *lat, *latRef, 'S', 'N', 90.0);
        if (lon && lonRef)
            out.gpsLongitude = gpsCoordinate(v, *lon, *lonRef, 'W', 'E', 180.0);
    }

    return out;
}

std::optional<common::ExifData> extractExif(const std::vector<std::uint8_t>& file) {
    switch (sniffImageFormat(file.data(), file.size())) {
    case ImageFormat::Jpeg: return exifFromJpeg(file);
    case ImageFormat::Png: return exifFromPng(file);
    // The M4 containers carry EXIF too (a WebP "EXIF" chunk, an AVIF Exif item, a TIFF sub-IFD),
    // but each needs its own container walk -- and its own hostile-input suite -- while the read
    // side's only consumer today (the sky generator's estimate-from-layer) is fed camera JPEGs
    // and PNGs. The WRITE half already serves all three (io/exif_write.hpp).
    case ImageFormat::WebP:
    case ImageFormat::Avif:
    case ImageFormat::Tiff:
    case ImageFormat::Gif:
    case ImageFormat::Unknown: break;
    }
    return std::nullopt;
}

void applyExifOrientation(common::Image& img, int orientation) {
    if (orientation < 2 || orientation > 8 || img.empty())
        return;  // 1 = already upright; out of range = not a real orientation, leave it be
    const std::uint32_t w = img.width, h = img.height;
    const bool transposed = orientation >= 5;  // 5..8 swap the axes
    common::Image out(transposed ? h : w, transposed ? w : h);
    for (std::uint32_t y = 0; y < out.height; ++y) {
        for (std::uint32_t x = 0; x < out.width; ++x) {
            std::uint32_t sx = 0, sy = 0;
            switch (orientation) {
            case 2: sx = w - 1 - x; sy = y; break;          // mirrored horizontally
            case 3: sx = w - 1 - x; sy = h - 1 - y; break;  // rotated 180
            case 4: sx = x; sy = h - 1 - y; break;          // mirrored vertically
            case 5: sx = y; sy = x; break;                  // transposed
            case 6: sx = y; sy = h - 1 - x; break;          // rotated 90 CW (in the file)
            case 7: sx = w - 1 - y; sy = h - 1 - x; break;  // transverse
            default: sx = w - 1 - y; sy = x; break;         // 8: rotated 90 CCW
            }
            const std::size_t s = (std::size_t{sy} * w + sx) * 4;
            const std::size_t d = (std::size_t{y} * out.width + x) * 4;
            std::memcpy(out.rgba.data() + d, img.rgba.data() + s, 4);
        }
    }
    img = std::move(out);
}

}  // namespace mosaic::io
