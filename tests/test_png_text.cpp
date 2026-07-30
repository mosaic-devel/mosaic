#include "io/brush/png_text.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// The PNG text-chunk walker (io/brush/png_text.hpp) -- the container layer under `.kpp`. The
// load-bearing case is the keyword-vs-chunk-type trap of docs/brushes.md §3.1: the `preset`
// payload ships as zTXt in 213 of the default presets and as plain tEXt in 35, so the SAME lookup
// must find it through either type. Everything else here is the hostile-input posture: a
// third-party file may claim anything, and the walker must come back with a scan or an error,
// never with a crash or a gigabyte allocation.
//
// PNGs are built byte-by-byte in the test so every case pins the exact container shape it means
// to; only the zip-bomb stream (16 MB + 1 of inflated payload) is a committed fixture, because a
// 16 KB deflate stream has no readable source form.
namespace {

using mosaic::io::brush::PngText;
using mosaic::io::brush::PngTextScan;
using mosaic::io::brush::scanPngText;

// CRC-32 as PNG defines it, table-free (the test's independent implementation -- agreeing with
// zlib's through a different formulation is part of the point).
std::uint32_t crc32(const std::uint8_t* p, std::size_t n) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

void be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

// One chunk: length + type + data + CRC(type..data). `crcDelta` corrupts the stored CRC.
std::vector<std::uint8_t> chunk(const char type[5], const std::vector<std::uint8_t>& data,
                                std::uint32_t crcDelta = 0) {
    std::vector<std::uint8_t> out;
    be32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    be32(out, crc32(body.data(), body.size()) + crcDelta);
    return out;
}

std::vector<std::uint8_t> bytes(std::string_view s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

// Signature + a minimal IHDR. The walker never validates IHDR, but every real file has one and
// the tests should look like real files.
std::vector<std::uint8_t> pngStart() {
    std::vector<std::uint8_t> out = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, 1);
    be32(ihdr, 1);
    for (const std::uint8_t b : {8, 6, 0, 0, 0})
        ihdr.push_back(b);
    const auto c = chunk("IHDR", ihdr);
    out.insert(out.end(), c.begin(), c.end());
    return out;
}

void append(std::vector<std::uint8_t>& png, const std::vector<std::uint8_t>& c) {
    png.insert(png.end(), c.begin(), c.end());
}

std::vector<std::uint8_t> withEnd(std::vector<std::uint8_t> png) {
    append(png, chunk("IEND", {}));
    return png;
}

// zlib.compress(b'<Preset name="walker"/>') -- a real deflate stream, generated once and pinned.
const std::vector<std::uint8_t> kDeflated = {
    0x78, 0x9C, 0xB3, 0x09, 0x28, 0x4A, 0x2D, 0x4E, 0x2D, 0x51, 0xC8,
    0x4B, 0xCC, 0x4D, 0xB5, 0x55, 0x2A, 0x4F, 0xCC, 0xC9, 0x4E, 0x2D,
    0x52, 0xD2, 0xB7, 0x03, 0x00, 0x62, 0x58, 0x07, 0xE5};
constexpr const char* kInflated = "<Preset name=\"walker\"/>";

std::vector<std::uint8_t> ztxtData(std::string_view keyword,
                                   const std::vector<std::uint8_t>& stream) {
    auto d = bytes(keyword);
    d.push_back(0); // keyword terminator
    d.push_back(0); // compression method 0 = deflate
    d.insert(d.end(), stream.begin(), stream.end());
    return d;
}

std::string fixture(const char* name) { return std::string(MOSAIC_FIXTURE_DIR) + "/" + name; }

} // namespace

TEST_CASE("png_text: the same keyword is found through tEXt and through zTXt") {
    // §3.1's trap, distilled: 35 shipped presets store `preset` as tEXt, 213 as zTXt, and the
    // version string predicts nothing. One lookup, two container shapes, identical result.
    auto asText = pngStart();
    append(asText, chunk("tEXt", bytes(std::string("preset") + '\0' + kInflated)));
    asText = withEnd(asText);

    auto asZ = pngStart();
    append(asZ, chunk("zTXt", ztxtData("preset", kDeflated)));
    asZ = withEnd(asZ);

    const mosaic::io::brush::PngTextKind kinds[] = {mosaic::io::brush::PngTextKind::Text,
                                                    mosaic::io::brush::PngTextKind::ZText};
    int i = 0;
    for (const auto* png : {&asText, &asZ}) {
        std::string err;
        const auto scan = scanPngText(png->data(), png->size(), &err);
        REQUIRE_MESSAGE(scan.has_value(), err);
        CHECK(scan->undecodable == 0);
        const PngText* preset = scan->find("preset");
        REQUIRE(preset != nullptr);
        CHECK(preset->text == kInflated);
        // The kind is a report of what carried the payload, never part of the lookup.
        CHECK(preset->kind == kinds[i++]);
    }
}

