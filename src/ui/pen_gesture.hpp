#pragma once

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/object.hpp"
#include "ui/shape_gesture.hpp" // ShapeDraft -- a finished path lands exactly like a shape does

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Pen / custom-path tool math (S28) -- the pure, FLTK-free core behind Bézier authoring, the
// node/handle editor and the paint the finished path carries. Kept free of FLTK and the canvas so
// it can be unit-tested headlessly (exactly like shape_gesture / gradient_gesture); the
// VulkanCanvas gesture and the app_window wiring consume it.
//
// ⚠ COORDINATE DISCIPLINE. Nothing in this header knows about the screen. The AUTHORING state
// machine works in DOCUMENT space; the EDITING helpers work in LAYER-LOCAL space (the space
// core::vec::Path stores, per docs/vector-model.md §2). The canvas converts once, at the event,
// through VulkanCanvas::eventDocPoint() -- which is only honest inside the canvas widget's own
// handle() call -- and never hands this file a widget-relative or window-relative pixel. That is
// the whole of the "events must be rooted in the canvas, not the app" rule: if a press were
// intercepted at the window level and passed down, every point below would be off by the canvas's
// origin inside the top-level (the menu bar + options bar above it, the tool rail to its left) and
// the bug would be invisible until you noticed the path landing a couple of centimetres away.
//
// Model note (docs/vector-model.md §2.2): a Node carries an ABSOLUTE anchor plus two absolute
// handles; the cubic from node i to node i+1 uses (node[i].outHandle, node[i+1].inHandle). A handle
// sitting exactly on its anchor means "straight on that side", so a polyline is a path whose
// handles all equal their anchors. `Node::type` is an editing hint only -- flatten() ignores it.
namespace mosaic::ui {

// A finished pen path becomes a VectorLayer through the SAME draft the Shape tool uses (geometry
// centred on the layer's local origin + a rigid placement translation), so it rides the existing
// spawn/commit path in app_window and inherits its one-undo-step guarantee for free.
using PenDraft = ShapeDraft;

// A drag whose whole extent is under this many document px authors nothing (a click, or a jitter).
inline constexpr double kPenMinExtentDoc = 0.5;

// The Shift constraint's angular grid for a handle pull / the rubber-band segment: 45 degrees, the
// same step the Shape tool's Shift-constrained line uses.
inline constexpr double kPenAngleStep = 0.7853981633974483; // pi / 4

// ---- Options (the tool bar's snapshot) --------------------------------------------------------

// A flat snapshot of the Pen tool's options, read off the options bar by the canvas so the maths
// takes plain values and never touches the ToolManager (the ShapeOptions convention).
//
// Unlike the Shape tool (S26-c retired its stroke controls in favour of the layer-effects Stroke),
// the Pen KEEPS a real stroke: an open path has no interior at all, so -- exactly like the Line
// shape -- a stroke is the only way most pen paths can exist. That is also what PLAN S28's "custom
// stroke" names, and the params are S27's set: width / cap / join / dash.
struct PenOptions {
    common::ColorF foreground{0.0f, 0.0f, 0.0f, 1.0f}; // the fill (a stroke-only path's stroke)
    common::ColorF background{1.0f, 1.0f, 1.0f, 1.0f}; // the outline when a fill is also present
    bool fill = true;                                  // fill the path with the foreground colour
    bool strokeEnabled = true;
    double strokeWidth = 2.0;
    core::vec::LineCap cap = core::vec::LineCap::Round;
    core::vec::LineJoin join = core::vec::LineJoin::Round;
    int dashStyle = 0; // 0 Solid / 1 Dashed / 2 Dotted / 3 Dash-dot (see penDashArray)
};

// The dash pattern a style index produces, scaled by the stroke width so it reads at any weight.
// The SAME four patterns the shape designer's outline section produces (its dashPatternFor at
// spacing 1.0) -- a dashed pen path and a dashed shape outline must not look like different
// features. Empty for Solid.
[[nodiscard]] std::vector<double> penDashArray(int style, double width);

// Reflect an existing dash array back to a style index (the select-to-edit direction).
[[nodiscard]] int penDashStyleOf(const std::vector<double>& dashArray);

// ---- Node identity, hit-testing and the pure edit operations -----------------------------------

// Which node of which subpath -- the editor's selection, and the address every edit takes.
struct PenSelection {
    bool valid = false;
    std::size_t subpath = 0;
    std::size_t node = 0;

