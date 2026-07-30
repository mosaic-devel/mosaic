#include "io/exif_write.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace mosaic::io {
namespace {

// The TIFF field types and the tag numbers io/exif.cpp reads back. Deliberately duplicated
// rather than shared through a header: the reader's copy describes what it TOLERATES, this one
// describes what we EMIT, and the round-trip test is what proves the two agree. A shared
// constant would make the test tautological.
constexpr std::uint16_t kAscii = 2;
constexpr std::uint16_t kShort = 3;
constexpr std::uint16_t kLong = 4;
constexpr std::uint16_t kRational = 5;

constexpr std::uint16_t kTagMake = 271;
constexpr std::uint16_t kTagModel = 272;
constexpr std::uint16_t kTagOrientation = 274;
constexpr std::uint16_t kTagExifIfd = 34665;
constexpr std::uint16_t kTagGpsIfd = 34853;
constexpr std::uint16_t kTagDateTimeOriginal = 36867;
constexpr std::uint16_t kTagFocalLength = 37386;
constexpr std::uint16_t kTagFocalLength35mm = 41989;
constexpr std::uint16_t kTagGpsLatitudeRef = 1;
constexpr std::uint16_t kTagGpsLatitude = 2;
constexpr std::uint16_t kTagGpsLongitudeRef = 3;
constexpr std::uint16_t kTagGpsLongitude = 4;

// Seconds are stored as ten-thousandths (about 3 mm of ground distance at the equator) and the
// focal length as thousandths of a millimetre: both are far finer than any camera reports, and
// both keep the numerator inside 32 bits for the whole legal range.
constexpr std::uint32_t kSecondsScale = 10000;
constexpr std::uint32_t kFocalScale = 1000;

using Rational = std::pair<std::uint32_t, std::uint32_t>;

// One field, with its value already serialised little-endian. Values of four bytes or fewer live
// inside the directory entry; longer ones spill into the heap after the last IFD.
struct Field {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::vector<std::uint8_t> value;
};

void putU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

[[nodiscard]] Field shortField(std::uint16_t tag, std::uint16_t v) {
    Field f{tag, kShort, 1, {}};
    putU16(f.value, v);
    return f;
}

[[nodiscard]] Field longField(std::uint16_t tag, std::uint32_t v) {
    Field f{tag, kLong, 1, {}};
    putU32(f.value, v);
    return f;
}

// EXIF ASCII counts INCLUDE the terminating NUL, which is why the reader's cap is
// kMaxExifString + 1 rather than kMaxExifString.
[[nodiscard]] Field asciiField(std::uint16_t tag, std::string_view s) {
    Field f{tag, kAscii, static_cast<std::uint32_t>(s.size() + 1), {}};
    f.value.assign(s.begin(), s.end());
    f.value.push_back(0);
    return f;
}

[[nodiscard]] Field rationalField(std::uint16_t tag, const std::vector<Rational>& values) {
    Field f{tag, kRational, static_cast<std::uint32_t>(values.size()), {}};
    for (const Rational& r : values) {
        putU32(f.value, r.first);
        putU32(f.value, r.second);
    }
    return f;
}

// EXIF's one date spelling, "YYYY:MM:DD HH:MM:SS", built without printf so no format-truncation
// diagnostic has to be argued with. The caller has already validated the fields, so every part
// is in range and the result is exactly 19 characters.
[[nodiscard]] std::string formatDateTime(const common::ExifDateTime& dt) {
    const auto pad = [](int value, std::size_t digits) {
        std::string s = std::to_string(value);
        while (s.size() < digits)
            s.insert(s.begin(), '0');
        return s;
    };
    return pad(dt.year, 4) + ':' + pad(dt.month, 2) + ':' + pad(dt.day, 2) + ' ' +
           pad(dt.hour, 2) + ':' + pad(dt.minute, 2) + ':' + pad(dt.second, 2);
}

[[nodiscard]] Rational toRational(double v) {
    const double clamped = std::clamp(v, 0.0, 10000.0);
    return {static_cast<std::uint32_t>(clamped * kFocalScale + 0.5), kFocalScale};
}

// Decimal degrees -> the degrees/minutes/seconds triple EXIF stores. `degrees` is already the
// absolute value; the hemisphere travels separately, in its own Ref tag.
[[nodiscard]] std::vector<Rational> toDegreesMinutesSeconds(double degrees) {
    const double clamped = std::clamp(degrees, 0.0, 180.0);
    const auto d = static_cast<std::uint32_t>(clamped);
    const double minutes = (clamped - static_cast<double>(d)) * 60.0;
    const auto m = static_cast<std::uint32_t>(minutes);
    std::uint32_t s = static_cast<std::uint32_t>(
        (minutes - static_cast<double>(m)) * 60.0 * kSecondsScale + 0.5);
    // A seconds value that rounds up to a full 60 would read back as the next minute and could
    // push a pole-adjacent coordinate past its own range check. One ten-thousandth of a second
    // below is still four orders of magnitude finer than any GPS receiver.
    if (s >= 60u * kSecondsScale)
        s = 60u * kSecondsScale - 1u;
    return {Rational{d, 1u}, Rational{m, 1u}, Rational{s, kSecondsScale}};
}

// Entry count + 12 bytes per entry + the next-IFD pointer.
[[nodiscard]] std::size_t ifdSize(std::size_t entries) { return 2 + entries * 12 + 4; }

// Serialise one IFD into `out`, spilling over-long values into `heap` (whose first byte will sit
// at `heapOffset` in the finished payload). `heap` is shared across all three IFDs and only ever
// appended to, so the running offset stays correct for every later value.
void writeIfd(std::vector<std::uint8_t>& out, const std::vector<Field>& fields,
              std::vector<std::uint8_t>& heap, std::size_t heapOffset) {
    putU16(out, static_cast<std::uint16_t>(fields.size()));
    for (const Field& f : fields) {
        putU16(out, f.tag);
        putU16(out, f.type);
        putU32(out, f.count);
        if (f.value.size() <= 4) {
            const std::size_t before = out.size();
            out.insert(out.end(), f.value.begin(), f.value.end());
            out.resize(before + 4, 0);  // the value field is always four bytes wide
        } else {
            putU32(out, static_cast<std::uint32_t>(heapOffset + heap.size()));
            heap.insert(heap.end(), f.value.begin(), f.value.end());
            if (heap.size() % 2 != 0)
                heap.push_back(0);  // TIFF values start on even offsets
        }
    }
    putU32(out, 0);  // no next IFD; the reader never follows one in any case
}

}  // namespace

