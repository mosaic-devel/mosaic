#pragma once

#include "common/image.hpp"
#include "core/brush/brush_engine.hpp" // core::brush::BrushEngine (S19-a brush tool)
#include "core/brush/mask_stroke.hpp"  // core::brush::MaskStroke (S18 select brush)
#include "core/brush/stroke_smoother.hpp" // core::brush::StrokeSmoother (brush smoothing)
#include "core/clone_stamp.hpp"        // core::CloneAnchorState / CloneSampleSource (S38)
#include "core/guides.hpp"             // core::Guide (document guides, View -> Guides)
#include "core/layer.hpp"              // core::LayerId (the Move tool's target, S15)
#include "core/layer_grow.hpp"         // core::PixelBox (the brush's bounded auto-grow)
#include "core/snap.hpp"               // core::SnapCandidates (View -> Snap)
#include "core/stroke_confinement.hpp" // core::StrokeConfinement (the brush's selection confinement)
#include "core/selection.hpp"
#include "core/text/extrude_render.hpp" // ExtrudePlaneMap (3D-faithful editing chrome, S30-d r2)
#include "core/text/shaping.hpp"   // core::text::ShapedBlock (Type-tool caret geometry, S29-b)
#include "core/text/spell_scan.hpp" // core::text::MisspelledRange (spell-check squiggles, deferred §2)
#include "core/text/text_edit.hpp" // core::text::TextSelection
#include "ui/canvas_view.hpp"
#include "ui/clone_stamp_gesture.hpp" // CloneStampOptions + the clone tool's pure gesture math (S38)
#include "ui/crop_gesture.hpp"
#include "ui/idle_fade.hpp" // IdleFadeState (the documentless idle pass's choreography)
#include "ui/brush_reticle.hpp" // HoverHeading: the pointer's direction, for a direction-following tip
#include "ui/cursor_apply.hpp" // MoveCursor + makeCursorImage (the FLTK side of ui/cursors.hpp)
#include "ui/cursors.hpp"
#include "ui/selection_gesture.hpp"
#include "ui/gradient_gesture.hpp"
#include "ui/shape_gesture.hpp"
#include "ui/pen_gesture.hpp"
#include "ui/warp_gesture.hpp"   // WarpOptions + the warp tools' pure gesture math (S35-b)
#include "render/warp.hpp"       // render::WarpQuality: the draft/final split the drag rides
#include "ui/red_eye_gesture.hpp" // RedEyeOptions + the eye tool's pure gesture math (S38-b)
#include "ui/tablet_input.hpp" // the tablet wiring: samples in, StrokeInput out (docs/tablet.md)
#include "ui/transform_gesture.hpp"

#include <FL/Fl_Window.H>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Fl_RGB_Image;

namespace mosaic::core {
class Document;
}

namespace mosaic::render {
class WindowRenderer;
}

namespace mosaic::platform {
class WaylandSubsurface;
}

namespace mosaic::ui {

class ToolManager;

// A child Fl_Window whose contents are drawn by Vulkan instead of FLTK. Because it is its
// own (sub)window it owns a dedicated native surface, so the Vulkan swapchain never competes
// with the FLTK-drawn menu bar for the parent window's surface. The renderer is created
// lazily on the first frame, once the native handle exists (i.e. after show()).
class VulkanCanvas : public Fl_Window {
public:
    VulkanCanvas(int X, int Y, int W, int H);
    ~VulkanCanvas() override;

    // Render + present one frame (a no-op until the window is shown). Safe to call from a
    // timer; also invoked from draw() on expose.
    void renderFrame();

    void setClearColor(common::Color8 c) noexcept { m_clearColor = c; }

    // Show `img` (a document composite) on the canvas. Held until the renderer exists (created
    // lazily on the first frame), then handed to it; safe to call before show(). `fitView`
    // re-centres/zooms the viewport to frame the whole document -- pass it when a new document is
    // created or opened (S9); the first non-empty image always fits. When false, the current
    // pan/zoom/rotation is kept (a re-composite of the same document -- S10+).
    void setDocumentImage(const common::Image& img, bool fitView = false);

    // Patch only the sub-rectangle `sub` (placed at document px (x,y)) of the canvas without
    // re-uploading the whole composite -- the dirty-region path (S60-a) for live brush strokes and
    // inpaint previews. The document size/view are unchanged. Requires a full setDocumentImage to
    // have established the texture first (the caller guarantees this); copied, handed to the
    // renderer on the next frame like setDocumentImage.
    void setDocumentRegion(const common::Image& sub, std::uint32_t x, std::uint32_t y);

    // ---- The resident compositor's seat (S60-a items 11/13) -----------------------------------
    //
    // The renderer, once it exists. Null until the first renderFrame() -- it is created lazily on
    // the first frame, once the native handle exists -- and the caller's answer to null is always
    // "not this frame", never a wait. `render::TileCompositor` is built on THIS object's device
    // (WindowRenderer::computeContext) because a VkImage does not cross a VkDevice.
    [[nodiscard]] render::WindowRenderer* renderer() noexcept { return m_renderer.get(); }

    // Called with the device still ALIVE and idle, immediately before the renderer is destroyed,
    // so anything built on WindowRenderer::computeContext() can be torn down in time.
    // ⚠ The device does NOT die in a destructor: hide() destroys it too (FLTK tears the renderer
    // down before it frees the native window), and hide() runs long before ~MainWindow. A lane
    // released in the host's destructor is therefore released after its VkDevice is already gone,
    // which surfaces as leaked-object reports from the validation layer and a vkDeviceWaitIdle on
    // an invalid handle. Must be idempotent: hide() and ~VulkanCanvas() can both fire it.
    void setOnRendererShutdown(std::function<void()> cb) { m_onRendererShutdown = std::move(cb); }

    // The document is `w`x`h` and its pixels are ALREADY in the canvas texture, written on the
    // device by TileCompositor::resolve(). This is setDocumentImage's resident twin: it does the
    // view bookkeeping (document size, the first/fitView frame) and CANCELS any queued CPU upload,
    // because two writers to one texture in one frame is how a resident path silently reinstates
    // the copy it exists to delete.
    void adoptResidentDocument(std::uint32_t docW, std::uint32_t docH, bool fitView);

    // Whether ANY pointer gesture is in flight. The aggregate the host needs for the readback
    // seam's gesture predicate (docs/s60-readback-consumers.md §7(e)) and to keep the resident
    // composite from alternating lanes inside one stroke.
    [[nodiscard]] bool anyGestureActive() const { return pointerGestureActive(); }

    // GPU-resident Move/Resize/Rotate drag (S60-a), forwarded to the renderer. beginGpuDrag returns
    // whether the GPU path is armed (false if the renderer isn't up yet or the upload failed -- the
    // caller then stays on the CPU recomposite). setGpuDragTransform updates the dragged layer's
    // doc->layer-local transform + blend each frame; endGpuDrag disarms it.
    bool beginGpuDrag(const common::Image& below, const common::Image& dragged);
    // Ask before paying for the below-composite -- see WindowRenderer::canHostDragTextures.
    [[nodiscard]] bool canHostGpuDrag(std::uint32_t belowW, std::uint32_t belowH,
                                      std::uint32_t dragW, std::uint32_t dragH) const;
    void setGpuDragTransform(const common::Affine2D& docToLayerLocal, int blendMode, float opacity);
    void endGpuDrag();

    // Show the document's selection as animated marching ants (S13). `coverage` is the 8-bit
    // document-sized selection mask (copied; handed to the renderer on the next frame, like the
    // document image); null / zero size hides the marquee ("no selection"). Call on selection
    // changes -- the crawl animates by itself off the frame clock.
    void setSelectionMask(std::uint32_t w, std::uint32_t h, const std::uint8_t* coverage);

    // View controls (S8) -- also driven by the View menu. Each updates the per-canvas view
    // transform; the ~60 Hz frame timer re-presents with it.
    void zoomIn();
    void zoomOut();
    void fitToWindow();
    void actualPixels();

    // S13-b status-bar notifications (events, never the frame loop). The cursor callback fires
    // on move/drag/leave with the pointer's *document* coordinates (`overCanvas` false when the
    // pointer leaves); the view callback fires when zoom or rotation changes (a pan moves
    // neither, but it does shift the document under a stationary cursor -- the drag path covers
    // that via the cursor callback).
    void setCursorCallback(std::function<void(double docX, double docY, bool overCanvas)> cb) {
        m_onCursor = std::move(cb);
    }
    void setViewChangedCallback(std::function<void()> cb) { m_onViewChanged = std::move(cb); }
    [[nodiscard]] const CanvasView& view() const noexcept { return m_view; }

    // S49: a document tab's zoom/pan/rotation, saved on switch away and restored on switch back.
    [[nodiscard]] CanvasView::ViewState viewState() const { return m_view.state(); }
    void setViewState(const CanvasView::ViewState& s) {
        m_view.setState(s);
        notifyViewChanged(); // the zoom/rotation readouts follow the tab
        requestHostFrame();
    }

    // S14 marquee/lasso tools. The canvas routes tool-aware pointer input (active tool read from
    // the ToolManager) through a SelectionGesture: the live preview goes straight to the canvas
    // mask (rebuilt at most once per frame), and the host's `commit` callback receives the final
    // mask to land as the gesture's single SetSelectionCommand. `base` supplies the document's
    // current selection for the boolean ops (null = no document).
    struct SelectionHost {
        std::function<const core::Selection*()> base;
        // Document::selectionRevision(). The S16-i nudge session caches the press-time mask, so it
        // must notice when anything else (undo, the Select menu, a marquee) replaced the selection
        // underneath it -- a revision that is not the one our own last commit produced ends the
        // session and starts a fresh undo step.
        std::function<std::uint64_t()> revision;
        // Land a mask as one SetSelectionCommand. `coalesce` (non-zero) merges consecutive commits
        // of one nudge burst into a single undo step; `label` names the step in History.
        std::function<void(core::Selection, std::uint64_t coalesce, std::string_view label)> commit;
    };
    void setToolManager(ToolManager* tools) noexcept { m_tools = tools; }
    void setSelectionHost(SelectionHost host) { m_selectionHost = std::move(host); }
    // S17 Magic wand. The canvas owns only the pointer side (map the click to a document point, read
    // the press-time modifiers into a boolean op); the host owns everything a colour flood needs --
    // the document, the merged composite, the tool options, and the commit funnel -- and lands ONE
    // SetSelectionCommand per click (docs/research-selection.md §8). A single click, no gesture.
    struct MagicWandHost {
        std::function<void(common::Vec2 docPt, core::SelectOp op)> click;
    };
    void setMagicWandHost(MagicWandHost host) { m_magicWandHost = std::move(host); }
    // L1 edge-aware select brush (grow-to-edges). The canvas owns the pointer
    // side -- the seed stroke (a plain MaskStroke) and the press-time op; the host owns what the
    // grow needs (the document-space source image per the tool's Source option + the Reach / Edge
    // Stop options) and returns the grown selection. Empty result = no source / nothing grown (the
    // host shows any hint). ⚠ The grow runs ONCE, on release -- never during the stroke: no grown
    // region is computed or displayed while the stroke is in progress, and that is an invariant.
    struct EdgeBrushHost {
        std::function<core::Selection(const core::Selection& seeds)> grow;
    };
    void setEdgeBrushHost(EdgeBrushHost host) { m_edgeBrushHost = std::move(host); }
    // S38-b Eye retouch (docs/red-eye-tool.md §4). Both modes are ONE gesture shape: the canvas
    // paints a coverage scope with a core::brush::MaskStroke (the select-brush lane's engine --
    // coverage only, no image analysis) and hands the finished scope to the host on RELEASE. The
    // host owns the active layer, the document selection it must be clipped to (§2.4), the colour
    // math and the single region-scoped SetLayerPixelsCommand. Nothing is computed or shown during
    // the stroke beyond the raw painted trail and the size ring -- the tool never looks at the
    // image until the user says where -- always user-scoped, with no detection of any kind.
    struct RedEyeHost {
        std::function<void(core::RedEyeMode mode, core::Selection scope, RedEyeOptions options)>
            apply;
    };
    void setRedEyeHost(RedEyeHost host) { m_redEyeHost = std::move(host); }
    // S38 Stamp / Clone (docs/clone-stamp.md §5). The stroke itself rides the ordinary brush lane --
    // the canvas owns the engine, the reticle and the source anchor, and the finished pixels land
    // through BrushToolHost::commitStroke like any other stroke -- so the only thing the host owes
    // it is the SAMPLE SOURCE for the two composited modes, which needs the document and the
    // compositor and therefore cannot live here.
    struct CloneStampHost {
        // A DOCUMENT-SPACE snapshot of the pixels the stroke samples, taken ONCE at the press.
        // `belowOnly` picks "current & below" over "all layers". An empty return means the snapshot
        // could not be made; the canvas then refuses the stroke rather than quietly cloning
        // something else.
        std::function<common::Image(bool belowOnly)> backdrop;
        // Painted with no source anchor picked yet: show a status hint naming the way in. A silent
        // no-op reads as a broken tool.
        std::function<void()> noSourceAttempt;
    };
    void setCloneStampHost(CloneStampHost host) { m_cloneHost = std::move(host); }
    // Forget the picked clone source (a document swap: an anchor is a point in a document that no
    // longer exists). NOT called on a tool switch -- the anchor is the tool's memory, and losing it
    // every time you reach for the eyedropper would make the tool unusable.
    void clearCloneSource();
    // S21 Bucket fill. Like the wand, a single click with no gesture: the canvas maps the press to
    // a document point and the host owns everything a flood-fill needs -- the active layer, the
    // tool options, the seed read, the flood, the selection intersection, and the FillCommand.
    struct BucketFillHost {
        std::function<void(common::Vec2 docPt)> click;
    };
    void setBucketFillHost(BucketFillHost host) { m_bucketFillHost = std::move(host); }
    // S24 Eyedropper. The canvas maps the cursor to a document point and drives the loupe; the host
    // owns the pixels. `sample` resolves the colour at a doc point under the current tool options
    // (Source = active layer / composite, and the sample-size average), returning nullopt when there
    // is nothing to pick (off the pixels / a non-raster active layer) -- it drives the loupe's live
    // readout on hover. `commit` samples the same way and writes the result into the foreground swatch
    // (or the background when `toBackground`). Both are pure reads of the document; no undo step (a
    // swatch change is app state, not a document command).
    struct EyedropperHost {
        std::function<std::optional<common::Color8>(common::Vec2 docPt)> sample;
        std::function<void(common::Vec2 docPt, bool toBackground)> commit;
        // The swatch a pick would REPLACE right now (fg, or bg when `background`) -- the loupe's
        // colour-comparison ring shows it as the bottom arc, under the live sample's top arc.
        std::function<common::Color8(bool background)> previous;
    };
    void setEyedropperHost(EyedropperHost host) { m_eyedropperHost = std::move(host); }
    // S18 select brush default combine op (Settings::selectBrushAddByDefault, §9-B): a no-modifier
    // stroke uses this op (true = Add); Alt always subtracts. No Settings-dialog UI -- applied at load.
    void setSelectBrushAddByDefault(bool on) noexcept { m_selectBrushAddByDefault = on; }
    // S50: files dropped ON THE CANVAS, as local paths in drop order. The host places each as a
    // magic layer (a .mosaic opens as a document instead -- a document is not a layer). Leave the
    // callback unset, or return with no document open, and the canvas refuses the drag outright, so
    // the drag source shows "no drop target here". The canvas is its own Fl_Window, so it receives
    // FL_DND_* directly and the payload arrives as a DIRECT FL_PASTE -- it never bubbles to the
    // main window (the EmptyStateView learnt this the hard way).
    void setFileDropHost(std::function<void(const std::vector<std::string>&)> cb) {
        m_onFilesDropped = std::move(cb);
    }
    // The documentless idle state (the EmptyStateView's successor): with no document open the
    // canvas renders the ambient ripple field + the open-an-image invitation itself (the idle
    // pass, canvas_idle.comp) and takes over the invitation's input -- click-anywhere opens,
    // hover shows the hand + lifts the frame, and the DND conversation lands here directly (the
    // canvas is its own Fl_Window; see setFileDropHost above). The host flips setIdleEnabled on
    // clearDocument / adoptActiveDocument; the fades run from the canvas's own clock, phase-
    // continuous, so close-then-reopen resumes the field rather than restarting it.
    void setIdleEnabled(bool on);
    // A file drag is hovering the window's CHROME (toolbar/panels/status bar): mirror the hot
    // bloom, exactly as the window-level handler used to light the old view's frame.
    void setIdleDropHot(bool hot);
    [[nodiscard]] bool idleActive() const noexcept { return m_idleEnabled; }
    void setOnIdleOpen(std::function<void()> cb) { m_onIdleOpen = std::move(cb); }
    // A drop on the idle canvas resolved to a local path (the invitation is a full drop target).
    void setOnIdleOpenPath(std::function<void(const std::string&)> cb) {
        m_onIdleOpenPath = std::move(cb);
    }
    // S33 blur-adjustment centre gizmo (docs/blur-filters.md §6). Two shapes, one chrome family,
    // picked by DofGizmoState::kind:
    //   * Band (DofBlur): the focus-band chrome -- the centre line, the two band edges, the two
    //     feather edges, a move knob on the centre and a rotate knob on the line -- drags edit the
    //     GEOMETRY (centre/angle/band/feather).
    //   * Crosshair (RadialBlur; S35 Wave in RIPPLE mode): a single target mark at the centre --
    //     one knob, drags move only center_x/center_y (the pivot for Spin, the vanishing point for
    //     Zoom, the ring origin for Ripple). Plain Wave does not use its centre at all, so it wears
    //     NO gizmo -- a handle that moves a parameter nothing reads is worse than no handle.
    //   * Ring (S35 Vignette): the centre knob plus a RADIUS knob on a hairline arm, so the falloff
    //     can be placed and sized on the image instead of through two number sliders.
    // Either way amounts stay in the popover, never on canvas (a hard split) -- the
    // Vignette radius is geometry, the same class as the DoF band's half-width, not an amount. All
    // three are layer-bound and tool-INDEPENDENT: the handles work whatever tool is active (the
    // first such chrome). The canvas owns the pointer side; the host owns the layer lookup and the
    // params-bag commands. The gizmo state lives in the adjustment's PARENT space plus the
    // transform that places that space onto the document (identity for a root-level layer) -- so a
    // filter nested in a transformed group keeps honest handles.
    enum class BlurGizmoKind { Band, Crosshair, Ring };
    struct DofGizmoState {
        BlurGizmoKind kind = BlurGizmoKind::Band;
        common::Vec2 center;          // parent-space px (the crosshair's / ring's centre handle)
        double angleDeg = 0.0;        // Band only
        double band = 0.0;            // Band only: half-width, parent-space px
        double feather = 1.0;         // Band only
        double radius = 0.0;          // Ring only: parent-space px
        common::Affine2D parentToDoc; // parent space -> document space
    };
    // Fills the state for the ACTIVE layer iff it is one of the centre-carrying kinds (DofBlur,
    // RadialBlur, Vignette, Wave-in-Ripple); returns false otherwise (the gizmo hides). Queried per
    // frame (syncDofOverlay) and per hover/press.
    void setDofGizmoProvider(std::function<bool(DofGizmoState&)> cb) {
        m_dofGizmoProvider = std::move(cb);
    }
    // The canvas pushes edited geometry here; the host maps it into a params-bag edit. `coalesceId`
    // names the gesture's parameter ("dof:center" / "dof:angle" / "dof:band" / "dof:feather" /
    // "radial:center" / "ring:center" / "ring:radius"), so one drag coalesces into ONE undo step,
    // mirroring the editShape/editGradient coalescing.
    void setDofGizmoEdit(std::function<void(const char* coalesceId, const DofGizmoState&)> cb) {
        m_dofGizmoEdit = std::move(cb);
    }
    // True while a DoF handle drag is in flight -- the host switches its composites into draft
    // mode for the gesture, like transformGestureActive().
    [[nodiscard]] bool dofHandleGestureActive() const noexcept { return m_dofDrag.active; }

