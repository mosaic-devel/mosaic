// render::DragCompositeCache -- the Move-drag fast path (S15-b). It freezes the stack BELOW and
// ABOVE the dragged layer for the whole gesture and re-produces only the moved layer per frame,
// instead of walking the document again. Its entire contract is that the replay is
// **byte-identical** to the full composite for the same options: a drag preview that drifts from
// what lands on release is worse than a slow one.
//
// That contract had no test, which is how a whole layer KIND stayed excluded from the fast path
// without anyone noticing -- a vector layer fell back to a full canvas walk on every frame of
// every drag ("even dragging the 2d shape around is very slow", user 2026-08-28). These cover the
// parity for each kind the cache claims to serve, and that it claims to serve them at all.
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/vector/object.hpp"
#include "render/compositor.hpp"

#include <doctest/doctest.h>
#include <string>

namespace vec = mosaic::core::vec;
namespace tx = mosaic::core::text;
using mosaic::common::Affine2D;
using mosaic::common::ColorF;

namespace {

// The options a live drag frame runs under. liveDrag matters: the replay hard-sets it, so a full
// composite compared against it must ask for the same thing or the two legitimately differ.
mosaic::render::CompositeOptions dragOptions() {
    mosaic::render::CompositeOptions o;
    o.liveDrag = true;
    return o;
}

void paint(mosaic::common::Image& img, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        img.rgba[i] = r;
        img.rgba[i + 1] = g;
        img.rgba[i + 2] = b;
        img.rgba[i + 3] = 255;
    }
}

vec::Object squareObject(bool extruded) {
    vec::Object o;
    o.geometry = vec::ParametricShape{vec::RectShape::uniform({60, 40}, 0.0)};
    o.fill = vec::SolidPaint{ColorF{0.9f, 0.2f, 0.2f, 1.0f}};
    if (extruded) {
        tx::Extrude e;
        e.depth = 12.0f;
        e.lightingEnabled = false;
        e.orientation = mosaic::common::Quat::fromAxisAngle({0, 1, 0}, 0.5);
        o.extrude = e;
    }
    return o;
}

// Drag `target` through a few positions; at each one the cache's replay must equal a full walk.
void checkDragParity(mosaic::core::Document& doc, mosaic::core::LayerId target,
                     mosaic::core::Layer& moved, const std::string& what) {
    mosaic::render::DragCompositeCache cache;
    const mosaic::render::CompositeOptions opts = dragOptions();
    for (int step = 0; step < 4; ++step) {
        moved.setTransform(Affine2D::translation(70.0 + step * 9.0, 55.0 - step * 5.0));
        const std::optional<mosaic::common::Image> replay = cache.composite(doc, target, opts);
        INFO(what << " step " << step);
        REQUIRE_MESSAGE(replay.has_value(), "the drag cache declined to serve " << what);
        const mosaic::render::CompositeResult full =
            mosaic::render::composite(doc, opts, mosaic::render::Backend::Cpu);
        REQUIRE(full.ok);
        REQUIRE(replay->width == full.image.width);
        REQUIRE(replay->height == full.image.height);
        CHECK(replay->rgba == full.image.rgba); // byte-identical, not merely close
    }
}

// A background, the layer under test, and one layer above it -- so the replay exercises both
// cached halves rather than just the trivial top-of-stack case.
struct Scene {
    std::unique_ptr<mosaic::core::Document> doc;
    mosaic::core::Layer* target = nullptr;
};

Scene makeScene() {
    Scene s;
    s.doc = std::make_unique<mosaic::core::Document>(160, 120);
    auto bg = s.doc->makeRaster("Background", 160, 120);
    paint(bg->image(), 240, 240, 240);
    s.doc->root().addOnTop(std::move(bg));
    return s;
}

void addLayerAbove(mosaic::core::Document& doc) {
    auto top = doc.makeRaster("Above", 40, 40);
    paint(top->image(), 20, 90, 200);
    top->setOpacity(0.6f);
    doc.root().addOnTop(std::move(top)).setTransform(Affine2D::translation(30.0, 20.0));
}

} // namespace

