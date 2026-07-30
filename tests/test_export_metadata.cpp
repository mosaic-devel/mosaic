#include "common/exif.hpp"
#include "common/image.hpp"
#include "core/color_management.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "io/caps.hpp"
#include "io/document_profile.hpp"
#include "io/exif.hpp"
#include "io/exif_write.hpp"
#include "io/format_backend.hpp"
#include "io/format_registry.hpp"
#include "io/io.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Metadata + ICC, end to end (docs/export-system-plan.md §7d): the slice that made the Export
// modal's Metadata toggle and Colour row TRUE. Three things are proven here, and they are the three
// that go wrong silently:
//
//   1. WHICH metadata an export writes. EXIF lives per layer and an export is a flatten, so the
//      provenance rule (io::documentExif) is a decision, not a fact -- and a decision nobody
//      tested is a decision that drifts.
//   2. That every container really carries it. Not "the flag was set": the bytes are searched.
//      A JPEG in particular has to split an ICC profile across numbered APP2 segments, and writing
//      one oversized segment instead fails silently -- the file simply has a profile no
//      colour-managed reader will use.
//   3. That STRIP means strip. The toggle's whole value is that a file can be shipped without
//      saying where it was taken, so "no EXIF marker anywhere in the bytes" is the only assertion
//      worth making.
//
// ⚠ NOTE for anyone adding a round-trip case here: doctest's Approx compares
// `fabs(diff) < epsilon * scale`, so `Approx(x).epsilon(0.0)` is a strict `< 0` that NO pair of
// values satisfies -- it fails against an identical double. Exact round-trips use plain `==`.
using namespace mosaic;

namespace {

// ---- byte-level oracles (deliberately independent of the encoders' own writers) ---------------

[[nodiscard]] bool contains(const std::vector<std::uint8_t>& haystack,
                            const std::vector<std::uint8_t>& needle) {
    if (needle.empty() || needle.size() > haystack.size())
        return false;
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) !=
           haystack.end();
}

[[nodiscard]] bool contains(const std::vector<std::uint8_t>& haystack, std::string_view needle) {
    return contains(haystack, std::vector<std::uint8_t>(needle.begin(), needle.end()));
}

// One APPn/marker segment of a JPEG: its marker byte and its payload (the length word stripped).
struct JpegSegment {
    std::uint8_t marker = 0;
    std::vector<std::uint8_t> payload;
};

// Walk a JPEG's marker segments up to the scan. A second, independent implementation of the walk
// io/jpeg.cpp's splicer writes into -- if the two ever disagree about framing, this fails.
[[nodiscard]] std::vector<JpegSegment> jpegSegments(const std::vector<std::uint8_t>& file) {
    std::vector<JpegSegment> out;
    if (file.size() < 4 || file[0] != 0xFF || file[1] != 0xD8)
        return out;
    std::size_t at = 2;
    while (at + 4 <= file.size()) {
        if (file[at] != 0xFF)
            break;
        const std::uint8_t marker = file[at + 1];
        if (marker == 0xFF) { // fill byte before a marker: legal padding
            ++at;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) { // standalone, no length word
            at += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9)
            break; // SOS / EOI: entropy-coded data follows, and no metadata may live past here
        const std::size_t length = (static_cast<std::size_t>(file[at + 2]) << 8) |
                                   static_cast<std::size_t>(file[at + 3]);
        if (length < 2 || at + 2 + length > file.size())
            break;
        JpegSegment seg;
        seg.marker = marker;
        seg.payload.assign(file.begin() + static_cast<std::ptrdiff_t>(at + 4),
                           file.begin() + static_cast<std::ptrdiff_t>(at + 2 + length));
        out.push_back(std::move(seg));
        at += 2 + length;
    }
    return out;
}

