#include "io/exif.hpp"

#include "io/io.hpp"
#include "io/mosaic/docjson.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The EXIF READ slice (io/exif.hpp): the hand-rolled TIFF/IFD parser under a hostile corpus
// (truncations, lying offsets/counts, loops, absurd values -- reject or drop, never crash), the
// eight Orientation bakes, container extraction (JPEG APP1 / PNG eXIf) through the real load
// path, and the .mosaic manifest spellings (docjson exifToJson/exifFromJson). Payloads are built
// programmatically -- a builder beats binary fixtures for making every corruption deliberate.
namespace {

using mosaic::common::ExifData;
using mosaic::common::ExifDateTime;
using mosaic::common::Image;
using mosaic::io::applyExifOrientation;
using mosaic::io::extractExif;
using mosaic::io::loadImage;
using mosaic::io::loadImageWithMetadata;
using mosaic::io::parseExif;

std::string fixture(const char* name) { return std::string(MOSAIC_FIXTURE_DIR) + "/" + name; }

std::vector<std::uint8_t> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// A per-test scratch file under the OS temp dir, removed when the test ends (test_io's pattern).
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const char* name)
        : path(std::filesystem::temp_directory_path() /
               (std::string("mosaic_test_exif_") + name)) {}
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    void write(const std::vector<std::uint8_t>& bytes) const {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        REQUIRE(f.good());
    }
    [[nodiscard]] std::string str() const { return path.string(); }
};

// ---- TIFF/IFD payload builder -------------------------------------------------------------
// Assembles payloads entry by entry in either byte order: header, IFD0, the Exif/GPS sub-IFDs
// (pointer entries added automatically when used), and a trailing data area for values over 4
// bytes. Counts are caller-supplied, NOT derived from the value bytes -- lying is the point.

struct TiffEntry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::vector<std::uint8_t> value;  // encoded bytes; <= 4 inline, else placed in the data area
};

class ExifPayload {
public:
    explicit ExifPayload(bool littleEndian = true) : le(littleEndian) {}

    [[nodiscard]] std::vector<std::uint8_t> u16(std::uint16_t v) const {
        std::vector<std::uint8_t> b;
        put16(b, v);
        return b;
    }
    [[nodiscard]] std::vector<std::uint8_t> u32(std::uint32_t v) const {
        std::vector<std::uint8_t> b;
        put32(b, v);
        return b;
    }
    [[nodiscard]] std::vector<std::uint8_t>
    rationals(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> rs) const {
        std::vector<std::uint8_t> b;
        for (const auto& [num, den] : rs) {
            put32(b, num);
            put32(b, den);
        }
        return b;
    }
    [[nodiscard]] static std::vector<std::uint8_t> ascii(std::string_view s, bool nul = true) {
        std::vector<std::uint8_t> b;
        b.reserve(s.size() + 1);  // reserve-then-fill (GCC's -O3 bounds check dislikes grow-by-one)
        b.insert(b.end(), s.begin(), s.end());
        if (nul)
            b.push_back(0);
        return b;
    }

    ExifPayload& ifd0(std::uint16_t tag, std::uint16_t type, std::uint32_t count,
                      std::vector<std::uint8_t> value) {
        e0.push_back({tag, type, count, std::move(value)});
        return *this;
    }
    ExifPayload& exif(std::uint16_t tag, std::uint16_t type, std::uint32_t count,
                      std::vector<std::uint8_t> value) {
        eExif.push_back({tag, type, count, std::move(value)});
        return *this;
    }
    ExifPayload& gps(std::uint16_t tag, std::uint16_t type, std::uint32_t count,
                     std::vector<std::uint8_t> value) {
        eGps.push_back({tag, type, count, std::move(value)});
        return *this;
    }

