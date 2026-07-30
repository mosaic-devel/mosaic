// THE APP SIDE OF THE RESIDENT COMPOSITE LANE (S60-a item 13's wiring; the flip is NOT made).
//
// Five things are tested here, and none needs a Vulkan device -- deliberately, because all five
// are the parts that decide whether the flip is *safe* rather than whether it is *fast*:
//
//   1. THE OPT-IN IS OFF BY DEFAULT AND STRICT. `MOSAIC_TILE_COMPOSITOR` unset means the whole lane
//      is never constructed and the app composites exactly as it does today. A near-miss value must
//      NOT switch the compositor the entire canvas is drawn through.
//   2. THE LAYER-LOCAL CLAIM IS LAYER-LOCAL. `Command::dirtyLayerPixels` reports the rect in the
//      space `RasterLayer::image()` is indexed in; `Command::dirtyRegion` reports the document AABB
//      the same edit projects onto. On a TRANSFORMED layer those two differ, and handing the second
//      one to `TileCompositor::markLayerDirty(layer, rect)` would upload the wrong texels and look
//      right until it did not. The case below is the one that would catch it.
//   3. A REFUSAL LOSES NOTHING. The lane refreshes the text/texture pixel caches before it can know
//      whether it will serve, and that refresh reports the band it re-rendered exactly ONCE. When
//      the lane then refuses -- which it does for every document holding a text layer -- the band
//      has to reach the CPU fallback running in the same frame, or the typed glyphs never reach the
//      canvas at all (user report, MOSAIC_TILE_COMPOSITOR=1, 2026-07-29). `PendingRegion` is where
//      that hand-back lives, and the cases at the bottom are the ones that would catch it coming
//      back.
//   4. A LIVE MOVE DRAG IS NOT A REASON TO LEAVE THE LANE. It was one, by name, and "moving text
//      layers around is still CPU lane" (user report, 2026-07-29) was the whole of it. What is left
//      is one physical exception -- the renderer's own drag pass writes the same VkImage the resolve
//      writes -- and `residentGateSkip` is where the ORDER of those reasons is pinned, because the
//      order is what the once-per-reason log line ends up naming.
//   5. THE PER-GESTURE LATCH ONLY LATCHES WHAT WILL STILL BE TRUE NEXT FRAME. Pinning a stroke to
//      one lane is worth it for a refusal that describes the DOCUMENT; doing it for one that
//      describes a single instant re-creates the stranded drag one level down.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/command.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "ui/resident_composite.hpp"

using namespace mosaic;
using core::Document;
using core::LayerId;
using core::LayerPixelEdit;
using core::SetLayerPixelsCommand;
using core::SetTransformCommand;
using core::SetTransformsCommand;
using render::ResampleFilter;
using ui::GestureLatch;
using ui::ResidentGate;
using ui::ResidentSkip;

namespace {

// setenv/unsetenv around one probe, restoring whatever the environment had.
struct ScopedEnv {
    explicit ScopedEnv(const char* value) {
        if (const char* prev = std::getenv("MOSAIC_TILE_COMPOSITOR")) {
            m_had = true;
            m_prev = prev;
        }
        if (value == nullptr)
            ::unsetenv("MOSAIC_TILE_COMPOSITOR");
        else
            ::setenv("MOSAIC_TILE_COMPOSITOR", value, 1);
    }
    ~ScopedEnv() {
        if (m_had)
            ::setenv("MOSAIC_TILE_COMPOSITOR", m_prev.c_str(), 1);
        else
            ::unsetenv("MOSAIC_TILE_COMPOSITOR");
    }
    bool m_had = false;
    std::string m_prev;
};

}  // namespace

