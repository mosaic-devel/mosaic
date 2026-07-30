#include "common/image.hpp"
#include "io/format_registry.hpp"
#include "io/io.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

// The FormatBackend registry (docs/export-system-plan.md §2.1) and the PNG backend that proves
// the shape works. The invariants asserted here are the ones a NEW backend is most likely to
// break, so they are deliberately written over "every registered backend" rather than over PNG.
using namespace mosaic;
using namespace mosaic::io;

namespace {

// A stand-in backend, so the ordering and lookup rules can be exercised over a known set
// without waiting for M4/M5 to land the real ones.
class StubBackend final : public FormatBackend {
public:
    StubBackend(FormatId id, FormatTier tier, const char* name, std::string ext, bool avail)
        : m_id(id), m_tier(tier), m_name(name), m_ext(std::move(ext)), m_available(avail) {}

    [[nodiscard]] FormatId id() const noexcept override { return m_id; }
    [[nodiscard]] FormatTier tier() const noexcept override { return m_tier; }
    [[nodiscard]] std::string_view displayName() const noexcept override { return m_name; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {m_ext}; }
    [[nodiscard]] std::string_view mimeType() const noexcept override { return "image/x-stub"; }
    [[nodiscard]] FormatCaps caps() const override { return {}; }
    [[nodiscard]] OptionsSchema optionsSchema() const override { return {}; }
    [[nodiscard]] bool available() const noexcept override { return m_available; }
    [[nodiscard]] EncodeResult encode(const RenderInput&, const OptionValues&,
                                      const ProgressFn&) const override {
        return EncodeResult::failure("stub");
    }

private:
    FormatId m_id;
    FormatTier m_tier;
    std::string m_name;
    std::string m_ext;
    bool m_available;
};

std::unique_ptr<FormatBackend> stub(FormatId id, FormatTier tier, const char* name,
                                    std::string ext, bool avail = true) {
    return std::make_unique<StubBackend>(id, tier, name, std::move(ext), avail);
}

std::string nameList(const std::vector<const FormatBackend*>& backends) {
    std::string s;
    for (const FormatBackend* b : backends) {
        if (!s.empty())
            s += '|';
        s += b->displayName();
    }
    return s;
}

// A scratch path under the OS temp dir, removed when the test ends (the TempPng pattern from
// tests/test_io.cpp -- tests never write into the source tree).
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const char* stem)
        : path(std::filesystem::temp_directory_path() /
               (std::string("mosaic_test_") + stem + ".png")) {}
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    [[nodiscard]] std::string str() const { return path.string(); }
};

common::Image checker(std::uint32_t w, std::uint32_t h) {
    common::Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            const std::uint8_t v = ((x + y) % 2 == 0) ? 0u : 255u;
            img.rgba[p] = v;
            img.rgba[p + 1] = static_cast<std::uint8_t>(x * 7);
            img.rgba[p + 2] = static_cast<std::uint8_t>(y * 11);
            img.rgba[p + 3] = static_cast<std::uint8_t>(x == 0 ? 128 : 255);
        }
    return img;
}

} // namespace

TEST_CASE("the shipping registry holds the PNG backend and nothing half-built") {
    const FormatRegistry& reg = FormatRegistry::instance();
    REQUIRE(reg.size() >= 1);

    const FormatBackend* png = reg.find(FormatId::Png);
    REQUIRE(png != nullptr);
    CHECK(png->tier() == FormatTier::Common);
    CHECK(png->displayName() == "PNG image");
    CHECK(png->mimeType() == "image/png");
    REQUIRE(png->extensions().size() == 1);
    CHECK(png->extensions()[0] == "png");
    CHECK(png->available());

    // Nothing is registered twice, and every id resolves back to itself.
    std::set<std::string> names;
    for (const FormatBackend* b : reg.all()) {
        CHECK(reg.find(b->id()) == b);
        CHECK_FALSE(std::string(b->displayName()).empty());
        CHECK(names.insert(std::string(formatIdName(b->id()))).second);
        CHECK_FALSE(b->extensions().empty());
        CHECK_FALSE(b->extensions()[0].empty());
    }

    // M4 built AVIF, so the old "absent until someone builds it" pin now asserts the opposite --
    // and asserts the thing that actually matters about this backend: it is registered, and it
    // answers honestly when the system libavif carries no encoder Mosaic will use (never rav1e),
    // rather than being a null-dereferencing placeholder.
    const FormatBackend* avif = reg.find(FormatId::Avif);
    REQUIRE(avif != nullptr);
    CHECK_FALSE(std::string(avif->displayName()).empty());
    CHECK_FALSE(avif->extensions().empty());
}