TEST_CASE("png_text: tEXt with an empty value, and keyword lookup misses") {
    auto png = pngStart();
    append(png, chunk("tEXt", bytes(std::string_view("empty\0", 6))));
    append(png, chunk("tEXt", bytes(std::string_view("version\0002.2", 11))));
    png = withEnd(png);

    const auto scan = scanPngText(png.data(), png.size());
    REQUIRE(scan.has_value());
    CHECK(scan->undecodable == 0);
    REQUIRE(scan->chunks.size() == 2);
    CHECK(scan->find("empty")->text.empty());
    CHECK(scan->find("version")->text == "2.2");
    CHECK(scan->find("preset") == nullptr);
    CHECK(scan->find("") == nullptr); // an empty keyword is unrepresentable, not a wildcard
}

TEST_CASE("png_text: iTXt decodes in both its compressed and uncompressed shapes") {
    // keyword NUL flag method language NUL translated-keyword NUL text. The language tag and
    // translated keyword must be skipped over, not concatenated into the value.
    std::vector<std::uint8_t> raw = bytes("note");
    for (const std::uint8_t b : {0, 0, 0})
        raw.push_back(b); // NUL, flag=0 (uncompressed), method=0
    auto lang = bytes(std::string_view("en\0Notiz\0", 9));
    raw.insert(raw.end(), lang.begin(), lang.end());
    auto text = bytes("hello");
    raw.insert(raw.end(), text.begin(), text.end());

    std::vector<std::uint8_t> comp = bytes("mosaic-preset");
    comp.push_back(0);
    comp.push_back(1); // flag=1: compressed
    comp.push_back(0); // method 0 = deflate
    comp.push_back(0); // empty language
    comp.push_back(0); // empty translated keyword
    comp.insert(comp.end(), kDeflated.begin(), kDeflated.end());

    auto png = pngStart();
    append(png, chunk("iTXt", raw));
    append(png, chunk("iTXt", comp));
    png = withEnd(png);

    const auto scan = scanPngText(png.data(), png.size());
    REQUIRE(scan.has_value());
    CHECK(scan->undecodable == 0);
    CHECK(scan->find("note")->text == "hello");
    CHECK(scan->find("mosaic-preset")->text == kInflated);
}

TEST_CASE("png_text: per-chunk damage is counted and skipped, not fatal") {
    auto good = chunk("tEXt", bytes(std::string_view("version\0002.2", 11)));

    // Every flavour of individually-bad text chunk; the good chunk must survive them all.
    const struct {
        const char* why;
        std::vector<std::uint8_t> bad;
    } cases[] = {
        {"corrupted CRC", chunk("tEXt", bytes(std::string_view("k\0v", 3)), /*crcDelta=*/1)},
        {"unknown zTXt compression method",
         [] {
             auto d = ztxtData("preset", kDeflated);
             d[7] = 1; // method byte (after "preset\0")
             return chunk("zTXt", d);
         }()},
        {"truncated deflate stream",
         chunk("zTXt", ztxtData("preset", std::vector<std::uint8_t>(kDeflated.begin(),
                                                                    kDeflated.begin() + 10)))},
        {"garbage deflate stream", chunk("zTXt", ztxtData("preset", bytes("not deflate")))},
        {"keyword unterminated", chunk("tEXt", bytes("no separator here"))},
        {"keyword empty", chunk("tEXt", bytes(std::string_view("\0text", 5)))},
        {"keyword over 79 bytes", chunk("tEXt", bytes(std::string(80, 'k') + std::string("\0v", 2)))},
        {"zTXt too short for its method byte", chunk("zTXt", bytes(std::string_view("k\0", 2)))},
        {"iTXt missing its language fields",
         chunk("iTXt", bytes(std::string_view("k\0\0\0", 4)))},
    };
    for (const auto& c : cases) {
        CAPTURE(c.why);
        auto png = pngStart();
        append(png, c.bad);
        append(png, good);
        png = withEnd(png);

        const auto scan = scanPngText(png.data(), png.size());
        REQUIRE(scan.has_value());
        CHECK(scan->undecodable == 1);
        REQUIRE(scan->chunks.size() == 1);
        CHECK(scan->find("version")->text == "2.2");
    }
}

TEST_CASE("png_text: a text payload inflating past the cap is skipped (committed zip bomb)") {
    std::ifstream f(fixture("brush/ztxt_bomb.png"), std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    REQUIRE(f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))
                .good());

    // A 16 KB zTXt claiming 16 MB + 1 of payload: one byte over kMaxTextBytes. Skipped -- and
    // without a 16 MB allocation spike being observable, though the test can only pin the verdict.
    const auto scan = scanPngText(buf.data(), buf.size());
    REQUIRE(scan.has_value());
    CHECK(scan->undecodable == 1);
    REQUIRE(scan->chunks.size() == 1);
    CHECK(scan->find("preset") == nullptr);
    CHECK(scan->find("version")->text == "2.2");
}