TEST_CASE("resident compositor opt-in defaults OFF and is strict") {
    {
        const ScopedEnv unset(nullptr);
        CHECK_FALSE(ui::residentCompositorRequested());
    }
    {
        const ScopedEnv empty("");
        CHECK_FALSE(ui::residentCompositorRequested());
    }
    for (const char* on : {"1", "true", "TRUE", "yes", "On"}) {
        const ScopedEnv e(on);
        CHECK(ui::residentCompositorRequested());
    }
    // Anything that is not an explicit yes leaves the app on the CPU walk. A near-miss must not
    // half-enable a compositor.
    for (const char* off : {"0", "false", "no", "off", "2", "1 ", "enabled", "please"}) {
        const ScopedEnv e(off);
        CHECK_FALSE(ui::residentCompositorRequested());
    }
}

TEST_CASE("dirtyLayerPixels reports the LAYER-LOCAL rect, not the document AABB") {
    // A layer rotated 45 degrees: the document AABB of a 40x10 edit is much wider than the edit,
    // and is not axis-aligned in layer space at all. Feeding it to the resident compositor's
    // incremental upload would copy the wrong texels.
    Document doc(512, 512);
    auto* raster = doc.root().addOnTop(doc.makeRaster("L", 256, 256)).as<core::RasterLayer>();
    REQUIRE(raster != nullptr);
    raster->setTransform(common::Affine2D::rotation(0.785398163397448)); // pi/4
    const LayerId id = raster->id();

    common::Image patch(40, 10);
    patch.fill({255, 0, 0, 255});
    const SetLayerPixelsCommand cmd(id, patch, 30, 70);

    const std::optional<common::Rect> docRect = cmd.dirtyRegion(doc);
    REQUIRE(docRect.has_value());
    const std::optional<LayerPixelEdit> claim = cmd.dirtyLayerPixels(doc);
    REQUIRE(claim.has_value());

    CHECK(claim->layer == id);
    CHECK(claim->rect.x == doctest::Approx(30.0));
    CHECK(claim->rect.y == doctest::Approx(70.0));
    CHECK(claim->rect.w == doctest::Approx(40.0));
    CHECK(claim->rect.h == doctest::Approx(10.0));
    // ... and it is emphatically NOT the document rect. A 45-degree rotation turns the 40x10
    // sliver into a ~35x35 square in document space: the height grows 3.5x and the width actually
    // SHRINKS. That asymmetry is the whole point -- a document AABB fed to a layer-local API does
    // not merely over-copy, it copies the WRONG texels, and no bound on its area would catch that.
    // If these ever coincide the case has stopped testing anything and the transform needs
    // restoring.
    CHECK(docRect->h > claim->rect.h + 1.0);
    CHECK(docRect->w < claim->rect.w - 1.0);
}

TEST_CASE("dirtyLayerPixels: the whole-layer form claims no rect") {
    Document doc(64, 64);
    auto* raster = doc.root().addOnTop(doc.makeRaster("L", 64, 64)).as<core::RasterLayer>();
    REQUIRE(raster != nullptr);
    const common::Image whole(80, 80); // a replace may RESIZE, so no tile index survives it
    const SetLayerPixelsCommand cmd(raster->id(), whole);
    const std::optional<LayerPixelEdit> claim = cmd.dirtyLayerPixels(doc);
    REQUIRE(claim.has_value());
    CHECK(claim->layer == raster->id());
    CHECK(claim->rect.empty()); // empty == "the whole layer", the always-correct claim
}

TEST_CASE("dirtyLayerPixels: a vanished layer makes no claim") {
    Document doc(64, 64);
    doc.root().addOnTop(doc.makeRaster("L", 64, 64));
    const common::Image patch(4, 4);
    const SetLayerPixelsCommand gone(999999, patch, 0, 0);
    CHECK_FALSE(gone.dirtyLayerPixels(doc).has_value());
}

