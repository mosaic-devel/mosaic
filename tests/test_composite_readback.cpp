// The EXPLICIT READBACK SEAM (S60-a item 12; docs/s60-readback-consumers.md §6, §7).
//
// The audit that produced that document found nineteen consumers of the composited image where the
// plan expected five, and its central warning is that **a missed one silently forces a full
// readback per frame and eats the entire win**. Three of the six most dangerous were invisible to
// every grep it ran, because a `std::function` provider closing over the composite names nothing.
//
// So this file tests two different things, and the second is the important one:
//
//   1. THE MECHANISM -- that `pinMirror` + `peek` really do serve the per-frame, per-event
//      consumers out of host memory, that the pixels they serve match the CPU reference, and that
//      an unpinned point MISSES rather than quietly fencing.
//   2. THE GUARD -- that the consumer vocabulary is a closed, checked-in set. §7(c)/(d) of the
//      audit argue, correctly, that no amount of discipline keeps an enumeration honest; only an
//      API seam that forces every consumer to name itself, plus a test that pins the list, does.
//      `knownConsumerNames()` is that list and the case below is that test.
//
// The mechanism cases need a device and follow the file-wide convention: no usable Vulkan device
// WARNs and passes. The guard cases are pure and run everywhere, which is deliberate -- the guard
// must not be the thing that stops working on a CI box without a GPU.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "render/composite_readback.hpp"
#include "render/compositor.hpp"
#include "render/tile_compositor.hpp"

using mosaic::common::Affine2D;
using mosaic::common::Image;
using mosaic::render::CompositeReadback;
using mosaic::render::Freshness;
using mosaic::render::MirrorPin;
using mosaic::render::ReadbackRequest;
using mosaic::render::ReadbackResult;
namespace common = mosaic::common;
namespace consumers = mosaic::render::consumers;
namespace core = mosaic::core;
namespace render = mosaic::render;

