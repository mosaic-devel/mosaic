#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/geometry.hpp"  // common::Affine2D (the placement transforms below)
#include "common/image.hpp"
#include "core/layer.hpp"  // core::LayerId (the drag cache's key, S15-b)
#include "render/render.hpp"
#include "render/resample.hpp"  // the kernel bank + samplers (moved out of compositor.cpp, S53-a)

// The compositor turns a `core::Document`'s layer tree into a flat image (PLAN §3.7, §4). It
// walks each group bottom(0)->top, blending every layer onto an accumulated working buffer with
// its opacity, blend mode, optional mask, and clip-to-below; groups composite into their own
// buffer first (so their opacity/blend/mask apply as a unit, and adjustment layers inside a
// group scope to that group). Adjustment/filter layers modify the accumulated backdrop below
// them -- within their group, or globally downward at the root.
//
// S7-a is the CPU reference implementation: deterministic, GPU-free, and the golden reference
// the Vulkan compositor (S7-b) is checked against. It backs the headless harness and tests.
namespace mosaic::core {
class CompositeCommand;
class Document;
class GroupLayer;
class Layer;
class RasterLayer;
}

namespace mosaic::render {

// ---- Drag-cache admission (S15-b cache, budget corrected in S60-a) ---------------------------
//
// The live-drag cache holds `buffers` DOCUMENT-SIZED float buffers (belowAcc, the cached above
// rasters, and a group target's local composite). Admission is bounded two ways and the tighter
// one wins:
//
//   * a COUNT, which is the S15-b working-set spec, and
//   * a BYTE budget, because the count alone is not a memory bound. A buffer is w*h*16 bytes, so
//     the count was calibrated where it was written (~31.6 MiB at 1080p) and nowhere else: at
//     5000x8000 a buffer is 610 MiB and six is 3.6 GiB, which swaps or dies rather than caching.
//
// A byte cap ALONE would be its own regression -- the cache is still ~2.7x faster than the full
// walk at 5000x8000 (bench move-fullcanvas: 1755 ms vs 4672 ms), so refusing it there would make
// big drags slower, not safer. The pair leaves every size at or below 4K exactly as it was
// (6 x 126.6 MiB = 759 MiB at 3840x2160 still fits, so the count binds) while 5000x8000 caches
// belowAcc alone and declines the multi-buffer cases.
inline constexpr std::size_t kMaxCachedDragBuffers = 6;
inline constexpr std::uint64_t kMaxCachedDragBytes = 1024ull * 1024ull * 1024ull;

// Bytes one document-sized float buffer costs (ImageF: 4 channels x 4 bytes).
[[nodiscard]] constexpr std::uint64_t dragBufferBytes(std::uint32_t w, std::uint32_t h) noexcept {
    return static_cast<std::uint64_t>(w) * h * 4ull * sizeof(float);
}

// Does a drag cache of `buffers` document-sized buffers fit both budgets? Pure.
[[nodiscard]] constexpr bool dragCacheFits(std::size_t buffers, std::uint32_t w,
                                           std::uint32_t h) noexcept {
    return buffers <= kMaxCachedDragBuffers &&
           static_cast<std::uint64_t>(buffers) * dragBufferBytes(w, h) <= kMaxCachedDragBytes;
}


struct CompositeOptions {
    // Flatten the result over the transparency checkerboard (so transparent regions are
    // visible and the output is opaque). Off by default: the raw composite keeps real alpha.
    bool checkerboard = false;
    std::uint32_t checkerSize = 8;                  // checker square size, in pixels
    common::Color8 checkerLight{255, 255, 255, 255};
    common::Color8 checkerDark{205, 205, 205, 255};

    // How transformed layers are resampled (the Transform Anti-aliasing feature). Defaults to
    // Nearest -- the cheap, lossless legacy behaviour, so headless/test callers stay byte-stable;
    // the interactive app passes Auto (the Move tool's "Anti-aliasing" option) so rotate/scale and
    // sub-pixel placement get anti-aliased. `liveDrag` marks a transform gesture in flight: Auto
    // then drops to the cheap Bilinear kernel for the per-frame whole-document recomposite, and
    // resolves to the full-quality kernel again once the gesture commits.
    ResampleFilter resampleFilter = ResampleFilter::Nearest;
    bool liveDrag = false;

