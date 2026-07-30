#include "core/vector/flatten.hpp"
#include "ui/pen_gesture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

// S28 -- the Pen / custom-path tool's pure core (ui/pen_gesture.hpp): the authoring state machine
// (click / click-drag / close / backspace), the node-and-handle edit operations, hit-testing with a
// SCREEN-derived tolerance, the paint the finished path carries, and the draft it lands as. All
// FLTK-free; the canvas-side event plumbing (and the coordinate-frame rule it turns on) is pinned
// separately in test_pen_canvas.cpp.
namespace {

using mosaic::common::Affine2D;
using mosaic::common::ColorF;
using mosaic::common::Vec2;
using mosaic::ui::buildPenDraft;
using mosaic::ui::isPolylineBreak;
using mosaic::ui::kPolylineBreak;
using mosaic::ui::PenChrome;
using mosaic::ui::penChromeMarks;
using mosaic::ui::PenChromeMark;
using mosaic::ui::PenChromeStem;
using mosaic::ui::penCloseTarget;
using mosaic::ui::penConstrainAngle;
using mosaic::ui::penCubicAt;
using mosaic::ui::penDashArray;
using mosaic::ui::penDashStyleOf;
using mosaic::ui::penDeleteNode;
using mosaic::ui::PenDraft;
using mosaic::ui::PenGesture;
using mosaic::ui::PenHit;
using mosaic::ui::penHitTest;
using mosaic::ui::penInsertNode;
using mosaic::ui::penMoveAnchor;
using mosaic::ui::penMoveHandle;
using mosaic::ui::PenOptions;
using mosaic::ui::penPaintedObject;
using mosaic::ui::penPathPolyline;
using mosaic::ui::penRecoloredObject;
using mosaic::ui::PenSelection;
using mosaic::ui::penSegmentCubic;
using mosaic::ui::penToggleNodeType;
using mosaic::ui::penToolBinds;
using mosaic::ui::readPenOptions;

namespace vec = mosaic::core::vec;

// The pen never places a node closer than this to the last one in these fixtures, so a close test
// can never fire by accident.
constexpr double kCloseR = 6.0;

bool near(Vec2 a, Vec2 b, double eps = 1e-9) {
    return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps;
}

// One straight-sided open path: (0,0) -> (100,0) -> (100,100), all corners.
vec::Path polyline3() {
    vec::SubPath sp;
    for (const Vec2 p : {Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 100}}) {
        vec::Node n;
        n.anchor = p;
        n.inHandle = p;
        n.outHandle = p;
        sp.nodes.push_back(n);
    }
    vec::Path path;
    path.subpaths.push_back(sp);
    return path;
}

// polyline3 with a real handle pair pulled out of EVERY node, so the chrome has something to draw
// for each of them: out along +x, in along -x, both 20 units long, and the node marked Smooth.
vec::Path polyline3WithHandles() {
    vec::Path p = polyline3();
    for (vec::Node& n : p.subpaths[0].nodes) {
        n.outHandle = n.anchor + Vec2{20, 0};
        n.inHandle = n.anchor - Vec2{20, 0};
        n.type = vec::Node::Type::Smooth;
    }
    return p;
}

// How many marks of a given kind the chrome carries -- the chrome is a bag, so the tests count
// rather than index wherever the ORDER is not itself the thing under test.
std::size_t countKind(const PenChrome& c, PenChromeMark::Kind k) {
    return static_cast<std::size_t>(
        std::count_if(c.marks.begin(), c.marks.end(),
                      [k](const PenChromeMark& m) { return m.kind == k; }));
}

// A single genuinely curved segment: (0,0) with out (40,-60) -> (100,0) with in (60,60).
vec::Path oneCurve() {
    vec::Node a;
    a.anchor = {0, 0};
    a.inHandle = {0, 0};
    a.outHandle = {40, -60};
    a.type = vec::Node::Type::Smooth;
    vec::Node b;
    b.anchor = {100, 0};
    b.inHandle = {60, 60};
    b.outHandle = {100, 0};
    b.type = vec::Node::Type::Smooth;
    vec::SubPath sp;
    sp.nodes = {a, b};
    vec::Path path;
    path.subpaths.push_back(sp);
    return path;
}

} // namespace

// ---- Authoring ---------------------------------------------------------------------------------

TEST_CASE("pen authoring: a bare click places a CORNER node, handles collapsed on the anchor") {
    PenGesture g;
    CHECK_FALSE(g.active());

    CHECK_FALSE(g.press({10, 10}, kCloseR, /*shift=*/false));
    g.release();
    CHECK(g.active());
    CHECK(g.nodeCount() == 1);
    CHECK(g.path().subpaths.empty()); // one anchor is not a path yet

    CHECK_FALSE(g.press({80, 10}, kCloseR, false));
    g.release();
    CHECK_FALSE(g.press({80, 90}, kCloseR, false));
    g.release();

    const vec::Path p = g.path();
    REQUIRE(p.subpaths.size() == 1);
    REQUIRE(p.subpaths[0].nodes.size() == 3);
    CHECK_FALSE(p.subpaths[0].closed);
    for (const vec::Node& n : p.subpaths[0].nodes) {
        CHECK(n.type == vec::Node::Type::Corner);
        CHECK(near(n.inHandle, n.anchor));
        CHECK(near(n.outHandle, n.anchor));
    }
    CHECK(near(p.subpaths[0].nodes[0].anchor, {10, 10}));
    CHECK(near(p.subpaths[0].nodes[2].anchor, {80, 90}));
}

TEST_CASE("pen authoring: click-and-drag pulls a SYMMETRIC handle pair; Alt breaks it into a cusp") {
    PenGesture g;
    g.press({50, 50}, kCloseR, false);
    CHECK(g.draggingHandle());
    g.dragHandle({90, 50}, /*shift=*/false, /*alt=*/false);
    {
        const vec::Node& n = g.nodes().front();
        CHECK(n.type == vec::Node::Type::Symmetric);
        CHECK(near(n.outHandle, {90, 50}));
        CHECK(near(n.inHandle, {10, 50})); // the exact mirror through the anchor
    }
    // Alt mid-pull: the incoming side collapses back onto the anchor -> a cusp.
    g.dragHandle({90, 50}, false, /*alt=*/true);
    {
        const vec::Node& n = g.nodes().front();
        CHECK(n.type == vec::Node::Type::Corner);
        CHECK(near(n.outHandle, {90, 50}));
        CHECK(near(n.inHandle, {50, 50}));
    }
    g.release();
    CHECK_FALSE(g.draggingHandle());
}

