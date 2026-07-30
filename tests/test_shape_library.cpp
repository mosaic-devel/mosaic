#include "core/vector/corner.hpp"
#include "core/vector/flatten.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/to_path.hpp"
#include "ui/shape_designer.hpp"
#include "ui/shape_gesture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// The S26-c shape library: speech bubble / callout, arrow, ring-pie, cross, heart and
// chevron-banner. Three things are load-bearing for each of them and are pinned here:
//
//   1. the GEOMETRY -- the flattened outline is closed, non-degenerate, and TIGHT in the shape's
//      own size parameters (a figure that floats inside its own bounds makes every alignment,
//      snap and resize feel wrong, and is invisible in a screenshot);
//   2. the DRAG MAPPING -- the box a drag describes lands in the parameters, with the placement a
//      pure translation to the box centre (docs/vector-model.md §1);
//   3. the RESIZE -- scaling the size parameters keeps the opposite handle pinned in document
//      space, which is what makes a bbox drag feel like a resize rather than a jump.
//
// Plus the two bridges every kind must cross: shapeKindOf / readShapeOptions / editedObject, and
// "convert to path" tracing the same outline the flattener draws.

using namespace mosaic::ui;
using mosaic::common::Rect;
using mosaic::common::Vec2;
namespace vec = mosaic::core::vec;
using doctest::Approx;

namespace {

ShapeOptions fillOpts() {
    ShapeOptions o;
    o.foreground = {1, 0, 0, 1};
    return o;
}

const vec::ParametricShape& shapeOf(const vec::Object& o) {
    return std::get<vec::ParametricShape>(o.geometry);
}

Rect boundsOf(const vec::Contours& cs) {
    bool any = false;
    double lox = 0, loy = 0, hix = 0, hiy = 0;
    for (const vec::Contour& c : cs)
        for (const Vec2& p : c.points) {
            if (!any) {
                lox = hix = p.x;
                loy = hiy = p.y;
                any = true;
            } else {
                lox = std::min(lox, p.x);
                loy = std::min(loy, p.y);
                hix = std::max(hix, p.x);
                hiy = std::max(hiy, p.y);
            }
        }
    return any ? Rect{lox, loy, hix - lox, hiy - loy} : Rect{};
}

// Twice the signed area of a closed contour: its SIGN is the winding direction, which is what makes
// a ring's hole a hole under the NonZero rule.
double signedArea2(const vec::Contour& c) {
    double a = 0.0;
    const std::size_t n = c.points.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2 p = c.points[i], q = c.points[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }
    return a;
}

vec::Contours flattenOf(const vec::ParametricShape& s) {
    return vec::flatten(vec::Geometry{s});
}

// Every contour closed, with enough points to enclose area, and none of them NaN.
void checkSaneContours(const vec::Contours& cs, int minContours = 1) {
    REQUIRE(static_cast<int>(cs.size()) >= minContours);
    for (const vec::Contour& c : cs) {
        CHECK(c.closed);
        CHECK(c.points.size() >= 3);
        CHECK(std::abs(signedArea2(c)) > 1e-6);
        for (const Vec2& p : c.points) {
            CHECK(std::isfinite(p.x));
            CHECK(std::isfinite(p.y));
        }
    }
}

}  // namespace

// ---- the drag box -> parameters mapping -------------------------------------------------------

TEST_CASE("every library kind maps the drag box into its own size parameters") {
    const auto box = [](ShapeKind k) { return buildShapeDraft(k, {10, 20}, {110, 80}, false, false,
                                                              fillOpts()); };
    // 100 x 60 box centred at (60, 50).
    for (ShapeKind k : {ShapeKind::Callout, ShapeKind::Arrow, ShapeKind::Ring, ShapeKind::Cross,
                        ShapeKind::Heart, ShapeKind::Banner}) {
        const auto d = box(k);
        REQUIRE(d.has_value());
        CHECK(d->placement.apply({0, 0}).x == Approx(60.0));
        CHECK(d->placement.apply({0, 0}).y == Approx(50.0));
        CHECK(shapeKindOf(d->object) == k);
        // A closed library shape is a FILL and authors no stroke of its own (S26-c).
        CHECK(std::holds_alternative<vec::SolidPaint>(d->object.fill));
        CHECK_FALSE(d->object.stroke.enabled);
    }

    const auto callout = box(ShapeKind::Callout);
    const auto& c = std::get<vec::CalloutShape>(shapeOf(callout->object));
    CHECK(c.size.x == Approx(100.0));
    CHECK(c.size.y == Approx(60.0));
    CHECK(c.tailLength > 0.0);  // a bubble is born WITH a tail, else it is just a rounded box
    CHECK(c.tailWidth > 0.0);
    CHECK(c.cornerRadius > 0.0);

    const auto aDraft = box(ShapeKind::Arrow);
    const auto& a = std::get<vec::ArrowShape>(shapeOf(aDraft->object));
    CHECK(a.size.x == Approx(100.0));  // length along +x
    CHECK(a.size.y == Approx(60.0));   // head width
    CHECK(a.headRatio > 0.0);
    CHECK(a.headRatio < 1.0);

    const auto rDraft = box(ShapeKind::Ring);
    const auto& r = std::get<vec::RingShape>(shapeOf(rDraft->object));
    CHECK(r.radii.x == Approx(50.0));
    CHECK(r.radii.y == Approx(30.0));
    CHECK(r.innerRatio == Approx(0.5));  // the bar's "Inner %" is shared with the star

    const auto xDraft = box(ShapeKind::Cross);
    const auto& x = std::get<vec::CrossShape>(shapeOf(xDraft->object));
    CHECK(x.size.x == Approx(100.0));
    CHECK(x.size.y == Approx(60.0));

    const auto hDraft = box(ShapeKind::Heart);
    const auto& h = std::get<vec::HeartShape>(shapeOf(hDraft->object));
    CHECK(h.size.x == Approx(100.0));
    CHECK(h.size.y == Approx(60.0));

    const auto bDraft = box(ShapeKind::Banner);
    const auto& b = std::get<vec::BannerShape>(shapeOf(bDraft->object));
    CHECK(b.size.x == Approx(100.0));
    CHECK(b.size.y == Approx(60.0));
}

