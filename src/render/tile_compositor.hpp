#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/layer.hpp"
#include "core/tile_grid.hpp"
#include "render/compositor.hpp"
#include "render/gpu_caps.hpp"
#include "render/tile_residency.hpp"

// The RESIDENT TILED COMPOSITOR (S60-a items 8/9; docs/s60-performance-plan.md §3, §4).
//
// This is the host half of `shaders/composite_tile.comp`. The shader absorbs the whole per-layer
// step -- inverse transform, resample, mask fold, clip-to-below, blend -- in ONE dispatch per
// macrotile per layer; this class keeps the pixels those dispatches read and write ON THE DEVICE
// between frames, which is the part that decides whether any of it pays.
//
// ---- Why residency, restated as the thing the code must not do -------------------------------
//
// S7-b's `GpuCompositor` uploads both operands and reads the accumulator back PER LAYER. At
// 1920x1080 that is ~25 MB of PCIe traffic per layer per frame, which is why it is SLOWER than the
// CPU walk (§1.1) despite doing the blend on a GPU. So the invariants here are:
//
//   1. A layer's pixels are uploaded when they CHANGE, never per frame and never per tile.
//   2. The accumulator is never read back as a side effect of compositing. Readback is an
//      explicit, named call (`readback()`), and `docs/s60-readback-consumers.md` is the list of
//      who is allowed to make it.
//   3. Only DIRTY macrotiles are recomposited. The dirty set is the `.mosaic` 64 px grid
//      (core::TileGrid), projected up to `64 << k` macrotiles for dispatch (§3.1).
//
// ---- What this cut does NOT do, stated so it is not mistaken for a bug -----------------------
//
// The lane REFUSES a document it cannot composite exactly, and the caller falls back to
// `render::composite(..., Backend::Cpu)`. Refusing is not a failure mode, it is the design
// (gpu_caps.hpp: "a GPU lane that does not fit refuses ITSELF"). Today it refuses groups, layer
// effects, live coverage partitions, VECTOR leaves, the adjustment KINDS listed below, and any
// layer whose source exceeds the device's `maxImageDimension2D`. Each of those needs a walk the
// kernel does not express yet; a lane that guessed at them would draw the wrong picture.
//
// It DOES serve every leaf that composites FROM A FIXED-RESOLUTION SOURCE IMAGE, which is raster,
// magic, text and texture (S60-a; compositor.cpp's renderLayerRaw leaf arm treats all four the
// same way). The two cache-backed kinds differ from raster in exactly one place -- the cache's own
// pixel -> layer-local map folds in FRONT of the layer transform -- and in what makes their device
// copy stale, which is `cacheGeneration()` and NOT `contentRevision()`; see LayerSource's ledger.
// Vector stays refused because there IS no fixed-resolution source: the CPU lane rasterises the
// object analytically at TARGET resolution through the placement (core::vec::rasterizeObjectF), so
// a bitmap stand-in would be a different picture at every zoom, not merely a slower one.
//
// ---- ADJUSTMENT LAYERS, and why admission is PER KIND -----------------------------------------
//
// An adjustment is not a source at all: it is a function of the accumulated backdrop, so it takes
// a SECOND KERNEL (`shaders/adjust_tile.comp`) over the same dirty macrotiles -- load the
// accumulator, apply the transfer, store it back -- under the same opacity x mask x clip-to-below
// modulation compositor.cpp's walkStep applies. Nothing round-trips to the host, so `uploadBytes`
// does not move for an adjustment that carries neither a mask nor a lookup table, and
// `stats().readbacks` stays at zero either way.
//
// ADMISSION IS PER KIND, and that is the load-bearing decision: `core::AdjustmentKind` has ~30
// members and a lane that guessed at an unported one would draw the wrong picture, which is worse
// than being slow. So the served set is exactly the PER-PIXEL, FINITE-SLOPE transfers --
// Invert, Brightness/Contrast, Levels, Exposure, Hue/Saturation, Color Balance, Grayscale
// (its per-pixel projections, at a continuous palette), Curves, Gradient Map, Vibrance,
// Photo Filter and Haze Removal -- and every other kind is a named refusal that sends the whole
// document to the CPU walk, exactly as it went before. The bound that draws the line is stated in
// adjust_tile.comp: the reference works in fp32 and the accumulator is rgba16f, so a transfer
// amplifies a ~2^-11 relative difference by its own |f'|; a LATTICE (Threshold, Posterize, a
// quantised Grayscale) has an infinite slope at every step and flips a whole level, which 1/255
// cannot hold. `tileRefusalName` names the reason and `TileCompositeStatus::error` names the kind.
//
// ⚠ AN ADJUSTMENT'S BLEND MODE IS NOT READ, BY EITHER LANE. compositor.cpp's walkStep takes the
// adjustment branch before any `blend()` call, so the mode never reaches a pixel; the kernel and
// the plan-diff fingerprint both mirror that omission deliberately rather than by oversight.
//
// The CPU reference in `compositor.cpp` is untouched and stays the golden lane. Parity is proven
// per blend mode x per resample filter at 1/255 in tests/test_composite_tile_parity.cpp (the
// kernel) and tests/test_tile_compositor.cpp (this class, end to end against `render::composite`).

namespace mosaic::core {
class Document;
}

