#pragma once

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "platform/native_window.hpp"
#include "render/gpu_budget.hpp"
#include "render/gpu_caps.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

// Opaque VMA handles (defined in vk_mem_alloc.h, used only in the .cpp). The canvas texture's
// device memory is allocated through VMA, like the rest of the renderer from S7 onward.
struct VmaAllocator_T;
struct VmaAllocation_T;

namespace mosaic::render {

class GpuTimer;
class VulkanContext;

// Integer destination rectangle for presenting the document, letterboxed on the canvas.
struct BlitRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
    bool operator==(const BlitRect&) const = default;
};

// Centered aspect-fit placement of a `srcW`x`srcH` image inside a `dstW`x`dstH` area (scaling up
// or down to fit). An empty source or destination yields a zero rect. Pure; unit-tested.
[[nodiscard]] BlitRect fitCentered(std::uint32_t srcW, std::uint32_t srcH, std::uint32_t dstW,
                                   std::uint32_t dstH);

// Marching-ants dash period in screen px (S13). The phase passed to setAntsPhase should be
// pre-wrapped to [0, period) in double precision so the shader's float mod stays exact over
// long sessions. Must match ANTS_PERIOD in shaders/canvas_present.comp.
inline constexpr double kAntsDashPeriodPx = 8.0;

// Max in-flight lasso polyline vertices the present-pass SSBO holds (binding 4). Large because the
// present shader bbox-culls pixels outside the path, so the cap barely affects cost; a big budget
// keeps the stable-stride re-pick (canvas) rare even on a very long freehand drag.
inline constexpr std::uint32_t kLassoMaxVerts = 4096;

// That lane draws ONE open polyline (segments between consecutive vertices), so a path of several
// contours needs an explicit break -- without one, the last point of one contour is chorded to the
// first of the next by a line that is not in the path. A vertex whose x is at or below this is that
// break: the run ends and the next real vertex opens a fresh one. The same out-of-range-sentinel
// idiom the present pass already uses for the Move anchor and the Type bend tab; the writer emits
// kPolylineBreakValue, readers test the threshold. Must match ui::kPolylineBreakX and
// canvas_present.comp's kPolyBreak (which is what makes lassoDist skip the chord).
inline constexpr double kPolylineBreakX = -1.0e8;
inline constexpr double kPolylineBreakValue = -1.0e9;

// Max selection-highlight quads (one per visual line) the Type-tool overlay SSBO holds (binding 5).
// A very long selection past this is clamped; far beyond any on-screen line count.
inline constexpr std::uint32_t kTextSelMaxRects = 1024;

// Max spell-check squiggle segments (one per misspelled span per visual line) packed into the same
// overlay SSBO after the selection quads (binding 5). Excess is clamped; a screen rarely shows this
// many misspelled words. Kept modest because the present shader loops all of them per covered pixel.
inline constexpr std::uint32_t kSpellSquiggleMaxSegs = 256;

// Max Smart Resize keep-region chips the chip overlay SSBO holds (binding 8, S16-f). The extractor
// caps regions well below this (8 + faces); excess is clamped.
inline constexpr std::uint32_t kKeepChipMaxRects = 16;

// Max ruler-guide + smart-guide line segments the guide overlay SSBO holds (binding 13). Document
// guides are few; the Move tool's smart guides add a handful more during a drag. Excess is clamped;
// the present shader loops all of them per covered pixel, so it is kept modest.
inline constexpr std::uint32_t kGuideLineMax = 64;

// Max Pen-tool chrome knobs (anchors + handle tips) and stems the pen overlay SSBO holds
// (binding 6, S28). Sized so a path of ~170 nodes draws EVERY node's anchor and both its handles:
// the chrome used to borrow 40 of the 64 guide-lane entries, which silently dropped a path's marks
// past ~17 nodes and starved the document's own guides at the same time. Excess is clamped, and the
// builder emits the selected node first and every anchor before any handle, so a clamp degrades
// from "all the handles" down to "all the anchors" instead of losing the node under your cursor.
inline constexpr std::uint32_t kPenMarkMax = 512;
inline constexpr std::uint32_t kPenStemMax = 512;

// Max cells of the brush reticle's tip-outline SDF (binding 9, S19 §6.3). Must hold
// core::brush::kTipSdfMaxCells -- the padded grid of a tip built at the core's own resolution. A
// field larger than this is REFUSED (the reticle falls back to its analytic ellipse), never clamped:
// half a tip's outline is worse than an honest oval.
inline constexpr std::uint32_t kTipSdfMaxCells = 140 * 140;

// ---- Admission for the PRESENTING device's own textures (S60-a) -------------------------------
//
// S60-alpha item 4 gated the three compute lanes and stopped there. The textures the presenting
// device allocates for ITSELF -- the canvas texture, and the Move gesture's `below`/`dragged` pair
// -- were never part of that sweep, and they are the largest single allocations in the app: at
// 5000x8000 each is 160 MiB of device image plus a host-visible staging buffer of the same size
// (docs/s60-gesture-start-stall.md finding G5). Two things were missing, and both are here: the
// question is answerable BEFORE any CPU work is paid, and a refusal carries a NAME.
//
// Named rather than a bare bool, in the shape `render::TileRefusal` already uses, because "no" is
// unactionable in a log and these reasons have different answers. Over the device's image limit is
// permanent and belongs to the hardware; over the memory budget is about right now and may pass on
// the next gesture; an empty source is not really a refusal at all.
enum class TextureRefusal : std::uint8_t {
    // ⚠ NOT `None`: this header is reached from translation units that pull in X11's `X.h`, where
    // `None` is a macro (`#define None 0L`) and an enumerator by that name stops being an
    // identifier at all. `Admitted` also reads better at the call sites, which ask "was it let in?".
    Admitted = 0,
    NoDevice,         // the renderer has no device (nothing can be hosted anywhere)
    EmptySource,      // a zero-sized image: nothing to host
    OverMaxImageDim,  // past maxImageDimension2D -- Vulkan 1.0 guarantees only 4096 px
    OverMemoryBudget, // the images plus their staging would not fit the device's headroom
};
[[nodiscard]] std::string_view textureRefusalName(TextureRefusal r) noexcept;

// Bytes one presenting-device texture of `w`x`h` occupies. These are R8G8B8A8_UNORM -- they mirror
// common::Image and feed the present pass -- NOT `GpuCaps::workingFormat`, so do not reach for
// workingFormatBytes() here: that reports the rgba16f compute lanes' cost, which is twice this.
[[nodiscard]] constexpr std::uint64_t presentTextureBytes(std::uint32_t w,
                                                          std::uint32_t h) noexcept {
    return static_cast<std::uint64_t>(w) * h * 4ull;
}

// What ONE Move gesture asks of device memory. The staging buffer is host-visible rather than
// device-local on a discrete part, and it is still charged here: on a unified or resizable-BAR
// device it comes out of the very same heap, and a budget that is right on one class of device and
// optimistic on the other is not a budget.
struct DragTextureCost {
    std::uint64_t belowBytes = 0;
    std::uint64_t draggedBytes = 0;
    // The transient PEAK, not the sum of two: uploadSampledTexture allocates its staging buffer,
    // blocks on the copy and frees it before the second texture is started, so only one staging
    // buffer is ever alive -- the larger of the two.
    std::uint64_t stagingBytes = 0;

    [[nodiscard]] constexpr std::uint64_t peakBytes() const noexcept {
        return belowBytes + draggedBytes + stagingBytes;
    }
};
[[nodiscard]] constexpr DragTextureCost dragTextureCost(std::uint32_t belowW, std::uint32_t belowH,
                                                        std::uint32_t dragW,
                                                        std::uint32_t dragH) noexcept {
    DragTextureCost c;
    c.belowBytes = presentTextureBytes(belowW, belowH);
    c.draggedBytes = presentTextureBytes(dragW, dragH);
    c.stagingBytes = c.belowBytes > c.draggedBytes ? c.belowBytes : c.draggedBytes;
    return c;
}

// The drag pair's share of what is FREE on the device, and the ceiling on that share. Both are
// handed to `atlasBudgetBytes` -- there is ONE budget policy in this tree and this is not allowed
// to become a second one -- with a single deliberate departure: `minBytes` is ZERO here, where the
// tile atlas floors itself at 32 MiB. The atlas floors because an atlas that can hold nothing
// thrashes forever; the drag lane has a CORRECT fallback in the CPU drag cache, so on a squeezed
// device the honest answer is "nothing fits", and a floor would hand it a budget the device cannot
// actually honour. A larger share than the atlas's because the pair is transient (freed at gesture
// end) rather than the persistent working set.
inline constexpr double kDragTextureHeadroom = 0.50;
inline constexpr std::uint64_t kDragTextureHardCapBytes = 1024ull << 20;

