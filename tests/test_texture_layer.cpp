#include "core/texture/texture_layer_render.hpp"
#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <variant>

#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "render/compositor.hpp"

// S55-a: the TextureLayer model + baseline generators + refresh pass + compositor integration
// (docs/texture-generator.md §2/§3). Golden hashes at the bottom pin the baseline renders
// byte-for-byte -- replacing a baseline with its real renderer (S55-b/-d/-e) is a DELIBERATE
// golden-breaking change in that session; anything else moving a pixel is a bug.
namespace {

using namespace mosaic;
using core::Document;
using core::LayerId;
using core::TextureLayer;
namespace texture = core::texture;

template <class T, class... Args>
void push(Document& doc, Args&&... args) {
    doc.commands().push(std::make_unique<T>(std::forward<Args>(args)...));
}

std::uint64_t fnv1a(const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Hash a float image at 1/8192 fixed point over [0, 8) (the wash-golden precedent: pinning raw
// IEEE bytes would over-specify an internal buffer; 1/8192 is far finer than any real
// regression). The sky is HDR since S55-b -- the solar disc legitimately exceeds 1 (§4.4) --
// so the range assertion is [0, 8): an escape is caught, not rounded out of sight.
std::uint64_t hashImageF(const common::ImageF& img, bool* inRange) {
    std::vector<std::uint32_t> q;
    q.reserve(img.rgba.size());
    *inRange = true;
    for (const float v : img.rgba) {
        if (!(v >= 0.0f && v < 8.0f)) *inRange = false;
        q.push_back(static_cast<std::uint32_t>(std::clamp(v, 0.0f, 8.0f) * 8192.0f + 0.5f));
    }
    return fnv1a(q.data(), q.size() * sizeof(std::uint32_t));
}

}  // namespace

TEST_CASE("defaultTextureParams seeds the matching variant arm") {
    CHECK(std::holds_alternative<texture::SkyParams>(
        texture::defaultTextureParams(texture::Generator::Sky).spec));
    CHECK(std::holds_alternative<texture::PaperParams>(
        texture::defaultTextureParams(texture::Generator::Paper).spec));
    CHECK(std::holds_alternative<texture::GrassParams>(
        texture::defaultTextureParams(texture::Generator::Grass).spec));
    CHECK(std::string(texture::generatorName(texture::Generator::Sky)) == "Sky");
    CHECK(std::string(texture::generatorName(texture::Generator::Paper)) == "Paper");
    CHECK(std::string(texture::generatorName(texture::Generator::Grass)) == "Grass");
}

TEST_CASE("renderTexture populates the per-generator cache lane") {
    const auto sky = texture::renderTexture(texture::defaultTextureParams(texture::Generator::Sky),
                                            32, 24);
    CHECK(!sky.image8.has_value());   // sky is the FLOAT lane (§4.4)
    REQUIRE(sky.imageF.has_value());
    CHECK(sky.imageF->width == 32);
    CHECK(sky.imageF->height == 24);

    for (const auto gen : {texture::Generator::Paper, texture::Generator::Grass}) {
        const auto r = texture::renderTexture(texture::defaultTextureParams(gen), 32, 24);
        REQUIRE(r.image8.has_value());  // paper/grass are the 8-bit lane
        CHECK(!r.imageF.has_value());
        CHECK(r.image8->width == 32);
        CHECK(r.image8->height == 24);
        // Both baselines render opaque surfaces.
        for (std::size_t i = 3; i < r.image8->rgba.size(); i += 4)
            CHECK(r.image8->rgba[i] == 255);
    }

    const auto degenerate =
        texture::renderTexture(texture::defaultTextureParams(texture::Generator::Sky), 0, 8);
    CHECK(!degenerate.image8.has_value());
    CHECK(!degenerate.imageF.has_value());
}

TEST_CASE("renderTexture is deterministic; seed and scale change the pixels") {
    texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Grass);
    p.seed = 7;
    const auto a = texture::renderTexture(p, 48, 48);
    const auto b = texture::renderTexture(p, 48, 48);
    REQUIRE(a.image8.has_value());
    REQUIRE(b.image8.has_value());
    CHECK(a.image8->rgba == b.image8->rgba);  // same params + seed => the same pixels (§8.3)

    texture::TextureParams reseeded = p;
    reseeded.seed = 8;
    const auto c = texture::renderTexture(reseeded, 48, 48);
    REQUIRE(c.image8.has_value());
    CHECK(a.image8->rgba != c.image8->rgba);

    texture::TextureParams rescaled = p;
    rescaled.scale = 3.0;
    const auto d = texture::renderTexture(rescaled, 48, 48);
    REQUIRE(d.image8.has_value());
    CHECK(a.image8->rgba != d.image8->rgba);
}

TEST_CASE("sky alpha carry: dome off yields a genuinely transparent layer (§3.4)") {
    texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
    auto& sky = std::get<texture::SkyParams>(p.spec);

    // Everything on: the dome fills opaque.
    const auto full = texture::renderTexture(p, 40, 30);
    REQUIRE(full.imageF.has_value());
    for (std::uint32_t y = 0; y < full.imageF->height; ++y)
        for (std::uint32_t x = 0; x < full.imageF->width; ++x)
            CHECK(full.imageF->at(x, y).a == doctest::Approx(1.0f));

    // Dome off, clouds on: coverage only where clouds write -- partially transparent.
    sky.enableDome = false;
    sky.enableSun = false;
    sky.enableHaze = false;
    sky.cloudCoverage = 0.5;
    const auto clouds = texture::renderTexture(p, 40, 30);
    REQUIRE(clouds.imageF.has_value());
    bool sawTransparent = false, sawCoverage = false;
    for (std::uint32_t y = 0; y < clouds.imageF->height; ++y) {
        for (std::uint32_t x = 0; x < clouds.imageF->width; ++x) {
            const float a = clouds.imageF->at(x, y).a;
            if (a < 0.01f) sawTransparent = true;
            if (a > 0.1f) sawCoverage = true;
        }
    }
    CHECK(sawTransparent);
    CHECK(sawCoverage);

    // Everything off: a fully transparent layer.
    sky.enableClouds = false;
    const auto none = texture::renderTexture(p, 40, 30);
    REQUIRE(none.imageF.has_value());
    for (std::uint32_t y = 0; y < none.imageF->height; ++y)
        for (std::uint32_t x = 0; x < none.imageF->width; ++x)
            CHECK(none.imageF->at(x, y).a == 0.0f);
}

TEST_CASE("TextureLayer cache staleness follows params edits and document size") {
    Document doc(64, 40);
    auto made = doc.makeTexture("Sky", texture::defaultTextureParams(texture::Generator::Sky));
    TextureLayer* layer = made.get();
    doc.root().addOnTop(std::move(made));

    CHECK(layer->kind() == core::LayerKind::Texture);
    CHECK(core::layerKindName(layer->kind()) == "Texture");
    CHECK(!layer->cacheCurrent());
    CHECK(!layer->contentBounds().has_value());  // unrendered: hits nothing, frames nothing

    CHECK(texture::refreshTextureCache(*layer, 64, 40));
    CHECK(layer->cacheCurrent());
    REQUIRE(layer->cachedImageF() != nullptr);  // sky = float lane
    CHECK(layer->cachedImage() == nullptr);
    REQUIRE(layer->contentBounds().has_value());
    CHECK(layer->contentBounds()->w == 64.0);
    CHECK(layer->contentBounds()->h == 40.0);
    CHECK(!texture::refreshTextureCache(*layer, 64, 40));  // current: no re-render

    // A params edit stales the cache.
    texture::TextureParams p = layer->params();
    std::get<texture::SkyParams>(p.spec).cloudCoverage = 0.9;
    layer->setParams(std::move(p));
    CHECK(!layer->cacheCurrent());
    CHECK(texture::refreshTextureCache(*layer, 64, 40));

    // A document resize stales it even though the params revision never moved.
    CHECK(texture::refreshTextureCache(*layer, 32, 20));
    REQUIRE(layer->cachedImageF() != nullptr);
    CHECK(layer->cachedImageF()->width == 32);

    // The tree walk covers nested groups and reports a dirty rect.
    auto group = doc.makeGroup("G");
    auto nested = doc.makeTexture("Paper", texture::defaultTextureParams(texture::Generator::Paper));
    TextureLayer* nestedPtr = nested.get();
    group->addOnTop(std::move(nested));
    doc.root().addOnTop(std::move(group));
    common::Rect dirty{};
    CHECK(texture::refreshTextureCaches(doc, &dirty));
    CHECK(nestedPtr->cacheCurrent());
    CHECK(nestedPtr->cachedImage() != nullptr);  // paper = 8-bit lane
    CHECK(!dirty.empty());
    CHECK(!texture::refreshTextureCaches(doc));  // everything current now
}

TEST_CASE("SetTextureCommand applies, undoes, and coalesces a gesture") {
    Document doc(16, 16);
    auto made = doc.makeTexture("Grass", texture::defaultTextureParams(texture::Generator::Grass));
    const LayerId id = made->id();
    doc.root().addOnTop(std::move(made));
    auto* layer = static_cast<TextureLayer*>(doc.find(id));
    const std::uint64_t rev0 = layer->contentRevision();

    texture::TextureParams edit = layer->params();
    std::get<texture::GrassParams>(edit.spec).patchiness = 0.9;
    push<core::SetTextureCommand>(doc, id, edit, std::string("Edit Texture"), /*coalesceId=*/3);
    CHECK(std::get<texture::GrassParams>(layer->params().spec).patchiness == 0.9);
    CHECK(layer->contentRevision() > rev0);  // the edit staled the cache

    // Two more slider ticks in the same gesture collapse into the same undo step.
    std::get<texture::GrassParams>(edit.spec).patchiness = 0.95;
    push<core::SetTextureCommand>(doc, id, edit, std::string("Edit Texture"), 3);
    std::get<texture::GrassParams>(edit.spec).patchiness = 1.0;
    push<core::SetTextureCommand>(doc, id, edit, std::string("Edit Texture"), 3);
    CHECK(std::get<texture::GrassParams>(layer->params().spec).patchiness == 1.0);

    doc.commands().undo();  // one step back to the pre-gesture value
    CHECK(std::get<texture::GrassParams>(layer->params().spec).patchiness ==
          texture::GrassParams{}.patchiness);
    doc.commands().redo();
    CHECK(std::get<texture::GrassParams>(layer->params().spec).patchiness == 1.0);

    // A different coalesce id starts a new step.
    std::get<texture::GrassParams>(edit.spec).clumpScale = 2.0;
    push<core::SetTextureCommand>(doc, id, edit, std::string("Edit Texture"), 4);
    doc.commands().undo();
    CHECK(std::get<texture::GrassParams>(layer->params().spec).clumpScale == 1.0);
    CHECK(std::get<texture::GrassParams>(layer->params().spec).patchiness == 1.0);
}

TEST_CASE("duplicateLayer copies texture params under a fresh id") {
    Document doc(16, 16);
    texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Paper);
    p.seed = 99;
    std::get<texture::PaperParams>(p.spec).roughness = 0.7;
    auto made = doc.makeTexture("Cardstock", p);
    const LayerId id = made->id();
    doc.root().addOnTop(std::move(made));

