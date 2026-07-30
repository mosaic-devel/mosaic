#include "core/clipboard.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/selection.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <utility>

// The S14-b clipboard model: copy (layer / merged), cut's clear, paste placement, and the
// white-flatten used for OS-clipboard interop, plus the SetLayerPixelsCommand round trip.
namespace {

using mosaic::common::Affine2D;
using mosaic::common::Image;
using mosaic::core::ClipboardContent;
using mosaic::core::copyFromLayer;
using mosaic::core::copyMerged;
using mosaic::core::Document;
using mosaic::core::flattenedOverWhite;
using mosaic::core::imageWithSelectionCleared;
using mosaic::core::pastePosition;
using mosaic::core::Selection;

void setPixel(Image& img, std::uint32_t x, std::uint32_t y, std::uint8_t r, std::uint8_t g,
              std::uint8_t b, std::uint8_t a) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    img.rgba[p] = r;
    img.rgba[p + 1] = g;
    img.rgba[p + 2] = b;
    img.rgba[p + 3] = a;
}

std::uint8_t alphaAt(const Image& img, std::uint32_t x, std::uint32_t y) {
    return img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3];
}

} // namespace

TEST_CASE("copyFromLayer: selection crops, masks alpha, and records the source position") {
    Document doc(8, 8);
    auto layer = doc.makeRaster("L"); // document-sized, identity transform
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 8; ++x)
            setPixel(layer->image(), x, y, 100, 150, 200, 255);

    const Selection sel = Selection::rectangle(8, 8, {2, 3, 4, 2});
    const auto content = copyFromLayer(*layer, sel, 8, 8);
    REQUIRE(content.has_value());
    CHECK(content->docX == 2);
    CHECK(content->docY == 3);
    CHECK(content->image.width == 4);
    CHECK(content->image.height == 2);
    CHECK(alphaAt(content->image, 0, 0) == 255);
    CHECK(content->image.rgba[2] == 200); // colour channels survive

    // Partial coverage multiplies alpha.
    Selection half(8, 8);
    half.data()[3 * 8 + 2] = 128; // one pixel at (2,3), half-covered
    const auto faded = copyFromLayer(*layer, half, 8, 8);
    REQUIRE(faded.has_value());
    CHECK(faded->image.width == 1);
    CHECK(alphaAt(faded->image, 0, 0) == 128);

    // An empty selection copies the whole layer.
    const auto whole = copyFromLayer(*layer, Selection{}, 8, 8);
    REQUIRE(whole.has_value());
    CHECK(whole->image.width == 8);
    CHECK(whole->image.height == 8);
    CHECK(whole->docX == 0);

    // A selection over fully transparent pixels copies nothing.
    auto clear = doc.makeRaster("empty");
    CHECK_FALSE(copyFromLayer(*clear, sel, 8, 8).has_value());

    // Groups have no pixels to copy.
    const auto group = doc.makeGroup("g");
    CHECK_FALSE(copyFromLayer(*group, sel, 8, 8).has_value());
}

TEST_CASE("copyFromLayer: provenance + the whole-layer style (paste-semantics rules)") {
    Document doc(8, 8);
    auto layer = doc.makeRaster("Sky");
    setPixel(layer->image(), 2, 2, 10, 20, 30, 255);
    layer->setOpacity(0.5f);
    layer->setBlendMode(mosaic::core::BlendMode::Multiply);

    // A pixel-selection copy records WHERE the pixels came from, but no style: it pastes as
    // anonymous pixel data ("Selection from Sky"), not as a layer duplicate.
    const Selection sel = Selection::rectangle(8, 8, {2, 2, 2, 2});
    const auto partial = copyFromLayer(*layer, sel, 8, 8);
    REQUIRE(partial.has_value());
    CHECK(partial->sourceName == "Sky");
    CHECK_FALSE(partial->style.has_value());

    // A whole-layer copy (no selection) carries the layer's restorable style.
    const auto whole = copyFromLayer(*layer, Selection{}, 8, 8);
    REQUIRE(whole.has_value());
    CHECK(whole->sourceName == "Sky");
    REQUIRE(whole->style.has_value());
    CHECK(whole->style->name == "Sky");
    CHECK(whole->style->opacity == 0.5f);
    CHECK(whole->style->blend == mosaic::core::BlendMode::Multiply);
}

TEST_CASE("the pasted marker clears on rename and survives undo") {
    Document doc(8, 8);
    auto fresh = doc.makeRaster("Selection from Sky");
    fresh->setPastedMarker(true);
    auto* layer = &doc.root().addOnTop(std::move(fresh));

    doc.commands().push(
        std::make_unique<mosaic::core::SetNameCommand>(layer->id(), "Clouds"));
    CHECK(layer->name() == "Clouds");
    CHECK_FALSE(layer->pastedMarker()); // naming the paste adopts it

    doc.commands().undo();
    CHECK(layer->name() == "Selection from Sky");
    CHECK(layer->pastedMarker()); // undo restores the badge with the name

    doc.commands().redo();
    CHECK_FALSE(layer->pastedMarker());
}

