#include "io/mosaic/docio.hpp"

#include "core/command.hpp"
#include "io/mosaic/docjson.hpp"
#include "io/mosaic/file.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The document<->container bridge (S48 Build 1, document-model slice; spec 2.2 keys, 6 layer
// kinds, docs/mosaic-native-format.md): full-fidelity round-trips of everything the model can
// hold, sparse-tile reconstruction, the id-allocator and uuid surviving reopen, damage honesty
// (parity compose, lost payloads counted, hostile manifests rejected), and hostile-input caps.
namespace {

using namespace mosaic;
using namespace mosaic::io::native;

core::vec::Object sampleVectorObject() {
    core::vec::Object o;
    core::vec::Path path;
    core::vec::SubPath sp;
    sp.closed = true;
    sp.nodes.push_back({{0, 0}, {-4, -2}, {4, 2}, core::vec::Node::Type::Smooth});
    sp.nodes.push_back({{40, 10}, {30, 5}, {50, 15}, core::vec::Node::Type::Corner});
    sp.nodes.push_back({{20, 44}, {25, 40}, {15, 48}, core::vec::Node::Type::Symmetric});
    path.subpaths.push_back(sp);
    core::vec::SubPath hole;
    hole.closed = false;
    hole.nodes.push_back({{10, 10}, {10, 10}, {10, 10}, core::vec::Node::Type::Corner});
    hole.nodes.push_back({{12, 18}, {12, 18}, {12, 18}, core::vec::Node::Type::Corner});
    path.subpaths.push_back(hole);
    path.fillRule = core::vec::FillRule::EvenOdd;
    o.geometry = path;

    core::vec::Gradient g;
    g.type = core::vec::GradientType::Conic;
    g.spread = core::vec::SpreadMethod::Reflect;
    g.transform = common::Affine2D::trs({3, 4}, 0.25, {2, 1});
    g.stops.push_back({0.0, {1.0f, 0.0f, 0.0f, 1.0f}});
    g.stops.push_back({0.6, {0.0f, 1.0f, 0.25f, 0.5f}});
    g.stops.push_back({1.0, {0.0f, 0.0f, 1.0f, 0.0f}});
    o.fill = g;

    o.stroke.enabled = true;
    o.stroke.width = 2.5;
    o.stroke.miterLimit = 3.0;
    o.stroke.dashOffset = 1.5;
    o.stroke.cap = core::vec::LineCap::Square;
    o.stroke.join = core::vec::LineJoin::Bevel;
    o.stroke.align = core::vec::StrokeAlign::Inside;
    o.stroke.dashArray = {4.0, 2.0, 1.0};
    core::vec::ProceduralPattern pat;
    pat.kind = core::vec::ProceduralPattern::Kind::Honeycomb;
    pat.fg = {0.9f, 0.8f, 0.1f, 1.0f};
    pat.scale = 12.0f;
    pat.angleDeg = 30.0f;
    pat.anchorToCanvas = true;
    o.stroke.paint = core::vec::Pattern{pat};
    o.paintOrder = core::vec::Object::PaintOrder::StrokeThenFill;
    return o;
}

core::LayerEffects sampleEffects() {
    core::LayerEffects fx;
    fx.fillOpacity = 0.7f;
    core::ShadowEffect drop;
    drop.enabled = true;
    drop.color = {0.1f, 0.2f, 0.3f, 1.0f};
    drop.blend = core::BlendMode::LinearBurn;
    drop.distance = 9.0f;
    fx.dropShadows = {drop, core::ShadowEffect{}};
    fx.outerGlow.enabled = true;
    fx.outerGlow.paint = core::vec::SolidPaint{{1.0f, 0.9f, 0.2f, 1.0f}};
    fx.outerGlow.blend = core::BlendMode::Screen;
    core::vec::ProceduralPattern pat;
    pat.kind = core::vec::ProceduralPattern::Kind::Chainmail;
    fx.patternOverlay.enabled = true;
    fx.patternOverlay.paint = core::vec::Pattern{pat};
    fx.satin.enabled = true;
    fx.satin.invert = false;
    fx.bevel.enabled = true;
    fx.bevel.style = core::BevelEffect::Style::PillowEmboss;
    core::StrokeEffect stroke;
    stroke.enabled = true;
    stroke.align = core::StrokeEffect::Align::Center;
    stroke.paint = core::vec::SolidPaint{{0.0f, 0.0f, 0.0f, 1.0f}};
    fx.strokes = {stroke, stroke};
    return fx;
}

core::text::TextBlock sampleTextBlock() {
    core::text::CharStyle base;
    base.font.family = "Noto Serif";
    base.font.weight = 650.0f;
    base.font.italic = true;
    base.font.variations["opsz"] = 14.0f;
    base.sizePx = 32.0f;
    base.tracking = 20.0f;
    base.features = {"smcp", "-liga"};
    base.kerning = core::text::Kerning::Optical;
    core::text::TextBlock b = core::text::makeBlock("Hello\nMosaic", base);
    b.frame = core::text::TextFrame::Area;
    b.areaSize = {180, 90};
    b.aa = core::text::AntiAlias::Subpixel;
    b.writingMode = core::text::WritingMode::VerticalRL;
    b.orientation = core::text::TextOrientation::Upright;
    b.bend = -0.4f;
    core::text::CharStyle second = base;
    second.font.family = "Inter";
    second.paint = core::vec::SolidPaint{{0.2f, 0.4f, 0.9f, 1.0f}};
    second.underline = true;
    core::text::setStyleRange(b, 6, 12, second);
    b.paragraphs[0].align = core::text::Paragraph::Align::Justify;
    b.paragraphs[0].language = "de";
    b.paragraphs[0].hyphenate = true;
    b.paragraphs[1].direction = core::text::Paragraph::Direction::RTL;
    b.paragraphs[1].leadingAbsolute = true;
    core::text::PathFit fit;
    fit.layer = 42;
    fit.s0 = 3.5;
    fit.s1 = 120.25;
    fit.flip = true;
    fit.baked.push_back({{{0, 0}, {10, 5}, {20, 0}}, false});
    b.pathFit = fit;
    core::text::Extrude ex;
    ex.depth = 14.0f;
    ex.bevelFront = {core::text::Bevel::Profile::Concave, 2.0f, 5};
    ex.material.albedo = {0.9f, 0.3f, 0.2f, 1.0f};
    ex.material.metalness = 0.8f;
    ex.orientation = {0.9, 0.1, 0.2, 0.05};
    ex.perspective = 25.0f;
    ex.lights.push_back({{0.1, -0.5, -0.8}, {1.0f, 0.4f, 0.4f, 1.0f}, 0.5f});
    ex.reflectCanvas = true;
    ex.runMaterials[1] = {{0.1f, 0.9f, 0.1f, 1.0f}, 0.0f, 0.9f};
    b.extrude = ex;
    return b;
}

void paintNoise(common::Image& img, std::uint8_t seed) {
    for (std::size_t i = 0; i < img.rgba.size(); ++i)
        img.rgba[i] = static_cast<std::uint8_t>(seed + i * 7 + (i >> 8));
}

// The kitchen sink: one document exercising every kind, every chrome field, masks, effects,
// sparse tiles (an untouched layer), edge tiles (canvas not a tile multiple), nested groups.
std::unique_ptr<core::Document> sampleDocument() {
    auto doc = std::make_unique<core::Document>(200, 150, core::ColorSpace::DisplayP3,
                                                core::Precision::F16);
    doc->setDpi(300.0);
    doc->setTitle("Kitchen Sink");
    doc->setUuid(mintDocumentUuid());

    auto raster = doc->makeRaster("Paint", 200, 150);
    paintNoise(raster->image(), 3);
    raster->setOpacity(0.5f);
    raster->setBlendMode(core::BlendMode::Multiply);
    raster->setTransform(common::Affine2D::trs({5, -3}, 0.1, {1.5, 0.75}));
    raster->setLocked(true);
    raster->setVisible(false);
    raster->setClipToBelow(true);
    raster->setPastedMarker(true);
    core::RasterMask mask(200, 150, 255);
    for (std::size_t i = 0; i < 900; ++i)
        mask.coverage[i * 7 % mask.coverage.size()] = static_cast<std::uint8_t>(i);
    mask.enabled = false;
    mask.linked = false;
    raster->setMask(std::move(mask));
    raster->setEffects(sampleEffects());
    common::ExifData exif;  // the full field set (the EXIF read slice; orientation post-bake = 1)
    exif.orientation = 1;
    exif.focalLengthMm = 23.5;
    exif.focalLength35mm = 35;
    exif.dateTimeOriginal = common::ExifDateTime{2024, 2, 29, 17, 3, 59};
    exif.gpsLatitude = 37.775;
    exif.gpsLongitude = -122.418;
    exif.make = "Example";
    exif.model = "Camera Mk II";
    raster->setExif(std::move(exif));
    doc->root().addOnTop(std::move(raster));

    auto empty = doc->makeRaster("Untouched", 200, 150); // fully transparent: zero tiles stored
    doc->root().addOnTop(std::move(empty));

    auto group = doc->makeGroup("Shapes");
    group->setExpanded(false);
    auto vector = doc->makeVector("Blob");
    vector->setObject(sampleVectorObject());
    group->addOnTop(std::move(vector));
    auto star = doc->makeVector("Star");
    core::vec::Object starObj;
    starObj.geometry = core::vec::ParametricShape{core::vec::StarShape{7, 30, 12, 1.5, 0.5}};
    starObj.fill = core::vec::SolidPaint{{0.9f, 0.2f, 0.4f, 1.0f}};
    star->setObject(starObj);
    group->addOnTop(std::move(star));
    auto emptyVector = doc->makeVector("No object yet");
    group->addOnTop(std::move(emptyVector));
    auto inner = doc->makeGroup("Inner");
    auto adjustment = doc->makeAdjustment("Curves", core::AdjustmentKind::Curves);
    adjustment->params()["gamma"] = 1.8;
    adjustment->params()["black_point"] = 0.02;
    inner->addOnTop(std::move(adjustment));
    group->addOnTop(std::move(inner));
    doc->root().addOnTop(std::move(group));

    auto text = doc->makeText("Headline");
    text->setBlock(sampleTextBlock());
    text->setAutoNamed(false);
    doc->root().addOnTop(std::move(text));

    common::Image source(70, 40);
    paintNoise(source, 90);
    auto magic = doc->makeMagic("Placed photo", std::move(source));
    common::ExifData placedExif;  // a sparse set on a non-raster kind (the slot is base chrome)
    placedExif.focalLength35mm = 200;
    placedExif.gpsLatitude = -33.87;  // southern + western hemispheres: signs survive the wire
    placedExif.gpsLongitude = -70.66;
    magic->setExif(std::move(placedExif));
    doc->root().addOnTop(std::move(magic));

    // A texture-generator layer with every spec arm's fields off their defaults (S55-a): the
    // params round-trip; the pixel cache is regenerated content, never stored.
    core::texture::TextureParams texParams = core::texture::defaultTextureParams(
        core::texture::Generator::Paper);
    texParams.seed = 0xfeedface12345678ull;
    texParams.scale = 2.25;
    auto& paperSpec = std::get<core::texture::PaperParams>(texParams.spec);
    paperSpec.tint = {0.8f, 0.7f, 0.55f, 1.0f};
    paperSpec.roughness = 0.9;
    paperSpec.grainAngleDeg = 33.0;
    paperSpec.grainAnisotropy = 0.6;
    paperSpec.lightAzimuthDeg = 120.0;
    paperSpec.lightElevationDeg = 15.0;
    paperSpec.kind = core::texture::PaperKind::Laid;  // S55-d growth fields, all off-default
    paperSpec.fiber = 0.8;
    paperSpec.laidSpacing = 6.5;
    paperSpec.chainSpacing = 70.0;
    paperSpec.laidDepth = 0.85;
    paperSpec.matte = 0.4;
    paperSpec.sheen = 0.3;
    paperSpec.deckleEdge = true;
    paperSpec.deckleAmount = 0.7;
    paperSpec.deckleInset = 0.09;
    paperSpec.printTooth = true;
    paperSpec.printAmount = 0.5;
    auto texture = doc->makeTexture("Cardstock", std::move(texParams));
    doc->root().addOnTop(std::move(texture));

    // A sky texture layer with every S55-b growth field off-default, including a custom cloud
    // deck stack (empty biases/altitudes must survive too -- the §3.1 growth rule's test bed).
    core::texture::TextureParams skyParams = core::texture::defaultTextureParams(
        core::texture::Generator::Sky);
    skyParams.seed = 0x5107'CAFE'F00Dull;
    skyParams.scale = 0.75;
    auto& skySpec = std::get<core::texture::SkyParams>(skyParams.spec);
    skySpec.enableDome = false;
    skySpec.enableHaze = false;
    skySpec.sunAzimuthDeg = 205.0;
    skySpec.sunElevationDeg = 8.5;
    skySpec.turbidity = 4.75;
    skySpec.cloudCoverage = 0.62;
    skySpec.cloudScale = 1.4;
    skySpec.groundAlbedo = 0.55;
    skySpec.exposure = -0.6;
    skySpec.sunDiscScale = 2.5;
    skySpec.fovDeg = 84.0;
    skySpec.pitchDeg = 3.0;
    skySpec.rollDeg = -2.25;
    skySpec.shiftY = 0.12;
    skySpec.windDirectionDeg = 310.0;
    skySpec.windStrength = 0.85;
    skySpec.volumetricClouds = false;  // S55-c growth field, off its default (true)
    skySpec.enableMoon = true;         // S55-f night growth fields, all off their defaults
    skySpec.moonAzimuthDeg = 199.0;
    skySpec.moonElevationDeg = 41.5;
    skySpec.moonScale = 2.75;
    skySpec.starsAmount = 0.9;
    skySpec.enableLensFlare = true;  // lens-flare growth fields, off their defaults
    skySpec.flareStrength = 0.85;
    skySpec.obsYear = 2031;  // S55 night-overhaul observer clock, all off their defaults
    skySpec.obsMonth = 9;
    skySpec.obsDay = 23;
    skySpec.obsHourUtc = 21.5;
    skySpec.obsLatitudeDeg = -33.87;
    skySpec.obsLongitudeDeg = 151.21;
    skySpec.cloudLayers = {
        {true, core::texture::CloudType::Cumulonimbus, 1.2, 0.8, 950.0},
        {false, core::texture::CloudType::Cirrostratus, 0.4, 2.0, 0.0},
        {true, core::texture::CloudType::Altocumulus, 1.0, 1.0, 4200.0},
    };
    auto skyTexture = doc->makeTexture("Storm sky", std::move(skyParams));
    doc->root().addOnTop(std::move(skyTexture));

    // A grass texture layer with every S55-e growth field off-default (the §3.1 growth rule bed).
    core::texture::TextureParams grassParams = core::texture::defaultTextureParams(
        core::texture::Generator::Grass);
    grassParams.seed = 0x6712'BEEF'0033ull;
    grassParams.scale = 1.6;
    auto& grassSpec = std::get<core::texture::GrassParams>(grassParams.spec);
    grassSpec.baseColor = {0.20f, 0.36f, 0.12f, 1.0f};
    grassSpec.tipColor = {0.55f, 0.70f, 0.30f, 1.0f};
    grassSpec.clumpScale = 1.4;
    grassSpec.patchiness = 0.7;
    grassSpec.soilColor = {0.14f, 0.12f, 0.06f, 1.0f};  // S55-e growth fields, all off-default
    grassSpec.dryColor = {0.66f, 0.60f, 0.28f, 1.0f};
    grassSpec.enableTurf = false;
    grassSpec.enableBlades = false;
    grassSpec.density = 0.65;
    grassSpec.bladeHeight = 1.7;
    grassSpec.bladeWidth = 1.25;
    grassSpec.curvature = 0.8;
    grassSpec.windDirectionDeg = 65.0;
    grassSpec.windStrength = 0.55;
    grassSpec.fovDeg = 48.0;
    grassSpec.pitchDeg = 24.0;
    grassSpec.lightAzimuthDeg = 210.0;
    grassSpec.lightElevationDeg = 22.0;
    grassSpec.dryAmount = 0.4;
    auto grassTexture = doc->makeTexture("Wild lawn", std::move(grassParams));
    doc->root().addOnTop(std::move(grassTexture));

    // An S55-g material texture layer with every field off its default (wood stands in for the
    // whole material family here; the per-arm wire coverage lives in the dedicated material
    // round-trip case below).
    core::texture::TextureParams woodParams = core::texture::defaultTextureParams(
        core::texture::Generator::Wood);
    woodParams.seed = 0x0DDF'00D5'1234ull;
    woodParams.scale = 3.5;
    auto& woodSpec = std::get<core::texture::WoodParams>(woodParams.spec);
    woodSpec.earlyColor = {0.85f, 0.70f, 0.50f, 1.0f};
    woodSpec.lateColor = {0.35f, 0.20f, 0.10f, 1.0f};
    woodSpec.ringSpacing = 31.0;
    woodSpec.ringContrast = 0.8;
    woodSpec.waviness = 0.55;
    woodSpec.knots = 0.6;
    woodSpec.fiber = 0.75;
    woodSpec.grainAngleDeg = 12.0;
    woodSpec.roughness = 0.66;
    woodSpec.matte = 0.45;
    woodSpec.sheen = 0.35;
    woodSpec.lightAzimuthDeg = 100.0;
    woodSpec.lightElevationDeg = 18.0;
    auto woodTexture = doc->makeTexture("Oak shelf", std::move(woodParams));
    doc->root().addOnTop(std::move(woodTexture));

    // Ids consumed beyond the live layers must survive the round-trip too (never reused).
    (void)doc->mintLayerId();
    (void)doc->mintLayerId();
    return doc;
}

std::vector<std::uint8_t> saveDocument(const core::Document& doc) {
    std::string error;
    const auto input = buildDocumentCheckpoint(doc, &error);
    REQUIRE_MESSAGE(input.has_value(), error);
    return buildCheckpoint(*input);
}

DocumentReadResult loadDocument(const std::vector<std::uint8_t>& bytes) {
    const OpenReport report = openDocument(bytes);
    std::string error;
    auto result = documentFromReport(report, &error);
    REQUIRE_MESSAGE(result.has_value(), error);
    REQUIRE(result->document != nullptr);
    return std::move(*result);
}

void compareLayers(const core::Layer& a, const core::Layer& b);

void compareChrome(const core::Layer& a, const core::Layer& b) {
    CHECK(a.id() == b.id());
    CHECK(a.kind() == b.kind());
    CHECK(a.name() == b.name());
    CHECK(a.visible() == b.visible());
    CHECK(a.locked() == b.locked());
    CHECK(a.clipToBelow() == b.clipToBelow());
    CHECK(a.pastedMarker() == b.pastedMarker());
    CHECK(a.opacity() == b.opacity());
    CHECK(a.blendMode() == b.blendMode());
    CHECK(a.transform() == b.transform());
    CHECK(a.hasMask() == b.hasMask());
    if (a.hasMask() && b.hasMask())
        CHECK(*a.mask() == *b.mask());
    CHECK(a.hasEffects() == b.hasEffects());
    if (a.hasEffects() && b.hasEffects())
        CHECK(a.effects() == b.effects());
    CHECK(a.exif() == b.exif());
}

void compareLayers(const core::Layer& a, const core::Layer& b) {
    compareChrome(a, b);
    switch (a.kind()) {
    case core::LayerKind::Group: {
        const auto& ga = static_cast<const core::GroupLayer&>(a);
        const auto& gb = static_cast<const core::GroupLayer&>(b);
        CHECK(ga.expanded() == gb.expanded());
        REQUIRE(ga.childCount() == gb.childCount());
        for (std::size_t i = 0; i < ga.childCount(); ++i)
            compareLayers(ga.child(i), gb.child(i));
        break;
    }
    case core::LayerKind::Raster:
        CHECK(static_cast<const core::RasterLayer&>(a).image() ==
              static_cast<const core::RasterLayer&>(b).image());
        break;
    case core::LayerKind::Magic:
        CHECK(static_cast<const core::MagicLayer&>(a).source() ==
              static_cast<const core::MagicLayer&>(b).source());
        break;
    case core::LayerKind::Vector: {
        const auto& va = static_cast<const core::VectorLayer&>(a);
        const auto& vb = static_cast<const core::VectorLayer&>(b);
        REQUIRE(va.hasObject() == vb.hasObject());
        if (va.hasObject())
            CHECK(*va.object() == *vb.object());
        break;
    }
    case core::LayerKind::Text: {
        const auto& ta = static_cast<const core::TextLayer&>(a);
        const auto& tb = static_cast<const core::TextLayer&>(b);
        CHECK(ta.autoNamed() == tb.autoNamed());
        CHECK(ta.block() == tb.block());
        break;
    }
    case core::LayerKind::Adjustment: {
        const auto& aa = static_cast<const core::AdjustmentLayer&>(a);
        const auto& ab = static_cast<const core::AdjustmentLayer&>(b);
        CHECK(aa.adjustmentKind() == ab.adjustmentKind());
        CHECK(aa.params() == ab.params());
        break;
    }
    case core::LayerKind::Texture:
        // The params are the whole content (the pixel cache regenerates; never persisted).
        CHECK(static_cast<const core::TextureLayer&>(a).params() ==
              static_cast<const core::TextureLayer&>(b).params());
        break;
    }
}

void compareDocuments(const core::Document& a, const core::Document& b) {
    CHECK(a.width() == b.width());
    CHECK(a.height() == b.height());
    CHECK(a.dpi() == b.dpi());
    CHECK(a.colorSpace() == b.colorSpace());
    CHECK(a.precision() == b.precision());
    CHECK(a.title() == b.title());
    CHECK(a.uuid() == b.uuid());
    CHECK(a.nextLayerId() == b.nextLayerId());
    REQUIRE(a.root().childCount() == b.root().childCount());
    for (std::size_t i = 0; i < a.root().childCount(); ++i)
        compareLayers(a.root().child(i), b.root().child(i));
}

} // namespace