// M3 adapted the two encoders that already shipped (io::encodeJpeg / io::encodeJxl) into backends
// so the Export modal could stop driving codecs directly. They are adapters, not new codecs -- the
// rest of M4's format list is still absent above.
TEST_CASE("the JPEG and JPEG XL adapters are registered and honest about themselves") {
    const FormatRegistry& reg = FormatRegistry::instance();

    const FormatBackend* jpeg = reg.find(FormatId::Jpeg);
    REQUIRE(jpeg != nullptr);
    CHECK(jpeg->available());
    CHECK(jpeg->extensions()[0] == "jpg");
    const FormatCaps jc = jpeg->caps();
    CHECK(jc.alpha == AlphaKind::None); // which is what makes the modal show its Matte row
    CHECK(jc.chromaSubsampling);
    // The honest half (§4.1): JPEG has NO lossless mode, not even at quality 100.
    CHECK_FALSE(jc.lossless);
    CHECK(jc.lossy);
    OptionValues q100 = jpeg->optionsSchema().defaults();
    q100.set(kOptQuality, intValue(100));
    CHECK_FALSE(encodeIsLossless(jc, q100));

    const FormatBackend* jxl = reg.find(FormatId::Jxl);
    REQUIRE(jxl != nullptr);
    CHECK(jxl->available() == io::jxlSupported()); // an optional build dependency
    const FormatCaps xc = jxl->caps();
    CHECK(xc.lossless);
    CHECK(xc.lossy);
    CHECK(xc.alpha == AlphaKind::Straight);
    // The shared option vocabulary is what lets the format-agnostic diff() read these at all.
    OptionValues lossless = jxl->optionsSchema().defaults();
    CHECK_FALSE(encodeIsLossless(xc, lossless)); // distance 1.0 by default
    lossless.set(kOptLossless, boolValue(true));
    CHECK(encodeIsLossless(xc, lossless));
}

TEST_CASE("JXL's distance is hidden by its lossless checkbox -- as DATA, not panel code") {
    const FormatBackend* jxl = FormatRegistry::instance().find(FormatId::Jxl);
    REQUIRE(jxl != nullptr);
    const OptionsSchema schema = jxl->optionsSchema();
    OptionValues values = schema.defaults();
    CHECK(schema.visible(kOptDistance, values));
    values.set(kOptLossless, boolValue(true));
    CHECK_FALSE(schema.visible(kOptDistance, values));
    // Hiding is a UI affordance only: the value stays in the bag, so unchecking restores it.
    CHECK(values.number(kOptDistance) == doctest::Approx(1.0));
}

TEST_CASE("the JPEG backend encodes through the registry at the chosen quality") {
    const FormatBackend* jpeg = FormatRegistry::instance().find(FormatId::Jpeg);
    REQUIRE(jpeg != nullptr);

    const common::Image source = checker(64, 48);
    RenderInput input;
    input.pixels = &source;

    const OptionsSchema schema = jpeg->optionsSchema();
    OptionValues low = schema.defaults();
    low.set(kOptQuality, intValue(20));
    OptionValues high = schema.defaults();
    high.set(kOptQuality, intValue(95));

    const EncodeResult lo = jpeg->encode(input, low);
    const EncodeResult hi = jpeg->encode(input, high);
    REQUIRE(lo.ok);
    REQUIRE(hi.ok);
    REQUIRE(lo.bytes.size() > 3);
    CHECK(lo.bytes[0] == 0xFF); // a real JPEG SOI, not a stub
    CHECK(lo.bytes[1] == 0xD8);
    CHECK(lo.bytes.size() < hi.bytes.size()); // the quality knob is actually wired
}

TEST_CASE("every registered backend's options schema is structurally sound") {
    for (const FormatBackend* b : FormatRegistry::instance().all()) {
        const OptionsSchema schema = b->optionsSchema();
        const std::vector<std::string> problems = validateSchema(schema);
        for (const std::string& p : problems) {
            // Concatenate first: doctest's message builder resolves its own operator+ against the
            // arguments, so a concatenation spelled inline picks the wrong overload.
            const std::string note = std::string(b->displayName()) + ": " + p;
            MESSAGE(note);
        }
        CHECK(problems.empty());

        // defaults() must survive its own coerce untouched -- otherwise the panel opens showing
        // one value and encodes with another.
        OptionValues values = schema.defaults();
        const OptionValues before = values;
        schema.coerce(values);
        CHECK(values == before);
    }
}

