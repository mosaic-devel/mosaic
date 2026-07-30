#include "core/selection.hpp"
#include "ui/selection_gesture.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <optional>

// The S14 marquee/lasso gesture engine (ui::SelectionGesture): modifier mapping, drag
// constraints, the press-drag-release and click-click-close state machines, commit semantics
// (degenerate clicks, no-op combines, never landing an all-zero mask), and the live preview.
// Pure logic -- the FLTK event plumbing in VulkanCanvas stays thin and is exercised by the
// --gui-frames smoke run instead.
namespace {

using mosaic::common::Rect;
using mosaic::common::Vec2;
using mosaic::core::Selection;
using mosaic::core::SelectOp;
using mosaic::ui::catmullRomSmooth;
using mosaic::ui::laplacianSmooth;
using mosaic::ui::marqueeRect;
using mosaic::ui::selectOpForModifiers;
using mosaic::ui::SelectionGesture;

using Kind = SelectionGesture::Kind;
using Phase = SelectionGesture::Phase;

} // namespace

TEST_CASE("selectOpForModifiers: press-time modifiers choose the boolean op") {
    CHECK(selectOpForModifiers(false, false, false) == SelectOp::Replace);
    CHECK(selectOpForModifiers(true, false, false) == SelectOp::Add);
    CHECK(selectOpForModifiers(false, true, false) == SelectOp::Subtract);
    CHECK(selectOpForModifiers(true, true, false) == SelectOp::Intersect);
    CHECK(selectOpForModifiers(false, false, true) == SelectOp::Intersect);
}

TEST_CASE("marqueeRect: plain, square-constrained, and from-centre drags") {
    CHECK(marqueeRect({2, 3}, {10, 7}, false, false) == Rect{2, 3, 8, 4});
    // Dragging up-left still yields a normalised rect.
    CHECK(marqueeRect({10, 7}, {2, 3}, false, false) == Rect{2, 3, 8, 4});
    // Shift: the longer axis wins, the corner keeps its quadrant.
    CHECK(marqueeRect({2, 3}, {10, 7}, true, false) == Rect{2, 3, 8, 8});
    CHECK(marqueeRect({2, 3}, {-6, 7}, true, false) == Rect{-6, 3, 8, 8});
    // Alt: grow around the anchor.
    CHECK(marqueeRect({5, 5}, {8, 7}, false, true) == Rect{2, 3, 6, 4});
    // Both: a centred square.
    CHECK(marqueeRect({5, 5}, {8, 7}, true, true) == Rect{2, 2, 6, 6});
}

TEST_CASE("rect marquee: press-drag-release commits the combined mask once") {
    SelectionGesture g;
    CHECK_FALSE(g.active());
    g.beginDrag(Kind::Rect, SelectOp::Replace, {2, 3});
    CHECK(g.phase() == Phase::Dragging);
    CHECK(g.previewDirty());
    g.clearPreviewDirty();
    g.dragTo({10, 7}, false, false);
    CHECK(g.previewDirty());

    const std::optional<Selection> out = g.finish(Selection{}, 16, 16);
    REQUIRE(out.has_value());
    CHECK(*out == Selection::rectangle(16, 16, {2, 3, 8, 4}));
    CHECK_FALSE(g.active()); // finish() resets to Idle
}

TEST_CASE("rect marquee: boolean ops combine with the base selection") {
    const Selection base = Selection::rectangle(16, 16, {0, 0, 8, 16});
    SelectionGesture g;

    g.beginDrag(Kind::Rect, SelectOp::Add, {6, 0});
    g.dragTo({12, 16}, false, false);
    const auto added = g.finish(base, 16, 16);
    REQUIRE(added.has_value());
    CHECK(added->at(2, 2) == 255);  // base survives
    CHECK(added->at(10, 2) == 255); // the new rect joined
    CHECK(added->at(14, 2) == 0);

    g.beginDrag(Kind::Rect, SelectOp::Subtract, {4, 0});
    g.dragTo({16, 16}, false, false);
    const auto cut = g.finish(base, 16, 16);
    REQUIRE(cut.has_value());
    CHECK(cut->at(2, 2) == 255);
    CHECK(cut->at(6, 2) == 0); // subtracted

    g.beginDrag(Kind::Rect, SelectOp::Intersect, {4, 4}); // overlap = [4,8) x [4,12)
    g.dragTo({12, 12}, false, false);
    const auto isect = g.finish(base, 16, 16);
    REQUIRE(isect.has_value());
    CHECK(isect->at(5, 5) == 255);
    CHECK(isect->at(2, 2) == 0);
    CHECK(isect->at(10, 5) == 0);
}