TEST_CASE("mosaic docio: the kitchen-sink document round-trips with full fidelity") {
    const auto doc = sampleDocument();
    const auto bytes = saveDocument(*doc);
    const DocumentReadResult back = loadDocument(bytes);
    CHECK(back.rejectedChunks == 0);
    CHECK(back.uuid == doc->uuid());
    compareDocuments(*doc, *back.document);

    // Round-trip fixpoint: saving the reopened document reproduces the same document again
    // (and proves the reopened one is fully serializable, not just comparable).
    const auto bytes2 = saveDocument(*back.document);
    const DocumentReadResult back2 = loadDocument(bytes2);
    compareDocuments(*back.document, *back2.document);

    // The id allocator cleared every persisted id: a fresh mint must not collide.
    const core::LayerId minted = back.document->mintLayerId();
    CHECK(back.document->find(minted) == nullptr);
}

TEST_CASE("mosaic docio: a pre-growth (S55-a) sky spec still reads -- absent fields default") {
    // The §3.1 growth rule's contract: SkyParams only ever GAINS fields, and a spec written
    // before a field existed loads with that field at its default (schema stays 1). This is the
    // exact nine-field shape S55-a wrote.
    const nlohmann::json old = {
        {"generator", "sky"},
        {"seed", std::uint64_t{42}},  // literal 42 would store SIGNED; getU64 is strict
        {"scale", 1.5},
        {"spec",
         {{"kind", "sky"},
          {"dome", true},
          {"sun", false},
          {"clouds", true},
          {"haze", true},
          {"sun_azimuth", 90.0},
          {"sun_elevation", 12.0},
          {"turbidity", 6.0},
          {"cloud_coverage", 0.8},
          {"cloud_scale", 2.0}}}};
    const auto p = detail::textureParamsFromJson(old);
    REQUIRE(p.has_value());
    CHECK(p->seed == 42);
    CHECK(p->scale == 1.5);
    const auto* s = std::get_if<core::texture::SkyParams>(&p->spec);
    REQUIRE(s != nullptr);
    CHECK(!s->enableSun);
    CHECK(s->sunAzimuthDeg == 90.0);
    CHECK(s->turbidity == 6.0);
    const core::texture::SkyParams defaults;
    CHECK(s->groundAlbedo == defaults.groundAlbedo);
    CHECK(s->exposure == defaults.exposure);
    CHECK(s->fovDeg == defaults.fovDeg);
    CHECK(s->pitchDeg == defaults.pitchDeg);
    CHECK(s->shiftY == defaults.shiftY);
    CHECK(s->windStrength == defaults.windStrength);
    CHECK(s->volumetricClouds == defaults.volumetricClouds);  // absent S55-c field -> true default
    CHECK(s->enableMoon == defaults.enableMoon);  // absent S55-f night fields -> defaults
    CHECK(s->moonAzimuthDeg == defaults.moonAzimuthDeg);
    CHECK(s->moonScale == defaults.moonScale);
    CHECK(s->starsAmount == defaults.starsAmount);
    CHECK(s->obsYear == defaults.obsYear);  // absent S55 night-overhaul clock -> defaults
    CHECK(s->obsMonth == defaults.obsMonth);
    CHECK(s->obsDay == defaults.obsDay);
    CHECK(s->obsHourUtc == defaults.obsHourUtc);
    CHECK(s->obsLatitudeDeg == defaults.obsLatitudeDeg);
    CHECK(s->obsLongitudeDeg == defaults.obsLongitudeDeg);
    CHECK(s->enableLensFlare == defaults.enableLensFlare);  // absent lens-flare fields -> off
    CHECK(s->flareStrength == defaults.flareStrength);
    CHECK(s->cloudLayers == defaults.cloudLayers);

    // A malformed PRESENT growth field still rejects (lenient means absent-tolerant, not
    // garbage-tolerant).
    nlohmann::json bad = old;
    bad["spec"]["fov"] = "wide";
    CHECK(!detail::textureParamsFromJson(bad).has_value());

    // An explicitly EMPTY deck stack is a real (preserved) state, distinct from absent.
    nlohmann::json noDecks = old;
    noDecks["spec"]["cloud_layers"] = nlohmann::json::array();
    const auto p2 = detail::textureParamsFromJson(noDecks);
    REQUIRE(p2.has_value());
    CHECK(std::get<core::texture::SkyParams>(p2->spec).cloudLayers.empty());
}