    // Abort an in-flight gesture and restore the document's own ants (tool switch, new document).
    void cancelSelectionGesture();
    // Forwarded by the main window on modifier keydown/keyup: refresh the tool cursor (the
    // op badge follows the live modifiers) for the current hover state. Our own key handler
    // covers this only while we hold focus -- which hovering alone never grants.
    void modifiersChanged();
    // The tracked pointer in canvas-relative logical px -- the anchor the GPU brush reticle and
    // eyedropper loupe are drawn at. Written ONLY by pointer events delivered to the canvas (and
    // the tablet sink); keyboard events must never move it, because their Fl::event_x/y are
    // relative to the top-level window (see updateToolCursor). Public so the regression test can
    // pin exactly that.
    [[nodiscard]] common::Vec2 pointerLogical() const noexcept { return m_cursorLogical; }
    // The window system's own "is this key physically held?" oracle, which the Space/R gesture
    // modifiers arbitrate their key events against (see m_spaceDown). Production NEVER sets this:
    // the default asks Fl::event_key(), which needs an open display. It exists so the headless
    // regression test can drive the one input sequence no test rig can otherwise produce -- a KEYUP
    // for a key that is still down, which is precisely the Wayland defect being fenced.
    void setHeldKeyQuery(std::function<bool(int)> q) { m_heldKeyQuery = std::move(q); }
    // ⚠ THE pointer-position rule -- one function, one answer. Every gesture path in this class
    // takes its coordinate from here (directly, or through eventDocPoint()), and nothing else may
    // read Fl::event_x/y for a POSITION.
    //
    // Why it cannot just be Fl::event_x/y: the canvas is a child Fl_Window, and FLTK translates
    // the event pair into a sub-window's frame *only for the duration of its handle() call* --
    // Fl_Group's send() subtracts our x()/y(), calls handle(), and RESTORES the pair on the way
    // out (Fl_Group.cxx:104-108; Fl.cxx send_event() does the same for the pushed()-routed drag /
    // release). So outside that window Fl::event_x/y still carry the canvas's origin inside the
    // top-level -- exactly the chrome above and to the left of us (menu + options bar [+ tab
    // strip / ruler], and the tool rail) -- and anything reading them from the FRAME LOOP, a
    // timeout, a host callback or a KEYBOARD handler lands that whole offset in the result.
    // Public so the regression test (test_canvas_cursor.cpp) can pin both frames.
    [[nodiscard]] common::Vec2 eventLogicalPoint() const;

    // S15 Move tool. The canvas owns the pointer side (click-select via core::topmostLayerAt,
    // handle hit-testing, the TransformGesture math) and the handles overlay; the host supplies
    // the document and lands the results: selectLayer mirrors a canvas click into the Layers
    // panel, setTransforms pushes ONE coalescing SetTransformsCommand for the whole move
    // selection (S15-c shift-click multi-select) + queues a recomposite.
    struct MoveToolHost {
        std::function<core::Document*()> document;
        std::function<void(core::LayerId)> selectLayer;
        std::function<void(const std::vector<std::pair<core::LayerId, common::Affine2D>>&,
                           std::uint64_t)>
            setTransforms;
        // Fired when a gesture that actually pushed a transform ends: the host refreshes
        // derived views (panel thumbnails) once, instead of per drag frame.
        std::function<void()> gestureEnded;
        // Fired whenever the move selection set changes (S15-c) so the host can mirror it into the
        // Layers panel (highlight all selected rows; gate the blend/opacity strip on multi-select).
        std::function<void(const std::vector<core::LayerId>&)> selectionChanged;
        // Fired when a transform gesture is refused because a target layer is locked. The canvas
        // shows nothing on its own (unlike the brush, which has a padlock reticle), so a silent
        // no-op would read as a broken Move tool -- the host says so in the status bar.
        std::function<void()> lockedAttempt;
    };
    void setMoveToolHost(MoveToolHost host) { m_moveHost = std::move(host); }
    // S15-b event-driven frame kicks: fired when canvas-affecting input lands (drag state
    // recorded, pan/zoom/rotate, a selection-mask change) so the host can run its frame loop
    // immediately instead of letting the input wait out the free-running ~60 Hz tick. The
    // host guards against storms (at most one immediate kick per frame interval).
    void setFrameRequestCallback(std::function<void()> cb) { m_requestFrame = std::move(cb); }
    // The single layer a Move drag is transforming right now (kInvalidLayerId when no drag is
    // live OR when several layers move at once). The host keys its drag-scoped composite cache
    // (render::DragCompositeCache) on this; the cache only models one moving layer, so a
    // multi-selection drag falls back to the full composite (returns invalid here).
    [[nodiscard]] core::LayerId activeDragLayer() const noexcept {
        if (m_transform.active() && m_moveGesturePushed && m_moveTargets.size() == 1)
            return m_moveTargets.front();
        // The Type-edit box Move/Rotate drag transforms the single edited text layer the same way the
        // Move tool does (only its transform changes), so it can ride the same drag-scoped cache. The
        // resize handle (ResizeBR) edits the BLOCK each frame, so it can't -- it stays a full recomposite.
        if (m_textBoxLatched &&
            (m_textBoxCtl == TextBoxControl::Move || m_textBoxCtl == TextBoxControl::Rotate))
            return m_textEditTarget;
        return core::kInvalidLayerId;
    }
    // Whether a Move transform gesture is in flight (single OR multi-layer). The host passes this
    // as CompositeOptions::liveDrag so the per-frame recomposite resamples cheaply (Auto ->
    // Bilinear) during the drag and snaps back to full quality once the gesture commits.
    [[nodiscard]] bool transformGestureActive() const noexcept {
        return m_transform.active() || m_textBox.active();
    }
    // Drop the handles target and abort any transform drag, restoring the layer's base
    // transform (tool switch, new document).
    void clearMoveTarget();
    // Land the frame-coalesced Move drag (at most one transform per frame tick). The frame
    // loop calls this BEFORE its recomposite check so the composite shown is current-frame;
    // FL_DRAG events only record the cursor state (pushing per event re-ran the full
    // composite backlog and made drags lag, S15 bug).
    void flushMoveDrag();

    // ---- Document guides (View -> Guides) ----
    // Pull a new guide off a ruler. The ruler owns the pointer capture and feeds document
    // coordinates here; `horizontalGuide` = a horizontal line (constant Y), pulled from the top
    // ruler. No-op while the active document's guides are locked. The guide previews on-canvas until
    // commitGuideCreate lands it (one AddGuideCommand) or cancelGuideCreate discards it.
    void beginGuideCreate(bool horizontalGuide, double docPos);
    void updateGuideCreate(double docPos);
    void commitGuideCreate();
    void cancelGuideCreate();
    // Whether guide interaction is possible on the active document (guides shown + not locked): the
    // ruler asks before starting a drag-out, and the canvas gates grabbing existing guides on it.
    [[nodiscard]] bool guidesInteractive() const;

    // ---- Snapping (View -> Snap) ----
    // Snap the Move tool's drag to guides, the canvas edges + centre, and other layers' bounds. All
    // runtime toggles (not persisted). setSnapEnabled is the master switch; the three targets gate
    // their candidate sources.
    void setSnapEnabled(bool on) noexcept { m_snapEnabled = on; }
    void setSnapToGuides(bool on) noexcept { m_snapToGuides = on; }
    void setSnapToCanvas(bool on) noexcept { m_snapToCanvas = on; }
    void setSnapToLayers(bool on) noexcept { m_snapToLayers = on; }
    // View -> Smart Guides: while dragging a layer, show dynamic alignment lines (magenta) where its
    // edges/centres line up with other layers' or the canvas's, and snap to them -- even when the
    // master Snap toggle is off (the Figma/Illustrator behaviour). Runtime toggle, not persisted.
    void setSmartGuides(bool on) noexcept { m_smartGuides = on; }

    // S16 Crop tool. The canvas owns the staged rect (lazily the full canvas until a gesture
    // touches it) and the pointer side -- draw/move/resize via the S15 handle hit-testing, the
    // CropGesture math, Esc reset / Enter / double-click apply; the host supplies the document
    // and lands the crop (render::buildCropCommand) when `apply` fires. `rectChanged` reports
    // the staged rect for the status bar's live size readout (nullopt = no rect staged).
    struct CropToolHost {
        std::function<core::Document*()> document;
        // Land the staged crop. `angle`/`pivot` describe a ROTATED box (S16-f rotate): the rect
        // lives in the document plane rotated by `angle` radians about `pivot`; angle 0 = the
        // familiar axis-aligned crop.
        std::function<void(common::Rect, double angle, common::Vec2 pivot)> apply;
        std::function<void(const std::optional<common::Rect>&)> rectChanged;
        // Fires when the staged crop's ROTATION changes. The host greys the Fill combo's
        // Inpaint entry while angle != 0 — rotation and content-aware fill are mutually
        // exclusive by construction ("Inpaint (unavailable when rotated)"). ⚠ That exclusion
        // is a hard constraint on the crop tool, not a UI convenience: do not relax it.
        std::function<void(double angle)> angleChanged;
        std::function<bool()>
            metricUnits; // true = cm, false = in (the crop size HUD's unit, S16-e)
        // S16-f Smart Resize: the host analyses the composite and returns THE suggested crop
        // window for the given w/h aspect (one window, deterministic — a recorded
        // guardrail; aspect <= 0 = Free = smart TRIM), or
        // nullopt when unavailable. `protectRects` are the ENABLED keep-region chips (doc
        // space): never sliced. `excludeRects` are the toggled-OFF chips: actively ignored
        // (their importance is masked out of the search).
        std::function<std::optional<common::Rect>(double targetAspect,
                                                  const std::vector<common::Rect>& protectRects,
                                                  const std::vector<common::Rect>& excludeRects)>
            smartRect;
        // The automatic keep-regions of the current composite (doc space, snug boxes; Smart
        // Recompose plan §1) — the canvas shows them as chips and feeds the enabled ones back
        // through smartRect. Same cached analysis as smartRect on the host side.
        std::function<std::vector<common::Rect>()> keepRegions;
        // Smart Recompose (plan §1.3–§1.4). recomposeOffer fires when the Recompose button's
        // enable state changes: true exactly when the enabled chips cannot all fit ANY crop
        // window at the chosen aspect but a rigid placement IS feasible (the offer; the USER
        // still invokes it — guardrail 4). The review trio drives the post-run preview: nudge
        // reports placement `index`'s snug top-left moved to `topLeft` (PREVIEW space; the host
        // re-assembles), apply/cancel land or drop the result (Enter/Esc + the bar buttons).
        std::function<void(bool)> recomposeOffer;
        std::function<void(std::size_t index, common::Vec2 topLeft)> reviewNudge;
        std::function<void()> reviewApply;
        std::function<void()> reviewCancel;
    };
    void setCropToolHost(CropToolHost host) { m_cropHost = std::move(host); }
    // Smart Recompose review (plan §1.4): while active the canvas is modal to the review — the
    // host has swapped the displayed image for the assembled preview, the placements show as
    // draggable kept-green chips (dragging fires reviewNudge), Enter/Esc fire reviewApply/
    // reviewCancel, and every other editing gesture is suppressed (pan/zoom stay live).
    // `placements` are the regions' snug rects in PREVIEW space.
    void enterRecomposeReview(std::vector<common::Rect> placements);
    void exitRecomposeReview();
    [[nodiscard]] bool recomposeReviewActive() const noexcept { return m_recomposeReview; }
    // The Recompose button's ask: the chosen aspect + the enabled keep-region chips (doc space),
    // or nullopt when it does not apply (tool/toggle off, Free ratio, no enabled chips).
    [[nodiscard]] std::optional<std::pair<double, std::vector<common::Rect>>>
    recomposeRequest() const;
    // Drop the staged rect + abort any crop drag (new document, after apply, Esc on a full rect).
    void resetCropTool();
    // Abort a half-built crop drag but KEEP the staged rect, so re-entering the tool restores the
    // same framing (tool switch). Pairs with ensureCropRect().
    void cancelCropGesture();
    // Stage a ratio-conformed full-canvas rect ONLY if none is staged yet (first entry / post-apply
    // / post-reset). An existing staged rect persists -- re-entry does not snap it back to full.
    void ensureCropRect();
    // The Crop tool's options changed (ratio preset / swap): re-conform a staged rect.
    void cropOptionsChanged();
    // Options-bar Apply: land the staged crop (same as Enter). No-op off the Crop tool.
    void commitCrop();
    // Options-bar Cancel: reset the framing to the (ratio-conformed) full canvas (same as Esc).
    void cancelCrop();
    // S16-q: what the Crop tool stages when first picked (read by ensureCropRect). WholeCanvas (the
    // default / industry behaviour) frames the whole canvas; Inset frames it with a fixed 15%
    // margin (kCropInsetFraction); DrawToBegin stages nothing GIMP-style, so the resting box is
    // suppressed and the first drag draws the rect. A live change applies the next time the tool
    // stages a rect (re-entry / new document), like S16-p's post-apply switch.
    enum class CropFraming { WholeCanvas, Inset, DrawToBegin };
    void setCropFraming(CropFraming f) { m_cropFraming = f; }

    // S35-b Mesh Warp / Perspective Warp (docs/warp-tools.md §5). The canvas owns the WHOLE gesture
    // -- binding the active layer, the staged lattice, the handle drags, the live pixel preview and
    // the overlay chrome -- and the host owns only what needs the document: which layer is active,
    // landing the finished warp as one core::SetLayerWarpCommand, and saying the refusals out loud.
    //
    // The live preview writes the deformed pixels straight into the bound layer (and re-places it),
    // exactly as a brush stroke previews by painting into the live image; cancelWarpSession() puts
    // the pre-warp pixels and placement back. Every bake starts from the session's pristine base, so
    // a drag can never compound its own resampling however many frames it runs for.
    struct WarpToolHost {
        std::function<core::Document*()> document;
        std::function<core::LayerId()> activeLayer; // the layer the warp deforms
        // Land ONE undoable warp: the layer's new whole-image pixels, its new placement (the warp's
        // own offset already folded in) and the grid to store on it for re-entry.
        std::function<void(core::LayerId, common::Image, common::Affine2D, core::WarpGrid)>
            commitWarp;
        // A preview bake landed: recomposite. Whole-document on purpose -- a warp moves the layer's
        // EXTENT and its placement, so the region that needs redrawing is where it was plus where it
        // now is, which is the same reason SetLayerWarpCommand names no dirty rect.
        std::function<void()> previewChanged;
        // A refusal, in the status bar. The canvas supplies the WORDS: the reasons are the tool's
        // own (no layer / no pixel grid / rasterize first / locked / masked / the quad folded over),
        // and one generic "cannot warp that" would be exactly the message S36 replaced.
        std::function<void(const std::string&)> refuse;
    };
    void setWarpToolHost(WarpToolHost host) { m_warpHost = std::move(host); }
    // Whether one of the two warp tools is the active tool (host: options-bar gating reads this).
    [[nodiscard]] bool warpToolActive() const;
    // Options-bar Apply: bake at full quality and land the command (same as Enter). No-op off the
    // warp tools or with nothing deformed.
    void commitWarp();
    // Options-bar Cancel: put the handles back where the layer's stored warp left them, and the
    // preview pixels back with them (same as Esc).
    void cancelWarp();
    // The warp tool's options changed (Rows / Columns / Quality): re-stage the lattice at the new
    // size, dropping any staged deformation -- a 4x4 drag has no meaning on a 6x6 lattice.
    void warpOptionsChanged();
    // Bind the active layer for warping, so the handles show the moment the tool becomes active
    // (mirrors bindGradientEditToActiveLayer / the Type re-entry). Refuses -- with a specific
    // message -- when the layer cannot be warped.
    void bindWarpToActiveLayer(core::LayerId activeLayer);
    // Drop the session outright, restoring the pre-warp pixels + placement (tool switch, document
    // swap, new document). Safe when idle.
    void cancelWarpSession();
    // True while a warp handle drag is in flight -- the host renders draft quality for it, like
    // transformGestureActive().
    [[nodiscard]] bool warpGestureActive() const noexcept { return m_warpDragging; }

    // ---- Image-ops live preview (the Image Size / Canvas Size / Rotate Arbitrary panel) --------
    // The panel stages its PENDING result here and the canvas previews it on the Crop tool's
    // overlay channel. That is not a shortcut: what a resize preview has to show — the region that
    // will be discarded, and the region that will be added — is exactly what a crop box already
    // draws. Staging the right quad therefore buys the whole finished look (the dim-outside
    // shield, the green expansion wash + 45° hatching outside the document, the box outline and
    // its handles) with no new shader and no growth of the present-pass push block, which is at
    // its 128-byte guaranteed budget. The 3×3 ANCHOR grid stays in the panel: the crop channel's
    // 8 handles are corners + edge midpoints with no centre slot, and a ninth would need push
    // room there is none of.
    struct ImageOpPreview {
        long x = 0, y = 0;          // staged new-canvas top-left in CURRENT document coords; may be
                                    // negative (growing the canvas moves the origin off the image)
        std::uint32_t w = 0, h = 0; // staged canvas size, px (0 in either: nothing is drawn)
        double angleRad = 0.0;      // preview rotation about the staged rect's centre
        bool flipH = false, flipV = false;
        // Image Size stages a RESAMPLE, not a re-framing. The panel therefore stages the document
        // at its NEW pixel size, anchored on the current origin — (0, 0, newW, newH) — and this
        // flag tells the canvas to add the two things a same-size rectangle could never show: a
        // GHOST outline of the frame the box replaces (controls mode 7; the present pass otherwise
        // never draws the document's edge as a line, only as a coverage feather), and the SCALE
        // FACTOR beside the HUD's pixel count. Canvas Size and Rotate leave it false — their rect
        // IS the new framing, and the shield + expansion wash already say the whole thing.
        bool scale = false;
        std::string hud; // one-line readout, e.g. "2048 × 1536 px"; empty = no HUD
    };
    // Stage the preview, or drop it with nullopt. Cheap and idempotent — call it on every keystroke
    // in the panel. The screen quad is re-derived from the view each frame, so the preview tracks
    // pan / zoom / view rotation like the crop box does.
    void setImageOpPreview(const std::optional<ImageOpPreview>& preview);
    // Whether a preview is staged. (Whether it is the one DRAWING is a further question — the Crop
    // tool always outranks it on the shared channel; see imageOpPreviewShowing().)
    [[nodiscard]] bool imageOpPreviewActive() const;
    // The preview's 8 handles are REAL: a corner/edge drag restages the rect exactly as the crop
    // box's handles do (CropGesture does the maths, so Shift = keep aspect and Alt = around the
    // centre come along), and the result is pushed here in whole DOCUMENT pixels — `x`/`y` the
    // staged top-left in the CURRENT document's coordinates (negative where the canvas grows past
    // the origin), `w`/`h` the staged size. Fired on every drag frame AND once more on release, so
    // the panel's fields track live and settle on exactly what is drawn.
    //
    // The PANEL stays the single source of truth: the canvas never mutates the staged preview
    // itself, it only reports, and what is drawn is whatever the panel last handed to
    // setImageOpPreview. That is what lets the panel's own rules (the proportions lock, the anchor
    // grid, a clamp) win a drag without the two ever fighting — and the drag maths work from the
    // rect latched at the PRESS, so a re-stage arriving mid-drag can never make it accumulate.
    //
    // A drag never fires for a preview whose rect is DERIVED rather than authored — a rotated or
    // flipped one — because those draw no handles at all (see the Locked channel). At exactly 0°
    // the canvas cannot tell a Rotate preview from a Canvas Size one, so a panel in Rotate mode
    // must ignore this callback rather than rely on it staying quiet.
    void setOnImageOpPreviewDrag(
        std::function<void(long x, long y, std::uint32_t w, std::uint32_t h)> cb) {
        m_onImageOpPreviewDrag = std::move(cb);
    }

