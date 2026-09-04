// 3D for SHAPES (docs/vector-model.md §11): a vec::Object carrying an Extrude meshes through the
// same builder and renders through the same lane 3D text does. These assert the shape-specific
// half -- which solids an object contributes, what the mesh cache is keyed on, and that a 3D
// vector layer actually composites as a solid -- rather than re-testing the mesher and the
// rasterizer, which tests/test_extrude_mesh.cpp and tests/test_extrude_render.cpp already own.
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/layer_effects.hpp"
#include "core/text/extrude_mesh.hpp"
#include "core/vector/extrude_shape.hpp"
#include "core/vector/flatten.hpp"
#include "core/vector/object.hpp"
#include "core/vector/stroke.hpp"
#include "render/compositor.hpp"

#include <array>
#include <cmath>
#include <doctest/doctest.h>

namespace vec = mosaic::core::vec;
namespace tx = mosaic::core::text;
using mosaic::common::ColorF;
namespace common = mosaic::common;

namespace {

// A 40x30 rectangle path in layer-local space, filled, centred on the local origin the way the
// Shape tool authors one.
vec::Object rectObject(bool filled = true, bool stroked = false) {
    vec::Path p;
    vec::SubPath sp;
    sp.closed = true;
    for (const mosaic::common::Vec2 v :
         {mosaic::common::Vec2{-20, -15}, {20, -15}, {20, 15}, {-20, 15}})
        sp.nodes.push_back(vec::Node{v, v, v});
    p.subpaths.push_back(std::move(sp));
    vec::Object o;
    o.geometry = std::move(p);
    if (filled)
        o.fill = vec::SolidPaint{ColorF{0.9f, 0.2f, 0.2f, 1.0f}};
    o.stroke.enabled = stroked;
    if (stroked) {
        o.stroke.paint = vec::SolidPaint{ColorF{0.1f, 0.1f, 0.9f, 1.0f}};
        o.stroke.width = 4.0;
    }
    return o;
}

tx::Extrude flatLitExtrude() {
    tx::Extrude e;
    e.depth = 8.0f;
    e.lightingEnabled = false; // flat self-lit faces: ink presence, not shading, is the question
    return e;
}

int inkedPixels(const mosaic::common::Image& img) {
    int n = 0;
    for (std::size_t p = 3; p < img.rgba.size(); p += 4)
        if (img.rgba[p] > 127)
            ++n;
    return n;
}

} // namespace

TEST_CASE("the fill is run 0 and the stroke run 1; neither paints, neither meshes") {
    const tx::Extrude ex = flatLitExtrude();

    const tx::ExtrudeMesh fillOnly = vec::buildShapeExtrudeMesh(rectObject(true, false), ex);
    REQUIRE(!fillOnly.empty());
    for (const tx::ExtrudeMeshRange& r : fillOnly.ranges)
        CHECK(r.runIndex == 0);

    const tx::ExtrudeMesh both = vec::buildShapeExtrudeMesh(rectObject(true, true), ex);
    bool sawRun1 = false;
    for (const tx::ExtrudeMeshRange& r : both.ranges)
        sawRun1 = sawRun1 || r.runIndex == 1;
    CHECK(sawRun1); // the outline is its own, materialable, solid
    CHECK(both.triangleCount() > fillOnly.triangleCount());

    // A stroke with no fill still extrudes -- an outline-only shape is a real shape.
    const tx::ExtrudeMesh strokeOnly = vec::buildShapeExtrudeMesh(rectObject(false, true), ex);
    REQUIRE(!strokeOnly.empty());
    for (const tx::ExtrudeMeshRange& r : strokeOnly.ranges)
        CHECK(r.runIndex == 1);

    // Neither filled nor stroked draws nothing in 2D, so it must mesh to nothing in 3D.
    CHECK(vec::buildShapeExtrudeMesh(rectObject(false, false), ex).empty());
}

