#include "ui/shape_gesture.hpp"
#include "ui/widgets.hpp" // ui::Dial -- the angle control the designer edits every angle on

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <variant>
#include <vector>

#include "core/vector/flatten.hpp"

using namespace mosaic::ui;
using mosaic::common::Vec2;
namespace vec = mosaic::core::vec;
using doctest::Approx;

namespace {
// The Shape tool authors a FILL in the foreground colour and nothing else (S26-c) -- there is no
// paint mode and no second colour left to choose.
ShapeOptions fillOpts() {
    ShapeOptions o;
    o.foreground = {1, 0, 0, 1};
    return o;
}
// A pre-S26-c object: a fill UNDER a stroke, the shape the retired "Fill + Outline" paint mode
// authored. Nothing writes these any more, but every document that already contains one must keep
// loading, rendering and editing exactly as it did -- which is what the "legacy" cases below pin.
vec::Object legacyStrokedRect() {
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::RectShape::uniform({20, 20}, 0)};
    o.fill = vec::SolidPaint{mosaic::common::ColorF{1, 0, 0, 1}};
    o.stroke.enabled = true;
    o.stroke.width = 4.0;
    o.stroke.paint = vec::SolidPaint{mosaic::common::ColorF{0, 1, 0, 1}};
    return o;
}
}  // namespace

TEST_CASE("shapeKindFor maps the shape tools and rejects the rest") {
    CHECK(shapeKindFor(ToolId::RectShape) == ShapeKind::Rect);
    CHECK(shapeKindFor(ToolId::EllipseShape) == ShapeKind::Ellipse);
    CHECK(shapeKindFor(ToolId::PolygonShape) == ShapeKind::Polygon);
    CHECK(shapeKindFor(ToolId::StarShape) == ShapeKind::Star);
    CHECK(shapeKindFor(ToolId::LineShape) == ShapeKind::Line);
    CHECK_FALSE(shapeKindFor(ToolId::Move).has_value());
    CHECK_FALSE(shapeKindFor(ToolId::Brush).has_value());
}

TEST_CASE("rect: size lives in the params, placement centres the box") {
    const auto d = buildShapeDraft(ShapeKind::Rect, {10, 20}, {30, 50}, false, false, fillOpts());
    REQUIRE(d.has_value());
    const auto* shape = std::get_if<vec::ParametricShape>(&d->object.geometry);
    REQUIRE(shape != nullptr);
    const auto* rect = std::get_if<vec::RectShape>(shape);
    REQUIRE(rect != nullptr);
    CHECK(rect->size.x == Approx(20.0));  // 30-10
    CHECK(rect->size.y == Approx(30.0));  // 50-20
    CHECK(d->placement.apply({0, 0}).x == Approx(20.0));  // box centre = (20,35)
    CHECK(d->placement.apply({0, 0}).y == Approx(35.0));
    // A solid fill in the foreground colour, and NO stroke: the outline is a layer effect now.
    REQUIRE(std::holds_alternative<vec::SolidPaint>(d->object.fill));
    CHECK(std::get<vec::SolidPaint>(d->object.fill).color == fillOpts().foreground);
    CHECK_FALSE(d->object.stroke.enabled);
}

TEST_CASE("rect: dragging up-left still yields positive size and the right centre") {
    const auto d = buildShapeDraft(ShapeKind::Rect, {30, 50}, {10, 20}, false, false, fillOpts());
    REQUIRE(d.has_value());
    const auto& rect = std::get<vec::RectShape>(std::get<vec::ParametricShape>(d->object.geometry));
    CHECK(rect.size.x == Approx(20.0));
    CHECK(rect.size.y == Approx(30.0));
    CHECK(d->placement.apply({0, 0}).x == Approx(20.0));
    CHECK(d->placement.apply({0, 0}).y == Approx(35.0));
}

TEST_CASE("shift constrains a rect to a square (larger axis wins)") {
    const auto d = buildShapeDraft(ShapeKind::Rect, {0, 0}, {40, 10}, true, false, fillOpts());
    REQUIRE(d.has_value());
    const auto& rect = std::get<vec::RectShape>(std::get<vec::ParametricShape>(d->object.geometry));
    CHECK(rect.size.x == Approx(40.0));
    CHECK(rect.size.y == Approx(40.0));  // squared up to the dominant axis
}