TEST_CASE("structural commands make no layer-pixel claim") {
    Document doc(64, 64);
    auto* raster = doc.root().addOnTop(doc.makeRaster("L", 64, 64)).as<core::RasterLayer>();
    REQUIRE(raster != nullptr);
    // A transform is exactly the case the compositor's own plan diff already sees; claiming pixels
    // for it would force an upload that never needed to happen.
    const SetTransformCommand xform(raster->id(), common::Affine2D::translation(5.0, 5.0), 0);
    CHECK_FALSE(xform.dirtyLayerPixels(doc).has_value());
}

TEST_CASE("CommandStack carries the layer-local claims through undo and redo") {
    Document doc(64, 64);
    auto* raster = doc.root().addOnTop(doc.makeRaster("L", 64, 64)).as<core::RasterLayer>();
    REQUIRE(raster != nullptr);
    const LayerId id = raster->id();
    common::Image patch(8, 8);
    patch.fill({1, 2, 3, 255});
    doc.commands().push(std::make_unique<SetLayerPixelsCommand>(id, patch, 16, 24));

    doc.commands().undo();
    REQUIRE(doc.commands().lastAffectedLayerEdits().size() == 1);
    CHECK(doc.commands().lastAffectedLayerEdits()[0].layer == id);
    CHECK(doc.commands().lastAffectedLayerEdits()[0].rect.x == doctest::Approx(16.0));
    CHECK(doc.commands().lastAffectedLayerEdits()[0].rect.h == doctest::Approx(8.0));

    doc.commands().redo();
    REQUIRE(doc.commands().lastAffectedLayerEdits().size() == 1);
    CHECK(doc.commands().lastAffectedLayerEdits()[0].rect.y == doctest::Approx(24.0));
}

TEST_CASE("CommandStack accumulates one claim per stepped command across a jump") {
    Document doc(64, 64);
    auto* raster = doc.root().addOnTop(doc.makeRaster("L", 64, 64)).as<core::RasterLayer>();
    REQUIRE(raster != nullptr);
    const LayerId id = raster->id();
    const common::Image a(8, 8), b(4, 4);
    doc.commands().push(std::make_unique<SetLayerPixelsCommand>(id, a, 0, 0));
    doc.commands().push(std::make_unique<SetLayerPixelsCommand>(id, b, 32, 32));
    REQUIRE(doc.commands().position() == 2);

    doc.commands().jumpTo(0);
    // Two claims, not their bounding box: two disjoint dabs cost two macrotiles on the resident
    // lane, and folding them into one rect would put everything between them back on the bus.
    CHECK(doc.commands().lastAffectedLayerEdits().size() == 2);
}

// ---- The eligibility gate: a live drag is the lane's best case, not its exception --------------

namespace {

// Every app-side fact in the state a served frame needs. Each case below spoils exactly one.
[[nodiscard]] ResidentGate eligibleGate() {
    ResidentGate g;
    g.haveLane = true;
    g.haveDocument = true;
    g.haveRenderer = true;
    return g;
}

}  // namespace

TEST_CASE("the resident gate serves a live Move drag") {
    // THE REGRESSION THIS FILE'S ITEM 4 IS ABOUT. There is no longer any "a drag is in flight" input
    // to this decision AT ALL -- the enum has no LiveDrag member to set -- which is the strongest
    // form the fix can take: the gate cannot skip for a reason it cannot be told. A Move gesture
    // changes a placement, the plan diff is built on placement fingerprints, and the layer's pixels
    // never move, so the gesture is the cheapest thing the lane ever does.
    CHECK(ui::residentGateSkip(eligibleGate()) == ResidentSkip::None);
}

TEST_CASE("the resident gate: the GPU drag pass is the one drag-shaped skip left") {
    // The exception that survived, and it is not about drags: WindowRenderer's drag pass dispatches
    // into the SAME canvas VkImage the resolve writes, from inside drawFrame. A frame it owns is a
    // frame the lane must not partially resolve into, or the macrotiles the drag did not dirty keep
    // showing whichever writer got there second.
    ResidentGate g = eligibleGate();
    g.gpuDragPass = true;
    CHECK(ui::residentGateSkip(g) == ResidentSkip::GpuDragPass);
    // ... and it is named, not silent. An unnamed lane switch is how a regression hides.
    CHECK(ui::residentSkipName(ResidentSkip::GpuDragPass) ==
          "the GPU drag pass owns the canvas texture");
}