TEST_CASE("the ring takes the options bar's inner percentage") {
    ShapeOptions o = fillOpts();
    o.innerRatio = 0.25;
    const auto d = buildShapeDraft(ShapeKind::Ring, {0, 0}, {80, 80}, false, false, o);
    REQUIRE(d.has_value());
    CHECK(std::get<vec::RingShape>(shapeOf(d->object)).innerRatio == Approx(0.25));
}

// ---- the outlines themselves ------------------------------------------------------------------

TEST_CASE("speech bubble: the tail is spliced INTO the body, as one closed ring") {
    vec::CalloutShape c;
    c.size = {100, 60};
    c.cornerRadius = 0.0;
    c.tailAngle = M_PI / 2.0;  // straight down (y-down), so the reach is easy to state
    c.tailLength = 30.0;
    c.tailWidth = 20.0;
    const vec::Contours cs = flattenOf(vec::ParametricShape{c});
    // ONE contour: a bubble whose tail was merely overlaid would fill the same but show a seam
    // right across the body edge under any stroke.
    REQUIRE(cs.size() == 1);
    checkSaneContours(cs);
    const Rect b = boundsOf(cs);
    CHECK(b.x == Approx(-50.0));
    CHECK(b.right() == Approx(50.0));
    CHECK(b.y == Approx(-30.0));
    CHECK(b.bottom() == Approx(60.0));  // body half-height 30 + the 30 the tail reaches past it
    // The tip is a real vertex of the ring, and the two base points sit on the body edge.
    const auto& pts = cs.front().points;
    CHECK(std::any_of(pts.begin(), pts.end(), [](const Vec2& p) {
        return std::abs(p.x) < 1e-6 && p.y == Approx(60.0);
    }));
}

TEST_CASE("speech bubble: no tail length leaves the bare body; the ellipse body is an ellipse") {
    vec::CalloutShape c;
    c.size = {80, 40};
    c.tailLength = 0.0;
    const vec::Contours rectBody = flattenOf(vec::ParametricShape{c});
    REQUIRE(rectBody.size() == 1);
    CHECK(boundsOf(rectBody).w == Approx(80.0));
    CHECK(boundsOf(rectBody).h == Approx(40.0));

    c.body = vec::CalloutShape::Body::Ellipse;
    const vec::Contours ell = flattenOf(vec::ParametricShape{c});
    REQUIRE(ell.size() == 1);
    CHECK(boundsOf(ell).w == Approx(80.0).epsilon(0.01));
    CHECK(boundsOf(ell).h == Approx(40.0).epsilon(0.01));
}

TEST_CASE("thought balloon: the body plus a trail of shrinking puffs") {
    vec::CalloutShape c;
    c.size = {100, 60};
    c.tail = vec::CalloutShape::Tail::Bubbles;
    c.tailAngle = M_PI / 2.0;
    c.tailLength = 40.0;
    c.tailWidth = 18.0;
    c.bubbleCount = 3;
    const vec::Contours cs = flattenOf(vec::ParametricShape{c});
    REQUIRE(cs.size() == 4);  // the body + three puffs
    checkSaneContours(cs, 4);
    // The puffs march away from the body and shrink as they go.
    double prevArea = std::abs(signedArea2(cs[1]));
    for (std::size_t i = 2; i < cs.size(); ++i) {
        const double area = std::abs(signedArea2(cs[i]));
        CHECK(area < prevArea);
        prevArea = area;
    }
    CHECK(boundsOf(cs).bottom() > 30.0);  // they clear the body
}

TEST_CASE("speech bubble: the tail angle actually aims the tail") {
    const auto reachAt = [](double angle) {
        vec::CalloutShape c;
        c.size = {60, 60};
        c.tailAngle = angle;
        c.tailLength = 40.0;
        c.tailWidth = 16.0;
        return boundsOf(flattenOf(vec::ParametricShape{c}));
    };
    CHECK(reachAt(0.0).right() > 30.0);          // to the right
    CHECK(reachAt(M_PI).x < -30.0);              // to the left
    CHECK(reachAt(-M_PI / 2.0).y < -30.0);       // straight up (y-down)
    CHECK(reachAt(M_PI / 2.0).bottom() > 30.0);  // straight down
}

TEST_CASE("ring: a full annulus is two contours with OPPOSITE winding (the hole)") {
    vec::RingShape r;
    r.radii = {50, 50};
    r.innerRatio = 0.5;
    const vec::Contours cs = flattenOf(vec::ParametricShape{r});
    REQUIRE(cs.size() == 2);
    checkSaneContours(cs, 2);
    CHECK(signedArea2(cs[0]) * signedArea2(cs[1]) < 0.0);  // opposite -> NonZero cancels inside
    CHECK(std::abs(signedArea2(cs[0])) > std::abs(signedArea2(cs[1])));
    const Rect b = boundsOf(cs);
    CHECK(b.w == Approx(100.0).epsilon(0.01));
    CHECK(b.h == Approx(100.0).epsilon(0.01));
}

