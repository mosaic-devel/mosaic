#include "common/image.hpp"
#include "io/caps.hpp"
#include "io/format_registry.hpp"
#include "io/io.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

// The M4 raster codecs: WebP, AVIF, TIFF and GIF (docs/export-system-plan.md §10 item 4), both as
// encoders (io/io.hpp) and as FormatBackends in the registry.
//
// Every codec is an OPTIONAL build dependency, so every case is gated on its own *Supported()
// probe: a machine without libavif must still run a green suite. The probe is asserted to agree
// with the backend's available(), which is the one thing that would silently offer a format that
// cannot encode.
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;
namespace io = mosaic::io;

// A per-test scratch path under the OS temp dir, removed when the test ends (the TempPng pattern
// from tests/test_io.cpp -- tests never write into the source tree).
struct TempFile {
    std::filesystem::path path;
    TempFile(const char* stem, const char* extension)
        : path(std::filesystem::temp_directory_path() /
               (std::string("mosaic_test_") + stem + extension)) {}
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    [[nodiscard]] std::string str() const { return path.string(); }
};

// Varied colour AND alpha, including fully transparent pixels with non-zero colour underneath --
// the case that separates a truly exact encoder from a nearly exact one.
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

Image opaquePattern(std::uint32_t w, std::uint32_t h) {
    Image img = pattern(w, h);
    for (std::size_t i = 3; i < img.rgba.size(); i += 4)
        img.rgba[i] = 255;
    return img;
}

// A photographic-ish opaque source: smooth ramps carrying a little deterministic grain. Lossy
// coding needs CONTENT to beat lossless on -- see the size assertion in the WebP case, which a
// small synthetic swatch fails for reasons that have nothing to do with the encoder.
Image photoish(std::uint32_t w, std::uint32_t h) {
    Image img(w, h);
    std::uint32_t s = 0x12345678u;
    const auto chan = [](int v) {
        return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
    };
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            s = s * 1664525u + 1013904223u;
            const int grain = static_cast<int>((s >> 24) & 0x1Fu) - 16;
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            img.rgba[p + 0] = chan(static_cast<int>(x * 255 / std::max(1u, w - 1)) + grain);
            img.rgba[p + 1] = chan(static_cast<int>(y * 255 / std::max(1u, h - 1)) + grain);
            img.rgba[p + 2] = chan(128 + grain);
            img.rgba[p + 3] = 255;
        }
    return img;
}

// A handful of flat colours plus genuinely empty pixels: what an indexed format can hold exactly.
Image poster(std::uint32_t w, std::uint32_t h) {
    static const Color8 kColors[5] = {{220, 30, 40, 255},
                                      {30, 220, 40, 255},
                                      {40, 30, 220, 255},
                                      {250, 250, 250, 255},
                                      {12, 12, 12, 255}};
    Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            // A transparent border; GIF can only store transparency as "no colour at all", so
            // the source must spell it the same way for the round trip to be exact.
            const bool border = x == 0 || y == 0 || x + 1 == w || y + 1 == h;
            const Color8 c =
                border ? Color8{0, 0, 0, 0} : kColors[(x / 2 + y / 3) % 5];
            img.rgba[p + 0] = c.r;
            img.rgba[p + 1] = c.g;
            img.rgba[p + 2] = c.b;
            img.rgba[p + 3] = c.a;
        }
    return img;
}

int maxChannelDelta(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size())
        return 1000;
    int worst = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i])));
    return worst;
}