    friend bool operator==(const PenSelection&, const PenSelection&) = default;
};

// What a pick landed on. `point` is the element's position (the anchor / the handle tip / the point
// ON the segment), so the caller can start a drag from it without re-deriving anything.
struct PenHit {
    enum class Kind { None, Anchor, InHandle, OutHandle, Segment };
    Kind kind = Kind::None;
    std::size_t subpath = 0;
    std::size_t node = 0; // Segment: the segment LEAVING this node
    double t = 0.0;       // Segment only: the cubic parameter of the nearest point, in [0,1]
    common::Vec2 point{};

    [[nodiscard]] bool hit() const noexcept { return kind != Kind::None; }
    [[nodiscard]] PenSelection selection() const {
        return PenSelection{kind != Kind::None, subpath, node};
    }
};

// Pick the path element within `tolLocal` of `pLocal` (BOTH in layer-local units -- the caller
// converts its screen-pixel tolerance through the view zoom AND the layer's world transform, so the
// grab stays the same size on screen at every zoom; that conversion is the same class of bug as the
// coordinate rule at the top of this header).
//
// Priority: the handles of `handlesOf` first (they are drawn on top, and a handle pulled only a
// little way out otherwise sits inside its own anchor's pick disc and could never be taken), then
// any anchor, then ANY node's handles, then the segments.
//
// ⚠ That third tier exists because the chrome now draws EVERY node's handles (it has its own
// overlay lane rather than 40 borrowed entries of the 64-entry guide lane), and what you can grab
// must be exactly what you can see. It sits BELOW anchors on purpose: an anchor is the primary
// element, so a handle parked on top of a neighbouring anchor never steals that anchor's grab.
[[nodiscard]] PenHit penHitTest(const core::vec::Path& path, common::Vec2 pLocal, double tolLocal,
                                PenSelection handlesOf);

// The four control points of the cubic leaving node `i` of `sp` (p0, c0, c1, p1), or nullopt when
// that node ends an open subpath. The closing segment of a closed subpath is node n-1 -> node 0.
[[nodiscard]] std::optional<std::array<common::Vec2, 4>> penSegmentCubic(const core::vec::SubPath& sp,
                                                                         std::size_t i);

// A cubic evaluated at `t` (de Casteljau).
[[nodiscard]] common::Vec2 penCubicAt(const std::array<common::Vec2, 4>& c, double t);

// Move the anchor of `n` by `deltaLocal`, CARRYING ITS HANDLES with it (moving a node must not
// reshape the curve on either side -- only where it passes). Returns `base` unchanged for an
// invalid address.
[[nodiscard]] core::vec::Path penMoveAnchor(const core::vec::Path& base, PenSelection n,
                                            common::Vec2 deltaLocal);

// Move one handle of `n` to `toLocal`. `outSide` picks which. Unless `breakPair`, the OPPOSITE
// handle is swung to stay collinear through the anchor -- keeping its own LENGTH for a Smooth node
// and mirroring exactly for a Symmetric one, which is the difference between the two types.
// `breakPair` (Alt) leaves the opposite handle alone and demotes the node to Corner: a cusp.
[[nodiscard]] core::vec::Path penMoveHandle(const core::vec::Path& base, PenSelection n, bool outSide,
                                            common::Vec2 toLocal, bool breakPair);

// Insert a node on the segment leaving `seg` at parameter `t`, by de Casteljau SPLIT -- so the
// drawn curve is bit-for-bit the curve it was before the insertion, which is the whole point of
// adding a node on a segment rather than near one. The new node is Smooth. `outSelection` (when
// given) receives the address of the inserted node.
[[nodiscard]] core::vec::Path penInsertNode(const core::vec::Path& base, PenSelection seg, double t,
                                            PenSelection* outSelection = nullptr);

// Delete node `n`. Its neighbours keep their own handles (the curve through the gap re-joins them
// directly). A subpath left with no nodes is dropped; an OPEN subpath left with one node is dropped
// too (a single anchor draws and fills nothing).
[[nodiscard]] core::vec::Path penDeleteNode(const core::vec::Path& base, PenSelection n);

// Toggle node `n` between a cusp and a smooth node -- the Alt-click-an-anchor gesture. A node with
// any handle off its anchor collapses BOTH handles onto it (Corner); a node with none pulls a
// symmetric pair out along the chord between its neighbours, at a third of the shorter neighbour
// distance (the classic "smooth this corner" construction).
[[nodiscard]] core::vec::Path penToggleNodeType(const core::vec::Path& base, PenSelection n);

// Snap the direction from `from` to `to` onto the nearest multiple of `stepRad`, keeping the
// distance -- the Shift constraint, shared by the rubber-band segment and the handle pull.
[[nodiscard]] common::Vec2 penConstrainAngle(common::Vec2 from, common::Vec2 to,
                                             double stepRad = kPenAngleStep);

// ---- The authoring state machine ---------------------------------------------------------------

// Click = a corner node; click-and-drag = a smooth node whose symmetric handles are pulled out
// live; releasing continues the path; clicking the first node closes it. Everything is DOCUMENT
// space. The gesture never touches the document: the canvas draws the in-flight path on the
// overlay's polyline lane (exactly as the S26-c shape wireframe does) and only on finish does the
// host spawn the real VectorLayer -- one undo step for the whole authoring session.
//
// ⚠ AUTHORING IS ONE SUBPATH, and that is the tool's stated limit rather than an oversight: path()
// and pathWithRubberBand() always answer with a single-contour Path, liveNode() always addresses
// subpath 0, and penCloseTarget rings the first node of the first subpath. EDITING a committed path
// is a different matter and is fully multi-subpath (penHitTest / penMoveAnchor / penMoveHandle /
// penInsertNode / penDeleteNode / penChromeMarks all address (subpath, node) and walk every
// contour), which is what lets the Pen bind the baked multi-subpath Path that Layer ▸ Combine Paths
// commits. Drawing a SECOND contour into an already-bound path is the gesture that does not exist.
class PenGesture {
public:
    [[nodiscard]] bool active() const noexcept { return m_active; }
    [[nodiscard]] bool closed() const noexcept { return m_closed; }
    [[nodiscard]] bool draggingHandle() const noexcept { return m_dragging; }
    [[nodiscard]] std::size_t nodeCount() const noexcept { return m_nodes.size(); }
    [[nodiscard]] const std::vector<core::vec::Node>& nodes() const noexcept { return m_nodes; }