TEST_CASE("ring: no hole is a disc; a partial sweep with no hole is a pie slice") {
    vec::RingShape disc;
    disc.radii = {40, 40};
    disc.innerRatio = 0.0;
    CHECK(flattenOf(vec::ParametricShape{disc}).size() == 1);

    vec::RingShape pie = disc;
    pie.startAngle = 0.0;
    pie.endAngle = M_PI / 2.0;  // a quarter turn
    const vec::Contours cs = flattenOf(vec::ParametricShape{pie});
    REQUIRE(cs.size() == 1);
    checkSaneContours(cs);
    // A slice closes VIA THE CENTRE, so the origin is one of its vertices.
    const auto& pts = cs.front().points;
    CHECK(std::any_of(pts.begin(), pts.end(),
                      [](const Vec2& p) { return p.length() < 1e-9; }));
    const Rect b = boundsOf(cs);          // the quarter in +x/+y only
    CHECK(b.x == Approx(0.0).epsilon(0.01));
    CHECK(b.y == Approx(0.0).epsilon(0.01));

    vec::RingShape segment = pie;
    segment.innerRatio = 0.5;             // a ring SEGMENT: still one contour, but no centre vertex
    const vec::Contours seg = flattenOf(vec::ParametricShape{segment});
    REQUIRE(seg.size() == 1);
    CHECK(std::none_of(seg.front().points.begin(), seg.front().points.end(),
                       [](const Vec2& p) { return p.length() < 1e-9; }));
}

TEST_CASE("cross: tight in the box, arms sized by the ratio of the shorter side") {
    vec::CrossShape x;
    x.size = {120, 80};
    x.armRatio = 0.4;  // 0.4 * min(120, 80) = 32 thick
    x.cornerRadius = 0.0;
    const vec::Contours cs = flattenOf(vec::ParametricShape{x});
    REQUIRE(cs.size() == 1);
    checkSaneContours(cs);
    CHECK(cs.front().points.size() == 12);
    const Rect b = boundsOf(cs);
    CHECK(b.w == Approx(120.0));
    CHECK(b.h == Approx(80.0));
    // The vertical bar's half-width is armRatio * min(w,h) / 2 = 16.
    const auto& pts = cs.front().points;
    const double narrowest = std::abs(std::min_element(pts.begin(), pts.end(),
                                                       [](const Vec2& a, const Vec2& b2) {
                                                           return std::abs(a.x) < std::abs(b2.x);
                                                       })->x);
    CHECK(narrowest == Approx(16.0));
    // Rounding the corners pulls the outline in but never past the arms.
    x.cornerRadius = 6.0;
    const vec::Contours rounded = flattenOf(vec::ParametricShape{x});
    checkSaneContours(rounded);
    CHECK(rounded.front().points.size() > 12);
}

TEST_CASE("arrow: tight in the box, pointing along +x, shaft inside the head") {
    vec::ArrowShape a;
    a.size = {100, 60};
    a.shaftRatio = 0.5;  // 0.5 * halfHeight(30) = 15 half-thickness
    a.headRatio = 0.3;
    const vec::Contours cs = flattenOf(vec::ParametricShape{a});
    REQUIRE(cs.size() == 1);
    checkSaneContours(cs);
    CHECK(cs.front().points.size() == 7);
    const Rect b = boundsOf(cs);
    CHECK(b.w == Approx(100.0));
    CHECK(b.h == Approx(60.0));
    // The tip is on the +x axis at the box's right edge.
    const auto& pts = cs.front().points;
    CHECK(std::any_of(pts.begin(), pts.end(), [](const Vec2& p) {
        return p.x == Approx(50.0) && std::abs(p.y) < 1e-9;
    }));
    // The tail end is the shaft alone, so it is narrower than the head: shaftRatio * halfHeight.
    for (const Vec2& p : pts)
        if (p.x == Approx(-50.0)) CHECK(std::abs(p.y) == Approx(15.0));

    a.doubleHeaded = true;  // ten vertices, and now a tip at BOTH ends
    const vec::Contours two = flattenOf(vec::ParametricShape{a});
    REQUIRE(two.size() == 1);
    CHECK(two.front().points.size() == 10);
    CHECK(boundsOf(two).w == Approx(100.0));
}

TEST_CASE("heart: tight in its box and symmetric about the vertical axis") {
    vec::HeartShape h;
    h.size = {100, 90};
    const vec::Contours cs = flattenOf(vec::ParametricShape{h});
    REQUIRE(cs.size() == 1);
    checkSaneContours(cs);
    const Rect b = boundsOf(cs);
    // Tight: the shoulders peak exactly on the top edge, the tip lands on the bottom one.
    CHECK(b.x == Approx(-50.0).epsilon(0.01));
    CHECK(b.right() == Approx(50.0).epsilon(0.01));
    CHECK(b.y == Approx(-45.0).epsilon(0.02));
    CHECK(b.bottom() == Approx(45.0).epsilon(0.01));
    // Symmetry: the outline's x-extent is balanced about 0.
    CHECK(b.x + b.right() == Approx(0.0).epsilon(0.02));
    // The parameters actually move the figure.
    vec::HeartShape deep = h;
    deep.cleft = 0.5;
    CHECK(boundsOf(flattenOf(vec::ParametricShape{deep})).h == Approx(b.h).epsilon(0.05));
    CHECK(flattenOf(vec::ParametricShape{deep}) != cs);
}

TEST_CASE("banner: a chevron points right, a swallow-tail cuts in, the tail notch is optional") {
    vec::BannerShape b;
    b.size = {100, 40};
    b.pointRatio = 0.2;
    b.notchTail = true;
    const vec::Contours chev = flattenOf(vec::ParametricShape{b});
    REQUIRE(chev.size() == 1);
    checkSaneContours(chev);
    CHECK(chev.front().points.size() == 6);
    CHECK(boundsOf(chev).w == Approx(100.0));
    CHECK(boundsOf(chev).h == Approx(40.0));
    CHECK(std::any_of(chev.front().points.begin(), chev.front().points.end(), [](const Vec2& p) {
        return p.x == Approx(50.0) && std::abs(p.y) < 1e-9;  // the point
    }));

    b.style = vec::BannerShape::Style::Banner;  // the right edge is cut IN instead
    const vec::Contours ribbon = flattenOf(vec::ParametricShape{b});
    REQUIRE(ribbon.size() == 1);
    CHECK(boundsOf(ribbon).w == Approx(100.0));
    CHECK(std::any_of(ribbon.front().points.begin(), ribbon.front().points.end(),
                      [](const Vec2& p) { return p.x == Approx(30.0) && std::abs(p.y) < 1e-9; }));

    b.notchTail = false;  // a straight tail: one vertex fewer
    CHECK(flattenOf(vec::ParametricShape{b}).front().points.size() == 5);
}