    const auto copy = doc.duplicateLayer(*doc.find(id));
    REQUIRE(copy != nullptr);
    CHECK(copy->id() != id);
    CHECK(copy->kind() == core::LayerKind::Texture);
    CHECK(static_cast<const TextureLayer&>(*copy).params() == p);
}

TEST_CASE("a texture layer composites through the document pipeline") {
    Document doc(48, 32);
    auto made = doc.makeTexture("Sky", texture::defaultTextureParams(texture::Generator::Sky));
    TextureLayer* layer = made.get();
    doc.root().addOnTop(std::move(made));
    texture::refreshTextureCaches(doc);

    render::CompositeOptions opts;  // checkerboard off: real alpha
    const render::CompositeResult r = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(r.ok);
    REQUIRE(r.image.width == 48);
    REQUIRE(r.image.height == 32);

    // The single-layer identity composite is exactly the float cache, quantised once: the float
    // lane reaches the accumulator unquantised (no 8-bit round-trip on the way).
    const common::Image direct = common::toImage8(*layer->cachedImageF());
    CHECK(r.image.rgba == direct.rgba);

    // Sanity that a real sky was rendered (the S55-b camera frames sky above a ground band; the
    // default sun sits near top-centre, so probe the away-from-sun left edge).
    const auto px = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t o = (static_cast<std::size_t>(y) * r.image.width + x) * 4;
        return std::array<int, 3>{r.image.rgba[o], r.image.rgba[o + 1], r.image.rgba[o + 2]};
    };
    const auto luma = [&](std::uint32_t x, std::uint32_t y) {
        const auto c = px(x, y);
        return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    };
    const auto top = px(0, 1);
    CHECK(top[2] > top[0]);            // high-elevation sky: blue dominates
    CHECK(luma(0, 30) < luma(0, 16));  // the below-horizon ground band is darker than the sky

    // The layer hits by its rendered extent (Move select-to-edit semantics).
    CHECK(core::topmostLayerAt(doc.root(), {24.0, 16.0}) == layer);

    // A mask folds into the float lane like any raster source.
    core::RasterMask mask(48, 32, 255);
    for (std::uint32_t y = 0; y < 32; ++y)
        for (std::uint32_t x = 0; x < 24; ++x)
            mask.coverage[y * 48 + x] = 0;
    layer->setMask(std::move(mask));
    const render::CompositeResult masked = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(masked.ok);
    CHECK(masked.image.rgba[(16 * 48 + 4) * 4 + 3] == 0);    // masked half: transparent
    CHECK(masked.image.rgba[(16 * 48 + 40) * 4 + 3] == 255); // open half: the opaque dome
}

