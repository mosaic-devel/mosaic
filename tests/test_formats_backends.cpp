#include "common/image.hpp"
#include "formats/bmp.hpp"
#include "formats/formats.hpp"
#include "formats/hdr.hpp"
#include "formats/ico.hpp"
#include "formats/pnm.hpp"
#include "formats/qoi.hpp"
#include "formats/tga.hpp"
#include "io/caps.hpp"
#include "io/format_registry.hpp"
#include "io/io.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The M5 ADAPTER layer: the six curated-pro FormatBackends that front libmosaicformats
// (src/io/backends/mosaicformats_backend.cpp). Unlike tests/test_formats_*.cpp -- which exercise the
// library standalone, with no Mosaic dependency -- this file is deliberately about the seam: the
// registry entries, the capability rows the loss banner reads, the options each schema really
// honours, and the common::Image <-> mosaicfmt::ImageView conversion.
namespace {

namespace io = mosaic::io;
using mosaic::common::Color8;
using mosaic::common::Image;

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

// Encode `id` at its schema defaults, optionally with a few overrides.
io::EncodeResult encodeWith(io::FormatId id, const Image& image,
                            const std::vector<std::pair<std::string, io::OptionValue>>& overrides =
                                {}) {
    const io::FormatBackend* backend = io::FormatRegistry::instance().find(id);
    REQUIRE(backend != nullptr);
    io::OptionValues values = backend->optionsSchema().defaults();
    for (const auto& [key, value] : overrides)
        values.set(key, value);
    backend->optionsSchema().coerce(values);
    io::RenderInput input;
    input.pixels = &image;
    input.dpi = 96.0;
    input.matte = Color8{7, 8, 9, 255};
    return backend->encode(input, values);
}

int maxDelta(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size())
        return 1000;
    int worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
        worst = std::max(worst, d < 0 ? -d : d);
    }
    return worst;
}

} // namespace

TEST_CASE("the six curated-pro backends are registered and answer for themselves") {
    const io::FormatRegistry& reg = io::FormatRegistry::instance();
    const io::FormatId ids[6] = {io::FormatId::Bmp, io::FormatId::Tga, io::FormatId::Pnm,
                                 io::FormatId::Qoi, io::FormatId::Ico, io::FormatId::RadianceHdr};
    for (const io::FormatId id : ids) {
        const io::FormatBackend* b = reg.find(id);
        REQUIRE(b != nullptr);
        CHECK(b->tier() == io::FormatTier::CuratedPro);
        // None of these is build-optional: the codecs are ours, so there is no probe to fail.
        CHECK(b->available());
        CHECK_FALSE(b->extensions().empty());
        CHECK_FALSE(b->extensions()[0].empty());
        CHECK_FALSE(std::string(b->mimeType()).empty());
        CHECK(io::validateSchema(b->optionsSchema()).empty());
    }

    // Every extension the family claims resolves back to its own backend.
    CHECK(reg.findByExtension("bmp") == reg.find(io::FormatId::Bmp));
    CHECK(reg.findByExtension(".DIB") == reg.find(io::FormatId::Bmp));
    CHECK(reg.findByExtension("tga") == reg.find(io::FormatId::Tga));
    CHECK(reg.findByExtension("ppm") == reg.find(io::FormatId::Pnm));
    CHECK(reg.findByExtension("pam") == reg.find(io::FormatId::Pnm));
    CHECK(reg.findByExtension("pbm") == reg.find(io::FormatId::Pnm));
    CHECK(reg.findByExtension("qoi") == reg.find(io::FormatId::Qoi));
    CHECK(reg.findByExtension("ico") == reg.find(io::FormatId::Ico));
    CHECK(reg.findByExtension("hdr") == reg.find(io::FormatId::RadianceHdr));
    CHECK(reg.findByPath("/tmp/whatever.qoi") == reg.find(io::FormatId::Qoi));
}