TEST_CASE("a degenerate library shape flattens to nothing rather than to garbage") {
    vec::CalloutShape c;
    c.size = {0, 0};
    CHECK(flattenOf(vec::ParametricShape{c}).empty());
    vec::HeartShape h;
    h.size = {40, 0};
    CHECK(flattenOf(vec::ParametricShape{h}).empty());
    vec::RingShape r;
    r.radii = {0, 20};
    CHECK(flattenOf(vec::ParametricShape{r}).empty());
    vec::ArrowShape a;
    a.size = {0, 30};
    CHECK(flattenOf(vec::ParametricShape{a}).empty());
}

// ---- the options-bar bridge -------------------------------------------------------------------

TEST_CASE("readShapeOptions / editedObject reach the library kinds' hot parameters") {
    // The ring shares the star's "Inner %".
    ShapeOptions o = fillOpts();
    o.innerRatio = 0.3;
    const auto ring = buildShapeDraft(ShapeKind::Ring, {0, 0}, {60, 60}, false, false, o);
    REQUIRE(ring.has_value());
    ShapeOptions read;
    readShapeOptions(ring->object, read);
    CHECK(read.innerRatio == Approx(0.3));
    ShapeOptions edit = read;
    edit.innerRatio = 0.8;
    const vec::Object e = editedObject(ring->object, edit);
    CHECK(std::get<vec::RingShape>(shapeOf(e)).innerRatio == Approx(0.8));
    CHECK(std::get<vec::RingShape>(shapeOf(e)).radii.x == Approx(30.0));  // size preserved

    // The callout / cross / banner share the rect's corner radius.
    for (ShapeKind k : {ShapeKind::Callout, ShapeKind::Cross, ShapeKind::Banner}) {
        ShapeOptions co = fillOpts();
        co.cornerRadius = 7.0;
        const auto d = buildShapeDraft(k, {0, 0}, {80, 80}, false, false, co);
        REQUIRE(d.has_value());
        ShapeOptions back;
        readShapeOptions(d->object, back);
        CHECK(back.cornerRadius == Approx(7.0));
        ShapeOptions bump = back;
        bump.cornerRadius = 12.0;
        const vec::Object edited = editedObject(d->object, bump);
        ShapeOptions again;
        readShapeOptions(edited, again);
        CHECK(again.cornerRadius == Approx(12.0));
        // The bar clamps to half the shorter side, exactly as it does for a rect.
        bump.cornerRadius = 5000.0;
        ShapeOptions clamped;
        readShapeOptions(editedObject(d->object, bump), clamped);
        CHECK(clamped.cornerRadius == Approx(40.0));
    }
}

TEST_CASE("the shape-kind catalogue covers the whole enum, with a distinct icon key each") {
    const auto& all = shapeKindCatalog();
    CHECK(all.size() == 11);
    std::vector<std::string> keys;
    for (const ShapeKindInfo& info : all) {
        CHECK(info.iconKey != nullptr);
        CHECK(info.name != nullptr);
        CHECK(shapeKindInfo(info.kind).kind == info.kind);
        keys.emplace_back(info.iconKey);
    }
    std::sort(keys.begin(), keys.end());
    CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
}

TEST_CASE("convertedShape re-shapes in place, keeping the footprint and the paint") {
    const auto rect = buildShapeDraft(ShapeKind::Rect, {0, 0}, {80, 40}, false, false, fillOpts());
    REQUIRE(rect.has_value());
    for (ShapeKind k : {ShapeKind::Heart, ShapeKind::Cross, ShapeKind::Arrow, ShapeKind::Banner}) {
        const vec::Object out = convertedShape(rect->object, k);
        CHECK(shapeKindOf(out) == k);
        const auto b = vec::contentBounds(out);
        REQUIRE(b.has_value());
        CHECK(b->w == Approx(80.0).epsilon(0.02));
        CHECK(b->h == Approx(40.0).epsilon(0.02));
        REQUIRE(std::holds_alternative<vec::SolidPaint>(out.fill));  // the paint rides across
        CHECK(std::get<vec::SolidPaint>(out.fill).color.r == Approx(1.0));
    }
    // Converting to the kind it already is changes nothing at all, and a Path is out of scope.
    CHECK(convertedShape(rect->object, ShapeKind::Rect).geometry == rect->object.geometry);
    vec::Object path;
    path.geometry = vec::Path{};
    CHECK(convertedShape(path, ShapeKind::Heart).geometry == path.geometry);
}

// ---- resize ------------------------------------------------------------------------------------