// Reassemble the ICC profile out of a JPEG's APP2 sequence, exactly as a colour-managed reader
// would: the 12-byte "ICC_PROFILE\0" identifier, a 1-based chunk number, the total count, then the
// slice. Returns empty if any of that does not hold -- including a count/index mismatch, which is
// the failure a single oversized segment would produce.
[[nodiscard]] std::vector<std::uint8_t> jpegIccProfile(const std::vector<std::uint8_t>& file) {
    static constexpr char kIdent[] = "ICC_PROFILE";
    static constexpr std::size_t kPrefix = sizeof kIdent + 2; // 12 + chunk + count
    std::vector<std::vector<std::uint8_t>> chunks;
    std::size_t declaredCount = 0;
    for (const JpegSegment& seg : jpegSegments(file)) {
        if (seg.marker != 0xE2 || seg.payload.size() <= kPrefix)
            continue;
        if (std::memcmp(seg.payload.data(), kIdent, sizeof kIdent) != 0)
            continue;
        const std::size_t index = seg.payload[sizeof kIdent];
        const std::size_t count = seg.payload[sizeof kIdent + 1];
        if (index == 0 || count == 0 || index > count)
            return {};
        if (declaredCount == 0)
            declaredCount = count;
        else if (declaredCount != count)
            return {}; // the segments disagree about how many there are
        if (chunks.size() < count)
            chunks.resize(count);
        if (!chunks[index - 1].empty())
            return {}; // the same chunk number twice
        chunks[index - 1].assign(seg.payload.begin() + static_cast<std::ptrdiff_t>(kPrefix),
                                 seg.payload.end());
    }
    if (declaredCount == 0)
        return {};
    std::vector<std::uint8_t> out;
    for (const std::vector<std::uint8_t>& chunk : chunks) {
        if (chunk.empty())
            return {}; // a hole in the sequence: a reader would refuse the profile
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return out;
}

// ---- fixtures --------------------------------------------------------------------------------

common::Image pattern(std::uint32_t w, std::uint32_t h) {
    common::Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            img.rgba[p + 0] = static_cast<std::uint8_t>(x * 29 + 7);
            img.rgba[p + 1] = static_cast<std::uint8_t>(y * 41 + 13);
            img.rgba[p + 2] = static_cast<std::uint8_t>((x ^ y) * 19);
            img.rgba[p + 3] = 255; // opaque: JPEG and GIF must not be judged on their matte here
        }
    return img;
}

common::ExifData camera() {
    common::ExifData d;
    d.orientation = 1;
    d.focalLengthMm = 24.0;
    d.focalLength35mm = 36;
    d.dateTimeOriginal = common::ExifDateTime{2026, 7, 29, 9, 41, 2};
    d.gpsLatitude = 48.858;
    d.gpsLongitude = 2.294;
    d.make = "Mosaic";
    d.model = "Provenance Rig";
    return d;
}

// The vendored press profile: 1.8 MB, i.e. far more than one JPEG APP2 segment can hold. That is
// exactly why it is the fixture for the multi-segment case.
std::vector<std::uint8_t> bigProfile() {
    return io::readIccProfile(std::string(MOSAIC_ICC_DIR) + "/ISOcoated_v2_300_eci.icc");
}

// A small, structurally valid RGB profile, built rather than shipped.
std::vector<std::uint8_t> smallProfile() {
    return core::workingSpaceIccProfile(core::ColorSpace::DisplayP3);
}

core::Document makeDoc(core::ColorSpace cs = core::ColorSpace::SRGB) {
    return core::Document(16, 12, cs, core::Precision::U8);
}

std::unique_ptr<core::RasterLayer> photoLayer(core::Document& doc, const char* name,
                                              const std::optional<common::ExifData>& exif) {
    std::unique_ptr<core::RasterLayer> layer = doc.makeRaster(name);
    layer->image().fill(common::Color8{20, 30, 40, 255});
    layer->setExif(exif);
    return layer;
}

// Encode through the REGISTRY, which is the path the modal takes: RenderInput in, bytes out.
[[nodiscard]] io::EncodeResult encodeVia(io::FormatId id, const common::Image& pixels,
                                         const std::optional<common::ExifData>& exif,
                                         const std::vector<std::uint8_t>& icc, bool strip,
                                         double dpi = 72.0) {
    const io::FormatBackend* backend = io::FormatRegistry::instance().find(id);
    REQUIRE(backend != nullptr);
    io::RenderInput input;
    input.pixels = &pixels;
    input.dpi = dpi;
    input.exif = exif.has_value() ? &*exif : nullptr;
    input.iccProfile = icc;
    input.stripMetadata = strip;
    io::OptionValues values = backend->optionsSchema().defaults();
    return backend->encode(input, values);
}

} // namespace

