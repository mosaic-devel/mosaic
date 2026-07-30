#pragma once

#include <vector>

#include "common/image.hpp"

// The S33 blur-kernel engine (docs/blur-filters.md §3): the CPU reference lane for every blur
// adjustment kind. Pure float math, no FLTK / no GPU; effect_primitives holds the single-plane
// primitives, this holds the RGBA machinery the compositor's blur branch drives.
//
// Shared contract (every kernel):
//   * STRAIGHT-alpha ImageF in / out, transformed IN PLACE. Internally each kernel
//     premultiplies, convolves, un-premultiplies -- straight-space convolution would bleed the
//     (arbitrary) RGB of fully-transparent pixels into visible ones. Alpha convolves along with
//     color: a blur MOVES coverage, it does not recolor it (§2).
//   * Edge policy: clamp-to-edge (replicate). Blurring a composite must not vignette at the
//     canvas edge. (Deliberately NOT effect_primitives' reflect-101 -- that blurs coverage
//     planes for effects; these blur content. Divergence documented here on purpose.)
//   * Deterministic banded parallelism, bit-identical to the serial loop (the compositor's
//     parallelFor rule) -- goldens and the region==full equivalence test depend on it.
//   * All lengths (sigma / radius / distance) are image px. Non-positive amounts and empty
//     images are no-ops.
//   * `draft` (where offered) subsamples gather taps for live-drag composites (S30-draft
//     pattern); the settled composite always runs full quality. Both are deterministic.
namespace mosaic::render::fx {

// Separable Gaussian, std-dev `sigma` (support 3*sigma).
void gaussianBlurImage(common::ImageF& img, float sigma);

// One exact separable box pass, half-width `radius` (running sum, O(1)/px). The flat, cheap
// look is the point -- this is NOT the Kovesi 3-pass Gaussian approximation.
void boxBlurImage(common::ImageF& img, int radius);

// Uniform line integral: bilinear taps along +-distance/2 in direction `angleRad`
// (0 = +x, measured toward +y).
void motionBlurImage(common::ImageF& img, float angleRad, float distancePx, bool draft);

// Rotational smear about (cx, cy): per pixel, average along its circular arc, total arc
// `arcDeg` degrees (capped at 100), tap count proportional to arc length at that radius.
void spinBlurImage(common::ImageF& img, double cx, double cy, float arcDeg, bool draft);

// Radial streak toward (cx, cy): per pixel, average along the segment from the pixel toward
// the center, length = `frac` (0..1) of its center distance.
void zoomBlurImage(common::ImageF& img, double cx, double cy, float frac, bool draft);

// Edge-preserving bilateral, separable two-pass approximation (Pham & van Vliet 2005 lineage;
// see docs/blur-filters.md §3/§7 -- the guided filter is fenced, never "upgrade" to it).
// Spatial std-dev = radius/2; `threshold01` is the range std-dev on premultiplied luma.
void surfaceBlurImage(common::ImageF& img, float radius, float threshold01);

// A baked aperture-shaped gather kernel for lens bokeh: an N-blade polygon (3..8) rotated by
// `rotationRad`, morphed toward a disc by `curvature01` (0 = hard polygon, 1 = circle), with
// anti-aliased edge coverage. Taps are baked at `stride`-spaced offsets with weights
// renormalized to sum 1, so subsampled kernels conserve mass.
struct ApertureKernel {
    int radius = 0;              // tap support radius, px
    int stride = 1;              // tap spacing actually baked (1 = exact)
    std::vector<float> offX;     // tap offsets, px (stride-spaced lattice)
    std::vector<float> offY;
    std::vector<float> weight;   // per-tap, sums to 1
};
[[nodiscard]] ApertureKernel makeApertureKernel(float radius, int blades, float curvature01,
                                                float rotationRad, bool draft);

// Aperture gather in LINEAR light (decode -> gather -> encode; the bokeh look lives there).
// `boost01` > 0 gain-boosts pixels whose linear luma exceeds `boostThreshold01` before the
// gather (and never un-boosts -- the bloom IS the point), so clipped SDR highlights still
// bloom into aperture shapes.
void lensBlurImage(common::ImageF& img, const ApertureKernel& k, float boost01,
                   float boostThreshold01);

// Spatially-varying blur: `radiusPlane` (size w*h) holds each pixel's blur radius in px,
// values in [0, maxRadius]. Interpolates across a pyramid of pre-blurred levels at radii
// {0, 1/4, 1/2, 3/4, 1} * maxRadius (docs/blur-filters.md §3): `iris` false = Gaussian levels
// (sigma = level/2), true = hexagonal aperture gather levels. A zero field entry returns the
// backdrop byte-identically (level 0 is the untouched image); a saturated one returns the top
// level exactly.
void dofBlurImage(common::ImageF& img, const std::vector<float>& radiusPlane, float maxRadius,
                  bool iris, bool draft);

}  // namespace mosaic::render::fx
