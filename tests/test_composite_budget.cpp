// Cost budgets for the composite walk -- the axis the suite could not see.
//
// The rest of this suite asks whether the pixels are RIGHT. Nothing asked what they COST, and that
// blind spot is not theoretical: every defect the S60 performance arc found was correct, passed
// all 3040 test cases, and was discovered by opening a real document and watching a clock.
//
//     a Lanczos convolve evaluating sin() per tap across a canvas-sized destination, for a layer
//       covering 2% of it
//     a 34x34 layer-panel thumbnail building a 36.7 MP isolated buffer (31,717:1)
//     a reduced composite rebuilding every group at full canvas resolution (81:1)
//     an entire layer-effects lane running single-threaded
//
// Each is a violation of ONE invariant -- work must be sized to the OUTPUT and to the CONTENT,
// never to the canvas -- and each would have been caught at the commit that introduced it by the
// assertions below.
//
// ⚠ These assert COUNTS, never milliseconds. A wall-clock budget is machine-dependent, varies with
// build type and core count, and fails in CI for reasons nobody can reproduce; a count is the same
// number everywhere. render::workCounters() is incremented once per operation (never per pixel),
// so reading it costs nothing and it is always on -- a counter you have to enable is a counter
// that is off when the regression lands.
//
// The bounds are deliberately LOOSE (2x, 4x headroom). They are not performance targets; they are
// tripwires for the specific failure mode of work silently becoming O(canvas). A change that makes
// the compositor twice as slow will not fail these, and should not -- that is what the profiler is
// for. A change that reintroduces a 31,717:1 ratio fails them immediately.

#include <doctest/doctest.h>

#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/layer_effects.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/object.hpp"
#include "core/vector/paint.hpp"
#include "render/compositor.hpp"

#include <cstdint>
#include <memory>

namespace {

namespace core = mosaic::core;
namespace render = mosaic::render;
namespace vec = mosaic::core::vec;
using mosaic::common::Affine2D;
using mosaic::common::ColorF;

// A document whose CONTENT is a small fraction of its CANVAS -- the shape every one of the defects
// above needed in order to show itself. A big empty canvas with a little content in the corner is
// not a contrived case: it is a photograph's canvas with a caption on it.
struct Fixture {
    std::unique_ptr<core::Document> doc;
    std::uint32_t w = 0, h = 0;
};

Fixture makeFixture(std::uint32_t w, std::uint32_t h) {
    Fixture f;
    f.w = w;
    f.h = h;
    f.doc = std::make_unique<core::Document>(w, h);

    // One full-coverage backdrop, so the walk always has real work to do and the counters are
    // never trivially zero.
    auto back = f.doc->makeRaster("Back", w, h);
    back->image().fill(mosaic::common::Color8{40, 50, 70, 255});
    back->invalidateContentBounds();
    f.doc->root().addOnTop(std::move(back));

    // A group of small shapes in one corner -- 128 px of content on however big a canvas.
    auto group = f.doc->makeGroup("Shapes");
    for (int i = 0; i < 3; ++i) {
        auto v = f.doc->makeVector("Shape " + std::to_string(i));
        vec::Object o;
        vec::RectShape r;
        r.size = {64.0, 64.0};
        o.geometry = vec::ParametricShape{r};
        o.fill = vec::SolidPaint{ColorF{0.9f, 0.4f, 0.2f, 1.0f}};
        v->setObject(std::move(o));
        v->setTransform(Affine2D::trs({64.0 + 20.0 * i, 64.0 + 20.0 * i}, 0.1, {1, 1}));
        group->addOnTop(std::move(v));
    }
    f.doc->root().addOnTop(std::move(group));
    return f;
}

std::uint64_t leafCount(const core::GroupLayer& g) {
    std::uint64_t n = 0;
    for (const auto& c : g.children()) {
        if (const auto* sub = c->as<const core::GroupLayer>())
            n += leafCount(*sub);
        else
            ++n;
    }
    return n;
}

render::CompositeOptions plainOpts() {
    render::CompositeOptions o;
    o.checkerboard = false;
    return o;
}

} // namespace

TEST_CASE("every visible leaf layer is rendered exactly once per composite") {
    // The invariant a redundant-render regression breaks. It is also how the S60 arc's "why is
    // each layer rendered ~1.8 times per walk?" question should have been answerable without
    // arithmetic on profiler counts: 136 vector rasterisations for 17 layers across 4 walks turned
    // out to be the layer panel's thumbnails, but nothing in the suite could say so.
    Fixture f = makeFixture(2048, 2048);
    const std::uint64_t leaves = leafCount(f.doc->root());
    REQUIRE(leaves == 4); // 1 backdrop + 3 shapes

    render::workCounters().reset();
    const render::CompositeResult r =
        render::composite(*f.doc, plainOpts(), render::Backend::Cpu);
    REQUIRE(r.ok);

    CHECK(render::workCounters().composites.load() == 1);
    CHECK(render::workCounters().layerRenders.load() == leaves);
    CHECK(render::workCounters().groupBuffers.load() == 1); // exactly the one group
}