// ---- 1. the orientation rule ------------------------------------------------------------------

TEST_CASE("an export always writes orientation 1, whatever the record says") {
    // The bug this exists to prevent shows up only in SOMEBODY ELSE'S viewer: the load baked the
    // rotation into the pixels, so writing the original 6 back out asks every reader to rotate an
    // already-upright picture a second time.
    for (const int shot : {2, 3, 4, 5, 6, 7, 8}) {
        common::ExifData d = camera();
        d.orientation = shot;
        const common::ExifData out = io::exifForExport(d);
        REQUIRE(out.orientation.has_value());
        CHECK(*out.orientation == 1);
        // Nothing else is touched.
        CHECK(out.make == d.make);
        CHECK(out.focalLength35mm == d.focalLength35mm);
        CHECK(out.dateTimeOriginal == d.dateTimeOriginal);
    }

    // Absent stays absent: an export must not invent metadata, and absent already means 1.
    common::ExifData none = camera();
    none.orientation.reset();
    CHECK_FALSE(io::exifForExport(none).orientation.has_value());
}

TEST_CASE("orientation 1 survives all the way into a real container") {
    common::ExifData sideways = camera();
    sideways.orientation = 6; // 90 degrees clockwise -- the commonest phone-portrait value

    io::PngSaveOptions opts;
    opts.metadata.exif = io::buildExifPayload(io::exifForExport(sideways));
    REQUIRE_FALSE(opts.metadata.exif.empty());

    std::string err;
    const auto bytes = io::encodePng(pattern(8, 6), opts, &err);
    REQUIRE_MESSAGE(bytes.has_value(), err);
    const auto read = io::extractExif(*bytes);
    REQUIRE(read.has_value());
    REQUIRE(read->orientation.has_value());
    CHECK(*read->orientation == 1);
}

// ---- 2. the provenance rule --------------------------------------------------------------------

TEST_CASE("a document with no camera metadata exports none") {
    core::Document doc = makeDoc();
    doc.root().addOnTop(photoLayer(doc, "empty", std::nullopt));
    CHECK_FALSE(io::documentExif(doc).has_value());

    // A record with every field absent is not metadata either (ExifData::hasAny()).
    core::Document blank = makeDoc();
    blank.root().addOnTop(photoLayer(blank, "blank", common::ExifData{}));
    CHECK_FALSE(io::documentExif(blank).has_value());
}

TEST_CASE("the provenance layer is the earliest-minted one, not the bottom-most") {
    core::Document doc = makeDoc();

    common::ExifData first = camera();
    first.model = "Opened From";
    common::ExifData later = camera();
    later.model = "Placed Later";

    // Minted in this order, then stacked in the OPPOSITE order -- which is what "Open as layer"
    // above the active layer, or any drag in the layer dock, produces.
    std::unique_ptr<core::RasterLayer> opened = photoLayer(doc, "opened", first);
    std::unique_ptr<core::RasterLayer> placed = photoLayer(doc, "placed", later);
    doc.root().addOnTop(std::move(placed));
    doc.root().addOnTop(std::move(opened));

    const std::optional<common::ExifData> got = io::documentExif(doc);
    REQUIRE(got.has_value());
    CHECK(got->model == "Opened From");
}

