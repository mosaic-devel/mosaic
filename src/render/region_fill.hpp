#pragma once

#include "common/image.hpp"
#include "core/blend_mode.hpp"
#include "core/vector/paint.hpp"

#include <cstdint>
#include <vector>

namespace mosaic::render {

// Compute the result of filling a layer region with a solid colour — the shared core of
// Edit→Fill… (S39) and the bucket/pattern fill (S21). Pure + CPU, with no document or coordinate
// knowledge: the caller has already cropped the region out of the layer and mapped the document
// selection into `coverage` (layer-local). The blend math lives here in render/ because it leans on
// the one blend-mode definition in blend.hpp (core, where the command lives, may not depend on
// render).
//
//   region       the layer's current pixels for the rect being filled (its own width x height)
//   coverage     per-pixel selection coverage over `region` (0..255), row-major, size width*height;
//                an EMPTY vector means "fully covered" (no active selection → fill the whole
//                region)
//   fill         the solid fill colour (treated as fully opaque; its alpha is ignored)
//   mode         blend mode between the fill (source) and the existing pixels (backdrop)
//   opacity      fill strength in [0,1]; multiplied by the per-pixel coverage
//   protectAlpha "Protect Alpha" / preserve-transparency: only recolour pixels that already have
//                alpha > 0, and leave the alpha channel unchanged (transparent stays transparent)
//
// Returns a fresh region image with the same extent as `region`.
[[nodiscard]] common::Image computeFill(const common::Image& region,
                                        const std::vector<std::uint8_t>& coverage,
                                        common::Color8 fill, core::BlendMode mode, float opacity,
                                        bool protectAlpha);

// The paint-aware sibling of computeFill: fills a layer region with any core::vec::Paint (a
// gradient or a pattern, as well as a solid), used by Edit→Fill… when the Contents is Gradient or
// Pattern. The paint is evaluated per pixel via vec::sampleAt, source-over-composited with
// `mode`/`opacity`, and coverage/protectAlpha behave exactly as in computeFill. The paint's
// coordinate space matches the rest of the app (see paint.hpp / layer_effects_render.cpp):
//   * A GRADIENT is keyed to the region, NORMALISED to [0,1]^2, so it runs across the fill area
//   (its
//     own transform + spread on top) -- like a gradient overlay spanning its content box.
//   * A PATTERN tiles in LAYER PX with a fixed feature size, so it is sampled at (originX+x,
//   originY+y):
//     `scale` is a real px period and the tiling is stable regardless of the region's size.
//   `antialias` (the document-wide AA setting) only affects pattern edges; solids/gradients ignore
//   it.
//
//   originX/originY  the region's top-left in layer-pixel space (for pattern tiling + gradient
//   span)
[[nodiscard]] common::Image computeFillPaint(const common::Image& region,
                                             const std::vector<std::uint8_t>& coverage,
                                             const core::vec::Paint& paint, long originX,
                                             long originY, core::BlendMode mode, float opacity,
                                             bool protectAlpha, bool antialias);

} // namespace mosaic::render