TEST_CASE("mosaic docio: a pre-growth (S55-a) paper spec still reads -- absent fields default") {
    // The same §3.1 growth contract for PaperParams: the exact six-field shape S55-a wrote loads,
    // and the S55-d growth fields (kind/fibre/laid/matte/sheen/deckle/print) come up at default.
    const nlohmann::json old = {
        {"generator", "paper"},
        {"seed", std::uint64_t{11}},
        {"scale", 2.0},
        {"spec",
         {{"kind", "paper"},
          {"tint", {0.9, 0.85, 0.8, 1.0}},
          {"roughness", 0.7},
          {"grain_angle", 45.0},
          {"grain_anisotropy", 0.5},
          {"light_azimuth", 200.0},
          {"light_elevation", 20.0}}}};
    const auto p = detail::textureParamsFromJson(old);
    REQUIRE(p.has_value());
    CHECK(p->seed == 11);
    CHECK(p->scale == 2.0);
    const auto* s = std::get_if<core::texture::PaperParams>(&p->spec);
    REQUIRE(s != nullptr);
    CHECK(s->roughness == 0.7);
    CHECK(s->grainAngleDeg == 45.0);
    const core::texture::PaperParams defaults;
    CHECK(s->kind == defaults.kind);  // absent S55-d enum -> Wove default
    CHECK(s->fiber == defaults.fiber);
    CHECK(s->laidSpacing == defaults.laidSpacing);
    CHECK(s->matte == defaults.matte);
    CHECK(s->sheen == defaults.sheen);
    CHECK(s->deckleEdge == defaults.deckleEdge);  // absent -> false (opaque)
    CHECK(s->printTooth == defaults.printTooth);

    // A malformed PRESENT growth enum still rejects (lenient = absent-tolerant, not garbage).
    nlohmann::json bad = old;
    bad["spec"]["paper_kind"] = "papyrus";
    CHECK(!detail::textureParamsFromJson(bad).has_value());
}