    // S19-a Brush tool. The canvas owns the pointer side + the stroke (the core::brush CPU engine,
    // live preview, the circular reticle) and the host supplies the document, the layer to paint,
    // the active colour, and lands the result. `commitStroke` pushes one undoable
    // SetLayerPixelsCommand per stroke (a stroke is one undo step); `previewChanged` recomposites
    // the in-flight stroke (frame-coalesced). The reticle/engine are reused as-is by the
    // eraser/heal/inpaint brushes.
    struct BrushToolHost {
        std::function<core::Document*()> document;
        std::function<core::LayerId()> activeLayer; // the layer the stroke paints onto
        std::function<common::Color8()> foreground; // the active paint colour
        // A dab landed: recomposite this frame. `docRect` is the document-space bounding box of the
        // pixels touched since the last preview, so the host can recomposite just that region
        // (S60-a) instead of the whole document. Empty rect => nothing changed (skip).
        //
        // `layer` + `layerRect` name the SAME edit in the layer's OWN pixel space -- the space
        // `RasterLayer::image()` is indexed in -- which is the space a device-resident upload
        // copies out of (S60-a item 13; render/tile_compositor.hpp's markLayerDirty overload).
        // They ride on this callback rather than beside it precisely so the two claims cannot
        // drift: one dab, one call, both spaces. `layer` is kInvalidLayerId when the stroke went
        // to a MASK -- a mask's pixels are not in the layer's image space at all, and the resident
        // lane's answer to a mask change is to re-send the layer whole regardless.
        std::function<void(common::Rect docRect, core::LayerId layer, common::Rect layerRect)>
            previewChanged;
        // Land one undoable edit. The image is JUST the stroke's bounding box (layer-local),
        // placed at (originX, originY) -- so the command stores only the touched region, not the
        // whole layer (S60-a). Not called when nothing was painted.
        std::function<void(core::LayerId, common::Image, long originX, long originY)> commitStroke;
        std::function<void()> lockedAttempt; // pressed to paint a locked layer: show a status hint
        // Pressed to paint a layer that isn't a raster (a vector/text layer): brushing it is
        // impossible, not merely locked, so the hint must say "rasterize first" -- a different
        // message from lockedAttempt.
        std::function<void()> unpaintableAttempt;
        // Inpaint brush (S39): the brushed region (coverage > 0 = hole) is handed back on release
        // to be filled by the inpainting engine + landed as one undoable edit. The same stroke
        // machinery serves both tools; only the release differs (paint commits pixels, inpaint runs
        // the engine).
        std::function<void(core::LayerId, core::Selection)> commitInpaint;
        // The Brush tool's active preset, resolved ONCE when the user picked it (ui/brush_presets.hpp)
        // -- its tip, its option pipeline, its spacing cadence, its blend/paint mode and its masking
        // brush. NULL, or absent, means the engine's own analytic round tip: exactly what the Brush
        // painted before presets existed, and still the default.
        //
        // ⚠ Consulted for the BRUSH ALONE (§8.2). The Inpaint brush's dabs are not paint, they are the
        // hole MASK the solver fills -- a scattered, textured tip would mark a region the reticle never
        // promised. Heal/Clone/Smudge keep a plain round tip for the same reason.
        std::function<const core::brush::BrushParams*()> brushPreset;
        // §8.4: the Eraser's OWN preset -- one of the erasers the Brush is never offered (a preset
        // carrying CompositeOp=erase). A separate slot, not the Brush's: switching tools must not
        // silently change which brush you were painting with.
        std::function<const core::brush::BrushParams*()> eraserPreset;
        // S31 mask painting: when this reads true, the Brush and Eraser write the ACTIVE layer's
        // MASK coverage instead of its pixels (the dock's click-the-mask-thumbnail aim,
        // LayerPanel::maskEditTarget). The Inpaint brush ignores it -- it repairs CONTENT.
        std::function<bool()> maskTarget;
        // Land one undoable mask edit (SetMaskPixelsCommand): w x h coverage bytes at mask-local
        // (originX, originY) -- the mask lane's commitStroke. Not called when nothing changed.
        std::function<void(core::LayerId, std::vector<std::uint8_t>, std::uint32_t, std::uint32_t,
                           long, long)>
            commitMaskStroke;
        // `commitStroke`'s grown sibling: the stroke ran off the layer's own pixel grid, so the
        // grid GREW to take it -- bounded by the canvas (core/layer_grow.hpp). Growth and paint are
        // ONE undo step (core::GrowAndPaintLayerCommand): the layer's image is re-homed into a
        // `newW` x `newH` grid with its old top-left at (offsetX, offsetY), the placement absorbs
        // the shift so nothing already painted moves, and the region lands at (originX, originY) in
        // the NEW grid's coordinates. Absent, or unreachable, means a stroke simply clips at the
        // layer edge, exactly as it did before auto-grow existed.
        std::function<void(core::LayerId, std::uint32_t newW, std::uint32_t newH, long offsetX,
                           long offsetY, common::Image region, long originX, long originY)>
            commitGrownStroke;
    };
    void setBrushToolHost(BrushToolHost host) { m_brushHost = std::move(host); }
    // Abort an in-flight brush stroke and restore the pre-stroke pixels (tool switch / new
    // document).
    void cancelBrushStroke();

    // S26 Shape tool. The canvas owns the pointer side: a press->current drag (in document space)
    // builds a parametric shape via ui::buildShapeDraft (the pure math). Shift/Alt constrain /
    // centre the drag; the active shape variant + its options select the primitive.
    //
    // S26-c: WHILE THE DRAG RUNS THE SHAPE IS ONLY AN OUTLINE. The canvas draws the draft's
    // silhouette as a wireframe on the overlay lane (shapeOutlineScreenPolyline, riding the lasso
    // polyline channel like the crop rect and the Type frame do) and touches the document not at
    // all. On RELEASE the host spawns the real, FILLED VectorLayer from that same draft and bakes
    // it into one undoable command -- exactly one undo step for the whole gesture, as before.
    struct ShapeToolHost {
        std::function<core::Document*()> document;
        std::function<common::ColorF()> foreground;  // the authored fill (a line's stroke) colour
        std::function<common::ColorF()> background;  // recolours a pre-S26-c shape's outline (§7.1)
        // Create the shape's VectorLayer from `draft` and recomposite. Called ONCE, on release,
        // directly before commitShape; the layer is inserted outside the command stack so the pair
        // lands as a single undo step.
        std::function<void(const ShapeDraft&)> spawnShape;
        // Bake the spawned layer into one undoable command named `name` (the shape variant).
        std::function<void(const std::string& name)> commitShape;
        // Drop a spawned-but-uncommitted layer (no commit sink / Esc / tool switch / new document).
        std::function<void()> cancelShape;
        // Select-to-edit (§7.1): land a live edit of an existing shape's object as a coalesced
        // SetVectorObjectCommand (same coalesce id within one edit session = one undo step). The
        // optional `placement` sets the layer transform in the SAME step -- the parametric resize
        // (§7.1 resize-vs-transform) scales the size params AND re-anchors the layer; nullopt (the
        // options-bar / colour edits) leaves the transform untouched.
        std::function<void(core::LayerId, core::vec::Object, std::optional<common::Affine2D> placement,
                           std::uint64_t coalesce)>
            editShape;
        // Resize-vs-transform (§7.1): TRANSFORM-mode handle drags (and the rigid body move / rotate)
        // write the layer transform as a coalesced SetTransformCommand -- the stroke distorts with
        // the box, unlike a parametric resize.
        std::function<void(core::LayerId, common::Affine2D, std::uint64_t coalesce)> transformShape;
    };
    void setShapeToolHost(ShapeToolHost host) { m_shapeHost = std::move(host); }
    // Abort an in-flight shape drag and drop its preview (tool switch / new document / Esc).
    void cancelShapeGesture();

    // S28 Pen / custom path tool. The canvas owns the pointer side ENTIRELY -- authoring clicks and
    // handle drags, the node/handle editor on a committed path, and the overlay chrome -- and the
    // host owns the document side, in the same division the Shape tool uses.
    //
    // ⚠ COORDINATES. Every pen point is taken with eventDocPoint() / eventLogicalPoint(), i.e.
    // INSIDE this widget's own handle() call, where FLTK has translated the event pair into the
    // canvas's frame (see eventLogicalPoint()'s note). Nothing here may be driven from the main
    // window's handle(): a press intercepted up there carries window-relative coordinates, and
    // mixing one with a canvas-relative drag misplaces the whole path by the canvas's origin --
    // the menu bar + options bar above it and the tool rail to its left.
    //
    // The finished path is a ShapeDraft, so it lands through the SAME spawn/commit pair the Shape
    // tool uses (a direct insert followed by one AddLayerCommand = exactly one undo step).
    //
    // ⚠ MULTI-SUBPATH — where the line is. Layer ▸ Combine Paths commits a BAKED, multi-subpath
    // core::vec::Path, so penToolBinds() accepts it and the whole EDITING half works across every
    // contour: the spine draws them all (penPathPolyline emits each behind a kPolylineBreak
    // sentinel), the chrome marks every node of every subpath, penHitTest picks across all of them,
    // and every edit — move an anchor, pull a handle, split a segment, delete a node, toggle a
    // cusp — is addressed by (subpath, node) and lands in the contour the user actually grabbed. A
    // node ADDED while editing is a segment split, so it too goes into that segment's own subpath.
    //
    // What is NOT here: AUTHORING a further contour into a bound path. PenGesture builds exactly one
    // subpath, and a press that misses the bound path drops the binding and starts a fresh path in a
    // NEW layer. That is the honest behaviour rather than a silent wrong-subpath append — but it
    // does mean "draw another loop into this same path object" is not yet a gesture the tool has.
    struct PenToolHost {
        std::function<core::Document*()> document;
        std::function<common::ColorF()> foreground; // the fill (a stroke-only path's stroke)
        std::function<common::ColorF()> background; // the outline when a fill is present too
        // Create the path's VectorLayer from `draft` and recomposite. Called ONCE, on finish,
        // directly before commitPath; the layer is inserted outside the command stack so the pair
        // lands as a single undo step.
        std::function<void(const ShapeDraft&)> spawnPath;
        std::function<void(const std::string& name)> commitPath;
        std::function<void()> cancelPath; // drop a spawned-but-uncommitted layer
        // Land a node/handle edit of a bound path as a coalesced SetVectorObjectCommand (one undo
        // step per gesture). `placement` is always nullopt today -- node edits never move the layer
        // -- but the slot mirrors ShapeToolHost::editShape so the two hosts stay the same shape.
        std::function<void(core::LayerId, core::vec::Object,
                           std::optional<common::Affine2D> placement, std::uint64_t coalesce)>
            editPath;
    };
    void setPenToolHost(PenToolHost host) { m_penHost = std::move(host); }
    // Finish an open pen path: the authored nodes become a VectorLayer in one undo step (Enter,
    // Esc, a double-click, or a tool switch -- Illustrator's rule that Esc ENDS a path rather than
    // discarding it). A no-op when nothing is being authored; safe to call at any time.
    void commitPenPath();
    // Drop the in-flight path outright, authoring nothing (new document / document swap).
    void cancelPenGesture();
    // The committed path layer bound for node editing (kInvalidLayerId when none).
    [[nodiscard]] core::LayerId penEditTarget() const noexcept { return m_penEditTarget; }
    // Drop that binding (the active tool moved away from the Pen / a new document).
    void cancelPenEdit();
    // An options-bar value changed: re-paint the bound path live (no-op with nothing bound). The
    // colour-swatch twin recolours it, mirroring onShapeOptionsEdited / onShapeColorEdited.
    void onPenOptionsEdited();
    void onPenColorEdited();

    // S22 Gradient tool. A drag on the canvas lays down a full-bleed gradient VectorLayer (an
    // editable, maskable "gradient layer" -- docs/vector-model.md §1); re-selecting the tool on that
    // layer re-shows the on-canvas handles so the geometry can be re-dragged. The canvas owns the
    // pointer side (the press->current drag, the axis/handle gizmo, hit-testing) and reads the shape
    // + opacity off the options bar; the host owns the document side (the working stops/spread, the
    // live preview layer, and the undoable commands). Mirrors ShapeToolHost.
    struct GradientToolHost {
        std::function<core::Document*()> document;
        // The tool's working ramp -- only its stops + spread are read (the type/transform come from
        // the drag). app_window owns it; the "Stops…" flyout edits it.
        std::function<core::vec::Gradient()> workingGradient;
        // Create-or-update the live preview gradient layer to match `draft` at layer `opacity`
        // (0..1), and recomposite.
        std::function<void(const GradientDraft&, double opacity)> previewGradient;
        // Bake the live preview into one undoable Add-Layer command.
        std::function<void()> commitGradient;
        // Drop the live preview layer (sub-pixel release / Esc / tool switch / new document).
        std::function<void()> cancelGradient;
        // Land a live edit of an existing gradient layer's object as a coalesced
        // SetVectorObjectCommand (same coalesce id within one handle drag = one undo step).
        std::function<void(core::LayerId, core::vec::Object, std::uint64_t coalesce)> editGradient;
    };
    void setGradientToolHost(GradientToolHost host) { m_gradientHost = std::move(host); }
    // Abort an in-flight gradient drag and drop its preview (tool switch / new document / Esc).
    void cancelGradientGesture();
    // Drop the gradient edit target (active tool changed away / new document), hiding its handles.
    void cancelGradientEdit();
    // If the panel's active layer is a gradient VectorLayer, bind it as the edit target so its
    // handles show the moment the Gradient tool becomes active (mirrors the Type/Shape re-entry).
    void bindGradientEditToActiveLayer(core::LayerId activeLayer);
    // The layer currently bound for gradient editing (kInvalidLayerId when none).
    [[nodiscard]] core::LayerId gradientEditTarget() const noexcept { return m_gradientEditTarget; }
    // Start a new gradient-edit undo session, from the SAME monotonic sequence the handle drags use
    // (so a flyout stops-edit never collides/merges with a handle drag on the same layer). (S22)
    [[nodiscard]] std::uint64_t beginGradientEditSession() noexcept { return ++m_gradientEditCoalesce; }

    // S29-b Type tool. The canvas owns the on-canvas editing session (caret/selection, key input,
    // the blinking-caret + selection-highlight overlay); the host owns the document side: the font
    // stack (so the canvas can lay out a block for hit-test/caret geometry), creating the TextLayer,
    // landing edits as coalesced SetTextCommands, and dropping an empty block on commit.
    struct TypeToolHost {
        std::function<core::Document*()> document;
        // The CharStyle a freshly-created/typed run uses, from the Type tool options + colour swatch
        // (family, size, colour). The canvas seeds new Point/Area blocks with it.
        std::function<core::text::CharStyle()> defaultStyle;
        // Lay out `block` through the app's shaper + FontDB (the canvas has no font stack) -> the
        // ShapedBlock the caret/hit-test geometry needs. Called when the edited block changes.
        std::function<core::text::ShapedBlock(const core::text::TextBlock&)> layout;
        // Create a TextLayer carrying `block`, placed by `placement` (layer-local -> document), as one
        // undoable step, and return its id (kInvalidLayerId on failure). The canvas then edits it live.
        std::function<core::LayerId(core::text::TextBlock, common::Affine2D placement)> createText;
        // Land a live edit of the block as a coalesced SetTextCommand (same id within a typing burst =
        // one undo step), mirroring editShape.
        std::function<void(core::LayerId, core::text::TextBlock, std::uint64_t coalesce)> editText;
        // Commit finished: if the block is empty, remove the layer (an empty text block is discarded,
        // §6); otherwise a no-op. Called on Esc / click-away / tool switch.
        std::function<void(core::LayerId)> finishText;
        // The Settings "new click text reuses the last text box size" toggle (§7): false (default) =
        // a click makes Point text at the size slider; true = reuse the last Area box size.
        std::function<bool()> reuseLastBoxSize;
        // Mirror a canvas-driven text selection into the Layers panel's active row -- so creating a
        // block AND select-to-edit (clicking a different existing block) both move the panel's active
        // layer to it (fixlist #4), mirroring MoveToolHost::selectLayer.
        std::function<void(core::LayerId)> selectLayer;
        // Request a recomposite (no fit). Called on edit enter/leave so an Area block's overflow
        // shows the moment you start editing and is re-clipped to the box the moment you leave (#3).
        std::function<void()> recomposite;
        // Write the text layer's transform (MOVE / ROTATE from the Type-edit box) as a coalesced
        // SetTransformCommand -- one undo step per gesture, like ShapeToolHost::transformShape. The
        // box's resize handle edits the BLOCK instead (Area frame / Point font size) via editText, so
        // the geometric and typographic edits stay separate (docs/type-tool.md §7).
        std::function<void(core::LayerId, common::Affine2D, std::uint64_t coalesce)> transformText;
        // S29-c: the caret/selection (or its style) changed -- re-sync the context bar + Type panel to
        // selectionStyle()/selectionParagraph(). Fired from the render path, gated to actual changes
        // (target, selection range, or block revision), so it catches every mutation site (keyboard,
        // mouse, undo) without instrumenting each. Also fires once when a session ends (target clears).
        std::function<void()> onSelectionChanged;
        // S29-c font-picker hover (§8): set the layer's block for DISPLAY ONLY -- no undo command --
        // refreshing its pixel cache + the composite (mirrors editText minus the command). The canvas
        // drives the transient preview through this; clearStylePreview() restores via the same call.
        std::function<void(core::LayerId, core::text::TextBlock)> previewText;
        // Spell-check (deferred §2), backed by the host's background SpellCheckWorker. spellSuggest
        // returns replacement candidates for the right-click menu; spellAddToDict (learn) and
        // spellIgnore (ignore for the session) also kick a rescan in the host so the squiggle clears.
        // spellLanguage() is the app default language behind an empty Paragraph::language. All optional
        // (unset until the worker exists); the menu shows spell items only when spellSuggest is set.
        std::function<std::vector<std::string>(const std::string& word, const std::string& language)>
            spellSuggest;
        std::function<void(const std::string& word, const std::string& language)> spellAddToDict;
        std::function<void(const std::string& word)> spellIgnore;
        std::function<std::string()> spellLanguage;
    };
    void setTypeToolHost(TypeToolHost host) { m_typeHost = std::move(host); }
    // Commit + leave any active text-edit session (tool switch / new document / Esc). Safe when idle.
    void commitTextEdit();
    // The text layer currently being edited (kInvalidLayerId when none) -- the host gates the panel.
    [[nodiscard]] core::LayerId textEditTarget() const noexcept { return m_textEditTarget; }
    // True while a type-box gesture that EDITS THE BLOCK per event is in flight (size / bend / path
    // brackets -- not Move/Rotate, which write the transform). The host renders the edited layer at
    // DRAFT quality while this holds (refreshTextCaches' draftEditing): those frames re-raster the
    // whole block per event and are replaced momentarily; the release lands one crisp pass.
    [[nodiscard]] bool textBlockEditGestureActive() const;

    // Spell-check squiggles (deferred §2): the misspelled BYTE ranges the background worker found for
    // the edited block, which syncTextOverlay maps to on-canvas wavy underlines. The host pushes the
    // latest result here from its frame-tick poll; cleared automatically when the session ends. Ranges
    // that no longer fit the (possibly shorter) block are clamped/skipped at draw time.
    void setTextMisspelledRanges(std::vector<core::text::MisspelledRange> ranges);

    // S29-c: the edited text object's axis-aligned bounding box in canvas-widget-local logical px (the
    // space Fl::event_x/y use), or nullopt when no session. The Type panel maps it to window coords to
    // flip itself away from the text it edits, so the popup never occludes what you are typing (§8.1).
    [[nodiscard]] std::optional<common::Rect> textEditScreenBounds() const;