TEST_CASE("only an effectively-visible raster or magic layer can be the provenance") {
    common::ExifData hiddenData = camera();
    hiddenData.model = "Hidden";
    common::ExifData shownData = camera();
    shownData.model = "Shown";

    SUBCASE("a hidden layer is skipped even though it was minted first") {
        core::Document doc = makeDoc();
        std::unique_ptr<core::RasterLayer> hidden = photoLayer(doc, "hidden", hiddenData);
        hidden->setVisible(false);
        doc.root().addOnTop(std::move(hidden));
        doc.root().addOnTop(photoLayer(doc, "shown", shownData));

        const std::optional<common::ExifData> got = io::documentExif(doc);
        REQUIRE(got.has_value());
        CHECK(got->model == "Shown");
    }

    SUBCASE("a hidden GROUP hides its children's metadata too") {
        core::Document doc = makeDoc();
        std::unique_ptr<core::GroupLayer> group = doc.makeGroup("folded away");
        group->addOnTop(photoLayer(doc, "inside", hiddenData));
        group->setVisible(false);
        doc.root().addOnTop(std::move(group));
        doc.root().addOnTop(photoLayer(doc, "shown", shownData));

        const std::optional<common::ExifData> got = io::documentExif(doc);
        REQUIRE(got.has_value());
        CHECK(got->model == "Shown");
    }

    SUBCASE("a VISIBLE group's children are candidates, at their own ids") {
        core::Document doc = makeDoc();
        common::ExifData innerData = camera();
        innerData.model = "Inner";
        common::ExifData outerData = camera();
        outerData.model = "Outer";

        std::unique_ptr<core::GroupLayer> group = doc.makeGroup("open");
        group->addOnTop(photoLayer(doc, "inside", innerData)); // minted before the outer one
        doc.root().addOnTop(std::move(group));
        doc.root().addOnTop(photoLayer(doc, "outside", outerData));

        const std::optional<common::ExifData> got = io::documentExif(doc);
        REQUIRE(got.has_value());
        CHECK(got->model == "Inner"); // nesting does not exempt a layer from the id rule
    }

    SUBCASE("a magic layer counts -- File > Open as Layer stamps one") {
        core::Document doc = makeDoc();
        std::unique_ptr<core::MagicLayer> magic = doc.makeMagic("placed", pattern(4, 4));
        common::ExifData d = camera();
        d.model = "Magic";
        magic->setExif(d);
        doc.root().addOnTop(std::move(magic));

        const std::optional<common::ExifData> got = io::documentExif(doc);
        REQUIRE(got.has_value());
        CHECK(got->model == "Magic");
    }
}

TEST_CASE("documentExif normalises the orientation it hands out") {
    core::Document doc = makeDoc();
    common::ExifData sideways = camera();
    sideways.orientation = 8;
    doc.root().addOnTop(photoLayer(doc, "sideways", sideways));

    const std::optional<common::ExifData> got = io::documentExif(doc);
    REQUIRE(got.has_value());
    REQUIRE(got->orientation.has_value());
    CHECK(*got->orientation == 1);
}

// ---- 3. the document's colour profile ---------------------------------------------------------

TEST_CASE("a plain sRGB document embeds nothing; a wide-gamut one embeds its own space") {
    // sRGB is what an untagged file is read as everywhere, so tagging it is cost with no benefit.
    CHECK(core::documentIccProfile(makeDoc(core::ColorSpace::SRGB)).empty());

    for (const core::ColorSpace cs : {core::ColorSpace::DisplayP3, core::ColorSpace::AdobeRGB,
                                      core::ColorSpace::Rec2020}) {
        const std::vector<std::uint8_t> icc = core::documentIccProfile(makeDoc(cs));
        REQUIRE(icc.size() > std::size_t{128});
        // The same two structural facts io::readIccProfile insists on: the header's big-endian
        // size field matches the file, and 'acsp' sits at offset 36.
        const std::uint32_t declared = (static_cast<std::uint32_t>(icc[0]) << 24) |
                                       (static_cast<std::uint32_t>(icc[1]) << 16) |
                                       (static_cast<std::uint32_t>(icc[2]) << 8) | icc[3];
        CHECK(declared == icc.size());
        CHECK(std::memcmp(icc.data() + 36, "acsp", 4) == 0);
    }
}

// ---- 4. per-container round trips ---------------------------------------------------------------

TEST_CASE("PNG carries EXIF and a profile, and the pixels stay bit-exact") {
    const common::Image src = pattern(12, 9);
    const std::vector<std::uint8_t> icc = smallProfile();
    REQUIRE_FALSE(icc.empty());

    const io::EncodeResult r = encodeVia(io::FormatId::Png, src, camera(), icc, /*strip=*/false,
                                        /*dpi=*/300.0);
    REQUIRE_MESSAGE(r.ok, r.error);

    const auto read = io::extractExif(r.bytes);
    REQUIRE(read.has_value());
    CHECK(read->make == "Mosaic");
    CHECK(read->model == "Provenance Rig");
    CHECK(read->focalLength35mm == 36);
    // The profile is present as an iCCP chunk. Only the TAG is searched for, not the bytes: libpng
    // DEFLATES an iCCP payload, so the profile does not appear verbatim in a PNG. The byte-exact
    // profile round trip is the JPEG case below, where the segments are stored raw.
    CHECK(contains(r.bytes, "iCCP"));
    CHECK(contains(r.bytes, "eXIf"));
    CHECK(contains(r.bytes, "pHYs")); // 300 dpi is a real claim about print size

    std::string err;
    const auto decoded = io::decodeImageBytes(r.bytes.data(), r.bytes.size(), &err);
    REQUIRE_MESSAGE(decoded.has_value(), err);
    CHECK(*decoded == src); // metadata is a side-car, never a re-encode
}