TEST_CASE("mosaic docio: a pre-growth (S55-a) grass spec still reads -- absent fields default") {
    // The §3.1 growth contract for GrassParams: the exact four-field shape S55-a wrote loads, and
    // the S55-e growth fields (soil/dry colours, toggles, density, blades, wind, camera, light)
    // come up at their defaults (schema stays 1).
    const nlohmann::json old = {
        {"generator", "grass"},
        {"seed", std::uint64_t{5}},
        {"scale", 1.25},
        {"spec",
         {{"kind", "grass"},
          {"base_color", {0.2, 0.35, 0.1, 1.0}},
          {"tip_color", {0.45, 0.6, 0.22, 1.0}},
          {"clump_scale", 1.1},
          {"patchiness", 0.4}}}};
    const auto p = detail::textureParamsFromJson(old);
    REQUIRE(p.has_value());
    CHECK(p->seed == 5);
    CHECK(p->scale == 1.25);
    const auto* s = std::get_if<core::texture::GrassParams>(&p->spec);
    REQUIRE(s != nullptr);
    CHECK(s->clumpScale == 1.1);
    CHECK(s->patchiness == 0.4);
    const core::texture::GrassParams defaults;
    CHECK(s->enableTurf == defaults.enableTurf);    // absent -> true (opaque ground)
    CHECK(s->enableBlades == defaults.enableBlades);  // absent -> true
    CHECK(s->density == defaults.density);
    CHECK(s->bladeHeight == defaults.bladeHeight);
    CHECK(s->pitchDeg == defaults.pitchDeg);
    CHECK(s->soilColor == defaults.soilColor);  // absent grown colour -> the default earth tone

    // A malformed PRESENT growth colour still rejects (lenient = absent-tolerant, not garbage).
    nlohmann::json bad = old;
    bad["spec"]["soil_color"] = "brown";
    CHECK(!detail::textureParamsFromJson(bad).has_value());
}

