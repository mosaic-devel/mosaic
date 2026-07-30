#include "core/clipboard.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/selection.hpp"
#include "render/compositor.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// Coverage partitions (core::CoveragePartition). Cutting a feathered -- or merely anti-aliased --
// selection splits one surface into two layers holding A*m and A*(1-m). Porter-Duff `over` assumes
// those coverages are independent and charges m*(1-m) to an overlap that does not exist, so
// recombining them yields A - r*f instead of A: up to 25% missing alpha at the half-coverage
// contour, which is the translucent rim along the cut.
//
// The fix rewrites the RESIDUAL's alpha to b = r/(1-f) while the partition is live, so plain `over`
// reconstructs the surface exactly. Because `over` is associative, that holds wherever the fragment
// ends up above the hole -- directly on top, several layers up, or nested in groups -- which is
// what these tests pin, alongside every way the link is supposed to retire.
namespace {

using mosaic::common::Affine2D;
using mosaic::common::Image;
using mosaic::common::Vec2;
using mosaic::core::copyFromLayer;
using mosaic::core::Document;
using mosaic::core::GroupLayer;
using mosaic::core::imageWithSelectionCleared;
using mosaic::core::Layer;
using mosaic::core::linkCoveragePartition;
using mosaic::core::partitionEligibleSource;
using mosaic::core::RasterLayer;
using mosaic::core::Selection;

std::uint8_t alphaAt(const Image& img, std::uint32_t x, std::uint32_t y) {
    return img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3];
}

Image flatten(const Document& doc) {
    const auto res = mosaic::render::composite(doc, {}, mosaic::render::Backend::Cpu);
    REQUIRE(res.ok);
    return res.image;
}

// An opaque photo-like base layer: colour varies in both axes so a wrong reconstruction shows up in
// RGB as well as alpha.
std::unique_ptr<Document> makeDoc(std::uint32_t w = 16, std::uint32_t h = 16) {
    auto doc = std::make_unique<Document>(w, h);
    auto layer = doc->makeRaster("Base", w, h);
    Image& img = layer->image();
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            img.rgba[p + 0] = static_cast<std::uint8_t>(20 + x * 13);
            img.rgba[p + 1] = static_cast<std::uint8_t>(40 + y * 11);
            img.rgba[p + 2] = 200;
            img.rgba[p + 3] = 255;
        }
    layer->invalidateContentBounds();
    doc->root().addOnTop(std::move(layer));
    return doc;
}

struct Halves {
    RasterLayer* residual = nullptr;
    RasterLayer* fragment = nullptr;
};

// What app_window's cutSelection + pasteContent(atSource=true) do, minus the command stack: lift
// the coverage, erase its complement from the source, paste the piece back at its own document
// position on top, link the two halves.
Halves cutAndPasteInPlace(Document& doc, Layer& source, const Selection& sel) {
    auto content = copyFromLayer(source, sel, doc.width(), doc.height());
    REQUIRE(content.has_value());
    auto cleared = imageWithSelectionCleared(source, sel);
    REQUIRE(cleared.has_value());
    auto* residual = source.as<RasterLayer>();
    REQUIRE(residual != nullptr);
    residual->image() = std::move(*cleared);
    residual->invalidateContentBounds();

    auto frag = doc.makeRaster("cutout", content->image.width, content->image.height);
    frag->image() = std::move(content->image);
    frag->invalidateContentBounds();
    frag->setTransform(Affine2D::translation(content->docX, content->docY));
    auto* fragPtr = frag.get();
    doc.root().addOnTop(std::move(frag));
    linkCoveragePartition(*residual, *fragPtr);
    return {residual, fragPtr};
}

// A deterministic coverage ramp across an 8x4 canvas, uniform down each column, so a single named
// column pins an exact arithmetic outcome. Column 3 is the half-coverage contour -- the worst case.
constexpr std::uint8_t kRamp[8] = {0, 32, 64, 128, 192, 255, 255, 255};
constexpr std::uint32_t kHalfCol = 3;

