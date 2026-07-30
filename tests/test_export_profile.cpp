#include "common/image.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/text/text_model.hpp"
#include "core/vector/object.hpp"
#include "io/document_profile.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <string>

// DocumentProfile extraction (docs/export-system-plan.md §4): what the document ACTUALLY uses,
// probed from the real model -- not the hand-built structs the diff() goldens use. This is the
// half that goes stale when the model moves, so every field is driven from a document built the
// way the app builds one.
using namespace mosaic;
using namespace mosaic::io;

namespace {

core::Document makeDoc(std::uint32_t w = 8, std::uint32_t h = 8) {
    return core::Document(w, h, core::ColorSpace::SRGB, core::Precision::U8);
}

// A raster layer whose pixels are `fill` everywhere, sized to the canvas.
std::unique_ptr<core::RasterLayer> rasterLayer(core::Document& doc, const char* name,
                                               common::Color8 fill) {
    auto layer = std::make_unique<core::RasterLayer>(doc.mintLayerId(), name, doc.width(),
                                                     doc.height());
    layer->image().fill(fill);
    return layer;
}

const common::Color8 kOpaque{10, 20, 30, 255};
const common::Color8 kTranslucent{10, 20, 30, 128};

} // namespace

TEST_CASE("an empty document profiles as nothing-in-use") {
    core::Document doc = makeDoc();
    const DocumentProfile p = profileDocument(doc);

    CHECK(p.layerCount == 0);
    CHECK_FALSE(p.hasMultipleLayers);
    CHECK_FALSE(p.hasVector);
    CHECK_FALSE(p.hasText);
    CHECK_FALSE(p.hasEffects);
    CHECK_FALSE(p.hasExtrude3d);
    CHECK_FALSE(p.hasAdjustments);
    CHECK_FALSE(p.usesBlendModes);
    CHECK_FALSE(p.usesSoftMask);
    CHECK_FALSE(p.usesConicGradient);
    CHECK_FALSE(p.usesStrokeAlign);
    CHECK_FALSE(p.hasICC);
    CHECK_FALSE(p.hasNonSrgbSpace);
    CHECK_FALSE(p.hasEXIF);
    CHECK(p.dpi == doctest::Approx(72.0));
    CHECK(p.distinctColors == -1); // never counted without a flatten
    // Nothing covers the canvas, so the honest answer about transparency is "yes".
    CHECK(p.hasAlpha);
    // The pipeline still hands the encoder 8-bit pixels whatever the document declares.
    CHECK(p.sourceBitDepth == 8);
    CHECK_FALSE(p.sourceIsFloat);
}

TEST_CASE("the layer count walks the whole tree, groups and hidden layers included") {
    core::Document doc = makeDoc();
    auto group = std::make_unique<core::GroupLayer>(doc.mintLayerId(), "Group");
    group->addOnTop(rasterLayer(doc, "inner a", kOpaque));
    auto hidden = rasterLayer(doc, "inner b", kOpaque);
    hidden->setVisible(false); // an export drops it entirely -- it still counts as a loss
    group->addOnTop(std::move(hidden));
    doc.root().addOnTop(std::move(group));
    doc.root().addOnTop(rasterLayer(doc, "top", kOpaque));

    const DocumentProfile p = profileDocument(doc);
    CHECK(p.layerCount == 4); // group + 2 children + top
    CHECK(p.hasMultipleLayers);
}

