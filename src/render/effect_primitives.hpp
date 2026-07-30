#pragma once

#include <vector>

#include "common/image.hpp"

// Shared image primitives for layer effects (LE-a) and the S33 blur filters -- built once here,
// reused by both (docs/layer-effects.md §4). Pure CPU float math, no FLTK / no GPU. Technique
// lineage: separable Gaussian (textbook); fast almost-Gaussian box approximation (P. Kovesi,
// 2010); signed distance transform (Felzenszwalb & Huttenlocher, 2004). All public domain.
namespace mosaic::render::fx {

// The straight-alpha channel of `img` as a width*height row-major plane.
[[nodiscard]] std::vector<float> extractAlpha(const common::ImageF& img);

// In-place separable Gaussian blur of a single-channel `w`*`h` float plane, std-dev `sigma` px
// (reflect-101 edges). The exact reference; `sigma <= 0` is a no-op.
void gaussianBlur(std::vector<float>& plane, int w, int h, float sigma);

// In-place fast almost-Gaussian blur (three successive box passes, Kovesi 2010), std-dev
// ~`sigma`. Linear time regardless of radius -- the large-radius path; by the third pass it is
// visually indistinguishable from a true Gaussian. `sigma <= 0` is a no-op.
void boxBlurApprox(std::vector<float>& plane, int w, int h, float sigma);

// Signed Euclidean distance (px) to the alpha = 0.5 contour of a `w`*`h` coverage plane, via the
// exact Felzenszwalb-Huttenlocher (2004) distance transform. NEGATIVE inside the shape
// (alpha >= 0.5), POSITIVE outside, ~0 at the edge. The shared engine behind stroke (offset the
// field into a band), bevel (height = clamped field) and the shadow/glow choke.
[[nodiscard]] std::vector<float> signedDistanceField(const std::vector<float>& alpha, int w, int h);

// Anti-aliased signed distance field: bilinearly upsample the coverage `ss`x before the transform,
// so the field is built from the sub-pixel (interpolated) 0.5 crossing instead of the 1x binary
// staircase, then average back down. An offset iso-contour (a stroke's outer edge) derived from it
// is SMOOTH, not bumpy along an anti-aliased content edge. `ss <= 1` falls through to the plain
// transform. Costs ~ss*ss the work, so callers bound it to the effect ROI.
[[nodiscard]] std::vector<float> signedDistanceFieldAA(const std::vector<float>& alpha, int w, int h,
                                                       int ss);

}  // namespace mosaic::render::fx
