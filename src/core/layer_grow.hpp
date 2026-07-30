#pragma once

#include "common/geometry.hpp" // Affine2D, Rect, Vec2

#include <cstdint>

// Bounded layer growth for the painting tools -- "the layer grows as the stroke paints outside it,
// up to the canvas".
//
// A raster layer's image is its own pixel grid, placed in the document by the layer's transform, and
// a stroke that wandered off that grid used to lose its paint at the edge (BrushEngine::stamp clips
// every dab box to [0,w) x [0,h)). Growing the grid fixes that -- but growing it to fit the STROKE
// is an out-of-memory waiting for a pointer at an absurd coordinate, which is precisely the failure
// this file exists to make impossible. Every function here is written around one rule:
//
//   ⚠ THE CLAMP SITS AHEAD OF THE ARITHMETIC. A requested region is intersected with the canvas
//   ceiling BEFORE any width, height or cell count is derived from it, and a floating-point corner
//   is saturated BEFORE it is cast to an integer. A coordinate ten trillion pixels off the canvas
//   therefore costs two comparisons and nothing else: it never sizes an allocation that is later
//   trimmed, because "allocate, then trim" IS the failure being prevented.
//
// Two integer traps are load-bearing here, and both are avoided by construction rather than by
// checking afterwards:
//   * `static_cast<int>(1e13)` is UNDEFINED BEHAVIOUR, not a wrap. Every double that becomes an
//     integer in this file is saturated into range first (pixelBoxCovering, clampStrokePos).
//   * a width computed from two corners can be NEGATIVE, and a negative width handed to a
//     `std::uint32_t` image constructor wraps to ~4.3e9 -- a 16 GB allocation from a box that was
//     merely inside-out. PixelBox is signed, empty() is checked before width() is ever used, and
//     the cell count is accumulated in `long long`.
//
// FLTK- and Vulkan-free, and every function is pure -> unit-tested headlessly
// (tests/test_layer_grow.cpp).
namespace mosaic::core {

// A half-open integer box in a layer's OWN pixel grid: [x0, x1) x [y0, y1). Signed, and `long`
// rather than `std::uint32_t`, because a growth box legitimately has negative corners -- growing a
// layer leftward means the canvas begins at a negative layer-local x.
struct PixelBox {
    long x0 = 0;
    long y0 = 0;
    long x1 = 0;
    long y1 = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
    // ⚠ Only meaningful on a NON-EMPTY box: an inside-out box's width is negative, and it is exactly
    // that negative width -- made unsigned by a caller who trusted it -- that wraps into a
    // multi-gigabyte allocation. Check empty() first; brushGrowthBox() never returns an empty box
    // for a non-empty layer.
    [[nodiscard]] constexpr long width() const noexcept { return x1 - x0; }
    [[nodiscard]] constexpr long height() const noexcept { return y1 - y0; }
    // Accumulated in `long long` on purpose: two in-range `long` sides still multiply out of an
    // `int`, and the cell count is what the ceiling below is expressed in.
    [[nodiscard]] constexpr long long cells() const noexcept {
        return empty() ? 0LL : static_cast<long long>(width()) * static_cast<long long>(height());
    }

    friend constexpr bool operator==(const PixelBox&, const PixelBox&) = default;
};

// How far a layer-local coordinate may sit from the origin before it is saturated. Chosen with a
// wide margin on both sides: it is ~134 million px, so no real canvas (or layer-space image of one)
// is anywhere near it, while `int` still has 16x headroom above it for the dab-radius arithmetic
// the engine adds before it casts (`static_cast<int>(std::ceil(center.x + ax + 1.0))`).
inline constexpr long kMaxLayerCoord = 1L << 27; // 134,217,728 px

// The ceiling on a GROWN layer -- per side, and on the total cell count. A layer that is already
// bigger than this keeps every pixel it has (growth never shrinks); these bound only what growth may
// ADD. 2^28 cells is 268 million px, i.e. 1 GiB of 8-bit RGBA: past that, refusing to grow is the
// kinder answer than trying.
inline constexpr long kMaxLayerSide = 1L << 20;        // 1,048,576 px
inline constexpr long long kMaxLayerCells = 1LL << 28; // 268,435,456 px

// A layer's current image, as a box.
[[nodiscard]] constexpr PixelBox layerPixelBox(std::uint32_t w, std::uint32_t h) noexcept {
    return {0, 0, static_cast<long>(w), static_cast<long>(h)};
}

// The smallest integer box covering `r` (floor the min corner, ceil the max), with every corner
// SATURATED into +-kMaxLayerCoord before it is made an integer. A non-finite rect -- which a
// near-singular transform produces readily -- yields an empty box rather than a cast whose result
// the standard does not define.
[[nodiscard]] PixelBox pixelBoxCovering(const common::Rect& r) noexcept;

// The document (canvas) rectangle expressed in a layer's own pixel grid: the axis-aligned box of the
// canvas's four corners mapped through `layerToDoc`^-1. Empty when the document is degenerate or the
// placement is singular (there is then no layer-space image of the canvas, and the layer keeps the
// size it has).
[[nodiscard]] PixelBox canvasBoxInLayer(const common::Affine2D& layerToDoc, std::uint32_t docW,
                                        std::uint32_t docH);

// The box a paint stroke may grow a `layerW` x `layerH` layer to.
//
//   `requested`  the layer-local box the stroke would like to reach. UNTRUSTED: it may be empty,
//                inside-out, or ten trillion pixels wide.
//   `canvasBox`  the canvas in this layer's pixel grid (canvasBoxInLayer).
//
// The result ALWAYS contains the layer's own box (growth never shrinks a layer, and never returns
// an empty box for a non-empty one) and NEVER reaches past `layerBox` united with `canvasBox`.
// `requested` can only ever subtract from that ceiling, never add to it -- so the pointer decides
// how much of the permitted growth is taken and nothing else. A result whose sides or cell count
// exceed the ceilings above is refused outright (the layer's own box comes back, and the stroke
// clips at the layer edge exactly as it did before auto-grow existed).
[[nodiscard]] PixelBox brushGrowthBox(std::uint32_t layerW, std::uint32_t layerH,
                                      const PixelBox& requested, const PixelBox& canvasBox);

// One stroke sample's layer-local position, made safe for the engine's integer arithmetic. The
// engine turns a dab centre into an integer box with `static_cast<int>(std::floor(...))`, and a
// double past INT_MAX makes that cast undefined -- so the coordinate is saturated to +-kMaxLayerCoord
// (and a NaN/inf to 0) before it is ever handed over. Anything remotely on-canvas passes through
// BIT-IDENTICAL, because the saturation only bites 134 million px out; no real stroke's geometry
// moves, and that is a test.
[[nodiscard]] common::Vec2 clampStrokePos(common::Vec2 p) noexcept;

} // namespace mosaic::core