// Can the presenting device host the canvas texture for a `w`x`h` document?
//
// DIMENSIONS ONLY, and the asymmetry with the drag pair below is the whole point. The canvas
// texture IS the present path -- there is no second way to show a document -- so a refusal here
// shows the user an empty canvas. A byte budget that guessed wrong would therefore blank the window
// on exactly the devices least able to spare the memory, so the byte question stays the driver's
// and a failing vmaCreateImage is handled where it always was. What IS worth refusing up front is
// the case no driver can serve: 5000 > 4096, the Vulkan 1.0 guaranteed maxImageDimension2D, which
// MOSAIC_GPU_PROFILE=floor reproduces on any device. Pure; unit-tested.
[[nodiscard]] TextureRefusal admitCanvasTexture(const GpuCaps& caps, std::uint32_t w,
                                                std::uint32_t h) noexcept;

// ... and the Move gesture's texture pair? ASK BEFORE building the below-composite: that composite
// is a full CPU walk -- seconds on a large document -- and a refusal afterwards means the whole
// bill was paid for a lane that was never going to run.
//
// `mem` is a snapshot from `queryGpuMemory`. A snapshot nobody took (`budget == 0`) ABSTAINS rather
// than refusing: declining a lane on the strength of a number that was never measured is worse than
// letting the allocation answer for itself.
//
// The lesson `TileResidency` paid for applies here, in the one form this code can express it. That
// class refuses rather than evicting a tile the dispatch in flight still reads, because an LRU that
// always makes room draws the WRONG PICTURE rather than merely thrashing. Here the canvas texture is
// the pinned resident -- the present pass samples it every frame and nothing may free it to make
// room for a gesture -- so the drag pair is weighed against what is free BESIDE it, never against a
// total that counts it as reclaimable. Pure; unit-tested.
[[nodiscard]] TextureRefusal admitDragTextures(const GpuCaps& caps, const GpuMemoryBudget& mem,
                                               std::uint32_t belowW, std::uint32_t belowH,
                                               std::uint32_t dragW, std::uint32_t dragH) noexcept;

// A persistent, surface-backed Vulkan renderer that owns a swapchain and presents to a
// platform window. Unlike the headless VulkanContext (one-shot, offscreen, recreated per
// call), this keeps the instance/device alive for the window's lifetime and drives
// per-frame acquire/clear/present.
//
// S3 scope: it clears the swapchain to a solid color each frame -- the proof that Vulkan is
// driving the FLTK canvas. The layer-tree compositor (render passes, blending) lands in S7.
class WindowRenderer {
public:
    // Builds the instance (with the surface extension matching `handle.system`), surface,
    // device (with VK_KHR_swapchain) and the initial swapchain. Returns nullptr / sets
    // `error` if Vulkan or a presentable device is unavailable.
    static std::unique_ptr<WindowRenderer> create(const platform::NativeSurfaceHandle& handle,
                                                  bool enableValidation, std::string& error);
    ~WindowRenderer();

    WindowRenderer(const WindowRenderer&) = delete;
    WindowRenderer& operator=(const WindowRenderer&) = delete;
    WindowRenderer(WindowRenderer&&) = delete;
    WindowRenderer& operator=(WindowRenderer&&) = delete;

    // Acquire/clear/present one frame. Recreates the swapchain transparently on resize or
    // out-of-date. Returns false / sets `error` only on an unrecoverable error; a zero-size
    // (minimized) surface is a successful no-op. `clearColor` fills the canvas background (the
    // letterbox area around the document, if any).
    bool drawFrame(common::Color8 clearColor, std::string& error);

    // Set the document composite to display on the canvas. The pixels are copied and uploaded
    // to the GPU on the next frame; pass an empty image to show nothing but the background.
    // Call when the document's composite changes.
    void setCanvasImage(const common::Image& img);

    // Patch only the sub-rectangle `sub` (placed at document px (x,y)) of the existing canvas
    // texture on the next frame -- the dirty-region upload (S60-a) for live brush strokes and
    // inpaint previews, avoiding a whole-document re-upload. No-op if no texture exists yet or the
    // rect overflows it; the caller falls back to setCanvasImage in that case.
    void setCanvasRegion(const common::Image& sub, std::uint32_t x, std::uint32_t y);

    // GPU-resident Move/Resize/Rotate drag (S60-a). During a live transform gesture on a top-level
    // layer the document is static except the dragged layer's affine; instead of re-compositing +
    // re-uploading the whole canvas on the CPU each frame, a compute pass composites `below` (the
    // static composite under the dragged layer) + the dragged layer's source pixels (sampled
    // through its live transform) into the canvas texture on the GPU.
    //  - beginGpuDrag uploads below + dragged ONCE (per gesture) and arms the pass.
    //  - setGpuDragTransform updates the dragged layer's doc->layer-local transform + blend each
    //    frame (cheap -- no upload).
    //  - endGpuDrag disarms it and frees the textures; the caller then lands the committed pixels
    //    with a normal setCanvasImage. Only the fast-path callers (render::canUseGpuDrag) use this.
    void beginGpuDrag(const common::Image& below, const common::Image& dragged);
    // Can this device host drag textures of these sizes? Ask BEFORE building the below-composite:
    // that composite costs seconds on a large document, and beginGpuDrag refusing afterwards
    // means the whole bill was paid for nothing. Vulkan 1.0 guarantees only 4096 px, so a
    // 5000x8000 document genuinely does not fit on a floor device (S60-alpha caps gate; missed in
    // the first gate pass, which covered only the compute lanes).
    [[nodiscard]] bool canHostDragTextures(std::uint32_t belowW, std::uint32_t belowH,
                                           std::uint32_t dragW, std::uint32_t dragH) const noexcept;
    // The same question answered by NAME, against this device's caps and its latest memory
    // snapshot. `canHostDragTextures` is this reduced to a bool for the call sites that only
    // branch; a caller that reports, logs or tests wants the reason.
    [[nodiscard]] TextureRefusal dragAdmission(std::uint32_t belowW, std::uint32_t belowH,
                                               std::uint32_t dragW,
                                               std::uint32_t dragH) const noexcept;
    // The device-memory snapshot the drag admission is decided against (see refreshMemoryBudget:
    // it is taken when the canvas texture is allocated, not per frame). Diagnostics and tests.
    [[nodiscard]] const GpuMemoryBudget& memoryBudget() const noexcept { return m_memory; }
    void setGpuDragTransform(const common::Affine2D& docToLayerLocal, int blendMode, float opacity);
    void endGpuDrag();
    [[nodiscard]] bool gpuDragActive() const noexcept { return m_dragActive; }

    // The documentless idle pass: an ambient ripple dot field + the open-an-image invitation,
    // rendered by canvas_idle.comp. With no document it REPLACES the bare background clear
    // (standalone mode); while a document arrives it runs after the present pass and blends the
    // settling field over it (the fade-out crossfade). The canvas owns the animation clock and
    // fade state machine and pushes this every frame; `active` false skips the pass entirely,
    // so a session with a document open never pays for it.
    struct IdleField {
        bool active = false;   // any visible contribution this frame (field or invitation)
        float fade = 0.0f;     // field presence 0..1 -- scales amplitude AND alpha (the settle)
        float hot = 0.0f;      // drag-over bloom 0..1 (also the hot atlas row's crossfade)
        float hover = 0.0f;    // pointer-hover atlas row crossfade 0..1
        float invAlpha = 0.0f; // invitation quad opacity (its own fade timeline)
        float timePhase = 0.0f; // seconds * field speed, pre-wrapped in double on the CPU
        float pitch = 32.0f;    // dot lattice pitch, device px
        float amp = 0.7f;       // field amplitude at rest (the chosen temperament)
        float quietPad = 0.0f;  // quiet-zone margin beyond the invitation quad, device px
        float ink[3] = {0, 0, 0};    // dot ink (theme-derived, pushed by the canvas)
        float accent[3] = {0, 0, 0}; // crest/bloom accent
    };
    void setIdleField(const IdleField& f) noexcept { m_idleField = f; }