TEST_CASE("crop keeps a texture generator canvas-locked and remaps its mask (S31/S55)") {
    // A texture generator is a canvas-locked procedural fill: refreshTextureCache regenerates it to
    // cover the whole canvas at identity. So a crop must NOT slide it by the rebase shift (that
    // displaces the regenerated fill -> an empty stripe) and must crop its document-window mask to
    // the kept window (else the doc-sized mask folds STRETCHED against the resized cache -- the
    // "mask moved, a masked-out band survived the crop, can't paint it out" report).
    Document doc(48, 32);
    auto made = doc.makeTexture("Paper", texture::defaultTextureParams(texture::Generator::Paper));
    auto* layer = made.get();
    core::RasterMask mask(48, 32, 255);
    for (std::uint32_t y = 0; y < 32; ++y)
        for (std::uint32_t x = 0; x < 24; ++x)
            mask.coverage[y * 48 + x] = 0;  // hide document columns [0,24)
    layer->setMask(std::move(mask));
    doc.root().addOnTop(std::move(made));
    texture::refreshTextureCaches(doc);

    // Delete-mode crop keeping document x in [16,48): a 32x32 canvas whose new origin is old x=16.
    doc.commands().push(render::buildCropCommand(doc, 16, 0, 32, 32, /*deletePixels=*/true));
    CHECK(doc.width() == 32);

    // Canvas-locked: the layer keeps identity (NOT slid by translate(-16,0)), so its regenerated
    // cache fills the whole new canvas instead of sliding off and leaving a stripe.
    CHECK(layer->transform() == common::Affine2D::identity());

    // The document-window mask was cropped to the kept window: the hidden band that spanned old
    // columns [0,24) now covers new columns [0,8) (old 16..23), the rest revealed.
    REQUIRE(layer->hasMask());
    const core::RasterMask* m = layer->mask();
    REQUIRE(m->width == 32);
    REQUIRE(m->height == 32);
    CHECK(m->coverage[16 * 32 + 2] == 0);     // new col 2 (old 18): still hidden
    CHECK(m->coverage[16 * 32 + 7] == 0);     // new col 7 (old 23): last hidden column
    CHECK(m->coverage[16 * 32 + 8] == 255);   // new col 8 (old 24): revealed
    CHECK(m->coverage[16 * 32 + 31] == 255);  // far edge: revealed

    // The cache regenerates to the new canvas size, so the 32-wide mask folds 1:1 against it.
    texture::refreshTextureCaches(doc);
    const bool sized = (layer->cachedImage() && layer->cachedImage()->width == 32) ||
                       (layer->cachedImageF() && layer->cachedImageF()->width == 32);
    CHECK(sized);

    // One undo restores the pre-crop canvas, transform and full-size mask.
    doc.commands().undo();
    CHECK(doc.width() == 48);
    CHECK(layer->transform() == common::Affine2D::identity());
    REQUIRE(layer->hasMask());
    CHECK(layer->mask()->width == 48);
    CHECK(layer->mask()->coverage[16 * 48 + 2] == 0);     // old col 2: hidden again
    CHECK(layer->mask()->coverage[16 * 48 + 40] == 255);  // old col 40: revealed
}