TEST_CASE("alt anchors the box at the press point (grows symmetrically)") {
    const auto d = buildShapeDraft(ShapeKind::Ellipse, {100, 100}, {120, 110}, false, true, fillOpts());
    REQUIRE(d.has_value());
    const auto& ell = std::get<vec::EllipseShape>(std::get<vec::ParametricShape>(d->object.geometry));
    CHECK(ell.radii.x == Approx(20.0));  // |dx| = 20 is the half-extent -> full radius 20
    CHECK(ell.radii.y == Approx(10.0));
    CHECK(d->placement.apply({0, 0}).x == Approx(100.0));  // centre stays at the press point
    CHECK(d->placement.apply({0, 0}).y == Approx(100.0));
}

TEST_CASE("polygon and star take a regular radius from the smaller half-extent") {
    ShapeOptions po = fillOpts();
    po.sides = 6;
    const auto poly = buildShapeDraft(ShapeKind::Polygon, {0, 0}, {40, 20}, false, false, po);
    REQUIRE(poly.has_value());
    const auto& p = std::get<vec::PolygonShape>(std::get<vec::ParametricShape>(poly->object.geometry));
    CHECK(p.sides == 6);
    CHECK(p.radius == Approx(10.0));  // min(40,20)/2

    ShapeOptions so = fillOpts();
    so.points = 5;
    so.innerRatio = 0.4;
    const auto star = buildShapeDraft(ShapeKind::Star, {0, 0}, {60, 60}, false, false, so);
    REQUIRE(star.has_value());
    const auto& s = std::get<vec::StarShape>(std::get<vec::ParametricShape>(star->object.geometry));
    CHECK(s.points == 5);
    CHECK(s.outerRadius == Approx(30.0));
    CHECK(s.innerRadius == Approx(12.0));  // 30 * 0.4
}

TEST_CASE("line authors a stroke-only object centred on its midpoint") {
    ShapeOptions o;
    o.foreground = {0, 0, 1, 1};
    o.lineWidth = 5.0;
    o.cap = vec::LineCap::Round;
    const auto d = buildShapeDraft(ShapeKind::Line, {10, 10}, {30, 10}, false, false, o);
    REQUIRE(d.has_value());
    const auto& ln = std::get<vec::LineShape>(std::get<vec::ParametricShape>(d->object.geometry));
    CHECK(ln.a.x == Approx(-10.0));  // local, centred on midpoint (20,10)
    CHECK(ln.b.x == Approx(10.0));
    CHECK(d->placement.apply({0, 0}).x == Approx(20.0));
    CHECK(std::holds_alternative<vec::NoPaint>(d->object.fill));
    CHECK(d->object.stroke.enabled);
    CHECK(d->object.stroke.width == Approx(5.0));
    CHECK(d->object.stroke.cap == vec::LineCap::Round);
}

TEST_CASE("shift snaps a line to 45 degrees") {
    const auto d = buildShapeDraft(ShapeKind::Line, {0, 0}, {100, 10}, true, false, fillOpts());
    REQUIRE(d.has_value());
    const auto& ln = std::get<vec::LineShape>(std::get<vec::ParametricShape>(d->object.geometry));
    const Vec2 dir = ln.b - ln.a;  // near-horizontal drag snaps to exactly horizontal
    CHECK(dir.y == Approx(0.0));
    CHECK(dir.x > 0.0);
}

TEST_CASE("S26-c: every closed kind authors a fill and never a stroke") {
    ShapeOptions o = fillOpts();
    o.lineWidth = 40.0;  // the LINE's weight: it must not leak onto a closed shape
    o.sides = 6;
    o.points = 7;
    for (const ShapeKind k : {ShapeKind::Rect, ShapeKind::Ellipse, ShapeKind::Polygon,
                              ShapeKind::Star}) {
        const auto d = buildShapeDraft(k, {0, 0}, {40, 40}, false, false, o);
        REQUIRE(d.has_value());
        REQUIRE(std::holds_alternative<vec::SolidPaint>(d->object.fill));
        CHECK(std::get<vec::SolidPaint>(d->object.fill).color == o.foreground);
        CHECK_FALSE(d->object.stroke.enabled);  // the tool authors no outline at all any more
    }
    // The line is the one exception: no interior, so its stroke IS the shape.
    const auto line = buildShapeDraft(ShapeKind::Line, {0, 0}, {40, 0}, false, false, o);
    REQUIRE(line.has_value());
    CHECK(std::holds_alternative<vec::NoPaint>(line->object.fill));
    CHECK(line->object.stroke.enabled);
    CHECK(line->object.stroke.width == Approx(40.0));
    // ... and it stays the plain Solid line: the retired Hollow / Outlined modes are never authored.
    const auto& ln = std::get<vec::LineShape>(std::get<vec::ParametricShape>(line->object.geometry));
    CHECK(ln.paint == vec::LineShape::Paint::Solid);
}

