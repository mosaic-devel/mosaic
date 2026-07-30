#pragma once

#include "common/geometry.hpp" // Rect
#include "common/image.hpp"    // Color8 (the wand metric), Image (the wand's flood source)

#include <cstdint>
#include <optional>
#include <vector>

// The document-level selection (PLAN S13): an 8-bit coverage mask the size of the document.
// 0 = unselected, 255 = fully selected; intermediate values are anti-aliased / feathered coverage
// (marquee edges arrive with S14, feather with S18). **"No selection" is a default-constructed,
// empty Selection** (isEmpty()) and means "everything is editable"; an all-zero mask is NOT the
// same thing — it is an active selection of nothing. Boolean ops saturate per pixel in coverage
// semantics: Add = max, Subtract = a·(255−b)/255, Intersect = min.
//
// Consumers lined up behind this type: the marching-ants present pass (S13 part 2), Shift-click-
// thumbnail → select layer pixels (S10-c stub, wired in S13 part 2), clipboard copy (S14-b), the
// Select menu's grow/shrink/feather (S18), and Mask-from-selection (S31). Storage is a flat
// document-sized buffer for now — tiling arrives with the S60-c storage work.
namespace mosaic::core {

class Layer;
struct RasterMask;  // core/layer.hpp (the S31 helpers below build/read layer masks)

enum class SelectOp { Replace, Add, Subtract, Intersect };

// The coverage at which a pixel counts as "inside the selection" for anything that needs a hard
// yes/no over a soft mask. The marching-ants present pass draws the boundary of exactly this set
// (`canvas_present.comp`: `texture(uMask, ...).r >= 0.5`, i.e. 128/255), so a hit test that shares
// the threshold agrees with what the user sees enclosed -- which is what the S16-i grab-inside-the-
// ants gesture needs. Do not change one without the other.
inline constexpr std::uint8_t kAntsCoverageThreshold = 128;

class Selection {
public:
    Selection() = default; // empty: no active selection
    Selection(std::uint32_t width, std::uint32_t height); // all-zero mask (selects nothing)

    // A hard-edged filled rectangle, clamped to the document; the S14 marquee's building block.
    [[nodiscard]] static Selection rectangle(std::uint32_t docW, std::uint32_t docH,
                                             common::Rect r);

    // An anti-aliased filled ellipse inscribed in `r`, clamped to the document (the S14
    // elliptical marquee). Edge pixels carry fractional coverage (the AA semantics above).
    [[nodiscard]] static Selection ellipse(std::uint32_t docW, std::uint32_t docH, common::Rect r);

    // An anti-aliased filled polygon, clamped to the document (the S14 lasso tools' rasteriser;
    // the path is closed implicitly). Even-odd rule, so a self-crossing lasso makes holes —
    // Photoshop semantics. Fewer than 3 points select nothing.
    [[nodiscard]] static Selection polygon(std::uint32_t docW, std::uint32_t docH,
                                           const std::vector<common::Vec2>& points);

    // Combine in coverage semantics (see header comment). Replace simply returns `b`. Both
    // non-empty inputs must share dimensions; an empty `a` acts as a zero mask of b's size.
    [[nodiscard]] static Selection combine(const Selection& a, const Selection& b, SelectOp op);

    [[nodiscard]] bool isEmpty() const noexcept { return m_data.empty(); }
    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }

