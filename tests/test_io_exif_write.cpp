#include "common/exif.hpp"
#include "common/image.hpp"
#include "io/exif.hpp"
#include "io/exif_write.hpp"
#include "io/io.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// io/exif_write -- the WRITE half of the EXIF slice (M4). The contract the whole thing rests on
// is a round trip through the EXISTING reader: buildExifPayload -> parseExif must return what
// went in, and must never emit a field the reader would then reject.
namespace {

using mosaic::common::Color8;
using mosaic::common::ExifData;
using mosaic::common::ExifDateTime;
using mosaic::common::Image;
using mosaic::io::buildExifPayload;
using mosaic::io::parseExif;

std::optional<ExifData> roundTrip(const ExifData& in) {
    const std::vector<std::uint8_t> payload = buildExifPayload(in);
    if (payload.empty())
        return std::nullopt;
    return parseExif(payload.data(), payload.size());
}

ExifData fullyPopulated() {
    ExifData d;
    d.orientation = 1;
    d.focalLengthMm = 35.5;
    d.focalLength35mm = 53;
    d.dateTimeOriginal = ExifDateTime{2026, 7, 28, 14, 5, 9};
    d.gpsLatitude = 52.516300;
    d.gpsLongitude = 13.377700;
    d.make = "Mosaic";
    d.model = "Test Rig 9000";
    return d;
}

Image pattern(std::uint32_t w, std::uint32_t h) {
    Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            img.rgba[p + 0] = static_cast<std::uint8_t>(x * 37 + 3);
            img.rgba[p + 1] = static_cast<std::uint8_t>(y * 53 + 11);
            img.rgba[p + 2] = static_cast<std::uint8_t>((x + y) * 17);
            img.rgba[p + 3] = static_cast<std::uint8_t>((x * y) % 256);
        }
    return img;
}

} // namespace

TEST_CASE("nothing to say produces no payload at all") {
    CHECK(buildExifPayload(ExifData{}).empty());

    // Fields that the READER would refuse are not written either -- an empty payload is honest,
    // a payload our own parser rejects would be a silent round-trip hole.
    ExifData bad;
    bad.orientation = 42;
    bad.focalLengthMm = -3.0;
    bad.focalLength35mm = 0;  // EXIF's spelling of "unknown"
    bad.gpsLatitude = 1000.0;
    bad.make = std::string("\x01\x02 not printable");
    CHECK(buildExifPayload(bad).empty());
}

TEST_CASE("a fully populated record survives the round trip") {
    const ExifData in = fullyPopulated();
    const std::vector<std::uint8_t> payload = buildExifPayload(in);
    REQUIRE_FALSE(payload.empty());
    // Little-endian TIFF: "II", 42. Every container stores exactly these bytes.
    CHECK(payload[0] == 'I');
    CHECK(payload[1] == 'I');
    CHECK(payload[2] == 42);
    CHECK(payload[3] == 0);

    const auto out = parseExif(payload.data(), payload.size());
    REQUIRE(out.has_value());
    CHECK(out->orientation == 1);
    CHECK(out->focalLength35mm == 53);
    REQUIRE(out->dateTimeOriginal.has_value());
    CHECK(*out->dateTimeOriginal == *in.dateTimeOriginal);
    CHECK(out->make == in.make);
    CHECK(out->model == in.model);
    // The two numeric fields are stored as rationals, so they come back to the wire format's own
    // resolution rather than bit-identically: thousandths of a millimetre, ten-thousandths of an
    // arc-second.
    REQUIRE(out->focalLengthMm.has_value());
    // ⚠ NOT `Approx(35.5).epsilon(0.0)`: doctest's Approx compares `fabs(diff) < epsilon * scale`,
    // so a zero epsilon is a STRICT `< 0` that no pair of values can satisfy -- the assertion fails
    // even against an identical double. 35.5 lands on kFocalScale's grid (35500/1000) and survives
    // exactly, so this is a plain equality.
    CHECK(*out->focalLengthMm == 35.5);
    REQUIRE(out->gpsLatitude.has_value());
    REQUIRE(out->gpsLongitude.has_value());
    CHECK(*out->gpsLatitude == doctest::Approx(52.5163).epsilon(1e-7));
    CHECK(*out->gpsLongitude == doctest::Approx(13.3777).epsilon(1e-7));
}

