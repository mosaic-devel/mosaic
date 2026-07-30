#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "core/command.hpp"
#include "core/document.hpp"
#include "render/render.hpp"      // ResampleFilter
#include "render/compositor.hpp"  // CropFill (and buildDocumentRemapCommand, the shared engine)

// The Image menu's whole-document operations (S53-a): Canvas Size, Image Size, the four
// orientation ops, an arbitrary-angle document rotation, and Trim to Content.
//
// Every one of them is ONE undo step and is built on the same engine the Crop tool uses --
// render::buildDocumentRemapCommand -- rather than a private copy of it: the canvas resizes and
// every layer transform, layer mask, guide and the selection are rebased by one affine. That is
// why a crop and an Image Size share the group push-down, the masked-group rule, the canvas-locked
// texture rule and the delete-mode bake instead of drifting apart. See docs/image-operations.md.
//
// None of these narrows Command::dirtyRegion(): a whole-document remap invalidates the whole
// canvas, and nullopt (the default) is the correct answer for all of them.
namespace mosaic::render {

// The nine-point anchor grid of the Canvas Size dialog, row-major from the top-left (so
// `int(a) % 3` is the column and `int(a) / 3` the row).
enum class CanvasAnchor : int {
    TopLeft = 0, Top, TopRight,
    Left,        Center, Right,
    BottomLeft,  Bottom, BottomRight,
};

// The four lossless orientation changes of Image -> Rotation. Rotate90 swaps the canvas's width
// and height; the flips keep it.
enum class DocOrient : int { Rotate90CW = 0, Rotate90CCW, Rotate180, FlipHorizontal, FlipVertical };

// An integer document-pixel rectangle. `x`/`y` may be negative (see canvasRectFor).
struct CanvasRect { long x; long y; std::uint32_t w; std::uint32_t h; };

// Where the OLD canvas lands inside the NEW one for the given anchor: `{x, y}` is the old canvas's
// top-left expressed in NEW-canvas coordinates and `{w, h}` is `{oldW, oldH}`. Growing therefore
// gives a non-negative offset (the old content sits inside the new frame) and shrinking a negative
// one (the old content starts off the top/left edge and is cropped). The equivalent crop window in
// OLD coordinates is `(-x, -y, newW, newH)`, and the document remap is `translation(x, y)`.
//
// ROUNDING: the centred axes divide `newW - oldW` (an int) by 2 with C++'s truncation TOWARD ZERO,
// so an odd difference always puts the extra pixel on the RIGHT / BOTTOM -- growing a 100px canvas
// to 101 adds the column on the right, shrinking 101 to 100 removes it from the right. Truncation
// (rather than floor) is what makes those two agree; the rule is deterministic in both directions
// and pinned by a test.
//
// Pure: no document, no allocation. The Canvas Size dialog's preview frames the same rectangle.
[[nodiscard]] CanvasRect canvasRectFor(std::uint32_t oldW, std::uint32_t oldH, std::uint32_t newW,
                                       std::uint32_t newH, CanvasAnchor anchor);

// Image -> Canvas Size: re-frame the canvas to `newW` x `newH` without touching the picture's
// scale, placing the old content by `anchor`. Non-destructive: nothing is baked and no layer is
// clipped, so shrinking and growing back is lossless. `fill` colours the area the old canvas did
// not cover, on the same terms as the Crop tool's expansion (the standard Background is extended
// in place with a byte-exact blit; any other stack shape gains a bottom fill layer); no `fill`
// leaves it transparent. nullptr when the size is unchanged or degenerate.
[[nodiscard]] std::unique_ptr<core::Command> buildCanvasResizeCommand(
    core::Document& doc, std::uint32_t newW, std::uint32_t newH, CanvasAnchor anchor,
    const std::optional<CropFill>& fill);

// Image -> Image Size: scale the WHOLE document to `newW` x `newH` about the canvas origin.
//
// What resamples and what does not (docs/image-operations.md §5): plain unmasked raster layers
// under an all-identity ancestor chain are PHYSICALLY resampled with `filter` and land at the
// identity transform -- that is what makes the dialog's quality dropdown mean something, and it
// stops every later composite from re-resampling them. Everything else -- masked rasters, magic
// layers (whose whole point is keeping the full-resolution source), vector/text/texture layers,
// and anything under a transformed ancestor -- takes the scale into its transform and stays
// resolution-independent. The consequence to know: a physically resampled raster is CLIPPED to the
// new canvas, exactly as the Crop tool's "Delete Cropped Pixels" clips, and the scale is lossy in
// the usual raster sense (undo restores it; a scale-down then scale-up does not).
//
// The selection scales with the document (Selection::scaled); guides scale with it too. nullptr
// when the size is unchanged or degenerate.
[[nodiscard]] std::unique_ptr<core::Command> buildImageResizeCommand(core::Document& doc,
                                                                     std::uint32_t newW,
                                                                     std::uint32_t newH,
                                                                     ResampleFilter filter);

// Image -> Rotation: a quarter-turn or a mirror, BYTE-LOSSLESS by construction. The remap is a
// signed axis permutation with an integer translation, so render::isLosslessGrid holds and
// chooseAutoFilter resolves the placement to Nearest -- every pixel lands on a pixel and nothing is
// interpolated, however many times the op is applied. That is also why this takes NO ResampleFilter:
// the UI's current resample quality must never reach an orientation, or a 90-degree turn would
// silently blur. The selection follows as an exact index permutation and the guides swap axes with
// the canvas; no fill is ever needed (the turned canvas covers itself exactly).
[[nodiscard]] std::unique_ptr<core::Command> buildOrientCommand(core::Document& doc, DocOrient op);

// Image -> Rotate Arbitrary: turn the document by `angleRad` (positive = the +x axis toward +y)
// about the canvas centre, growing the canvas to the rotated bounding box so nothing is cut off.
// An arbitrary angle genuinely resamples, so this bakes eligible raster layers through `filter`
// (the same split buildImageResizeCommand documents) and `fill` paints the newly exposed corner
// wedges. The selection cannot survive an arbitrary rotation and is cleared; guides are dropped for
// the same reason (a rotated guide is not a horizontal or vertical line any more).
// nullptr for an angle that is a whole number of turns (a no-op).
[[nodiscard]] std::unique_ptr<core::Command> buildRotateDocumentCommand(
    core::Document& doc, double angleRad, ResampleFilter filter,
    const std::optional<CropFill>& fill);

// Image -> Trim to Content: shrink the canvas to the tight document-space box of every VISIBLE
// layer's content (alpha>0 for the pixel kinds, the flattened geometry for vectors -- whatever
// Layer::contentBounds() reports, mapped through the layer's ancestor chain). Trim only ever
// shrinks: the box is clamped to the current canvas, so content spilling past an edge is not a
// reason to grow. nullptr when nothing is visible or the box already IS the canvas.
[[nodiscard]] std::unique_ptr<core::Command> buildTrimToContentCommand(core::Document& doc);

}  // namespace mosaic::render