TEST_CASE("pixel snapping rounds the box to whole pixels (and off preserves fractions)") {
    ShapeOptions snap = fillOpts();  // snapToPixel defaults on
    const auto d = buildShapeDraft(ShapeKind::Rect, {10.3, 20.8}, {30.4, 49.6}, false, false, snap);
    REQUIRE(d.has_value());
    const auto& rect = std::get<vec::RectShape>(std::get<vec::ParametricShape>(d->object.geometry));
    CHECK(rect.size.x == Approx(20.0));  // round(30.4)-round(10.3) = 30-10
    CHECK(rect.size.y == Approx(29.0));  // round(49.6)-round(20.8) = 50-21
    CHECK(d->placement.apply({0, 0}).x == Approx(20.0));   // centre = (round(10.3)+round(30.4))/2
    CHECK(d->placement.apply({0, 0}).y == Approx(35.5));   // (21+50)/2

    ShapeOptions raw = snap;
    raw.snapToPixel = false;
    const auto r = buildShapeDraft(ShapeKind::Rect, {10.3, 20.8}, {30.4, 49.6}, false, false, raw);
    REQUIRE(r.has_value());
    const auto& rr = std::get<vec::RectShape>(std::get<vec::ParametricShape>(r->object.geometry));
    CHECK(rr.size.x == Approx(20.1));  // 30.4-10.3, unsnapped
    CHECK(rr.size.y == Approx(28.8));  // 49.6-20.8
}

TEST_CASE("a sub-pixel drag authors nothing") {
    CHECK_FALSE(buildShapeDraft(ShapeKind::Rect, {5, 5}, {5.4, 5.2}, false, false, fillOpts()));
    CHECK_FALSE(buildShapeDraft(ShapeKind::Line, {5, 5}, {5.3, 5.1}, false, false, fillOpts()));
}

TEST_CASE("the authored rect actually rasterises where the placement puts it") {
    // End-to-end sanity: place a 20x20 fill rect via the gesture, map local->doc with the
    // placement, and confirm contentBounds lands at the box the drag described.
    const auto d = buildShapeDraft(ShapeKind::Rect, {10, 10}, {30, 30}, false, false, fillOpts());
    REQUIRE(d.has_value());
    const auto b = vec::contentBounds(d->object);  // local-space bounds (no stroke)
    REQUIRE(b.has_value());
    const Vec2 tl = d->placement.apply({b->x, b->y});
    CHECK(tl.x == Approx(10.0));
    CHECK(tl.y == Approx(10.0));
}

// ---- Select-to-edit bridge (S26-b §7.1) -------------------------------------------------------

TEST_CASE("shapeKindOf maps geometry to a kind (nullopt for a path)") {
    for (auto k : {ShapeKind::Rect, ShapeKind::Ellipse, ShapeKind::Polygon, ShapeKind::Star,
                   ShapeKind::Line}) {
        const auto d = buildShapeDraft(k, {0, 0}, {30, 20}, false, false, fillOpts());
        REQUIRE(d.has_value());
        CHECK(shapeKindOf(d->object) == k);
    }
    vec::Object path;
    path.geometry = vec::Path{};
    CHECK_FALSE(shapeKindOf(path).has_value());
}

TEST_CASE("readShapeOptions reflects the hot parameter (and a line's weight + cap)") {
    ShapeOptions src = fillOpts();
    src.points = 7;
    src.innerRatio = 0.3;
    const auto star = buildShapeDraft(ShapeKind::Star, {0, 0}, {40, 40}, false, false, src);
    REQUIRE(star.has_value());
    ShapeOptions read;  // defaults; readShapeOptions must NOT touch its colours
    readShapeOptions(star->object, read);
    CHECK(read.points == 7);
    CHECK(read.innerRatio == Approx(0.3).epsilon(0.02));
    CHECK(read.foreground.r == Approx(0.0));  // colours stay the swatch's, never the shape's

    ShapeOptions lineSrc;
    lineSrc.lineWidth = 11.0;
    lineSrc.cap = vec::LineCap::Square;
    const auto line = buildShapeDraft(ShapeKind::Line, {0, 0}, {30, 0}, false, false, lineSrc);
    REQUIRE(line.has_value());
    ShapeOptions lineRead;
    readShapeOptions(line->object, lineRead);
    CHECK(lineRead.lineWidth == Approx(11.0));
    CHECK(lineRead.cap == vec::LineCap::Square);
}