TEST_CASE("the resident gate reports the FIRST reason, in a fixed order") {
    // The order is not cosmetic: it is what `logSkipOnce` prints, so it is what anyone reading a
    // MOSAIC_TILE_COMPOSITOR=1 log reasons from. A gate that reported "the GPU drag pass owns the
    // canvas texture" for a frame that had no renderer yet would send the next reader hunting the
    // wrong seam.
    ResidentGate none; // nothing set at all == the default build
    CHECK(ui::residentGateSkip(none) == ResidentSkip::NotRequested);

    ResidentGate g = eligibleGate();
    g.haveDocument = false;
    g.haveRenderer = false;
    g.recomposeReview = true;
    g.gpuDragPass = true;
    CHECK(ui::residentGateSkip(g) == ResidentSkip::NoDocument);
    g.haveDocument = true;
    CHECK(ui::residentGateSkip(g) == ResidentSkip::NoRenderer);
    g.haveRenderer = true;
    CHECK(ui::residentGateSkip(g) == ResidentSkip::RecomposeReview);
    g.recomposeReview = false;
    CHECK(ui::residentGateSkip(g) == ResidentSkip::GpuDragPass);
    g.gpuDragPass = false;
    CHECK(ui::residentGateSkip(g) == ResidentSkip::None);

    // The opt-in outranks every one of them: with no lane there is nothing to skip FROM, and the
    // default build must never log a reason that sounds like a problem.
    g = eligibleGate();
    g.haveLane = false;
    g.gpuDragPass = true;
    CHECK(ui::residentGateSkip(g) == ResidentSkip::NotRequested);
}

// ---- The per-gesture refusal latch -------------------------------------------------------------

TEST_CASE("GestureLatch: a structural refusal holds for the rest of ONE gesture") {
    GestureLatch l;
    l.beginFrame(true); // the press
    CHECK_FALSE(l.latched());
    l.refuse(true, ResidentSkip::Refused); // ... and this frame's serve() said no, structurally
    CHECK(l.latched());
    l.beginFrame(true); // every later frame of the SAME gesture stays on the CPU walk
    CHECK(l.latched());
    l.beginFrame(true);
    CHECK(l.latched());
    l.beginFrame(false); // released: the next frame is free to try again
    CHECK_FALSE(l.latched());
}

TEST_CASE("GestureLatch: a refusal outside a gesture does not latch anything") {
    // A document becomes eligible again after an edit, and the frame that discovers it must not
    // have been pre-refused by the frame before it.
    GestureLatch l;
    l.beginFrame(false);
    l.refuse(false, ResidentSkip::Refused);
    CHECK_FALSE(l.latched());
    l.beginFrame(true);
    CHECK_FALSE(l.latched());
}

TEST_CASE("GestureLatch: a MOMENTARY refusal at gesture start does not strand the drag") {
    // ⚠ THE ONE THAT MATTERS NOW THAT DRAGS REACH serve(). A `Refused` names a render::TileRefusal
    // -- a group, an adjustment, a leaf kind, a caps floor -- and none of those can appear because a
    // layer moved, so pinning the stroke to one lane for it is free. The rest name the INSTANT: a
    // canvas texture that could not be prepared or resolved into on one frame, a document whose
    // size has not landed yet. Latching those turned a single hiccup on the press frame into an
    // entire Move drag on the CPU walk -- which is the shape of the bug this whole change exists to
    // delete, reappearing one level down.
    for (const ResidentSkip momentary :
         {ResidentSkip::ResolveTarget, ResidentSkip::NoDocument, ResidentSkip::GpuDragPass,
          ResidentSkip::RecomposeReview, ResidentSkip::GestureLatched}) {
        GestureLatch l;
        l.beginFrame(true);
        l.refuse(true, momentary);
        CHECK_FALSE(l.latched()); // the very next frame of this drag may still be served
    }
}