TEST_CASE("each field travels on its own, and the hemispheres are signed correctly") {
    ExifData only;
    only.model = "Lone Field";
    const auto model = roundTrip(only);
    REQUIRE(model.has_value());
    CHECK(model->model == "Lone Field");
    CHECK_FALSE(model->make.has_value());
    CHECK_FALSE(model->gpsLatitude.has_value());

    ExifData gpsOnly;
    gpsOnly.gpsLatitude = -33.8688;   // southern
    gpsOnly.gpsLongitude = -70.6693;  // western
    const auto south = roundTrip(gpsOnly);
    REQUIRE(south.has_value());
    REQUIRE(south->gpsLatitude.has_value());
    REQUIRE(south->gpsLongitude.has_value());
    CHECK(*south->gpsLatitude == doctest::Approx(-33.8688).epsilon(1e-7));
    CHECK(*south->gpsLongitude == doctest::Approx(-70.6693).epsilon(1e-7));
    CHECK_FALSE(south->model.has_value());

    // A latitude at the very edge must not round its seconds past its own range check.
    ExifData pole;
    pole.gpsLatitude = 90.0;
    const auto atPole = roundTrip(pole);
    REQUIRE(atPole.has_value());
    REQUIRE(atPole->gpsLatitude.has_value());
    CHECK(*atPole->gpsLatitude == doctest::Approx(90.0).epsilon(1e-9));

    ExifData dateOnly;
    dateOnly.dateTimeOriginal = ExifDateTime{2000, 2, 29, 0, 0, 0};  // a real leap day
    const auto leap = roundTrip(dateOnly);
    REQUIRE(leap.has_value());
    REQUIRE(leap->dateTimeOriginal.has_value());
    CHECK(leap->dateTimeOriginal->year == 2000);
    CHECK(leap->dateTimeOriginal->month == 2);
    CHECK(leap->dateTimeOriginal->day == 29);
}

TEST_CASE("a maximum-length string still fits the reader's cap") {
    ExifData d;
    d.make = std::string(mosaic::common::kMaxExifString, 'M');  // exactly at the cap
    const auto out = roundTrip(d);
    REQUIRE(out.has_value());
    REQUIRE(out->make.has_value());
    CHECK(out->make->size() == mosaic::common::kMaxExifString);
}

// ---- through a real container -----------------------------------------------------------------

TEST_CASE("a PNG written with metadata carries it back out, pixels untouched") {
    const Image src = pattern(9, 5);
    mosaic::io::PngSaveOptions opts;
    opts.metadata.exif = buildExifPayload(fullyPopulated());
    opts.metadata.dpi = 300.0;
    REQUIRE_FALSE(opts.metadata.exif.empty());

    std::string err;
    const auto bytes = mosaic::io::encodePng(src, opts, &err);
    REQUIRE_MESSAGE(bytes.has_value(), err);

    // The eXIf chunk is found by the SAME container walk that reads a camera's PNG.
    const auto read = mosaic::io::extractExif(*bytes);
    REQUIRE(read.has_value());
    CHECK(read->make == "Mosaic");
    CHECK(read->focalLength35mm == 53);

    // ... and the picture is still bit-exact: metadata is a side-car, never a re-encode.
    const auto decoded = mosaic::io::decodeImageBytes(bytes->data(), bytes->size(), &err);
    REQUIRE_MESSAGE(decoded.has_value(), err);
    CHECK(*decoded == src);

    // A PNG written WITHOUT metadata carries none, and is smaller.
    const auto plain = mosaic::io::encodePng(src, {}, &err);
    REQUIRE_MESSAGE(plain.has_value(), err);
    CHECK_FALSE(mosaic::io::extractExif(*plain).has_value());
    CHECK(plain->size() < bytes->size());
}

TEST_CASE("an ICC profile libpng refuses costs the chunk, never the export") {
    // The vendored profile is a CMYK press profile: structurally valid, and exactly the kind
    // libpng rejects for an RGBA PNG. The export must still succeed -- losing a colour profile
    // is a metadata problem, losing the picture would be a bug.
    const std::vector<std::uint8_t> icc =
        mosaic::io::readIccProfile(std::string(MOSAIC_ICC_DIR) + "/ISOcoated_v2_300_eci.icc");
    REQUIRE_FALSE(icc.empty());  // readIccProfile accepted it: it IS a well-formed profile

    const Image src = pattern(6, 4);
    mosaic::io::PngSaveOptions opts;
    opts.metadata.icc = icc;
    std::string err;
    const auto bytes = mosaic::io::encodePng(src, opts, &err);
    REQUIRE_MESSAGE(bytes.has_value(), err);
    const auto decoded = mosaic::io::decodeImageBytes(bytes->data(), bytes->size(), &err);
    REQUIRE_MESSAGE(decoded.has_value(), err);
    CHECK(*decoded == src);
}

TEST_CASE("readIccProfile refuses anything that is not a profile") {
    CHECK(mosaic::io::readIccProfile("").empty());
    CHECK(mosaic::io::readIccProfile("/no/such/profile.icc").empty());
    // A real, readable file that is not an ICC profile (this very source file).
    CHECK(mosaic::io::readIccProfile(__FILE__).empty());
}
