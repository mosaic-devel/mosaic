#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// EXIF camera metadata -- the tiny, typed slice Mosaic actually consumes, not a general metadata
// system. io reads it when a photo is opened (io/exif.hpp), the source layer carries it
// (core::Layer::exif()), and the .mosaic manifest persists it (the optional "exif" layer field).
// Exactly the fields a feature has a use for today: the sky generator's "Estimate from layer"
// wants FocalLengthIn35mmFilm (the FOV is unrecoverable from pixels) plus DateTimeOriginal + GPS
// to prefill its date & place; Orientation drives the upright-on-load bake. Full metadata
// preservation and write-back on export stay the S41/S42 FormatBackend's job
// (docs/export-system-plan.md).
namespace mosaic::common {

// Make/Model byte cap. EXIF strings end up inside JSON manifests, so beyond the cap they are
// also restricted to printable ASCII at every parse site (a hostile Make full of raw bytes must
// never reach a JSON dump, which would throw on invalid UTF-8).
inline constexpr std::size_t kMaxExifString = 128;

// DateTimeOriginal (EXIF tag 36867), validated on parse: a real calendar date + wall-clock
// time. EXIF's field carries no timezone, so neither does this.
struct ExifDateTime {
    int year = 0;    // 1..9999
    int month = 0;   // 1..12
    int day = 0;     // 1..days-in-month (leap years honoured)
    int hour = 0;    // 0..23
    int minute = 0;  // 0..59
    int second = 0;  // 0..59
    bool operator==(const ExifDateTime&) const = default;
};

// Every field is optional: cameras write wildly different subsets, and a malformed tag simply
// stays absent -- absent is honest, a guessed value is not.
struct ExifData {
    // Tag 274, 1..8. Loaders BAKE the shot rotation into the pixels and then store 1 here
    // (io::applyExifOrientation): the pixels are upright, so applying it again would be a bug.
    std::optional<int> orientation;
    std::optional<double> focalLengthMm;           // tag 37386: the real lens focal length, mm
    std::optional<int> focalLength35mm;            // tag 41989: the FOV-recoverable one
    std::optional<ExifDateTime> dateTimeOriginal;  // tag 36867
    std::optional<double> gpsLatitude;             // signed decimal degrees, +N/-S, [-90, 90]
    std::optional<double> gpsLongitude;            // signed decimal degrees, +E/-W, [-180, 180]
    std::optional<std::string> make;               // tag 271, printable ASCII <= kMaxExifString
    std::optional<std::string> model;              // tag 272, same contract

    [[nodiscard]] bool hasAny() const noexcept {
        return orientation.has_value() || focalLengthMm.has_value() ||
               focalLength35mm.has_value() || dateTimeOriginal.has_value() ||
               gpsLatitude.has_value() || gpsLongitude.has_value() || make.has_value() ||
               model.has_value();
    }
    bool operator==(const ExifData&) const = default;
};

// The one date contract, shared by the EXIF parser and the .mosaic manifest reader so a value
// that passes one site can never be rejected by the other.
[[nodiscard]] constexpr bool isValidExifDateTime(const ExifDateTime& dt) noexcept {
    if (dt.year < 1 || dt.year > 9999 || dt.month < 1 || dt.month > 12)
        return false;
    if (dt.hour < 0 || dt.hour > 23 || dt.minute < 0 || dt.minute > 59 || dt.second < 0 ||
        dt.second > 59)
        return false;
    const bool leap = (dt.year % 4 == 0 && dt.year % 100 != 0) || dt.year % 400 == 0;
    constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int daysInMonth = (dt.month == 2 && leap) ? 29 : kDays[dt.month - 1];
    return dt.day >= 1 && dt.day <= daysInMonth;
}

// The one string contract (cap + printable ASCII), likewise shared by both parse sites.
[[nodiscard]] constexpr bool isValidExifString(std::string_view s) noexcept {
    if (s.size() > kMaxExifString)
        return false;
    for (const char c : s)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
            return false;
    return true;
}

}  // namespace mosaic::common
