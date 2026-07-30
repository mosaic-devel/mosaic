#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/geometry.hpp"    // Rect / Vec2 (design bounds, UVs)
#include "common/geometry3d.hpp"  // Vec3
#include "core/text/extrude.hpp"  // Extrude params
#include "core/vector/geometry.hpp"  // vec::Contours (the §5 seam)

// Contours -> the extruded-text mesh (docs/type-tool.md §10.2, S30-c). One watertight solid per
// glyph at ONE shared depth/bevel (the whole block shares them, §1.2), triangulated caps (earcut),
// quad-strip walls with smoothed-along-curves / sharp-at-corners normals, front/back bevel rings
// per Bevel::Profile, and design-space UVs on every vertex (§12 -- Layer Effects' sampling domain).
// Regenerated only when text/contours/depth/bevel change; rotate/light/recolour are render-side
// uniform updates (§10.5). Pure geometry: FLTK/FreeType/Vulkan-free, headless-tested.
namespace mosaic::core::text {

using common::Vec2;
using common::Vec3;

struct ExtrudeVertex {
    Vec3 position;  // model space: x/y = the glyphs' layer (design) space, z in [-depth/2, +depth/2]
    Vec3 normal;    // unit, analytic (never derived from winding)
    Vec2 uv;        // design-space UV, normalized over ExtrudeMesh::designBounds (§12)
    // The UNROLLED side parameterization (§12 wrap mode, S30-e): x = arc length along the outline
    // (normalized over ExtrudeMesh::sideLength, contours concatenated), y = depth position
    // (0 = front cap plane, 1 = back, normalized over the extrude depth). Walls AND bevels carry
    // it (a bevel is the top/bottom strip of the same band); caps leave it {0,0} (unused). This is
    // the developable "label wrapped around the solid" domain a pattern tiles UNDISTORTED in --
    // sampling the flat design UV on a wall stretches any texture into ruler lines down the depth.
    Vec2 side;
    // 1 on CAP triangles (front/back face), 0 on walls/bevels. Authored by the mesher (caps are
    // known exactly -- no normal-threshold guessing); triangles never mix the two, so per-triangle
    // reads need no interpolation. Drives Extrude::reflectSidesOnly in both render lanes.
    float cap = 0.0f;
};

// One knot of the side domain's return map: at unrolled position `s` (normalized over sideLength)
// the outline passes through design point `design`. Piecewise-linear between knots; the S30-e
// overlay builder uses it to evaluate a gradient's design-space continuation on the wall band.
struct SideStation {
    float s = 0.0f;
    Vec2 design;
};

// A contiguous run of `indices` drawn with one material: the §10.4 partition. `runIndex` keys
// Extrude::runMaterials (falling back to Extrude::material).
struct ExtrudeMeshRange {
    std::size_t runIndex = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
};

struct ExtrudeMesh {
    std::vector<ExtrudeVertex> vertices;
    std::vector<std::uint32_t> indices;  // triangle list (3 per triangle)
    std::vector<ExtrudeMeshRange> ranges;
    common::Rect designBounds;  // the 2D extent UVs are normalized over (all input outlines)
    double sideLength = 0.0;    // total unrolled outline length (design units; ExtrudeVertex::side)
    std::vector<SideStation> sideStations;  // s -> outline design point, sorted by s

    [[nodiscard]] bool empty() const noexcept { return indices.empty(); }
    [[nodiscard]] std::size_t triangleCount() const noexcept { return indices.size() / 3; }
};

// One glyph's flattened outlines (closed contours; open ones are ignored) tagged with the run
// whose material it draws with. Callers pass glyphs in run order so ranges stay contiguous.
struct GlyphSolidInput {
    vec::Contours contours;
    std::size_t runIndex = 0;
};

// Build the solid. Ring roles are derived from CONTAINMENT DEPTH (even = outer, odd = hole), so
// any input winding works; bevel sizes are clamped to the depth. Empty input -> an empty mesh.
[[nodiscard]] ExtrudeMesh buildExtrudeMesh(const std::vector<GlyphSolidInput>& glyphs,
                                           const Extrude& params);

}  // namespace mosaic::core::text