TEST_CASE("commit guards: click-away, all-zero results, and no-op combines") {
    const Selection base = Selection::rectangle(16, 16, {2, 2, 4, 4});
    SelectionGesture g;

    // A plain click (degenerate rect) with Replace deselects a non-empty base...
    g.beginDrag(Kind::Rect, SelectOp::Replace, {10, 10});
    const auto deselect = g.finish(base, 16, 16);
    REQUIRE(deselect.has_value());
    CHECK(deselect->isEmpty());

    // ...is nothing on an already-empty base...
    g.beginDrag(Kind::Rect, SelectOp::Replace, {10, 10});
    CHECK_FALSE(g.finish(Selection{}, 16, 16).has_value());

    // ...and a click with a combining op never clobbers the selection.
    g.beginDrag(Kind::Rect, SelectOp::Add, {10, 10});
    CHECK_FALSE(g.finish(base, 16, 16).has_value());

    // Subtracting everything deselects (an all-zero active mask must never land).
    g.beginDrag(Kind::Rect, SelectOp::Subtract, {0, 0});
    g.dragTo({16, 16}, false, false);
    const auto gone = g.finish(base, 16, 16);
    REQUIRE(gone.has_value());
    CHECK(gone->isEmpty());

    // Subtracting outside the base changes nothing: not worth an undo step.
    g.beginDrag(Kind::Rect, SelectOp::Subtract, {10, 10});
    g.dragTo({14, 14}, false, false);
    CHECK_FALSE(g.finish(base, 16, 16).has_value());

    // Cancel discards everything.
    g.beginDrag(Kind::Rect, SelectOp::Replace, {0, 0});
    g.dragTo({16, 16}, false, false);
    g.cancel();
    CHECK_FALSE(g.active());
    CHECK_FALSE(g.previewDirty());
}

TEST_CASE("shaping modifiers re-arm: a press-time op chooser doesn't constrain the drag") {
    const Selection base = Selection::rectangle(32, 32, {0, 0, 8, 8});
    SelectionGesture g;

    // Shift chose Add at press and stays held: the added rect must NOT be forced square.
    g.beginDrag(Kind::Rect, SelectOp::Add, {10, 10}, /*shiftAtPress=*/true, false);
    g.dragTo({26, 18}, /*shiftDown=*/true, false);
    const auto added = g.finish(base, 32, 32);
    REQUIRE(added.has_value());
    CHECK(added->at(25, 17) == 255); // 16x8: the far corner is in
    CHECK(added->at(25, 19) == 0);   // a forced 16x16 square would have covered this

    // Release Shift mid-drag and press it again: now it means square.
    g.beginDrag(Kind::Rect, SelectOp::Add, {10, 10}, true, false);
    g.dragTo({26, 18}, true, false);
    g.dragTo({26, 18}, false, false); // released: re-armed
    g.dragTo({26, 18}, true, false);  // pressed again: constrain
    const auto squared = g.finish(base, 32, 32);
    REQUIRE(squared.has_value());
    CHECK(squared->at(25, 19) == 255); // 16x16 square now

    // Same rule for Alt (Intersect at press vs from-centre during the drag).
    g.beginDrag(Kind::Rect, SelectOp::Intersect, {2, 2}, false, /*altAtPress=*/true);
    g.dragTo({4, 4}, false, true); // still held: NOT from-centre -> rect (2,2)-(4,4)
    const auto held = g.finish(base, 32, 32);
    REQUIRE(held.has_value());
    CHECK(held->at(0, 0) == 0); // from-centre would have reached (0,0)
    CHECK(held->at(3, 3) == 255);

    g.beginDrag(Kind::Rect, SelectOp::Intersect, {2, 2}, false, true);
    g.dragTo({4, 4}, false, false); // released: re-armed
    g.dragTo({4, 4}, false, true);  // pressed again: from-centre -> (0,0)-(4,4)
    const auto centred = g.finish(base, 32, 32);
    REQUIRE(centred.has_value());
    CHECK(centred->at(0, 0) == 255);
}