    // S29-c: the context bar + Type panel read & write the CURRENT selection's style here (§8.2). The
    // canvas owns the block + caret/selection, so these are the single funnel the surfaces talk to.
    //
    // selectionStyle()/selectionParagraph() report the common style across the selection (with per-
    // field "mixed" flags); a default-constructed result when no session is active. With a bare caret
    // a pending style picked at it (see applySelectionStyle) shows instead of the inherited run.
    [[nodiscard]] core::text::CommonStyle selectionStyle() const;
    [[nodiscard]] core::text::CommonParagraph selectionParagraph() const;
    // The block being edited (read-only, null without a session) -- for surfaces that mirror block-
    // level state the common style doesn't carry (anti-alias mode, Point/Area frame).
    [[nodiscard]] const core::text::TextBlock* textEditBlockForUi() const { return textEditBlock(); }
    // The current caret/selection (byte offsets). Lets the bar tell a real caret/selection MOVE (start
    // a new undo group) from a content-only change at the same range (keep coalescing the drag).
    [[nodiscard]] core::text::TextSelection textSelection() const noexcept { return m_textSel; }
    // Apply a one-property edit to the edit range, landing as a coalesced SetTextCommand. The range is
    // the selection when there is one, else the WHOLE block -- a bare caret means "the whole layer"
    // (selecting a Type layer auto-enters edit at a caret, so changes then affect the whole object).
    // `coalesce` merges this edit into the previous style edit's undo step (for continuous drags).
    void applySelectionStyle(const std::function<void(core::text::CharStyle&)>& mutate,
                             bool coalesce = false);
    void applySelectionParagraph(const std::function<void(core::text::Paragraph&)>& mutate,
                                 bool coalesce = false);
    // Apply a BLOCK-level edit (anti-alias mode, frame/area -- properties the whole block shares, not a
    // run or paragraph) to the edited block, coalescing like the style edits. No-op without a session.
    void applyTextBlockEdit(const std::function<void(core::text::TextBlock&)>& mutate,
                            bool coalesce = false);
    // Transient style PREVIEW (S29-c font-picker hover, §8): apply `mutate` to the edited block for
    // DISPLAY ONLY -- no undo command -- after saving the original, so clearStylePreview() restores it.
    // Re-previewing always starts from the saved original (no compounding). The bar/panel are NOT
    // re-synced while a preview is live (it is not a committed value). No-op without a session.
    void previewSelectionStyle(const std::function<void(core::text::CharStyle&)>& mutate);
    void clearStylePreview();
    // Carry the panel's active layer into the newly-active tool on a tool switch (selection
    // continuity, user 2026-06-29): the Move tool frames `id`; the Type tool re-enters editing on it
    // when it is a text layer. A no-op for other tools / a non-text layer under the Type tool / no id.
    void selectLayerForActiveTool(core::LayerId id);

    // Select-to-edit (S26-b §7.1). The host calls onShapeOptionsEdited() when an options-bar value
    // changes (so a selected shape edits live), and cancelShapeEdit() when the active tool changes
    // away from the selected shape's kind (or on new document) so the edit target is dropped.
    void onShapeOptionsEdited();
    // The colour swatch (fg/bg) changed: recolour the shape currently selected for editing, live
    // (no-op when no shape is bound / the Shape tool isn't active).
    void onShapeColorEdited();
    void cancelShapeEdit();
    // The layer currently selected for shape editing (kInvalidLayerId when none), so the host can
    // open the shape-designer popover (§7.4) on it.
    [[nodiscard]] core::LayerId shapeEditTarget() const noexcept { return m_shapeEditTarget; }
    // Re-read the edit target's object back into the options bar (after a designer edit changed
    // params the bar mirrors, e.g. a rect's corner radius). No-op when nothing is selected.
    void reflectActiveShape();
    // Start a new shape-edit undo session and return its coalesce id, drawn from the SAME monotonic
    // sequence the bar / colour / box edits use -- so an external surface (the designer popover) can
    // coalesce its own edits without ever colliding with the canvas's on the same layer.
    [[nodiscard]] std::uint64_t beginShapeEditSession() noexcept { return ++m_shapeEditCoalesce; }

    // S39-b: while an async inpaint runs the canvas is "busy" — editing gestures (brush, selection,
    // move, crop) are blocked and the brush reticle shows the padlock (reusing the locked-layer
    // affordance), but navigation (pan / zoom / rotate) stays live so the user can look around
    // while it works. Set false again when the inpaint finishes.
    void setInpaintBusy(bool busy);
    [[nodiscard]] bool inpaintBusy() const { return m_inpaintBusy; }

    // The inpaint sample-area preview (S39): show a faint blue wash over the document-space rectangle
    // the engine analyses while a run is active; clear it when the run ends. Stored in doc space and
    // re-projected each frame so it tracks pan/zoom.
    void setInpaintSampleArea(const common::Rect& docRect);
    void clearInpaintSampleArea();

    // Tools/Lasso: round the freehand lasso's hand-drawn path (Catmull-Rom) for both the in-flight
    // preview and the committed mask. Off by default; takes effect on the next lasso (and live if
    // one is in flight). Set from the Settings toggle via MainWindow.
    void setLassoSmoothing(bool on);

    // Settings -> Appearance "Selection and reticle line": the style the present shader colours the
    // shared content-keyed overlay line with (lasso, brush reticle, Type frames + caret).
    // 0 = Classic, 1 = Rim on demand, 2 = Adaptive. Stored so a set before the renderer exists is
    // applied when it is created; live otherwise. Set from the Settings dropdown via MainWindow.
    void setOverlayLineStyle(int style);

    // Settings -> Appearance "Feathered selection indicator": how a soft-edged selection is shown.
    // 0 = Bracketing ant pair (A, the DEFAULT); 1 = True-edge ant + soft band (F). Stored so a set
    // before the renderer exists is applied when it is created; live otherwise. Set from the Settings
    // cards via MainWindow.
    void setFeatherIndicator(int style);

    // Settings hidden key `antsCirculate` (S18, §5): the ants dash along the boundary tangent instead
    // of the diagonal crawl. Stored so a set before the renderer exists is applied on create; live
    // otherwise. Off by default (the diagonal crawl stays the default).
    void setAntsCirculate(bool on);

    // Settings -> Tablet (docs/tablet.md §8). The canvas owns the tablet because it owns the surface
    // the backends bind to; the Settings pane reaches the live policy (pressure curve / range / tilt
    // offset), the detected devices and the test area's readout through here.
    [[nodiscard]] TabletInput& tabletInput() noexcept { return m_tablet; }



    // Settings -> Tablet -> Speed smoothing (§7): calibration for the `speed` sensor's EMA. Applied
    // to the engine at the START of each stroke, so a change mid-stroke never re-scales a stroke
    // that is already running.
    void setSpeedParams(const core::brush::SpeedParams& p) { m_speedParams = p; }

    // Pixel grid (S19-c): toggle the texel-boundary hairlines (View menu). Forwards to the
    // renderer, which only draws them at high zoom; a frame is requested so the change shows
    // immediately.
    void setPixelGrid(bool on);

#ifdef MOSAIC_DEBUG
    // The canvas FPS readout (Help -> Show Canvas FPS, debug builds only): the MainWindow pushes the
    // toggle + the current rate each frame; the values ride to the renderer in the next renderFrame().
    void setFpsOverlay(bool show, int fps) noexcept {
        m_fpsShow = show;
        m_fpsValue = fps;
    }
#endif

    [[nodiscard]] bool initFailed() const noexcept { return m_initFailed; }
    [[nodiscard]] const std::string& lastError() const noexcept { return m_error; }

    // Public because the main window places the body regions itself once the dock became
    // width-resizable (Fl_Group's resizable() only knows how to hand the whole delta to one child).
    // Recreates the swapchain -- callers must not churn it (see MainWindow::applyDockWidth).
    void resize(int X, int Y, int W, int H) override;

protected:
    void draw() override;
    int handle(int event) override; // pan/zoom/rotate input (S8)
    void hide() override;

private:
    void ensureRenderer();

    // ---- S8 viewport interaction ----
    void zoomAtCenter(double factor);
    void beginRotate(int x, int y);
    void updateRotate(int x, int y, bool snap);
    [[nodiscard]] double angleFromCenter(int x, int y) const;
    int onKeyDown();
    int onKeyUp();
    // "Is `key` physically held right now?" -- asked of the WINDOW SYSTEM, not of our own event
    // bookkeeping (see the m_spaceDown comment). Fl::event_key() reads the compositor's key vector
    // on Wayland and XQueryKeymap on X11; both are the live device state, which is exactly what an
    // out-of-order or invented key event cannot forge. Routed through the test seam below.
    [[nodiscard]] bool keyPhysicallyHeld(int key) const;
    // Re-read Space/R from the window system on a POINTER event. wl_keyboard_leave clears the held
    // set and sends no KEYUP at all, so a popup, a portal dialog or the compositor moving focus
    // away mid-gesture used to leave the canvas permanently in pan or rotate mode. Cheap: one
    // driver call, only when a flag is actually set.
    void resyncGestureModifierKeys();

    // ---- S13-b notifications ----
    void notifyViewChanged();           // call after any zoom/rotation mutation
    void notifyCursor(bool overCanvas); // report the event position in document coordinates
    // notifyCursor without the Fl::event_x/y read -- for the tablet sink, which already knows where
    // the pen is (sub-pixel), and on Wayland has no FLTK event to read it from at all.
    void emitCursor(bool overCanvas);
    void requestHostFrame();            // S15-b: kick the host's frame loop (input landed)

    // ---- S14 selection tools ----
    // The active tool as a gesture kind, when it is one of the four selection tools.
    [[nodiscard]] std::optional<SelectionGesture::Kind> activeSelectionKind() const;
    [[nodiscard]] const core::Selection& baseSelection() const; // empty Selection fallback
    [[nodiscard]] common::Vec2 eventDocPoint() const; // eventLogicalPoint() -> doc coords
    // The TRACKED pointer (m_cursorLogical) -> doc coords, for hover tests that may run from
    // keyboard contexts (where Fl::event_x/y are not canvas-relative -- see updateToolCursor).
    [[nodiscard]] common::Vec2 cursorDocPoint() const;
    void pushSelectionGesture(SelectionGesture::Kind kind);     // FL_PUSH dispatch
    [[nodiscard]] bool magicWandToolActive() const;            // S17: the wand is active
    void pushMagicWand(); // FL_PUSH dispatch: one wand click -> the host floods + commits
    [[nodiscard]] bool bucketFillToolActive() const;           // S21: the bucket is active
    void pushBucketFill(); // FL_PUSH dispatch: one bucket click -> the host floods + fills
    // S24 Eyedropper: a click samples the colour under the cursor into the active foreground swatch
    // (Alt/right = background), and a circular GPU loupe follows the cursor while the tool is live.
    // Like the wand, the canvas owns only the pointer side; the host owns the sampling (which image
    // -- active layer vs. composite -- and the sample-size averaging) and the commit into ColorState.
    [[nodiscard]] bool eyedropperToolActive() const;
    // Ctrl held with a stroke tool (brush/eraser/inpaint) active = the TEMPORARY eyedropper (the
    // Space-pan convention): the loupe replaces the reticle while held, a click samples, and the
    // brush returns untouched on release. Never mid-stroke, and never for any other tool family
    // (their Ctrl meanings -- selection ops, keep-chip drags -- stay theirs).
    [[nodiscard]] bool temporaryEyedropperActive() const;
    void pushEyedropper();          // FL_PUSH: sample -> commit into fg (Alt/right -> bg)
    void dragEyedropper();          // FL_DRAG: keep sampling live as the pointer moves
    void syncLoupe(bool inside);    // per frame: place/hide the loupe + refresh its readout colour
    // S18 select brush (paint-to-select): a coverage-painting drag that previews combine(base,
    // stroke, op) straight to the canvas mask (frame-coalesced) and lands ONE SetSelectionCommand on
    // release -- the SelectionGesture commit path, driven by a core::brush::MaskStroke.
    [[nodiscard]] bool selectBrushToolActive() const;
    [[nodiscard]] core::SelectOp selectBrushOp() const; // Alt = Subtract, else the setting's op (§9-B)
    void pushSelectBrush();   // FL_PUSH: begin the mask stroke
    void dragSelectBrush();   // FL_DRAG: extend it + mark the preview dirty
    void finishSelectBrush(); // FL_RELEASE: commit the combined mask (or restore on a no-op)
    // L1 edge-aware select brush: the same seed-stroke shape as the select brush (the drag paints
    // and previews the RAW trail only), but release hands the trail to the host's grow -- the one
    // edge-stopped geodesic solve -- and commits combine(base, grown, op) as ONE SetSelectionCommand.
    [[nodiscard]] bool edgeBrushToolActive() const;
    void pushEdgeBrush();   // FL_PUSH: begin the seed stroke
    void dragEdgeBrush();   // FL_DRAG: extend it + mark the trail preview dirty (no grow here)
    void finishEdgeBrush(); // FL_RELEASE: the single grow -> combine -> commit (or restore)
    // S38-b eye retouch: the same scope-stroke shape, with the correction running ONCE on release.
    [[nodiscard]] bool redEyeToolActive() const;         // either mode
    [[nodiscard]] RedEyeOptions redEyeOptions() const;   // the options-bar snapshot
    void pushRedEye();   // FL_PUSH: begin the scope stroke
    void dragRedEye();   // FL_DRAG: extend it + mark the raw-trail preview dirty
    void finishRedEye(); // FL_RELEASE: hand the scope to the host, restore the ants
    void finishSelectionGesture(); // commit via the host (or restore the document mask)
    void restoreDocumentMask();    // re-show the document's selection (preview discarded)
    // S16-i. `pointInSelection` uses core::kAntsCoverageThreshold, so "inside" means exactly the
    // region the marching ants enclose. `endNudgeSession` drops the cached base so the next arrow
    // key opens a fresh undo step.
    [[nodiscard]] bool pointInSelection(common::Vec2 doc) const;
    [[nodiscard]] bool selectionMoveHover() const; // pointer over a grabbable selection, no modifiers
    void beginSelectionMove(common::Vec2 doc);
    void finishSelectionMove();
    void cancelSelectionMove();
    void nudgeSelection(long dx, long dy);
    void endNudgeSession();
    void commitSelection(core::Selection sel, std::uint64_t coalesce, std::string_view label);
    // The tool-aware pointer: crosshair (+ op badge) for the selection tools, move/resize/
    // rotate cursors over the Move tool's controls; badges follow the live modifiers, or the
    // latched op while a gesture is running.
    void updateToolCursor(bool inside);
    // The pen's cursor for one of this file's cursor states. On native Wayland the tool cursor is
    // ours to name -- FLTK's cursor calls cannot reach it (see updateToolCursor).
    [[nodiscard]] static Fl_Cursor tabletCursorFor(int want) noexcept;

    // ---- S15 Move tool ----
    [[nodiscard]] bool moveToolActive() const;
    [[nodiscard]] bool hasMoveTargets() const noexcept { return !m_moveTargets.empty(); }
    // The current move-selection ids (the Arrange menu's align/distribute operate on them). Raw --
    // may contain ids of vanished layers; the caller validates via Document::find.
public:
    [[nodiscard]] const std::vector<core::LayerId>& moveTargets() const noexcept {
        return m_moveTargets;
    }
    // Replace the whole move selection (the marquee's own path, and S53-b's Select ▸ Select All
    // Layers). Public because a menu command is a second, equally legitimate way to choose the set
    // the Arrange menu then aligns: while this was private, that menu item could only reach the
    // Layers panel's selection, so Arrange saw nothing and the item was half a feature.
    void setMoveTargets(std::vector<core::LayerId> ids);
    // WHAT THE ARRANGE MENU ACTS ON, as far as the canvas can answer it -- the first two rungs of
    // the host's ladder, which are the two only this class can see:
    //   1. the move selection above, whenever there is one (the Move tool's marquee and shift-
    //      clicks, the Layers panel's mirrored rows, Select ▸ Select All Layers);
    //   2. otherwise the ACTIVE TOOL's own layer-scoped edit target -- the shape / path / gradient
    //      object bound for editing, or the text block being typed into.
    // Rung 2 is the fix for a silent no-op: every one of those tools CLEARS the move selection on
    // its way in (onToolChanged -> clearMoveTarget), so with anything but Move active the Arrange
    // menu saw an empty set and did nothing at all -- the user had to switch to Move and re-select
    // what was already selected. An empty result here means the canvas has no opinion and the host
    // falls through to the Layers panel's multi-selection and then the active layer alone; the
    // paint/eyedropper/selection tools bind no layer of their own and are covered by that last rung.
    // Raw ids, exactly like moveTargets(): the caller validates them against the document and drops
    // what it may not move (core::arrangeTargets).
    [[nodiscard]] std::vector<core::LayerId> arrangeTargets() const;

private:
    // The current move selection, validated against the document (dead ids dropped). The single
    // "primary" target is the last one added — what the panel highlights and what a drill resolves
    // against.
    [[nodiscard]] std::vector<core::Layer*> moveTargetLayers() const;
    [[nodiscard]] core::Layer* primaryMoveTargetLayer() const;
    // The move selection's framing box, document space: for ONE layer it is the layer's own
    // (possibly rotated) content rect via worldTransform; for several it is the axis-aligned union
    // of their content rects (`base` = identity, `content` = that doc-space union). False = nothing
    // visible to frame.
    [[nodiscard]] bool moveSelectionBox(common::Affine2D& base, common::Rect& content) const;
    // The framing box's quad in logical screen px (TL,TR,BR,BL); false = no handles. While a
    // gesture runs it follows the gesture (the box transforms rigidly), else it reads live bounds.
    [[nodiscard]] bool moveTargetCorners(std::array<common::Vec2, 4>& out) const;
    void notifyMoveSelection(); // fire selectionChanged so the host mirrors the set into the panel
    // Mutate the move selection (and mirror the panel's active layer). recompute the overlay.
    void setSingleMoveTarget(core::LayerId id);
    void addMoveTarget(core::LayerId id);    // append if absent
    void toggleMoveTarget(core::LayerId id); // shift-click: add if absent, else remove
    // Drop the ENTIRE layer selection: the move set AND the panel's single active row (a click on
    // empty canvas is "nothing is selected", not "nothing is multi-selected"). Distinct from
    // clearMoveTarget(), which deliberately leaves the active layer alone on a tool switch.
    void clearMoveSelection();
    [[nodiscard]] bool isMoveTarget(core::LayerId id) const;
    // Arm a transform gesture on the whole move selection at `docPt`. Captures the framing box and
    // each target's press-time world transform, so per-frame the gesture delta is applied to all.
    bool beginMoveGesture(TransformMode mode, int handle, common::Vec2 docPt);
    void pushMoveTool(); // FL_PUSH dispatch: controls hit or click-select
    void dragMoveTool(); // record the drag state for flushMoveDrag's frame tick
    // Apply the gesture's current box transform to every selected layer's press-time world
    // transform and push them as ONE coalescing SetTransformsCommand (parent-relative).
    void pushSelectionTransform(const common::Affine2D& boxWorld);
    void endMoveGesture(bool restoreBase); // release (commit stands) or Esc/cancel (restore)
    void resetMoveRotation(); // double-click the rotate band: snap rotation back to 0.00 deg
    static void
    clearResetHudCb(void* self); // Fl timeout: end the brief post-reset "0.00 deg" HUD flash
    [[nodiscard]] int moveCursorState() const; // hover feedback (see m_cursorState encoding)
    void syncMoveOverlay();                    // per frame: hand the handles quad to the renderer
    // The transform ANCHOR / reference point (S15+): the pivot rotation + scaling turn around, which
    // the user drags off the box centre. `out` = its position in logical screen px; false = no box.
    // Uses the live gesture box while one runs (so the anchor rides the transform), else the resting
    // frame. The pivot itself is m_transformPivotLocal (layer-local; nullopt = auto default = centre).
    [[nodiscard]] bool moveAnchorScreen(common::Vec2& out) const;
    void dragMoveAnchor(); // FL_DRAG while dragging the anchor: reposition the pivot (light-snapping)

    // ---- S15-f: the Move tool's empty-space layer marquee ----
    // A press that misses every layer AND the transform box deselects at once (the click half);
    // dragging on from there rubber-bands a rectangle and, on release, selects every layer whose
    // content it touches (core::layersInMarquee). Shift/Ctrl at the press keep the existing
    // selection so the band EXTENDS it instead of replacing it.
    //
    // The band is drawn through the SELECTION MASK lane -- setSelectionMask feeds the present
    // pass's marching ants, so the rubber band inherits the user's Settings→Appearance overlay
    // line style, feather indicator and ants mode for free, and restyles live when they change.
    // Nothing here ever reaches the command stack: the document's own mask is put straight back
    // by finishLayerMarquee / cancelLayerMarquee (the S14 gesture-abort convention).
    void beginLayerMarquee(common::Vec2 docPt, bool extend);
    void dragLayerMarquee();   // FL_DRAG: latch past the dead zone, then track the cursor
    void finishLayerMarquee(); // FL_RELEASE: gather the touched layers, restore the document's ants
    void cancelLayerMarquee(); // Esc / tool switch / document swap: drop the band, restore the ants
    void syncLayerMarqueeMask(); // per frame: rasterize the band into the selection-overlay lane
    [[nodiscard]] common::Rect layerMarqueeRect() const; // anchor..cursor, document px