    // Upload the invitation atlas: `rows` equal-height rows stacked vertically (idle / hover /
    // drag-hot). Synchronous one-shot upload (it changes only on theme / DPI / copy changes);
    // the previous atlas is retired through the frame fence like the drag textures. An empty
    // image drops the atlas (the field then renders without an invitation).
    void setIdleAtlas(const common::Image& atlas, std::uint32_t rows);

    // Set the document->screen view transform used to present the canvas (S8: pan/zoom/rotate).
    // `docToScreenLogical` maps document px to the canvas widget's logical px; `contentScale`
    // converts logical to physical (HiDPI) px. Called each frame by the canvas; the renderer
    // inverts it for the present compute shader. Until set, the document is shown aspect-fit.
    void setView(const common::Affine2D& docToScreenLogical, double contentScale) noexcept;

    // Show/update the rotation degree-readout dial (S8-b), drawn over the canvas while the view is
    // being rotated. `active` toggles it; `angleRadians` drives the needle. The numeric readout is
    // supplied as a rasterized tile via setOverlayTile (the canvas formats + renders it with the
    // real UI font; the present pass just composites the texture). Pushed each frame by the canvas.
    void setRotationOverlay(bool active, double angleRadians) noexcept;

    // Set the document's selection coverage mask (8-bit, document-sized) for the marching-ants
    // marquee (S13). The pixels are copied and uploaded on the next frame, like the canvas image.
    // Pass null / zero size for "no selection" (the marquee disappears). Call on selection
    // changes, not per frame.
    void setSelectionMask(std::uint32_t w, std::uint32_t h, const std::uint8_t* coverage);

    // The in-flight lasso / polygonal-lasso path: an OPEN polyline drawn by the present pass as a
    // smooth inverted line (replacing the doc-pixel marching-ants preview, which staircased). `pts`
    // are the path vertices in the canvas widget's *logical* px (TL origin); the renderer applies the
    // content scale, like setTransformHandles. Empty / <2 points = inactive (no line). Pushed each
    // frame by the canvas during a lasso gesture; uploaded into the SSBO next frame, like the mask.
    void setLassoPolyline(const std::vector<common::Vec2>& pts);

    // The Type tool's caret + selection overlay (S29-b, binding 5): the blinking insertion bar
    // (`caretA`→`caretB`) and the per-visual-line selection highlight `selQuads` (each a rotated rect
    // TL,TR,BR,BL). All in the canvas widget's *logical* px; the renderer scales by the content scale,
    // like the lasso path. `caretActive` false hides the bar (the canvas drives the blink); an empty
    // `selQuads` hides the highlight. Pushed each frame by the canvas during an edit session.
    // `handleCount`: the final N entries of `selQuads` are Type-edit box handles (the resize handle and,
    // for a horizontal block, the bend handle), each drawn SOLID in the box colour (not the translucent
    // selection wash) -- see the present shader.
    void setTextOverlay(bool caretActive, common::Vec2 caretA, common::Vec2 caretB,
                        const std::vector<std::array<common::Vec2, 4>>& selQuads,
                        int handleCount = 0);

    // The Type baseline BEND drop-tab handle (S30 §9): a rounded pill (box-blue, with a ↕ glyph) on a
    // short stem, drawn by the present pass. `pill` is the pill centre, `apex` the bar apex the stem
    // reaches up to, both in the canvas widget's *logical* px (the renderer applies the content scale).
    // `active` false hides it. Pushed each frame during a horizontal, non-3D edit session.
    void setTextBendHandle(bool active, common::Vec2 pill, common::Vec2 apex) noexcept {
        m_textBendActive = active;
        m_textBendPill = pill;
        m_textBendApex = apex;
    }

    // Spell-check squiggles (deferred §2): a red wavy underline under each misspelled span. Each
    // `segment` is the underline baseline {A, B} in the canvas widget's *logical* px (the renderer
    // applies the content scale, like setTextOverlay); the present pass draws a fixed-thickness sine
    // wave along it. The segments ride the Type overlay SSBO (binding 5) after the selection quads, so
    // no new GPU channel is grown. Empty = no squiggles. Pushed each frame during an edit session.
    void setSpellSquiggles(const std::vector<std::array<common::Vec2, 2>>& segments);

    // The brush-family reticle (S19-a): a size ring drawn by the present pass at the cursor, the GPU
    // sibling of the lasso line (it rides the same overlay SSBO, so no push lane is grown).
    //
    // It traces the TIP'S SHAPE (docs/brushes.md §6.3), so it is an ELLIPSE, not a circle: `semiX` and
    // `semiY` are the tip's two semi-axes and `angleRad` its rotation, all already in the canvas
    // widget's *logical* px / screen frame (the canvas folds in the zoom and the view's own rotation;
    // the renderer applies only the content scale, like the lasso path). A round tip passes semiX ==
    // semiY and angle 0, which the shader reduces to exactly the circle it drew before tips had
    // shapes. `locked` draws a padlock glyph in the centre (the active layer can't be painted).
    // `active` false hides it. Reused by the eraser/heal/inpaint brushes.
    void setBrushReticle(bool active, common::Vec2 center, double semiX, double semiY,
                         double angleRad, bool locked) noexcept;

    // The reticle's TIP OUTLINE (S19 §6.3, binding 9): the signed distance field of the tip's own
    // silhouette (core::brush::buildTipSdf), which the shader samples INSTEAD of its analytic ellipse
    // -- because an ellipse over a bristle, a spatter or a spiked tip is a lie about where paint will
    // land, and that is the reticle's one job.
    //
    // `key` identifies the field: the tip's raster id and its frame, and nothing else. The field lives
    // in the TIP'S OWN frame, so the diameter, the zoom, the tip's angle and the cursor's position are
    // all applied when it is SAMPLED -- none of them may re-upload it. Pass the same key and the
    // payload is not even looked at; pass key == 0 (or an oversized field) and the reticle goes back
    // to the analytic ellipse, which is exactly what a round tip wants.
    //
    // `w` x `h` are the field's grid cells (padded by one background cell all round) and
    // `boxW`/`boxH` the tip's true extent in the grid's own build px -- the two are NOT the same
    // number, and assuming they were would place a bitmap tip's outline slightly off its own box.
    void setBrushReticleSdf(std::uint64_t key, int w, int h, int pad, double boxW, double boxH,
                            const float* data, std::size_t count);

    // Whether the reticle READS that field this frame -- the per-frame half of the decision, kept out
    // of the key above so that a zoom which shrinks the brush past legibility does not count as a
    // different tip and rebuild it. False falls back to the analytic ellipse, which is all a brush a
    // few pixels across can usefully show anyway.
    void setBrushReticleTracing(bool on) noexcept { m_reticleTracing = on; }

    // The eyedropper's loupe (S24, binding 10): a circular magnifier centred on the cursor, drawn by
    // the present pass. It samples the on-screen composite (uDoc) at `magnification` screen px per
    // document texel, with a pixel grid, the sampled centre cell outlined box-blue, and a
    // colour-comparison ring: the TOP arc shows `sampleColor` (what a click would pick; neutral when
    // `readout` is false -- nothing to pick), the BOTTOM arc `prevColor` (the swatch that pick would
    // replace). When `readout`, the hex/RGB tile set via setOverlayTile composites by the ring.
    // `center` is the cursor in the canvas widget's *logical* px; `radius` and `magnification` are
    // logical px too (the renderer applies the content scale, like the reticle).
    // `sampleDocTexelCenter` is the sampled texel's CENTRE in document px (floor(cursorDoc) + 0.5).
    // `active` false hides it. Pushed each frame while the eyedropper samples over the canvas.
    void setLoupe(bool active, common::Vec2 center, double radius, double magnification,
                  common::Vec2 sampleDocTexelCenter, common::Color8 sampleColor,
                  common::Color8 prevColor, bool readout) noexcept;

    // Settings -> Appearance "Selection and reticle line": how the present shader colours the shared
    // content-keyed overlay line (the in-flight lasso, the brush reticle, the Type frames/baseline
    // riding the lasso channel, and the caret). 0 = Classic (hard per-pixel luminance key),
    // 1 = Rim on demand, 2 = Adaptive (see the shader's styledLine). Written into the lasso/reticle
    // SSBO header each frame; applies to whatever chrome the next frame draws.
    void setOverlayLineStyle(int style) noexcept { m_overlayLineStyle = style; }