TEST_CASE("resizeShape scales a library shape and pins the opposite corner") {
    // An ABSOLUTE tolerance, in document pixels: the box a curved shape reports comes off its
    // tessellation, which re-samples at the new radius, so demanding exactness would be testing
    // the sampler rather than the resize. Under a pixel is what "the handle stayed put" means.
    constexpr double kPx = 0.75;
    for (ShapeKind k : {ShapeKind::Heart, ShapeKind::Cross, ShapeKind::Arrow, ShapeKind::Banner,
                        ShapeKind::Ring}) {
        // A 40 x 40 shape drawn at (0,0)-(40,40): local bounds [-20,-20]..[20,20], centre (20,20).
        const auto d = buildShapeDraft(k, {0, 0}, {40, 40}, false, false, fillOpts());
        REQUIRE(d.has_value());
        // Drag the BR handle (2) out to (60,60): 1.5x on both axes, TL pinned at the origin.
        const auto r = resizeShape(d->object, d->placement, 2, {60, 60}, false, false);
        REQUIRE(r.has_value());
        const auto b = vec::contentBounds(r->object);
        REQUIRE(b.has_value());
        CHECK(std::abs(b->w - 60.0) < kPx);
        CHECK(std::abs(b->h - 60.0) < kPx);
        const Vec2 tl = r->placement.apply({b->x, b->y});
        const Vec2 br = r->placement.apply({b->right(), b->bottom()});
        CHECK(std::abs(tl.x) < kPx);
        CHECK(std::abs(tl.y) < kPx);
        CHECK(std::abs(br.x - 60.0) < kPx);
        CHECK(std::abs(br.y - 60.0) < kPx);
    }
}

TEST_CASE("resizeShape scales a callout's tail with its body") {
    const auto d = buildShapeDraft(ShapeKind::Callout, {0, 0}, {40, 40}, false, false, fillOpts());
    REQUIRE(d.has_value());
    const auto& before = std::get<vec::CalloutShape>(shapeOf(d->object));
    const double tail0 = before.tailLength, width0 = before.tailWidth;
    REQUIRE(tail0 > 0.0);
    const auto r = resizeShape(d->object, d->placement, 2, {80, 80}, false, false);
    REQUIRE(r.has_value());
    const auto& after = std::get<vec::CalloutShape>(shapeOf(r->object));
    // A tail left absolute would shrink to nothing relative to the balloon (and drag the framed
    // box off its anchor); it takes the uniform factor instead.
    CHECK(after.size.x > before.size.x);
    CHECK(after.tailLength > tail0);
    CHECK(after.tailWidth > width0);
    // Both tail dimensions take the SAME factor (the tail points in an arbitrary direction, so it
    // scales uniformly), which is what keeps the balloon's proportions through a resize.
    CHECK(after.tailLength / tail0 == Approx(after.tailWidth / width0));
    CHECK(after.tailAngle == Approx(before.tailAngle));  // a direction is not a size
}

// ---- convert to path ---------------------------------------------------------------------------

TEST_CASE("pathFromShape traces the same outline for every library kind") {
    const auto sameBounds = [](const vec::ParametricShape& s, double eps) {
        const Rect direct = boundsOf(vec::flatten(vec::Geometry{s}, 0.01));
        const Rect viaPath = boundsOf(vec::flatten(vec::Geometry{vec::pathFromShape(s)}, 0.01));
        CHECK(viaPath.x == Approx(direct.x).epsilon(eps));
        CHECK(viaPath.y == Approx(direct.y).epsilon(eps));
        CHECK(viaPath.w == Approx(direct.w).epsilon(eps));
        CHECK(viaPath.h == Approx(direct.h).epsilon(eps));
    };
    vec::CalloutShape c;
    c.size = {100, 60};
    c.tailLength = 30;
    c.tailWidth = 20;
    sameBounds(vec::ParametricShape{c}, 0.02);

    vec::ArrowShape a;
    a.size = {100, 60};
    sameBounds(vec::ParametricShape{a}, 0.001);

    vec::RingShape r;
    r.radii = {50, 30};
    r.innerRatio = 0.4;
    sameBounds(vec::ParametricShape{r}, 0.01);

    vec::RingShape wedge = r;
    wedge.innerRatio = 0.0;
    wedge.endAngle = M_PI * 0.75;
    sameBounds(vec::ParametricShape{wedge}, 0.01);

    vec::CrossShape x;
    x.size = {90, 90};
    x.cornerRadius = 8;
    sameBounds(vec::ParametricShape{x}, 0.02);

    vec::HeartShape h;
    h.size = {80, 70};
    sameBounds(vec::ParametricShape{h}, 0.01);

    vec::BannerShape b;
    b.size = {120, 40};
    b.cornerRadius = 4;
    sameBounds(vec::ParametricShape{b}, 0.02);
}

TEST_CASE("pathFromShape yields closed subpaths (and two of them for an annulus)") {
    vec::RingShape r;
    r.radii = {50, 50};
    r.innerRatio = 0.5;
    const vec::Path p = vec::pathFromShape(vec::ParametricShape{r});
    REQUIRE(p.subpaths.size() == 2);
    for (const vec::SubPath& sp : p.subpaths) {
        CHECK(sp.closed);
        CHECK(sp.nodes.size() >= 3);
    }
    vec::HeartShape h;
    h.size = {60, 60};
    const vec::Path hp = vec::pathFromShape(vec::ParametricShape{h});
    REQUIRE(hp.subpaths.size() == 1);
    CHECK(hp.subpaths.front().closed);
    CHECK(hp.subpaths.front().nodes.size() == 4);  // four cubics: two lobes, two flanks
}

// ---- corner-conforming on-diagram handles (S26-c refinement) -----------------------------------
//
// The user report was that a handle "supposed to sit on a rounded corner sits where the SHARP
// corner would be", floating off the shape. The fix routes every such handle through the shared
// corner engine, so what these cases actually check is a distance: the handle's point must lie ON
// the flattened outline the renderer draws, at radius 0, mid-travel, at a saturated radius, and on
// a concave (reflex-vertex) fillet -- "a handle exists" would prove nothing.

