#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/vector/flatten.hpp"
#include "core/vector/hit.hpp"
#include "core/vector/object.hpp"
#include "core/vector/raster.hpp"
#include "core/vector/stroke.hpp"
#include "render/compositor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <doctest/doctest.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace mosaic::core;
using mosaic::common::Vec2;
using doctest::Approx;

namespace {

// A closed bezier subpath whose handles all equal their anchors == a polyline ring.
vec::Path polylineRing(std::vector<Vec2> pts) {
    vec::SubPath sp;
    sp.closed = true;
    for (const Vec2& p : pts) sp.nodes.push_back(vec::Node{p, p, p});
    return vec::Path{{sp}, vec::FillRule::NonZero};
}

// A closed polyline contour straight from points (for the classification tests).
vec::Contour ring(std::vector<Vec2> pts) {
    vec::Contour c;
    c.points = std::move(pts);
    c.closed = true;
    return c;
}

// A straight node (handles == anchor) for hand-built paths.
vec::Node node(Vec2 p) { return vec::Node{p, p, p}; }

float alphaF(const mosaic::common::ImageF& im, std::uint32_t x, std::uint32_t y) {
    return im.at(x, y).a;
}
float chanF(const mosaic::common::ImageF& im, std::uint32_t x, std::uint32_t y, int ch) {
    const mosaic::common::ColorF c = im.at(x, y);
    return ch == 0 ? c.r : ch == 1 ? c.g : ch == 2 ? c.b : c.a;
}

}  // namespace

TEST_CASE("flatten: sharp rectangle is a 4-point closed ring with exact bounds") {
    const vec::Geometry g = vec::ParametricShape{vec::RectShape::uniform({20, 10}, 0)};
    const vec::Contours cs = vec::flatten(g);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].closed);
    CHECK(cs[0].points.size() == 4);

    const auto b = vec::contentBounds(g);
    REQUIRE(b.has_value());
    CHECK(b->x == Approx(-10.0));
    CHECK(b->y == Approx(-5.0));
    CHECK(b->w == Approx(20.0));
    CHECK(b->h == Approx(10.0));
}

TEST_CASE("flatten: rounded rectangle keeps full extent but adds corner points") {
    const vec::Geometry g = vec::ParametricShape{vec::RectShape::uniform({20, 10}, 3.0)};
    const vec::Contours cs = vec::flatten(g);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].closed);
    CHECK(cs[0].points.size() > 4);  // corner arcs subdivided

    const auto b = vec::contentBounds(g);
    REQUIRE(b.has_value());
    CHECK(b->w == Approx(20.0));  // edges still reach the full box
    CHECK(b->h == Approx(10.0));
}

TEST_CASE("flatten: ellipse is closed, finely sampled, within its radii") {
    const vec::Geometry g = vec::ParametricShape{vec::EllipseShape{{10, 5}}};
    const vec::Contours cs = vec::flatten(g);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].closed);
    CHECK(cs[0].points.size() >= 8);
    for (const Vec2& p : cs[0].points) {
        CHECK(std::abs(p.x) <= 10.0 + 1e-6);
        CHECK(std::abs(p.y) <= 5.0 + 1e-6);
    }
    const auto b = vec::contentBounds(g);
    REQUIRE(b.has_value());
    CHECK(b->w == Approx(20.0).epsilon(0.05));  // chord error keeps it just under 2*rx
}

TEST_CASE("flatten: 4-sided polygon is an axis diamond with the first vertex up") {
    const vec::Geometry g = vec::ParametricShape{vec::PolygonShape{4, 10, 0}};
    const vec::Contours cs = vec::flatten(g);
    REQUIRE(cs.size() == 1);
    REQUIRE(cs[0].points.size() == 4);
    CHECK(cs[0].points[0].x == Approx(0.0).epsilon(1e-9));
    CHECK(cs[0].points[0].y == Approx(-10.0));  // y-down: "up" is negative
    const auto b = vec::contentBounds(g);
    REQUIRE(b.has_value());
    CHECK(b->w == Approx(20.0));
    CHECK(b->h == Approx(20.0));
}

TEST_CASE("flatten: star emits 2*points vertices") {
    const vec::Geometry g = vec::ParametricShape{vec::StarShape{5, 10, 4, 0, 0}};
    const vec::Contours cs = vec::flatten(g);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].closed);
    CHECK(cs[0].points.size() == 10);
}

// ---- S26-b shape-designer model extensions (docs/vector-model.md §7.3/§7.6) -------------------

namespace {
bool hasPoint(const vec::Contour& c, Vec2 v) {
    return std::any_of(c.points.begin(), c.points.end(),
                       [&](Vec2 p) { return std::abs(p.x - v.x) < 1e-6 && std::abs(p.y - v.y) < 1e-6; });
}
bool hasOrigin(const vec::Contour& c) { return hasPoint(c, {0, 0}); }
}  // namespace

TEST_CASE("flatten: per-corner radius rounds only the requested rect corner") {
    vec::RectShape rs;
    rs.size = {20, 20};
    rs.cornerRadius = {0, 4, 0, 0};  // only TR (index 1) rounded
    const vec::Contours cs = vec::flatten(vec::ParametricShape{rs});
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].points.size() > 4);          // the one rounded corner added arc points
    CHECK(hasPoint(cs[0], {-10, -10}));      // TL still a sharp vertex
    CHECK(hasPoint(cs[0], {10, 10}));        // BR
    CHECK(hasPoint(cs[0], {-10, 10}));       // BL
    CHECK_FALSE(hasPoint(cs[0], {10, -10})); // TR was rounded away
    const auto b = vec::contentBounds(vec::ParametricShape{rs});
    REQUIRE(b.has_value());
    CHECK(b->w == Approx(20.0));             // straight edges still reach the full box
    CHECK(b->h == Approx(20.0));
}

TEST_CASE("flatten: a bevel corner is a straight chamfer (two tangent points, no arc)") {
    vec::RectShape rs;
    rs.size = {20, 20};
    rs.cornerRadius = {0, 4, 0, 0};
    rs.cornerStyle = {vec::CornerStyle::Round, vec::CornerStyle::Bevel, vec::CornerStyle::Round,
                      vec::CornerStyle::Round};
    const vec::Contours cs = vec::flatten(vec::ParametricShape{rs});
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].points.size() == 5);  // 3 sharp vertices + 2 bevel tangent points
}

TEST_CASE("flatten: an inverse corner scoops inward (arc centred on the vertex)") {
    vec::RectShape rs;
    rs.size = {20, 20};
    rs.cornerRadius = {0, 4, 0, 0};
    rs.cornerStyle = {vec::CornerStyle::Round, vec::CornerStyle::Inverse, vec::CornerStyle::Round,
                      vec::CornerStyle::Round};
    const vec::Contours cs = vec::flatten(vec::ParametricShape{rs});
    REQUIRE(cs.size() == 1);
    const Vec2 V{10, -10};  // the TR vertex
    int arcPts = 0;         // an inverse arc is centred on V at radius t (== 4 for a 90deg corner)
    for (const Vec2& p : cs[0].points) {
        const bool sharp = (std::abs(p.x + 10) < 1e-6 && std::abs(p.y + 10) < 1e-6) ||
                           (std::abs(p.x - 10) < 1e-6 && std::abs(p.y - 10) < 1e-6) ||
                           (std::abs(p.x + 10) < 1e-6 && std::abs(p.y - 10) < 1e-6);
        if (sharp) continue;
        ++arcPts;
        CHECK((p - V).length() == Approx(4.0).epsilon(1e-6));
    }
    CHECK(arcPts >= 2);
}