TEST_CASE("JPEG writes an APP1 Exif segment our own reader finds") {
    const io::EncodeResult r =
        encodeVia(io::FormatId::Jpeg, pattern(24, 16), camera(), {}, /*strip=*/false);
    REQUIRE_MESSAGE(r.ok, r.error);

    const auto read = io::extractExif(r.bytes);
    REQUIRE(read.has_value());
    CHECK(read->make == "Mosaic");
    REQUIRE(read->gpsLatitude.has_value());
    CHECK(*read->gpsLatitude == doctest::Approx(48.858).epsilon(1e-6));

    // The segment sits before the scan, where every reader looks, and carries the JPEG-only
    // "Exif\0\0" prefix (which is the container's business, not the payload's).
    bool found = false;
    for (const JpegSegment& seg : jpegSegments(r.bytes)) {
        if (seg.marker != 0xE1 || seg.payload.size() < 6)
            continue;
        if (std::memcmp(seg.payload.data(), "Exif\0\0", 6) == 0)
            found = true;
    }
    CHECK(found);
}

TEST_CASE("a JPEG splits an over-long ICC profile across numbered APP2 segments") {
    const std::vector<std::uint8_t> icc = bigProfile();
    REQUIRE_FALSE(icc.empty());
    REQUIRE(icc.size() > std::size_t{65533}); // the whole point: it cannot fit one segment

    const io::EncodeResult r =
        encodeVia(io::FormatId::Jpeg, pattern(20, 20), std::nullopt, icc, /*strip=*/false);
    REQUIRE_MESSAGE(r.ok, r.error);

    // Every segment obeys the 16-bit length ceiling. A single oversized segment is the classic
    // bug, and it does not fail loudly -- it just produces an unusable profile.
    int app2 = 0;
    for (const JpegSegment& seg : jpegSegments(r.bytes)) {
        if (seg.marker != 0xE2)
            continue;
        ++app2;
        CHECK(seg.payload.size() + 2 <= std::size_t{65535});
    }
    CHECK(app2 > 1);

    // Reassembled the way a colour-managed reader does it, the profile is byte-identical.
    CHECK(jpegIccProfile(r.bytes) == icc);
}

TEST_CASE("a JPEG profile that fits one segment is still a well-formed sequence of one") {
    const std::vector<std::uint8_t> icc = smallProfile();
    REQUIRE_FALSE(icc.empty());
    REQUIRE(icc.size() < std::size_t{65519});

    const io::EncodeResult r =
        encodeVia(io::FormatId::Jpeg, pattern(16, 16), std::nullopt, icc, /*strip=*/false);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(jpegIccProfile(r.bytes) == icc);
}

TEST_CASE("a JPEG records the print resolution in its JFIF header") {
    const io::EncodeResult r = encodeVia(io::FormatId::Jpeg, pattern(16, 16), std::nullopt, {},
                                        /*strip=*/false, /*dpi=*/300.0);
    REQUIRE_MESSAGE(r.ok, r.error);

    bool found = false;
    for (const JpegSegment& seg : jpegSegments(r.bytes)) {
        if (seg.marker != 0xE0 || seg.payload.size() < 14)
            continue;
        if (std::memcmp(seg.payload.data(), "JFIF\0", 5) != 0)
            continue;
        // JFIF layout: 5-byte identifier, 2 version bytes, the units byte, then X and Y density
        // as big-endian 16-bit words. Units 1 == pixels per inch.
        CHECK(seg.payload[7] == 1);
        const int xd = (static_cast<int>(seg.payload[8]) << 8) | seg.payload[9];
        const int yd = (static_cast<int>(seg.payload[10]) << 8) | seg.payload[11];
        CHECK(xd == 300);
        CHECK(yd == 300);
        found = true;
    }
    CHECK(found);

    // 72 dpi means "no opinion" and must NOT be written as a claim about print size.
    const io::EncodeResult plain = encodeVia(io::FormatId::Jpeg, pattern(16, 16), std::nullopt, {},
                                            /*strip=*/false, /*dpi=*/72.0);
    REQUIRE_MESSAGE(plain.ok, plain.error);
    for (const JpegSegment& seg : jpegSegments(plain.bytes)) {
        if (seg.marker == 0xE0 && seg.payload.size() >= 14 &&
            std::memcmp(seg.payload.data(), "JFIF\0", 5) == 0)
            CHECK(seg.payload[7] == 0); // units: unknown
    }
}