    // Composite the document WITHOUT this one layer, wherever it sits in the tree -- the read-only
    // way to say "everything below the layer I am dragging" or "the document this text layer
    // mirrors" (S60, docs/s60-gesture-start-stall.md finding G6). It replaces a
    // `setVisible(false) ... setVisible(wasVisible)` round trip through the DOCUMENT, which was an
    // uncommanded, unobserved mutation: safe only while the walk was synchronous and non-reentrant
    // on the UI thread, and a hard blocker for ever moving the walk off it.
    //
    // The skip happens at exactly the point the walk already drops an invisible layer, so for the
    // documents both callers have it is pixel-for-pixel the old path. It is deliberately NOT the
    // same thing as hiding the layer, and the difference is the point: hiding one layer changes
    // what OTHER layers do -- an enclosing group's contentBounds() shrinks (a larger local buffer
    // is never a different picture, only a bigger allocation), and a coverage-partition pair with a
    // hidden half retires (whereupon the surviving half stops having its alpha rewritten). Neither
    // is reachable from the two callers: canUseGpuDrag refuses any document with a live partition,
    // and a partition half is always a RasterLayer, never the TextLayer the reflect env skips.
    //
    // DragCompositeCache declines a skipping request outright (its cached below/above buffers are
    // built for the whole stack), so the caller falls back to the full walk.
    core::LayerId skipLayer = core::kInvalidLayerId;
};

// chooseAutoFilter / cubicKernel / kernelRadius / kernelWeight / resolveFilter and the whole-image
// samplers moved to render/resample.hpp in S53-a; this header keeps including it, so every existing
// `#include "render/compositor.hpp"` caller still sees them.

struct CompositeResult {
    bool ok = false;
    std::string error;
    common::Image image;                  // the composited RGBA image, at document resolution
    Backend usedBackend = Backend::Cpu;
    std::uint32_t validationErrors = 0;   // GPU validation errors (S7-b); always 0 on the CPU path
};

// Composite `doc` into an image. `backend` selects the implementation: `Auto` prefers the GPU
// compute path (S7-b) and falls back to the CPU reference; `Cpu` forces the (deterministic)
// reference; `Gpu`/`GpuCompute` require the GPU and error if it is unavailable.
[[nodiscard]] CompositeResult composite(const core::Document& doc, const CompositeOptions& opts = {},
                                        Backend backend = Backend::Auto);

// The dirty-region recomposite (S60-a). Composite ONLY the document-pixel rectangle `roi` into a
// roi-sized RGBA image (`res.image` is roi.w x roi.h, NOT document-sized). The full layer stack is
// still walked — blend modes, opacity, transforms, masks, clip-to-below and adjustments are all
// honoured — but every per-pixel loop is restricted to the ROI by compositing through a
// translation that maps the ROI's top-left onto the buffer origin, so the cost scales with the ROI
// area rather than the canvas. For leaf layers (the live brush / inpaint-preview case) the result
// is BYTE-IDENTICAL to the matching sub-rectangle of a full `composite()` — each leaf samples its
// own full source image, so the ROI translation only shifts integer output coordinates. (A
// transformed *group* whose local extent gets clipped to the ROI window can differ by the filter
// footprint at the ROI boundary; not a concern for the single-raster-layer interactive callers.)
// `roi` is floored to integer pixels and clamped to the canvas; returns ok=false if it is empty or
// does not intersect the canvas.
//
// `clampToCanvas` false lets the ROI reach OUTSIDE the canvas on any side: the region is only
// floored/ceiled to whole pixels, never clamped to [0,docW]x[0,docH], so the result covers the full
// requested rect and any part beyond the canvas edge stays transparent (no layer projects there) --
// the off-canvas surround the modal live-preview panes emulate (a layer/effect that spills past the
// canvas edge stays visible in the preview instead of being cropped). The walk is unchanged: each
// layer still composites wherever it projects into the (now off-canvas-inclusive) buffer window.
[[nodiscard]] CompositeResult compositeRegion(const core::Document& doc, const common::Rect& roi,
                                              const CompositeOptions& opts = {},
                                              Backend backend = Backend::Auto,
                                              bool clampToCanvas = true);

// The BOUNDED composite (S60, docs/s60-gesture-start-stall.md §3.2). The whole document, the whole
// layer stack, every blend/opacity/mask/clip/adjustment -- composited straight into an `outW` x
// `outH` buffer instead of a docW x docH one, by placing the walk through a uniform scale. The
// compositor's cost is a function of OUTPUT pixels (~61 ms/Mpx over four layers on eight cores),
// so this is the one lever that makes a whole-document composite cost what the consumer can
// actually use rather than what the canvas happens to be.
//
// Two consumers want exactly this and both used to pay for the full canvas and then throw it away:
// the Move gesture's `below` backdrop (a texture the drag shader samples in NORMALISED coordinates,
// so any resolution just works) and the 3D reflect environment (which composited 40 Mpx and
// hand-box-downsampled it to <= 768 px).
//
// Anti-aliasing comes for free and is the reason to composite AT the small size rather than
// downsample afterwards: each layer's kernel is resolved from its COMPOSED placement, so a 1/k
// scale is a minification and `Auto` resolves to Area -- a real box filter. It also avoids
// resizing straight-alpha RGBA after the fact, which bleeds colour out of transparent pixels.
//
// One honest limit: a GROUP still composites its subtree into a buffer sized in the group's own
// local pixels, so a document whose content hangs under groups shrinks its final buffer but not
// its intermediate ones. Correct, just less of a win -- the same property adjustmentPreview's
// scaled walk has always had.
//
// `outW`/`outH` are the exact output size (no aspect correction -- the caller derives them from the
// document's own aspect); an out-size equal to the document is the plain full composite.
[[nodiscard]] CompositeResult compositeScaled(const core::Document& doc, std::uint32_t outW,
                                              std::uint32_t outH, const CompositeOptions& opts = {},
                                              Backend backend = Backend::Auto);

// ---------------------------------------------------------------------------------------------
// The S33 blur GPU seam (docs/blur-filters.md §8; the setTextureRenderOverride precedent).
// applyAdjustment's spatial branch resolves a blur adjustment's schema params into a BlurOp --
// every geometric value already mapped into BUFFER px by the walk's placement -- and offers it
// to the registered override before running the CPU reference kernels. Return true = `img` was
// transformed under the CPU lane's exact semantics (same op -> the same picture within
// float-lane tolerance); false = fall back to the CPU lane (no device, unsupported op, device
// error). The CPU kernels stay the source of truth: the test binary never installs an override,
// so the byte-pinned goldens always exercise the CPU lane, and the GPU lane is held to it by
// tolerance-based parity tests (test_blur_gpu.cpp).
// ⚠ INVARIANT, and it binds the GPU lane exactly as it binds the CPU one: DofBlur renders by
// INTERPOLATING between independently pre-blurred pyramid levels -- every level blurred from the
// SOURCE, never level k from level k-1 -- and never by a per-pixel variable-radius gather. The
// gather is the obvious "exact" rewrite and it is deliberately refused; do not take it.
struct BlurOp {
    core::AdjustmentKind kind{};
    // The kernel's principal size in buffer px: Gaussian SIGMA, box/surface/lens/DoF RADIUS,
    // motion DISTANCE. Always > 0 (identity ops never reach the seam).
    float size = 0.0f;
    float angleRad = 0.0f;         // motion direction (0 = +x, toward +y)
    float amount = 0.0f;           // radial: spin arc DEGREES or zoom fraction, mode-resolved
    int mode = 0;                  // radial: RadialBlurMode; DoF: DofBokeh (as int)
    float cx = 0.0f, cy = 0.0f;    // radial center / DoF line point, buffer px
    float threshold = 0.0f;        // surface: range sigma in [0,1]
    int blades = 6;                // lens
    float curvature = 0.0f;        // lens, [0,1]
    float rotationRad = 0.0f;      // lens
    float boost = 0.0f;            // lens, [0,1]
    // ⚠ INVARIANT: the highlight boost is a SINGLE lower threshold applied as a global pre-pass on
    // pixel values -- never an upper+lower "light range" pair, never a per-tap gather weight.
    float boostThreshold = 0.0f;   // lens, [0,1]
    float band = 0.0f;             // DoF focus half-width, buffer px
    float feather = 1.0f;          // DoF ramp length, buffer px
    float dirX = 1.0f, dirY = 0.0f;  // DoF line direction, unit, buffer space
    bool draft = false;            // live gesture: tap subsampling allowed
};
using BlurRenderOverride = std::function<bool(common::ImageF& img, const BlurOp& op)>;
void setBlurRenderOverride(BlurRenderOverride fn);

// The S60-e layer-effects GPU seam (docs/layer-effects.md §8; the setBlurRenderOverride
// precedent). renderLayer offers the isolated straight-alpha layer buffer to the registered
// override before running applyEffects. Return true = `io` was transformed under the CPU lane's
// exact semantics; false = fall back to render::applyEffects (empty stack, a refused effect kind,
// no device, device error), and `io` is BYTE-UNTOUCHED then. The CPU lane stays the source of
// truth: the test binary never installs an override, so the byte-pinned goldens always exercise
// it, and the GPU lane is held to it by tests/test_layer_effects_gpu.cpp.
using LayerEffectsRenderOverride =
    std::function<bool(common::ImageF& io, const core::LayerEffects& fx, bool antialias,
                       const std::optional<common::Affine2D>& bufferToLayer)>;
void setLayerEffectsRenderOverride(LayerEffectsRenderOverride fn);

// Composite ONE group's subtree into a document-sized RGBA image (straight alpha; the group's
// own transform applied, its opacity/blend ignored — they style its compositing into the
// parent, not its content). Powers the layer panel's group thumbnails and the Shift-click
// "select the group's pixels" gesture. CPU path, deterministic.
// ---- Work counters -----------------------------------------------------------------------------
//
// Deterministic COUNTS of what a composite did, as opposed to how long it took, and the distinction
// is the entire point. A wall-clock budget is machine-dependent and flaky; "this walk rendered 57
// layers and cleared 2.3 gigatexels" is the same number on every machine, every build type and
// every thread count, so it can be asserted.
//
// They exist because the suite could not see cost AT ALL. Every defect the S60 arc found -- a
// canvas-sized convolution for a 300 px layer, a 34x34 thumbnail building a 36.7 MP buffer, a
// reduced composite rebuilding groups at full resolution, one file open compositing the canvas
// three times -- was CORRECT, passed 3040 test cases, and was found by opening a real document and
// watching a clock. Counting the work is what makes that class assertable
// (tests/test_composite_budget.cpp).
//
// ⚠ Incremented once per OPERATION, never per pixel: a relaxed atomic add per layer, against a
// walk that then touches millions of texels. That is why they are always on rather than gated --
// a counter you have to enable is a counter that is off when the regression lands.
struct WorkCounters {
    std::atomic<std::uint64_t> composites{0};        // composite() / compositeScaled() entries
    std::atomic<std::uint64_t> layerRenders{0};      // renderLayerRaw calls for a LEAF layer
    std::atomic<std::uint64_t> groupBuffers{0};      // group isolated buffers built
    std::atomic<std::uint64_t> groupBufferTexels{0}; // ...and their total area
    std::atomic<std::uint64_t> clearedTexels{0};     // texels zeroed into per-layer leaf buffers