TEST_CASE("flatten: polygon and star corner rounding add points independently") {
    const vec::PolygonShape hexSharp{6, 10, 0, vec::CornerStyle::Round};
    const vec::PolygonShape hexRound{6, 10, 2, vec::CornerStyle::Round};
    CHECK(vec::flatten(vec::ParametricShape{hexSharp})[0].points.size() == 6);
    CHECK(vec::flatten(vec::ParametricShape{hexRound})[0].points.size() > 6);

    const auto n = [](const vec::StarShape& s) {
        return vec::flatten(vec::ParametricShape{s})[0].points.size();
    };
    CHECK(n(vec::StarShape{5, 10, 4, 0, 0}) == 10);  // sharp
    CHECK(n(vec::StarShape{5, 10, 4, 2, 0}) > 10);   // only outer tips rounded
    CHECK(n(vec::StarShape{5, 10, 4, 0, 1}) > 10);   // only inner valleys rounded
}

TEST_CASE("flatten: ellipse arc modes (open / chord / pie)") {
    const auto arc = [](vec::EllipseShape::ArcMode m) {
        vec::EllipseShape e;
        e.radii = {10, 5};
        e.startAngle = 0.0;
        e.endAngle = M_PI;  // a half sweep -> an arc
        e.arcMode = m;
        return vec::flatten(vec::ParametricShape{e});
    };
    CHECK_FALSE(arc(vec::EllipseShape::ArcMode::Open)[0].closed);  // bare arc, open for stroking

    const auto chord = arc(vec::EllipseShape::ArcMode::Chord);
    CHECK(chord[0].closed);
    CHECK_FALSE(hasOrigin(chord[0]));  // chord spans only the rim

    const auto pie = arc(vec::EllipseShape::ArcMode::Pie);
    CHECK(pie[0].closed);
    CHECK(hasOrigin(pie[0]));  // pie goes via the centre

    vec::EllipseShape fullE;  // a full sweep stays an ordinary ellipse regardless of arcMode
    fullE.radii = {10, 5};
    fullE.arcMode = vec::EllipseShape::ArcMode::Pie;
    const auto full = vec::flatten(vec::ParametricShape{fullE});
    CHECK(full[0].closed);
    CHECK_FALSE(hasOrigin(full[0]));
}

TEST_CASE("flatten: a cubic segment subdivides; a straight segment does not") {
    vec::SubPath line;  // two coincident-handle nodes == one straight segment
    line.nodes = {vec::Node{{0, 0}, {0, 0}, {0, 0}}, vec::Node{{10, 0}, {10, 0}, {10, 0}}};
    const vec::Contours straight = vec::flatten(vec::Path{{line}, vec::FillRule::NonZero});
    REQUIRE(straight.size() == 1);
    CHECK(straight[0].points.size() == 2);

    vec::SubPath curve;  // a bulging cubic from (0,0) to (10,0)
    curve.nodes = {vec::Node{{0, 0}, {0, 0}, {0, 8}}, vec::Node{{10, 0}, {10, 8}, {10, 0}}};
    const vec::Contours curved = vec::flatten(vec::Path{{curve}, vec::FillRule::NonZero});
    REQUIRE(curved.size() == 1);
    CHECK(curved[0].points.size() > 2);  // adaptively subdivided
}

TEST_CASE("samplePathAt and contourLength walk by arc length and clamp at the ends") {
    const vec::Contours cs = vec::flatten(vec::Path{{[] {
        vec::SubPath sp;
        sp.nodes = {vec::Node{{0, 0}, {0, 0}, {0, 0}}, vec::Node{{10, 0}, {10, 0}, {10, 0}}};
        return sp;
    }()}});
    CHECK(vec::contourLength(cs) == Approx(10.0));

    const auto mid = vec::samplePathAt(cs, 5.0);
    CHECK(mid.pos.x == Approx(5.0));
    CHECK(mid.pos.y == Approx(0.0));
    CHECK(mid.tangent.x == Approx(1.0));

    CHECK(vec::samplePathAt(cs, 0.0).pos.x == Approx(0.0));     // start
    CHECK(vec::samplePathAt(cs, 100.0).pos.x == Approx(10.0));  // clamped past the end
}

TEST_CASE("nearestArcDistance inverts samplePathAt's position (S30 text-on-path brackets)") {
    // An L: (0,0) -> (10,0) -> (10,10). Arc length 20.
    vec::Contour L;
    L.points = {{0, 0}, {10, 0}, {10, 10}};
    const vec::Contours cs{L};

    double d = -1.0;
    CHECK(vec::nearestArcDistance(cs, {5.0, -2.0}, &d) == Approx(5.0));  // above the first leg
    CHECK(d == Approx(2.0));
    CHECK(vec::nearestArcDistance(cs, {12.0, 7.0}, &d) == Approx(17.0));  // right of the second leg
    CHECK(d == Approx(2.0));
    CHECK(vec::nearestArcDistance(cs, {-5.0, 0.0}, &d) == Approx(0.0));  // clamped before the start
    CHECK(d == Approx(5.0));
    // Round-trip: a point ON the path maps back to its own arc distance.
    const auto at = vec::samplePathAt(cs, 13.5);
    CHECK(vec::nearestArcDistance(cs, at.pos, &d) == Approx(13.5));
    CHECK(d == Approx(0.0));
    // Degenerate input.
    CHECK(vec::nearestArcDistance({}, {1.0, 1.0}, &d) == Approx(0.0));
    CHECK(d == Approx(0.0));
}

TEST_CASE("topmostVectorSpineAt hits near the outline only; rebakeTextPathFit follows the path") {
    Document doc(400, 400);
    auto vl = doc.makeVector("path");
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::RectShape::uniform({100, 100}, 0)};  // local [-50,50]^2
    o.fill = vec::SolidPaint{{1, 0, 0, 1}};
    vl->setObject(std::move(o));
    vl->setTransform(mosaic::common::Affine2D::translation(200, 200));  // doc rect [150,250]^2
    Layer& pathLayer = doc.root().addOnTop(std::move(vl));

    // Near the top edge: a spine hit. Dead centre of the fill: NOT a spine hit (that is what
    // distinguishes this from topmostVectorLayerAt's fill-aware pick). Far away: nothing.
    CHECK(topmostVectorSpineAt(doc.root(), {200, 152}, 6.0) == &pathLayer);
    CHECK(topmostVectorSpineAt(doc.root(), {200, 200}, 6.0) == nullptr);
    CHECK(topmostVectorLayerAt(doc.root(), {200, 200}) == &pathLayer);
    CHECK(topmostVectorSpineAt(doc.root(), {40, 40}, 6.0) == nullptr);

    // A text layer riding that path: bake, then MOVE the path -- the re-bake shifts the contours.
    auto tl = doc.makeText("rider", "abc");
    mosaic::core::text::PathFit fit;
    fit.layer = pathLayer.id();
    fit.s0 = 10.0;
    fit.s1 = 300.0;
    tl->mutableBlock().pathFit = fit;
    TextLayer& rider = static_cast<TextLayer&>(doc.root().addOnTop(std::move(tl)));

    REQUIRE(rebakeTextPathFit(doc, rider));  // first bake fills the empty contours
    const vec::Contours baked0 = rider.block().pathFit->baked;
    REQUIRE_FALSE(baked0.empty());
    CHECK_FALSE(rebakeTextPathFit(doc, rider));  // nothing changed -> no-op

    pathLayer.setTransform(mosaic::common::Affine2D::translation(230, 200));  // slide 30 right
    REQUIRE(rebakeTextPathFit(doc, rider));
    const vec::Contours& baked1 = rider.block().pathFit->baked;
    REQUIRE(baked1.size() == baked0.size());
    REQUIRE(baked1[0].points.size() == baked0[0].points.size());
    CHECK(baked1[0].points[0].x == Approx(baked0[0].points[0].x + 30.0));
    CHECK(baked1[0].points[0].y == Approx(baked0[0].points[0].y));

    // Deleting the source keeps the last baked path (the text holds its shape).
    auto removed = doc.root().removeAt(doc.root().indexOf(pathLayer.id()));
    CHECK_FALSE(rebakeTextPathFit(doc, rider));
    CHECK(rider.block().pathFit->baked == baked1);
}