namespace {

// Distance from `p` to the polyline the renderer actually draws. nearestArcDistance walks every
// segment of every contour, so this is the true point-to-outline distance, not a vertex distance.
double distToOutline(const vec::ParametricShape& s, Vec2 p) {
    const vec::Contours cs = vec::flatten(vec::Geometry{s}, 0.01);
    REQUIRE_FALSE(cs.empty());
    double d = 0.0;
    // The arc POSITION is the [[nodiscard]] return; the out-param is the distance we want.
    static_cast<void>(vec::nearestArcDistance(cs, p, &d));
    return d;
}

// Every corner of `poly` lands on the outline `s` flattens to.
void checkCornersOnOutline(const vec::CorneredPolygon& poly, const vec::ParametricShape& s,
                           double tol, const char* what) {
    REQUIRE_MESSAGE(!poly.empty(), what);
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const std::string tag = std::string(what) + " corner " + std::to_string(i);
        const vec::CornerPoint cp = vec::cornerPointAt(poly, i);
        CHECK_MESSAGE(distToOutline(s, cp.apex) < tol, tag);
        // The tangent points are where the corner MEETS the straight edges: also on the outline.
        // Concatenate first: doctest resolves its own operator+ against the message argument.
        const std::string tag0 = tag + " p0";
        const std::string tag1 = tag + " p1";
        CHECK_MESSAGE(distToOutline(s, cp.p0) < tol, tag0);
        CHECK_MESSAGE(distToOutline(s, cp.p1) < tol, tag1);
    }
}

}  // namespace

TEST_CASE("a rounded corner's handle lies on the rendered outline, not on the sharp vertex") {
    vec::RectShape r;
    r.size = {120, 80};

    SUBCASE("radius 0 -- the apex IS the sharp vertex") {
        const vec::CorneredPolygon poly = vec::rectPolygon(r);
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const vec::CornerPoint cp = vec::cornerPointAt(poly, i);
            CHECK_FALSE(cp.rounded);
            CHECK(cp.apex.x == Approx(cp.vertex.x));
            CHECK(cp.apex.y == Approx(cp.vertex.y));
        }
        checkCornersOnOutline(poly, vec::ParametricShape{r}, 0.05, "sharp rect");
    }

    SUBCASE("a rounded corner pulls the handle OFF the vertex, onto the fillet") {
        r.cornerRadius = {20, 20, 20, 20};
        const vec::CorneredPolygon poly = vec::rectPolygon(r);
        const vec::CornerPoint cp = vec::cornerPointAt(poly, 0);
        CHECK(cp.rounded);
        // The classic 90-degree offset: the arc midpoint is r*(sqrt2 - 1) along the diagonal.
        const double off = (cp.apex - cp.vertex).length();
        CHECK(off == Approx(20.0 * (std::sqrt(2.0) - 1.0)).epsilon(1e-6));
        // ...and the OLD placement (the fillet's centre, vertex + r on both axes) was off the
        // outline by a full radius, which is exactly what the user saw.
        const Vec2 oldPlacement{cp.vertex.x + 20.0, cp.vertex.y + 20.0};
        CHECK(distToOutline(vec::ParametricShape{r}, oldPlacement) == Approx(20.0).epsilon(0.01));
        checkCornersOnOutline(poly, vec::ParametricShape{r}, 0.05, "rounded rect");
    }

    SUBCASE("the handle stays on the curve across the whole radius sweep, per corner style") {
        for (vec::CornerStyle st :
             {vec::CornerStyle::Round, vec::CornerStyle::Inverse, vec::CornerStyle::Bevel}) {
            for (double rad : {1.0, 5.0, 17.5, 30.0, 39.9}) {
                vec::RectShape rr = vec::RectShape::uniform({120, 80}, rad, st);
                checkCornersOnOutline(vec::rectPolygon(rr), vec::ParametricShape{rr}, 0.05,
                                      "swept rect");
            }
        }
    }

    SUBCASE("a saturated radius pins the handle instead of jumping it") {
        // Half the shorter side is the engine's ceiling for a rect (80 / 2 = 40).
        CHECK(vec::maxCornerRadius(vec::rectPolygon(r)) == Approx(40.0));
        vec::RectShape at = vec::RectShape::uniform({120, 80}, 40.0);
        vec::RectShape past = vec::RectShape::uniform({120, 80}, 400.0);
        const Vec2 a = vec::cornerPointAt(vec::rectPolygon(at), 0).apex;
        const Vec2 b = vec::cornerPointAt(vec::rectPolygon(past), 0).apex;
        CHECK((a - b).length() == Approx(0.0).epsilon(1e-9));
        CHECK(vec::cornerPointAt(vec::rectPolygon(past), 0).radius == Approx(40.0));
        checkCornersOnOutline(vec::rectPolygon(past), vec::ParametricShape{past}, 0.05,
                              "saturated rect");
    }
}

TEST_CASE("the corner engine handles CONCAVE fillets -- the sign of the offset inverts") {
    vec::CrossShape x;
    x.size = {120, 120};
    x.armRatio = 0.34;
    x.cornerRadius = 6;
    const vec::CorneredPolygon poly = vec::crossPolygon(x);
    REQUIRE(poly.size() == 12);

    // Ring index 1 is the vertical bar's top-right: a CONVEX corner, so its apex moves toward the
    // figure's body (down-left). Index 2 is the inner (reflex) corner where the bars meet: its
    // fillet is a scoop, so the apex moves the other way -- up-right, AWAY from the body.
    const vec::CornerPoint convex = vec::cornerPointAt(poly, 1);
    const vec::CornerPoint concave = vec::cornerPointAt(poly, 2);
    CHECK(convex.axis.x < 0.0);
    CHECK(convex.axis.y > 0.0);
    CHECK(concave.axis.x > 0.0);
    CHECK(concave.axis.y < 0.0);
    // The two bisectors face opposite ways: nothing in the math keys off "inside the box".
    CHECK(convex.axis.dot(concave.axis) < 0.0);

    for (vec::CornerStyle st :
         {vec::CornerStyle::Round, vec::CornerStyle::Inverse, vec::CornerStyle::Bevel}) {
        for (double rad : {0.0, 2.0, 9.0, 60.0}) {  // 60 saturates hard on a cross's short edges
            vec::CrossShape c = x;
            c.cornerRadius = rad;
            c.cornerStyle = st;
            checkCornersOnOutline(vec::crossPolygon(c), vec::ParametricShape{c}, 0.05, "cross");
        }
    }
    // The cross's ceiling is far BELOW half the shorter side (its edges are short), which is why
    // the designer takes the slider's range from the engine rather than from the box.
    const double ceiling = vec::maxCornerRadius(poly);
    CHECK(ceiling > 0.0);
    CHECK(ceiling < 120.0 * 0.5);
}

