#pragma once

#include "common/image.hpp"

#include <cstdint>
#include <vector>

// Bucket-fill flood engine (S21, docs/bucket-fill.md). Pure + CPU, FLTK-free, unit-testable: a
// single click seeds a colour-tolerance flood and the engine returns a per-pixel FILL coverage
// mask. It shares the S17 magic-wand colour metric (core::wandColorDistance) but produces a *fill*
// mask, not a selection: the flooded interior is solid (255) and only the region's outer boundary
// earns a soft ramp, so the paint lands opaque inside and feathers only at the true edge. (A
// selection instead caps its interior at the marching-ants threshold -- a different need, so
// magicWandSelection is deliberately not reused here.) The caller intersects the mask with any
// active selection and hands the covered region to render::computeFill / computeFillPaint (the
// shared S39 fill core).
namespace mosaic::core {

// The bucket's knobs. `tolerance` runs on the same normalised [0,1] colour-distance metric the wand
// uses (the tool bar's 0-255 slider maps onto it); `contiguous` picks a 4-connected flood from the
// seed vs. every matching pixel in the image; `antialias` softens the region's outer boundary with
// a 1px ramp; `sampleAlpha` folds alpha into the distance so a click on a transparent area floods
// its own region.
struct FillParams {
    double tolerance = 0.15;
    bool contiguous = true;
    bool antialias = true;
    bool sampleAlpha = true;
};

// Compute the per-pixel fill coverage (0..255, row-major, size src.width*src.height) a bucket-fill
// click at pixel (seedX, seedY) produces over `src` (the layer's own pixels, in its pixel space). A
// seed outside `src` or an empty `src` yields an EMPTY vector ("nothing to fill"); a result
// covering nothing likewise returns empty, so the caller can no-op cleanly.
[[nodiscard]] std::vector<std::uint8_t> bucketFillCoverage(const common::Image& src, int seedX,
                                                           int seedY, const FillParams& params);

} // namespace mosaic::core