TEST_CASE("hasAlpha: provably opaque means opaque, and nothing else does") {
    SUBCASE("one full-canvas opaque raster layer seals it") {
        core::Document doc = makeDoc();
        doc.root().addOnTop(rasterLayer(doc, "bg", kOpaque));
        CHECK(documentIsProvablyOpaque(doc));
        CHECK_FALSE(profileDocument(doc).hasAlpha);
    }
    SUBCASE("one transparent pixel is enough to un-seal it") {
        core::Document doc = makeDoc();
        auto bg = rasterLayer(doc, "bg", kOpaque);
        bg->image().rgba[3] = 254;
        doc.root().addOnTop(std::move(bg));
        CHECK_FALSE(documentIsProvablyOpaque(doc));
        CHECK(profileDocument(doc).hasAlpha);
    }
    SUBCASE("a translucent layer never seals it") {
        core::Document doc = makeDoc();
        doc.root().addOnTop(rasterLayer(doc, "bg", kTranslucent));
        CHECK_FALSE(documentIsProvablyOpaque(doc));
    }
    SUBCASE("a smaller layer never seals it") {
        core::Document doc = makeDoc(8, 8);
        auto small = std::make_unique<core::RasterLayer>(doc.mintLayerId(), "small", 4, 4);
        small->image().fill(kOpaque);
        doc.root().addOnTop(std::move(small));
        CHECK_FALSE(documentIsProvablyOpaque(doc));
    }
    SUBCASE("layer state that can rewrite alpha disqualifies the seal") {
        core::Document doc = makeDoc();
        auto bg = rasterLayer(doc, "bg", kOpaque);
        core::RasterLayer* raw = bg.get();
        doc.root().addOnTop(std::move(bg));
        REQUIRE(documentIsProvablyOpaque(doc));

        raw->setVisible(false);
        CHECK_FALSE(documentIsProvablyOpaque(doc));
        raw->setVisible(true);

        raw->setOpacity(0.5f);
        CHECK_FALSE(documentIsProvablyOpaque(doc));
        raw->setOpacity(1.0f);

        raw->setBlendMode(core::BlendMode::Multiply);
        CHECK_FALSE(documentIsProvablyOpaque(doc));
        raw->setBlendMode(core::BlendMode::Normal);

        raw->setMask(core::RasterMask(doc.width(), doc.height(), 255));
        CHECK_FALSE(documentIsProvablyOpaque(doc));
        raw->clearMask();

        core::LayerEffects fx;
        fx.fillOpacity = 0.4f; // dims the layer's OWN alpha
        raw->setEffects(fx);
        CHECK_FALSE(documentIsProvablyOpaque(doc));
        raw->clearEffects();

        raw->setTransform(common::Affine2D::translation(3.0, 0.0));
        CHECK_FALSE(documentIsProvablyOpaque(doc));
        raw->setTransform(common::Affine2D::identity());

        CHECK(documentIsProvablyOpaque(doc)); // every knob restored
    }
    SUBCASE("a sealing layer anywhere in the stack settles it, because alpha only grows") {
        core::Document doc = makeDoc();
        doc.root().addOnTop(rasterLayer(doc, "bg", kOpaque));
        doc.root().addOnTop(rasterLayer(doc, "translucent overlay", kTranslucent));
        CHECK(documentIsProvablyOpaque(doc));
    }
}

TEST_CASE("the flatten overload makes alpha and the colour count exact") {
    core::Document doc = makeDoc();
    doc.root().addOnTop(rasterLayer(doc, "bg", kOpaque)); // structurally opaque...

    common::Image flat(4, 4);
    flat.fill(common::Color8{1, 2, 3, 255});
    flat.rgba[7] = 0; // ...but the actual flatten has a transparent pixel
    const DocumentProfile p = profileDocument(doc, flat);
    CHECK(p.hasAlpha);       // the flatten outranks the structural guess
    CHECK(p.distinctColors == 2);

    common::Image opaque(4, 4);
    opaque.fill(common::Color8{9, 9, 9, 255});
    const DocumentProfile q = profileDocument(doc, opaque);
    CHECK_FALSE(q.hasAlpha);
    CHECK(q.distinctColors == 1);
}

