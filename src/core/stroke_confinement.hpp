#pragma once

#include "common/geometry.hpp" // Affine2D

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// SELECTION CONFINEMENT for the stamping-stroke engine: "a stroke may not deposit outside the active
// selection", the standard behaviour of every raster editor.
//
// It is a COVERAGE MULTIPLY, not a clip. The selection is an 8-bit coverage mask (core/selection.hpp)
// whose edges are anti-aliased and whose feather is genuinely fractional, so a half-selected pixel
// must take half the paint. Thresholding it (kAntsCoverageThreshold, which exists for the hit tests
// and the marching ants) would de-feather every selection the moment you painted into it.
//
// ⚠ THIS IS NOT THE MASKING BRUSH. The engine's `MaskingParams`/`m_mask` walk is a different
// feature -- a SECOND dab walk with its own tip and cadence, ACCUMULATING a stroke-scoped grayscale
// value that a preset-chosen `MaskingOp` folds into the stroke's alpha. Confinement is a STATIC
// field that belongs to the document, not to the brush; riding the masking buffer would (a) make a
// preset's `Subtract`/`LinearDodge` op silently reinterpret the selection, (b) evict the preset's
// own masking brush whenever a selection existed, and (c) cost a per-dab deposit for a value that
// never changes. So it rides BESIDE the masking brush, and the two compose.
//
// The whole design turns on one property: **no selection means no confinement object at all**
// (makeStrokeConfinement returns null for an empty Selection), so an unconfined stroke never
// executes a single instruction of this and is byte-for-byte the stroke the engine laid before
// confinement existed. Every brush golden in the suite depends on that, and it is a test
// (tests/test_brush_clip.cpp).
namespace mosaic::core {

class Selection;

// The active selection resampled onto ONE stroke's target pixel grid (the layer's image for a paint
// stroke, the mask grid for the S31 mask lane), stored as a WINDOW rather than a full-grid buffer:
// outside the window the coverage is 0, because outside the selection's own bounds it is 0 anyway.
// A selection that covers a corner of a 5k x 8k layer therefore costs its own bounding box, not
// 40 MB.
//
// Immutable for the stroke's lifetime and shared by `shared_ptr`, so the mark stays a pure function
// of the stroke's inputs: undo/redo replay and the editor's preview see exactly the field the live
// stroke saw, even if the document's selection changes underneath.
struct StrokeConfinement {
    std::int32_t x = 0; // target-local origin of the window
    std::int32_t y = 0;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::vector<std::uint8_t> v; // w*h coverage bytes; 0 = unselected, 255 = fully selected

    [[nodiscard]] bool empty() const noexcept { return w == 0 || h == 0; }

    // This pixel's share of the paint, in [0,1]. Outside the window: 0.
    //
    // ⚠ DIVISION, never multiplication by a reciprocal -- the same rule the engine's Colored
    // normalization is pinned to. IEEE guarantees `255.0/255.0 == 1.0` exactly, while
    // `255 * (1.0/255.0)` can land one ulp under it; and "one ulp under 1.0" is the difference
    // between a fully-selected pixel being byte-identical to an unconfined one and it flickering a
    // level wherever the rounding falls.
    [[nodiscard]] double at(int px, int py) const noexcept {
        const long lx = static_cast<long>(px) - static_cast<long>(x);
        const long ly = static_cast<long>(py) - static_cast<long>(y);
        if (lx < 0 || ly < 0 || lx >= static_cast<long>(w) || ly >= static_cast<long>(h))
            return 0.0;
        return v[static_cast<std::size_t>(ly) * w + static_cast<std::size_t>(lx)] / 255.0;
    }
};

// Build the confinement field for a stroke whose target grid maps to the document through
// `targetToDoc` (worldTransform(layer) for a paint stroke; the mask->doc map for the S31 mask lane).
// Sampled NEAREST at each target pixel's centre, exactly like the compositor's leaf walk and
// maskFromSelection -- the selection is a document-space field and the target grid may be rotated,
// scaled or offset under it.
//
// Returns NULL when `sel` is empty ("no selection" = everything editable): the caller then hands the
// engine nothing and the stroke is bit-identical to one laid before this existed. An ACTIVE
// selection that happens to cover nothing is a different thing entirely and returns a non-null,
// all-zero field -- a stroke inside it paints nothing, which is correct.
[[nodiscard]] std::shared_ptr<const StrokeConfinement>
makeStrokeConfinement(const Selection& sel, const common::Affine2D& targetToDoc,
                      std::uint32_t targetW, std::uint32_t targetH);

} // namespace mosaic::core