namespace mosaic::render {

class VulkanContext;
class GpuTimer;

// Why the lane declined. Every value is a documented, testable predicate rather than a generic
// failure: a caller that logs the name gets a one-line answer to "why is this document on the CPU
// path", which is the diagnostic §6.3 of the plan wants in the Settings capability readout.
enum class TileRefusal : std::uint8_t {
    None = 0,
    NoDevice,          // no usable Vulkan device at all
    DeviceTooSmall,    // the caps gate said no (descriptors, push constants, working format)
    DocumentTooLarge,  // the resident accumulator would not fit the memory budget
    OutOfBudget,       // this composite needs more source bytes at once than the atlas holds
    NestedGroup,       // a GroupLayer child -- needs the group's own local buffer
    // An adjustment layer whose KIND `shaders/adjust_tile.comp` does not serve: a spatial or
    // stylize kind (it reads a neighbourhood the dirty set does not cover), a lattice quantiser
    // (an fp16 backdrop flips a whole level at a step), or one whose transfer is conditioned on
    // 1/alpha. `TileCompositeStatus::error` carries the kind's own name and the pixel-level
    // reason; the served kinds are listed at the top of this header.
    Adjustment,
    LayerEffects,      // non-empty LayerEffects -- S60-d
    LivePartition,     // a live coverage partition rewrites alpha at render time
    // A leaf with no fixed-resolution source to make resident: a VECTOR layer (rasterised
    // analytically at target resolution, so there is nothing of fixed size to upload), or a
    // text/texture layer whose pixel cache the app has not populated yet.
    UnsupportedKind,
    LayerTooLarge,     // the layer's source exceeds maxImageDimension2D (the kernel's source
                       // WINDOW is the way out; the host wiring for it is item 10's work)
    SingularTransform, // a non-invertible placement -- the kernel needs the inverse
    DeviceError,       // a Vulkan call failed; `error` carries which
};
[[nodiscard]] std::string_view tileRefusalName(TileRefusal r) noexcept;

// ---- The DISPATCH SHAPE (S60-a item 10) --------------------------------------------------------
//
// All three shapes composite the SAME pixels -- byte for byte, not merely within a tolerance --
// because the kernel evaluates every sample at its TARGET pixel and the three integers that locate
// a macrotile are identical whichever way they reach the shader. What differs is only how much
// host work buys one dispatch, so this is a measurement knob and a fallback ladder, never a
// picture setting.
enum class TileDispatch : std::uint8_t {
    // Whatever this device supports best: Indexed where the caps allow it, TileList otherwise.
    // The shipping choice, and the only one the app ever asks for.
    Auto = 0,
    // ONE DISPATCH PER (LAYER, MACROTILE), the tile's geometry in push constants. S60-a item 9's
    // original shape; kept as the reference the other two are proven against, and as the answer
    // for the one case the list cannot serve (a run longer than maxComputeWorkGroupCount[2]).
    PerTile,
    // ONE DISPATCH PER (LAYER, ATLAS IMAGE): the dirty-macrotile list lives in a storage buffer
    // and an invocation maps ITSELF to (macrotile, pixel). Available at the Vulkan 1.0 FLOOR --
    // one storage buffer against a guaranteed four -- and it is most of item 10's win on a
    // single-atlas document, which is every document that fits maxImageDimension2D.
    TileList,
    // TileList, plus a runtime-sized descriptor array of layer sheets, so the per-layer source
    // and mask binds collapse into ONE descriptor set for the whole composite. Needs
    // GpuCaps::descriptorIndexing AND a device that accepts the variant blob's SPIR-V.
    Indexed,
};
[[nodiscard]] std::string_view tileDispatchName(TileDispatch d) noexcept;

// Why the descriptor-indexed shape is not the active one. NEVER an error: every value here means
// "the floor path serves this composite instead", and the floor path draws the same picture.
// `MOSAIC_GPU_PROFILE=floor` produces NoDescriptorIndexing on any device, which is the point.
enum class DispatchRefusal : std::uint8_t {
    None = 0,
    NotRequested,         // the caller asked for PerTile or TileList
    NoDescriptorIndexing, // GpuCaps::descriptorIndexing is false
    SpirvUnsupported,     // the device will not load the variant blob's SPIR-V version
    DescriptorBudget,     // the runtime array does not fit the device's sampled-image limits
    PipelineFailed,       // vkCreate* declined the variant; the floor path is untouched
    TooManyLayers,        // this document has more layers than the array can hold
};
[[nodiscard]] std::string_view dispatchRefusalName(DispatchRefusal r) noexcept;

// ---- Host-side resolution the kernel relies on ------------------------------------------------
//
// The kernel is told a CONCRETE filter and a CONCRETE sub-sample count; resolving them host-side
// in double is what keeps a ceil() or an Auto lookup from straddling differently on the two lanes.
// Both are pure, so they are unit-tested without a device.

// Is `t` the document's own integer pixel grid -- identity linear part, whole-pixel translation?
[[nodiscard]] bool isLosslessGridPlacement(const common::Affine2D& t) noexcept;

// The filter to hand the kernel for a layer placed by `t`. Two collapses the CPU reference
// performs implicitly and the kernel therefore must be told about:
//   1. ResampleFilter::Auto -> chooseAutoFilter(t, liveDrag).
//   2. The lossless fast path -> Nearest. compositor.cpp short-circuits a linear-identity
//      placement with an integer translation (or ANY translation under Nearest) to a whole-pixel
//      COPY, which is exactly what Nearest computes -- but NOT what convolving Mitchell or
//      Gaussian computes, because those kernels approximate rather than interpolate and blur even
//      at integer offsets.
[[nodiscard]] ResampleFilter resolveTileFilter(ResampleFilter user, const common::Affine2D& t,
                                               bool liveDrag) noexcept;

// supersampleInto's NxN sub-sample count for an inverse placement, clamped to [2,8] as the CPU
// reference clamps it.
[[nodiscard]] std::int32_t tileSupersampleN(const common::Affine2D& inverse) noexcept;

// What one composite did. Deliberately reports the WORK as well as the outcome: a resident
// compositor that quietly recomposites every macrotile every frame is indistinguishable from a
// non-resident one by wall clock alone on a fast device, and this is how a test tells them apart.
struct TileCompositeStatus {
    bool ok = false;
    TileRefusal refusal = TileRefusal::None;
    std::string error;
    std::uint64_t layers = 0;            // layers dispatched
    // How many of those `layers` ran the ADJUSTMENT kernel rather than the composite one. Reported
    // because the two are indistinguishable from the pixels and from `dispatches`: a test that
    // means "the adjustment lane actually ran" has to be able to say so, and a plan that silently
    // dropped an identity adjustment (which is correct -- the CPU returns before touching a pixel)
    // must be tellable from one that dispatched it.
    std::uint64_t adjustments = 0;
    std::uint64_t macrotiles = 0;        // dirty macrotiles recomposited
    // vkCmdDispatch calls issued. PerTile makes this `layers * macrotiles` -- §3.1's cost model,
    // and the number item 10 exists to reduce; TileList and Indexed make it
    // `layers * accumulator atlas images`, which is `layers` for any document that fits one atlas.
    std::uint64_t dispatches = 0;
    // Which shape actually served, AFTER every per-composite downgrade. Reported rather than
    // inferred, because "the indexed path ran" and "the indexed path silently declined and the
    // floor path drew the same picture" are indistinguishable from the pixels -- which is the
    // design, and also why a test has to be told.
    TileDispatch dispatch = TileDispatch::PerTile;
    std::uint64_t uploadBytes = 0;       // host -> device this composite (0 == fully resident)
    // The INCREMENTAL-UPLOAD witness (S60-a, the 2026-07-28 gate's condition 3). A cache SIZE
    // cannot see a re-upload -- re-sending a layer's whole image leaves `residentSourceBytes()`
    // exactly where it was -- so the only honest instrument is an event count of what crossed the
    // bus. `partialUploads + fullUploads` is the number of layers refreshed this composite.
    std::uint64_t uploadRegions = 0;     // vkCmdCopyBufferToImage regions issued
    std::uint64_t partialUploads = 0;    // layers refreshed by dirty macrotile
    std::uint64_t fullUploads = 0;       // layers re-sent whole (the always-correct fallback)
};

// Cumulative counters, for the profiler and for `--bench`'s readback column (readback audit §7b).
//
// ⚠ `readbacks` / `readbackBytes` are the number the whole arc is judged on. They must stay at ZERO
// across a steady stream of frames: the present path resolves ON THE DEVICE (`resolve()`), so the
// only things that may move them are the named CPU consumers in docs/s60-readback-consumers.md.
// A test that watches them across N frames is the regression net the plan's §9 asks for, and it
// sees what no wall clock can.
struct TileCompositeStats {
    std::uint64_t composites = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t macrotiles = 0;
    std::uint64_t uploadBytes = 0;
    std::uint64_t uploadRegions = 0;  // copy regions issued (see TileCompositeStatus)
    std::uint64_t partialUploads = 0; // layers refreshed by dirty macrotile
    std::uint64_t fullUploads = 0;    // layers re-sent whole
    std::uint64_t readbacks = 0;
    std::uint64_t readbackBytes = 0;
    std::uint64_t refusals = 0;
    std::uint64_t resolves = 0;       // resolve() calls that actually submitted work
    std::uint64_t resolveTiles = 0;   // macrotiles converted to 8-bit for the present pass
};

// Where `resolve()` writes: an 8-bit document-space image on the SAME device as the accumulator.
//
// In the app this is `WindowRenderer`'s canvas texture, so the composite reaches the screen without
// touching host memory at all. `memory` is set only for a target this class allocated
// (createResolveTarget); a borrowed one leaves it null and this class never frees it.
struct ResolveTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;  // owned iff non-null; see createResolveTarget
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The layout the image is in ON ENTRY to resolve(). The caller owns this fact -- guessing it
    // is either a validation error or, with UNDEFINED, silently discarded pixels in every
    // macrotile the resolve does not rewrite. resolve() always LEAVES the image in GENERAL.
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    [[nodiscard]] bool valid() const noexcept { return image != VK_NULL_HANDLE; }
};