TEST_CASE("countDistinctColors bails out above its cap instead of hashing a photograph") {
    common::Image gradient(64, 1);
    for (std::uint32_t x = 0; x < 64; ++x) {
        const std::size_t p = static_cast<std::size_t>(x) * 4;
        gradient.rgba[p] = static_cast<std::uint8_t>(x * 4);
        gradient.rgba[p + 3] = 255;
    }
    CHECK(countDistinctColors(gradient, 256) == 64);
    CHECK(countDistinctColors(gradient, 64) == 64);  // exactly the cap is still an answer
    CHECK(countDistinctColors(gradient, 63) == -1);  // one over: give up, report truecolour
    CHECK(countDistinctColors(common::Image{}, 256) == 0);

    CHECK_FALSE(imageHasTransparency(gradient));
    gradient.rgba[3] = 0;
    CHECK(imageHasTransparency(gradient));
    CHECK_FALSE(imageHasTransparency(common::Image{}));
}

TEST_CASE("blend modes and opacity are one signal; Normal at full opacity is not") {
    core::Document doc = makeDoc();
    auto layer = rasterLayer(doc, "bg", kOpaque);
    core::RasterLayer* raw = layer.get();
    doc.root().addOnTop(std::move(layer));
    CHECK_FALSE(profileDocument(doc).usesBlendModes);

    raw->setBlendMode(core::BlendMode::Screen);
    CHECK(profileDocument(doc).usesBlendModes);
    raw->setBlendMode(core::BlendMode::Normal);
    CHECK_FALSE(profileDocument(doc).usesBlendModes);

    raw->setOpacity(0.75f);
    CHECK(profileDocument(doc).usesBlendModes);
}

TEST_CASE("only a mask with partial coverage counts as soft") {
    core::Document doc = makeDoc(4, 4);
    auto layer = rasterLayer(doc, "bg", kOpaque);
    core::RasterLayer* raw = layer.get();
    doc.root().addOnTop(std::move(layer));

    core::RasterMask hard(4, 4, 255);
    hard.coverage[0] = 0; // a hard-edged hole: 1 bit of alpha reproduces it exactly
    raw->setMask(hard);
    CHECK_FALSE(profileDocument(doc).usesSoftMask);

    core::RasterMask soft(4, 4, 255);
    soft.coverage[0] = 128;
    raw->setMask(soft);
    CHECK(profileDocument(doc).usesSoftMask);

    // A disabled mask is ignored by the compositor, so it is not a loss either.
    core::RasterMask disabled = soft;
    disabled.enabled = false;
    raw->setMask(disabled);
    CHECK_FALSE(profileDocument(doc).usesSoftMask);
}

TEST_CASE("effects only count when something in them is enabled") {
    core::Document doc = makeDoc();
    auto layer = rasterLayer(doc, "bg", kOpaque);
    core::RasterLayer* raw = layer.get();
    doc.root().addOnTop(std::move(layer));

    raw->setEffects(core::LayerEffects{}); // present but empty == the untouched path
    CHECK_FALSE(profileDocument(doc).hasEffects);

    core::LayerEffects fx;
    core::ShadowEffect shadow;
    shadow.enabled = true;
    fx.dropShadows.push_back(shadow);
    raw->setEffects(fx);
    CHECK(profileDocument(doc).hasEffects);
}

TEST_CASE("text, 3D text and adjustments are found on their own layer kinds") {
    core::Document doc = makeDoc();

    auto empty = std::make_unique<core::TextLayer>(doc.mintLayerId(), "empty", "");
    doc.root().addOnTop(std::move(empty));
    CHECK_FALSE(profileDocument(doc).hasText); // an empty block is not live text

    auto text = std::make_unique<core::TextLayer>(doc.mintLayerId(), "text", "Hello");
    core::TextLayer* rawText = text.get();
    doc.root().addOnTop(std::move(text));
    CHECK(profileDocument(doc).hasText);
    CHECK_FALSE(profileDocument(doc).hasExtrude3d);

    rawText->mutableBlock().extrude = core::text::Extrude{};
    CHECK(profileDocument(doc).hasExtrude3d);

    doc.root().addOnTop(std::make_unique<core::AdjustmentLayer>(doc.mintLayerId(), "curves",
                                                                core::AdjustmentKind::Curves));
    CHECK(profileDocument(doc).hasAdjustments);
}