    [[nodiscard]] std::uint8_t at(std::uint32_t x, std::uint32_t y) const noexcept; // 0 outside
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return m_data; }
    [[nodiscard]] std::vector<std::uint8_t>& data() noexcept { return m_data; }

    // True if any pixel has coverage > 0 (an empty Selection has none).
    [[nodiscard]] bool anySelected() const noexcept;

    // Coverage complement (255 − v per pixel). Inverting an *empty* Selection returns empty —
    // "no selection" has no complement; the UI gates the gesture (Select→Inverse is a no-op /
    // disabled without an active selection, as in Photoshop).
    [[nodiscard]] Selection inverted() const;

    // --- The Select-menu morphology ops (S18, docs/research-select-brush.md §4) ------------------
    // All four operate on the existing coverage mask and PRESERVE its anti-aliased semantics -- the
    // ants ride the 0.5 iso-contour but fills read the fractional ramp, so an op that hard-thresholds
    // to binary would visibly de-AA every fill. They are pure `core` (a small separable Gaussian +
    // an exact Euclidean distance transform, no `render` dependency), so the Select-menu command
    // stays self-contained and headless-testable. An empty (or coverage-free) input returns empty;
    // a zero radius is the identity.
    //
    // Grow / Shrink expand or contract the selection by `px` document pixels via a signed-distance
    // threshold off the 0.5 iso-contour, re-derived as a 1-px coverage ramp at the shifted level
    // (§4.2) -- an EXACT N-px offset that keeps a clean AA edge, not an octagonal iterative dilate.
    // Shrink is Grow with a negated offset, so `grown(N).shrunk(N)` recovers the original within AA.
    [[nodiscard]] Selection grown(int px) const;
    [[nodiscard]] Selection shrunk(int px) const;
    // Feather blurs the coverage by `radius` px (a separable Gaussian, sigma = radius), leaving the
    // result in coverage semantics directly -- the mask is already fractional (§4.3).
    [[nodiscard]] Selection feathered(double radius) const;
    // Smooth rounds jagged edges and removes speckle smaller than `radius`: blur by radius, threshold
    // at 0.5, then re-derive the clean 1-px AA ramp (reusing Grow/Shrink's offset path, §4.4).
    [[nodiscard]] Selection smoothed(double radius) const;

    // The selection carried into a cropped document (S16): the `newW`x`newH` window of this
    // mask at offset (`offX`,`offY`). Cropping an empty Selection — or cropping ALL coverage
    // away — returns empty ("no selection"), never an active selection of nothing.
    [[nodiscard]] Selection cropped(long offX, long offY, std::uint32_t newW,
                                    std::uint32_t newH) const;

    // The selection carried through a LOSSLESS GRID remap of the document plane (S53-a: Image ->
    // Rotate 90 / 180, Flip Horizontal / Vertical). `docToNew` must carry pixel centres onto pixel
    // centres -- a signed axis permutation with an integer translation -- which makes this an exact
    // index permutation of the coverage bytes, not a resample: no value is ever interpolated, so a
    // flip-and-flip-back is byte-identical. Sampling is nearest through the inverse (the
    // compositor's own leaf semantics), so any pixel of the new canvas the remap does not cover
    // reads 0. Remapping an empty Selection -- or one whose coverage all falls outside -- returns
    // empty ("no selection"), exactly as cropped() does.
    [[nodiscard]] Selection remapped(const common::Affine2D& docToNew, std::uint32_t newW,
                                     std::uint32_t newH) const;

    // The selection carried into a RESIZED document (S53-a Image Size): the coverage mask scaled to
    // `newW` x `newH`. The footprint is one destination pixel wide in source units, so growing
    // interpolates linearly (an anti-aliased edge stays smooth instead of turning blocky) and
    // shrinking box-averages (a thin selected sliver keeps proportional coverage instead of being
    // point-sampled away); edge taps clamp, so a selection touching the canvas border does not gain
    // a soft fringe from outside it. There is deliberately no ResampleFilter parameter: that enum
    // lives in `render`, which depends on `core` and not the other way round, and a coverage mask
    // does not want a ringing kernel anyway. An exact-size request returns *this unchanged; an
    // empty (or fully-erased) result is empty ("no selection").
    [[nodiscard]] Selection scaled(std::uint32_t newW, std::uint32_t newH) const;

    // The mask shifted by (dx,dy) document pixels — the S16-i selection move / arrow-key nudge.
    // The mask is document-sized, so coverage pushed past an edge is CLIPPED (there is nowhere to
    // keep it) and the vacated band arrives as 0; pushing all of it off returns empty ("no
    // selection"), exactly as cropped() does when the window misses every covered pixel. A gesture
    // therefore translates its PRESS-TIME base by the accumulated offset on every update, never
    // the previous result — so dragging out past an edge and back within one gesture is lossless,
    // while the committed mask honestly stops at the canvas. Translating an empty Selection, or by
    // more than the document's extent, returns empty.
    [[nodiscard]] Selection translated(long dx, long dy) const;

    // Tight integer bounding box (document pixels) of coverage > 0; nullopt when none.
    [[nodiscard]] std::optional<common::Rect> bounds() const;

    friend bool operator==(const Selection&, const Selection&) = default;

private:
    // Shared engine behind grown()/shrunk() (and smoothed()'s final re-AA): shift the 0.5
    // iso-contour by `delta` document pixels (positive grows, negative shrinks) and re-derive
    // coverage as a 1-px linear ramp across the shifted level. Pure signed EDT, `render`-free.
    [[nodiscard]] Selection offsetBy(double delta) const;

    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::vector<std::uint8_t> m_data;
};

// "Select the layer's pixels" (the S10-c Shift-click-thumbnail gesture): a document-sized
// Selection whose coverage is the layer's alpha, placed through the layer's transform exactly
// like the compositor's leaf walk (inverse-sample per document pixel, nearest). Raster layers
// read their image, Magic layers their source; other kinds have no pixels of their own ->
// nullopt. The layer's raster *mask* is deliberately not folded in (clicking the mask thumbnail
// is the S31 gesture); visibility/opacity don't matter — this samples content, not appearance.
[[nodiscard]] std::optional<Selection> selectionFromLayerPixels(const Layer& layer,
                                                                std::uint32_t docW,
                                                                std::uint32_t docH);