std::vector<std::uint8_t> buildExifPayload(const common::ExifData& data) {
    std::vector<Field> ifd0;
    std::vector<Field> exifIfd;
    std::vector<Field> gpsIfd;

    // IFD0. Tags ascend, as TIFF requires -- the reader does not care, but every other reader in
    // the world might.
    if (data.make.has_value() && common::isValidExifString(*data.make) && !data.make->empty())
        ifd0.push_back(asciiField(kTagMake, *data.make));
    if (data.model.has_value() && common::isValidExifString(*data.model) && !data.model->empty())
        ifd0.push_back(asciiField(kTagModel, *data.model));
    if (data.orientation.has_value() && *data.orientation >= 1 && *data.orientation <= 8)
        ifd0.push_back(shortField(kTagOrientation, static_cast<std::uint16_t>(*data.orientation)));

    // The Exif sub-IFD. The range tests mirror io/exif.cpp's exactly: writing a value our own
    // reader would drop would make the round-trip silently lossy.
    if (data.dateTimeOriginal.has_value() &&
        common::isValidExifDateTime(*data.dateTimeOriginal))
        exifIfd.push_back(asciiField(kTagDateTimeOriginal, formatDateTime(*data.dateTimeOriginal)));
    if (data.focalLengthMm.has_value() && *data.focalLengthMm > 0.0 &&
        *data.focalLengthMm <= 10000.0)
        exifIfd.push_back(rationalField(kTagFocalLength, {toRational(*data.focalLengthMm)}));
    if (data.focalLength35mm.has_value() && *data.focalLength35mm >= 1 &&
        *data.focalLength35mm <= 10000)
        exifIfd.push_back(
            shortField(kTagFocalLength35mm, static_cast<std::uint16_t>(*data.focalLength35mm)));

    // The GPS sub-IFD. Each coordinate is a value plus its hemisphere ref; the reader drops a
    // coordinate that arrives without its ref, so the two are always written together.
    if (data.gpsLatitude.has_value() && *data.gpsLatitude >= -90.0 && *data.gpsLatitude <= 90.0) {
        gpsIfd.push_back(asciiField(kTagGpsLatitudeRef, *data.gpsLatitude < 0.0 ? "S" : "N"));
        gpsIfd.push_back(
            rationalField(kTagGpsLatitude, toDegreesMinutesSeconds(std::abs(*data.gpsLatitude))));
    }
    if (data.gpsLongitude.has_value() && *data.gpsLongitude >= -180.0 &&
        *data.gpsLongitude <= 180.0) {
        gpsIfd.push_back(asciiField(kTagGpsLongitudeRef, *data.gpsLongitude < 0.0 ? "W" : "E"));
        gpsIfd.push_back(
            rationalField(kTagGpsLongitude, toDegreesMinutesSeconds(std::abs(*data.gpsLongitude))));
    }

    // The two sub-IFD pointers join IFD0 last (and highest-numbered), with placeholder targets --
    // their offsets are not known until IFD0's own length is, and its length depends on them.
    constexpr std::size_t kNoPointer = static_cast<std::size_t>(-1);
    std::size_t exifPointer = kNoPointer;
    std::size_t gpsPointer = kNoPointer;
    if (!exifIfd.empty()) {
        exifPointer = ifd0.size();
        ifd0.push_back(longField(kTagExifIfd, 0));
    }
    if (!gpsIfd.empty()) {
        gpsPointer = ifd0.size();
        ifd0.push_back(longField(kTagGpsIfd, 0));
    }
    if (ifd0.empty())
        return {};  // nothing worth saying: write no metadata at all

    constexpr std::size_t kHeaderSize = 8;  // "II", 42, the IFD0 offset
    const std::size_t ifd0Offset = kHeaderSize;
    const std::size_t exifOffset = ifd0Offset + ifdSize(ifd0.size());
    const std::size_t gpsOffset = exifOffset + (exifIfd.empty() ? 0 : ifdSize(exifIfd.size()));
    const std::size_t heapOffset = gpsOffset + (gpsIfd.empty() ? 0 : ifdSize(gpsIfd.size()));
    if (exifPointer != kNoPointer)
        ifd0[exifPointer] = longField(kTagExifIfd, static_cast<std::uint32_t>(exifOffset));
    if (gpsPointer != kNoPointer)
        ifd0[gpsPointer] = longField(kTagGpsIfd, static_cast<std::uint32_t>(gpsOffset));

    std::vector<std::uint8_t> out;
    out.push_back('I');  // little-endian, matching everything putU16/putU32 write
    out.push_back('I');
    putU16(out, 42);
    putU32(out, static_cast<std::uint32_t>(ifd0Offset));

    std::vector<std::uint8_t> heap;
    writeIfd(out, ifd0, heap, heapOffset);
    if (!exifIfd.empty())
        writeIfd(out, exifIfd, heap, heapOffset);
    if (!gpsIfd.empty())
        writeIfd(out, gpsIfd, heap, heapOffset);
    out.insert(out.end(), heap.begin(), heap.end());
    return out;
}

common::ExifData exifForExport(const common::ExifData& data) {
    common::ExifData out = data;
    if (out.orientation.has_value())
        out.orientation = 1;  // the pixels ARE upright; see the header for why this is not optional
    return out;
}

}  // namespace mosaic::io
