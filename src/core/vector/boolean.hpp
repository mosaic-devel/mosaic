#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "common/geometry.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/object.hpp"

// Polygon booleans (S28) -- Add / Subtract / Intersect / Exclude, computed on the FLATTENED form.
// Design in docs/vector-model.md §9. Because flatten() is the one seam every consumer routes
// through, a boolean never has to clip Béziers: it clips polylines, and fill, stroke, hit-test,
// bounds, thumbnails, text-on-path and the rasterizer all become correct with no curve fitting.
//
// Technique lineage (the §6 source-header convention), all long-published:
//   - Region boolean by WINDING-NUMBER EDGE CLASSIFICATION: build the arrangement of both
//     operands' edges, keep exactly the edge fragments whose two sides disagree under the op's
//     predicate. Vatti, "A generic solution to polygon clipping", CACM 35(7):56-63, 1992. (We
//     evaluate the classification over an explicit arrangement rather than through Vatti's
//     active-edge sweep; the formulation and the winding bookkeeping are his.)
//   - Segment-intersection reporting, and the x-order pruning of the pair sweep: Bentley &
//     Ottmann, "Algorithms for reporting and counting geometric intersections", IEEE Trans.
//     Computers C-28(9):643-647, 1979.
//   - Point classification: the winding-number form of the classic Jordan-curve ray test
//     (Sunday) -- already this repo's rule, see vector/hit.cpp.
//   - Degeneracy: SNAP ROUNDING onto a fixed integer lattice, so coincident and touching edges
//     become EXACTLY coincident, and a computed crossing becomes a lattice VERTEX that both
//     chains are re-routed through -- it does not stay on either line it came from, and must not
//     be asked to. Greene & Yao, "Finite-resolution computational geometry", FOCS 1986; Hobby,
//     "Practical segment intersection with finite precision output", Computational Geometry
//     13(4):199-214, 1999.
//   - Contour classification into outers + holes (containment depth, then forced orientation):
//     the standard even-odd nesting test; the same one core/text/extrude_mesh.cpp uses.
//
// THE PREDICATE. Snap rounding is what buys exactness here: once every coordinate is a bounded
// lattice integer, orient2d is a 2x2 integer determinant that int64 evaluates EXACTLY -- no
// adaptive expansion, no epsilon, no filter. That is why this file has an integer predicate and
// not a floating-point one. Shewchuk, "Adaptive precision floating-point arithmetic and fast
// robust geometric predicates", Discrete & Computational Geometry 18(3):305-363, 1997, is the
// fallback lineage if the lattice is ever dropped for a full-precision kernel.
//
// Designed from those dated publications, and that lineage is a constraint rather than an
// accident: this kernel stays winding-number edge classification over an explicit snap-rounded
// arrangement. It is not modelled on any other clipping method and must not be re-based onto one.
namespace mosaic::core::vec {

// ---------------------------------------------------------------------------------------------
// The kernel
// ---------------------------------------------------------------------------------------------
// `BoolOp` lives in geometry.hpp (with BooleanCompound) because the MODEL needs it and this
// header depends on the model, not the other way round.
//
// Contract, in one paragraph. Operands are interpreted as CLOSED regions: an open contour is
// treated as implicitly closed for the area test, which is what every other area consumer in the
// tree already does (raster.cpp's scanline, hit.cpp's contains(), SVG's fill rule) -- an operand is
// never silently dropped for being open. Each operand is read under the NonZero rule unless a
// per-operand rule is supplied. Input may self-intersect, may nest holes inside holes, may share
// vertices or whole edges with another operand, and may be disjoint from or entirely inside
// another; all of those are handled, and a result with no area is an EMPTY Contours, never a
// crash. The OUTPUT is always normalized: closed rings only, outers wound positive (visually
// clockwise in this y-down space) and holes negative, unambiguously correct under **NonZero** --
// downstream code never has to negotiate a fill rule with a boolean result. Rings come back in a
// canonical order, each starting at its lexicographically smallest vertex, so two runs on the same
// input are byte-identical.
[[nodiscard]] Contours booleanContours(BoolOp op, const Contours& a, const Contours& b);

// The n-ary fold. `operands.front()` is the HOST: Subtract removes every later operand from it,
// Exclude is the odd-coverage parity, Union/Intersect are order-independent. Zero operands give
// an empty result; ONE operand is well defined and useful -- it is that shape renormalized (see
// normalizedContours).
[[nodiscard]] Contours booleanContours(BoolOp op, const std::vector<Contours>& operands);

// As above but with each operand's own fill rule (sizes must match, else the extras/missing
// default to NonZero). flatten()'s compound arm uses this so an EvenOdd child is read as EvenOdd.
[[nodiscard]] Contours booleanContours(BoolOp op, const std::vector<Contours>& operands,
                                       const std::vector<FillRule>& rules);

// `cs` read under `rule`, re-emitted as NonZero-normalized rings. The one-operand case of the
// kernel; the way to hand EvenOdd geometry to something that only speaks NonZero.
[[nodiscard]] Contours normalizedContours(const Contours& cs, FillRule rule);

// ---------------------------------------------------------------------------------------------
// The model side -- what flatten(), Convert-to-Path and the .mosaic writer call
// ---------------------------------------------------------------------------------------------
// Resolve a compound to contours: flatten every child at the SAME tolerance and device transform
// the caller passed (so curve smoothness still tracks zoom through a boolean), then run the op.
// Nested compounds recurse. This is what flatten()'s BooleanCompound arm calls.
[[nodiscard]] Contours flattenCompound(const BooleanCompound& compound, double tolerancePx = 0.25,
                                       const common::Affine2D& toDevice =
                                           common::Affine2D::identity());

// The compound BAKED to an editable path: the resolved region as polyline subpaths (nodes whose
// handles equal their anchors), NonZero, closed. The precedent CalloutShape already set
// (docs/vector-model.md §7.7) -- a converted shape may be a polyline when the outline is computed
// rather than parametric. Backs Convert-to-Path, the .mosaic forward-compatibility fallback and --
// since it is what the menu COMMITS -- Layer > Combine Paths (makeBooleanObject, below).
//
// The result is well formed FOR EDITING, which is a stronger contract than "draws correctly":
// every subpath is closed and has >= 3 nodes, no two consecutive anchors coincide (the wrap-around
// pair included), and every Node::type is Corner -- a polyline vertex is a corner, and an editor
// that showed two draggable nodes at one point, or a smooth hint on a zero-length segment, would
// be showing a defect. Holes keep the negative winding the NonZero normalization gave them.
[[nodiscard]] Path bakedBooleanPath(const BooleanCompound& compound, double tolerancePx = 0.25);

// `obj` with its geometry mapped through `t`, keeping its paint/stroke/paint-order. A Path or a
// ParametricShape is promoted to a Path and transformed EXACTLY (cubics are affine-invariant --
// vec::transformedPath); a compound child recurses, so nesting stays LIVE rather than baking.
// The identity transform is a pure copy.
[[nodiscard]] Object rebasedObject(const Object& obj, const common::Affine2D& t);

// The FOLD, as a live compound. `operandsInWorld` pairs each object with ITS layer's world
// transform; `hostWorld` is the world transform of the layer the result will live on (normally the
// first operand's). Every child is rebased into the host's local frame, so the returned object is
// self-contained and its layer transform stays a rigid placement. The result takes the FIRST
// operand's fill/stroke/paint-order -- the host's appearance wins, which is the Illustrator/Figma
// rule. nullopt when there are fewer than two operands or `hostWorld` is singular.
//
// This is the step makeBooleanObject bakes, and it is the entry point a future NON-DESTRUCTIVE
// "live boolean" mode wants -- the compound model, its flatten() arm, its serialization side-car
// and its tests all stay in place for it. No menu command commits its result today: Combine Paths
// commits a baked path (see below, and docs/vector-model.md §9.2).
[[nodiscard]] std::optional<Object> makeLiveBooleanObject(
    BoolOp op, const std::vector<std::pair<Object, common::Affine2D>>& operandsInWorld,
    const common::Affine2D& hostWorld);

// What `Layer > Combine Paths` commits: the same fold, BAKED -- the returned Object's geometry is
// an ordinary `vec::Path` (multi-subpath, closed, NonZero), never a BooleanCompound. That is the
// whole point of this entry point rather than the live one: a committed boolean IS a path, so the
// Pen tool binds it (`std::holds_alternative<vec::Path>`), the Layers panel badges it as a path,
// the .mosaic writer stores it with no side-car, and every node is draggable. The price is the
// documented one -- the outline is a polyline at the baking tolerance, because the kernel resolves
// flattened contours; there is no Bézier refitting and there is not meant to be.
//
// The tolerance is not the fixed 0.25 the live arm re-flattens at every frame, because a bake is
// permanent: it is derived from `hostWorld`'s scale and the operands' own extent, so a shape on a
// scaled-up layer, or one much larger than the canvas, does not come back visibly faceted. See
// boolean.cpp's bakeToleranceFor.
//
// nullopt for makeLiveBooleanObject's two refusals, and additionally when the resolved region is
// EMPTY (Subtract by something that covers the host, Exclude of two identical shapes): committing
// nothing would delete every consumed operand layer and leave the host holding an invisible,
// unpickable path. The caller's "these shapes cannot be combined" status is the right answer.
[[nodiscard]] std::optional<Object> makeBooleanObject(
    BoolOp op, const std::vector<std::pair<Object, common::Affine2D>>& operandsInWorld,
    const common::Affine2D& hostWorld);

}  // namespace mosaic::core::vec