TEST_CASE("pen authoring: Shift constrains the next anchor and the handle pull to 45 degrees") {
    PenGesture g;
    g.press({0, 0}, kCloseR, false);
    g.release();
    // A point 3 degrees off the horizontal snaps flat, keeping its distance.
    const double len = 100.0;
    const Vec2 nearlyFlat{len * std::cos(0.05), len * std::sin(0.05)};
    g.press(nearlyFlat, kCloseR, /*shift=*/true);
    CHECK(g.nodes()[1].anchor.x == doctest::Approx(len));
    CHECK(std::abs(g.nodes()[1].anchor.y) < 1e-9);

    // ... and a handle pull snaps about its own anchor, not the previous node's.
    g.dragHandle({g.nodes()[1].anchor.x + 40.0, g.nodes()[1].anchor.y + 2.0}, /*shift=*/true,
                 /*alt=*/false);
    CHECK(g.nodes()[1].outHandle.y == doctest::Approx(g.nodes()[1].anchor.y));

    // The bare helper, on its own: 45 degrees, length preserved.
    const Vec2 snapped = penConstrainAngle({0, 0}, {10, 9});
    CHECK(snapped.x == doctest::Approx(snapped.y));
    CHECK(Vec2{snapped.x, snapped.y}.length() == doctest::Approx(Vec2{10, 9}.length()));
}

TEST_CASE("pen authoring: clicking the first node CLOSES the path") {
    PenGesture g;
    g.press({0, 0}, kCloseR, false);
    g.release();
    g.press({100, 0}, kCloseR, false);
    g.release();
    g.press({100, 100}, kCloseR, false);
    g.release();
    CHECK_FALSE(g.closed());

    // Just inside the close radius of node 0.
    CHECK(g.press({3, 2}, kCloseR, false));
    CHECK(g.closed());
    CHECK(g.nodeCount() == 3); // closing adds no node
    const vec::Path p = g.path();
    REQUIRE(p.subpaths.size() == 1);
    CHECK(p.subpaths[0].closed);

    // The closing press may still shape the FIRST node's handles.
    g.dragHandle({-40, 0}, false, false);
    CHECK(near(g.nodes().front().outHandle, {-40, 0}));
    CHECK(near(g.nodes().front().inHandle, {40, 0}));
}

TEST_CASE("pen authoring: a close needs two nodes, and a far click just adds one") {
    PenGesture g;
    g.press({0, 0}, kCloseR, false);
    g.release();
    CHECK_FALSE(g.press({1, 1}, kCloseR, false)); // one node: nothing to close
    CHECK(g.nodeCount() == 2);
    CHECK_FALSE(g.closed());
    g.release();
    CHECK_FALSE(g.press({500, 500}, kCloseR, false)); // far away: an ordinary new node
    CHECK(g.nodeCount() == 3);
}

TEST_CASE("pen authoring: Backspace takes nodes back and finally ends the gesture") {
    PenGesture g;
    g.press({0, 0}, kCloseR, false);
    g.release();
    g.press({50, 0}, kCloseR, false);
    g.release();
    g.press({50, 50}, kCloseR, false);
    g.release();
    CHECK(g.nodeCount() == 3);

    CHECK(g.backspace());
    CHECK(g.nodeCount() == 2);
    CHECK(near(g.nodes().back().anchor, {50, 0}));
    CHECK(g.backspace());
    CHECK(g.backspace());
    CHECK(g.nodeCount() == 0);
    CHECK_FALSE(g.active());
    CHECK_FALSE(g.backspace()); // nothing left
}

TEST_CASE("pen authoring: the rubber band adds a provisional node, and only when idle") {
    PenGesture g;
    g.press({0, 0}, kCloseR, false);
    g.release();
    g.press({100, 0}, kCloseR, false);
    g.release();

    // No hover yet -> the drawn path is exactly the placed nodes.
    CHECK(g.pathWithRubberBand().subpaths[0].nodes.size() == 2);

    g.moveTo({100, 80}, false);
    const vec::Path band = g.pathWithRubberBand();
    REQUIRE(band.subpaths.size() == 1);
    REQUIRE(band.subpaths[0].nodes.size() == 3);
    CHECK(near(band.subpaths[0].nodes.back().anchor, {100, 80}));
    CHECK(g.path().subpaths[0].nodes.size() == 2); // ... and it never enters the real path

    // While a handle is being pulled the band must not double up on the live node.
    g.press({100, 160}, kCloseR, false);
    CHECK(g.draggingHandle());
    CHECK(g.pathWithRubberBand().subpaths[0].nodes.size() == 3);

    g.clearHover();
    g.release();
    CHECK(g.pathWithRubberBand().subpaths[0].nodes.size() == 3);
}

// ---- Landing the path --------------------------------------------------------------------------

TEST_CASE("buildPenDraft: geometry is re-centred on the local origin, the placement carries it") {
    PenGesture g;
    g.press({100, 200}, kCloseR, false);
    g.release();
    g.press({300, 200}, kCloseR, false);
    g.release();
    g.press({300, 400}, kCloseR, false);
    g.release();

    PenOptions opts;
    opts.foreground = ColorF{1.0f, 0.0f, 0.0f, 1.0f};
    const std::optional<PenDraft> draft = buildPenDraft(g.path(), opts);
    REQUIRE(draft.has_value());

    const auto* path = std::get_if<vec::Path>(&draft->object.geometry);
    REQUIRE(path != nullptr);
    REQUIRE(path->subpaths.size() == 1);
    REQUIRE(path->subpaths[0].nodes.size() == 3);

    // placement * local == the document points the user clicked. This is the invariant the whole
    // tool rests on: what you drew is where it lands.
    const std::array<Vec2, 3> want{Vec2{100, 200}, Vec2{300, 200}, Vec2{300, 400}};
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(near(draft->placement.apply(path->subpaths[0].nodes[i].anchor), want[i], 1e-9));

    // ... and the geometry really is centred (the S25/S26 rule), not merely translated.
    const std::optional<mosaic::common::Rect> box = vec::contentBounds(draft->object.geometry);
    REQUIRE(box.has_value());
    CHECK(box->center().x == doctest::Approx(0.0));
    CHECK(box->center().y == doctest::Approx(0.0));
}

TEST_CASE("buildPenDraft: refuses a degenerate path, accepts a straight axis-aligned run") {
    PenOptions opts;
    // Two coincident nodes: no extent at all -> authors nothing (a click authors no shape).
    vec::SubPath dot;
    for (int i = 0; i < 2; ++i) {
        vec::Node n;
        n.anchor = {5, 5};
        n.inHandle = {5, 5};
        n.outHandle = {5, 5};
        dot.nodes.push_back(n);
    }
    vec::Path degenerate;
    degenerate.subpaths.push_back(dot);
    CHECK_FALSE(buildPenDraft(degenerate, opts).has_value());

    // A single node is not a path either.
    vec::Path lone;
    lone.subpaths.push_back(vec::SubPath{{dot.nodes[0]}, false});
    CHECK_FALSE(buildPenDraft(lone, opts).has_value());

    // A perfectly horizontal run has a zero-height box and IS legitimate.
    vec::SubPath flat = dot;
    flat.nodes[1].anchor = {105, 5};
    flat.nodes[1].inHandle = {105, 5};
    flat.nodes[1].outHandle = {105, 5};
    vec::Path horizontal;
    horizontal.subpaths.push_back(flat);
    CHECK(buildPenDraft(horizontal, opts).has_value());
}