    // Settings->Appearance "Feathered selection indicator" (`featherStyle` in the present shader):
    // how a soft-edged selection is shown. 0 = Bracketing ant pair (A, the DEFAULT: ants at the
    // ~15% and ~85% coverage contours, the gap = the feather width); 1 = True-edge ant + soft band
    // (F: the crisp 50% ant plus a faint falloff tint). Written into the lasso/reticle SSBO header
    // each frame; applies to whatever selection the next frame draws.
    void setFeatherIndicator(int style) noexcept { m_featherIndicator = style; }

    // The Channels tab's on-canvas channel ISOLATION (S60-a item 10, `channelView` in the present
    // shader). `view` is an OPAQUE code the shader decodes -- ui::channelViewShaderCode builds it
    // from the tab's ChannelViewMask, and that header owns the encoding, because the meaning of the
    // bits is a UI concept the renderer has no business knowing. What the renderer DOES guarantee
    // is that **0 is normal and is the identity**: it is this member's initial value and the
    // shader's early-out, so a renderer nobody ever tells about a channel view presents exactly
    // what it always did.
    //
    // The isolation is DISPLAY-ONLY and is applied where the present pass samples the canvas
    // texture -- so the texture keeps holding the true composite, and the remap works the same
    // whether the resident tile resolve or a host upload wrote those pixels. It used to be a CPU
    // pass over a copy of the whole composite on its way to the texture, which is what forced the
    // device-resident lane back onto the CPU walk whenever an eye was closed.
    //
    // Written into the lasso/reticle SSBO header each frame (that header's last std430 pad, byte
    // 60); the push block has been at its 128-byte guaranteed budget since S15 and does not grow.
    void setChannelView(std::uint32_t view) noexcept { m_channelView = view; }

    // The Move tool's transform controls (S15): the selected layer's quad outline + 8 square
    // handles, composited by the present pass (FLTK can't draw over the Vulkan surface — the
    // S8-b dial's path). `corners` are the quad's TL,TR,BR,BL in the canvas widget's *logical*
    // px; the renderer applies the content scale. Pushed each frame by the canvas.
    // `rotateDotAlpha` (0 = off, ceiling ~0.5) fades in the faint rotate-hotspot dots at the
    // corners — the canvas derives it from the quad's wackiness (ui::transformQuadWackiness).
    void setTransformHandles(bool active, const std::array<common::Vec2, 4>& corners,
                             float rotateDotAlpha = 0.0f) noexcept {
        m_handlesActive = active;
        m_handleCorners = corners;
        m_rotateDotAlpha = rotateDotAlpha;
    }

    // The Move tool's transform ANCHOR / reference point (S15+): the pivot rotation AND scaling turn
    // around, which the user can drag off the box centre (Photoshop-style). Drawn by the present pass
    // as a crosshair-in-a-circle at `posLogical` (the canvas widget's *logical* px; the renderer
    // applies the content scale, like the handle corners). It rides the lasso/reticle overlay header
    // (like the rotate-dot alpha), so no push lane grows; only drawn when the transform box is up.
    // `active` false hides it. Pushed each frame by the canvas alongside setTransformHandles.
    void setTransformAnchor(bool active, common::Vec2 posLogical) noexcept {
        m_anchorActive = active;
        m_anchorPos = posLogical;
    }

    // The Type box's rotate-hotspot dots ALONE (controls mode 6): the Type tool draws its own frame
    // chrome, but its rotate band — the edit box corners for flat text, the projected solid extent
    // for 3D — is invisible, and for 3D it can sit far off the visible letters. Same corner/alpha
    // conventions as setTransformHandles; lowest claim on the controls quad lanes (anything else
    // active wins, and those tools are never live during a type session anyway).
    void setTextRotateDots(bool active, const std::array<common::Vec2, 4>& corners,
                           float alpha) noexcept {
        m_textDotsActive = active;
        m_textDotCorners = corners;
        m_textDotAlpha = alpha;
    }

    // The Line shape's gizmo (S26): NOT a bounding box -- a connector line between `a` and `b`, a
    // square handle at each end, and a round handle at `mid` (the bend handle). Shares the controls
    // quad lanes (packed a,b,mid into the corner slots); claims them over the box when active (a line
    // is never shown with the box). `a`/`b`/`mid` are in the canvas widget's *logical* px.
    // `mid2` is a SECOND round handle (the Gradient tool's elliptical minor-axis edge, S22); pass
    // `mid` for a gizmo that has none -- the shader reads an identical point as "not present".
    void setLineGizmo(bool active, common::Vec2 a, common::Vec2 b, common::Vec2 mid,
                      common::Vec2 mid2) noexcept {
        m_lineGizmoActive = active;
        if (active)
            m_handleCorners = {a, b, mid, mid2};
    }

    // Which picture the shared crop/resize channel draws this frame. `Crop` is the Crop tool's own
    // staged box (the rule-of-thirds toggle applies). The other three are the Image menu's live
    // preview riding the same channel (VulkanCanvas::setImageOpPreview):
    //   * Reframe — Canvas Size: the rect IS the new framing, so the shield (what is discarded) and
    //     the green expansion wash (what is added) already say the whole thing;
    //   * Scale — Image Size: the rect is the document at its NEW pixel size, so the pass also
    //     draws a GHOST outline of the current frame it replaces. Without it a resize preview whose
    //     box is the same rectangle reads as no change at all, which was the report;
    //   * Locked — a preview whose rect is DERIVED rather than authored (Rotate Arbitrary's
    //     bounding box): drawn WITHOUT the 8 handles, because a handle nothing can drag is worse
    //     than no handle.
    // Rides the existing controls-mode lane (pc.ants.z, values 7 and 8) — the push block has been
    // at its 128-byte guaranteed budget since S15 and does not grow for this.
    enum class CropChannel : std::uint8_t { Crop = 0, Reframe = 1, Scale = 2, Locked = 3 };

    // The Crop tool's staged rect (S16): the same outline + handles plus the dim-outside
    // shield and (optional) rule-of-thirds guides. Shares the Move controls' push lane (the
    // tools are never active together; crop wins if both are pushed). Same conventions as above.
    void setCropOverlay(bool active, const std::array<common::Vec2, 4>& corners,
                        bool showGrid = true,
                        CropChannel channel = CropChannel::Crop) noexcept {
        m_cropActive = active;
        m_cropCorners = corners;
        m_cropShowGrid = showGrid;
        m_cropChannel = channel;
    }

    // The inpaint sample-area preview (S39): a faint blue wash of the neighbourhood the engine
    // analyses, shown while an inpaint runs. Shares the controls quad lanes (claims them over crop /
    // Move when active). `corners` are the region quad TL,TR,BR,BL in the canvas widget's *logical*
    // px; pushed each frame by the canvas so it tracks pan/zoom.
    void setSampleArea(bool active, const std::array<common::Vec2, 4>& corners) noexcept {
        m_sampleAreaActive = active;
        m_sampleAreaCorners = corners;
    }

    // One Smart Resize keep-region chip (S16-f): the region quad TL,TR,BR,BL in the canvas
    // widget's *logical* px (view-rotation aware, like the crop corners) + its live state
    // against the staged crop rect, which picks the drawn style (see keepChips in the shader).
    enum class ChipState : std::uint8_t {
        Disabled = 0, // user toggled off: dim grey
        Kept = 1,     // fully inside the staged rect: green + wash
        Sliced = 2,   // the rect edge cuts through it: amber
        Lost = 3,     // marked but fully outside the rect: dim red
    };
    struct KeepChip {
        std::array<common::Vec2, 4> corners;
        ChipState state = ChipState::Kept;
    };
    // The chips drawn this frame (binding 8); pass empty for none. Pushed each frame by the
    // canvas while the Crop tool's Smart Resize is on, so chips track pan/zoom/rotate.
    void setKeepChips(const std::vector<KeepChip>& chips);

