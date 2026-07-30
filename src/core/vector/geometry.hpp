#pragma once

#include <array>
#include <cmath>
#include <variant>
#include <vector>

#include "common/geometry.hpp"

// Vector geometry -- the resolution-independent shape definitions behind Mosaic's vector
// layers (S25; design in docs/vector-model.md). This is deliberately the SVG model + a few
// modern extras, which keeps it export-friendly.
//
// Two ideas organize everything:
//   * Geometry  -- WHAT region a shape covers: an editable Path of cubic béziers, a
//                  ParametricShape (rect/ellipse/polygon/star/line) that keeps its defining
//                  parameters, or a BooleanCompound of live operands (S28; boolean.hpp).
//   * Paint     -- HOW the region is coloured (paint.hpp): solid or gradient. A "gradient
//                  layer" is just a full-bleed rect whose fill is a Gradient, not a kind.
//
// All coordinates are LAYER-LOCAL; the owning layer's transform() places the object in the
// document, exactly as RasterLayer::image() pixels are layer-local. Everything downstream
// (fill, stroke, hit-test, mask coverage, bounds, SVG export) consumes the single flattened
// form produced by flatten() (flatten.hpp) -- the one seam that keeps later features additive.
namespace mosaic::core::vec {

using common::Vec2;

// ---------------------------------------------------------------------------------------------
// Editable path (Pen tool, S28) -- the Illustrator/Inkscape node model
// ---------------------------------------------------------------------------------------------
// A node is an on-curve anchor with two control handles. The cubic segment between node i and
// node i+1 uses controls (node[i].outHandle, node[i+1].inHandle); when a handle coincides with
// its anchor that side is straight, so a polyline is just nodes whose handles equal the anchors.
// Handles are stored ABSOLUTE (not anchor-relative): simplest for flattening; the editor moves
// handles with the anchor. `type` is an editing hint only (S28) -- flatten() ignores it.
struct Node {
    Vec2 anchor;
    Vec2 inHandle;
    Vec2 outHandle;
    enum class Type { Corner, Smooth, Symmetric } type = Type::Corner;

    bool operator==(const Node&) const = default;
};

// One contour. `closed` joins the last node back to the first (a closing cubic); open subpaths
// are fillable too (implicitly closed for fill, left open for stroke -- caps instead of a join).
struct SubPath {
    std::vector<Node> nodes;
    bool closed = false;

    bool operator==(const SubPath&) const = default;
};

// How overlapping/nested subpaths combine when filled (a donut is one Path with two subpaths).
enum class FillRule { NonZero, EvenOdd };

struct Path {
    std::vector<SubPath> subpaths;
    FillRule fillRule = FillRule::NonZero;