TEST_CASE("the curated tier sits below the Common divider, alphabetically") {
    const std::vector<const io::FormatBackend*> order =
        io::FormatRegistry::instance().exportOrder(/*includeExotic=*/false);
    std::size_t firstCurated = order.size();
    for (std::size_t i = 0; i < order.size(); ++i)
        if (order[i]->tier() != io::FormatTier::Common) {
            firstCurated = i;
            break;
        }
    REQUIRE(firstCurated < order.size());
    // Nothing Common may appear after the first curated entry...
    for (std::size_t i = firstCurated; i < order.size(); ++i)
        CHECK(order[i]->tier() != io::FormatTier::Common);
    // ... and the curated run is sorted by display name.
    for (std::size_t i = firstCurated + 1; i < order.size(); ++i)
        CHECK(order[i - 1]->displayName() <= order[i]->displayName());
    // All six are offered, since none of them can be unavailable.
    CHECK(order.size() - firstCurated >= 6);
}

TEST_CASE("the curated caps rows say exactly what their encoders write") {
    const io::FormatRegistry& reg = io::FormatRegistry::instance();

    const io::FormatCaps bmp = reg.find(io::FormatId::Bmp)->caps();
    CHECK(bmp.alpha == io::AlphaKind::Straight);  // the DEFAULT is 32-bit V5
    CHECK(bmp.icc);                               // V5's embedded profile
    CHECK(has(bmp.metadata, io::MetadataKind::Dpi));
    CHECK(bmp.lossless);
    CHECK_FALSE(bmp.lossy);

    // TGA is the reason AlphaKind has an `Either` arm: the file itself records which convention
    // its alpha follows, in the v2 extension area.
    CHECK(reg.find(io::FormatId::Tga)->caps().alpha == io::AlphaKind::Either);

    // PNM's default variant is PPM, which cannot carry alpha -- and the caps row states the
    // default, never the best case (docs/formats-curated.md).
    CHECK(reg.find(io::FormatId::Pnm)->caps().alpha == io::AlphaKind::None);

    CHECK(reg.find(io::FormatId::Qoi)->caps().alpha == io::AlphaKind::Straight);
    CHECK_FALSE(reg.find(io::FormatId::Qoi)->caps().icc);  // the colourspace byte is a tag, not a profile

    // ⚠ The honest one: Radiance HDR is a high-dynamic-range format fed by an 8-bit pipeline
    // (§5's high-bit note), so its caps row claims neither float samples nor more than 8 bits.
    const io::FormatCaps hdr = reg.find(io::FormatId::RadianceHdr)->caps();
    CHECK(hdr.maxBitDepth == 8);
    CHECK_FALSE(hdr.floatPixels);
    CHECK(hdr.alpha == io::AlphaKind::None);

    for (const io::FormatId id : {io::FormatId::Tga, io::FormatId::Pnm, io::FormatId::Qoi,
                                  io::FormatId::Ico, io::FormatId::RadianceHdr})
        CHECK_FALSE(reg.find(id)->caps().icc);  // BMP V5 is the only one with room for a profile
}

TEST_CASE("the loss banner reads the curated rows the way a user would expect") {
    io::DocumentProfile doc;
    doc.hasAlpha = true;
    doc.layerCount = 1;

    const auto codes = [&](io::FormatId id) {
        const io::FormatBackend* b = io::FormatRegistry::instance().find(id);
        std::vector<io::LossCode> out;
        for (const io::LossWarning& w : io::diff(doc, b->caps(), b->optionsSchema().defaults()))
            out.push_back(w.code);
        return out;
    };
    const auto mentions = [](const std::vector<io::LossCode>& list, io::LossCode code) {
        return std::find(list.begin(), list.end(), code) != list.end();
    };

    // The two that cannot carry transparency at all must say so...
    CHECK(mentions(codes(io::FormatId::RadianceHdr), io::LossCode::AlphaDropped));
    CHECK(mentions(codes(io::FormatId::Pnm), io::LossCode::AlphaDropped));
    // ... and the ones that can must not.
    CHECK_FALSE(mentions(codes(io::FormatId::Qoi), io::LossCode::AlphaDropped));
    CHECK_FALSE(mentions(codes(io::FormatId::Bmp), io::LossCode::AlphaDropped));
    CHECK_FALSE(mentions(codes(io::FormatId::Ico), io::LossCode::AlphaDropped));
    // Nothing curated is a lossy encode.
    for (const io::FormatId id : {io::FormatId::Bmp, io::FormatId::Tga, io::FormatId::Pnm,
                                  io::FormatId::Qoi, io::FormatId::Ico, io::FormatId::RadianceHdr})
        CHECK(io::encodeIsLossless(io::FormatRegistry::instance().find(id)->caps(),
                                   io::FormatRegistry::instance().find(id)->optionsSchema().defaults()));
}