TEST_CASE("pen paint: fill / stroke / width / cap / join / dash round-trip through the options") {
    PenOptions opts;
    opts.foreground = ColorF{1.0f, 0.0f, 0.0f, 1.0f};
    opts.background = ColorF{0.0f, 0.0f, 1.0f, 1.0f};
    opts.fill = true;
    opts.strokeEnabled = true;
    opts.strokeWidth = 7.0;
    opts.cap = vec::LineCap::Square;
    opts.join = vec::LineJoin::Bevel;
    opts.dashStyle = 3;

    vec::Object base;
    base.geometry = polyline3();
    const vec::Object painted = penPaintedObject(base, opts);
    CHECK(std::holds_alternative<vec::SolidPaint>(painted.fill));
    CHECK(std::get<vec::SolidPaint>(painted.fill).color == opts.foreground);
    CHECK(painted.stroke.enabled);
    CHECK(painted.stroke.width == doctest::Approx(7.0));
    CHECK(painted.stroke.cap == vec::LineCap::Square);
    CHECK(painted.stroke.join == vec::LineJoin::Bevel);
    CHECK(painted.stroke.dashArray.size() == 4);
    // The fill is primary (fg), so the outline over it takes the secondary accent (bg) -- the same
    // convention ui::recoloredObject applies to shapes.
    CHECK(std::get<vec::SolidPaint>(painted.stroke.paint).color == opts.background);

    PenOptions readBack;
    readPenOptions(painted, readBack);
    CHECK(readBack.fill);
    CHECK(readBack.strokeEnabled);
    CHECK(readBack.strokeWidth == doctest::Approx(7.0));
    CHECK(readBack.cap == vec::LineCap::Square);
    CHECK(readBack.join == vec::LineJoin::Bevel);
    CHECK(readBack.dashStyle == 3);
    // Colours are NOT read back: the swatch owns them (readShapeOptions' rule).
    CHECK(readBack.foreground == PenOptions{}.foreground);
}

TEST_CASE("pen paint: a lone stroke takes the FOREGROUND, and both-off still paints something") {
    PenOptions opts;
    opts.foreground = ColorF{0.2f, 0.4f, 0.6f, 1.0f};
    opts.background = ColorF{1.0f, 1.0f, 1.0f, 1.0f};
    opts.fill = false;
    opts.strokeEnabled = true;
    vec::Object base;
    base.geometry = polyline3();

    const vec::Object strokeOnly = penPaintedObject(base, opts);
    CHECK(std::holds_alternative<vec::NoPaint>(strokeOnly.fill));
    CHECK(strokeOnly.stroke.enabled);
    CHECK(std::get<vec::SolidPaint>(strokeOnly.stroke.paint).color == opts.foreground);

    // Both switched off would be an invisible layer; the stroke is forced back on instead.
    opts.strokeEnabled = false;
    const vec::Object neither = penPaintedObject(base, opts);
    CHECK(std::holds_alternative<vec::NoPaint>(neither.fill));
    CHECK(neither.stroke.enabled);

    // Recolour follows the same fg/bg convention in both directions.
    const vec::Object recoloured =
        penRecoloredObject(strokeOnly, ColorF{1, 0, 0, 1}, ColorF{0, 1, 0, 1});
    CHECK(std::get<vec::SolidPaint>(recoloured.stroke.paint).color == ColorF{1, 0, 0, 1});
    opts.fill = true;
    opts.strokeEnabled = true;
    const vec::Object both = penPaintedObject(base, opts);
    const vec::Object bothRecoloured =
        penRecoloredObject(both, ColorF{1, 0, 0, 1}, ColorF{0, 1, 0, 1});
    CHECK(std::get<vec::SolidPaint>(bothRecoloured.fill).color == ColorF{1, 0, 0, 1});
    CHECK(std::get<vec::SolidPaint>(bothRecoloured.stroke.paint).color == ColorF{0, 1, 0, 1});
}

TEST_CASE("pen dash: the four styles scale with the width and reflect back to their index") {
    CHECK(penDashArray(0, 4.0).empty());
    CHECK(penDashStyleOf({}) == 0);
    for (int style = 1; style <= 3; ++style) {
        const std::vector<double> thin = penDashArray(style, 2.0);
        const std::vector<double> thick = penDashArray(style, 8.0);
        REQUIRE_FALSE(thin.empty());
        REQUIRE(thin.size() == thick.size());
        CHECK(thick[0] == doctest::Approx(thin[0] * 4.0)); // scales with the weight
        CHECK(penDashStyleOf(thin) == style);
    }
    // Dotted is the one whose "on" run is much shorter than its gap -- that is how it is told from
    // Dashed on the way back.
    CHECK(penDashArray(2, 10.0)[0] < penDashArray(2, 10.0)[1] * 0.5);
}

TEST_CASE("penToolBinds: the Pen owns editable paths and nothing else") {
    vec::Object path;
    path.geometry = polyline3();
    CHECK(penToolBinds(path));

    vec::Object rect;
    rect.geometry = vec::ParametricShape{vec::RectShape::uniform({40, 20}, 0.0)};
    CHECK_FALSE(penToolBinds(rect));
}

// ---- Hit-testing --------------------------------------------------------------------------------

TEST_CASE("penHitTest: anchors, then segments; only the SELECTED node's handles are grabbable") {
    const vec::Path p = oneCurve();

    // An anchor, from a couple of units away.
    const PenHit anchor = penHitTest(p, {2, 2}, 6.0, PenSelection{});
    CHECK(anchor.kind == PenHit::Kind::Anchor);
    CHECK(anchor.node == 0);

    // Node 0's out handle is at (40,-60), well clear of the curve it steers. It is DRAWN whether or
    // not its node is selected (the chrome has its own overlay lane now), so it is grabbable either
    // way -- what you can grab is what you can see, in both directions.
    const PenHit cold = penHitTest(p, {40, -60}, 6.0, PenSelection{});
    CHECK(cold.kind == PenHit::Kind::OutHandle);
    CHECK(cold.node == 0);
    const PenHit handle = penHitTest(p, {41, -59}, 6.0, PenSelection{true, 0, 0});
    CHECK(handle.kind == PenHit::Kind::OutHandle);
    CHECK(handle.node == 0);

    // A point on the curve's belly picks the SEGMENT, with a usable parameter.
    const std::optional<std::array<Vec2, 4>> cub = penSegmentCubic(p.subpaths[0], 0);
    REQUIRE(cub.has_value());
    const Vec2 belly = penCubicAt(*cub, 0.5);
    const PenHit seg = penHitTest(p, belly, 4.0, PenSelection{});
    CHECK(seg.kind == PenHit::Kind::Segment);
    CHECK(seg.node == 0);
    CHECK(seg.t == doctest::Approx(0.5).epsilon(0.02));
    CHECK(near(seg.point, belly, 0.5));

    // Far from everything: nothing.
    CHECK_FALSE(penHitTest(p, {-500, -500}, 6.0, PenSelection{}).hit());
}