    bool operator==(const Path&) const = default;
};

// ---------------------------------------------------------------------------------------------
// Parametric primitives (Shape tool, S26) -- each defined centred on the local origin so the
// layer transform carries position/rotation/scale. Editing a node "converts to path" (S26),
// promoting the shape to a Path; until then the parameters stay live and constrained.
// ---------------------------------------------------------------------------------------------
// How a rounded corner is shaped (S26-b shape-designer; docs/vector-model.md §7.6). All four
// share the same tangent points on the two edges (an inset of the rounding radius from the
// vertex); they differ only in what joins those tangent points:
//   Round   -- a convex fillet arc tangent to both edges (the normal rounded corner).
//   Inverse -- a concave scoop: an arc centred ON the vertex, biting inward (radius == the inset).
//   Bevel   -- a straight chamfer between the tangent points.
//   None    -- the sharp vertex (the radius is ignored).
enum class CornerStyle { Round, Inverse, Bevel, None };

struct RectShape {
    Vec2 size;                                                  // full width/height
    // Per-corner rounding, indexed TL, TR, BR, BL (each clamped to half the adjacent edges on
    // flatten). A uniform rect is the common case -- use RectShape::uniform().
    std::array<double, 4> cornerRadius{0, 0, 0, 0};
    std::array<CornerStyle, 4> cornerStyle{CornerStyle::Round, CornerStyle::Round,
                                           CornerStyle::Round, CornerStyle::Round};
    static RectShape uniform(Vec2 s, double r, CornerStyle st = CornerStyle::Round) {
        return RectShape{s, {r, r, r, r}, {st, st, st, st}};
    }
    bool operator==(const RectShape&) const = default;
};
struct EllipseShape {
    Vec2 radii;                                                 // rx, ry
    // A partial sweep (endAngle - startAngle < 2*pi) is an arc; arcMode says how it closes for
    // fill. A full sweep is an ordinary ellipse regardless of arcMode. Angles in radians, y-down.
    double startAngle = 0.0;
    double endAngle = 2.0 * M_PI;
    enum class ArcMode { Open, Chord, Pie } arcMode = ArcMode::Open;
    bool operator==(const EllipseShape&) const = default;
};
struct PolygonShape {
    int sides = 5;
    double radius = 1;
    double cornerRadius = 0;                                    // uniform corner rounding (0 = sharp)
    CornerStyle cornerStyle = CornerStyle::Round;               // Round / Bevel / None (UI subset)
    bool operator==(const PolygonShape&) const = default;
};
struct StarShape {
    int points = 5;
    double outerRadius = 1;
    double innerRadius = 0.5;
    double pointRadius = 0;                                     // rounding at the outer tips
    double valleyRadius = 0;                                    // rounding at the inner valleys
    bool operator==(const StarShape&) const = default;
};
struct LineShape {
    Vec2 a, b;                 // the centreline; the Object's Stroke gives its weight / cap / colour
    // Paint mode (S26-b §7.5), reinterpreted for a 1-D primitive: Solid = the weight-thick line
    // filled (the Object's Stroke, exactly as S26-a shipped); Hollow = only a thin border at the
    // thick line's edge, empty inside; Outlined = the filled line PLUS a contrasting border around
    // it. The line colour is the Object's STROKE paint (fg); the Outlined border colour is the
    // Object's FILL paint (bg) -- a line has no interior, so the fill slot carries the border colour.
    enum class Paint { Solid, Hollow, Outlined } paint = Paint::Solid;
    double borderWidth = 1.0;  // the thin contrasting edge (Hollow / Outlined only)
    // Bend: the offset of the curve's MIDPOINT from the straight a-b midpoint (S26, the line
    // gizmo's round handle). Zero = a straight segment; non-zero bows it into a quadratic curve
    // passing through (midpoint + bend). flatten() emits a polyline either way.
    Vec2 bend{0, 0};
    bool operator==(const LineShape&) const = default;
};

// ---------------------------------------------------------------------------------------------
// The widened shape library (S26-c). Every one of these keeps the S25 discipline: its real size
// lives in its own parameters, centred on the local origin, so the layer transform stays a rigid
// placement and a parametric resize only scales parameters. They are FILLED primitives -- an
// outline is the layer-effects Stroke's job, so none of them repeats LineShape's paint-mode
// special case; where a shape needs a proportion it stores a RATIO of its size, so a resize keeps
// the look and only absolute distances (corner radii, a callout's tail) need scaling.
// ---------------------------------------------------------------------------------------------

// Speech bubble / callout: a body (a rounded rect or an ellipse) plus a tail leaving the body's
// perimeter in the direction `tailAngle` and reaching `tailLength` past it. Two tail kinds:
//   Pointer -- a solid triangle SPLICED INTO the body outline, so the whole balloon is ONE closed
//              contour and a stroke traces it without a seam across the body edge.
//   Bubbles -- the thought-balloon trail: the body plus a row of shrinking discs (genuinely
//              separate contours, which is what a thought balloon is).
struct CalloutShape {
    Vec2 size;                                     // full body width/height
    enum class Body { RoundedRect, Ellipse } body = Body::RoundedRect;
    double cornerRadius = 0.0;                     // RoundedRect body only (clamped on flatten)
    enum class Tail { Pointer, Bubbles } tail = Tail::Pointer;
    // Direction from the body CENTRE the tail leaves in: radians, y-down, 0 = +x (to the right),
    // +pi/2 = straight down. The default aims it down-left, the comic-book default.
    double tailAngle = 2.36;
    double tailLength = 0.0;                       // reach past the body edge (0 = no tail at all)
    double tailWidth = 0.0;                        // Pointer: base width where it meets the body
    // -1..1: slides the TIP sideways along the body's tangent, hooking the pointer without moving
    // where it leaves the body (the hand-drawn look). Ignored by Bubbles.
    double tailSkew = 0.0;
    int bubbleCount = 3;                           // Bubbles: how many puffs trail the body
    bool operator==(const CalloutShape&) const = default;
};

// An arrow, drawn along the local +x axis (rotation is the layer transform's job, exactly as for
// every other primitive). `size` is the full extent: x = tip-to-tail length, y = head width -- so
// the drag box and a bbox resize map onto it directly, and the two proportions are ratios.
struct ArrowShape {
    Vec2 size;                  // full length (x) x head width (y)
    double shaftRatio = 0.38;   // shaft thickness / head width, in (0,1]
    double headRatio = 0.34;    // head length / total length, in (0,1)
    double notchRatio = 0.0;    // 0..1: sweeps the head's back edge into a barb (0 = flat)
    bool doubleHeaded = false;  // a head on BOTH ends (the dimension arrow)
    bool operator==(const ArrowShape&) const = default;
};

// A ring / donut / pie wedge: the region between two concentric ellipses over an angular sweep.
// innerRatio 0 collapses the hole, so a partial sweep with innerRatio 0 IS a pie slice, and a full
// sweep with innerRatio 0 is a plain disc. Angles are radians, y-down (EllipseShape's convention).
struct RingShape {
    Vec2 radii;                  // outer rx, ry
    double innerRatio = 0.55;    // inner radii / outer, in [0,1)
    double startAngle = 0.0;
    double endAngle = 2.0 * M_PI;
    bool operator==(const RingShape&) const = default;
};

// A cross / plus: two bars crossing at the centre, filling `size`. The arm thickness is a fraction
// of the SHORTER side, so the figure stays a cross under any resize.
struct CrossShape {
    Vec2 size;                                     // full width/height
    double armRatio = 0.34;                        // arm thickness / min(width, height), in (0,1]
    double cornerRadius = 0.0;                     // uniform rounding at all twelve vertices
    CornerStyle cornerStyle = CornerStyle::Round;
    bool operator==(const CrossShape&) const = default;
};

// A heart, tight in its box: two cubic lobes meeting at a top cleft and running to a bottom tip.
// `lobe` fattens/raises the shoulders, `cleft` deepens the notch between the lobes.
struct HeartShape {
    Vec2 size;              // full width/height
    double lobe = 0.5;      // 0..1: how low and full the two shoulders sit
    double cleft = 0.235;   // 0..0.6: depth of the top notch as a fraction of the height
    bool operator==(const HeartShape&) const = default;
};

// The chevron / banner family: a rectangle whose RIGHT edge is pushed out into a point (Chevron)
// or cut in as a swallow-tail (Banner), optionally with the matching notch cut into the left edge.
// Chevron + notchTail is the breadcrumb arrow; Banner + notchTail the classic ribbon.
struct BannerShape {
    Vec2 size;                                     // full width/height
    enum class Style { Chevron, Banner } style = Style::Chevron;
    double pointRatio = 0.22;                      // point / notch depth as a fraction of the width
    bool notchTail = true;                         // cut the matching notch into the LEFT edge
    double cornerRadius = 0.0;                     // uniform rounding at every vertex
    bool operator==(const BannerShape&) const = default;
};

// NB: new alternatives are APPENDED, so every existing variant index (and anything that persisted
// one) is unchanged; the .mosaic writer keys on a string token anyway (io/mosaic/docjson).
using ParametricShape =
    std::variant<RectShape, EllipseShape, PolygonShape, StarShape, LineShape, CalloutShape,
                 ArrowShape, RingShape, CrossShape, HeartShape, BannerShape>;

// ---------------------------------------------------------------------------------------------
// Live boolean compound (S28) -- the reserved case, now built (docs/vector-model.md §9)
// ---------------------------------------------------------------------------------------------
// Add / Subtract / Intersect / Exclude. The vocabulary matches the Illustrator/Affinity Pathfinder
// set the menu exposes; "Add" is spelled Union here because that is what the kernel computes.
enum class BoolOp { Union, Subtract, Intersect, Exclude };

// `Object` (object.hpp) is INCOMPLETE at this point: Geometry is recursive through it, which is
// the whole point -- a compound's operands are ordinary objects (own paint, own geometry, possibly
// compounds themselves) that stay live and editable inside one object on one layer. std::vector of
// an incomplete type is well-formed (C++17 [container.requirements.general]); Object is complete
// long before any vector member is used.
struct Object;

// The operands, rebased into the HOST's local frame at build time (vec::makeBooleanObject does
// that), so a compound needs no per-child transform: every child's coordinates are already in the
// same layer-local space as the compound itself. flatten() resolves the op (boolean.hpp).
struct BooleanCompound {
    BoolOp op = BoolOp::Union;
    std::vector<Object> children;

    // Written out rather than `= default`: a defaulted comparison would have to be analysed
    // against `std::vector<Object>` with Object still incomplete here. Defined in boolean.cpp,
    // where object.hpp has made Object complete. (C++20 rewrites `!=` from this.)
    bool operator==(const BooleanCompound& other) const;
};

// The geometry of one vector object. CLOSED variant by design; new alternatives are APPENDED so
// every existing variant index stays put (the .mosaic writer keys on a string token anyway).
using Geometry = std::variant<Path, ParametricShape, BooleanCompound>;

// ---------------------------------------------------------------------------------------------
// The flattened form everything downstream consumes (produced by flatten(); see flatten.hpp)
// ---------------------------------------------------------------------------------------------
struct Contour {
    std::vector<Vec2> points;
    bool closed = false;

    // Value-comparable so aggregates carrying baked contours (TextBlock::PathFit) keep their
    // defaulted operator== (S30 fit-to-path).
    friend bool operator==(const Contour&, const Contour&) = default;
};
using Contours = std::vector<Contour>;

}  // namespace mosaic::core::vec