    // The S33 blur-adjustment centre gizmo (binding 11), drawn by the present pass in the crop/
    // transform chrome language (hairlines + square knobs). `kind` picks the shape: 0 = the DoF
    // focus band (five infinite guides -- the focus line, the two band edges, the two feather
    // edges -- pushed as long segments so they read as infinite lines, plus the centre move knob
    // and the rotate knob); 1 = the Radial crosshair (a single target mark at centerKnob, the
    // other fields unused). All in the canvas widget's *logical* px; the renderer applies the
    // content scale, keep-chips pattern. `hot` is the hovered/dragged handle id (0 centre knob,
    // 1 rotate knob, 2/3 band edges, 4/5 feather edges; -1 none) -- the shader brightens that
    // element. Pushed each frame while a DofBlur or RadialBlur adjustment layer is active.
    struct DofOverlay {
        int kind = 0;                           // 0 = focus band, 1 = radial crosshair
        std::array<common::Vec2, 2> line{};     // the focus (centre) line segment A,B
        std::array<common::Vec2, 2> bandA{};    // the band edge offset along +normal
        std::array<common::Vec2, 2> bandB{};    // ... and along -normal
        std::array<common::Vec2, 2> featherA{}; // the feather edge along +normal
        std::array<common::Vec2, 2> featherB{}; // ... and along -normal
        common::Vec2 centerKnob{};              // the crosshair's mark / the band's move knob
        common::Vec2 rotateKnob{};
    };
    void setDofOverlay(bool active, const DofOverlay& gizmo, int hot) noexcept {
        m_dofActive = active;
        m_dofGizmo = gizmo;
        m_dofHot = hot;
    }
    void setDofOverlay(bool active) noexcept { setDofOverlay(active, DofOverlay{}, -1); }

    // One rulers/guides line segment (binding 13): the two endpoints in the canvas widget's
    // *logical* px (the renderer applies the content scale on upload, the keep-chips pattern) and
    // an RGB colour so document guides (cyan) and the Move tool's smart guides (magenta) can share
    // the one channel. Endpoints are extended past the viewport by the canvas so a guide reads as an
    // infinite line.
    struct GuideLine {
        common::Vec2 a{};
        common::Vec2 b{};
        float cr = 0.0f, cg = 0.0f, cb = 0.0f; // colour, 0..1
    };
    // ONE non-segment rider (S22, the Gradient tool's shape-outline ring): a pair of consecutive
    // entries whose FIRST carries a NEGATIVE `cr` is read by the present pass as an analytic
    // ELLIPSE, not as two chords -- entry k gives the centre (a) and the major-axis tip (b), entry
    // k+1 the minor-axis tip (a) and the real colour. A chorded ring cannot live in a 64-entry lane
    // it shares with the document's guides; two slots and a per-pixel distance field can. This
    // struct has no kind field and the per-frame writer hard-zeroes the trailing std430 float, so
    // the marker rides an impossible colour -- the same out-of-range-sentinel idiom the present
    // pass already uses for the Move anchor and the Type bend tab. See
    // VulkanCanvas::syncGuidesOverlay and canvas_present.comp's guideOverlay/gradientRing.
    // The guide lines drawn this frame (binding 13); pass empty for none. Pushed each frame by the
    // canvas so guides track pan/zoom/rotate. Clamped to kGuideLineMax.
    void setGuideLines(const std::vector<GuideLine>& lines);

    // One knob of the Pen tool's on-canvas chrome (binding 6, S28): a node anchor or a handle tip,
    // in the canvas widget's *logical* px (the renderer applies the content scale on upload, the
    // keep-chips pattern). `kind` picks the drawn shape -- 0 a SQUARE anchor (a cusp / corner node),
    // 1 a ROUND anchor (a smooth / symmetric node), 2 a smaller round handle TIP -- and `state` is a
    // bit set: 1 = selected (the knob fills box-blue instead of white), 2 = hovered (its border
    // turns box-blue). See canvas_present.comp's penChrome for the full treatment.
    struct PenMark {
        common::Vec2 pos{};
        std::uint32_t kind = 0;
        std::uint32_t state = 0;
    };
    // One stem: the hairline from an anchor to one of its handle tips, same logical px.
    struct PenStem {
        common::Vec2 a{};
        common::Vec2 b{};
    };
    // The pen chrome drawn this frame; pass empty vectors and a non-positive `ringRadius` for none.
    // `ringCenter`/`ringRadius` (logical px) are the closing-loop affordance -- the ring shown at the
    // click radius around the path's first node while the pointer is inside it. Pushed each frame by
    // the canvas while the Pen tool is active, so the chrome tracks pan/zoom/rotate. Clamped to
    // kPenMarkMax / kPenStemMax.
    void setPenChrome(const std::vector<PenMark>& marks, const std::vector<PenStem>& stems,
                      common::Vec2 ringCenter, double ringRadius);

#ifdef MOSAIC_DEBUG
    // The canvas FPS readout (Help -> Show Canvas FPS, binding 12): a debug-only diagnostic drawn
    // top-right, on top of every other overlay. `active` toggles it; `fps` is the rounded rate the
    // present shader spells out in its tiny bitmap font. Pushed each frame while the toggle is on.
    // The whole binding-12 channel (and its shader half) compiles out of release builds.
    void setFpsOverlay(bool active, int fps) noexcept {
        m_fpsActive = active;
        m_fpsValue = fps;
    }
#endif

    // The Crop tool's size HUD (S16): a readout below the staged rect. `active` toggles it; its
    // text arrives as a rasterized tile via setOverlayTile. The present pass positions the tile
    // below the (possibly rotated) crop quad, clamped/parked into the viewport. Pushed each frame.
    void setCropHudActive(bool active) noexcept { m_cropHudActive = active; }
    // The Move-tool transform HUD (S15 follow-up): same rasterized tile, parked bottom-right of the
    // viewport instead of below the box. Suppressed by the rotation dial, like the crop HUD.
    void setMoveHudActive(bool active) noexcept { m_moveHudActive = active; }

    // The overlay text tile (S16 rework): an RGBA image the canvas rasterizes with the real UI font
    // for whichever overlay is active (the dial readout or the crop HUD -- they never coexist).
    // `w`x`h` is the uploaded texture (a stable capacity, so it is not reallocated mid-drag);
    // `contentW`x`contentH` is the used sub-rect at its top-left (handed to the shader to place it).
    // Pixels are copied and uploaded next frame, like the mask. Call only when the tile changes.
    void setOverlayTile(const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h,
                        std::uint32_t contentW, std::uint32_t contentH);

    // Advance the marching-ants dash phase (screen px, pre-wrapped to kAntsDashPeriodPx).
    // Pushed each frame by the canvas; it drives the crawl.
    void setAntsPhase(float phasePx) noexcept { m_antsPhase = phasePx; }

    // The hidden `antsCirculate` experiment (S18, docs/research-select-brush.md §5): when on, the
    // ants dash along the local boundary tangent (circulating) instead of the default diagonal crawl.
    // Off by default; the default path is byte-identical to before (pc.ants.x stays 1.0 when active).
    void setAntsCirculate(bool on) noexcept { m_antsCirculate = on; }

    // Pixel grid (S19-c): hairlines on texel boundaries, shown only at high zoom. View-menu toggle.
    void setPixelGrid(bool on) noexcept { m_pixelGrid = on; }

    // Request a swapchain rebuild before the next frame, with the new drawable size in
    // pixels (used as the extent when the surface reports no fixed currentExtent, as Wayland
    // does). Call from the window's resize() handler.
    void notifyResize(int pixelWidth, int pixelHeight) noexcept {
        m_hintWidth = pixelWidth;
        m_hintHeight = pixelHeight;
        m_needsRecreate = true;
    }

    void waitIdle() const noexcept;

    // Can this device tell us when a present reached the screen? (VK_KHR_present_id +
    // VK_KHR_present_wait, probed together.) False is ordinary -- the caller keeps its own clock.
    [[nodiscard]] bool canPacePresents() const noexcept { return m_caps.presentWait; }

    // Has the most recent present been displayed? Polled with a zero timeout: NEVER blocks, and
    // answers true when there is no pacing signal, so a caller written against it degrades to
    // "always ready" rather than to a stall.
    //
    // ⚠ NOT the frame loop's pacing signal, and it must not be made one again. app_window paced on
    // it for one session and the frame rate reached 600 fps on a 200 Hz panel (user report): under
    // MAILBOX -- which S15.z pins in place, see the present-mode choice in createSwapchain -- a
    // "yes" from here is not a statement about the display's CADENCE, so a loop that runs the next
    // frame the moment this turns true is a loop with no upper bound at all. The frame rate is
    // bounded by the panel's refresh interval instead (platform::displayRefreshHz). Kept because it
    // is the only honest answer to "has this exact frame been scanned out", which is a different
    // and still occasionally useful question.
    [[nodiscard]] bool lastPresentDisplayed() const noexcept;

    [[nodiscard]] std::string deviceName() const;
    [[nodiscard]] std::uint32_t validationErrors() const noexcept { return m_validationErrors; }