TEST_CASE("topmostVectorLayerAt picks the topmost shape actually hit (geometry-aware)") {
    Document doc(100, 100);
    const auto add = [&](const char* nm, Vec2 at) -> Layer& {
        auto vl = doc.makeVector(nm);
        vec::Object o;
        o.geometry = vec::ParametricShape{vec::RectShape::uniform({40, 40}, 0)};  // local [-20,20]
        o.fill = vec::SolidPaint{{1, 0, 0, 1}};
        vl->setObject(std::move(o));
        vl->setTransform(mosaic::common::Affine2D::translation(at.x, at.y));  // place the centre
        return doc.root().addOnTop(std::move(vl));
    };
    Layer& bottom = add("bottom", {30, 30});  // doc rect [10,50]^2
    Layer& top = add("top", {50, 50});        // doc rect [30,70]^2

    CHECK(topmostVectorLayerAt(doc.root(), {15, 15}) == &bottom);  // only the bottom rect
    CHECK(topmostVectorLayerAt(doc.root(), {40, 40}) == &top);     // overlap -> topmost
    CHECK(topmostVectorLayerAt(doc.root(), {90, 90}) == nullptr);  // empty space

    // The Move tool's general pick (topmostLayerAt) also hits a vector shape's geometry now, so
    // shapes are click-selectable/movable; empty space still misses.
    CHECK(topmostLayerAt(doc.root(), {15, 15}) == &bottom);
    CHECK(topmostLayerAt(doc.root(), {90, 90}) == nullptr);
}

TEST_CASE("VectorLayer holds one object, derives bounds, and revisions on edit") {
    VectorLayer layer{1, "Shape 1"};
    CHECK_FALSE(layer.hasObject());
    CHECK_FALSE(layer.contentBounds().has_value());

    const std::uint64_t rev0 = layer.contentRevision();
    vec::Object obj;
    obj.geometry = polylineRing({{-5, -5}, {5, -5}, {5, 5}, {-5, 5}});
    obj.fill = vec::SolidPaint{{1, 0, 0, 1}};
    layer.setObject(std::move(obj));

    REQUIRE(layer.hasObject());
    CHECK(layer.contentRevision() > rev0);  // edit invalidated the cache
    const auto b = layer.contentBounds();
    REQUIRE(b.has_value());
    CHECK(b->w == Approx(10.0));
    CHECK(b->h == Approx(10.0));

    layer.clearObject();
    CHECK_FALSE(layer.hasObject());
    CHECK_FALSE(layer.contentBounds().has_value());
}

TEST_CASE("contains: winding rule, even-odd rule, and an even-odd donut hole") {
    const vec::Contours square = {ring({{-5, -5}, {5, -5}, {5, 5}, {-5, 5}})};
    CHECK(vec::contains(square, {0, 0}, vec::FillRule::NonZero));
    CHECK_FALSE(vec::contains(square, {6, 0}, vec::FillRule::NonZero));

    // Outer 10x10 + inner 4x4, same winding: even-odd makes the inner square a hole.
    const vec::Contours donut = {ring({{-5, -5}, {5, -5}, {5, 5}, {-5, 5}}),
                                 ring({{-2, -2}, {2, -2}, {2, 2}, {-2, 2}})};
    CHECK_FALSE(vec::contains(donut, {0, 0}, vec::FillRule::EvenOdd));  // in the hole
    CHECK(vec::contains(donut, {3.5, 0}, vec::FillRule::EvenOdd));      // in the ring
    CHECK(vec::contains(donut, {0, 0}, vec::FillRule::NonZero));        // nonzero fills it solid
}

TEST_CASE("distanceToOutline measures the nearest edge") {
    const vec::Contours sq = {ring({{-5, -5}, {5, -5}, {5, 5}, {-5, 5}})};
    CHECK(vec::distanceToOutline(sq, {0, 0}) == Approx(5.0));  // centre to nearest edge
    CHECK(vec::distanceToOutline(sq, {5, 0}) == Approx(0.0));  // on the edge
    CHECK(vec::distanceToOutline(sq, {7, 0}) == Approx(2.0));  // outside
}

TEST_CASE("hitTest picks the fill interior and the stroke band") {
    vec::Object filled;
    filled.geometry = vec::ParametricShape{vec::RectShape::uniform({10, 10}, 0)};
    filled.fill = vec::SolidPaint{{1, 1, 1, 1}};
    CHECK(vec::hitTest(filled, {0, 0}));        // inside the fill
    CHECK_FALSE(vec::hitTest(filled, {9, 9}));  // outside the box

    vec::Object strokeOnly;
    strokeOnly.geometry = vec::ParametricShape{vec::RectShape::uniform({10, 10}, 0)};
    strokeOnly.fill = vec::NoPaint{};
    strokeOnly.stroke.enabled = true;
    strokeOnly.stroke.width = 2.0;                       // half-width band == 1
    CHECK_FALSE(vec::hitTest(strokeOnly, {0, 0}));       // unfilled interior -> miss
    CHECK(vec::hitTest(strokeOnly, {5, 0}));             // on the edge
    CHECK(vec::hitTest(strokeOnly, {5.9, 0}));           // within the band
    CHECK_FALSE(vec::hitTest(strokeOnly, {7, 0}));       // beyond the band
}