Selection rampSelection() {
    Selection sel(8, 4);
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 8; ++x) sel.data()[y * 8 + x] = kRamp[x];
    return sel;
}

// Move `id` out of the root and into a fresh group (itself pushed on top) -- "drag the cutout into
// a group layer", the gesture that used to bring the rim straight back.
GroupLayer* wrapInGroup(Document& doc, mosaic::core::LayerId id, float groupOpacity = 1.0f) {
    const std::size_t idx = doc.root().indexOf(id);
    REQUIRE(idx != GroupLayer::npos);
    std::unique_ptr<Layer> taken = doc.root().removeAt(idx);
    auto group = doc.makeGroup("Cutouts");
    group->setOpacity(groupOpacity);
    group->addOnTop(std::move(taken));
    auto* raw = group.get();
    doc.root().addOnTop(std::move(group));
    return raw;
}

}  // namespace

TEST_CASE("cut: the two halves partition the source alpha exactly in 8 bits") {
    // Every (alpha, coverage) pair must satisfy fragment + residual == original. Independent
    // rounding of a*cov/255 and a*(255-cov)/255 loses up to 2/255 on top of the `over` seam.
    Document doc(4, 4);
    for (int a = 0; a <= 255; a += 5) {
        for (int cov = 0; cov <= 255; cov += 5) {
            auto layer = doc.makeRaster("L", 1, 1);
            layer->image().rgba = {9, 9, 9, static_cast<std::uint8_t>(a)};
            Selection sel(4, 4);
            sel.data()[0] = static_cast<std::uint8_t>(cov);
            const auto lifted = copyFromLayer(*layer, sel, 4, 4);
            const auto cleared = imageWithSelectionCleared(*layer, sel);
            const int f = lifted ? lifted->image.rgba[3] : 0;
            const int r = cleared ? cleared->rgba[3] : a;
            CHECK(f + r == a);
        }
    }
}

TEST_CASE("a feathered cut pasted back in place recomposites to the original, byte for byte") {
    auto doc = makeDoc();
    const Image before = flatten(*doc);

    const Selection sel = Selection::ellipse(16, 16, {3.0, 3.0, 10.0, 10.0}).feathered(2.0);
    const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), sel);
    REQUIRE(h.fragment != nullptr);

    CHECK(flatten(*doc).rgba == before.rgba);
}

TEST_CASE("an anti-aliased lasso with NO feather is the same defect, and the same fix") {
    // The rim does not need feathering: Selection::polygon anti-aliases, so the 1px boundary already
    // carries fractional coverage and `over` already loses a quarter of it at the midpoint.
    auto doc = makeDoc();
    const Image before = flatten(*doc);

    const std::vector<Vec2> lasso{{2.5, 3.5}, {12.0, 2.0}, {13.5, 11.5}, {4.0, 13.0}};
    const Selection sel = Selection::polygon(16, 16, lasso);
    cutAndPasteInPlace(*doc, doc->root().child(0), sel);

    CHECK(flatten(*doc).rgba == before.rgba);
}

TEST_CASE("the rim is real: without the link, `over` loses a quarter of the alpha at m = 0.5") {
    auto doc = makeDoc(8, 4);
    const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), rampSelection());

    // round(255 * 128/255) = 128 lifted, 127 left behind.
    // The fragment is cropped to the selection's bounds (column 0 has no coverage), so document
    // column kHalfCol is its column kHalfCol-1.
    CHECK(h.fragment->image().rgba[(kHalfCol - 1) * 4 + 3] == 128);
    CHECK(h.residual->image().rgba[kHalfCol * 4 + 3] == 127);

    CHECK(alphaAt(flatten(*doc), kHalfCol, 0) == 255);  // reconstructed

    // Drop the link and the very same pixels composite to 128/255 + 127/255*(1 - 128/255) = 0.750.
    h.residual->setPartition(std::nullopt);
    CHECK(alphaAt(flatten(*doc), kHalfCol, 0) == 191);
}