    [[nodiscard]] std::vector<std::uint8_t> build() const {
        const auto ifdSize = [](std::size_t n) { return 2 + n * 12 + 4; };
        std::vector<TiffEntry> t0 = e0;
        const std::size_t n0 = t0.size() + (eExif.empty() ? 0 : 1) + (eGps.empty() ? 0 : 1);
        const std::size_t offExif = 8 + ifdSize(n0);
        const std::size_t offGps = offExif + (eExif.empty() ? 0 : ifdSize(eExif.size()));
        std::size_t dataOff = offGps + (eGps.empty() ? 0 : ifdSize(eGps.size()));
        if (!eExif.empty())
            t0.push_back({34665, 4, 1, u32(static_cast<std::uint32_t>(offExif))});
        if (!eGps.empty())
            t0.push_back({34853, 4, 1, u32(static_cast<std::uint32_t>(offGps))});
        std::vector<std::uint8_t> out, data;
        out.push_back(le ? 'I' : 'M');
        out.push_back(le ? 'I' : 'M');
        put16(out, 42);
        put32(out, 8);  // IFD0 straight after the header
        const auto writeIfd = [&](const std::vector<TiffEntry>& es) {
            put16(out, static_cast<std::uint16_t>(es.size()));
            for (const TiffEntry& e : es) {
                put16(out, e.tag);
                put16(out, e.type);
                put32(out, e.count);
                if (e.value.size() <= 4) {
                    out.insert(out.end(), e.value.begin(), e.value.end());
                    out.resize(out.size() + (4 - e.value.size()), 0);
                } else {
                    put32(out, static_cast<std::uint32_t>(dataOff + data.size()));
                    data.insert(data.end(), e.value.begin(), e.value.end());
                }
            }
            put32(out, 0);  // next-IFD pointer: none
        };
        writeIfd(t0);
        if (!eExif.empty())
            writeIfd(eExif);
        if (!eGps.empty())
            writeIfd(eGps);
        out.insert(out.end(), data.begin(), data.end());
        return out;
    }

private:
    void put16(std::vector<std::uint8_t>& b, std::uint16_t v) const {
        if (le) {
            b.push_back(static_cast<std::uint8_t>(v & 0xFF));
            b.push_back(static_cast<std::uint8_t>(v >> 8));
        } else {
            b.push_back(static_cast<std::uint8_t>(v >> 8));
            b.push_back(static_cast<std::uint8_t>(v & 0xFF));
        }
    }
    void put32(std::vector<std::uint8_t>& b, std::uint32_t v) const {
        if (le) {
            for (int i = 0; i < 4; ++i)
                b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        } else {
            for (int i = 3; i >= 0; --i)
                b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }

    bool le;
    std::vector<TiffEntry> e0, eExif, eGps;
};

// Every tag the slice reads, all off their defaults ("W" exercises the negative hemisphere).
ExifPayload fullPayload(bool le) {
    ExifPayload p(le);
    p.ifd0(271, 2, 6, ExifPayload::ascii("Canon"))
        .ifd0(272, 2, 7, ExifPayload::ascii("EOS R5"))
        .ifd0(274, 3, 1, p.u16(6))
        .exif(36867, 2, 20, ExifPayload::ascii("2024:06:01 12:30:45"))
        .exif(37386, 5, 1, p.rationals({{50, 1}}))
        .exif(41989, 3, 1, p.u16(75))
        .gps(1, 2, 2, ExifPayload::ascii("N"))
        .gps(2, 5, 3, p.rationals({{37, 1}, {46, 1}, {30, 1}}))
        .gps(3, 2, 2, ExifPayload::ascii("W"))
        .gps(4, 5, 3, p.rationals({{122, 1}, {25, 1}, {0, 1}}));
    return p;
}

std::optional<ExifData> parse(const std::vector<std::uint8_t>& payload) {
    return parseExif(payload.data(), payload.size());
}

// ---- container splicers ---------------------------------------------------------------------

// Insert an APP1 "Exif\0\0" segment straight after a JPEG's SOI.
std::vector<std::uint8_t> jpegWithExif(const std::vector<std::uint8_t>& jpeg,
                                       const std::vector<std::uint8_t>& tiff) {
    const std::size_t len = 2 + 6 + tiff.size();  // the length field counts itself
    REQUIRE(len <= 0xFFFF);
    std::vector<std::uint8_t> out(jpeg.begin(), jpeg.begin() + 2);
    out.push_back(0xFF);
    out.push_back(0xE1);
    out.push_back(static_cast<std::uint8_t>(len >> 8));
    out.push_back(static_cast<std::uint8_t>(len & 0xFF));
    const char* prefix = "Exif\0\0";
    out.insert(out.end(), prefix, prefix + 6);
    out.insert(out.end(), tiff.begin(), tiff.end());
    out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
    return out;
}

std::uint32_t crc32Of(const std::uint8_t* d, std::size_t n) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

// Insert an eXIf chunk (with a real CRC -- libpng checks) after a PNG's IHDR.
std::vector<std::uint8_t> pngWithExif(const std::vector<std::uint8_t>& png,
                                      const std::vector<std::uint8_t>& tiff) {
    REQUIRE(png.size() >= 33);  // signature + a complete IHDR
    const std::size_t ihdrLen = std::size_t{png[8]} << 24 | std::size_t{png[9]} << 16 |
                                std::size_t{png[10]} << 8 | png[11];
    const std::size_t insertAt = 8 + 8 + ihdrLen + 4;
    std::vector<std::uint8_t> chunk;
    for (int i = 3; i >= 0; --i)
        chunk.push_back(static_cast<std::uint8_t>(tiff.size() >> (8 * i)));
    const char* type = "eXIf";
    chunk.insert(chunk.end(), type, type + 4);
    chunk.insert(chunk.end(), tiff.begin(), tiff.end());
    const std::uint32_t crc = crc32Of(chunk.data() + 4, 4 + tiff.size());
    for (int i = 3; i >= 0; --i)
        chunk.push_back(static_cast<std::uint8_t>(crc >> (8 * i)));
    std::vector<std::uint8_t> out(png.begin(), png.begin() + insertAt);
    out.insert(out.end(), chunk.begin(), chunk.end());
    out.insert(out.end(), png.begin() + insertAt, png.end());
    return out;
}

// A 3x2 image whose pixels are labeled 1..6 in the red channel -- asymmetric on both axes, so
// every one of the eight orientations produces a distinct pixel arrangement.
Image labeledImage() {
    Image img(3, 2);
    for (std::uint32_t i = 0; i < 6; ++i) {
        img.rgba[i * 4 + 0] = static_cast<std::uint8_t>(i + 1);
        img.rgba[i * 4 + 3] = 255;
    }
    return img;
}

std::vector<int> labelsOf(const Image& img) {
    std::vector<int> out;
    for (std::size_t i = 0; i < img.pixelCount(); ++i)
        out.push_back(img.rgba[i * 4]);
    return out;
}

}  // namespace