TEST_CASE("SetVectorObjectCommand sets, clears, and round-trips through undo/redo") {
    Document doc{64, 64};
    const LayerId id = doc.mintLayerId();
    doc.root().addOnTop(std::make_unique<VectorLayer>(id, "Shape 1"));
    auto* vl = doc.find(id)->as<VectorLayer>();
    REQUIRE(vl != nullptr);
    REQUIRE_FALSE(vl->hasObject());

    vec::Object obj;
    obj.geometry = vec::ParametricShape{vec::RectShape::uniform({20, 10}, 0)};
    obj.fill = vec::SolidPaint{{0, 0, 1, 1}};
    doc.commands().push(std::make_unique<SetVectorObjectCommand>(id, obj, "Add Rectangle"));

    REQUIRE(vl->hasObject());
    CHECK(std::holds_alternative<vec::SolidPaint>(vl->object()->fill));
    CHECK(doc.commands().undoName() == "Add Rectangle");

    doc.commands().undo();
    CHECK_FALSE(vl->hasObject());
    doc.commands().redo();
    REQUIRE(vl->hasObject());  // redo re-applies the stored object

    // A second edit that clears the object (nullopt); undo must restore the rectangle.
    doc.commands().push(std::make_unique<SetVectorObjectCommand>(id, std::nullopt, "Delete Shape"));
    CHECK_FALSE(vl->hasObject());
    doc.commands().undo();
    REQUIRE(vl->hasObject());
    CHECK(std::holds_alternative<vec::SolidPaint>(vl->object()->fill));
}

namespace {
std::uint8_t alphaAt(const mosaic::common::Image& img, std::uint32_t x, std::uint32_t y) {
    return img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3];
}
std::uint8_t channelAt(const mosaic::common::Image& img, std::uint32_t x, std::uint32_t y, int ch) {
    return img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + ch];
}
}  // namespace

TEST_CASE("rasterizeFill: a solid rect covers its mapped pixel region") {
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::RectShape::uniform({10, 10}, 0)};  // local [-5,5]^2
    o.fill = vec::SolidPaint{{1, 0, 0, 1}};                          // opaque red
    const auto toPixel = mosaic::common::Affine2D::translation(10, 10);  // -> pixel [5,15]
    const auto img = vec::rasterizeFill(o, 20, 20, toPixel);

    REQUIRE(img.width == 20);
    CHECK(alphaAt(img, 10, 10) == 255);          // interior, fully covered
    CHECK(channelAt(img, 10, 10, 0) == 255);     // red
    CHECK(channelAt(img, 10, 10, 2) == 0);       // not blue
    CHECK(alphaAt(img, 0, 0) == 0);              // outside the shape
    CHECK(alphaAt(img, 18, 18) == 0);            // outside the shape
}

TEST_CASE("rasterizeFill: a fractional edge produces partial (anti-aliased) coverage") {
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::RectShape::uniform({9, 9}, 0)};  // local [-4.5,4.5]
    o.fill = vec::SolidPaint{{1, 1, 1, 1}};
    const auto toPixel = mosaic::common::Affine2D::translation(10, 10);  // edges at x=5.5, 14.5
    const auto img = vec::rasterizeFill(o, 20, 20, toPixel);

    // Column 5 spans [5,6) but the shape starts at x=5.5 -> ~half coverage on a fully-inside row.
    const std::uint8_t edge = alphaAt(img, 5, 10);
    CHECK(edge > 100);
    CHECK(edge < 160);
    CHECK(alphaAt(img, 10, 10) == 255);  // interior still solid
}

TEST_CASE("rasterizeFill: a linear gradient runs red->blue across the shape") {
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::RectShape::uniform({10, 10}, 0)};  // local [-5,5]
    vec::Gradient grad;
    grad.type = vec::GradientType::Linear;
    grad.stops = {{0.0, {1, 0, 0, 1}}, {1.0, {0, 0, 1, 1}}};
    // gradient unit-space x in [0,1] -> local x in [-5,5]:  local = 10*gx - 5.
    grad.transform = mosaic::common::Affine2D{10, 0, -5, 0, 1, 0};
    o.fill = grad;
    const auto toPixel = mosaic::common::Affine2D::translation(10, 10);  // shape at pixel [5,15]
    const auto img = vec::rasterizeFill(o, 20, 20, toPixel);

    const int redLeft = channelAt(img, 6, 10, 0), blueLeft = channelAt(img, 6, 10, 2);
    const int redRight = channelAt(img, 14, 10, 0), blueRight = channelAt(img, 14, 10, 2);
    CHECK(redLeft > redRight);    // redder on the left
    CHECK(blueRight > blueLeft);  // bluer on the right
}

TEST_CASE("rasterizeFillF keeps sub-8-bit precision (no quantization round-trip)") {
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::RectShape::uniform({20, 20}, 0)};  // local [-10,10]
    vec::Gradient grad;
    grad.type = vec::GradientType::Linear;
    grad.stops = {{0.0, {0, 0, 0, 1}}, {1.0, {1, 1, 1, 1}}};            // black -> white
    grad.transform = mosaic::common::Affine2D{20, 0, -10, 0, 1, 0};     // gx[0,1] -> local x[-10,10]
    o.fill = grad;
    const auto toPixel = mosaic::common::Affine2D::translation(10, 10);  // shape fills [0,20]
    const auto imgF = vec::rasterizeFillF(o, 20, 20, toPixel);

    double maxDev = 0.0;  // distance from the nearest 8-bit level: ~0 if we were quantizing
    for (std::uint32_t x = 0; x < 20; ++x) {
        const float v = imgF.at(x, 10).r;
        maxDev = std::max(maxDev, std::abs(static_cast<double>(v) - std::round(v * 255.0) / 255.0));
    }
    CHECK(maxDev > 1e-4);  // genuinely float, not crushed to 256 levels before compositing
}

TEST_CASE("rasterizeCoverage: a full-canvas rect covers every pixel") {
    const vec::Contours full = {[] {
        vec::Contour c;
        c.closed = true;
        c.points = {{0, 0}, {8, 0}, {8, 8}, {0, 8}};
        return c;
    }()};
    const auto cov = vec::rasterizeCoverage(full, 8, 8, vec::FillRule::NonZero);
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 8; ++x) CHECK(cov.at(x, y) == Approx(1.0));
}

TEST_CASE("compositor renders a vector layer's fill into the document") {
    Document doc{32, 32};
    const LayerId id = doc.mintLayerId();
    auto layer = std::make_unique<VectorLayer>(id, "Rect");
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::RectShape::uniform({16, 16}, 0)};  // local [-8,8]
    o.fill = vec::SolidPaint{{1, 0, 0, 1}};                          // opaque red
    layer->setObject(std::move(o));
    layer->setTransform(mosaic::common::Affine2D::translation(16, 16));  // centre -> pixel [8,24]
    doc.root().addOnTop(std::move(layer));

    const auto res = mosaic::render::composite(doc);
    const auto& img = res.image;
    REQUIRE(img.width == 32);
    CHECK(channelAt(img, 16, 16, 0) > 200);  // shape centre is red
    CHECK(channelAt(img, 16, 16, 1) < 80);
    CHECK(channelAt(img, 16, 16, 2) < 80);
    // A far corner is NOT the red fill (transparent or the checkerboard background).
    const bool cornerIsRed = channelAt(img, 0, 0, 0) > 200 && channelAt(img, 0, 0, 1) < 80 &&
                             channelAt(img, 0, 0, 2) < 80;
    CHECK_FALSE(cornerIsRed);
}