// Truncate `bytes` at several fractions and feed each to the decoder: none may crash, none may
// claim success with a wrong size. Also feeds pure garbage that keeps the magic bytes.
void expectSurvivesTruncation(const std::vector<std::uint8_t>& bytes) {
    REQUIRE(bytes.size() > 16);
    for (const std::size_t denominator : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                                          std::size_t{8}, std::size_t{16}}) {
        const std::size_t keep = bytes.size() / denominator - (denominator == 1 ? 1 : 0);
        std::string err;
        const auto decoded = io::decodeImageBytes(bytes.data(), keep, &err);
        // A truncation MAY still decode (some formats front-load everything); what it may never
        // do is crash, hang, or come back with a nonsense buffer.
        if (decoded.has_value())
            CHECK(decoded->rgba.size() == decoded->pixelCount() * 4);
        else
            CHECK_FALSE(err.empty());
    }

    // Damage the PAYLOAD, not the headers: a scrambled entropy-coded tail is the realistic
    // corruption, and it exercises the codec's own error path rather than our dimension guard.
    std::vector<std::uint8_t> corrupt = bytes;
    for (std::size_t i = corrupt.size() * 3 / 4; i < corrupt.size(); ++i)
        corrupt[i] = static_cast<std::uint8_t>((i * 7919u) & 0xFFu);
    std::string err;
    const auto decoded = io::decodeImageBytes(corrupt.data(), corrupt.size(), &err);
    if (decoded.has_value())
        CHECK(decoded->rgba.size() == decoded->pixelCount() * 4);
}

} // namespace