TEST_CASE("WebP muxes the EXIF and ICCP chunks when the library is present") {
    if (!io::webpSupported())
        return; // an optional build dependency; the format is simply not offered without it
    const std::vector<std::uint8_t> icc = smallProfile();
    REQUIRE_FALSE(icc.empty());

    const io::EncodeResult r =
        encodeVia(io::FormatId::WebP, pattern(24, 18), camera(), icc, /*strip=*/false);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(contains(r.bytes, "EXIF")); // the extended-container chunk tags
    CHECK(contains(r.bytes, "ICCP"));
    CHECK(contains(r.bytes, icc));
    CHECK(contains(r.bytes, io::buildExifPayload(io::exifForExport(camera()))));
}

TEST_CASE("TIFF writes the ICC profile tag, and honestly claims no EXIF") {
    if (!io::tiffSupported())
        return;
    const std::vector<std::uint8_t> icc = smallProfile();
    REQUIRE_FALSE(icc.empty());

    const io::EncodeResult r =
        encodeVia(io::FormatId::Tiff, pattern(20, 15), camera(), icc, /*strip=*/false);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(contains(r.bytes, icc)); // TIFFTAG_ICCPROFILE stores it uncompressed

    // The caps row says no EXIF, and the file agrees -- a TIFF needs a private sub-directory and a
    // back-patched offset for that, which io/io.hpp records as still owed. The claim and the bytes
    // must not drift apart in EITHER direction, so this is asserted, not assumed.
    const io::FormatBackend* tiff = io::FormatRegistry::instance().find(io::FormatId::Tiff);
    REQUIRE(tiff != nullptr);
    CHECK_FALSE(io::has(tiff->caps().metadata, io::MetadataKind::Exif));
    CHECK_FALSE(contains(r.bytes, io::buildExifPayload(io::exifForExport(camera()))));
}

TEST_CASE("AVIF carries the profile and the Exif item when an encoder is available") {
    if (!io::avifSupported())
        return; // no libaom / SVT-AV1: the format is not offered at all (the never-rav1e rule)
    const std::vector<std::uint8_t> icc = smallProfile();
    REQUIRE_FALSE(icc.empty());

    const io::EncodeResult r =
        encodeVia(io::FormatId::Avif, pattern(32, 24), camera(), icc, /*strip=*/false);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(contains(r.bytes, icc));
    CHECK(contains(r.bytes, io::buildExifPayload(io::exifForExport(camera()))));
}

TEST_CASE("JXL carries the profile and an Exif box when libjxl is present") {
    if (!io::jxlSupported())
        return;
    const common::Image src = pattern(32, 24);
    const std::vector<std::uint8_t> icc = smallProfile();
    REQUIRE_FALSE(icc.empty());

    const io::EncodeResult withMeta =
        encodeVia(io::FormatId::Jxl, src, camera(), icc, /*strip=*/false);
    REQUIRE_MESSAGE(withMeta.ok, withMeta.error);
    // The Exif box's type is four ASCII bytes in the container, and its contents are the raw
    // payload behind the four-byte TIFF-header offset JXL alone requires.
    CHECK(contains(withMeta.bytes, "Exif"));
    CHECK(contains(withMeta.bytes, io::buildExifPayload(io::exifForExport(camera()))));

    // A metadata-free encode of the same pixels stays a bare codestream: no box, no profile.
    const io::EncodeResult plain = encodeVia(io::FormatId::Jxl, src, std::nullopt, {},
                                            /*strip=*/false);
    REQUIRE_MESSAGE(plain.ok, plain.error);
    CHECK_FALSE(contains(plain.bytes, "Exif"));
    CHECK(plain.bytes.size() < withMeta.bytes.size());
}

