#pragma once

#include <optional>

#include "common/geometry.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/object.hpp"

// The flatten seam (docs/vector-model.md §2.1) -- the one chokepoint every consumer of vector
// geometry routes through. flatten() reduces any Geometry to polyline Contours; fill, stroke,
// hit-test, mask coverage, bounds and SVG export all build on that, which is what keeps each
// later feature (booleans, text-on-path, masks, rasterize) additive rather than a rewrite.
//
// Renderer-agnostic on purpose: flatten() serves the CPU consumers (hit-test, bounds, export,
// the bring-up rasterizer), while a future GPU-resident path can read raw control points from
// the same Geometry. Nothing here touches the GPU.
namespace mosaic::core::vec {

// Reduce `geometry` to polyline contours in layer-local space. Béziers and curved primitives
// are subdivided until their deviation is below `tolerancePx` measured in DEVICE pixels -- pass
// the layer-local -> device transform as `toDevice` so curve smoothness tracks zoom (use the
// identity for resolution-independent uses like bounds/export).
[[nodiscard]] Contours flatten(const Geometry& geometry, double tolerancePx = 0.25,
                               const common::Affine2D& toDevice = common::Affine2D::identity());

// Tight axis-aligned bounds of `geometry` in layer-local space (nullopt when it has no extent).
[[nodiscard]] std::optional<common::Rect> contentBounds(const Geometry& geometry);

// Bounds of a whole object INCLUDING its stroke's outward reach (half-width for Center, full width
// for Outside, nothing for Inside), in layer-local space. This is what VectorLayer::contentBounds()
// uses so the Move gizmo / thumbnails frame the stroked shape, not just the geometry. Miter spikes
// are not accounted for -- the rasterizer derives its own tight pixel bbox from the actual outline.
[[nodiscard]] std::optional<common::Rect> contentBounds(const Object& object);

// Position + unit tangent at `arcDistance` along the flattened contours, walking segments in
// order and clamping to the ends. The shared arc-length primitive behind BOTH dashed strokes
// (dashOffset is a distance) and text-on-path/-shape (S30) -- built once, reused three ways.
struct PathSample {
    Vec2 pos;
    Vec2 tangent;  // unit length (falls back to {1,0} on a degenerate/empty path)
};
[[nodiscard]] PathSample samplePathAt(const Contours& contours, double arcDistance);

// Total arc length of all contours (closing segments of closed contours included).
[[nodiscard]] double contourLength(const Contours& contours);

// The arc-distance along `contours` of the point nearest `p` (the inverse of samplePathAt's
// position half). Walks every segment, projecting `p` onto each; `outDistance` (optional)
// receives the point-to-path distance. 0 for an empty/degenerate path. Drives the S30
// text-on-path range-handle drags: the pointer maps to a bracket's new arc-distance.
[[nodiscard]] double nearestArcDistance(const Contours& contours, Vec2 p,
                                        double* outDistance = nullptr);

}  // namespace mosaic::core::vec