TEST_CASE("the mesh key moves with the geometry and the bevel, never with the camera") {
    const vec::Object o = rectObject();
    const tx::Extrude base = flatLitExtrude();
    const std::uint64_t key = vec::shapeExtrudeMeshKey(o, base);
    CHECK(vec::shapeExtrudeMeshKey(o, base) == key); // stable

    // ⚠ The point of the key: an orbit / relight / recolour must NOT re-mesh (type-tool §10.5).
    tx::Extrude spun = base;
    spun.orientation = mosaic::common::Quat::fromAxisAngle({0, 1, 0}, 0.7);
    spun.perspective = 40.0f;
    spun.lightingEnabled = true;
    spun.reflectCanvas = true;
    CHECK(vec::shapeExtrudeMeshKey(o, spun) == key);
    // A RECOLOUR is render-side too, and the colour is the object's own fill now (§10.4) -- so
    // the key must not move for that either, or every colour pick would re-tessellate the solid.
    vec::Object recoloured = o;
    recoloured.fill = vec::SolidPaint{ColorF{0.1f, 0.9f, 0.3f, 1.0f}};
    CHECK(vec::shapeExtrudeMeshKey(recoloured, spun) == key);

    // ... while anything the SOLID is built from must.
    tx::Extrude deeper = base;
    deeper.depth = 20.0f;
    CHECK(vec::shapeExtrudeMeshKey(o, deeper) != key);
    tx::Extrude bevelled = base;
    bevelled.bevelFront.size = 2.0f;
    CHECK(vec::shapeExtrudeMeshKey(o, bevelled) != key);
    CHECK(vec::shapeExtrudeMeshKey(rectObject(true, true), base) != key); // + the stroke solid
}

TEST_CASE("an extruded vector layer composites as a solid, and its bounds cover it") {
    mosaic::core::Document doc(160, 120);
    mosaic::core::Layer& layer = doc.root().addOnTop(doc.makeVector("Shape"));
    auto* vl = layer.as<mosaic::core::VectorLayer>();
    REQUIRE(vl != nullptr);
    vl->setObject(rectObject());
    vl->setTransform(mosaic::common::Affine2D::translation(80.0, 60.0)); // centre it on the canvas

    mosaic::render::CompositeOptions opts; // true alpha (no checkerboard)
    const mosaic::render::CompositeResult flat =
        mosaic::render::composite(doc, opts, mosaic::render::Backend::Cpu);
    REQUIRE(flat.ok);
    const int flatInk = inkedPixels(flat.image);
    CHECK(flatInk > 0);

    // Turn it 3D, spun about Y so the solid's depth shows as extra width -- a result the flat
    // fill cannot produce, so "did the 3D lane actually run" is answered by the pixels.
    vec::Object o = rectObject();
    tx::Extrude ex = flatLitExtrude();
    ex.depth = 30.0f;
    ex.orientation = mosaic::common::Quat::fromAxisAngle({0, 1, 0}, 0.6);
    o.extrude = ex;
    vl->setObject(o);

    const mosaic::render::CompositeResult solid =
        mosaic::render::composite(doc, opts, mosaic::render::Backend::Cpu);
    REQUIRE(solid.ok);
    CHECK(inkedPixels(solid.image) > 0);
    CHECK(inkedPixels(solid.image) != flatInk); // the silhouette is not the flat rect's

    // The layer's content box must cover the SOLID, not the flat path it was built from: the Move
    // gizmo frames this, and a rotated deep solid reaches outside its own outline.
    const auto box = vl->contentBounds();
    REQUIRE(box.has_value());
    CHECK(box->w >= 40.0);
    CHECK(box->h >= 30.0);
}

// §12 parity: a Layer-Effects colour overlay on a 3D SHAPE is baked onto the solid's faces (the
// text lane's behaviour), not left to the 2D effect pass to smear over the projected rectangle.
// Assert it through the pixels -- the front face must take the overlay's colour.
TEST_CASE("a colour overlay on a 3D shape is baked onto its faces") {
    mosaic::core::Document doc(120, 120);
    mosaic::core::Layer& layer = doc.root().addOnTop(doc.makeVector("Shape"));
    auto* vl = layer.as<mosaic::core::VectorLayer>();
    REQUIRE(vl != nullptr);
    vec::Object o = rectObject();
    o.extrude = flatLitExtrude(); // lighting off: the face IS the albedo, so colour is readable
    vl->setObject(o);
    vl->setTransform(mosaic::common::Affine2D::translation(60.0, 60.0));

    auto centreOf = [&](const mosaic::common::Image& img) {
        const std::size_t p = (static_cast<std::size_t>(60) * img.width + 60) * 4;
        return std::array<std::uint8_t, 4>{img.rgba[p], img.rgba[p + 1], img.rgba[p + 2],
                                           img.rgba[p + 3]};
    };
    mosaic::render::CompositeOptions opts;
    const mosaic::render::CompositeResult before =
        mosaic::render::composite(doc, opts, mosaic::render::Backend::Cpu);
    REQUIRE(before.ok);
    const auto plain = centreOf(before.image);
    REQUIRE(plain[3] > 200); // the solid covers the canvas centre

    // A green colour overlay at full opacity: the face must come out green.
    mosaic::core::LayerEffects fx;
    fx.colorOverlay.enabled = true;
    fx.colorOverlay.paint = vec::SolidPaint{ColorF{0.0f, 1.0f, 0.0f, 1.0f}};
    fx.colorOverlay.opacity = 1.0f;
    layer.setEffects(fx);

    const mosaic::render::CompositeResult after =
        mosaic::render::composite(doc, opts, mosaic::render::Backend::Cpu);
    REQUIRE(after.ok);
    const auto overlaid = centreOf(after.image);
    CHECK(overlaid[1] > overlaid[0]); // green now dominates the red the shape was filled with
    CHECK(overlaid[1] > plain[1]);
}