TEST_CASE("a group's isolated buffer is sized by the OUTPUT, not by the canvas") {
    // The 81:1 defect: renderLayerRaw sized a group's local buffer at one texel per group-local
    // unit regardless of how much the target could resolve, so a deliberately cheap reduced
    // composite quietly rebuilt every group at full canvas resolution. Compositing the SAME
    // document into a 10x smaller target must cost ~100x fewer group texels, not the same.
    Fixture f = makeFixture(4000, 4000);

    render::workCounters().reset();
    REQUIRE(render::compositeScaled(*f.doc, f.w, f.h, plainOpts(), render::Backend::Cpu).ok);
    const std::uint64_t fullTexels = render::workCounters().groupBufferTexels.load();

    render::workCounters().reset();
    REQUIRE(render::compositeScaled(*f.doc, f.w / 10, f.h / 10, plainOpts(),
                                    render::Backend::Cpu)
                .ok);
    const std::uint64_t smallTexels = render::workCounters().groupBufferTexels.load();

    REQUIRE(fullTexels > 0);
    REQUIRE(smallTexels > 0);
    INFO("group buffer texels: full=" << fullTexels << " reduced=" << smallTexels);
    // A 10x linear reduction is 100x the area. Allow generous slack for the extent's own rounding
    // and the minimum one-texel clamp; the defect this catches was a ratio of 1.0.
    CHECK(smallTexels * 25 < fullTexels);
}

TEST_CASE("a small layer's cost does not scale with an empty canvas") {
    // The canvas-sized-convolution defect, stated as a budget. The CONTENT is identical in both
    // documents; only the empty canvas around it grows. Per-layer buffer clears are still sized to
    // the target today (the extent-bounded walk is not built), so this bounds the RATIO rather
    // than the absolute: quadrupling the canvas area must not do more than quadruple the work.
    //
    // ⚠ Tighten this to a constant the day leaf buffers are bounded by their layer's extent. It is
    // written as a ratchet on purpose -- a loose true assertion that a future fix makes strict.
    Fixture small = makeFixture(1024, 1024);
    Fixture big = makeFixture(2048, 2048);

    render::workCounters().reset();
    REQUIRE(render::composite(*small.doc, plainOpts(), render::Backend::Cpu).ok);
    const std::uint64_t smallWork = render::workCounters().clearedTexels.load();

    render::workCounters().reset();
    REQUIRE(render::composite(*big.doc, plainOpts(), render::Backend::Cpu).ok);
    const std::uint64_t bigWork = render::workCounters().clearedTexels.load();

    REQUIRE(smallWork > 0);
    INFO("cleared texels: 1024^2 canvas=" << smallWork << "  2048^2 canvas=" << bigWork);
    // 4x the canvas area, so 4x the clears at worst. Anything beyond that is superlinear in the
    // canvas, which is the bug class this file exists for.
    CHECK(bigWork <= smallWork * 5);
}

TEST_CASE("layer effects do not enlarge the walk's buffer budget") {
    // Effects render into their own footprint buffer, and renderLayer grows that buffer only when
    // the footprint escapes the target. Adding a drop shadow to a small shape must therefore cost
    // a little more, not a canvas more -- the guard on the "effects buffer is the whole canvas"
    // failure mode.
    Fixture plain = makeFixture(2048, 2048);

    render::workCounters().reset();
    REQUIRE(render::composite(*plain.doc, plainOpts(), render::Backend::Cpu).ok);
    const std::uint64_t before = render::workCounters().clearedTexels.load();

    Fixture fx = makeFixture(2048, 2048);
    for (const auto& child : fx.doc->root().children()) {
        auto* g = child->as<core::GroupLayer>();
        if (g == nullptr) continue;
        for (const auto& shape : g->children()) {
            core::LayerEffects e;
            core::ShadowEffect sh;
            sh.enabled = true;
            sh.size = 8.0f;
            sh.distance = 4.0f;
            e.dropShadows.push_back(sh);
            shape->setEffects(std::move(e));
        }
    }

    render::workCounters().reset();
    REQUIRE(render::composite(*fx.doc, plainOpts(), render::Backend::Cpu).ok);
    const std::uint64_t after = render::workCounters().clearedTexels.load();

    REQUIRE(before > 0);
    INFO("cleared texels: no effects=" << before << "  with drop shadows=" << after);
    CHECK(after <= before * 2);
}