TEST_CASE("mosaic docio: every S55-g material spec round-trips and rejects garbage") {
    // The five material arms were born whole, so every field reads STRICT; each arm must survive
    // the wire with every field off its default, and a malformed field must reject (the hostile-
    // caps discipline: absent-tolerance is reserved for FUTURE growth fields).
    namespace txg = core::texture;
    const auto roundTrip = [](const txg::TextureParams& p) {
        const auto back = detail::textureParamsFromJson(detail::textureParamsToJson(p));
        REQUIRE(back.has_value());
        CHECK(*back == p);
    };
    const auto rejects = [](const txg::TextureParams& p, const char* field) {
        nlohmann::json j = detail::textureParamsToJson(p);
        j["spec"][field] = "garbage";
        CHECK(!detail::textureParamsFromJson(j).has_value());
    };

    {
        txg::TextureParams p = txg::defaultTextureParams(txg::Generator::Wood);
        p.seed = 7;
        p.scale = 2.0;
        auto& v = std::get<txg::WoodParams>(p.spec);
        v.earlyColor = {0.9f, 0.8f, 0.6f, 1.0f};
        v.ringSpacing = 40.0;
        v.knots = 0.9;
        v.grainAngleDeg = 77.0;
        roundTrip(p);
        rejects(p, "ring_spacing");
        rejects(p, "early_color");
    }
    {
        txg::TextureParams p = txg::defaultTextureParams(txg::Generator::Marble);
        p.seed = 8;
        auto& v = std::get<txg::MarbleParams>(p.spec);
        v.veinColor = {0.2f, 0.25f, 0.3f, 1.0f};
        v.veinSpacing = 120.0;
        v.turbulence = 0.9;
        v.veinAngleDeg = 140.0;
        roundTrip(p);
        rejects(p, "vein_spacing");
    }
    {
        txg::TextureParams p = txg::defaultTextureParams(txg::Generator::Stone);
        p.seed = 9;
        auto& v = std::get<txg::StoneParams>(p.spec);
        v.baseColor = {0.5f, 0.45f, 0.4f, 1.0f};
        v.cellSize = 100.0;
        v.crackDepth = 0.95;
        v.variation = 0.8;
        roundTrip(p);
        rejects(p, "cell_size");
    }
    {
        txg::TextureParams p = txg::defaultTextureParams(txg::Generator::Canvas);
        p.seed = 10;
        auto& v = std::get<txg::CanvasParams>(p.spec);
        v.tint = {0.95f, 0.9f, 0.85f, 1.0f};
        v.threadPitch = 12.5;
        v.weaveAngleDeg = 45.0;
        v.fuzz = 0.6;
        roundTrip(p);
        rejects(p, "thread_pitch");
    }
    {
        txg::TextureParams p = txg::defaultTextureParams(txg::Generator::Metal);
        p.seed = 11;
        auto& v = std::get<txg::MetalParams>(p.spec);
        v.tint = {0.8f, 0.65f, 0.35f, 1.0f};
        v.brushAngleDeg = 90.0;
        v.gradient = 0.9;
        v.sheen = 1.0;
        roundTrip(p);
        rejects(p, "gradient");
        rejects(p, "tint");
    }

    // A truly absent field on a born-whole arm is malformed input, not a growth default.
    txg::TextureParams p = txg::defaultTextureParams(txg::Generator::Metal);
    nlohmann::json j = detail::textureParamsToJson(p);
    j["spec"].erase("brush_angle");
    CHECK(!detail::textureParamsFromJson(j).has_value());
}