    // What this device can actually do (S60-alpha). Ask about a CAPABILITY, never a version.
    [[nodiscard]] const GpuCaps& caps() const noexcept { return m_caps; }

    // The one-time, user-facing line this device earned at start-up, or "" when it has nothing to
    // say (the overwhelmingly common case) or when it has already been taken. Today it says
    // exactly one thing: **this is a software rasterizer** (lavapipe / SwiftShader).
    //
    // Where the line sits between "software device" and "no device" (S60-b, plan section 6.2's
    // Level 2): a VK_PHYSICAL_DEVICE_TYPE_CPU device is ACCEPTED, in full -- same instance, same
    // device, same shaders, same present pass, same compute lanes, and `GpuCaps` already gives it
    // a conservative profile (128 px macrotiles, a smaller budget). It is a slow GPU, not a
    // missing one. "No device" means absence: nothing enumerates, or nothing can present to this
    // surface -- and that is the only case that fails `create()`.
    //
    // Handed over ONCE, and by the RENDERER rather than by a bool in the UI, so the "once" belongs
    // to the device it is about: a caller that polls this every frame (which is exactly what the
    // frame loop does, because the renderer comes up lazily) gets the notice on the first frame
    // that has a device and nothing on any frame after it.
    [[nodiscard]] std::string takeStartupNotice();

    // ---- The resident compositor's seat on THIS device (S60-a items 11/12) ---------------------
    //
    // `render::TileCompositor` keeps the composite in device memory frame to frame. For the
    // present pass to use those pixels without a readback they have to be on the device that
    // presents -- images do not cross a VkDevice, and external memory is not in the Vulkan 1.0
    // floor. So the compositor is built on THIS device, wrapped as a borrowing VulkanContext.
    // Created on first call and cached; null if the wrap fails (the caller takes the CPU lane).
    [[nodiscard]] std::shared_ptr<VulkanContext> computeContext();

    // Make the canvas texture exist at the document's size WITHOUT staging any pixels, so the
    // compositor can resolve its resident accumulator straight into it on the device. False when
    // the texture cannot be allocated (a document over maxImageDimension2D, which the present path
    // could not show as one texture anyway) -- the caller falls back to setCanvasImage.
    [[nodiscard]] bool prepareResidentCanvas(std::uint32_t docW, std::uint32_t docH);
    [[nodiscard]] VkImage residentCanvasImage() const noexcept { return m_canvasImage; }
    [[nodiscard]] VkImageView residentCanvasView() const noexcept { return m_canvasView; }
    // The layout the canvas texture is in RIGHT NOW. There is exactly one owner of this fact --
    // this class -- and the resolve pass is handed it rather than guessing, because a wrong guess
    // is either a validation error or silently discarded pixels (an UNDEFINED transition throws
    // away every macrotile the resolve did not touch).
    [[nodiscard]] VkImageLayout residentCanvasLayout() const noexcept { return m_canvasLayout; }
    // "The device wrote the canvas texture and left it in GENERAL." Marks it valid without a
    // single host byte crossing the bus; the next frame transitions it to SHADER_READ_ONLY for the
    // present pass. THIS is what replaces the composite -> CPU mirror -> staging -> upload chain.
    // `wroteImage` false == the resolve found nothing to do and left the image untouched, INCLUDING
    // its layout. Passing true regardless is a layout desync one frame later, not a rounding error.
    void noteResidentCanvasWritten(bool wroteImage = true) noexcept;

private:
    WindowRenderer() = default;

    bool createSwapchain(std::string& error);
    void destroySwapchainObjects() noexcept;
    bool recreate(std::string& error);

    // Canvas (document) texture: (re)allocate to `w`x`h`, or tear down. Independent of the
    // swapchain, so it survives resize. Returns false on allocation failure.
    bool ensureCanvasTexture(std::uint32_t w, std::uint32_t h);
    void destroyCanvasTexture() noexcept;

    // Selection-mask texture (R8, document-sized): same lifecycle as the canvas texture.
    bool ensureMaskTexture(std::uint32_t w, std::uint32_t h);
    void destroyMaskTexture() noexcept;

    // Overlay-text texture (RGBA8): the rasterized HUD/dial tile (S16 rework). Fixed capacity so a
    // size change during a drag does not force a reallocation (which would stall on vkDeviceWaitIdle).
    bool ensureOverlayTexture(std::uint32_t w, std::uint32_t h);
    void destroyOverlayTexture() noexcept;

    // S8 present pipeline: a compute pass that samples the document through the inverse view
    // transform into the rgba8 view image, which is then blitted 1:1 onto the swapchain.
    bool createPresentPipeline(std::string& error);
    bool ensureViewImage(std::string& error); // (re)create the intermediate image == m_extent
    void destroyViewImage() noexcept;
    void writeDescriptors() noexcept; // point the descriptor set at the current images
    bool createDragPipeline(std::string& error);     // S60-a GPU-resident drag pass
    void writeDragDescriptors() noexcept;            // point the drag set at below/dragged/canvas
    void destroyDragTextures() noexcept;             // free the per-gesture below/dragged textures
    bool createIdlePipeline(std::string& error);     // the documentless idle pass (canvas_idle.comp)
    void writeIdleDescriptors() noexcept;            // point the idle set at view image + atlas
    // One-shot create + upload of a device-local rgba8 SAMPLED texture (synchronous; for the
    // per-gesture drag textures). Returns false + sets error on failure.
    bool uploadSampledTexture(const common::Image& img, VkImage& outImage,
                              VmaAllocation_T*& outAlloc, VkImageView& outView, std::string& error);
    [[nodiscard]] common::Affine2D presentInverse() const; // screen(physical px) -> doc px
    // Re-take the device memory snapshot the drag admission is decided against. NOT per frame:
    // vkGetPhysicalDeviceMemoryProperties2 walks every heap and type, so it is taken when the
    // canvas texture is (re)allocated -- the moment this renderer's own footprint on the heap
    // changes, and always before any gesture on that document.
    void refreshMemoryBudget();
    // Build the device timer on first use, and only under a profiler (see the .cpp).
    void ensureGpuTimer();

    GpuCaps m_caps;
    // Device memory as of the last refreshMemoryBudget(). Default-constructed means "nobody has
    // asked yet", which admitDragTextures reads as ABSTAIN rather than as an empty heap.
    GpuMemoryBudget m_memory;
    // Device-time twin for the present chain's CPU rows (render::GpuTimer). Null is ordinary: no
    // timestamp support, or nobody is profiling. Every use is null-guarded.
    std::unique_ptr<GpuTimer> m_timer;
    bool m_timerTried = false; // one creation attempt, made the first frame profiling is on
    // takeStartupNotice()'s payload: set once during create(), emptied by the first taker.
    std::string m_startupNotice;
    // The borrowing compute context over this device (computeContext()). Held by shared_ptr so a
    // lane built on it keeps it alive; it must still be released before ~WindowRenderer destroys
    // the device, which is why the destructor drops it first.
    std::shared_ptr<VulkanContext> m_computeCtx;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    std::uint32_t m_queueFamily = 0;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::uint64_t m_presentId = 0;  // monotonic present tag; 0 == nothing presented yet
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR m_colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D m_extent{0, 0};
    std::vector<VkImage> m_images;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VkSemaphore> m_renderFinished; // one per swapchain image

    VkSemaphore m_imageAvailable = VK_NULL_HANDLE;
    VkFence m_inFlight = VK_NULL_HANDLE;

    // Canvas (document) texture, blitted onto the swapchain each frame; persists across resizes.
    VmaAllocator_T* m_allocator = nullptr;
    VkImage m_canvasImage = VK_NULL_HANDLE;
    VmaAllocation_T* m_canvasAlloc = nullptr;
    VkBuffer m_canvasStaging = VK_NULL_HANDLE;
    VmaAllocation_T* m_canvasStagingAlloc = nullptr;
    void* m_canvasStagingPtr = nullptr;
    std::uint32_t m_canvasW = 0;
    std::uint32_t m_canvasH = 0;
    // The last size the caps gate refused, so the warning is said once per size rather than once
    // per composite -- this is reached from the per-upload path, and a line that repeats every
    // frame buries the log it exists to be found in.
    std::uint32_t m_refusedCanvasW = 0;
    std::uint32_t m_refusedCanvasH = 0;
    VkImageLayout m_canvasLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool m_canvasValid = false;    // a document image has been uploaded at least once
    common::Image m_pendingCanvas; // set by setCanvasImage(), uploaded on the next frame
    bool m_hasPendingCanvas = false;
    common::Image m_pendingCanvasRegion;        // set by setCanvasRegion(), partial upload (S60-a)
    std::uint32_t m_pendingCanvasRegionX = 0;
    std::uint32_t m_pendingCanvasRegionY = 0;
    bool m_hasPendingCanvasRegion = false;
    VkImageView m_canvasView = VK_NULL_HANDLE; // sampled view of the document texture