TEST_CASE("penHitTest: an unselected node's handle is grabbable, but an anchor still outranks it") {
    // Every node's handles are drawn, so every node's handles must be pickable -- and the tier they
    // sit in matters: park a handle exactly on a NEIGHBOUR's anchor and the anchor has to win, or a
    // curve with a long arm would make its own neighbour ungrabbable.
    vec::Path p = polyline3();
    p.subpaths[0].nodes[0].outHandle = {100, 0}; // node 0's arm parked on node 1's anchor
    p.subpaths[0].nodes[0].type = vec::Node::Type::Smooth;
    const PenHit onAnchor = penHitTest(p, {100, 0}, 6.0, PenSelection{});
    CHECK(onAnchor.kind == PenHit::Kind::Anchor);
    CHECK(onAnchor.node == 1);

    // Move it clear and it is pickable on its own, with nothing selected.
    p.subpaths[0].nodes[0].outHandle = {40, 40};
    const PenHit free = penHitTest(p, {41, 41}, 6.0, PenSelection{});
    CHECK(free.kind == PenHit::Kind::OutHandle);
    CHECK(free.node == 0);
    CHECK(free.selection() == PenSelection{true, 0, 0}); // the grab carries its node's address

    // A handle still collapsed on its own anchor is not a separate target.
    const PenHit collapsed = penHitTest(polyline3(), {100, 0}, 6.0, PenSelection{});
    CHECK(collapsed.kind == PenHit::Kind::Anchor);
}

TEST_CASE("penHitTest tolerance: a SCREEN-pixel grab stays usable at extreme zooms") {
    // The canvas derives its layer-local tolerance as kPenPickScreenPx / zoom (and then through the
    // layer's inverse world transform). That conversion is the whole reason a node stays grabbable
    // at 5% and at 6400%; a tolerance left in document units would be unusable at both ends. This
    // pins the arithmetic on the pure side.
    constexpr double kPickScreenPx = 6.0;
    const vec::Path p = polyline3();

    for (const double zoom : {0.05, 1.0, 64.0}) {
        const double tolDoc = kPickScreenPx / zoom;
        const double fiveScreenPxInDoc = 5.0 / zoom;
        const double eightScreenPxInDoc = 8.0 / zoom;
        // 5 screen px off the anchor: a hit at EVERY zoom.
        const PenHit hit = penHitTest(p, {100.0 + fiveScreenPxInDoc, 0.0}, tolDoc, PenSelection{});
        CHECK(hit.kind == PenHit::Kind::Anchor);
        CHECK(hit.node == 1);
        // 8 screen px off, and away from the outline: a miss at EVERY zoom.
        CHECK_FALSE(
            penHitTest(p, {100.0 + eightScreenPxInDoc, -eightScreenPxInDoc}, tolDoc, PenSelection{})
                .hit());
    }
}

// ---- The edit operations -------------------------------------------------------------------------

TEST_CASE("penMoveAnchor: the handles ride with the anchor (a move re-places, never re-shapes)") {
    const vec::Path p = oneCurve();
    const vec::Path moved = penMoveAnchor(p, PenSelection{true, 0, 0}, {10, 20});
    const vec::Node& before = p.subpaths[0].nodes[0];
    const vec::Node& after = moved.subpaths[0].nodes[0];
    CHECK(near(after.anchor, before.anchor + Vec2{10, 20}));
    CHECK(near(after.outHandle, before.outHandle + Vec2{10, 20}));
    CHECK(near(after.inHandle, before.inHandle + Vec2{10, 20}));
    // The neighbour is untouched.
    CHECK(moved.subpaths[0].nodes[1] == p.subpaths[0].nodes[1]);
    // An invalid address is a no-op, not a crash.
    CHECK(penMoveAnchor(p, PenSelection{true, 3, 9}, {1, 1}) == p);
    CHECK(penMoveAnchor(p, PenSelection{}, {1, 1}) == p);
}

TEST_CASE("penMoveHandle: Symmetric mirrors, Smooth keeps its length, Alt breaks the pair") {
    vec::Path p = oneCurve();
    p.subpaths[0].nodes[0].inHandle = {-20, 30};       // a real incoming arm, length 36.06
    p.subpaths[0].nodes[0].type = vec::Node::Type::Smooth;
    const PenSelection n0{true, 0, 0};
    const double inLen = (p.subpaths[0].nodes[0].inHandle - p.subpaths[0].nodes[0].anchor).length();

    // Smooth: the opposite arm swings collinear but keeps its own length.
    const vec::Path smooth = penMoveHandle(p, n0, /*outSide=*/true, {0, -50}, /*breakPair=*/false);
    const vec::Node& sn = smooth.subpaths[0].nodes[0];
    CHECK(near(sn.outHandle, {0, -50}));
    CHECK((sn.inHandle - sn.anchor).length() == doctest::Approx(inLen));
    CHECK(sn.inHandle.x == doctest::Approx(0.0));
    CHECK(sn.inHandle.y == doctest::Approx(inLen)); // straight opposite (+y), since out is -y

    // Symmetric: an exact mirror.
    p.subpaths[0].nodes[0].type = vec::Node::Type::Symmetric;
    const vec::Path sym = penMoveHandle(p, n0, true, {0, -50}, false);
    CHECK(near(sym.subpaths[0].nodes[0].inHandle, {0, 50}));

    // Alt: the other arm stays exactly where it was, and the node becomes a cusp.
    const vec::Path cusp = penMoveHandle(p, n0, true, {0, -50}, /*breakPair=*/true);
    CHECK(near(cusp.subpaths[0].nodes[0].inHandle, {-20, 30}));
    CHECK(cusp.subpaths[0].nodes[0].type == vec::Node::Type::Corner);

    // An existing cusp keeps its independence without Alt.
    vec::Path corner = p;
    corner.subpaths[0].nodes[0].type = vec::Node::Type::Corner;
    const vec::Path stillCusp = penMoveHandle(corner, n0, true, {0, -50}, false);
    CHECK(near(stillCusp.subpaths[0].nodes[0].inHandle, {-20, 30}));
}