// ---- 5. strip means strip ----------------------------------------------------------------------

TEST_CASE("the Metadata toggle off leaves no marker anywhere in the bytes") {
    const common::Image src = pattern(24, 18);
    const std::vector<std::uint8_t> icc = smallProfile();
    const std::vector<std::uint8_t> exifPayload = io::buildExifPayload(io::exifForExport(camera()));
    REQUIRE_FALSE(icc.empty());
    REQUIRE_FALSE(exifPayload.empty());

    // Every registered, AVAILABLE backend, handed a full payload and told to strip. The assertion
    // searches the encoded bytes rather than trusting the flag: that is the only way to catch a
    // backend that reads input.exif directly and forgets buildMetadata's single gate.
    for (const io::FormatBackend* backend : io::FormatRegistry::instance().all()) {
        if (!backend->available())
            continue;
        const std::string name(io::formatIdName(backend->id()));
        const io::EncodeResult r =
            encodeVia(backend->id(), src, camera(), icc, /*strip=*/true, /*dpi=*/300.0);
        // Parenthesised: doctest's *_MESSAGE macros expand to `MessageBuilder * msg`, and `*` binds
        // tighter than `+`, so an unbracketed concatenation is parsed as (MB * name) + ": ".
        REQUIRE_MESSAGE(r.ok, (name + ": " + r.error));
        CHECK_MESSAGE(!contains(r.bytes, exifPayload), (name + " kept the EXIF payload"));
        CHECK_MESSAGE(!contains(r.bytes, icc), (name + " kept the ICC profile"));
        CHECK_MESSAGE(!io::extractExif(r.bytes).has_value(), (name + " kept readable EXIF"));
    }
}

TEST_CASE("a stripped PNG and JPEG carry none of the chunk or segment tags either") {
    const common::Image src = pattern(16, 16);
    const std::vector<std::uint8_t> icc = smallProfile();

    const io::EncodeResult png =
        encodeVia(io::FormatId::Png, src, camera(), icc, /*strip=*/true, /*dpi=*/300.0);
    REQUIRE_MESSAGE(png.ok, png.error);
    CHECK_FALSE(contains(png.bytes, "eXIf"));
    CHECK_FALSE(contains(png.bytes, "iCCP"));
    CHECK_FALSE(contains(png.bytes, "pHYs")); // the density is metadata too

    const io::EncodeResult jpeg =
        encodeVia(io::FormatId::Jpeg, src, camera(), icc, /*strip=*/true, /*dpi=*/300.0);
    REQUIRE_MESSAGE(jpeg.ok, jpeg.error);
    CHECK_FALSE(contains(jpeg.bytes, "ICC_PROFILE"));
    for (const JpegSegment& seg : jpegSegments(jpeg.bytes)) {
        CHECK(seg.marker != 0xE1); // no APP1 at all
        CHECK(seg.marker != 0xE2); // no APP2 at all
    }
}

// ---- 6. the loss diff --------------------------------------------------------------------------

namespace {

[[nodiscard]] bool warns(const std::vector<io::LossWarning>& warnings, io::LossCode code) {
    return std::any_of(warnings.begin(), warnings.end(),
                       [code](const io::LossWarning& w) { return w.code == code; });
}

[[nodiscard]] io::FormatCaps capsOf(io::FormatId id) {
    const io::FormatBackend* backend = io::FormatRegistry::instance().find(id);
    REQUIRE(backend != nullptr);
    return backend->caps();
}

} // namespace