TEST_CASE("free lasso: the path decimates and commits as a polygon") {
    SelectionGesture g;
    g.beginDrag(Kind::FreeLasso, SelectOp::Replace, {1, 1});
    g.dragTo({1.1, 1.1}, false, false); // < the decimation step from (1,1): dropped
    CHECK(g.points().size() == 1);
    g.dragTo({13, 1}, false, false);
    g.dragTo({1, 13}, false, false);
    CHECK(g.points().size() == 3);

    const auto out = g.finish(Selection{}, 16, 16);
    REQUIRE(out.has_value());
    CHECK(*out == Selection::polygon(16, 16, {{1, 1}, {13, 1}, {1, 13}}));

    // A click (too few points to enclose anything) on an empty base commits nothing.
    g.beginDrag(Kind::FreeLasso, SelectOp::Replace, {5, 5});
    g.dragTo({6, 5}, false, false);
    CHECK_FALSE(g.finish(Selection{}, 16, 16).has_value());
}

TEST_CASE("catmullRomSmooth: passthrough, interpolation, collinearity, arc-length density") {
    // Fewer than 3 points: nothing to round (a point or single segment is already "smooth").
    CHECK(catmullRomSmooth({}, 2.0).empty());
    CHECK(catmullRomSmooth({{1, 2}}, 2.0).size() == 1);
    CHECK(catmullRomSmooth({{1, 2}, {3, 4}}, 2.0).size() == 2);

    // Right-angle path; each segment is length 4, spacing 2 -> 2 samples per segment.
    const std::vector<Vec2> tri = {{0, 0}, {4, 0}, {4, 4}};
    const std::vector<Vec2> s = catmullRomSmooth(tri, 2.0);
    CHECK(s.size() == 5);
    // The curve passes through every control point: segment boundaries land on the inputs.
    CHECK(s.front() == tri[0]);
    CHECK(s[2].x == doctest::Approx(4.0)); // end of segment 0 == tri[1]
    CHECK(s[2].y == doctest::Approx(0.0));
    CHECK(s.back() == tri[2]);

    // A straight run stays straight: centripetal smoothing only adds points along the line, no bulge.
    for (const Vec2& p : catmullRomSmooth({{0, 0}, {3, 0}, {6, 0}, {9, 0}}, 1.5)) {
        CHECK(p.y == doctest::Approx(0.0));
        CHECK(p.x >= -0.001);
        CHECK(p.x <= 9.001);
    }

    // Arc-length sampling: at one spacing a long segment yields more samples than a short one (the
    // fix for fast-drag faceting -- a fixed per-segment count under-sampled long segments).
    const std::size_t few = catmullRomSmooth({{0, 0}, {2, 0}, {4, 0}}, 1.0).size();
    const std::size_t many = catmullRomSmooth({{0, 0}, {40, 0}, {80, 0}}, 1.0).size();
    CHECK(many > few);
}