TEST_CASE("readShapeOptions reads no paint state off a legacy stroked shape") {
    // A pre-S26-c fill+stroke object has nothing left on the bar to reflect INTO: reading it must
    // leave the (fill-only) options untouched rather than resurrecting a paint mode.
    const ShapeOptions before;
    ShapeOptions read;
    readShapeOptions(legacyStrokedRect(), read);
    CHECK(read.lineWidth == Approx(before.lineWidth));  // 4.0 stroke width is NOT a line weight
    CHECK(read.cap == before.cap);
    CHECK(read.cornerRadius == Approx(0.0));
}

TEST_CASE("editedObject keeps size + colours, edits only the hot parameter") {
    const ShapeOptions o = fillOpts();  // Fill, fg red
    const auto star = buildShapeDraft(ShapeKind::Star, {0, 0}, {40, 40}, false, false, o);
    REQUIRE(star.has_value());
    const double outer0 =
        std::get<vec::StarShape>(std::get<vec::ParametricShape>(star->object.geometry)).outerRadius;

    ShapeOptions edit;  // a fresh snapshot (default colours) reflecting the shape, then nudged
    readShapeOptions(star->object, edit);
    edit.points = 9;
    const vec::Object e = editedObject(star->object, edit);
    const auto& s = std::get<vec::StarShape>(std::get<vec::ParametricShape>(e.geometry));
    CHECK(s.points == 9);
    CHECK(s.outerRadius == Approx(outer0));  // size preserved (not driven by the bar)
    REQUIRE(std::holds_alternative<vec::SolidPaint>(e.fill));
    CHECK(std::get<vec::SolidPaint>(e.fill).color == o.foreground);  // red kept, not recoloured
}

TEST_CASE("editedObject leaves a legacy stroke exactly as it found it") {
    // Nudging the corner radius of a pre-S26-c fill+stroke rect must not disturb its outline: the
    // bar has no paint controls left, so an unrelated edit can never "re-author" the paint.
    const vec::Object base = legacyStrokedRect();
    ShapeOptions edit;
    edit.cornerRadius = 5.0;
    edit.foreground = {0, 0, 1, 1};  // a swatch colour that must NOT bleed into the object
    const vec::Object e = editedObject(base, edit);
    const auto& r = std::get<vec::RectShape>(std::get<vec::ParametricShape>(e.geometry));
    CHECK(r.cornerRadius[0] == Approx(5.0));  // the hot parameter did change
    // ... and nothing about the paint did: same fill colour, same stroke, still enabled.
    CHECK(std::get<vec::SolidPaint>(e.fill).color == std::get<vec::SolidPaint>(base.fill).color);
    CHECK(e.stroke.enabled);
    CHECK(e.stroke.width == Approx(base.stroke.width));
    CHECK(std::get<vec::SolidPaint>(e.stroke.paint).color ==
          std::get<vec::SolidPaint>(base.stroke.paint).color);
}

TEST_CASE("recoloredObject: fill takes fg; a legacy shape's outline still takes bg") {
    const mosaic::common::ColorF red{1, 0, 0, 1}, green{0, 1, 0, 1};
    const auto rect = buildShapeDraft(ShapeKind::Rect, {0, 0}, {20, 20}, false, false, fillOpts());
    REQUIRE(rect.has_value());
    const vec::Object f = recoloredObject(rect->object, red, green);
    CHECK(std::get<vec::SolidPaint>(f.fill).color == red);  // fill is primary -> fg
    CHECK_FALSE(f.stroke.enabled);                          // recolouring never grows an outline

    // A pre-S26-c fill+stroke object keeps the old two-colour convention, so those shapes stay
    // recolourable from the swatch after the paint modes went away.
    const vec::Object b = recoloredObject(legacyStrokedRect(), red, green);
    CHECK(std::get<vec::SolidPaint>(b.fill).color == red);
    CHECK(std::get<vec::SolidPaint>(b.stroke.paint).color == green);

    // A lone outline (the retired "Outline" mode) is the primary element -> fg.
    vec::Object outlineOnly = legacyStrokedRect();
    outlineOnly.fill = vec::NoPaint{};
    const vec::Object s = recoloredObject(outlineOnly, red, green);
    CHECK(std::holds_alternative<vec::NoPaint>(s.fill));
    CHECK(std::get<vec::SolidPaint>(s.stroke.paint).color == red);
}