TEST_CASE("sniffImageFormat knows the M4 signatures, and only those") {
    const std::uint8_t webp[12] = {'R', 'I', 'F', 'F', 0x20, 0, 0, 0, 'W', 'E', 'B', 'P'};
    const std::uint8_t avif[12] = {0, 0, 0, 0x20, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f'};
    const std::uint8_t avis[12] = {0, 0, 0, 0x20, 'f', 't', 'y', 'p', 'a', 'v', 'i', 's'};
    const std::uint8_t heic[12] = {0, 0, 0, 0x20, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c'};
    const std::uint8_t tiffLe[4] = {'I', 'I', 0x2A, 0x00};
    const std::uint8_t tiffBe[4] = {'M', 'M', 0x00, 0x2A};
    const std::uint8_t bigTiff[4] = {'I', 'I', 0x2B, 0x00};
    const std::uint8_t gif87[6] = {'G', 'I', 'F', '8', '7', 'a'};
    const std::uint8_t gif89[6] = {'G', 'I', 'F', '8', '9', 'a'};
    const std::uint8_t riffWave[12] = {'R', 'I', 'F', 'F', 0x20, 0, 0, 0, 'W', 'A', 'V', 'E'};

    CHECK(io::sniffImageFormat(webp, sizeof webp) == io::ImageFormat::WebP);
    CHECK(io::sniffImageFormat(avif, sizeof avif) == io::ImageFormat::Avif);
    CHECK(io::sniffImageFormat(avis, sizeof avis) == io::ImageFormat::Avif);
    CHECK(io::sniffImageFormat(tiffLe, sizeof tiffLe) == io::ImageFormat::Tiff);
    CHECK(io::sniffImageFormat(tiffBe, sizeof tiffBe) == io::ImageFormat::Tiff);
    CHECK(io::sniffImageFormat(bigTiff, sizeof bigTiff) == io::ImageFormat::Tiff);
    CHECK(io::sniffImageFormat(gif87, sizeof gif87) == io::ImageFormat::Gif);
    CHECK(io::sniffImageFormat(gif89, sizeof gif89) == io::ImageFormat::Gif);

    // HEIC is deliberately unsupported and Mosaic ships no HEVC code: its brand must not sniff
    // as anything we claim to decode, or the error would blame the file instead of explaining.
    CHECK(io::sniffImageFormat(heic, sizeof heic) == io::ImageFormat::Unknown);
    CHECK(io::sniffImageFormat(riffWave, sizeof riffWave) == io::ImageFormat::Unknown);
    CHECK(io::sniffImageFormat(webp, 8) == io::ImageFormat::Unknown);  // too short to be sure

    CHECK(io::imageFormatName(io::ImageFormat::WebP) == "WebP");
    CHECK(io::imageFormatName(io::ImageFormat::Unknown) == "unknown");
}

// ---- WebP --------------------------------------------------------------------------------------

TEST_CASE("WebP: lossless + exact is a bit-exact round trip; lossy is close") {
    if (!io::webpSupported()) {
        MESSAGE("libwebp not compiled in -- skipping");
        return;
    }
    const Image src = pattern(23, 17);

    io::WebpSaveOptions lossless;
    lossless.lossless = true;
    lossless.exact = true;  // keeps the colour under fully transparent pixels
    std::string err;
    const auto exact = io::encodeWebp(src, lossless, &err);
    REQUIRE_MESSAGE(exact.has_value(), err);
    const auto back = io::decodeImageBytes(exact->data(), exact->size(), &err);
    REQUIRE_MESSAGE(back.has_value(), err);
    CHECK(*back == src);

    // The lossy/lossless size comparison uses an OPAQUE source on purpose: a WebP's alpha plane
    // is lossless-coded in both modes, and this pattern's alpha is high-entropy enough to
    // dominate the file and mask the difference the assertion is about.
    // ⚠ Big enough, and photographic enough, to MEAN something: a 23x17 synthetic swatch codes to
    // ~60 bytes losslessly while a lossy VP8 keyframe's fixed overhead alone is ~300, so the
    // comparison below inverts on a small image for reasons that are not about quality.
    const Image opaque = photoish(192, 144);
    io::WebpSaveOptions lossy;
    lossy.lossless = false;
    lossy.quality = 70;
    const auto small = io::encodeWebp(opaque, lossy, &err);
    REQUIRE_MESSAGE(small.has_value(), err);
    const auto opaqueLossless = io::encodeWebp(opaque, lossless, &err);
    REQUIRE_MESSAGE(opaqueLossless.has_value(), err);
    const auto lossyBack = io::decodeImageBytes(small->data(), small->size(), &err);
    REQUIRE_MESSAGE(lossyBack.has_value(), err);
    CHECK(lossyBack->width == opaque.width);   // `small` encodes `opaque`, not `src`
    CHECK(lossyBack->height == opaque.height);
    CHECK(small->size() < opaqueLossless->size());  // that is the whole point of a lossy mode

    expectSurvivesTruncation(*exact);
}

TEST_CASE("WebP: an empty image and an impossible size are refused with a reason") {
    if (!io::webpSupported())
        return;
    std::string err;
    CHECK_FALSE(io::encodeWebp(Image{}, {}, &err).has_value());
    CHECK_FALSE(err.empty());

    // WEBP_MAX_DIMENSION is 16383; the check must fire before any allocation of that size.
    Image huge;
    huge.width = 20000;
    huge.height = 1;
    huge.rgba.assign(static_cast<std::size_t>(20000) * 4, 0);
    err.clear();
    CHECK_FALSE(io::encodeWebp(huge, {}, &err).has_value());
    CHECK_FALSE(err.empty());
}

TEST_CASE("WebP: metadata is muxed in and the picture still decodes") {
    if (!io::webpSupported())
        return;
    const Image src = opaquePattern(12, 9);
    io::WebpSaveOptions plain;
    plain.lossless = true;
    plain.exact = true;
    io::WebpSaveOptions tagged = plain;
    tagged.metadata.exif = std::vector<std::uint8_t>{'I', 'I', 42, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    std::string err;
    const auto bare = io::encodeWebp(src, plain, &err);
    REQUIRE_MESSAGE(bare.has_value(), err);
    const auto withExif = io::encodeWebp(src, tagged, &err);
    REQUIRE_MESSAGE(withExif.has_value(), err);
    CHECK(withExif->size() > bare->size());  // the mux really added a chunk

    const auto back = io::decodeImageBytes(withExif->data(), withExif->size(), &err);
    REQUIRE_MESSAGE(back.has_value(), err);
    CHECK(*back == src);
}

// ---- AVIF --------------------------------------------------------------------------------------

TEST_CASE("AVIF: lossless reproduces the image; quality moves the file size") {
    if (!io::avifSupported()) {
        MESSAGE("no AV1 encoder Mosaic will drive -- skipping");
        return;
    }
    // Big enough that the lossy/lossless size difference clears AVIF's fixed container overhead,
    // small enough that a speed-8 encode is instant.
    const Image src = opaquePattern(48, 48);

    io::AvifSaveOptions lossless;
    lossless.lossless = true;
    lossless.speed = 8;  // the test wants an answer, not the smallest possible file
    std::string err;
    const auto exact = io::encodeAvif(src, lossless, &err);
    REQUIRE_MESSAGE(exact.has_value(), err);
    const auto back = io::decodeImageBytes(exact->data(), exact->size(), &err);
    REQUIRE_MESSAGE(back.has_value(), err);
    REQUIRE(back->width == src.width);
    REQUIRE(back->height == src.height);
    // libavif's documented lossless recipe (quality 100 + 4:4:4 + identity matrix, all of which
    // encodeAvif enforces together) is bit-exact. One count of slack keeps a codec-version quirk
    // from failing the suite while still catching a real regression: the lossy check below shows
    // what a genuine quality loss looks like at this size.
    CHECK(maxChannelDelta(*back, src) <= 1);

    io::AvifSaveOptions lossy;
    lossy.lossless = false;
    lossy.quality = 20;
    lossy.speed = 8;
    lossy.yuv = io::AvifSaveOptions::Yuv::Yuv420;
    const auto small = io::encodeAvif(src, lossy, &err);
    REQUIRE_MESSAGE(small.has_value(), err);
    CHECK(small->size() < exact->size());

    expectSurvivesTruncation(*exact);
}

TEST_CASE("AVIF: transparency survives a lossless encode") {
    if (!io::avifSupported())
        return;
    Image src = opaquePattern(16, 16);
    for (std::uint32_t x = 0; x < 16; ++x)
        src.rgba[static_cast<std::size_t>(x) * 4 + 3] = 0;  // a fully transparent first row

    io::AvifSaveOptions opts;
    opts.lossless = true;
    opts.speed = 8;
    std::string err;
    const auto bytes = io::encodeAvif(src, opts, &err);
    REQUIRE_MESSAGE(bytes.has_value(), err);
    const auto back = io::decodeImageBytes(bytes->data(), bytes->size(), &err);
    REQUIRE_MESSAGE(back.has_value(), err);
    for (std::uint32_t x = 0; x < 16; ++x)
        CHECK(back->rgba[static_cast<std::size_t>(x) * 4 + 3] == 0);
    CHECK(back->rgba[static_cast<std::size_t>(16 * 4) + 3] == 255);  // the second row is opaque
}

// ---- TIFF --------------------------------------------------------------------------------------

TEST_CASE("TIFF: every offered compression is a bit-exact round trip") {
    if (!io::tiffSupported()) {
        MESSAGE("libtiff not compiled in -- skipping");
        return;
    }
    const Image src = pattern(31, 13);
    using Compression = io::TiffSaveOptions::Compression;
    const Compression kAll[] = {Compression::None, Compression::Lzw, Compression::Deflate,
                                Compression::PackBits, Compression::Zstd};

    std::size_t tried = 0;
    for (const Compression compression : kAll) {
        if (!io::tiffCompressionAvailable(compression))
            continue;  // this libtiff was not built with it, and the schema will not offer it
        ++tried;
        for (const bool predictor : {false, true}) {
            io::TiffSaveOptions opts;
            opts.compression = compression;
            opts.predictor = predictor;
            std::string err;
            const auto bytes = io::encodeTiff(src, opts, &err);
            REQUIRE_MESSAGE(bytes.has_value(), err);
            const auto back = io::decodeImageBytes(bytes->data(), bytes->size(), &err);
            REQUIRE_MESSAGE(back.has_value(), err);
            CHECK(*back == src);  // TIFF is lossless in every mode we offer
        }
    }
    CHECK(tried >= 2);  // None and LZW are always configured in libtiff
}

TEST_CASE("TIFF: BigTIFF and the associated-alpha tag both stay readable") {
    if (!io::tiffSupported())
        return;
    const Image src = pattern(9, 7);

    io::TiffSaveOptions big;
    big.bigTiff = true;
    big.compression = io::TiffSaveOptions::Compression::None;  // always configured

    std::string err;
    const auto bytes = io::encodeTiff(src, big, &err);
    REQUIRE_MESSAGE(bytes.has_value(), err);
    CHECK(io::sniffImageFormat(bytes->data(), bytes->size()) == io::ImageFormat::Tiff);
    const auto back = io::decodeImageBytes(bytes->data(), bytes->size(), &err);
    REQUIRE_MESSAGE(back.has_value(), err);
    CHECK(*back == src);

    // Associated alpha is a claim about the PIXELS: the writer premultiplies them, so the reader
    // has to undo it. That is genuinely lossy at low alpha, so only the alpha channel is exact.
    io::TiffSaveOptions associated;
    associated.premultipliedAlpha = true;
    associated.compression = io::TiffSaveOptions::Compression::None;
    const auto premul = io::encodeTiff(src, associated, &err);
    REQUIRE_MESSAGE(premul.has_value(), err);
    const auto undone = io::decodeImageBytes(premul->data(), premul->size(), &err);
    REQUIRE_MESSAGE(undone.has_value(), err);
    for (std::size_t i = 0; i < src.pixelCount(); ++i) {
        REQUIRE(undone->rgba[i * 4 + 3] == src.rgba[i * 4 + 3]);
        if (src.rgba[i * 4 + 3] < 200)
            continue;  // heavy premultiply rounding: not what this case is checking
        for (int c = 0; c < 3; ++c)
            CHECK(std::abs(static_cast<int>(undone->rgba[i * 4 + c]) -
                           static_cast<int>(src.rgba[i * 4 + c])) <= 2);
    }

    expectSurvivesTruncation(*bytes);
}

TEST_CASE("TIFF: an unconfigured compression is refused rather than silently swapped") {
    if (!io::tiffSupported())
        return;
    using Compression = io::TiffSaveOptions::Compression;
    for (const Compression compression : {Compression::Zstd, Compression::Lzw}) {
        if (io::tiffCompressionAvailable(compression))
            continue;
        io::TiffSaveOptions opts;
        opts.compression = compression;
        std::string err;
        CHECK_FALSE(io::encodeTiff(pattern(4, 4), opts, &err).has_value());
        CHECK_FALSE(err.empty());
    }
    // COMPRESSION_NONE is part of libtiff's core and can never be missing.
    CHECK(io::tiffCompressionAvailable(Compression::None));
}

// ---- GIF ---------------------------------------------------------------------------------------

TEST_CASE("GIF: a picture that fits the palette round-trips exactly, interlaced or not") {
    if (!io::gifSupported()) {
        MESSAGE("giflib not compiled in -- skipping");
        return;
    }
    const Image src = poster(24, 18);

    for (const bool interlace : {false, true}) {
        io::GifSaveOptions opts;
        opts.interlace = interlace;
        opts.dither = false;
        std::string err;
        const auto bytes = io::encodeGif(src, opts, &err);
        REQUIRE_MESSAGE(bytes.has_value(), err);
        CHECK(io::sniffImageFormat(bytes->data(), bytes->size()) == io::ImageFormat::Gif);
        const auto back = io::decodeImageBytes(bytes->data(), bytes->size(), &err);
        REQUIRE_MESSAGE(back.has_value(), err);
        CAPTURE(interlace);
        CHECK(*back == src);  // five flat colours plus a transparent border: all of it fits
    }
}

TEST_CASE("GIF: a truecolour picture is quantised, and the palette budget is honoured") {
    if (!io::gifSupported())
        return;
    const Image src = opaquePattern(40, 30);

    io::GifSaveOptions opts;
    opts.paletteSize = 16;
    opts.dither = true;
    std::string err;
    const auto bytes = io::encodeGif(src, opts, &err);
    REQUIRE_MESSAGE(bytes.has_value(), err);
    const auto back = io::decodeImageBytes(bytes->data(), bytes->size(), &err);
    REQUIRE_MESSAGE(back.has_value(), err);
    REQUIRE(back->width == src.width);
    REQUIRE(back->height == src.height);

    std::set<std::uint32_t> colours;
    for (std::size_t i = 0; i < back->pixelCount(); ++i)
        colours.insert((static_cast<std::uint32_t>(back->rgba[i * 4]) << 16) |
                       (static_cast<std::uint32_t>(back->rgba[i * 4 + 1]) << 8) |
                       back->rgba[i * 4 + 2]);
    CHECK(colours.size() <= 16);
    CHECK(colours.size() > 1);

    // The comment extension is written and does not disturb the pixels.
    io::GifSaveOptions commented = opts;
    commented.comment = "Made with Mosaic";
    const auto withComment = io::encodeGif(src, commented, &err);
    REQUIRE_MESSAGE(withComment.has_value(), err);
    CHECK(withComment->size() > bytes->size());
    const auto commentedBack = io::decodeImageBytes(withComment->data(), withComment->size(), &err);
    REQUIRE_MESSAGE(commentedBack.has_value(), err);
    CHECK(*commentedBack == *back);

    expectSurvivesTruncation(*bytes);
}

TEST_CASE("GIF: soft edges are cut against the threshold and land on the matte") {
    if (!io::gifSupported())
        return;
    Image src(4, 1);
    const std::uint8_t alphas[4] = {0, 100, 200, 255};
    for (std::uint32_t x = 0; x < 4; ++x) {
        src.rgba[x * 4 + 0] = 0;
        src.rgba[x * 4 + 1] = 0;
        src.rgba[x * 4 + 2] = 0;
        src.rgba[x * 4 + 3] = alphas[x];
    }

    io::GifSaveOptions opts;
    opts.alphaThreshold = 128;
    opts.matte = Color8{255, 255, 255, 255};
    opts.dither = false;
    std::string err;
    const auto bytes = io::encodeGif(src, opts, &err);
    REQUIRE_MESSAGE(bytes.has_value(), err);
    const auto back = io::decodeImageBytes(bytes->data(), bytes->size(), &err);
    REQUIRE_MESSAGE(back.has_value(), err);
    CHECK(back->rgba[3] == 0);    // alpha 0   -> transparent
    CHECK(back->rgba[7] == 0);    // alpha 100 -> below the threshold, also transparent
    CHECK(back->rgba[11] == 255); // alpha 200 -> opaque, blended onto the white matte
    CHECK(back->rgba[15] == 255);
    CHECK(back->rgba[8] > back->rgba[12]);  // the 200 pixel is lighter than the fully opaque one
}

// ---- file paths, probes and the registry --------------------------------------------------------

TEST_CASE("the save-to-path entry points agree with the in-memory encoders") {
    const Image src = poster(10, 8);
    std::string err;

    if (io::gifSupported()) {
        TempFile out("save", ".gif");
        REQUIRE_MESSAGE(io::saveGif(src, out.str(), {}, &err), err);
        const auto back = io::loadImage(out.str(), &err);
        REQUIRE_MESSAGE(back.has_value(), err);
        CHECK(*back == src);

        const auto dims = io::probeImageDimensions(out.str());
        REQUIRE(dims.has_value());
        CHECK(dims->width == src.width);
        CHECK(dims->height == src.height);
    }
    if (io::webpSupported()) {
        TempFile out("save", ".webp");
        io::WebpSaveOptions opts;
        opts.lossless = true;
        opts.exact = true;
        REQUIRE_MESSAGE(io::saveWebp(src, out.str(), opts, &err), err);
        const auto back = io::loadImage(out.str(), &err);
        REQUIRE_MESSAGE(back.has_value(), err);
        CHECK(*back == src);

        const auto dims = io::probeImageDimensions(out.str());
        REQUIRE(dims.has_value());
        CHECK(dims->width == src.width);
        CHECK(dims->height == src.height);
    }
    if (io::tiffSupported()) {
        TempFile out("save", ".tif");
        io::TiffSaveOptions opts;
        // Explicitly uncompressed: the struct's own default is Deflate, and this case is about
        // the path plumbing, not about which codecs this libtiff happens to carry.
        opts.compression = io::TiffSaveOptions::Compression::None;
        REQUIRE_MESSAGE(io::saveTiff(src, out.str(), opts, &err), err);
        const auto back = io::loadImage(out.str(), &err);
        REQUIRE_MESSAGE(back.has_value(), err);
        CHECK(*back == src);
        // TIFF deliberately has no header-only dimension probe (io/io.hpp explains why).
        CHECK_FALSE(io::probeImageDimensions(out.str()).has_value());
    }

    // An unwritable path fails with a reason, whatever the codec.
    if (io::gifSupported()) {
        err.clear();
        CHECK_FALSE(io::saveGif(src, "/no/such/directory/x.gif", {}, &err));
        CHECK_FALSE(err.empty());
    }
}

TEST_CASE("the M4 backends are registered, findable, and honest about availability") {
    const io::FormatRegistry& registry = io::FormatRegistry::instance();

    struct Row {
        io::FormatId id;
        const char* extension;
        bool supported;
    };
    const Row rows[] = {
        {io::FormatId::WebP, "webp", io::webpSupported()},
        {io::FormatId::Avif, "avif", io::avifSupported()},
        {io::FormatId::Tiff, "tiff", io::tiffSupported()},
        {io::FormatId::Gif, "gif", io::gifSupported()},
    };
    for (const Row& row : rows) {
        const io::FormatBackend* backend = registry.find(row.id);
        REQUIRE(backend != nullptr);  // registration is unconditional
        CAPTURE(row.extension);
        CHECK(backend->available() == row.supported);
        CHECK(registry.findByExtension(row.extension) == backend);
        CHECK(backend->tier() == io::FormatTier::Common);
        CHECK_FALSE(backend->extensions().empty());
        CHECK(io::validateSchema(backend->optionsSchema()).empty());
    }
    // The second TIFF extension resolves to the same backend.
    CHECK(registry.findByPath("/tmp/scan.tif") == registry.find(io::FormatId::Tiff));
    CHECK(registry.findByPath("/tmp/photo.WEBP") == registry.find(io::FormatId::WebP));
}

TEST_CASE("the M4 caps rows drive the loss banner truthfully") {
    const io::FormatRegistry& registry = io::FormatRegistry::instance();

    // GIF: indexed, one-bit alpha, no colour profile. A truecolour document with soft edges is
    // the case the red banner exists for.
    const io::FormatBackend* gif = registry.find(io::FormatId::Gif);
    REQUIRE(gif != nullptr);
    const io::FormatCaps gifCaps = gif->caps();
    CHECK(gifCaps.alpha == io::AlphaKind::Binary);
    CHECK(gifCaps.maxColors == 256);
    CHECK_FALSE(gifCaps.icc);

    io::DocumentProfile doc;
    doc.hasAlpha = true;
    doc.distinctColors = -1;  // truecolour
    doc.hasICC = true;
    const std::vector<io::LossWarning> warnings =
        io::diff(doc, gifCaps, gif->optionsSchema().defaults());
    CHECK(io::worstSeverity(warnings) == io::Severity::HardLoss);
    bool binaryAlpha = false;
    bool quantized = false;
    bool iccDropped = false;
    for (const io::LossWarning& w : warnings) {
        binaryAlpha = binaryAlpha || w.code == io::LossCode::AlphaReducedToBinary;
        quantized = quantized || w.code == io::LossCode::ColorsQuantized;
        iccDropped = iccDropped || w.code == io::LossCode::IccDropped;
    }
    CHECK(binaryAlpha);
    CHECK(quantized);
    CHECK(iccDropped);

    // TIFF: lossless in every mode we offer, so its defaults must produce NO encode warning.
    const io::FormatBackend* tiff = registry.find(io::FormatId::Tiff);
    REQUIRE(tiff != nullptr);
    CHECK(tiff->caps().lossless);
    CHECK_FALSE(tiff->caps().lossy);
    CHECK(io::encodeIsLossless(tiff->caps(), tiff->optionsSchema().defaults()));

    // WebP: the near-lossless knob is the one thing that makes a "lossless" encode lossy, and
    // io/caps.cpp has to know it by name or the banner would promise exactness it does not have.
    const io::FormatBackend* webp = registry.find(io::FormatId::WebP);
    REQUIRE(webp != nullptr);
    io::OptionValues values = webp->optionsSchema().defaults();
    values.set(io::kOptLossless, io::boolValue(true));
    CHECK(io::encodeIsLossless(webp->caps(), values));
    values.set(io::kOptNearLossless, io::intValue(60));
    CHECK_FALSE(io::encodeIsLossless(webp->caps(), values));

    // AVIF: the subsampling ratio is remembered while hidden, and a lossless encode must not be
    // accused of subsampling because of a value the panel is not even showing.
    const io::FormatBackend* avif = registry.find(io::FormatId::Avif);
    REQUIRE(avif != nullptr);
    io::OptionValues avifValues = avif->optionsSchema().defaults();
    CHECK(avifValues.text(io::kOptSubsampling) == "4:2:0");
    avifValues.set(io::kOptLossless, io::boolValue(true));
    CHECK_FALSE(avif->optionsSchema().visible(io::kOptSubsampling, avifValues));
    const std::vector<io::LossWarning> avifWarnings =
        io::diff(io::DocumentProfile{}, avif->caps(), avifValues);
    for (const io::LossWarning& w : avifWarnings)
        CHECK(w.code != io::LossCode::ChromaSubsampled);
}

TEST_CASE("an encode through the registry produces the same bytes as the codec entry point") {
    const io::FormatRegistry& registry = io::FormatRegistry::instance();
    const Image src = poster(12, 9);

    if (io::gifSupported()) {
        const io::FormatBackend* gif = registry.find(io::FormatId::Gif);
        REQUIRE(gif != nullptr);
        io::RenderInput input;
        input.pixels = &src;
        const io::EncodeResult result = gif->encode(input, gif->optionsSchema().defaults());
        REQUIRE_MESSAGE(result.ok, result.error);
        std::string err;
        const auto back = io::decodeImageBytes(result.bytes.data(), result.bytes.size(), &err);
        REQUIRE_MESSAGE(back.has_value(), err);
        CHECK(*back == src);

        TempFile out("registry", ".gif");
        std::string writeError;
        CHECK(io::encodeToFile(*gif, input, gif->optionsSchema().defaults(), out.str(),
                               &writeError));
    }

    // An unavailable backend explains itself instead of producing a broken file.
    for (const io::FormatId id : {io::FormatId::WebP, io::FormatId::Avif, io::FormatId::Tiff,
                                  io::FormatId::Gif}) {
        const io::FormatBackend* backend = registry.find(id);
        REQUIRE(backend != nullptr);
        if (backend->available())
            continue;
        io::RenderInput input;
        input.pixels = &src;
        const io::EncodeResult result = backend->encode(input, backend->optionsSchema().defaults());
        CHECK_FALSE(result.ok);
        CHECK_FALSE(result.error.empty());
    }
}
