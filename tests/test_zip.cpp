// The read-only ZIP walker (io/brush/zip.{hpp,cpp}) -- the container under `.bundle`.
//
// Every archive here is BUILT, byte by byte, by the test's own writer: each field is under the
// test's control, so each hostile shape below corrupts exactly one thing and the assertion says
// which rule caught it. The two real-bundle quirks (data-descriptor entries with zeroed local
// sizes; a local extra field the central directory does not have) get dedicated cases because
// the shipped bundles depend on them (RGBA_brushes.bundle sets the descriptor flag on EVERY
// entry).

#include "io/brush/zip.hpp"

#include "zip_builder.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using mosaic::io::brush::ZipEntry;
using mosaic::io::brush::ZipReader;
using ziptest::buildZip;
using ziptest::bytesOf;
using ziptest::eocdAt;
using ziptest::TestEntry;

TEST_CASE("zip: stored and deflated entries round-trip, byte-exact") {
    const std::vector<std::uint8_t> text = bytesOf("application/x-krita-resourcebundle");
    std::vector<std::uint8_t> big(100000);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<std::uint8_t>((i * 31) & 0xFF);

    const auto zip = buildZip({{"mimetype", text, /*deflate=*/false},
                               {"brushes/tip.gbr", big, /*deflate=*/true}});
    std::string error;
    auto reader = ZipReader::open(zip.data(), zip.size(), &error);
    REQUIRE_MESSAGE(reader.has_value(), error);
    CHECK(reader->entries().size() == 2);
    CHECK(reader->skippedEntries() == 0);

    const ZipEntry* mime = reader->find("mimetype");
    REQUIRE(mime != nullptr);
    CHECK(mime->method == 0);
    auto mimeBytes = reader->read(*mime, &error);
    REQUIRE_MESSAGE(mimeBytes.has_value(), error);
    CHECK(*mimeBytes == text);

    const ZipEntry* tip = reader->find("brushes/tip.gbr");
    REQUIRE(tip != nullptr);
    CHECK(tip->method == 8);
    CHECK(tip->uncompressedSize == big.size());
    CHECK(tip->compressedSize < big.size()); // it did compress
    auto tipBytes = reader->read(*tip, &error);
    REQUIRE_MESSAGE(tipBytes.has_value(), error);
    CHECK(*tipBytes == big);

    CHECK(reader->find("no/such/entry") == nullptr);
}

TEST_CASE("zip: empty files and directory placeholders read as zero bytes") {
    const auto zip = buildZip({{"brushes/", {}, false}, {"empty.txt", {}, true}});
    auto reader = ZipReader::open(zip.data(), zip.size());
    REQUIRE(reader.has_value());
    for (const ZipEntry& e : reader->entries()) {
        auto bytes = reader->read(e);
        REQUIRE(bytes.has_value());
        CHECK(bytes->empty());
    }
}

TEST_CASE("zip: sizes come from the central directory, not the local header") {
    // The RGBA_brushes.bundle shape: data-descriptor flag set, local crc/sizes zeroed. A reader
    // trusting the local header sees every entry as empty.
    TestEntry e{"paintoppresets/a.kpp", bytesOf("payload payload payload"), true};
    e.flags = 0x8;
    e.zeroLocalSizes = true;
    const auto zip = buildZip({e});
    auto reader = ZipReader::open(zip.data(), zip.size());
    REQUIRE(reader.has_value());
    auto bytes = reader->read(reader->entries()[0]);
    REQUIRE(bytes.has_value());
    CHECK(*bytes == e.data);
}

TEST_CASE("zip: a local extra field absent from the central directory shifts the data offset") {
    TestEntry e{"meta.xml", bytesOf("<meta:meta/>"), false};
    e.localExtraLen = 28; // e.g. an mtime extra a repacker added locally only
    const auto zip = buildZip({e});
    auto reader = ZipReader::open(zip.data(), zip.size());
    REQUIRE(reader.has_value());
    auto bytes = reader->read(reader->entries()[0]);
    REQUIRE(bytes.has_value());
    CHECK(*bytes == e.data);
}

TEST_CASE("zip: an archive comment does not hide the end-of-central-directory record") {
    const auto zip = buildZip({{"a", bytesOf("x"), false}}, /*commentLen=*/321);
    auto reader = ZipReader::open(zip.data(), zip.size());
    REQUIRE(reader.has_value());
    CHECK(reader->entries().size() == 1);
}