TEST_CASE("laplacianSmooth: pins endpoints, keeps straight runs, pulls jitter in") {
    CHECK(laplacianSmooth({{0, 0}, {1, 1}}, 2, 0.5).size() == 2); // < 3 points: passthrough

    const std::vector<Vec2> line = {{0, 0}, {1, 0}, {2, 0}, {3, 0}};
    const std::vector<Vec2> flat = laplacianSmooth(line, 3, 0.5);
    REQUIRE(flat.size() == line.size());
    CHECK(flat.front() == line.front()); // endpoints pinned
    CHECK(flat.back() == line.back());
    for (const Vec2& p : flat)
        CHECK(p.y == doctest::Approx(0.0)); // a straight run stays straight

    // A single off-axis spike is pulled toward the line between its (pinned) neighbours.
    const std::vector<Vec2> spike = {{0, 0}, {1, 4}, {2, 0}};
    const std::vector<Vec2> pulled = laplacianSmooth(spike, 1, 0.5);
    CHECK(pulled.front() == spike.front());
    CHECK(pulled.back() == spike.back());
    CHECK(pulled[1].x == doctest::Approx(1.0));
    CHECK(pulled[1].y == doctest::Approx(2.0)); // 4 + 0.5*(0 - 4)
}

TEST_CASE("free-lasso smoothing toggle drives pathPoints (freehand only)") {
    SelectionGesture g;
    g.beginDrag(Kind::FreeLasso, SelectOp::Replace, {1, 1});
    g.dragTo({13, 1}, false, false);
    g.dragTo({1, 13}, false, false);
    REQUIRE(g.points().size() == 3);
    CHECK_FALSE(g.smoothing());
    CHECK(g.pathPoints() == g.points()); // off: the raw path verbatim

    g.setSmoothing(true);
    CHECK(g.smoothing());
    CHECK(g.pathPoints().size() > g.points().size()); // on: a denser, rounded path
    CHECK(g.pathPoints().front() == g.points().front()); // still anchored to the first sample
    CHECK(g.pathPoints().back() == g.points().back());   // ... and the last

    // The polygonal lasso is never smoothed, even with the toggle on.
    SelectionGesture poly;
    poly.setSmoothing(true);
    poly.beginPoly(SelectOp::Replace, {1, 1});
    poly.addVertex({13, 1});
    poly.addVertex({1, 13});
    CHECK(poly.pathPoints() == poly.points());
}

TEST_CASE("poly lasso: click-click-close state machine") {
    SelectionGesture g;
    g.beginPoly(SelectOp::Replace, {1, 1});
    CHECK(g.phase() == Phase::Placing);
    CHECK_FALSE(g.shouldClose({1, 1}, 2.0, false)); // nothing to enclose yet
    g.addVertex({13, 1});
    CHECK_FALSE(g.shouldClose({1.5, 1.5}, 2.0, false)); // still only two vertices
    g.addVertex({13, 13});
    g.addVertex({1, 13});
    CHECK(g.shouldClose({1.5, 1.5}, 2.0, false));        // near the first vertex
    CHECK_FALSE(g.shouldClose({7, 7}, 2.0, false));      // mid-shape: keep placing
    CHECK(g.shouldClose({1.2, 12.8}, 2.0, true));        // double-click beside the last vertex
    CHECK_FALSE(g.shouldClose({7, 7}, 2.0, true));       // a far double-click does not close

    const auto out = g.finish(Selection{}, 16, 16);
    REQUIRE(out.has_value());
    CHECK(*out == Selection::polygon(16, 16, {{1, 1}, {13, 1}, {13, 13}, {1, 13}}));
}

TEST_CASE("poly lasso: Shift snaps the new segment to 15 degree increments") {
    SelectionGesture g;
    g.beginPoly(SelectOp::Replace, {0, 0});
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    constexpr double len = 10.0;
    // A segment ~10 deg above horizontal with Shift snaps UP to the nearest 15 deg multiple (a 5 deg
    // step would instead have kept 10 deg) -- the direction quantises, the length is preserved.
    g.addVertex({len * std::cos(10.0 * kDeg), len * std::sin(10.0 * kDeg)}, /*shiftDown=*/true);
    REQUIRE(g.points().size() == 2);
    CHECK(g.points().back().x == doctest::Approx(len * std::cos(15.0 * kDeg)));
    CHECK(g.points().back().y == doctest::Approx(len * std::sin(15.0 * kDeg)));
    // Without Shift the raw click is kept verbatim.
    g.addVertex({20.0, 7.0}, /*shiftDown=*/false);
    CHECK(g.points().back().x == doctest::Approx(20.0));
    CHECK(g.points().back().y == doctest::Approx(7.0));
}