// ---- the parser: well-formed payloads ---------------------------------------------------------

TEST_CASE("parseExif reads the full tag set in both byte orders") {
    for (const bool le : {true, false}) {
        CAPTURE(le);
        const auto exif = parse(fullPayload(le).build());
        REQUIRE(exif.has_value());
        CHECK(exif->make == "Canon");
        CHECK(exif->model == "EOS R5");
        CHECK(exif->orientation == 6);
        REQUIRE(exif->dateTimeOriginal.has_value());
        CHECK(*exif->dateTimeOriginal == ExifDateTime{2024, 6, 1, 12, 30, 45});
        REQUIRE(exif->focalLengthMm.has_value());
        CHECK(*exif->focalLengthMm == doctest::Approx(50.0));
        CHECK(exif->focalLength35mm == 75);
        REQUIRE(exif->gpsLatitude.has_value());
        CHECK(*exif->gpsLatitude == doctest::Approx(37.775));            // 37 deg 46' 30" N
        REQUIRE(exif->gpsLongitude.has_value());
        CHECK(*exif->gpsLongitude == doctest::Approx(-122.4166666667));  // 122 deg 25' W
    }
}

TEST_CASE("parseExif: fractional rationals, southern hemisphere, sparse tag sets") {
    ExifPayload p;
    p.exif(37386, 5, 1, p.rationals({{2735, 100}}))  // 27.35mm: a real phone-lens spelling
        .gps(1, 2, 2, ExifPayload::ascii("S"))
        .gps(2, 5, 3, p.rationals({{33, 1}, {52, 1}, {120, 10}}));  // 33 deg 52' 12.0" S
    const auto exif = parse(p.build());
    REQUIRE(exif.has_value());
    CHECK(*exif->focalLengthMm == doctest::Approx(27.35));
    CHECK(*exif->gpsLatitude == doctest::Approx(-33.87));
    CHECK_FALSE(exif->gpsLongitude.has_value());  // no longitude tags: absent, not guessed
    CHECK_FALSE(exif->make.has_value());
    CHECK_FALSE(exif->orientation.has_value());
}