    // FL_PUSH. Returns TRUE when the press CLOSED the path -- the caller then finishes the gesture
    // on the following release (the closing drag may still shape the first node's handles).
    // `closeRadiusDoc` is the screen-pixel close tolerance already divided by the zoom.
    bool press(common::Vec2 doc, double closeRadiusDoc, bool shift);

    // FL_DRAG with the button still down: pull the just-placed node's handles. Symmetric by
    // default; `alt` breaks the pair (a cusp); `shift` snaps the handle direction to 45 degrees.
    void dragHandle(common::Vec2 doc, bool shift, bool alt);

    // FL_RELEASE: the node stands as dragged and authoring continues.
    void release() noexcept { m_dragging = false; }

    // FL_MOVE: where the rubber-band segment reaches. `shift` constrains it off the last anchor.
    void moveTo(common::Vec2 doc, bool shift);
    void clearHover() noexcept { m_hasHover = false; }

    // Backspace: drop the last placed node. False when there was nothing left to drop.
    bool backspace();

    // Drop everything (a commit that authored nothing, a new document, a cancelled session).
    void reset() noexcept;

    // The authored path. EMPTY (no subpaths) until there are two nodes -- one anchor is not a path.
    [[nodiscard]] core::vec::Path path() const;

    // The path as it should be DRAWN right now: the placed nodes plus, on an open path with the
    // pointer hovering and no drag in flight, a provisional node at the pointer -- so the
    // rubber-band segment shows the real cubic the next click would commit, not a straight chord.
    [[nodiscard]] core::vec::Path pathWithRubberBand() const;