TEST_CASE("stroke: a stroked square is hollow, blue, and miters its corners") {
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::RectShape::uniform({20, 20}, 0)};  // local [-10,10]
    o.fill = vec::NoPaint{};
    o.stroke.enabled = true;
    o.stroke.paint = vec::SolidPaint{{0, 0, 1, 1}};  // blue
    o.stroke.width = 4.0;                            // half-width 2 straddles each edge
    o.stroke.join = vec::LineJoin::Miter;
    const auto toPixel = mosaic::common::Affine2D::translation(16, 16);  // rect -> pixel [6,26]
    const auto img = vec::rasterizeObjectF(o, 32, 32, toPixel);

    CHECK(alphaF(img, 6, 16) > 0.5);          // on the left edge -> stroked
    CHECK(chanF(img, 6, 16, 2) > 0.8);        // and blue
    CHECK(alphaF(img, 16, 16) == Approx(0.0));// hollow centre (no fill)
    CHECK(alphaF(img, 0, 0) == Approx(0.0));  // far outside
    // Outer corner pixel (local (-11,-11)) is reachable ONLY via the miter join piece.
    CHECK(alphaF(img, 5, 5) > 0.5);
}

TEST_CASE("stroke: round caps extend past an open end, butt caps do not") {
    vec::SubPath sp;
    sp.closed = false;
    sp.nodes = {node({0, 0}), node({20, 0})};  // horizontal segment, local
    vec::Object line;
    line.geometry = vec::Path{{sp}, vec::FillRule::NonZero};
    line.fill = vec::NoPaint{};
    line.stroke.enabled = true;
    line.stroke.paint = vec::SolidPaint{{1, 1, 1, 1}};
    line.stroke.width = 4.0;  // half-width 2
    const auto toPixel = mosaic::common::Affine2D::translation(6, 16);  // ends at pixel (6,16),(26,16)

    line.stroke.cap = vec::LineCap::Round;
    const auto round = vec::rasterizeObjectF(line, 32, 32, toPixel);
    CHECK(alphaF(round, 13, 16) > 0.5);  // on the line
    CHECK(alphaF(round, 27, 16) > 0.3);  // 1px past the end, within the round cap

    line.stroke.cap = vec::LineCap::Butt;
    const auto butt = vec::rasterizeObjectF(line, 32, 32, toPixel);
    CHECK(alphaF(butt, 13, 16) > 0.5);            // still on the line
    CHECK(alphaF(butt, 27, 16) == Approx(0.0));   // butt does not extend
}

TEST_CASE("stroke: a dash pattern leaves gaps along the path") {
    vec::SubPath sp;
    sp.closed = false;
    sp.nodes = {node({0, 0}), node({40, 0})};
    vec::Object dl;
    dl.geometry = vec::Path{{sp}, vec::FillRule::NonZero};
    dl.fill = vec::NoPaint{};
    dl.stroke.enabled = true;
    dl.stroke.paint = vec::SolidPaint{{1, 1, 1, 1}};
    dl.stroke.width = 4.0;
    dl.stroke.cap = vec::LineCap::Butt;
    dl.stroke.dashArray = {6.0, 6.0};  // 6 on, 6 off
    const auto toPixel = mosaic::common::Affine2D::translation(4, 16);
    const auto img = vec::rasterizeObjectF(dl, 48, 32, toPixel);

    CHECK(alphaF(img, 5, 16) > 0.5);            // local x≈1 -> first "on" dash
    CHECK(alphaF(img, 13, 16) == Approx(0.0));  // local x≈9 -> in the first "off" gap
}

TEST_CASE("line paint modes: Hollow draws only the border, Outlined adds a contrasting edge") {
    // A horizontal weight-8 line centred at local y=0, mapped so its centreline lands on pixel y=16;
    // the thick band covers pixel rows [12,20].
    vec::LineShape ls;
    ls.a = {-10, 0};
    ls.b = {10, 0};
    ls.borderWidth = 2.0;
    vec::Object line;
    line.geometry = vec::ParametricShape{ls};
    line.stroke.enabled = true;
    line.stroke.width = 8.0;  // the weight
    line.stroke.cap = vec::LineCap::Butt;
    line.stroke.paint = vec::SolidPaint{{1, 0, 0, 1}};  // line colour (red)
    const auto toPixel = mosaic::common::Affine2D::translation(16, 16);

    SUBCASE("Hollow: empty interior, an inked border at the band edge, in the line colour") {
        std::get<vec::LineShape>(std::get<vec::ParametricShape>(line.geometry)).paint =
            vec::LineShape::Paint::Hollow;
        const auto img = vec::rasterizeObjectF(line, 32, 32, toPixel);
        CHECK(alphaF(img, 16, 16) == Approx(0.0));  // interior is empty (hollow)
        CHECK(alphaF(img, 16, 12) > 0.5);           // the top band edge is the border
        CHECK(chanF(img, 16, 12, 0) > 0.8);         // red (the line colour)
    }
    SUBCASE("Outlined: a filled body in the line colour, a border ring in the fill colour") {
        std::get<vec::LineShape>(std::get<vec::ParametricShape>(line.geometry)).paint =
            vec::LineShape::Paint::Outlined;
        line.fill = vec::SolidPaint{{0, 0, 1, 1}};  // border colour (blue)
        const auto img = vec::rasterizeObjectF(line, 32, 32, toPixel);
        CHECK(chanF(img, 16, 16, 0) > 0.8);  // body is red (the line colour)
        CHECK(alphaF(img, 16, 11) > 0.5);    // a border ring sits just outside the weight edge
        CHECK(chanF(img, 16, 11, 2) > 0.8);  // and it is blue (the fill = border colour)
    }
    SUBCASE("dashes carry through to a Hollow line (the border is dashed, not solid)") {
        std::get<vec::LineShape>(std::get<vec::ParametricShape>(line.geometry)).paint =
            vec::LineShape::Paint::Hollow;
        line.stroke.dashArray = {8.0, 8.0}; // centreline x 6..26 -> on [6,14), off [14,22), on [22,26)
        const auto img = vec::rasterizeObjectF(line, 32, 32, toPixel);
        CHECK(alphaF(img, 10, 12) > 0.4);           // within the first dash, on the border
        CHECK(alphaF(img, 18, 12) == Approx(0.0));  // within the first gap -> empty
    }
}

TEST_CASE("a bent line flattens to a curve through midpoint+bend; zero bend stays straight") {
    vec::LineShape l;
    l.a = {0, 0};
    l.b = {10, 0};
    l.bend = {0, -5}; // bow up (y-down): the curve midpoint should reach (5, -5)
    vec::Object o;
    o.geometry = vec::ParametricShape{l};
    const vec::Contours cs = vec::flatten(o.geometry);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].points.size() > 2);  // curved, not a 2-point segment
    CHECK_FALSE(cs[0].closed);       // still an open stroke
    double best = 1e9;
    for (const auto& p : cs[0].points)
        best = std::min(best, (p - mosaic::common::Vec2{5, -5}).length());
    CHECK(best < 0.5); // the curve passes through midpoint + bend

    std::get<vec::LineShape>(std::get<vec::ParametricShape>(o.geometry)).bend = {0, 0};
    const vec::Contours straight = vec::flatten(o.geometry);
    CHECK(straight[0].points.size() == 2); // zero bend == a straight segment
}