TEST_CASE("preview: marquees show the combined mask, lassos a rubber-band stroke") {
    const Selection base = Selection::rectangle(16, 16, {0, 0, 4, 4});
    SelectionGesture g;

    // Subtract previews the live result, not the raw shape.
    g.beginDrag(Kind::Rect, SelectOp::Subtract, {0, 0});
    g.dragTo({2, 16}, false, false);
    const Selection sub = g.preview(base, 16, 16);
    CHECK(sub.at(1, 1) == 0);   // already cut away in the preview
    CHECK(sub.at(3, 1) == 255); // the rest of the base still shows
    g.cancel();

    // The poly lasso previews its placed segments + the rubber segment to the cursor, on top of
    // the base; nothing is filled until the close.
    g.beginPoly(SelectOp::Replace, {8, 2});
    g.addVertex({14, 2});
    g.moveTo({14, 9});
    CHECK(g.previewDirty());
    const Selection band = g.preview(base, 16, 16);
    CHECK(band.at(1, 1) == 255);  // the base's own mask still shows
    CHECK(band.at(11, 2) == 255); // on the placed segment
    CHECK(band.at(14, 6) == 255); // on the rubber segment to the cursor
    CHECK(band.at(11, 6) == 0);   // the enclosed area is NOT filled yet

    // Zoomed out the band thickens in doc px (so it stays ~1 screen px wide).
    CHECK(band.at(11, 3) == 0); // 1-px stroke: the next row is clear
    const Selection thick = g.preview(base, 16, 16, 3.0);
    CHECK(thick.at(11, 3) == 255); // 3-px stroke: the stamp reaches it
    CHECK(thick.at(11, 6) == 0);   // still no fill
}

// ---- S16-i: SelectionMoveGesture -------------------------------------------------------------

TEST_CASE("selection move: a drag translates the mask by whole pixels, sub-pixel motion is free") {
    const Selection base = Selection::rectangle(16, 16, {2, 2, 4, 4});
    mosaic::ui::SelectionMoveGesture g;

    g.begin(base, {4.0, 4.0});
    CHECK(g.active());
    CHECK(g.dragging());
    CHECK_FALSE(g.moved());
    CHECK(g.current() == base); // zero offset: the mask is untouched

    CHECK_FALSE(g.dragTo({4.2, 4.1})); // rounds to (0,0): no rebuild
    CHECK_FALSE(g.moved());
    CHECK(g.dragTo({7.0, 4.0})); // +3 in x
    CHECK(g.offsetX() == 3);
    CHECK(g.offsetY() == 0);
    CHECK(g.current() == base.translated(3, 0));
    CHECK_FALSE(g.dragTo({6.8, 4.2})); // still rounds to +3

    const auto out = g.finish();
    REQUIRE(out.has_value());
    CHECK(*out == base.translated(3, 0));
    CHECK_FALSE(g.active()); // finish() resets
}

TEST_CASE("selection move: a click that never moved commits nothing") {
    const Selection base = Selection::rectangle(16, 16, {2, 2, 4, 4});
    mosaic::ui::SelectionMoveGesture g;
    g.begin(base, {4.0, 4.0});
    g.dragTo({4.3, 3.8}); // inside the rounding slop
    CHECK_FALSE(g.finish().has_value());
    CHECK_FALSE(g.active());
}

TEST_CASE("selection move: cancel drops the gesture, leaving the caller to restore the mask") {
    const Selection base = Selection::rectangle(16, 16, {2, 2, 4, 4});
    mosaic::ui::SelectionMoveGesture g;
    g.begin(base, {4.0, 4.0});
    g.dragTo({9.0, 4.0});
    g.cancel();
    CHECK_FALSE(g.active());
    CHECK_FALSE(g.moved());
    CHECK(g.current().isEmpty()); // nothing to preview once cancelled
}