TEST_CASE("the drag replay is byte-identical to a full composite, per layer kind") {
    SUBCASE("raster") {
        Scene s = makeScene();
        auto layer = s.doc->makeRaster("Dragged", 50, 50);
        paint(layer->image(), 200, 60, 60);
        mosaic::core::Layer& moved = s.doc->root().addOnTop(std::move(layer));
        addLayerAbove(*s.doc);
        checkDragParity(*s.doc, moved.id(), moved, "raster");
    }

    // ⚠ THE ONE THAT WAS MISSING. A vector layer has no cached pixels, so the replay re-runs its
    // render arm rather than re-placing a bitmap -- which is exactly why it must be checked
    // against the full walk rather than assumed.
    SUBCASE("vector (flat)") {
        Scene s = makeScene();
        auto layer = s.doc->makeVector("Shape");
        layer->setObject(squareObject(/*extruded=*/false));
        mosaic::core::Layer& moved = s.doc->root().addOnTop(std::move(layer));
        addLayerAbove(*s.doc);
        checkDragParity(*s.doc, moved.id(), moved, "flat vector");
    }

    SUBCASE("vector (3D solid)") {
        Scene s = makeScene();
        auto layer = s.doc->makeVector("Solid");
        layer->setObject(squareObject(/*extruded=*/true));
        mosaic::core::Layer& moved = s.doc->root().addOnTop(std::move(layer));
        addLayerAbove(*s.doc);
        checkDragParity(*s.doc, moved.id(), moved, "3D vector");
    }
}

// The fast path has to actually BE the fast path: serving means the stack below and above the
// dragged layer is composited ONCE for the gesture, not once per frame.
//
// Proved directly rather than through a proxy counter: put a SECOND 3D shape below the dragged one.
// `shapeSolidRenders` fires inside the vector render arm, so if the cache were re-walking the stack
// it would count two per frame. One per frame means the frozen half really is frozen.
TEST_CASE("a served drag re-produces the moved layer and nothing below it") {
    Scene s = makeScene();
    auto under = s.doc->makeVector("Solid below");
    under->setObject(squareObject(/*extruded=*/true));
    s.doc->root().addOnTop(std::move(under)).setTransform(Affine2D::translation(40.0, 40.0));

    auto layer = s.doc->makeVector("Dragged solid");
    layer->setObject(squareObject(/*extruded=*/true));
    mosaic::core::Layer& moved = s.doc->root().addOnTop(std::move(layer));
    addLayerAbove(*s.doc);

    mosaic::render::DragCompositeCache cache;
    const mosaic::render::CompositeOptions opts = dragOptions();
    auto& wc = mosaic::render::workCounters();

    // The build frame produces BOTH solids: the one below goes into the frozen backdrop.
    moved.setTransform(Affine2D::translation(70.0, 55.0));
    wc.reset();
    REQUIRE(cache.composite(*s.doc, moved.id(), opts).has_value());
    CHECK(wc.shapeSolidRenders.load() == 2);

    // Every frame after it produces ONE -- the layer under the pointer.
    wc.reset();
    for (int step = 1; step < 5; ++step) {
        moved.setTransform(Affine2D::translation(70.0 + step * 7.0, 55.0));
        REQUIRE(cache.composite(*s.doc, moved.id(), opts).has_value());
    }
    CHECK(wc.shapeSolidRenders.load() == 4); // one per frame, not two
    CHECK(wc.shapeMeshBuilds.load() == 0);   // reusing the mesh: only the placement moved
    CHECK(wc.composites.load() == 0);        // and no full composite() was entered at all
}

// The flat-vector twin: the cache must SERVE it. Before the vector arm it declined outright, and
// the caller's silent fallback -- a full canvas walk per drag frame -- is what made dragging a
// plain shape cost the same as dragging nothing at all.
TEST_CASE("the drag cache serves a flat shape layer at all") {
    Scene s = makeScene();
    auto layer = s.doc->makeVector("Shape");
    layer->setObject(squareObject(/*extruded=*/false));
    mosaic::core::Layer& moved = s.doc->root().addOnTop(std::move(layer));
    addLayerAbove(*s.doc);

    mosaic::render::DragCompositeCache cache;
    const mosaic::render::CompositeOptions opts = dragOptions();
    for (int step = 0; step < 4; ++step) {
        moved.setTransform(Affine2D::translation(70.0 + step * 7.0, 55.0));
        CHECK(cache.composite(*s.doc, moved.id(), opts).has_value());
    }
}