TEST_CASE("GestureLatch: reset drops it, whatever a gesture was doing") {
    // A document arrived, was replaced, or left: nothing about the old one may decide a frame of
    // the new one.
    GestureLatch l;
    l.beginFrame(true);
    l.refuse(true, ResidentSkip::Refused);
    CHECK(l.latched());
    l.reset();
    CHECK_FALSE(l.latched());
    l.beginFrame(true);
    CHECK_FALSE(l.latched());
}

// ---- What a Move drag actually asks the lane for -----------------------------------------------

TEST_CASE("a Move drag's command claims no pixels and names no region") {
    // The bookkeeping a SERVED drag needs, stated as the properties of the command the Move tool
    // actually pushes (SetTransformsCommand -- the multi-entry one, used for a single layer too):
    //
    //   * no LAYER-PIXEL claim, so the resident lane is never told to re-upload a source. This is
    //     what makes `uploadBytes` 0 for the whole gesture; a claim here would put the dragged
    //     layer's texture back on the bus once per frame.
    //   * no dirty REGION, so onFrame takes the full-composite branch and `residentRecompositeNow`
    //     drains it. A region would be wrong twice over: a moved layer dirties where it WAS as well
    //     as where it IS, and the plan diff already unions exactly those two footprints.
    Document doc(256, 256);
    auto* a = doc.root().addOnTop(doc.makeRaster("A", 128, 128)).as<core::RasterLayer>();
    auto* b = doc.root().addOnTop(doc.makeRaster("B", 64, 64)).as<core::RasterLayer>();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    const std::uint64_t revA = a->contentRevision();
    const std::uint64_t revB = b->contentRevision();

    std::vector<SetTransformsCommand::Entry> entries{
        {a->id(), common::Affine2D::translation(12.0, -7.0)},
        {b->id(), common::Affine2D::translation(12.0, -7.0)}};
    auto cmd = std::make_unique<SetTransformsCommand>(std::move(entries), /*coalesceId=*/7);
    CHECK_FALSE(cmd->dirtyLayerPixels(doc).has_value());
    CHECK_FALSE(cmd->dirtyRegion(doc).has_value());

    doc.commands().push(std::move(cmd));
    // ⚠ THE INVARIANT THE WHOLE OPTIMISATION RESTS ON: a transform is not content. If moving a
    // layer moved its contentRevision, `TileCompositor`'s staleness ledger would re-send its source
    // on every single frame of the drag -- residency would be nominal and the gesture would cost
    // more than the CPU walk it replaced.
    CHECK(a->contentRevision() == revA);
    CHECK(b->contentRevision() == revB);
    CHECK(a->transform().m02 == 12.0); // exact: a translation round-trips bit-for-bit
    CHECK(a->transform().m12 == -7.0);
}

TEST_CASE("the release frame re-plans at full quality, and only when that means anything") {
    // `CompositeOptions::liveDrag` drops Auto to cheap Bilinear mid-gesture; the release frame
    // clears it. That snap-back reaches the resident lane through the PLAN, not through a flush:
    // `resolveTileFilter` is hashed into `Step::fingerprint`, so a filter that changed with the
    // placement standing still is still a placement change, and the diff re-composites the layer's
    // footprint at full quality. If these two ever agree for a rotated layer, the drag's cheap bake
    // would stay resident after the mouse came up.
    const common::Affine2D rotated = common::Affine2D::rotation(0.4);
    CHECK(render::resolveTileFilter(ResampleFilter::Auto, rotated, /*liveDrag=*/true) !=
          render::resolveTileFilter(ResampleFilter::Auto, rotated, /*liveDrag=*/false));

    // ... and the one case where nothing is re-composited is the case where nothing needs to be: a
    // linear-identity placement on the integer grid resolves to Nearest under BOTH values, and
    // Nearest there is an exact whole-pixel copy. Same fingerprint, same pixels, no work.
    const common::Affine2D snapped = common::Affine2D::translation(19.0, -4.0);
    CHECK(render::resolveTileFilter(ResampleFilter::Auto, snapped, /*liveDrag=*/true) ==
          ResampleFilter::Nearest);
    CHECK(render::resolveTileFilter(ResampleFilter::Auto, snapped, /*liveDrag=*/false) ==
          ResampleFilter::Nearest);
}