TEST_CASE("editedObject on a line touches only weight + cap") {
    ShapeOptions o;
    o.foreground = {0, 0, 1, 1};
    o.lineWidth = 5.0;
    const auto line = buildShapeDraft(ShapeKind::Line, {0, 0}, {20, 0}, false, false, o);
    REQUIRE(line.has_value());
    ShapeOptions edit;
    edit.lineWidth = 9.0;
    edit.cap = vec::LineCap::Round;
    const vec::Object e = editedObject(line->object, edit);
    CHECK(e.stroke.width == Approx(9.0));
    CHECK(e.stroke.cap == vec::LineCap::Round);
    CHECK(e.stroke.enabled);                            // still a stroke
    CHECK(std::holds_alternative<vec::NoPaint>(e.fill)); // lines never gain a fill here
    // The line's colour is the object's, not the (default black) swatch on `edit`.
    CHECK(std::get<vec::SolidPaint>(e.stroke.paint).color == o.foreground);
}

TEST_CASE("a legacy Outlined line keeps its border through an edit and a recolour") {
    // LineShape::paint / borderWidth are no longer authored, but they are still model state: a line
    // saved as Outlined must survive the bar's weight/cap edits and the colour swatch untouched.
    ShapeOptions o;
    o.foreground = {1, 0, 0, 1};
    o.lineWidth = 6.0;
    auto line = buildShapeDraft(ShapeKind::Line, {0, 0}, {30, 0}, false, false, o);
    REQUIRE(line.has_value());
    vec::Object legacy = line->object;  // ... then age it into an Outlined line by hand
    auto& ln = std::get<vec::LineShape>(std::get<vec::ParametricShape>(legacy.geometry));
    ln.paint = vec::LineShape::Paint::Outlined;
    ln.borderWidth = 2.0;
    legacy.fill = vec::SolidPaint{mosaic::common::ColorF{0, 0, 1, 1}};  // the border colour

    ShapeOptions edit;
    edit.lineWidth = 10.0;
    const vec::Object e = editedObject(legacy, edit);
    const auto& el = std::get<vec::LineShape>(std::get<vec::ParametricShape>(e.geometry));
    CHECK(e.stroke.width == Approx(10.0));                   // the weight edit landed
    CHECK(el.paint == vec::LineShape::Paint::Outlined);      // ... and the mode is untouched
    CHECK(el.borderWidth == Approx(2.0));
    CHECK(std::get<vec::SolidPaint>(e.fill).color.b == Approx(1.0));

    const vec::Object rc = recoloredObject(legacy, {0, 1, 0, 1}, {1, 1, 0, 1});
    CHECK(std::get<vec::SolidPaint>(rc.stroke.paint).color.g == Approx(1.0));  // fg -> line
    CHECK(std::get<vec::SolidPaint>(rc.fill).color.r == Approx(1.0));          // bg -> border
}

// ---- The wireframe preview (S26-c): outline while dragging, fill on release -------------------

TEST_CASE("shapeOutlinePolyline traces the drag box and closes the contour") {
    const auto d = buildShapeDraft(ShapeKind::Rect, {10, 20}, {30, 50}, false, false, fillOpts());
    REQUIRE(d.has_value());
    const std::vector<Vec2> wire = shapeOutlinePolyline(*d);
    REQUIRE(wire.size() >= 5);           // 4 corners + the repeated first point
    CHECK(wire.front().x == Approx(wire.back().x));  // closed: drawable as one open polyline
    CHECK(wire.front().y == Approx(wire.back().y));
    // The wireframe is in DOCUMENT space and sits exactly on the box the drag described.
    Vec2 lo = wire.front(), hi = wire.front();
    for (const Vec2& p : wire) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
    }
    CHECK(lo.x == Approx(10.0));
    CHECK(lo.y == Approx(20.0));
    CHECK(hi.x == Approx(30.0));
    CHECK(hi.y == Approx(50.0));
}