TEST_CASE("png_text: the per-FILE text budget caps accumulation across chunks") {
    // Five chunks each inflating to exactly the per-chunk cap: individually legal, but 64 chunks
    // of 16 MB would be a gigabyte out of a ~1 MB file. The first four fill kMaxTotalTextBytes
    // exactly; the fifth is over, and so is every non-empty text after it -- the budget does not
    // reopen once exhausted.
    std::ifstream f(fixture("brush/ztxt_total_bomb.png"), std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    REQUIRE(f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))
                .good());

    const auto scan = scanPngText(buf.data(), buf.size());
    REQUIRE(scan.has_value());
    CHECK(scan->undecodable == 2); // bomb4, and the small `version` behind it
    REQUIRE(scan->chunks.size() == 4);
    CHECK(scan->find("bomb3") != nullptr);
    CHECK(scan->find("bomb4") == nullptr);
    CHECK(scan->find("version") == nullptr);
}

TEST_CASE("png_text: container-level damage is an error, not a scan") {
    std::string err;

    SUBCASE("not a PNG") {
        const auto junk = bytes("GIF89a definitely not a PNG");
        CHECK(!scanPngText(junk.data(), junk.size(), &err).has_value());
        CHECK(!scanPngText(nullptr, 0, &err).has_value());
    }
    SUBCASE("chunk length runs past the end of the file") {
        auto png = pngStart();
        auto lying = chunk("tEXt", bytes(std::string_view("k\0v", 3)));
        lying[3] = 200; // claimed length >> actual remaining bytes
        append(png, lying);
        png = withEnd(png);
        CHECK(!scanPngText(png.data(), png.size(), &err).has_value());
        CHECK(err.find("truncated") != std::string::npos);
    }
    SUBCASE("chunk length over 2^31 is malformed even if the bytes existed") {
        auto png = pngStart();
        for (const std::uint8_t b : {0x80, 0x00, 0x00, 0x00})
            png.push_back(b);
        append(png, bytes("tEXt"));
        CHECK(!scanPngText(png.data(), png.size(), &err).has_value());
    }
    SUBCASE("missing IEND") {
        auto png = pngStart();
        append(png, chunk("tEXt", bytes(std::string_view("k\0v", 3))));
        // no IEND: the walk must not fall off the end quietly
        CHECK(!scanPngText(png.data(), png.size(), &err).has_value());
        CHECK(err.find("truncated") != std::string::npos);
    }
    SUBCASE("file cut mid-CRC") {
        auto png = pngStart();
        append(png, chunk("tEXt", bytes(std::string_view("k\0v", 3))));
        png = withEnd(png);
        png.resize(png.size() - 2);
        CHECK(!scanPngText(png.data(), png.size(), &err).has_value());
    }
    SUBCASE("chunk data fits but its CRC is cut by the end of the file") {
        // The framing check must count the 4 CRC bytes, not just the data: with 2 of them missing
        // the walk must stop HERE, because reading the CRC would read past the buffer. The
        // exact-size copy matters -- in the original vector the stale capacity would hide the
        // overread from ASan, and this case is as much for the sanitizer as for the verdict.
        auto png = pngStart();
        append(png, chunk("tEXt", bytes(std::string_view("k\0v", 3))));
        const std::vector<std::uint8_t> cut(png.begin(), png.end() - 2);
        CHECK(!scanPngText(cut.data(), cut.size(), &err).has_value());
        CHECK(err.find("truncated") != std::string::npos);
    }
}

TEST_CASE("png_text: the walk stops at IEND; trailing bytes are not chunks") {
    auto png = pngStart();
    append(png, chunk("tEXt", bytes(std::string_view("before\0yes", 10))));
    png = withEnd(png);
    // Trailing garbage after IEND -- including something chunk-shaped -- is out of the container.
    append(png, chunk("tEXt", bytes(std::string_view("after\0no", 8))));

    const auto scan = scanPngText(png.data(), png.size());
    REQUIRE(scan.has_value());
    CHECK(scan->chunks.size() == 1);
    CHECK(scan->find("before") != nullptr);
    CHECK(scan->find("after") == nullptr);
    CHECK(scan->undecodable == 0);
}

TEST_CASE("png_text: the per-file chunk count is capped, and the overflow is counted") {
    auto png = pngStart();
    for (int i = 0; i < mosaic::io::brush::kMaxTextChunks + 3; ++i) {
        const std::string kw = "k" + std::to_string(i);
        append(png, chunk("tEXt", bytes(kw + std::string("\0v", 2))));
    }
    png = withEnd(png);

    const auto scan = scanPngText(png.data(), png.size());
    REQUIRE(scan.has_value());
    CHECK(static_cast<int>(scan->chunks.size()) == mosaic::io::brush::kMaxTextChunks);
    CHECK(scan->undecodable == 3);
    CHECK(scan->find("k0") != nullptr); // first in, kept
}

TEST_CASE("png_text: non-text chunks are never inspected, damaged or not") {
    // A corrupt IDAT CRC is the pixel decoder's business; the text walk must not care.
    auto png = pngStart();
    append(png, chunk("IDAT", bytes("pixels"), /*crcDelta=*/7));
    append(png, chunk("tEXt", bytes(std::string_view("version\0005.0", 11))));
    png = withEnd(png);

    const auto scan = scanPngText(png.data(), png.size());
    REQUIRE(scan.has_value());
    CHECK(scan->undecodable == 0);
    CHECK(scan->find("version")->text == "5.0");
}