TEST_CASE("selection move always translates the BASE, so a drag over an edge and back is lossless") {
    const Selection base = Selection::rectangle(16, 16, {0, 4, 4, 4}); // flush left
    mosaic::ui::SelectionMoveGesture g;
    g.begin(base, {2.0, 6.0});

    g.dragTo({0.0, 6.0}); // -2: half the rect is off-canvas
    CHECK(g.current().bounds()->w == 2);
    g.dragTo({-10.0, 6.0}); // way off: nothing left
    CHECK(g.current().isEmpty());
    g.dragTo({2.0, 6.0}); // back to the anchor
    CHECK(g.offsetX() == 0);
    CHECK(g.current() == base); // every pixel returned -- the base was never eroded

    CHECK_FALSE(g.finish().has_value()); // net zero: no undo step
}

TEST_CASE("selection nudge: a session accumulates whole-pixel steps from one cached base") {
    const Selection base = Selection::rectangle(16, 16, {2, 2, 4, 4});
    mosaic::ui::SelectionMoveGesture g;

    g.beginNudge(base);
    CHECK(g.active());
    CHECK_FALSE(g.dragging()); // no anchor: dragTo is inert
    CHECK_FALSE(g.dragTo({9.0, 9.0}));
    CHECK_FALSE(g.moved());

    g.nudge(1, 0);
    g.nudge(1, 0);
    g.nudge(0, 10); // Shift-Down
    CHECK(g.offsetX() == 2);
    CHECK(g.offsetY() == 10);
    CHECK(g.current() == base.translated(2, 10));

    const auto out = g.finish();
    REQUIRE(out.has_value());
    CHECK(*out == base.translated(2, 10));
}

TEST_CASE("selection nudge: a burst out past an edge and back keeps every pixel") {
    const Selection base = Selection::rectangle(16, 16, {0, 4, 4, 4}); // flush left
    mosaic::ui::SelectionMoveGesture g;
    g.beginNudge(base);
    for (int i = 0; i < 6; ++i)
        g.nudge(-1, 0); // walk it off the left edge entirely
    CHECK(g.current().isEmpty());
    for (int i = 0; i < 6; ++i)
        g.nudge(1, 0); // ... and back
    CHECK(g.current() == base);
}

TEST_CASE("selection move: nudging a mask entirely off the canvas commits no-selection") {
    const Selection base = Selection::rectangle(16, 16, {0, 4, 4, 4});
    mosaic::ui::SelectionMoveGesture g;
    g.beginNudge(base);
    g.nudge(-10, 0);
    const auto out = g.finish();
    REQUIRE(out.has_value()); // it MOVED, so there is a step to push...
    CHECK(out->isEmpty());    // ... and what it pushes is "no selection"
}

TEST_CASE("selection nudge: a diagonal step is one step, and opposing arrows cancel") {
    const Selection base = Selection::rectangle(16, 16, {4, 4, 4, 4});
    mosaic::ui::SelectionMoveGesture g;
    g.beginNudge(base);

    // Left + Down held together -- one diagonal nudge, not a nudge per axis. (The canvas reads the
    // whole arrow-key state per keydown, because the window system only auto-repeats the last key.)
    g.nudge(-1, 1);
    CHECK(g.offsetX() == -1);
    CHECK(g.offsetY() == 1);
    CHECK(g.current() == base.translated(-1, 1));

    g.nudge(-10, 10); // Shift-held diagonal
    CHECK(g.offsetX() == -11);
    CHECK(g.offsetY() == 11);

    // Opposing arrows sum to zero on that axis; the caller declines to push a no-op step, but the
    // gesture itself must simply accumulate honestly.
    g.nudge(11, 0);
    CHECK(g.offsetX() == 0);
    CHECK(g.offsetY() == 11);
    CHECK(g.current() == base.translated(0, 11));
}