TEST_CASE("every registered backend's caps are internally consistent") {
    for (const FormatBackend* b : FormatRegistry::instance().all()) {
        const FormatCaps c = b->caps();
        CHECK((c.lossless || c.lossy));      // a format that can encode nothing is a bug
        CHECK(c.maxBitDepth > 0);
        CHECK(c.channels != ChannelModel::None);
        if (c.maxColors >= 0)
            CHECK(has(c.channels, ChannelModel::Indexed));
        if (!c.vector) {
            CHECK_FALSE(c.conicGradients);   // the three vector flags mean nothing without it
            CHECK_FALSE(c.strokeAlignment);
            CHECK_FALSE(c.blendModes);
        }
    }
}

TEST_CASE("PNG caps describe what the encoder writes, not what the container permits") {
    const FormatBackend* png = FormatRegistry::instance().find(FormatId::Png);
    REQUIRE(png != nullptr);
    const FormatCaps c = png->caps();
    CHECK(c.alpha == AlphaKind::Straight);
    CHECK(c.lossless);
    CHECK_FALSE(c.lossy);
    CHECK(c.maxColors == -1);
    CHECK_FALSE(c.layers);
    CHECK_FALSE(c.vector);
    // The honest half: io::encodePng writes 8-bit colour-type-6 only, no APNG, no iCCP/eXIf/pHYs.
    // These flip on as the encoder grows (the >8-bit tap, M4 metadata) -- and until they do, the
    // loss banner tells the user the truth.
    CHECK(c.maxBitDepth == 8);
    CHECK_FALSE(c.floatPixels);
    CHECK_FALSE(c.animation);
    // M4 grew the PNG encoder an iCCP/eXIf/pHYs writer, so these two flipped exactly as the
    // comment above predicted. The claim has to track the encoder or the loss banner lies in the
    // other direction -- promising a profile will be dropped that is in fact written.
    CHECK(c.icc);
    CHECK(c.metadata == (MetadataKind::Exif | MetadataKind::Dpi));
}

TEST_CASE("the PNG backend encodes through the registry, honouring its schema") {
    const FormatBackend* png = FormatRegistry::instance().find(FormatId::Png);
    REQUIRE(png != nullptr);

    const common::Image source = checker(64, 48);
    RenderInput input;
    input.pixels = &source;

    const OptionsSchema schema = png->optionsSchema();
    OptionValues values = schema.defaults();
    CHECK(values.integer("compression") == 6);
    CHECK_FALSE(values.boolean("interlace"));

    const EncodeResult r = png->encode(input, values);
    REQUIRE(r.ok);
    CHECK(r.error.empty());
    REQUIRE(r.bytes.size() > 8);
    // A real PNG signature, not a stub.
    CHECK(r.bytes[0] == 0x89);
    CHECK(r.bytes[1] == 'P');
    CHECK(r.bytes[2] == 'N');
    CHECK(r.bytes[3] == 'G');

    // PNG is lossless: the bytes the backend produced decode back to the exact source pixels.
    const TempFile out("registry_roundtrip");
    REQUIRE(encodeToFile(*png, input, values, out.str()));
    const std::optional<common::Image> reloaded = io::loadImage(out.str());
    REQUIRE(reloaded.has_value());
    CHECK(*reloaded == source);

    SUBCASE("the options actually reach libpng") {
        OptionValues stored = values;
        stored.set("compression", intValue(0)); // no deflate at all
        const EncodeResult uncompressed = png->encode(input, stored);
        REQUIRE(uncompressed.ok);
        CHECK(uncompressed.bytes.size() > r.bytes.size());

        OptionValues interlaced = values;
        interlaced.set("interlace", boolValue(true));
        const EncodeResult adam7 = png->encode(input, interlaced);
        REQUIRE(adam7.ok);
        CHECK(adam7.bytes != r.bytes); // a different file, same picture
    }

    SUBCASE("an out-of-range value is clamped, not passed through to libpng") {
        OptionValues wild;
        wild.set("compression", intValue(500));
        schema.coerce(wild);
        CHECK(wild.integer("compression") == 9);
        CHECK(png->encode(input, wild).ok);
    }

    SUBCASE("an empty or missing image fails cleanly instead of crashing") {
        RenderInput nothing;
        CHECK_FALSE(png->encode(nothing, values).ok);
        CHECK_FALSE(png->encode(nothing, values).error.empty());

        const common::Image blank;
        RenderInput empty;
        empty.pixels = &blank;
        CHECK_FALSE(png->encode(empty, values).ok);
    }

    SUBCASE("a progress callback that returns false cancels the encode") {
        bool called = false;
        const EncodeResult cancelled = png->encode(input, values, [&](float) {
            called = true;
            return false;
        });
        CHECK(called);
        CHECK_FALSE(cancelled.ok);
        CHECK(cancelled.bytes.empty());
    }
}