    // ---- S16 Crop tool ----
public:
    [[nodiscard]] bool cropToolActive() const; // host: status-bar gating reads this
private:
    // The staged rect, or the full document while none is staged (the tool's resting state).
    [[nodiscard]] common::Rect cropRectValue() const;
    void setCropRect(const common::Rect& r); // stage + notify the host (status readout)
    [[nodiscard]] double cropRatio() const;  // the Ratio/Swap options as a w/h aspect (0 free)
    [[nodiscard]] bool smartResizeOn() const; // the Crop tool's "Smart Resize" toggle (S16-f)
    // Stage the host's Smart Resize suggestion for the current ratio (seeds the staged rect
    // like a hand-drawn one). False when it does not apply (no host/document, Free ratio).
    bool applySmartCropSuggestion();
    // Re-fetch the automatic keep-regions from the host and merge the user's enabled/disabled
    // flags (matched by rect; new regions arrive enabled). Called before each suggestion.
    void refreshSmartChips();
    // Per frame: hand the chips to the renderer (view-mapped quads), or none when the Crop
    // tool / Smart Resize / staged rect gate is not met.
    void syncSmartChips();
    // The chip under docPt (smallest wins so nested chips stay reachable), or null.
    struct SmartChip;
    [[nodiscard]] SmartChip* smartChipAt(common::Vec2 docPt);
    // The staged rect's screen quad (TL,TR,BR,BL, logical px); false = no document yet.
    [[nodiscard]] bool cropCorners(std::array<common::Vec2, 4>& out) const;
    void pushCropTool();                       // FL_PUSH dispatch: handles/body/double-click/draw
    void dragCropTool();                       // FL_DRAG: update the staged rect from the gesture
    void applyCropNow();                       // hand the staged rect to the host
    [[nodiscard]] int cropCursorState() const; // hover feedback (m_cursorState encoding)
    void syncCropOverlay();                    // per frame: hand the crop quad to the renderer
    // The image-op preview's screen quad (TL,TR,BR,BL, logical px), mapped exactly the way
    // cropCorners() maps the crop box. False when nothing usable is staged.
    [[nodiscard]] bool imageOpPreviewCorners(std::array<common::Vec2, 4>& out) const;
    // Whether the preview is the one holding the shared crop overlay channel this frame: staged,
    // non-degenerate, and the Crop tool is not itself using it. The tool always wins, so the two
    // can never fight over the lane. ⚠ This is ALSO the tool-suppression gate (a staged preview is
    // modal-ish: no tool may take a press while it is up), and gating on *showing* rather than
    // *active* is what leaves the Crop tool's own claim intact — the Crop tool holding the channel
    // is precisely what makes this false.
    [[nodiscard]] bool imageOpPreviewShowing() const;
    // Whether the preview's handles are REAL this frame: showing, and its rect is one a drag can
    // author — axis-aligned and unflipped. A Rotate-Arbitrary preview fails it: that rect is
    // derived from the angle, so a handle on it could only lie, and the channel draws none.
    [[nodiscard]] bool imageOpHandlesLive() const;
    // The staged rect as a document-space Rect (the preview's x/y/w/h); empty when nothing staged.
    [[nodiscard]] common::Rect imageOpPreviewRect() const;
    bool pushImageOpPreview();  // FL_PUSH: grab a handle (the press is swallowed either way)
    void dragImageOpPreview();  // FL_DRAG: restage from the grabbed handle + report
    void finishImageOpDrag();   // FL_RELEASE: one settling report, then the drag ends
    void cancelImageOpDrag();   // Esc: report the press-time rect back and end the drag
    void emitImageOpRect(const common::Rect& r); // snap to whole doc px + fire the callback
    [[nodiscard]] int imageOpCursorState() const; // hover/gesture cursor over the handles (-1 none)
    // ---- S35-b Mesh Warp / Perspective Warp ----
    // A session exists iff m_warpLayer resolves; everything below is a no-op without one.
    [[nodiscard]] bool warpSessionActive() const noexcept;
    [[nodiscard]] core::Layer* warpLayer() const;      // the bound layer, or null when it went away
    [[nodiscard]] WarpOptions warpOptions() const;     // the options-bar snapshot
    // The pixels a warp reads and writes for `layer`: RasterLayer::image() / MagicLayer::source(),
    // or null for a kind that owns none. The single place the two kinds are unified (its const twin
    // is core's own warpablePixels, in commands.cpp -- the same rule, on the other side of the wall).
    [[nodiscard]] static common::Image* warpablePixels(core::Layer* layer);
    // Why `layer` cannot be warped, as the SENTENCE to show, or empty when it can be. Six specific
    // refusals in the S36 house style -- each one names the way forward, because a tool that says
    // "cannot warp that" has told the user nothing they could not already see.
    [[nodiscard]] static std::string warpRefusal(core::Layer* layer);
    // The handles in LOGICAL SCREEN px, in warpHandlePoints' order (so an index means the same thing
    // to the hit test, the drag and the chrome). Empty without a session.
    [[nodiscard]] std::vector<common::Vec2> warpHandleScreen() const;
    void pushWarpTool();  // FL_PUSH: grab a handle (or, with Alt, the whole lattice)
    void dragWarpTool();  // FL_DRAG: record the drag state for flushWarpDrag's frame tick
    void finishWarpDrag(); // FL_RELEASE: one full-quality bake, then the drag ends
    void flushWarpDrag(); // per frame: at most ONE draft bake per tick, however fast the pointer
    // Bake `m_warpGrid` over the session's pristine base into the live layer (and re-place it).
    // False when the deformation was refused (a folded perspective quad, an impossible extent).
    bool bakeWarpPreview(render::WarpQuality quality);
    void restoreWarpBase();               // put the pre-warp pixels + placement back
    [[nodiscard]] int warpCursorState() const; // hover feedback (m_cursorState encoding), -1 = none
    void syncWarpOverlay();               // per frame: the lattice + its handle squares
    void updateWarpHover();               // FL_MOVE: which handle lights up (no-op when unchanged)

    void syncSampleArea();                     // per frame: hand the inpaint sample-area quad (S39)
    void syncLassoOverlay();  // per frame: hand the in-flight lasso/poly path to the renderer
    void updateOverlayTile(); // per frame: rasterize the dial/HUD text tile when it changes
    void updateIdleField();   // per frame: bake-on-demand + push the idle pass's fade state

    // ---- S33 DoF focus-band gizmo ----
    // The gizmo's screen-space frame for one query: the focus line's centre + unit direction, the
    // +band/+feather edge OFFSET vectors (the edges stay parallel to the line, but under an
    // anisotropic parent placement their offset need not be perpendicular to it), and the rotate
    // knob. Derived fresh from the provider each call.
    struct DofScreenGeom {
        common::Vec2 center;     // the centre knob, logical screen px
        common::Vec2 dir;        // unit line direction, screen space
        common::Vec2 offBand;    // the +band edge passes through center + offBand
        common::Vec2 offFeather; // the +feather edge passes through center + offFeather
        common::Vec2 rotateKnob; // center + dir * kDofRotateKnobPx
    };
    [[nodiscard]] bool dofScreenGeom(DofGizmoState& state, DofScreenGeom& out) const;
    // The handle under `screenPt` (0 centre knob, 1 rotate knob, 2 +band, 3 -band, 4 +feather,
    // 5 -feather), or nullopt. Knobs first (they sit ON the lines); band before feather so the
    // closer-in line wins ties.
    [[nodiscard]] std::optional<int> hitDofHandle(common::Vec2 screenPt) const;
    // Whether ANY pointer gesture is in flight -- the gate the tool-independent DoF hover paths
    // use, so they never speak over another tool's live gesture.
    [[nodiscard]] bool pointerGestureActive() const;
    bool pushDofHandles();   // FL_PUSH dispatch: claim a handle (false = nothing hit, fall through)
    void dragDofHandle();    // FL_DRAG: stream the edited geometry through the host (coalesced)
    void finishDofGesture(); // FL_RELEASE: the last streamed edit stands
    [[nodiscard]] int dofCursorState() const; // hover/gesture cursor (m_cursorState), -1 = none
    void syncDofOverlay();   // per frame: hand the guide lines + knobs to the renderer (binding 11)

    // ---- Document guides (View -> Guides): grab/move an existing guide + the overlay ----
    // The guide near `screenPt` (logical px) within the grab tolerance, or nullptr. Returns null
    // when guides are hidden or locked (guidesInteractive() is false).
    [[nodiscard]] const core::Guide* hitGuide(common::Vec2 screenPt) const;
    bool pushGuideGesture();  // FL_PUSH (Move tool): grab a guide to drag (false = none hit)
    void dragGuideMove();     // FL_DRAG: track the grabbed guide to the cursor (preview only)
    void finishGuideDrag();   // FL_RELEASE: land the move, or delete if dragged off-canvas
    void syncGuidesOverlay(); // per frame: hand the guide + smart-guide lines to the renderer (b.13)

    // ---- Snapping (View -> Snap) ----
    // Candidate snap lines (document space) for the current move drag: guides + canvas + other
    // layers' bounds, per the snap-target toggles. Excludes the moving layers (and their subtrees).
    [[nodiscard]] core::SnapCandidates gatherSnapCandidates(bool guides, bool canvas,
                                                            bool layers) const;
    void collectLayerSnapLines(const core::GroupLayer& group, core::SnapCandidates& cand) const;
    // Snap the move box (document space) in `boxWorld`, mutating it by a translation. No-op unless
    // snapping is enabled and the live gesture is a plain Move.
    void applyMoveSnap(common::Affine2D& boxWorld);

    // ---- S19-a Brush tool / S39 Inpaint brush (shared stamping-stroke machinery) ----
    [[nodiscard]] bool brushToolActive() const;   // the paint Brush
    [[nodiscard]] bool inpaintToolActive() const; // the Inpaint brush (red mask -> engine)
    [[nodiscard]] bool eraserToolActive() const;  // the Eraser (StrokeMode::Erase, §8.4)
    [[nodiscard]] bool cloneToolActive() const;   // the Clone stamp (S38; source pixels, not paint)
    [[nodiscard]] bool strokeToolActive() const;  // any of them: all drive one stroke gesture

    // ---- S38 Stamp / Clone (docs/clone-stamp.md §5) -------------------------------------------
    // The source-pick modifier is held (Ctrl, or Command on macOS -- FL_COMMAND resolves to the
    // platform's own). While it is, the next press picks a SOURCE instead of starting a stroke, and
    // the cursor says so.
    [[nodiscard]] bool cloneAnchorModifier() const;
    [[nodiscard]] CloneStampOptions cloneOptions() const; // the options-bar snapshot
    void pushCloneAnchor();                               // FL_PUSH with the modifier: pick a source
    // Resolve the stroke's source before the engine begins: the offset, the pre-stroke snapshots
    // and the target->source map. False = refuse the stroke (no anchor / no usable source), and the
    // caller must not begin the engine.
    [[nodiscard]] bool beginCloneStroke(core::RasterLayer& layer,
                                        const core::brush::StrokeInput& in);
    // Rewrite one freshly-composited rectangle as a CLONE deposit. Called after every
    // BrushEngine::composite() of a clone stroke, over exactly the rect that composite() reported,
    // so the engine's own paint is replaced everywhere it landed and nowhere else -- which is what
    // keeps restore()/dirtyBounds()/the undo commit working unchanged.
    void stampCloneRegion(const common::Rect& dirty);
    void clearCloneStrokeState(); // drop the stroke's snapshots (the anchor SURVIVES)
    // Where the source marker sits, document px: the live source under the cursor while a clone
    // stroke runs, the picked anchor otherwise, nullopt when nothing is picked.
    [[nodiscard]] std::optional<common::Vec2> cloneMarkerDocPoint() const;
    void syncCloneOverlay(); // per frame: the source marker, on the overlay-line channel
    // The brushed region as a hole mask (coverage > 0), built from the engine's coverage buffer --
    // what the Inpaint brush hands the engine on release.
    [[nodiscard]] core::Selection brushHoleMask() const;
    // The active layer as a raster the brush can target (null if not a raster, hidden, or no
    // document/host). A LOCKED layer is still returned -- the lock is enforced at paint time (the
    // reticle shows a padlock + the press fires a status hint), not by hiding the target.
    [[nodiscard]] core::RasterLayer* activeBrushLayer() const;
    // True when the active layer is visible but NOT a raster (a vector/text layer): the brush can't
    // paint it at all, so the reticle padlocks and a press shows the hint (S26).
    [[nodiscard]] bool activeBrushLayerUnpaintable() const;
    // True when the brush's target raster exists but is locked (drives the reticle padlock + hint).
    [[nodiscard]] bool activeBrushLayerLocked() const;
    // The S31 mask-paint lane's target: the active layer -- ANY kind -- when the dock aims edits
    // at its mask and the tool is the Brush or Eraser. nullptr = the ordinary pixel lane.
    [[nodiscard]] core::Layer* maskPaintTarget() const;
    // Begin a stroke on `layer`'s mask: the engine paints an opaque-gray RGBA PROXY of the
    // coverage (full tips/dynamics/presets work on masks for free; the eraser's destination-out
    // carves toward 0 = hides), mirrored into the live mask per pump for the composite preview.
    void beginMaskStroke(core::Layer& layer, const core::brush::StrokeInput& in);
    // Mirror a proxy region back into the live mask coverage (luma * alpha) + bump maskRevision.
    void mirrorMaskProxy(core::Layer& layer, const common::Rect& rect);
    // The active selection resampled onto a stroke's target grid (core/stroke_confinement.hpp), or
    // NULL when there is no selection -- in which case the stroke is byte-identical to one laid
    // before confinement existed. `targetToDoc` is worldTransform(layer) on the pixel lane and the
    // mask->document map on the S31 mask lane.
    [[nodiscard]] std::shared_ptr<const core::StrokeConfinement>
    strokeConfinement(const common::Affine2D& targetToDoc, std::uint32_t targetW,
                      std::uint32_t targetH) const;
    // Grow `layer`'s pixel grid so the stroke about to begin can paint off its edge -- bounded by
    // the canvas and refused outright past core::kMaxLayerCells (core/layer_grow.hpp). Records what
    // it did in m_brushGrow* so revertBrushGrowth() can put it back byte-exactly; a no-op (and NOT
    // one allocation) when the layer already covers the canvas, which is the ordinary case.
    void growBrushLayer(core::RasterLayer& layer, const core::Document* doc);
    // Undo the press-time growth: crop the (already restore()d, therefore pristine) image and mask
    // back to the pre-press grid and put the pre-press transform back. The PERMANENT growth is the
    // undoable command's, not this one's -- the live grid is only ever a working surface.
    void revertBrushGrowth(core::RasterLayer& layer);
    [[nodiscard]] bool brushLayerGrew() const noexcept;
    // Read a brush option (size/hardness/flow/opacity) from the active tool, with a fallback.
    [[nodiscard]] double brushOption(const char* id, double fallback) const;
    // Start the smoother for a stroke, reading its strength FRESH from the tool's context bar. Read
    // at the PRESS, never mid-stroke: changing the window under a filter that is averaging the last
    // N points would move the goalposts halfway through a line.
    void beginSmoothedStroke(const core::brush::StrokeInput& first);
    [[nodiscard]] core::brush::BrushParams currentBrushParams() const; // options + active colour
    // Which pressure channels this tool lets the pen drive (docs/tablet.md §10 step 5). Both flags
    // are inert for a mouse (pressure 1 scales nothing), so this changes a mouse stroke by nothing.
    [[nodiscard]] core::brush::BrushDynamics currentBrushDynamics() const;
    void pushBrushTool();               // FL_PUSH: begin a stroke (snapshot + first dab)
    void dragBrushTool();               // FL_DRAG: stamp dabs along the segment + live preview
    void finishBrushStroke();           // FL_RELEASE: end the stroke + land one undoable command
    void syncBrushReticle(bool inside); // set the renderer's reticle from the cursor + brush size

    // The stroke, driven by SAMPLES rather than by FLTK events -- the seam both tablet backends
    // meet (docs/tablet.md §10 step 5). `in.pos` is canvas-local LOGICAL (sub-pixel); the doc/layer
    // mapping happens here, once. pushBrushTool/dragBrushTool are the FLTK-event front ends.
    void beginBrushStroke(const core::brush::StrokeInput& in);
    void extendBrushSample(const core::brush::StrokeInput& in); // one sample into the live stroke
    // One sample off a backend: it BEGINS the stroke when a press is still owed (the deferred first
    // dab -- see pushBrushTool), and extends it otherwise.
    void brushSample(const core::brush::StrokeInput& in);
    // Run `feed` (which calls extendBrushSample once per sample) against the live stroke, then
    // composite and refresh ONCE for the whole batch: an X11 drain hands over a ~200 Hz burst, and
    // recompositing per sample would pay for the sampling rate ten times a frame.
    void pumpBrushStroke(const std::function<void()>& feed);
    void ensureTabletInput(); // bring the backend up once the window is shown (needs its handle)

    // S26 Shape tool: a press->current drag authors a parametric VectorLayer.
    [[nodiscard]] bool shapeToolActive() const;
    void pushShapeTool();                                 // FL_PUSH: anchor the drag
    void dragShapeTool();                                 // FL_DRAG: re-latch the drag, redraw wire
    void finishShapeTool();                               // FL_RELEASE: spawn + commit (or nothing)
    [[nodiscard]] ShapeOptions activeShapeOptions() const;       // tool options + foreground colour
    [[nodiscard]] std::optional<ShapeDraft> currentShapeDraft() const;  // press + latched drag state
    // Latch the live drag point + modifiers at EVENT time. The wireframe is rebuilt from the RENDER
    // path, a frame later, where Fl::event_* no longer describes this drag -- the same event-time
    // capture the Type tool's Area create-drag uses (m_textCreateDragDoc).
    void captureShapeDrag();
    // The in-flight shape's wireframe in logical screen px (empty when no drag is latched): the
    // draft's silhouette, ready for the overlay's polyline lane. Subsampled to its vertex budget.
    [[nodiscard]] std::vector<common::Vec2> shapeOutlineScreenPolyline() const;
    void syncShapeOverlay(); // per frame: hand that wireframe to the renderer (after the lasso's)
    // Select-to-edit helpers (§7.1): pick an existing shape under the press to edit it (returns true
    // when one was picked, so the press does NOT start a new shape), switch the Shape tool to its
    // kind + load its parameters into the options bar.
    [[nodiscard]] bool pickShapeForEdit();
    void reflectShapeOptions(const core::vec::Object& obj); // write params into the active tool + sync

    // Resize-vs-transform (§7.1): the selected shape's on-canvas selection box + handles. A press on
    // a handle resizes the size params (default) or transform-scales the layer (the "Transform"
    // toggle); the body moves it; the corner band rotates it. Mirrors the Move tool's gesture, but
    // bound to the single edit-target shape and routed through the shape host.
    [[nodiscard]] bool shapeEditActive() const;             // Shape tool + a valid edit target
    [[nodiscard]] bool shapeTransformMode() const;          // the "transform" tool toggle is on
    [[nodiscard]] bool shapeBoxCorners(std::array<common::Vec2, 4>& out) const; // box, logical screen px
    void pushShapeBoxGesture(const TransformHit& hit, common::Vec2 docPt);      // arm a box gesture
    void dragShapeBox();                                    // record the drag for the frame flush
public:
    void flushShapeBoxDrag();                               // land ONE coalesced edit per frame tick
private:
    void endShapeBoxGesture();                              // commit + start the next undo step
    void cancelShapeBoxGesture();                           // Esc / tool switch: restore the press state
    [[nodiscard]] int shapeBoxCursorState() const;          // hover cursor over the box controls