TEST_CASE("zip: open() failures name their cause") {
    std::string error;

    SUBCASE("not a zip at all") {
        const auto junk = bytesOf("GIF89a definitely not a zip");
        CHECK(!ZipReader::open(junk.data(), junk.size(), &error).has_value());
        CHECK(error.find("end-of-central-directory") != std::string::npos);
    }
    SUBCASE("too small to hold the record") {
        const auto tiny = bytesOf("PK");
        CHECK(!ZipReader::open(tiny.data(), tiny.size(), &error).has_value());
    }
    SUBCASE("multi-disk") {
        auto zip = buildZip({{"a", bytesOf("x"), false}});
        zip[eocdAt(zip) + 4] = 1; // disk number
        CHECK(!ZipReader::open(zip.data(), zip.size(), &error).has_value());
        CHECK(error.find("multi-disk") != std::string::npos);
    }
    SUBCASE("zip64 sentinel entry count") {
        auto zip = buildZip({{"a", bytesOf("x"), false}});
        const std::size_t eocd = eocdAt(zip);
        zip[eocd + 8] = zip[eocd + 9] = 0xFF;  // entries this disk
        zip[eocd + 10] = zip[eocd + 11] = 0xFF; // entries total
        CHECK(!ZipReader::open(zip.data(), zip.size(), &error).has_value());
        CHECK(error.find("zip64") != std::string::npos);
    }
    SUBCASE("central directory outside the archive") {
        auto zip = buildZip({{"a", bytesOf("x"), false}});
        const std::size_t eocd = eocdAt(zip);
        zip[eocd + 16] = 0xF0; // central offset low byte, pushed past the record
        zip[eocd + 17] = 0xFF;
        CHECK(!ZipReader::open(zip.data(), zip.size(), &error).has_value());
        CHECK(error.find("outside") != std::string::npos);
    }
    SUBCASE("entry count over the cap") {
        auto zip = buildZip({{"a", bytesOf("x"), false}});
        const std::size_t eocd = eocdAt(zip);
        // 0x4001 = 16385 entries claimed (both fields, to pass the multi-disk equality check).
        // The bounds check passes because the claimed count is only walked, and the walk fails
        // on the missing signature -- but the cap must fire FIRST.
        zip[eocd + 8] = 0x01;
        zip[eocd + 9] = 0x40;
        zip[eocd + 10] = 0x01;
        zip[eocd + 11] = 0x40;
        CHECK(!ZipReader::open(zip.data(), zip.size(), &error).has_value());
        CHECK(error.find("cap") != std::string::npos);
    }
    SUBCASE("truncated central directory") {
        auto zip = buildZip({{"a", bytesOf("x"), false}});
        const std::size_t eocd = eocdAt(zip);
        zip[eocd + 12] -= 10; // central size lies short: the walk runs off its extent
        CHECK(!ZipReader::open(zip.data(), zip.size(), &error).has_value());
    }
}

TEST_CASE("zip: individually unusable central records are skipped and counted") {
    SUBCASE("zip64 sentinel sizes on one entry") {
        TestEntry bad{"big64", bytesOf("x"), false};
        bad.centralUncompressedOverride = 0xFFFFFFFF;
        const auto zip = buildZip({{"good", bytesOf("fine"), false}, bad});
        auto reader = ZipReader::open(zip.data(), zip.size());
        REQUIRE(reader.has_value());
        CHECK(reader->entries().size() == 1);
        CHECK(reader->skippedEntries() == 1);
        CHECK(reader->find("good") != nullptr);
        CHECK(reader->find("big64") == nullptr);
    }
    SUBCASE("a name containing NUL") {
        const auto zip = buildZip({{std::string("bad\0name", 8), bytesOf("x"), false},
                                   {"good", bytesOf("fine"), false}});
        auto reader = ZipReader::open(zip.data(), zip.size());
        REQUIRE(reader.has_value());
        CHECK(reader->entries().size() == 1);
        CHECK(reader->skippedEntries() == 1);
    }
}