TEST_CASE("pixelBoundsOf clamps to the canvas and rejects off-canvas geometry") {
    const vec::Contours on = {ring({{10, 10}, {30, 10}, {30, 30}, {10, 30}})};
    const auto b = vec::pixelBoundsOf(on, 40, 40, 1);
    REQUIRE(b.has_value());
    CHECK(b->x0 == 9);   // floor(10) - pad
    CHECK(b->y0 == 9);
    CHECK(b->x1 == 31);  // ceil(30) + pad
    CHECK(b->y1 == 31);

    const vec::Contours off = {ring({{100, 100}, {120, 100}, {120, 120}, {100, 120}})};
    CHECK_FALSE(vec::pixelBoundsOf(off, 40, 40, 1).has_value());  // entirely past the canvas
}

TEST_CASE("rasterizeCoverage bounds its buffer to the bbox but matches the full sweep") {
    const vec::Contours c = {ring({{10, 10}, {30, 10}, {30, 30}, {10, 30}})};
    const auto bbox = vec::pixelBoundsOf(c, 64, 64, 1);
    REQUIRE(bbox.has_value());
    const auto clipped = vec::rasterizeCoverage(c, 64, 64, vec::FillRule::NonZero, 4, bbox);
    const auto full = vec::rasterizeCoverage(c, 64, 64, vec::FillRule::NonZero);

    CHECK(clipped.width == bbox->width());       // sub-rect storage, not the whole 64x64
    CHECK(clipped.height == bbox->height());
    CHECK(clipped.a.size() < full.a.size());     // genuinely smaller allocation
    CHECK(clipped.at(20, 20) == Approx(full.at(20, 20)));  // identical where they overlap
    CHECK(clipped.at(29, 29) == Approx(full.at(29, 29)));
    CHECK(clipped.at(20, 20) == Approx(1.0));    // interior fully covered
}

TEST_CASE("stroke alignment: Inside stays within the fill, Outside stays outside, Center straddles") {
    vec::Object base;
    base.geometry = vec::ParametricShape{vec::RectShape::uniform({20, 20}, 0)};  // local [-10,10]
    base.fill = vec::NoPaint{};
    base.stroke.enabled = true;
    base.stroke.paint = vec::SolidPaint{{1, 1, 1, 1}};
    base.stroke.width = 4.0;
    const auto toPixel = mosaic::common::Affine2D::translation(20, 20);  // pixel rect [10,30]
    // Left edge maps to pixel x=10 on the mid-height row y=20; x=9 is just outside it, x=11 inside.

    vec::Object inside = base;
    inside.stroke.align = vec::StrokeAlign::Inside;
    const auto in = vec::rasterizeObjectF(inside, 40, 40, toPixel);
    CHECK(alphaF(in, 11, 20) > 0.8);            // inside band -> painted
    CHECK(alphaF(in, 9, 20) == Approx(0.0));    // outside the edge -> NOT painted (inside-aligned)
    CHECK(alphaF(in, 20, 20) == Approx(0.0));   // hollow centre, no fill

    vec::Object outside = base;
    outside.stroke.align = vec::StrokeAlign::Outside;
    const auto out = vec::rasterizeObjectF(outside, 40, 40, toPixel);
    CHECK(alphaF(out, 9, 20) > 0.8);            // outside band -> painted
    CHECK(alphaF(out, 11, 20) == Approx(0.0));  // inside the edge -> NOT painted (outside-aligned)

    vec::Object center = base;
    center.stroke.align = vec::StrokeAlign::Center;
    const auto ctr = vec::rasterizeObjectF(center, 40, 40, toPixel);
    CHECK(alphaF(ctr, 9, 20) > 0.8);            // centred straddles the edge -> both sides painted
    CHECK(alphaF(ctr, 11, 20) > 0.8);
}

TEST_CASE("contentBounds widens by the stroke's outward reach per alignment") {
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::RectShape::uniform({20, 10}, 0)};  // local [-10,5]..[10,5]
    o.stroke.enabled = true;
    o.stroke.width = 4.0;

    o.stroke.align = vec::StrokeAlign::Center;  // +half-width (2) all round
    auto b = vec::contentBounds(o);
    REQUIRE(b.has_value());
    CHECK(b->w == Approx(24.0));
    CHECK(b->h == Approx(14.0));

    o.stroke.align = vec::StrokeAlign::Outside;  // +full width (4) all round
    b = vec::contentBounds(o);
    REQUIRE(b.has_value());
    CHECK(b->w == Approx(28.0));

    o.stroke.align = vec::StrokeAlign::Inside;  // no outward growth
    b = vec::contentBounds(o);
    REQUIRE(b.has_value());
    CHECK(b->w == Approx(20.0));

    o.stroke.enabled = false;  // disabled stroke -> bare geometry bounds
    b = vec::contentBounds(o);
    REQUIRE(b.has_value());
    CHECK(b->w == Approx(20.0));
}

TEST_CASE("rasterizeObjectF: antialias=false hardens edge coverage to 0/1") {
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::RectShape::uniform({9, 9}, 0)};  // local [-4.5,4.5]
    o.fill = vec::SolidPaint{{1, 1, 1, 1}};
    const auto toPixel = mosaic::common::Affine2D::translation(10, 10);  // edges at x=5.5, 14.5

    const auto aa = vec::rasterizeObjectF(o, 20, 20, toPixel, 0.25, /*antialias=*/true);
    const auto hard = vec::rasterizeObjectF(o, 20, 20, toPixel, 0.25, /*antialias=*/false);
    // The fractional edge column is partial under AA, but snaps to a hard 0 or 1 without it.
    const float edgeAA = alphaF(aa, 5, 10);
    const float edgeHard = alphaF(hard, 5, 10);
    CHECK(edgeAA > 0.1f);
    CHECK(edgeAA < 0.9f);                                  // genuinely partial coverage
    CHECK((edgeHard == Approx(0.0f) || edgeHard == Approx(1.0f)));  // hardened
    CHECK(alphaF(hard, 10, 10) == Approx(1.0f));           // interior still solid
}

TEST_CASE("stroke: fill and stroke compose (red fill under a blue stroke)") {
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::RectShape::uniform({20, 20}, 0)};
    o.fill = vec::SolidPaint{{1, 0, 0, 1}};  // red
    o.stroke.enabled = true;
    o.stroke.paint = vec::SolidPaint{{0, 0, 1, 1}};  // blue
    o.stroke.width = 4.0;
    const auto toPixel = mosaic::common::Affine2D::translation(16, 16);
    const auto img = vec::rasterizeObjectF(o, 32, 32, toPixel);

    CHECK(chanF(img, 16, 16, 0) > 0.8);  // centre is the red fill
    CHECK(chanF(img, 16, 16, 2) < 0.2);
    CHECK(chanF(img, 6, 16, 2) > 0.8);   // edge is the blue stroke
    CHECK(chanF(img, 6, 16, 0) < 0.2);
}