    // Selection-mask texture (S13 marching ants), uploaded like the canvas texture. A 1x1 zero
    // mask stands in while no selection has been pushed, so binding 2 is always valid.
    VkImage m_maskImage = VK_NULL_HANDLE;
    VmaAllocation_T* m_maskAlloc = nullptr;
    VkBuffer m_maskStaging = VK_NULL_HANDLE;
    VmaAllocation_T* m_maskStagingAlloc = nullptr;
    void* m_maskStagingPtr = nullptr;
    std::uint32_t m_maskW = 0;
    std::uint32_t m_maskH = 0;
    VkImageLayout m_maskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool m_maskValid = false; // a mask has been uploaded at least once
    VkImageView m_maskView = VK_NULL_HANDLE;
    std::vector<std::uint8_t> m_pendingMask; // set by setSelectionMask(), uploaded next frame
    std::uint32_t m_pendingMaskW = 0;
    std::uint32_t m_pendingMaskH = 0;
    bool m_hasPendingMask = false;
    bool m_antsEnabled = false;   // a real (non-placeholder) selection mask is loaded
    bool m_antsCirculate = false; // §5 experiment: dash along the boundary tangent (default off)
    float m_antsPhase = 0.0f;   // dash crawl phase, screen px (pushed each frame)

    // Overlay-text texture (S16 rework): the rasterized HUD/dial tile, uploaded like the mask. A
    // 1x1 transparent tile stands in until the canvas pushes one, so binding 3 is always valid.
    VkImage m_overlayImage = VK_NULL_HANDLE;
    VmaAllocation_T* m_overlayAlloc = nullptr;
    VkBuffer m_overlayStaging = VK_NULL_HANDLE;
    VmaAllocation_T* m_overlayStagingAlloc = nullptr;
    void* m_overlayStagingPtr = nullptr;
    std::uint32_t m_overlayW = 0;
    std::uint32_t m_overlayH = 0;
    VkImageLayout m_overlayLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool m_overlayValid = false;
    VkImageView m_overlayView = VK_NULL_HANDLE;
    std::vector<std::uint8_t> m_pendingOverlay; // set by setOverlayTile(), uploaded next frame
    std::uint32_t m_pendingOverlayW = 0;
    std::uint32_t m_pendingOverlayH = 0;
    bool m_hasPendingOverlay = false;
    std::uint32_t m_overlayContentW = 0, m_overlayContentH = 0; // used sub-rect (pushed each frame)

    // In-flight lasso polyline (SSBO, binding 4): the present pass draws it as a smooth inverted line.
    // Host-visible + persistently mapped, fixed capacity (kLassoMaxVerts); the latest path is written
    // each frame after the fence wait (single-frame-in-flight), like the staging copies. Buffer layout
    // is std430 {uint count; uint pad; vec2 pts[]} -- count <= 1 means inactive.
    VkBuffer m_lassoBuffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_lassoAlloc = nullptr;
    void* m_lassoPtr = nullptr;
    std::vector<common::Vec2> m_lassoVerts; // latest logical-px polyline (set by setLassoPolyline)
    // Brush reticle (S19-a): rides the same overlay SSBO. Logical px; scaled by the content scale on
    // upload, like the lasso path. Inactive until the canvas sets it for a brush-family tool.
    bool m_reticleActive = false;
    common::Vec2 m_reticleCenter{0.0, 0.0};
    double m_reticleSemiX = 0.0; // the tip's two semi-axes and its screen rotation -- an ELLIPSE
    double m_reticleSemiY = 0.0; // (§6.3); a round tip has semiX == semiY and angle 0
    double m_reticleAngle = 0.0;
    bool m_reticleLocked = false;
    int m_overlayLineStyle = 1; // Settings->Appearance line style (setOverlayLineStyle);
                                // 1 = Shadowed/rim, the default
    int m_featherIndicator = 0; // Settings->Appearance feathered-selection indicator
                                // (setFeatherIndicator); 0 = Bracketing ant pair (A), the default
    std::uint32_t m_channelView = 0; // Channels-tab on-canvas isolation (setChannelView); 0 =
                                     // the normal full-colour composite, and the shader's identity

    // The reticle's tip-outline SDF (binding 9, §6.3): its OWN host-visible SSBO, because binding 4's
    // struct ends in a flexible array member (`pts[]`) and nothing can follow one. std430 header:
    // {uint active; uint w; uint h; uint pad; vec2 box; vec2 pad2;} = 32 bytes, then the float grid.
    // The payload is memcpy'd into the mapped buffer only when `m_sdfKey` changes -- the header (the
    // active flag) is written every frame like the others, but the grid is not: it is the one thing
    // here that would cost real bandwidth on a mouse move.
    VkBuffer m_sdfBuffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_sdfAlloc = nullptr;
    void* m_sdfPtr = nullptr;
    std::uint64_t m_sdfKey = 0;      // tip raster id + frame; 0 = no field (the analytic ellipse)
    std::uint32_t m_sdfW = 0;        // padded grid cells
    std::uint32_t m_sdfH = 0;
    std::uint32_t m_sdfPad = 0;      // cells of background per side (core::brush::kTipSdfPad)
    double m_sdfBoxW = 0.0;          // the tip's true extent in the grid's build px (NOT w/h)
    double m_sdfBoxH = 0.0;
    std::vector<float> m_pendingSdf; // uploaded on the next frame, then dropped (like m_pendingMask)
    bool m_sdfDirty = false;
    bool m_reticleTracing = false; // per frame: does the ring read the field, or draw the ellipse?

    // The eyedropper's loupe (S24, binding 10): its own host-visible + persistently mapped SSBO -- a
    // fixed 48-byte struct (no flexible array), rewritten each frame like the reticle header. Logical
    // px for the centre/radius/magnification (scaled by the content scale on upload); document px for
    // the sample texel centre. Inactive until the canvas sets it for the eyedropper.
    VkBuffer m_loupeBuffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_loupeAlloc = nullptr;
    void* m_loupePtr = nullptr;
    bool m_loupeActive = false;
    common::Vec2 m_loupeCenter{0.0, 0.0};    // the cursor, logical px
    double m_loupeRadius = 0.0;              // disk radius, logical px
    double m_loupeMag = 0.0;                 // logical px per document texel
    common::Vec2 m_loupeSampleDoc{0.0, 0.0}; // sampled texel centre, document px
    common::Color8 m_loupeSampleColor{};     // the resolved sample colour (ring top arc + readout)
    common::Color8 m_loupePrevColor{};       // the swatch the pick would replace (ring bottom arc)
    bool m_loupeReadout = false;             // composite the hex/RGB tile by the ring

    // Type-tool caret + selection overlay (S29-b, binding 5): its own host-visible + persistently
    // mapped SSBO (fixed capacity), written each frame like the lasso buffer. Caret bar endpoints +
    // selection quads in logical px (scaled by the content scale on upload). Inactive by default.
    VkBuffer m_textBuffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_textAlloc = nullptr;
    void* m_textPtr = nullptr;
    bool m_textCaretActive = false;
    unsigned m_textHandleCount = 0u; // trailing selQuads drawn as solid box-coloured handles (resize/bend)
    bool m_textBendActive = false;   // the baseline bend drop-tab handle is shown this frame (§9)
    common::Vec2 m_textBendPill{};   // its pill centre (logical px)
    common::Vec2 m_textBendApex{};   // the bar apex its stem reaches (logical px)
    common::Vec2 m_textCaretA{0.0, 0.0};
    common::Vec2 m_textCaretB{0.0, 0.0};
    std::vector<std::array<common::Vec2, 4>> m_textSelQuads; // logical px, set by setTextOverlay
    // Spell-check squiggle underline segments (deferred §2), logical px, set by setSpellSquiggles;
    // uploaded after the selection quads in the same SSBO (binding 5).
    std::vector<std::array<common::Vec2, 2>> m_spellSquiggles;