class TileCompositor {
public:
    // Build the lane on `ctx`. Returns null with `error` set when there is no device, or when the
    // caps gate says this kernel does not fit; both are ordinary outcomes and the caller takes its
    // CPU lane.
    //
    // ⚠ WHICH DEVICE (the S60-a decision, plan §7 item 3's deferred question). The accumulator has
    // to live on the device that PRESENTS it, or item 11 is impossible -- a VkImage does not cross
    // a VkDevice, and external memory is not in the Vulkan 1.0 floor. So the app passes
    // `WindowRenderer::computeContext()`, a context BORROWING the window's own device, and gets a
    // present pass that samples the resident composite directly. Headless callers (tests, --bench,
    // --composite-demo) pass nothing and get `VulkanContext::shared()`, which keeps the shared-
    // device path exercised. Both are one code path here on purpose.
    [[nodiscard]] static std::unique_ptr<TileCompositor> create(std::shared_ptr<VulkanContext> ctx,
                                                                std::string& error);
    // The headless overload: `VulkanContext::shared()`.
    [[nodiscard]] static std::unique_ptr<TileCompositor> create(std::string& error);

    ~TileCompositor();

    TileCompositor(const TileCompositor&) = delete;
    TileCompositor& operator=(const TileCompositor&) = delete;
    TileCompositor(TileCompositor&&) = delete;
    TileCompositor& operator=(TileCompositor&&) = delete;

    [[nodiscard]] const GpuCaps& caps() const noexcept;
    [[nodiscard]] std::uint32_t validationErrors() const noexcept;

    // True when this lane can report REAL device time (a timestamp query pool was created) rather
    // than only the submit wall-clock its `Lane::Gpu` rows carry. False is an ordinary outcome --
    // `MOSAIC_GPU_PROFILE=floor` produces it deliberately -- and changes nothing about the pixels.
    // See render/gpu_timer.hpp and docs/s60-performance-plan.md section 8.1.
    [[nodiscard]] bool hasDeviceTimer() const noexcept { return m_timer != nullptr; }

    // ---- The dispatch shape (item 10) ---------------------------------------------------------

