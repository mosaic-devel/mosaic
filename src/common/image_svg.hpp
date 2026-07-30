#pragma once

#include "common/image.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace mosaic::common {

// Rasterize an SVG document (held in `data`, `len` bytes) into a `width` x `height` RGBA8
// image. The drawing is uniformly scaled to fit the target box (preserving aspect ratio)
// and centered; uncovered pixels are left transparent. Returns an empty Image and sets
// *error on failure (parse error, zero size). Backed by the vendored nanosvg (zlib).
//
// This is the shared SVG entry point: the app icon is baked in at build time and
// rasterized through here at startup, and runtime tool-icon rendering (S52) will reuse it.
[[nodiscard]] Image rasterizeSvg(const unsigned char* data, std::size_t len, int width, int height,
                                 std::string* error = nullptr);

// The document's intrinsic size in px at 96 dpi (its width/height attributes, else its viewBox)
// -- what a caller needs to pick an aspect-true raster size BEFORE rasterizing (the SVG brush
// tips render at a fixed width with the height derived from this, docs/brushes.md §3.6).
// Nullopt when the document does not parse or its size is degenerate.
struct SvgSize {
    double width = 0.0;
    double height = 0.0;
};
[[nodiscard]] std::optional<SvgSize> svgIntrinsicSize(const unsigned char* data, std::size_t len);

} // namespace mosaic::common
