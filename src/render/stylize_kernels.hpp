#pragma once

#include <cstdint>

#include "common/geometry.hpp"
#include "common/image.hpp"

// The S35 stylize-kernel engine (docs/filters-stylize.md §3): the CPU reference lane for every
// artistic/stylize adjustment kind. Pure float math, no FLTK / no GPU -- the twin of
// render/blur.hpp for the S33 family, and deliberately a separate translation unit from the
// dispatch in stylize.cpp so a kernel can be read (and tested) on its own.
//
// Shared contract (every kernel), mirroring blur.hpp so the two families behave alike:
//   * STRAIGHT-alpha ImageF in / out, transformed IN PLACE. Internally each kernel that reads
//     neighbours premultiplies, filters, un-premultiplies -- straight-space filtering would
//     bleed the (arbitrary) RGB of fully-transparent pixels into visible ones.
//   * ALPHA is only moved by the kernels that genuinely RESAMPLE coverage -- pixelate, oil paint
//     and wave, all of which relocate content the way a blur does. Sharpen, unsharp, emboss,
//     denoise, noise and vignette leave alpha exactly as they found it: they recolor the
//     backdrop, they do not add or move coverage (docs/adjustment-layers.md §2). Each kernel
//     below states which it is.
//   * Edge policy: clamp-to-edge (replicate), like the blur family -- a stylize filter must not
//     vignette (or ring) at the canvas edge, and a region buffer's edge taps then land on the
//     same physical pixel the full composite's do, which is what makes region == crop(full)
//     hold (docs/blur-filters.md §5).
//   * Deterministic banded parallelism, bit-identical to the serial loop (the compositor's
//     parallelFor rule). The one deliberately SERIAL pass is pixelate's cell accumulation --
//     see pixelateImage.
//   * All lengths are BUFFER px unless the parameter says "parent"; the caller (stylize.cpp)
//     has already scaled the schema's parent-space px by the walk's placement.
//   * A zero/degenerate amount is a no-op, and the caller checks for that BEFORE copying the
//     backdrop, so an identity-parameter layer never touches a pixel (docs/adjustment-layers.md
//     §1 identity rule).
namespace mosaic::render::fx {

// ---- Sharpen / Unsharp mask -----------------------------------------------------------------

// The textbook 3x3 sharpening kernel, scaled: out = p + amount * (p - mean4(p)), where mean4 is
// the four-neighbour (von Neumann) mean. At amount == 1 this is exactly the classic
// [[0,-1,0],[-1,5,-1],[0,-1,0]] convolution. Premultiplied RGB; ALPHA UNTOUCHED.
void sharpenImage(common::ImageF& img, float amount);

// Unsharp masking (the darkroom technique, digital form): out = p + amount * (p - gaussian(p)),
// gated per pixel by `threshold01` on the |luma| of the difference so flat, noisy areas are left
// alone. Premultiplied RGB; ALPHA UNTOUCHED. `draft` truncates the Gaussian's support from 3
// sigma to 2 sigma for live-drag composites (the settled composite always runs full quality).
void unsharpMaskImage(common::ImageF& img, float sigma, float amount, float threshold01,
                      bool draft);

// High pass (S34-a): out = 1/2 + (p - gaussian(p)), per channel -- the unsharp mask's OWN
// difference term, drawn on its own instead of added back. The missing half of frequency
// separation: the low band is a Gaussian Blur layer, this is what is left over. Mid-grey is the
// zero of the difference, which is exactly why the result composites through Overlay / Soft Light
// (both leave 0.5 alone) and why it reads as "flat grey plus edges".
//
// The difference is taken on PREMULTIPLIED planes (a transparent neighbour must not bleed its
// arbitrary RGB in) and then un-premultiplied before the 1/2 bias is added, so the bias is a
// straight-space constant rather than something divided by coverage. Result clamped to [0,1] --
// a high pass is a display-space read, and HDR headroom has no meaning around a fixed mid-grey.
// ALPHA UNTOUCHED. `draft` truncates the Gaussian's support from 3 sigma to 2, like unsharp.
void highPassImage(common::ImageF& img, float sigma, bool draft);

// ---- Add noise ------------------------------------------------------------------------------

// Adds IID noise of std-dev `sigma` (in encoded [0,1] channel units) to RGB. ALPHA UNTOUCHED.
//
// DETERMINISM (docs/filters-stylize.md §4): the sample is a pure hash of (seed, the floor of the
// pixel's PARENT-space coordinate, channel) -- never a running RNG. So the grain is identical on
// every recomposite, identical between a full composite and a dirty-rect region composite, and
// pinned to the document instead of swimming under pan/zoom. `bufToParent` is the inverse of the
// walk's placement. `uniform` picks the uniform distribution over the Gaussian, scaled to the
// SAME variance so the Amount slider means one thing in both; `monochrome` draws one sample per
// pixel instead of one per channel.
void addNoiseImage(common::ImageF& img, const common::Affine2D& bufToParent, float sigma,
                   bool uniform, bool monochrome, std::uint32_t seed);

// ---- Denoise --------------------------------------------------------------------------------

// Lee's local linear MMSE filter (J.-S. Lee, "Digital image enhancement and noise filtering by
// use of local statistics", IEEE TPAMI 1980): out = m + k * (p - m) over a (2r+1)^2 window, with
// m the local mean and k = max(0, var - noiseVar) / var. Flat areas (var <= noiseVar) collapse to
// the mean; edges (var >> noiseVar) pass through untouched, which is what keeps it from smearing.
// O(1) per pixel via separable running-sum box means. Premultiplied RGB; ALPHA UNTOUCHED.
void denoiseImage(common::ImageF& img, int radius, float noiseSigma);

// ---- Pixelate -------------------------------------------------------------------------------

// Mosaic cells: every pixel takes the mean of the cell it falls in. The cell LATTICE is anchored
// in PARENT space (cell index = floor(parentCoord / cellParent)), not in the buffer, so the
// blocks stay put under a region crop, a pan and a scaled preview -- and a region composite's
// blocks are byte-identical to the full composite's. Premultiplied RGBA: ALPHA IS AVERAGED TOO
// (a mosaic resamples coverage the way a blur does).
void pixelateImage(common::ImageF& img, const common::Affine2D& bufToParent, double cellParent);

// ---- Emboss ---------------------------------------------------------------------------------

// Directional relief: out.rgb = 0.5 + amount * (L(p + off/2) - L(p - off/2)) on the premultiplied
// luma plane, clamped to [0,1]; the result is GRAY (a relief map is a height read, not a
// recoloring). ALPHA UNTOUCHED, so an embossed transparent region stays transparent.
void embossImage(common::ImageF& img, float offX, float offY, float amount);

// ---- Oil paint ------------------------------------------------------------------------------

// The Kuwahara filter (M. Kuwahara, K. Hachimura, S. Haruyama, M. Kinoshita, 1976): the window is
// split into four overlapping quadrants and the pixel takes the MEAN of the quadrant with the
// smallest luminance VARIANCE, which flattens interiors into paint-like patches while leaving
// edges where they are. `half` is the quadrant box half-width, so the full window is 4*half+1.
// O(1) per pixel: the four quadrant means/variances are separable box means read at the four
// (+-half, +-half) offsets. Premultiplied RGBA: ALPHA IS AVERAGED with its quadrant.
void oilPaintImage(common::ImageF& img, int half);

// ---- Wave / Ripple --------------------------------------------------------------------------

// A sinusoidal displacement resample. Everything is computed in PARENT space so the wave rides
// the document (region- and preview-stable) and only the final sample position is mapped back
// into the buffer by `pre`.
//   Wave:   displacement = amplitude * sin(2*pi * (p . n) / wavelength + phase) * dir,
//           with dir = (cos angle, sin angle) and n its perpendicular -- at angle 0 each row
//           slides horizontally by a sine of its y, the classic wave.
//   Ripple: displacement = amplitude * sin(2*pi * |p - center| / wavelength + phase) radially
//           outward from `center`.
// Source reads are bilinear on premultiplied RGBA with clamp-to-edge; ALPHA IS RESAMPLED TOO.
struct WaveOp {
    bool ripple = false;
    double amplitude = 0.0;    // parent px
    double wavelength = 1.0;   // parent px (> 0)
    double dirX = 1.0;         // unit direction, parent space
    double dirY = 0.0;
    double phase = 0.0;        // radians
    common::Vec2 center{};     // parent px (Ripple only)
};
void waveImage(common::ImageF& img, const common::Affine2D& pre,
               const common::Affine2D& bufToParent, const WaveOp& op);

// ---- Vignette -------------------------------------------------------------------------------

// A radial exposure falloff, per pixel, evaluated on the PARENT-space distance from `center`:
//   q = (|dx/radius|^n + |dy/radius|^n)^(1/n)      (n = the superellipse exponent)
//   t = smoothstep(0, 1, (q - 1) / (outer - 1))    (0 inside the un-vignetted core)
//   gain = 2^(exposure * t), applied in LINEAR light (the sRGB LUT pair), alpha untouched.
// q <= 1 returns the backdrop BYTE-IDENTICALLY (the kernel skips the pixel outright), so the
// centre of a vignette is provably untouched -- no decode/encode round-trip error there.
struct VignetteOp {
    common::Vec2 center{};   // parent px
    double radius = 1.0;     // parent px, > 0
    double outer = 1.0;      // falloff end in q units, >= 1 (1 = a hard edge)
    double exponent = 2.0;   // 2 = ellipse; > 2 squarer; < 2 diamond-ward
    float exposure = 0.0f;   // EV applied at t == 1
};
void vignetteImage(common::ImageF& img, const common::Affine2D& bufToParent,
                   const VignetteOp& op);

}  // namespace mosaic::render::fx
