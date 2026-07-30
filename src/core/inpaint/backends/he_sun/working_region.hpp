#pragma once

// Working-region extraction for the He & Sun offset-statistics backend (PLAN S37-c).
// Crops the image to the hole's neighbourhood (its bounding box grown 3x in each dim, clamped) and
// box-average downsamples so the longest side fits maxRegionW/H — the offset statistics run on this
// small region, then offsets are rescaled by `scale`. Pure geometry + averaging.

#include <optional>

#include "common/geometry.hpp"  // common::Rect
#include "common/image.hpp"
#include "core/inpaint/inpaint_backend.hpp"  // Params
#include "core/selection.hpp"

namespace mosaic::core::inpaint {

// The document-pixel rectangle the offset statistics analyse for a hole whose bounding box is
// `holeBounds` (nullopt = no coverage => the whole image), in an `imageW` x `imageH` image: the
// bbox grown by the margin multiplier (adaptive per Params) and clamped/shifted to stay in bounds.
// Pure geometry, shared by extractWorkingRegion and the UI's sample-area preview (S39).
[[nodiscard]] common::Rect workingRegionRect(std::uint32_t imageW, std::uint32_t imageH,
                                             const std::optional<common::Rect>& holeBounds,
                                             const Params& p);

struct WorkingRegion {
    common::ImageF image;        // cropped + downsampled working image
    int            scale = 1;    // source pixels per working pixel (>= 1)
    int            originX = 0;  // crop top-left in source coordinates
    int            originY = 0;
    int            regionW = 0;  // crop size in source pixels (before downsample)
    int            regionH = 0;
};

// Extract the working region for the given hole. An empty hole mask uses the whole image as the
// region. The returned `image` is empty only if the source is empty.
[[nodiscard]] WorkingRegion extractWorkingRegion(const common::ImageF& image,
                                                 const Selection& holeMask, const Params& p);

}  // namespace mosaic::core::inpaint