TEST_CASE("shapeOutlinePolyline: a line previews as its open centreline") {
    ShapeOptions o;
    o.lineWidth = 9.0;  // the weight must NOT widen the wireframe: it traces the centreline
    const auto d = buildShapeDraft(ShapeKind::Line, {10, 10}, {40, 10}, false, false, o);
    REQUIRE(d.has_value());
    const std::vector<Vec2> wire = shapeOutlinePolyline(*d);
    REQUIRE(wire.size() >= 2);
    CHECK(wire.front().x == Approx(10.0));
    CHECK(wire.back().x == Approx(40.0));
    for (const Vec2& p : wire)
        CHECK(p.y == Approx(10.0));  // open, on the centreline -- not a stroked outline
}

TEST_CASE("shapeOutlinePolyline: the tolerance follows the device transform") {
    // The same ellipse flattens finer when the layer is magnified on screen, so the wireframe never
    // reads coarser than the fill it is promising.
    const auto d = buildShapeDraft(ShapeKind::Ellipse, {0, 0}, {200, 200}, false, false, fillOpts());
    REQUIRE(d.has_value());
    const std::size_t at1x = shapeOutlinePolyline(*d).size();
    const std::size_t at8x =
        shapeOutlinePolyline(*d, mosaic::common::Affine2D::scaling(8.0, 8.0)).size();
    CHECK(at1x > 8);
    CHECK(at8x > at1x);
}

TEST_CASE("the wireframe and the committed fill come from ONE draft") {
    // The contract behind "outline while dragging, fill on release": the canvas draws
    // shapeOutlinePolyline(draft) each frame and, on release, hands the SAME draft to the host to
    // spawn the layer. So the wireframe's silhouette IS the committed object's silhouette -- and
    // the committed object is filled, though no frame of the drag ever showed a fill.
    const auto d = buildShapeDraft(ShapeKind::Star, {0, 0}, {60, 60}, false, false, fillOpts());
    REQUIRE(d.has_value());
    const std::vector<Vec2> wire = shapeOutlinePolyline(*d);
    REQUIRE(wire.size() > 2);
    const std::optional<mosaic::common::Affine2D> inv = d->placement.inverse();
    REQUIRE(inv.has_value());
    const auto box = vec::contentBounds(d->object);  // the SAME object the layer will carry
    REQUIRE(box.has_value());
    for (const Vec2& p : wire) {  // every wireframe point sits on the object's own bounds box
        const Vec2 local = inv->apply(p);
        CHECK(local.x >= box->x - 0.01);
        CHECK(local.x <= box->right() + 0.01);
        CHECK(local.y >= box->y - 0.01);
        CHECK(local.y <= box->bottom() + 0.01);
    }
    CHECK(std::holds_alternative<vec::SolidPaint>(d->object.fill));
    CHECK_FALSE(d->object.stroke.enabled);
}

TEST_CASE("shapeOutlinePolyline: nothing to trace yields nothing") {
    ShapeDraft empty;  // default-constructed: an empty Path, placed at the identity
    CHECK(shapeOutlinePolyline(empty).empty());
}

// ---- resizeShape (S26-b §7.1 resize-vs-transform) ---------------------------------------------

namespace {
const vec::RectShape& rectOf(const vec::Object& o) {
    return std::get<vec::RectShape>(std::get<vec::ParametricShape>(o.geometry));
}
}  // namespace

TEST_CASE("resizeShape: dragging a corner scales the size and pins the opposite corner") {
    // A 20x30 rect, fill-only, centred by its placement at (20,35); contentBounds = [-10,-15]..[10,15].
    const auto d = buildShapeDraft(ShapeKind::Rect, {10, 20}, {30, 50}, false, false, fillOpts());
    REQUIRE(d.has_value());
    // Drag the BR handle (2) out to doc (40,65): the box grows 1.5x on both axes.
    const auto r = resizeShape(d->object, d->placement, 2, {40, 65}, false, false);
    REQUIRE(r.has_value());
    CHECK(rectOf(r->object).size.x == Approx(30.0));  // 20 * 1.5
    CHECK(rectOf(r->object).size.y == Approx(45.0));  // 30 * 1.5
    // The TL anchor holds in document space; the dragged BR corner tracks the cursor.
    const Vec2 tl = r->placement.apply({-15, -22.5});  // new half-extents (15, 22.5)
    const Vec2 br = r->placement.apply({15, 22.5});
    CHECK(tl.x == Approx(10.0));
    CHECK(tl.y == Approx(20.0));
    CHECK(br.x == Approx(40.0));
    CHECK(br.y == Approx(65.0));
    CHECK(r->object.stroke.width == d->object.stroke.width);  // stroke stays uniform
}