TEST_CASE("mosaic docio: sparse tiles -- untouched content costs (almost) nothing") {
    // Same canvas, one noisy layer vs one noisy + one empty layer: the empty layer must add no
    // TILE chunks at all (absent tile == transparent), only its manifest entry.
    auto noisy = std::make_unique<core::Document>(256, 256);
    noisy->setUuid(mintDocumentUuid());
    auto l1 = noisy->makeRaster("A");
    paintNoise(l1->image(), 1);
    noisy->root().addOnTop(std::move(l1));
    const auto one = buildDocumentCheckpoint(*noisy);
    REQUIRE(one.has_value());

    auto l2 = noisy->makeRaster("B"); // stays fully transparent
    noisy->root().addOnTop(std::move(l2));
    const auto two = buildDocumentCheckpoint(*noisy);
    REQUIRE(two.has_value());

    std::size_t tiles1 = 0, tiles2 = 0;
    for (const FileChunk& c : one->chunks)
        tiles1 += c.type == kTypeTile ? 1 : 0;
    for (const FileChunk& c : two->chunks)
        tiles2 += c.type == kTypeTile ? 1 : 0;
    CHECK(tiles1 == 16); // 256/64 squared, all populated
    CHECK(tiles2 == tiles1); // the empty layer added zero

    // And a mask that is default (255) except one corner stores only that corner's tiles.
    auto masked = std::make_unique<core::Document>(256, 256);
    masked->setUuid(mintDocumentUuid());
    auto l3 = masked->makeRaster("C");
    paintNoise(l3->image(), 2);
    core::RasterMask m(256, 256, 255);
    m.coverage[0] = 0; // one touched pixel in tile (0,0)
    l3->setMask(std::move(m));
    masked->root().addOnTop(std::move(l3));
    const auto three = buildDocumentCheckpoint(*masked);
    REQUIRE(three.has_value());
    std::size_t maskTiles = 0;
    for (const FileChunk& c : three->chunks)
        if (c.type == kTypeTile && (c.key.bytes[7] & 0x80) != 0) // mask-bit owners (LE byte 7)
            ++maskTiles;
    CHECK(maskTiles == 1);

    // The sparse reconstruction really reads as default on the way back in.
    const auto bytes = buildCheckpoint(*three);
    const DocumentReadResult back = loadDocument(bytes);
    const auto* layer = back.document->root().child(0).as<core::RasterLayer>();
    REQUIRE(layer != nullptr);
    REQUIRE(layer->hasMask());
    CHECK(layer->mask()->coverage[0] == 0);
    CHECK(layer->mask()->coverage[100 * 256 + 100] == 255);
}