    void reset() noexcept {
        composites.store(0, std::memory_order_relaxed);
        layerRenders.store(0, std::memory_order_relaxed);
        groupBuffers.store(0, std::memory_order_relaxed);
        groupBufferTexels.store(0, std::memory_order_relaxed);
        clearedTexels.store(0, std::memory_order_relaxed);
    }
};

// The process-wide counters. A test resets them, runs one composite, and reads them.
[[nodiscard]] WorkCounters& workCounters() noexcept;

[[nodiscard]] common::Image compositeGroup(const core::GroupLayer& group, std::uint32_t docW,
                                           std::uint32_t docH);

// The same subtree composite, rendered DIRECTLY into an outW x outH buffer through `docToOut`
// (document space -> that buffer). The walk rasterises every descendant at the output resolution,
// so the cost is bounded by outW x outH rather than by the canvas -- the property adjustmentPreview
// already relies on, offered to the other thumbnail arms.
//
// It is also BETTER FILTERED than compositing at canvas size and sampling down: each layer's kernel
// is resolved from its composed placement, so a reduction resolves Auto to a real box filter applied
// per layer before blending, instead of one point-sample taken afterwards.
[[nodiscard]] common::Image compositeGroupInto(const core::GroupLayer& group,
                                               const common::Affine2D& docToOut, std::uint32_t outW,
                                               std::uint32_t outH);

// An adjustment layer's dock-thumbnail preview (S32): the layers it AFFECTS — its parent group's
// children below it (globally downward at the root), i.e. the accumulated backdrop exactly as the
// compositor walk hands it to the adjustment — WITH the adjustment applied, composited into an
// outW x outH buffer framing the document window (docW x docH scaled down, so the preview is
// cheap at thumbnail size). The same walkStep semantics as the real composite: sibling
// adjustments below it apply too, clip-to-below and masks fold identically, and an invisible /
// zero-opacity adjustment previews the plain backdrop (the honest answer — the document shows no
// effect either). Straight alpha; empty where nothing composites. CPU path, deterministic.
[[nodiscard]] common::Image adjustmentPreview(const core::AdjustmentLayer& adj, std::uint32_t docW,
                                              std::uint32_t docH, std::uint32_t outW,
                                              std::uint32_t outH);

// The same scope composite WITHOUT the adjustment's own step: the raw backdrop it grades. Feeds
// the S32 editor's histogram (Levels / Threshold) — the distribution the handles slice is the
// input the math sees, so it deliberately excludes the adjustment's own effect.
[[nodiscard]] common::Image adjustmentBackdrop(const core::AdjustmentLayer& adj, std::uint32_t docW,
                                               std::uint32_t docH, std::uint32_t outW,
                                               std::uint32_t outH);

// Layer -> Rasterize: `layer` baked to a document-sized RGBA image in its PARENT'S coordinate
// space, ready to become a RasterLayer sitting at the same index with an identity transform. Works
// for every kind that has pixels of its own -- text (including 3D/warped), vector, magic, and a
// whole group's subtree.
//
// What is baked in: the layer's own transform, its mask, and its layer effects. What is NOT: its
// opacity, blend mode and visibility, which style how it composites INTO the parent and so belong
// on the raster that replaces it. Carry those four across and the document composites to exactly
// the same pixels afterwards -- the invariant tests/test_compositor.cpp pins.
//
// The buffer is the document's size in parent-local coordinates, so content that a parent-local
// transform pushes outside that window is CLIPPED. For a top-level layer that is just "rasterizing
// clips to the canvas", the same boundary Crop's "Delete Cropped" and Merge Down already accept.
//
// `filter` MUST be the filter the document is being composited with (CompositeOptions::resampleFilter
// -- the Move tool's "Anti-aliasing" option). It decides both how a transform resamples AND whether
// vector/pattern edges keep their analytic AA (Nearest hardens them), so baking with a different
// filter than the canvas is drawn with would silently change the picture the moment you rasterize.
// Never a live-drag filter: this is a commit, not a preview.
[[nodiscard]] common::Image rasterizeLayer(const core::Layer& layer, std::uint32_t docW,
                                           std::uint32_t docH,
                                           ResampleFilter filter = ResampleFilter::Auto);

// Layer→Merge Down's pixel math: `upper` baked onto `lower` in LOWER's pixel space — upper's
// content (mask folded in) sampled through lower.transform⁻¹ ∘ upper.transform, then blended
// with upper's mode/opacity (clip-to-below clips against lower's own alpha). The caller
// replaces lower's pixels with the result and removes upper, as one undo step. Baking into
// lower's raster means a non-identity lower transform resamples once more than the live
// composite did — same trade Photoshop makes. nullopt when upper has no pixels, either image
// is empty, or lower's transform is singular.
[[nodiscard]] std::optional<common::Image> mergeDown(const core::Layer& upper,
                                                     const core::RasterLayer& lower);

// ---- Merge Down across layer KINDS (S36) -----------------------------------------------------
//
// mergeDown() above stays the fast path for the pair it was written for -- raster/magic pixels
// baked into the RASTER below, in that raster's own pixel space, so the lower layer's transform
// never resamples and a live coverage partition still recombines disjointly. Every other pair
// routes through mergeDownBaked(), which bakes BOTH sides into the space they already share (their
// parent's) through the one rasterizer there is -- rasterizeLayer's contract: transform, mask and
// effects folded, opacity/blend/visibility/clip left over -- and then replays the compositor's own
// two walk steps over them. That is what lets Text / Vector (shape and gradient) / Texture / Magic
// / Group merge onto anything with pixels without a second rasterizer existing.
//
// The result is ready to become a RasterLayer with an IDENTITY transform, opacity 1, Normal blend
// and clip-to-below OFF, because all four have already been consumed here:
//   * the lower layer's opacity and blend mode are baked (its walk step ran against an empty
//     accumulator), and carrying them across would be wrong -- they would restyle the UPPER
//     layer's contribution too: over(acc, over(L,U)*k) is not over(over(acc, L*k), U);
//   * the upper layer's are baked by its own walk step.
// Two honesty gates are the CALLER's, because they are about the stack rather than the pair: the
// lower layer must not clip to the layer beneath it (the merged pixels would inherit that clip),
// and -- unless `emptyBackdrop` -- its blend mode must be Normal, or the merged pixels would be
// re-styled as one unit against a backdrop the original never showed them.
//
// `emptyBackdrop` says nothing composites BELOW the lower layer (it is the bottom child of the
// document root). Blending onto an empty backdrop is just the source, which is what lets a
// non-Normal blend mode bake at all. With a real backdrop the UPPER layer's blend mode can only be
// baked where the lower is fully opaque; anywhere else the live composite blends against pixels
// this bake cannot see, and the honest answer is to refuse rather than invent them -- the same
// standing limitation coverage partitions record (docs/document-model.md "Coverage partitions").
struct MergeDownBake {
    enum class Status {
        Ok,
        EmptyDocument,      // zero-sized canvas: nothing to bake into
        UpperBlendUnbaked,  // the upper's blend mode has no honest backdrop (see above)
    };
    Status status = Status::EmptyDocument;
    common::Image image;  // parent-space, document-sized; identity transform, opacity 1, Normal
};
[[nodiscard]] MergeDownBake mergeDownBaked(const core::Layer& upper, const core::Layer& lower,
                                           std::uint32_t docW, std::uint32_t docH,
                                           ResampleFilter filter, bool emptyBackdrop);

// Merge Down of an ADJUSTMENT layer: `img` graded by `adj` exactly as the compositor's walk grades
// the backdrop under it -- the adjustment's own opacity, layer mask and clip-to-below included
// (merged down, the "below" a clipping adjustment clips to IS this image, so the image's own alpha
// is that coverage). `imageToParent` places `img`'s pixel grid in the space the two layers share:
// the lower layer's transform when grading its own raster in place, the identity when grading a
// rasterizeLayer() bake. That placement is what puts the adjustment's document-window mask sheet
// over the right pixels and what converts a blur radius from parent px into the grid's own px.
// `docW`/`docH` frame the parent-space window the sheet spans.
//
// This grades ONLY the layer below, deliberately. An adjustment's live scope is the whole
// accumulated backdrop beneath it, so merging it down onto one layer genuinely changes the picture
// whenever there was more below -- that is what "merge the adjustment DOWN" asks for, and the
// caller says so out loud instead of pretending the merge was free.
[[nodiscard]] common::Image applyAdjustmentToImage(const core::AdjustmentLayer& adj,
                                                   const common::Image& img,
                                                   const common::Affine2D& imageToParent,
                                                   std::uint32_t docW, std::uint32_t docH);

// Merge Down of a SHAPE onto a SHAPE: the two objects combined into ONE vec::Object in `lower`'s
// local space (so the merged layer keeps lower's transform), or nullopt when they cannot be --
// and then the caller rasterizes both instead, which is never silently lossy.
//
// One object carries one fill, one stroke and one paint order; one layer carries one opacity, one
// blend mode, one clip flag, one mask and one effect stack. All of those must therefore already
// agree. The geometries are promoted to editable cubics (vec::pathFromGeometry) and their subpaths
// concatenated -- no boolean op, which is exactly why the two objects' STROKED bounds must be
// disjoint: separate contours in one path draw what two layers drew only where no pixel is reached
// by both, whatever the fill rule, the winding and the anti-aliased edges would otherwise make of
// an overlap. A gradient or pattern paint is evaluated in the object's own local space, and a
// stroke's width / dashes / miter are measured there, so those additionally pin how far `upper`
// may be rebased into `lower`'s space.
//
// Fidelity: a PARAMETRIC shape converts through the same exact-cubic promotion Layer->Convert to
// Path uses -- straight edges exactly, circular arcs within 3e-4 of the radius (see
// core/vector/to_path.hpp). Everything else here is bit-exact.
[[nodiscard]] std::optional<core::vec::Object> mergeDownVector(const core::VectorLayer& upper,
                                                               const core::VectorLayer& lower);

// The Crop tool's apply (S16): ONE undoable "Crop" step for the integer document-pixel rect
// `x,y,w,h`. The canvas resizes to `w`x`h` and the layer tree is rebased by the crop origin:
// leaf transforms take the (ancestor-conjugated) shift while unmasked groups keep their own
// transform — pushing the shift into a group rather than onto it, which keeps the rebase
// affine-only (no child resampling). Masked/singular groups rebase as a unit; their content can
// still be clipped to a canvas-aligned window (the compositor's remaining local-extent limit,
// owned by S60-a — unmasked groups now size their buffer to the visible content).
// `deletePixels` additionally bakes each unmasked raster whose ancestor chain is all-identity
// (top level, or inside untransformed groups) to the new canvas through the compositor's own
// sampler — the composite stays byte-exact, but the layer's live transform flattens to
// identity and the pixels outside the canvas are gone (the destructive trade the option
// names). Masked rasters, magic layers (whose point is keeping the source), and rasters under
// transformed ancestors keep their pixels and are only rebased. A non-empty selection is
// cropped along (deselected if the crop removes all of it). The command is returned
// UNAPPLIED, ready for CommandStack::push. Returns null only for a degenerate (zero-area)
// rect.
//
// S16-f expansion: the rect may reach beyond the old canvas on any side (x/y negative, or
// x+w / y+h past the old size) — the canvas grows and existing content is rebased inward.
// `fill` colours the ADDED area (only ever the part of the new canvas the old canvas did not
// cover — the old footprint's appearance cannot change): the standard Background case (bottom
// child of root: unmasked raster, identity transform, old-canvas-sized image) is extended in
// place — its buffer grows and old pixels move by an integer offset, byte-exact, no resample
// (in non-delete mode the buffer takes the UNION of old content and new canvas so no pixel is
// destroyed); any other stack shape instead gains a new bottom raster (named `fill->layerName`)
// covering only the expansion. No `fill` = the new area stays transparent. ⚠ INVARIANT: this
// engine never DERIVES fill content from the picture. What it writes is a plain constant colour,
// or pixels the caller already computed and handed in (`pixels` below) on an explicit Apply of a
// pre-chosen fill mode — never a content-aware fill this function decides to run by itself.
struct CropFill {
    common::Color8 color;
    std::string layerName; // history/panel name of the fallback bottom fill layer
    // When set (must be exactly new-canvas-sized), the expansion copies ITS pixels instead of
    // the constant colour — the Inpaint fill mode's healed ring, computed asynchronously by the
    // UI and landed through the very same one-undo-step command as the solid fills. Only the
    // expansion region is ever read from it.
    std::optional<common::Image> pixels;
};
// S16-f rotate: `angle`/`pivot` describe a ROTATED crop box — the rect x,y,w,h lives in the
// document plane rotated by `angle` radians about `pivot` (the canvas's crop frame). Apply
// rotates every layer by -angle about the pivot (through the same conjugated rebase; the
// compositor resamples transformed layers exactly as it does for the Move tool's rotation), so
// the box comes out axis-aligned in the new canvas. A rotated crop always creates fill
// portions (the corner wedges), which take `fill` like any expansion; the extend-in-place
// Background path is angle==0 only (rotation cannot be a lossless blit), so rotated fills go
// through the fallback bottom layer (non-delete) or the delete-mode bake. A non-empty
// selection is CLEARED by a rotated crop (its geometry does not survive the resample).
//
// S53-a: the guides ride along too (they used to be stranded at their old coordinates by every
// crop — a plain bug, fixed in the shared engine below so the Crop tool inherits the fix).
[[nodiscard]] std::unique_ptr<core::CompositeCommand>
buildCropCommand(core::Document& doc, long x, long y, std::uint32_t w, std::uint32_t h,
                 bool deletePixels, const std::optional<CropFill>& fill = std::nullopt,
                 double angle = 0.0, common::Vec2 pivot = {});

// The engine buildCropCommand is a wrapper over, and the one every render/document_ops.hpp
// operation (Canvas Size / Image Size / orientation / arbitrary rotation / Trim) is built on
// (S53-a). It re-frames the WHOLE document: the canvas becomes `newW` x `newH` and `worldToNew`
// maps a point of the old document plane to its place in the new one. Everything the crop
// contract above describes is this function's behaviour, generalised from "a translation" to "any
// invertible affine":
//   * every layer transform is rebased by `worldToNew` (conjugated into unmasked groups);
//   * `deletePixels` bakes the eligible rasters to the new canvas through `bakeFilter`;
//   * `fill` colours only the part of the new canvas the old canvas did not cover, taking the
//     lossless extend-in-place Background path when `worldToNew` is a whole-pixel translation;
//   * the guides are rebased (dropped when the remap turns them into slanted lines) and the
//     selection follows exactly as far as it honestly can — cropped for a translation, index-
//     remapped for a flip / 90-degree turn, box-resampled for a pure scale, and cleared for an
//     arbitrary rotation, which its pixel geometry does not survive.
// `label` names the single undo step. Returns null for a zero-area canvas or a singular remap.
[[nodiscard]] std::unique_ptr<core::CompositeCommand>
buildDocumentRemapCommand(core::Document& doc, std::uint32_t newW, std::uint32_t newH,
                          const common::Affine2D& worldToNew, bool deletePixels,
                          const std::optional<CropFill>& fill, std::string_view label,
                          ResampleFilter bakeFilter = ResampleFilter::Nearest);

// The drag-scoped composite cache (S15-b). During a Move drag only the dragged layer's
// transform changes per frame; this caches everything else — the composite of the root
// children below the target, the doc-space raster of each child above it, and the clip-base
// state in between — and per frame re-rasterises ONLY the moved layer before replaying the
// cached buffers in stack order. Bit-identical to composite(..., Backend::Cpu) for every
// blend mode, clip-to-below and adjustment layer (those all run live against the replayed
// accumulator); it backs the UI's deterministic CPU recomposite, nothing else.
//
// composite() returns nullopt when the fast path cannot apply — the target is not a TOP-LEVEL
// child of the root (nested targets stay with the full walk until S60-a's tiles), has no
// pixels, or the cache would exceed its buffer budget — and the caller falls back to the full
// composite(). A stale cache (size/structure mismatch) rebuilds transparently.
//
// The owner must invalidate() whenever anything OTHER than the dragged transform changes:
// drag end, undo/redo, panel edits, document switch (in the UI: whenever syncAfterEdit runs).
class DragCompositeCache {
public:
    [[nodiscard]] std::optional<common::Image> composite(const core::Document& doc,
                                                         core::LayerId target,
                                                         const CompositeOptions& opts = {});
    void invalidate() noexcept;

private:
    [[nodiscard]] bool matches(const core::Document& doc, core::LayerId target,
                               const CompositeOptions& opts) const noexcept;
    bool rebuild(const core::Document& doc, core::LayerId target, const CompositeOptions& opts);