TEST_CASE("parseExif: a well-formed payload with none of our tags parses to an empty ExifData") {
    ExifPayload p;
    p.ifd0(305, 2, 9, ExifPayload::ascii("Software"));  // a tag the slice never reads
    const auto exif = parse(p.build());
    REQUIRE(exif.has_value());
    CHECK_FALSE(exif->hasAny());
}

// ---- the parser: hostile corpus ----------------------------------------------------------------

TEST_CASE("parseExif rejects broken headers and impossible IFD offsets") {
    CHECK_FALSE(parse({}).has_value());
    CHECK_FALSE(parse({'I', 'I', 42, 0}).has_value());  // shorter than a TIFF header
    const std::vector<std::uint8_t> good = fullPayload(true).build();

    std::vector<std::uint8_t> badMagic = good;
    badMagic[0] = 'X';
    badMagic[1] = 'X';
    CHECK_FALSE(parse(badMagic).has_value());
    std::vector<std::uint8_t> mixedMagic = good;
    mixedMagic[1] = 'M';  // "IM": neither byte order
    CHECK_FALSE(parse(mixedMagic).has_value());
    std::vector<std::uint8_t> bad42 = good;
    bad42[2] = 43;
    CHECK_FALSE(parse(bad42).has_value());

    for (const std::uint32_t off : {0u, 4u, 0xFFFFFFF0u, static_cast<std::uint32_t>(good.size())}) {
        std::vector<std::uint8_t> badIfd = good;
        std::memcpy(badIfd.data() + 4, &off, 4);  // little-endian build: raw store is fine
        CAPTURE(off);
        CHECK_FALSE(parse(badIfd).has_value());
    }
}

TEST_CASE("parseExif rejects entry tables and value offsets that overrun the payload") {
    // An entry count larger than the cap, and one whose table runs off the end.
    std::vector<std::uint8_t> huge = {'I', 'I', 42, 0, 8, 0, 0, 0, 0xFF, 0xFF};
    CHECK_FALSE(parse(huge).has_value());
    std::vector<std::uint8_t> overrun = {'I', 'I', 42, 0, 8, 0, 0, 0, 12, 0};  // 12 entries, 0 B
    CHECK_FALSE(parse(overrun).has_value());

    // A consumed tag whose declared count reaches past the end of the payload: structural lie.
    ExifPayload lying;
    lying.ifd0(271, 2, 100, ExifPayload::ascii("Canon"));  // 100 declared, 6 present
    CHECK_FALSE(parse(lying.build()).has_value());

    // A rational count that overflows 32-bit byte math if computed narrowly.
    ExifPayload wide;
    wide.exif(37386, 5, 0x40000000u, wide.rationals({{1, 1}}));
    CHECK_FALSE(parse(wide.build()).has_value());

    // Sub-IFD pointers into nowhere: also structural.
    ExifPayload badExifPtr;
    badExifPtr.ifd0(34665, 4, 1, badExifPtr.u32(0xFFFFFF00u));
    CHECK_FALSE(parse(badExifPtr.build()).has_value());
    ExifPayload badGpsPtr;
    badGpsPtr.ifd0(34853, 4, 1, badGpsPtr.u32(3));  // inside the TIFF header
    CHECK_FALSE(parse(badGpsPtr.build()).has_value());
}