    // What the caller ASKED for. `Auto` is the shipping choice and the default; the explicit
    // values exist so `--bench` can measure the three against each other and so the tests can
    // assert that they produce identical bytes. This is deliberately NOT a user setting: there is
    // one correct answer per device and `Auto` already knows it. `MOSAIC_TILE_DISPATCH`
    // (per-tile / list / indexed) seeds it at create() for a measurement run.
    void setDispatchMode(TileDispatch d) noexcept { m_requested = d; }
    [[nodiscard]] TileDispatch dispatchMode() const noexcept { return m_requested; }
    // What this device can actually run, given the request and the caps gate. A composite may
    // still downgrade further for reasons only it can see (see TileCompositeStatus::dispatch).
    [[nodiscard]] TileDispatch activeDispatch() const noexcept;
    // Why `Indexed` is not available here. `None` means it is.
    [[nodiscard]] DispatchRefusal indexedRefusal() const noexcept { return m_indexedRefusal; }

    // ---- Geometry ---------------------------------------------------------------------------

    // Point the compositor at a document size. Reallocates the accumulator atlas and marks
    // everything dirty; a no-op when the size is unchanged. False (with `error`) means the
    // accumulator does not fit the memory budget -- the caller stays on the CPU lane.
    [[nodiscard]] bool setDocumentSize(std::uint32_t w, std::uint32_t h, std::string& error);

    [[nodiscard]] std::uint32_t documentWidth() const noexcept { return m_docW; }
    [[nodiscard]] std::uint32_t documentHeight() const noexcept { return m_docH; }
    // The DIRTY-tracking grid: 64 px, the `.mosaic` store's own grid, shared verbatim so one
    // dirty set feeds both the recomposite and the autosave journal (§3.1).
    [[nodiscard]] const core::TileGrid& dirtyGrid() const noexcept { return m_dirty.grid(); }
    // The DISPATCH grid: `64 << k` macrotiles, k from GpuCaps. A dirty 64 px tile marks its
    // containing macrotile dirty.
    [[nodiscard]] const core::TileGrid& macroGrid() const noexcept { return m_macroGrid; }
    [[nodiscard]] std::uint32_t macrotileSize() const noexcept { return m_macrotile; }

    // ---- The dirty set ------------------------------------------------------------------------

    // ⚠ Forget EVERYTHING: the accumulator's contents, every resident layer image, and the plan.
    //
    // This is not an optimisation knob, it is a correctness requirement on DOCUMENT SWITCH. The
    // source cache is keyed by `core::LayerId`, and layer ids are unique only WITHIN one document
    // -- two documents both mint 1, 2, 3 -- so a cache carried across a switch would serve the
    // other document's pixels for a layer whose id and revision both happen to match. `composite`
    // resets itself when it is handed a different `core::Document` object, which covers a live tab
    // switch; it CANNOT cover a document that was destroyed and replaced at the same address, so
    // the owner must call this when it closes or replaces a document.
    void reset() noexcept;

    void markAllDirty() noexcept;
    void markDirty(const common::Rect& docRect) noexcept;  // every 64 px tile the rect touches
    // A layer's pixels or mask changed: drop its device copy and dirty wherever it projects. Call
    // this from the same place the autosave journal is told, so the two dirty sets cannot diverge.
    //
    // This overload means EVERYTHING about the layer's pixels: the next composite re-sends the
    // whole image. That is always correct and it is what a caller with no region information must
    // use; the overload below is what makes a brush dab cost a macrotile instead of a canvas.
    void markLayerDirty(core::LayerId id) noexcept;

    // ---- INCREMENTAL UPLOAD (S60-a; the 2026-07-28 gate's condition 3) ------------------------
    //
    // The same signal, plus WHERE. `layerRect` is in LAYER-LOCAL pixels -- the space
    // `RasterLayer::image()` is indexed in, not document space -- because that is the space the
    // upload copies out of. The next composite then refreshes only the macrotiles of that layer's
    // device image which the rect touches, and dirties only the document macrotiles those pixels
    // project onto.
    //
    // ⚠ THE CONTRACT, and it is on the CALLER: bump the layer's `contentRevision` first
    // (`invalidateContentBounds()`), then call this with the rect that edit touched, and call it
    // for EVERY such edit. The class enforces the contract rather than trusting it -- a content
    // revision that moves without an accompanying rect, a rect that skips a revision step, a
    // resized or reformatted image, a mask change, or a layer with no device copy yet all drop
    // straight back to the whole-layer upload above. A missed region can therefore cost time; it
    // can never cost pixels.
    //
    // Pixels are BIT-IDENTICAL either way. This is a pure transfer optimisation: the composited
    // result of a partial upload plus a partial recomposite must equal a full upload plus a full
    // recomposite byte for byte, and tests/test_tile_compositor.cpp asserts exactly that.
    void markLayerDirty(const core::RasterLayer& layer, const common::Rect& layerRect) noexcept;

    [[nodiscard]] bool anyDirty() const noexcept { return !m_dirty.empty(); }
    [[nodiscard]] const core::TileSet& dirtySet() const noexcept { return m_dirty; }

    // ---- The composite ------------------------------------------------------------------------

    // Recomposite every dirty macrotile of `doc` into the resident accumulator. Layers that did
    // not change since the last call are NOT re-uploaded; macrotiles that are not dirty are NOT
    // touched. Returns ok=false with a `refusal` when the lane cannot serve this document
    // exactly, in which case the accumulator is left as it was and the caller uses the CPU lane.
    [[nodiscard]] TileCompositeStatus composite(const core::Document& doc,
                                                const CompositeOptions& opts);

    // A token that moves whenever the accumulator's CONTENT changed. Not a hash: an identical
    // recomposite still advances it, exactly like `MainWindow::m_compositeRevision`, which is what
    // the staleness-tolerant consumers already memoize against.
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

    // ---- The present path (item 11): resolve ON THE DEVICE, never read back ---------------------