TEST_CASE("penInsertNode: a de Casteljau split leaves the DRAWN curve identical") {
    const vec::Path p = oneCurve();
    const std::optional<std::array<Vec2, 4>> orig = penSegmentCubic(p.subpaths[0], 0);
    REQUIRE(orig.has_value());

    constexpr double u = 0.375;
    PenSelection added;
    const vec::Path split = penInsertNode(p, PenSelection{true, 0, 0}, u, &added);
    REQUIRE(added.valid);
    CHECK(added.node == 1);
    REQUIRE(split.subpaths[0].nodes.size() == 3);
    CHECK(near(split.subpaths[0].nodes[1].anchor, penCubicAt(*orig, u), 1e-9));

    const std::optional<std::array<Vec2, 4>> left = penSegmentCubic(split.subpaths[0], 0);
    const std::optional<std::array<Vec2, 4>> right = penSegmentCubic(split.subpaths[0], 1);
    REQUIRE(left.has_value());
    REQUIRE(right.has_value());
    for (int k = 0; k <= 8; ++k) {
        const double s = static_cast<double>(k) / 8.0;
        CHECK(near(penCubicAt(*left, s), penCubicAt(*orig, u * s), 1e-9));
        CHECK(near(penCubicAt(*right, s), penCubicAt(*orig, u + (1.0 - u) * s), 1e-9));
    }
}

TEST_CASE("penInsertNode: the CLOSING segment of a closed path appends rather than wraps") {
    vec::Path p = polyline3();
    p.subpaths[0].closed = true;
    const std::size_t last = p.subpaths[0].nodes.size() - 1;
    PenSelection added;
    const vec::Path split = penInsertNode(p, PenSelection{true, 0, last}, 0.5, &added);
    REQUIRE(added.valid);
    CHECK(added.node == 3); // appended at the end, not wrapped to index 0
    REQUIRE(split.subpaths[0].nodes.size() == 4);
    CHECK(split.subpaths[0].closed);
    CHECK(near(split.subpaths[0].nodes[3].anchor, {50, 50}));
    CHECK(near(split.subpaths[0].nodes[0].anchor, {0, 0})); // the first node did not move
}

TEST_CASE("penDeleteNode: removes the node, and drops a subpath left too small to draw") {
    const vec::Path p = polyline3();
    const vec::Path gone = penDeleteNode(p, PenSelection{true, 0, 1});
    REQUIRE(gone.subpaths.size() == 1);
    REQUIRE(gone.subpaths[0].nodes.size() == 2);
    CHECK(near(gone.subpaths[0].nodes[0].anchor, {0, 0}));
    CHECK(near(gone.subpaths[0].nodes[1].anchor, {100, 100}));

    const vec::Path one = penDeleteNode(gone, PenSelection{true, 0, 0});
    CHECK(one.subpaths.empty()); // a lone anchor draws nothing: the subpath goes with it
    CHECK(penDeleteNode(p, PenSelection{}) == p);
}

TEST_CASE("penToggleNodeType: a corner rounds off, and a smooth node collapses back to a cusp") {
    const vec::Path p = polyline3();
    const PenSelection mid{true, 0, 1};
    const vec::Path smoothed = penToggleNodeType(p, mid);
    const vec::Node& n = smoothed.subpaths[0].nodes[1];
    CHECK(n.type == vec::Node::Type::Symmetric);
    const Vec2 out = n.outHandle - n.anchor;
    const Vec2 in = n.inHandle - n.anchor;
    CHECK(out.length() > 1.0);
    CHECK(near(in, -out, 1e-9));                                   // symmetric
    CHECK(out.length() == doctest::Approx(100.0 / 3.0));           // a third of the shorter arm
    // Collinear with the chord through the neighbours ((0,0) -> (100,100)), i.e. 45 degrees.
    CHECK(out.x == doctest::Approx(out.y));

    const vec::Path backToCorner = penToggleNodeType(smoothed, mid);
    const vec::Node& c = backToCorner.subpaths[0].nodes[1];
    CHECK(c.type == vec::Node::Type::Corner);
    CHECK(near(c.inHandle, c.anchor));
    CHECK(near(c.outHandle, c.anchor));
}

// ---- Overlay geometry ------------------------------------------------------------------------------

TEST_CASE("penPathPolyline: one polyline; a closed contour repeats its first point") {
    const std::vector<Vec2> open = penPathPolyline(polyline3());
    REQUIRE(open.size() >= 3);
    CHECK(near(open.front(), {0, 0}));
    CHECK(near(open.back(), {100, 100}));
    CHECK_FALSE(near(open.front(), open.back()));

    vec::Path closedPath = polyline3();
    closedPath.subpaths[0].closed = true;
    const std::vector<Vec2> closed = penPathPolyline(closedPath);
    REQUIRE(closed.size() >= 4);
    CHECK(near(closed.front(), closed.back())); // drawn as ONE open polyline by the overlay lane

    CHECK(penPathPolyline(vec::Path{}).empty());
}

TEST_CASE("penPathPolyline: EVERY contour is emitted, fenced off by a break marker") {
    // The overlay lane joins consecutive vertices, so without the break the last point of contour A
    // would be chorded to the first of contour B -- a line that is nowhere in the path. Returning
    // only the first contour (what this did before) hid that, and hid the second contour with it:
    // a boolean result or a two-subpath pen path previewed as half of itself.
    vec::Path two = polyline3();
    vec::SubPath second;
    for (const Vec2 q : {Vec2{300, 300}, Vec2{400, 300}, Vec2{400, 400}}) {
        vec::Node n;
        n.anchor = q;
        n.inHandle = q;
        n.outHandle = q;
        second.nodes.push_back(n);
    }
    two.subpaths.push_back(second);

    const std::vector<Vec2> pts = penPathPolyline(two);
    const std::size_t breaks =
        static_cast<std::size_t>(std::count_if(pts.begin(), pts.end(), isPolylineBreak));
    CHECK(breaks == 1); // exactly one fence, and only BETWEEN contours -- never leading or trailing
    CHECK_FALSE(isPolylineBreak(pts.front()));
    CHECK_FALSE(isPolylineBreak(pts.back()));
    CHECK(near(pts.front(), {0, 0}));
    CHECK(near(pts.back(), {400, 400}));

    // Both contours are really there, each in its own run, and neither run straddles the fence.
    const auto fence = std::find_if(pts.begin(), pts.end(), isPolylineBreak);
    REQUIRE(fence != pts.end());
    CHECK(near(*(fence - 1), {100, 100}));
    CHECK(near(*(fence + 1), {300, 300}));

    // The marker really is out of range -- that is the whole mechanism (the shader tests x, and the
    // renderer keeps the marker out of its bbox rather than scaling it).
    CHECK(isPolylineBreak(kPolylineBreak));
    CHECK_FALSE(isPolylineBreak(Vec2{-1.0e6, -1.0e6})); // a merely far-off-canvas point is a POINT
}

