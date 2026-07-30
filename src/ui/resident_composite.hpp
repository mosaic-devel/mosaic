#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "common/log.hpp"
#include "common/profiler.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "render/composite_readback.hpp"
#include "render/compositor.hpp"
#include "render/tile_compositor.hpp"
#include "render/window_renderer.hpp"

// THE APP SIDE OF THE RESIDENT TILED COMPOSITOR (S60-a item 13's wiring; the FLIP ITSELF IS NOT
// MADE HERE). docs/s60-performance-plan.md §7 items 11-13 is the design; this file is the seam
// between `render::TileCompositor` -- which has never run in the app -- and `MainWindow`.
//
// ---- What this is, and what it deliberately is not --------------------------------------------
//
// `render::TileCompositor` composites a document into a device-resident macrotile atlas,
// recomposites only dirty macrotiles, uploads only the dirty macrotiles of an edited layer, and
// resolves straight into the 8-bit present texture with no host round trip. Everything below is
// the plumbing that lets it serve the interactive canvas: which device it lives on, where the
// dirty set comes from, how a refusal falls back, and how the CPU readers of the composite keep
// working when the pixels are no longer in host memory.
//
// It is gated on `MOSAIC_TILE_COMPOSITOR=1` and DEFAULTS OFF. With the variable unset,
// `createIfRequested` returns null before it touches anything, `MainWindow` holds a null pointer,
// and every seam below is a `if (m_tiles != nullptr)` that is not taken -- so the app composites
// exactly as it does today, through `render::Backend::Cpu`, with no new work on the frame path.
// The flip is a benchmark's decision (plan §7 "Item 13: the gate"), not this file's.
//
// ---- Which device, and the one-thread constraint that comes with it ---------------------------
//
// The lane is built on the PRESENTING device via `WindowRenderer::computeContext()`
// (`VulkanContext::adopt`), not on `VulkanContext::shared()`. A VkImage does not cross a VkDevice
// and external memory is not in the Vulkan 1.0 floor this arc stands on, so the accumulator and
// the present texture have to be on one device or item 11 is impossible. See the plan's
// "The presenting-device question, settled".
//
// ⚠ An adopted context's queue mutex CANNOT see the adopter's own vkQueueSubmit, so the adopter
// (WindowRenderer) and the borrower (TileCompositor) must submit from ONE thread. Everything here
// runs on the FLTK UI thread, where both already do; nothing in this file may be moved to a worker
// without revisiting that. It is the first thing to re-read when S60-c takes compositing off the
// UI thread.
//
// ---- Refusals are ordinary ---------------------------------------------------------------------
//
// The lane refuses -- by name -- documents it cannot composite EXACTLY (groups, adjustments, layer
// effects, non-raster leaves, a layer over maxImageDimension2D, a caps floor that does not fit).
// Every refusal falls back to the CPU walk for that frame; the reason is logged ONCE per distinct
// reason, never per frame. A STRUCTURAL refusal taken during a pointer gesture latches for the rest
// of that gesture, so a document that flickers in and out of eligibility cannot make the canvas
// alternate lanes mid-stroke -- see GestureLatch for why only that kind may latch.
//
// ⚠ A fallback is only transparent if it is also LOSSLESS. Anything the lane consumes before it
// knows whether it will serve has to be handed back when it refuses -- see PendingRegion below,
// which is the type that fact lives in.
//
// ---- Who writes the canvas texture (there must be exactly one, per frame) ---------------------
//
// THREE things can write `WindowRenderer`'s canvas VkImage, and a partial resolve laid over a frame
// somebody else wrote is the failure mode this whole file is arranged around:
//
//   1. the HOST UPLOAD -- setDocumentImage / setDocumentRegion, staged in drawFrame. Both present
//      seams funnel `noteCpuFrame()` (app_window's presentComposite / presentCompositeRegion), and
//      `serve()` covers the canvas again on the way back in (`if (!m_serving) markResolveDirty()`).
//   2. the RESOLVE -- `TileCompositor::resolve` into the same image, on its own submit. It writes
//      only the macrotiles it recomposited, which is only safe because of 1 and 3.
//   3. the GPU DRAG PASS -- `WindowRenderer`'s `createDragPipeline` dispatch, from INSIDE drawFrame,
//      outside every present seam. It is the one writer that cannot announce itself, so the app
//      announces it: driveTransformPreview calls `noteCpuFrame()` on every frame the pass is armed,
//      and `ResidentSkip::GpuDragPass` keeps the lane out of those frames entirely.
//
// The arbitration between 2 and 3 is made ONCE, on a gesture's first frame with a real drag target
// (driveTransformPreview declines to arm the pass while the lane is serving), so a drag cannot make
// them alternate frame by frame -- and if the lane refuses mid-gesture anyway, the latch below puts
// the rest of the gesture on the CPU walk rather than handing the texture back and forth.