TEST_CASE("resizeShape: an edge handle scales one axis only") {
    const auto d = buildShapeDraft(ShapeKind::Rect, {0, 0}, {20, 20}, false, false, fillOpts());
    REQUIRE(d.has_value());  // 20x20, centred at (10,10); box [-10,-10]..[10,10]
    // Right edge (5): drag to doc x=20 (local x=10 -> no change), then to x=30 (local 20) -> 1.5x width.
    const auto r = resizeShape(d->object, d->placement, 5, {30, 10}, false, false);
    REQUIRE(r.has_value());
    CHECK(rectOf(r->object).size.x == Approx(30.0));  // x grew
    CHECK(rectOf(r->object).size.y == Approx(20.0));  // y untouched
}

TEST_CASE("resizeShape: keepAspect locks the aspect ratio from the dominant axis") {
    const auto d = buildShapeDraft(ShapeKind::Rect, {0, 0}, {20, 20}, false, false, fillOpts());
    REQUIRE(d.has_value());
    // BR drag where x wants 1.5x (span -10..20) and y wants 1.25x (span -10..15): keepAspect picks
    // the dominant factor (1.5x) and applies it to both, so y is bumped from 25 up to 30.
    const auto r = resizeShape(d->object, d->placement, 2, {30, 25}, /*keepAspect=*/true, false);
    REQUIRE(r.has_value());
    CHECK(rectOf(r->object).size.x == Approx(30.0));
    CHECK(rectOf(r->object).size.y == Approx(30.0));
}

TEST_CASE("resizeShape: fromCenter grows symmetrically and keeps the centre fixed") {
    const auto d = buildShapeDraft(ShapeKind::Rect, {0, 0}, {20, 20}, false, false, fillOpts());
    REQUIRE(d.has_value());  // centred at (10,10); local half-extent 10
    // Drag BR to local (20,20): about the centre the half-extent doubles -> 2x size, both axes.
    const auto r = resizeShape(d->object, d->placement, 2, {30, 30}, false, /*fromCenter=*/true);
    REQUIRE(r.has_value());
    CHECK(rectOf(r->object).size.x == Approx(40.0));
    CHECK(rectOf(r->object).size.y == Approx(40.0));
    CHECK(r->placement.apply({0, 0}).x == Approx(10.0));  // centre held
    CHECK(r->placement.apply({0, 0}).y == Approx(10.0));
}

TEST_CASE("resizeShape: a polygon scales uniformly (single radius)") {
    ShapeOptions o = fillOpts();
    o.sides = 6;
    const auto d = buildShapeDraft(ShapeKind::Polygon, {0, 0}, {40, 40}, false, false, o);
    REQUIRE(d.has_value());
    const double r0 = std::get<vec::PolygonShape>(std::get<vec::ParametricShape>(d->object.geometry)).radius;
    // Drag a corner so the x-axis wants a larger factor than y: the uniform factor takes the max.
    const auto rs = resizeShape(d->object, d->placement, 2, {60, 50}, false, false);
    REQUIRE(rs.has_value());
    const auto& poly = std::get<vec::PolygonShape>(std::get<vec::ParametricShape>(rs->object.geometry));
    CHECK(poly.radius > r0);   // grew
    CHECK(poly.sides == 6);    // unchanged
}

TEST_CASE("resizeShape: clamps a collapsing drag and rejects a non-parametric object") {
    const auto d = buildShapeDraft(ShapeKind::Rect, {0, 0}, {20, 20}, false, false, fillOpts());
    REQUIRE(d.has_value());
    // Drag BR past the TL anchor: the size is clamped to a non-degenerate minimum, never flipped.
    const auto r = resizeShape(d->object, d->placement, 2, {-100, -100}, false, false);
    REQUIRE(r.has_value());
    CHECK(rectOf(r->object).size.x > 0.0);
    CHECK(rectOf(r->object).size.y > 0.0);
    // A path (no parametric kind) is out of scope.
    vec::Object path;
    path.geometry = vec::Path{};
    CHECK_FALSE(resizeShape(path, d->placement, 2, {0, 0}, false, false).has_value());
}

// ---- the angle DIAL's mapping (S26-c refinement) ----------------------------------------------
//
// Every angle in the shape designer is edited on a ui::Dial rather than a degree slider, because an
// angle is CYCLIC: a linear track has arbitrary endpoints and cannot express wrap-around. The whole
// mapping -- cursor offset -> screen angle, the Shift snap grid, and the fold back into range -- is
// pure, so it is pinned here without ever constructing a widget (the app's headless-only rule).