TEST_CASE("each curated backend encodes a file its own decoder reads back") {
    const Image src = pattern(23, 9);

    SUBCASE("BMP, at the default 32-bit V5") {
        const io::EncodeResult r = encodeWith(io::FormatId::Bmp, src);
        REQUIRE(r.ok);
        const auto back = mosaicfmt::decodeBmp(r.bytes.data(), r.bytes.size());
        REQUIRE(back.has_value());
        CHECK(back->rgba == src.rgba);
    }
    SUBCASE("BMP, 8-bit, palettised by io::quantize and run-length coded") {
        // The palette comes from Mosaic's quantizer, not from the codec -- this is the one option
        // where the adapter does real work rather than translating an enum.
        const io::EncodeResult r =
            encodeWith(io::FormatId::Bmp, opaquePattern(16, 8),
                       {{"depth", io::textValue("8")}, {"rle", io::boolValue(true)}});
        REQUIRE(r.ok);
        const auto back = mosaicfmt::decodeBmp(r.bytes.data(), r.bytes.size());
        REQUIRE(back.has_value());
        CHECK(back->width == 16);
        CHECK(back->height == 8);
        for (std::size_t i = 3; i < back->rgba.size(); i += 4)
            REQUIRE(back->rgba[i] == 255);  // an indexed BMP is opaque by construction
    }
    SUBCASE("TGA") {
        const io::EncodeResult r = encodeWith(io::FormatId::Tga, src);
        REQUIRE(r.ok);
        const auto back = mosaicfmt::decodeTga(r.bytes.data(), r.bytes.size());
        REQUIRE(back.has_value());
        CHECK(back->rgba == src.rgba);
    }
    SUBCASE("PNM, at its PPM default and as PAM") {
        const io::EncodeResult ppm = encodeWith(io::FormatId::Pnm, src);
        REQUIRE(ppm.ok);
        CHECK(ppm.bytes[1] == '6');
        const io::EncodeResult pam =
            encodeWith(io::FormatId::Pnm, src, {{"variant", io::textValue("pam")}});
        REQUIRE(pam.ok);
        CHECK(pam.bytes[1] == '7');
        const auto back = mosaicfmt::decodePnm(pam.bytes.data(), pam.bytes.size());
        REQUIRE(back.has_value());
        CHECK(back->rgba == src.rgba);  // PAM is the variant that keeps the alpha channel
    }
    SUBCASE("QOI") {
        const io::EncodeResult r = encodeWith(io::FormatId::Qoi, src);
        REQUIRE(r.ok);
        const auto back = mosaicfmt::decodeQoi(r.bytes.data(), r.bytes.size());
        REQUIRE(back.has_value());
        CHECK(back->rgba == src.rgba);
    }
    SUBCASE("Radiance HDR") {
        const Image grey = [] {
            Image img(16, 4);
            for (std::uint32_t y = 0; y < 4; ++y)
                for (std::uint32_t x = 0; x < 16; ++x) {
                    const std::size_t p = (static_cast<std::size_t>(y) * 16 + x) * 4;
                    img.rgba[p] = img.rgba[p + 1] = img.rgba[p + 2] =
                        static_cast<std::uint8_t>(x * 17);
                    img.rgba[p + 3] = 255;
                }
            return img;
        }();
        const io::EncodeResult r = encodeWith(io::FormatId::RadianceHdr, grey);
        REQUIRE(r.ok);
        const auto back = mosaicfmt::decodeHdr(r.bytes.data(), r.bytes.size());
        REQUIRE(back.has_value());
        CHECK(maxDelta(back->rgba, grey.rgba) <= 2);  // see the tolerance note in test_formats_hdr
    }
    SUBCASE("ICO writes its whole size set from one image") {
        const io::EncodeResult r = encodeWith(io::FormatId::Ico, opaquePattern(256, 256));
        REQUIRE(r.ok);
        // The default set is 16, 32, 48 and 256 -- four directory slots off one exported picture.
        CHECK((static_cast<std::uint32_t>(r.bytes[4]) |
               (static_cast<std::uint32_t>(r.bytes[5]) << 8)) == 4u);
        const auto chosen = mosaicfmt::selectIcoEntry(r.bytes.data(), r.bytes.size());
        REQUIRE(chosen.has_value());
        CHECK(chosen->width == 256);
    }
}