TEST_CASE("cornerRadiusForPoint is the exact inverse of the apex a handle is drawn at") {
    const auto roundTrip = [](const vec::CorneredPolygon& poly, std::size_t i, double radius) {
        // Place the handle for `radius`, then drag it to exactly where it already is.
        const vec::CornerPoint cp = vec::cornerPointAt(poly.verts, i, radius, poly.styles[i]);
        return vec::cornerRadiusForPoint(poly.verts, i, poly.styles[i], cp.apex);
    };
    vec::RectShape r = vec::RectShape::uniform({120, 80}, 0.0);
    const vec::CorneredPolygon rect = vec::rectPolygon(r);
    for (double rad : {0.0, 3.0, 12.0, 39.0})
        CHECK(roundTrip(rect, 0, rad) == Approx(rad).epsilon(1e-9));
    // Past the ceiling the inverse saturates (never runs away), so the parameter a drag can write
    // is always one the outline can show.
    CHECK(roundTrip(rect, 0, 999.0) == Approx(40.0));

    vec::CrossShape x;
    x.size = {120, 120};
    x.armRatio = 0.34;
    const vec::CorneredPolygon cross = vec::crossPolygon(x);
    const double ceiling = vec::maxCornerRadius(cross);
    for (std::size_t i : {std::size_t{0}, std::size_t{2}, std::size_t{5}})  // incl. a reflex vertex
        for (double rad : {0.0, ceiling * 0.5, ceiling})
            CHECK(roundTrip(cross, i, rad) == Approx(rad).epsilon(1e-6));
}

TEST_CASE("the designer's on-diagram handles sit on the outline for every rounded kind") {
    const auto handleOf = [](const vec::Object& o, int id) {
        const auto pts = shapeHandlePoints(o);
        for (const auto& h : pts)
            if (h.first == id) return h.second;
        const std::string missing = "no handle " + std::to_string(id);
        FAIL(missing);
        return Vec2{};
    };
    const auto objOf = [](const vec::ParametricShape& s) {
        vec::Object o;
        o.geometry = s;
        return o;
    };

    SUBCASE("rect -- four corner handles, sharp and rounded") {
        for (double rad : {0.0, 9.0, 40.0, 500.0}) {
            const vec::RectShape r = vec::RectShape::uniform({140, 90}, rad);
            const vec::Object o = objOf(vec::ParametricShape{r});
            const auto pts = shapeHandlePoints(o);
            REQUIRE(pts.size() == 4);
            for (const auto& h : pts)
                CHECK(distToOutline(vec::ParametricShape{r}, h.second) < 0.05);
        }
    }

    SUBCASE("cross -- the arm handle and the corner handle both ride their own corner") {
        vec::CrossShape x;
        x.size = {120, 120};
        x.armRatio = 0.3;
        for (double rad : {0.0, 4.0, 12.0}) {
            x.cornerRadius = rad;
            const vec::Object o = objOf(vec::ParametricShape{x});
            CHECK(distToOutline(vec::ParametricShape{x}, handleOf(o, 0)) < 0.05);
            CHECK(distToOutline(vec::ParametricShape{x}, handleOf(o, 1)) < 0.05);
        }
    }

    SUBCASE("banner -- both styles, notched and not") {
        for (vec::BannerShape::Style st :
             {vec::BannerShape::Style::Chevron, vec::BannerShape::Style::Banner})
            for (bool notch : {false, true})
                for (double rad : {0.0, 3.0, 8.0}) {
                    vec::BannerShape b;
                    b.size = {160, 60};
                    b.style = st;
                    b.notchTail = notch;
                    b.cornerRadius = rad;
                    const vec::Object o = objOf(vec::ParametricShape{b});
                    CHECK(distToOutline(vec::ParametricShape{b}, handleOf(o, 0)) < 0.05);
                    CHECK(distToOutline(vec::ParametricShape{b}, handleOf(o, 1)) < 0.05);
                }
    }

    SUBCASE("callout body -- the corner handle sits on the balloon's own outline") {
        for (double rad : {0.0, 10.0, 45.0}) {
            vec::CalloutShape c;
            c.size = {140, 90};
            c.cornerRadius = rad;
            c.tailAngle = 0.0;      // straight out to the RIGHT, so the tail's splice is nowhere
            c.tailLength = 30.0;    // near the top-left corner the handle rides
            c.tailWidth = 24.0;
            const vec::Object o = objOf(vec::ParametricShape{c});
            CHECK(distToOutline(vec::ParametricShape{c}, handleOf(o, 2)) < 0.05);
        }
        // An elliptical body has no corners to round, so it publishes no corner handle.
        vec::CalloutShape e;
        e.size = {140, 90};
        e.body = vec::CalloutShape::Body::Ellipse;
        bool has2 = false;
        for (const auto& h : shapeHandlePoints(objOf(vec::ParametricShape{e})))
            has2 = has2 || h.first == 2;
        CHECK_FALSE(has2);
    }

    SUBCASE("polygon + star -- the rounding pulls the handle in off the sharp tip / valley") {
        for (double rad : {0.0, 6.0, 20.0}) {
            vec::PolygonShape p;
            p.sides = 6;
            p.radius = 60;
            p.cornerRadius = rad;
            const vec::Object o = objOf(vec::ParametricShape{p});
            CHECK(distToOutline(vec::ParametricShape{p}, handleOf(o, 0)) < 0.05);
        }
        for (double vr : {0.0, 5.0, 14.0}) {
            vec::StarShape s;
            s.points = 5;
            s.outerRadius = 60;
            s.innerRadius = 28;
            s.valleyRadius = vr;
            const vec::Object o = objOf(vec::ParametricShape{s});
            CHECK(distToOutline(vec::ParametricShape{s}, handleOf(o, 0)) < 0.05);
        }
    }
}