    bool m_valid = false;
    // The resample settings the cache was built with. A live drag re-rasterises only the moved
    // layer per frame, but the cached below/above buffers must match what the full composite would
    // produce for the SAME options -- so a filter/liveDrag change invalidates the cache.
    ResampleFilter m_filter = ResampleFilter::Nearest;
    bool m_liveDrag = false;
    core::LayerId m_target = core::kInvalidLayerId;
    std::size_t m_targetIndex = 0;   // the target's index among the root's children
    std::size_t m_childCount = 0;    // root child count at build time (structure check)
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    common::ImageF m_belowAcc;       // children [0, targetIndex) composited (CPU walk)
    std::vector<float> m_clipBase;   // clip-base state entering the target's index
    bool m_haveClipBase = false;
    bool m_anyClips = false;
    bool m_targetIsGroup = false;
    common::ImageF m_groupLocal;     // a group target's local composite, mask folded
    long m_groupLocalOX = 0;         // m_groupLocal's origin in the group's local space (unmasked
    long m_groupLocalOY = 0;         // target: the buffer covers the group's content bounds)
    // renderLayer output per child above the target (aligned to targetIndex+1+k); nullopt =
    // an adjustment or a skipped (invisible) child, replayed live from the layer.
    std::vector<std::optional<common::ImageF>> m_above;
    // The replay's working buffers (the accumulator and the moved layer's raster), kept
    // across frames so each frame reuses their allocations — a fresh ~32 MiB buffer per
    // frame spends more time re-faulting pages than computing (the S15.y lesson).
    common::ImageF m_replayAcc;
    common::ImageF m_replaySrc;
};

// Whether the GPU-resident drag fast-path (canvas_drag_composite.comp, S60-a) applies to a live
// transform gesture on layer `target`: the document below it is static and only its affine changes,
// so the GPU can composite it over a cached "below" texture each frame instead of the CPU
// re-compositing + re-uploading the whole canvas. Requires `target` to be a TOP-LEVEL, TOPMOST
// child of the root (nothing composites above it), a visible unmasked non-clipping RasterLayer with
// a SEPARABLE blend mode (0..18). Anything else uses the CPU DragCompositeCache. Pure (no GPU) so
// it is unit-tested.
[[nodiscard]] bool canUseGpuDrag(const core::Document& doc, core::LayerId target);

// Build the deterministic demo document used by `mosaic --headless --composite-demo` and the
// compositor golden test: a few overlapping raster layers (with alpha, a Multiply and a Screen
// layer) under a global Invert adjustment, on a 64x64 canvas with a transparent border.
[[nodiscard]] std::unique_ptr<core::Document> makeCompositorDemo();

}  // namespace mosaic::render