namespace mosaic::ui {

// The opt-in. `1`, `true`, `yes` and `on` (any case) enable the lane; anything else -- including
// the variable being unset, which is the default -- leaves it off. Deliberately strict: a typo
// must not silently switch the compositor the whole app draws through.
[[nodiscard]] inline bool residentCompositorRequested() noexcept {
    const char* raw = std::getenv("MOSAIC_TILE_COMPOSITOR");
    if (raw == nullptr || *raw == '\0')
        return false;
    std::string v(raw);
    for (char& c : v)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

// Why a frame did not go through the resident lane. Kept apart from `render::TileRefusal` because
// two of these are app-side facts the compositor cannot see: the Recompose review (the canvas shows
// a preview, not the document), and `WindowRenderer`'s own GPU drag pass (which writes the canvas
// texture straight out of drawFrame).
//
// ⚠ "THE CHANNELS TAB IS ISOLATING A CHANNEL" IS NOT ON THIS LIST EITHER, and used to be the
// sharpest entry on it: closing one eye dropped the whole document to the CPU walk plus a
// full-canvas upload per frame, because the remap was a host pass over a COPY of the composite.
// S60-a item 10 moved it into canvas_present.comp, where it is applied on the way OUT of the canvas
// texture -- downstream of every writer, so no lane owes it an exception and the texture keeps
// holding the true composite.
//
// ⚠ "A LIVE MOVE DRAG" IS NOT ON THIS LIST, and used to be. A drag changes a layer's TRANSFORM and
// nothing else, which is exactly what the plan diff was built for: the fingerprint hashes
// placement, the diff dirties the union of where the layer WAS and where it IS, and the layer's
// pixels never move -- so `uploadBytes` stays at 0 for the whole gesture and only the dirty
// macrotiles are recomposited and resolved. The lane's best case is not its exception. What
// survives is narrower and physical: see GpuDragPass.
enum class ResidentSkip : std::uint8_t {
    None = 0,
    NotRequested,     // MOSAIC_TILE_COMPOSITOR is not set -- the ordinary case
    NoRenderer,       // the Vulkan canvas has not come up yet (first frames)
    NoDocument,
    Refused,          // render::TileRefusal -- carried in `reason`
    RecomposeReview,  // the canvas is showing the Recompose preview, not the composite
    GpuDragPass,      // WindowRenderer's drag pass is armed: it writes the canvas texture itself
    ResolveTarget,    // the canvas texture could not be prepared / resolved into
    GestureLatched,   // refused earlier in this gesture; do not thrash lanes mid-stroke
};

[[nodiscard]] inline std::string_view residentSkipName(ResidentSkip s) noexcept {
    switch (s) {
    case ResidentSkip::None: return "none";
    case ResidentSkip::NotRequested: return "not requested";
    case ResidentSkip::NoRenderer: return "no renderer yet";
    case ResidentSkip::NoDocument: return "no document";
    case ResidentSkip::Refused: return "lane refused";
    case ResidentSkip::RecomposeReview: return "recompose review";
    case ResidentSkip::GpuDragPass: return "the GPU drag pass owns the canvas texture";
    case ResidentSkip::ResolveTarget: return "no resolve target";
    case ResidentSkip::GestureLatched: return "refused earlier in this gesture";
    }
    return "unknown";
}

// ---- The app-side eligibility gate, as a value ------------------------------------------------
//
// The facts `MainWindow` knows and `render::TileCompositor` cannot, gathered into one value so the
// decision -- including its ORDER, which is what the once-per-reason log line ends up naming -- is
// pure and testable without a window, a device or a document. `MainWindow::residentEligibility()`
// fills this in and does nothing else.
//
// ⚠ `gpuDragPass` is the one drag-shaped entry left, and it is not about drags: it is about who
// holds the pen. `WindowRenderer`'s drag pass dispatches into the SAME VkImage the resolve writes,
// from inside drawFrame, outside every present seam -- so a frame it owns is a frame the lane must
// not partially resolve into. The app arbitrates ONCE per gesture (app_window's
// driveTransformPreview declines to arm the pass while the lane is serving), so the two cannot
// alternate inside one drag; this gate is the second lock on that same door.
struct ResidentGate {
    bool haveLane = false;         // the opt-in built a lane
    bool haveDocument = false;     // ... and there is a document (and a canvas) to composite
    bool haveRenderer = false;     // ... and the Vulkan canvas has come up
    bool recomposeReview = false;  // the canvas shows the Recompose preview, in its own space
    bool gpuDragPass = false;      // the renderer's drag pass is armed -- it owns the texture
};

[[nodiscard]] inline ResidentSkip residentGateSkip(const ResidentGate& g) noexcept {
    if (!g.haveLane) return ResidentSkip::NotRequested;
    if (!g.haveDocument) return ResidentSkip::NoDocument;
    if (!g.haveRenderer) return ResidentSkip::NoRenderer;
    if (g.recomposeReview) return ResidentSkip::RecomposeReview;
    if (g.gpuDragPass) return ResidentSkip::GpuDragPass;
    return ResidentSkip::None;
}

// ---- The per-gesture refusal latch -------------------------------------------------------------
//
// A refusal taken DURING a pointer gesture holds for the rest of that gesture, so a document that
// flickers in and out of eligibility cannot make the canvas alternate lanes mid-stroke. Its own
// type because WHICH refusals may set it is a real decision, and one worth pinning in a test now
// that a Move drag reaches serve() at all.
//
// ⚠ ONLY A STRUCTURAL REFUSAL LATCHES, and that is the whole of the type. `Refused` carries a
// `render::TileRefusal`, which describes the DOCUMENT -- a group, an adjustment, layer effects, a
// leaf kind with no source, a caps or budget floor. None of those can appear or vanish because a
// layer moved, so latching one costs a re-evaluation nobody wanted and buys a stroke that stays in
// one lane from end to end. Every other refusal describes the INSTANT (no document size yet, a
// canvas texture that could not be prepared or resolved into), and latching one of THOSE is exactly
// how a single hiccup on a gesture's first frame strands the whole drag on the CPU walk. They cost
// nothing to re-ask: if the condition is still there, the next frame refuses again by itself.
//
// The clear is the first frame with no gesture in flight. Two gestures CAN in principle meet
// without such a frame in between -- the frame a release queues is scheduled, not immediate, and a
// fast second press can beat it -- so the latch is allowed to outlive its gesture by one. That is
// survivable precisely because of the rule above: a structural refusal would have refused the next
// gesture too.
class GestureLatch {
public:
    void beginFrame(bool gestureActive) noexcept {
        if (!gestureActive)
            m_latched = false;
    }
    // A frame refused, for `why`. Outside a gesture nothing latches at all -- that is how a document
    // becomes eligible again after an edit changes it.
    void refuse(bool gestureActive, ResidentSkip why) noexcept {
        if (gestureActive && why == ResidentSkip::Refused)
            m_latched = true;
    }
    void reset() noexcept { m_latched = false; } // a document arrived or left; nothing carries over
    [[nodiscard]] bool latched() const noexcept { return m_latched; }

private:
    bool m_latched = false;
};

// ---- The frame's queued dirty region -----------------------------------------------------------
//
// The document-space rect the canvas still owes, and whether a region pass is queued at all. Those
// are two facts, not one, and conflating them is the defect this type exists to prevent: QUEUED
// WITH AN EMPTY RECT IS A REAL STATE. A text edit knows it dirtied something before it can know
// where -- the rect is the OLD text cache's extent united with the NEW one, and only the cache
// refresh can report that -- so it queues an unnamed request and the refresh fills the rect in at
// frame time.
//
// It lives here, next to the lane, because it is what a REFUSAL hands back. `residentRecompositeNow`
// refreshes the text/texture caches before it can learn whether the lane will serve; the refresh
// reports its dirty rect exactly ONCE (the second call in the same frame finds the caches current
// and reports nothing), so a refusal that swallowed that report left the CPU fallback in the same
// frame patching an empty region -- i.e. typed glyphs that never reached the canvas at all. Owed
// pixels go back in the queue.
struct PendingRegion {
    common::Rect rect;   // the union of every rect coalesced into this frame's patch
    bool queued = false; // ⚠ true with an EMPTY `rect` means "changed; ask the refresh where"

    // "Something changed HERE." An empty rect names nothing: it neither queues a pass nor disturbs
    // one already queued. `Rect::united` treats an empty operand as nothing, which is exactly what
    // makes the empty-seed state safe to union into.
    void add(const common::Rect& r) noexcept {
        if (r.empty())
            return;
        rect = queued ? rect.united(r) : r;
        queued = true;
    }

    // "Something changed, and the rect is not knowable yet" -- the typing path. The seed starts
    // EMPTY rather than inheriting last frame's rect, which would patch a band nothing touched.
    void queueUnnamed() noexcept {
        if (!queued)
            rect = {};
        queued = true;
    }

    // Drained, or superseded by a full composite. Both halves go: a rect left behind an unqueued
    // flag is the stale seed `queueUnnamed` refuses to inherit.
    void clear() noexcept {
        rect = {};
        queued = false;
    }
};

// One serve() outcome, for the caller's profiler rows and the once-per-reason log.
struct ResidentServeResult {
    bool served = false;
    ResidentSkip skip = ResidentSkip::None;
    std::string reason;            // the refusal's own words, when there are any
    std::uint64_t macrotiles = 0;  // dirty macrotiles recomposited
    std::uint64_t uploadBytes = 0; // host -> device this composite (0 == fully resident)
};

class ResidentComposite {
public:
    // Build the lane on `wr`'s device. Returns null when the opt-in is absent (the default), when
    // the presenting device cannot host a borrowed compute context, or when the caps gate refuses
    // the kernel. All three are ordinary outcomes and the caller keeps its CPU walk.
    [[nodiscard]] static std::unique_ptr<ResidentComposite> createIfRequested(
        render::WindowRenderer& wr) {
        if (!residentCompositorRequested())
            return nullptr;
        std::shared_ptr<render::VulkanContext> ctx = wr.computeContext();
        if (!ctx) {
            log().warn("resident compositor: the presenting device cannot host a compute context; "
                       "staying on the CPU lane");
            return nullptr;
        }
        std::string error;
        std::unique_ptr<render::TileCompositor> tiles =
            render::TileCompositor::create(std::move(ctx), error);
        if (!tiles) {
            log().warn("resident compositor: {}; staying on the CPU lane", error);
            return nullptr;
        }
        auto self = std::unique_ptr<ResidentComposite>(new ResidentComposite());
        self->m_tiles = std::move(tiles);
        self->m_readback = std::make_unique<render::CompositeReadback>(*self->m_tiles);
        // No macrotile size here: it is chosen in buildAtlas(), i.e. on the first setDocumentSize,
        // and reading it now would report a confident 0. The size is a per-document fact, so it is
        // logged on the first served frame instead of guessed at construction.
        log().info("resident compositor: ON (MOSAIC_TILE_COMPOSITOR), device timer {}",
                   self->m_tiles->hasDeviceTimer() ? "yes" : "no");
        return self;
    }

    ~ResidentComposite() {
        // ⚠ ORDER, and it is not the declaration order: the readback borrows the compositor, so it
        // goes first and the lane second. Member destruction alone would not guarantee that.
        m_readback.reset();
        m_tiles.reset();
    }

    ResidentComposite(const ResidentComposite&) = delete;
    ResidentComposite& operator=(const ResidentComposite&) = delete;

    [[nodiscard]] render::TileCompositor& compositor() noexcept { return *m_tiles; }
    [[nodiscard]] render::CompositeReadback& readback() noexcept { return *m_readback; }
    // True when the LAST frame was drawn by this lane. It is the switch every host-side consumer
    // reads: while it is false, `MainWindow::m_lastComposite` is still the CPU walk's own output
    // and nothing here may touch it.
    [[nodiscard]] bool serving() const noexcept { return m_serving; }

    // ---- The dirty seams --------------------------------------------------------------------
    //
    // ⚠ NONE of these is load-bearing for CORRECTNESS, and that is by design. `TileCompositor`
    // notices a layer whose `contentRevision` moved with no rect attached and re-sends it whole
    // (tile_compositor.cpp's staleness ledger), so a missed claim costs a transfer, never pixels.
    // What they buy is the incremental upload: a 256 px dab that moves 256 KiB instead of a layer.
    // A WRONG claim would cost pixels, which is why `markLayerPixels` takes the layer and derives
    // the layer-LOCAL rect itself rather than trusting a document-space one.

    void markAllDirty() noexcept { m_tiles->markAllDirty(); }
    void markDirty(const common::Rect& docRect) noexcept { m_tiles->markDirty(docRect); }

    // "Everything about this layer's pixels changed." The always-correct claim, for an edit whose
    // extent is not knowable (a whole-image replace, a resize, a mask swap).
    void markLayerWhole(core::LayerId id) noexcept { m_tiles->markLayerDirty(id); }

    // "This layer's pixels changed HERE." `layerRect` is in LAYER-LOCAL pixels -- the space
    // `RasterLayer::image()` is indexed in, NOT document space. Getting that wrong is silent
    // corruption, so every caller in the app passes a rect it took from the edit itself (the
    // command's stored region, the brush's own dirty rect), never one mapped back from the canvas.
    void markLayerPixels(const core::RasterLayer& layer, const common::Rect& layerRect) noexcept {
        if (layerRect.empty()) {
            m_tiles->markLayerDirty(layer.id()); // no region == the whole layer, by contract
            return;
        }
        m_tiles->markLayerDirty(layer, layerRect);
    }

    // A document arrived, was replaced, or left. The source cache is keyed by `core::LayerId` and
    // ids are unique only WITHIN one document, so carrying it across a switch would serve the other
    // document's pixels for a layer whose id and revision happen to match. Not an optimisation.
    void noteDocumentReplaced() noexcept {
        noteLayerTreeReplaced();
        m_loggedSkip = ResidentSkip::None;
        m_loggedReason.clear();
        m_latch.reset();
    }

    // ⚠ THE SAME HAZARD, INSIDE ONE DOCUMENT. An undo/redo that swaps the WHOLE layer tree (the
    // loaded-save-history branch: each earlier save is reconstructed as fresh layer objects) hands
    // back layers that KEEP their ids and RESTART their contentRevision at 0. To a cache keyed by
    // (id, revision) that is indistinguishable from "nothing changed" -- so the two states' pixels
    // could be swapped without a single revision moving. Forgetting the cache is the only claim
    // that stays true; a full resident recomposite is milliseconds and this is a rare step.
    void noteLayerTreeReplaced() noexcept {
        m_tiles->reset();
        m_serving = false;
        m_mirrorRevision = 0;
        m_mirrorW = m_mirrorH = 0;
    }

    // Once per frame, wherever the readback budget is rolled (audit §7(a)). The gesture latch is
    // rolled here too: a frame with nothing held clears it.
    void beginFrame(bool gestureActive) noexcept {
        m_readback->beginFrame();
        m_readback->setGestureActive(gestureActive);
        m_latch.beginFrame(gestureActive);
    }

    // ---- Serve one frame ----------------------------------------------------------------------
    //
    // Composite every dirty macrotile into the resident accumulator and resolve the ones that
    // changed straight into `wr`'s canvas texture -- no readback, no staging copy, no host bytes.
    // False means "the CPU walk owns this frame", for a reason the caller can name.
    [[nodiscard]] ResidentServeResult serve(const core::Document& doc,
                                            const render::CompositeOptions& opts,
                                            render::WindowRenderer& wr, bool gestureActive) {
        ResidentServeResult out;
        if (m_latch.latched() && gestureActive) {
            out.skip = ResidentSkip::GestureLatched;
            // ⚠ NOT through noteSkip(): a latched frame is not a fresh refusal, and re-marking the
            // resolve dirty every frame of a stroke would make the first served frame after the
            // stroke re-resolve the whole canvas for nothing. m_serving is already false -- the
            // refusal that SET the latch cleared it and covered the canvas.
            return out;
        }
        MOSAIC_PERF_SCOPE("Resident composite (serve)", Lane::Gpu);

        const std::uint32_t docW = doc.width();
        const std::uint32_t docH = doc.height();
        if (docW == 0 || docH == 0) {
            out.skip = ResidentSkip::NoDocument;
            return noteSkip(out, gestureActive);
        }
        {
            MOSAIC_PERF_SCOPE("Resident composite (geometry)", Lane::Cpu);
            std::string error;
            if (!m_tiles->setDocumentSize(docW, docH, error)) {
                out.skip = ResidentSkip::Refused;
                out.reason = std::move(error);
                return noteSkip(out, gestureActive);
            }
        }

        const render::TileCompositeStatus status = m_tiles->composite(doc, opts);
        if (!status.ok) {
            out.skip = ResidentSkip::Refused;
            out.reason = std::string(render::tileRefusalName(status.refusal));
            if (!status.error.empty())
                out.reason += " (" + status.error + ")";
            return noteSkip(out, gestureActive);
        }
        out.macrotiles = status.macrotiles;
        out.uploadBytes = status.uploadBytes;

        // The canvas texture, at the document's size, with NOTHING staged into it. The resolve is
        // the only writer from here on.
        if (!wr.prepareResidentCanvas(docW, docH)) {
            out.skip = ResidentSkip::ResolveTarget;
            out.reason = "the canvas texture could not be allocated at the document size";
            return noteSkip(out, gestureActive);
        }
        // A frame the CPU lane served wrote this texture behind the compositor's back, so the
        // partial resolve's untouched macrotiles would still be showing that upload. Cover the
        // canvas once on the way back in; steady frames pay nothing.
        if (!m_serving)
            m_tiles->markResolveDirty();

        render::ResolveTarget dst;
        dst.image = wr.residentCanvasImage();
        dst.view = wr.residentCanvasView();
        dst.width = docW;
        dst.height = docH;
        dst.layout = wr.residentCanvasLayout(); // the renderer is the ONE owner of this fact
        if (!dst.valid()) {
            out.skip = ResidentSkip::ResolveTarget;
            out.reason = "the canvas texture has no image";
            return noteSkip(out, gestureActive);
        }
        bool resolveWrote = false;
        {
            MOSAIC_PERF_SCOPE("Resident composite (resolve)", Lane::Gpu);
            std::string error;
            if (!m_tiles->resolve(dst, error, &resolveWrote)) {
                out.skip = ResidentSkip::ResolveTarget;
                out.reason = std::move(error);
                return noteSkip(out, gestureActive);
            }
        }
        // Only a resolve that SUBMITTED changed the layout. See noteResidentCanvasWritten.
        wr.noteResidentCanvasWritten(resolveWrote);

        // The pinned mirror is refreshed here -- the same place the dirty set was consumed -- but
        // ⚠ NEVER DURING A GESTURE, and that exception is the whole reason the resident lane was
        // *slower* than the CPU walk in its first interactive pass (user report: 25-30 ms spikes on
        // every resident row, no lag with the opt-in off).
        //
        // refreshMirror() is memoised on the accumulator's revision, which reads like a sufficient
        // guard and is exactly the wrong one: during an edit the revision moves EVERY frame, so the
        // memo never hits. With a cursor-readout pin held -- i.e. whenever the pointer is over the
        // canvas, which during a brush stroke is always -- every served frame did a device->host
        // readback and waited on a fence. That is the per-frame round trip item 11 deleted,
        // reintroduced to keep a status-bar colour current that nobody reads mid-stroke. The cost
        // is the FENCE, not the 256 KB: it serialises the frame against the device.
        //
        // Mid-gesture the mirror therefore goes stale, and `peek` REFUSES a stale mirror rather
        // than serving it -- the readout blinks off, which is a glitch, instead of reporting last
        // frame's colour, which is a bug report. One refresh lands on the first non-gesture frame.
        //
        // The gesture guard lives in refreshMirror() itself, not here: this is a policy about what
        // a readback may cost, and a policy every caller has to remember is one a caller will
        // forget. The call stays unconditional so the profiler row keeps reporting -- a row that
        // reads ~0 through a whole stroke is the evidence this stayed fixed.
        {
            MOSAIC_PERF_SCOPE("Resident composite (mirror refresh)", Lane::Gpu);
            m_readback->refreshMirror();
        }

        if (!m_loggedMacrotile) { // now it is a fact rather than a guess -- see createIfRequested
            m_loggedMacrotile = true;
            log().info("resident compositor: serving, macrotile {} px", m_tiles->macrotileSize());
        }
        m_serving = true;
        m_loggedSkip = ResidentSkip::None;
        m_loggedReason.clear();
        out.served = true;
        return out;
    }

    // The caller decided, for its own reasons, that this frame belongs to the CPU walk.
    void noteCpuFrame() noexcept {
        if (m_serving)
            m_tiles->markResolveDirty(); // the CPU upload is about to write the canvas texture
        m_serving = false;
    }

    // ---- Host pixels: the explicit readback seam (item 12) -------------------------------------
    //
    // Under the resident lane `MainWindow::m_lastComposite` stops being the source of truth and
    // becomes a LAZILY MATERIALISED MIRROR. This is the one place that brings it back, and it is
    // memoised on the accumulator's revision, so a consumer that asks repeatedly between edits
    // (the eyedropper following the pointer) pays exactly once.
    //
    // ⚠ It is NOT the per-frame path. `serve()` is, and it moves zero host bytes. A consumer that
    // both fires per pointer event AND runs while the composite is changing (the status-bar cursor
    // readout during a brush stroke) must use `peekPixel` instead -- that is what the pinned mirror
    // is for, and it is why `peek` has no "fetch if absent" branch.
    void materialise(common::Image& mirror, std::string_view consumer,
                     render::Freshness freshness = render::Freshness::AnyRecent) {
        if (!m_serving)
            return; // the CPU walk owns the mirror; leave its pixels exactly where they are
        // ⚠ A gesture frame does not get a BLOCKING FULL-CANVAS readback, and `Freshness` is the
        // whole point of the enum: `AnyRecent` may lag, `Settled` may lag a gesture in flight and
        // must be current once it ends. Honouring that is not an optimisation -- the histogram
        // provider asks with AnyRecent, so with the Channels panel open this fired on every
        // revision bump, i.e. every frame of a brush stroke, dragging the entire canvas back across
        // the bus (~33 MB at 3840x2160) and fencing on it. The revision memo does not bound that
        // for exactly the reason it does not bound the pinned mirror: during an edit the revision
        // moves every frame, so the memo never hits. Stale is correct here -- nobody reads a
        // histogram mid-stroke -- and the first post-gesture ask refreshes it.
        if (freshness != render::Freshness::Current &&
            (m_readback->gestureActive() || m_readback->editingThisFrame()))
            return;
        const std::uint64_t rev = m_tiles->revision();
        if (rev == m_mirrorRevision && !mirror.empty() && mirror.width == m_mirrorW &&
            mirror.height == m_mirrorH)
            return;
        MOSAIC_PERF_SCOPE("Composite mirror (host readback)", Lane::Gpu);
        render::ReadbackRequest req;
        req.freshness = freshness;
        req.blocking = true;
        req.name = consumer;
        render::ReadbackResult r = m_readback->request(req).get();
        if (!r.ok) {
            log().warn("resident compositor: readback for \"{}\" failed: {}", consumer, r.error);
            return;
        }
        mirror = std::move(r.image);
        m_mirrorRevision = r.revision;
        m_mirrorW = mirror.width;
        m_mirrorH = mirror.height;
    }

    // ⚠ A1 (the status-bar cursor readout) DOES NOT LIVE HERE ANY MORE, and the pinned-mirror
    // helpers it used are gone with it. The mirror made the READ free but never the FILL: seeding
    // a pin is a macrotile transfer plus a fence, the pointer crosses macrotiles constantly, and
    // each seed measured ~20 ms against a live canvas because the fence waits behind the frame
    // already in flight. Throttling turned a continuous cost into a periodic hitch, which is not
    // a fix. app_window now composites the single pixel on the CPU instead -- microseconds, no
    // device contact, no fence, and within the 1 LSB the parity tests already hold the GPU lane to.
    //
    // `render::CompositeReadback`'s pin/peek machinery is untouched and still correct; what this
    // records is that a consumer firing PER POINTER EVENT is not a candidate for it, because
    // acquiring the pin is the expensive half.


    // Log a skip ONCE per distinct reason. An unnamed lane switch is how a regression hides; a
    // per-frame log line is how a log stops being read.
    void logSkipOnce(const ResidentServeResult& r) {
        if (r.served || r.skip == ResidentSkip::None)
            return;
        if (r.skip == m_loggedSkip && r.reason == m_loggedReason)
            return;
        m_loggedSkip = r.skip;
        m_loggedReason = r.reason;
        if (r.reason.empty())
            log().info("resident compositor: CPU lane this frame -- {}", residentSkipName(r.skip));
        else
            log().info("resident compositor: CPU lane this frame -- {}: {}",
                       residentSkipName(r.skip), r.reason);
    }

private:
    ResidentComposite() = default;

    [[nodiscard]] static spdlog::logger& log() {
        static const auto logger = common::log::category("ui");
        return *logger;
    }

    ResidentServeResult& noteSkip(ResidentServeResult& out, bool gestureActive) noexcept {
        m_latch.refuse(gestureActive, out.skip); // a STRUCTURAL no pins the stroke to one lane
        if (m_serving)
            m_tiles->markResolveDirty(); // the CPU upload will write the texture we resolved into
        m_serving = false;
        return out;
    }

    std::unique_ptr<render::TileCompositor> m_tiles;
    std::unique_ptr<render::CompositeReadback> m_readback;
    bool m_serving = false;
    bool m_loggedMacrotile = false; // the macrotile size is logged once, on the first served frame
    GestureLatch m_latch;

    // The materialised-mirror memo (see materialise()).
    std::uint64_t m_mirrorRevision = 0;
    std::uint32_t m_mirrorW = 0, m_mirrorH = 0;

    // A1's pinned macrotile, and the doc-space bounds it covers.

    ResidentSkip m_loggedSkip = ResidentSkip::None;
    std::string m_loggedReason;
};

}  // namespace mosaic::ui
