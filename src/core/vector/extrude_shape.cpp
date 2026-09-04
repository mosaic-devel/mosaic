#include "core/vector/extrude_shape.hpp"

#include "core/vector/boolean.hpp" // normalizedContours: the stroke pieces -> one ribbon
#include "core/vector/flatten.hpp"
#include "core/vector/stroke.hpp"

#include <algorithm>
#include <cstring>
#include <variant>

namespace mosaic::core::vec {
namespace {

// The solids an object contributes, in run order (see the header). Shared by the mesh build and
// the design-bounds query so the two can never disagree about what the solid IS.
std::vector<text::GlyphSolidInput> shapeSolids(const Object& object, double tolerancePx,
                                               const common::Affine2D& toDevice) {
    std::vector<text::GlyphSolidInput> solids;
    const Contours geo = flatten(object.geometry, tolerancePx, toDevice);
    if (geo.empty())
        return solids;
    if (!std::holds_alternative<NoPaint>(object.fill))
        solids.push_back({geo, 0});
    if (object.stroke.enabled && object.stroke.width > 0.0 &&
        !std::holds_alternative<NoPaint>(object.stroke.paint)) {
        // ⚠ strokeOutline's output has to be UNIONED before it can be a solid, and this is the
        // single most expensive thing about a 3D shape if it is not.
        //
        // The stroker deliberately emits one OVERLAPPING PIECE per segment and per join, all wound
        // the same way, because a NonZero *fill* unions them for free -- that is what sidesteps the
        // self-intersection problems of a single offset outline (see stroke.hpp). A 2D rasterizer
        // is happy with that. A MESHER is not: it turns each piece into its own watertight solid,
        // with its own caps and its own bevel rings. Measured on a 41-lobe stroked rosette:
        // strokeOutline returned 656 pieces for a 328-point path, which meshed to 69,708 triangles
        // -- 88% of the whole solid -- of hundreds of overlapping little boxes with interior walls
        // z-fighting inside the ribbon. Not merely slow: wrong.
        //
        // normalizedContours re-emits the same region as non-overlapping NonZero rings -- one
        // ribbon boundary plus its holes -- which is what a solid is made of. The boolean kernel
        // costs a fraction of what meshing the pieces did, and it runs behind the mesh cache.
        const Contours pieces = strokeOutline(geo, object.stroke, tolerancePx, toDevice);
        if (!pieces.empty()) {
            Contours ribbon = normalizedContours(pieces, FillRule::NonZero);
            if (!ribbon.empty())
                solids.push_back({std::move(ribbon), 1});
        }
    }
    return solids;
}

void hashBytes(std::uint64_t& h, const void* data, std::size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
}
template <typename T> void hashPod(std::uint64_t& h, const T& v) {
    hashBytes(h, &v, sizeof(v));
}

} // namespace

text::ExtrudeMesh buildShapeExtrudeMesh(const Object& object, const text::Extrude& params,
                                        double tolerancePx, const common::Affine2D& toDevice) {
    return text::buildExtrudeMesh(shapeSolids(object, tolerancePx, toDevice), params);
}

common::Rect shapeExtrudeDesignBounds(const Object& object, double tolerancePx,
                                      const common::Affine2D& toDevice) {
    // ⚠ Accumulate min/max, NOT Rect::united over degenerate point rects. `united` treats an EMPTY
    // rect as "nothing" and a zero-size rect IS empty (w <= 0), so folding points in as {x,y,0,0}
    // returns the empty rect it started with, every time -- which read as "this shape has no
    // bounds" and left the 3D popup's viewport blank (user 2026-08-28).
    bool any = false;
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    for (const text::GlyphSolidInput& s : shapeSolids(object, tolerancePx, toDevice))
        for (const Contour& c : s.contours)
            for (const common::Vec2& p : c.points) {
                if (!any) {
                    x0 = x1 = p.x;
                    y0 = y1 = p.y;
                    any = true;
                    continue;
                }
                x0 = std::min(x0, p.x);
                y0 = std::min(y0, p.y);
                x1 = std::max(x1, p.x);
                y1 = std::max(y1, p.y);
            }
    return any ? common::Rect{x0, y0, x1 - x0, y1 - y0} : common::Rect{};
}

std::uint64_t shapeExtrudeMeshKey(const Object& object, const text::Extrude& params) {
    // ⚠ A HASH, not a comparison, and it keys a CACHE -- so a collision serves a stale mesh rather
    // than corrupting anything, and 64 FNV-1a bits over this much input make that not a thing that
    // happens. It is a hash because the alternative is keeping a whole copy of the geometry beside
    // the mesh to compare against, which for a 41-lobe rosette costs more than the mesh.
    std::uint64_t h = 1469598103934665603ull;
    // The geometry, flattened at the FIXED module tolerance -- never the caller's render one. Two
    // composites at different zooms must not each claim the mesh is stale; the mesh is
    // re-tessellated when the SHAPE changes, and the flattening tolerance rides the same cache
    // miss the shape does. (Measured at 0.03 ms on a 41-lobe rosette, against a mesh build of 45 --
    // it is the cheap half of the cache by three orders of magnitude, which is the point.)
    for (const Contour& c : flatten(object.geometry, kMeshTolerancePx)) {
        hashPod(h, c.closed);
        for (const common::Vec2& p : c.points) {
            hashPod(h, p.x);
            hashPod(h, p.y);
        }
    }
    // Whether each solid exists at all, and the stroke's own geometry inputs (its outline is the
    // second solid, so its width/cap/join/dashes are mesh inputs like the path is).
    const bool filled = !std::holds_alternative<NoPaint>(object.fill);
    hashPod(h, filled);
    const bool stroked =
        object.stroke.enabled && !std::holds_alternative<NoPaint>(object.stroke.paint);
    hashPod(h, stroked);
    if (stroked) {
        hashPod(h, object.stroke.width);
        hashPod(h, object.stroke.miterLimit);
        hashPod(h, object.stroke.dashOffset);
        hashPod(h, object.stroke.cap);
        hashPod(h, object.stroke.join);
        for (const double d : object.stroke.dashArray)
            hashPod(h, d);
    }
    // The extrude params the MESH depends on -- depth and the two bevels. Orientation, camera,
    // lighting and materials are deliberately absent: those are render-side uniforms, and mixing
    // them in here would re-tessellate the solid on every frame of an orbit drag.
    hashPod(h, params.depth);
    hashPod(h, params.bevelFront.profile);
    hashPod(h, params.bevelFront.size);
    hashPod(h, params.bevelFront.segments);
    hashPod(h, params.bevelBack.profile);
    hashPod(h, params.bevelBack.size);
    hashPod(h, params.bevelBack.segments);
    return h;
}

} // namespace mosaic::core::vec