TEST_CASE("lookup by extension and by path") {
    const FormatRegistry& reg = FormatRegistry::instance();
    const FormatBackend* png = reg.find(FormatId::Png);
    REQUIRE(png != nullptr);

    CHECK(reg.findByExtension("png") == png);
    CHECK(reg.findByExtension(".png") == png);
    CHECK(reg.findByExtension(".PNG") == png);
    CHECK(reg.findByExtension("PnG") == png);
    CHECK(reg.findByExtension("") == nullptr);
    CHECK(reg.findByExtension(".") == nullptr);
    // M3 adapted JPEG (both spellings) and JXL; the rest of M4's list is still unbuilt.
    CHECK(reg.findByExtension("jpg") == reg.find(FormatId::Jpeg));
    CHECK(reg.findByExtension("jpeg") == reg.find(FormatId::Jpeg));
    CHECK(reg.findByExtension("webp") == reg.find(FormatId::WebP)); // M4 built it

    CHECK(reg.findByPath("/home/a/b/sunset.PNG") == png);
    CHECK(reg.findByPath("sunset.png") == png);
    CHECK(reg.findByPath("no-extension") == nullptr);
    CHECK(reg.findByPath("/a.dir/file") == nullptr); // the dot is not in the last component

    CHECK(extensionOf("a/b/c.tar.gz") == "gz");
    CHECK(extensionOf("a/b/c") == "");
    CHECK(extensionOf("/a.dir/c") == "");
    CHECK(extensionOf(".hidden") == "hidden");
}

TEST_CASE("the combobox order is Common-by-popularity, then everything else alphabetically") {
    FormatRegistry reg;
    // Registered deliberately out of order, and out of tier.
    CHECK(reg.add(stub(FormatId::Qoi, FormatTier::CuratedPro, "QOI image", "qoi")));
    CHECK(reg.add(stub(FormatId::Svg, FormatTier::Common, "SVG document", "svg")));
    CHECK(reg.add(stub(FormatId::Bmp, FormatTier::CuratedPro, "BMP image", "bmp")));
    CHECK(reg.add(stub(FormatId::Jpeg, FormatTier::Common, "JPEG image", "jpg")));
    CHECK(reg.add(stub(FormatId::Png, FormatTier::Common, "PNG image", "png")));
    // The tier is the BACKEND's declaration, not the id's -- an exotic entry needs no new id.
    CHECK(reg.add(stub(FormatId::Ico, FormatTier::Exotic, "ICO image", "ico")));

    // A duplicate id is refused, and does not displace the incumbent.
    CHECK_FALSE(reg.add(stub(FormatId::Png, FormatTier::Exotic, "Impostor", "png")));
    CHECK(reg.size() == 6);
    CHECK(reg.find(FormatId::Png)->displayName() == "PNG image");

    CHECK(nameList(reg.exportOrder(/*includeExotic=*/false)) ==
          "PNG image|JPEG image|SVG document|BMP image|QOI image");
    CHECK(nameList(reg.exportOrder(/*includeExotic=*/true)) ==
          "PNG image|JPEG image|SVG document|BMP image|ICO image|QOI image");

    // all() keeps registration order; exportOrder() does not.
    CHECK(nameList(reg.all()) ==
          "QOI image|SVG document|BMP image|JPEG image|PNG image|ICO image");
}

TEST_CASE("an unavailable backend is not offered for export but is still findable") {
    FormatRegistry reg;
    CHECK(reg.add(stub(FormatId::Png, FormatTier::Common, "PNG image", "png")));
    CHECK(reg.add(stub(FormatId::Jxl, FormatTier::Common, "JPEG XL image", "jxl",
                       /*avail=*/false)));

    CHECK(reg.size() == 2);
    CHECK(reg.find(FormatId::Jxl) != nullptr); // the codec probe failed, the backend still exists
    CHECK(nameList(reg.exportOrder(true)) == "PNG image");
}

TEST_CASE("encodeToFile reports a bad path instead of silently succeeding") {
    const FormatBackend* png = FormatRegistry::instance().find(FormatId::Png);
    REQUIRE(png != nullptr);
    const common::Image source = checker(4, 4);
    RenderInput input;
    input.pixels = &source;

    std::string error;
    CHECK_FALSE(encodeToFile(*png, input, png->optionsSchema().defaults(),
                             "/nonexistent-directory-for-mosaic/out.png", &error));
    CHECK_FALSE(error.empty());
}