TEST_CASE("copyFromLayer: a translated layer samples through its transform") {
    Document doc(8, 8);
    auto layer = doc.makeRaster("small", 2, 2);
    setPixel(layer->image(), 0, 0, 10, 20, 30, 255);
    setPixel(layer->image(), 1, 1, 40, 50, 60, 200);
    layer->setTransform(Affine2D::translation(4, 4));

    // Empty selection: the region is the layer's transformed extent.
    const auto content = copyFromLayer(*layer, Selection{}, 8, 8);
    REQUIRE(content.has_value());
    CHECK(content->docX == 4);
    CHECK(content->docY == 4);
    CHECK(content->image.width == 2);
    CHECK(content->image.rgba[0] == 10); // (4,4) -> layer (0,0)
    CHECK(alphaAt(content->image, 1, 1) == 200);
}

TEST_CASE("copyMerged: crops the flattened composite under the selection") {
    Image composite(6, 6);
    for (std::uint32_t y = 0; y < 6; ++y)
        for (std::uint32_t x = 0; x < 6; ++x)
            setPixel(composite, x, y, static_cast<std::uint8_t>(x * 40), 0, 0, 255);

    const Selection sel = Selection::rectangle(6, 6, {1, 1, 3, 3});
    const auto content = copyMerged(composite, sel);
    REQUIRE(content.has_value());
    CHECK(content->docX == 1);
    CHECK(content->image.width == 3);
    CHECK(content->image.rgba[0] == 40); // column 1 of the composite

    const auto whole = copyMerged(composite, Selection{});
    REQUIRE(whole.has_value());
    CHECK(whole->image.width == 6);

    CHECK_FALSE(copyMerged(composite, Selection(6, 6)).has_value()); // active-but-empty mask
}

TEST_CASE("imageWithSelectionCleared: erases coverage from alpha; no-ops return nullopt") {
    Document doc(4, 4);
    auto layer = doc.makeRaster("L");
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 4; ++x)
            setPixel(layer->image(), x, y, 9, 9, 9, 200);

    Selection sel(4, 4);
    sel.data()[0] = 255; // (0,0) fully selected
    sel.data()[1] = 128; // (1,0) half
    const auto cleared = imageWithSelectionCleared(*layer, sel);
    REQUIRE(cleared.has_value());
    CHECK(alphaAt(*cleared, 0, 0) == 0);
    // The residual is the COMPLEMENT of what the copy lifted, not an independent (255-cov) scale:
    // round(200*128/255) = 100 lifted, so 100 stays. (Both halves used to truncate separately and
    // summed to 199 — a 1/255 leak on top of the structural `over` seam the partition fixes.)
    CHECK(alphaAt(*cleared, 1, 0) == 100);
    CHECK(alphaAt(*cleared, 2, 2) == 200); // untouched

    // An empty selection means cut-the-whole-layer.
    const auto all = imageWithSelectionCleared(*layer, Selection{});
    REQUIRE(all.has_value());
    CHECK(alphaAt(*all, 3, 3) == 0);

    // A selection that misses the layer changes nothing -> nullopt (skip the undo step).
    Selection miss(4, 4); // selects nothing
    CHECK_FALSE(imageWithSelectionCleared(*layer, miss).has_value());

    const auto group = doc.makeGroup("g");
    CHECK_FALSE(imageWithSelectionCleared(*group, sel).has_value());
}

TEST_CASE("pastePosition: source position in-document, centred otherwise") {
    CHECK(pastePosition(4, 2, std::pair{3, 5}, 16, 16) == std::pair{3, 5});
    CHECK(pastePosition(4, 2, std::nullopt, 16, 16) == std::pair{6, 7});
    CHECK(pastePosition(20, 20, std::nullopt, 16, 16) == std::pair{-2, -2}); // larger: negative
}

TEST_CASE("flattenedOverWhite: alpha composites against white and goes opaque") {
    Image img(2, 1);
    setPixel(img, 0, 0, 0, 0, 0, 0);     // fully transparent -> white
    setPixel(img, 1, 0, 0, 0, 0, 255);   // opaque black stays black
    const Image flat = flattenedOverWhite(img);
    CHECK(flat.rgba[0] == 255);
    CHECK(flat.rgba[3] == 255);
    CHECK(flat.rgba[4] == 0);
    CHECK(flat.rgba[7] == 255);
}

TEST_CASE("SetLayerPixelsCommand: apply/undo round-trips the raster image") {
    Document doc(2, 2);
    auto layer = doc.makeRaster("L");
    setPixel(layer->image(), 0, 0, 1, 2, 3, 255);
    const Image before = layer->image();
    const auto id = layer->id();
    doc.root().addOnTop(std::move(layer));

    Image next(2, 2);
    setPixel(next, 1, 1, 7, 8, 9, 100);
    doc.commands().push(std::make_unique<mosaic::core::SetLayerPixelsCommand>(id, next));
    CHECK(doc.find(id)->as<mosaic::core::RasterLayer>()->image() == next);
    doc.commands().undo();
    CHECK(doc.find(id)->as<mosaic::core::RasterLayer>()->image() == before);
    doc.commands().redo();
    CHECK(doc.find(id)->as<mosaic::core::RasterLayer>()->image() == next);
}