    // Convert every macrotile recomposited since the last resolve into `dst`, an 8-bit
    // document-space image on this device. Leaves `dst` in VK_IMAGE_LAYOUT_GENERAL.
    //
    // THIS IS WHAT KILLS THE PER-FRAME READBACK. The old chain for one dab was: region composite ->
    // patch a doc-sized CPU mirror -> copy the sub-rect -> copy it again -> memcpy into staging ->
    // vkCmdCopyBufferToImage. Now the pixels are already on the device and stay there; only the
    // macrotiles that changed are touched, and `stats().readbackBytes` stays at zero.
    //
    // Resolves EVERYTHING when the destination image changes identity or size, or when the
    // accumulator was reset -- a partial resolve into an image that does not already hold this
    // document would leave the untouched macrotiles showing the previous one.
    // `wrote` (optional) reports whether the image was actually SUBMITTED to -- false means the
    // composite changed nothing and the target is already correct. ⚠ The caller must not claim the
    // image changed layout when this is false: resolve leaves it exactly as it found it, and a
    // caller that "notes" a write anyway desynchronises the renderer's layout tracking from the
    // image, which surfaces as a storage-descriptor layout mismatch one frame later.
    [[nodiscard]] bool resolve(const ResolveTarget& dst, std::string& error, bool* wrote = nullptr);

    // Force the next resolve to cover the whole canvas. For a caller that knows the destination's
    // contents were invalidated behind this class's back (a swapchain-driven texture rebuild).
    void markResolveDirty() noexcept;

    // Allocate / free a resolve target on this device. The app does NOT use these -- it resolves
    // into `WindowRenderer`'s canvas texture, which is the whole point -- but a headless caller
    // (a test, a future export lane) needs a destination and should not have to write Vulkan.
    [[nodiscard]] bool createResolveTarget(std::uint32_t w, std::uint32_t h, ResolveTarget& out,
                                           std::string& error);
    void destroyResolveTarget(ResolveTarget& t) noexcept;

    // ---- The readback seam (item 12) ------------------------------------------------------------

    // Copy `roi` (document pixels; empty == the whole canvas) out of the resident accumulator into
    // an 8-bit straight-alpha image. EXPLICIT and synchronous on purpose: every caller has to
    // write this line, which is the guard `docs/s60-readback-consumers.md` §7(c) asks for. It is
    // NOT the per-frame path -- `resolve()` is, and `render::CompositeReadback`'s pinned mirror is
    // what the per-event consumers (the cursor readout, the eyedropper loupe) read instead.
    [[nodiscard]] bool readback(const common::Rect& roi, common::Image& out, std::string& error);

    // Copy `roi` back out of an 8-BIT target (a resolve destination). Cheaper than `readback()` --
    // half the bytes and no fp16 conversion -- but it is only as current as the last resolve, so
    // it serves `Freshness::AnyRecent` consumers and verification, never a `Current` one.
    [[nodiscard]] bool readTarget(const ResolveTarget& src, const common::Rect& roi,
                                  common::Image& out, std::string& error);

    [[nodiscard]] const TileResidency& residency() const noexcept { return m_residency; }
    [[nodiscard]] const TileCompositeStats& stats() const noexcept { return m_stats; }
    void resetStats() noexcept { m_stats = {}; }

    // The number of source bytes currently held on the device. A test asserting that this does
    // NOT move across frames is asserting residency itself.
    [[nodiscard]] std::uint64_t residentSourceBytes() const noexcept;

private:
    TileCompositor() = default;