TEST_CASE("an ICO's PNG payload is encoded up here, where libpng lives") {
    // `payload: auto` uses PNG for the 256-pixel entry, and the PNG comes from io::encodePng --
    // libmosaicformats has no PNG encoder and gains nothing by having one. The decode half of the
    // same seam is io::decodeImageBytes, which reads the extracted payload directly.
    const io::EncodeResult r = encodeWith(io::FormatId::Ico, opaquePattern(256, 256),
                                          {{"sizes", io::textValue("256")}});
    REQUIRE(r.ok);
    const auto chosen = mosaicfmt::selectIcoEntry(r.bytes.data(), r.bytes.size());
    REQUIRE(chosen.has_value());
    CHECK(chosen->isPng);

    const std::vector<std::uint8_t> payload(
        r.bytes.begin() + static_cast<std::ptrdiff_t>(chosen->offset),
        r.bytes.begin() + static_cast<std::ptrdiff_t>(chosen->offset + chosen->size));
    std::string error;
    const auto decoded = io::decodeImageBytes(payload.data(), payload.size(), &error);
    REQUIRE(decoded.has_value());
    CHECK(decoded->width == 256);
    CHECK(decoded->height == 256);

    // And forcing bitmaps produces a directory of DIBs instead.
    const io::EncodeResult dib = encodeWith(io::FormatId::Ico, opaquePattern(256, 256),
                                            {{"sizes", io::textValue("256")},
                                             {"payload", io::textValue("bmp")}});
    REQUIRE(dib.ok);
    const auto dibEntry = mosaicfmt::selectIcoEntry(dib.bytes.data(), dib.bytes.size());
    REQUIRE(dibEntry.has_value());
    CHECK_FALSE(dibEntry->isPng);
}

TEST_CASE("an empty image is refused rather than written") {
    const Image empty;
    for (const io::FormatId id : {io::FormatId::Bmp, io::FormatId::Tga, io::FormatId::Pnm,
                                  io::FormatId::Qoi, io::FormatId::Ico, io::FormatId::RadianceHdr}) {
        const io::FormatBackend* b = io::FormatRegistry::instance().find(id);
        REQUIRE(b != nullptr);
        io::RenderInput input;
        input.pixels = &empty;
        const io::EncodeResult r = b->encode(input, b->optionsSchema().defaults());
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }
}

// ⚠ THIS CASE DEPENDS ON THE io.cpp / detail.hpp DISPATCH PATCH (see the M5 report): the curated
// formats have to be OPENABLE, not only writable, and the one line that makes that true lives in
// io::decodeSniffed. Without it this is the case that fails, and it fails loudly rather than
// leaving the six formats quietly export-only.
TEST_CASE("the curated formats can be opened again through io's own dispatch") {
    const Image src = pattern(13, 7);
    struct Case {
        io::FormatId id;
        std::vector<std::pair<std::string, io::OptionValue>> overrides;
        bool exact;
    };
    const std::vector<Case> cases = {
        {io::FormatId::Qoi, {}, true},
        {io::FormatId::Bmp, {}, true},
        {io::FormatId::Tga, {}, true},
        {io::FormatId::Pnm, {{"variant", io::textValue("pam")}}, true},
        {io::FormatId::Ico, {{"sizes", io::textValue("source")}}, false},
        {io::FormatId::RadianceHdr, {}, false},
    };
    for (const Case& c : cases) {
        const io::EncodeResult r = encodeWith(c.id, src, c.overrides);
        REQUIRE(r.ok);
        std::string error;
        const auto decoded = io::decodeImageBytes(r.bytes.data(), r.bytes.size(), &error);
        // Concatenate first: doctest's message builder resolves its own operator+ against the
        // arguments, so a concatenation spelled inline picks the wrong overload.
        const std::string note = std::string(io::formatIdName(c.id)) + ": " + error;
        REQUIRE_MESSAGE(decoded.has_value(), note);
        if (c.exact) {
            CHECK(decoded->width == src.width);
            CHECK(decoded->height == src.height);
            CHECK(decoded->rgba == src.rgba);
        } else {
            CHECK(decoded->rgba.size() == decoded->pixelCount() * 4);
        }
    }
}