namespace {

// Wider than one macrotile on every plausible k, so "pin a rect, miss outside it" is a real
// property rather than an artefact of the whole document fitting one tile.
constexpr std::uint32_t kW = 900;
constexpr std::uint32_t kH = 700;

std::unique_ptr<render::TileCompositor> makeLane(const char* who) {
    std::string err;
    auto lane = render::TileCompositor::create(err);
    if (!lane) {
        const std::string note =
            std::string("no usable Vulkan device -- skipping ") + who + " (" + err + ")";
        WARN_MESSAGE(true, note);
    }
    return lane;
}

Image ramp(std::uint32_t w, std::uint32_t h) {
    Image img(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            img.rgba[p + 0] = static_cast<std::uint8_t>(x % 256);
            img.rgba[p + 1] = static_cast<std::uint8_t>(y % 256);
            img.rgba[p + 2] = static_cast<std::uint8_t>((x * 3 + y * 7) % 256);
            img.rgba[p + 3] = 255;
        }
    return img;
}

void seed(core::Document& doc) {
    auto bg = doc.makeRaster("bg", doc.width(), doc.height());
    bg->image() = ramp(doc.width(), doc.height());
    doc.root().addOnTop(std::move(bg));
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The guard: the consumer registry. Pure -- runs with no GPU, on purpose.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the consumer vocabulary is a closed, checked-in set") {
    // ⚠ IF THIS FAILS BECAUSE YOU ADDED A CONSUMER, THAT IS THE TEST WORKING. Add the name to
    // render/composite_readback.hpp's `consumers` namespace and to the literal below, and while
    // you are there write down its cadence, its extent and its freshness in
    // docs/s60-readback-consumers.md §3. A consumer nobody characterised is how this became a
    // per-frame full-canvas cost in two places.
    const std::vector<std::string> expected = {
        "3D text reflect environment",     "adjustment panel fade",
        "channels histogram",              "copy merged",
        "crop-expansion inpaint seed",     "edge select brush (all layers)",
        "eyedropper sample + loupe",       "magic wand (all layers)",
        "smart recompose seed",            "smart resize importance map",
        "status-bar cursor colour",
    };
    CHECK(render::knownConsumerNames() == expected);

    // The five that must NEVER appear: they keep `render::composite` because they need
    // byte-determinism (export, save, the PRVW thumbnail) or off-canvas pixels (the two modal
    // preview panes). Naming them here is what makes their absence a decision rather than a gap.
    for (const char* forbidden : {"quick export png", "quick export jpeg", "mosaic PRVW preview",
                                  "fill dialog preview", "layer effects preview"}) {
        const auto& names = render::knownConsumerNames();
        CHECK(std::find(names.begin(), names.end(), std::string(forbidden)) == names.end());
    }
}

TEST_CASE("the API's defaults are the safe ones, not the convenient ones") {
    // Today `m_lastComposite.rgba[p]` gives a caller a synchronous, always-current read for free.
    // The whole point of the seam is that the free thing is now the CHEAP thing: a consumer that
    // needs synchrony has to say so, in writing, at the call site.
    const ReadbackRequest def;
    CHECK(def.freshness == Freshness::AnyRecent);
    CHECK_FALSE(def.blocking);
    CHECK(def.roi.empty());

    CHECK(render::freshnessName(Freshness::Current) == "current");
    CHECK(render::freshnessName(Freshness::Settled) == "settled");
    CHECK(render::freshnessName(Freshness::AnyRecent) == "any-recent");

    MirrorPin unheld;
    CHECK_FALSE(unheld.held());
    unheld.release();  // idempotent
    CHECK_FALSE(unheld.held());
}

// ---------------------------------------------------------------------------------------------
// The mechanism.
// ---------------------------------------------------------------------------------------------

TEST_CASE("a pinned mirror serves the per-event consumers without a transfer") {
    auto lane = makeLane("the pinned mirror");
    if (!lane) return;

    core::Document doc(kW, kH);
    seed(doc);
    const render::CompositeOptions opts;
    const render::CompositeResult cpu = render::composite(doc, opts, render::Backend::Cpu);
    REQUIRE(cpu.ok);

    lane->reset();
    const render::TileCompositeStatus st = lane->composite(doc, opts);
    INFO("refusal: " << std::string(render::tileRefusalName(st.refusal)) << " " << st.error);
    REQUIRE(st.ok);

    CompositeReadback rb(*lane);
    CHECK(rb.mirroredTiles() == 0);
    // Nothing pinned: a peek MISSES. It must not reach for the device -- a readout that blinks off
    // for one frame beats a fence on the pointer path, and this assertion is what keeps a
    // well-meaning "just fetch it" branch out of peek().
    CHECK_FALSE(rb.peek(common::Vec2{100.0, 100.0}).has_value());

    {
        // The eyedropper pins the macrotile under the pointer for as long as its tool is up.
        const MirrorPin pin =
            rb.pinMirror(common::Rect{100.0, 100.0, 8.0, 8.0}, consumers::kEyedropper);
        CHECK(pin.held());
        CHECK(rb.mirroredTiles() >= 1);
        CHECK(rb.mirrorBytes() > 0);

        const auto px = rb.peek(common::Vec2{100.5, 100.5});
        REQUIRE(px.has_value());
        const std::size_t p = (static_cast<std::size_t>(100) * kW + 100) * 4;
        // One LSB, the project-wide GPU tolerance: the accumulator is fp16 and the 8-bit store
        // rounds on the device.
        CHECK(std::abs(static_cast<int>(px->r) - static_cast<int>(cpu.image.rgba[p + 0])) <= 1);
        CHECK(std::abs(static_cast<int>(px->g) - static_cast<int>(cpu.image.rgba[p + 1])) <= 1);
        CHECK(std::abs(static_cast<int>(px->b) - static_cast<int>(cpu.image.rgba[p + 2])) <= 1);
        CHECK(px->a == cpu.image.rgba[p + 3]);

        // The eyedropper's real payload is an (2r+1)^2 window, not one pixel.
        const auto win = rb.peekRect(common::Rect{98.0, 98.0, 11.0, 11.0});
        REQUIRE(win.has_value());
        CHECK(win->width == 11);
        CHECK(win->height == 11);

        // A point in a macrotile nobody pinned is still a miss. This is the property that bounds
        // the mirror: pinning is not "mirror the document cheaply".
        CHECK_FALSE(rb.peek(common::Vec2{kW - 4.0, kH - 4.0}).has_value());
        CHECK_FALSE(rb.peekRect(common::Rect{kW - 20.0, kH - 20.0, 16.0, 16.0}).has_value());
    }
    // The pin is RAII: leaving the scope releases the mirror, and the memory with it.
    CHECK(rb.mirroredTiles() == 0);
    CHECK(rb.mirrorBytes() == 0);
    CHECK_FALSE(rb.peek(common::Vec2{100.5, 100.5}).has_value());
}

TEST_CASE("the mirror follows the composite, and refreshing it is bounded by the pin") {
    auto lane = makeLane("mirror refresh");
    if (!lane) return;

    core::Document doc(kW, kH);
    seed(doc);
    auto top = doc.makeRaster("top", 32, 32);
    Image solid(32, 32);
    solid.fill(common::Color8{255, 0, 0, 255});
    top->image() = solid;
    top->setTransform(Affine2D::translation(100.0, 100.0));
    const core::LayerId topId = top->id();
    doc.root().addOnTop(std::move(top));

    const render::CompositeOptions opts;
    lane->reset();
    REQUIRE(lane->composite(doc, opts).ok);

    CompositeReadback rb(*lane);
    const MirrorPin pin =
        rb.pinMirror(common::Rect{100.0, 100.0, 4.0, 4.0}, consumers::kCursorReadout);
    REQUIRE(pin.held());
    const auto before = rb.peek(common::Vec2{110.0, 110.0});
    REQUIRE(before.has_value());
    CHECK(before->r == 255);
    CHECK(before->g == 0);

    // The mirror must not be a snapshot: an edit under a held pin has to become visible, or the
    // eyedropper reports the colour the pointer just left.
    core::Layer* l = doc.find(topId);
    REQUIRE(l != nullptr);
    Image blue(32, 32);
    blue.fill(common::Color8{0, 0, 255, 255});
    l->as<core::RasterLayer>()->image() = blue;
    l->as<core::RasterLayer>()->invalidateContentBounds();  // bumps contentRevision
    lane->markLayerDirty(topId);
    REQUIRE(lane->composite(doc, opts).ok);

    const std::uint64_t bytesBefore = rb.thisFrameStats().bytes;
    rb.refreshMirror();
    const auto after = rb.peek(common::Vec2{110.0, 110.0});
    REQUIRE(after.has_value());
    CHECK(after->b == 255);
    CHECK(after->r == 0);

    // A refresh transfers the PINNED TILES and nothing else -- one 256 px macrotile is 256 KiB of
    // fp16, which is the price that makes the whole pointer-following class free. If this ever
    // scales with the document, the mirror has become the CPU copy it replaced.
    const std::uint64_t moved = rb.thisFrameStats().bytes - bytesBefore;
    INFO("mirror refresh moved " << moved << " bytes for " << rb.mirroredTiles() << " tile(s)");
    CHECK(moved > 0);
    CHECK(moved < static_cast<std::uint64_t>(kW) * kH * 4);

    // Nothing changed since: a second refresh is a no-op, so holding a pin costs nothing while
    // the user is idle.
    const std::uint64_t settled = rb.thisFrameStats().bytes;
    rb.refreshMirror();
    CHECK(rb.thisFrameStats().bytes == settled);
}

TEST_CASE("a gesture frame moves ZERO bytes, and a stale mirror refuses rather than lies") {
    // The regression this pins made the resident lane SLOWER than the CPU walk it replaces, and it
    // hid behind a memo that looks sufficient: refreshMirror() short-circuits when the accumulator's
    // revision has not moved -- but during an edit the revision moves every frame, so the memo never
    // hits. One held cursor-readout pin (i.e. the pointer anywhere over the canvas) was therefore a
    // device->host transfer plus a fence on EVERY frame of EVERY brush stroke. The fence is the
    // cost, not the bytes: it serialises the frame against the device.
    auto lane = makeLane("the gesture guard");
    if (!lane) return;

    core::Document doc(kW, kH);
    seed(doc);
    auto top = doc.makeRaster("top", 32, 32);
    Image solid(32, 32);
    solid.fill(common::Color8{255, 0, 0, 255});
    top->image() = solid;
    top->setTransform(Affine2D::translation(100.0, 100.0));
    const core::LayerId topId = top->id();
    doc.root().addOnTop(std::move(top));

    const render::CompositeOptions opts;
    lane->reset();
    REQUIRE(lane->composite(doc, opts).ok);

    CompositeReadback rb(*lane);
    const MirrorPin pin =
        rb.pinMirror(common::Rect{100.0, 100.0, 4.0, 4.0}, consumers::kCursorReadout);
    REQUIRE(pin.held());
    REQUIRE(rb.peek(common::Vec2{110.0, 110.0}).has_value());  // current: it serves

    // The gesture starts, and the stroke repaints the layer -- the composite moves under the pin.
    rb.setGestureActive(true);
    core::Layer* l = doc.find(topId);
    REQUIRE(l != nullptr);
    Image blue(32, 32);
    blue.fill(common::Color8{0, 0, 255, 255});
    l->as<core::RasterLayer>()->image() = blue;
    l->as<core::RasterLayer>()->invalidateContentBounds();
    lane->markLayerDirty(topId);
    REQUIRE(lane->composite(doc, opts).ok);

    const std::uint64_t bytesBefore = rb.thisFrameStats().bytes;
    rb.refreshMirror();
    CHECK(rb.thisFrameStats().bytes == bytesBefore);  // ZERO. Not "less". Not "bounded".

    // ... and because it did not refresh, the mirror now holds the colour from BEFORE the stroke.
    // Serving that is the failure mode this guard trades for: a readout that blinks off is a
    // glitch, a readout reporting the colour the canvas had a moment ago is a bug report.
    CHECK_FALSE(rb.mirrorCurrent());
    CHECK_FALSE(rb.peek(common::Vec2{110.0, 110.0}).has_value());

    // The gesture ends -- but an edit landing THIS frame still blocks the transfer, because typing
    // is not a pointer gesture and would otherwise fence once per character. beginFrame() sees the
    // revision move and says so.
    rb.setGestureActive(false);
    rb.beginFrame();
    CHECK(rb.editingThisFrame());
    const std::uint64_t bytesQuiet = rb.thisFrameStats().bytes;
    rb.refreshMirror();
    CHECK(rb.thisFrameStats().bytes == bytesQuiet);
    CHECK_FALSE(rb.peek(common::Vec2{110.0, 110.0}).has_value());

    // One frame of quiet, and the readout comes back with the NEW colour.
    rb.beginFrame();
    CHECK_FALSE(rb.editingThisFrame());
    rb.refreshMirror();
    CHECK(rb.mirrorCurrent());
    const auto after = rb.peek(common::Vec2{110.0, 110.0});
    REQUIRE(after.has_value());
    CHECK(after->b == 255);
    CHECK(after->r == 0);
    CHECK(rb.thisFrameStats().bytes > 0);  // this frame's counter -- beginFrame() reset it

    // A pin TAKEN mid-gesture still seeds itself. It is an explicit consumer act, not the frame
    // path, and `peek` after `pinMirror` has to work or the handle means nothing.
    rb.setGestureActive(true);
    const MirrorPin second =
        rb.pinMirror(common::Rect{300.0, 300.0, 4.0, 4.0}, consumers::kEyedropper);
    REQUIRE(second.held());
    CHECK(rb.peek(common::Vec2{302.0, 302.0}).has_value());
}

TEST_CASE("the general path names itself, reports its revision, and is served from the mirror") {
    auto lane = makeLane("the request path");
    if (!lane) return;

    core::Document doc(kW, kH);
    seed(doc);
    const render::CompositeOptions opts;
    lane->reset();
    REQUIRE(lane->composite(doc, opts).ok);

    CompositeReadback rb(*lane);
    rb.beginFrame();

    // A discrete whole-canvas consumer: a click, not a frame. It is allowed to be a transfer.
    ReadbackRequest wand;
    wand.name = consumers::kMagicWand;
    wand.freshness = Freshness::Current;
    wand.blocking = true;
    ReadbackResult r = rb.request(wand).get();
    INFO(r.error);
    REQUIRE(r.ok);
    CHECK(r.image.width == kW);
    CHECK(r.image.height == kH);
    CHECK(r.revision == lane->revision());
    CHECK(rb.thisFrameStats().requests == 1);
    CHECK(rb.thisFrameStats().bytes >= r.image.rgba.size());
    CHECK(rb.thisFrameStats().fences == 1);  // blocking + Current: the thing to keep at zero

    // The same request, but covered by a pin, costs no transfer at all. This is the eyedropper's
    // commit path, which goes through request() while its per-frame path goes through peek().
    const MirrorPin pin =
        rb.pinMirror(common::Rect{200.0, 200.0, 4.0, 4.0}, consumers::kEyedropper);
    REQUIRE(pin.held());
    rb.beginFrame();
    ReadbackRequest drop;
    drop.name = consumers::kEyedropper;
    drop.roi = common::Rect{200.0, 200.0, 3.0, 3.0};
    drop.freshness = Freshness::Current;
    const ReadbackResult s = rb.request(drop).get();
    REQUIRE(s.ok);
    CHECK(s.image.width == 3);
    CHECK(rb.thisFrameStats().bytes == 0);  // served from host memory
    CHECK(rb.thisFrameStats().fences == 0);

    // Every consumer that has asked is on the record, and everything on the record is in the
    // checked-in vocabulary.
    for (const std::string& n : rb.consumers()) {
        const auto& known = render::knownConsumerNames();
        INFO("unregistered consumer name: " << n);
        CHECK(std::find(known.begin(), known.end(), n) != known.end());
    }
    CHECK(rb.consumers().size() == 2);
}

TEST_CASE("a screen-space request refuses by name when nothing can serve it") {
    auto lane = makeLane("the screen-space path");
    if (!lane) return;

    core::Document doc(kW, kH);
    seed(doc);
    lane->reset();
    REQUIRE(lane->composite(doc, render::CompositeOptions{}).ok);

    CompositeReadback rb(*lane);
    // The adjustment panel's fade wants SCREEN pixels. Expressed in document space its footprint
    // is an arbitrary rotated quad whose AABB can be most of the canvas at low zoom, which is why
    // it gets its own entry point -- and why, with no provider installed, it must FAIL rather than
    // fall back to the unbounded document-space gather.
    ReadbackResult r = rb.requestScreenRect(common::Rect{0.0, 0.0, 320.0, 200.0},
                                            consumers::kPanelFade)
                           .get();
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());

    bool called = false;
    rb.setScreenCapture([&](const common::Rect& rect, Image& out, std::string&) {
        called = true;
        out = Image(static_cast<std::uint32_t>(rect.w), static_cast<std::uint32_t>(rect.h));
        return true;
    });
    r = rb.requestScreenRect(common::Rect{0.0, 0.0, 320.0, 200.0}, consumers::kPanelFade).get();
    CHECK(called);
    CHECK(r.ok);
    CHECK(r.image.width == 320);
}
