#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/object.hpp"

// CPU fill rasterizer (S25). The "floor" renderer from docs/vector-model.md §5.3: flatten ->
// classic scanline coverage fill (Wylie/Romney/Evans 1967) with analytic horizontal coverage and
// sub-scanline vertical AA. INVARIANT: this rasterizer stays scanline/coverage-based and is
// deliberately NOT Loop-Blinn and NOT stencil-then-cover -- a hard constraint on this file, not an
// oversight. The GPU-resident path is a later, separate build. This is what makes vector layers
// show pixels, and the coverage buffer is exactly what vector masks (S31) will reuse.
namespace mosaic::core::vec {

// A half-open integer pixel rectangle [x0,x1) x [y0,y1) in TARGET pixel space -- the clamped
// bounding box a coverage pass is confined to, so a small shape on a big canvas costs O(bbox),
// not O(canvas). (The full-canvas result ImageF stays full-window: that allocation is shared by
// every layer and is S60-c tiling territory, not this.)
struct PixelBounds {
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    [[nodiscard]] bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
    [[nodiscard]] std::uint32_t width() const noexcept { return x1 - x0; }
    [[nodiscard]] std::uint32_t height() const noexcept { return y1 - y0; }
};

// Clamped integer pixel bounds of PIXEL-SPACE contours, padded by `pad` px for AA, intersected
// with [0,W) x [0,H). nullopt when nothing lands on the canvas (caller skips the pass entirely).
[[nodiscard]] std::optional<PixelBounds> pixelBoundsOf(const Contours& pixelContours,
                                                       std::uint32_t width, std::uint32_t height,
                                                       std::uint32_t pad = 1);

// A single-channel anti-aliased fill mask, coverage in [0,1], row-major over a SUB-RECT of the
// target: storage is `width*height` covering [ox,ox+width) x [oy,oy+height). `at()` takes ABSOLUTE
// target pixel coords (caller must stay inside the sub-rect). A full-window buffer is just ox==oy==0
// with width/height == the canvas, so existing full-canvas callers are unchanged.
struct CoverageBuffer {
    std::uint32_t ox = 0, oy = 0;  // sub-rect origin in target pixel space
    std::uint32_t width = 0;       // sub-rect extent
    std::uint32_t height = 0;
    std::vector<float> a;  // size == width * height

    [[nodiscard]] float at(std::uint32_t x, std::uint32_t y) const noexcept {
        return a[static_cast<std::size_t>(y - oy) * width + (x - ox)];
    }
};

// Scanline-rasterize PIXEL-SPACE contours into a coverage buffer under `rule`. `subsamples` (>=1)
// vertical sub-scanlines per row set the vertical AA quality; horizontal AA is analytic. `clip`
// confines the buffer/sweep to a sub-rect (default: the whole [0,W) x [0,H) canvas window).
[[nodiscard]] CoverageBuffer rasterizeCoverage(const Contours& pixelContours, std::uint32_t width,
                                               std::uint32_t height, FillRule rule,
                                               int subsamples = 4,
                                               std::optional<PixelBounds> clip = std::nullopt);

// Render an object's FILL into a fresh straight-alpha FLOAT RGBA image of (width,height), with
// `toPixel` mapping layer-local space -> pixel space. Handles solid and gradient (linear/radial/
// conic, with pad/repeat/reflect) paints; NoPaint yields a transparent image. The stroke is
// rendered by rasterizeObjectF (below); this entry point is fill-only.
//
// Float is the NATIVE output: the rasterizer already computes coverage and colour in float, and
// the compositor works in float (ImageF), so the live path stays full-precision -- no 8-bit
// quantization to band gradients, and it pre-aligns with the S43-a float-storage migration
// (PLAN §3.6). The 8-bit overload below is a thin convenience for standalone/export consumers.
[[nodiscard]] common::ImageF rasterizeFillF(const Object& obj, std::uint32_t width,
                                            std::uint32_t height, const common::Affine2D& toPixel,
                                            double tolerancePx = 0.25);

// 8-bit RGBA convenience wrapper (toImage8 of rasterizeFillF) for callers that want bytes.
[[nodiscard]] common::Image rasterizeFill(const Object& obj, std::uint32_t width,
                                          std::uint32_t height, const common::Affine2D& toPixel,
                                          double tolerancePx = 0.25);

// Render the WHOLE object -- fill then stroke (or stroke then fill, per obj.paintOrder) composited
// source-over into one float buffer. This is the compositor's entry point for a vector layer; the
// stroke outline is built by strokeOutline() and filled NonZero, painted with obj.stroke.paint.
// `antialias` false hardens coverage to 0/1 (crisp/aliased edges) -- the compositor passes false
// when the user picks the Nearest resample filter, so the AA combo governs vector edges too (S26).
[[nodiscard]] common::ImageF rasterizeObjectF(const Object& obj, std::uint32_t width,
                                              std::uint32_t height, const common::Affine2D& toPixel,
                                              double tolerancePx = 0.25, bool antialias = true);

}  // namespace mosaic::core::vec
