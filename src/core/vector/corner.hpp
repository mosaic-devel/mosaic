#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "common/geometry.hpp"
#include "core/vector/geometry.hpp"

// The corner engine's PUBLIC half (S26-c). flatten() already replaces every sharp vertex of a
// cornered primitive with a Round / Inverse / Bevel / None join (docs/vector-model.md §7.6); this
// header exposes the two things a consumer OUTSIDE the flattener needs from that same math:
//
//   1. the vertex ring a primitive is built from (`rectPolygon` and friends) -- flatten() itself
//      builds from these, so there is exactly ONE definition of "where a rect's corners are";
//   2. where a rounded corner actually LANDS (`cornerPointAt`) and the inverse mapping from a
//      dragged point back to a radius (`cornerRadiusForPoint`).
//
// The shape designer's on-diagram handles are the first consumer: a handle placed at the raw
// parameter box floats off a rounded outline (user report -- the corner handles "sit where the
// sharp corner would be"), so they are derived from the emitted corner instead. Deliberately
// FLTK-free and pure, so it unit-tests headlessly (tests/test_shape_library.cpp).
namespace mosaic::core::vec {

// A closed polygon plus the per-vertex rounding flatten() applies to it. The three arrays are
// parallel and always the same length.
struct CorneredPolygon {
    std::vector<Vec2> verts;
    std::vector<double> radii;
    std::vector<CornerStyle> styles;

    [[nodiscard]] std::size_t size() const { return verts.size(); }
    [[nodiscard]] bool empty() const { return verts.empty(); }
};

// The vertex rings behind the cornered primitives -- identical to what flatten() consumes.
[[nodiscard]] CorneredPolygon rectPolygon(const RectShape& r);
[[nodiscard]] CorneredPolygon polygonPolygon(const PolygonShape& p);
[[nodiscard]] CorneredPolygon starPolygon(const StarShape& s);
[[nodiscard]] CorneredPolygon crossPolygon(const CrossShape& x);
[[nodiscard]] CorneredPolygon bannerPolygon(const BannerShape& b);
// A callout's BODY ring (the tail is spliced into it afterwards). Empty for an elliptical body,
// which has no corners to round.
[[nodiscard]] CorneredPolygon calloutBodyPolygon(const CalloutShape& c);

// The ring for any parametric shape that has one; nullopt for the kinds built from curves or from
// a fixed vertex list with no rounding (ellipse, line, arrow, ring, heart, elliptical callout).
[[nodiscard]] std::optional<CorneredPolygon> corneredPolygonOf(const ParametricShape& s);

// One resolved corner. Everything here is in the shape's LOCAL space and matches what flatten()
// emits at that vertex, clamp included.
struct CornerPoint {
    Vec2 vertex;             // V, the sharp vertex (where an un-rounded corner sits)
    Vec2 axis;               // unit bisector V -> apex. Derived from the two edge directions, so a
                             // REFLEX (concave) vertex -- a cross's inner corner, a star's valley --
                             // gets the opposite sign for free; nothing keys off "inside the box".
    Vec2 apex;               // the emitted corner's midpoint: ON the rendered outline, for every
                             // style. Equals `vertex` when the corner stays sharp.
    Vec2 p0;                 // tangent point where the corner leaves the previous edge...
    Vec2 p1;                 // ...and rejoins the next one (both == vertex when sharp)
    double radius = 0.0;     // EFFECTIVE radius after the half-shorter-edge clamp
    double maxRadius = 0.0;  // largest radius this vertex can carry before that clamp bites
    bool rounded = false;    // false -> the corner degenerates to the sharp vertex
};

// The corner at `verts[index]`, rounded by `radius`/`style`. Mirrors flatten()'s emitCorner()
// exactly: the tangent inset t = radius / tan(alpha/2) clamped to half the shorter adjacent edge,
// and the apex is the midpoint of whatever that inset produces (a convex fillet, a concave scoop
// centred on the vertex, or a straight chamfer).
[[nodiscard]] CornerPoint cornerPointAt(const std::vector<Vec2>& verts, std::size_t index,
                                        double radius, CornerStyle style);
[[nodiscard]] CornerPoint cornerPointAt(const CorneredPolygon& poly, std::size_t index);

// The inverse: the radius whose apex lands nearest `p`, measured along the corner's bisector and
// clamped to [0, maxRadius]. This is what an on-diagram radius handle drags through -- because it
// saturates at exactly the radius flatten() saturates at, the handle stops dead instead of jumping
// when the clamp bites, and a drag can never push the parameter past what is drawable.
[[nodiscard]] double cornerRadiusForPoint(const std::vector<Vec2>& verts, std::size_t index,
                                          CornerStyle style, Vec2 p);

// The largest UNIFORM radius `poly` can carry -- the tightest vertex wins. A control whose range
// ends here spans exactly the useful travel (past it the outline stops changing).
[[nodiscard]] double maxCornerRadius(const CorneredPolygon& poly);

}  // namespace mosaic::core::vec