    // ---- S28 Pen / custom path tool ----
    // Authoring and node editing are ONE tool with two states: with nothing bound a press places
    // nodes; with a committed path bound a press grabs one of its nodes/handles/segments. Both
    // take their points from eventDocPoint() -- inside handle(), never from the window.
    [[nodiscard]] bool penToolActive() const;
    [[nodiscard]] PenOptions activePenOptions() const;   // tool options + the active colours
    void pushPenTool();     // FL_PUSH: place/close a node, or grab a bound path's node/handle
    void dragPenTool();     // FL_DRAG: pull the live handles, or move the grabbed node/handle
    void finishPenDrag();   // FL_RELEASE: the node stands; a node edit's coalesced step closes
    void movePenTool();     // FL_MOVE: the rubber-band target (latched at event time)
    [[nodiscard]] bool penGestureActive() const noexcept;  // authoring OR editing is in flight
    // Bind the committed path under the press for node editing (true = bound, so the press must
    // not start authoring a new path). Mirrors pickShapeForEdit.
    [[nodiscard]] bool pickPathForEdit();
    [[nodiscard]] core::VectorLayer* penEditLayer() const; // the bound layer (null when gone)
    // The bound layer's world transform and its inverse; false when unavailable/singular.
    [[nodiscard]] bool penEditFrame(common::Affine2D& world, common::Affine2D& inv) const;
    // The grab tolerance in LAYER-LOCAL units: kPenPickScreenPx of SCREEN pixels divided by the
    // view zoom (-> document px) and then mapped through the layer's inverse world transform. It
    // has to be derived that way round or the pick band would shrink/grow with the zoom, which is
    // the same coordinate-level mistake as reading a press in the wrong widget's frame.
    [[nodiscard]] double penPickLocal(const common::Affine2D& worldInv) const;
    void applyPenEdit(const core::vec::Path& next, bool newUndoStep); // land a coalesced edit
    void reflectPenOptions(const core::vec::Object& obj); // write the bar from a bound path
    void syncPenOverlay();     // per frame: the path spine on the polyline lane (authoring OR edit)
    void syncPenChrome();      // per frame: the nodes/handles/stems on the pen lane (binding 6)
    // The path the chrome is drawn from, its selected node, and the transform that carries it to
    // logical screen px: the in-flight authored path (DOCUMENT space) or the bound layer's path
    // (LAYER-LOCAL). False when the Pen is not the active tool or nothing is bound.
    [[nodiscard]] bool penChromeSource(core::vec::Path& src, PenSelection& sel,
                                       common::Affine2D& pathToScreen) const;
    void updatePenHover();     // FL_MOVE in edit mode: which knob lights up (no-op when unchanged)
    // Drop every (subpath, node) address held here -- selection, grab and hover -- after an edit
    // that changed the path's STRUCTURE. Mandatory on a multi-subpath path: deleting a contour
    // renumbers the ones after it, so a kept address silently starts naming a different curve.
    void clearPenAddresses();
    // The path's SPINE in logical screen px, with kPolylineBreak between contours: the in-flight
    // path while authoring, the BOUND path while editing (edit mode used to show no spine at all).
    // Empty when the Pen is not the active tool, or has nothing to draw.
    [[nodiscard]] std::vector<common::Vec2> penOutlineScreenPolyline() const;
    [[nodiscard]] bool penDeleteSelectedNode();  // Delete/Backspace on a bound path's selected node

    // S22 Gradient tool: a press->current drag lays down a full-bleed gradient VectorLayer, and its
    // on-canvas axis/handle gizmo re-drags an existing gradient layer.
    [[nodiscard]] bool gradientToolActive() const;
    [[nodiscard]] GradientShape activeGradientShape() const;     // the "type" option -> a shape
    [[nodiscard]] double activeGradientOpacity() const;          // the "opacity" option, 0..1
    [[nodiscard]] std::optional<GradientDraft> currentGradientDraft() const; // press+cursor+shift
    void pushGradientTool();                                     // FL_PUSH: grab a handle / anchor a drag
    void dragGradientTool();                                     // FL_DRAG: preview / drag a handle
    void finishGradientTool();                                   // FL_RELEASE: commit / end the session
    [[nodiscard]] bool gradientEditActive() const;               // tool active + a valid edit target
    // The bound (or in-flight) gradient's handle anchors in doc space + the world transform used; false
    // when neither a preview nor an edit target is showing. Feeds both the gizmo and hit-testing.
    [[nodiscard]] bool currentGradientHandles(GradientHandles& out) const;
    // The gradient axis gizmo in logical screen px (start, end, mid) -- false when no gradient shows.
    // `minor` carries the Elliptical minor-axis handle, and repeats `mid` for every shape that has
    // none -- which is exactly how the gizmo lane reads "there is no fourth handle".
    [[nodiscard]] bool gradientGizmoPoints(common::Vec2& a, common::Vec2& b, common::Vec2& mid,
                                           common::Vec2& minor) const;

    // S29-b Type tool: on-canvas authoring + editing.
    [[nodiscard]] bool typeToolActive() const;
    [[nodiscard]] bool textBlockUnderPointer() const; // a text block sits under Fl::event_x/y (cursor)
    void pushTypeTool();     // FL_PUSH: place the caret / select-to-edit / anchor a create gesture
    void dragTypeTool();     // FL_DRAG: extend the selection, or size an Area box
    void finishTypeTool();   // FL_RELEASE: create the Point/Area block + enter editing
    [[nodiscard]] core::Layer* textEditLayer() const;        // the edit target's layer (or null)
    [[nodiscard]] const core::text::TextBlock* textEditBlock() const; // its block (or null)
    // The byte range the bar/panel edits target: the selection, or the whole block at a bare caret.
    [[nodiscard]] std::pair<std::size_t, std::size_t> textEditRange() const;
    [[nodiscard]] const core::text::ShapedBlock& ensureTextShaped();  // (re)layout on a block change
    void enterTextEdit(core::LayerId id, std::optional<std::size_t> caret); // begin a session
    void beginTextEditFromMove(core::LayerId id, common::Vec2 docPt); // Move dbl-click -> Type edit (#8)
    void applyTextEdit(core::text::TextBlock next, bool newUndoStep);  // land a coalesced edit
    [[nodiscard]] int onTextKey();          // key handling while editing (returns 1 if consumed)
    void insertTextAtCaret(const std::string& utf8);  // typed/pasted text replaces the selection
    void deleteTextRange(std::size_t from, std::size_t to); // delete [from,to) as a coalesced edit
    // Replace the byte range [begin, end) with `replacement` as ONE discrete undo step (a fresh
    // coalesce id, distinct from surrounding typing) -- the right-click "pick a spelling" action.
    void replaceMisspelledWord(std::size_t begin, std::size_t end, const std::string& replacement);
    void copyTextSelectionToClipboard() const;        // selection -> system clipboard (Ctrl-C / menu)
    void selectAllText();                             // select the whole block (Ctrl-A / menu)
    void showTextContextMenu();                       // themed Cut/Copy/Paste/Select-All on right-click
    // doc -> the edit block's FLAT design point. Non-const: an extruded block ray-casts back
    // through the front-cap plane (ExtrudePlaneMap), which wants the freshly-shaped bounds.
    [[nodiscard]] common::Vec2 textDocToLocal(common::Vec2 docPt);
    // The 3D editing-plane map for the edited block (last-shaped basis), or nullopt when flat --
    // the caret/selection/box chrome projects through it so it hugs the solid (S30-d round 2).
    [[nodiscard]] std::optional<core::text::ExtrudePlaneMap> textPlaneMap() const;
    void moveTextCaret(std::size_t to, bool extendSelection, bool newUndoStep);
    void syncTextOverlay();  // per frame: caret bar + selection quads -> the renderer (binding 5)
    void notifyTextSelectionIfChanged(); // per frame: fire onSelectionChanged when the selection moved
    // The Type-edit box as a closed screen-px polyline (or empty): the in-flight Area create-drag box,
    // or the edited block's box (Area frame / padded Point bounds) with a bottom-right resize handle.
    // Drawn via the lasso channel's smooth inverted line (#1).
    [[nodiscard]] std::vector<common::Vec2> textAreaFramePolyline() const;
    void updateTextBlink();  // toggle the caret blink phase off the wall clock

    [[nodiscard]] bool textSessionActive() const noexcept {
        return m_textEditTarget != core::kInvalidLayerId;
    }

    // The Type-edit box (move / resize / rotate while editing, docs/type-tool.md §7). The box EDGE
    // moves the layer, the bottom-right corner resizes (Area frame reflow / Point font size), a band
    // outside the corners rotates -- the interior stays the caret. Move/Rotate write the layer
    // transform (TransformGesture, like the Shape box); resize edits the block.
    [[nodiscard]] std::optional<common::Rect> textEditBoxLocal() const; // box in layer-local space
    [[nodiscard]] bool textEditBoxCorners(std::array<common::Vec2, 4>& out) const; // -> screen px
    [[nodiscard]] int textBoxCursorState() const;  // hover/gesture cursor over the box (-1 = not on it)
    // The box corner carrying the resize handle (TL,TR,BR,BL index): BR, except vertical Point
    // text uses BL so the handle stays joined to the left-edge side baseline.
    [[nodiscard]] int textResizeCorner() const;
    // The Type baseline BEND handle (S30 §9): a grab hanging a fixed gap off the bottom bar's apex;
    // dragging it up/down bows the block's baseline into a circular arch. Shown for horizontal,
    // non-path-fitted blocks only (bend is inert otherwise); a 3D block's handle rides the
    // plane-projected bar, and the drag unprojects through the press-time cap plane. `outScreen` is
    // the pill centre (which is also its grab point); `outApex`, when given, is the point ON THE BAR
    // the stem reaches. The drop runs along the BAR'S OWN outward normal, not screen-down, so the
    // handle turns with the block, the view and the cap -- which is why the apex is returned rather
    // than re-derived by the caller.
    [[nodiscard]] bool textBendHandle(common::Vec2& outScreen,
                                      common::Vec2* outApex = nullptr) const;
    // The edit box's bottom edge as a screen-space polyline, BOWED by the block's bend so the baseline
    // bar (Point) / bottom frame line (Area) conforms to the arched text. Straight (endpoints only) when
    // unbent. Shared by the drawn bar and the bend handle (which rides its apex). False if no box.
    [[nodiscard]] bool textBottomBarScreen(std::vector<common::Vec2>& out) const;
    // The same bar in FLAT layer-local units -- before the world transform and the 3D cap
    // projection. textBottomBarScreen is a thin mapper over this; the bend handle needs the local
    // form to take its drop direction from the bar's own normal (a screen-space perpendicular
    // cannot tell "under the baseline" from "over it" through a mirroring transform).
    [[nodiscard]] bool textBottomBarLocal(std::vector<common::Vec2>& out) const;
    // A bent AREA frame's horizontal edge at layer-local depth `edgeLocalY`, sampled along the SAME
    // arc family applyBend laid the text on (BentArc::warp of the flat edge) -- the top edge at
    // box.y, the bottom at box.bottom(). False unless the block is a bent Area block with a live
    // bentArc; the callers then fall back to the straight edge. ...Local is the flat layer-space
    // form; ...Screen maps it through the world transform + cap projection.
    [[nodiscard]] bool textFrameArcEdgeScreen(double edgeLocalY, std::vector<common::Vec2>& out) const;
    [[nodiscard]] bool textFrameArcEdgeLocal(double edgeLocalY, std::vector<common::Vec2>& out) const;
    // A horizontal, bent, non-3D Point block whose visible chrome is the bent bar (so the box chrome
    // conforms to it rather than the flat box). The size handle sits at the bar's right end, Move rides
    // the bar itself.
    [[nodiscard]] bool isBentPointBlock() const;
    // The Area counterpart: a horizontal Area block with a live frame arc, whose whole frame is the
    // warped sector textAreaFramePolyline draws. Its box corners are warped too (textEditBoxCorners),
    // so handle placement, hit-testing and the drawn frame are one geometry.
    [[nodiscard]] bool isBentAreaBlock() const;
    // Hit-test the bent-Point bar chrome (screen px): ResizeBR near the bar's right end, Move near the
    // bar polyline, else None. The Bend handle is tested separately/earlier; Rotate falls back to the
    // flat corners. None for non-bent-Point blocks (the caller then uses hitTextEditBox).
    [[nodiscard]] TextBoxControl hitBentPointBar(common::Vec2 p) const;
    // The Area counterpart: ResizeBR on the warped resize corner, Move within the edge band of the
    // DRAWN frame polyline, else None (the caret interior, or clear of the frame). Rotate stays
    // hitTextRotate's one shared test. None for non-bent-Area blocks.
    [[nodiscard]] TextBoxControl hitBentAreaFrame(common::Vec2 p) const;
    // The fit-to-path range brackets (S30 §9) in screen px + their local tangent angles: [0] the
    // start bracket (arc-distance s0), [1] the end bracket (s1), [2] the centre slide/flip grip.
    // Sampled side-agnostic (unflipped) so the brackets sit ON the path; projected through the
    // plane map for a 3D block like the rest of the chrome. False when no path fit is active.
    [[nodiscard]] bool textPathBrackets(std::array<common::Vec2, 3>& outPos,
                                        std::array<double, 3>& outAngleRad) const;
    // Which bracket (if any) `p` grabs: PathStart / PathEnd / PathSlide, else None.
    [[nodiscard]] TextBoxControl hitTextPathBrackets(common::Vec2 p) const;
    // The corners the ROTATE affordance hugs, in screen px. Flat text: the edit box itself. A BENT
    // block's corners are the box's own WARPED corners -- carried through the same arc the letters
    // and chrome ride, so the hotspots sit ON the visible frame (an AABB around an arch parks its
    // corners in empty space off the arc ends); bend is tested BEFORE 3D, so a bent extruded block
    // gets its warped corners projected onto the visible cap rather than an axis-aligned extent.
    // A non-bent 3D block anchors to the rendered INK's measured bounds (cachedInkBounds;
    // projectedExtrudeBounds as the cold-cache fallback).
    [[nodiscard]] bool textRotateCorners(std::array<common::Vec2, 4>& out) const;
    // True when `p` (screen px) lands in the rotate band: outside the rotate quad, within
    // kRotateBandPx of one of its corners. The one rotate test every Type-box path shares.
    [[nodiscard]] bool hitTextRotate(common::Vec2 p) const;
    void beginTextBoxGesture(TextBoxControl ctl, common::Vec2 docPt); // arm move/resize/rotate/bend
    void dragTextBox();      // stream the gesture into a transform (move/rotate) or block edit (resize)
    void endTextBoxGesture(); // commit (the last drag stands) + start the next undo step
    [[nodiscard]] bool textBoxGestureActive() const noexcept {
        return m_textBoxCtl != TextBoxControl::None;
    }

    // Line gizmo (S26): a selected LINE shows a connector line + a square handle at each end + a round
    // handle in the middle (for bending), NOT the box. Endpoints set a/b, the mid sets the bend curve,
    // the body moves the line. Replaces the box gesture for lines.
    [[nodiscard]] bool editTargetIsLine() const;
    // The gizmo's three handles in logical screen px (from the live edit layer); false if unavailable.
    [[nodiscard]] bool lineGizmoPoints(common::Vec2& a, common::Vec2& b, common::Vec2& mid) const;
    // Which gizmo control is under `screenPt`: 0 a, 1 b, 2 mid(bend), 3 body(connector), -1 none.
    [[nodiscard]] int hitLineGizmo(common::Vec2 screenPt) const;
    void beginLineGizmoGesture(int handle, common::Vec2 docPt);
    void flushLineGizmoDrag(); // land one coalesced line edit per frame tick

    // Native-Wayland only: a dedicated child surface the swapchain presents to (see
    // platform::WaylandSubsurface). Declared before m_renderer so it is destroyed *after* it --
    // the VkSurfaceKHR built on its wl_surface must go first. Null on X11/XWayland. Absent on macOS
    // (S58) and on Windows (S57), where there is no Wayland and the class is not compiled --
    // guarded so the unique_ptr destructor does not reference the (unlinked) WaylandSubsurface
    // dtor.
#if !defined(__APPLE__) && !defined(_WIN32)
    std::unique_ptr<platform::WaylandSubsurface> m_subsurface;
#endif
    std::unique_ptr<render::WindowRenderer> m_renderer;
    common::Color8 m_clearColor{30, 33, 48, 255}; // a calm dark canvas backdrop
#ifdef MOSAIC_DEBUG
    bool m_fpsShow = false; // Help -> Show Canvas FPS (pushed each frame; debug builds only)
    int m_fpsValue = 0;     // the rate to spell out, set by the MainWindow
#endif
    common::Image m_documentImage;                // pending composite to hand to the renderer
    bool m_documentPending = false;
    common::Image m_documentRegion;               // pending dirty-region patch (S60-a)
    std::uint32_t m_documentRegionX = 0;
    std::uint32_t m_documentRegionY = 0;
    bool m_documentRegionPending = false;
    std::vector<std::uint8_t> m_selectionMask; // pending selection coverage (S13 marching ants)
    std::uint32_t m_selectionW = 0;
    std::uint32_t m_selectionH = 0;
    bool m_selectionPending = false;
    bool m_initFailed = false;
    std::string m_error;
    std::string m_overlayTileKey; // last rasterized dial/HUD tile content (skip re-raster if same)

    // View transform + interaction (S8).
    CanvasView m_view;
    double m_contentScale = 1.0;    // logical -> physical px (HiDPI), from the native handle
    // The scale to rasterize custom RGBA cursors at. macOS shows an Fl_RGB_Image cursor at
    // pixel==point (it does NOT treat a 2x bitmap as HiDPI), so building at m_contentScale makes the
    // pointer 2x too big; build at logical size there instead -- slightly softer on Retina but
    // correctly sized. (S58; revisit if FLTK gains HiDPI cursor support.)
    [[nodiscard]] double cursorBuildScale() const noexcept {
#ifdef __APPLE__
        return 1.0;
#else
        return m_contentScale;
#endif
    }
    bool m_viewInitialized = false; // first document fits the viewport

    // ⚠ THE GESTURE-MODIFIER KEY RULE (S59-b). Space and R are the canvas's two bare-key gesture
    // modifiers, and both used to infer "held" from a press/release PAIRING -- an assumption that
    // only holds on a backend that delivers exactly one KEYUP per KEYDOWN, in order. FLTK's Wayland
    // backend does not: it SYNTHESISES auto-repeat from a timer (Fl_Wayland_Screen_Driver's
    // key_repeat_timer_cb calls Fl::handle(FL_KEYDOWN, ...) every 50 ms against the window captured
    // at press time, with Fl::e_keysym left at whatever the last REAL event set -- a mouse button,
    // mid-drag), and it drops the whole held-key set on wl_keyboard_leave WITHOUT sending a single
    // KEYUP. So both directions fail: extra downs, and ups that never come.
    //
    // The two flags below are therefore only ever a CACHE of the window system's own answer
    // (keyPhysicallyHeld -> Fl::event_key(k), which reads the compositor's key vector on Wayland
    // and XQueryKeymap on X11). A KEYUP for a key the system still holds is refused outright, and
    // the pointer events resync the pair, so a lost KEYUP cannot strand the canvas in pan/rotate
    // mode either. See docs/wayland.md §6.
    bool m_spaceDown = false; // Space held -> pan mode (a CACHE; keyPhysicallyHeld is the truth)
    bool m_panning = false;   // a pan drag is in progress -- owned by the POINTER, PUSH..RELEASE
    bool m_rotateDown = false;        // R held -> rotate mode (same cache rule as m_spaceDown)
    bool m_rotating = false;          // a rotate drag is in progress
    bool m_rotatedSincePress = false; // distinguishes a rotate-drag from a double-tap reset
    int m_lastMouseX = 0;
    int m_lastMouseY = 0;
    double m_rotateGrabAngle = 0.0; // cursor angle at the start of a rotate drag
    double m_rotateBaseRotation = 0.0;
    double m_lastRDownTime = -1.0e9; // for double-tap-R detection
    double m_dialResetUntil =
        -1.0e9; // double-tap-R reset: keep the dial up (showing "Reset") until

    std::function<bool(int)> m_heldKeyQuery;              // test seam; see setHeldKeyQuery
    std::function<void(double, double, bool)> m_onCursor; // S13-b: cursor doc-position readout
    std::function<void()> m_onRendererShutdown;           // S60-a: release the resident lane first
    std::function<void()> m_onViewChanged;                // S13-b: zoom/rotation readout
    std::function<void()> m_requestFrame;                 // S15-b: immediate frame kick