TEST_CASE("Dial::screenAngleAt measures clockwise from 12 o'clock and always lands in [0,360)") {
    // dx is screen-right, dy is screen-down (FLTK's frame), 0 = straight up.
    CHECK(Dial::screenAngleAt(0, -10) == Approx(0.0));
    CHECK(Dial::screenAngleAt(10, 0) == Approx(90.0));    // 3 o'clock
    CHECK(Dial::screenAngleAt(0, 10) == Approx(180.0));   // 6 o'clock
    CHECK(Dial::screenAngleAt(-10, 0) == Approx(270.0));  // 9 o'clock
    CHECK(Dial::screenAngleAt(10, -10) == Approx(45.0));
    CHECK(Dial::screenAngleAt(-1, -10) < 360.0);          // just west of 12 folds to ~354, not -6
    CHECK(Dial::screenAngleAt(-1, -10) > 350.0);
    // The mapping is scale-free: only the direction matters, so a drag far outside the knob still
    // points the needle rather than clamping.
    CHECK(Dial::screenAngleAt(3000, -3000) == Approx(45.0));
    for (int i = 0; i < 720; ++i) {  // two full turns of cursor directions, none escaping the range
        const double t = i * M_PI / 180.0;
        const double a = Dial::screenAngleAt(std::cos(t) * 40.0, std::sin(t) * 40.0);
        CHECK(a >= 0.0);
        CHECK(a < 360.0);
    }
}

TEST_CASE("Dial's Shift snap is the conventional 15 degrees, and it wraps rather than sticking") {
    CHECK(Dial::kDefaultSnapDeg == Approx(15.0));
    const double snap = Dial::kDefaultSnapDeg;
    CHECK(Dial::screenAngleAt(10, -10, snap) == Approx(45.0));  // already on the grid
    const auto atDeg = [&](double deg) {  // a cursor `deg` clockwise from 12 o'clock
        const double t = deg * M_PI / 180.0;
        return Dial::screenAngleAt(std::sin(t) * 40.0, -std::cos(t) * 40.0, snap);
    };
    CHECK(atDeg(5.0) == Approx(0.0));     // within half a grid step of 12: back to 0
    CHECK(atDeg(-5.0) == Approx(0.0));    // ...from the other side too, and NOT to 355
    CHECK(atDeg(10.0) == Approx(15.0));
    CHECK(atDeg(170.0) == Approx(165.0));
    // A snap that would push the value off the end of the turn folds back into range instead of
    // landing on a duplicate 360 endpoint.
    CHECK(atDeg(-175.0) == Approx(180.0));
    // Just past half a step west of 12 the grid's nearest stop is BEHIND the seam, so the value
    // wraps to 345 rather than clamping at 0 -- the wrap-around a linear degree slider cannot do.
    CHECK(atDeg(-8.0) == Approx(345.0));
    for (int i = 0; i < 360; ++i) {  // every snapped result is on the grid and inside the range
        const double t = i * M_PI / 180.0;
        const double a = Dial::screenAngleAt(std::cos(t), std::sin(t), snap);
        CHECK(a >= 0.0);
        CHECK(a < 360.0);
        CHECK(std::abs(a - std::round(a / snap) * snap) < 1e-9);
    }
}

TEST_CASE("wrapDialValue folds a full-turn range and clamps a partial one") {
    // A full turn wraps in BOTH directions, which is what stops a drag past the seam from sticking.
    CHECK(wrapDialValue(0.0, 0, 360) == Approx(0.0));
    CHECK(wrapDialValue(360.0, 0, 360) == Approx(0.0));
    CHECK(wrapDialValue(361.0, 0, 360) == Approx(1.0));
    CHECK(wrapDialValue(-1.0, 0, 360) == Approx(359.0));
    CHECK(wrapDialValue(-721.0, 0, 360) == Approx(359.0));
    CHECK(wrapDialValue(1080.5, 0, 360) == Approx(0.5));
    // The signed convention some hosts store (the blur filters' [-180, 180]) folds the same way.
    CHECK(wrapDialValue(190.0, -180, 180) == Approx(-170.0));
    CHECK(wrapDialValue(-190.0, -180, 180) == Approx(170.0));
    // A range that is NOT a full turn is a bounded control again: clamp, never wrap.
    CHECK(wrapDialValue(200.0, 0, 90) == Approx(90.0));
    CHECK(wrapDialValue(-5.0, 0, 90) == Approx(0.0));
}