    // ---- Small Vulkan holders (raw Vulkan, not VMA: a handful of long-lived allocations, well
    // under the guaranteed maxMemoryAllocationCount, and nothing hiding the layout) -------------
    struct GpuImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        [[nodiscard]] bool valid() const noexcept { return image != VK_NULL_HANDLE; }
    };
    struct GpuBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    // One layer's device-resident pixels. The whole layer is bound as one image in this cut; the
    // kernel's source WINDOW (composite_tile.comp's .w push lanes) is what lets a layer bigger
    // than maxImageDimension2D be composited, and wiring it needs the per-tile descriptor loop
    // that item 10 brings. Until then an oversized layer is a clean refusal.
    struct LayerSource {
        GpuImage pixels;                  // the leaf's source sheet, uploaded verbatim; see
                                          // `format` for which of the two encodings it is in
        GpuImage mask;                    // R8_UNORM coverage, or invalid when there is none
        // R8G8B8A8_UNORM for every 8-bit source (a raster/magic layer's own storage -- also the
        // more ACCURATE choice, see test_composite_tile_parity's format case -- and the 8-bit
        // text/texture caches), R16G16B16A16_SFLOAT for the texture generator's FLOAT lane. Part
        // of the staleness key: a layer that switched lanes keeps its dimensions and needs a new
        // image anyway.
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        // ⚠ THE REVISION `pixels` HOLDS -- and it is the layer's SOURCE revision, which is
        // `contentRevision()` only for the kinds whose source IS their own storage. A text or
        // texture layer's source is an app-populated CACHE that is re-rendered without
        // contentRevision moving (a draft bake, a crisp re-render, a clip flip, a canvas resize),
        // so for those two it is `cacheGeneration()`. tile_compositor.cpp's `leafSourceFor` is the
        // single place that decides, and every ledger field below speaks the key it returns.
        std::uint64_t sourceRevision = 0;
        std::uint64_t maskRevision = 0;
        std::uint32_t maskW = 0, maskH = 0;
        std::uint32_t pages = 0;          // residency pages charged for this layer
        std::uint64_t bytes = 0;

        // ---- The staleness ledger the incremental upload runs on ----------------------------
        //
        // THE INVARIANT, and everything below exists to keep it true: `pixels` equals the layer's
        // CPU image everywhere EXCEPT inside `pending` (when `pendingKnown`), or nowhere at all
        // (when `!pendingKnown`). A full upload re-establishes it with an empty `pending`.
        //
        // `pending` is a core::TileSet on the shared 64 px grid -- the same vocabulary as the
        // document dirty set, sized to THIS LAYER's image rather than the canvas -- so the upload
        // and the recomposite project through the same `macrotiles(k)` and cannot disagree about
        // what a macrotile is.
        //
        // `pendingRevision` is the SOURCE revision `pending` describes up to. It is the guard
        // against the one dangerous case: an edit that bumps the revision WITHOUT a region claim,
        // whose bytes would otherwise be silently absorbed into a neighbouring claim and never
        // sent. A mismatch at upload time means "someone changed something and did not say where",
        // and the answer to that is always the whole image. A cache-backed layer arriving with a
        // generation this ledger has not seen therefore takes the FULL upload path, always -- the
        // app names no regions inside a re-rendered cache, and a missed region may cost time but
        // may never cost pixels.
        core::TileSet pending;
        bool pendingKnown = true;
        std::uint64_t pendingRevision = 0;
    };

    // One run of dirty macrotiles in a single macrotile ROW of a layer's own grid, in layer-local
    // pixels: one `VkBufferImageCopy`. Runs rather than tiles because the per-region cost is
    // constant while the byte count is not, and a brush dab's tiles are almost always adjacent.
    struct UploadRegion {
        std::uint32_t x = 0, y = 0, w = 0, h = 0;
    };

    // What one composite's uploads cost, threaded through ensureLayerResident so the per-layer
    // decision (partial / full / nothing) reaches TileCompositeStatus without a second walk.
    struct UploadTally {
        std::uint64_t bytes = 0;
        std::uint64_t regions = 0;
        std::uint64_t partial = 0;
        std::uint64_t full = 0;
    };

    // One planned layer step: everything the kernel's push block needs, resolved host-side.
    struct Step {
        core::LayerId layer = core::kInvalidLayerId;
        // SOURCE PIXEL -> document. For raster/magic that is the layer transform alone; for the
        // cache-backed kinds the cache's own `cacheImageToLayer()` folds in FRONT of it, exactly
        // as compositor.cpp composes `pre * layer.transform() * cacheImageToLayer()`. Everything
        // else on this Step -- `inverse`, `filter`, `superN`, `scaleX/Y`, `pad`, `bounds` and the
        // fingerprint -- is derived from THIS, so a cache whose origin moved is a placement change
        // and is diffed as one.
        common::Affine2D place = common::Affine2D::identity();
        common::Affine2D inverse = common::Affine2D::identity();
        common::Affine2D maskXform = common::Affine2D::identity();
        common::Rect bounds;                                    // doc-space AABB it can touch
        ResampleFilter filter = ResampleFilter::Nearest;         // ALREADY resolved
        std::int32_t superN = 2;
        core::BlendMode blend = core::BlendMode::Normal;
        float opacity = 1.0f;
        float scaleX = 1.0f, scaleY = 1.0f;
        // Which fold the kernel applies. For a LEAF these are composite_tile.comp's modes
        // (0 none / 1 proportional-in-source / 2 affine-in-source / 3 affine-in-target); for an
        // ADJUSTMENT step they are adjust_tile.comp's, which has only 0 (none) and 1 (the
        // clamped-domain stretch). The two kernels never share a step, so the two vocabularies
        // never meet in one value.
        std::int32_t maskMode = 0;
        bool clip = false;      // read the clip base
        bool clipWrite = false; // publish a new clip base

        // ---- ADJUSTMENT steps ---------------------------------------------------------------
        //
        // `adjust` picks the KERNEL: false runs composite_tile.comp over a resident source sheet,
        // true runs adjust_tile.comp over the accumulator the steps below it already built. Every
        // field from here down is read only when it is true, and `place` / `inverse` / `filter` /
        // `superN` / `scaleX` / `scaleY` are read only when it is false.
        bool adjust = false;
        std::int32_t adjustKind = 0;      // adjust_tile.comp's OWN dense enum, never core's
        std::int32_t adjustFlags = 0;     // curve-channel actives, preserve-luminosity, a choice
        float adjustParams[12] = {};      // the kind's scalars, in the kernel's lane order
        // DOC pixel -> MASK texel, as the (scale, origin) PAIR compositor.cpp's adjustmentMaskAt
        // multiplies out -- (sx, sy, originX, originY) -- and NOT a composed affine, because the
        // mirror has to keep the subtraction the CPU does before the scale.
        float maskMap[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        double pad = 0.0;       // the doc-space margin `bounds` was grown by (the filter footprint)
        // ⚠ THE PLAN DIFF KEY IS SPLIT IN THREE, and the split is what makes an incremental upload
        // possible at all. `fingerprint` hashes the PLACEMENT only (transform, opacity, blend,
        // filter, sub-samples, mask mode, clip role); the two revisions stay beside it as plain
        // numbers. Folding them in -- as the first cut did -- makes "the layer moved" and "one
        // 256 px block of the layer was repainted" the same event, and the only answer to that
        // event is to dirty the layer's WHOLE footprint. Keeping them apart lets diffPlanIntoDirty
        // recognise a content-only change and trust the region the caller named for it.
        // ZERO is reserved on `fingerprint`: markLayerDirty(id) poisons it to force a re-dirty.
        std::uint64_t fingerprint = 0;
        // The SOURCE revision `leafSourceFor` returned -- contentRevision() for the kinds whose
        // source is their own storage, cacheGeneration() for the two whose source is an
        // app-populated cache. Keying the diff on contentRevision() there would miss every cache
        // swap that does not move it (the draft bake, the crisp re-render), which is a stale
        // picture rather than a slow one.
        std::uint64_t sourceRevision = 0;
        std::uint64_t maskRevision = 0;
    };

    // Where one macrotile's pixels live inside the accumulator atlas.
    struct Slot {
        std::uint32_t image = 0;
        std::int32_t x = 0;
        std::int32_t y = 0;
    };

    // A contiguous run of dirty macrotiles sharing ONE accumulator atlas image -- the unit the
    // list shapes dispatch. Runs rather than a per-image scan because `TileSet::forEach` walks
    // row-major and `slotFor`'s image index is monotone in that order, so an image's tiles are
    // already adjacent and a run is found in one pass.
    struct AtlasRun {
        std::uint32_t image = 0;
        std::uint32_t first = 0;      // index into the composite's tile vector
        std::uint32_t count = 0;
        VkDeviceSize offset = 0;      // where this run's records start in m_tileList
        VkDeviceSize bytes = 0;
    };

    // ---- Setup -------------------------------------------------------------------------------
    [[nodiscard]] bool initPipeline(std::string& error);
    // The descriptor-indexed variant. Returns false with `m_indexedRefusal` set for any reason at
    // all -- an unsupported device, a budget, a driver that declined the pipeline -- and that is
    // never fatal: `create()` succeeds and the lane runs the floor shape.
    [[nodiscard]] bool initIndexedPipeline();
    [[nodiscard]] bool ensureIndexedPool(std::uint32_t sets, std::string& error);
    [[nodiscard]] bool initResolvePipeline(std::string& error);
    [[nodiscard]] bool ensureResolvePool(std::uint32_t sets, std::string& error);
    [[nodiscard]] bool initStaticImages(std::string& error);
    [[nodiscard]] bool ensureDescriptorPool(std::uint32_t sets, std::string& error);
    void destroyAtlas() noexcept;
    void destroySources() noexcept;
    [[nodiscard]] bool buildAtlas(std::string& error);

    // ---- Planning ----------------------------------------------------------------------------
    // `why`, when the refusal has something more specific to say than its enum name, carries it --
    // today that is the adjustment arm, which names the KIND and the pixel-level reason it cannot
    // be served exactly. Left untouched otherwise, and the caller falls back to `tileRefusalName`.
    [[nodiscard]] TileRefusal planDocument(const core::Document& doc, const CompositeOptions& opts,
                                           std::vector<Step>& out, std::string& why) const;
    // Dirty every macrotile the OLD and NEW plans disagree about. A layer that moved dirties the
    // union of where it was and where it is; an unchanged layer dirties nothing. This is item 7's
    // plumbing and it is what makes a brush dab cost one macrotile instead of the canvas.
    void diffPlanIntoDirty(const std::vector<Step>& next) noexcept;

    // ---- Residency ---------------------------------------------------------------------------
    // Takes the LAYER, not its pixels: which image a leaf composites from, where that image sits
    // in layer-local space and what makes it stale are all per-kind facts, and `leafSourceFor` is
    // the one place that answers them. A layer whose kind this lane does not serve refuses here
    // too -- planDocument has already rejected it, so reaching that arm means the tree changed
    // under the plan.
    [[nodiscard]] bool ensureLayerResident(const core::Layer& layer, VkCommandBuffer cmd,
                                           UploadTally& tally, TileRefusal& refusal,
                                           std::string& error);
    // Coalesce a layer's pending 64 px tiles into macrotile-aligned copy regions. False means
    // "do not do this partially" -- an empty set, more runs than kMaxUploadRegions, or regions
    // that already add up to the whole image -- and the caller's answer is the full upload, which
    // is always correct. Never a failure, always a choice.
    [[nodiscard]] bool planUploadRegions(const core::TileSet& pending, std::uint32_t imgW,
                                         std::uint32_t imgH,
                                         std::vector<UploadRegion>& out) const;
    // Record the copies for `regions` into `cmd`. The layer image must already exist and hold the
    // previous revision's pixels; see the layout note in the .cpp -- this is the one place where
    // an UNDEFINED oldLayout would be silently, intermittently wrong. 8-BIT SOURCES ONLY (the
    // caller gates on it): every offset and row stride here is in 4-byte texels, and only the
    // kinds that take region claims -- raster, through markLayerDirty(layer, rect) -- can reach it.
    [[nodiscard]] bool uploadLayerRegions(const common::Image& img, LayerSource& src,
                                          const std::vector<UploadRegion>& regions,
                                          VkCommandBuffer cmd, UploadTally& tally,
                                          std::string& error);
    void dropLayer(core::LayerId id) noexcept;
    void releaseEvicted(const std::vector<core::TileKey>& evicted) noexcept;
    // Forget every layer whose upload was RECORDED into this composite's command buffer but never
    // submitted. An abandoned command buffer means those copies never happened, so both the
    // device image's CONTENTS and its LAYOUT are whatever they were before -- and the cache says
    // otherwise. Dropping them is the only claim that stays true whatever the driver did.
    void discardRecordedUploads() noexcept;

    // ---- The dirty-macrotile list (item 10) --------------------------------------------------
    // Group `tiles` into per-atlas-image runs and record the copy that puts their geometry on the
    // device. The records are written in exactly the order the per-tile loop would have PUSHED
    // them, which is the whole parity argument: same integers, different courier.
    //
    // False means "do this composite with PerTile" -- an allocation declined, or a run longer than
    // the device's z-axis workgroup limit. Never a failure; the command buffer is untouched on
    // every false path, so the caller simply does not take the list branch.
    [[nodiscard]] bool buildTileList(const std::vector<std::pair<core::TileCoord, Slot>>& tiles,
                                     std::vector<AtlasRun>& runs, VkCommandBuffer cmd,
                                     std::string& error);

    // ---- Vulkan helpers ----------------------------------------------------------------------
    [[nodiscard]] bool makeImage(std::uint32_t w, std::uint32_t h, VkFormat fmt,
                                 VkImageUsageFlags usage, GpuImage& out, std::string& error) const;
    [[nodiscard]] bool makeHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& out,
                                      std::string& error) const;
    [[nodiscard]] bool makeDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& out,
                                        std::string& error) const;
    void destroyImage(GpuImage& img) const noexcept;
    void destroyBuffer(GpuBuffer& buf) const noexcept;
    [[nodiscard]] Slot slotFor(std::uint32_t mx, std::uint32_t my) const noexcept;

    std::shared_ptr<VulkanContext> m_ctx;
    // Device-time instrumentation; null when the device has no timestamp counter. Held by pointer
    // so an incomplete type suffices here -- ~TileCompositor is out of line in the .cpp, which is
    // where the deleter needs the definition.
    std::unique_ptr<GpuTimer> m_timer;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
    VkShaderModule m_shader = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    std::uint32_t m_descPoolSets = 0;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;

    // ---- Item 10: the two extra dispatch shapes ------------------------------------------------
    //
    // `m_listPipeline` is the SAME shader module as `m_pipeline`, specialized with kTileList = 1 --
    // one blob, one pipeline layout, one descriptor set layout, two pipelines. `m_indexed*` is the
    // separate variant blob and its own (single-set-per-atlas-image) layout, and every one of its
    // handles may legitimately be null: `m_indexedRefusal` says why.
    VkPipeline m_listPipeline = VK_NULL_HANDLE;
    // The ADJUSTMENT kernel, `shaders/adjust_tile.comp`. A second SHADER on the SAME descriptor set
    // layout and the SAME pipeline layout as the composite kernel -- an adjustment step binds the
    // same set (accumulator, mask, clip base, tile list) and pushes into the same range -- so the
    // dispatch loop's whole switch is which pipeline is bound. Two of them for the same reason the
    // composite kernel has two: one module specialized with kTileList = 0 and = 1.
    VkShaderModule m_adjustShader = VK_NULL_HANDLE;
    VkPipeline m_adjustPipeline = VK_NULL_HANDLE;      // per-tile geometry in push constants
    VkPipeline m_adjustListPipeline = VK_NULL_HANDLE;  // geometry in the dirty-macrotile list
    VkShaderModule m_indexedShader = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_indexedSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_indexedPipeLayout = VK_NULL_HANDLE;
    VkPipeline m_indexedPipeline = VK_NULL_HANDLE;
    // The adjustment kernel's descriptor-indexed twin. Built by the SAME gate: `initIndexedPipeline`
    // succeeds only when BOTH variants load, so a device that can run one can run the other and
    // `activeDispatch()` never has to ask which kernel a document happens to contain.
    VkShaderModule m_indexedAdjustShader = VK_NULL_HANDLE;
    VkPipeline m_indexedAdjustPipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_indexedPool = VK_NULL_HANDLE;
    std::uint32_t m_indexedPoolSets = 0;
    std::uint32_t m_indexedMaxSources = 0;  // descriptors in the runtime array: 2 per layer
    TileDispatch m_requested = TileDispatch::Auto;
    DispatchRefusal m_indexedRefusal = DispatchRefusal::NoDescriptorIndexing;
    // The dirty-macrotile list the list shapes read, device-local with a per-composite staging
    // copy. Device-local because every invocation reads its macrotile's record: a host-visible
    // buffer would make that an uncached read per workgroup, which is exactly the bus traffic this
    // class exists to remove.
    GpuBuffer m_tileList;

    // 1x1 stand-ins so every binding is always written, whatever the layer needs. A descriptor
    // set with a hole is undefined behaviour even for a binding the shader never reads.
    GpuImage m_dummyMask;    // R8_UNORM, 255
    GpuImage m_dummyClipRead;  // R16_SFLOAT sampled
    GpuImage m_dummyClipWrite; // R16_SFLOAT storage

    std::uint32_t m_docW = 0, m_docH = 0;
    std::uint32_t m_macrotile = 0;
    core::TileGrid m_macroGrid;
    core::TileSet m_dirty;        // on the 64 px grid, always
    std::vector<GpuImage> m_acc;  // the resident accumulator, macrotile slots packed into atlases
    std::vector<GpuImage> m_clip; // the clip base, same slot layout; built lazily
    std::uint32_t m_slotsX = 0, m_slotsY = 0, m_slotsPerImage = 0;
    std::uint64_t m_accBytes = 0;
    bool m_accValid = false;        // false until the first successful composite (CONTENT)
    bool m_accInitialised = false;  // false until the accumulator reached GENERAL (LAYOUT)
    bool m_clipInitialised = false; // ditto for the clip atlas, which is built lazily
    GpuBuffer m_zero;               // one macrotile of zeros: the dirty-slot clear source

    // The resolve pass (item 11): rgba16f accumulator slot -> the 8-bit present texture, on the
    // device. Its own pipeline because it runs once per dirty macrotile per FRAME, not per layer.
    VkDescriptorSetLayout m_resolveSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_resolvePipeLayout = VK_NULL_HANDLE;
    VkShaderModule m_resolveShader = VK_NULL_HANDLE;
    VkPipeline m_resolvePipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_resolvePool = VK_NULL_HANDLE;
    std::uint32_t m_resolvePoolSets = 0;
    // Macrotiles composited but not yet resolved. Separate from m_dirty because the two are
    // consumed by different callers at different cadences: a caller may composite twice before it
    // presents once, and the second composite must not lose the first one's tiles.
    core::TileSet m_unresolved;
    VkImage m_lastResolveImage = VK_NULL_HANDLE;  // identity check; a new target resolves in full
    std::uint32_t m_lastResolveW = 0, m_lastResolveH = 0;

    std::unordered_map<core::LayerId, LayerSource> m_sources;
    // Layers whose upload copies are in the command buffer this composite is recording. Emptied
    // once the submit's fence signals; see discardRecordedUploads() for the other outcome.
    std::vector<core::LayerId> m_uploading;
    TileResidency m_residency;
    std::vector<Step> m_lastPlan;
    const core::Document* m_lastDoc = nullptr;  // identity check; see reset()
    std::vector<GpuBuffer> m_staging;  // alive until the frame's fence signals
    std::uint64_t m_revision = 0;

    TileCompositeStats m_stats;
};

}  // namespace mosaic::render