TEST_CASE("mosaic docio: parity repairs tile damage; lost payloads are counted, never guessed") {
    const auto doc = sampleDocument();
    auto bytes = saveDocument(*doc);

    SUBCASE("one flipped tile byte reconstructs through Reed-Solomon, byte-exact") {
        const auto recs = scanChunks(bytes);
        const mosaic::io::native::ChunkRecord* victim = nullptr;
        for (const auto& r : recs)
            if (r.valid && r.type == kTypeTile)
                victim = &r; // the last tile: any parity-covered content chunk works
        REQUIRE(victim != nullptr);
        bytes[victim->payloadOffset + 3] ^= 0xFF;
        const OpenReport report = openDocument(bytes);
        CHECK(report.base.rsReconstructed >= 1);
        std::string error;
        const auto back = documentFromReport(report, &error);
        REQUIRE_MESSAGE(back.has_value(), error);
        CHECK(back->rejectedChunks == 0);
        compareDocuments(*doc, *back->document);
    }

    SUBCASE("a hand-forged undersized tile is rejected and counted, not applied") {
        // Append a linked... no -- an ordinary TILE chunk whose payload is the wrong size for
        // its grid slot, at a HIGHER generation so it would shadow the real tile if trusted.
        const auto* first = doc->root().child(0).as<core::RasterLayer>();
        REQUIRE(first != nullptr);
        std::vector<std::uint8_t> junk(16, 0xAB);
        appendChunk(bytes, kTypeTile, tileKey(first->id(), 0, 0), 99, junk, Profile::Store);
        // Destroy the roots so the full scan (which cannot consult wal offsets) resolves it in.
        for (const auto& r : scanChunks(bytes))
            if (r.valid && r.type == kTypeRoot)
                bytes[r.payloadOffset + 2] ^= 0xFF;
        const OpenReport report = openDocument(bytes);
        CHECK(report.base.usedFullScan);
        std::string error;
        const auto back = documentFromReport(report, &error);
        REQUIRE_MESSAGE(back.has_value(), error);
        CHECK(back->rejectedChunks == 1); // rejected loudly...
        const auto* layer = back->document->root().child(0).as<core::RasterLayer>();
        REQUIRE(layer != nullptr);
        // ...and the real tile at generation 0 lost the highest-generation contest, so the
        // region honestly reads transparent rather than as forged or stale bytes.
        CHECK(layer->image().rgba[0] == 0);
    }
}

TEST_CASE("mosaic docio: a file from a newer Mosaic says so, instead of pretending to be damaged") {
    const auto doc = sampleDocument();
    auto in = *buildDocumentCheckpoint(*doc);
    in.formatVersion = kFormatVersion + 1;
    const auto future = buildCheckpoint(in);

    const OpenReport report = openDocument(future);
    REQUIRE(report.base.unsupportedVersion);
    std::string error;
    CHECK(!documentFromReport(report, &error).has_value());
    CHECK(error.find("needs a newer Mosaic") != std::string::npos);
    CHECK(error.find("damaged") == std::string::npos); // never the recovery face

    // And the same document at THIS version still opens, so the gate is the version and nothing else.
    const auto ours = buildCheckpoint(*buildDocumentCheckpoint(*doc));
    const auto back = documentFromReport(openDocument(ours), &error);
    REQUIRE_MESSAGE(back.has_value(), error);
    compareDocuments(*doc, *back->document);
}

TEST_CASE("mosaic docio: the manifest survives losing either of its copies") {
    // The manifest is the ONE chunk without which nothing opens: nothing else says how big the
    // canvas is or which layer a tile belongs to. Before it was replicated, flipping a single byte
    // in its ~500-byte frame -- 0.07% of a 744KB file -- made the whole document unopenable while
    // every other frame stayed valid, because parity does not cover it (a stripe pads to its
    // longest member, so a large manifest would inflate its whole stripe). Found by a user feeding
    // random corruption at a real file; this is the regression.
    const auto doc = sampleDocument();
    const auto clean = saveDocument(*doc);

    std::vector<std::size_t> copies;
    for (const auto& r : scanChunks(clean))
        if (r.valid && r.type == kTypeManifest)
            copies.push_back(r.offset);
    REQUIRE(copies.size() == 2);
    CHECK(copies[1] - copies[0] > 1024); // far apart: one burst of damage cannot take both

    // Either copy alone carries the document -- and losing a replica is not damage to report.
    for (std::size_t which = 0; which < copies.size(); ++which) {
        CAPTURE(which);
        auto bytes = clean;
        const auto rec = parseChunkAt(bytes, copies[which]);
        REQUIRE(rec.has_value());
        bytes[rec->payloadOffset] ^= 0xFF;

        const OpenReport report = openDocument(bytes);
        CHECK(report.base.rootFound);
        CHECK(!report.base.usedFullScan);
        CHECK(report.base.lostEntries == 0);        // a replica answered: nothing was lost
        CHECK(report.base.lostHistoryEntries == 0); // and it is NOT retained history
        std::size_t manifests = 0;
        for (const auto& c : report.base.chunks)
            if (c.type == kTypeManifest)
                ++manifests;
        CHECK(manifests == 1); // the survivor, exactly once -- replicas collapse, never duplicate
        CHECK(report.base.retained.empty());

        std::string error;
        const auto back = documentFromReport(report, &error);
        REQUIRE_MESSAGE(back.has_value(), error);
        CHECK(back->rejectedChunks == 0);
        compareDocuments(*doc, *back->document);
    }

    // Both copies gone: honestly unopenable, and counted ONCE -- one chunk lost, not two areas.
    {
        auto bytes = clean;
        for (const std::size_t off : copies) {
            const auto rec = parseChunkAt(bytes, off);
            REQUIRE(rec.has_value());
            bytes[rec->payloadOffset] ^= 0xFF;
        }
        const OpenReport report = openDocument(bytes);
        CHECK(report.base.lostEntries == 1);
        std::string error;
        CHECK(!documentFromReport(report, &error).has_value());
        CHECK(!error.empty());
    }
}