TEST_CASE("the reconstruction survives the cutout being organised away from its hole") {
    const Selection sel = Selection::ellipse(16, 16, {3.0, 3.0, 10.0, 10.0}).feathered(2.0);

    SUBCASE("dropped into a group") {
        auto doc = makeDoc();
        const Image before = flatten(*doc);
        const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), sel);
        wrapInGroup(*doc, h.fragment->id());
        CHECK(flatten(*doc).rgba == before.rgba);
    }

    SUBCASE("dropped into a group inside a group") {
        auto doc = makeDoc();
        const Image before = flatten(*doc);
        const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), sel);
        GroupLayer* inner = wrapInGroup(*doc, h.fragment->id());
        wrapInGroup(*doc, inner->id());
        CHECK(flatten(*doc).rgba == before.rgba);
    }

    SUBCASE("several layers further up the stack") {
        auto doc = makeDoc();
        const Image before = flatten(*doc);
        const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), sel);
        // Two empty layers slid between the hole and the piece. `over` is associative, so the
        // reconstruction does not care what sits in the gap.
        const std::size_t fragIdx = doc->root().indexOf(h.fragment->id());
        doc->root().insert(fragIdx, doc->makeRaster("spacer A", 16, 16));
        doc->root().insert(fragIdx, doc->makeRaster("spacer B", 16, 16));
        CHECK(flatten(*doc).rgba == before.rgba);
    }

    SUBCASE("the hole itself grouped, the piece left at the root") {
        auto doc = makeDoc();
        const Image before = flatten(*doc);
        const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), sel);
        wrapInGroup(*doc, h.residual->id());
        // The group went on TOP, so put it back under the fragment.
        const std::size_t gi = doc->root().childCount() - 1;
        std::unique_ptr<Layer> g = doc->root().removeAt(gi);
        doc->root().insert(0, std::move(g));
        CHECK(flatten(*doc).rgba == before.rgba);
    }
}

TEST_CASE("the link retires the moment the halves stop tiling") {
    // Each of these genuinely changes what lands on the seam, so falling back to `over` -- rim and
    // all -- is the honest answer rather than a regression.
    // 128/255 + 127/255*(1 - 128/255) = 0.750 -> 191: the quarter of the alpha `over` drops.
    const auto rimAlpha = [](Document& doc) { return static_cast<int>(alphaAt(flatten(doc), kHalfCol, 0)); };

    SUBCASE("the piece is moved") {
        auto doc = makeDoc(8, 4);
        const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), rampSelection());
        h.fragment->setTransform(Affine2D::translation(4.0, 2.0));
        // The piece has slid off this column entirely, so what is left is the hole ALONE, still
        // carrying its true soft edge (127) rather than the hardened shape the rewrite would give.
        // That soft hole is the whole reason the reconstruction is a compositing-time reading
        // instead of something baked into the pixels at cut time.
        CHECK(rimAlpha(*doc) == 127);
    }

    SUBCASE("the piece takes an UNLINKED mask") {
        // A linked mask rides the fragment's own grid and folds into the reconstruction (see
        // below); an unlinked one folds in parent space AFTER placement, which the per-pixel read
        // cannot mirror, so this one really does retire.
        auto doc = makeDoc(8, 4);
        const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), rampSelection());
        mosaic::core::RasterMask m(h.fragment->image().width, h.fragment->image().height, 255);
        m.linked = false;
        h.fragment->setMask(std::move(m));
        CHECK(rimAlpha(*doc) == 191);
    }

    SUBCASE("the hole is repainted") {
        auto doc = makeDoc(8, 4);
        const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), rampSelection());
        h.residual->image().rgba[kHalfCol * 4 + 3] = 200;  // coverage changed: no longer the hole
        h.residual->invalidateContentBounds();
        CHECK(rimAlpha(*doc) != 255);
    }

    SUBCASE("the hole takes a mask") {
        // The LOWER half is the one being rewritten, so it must reach the composite at exactly the
        // alpha it stores -- a mask (or reduced opacity) on it scales the rewrite too.
        auto doc = makeDoc(8, 4);
        const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), rampSelection());
        h.residual->setMask(mosaic::core::RasterMask(8, 4, 255));
        CHECK(rimAlpha(*doc) == 191);
    }

    SUBCASE("the piece is clipped to below") {
        auto doc = makeDoc(8, 4);
        const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), rampSelection());
        h.fragment->setClipToBelow(true);
        // Clipping multiplies the piece's alpha by the clip BASE's -- an attenuation that depends
        // on a layer the pair knows nothing about, so the rewrite stands down and `over` runs on
        // 0.502*0.498 over 0.498 = 0.624.
        CHECK(rimAlpha(*doc) == 159);
    }
}

