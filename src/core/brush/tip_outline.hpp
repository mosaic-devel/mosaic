#pragma once

#include "core/brush/brush_tip.hpp"

#include <cstddef>
#include <vector>

// The tip's OUTLINE, as a signed distance field (docs/brushes.md §6.3).
//
// The reticle traces the tip. An ellipse is the truth for a round or oval nib and a LIE for every
// other one -- a bristle, a spatter, a ring, a spiked star -- and a user who picks
// `i)_Wet_Bristles_Rough` and gets a perfect circle over it has been told the wrong thing about where
// paint will land, which is the reticle's one job.
//
// So the shader gets a second way to answer the only question it ever asks: `tipDist(d)` -- the SIGNED
// DISTANCE IN SCREEN PX to the tip's outline, zero on it. That contract is what the antialiasing, the
// 3x3 supersampling, the three Settings->Appearance line styles and the lock punch-out are all built
// on, so keeping it means none of them has to know a tip has a shape. For the tips the analytic
// ellipse already describes exactly, the shader keeps evaluating it (`tipNeedsSdf` is false, and a
// NULL tip's ring stays bit-for-bit what it was). For every other tip, the shader samples the field
// this file builds.
//
// Three properties the design leans on:
//
//   * **The field lives in the TIP'S OWN frame**, built at angle 0 and at a fixed build resolution.
//     The dab's diameter, the view's zoom, the tip's angle and the cursor's position are all applied
//     when the field is SAMPLED -- so none of them rebuilds it. It is rebuilt only when the tip's
//     raster (`BrushTip::id`) or its frame changes, which is once per preset pick, not once per mouse
//     move.
//
//   * **The silhouette is `coverage != 0`, not `coverage >= 0.5`.** ANY non-zero coverage is inside.
//     A soft tip fades to nothing well before its rim; trace it at half coverage and the ring lands
//     deep inside the tip's real extent, understating the brush's size -- which destroys the one job
//     the reticle has. (It is also what the reference does.)
//
//   * **The distance transform is EXACT** (Felzenszwalb & Huttenlocher's two-pass squared EDT, an
//     O(n) exact Euclidean transform), not an approximate chamfer. A chamfer's error is anisotropic:
//     it would make a ring visibly tighter on the diagonals than on the axes, and that is precisely
//     the kind of quiet, plausible wrongness this file exists to avoid.
//
// FLTK-, Vulkan- and platform-free.
namespace mosaic::core::brush {

// The build resolution: the tip's LONG axis, in build px. The field is bilinearly interpolated, so
// this is not the ring's resolution -- it is the resolution at which the tip's SILHOUETTE was read.
// 128 keeps a bristle tip's gaps legible without making the grid (and its per-frame upload) large.
inline constexpr int kTipSdfRes = 128;

// Cells of background around the tip's box. TWO reasons, and the second is why it is not 1:
//
//   * a `rect` generator fills its box edge to edge, and with no background around it the transform
//     would find no outside at all -- the box's own border, which IS the outline there, would go
//     untraced;
//   * the ring is drawn over a BAND either side of the outline (a few screen px), and the outline
//     touches the box wherever the tip is widest. The band therefore reaches OUTSIDE the box, and the
//     field has to still be exact there. Beyond the grid it can only be extrapolated, and an
//     extrapolation's gradient points the wrong way (`sdfSample` walks straight out of the nearest
//     edge, whatever direction the real outline lies in) -- which is a visibly uneven ring, not a
//     wrong number in a corner.
inline constexpr int kTipSdfPad = 6;

// The padded grid never exceeds (res + 2*pad) on either axis (`buildTipSdf` fits the tip's LONG axis
// to `res`, whichever axis that is). The renderer sizes its storage buffer from this.
inline constexpr int kTipSdfMaxDim = kTipSdfRes + 2 * kTipSdfPad;
inline constexpr std::size_t kTipSdfMaxCells =
    static_cast<std::size_t>(kTipSdfMaxDim) * kTipSdfMaxDim;

// A signed distance field of one tip's silhouette, sampled on a `w x h` grid.
//
// GEOMETRY -- the contract the shader mirrors, and the one thing here that is easy to get subtly
// wrong. The grid is the tip's bounding box padded by `pad` cells of background on every side, so
// cell (i, j) samples the point
//
//     (i - pad + 0.5 - boxW/2,  j - pad + 0.5 - boxH/2)
//
// build px from the tip's CENTRE: the pad shifted out, and the pixel-centre convention (`stamp()`
// measures coverage at (x + 0.5, y + 0.5), so the pixel CONTAINING p is floor(p), never lround(p))
// already folded in.
//
// ⚠ `boxW`/`boxH` are NOT `w`/`h` minus the pad. The tip's box is CONTINUOUS -- a bitmap tip's box is
// `frameW * diameter / max(frameW, frameH)` px wide, which lands on a whole cell about never -- and
// the grid is the ceiling of it. Assume the two are equal and every bitmap tip's outline sits up to
// half a cell off its own box, which at a high zoom is several visible pixels of drift.
//
// `d` is NORMALIZED by `boxW` (a fraction of the tip's box width), negative inside. The shader turns
// that back into screen px by dividing by the field's screen-space gradient magnitude, so the
// normalizer only has to agree at both ends; `boxW` is chosen because it makes the scale intuitive
// (a distance of 1.0 is the tip's whole width).
struct TipSdf {
    int w = 0; // padded grid, cells
    int h = 0;
    int pad = 0;       // cells of background around the box, per side (kTipSdfPad)
    double boxW = 0.0; // the tip's TRUE (unpadded, continuous) extent, in the grid's build px
    double boxH = 0.0;
    std::vector<float> d; // w * h, row-major