// ---- PendingRegion: the refusal hand-back ------------------------------------------------------

TEST_CASE("PendingRegion: an unnamed request queues with an EMPTY rect") {
    // The typing path. `requestTextRecomposite` knows a TextLayer changed but not where -- the band
    // is the old cache extent united with the new, and only the refresh can measure it. "Queued
    // with nothing in it" therefore has to be representable, which is the whole reason the flag and
    // the rect are two fields.
    ui::PendingRegion p;
    p.queueUnnamed();
    CHECK(p.queued);
    CHECK(p.rect.empty());

    p.add({10.0, 20.0, 30.0, 40.0}); // ... and the refresh fills it in
    CHECK(p.queued);
    CHECK(p.rect == common::Rect{10.0, 20.0, 30.0, 40.0});
}

TEST_CASE("PendingRegion: a refused resident frame hands the refresh's band back") {
    // THE REGRESSION, as a sequence. A keystroke queues an unnamed pass; the resident lane
    // refreshes the text caches (which report the band ONCE) and then refuses the document for its
    // text layer; the CPU fallback runs later in the same frame and asks the refresh again, which
    // now finds the caches current and reports NOTHING. Unless the refusal put the band back, the
    // fallback unions empty with an empty seed and patches a rect of zero pixels.
    ui::PendingRegion p;
    p.queueUnnamed();
    const common::Rect reported{64.0, 96.0, 200.0, 48.0}; // what the ONE refresh measured
    p.add(reported);                                      // the refusal hands it back

    // The second ask in the same frame -- the no-op refresh -- must not erase it.
    p.add({});
    CHECK(p.queued);
    CHECK(p.rect == reported);
    CHECK_FALSE(p.rect.empty()); // an empty rect here is the bug: nothing would be patched
}

TEST_CASE("PendingRegion: coalescing is the bounding box, and an empty rect is not an edit") {
    ui::PendingRegion p;
    // An empty rect names nothing, so it cannot queue a pass on its own: a frame with no edits must
    // not composite a region.
    p.add({});
    CHECK_FALSE(p.queued);
    CHECK(p.rect.empty());

    p.add({0.0, 0.0, 10.0, 10.0});
    p.add({90.0, 40.0, 10.0, 10.0});
    CHECK(p.queued);
    CHECK(p.rect == common::Rect{0.0, 0.0, 100.0, 50.0});

    // ... and an empty rect does not disturb a pass already queued either.
    p.add({});
    CHECK(p.rect == common::Rect{0.0, 0.0, 100.0, 50.0});
}

TEST_CASE("PendingRegion: clear() drops the rect with the flag") {
    // A full composite supersedes the queued region. Leaving the rect behind an unqueued flag is
    // exactly the stale seed the typing path refuses to inherit: the NEXT unnamed request would
    // start life patching a band that nothing touched.
    ui::PendingRegion p;
    p.add({5.0, 5.0, 5.0, 5.0});
    p.clear();
    CHECK_FALSE(p.queued);
    CHECK(p.rect.empty());

    p.queueUnnamed();
    CHECK(p.queued);
    CHECK(p.rect.empty());
}