TEST_CASE("a partition is symmetric: swapping which half is on top still reconstructs") {
    // Neither half is privileged -- they are two complementary parts of one surface, and whichever
    // ends up underneath is the one whose alpha gets rewritten. Reordering them past each other is
    // therefore not a reason to retire the link.
    auto doc = makeDoc();
    const Image before = flatten(*doc);
    const Selection sel = Selection::ellipse(16, 16, {3.0, 3.0, 10.0, 10.0}).feathered(2.0);
    const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), sel);

    std::unique_ptr<Layer> frag = doc->root().removeAt(doc->root().indexOf(h.fragment->id()));
    doc->root().insert(0, std::move(frag));
    CHECK(flatten(*doc).rgba == before.rgba);
}

TEST_CASE("an isolated render of the hole shows the TRUE soft hole, not the reconstruction shape") {
    // Rasterize / Merge Down / thumbnails bake what the layer actually stores. The rewrite is a
    // compositing-time reading that only makes sense with the piece overhead.
    auto doc = makeDoc(8, 4);
    const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), rampSelection());
    CHECK(alphaAt(flatten(*doc), kHalfCol, 0) == 255);

    const Image baked = mosaic::render::rasterizeLayer(*h.residual, 8, 4);
    CHECK(alphaAt(baked, kHalfCol, 0) == 127);
}

TEST_CASE("partitionEligibleSource: only a raster on an integer-translated grid can partition") {
    auto doc = makeDoc();
    Layer& base = doc->root().child(0);
    CHECK(partitionEligibleSource(base));

    base.setTransform(Affine2D::translation(4.0, -7.0));
    CHECK(partitionEligibleSource(base));

    // Half-pixel placement, rotation and scale all resample the two halves onto grids that
    // disagree, which would trade a faint rim for a hard one.
    base.setTransform(Affine2D::translation(4.5, 0.0));
    CHECK_FALSE(partitionEligibleSource(base));
    base.setTransform(Affine2D::rotation(0.3));
    CHECK_FALSE(partitionEligibleSource(base));
    base.setTransform(Affine2D::scaling(2.0, 2.0));
    CHECK_FALSE(partitionEligibleSource(base));

    base.setTransform(Affine2D::identity());
    base.setMask(mosaic::core::RasterMask(16, 16, 255));
    CHECK_FALSE(partitionEligibleSource(base));

    CHECK_FALSE(partitionEligibleSource(*doc->makeGroup("g")));
}