    [[nodiscard]] bool empty() const noexcept {
        return w <= 0 || h <= 0 || d.size() != static_cast<std::size_t>(w) * h;
    }
    // Unchecked; `x < w && y < h` is the caller's contract.
    [[nodiscard]] float at(int x, int y) const noexcept {
        return d[static_cast<std::size_t>(y) * w + x];
    }
};

// Build the field for `tip`'s frame `frame`, squashed by `ratio` (height/width, the dab's), with its
// LONG axis scaled to `res` build px.
//
// The tip's envelope is its own (`tipDabShape`), which matters for a bitmap tip: a 300x80 stamp does
// NOT fill a square box, and a field built on one would place the outline where the tip is not.
//
// ⚠ `ratio` IS a parameter, and it is the one thing here that looks like it should not be. The dab's
// squash is an affine map of the tip's box, so for a bitmap tip and for five of the six generators
// the field could be built once at ratio 1 and stretched when sampled -- but NOT for a SPIKED one. A
// spiked generator folds the raw, un-normalized offset into an angular wedge BEFORE normalizing it
// (mask_generator.cpp's `fixRotation`), and folding commutes with an isotropic scale and not with an
// anisotropic one. At ratio 1 the fold is invisible: the rotation it applies preserves x^2 + y^2, so
// a spiked circle's silhouette is exactly the circle, star or no star. Squash it and the star appears
// -- which means a field built at ratio 1 and stretched would trace an ellipse over the very tip
// whose shape the fold exists to make. The angle stays out (a rotation IS applied when sampled).
//
// The build is O(area) plus two distance transforms, so the caller caches it on (tip raster, frame,
// ratio) and NOT on the diameter, the zoom, the angle or the cursor -- none of which change it.
//
// Returns an empty field if the tip paints nothing (a zero diameter, an empty bitmap, a frame index
// out of range) -- the caller then falls back to the analytic ellipse, which at least still says
// where the cursor is.
[[nodiscard]] TipSdf buildTipSdf(const BrushTip& tip, int frame, double ratio = 1.0,
                                 int res = kTipSdfRes);

// Does this tip need the traced outline, or does the analytic ellipse already tell the truth about it?
//
// FALSE (the ellipse, which the shader evaluates in closed form) for:
//   * a NULL tip -- the engine's built-in analytic circle. ⚠ This branch must stay, and must stay
//     first: every golden in the suite and the 125 bit-exact antialiasing equalities were laid by it.
//   * a plain procedural CIRCLE generator with `spikes <= 2`. Its silhouette IS the dab's ellipse, to
//     the last pixel, whatever its falloff does inside -- so tracing it would spend a grid to
//     reproduce a closed form.
//
// TRUE for every bitmap tip, every spiked generator (`spikes > 2` folds the tip into a star), and
// every non-circle generator -- a `rect` tip is a rectangle, and the shader's analytic path knows only
// ellipses, so a ring drawn for one over the other would be a lie of exactly the kind this fixes.
[[nodiscard]] bool tipNeedsSdf(const BrushTip* tip) noexcept;

namespace detail {

// One pass of Felzenszwalb & Huttenlocher's exact distance transform of a sampled 1-D function:
//   out[q] = min over p of ( (q - p)^2 + f[p] )
// `v` and `z` are the algorithm's scratch (the parabola hull); they are passed in so the 2-D driver
// allocates them once rather than once per row.
void edt1d(const std::vector<double>& f, std::vector<double>& out, std::vector<int>& v,
           std::vector<double>& z) noexcept;

// The exact squared Euclidean distance, in cells, from every cell to the nearest cell with
// `seed[i] != 0`. Cells with no seed anywhere get a large finite value, never an infinity.
[[nodiscard]] std::vector<double> squaredEdt(const std::vector<std::uint8_t>& seed, int w, int h);

} // namespace detail

} // namespace mosaic::core::brush