TEST_CASE("vector detail: conic gradients, stroke alignment and dashes") {
    core::Document doc = makeDoc();
    auto layer = std::make_unique<core::VectorLayer>(doc.mintLayerId(), "shape");
    core::VectorLayer* raw = layer.get();
    doc.root().addOnTop(std::move(layer));

    CHECK_FALSE(profileDocument(doc).hasVector); // a vector layer with no object carries nothing

    core::vec::Object obj;
    raw->setObject(obj);
    DocumentProfile p = profileDocument(doc);
    CHECK(p.hasVector);
    CHECK_FALSE(p.usesConicGradient);
    CHECK_FALSE(p.usesStrokeAlign);
    CHECK_FALSE(p.usesDashes);

    SUBCASE("a linear gradient fill is not a conic one") {
        core::vec::Gradient g;
        g.type = core::vec::GradientType::Linear;
        obj.fill = g;
        raw->setObject(obj);
        CHECK_FALSE(profileDocument(doc).usesConicGradient);

        g.type = core::vec::GradientType::Conic;
        obj.fill = g;
        raw->setObject(obj);
        CHECK(profileDocument(doc).usesConicGradient);
    }

    SUBCASE("a conic gradient on the STROKE is found too") {
        core::vec::Gradient g;
        g.type = core::vec::GradientType::Conic;
        obj.stroke.enabled = true;
        obj.stroke.paint = g;
        raw->setObject(obj);
        CHECK(profileDocument(doc).usesConicGradient);
    }

    SUBCASE("stroke alignment and dashes only count on an enabled stroke") {
        obj.stroke.enabled = false;
        obj.stroke.align = core::vec::StrokeAlign::Outside;
        obj.stroke.dashArray = {4.0, 2.0};
        raw->setObject(obj);
        p = profileDocument(doc);
        CHECK_FALSE(p.usesStrokeAlign);
        CHECK_FALSE(p.usesDashes);

        obj.stroke.enabled = true;
        raw->setObject(obj);
        p = profileDocument(doc);
        CHECK(p.usesStrokeAlign);
        CHECK(p.usesDashes);

        obj.stroke.align = core::vec::StrokeAlign::Center; // the SVG-compatible one
        raw->setObject(obj);
        CHECK_FALSE(profileDocument(doc).usesStrokeAlign);
    }
}

TEST_CASE("colour state, dpi and EXIF come off the document and its layers") {
    core::Document doc(16, 16, core::ColorSpace::DisplayP3, core::Precision::F32);
    doc.setDpi(300.0);
    auto layer = rasterLayer(doc, "photo", kOpaque);
    common::ExifData exif;
    exif.make = std::string("Mosaic");
    layer->setExif(exif);
    doc.root().addOnTop(std::move(layer));

    const DocumentProfile p = profileDocument(doc);
    CHECK(p.hasNonSrgbSpace);
    CHECK_FALSE(p.hasICC); // no custom .icc path set
    CHECK(p.dpi == doctest::Approx(300.0));
    CHECK(p.hasEXIF);
    CHECK(p.precision == core::Precision::F32); // recorded as INTENT...
    CHECK(p.sourceBitDepth == 8);               // ...while the pipeline still delivers 8-bit
    CHECK_FALSE(p.sourceIsFloat);
    CHECK_FALSE(p.hasXMP); // no XMP in the model yet

    doc.setIccProfile("/tmp/does-not-need-to-exist.icc", "Test profile");
    CHECK(profileDocument(doc).hasICC);

    core::Document srgb = makeDoc();
    CHECK_FALSE(profileDocument(srgb).hasNonSrgbSpace);
}