TEST_CASE("penPathPolyline: the points come back in the path's own space, whatever toDevice says") {
    // toDevice picks the flattening TOLERANCE and nothing else; the canvas lifts the result to
    // screen itself (through the layer's world transform in edit mode). If this ever started
    // pre-transforming, the spine would land twice-transformed and sit a whole viewport away.
    const vec::Path p = oneCurve();
    const std::vector<Vec2> plain = penPathPolyline(p, Affine2D::identity());
    const std::vector<Vec2> scaled = penPathPolyline(p, Affine2D::scaling(8.0, 8.0));
    REQUIRE(plain.size() >= 2);
    REQUIRE(scaled.size() >= plain.size()); // a bigger device box earns MORE samples, not moved ones
    CHECK(near(plain.front(), scaled.front(), 1e-9));
    CHECK(near(plain.back(), scaled.back(), 1e-9));
    CHECK(near(plain.front(), {0, 0}, 1e-9));
}

// ---- The node / handle chrome ---------------------------------------------------------------------

TEST_CASE("penChromeMarks: every node gets an anchor knob and every live handle a stem and a tip") {
    // The defect this replaces: the old chrome drew handles for the SELECTED node only, so every
    // other node's handles were invisible -- and therefore ungrabbable.
    const vec::Path p = polyline3WithHandles();
    const PenChrome c = penChromeMarks(p, PenSelection{true, 0, 1}, PenHit{}, 512, 512);

    CHECK(countKind(c, PenChromeMark::Kind::AnchorSmooth) == 3); // one per node
    CHECK(countKind(c, PenChromeMark::Kind::AnchorCusp) == 0);
    CHECK(countKind(c, PenChromeMark::Kind::HandleTip) == 6);    // both arms of all three
    CHECK(c.stems.size() == 6);                                  // one stem per drawn tip
    for (const PenChromeStem& s : c.stems)                       // every stem runs anchor -> tip
        CHECK(std::abs((s.b - s.a).length() - 20.0) < 1e-9);

    // A handle collapsed on its anchor is straight, so it draws nothing at all.
    const PenChrome bare = penChromeMarks(polyline3(), PenSelection{}, PenHit{}, 512, 512);
    CHECK(bare.marks.size() == 3);
    CHECK(bare.stems.empty());
    CHECK(countKind(bare, PenChromeMark::Kind::AnchorCusp) == 3);
}

TEST_CASE("penChromeMarks: the selected node is emitted first and outlives a tight budget") {
    const vec::Path p = polyline3WithHandles();
    const PenSelection sel{true, 0, 2};
    const PenChrome full = penChromeMarks(p, sel, PenHit{}, 512, 512);
    REQUIRE(full.marks.size() >= 3);
    CHECK(near(full.marks[0].pos, {100, 100})); // node 2's anchor leads
    CHECK(full.marks[0].selected);
    CHECK(full.marks[1].kind == PenChromeMark::Kind::HandleTip); // ... then its own two tips
    CHECK(full.marks[2].kind == PenChromeMark::Kind::HandleTip);
    CHECK(full.marks[1].selected);

    // Exactly one node is ever `selected`, and it is the addressed one.
    const std::size_t selectedAnchors = static_cast<std::size_t>(
        std::count_if(full.marks.begin(), full.marks.end(), [](const PenChromeMark& m) {
            return m.selected && m.kind != PenChromeMark::Kind::HandleTip;
        }));
    CHECK(selectedAnchors == 1);

    // A budget of one mark keeps the selected node's anchor -- the part being worked on.
    const PenChrome clamped = penChromeMarks(p, sel, PenHit{}, 1, 0);
    REQUIRE(clamped.marks.size() == 1);
    CHECK(near(clamped.marks[0].pos, {100, 100}));
    CHECK(clamped.stems.empty());
    CHECK(penChromeMarks(p, sel, PenHit{}, 0, 0).marks.empty());
}

TEST_CASE("penChromeMarks: a budget squeeze costs other nodes' handles before their anchors") {
    // Ranking, not clipping: an anchor you cannot see is a node you did not know was there, while a
    // missing handle is only a curve you must select before you can shape.
    const vec::Path p = polyline3WithHandles();
    const PenChrome c = penChromeMarks(p, PenSelection{true, 0, 0}, PenHit{}, 5, 512);
    REQUIRE(c.marks.size() == 5);
    // The selected node's anchor + its two tips, then BOTH remaining anchors -- no other tips.
    CHECK(countKind(c, PenChromeMark::Kind::AnchorSmooth) == 3);
    CHECK(countKind(c, PenChromeMark::Kind::HandleTip) == 2);
    CHECK(c.marks[0].selected);
    CHECK(c.stems.size() == 2); // stem and tip go together: no stem points at a dropped knob

    // The stem cap is independent, so it can starve on its own without touching the knobs.
    const PenChrome noStems = penChromeMarks(p, PenSelection{true, 0, 0}, PenHit{}, 512, 0);
    CHECK(noStems.stems.empty());
    CHECK(countKind(noStems, PenChromeMark::Kind::AnchorSmooth) == 3);
}

TEST_CASE("penChromeMarks: a cusp anchor is square, a smoothed one is round, a tip is a tip") {
    // SHAPE carries the node's type, which is why the drawn mark can say something the old
    // identical crosses could not.
    vec::Path p = polyline3WithHandles();
    p.subpaths[0].nodes[0].type = vec::Node::Type::Corner;
    p.subpaths[0].nodes[1].type = vec::Node::Type::Smooth;
    p.subpaths[0].nodes[2].type = vec::Node::Type::Symmetric;
    const PenChrome c = penChromeMarks(p, PenSelection{}, PenHit{}, 512, 512);

    CHECK(countKind(c, PenChromeMark::Kind::AnchorCusp) == 1);   // Corner only
    CHECK(countKind(c, PenChromeMark::Kind::AnchorSmooth) == 2); // Smooth AND Symmetric
    for (const PenChromeMark& m : c.marks)
        if (near(m.pos, {0, 0}))
            CHECK(m.kind == PenChromeMark::Kind::AnchorCusp);
}

TEST_CASE("penChromeMarks: exactly the hovered element lights up, and a segment lights nothing") {
    const vec::Path p = polyline3WithHandles();
    const auto hoverCount = [](const PenChrome& c) {
        return static_cast<std::size_t>(std::count_if(
            c.marks.begin(), c.marks.end(), [](const PenChromeMark& m) { return m.hovered; }));
    };

    PenHit onAnchor;
    onAnchor.kind = PenHit::Kind::Anchor;
    onAnchor.node = 1;
    const PenChrome a = penChromeMarks(p, PenSelection{}, onAnchor, 512, 512);
    CHECK(hoverCount(a) == 1);
    for (const PenChromeMark& m : a.marks)
        if (m.hovered)
            CHECK(near(m.pos, {100, 0})); // node 1's anchor, not its handle tips

    PenHit onHandle;
    onHandle.kind = PenHit::Kind::OutHandle;
    onHandle.node = 2;
    const PenChrome h = penChromeMarks(p, PenSelection{}, onHandle, 512, 512);
    CHECK(hoverCount(h) == 1);
    for (const PenChromeMark& m : h.marks)
        if (m.hovered)
            CHECK(near(m.pos, {120, 100})); // node 2's OUT tip; the IN tip at (80,100) stays cold

    PenHit onSegment;
    onSegment.kind = PenHit::Kind::Segment;
    CHECK(hoverCount(penChromeMarks(p, PenSelection{}, onSegment, 512, 512)) == 0);
    CHECK(hoverCount(penChromeMarks(p, PenSelection{}, PenHit{}, 512, 512)) == 0);
}