TEST_CASE("parseExif cannot be made to loop: sub-IFD pointers back at IFD0 terminate") {
    ExifPayload p;
    p.ifd0(274, 3, 1, p.u16(3)).ifd0(34665, 4, 1, p.u32(8));  // Exif IFD "at" IFD0 itself
    const auto exif = parse(p.build());  // walks IFD0's entries twice, follows nothing further
    REQUIRE(exif.has_value());
    CHECK(exif->orientation == 3);
}

TEST_CASE("parseExif rejects payloads over the size cap") {
    std::vector<std::uint8_t> oversized((std::size_t{1} << 20) + 1, 0);
    oversized[0] = 'I';
    oversized[1] = 'I';
    oversized[2] = 42;
    oversized[6] = 8;  // plausible header; the size alone must reject it
    CHECK_FALSE(parseExif(oversized.data(), oversized.size()).has_value());
}

TEST_CASE("parseExif survives every truncation of a real payload (ASan-checked bounds)") {
    const std::vector<std::uint8_t> full = fullPayload(true).build();
    for (std::size_t n = 0; n < full.size(); ++n) {
        // A fresh exact-size allocation per length, so ASan catches any read past `n`.
        const std::vector<std::uint8_t> t(full.begin(), full.begin() + n);
        (void)parseExif(t.data(), t.size());  // must not crash; reject-or-partial both fine
    }
    CHECK(parse(full).has_value());  // and the untruncated original still parses
}

TEST_CASE("parseExif drops absurd values but keeps the honest fields") {
    ExifPayload p;
    p.ifd0(274, 3, 1, p.u16(9))                            // orientation out of 1..8
        .ifd0(271, 2, 200, ExifPayload::ascii(std::string(199, 'a')))  // over the string cap
        .ifd0(272, 2, 7, ExifPayload::ascii("EOS\x01R5"))  // non-printable byte
        .exif(37386, 5, 1, p.rationals({{50, 0}}))         // the classic zero denominator
        .exif(41989, 3, 1, p.u16(0))                       // 0 = EXIF's "unknown"
        .exif(36867, 2, 20, ExifPayload::ascii("2024:02:30 12:30:45"));  // February 30th
    const auto exif = parse(p.build());
    REQUIRE(exif.has_value());
    CHECK_FALSE(exif->orientation.has_value());
    CHECK_FALSE(exif->make.has_value());
    CHECK_FALSE(exif->model.has_value());
    CHECK_FALSE(exif->focalLengthMm.has_value());
    CHECK_FALSE(exif->focalLength35mm.has_value());
    CHECK_FALSE(exif->dateTimeOriginal.has_value());
    CHECK_FALSE(exif->hasAny());
}

TEST_CASE("parseExif drops wrong-typed and malformed date/orientation spellings") {
    ExifPayload p;
    p.ifd0(274, 4, 1, p.u32(6))                                          // orientation as LONG
        .exif(36867, 2, 19, ExifPayload::ascii("2024-06-01 12:30:45", false))  // wrong separators
        .exif(41989, 3, 1, p.u16(50));                                   // one honest field
    const auto exif = parse(p.build());
    REQUIRE(exif.has_value());
    CHECK_FALSE(exif->orientation.has_value());
    CHECK_FALSE(exif->dateTimeOriginal.has_value());
    CHECK(exif->focalLength35mm == 50);
}