    // S14 selection tools.
    ToolManager* m_tools = nullptr; // active-tool lookup (non-owning)
    SelectionHost m_selectionHost;
    MagicWandHost m_magicWandHost;
    BucketFillHost m_bucketFillHost;
    // S24 Eyedropper loupe state: the host, whether the loupe is shown this frame (so a hide is sent
    // exactly once on leave), and the last resolved sample colour -- fed to the loupe's swatch band
    // and, via updateOverlayTile, its hex/RGB readout. Empty optional = the pointer is off the
    // pixels, so the loupe drops the swatch + readout and shows the magnified content alone.
    EyedropperHost m_eyedropperHost;
    bool m_loupeVisible = false;
    std::optional<common::Color8> m_loupeReadout;
    SelectionGesture m_gesture;
    // S18 select brush: the in-flight coverage stroke, its press-time op, and the preview dirty flag
    // (drag events mark it; renderFrame rebuilds the combined mask at most once per frame).
    core::brush::MaskStroke m_maskStroke;
    core::SelectOp m_selectBrushOp = core::SelectOp::Add;
    bool m_maskStrokeActive = false;
    bool m_maskStrokePreviewDirty = false;
    bool m_selectBrushAddByDefault = true; // Settings::selectBrushAddByDefault (§9-B)
    // L1 edge brush: its own in-flight seed stroke + press-time op + preview dirty flag (the
    // preview is the raw trail combined with the base -- the grow itself never runs mid-stroke).
    EdgeBrushHost m_edgeBrushHost;
    core::brush::MaskStroke m_edgeStroke;
    core::SelectOp m_edgeBrushOp = core::SelectOp::Add;
    bool m_edgeStrokeActive = false;
    bool m_edgeStrokePreviewDirty = false;
    // S38-b eye retouch: its own in-flight scope stroke + preview dirty flag. The preview is the
    // RAW trail (what the user painted), never a corrected pixel -- the correction runs once, on
    // release, and lands as one command.
    RedEyeHost m_redEyeHost;
    core::brush::MaskStroke m_redEyeStroke;
    bool m_redEyeStrokeActive = false;
    bool m_redEyeStrokePreviewDirty = false;
    // S16-i: moving the selection outline. `m_selMove` serves both the pointer drag and the
    // arrow-key nudge session; the preview is frame-coalesced like the marquee's.
    SelectionMoveGesture m_selMove;
    // S50 file drops onto the canvas. m_expectDropPaste distinguishes the dropped-URI payload from
    // the Type tool's clipboard-text FL_PASTE, which arrives through the same event.
    std::function<void(const std::vector<std::string>&)> m_onFilesDropped;
    bool m_expectDropPaste = false;

    // The documentless idle state (the empty-state idle pass). m_idleEnabled mirrors
    // m_idleFade.enabled for cheap handle() checks; the atlas keys record what the current bake
    // was made for, so a theme or DPI change re-bakes on the next frame.
    std::function<void()> m_onIdleOpen;
    std::function<void(const std::string&)> m_onIdleOpenPath;
    IdleFadeState m_idleFade;
    // "A drop bloom is lit and nobody has cleared it." Distinct from m_idleFade's own hot timeline:
    // that keeps a nonzero value while easing out, whereas this is the latch handle() consults to
    // heal a FL_DND_LEAVE that never arrived (see the note at the top of handle()).
    bool m_dropHot = false;
    bool m_idleEnabled = false;
    bool m_idleAtlasBaked = false;
    bool m_idleAtlasDark = false;
    double m_idleAtlasScale = 0.0;
    common::Color8 m_idleAtlasAccent{};

    bool m_selMoveDirty = false;        // the translated mask needs a rebuild this frame
    std::uint64_t m_nudgeCoalesce = 0;  // undo-coalescing id of the live nudge burst (0 = none)
    std::uint64_t m_nudgeCoalesceNext = 0; // monotonic source of those ids
    std::uint64_t m_nudgeRev = 0;       // selectionRevision our last nudge commit produced
    bool m_pointerInside = false; // pointer is over the canvas (FL_ENTER .. FL_LEAVE)
    common::Vec2 m_cursorLogical{0.0,
                                 0.0}; // last pointer pos, logical screen px (drives the reticle)
    // >0 while handle() is dispatching a POSITIONAL event to us, i.e. exactly while FLTK has
    // Fl::event_x/y translated into our frame. eventLogicalPoint() is the only reader; the
    // PointerFrame guard in handle() is the only writer. See eventLogicalPoint()'s note.
    int m_pointerFrameDepth = 0;
    // Where the pointer is HEADING, in the document's frame. The reticle turns a direction-following
    // tip by it (14 shipped presets do), so it is updated in emitCursor -- the one funnel both the
    // mouse and the tablet path go through. During a stroke the engine's own heading wins.
    HoverHeading m_hoverHeading;
    // Cursor cache: -1 = default arrow; 0-3 = the selection-tool badge cursors (SelectOp);
    // 10+ = stock cursors (10 move, 11 NS, 12 WE, 13 NWSE, 14 NESW, 15 rotate; 16 pan-grab,
    // 17 pan-grabbing).
    // -2 is the INVALIDATION SENTINEL, not a state: updateToolCursor early-outs when the state it
    // resolves equals this field, and -1 is a legal resolution (the plain arrow), so parking -2
    // here is the only way to say "whatever is showing, re-install it". Written on canvas re-entry
    // (Wayland's seat->default_cursor is app-global, so another widget or a finished DND may have
    // replaced ours behind our back) and whenever the content scale changes (every cached bitmap
    // was rasterized for the old one). See S59-a.
    int m_cursorState = -1;
    // The four op cursors (Replace/Add/Subtract/Intersect), built lazily. FLTK keeps a pointer
    // into the Fl_RGB_Image, which points into the CursorImage pixels -- both stay alive here.
    std::array<CursorImage, 4> m_cursorPixels;
    std::array<std::unique_ptr<Fl_RGB_Image>, 4> m_cursorImages;
    // The two pan-gesture hands (0 = open "grab", 1 = closed "grabbing"; states 16/17), built
    // lazily and kept alive alongside the Fl_RGB_Image that points into them, exactly like the op
    // cursors.
    std::array<CursorImage, 2> m_panCursorPixels;
    std::array<std::unique_ptr<Fl_RGB_Image>, 2> m_panCursorImages;
    // The Move tool's rotate cursor (state 15): a theme-recoloured, bilinear-spun apple left_side
    // double-arrow, rebuilt as the nearest-handle->pointer angle (or the theme) changes so it
    // follows the box. Quantised to m_rotateCursorBucket (+ m_rotateCursorDark) to avoid rebuilding
    // every pixel.
    void applyRotateCursor();
    CursorImage m_rotateCursorPixels;
    std::unique_ptr<Fl_RGB_Image> m_rotateCursorImage;
    int m_rotateCursorBucket = -1;
    bool m_rotateCursorDark = true;

    // The four-way MOVE arrow (state 10), substituted for the stock FL_CURSOR_MOVE on WAYLAND only
    // -- FLTK resolves that request by the Xcursor name `move`, which breeze_cursors symlinks to
    // `dnd-move`, a closed GRABBING hand: hovering a Move-tool selection announced a drag that was
    // not happening. X11 resolves the same request to XC_fleur, which is exactly this art, so the
    // substitution is Wayland-only (see ui::moveCursor). ui::MoveCursor holds that decision and its
    // OWN rasterized cache -- different art from the rotate/resize double-arrow, so it cannot ride
    // that one -- and the same type serves the chrome widgets that show a move cursor.
    void applyMoveCursor();
    MoveCursor m_moveCursor;

    // The Type tool's rotating I-beam (state 21, §6.1): the vendored xterm glyph spun to the local
    // baseline (the edited / hovered text layer's screen-space rotation), theme-recoloured, bucketed
    // like the rotate cursor so it only rebuilds when the orientation or theme changes.
    void applyTextCursor();
    CursorImage m_textCursorPixels;
    std::unique_ptr<Fl_RGB_Image> m_textCursorImage;
    int m_textCursorBucket = -1;
    bool m_textCursorDark = true;

    // The Type tool's fit-to-path hover hand (state 22, §9 follow-up): apple_cursor hand2, shown
    // while the pointer sits near a vector path spine, where a click flows text onto that path.
    // Static art -- built once per HiDPI scale and cached.
    void applyFitTextCursor();
    [[nodiscard]] bool typePathSpineUnderPointer() const;
    CursorImage m_fitTextCursorPixels;
    std::unique_ptr<Fl_RGB_Image> m_fitTextCursorImage;
    int m_fitTextCursorScale = 0;

    // S33 DoF focus-band gizmo: the host callbacks + the in-flight handle drag. The drag latches
    // the press-time provider state and the parentToDoc INVERSE, so every drag event maps the
    // cursor into the same parent frame -- re-deriving it live would feed the edit back into
    // itself as the geometry moves (the Type bend-plane lesson).
    std::function<bool(DofGizmoState&)> m_dofGizmoProvider;
    std::function<void(const char* coalesceId, const DofGizmoState&)> m_dofGizmoEdit;
    struct DofDragState {
        bool active = false;
        int handle = -1;              // 0 centre, 1 rotate, 2 +band, 3 -band, 4 +feather, 5 -feather
        DofGizmoState press;          // the provider state at press time
        common::Vec2 pressDoc;        // press point, document space
        common::Affine2D docToParent; // parentToDoc's inverse, latched at press
    };
    DofDragState m_dofDrag;

    // Document guides (View -> Guides). Grabbing an existing guide (Move tool) or pulling a new one
    // off a ruler PREVIEWS on-canvas without mutating the document; the command lands on release.
    struct GuideDrag {
        bool active = false;
        std::uint64_t id = 0;
        bool horizontal = false; // orientation of the grabbed guide
        double startPos = 0.0;   // its document position at press (for undo + delete)
        double currentPos = 0.0; // the live dragged position
    };
    GuideDrag m_guideDrag;
    struct GuideCreate {
        bool active = false;
        bool horizontal = false; // a horizontal line, pulled from the top ruler
        double pos = 0.0;
    };
    GuideCreate m_guideCreate;
    std::uint64_t m_guideCoalesce = 1; // one undo step per guide drag (SetOpacity's trick)

    // Snapping (View -> Snap): a master toggle + the three target sources. Runtime, not persisted.
    bool m_snapEnabled = true;
    bool m_snapToGuides = true;
    bool m_snapToCanvas = true;
    bool m_snapToLayers = true;
    // Smart guides (View -> Smart Guides): the magenta alignment lines shown while a Move drag snaps
    // to other layers / the canvas. m_smartGuideLines is rebuilt each drag frame (in applyMoveSnap)
    // and cleared when the gesture ends.
    bool m_smartGuides = true;
    struct SmartGuideLine {
        bool horizontal = false; // a horizontal line (constant Y) vs a vertical one (constant X)
        double pos = 0.0;        // its document coordinate
    };
    std::vector<SmartGuideLine> m_smartGuideLines;

    // S15 Move tool. The move selection is canvas-only and ephemeral (cleared on tool switch / new
    // doc / click-away) — shift-click gathers layers to drag together WITHOUT grouping (S15-c).
    MoveToolHost m_moveHost;
    std::vector<core::LayerId> m_moveTargets; // layers wearing the shared handles; back() = primary
    TransformGesture m_transform;
    std::uint64_t m_transformCoalesce =
        1; // gesture id: one undo step per drag (SetOpacity's trick)
    // Gesture frame, captured at beginMoveGesture: the box's base transform + its inverse and the
    // framed content rect (so the overlay can follow the gesture), each target's press-time world
    // transform (the delta applies to these, not the live ones), and the latest box-world result.
    common::Affine2D m_gestureBaseInv;
    common::Rect m_gestureContent;
    common::Affine2D
        m_gestureResult; // box-world transform for the current cursor (overlay follows)
    std::vector<std::pair<core::LayerId, common::Affine2D>> m_gestureBaseWorld;
    // The latest FL_DRAG state, consumed once per frame by flushMoveDrag.
    bool m_moveDragPending = false;
    bool m_moveDragShift = false;
    bool m_moveDragAlt = false;
    bool m_moveGesturePushed = false; // a transform landed this gesture -> fire gestureEnded
    bool m_resetHudShowing = false;   // briefly hold the "0.00 deg" HUD after a double-click reset
    common::Rect m_sampleAreaDocRect{}; // inpaint sample-area preview, doc px (S39)
    bool m_sampleAreaActive = false;
    common::Vec2 m_moveDragDocPt;
    // Press bookkeeping: a press must ARM a drag (so select-and-drag works), but if it ends without
    // one it was a CLICK — and clicks drill into the selected group, collapse a multi-selection to
    // the clicked item, or (with Shift) toggle the clicked layer's membership. The dead-zone latch
    // keeps mouse jitter from turning a click into a micro-drag that defeats the click action.
    enum class MoveClickAction : std::uint8_t { None, Drill, Collapse, Toggle };
    MoveClickAction m_moveClickAction = MoveClickAction::None;
    core::LayerId m_moveClickLayer = core::kInvalidLayerId; // resolved target for Collapse/Toggle
    bool m_moveDragLatched = false; // the cursor left the press point's dead zone
    common::Vec2 m_movePressScreen; // press position, logical screen px
    common::Vec2 m_movePressDoc;    // press position, document space
    // The transform ANCHOR / reference point (S15+): where rotation and scaling pivot. Stored in
    // layer-local (content) space so it rides the box across gestures; nullopt = the auto default
    // (box centre for rotate, opposite handle for scale). Reset on any move-selection change.
    std::optional<common::Vec2> m_transformPivotLocal;
    bool m_anchorDragging = false;                 // an anchor-repositioning drag is in progress
    std::optional<common::Vec2> m_anchorDragPrev;  // pivot value before the drag, for Esc-cancel
    // S15-f empty-space layer marquee (see beginLayerMarquee). `active` from the press that missed
    // everything; `latched` once the cursor cleared the same dead zone a layer drag uses, which is
    // what separates the click (deselect) from the band. `base` is the press-time selection, kept
    // only for the Shift/Ctrl extend. `dirty` asks renderFrame to rebuild the band's overlay mask.
    bool m_layerMarqueeActive = false;
    bool m_layerMarqueeLatched = false;
    bool m_layerMarqueeExtend = false;
    bool m_layerMarqueeDirty = false;
    common::Vec2 m_layerMarqueeAnchor{};
    common::Vec2 m_layerMarqueeCursor{};
    std::vector<core::LayerId> m_layerMarqueeBase;

    // S16 Crop tool.
    CropToolHost m_cropHost;
    CropGesture m_crop;
    std::optional<common::Rect> m_cropRect; // staged rect, doc space (nullopt = full canvas)
    // S16-f rotate: the staged box = m_cropRect expressed in the document plane rotated by
    // m_cropAngle about m_cropFrameC (a FIXED pivot — see cropFrameToDoc's contract; rebased
    // between gestures, never during one). Angle 0 = the historical axis-aligned crop, with
    // m_cropFrameC irrelevant. The rotate drag is canvas-local state (CropGesture stays
    // rotation-blind: it works in frame coordinates throughout).
    double m_cropAngle = 0.0;
    common::Vec2 m_cropFrameC{};
    bool m_cropRotating = false;    // a rotate-band drag is in flight
    double m_cropAngle0 = 0.0;      // angle at rotate-press (Esc restores it)
    double m_cropRotatePress = 0.0; // atan2 of the press point about the pivot
    // Shrink-to-fit while rotating (user 2026-07-02): a box that was fully INSIDE the canvas at
    // rotate-press stays inside — each drag frame derives the rect from the press-time base
    // scaled about the pivot, so rotating back and forth is stable and never ratchets. A box
    // already staging an expansion rotates unconstrained.
    common::Rect m_cropRotateBase{}; // frame rect at rotate-press (post-rebase; Esc restores it)
    bool m_cropRotateFit = false;    // base was fully inside -> constrain the rotation
    void setCropAngle(double a);    // sets + fires m_cropHost.angleChanged on change
    void resetCropRotation();       // dbl-click the band: axis-aligned, centre + size kept
    CropFraming m_cropFraming = CropFraming::WholeCanvas; // S16-q: initial-framing mode
    // The Image-ops panel's staged preview, riding the crop overlay channel above (see
    // setImageOpPreview). Pure display state: it never touches the document, the crop rect or the
    // command stack, and it is dropped by the panel, not by any canvas gesture.
    std::optional<ImageOpPreview> m_imageOpPreview;
    // Its live handle drag. CropGesture does the rect maths — the preview's handles ARE a crop
    // box's, down to the Shift/Alt constraints, the canvas-edge snap and the safety envelope — and
    // it latches the base rect at the press, which is exactly what makes the panel restaging under
    // us mid-drag harmless. m_imageOpHandle keeps the grabbed index for the cursor.
    CropGesture m_imageOpDrag;
    int m_imageOpHandle = -1; // 0-3 corners TL,TR,BR,BL; 4-7 edge mids T,R,B,L; -1 = none grabbed
    std::function<void(long, long, std::uint32_t, std::uint32_t)> m_onImageOpPreviewDrag;
    int m_overlayLineStyle = 1; // Settings->Appearance line style (default Shadowed/rim); applied
                                // to the renderer on create
    int m_featherIndicator = 0; // Settings->Appearance feathered-selection indicator (default
                                // Bracketing ant pair, A); applied to the renderer on create
    bool m_antsCirculate = false; // Settings hidden `antsCirculate` (§5); applied to the renderer
    bool m_smartResizeWasOn = false; // S16-f: last-seen toggle state (detects ON/OFF edges)
    double m_smartLastRatio = 0.0;   // S16-f: last-seen aspect (re-suggests only on change)
    // Keep-region chips (Smart Recompose plan §1): the host's automatic regions + the user's
    // per-chip enabled flag (click toggles). Enabled chips feed the suggestion as protect rects.
    // A USER chip (Ctrl-drag, fork F-d) is hand-marked: it survives the per-frame re-fetch as-is
    // and a click REMOVES it (it has no detector to fall back to, so "off" means "gone").
    struct SmartChip {
        common::Rect rect; // doc space, snug
        bool enabled = true;
        bool user = false; // hand-marked via Ctrl-drag (KeepRegion::Source::User)
    };
    std::vector<SmartChip> m_smartChips;
    // The in-flight Ctrl-drag that marks a new user chip (crop tool + Smart Resize ON). Uses the
    // same press bookkeeping/slop as the crop gestures (m_cropPressScreen).
    bool m_chipDrawing = false;      // a Ctrl-press anchored a chip draw
    bool m_chipDrawLatched = false;  // the drag cleared the click slop (a real rect is forming)
    common::Vec2 m_chipDrawAnchor{}; // press point, doc space (clamped into the document)
    common::Rect m_chipDrawRect{};   // the live rect (anchor -> cursor, clamped)
    void dragChipDraw();             // FL_DRAG: grow the provisional rect
    void finishChipDraw(bool commit); // release (commit) or Esc/tool-switch (discard)
    bool m_cropDragMoved = false; // whether the current crop press actually dragged (else: click)
    common::Vec2 m_cropPressScreen; // the press point (logical px) for the click-slop test
    bool m_cropDrawFromEmpty = false; // a DrawToBegin draw is in flight (Esc returns to no rect)
    // The previous click of the current multi-click sequence toggled/removed a chip. The
    // double-click-apply swallow needs this because a REMOVED user chip is no longer under the
    // cursor for smartChipAt — without the flag the second click would commit the crop.
    bool m_chipClickConsumed = false;
    // Smart Recompose (plan §1.3–§1.4): the offer edge-detector + the review-mode state. During
    // review the "document" the view shows IS the preview (the host swapped it), so placements
    // and eventDocPoint share one space.
    bool m_recomposeOfferLast = false; // last state handed to recomposeOffer (fire on change)
    // Offer memo: the sync runs on the per-frame chip sync, so the bbox test + placement solver
    // only re-run when one of their inputs actually changed (chips / aspect / document size).
    bool m_offerValid = false;
    bool m_offerResult = false;
    double m_offerAspect = 0.0;
    std::uint32_t m_offerDocW = 0, m_offerDocH = 0;
    std::vector<common::Rect> m_offerKeeps;
    bool m_recomposeReview = false;
    std::vector<common::Rect> m_reviewPlacements; // snug placement chips, preview space
    int m_reviewDrag = -1;                        // placement being dragged (-1 = none)
    common::Vec2 m_reviewDragOffset{};            // grab point offset within the dragged rect
    // Nudges are frame-coalesced like the Move drag: FL_DRAG only records; the per-frame sync
    // fires ONE reviewNudge (host assemble + upload) per frame tick, however fast the mouse.
    bool m_reviewNudgePending = false;
    int m_reviewNudgeIdx = -1;
    void syncRecomposeOffer(); // per frame: recompute the offer, notify the host on change
    void pushReviewDrag();     // FL_PUSH during review: grab the placement under the cursor
    void dragReviewPlacement(); // FL_DRAG: move it (clamped in-frame) + fire reviewNudge

