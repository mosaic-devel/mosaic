#pragma once

// Smart Resize (content-aware cropping) for Mosaic — the importance map W(x,y)
// (PLAN S16-f; docs/smart-resize-research.md §4.1).
//
// A single-channel float map at a downsampled working resolution scoring how much each region
// of the picture matters, built from three plain signals:
//   1. gradient/edge energy — L1 gradient magnitude of the luminance. Textbook finite-difference
//      math. ⚠ INVARIANT: this energy image exists to be swept by a CROP-WINDOW search and
//      nothing else — Mosaic never computes a seam, and seam carving is deliberately not an
//      operator in this codebase. That is a hard architectural constraint, not an omission.
//   2. local edge density — a box average of (1): a texture-vs-flat discriminator so the crop
//      search prefers to discard flat regions (sky, water, grass).
//   3. composition prior — a mild centre weighting (photographic convention) that keeps the
//      optimizer from hugging a corner.
// The Viola-Jones face boost (§4.2) was scoped and DROPPED (2026-07-02 — not worth shipping
// cascade data); the map is (1)+(2)+(3), full stop. If a detector ever lands, its boxes fuse
// into THIS builder (the standing guardrail: ONE map, always).
//
// Lineage: saliency-driven crop-window selection follows Suh, Ling, Bederson & Jacobs,
// "Automatic Thumbnail Cropping and its Effectiveness" (UIST 2003), Chen et al. 2003, and
// Liu & Gleicher 2006; cropping-as-the-preferred-operator follows
// Rubinstein, Gutierrez, Sorkine & Shamir, "A Comparative Study of Image Retargeting"
// (SIGGRAPH Asia 2010, the RetargetMe benchmark). FLTK-free, deterministic (fixed iteration
// order, no RNG), unit-tested headless in tests/test_retarget.cpp.

#include "common/image.hpp" // mosaic::common::Image (the composited document pixels)

#include <cstdint>
#include <vector>

namespace mosaic::core::retarget {

// Tunables for buildImportanceMap. The defaults are the "balanced" behaviour; the weights are
// relative (W is used only to *compare* candidate crop windows, so absolute scale is irrelevant).
struct ImportanceOptions {
    int maxDim = 320;                // long side of the working-resolution map (never upsampled)
    double gradientWeight = 1.0;     // signal 1: edge/structure energy
    double edgeDensityWeight = 0.35; // signal 2: box-averaged energy (texture vs flat)
    double centerPriorWeight = 0.15; // signal 3: mild centre weighting
};

// W at working resolution plus the source dimensions it was built from, so a crop-window search
// over the map can hand back a rectangle in document space.
struct ImportanceMap {
    std::uint32_t width = 0;   // map (working) resolution
    std::uint32_t height = 0;
    std::uint32_t sourceW = 0; // the analysed image's size (document space)
    std::uint32_t sourceH = 0;
    std::vector<float> w;      // width * height, row-major, all values >= 0

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
    // Unchecked access (caller guarantees x < width, y < height).
    [[nodiscard]] float at(std::uint32_t x, std::uint32_t y) const noexcept {
        return w[static_cast<std::size_t>(y) * width + x];
    }
};

// Build W for `src` (the composited document, straight 8-bit RGBA; transparent pixels carry no
// importance). Downsamples to `opts.maxDim` on the long side by exact box averaging, then sums
// the weighted signals above. Deterministic; an empty image yields an empty map.
[[nodiscard]] ImportanceMap buildImportanceMap(const common::Image& src,
                                               const ImportanceOptions& opts = {});

} // namespace mosaic::core::retarget