TEST_CASE("parseExif validates GPS: hemispheres, ranges, triplet shape") {
    // Out-of-range latitude (91 deg) and longitude (181 deg): dropped, not clamped.
    ExifPayload outOfRange;
    outOfRange.gps(1, 2, 2, ExifPayload::ascii("N"))
        .gps(2, 5, 3, outOfRange.rationals({{91, 1}, {0, 1}, {0, 1}}))
        .gps(3, 2, 2, ExifPayload::ascii("E"))
        .gps(4, 5, 3, outOfRange.rationals({{181, 1}, {0, 1}, {0, 1}}));
    auto exif = parse(outOfRange.build());
    REQUIRE(exif.has_value());
    CHECK_FALSE(exif->gpsLatitude.has_value());
    CHECK_FALSE(exif->gpsLongitude.has_value());

    // A hemisphere that is not a hemisphere, and a value with a zero-denominator second.
    ExifPayload badRef;
    badRef.gps(1, 2, 2, ExifPayload::ascii("X"))
        .gps(2, 5, 3, badRef.rationals({{10, 1}, {0, 1}, {0, 1}}))
        .gps(3, 2, 2, ExifPayload::ascii("E"))
        .gps(4, 5, 3, badRef.rationals({{10, 1}, {0, 1}, {5, 0}}));
    exif = parse(badRef.build());
    REQUIRE(exif.has_value());
    CHECK_FALSE(exif->gpsLatitude.has_value());
    CHECK_FALSE(exif->gpsLongitude.has_value());

    // A two-rational "triplet", and a value without its ref: both absent.
    ExifPayload shape;
    shape.gps(1, 2, 2, ExifPayload::ascii("N"))
        .gps(2, 5, 2, shape.rationals({{10, 1}, {0, 1}}))
        .gps(4, 5, 3, shape.rationals({{10, 1}, {0, 1}, {0, 1}}));  // no GPSLongitudeRef
    exif = parse(shape.build());
    REQUIRE(exif.has_value());
    CHECK_FALSE(exif->gpsLatitude.has_value());
    CHECK_FALSE(exif->gpsLongitude.has_value());

    // The exact boundaries are valid coordinates.
    ExifPayload poles;
    poles.gps(1, 2, 2, ExifPayload::ascii("S"))
        .gps(2, 5, 3, poles.rationals({{90, 1}, {0, 1}, {0, 1}}))
        .gps(3, 2, 2, ExifPayload::ascii("W"))
        .gps(4, 5, 3, poles.rationals({{180, 1}, {0, 1}, {0, 1}}));
    exif = parse(poles.build());
    REQUIRE(exif.has_value());
    CHECK(*exif->gpsLatitude == doctest::Approx(-90.0));
    CHECK(*exif->gpsLongitude == doctest::Approx(-180.0));
}

// ---- orientation ---------------------------------------------------------------------------

TEST_CASE("applyExifOrientation: all eight transforms, pinned pixel by pixel") {
    // The source is 3x2 labeled 1..6 (see labeledImage); expectations are hand-derived from the
    // EXIF orientation definitions (values 5..8 transpose, swapping the axes).
    const std::vector<std::vector<int>> expected = {
        {1, 2, 3, 4, 5, 6},  // 1: already upright
        {3, 2, 1, 6, 5, 4},  // 2: mirrored horizontally
        {6, 5, 4, 3, 2, 1},  // 3: rotated 180
        {4, 5, 6, 1, 2, 3},  // 4: mirrored vertically
        {1, 4, 2, 5, 3, 6},  // 5: transposed
        {4, 1, 5, 2, 6, 3},  // 6: rotated 90 CW
        {6, 3, 5, 2, 4, 1},  // 7: transverse
        {3, 6, 2, 5, 1, 4},  // 8: rotated 90 CCW
    };
    for (int o = 1; o <= 8; ++o) {
        CAPTURE(o);
        Image img = labeledImage();
        applyExifOrientation(img, o);
        CHECK(img.width == (o >= 5 ? 2u : 3u));
        CHECK(img.height == (o >= 5 ? 3u : 2u));
        CHECK(labelsOf(img) == expected[static_cast<std::size_t>(o - 1)]);
    }
    // Out-of-range orientations leave the image alone.
    for (const int o : {0, 9, -1, 100}) {
        Image img = labeledImage();
        applyExifOrientation(img, o);
        CHECK(img == labeledImage());
    }
}