TEST_CASE("the mesh cache is reused for a camera change and dropped for a geometry change") {
    mosaic::core::Document doc(64, 64);
    mosaic::core::Layer& layer = doc.root().addOnTop(doc.makeVector("Shape"));
    auto* vl = layer.as<mosaic::core::VectorLayer>();
    REQUIRE(vl != nullptr);
    vec::Object o = rectObject();
    o.extrude = flatLitExtrude();
    vl->setObject(o);

    const std::uint64_t key = vec::shapeExtrudeMeshKey(o, *o.extrude);
    CHECK(vl->cachedExtrudeMesh(key) == nullptr); // nothing rendered yet
    vl->setCachedExtrudeMesh(vec::buildShapeExtrudeMesh(o, *o.extrude), key);
    REQUIRE(vl->cachedExtrudeMesh(key) != nullptr);

    vec::Object spun = o;
    spun.extrude->orientation = mosaic::common::Quat::fromAxisAngle({1, 0, 0}, 0.3);
    CHECK(vl->cachedExtrudeMesh(vec::shapeExtrudeMeshKey(spun, *spun.extrude)) != nullptr);

    vec::Object deeper = o;
    deeper.extrude->depth = 50.0f;
    CHECK(vl->cachedExtrudeMesh(vec::shapeExtrudeMeshKey(deeper, *deeper.extrude)) == nullptr);
}

// ---- What the 3D-shape lane COSTS -------------------------------------------------------------
//
// Pixel parity says the solid looks right; it says nothing about a solid that looks right and
// re-tessellates itself sixty times a second. render::workCounters() counts operations, never
// pixels, so these are the same numbers on every machine and every build type -- the
// tests/test_composite_budget.cpp discipline, applied to the lane that actually got slow.

TEST_CASE("repeat composites of an unchanged 3D shape build the mesh exactly once") {
    mosaic::core::Document doc(200, 160);
    mosaic::core::Layer& layer = doc.root().addOnTop(doc.makeVector("Shape"));
    auto* vl = layer.as<mosaic::core::VectorLayer>();
    REQUIRE(vl != nullptr);
    vec::Object o = rectObject();
    o.extrude = flatLitExtrude();
    vl->setObject(o);
    vl->setTransform(mosaic::common::Affine2D::translation(100.0, 80.0));

    auto& wc = mosaic::render::workCounters();
    mosaic::render::CompositeOptions opts;
    const auto composite = [&] {
        return mosaic::render::composite(doc, opts, mosaic::render::Backend::Cpu).ok;
    };

    wc.reset();
    REQUIRE(composite());
    CHECK(wc.shapeMeshBuilds.load() == 1); // the first one has to tessellate
    CHECK(wc.shapeMeshHits.load() == 0);
    CHECK(wc.shapeSolidRenders.load() == 1);
    CHECK(wc.shapeSolidTriangles.load() > 0);

    // ⚠ THE ONE THAT MATTERS. Compositing again -- a pan, another layer's brush stroke, a window
    // resize -- must reuse the mesh. Meshing per composite is what "horrendously slow" was.
    wc.reset();
    for (int i = 0; i < 5; ++i)
        REQUIRE(composite());
    CHECK(wc.shapeMeshBuilds.load() == 0);
    CHECK(wc.shapeMeshHits.load() == 5);
    CHECK(wc.shapeSolidRenders.load() == 5); // the rasterizer has no cache; only the mesh does
}