// ---------------------------------------------------------------------------------------------
// rasterizeCoverage's active-edge sweep.
//
// The scanline pass used to test EVERY edge of the object at every scanline sample; it now walks
// the edges in ylo order, admits each as its scanline arrives and retires it when its yhi passes.
// That is a second implementation of "which edges cross this scanline", and the two are only ever
// compared here. The reference below is the ORIGINAL linear scan, written out verbatim, so a sweep
// that admits an edge a sample early, retires one a sample late, or reorders the crossings fails
// against the thing it replaced rather than against a recorded picture.
//
// ⚠ BIT-EXACT, not approximate. `xs` is sorted by x with a comparator that ignores the winding
// direction, so two crossings at the same x come out in whichever order they went in -- and the
// winding accumulation reads that order. The sweep keeps its active list in EDGE-INDEX order
// precisely so the question cannot arise; this test is what holds it there.
namespace {

// The pre-sweep pass, verbatim: for every scanline sample, test every edge.
std::vector<float> referenceCoverage(const mosaic::core::vec::Contours& cs,
                                     mosaic::core::vec::FillRule rule, int subsamples,
                                     const mosaic::core::vec::PixelBounds& box) {
    std::vector<float> cov(static_cast<std::size_t>(box.width()) * box.height(), 0.0f);
    std::vector<std::array<double, 4>> edges;
    for (const auto& c : cs) {
        const auto& pts = c.points;
        const std::size_t n = pts.size();
        if (n < 2)
            continue;
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2 a = pts[i];
            const Vec2 b = pts[(i + 1) % n];
            if (a.y != b.y)
                edges.push_back({a.x, a.y, b.x, b.y});
        }
    }
    if (edges.empty())
        return cov;
    const int N = std::max(1, subsamples);
    const float weight = 1.0f / static_cast<float>(N);
    const auto addSpan = [&](float* row, double xa, double xb) {
        xa = std::clamp(xa, static_cast<double>(box.x0), static_cast<double>(box.x1));
        xb = std::clamp(xb, static_cast<double>(box.x0), static_cast<double>(box.x1));
        if (xb <= xa)
            return;
        const int ixa = static_cast<int>(std::floor(xa));
        const int ixb = static_cast<int>(std::floor(xb));
        const int hi = static_cast<int>(box.x1);
        const int base = static_cast<int>(box.x0);
        if (ixa == ixb) {
            if (ixa < hi)
                row[ixa - base] += static_cast<float>(xb - xa) * weight;
            return;
        }
        if (ixa < hi)
            row[ixa - base] += static_cast<float>((ixa + 1) - xa) * weight;
        for (int x = ixa + 1; x < ixb && x < hi; ++x)
            row[x - base] += weight;
        if (ixb < hi)
            row[ixb - base] += static_cast<float>(xb - ixb) * weight;
    };
    std::vector<std::pair<double, int>> xs;
    for (std::uint32_t y = box.y0; y < box.y1; ++y) {
        float* row = &cov[static_cast<std::size_t>(y - box.y0) * box.width()];
        for (int s = 0; s < N; ++s) {
            const double sy = static_cast<double>(y) + (static_cast<double>(s) + 0.5) / N;
            xs.clear();
            for (const auto& e : edges) {
                const double ay = e[1], by = e[3];
                if (sy < std::min(ay, by) || sy >= std::max(ay, by))
                    continue;
                const double t = (sy - ay) / (by - ay);
                xs.push_back({e[0] + t * (e[2] - e[0]), by > ay ? 1 : -1});
            }
            if (xs.size() < 2)
                continue;
            std::sort(xs.begin(), xs.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            int w = 0;
            for (std::size_t k = 0; k + 1 < xs.size(); ++k) {
                w += (rule == mosaic::core::vec::FillRule::NonZero) ? xs[k].second : 1;
                const bool inside =
                    (rule == mosaic::core::vec::FillRule::NonZero) ? (w != 0) : (w & 1);
                if (inside)
                    addSpan(row, xs[k].first, xs[k + 1].first);
            }
        }
    }
    return cov;
}

// A rosette: `lobes` cusps between an outer and an inner radius, flattened finely enough that the
// edge list is large and short-lived -- the shape the sweep exists for, and the one the fixture's
// Rosette layer is.
mosaic::core::vec::Contours rosetteContours(int lobes, double outer, double inner, double cx,
                                            double cy, int steps) {
    mosaic::core::vec::Contour c;
    c.closed = true;
    for (int i = 0; i < steps; ++i) {
        const double t = 2.0 * std::acos(-1.0) * i / steps;
        const double r = inner + (outer - inner) * 0.5 * (1.0 + std::cos(lobes * t));
        c.points.push_back({cx + r * std::cos(t), cy + r * std::sin(t)});
    }
    return {c};
}

} // namespace

TEST_CASE("rasterizeCoverage: the active-edge sweep matches the linear scan bit for bit") {
    using namespace mosaic::core::vec;
    struct Case {
        const char* what;
        Contours cs;
        FillRule rule;
        int subsamples;
    };
    // A concave self-overlapping star exercises NonZero vs EvenOdd differing; two nested squares
    // wound the same way exercise a non-zero interior; the rosette is the many-short-edges case.
    Contours star;
    {
        Contour c;
        c.closed = true;
        for (int i = 0; i < 10; ++i) {
            const double t = 2.0 * std::acos(-1.0) * i / 10.0;
            const double r = (i % 2 == 0) ? 40.0 : 15.0;
            c.points.push_back({48.0 + r * std::cos(t), 48.0 + r * std::sin(t)});
        }
        star = {c};
    }
    Contours nested;
    {
        Contour a, b;
        a.closed = b.closed = true;
        a.points = {{8.0, 8.0}, {88.0, 8.0}, {88.0, 88.0}, {8.0, 88.0}};
        b.points = {{28.0, 28.0}, {68.0, 28.0}, {68.0, 68.0}, {28.0, 68.0}};
        nested = {a, b};
    }
    // Fractional coordinates on purpose: integer vertices make every crossing land on a sample
    // boundary, which is the one case where an off-by-one in the sweep would not show.
    Contours slivers;
    {
        Contour c;
        c.closed = true;
        c.points = {{10.25, 4.75}, {10.5, 90.5}, {11.75, 90.25}, {11.5, 4.5}};
        slivers = {c};
    }
    // ⚠ VERTICES EXACTLY ON A SAMPLE LINE. With N subsamples the scanline samples sit at
    // y + (s + 0.5)/N, so at N=4 they are y.125 / y.375 / y.625 / y.875 -- and the admit/retire
    // tests are `ylo <= sy` and `yhi <= sy`, whose boundary is only reachable when a vertex lands
    // on one of those exactly. Every other case in this list has vertices that miss them, which
    // makes a `<=` silently interchangeable with a `<`. These do not.
    Contours onSample;
    {
        Contour c;
        c.closed = true;
        c.points = {{20.0, 12.125}, {70.0, 12.125}, {70.0, 60.875}, {45.0, 44.375}, {20.0, 60.875}};
        onSample = {c};
    }
    Contours onSampleHalf; // the same idea at N=2, where the samples are y.25 / y.75
    {
        Contour c;
        c.closed = true;
        c.points = {{18.0, 9.25}, {77.0, 30.75}, {60.0, 80.25}, {24.0, 55.75}};
        onSampleHalf = {c};
    }
    // ⚠ AND THE VERTEX MUST BE A MONOTONE PASS-THROUGH, not a local extremum. At a y-extremum the
    // two edges meeting there carry OPPOSITE windings, so admitting or retiring one sample early
    // adds a matched pair of crossings at the same x -- a zero-width span, invisible. Where the
    // contour merely passes through (prev.y < v.y < next.y) the two carry the SAME winding, the
    // parity flips, and an off-by-one shows. Under EvenOdd it shows as a whole scanline.
    Contours passThrough;
    {
        Contour c;
        c.closed = true; // y runs 8.125 -> 24.375 -> 56.625 -> 88.875 -> 40.125, so the second
        c.points = {{20.0, 8.125},
                    {60.0, 24.375},
                    {70.0, 56.625},
                    {40.0, 88.875},
                    {12.0, 40.125}}; // and third vertices pass straight through
        passThrough = {c};
    }
    const Case cases[] = {
        {"pass-through vertex on a sample line", passThrough, FillRule::EvenOdd, 4},
        {"pass-through vertex on a sample line, nonzero", passThrough, FillRule::NonZero, 4},
        {"vertices on the sample lines", onSample, FillRule::NonZero, 4},
        {"vertices on the sample lines, evenodd", onSample, FillRule::EvenOdd, 4},
        {"vertices on the sample lines, N=2", onSampleHalf, FillRule::NonZero, 2},
        {"star nonzero", star, FillRule::NonZero, 4},
        {"star evenodd", star, FillRule::EvenOdd, 4},
        {"nested nonzero", nested, FillRule::NonZero, 4},
        {"nested evenodd", nested, FillRule::EvenOdd, 4},
        {"slivers", slivers, FillRule::NonZero, 4},
        {"star 1 subsample", star, FillRule::NonZero, 1},
        {"star 7 subsamples", star, FillRule::NonZero, 7},
        {"rosette 41 lobes", rosetteContours(41, 44.0, 18.0, 48.0, 48.0, 900), FillRule::NonZero,
         4},
    };
    constexpr std::uint32_t kW = 96, kH = 96;
    std::size_t compared = 0;
    for (const Case& c : cases) {
        INFO("case=" << std::string(c.what));
        // Both the clipped sub-rect and the whole window: the sweep is seeded off the box, and a
        // band that starts partway down the shape is exactly where a re-seed can be wrong.
        const std::optional<PixelBounds> tight = pixelBoundsOf(c.cs, kW, kH);
        REQUIRE(tight.has_value());
        const PixelBounds boxes[] = {*tight, PixelBounds{0, 0, kW, kH}};
        for (const PixelBounds& box : boxes) {
            const CoverageBuffer got = rasterizeCoverage(c.cs, kW, kH, c.rule, c.subsamples, box);
            const std::vector<float> want = referenceCoverage(c.cs, c.rule, c.subsamples, box);
            REQUIRE(got.a.size() == want.size());
            for (std::size_t i = 0; i < want.size(); ++i) {
                if (got.a[i] != want[i])
                    INFO("index " << i << " got " << got.a[i] << " want " << want[i]);
                REQUIRE(got.a[i] == want[i]);
                ++compared;
            }
        }
    }
    CHECK(compared > 100000);
}