// ---- containers, through the real load path -----------------------------------------------

TEST_CASE("a JPEG's APP1 EXIF rides loadImageWithMetadata; orientation is baked and reset") {
    const std::vector<std::uint8_t> jpeg = readAll(fixture("sample.jpg"));
    TempFile out("app1.jpg");
    out.write(jpegWithExif(jpeg, fullPayload(true).build()));

    std::string err;
    const auto loaded = loadImageWithMetadata(out.str(), &err);
    REQUIRE_MESSAGE(loaded.has_value(), err);
    CHECK(loaded->image.width == 4);  // 4x4 source; a 90-degree bake keeps it square
    CHECK(loaded->image.height == 4);
    REQUIRE(loaded->exif.has_value());
    CHECK(loaded->exif->orientation == 1);  // 6 in the file; baked, then recorded upright
    CHECK(loaded->exif->make == "Canon");
    CHECK(loaded->exif->model == "EOS R5");
    CHECK(*loaded->exif->focalLengthMm == doctest::Approx(50.0));
    CHECK(loaded->exif->focalLength35mm == 75);
    CHECK(*loaded->exif->dateTimeOriginal == ExifDateTime{2024, 6, 1, 12, 30, 45});
    CHECK(*loaded->exif->gpsLatitude == doctest::Approx(37.775));
    CHECK(*loaded->exif->gpsLongitude == doctest::Approx(-122.4166666667));
}

TEST_CASE("a PNG eXIf orientation loads upright through both load entry points") {
    const Image src = labeledImage();
    std::string err;
    const auto plain = mosaic::io::encodePng(src, {}, &err);
    REQUIRE_MESSAGE(plain.has_value(), err);

    for (int o = 1; o <= 8; ++o) {
        CAPTURE(o);
        ExifPayload p;
        p.ifd0(274, 3, 1, p.u16(static_cast<std::uint16_t>(o)));
        TempFile out("oriented.png");
        out.write(pngWithExif(*plain, p.build()));

        Image expected = src;
        applyExifOrientation(expected, o);  // pinned pixel-exact by the TEST_CASE above

        const auto loaded = loadImageWithMetadata(out.str(), &err);
        REQUIRE_MESSAGE(loaded.has_value(), err);
        CHECK(loaded->image == expected);
        REQUIRE(loaded->exif.has_value());
        CHECK(loaded->exif->orientation == 1);

        // The metadata-free entry point bakes identically: every existing caller loads upright.
        const auto viaLoadImage = loadImage(out.str(), &err);
        REQUIRE_MESSAGE(viaLoadImage.has_value(), err);
        CHECK(*viaLoadImage == expected);
    }
}

TEST_CASE("an image without EXIF: nullopt metadata, byte-identical pixels (zero behavior change)") {
    std::string err;
    for (const char* name : {"sample.jpg", "sample.png"}) {
        CAPTURE(name);
        const auto loaded = loadImageWithMetadata(fixture(name), &err);
        REQUIRE_MESSAGE(loaded.has_value(), err);
        CHECK_FALSE(loaded->exif.has_value());
        const auto plain = loadImage(fixture(name), &err);
        REQUIRE_MESSAGE(plain.has_value(), err);
        CHECK(loaded->image == *plain);
    }
}