// ---- Layer masks (S31) --------------------------------------------------------------------------
// The mask grid a layer carries is stated once, at RasterMask in core/layer.hpp: a sheet on the
// layer's own sampling grid for the kinds that have one (raster/magic mask their source image
// 1 px per image px; text/texture their renderer's cache), and a DOCUMENT-WINDOW sheet, placed by
// RasterMask::toLocal at build time, for the kinds that do not (group/vector/adjustment). These
// helpers are the only supported way to build one -- they size the sheet AND place it, and a mask
// built any other way will disagree with the compositor's fold.

// A freshly added, all-revealing mask for `layer` ("Add Mask" with no selection): full coverage on
// the kind's grid, placed on the layer, enabled. Keeps the linkage of the mask already there (a
// fresh one is linked). Adding this mask must be a visual no-op on every kind -- it reveals
// everything -- which is exactly what the placement buys.
[[nodiscard]] RasterMask revealAllMask(const Layer& layer, std::uint32_t docW, std::uint32_t docH);

// "Mask from selection" (the S31 Select-menu entry): the document-space selection coverage on
// `layer`'s mask sheet, so the mask reveals exactly the document pixels the selection covered --
// whatever the layer's transform, and still exactly those pixels after the layer is moved, scaled
// or rotated (linked). A PLACED document-window sheet takes the coverage verbatim, at document
// resolution, because its captured placement carries the transform instead; a source-grid sheet is
// resampled onto that grid through maskToDocument (nearest, like the compositor's leaf walk). An
// empty selection yields the all-revealing mask.
[[nodiscard]] RasterMask maskFromSelection(const Layer& layer, const Selection& sel,
                                           std::uint32_t docW, std::uint32_t docH);

// "Select the mask's coverage" (Shift-click the mask thumbnail -- the gesture
// selectionFromLayerPixels deliberately left to S31): a document-sized Selection sampling the mask
// through core::maskToDocument, i.e. exactly where the compositor folds it. nullopt when the layer
// has no mask; a coverage-free result collapses to empty ("no selection").
[[nodiscard]] std::optional<Selection> selectionFromLayerMask(const Layer& layer,
                                                              std::uint32_t docW,
                                                              std::uint32_t docH);

// The magic wand's knobs (S17, docs/research-selection.md §3–§5). The wand is a stateless per-click
// colour-tolerance flood -- no learned region, no matting -- so this is the whole of its behaviour.
struct WandParams {
    // Threshold on the normalised colour-distance metric (0 = only the seed's exact colour, 1 = the
    // whole image / connected region). The options bar's 0-100 slider maps straight onto [0,1]; the
    // metric's own units are deliberately hidden from the user (research §3).
    double tolerance = 0.15;
    // Flood the 4-connected region reachable from the seed (Photoshop's default) vs. match EVERY
    // within-tolerance pixel regardless of connectivity ("Contiguous" off -> global "select by colour").
    bool contiguous = true;
    // Soften the boundary with a distance-to-tolerance coverage ramp (research §5). The connectivity
    // still gates on the HARD predicate, so the soft band never bridges neighbouring regions.
    bool antialias = true;
    // Include the alpha channel in the distance so a click on a transparent area forms its own region
    // (research OQ-4). false = compare RGB only.
    bool sampleAlpha = true;
};

// Normalised weighted-Euclidean colour distance in the image's ENCODED (gamma) space, in [0,1]
// (research §2.3 D2: the S43-b managed-CIELAB ΔE swap replaces THIS function alone). Luma-weighted
// RGB, with a lighter alpha term when `useAlpha`. The wand's swappable metric seam.
[[nodiscard]] double wandColorDistance(common::Color8 a, common::Color8 b, bool useAlpha) noexcept;

// Select by colour from `src` (a document-space RGBA image: the merged composite, or the active
// raster resampled into document space -- see docs/research-selection.md §8.1), seeded at pixel
// (seedX, seedY). 4-connected scanline flood when contiguous, a single linear scan when global; the
// AA edge is a hard flood + a one-pixel boundary ramp (research §5). The result is a `src`-sized
// Selection. A seed outside `src`, an empty `src`, or a result covering nothing all yield an empty
// ("no selection") Selection -- never an active selection of nothing (research §1).
[[nodiscard]] Selection magicWandSelection(const common::Image& src, int seedX, int seedY,
                                           const WandParams& params);

} // namespace mosaic::core