// ---- golden pins ---------------------------------------------------------------------------
// Recorded from linux-release and verified byte-identical on linux-debug (-O0) before blessing
// (the wash-golden discipline). 64x48, seed 42, defaults otherwise. If one fails, find the pixel
// that moved and why before re-blessing; the renderers have no licence to drift outside their
// replacement sessions (sky was re-blessed in S55-b and again in S55-c for the volumetric cloud
// lane; paper was re-blessed in S55-d for the real fibre/Sobel/Oren-Nayar renderer; grass was
// re-blessed in S55-e for the real distance-graded hybrid -- every generator now runs its real
// renderer, and no further re-bless is expected).

TEST_CASE("texture goldens (byte-pinned)") {
    texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
    p.seed = 42;
    const auto sky = texture::renderTexture(p, 64, 48);
    REQUIRE(sky.imageF.has_value());
    bool inRange = false;
    const std::uint64_t skyHash = hashImageF(*sky.imageF, &inRange);
    CHECK(inRange);  // [0, 8): finite, with the §4.4 solar-disc HDR headroom

    texture::TextureParams pp = texture::defaultTextureParams(texture::Generator::Paper);
    pp.seed = 42;
    const auto paper = texture::renderTexture(pp, 64, 48);
    REQUIRE(paper.image8.has_value());
    const std::uint64_t paperHash = fnv1a(paper.image8->rgba.data(), paper.image8->rgba.size());

    texture::TextureParams gp = texture::defaultTextureParams(texture::Generator::Grass);
    gp.seed = 42;
    const auto grass = texture::renderTexture(gp, 64, 48);
    REQUIRE(grass.image8.has_value());
    const std::uint64_t grassHash = fnv1a(grass.image8->rgba.data(), grass.image8->rgba.size());

    CHECK(skyHash == 6565761780459925611ull);  // volumetric-banding re-bless: the marcher now
                                               // slant-scales its step count and jitters each
                                               // ray's sample lattice by a per-FRAME-pixel hash
                                               // (deterministic -> still parallel/crop-exact), so
                                               // the default deck's cumulus pixels move. The only
                                               // day-visible change of its batch (moon relief is
                                               // moon-off-by-default; twilight scattering is gated
                                               // to the sub-horizon sun -- both proven inert here).
                                               // Prior: 15307404083731329921 (night-overhaul dither).
    CHECK(paperHash == 6902453368357319705ull);  // S55-d re-bless: the real paper renderer (fibre/
                                                 // Sobel/Oren-Nayar) replaces the S55-a baseline
    CHECK(grassHash == 11971561961918341435ull);  // S55-e re-bless: the distance-graded hybrid
                                                   // (homography turf base + Bezier-blade instancing)
                                                   // replaces the S55-a turf-only baseline
}

TEST_CASE("material goldens (byte-pinned)") {
    // The S55-g materials, pinned at birth under the same procedure (64x48, seed 42, defaults;
    // recorded from linux-release, verified byte-identical on linux-debug). The sky/paper/grass
    // pins above are UNTOUCHED by S55-g -- the registry refactor and these additions moved no
    // existing pixel.
    const auto pin = [](texture::Generator g) {
        texture::TextureParams p = texture::defaultTextureParams(g);
        p.seed = 42;
        const auto r = texture::renderTexture(p, 64, 48);
        REQUIRE(r.image8.has_value());  // every material renders the 8-bit lane
        return fnv1a(r.image8->rgba.data(), r.image8->rgba.size());
    };
    CHECK(pin(texture::Generator::Wood) == 10091008802904838105ull);
    CHECK(pin(texture::Generator::Marble) == 15809217301746616895ull);
    CHECK(pin(texture::Generator::Stone) == 5426541401162512905ull);
    CHECK(pin(texture::Generator::Canvas) == 16440316409148717988ull);
    CHECK(pin(texture::Generator::Metal) == 15259068460268106427ull);
}
