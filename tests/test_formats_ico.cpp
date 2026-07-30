#include "formats/bmp.hpp"
#include "formats/formats.hpp"
#include "formats/ico.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// libmosaicformats' ICO container (docs/export-system-plan.md §10 item 5,
// docs/formats-curated.md). Standalone: formats/{bmp,formats,ico}.hpp and nothing else of
// Mosaic's -- the PNG-payload half is exercised in tests/test_formats_backends.cpp, where libpng
// is available, because this library deliberately has no PNG encoder in it.
//
// The two cases that exist entirely because of ICO's traps:
//   * "the entry's bitmap declares twice its height" -- the classic bug, and the one that produces
//     a file that looks right in most viewers and half-height in Explorer;
//   * "a 256-pixel entry is recorded as zero" -- the directory has one byte for a side.
namespace {

using mosaicfmt::Bitmap;

Bitmap solid(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g, std::uint8_t b,
             std::uint8_t a) {
    Bitmap bitmap(w, h);
    for (std::size_t i = 0; i < bitmap.rgba.size(); i += 4) {
        bitmap.rgba[i + 0] = r;
        bitmap.rgba[i + 1] = g;
        bitmap.rgba[i + 2] = b;
        bitmap.rgba[i + 3] = a;
    }
    return bitmap;
}

Bitmap pattern(std::uint32_t w, std::uint32_t h) {
    Bitmap b(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = b.at(x, y);
            p[0] = static_cast<std::uint8_t>(x * 15 + 1);
            p[1] = static_cast<std::uint8_t>(y * 25 + 2);
            p[2] = static_cast<std::uint8_t>((x ^ y) * 9);
            p[3] = static_cast<std::uint8_t>(x + y == 0 ? 0 : 255);
        }
    return b;
}

[[nodiscard]] std::uint32_t u16At(const std::vector<std::uint8_t>& f, std::size_t off) {
    return static_cast<std::uint32_t>(f[off]) | (static_cast<std::uint32_t>(f[off + 1]) << 8);
}

[[nodiscard]] std::uint32_t u32At(const std::vector<std::uint8_t>& f, std::size_t off) {
    return u16At(f, off) | (u16At(f, off + 2) << 16);
}

[[nodiscard]] std::int32_t i32At(const std::vector<std::uint8_t>& f, std::size_t off) {
    return static_cast<std::int32_t>(u32At(f, off));
}

void pushU16(std::vector<std::uint8_t>& f, std::uint32_t v) {
    f.push_back(static_cast<std::uint8_t>(v & 0xFF));
    f.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void pushU32(std::vector<std::uint8_t>& f, std::uint32_t v) {
    pushU16(f, v & 0xFFFF);
    pushU16(f, (v >> 16) & 0xFFFF);
}

} // namespace

TEST_CASE("ICO's entry bitmap declares twice its height, and decodes at its real one") {
    const Bitmap src = pattern(16, 16);
    std::vector<mosaicfmt::IcoEntry> entries;
    mosaicfmt::IcoEntry entry;
    entry.pixels = src.view();
    entries.push_back(entry);
    const auto bytes = mosaicfmt::encodeIco(entries);
    REQUIRE(bytes.has_value());

    // Directory: reserved 0, type 1 (icon), one entry.
    CHECK(u16At(*bytes, 0) == 0);
    CHECK(u16At(*bytes, 2) == 1);
    CHECK(u16At(*bytes, 4) == 1);
    const std::size_t offset = u32At(*bytes, 6 + 12);
    REQUIRE(offset + 40 <= bytes->size());
    CHECK(i32At(*bytes, offset + 4) == 16);
    CHECK(i32At(*bytes, offset + 8) == 32);  // THE trap: the XOR image plus the AND mask
    CHECK(u16At(*bytes, offset + 14) == 32); // 32 bits per pixel

    const auto back = mosaicfmt::decodeIco(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    CHECK(back->width == 16);
    CHECK(back->height == 16);  // ... and NOT 32, and not 8
    CHECK(back->rgba == src.rgba);
}

TEST_CASE("ICO records a 256-pixel side as zero") {
    const Bitmap src = solid(256, 256, 10, 20, 30, 255);
    mosaicfmt::IcoEntry entry;
    entry.pixels = src.view();
    const auto bytes = mosaicfmt::encodeIco({entry});
    REQUIRE(bytes.has_value());
    CHECK((*bytes)[6] == 0);  // width, "256"
    CHECK((*bytes)[7] == 0);  // height

    const auto chosen = mosaicfmt::selectIcoEntry(bytes->data(), bytes->size());
    REQUIRE(chosen.has_value());
    CHECK(chosen->width == 256);
    CHECK(chosen->height == 256);
    CHECK_FALSE(chosen->isPng);
}

TEST_CASE("ICO holds several sizes and hands back the largest") {
    const Bitmap source = pattern(48, 48);
    std::vector<Bitmap> scaled;
    scaled.reserve(3);
    for (const std::uint32_t side : {16u, 32u, 48u})
        scaled.push_back(mosaicfmt::fitSquare(source.view(), side));
    std::vector<mosaicfmt::IcoEntry> entries;
    for (const Bitmap& b : scaled) {
        REQUIRE_FALSE(b.empty());
        mosaicfmt::IcoEntry entry;
        entry.pixels = b.view();
        entries.push_back(entry);
    }
    const auto bytes = mosaicfmt::encodeIco(entries);
    REQUIRE(bytes.has_value());
    CHECK(u16At(*bytes, 4) == 3);

    const auto chosen = mosaicfmt::selectIcoEntry(bytes->data(), bytes->size());
    REQUIRE(chosen.has_value());
    CHECK(chosen->width == 48);
    const auto back = mosaicfmt::decodeIco(bytes->data(), bytes->size());
    REQUIRE(back.has_value());
    CHECK(back->width == 48);
    CHECK(back->height == 48);
}

TEST_CASE("ICO reads transparency out of the AND mask when the bitmap has none") {
    // A hand-built 24-bit entry: two 2-pixel rows bottom-up, then a 1-bit mask whose set bit marks
    // the top-left pixel transparent. 24-bit payloads have no alpha channel at all, so the mask is
    // the only thing that can say a pixel is not there.
    std::vector<std::uint8_t> payload;
    pushU32(payload, 40);  // BITMAPINFOHEADER
    pushU32(payload, 2);   // width
    pushU32(payload, 4);   // height: doubled
    pushU16(payload, 1);   // planes
    pushU16(payload, 24);  // bit count
    pushU32(payload, 0);   // BI_RGB
    pushU32(payload, 0);   // sizeImage: never trusted
    pushU32(payload, 0);
    pushU32(payload, 0);
    pushU32(payload, 0);
    pushU32(payload, 0);
    // XOR rows, bottom-up: the stored first row is the BOTTOM one. Rows pad to four bytes.
    const std::uint8_t bottom[8] = {0, 0, 255, 0, 0, 255, 0, 0};  // two red pixels (B,G,R) + pad
    const std::uint8_t top[8] = {255, 0, 0, 255, 0, 0, 0, 0};     // two blue pixels + pad
    payload.insert(payload.end(), bottom, bottom + 8);
    payload.insert(payload.end(), top, top + 8);
    // AND mask, also bottom-up, four bytes a row: nothing masked in the bottom row, the leftmost
    // pixel masked in the top one.
    const std::uint8_t maskBottom[4] = {0x00, 0, 0, 0};
    const std::uint8_t maskTop[4] = {0x80, 0, 0, 0};
    payload.insert(payload.end(), maskBottom, maskBottom + 4);
    payload.insert(payload.end(), maskTop, maskTop + 4);

    std::vector<std::uint8_t> file;
    pushU16(file, 0);
    pushU16(file, 1);
    pushU16(file, 1);
    file.push_back(2);   // width
    file.push_back(2);   // height
    file.push_back(0);   // palette entries
    file.push_back(0);   // reserved
    pushU16(file, 1);    // planes
    pushU16(file, 24);   // bit count
    pushU32(file, static_cast<std::uint32_t>(payload.size()));
    pushU32(file, 22);   // the payload starts right after the directory
    file.insert(file.end(), payload.begin(), payload.end());

    const auto back = mosaicfmt::decodeIco(file.data(), file.size());
    REQUIRE(back.has_value());
    REQUIRE(back->width == 2);
    REQUIRE(back->height == 2);
    CHECK(back->at(0, 0)[3] == 0);    // masked
    CHECK(back->at(1, 0)[3] == 255);  // not masked
    CHECK(back->at(1, 0)[2] == 255);  // ... and it is the blue top row
    CHECK(back->at(0, 1)[0] == 255);  // ... over the red bottom one
    CHECK(back->at(0, 1)[3] == 255);
}

TEST_CASE("fitSquare pads with transparency rather than stretching the artwork") {
    const Bitmap wide = solid(8, 4, 200, 100, 50, 255);
    const Bitmap square = mosaicfmt::fitSquare(wide.view(), 8);
    REQUIRE(square.width == 8);
    REQUIRE(square.height == 8);
    // Two empty rows above and below the centred 8 x 4 artwork.
    for (std::uint32_t x = 0; x < 8; ++x) {
        CHECK(square.at(x, 0)[3] == 0);
        CHECK(square.at(x, 1)[3] == 0);
        CHECK(square.at(x, 2)[3] == 255);
        CHECK(square.at(x, 5)[3] == 255);
        CHECK(square.at(x, 6)[3] == 0);
        CHECK(square.at(x, 7)[3] == 0);
    }
    CHECK(square.at(0, 3)[0] == 200);

    // Downscaling averages; upscaling is refused outright, so a smaller request still fits.
    const Bitmap small = mosaicfmt::fitSquare(wide.view(), 4);
    REQUIRE(small.width == 4);
    REQUIRE(small.height == 4);
    CHECK(small.at(0, 1)[3] == 255);
    CHECK(small.at(0, 0)[3] == 0);
    CHECK(mosaicfmt::downscaleBox(wide.view(), 16, 8).empty());  // never invents pixels
}

TEST_CASE("ICO refuses the entries it cannot describe") {
    std::string error;
    const Bitmap ok = solid(16, 16, 1, 2, 3, 255);
    mosaicfmt::IcoEntry entry;
    entry.pixels = ok.view();

    CHECK_FALSE(mosaicfmt::encodeIco({}, &error).has_value());
    CHECK_FALSE(error.empty());

    const Bitmap huge = solid(300, 16, 1, 2, 3, 255);
    mosaicfmt::IcoEntry oversized;
    oversized.pixels = huge.view();
    error.clear();
    CHECK_FALSE(mosaicfmt::encodeIco({oversized}, &error).has_value());
    CHECK_FALSE(error.empty());

    // A "PNG" payload that is not a PNG would make the directory describe bytes it does not have.
    const std::vector<std::uint8_t> notPng(64, 0x42);
    mosaicfmt::IcoEntry lying;
    lying.pixels = ok.view();
    lying.png = &notPng;
    error.clear();
    CHECK_FALSE(mosaicfmt::encodeIco({lying}, &error).has_value());
    CHECK_FALSE(error.empty());
}

TEST_CASE("ICO skips a broken directory slot but keeps the good ones") {
    // Two directory slots, hand-assembled: a 32 x 32 one whose payload is past the end of the
    // file, and a good 16 x 16 one. A bad slot is a bad SLOT, not a bad file -- the good size must
    // still open, because an icon with four good sizes and one broken one still has four.
    const Bitmap src = pattern(16, 16);
    const std::vector<std::uint8_t> payload = mosaicfmt::encodeIcoDib(src.view());
    REQUIRE_FALSE(payload.empty());
    const std::uint32_t payloadOffset = 6 + 2 * 16;

    std::vector<std::uint8_t> patched;
    pushU16(patched, 0);
    pushU16(patched, 1);
    pushU16(patched, 2);  // two entries
    patched.push_back(32);
    patched.push_back(32);
    patched.push_back(0);
    patched.push_back(0);
    pushU16(patched, 1);
    pushU16(patched, 32);
    pushU32(patched, 999999);  // a length and an offset that are nowhere near the file
    pushU32(patched, 900000);
    patched.push_back(16);
    patched.push_back(16);
    patched.push_back(0);
    patched.push_back(0);
    pushU16(patched, 1);
    pushU16(patched, 32);
    pushU32(patched, static_cast<std::uint32_t>(payload.size()));
    pushU32(patched, payloadOffset);
    REQUIRE(patched.size() == payloadOffset);
    patched.insert(patched.end(), payload.begin(), payload.end());

    const auto chosen = mosaicfmt::selectIcoEntry(patched.data(), patched.size());
    REQUIRE(chosen.has_value());
    CHECK(chosen->width == 16);  // the 32 x 32 slot was unusable and is gone
    const auto back = mosaicfmt::decodeIco(patched.data(), patched.size());
    REQUIRE(back.has_value());
    CHECK(back->rgba == src.rgba);
}

TEST_CASE("ICO says so rather than guessing when an entry is a PNG") {
    // A directory whose payload starts with the PNG signature: this library has no PNG decoder, so
    // the honest outcome is a refusal that NAMES the reason, which is what lets the io adapter
    // hand the payload to libpng instead.
    std::vector<std::uint8_t> file;
    pushU16(file, 0);
    pushU16(file, 1);
    pushU16(file, 1);
    file.push_back(16);
    file.push_back(16);
    file.push_back(0);
    file.push_back(0);
    pushU16(file, 1);
    pushU16(file, 32);
    pushU32(file, 16);
    pushU32(file, 22);
    const std::uint8_t png[16] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                                  0,    0,   0,   13,  'I',  'H',  'D',  'R'};
    file.insert(file.end(), png, png + 16);

    const auto chosen = mosaicfmt::selectIcoEntry(file.data(), file.size());
    REQUIRE(chosen.has_value());
    CHECK(chosen->isPng);
    CHECK(chosen->offset == 22);
    CHECK(chosen->size == 16);
    std::string error;
    CHECK_FALSE(mosaicfmt::decodeIco(file.data(), file.size(), &error).has_value());
    CHECK(error.find("PNG") != std::string::npos);
}

TEST_CASE("ICO survives every truncation of a real file") {
    const Bitmap src = pattern(16, 16);
    mosaicfmt::IcoEntry entry;
    entry.pixels = src.view();
    const auto bytes = mosaicfmt::encodeIco({entry});
    REQUIRE(bytes.has_value());
    for (std::size_t n = 0; n < bytes->size(); ++n) {
        const std::vector<std::uint8_t> cut(bytes->begin(),
                                            bytes->begin() + static_cast<std::ptrdiff_t>(n));
        std::string error;
        const auto decoded = mosaicfmt::decodeIco(cut.data(), cut.size(), &error);
        if (decoded.has_value())
            CHECK(decoded->consistent());
    }
}