TEST_CASE("restyling the cutout: what survives, and what honestly cannot") {
    // The complaint this answers: a fix that only survives an untouched paste is not a fix. Alpha
    // compositing is blend-independent and an attenuated fragment has a well-defined target of its
    // own, so neither blend mode nor opacity has any business retiring the reconstruction.
    SUBCASE("a blend mode retires it, because colour is NOT blend-independent") {
        // Alpha compositing is blend-independent, so the alpha alone would reconstruct. The colour
        // would not: in the feather band the piece blends against the filled hole, in the fully-cut
        // core against the real backdrop, and that discontinuity draws its own ring (with Subtract,
        // a strikingly dark one). So a blended piece is honoured literally instead.
        for (const auto mode : {mosaic::core::BlendMode::Subtract, mosaic::core::BlendMode::Multiply,
                                mosaic::core::BlendMode::Difference}) {
            auto doc = makeDoc(8, 4);
            const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), rampSelection());
            h.fragment->setBlendMode(mode);
            CAPTURE(mosaic::core::blendModeName(mode));
            CHECK(alphaAt(flatten(*doc), kHalfCol, 0) == 191);
        }
    }

    SUBCASE("opacity slides continuously from reassembled to bare hole") {
        // Target alpha is r + k*f: 127/255 + k*128/255. No cliff anywhere on the way down, and the
        // endpoints are the whole surface (255) and the hole alone (127).
        const int expected[] = {255, 223, 191, 159, 127};
        int i = 0;
        for (const float k : {1.0f, 0.75f, 0.5f, 0.25f, 0.0f}) {
            auto doc = makeDoc(8, 4);
            const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), rampSelection());
            h.fragment->setOpacity(k);
            CAPTURE(k);
            CHECK(alphaAt(flatten(*doc), kHalfCol, 0) == expected[i++]);
        }
    }

    SUBCASE("a dimmed group wrapping the cutout attenuates it the same way") {
        auto doc = makeDoc(8, 4);
        const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), rampSelection());
        wrapInGroup(*doc, h.fragment->id(), /*groupOpacity=*/0.5f);
        CHECK(alphaAt(flatten(*doc), kHalfCol, 0) == 191);
    }

    SUBCASE("a linked mask on the cutout folds into the reconstruction") {
        auto doc = makeDoc(8, 4);
        const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), rampSelection());
        mosaic::core::RasterMask m(h.fragment->image().width, h.fragment->image().height, 128);
        h.fragment->setMask(std::move(m));
        // Target is r + k*f = 127/255 + (128/255 * 128/255) = 0.750 -> 191.
        CHECK(alphaAt(flatten(*doc), kHalfCol, 0) == 191);
    }
}

TEST_CASE("undo/redo back onto the split pixels revives the partition") {
    // Liveness keys on the coverage's FINGERPRINT, not on the edit counter: undo restores
    // byte-identical pixels through a fresh command, so a revision comparison would leave the
    // partition retired forever and the rim would reappear on the way back through history.
    auto doc = makeDoc(8, 4);
    const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), rampSelection());
    CHECK(alphaAt(flatten(*doc), kHalfCol, 0) == 255);

    const Image holePixels = h.residual->image();
    h.residual->image().rgba[kHalfCol * 4 + 3] = 200;  // an edit that retires it
    h.residual->invalidateContentBounds();
    CHECK(alphaAt(flatten(*doc), kHalfCol, 0) != 255);

    h.residual->image() = holePixels;  // ... and stepping back onto the same coverage
    h.residual->invalidateContentBounds();
    CHECK(h.residual->contentRevision() != 0);  // the counter has moved on regardless
    CHECK(alphaAt(flatten(*doc), kHalfCol, 0) == 255);
}

TEST_CASE("Merge Down bakes the reconstruction, not the rim") {
    // The one irreversible path: `over` here would freeze the gap into the pixels for good.
    auto doc = makeDoc();
    const Image before = flatten(*doc);
    const Selection sel = Selection::ellipse(16, 16, {3.0, 3.0, 10.0, 10.0}).feathered(2.0);
    const Halves h = cutAndPasteInPlace(*doc, doc->root().child(0), sel);

    const auto merged = mosaic::render::mergeDown(*h.fragment, *h.residual);
    REQUIRE(merged.has_value());
    CHECK(merged->rgba == before.rgba);
}