    // ---- S35-b Mesh Warp / Perspective Warp (docs/warp-tools.md §5) ---------------------------
    // ⚠ EVERY grid below lives in the BOUND LAYER'S BASE-LOCAL pixel space -- the space m_warpBase is
    // indexed in -- and never in the layer's CURRENT local space. The two differ the moment a preview
    // bake lands, because a bake re-homes the pixel origin and post-translates the transform to
    // absorb it. Pinning the whole session to the base space is what keeps the handles, the hit test
    // and every bake reading one coordinate system: m_warpBaseWorld is captured once at bind time and
    // is the only doc<->grid map any of this uses.
    WarpToolHost m_warpHost;
    core::LayerId m_warpLayer = core::kInvalidLayerId; // the bound layer (invalid = no session)
    common::Image m_warpBase;                          // its PRISTINE pre-warp pixels
    common::Affine2D m_warpBaseTransform;              // ... and its placement at bind time
    common::Affine2D m_warpBaseWorld;                  // base-local -> document (fixed per session)
    common::Affine2D m_warpBaseWorldInv;               // ... and its inverse
    // The grid m_warpBase is ALREADY deformed by (the layer's stored warp, or the undeformed lattice
    // when it has none) and the live, edited grid. warpImage(from, to) applies exactly the difference
    // -- which is the only reading under which re-entering the tool and nudging one handle does what
    // it looks like it does, instead of applying the stored displacement a second time.
    core::WarpGrid m_warpFrom;
    core::WarpGrid m_warpGrid;
    core::WarpGrid m_warpDragBase; // the grid at the PRESS: every drag frame applies to THIS, so a
                                   // drag never accumulates its own rounding
    bool m_warpPreviewed = false;  // the layer currently shows a preview bake (restore on cancel)
    int m_warpHandle = -1;         // the grabbed handle (-1 = none)
    int m_warpHover = -1;          // the hovered handle, chrome only (-1 = none)
    bool m_warpDragging = false;
    bool m_warpDragPending = false; // a drag frame is queued for flushWarpDrag
    bool m_warpDragMoved = false;   // at least one drag frame changed the lattice
    common::Vec2 m_warpPressBase{}; // press point, base-local px (the drag delta's origin)
    common::Vec2 m_warpDragBasePt{}; // latest drag point, base-local px (latched at event time)
    bool m_warpDragShift = false;
    bool m_warpDragAlt = false;

    // S19-a Brush tool. The stroke paints directly into the live layer image (for the recomposited
    // preview); the engine keeps its own BOUNDED pristine snapshot (S60-c) and restore()s it before
    // the single SetLayerPixelsCommand lands (so undo captures the pre-stroke pixels).
    BrushToolHost m_brushHost;
    core::brush::BrushEngine m_brushEngine;
    core::LayerId m_brushLayer = core::kInvalidLayerId; // the layer this stroke paints
    common::Affine2D m_brushWorldInv;                   // doc -> layer-local (captured at press)
    bool m_brushStroking = false;
    // S31 mask painting: this stroke writes the layer's MASK coverage through a gray RGBA proxy
    // (see beginMaskStroke). m_maskToDoc maps mask px -> document for previews/commits.
    bool m_brushMaskLane = false;
    common::Image m_maskProxy;
    common::Affine2D m_maskToDoc;
    // The press-time auto-grow (core/layer_grow.hpp), all of it undone before the stroke commits.
    // `m_brushGrowBox` is the box the layer was grown to, in PRE-PRESS layer-local coordinates -- so
    // its negated origin is where the old (0,0) sits in the working grid, and it is the identity box
    // {0,0,w,h} whenever nothing grew. `m_brushCanvasBox` is the canvas in those same pre-press
    // coordinates, kept so the commit can bound the PERMANENT growth without re-deriving it from a
    // transform the growth has since changed.
    core::PixelBox m_brushGrowBox;
    core::PixelBox m_brushCanvasBox;
    std::uint32_t m_brushGrowOrigW = 0;
    std::uint32_t m_brushGrowOrigH = 0;
    common::Affine2D m_brushGrowTransform; // the layer's transform before the growth
    // A press whose FIRST DAB is owed to the next real device sample: the tip is down, but the
    // sample carrying its contact pressure has not arrived yet (pushBrushTool explains why, and it
    // is measured, not assumed). Only ever set with a stylus in proximity -- a mouse press begins
    // its stroke on the spot, exactly as it always has.
    bool m_brushPressPending = false;

    // ---- S38 Stamp / Clone (docs/clone-stamp.md §5) -------------------------------------------
    // The stroke rides the brush lane above, and everything here is what makes its DEPOSIT source
    // pixels instead of paint. The engine still lays the stroke -- its tip, its spacing cadence, its
    // flow build-up and its opacity ceiling -- and stampCloneRegion then rewrites exactly the pixels
    // the engine's own composite() just wrote, reading `m_cloneBase` for the destination and
    // `m_cloneSource` for what lands on it. The engine's coverage IS the stroke's alpha, so the two
    // agree pixel for pixel about where the stroke is.
    CloneStampHost m_cloneHost;
    // The picked source + the latched aligned offset. Outlives strokes and tool switches; only a
    // document swap clears it (clearCloneSource).
    core::CloneAnchorState m_cloneAnchor;
    bool m_cloneStroking = false;   // the live brush stroke is a clone stroke
    common::Vec2 m_cloneOffsetDoc{}; // its offset (target - source), document px, fixed at the press
    // The target layer's PRE-STROKE pixels. ⚠ The clone composite reads THIS, never the live layer
    // (docs/brushes.md §6.6b): a dab landing on an earlier dab of the same stroke must still stamp
    // onto pristine pixels, or the mark would depend on how often composite() ran.
    common::Image m_cloneBase;
    // Only for the two composited Sample modes: a DOCUMENT-space snapshot taken once at the press.
    // Empty for "Current layer", which samples m_cloneBase itself -- one buffer, not two, in the
    // mode that is the default and by far the common one.
    common::Image m_cloneBackdrop;
    bool m_cloneFromBackdrop = false;
    common::Affine2D m_cloneTargetToSource; // layer px -> source px, the offset folded in
    bool m_cloneBilinear = false;           // !isWholePixelShift: a resample, not a byte copy
    std::shared_ptr<const core::StrokeConfinement> m_cloneConfine; // the stroke's own, borrowed
    // Brush smoothing (core/brush/stroke_smoother.hpp). Sits between the INPUT and the engine: it
    // moves the user's points, which is the one thing the engine's own path code may never do.
    core::brush::StrokeSmoother m_smoother;
    // The tablet (docs/tablet.md). Owned by the canvas because the canvas is the surface the
    // backends bind to AND the consumer of their samples. Settings -> Tablet reaches its policy and
    // its device list through tabletInput().
    TabletInput m_tablet;
    core::brush::SpeedParams m_speedParams{}; // Settings -> Tablet -> Speed smoothing (§7)
    bool m_reticleVisible =
        false;                  // last reticle state handed to the renderer (skip redundant sets)
    bool m_inpaintBusy = false; // an async inpaint is running: block editing, padlock the reticle
    // The reticle's TIP OUTLINE (docs/brushes.md §6.3): the traced silhouette of a shaped tip -- a
    // bristle, a spatter, a spiked star -- which the present shader samples instead of drawing an
    // ellipse over a tip that is not one.
    //
    // These describe the field the RENDERER is holding, and their only job is to keep
    // `core::brush::buildTipSdf` -- an O(area) rasterization plus two distance transforms -- from
    // running on a mouse move. The reticle is re-driven on every motion, zoom, resize and rotation,
    // and not one of those changes the tip's silhouette: the field is built in the tip's OWN frame,
    // and every one of them is applied when the shader SAMPLES it. Only the tip's raster and the
    // dab's RATIO do (tip_outline.hpp: a spiked tip has no star until it is squashed).
    std::uint64_t m_reticleSdfTip = 0;   // BrushTip::id it was built from (0 = none: the ellipse)
    double m_reticleSdfRatio = 0.0;      // ... and the ratio
    std::uint64_t m_reticleSdfGen = 0;   // bumped per build: the renderer's re-upload key

    // S26 Shape tool. Nothing enters the document while the drag runs (S26-c): the latched drag
    // state below is all there is until release, and the wireframe is derived from it per frame.
    ShapeToolHost m_shapeHost;
    bool m_shapeDragging = false;
    common::Vec2 m_shapePressDoc{}; // the press point in document space (drag anchor)
    common::Vec2 m_shapeDragDoc{};  // ... and the latest drag point, latched at event time
    bool m_shapeDragShift = false;  // the modifiers as of that same event (constrain / centre)
    bool m_shapeDragAlt = false;
    bool m_shapeDragMoved = false;  // at least one drag frame latched (a bare press draws nothing)
    // Select-to-edit (§7.1): the existing shape currently bound to the options bar (invalid = none,
    // the bar sets defaults for the next authored shape). m_shapeEditCoalesce bumps per selection so
    // one edit session is one undo step; the two guards prevent the reflect/tool-switch plumbing from
    // looping back as a user edit.
    core::LayerId m_shapeEditTarget = core::kInvalidLayerId;
    std::uint64_t m_shapeEditCoalesce = 0;
    bool m_reflectingShapeOptions = false; // writing params INTO the bar (not a user edit)
    bool m_selectingShapeForEdit = false;  // inside pickShapeForEdit's tool switch (keep the target)

    // Resize-vs-transform (§7.1): the selected shape's on-canvas box gesture. Reuses TransformGesture
    // for the Move/Rotate math; a RESIZE (the default Scale) is computed directly via resizeShape off
    // the press-time base, so it never drifts. The result is pushed coalesced once per frame tick.
    TransformGesture m_shapeBox;                 // active while a handle/body/band is grabbed
    TransformMode m_shapeBoxMode = TransformMode::None;
    int m_shapeBoxHandle = -1;
    bool m_shapeBoxResize = false;               // Scale + !transform-toggle: edit params, not the xform
    common::Affine2D m_shapeBoxBase;             // edit layer's WORLD transform at press (docPt mapping)
    common::Affine2D m_shapeBoxBaseLayer;        // edit layer's own transform at press (restore on Esc)
    common::Affine2D m_shapeBoxParentInv;        // parent-world inverse: world result -> layer transform
    common::Rect m_shapeBoxContent;              // the handle box (contentBounds) at press
    core::vec::Object m_shapeBoxBaseObject;      // the object at press (resize edits a copy each frame)
    common::Vec2 m_shapeBoxPressScreen{};        // press point, logical screen px (drag dead zone)
    common::Vec2 m_shapeBoxDocPt{};              // latest drag point, document space (frame-coalesced)
    bool m_shapeBoxShift = false, m_shapeBoxAlt = false;
    bool m_shapeBoxPending = false;              // a drag frame is queued for flushShapeBoxDrag
    bool m_shapeBoxLatched = false;              // moved past the dead zone (a real drag, not a click)
    bool m_shapeBoxPushed = false;               // at least one edit landed (so Esc restores / commit)
    // Line gizmo (S26): which control is grabbed (-1 none, 0 a, 1 b, 2 mid/bend, 3 body). Shares the
    // m_shapeBox* capture/flush members (a line uses the gizmo, never the box -- mutually exclusive).
    int m_lineHandle = -1;
    common::Vec2 m_linePressDoc{}; // press point, document space (body-move delta)

    // S28 Pen tool. Two independent halves that never run at once: AUTHORING (m_pen, document
    // space, nothing in the document until the path is finished -- the S26-c wireframe discipline)
    // and NODE EDITING of a committed path (m_penEditTarget + the press-time base below, layer-local
    // space, one coalesced SetVectorObjectCommand per gesture).
    PenToolHost m_penHost;
    PenGesture m_pen;
    // The bar's paint, latched while AUTHORING (every press + every hover tick). A path can be
    // finished by a TOOL SWITCH, and by then the active tool is no longer the Pen -- reading the
    // options bar at that moment would paint the path with the defaults instead of the settings it
    // was drawn with. The latch is the honest answer at every finish route.
    PenOptions m_penAuthorOpts;
    // The authoring press's screen position + its dead zone. A CLICK is a corner node and a DRAG
    // pulls a smooth handle pair, so the two have to be told apart by more than "did FLTK send an
    // FL_DRAG" -- a one-pixel jitter would otherwise turn every click into a smooth node with a
    // one-pixel handle. Same threshold, and the same reasoning, as the Move/shape box gestures.
    common::Vec2 m_penAuthorPressScreen{};
    bool m_penAuthorLatched = false;
    core::LayerId m_penEditTarget = core::kInvalidLayerId;
    std::uint64_t m_penEditCoalesce = 0;
    PenSelection m_penSel;                  // the selected node of the bound path (chrome + Delete)
    PenHit m_penGrab;                       // what the live edit drag is moving (Kind::None = idle)
    // Hover state, chrome-only: which knob of the bound path the pointer is over (Kind::None = no
    // knob), and where the pointer last was in logical screen px -- the closing-loop ring's test.
    // Latched at FL_MOVE and cleared on FL_LEAVE, never read from FLTK by the frame loop.
    PenHit m_penHover;
    common::Vec2 m_penHoverScreen{};
    bool m_penHasHover = false;
    core::vec::Path m_penEditBase;          // the bound path at press (the drag is applied to it)
    common::Vec2 m_penEditPressLocal{};     // press point in the bound layer's local space
    common::Vec2 m_penEditPressScreen{};    // ... and in logical screen px (the drag dead zone)
    bool m_penEditDragging = false;
    bool m_penEditLatched = false;          // moved past the dead zone (a drag, not a click)
    bool m_penEditPushed = false;           // at least one edit landed in this gesture
    bool m_reflectingPenOptions = false;    // writing the bar from a path (not a user edit)

    // S22 Gradient tool. A drag authors a full-bleed gradient layer (host owns the live preview
    // layer); re-selecting the tool on a gradient layer binds it and its axis/handle gizmo re-drags
    // the geometry. Handle drags coalesce into one undo step per gesture (m_gradientEditCoalesce).
    GradientToolHost m_gradientHost;
    bool m_gradientDragging = false;     // authoring a new gradient (press..release lays it down)
    bool m_gradientPreviewing = false;   // a live preview layer exists (host owns it)
    common::Vec2 m_gradientPressDoc{};   // authoring press point, document space (drag anchor)
    core::LayerId m_gradientEditTarget = core::kInvalidLayerId; // an existing gradient bound for edit
    std::uint64_t m_gradientEditCoalesce = 0; // bumped per handle-drag session (one undo step)
    int m_gradientHandle = -1;           // handle grabbed on an edit target (-1 = none; 0/1/2/3)
    common::Vec2 m_gradientHandlePressDoc{}; // press point of the handle drag (rigid-move delta base)
    core::vec::Object m_gradientHandleBase;  // the edit object at handle-grab (dragGradientHandle base)
    common::Affine2D m_gradientHandleWorld;  // the edit layer's world transform at handle-grab

    // S29-b Type tool: the on-canvas editing session.
    TypeToolHost m_typeHost;
    core::LayerId m_textEditTarget = core::kInvalidLayerId; // the text layer being edited (invalid = none)
    core::text::TextSelection m_textSel;                    // caret/selection, byte offsets into the block
    std::vector<core::text::MisspelledRange> m_textMisspelled; // spell squiggles, byte ranges (deferred §2)
    double m_textDesiredInline = -1.0; // caret goal for line-crossing motion: the kept INLINE coord
                                       // (layer-local x horizontal, y down the column for vertical)
    std::uint64_t m_textEditCoalesce = 0;                   // SetTextCommand coalesce id (bumped per burst)
    core::text::ShapedBlock m_textShaped;                   // cached layout of the edited block (for geometry)
    std::uint64_t m_textShapedRev = static_cast<std::uint64_t>(-1); // contentRevision the cache reflects
    bool m_textBlinkOn = true;                              // caret blink phase (drawn when true)
    double m_textBlinkAt = 0.0;                             // wall-clock time of the next blink toggle
    bool m_textSelecting = false;                           // a click-drag selection is in flight
    bool m_textCreating = false;                            // a press has anchored a create gesture
    common::Vec2 m_textCreatePressDoc{};                   // create-gesture press point, document space
    common::Vec2 m_textCreateDragDoc{};                    // latest drag point (event-time), document space
    bool m_textCreateDragged = false;                       // moved past the dead zone -> Area, not Point
    common::Vec2 m_lastAreaBoxSize{0, 0};                   // last Area box size (the §7 reuse-size toggle)
    // S29-c: the input style picked at a bare caret (font/size/colour set before typing). Used only
    // while the caret stays put (focus == m_textPendingAt && empty selection); any caret move silently
    // drops it. insertTextAtCaret consumes it for the next run; the bar/panel read it for the readout.
    // onSelectionChanged change-detection: the (target, selection, block revision) last reported, so
    // syncTextOverlay() fires the host callback only when one actually changed.
    core::LayerId m_textNotifiedTarget = core::kInvalidLayerId;
    core::text::TextSelection m_textNotifiedSel;
    std::uint64_t m_textNotifiedRev = static_cast<std::uint64_t>(-1);
    // Transient font-hover preview (S29-c §8): the original block saved while a display-only preview is
    // live, restored on clear. The bar/panel notify is suppressed while active (it is not a commit).
    bool m_stylePreviewActive = false;
    core::text::TextBlock m_stylePreviewOriginal;

    // Type-edit box gesture (move/rotate via TransformGesture writing the layer transform; resize via
    // a block edit). Distinct from the Move tool's gizmo: Move stretches, Type sizes (§7).
    TextBoxControl m_textBoxCtl = TextBoxControl::None; // the grabbed control (None = idle)
    TransformGesture m_textBox;                  // active for Move/Rotate
    common::Affine2D m_textBoxBase;              // edit layer's WORLD transform at press
    common::Affine2D m_textBoxBaseInv;           // its inverse (cursor doc -> layer-local; valid in-gesture)
    common::Affine2D m_textBoxParentInv;         // parent-world inverse: world result -> layer transform
    common::Rect m_textBoxContent;               // the framed box (layer-local) at press
    core::text::TextBlock m_textBoxBaseBlock;    // the block at press (resize scales a copy each frame)
    common::Vec2 m_textBoxPressScreen{};         // press point, logical screen px (drag dead zone)
    bool m_textBoxLatched = false;               // moved past the dead zone (a real drag, not a click)
    std::uint64_t m_textBoxCoalesce = 0;         // this gesture's coalesce id (one undo step)
    common::Vec2 m_textBendPressLocal{};         // bend gesture: press point in layer-local space
    double m_textBendW = 1.0;                     // bend gesture: flat baseline span (drag -> bend gain)
    // Bend/bracket gesture over a 3D block: the plane map captured at PRESS time, so every drag
    // event unprojects through the SAME cap plane. Re-deriving it live would move the pivot as the
    // edit reshapes the bounds -- the mapping would feed back into itself and the handle would creep.
    std::optional<core::text::ExtrudePlaneMap> m_textBendPlane;
    double m_textPathPressS = 0.0; // bracket gesture: the press point's arc-distance (continuity ref)
    common::Vec2 m_textBoxPressLocal{};          // resize gesture: press point (layer) -> scale anchor
};

} // namespace mosaic::ui
