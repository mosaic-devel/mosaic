#pragma once

#include "common/geometry.hpp"
#include "core/text/extrude.hpp"
#include "core/text/extrude_mesh.hpp"
#include "core/vector/object.hpp"

#include <cstdint>

// A vector object -> the extruded-solid mesh (docs/vector-model.md §11). The Shape/Pen side of the
// same 3D that the Type tool gives a text block: an Object's flattened contours are handed to the
// SAME mesher (core/text/extrude_mesh.hpp) and rendered by the SAME lanes
// (core/text/extrude_render.hpp), so shapes and text are one feature rather than two.
//
// The only thing this module decides is what the SOLIDS are, which is the one question a shape
// answers differently from a text block:
//
//   * run 0 -- the FILL region, present when the object is filled (a NoPaint fill contributes no
//     solid, exactly as it contributes no 2D ink).
//   * run 1 -- the STROKE outline, present when the object is stroked, built by vec::strokeOutline
//     so a dashed / round-joined outline extrudes as the dashes and joins it actually draws.
//
// Two runs, because Extrude::runMaterials is keyed by run: it is what lets a 3D shape carry a
// chrome outline around a matte face without any new machinery. An object that is neither filled
// nor stroked meshes to nothing, which is what it draws in 2D too.
namespace mosaic::core::vec {

// The curve-flattening tolerance a MESH is built at -- deliberately coarser than the 0.25 device px
// the 2D rasteriser fills at, and the single biggest lever on what a 3D shape costs.
//
// An outline point on a flat fill buys one thing: a hard edge against the backdrop, where a
// quarter-pixel deviation is visible. The same point on a SOLID buys a wall quad in every depth
// band -- two caps, a bevel ring per profile segment at each end, the wall itself -- and the
// silhouette it contributes is then shaded and bevelled, where a whole pixel is not. Measured on a
// 41-lobe stroked rosette: 0.25 px gave 328 outline points, a 205 ms mesh build and 45,876
// triangles; 1.0 px gives 205 points, a 45 ms build and 31,260 triangles -- 4.6x the build and
// 1.5x the per-composite rasterisation, for a difference nobody can point at on screen. (The
// stroke's boolean union below is superlinear in point count, which is why the build gains more
// than the triangle count alone suggests.)
inline constexpr double kMeshTolerancePx = 1.0;

// Build the solid for `object` under `params`. `tolerancePx` / `toDevice` tune the curve
// flattening -- pass the transform the mesh will be drawn through so a magnified shape is
// tessellated finely enough not to show facets. Empty mesh for an object with nothing to extrude.
[[nodiscard]] text::ExtrudeMesh
buildShapeExtrudeMesh(const Object& object, const text::Extrude& params,
                      double tolerancePx = kMeshTolerancePx,
                      const common::Affine2D& toDevice = common::Affine2D::identity());

// The design-space 2D bounds the mesh is built over -- the extent of the solids' contours, which is
// what ExtrudeMesh::designBounds is set to and what the camera pivots on. Cheap enough to ask for
// on its own (it flattens, it does not mesh), for the bounds/gizmo path.
[[nodiscard]] common::Rect
shapeExtrudeDesignBounds(const Object& object, double tolerancePx = kMeshTolerancePx,
                         const common::Affine2D& toDevice = common::Affine2D::identity());

// Everything about `object` + `params` that changes the MESH, hashed: the geometry, the stroke (it
// is a solid too), and the depth/bevel pair. Deliberately NOT the orientation, camera, lighting or
// materials -- those are render-side and must not re-mesh (docs/type-tool.md §10.5). This is the
// key a mesh cache is kept on; see core::VectorLayer::cachedExtrudeMesh.
[[nodiscard]] std::uint64_t shapeExtrudeMeshKey(const Object& object, const text::Extrude& params);

} // namespace mosaic::core::vec