TEST_CASE("rasterizeObjectF's reported rect bounds every texel it painted") {
    // The rect is a CONTRACT, not a hint: the text renderer composites each styled run out of a
    // full-window image and now walks only this rect, so a texel painted outside it would simply
    // never reach the block. Checked the only way that means anything -- scan the whole image and
    // require every non-transparent texel to fall inside.
    using namespace mosaic::core::vec;
    const auto probe = [](const Object& obj, std::uint32_t W, std::uint32_t H,
                          const mosaic::common::Affine2D& toPixel, const char* what) {
        mosaic::common::Rect written;
        const mosaic::common::ImageF img =
            rasterizeObjectF(obj, W, H, toPixel, 0.25, true, &written);
        REQUIRE(img.width == W);
        std::size_t painted = 0;
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) {
                if (img.at(x, y).a <= 0.0f)
                    continue;
                ++painted;
                INFO("case=" << std::string(what) << " texel " << x << "," << y << " outside "
                             << written.x << "," << written.y << " " << written.w << "x"
                             << written.h);
                REQUIRE(static_cast<double>(x) >= written.x);
                REQUIRE(static_cast<double>(y) >= written.y);
                REQUIRE(static_cast<double>(x) < written.right());
                REQUIRE(static_cast<double>(y) < written.bottom());
            }
        return painted;
    };

    // A filled path well inside the window: the rect must be a real sub-rect, or the test would
    // pass trivially on a rect that is just the whole image.
    {
        Object obj;
        Path p;
        SubPath sp;
        sp.closed = true;
        for (const mosaic::common::Vec2 v :
             {mosaic::common::Vec2{20.0, 14.0}, mosaic::common::Vec2{44.0, 18.0},
              mosaic::common::Vec2{38.0, 40.0}, mosaic::common::Vec2{18.0, 36.0}})
            sp.nodes.push_back(Node{v, v, v});
        p.subpaths.push_back(sp);
        obj.geometry = p;
        obj.fill = SolidPaint{mosaic::common::ColorF{1.0f, 0.2f, 0.1f, 1.0f}};
        obj.stroke.enabled = false;
        mosaic::common::Rect written;
        const mosaic::common::ImageF img = rasterizeObjectF(
            obj, 96, 64, mosaic::common::Affine2D::identity(), 0.25, true, &written);
        (void)img;
        CHECK(written.w < 96.0); // a real sub-rect...
        CHECK(written.h < 64.0);
        CHECK(probe(obj, 96, 64, mosaic::common::Affine2D::identity(), "fill") > 100);
    }
    // ... and with a STROKE on, which paints outside the fill's own bounds -- the arm where a rect
    // derived from the fill geometry alone would be too small.
    {
        Object obj;
        Path p;
        SubPath sp;
        sp.closed = true;
        for (const mosaic::common::Vec2 v :
             {mosaic::common::Vec2{30.0, 24.0}, mosaic::common::Vec2{50.0, 24.0},
              mosaic::common::Vec2{50.0, 40.0}, mosaic::common::Vec2{30.0, 40.0}})
            sp.nodes.push_back(Node{v, v, v});
        p.subpaths.push_back(sp);
        obj.geometry = p;
        obj.fill = SolidPaint{mosaic::common::ColorF{0.1f, 0.9f, 0.2f, 1.0f}};
        obj.stroke.enabled = true;
        obj.stroke.width = 7.0;
        obj.stroke.paint = SolidPaint{mosaic::common::ColorF{0.0f, 0.0f, 1.0f, 1.0f}};
        CHECK(probe(obj, 96, 64, mosaic::common::Affine2D::identity(), "fill+stroke") > 100);
    }
    // An object that lands entirely off-canvas paints nothing and must report an EMPTY rect, not a
    // stale or whole-image one -- the text renderer skips the run on exactly that test.
    {
        Object obj;
        Path p;
        SubPath sp;
        sp.closed = true;
        for (const mosaic::common::Vec2 v :
             {mosaic::common::Vec2{500.0, 500.0}, mosaic::common::Vec2{520.0, 500.0},
              mosaic::common::Vec2{520.0, 520.0}})
            sp.nodes.push_back(Node{v, v, v});
        p.subpaths.push_back(sp);
        obj.geometry = p;
        obj.fill = SolidPaint{mosaic::common::ColorF{1.0f, 1.0f, 1.0f, 1.0f}};
        obj.stroke.enabled = false;
        mosaic::common::Rect written{1.0, 2.0, 3.0,
                                     4.0}; // pre-dirtied: it must be RESET, not left alone
        const mosaic::common::ImageF img = rasterizeObjectF(
            obj, 96, 64, mosaic::common::Affine2D::identity(), 0.25, true, &written);
        (void)img;
        CHECK(written.empty());
    }
}