TEST_CASE("the metadata warning fires for a format that cannot carry it, and not for one that can") {
    io::DocumentProfile doc;
    doc.hasEXIF = true;
    doc.hasICC = true;
    doc.dpi = 300.0;

    // GIF: no profile, no EXIF, no density record. Three amber warnings, all earned.
    const std::vector<io::LossWarning> gif = io::diff(doc, capsOf(io::FormatId::Gif), {});
    CHECK(warns(gif, io::LossCode::ExifDropped));
    CHECK(warns(gif, io::LossCode::IccDropped));
    CHECK(warns(gif, io::LossCode::DpiDropped));

    // PNG writes all three, so it must warn about none of them.
    const std::vector<io::LossWarning> png = io::diff(doc, capsOf(io::FormatId::Png), {});
    CHECK_FALSE(warns(png, io::LossCode::ExifDropped));
    CHECK_FALSE(warns(png, io::LossCode::IccDropped));
    CHECK_FALSE(warns(png, io::LossCode::DpiDropped));

    // JPEG earned all three in M5 as well (APP1 + APP2 + the JFIF density).
    const std::vector<io::LossWarning> jpeg = io::diff(doc, capsOf(io::FormatId::Jpeg), {});
    CHECK_FALSE(warns(jpeg, io::LossCode::ExifDropped));
    CHECK_FALSE(warns(jpeg, io::LossCode::IccDropped));
    CHECK_FALSE(warns(jpeg, io::LossCode::DpiDropped));

    // TIFF: profile and density yes, EXIF no -- the one row where the answer is split.
    const std::vector<io::LossWarning> tiff = io::diff(doc, capsOf(io::FormatId::Tiff), {});
    CHECK(warns(tiff, io::LossCode::ExifDropped));
    CHECK_FALSE(warns(tiff, io::LossCode::IccDropped));
    CHECK_FALSE(warns(tiff, io::LossCode::DpiDropped));
}

TEST_CASE("what the user asked to leave out is never reported as a loss") {
    io::DocumentProfile doc;
    doc.hasEXIF = true;
    doc.hasICC = true;
    doc.hasNonSrgbSpace = true;
    doc.dpi = 300.0;
    const io::FormatCaps gif = capsOf(io::FormatId::Gif);

    // The default request is "carry everything", which is what every pre-M5 caller meant.
    CHECK(io::MetadataRequest{}.keepMetadata);
    CHECK(io::MetadataRequest{}.embedIcc);

    io::MetadataRequest stripped;
    stripped.keepMetadata = false;
    stripped.embedIcc = false;
    const std::vector<io::LossWarning> quiet = io::diff(doc, gif, {}, stripped);
    CHECK_FALSE(warns(quiet, io::LossCode::ExifDropped));
    CHECK_FALSE(warns(quiet, io::LossCode::DpiDropped));
    CHECK_FALSE(warns(quiet, io::LossCode::IccDropped));
    CHECK_FALSE(warns(quiet, io::LossCode::ColorSpaceConverted));

    // Each half gates only its own group.
    io::MetadataRequest noIcc;
    noIcc.embedIcc = false;
    const std::vector<io::LossWarning> exifOnly = io::diff(doc, gif, {}, noIcc);
    CHECK(warns(exifOnly, io::LossCode::ExifDropped));
    CHECK_FALSE(warns(exifOnly, io::LossCode::IccDropped));

    io::MetadataRequest noExif;
    noExif.keepMetadata = false;
    const std::vector<io::LossWarning> iccOnly = io::diff(doc, gif, {}, noExif);
    CHECK_FALSE(warns(iccOnly, io::LossCode::ExifDropped));
    CHECK(warns(iccOnly, io::LossCode::IccDropped));

    // And a request can only ever REMOVE warnings: the picture-level ones are untouched.
    io::DocumentProfile alpha;
    alpha.hasAlpha = true;
    CHECK(warns(io::diff(alpha, gif, {}, stripped), io::LossCode::AlphaReducedToBinary));
}

TEST_CASE("GIF cannot carry a profile, and says so instead of silently dropping it") {
    if (!io::gifSupported())
        return;
    const std::vector<std::uint8_t> icc = smallProfile();
    REQUIRE_FALSE(icc.empty());

    // The encode still succeeds -- an unembeddable profile must never fail an export -- but the
    // bytes carry nothing of it, and the loss diff is what tells the user.
    const io::EncodeResult r =
        encodeVia(io::FormatId::Gif, pattern(16, 16), camera(), icc, /*strip=*/false);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK_FALSE(contains(r.bytes, icc));

    io::DocumentProfile doc;
    doc.hasICC = true;
    CHECK(warns(io::diff(doc, capsOf(io::FormatId::Gif), {}), io::LossCode::IccDropped));
}