TEST_CASE("zip: read() failures name their cause") {
    std::string error;

    SUBCASE("encrypted entry") {
        TestEntry e{"secret", bytesOf("data"), false};
        e.flags = 0x1;
        const auto zip = buildZip({e});
        auto reader = ZipReader::open(zip.data(), zip.size());
        REQUIRE(reader.has_value());
        CHECK(reader->entries()[0].encrypted);
        CHECK(!reader->read(reader->entries()[0], &error).has_value());
        CHECK(error.find("encrypted") != std::string::npos);
    }
    SUBCASE("unsupported method") {
        TestEntry e{"lzma", bytesOf("data"), false};
        e.methodOverride = 14;
        const auto zip = buildZip({e});
        auto reader = ZipReader::open(zip.data(), zip.size());
        REQUIRE(reader.has_value());
        CHECK(!reader->read(reader->entries()[0], &error).has_value());
        CHECK(error.find("method") != std::string::npos);
    }
    SUBCASE("declared size over the cap") {
        TestEntry e{"huge", bytesOf("tiny"), false};
        e.centralUncompressedOverride = static_cast<long long>(
            mosaic::io::brush::kMaxZipEntryBytes + 1);
        const auto zip = buildZip({e});
        auto reader = ZipReader::open(zip.data(), zip.size());
        REQUIRE(reader.has_value());
        CHECK(!reader->read(reader->entries()[0], &error).has_value());
        CHECK(error.find("cap") != std::string::npos);
    }
    SUBCASE("stored entry whose sizes disagree") {
        TestEntry e{"stored", bytesOf("eight by"), false};
        e.centralUncompressedOverride = 4; // stored: compressed must equal uncompressed
        const auto zip = buildZip({e});
        auto reader = ZipReader::open(zip.data(), zip.size());
        REQUIRE(reader.has_value());
        CHECK(!reader->read(reader->entries()[0], &error).has_value());
        CHECK(error.find("mismatched sizes") != std::string::npos);
    }
    SUBCASE("deflate stream longer than its declared output") {
        TestEntry e{"amplifier", std::vector<std::uint8_t>(4096, 0x42), true};
        e.centralUncompressedOverride = 100; // stream wants 4096; entry declares 100
        const auto zip = buildZip({e});
        auto reader = ZipReader::open(zip.data(), zip.size());
        REQUIRE(reader.has_value());
        CHECK(!reader->read(reader->entries()[0], &error).has_value());
    }
    SUBCASE("deflate stream shorter than its declared output") {
        TestEntry e{"short", bytesOf("abc"), true};
        e.centralUncompressedOverride = 1000;
        const auto zip = buildZip({e});
        auto reader = ZipReader::open(zip.data(), zip.size());
        REQUIRE(reader.has_value());
        CHECK(!reader->read(reader->entries()[0], &error).has_value());
    }
    SUBCASE("a short stream whose CRC was forged over the zero padding") {
        // The subcase above is also caught by the CRC backstop (the builder's CRC is honest).
        // Here the CRC is computed over data-plus-padding, so ONLY the avail_out side of the
        // completion check stands between the caller and silently zero-padded content.
        std::vector<std::uint8_t> padded = bytesOf("abc");
        padded.resize(1000, 0);
        TestEntry e{"forged", bytesOf("abc"), true};
        e.centralUncompressedOverride = 1000;
        e.centralCrcOverride = static_cast<long long>(
            ::crc32(0, padded.data(), static_cast<uInt>(padded.size())));
        const auto zip = buildZip({e});
        auto reader = ZipReader::open(zip.data(), zip.size());
        REQUIRE(reader.has_value());
        CHECK(!reader->read(reader->entries()[0], &error).has_value());
    }
    SUBCASE("corrupt deflate payload") {
        TestEntry e{"corrupt", std::vector<std::uint8_t>(4096, 0x42), true};
        e.corruptData = true;
        const auto zip = buildZip({e});
        auto reader = ZipReader::open(zip.data(), zip.size());
        REQUIRE(reader.has_value());
        CHECK(!reader->read(reader->entries()[0], &error).has_value());
    }
    SUBCASE("CRC mismatch on an otherwise valid stored entry") {
        TestEntry e{"crc", bytesOf("payload"), false};
        e.centralCrcOverride = 0xDEADBEEF;
        const auto zip = buildZip({e});
        auto reader = ZipReader::open(zip.data(), zip.size());
        REQUIRE(reader.has_value());
        CHECK(!reader->read(reader->entries()[0], &error).has_value());
        CHECK(error.find("CRC") != std::string::npos);
    }
    SUBCASE("local header offset pointing at garbage") {
        auto zip = buildZip({{"a", bytesOf("x"), false}});
        // Patch the central directory's local-offset field. The CD starts right after the one
        // local record; its offset field is at +42.
        const std::size_t eocd = eocdAt(zip);
        const std::uint32_t centralOffset = static_cast<std::uint32_t>(
            zip[eocd + 16] | (zip[eocd + 17] << 8) | (zip[eocd + 18] << 16) |
            (static_cast<std::uint32_t>(zip[eocd + 19]) << 24));
        zip[centralOffset + 42] = 7; // now points mid-payload, no local signature there
        auto reader = ZipReader::open(zip.data(), zip.size());
        REQUIRE(reader.has_value());
        std::string err;
        CHECK(!reader->read(reader->entries()[0], &err).has_value());
        CHECK(err.find("local header") != std::string::npos);
    }
}