TEST_CASE("mosaic docio: hostile manifests are refused with words, not crashes") {
    const auto doc = sampleDocument();
    const auto input = buildDocumentCheckpoint(*doc);
    REQUIRE(input.has_value());

    const auto rebuildWithManifest = [&](const std::string& manifest) {
        CheckpointInput mutated = *input;
        mutated.chunks[0].payload.assign(manifest.begin(), manifest.end());
        const auto bytes = buildCheckpoint(mutated);
        std::string error;
        const auto result = documentFromReport(openDocument(bytes), &error);
        CHECK(!result.has_value());
        CHECK(!error.empty());
        return error;
    };

    CHECK(rebuildWithManifest("not json at all").find("unreadable") != std::string::npos);
    CHECK(rebuildWithManifest(R"({"schema": 999})").find("newer Mosaic") != std::string::npos);
    // A canvas far past the dimension cap.
    CHECK(rebuildWithManifest(
              R"({"schema":1,"uuid":"u","title":"t","canvas":{"w":4000000000,"h":10,"dpi":72},)"
              R"("color":{"space":"srgb","precision":"u8"},"next_layer_id":1,"surfaces":[],)"
              R"("layers":[]})")
              .find("out of range") != std::string::npos);
    // A surface table asking for terabytes across many entries.
    std::string bomb = R"({"schema":1,"uuid":"u","title":"t",)"
                       R"("canvas":{"w":100,"h":100,"dpi":72},)"
                       R"("color":{"space":"srgb","precision":"u8"},"next_layer_id":1,)"
                       R"("surfaces":[)";
    for (int i = 0; i < 2000; ++i) {
        if (i > 0)
            bomb += ',';
        bomb += R"({"id":)" + std::to_string(i + 1) +
                R"(,"fmt":"rgba8","w":30000,"h":30000})";
    }
    bomb += R"(],"layers":[]})";
    CHECK(rebuildWithManifest(bomb).find("unreasonable") != std::string::npos);

    // No manifest at all (destroy it): structure gone, said plainly.
    {
        auto bytes = buildCheckpoint(*input);
        for (const auto& r : scanChunks(bytes))
            if (r.valid && r.type == kTypeManifest)
                bytes[r.payloadOffset + 2] ^= 0xFF;
        // The manifest is not parity-covered (spec 2.7 stripes tile/vector content), so the
        // damage stands and the read must refuse with an honest message.
        std::string error;
        const auto result = documentFromReport(openDocument(bytes), &error);
        CHECK(!result.has_value());
        CHECK(error.find("manifest") != std::string::npos);
    }
}

TEST_CASE("mosaic docio: uuids are well-formed and distinct") {
    const std::string a = mintDocumentUuid();
    const std::string b = mintDocumentUuid();
    CHECK(a.size() == 36);
    CHECK(a[8] == '-');
    CHECK(a[13] == '-');
    CHECK(a[18] == '-');
    CHECK(a[23] == '-');
    CHECK(a != b);

    // A document without a uuid refuses to serialize -- identity is never invented silently.
    core::Document doc(32, 32);
    std::string error;
    CHECK(!buildDocumentCheckpoint(doc, &error).has_value());
    CHECK(error.find("uuid") != std::string::npos);
}

TEST_CASE("mosaic docio: an empty document round-trips") {
    auto doc = std::make_unique<core::Document>(64, 48);
    doc->setUuid(mintDocumentUuid());
    const auto bytes = saveDocument(*doc);
    const DocumentReadResult back = loadDocument(bytes);
    CHECK(back.rejectedChunks == 0);
    compareDocuments(*doc, *back.document);
    CHECK(back.document->root().childCount() == 0);
}

TEST_CASE("mosaic docio: the document ICC working profile round-trips (grown color fields)") {
    auto doc = std::make_unique<core::Document>(32, 24);
    doc->setUuid("uuid-icc");
    doc->setIccProfile("/somewhere/profile.icc", "My Wide Gamut");
    auto raster = doc->makeRaster("bg", 32, 24);
    raster->image().fill({10, 20, 30, 255});
    doc->root().addOnTop(std::move(raster));

    const DocumentReadResult back = loadDocument(saveDocument(*doc));
    CHECK(back.document->iccProfilePath() == "/somewhere/profile.icc");
    CHECK(back.document->iccProfileName() == "My Wide Gamut");
    // The enum fallback still rides "space" for readers/builds without the file.
    CHECK(back.document->colorSpace() == doc->colorSpace());

    // A profile-less document writes NO icc fields and reads back clean (pre-growth parity).
    auto plain = std::make_unique<core::Document>(16, 16);
    plain->setUuid("uuid-plain");
    auto layer = plain->makeRaster("bg", 16, 16);
    plain->root().addOnTop(std::move(layer));
    const DocumentReadResult plainBack = loadDocument(saveDocument(*plain));
    CHECK(plainBack.document->iccProfilePath().empty());
    CHECK(plainBack.document->iccProfileName().empty());
}