TEST_CASE("orbiting a 3D shape re-renders but never re-meshes; a geometry edit re-meshes") {
    mosaic::core::Document doc(200, 160);
    mosaic::core::Layer& layer = doc.root().addOnTop(doc.makeVector("Shape"));
    auto* vl = layer.as<mosaic::core::VectorLayer>();
    REQUIRE(vl != nullptr);
    vec::Object o = rectObject();
    o.extrude = flatLitExtrude();
    vl->setObject(o);
    vl->setTransform(mosaic::common::Affine2D::translation(100.0, 80.0));

    auto& wc = mosaic::render::workCounters();
    mosaic::render::CompositeOptions opts;
    REQUIRE(mosaic::render::composite(doc, opts, mosaic::render::Backend::Cpu).ok); // prime

    // An orbit drag: the orientation moves every frame, the SOLID does not (type-tool §10.5 --
    // rotate / light / recolour are render-side, and re-tessellating through one would be the
    // whole gesture's cost).
    wc.reset();
    for (int i = 0; i < 4; ++i) {
        vec::Object spun = *vl->object();
        spun.extrude->orientation = mosaic::common::Quat::fromAxisAngle({0, 1, 0}, 0.1 * (i + 1));
        spun.fill = vec::SolidPaint{ColorF{0.1f * i, 0.5f, 0.5f, 1.0f}}; // a live recolour
        spun.extrude->lights[0].intensity = 0.5f + 0.1f * i;
        vl->setObject(spun);
        REQUIRE(mosaic::render::composite(doc, opts, mosaic::render::Backend::Cpu).ok);
    }
    CHECK(wc.shapeMeshBuilds.load() == 0);
    CHECK(wc.shapeMeshHits.load() == 4);

    // The depth, though, IS the solid: changing it must re-mesh, exactly once.
    wc.reset();
    vec::Object deeper = *vl->object();
    deeper.extrude->depth = 44.0f;
    vl->setObject(deeper);
    REQUIRE(mosaic::render::composite(doc, opts, mosaic::render::Backend::Cpu).ok);
    REQUIRE(mosaic::render::composite(doc, opts, mosaic::render::Backend::Cpu).ok);
    CHECK(wc.shapeMeshBuilds.load() == 1);
    CHECK(wc.shapeMeshHits.load() == 1);
}

// The stroke defect, guarded by the numbers that exposed it. vec::strokeOutline emits one
// OVERLAPPING piece per segment and per join -- correct for a NonZero fill, catastrophic as solid
// input, because the mesher turns each piece into its own watertight box with its own caps and
// bevel rings. Measured on a 41-lobe rosette before the fix: 656 pieces -> 69,708 triangles, 88%
// of the whole solid, of little boxes z-fighting inside the ribbon.
TEST_CASE("a stroked 3D shape unions its outline into one ribbon instead of per-segment boxes") {
    vec::Object o = rectObject(/*filled=*/false, /*stroked=*/true);
    tx::Extrude ex = flatLitExtrude();
    ex.bevelFront = {tx::Bevel::Profile::Round, 1.0f, 4};
    ex.bevelBack = ex.bevelFront;

    const tx::ExtrudeMesh ribbon = vec::buildShapeExtrudeMesh(o, ex);
    REQUIRE(!ribbon.empty());

    // The same stroke handed to the mesher RAW -- what the code used to do. Rebuild the pieces the
    // way shapeSolids does, minus the union, and mesh them.
    const vec::Contours pieces = vec::strokeOutline(vec::flatten(o.geometry, vec::kMeshTolerancePx),
                                                    o.stroke, vec::kMeshTolerancePx);
    REQUIRE(pieces.size() > 1); // the stroker really does emit a piece per segment/join
    const tx::ExtrudeMesh raw = mosaic::core::text::buildExtrudeMesh({{pieces, 1}}, ex);

    CHECK(ribbon.triangleCount() < raw.triangleCount());
    // A rectangle's ribbon is 2 rings (outer + inner) against 8+ overlapping pieces, so the gap is
    // wide. Assert a floor rather than a ratio: the point is that it does not scale with the
    // SEGMENT COUNT any more, and a regression that reinstates per-piece solids fails this by a
    // long way rather than by a few percent.
    CHECK(ribbon.triangleCount() * 2 < raw.triangleCount());
    // ... and the ribbon is still one run's worth of solid, drawn with the stroke's material.
    for (const tx::ExtrudeMeshRange& r : ribbon.ranges)
        CHECK(r.runIndex == 1);
}