TEST_CASE("penChromeMarks: the output is pure position -- no size, no DPI, no scale enters it") {
    // The knob sizes live entirely in the shader (they hang off the transform handles' own H, which
    // the renderer scales by the content scale), so this builder takes no size argument at all and
    // its output is exactly the node positions it was handed. That is what makes the chrome
    // DPI-correct by construction: there is no length here for a display scale to get wrong.
    const vec::Path p = polyline3WithHandles();
    const PenChrome one = penChromeMarks(p, PenSelection{true, 0, 1}, PenHit{}, 512, 512);

    // The same path at 2x -- a HiDPI frame is the identical geometry, scaled -- yields the identical
    // chrome, scaled by exactly 2, mark for mark and stem for stem.
    vec::Path twice = p;
    for (vec::Node& n : twice.subpaths[0].nodes) {
        n.anchor = n.anchor * 2.0;
        n.inHandle = n.inHandle * 2.0;
        n.outHandle = n.outHandle * 2.0;
    }
    const PenChrome two = penChromeMarks(twice, PenSelection{true, 0, 1}, PenHit{}, 512, 512);
    REQUIRE(two.marks.size() == one.marks.size());
    REQUIRE(two.stems.size() == one.stems.size());
    for (std::size_t i = 0; i < one.marks.size(); ++i) {
        CHECK(two.marks[i].pos == one.marks[i].pos * 2.0); // exact: it is a copy, not a computation
        CHECK(two.marks[i].kind == one.marks[i].kind);
        CHECK(two.marks[i].selected == one.marks[i].selected);
    }
    for (std::size_t i = 0; i < one.stems.size(); ++i) {
        CHECK(two.stems[i].a == one.stems[i].a * 2.0);
        CHECK(two.stems[i].b == one.stems[i].b * 2.0);
    }
}

TEST_CASE("penCloseTarget: the ring shows exactly where a click would close the path") {
    // The affordance and the actual close must agree, or the ring is a lie. Both measure from the
    // FIRST node with the same radius -- this pins the drawn half against PenGesture::press below.
    const vec::Path p = polyline3();
    Vec2 centre{-1, -1};
    CHECK(penCloseTarget(p, {3, 2}, 9.0, centre));
    CHECK(near(centre, {0, 0})); // the ring rides node 0, not the pointer

    centre = {-1, -1};
    CHECK_FALSE(penCloseTarget(p, {20, 20}, 9.0, centre));
    CHECK(near(centre, {-1, -1})); // a miss leaves the caller's centre untouched

    // Just inside / just outside the radius, and the boundary itself (inclusive, like the gesture).
    CHECK(penCloseTarget(p, {0, 8.999}, 9.0, centre));
    CHECK(penCloseTarget(p, {0, 9.0}, 9.0, centre));
    CHECK_FALSE(penCloseTarget(p, {0, 9.001}, 9.0, centre));

    // No path, no first node, no ring; and a non-positive radius never rings.
    CHECK_FALSE(penCloseTarget(vec::Path{}, {0, 0}, 9.0, centre));
    CHECK_FALSE(penCloseTarget(p, {0, 0}, 0.0, centre));

    // The gesture agrees: the same 9-unit reach from the same node really does close.
    PenGesture g;
    g.press({0, 0}, 9.0, false);
    g.release();
    g.press({100, 0}, 9.0, false);
    g.release();
    g.press({100, 100}, 9.0, false);
    g.release();
    CHECK(g.press({3, 2}, 9.0, false)); // the very point the ring appeared at
    CHECK(g.closed());
}

// ---- Multi-subpath editing (the baked Combine Paths result) -------------------------------------
//
// Layer ▸ Combine Paths commits a BAKED, multi-subpath core::vec::Path rather than a live compound,
// so penToolBinds accepts it and the Pen has to EDIT it -- every contour, addressed by (subpath,
// node). These cases pin the property the tool cannot be allowed to lose: an edit lands in the
// contour the pick named, and nowhere else. Silently reshaping a curve the user never touched is
// the one failure a node editor may not have.

namespace {

// One straight-sided open contour of three corner nodes, translated by `o` -- so several of them
// can be stacked into one Path far apart from each other and a pick can never be ambiguous.
vec::SubPath contourAt(Vec2 o) {
    vec::SubPath sp;
    for (const Vec2 q : {Vec2{0, 0}, Vec2{100, 0}, Vec2{100, 100}}) {
        vec::Node n;
        n.anchor = o + q;
        n.inHandle = n.anchor;
        n.outHandle = n.anchor;
        sp.nodes.push_back(n);
    }
    return sp;
}

// Three well-separated contours: A at the origin, B at +1000x, C at +2000x.
vec::Path threeContours() {
    vec::Path p;
    p.subpaths.push_back(contourAt({0, 0}));
    p.subpaths.push_back(contourAt({1000, 0}));
    p.subpaths.push_back(contourAt({2000, 0}));
    return p;
}

} // namespace

TEST_CASE("multi-subpath: penHitTest names the contour it actually hit") {
    const vec::Path p = threeContours();
    vec::Object baked; // the Pen binds a BAKED multi-subpath Path, which is what Combine Paths now
    baked.geometry = p; // commits -- a live BooleanCompound would fail this predicate
    CHECK(penToolBinds(baked));

    // An anchor of the MIDDLE contour. Getting subpath 0 here -- the shape of the bug this guards --
    // would have every later edit land on a curve a thousand units away.
    const PenHit mid = penHitTest(p, {1100, 0}, 8.0, PenSelection{});
    CHECK(mid.kind == PenHit::Kind::Anchor);
    CHECK(mid.subpath == 1);
    CHECK(mid.node == 1);

    const PenHit last = penHitTest(p, {2100, 100}, 8.0, PenSelection{});
    CHECK(last.kind == PenHit::Kind::Anchor);
    CHECK(last.subpath == 2);
    CHECK(last.node == 2);

    const PenHit first = penHitTest(p, {0, 0}, 8.0, PenSelection{});
    CHECK(first.kind == PenHit::Kind::Anchor);
    CHECK(first.subpath == 0);
    CHECK(first.node == 0);

    // A SEGMENT of the middle contour: the bar the node-insert gesture rides.
    const PenHit seg = penHitTest(p, {1050, 0}, 8.0, PenSelection{});
    CHECK(seg.kind == PenHit::Kind::Segment);
    CHECK(seg.subpath == 1);
    CHECK(seg.node == 0);

    // And a handle of a LATER contour, which is the tier that only exists because every node's
    // handles are now drawn: what you can see is what you can grab, on every contour.
    vec::Path withHandle = p;
    withHandle.subpaths[2].nodes[0].outHandle = Vec2{2040, -60};
    withHandle.subpaths[2].nodes[0].type = vec::Node::Type::Smooth;
    const PenHit tip = penHitTest(withHandle, {2040, -60}, 8.0, PenSelection{});
    CHECK(tip.kind == PenHit::Kind::OutHandle);
    CHECK(tip.subpath == 2);
    CHECK(tip.node == 0);
}

