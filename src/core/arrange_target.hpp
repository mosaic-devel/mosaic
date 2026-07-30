#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "common/geometry.hpp"
#include "core/layer.hpp"

// What the Arrange menu (core/arrange.hpp) acts ON: which layers, and -- for each -- the box that
// "aligned" has to be true of afterwards, plus the second half of the edit that box implies.
//
// arrange.hpp is pure rect maths that knows nothing about a document; this header is the other
// half, the one that turns a layer into a rect and a rect delta back into an edit. It exists
// because Arrange used to ask every layer for `worldTransform * contentBounds` and nothing else,
// which is wrong for two whole families:
//
//   * an AdjustmentLayer -- every adjustment and filter kind -- has NO contentBounds override at
//     all, so the base returns nullopt and Arrange skipped it outright. A masked Curves or a masked
//     blur is a REGION of the image and has a perfectly good position; it just lives in the mask.
//   * a texture generator, a gradient layer, a canvas-sized raster: their content IS the canvas, so
//     every align against the canvas is a no-op, even though the visible blob -- the part the mask
//     lets through -- is small and plainly off-centre.
//
// One answer covers both: the box that matters is what is actually VISIBLE, which is the content
// box intersected with the mask's coverage. A plain unmasked raster/shape/text layer lands on
// exactly the rect it landed on before (arrangeBounds is literally the old expression on that
// path), so widening the menu costs nothing that already worked.
//
// FLTK-free and pure, so the whole thing is pinned headlessly (tests/test_arrange.cpp).
namespace mosaic::core {

// The tight bbox of the mask cells that let ANYTHING through (coverage > 0), in MASK PIXEL space.
// Cell (x, y) spans [x, x+1) x [y, y+1) -- the same convention SetMaskPixelsCommand::dirtyRegion
// hands to the same map. nullopt for a sheet with no cells, a truncated coverage plane, or one that
// is uniformly zero (see maskDocumentBounds for what each of those means).
//
// COST: O(mask px), and nothing is cached. Deliberate: this runs once per Arrange menu invocation,
// never per frame, and a memoised bbox would have to be invalidated from every mask-paint dab, every
// mask command and every undo -- a whole staleness surface bought for an operation the user cannot
// perceive the cost of.
[[nodiscard]] inline std::optional<common::Rect> maskCoverageBounds(const RasterMask& mask) {
    if (mask.empty() || mask.coverage.size() < static_cast<std::size_t>(mask.width) * mask.height)
        return std::nullopt;
    std::uint32_t minX = mask.width, minY = mask.height, maxX = 0, maxY = 0;
    bool any = false;
    for (std::uint32_t y = 0; y < mask.height; ++y) {
        const std::uint8_t* row = mask.coverage.data() + static_cast<std::size_t>(y) * mask.width;
        for (std::uint32_t x = 0; x < mask.width; ++x) {
            if (row[x] == 0)
                continue;
            any = true;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }
    if (!any)
        return std::nullopt;
    return common::Rect{static_cast<double>(minX), static_cast<double>(minY),
                        static_cast<double>(maxX - minX + 1),
                        static_cast<double>(maxY - minY + 1)};
}

// Where `layer`'s mask lets light through, in DOCUMENT space: maskCoverageBounds mapped through
// core::maskToDocument -- the ONLY correct mask-px -> document map (the grid contract in
// core/layer.hpp: the sheet's captured placement, under the layer's world transform when linked and
// its parent's when not). Composing those transforms by hand here is exactly the disagreement with
// the compositor that the contract's comment forbids.
//
// nullopt in four cases which do NOT all mean the same thing -- see maskIsLive below, which is what
// tells them apart:
//   * no mask at all;
//   * a DISABLED mask -- the compositor ignores it, so it must not decide where the layer sits;
//   * an EMPTY SHEET (0 x 0) -- the compositor also treats that as no mask (compositor.cpp maps
//     `mask->empty()` to nullptr), so a sheet that was never given cells cannot hide anything;
//   * ... and coverage that is uniformly ZERO, which is the opposite case: such a mask is live and
//     hides the layer completely, so the layer has no visible box at all.
[[nodiscard]] inline std::optional<common::Rect> maskDocumentBounds(const Layer& layer) {
    const RasterMask* mask = layer.mask();
    if (mask == nullptr || !mask->enabled || mask->empty())
        return std::nullopt;
    const std::optional<common::Rect> cells = maskCoverageBounds(*mask);
    if (!cells)
        return std::nullopt;
    return maskToDocument(layer, *mask).mapBounds(*cells);
}

// Does this layer have a mask the compositor will actually fold? (The compositor's own test, twice
// over: `mask != nullptr && enabled && !empty()`.) A false here means "no mask", so a nullopt from
// maskDocumentBounds is an absent opinion; a true means a nullopt is a verdict -- the sheet is live
// and lets nothing through.
[[nodiscard]] inline bool maskIsLive(const Layer& layer) noexcept {
    const RasterMask* mask = layer.mask();
    return mask != nullptr && mask->enabled && !mask->empty();
}

// The layer's EFFECTIVE VISIBLE BOX in document space -- the rect Arrange lines up, and the single
// place the menu's idea of "where is this layer" now comes from:
//
//   content only  -> worldTransform(layer) * contentBounds(), character for character what the
//                    Arrange menu computed before masks entered the picture. Every plain raster,
//                    shape, path and text layer takes this path and must not move by one ULP.
//   mask only     -> the mask's document coverage box. This is what rescues adjustment and filter
//                    layers: they have no content of their own, so the mask IS their position.
//   both          -> their INTERSECTION, because that is what is actually on screen. A canvas-
//                    filling generator or gradient with a small mask therefore aligns by the blob
//                    you can see rather than by the canvas it technically spans.
//   neither       -> nullopt: nothing to align, and the caller skips the layer.
//
// nullopt also for an empty content box (the `cb->empty()` skip Arrange always had, kept in
// LAYER-LOCAL space so a singular placement behaves exactly as it used to) and for an empty
// intersection -- a mask that reveals nothing OF THIS LAYER leaves nothing visible to line up, and
// moving an invisible thing is not an alignment.
[[nodiscard]] inline std::optional<common::Rect> arrangeBounds(const Layer& layer) {
    const std::optional<common::Rect> maskBox = maskDocumentBounds(layer);
    if (!maskBox && maskIsLive(layer))
        return std::nullopt; // a live sheet revealing nothing: the layer shows nothing at all
    const std::optional<common::Rect> content = layer.contentBounds();
    if (!content)
        return maskBox; // adjustment / filter: the mask is the only thing carrying a position
    if (content->empty())
        return std::nullopt; // exactly applyArrange's own `cb->empty()` skip, unchanged
    const common::Rect docContent = worldTransform(layer).mapBounds(*content);
    if (!maskBox)
        return docContent; // no live mask: byte-identical to the pre-mask Arrange
    const common::Rect visible = docContent.intersected(*maskBox);
    if (visible.empty())
        return std::nullopt;
    return visible;
}

// The RasterMask::toLocal that slides an UNLINKED mask sheet by `docDelta` (document space), or
// nullopt when there is nothing of the sort to slide.
//
// WHY it is needed at all: aligning a layer writes its TRANSFORM, and a LINKED mask rides that for
// free (maskToDocument composes the layer's world transform for it). An unlinked one deliberately
// does not -- it stays pinned in the layer's PARENT space while the pixels move under it -- so
// aligning a masked generator/filter without this slides the content out from under a sheet that
// never moved, and the visible blob does not budge. That is the whole bug for those kinds.
//
// WHY the delta converts as a VECTOR: the sheet lives in parent space, so the placement we want is
//     toLocal' = P^-1 . T(docDelta) . P . toLocal      with P = parentWorldTransform(layer)
// and for a pure translation that conjugation collapses to a translation by P's LINEAR part applied
// to the delta -- P's own translation cancels itself. Mapping the delta as a POINT instead leaks the
// parent's offset into the result, which is invisible under an identity parent and wrong under every
// other one (hence the test that puts the layer inside a translated, scaled group).
[[nodiscard]] inline std::optional<common::Affine2D>
translatedMaskPlacement(const Layer& layer, common::Vec2 docDelta) {
    const RasterMask* mask = layer.mask();
    if (mask == nullptr || !mask->enabled || mask->linked || mask->empty())
        return std::nullopt;
    const std::optional<common::Affine2D> invParent = parentWorldTransform(layer).inverse();
    if (!invParent)
        return std::nullopt; // a singular ancestor: nothing sane to edit through it
    const common::Vec2 v = invParent->applyVector(docDelta);
    return common::Affine2D::translation(v.x, v.y) * mask->toLocal;
}

// May Arrange move this layer? A HIDDEN layer has no visible box to line up, and a LOCKED one
// refuses transform edits everywhere else in the app -- the Move tool vetoes an entire gesture over
// one (VulkanCanvas::beginMoveGesture), Merge Down and the paint guard refuse it, and SetLockedCommand
// spells the rule out: a lock refuses anything that changes a layer's pixels, its TRANSFORM or its
// place in the stack. Arrange moved both regardless, which was an oversight rather than a policy.
[[nodiscard]] inline bool arrangeMovable(const Layer& layer) noexcept {
    return layer.visible() && !layer.locked();
}

// Resolve a candidate id list into the set Arrange will actually act on: drop the invalid, the
// vanished and the unmovable, de-duplicate, and keep the order the candidates were gathered in
// (first occurrence wins -- the marquee's bottom-to-top sweep and the panel's row order both carry
// meaning, and re-sorting here would silently re-pick which box "stays put" in an align-to-union).
// `resolve` is the caller's document lookup, so this stays a leaf header the tests can drive with a
// lambda over a handful of layers.
[[nodiscard]] inline std::vector<LayerId>
arrangeTargets(const std::vector<LayerId>& candidates,
               const std::function<const Layer*(LayerId)>& resolve) {
    std::vector<LayerId> out;
    out.reserve(candidates.size());
    for (const LayerId id : candidates) {
        if (id == kInvalidLayerId || !resolve)
            continue;
        const Layer* layer = resolve(id);
        if (layer == nullptr || !arrangeMovable(*layer))
            continue;
        if (std::find(out.begin(), out.end(), id) != out.end())
            continue;
        out.push_back(id);
    }
    return out;
}

} // namespace mosaic::core