TEST_CASE("extractExif: hostile containers are refused, odd-but-legal ones are handled") {
    // A JPEG whose APP1 declares a length past the end of the file.
    CHECK_FALSE(extractExif({0xFF, 0xD8, 0xFF, 0xE1, 0xFF, 0xFF, 'E', 'x'}).has_value());
    // A JPEG that hits SOS before any APP1: the scan stops, it never wades into entropy data.
    CHECK_FALSE(extractExif({0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x04, 0x01, 0x02}).has_value());
    // A PNG chunk length past the end of the file.
    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                                     0xFF, 0xFF, 0xFF, 0x00, 'e', 'X', 'I', 'f'};
    CHECK_FALSE(extractExif(png).has_value());
    // Not a sniffable image at all.
    CHECK_FALSE(extractExif({'h', 'i', '!'}).has_value());
    CHECK_FALSE(extractExif({}).has_value());

    // An APP1 that is XMP (not EXIF) followed by the real EXIF APP1: the scan keeps looking.
    const std::vector<std::uint8_t> jpeg = readAll(fixture("sample.jpg"));
    const std::vector<std::uint8_t> xmpBody = {'h', 't', 't', 'p', ':', '/', '/', 0};
    std::vector<std::uint8_t> doubled(jpeg.begin(), jpeg.begin() + 2);
    doubled.push_back(0xFF);
    doubled.push_back(0xE1);
    doubled.push_back(0);
    doubled.push_back(static_cast<std::uint8_t>(2 + xmpBody.size()));
    doubled.insert(doubled.end(), xmpBody.begin(), xmpBody.end());
    std::vector<std::uint8_t> tail = jpegWithExif(jpeg, fullPayload(false).build());
    doubled.insert(doubled.end(), tail.begin() + 2, tail.end());
    const auto exif = extractExif(doubled);
    REQUIRE(exif.has_value());
    CHECK(exif->make == "Canon");
}

// ---- the .mosaic manifest spellings (docjson) -----------------------------------------------

TEST_CASE("docjson exif: round-trips full and sparse data; rejects malformed nodes") {
    namespace detail = mosaic::io::native::detail;

    ExifData full;
    full.orientation = 1;
    full.focalLengthMm = 27.35;
    full.focalLength35mm = 28;
    full.dateTimeOriginal = ExifDateTime{2024, 2, 29, 23, 59, 59};
    full.gpsLatitude = -33.87;
    full.gpsLongitude = 151.21;
    full.make = "Example";
    full.model = "Camera Mk II";
    CHECK(detail::exifFromJson(detail::exifToJson(full)) == full);

    ExifData sparse;
    sparse.focalLength35mm = 200;
    CHECK(detail::exifFromJson(detail::exifToJson(sparse)) == sparse);

    using nlohmann::json;
    // Present-but-malformed fields reject the node whole (the strict-parse rule); an empty node
    // is not a state the writer emits, so it rejects too.
    CHECK_FALSE(detail::exifFromJson(json::array()).has_value());
    CHECK_FALSE(detail::exifFromJson(json::object()).has_value());
    CHECK_FALSE(detail::exifFromJson(json{{"orientation", 0}}).has_value());
    CHECK_FALSE(detail::exifFromJson(json{{"orientation", 9}}).has_value());
    CHECK_FALSE(detail::exifFromJson(json{{"focal_mm", -1.0}}).has_value());
    CHECK_FALSE(detail::exifFromJson(json{{"focal_mm", 1e9}}).has_value());
    CHECK_FALSE(detail::exifFromJson(json{{"focal_35mm", 0}}).has_value());
    CHECK_FALSE(detail::exifFromJson(json{{"lat", 90.5}}).has_value());
    CHECK_FALSE(detail::exifFromJson(json{{"lon", -180.5}}).has_value());
    CHECK_FALSE(detail::exifFromJson(json{{"taken", 5}}).has_value());
    CHECK_FALSE(detail::exifFromJson(
                    json{{"taken", json{{"y", 2023}, {"mo", 2}, {"d", 29}, {"h", 0}, {"mi", 0},
                                        {"s", 0}}}})  // 2023 is not a leap year
                    .has_value());
    CHECK_FALSE(detail::exifFromJson(json{{"make", std::string(200, 'a')}}).has_value());
    CHECK_FALSE(detail::exifFromJson(json{{"make", "raw\x01" "bytes"}}).has_value());
    CHECK_FALSE(detail::exifFromJson(json{{"model", 42}}).has_value());
}