    // The node the pointer is currently shaping (the one whose handles the overlay draws), or an
    // invalid selection when nothing is live.
    [[nodiscard]] PenSelection liveNode() const;

private:
    std::vector<core::vec::Node> m_nodes; // document space
    bool m_active = false;
    bool m_closed = false;
    bool m_dragging = false;
    std::size_t m_dragIndex = 0;
    bool m_hasHover = false;
    common::Vec2 m_hover{0.0, 0.0};
};

// ---- Landing the path, and the paint it carries -------------------------------------------------

// The layer a finished `docPath` (DOCUMENT space) becomes: the geometry re-centred on the local
// origin and the offset carried by a rigid placement translation (the S25/S26 rule -- so Move
// rotates and scales the layer without distorting the path), painted from `opts`. nullopt for a
// path with under two nodes or no measurable extent.
[[nodiscard]] std::optional<PenDraft> buildPenDraft(const core::vec::Path& docPath,
                                                    const PenOptions& opts);

// Apply `opts`'s paint onto a COPY of `base`, geometry untouched: the fill, and the stroke's
// width / cap / join / dash. Both the freshly authored path (through buildPenDraft) and a live
// options-bar edit of a bound path go through here, so the two can never drift.
//
// When BOTH fill and stroke are switched off the STROKE is forced back on: a path that paints
// nothing is not a state the tool can leave you in (you would be editing an invisible layer).
[[nodiscard]] core::vec::Object penPaintedObject(const core::vec::Object& base,
                                                 const PenOptions& opts);

// Read `obj`'s pen-editable paint parameters into `io` (the select-to-edit direction: a clicked
// path loads its own settings into the bar). Colours are NOT read -- they stay on `io`, exactly as
// readShapeOptions leaves them, because the colour swatch owns them.
void readPenOptions(const core::vec::Object& obj, PenOptions& io);

// Recolour a bound path from the swatch: the fill takes `fg`; the outline takes `bg` when there is
// also a fill and `fg` when the stroke is the only paint. The identical convention (and reasoning)
// as ui::recoloredObject -- the fill is the primary element, the outline the secondary accent.
[[nodiscard]] core::vec::Object penRecoloredObject(const core::vec::Object& base, common::ColorF fg,
                                                   common::ColorF bg);

// Is this object one the PEN binds for editing? True exactly for a Path geometry -- a parametric
// shape belongs to the Shape tool's bar (which can express its parameters) and a gradient to the
// Gradient tool's, the same three-way exclusion gradientToolBinds / shapeToolBinds already draw.
[[nodiscard]] bool penToolBinds(const core::vec::Object& obj);

// ---- Overlay geometry ---------------------------------------------------------------------------

// The overlay's polyline lane draws ONE open polyline (segments between consecutive vertices), so a
// path of several contours needs an explicit break between them -- otherwise one contour's end is
// strung to the next one's start by a chord that is not in the path at all. A vertex whose x is at
// or below kPolylineBreakX is that break: the run ends, and the next real vertex opens a fresh one.
// The same out-of-range-sentinel idiom the present pass already uses for the Move anchor and the
// Type bend tab; kPolylineBreak is the value writers emit, kPolylineBreakX the threshold readers
// test. Must match render::kPolylineBreakX / kPolylineBreakValue (the lane's own copy -- ui cannot
// pull in the Vulkan headers for a constant) and canvas_present.comp's kPolyBreak, which is what
// makes lassoDist skip the chord.
inline constexpr double kPolylineBreakX = -1.0e8;
inline constexpr common::Vec2 kPolylineBreak{-1.0e9, -1.0e9};
[[nodiscard]] inline bool isPolylineBreak(common::Vec2 p) noexcept {
    return p.x <= kPolylineBreakX;
}

// `path` flattened for the overlay's single polyline lane. `toDevice` maps the path's own space to
// PHYSICAL screen px and is used ONLY to pick the flattening tolerance, so the preview's smoothness
// tracks zoom + HiDPI exactly like the rasteriser's does (shapeOutlinePolyline verbatim). A closed
// contour repeats its first point, and EVERY contour is emitted, separated by kPolylineBreak -- a
// boolean result or a two-subpath pen path must preview as the shape it is, not as its first
// contour. (ui::shapeOutlinePolyline instead keeps only the LARGEST contour; that is the Shape
// tool's silhouette rule, not a shared one, and the two are deliberately different.)
[[nodiscard]] std::vector<common::Vec2> penPathPolyline(
    const core::vec::Path& path, const common::Affine2D& toDevice = common::Affine2D::identity());

// One knob of the node/handle chrome, in whatever space the caller works in (the canvas passes
// LOGICAL SCREEN px, having mapped the path through the layer's world transform and the view). The
// renderer's PenMark is this struct flattened for the SSBO; the shader draws `kind` as a shape and
// `selected`/`hovered` as a fill/border colour (canvas_present.comp, penChrome).
struct PenChromeMark {
    enum class Kind : std::uint8_t {
        AnchorCusp = 0,   // a Corner node: a SQUARE knob -- each side steers independently
        AnchorSmooth = 1, // a Smooth / Symmetric node: a ROUND knob
        HandleTip = 2,    // a handle's far end: a smaller round knob
    };
    common::Vec2 pos{0.0, 0.0};
    Kind kind = Kind::AnchorCusp;
    bool selected = false;
    bool hovered = false;
};

// One stem: the hairline joining an anchor to one of its handle tips.
struct PenChromeStem {
    common::Vec2 a{0.0, 0.0}; // the anchor
    common::Vec2 b{0.0, 0.0}; // the handle tip
};

// Everything the pen chrome draws for one frame.
struct PenChrome {
    std::vector<PenChromeMark> marks;
    std::vector<PenChromeStem> stems;
};

// A handle nearer than this to its own anchor -- in the SCREEN px penChromeMarks is handed -- is
// "collapsed": the node is straight on that side, and a stem and tip crushed under the anchor's own
// knob would be noise. Drawing-only; penHitTest works in layer-local units and asks the weaker
// question ("off its anchor at all"), so a hair-thin handle at 3000 % zoom is still grabbable.
inline constexpr double kPenHandleMinPx = 0.5;

// The chrome for `screenPath`: a knob per anchor, and per handle a stem plus a tip knob. `sel` is
// the selected node (its knob fills) and `hover` the element under the pointer (its knob's border
// lights up); a hover of Kind::Segment or Kind::None lights nothing.
//
// Emission ORDER is the budget policy, and it is the reason the caller can clamp without thinking:
//   1. the selected node's own chrome -- knob, stems and tips -- so the part you are working on can
//      never be the part that is dropped;
//   2. every other anchor knob;
//   3. every other node's stems and handle tips.
// So a clamp costs other nodes' HANDLES first and their anchors only after that, and it can never
// cost the selected node's own three knobs. The caps are the caller's SSBO capacities
// (render::kPenMarkMax / kPenStemMax); pass 0 for both and nothing is emitted.
[[nodiscard]] PenChrome penChromeMarks(const core::vec::Path& screenPath, PenSelection sel,
                                       const PenHit& hover, std::size_t maxMarks,
                                       std::size_t maxStems);

// The closing-loop affordance: is `screenPt` within `radiusPx` of the FIRST node of the first
// subpath -- i.e. would a click there close the path being authored? `outCenter` receives that
// node's position, which is where the ring is drawn. Pure, so the ring and the actual close test
// (PenGesture::press's own close radius) can be pinned against each other.
[[nodiscard]] bool penCloseTarget(const core::vec::Path& screenPath, common::Vec2 screenPt,
                                  double radiusPx, common::Vec2& outCenter);

} // namespace mosaic::ui