// The blank 3D popup (user 2026-08-28): shapeExtrudeDesignBounds folded points in as {x,y,0,0}
// rects through Rect::united, which treats an EMPTY rect as "nothing" -- and a zero-size rect IS
// empty. It therefore returned the empty rect it started with for every shape, and the popup's
// viewport, which bails on empty bounds, drew nothing at all.
TEST_CASE("shapeExtrudeDesignBounds measures the shape rather than returning nothing") {
    const vec::Object o = rectObject(); // 40 x 30, centred on the local origin
    const common::Rect b = vec::shapeExtrudeDesignBounds(o);
    REQUIRE_FALSE(b.empty());
    CHECK(b.x == doctest::Approx(-20.0));
    CHECK(b.y == doctest::Approx(-15.0));
    CHECK(b.w == doctest::Approx(40.0));
    CHECK(b.h == doctest::Approx(30.0));
    // It measures the SOLIDS, so a stroke widens it by half the stroke width on every side.
    const common::Rect sb = vec::shapeExtrudeDesignBounds(rectObject(true, true));
    CHECK(sb.w > b.w);
    CHECK(sb.h > b.h);
    // Nothing to extrude -> honestly nothing, which is what the viewport's early-out reads.
    CHECK(vec::shapeExtrudeDesignBounds(rectObject(false, false)).empty());
}

// What an ORDINARY shape costs, pinned. The mesher is shared with 3D text, where the input is
// hundreds of glyph contours -- so it is easy for a change tuned there to make a rectangle
// expensive without anyone noticing, and easy (this happened) to quote a worst case as if it were
// the normal one. A square is four points and twelve triangles; that is the number to defend.
//
// Bevel segments multiply the wall bands, so a bevelled square is a small multiple of a flat one
// -- not a different order of magnitude. The ceilings are generous: they are here to catch a
// regression that reinstates per-piece solids or forgets the coarse mesh tolerance, not to freeze
// the mesher's exact output.
TEST_CASE("an ordinary shape meshes to a handful of triangles") {
    tx::Extrude plain; // depth only -- the default Extrude, no bevel
    plain.depth = 20.0f;
    tx::Extrude bevelled = plain;
    bevelled.bevelFront = {tx::Bevel::Profile::Round, 3.0f, 3};
    bevelled.bevelBack = bevelled.bevelFront;

    const auto tris = [](const vec::Object& o, const tx::Extrude& e) {
        return vec::buildShapeExtrudeMesh(o, e).triangleCount();
    };
    const auto solidRect = [](double radius, bool stroked) {
        vec::Object o;
        o.geometry = vec::ParametricShape{vec::RectShape::uniform({200, 200}, radius)};
        o.fill = vec::SolidPaint{ColorF{1, 0, 0, 1}};
        if (stroked) {
            o.stroke.enabled = true;
            o.stroke.paint = vec::SolidPaint{ColorF{0, 0, 1, 1}};
            o.stroke.width = 4.0;
        }
        return o;
    };

    // A square: two cap triangles each end, two per side wall. Twelve, exactly.
    CHECK(tris(solidRect(0.0, false), plain) == 12);
    CHECK(tris(solidRect(0.0, false), bevelled) <= 100);
    // A stroked square adds the ribbon: still tens, NOT one solid per stroke segment (which is
    // what strokeOutline hands over before shapeSolids unions it).
    CHECK(tris(solidRect(0.0, true), plain) <= 100);
    CHECK(tris(solidRect(0.0, true), bevelled) <= 400);
    // Curves cost points, and points cost wall bands -- but a rounded rect is still hundreds.
    CHECK(tris(solidRect(20.0, false), plain) <= 100);
    CHECK(tris(solidRect(20.0, false), bevelled) <= 500);

    vec::Object ellipse;
    ellipse.geometry = vec::ParametricShape{vec::EllipseShape{{100, 100}}};
    ellipse.fill = vec::SolidPaint{ColorF{1, 0, 0, 1}};
    CHECK(tris(ellipse, plain) <= 150);
    CHECK(tris(ellipse, bevelled) <= 600);
}