    // Smart Resize keep-region chips (S16-f, binding 8): its own host-visible + persistently
    // mapped SSBO (fixed capacity kKeepChipMaxRects), written each frame like the text overlay.
    // Layout: std430 {uint count; uint pad0; vec2 min; vec2 max; vec2 pad1;} = 32 bytes, then
    // 3 vec4 per chip (two corner pairs + a meta vec4, x = enabled). Inactive at count == 0.
    VkBuffer m_chipBuffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_chipAlloc = nullptr;
    void* m_chipPtr = nullptr;
    std::vector<KeepChip> m_keepChips; // logical px, set by setKeepChips

    // The DoF focus-band gizmo (S33, binding 11): its own host-visible + persistently mapped SSBO
    // -- a fixed 112-byte struct (a 16-byte header + five guide segments + the knob pair),
    // rewritten each frame like the loupe. Logical px, scaled by the content scale on upload (in
    // the drawFrame block, not the setter -- single-frame-in-flight discipline). Inactive until
    // the canvas pushes it for an active DofBlur adjustment layer.
    VkBuffer m_dofBuffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_dofAlloc = nullptr;
    void* m_dofPtr = nullptr;
    bool m_dofActive = false;
    DofOverlay m_dofGizmo{};
    int m_dofHot = -1; // hovered/dragged handle id (-1 = none); the header stores id + 1

    // Rulers/guides line overlay (binding 13): its own host-visible + persistently mapped SSBO
    // (fixed capacity kGuideLineMax), written each frame like the keep chips. std430 header 16 bytes
    // (uint count + 3 pad), then 2 vec4 per line (endpoint pair + colour). Inactive at count == 0.
    VkBuffer m_guideBuffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_guideAlloc = nullptr;
    void* m_guidePtr = nullptr;
    std::vector<GuideLine> m_guideLines; // logical px, set by setGuideLines

    // The Pen tool's node/handle chrome (binding 6, S28): its own host-visible + persistently mapped
    // SSBO (fixed capacity kPenMarkMax + kPenStemMax), written each frame like the guides. std430
    // header 48 bytes, then one vec4 per mark followed by one per stem. Inactive at both counts 0
    // with the ring off. Logical px, scaled on upload.
    VkBuffer m_penBuffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_penAlloc = nullptr;
    void* m_penPtr = nullptr;
    std::vector<PenMark> m_penMarks; // logical px, set by setPenChrome
    std::vector<PenStem> m_penStems;
    common::Vec2 m_penRingCenter{};
    double m_penRingRadius = 0.0; // <= 0 = the closing-loop ring is not shown this frame

#ifdef MOSAIC_DEBUG
    // The canvas FPS readout SSBO (binding 12, debug builds only): its own host-visible +
    // persistently mapped buffer, the loupe/DoF pattern. The host spells the rate into glyph indices
    // in the per-frame write. Release builds carry no binding-12 channel at all.
    VkBuffer m_fpsBuffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_fpsAlloc = nullptr;
    void* m_fpsPtr = nullptr;
    bool m_fpsActive = false;
    int m_fpsValue = 0;
#endif

    // S8 canvas presenter: a compute pass samples the document through the inverse view transform
    // into an rgba8 view image (== swapchain extent), then it is blitted 1:1 onto the swapchain.
    // Replaces the S7-c fixed centered blit; supports pan/zoom/rotate.
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule m_presentShader = VK_NULL_HANDLE;
    VkPipeline m_presentPipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descSet = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkImage m_viewImage = VK_NULL_HANDLE; // intermediate compute output (size == m_extent)
    VmaAllocation_T* m_viewAlloc = nullptr;
    VkImageView m_viewImageView = VK_NULL_HANDLE;
    VkExtent2D m_viewExtent{0, 0};
    bool m_descDirty = true; // descriptor set needs re-pointing at the images

    // GPU-resident drag pass (S60-a): a second compute pipeline + its own descriptor set, plus the
    // per-gesture `below` and `dragged` source textures it composites into the canvas texture.
    VkDescriptorSetLayout m_dragSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_dragPipelineLayout = VK_NULL_HANDLE;
    VkShaderModule m_dragShader = VK_NULL_HANDLE;
    VkPipeline m_dragPipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_dragDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_dragDescSet = VK_NULL_HANDLE;
    VkImage m_belowImage = VK_NULL_HANDLE;
    VmaAllocation_T* m_belowAlloc = nullptr;
    VkImageView m_belowView = VK_NULL_HANDLE;
    VkImage m_draggedImage = VK_NULL_HANDLE;
    VmaAllocation_T* m_draggedAlloc = nullptr;
    VkImageView m_draggedView = VK_NULL_HANDLE;
    bool m_dragActive = false;
    std::uint32_t m_draggedW = 0;
    std::uint32_t m_draggedH = 0;
    float m_dragInv[6] = {1, 0, 0, 1, 0, 0}; // doc->layer-local: invR0.xy, invR1.xy, invT.xy
    int m_dragMode = 0;
    float m_dragOpacity = 1.0f;
    // Retired drag textures awaiting destruction after the frame fence (so endGpuDrag never blocks
    // the UI thread on a device-idle wait -- the inconsistent release stall). Drained in drawFrame.
    struct DeadTexture {
        VkImage image;
        VmaAllocation_T* alloc;
        VkImageView view;
    };
    std::vector<DeadTexture> m_deadDragTextures;

    // The documentless idle pass (canvas_idle.comp): its own pipeline + descriptor set (view
    // image + invitation atlas), the drag pass's structural sibling. The atlas keeps a 1x1
    // transparent placeholder so the descriptor set is valid before the first bake arrives.
    VkDescriptorSetLayout m_idleSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_idlePipelineLayout = VK_NULL_HANDLE;
    VkShaderModule m_idleShader = VK_NULL_HANDLE;
    VkPipeline m_idlePipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_idleDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_idleDescSet = VK_NULL_HANDLE;
    VkImage m_idleAtlasImage = VK_NULL_HANDLE;
    VmaAllocation_T* m_idleAtlasAlloc = nullptr;
    VkImageView m_idleAtlasView = VK_NULL_HANDLE;
    std::uint32_t m_idleAtlasW = 0; // full atlas texture size (rowH = m_idleAtlasH / rows)
    std::uint32_t m_idleAtlasH = 0;
    std::uint32_t m_idleAtlasRows = 1;
    bool m_idleAtlasReal = false;  // a baked invitation (not the placeholder) is loaded
    bool m_idleDescDirty = true;   // idle set needs re-pointing (view image or atlas changed)
    IdleField m_idleField;

    common::Affine2D m_docToScreen{}; // document px -> logical screen px (set by the UI)
    double m_contentScale = 1.0;      // logical -> physical px (HiDPI)
    bool m_hasView = false;           // setView() called at least once

    bool m_overlayActive = false; // rotation dial (S8-b) shown this frame
    double m_overlayAngle = 0.0;  // radians
    bool m_handlesActive = false;                     // Move-tool controls (S15) this frame
    bool m_lineGizmoActive = false;                   // the Line shape gizmo (S26) this frame
    std::array<common::Vec2, 4> m_handleCorners{};    // quad TL,TR,BR,BL (or a,b,mid for the gizmo)
    float m_rotateDotAlpha = 0.0f;                    // rotate-hotspot dots on the Move/Shape box
    bool m_anchorActive = false;                      // Move-tool transform anchor (pivot) glyph
    common::Vec2 m_anchorPos{};                       // its position, canvas logical px
    bool m_textDotsActive = false;                    // ... and the Type box's, dots alone (mode 6)
    std::array<common::Vec2, 4> m_textDotCorners{};
    float m_textDotAlpha = 0.0f;
    bool m_cropActive = false;                        // crop overlay (S16) this frame
    std::array<common::Vec2, 4> m_cropCorners{};
    bool m_cropShowGrid = true;                       // crop rule-of-thirds guides (S16-c toggle)
    CropChannel m_cropChannel = CropChannel::Crop;    // whose picture the channel draws (see above)
    bool m_sampleAreaActive = false;                  // inpaint sample-area preview (S39) this frame
    std::array<common::Vec2, 4> m_sampleAreaCorners{};
    bool m_cropHudActive = false;                     // crop size HUD (S16) this frame
    bool m_moveHudActive = false;                     // Move-tool transform HUD (S15) this frame
    bool m_pixelGrid = true;                          // pixel grid (S19-c); Photoshop default on

    int m_hintWidth = 0;
    int m_hintHeight = 0;
    bool m_needsRecreate = false;
    std::uint32_t m_validationErrors = 0;
};

} // namespace mosaic::render