TEST_CASE("dragging a corner handle leaves it under the cursor (and pins it when saturated)") {
    const auto handleOf = [](const vec::Object& o, int id) {
        for (const auto& h : shapeHandlePoints(o))
            if (h.first == id) return h.second;
        const std::string missing = "no handle " + std::to_string(id);
        FAIL(missing);
        return Vec2{};
    };
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::RectShape::uniform({120, 80}, 0.0)};

    // Walk the top-left corner's handle in along its diagonal; each step must land the handle on
    // the point it was dragged to, AND on the outline the new radius draws.
    for (double d : {2.0, 5.0, 10.0}) {  // stays clear of the clamp, so each drag lands exactly
        const Vec2 target{-60.0 + d, -40.0 + d};
        const std::optional<vec::Object> next = shapeAfterHandleDrag(o, 0, target, true);
        REQUIRE(next.has_value());
        o = *next;
        const Vec2 h = handleOf(o, 0);
        CHECK((h - target).length() < 1e-6);
        CHECK(distToOutline(shapeOf(o), h) < 0.05);
    }
    // Linked corners moved together.
    const auto& rect = std::get<vec::RectShape>(shapeOf(o));
    for (int i = 1; i < 4; ++i)
        CHECK(rect.cornerRadius[static_cast<std::size_t>(i)] == Approx(rect.cornerRadius[0]));

    // Far past the clamp the handle STOPS -- it does not run off with the cursor and it does not
    // jump back when the drag reverses.
    const Vec2 far{200.0, 200.0};
    const std::optional<vec::Object> sat = shapeAfterHandleDrag(o, 0, far, true);
    REQUIRE(sat.has_value());
    const Vec2 hSat = handleOf(*sat, 0);
    CHECK(std::get<vec::RectShape>(shapeOf(*sat)).cornerRadius[0] == Approx(40.0));
    const std::optional<vec::Object> sat2 = shapeAfterHandleDrag(*sat, 0, {400.0, 400.0}, true);
    REQUIRE(sat2.has_value());
    CHECK((handleOf(*sat2, 0) - hSat).length() == Approx(0.0).epsilon(1e-9));

    // The cross's CONCAVE inner fillet under a drag: the radius handle is on the convex corner, but
    // the concave corners must follow it onto the outline rather than inverting away from it.
    vec::CrossShape x;
    x.size = {120, 120};
    x.armRatio = 0.32;
    vec::Object cx;
    cx.geometry = vec::ParametricShape{x};
    const vec::CorneredPolygon base = vec::crossPolygon(x);
    const Vec2 v0 = base.verts[0];
    const std::optional<vec::Object> dragged =
        shapeAfterHandleDrag(cx, 1, {v0.x + 3.0, v0.y + 3.0}, false);
    REQUIRE(dragged.has_value());
    const auto& xs = std::get<vec::CrossShape>(shapeOf(*dragged));
    CHECK(xs.cornerRadius > 0.0);
    checkCornersOnOutline(vec::crossPolygon(xs), shapeOf(*dragged), 0.05, "dragged cross");
}

TEST_CASE("an arc's sweep endpoints wrap instead of flipping the figure inside out") {
    // Both routes into a sweep endpoint -- the angle DIAL and this on-diagram handle -- share one
    // unwrap, so what is pinned here is the contract the dial rides on too.
    const auto afterEndDrag = [](vec::EllipseShape e, Vec2 p) {
        vec::Object o;
        o.geometry = vec::ParametricShape{e};
        const std::optional<vec::Object> next = shapeAfterHandleDrag(o, 1, p, false);
        REQUIRE(next.has_value());
        return std::get<vec::EllipseShape>(shapeOf(*next));
    };
    vec::EllipseShape full;
    full.radii = {50, 30};  // start 0, end 2*pi: a whole ellipse

    // A FULL sweep can still be cut down. Its End sits at 2*pi, so "a quarter turn on" unwraps to
    // 450 degrees -- the naive nearest-branch rule would clamp that straight back to full.
    const vec::EllipseShape q = afterEndDrag(full, {0.0, 30.0});
    CHECK(q.endAngle - q.startAngle == Approx(M_PI / 2.0).epsilon(1e-6));

    // Dragging the End round PAST the start saturates at a full turn: it never collapses to
    // nothing, and the sweep never runs negative (which is what made the figure vanish).
    vec::EllipseShape half = full;
    half.endAngle = M_PI;
    const vec::EllipseShape wrapped = afterEndDrag(half, {0.0, -30.0}); // 270 deg, the far side of 0
    CHECK(wrapped.endAngle > wrapped.startAngle);
    CHECK(wrapped.endAngle - wrapped.startAngle > M_PI);
    CHECK(wrapped.endAngle - wrapped.startAngle <= 2.0 * M_PI + 1e-9);

    // Shrinking past the minimum stops at a sliver rather than inverting: 5 degrees off the start
    // ray is inside the minimum sweep, so the endpoint holds there.
    const double fiveDeg = 5.0 * M_PI / 180.0;
    const vec::EllipseShape tiny =
        afterEndDrag(half, {50.0 * std::cos(fiveDeg), 30.0 * std::sin(fiveDeg)});
    CHECK(tiny.endAngle - tiny.startAngle > 0.0);
    CHECK(tiny.endAngle - tiny.startAngle < 0.2);
}