TEST_CASE("multi-subpath: an inserted node goes into the segment's OWN contour") {
    const vec::Path p = threeContours();
    PenSelection added;
    const vec::Path next = penInsertNode(p, PenSelection{true, 1, 0}, 0.5, &added);

    REQUIRE(added.valid);
    CHECK(added.subpath == 1); // ... not 0, which is exactly the "blindly to the first" failure
    CHECK(added.node == 1);
    REQUIRE(next.subpaths.size() == 3);
    CHECK(next.subpaths[0].nodes.size() == 3); // the other two contours are untouched, node for node
    CHECK(next.subpaths[1].nodes.size() == 4);
    CHECK(next.subpaths[2].nodes.size() == 3);
    CHECK(near(next.subpaths[1].nodes[added.node].anchor, {1050, 0}));
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(near(next.subpaths[0].nodes[i].anchor, p.subpaths[0].nodes[i].anchor));
        CHECK(near(next.subpaths[2].nodes[i].anchor, p.subpaths[2].nodes[i].anchor));
    }

    // A segment address in a contour that does not exist changes nothing and reports no node -- the
    // canvas keys its drag arming on exactly this, rather than falling back to {0, 0}.
    PenSelection none{true, 9, 0};
    const vec::Path same = penInsertNode(p, PenSelection{true, 9, 0}, 0.5, &none);
    CHECK_FALSE(none.valid);
    CHECK(same.subpaths.size() == 3);
    CHECK(same.subpaths[1].nodes.size() == 3);
}

TEST_CASE("multi-subpath: moving an anchor or a handle leaves the other contours alone") {
    const vec::Path p = threeContours();

    const vec::Path moved = penMoveAnchor(p, PenSelection{true, 2, 1}, {7, -3});
    REQUIRE(moved.subpaths.size() == 3);
    CHECK(near(moved.subpaths[2].nodes[1].anchor, {2107, -3}));
    for (std::size_t s = 0; s < 2; ++s)
        for (std::size_t i = 0; i < 3; ++i)
            CHECK(near(moved.subpaths[s].nodes[i].anchor, p.subpaths[s].nodes[i].anchor));

    const vec::Path pulled = penMoveHandle(p, PenSelection{true, 1, 2}, /*outSide=*/true,
                                           {1140, 130}, /*breakPair=*/false);
    CHECK(near(pulled.subpaths[1].nodes[2].outHandle, {1140, 130}));
    CHECK(near(pulled.subpaths[0].nodes[2].outHandle, p.subpaths[0].nodes[2].outHandle));
    CHECK(near(pulled.subpaths[2].nodes[2].outHandle, p.subpaths[2].nodes[2].outHandle));
}

TEST_CASE("multi-subpath: dropping a contour RENUMBERS the ones after it") {
    // This is the whole reason the canvas clears its selection / grab / hover addresses after a
    // structural edit. On a ONE-contour path a stale address was self-limiting -- the path went with
    // the contour, so every address simply failed. Here contour 2 becomes contour 1, so a kept
    // address stops being invalid and starts naming a DIFFERENT curve's node.
    vec::Path p = threeContours();
    p.subpaths[1].nodes.resize(2); // two nodes: one delete takes the contour below the minimum

    const vec::Path gone = penDeleteNode(p, PenSelection{true, 1, 0});
    REQUIRE(gone.subpaths.size() == 2);
    CHECK(near(gone.subpaths[0].nodes[0].anchor, {0, 0}));       // A stayed put...
    CHECK(near(gone.subpaths[1].nodes[0].anchor, {2000, 0}));    // ...and C slid into B's index
    // The trap in one line: an address that named C before the delete now names C's OLD neighbour.
    CHECK_FALSE(near(gone.subpaths[1].nodes[0].anchor, p.subpaths[1].nodes[0].anchor));

    // A plain delete that does NOT drop a contour renumbers nothing.
    const vec::Path trimmed = penDeleteNode(threeContours(), PenSelection{true, 1, 1});
    REQUIRE(trimmed.subpaths.size() == 3);
    CHECK(trimmed.subpaths[1].nodes.size() == 2);
    CHECK(near(trimmed.subpaths[2].nodes[0].anchor, {2000, 0}));
}

TEST_CASE("multi-subpath: the chrome draws every contour, and the clamp never costs the selection") {
    const vec::Path p = threeContours();
    const PenChrome all = penChromeMarks(p, PenSelection{}, PenHit{}, 512, 512);
    CHECK(countKind(all, PenChromeMark::Kind::AnchorCusp) == 9); // 3 contours x 3 corner nodes

    // The budget policy across contours: with room for exactly one mark, the one emitted is the
    // SELECTED node -- even when it lives in the LAST contour, which a first-contour-first emission
    // order would have spent the budget long before reaching.
    const PenChrome tight = penChromeMarks(p, PenSelection{true, 2, 1}, PenHit{}, 1, 512);
    REQUIRE(tight.marks.size() == 1);
    CHECK(tight.marks[0].selected);
    CHECK(near(tight.marks[0].pos, {2100, 0}));

    // A hover in a later contour lights that contour's knob and no other.
    const PenHit hover{PenHit::Kind::Anchor, 1, 2, 0.0, {1100, 100}};
    const PenChrome lit = penChromeMarks(p, PenSelection{}, hover, 512, 512);
    std::size_t hovered = 0;
    for (const PenChromeMark& m : lit.marks)
        if (m.hovered) {
            ++hovered;
            CHECK(near(m.pos, {1100, 100}));
        }
    CHECK(hovered == 1);
}

TEST_CASE("multi-subpath: buildPenDraft keeps every contour and re-centres them together") {
    // One placement for the whole object: the contours must move as a rigid set, or a combined path
    // would come apart the moment it landed.
    PenOptions opts;
    const vec::Path p = threeContours();
    const std::optional<PenDraft> draft = buildPenDraft(p, opts);
    REQUIRE(draft.has_value());
    const auto* local = std::get_if<vec::Path>(&draft->object.geometry);
    REQUIRE(local != nullptr);
    REQUIRE(local->subpaths.size() == 3);
    for (std::size_t s = 0; s < 3; ++s) {
        REQUIRE(local->subpaths[s].nodes.size() == 3);
        for (std::size_t i = 0; i < 3; ++i)
            CHECK(near(draft->placement.apply(local->subpaths[s].nodes[i].anchor),
                       p.subpaths[s].nodes[i].anchor, 1e-9));
    }
}
