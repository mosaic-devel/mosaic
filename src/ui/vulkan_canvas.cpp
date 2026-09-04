#include "ui/vulkan_canvas.hpp"

#include "ui/brush_reticle.hpp"

#include "common/i18n.hpp" // _() for the Type tool's right-click menu labels
#include "common/log.hpp"
#include "common/profiler.hpp" // MOSAIC_PERF_SCOPE (runtime-gated in every build)
#include "core/brush/tip_outline.hpp" // buildTipSdf/tipNeedsSdf -- the reticle traces the real tip
#include "core/commands.hpp" // AddGuide/RemoveGuide/MoveGuide commands (View -> Guides)
#include "core/document.hpp"
#include "core/layer_grow.hpp"    // the bounded brush auto-grow + the absurd-coordinate guards
#include "core/layer_marquee.hpp" // layersInMarquee -- the Move tool's empty-space drag marquee
#include "core/retarget/recompose.hpp" // solvePlacements — the Recompose offer's feasibility test
#include "core/stroke_confinement.hpp" // the active selection, resampled onto the stroke's grid
#include "core/text/language.hpp" // resolveLanguage (spell-check menu word language, deferred §2)
#include "core/vector/flatten.hpp" // flatten/samplers -- fit-to-path creation + brackets (S30 §9)
#include "platform/native_window.hpp"
#include "platform/wayland_subsurface.hpp"
#include "render/window_renderer.hpp"
#include "ui/color_flyout.hpp" // dismissActiveColorFlyout (a work-area click closes the colour bubble)
#include "ui/color_state.hpp"  // hexString: the eyedropper loupe's hex readout (S24)
#include "ui/idle_invitation.hpp" // the baked open-an-image atlas (documentless idle pass)
#include "ui/menu_bar.hpp"
#include "ui/popover.hpp"
#include "ui/theme.hpp" // activePalette() -> the rotate cursor's two-tone follows the theme
#include "ui/tool.hpp"
#include "ui/widgets.hpp" // ui::ContextMenu -> the Type tool's right-click Cut/Copy/Paste menu

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace mosaic::ui {
namespace {
spdlog::logger& uiLog() {
    static const auto logger = common::log::category("ui");
    return *logger;
}

// The Shape tool id authoring a given ShapeKind (inverse of shapeKindFor) -- used to switch the
// Shape slot to a clicked shape's kind for select-to-edit (S26-b §7.1). nullopt for a kind with no
// toolbar variant of its own (the S26-c shape library is reached through the designer's gallery, not
// the slot): the caller then binds the shape for editing WITHOUT switching tools, which is right --
// there is no bar to switch to, and the designer edits it either way.
std::optional<ToolId> toolIdForKind(ShapeKind kind) {
    switch (kind) {
    case ShapeKind::Rect: return ToolId::RectShape;
    case ShapeKind::Ellipse: return ToolId::EllipseShape;
    case ShapeKind::Polygon: return ToolId::PolygonShape;
    case ShapeKind::Star: return ToolId::StarShape;
    case ShapeKind::Line: return ToolId::LineShape;
    default: return std::nullopt;
    }
}

// Rasterize `text` with the real UI font into an RGBA overlay tile (S16 rework): anti-aliased white
// text over an optional dark pill, drawn at the TOP-LEFT of a fixed `capW`x`capH` buffer (so the
// renderer's texture size stays stable across a drag and never reallocates). `outW`/`outH` get the
// used content size. FLTK's offscreen surface is RGB only, so we draw white-on-black and read the
// coverage from a colour channel, then composite white text over a translucent black pill
// ourselves.
std::vector<std::uint8_t> rasterizeOverlayTile(const std::string& text, bool withPill, double scale,
                                               int fontPx, int capW, int capH, int& outW,
                                               int& outH) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(capW) * capH * 4, 0); // transparent
    fl_font(FL_HELVETICA, fontPx);
    int tw = 0, th = 0;
    fl_measure(text.c_str(), tw, th, 0);
    const int padX = static_cast<int>(std::lround((withPill ? 9.0 : 3.0) * scale));
    const int padY = static_cast<int>(std::lround((withPill ? 5.0 : 3.0) * scale));
    const int cw = std::min(capW, tw + 2 * padX);
    const int ch = std::min(capH, th + 2 * padY);
    outW = cw;
    outH = ch;

    Fl_Image_Surface surf(cw, ch);
    Fl_Surface_Device::push_current(&surf);
    fl_color(FL_BLACK);
    fl_rectf(0, 0, cw, ch);
    fl_color(FL_WHITE);
    fl_font(FL_HELVETICA, fontPx);
    fl_draw(text.c_str(), padX, padY + fl_height() - fl_descent()); // y = baseline
    Fl_RGB_Image* img = surf.image();
    Fl_Surface_Device::pop_current();
    if (img == nullptr)
        return rgba;

    const auto* src = static_cast<const std::uint8_t*>(static_cast<const void*>(img->array));
    const int d = img->d() > 0 ? img->d() : 3;
    const int ld = img->ld() != 0 ? img->ld() : cw * d;

    // Text coverage (white-on-black) per pixel, copied out so we can dilate it for the outline
    // below.
    std::vector<float> tcov(static_cast<std::size_t>(cw) * ch, 0.0f);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x)
            tcov[static_cast<std::size_t>(y) * cw + x] = src[y * ld + x * d] / 255.0f;
    delete img;

    // The HUD draws white text over a translucent dark pill. The dial readout (no pill) instead
    // gets a dark 1px OUTLINE so it reads on any canvas content without a box: dilate the text
    // coverage by a ~1px ring, composite the dark ring, then the white text on top (user
    // experiment, 2026-06-14).
    const float pillA = withPill ? 0.62f : 0.0f;
    const int outlineR = withPill ? 0 : std::max(1, static_cast<int>(std::lround(scale)));
    const float outlineA = 0.85f; // opacity of the dark outline ring
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            const float t = tcov[static_cast<std::size_t>(y) * cw + x];
            float a;
            if (outlineR > 0) {
                float o = t; // dilated coverage: max over the (2r+1)^2 neighbourhood
                for (int dy = -outlineR; dy <= outlineR; ++dy)
                    for (int dx = -outlineR; dx <= outlineR; ++dx) {
                        const int yy = y + dy, xx = x + dx;
                        if (yy >= 0 && yy < ch && xx >= 0 && xx < cw)
                            o = std::max(o, tcov[static_cast<std::size_t>(yy) * cw + xx]);
                    }
                a = t + o * outlineA * (1.0f - t); // white text over a dark ring
            } else {
                a = t + pillA * (1.0f - t); // white text over the translucent pill
            }
            const float white =
                a > 0.0f ? t / a : 0.0f; // straight-alpha colour: outline black, text white
            const auto c = static_cast<std::uint8_t>(std::lround(255.0f * white));
            const std::size_t off = (static_cast<std::size_t>(y) * capW + x) * 4;
            rgba[off + 0] = c;
            rgba[off + 1] = c;
            rgba[off + 2] = c;
            rgba[off + 3] = static_cast<std::uint8_t>(std::lround(255.0f * a));
        }
    }
    return rgba;
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kRotateSnapRad = 5.0 * kPi / 180.0; // Shift snaps rotation to 5 degrees
constexpr double kDoubleTapSeconds = 0.35;           // double-tap-R window
constexpr double kAntsSpeedPxPerSec = 12.0;          // marching-ants crawl speed (S13)
// One notch of zoom. Shared by the mouse wheel and the Zoom tool's click so that a click and a
// wheel notch are the same size of step -- two different-sized "one step"s in one app is the kind
// of thing nobody can name but everybody feels.
constexpr double kZoomClickStep = 1.2;
constexpr double kPolyCloseScreenPx = 8.0;  // a poly-lasso click this close to the start closes it
constexpr double kHandleHitPx = 7.0;        // Move tool: handle grab radius, logical px (S15)
constexpr double kRotateBandPx = 18.0;      // ... and the rotate band hugging each corner
constexpr double kAnchorHitPx = 8.0;        // ... the transform anchor's grab radius (S15+ pivot)
constexpr double kAnchorSnapPx = 7.0;       // ... anchor snaps to a handle/centre within this (screen)
constexpr double kMoveDragDeadZonePx = 3.0; // cursor travel before a press becomes a drag
constexpr double kTextBoxEdgeBandPx = 5.0;  // Type-edit box: the frame-edge MOVE band (interior=caret)
constexpr double kDofLineHitPx = 4.0;       // DoF gizmo: guide-line grab distance, logical px (S33)
constexpr double kDofRotateKnobPx = 64.0;   // ... and the rotate knob's fixed offset along the line

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// The stock resize cursor (m_cursorState 11-14) whose axis best matches handle `i`'s screen
// direction, so rotated quads still feel right. Shared by the Move tool's handles and the
// crop rect's (the view can rotate the crop quad on screen too).
int resizeCursorFor(const std::array<common::Vec2, 4>& corners, int handle) {
    const std::array<common::Vec2, 8> handles = transformHandleCenters(corners);
    const common::Vec2 center = (corners[0] + corners[2]) * 0.5;
    const common::Vec2 d = handles[static_cast<std::size_t>(handle)] - center;
    double deg = std::atan2(d.y, d.x) * 180.0 / kPi; // screen-space direction of the pull
    deg = std::fmod(deg + 360.0, 180.0);             // axis, not direction
    if (deg < 22.5 || deg >= 157.5)
        return 12; // WE
    if (deg < 67.5)
        return 13; // NWSE
    if (deg < 112.5)
        return 11; // NS
    return 14;     // NESW
}

// Map a layer-local pixel rect (e.g. a brush engine's dirty region) to its document-space
// bounding box through the layer's world transform `fwd` -- the four transformed corners' extent,
// then floored/ceiled to whole document pixels. Empty in => empty out. Used so a brush stroke can
// recomposite just the touched region (S60-a) even on a rotated/scaled layer.
common::Rect localRectToDocBBox(const common::Rect& local, const common::Affine2D& fwd) {
    if (local.empty())
        return {};
    const common::Vec2 c[4] = {fwd.apply({local.x, local.y}),
                               fwd.apply({local.right(), local.y}),
                               fwd.apply({local.right(), local.bottom()}),
                               fwd.apply({local.x, local.bottom()})};
    double minx = c[0].x, miny = c[0].y, maxx = c[0].x, maxy = c[0].y;
    for (int i = 1; i < 4; ++i) {
        minx = std::min(minx, c[i].x);
        miny = std::min(miny, c[i].y);
        maxx = std::max(maxx, c[i].x);
        maxy = std::max(maxy, c[i].y);
    }
    const double x0 = std::floor(minx), y0 = std::floor(miny);
    return {x0, y0, std::ceil(maxx) - x0, std::ceil(maxy) - y0};
}
} // namespace

VulkanCanvas::VulkanCanvas(int X, int Y, int W, int H) : Fl_Window(X, Y, W, H) {
    end(); // no FLTK children: Vulkan owns this surface
}

VulkanCanvas::~VulkanCanvas() {
    Fl::remove_timeout(clearResetHudCb, this); // never fire the reset-HUD timeout on a dead canvas
    if (m_renderer) {
        m_renderer->waitIdle();
        if (m_onRendererShutdown)
            m_onRendererShutdown(); // borrowers of our device go while it is still alive
    }
    m_renderer.reset();   // destroys the VkSurfaceKHR ...
#if !defined(__APPLE__) && !defined(_WIN32)
    m_subsurface.reset(); // ... before the wl_surface it was built on
#endif
}

void VulkanCanvas::ensureTabletInput() {
    if (shown() == 0)
        return; // both backends need the shown window's native handle
    // ONE lifecycle on both platforms: the pen fills a ring, FLTK's FL_PUSH/FL_DRAG/FL_MOVE/
    // FL_RELEASE stream drives the tools, and they drain the ring for pressure. Wayland gets that
    // stream because the wiring SYNTHESIZES it (the compositor stops emulating pointer events for a
    // pen once the tablet manager is bound -- §4 finding 4); X11 gets it from the X server. init()
    // is a no-op the second time, and a failure is not one: with no backend the canvas keeps
    // painting from synthesized pressure-1 samples, exactly as it did before any of this (§3.2).
    m_tablet.init(this);
}

void VulkanCanvas::ensureRenderer() {
    ensureTabletInput(); // needs the same shown-window handle, and must not wait on the swapchain
    if (m_renderer || m_initFailed)
        return;
    if (shown() == 0)
        return; // native handle not ready yet

    platform::NativeSurfaceHandle handle;
    std::string err;
    if (!platform::nativeSurfaceHandle(this, handle, err)) {
        m_initFailed = true;
        m_error = err;
        uiLog().error("canvas surface unavailable: {}", err);
        return;
    }
    m_contentScale = handle.scale > 0 ? handle.scale : 1;
    uiLog().info("canvas content scale: {}x ({}x{} logical -> {}x{} physical)", m_contentScale, w(),
                 h(), handle.pixelWidth, handle.pixelHeight);

    // Native Wayland: the swapchain must present to a dedicated child surface, because Mesa's
    // WSI cannot attach to FLTK's own wl_surface (it aborts on wp_linux_drm_syncobj_surface_v1).
    // On X11/XWayland the window's surface is used directly. See platform::WaylandSubsurface.
    // (Neither macOS nor Windows has Wayland: the CAMetalLayer from native_window_macos.mm and the
    // HWND from native_window_win32.cpp are each used directly.)
#if !defined(__APPLE__) && !defined(_WIN32)
    if (handle.system == platform::WindowSystem::Wayland) {
        m_subsurface = platform::WaylandSubsurface::create(this, err);
        if (!m_subsurface) {
            m_initFailed = true;
            m_error = err;
            uiLog().error("canvas Wayland subsurface failed: {}", err);
            return;
        }
        handle.window = m_subsurface->surface();
    }
#endif

    m_renderer = render::WindowRenderer::create(handle, /*enableValidation=*/true, err);
    if (!m_renderer) {
        m_initFailed = true;
        m_error = err;
        uiLog().error("Vulkan canvas init failed: {}", err);
        return;
    }
    uiLog().info("Vulkan canvas on {}", m_renderer->deviceName());
    // Settings applied before the (lazy) renderer existed land now.
    m_renderer->setOverlayLineStyle(m_overlayLineStyle);
    m_renderer->setFeatherIndicator(m_featherIndicator);
    m_renderer->setAntsCirculate(m_antsCirculate);
}

void VulkanCanvas::setDocumentImage(const common::Image& img, bool fitView) {
    m_documentImage = img;
    m_documentPending = true;
    if (!img.empty()) {
        m_view.setDocumentSize({static_cast<double>(img.width), static_cast<double>(img.height)});
        if (fitView || !m_viewInitialized) {
            m_view.setViewportSize({static_cast<double>(w()), static_cast<double>(h())});
            m_view.fit(); // a new/first document opens fitted to the viewport
            m_viewInitialized = true;
            notifyViewChanged();
        }
    }
}

void VulkanCanvas::adoptResidentDocument(std::uint32_t docW, std::uint32_t docH, bool fitView) {
    // setDocumentImage's resident twin (S60-a item 13). The pixels are ALREADY in the canvas
    // texture -- TileCompositor::resolve() wrote them on the device -- so there is nothing to copy
    // and nothing to stage; what is left is the view bookkeeping, which is identical.
    if (docW == 0 || docH == 0)
        return;
    // ⚠ Drop anything the CPU lane queued for this frame. Two writers to one texture in one frame
    // is how a resident path silently reinstates the copy it exists to delete -- and the stale one
    // would win, because renderFrame uploads after the resolve has already happened.
    m_documentImage = common::Image{};
    m_documentPending = false;
    m_documentRegion = common::Image{};
    m_documentRegionPending = false;
    m_view.setDocumentSize({static_cast<double>(docW), static_cast<double>(docH)});
    if (fitView || !m_viewInitialized) {
        m_view.setViewportSize({static_cast<double>(w()), static_cast<double>(h())});
        m_view.fit();
        m_viewInitialized = true;
        notifyViewChanged();
    }
}

void VulkanCanvas::setDocumentRegion(const common::Image& sub, std::uint32_t x, std::uint32_t y) {
    // S60-a: patch a sub-rect of the canvas, no view change. A full document image queued for this
    // frame already carries the region, so don't also issue a partial patch over it.
    if (m_documentPending || sub.empty())
        return;
    m_documentRegion = sub;
    m_documentRegionX = x;
    m_documentRegionY = y;
    m_documentRegionPending = true;
}

bool VulkanCanvas::canHostGpuDrag(std::uint32_t belowW, std::uint32_t belowH,
                                  std::uint32_t dragW, std::uint32_t dragH) const {
    return m_renderer && m_renderer->canHostDragTextures(belowW, belowH, dragW, dragH);
}

bool VulkanCanvas::beginGpuDrag(const common::Image& below, const common::Image& dragged) {
    if (!m_renderer)
        return false; // renderer not up yet -> caller uses the CPU path
    m_renderer->beginGpuDrag(below, dragged);
    return m_renderer->gpuDragActive();
}

void VulkanCanvas::setGpuDragTransform(const common::Affine2D& docToLayerLocal, int blendMode,
                                       float opacity) {
    if (m_renderer)
        m_renderer->setGpuDragTransform(docToLayerLocal, blendMode, opacity);
}

void VulkanCanvas::endGpuDrag() {
    if (m_renderer)
        m_renderer->endGpuDrag();
}

void VulkanCanvas::notifyViewChanged() {
    if (m_onViewChanged)
        m_onViewChanged();
}

void VulkanCanvas::requestHostFrame() {
    if (m_requestFrame)
        m_requestFrame();
}

void VulkanCanvas::notifyCursor(bool overCanvas) {
    // Track the pointer for the brush reticle on EVERY move/drag (FL_DRAG never fires FL_MOVE, so
    // the ring would otherwise freeze while the button is held over a non-stroking layer -- e.g. a
    // locked one). Done before the m_onCursor early-out so it runs regardless of the status-bar
    // callback.
    if (overCanvas)
        m_cursorLogical = eventLogicalPoint();
    emitCursor(overCanvas);
}

void VulkanCanvas::emitCursor(bool overCanvas) {
    // notifyCursor() minus the Fl::event_x/y read: the tablet sink already knows where the pen is
    // (sub-pixel, and on Wayland there is no FLTK event to read it from anyway), so it sets
    // m_cursorLogical itself and calls this.
    if (!overCanvas) {
        // Off the canvas the pointer has no heading. Coming back in must not resume the one it left
        // with -- the ring would show a direction the pointer is no longer travelling in.
        m_hoverHeading.reset();
        if (m_onCursor)
            m_onCursor(0.0, 0.0, false);
        return;
    }
    const common::Vec2 doc = m_view.toDoc(m_cursorLogical);
    // Every move, mouse or pen, on both backends: this is the one funnel both paths go through, and
    // the reticle's heading has to be updated even when nothing is listening for the status bar.
    m_hoverHeading.moveTo(m_cursorLogical.x, m_cursorLogical.y, doc.x, doc.y);
    if (m_onCursor)
        m_onCursor(doc.x, doc.y, true);
}

void VulkanCanvas::setSelectionMask(std::uint32_t w, std::uint32_t h,
                                    const std::uint8_t* coverage) {
    if (w == 0 || h == 0 || coverage == nullptr) {
        m_selectionMask.clear();
        m_selectionW = m_selectionH = 0;
    } else {
        m_selectionMask.assign(coverage, coverage + static_cast<std::size_t>(w) * h);
        m_selectionW = w;
        m_selectionH = h;
    }
    m_selectionPending = true;
    // Mask changes from outside the frame loop (Select menu, gesture commit/cancel) should
    // show this frame, not the next heartbeat; in-frame calls are absorbed by the host guard.
    requestHostFrame();
}

// ---- S14 selection tools --------------------------------------------------------------------
//
// The canvas owns the pointer half of the marquee/lasso tools: it maps events to document
// coordinates, drives the (pure, unit-tested) SelectionGesture, and previews straight to its own
// mask -- at most once per frame, in renderFrame(). The document half stays with the host: the
// commit callback pushes the gesture's single SetSelectionCommand (UI-coalescing, per S13).

std::optional<SelectionGesture::Kind> VulkanCanvas::activeSelectionKind() const {
    if (m_tools == nullptr)
        return std::nullopt;
    switch (m_tools->active()) {
    case ToolId::RectMarquee:
        return SelectionGesture::Kind::Rect;
    case ToolId::EllipseMarquee:
        return SelectionGesture::Kind::Ellipse;
    case ToolId::Lasso:
        return SelectionGesture::Kind::FreeLasso;
    case ToolId::PolygonLasso:
        return SelectionGesture::Kind::PolyLasso;
    default:
        return std::nullopt;
    }
}

const core::Selection& VulkanCanvas::baseSelection() const {
    static const core::Selection kNone;
    const core::Selection* sel = m_selectionHost.base ? m_selectionHost.base() : nullptr;
    return sel != nullptr ? *sel : kNone;
}

namespace {

// Is `event` one whose Fl::event_x/y describe where the POINTER is? FLTK translates the pair into
// a sub-window's own frame for every event it sends down (Fl_Group::send), but the keyboard family
// is routed to the FOCUS widget carrying whatever coordinates the last pointer event left behind --
// against the TOP-LEVEL window, so they arrive with the canvas's origin baked in (updateToolCursor
// has carried that warning since the S24 reticle bug). FL_PASTE / focus events have no position at
// all. Only the events listed here open the window in which eventLogicalPoint() may read FLTK.
bool eventCarriesLocalPosition(int event) noexcept {
    switch (event) {
    case FL_ENTER:
    case FL_MOVE:
    case FL_LEAVE:
    case FL_PUSH:
    case FL_DRAG:
    case FL_RELEASE:
    case FL_MOUSEWHEEL:
    case FL_DND_ENTER:
    case FL_DND_DRAG:
    case FL_DND_LEAVE:
    case FL_DND_RELEASE:
        return true;
    default:
        return false;
    }
}

// RAII bracket around handle()'s positional dispatch. handle() returns from ~40 places, so the
// counter has to be scoped, not hand-balanced. A counter rather than a flag: FLTK can re-enter
// handle() (a nested Fl::wait() inside a gesture's host callback), and the inner scope must not
// close the outer one's window.
class PointerFrame {
public:
    PointerFrame(int& depth, int event)
        : m_depth(eventCarriesLocalPosition(event) ? &depth : nullptr) {
        if (m_depth != nullptr)
            ++*m_depth;
    }
    ~PointerFrame() {
        if (m_depth != nullptr)
            --*m_depth;
    }
    PointerFrame(const PointerFrame&) = delete;
    PointerFrame& operator=(const PointerFrame&) = delete;

private:
    int* m_depth;
};

} // namespace

common::Vec2 VulkanCanvas::eventLogicalPoint() const {
    // Inside our own positional dispatch the FLTK pair IS canvas-local: Fl_Group::send subtracted
    // our x()/y() on the way in. Outside it the pair is stale AND in the top-level's frame, so the
    // tracked pointer -- which only ever gets written from in here -- is the honest answer. Both
    // branches therefore return the SAME point for the same pointer position; the fallback is not
    // an approximation, it is the same number carried across the frame boundary.
    if (m_pointerFrameDepth > 0)
        return {static_cast<double>(Fl::event_x()), static_cast<double>(Fl::event_y())};
    return m_cursorLogical;
}

common::Vec2 VulkanCanvas::eventDocPoint() const {
    return m_view.toDoc(eventLogicalPoint());
}

common::Vec2 VulkanCanvas::cursorDocPoint() const {
    // The TRACKED pointer (m_cursorLogical) mapped to doc coords -- for the hover/cursor-shape
    // tests, which run from keyboard contexts too, where Fl::event_x/y are relative to the
    // top-level window rather than the canvas (see updateToolCursor). The pointer events keep
    // m_cursorLogical current; this never reads the live event.
    return m_view.toDoc(m_cursorLogical);
}

void VulkanCanvas::restoreDocumentMask() {
    const core::Selection& sel = baseSelection();
    if (sel.isEmpty())
        setSelectionMask(0, 0, nullptr);
    else
        setSelectionMask(sel.width(), sel.height(), sel.data().data());
}

void VulkanCanvas::commitSelection(core::Selection sel, std::uint64_t coalesce,
                                   std::string_view label) {
    if (m_selectionHost.commit)
        m_selectionHost.commit(std::move(sel), coalesce, label);
}

void VulkanCanvas::cancelSelectionGesture() {
    endNudgeSession();
    if (m_selMove.active())
        cancelSelectionMove();
    if (m_maskStrokeActive) {
        // S18: a tool switch / new document mid-stroke drops the in-flight coverage and re-shows the
        // document's own ants (the preview never touched the command stack).
        m_maskStroke.end();
        m_maskStrokeActive = false;
        m_maskStrokePreviewDirty = false;
        restoreDocumentMask();
        updateToolCursor(m_pointerInside);
    }
    if (m_edgeStrokeActive) {
        // L1: same drop for an in-flight edge-brush seed trail -- no grow ever ran, nothing landed.
        m_edgeStroke.end();
        m_edgeStrokeActive = false;
        m_edgeStrokePreviewDirty = false;
        restoreDocumentMask();
        updateToolCursor(m_pointerInside);
    }
    if (m_redEyeStrokeActive) {
        // S38-b: same drop for an in-flight eye-retouch scope -- no correction ever ran, and the
        // trail preview only ever rode the mask channel, so nothing reached the command stack.
        m_redEyeStroke.end();
        m_redEyeStrokeActive = false;
        m_redEyeStrokePreviewDirty = false;
        restoreDocumentMask();
        updateToolCursor(m_pointerInside);
    }
    if (!m_gesture.active())
        return;
    m_gesture.cancel();
    restoreDocumentMask();
    updateToolCursor(m_pointerInside); // back from the latched badge (or off a switched tool)
}

void VulkanCanvas::finishSelectionGesture() {
    const common::Vec2 size = m_view.documentSize();
    std::optional<core::Selection> result = m_gesture.finish(
        baseSelection(), static_cast<std::uint32_t>(size.x), static_cast<std::uint32_t>(size.y));
    if (result)
        commitSelection(std::move(*result), 0, "Select"); // the host re-syncs the mask after the push
    else
        restoreDocumentMask(); // nothing committed: drop the preview, re-show the document's ants
}

void VulkanCanvas::pushSelectionGesture(SelectionGesture::Kind kind) {
    const common::Vec2 p = eventDocPoint();
    const auto state = Fl::event_state();
    const bool shift = (state & FL_SHIFT) != 0;
    const bool alt = (state & FL_ALT) != 0;
    const core::SelectOp op = selectOpForModifiers(shift, (state & FL_CTRL) != 0, alt);
    // A modifier-free press inside the ants GRABS the selection instead of starting a new marquee
    // (S16-i). The modifiers keep their S14 meaning, so Shift/Ctrl/Alt still draw a fresh shape
    // there -- which is how one adds to or subtracts from a selection from the inside.
    if (!m_gesture.active() && op == core::SelectOp::Replace && pointInSelection(p)) {
        beginSelectionMove(p);
        return;
    }
    endNudgeSession(); // a pointer gesture ends any arrow-key burst: the next one is its own step
    if (kind != SelectionGesture::Kind::PolyLasso) {
        m_gesture.beginDrag(kind, op, p, shift, alt);
        return;
    }
    if (m_gesture.phase() != SelectionGesture::Phase::Placing) {
        m_gesture.beginPoly(op, p);
        return;
    }
    // The zoom maps the screen-px close tolerance to document px (rotation preserves lengths).
    const double closeRadius = kPolyCloseScreenPx / std::max(m_view.zoom(), CanvasView::kMinZoom);
    if (m_gesture.shouldClose(p, closeRadius, Fl::event_clicks() > 0))
        finishSelectionGesture();
    else
        m_gesture.addVertex(p,
                            shift); // Shift snaps the new segment's angle (op was latched at begin)
}

// ---- S16-i: move the selection outline -------------------------------------------------------
//
// Distinct from the Move tool: this drags the MASK, leaving the pixels where they are. The grab is
// modifier-free inside the ants (see pushSelectionGesture) and the arrow keys nudge by whole
// document pixels -- document, not screen, axes: the mask stores integer doc pixels, so a rotated
// view cannot be honoured without resampling the coverage. Both routes share SelectionMoveGesture,
// which translates the press-time mask rather than the previous result (lossless across an edge).

bool VulkanCanvas::pointInSelection(common::Vec2 doc) const {
    const core::Selection& sel = baseSelection();
    if (sel.isEmpty())
        return false;
    const double fx = std::floor(doc.x);
    const double fy = std::floor(doc.y);
    if (fx < 0.0 || fy < 0.0 || fx >= static_cast<double>(sel.width()) ||
        fy >= static_cast<double>(sel.height())) {
        return false;
    }
    return sel.at(static_cast<std::uint32_t>(fx), static_cast<std::uint32_t>(fy)) >=
           core::kAntsCoverageThreshold;
}

bool VulkanCanvas::selectionMoveHover() const {
    if (!activeSelectionKind() || m_gesture.active())
        return false;
    const auto s = Fl::event_state();
    if ((s & (FL_SHIFT | FL_CTRL | FL_ALT)) != 0)
        return false; // a modifier means "draw a new shape here", not "grab"
    return pointInSelection(m_view.toDoc(m_cursorLogical));
}

void VulkanCanvas::beginSelectionMove(common::Vec2 doc) {
    endNudgeSession();
    m_selMove.begin(baseSelection(), doc);
    m_selMoveDirty = false; // offset is still zero: the document's own mask is already on screen
}

void VulkanCanvas::finishSelectionMove() {
    std::optional<core::Selection> result = m_selMove.finish();
    if (result)
        commitSelection(std::move(*result), 0, "Move Selection");
    else
        restoreDocumentMask(); // a click that never moved: nothing to undo, just re-show the ants
    m_selMoveDirty = false;
}

void VulkanCanvas::cancelSelectionMove() {
    if (!m_selMove.active())
        return;
    m_selMove.cancel();
    m_selMoveDirty = false;
    restoreDocumentMask();
    updateToolCursor(m_pointerInside);
}

void VulkanCanvas::endNudgeSession() {
    if (!m_selMove.active() || m_selMove.dragging())
        return; // no session, or the pointer owns the gesture
    m_selMove.cancel();
    m_nudgeCoalesce = 0;
    m_selMoveDirty = false;
}

void VulkanCanvas::nudgeSelection(long dx, long dy) {
    // A burst continues only while the document's selection is still the one our last commit made.
    // Anything else touching it (undo, the Select menu, a marquee) invalidates the cached base, so
    // the next arrow opens a fresh undo step against the new mask.
    const std::uint64_t rev = m_selectionHost.revision ? m_selectionHost.revision() : 0;
    const bool sessionLive = m_selMove.active() && !m_selMove.dragging() && rev == m_nudgeRev;
    if (!sessionLive) {
        endNudgeSession();
        if (baseSelection().isEmpty())
            return;
        m_selMove.beginNudge(baseSelection());
        m_nudgeCoalesce = ++m_nudgeCoalesceNext; // monotonic: a new burst can never merge into an old
    }
    m_selMove.nudge(dx, dy);
    commitSelection(m_selMove.current(), m_nudgeCoalesce, "Move Selection");
    // Learn the revision our own commit produced, so the next arrow recognises the session.
    m_nudgeRev = m_selectionHost.revision ? m_selectionHost.revision() : 0;
    requestHostFrame();
}

// ---- S15 Move tool -------------------------------------------------------------------------
//
// Affinity-style: click an object to select it (mirrored into the Layers panel), drag the body
// to move, a handle to scale, just outside a corner to rotate; click empty space to drop the
// handles. SHIFT-click gathers several layers into the selection (toggle: add if absent, remove
// if present — the canvas-object norm of Photoshop/Illustrator/Figma) and they drag, scale and
// rotate together as a set WITHOUT being grouped (S15-c). The transform math lives in
// ui::TransformGesture (pure), run once in the framing box's frame; the resulting box delta is
// applied to every selected layer's press-time world transform and pushed as ONE coalescing
// SetTransformsCommand, so undo treats the whole multi-layer gesture as a single step and Esc
// can restore the bases mid-drag.

bool VulkanCanvas::moveToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::Move;
}

bool VulkanCanvas::magicWandToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::MagicWand;
}

void VulkanCanvas::pushMagicWand() {
    // A single click: map it to a document point, read the press-time modifiers into a boolean op
    // (the same S14 semantics the marquee/lasso use), and hand it to the host, which owns the seed
    // read, the flood, the combine and the single SetSelectionCommand (docs/research-selection.md §8).
    if (!m_magicWandHost.click)
        return;
    const auto state = Fl::event_state();
    const core::SelectOp op = selectOpForModifiers((state & FL_SHIFT) != 0, (state & FL_CTRL) != 0,
                                                   (state & FL_ALT) != 0);
    m_magicWandHost.click(eventDocPoint(), op);
}

bool VulkanCanvas::bucketFillToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::BucketFill;
}

void VulkanCanvas::pushBucketFill() {
    // A single click, no gesture (like the wand): map it to a document point and hand it to the
    // host, which owns the seed read, the tolerance flood, the selection intersection and the one
    // core::FillCommand (docs/bucket-fill.md §2). No modifiers -- the bucket fills the active
    // colour.
    if (m_bucketFillHost.click)
        m_bucketFillHost.click(eventDocPoint());
}

// ---- S24 Eyedropper + loupe -----------------------------------------------------------------
//
// The loupe's fixed on-screen geometry (canvas widget logical px; the renderer applies the content
// scale). The magnification is chosen so the disk shows ~8 document texels across -- enough to see a
// pixel and its neighbours, the "nearest pixels around the cursor" the spec asks for.
namespace {
constexpr double kLoupeRadiusLogical = 58.0; // the disk radius
constexpr double kLoupeMagLogical = 14.0;    // logical px per document texel inside the disk
} // namespace

bool VulkanCanvas::eyedropperToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::Eyedropper;
}

bool VulkanCanvas::temporaryEyedropperActive() const {
    // Holding CTRL with a stroke tool (brush/eraser/inpaint) active engages the eyedropper for as
    // long as it is held -- the Space-pan convention: a temporary mode, nothing latched, and the
    // brush returns exactly as it was on release. Never mid-stroke: a stroke in flight owns the
    // pointer, so Ctrl pressed while painting must not hijack the drag (the loupe waits for the
    // release). Deliberately gated on the STROKE family only -- everywhere else Ctrl keeps its
    // meaning (the selection tools' boolean op, the crop tool's Smart-Resize keep-chip drag, the
    // text session's word-step accelerators). Ctrl is read from the live modifier state, which the
    // main window's fan-out keeps post-event-true, so the loupe follows the key with no pointer
    // motion needed (modifiersChanged kicks the frame).
    // ⚠ NOT the Clone stamp. There the very same chord PICKS THE SOURCE (S38, the user's own
    // specification), and a tool cannot have two meanings for one modifier -- so the clone stamp is
    // the one stroke-family tool that trades the temporary eyedropper away. It is the right trade:
    // a clone tool is unusable without a source-pick gesture, and Ctrl is the gesture the user asked
    // for.
    return strokeToolActive() && !cloneToolActive() && (Fl::event_state() & FL_CTRL) != 0 &&
           !m_brushStroking && !m_brushPressPending;
}

// fg vs bg for an eyedropper press/drag: Alt or the right button targets the background. Checks both
// the current button (reliable on FL_PUSH) and the held-button state mask (reliable mid-FL_DRAG).
static bool eyedropperToBackground() {
    return (Fl::event_state() & FL_ALT) != 0 || Fl::event_button() == FL_RIGHT_MOUSE ||
           (Fl::event_state() & FL_BUTTON3) != 0;
}

void VulkanCanvas::pushEyedropper() {
    // One click samples the colour under the cursor into the foreground swatch; Alt (or the right
    // button) targets the background instead -- the app convention the colour picker already uses.
    // No undo step: a swatch change is app state, not a document command.
    if (m_eyedropperHost.commit)
        m_eyedropperHost.commit(eventDocPoint(), eyedropperToBackground());
}

void VulkanCanvas::dragEyedropper() {
    // Photoshop convention: the eyedropper keeps sampling as you drag, so the foreground tracks the
    // pixel under the cursor live. The held button/modifier decides fg vs bg for the whole drag.
    if (m_eyedropperHost.commit)
        m_eyedropperHost.commit(eventDocPoint(), eyedropperToBackground());
}

void VulkanCanvas::syncLoupe(bool inside) {
    if (!m_renderer)
        return;
    const bool show = (eyedropperToolActive() || temporaryEyedropperActive()) && inside &&
                      !m_panning && !m_spaceDown && !m_rotateDown;
    if (!show) {
        if (m_loupeVisible) {
            m_renderer->setLoupe(false, {}, 0.0, 0.0, {}, {}, {}, false);
            m_loupeVisible = false;
        }
        m_loupeReadout.reset();
        return;
    }
    const common::Vec2 doc = m_view.toDoc(m_cursorLogical);
    // The live readout colour: what the host would pick right here (honouring Source + sample size).
    // Off the pixels it is empty, and the loupe drops its readout (the ring's top arc goes neutral),
    // showing only the magnified content + grid + the centre cell.
    m_loupeReadout =
        m_eyedropperHost.sample ? m_eyedropperHost.sample(doc) : std::optional<common::Color8>{};
    // The loupe's centre cell is the sampled texel; pass its CENTRE so the shader centres the cell
    // exactly on the disk (loupeSampleDoc = floor(doc) + 0.5).
    const common::Vec2 sampleDoc{std::floor(doc.x) + 0.5, std::floor(doc.y) + 0.5};
    const common::Color8 swatch = m_loupeReadout.value_or(common::Color8{});
    // The comparison ring's bottom arc: the swatch a pick right now would replace. Follows the
    // live fg/bg routing (Alt or the right button target the background), so what is compared is
    // exactly what would change.
    const common::Color8 prev = m_eyedropperHost.previous
                                    ? m_eyedropperHost.previous(eyedropperToBackground())
                                    : common::Color8{};
    m_renderer->setLoupe(true, m_cursorLogical, kLoupeRadiusLogical, kLoupeMagLogical, sampleDoc,
                         swatch, prev, /*readout=*/m_loupeReadout.has_value());
    m_loupeVisible = true;
}

// ---- S18 select brush (paint-to-select) -----------------------------------------------------
//
// The select brush is a coverage-painting DRAG, so it rides the SelectionGesture commit shape
// (docs/research-select-brush.md §3.2): the live combined mask goes straight to the canvas
// (frame-coalesced), and the host lands ONE SetSelectionCommand on release. What it paints is a
// core::brush::MaskStroke -- no image analysis, just the tip's coverage into the selection mask.

bool VulkanCanvas::selectBrushToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::SelectBrush;
}

core::SelectOp VulkanCanvas::selectBrushOp() const {
    // §9-B: no-modifier uses the setting's op (Add by default); Alt subtracts. The other marquee
    // modifiers keep out of it -- the select brush's op is the setting + Alt, nothing else.
    if ((Fl::event_state() & FL_ALT) != 0)
        return core::SelectOp::Subtract;
    return m_selectBrushAddByDefault ? core::SelectOp::Add : core::SelectOp::Subtract;
}

void VulkanCanvas::pushSelectBrush() {
    const common::Vec2 size = m_view.documentSize();
    if (size.x < 1.0 || size.y < 1.0)
        return; // no document to select into
    endNudgeSession(); // a fresh pointer gesture ends any arrow-key nudge burst
    m_selectBrushOp = selectBrushOp();
    core::brush::MaskStrokeParams p;
    p.diameter = std::max(0.1, brushOption("size", 24.0));
    p.hardness = std::clamp(brushOption("hardness", 80.0) / 100.0, 0.0, 1.0);
    p.flow = std::clamp(brushOption("flow", 100.0) / 100.0, 0.0, 1.0);
    p.opacity = std::clamp(brushOption("opacity", 100.0) / 100.0, 0.0, 1.0);
    m_maskStroke.begin(static_cast<std::uint32_t>(size.x), static_cast<std::uint32_t>(size.y), p,
                       eventDocPoint());
    m_maskStrokeActive = true;
    m_maskStrokePreviewDirty = true;
    requestHostFrame();
}

void VulkanCanvas::dragSelectBrush() {
    if (!m_maskStrokeActive)
        return;
    m_maskStroke.extendTo(eventDocPoint());
    m_maskStrokePreviewDirty = true; // renderFrame rebuilds the combined preview once this frame
    requestHostFrame();
}

void VulkanCanvas::finishSelectBrush() {
    if (!m_maskStrokeActive)
        return;
    m_maskStroke.end();
    m_maskStrokeActive = false;
    const core::Selection contribution = m_maskStroke.toSelection();
    if (contribution.isEmpty()) {
        restoreDocumentMask(); // the stroke deposited nothing new (e.g. opacity 0): drop the preview
        return;
    }
    core::Selection combined =
        core::Selection::combine(baseSelection(), contribution, m_selectBrushOp);
    if (!combined.anySelected())
        combined = core::Selection{}; // land "no selection", not an active selection of nothing
    if (combined == baseSelection()) {
        restoreDocumentMask(); // no actual change (a subtract that removed nothing): no undo step
        return;
    }
    commitSelection(std::move(combined), 0, "Select Brush"); // the host pushes the one command
}

// ---- L1 edge-aware select brush (grow-to-edges) ----------------------------------------------
//
// The same seed-stroke shape as the S18 select brush, with ONE added step on release: the painted
// trail becomes geometric seeds for the host's edge-stopped geodesic grow (core::edgeGrowSelection),
// and the GROWN mask is what combines and commits. ⚠ INVARIANT: the grow runs on mouse-up ONLY.
// While the unbroken stroke is in progress the canvas computes and shows nothing beyond the raw
// painted trail itself -- the sample, never a grown/"identified" region -- plus the ordinary size
// ring. Do not add a live grow preview to the drag path: solve-on-release is a hard constraint on
// this tool, deliberately chosen and deliberately paid for, not a performance compromise.

bool VulkanCanvas::edgeBrushToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::EdgeBrush;
}

void VulkanCanvas::pushEdgeBrush() {
    const common::Vec2 size = m_view.documentSize();
    if (size.x < 1.0 || size.y < 1.0)
        return; // no document to select into
    endNudgeSession(); // a fresh pointer gesture ends any arrow-key nudge burst
    m_edgeBrushOp = selectBrushOp(); // the select-brush family op: the setting + Alt (§9-B)
    core::brush::MaskStrokeParams p;
    p.diameter = std::max(0.1, brushOption("size", 24.0));
    // A seed stroke wants a solid core (the grow seeds off the >=128 set): hard tip, full flow.
    p.hardness = 1.0;
    p.flow = 1.0;
    p.opacity = 1.0;
    m_edgeStroke.begin(static_cast<std::uint32_t>(size.x), static_cast<std::uint32_t>(size.y), p,
                       eventDocPoint());
    m_edgeStrokeActive = true;
    m_edgeStrokePreviewDirty = true;
    requestHostFrame();
}

void VulkanCanvas::dragEdgeBrush() {
    if (!m_edgeStrokeActive)
        return;
    m_edgeStroke.extendTo(eventDocPoint());
    m_edgeStrokePreviewDirty = true; // renderFrame re-shows the RAW trail only: still no grow
    requestHostFrame();
}

void VulkanCanvas::finishEdgeBrush() {
    if (!m_edgeStrokeActive)
        return;
    m_edgeStroke.end();
    m_edgeStrokeActive = false;
    m_edgeStrokePreviewDirty = false;
    const core::Selection seeds = m_edgeStroke.toSelection();
    if (seeds.isEmpty() || !m_edgeBrushHost.grow) {
        restoreDocumentMask(); // the stroke deposited nothing (or no host): drop the trail preview
        return;
    }
    // The one solve, on release only: the host resolves the source image + Reach/Edge Stop and
    // runs the geodesic grow. Empty = no source (the host already hinted) or nothing grown.
    const core::Selection grown = m_edgeBrushHost.grow(seeds);
    if (grown.isEmpty()) {
        restoreDocumentMask();
        return;
    }
    core::Selection combined = core::Selection::combine(baseSelection(), grown, m_edgeBrushOp);
    if (!combined.anySelected())
        combined = core::Selection{}; // land "no selection", not an active selection of nothing
    if (combined == baseSelection()) {
        restoreDocumentMask(); // no actual change (a subtract that removed nothing): no undo step
        return;
    }
    commitSelection(std::move(combined), 0, "Edge Select Brush"); // the one SetSelectionCommand
}

// ---- S38-b eye retouch (flash red-eye / sclera de-redding) ------------------------------------
//
// One gesture shape for both modes (docs/red-eye-tool.md §4): the stroke paints a coverage SCOPE
// with the select-brush lane's MaskStroke -- coverage only, no image analysis -- and the correction
// runs ONCE, on release, in the host. ⚠ INVARIANT, and it lives in that ordering as much as in
// the math: while the stroke is in flight the canvas shows the raw painted trail and the size ring
// and nothing else, so no region of the image is ever "identified" for the user. The tool has no
// detector of any kind and never looks for an eye -- the user says where, always.

bool VulkanCanvas::redEyeToolActive() const {
    return m_tools != nullptr && redEyeModeFor(m_tools->active()).has_value();
}

RedEyeOptions VulkanCanvas::redEyeOptions() const {
    RedEyeOptions o;
    if (m_tools == nullptr)
        return o;
    o.mode = redEyeModeFor(m_tools->active()).value_or(core::RedEyeMode::Flash);
    o.size = brushOption("size", o.size);
    o.spread = brushOption("spread", o.spread);
    o.strength = brushOption("strength", o.strength);
    o.darken = brushOption("darken", o.darken);
    o.keepCatchlight = brushOption("catchlight", 1.0) != 0.0;
    o.amount = brushOption("amount", o.amount);
    o.vascularity = brushOption("vascularity", o.vascularity);
    o.suppressVeins = brushOption("method", 1.0) != 0.0; // 0 = harmonize, 1 = harmonize + veins
    o.protectCornerWarmth = brushOption("warmth", 1.0) != 0.0;
    return o;
}

void VulkanCanvas::pushRedEye() {
    const common::Vec2 size = m_view.documentSize();
    if (size.x < 1.0 || size.y < 1.0)
        return; // no document to retouch
    endNudgeSession(); // a fresh pointer gesture ends any arrow-key nudge burst
    m_redEyeStroke.begin(static_cast<std::uint32_t>(size.x), static_cast<std::uint32_t>(size.y),
                         redEyeStrokeParams(redEyeOptions()), eventDocPoint());
    m_redEyeStrokeActive = true;
    m_redEyeStrokePreviewDirty = true;
    requestHostFrame();
}

void VulkanCanvas::dragRedEye() {
    if (!m_redEyeStrokeActive)
        return;
    m_redEyeStroke.extendTo(eventDocPoint());
    m_redEyeStrokePreviewDirty = true; // renderFrame re-shows the RAW trail once this frame
    requestHostFrame();
}

void VulkanCanvas::finishRedEye() {
    if (!m_redEyeStrokeActive)
        return;
    m_redEyeStroke.end();
    m_redEyeStrokeActive = false;
    m_redEyeStrokePreviewDirty = false;
    core::Selection scope = m_redEyeStroke.toSelection();
    // The trail preview rode the selection-mask channel; give the document's own ants back before
    // the correction lands (the host recomposites, and a stale trail would linger over the result).
    restoreDocumentMask();
    if (scope.isEmpty() || !m_redEyeHost.apply)
        return; // the gesture deposited nothing (or nobody is listening): no undo step
    const RedEyeOptions options = redEyeOptions();
    m_redEyeHost.apply(options.mode, std::move(scope), options);
}

std::vector<core::Layer*> VulkanCanvas::moveTargetLayers() const {
    std::vector<core::Layer*> out;
    core::Document* doc = m_moveHost.document ? m_moveHost.document() : nullptr;
    if (doc == nullptr)
        return out;
    out.reserve(m_moveTargets.size());
    for (const core::LayerId id : m_moveTargets)
        if (core::Layer* l = doc->find(id))
            out.push_back(l);
    return out;
}

core::Layer* VulkanCanvas::primaryMoveTargetLayer() const {
    if (m_moveTargets.empty() || !m_moveHost.document)
        return nullptr;
    core::Document* doc = m_moveHost.document();
    return doc != nullptr ? doc->find(m_moveTargets.back()) : nullptr;
}

bool VulkanCanvas::isMoveTarget(core::LayerId id) const {
    return std::find(m_moveTargets.begin(), m_moveTargets.end(), id) != m_moveTargets.end();
}

// The four document-space corners of a content rect framed by `base`, TL,TR,BR,BL order.
static std::array<common::Vec2, 4> framedCorners(const common::Affine2D& base,
                                                 const common::Rect& r) {
    return {base.apply(r.topLeft()), base.apply({r.right(), r.y}),
            base.apply({r.right(), r.bottom()}), base.apply({r.x, r.bottom()})};
}

// The Type-edit box's resize handle as a small square that is ALIGNED to the (possibly rotated)
// box rather than the screen axes -- because it scales typographic size, which lives in the box's
// frame, not geometry (user 2026-06-29). `hs` is the handle half-size in screen px; corners in the
// box's TL,TR,BR,BL order, centred on box corner `corner` (BR for horizontal text; BL for vertical
// Point text, so the handle stays connected to the left-edge side baseline -- see textResizeCorner).
static std::array<common::Vec2, 4> textHandleQuad(const std::array<common::Vec2, 4>& c, double hs,
                                                  int corner = 2) {
    const std::size_t k = static_cast<std::size_t>(corner & 3);
    const common::Vec2 at = c[k];
    common::Vec2 dx = at - c[(k + 3) % 4]; // outward along one adjacent edge
    common::Vec2 dy = at - c[(k + 1) % 4]; // outward along the other
    const double lx = dx.length(), ly = dy.length();
    dx = lx > 1e-6 ? dx * (1.0 / lx) : common::Vec2{1.0, 0.0};
    dy = ly > 1e-6 ? dy * (1.0 / ly) : common::Vec2{0.0, 1.0};
    std::array<common::Vec2, 4> q{at - dx * hs - dy * hs, at + dx * hs - dy * hs,
                                  at + dx * hs + dy * hs, at - dx * hs + dy * hs};
    // The shader's quadInside needs one consistent winding; the outward edge pair flips handedness
    // at the odd corners (TR/BL), so re-order there or the solid handle fill vanishes.
    if (dx.x * dy.y - dx.y * dy.x < 0.0)
        std::swap(q[1], q[3]);
    return q;
}
constexpr double kTextHandleHalfPx = 4.0; // resize-handle half-size, screen px
constexpr double kTextBendDrop = 13.0;    // bend pill-centre drop below the bar apex, screen px
constexpr double kTextBendHitPx = 11.0;   // bend pill grab radius (the pill is wider than a corner handle)
// The bend DROP-TAB handle itself (rounded pill + up/down glyph on a stem) is drawn by the present
// pass (WindowRenderer::setTextBendHandle -> canvas_present.comp), not as solid quads, so it can be
// two-tone; the canvas only supplies its pill centre + the bar apex (textBendHandle / syncTextOverlay).

// What the MOVE box frames for one layer: contentBounds, except an extruded text block frames the
// rendered SOLID's extent -- the pixel cache mapped back to layer units -- so the box wraps what
// is actually on screen (round 3: "the Move box isn't faithful to the 3D text"). contentBounds
// itself stays the flat frame on purpose: it is what the TYPE box gizmo places (§10.3).
static std::optional<common::Rect> moveFrameBounds(const core::Layer& l) {
    if (const auto* tl = l.as<core::TextLayer>())
        if (tl->block().extrude && tl->cachedImage() != nullptr) {
            const common::Image* img = tl->cachedImage();
            const common::Rect px{0.0, 0.0, static_cast<double>(img->width),
                                  static_cast<double>(img->height)};
            return tl->cacheImageToLayer().mapBounds(px);
        }
    return l.contentBounds();
}

bool VulkanCanvas::moveSelectionBox(common::Affine2D& base, common::Rect& content) const {
    const std::vector<core::Layer*> layers = moveTargetLayers();
    if (layers.empty())
        return false;
    if (layers.size() == 1) {
        // One layer: frame its CONTENT (tight alpha bbox) in the layer's own (possibly rotated)
        // frame — layers are document-sized images, so the extent would be the whole canvas.
        const std::optional<common::Rect> c = moveFrameBounds(*layers.front());
        if (!c || c->empty())
            return false;
        base = core::worldTransform(*layers.front());
        content = *c;
        return true;
    }
    // Several layers: the axis-aligned union of their content rects in document space, framed by
    // identity (so a multi-selection box is canvas-aligned, the Photoshop/Illustrator convention).
    bool any = false;
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    for (const core::Layer* l : layers) {
        const std::optional<common::Rect> c = moveFrameBounds(*l);
        if (!c || c->empty())
            continue;
        const std::array<common::Vec2, 4> corners = framedCorners(core::worldTransform(*l), *c);
        for (const common::Vec2& p : corners) {
            if (!any) {
                minX = maxX = p.x;
                minY = maxY = p.y;
                any = true;
            } else {
                minX = std::min(minX, p.x);
                minY = std::min(minY, p.y);
                maxX = std::max(maxX, p.x);
                maxY = std::max(maxY, p.y);
            }
        }
    }
    if (!any)
        return false;
    base = common::Affine2D{}; // identity
    content = common::Rect{minX, minY, maxX - minX, maxY - minY};
    return !content.empty();
}

bool VulkanCanvas::moveTargetCorners(std::array<common::Vec2, 4>& out) const {
    std::array<common::Vec2, 4> doc{};
    if (m_transform.active()) {
        // Mid-gesture: the box follows the gesture rigidly (m_gestureResult maps the captured
        // content frame to its current world position) — for one layer this matches the live
        // layer box, for several it keeps the box stable instead of chasing the union bounds.
        doc = framedCorners(m_gestureResult, m_gestureContent);
    } else {
        common::Affine2D base{};
        common::Rect content{};
        if (!moveSelectionBox(base, content))
            return false;
        doc = framedCorners(base, content);
    }
    for (std::size_t i = 0; i < 4; ++i)
        out[i] = m_view.toScreen(doc[i]);
    return true;
}

bool VulkanCanvas::moveAnchorScreen(common::Vec2& out) const {
    common::Vec2 docPt{};
    if (m_transform.active()) {
        // Mid-gesture: the anchor rides the box exactly like moveTargetCorners -- the captured
        // content frame mapped by the live gesture result (for rotate/scale the pivot is fixed, so
        // it stays put; for a move it travels with the box).
        const common::Vec2 local = m_transformPivotLocal.value_or(m_gestureContent.center());
        docPt = m_gestureResult.apply(local);
    } else {
        common::Affine2D base{};
        common::Rect content{};
        if (!moveSelectionBox(base, content))
            return false;
        const common::Vec2 local = m_transformPivotLocal.value_or(content.center());
        docPt = base.apply(local);
    }
    out = m_view.toScreen(docPt);
    return true;
}

void VulkanCanvas::dragMoveAnchor() {
    if (!m_anchorDragging)
        return;
    common::Affine2D base{};
    common::Rect content{};
    if (!moveSelectionBox(base, content)) { // the box vanished under us: abandon quietly
        m_anchorDragging = false;
        return;
    }
    const std::optional<common::Affine2D> inv = base.inverse();
    if (!inv)
        return;
    common::Vec2 local = inv->apply(eventDocPoint());
    // Light snapping (Photoshop's reference-point grid): to the 8 handle points + the centre, within
    // kAnchorSnapPx measured on SCREEN so the pull is zoom-independent. Snapping to the centre stores
    // it as an EXPLICIT value, so scaling then pivots there too (unlike the untouched auto default,
    // which keeps the opposite-handle scale) -- the anchor mapping order is TL,TR,BR,BL then T,R,B,L.
    const common::Vec2 targets[9] = {
        content.center(),
        {content.x, content.y},
        {content.right(), content.y},
        {content.right(), content.bottom()},
        {content.x, content.bottom()},
        {content.x + content.w * 0.5, content.y},
        {content.right(), content.y + content.h * 0.5},
        {content.x + content.w * 0.5, content.bottom()},
        {content.x, content.y + content.h * 0.5},
    };
    const common::Vec2 cursorScreen = m_view.toScreen(base.apply(local));
    double best = kAnchorSnapPx;
    int bestI = -1;
    for (int i = 0; i < 9; ++i) {
        const double d = (m_view.toScreen(base.apply(targets[i])) - cursorScreen).length();
        if (d <= best) {
            best = d;
            bestI = i;
        }
    }
    if (bestI >= 0)
        local = targets[bestI];
    m_transformPivotLocal = local;
    requestHostFrame();
}

// ---- Document guides (View -> Guides) -------------------------------------------------------

namespace {
constexpr double kGuideHitPx = 6.0; // grab tolerance for an existing guide, logical screen px
constexpr double kSnapPx = 8.0;     // snap pull radius (View -> Snap), logical screen px
// Photoshop/Illustrator guide cyan -- distinct from the box-blue selection/transform chrome.
constexpr float kGuideColorR = 0.05f, kGuideColorG = 0.62f, kGuideColorB = 0.95f;
// Smart-guide magenta (Figma/Illustrator) -- the dynamic alignment lines shown during a drag.
constexpr float kSmartColorR = 0.98f, kSmartColorG = 0.24f, kSmartColorB = 0.55f;
// The Gradient tool's shape-outline ring (S22): the chrome box-blue the handles use, so the ring
// reads as part of the gizmo rather than as a document guide.
constexpr float kGradientRingR = 0.184f, kGradientRingG = 0.502f, kGradientRingB = 0.929f;
// ... and the marker that says "the next TWO guide-lane entries are an analytic ELLIPSE, not two
// straight segments". The lane carries no kind tag of its own -- WindowRenderer hard-writes the
// trailing std430 float as zero -- so the flag rides an IMPOSSIBLE colour: a negative red. Same
// out-of-range-sentinel idiom the present pass already uses for the Move anchor (anchorPos.x
// > -1e8) and the Type bend tab. Must match canvas_present.comp's guideOverlay().
constexpr float kGradientRingMark = -1.0f;
// ⚠ The Pen tool's node/handle chrome USED to ride this lane too, as a bag of short coloured
// segments capped at 40 of the 64 entries. It has its own channel now (binding 6, syncPenChrome):
// the guide lane draws a flat coloured line with no casing and no knob idiom, which is exactly why
// the pen chrome read as weak -- and a path over ~17 nodes silently lost its marks while starving
// the document's own guides at the same time. Nothing pen-shaped belongs here again.

double pointSegDist(common::Vec2 p, common::Vec2 a, common::Vec2 b) {
    const common::Vec2 ab = b - a;
    const double denom = ab.dot(ab);
    double t = denom > 1e-9 ? (p - a).dot(ab) / denom : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const common::Vec2 proj = a + ab * t;
    return (p - proj).length();
}

// The screen-space segment (two endpoints, logical px) for a guide under `view`, extended far past
// the viewport so it reads as an infinite line. `docSpan` is the document extent the guide spans
// (width for a horizontal guide, height for a vertical one).
std::array<common::Vec2, 2> guideScreenSegment(const CanvasView& view, bool horizontal, double pos,
                                               double docSpan) {
    common::Vec2 a = horizontal ? view.toScreen({0.0, pos}) : view.toScreen({pos, 0.0});
    common::Vec2 b = horizontal ? view.toScreen({docSpan, pos}) : view.toScreen({pos, docSpan});
    common::Vec2 dir = b - a;
    const double len = dir.length();
    if (len < 1e-6)
        dir = horizontal ? common::Vec2{1.0, 0.0} : common::Vec2{0.0, 1.0};
    else
        dir = dir * (1.0 / len);
    constexpr double kBig = 10000.0; // long enough to cross any viewport -> reads as infinite
    return {a - dir * kBig, b + dir * kBig};
}
} // namespace

bool VulkanCanvas::guidesInteractive() const {
    core::Document* doc = m_moveHost.document ? m_moveHost.document() : nullptr;
    return doc != nullptr && doc->showGuides() && !doc->lockGuides();
}

const core::Guide* VulkanCanvas::hitGuide(common::Vec2 screenPt) const {
    core::Document* doc = m_moveHost.document ? m_moveHost.document() : nullptr;
    if (doc == nullptr || !doc->showGuides() || doc->lockGuides())
        return nullptr;
    const common::Vec2 docSize = m_view.documentSize();
    const core::Guide* best = nullptr;
    double bestD = kGuideHitPx;
    for (const core::Guide& g : doc->guides()) {
        const auto seg =
            guideScreenSegment(m_view, g.horizontal(), g.position,
                               g.horizontal() ? docSize.x : docSize.y);
        const double d = pointSegDist(screenPt, seg[0], seg[1]);
        if (d <= bestD) {
            bestD = d;
            best = &g;
        }
    }
    return best;
}

void VulkanCanvas::beginGuideCreate(bool horizontalGuide, double docPos) {
    if (!guidesInteractive())
        return; // guides hidden/locked: the preview would be invisible + the commit disallowed
    m_guideCreate = {true, horizontalGuide, docPos};
    requestHostFrame();
}

void VulkanCanvas::updateGuideCreate(double docPos) {
    if (!m_guideCreate.active)
        return;
    m_guideCreate.pos = docPos;
    requestHostFrame();
}

void VulkanCanvas::commitGuideCreate() {
    if (!m_guideCreate.active)
        return;
    const GuideCreate gc = m_guideCreate;
    m_guideCreate.active = false;
    core::Document* doc = m_moveHost.document ? m_moveHost.document() : nullptr;
    if (doc != nullptr) {
        core::Guide g;
        g.orientation = gc.horizontal ? core::Guide::Orientation::Horizontal
                                      : core::Guide::Orientation::Vertical;
        g.position = gc.pos;
        g.id = doc->mintGuideId();
        doc->commands().push(std::make_unique<core::AddGuideCommand>(g));
    }
    requestHostFrame();
}

void VulkanCanvas::cancelGuideCreate() {
    m_guideCreate.active = false;
    requestHostFrame();
}

bool VulkanCanvas::pushGuideGesture() {
    const core::Guide* g = hitGuide(m_cursorLogical);
    if (g == nullptr)
        return false;
    m_guideDrag = {true, g->id, g->horizontal(), g->position, g->position};
    requestHostFrame();
    return true;
}

void VulkanCanvas::dragGuideMove() {
    if (!m_guideDrag.active)
        return;
    const common::Vec2 doc = eventDocPoint();
    m_guideDrag.currentPos = m_guideDrag.horizontal ? doc.y : doc.x;
    requestHostFrame();
}

void VulkanCanvas::finishGuideDrag() {
    if (!m_guideDrag.active)
        return;
    const GuideDrag gd = m_guideDrag;
    m_guideDrag.active = false;
    core::Document* doc = m_moveHost.document ? m_moveHost.document() : nullptr;
    if (doc != nullptr) {
        // Dragged onto the ruler/gutter (a negative canvas-local coordinate along the guide's axis)
        // deletes the guide -- the Photoshop convention. The document was never mutated during the
        // drag, so the removed guide's captured position is its original one.
        const bool offCanvas = gd.horizontal ? m_cursorLogical.y < 0.0 : m_cursorLogical.x < 0.0;
        if (offCanvas)
            doc->commands().push(std::make_unique<core::RemoveGuideCommand>(gd.id));
        else if (gd.currentPos != gd.startPos)
            doc->commands().push(std::make_unique<core::MoveGuideCommand>(
                gd.id, gd.startPos, gd.currentPos, m_guideCoalesce));
    }
    ++m_guideCoalesce;
    requestHostFrame();
}

void VulkanCanvas::syncGuidesOverlay() {
    if (!m_renderer)
        return;
    std::vector<render::WindowRenderer::GuideLine> lines;
    // The Gradient tool's shape outline (S22): the circle a Radial/Conic covers, the ellipse an
    // Elliptical does. It rides the guide lane as an ANALYTIC ELLIPSE -- two entries carrying the
    // centre and the two basis vectors -- not as chords. A chorded ring needed ~pi*sqrt(2R)
    // segments to keep its sag under a quarter pixel, i.e. more than the WHOLE 64-entry lane above
    // a ~200 px on-screen radius, so it both read as a visible polygon AND left the document's own
    // guides nothing to draw with. Two slots cost nothing and are exactly smooth at any zoom.
    // Entry 1: a = centre, b = the major-axis tip, colour = the marker. Entry 2: a = the minor-axis
    // tip (b unused), colour = the ring's real ink. Still pushed FIRST so the pair can never be
    // split by the lane's tail clamp. Maths: ui::gradientRingDistance (shader: gradientRing()).
    GradientHandles gh;
    if (gradientToolActive() && currentGradientHandles(gh) && gradientHasRing(gh)) {
        const common::Vec2 c = m_view.toScreen(gh.start);
        lines.push_back({c, m_view.toScreen(gh.end), kGradientRingMark, 0.0f, 0.0f});
        lines.push_back(
            {m_view.toScreen(gh.minor), c, kGradientRingR, kGradientRingG, kGradientRingB});
    }
    core::Document* doc = m_moveHost.document ? m_moveHost.document() : nullptr;
    const common::Vec2 docSize = m_view.documentSize();
    if (doc != nullptr && doc->showGuides()) {
        for (const core::Guide& g : doc->guides()) {
            const bool horiz = g.horizontal();
            // The dragged guide previews at its live position; the rest at their stored positions.
            const double pos = (m_guideDrag.active && m_guideDrag.id == g.id) ? m_guideDrag.currentPos
                                                                              : g.position;
            const auto seg =
                guideScreenSegment(m_view, horiz, pos, horiz ? docSize.x : docSize.y);
            lines.push_back({seg[0], seg[1], kGuideColorR, kGuideColorG, kGuideColorB});
        }
    }
    // The guide being pulled off a ruler (preview until commit).
    if (m_guideCreate.active) {
        const auto seg = guideScreenSegment(m_view, m_guideCreate.horizontal, m_guideCreate.pos,
                                            m_guideCreate.horizontal ? docSize.x : docSize.y);
        lines.push_back({seg[0], seg[1], kGuideColorR, kGuideColorG, kGuideColorB});
    }
    // Smart-guide alignment lines (magenta) while a Move drag snaps to other layers / the canvas.
    if (m_smartGuides && m_transform.active())
        for (const SmartGuideLine& sg : m_smartGuideLines) {
            const auto seg = guideScreenSegment(m_view, sg.horizontal, sg.pos,
                                                sg.horizontal ? docSize.x : docSize.y);
            lines.push_back({seg[0], seg[1], kSmartColorR, kSmartColorG, kSmartColorB});
        }
    m_renderer->setGuideLines(lines);
}

// ---- Snapping (View -> Snap) ----------------------------------------------------------------

core::SnapCandidates VulkanCanvas::gatherSnapCandidates(bool guides, bool canvas, bool layers) const {
    core::SnapCandidates cand;
    core::Document* doc = m_moveHost.document ? m_moveHost.document() : nullptr;
    if (doc == nullptr)
        return cand;
    if (canvas) {
        const double w = static_cast<double>(doc->width());
        const double h = static_cast<double>(doc->height());
        cand.vertical.insert(cand.vertical.end(), {0.0, w * 0.5, w});    // edges + centre
        cand.horizontal.insert(cand.horizontal.end(), {0.0, h * 0.5, h});
    }
    if (guides)
        for (const core::Guide& g : doc->guides()) {
            if (g.horizontal())
                cand.horizontal.push_back(g.position);
            else
                cand.vertical.push_back(g.position);
        }
    if (layers)
        collectLayerSnapLines(doc->root(), cand);
    return cand;
}

void VulkanCanvas::collectLayerSnapLines(const core::GroupLayer& group,
                                         core::SnapCandidates& cand) const {
    for (const auto& childPtr : group.children()) {
        const core::Layer& l = *childPtr;
        if (!l.visible() || isMoveTarget(l.id()))
            continue; // a moving layer (and its whole subtree) is not a snap target
        if (const auto* g = l.as<core::GroupLayer>()) {
            collectLayerSnapLines(*g, cand);
            continue;
        }
        const std::optional<common::Rect> cb = l.contentBounds();
        if (!cb || cb->empty())
            continue;
        const common::Rect box = core::worldTransform(l).mapBounds(*cb);
        cand.vertical.insert(cand.vertical.end(), {box.x, box.center().x, box.right()});
        cand.horizontal.insert(cand.horizontal.end(), {box.y, box.center().y, box.bottom()});
    }
}

void VulkanCanvas::applyMoveSnap(common::Affine2D& boxWorld) {
    m_smartGuideLines.clear();
    if (m_transform.mode() != TransformMode::Move || (!m_snapEnabled && !m_smartGuides))
        return;
    // Candidate sources: the snap toggles when Snap is on; Smart Guides always adds the canvas +
    // other layers (its alignment feedback works even with the master Snap toggle off).
    const bool wantGuides = m_snapEnabled && m_snapToGuides;
    const bool wantCanvas = (m_snapEnabled && m_snapToCanvas) || m_smartGuides;
    const bool wantLayers = (m_snapEnabled && m_snapToLayers) || m_smartGuides;
    const core::SnapCandidates cand = gatherSnapCandidates(wantGuides, wantCanvas, wantLayers);
    if (cand.vertical.empty() && cand.horizontal.empty())
        return;
    // The moving box's document-space AABB after the drag.
    const std::array<common::Vec2, 4> corners = framedCorners(boxWorld, m_gestureContent);
    double minX = corners[0].x, maxX = corners[0].x, minY = corners[0].y, maxY = corners[0].y;
    for (const common::Vec2& c : corners) {
        minX = std::min(minX, c.x);
        maxX = std::max(maxX, c.x);
        minY = std::min(minY, c.y);
        maxY = std::max(maxY, c.y);
    }
    const common::Rect box{minX, minY, maxX - minX, maxY - minY};
    const double threshold = kSnapPx / std::max(m_view.zoom(), CanvasView::kMinZoom);
    const core::SnapResult snap = core::snapBox(box, cand, threshold);
    if (snap.snappedX || snap.snappedY)
        boxWorld = common::Affine2D::translation(snap.snappedX ? snap.dx : 0.0,
                                                 snap.snappedY ? snap.dy : 0.0) *
                   boxWorld;
    // Smart guides: draw the matched alignment line(s) as magenta lines this frame.
    if (m_smartGuides) {
        if (snap.snappedX)
            m_smartGuideLines.push_back({/*horizontal=*/false, snap.lineX}); // vertical line at x
        if (snap.snappedY)
            m_smartGuideLines.push_back({/*horizontal=*/true, snap.lineY}); // horizontal line at y
    }
}

void VulkanCanvas::notifyMoveSelection() {
    // The reference point belongs to the box that was on screen; a new move selection reframes it, so
    // the anchor drops back to the auto default (centre) rather than pointing into stale geometry.
    m_transformPivotLocal.reset();
    if (m_moveHost.selectionChanged)
        m_moveHost.selectionChanged(m_moveTargets);
}

void VulkanCanvas::setSingleMoveTarget(core::LayerId id) {
    m_moveTargets.clear();
    if (id != core::kInvalidLayerId)
        m_moveTargets.push_back(id);
    if (m_moveHost.selectLayer && id != core::kInvalidLayerId)
        m_moveHost.selectLayer(id);
    notifyMoveSelection();
}

void VulkanCanvas::addMoveTarget(core::LayerId id) {
    if (id == core::kInvalidLayerId || isMoveTarget(id))
        return;
    m_moveTargets.push_back(id);
    if (m_moveHost.selectLayer)
        m_moveHost.selectLayer(id); // the panel's active layer follows the latest addition
    notifyMoveSelection();
}

void VulkanCanvas::toggleMoveTarget(core::LayerId id) {
    if (id == core::kInvalidLayerId)
        return;
    const auto it = std::find(m_moveTargets.begin(), m_moveTargets.end(), id);
    if (it != m_moveTargets.end()) {
        m_moveTargets.erase(it); // shift-click an already-selected layer removes it
        if (m_moveHost.selectLayer && !m_moveTargets.empty())
            m_moveHost.selectLayer(m_moveTargets.back());
    } else {
        m_moveTargets.push_back(id);
        if (m_moveHost.selectLayer)
            m_moveHost.selectLayer(id);
    }
    notifyMoveSelection();
}

void VulkanCanvas::setMoveTargets(std::vector<core::LayerId> ids) {
    m_moveTargets = std::move(ids);
    // The panel's active row follows the set's primary (its last member), exactly as it follows the
    // latest addition of a shift-click; an empty set leaves the panel with no active row at all.
    if (m_moveHost.selectLayer)
        m_moveHost.selectLayer(m_moveTargets.empty() ? core::kInvalidLayerId
                                                     : m_moveTargets.back());
    notifyMoveSelection();
}

std::vector<core::LayerId> VulkanCanvas::arrangeTargets() const {
    if (!m_moveTargets.empty())
        return m_moveTargets; // rung 1: today's behaviour, untouched
    // Rung 2. Exactly one tool is active at a time, so at most one of these binds anything; each is
    // gated on its own tool anyway, because a binding outliving its tool is a real state to guard
    // against (they are dropped by cancelShapeEdit / cancelPenEdit / cancelGradientEdit /
    // commitTextEdit on the switch, and a missed one would let Arrange move a layer the user
    // stopped editing).
    std::vector<core::LayerId> out;
    const auto bind = [&out](core::LayerId id) {
        if (id != core::kInvalidLayerId)
            out.push_back(id);
    };
    if (shapeToolActive())
        bind(m_shapeEditTarget); // S26-b select-to-edit; one vec::Object per VectorLayer, so a
                                 // selected SHAPE already IS a selected layer
    if (penToolActive())
        bind(m_penEditTarget); // S28: the committed path bound for node editing
    if (gradientToolActive())
        bind(m_gradientEditTarget); // S22: a gradient layer bound for handle editing
    if (typeToolActive())
        bind(m_textEditTarget); // S29-b: the text layer being edited
    return out;
}

void VulkanCanvas::clearMoveSelection() {
    m_moveTargets.clear();
    if (m_moveHost.selectLayer)
        m_moveHost.selectLayer(core::kInvalidLayerId); // ... and the panel's single active row
    notifyMoveSelection();
}

bool VulkanCanvas::beginMoveGesture(TransformMode mode, int handle, common::Vec2 docPt) {
    // A locked layer refuses every transform. One guard covers move / rotate / scale because every
    // Move-tool gesture funnels through here. ANY locked layer in a multi-selection vetoes the whole
    // gesture (Photoshop's rule): transforming the rest would silently break the set apart.
    for (const core::Layer* layer : moveTargetLayers()) {
        if (layer->locked()) {
            if (m_moveHost.lockedAttempt)
                m_moveHost.lockedAttempt(); // the host explains the refusal in the status bar
            return false;
        }
    }
    common::Affine2D base{};
    common::Rect content{};
    if (!moveSelectionBox(base, content))
        return false;
    const std::optional<common::Affine2D> baseInv = base.inverse();
    if (!baseInv)
        return false; // a singular framing transform has no usable space (degenerate layer)
    // The user's anchor (reference point) governs the pivot for rotate + scale; nullopt = auto
    // default (content centre for rotate, opposite handle for scale). It lives in the SAME content
    // frame beginMoveGesture captured, so it stays valid for the gesture's life.
    if (!m_transform.begin(mode, handle, docPt, base, content, m_transformPivotLocal))
        return false;
    m_gestureBaseInv = *baseInv;
    m_gestureContent = content;
    m_gestureResult = base; // no motion yet: the box rests at its captured frame
    m_gestureBaseWorld.clear();
    for (const core::Layer* l : moveTargetLayers())
        m_gestureBaseWorld.emplace_back(l->id(), core::worldTransform(*l));
    return true;
}

void VulkanCanvas::pushMoveTool() {
    core::Document* doc = m_moveHost.document ? m_moveHost.document() : nullptr;
    if (doc == nullptr)
        return;
    const common::Vec2 screenPt = eventLogicalPoint();
    const common::Vec2 docPt = m_view.toDoc(screenPt);
    const bool shift = (Fl::event_state() & FL_SHIFT) != 0;
    m_moveClickAction = MoveClickAction::None;
    m_moveClickLayer = core::kInvalidLayerId;
    m_moveDragLatched = false;
    m_movePressScreen = screenPt;
    m_movePressDoc = docPt;

    // The transform ANCHOR / reference point (S15+) claims the press ahead of the handles/body/text:
    // it is the pivot rotation + scaling turn around, and dragging it repositions the pivot. A
    // double-click on it snaps the pivot back to the box centre (mirrors the rotate-band reset).
    common::Vec2 anchorScreen{};
    if (moveToolActive() && moveAnchorScreen(anchorScreen) &&
        (screenPt - anchorScreen).length() <= kAnchorHitPx) {
        if (Fl::event_clicks() > 0) {
            m_transformPivotLocal.reset(); // back to the auto default (centre)
            requestHostFrame();
            return;
        }
        m_anchorDragging = true;
        m_anchorDragPrev = m_transformPivotLocal; // remembered so Esc can restore it
        return;                                   // armed: FL_DRAG repositions the pivot
    }

    // Double-click a selected text object: hand off to the Type tool and edit it in place (the Affinity
    // Move<->Type unify, fixlist #8/#11). The first click already selected it (its transform box is up);
    // the second click (event_clicks()>0) enters editing at the click point.
    if (Fl::event_clicks() > 0) {
        if (core::Layer* hit = core::topmostLayerAt(doc->root(), docPt)) {
            if (auto* tl = hit->as<core::TextLayer>(); tl != nullptr && isMoveTarget(tl->id())) {
                beginTextEditFromMove(tl->id(), docPt);
                return;
            }
        }
    }

    // 1. A press on the existing selection box: a handle scales, the corner band rotates, the body
    //    moves — all on the WHOLE selection. A body press is also a potential click (drill /
    //    collapse / shift-toggle), resolved on release if no drag follows.
    std::array<common::Vec2, 4> corners{};
    if (moveTargetCorners(corners)) {
        if (const std::optional<TransformHit> hit =
                hitTransformControls(screenPt, corners, kHandleHitPx, kRotateBandPx)) {
            if (hit->mode == TransformMode::Rotate && Fl::event_clicks() > 0) {
                resetMoveRotation(); // double-click the rotate band -> snap rotation back to 0.00
                                     // deg
                return;
            }
            if (hit->mode != TransformMode::Move) { // scale / rotate the set
                beginMoveGesture(hit->mode, hit->handle, docPt);
                return;
            }
            // Body press: resolve the layer under the cursor for the release-time click action.
            core::Layer* hitLayer = core::topmostLayerAt(doc->root(), docPt);
            const core::LayerId unit = hitLayer != nullptr
                                           ? core::moveClickTarget(hitLayer, nullptr)->id()
                                           : core::kInvalidLayerId;
            if (shift && unit != core::kInvalidLayerId && !isMoveTarget(unit)) {
                // Shift over a layer outside the selection: add it now so the drag moves it too.
                addMoveTarget(unit);
            } else if (shift) {
                m_moveClickAction =
                    MoveClickAction::Toggle; // release toggles `unit` off (or no-op)
                m_moveClickLayer = unit;
            } else if (m_moveTargets.size() > 1) {
                m_moveClickAction =
                    MoveClickAction::Collapse; // release collapses to the clicked one
                m_moveClickLayer = unit;
            } else {
                m_moveClickAction = MoveClickAction::Drill; // release drills into the single target
            }
            beginMoveGesture(TransformMode::Move, -1, docPt);
            return; // armed: FL_DRAG streams transforms
        }
    }

    // 2. A press that missed the box. Resolve the unit under the cursor through the Affinity group
    //    model (the outermost group moves as one); click-away on empty drops the whole selection
    //    and arms the rubber band that gathers layers if the press turns into a drag (S15-f).
    core::Layer* hitLayer = core::topmostLayerAt(doc->root(), docPt);
    if (hitLayer == nullptr) {
        // "Empty" is exactly what the click-pick says it is: topmostLayerAt walks top-of-stack
        // first, skips invisible layers (and invisible groups' whole subtrees), samples
        // raster/magic ALPHA, hit-tests vector geometry and text/texture content boxes, and never
        // lands on a group or adjustment layer. Locked layers still count as hits -- a lock
        // refuses transforms, not selection -- so a press on one selects it, it does not band.
        beginLayerMarquee(docPt, shift || (Fl::event_state() & FL_CTRL) != 0);
        return;
    }
    core::Layer* unit = core::moveClickTarget(hitLayer, nullptr);
    if (unit == nullptr)
        return;
    if (shift) {
        if (isMoveTarget(unit->id())) {
            // Shift over an already-selected unit whose pixels sit outside the box: remove on a
            // click, but allow shift+drag to still move the set (constrained).
            m_moveClickAction = MoveClickAction::Toggle;
            m_moveClickLayer = unit->id();
        } else {
            addMoveTarget(unit->id()); // add now so a follow-on drag moves the expanded set
        }
    } else {
        setSingleMoveTarget(unit->id()); // plain click selects just this unit (select-and-drag)
    }
    beginMoveGesture(TransformMode::Move, -1, docPt);
}

void VulkanCanvas::dragMoveTool() {
    if (!m_transform.active())
        return;
    if (!m_moveDragLatched) {
        // The dead zone: clicks (with their inevitable jitter) must not become micro-drags —
        // they would push a junk undo step and defeat the click-to-drill on release.
        const common::Vec2 p = eventLogicalPoint();
        if ((p - m_movePressScreen).length() < kMoveDragDeadZonePx)
            return;
        m_moveDragLatched = true;
    }
    // Record only: flushMoveDrag lands ONE coalesced transform per frame tick (the S14
    // preview precedent). FLTK can deliver many FL_DRAG events per frame at high pointer
    // rates, and each push used to queue document-sized work.
    const auto state = Fl::event_state();
    m_moveDragDocPt = eventDocPoint();
    m_moveDragShift = (state & FL_SHIFT) != 0;
    m_moveDragAlt = (state & FL_ALT) != 0;
    m_moveDragPending = true;
    requestHostFrame(); // don't let the recorded drag wait out the heartbeat (S15-b)
}

void VulkanCanvas::flushMoveDrag() {
    if (!m_moveDragPending)
        return;
    m_moveDragPending = false;
    if (!m_transform.active())
        return;
    if (moveTargetLayers().empty()) { // undone/removed beneath us: abandon quietly
        m_transform.cancel();
        return;
    }
    m_gestureResult = m_transform.transformFor(m_moveDragDocPt, m_moveDragShift, m_moveDragAlt);
    applyMoveSnap(m_gestureResult); // View -> Snap: pull the box onto guides/canvas/layer bounds
    pushSelectionTransform(m_gestureResult);
    // Keep the cursor live through the drag: the rotate cursor (state 15) reorients with the
    // pointer angle, so it must refresh as the box turns -- without this it froze at the press
    // orientation (user 2026-06-17). Cheap for Move/Scale: the state is unchanged, so it no-ops.
    updateToolCursor(m_pointerInside);
}

void VulkanCanvas::pushSelectionTransform(const common::Affine2D& boxWorld) {
    if (m_gestureBaseWorld.empty() || !m_moveHost.setTransforms)
        return;
    core::Document* doc = m_moveHost.document ? m_moveHost.document() : nullptr;
    if (doc == nullptr)
        return;
    // The box delta maps each layer's PRESS-TIME world transform to its new one (transformFor is
    // absolute from the press, so it must compose with the captured base, never the live state).
    const common::Affine2D delta = boxWorld * m_gestureBaseInv;
    std::vector<std::pair<core::LayerId, common::Affine2D>> entries;
    entries.reserve(m_gestureBaseWorld.size());
    for (const auto& [id, baseWorld] : m_gestureBaseWorld) {
        core::Layer* l = doc->find(id);
        if (l == nullptr)
            continue;
        const std::optional<common::Affine2D> invParent = core::parentWorldTransform(*l).inverse();
        if (!invParent)
            continue; // a singular ancestor shows nothing; nothing sane to edit through it
        entries.emplace_back(id, *invParent * delta * baseWorld);
    }
    if (entries.empty())
        return;
    // Mark the gesture pushed BEFORE the callback: the host's setTransforms handler reads
    // activeDragLayer() (gated on m_moveGesturePushed) to decide the GPU-resident drag / drag-cache
    // fast path, and it must see a valid target on the gesture's very first frame (S60-a).
    m_moveGesturePushed = true;
    m_moveHost.setTransforms(entries, m_transformCoalesce);
}

void VulkanCanvas::endMoveGesture(bool restoreBase) {
    if (!m_transform.active())
        return;
    if (restoreBase) {
        m_moveDragPending = false; // the recorded drag is moot: the bases are coming back
        if (m_moveGesturePushed)   // nothing landed -> nothing to restore (no junk undo step)
            pushSelectionTransform(m_transform.base());
    } else {
        flushMoveDrag(); // the last recorded cursor state must land as the commit
    }
    m_transform.cancel();
    ++m_transformCoalesce; // the next drag is its own undo step
    if (m_moveGesturePushed && m_moveHost.gestureEnded)
        m_moveHost.gestureEnded(); // refresh derived views once, now that the gesture is done
    // A press that ended without a drag was a CLICK: drill into the single target, collapse a
    // multi-selection to the clicked layer, or (Shift) toggle the clicked layer's membership. The
    // box swallows body presses before pushMoveTool's click-select can see them, so the click
    // action is resolved here, on release.
    if (!restoreBase && !m_moveGesturePushed) {
        core::Document* doc = m_moveHost.document ? m_moveHost.document() : nullptr;
        switch (m_moveClickAction) {
        case MoveClickAction::Drill:
            // A hole in the box (the press hit the BODY but no layer pixels) is empty canvas as far
            // as the user can see, so it deselects like a click outside the box would -- the click
            // that misses everything is always "select nothing" (S15-f).
            if (doc != nullptr) {
                core::Layer* hit = core::topmostLayerAt(doc->root(), m_movePressDoc);
                if (hit == nullptr)
                    clearMoveSelection();
                else if (core::Layer* t = core::moveClickTarget(hit, primaryMoveTargetLayer()))
                    setSingleMoveTarget(t->id());
            }
            break;
        case MoveClickAction::Collapse:
            if (m_moveClickLayer != core::kInvalidLayerId)
                setSingleMoveTarget(m_moveClickLayer);
            else
                clearMoveSelection(); // a hole in a multi-selection's box: same rule as Drill
            break;
        case MoveClickAction::Toggle:
            toggleMoveTarget(m_moveClickLayer); // no-op for an invalid (hole) target
            break;
        case MoveClickAction::None:
            break;
        }
    }
    m_moveGesturePushed = false;
    m_moveClickAction = MoveClickAction::None;
    m_gestureBaseWorld.clear();
    m_smartGuideLines.clear(); // the drag's alignment lines vanish with the gesture
}

// Double-clicking the rotate band snaps the selection's rotation back to exactly 0.00 deg (the box
// becomes axis-aligned again, centre + scale preserved): rotate the box by -theta about its own
// centre and commit it as ONE undo step. Then flash the "0.00 deg" HUD briefly so the reset reads.
void VulkanCanvas::resetMoveRotation() {
    common::Affine2D base{};
    common::Rect content{};
    if (!moveSelectionBox(base, content))
        return;
    const std::optional<common::Affine2D> baseInv = base.inverse();
    if (!baseInv)
        return;
    const std::array<common::Vec2, 4> c = framedCorners(base, content);
    const double theta = std::atan2(c[1].y - c[0].y, c[1].x - c[0].x); // the box's current tilt
    if (std::abs(theta) >
        1e-4) { // already axis-aligned -> skip the no-op command, still flash the HUD
        const common::Vec2 centre = base.apply(content.center());
        const common::Affine2D target = common::Affine2D::translation(centre.x, centre.y) *
                                        common::Affine2D::rotation(-theta) *
                                        common::Affine2D::translation(-centre.x, -centre.y) * base;
        m_gestureBaseInv = *baseInv;
        m_gestureContent = content;
        m_gestureResult = target;
        m_gestureBaseWorld.clear();
        for (const core::Layer* l : moveTargetLayers())
            m_gestureBaseWorld.emplace_back(l->id(), core::worldTransform(*l));
        m_moveGesturePushed = false;
        pushSelectionTransform(target);
        ++m_transformCoalesce; // its own undo step
        if (m_moveGesturePushed && m_moveHost.gestureEnded)
            m_moveHost.gestureEnded();
        m_moveGesturePushed = false; // not a drag; the flash HUD is driven by m_resetHudShowing
    }
    // Briefly hold a "0.00 deg" HUD (a one-shot timeout ends it).
    m_resetHudShowing = true;
    Fl::remove_timeout(clearResetHudCb, this);
    Fl::add_timeout(0.8, clearResetHudCb, this);
    syncMoveOverlay();
    requestHostFrame();
}

void VulkanCanvas::clearResetHudCb(void* self) {
    auto* canvas = static_cast<VulkanCanvas*>(self);
    canvas->m_resetHudShowing = false;
    canvas->syncMoveOverlay();
    canvas->requestHostFrame();
}

void VulkanCanvas::clearMoveTarget() {
    endMoveGesture(/*restoreBase=*/true);
    cancelLayerMarquee(); // no in-flight band may straddle a tool switch / document swap
    if (!m_moveTargets.empty()) {
        m_moveTargets.clear();
        notifyMoveSelection(); // drop the panel's multi-highlight; the active layer is untouched
    }
}

// ---- S15-f: the Move tool's empty-space layer marquee ----------------------------------------
//
// Press on empty canvas -> the layer selection drops (handles + panel row alike). Drag on from
// there -> a rubber band; on release every layer whose content the band touched is selected as a
// multi-selection, so the whole set drags/scales/rotates together like a shift-click-gathered one.
// Shift or Ctrl at the press keeps the press-time selection so the band ADDS to it (the union: a
// band has no per-layer "was it already in?" reading to toggle against, unlike shift-CLICK).
//
// The band itself is not hand-drawn: it goes out through setSelectionMask, the same lane the S14
// marquee previews through, so the present pass renders it as marching ants in whatever overlay
// line style Settings→Appearance selects -- and a style change mid-drag restyles it live. The
// document's real pixel selection is never touched; its mask is put straight back on release/abort.

common::Rect VulkanCanvas::layerMarqueeRect() const {
    return common::Rect::fromCorners(m_layerMarqueeAnchor, m_layerMarqueeCursor);
}

void VulkanCanvas::beginLayerMarquee(common::Vec2 docPt, bool extend) {
    m_layerMarqueeActive = true;
    m_layerMarqueeLatched = false; // nothing is drawn until the drag clears the dead zone
    m_layerMarqueeDirty = false;
    m_layerMarqueeExtend = extend;
    m_layerMarqueeAnchor = m_layerMarqueeCursor = docPt;
    m_layerMarqueeBase = m_moveTargets;
    // The click half, resolved at the press (as click-away always was): a modifier-free press on
    // empty canvas deselects immediately -- the handles vanish and the panel loses its active row.
    // An extending press keeps everything, so the band can grow the set it was started from.
    if (!extend)
        clearMoveSelection();
    requestHostFrame();
}

void VulkanCanvas::dragLayerMarquee() {
    if (!m_layerMarqueeActive)
        return;
    if (!m_layerMarqueeLatched) {
        // The same dead zone the layer drag uses: a click's jitter must not flash a band.
        const common::Vec2 p = eventLogicalPoint();
        if ((p - m_movePressScreen).length() < kMoveDragDeadZonePx)
            return;
        m_layerMarqueeLatched = true;
        updateToolCursor(m_pointerInside); // the crosshair latches for the band's life
    }
    m_layerMarqueeCursor = eventDocPoint();
    m_layerMarqueeDirty = true; // the band's mask is rebuilt once per frame, in renderFrame
    requestHostFrame();
}

void VulkanCanvas::syncLayerMarqueeMask() {
    if (!m_layerMarqueeDirty)
        return;
    m_layerMarqueeDirty = false;
    const common::Vec2 size = m_view.documentSize();
    const core::Selection band =
        core::Selection::rectangle(static_cast<std::uint32_t>(size.x),
                                   static_cast<std::uint32_t>(size.y), layerMarqueeRect());
    setSelectionMask(band.width(), band.height(),
                     band.data().empty() ? nullptr : band.data().data());
}

void VulkanCanvas::finishLayerMarquee() {
    if (!m_layerMarqueeActive)
        return;
    const bool latched = m_layerMarqueeLatched;
    const bool extend = m_layerMarqueeExtend;
    const common::Rect rect = layerMarqueeRect();
    std::vector<core::LayerId> base = std::move(m_layerMarqueeBase);
    m_layerMarqueeActive = false;
    m_layerMarqueeLatched = false;
    m_layerMarqueeDirty = false;
    m_layerMarqueeBase.clear();
    if (!latched) {
        // A press that never travelled: the press already did the deselect (or, extended, meant
        // nothing at all), and no band was ever drawn. Nothing to gather, nothing to restore.
        updateToolCursor(m_pointerInside);
        return;
    }
    restoreDocumentMask(); // the band was canvas-only: the document's own ants come back
    core::Document* doc = m_moveHost.document ? m_moveHost.document() : nullptr;
    if (doc != nullptr) {
        // The UNCLIPPED band decides membership: Selection::rectangle clamped the drawn mask to the
        // canvas, but a layer parked outside the canvas is still a layer the band swept over.
        std::vector<core::LayerId> hits = core::layersInMarquee(doc->root(), rect);
        if (extend) { // Shift/Ctrl: the band grows the press-time set rather than replacing it
            std::vector<core::LayerId> merged = std::move(base);
            for (const core::LayerId id : hits)
                if (std::find(merged.begin(), merged.end(), id) == merged.end())
                    merged.push_back(id);
            hits = std::move(merged);
        }
        setMoveTargets(std::move(hits));
    }
    updateToolCursor(m_pointerInside);
    requestHostFrame();
}

void VulkanCanvas::cancelLayerMarquee() {
    if (!m_layerMarqueeActive)
        return;
    const bool latched = m_layerMarqueeLatched;
    m_layerMarqueeActive = false;
    m_layerMarqueeLatched = false;
    m_layerMarqueeDirty = false;
    m_layerMarqueeBase.clear();
    // Abandoning the BAND is all this does: the press-time deselect stands, exactly as a completed
    // click-away would (Esc has never un-deselected). Nothing is gathered.
    if (latched)
        restoreDocumentMask();
    updateToolCursor(m_pointerInside);
    requestHostFrame();
}

// ---- The Zoom tool ---------------------------------------------------------------------------
//
// See the block comment at zoomToolActive's declaration for the design. The framing preview goes
// out through the CONTROLS QUAD lane (WindowRenderer::setFramingPreview -> present-pass mode 9),
// the same lane the Move box and the crop rect use -- four screen-space corners and a mode, no
// document-sized anything. Not the selection/ants lane: ants mean "selected", they animate, and a
// crawling dashed rectangle following the pointer reads as a selection being dragged.

bool VulkanCanvas::zoomToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::Zoom;
}

std::optional<std::array<common::Vec2, 4>> VulkanCanvas::zoomPreviewQuad() const {
    // The preview shows while the Zoom tool has the pointer and nothing else is using it. A pan, a
    // rotate or a locked canvas is a different thing happening under the same cursor, and
    // previewing a click that is not the next thing to happen is a lie.
    if (!zoomToolActive() || !m_pointerInside || m_panning || m_rotating || m_spaceDown ||
        m_rotateDown || m_inpaintBusy)
        return std::nullopt;

    // What a left click actually does to the zoom -- asked of a COPY of the view rather than
    // assumed, so the CLAMP is honoured: at kMaxZoom the effective factor is 1 and the box becomes
    // the whole viewport, which is the honest picture of "this click changes nothing".
    CanvasView after = m_view;
    after.zoomAround(m_cursorLogical, kZoomClickStep);
    const double factor = m_view.zoom() > 0.0 ? after.zoom() / m_view.zoom() : 1.0;
    if (!(factor > 0.0))
        return std::nullopt;

    // zoomAround keeps the doc point under the anchor fixed, so in SCREEN space the whole
    // transform is a uniform scale about that anchor:
    //     screen'(p) = A + factor * (screen(p) - A)     for every document point p
    // (it falls straight out of the pan formula, and the view rotation cancels -- both frames
    // carry the same R). The region that will FILL the viewport is therefore the viewport rect
    // pulled toward the anchor by 1/factor. No document round-trip, no rotated-AABB approximation,
    // and exact at any view rotation -- the corners are already the screen quad the shader wants.
    const common::Vec2 a = m_cursorLogical;
    const double k = 1.0 / factor;
    const common::Vec2 v{static_cast<double>(w()), static_cast<double>(h())};
    const auto pull = [&](common::Vec2 corner) { return a + (corner - a) * k; };
    return std::array<common::Vec2, 4>{pull({0.0, 0.0}), pull({v.x, 0.0}), pull({v.x, v.y}),
                                       pull({0.0, v.y})};
}

void VulkanCanvas::syncZoomPreview() {
    if (!m_renderer)
        return;
    const std::optional<std::array<common::Vec2, 4>> quad = zoomPreviewQuad();
    m_renderer->setFramingPreview(quad.has_value(), quad.value_or(std::array<common::Vec2, 4>{}));
}

void VulkanCanvas::clickZoom(bool out) {
    // About the point clicked, so the pixel under the magnifier stays under the magnifier. The
    // tracked pointer, not the event pair, for the reason updateToolCursor gives -- and it is the
    // same point zoomPreviewQuad framed, so the click lands where the box promised.
    m_view.zoomAround(m_cursorLogical, out ? 1.0 / kZoomClickStep : kZoomClickStep);
    notifyViewChanged(); // the status bar's zoom readout + the rulers
    notifyCursor(true);  // a new document point sits under the same screen point
    // The box is re-derived from the new view on the next frame, so there is nothing to invalidate.
    requestHostFrame();
}

void VulkanCanvas::syncSampleArea() {
    if (!m_renderer)
        return;
    std::array<common::Vec2, 4> corners{};
    if (m_sampleAreaActive && !m_sampleAreaDocRect.empty()) {
        const common::Rect& r = m_sampleAreaDocRect; // doc px -> logical screen px (tracks the view)
        corners[0] = m_view.toScreen(r.topLeft());
        corners[1] = m_view.toScreen({r.right(), r.y});
        corners[2] = m_view.toScreen({r.right(), r.bottom()});
        corners[3] = m_view.toScreen({r.x, r.bottom()});
    }
    m_renderer->setSampleArea(m_sampleAreaActive, corners);
}

void VulkanCanvas::setInpaintSampleArea(const common::Rect& docRect) {
    m_sampleAreaDocRect = docRect;
    m_sampleAreaActive = !docRect.empty();
    requestHostFrame();
}

void VulkanCanvas::clearInpaintSampleArea() {
    m_sampleAreaActive = false;
    requestHostFrame();
}

void VulkanCanvas::syncMoveOverlay() {
    if (!m_renderer)
        return;
    std::array<common::Vec2, 4> corners{};
    // The same outline + 8 handles serve the Move tool's selection AND the Shape tool's selected
    // shape (§7.1 resize-vs-transform) -- the two tools are never active together, so one lane does
    // both. The Shape box reads the live edit-target layer (shapeBoxCorners). A selected LINE shows
    // its own gizmo (a connector + end handles + a round bend handle) INSTEAD of the box (S26).
    common::Vec2 la{}, lb{}, lmid{}, lminor{};
    bool lineGizmo = shapeEditActive() && editTargetIsLine() && lineGizmoPoints(la, lb, lmid);
    if (lineGizmo)
        lminor = lmid; // a Line carries no fourth handle
    // The Gradient tool's axis gizmo (S22) rides the same connector-plus-handles lane (the Gradient
    // tool is never active with the Shape/Move tools): a square handle at each end of the axis + a
    // round handle at its midpoint (drag to move the whole gradient).
    if (!lineGizmo && gradientToolActive() && gradientGizmoPoints(la, lb, lmid, lminor))
        lineGizmo = true;
    const bool show = !lineGizmo && ((moveToolActive() && moveTargetCorners(corners)) ||
                                     (shapeEditActive() && shapeBoxCorners(corners)));
    // The rotate-hotspot dots (user 2026-07-14): invisible corner rings are findable on a plain
    // rectangle and unfindable on a sheared/foreshortened one, so the corners grow faint dots that
    // fade in with the quad's wackiness (to a 25% ceiling). Pure maths; zero for any plain
    // rotated/scaled rectangle, so nothing changes on the boxes everyone already knows.
    const float dotAlpha =
        show ? static_cast<float>(rotateDotOpacity(transformQuadWackiness(corners))) : 0.0f;
    m_renderer->setTransformHandles(show, corners, dotAlpha);
    // The transform ANCHOR / reference point glyph (S15+): the Move tool's pivot for rotate + scale.
    // Only the Move tool (not the Shape gizmo, which shares this outline) carries a movable pivot.
    common::Vec2 anchorScreen{};
    const bool anchorShow = show && moveToolActive() && moveAnchorScreen(anchorScreen);
    m_renderer->setTransformAnchor(anchorShow, anchorScreen);
    m_renderer->setLineGizmo(lineGizmo, la, lb, lmid, lminor);
    // The transform HUD shows once a gesture has actually transformed the box (m_moveGesturePushed)
    // -- a press-without-drag (click-to-select) must not flash it (user 2026-06-17) -- OR briefly
    // after a double-click rotation reset (m_resetHudShowing, the "0.00 deg" flash).
    m_renderer->setMoveHudActive(
        moveToolActive() && ((m_transform.active() && m_moveGesturePushed) || m_resetHudShowing));
}

// The hover cursor over the Move tool's controls (see m_cursorState's encoding). Scale handles
// pick the stock resize cursor whose axis best matches the handle's screen direction, so
// rotated layers still feel right.
int VulkanCanvas::moveCursorState() const {
    if (m_layerMarqueeLatched)
        return 0; // banding empty canvas: the marquee crosshair, same glyph the S14 tools show
    if (m_anchorDragging)
        return 10; // dragging the pivot: the four-way move cursor
    if (m_transform.active()) {
        switch (m_transform.mode()) {
        case TransformMode::Move:
            return 10;
        case TransformMode::Rotate:
            return 15;
        default:
            return m_cursorState >= 10 ? m_cursorState : 10; // keep the grabbed handle's arrow
        }
    }
    const common::Vec2 p = m_cursorLogical; // the tracked pointer, never the live event (keyboard
                                            // contexts carry top-window coords -- updateToolCursor)
    // The transform anchor (pivot) sits over the handles/body, so its hover is picked first: the
    // four-way move cursor signals it is draggable.
    common::Vec2 anchorScreen{};
    if (moveAnchorScreen(anchorScreen) && (p - anchorScreen).length() <= kAnchorHitPx)
        return 10;
    std::array<common::Vec2, 4> corners{};
    if (!moveTargetCorners(corners))
        return -1;
    const std::optional<TransformHit> hit =
        hitTransformControls(p, corners, kHandleHitPx, kRotateBandPx);
    if (!hit)
        return -1;
    if (hit->mode == TransformMode::Move)
        return 10;
    if (hit->mode == TransformMode::Rotate)
        return 15;
    return resizeCursorFor(corners, hit->handle);
}

// ---- S33 DoF focus-band gizmo ----------------------------------------------------------------
//
// When the ACTIVE layer is a DofBlur adjustment (the host's provider says so), the canvas draws
// the focus-band chrome and lets the user drag it: the centre knob moves the band, the rotate
// knob turns it, and the band/feather edge lines set their half-widths. GEOMETRY ONLY -- the
// radius amount is scrubbed in the S32 popover, never on canvas, and that split is a hard
// constraint on this chrome. Deliberately tool-INDEPENDENT (doc §6): the press claims the handles
// ahead of every tool gesture, whatever tool is active. All drag maths run in the adjustment's
// PARENT space through the press-latched parentToDoc inverse, so a blur nested in a stretched
// group edits honestly.

bool VulkanCanvas::dofScreenGeom(DofGizmoState& state, DofScreenGeom& out) const {
    if (!m_dofGizmoProvider || !m_dofGizmoProvider(state))
        return false;
    const common::Vec2 docCenter = state.parentToDoc.apply(state.center);
    out.center = m_view.toScreen(docCenter);
    if (state.kind == BlurGizmoKind::Crosshair) {
        // The radial / ripple centre is a single knob -- no line, band edges, or rotate handle.
        out.dir = {1.0, 0.0};
        out.offBand = {0.0, 0.0};
        out.offFeather = {0.0, 0.0};
        out.rotateKnob = out.center;
        return true;
    }
    if (state.kind == BlurGizmoKind::Ring) {
        // The Vignette's radius arm. It points along the adjustment's parent +x, mapped through the
        // placement -- NOT along the pointer -- so the knob sits at one predictable compass point
        // whichever way the drag pulls, and a vignette inside a rotated group shows an arm that
        // rotates with the group. The drag reads |cursor - centre| in parent space, so pulling in
        // any direction still sizes the radius; only the knob's resting place is fixed.
        const common::Vec2 docArm = state.parentToDoc.applyVector({1.0, 0.0});
        if (docArm.length() < 1e-9)
            return false; // a collapsed placement leaves no arm to draw or grab
        const common::Vec2 screenArm = m_view.toScreen(docCenter + docArm) - out.center;
        const double len = screenArm.length();
        if (len < 1e-9)
            return false;
        out.dir = screenArm * (1.0 / len);
        const double radius = std::max(state.radius, 0.0);
        const common::Vec2 knob =
            m_view.toScreen(docCenter + state.parentToDoc.applyVector({radius, 0.0}));
        // Never let the radius knob collapse INTO the centre knob: at radius 0 (or a zoomed-far-out
        // view) the two Chebyshev boxes would overlap and the centre would swallow every press, so
        // the radius could never be dragged back out again. Push it to the rotate knob's fixed
        // stand-off distance, exactly the collision rule the band's rotate knob already uses.
        const double reach = (knob - out.center).length();
        out.rotateKnob = reach < kDofRotateKnobPx ? out.center + out.dir * kDofRotateKnobPx : knob;
        out.offBand = {0.0, 0.0};
        out.offFeather = {0.0, 0.0};
        return true;
    }
    const double a = state.angleDeg * kPi / 180.0;
    const common::Vec2 dp{std::cos(a), std::sin(a)}; // parent-space line direction
    // The PARENT-space normal, perpendicular BEFORE mapping: an anisotropic parent placement does
    // not preserve angles, so perping the mapped direction would put the edges on the wrong lines.
    const common::Vec2 np{-dp.y, dp.x};
    const common::Vec2 docDir = state.parentToDoc.applyVector(dp);
    if (docDir.length() < 1e-9)
        return false; // a collapsed placement leaves no line to draw or grab
    const common::Vec2 screenDir = m_view.toScreen(docCenter + docDir) - out.center;
    const double len = screenDir.length();
    if (len < 1e-9)
        return false;
    out.dir = screenDir * (1.0 / len);
    // The band/feather offsets map through parentToDoc as VECTORS along the parent normal. The
    // edges stay parallel to the focus line (an affine map preserves parallels), but their offset
    // need not stay perpendicular to it -- which is the honest picture of a sheared parent.
    const double band = std::max(state.band, 0.0);
    const double feather = std::max(state.feather, 0.0);
    out.offBand =
        m_view.toScreen(docCenter + state.parentToDoc.applyVector(np * band)) - out.center;
    out.offFeather =
        m_view.toScreen(docCenter + state.parentToDoc.applyVector(np * (band + feather))) -
        out.center;
    // The rotate knob rides the line at a FIXED logical offset from the centre knob, so it never
    // collides with the band lines' spacing at any zoom or band width.
    out.rotateKnob = out.center + out.dir * kDofRotateKnobPx;
    return true;
}

std::optional<int> VulkanCanvas::hitDofHandle(common::Vec2 screenPt) const {
    DofGizmoState st;
    DofScreenGeom g;
    if (!dofScreenGeom(st, g))
        return std::nullopt;
    // Knobs first: they sit ON the lines, so they must win the tie. Square knobs, so the grab
    // zone is the Chebyshev box the shader draws (kHandleHitPx, like the transform handles).
    const auto onKnob = [&](common::Vec2 k) {
        return std::max(std::abs(screenPt.x - k.x), std::abs(screenPt.y - k.y)) <= kHandleHitPx;
    };
    if (onKnob(g.center))
        return 0;
    if (st.kind == BlurGizmoKind::Crosshair)
        return std::nullopt; // the radial / ripple crosshair carries only the centre knob
    if (onKnob(g.rotateKnob))
        return 1; // the band's rotate knob, or the ring's radius knob (both handle id 1)
    if (st.kind == BlurGizmoKind::Ring)
        return std::nullopt; // the ring has no guide lines to grab -- centre and radius, no more
    // The guide lines are parallel, so ONE signed perpendicular coordinate tests all four: the
    // edge through center + off sits at dot(off, n). Band before feather, so the closer-in line
    // wins the tie when the feather is small.
    const common::Vec2 n{-g.dir.y, g.dir.x};
    const double s = (screenPt - g.center).dot(n);
    const double sBand = g.offBand.dot(n);
    const double sFeather = g.offFeather.dot(n);
    if (std::abs(s - sBand) <= kDofLineHitPx)
        return 2;
    if (std::abs(s + sBand) <= kDofLineHitPx)
        return 3;
    if (std::abs(s - sFeather) <= kDofLineHitPx)
        return 4;
    if (std::abs(s + sFeather) <= kDofLineHitPx)
        return 5;
    return std::nullopt;
}

// The union of every pointer gesture this file runs. The DoF gizmo is the first TOOL-INDEPENDENT
// chrome, so its hover paths (cursor shape, hot-handle highlight) run whatever tool is active --
// and must therefore go quiet the moment any tool's own gesture owns the pointer, which no single
// existing flag covers.
bool VulkanCanvas::pointerGestureActive() const {
    return m_panning || m_rotating || m_selMove.dragging() || m_gesture.active() ||
           m_transform.active() || m_reviewDrag >= 0 || m_chipDrawing || m_crop.active() ||
           m_cropRotating || m_brushStroking || m_brushPressPending || m_maskStrokeActive ||
           m_edgeStrokeActive || m_shapeBox.active() || m_lineHandle >= 0 || m_shapeDragging ||
           m_gradientDragging || m_gradientHandle >= 0 || m_textSelecting || m_textCreating ||
           textBoxGestureActive() || m_dofDrag.active || m_imageOpDrag.active() || m_warpDragging;
}

bool VulkanCanvas::pushDofHandles() {
    // The press position was tracked into m_cursorLogical just above the dispatch ladder, so the
    // hover test and the press test are the same test.
    const std::optional<int> hit = hitDofHandle(m_cursorLogical);
    if (!hit)
        return false; // no claim: the press falls through to the active tool's own gesture
    DofGizmoState st;
    DofScreenGeom g;
    if (!dofScreenGeom(st, g))
        return false;
    const std::optional<common::Affine2D> inv = st.parentToDoc.inverse();
    if (!inv)
        return false; // a singular placement has no parent frame to edit in -- refuse the claim
    m_dofDrag.active = true;
    m_dofDrag.handle = *hit;
    m_dofDrag.press = st;
    m_dofDrag.pressDoc = eventDocPoint();
    m_dofDrag.docToParent = *inv;
    return true;
}

void VulkanCanvas::dragDofHandle() {
    if (!m_dofDrag.active || !m_dofGizmoEdit)
        return;
    const common::Vec2 cursorDoc = eventDocPoint();
    // Every field starts from the PRESS-time state: one gesture edits one parameter, and basing
    // the others on the live layer would let a concurrent popover edit bleed into the drag.
    DofGizmoState next = m_dofDrag.press;
    const double a = m_dofDrag.press.angleDeg * kPi / 180.0;
    const common::Vec2 pn{-std::sin(a), std::cos(a)}; // parent-space band normal
    const char* coalesce = nullptr;
    switch (m_dofDrag.handle) {
    case 0: // centre knob: rigid move by the parent-space image of the cursor delta
        next.center = m_dofDrag.press.center +
                      m_dofDrag.docToParent.applyVector(cursorDoc - m_dofDrag.pressDoc);
        switch (m_dofDrag.press.kind) {
        case BlurGizmoKind::Crosshair: coalesce = "radial:center"; break;
        case BlurGizmoKind::Ring: coalesce = "ring:center"; break;
        case BlurGizmoKind::Band: coalesce = "dof:center"; break;
        }
        break;
    case 1: { // the band's rotate knob / the ring's radius knob
        const common::Vec2 rel = m_dofDrag.docToParent.apply(cursorDoc) - m_dofDrag.press.center;
        if (m_dofDrag.press.kind == BlurGizmoKind::Ring) {
            // Radius = the parent-space distance from the centre, whichever way the pull went.
            next.radius = std::max(0.0, rel.length());
            coalesce = "ring:radius";
            break;
        }
        if (rel.length() < 1e-6)
            return; // over the very centre the angle is undefined -- keep the last one
        next.angleDeg = std::atan2(rel.y, rel.x) * 180.0 / kPi;
        coalesce = "dof:angle";
        break;
    }
    case 2:
    case 3: { // band edges: the half-width is the cursor's |normal| coordinate (either side)
        const common::Vec2 rel = m_dofDrag.docToParent.apply(cursorDoc) - m_dofDrag.press.center;
        next.band = std::max(0.0, std::abs(rel.dot(pn)));
        coalesce = "dof:band";
        break;
    }
    case 4:
    case 5: { // feather edges: what remains past the press-time band (schema floor 1 px)
        const common::Vec2 rel = m_dofDrag.docToParent.apply(cursorDoc) - m_dofDrag.press.center;
        next.feather = std::max(1.0, std::abs(rel.dot(pn)) - m_dofDrag.press.band);
        coalesce = "dof:feather";
        break;
    }
    default:
        return;
    }
    m_dofGizmoEdit(coalesce, next);
    // Keep the rotate cursor orbiting as the knob turns: the FL_DRAG dispatch's notifyCursor only
    // TRACKS the pointer, it never rebuilds the cursor art -- so, like the Move box's rotate drag
    // (dragMoveTool), the drag routine has to re-run updateToolCursor itself. `true`: the pointer is
    // latched to this gesture, so keep the gizmo cursor even if the drag strays off the widget.
    updateToolCursor(true);
    requestHostFrame();
}

void VulkanCanvas::finishDofGesture() {
    if (!m_dofDrag.active)
        return;
    // The last streamed edit stands as the commit (the host coalesced the gesture into one undo
    // step); nothing lands here. Cleared so the next press opens a fresh step.
    m_dofDrag = DofDragState{};
}

// The hover/gesture cursor over the DoF gizmo's handles (m_cursorState encoding), or -1 when the
// gizmo does not own the pointer. Mirrors moveCursorState: a latched drag keeps its handle's
// cursor; a hover answers only while no other pointer gesture runs (a brush stroke passing near a
// guide line must not flip the cursor mid-stroke).
int VulkanCanvas::dofCursorState() const {
    int handle = -1;
    if (m_dofDrag.active) {
        handle = m_dofDrag.handle;
    } else {
        if (pointerGestureActive() || m_inpaintBusy || m_recomposeReview)
            return -1;
        const std::optional<int> hit = hitDofHandle(m_cursorLogical);
        if (!hit)
            return -1;
        handle = *hit;
    }
    if (handle == 0)
        return 10; // the four-way move arrows: the centre knob drags the whole band
    DofGizmoState st;
    DofScreenGeom g;
    if (!dofScreenGeom(st, g))
        return -1;
    if (handle == 1 && st.kind != BlurGizmoKind::Ring)
        return 15; // the rotate arrow, like the Move box's rotate band
    // A guide line adjusts along its screen NORMAL; the ring's radius knob adjusts along the arm
    // itself. Either way, pick the stock resize cursor whose axis best matches that pull, exactly
    // as resizeCursorFor does for the box handles -- a rotate arrow on a radius would lie.
    const common::Vec2 n = st.kind == BlurGizmoKind::Ring ? g.dir : common::Vec2{-g.dir.y, g.dir.x};
    double deg = std::atan2(n.y, n.x) * 180.0 / kPi;
    deg = std::fmod(deg + 360.0, 180.0); // axis, not direction
    if (deg < 22.5 || deg >= 157.5)
        return 12; // WE
    if (deg < 67.5)
        return 13; // NWSE
    if (deg < 112.5)
        return 11; // NS
    return 14;     // NESW
}

void VulkanCanvas::syncDofOverlay() {
    if (!m_renderer)
        return;
    DofGizmoState st;
    DofScreenGeom g;
    // The gizmo shows whenever the ACTIVE layer is a DofBlur adjustment, regardless of the active
    // tool (doc §6). The modal Recompose review suppresses it like every other editing surface.
    if (m_recomposeReview || !dofScreenGeom(st, g)) {
        m_renderer->setDofOverlay(false);
        return;
    }
    // The highlight: the latched handle mid-gesture, else the hovered one (only while no other
    // gesture owns the pointer -- the same gate the cursor uses, so the two always agree).
    int hot = -1;
    if (m_dofDrag.active)
        hot = m_dofDrag.handle;
    else if (m_pointerInside && !pointerGestureActive() && !m_inpaintBusy)
        hot = hitDofHandle(m_cursorLogical).value_or(-1);
    render::WindowRenderer::DofOverlay ov;
    if (st.kind == BlurGizmoKind::Crosshair) {
        // The radial / ripple centre: just the target mark, no guides (the shader draws it from
        // centerKnob).
        ov.kind = 1;
        ov.centerKnob = g.center;
        m_renderer->setDofOverlay(true, ov, hot);
        return;
    }
    if (st.kind == BlurGizmoKind::Ring) {
        // The Vignette ring rides the BAND shader path (kind 0) rather than a third one, because
        // that path already draws exactly the two things it needs and nothing else has to change on
        // the GPU side: a square move knob at centerKnob, a round knob at rotateKnob, and the
        // `line` guide between them as the radius arm. The four band/feather guides have no meaning
        // here, so they are parked far outside any viewport -- the shader's per-guide
        // `dl[i] >= 3.5 * S` skip then drops them for free, and the distance cull is a min() so a
        // remote segment cannot hold the gizmo awake either. (Zero-length stubs at the centre would
        // paint a dot under the knob; off-screen says "absent" without relying on cover-up.)
        constexpr common::Vec2 kParked{-1.0e5, -1.0e5};
        ov.kind = 0;
        ov.line = {g.center, g.rotateKnob};
        ov.bandA = ov.bandB = ov.featherA = ov.featherB = std::array<common::Vec2, 2>{kParked,
                                                                                      kParked};
        ov.centerKnob = g.center;
        ov.rotateKnob = g.rotateKnob;
        m_renderer->setDofOverlay(true, ov, hot);
        return;
    }
    // The DoF band: each infinite guide is pushed as a segment long enough to cross any viewport
    // from its own base point -- the widget diagonal plus the base's distance from the origin.
    const double diag = std::hypot(static_cast<double>(w()), static_cast<double>(h()));
    const auto guideSeg = [&](common::Vec2 off) {
        const common::Vec2 base = g.center + off;
        const double reach = diag + base.length() + kDofRotateKnobPx;
        return std::array<common::Vec2, 2>{base - g.dir * reach, base + g.dir * reach};
    };
    ov.line = guideSeg({0.0, 0.0});
    ov.bandA = guideSeg(g.offBand);
    ov.bandB = guideSeg(-g.offBand);
    ov.featherA = guideSeg(g.offFeather);
    ov.featherB = guideSeg(-g.offFeather);
    ov.centerKnob = g.center;
    ov.rotateKnob = g.rotateKnob;
    m_renderer->setDofOverlay(true, ov, hot);
}

// ---- S16 Crop tool ---------------------------------------------------------------------------
//
// The staged crop rect lives here (document space, axis-aligned); until a gesture touches it,
// it rests at the full canvas (the Photoshop/Affinity model: selecting the tool frames
// everything). The pointer side reuses the S15 controls -- the same 8 handles and hit-testing,
// with the rotate band disabled -- plus draw-a-fresh-rect outside the quad; the rect math is
// ui::CropGesture (pure). Apply (Enter / double-click inside) hands the rect to the host,
// which lands render::buildCropCommand as ONE undo step.

bool VulkanCanvas::cropToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::Crop;
}

common::Rect VulkanCanvas::cropRectValue() const {
    if (m_cropRect)
        return *m_cropRect;
    if (core::Document* doc = m_cropHost.document ? m_cropHost.document() : nullptr)
        return {0.0, 0.0, static_cast<double>(doc->width()), static_cast<double>(doc->height())};
    const common::Vec2 s = m_view.documentSize();
    return {0.0, 0.0, s.x, s.y};
}

void VulkanCanvas::setCropRect(const common::Rect& r) {
    // Snap the staged rect to whole document pixels so the crop box rides the pixel grid -- a crop
    // is integer pixels. This is the SAME snapCropRect the apply path uses (origin + size rounded
    // independently, S16-h), so the displayed box, the W/H readout and the committed crop all agree
    // (user 2026-06-17). Without a document we cannot clamp, so the raw rect passes through.
    common::Rect snapped = r;
    if (core::Document* doc = m_cropHost.document ? m_cropHost.document() : nullptr) {
        const CropPixels cp = snapCropRect(r, doc->width(), doc->height());
        snapped = {static_cast<double>(cp.x), static_cast<double>(cp.y), static_cast<double>(cp.w),
                   static_cast<double>(cp.h)};
    }
    m_cropRect = snapped;
    if (m_cropHost.rectChanged)
        m_cropHost.rectChanged(m_cropRect);
}

void VulkanCanvas::resetCropTool() {
    m_crop.cancel();
    m_cropRotating = false;
    setCropAngle(0.0); // the rotation dies with the staged box (S16-f rotate)
    m_chipDrawing = false; // any half-drawn Ctrl-drag chip dies with the staged rect
    m_chipDrawLatched = false;
    if (m_cropRect) {
        m_cropRect.reset();
        if (m_cropHost.rectChanged)
            m_cropHost.rectChanged(std::nullopt);
    }
}

void VulkanCanvas::cancelCropGesture() {
    m_crop.cancel(); // the STAGED rect (m_cropRect) survives -- only a half-built drag is dropped
    m_chipDrawing = false; // a half-drawn Ctrl-drag chip is dropped with it (tool switch)
    m_chipDrawLatched = false;
}

void VulkanCanvas::commitCrop() {
    if (cropToolActive())
        applyCropNow();
}

void VulkanCanvas::cancelCrop() {
    if (!cropToolActive())
        return;
    resetCropTool();  // drop the staged rect...
    ensureCropRect(); // ...and re-stage a ratio-conformed full-canvas rect
}

void VulkanCanvas::ensureCropRect() {
    if (!cropToolActive() || m_cropRect)
        return; // keep an already-staged rect; only establish one when there is none
    // S16-f: with Smart Resize ON the resting rect IS the suggestion (a set ratio repositions
    // the frame; Free trims it to the interesting content). It overrides the S16-q initial
    // framing (including DrawToBegin -- the toggle is the more specific ask: the user wants to
    // be shown a starting rect).
    if (smartResizeOn() && applySmartCropSuggestion())
        return;
    if (m_cropFraming == CropFraming::DrawToBegin)
        return; // S16-q: stage nothing -- the first drag draws the rect (GIMP-style)
    core::Document* doc = m_cropHost.document ? m_cropHost.document() : nullptr;
    if (doc == nullptr)
        return;
    const double w = static_cast<double>(doc->width());
    const double h = static_cast<double>(doc->height());
    common::Rect r{0.0, 0.0, w, h};
    if (m_cropFraming == CropFraming::Inset) { // S16-q: a fixed margin in from every edge
        constexpr double kCropInsetFraction = 0.15;
        r = {w * kCropInsetFraction, h * kCropInsetFraction, w * (1.0 - 2.0 * kCropInsetFraction),
             h * (1.0 - 2.0 * kCropInsetFraction)};
    }
    if (const double ratio = cropRatio(); ratio > 0.0)
        r = conformCropRect(r, ratio, doc->width(), doc->height());
    setCropRect(r);
    requestHostFrame();
}

bool VulkanCanvas::smartResizeOn() const {
    const Tool* tool = m_tools != nullptr ? m_tools->find(ToolId::Crop) : nullptr;
    if (tool == nullptr)
        return false;
    for (const ToolOption& o : tool->options())
        if (o.id == "smartResize")
            return o.value != 0.0;
    return false;
}

bool VulkanCanvas::applySmartCropSuggestion() {
    if (!m_cropHost.smartRect)
        return false;
    // The chips steer the search: enabled = never sliced, toggled-off = actively ignored. A
    // Free ratio is a valid ask (smart TRIM); the host handles every aspect.
    refreshSmartChips();
    std::vector<common::Rect> protects, excludes;
    for (const SmartChip& c : m_smartChips)
        (c.enabled ? protects : excludes).push_back(c.rect);
    const std::optional<common::Rect> r = m_cropHost.smartRect(cropRatio(), protects, excludes);
    if (r)
        setCropAngle(0.0); // a suggestion is axis-aligned; it replaces any staged rotation
    if (!r || r->empty())
        return false;
    setCropRect(*r);
    requestHostFrame();
    return true;
}

void VulkanCanvas::refreshSmartChips() {
    if (!m_cropHost.keepRegions) {
        m_smartChips.clear();
        return;
    }
    const std::vector<common::Rect> rects = m_cropHost.keepRegions();
    std::vector<SmartChip> next;
    next.reserve(rects.size());
    for (const common::Rect& r : rects) {
        SmartChip chip{r, true, false};
        // Keep the user's toggle across recomputes: the same document yields the same rects, so
        // exact-match carries the flag; an edited document naturally resets to all-enabled.
        for (const SmartChip& old : m_smartChips)
            if (!old.user && old.rect == r) {
                chip.enabled = old.enabled;
                break;
            }
        next.push_back(chip);
    }
    // Hand-marked chips (fork F-d) are the user's own, not the detector's: they ride along
    // verbatim until clicked away (or Smart Resize turns off, which clears the lot).
    for (const SmartChip& old : m_smartChips)
        if (old.user)
            next.push_back(old);
    m_smartChips = std::move(next);
}

VulkanCanvas::SmartChip* VulkanCanvas::smartChipAt(common::Vec2 docPt) {
    // Smallest hit chip wins so a chip nested in a bigger one stays reachable.
    SmartChip* hit = nullptr;
    for (SmartChip& c : m_smartChips)
        if (c.rect.contains(docPt) &&
            (hit == nullptr || c.rect.w * c.rect.h < hit->rect.w * hit->rect.h))
            hit = &c;
    return hit;
}

void VulkanCanvas::syncSmartChips() {
    if (m_renderer == nullptr)
        return;
    // One rect -> chip-quad mapping for every chip pushed below (placements, detector chips,
    // the in-flight Ctrl-drag rect) — corner order and view mapping can never diverge.
    const auto quadFor = [this](const common::Rect& r, render::WindowRenderer::ChipState state) {
        return render::WindowRenderer::KeepChip{
            {m_view.toScreen(r.topLeft()), m_view.toScreen({r.right(), r.y}),
             m_view.toScreen({r.right(), r.bottom()}), m_view.toScreen({r.x, r.bottom()})},
            state};
    };
    // Recompose review (plan §1.4): the chips channel shows the PLACEMENTS over the preview —
    // all kept-green, draggable — and the ordinary chip/offer machinery pauses underneath.
    if (m_recomposeReview) {
        std::vector<render::WindowRenderer::KeepChip> chips;
        chips.reserve(m_reviewPlacements.size());
        for (const common::Rect& r : m_reviewPlacements)
            chips.push_back(quadFor(r, render::WindowRenderer::ChipState::Kept));
        m_renderer->setKeepChips(chips);
        // Flush the frame's coalesced placement nudge (host: re-assemble + upload, ms-cheap).
        if (m_reviewNudgePending && m_reviewNudgeIdx >= 0 &&
            m_reviewNudgeIdx < static_cast<int>(m_reviewPlacements.size()) &&
            m_cropHost.reviewNudge) {
            m_reviewNudgePending = false;
            const common::Rect& r = m_reviewPlacements[static_cast<std::size_t>(m_reviewNudgeIdx)];
            m_cropHost.reviewNudge(static_cast<std::size_t>(m_reviewNudgeIdx), {r.x, r.y});
        }
        return;
    }
    // Chips show while the Crop tool is up with Smart Resize ON and a rect staged; every other
    // state (other tools, toggle off, post-apply rest) pushes none.
    std::vector<render::WindowRenderer::KeepChip> chips;
    if (cropToolActive() && smartResizeOn() && m_cropRect) {
        // Re-fetch each frame so the chips track the DOCUMENT, not the last suggestion — undo/
        // redo/edits move them immediately. The host caches the analysis on its composite
        // revision, so an unchanged frame costs two integer compares; user toggles survive the
        // merge (refreshSmartChips matches by rect).
        refreshSmartChips();
        const common::Rect staged = cropRectValue();
        chips.reserve(m_smartChips.size());
        for (const SmartChip& c : m_smartChips) {
            using ChipState = render::WindowRenderer::ChipState;
            // The chip's LIVE fate under the staged rect (the crop box stays boss — chips only
            // report): kept, sliced by an edge, or lost outside. Half-pixel slack so the snap-
            // rounded rect never flags a hairline "slice" on a chip it fully contains.
            ChipState state = ChipState::Disabled;
            if (c.enabled) {
                const common::Rect ov = staged.intersected(c.rect);
                if (ov.empty())
                    state = ChipState::Lost;
                else if (ov.w >= c.rect.w - 0.5 && ov.h >= c.rect.h - 0.5)
                    state = ChipState::Kept;
                else
                    state = ChipState::Sliced;
            }
            chips.push_back(quadFor(c.rect, state));
        }
        // The Ctrl-drag in flight (fork F-d): show the forming user chip live, in the kept
        // green -- it is being marked to keep, and the suggestion re-runs only on release.
        if (m_chipDrawing && m_chipDrawLatched && m_chipDrawRect.w >= 2.0 &&
            m_chipDrawRect.h >= 2.0)
            chips.push_back(quadFor(m_chipDrawRect, render::WindowRenderer::ChipState::Kept));
    }
    m_renderer->setKeepChips(chips);
    syncRecomposeOffer(); // same cadence, same inputs: the button state rides the chip sync
}

// ---- Smart Recompose (plan §1.3–§1.4): the offer + the review mode -----------------------------

void VulkanCanvas::syncRecomposeOffer() {
    // The Recompose button enables exactly when the enabled chips cannot all fit ANY crop
    // window at the chosen aspect (pure geometry: their union bbox vs the max-fit window — the
    // window slides anywhere in the document, so bbox-fits ⇔ some window keeps everything) AND
    // a rigid placement is actually feasible (or the button would offer a guaranteed failure).
    // Runs on the per-frame chip sync, so the test is MEMOED on its inputs (chips / aspect /
    // doc size) — an unchanged frame is a few compares, never a solver run. Free ratio never
    // offers: smart trim already keeps everything marked.
    bool offer = false;
    core::Document* doc = m_cropHost.document ? m_cropHost.document() : nullptr;
    if (cropToolActive() && smartResizeOn() && !m_recomposeReview && !m_inpaintBusy &&
        doc != nullptr) {
        const double aspect = cropRatio();
        if (aspect > 0.0) {
            std::vector<common::Rect> keeps;
            for (const SmartChip& c : m_smartChips)
                if (c.enabled)
                    keeps.push_back(c.rect);
            if (!keeps.empty()) {
                if (m_offerValid && aspect == m_offerAspect && doc->width() == m_offerDocW &&
                    doc->height() == m_offerDocH && keeps == m_offerKeeps) {
                    offer = m_offerResult; // inputs unchanged: reuse the memoed verdict
                } else {
                    const double dw = static_cast<double>(doc->width());
                    const double dh = static_cast<double>(doc->height());
                    // The ROUNDED max-fit box, exactly as recompose() computes its target — so
                    // the offer and the run can never disagree at the sub-pixel boundary.
                    const double maxW = std::min(dw, dh * aspect);
                    const auto tw = static_cast<double>(std::max(1L, std::lround(maxW)));
                    const auto th =
                        static_cast<double>(std::max(1L, std::lround(maxW / aspect)));
                    common::Rect bbox = keeps.front();
                    for (const common::Rect& r : keeps)
                        bbox = bbox.united(r);
                    const bool cropCanKeepAll = bbox.w <= tw + 0.5 && bbox.h <= th + 0.5;
                    if (!cropCanKeepAll) {
                        const core::retarget::RecomposeOptions defaults;
                        const double gap = defaults.minGapFrac * std::min(tw, th);
                        offer = !core::retarget::solvePlacements(keeps, dw, dh, tw, th, gap,
                                                                 defaults.solverMaxSweeps)
                                     .empty();
                    }
                    m_offerValid = true;
                    m_offerResult = offer;
                    m_offerAspect = aspect;
                    m_offerDocW = doc->width();
                    m_offerDocH = doc->height();
                    m_offerKeeps = std::move(keeps);
                }
            }
        }
    }
    if (offer != m_recomposeOfferLast) {
        m_recomposeOfferLast = offer;
        if (m_cropHost.recomposeOffer)
            m_cropHost.recomposeOffer(offer);
    }
}

std::optional<std::pair<double, std::vector<common::Rect>>> VulkanCanvas::recomposeRequest() const {
    if (!cropToolActive() || !smartResizeOn() || m_recomposeReview)
        return std::nullopt;
    const double aspect = cropRatio();
    if (aspect <= 0.0)
        return std::nullopt;
    std::vector<common::Rect> keeps;
    for (const SmartChip& c : m_smartChips)
        if (c.enabled)
            keeps.push_back(c.rect);
    if (keeps.empty())
        return std::nullopt;
    return std::make_pair(aspect, std::move(keeps));
}

void VulkanCanvas::enterRecomposeReview(std::vector<common::Rect> placements) {
    m_crop.cancel(); // no crop gesture survives into the modal review
    m_chipDrawing = false;
    m_chipDrawLatched = false;
    m_recomposeReview = true;
    m_reviewPlacements = std::move(placements);
    m_reviewDrag = -1;
    requestHostFrame();
}

void VulkanCanvas::exitRecomposeReview() {
    m_recomposeReview = false;
    m_reviewPlacements.clear();
    m_reviewDrag = -1;
    requestHostFrame();
}

void VulkanCanvas::pushReviewDrag() {
    // Grab the placement under the cursor (smallest wins, like the chips); anywhere else the
    // press is inert — the review is modal, pan/zoom stay on their own gestures.
    const common::Vec2 pt = eventDocPoint(); // preview space: the view shows the preview image
    int hit = -1;
    for (int i = 0; i < static_cast<int>(m_reviewPlacements.size()); ++i) {
        const common::Rect& r = m_reviewPlacements[static_cast<std::size_t>(i)];
        if (r.contains(pt) &&
            (hit < 0 || r.w * r.h < m_reviewPlacements[static_cast<std::size_t>(hit)].w *
                                        m_reviewPlacements[static_cast<std::size_t>(hit)].h))
            hit = i;
    }
    if (hit < 0)
        return;
    m_reviewDrag = hit;
    const common::Rect& r = m_reviewPlacements[static_cast<std::size_t>(hit)];
    m_reviewDragOffset = {pt.x - r.x, pt.y - r.y};
}

void VulkanCanvas::dragReviewPlacement() {
    if (m_reviewDrag < 0 || m_reviewDrag >= static_cast<int>(m_reviewPlacements.size()))
        return;
    common::Rect& r = m_reviewPlacements[static_cast<std::size_t>(m_reviewDrag)];
    const common::Vec2 pt = eventDocPoint();
    // The preview IS the displayed document right now, so its dims bound the placement.
    const common::Vec2 frame = m_view.documentSize();
    r.x = std::clamp(pt.x - m_reviewDragOffset.x, 0.0, std::max(0.0, frame.x - r.w));
    r.y = std::clamp(pt.y - m_reviewDragOffset.y, 0.0, std::max(0.0, frame.y - r.h));
    // Record only: the per-frame sync fires ONE reviewNudge (a document-sized assemble +
    // upload on the host side) per frame tick, like flushMoveDrag — never one per FL_DRAG.
    m_reviewNudgePending = true;
    m_reviewNudgeIdx = m_reviewDrag;
    requestHostFrame();
}

double VulkanCanvas::cropRatio() const {
    const Tool* tool = m_tools != nullptr ? m_tools->find(ToolId::Crop) : nullptr;
    if (tool == nullptr)
        return 0.0;
    int choice = 0;
    bool swap = false;
    double customW = 1.0, customH = 1.0;
    for (const ToolOption& o : tool->options()) {
        if (o.id == "ratio")
            choice = static_cast<int>(o.value);
        else if (o.id == "swap")
            swap = o.value != 0.0;
        else if (o.id == "ratioW")
            customW = o.value;
        else if (o.id == "ratioH")
            customH = o.value;
    }
    if (choice == kCropRatioCustom) // the ratioW:ratioH fields, not a preset
        return customCropRatio(customW, customH, swap);
    // "Original" is the document's own aspect.
    core::Document* doc = m_cropHost.document ? m_cropHost.document() : nullptr;
    const double docW = doc != nullptr ? doc->width() : m_view.documentSize().x;
    const double docH = doc != nullptr ? doc->height() : m_view.documentSize().y;
    return cropRatioForOptions(choice, swap, docW, docH);
}

void VulkanCanvas::cropOptionsChanged() {
    if (!cropToolActive())
        return;
    // Mid-review, only a change to the ASK invalidates the reviewed result: a new ratio or the
    // Smart Resize toggle going off drops the (expensive) preview. Unrelated options — Guides,
    // Delete Cropped Pixels — leave the review alone; they cost the user nothing here.
    if (m_recomposeReview && (cropRatio() != m_smartLastRatio || !smartResizeOn()) &&
        m_cropHost.reviewCancel)
        m_cropHost.reviewCancel();
    const double ratio = cropRatio();
    // S16-f Smart Resize edges: (re)compute the ONE suggestion when the toggle turns ON or the
    // aspect changes while ON (an input changed -- never a "different answer" re-roll, a
    // recorded guardrail); turning OFF reverts to the S16-q resting framing.
    const bool smart = smartResizeOn();
    const bool wasOn = m_smartResizeWasOn;
    const double lastRatio = m_smartLastRatio;
    m_smartResizeWasOn = smart;
    m_smartLastRatio = ratio;
    if (smart && (!wasOn || ratio != lastRatio) && applySmartCropSuggestion()) {
        requestHostFrame();
        return;
    }
    if (!smart && wasOn) {
        m_smartChips.clear(); // fresh chips (all-enabled) next time the toggle comes back on
        resetCropTool();
        ensureCropRect(); // smartResizeOn() is now false: stages per the S16-q framing mode
        requestHostFrame();
        return;
    }
    core::Document* doc = m_cropHost.document ? m_cropHost.document() : nullptr;
    // Free leaves the staged rect as-is; with nothing staged (DrawToBegin, pre-draw) a ratio change
    // must NOT synthesize a rect from the full-canvas fallback -- the resting box stays suppressed.
    if (ratio > 0.0 && doc != nullptr && m_cropRect)
        setCropRect(conformCropRect(cropRectValue(), ratio, doc->width(), doc->height()));
    // Any crop option may have changed the overlay (the Guides toggle, the ratio) -- kick a frame
    // so syncCropOverlay re-reads the options even when the rect itself did not move.
    requestHostFrame();
}

bool VulkanCanvas::cropCorners(std::array<common::Vec2, 4>& out) const {
    core::Document* doc = m_cropHost.document ? m_cropHost.document() : nullptr;
    if (doc == nullptr)
        return false;
    // S16-f rotate: the frame rect's corners mapped through the crop frame (identity at 0),
    // then into screen space — everything downstream (shader quad, hit-tests, guides) already
    // handles arbitrary quads.
    const std::array<common::Vec2, 4> dc =
        cropBoxCorners(cropRectValue(), m_cropAngle, m_cropFrameC);
    for (std::size_t i = 0; i < 4; ++i)
        out[i] = m_view.toScreen(dc[i]);
    return true;
}

void VulkanCanvas::setCropAngle(double a) {
    if (a == m_cropAngle)
        return;
    m_cropAngle = a;
    if (m_cropHost.angleChanged)
        m_cropHost.angleChanged(a);
}

void VulkanCanvas::pushCropTool() {
    core::Document* doc = m_cropHost.document ? m_cropHost.document() : nullptr;
    if (doc == nullptr)
        return;
    m_cropDragMoved = false; // a press is a "click" until the drag clears the slop (chip toggling)
    m_cropPressScreen = eventLogicalPoint();
    if (Fl::event_clicks() == 0)
        m_chipClickConsumed = false; // a FRESH press starts a new click sequence
    const common::Vec2 docPt = m_view.toDoc(m_cropPressScreen);
    // Ctrl-drag marks a keep-region by hand (Smart Recompose plan §1, fork F-d: manual marking is
    // refinement, not a hidden mode). The press only anchors; the chip is born on release, and a
    // sub-slop Ctrl-click falls through to the ordinary click actions (chip toggle/remove).
    if (smartResizeOn() && (Fl::event_state() & FL_CTRL) != 0) {
        m_chipDrawing = true;
        m_chipDrawLatched = false;
        m_chipDrawAnchor = {std::clamp(docPt.x, 0.0, static_cast<double>(doc->width())),
                            std::clamp(docPt.y, 0.0, static_cast<double>(doc->height()))};
        m_chipDrawRect = {m_chipDrawAnchor.x, m_chipDrawAnchor.y, 0.0, 0.0};
        return;
    }
    const common::Vec2 screenPt = m_cropPressScreen;
    const common::Rect rect = cropRectValue();
    std::array<common::Vec2, 4> corners{};
    // With nothing staged (DrawToBegin, before the first drag) there are no handles or body to
    // grab, so skip the hit-test and draw a fresh rect from this press.
    if (m_cropRect && cropCorners(corners)) {
        // S16-f rotate: the corner band arms a rotation — except in Smart Resize, whose chips
        // and suggestions are axis-aligned machinery (rotation is not a retargeting gesture).
        const double rotateBand = smartResizeOn() ? 0.0 : kRotateBandPx;
        if (const std::optional<TransformHit> hit =
                hitTransformControls(screenPt, corners, kHandleHitPx, rotateBand)) {
            if (hit->mode == TransformMode::Rotate) {
                if (Fl::event_clicks() > 0) { // double-click the band: back to axis-aligned,
                    resetCropRotation();      // box centre + size kept (the Move convention)
                    return;
                }
                // Rebase the frame so the pivot = the box's CURRENT world centre (same box,
                // new representation), then arm the drag about it.
                const std::array<common::Vec2, 4> dc =
                    cropBoxCorners(rect, m_cropAngle, m_cropFrameC);
                const common::Vec2 c{(dc[0].x + dc[2].x) * 0.5, (dc[0].y + dc[2].y) * 0.5};
                const common::Vec2 tl = docToCropFrame(dc[0], m_cropAngle, c);
                m_cropRect = common::Rect{tl.x, tl.y, rect.w, rect.h};
                m_cropFrameC = c;
                const common::Vec2 d{docPt.x - c.x, docPt.y - c.y};
                if (d.length() < 1e-9)
                    return; // grabbing the pivot itself: the angle is undefined
                m_cropRotating = true;
                m_cropAngle0 = m_cropAngle;
                m_cropRotatePress = std::atan2(d.y, d.x);
                m_cropRotateBase = *m_cropRect;
                // Constrain the rotation to the canvas only if the box STARTS fully inside
                // (half-pixel tolerance absorbs the snap rounding); an already-expanding box
                // rotates free.
                const double W = static_cast<double>(doc->width());
                const double H = static_cast<double>(doc->height());
                m_cropRotateFit = true;
                for (const common::Vec2& q : dc)
                    if (q.x < -0.51 || q.y < -0.51 || q.x > W + 0.51 || q.y > H + 0.51)
                        m_cropRotateFit = false;
                return;
            }
            // Move/Resize run in FRAME coordinates (CropGesture is rotation-blind; with the
            // fixed pivot, frame deltas map to document deltas exactly — cursor-following).
            const common::Vec2 framePt = docToCropFrame(docPt, m_cropAngle, m_cropFrameC);
            if (hit->mode == TransformMode::Scale) {
                m_crop.begin(CropMode::Resize, hit->handle, framePt, rect);
                return;
            }
            if (Fl::event_clicks() > 0) { // double-click inside applies (Photoshop)...
                // ...unless it lands on a keep-region chip: chip clicks belong to TOGGLING (the
                // first click already flipped it), and a quick second click must not commit the
                // crop mid-toggle (user 2026-07-02). Swallowed -- no apply, no second toggle
                // (the release handler skips clicks with a repeat count too). The consumed flag
                // covers a REMOVED user chip, which smartChipAt can no longer see.
                if (smartResizeOn() && (m_chipClickConsumed || smartChipAt(docPt) != nullptr))
                    return;
                applyCropNow();
                return;
            }
            m_crop.begin(CropMode::Move, -1, framePt, rect);
            return;
        }
    }
    // Outside the rect: draw a fresh one from this point. The anchor is bounded by the safety
    // ENVELOPE, not the canvas (S16-f: a draw may START in the letterbox and stage an expansion
    // from there — clamping to the canvas made outside presses start at the edge). A fresh draw
    // is axis-aligned: any staged rotation is dropped with the old box. The staged rect only
    // changes once the drag moves (a stray click keeps it), and Esc mid-drag restores it (angle
    // included).
    const double W = static_cast<double>(doc->width());
    const double H = static_cast<double>(doc->height());
    const double out = kCropOutsetFactor * std::max(W, H);
    const common::Vec2 anchor{std::clamp(docPt.x, -out, W + out),
                              std::clamp(docPt.y, -out, H + out)};
    m_cropDrawFromEmpty = !m_cropRect.has_value(); // DrawToBegin: Esc mid-draw returns to no rect
    m_cropAngle0 = m_cropAngle; // Esc mid-draw restores the rotated box
    setCropAngle(0.0);
    m_crop.begin(CropMode::Draw, -1, anchor, rect);
}

// Double-clicking the rotate band snaps the crop's rotation back to exactly 0: the box becomes
// axis-aligned again, world centre + size preserved (mirrors the Move tool's convention).
void VulkanCanvas::resetCropRotation() {
    if (m_cropAngle == 0.0 || !m_cropRect)
        return;
    const common::Rect r = *m_cropRect;
    const std::array<common::Vec2, 4> dc = cropBoxCorners(r, m_cropAngle, m_cropFrameC);
    const common::Vec2 c{(dc[0].x + dc[2].x) * 0.5, (dc[0].y + dc[2].y) * 0.5};
    setCropAngle(0.0);
    setCropRect({c.x - r.w * 0.5, c.y - r.h * 0.5, r.w, r.h});
}

void VulkanCanvas::dragCropTool() {
    // S16-f rotate: a rotate-band drag spins the staged box about the (rebased) frame pivot.
    // Shift snaps to the 15° grid; a small always-on magnet at exactly 0 makes returning to
    // axis-aligned (and so re-enabling the Inpaint fill) effortless.
    if (m_cropRotating) {
        core::Document* doc = m_cropHost.document ? m_cropHost.document() : nullptr;
        if (doc == nullptr)
            return;
        const common::Vec2 docPt = eventDocPoint();
        const common::Vec2 d{docPt.x - m_cropFrameC.x, docPt.y - m_cropFrameC.y};
        if (d.length() < 1e-9)
            return;
        double a = m_cropAngle0 + (std::atan2(d.y, d.x) - m_cropRotatePress);
        // Normalize to (-pi, pi] so the snaps and the zero-magnet act on the short way around.
        while (a > M_PI)
            a -= 2.0 * M_PI;
        while (a <= -M_PI)
            a += 2.0 * M_PI;
        if ((Fl::event_state() & FL_SHIFT) != 0) {
            constexpr double kSnap = 15.0 * M_PI / 180.0;
            a = std::round(a / kSnap) * kSnap;
        }
        if (std::abs(a) < 1.0 * M_PI / 180.0)
            a = 0.0; // the zero magnet
        setCropAngle(a);
        if (m_cropRotateFit) {
            // Largest centred scale of the press-time base whose rotated corners stay inside
            // the canvas (the Photoshop straighten behaviour). Derived from the BASE each
            // frame, so un-rotating grows the box right back.
            const common::Vec2 c = m_cropFrameC; // == the base rect's centre (rebased at press)
            const double W = static_cast<double>(doc->width());
            const double H = static_cast<double>(doc->height());
            double sfit = 1.0;
            const std::array<common::Vec2, 4> base = {
                m_cropRotateBase.topLeft(),
                common::Vec2{m_cropRotateBase.right(), m_cropRotateBase.y},
                common::Vec2{m_cropRotateBase.right(), m_cropRotateBase.bottom()},
                common::Vec2{m_cropRotateBase.x, m_cropRotateBase.bottom()}};
            for (const common::Vec2& q : base) {
                const common::Vec2 world = cropFrameToDoc(q, a, c);
                const common::Vec2 dir{world.x - c.x, world.y - c.y};
                if (dir.x > 1e-9)
                    sfit = std::min(sfit, (W - c.x) / dir.x);
                else if (dir.x < -1e-9)
                    sfit = std::min(sfit, c.x / -dir.x);
                if (dir.y > 1e-9)
                    sfit = std::min(sfit, (H - c.y) / dir.y);
                else if (dir.y < -1e-9)
                    sfit = std::min(sfit, c.y / -dir.y);
            }
            sfit = std::max(sfit, 0.0);
            setCropRect({c.x - m_cropRotateBase.w * 0.5 * sfit,
                         c.y - m_cropRotateBase.h * 0.5 * sfit, m_cropRotateBase.w * sfit,
                         m_cropRotateBase.h * sfit});
        }
        updateToolCursor(true); // the rotate glyph tracks the drag (the Move tool's convention)
        requestHostFrame();
        return;
    }
    if (!m_crop.active())
        return;
    core::Document* doc = m_cropHost.document ? m_cropHost.document() : nullptr;
    if (doc == nullptr)
        return;
    // Click slop: a real mouse click almost always jitters a pixel or two while pressed, and
    // FLTK reports that as FL_DRAG -- without a threshold, "clicking" a chip drew a tiny fresh
    // rect at the click point instead of toggling (user 2026-07-02). Until the cursor clears the
    // slop the press is still a click and the staged rect must not move.
    if (!m_cropDragMoved) {
        constexpr double kCropClickSlopPx = 3.0;
        if ((eventLogicalPoint() - m_cropPressScreen).length() < kCropClickSlopPx)
            return;
    }
    // Updating the staged rect per event is cheap (no recomposite -- the overlay reads it next
    // frame), unlike the Move tool's document-sized transform pushes.
    m_cropDragMoved = true; // a real drag: the release is not a chip-toggle click
    const auto state = Fl::event_state();
    // S16-f: the rect may leave the canvas (staged expansion); a zoom-aware snap band keeps
    // exact-edge crops effortless (8 screen px expressed in document units). Under a rotation
    // the box edges cannot align with the canvas, so the band is off — and the cursor works in
    // FRAME coordinates (see pushCropTool).
    const double snapTol = m_cropAngle == 0.0 ? 8.0 / std::max(m_view.zoom(), 1e-6) : 0.0;
    const common::Vec2 framePt = docToCropFrame(eventDocPoint(), m_cropAngle, m_cropFrameC);
    setCropRect(m_crop.rectFor(framePt, cropRatio(), (state & FL_SHIFT) != 0,
                               (state & FL_ALT) != 0, doc->width(), doc->height(), snapTol));
    requestHostFrame();
}

void VulkanCanvas::dragChipDraw() {
    if (!m_chipDrawing)
        return;
    core::Document* doc = m_cropHost.document ? m_cropHost.document() : nullptr;
    if (doc == nullptr)
        return;
    // Same click slop as the crop gestures: a jittery Ctrl-click stays a click (chip toggle),
    // it never smears a 1-px chip across the press point.
    if (!m_chipDrawLatched) {
        constexpr double kCropClickSlopPx = 3.0;
        if ((eventLogicalPoint() - m_cropPressScreen).length() < kCropClickSlopPx)
            return;
        m_chipDrawLatched = true;
    }
    const common::Vec2 docPt = eventDocPoint();
    const common::Vec2 cur{std::clamp(docPt.x, 0.0, static_cast<double>(doc->width())),
                           std::clamp(docPt.y, 0.0, static_cast<double>(doc->height()))};
    m_chipDrawRect = common::Rect::fromCorners(m_chipDrawAnchor, cur);
    requestHostFrame();
}

void VulkanCanvas::finishChipDraw(bool commit) {
    const bool wasLatched = m_chipDrawLatched;
    const common::Rect r = m_chipDrawRect;
    m_chipDrawing = false;
    m_chipDrawLatched = false;
    // A degenerate rect marks nothing (and a sub-slop Ctrl-click never latched at all -- the
    // release falls through to the ordinary chip click actions).
    if (!commit || !wasLatched || r.w < 2.0 || r.h < 2.0)
        return;
    m_smartChips.push_back({r, /*enabled=*/true, /*user=*/true});
    applySmartCropSuggestion(); // an input changed: the ONE suggestion re-runs (plan §3.8)
    requestHostFrame();
}

void VulkanCanvas::applyCropNow() {
    m_crop.cancel();
    m_cropRotating = false;
    if (m_cropHost.apply)
        m_cropHost.apply(cropRectValue(), m_cropAngle, m_cropFrameC);
    // The crop landed (or no-opped); either way the staged rect is consumed and the tool
    // rests at the full -- possibly just cropped -- canvas again.
    resetCropTool();
}

int VulkanCanvas::cropCursorState() const {
    if (m_recomposeReview) {
        // Review (plan §1.4): the move cursor over a draggable placement, the arrow elsewhere.
        const common::Vec2 pt = cursorDocPoint();
        for (const common::Rect& r : m_reviewPlacements)
            if (r.contains(pt))
                return 10;
        return -1;
    }
    if (m_cropRotating)
        return 15; // the reorienting rotate cursor, like the Move tool's band
    if (m_crop.active()) {
        switch (m_crop.mode()) {
        case CropMode::Move:
            return 10;
        case CropMode::Draw:
            return 0; // the marquee crosshair
        default:
            return m_cursorState >= 10 ? m_cursorState : 13; // keep the grabbed handle's arrow
        }
    }
    std::array<common::Vec2, 4> corners{};
    if (!cropCorners(corners))
        return -1;
    if (!m_cropRect)
        return 0; // nothing staged (DrawToBegin, pre-draw): a drag draws a fresh rect -> the
                  // crosshair, never the phantom full-canvas fallback's body/handle cursors
    const common::Vec2 p = m_cursorLogical; // hover test: the tracked pointer (updateToolCursor)
    const std::optional<TransformHit> hit = hitTransformControls(
        p, corners, kHandleHitPx, smartResizeOn() ? 0.0 : kRotateBandPx); // rotate band (S16-f)
    if (!hit)
        return 0; // a drag out here draws a fresh rect: the crosshair
    if (hit->mode == TransformMode::Rotate)
        return 15;
    if (hit->mode == TransformMode::Move)
        return 10;
    return resizeCursorFor(corners, hit->handle);
}

// ---- S35-b Mesh Warp / Perspective Warp (docs/warp-tools.md §5) --------------------------------

namespace {
// The outer boundary is drawn a touch heavier than the interior lines by emitting it TWICE, a
// fraction of a pixel either side of its own normal. The lane has exactly one line weight and the
// release overlay channel budget is 12, so a second weight cannot be bought with a new binding --
// and this file's own history says a new binding is also where a use-after-free lives.
constexpr double kWarpBoundaryOffsetPx = 0.4;
} // namespace

bool VulkanCanvas::warpToolActive() const {
    if (m_tools == nullptr)
        return false;
    const ToolId a = m_tools->active();
    return a == ToolId::MeshWarp || a == ToolId::PerspectiveWarp;
}

bool VulkanCanvas::warpSessionActive() const noexcept {
    return m_warpLayer != core::kInvalidLayerId;
}

core::Layer* VulkanCanvas::warpLayer() const {
    if (!warpSessionActive() || !m_warpHost.document)
        return nullptr;
    core::Document* doc = m_warpHost.document();
    return doc != nullptr ? doc->find(m_warpLayer) : nullptr;
}

common::Image* VulkanCanvas::warpablePixels(core::Layer* layer) {
    if (layer == nullptr)
        return nullptr;
    if (auto* raster = layer->as<core::RasterLayer>())
        return &raster->image();
    if (auto* magic = layer->as<core::MagicLayer>())
        return &magic->source();
    return nullptr;
}

std::string VulkanCanvas::warpRefusal(core::Layer* layer) {
    // Six SPECIFIC refusals, each naming the way forward -- the S36 rule, where four specific
    // messages replaced one generic one. A tool that says "cannot warp that" has told the user
    // nothing they could not already see.
    if (layer == nullptr)
        return _("Pick a layer to warp: the Layers panel's active row is what the warp deforms");
    if (layer->locked())
        return _("This layer is locked — unlock it (Layer ▸ Lock Layer) to warp it");
    switch (layer->kind()) {
    case core::LayerKind::Group:
        return _("A group has no pixels of its own to warp — warp the layers inside it instead");
    case core::LayerKind::Adjustment:
        return _("An adjustment layer has no pixel grid to warp — it is re-evaluated over whatever "
                 "is beneath it");
    case core::LayerKind::Vector:
        return _("Rasterize the shape first (Layer ▸ Rasterize): Warp deforms pixels, and a vector "
                 "layer has none of its own");
    case core::LayerKind::Text:
        return _("Rasterize the type first (Layer ▸ Rasterize): a text layer's pixels are re-drawn "
                 "from the block, so a warp of them would be discarded on the next re-draw");
    case core::LayerKind::Texture:
        return _("Rasterize the texture first (Layer ▸ Rasterize): a texture layer's pixels are "
                 "regenerated from its parameters, so a warp of them would not survive");
    case core::LayerKind::Raster:
    case core::LayerKind::Magic:
        break;
    }
    // A mask sheet rides a raster layer's pixel grid 1:1, so deforming the pixels without deforming
    // the coverage would slide the mask off what it was masking. Warping the mask WITH them is the
    // named follow-up (docs/warp-tools.md §5.2); refusing is the honest answer until it lands.
    if (const core::RasterMask* m = layer->mask(); m != nullptr && !m->empty() && m->enabled)
        return _("Warp does not deform a layer mask yet — apply or delete the mask first, so the "
                 "coverage cannot slide off the pixels it was masking");
    if (warpablePixels(layer) == nullptr)
        return _("This layer has no pixels of its own to warp");
    return {};
}

WarpOptions VulkanCanvas::warpOptions() const {
    WarpOptions o;
    const Tool* tool = m_tools != nullptr ? m_tools->activeTool() : nullptr;
    if (tool == nullptr)
        return o;
    for (const ToolOption& opt : tool->options()) {
        if (opt.id == "rows")
            o.rows = std::clamp(static_cast<int>(opt.value), kWarpMinNodes, kWarpMaxNodes);
        else if (opt.id == "cols")
            o.cols = std::clamp(static_cast<int>(opt.value), kWarpMinNodes, kWarpMaxNodes);
        else if (opt.id == "quality")
            o.quality = warpQualityForChoice(static_cast<int>(opt.value));
        else if (opt.id == "grid")
            o.showGrid = opt.value != 0.0;
    }
    return o;
}

void VulkanCanvas::bindWarpToActiveLayer(core::LayerId activeLayer) {
    if (!warpToolActive() || !m_warpHost.document)
        return;
    if (warpSessionActive() && m_warpLayer == activeLayer)
        return; // already bound to it: keep the handles exactly where they are
    cancelWarpSession();
    core::Document* doc = m_warpHost.document();
    core::Layer* layer = doc != nullptr ? doc->find(activeLayer) : nullptr;
    const std::string refusal = warpRefusal(layer);
    if (!refusal.empty()) {
        if (m_warpHost.refuse)
            m_warpHost.refuse(refusal);
        return;
    }
    const common::Image* px = warpablePixels(layer);
    if (px == nullptr || px->empty())
        return;
    m_warpLayer = activeLayer;
    m_warpBase = *px;
    m_warpBaseTransform = layer->transform();
    m_warpBaseWorld = core::worldTransform(*layer);
    m_warpBaseWorldInv = m_warpBaseWorld.inverse().value_or(common::Affine2D::identity());
    m_warpPreviewed = false;
    m_warpHandle = -1;
    m_warpHover = -1;
    // The lattice frames the layer's CONTENT, not its (usually document-sized) pixel grid: handles
    // parked in a transparent margin are handles on nothing. contentBounds() is the same box the Move
    // tool's own gizmo frames, so the two tools agree about where the layer is.
    const std::optional<common::Rect> content = layer->contentBounds();
    const common::Rect frame =
        content && !content->empty()
            ? *content
            : common::Rect{0.0, 0.0, static_cast<double>(px->width),
                           static_cast<double>(px->height)};
    const WarpOptions o = warpOptions();
    const core::WarpKind kind = m_tools != nullptr && m_tools->active() == ToolId::PerspectiveWarp
                                    ? core::WarpKind::Perspective
                                    : core::WarpKind::Mesh;
    // A stored grid of the SAME kind and size is re-adopted verbatim -- that is the re-editability
    // the tool promises. A stored grid of a different shape cannot be: its points describe a lattice
    // this session does not have, so the session starts undeformed over the current content and the
    // old grid is what the pixels already carry.
    const core::WarpGrid* stored = layer->warp();
    if (stored != nullptr && stored->valid() && stored->kind == kind
        && stored->cols == (kind == core::WarpKind::Perspective ? 2 : o.cols)
        && stored->rows == (kind == core::WarpKind::Perspective ? 2 : o.rows))
        m_warpFrom = *stored;
    else
        m_warpFrom = core::identityWarpGrid(frame, o.cols, o.rows, kind);
    m_warpGrid = m_warpFrom;
    requestHostFrame();
}

void VulkanCanvas::warpOptionsChanged() {
    if (!warpSessionActive())
        return;
    const WarpOptions o = warpOptions();
    const int wantCols = m_warpGrid.kind == core::WarpKind::Perspective ? 2 : o.cols;
    const int wantRows = m_warpGrid.kind == core::WarpKind::Perspective ? 2 : o.rows;
    if (wantCols == m_warpGrid.cols && wantRows == m_warpGrid.rows)
        return; // only Quality / Show grid moved: the lattice is untouched
    // A staged deformation cannot be carried across a resize -- a 4x4 drag has no meaning on a 6x6
    // lattice -- so the preview is put back and the new lattice starts where the pixels already are.
    restoreWarpBase();
    m_warpFrom = core::identityWarpGrid(m_warpFrom.source, wantCols, wantRows, m_warpGrid.kind);
    m_warpGrid = m_warpFrom;
    requestHostFrame();
}

std::vector<common::Vec2> VulkanCanvas::warpHandleScreen() const {
    std::vector<common::Vec2> out;
    if (!warpSessionActive())
        return out;
    const std::vector<common::Vec2> local = warpHandlePoints(m_warpGrid);
    out.reserve(local.size());
    for (const common::Vec2& p : local)
        out.push_back(m_view.toScreen(m_warpBaseWorld.apply(p)));
    return out;
}

void VulkanCanvas::pushWarpTool() {
    if (!warpSessionActive()) {
        // No session yet (the tool was picked while the active layer changed under it): try to bind
        // on the press, so the first click is never a silent no-op.
        if (m_warpHost.activeLayer)
            bindWarpToActiveLayer(m_warpHost.activeLayer());
        if (!warpSessionActive())
            return;
    }
    const std::optional<int> hit = hitWarpHandle(warpHandleScreen(), eventLogicalPoint());
    const bool alt = (Fl::event_state() & FL_ALT) != 0;
    if (!hit && !alt)
        return; // a press in open canvas is not a warp: the handles are the whole gesture
    m_warpHandle = hit.value_or(0); // Alt drags the WHOLE lattice, so any index will do
    m_warpDragBase = m_warpGrid;
    m_warpPressBase = m_warpBaseWorldInv.apply(eventDocPoint());
    m_warpDragBasePt = m_warpPressBase;
    m_warpDragShift = (Fl::event_state() & FL_SHIFT) != 0;
    m_warpDragAlt = alt;
    m_warpDragging = true;
    m_warpDragPending = false;
    m_warpDragMoved = false;
    updateToolCursor(true);
}

void VulkanCanvas::dragWarpTool() {
    if (!m_warpDragging)
        return;
    // Latched at EVENT time, applied from the frame loop: the drag point must be taken where
    // Fl::event_x/y still means our frame (eventLogicalPoint's rule), and the bake is far too
    // expensive to run once per motion event -- pushing per event is exactly what made the S15 Move
    // drag lag behind the pointer.
    m_warpDragBasePt = m_warpBaseWorldInv.apply(eventDocPoint());
    m_warpDragShift = (Fl::event_state() & FL_SHIFT) != 0;
    m_warpDragPending = true;
    requestHostFrame();
}

void VulkanCanvas::flushWarpDrag() {
    if (!m_warpDragging || !m_warpDragPending)
        return;
    m_warpDragPending = false;
    const core::WarpGrid next =
        warpDragged(m_warpDragBase, m_warpHandle, m_warpPressBase, m_warpDragBasePt,
                    warpDragModeFor(m_warpDragShift, m_warpDragAlt));
    if (next.points == m_warpGrid.points)
        return; // the pointer moved less than a pixel of lattice: nothing to re-bake
    m_warpGrid = next;
    m_warpDragMoved = true;
    // The live PIXEL preview, at draft quality (render::WarpQuality::Draft: a coarser subdivision and
    // the cheap kernel). A grid-only preview would leave the user guessing what the deformation
    // actually does to the picture, which is most of what this tool is for.
    //
    // A REFUSED bake (a perspective quad dragged inside-out) is deliberately not undone: the handles
    // stay where the user put them and the overlay draws the fold, which is the honest feedback,
    // while the last good pixels stay on screen rather than the layer blanking under the cursor.
    (void)bakeWarpPreview(render::WarpQuality::Draft);
}

void VulkanCanvas::finishWarpDrag() {
    if (!m_warpDragging)
        return;
    flushWarpDrag(); // land whatever the last motion event asked for before the drag closes
    m_warpDragging = false;
    m_warpHandle = -1;
    if (m_warpDragMoved)
        bakeWarpPreview(render::WarpQuality::Final); // the release pays for the real kernel
    m_warpDragMoved = false;
    updateToolCursor(m_pointerInside);
    requestHostFrame();
}

bool VulkanCanvas::bakeWarpPreview(render::WarpQuality quality) {
    core::Layer* layer = warpLayer();
    common::Image* px = warpablePixels(layer);
    if (px == nullptr || m_warpBase.empty())
        return false;
    if (m_warpGrid.points == m_warpFrom.points) {
        restoreWarpBase(); // back to undeformed: the pristine base IS the answer, byte for byte
        return true;
    }
    const render::WarpResult r =
        render::warpImage(m_warpBase, m_warpFrom, m_warpGrid, warpOptions().quality, quality);
    if (!r.ok || r.px.empty())
        return false;
    *px = r.px;
    // The offset is absorbed by the PLACEMENT, so not one pixel already on the canvas moves: the new
    // image's (0,0) sits at base-local (offX, offY), so the layer's transform gains exactly that
    // translation. Composed from the BASE transform every time, never from the live one, so a
    // hundred drag frames cannot drift.
    layer->setTransform(m_warpBaseTransform
                        * common::Affine2D::translation(static_cast<double>(r.offX),
                                                        static_cast<double>(r.offY)));
    if (auto* raster = layer->as<core::RasterLayer>())
        raster->invalidateContentBounds();
    else if (auto* magic = layer->as<core::MagicLayer>())
        magic->invalidateContentBounds();
    m_warpPreviewed = true;
    if (m_warpHost.previewChanged)
        m_warpHost.previewChanged();
    return true;
}

void VulkanCanvas::restoreWarpBase() {
    if (!m_warpPreviewed)
        return;
    core::Layer* layer = warpLayer();
    common::Image* px = warpablePixels(layer);
    m_warpPreviewed = false;
    if (px == nullptr)
        return;
    *px = m_warpBase;
    layer->setTransform(m_warpBaseTransform);
    if (auto* raster = layer->as<core::RasterLayer>())
        raster->invalidateContentBounds();
    else if (auto* magic = layer->as<core::MagicLayer>())
        magic->invalidateContentBounds();
    if (m_warpHost.previewChanged)
        m_warpHost.previewChanged();
}

void VulkanCanvas::commitWarp() {
    if (!warpSessionActive())
        return;
    if (m_warpGrid.points == m_warpFrom.points) {
        if (m_warpHost.refuse)
            m_warpHost.refuse(_("Nothing to apply: drag a handle to warp the layer first"));
        return;
    }
    core::Layer* layer = warpLayer();
    if (warpablePixels(layer) == nullptr)
        return;
    // Bake once more at FINAL quality from the pristine base -- never trust whatever the last drag
    // frame happened to leave in the layer, which may be a draft bake or a refused one.
    const render::WarpResult r = render::warpImage(m_warpBase, m_warpFrom, m_warpGrid,
                                                  warpOptions().quality, render::WarpQuality::Final);
    if (!r.ok || r.px.empty()) {
        if (m_warpHost.refuse)
            m_warpHost.refuse(_("That corner placement folds the layer back over itself — drag the "
                                "corners back into a convex shape"));
        return;
    }
    // The command must see the layer as it was BEFORE any preview bake, or its captured "old" state
    // would be a draft render. Put the base back first; the command then applies the real thing.
    restoreWarpBase();
    const common::Affine2D placement =
        m_warpBaseTransform
        * common::Affine2D::translation(static_cast<double>(r.offX), static_cast<double>(r.offY));
    // The grid PERSISTED with the layer is the live one re-homed into the new image's own pixel
    // space: the bake moved the origin by (offX, offY), so a grid left in the old coordinates would
    // describe handles in a space the layer no longer has (core::translatedWarpGrid's note).
    const core::WarpGrid stored = core::translatedWarpGrid(
        m_warpGrid, {-static_cast<double>(r.offX), -static_cast<double>(r.offY)});
    const core::LayerId id = m_warpLayer;
    // The session is dropped BEFORE the command lands: the command rewrites the very pixels and
    // placement the session's base was captured from, so re-binding afterwards is the only way the
    // handles can describe the layer that now exists.
    m_warpLayer = core::kInvalidLayerId;
    m_warpBase = common::Image{};
    m_warpDragging = false;
    m_warpHandle = -1;
    m_warpHover = -1;
    if (m_warpHost.commitWarp)
        m_warpHost.commitWarp(id, r.px, placement, stored);
    if (m_warpHost.activeLayer)
        bindWarpToActiveLayer(m_warpHost.activeLayer()); // re-arm on the warped layer
    requestHostFrame();
}

void VulkanCanvas::cancelWarp() {
    if (!warpSessionActive())
        return;
    restoreWarpBase();
    m_warpGrid = m_warpFrom; // the handles go back to where the layer's stored warp left them
    m_warpDragging = false;
    m_warpDragPending = false;
    m_warpHandle = -1;
    updateToolCursor(m_pointerInside);
    requestHostFrame();
}

void VulkanCanvas::cancelWarpSession() {
    if (!warpSessionActive())
        return;
    restoreWarpBase();
    m_warpLayer = core::kInvalidLayerId;
    m_warpBase = common::Image{};
    m_warpFrom = core::WarpGrid{};
    m_warpGrid = core::WarpGrid{};
    m_warpDragBase = core::WarpGrid{};
    m_warpDragging = false;
    m_warpDragPending = false;
    m_warpDragMoved = false;
    m_warpHandle = -1;
    m_warpHover = -1;
    requestHostFrame();
}

void VulkanCanvas::updateWarpHover() {
    if (!warpToolActive() || !warpSessionActive() || m_warpDragging)
        return;
    const std::optional<int> hit = hitWarpHandle(warpHandleScreen(), m_cursorLogical);
    const int want = hit.value_or(-1);
    if (want == m_warpHover)
        return;
    m_warpHover = want;
    requestHostFrame(); // the chrome's accent fill follows the hover
}

int VulkanCanvas::warpCursorState() const {
    if (!warpSessionActive())
        return -1;
    if (m_warpDragging)
        return 10; // the four-way move arrows: a handle drag moves a point
    return m_warpHover >= 0 ? 10 : -1;
}

void VulkanCanvas::syncWarpOverlay() {
    if (!m_renderer)
        return;
    if (!warpToolActive() || !warpSessionActive())
        return; // the lane was already cleared by syncLassoOverlay / syncPenChrome
    const WarpOptions o = warpOptions();
    // The lattice, sampled along the SAME Catmull-Rom surface the pixels ride (warpGridLines calls
    // render::warpSurfacePoint), then mapped base-local -> document -> logical screen px. Straight
    // chords between control points would draw a grid that does not bend the way the image does,
    // which is the one thing a warp overlay exists to show.
    if (o.showGrid) {
        const std::vector<std::vector<common::Vec2>> runs =
            warpGridLines(m_warpGrid, render::kLassoMaxVerts);
        const std::vector<std::size_t> boundary = warpBoundaryLines(m_warpGrid, runs.size());
        std::vector<common::Vec2> pts;
        pts.reserve(render::kLassoMaxVerts);
        const auto emit = [&](const std::vector<common::Vec2>& run) {
            if (run.size() < 2 || pts.size() + run.size() + 1 > render::kLassoMaxVerts)
                return;
            if (!pts.empty())
                pts.push_back(kPolylineBreak);
            for (const common::Vec2& p : run)
                pts.push_back(p);
        };
        for (std::size_t i = 0; i < runs.size(); ++i) {
            std::vector<common::Vec2> screen;
            screen.reserve(runs[i].size());
            for (const common::Vec2& p : runs[i])
                screen.push_back(m_view.toScreen(m_warpBaseWorld.apply(p)));
            emit(screen);
            // The outer boundary reads a touch heavier: it is emitted a second and third time, a
            // fraction of a pixel either side of its own normal (ui::thickenPolyline). Offsetting in
            // SCREEN space is deliberate -- the extra weight must not scale with the zoom.
            if (std::find(boundary.begin(), boundary.end(), i) != boundary.end())
                for (const std::vector<common::Vec2>& side :
                     thickenPolyline(screen, kWarpBoundaryOffsetPx))
                    emit(side);
        }
        m_renderer->setLassoPolyline(pts);
    } else {
        m_renderer->setLassoPolyline({});
    }
    // The handles: the PEN tool's own anchor squares, so the app has ONE handle vocabulary -- hollow
    // when idle, accent-filled while hovered or dragged (PenMark kind 0 = square, state bit 1 =
    // selected/filled, bit 2 = hovered). Rides the pen chrome channel, which syncPenChrome has just
    // cleared (the Pen can never be active while a warp tool is).
    std::vector<render::WindowRenderer::PenMark> marks;
    const std::vector<common::Vec2> handles = warpHandleScreen();
    marks.reserve(handles.size());
    for (std::size_t i = 0; i < handles.size(); ++i) {
        const bool hot = static_cast<int>(i) == (m_warpDragging ? m_warpHandle : m_warpHover);
        marks.push_back({handles[i], /*kind=*/0u,
                         static_cast<std::uint32_t>(hot ? (1u | 2u) : 0u)});
    }
    m_renderer->setPenChrome(marks, {}, {}, 0.0);
}

void VulkanCanvas::setLassoSmoothing(bool on) {
    if (m_gesture.smoothing() == on)
        return;
    m_gesture.setSmoothing(on); // marks the preview dirty; the next frame rebuilds line + mask
    if (m_gesture.active())
        requestHostFrame();
}

void VulkanCanvas::setPixelGrid(bool on) {
    if (!m_renderer)
        return;
    m_renderer->setPixelGrid(on);
    requestHostFrame();
}

void VulkanCanvas::setOverlayLineStyle(int style) {
    m_overlayLineStyle = style;
    if (!m_renderer)
        return; // applied when the renderer is created (the startup settings pass runs first)
    m_renderer->setOverlayLineStyle(style);
    requestHostFrame(); // restyle whatever chrome is on screen right now
}

void VulkanCanvas::setFeatherIndicator(int style) {
    m_featherIndicator = style;
    if (!m_renderer)
        return; // applied when the renderer is created (the startup settings pass runs first)
    m_renderer->setFeatherIndicator(style);
    requestHostFrame(); // re-indicate whatever selection is on screen right now
}

void VulkanCanvas::setAntsCirculate(bool on) {
    m_antsCirculate = on;
    if (!m_renderer)
        return; // applied when the renderer is created (the startup settings pass runs first)
    m_renderer->setAntsCirculate(on);
    requestHostFrame(); // re-dash whatever ants are on screen right now
}

void VulkanCanvas::syncLassoOverlay() {
    if (!m_renderer)
        return;
    const SelectionGesture::Kind kind = m_gesture.kind();
    const bool isLasso = m_gesture.active() && (kind == SelectionGesture::Kind::FreeLasso ||
                                                kind == SelectionGesture::Kind::PolyLasso);
    if (!isLasso) {
        m_renderer->setLassoPolyline({}); // inactive -> no line
        return;
    }
    // Map the path (doc px) to logical screen px (the renderer scales to physical, like the handle
    // corners). While a polygon is being placed, append the open rubber-band segment to the cursor.
    // pathPoints() is the smoothed freehand path when the Lasso toggle is on (else the raw
    // samples).
    const std::vector<common::Vec2> dp = m_gesture.pathPoints();
    std::vector<common::Vec2> screen;
    screen.reserve(dp.size() + 1);
    if (kind == SelectionGesture::Kind::FreeLasso && dp.size() > render::kLassoMaxVerts) {
        // A long freehand drag accumulates more points than the SSBO holds. Subsample by a STABLE
        // STRIDE on fixed indices (0, s, 2s, ...): m_points is append-only, so while s is constant
        // the kept points don't move -> no jiggle. s only steps up when the path crosses a whole
        // multiple of the cap (a rare, one-frame re-pick). Reserve one slot for the cursor end. (A
        // proportional resample re-picked every frame -> the line crawled, worse the longer it
        // got.) Commit uses all.
        const std::size_t n = dp.size();
        const std::size_t budget = render::kLassoMaxVerts - 1;
        const std::size_t stride = (n + budget - 1) / budget; // ceil(n / budget) >= 2 here
        for (std::size_t i = 0; i < n; i += stride)
            screen.push_back(m_view.toScreen(dp[i]));
        if ((n - 1) % stride != 0)
            screen.push_back(m_view.toScreen(dp[n - 1])); // always include the cursor end
    } else {
        for (const common::Vec2& d : dp)
            screen.push_back(m_view.toScreen(d));
    }
    if (kind == SelectionGesture::Kind::PolyLasso &&
        m_gesture.phase() == SelectionGesture::Phase::Placing)
        screen.push_back(m_view.toScreen(m_gesture.cursor()));
    m_renderer->setLassoPolyline(screen);
}

// ---- S19-a Brush tool ----

bool VulkanCanvas::brushToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::Brush;
}

bool VulkanCanvas::inpaintToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::InpaintBrush;
}

bool VulkanCanvas::eraserToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::Eraser;
}

bool VulkanCanvas::cloneToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::CloneStamp;
}

bool VulkanCanvas::strokeToolActive() const {
    return brushToolActive() || inpaintToolActive() || eraserToolActive() || cloneToolActive();
}

core::RasterLayer* VulkanCanvas::activeBrushLayer() const {
    if (!strokeToolActive() || !m_brushHost.document || !m_brushHost.activeLayer)
        return nullptr;
    core::Document* doc = m_brushHost.document();
    if (doc == nullptr)
        return nullptr;
    core::Layer* layer = doc->find(m_brushHost.activeLayer());
    if (layer == nullptr || !layer->visible())
        return nullptr; // no active layer, or a hidden one (painting it would show nothing)
    return layer->as<core::RasterLayer>(); // locked is allowed through; enforced at paint time
}

// A visible active layer the brush can't paint because it isn't a raster (a vector/text/etc. layer):
// brushing it is literally impossible, so the reticle padlocks on hover and a press shows the hint.
bool VulkanCanvas::activeBrushLayerUnpaintable() const {
    if (!strokeToolActive() || !m_brushHost.document || !m_brushHost.activeLayer)
        return false;
    if (maskPaintTarget() != nullptr)
        return false; // painting the MASK: any kind carrying one is a real target (S31)
    core::Document* doc = m_brushHost.document();
    core::Layer* layer = doc != nullptr ? doc->find(m_brushHost.activeLayer()) : nullptr;
    return layer != nullptr && layer->visible() && layer->as<core::RasterLayer>() == nullptr;
}

bool VulkanCanvas::activeBrushLayerLocked() const {
    // A running inpaint locks editing the same way a locked layer does (padlock reticle + no-op
    // press), so the brush can't disturb the pixels mid-operation.
    if (m_inpaintBusy) {
        return true;
    }
    if (const core::Layer* ml = maskPaintTarget(); ml != nullptr)
        return ml->locked(); // the mask lane's lock is the layer's own (S31)
    if (activeBrushLayerUnpaintable()) // a vector/text active layer: not paintable at all
        return true;
    const core::RasterLayer* raster = activeBrushLayer();
    return raster != nullptr && raster->locked();
}

core::Layer* VulkanCanvas::maskPaintTarget() const {
    // Only the Brush and Eraser follow the dock's mask aim; the Inpaint brush repairs CONTENT
    // and keeps painting pixels whatever the dock says (S31).
    if (!brushToolActive() && !eraserToolActive())
        return nullptr;
    if (!m_brushHost.maskTarget || !m_brushHost.maskTarget())
        return nullptr;
    if (!m_brushHost.document || !m_brushHost.activeLayer)
        return nullptr;
    core::Document* doc = m_brushHost.document();
    core::Layer* layer = doc != nullptr ? doc->find(m_brushHost.activeLayer()) : nullptr;
    if (layer == nullptr || !layer->visible())
        return nullptr; // painting an invisible layer's mask would show nothing
    core::RasterMask* mask = layer->mask();
    return (mask != nullptr && !mask->empty()) ? layer : nullptr;
}

void VulkanCanvas::setInpaintBusy(bool busy) {
    if (m_inpaintBusy == busy) {
        return;
    }
    m_inpaintBusy = busy;
    syncBrushReticle(m_pointerInside); // flip the reticle to/from the padlock immediately
    requestHostFrame();
}

void VulkanCanvas::beginSmoothedStroke(const core::brush::StrokeInput& first) {
    // Read the toggle FRESH at every press, never mid-stroke: changing the window under a filter that
    // is averaging the last N points would move the goalposts halfway through a line.
    const bool on = brushOption("smoothing", 1.0) > 0.5;
    m_smoother.setParams(
        core::brush::SmoothingParams{on ? core::brush::kSmoothingOnStrength : 0.0});
    m_smoother.begin(first);
}

double VulkanCanvas::brushOption(const char* id, double fallback) const {
    if (const Tool* tool = m_tools != nullptr ? m_tools->activeTool() : nullptr)
        for (const ToolOption& o : tool->options())
            if (o.id == id)
                return o.value;
    return fallback;
}

core::brush::BrushParams VulkanCanvas::currentBrushParams() const {
    core::brush::BrushParams p;

    // The Brush's active preset, if it has one: its TIP (procedural or bitmap), its option pipeline,
    // its spacing cadence, its paint/blend mode and its masking brush. Everything the preset decides
    // is copied in FIRST, and the context bar's live values are laid over it below -- so a preset is
    // a starting point the user can still steer, not a cage.
    //
    // Every other brush-family tool takes the branch below with `p` left at its defaults, which is a
    // null tip: the engine's analytic circle, parameterized by `hardness`, exactly as before.
    const bool paintTool = !inpaintToolActive() && !eraserToolActive() && !cloneToolActive();
    if (paintTool && m_brushHost.brushPreset) {
        if (const core::brush::BrushParams* preset = m_brushHost.brushPreset())
            p = *preset;
    } else if (eraserToolActive() && m_brushHost.eraserPreset) {
        // §8.4: the Eraser has a preset of its OWN, drawn from the erasers the Brush never offers.
        // Its tip, its options and its spacing are the preset's; what it does with them is fixed
        // below, because an eraser carves whatever nib it is holding.
        if (const core::brush::BrushParams* preset = m_brushHost.eraserPreset())
            p = *preset;
    }

    p.diameter = brushOption("size", 24.0);
    // ⚠ Only when there is no tip. A real tip carries its own edge, and `hardness` means nothing to
    // it (brush_engine.hpp) -- which is why Hardness is a `secondary()` option the bar does not show:
    // it belongs to the editor, on the tip page, where it can rebuild the tip that owns it (§8.1).
    if (p.tip == nullptr)
        p.hardness = brushOption("hardness", 80.0) / 100.0; // option is a percent
    if (inpaintToolActive()) {
        // The Inpaint brush paints a fixed translucent red overlay marking the region to fill (the
        // coverage doubles as the hole mask). Solid flow, capped at ~35% so it reads over content.
        p.flow = 1.0;
        p.opacity = 0.35;
        p.color = common::Color8{220, 40, 40, 255};
    } else if (eraserToolActive()) {
        // §8.4: the same stroke machinery, carving instead of painting (destination-out against
        // the pristine base). The Eraser bar has no Flow, so Opacity alone caps the carve -- the
        // engine's erase ceiling is opacity, never a colour's alpha, and the colour stays whatever
        // it defaults to because destination-out reads none.
        p.strokeMode = core::brush::StrokeMode::Erase;
        // ⚠ A PRESET'S FLOW IS ITS OWN -- the same rule the Brush obeys below, and for the same
        // reason: the preset's always-on Flow option carries the authored FlowValue as its strength
        // and the pipeline multiplies this base by it, so overwriting the base scales the preset's
        // flow by a number nothing shows. With NO preset (a null tip) the Eraser carves at full flow,
        // exactly as it always has, and this line is the identity it always was.
        if (p.tip == nullptr)
            p.flow = 1.0;
        p.opacity = brushOption("opacity", 100.0) / 100.0;
    } else if (cloneToolActive()) {
        // S38. The engine lays the stroke's ALPHA -- tip, spacing cadence, flow build-up, opacity
        // ceiling -- and stampCloneRegion replaces its deposit with the source pixels, so the colour
        // set here is never seen. What matters is that the stroke stays on the plain
        // `Uniform x Wash x Normal` path with an OPAQUE colour: that is the one combination whose
        // finished alpha is exactly `coverage x opacity`, which is the number core::applyCloneStamp
        // recomputes. Any other mode (a preset's tip, Buildup, a blend mode, the masking walk) would
        // make the engine's alpha and the clone's disagree, and the mark would land in one place
        // while the preview showed another. Hence: no preset here, ever.
        p.flow = brushOption("flow", 100.0) / 100.0;
        p.opacity = brushOption("opacity", 100.0) / 100.0;
        // Spacing is a fraction of the dab's extents (brush_engine.hpp); the bar states it as the
        // percentage every editor states it as.
        p.spacing = std::clamp(brushOption("spacing", 10.0) / 100.0, 0.01, 2.0);
        p.color = common::Color8{255, 255, 255, 255};
    } else {
        // ⚠ A PRESET'S FLOW IS ITS OWN. Its always-on Flow option carries the authored `FlowValue`
        // as that option's strength, and the pipeline multiplies the base by it -- so the base must
        // stay at the 1.0 the mapping set it to (io/brush/preset_brush.cpp). Writing the bar's flow
        // over it would scale the preset's flow by a number the bar does not even show (Flow is a
        // `secondary()` option; §8.1 gives it a home in the editor, where it edits the PRESET).
        if (p.tip == nullptr)
            p.flow = brushOption("flow", 100.0) / 100.0;
        // Opacity and Size are the bar's, always: they are the two live controls the user can see,
        // and a preset SEEDS them (app_window's applyBrushPreset) rather than overriding them.
        p.opacity = brushOption("opacity", 100.0) / 100.0;
        if (m_brushHost.foreground)
            p.color = m_brushHost.foreground();
    }
    return p;
}

core::brush::BrushDynamics VulkanCanvas::currentBrushDynamics() const {
    core::brush::BrushDynamics d;
    // Pressure drives size and flow -- the universal default for a paint brush, and the only thing
    // that makes a stylus feel like one. Both are exact identities at pressure 1, so every MOUSE
    // stroke stays byte-for-byte what it was before the tablet existed; only a real valuator moves
    // them. (Arc D's presets carry their own per-option dynamics and supersede this pair.)
    //
    // The Inpaint brush opts OUT of both, and that is not an oversight: its dabs are not paint, they
    // are the hole MASK the solver fills. A pressure-shrunken dab would mark a smaller region than
    // the reticle promises, and a pressure-thinned one could fall under the coverage threshold that
    // decides what counts as a hole -- so what you see the reticle cover is exactly what you mark.
    if (!inpaintToolActive()) {
        d.sizeFromPressure = true;
        d.flowFromPressure = true;
    }
    return d;
}

std::shared_ptr<const core::StrokeConfinement>
VulkanCanvas::strokeConfinement(const common::Affine2D& targetToDoc, std::uint32_t targetW,
                                std::uint32_t targetH) const {
    const core::Document* doc = m_brushHost.document ? m_brushHost.document() : nullptr;
    if (doc == nullptr)
        return nullptr; // no document, no selection: nothing to confine to
    return core::makeStrokeConfinement(doc->selection(), targetToDoc, targetW, targetH);
}

bool VulkanCanvas::brushLayerGrew() const noexcept {
    return m_brushGrowBox != core::layerPixelBox(m_brushGrowOrigW, m_brushGrowOrigH);
}

void VulkanCanvas::growBrushLayer(core::RasterLayer& layer, const core::Document* doc) {
    common::Image& img = layer.image();
    // The pre-press facts, recorded FIRST and unconditionally: everything below (and
    // revertBrushGrowth, and the commit's own growth) is expressed relative to them, and a stale
    // set left over from the previous stroke would re-home this one's pixels by the wrong offset.
    m_brushGrowOrigW = img.width;
    m_brushGrowOrigH = img.height;
    m_brushGrowTransform = layer.transform();
    m_brushGrowBox = core::layerPixelBox(img.width, img.height);
    m_brushCanvasBox = core::PixelBox{};
    if (doc == nullptr)
        return;
    // ONLY THE PAINT BRUSH GROWS. The Eraser takes paint away -- growing a layer to hold pixels it
    // just made transparent is pure waste -- and the Inpaint brush's dabs are not paint at all but a
    // hole mask over EXISTING content, read back out of the engine in the target's own coordinates
    // (brushHoleMask), which a growth would silently re-base.
    //
    // The Clone stamp opts out for the inpaint brush's reason and one of its own: it repairs
    // EXISTING content, and its pre-stroke snapshots (m_cloneBase, and the target->source map built
    // from the layer's world transform) are taken in the layer's own grid at the press -- a growth
    // would re-home the grid under them and slide every stamped pixel by the growth's origin.
    if (inpaintToolActive() || eraserToolActive() || cloneToolActive())
        return;
    // A raster layer's mask rides its image grid 1 px per image px, so either both grow or neither
    // does -- growing one alone would slide the mask across the content. "Neither" is a safe answer
    // (the stroke clips at the layer edge, exactly as it did before auto-grow existed) for the odd
    // case where something has already put the two grids out of step.
    const core::RasterMask* mask = layer.mask();
    if (mask != nullptr && (mask->width != img.width || mask->height != img.height))
        return;

    m_brushCanvasBox =
        core::canvasBoxInLayer(core::worldTransform(layer), doc->width(), doc->height());
    // At the press the stroke's own reach is unknown, so what is asked for is the CEILING itself --
    // the canvas -- never a pointer-derived box. (The commit asks for what the stroke actually
    // touched, through the same guarded function, so the growth that SURVIVES is tight.)
    const core::PixelBox grown =
        core::brushGrowthBox(img.width, img.height, m_brushCanvasBox, m_brushCanvasBox);
    if (grown == m_brushGrowBox)
        return; // the layer already covers the canvas: no allocation, and nothing to revert
    m_brushGrowBox = grown;
    const auto nw = static_cast<std::uint32_t>(grown.width());
    const auto nh = static_cast<std::uint32_t>(grown.height());
    // copyRegion reads outside its source as transparent, so the existing pixels land byte-exact
    // and the new band arrives empty -- which is what a layer that never had them looks like.
    img = common::copyRegion(img, grown.x0, grown.y0, nw, nh);
    if (mask != nullptr) {
        core::RasterMask ngrown(nw, nh, 255); // the new band REVEALS (GrowAndPaintLayerCommand)
        ngrown.enabled = mask->enabled;
        ngrown.linked = mask->linked;
        for (std::uint32_t row = 0; row < mask->height; ++row) {
            const long dy = static_cast<long>(row) - grown.y0;
            if (dy < 0 || dy >= static_cast<long>(nh))
                continue;
            for (std::uint32_t col = 0; col < mask->width; ++col) {
                const long dx = static_cast<long>(col) - grown.x0;
                if (dx < 0 || dx >= static_cast<long>(nw))
                    continue;
                ngrown.coverage[static_cast<std::size_t>(dy) * nw + static_cast<std::size_t>(dx)] =
                    mask->coverage[static_cast<std::size_t>(row) * mask->width + col];
            }
        }
        layer.setMask(std::move(ngrown));
    }
    // ... and the placement absorbs the shift, so nothing already on the layer moves by a pixel in
    // document space: old-local p now sits at p - origin, and `old * translate(origin)` maps it back.
    layer.setTransform(m_brushGrowTransform *
                       common::Affine2D::translation(static_cast<double>(grown.x0),
                                                     static_cast<double>(grown.y0)));
    layer.invalidateContentBounds();
}

void VulkanCanvas::revertBrushGrowth(core::RasterLayer& layer) {
    if (!brushLayerGrew())
        return;
    // ⚠ ONLY EVER AFTER BrushEngine::restore(): the image is pristine by then, so taking the
    // pre-press window back out of it recovers the layer byte for byte. The PERMANENT growth is the
    // undoable command's -- this grid was a working surface, and it leaves no trace.
    common::Image& img = layer.image();
    const long ox = m_brushGrowBox.x0;
    const long oy = m_brushGrowBox.y0;
    const auto gw = static_cast<std::uint32_t>(m_brushGrowBox.width());
    const auto gh = static_cast<std::uint32_t>(m_brushGrowBox.height());
    img = common::copyRegion(img, -ox, -oy, m_brushGrowOrigW, m_brushGrowOrigH);
    if (const core::RasterMask* mask = layer.mask();
        mask != nullptr && mask->width == gw && mask->height == gh) {
        core::RasterMask back(m_brushGrowOrigW, m_brushGrowOrigH, 255);
        back.enabled = mask->enabled;
        back.linked = mask->linked;
        for (std::uint32_t row = 0; row < back.height; ++row) {
            const long sy = static_cast<long>(row) - oy;
            for (std::uint32_t col = 0; col < back.width; ++col) {
                const long sx = static_cast<long>(col) - ox;
                back.coverage[static_cast<std::size_t>(row) * back.width + col] =
                    mask->coverage[static_cast<std::size_t>(sy) * gw + static_cast<std::size_t>(sx)];
            }
        }
        layer.setMask(std::move(back));
    }
    layer.setTransform(m_brushGrowTransform);
    layer.invalidateContentBounds();
    m_brushGrowBox = core::layerPixelBox(m_brushGrowOrigW, m_brushGrowOrigH);
}

void VulkanCanvas::beginBrushStroke(const core::brush::StrokeInput& in) {
    if (core::Layer* mlayer = maskPaintTarget(); mlayer != nullptr) { // the S31 mask lane
        if (mlayer->locked()) {
            if (m_brushHost.lockedAttempt)
                m_brushHost.lockedAttempt(); // the reticle already shows the padlock; add the hint
            return;
        }
        beginMaskStroke(*mlayer, in);
        return;
    }
    if (activeBrushLayerUnpaintable()) { // vector/text active layer: can't paint it -> hint + no-op
        if (m_brushHost.unpaintableAttempt)
            m_brushHost.unpaintableAttempt(); // "rasterize first", not "unlock" -- a different cause
        return;
    }
    core::RasterLayer* layer = activeBrushLayer();
    if (layer == nullptr)
        return; // no raster target (hidden / no layer): the press is a no-op
    if (layer->locked()) {
        if (m_brushHost.lockedAttempt)
            m_brushHost.lockedAttempt(); // the reticle already shows a padlock; add a status hint
        return;
    }
    if (layer->image().empty())
        return;

    m_brushLayer = layer->id();
    // AUTO-GROW, BEFORE ANYTHING READS THE GRID. The engine cannot be re-targeted mid-stroke (its
    // bounded working rect, its base snapshot and its two bboxes are all in layer-local pixels), so
    // the working grid the stroke will need has to exist at the press. It is the layer's own grid
    // united with the CANVAS and nothing more -- the pointer never sizes it -- and it is a no-op
    // costing not one allocation whenever the layer already covers the canvas, which is what a
    // layer created at document size looks like. Everything it does is undone in
    // finishBrushStroke/cancelBrushStroke; the growth that SURVIVES is the command's, sized to what
    // the stroke actually touched.
    growBrushLayer(*layer, m_brushHost.document ? m_brushHost.document() : nullptr);
    common::Image& img = layer->image();
    m_brushWorldInv = core::worldTransform(*layer).inverse().value_or(common::Affine2D::identity());
    core::brush::StrokeInput first = in; // canvas-local logical -> document -> layer-local
    // ⚠ SATURATED on the way in: the engine turns a dab centre into an integer box, and
    // `static_cast<int>` of a double at 1e13 is undefined behaviour, not a wrap (core/layer_grow.hpp).
    // Anything remotely on-canvas passes through bit-identical.
    first.pos = core::clampStrokePos(m_brushWorldInv.apply(m_view.toDoc(in.pos)));
    // Speed calibration is the user's (§7) and belongs to the stroke, so it lands before begin() --
    // which resets the EMA's value but never its params.
    m_brushEngine.setSpeedParams(m_speedParams);
    // The stroke is CONFINED to the active selection: the document's coverage mask, resampled once
    // onto this layer's pixel grid and multiplied into the stroke's alpha at composite (the engine
    // reads it; core/stroke_confinement.hpp explains why it rides beside the masking brush rather
    // than through it). No selection -> null -> the stroke is byte-identical to before.
    core::brush::BrushParams params = currentBrushParams();
    params.confine = strokeConfinement(core::worldTransform(*layer), img.width, img.height);
    // S38: resolve the clone's source BEFORE the engine begins. A stroke with no picked source has
    // nothing to stamp, so it must not begin at all -- an engine started and then abandoned would
    // leave a dab of the placeholder colour on the layer.
    if (cloneToolActive() && !beginCloneStroke(*layer, in)) {
        m_brushLayer = core::kInvalidLayerId;
        return;
    }
    m_cloneConfine = params.confine; // the clone composite applies it exactly as composite() does
    // The engine paints directly onto the live layer image and keeps its own BOUNDED pristine
    // snapshot of the touched region (S60-c) -- no full-layer copy here.
    m_brushEngine.begin(img.width, img.height, img, params, currentBrushDynamics(), first);
    const common::Rect dirty =
        m_brushEngine.composite(); // the first dab (under the cursor) shows immediately
    stampCloneRegion(dirty);       // ... as SOURCE pixels, when the clone stamp is what is live
    layer->invalidateContentBounds();
    m_brushStroking = true;
    if (m_brushHost.previewChanged)
        // `dirty` is already in the WORKING grid, which after growBrushLayer IS this layer's own
        // image grid -- so it is the layer-local claim the resident lane's incremental upload wants,
        // handed over beside the document rect rather than re-derived from it (S60-a item 13).
        m_brushHost.previewChanged(localRectToDocBBox(dirty, core::worldTransform(*layer)),
                                   layer->id(), dirty);
}

// The S31 mask lane's readout: a proxy texel's coverage byte. Painting gray writes its luma
// (Rec.709 integer weights); the eraser's destination-out carves alpha, and coverage follows it
// toward 0 -- erasing a mask HIDES (coverage is the mask's own "paint").
static std::uint8_t proxyCoverage(const common::Image& proxy, std::size_t px) {
    const std::size_t p = px * 4;
    const unsigned luma = (54u * proxy.rgba[p] + 183u * proxy.rgba[p + 1] +
                           19u * proxy.rgba[p + 2]) >> 8;
    return static_cast<std::uint8_t>((luma * proxy.rgba[p + 3] + 127u) / 255u);
}

void VulkanCanvas::mirrorMaskProxy(core::Layer& layer, const common::Rect& rect) {
    core::RasterMask* mask = layer.mask();
    if (mask == nullptr || mask->empty() || m_maskProxy.empty() || rect.empty())
        return;
    if (m_maskProxy.width != mask->width || m_maskProxy.height != mask->height)
        return; // the mask was replaced under the stroke; the next pump cancels cleanly
    const long x0 = std::max(0L, static_cast<long>(std::floor(rect.x)));
    const long y0 = std::max(0L, static_cast<long>(std::floor(rect.y)));
    const long x1 = std::min<long>(mask->width, static_cast<long>(std::ceil(rect.right())));
    const long y1 = std::min<long>(mask->height, static_cast<long>(std::ceil(rect.bottom())));
    for (long y = y0; y < y1; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * mask->width;
        for (long x = x0; x < x1; ++x)
            mask->coverage[row + x] = proxyCoverage(m_maskProxy, row + x);
    }
    layer.bumpMaskRevision(); // the dock's mask thumbnail re-renders on the next refresh
}

void VulkanCanvas::beginMaskStroke(core::Layer& layer, const core::brush::StrokeInput& in) {
    core::RasterMask* mask = layer.mask();
    // The engine paints RGBA; hand it the MASK as an opaque gray proxy -- full tips, dynamics
    // and presets work on masks for free, and the eraser carves coverage (see proxyCoverage).
    m_maskProxy = common::Image(mask->width, mask->height);
    for (std::size_t i = 0, n = static_cast<std::size_t>(mask->width) * mask->height; i < n; ++i) {
        const std::uint8_t v = mask->coverage[i];
        m_maskProxy.rgba[i * 4 + 0] = v;
        m_maskProxy.rgba[i * 4 + 1] = v;
        m_maskProxy.rgba[i * 4 + 2] = v;
        m_maskProxy.rgba[i * 4 + 3] = 255;
    }
    // Mask px -> document: through the layer's transform when linked (the mask grid is the
    // source grid for raster/magic, the doc window otherwise), through the parent chain alone
    // when unlinked -- exactly where the compositor folds it (render foldUnlinkedMask).
    // ⚠ ONE MAP, shared with the compositor. This block used to compose the transform by hand and
    // silently assumed the sheet sits at layer-local (0,0) -- true for a source-grid kind, false
    // for a vector/group/adjustment sheet, whose grid is the DOC WINDOW carried on
    // RasterMask::toLocal. A shape is centred on its own origin, so painting its mask landed
    // offset by half the shape. core::maskToDocument is the sanctioned map.
    const common::Affine2D maskToDoc = core::maskToDocument(layer, *mask);
    m_maskToDoc = maskToDoc;
    m_brushWorldInv = maskToDoc.inverse().value_or(common::Affine2D::identity());
    m_brushLayer = layer.id();
    core::brush::StrokeInput first = in; // canvas-local logical -> document -> mask px
    first.pos = core::clampStrokePos(m_brushWorldInv.apply(m_view.toDoc(in.pos)));
    m_brushEngine.setSpeedParams(m_speedParams);
    // The mask lane is confined too, and through the mask's OWN grid: the selection is a
    // document-space field, m_maskToDoc is what places this grid in the document, so painting a
    // mask inside a marquee stops at the marquee like painting pixels does. The mask lane never
    // GROWS -- a mask's grid is fixed by the layer it belongs to (the raster's own image grid), and
    // stretching it here would slide it off the content it masks.
    core::brush::BrushParams params = currentBrushParams();
    params.confine = strokeConfinement(m_maskToDoc, m_maskProxy.width, m_maskProxy.height);
    m_brushEngine.begin(m_maskProxy.width, m_maskProxy.height, m_maskProxy, params,
                        currentBrushDynamics(), first);
    const common::Rect dirty = m_brushEngine.composite(); // the first dab shows immediately
    mirrorMaskProxy(layer, dirty);
    m_brushStroking = true;
    m_brushMaskLane = true;
    if (m_brushHost.previewChanged)
        // The MASK lane names no layer rect: a mask's pixels are not in the layer's image space,
        // and a mask revision step re-sends the layer whole on the resident lane anyway.
        m_brushHost.previewChanged(localRectToDocBBox(dirty, m_maskToDoc), core::kInvalidLayerId,
                                   common::Rect{});
}

void VulkanCanvas::extendBrushSample(const core::brush::StrokeInput& in) {
    core::brush::StrokeInput s = in;
    // Saturated, exactly as the press is: a sample the engine cannot turn into an integer box is a
    // sample with undefined behaviour behind it, not merely a sample off the canvas.
    s.pos = core::clampStrokePos(m_brushWorldInv.apply(m_view.toDoc(in.pos)));
    m_brushEngine.extendTo(s);
}

void VulkanCanvas::brushSample(const core::brush::StrokeInput& in) {
    if (m_brushPressPending) {
        // The deferred first dab (see pushBrushTool). THIS sample -- the first the device actually
        // produced after the tip touched down -- is the one that carries the contact pressure, so
        // it is the one the stroke begins from. It is NOT smoothed: a stroke begins exactly where
        // the user pressed, and there is nothing behind it to average against anyway.
        m_brushPressPending = false;
        beginSmoothedStroke(in);
        beginBrushStroke(in);
        return;
    }
    // ⚠ THE ONE PLACE THE USER'S INPUT POINTS ARE MOVED. Everything downstream of here -- the spline,
    // the dab walk -- passes exactly through the points it is given, by design and as a hard rule
    // (core/brush/stroke_path.hpp). Denoising has to happen BEFORE that, or not at all: a mouse's
    // samples are integer positions at 60 Hz, and an interpolating scheme reproduces that rattle
    // faithfully. With smoothing off this is an exact identity.
    extendBrushSample(m_smoother.smooth(in));
}

void VulkanCanvas::pumpBrushStroke(const std::function<void()>& feed) {
    feed(); // may BEGIN the stroke (a deferred press) as well as extend it
    if (!m_brushStroking)
        return; // nothing began: no raster target, or it was locked / unpaintable
    core::Document* doc = m_brushHost.document ? m_brushHost.document() : nullptr;
    core::Layer* layer = doc != nullptr ? doc->find(m_brushLayer) : nullptr;
    if (m_brushMaskLane) { // the S31 mask lane: composite the proxy, mirror it into the mask
        if (layer == nullptr || layer->mask() == nullptr) {
            cancelBrushStroke(); // the layer or its mask vanished mid-stroke
            return;
        }
        common::Rect dirty;
        {
            MOSAIC_PERF_SCOPE("Brush dabs (mask)", Lane::Cpu);
            dirty = m_brushEngine.composite();
        }
        mirrorMaskProxy(*layer, dirty);
        if (m_brushHost.previewChanged)
            m_brushHost.previewChanged(localRectToDocBBox(dirty, m_maskToDoc),
                                       core::kInvalidLayerId, common::Rect{});
        return;
    }
    auto* raster = layer != nullptr ? layer->as<core::RasterLayer>() : nullptr;
    if (raster == nullptr) {
        cancelBrushStroke(); // the layer vanished mid-stroke: drop the gesture cleanly
        return;
    }
    // The paint-stroke hot path: one dab-batch composite per pump. The --bench paint-stroke
    // scenario measures this same call, so the two are directly comparable.
    common::Rect dirty;
    {
        MOSAIC_PERF_SCOPE("Brush dabs", Lane::Cpu);
        dirty = m_brushEngine.composite(); // ONE refresh covers the whole batch
    }
    {
        // S38: the clone stamp's own half of the batch -- a second pass over the SAME rect, so it
        // gets its own row rather than hiding inside the brush's. A no-op for every other tool.
        MOSAIC_PERF_SCOPE("Clone stamp deposit", Lane::Cpu);
        stampCloneRegion(dirty);
    }
    raster->invalidateContentBounds();
    if (m_brushHost.previewChanged)
        // THE HOT PATH the S60-a gate's `gpu edit 256` row measures: one dab batch, one layer-local
        // rect, one macrotile of transfer instead of a whole layer.
        m_brushHost.previewChanged(localRectToDocBBox(dirty, core::worldTransform(*layer)),
                                   raster->id(), dirty);
}

void VulkanCanvas::pushBrushTool() {
    const common::Vec2 press = eventLogicalPoint();
    const double fx = press.x;
    const double fy = press.y;
    // ⚠ With a stylus on the tablet, the contact sample HAS NOT ARRIVED YET. Measured on XWayland /
    // KWin 6.7.0: the core ButtonPress that becomes this FL_PUSH reaches the client ahead of the XI2
    // events carrying the same contact, so the ring is empty right now and pressSample() would hand
    // back a synthesized pressure-1.0 sample -- stamping a full-size, full-flow blob at the head of
    // every single tablet stroke before it settled to the pressure the nib really made.
    //
    // So DEFER: remember that a press is owed, and let the first real sample begin the stroke
    // (brushSample). The drain on the next FL_DRAG does it, and a tap that never drags is closed out
    // by finishBrushStroke, which drains before it ends. A MOUSE takes the old path unchanged -- it
    // has no contact sample to wait for, and its first dab must land on the press, not on the drag.
    if (m_tablet.stylusInProximity()) {
        m_brushPressPending = true;
        return;
    }
    const core::brush::StrokeInput first = m_tablet.pressSample(fx, fy);
    beginSmoothedStroke(first); // a MOUSE begins on the spot; the smoother starts from that point
    beginBrushStroke(first);
}

void VulkanCanvas::dragBrushTool() {
    if (!m_brushStroking && !m_brushPressPending)
        return;
    m_cursorLogical = eventLogicalPoint();
    // Drain the whole XI2 burst into the stroke (§3.1): FLTK coalesces motion to the frame rate,
    // the device does not, and feeding every buffered sample in order is what stops a fast stroke
    // from going polygonal. With no stylus, the drain synthesizes exactly one sample from the event.
    const common::Vec2 at = m_cursorLogical; // captured here: pumpBrushStroke may re-enter the
                                             // host, and the fallback synth needs OUR frame
    pumpBrushStroke([this, at] {
        m_tablet.drain(at.x, at.y, [this](const core::brush::StrokeInput& in) { brushSample(in); });
    });
}

// --- the Wayland lifecycle sink -----------------------------------------------------------------
// On native Wayland the compositor stops emulating pointer events for a tablet-aware client (§4
// finding 4), so FLTK never sees the pen and these five replace FL_PUSH/FL_DRAG/FL_RELEASE/FL_MOVE.
// The pen therefore drives the STROKE TOOLS and nothing else there -- it cannot press a button or
// drag a marquee, because those need FLTK events nobody is sending. XWayland (the shipped default)
// is unaffected: there the pen is still a mouse to FLTK, and every tool works.

core::Selection VulkanCanvas::brushHoleMask() const {
    const std::uint32_t w = m_brushEngine.width();   // layer (document) dimensions
    const std::uint32_t h = m_brushEngine.height();
    // Coverage is now the stroke's BOUNDED footprint, placed at (ox, oy); scatter it into a
    // document-sized hole mask (S60-c).
    const std::uint32_t cw = m_brushEngine.coverageWidth();
    const std::uint32_t ch = m_brushEngine.coverageHeight();
    const std::int32_t ox = m_brushEngine.coverageOriginX();
    const std::int32_t oy = m_brushEngine.coverageOriginY();
    const std::vector<float>& cov = m_brushEngine.coverage();
    if (w == 0 || h == 0 || cov.size() != static_cast<std::size_t>(cw) * ch)
        return {};
    core::Selection sel(w, h); // all-zero
    std::vector<std::uint8_t>& d = sel.data();
    bool any = false;
    for (std::uint32_t ly = 0; ly < ch; ++ly)
        for (std::uint32_t lx = 0; lx < cw; ++lx)
            if (cov[static_cast<std::size_t>(ly) * cw + lx] > 0.1f) { // solid hole; drop the AA fringe
                d[static_cast<std::size_t>(oy + ly) * w + (ox + lx)] = 255;
                any = true;
            }
    return any ? std::move(sel) : core::Selection{};
}

void VulkanCanvas::finishBrushStroke() {
    if (m_brushPressPending) {
        // A TAP: the tip touched and lifted, and FLTK never sent a drag in between. The contact
        // samples are sitting in the ring -- drain them, so the tap lands its dab at the pressure
        // the nib actually made instead of painting nothing at all.
        dragBrushTool(); // begins from the first real sample, extends with the rest
        m_brushPressPending = false; // ... and if the ring somehow held nothing, the press is spent
    }
    if (!m_brushStroking)
        return;
    // TWO tails are owed here, in this order, and both for the same underlying reason -- something
    // downstream of the pointer is running behind it:
    //
    //  1. THE SMOOTHER's. An averaged point necessarily TRAILS the raw input (that is what a filter
    //     is), so a stroke that just stopped would fall SHORT of the last thing the user did -- the
    //     pen lifts at the end of a flick and the paint never gets there. flush() ramps the window
    //     down and finishes on the user's own final point, unsmoothed.
    //  2. THE DAB WALK's. The path is a curve THROUGH the samples, and fitting one needs to know
    //     where the stroke goes next, so the span ending at the last sample is still unstamped.
    //
    // Both go through pumpBrushStroke so the result is COMPOSITED: everything below reads the
    // stroke's PIXELS (the inpaint hole mask, the committed region), and without this they would read
    // a stroke that stops short of where the user actually stopped.
    pumpBrushStroke([this] {
        for (const core::brush::StrokeInput& owed : m_smoother.flush())
            extendBrushSample(owed);
        m_brushEngine.flush();
    });
    m_brushStroking = false;
    const bool inpaint = inpaintToolActive();
    core::Selection hole =
        inpaint ? brushHoleMask() : core::Selection{}; // before end() (engine kept)
    m_brushEngine.end();
    // S38: the clone's pre-stroke snapshots are spent -- the pixels are already in the layer, and
    // everything below is the ordinary paint commit. The PICKED SOURCE survives; it is the tool's
    // memory, and an aligned second stroke depends on it.
    clearCloneStrokeState();
    core::Document* doc = m_brushHost.document ? m_brushHost.document() : nullptr;
    core::Layer* layer = doc != nullptr ? doc->find(m_brushLayer) : nullptr;

    if (m_brushMaskLane) { // the S31 mask lane's commit -- the paint branch's exact dance
        m_brushMaskLane = false;
        core::RasterMask* mask = layer != nullptr ? layer->mask() : nullptr;
        if (mask == nullptr || m_maskProxy.width != mask->width ||
            m_maskProxy.height != mask->height) {
            m_maskProxy = common::Image{};
            m_brushLayer = core::kInvalidLayerId; // the mask vanished: nothing to restore/commit
            return;
        }
        const common::Rect db = m_brushEngine.dirtyBounds();
        if (m_brushHost.commitMaskStroke && !db.empty()) {
            // Read the stroke's new coverage out of the proxy, then restore BOTH the proxy and
            // the live mask to their pre-stroke bytes so the command captures the correct "old"
            // region on apply (it re-applies the painted bytes -- no flicker).
            const long ox = static_cast<long>(db.x);
            const long oy = static_cast<long>(db.y);
            const auto rw = static_cast<std::uint32_t>(db.w);
            const auto rh = static_cast<std::uint32_t>(db.h);
            std::vector<std::uint8_t> region(static_cast<std::size_t>(rw) * rh, 0);
            for (std::uint32_t ry = 0; ry < rh; ++ry) {
                const long my = oy + static_cast<long>(ry);
                if (my < 0 || my >= static_cast<long>(mask->height))
                    continue;
                for (std::uint32_t rx = 0; rx < rw; ++rx) {
                    const long mxp = ox + static_cast<long>(rx);
                    if (mxp < 0 || mxp >= static_cast<long>(mask->width))
                        continue;
                    region[static_cast<std::size_t>(ry) * rw + rx] = proxyCoverage(
                        m_maskProxy, static_cast<std::size_t>(my) * mask->width + mxp);
                }
            }
            m_brushEngine.restore();
            mirrorMaskProxy(*layer, db);
            m_brushHost.commitMaskStroke(m_brushLayer, std::move(region), rw, rh, ox, oy);
        } else { // nothing painted: just revert, no edit
            m_brushEngine.restore();
            mirrorMaskProxy(*layer, db);
            if (!db.empty() && m_brushHost.previewChanged)
                m_brushHost.previewChanged(localRectToDocBBox(db, m_maskToDoc),
                                           core::kInvalidLayerId, common::Rect{});
        }
        m_maskProxy = common::Image{};
        m_brushLayer = core::kInvalidLayerId;
        return;
    }

    auto* raster = layer != nullptr ? layer->as<core::RasterLayer>() : nullptr;
    if (raster == nullptr) {
        m_brushLayer = core::kInvalidLayerId; // layer vanished: nothing to restore/commit
        return;
    }

    if (inpaint) {
        // Drop the red overlay (revert the stamped pixels), then hand the brushed region to the
        // host to fill with the inpainting engine. The host's commit recomposites to the result, so
        // the overlay simply gives way to the filled pixels.
        const common::Rect db = m_brushEngine.dirtyBounds();
        m_brushEngine.restore();
        revertBrushGrowth(*raster); // a no-op: the Inpaint brush never grows (growBrushLayer)
        raster->invalidateContentBounds();
        if (!hole.isEmpty() && m_brushHost.commitInpaint)
            m_brushHost.commitInpaint(m_brushLayer, std::move(hole));
        else if (m_brushHost.previewChanged)
            // Nothing fillable brushed: recomposite just the overlay's region to clear it. The
            // Inpaint brush never grows the layer (revertBrushGrowth above is a no-op for it), so
            // `db` is still in this layer's own grid and can be claimed as the layer-local rect.
            m_brushHost.previewChanged(localRectToDocBBox(db, core::worldTransform(*layer)),
                                       raster->id(), db);
        m_brushLayer = core::kInvalidLayerId;
        return;
    }

    // Paint brush: the live preview already painted into raster->image(). Hand only the stroke's
    // bounding box (layer-local) to the host as one undoable edit -- storing the whole layer per
    // stroke was the History memory bloat + undo hiccup on big canvases (S60-a). Read the painted
    // region out, then restore the pre-stroke pixels so the command captures the correct "old"
    // region on apply (it re-applies the painted region -- no flicker; dirtyBounds survives end()).
    const common::Rect db = m_brushEngine.dirtyBounds();
    if (m_brushHost.commitStroke && !db.empty()) {
        // `db` is in the WORKING grid -- which is the pre-press grid shifted by the auto-grow's
        // origin. Everything the command is built from is stated in PRE-PRESS coordinates, so the
        // shift is undone once, here, and never thought about again.
        const long gx = m_brushGrowBox.x0;
        const long gy = m_brushGrowBox.y0;
        const long ox = static_cast<long>(db.x);
        const long oy = static_cast<long>(db.y);
        const auto rw = static_cast<std::uint32_t>(db.w);
        const auto rh = static_cast<std::uint32_t>(db.h);
        common::Image region = common::copyRegion(raster->image(), ox, oy, rw, rh);
        m_brushEngine.restore();
        // The growth that SURVIVES: the pre-press layer united with what the stroke actually
        // touched, through the same guarded function the press used -- so a stroke that stayed
        // inside the layer leaves the layer exactly the size it was, and a `requested` box that is
        // somehow absurd is intersected away before it sizes anything (core/layer_grow.hpp).
        const core::PixelBox touched{ox + gx, oy + gy, ox + gx + static_cast<long>(rw),
                                     oy + gy + static_cast<long>(rh)};
        const core::PixelBox keep = core::brushGrowthBox(m_brushGrowOrigW, m_brushGrowOrigH,
                                                         touched, m_brushCanvasBox);
        revertBrushGrowth(*raster); // the working grid leaves no trace; the command owns the growth
        raster->invalidateContentBounds();
        if (keep == core::layerPixelBox(m_brushGrowOrigW, m_brushGrowOrigH) ||
            !m_brushHost.commitGrownStroke) {
            // Nothing grew (or there is nowhere to land growth): the ordinary region patch, in
            // pre-press coordinates -- byte-for-byte the commit this has always been.
            m_brushHost.commitStroke(m_brushLayer, std::move(region), ox + gx, oy + gy);
        } else {
            // Growth + paint as ONE undo step. The region's origin moves into the NEW grid, whose
            // own origin is `keep`'s: a point at pre-press p lands at p - keep.origin.
            m_brushHost.commitGrownStroke(
                m_brushLayer, static_cast<std::uint32_t>(keep.width()),
                static_cast<std::uint32_t>(keep.height()), -keep.x0, -keep.y0, std::move(region),
                ox + gx - keep.x0, oy + gy - keep.y0);
        }
    } else { // nothing painted: just revert, no edit
        m_brushEngine.restore();
        revertBrushGrowth(*raster);
        raster->invalidateContentBounds();
    }
    m_brushLayer = core::kInvalidLayerId;
}

void VulkanCanvas::cancelBrushStroke() {
    m_brushPressPending = false; // an owed first dab is owed no longer
    (void)m_smoother.flush();    // ... and so is the smoother's tail: this stroke is being discarded
    if (!m_brushStroking)
        return;
    m_brushStroking = false;
    m_brushEngine.end();
    clearCloneStrokeState(); // S38: drop the abandoned stroke's snapshots (the source survives)
    core::Document* doc = m_brushHost.document ? m_brushHost.document() : nullptr;
    core::Layer* layer = doc != nullptr ? doc->find(m_brushLayer) : nullptr;
    if (m_brushMaskLane) { // the S31 mask lane: discard the in-flight coverage
        m_brushMaskLane = false;
        if (core::RasterMask* mask = layer != nullptr ? layer->mask() : nullptr;
            mask != nullptr && m_maskProxy.width == mask->width &&
            m_maskProxy.height == mask->height) {
            const common::Rect db = m_brushEngine.dirtyBounds();
            m_brushEngine.restore();
            mirrorMaskProxy(*layer, db);
            if (!db.empty() && m_brushHost.previewChanged)
                m_brushHost.previewChanged(localRectToDocBBox(db, m_maskToDoc),
                                           core::kInvalidLayerId, common::Rect{});
        }
        m_maskProxy = common::Image{};
        m_brushLayer = core::kInvalidLayerId;
        return;
    }
    if (auto* raster = layer != nullptr ? layer->as<core::RasterLayer>() : nullptr;
        raster != nullptr) {
        const common::Rect db = m_brushEngine.dirtyBounds();
        // The preview box is read in the WORKING grid, so its document mapping has to be taken
        // while that grid (and the placement that goes with it) is still the layer's -- reverting
        // the growth first would map a rect through the wrong transform.
        const common::Rect preview = localRectToDocBBox(db, core::worldTransform(*raster));
        m_brushEngine.restore(); // discard the in-flight stroke
        revertBrushGrowth(*raster); // ... and the press-time growth with it
        raster->invalidateContentBounds();
        if (!db.empty() && m_brushHost.previewChanged)
            // Recomposite just the stroke's region to clear the in-flight preview. NO layer-local
            // claim: revertBrushGrowth has already put the image back on its pre-press grid, so
            // `db` -- read in the working grid -- names different pixels now. A whole-layer claim
            // is the only one that stays true, and it is what an empty rect asks for.
            m_brushHost.previewChanged(preview, raster->id(), common::Rect{});
    }
    m_brushLayer = core::kInvalidLayerId;
}

// ---- S38 Stamp / Clone (docs/clone-stamp.md §5) ----------------------------------------------
//
// The clone stamp is the BRUSH LANE with a different deposit. Everything above -- the press/drag
// dispatch, the tablet drain, the smoother, the engine, the auto-grow opt-out, the reticle, the
// single SetLayerPixelsCommand on release -- serves it unchanged; the only clone-specific steps are
// (a) picking a source, (b) resolving the offset at the press, and (c) rewriting each composited
// batch as source pixels. There is no second dab walk, and there must never be one.

bool VulkanCanvas::cloneAnchorModifier() const {
    // FL_COMMAND is Command on macOS and Ctrl everywhere else -- exactly the "Ctrl (⌘ on macOS)"
    // the tool is specified with, and the same constant the menu bar's accelerators use.
    return cloneToolActive() && (Fl::event_state() & FL_COMMAND) != 0;
}

CloneStampOptions VulkanCanvas::cloneOptions() const {
    CloneStampOptions o;
    if (m_tools == nullptr)
        return o;
    o.size = brushOption("size", o.size);
    o.hardness = brushOption("hardness", o.hardness);
    o.flow = brushOption("flow", o.flow);
    o.opacity = brushOption("opacity", o.opacity);
    o.spacing = brushOption("spacing", o.spacing);
    o.aligned = brushOption("aligned", 1.0) > 0.5;
    o.sample = cloneSampleForChoice(static_cast<int>(brushOption("sample", 0.0)));
    return o;
}

void VulkanCanvas::pushCloneAnchor() {
    // One click, no gesture (the wand's shape): the source is a POINT in document space, so it
    // survives a zoom, a pan and a canvas rotation, and it means the same thing whatever layer is
    // active. Nothing enters the document -- an anchor is tool state, not an edit, so there is no
    // undo step and there must not be one.
    core::setCloneAnchor(m_cloneAnchor, eventDocPoint());
    requestHostFrame(); // the marker should appear on THIS frame, not on the next mouse move
}

bool VulkanCanvas::beginCloneStroke(core::RasterLayer& layer, const core::brush::StrokeInput& in) {
    clearCloneStrokeState();
    const CloneStampOptions o = cloneOptions();
    const std::optional<common::Vec2> offset =
        core::cloneStrokeOffset(m_cloneAnchor, m_view.toDoc(in.pos), o.aligned);
    if (!offset) {
        if (m_cloneHost.noSourceAttempt)
            m_cloneHost.noSourceAttempt(); // name the way in; a silent no-op reads as broken
        return false;
    }
    const common::Affine2D world = core::worldTransform(layer);
    // doc -> the SOURCE image's own pixel grid. For the current-layer mode the source is the layer
    // itself, so that map is the layer's inverse world transform; for the composited modes the
    // snapshot IS a document-space image, so it is the identity. Building both as an affine means
    // the two modes share one sampling loop -- and it is what makes a rotated or scaled layer clone
    // from the document the user is actually looking at rather than from its own skewed grid.
    common::Affine2D docToSource = common::Affine2D::identity();
    if (o.sample == core::CloneSampleSource::CurrentLayer) {
        const std::optional<common::Affine2D> inv = world.inverse();
        if (!inv)
            return false; // singular placement: there is no honest source to read
        docToSource = *inv;
    } else {
        m_cloneBackdrop = m_cloneHost.backdrop
                              ? m_cloneHost.backdrop(o.sample ==
                                                     core::CloneSampleSource::CurrentAndBelow)
                              : common::Image{};
        if (m_cloneBackdrop.empty()) {
            // No snapshot means no source. Refusing is the honest answer: falling back to the
            // active layer would silently clone something the user did not choose.
            if (m_cloneHost.noSourceAttempt)
                m_cloneHost.noSourceAttempt();
            m_cloneBackdrop = common::Image{};
            return false;
        }
        m_cloneFromBackdrop = true;
    }
    // The PRE-STROKE destination. Taken here, before the engine's first composite, and read by
    // every later batch (docs/brushes.md §6.6b): the clone composite must never read a pixel this
    // very stroke has already stamped, or the mark would depend on composite cadence and a slow
    // frame would print a different picture from a fast one.
    m_cloneBase = layer.image();
    m_cloneOffsetDoc = *offset;
    m_cloneTargetToSource =
        docToSource * common::Affine2D::translation(-m_cloneOffsetDoc.x, -m_cloneOffsetDoc.y) *
        world;
    // Resolved ONCE per stroke: at a whole-pixel shift on an untransformed layer -- the ordinary
    // case -- the stamp must MOVE BYTES, not resample them (core/clone_stamp.hpp says why).
    m_cloneBilinear = !core::isWholePixelShift(m_cloneTargetToSource);
    m_cloneStroking = true;
    return true;
}

void VulkanCanvas::stampCloneRegion(const common::Rect& dirty) {
    if (!m_cloneStroking || dirty.empty())
        return;
    core::Document* doc = m_brushHost.document ? m_brushHost.document() : nullptr;
    core::Layer* layer = doc != nullptr ? doc->find(m_brushLayer) : nullptr;
    auto* raster = layer != nullptr ? layer->as<core::RasterLayer>() : nullptr;
    if (raster == nullptr)
        return; // the layer vanished mid-stroke; pumpBrushStroke cancels on the next pump

    core::CloneStampInput in;
    in.target = &raster->image();
    in.base = &m_cloneBase;
    in.source = m_cloneFromBackdrop ? &m_cloneBackdrop : &m_cloneBase;
    in.targetToSource = m_cloneTargetToSource;
    in.bilinear = m_cloneBilinear;
    in.coverage = m_brushEngine.coverage().data();
    in.covX = m_brushEngine.coverageOriginX();
    in.covY = m_brushEngine.coverageOriginY();
    in.covW = m_brushEngine.coverageWidth();
    in.covH = m_brushEngine.coverageHeight();
    // The engine's own ceiling for this stroke: `opacity x colour alpha`, and currentBrushParams
    // pins the clone's colour opaque precisely so the two are the same number.
    in.opacity = std::clamp(brushOption("opacity", 100.0) / 100.0, 0.0, 1.0);
    in.confine = m_cloneConfine.get();
    // Exactly the rect composite() reported, so every pixel the engine wrote is replaced and no
    // pixel it left pristine is touched -- which is what lets BrushEngine::restore() put a
    // cancelled clone stroke back byte for byte.
    core::applyCloneStamp(in, static_cast<int>(std::floor(dirty.x)),
                          static_cast<int>(std::floor(dirty.y)),
                          static_cast<int>(std::ceil(dirty.right())),
                          static_cast<int>(std::ceil(dirty.bottom())));
}

void VulkanCanvas::clearCloneStrokeState() {
    m_cloneStroking = false;
    m_cloneFromBackdrop = false;
    m_cloneBilinear = false;
    m_cloneBase = common::Image{};
    m_cloneBackdrop = common::Image{};
    m_cloneConfine.reset();
}

void VulkanCanvas::clearCloneSource() {
    clearCloneStrokeState();
    m_cloneAnchor = core::CloneAnchorState{};
}

std::optional<common::Vec2> VulkanCanvas::cloneMarkerDocPoint() const {
    // While a stroke runs the marker tracks the LIVE source -- the point the next dab will read --
    // because the whole tool is "this lands over there", and a marker frozen at the anchor would
    // stop telling you that halfway through the first stroke.
    if (m_cloneStroking) {
        const common::Vec2 cursor = m_view.toDoc(m_cursorLogical);
        return common::Vec2{cursor.x - m_cloneOffsetDoc.x, cursor.y - m_cloneOffsetDoc.y};
    }
    if (!m_cloneAnchor.hasAnchor)
        return std::nullopt;
    return m_cloneAnchor.anchor;
}

void VulkanCanvas::syncCloneOverlay() {
    if (!m_renderer || !cloneToolActive())
        return;
    // The overlay-line channel: a lasso/shape/pen gesture can never be live while the clone stamp
    // is the active tool, and syncLassoOverlay has just cleared it, so we own it here -- exactly
    // the arrangement syncShapeOverlay / syncPenOverlay / syncTextOverlay already use.
    const std::optional<common::Vec2> at = cloneMarkerDocPoint();
    if (!at)
        return; // no source picked yet: the cleared channel is the honest picture
    m_renderer->setLassoPolyline(cloneMarkerPolyline(m_view.toScreen(*at), kCloneMarkerRadius));
}

// The reticle's tip-outline field crosses from the core (which builds it) to the renderer (which
// uploads it) through this file, and this is the one place that sees both capacities. A core that can
// build a field the renderer would refuse is a reticle that silently stops tracing.
static_assert(core::brush::kTipSdfMaxCells <= render::kTipSdfMaxCells,
              "the present pass's tip-SDF storage must hold whatever buildTipSdf can produce");

void VulkanCanvas::syncBrushReticle(bool inside) {
    if (!m_renderer)
        return;
    // S18 select brush + L1 edge brush: the SAME size ring as the paint brush, sized to the active
    // tool's own tip diameter -- a plain round analytic ellipse, no tip SDF
    // (docs/research-select-brush.md §3.1). The ring is fine to show during an L1 drag: it is a
    // brush-size hint, not the grown selection, so it does not breach the solve-on-release rule.
    // Handled up front so the paint-brush path below stays byte-identical (it reads
    // currentBrushParams, which neither select-family brush is a client of).
    // The S38-b eye tool joins them: its ring is not merely a size hint, it IS the scope -- what
    // the ring covers is what a click corrects, which is the affordance the whole "no detector,
    // the user says where" rule rests on (docs/red-eye-tool.md §4).
    if (selectBrushToolActive() || edgeBrushToolActive() || redEyeToolActive()) {
        const bool showRing = inside && !m_panning && !m_spaceDown && !m_rotateDown;
        if (!showRing) {
            if (m_reticleVisible) {
                m_renderer->setBrushReticle(false, {}, 0.0, 0.0, 0.0, false);
                m_reticleVisible = false;
            }
            return;
        }
        const double diameter = std::max(0.1, brushOption("size", 24.0));
        const ReticleShape shape =
            reticleShape(diameter, 1.0, 0.0, m_view.zoom(), m_view.rotation());
        m_renderer->setBrushReticleTracing(false); // a plain ring: never trace a stale tip SDF
        m_renderer->setBrushReticle(true, m_cursorLogical, shape.semiX, shape.semiY, shape.angleRad,
                                    false);
        m_reticleVisible = true;
        return;
    }
    // Show the ring while the brush tool is live over the canvas (including during a stroke), but
    // not while a pan/rotate gesture has taken over the pointer -- and not while Ctrl holds the
    // temporary eyedropper, whose loupe replaces the ring for exactly as long as the key is down.
    // ... and not while the clone stamp's source-pick modifier is held: the next click picks a
    // POINT, so the crosshair (updateToolCursor) replaces the ring for exactly as long as the key
    // is down -- the same swap the temporary eyedropper makes.
    const bool show = strokeToolActive() && !temporaryEyedropperActive() && !cloneAnchorModifier() &&
                      inside && !m_panning && !m_spaceDown && !m_rotateDown;
    if (!show) {
        if (m_reticleVisible) {
            m_renderer->setBrushReticle(false, {}, 0.0, 0.0, 0.0, false);
            m_reticleVisible = false;
        }
        return;
    }
    // The ring traces THE TIP THE STROKE WILL LAY, so it reads the tip's shape out of the very
    // params the engine is handed -- diameter, ratio and angle -- rather than re-deriving a radius
    // from the size option. The two cannot then disagree (docs/brushes.md §6.3).
    const core::brush::BrushParams params = currentBrushParams();
    const core::brush::BrushTip* tip = params.tip.get();

    // §6.3: an animated (`.gih`) tip draws FRAME 0's silhouette. 27 of the 31 shipped hose tips pick
    // their cell at random, so "the next dab's mask" is not knowable before the dab happens -- and a
    // reticle that flickered through a different frame on every motion event would be unusable.
    constexpr int kReticleFrame = 0;

    // THE ANGLE THE NEXT DAB WILL TAKE, not the tip's authored slant. 14 of the 82 shipped presets
    // turn their nib to follow the stroke, and the ring used to ignore that entirely -- the bug the
    // user reported. During a stroke the heading is the engine's own (so the ring and the dabs cannot
    // disagree); on hover it is the pointer's recent travel.
    //
    // The pen sample carries tilt and barrel rotation, which four more presets steer by. A MOUSE has
    // neither, so it is handed the rest values rather than a stale sample from the last time a stylus
    // was in proximity -- `lastSample()` remembers, and a ring leaning because of a pen that was put
    // down ten minutes ago is a ghost.
    const double heading = m_brushEngine.strokeState().active()
                               ? m_brushEngine.strokeState().drawingAngle()
                               : m_hoverHeading.headingRad();
    const core::brush::StrokeInput pen =
        m_tablet.stylusInProximity() ? m_tablet.lastSample() : core::brush::StrokeInput{};
    // No option table at all is the pre-preset brush: it lays the authored angle and nothing turns it.
    const double angleRad =
        params.options ? core::brush::reticleDabAngle(*params.options, params.angleRad, pen, heading)
                       : params.angleRad;

    // The ring's screen FRAME. A real tip is measured by its TRUE extent rather than by
    // `diameter x diameter*ratio`: a bitmap tip's `diameter` sets its LONG axis and its frame's own
    // aspect fills in the rest (core::brush::tipDabShape), so a 300x80 stamp paints a dab nearly four
    // times wider than it is tall whatever `ratio` says -- and the diameter-and-ratio form would ring
    // it with a box that tall.
    //
    // ⚠ A NULL tip keeps the diameter-and-ratio form, which is the very function it has always taken.
    // It IS an ellipse of exactly that box -- the engine's built-in analytic circle -- and every
    // golden in the suite was laid through here.
    const ReticleShape shape = [&] {
        if (tip == nullptr)
            return reticleShape(params.diameter, params.ratio, angleRad, m_view.zoom(),
                                m_view.rotation());
        const core::brush::DabShape s =
            core::brush::tipDabShape(*tip, kReticleFrame, params.diameter, params.ratio,
                                     /*angleRad=*/0.0, /*mirrorH=*/false, /*mirrorV=*/false);
        return reticleShapeFromExtent(s.width, s.height, angleRad, m_view.zoom(),
                                      m_view.rotation());
    }();

    // Does the ring TRACE the tip, or is the analytic ellipse already the truth about it? An ellipse
    // over a bristle tip is a lie, and it is exactly the lie the user complained about -- but an
    // ellipse over a plain round generator is that tip's own outline, to the pixel, and a NULL tip's
    // ring must stay bit-for-bit the one every golden in the suite was laid by.
    //
    // The field is rebuilt only when the tip's RASTER, its frame or its RATIO changes -- and on
    // nothing else. It lives in the tip's own frame, so the diameter, the zoom, the view's rotation
    // and the cursor's position are all applied when the shader samples it; rebuilding it here per
    // motion event would run a distance transform on every pixel of pointer travel. (The ratio is in
    // there because a SPIKED generator's star exists only when the tip is squashed -- see
    // core/brush/tip_outline.hpp. It is a configured constant, so it changes when the preset does.)
    //
    // Note what is deliberately NOT in it either: the on-screen SIZE. A brush zoomed down to a few
    // pixels stops tracing -- below -- but that is a per-FRAME decision about what to DRAW, not a
    // different field, and folding it in would make a zoom that crosses the threshold rebuild the tip.
    const bool wantSdf = tip != nullptr && core::brush::tipNeedsSdf(tip);
    const std::uint64_t wantTip = wantSdf ? tip->id : 0;
    const double wantRatio = (wantSdf && std::isfinite(params.ratio) && params.ratio > 0.0)
                                 ? params.ratio
                                 : 1.0; // sanitized: a NaN would never compare equal to itself
    if (wantTip != m_reticleSdfTip || wantRatio != m_reticleSdfRatio) {
        m_reticleSdfTip = wantTip;
        m_reticleSdfRatio = wantRatio;
        if (wantTip == 0) {
            m_renderer->setBrushReticleSdf(0, 0, 0, 0, 0.0, 0.0, nullptr, 0);
        } else {
            const core::brush::TipSdf sdf =
                core::brush::buildTipSdf(*tip, kReticleFrame, wantRatio);
            // A fresh key per BUILD, not per tip: the ratio can change under a tip whose id has not,
            // and the renderer decides whether to re-upload by the key alone. An empty field (a tip
            // that paints nothing) is pushed as key 0 -- the renderer keeps its ellipse -- but the
            // build is still REMEMBERED, so it is not retried on the next mouse move.
            m_renderer->setBrushReticleSdf(sdf.empty() ? 0 : ++m_reticleSdfGen, sdf.w, sdf.h,
                                           sdf.pad, sdf.boxW, sdf.boxH, sdf.d.data(),
                                           sdf.d.size());
        }
    }
    // ... and the per-frame half: below a couple of pixels on screen a traced contour degenerates
    // into a blob, which says less about the tip than a plain ring does. The field stays uploaded;
    // the shader just stops reading it.
    m_renderer->setBrushReticleTracing(wantTip != 0 && reticleTracesTip(shape));

    // While a stroke is in flight the target is unlocked (a locked layer never arms one), so only
    // show the padlock on hover; this also keeps the glyph from flashing during a legitimate
    // stroke.
    const bool locked = !m_brushStroking && activeBrushLayerLocked();
    m_renderer->setBrushReticle(true, m_cursorLogical, shape.semiX, shape.semiY, shape.angleRad,
                                locked);
    m_reticleVisible = true;
}

// ---- S26 Shape tool -------------------------------------------------------------------------
//
// The canvas owns the pointer half: a press->current drag (document space) feeds the pure
// ui::buildShapeDraft, which yields the vec::Object a fresh VectorLayer will carry + its placement.
// While the drag runs the shape is drawn as a WIREFRAME OUTLINE on the overlay (the lasso polyline
// channel) and the document is not touched at all; on release the host spawns the real, FILLED
// layer from the final draft and lands it as one undoable command. The document half stays with
// the host. (Before S26-c the live layer itself was the preview -- an Affinity-style figure that
// recomposited the whole document on every drag frame.)

bool VulkanCanvas::shapeToolActive() const {
    return m_tools != nullptr && shapeKindFor(m_tools->active()).has_value();
}

ShapeOptions VulkanCanvas::activeShapeOptions() const {
    ShapeOptions o;
    const Tool* tool = m_tools != nullptr ? m_tools->activeTool() : nullptr;
    if (tool == nullptr)
        return o;
    if (m_shapeHost.foreground)
        o.foreground = m_shapeHost.foreground(); // the fill colour (a line's stroke); the only one
    const auto opt = [&](const char* id, double def) {
        for (const ToolOption& v : tool->options())
            if (v.id == id)
                return v.value;
        return def;
    };
    o.cornerRadius = opt("radius", 0.0);
    o.sides = static_cast<int>(opt("sides", 5.0));
    o.points = static_cast<int>(opt("points", 5.0));
    o.innerRatio = opt("inner", 50.0) / 100.0;
    if (tool->id() == ToolId::LineShape) { // the one stroke the bar still owns (S26-c)
        o.lineWidth = opt("weight", 3.0);
        const double cap = opt("cap", 0.0); // 0 Butt, 1 Round, 2 Square
        o.cap = cap < 0.5 ? core::vec::LineCap::Butt
                : cap < 1.5 ? core::vec::LineCap::Round
                            : core::vec::LineCap::Square;
    }
    return o;
}

std::optional<ShapeDraft> VulkanCanvas::currentShapeDraft() const {
    if (m_tools == nullptr || !m_shapeDragMoved)
        return std::nullopt; // a bare press describes no box yet
    const std::optional<ShapeKind> kind = shapeKindFor(m_tools->active());
    if (!kind)
        return std::nullopt;
    return buildShapeDraft(*kind, m_shapePressDoc, m_shapeDragDoc, m_shapeDragShift, m_shapeDragAlt,
                           activeShapeOptions());
}

void VulkanCanvas::captureShapeDrag() {
    m_shapeDragDoc = eventDocPoint();
    const auto state = Fl::event_state();
    m_shapeDragShift = (state & FL_SHIFT) != 0;
    m_shapeDragAlt = (state & FL_ALT) != 0;
    m_shapeDragMoved = true;
}

std::vector<common::Vec2> VulkanCanvas::shapeOutlineScreenPolyline() const {
    if (!m_shapeDragging)
        return {};
    const std::optional<ShapeDraft> draft = currentShapeDraft();
    if (!draft)
        return {}; // sub-pixel so far: nothing would be authored, so show nothing
    // Flatten against the local -> PHYSICAL px transform, so the curve tolerance tracks the zoom
    // (and HiDPI) exactly like the vector rasteriser's does -- a wireframe that is visibly coarser
    // than the fill it promises would be a lie about the shape.
    const common::Affine2D docToDevice =
        common::Affine2D::scaling(m_contentScale, m_contentScale) * m_view.docToScreen();
    std::vector<common::Vec2> pts = shapeOutlinePolyline(*draft, docToDevice);
    if (pts.size() > render::kLassoMaxVerts) {
        // Over the overlay SSBO's capacity (a huge shape at a deep zoom): drop to a fixed stride,
        // always keeping the last vertex so a closed silhouette still closes. Same treatment the
        // long freehand lasso path gets, for the same reason.
        const std::size_t n = pts.size();
        const std::size_t stride = (n + render::kLassoMaxVerts - 2) / (render::kLassoMaxVerts - 1);
        std::vector<common::Vec2> thinned;
        thinned.reserve(render::kLassoMaxVerts);
        for (std::size_t i = 0; i < n; i += stride)
            thinned.push_back(pts[i]);
        if ((n - 1) % stride != 0)
            thinned.push_back(pts[n - 1]);
        pts = std::move(thinned);
    }
    for (common::Vec2& p : pts) // document px -> logical screen px (the renderer scales to physical)
        p = m_view.toScreen(p);
    return pts;
}

void VulkanCanvas::syncShapeOverlay() {
    // The wireframe rides the lasso polyline channel: a shape tool and a lasso tool can never both
    // be active, and syncLassoOverlay has just cleared the channel, so we own it here -- exactly the
    // arrangement the Type tool's Area frame uses (syncTextOverlay).
    if (!m_renderer || !shapeToolActive())
        return;
    m_renderer->setLassoPolyline(shapeOutlineScreenPolyline());
}

void VulkanCanvas::pushShapeTool() {
    // Resize-vs-transform (§7.1): with a shape already selected, a press on its selection-box
    // controls (a handle, the body, or the rotate band) starts a resize / move / rotate -- NOT a
    // new shape and NOT a re-pick.
    if (shapeEditActive()) {
        const common::Vec2 screenPt = eventLogicalPoint();
        if (editTargetIsLine()) { // a line uses its own gizmo (endpoints / bend / move), not the box
            const int h = hitLineGizmo(screenPt);
            if (h >= 0) {
                beginLineGizmoGesture(h, eventDocPoint());
                return;
            }
        } else {
            std::array<common::Vec2, 4> corners{};
            if (shapeBoxCorners(corners))
                if (const std::optional<TransformHit> hit =
                        hitTransformControls(screenPt, corners, kHandleHitPx, kRotateBandPx)) {
                    pushShapeBoxGesture(*hit, eventDocPoint());
                    return;
                }
        }
    }
    // Select-to-edit (§7.1): a press on an existing vector shape binds it to the options bar and
    // does NOT start a new shape. A press on empty space clears the edit target and authors anew.
    if (pickShapeForEdit())
        return;
    m_shapeEditTarget = core::kInvalidLayerId;
    m_shapePressDoc = eventDocPoint();
    m_shapeDragDoc = m_shapePressDoc;
    m_shapeDragShift = false;
    m_shapeDragAlt = false;
    m_shapeDragMoved = false; // no box yet -- the wireframe appears with the first drag frame
    m_shapeDragging = true;
}

bool VulkanCanvas::pickShapeForEdit() {
    if (!m_shapeHost.document)
        return false;
    core::Document* doc = m_shapeHost.document();
    if (doc == nullptr)
        return false;
    const double pickDoc = 4.0 / std::max(1e-6, m_view.zoom()); // a few device px of outline slack
    core::VectorLayer* vl = core::topmostVectorLayerAt(doc->root(), eventDocPoint(), pickDoc);
    if (vl == nullptr || !vl->hasObject())
        return false;
    if (!shapeToolBinds(*vl->object()))
        return false; // a GRADIENT layer belongs to the Gradient tool, not this bar (S22)
    const std::optional<ShapeKind> kind = shapeKindOf(*vl->object());
    if (!kind)
        return false; // an editable Path is the Pen tool's job (S28), not the Shape tool
    m_shapeEditTarget = vl->id();
    ++m_shapeEditCoalesce; // a fresh edit session = a new undo step
    if (const std::optional<ToolId> slot = toolIdForKind(*kind); slot && m_tools != nullptr) {
        m_selectingShapeForEdit = true; // keep onToolChanged from clearing the target we just set
        m_tools->setActive(*slot);
        m_selectingShapeForEdit = false;
    }
    reflectShapeOptions(*vl->object());
    requestHostFrame();
    return true;
}

void VulkanCanvas::reflectShapeOptions(const core::vec::Object& obj) {
    Tool* tool = m_tools != nullptr ? m_tools->activeTool() : nullptr;
    if (tool == nullptr)
        return;
    ShapeOptions ro; // colours stay as the swatch has them; readShapeOptions fills the rest
    readShapeOptions(obj, ro);
    const auto setOpt = [&](const char* id, double v) {
        for (ToolOption& o : tool->options())
            if (o.id == id)
                o.value = v;
    };
    setOpt("radius", ro.cornerRadius);
    setOpt("sides", static_cast<double>(ro.sides));
    setOpt("points", static_cast<double>(ro.points));
    setOpt("inner", ro.innerRatio * 100.0);                          // option is a percent
    setOpt("weight", ro.lineWidth);                                  // the line's own stroke (S26-c)
    setOpt("cap", static_cast<double>(static_cast<int>(ro.cap)));    // 0 Butt / 1 Round / 2 Square
    m_reflectingShapeOptions = true; // the resulting sync is our own write, not a user edit
    m_tools->notifyOptionsChanged();
    m_reflectingShapeOptions = false;
}

// ---- Resize-vs-transform: the selected shape's selection box + handles (§7.1) ------------------

bool VulkanCanvas::shapeEditActive() const {
    return shapeToolActive() && m_shapeEditTarget != core::kInvalidLayerId;
}

bool VulkanCanvas::shapeTransformMode() const {
    const Tool* tool = m_tools != nullptr ? m_tools->activeTool() : nullptr;
    if (tool == nullptr)
        return false;
    for (const ToolOption& o : tool->options())
        if (o.id == "transform")
            return o.value != 0.0;
    return false;
}

// The edit shape's box, framed by its world transform and tight to its contentBounds (the same box
// the Move tool draws for a vector layer), mapped to logical screen px. Reads the LIVE layer, so it
// tracks the shape through a resize / transform drag and the view through a pan/zoom.
bool VulkanCanvas::shapeBoxCorners(std::array<common::Vec2, 4>& out) const {
    if (!shapeEditActive() || !m_shapeHost.document)
        return false;
    core::Document* doc = m_shapeHost.document();
    core::Layer* layer = doc != nullptr ? doc->find(m_shapeEditTarget) : nullptr;
    auto* vl = layer != nullptr ? layer->as<core::VectorLayer>() : nullptr;
    if (vl == nullptr || !vl->hasObject())
        return false;
    const std::optional<common::Rect> content = vl->contentBounds();
    if (!content || content->empty())
        return false;
    const std::array<common::Vec2, 4> doc4 = framedCorners(core::worldTransform(*layer), *content);
    for (std::size_t i = 0; i < 4; ++i)
        out[i] = m_view.toScreen(doc4[i]);
    return true;
}

void VulkanCanvas::pushShapeBoxGesture(const TransformHit& hit, common::Vec2 docPt) {
    core::Document* doc = m_shapeHost.document ? m_shapeHost.document() : nullptr;
    core::Layer* layer = doc != nullptr ? doc->find(m_shapeEditTarget) : nullptr;
    auto* vl = layer != nullptr ? layer->as<core::VectorLayer>() : nullptr;
    if (vl == nullptr || !vl->hasObject())
        return;
    const std::optional<common::Rect> content = vl->contentBounds();
    if (!content || content->empty())
        return;
    m_shapeBoxBase = core::worldTransform(*layer);
    m_shapeBoxBaseLayer = layer->transform();
    const std::optional<common::Affine2D> parentInv = core::parentWorldTransform(*layer).inverse();
    if (!parentInv)
        return; // a singular ancestor: nothing sane to edit through it
    m_shapeBoxParentInv = *parentInv;
    m_shapeBoxContent = *content;
    m_shapeBoxBaseObject = *vl->object();
    m_shapeBoxHandle = hit.handle;
    m_shapeBoxMode = hit.mode;
    // Rotate has no parameter, so it always transforms; the body always rigid-moves; a handle Scale
    // resizes the params by default, or transform-scales the layer when the "Transform" toggle is on.
    m_shapeBoxResize = hit.mode == TransformMode::Scale && !shapeTransformMode();
    if (!m_shapeBox.begin(hit.mode, hit.handle, docPt, m_shapeBoxBase, m_shapeBoxContent))
        return;
    ++m_shapeEditCoalesce; // this gesture is its own undo step, distinct from prior options edits
    m_shapeBoxPushed = false;
    m_shapeBoxLatched = false;
    m_shapeBoxPressScreen = eventLogicalPoint();
}

void VulkanCanvas::dragShapeBox() {
    if (!m_shapeBox.active() && m_lineHandle < 0) // a box gesture OR a line-gizmo gesture
        return;
    if (!m_shapeBoxLatched) { // a click (with its jitter) must not push a zero-delta undo step
        const common::Vec2 p = eventLogicalPoint();
        if ((p - m_shapeBoxPressScreen).length() < kMoveDragDeadZonePx)
            return;
        m_shapeBoxLatched = true;
    }
    const auto state = Fl::event_state();
    m_shapeBoxDocPt = eventDocPoint();
    m_shapeBoxShift = (state & FL_SHIFT) != 0;
    m_shapeBoxAlt = (state & FL_ALT) != 0;
    m_shapeBoxPending = true;
    requestHostFrame(); // land the recorded drag this frame (flushShapeBoxDrag), not on the heartbeat
}

void VulkanCanvas::flushShapeBoxDrag() {
    if (!m_shapeBoxPending)
        return;
    m_shapeBoxPending = false;
    if (m_lineHandle >= 0) { // a line-gizmo drag (its own, simpler path)
        flushLineGizmoDrag();
        return;
    }
    if (!m_shapeBox.active())
        return;
    core::Document* doc = m_shapeHost.document ? m_shapeHost.document() : nullptr;
    core::Layer* layer = doc != nullptr ? doc->find(m_shapeEditTarget) : nullptr;
    auto* vl = layer != nullptr ? layer->as<core::VectorLayer>() : nullptr;
    if (vl == nullptr || !vl->hasObject()) { // undone / deleted beneath us: abandon quietly
        m_shapeBox.cancel();
        return;
    }
    if (m_shapeBoxResize) {
        const std::optional<ShapeResize> r =
            resizeShape(m_shapeBoxBaseObject, m_shapeBoxBase, m_shapeBoxHandle, m_shapeBoxDocPt,
                        m_shapeBoxShift, m_shapeBoxAlt);
        if (r && m_shapeHost.editShape) {
            m_shapeBoxPushed = true;
            m_shapeHost.editShape(m_shapeEditTarget, r->object,
                                  m_shapeBoxParentInv * r->placement, m_shapeEditCoalesce);
        }
    } else if (m_shapeHost.transformShape) { // Move / Rotate / transform-scale: write the transform
        const common::Affine2D world =
            m_shapeBox.transformFor(m_shapeBoxDocPt, m_shapeBoxShift, m_shapeBoxAlt);
        m_shapeBoxPushed = true;
        m_shapeHost.transformShape(m_shapeEditTarget, m_shapeBoxParentInv * world,
                                   m_shapeEditCoalesce);
    }
    updateToolCursor(m_pointerInside); // keep the rotate cursor reoriented as the box turns
}

void VulkanCanvas::endShapeBoxGesture() {
    if (m_lineHandle >= 0) { // a line-gizmo gesture commits (the last drag stands)
        flushShapeBoxDrag();
        m_lineHandle = -1;
        ++m_shapeEditCoalesce;
        if (m_shapeBoxPushed)
            requestHostFrame();
        m_shapeBoxPushed = false;
        return;
    }
    if (!m_shapeBox.active())
        return;
    flushShapeBoxDrag(); // the last recorded cursor position stands as the commit
    m_shapeBox.cancel();
    m_shapeBoxMode = TransformMode::None;
    m_shapeBoxHandle = -1;
    ++m_shapeEditCoalesce; // the next edit (options bar / another gesture) is its own step
    if (m_shapeBoxPushed)
        requestHostFrame();
    m_shapeBoxPushed = false;
}

void VulkanCanvas::cancelShapeBoxGesture() {
    if (m_lineHandle >= 0) { // a line-gizmo gesture: restore the press-time object/transform
        m_shapeBoxPending = false;
        if (m_shapeBoxPushed && m_shapeHost.editShape)
            m_shapeHost.editShape(m_shapeEditTarget, m_shapeBoxBaseObject, m_shapeBoxBaseLayer,
                                  m_shapeEditCoalesce);
        m_lineHandle = -1;
        ++m_shapeEditCoalesce;
        m_shapeBoxPushed = false;
        requestHostFrame();
        return;
    }
    if (!m_shapeBox.active())
        return;
    m_shapeBoxPending = false;
    if (m_shapeBoxPushed) { // restore the press-time state as a final coalesced edit (no junk undo)
        if (m_shapeBoxResize && m_shapeHost.editShape)
            m_shapeHost.editShape(m_shapeEditTarget, m_shapeBoxBaseObject, m_shapeBoxBaseLayer,
                                  m_shapeEditCoalesce);
        else if (m_shapeHost.transformShape)
            m_shapeHost.transformShape(m_shapeEditTarget, m_shapeBoxBaseLayer, m_shapeEditCoalesce);
    }
    m_shapeBox.cancel();
    m_shapeBoxMode = TransformMode::None;
    m_shapeBoxHandle = -1;
    ++m_shapeEditCoalesce;
    m_shapeBoxPushed = false;
    requestHostFrame();
}

// The hover cursor over the selected shape's box controls (mirrors moveCursorState): a handle picks
// the stock resize cursor best matching its screen direction, the body the move cursor, the band rotate.
int VulkanCanvas::shapeBoxCursorState() const {
    if (m_lineHandle >= 0)
        return 10; // dragging a line gizmo control: the move cursor
    if (editTargetIsLine()) {
        // hover test: the tracked pointer, never the live event (updateToolCursor)
        return hitLineGizmo(m_cursorLogical) >= 0 ? 10 : -1; // a gizmo handle / connector
    }
    if (m_shapeBox.active()) {
        switch (m_shapeBox.mode()) {
        case TransformMode::Move:
            return 10;
        case TransformMode::Rotate:
            return 15;
        default:
            return m_cursorState >= 10 ? m_cursorState : 13;
        }
    }
    std::array<common::Vec2, 4> corners{};
    if (!shapeBoxCorners(corners))
        return -1;
    const common::Vec2 p = m_cursorLogical; // hover test: the tracked pointer (updateToolCursor)
    const std::optional<TransformHit> hit =
        hitTransformControls(p, corners, kHandleHitPx, kRotateBandPx);
    if (!hit)
        return -1;
    if (hit->mode == TransformMode::Move)
        return 10;
    if (hit->mode == TransformMode::Rotate)
        return 15;
    return resizeCursorFor(corners, hit->handle);
}

// ---- Line gizmo (S26): a connector + 2 square end handles + 1 round bend handle ----------------

namespace {
// The edit target's line object + layer (null if the edit target isn't a Line shape).
const core::vec::LineShape* asEditLine(core::Document* doc, core::LayerId id, core::Layer*& layerOut) {
    layerOut = doc != nullptr ? doc->find(id) : nullptr;
    auto* vl = layerOut != nullptr ? layerOut->as<core::VectorLayer>() : nullptr;
    if (vl == nullptr || !vl->hasObject())
        return nullptr;
    const auto* ps = std::get_if<core::vec::ParametricShape>(&vl->object()->geometry);
    return ps != nullptr ? std::get_if<core::vec::LineShape>(ps) : nullptr;
}
// Distance from p to the segment ab (screen px).
double distToSegment(common::Vec2 p, common::Vec2 a, common::Vec2 b) {
    const common::Vec2 ab = b - a;
    const double len2 = ab.x * ab.x + ab.y * ab.y;
    const double t = len2 > 1e-9 ? std::clamp(((p - a).x * ab.x + (p - a).y * ab.y) / len2, 0.0, 1.0)
                                 : 0.0;
    return (p - (a + ab * t)).length();
}
} // namespace

bool VulkanCanvas::editTargetIsLine() const {
    if (!shapeEditActive() || !m_shapeHost.document)
        return false;
    core::Layer* layer = nullptr;
    return asEditLine(m_shapeHost.document(), m_shapeEditTarget, layer) != nullptr;
}

bool VulkanCanvas::lineGizmoPoints(common::Vec2& a, common::Vec2& b, common::Vec2& mid) const {
    if (!m_shapeHost.document)
        return false;
    core::Layer* layer = nullptr;
    const core::vec::LineShape* line =
        asEditLine(m_shapeHost.document(), m_shapeEditTarget, layer);
    if (line == nullptr)
        return false;
    const common::Affine2D world = core::worldTransform(*layer);
    const common::Vec2 midLocal = (line->a + line->b) * 0.5 + line->bend;
    a = m_view.toScreen(world.apply(line->a));
    b = m_view.toScreen(world.apply(line->b));
    mid = m_view.toScreen(world.apply(midLocal));
    return true;
}

int VulkanCanvas::hitLineGizmo(common::Vec2 p) const {
    common::Vec2 a{}, b{}, mid{};
    if (!lineGizmoPoints(a, b, mid))
        return -1;
    if ((p - a).length() <= kHandleHitPx)
        return 0;
    if ((p - b).length() <= kHandleHitPx)
        return 1;
    if ((p - mid).length() <= kHandleHitPx)
        return 2;
    if (distToSegment(p, a, b) <= kHandleHitPx)
        return 3; // the connector line (body) -> move
    return -1;
}

void VulkanCanvas::beginLineGizmoGesture(int handle, common::Vec2 docPt) {
    core::Document* doc = m_shapeHost.document ? m_shapeHost.document() : nullptr;
    core::Layer* layer = doc != nullptr ? doc->find(m_shapeEditTarget) : nullptr;
    auto* vl = layer != nullptr ? layer->as<core::VectorLayer>() : nullptr;
    if (vl == nullptr || !vl->hasObject())
        return;
    const std::optional<common::Affine2D> parentInv =
        core::parentWorldTransform(*layer).inverse();
    if (!parentInv)
        return;
    m_shapeBoxBase = core::worldTransform(*layer);
    m_shapeBoxBaseLayer = layer->transform();
    m_shapeBoxParentInv = *parentInv;
    m_shapeBoxBaseObject = *vl->object();
    m_lineHandle = handle;
    m_linePressDoc = docPt;
    ++m_shapeEditCoalesce; // this gesture is its own undo step
    m_shapeBoxPushed = false;
    m_shapeBoxLatched = false;
    m_shapeBoxPressScreen = eventLogicalPoint();
}

void VulkanCanvas::flushLineGizmoDrag() {
    core::Document* doc = m_shapeHost.document ? m_shapeHost.document() : nullptr;
    core::Layer* layer = doc != nullptr ? doc->find(m_shapeEditTarget) : nullptr;
    auto* vl = layer != nullptr ? layer->as<core::VectorLayer>() : nullptr;
    if (vl == nullptr || !vl->hasObject()) {
        m_lineHandle = -1;
        return;
    }
    if (m_lineHandle == 3) { // body: translate the layer by the drag delta (in the parent's space)
        const common::Vec2 deltaParent =
            m_shapeBoxParentInv.applyVector(m_shapeBoxDocPt - m_linePressDoc);
        const common::Affine2D newLayer =
            common::Affine2D::translation(deltaParent.x, deltaParent.y) * m_shapeBoxBaseLayer;
        if (m_shapeHost.transformShape) {
            m_shapeBoxPushed = true;
            m_shapeHost.transformShape(m_shapeEditTarget, newLayer, m_shapeEditCoalesce);
        }
        return;
    }
    const std::optional<common::Affine2D> inv = m_shapeBoxBase.inverse();
    if (!inv)
        return;
    const common::Vec2 local = inv->apply(m_shapeBoxDocPt); // cursor in the layer's own frame
    core::vec::Object edited = m_shapeBoxBaseObject;
    auto* ps = std::get_if<core::vec::ParametricShape>(&edited.geometry);
    auto* line = ps != nullptr ? std::get_if<core::vec::LineShape>(ps) : nullptr;
    if (line == nullptr)
        return;
    if (m_lineHandle == 0)
        line->a = local;
    else if (m_lineHandle == 1)
        line->b = local;
    else if (m_lineHandle == 2) // the round handle bends the line through (midpoint + bend)
        line->bend = local - (line->a + line->b) * 0.5;
    if (m_shapeHost.editShape) {
        m_shapeBoxPushed = true;
        m_shapeHost.editShape(m_shapeEditTarget, edited, std::nullopt, m_shapeEditCoalesce);
    }
}

void VulkanCanvas::reflectActiveShape() {
    if (m_shapeEditTarget == core::kInvalidLayerId || !m_shapeHost.document)
        return;
    core::Document* doc = m_shapeHost.document();
    core::Layer* layer = doc != nullptr ? doc->find(m_shapeEditTarget) : nullptr;
    auto* vl = layer != nullptr ? layer->as<core::VectorLayer>() : nullptr;
    if (vl != nullptr && vl->hasObject())
        reflectShapeOptions(*vl->object());
}

void VulkanCanvas::onShapeOptionsEdited() {
    if (m_reflectingShapeOptions || m_shapeEditTarget == core::kInvalidLayerId || !shapeToolActive())
        return;
    if (!m_shapeHost.document || !m_shapeHost.editShape)
        return;
    core::Document* doc = m_shapeHost.document();
    core::Layer* layer = doc != nullptr ? doc->find(m_shapeEditTarget) : nullptr;
    auto* vl = layer != nullptr ? layer->as<core::VectorLayer>() : nullptr;
    if (vl == nullptr || !vl->hasObject()) {
        m_shapeEditTarget = core::kInvalidLayerId; // the layer vanished (undo/delete): drop it
        return;
    }
    // The active shape tool must match the target's kind (the bar's controls only map then).
    if (shapeKindFor(m_tools->active()) != shapeKindOf(*vl->object()))
        return;
    const core::vec::Object edited = editedObject(*vl->object(), activeShapeOptions());
    if (edited == *vl->object())
        return; // a no-op (e.g. toggling the UI-only "Transform" mode) -- don't push a junk undo step
    m_shapeHost.editShape(m_shapeEditTarget, edited, std::nullopt, m_shapeEditCoalesce);
}

void VulkanCanvas::onShapeColorEdited() {
    if (m_shapeEditTarget == core::kInvalidLayerId || !shapeToolActive())
        return;
    if (!m_shapeHost.document || !m_shapeHost.editShape || !m_shapeHost.foreground ||
        !m_shapeHost.background)
        return;
    core::Document* doc = m_shapeHost.document();
    core::Layer* layer = doc != nullptr ? doc->find(m_shapeEditTarget) : nullptr;
    auto* vl = layer != nullptr ? layer->as<core::VectorLayer>() : nullptr;
    if (vl == nullptr || !vl->hasObject()) {
        m_shapeEditTarget = core::kInvalidLayerId;
        return;
    }
    const core::vec::Object edited =
        recoloredObject(*vl->object(), m_shapeHost.foreground(), m_shapeHost.background());
    m_shapeHost.editShape(m_shapeEditTarget, edited, std::nullopt, m_shapeEditCoalesce);
}

void VulkanCanvas::cancelShapeEdit() {
    if (m_selectingShapeForEdit || m_shapeEditTarget == core::kInvalidLayerId)
        return; // mid-select tool switch keeps the target; nothing bound = nothing to clear
    m_shapeEditTarget = core::kInvalidLayerId;
    requestHostFrame();
}

// A drag frame only re-latches the gesture: the wireframe is rebuilt from that state by the render
// path. Nothing is inserted, nothing is recomposited -- which is the whole point of drawing an
// outline until release (the old live-layer preview re-composited the document per drag frame).
void VulkanCanvas::dragShapeTool() {
    if (!m_shapeDragging)
        return;
    captureShapeDrag();
    requestHostFrame(); // redraw the wireframe at the new box
}

// Release: the shape stops being a wireframe and becomes real. The host spawns the FILLED
// VectorLayer from the final draft (a direct insert, outside the command stack) and commitShape
// immediately detaches and re-adds it through one command -- so the whole gesture is exactly one
// undo step, the same guarantee the live-layer preview gave.
void VulkanCanvas::finishShapeTool() {
    if (!m_shapeDragging)
        return;
    m_shapeDragging = false;
    captureShapeDrag(); // the release position is the shape's final box
    const std::optional<ShapeDraft> draft = currentShapeDraft();
    m_shapeDragMoved = false;
    requestHostFrame(); // the wireframe goes away either way
    if (!draft || !m_shapeHost.spawnShape)
        return; // a click / sub-pixel drag authors nothing at all
    m_shapeHost.spawnShape(*draft);
    if (m_shapeHost.commitShape) {
        const Tool* tool = m_tools != nullptr ? m_tools->activeTool() : nullptr;
        m_shapeHost.commitShape(tool != nullptr ? tool->name() : std::string("Shape"));
    } else if (m_shapeHost.cancelShape) {
        m_shapeHost.cancelShape(); // no commit sink: never leave the spawned layer behind
    }
}

void VulkanCanvas::cancelShapeGesture() {
    cancelShapeBoxGesture(); // an in-flight resize/transform of the selected shape (§7.1; no-op idle)
    if (!m_shapeDragging)
        return;
    m_shapeDragging = false;
    m_shapeDragMoved = false;
    if (m_shapeHost.cancelShape)
        m_shapeHost.cancelShape(); // belt and braces: a spawn only ever survives a failed commit
    requestHostFrame();            // the wireframe disappears
}

// ---- S28 Pen / custom path tool ----------------------------------------------------------------
//
// ⚠ THE COORDINATE RULE, stated once for this whole block. Every pen point comes from
// eventDocPoint() (or eventLogicalPoint(), for a screen-space tolerance), and those are honest ONLY
// inside this widget's own handle() -- the PointerFrame guard at the top of handle() opens that
// window, and FLTK restores the event pair the instant handle() returns. So the pen is driven from
// VulkanCanvas::handle() and from NOWHERE else. Routing its presses through MainWindow::handle()
// would deliver window-relative coordinates, and mixing a window-space press with a canvas-space
// drag displaces the whole path by the canvas's origin inside the top-level -- the menu bar +
// options bar above us and the tool rail to our left. The per-frame render path (syncPenOverlay,
// syncPenChrome) never reads FLTK at all: it works from the LATCHED gesture state, exactly as the
// shape wireframe works from m_shapeDragDoc.
//
// Two halves that never run together: AUTHORING (ui::PenGesture, document space, nothing enters the
// document until the path is finished -- the S26-c wireframe discipline) and NODE EDITING of a
// committed vec::Path layer (layer-local space, one coalesced SetVectorObjectCommand per gesture).

namespace {
constexpr double kPenPickScreenPx = 6.0;  // node / handle / segment grab radius, logical screen px
constexpr double kPenCloseScreenPx = 9.0; // a click this close to the first node closes the path
} // namespace

bool VulkanCanvas::penToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::Pen;
}

bool VulkanCanvas::penGestureActive() const noexcept {
    return m_pen.active() || m_penEditDragging;
}

PenOptions VulkanCanvas::activePenOptions() const {
    PenOptions o;
    if (m_penHost.foreground)
        o.foreground = m_penHost.foreground();
    if (m_penHost.background)
        o.background = m_penHost.background();
    const Tool* tool = m_tools != nullptr ? m_tools->activeTool() : nullptr;
    if (tool == nullptr || tool->id() != ToolId::Pen)
        return o;
    const auto opt = [&](const char* id, double def) {
        for (const ToolOption& v : tool->options())
            if (v.id == id)
                return v.value;
        return def;
    };
    o.fill = opt("fill", 1.0) != 0.0;
    o.strokeEnabled = opt("stroke", 1.0) != 0.0;
    o.strokeWidth = opt("weight", 2.0);
    const int cap = static_cast<int>(opt("cap", 1.0)); // 0 Butt / 1 Round / 2 Square
    o.cap = cap <= 0 ? core::vec::LineCap::Butt
            : cap == 1 ? core::vec::LineCap::Round
                       : core::vec::LineCap::Square;
    const int join = static_cast<int>(opt("join", 1.0)); // 0 Miter / 1 Round / 2 Bevel
    o.join = join <= 0 ? core::vec::LineJoin::Miter
             : join == 1 ? core::vec::LineJoin::Round
                         : core::vec::LineJoin::Bevel;
    o.dashStyle = static_cast<int>(opt("dash", 0.0));
    return o;
}

core::VectorLayer* VulkanCanvas::penEditLayer() const {
    if (m_penEditTarget == core::kInvalidLayerId || !m_penHost.document)
        return nullptr;
    core::Document* doc = m_penHost.document();
    core::Layer* l = doc != nullptr ? doc->find(m_penEditTarget) : nullptr;
    auto* vl = l != nullptr ? l->as<core::VectorLayer>() : nullptr;
    if (vl == nullptr || !vl->hasObject())
        return nullptr;
    return penToolBinds(*vl->object()) ? vl : nullptr; // a shape / gradient is not the Pen's
}

bool VulkanCanvas::penEditFrame(common::Affine2D& world, common::Affine2D& inv) const {
    const core::VectorLayer* vl = penEditLayer();
    if (vl == nullptr)
        return false;
    world = core::worldTransform(*vl);
    const std::optional<common::Affine2D> i = world.inverse();
    if (!i)
        return false; // a singular ancestor: nothing sane to edit through it
    inv = *i;
    return true;
}

double VulkanCanvas::penPickLocal(const common::Affine2D& worldInv) const {
    // SCREEN px -> document px (the zoom; rotation preserves lengths) -> LAYER-LOCAL px (the layer's
    // own scale). Derived in that order so the grab band is a constant number of pixels ON SCREEN at
    // every zoom -- a tolerance left in document units would be unusable at 6400% and at 5%.
    const double docPx = kPenPickScreenPx / std::max(m_view.zoom(), CanvasView::kMinZoom);
    const double local = worldInv.applyVector({docPx, 0.0}).length();
    return local > 0.0 ? local : docPx;
}

bool VulkanCanvas::pickPathForEdit() {
    if (!m_penHost.document)
        return false;
    core::Document* doc = m_penHost.document();
    if (doc == nullptr)
        return false;
    const double pickDoc = kPenPickScreenPx / std::max(m_view.zoom(), CanvasView::kMinZoom);
    core::VectorLayer* vl = core::topmostVectorLayerAt(doc->root(), eventDocPoint(), pickDoc);
    if (vl == nullptr || !vl->hasObject() || !penToolBinds(*vl->object()))
        return false;
    if (vl->id() != m_penEditTarget) {
        m_penEditTarget = vl->id();
        m_penSel = PenSelection{};
        ++m_penEditCoalesce; // a fresh edit session is a new undo step
        reflectPenOptions(*vl->object());
    }
    requestHostFrame();
    return true;
}

void VulkanCanvas::reflectPenOptions(const core::vec::Object& obj) {
    Tool* tool = m_tools != nullptr ? m_tools->activeTool() : nullptr;
    if (tool == nullptr || tool->id() != ToolId::Pen)
        return;
    PenOptions ro; // colours stay as the swatch has them (readPenOptions leaves them alone)
    readPenOptions(obj, ro);
    const auto setOpt = [&](const char* id, double v) {
        for (ToolOption& o : tool->options())
            if (o.id == id)
                o.value = v;
    };
    setOpt("fill", ro.fill ? 1.0 : 0.0);
    setOpt("stroke", ro.strokeEnabled ? 1.0 : 0.0);
    setOpt("weight", ro.strokeWidth);
    setOpt("cap", static_cast<double>(static_cast<int>(ro.cap)));
    setOpt("join", static_cast<double>(static_cast<int>(ro.join)));
    setOpt("dash", static_cast<double>(ro.dashStyle));
    m_reflectingPenOptions = true; // the resulting sync is our own write, not a user edit
    m_tools->notifyOptionsChanged();
    m_reflectingPenOptions = false;
}

void VulkanCanvas::applyPenEdit(const core::vec::Path& next, bool newUndoStep) {
    core::VectorLayer* vl = penEditLayer();
    if (vl == nullptr || !m_penHost.editPath)
        return;
    if (newUndoStep)
        ++m_penEditCoalesce;
    core::vec::Object obj = *vl->object();
    obj.geometry = next;
    if (obj == *vl->object())
        return; // a no-op edit must never push a junk undo step
    m_penEditPushed = true;
    m_penHost.editPath(m_penEditTarget, std::move(obj), std::nullopt, m_penEditCoalesce);
}

void VulkanCanvas::pushPenTool() {
    const common::Vec2 doc = eventDocPoint();
    const auto state = Fl::event_state();
    const bool shift = (state & FL_SHIFT) != 0;
    const bool alt = (state & FL_ALT) != 0;
    const bool ctrl = (state & FL_CTRL) != 0;
    const double closeDoc = kPenCloseScreenPx / std::max(m_view.zoom(), CanvasView::kMinZoom);
    m_penAuthorPressScreen = eventLogicalPoint(); // the click-vs-drag dead zone's origin
    m_penAuthorLatched = false;

    // --- authoring in flight: extend, close, or (double-click) finish the path.
    if (m_pen.active()) {
        if (Fl::event_clicks() > 0 && m_pen.nodeCount() >= 2) {
            commitPenPath(); // the poly-lasso convention: a double-click ends an open path
            return;
        }
        m_penAuthorOpts = activePenOptions(); // latch: a tool switch may be what finishes this path
        m_pen.press(doc, closeDoc, shift);
        requestHostFrame();
        return;
    }

    // --- a bound path: grab one of its nodes / handles / segments.
    common::Affine2D world;
    common::Affine2D inv;
    if (penEditFrame(world, inv)) {
        const core::VectorLayer* vl = penEditLayer();
        const auto* p = std::get_if<core::vec::Path>(&vl->object()->geometry);
        if (p != nullptr) {
            const common::Vec2 local = inv.apply(doc);
            const PenHit hit = penHitTest(*p, local, penPickLocal(inv), m_penSel);
            if (hit.hit()) {
                m_penEditBase = *p;
                m_penEditPressLocal = local;
                m_penEditPressScreen = eventLogicalPoint();
                m_penGrab = hit;
                m_penEditLatched = false;
                m_penEditPushed = false;
                ++m_penEditCoalesce; // this gesture is its own undo step
                if (hit.kind == PenHit::Kind::Segment) {
                    // A press on a SEGMENT inserts a node there -- a de Casteljau split, so not one
                    // drawn pixel moves -- and hands the drag straight to the new anchor, so a
                    // single gesture both adds the node and places it.
                    //
                    // ⚠ MULTI-SUBPATH: the insert goes into hit.selection()'s OWN subpath, and the
                    // drag is armed at the address penInsertNode reports back -- never at a
                    // hard-coded first subpath. On a path with several contours (a baked Combine
                    // Paths result) an address taken from the wrong end of that chain edits a
                    // curve the user never touched, silently, which is the one failure a node
                    // editor may not have. When the insert reports NO address (a degenerate
                    // segment) the press selects nothing and arms nothing rather than falling back
                    // to {0, 0} -- the honest answer to "I could not tell where you meant".
                    PenSelection added;
                    const core::vec::Path next = penInsertNode(*p, hit.selection(), hit.t, &added);
                    m_penSel = added;
                    if (!added.valid) {
                        m_penGrab = PenHit{};
                        m_penHover = PenHit{};
                    } else {
                        m_penGrab.kind = PenHit::Kind::Anchor;
                        m_penGrab.subpath = added.subpath;
                        m_penGrab.node = added.node;
                        m_penEditBase = next;
                        m_penEditPressLocal = next.subpaths[added.subpath].nodes[added.node].anchor;
                        applyPenEdit(next, /*newUndoStep=*/false);
                    }
                } else if (hit.kind == PenHit::Kind::Anchor && ctrl) {
                    // Ctrl-click an anchor REMOVES it (the Pen owns Ctrl -- the temporary
                    // eyedropper is gated on the stroke tools, which this is not).
                    applyPenEdit(penDeleteNode(*p, hit.selection()), /*newUndoStep=*/false);
                    clearPenAddresses();
                } else if (hit.kind == PenHit::Kind::Anchor && alt) {
                    // Alt-click an anchor toggles cusp <-> smooth (Illustrator's Anchor Point tool).
                    applyPenEdit(penToggleNodeType(*p, hit.selection()), /*newUndoStep=*/false);
                    m_penSel = hit.selection();
                    m_penGrab = PenHit{};
                } else if (hit.kind == PenHit::Kind::Anchor) {
                    m_penSel = hit.selection();
                } else if (hit.kind == PenHit::Kind::InHandle ||
                           hit.kind == PenHit::Kind::OutHandle) {
                    // Every node's handles are drawn AND pickable now, so grabbing one has to move
                    // the selection onto its node -- otherwise the knob you are dragging would be
                    // hollow while some other node stayed filled.
                    m_penSel = hit.selection();
                }
                m_penEditDragging = m_penGrab.kind != PenHit::Kind::None;
                requestHostFrame();
                return;
            }
        }
    }

    // --- the press missed the bound path (or nothing was bound): bind whatever IS under it...
    if (pickPathForEdit())
        return;
    // ... and on empty canvas drop the binding and start authoring a fresh path.
    m_penEditTarget = core::kInvalidLayerId;
    m_penSel = PenSelection{};
    m_penGrab = PenHit{};
    m_penAuthorOpts = activePenOptions(); // latch: see the member's comment
    m_pen.press(doc, closeDoc, shift);
    requestHostFrame();
}

void VulkanCanvas::dragPenTool() {
    const auto state = Fl::event_state();
    const bool shift = (state & FL_SHIFT) != 0;
    const bool alt = (state & FL_ALT) != 0;
    if (m_pen.draggingHandle()) {
        // A click is a CORNER node and a drag is a smooth one, so the two must be told apart by
        // real travel -- a one-pixel jitter must not pull a one-pixel handle pair out of a click.
        if (!m_penAuthorLatched) {
            if ((eventLogicalPoint() - m_penAuthorPressScreen).length() < kMoveDragDeadZonePx)
                return;
            m_penAuthorLatched = true;
        }
        m_pen.dragHandle(eventDocPoint(), shift, alt); // pull the just-placed node's handles out
        requestHostFrame();
        return;
    }
    if (!m_penEditDragging || m_penGrab.kind == PenHit::Kind::None)
        return;
    if (!m_penEditLatched) { // a click (with its jitter) must not push a zero-delta undo step
        if ((eventLogicalPoint() - m_penEditPressScreen).length() < kMoveDragDeadZonePx)
            return;
        m_penEditLatched = true;
    }
    common::Affine2D world;
    common::Affine2D inv;
    if (!penEditFrame(world, inv))
        return;
    const PenSelection at{true, m_penGrab.subpath, m_penGrab.node};
    if (at.subpath >= m_penEditBase.subpaths.size() ||
        at.node >= m_penEditBase.subpaths[at.subpath].nodes.size())
        return; // the path changed underneath us (an undo mid-drag): abandon quietly
    const common::Vec2 local = inv.apply(eventDocPoint());
    core::vec::Path next;
    if (m_penGrab.kind == PenHit::Kind::Anchor) {
        const common::Vec2 target = shift ? penConstrainAngle(m_penEditPressLocal, local) : local;
        next = penMoveAnchor(m_penEditBase, at, target - m_penEditPressLocal);
    } else {
        const core::vec::Node& base = m_penEditBase.subpaths[at.subpath].nodes[at.node];
        const common::Vec2 target = shift ? penConstrainAngle(base.anchor, local) : local;
        next = penMoveHandle(m_penEditBase, at, m_penGrab.kind == PenHit::Kind::OutHandle, target,
                             alt);
    }
    applyPenEdit(next, /*newUndoStep=*/false);
    requestHostFrame();
}

void VulkanCanvas::finishPenDrag() {
    if (m_pen.draggingHandle()) {
        m_pen.release();
        if (m_pen.closed())
            commitPenPath(); // the closing press (and its optional handle pull) ends the path
        requestHostFrame();
        return;
    }
    if (!m_penEditDragging)
        return;
    m_penEditDragging = false;
    m_penEditLatched = false;
    m_penGrab = PenHit{};
    ++m_penEditCoalesce; // the next gesture opens its own undo step
    if (m_penEditPushed)
        requestHostFrame();
    m_penEditPushed = false;
}

void VulkanCanvas::movePenTool() {
    if (!penToolActive())
        return;
    m_penHoverScreen = eventLogicalPoint(); // latched here: the frame loop must never read FLTK
    m_penHasHover = true;
    if (m_pen.active()) {
        m_penAuthorOpts = activePenOptions(); // keep the latch current with the bar while you draw
        m_pen.moveTo(eventDocPoint(), (Fl::event_state() & FL_SHIFT) != 0);
        requestHostFrame(); // the rubber-band segment (and the closing ring) follow the pointer
        return;
    }
    updatePenHover();
}

void VulkanCanvas::updatePenHover() {
    // Which knob of the BOUND path is under the pointer, so its border can light up. The same pick
    // the press does (penHitTest with the same screen-derived tolerance), so what highlights is
    // exactly what a click would take -- a hover that promised a different element than the press
    // delivers is worse than no hover at all.
    PenHit hit;
    common::Affine2D world;
    common::Affine2D inv;
    if (penEditFrame(world, inv)) {
        const core::VectorLayer* vl = penEditLayer();
        const auto* p = std::get_if<core::vec::Path>(&vl->object()->geometry);
        if (p != nullptr)
            hit = penHitTest(*p, inv.apply(eventDocPoint()), penPickLocal(inv), m_penSel);
    }
    if (hit.kind == PenHit::Kind::Segment)
        hit = PenHit{}; // a segment is not a knob; there is nothing to brighten
    if (hit.kind == m_penHover.kind && hit.subpath == m_penHover.subpath &&
        hit.node == m_penHover.node)
        return; // unchanged: no repaint (FL_MOVE fires on every pixel of travel)
    m_penHover = hit;
    requestHostFrame();
}

// Every (subpath, node) address the canvas is holding, dropped at once. Called after any edit that
// changes the path's STRUCTURE rather than a coordinate.
//
// ⚠ MULTI-SUBPATH. On a one-contour path a stale address was self-limiting: penDeleteNode drops a
// subpath that falls under two nodes, so the whole path went with it and every address simply
// failed addressValid(). On a path of SEVERAL contours -- what Layer ▸ Combine Paths now commits --
// dropping subpath 1 of 3 RENUMBERS subpath 2 to 1, so a kept address stops being invalid and
// starts naming a different contour's node instead: the hover would light a knob nobody is near
// and the next drag would reshape a curve nobody touched. Clearing all three is the cheap, total
// answer; the next FL_MOVE re-derives the hover from the path as it now stands.
void VulkanCanvas::clearPenAddresses() {
    m_penSel = PenSelection{};
    m_penGrab = PenHit{};
    m_penHover = PenHit{};
}

bool VulkanCanvas::penDeleteSelectedNode() {
    const core::VectorLayer* vl = penEditLayer();
    if (vl == nullptr || !m_penSel.valid)
        return false;
    const auto* p = std::get_if<core::vec::Path>(&vl->object()->geometry);
    if (p == nullptr)
        return false;
    ++m_penEditCoalesce; // a keyboard delete is a discrete undo step of its own
    const core::vec::Path next = penDeleteNode(*p, m_penSel);
    clearPenAddresses();
    applyPenEdit(next, /*newUndoStep=*/false);
    requestHostFrame();
    return true;
}

void VulkanCanvas::commitPenPath() {
    if (!m_pen.active())
        return;
    const core::vec::Path authored = m_pen.path();
    m_pen.reset();
    requestHostFrame(); // the in-flight preview goes away either way
    // The LATCHED options, not a fresh read: one of the finish routes is a TOOL SWITCH, and by the
    // time onToolChanged runs the active tool is no longer the Pen (see m_penAuthorOpts).
    const std::optional<PenDraft> draft = buildPenDraft(authored, m_penAuthorOpts);
    if (!draft || !m_penHost.spawnPath) {
        if (m_penHost.cancelPath)
            m_penHost.cancelPath(); // belt and braces: never leave a spawn behind
        return;
    }
    // The same spawn-then-commit pair the Shape tool uses: a direct insert outside the command
    // stack, immediately detached and re-added THROUGH one command, so the whole authoring session
    // -- however many clicks it took -- is exactly one undo step.
    m_penHost.spawnPath(*draft);
    if (m_penHost.commitPath)
        m_penHost.commitPath(std::string(_("Path")));
    else if (m_penHost.cancelPath)
        m_penHost.cancelPath();
}

void VulkanCanvas::cancelPenGesture() {
    if (!m_pen.active())
        return;
    m_pen.reset();
    if (m_penHost.cancelPath)
        m_penHost.cancelPath();
    requestHostFrame();
}

void VulkanCanvas::cancelPenEdit() {
    m_penEditDragging = false;
    m_penEditLatched = false;
    m_penGrab = PenHit{};
    m_penHover = PenHit{}; // the binding is gone, so the highlight it named is too
    if (m_penEditTarget == core::kInvalidLayerId)
        return;
    m_penEditTarget = core::kInvalidLayerId;
    m_penSel = PenSelection{};
    requestHostFrame();
}

void VulkanCanvas::onPenOptionsEdited() {
    if (m_reflectingPenOptions || !penToolActive())
        return;
    const core::VectorLayer* vl = penEditLayer();
    if (vl == nullptr || !m_penHost.editPath)
        return;
    const core::vec::Object edited = penPaintedObject(*vl->object(), activePenOptions());
    if (edited == *vl->object())
        return;
    m_penHost.editPath(m_penEditTarget, edited, std::nullopt, m_penEditCoalesce);
}

void VulkanCanvas::onPenColorEdited() {
    if (!penToolActive() || !m_penHost.foreground || !m_penHost.background)
        return;
    const core::VectorLayer* vl = penEditLayer();
    if (vl == nullptr || !m_penHost.editPath)
        return;
    const core::vec::Object edited =
        penRecoloredObject(*vl->object(), m_penHost.foreground(), m_penHost.background());
    if (edited == *vl->object())
        return;
    m_penHost.editPath(m_penEditTarget, edited, std::nullopt, m_penEditCoalesce);
}

bool VulkanCanvas::penChromeSource(core::vec::Path& src, PenSelection& sel,
                                   common::Affine2D& pathToScreen) const {
    if (!penToolActive())
        return false;
    if (m_pen.active()) {
        src = m_pen.pathWithRubberBand();
        sel = m_pen.liveNode();
        pathToScreen = m_view.docToScreen(); // authoring works in DOCUMENT space
        return !src.subpaths.empty();
    }
    const core::VectorLayer* vl = penEditLayer();
    if (vl == nullptr)
        return false;
    const auto* p = std::get_if<core::vec::Path>(&vl->object()->geometry);
    if (p == nullptr)
        return false;
    src = *p;
    sel = m_penSel;
    pathToScreen = m_view.docToScreen() * core::worldTransform(*vl); // ... editing, LAYER-local
    return true;
}

std::vector<common::Vec2> VulkanCanvas::penOutlineScreenPolyline() const {
    // BOTH halves draw a spine: the in-flight path while authoring, and the BOUND path while
    // editing. Edit mode used to show none at all -- only the node marks -- so a path you were
    // shaping had no visible curve of its own beyond whatever the layer happened to paint, and a
    // stroke-less filled path had nothing at all. That was the single largest hole in the tool.
    core::vec::Path src;
    PenSelection sel;
    common::Affine2D pathToScreen = common::Affine2D::identity();
    if (!penChromeSource(src, sel, pathToScreen))
        return {};
    // Flatten in the PATH's own space with the path -> PHYSICAL px transform, so the preview's
    // smoothness tracks zoom + HiDPI exactly like the rasteriser's does (shapeOutlinePolyline's
    // rule). The points come back in the path's own space and are lifted to screen below.
    const common::Affine2D toDevice =
        common::Affine2D::scaling(m_contentScale, m_contentScale) * pathToScreen;
    std::vector<common::Vec2> pts = penPathPolyline(src, toDevice);
    if (pts.size() > render::kLassoMaxVerts) {
        // A huge path at a deep zoom: fixed-stride thinning, applied PER CONTOUR so the break
        // markers (and each contour's own last point) survive. The final clamp is the honest one --
        // many contours can each round their own tail up -- and it can only cost whole trailing
        // contours, never splice two of them together, because a truncation leaves a dangling break
        // and the shader skips any segment touching one.
        const std::size_t n = pts.size();
        const std::size_t stride = (n + render::kLassoMaxVerts - 2) / (render::kLassoMaxVerts - 1);
        std::vector<common::Vec2> thinned;
        thinned.reserve(render::kLassoMaxVerts);
        std::size_t run = 0; // index of the contour's first point in `pts`
        while (run < n) {
            std::size_t end = run;
            while (end < n && !isPolylineBreak(pts[end]))
                ++end;
            for (std::size_t i = run; i < end; i += stride)
                thinned.push_back(pts[i]);
            if (end > run && (end - 1 - run) % stride != 0)
                thinned.push_back(pts[end - 1]); // keep the contour's own tail
            if (end < n)
                thinned.push_back(kPolylineBreak);
            run = end + 1;
        }
        if (thinned.size() > render::kLassoMaxVerts)
            thinned.resize(render::kLassoMaxVerts);
        pts = std::move(thinned);
    }
    // The points come back in the PATH's own space (flatten never moves them; toDevice only picks
    // the tolerance), so lift them here -- and leave the break markers alone, or they would stop
    // being out-of-range sentinels the moment the view panned.
    for (common::Vec2& p : pts)
        if (!isPolylineBreak(p))
            p = pathToScreen.apply(p); // -> logical screen px (the renderer scales to physical)
    return pts;
}

void VulkanCanvas::syncPenOverlay() {
    // Rides the lasso polyline channel, like the shape wireframe and the Type frame: a pen tool and
    // a lasso/shape tool can never both be active, and syncLassoOverlay/syncShapeOverlay have
    // already had their say, so we own the lane while the Pen is the active tool.
    if (!m_renderer || !penToolActive())
        return;
    m_renderer->setLassoPolyline(penOutlineScreenPolyline());
}

void VulkanCanvas::syncPenChrome() {
    // The pen's OWN overlay channel (binding 6), pushed every frame so the chrome tracks pan / zoom
    // / rotate like every other on-canvas gizmo. penChromeSource() reads only LATCHED state (never
    // Fl::event_x/y): this runs from the frame loop, where the FLTK event pair means nothing.
    if (!m_renderer)
        return;
    core::vec::Path src;
    PenSelection sel;
    common::Affine2D pathToScreen = common::Affine2D::identity();
    if (!penChromeSource(src, sel, pathToScreen)) {
        m_renderer->setPenChrome({}, {}, {}, 0.0); // the pen is not up: clear last frame's chrome
        return;
    }
    for (core::vec::SubPath& sp : src.subpaths)
        for (core::vec::Node& n : sp.nodes) {
            n.anchor = pathToScreen.apply(n.anchor);
            n.inHandle = pathToScreen.apply(n.inHandle);
            n.outHandle = pathToScreen.apply(n.outHandle);
        }
    const PenChrome chrome =
        penChromeMarks(src, sel, m_penHover, render::kPenMarkMax, render::kPenStemMax);
    std::vector<render::WindowRenderer::PenMark> marks;
    marks.reserve(chrome.marks.size());
    for (const PenChromeMark& m : chrome.marks)
        marks.push_back({m.pos, static_cast<std::uint32_t>(m.kind),
                         static_cast<std::uint32_t>((m.selected ? 1u : 0u) |
                                                    (m.hovered ? 2u : 0u))});
    std::vector<render::WindowRenderer::PenStem> stems;
    stems.reserve(chrome.stems.size());
    for (const PenChromeStem& s : chrome.stems)
        stems.push_back({s.a, s.b});
    // The closing-loop affordance: while an OPEN path is being authored and the pointer sits inside
    // the very radius PenGesture::press closes on, ring that first node. Pure hit tests are
    // invisible affordances; this is the one place the tool can say "click here and it shuts".
    common::Vec2 ringCenter{};
    double ringRadius = 0.0;
    if (m_pen.active() && m_penHasHover && !m_pen.closed() && m_pen.nodeCount() >= 2 &&
        penCloseTarget(src, m_penHoverScreen, kPenCloseScreenPx, ringCenter)) {
        ringRadius = kPenCloseScreenPx;
    }
    m_renderer->setPenChrome(marks, stems, ringCenter, ringRadius);
}

// ---- S22 Gradient tool -------------------------------------------------------------------------

bool VulkanCanvas::gradientToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::Gradient;
}

GradientShape VulkanCanvas::activeGradientShape() const {
    const Tool* tool = m_tools != nullptr ? m_tools->activeTool() : nullptr;
    if (tool != nullptr)
        for (const ToolOption& o : tool->options())
            if (o.id == "type")
                return gradientShapeFromChoice(static_cast<int>(o.value));
    return GradientShape::Linear;
}

double VulkanCanvas::activeGradientOpacity() const {
    const Tool* tool = m_tools != nullptr ? m_tools->activeTool() : nullptr;
    if (tool != nullptr)
        for (const ToolOption& o : tool->options())
            if (o.id == "opacity")
                return std::clamp(o.value / 100.0, 0.0, 1.0);
    return 1.0;
}

std::optional<GradientDraft> VulkanCanvas::currentGradientDraft() const {
    if (!m_gradientHost.document || !m_gradientHost.workingGradient)
        return std::nullopt;
    core::Document* doc = m_gradientHost.document();
    if (doc == nullptr)
        return std::nullopt;
    const core::vec::Gradient work = m_gradientHost.workingGradient();
    const auto state = Fl::event_state();
    // ⚠ This runs from the FRAME LOOP as well as from the drag: currentGradientHandles() feeds the
    // axis gizmo, and syncMoveOverlay() places that every frame. eventDocPoint() is safe in BOTH
    // only because it goes through eventLogicalPoint() -- before that it read Fl::event_x/y raw,
    // which outside our dispatch still carry the canvas's origin inside the window, and the gizmo's
    // far handle sat a whole toolbar + options bar away from the cursor for the length of the drag.
    return buildGradientDraft(activeGradientShape(), m_gradientPressDoc, eventDocPoint(),
                              static_cast<double>(doc->width()), static_cast<double>(doc->height()),
                              work.stops, work.spread, (state & FL_SHIFT) != 0, work.dither);
}

bool VulkanCanvas::gradientEditActive() const {
    return gradientToolActive() && m_gradientEditTarget != core::kInvalidLayerId;
}

void VulkanCanvas::bindGradientEditToActiveLayer(core::LayerId activeLayer) {
    // Binding comes from the ACTIVE layer, not a hit-test: a gradient layer is full-bleed, so a click
    // always lands on it and a hit-test could never distinguish "edit this" from "lay a new one".
    m_gradientHandle = -1;
    m_gradientEditTarget = core::kInvalidLayerId;
    if (!gradientToolActive() || !m_gradientHost.document || activeLayer == core::kInvalidLayerId)
        return;
    core::Document* doc = m_gradientHost.document();
    core::Layer* layer = doc != nullptr ? doc->find(activeLayer) : nullptr;
    auto* vl = layer != nullptr ? layer->as<core::VectorLayer>() : nullptr;
    if (vl == nullptr || !vl->hasObject() || !gradientToolBinds(*vl->object()))
        return; // a plain SHAPE layer is not ours -> nothing to bind (a drag authors a fresh one)
    m_gradientEditTarget = activeLayer;
    ++m_gradientEditCoalesce; // a fresh bind starts a new undo step for the first handle drag
    requestHostFrame();       // its handles should show this frame
}

void VulkanCanvas::cancelGradientEdit() {
    m_gradientEditTarget = core::kInvalidLayerId;
    m_gradientHandle = -1;
}

bool VulkanCanvas::currentGradientHandles(GradientHandles& out) const {
    if (!gradientToolActive() || !m_gradientHost.document)
        return false;
    core::Document* doc = m_gradientHost.document();
    if (doc == nullptr)
        return false;
    // An in-flight authoring drag drives the gizmo off the live draft (tracks the cursor).
    if (m_gradientDragging && m_gradientHandle < 0) {
        if (const std::optional<GradientDraft> draft = currentGradientDraft()) {
            out = gradientHandles(draft->object, draft->placement);
            return out.valid;
        }
        return false;
    }
    // Otherwise the bound edit target's current geometry.
    if (m_gradientEditTarget != core::kInvalidLayerId) {
        core::Layer* layer = doc->find(m_gradientEditTarget);
        auto* vl = layer != nullptr ? layer->as<core::VectorLayer>() : nullptr;
        if (vl != nullptr && vl->hasObject()) {
            out = gradientHandles(*vl->object(), core::worldTransform(*layer));
            return out.valid;
        }
    }
    return false;
}

bool VulkanCanvas::gradientGizmoPoints(common::Vec2& a, common::Vec2& b, common::Vec2& mid,
                                       common::Vec2& minor) const {
    GradientHandles h;
    if (!currentGradientHandles(h))
        return false;
    a = m_view.toScreen(h.start);
    b = m_view.toScreen(h.end);
    mid = m_view.toScreen((h.start + h.end) * 0.5);
    // The Elliptical minor-axis handle rides the gizmo's spare corner slot; every other shape
    // repeats the midpoint there, which the shader reads as "no fourth handle".
    minor = h.hasMinor ? m_view.toScreen(h.minor) : mid;
    return true;
}

void VulkanCanvas::pushGradientTool() {
    // With a gradient bound for edit, a press on one of its handles starts a re-drag of that handle.
    if (gradientEditActive()) {
        GradientHandles h;
        if (currentGradientHandles(h)) {
            const double pickDoc = kHandleHitPx / std::max(1e-6, m_view.zoom());
            const int hit = hitGradientHandle(h, eventDocPoint(), pickDoc);
            if (hit >= 0) {
                core::Document* doc = m_gradientHost.document ? m_gradientHost.document() : nullptr;
                core::Layer* layer = doc != nullptr ? doc->find(m_gradientEditTarget) : nullptr;
                auto* vl = layer != nullptr ? layer->as<core::VectorLayer>() : nullptr;
                if (vl != nullptr && vl->hasObject()) {
                    m_gradientHandle = hit;
                    m_gradientHandlePressDoc = eventDocPoint();
                    m_gradientHandleBase = *vl->object();
                    m_gradientHandleWorld = core::worldTransform(*layer);
                    ++m_gradientEditCoalesce; // a fresh handle drag = a new undo step
                    return;
                }
            }
        }
    }
    // Otherwise anchor a fresh authoring drag (which lays down a NEW gradient layer on release).
    m_gradientHandle = -1;
    m_gradientPressDoc = eventDocPoint();
    m_gradientDragging = true;
}

void VulkanCanvas::dragGradientTool() {
    if (m_gradientHandle >= 0) { // re-dragging a handle of the bound gradient layer
        if (m_gradientEditTarget == core::kInvalidLayerId || !m_gradientHost.editGradient)
            return;
        const auto state = Fl::event_state();
        core::vec::Object edited =
            dragGradientHandle(m_gradientHandleBase, m_gradientHandleWorld, m_gradientHandle,
                               m_gradientHandlePressDoc, eventDocPoint(), (state & FL_SHIFT) != 0);
        m_gradientHost.editGradient(m_gradientEditTarget, std::move(edited), m_gradientEditCoalesce);
        return;
    }
    if (!m_gradientDragging)
        return;
    if (const std::optional<GradientDraft> draft = currentGradientDraft()) {
        if (m_gradientHost.previewGradient) { // the live layer IS the preview (Affinity-style)
            m_gradientHost.previewGradient(*draft, activeGradientOpacity());
            m_gradientPreviewing = true;
        }
    }
}

void VulkanCanvas::finishGradientTool() {
    if (m_gradientHandle >= 0) { // a handle re-drag settled: the next one is a new undo step
        m_gradientHandle = -1;
        ++m_gradientEditCoalesce;
        return;
    }
    if (!m_gradientDragging)
        return;
    m_gradientDragging = false;
    const std::optional<GradientDraft> draft = currentGradientDraft();
    if (draft && m_gradientPreviewing) {
        if (m_gradientHost.previewGradient)
            m_gradientHost.previewGradient(*draft, activeGradientOpacity()); // settle on release
        if (m_gradientHost.commitGradient)
            m_gradientHost.commitGradient(); // bake to one Add-Layer undo step; the app re-binds it
    } else if (m_gradientPreviewing && m_gradientHost.cancelGradient) {
        m_gradientHost.cancelGradient(); // released without a usable drag -> drop the preview
    }
    m_gradientPreviewing = false;
}

void VulkanCanvas::cancelGradientGesture() {
    if (m_gradientHandle >= 0)
        m_gradientHandle = -1;
    if (!m_gradientDragging && !m_gradientPreviewing)
        return;
    m_gradientDragging = false;
    if (m_gradientPreviewing && m_gradientHost.cancelGradient)
        m_gradientHost.cancelGradient();
    m_gradientPreviewing = false;
}

// ---- Image-ops live preview (Image Size / Canvas Size / Rotate Arbitrary) --------------------
// The panel stages its pending result; the canvas draws it on the CROP tool's overlay channel,
// because canvas_present.comp's cropOverlay() already renders exactly this feature's picture: the
// staged quad dims everything outside it (a SHRINK shows what will be discarded) and washes +
// hatches in kExpandGreen wherever the quad's document coordinates leave pc.docSize (a GROW shows
// the area that will be added, in the app's working-region hue), with the kBoxColor outline and
// its 8 handles on top. No new shader, and the present push block does not grow -- it is at its
// 128-byte guaranteed budget.
//
// Two things the first cut got wrong, and both are answered below rather than by a new lane:
//   * those 8 handles were DECORATION. They are live now: a corner/edge drag restages the rect
//     through the same CropGesture maths the crop box uses and reports it to the panel, which owns
//     the numbers (setOnImageOpPreviewDrag). A preview whose rect is derived rather than authored
//     -- Rotate Arbitrary's bounding box -- draws no handles at all instead (channel Locked).
//   * the whole canvas is MODAL to a staged preview. handle()'s FL_PUSH / FL_DRAG / FL_RELEASE gate
//     on imageOpPreviewShowing(), so no tool receives the pointer while the panel is up -- one gate
//     at the shared dispatch rather than a check per tool, and the Crop tool is untouched because
//     the Crop tool holding this channel is exactly what makes imageOpPreviewShowing() false.

void VulkanCanvas::setImageOpPreview(const std::optional<ImageOpPreview>& preview) {
    if (!m_imageOpPreview && !preview)
        return; // nothing was staged and nothing is being staged: no frame to spend
    m_imageOpPreview = preview;
    if (!preview) {
        // The panel dropped the preview (Apply / close / document swap). Any handle drag it owned
        // goes with it -- silently, since there is nothing left to report to.
        m_imageOpDrag.cancel();
        m_imageOpHandle = -1;
    }
    // syncCropOverlay() + updateOverlayTile() re-read this every frame, so both the claim and the
    // clean release happen on the next one -- there is no torn state to unwind here.
    requestHostFrame();
}

bool VulkanCanvas::imageOpPreviewActive() const { return m_imageOpPreview.has_value(); }

bool VulkanCanvas::imageOpPreviewShowing() const {
    // The Crop tool ALWAYS wins the shared channel: it is a live gesture with its own hit-testing
    // and handles, the preview is a panel readout. In practice they never coexist (picking Crop
    // closes the panel), so this makes the arbitration a guarantee instead of a policy every caller
    // has to remember. The Smart Recompose review outranks both, and for a stronger reason: while
    // it runs, what the view shows IS the assembled preview -- a different space -- so a rect in
    // DOCUMENT coordinates drawn over it would be meaningless. (This is also the tool-suppression
    // gate, so the review's own placement drags keep working for free.)
    return m_imageOpPreview.has_value() && m_imageOpPreview->w > 0 && m_imageOpPreview->h > 0 &&
           !cropToolActive() && !m_recomposeReview;
}

bool VulkanCanvas::imageOpHandlesLive() const {
    // A rotated or mirrored preview's rect is DERIVED (Rotate Arbitrary computes the bounding box
    // of the turned document), so there is no edit a handle drag could express -- the channel
    // draws none rather than showing eight decorations, which is the whole of the "handles that do
    // nothing" report.
    return imageOpPreviewShowing() && m_imageOpPreview->angleRad == 0.0 &&
           !m_imageOpPreview->flipH && !m_imageOpPreview->flipV;
}

common::Rect VulkanCanvas::imageOpPreviewRect() const {
    if (!m_imageOpPreview)
        return {};
    return {static_cast<double>(m_imageOpPreview->x), static_cast<double>(m_imageOpPreview->y),
            static_cast<double>(m_imageOpPreview->w), static_cast<double>(m_imageOpPreview->h)};
}

// The handle drag. Everything below takes the document size from the VIEW rather than from a tool
// host: the preview belongs to no tool, and m_view.documentSize() is the same number the present
// pass's pc.docSize carries -- so the snap band, the safety envelope and the drawn expansion wash
// all agree by construction.
bool VulkanCanvas::pushImageOpPreview() {
    m_imageOpHandle = -1;
    m_imageOpDrag.cancel();
    if (!imageOpHandlesLive())
        return false;
    std::array<common::Vec2, 4> corners{};
    if (!imageOpPreviewCorners(corners))
        return false;
    // Handles ONLY -- no body Move and no rotate band. The rect's POSITION is the panel's 9-point
    // anchor (Canvas Size) or pinned to the origin (Image Size); a free drag of the whole box would
    // express a state neither of those can hold, so the body stays inert while still swallowing the
    // press. rotateBand 0 makes hitTransformControls answer Move (inside) or nothing (outside),
    // both of which we decline.
    const std::optional<TransformHit> hit =
        hitTransformControls(m_cursorLogical, corners, kHandleHitPx, /*rotateBand=*/0.0);
    if (!hit || hit->mode != TransformMode::Scale || hit->handle < 0)
        return false;
    if (!m_imageOpDrag.begin(CropMode::Resize, hit->handle, eventDocPoint(), imageOpPreviewRect()))
        return false;
    m_imageOpHandle = hit->handle;
    return true;
}

void VulkanCanvas::dragImageOpPreview() {
    if (!m_imageOpDrag.active())
        return;
    const auto state = Fl::event_state();
    const common::Vec2 size = m_view.documentSize();
    // The crop drag's own snap band (8 logical px, zoom-corrected): an edge released near a canvas
    // edge lands exactly on it, so "back to the original size" is reachable by hand.
    const double snapTol = 8.0 / std::max(m_view.zoom(), 1e-6);
    emitImageOpRect(m_imageOpDrag.rectFor(eventDocPoint(), /*ratio=*/0.0, (state & FL_SHIFT) != 0,
                                          (state & FL_ALT) != 0, size.x, size.y, snapTol));
    updateToolCursor(true); // the grabbed handle keeps its arrow even if the drag strays off us
    requestHostFrame();
}

void VulkanCanvas::finishImageOpDrag() {
    if (!m_imageOpDrag.active())
        return;
    // One settling report at the release point: FL_DRAG delivery can coalesce the last motion away,
    // and the panel's fields must end on exactly the rect that is drawn.
    const auto state = Fl::event_state();
    const common::Vec2 size = m_view.documentSize();
    const double snapTol = 8.0 / std::max(m_view.zoom(), 1e-6);
    emitImageOpRect(m_imageOpDrag.rectFor(eventDocPoint(), /*ratio=*/0.0, (state & FL_SHIFT) != 0,
                                          (state & FL_ALT) != 0, size.x, size.y, snapTol));
    m_imageOpDrag.cancel();
    m_imageOpHandle = -1;
    requestHostFrame();
}

void VulkanCanvas::cancelImageOpDrag() {
    if (!m_imageOpDrag.active())
        return;
    // ⚠ Runs from the KEY handler (Esc), where the pointer frame is closed -- so it reports the
    // press-time rect the gesture latched, and never reads an event position.
    emitImageOpRect(m_imageOpDrag.base());
    m_imageOpDrag.cancel();
    m_imageOpHandle = -1;
    updateToolCursor(m_pointerInside);
    requestHostFrame();
}

void VulkanCanvas::emitImageOpRect(const common::Rect& r) {
    if (!m_onImageOpPreviewDrag)
        return;
    const common::Vec2 size = m_view.documentSize();
    if (size.x < 0.5 || size.y < 0.5)
        return; // no document: nothing to express the rect against
    const CropPixels cp = snapCropRect(r, static_cast<std::uint32_t>(std::lround(size.x)),
                                       static_cast<std::uint32_t>(std::lround(size.y)));
    if (cp.w == 0 || cp.h == 0)
        return;
    m_onImageOpPreviewDrag(cp.x, cp.y, cp.w, cp.h);
}

// The pointer over the preview: its handles get the matching resize arrow; everywhere else the
// plain arrow, which is the honest reading -- the tools are parked while the preview is staged, so
// nothing else on this canvas is armed.
int VulkanCanvas::imageOpCursorState() const {
    if (!imageOpHandlesLive())
        return -1;
    std::array<common::Vec2, 4> corners{};
    if (!imageOpPreviewCorners(corners))
        return -1;
    if (m_imageOpDrag.active())
        return m_imageOpHandle >= 0 ? resizeCursorFor(corners, m_imageOpHandle) : -1;
    const std::optional<TransformHit> hit =
        hitTransformControls(m_cursorLogical, corners, kHandleHitPx, /*rotateBand=*/0.0);
    if (!hit || hit->mode != TransformMode::Scale || hit->handle < 0)
        return -1;
    return resizeCursorFor(corners, hit->handle);
}

bool VulkanCanvas::imageOpPreviewCorners(std::array<common::Vec2, 4>& out) const {
    if (!m_imageOpPreview || m_imageOpPreview->w == 0 || m_imageOpPreview->h == 0)
        return false;
    const ImageOpPreview& p = *m_imageOpPreview;
    const common::Rect rect{static_cast<double>(p.x), static_cast<double>(p.y),
                            static_cast<double>(p.w), static_cast<double>(p.h)};
    // A mirror maps a rectangle's corner SET onto itself, so a flip never moves this outline -- but
    // one mirror does reverse the sense of a rotation taken inside it (and two compose back into a
    // 180 deg turn, which is again a no-op on the outline). Folding it into the angle is the whole
    // of the flip's effect here. What must NOT happen is expressing the flip by reordering the
    // corners: controlsQuadDist() decides inside/outside by WINDING, so a reversed quad would
    // invert the shield and dim precisely the region the preview is there to keep.
    const double angle = p.flipH != p.flipV ? -p.angleRad : p.angleRad;
    // Mapped exactly as cropCorners() maps the crop box (rotated-frame corners -> screen), so a
    // rotation preview rides the same path the crop rotation already proved out.
    const std::array<common::Vec2, 4> dc = cropBoxCorners(rect, angle, rect.center());
    for (std::size_t i = 0; i < 4; ++i)
        out[i] = m_view.toScreen(dc[i]);
    return true;
}

void VulkanCanvas::syncCropOverlay() {
    if (!m_renderer)
        return;
    std::array<common::Vec2, 4> corners{};
    // Gate on a staged rect so DrawToBegin shows no resting box/handles before the first drag; in
    // WholeCanvas mode ensureCropRect always stages one on entry, so this never hides a real frame.
    // The Recompose review hides the box outright: the staged rect belongs to the DOCUMENT, and
    // the view is showing the preview (a different space) until the review resolves.
    const bool show = cropToolActive() && !m_recomposeReview && m_cropRect.has_value() &&
                      cropCorners(corners);
    // The Image-ops preview borrows this channel whenever the Crop tool is not holding it. Same
    // quad lane, same shield, same expansion indicator -- but no rule-of-thirds guides: those are a
    // compositional crop affordance, not a resize one.
    if (!show && imageOpPreviewShowing() && imageOpPreviewCorners(corners)) {
        // Which flavour of the preview this is decides what the channel draws beyond the shared
        // picture: Scale adds the ghost outline of the CURRENT frame (Image Size draws its box at
        // the NEW pixel size, and a size change only reads against the frame it replaces), Locked
        // drops the 8 handles (a derived rect -- Rotate Arbitrary's bounding box -- has no drag
        // behind them), Reframe is the crop picture unchanged (Canvas Size).
        const auto channel = !imageOpHandlesLive()
                                 ? render::WindowRenderer::CropChannel::Locked
                             : m_imageOpPreview->scale
                                 ? render::WindowRenderer::CropChannel::Scale
                                 : render::WindowRenderer::CropChannel::Reframe;
        m_renderer->setCropOverlay(true, corners, /*showGrid=*/false, channel);
        m_renderer->setCropHudActive(!m_imageOpPreview->hud.empty());
        return;
    }
    bool grid = true; // the crop tool's "Guides" toggle drives the rule-of-thirds overlay
    if (const Tool* tool = m_tools != nullptr ? m_tools->find(ToolId::Crop) : nullptr)
        for (const ToolOption& o : tool->options())
            if (o.id == "grid")
                grid = o.value != 0.0;
    m_renderer->setCropOverlay(show, corners, grid);
    // The HUD's active flag; its text tile is rasterized in updateOverlayTile() (S16 rework). The
    // present pass clamps/parks the tile into the viewport, so the readout shows at any zoom.
    core::Document* doc = m_cropHost.document ? m_cropHost.document() : nullptr;
    m_renderer->setCropHudActive(show && doc != nullptr);
}

// Rasterize the active overlay's text tile (the rotation dial readout or the crop HUD -- they never
// coexist, dial wins) with the real UI font and hand it to the renderer (S16 rework, replacing the
// in-shader 5x7 bitmap font). Only re-rasterizes + uploads when the string changes, so a static
// readout costs nothing and a drag re-renders one short string per frame.
void VulkanCanvas::updateOverlayTile() {
    if (!m_renderer)
        return;
    const double scale = m_contentScale > 0.0 ? m_contentScale : 1.0;
    const int capW = std::max(64, static_cast<int>(std::lround(420.0 * scale)));
    const int capH = std::max(24, static_cast<int>(std::lround(36.0 * scale)));

    std::string text;
    bool withPill = false;
    int fontPx = std::max(11, static_cast<int>(std::lround(12.5 * scale))); // HUD default
    // The dial shows while R is held, plus a brief "Reset" flash after a double-tap-R reset.
    const bool dialActive = m_rotateDown || nowSeconds() < m_dialResetUntil;
    if (dialActive) {
        fontPx = std::max(15, static_cast<int>(std::lround(18.0 * scale))); // dial reads bigger
        if (!m_rotateDown) {
            text = "Reset"; // the post-reset flash
        } else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%d°", // "N°"
                          static_cast<int>(std::lround(m_view.rotationDegrees())));
            text = buf;
        }
    } else if (cropToolActive() && m_cropRect) { // no staged rect (DrawToBegin) -> no size readout
        if (core::Document* doc = m_cropHost.document ? m_cropHost.document() : nullptr) {
            const CropPixels cp = snapCropRect(cropRectValue(), doc->width(), doc->height());
            const bool metric = m_cropHost.metricUnits && m_cropHost.metricUnits();
            const double perPx = doc->dpi() > 0.0 ? 1.0 / doc->dpi() : 0.0;
            double uw = static_cast<double>(cp.w) * perPx;
            double uh = static_cast<double>(cp.h) * perPx;
            if (metric) {
                uw *= 2.54;
                uh *= 2.54;
            }
            char buf[160];
            std::snprintf(buf, sizeof buf, "%u × %u px · %.1f × %.1f %s", // "× … ·"
                          cp.w, cp.h, uw, uh, metric ? "cm" : "in");
            text = buf;
            withPill = true;
        }
    } else if (imageOpPreviewShowing() && !m_imageOpPreview->hud.empty()) {
        // The Image-ops panel's readout, in the crop HUD's slot -- the same tile, the same pill and
        // the same in-viewport parking, because it IS the crop channel (see syncCropOverlay). The
        // panel owns the wording and the units; the canvas renders the line it was handed.
        text = m_imageOpPreview->hud;
        // ... and, for an Image Size, appends the SCALE FACTOR. With the box drawn at the new pixel
        // size the growth is finally visible, but "how much bigger" is a number and "W × H px"
        // alone never says it. Derived HERE rather than in the panel's string so the percentage
        // reports the rect the canvas is actually drawing, against the live document size -- a HUD
        // that could disagree with the box beside it would be worse than no HUD at all.
        if (m_imageOpPreview->scale) {
            const common::Vec2 size = m_view.documentSize();
            if (size.x > 0.5 && m_imageOpPreview->w > 0) {
                const double pct = 100.0 * static_cast<double>(m_imageOpPreview->w) / size.x;
                char buf[48];
                if (std::abs(pct - std::round(pct)) < 0.05)
                    std::snprintf(buf, sizeof buf, "  ·  %ld%%", std::lround(pct)); // "·"
                else
                    std::snprintf(buf, sizeof buf, "  ·  %.1f%%", pct);
                text += buf;
            }
        }
        withPill = true;
    } else if (moveToolActive() &&
               ((m_transform.active() && m_moveGesturePushed) || m_resetHudShowing)) {
        // The Move-tool transform HUD (S15 follow-up): position/size while moving or scaling, the
        // angle while rotating. The box's live doc-space corners come from the gesture result.
        // Gated on m_moveGesturePushed so a press-without-drag (a click-to-select) doesn't flash
        // the HUD -- it appears only once the box has actually moved from its start (user
        // 2026-06-17). The m_resetHudShowing branch is the brief "0.00 deg" flash after a
        // double-click rotation reset.
        char buf[96];
        if (m_resetHudShowing && !(m_transform.active() && m_moveGesturePushed)) {
            std::snprintf(buf, sizeof buf, "0.00°"); // the just-reset rotation, briefly held
        } else {
            const std::array<common::Vec2, 4> c = framedCorners(m_gestureResult, m_gestureContent);
            if (m_transform.mode() == TransformMode::Rotate) {
                const double pi = 3.14159265358979323846;
                // atan2 already yields the signed (-180, 180] reading; two decimals so the live,
                // un-snapped angle is visible while rotating (user 2026-06-17).
                const double deg = std::atan2(c[1].y - c[0].y, c[1].x - c[0].x) * 180.0 / pi;
                std::snprintf(buf, sizeof buf, "%.2f°", deg);
            } else { // Move / Scale: top-left corner position + box size, in document pixels
                const long w = std::lround((c[1] - c[0]).length());
                const long h = std::lround((c[3] - c[0]).length());
                std::snprintf(buf, sizeof buf, "X %ld  Y %ld  ·  %ld × %ld px", std::lround(c[0].x),
                              std::lround(c[0].y), w, h);
            }
        }
        text = buf;
        withPill = true;
    } else if ((eyedropperToolActive() || temporaryEyedropperActive()) &&
               m_loupeReadout.has_value()) {
        // The eyedropper loupe's hex/RGB readout (S24): the resolved sample colour, formatted into
        // the shared overlay tile the loupe composites by its ring. The dial/crop/Move HUDs are never
        // active alongside the eyedropper (nor its Ctrl temporary mode over a brush), so the tile is
        // the loupe's alone this frame.
        const common::Color8 c = *m_loupeReadout;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s \xC2\xB7 %u %u %u", // "#RRGGBB · R G B"
                      hexString(c).c_str(), c.r, c.g, c.b);
        text = buf;
        withPill = true;
    }

    // Re-rasterize only when the tile content (string / pill / font / capacity) actually changes.
    const std::string key = (withPill ? "H|" : "D|") + std::to_string(fontPx) + "|" +
                            std::to_string(capW) + "x" + std::to_string(capH) + "|" + text;
    if (key == m_overlayTileKey)
        return;
    m_overlayTileKey = key;
    if (text.empty())
        return; // inactive: the overlay flags gate sampling, so the stale tile is never read, and
                // leaving the texture at capacity avoids a vkDeviceWaitIdle recreate on tool
                // toggles
    int cw = 0, ch = 0;
    const std::vector<std::uint8_t> rgba =
        rasterizeOverlayTile(text, withPill, scale, fontPx, capW, capH, cw, ch);
    m_renderer->setOverlayTile(rgba.data(), static_cast<std::uint32_t>(capW),
                               static_cast<std::uint32_t>(capH), static_cast<std::uint32_t>(cw),
                               static_cast<std::uint32_t>(ch));
}

// The selection tools' pointer: our own fine crosshair (ui::selectionCursor) instead of the
// chunky legacy FL_CURSOR_CROSS, carrying the boolean op as a +/-/x badge. While no gesture
// runs the badge follows the live modifiers (what a press right now would do); during a
// gesture it shows the latched op. Cached so the X11/Wayland cursor isn't re-set per event.
// The cursor codes this file uses (see updateToolCursor) mapped onto what a PEN can show. A tablet
// tool's cursor is named, not drawn: the custom art below (the selection-op badges, the pan hands,
// the reoriented rotate arrow) has no cursor-shape equivalent, so each collapses to the nearest
// named shape rather than to nothing. Only three distinctions actually matter to a user: gone (the
// brush), the hand (a pan), and an arrow (everything else).
Fl_Cursor VulkanCanvas::tabletCursorFor(int want) noexcept {
    switch (want) {
    case 20: // brush / inpaint: the reticle ring is the cursor, so there must not be a second one
        return FL_CURSOR_NONE;
    case 10: // the selection-move four-way arrows
    case 16: // pan, ready
    case 17: // pan, dragging
        return FL_CURSOR_MOVE;
    case 21: // the type tool's reoriented I-beam: the pen gets the named I-beam shape
        return FL_CURSOR_INSERT;
    case 22: // the fit-to-path hover hand: the pen gets the named pointing hand
        return FL_CURSOR_HAND;
    case 23: // the Zoom tool's magnifiers: no named "zoom" shape exists, and the pen still has to
    case 24: // aim at a point, so both collapse to the crosshair
        return FL_CURSOR_CROSS;
    default:
        break; // 15 (rotate) falls through to the arrow: no named shape reads as "rotate"
    }
    // 0..9 are core::SelectOp, whose badges all read as a crosshair; below 0 is the plain arrow.
    if (want >= 0 && want <= 9)
        return FL_CURSOR_CROSS;
    return FL_CURSOR_DEFAULT;
}

void VulkanCanvas::modifiersChanged() {
    updateToolCursor(m_pointerInside);
    // Ctrl over a brush tool swaps the reticle for the eyedropper loupe (and back on release) --
    // kick a frame so the swap shows NOW; a motionless pointer would otherwise wait out the
    // heartbeat. Scoped to the stroke family: no other tool's overlay changes with a modifier
    // alone (the op-badge cursors above are OS cursors, not present-pass overlays).
    if (strokeToolActive() && m_pointerInside)
        requestHostFrame();
}

void VulkanCanvas::updateToolCursor(bool inside) {
    // ⚠ Never read Fl::event_x/y here, and never move m_cursorLogical: this runs from KEYBOARD
    // events too -- our own modifier/Space/R key cases and the main window's modifier fan-out
    // (modifiersChanged) -- and a key event is delivered against the TOP-LEVEL window, so its
    // coordinates carry the canvas's origin inside the window baked in. Reading them here shifted
    // the tracked pointer (and with it the GPU brush reticle / eyedropper loupe) by exactly that
    // origin whenever a modifier key went down over a brush tool. The pointer events (FL_ENTER/
    // FL_MOVE/FL_DRAG/FL_PUSH and the tablet sink) own m_cursorLogical; every position-shaped
    // hover test below reads the tracked value instead. Since the eventLogicalPoint() choke point
    // this is ENFORCED rather than merely asked for: a key event never opens the pointer frame, so
    // even eventDocPoint() would answer with the tracked pointer from in here.
    m_pointerInside = inside;
    int want = -1; // the default arrow
    if (m_panning) {
        want = 17; // grabbing: a pan drag is under way (Space+drag, or middle mouse)
    } else if (m_spaceDown && inside) {
        want = 16; // grab: Space is held over the canvas, ready to pan
    } else if (inside && !m_rotateDown) {
        // The Image-menu preview outranks everything, because while it is staged the canvas is
        // modal to it: no tool receives a press (see handle()'s FL_PUSH gate), so no tool gets to
        // name the cursor either. Its own handles answer with the matching resize arrow; anywhere
        // else it answers -1, the plain arrow, which is the honest reading of a parked canvas.
        if (imageOpPreviewShowing()) {
            want = imageOpCursorState();
        } else if (const int dofCursor = dofCursorState(); dofCursor >= 0) {
            // The DoF focus-band gizmo (S33) owns the pointer over ANY tool -- the press claims its
            // handles ahead of every tool gesture (pushDofHandles), so the hover cursor must agree.
            // dofCursorState answers only for a latched DoF drag or an unclaimed-pointer hover.
            want = dofCursor;
        } else if (cloneAnchorModifier()) {
            want = 0; // S38: Ctrl (⌘) is held, so the next click picks the clone SOURCE -- a point,
                      // not a stroke. The crosshair says exactly that, and the size ring stands
                      // down for as long as the key is held (syncBrushReticle).
        } else if (strokeToolActive() || selectBrushToolActive() || edgeBrushToolActive() ||
                   redEyeToolActive() || eyedropperToolActive()) {
            want = 20; // brush/inpaint/select-brush/edge-brush/eye-retouch/eyedropper hide the OS
                       // pointer -- the reticle ring (brushes; for the eye tool it is literally the
                       // region a click corrects) or the loupe's centre cell IS the cursor
        } else if (activeSelectionKind().has_value()) {
            core::SelectOp op;
            if (m_selMove.dragging() || selectionMoveHover()) {
                want = 10; // S16-i: the four-way move arrows -- grabbing the ants, not the pixels
            } else if (m_gesture.active()) {
                op = m_gesture.op();
                want = static_cast<int>(op);
            } else {
                const auto s = Fl::event_state();
                op = selectOpForModifiers((s & FL_SHIFT) != 0, (s & FL_CTRL) != 0,
                                          (s & FL_ALT) != 0);
                want = static_cast<int>(op);
            }
        } else if (magicWandToolActive()) {
            // The op-badge crosshair, following the live modifiers -- same badges the marquee shows.
            const auto s = Fl::event_state();
            want = static_cast<int>(selectOpForModifiers((s & FL_SHIFT) != 0, (s & FL_CTRL) != 0,
                                                         (s & FL_ALT) != 0));
        } else if (moveToolActive()) {
            want = moveCursorState();
        } else if (cropToolActive()) {
            want = cropCursorState();
        } else if (warpToolActive()) {
            want = warpCursorState(); // the move arrows over a lattice handle, the arrow elsewhere
        } else if (shapeEditActive()) {
            want = shapeBoxCursorState(); // resize/move/rotate cursors over the selected shape's box
        } else if (penToolActive()) {
            want = 0; // the crosshair: a click places a node / grabs one (S28)
        } else if (typeToolActive()) {
            // While editing, the box controls (resize/move/rotate) own the cursor where the pointer is
            // over them; elsewhere fall to the I-beam over a block / the marquee crosshair over empty
            // canvas, where a click=Point / drag=Area CREATES a new block (§7, fixlist #2).
            const int boxCursor = textSessionActive() ? textBoxCursorState() : -1;
            if (boxCursor >= 0)
                want = boxCursor;
            else if (textBlockUnderPointer())
                want = 21; // the rotating I-beam: a click places the caret in that block
            else if (typePathSpineUnderPointer())
                want = 22; // the fit-to-path hand: a click flows text onto the path under the
                           // pointer -- the affordance finally has an indicator (user 2026-07-14)
            else
                want = 0; // the marquee crosshair: a click=Point / drag=Area creates a block
        } else if (zoomToolActive()) {
            // The "+" magnifier at rest -- that is what a left click does, and what the preview
            // box on the canvas is promising. It flips to "-" only while the right button is
            // actually held, because a right click is the only thing that zooms out and there is
            // no hovering it.
            want = m_zoomOutPressed ? 24 : 23;
        }
    }
    // The rotate cursor (15) reorients with the box, so its glyph changes while the state stays 15
    // -- handle it before the plain-state early-out (which would otherwise freeze the orientation).
    //
    // ⚠ The PEN still gets its named cursor on these two paths. They return before the
    // setToolCursor below, and the first cut forgot that -- so a pen arriving from the brush
    // (Hidden) stayed INVISIBLE over a rotate handle or the type canvas until some other state
    // change happened by. setToolCursor dedups on value, so the per-tick re-sends are free.
    if (want == 15) {
        applyRotateCursor();
        m_cursorState = 15;
        m_tablet.setToolCursor(tabletCursorFor(15));
        return;
    }
    if (want == 21) { // the I-beam reorients with the baseline; rebuild before the plain early-out
        applyTextCursor();
        m_cursorState = 21;
        m_tablet.setToolCursor(tabletCursorFor(21));
        return;
    }
    if (want == m_cursorState)
        return;
    m_cursorState = want;
    // The PEN's cursor is a separate thing on native Wayland. A tablet tool has no wl_pointer, so
    // none of the cursor() calls below reach it -- and a client that binds the tablet manager owns
    // its tool cursor, so one that never sets it shows whatever the compositor defaults to (KWin: a
    // crosshair, over every pixel of the app, which is exactly what Mosaic used to do). The state
    // resolved just above is the answer; hand it to the pen as well. The load-bearing case is the
    // brush -- hidden, because the GPU reticle ring IS the cursor.
    m_tablet.setToolCursor(tabletCursorFor(want));
    if (want < 0) {
        cursor(FL_CURSOR_DEFAULT);
        return;
    }
    if (want == 20) { // the brush: hide the OS pointer so only the GPU reticle ring shows (S19-a)
        cursor(FL_CURSOR_NONE);
        return;
    }
    if (want == 22) { // the Type tool's fit-to-path hover hand (static art, cached per scale)
        applyFitTextCursor();
        return;
    }
    if (want == 23 || want == 24) { // the Zoom tool's magnifier (23 = "+", 24 = "-")
        applyZoomCursor(/*out=*/want == 24);
        return;
    }
    if (want == 16 || want == 17) { // the pan-gesture hands (16 grab/open, 17 grabbing/closed)
        const std::size_t idx = want == 17 ? 1 : 0;
        if (!m_panCursorImages[idx]) {
            const int scale = cursorBuildScale() > 1.5 ? 2 : 1;
            m_panCursorPixels[idx] = panCursor(/*grabbing=*/want == 17, scale);
            m_panCursorImages[idx] = makeCursorImage(m_panCursorPixels[idx]);
        }
        if (m_panCursorImages[idx])
            cursor(m_panCursorImages[idx].get(), m_panCursorPixels[idx].logicalHotX,
                   m_panCursorPixels[idx].logicalHotY);
        else // rasterization failed: fall back to the nearest stock cursor
            cursor(want == 17 ? FL_CURSOR_MOVE : FL_CURSOR_HAND);
        return;
    }
    if (want >= 10) { // stock cursors: the Move tool's handles + the rotate gesture
        switch (want) {
        case 10:
            // The four-way move arrow. On WAYLAND our own art, for the same class of reason the
            // diagonal pair below carries theirs -- but a worse failure: FLTK asks for the Xcursor
            // name `move`, and breeze_cursors symlinks `move` -> `dnd-move`, a CLOSED GRABBING
            // HAND. So a mere hover over a Move-tool-selected layer said "you are dragging right
            // now", in a vocabulary that is not the Move tool's at all (a hand is the PAN gesture's
            // -- panCursor's open/closed pair). X11 resolves the same request to XC_fleur, the
            // four-way arrow this rebuilds, so the substitution stays Wayland-only.
            applyMoveCursor();
            break;
        case 11:
            cursor(FL_CURSOR_NS);
            break;
        case 12:
            cursor(FL_CURSOR_WE);
            break;
        case 13:
        case 14: {
            // The two DIAGONAL resize arrows (S59-a). FLTK's Wayland backend resolves the stock
            // FL_CURSOR_NWSE / FL_CURSOR_NESW by their legacy Xcursor names -- `fd_double_arrow` /
            // `bd_double_arrow` -- which a theme is free not to ship, and breeze_cursors (the KDE
            // default) ships NEITHER. On a miss FLTK installs a built-in 15x15 XPM with a
            // dead-centre (7,7) hotspot, inside a theme whose real arrows are 24 px and point from
            // near their top-left corner: the handle cursor then sits several px from where it
            // appears to point. So substitute our own art -- but only there. X11 cannot miss
            // (XCreateFontCursor always answers), and swapping unconditionally would change the
            // resize cursors for every X11 user.
            //
            // That art IS the rotate cursor's, baked to +-45 deg -- buckets 8 and 56 of the 64
            // applyRotateCursor quantises to -- so it shares that cursor's cache outright. Safe in
            // both directions: applyRotateCursor's early-out also demands m_cursorState == 15, and
            // arriving there from here means the state was 13/14, so a genuine rotate rebuild can
            // never be swallowed by a bucket this block parked.
            if (platform::activeBackend() == platform::WindowSystem::Wayland) {
                const bool dark = activePalette().dark;
                const int bucket = want == 13 ? 8 : 56; // +pi/4 : -pi/4, in rotate-cursor buckets
                if (bucket != m_rotateCursorBucket || dark != m_rotateCursorDark ||
                    !m_rotateCursorImage) {
                    m_rotateCursorPixels = want == 13 ? nwseCursor(dark, cursorBuildScale())
                                                      : neswCursor(dark, cursorBuildScale());
                    m_rotateCursorBucket = bucket;
                    m_rotateCursorDark = dark;
                    m_rotateCursorImage = makeCursorImage(m_rotateCursorPixels);
                }
                if (m_rotateCursorImage) {
                    cursor(m_rotateCursorImage.get(), m_rotateCursorPixels.logicalHotX,
                           m_rotateCursorPixels.logicalHotY);
                    break;
                }
                m_rotateCursorBucket = -1; // build failed: never cache the miss
            }
            cursor(want == 13 ? FL_CURSOR_NWSE : FL_CURSOR_NESW);
            break;
        }
        default:
            cursor(FL_CURSOR_HAND);
            break; // 15 (rotate) is handled above; this is a safety net
        }
        return;
    }
    const auto slot = static_cast<std::size_t>(want);
    if (!m_cursorImages[slot]) {
        // Built once per op, at 2x when the surface is HiDPI (the content scale is known by
        // now -- the pointer is over a realized window).
        const int scale = cursorBuildScale() > 1.5 ? 2 : 1;
        m_cursorPixels[slot] = selectionCursor(static_cast<core::SelectOp>(want), scale);
        m_cursorImages[slot] = makeCursorImage(m_cursorPixels[slot]);
    }
    if (m_cursorImages[slot])
        cursor(m_cursorImages[slot].get(), m_cursorPixels[slot].logicalHotX,
               m_cursorPixels[slot].logicalHotY);
    else
        cursor(FL_CURSOR_CROSS); // rasterization failed: the stock crosshair
}

// Build + set the rotate cursor (state 15) -- the direction the straight arrow points along is the
// glyph's orientation (rotateCursor bakes it into the art). While DRAGGING a rotation the arrow is
// tangent to the box centre, so it tracks the actual rotation smoothly. On HOVER it sweeps the 90
// deg between the two box edges meeting at the nearest corner, lying straight along an edge at each
// end of that wedge (user 2026-06-17). Quantised to kRotateBuckets so the glyph + OS cursor only
// rebuild when the orientation (or theme) actually changes.
void VulkanCanvas::applyRotateCursor() {
    double angle = 0.0;
    std::array<common::Vec2, 4> corners{};
    // The rotate cursor reorients to the box it sits on: the Move/Shape selection box, or -- while the
    // Type tool is editing -- the text-edit box (so rotating text reads correctly, like the I-beam).
    const bool textBoxRotate = m_textBox.active() && m_textBox.mode() == TransformMode::Rotate;
    // The DoF gizmo's rotate knob is not on a box, so it gets its own orbit: the arrow is tangent to
    // the circle about the CENTRE (move) knob, so it reads as "spin about here" like the Move box's
    // rotate band. Dragging tracks the cursor's angle about the centre (smooth, the box-drag lesson);
    // hovering sits at the knob's own angular position. Claimed ahead of any box below -- the DoF
    // handles outrank tool gestures (dofCursorState is consulted first), so this must match.
    // (Handle 1 is the RADIUS knob on the Vignette ring, which spins nothing -- dofCursorState
    // answers a resize arrow there, so this must agree and leave the ring alone.)
    const bool dofRotate = (m_dofDrag.active && m_dofDrag.handle == 1 &&
                            m_dofDrag.press.kind != BlurGizmoKind::Ring) ||
                           (!m_dofDrag.active && hitDofHandle(m_cursorLogical) == 1);
    // The Type box reads the ROTATE corners (the solid's extent for 3D text) so the hover wedge
    // sweeps the same corners the band actually hits, not the cap-projected box.
    if (dofRotate) {
        DofGizmoState st;
        DofScreenGeom g;
        if (dofScreenGeom(st, g) && st.kind != BlurGizmoKind::Ring) {
            const common::Vec2 c = g.center;
            const common::Vec2 p = m_dofDrag.active ? m_cursorLogical : g.rotateKnob;
            angle = std::atan2(p.y - c.y, p.x - c.x) + kPi / 2.0; // +90deg: tangent, not radial
        }
    } else if (moveTargetCorners(corners) || (typeToolActive() && textRotateCorners(corners)) ||
        (cropToolActive() && m_cropRect && cropCorners(corners))) { // the crop box (S16-f rotate)
        const common::Vec2 p = m_cursorLogical; // the tracked pointer (see updateToolCursor)
        if ((m_transform.active() && m_transform.mode() == TransformMode::Rotate) ||
            textBoxRotate || m_cropRotating) {
            // Dragging: track the actual rotation about the box centre (tangent), not the handle --
            // anchoring to the moving handle made the cursor snap/jitter mid-drag (user
            // 2026-06-17).
            const common::Vec2 c = (corners[0] + corners[1] + corners[2] + corners[3]) * 0.25;
            angle = std::atan2(p.y - c.y, p.x - c.x) + kPi / 2.0;
        } else {
            // Hover: the arrow sweeps the 90 deg wedge OUTSIDE the nearest corner, lying straight
            // along each of the two box edges at the wedge ends -- so the rotation reads at a
            // glance.
            int ni = 0;
            double best = (p - corners[0]).length();
            for (int i = 1; i < 4; ++i) {
                const double d = (p - corners[i]).length();
                if (d < best) {
                    best = d;
                    ni = i;
                }
            }
            const common::Vec2 corner = corners[ni];
            const common::Vec2 e1 = corner - corners[(ni + 1) % 4]; // outward along one edge
            const common::Vec2 e2 = corner - corners[(ni + 3) % 4]; // outward along the other
            const double a1 = std::atan2(e1.y, e1.x);
            double span =
                std::atan2(e2.y, e2.x) - a1; // signed shortest arc between the edges (~+-90)
            while (span > kPi)
                span -= 2.0 * kPi;
            while (span < -kPi)
                span += 2.0 * kPi;
            const common::Vec2 w = p - corner;
            double dw = std::atan2(w.y, w.x) - a1;
            while (dw > kPi)
                dw -= 2.0 * kPi;
            while (dw < -kPi)
                dw += 2.0 * kPi;
            const double t = std::abs(span) > 1e-9 ? std::clamp(dw / span, 0.0, 1.0) : 0.0;
            // +90deg: aim the arrow TANGENT to the sweep -- without it the arrow pointed straight
            // at the handle (radial) instead of along the rotation (user 2026-06-17).
            angle = a1 + span * t + kPi / 2.0;
        }
    }
    const bool dark = activePalette().dark;
    constexpr int kRotateBuckets = 64;
    int bucket =
        static_cast<int>(std::lround(angle / (2.0 * kPi) * kRotateBuckets)) % kRotateBuckets;
    if (bucket < 0)
        bucket += kRotateBuckets;
    if (m_cursorState == 15 && bucket == m_rotateCursorBucket && dark == m_rotateCursorDark &&
        m_rotateCursorImage)
        return; // same orientation + theme already shown
    m_rotateCursorBucket = bucket;
    m_rotateCursorDark = dark;
    // Rasterize at the true device scale (not a 1x/2x step) so it stays crisp at fractional HiDPI.
    m_rotateCursorPixels =
        rotateCursor(bucket * (2.0 * kPi) / kRotateBuckets, dark, cursorBuildScale());
    m_rotateCursorImage = makeCursorImage(m_rotateCursorPixels);
    if (!m_rotateCursorImage) {
        cursor(FL_CURSOR_HAND); // build failed: nearest stock fallback
        return;
    }
    cursor(m_rotateCursorImage.get(), m_rotateCursorPixels.logicalHotX,
           m_rotateCursorPixels.logicalHotY);
}

void VulkanCanvas::applyTextCursor() {
    // Angle the I-beam to the local baseline projected to the screen: the text layer UNDER THE
    // POINTER first -- a click there is what the caret would land in (pushTypeTool routes by the
    // topmost hit), so the cursor must angle to THAT block, not the one being edited ("the rotating
    // I-beam anchors on the active text layer when hovering over another text layer", user
    // 2026-07-14). Only between blocks does it fall back to the edited layer (keeping the session's
    // angle rather than snapping upright at every gap), else upright. The baseline runs along layer
    // +x for horizontal text but DOWN THE COLUMN (+y) for a vertical block, so the I-beam lies on
    // its side there -- the vertical-type convention.
    double angle = 0.0;
    core::Layer* l = nullptr;
    if (m_typeHost.document) {
        if (core::Document* doc = m_typeHost.document())
            l = core::topmostTextLayerAt(doc->root(), cursorDocPoint(), 4.0);
    }
    if (l == nullptr)
        l = textEditLayer();
    if (l != nullptr) {
        auto* tl = l->as<core::TextLayer>();
        const bool vertical = tl != nullptr && tl->block().writingMode !=
                                                   core::text::WritingMode::HorizontalTB;
        common::Vec2 axis = vertical ? common::Vec2{0.0, 1.0} : common::Vec2{1.0, 0.0};
        // Bent baseline (§9): tilt the I-beam to the local arch tangent UNDER THE POINTER, so it stays
        // perpendicular to the curved baseline (hover and select-drag alike). The tangent at parameter
        // t is (W, 4·h·(1-2t)) with h = -bend·½·W; W/x0 approximated from the content bounds (exact
        // enough for a cursor, and works for a hovered block without re-shaping it).
        if (tl != nullptr && !vertical && tl->block().bend != 0.0f && !tl->block().extrude) {
            if (const std::optional<common::Rect> cb = tl->contentBounds(); cb && cb->w > 1e-3) {
                const std::optional<common::Affine2D> winv = core::worldTransform(*l).inverse();
                const common::Vec2 pl =
                    winv ? winv->apply(cursorDocPoint()) : common::Vec2{cb->x + cb->w * 0.5, cb->y};
                const double t = std::clamp((pl.x - cb->x) / cb->w, 0.0, 1.0);
                const double h = -static_cast<double>(tl->block().bend) * 0.5 * cb->w;
                axis = {cb->w, 4.0 * h * (1.0 - 2.0 * t)};
            }
        } else if (tl != nullptr && tl->block().extrude) {
            // 3D: tilt the I-beam with the projected flow axis at the block's centre (a finite
            // difference through the front-cap plane map), so the cursor lies on the solid's face.
            if (const std::optional<common::Rect> cb = tl->contentBounds(); cb && !cb->empty()) {
                const auto pmap = core::text::ExtrudePlaneMap::from(*cb, *tl->block().extrude);
                const common::Vec2 c{cb->x + cb->w * 0.5, cb->y + cb->h * 0.5};
                const common::Vec2 d = pmap.project({c.x + axis.x, c.y + axis.y}) - pmap.project(c);
                if (d.length() > 1e-9) axis = d;
            }
        }
        const common::Vec2 dir =
            m_view.docToScreen().applyVector(core::worldTransform(*l).applyVector(axis));
        if (dir.length() > 1e-9)
            angle = std::atan2(dir.y, dir.x);
    }
    const bool dark = activePalette().dark;
    constexpr int kBuckets = 64;
    int bucket = static_cast<int>(std::lround(angle / (2.0 * kPi) * kBuckets)) % kBuckets;
    if (bucket < 0)
        bucket += kBuckets;
    if (m_cursorState == 21 && bucket == m_textCursorBucket && dark == m_textCursorDark &&
        m_textCursorImage)
        return; // same orientation + theme already shown
    m_textCursorBucket = bucket;
    m_textCursorDark = dark;
    m_textCursorPixels = textCursor(bucket * (2.0 * kPi) / kBuckets, dark, cursorBuildScale());
    m_textCursorImage = makeCursorImage(m_textCursorPixels);
    if (!m_textCursorImage) {
        cursor(FL_CURSOR_INSERT); // build failed: the stock I-beam
        return;
    }
    cursor(m_textCursorImage.get(), m_textCursorPixels.logicalHotX,
           m_textCursorPixels.logicalHotY);
}

// Build + set the four-way move cursor (state 10). Static art -- it varies only with the theme and
// the build scale -- so one cached bitmap serves the whole session. Wayland only: on X11 the stock
// FL_CURSOR_MOVE already resolves to XC_fleur, and substituting there would change the cursor for
// every X11 user for no gain (the S59-a rule for nwse/nesw, applied to the same class of problem).
// The decision, the cache and the stock fallback all live in ui::MoveCursor, which the two gizmo
// panes and the Export preview share -- the canvas differs only in the build scale, which follows
// its own content scale rather than the window's (cursorBuildScale carries the macOS pin).
void VulkanCanvas::applyMoveCursor() {
    m_moveCursor.apply(this, activePalette().dark, cursorBuildScale());
}

void VulkanCanvas::applyFitTextCursor() {
    const int scale = cursorBuildScale() > 1.5 ? 2 : 1;
    if (!m_fitTextCursorImage || scale != m_fitTextCursorScale) {
        m_fitTextCursorPixels = fitTextCursor(scale);
        m_fitTextCursorScale = scale;
        m_fitTextCursorImage = makeCursorImage(m_fitTextCursorPixels);
    }
    if (m_fitTextCursorImage)
        cursor(m_fitTextCursorImage.get(), m_fitTextCursorPixels.logicalHotX,
               m_fitTextCursorPixels.logicalHotY);
    else
        cursor(FL_CURSOR_HAND); // rasterization failed: the stock pointing hand
}

// Build + set a magnifier (states 23 in / 24 out). The pair is cached together on the two things
// the art varies with -- the theme two-tone and the build scale -- so pressing and releasing the
// right button swaps between two ready bitmaps instead of rasterizing an SVG each way.
void VulkanCanvas::applyZoomCursor(bool out) {
    const bool dark = activePalette().dark;
    const double scale = cursorBuildScale();
    if (m_zoomCursorScale != scale || m_zoomCursorDark != dark) {
        m_zoomCursorImages[0].reset();
        m_zoomCursorImages[1].reset();
        m_zoomCursorScale = scale;
        m_zoomCursorDark = dark;
    }
    const std::size_t idx = out ? 1 : 0;
    if (!m_zoomCursorImages[idx]) {
        m_zoomCursorPixels[idx] = zoomCursor(out, dark, scale);
        m_zoomCursorImages[idx] = makeCursorImage(m_zoomCursorPixels[idx]);
    }
    if (m_zoomCursorImages[idx])
        cursor(m_zoomCursorImages[idx].get(), m_zoomCursorPixels[idx].logicalHotX,
               m_zoomCursorPixels[idx].logicalHotY);
    else
        cursor(FL_CURSOR_CROSS); // rasterization failed: the nearest stock "aim at a point"
}

void VulkanCanvas::renderFrame() {
    ensureRenderer();
    if (!m_renderer)
        return;
    if (m_gesture.active() && m_gesture.previewDirty()) {
        // Live gesture preview, coalesced to the frame (drag events only mark it dirty), straight
        // to the canvas, never the command stack. LASSOS draw their path as a smooth inverted line
        // overlay (syncLassoOverlay) instead of a doc-pixel stroked mask (which staircased at
        // angles)
        // -- so for them we leave the committed selection's mask loaded (its ants keep animating
        // underneath). Only rect/ellipse rasterize the *combined* result so Subtract/Intersect read
        // live.
        const SelectionGesture::Kind kind = m_gesture.kind();
        if (kind != SelectionGesture::Kind::FreeLasso &&
            kind != SelectionGesture::Kind::PolyLasso) {
            const common::Vec2 size = m_view.documentSize();
            const double stroke = 1.0 / std::max(m_view.zoom(), CanvasView::kMinZoom);
            const core::Selection p =
                m_gesture.preview(baseSelection(), static_cast<std::uint32_t>(size.x),
                                  static_cast<std::uint32_t>(size.y), stroke);
            setSelectionMask(p.width(), p.height(), p.data().empty() ? nullptr : p.data().data());
        }
        m_gesture.clearPreviewDirty();
    }
    if (m_selMoveDirty) {
        // S16-i: the moved outline, rebuilt at most once per frame (drag events only set the flag).
        // Straight to the canvas mask -- the command stack sees one step, on release.
        const core::Selection p = m_selMove.current();
        setSelectionMask(p.width(), p.height(), p.data().empty() ? nullptr : p.data().data());
        m_selMoveDirty = false;
    }
    if (m_maskStrokeActive && m_maskStrokePreviewDirty) {
        // S18 select brush: the live combined mask, rebuilt at most once per frame (drag events only
        // mark it dirty), straight to the canvas -- the command stack sees nothing until release.
        const core::Selection preview = core::Selection::combine(
            baseSelection(), m_maskStroke.toSelection(), m_selectBrushOp);
        setSelectionMask(preview.width(), preview.height(),
                         preview.data().empty() ? nullptr : preview.data().data());
        m_maskStrokePreviewDirty = false;
    }
    if (m_redEyeStrokeActive && m_redEyeStrokePreviewDirty) {
        // S38-b eye retouch: the live preview is the RAW painted scope, added to the document's own
        // selection so the user can see exactly where the correction will land. No corrected pixel
        // is ever shown before release -- the correction runs once, in the host.
        const core::Selection preview = core::Selection::combine(
            baseSelection(), m_redEyeStroke.toSelection(), core::SelectOp::Add);
        setSelectionMask(preview.width(), preview.height(),
                         preview.data().empty() ? nullptr : preview.data().data());
        m_redEyeStrokePreviewDirty = false;
    }
    if (m_edgeStrokeActive && m_edgeStrokePreviewDirty) {
        // L1 edge brush: the live preview is the RAW painted trail combined with the base -- the
        // stroke's own sample pixels, exactly what the S18 brush shows. The edge-stopped grow is
        // deliberately NOT computed or shown here: it runs once, on release -- no grown region
        // ever appears during the unbroken stroke. That ordering is an invariant of the tool.
        const core::Selection preview = core::Selection::combine(
            baseSelection(), m_edgeStroke.toSelection(), m_edgeBrushOp);
        setSelectionMask(preview.width(), preview.height(),
                         preview.data().empty() ? nullptr : preview.data().data());
        m_edgeStrokePreviewDirty = false;
    }
    // S15-f: the Move tool's empty-space band, rasterized into the SAME overlay lane (drag events
    // only mark it dirty), so it wears the user's overlay line style and its ants crawl like any
    // other selection outline. Transient: finish/cancelLayerMarquee restore the document's mask.
    syncLayerMarqueeMask();
    syncZoomPreview();
    if (m_documentPending) {
        m_renderer->setCanvasImage(m_documentImage);
        m_documentImage = common::Image{}; // the renderer copied it
        m_documentPending = false;
        m_documentRegionPending = false;   // superseded by the full upload
        m_documentRegion = common::Image{};
    } else if (m_documentRegionPending) {
        m_renderer->setCanvasRegion(m_documentRegion, m_documentRegionX, m_documentRegionY);
        m_documentRegion = common::Image{}; // the renderer copied it
        m_documentRegionPending = false;
    }
    if (m_selectionPending) {
        m_renderer->setSelectionMask(m_selectionW, m_selectionH,
                                     m_selectionMask.empty() ? nullptr : m_selectionMask.data());
        m_selectionMask = {}; // the renderer copied it
        m_selectionPending = false;
    }
    // The ants crawl off the wall clock; wrap in double precision so the shader's float phase
    // stays exact over long sessions.
    m_renderer->setAntsPhase(static_cast<float>(
        std::fmod(nowSeconds() * kAntsSpeedPxPerSec, render::kAntsDashPeriodPx)));
    m_renderer->setView(m_view.docToScreen(), m_contentScale);
    syncMoveOverlay();   // the Move tool's handles follow the layer + view each frame (S15)
    syncDofOverlay();    // ... and the DoF focus-band gizmo, while a DofBlur layer is active (S33)
    syncGuidesOverlay(); // ... and the document guides + smart-guide lines (View -> Guides, b.13)
    syncCropOverlay();  // ... and the crop rect follows the view (S16)
    syncSmartChips();   // ... and the keep-region chips, while Smart Resize is on (S16-f)
    syncSampleArea();   // ... and the inpaint sample-area wash, while a run is active (S39)
    syncLassoOverlay(); // ... and the in-flight lasso/poly path (smooth inverted line)
    syncShapeOverlay(); // ... and the in-flight shape's wireframe, on that same channel (S26-c)
    syncPenOverlay();   // ... and the pen path's spine, on that same channel again (S28)
    syncCloneOverlay(); // ... and the clone stamp's source marker, likewise on that channel (S38)
    syncPenChrome();    // ... and the pen's nodes / handles / stems, on their own lane (b.6)
    // S35-b: the warp lattice rides the polyline lane and its handle squares ride the PEN's lane, so
    // it must run AFTER syncPenChrome -- which unconditionally clears that lane when the Pen is not
    // up, and would otherwise wipe the handles we had just placed.
    flushWarpDrag();    // ... one draft bake per frame tick, however fast the pointer moved
    syncWarpOverlay();
    syncBrushReticle(m_pointerInside); // ... and the brush size ring at the cursor (S19-a)
    syncLoupe(m_pointerInside);        // ... and the eyedropper's magnifier loupe at the cursor (S24)
    updateTextBlink();                 // ... advance the Type-tool caret blink (S29-b)
    syncTextOverlay();                 // ... and hand the caret bar + selection quads to the renderer
    // Show the rotation dial while R is held (S8-b), or briefly after a double-tap-R reset.
    const bool dialActive = m_rotateDown || nowSeconds() < m_dialResetUntil;
    m_renderer->setRotationOverlay(dialActive, m_view.rotation());
    updateOverlayTile(); // rasterize the dial/HUD text tile (real UI font) when it changes (S16)
    updateIdleField();   // ... and the documentless idle pass's fade state + atlas (empty state)
#ifdef MOSAIC_DEBUG
    m_renderer->setFpsOverlay(m_fpsShow, m_fpsValue); // the Help-menu FPS diagnostic (top-right)
#endif
    std::string err;
    if (!m_renderer->drawFrame(m_clearColor, err)) {
        uiLog().warn("drawFrame error: {}", err);
    }
}

// ---- S29-b Type tool: on-canvas authoring + editing -----------------------------------------
namespace {
constexpr double kCaretBlinkSec = 0.53;     // caret on/off half-period
constexpr double kTextPickPadDoc = 4.0;     // select-to-edit hit padding around a block, doc px
constexpr double kTextPathPickPx = 6.0;     // fit-to-path: click-near-a-spine pickup, SCREEN px (§9)
}  // namespace

bool VulkanCanvas::typeToolActive() const {
    return m_tools != nullptr && m_tools->active() == ToolId::Text;
}

bool VulkanCanvas::textBlockUnderPointer() const {
    if (!m_typeHost.document)
        return false;
    core::Document* doc = m_typeHost.document();
    return doc != nullptr &&
           core::topmostTextLayerAt(doc->root(), cursorDocPoint(), kTextPickPadDoc) != nullptr;
}

bool VulkanCanvas::typePathSpineUnderPointer() const {
    core::Document* doc = m_typeHost.document ? m_typeHost.document() : nullptr;
    if (doc == nullptr)
        return false;
    // The SAME pickup the click uses (pushTypeTool's fit-to-path entry): the topmost vector layer
    // whose spine passes within kTextPathPickPx screen px -- so the hover hand shows exactly where
    // a click would flow text onto a path, and nowhere else.
    return core::topmostVectorSpineAt(doc->root(), cursorDocPoint(),
                                      kTextPathPickPx / std::max(1e-6, m_view.zoom())) != nullptr;
}

core::Layer* VulkanCanvas::textEditLayer() const {
    if (m_textEditTarget == core::kInvalidLayerId || !m_typeHost.document)
        return nullptr;
    core::Document* doc = m_typeHost.document();
    return doc != nullptr ? doc->find(m_textEditTarget) : nullptr;
}

const core::text::TextBlock* VulkanCanvas::textEditBlock() const {
    core::Layer* l = textEditLayer();
    auto* tl = l != nullptr ? l->as<core::TextLayer>() : nullptr;
    return tl != nullptr ? &tl->block() : nullptr;
}

int VulkanCanvas::textResizeCorner() const {
    // Which Type-edit-box corner carries the solid resize handle (TL,TR,BR,BL order). BR for
    // horizontal text; a vertical POINT block moves it to BL so the handle stays joined to the
    // left-edge side baseline (the vertical stand-in for the bottom underline -- user 2026-07-02).
    // A vertical AREA box keeps BR: its full frame touches every corner, and BR-anchored box
    // resizing is unchanged by the writing mode.
    const core::text::TextBlock* b = textEditBlock();
    if (b != nullptr && b->frame == core::text::TextFrame::Point &&
        b->writingMode != core::text::WritingMode::HorizontalTB)
        return 3;
    return 2;
}

void VulkanCanvas::setTextMisspelledRanges(std::vector<core::text::MisspelledRange> ranges) {
    m_textMisspelled = std::move(ranges);
}

const core::text::ShapedBlock& VulkanCanvas::ensureTextShaped() {
    core::Layer* l = textEditLayer();
    auto* tl = l != nullptr ? l->as<core::TextLayer>() : nullptr;
    if (tl == nullptr) {
        m_textShaped = {};
        m_textShapedRev = static_cast<std::uint64_t>(-1);
        return m_textShaped;
    }
    if (m_textShapedRev != tl->contentRevision() || m_textShaped.glyphs.empty()) {
        if (m_typeHost.layout)
            m_textShaped = m_typeHost.layout(tl->block());
        m_textShapedRev = tl->contentRevision();
    }
    return m_textShaped;
}

common::Vec2 VulkanCanvas::textDocToLocal(common::Vec2 docPt) {
    core::Layer* l = textEditLayer();
    if (l == nullptr)
        return docPt;
    const std::optional<common::Affine2D> inv = core::worldTransform(*l).inverse();
    common::Vec2 p = inv ? inv->apply(docPt) : docPt;
    // 3D (S30-d round 2): the pointer hit the PROJECTED solid; ray-cast back through the
    // front-cap plane so the caret/selection land on the glyph the user actually clicked. Every
    // text hit-test funnels through here, so one mapping serves click, drag, double-click and the
    // spell menu alike. ensureTextShaped keeps the plane's pivot basis current mid-edit.
    const core::text::TextBlock* b = textEditBlock();
    if (b != nullptr && b->extrude) {
        const core::text::ShapedBlock& sh = ensureTextShaped();
        if (!sh.bounds.empty())
            if (const auto flat =
                    core::text::ExtrudePlaneMap::from(sh.bounds, *b->extrude).unproject(p))
                p = *flat;
    }
    return p;
}

std::optional<core::text::ExtrudePlaneMap> VulkanCanvas::textPlaneMap() const {
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr || !b->extrude || m_textShaped.bounds.empty() || m_textShaped.glyphs.empty())
        return std::nullopt;
    return core::text::ExtrudePlaneMap::from(m_textShaped.bounds, *b->extrude);
}

void VulkanCanvas::enterTextEdit(core::LayerId id, std::optional<std::size_t> caret) {
    m_textEditTarget = id;
    if (m_typeHost.selectLayer)
        m_typeHost.selectLayer(id); // the panel's active row follows create AND select-to-edit (#4)
    m_textShapedRev = static_cast<std::uint64_t>(-1); // force a re-layout for the new block
    (void)ensureTextShaped();                         // for the side effect: warm the shape cache
    const core::text::TextBlock* b = textEditBlock();
    const std::size_t pos = caret.value_or(b != nullptr ? b->utf8.size() : 0);
    m_textSel.collapseTo(pos);
    m_textDesiredInline = -1.0;
    ++m_textEditCoalesce; // the session's first edit is a fresh undo step
    m_textBlinkOn = true;
    m_textBlinkAt = nowSeconds() + kCaretBlinkSec;
    take_focus(); // the canvas must own the keyboard so keystrokes reach the session, not the panel
    // IME: enabling the seat's text-input belongs here (docs/type-tool.md §6.2). Left to the
    // focus-scoped spike (it must not break the Fl_Input fields' IME); ASCII typing works now.
    if (m_typeHost.recomposite)
        m_typeHost.recomposite(); // re-render this block UNCLIPPED so its Area overflow shows now (#3)
    requestHostFrame();
}

void VulkanCanvas::selectLayerForActiveTool(core::LayerId id) {
    if (id == core::kInvalidLayerId)
        return;
    if (moveToolActive()) {
        setSingleMoveTarget(id); // the Move tool frames the carried-over layer
    } else if (typeToolActive()) {
        // The Type tool re-enters editing on a text layer (so its box + caret are ready immediately);
        // a non-text active layer is left alone (you'd click to create new text instead).
        core::Document* doc = m_typeHost.document ? m_typeHost.document() : nullptr;
        core::Layer* l = doc != nullptr ? doc->find(id) : nullptr;
        if (l != nullptr && l->as<core::TextLayer>() != nullptr && id != m_textEditTarget)
            enterTextEdit(id, std::nullopt);
    } else if (gradientToolActive()) {
        // The Gradient tool re-opens editing on a gradient layer (its handles show at once, S22); a
        // non-gradient active layer just clears the target (a drag then authors a fresh gradient).
        bindGradientEditToActiveLayer(id);
    } else if (warpToolActive()) {
        // S35-b, the same re-entry: switching to a warp tool with a pixel layer active arms the
        // lattice at once -- restored from the layer's stored grid when it has one -- so the handles
        // are there to grab instead of appearing only after a first click. A layer that cannot be
        // warped says why, by name, rather than leaving an empty canvas to be puzzled over.
        bindWarpToActiveLayer(id);
    } else if (penToolActive()) {
        // S28, the same re-entry: switching to the Pen with a PATH layer active binds it for node
        // editing, so its nodes show immediately instead of after a hunt-and-click. A parametric
        // shape or a gradient is left alone -- those belong to the other two bars (penToolBinds).
        core::Document* doc = m_penHost.document ? m_penHost.document() : nullptr;
        core::Layer* l = doc != nullptr ? doc->find(id) : nullptr;
        auto* vl = l != nullptr ? l->as<core::VectorLayer>() : nullptr;
        if (vl != nullptr && vl->hasObject() && penToolBinds(*vl->object()) &&
            id != m_penEditTarget) {
            m_penEditTarget = id;
            m_penSel = PenSelection{};
            ++m_penEditCoalesce; // a fresh bind opens a new undo step
            reflectPenOptions(*vl->object());
            requestHostFrame();
        }
    }
}

void VulkanCanvas::beginTextEditFromMove(core::LayerId id, common::Vec2 docPt) {
    if (m_tools == nullptr)
        return;
    // Switching the active tool fires the host's onToolChanged, which clears the move handles and
    // commits any prior session (none here). enterTextEdit then opens the session AFTER that cleanup.
    m_tools->setActive(ToolId::Text);
    enterTextEdit(id, std::nullopt);
    if (const core::text::TextBlock* b = textEditBlock())
        m_textSel.collapseTo(core::text::hitTest(ensureTextShaped(), *b, textDocToLocal(docPt)));
    requestHostFrame();
}

void VulkanCanvas::commitTextEdit() {
    if (!textSessionActive())
        return;
    clearStylePreview(); // revert any live font-hover preview so it never becomes the committed block
    const core::LayerId id = m_textEditTarget;
    m_textEditTarget = core::kInvalidLayerId;
    m_textSelecting = false;
    m_textCreating = false;
    m_textBox.cancel(); // drop any in-flight box gesture (its last drag already landed as an edit)
    m_textBoxCtl = TextBoxControl::None;
    m_textShaped = {};
    m_textShapedRev = static_cast<std::uint64_t>(-1);
    if (m_renderer) {
        m_renderer->setTextOverlay(false, {}, {}, {});
        m_renderer->setTextBendHandle(false, {}, {});
    }
    if (m_typeHost.finishText)
        m_typeHost.finishText(id); // drops the layer if the block ended empty (§6)
    if (m_typeHost.recomposite)
        m_typeHost.recomposite(); // re-clip the (kept) block's Area overflow to its box now (#3)
    requestHostFrame();
}

void VulkanCanvas::applyTextEdit(core::text::TextBlock next, bool newUndoStep) {
    if (!textSessionActive() || !m_typeHost.editText)
        return;
    if (newUndoStep)
        ++m_textEditCoalesce;
    m_typeHost.editText(m_textEditTarget, std::move(next), m_textEditCoalesce);
    // ⚠ NO eager ensureTextShaped() here (it used to re-layout per EVENT). Every consumer of the
    // shaped block -- the frame's syncTextOverlay, caret geometry, hit tests -- already goes
    // through ensureTextShaped(), which re-lays out lazily off contentRevision. A pen drags the
    // bend handle at ~200 events/s; a layout per event was a solid slice of "bending text is
    // extremely laggy" (user 2026-07-14), all of it thrown away unread between frames.
    m_textBlinkOn = true;
    m_textBlinkAt = nowSeconds() + kCaretBlinkSec;
    requestHostFrame();
}

void VulkanCanvas::insertTextAtCaret(const std::string& utf8) {
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr)
        return;
    core::text::TextBlock next = *b;
    // A fresh (empty) block inherits its `emptyStyle` (seeded at create time from the tool defaults),
    // which styleAt() returns and which the caret height already reflects -- so the first glyph
    // matches the caret you see (fixlist #3). No empty-block special-case needed.
    const std::size_t caret = core::text::replaceText(next, m_textSel.lo(), m_textSel.hi(), utf8);
    applyTextEdit(std::move(next), /*newUndoStep=*/false);
    m_textSel.collapseTo(caret);
    m_textDesiredInline = -1.0;
}

// The byte range the bar/panel edits target: the selection when there is one, else the WHOLE block.
// A bare caret means "the whole layer" (user 2026-06-30): selecting a Type layer auto-enters edit at
// a caret, so font/size/colour changes then must affect the whole object, not silently wait for the
// next typed run. To restyle only part, select that range.
std::pair<std::size_t, std::size_t> VulkanCanvas::textEditRange() const {
    const core::text::TextBlock* b = textEditBlock();
    const std::size_t n = b != nullptr ? b->utf8.size() : 0;
    return m_textSel.empty() ? std::pair{std::size_t{0}, n}
                             : std::pair{m_textSel.lo(), m_textSel.hi()};
}

core::text::CommonStyle VulkanCanvas::selectionStyle() const {
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr)
        return {};
    const auto [lo, hi] = textEditRange(); // whole block at a bare caret; an empty block -> emptyStyle
    return core::text::commonStyle(*b, lo, hi);
}

core::text::CommonParagraph VulkanCanvas::selectionParagraph() const {
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr)
        return {};
    const auto [lo, hi] = textEditRange();
    return core::text::commonParagraph(*b, lo, hi);
}

void VulkanCanvas::applySelectionStyle(const std::function<void(core::text::CharStyle&)>& mutate,
                                       bool coalesce) {
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr)
        return;
    core::text::TextBlock next = *b;
    if (next.utf8.empty()) {
        mutate(next.emptyStyle); // no text yet: seed the empty/first-char style (caret + first glyph)
    } else {
        const auto [lo, hi] = textEditRange(); // bare caret -> the whole layer; else the selection
        core::text::mutateStyleRange(next, lo, hi, mutate);
    }
    applyTextEdit(std::move(next), /*newUndoStep=*/!coalesce);
}

void VulkanCanvas::applySelectionParagraph(
    const std::function<void(core::text::Paragraph&)>& mutate, bool coalesce) {
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr)
        return;
    core::text::TextBlock next = *b;
    const auto [lo, hi] = textEditRange(); // bare caret -> every paragraph; else those selected
    core::text::mutateParagraphRange(next, lo, hi, mutate);
    applyTextEdit(std::move(next), /*newUndoStep=*/!coalesce);
}

void VulkanCanvas::applyTextBlockEdit(const std::function<void(core::text::TextBlock&)>& mutate,
                                     bool coalesce) {
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr)
        return;
    core::text::TextBlock next = *b;
    mutate(next);
    applyTextEdit(std::move(next), /*newUndoStep=*/!coalesce);
}

void VulkanCanvas::previewSelectionStyle(
    const std::function<void(core::text::CharStyle&)>& mutate) {
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr || !m_typeHost.previewText)
        return;
    if (!m_stylePreviewActive) {
        m_stylePreviewOriginal = *b; // save the committed block once; previews build off it, not compound
        m_stylePreviewActive = true;
    }
    core::text::TextBlock next = m_stylePreviewOriginal;
    if (next.utf8.empty()) {
        mutate(next.emptyStyle);
    } else {
        const auto [lo, hi] = textEditRange();
        core::text::mutateStyleRange(next, lo, hi, mutate);
    }
    m_typeHost.previewText(m_textEditTarget, std::move(next)); // display only -- no command
    (void)ensureTextShaped(); // for the side effect: re-layout to the previewed style
    requestHostFrame();
}

void VulkanCanvas::clearStylePreview() {
    if (!m_stylePreviewActive)
        return;
    m_stylePreviewActive = false;
    if (m_typeHost.previewText && textEditBlock() != nullptr)
        m_typeHost.previewText(m_textEditTarget, std::move(m_stylePreviewOriginal)); // restore original
    m_stylePreviewOriginal = {};
    (void)ensureTextShaped(); // for the side effect: re-layout to the restored block
    requestHostFrame();
}

void VulkanCanvas::notifyTextSelectionIfChanged() {
    // Detect a change in (target, selection range, block revision) since the last report, and fire the
    // host re-sync once per change. Runs every frame from syncTextOverlay(), so it catches keyboard,
    // mouse, programmatic and undo-driven changes uniformly without instrumenting each mutation site.
    if (m_stylePreviewActive)
        return; // a live font-hover preview isn't a committed value -- don't re-sync the bar/panel to it
    core::Layer* l = textEditLayer();
    auto* tl = l != nullptr ? l->as<core::TextLayer>() : nullptr;
    const std::uint64_t rev =
        tl != nullptr ? tl->contentRevision() : static_cast<std::uint64_t>(-1);
    if (m_textEditTarget == m_textNotifiedTarget && m_textSel == m_textNotifiedSel &&
        rev == m_textNotifiedRev)
        return;
    m_textNotifiedTarget = m_textEditTarget;
    m_textNotifiedSel = m_textSel;
    m_textNotifiedRev = rev;
    if (m_typeHost.onSelectionChanged)
        m_typeHost.onSelectionChanged();
}

void VulkanCanvas::replaceMisspelledWord(std::size_t begin, std::size_t end,
                                         const std::string& replacement) {
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr || begin >= end || end > b->utf8.size())
        return;
    core::text::TextBlock next = *b;
    const std::size_t caret = core::text::replaceText(next, begin, end, replacement);
    applyTextEdit(std::move(next), /*newUndoStep=*/true); // a discrete action = its own undo step
    m_textSel.collapseTo(caret);
    m_textDesiredInline = -1.0;
    ++m_textEditCoalesce; // and the NEXT edit (typing) is distinct from this replacement too
}

void VulkanCanvas::deleteTextRange(std::size_t from, std::size_t to) {
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr || from == to)
        return;
    core::text::TextBlock next = *b;
    const std::size_t caret = core::text::replaceText(next, from, to, "");
    applyTextEdit(std::move(next), /*newUndoStep=*/false);
    m_textSel.collapseTo(caret);
    m_textDesiredInline = -1.0;
}

void VulkanCanvas::copyTextSelectionToClipboard() const {
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr || m_textSel.empty())
        return;
    const std::size_t lo = m_textSel.lo();
    Fl::copy(b->utf8.data() + lo, static_cast<int>(m_textSel.hi() - lo), 1); // 1 = system clipboard
}

void VulkanCanvas::selectAllText() {
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr)
        return;
    m_textSel = {0, b->utf8.size()}; // anchor at the start, caret/focus at the end
    m_textDesiredInline = -1.0;
    m_textBlinkOn = true;
    m_textBlinkAt = nowSeconds() + kCaretBlinkSec;
    requestHostFrame();
}

void VulkanCanvas::showTextContextMenu() {
    if (!textSessionActive())
        return;
    ContextMenu* menu = contextMenuFor(top_window());
    if (menu == nullptr)
        return; // no themed host -> no menu (the canvas has no stock fallback)
    take_focus(); // a follow-on keystroke / paste stays in the session
    const bool hasSel = !m_textSel.empty();
    const core::text::TextBlock* b = textEditBlock();
    const bool hasText = b != nullptr && !b->utf8.empty();
    std::vector<ContextAction> actions;

    // Spell-check (deferred §2): if the right-click landed inside a misspelled word, offer the
    // suggestions at the TOP, then Add to Dictionary / Ignore All, then the usual clipboard items.
    if (b != nullptr && m_typeHost.spellSuggest && !m_textMisspelled.empty()) {
        const std::size_t click =
            core::text::hitTest(ensureTextShaped(), *b, textDocToLocal(eventDocPoint()));
        const core::text::MisspelledRange* hit = nullptr;
        for (const core::text::MisspelledRange& r : m_textMisspelled) {
            if (r.begin < r.end && r.end <= b->utf8.size() && click >= r.begin && click <= r.end) {
                hit = &r;
                break;
            }
        }
        if (hit != nullptr) {
            const std::size_t wbegin = hit->begin;
            const std::size_t wend = hit->end;
            const std::string word = b->utf8.substr(wbegin, wend - wbegin);
            const std::size_t pi = core::text::paragraphIndexAt(b->utf8, wbegin);
            const std::string paraLang =
                pi < b->paragraphs.size() ? b->paragraphs[pi].language : std::string{};
            const std::string appDef = m_typeHost.spellLanguage ? m_typeHost.spellLanguage() : "";
            const std::string lang = core::text::resolveLanguage(paraLang, "", appDef);
            for (const std::string& s : m_typeHost.spellSuggest(word, lang)) {
                if (actions.size() >= 5) // cap the suggestion list; the menu stays compact
                    break;
                actions.push_back(
                    {s, [this, wbegin, wend, s] { replaceMisspelledWord(wbegin, wend, s); }, true});
            }
            if (!actions.empty()) // divider between suggestions and the learn/ignore group
                actions.back().divider = true;
            actions.push_back({_("Add to Dictionary"), [this, word, lang] {
                                   if (m_typeHost.spellAddToDict)
                                       m_typeHost.spellAddToDict(word, lang);
                               }});
            actions.push_back({_("Ignore All"),
                               [this, word] {
                                   if (m_typeHost.spellIgnore)
                                       m_typeHost.spellIgnore(word);
                               },
                               true, /*divider=*/true});
        }
    }

    actions.push_back({_("Cut"),
                       [this] {
                           copyTextSelectionToClipboard();
                           if (!m_textSel.empty())
                               deleteTextRange(m_textSel.lo(), m_textSel.hi());
                       },
                       hasSel});
    actions.push_back({_("Copy"), [this] { copyTextSelectionToClipboard(); }, hasSel});
    actions.push_back({_("Paste"), [this] { Fl::paste(*this, 1); }, true, /*divider=*/true});
    actions.push_back({_("Select All"), [this] { selectAllText(); }, hasText});

    // Anchor at the cursor in the menu's top-level coords: our own frame is canvas-local (the
    // sub-window rule), so add each ancestor offset up to the menu's top-level (mirrors
    // handleTextFieldEvent). This is the one place that deliberately LEAVES the canvas frame.
    const common::Vec2 anchor = eventLogicalPoint();
    int hx = static_cast<int>(std::lround(anchor.x));
    int hy = static_cast<int>(std::lround(anchor.y));
    for (Fl_Window* w = this; w != nullptr && w != menu->window(); w = w->window()) {
        hx += w->x();
        hy += w->y();
    }
    menu->openWith(hx, hy, std::move(actions));
}

void VulkanCanvas::moveTextCaret(std::size_t to, bool extendSelection, bool newUndoStep) {
    if (extendSelection)
        m_textSel.focus = to;
    else
        m_textSel.collapseTo(to);
    if (newUndoStep)
        ++m_textEditCoalesce; // a caret move ends the current typing burst
    m_textBlinkOn = true;
    m_textBlinkAt = nowSeconds() + kCaretBlinkSec;
    requestHostFrame();
}

int VulkanCanvas::onTextKey() {
    if (!textSessionActive())
        return 0;
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr)
        return 0;
    using namespace core::text;
    const std::string& s = b->utf8;
    const int key = Fl::event_key();
    const int state = Fl::event_state();
    const bool shift = (state & FL_SHIFT) != 0;
    const bool word = (state & (FL_CTRL | FL_META)) != 0; // Ctrl/Cmd: whole-word steps

    auto deleteRange = [&](std::size_t from, std::size_t to) { deleteTextRange(from, to); };

    switch (key) {
    case FL_Escape:
        commitTextEdit();
        return 1;
    case FL_Enter:
    case FL_KP_Enter:
        insertTextAtCaret("\n");
        return 1;
    case FL_BackSpace:
        if (!m_textSel.empty())
            deleteRange(m_textSel.lo(), m_textSel.hi());
        else if (m_textSel.focus > 0)
            deleteRange(word ? prevWordBoundary(s, m_textSel.focus)
                             : prevCharBoundary(s, m_textSel.focus),
                        m_textSel.focus);
        return 1;
    case FL_Delete:
        if (!m_textSel.empty())
            deleteRange(m_textSel.lo(), m_textSel.hi());
        else if (m_textSel.focus < s.size())
            deleteRange(m_textSel.focus, word ? nextWordBoundary(s, m_textSel.focus)
                                              : nextCharBoundary(s, m_textSel.focus));
        return 1;
    case FL_Left:
    case FL_Right:
    case FL_Up:
    case FL_Down: {
        // The visual arrow moves the caret the way it points (vertical writing-mode commit C): in
        // horizontal text Left/Right step characters and Up/Down cross lines; in a VERTICAL block
        // Up/Down step characters (up/down the column) and Left/Right cross columns -- Left is the
        // NEXT column in vertical-rl (columns advance leftward), the PREVIOUS in vertical-lr.
        const WritingMode wm = b->writingMode;
        const bool vertical = wm != WritingMode::HorizontalTB;
        const bool charStep =
            vertical ? (key == FL_Up || key == FL_Down) : (key == FL_Left || key == FL_Right);
        if (charStep) {
            const bool back = key == FL_Left || key == FL_Up; // toward the block start
            moveTextCaret(back ? (word ? prevWordBoundary(s, m_textSel.focus)
                                       : prevCharBoundary(s, m_textSel.focus))
                               : (word ? nextWordBoundary(s, m_textSel.focus)
                                       : nextCharBoundary(s, m_textSel.focus)),
                          shift, /*newUndoStep=*/true);
            m_textDesiredInline = -1.0;
            return 1;
        }
        int dir;
        if (!vertical)
            dir = key == FL_Up ? -1 : 1;
        else if (wm == WritingMode::VerticalRL)
            dir = key == FL_Left ? 1 : -1;
        else
            dir = key == FL_Right ? 1 : -1;
        const ShapedBlock& sh = ensureTextShaped();
        if (m_textDesiredInline < 0.0) { // the caret's INLINE coordinate is the goal to keep
            const CaretGeometry cg = caretGeometry(sh, *b, m_textSel.focus);
            m_textDesiredInline = vertical ? cg.top.y : cg.top.x;
        }
        const std::size_t to =
            moveCaretVertical(sh, *b, m_textSel.focus, dir, m_textDesiredInline);
        moveTextCaret(to, shift, /*newUndoStep=*/true);
        return 1;
    }
    case FL_Home:
        moveTextCaret(visualLineStart(ensureTextShaped(), *b, m_textSel.focus), shift, true);
        m_textDesiredInline = -1.0;
        return 1;
    case FL_End:
        moveTextCaret(visualLineEnd(ensureTextShaped(), *b, m_textSel.focus), shift, true);
        m_textDesiredInline = -1.0;
        return 1;
    default:
        break;
    }
    // Clipboard + Select-All while a session is live: intercept Ctrl/Cmd-C/X/V/A so they act on the
    // TEXT (not the menu's layer-level Copy/Paste). Shift excluded so Ctrl+Shift+C "Copy Merged" still
    // reaches the menu. (#9)
    if ((state & (FL_CTRL | FL_META)) != 0 && (state & FL_SHIFT) == 0) {
        switch (key | 0x20) { // case-fold the ASCII letter
        case 'c':
            copyTextSelectionToClipboard();
            return 1;
        case 'x':
            copyTextSelectionToClipboard();
            if (!m_textSel.empty())
                deleteTextRange(m_textSel.lo(), m_textSel.hi());
            return 1;
        case 'v':
            Fl::paste(*this, 1); // system clipboard -> FL_PASTE -> insertTextAtCaret
            return 1;
        case 'a':
            selectAllText();
            return 1;
        default:
            break;
        }
    }
    // Other modified accelerators (Ctrl/Cmd-Z/S…) pass through to the menus; only the word-step nav
    // above claims a modifier. Everything else is candidate text input.
    if (word)
        return 0;
    const char* t = Fl::event_text();
    const int len = Fl::event_length();
    if (t != nullptr && len > 0 &&
        (static_cast<unsigned char>(t[0]) >= 0x20 || t[0] == '\t')) { // printable / tab, not controls
        insertTextAtCaret(std::string(t, static_cast<std::size_t>(len)));
        return 1;
    }
    return 0; // pure modifiers / function keys fall through
}

void VulkanCanvas::pushTypeTool() {
    if (!m_typeHost.document)
        return;
    core::Document* doc = m_typeHost.document();
    if (doc == nullptr)
        return;
    const common::Vec2 docPt = eventDocPoint();
    core::TextLayer* hit = core::topmostTextLayerAt(doc->root(), docPt, kTextPickPadDoc);

    // While editing, a press on the box CONTROLS (edge=move, BR corner=resize, corner band=rotate)
    // arms that gesture before any caret placement -- the interior falls through to the caret below.
    if (textSessionActive()) {
        const common::Vec2 p = eventLogicalPoint();
        // The bend handle floats off the box (a gap above the top edge), so it is hit-tested first, on
        // its own -- hitTextEditBox only knows the four box corners.
        common::Vec2 bend{};
        if (textBendHandle(bend) && (p - bend).length() <= kTextBendHitPx) {
            beginTextBoxGesture(TextBoxControl::Bend, docPt);
            return;
        }
        // The fit-to-path range brackets (§9) float ON the path like the bend pill floats off the
        // box, so they are hit before the bar/box chrome (the bar's Move band overlaps them).
        if (const TextBoxControl pctl = hitTextPathBrackets(p); pctl != TextBoxControl::None) {
            beginTextBoxGesture(pctl, docPt);
            return;
        }
        // A bent Point block's Move/Resize ride the bent bar, and a bent AREA block's ride the
        // warped frame -- their visible chrome in both cases. Rotate is one shared test
        // (hitTextRotate): the box corners for flat text, the solid's extent for 3D -- so
        // hitTextEditBox's own corner band is muted for a 3D block (rotateBand 0).
        const core::text::TextBlock* eb = textEditBlock();
        const double rotBand = eb != nullptr && eb->extrude ? 0.0 : kRotateBandPx;
        std::array<common::Vec2, 4> corners{};
        if (isBentPointBlock() || isBentAreaBlock()) {
            TextBoxControl ctl =
                isBentPointBlock() ? hitBentPointBar(p) : hitBentAreaFrame(p);
            if (ctl == TextBoxControl::None && hitTextRotate(p))
                ctl = TextBoxControl::Rotate;
            if (ctl != TextBoxControl::None) {
                beginTextBoxGesture(ctl, docPt);
                return;
            }
        } else if (textEditBoxCorners(corners)) {
            TextBoxControl ctl = hitTextEditBox(p, corners, kHandleHitPx, rotBand,
                                                kTextBoxEdgeBandPx, textResizeCorner());
            if (ctl == TextBoxControl::None && hitTextRotate(p))
                ctl = TextBoxControl::Rotate;
            if (ctl != TextBoxControl::None) {
                beginTextBoxGesture(ctl, docPt);
                return;
            }
        }
    }

    // Clicking inside the block being edited places the caret / starts a selection (double-click =
    // select word). Clicking anywhere else commits the current block first.
    if (textSessionActive()) {
        if (hit != nullptr && hit->id() == m_textEditTarget) {
            const core::text::ShapedBlock& sh = ensureTextShaped();
            const core::text::TextBlock* b = textEditBlock();
            const std::size_t pos =
                b != nullptr ? core::text::hitTest(sh, *b, textDocToLocal(docPt)) : 0;
            if (b != nullptr && Fl::event_clicks() > 1) // triple-click selects the visual line (#6)
                m_textSel = {core::text::visualLineStart(sh, *b, pos),
                             core::text::visualLineEnd(sh, *b, pos)};
            else if (b != nullptr && Fl::event_clicks() > 0)
                m_textSel = core::text::wordAt(b->utf8, pos); // double-click selects the word
            else
                m_textSel.collapseTo(pos);
            m_textSelecting = true;
            m_textDesiredInline = -1.0;
            ++m_textEditCoalesce;
            m_textBlinkOn = true;
            m_textBlinkAt = nowSeconds() + kCaretBlinkSec;
            requestHostFrame();
            return;
        }
        commitTextEdit();
    }

    // Re-enter editing on an existing block under the click (select-to-edit, §6).
    if (hit != nullptr) {
        enterTextEdit(hit->id(), std::nullopt);
        const core::text::TextBlock* b = textEditBlock();
        if (b != nullptr) {
            const std::size_t pos = core::text::hitTest(ensureTextShaped(), *b, textDocToLocal(docPt));
            m_textSel.collapseTo(pos);
            m_textSelecting = true;
        }
        requestHostFrame();
        return;
    }

    // Empty space: anchor a create gesture (a plain click => Point text, a drag => Area text, §7).
    m_textCreating = true;
    m_textCreatePressDoc = docPt;
    m_textCreateDragDoc = docPt;
    m_textCreateDragged = false;
}

void VulkanCanvas::dragTypeTool() {
    if (textBoxGestureActive()) { // a move/resize/rotate of the edit box is in flight
        dragTextBox();
        return;
    }
    if (m_textSelecting && textSessionActive()) {
        const core::text::TextBlock* b = textEditBlock();
        if (b != nullptr) {
            m_textSel.focus = core::text::hitTest(ensureTextShaped(), *b, textDocToLocal(eventDocPoint()));
            m_textDesiredInline = -1.0;
            m_textBlinkOn = true;
            requestHostFrame();
            updateToolCursor(m_pointerInside); // re-tilt the I-beam to the tangent as the drag moves it
        }
        return;
    }
    if (m_textCreating) {
        // Latch the live drag point HERE. textAreaFramePolyline() runs from the render path
        // (syncTextOverlay); eventLogicalPoint() answers correctly there now, but the latch stays
        // deliberately: the preview must show the point the DRAG named, not wherever the pointer
        // has since wandered (a hover after the release would otherwise stretch the box).
        m_textCreateDragDoc = eventDocPoint();
        if (!m_textCreateDragged) {
            const common::Vec2 a = m_view.toScreen(m_textCreatePressDoc);
            const common::Vec2 b = m_view.toScreen(m_textCreateDragDoc);
            if ((b - a).length() > kMoveDragDeadZonePx)
                m_textCreateDragged = true; // past the dead zone -> an Area box, not a Point caret
        }
        requestHostFrame(); // re-draw the preview box each tick
    }
}

void VulkanCanvas::finishTypeTool() {
    if (m_textSelecting) {
        m_textSelecting = false;
        return;
    }
    if (!m_textCreating)
        return;
    m_textCreating = false;
    if (!m_typeHost.createText)
        return;
    core::text::CharStyle style =
        m_typeHost.defaultStyle ? m_typeHost.defaultStyle() : core::text::CharStyle{};

    if (m_textCreateDragged) { // Area text: the dragged box wraps the text
        const common::Rect box =
            common::Rect::fromCorners(m_textCreatePressDoc, eventDocPoint());
        core::text::TextBlock blk = core::text::makeBlock("", style, core::text::TextFrame::Area);
        blk.areaSize = {box.w, box.h};
        m_lastAreaBoxSize = {box.w, box.h};
        const core::LayerId id =
            m_typeHost.createText(std::move(blk), common::Affine2D::translation(box.x, box.y));
        if (id != core::kInvalidLayerId)
            enterTextEdit(id, std::size_t{0});
    } else if (core::VectorLayer* vl = [&]() -> core::VectorLayer* {
                   core::Document* doc = m_typeHost.document ? m_typeHost.document() : nullptr;
                   return doc != nullptr
                              ? core::topmostVectorSpineAt(
                                    doc->root(), m_textCreatePressDoc,
                                    kTextPathPickPx / std::max(1e-6, m_view.zoom()))
                              : nullptr;
               }()) {
        // Fit-to-path (§9): a CLICK near a vector layer's path spine starts text ON that path.
        // The block references the layer (edits re-flow); the geometry is baked into the text
        // layer's local space, which is the DOCUMENT frame here (identity transform), and the
        // start bracket lands at the click's arc-distance. The end bracket opens the rest of the
        // path (a closed path offers its whole circumference -- the text wraps freely).
        core::text::TextBlock blk = core::text::makeBlock("", style, core::text::TextFrame::Point);
        core::text::PathFit fit;
        fit.layer = vl->id();
        core::vec::Contours cs = core::vec::flatten(vl->object()->geometry);
        const common::Affine2D pathWorld = core::worldTransform(*vl);
        for (core::vec::Contour& c : cs)
            for (common::Vec2& pt : c.points)
                pt = pathWorld.apply(pt);
        fit.baked = std::move(cs);
        const double total = core::vec::contourLength(fit.baked);
        const bool wraps = fit.baked.size() == 1 && fit.baked.front().closed && total > 1e-9;
        fit.s0 = core::vec::nearestArcDistance(fit.baked, m_textCreatePressDoc);
        fit.s1 = wraps ? fit.s0 + total : std::max(total, fit.s0 + 1.0);
        blk.pathFit = std::move(fit);
        const core::LayerId id = m_typeHost.createText(std::move(blk), common::Affine2D{});
        if (id != core::kInvalidLayerId)
            enterTextEdit(id, std::size_t{0});
    } else { // Point text at the click, at the size slider (§7); the toggle reuses the last box size
        const bool reuse = m_typeHost.reuseLastBoxSize && m_typeHost.reuseLastBoxSize() &&
                           m_lastAreaBoxSize.x > 0.0 && m_lastAreaBoxSize.y > 0.0;
        core::text::TextBlock blk = core::text::makeBlock(
            "", style, reuse ? core::text::TextFrame::Area : core::text::TextFrame::Point);
        if (reuse)
            blk.areaSize = m_lastAreaBoxSize;
        const core::LayerId id = m_typeHost.createText(
            std::move(blk),
            common::Affine2D::translation(m_textCreatePressDoc.x, m_textCreatePressDoc.y));
        if (id != core::kInvalidLayerId)
            enterTextEdit(id, std::size_t{0});
    }
}

void VulkanCanvas::updateTextBlink() {
    if (!textSessionActive())
        return;
    const double now = nowSeconds();
    if (now >= m_textBlinkAt) {
        m_textBlinkOn = !m_textBlinkOn;
        m_textBlinkAt = now + kCaretBlinkSec;
    }
}

// The Type-edit box in layer-local space: an Area block's frame [0,0]..areaSize, or a Point block's
// content bounds padded so the (invisible-at-rest) frame sits just outside the glyphs and is
// grabbable for move/rotate. nullopt when there is nothing framable (e.g. an empty Point block).
std::optional<common::Rect> VulkanCanvas::textEditBoxLocal() const {
    const core::text::TextBlock* b = textEditBlock();
    core::Layer* l = textEditLayer();
    if (b == nullptr || l == nullptr)
        return std::nullopt;
    if (b->frame == core::text::TextFrame::Area && b->areaSize.x > 0.0 && b->areaSize.y > 0.0)
        return common::Rect{0.0, 0.0, b->areaSize.x, b->areaSize.y};
    const auto* tl = l->as<core::TextLayer>();
    const std::optional<common::Rect> cb = tl != nullptr ? tl->contentBounds() : std::nullopt;
    if (!cb || cb->empty()) {
        // A resize drag edits the block per event, which invalidates the cached content bounds
        // until the next frame re-measures them -- exactly when dragTextBox re-evaluates the
        // cursor. Fall back to the press-time box so the resize cursor / handle / baseline don't
        // flicker to the crosshair for that gap (user 2026-07-03); the live rect returns next frame.
        if (textBoxGestureActive() && !m_textBoxContent.empty())
            return m_textBoxContent;
        return std::nullopt;
    }
    constexpr double kPointPad = 4.0; // a small margin so the frame edge clears the glyphs
    return common::Rect{cb->x - kPointPad, cb->y - kPointPad, cb->w + 2.0 * kPointPad,
                        cb->h + 2.0 * kPointPad};
}

bool VulkanCanvas::textEditBoxCorners(std::array<common::Vec2, 4>& out) const {
    const std::optional<common::Rect> box = textEditBoxLocal();
    core::Layer* l = textEditLayer();
    if (!box || l == nullptr)
        return false;
    // BENT AREA: the frame the user SEES is an annular sector. applyBend anchors its arc at the
    // frame's TOP edge -- {0, 0, areaSize.x}, which is exactly textEditBoxLocal's Area rect -- and
    // textAreaFramePolyline already draws every edge on that arc family, so the four flat corners
    // have to come through the SAME warp or the resize handle, the Move band and the rotate
    // hotspots all sit on a rectangle that is no longer anywhere on screen ("place them based on
    // the line/box bend in their corners... applies to Area Type", user 2026-07-29). With this the
    // corners ARE the drawn frame polyline's own endpoints -- one named mapping rather than two
    // transcriptions that have to be kept in step: warp(0,0)/warp(W,0) are
    // textFrameArcEdgeLocal(box.y)'s first/last samples, warp(W,H)/warp(0,H) are the bottom bar's
    // (to within BentArc::W's float rounding of areaSize.x, ~1e-4 px on a 1000-px box).
    // BentArc::warp is the identity for a straight/absent arc, so unbent text is untouched to the bit.
    // ⚠ POINT text is deliberately NOT warped here: its box is contentBounds, and applyBend has
    // already written the WARPED bbox there (layoutBounds returns ShapedBlock::bounds), so a
    // second warp would double-count. textRotateCorners owns that case.
    const core::text::ShapedBlock::BentArc& arc = m_textShaped.bentArc;
    const bool warpBox = isBentAreaBlock();
    // 3D: the box corners ride the front cap too (the Area frame / Point baseline then wrap the
    // solid's face -- round 2's "the box isn't faithful"). Keeps TL,TR,BR,BL order. The warp runs
    // FIRST, in flat design space, exactly as it does for the glyphs the cap projection carries.
    const std::optional<core::text::ExtrudePlaneMap> pm = textPlaneMap();
    const common::Affine2D world = core::worldTransform(*l);
    const std::array<common::Vec2, 4> local{{{box->x, box->y},
                                             {box->right(), box->y},
                                             {box->right(), box->bottom()},
                                             {box->x, box->bottom()}}};
    for (std::size_t i = 0; i < 4; ++i) {
        common::Vec2 p = warpBox ? arc.warp(local[i]) : local[i];
        if (pm)
            p = pm->project(p);
        out[i] = m_view.toScreen(world.apply(p));
    }
    return true;
}

bool VulkanCanvas::textBottomBarScreen(std::vector<common::Vec2>& out) const {
    out.clear();
    std::vector<common::Vec2> local;
    core::Layer* l = textEditLayer();
    if (l == nullptr || !textBottomBarLocal(local))
        return false;
    // ONE funnel, hoisted: the plane map and the world transform are built once for the whole
    // polyline (ExtrudePlaneMap::from is not free, and this runs per frame).
    const common::Affine2D world = core::worldTransform(*l);
    const std::optional<core::text::ExtrudePlaneMap> pm = textPlaneMap();
    out.reserve(local.size());
    for (common::Vec2 p : local) {
        if (pm)
            p = pm->project(p);
        out.push_back(m_view.toScreen(world.apply(p)));
    }
    return true;
}

// The bar in FLAT layer-local units, before the world transform and the 3D cap projection: the one
// place its four cases live. Split out of textBottomBarScreen (which is now a thin mapper) because
// the bend handle needs the LOCAL form -- it hangs its pill along the bar's own outward normal, and
// a screen-space normal cannot tell "under the baseline" from "over it" through a mirroring layer
// transform or a back-facing 3D cap (user 2026-07-29).
bool VulkanCanvas::textBottomBarLocal(std::vector<common::Vec2>& out) const {
    out.clear();
    const std::optional<common::Rect> box = textEditBoxLocal();
    core::Layer* l = textEditLayer();
    const core::text::TextBlock* b = textEditBlock();
    if (!box || l == nullptr || b == nullptr)
        return false;
    const bool bent = b->writingMode == core::text::WritingMode::HorizontalTB &&
                      std::abs(b->bend) > 1e-4f;
    constexpr int kSamples = 24;    // even, so the middle sample is exactly the apex/centre
    constexpr double kBarPad = 4.0; // matches textEditBoxLocal's kPointPad -- the bar sits just below ink

    // Point + path-fitted (§9): the bar rides the fitted path under the text's flat advance span,
    // dropped perpendicular by the descent -- the same construction as the bent bar below, sampled
    // through the shared path samplers so it hugs exactly where the letters sit.
    if (b->pathFit && !b->pathFit->baked.empty() && m_textShaped.pathRide.active &&
        b->frame == core::text::TextFrame::Point && !m_textShaped.lines.empty()) {
        const core::text::ShapedLine& ln = m_textShaped.lines.front();
        const double d = ln.descent + kBarPad;
        const double x0 = m_textShaped.pathRide.originX;
        const double w = std::max(1.0f, m_textShaped.pathRide.flatW);
        out.reserve(kSamples + 1);
        for (int i = 0; i <= kSamples; ++i) {
            double ang = 0.0;
            const common::Vec2 p = core::text::samplePathBaseline(
                *b->pathFit,
                core::text::pathArcDistance(*b->pathFit,
                                            x0 + w * (static_cast<double>(i) / kSamples), x0),
                ang);
            out.push_back({p.x - std::sin(ang) * d, p.y + std::cos(ang) * d});
        }
        return true;
    }
    // Point + bent: ride the ACTUAL laid-out baseline. applyBend leaves the ShapedLine fields flat, so
    // line 0 gives the real baseline y, its inline extent [x,x+width], and the descent -- the bar is
    // that baseline arched by the block's bend and dropped by the descent, so it hugs the text bottom
    // rather than an approximated box-edge parabola with the wrong width/reference (user 2026-07-04).
    if (bent && b->frame == core::text::TextFrame::Point && !m_textShaped.lines.empty() &&
        m_textShaped.bentArc.active) {
        // Sample the SAME arc applyBend laid the text along (m_textShaped.bentArc), by distance s along
        // it, and drop each point PERPENDICULAR by the descent -- so the bar hugs the arched text and
        // runs parallel to the spell squiggle (which samples the same arc), never crossing it.
        const core::text::ShapedLine& ln = m_textShaped.lines.front();
        const core::text::ShapedBlock::BentArc& arc = m_textShaped.bentArc;
        const double d = ln.descent + kBarPad;
        out.reserve(kSamples + 1);
        for (int i = 0; i <= kSamples; ++i) {
            const double s = arc.W * (static_cast<double>(i) / kSamples);
            double ang = 0.0;
            const common::Vec2 p = arc.pointAt(s, ang);
            out.push_back({p.x - std::sin(ang) * d, p.y + std::cos(ang) * d});
        }
        return true;
    }
    // Bent AREA: the bottom edge is the FRAME ARC itself -- applyBend anchors the arc at the frame's
    // TOP edge now, and every horizontal line of the box (text lines and frame edges alike) rides a
    // parallel arc at its own depth. Sampling that same family here is what makes the frame's bottom,
    // the last text line and the radial sides all agree (user 2026-07-14: the old bar bowed on its
    // OWN circle -- anchored at the box bottom with the box width -- a different radius than the
    // text's, and the only edge that bowed at all).
    if (bent && b->frame == core::text::TextFrame::Area &&
        textFrameArcEdgeLocal(box->bottom(), out))
        return true;
    // Flat / vertical / empty-bent Area (no live arc): bow the box's bottom edge as a TRUE circular
    // arc with the text's own sweep (bend * kBendMaxSweep), anchored at the BL/BR corners -- the
    // earlier approximating parabola overshot the letters' real sag by up to 2x (user 2026-07-07).
    core::text::ShapedBlock::BentArc edge{
        static_cast<float>(box->x), static_cast<float>(box->bottom()), static_cast<float>(box->w),
        bent ? static_cast<float>(b->bend * core::text::kBendMaxSweep) : 0.0f, true};
    out.reserve(kSamples + 1);
    for (int i = 0; i <= kSamples; ++i) {
        double ang = 0.0;
        out.push_back(edge.pointAt(box->w * (static_cast<double>(i) / kSamples), ang));
    }
    return true;
}

bool VulkanCanvas::textFrameArcEdgeScreen(double edgeLocalY, std::vector<common::Vec2>& out) const {
    std::vector<common::Vec2> local;
    core::Layer* l = textEditLayer();
    out.clear();
    if (l == nullptr || !textFrameArcEdgeLocal(edgeLocalY, local))
        return false;
    const common::Affine2D world = core::worldTransform(*l);
    const std::optional<core::text::ExtrudePlaneMap> pm = textPlaneMap();
    out.reserve(local.size());
    for (common::Vec2 p : local) {
        if (pm)
            p = pm->project(p);
        out.push_back(m_view.toScreen(world.apply(p)));
    }
    return true;
}

bool VulkanCanvas::textFrameArcEdgeLocal(double edgeLocalY, std::vector<common::Vec2>& out) const {
    out.clear();
    const std::optional<common::Rect> box = textEditBoxLocal();
    core::Layer* l = textEditLayer();
    const core::text::TextBlock* b = textEditBlock();
    if (!box || l == nullptr || b == nullptr || b->frame != core::text::TextFrame::Area)
        return false;
    if (b->writingMode != core::text::WritingMode::HorizontalTB || std::abs(b->bend) <= 1e-4f)
        return false;
    const core::text::ShapedBlock::BentArc& arc = m_textShaped.bentArc;
    if (!arc.active)
        return false; // e.g. an empty block: applyBend laid nothing, there is no arc to ride
    // The edge = the frame-top reference arc offset down the local normal by the edge's depth --
    // which IS BentArc::warp of the flat edge, so the edge, the letters and the box CORNERS
    // (textEditBoxCorners) all come off one named mapping instead of three transcriptions of it.
    // The endpoints are the frame's warped corners (s = 0 and s = W).
    constexpr int kSamples = 24; // matches textBottomBarLocal: even, apex exactly mid-polyline
    out.reserve(kSamples + 1);
    for (int i = 0; i <= kSamples; ++i)
        out.push_back(arc.warp({arc.x0 + arc.W * (static_cast<double>(i) / kSamples), edgeLocalY}));
    return true;
}

bool VulkanCanvas::textBendHandle(common::Vec2& outScreen, common::Vec2* outApex) const {
    if (!textSessionActive())
        return false;
    const core::text::TextBlock* b = textEditBlock();
    core::Layer* l = textEditLayer();
    // Bend only bows a horizontal baseline (shaping.applyBend is gated the same way), so the handle
    // is offered only where it does something. 3D blocks bend too: the bar the pill hangs from is
    // already projected through the plane map, so the handle rides the bent solid. A path-fitted
    // block hides it -- the path owns the baseline there (bend is inert; the brackets are its chrome).
    if (b == nullptr || l == nullptr || b->writingMode != core::text::WritingMode::HorizontalTB ||
        (b->pathFit && !b->pathFit->baked.empty()))
        return false;
    std::vector<common::Vec2> bar;
    if (!textBottomBarLocal(bar) || bar.size() < 2)
        return false;
    const common::Affine2D world = core::worldTransform(*l);
    const std::optional<core::text::ExtrudePlaneMap> pm = textPlaneMap();
    const auto toScreen = [&](common::Vec2 p) {
        if (pm)
            p = pm->project(p);
        return m_view.toScreen(world.apply(p));
    };
    const std::size_t m = bar.size() / 2;
    const common::Vec2 apex = toScreen(bar[m]);
    // ⚠ The drop used to be a screen-axis `outScreen.y += kTextBendDrop`, so the pill stayed bolt
    // upright under a bar that had turned away from it: rotate the layer (or the view) and the
    // handle detached from its own stem, grabbing nowhere near where it was drawn ("bend handle
    // never rotates/conforms to text rotation", user 2026-07-29). Take the DIRECTION from the bar's
    // own local normal carried through the SAME world/cap funnel the bar itself rode -- layer
    // rotation, mirroring, the view's rotation and the 3D projection all come along, and a
    // mirroring transform genuinely flips which screen side is "under the baseline", which a
    // screen-space perpendicular could not have told. The LENGTH stays a constant screen distance:
    // this is a UI affordance, sized in screen px, not a document-space offset that zoom rescales.
    const common::Vec2 t = bar[std::min(m + 1, bar.size() - 1)] - bar[m > 0 ? m - 1 : 0];
    const double tlen = t.length();
    common::Vec2 dir{0.0, 1.0}; // screen-down: the straight, unturned case, and the last resort
    if (tlen > 1e-9) {
        const common::Vec2 n{-t.y / tlen, t.x / tlen}; // +90 deg in y-DOWN space = under the baseline
        const common::Vec2 d = toScreen(bar[m] + n) - apex; // one local unit of "under", projected
        if (const double dlen = d.length(); dlen > 1e-9)
            dir = d * (1.0 / dlen);
    }
    outScreen = apex + dir * kTextBendDrop;
    if (outApex != nullptr)
        *outApex = apex; // the stem's far end -- syncTextOverlay must not re-derive it screen-up
    return true;
}

bool VulkanCanvas::isBentPointBlock() const {
    // Bent OR path-fitted: either way the visible chrome is the curved bar, so Move/Resize ride it.
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr || b->frame != core::text::TextFrame::Point ||
        b->writingMode != core::text::WritingMode::HorizontalTB)
        return false;
    return b->bend != 0.0f || (b->pathFit && !b->pathFit->baked.empty());
}

bool VulkanCanvas::isBentAreaBlock() const {
    // The Area counterpart of isBentPointBlock: a horizontal Area block with a LIVE frame arc, so
    // its whole frame is the annular sector textAreaFramePolyline draws. The areaSize gate matters
    // -- textEditBoxLocal only returns the flat frame rect {0,0,areaSize} when BOTH extents are
    // positive, and applyBend only drives the arc from the frame when areaSize.x is; anywhere those
    // two disagree the box is contentBounds again and the frame arc does not describe it.
    const core::text::TextBlock* b = textEditBlock();
    if (b == nullptr || b->frame != core::text::TextFrame::Area ||
        b->writingMode != core::text::WritingMode::HorizontalTB)
        return false;
    return b->areaSize.x > 0.0 && b->areaSize.y > 0.0 && std::abs(b->bend) > 1e-4f &&
           m_textShaped.bentArc.active;
}

TextBoxControl VulkanCanvas::hitBentAreaFrame(common::Vec2 p) const {
    if (!isBentAreaBlock())
        return TextBoxControl::None;
    std::array<common::Vec2, 4> corners{};
    if (!textEditBoxCorners(corners))
        return TextBoxControl::None;
    // The resize handle wins wherever it sits, on the box's WARPED corner -- the same corner
    // syncTextOverlay fills the solid square on (hitTextEditBox's first test, transcribed for the
    // arc case).
    if ((p - corners[static_cast<std::size_t>(textResizeCorner() & 3)]).length() <= kHandleHitPx)
        return TextBoxControl::ResizeBR;
    // ...then the Move band, measured off the frame the user actually SEES rather than the flat
    // rect. hitTextEditBox's inside/outside split collapses to exactly this for a band: near an
    // edge is Move from either side, the deep interior is the caret, and Rotate is hitTextRotate's
    // one shared test. The polyline is the SAME one textAreaFramePolyline draws, so a strong bend
    // can no longer put the grabbable band a whole sagitta away from the arch (user 2026-07-29).
    const std::vector<common::Vec2> frame = textAreaFramePolyline();
    for (std::size_t i = 0; i + 1 < frame.size(); ++i)
        if (distToSegment(p, frame[i], frame[i + 1]) <= kTextBoxEdgeBandPx)
            return TextBoxControl::Move;
    return TextBoxControl::None;
}

bool VulkanCanvas::textPathBrackets(std::array<common::Vec2, 3>& outPos,
                                    std::array<double, 3>& outAngleRad) const {
    if (!textSessionActive())
        return false;
    const core::text::TextBlock* b = textEditBlock();
    core::Layer* l = textEditLayer();
    if (b == nullptr || l == nullptr || !b->pathFit || b->pathFit->baked.empty() ||
        b->writingMode != core::text::WritingMode::HorizontalTB)
        return false;
    const common::Affine2D world = core::worldTransform(*l);
    const std::optional<core::text::ExtrudePlaneMap> pm = textPlaneMap();
    const auto toScreen = [&](common::Vec2 p) {
        if (pm)
            p = pm->project(p);
        return m_view.toScreen(world.apply(p));
    };
    // Side-agnostic (unflipped) sampling: the brackets sit ON the path whichever side the text
    // rides. The screen tangent comes from projecting a step ahead, so it survives the layer
    // transform and the 3D plane projection alike.
    core::text::PathFit probe = *b->pathFit;
    probe.flip = false;
    const std::array<double, 3> ss{probe.s0, probe.s1, 0.5 * (probe.s0 + probe.s1)};
    for (int i = 0; i < 3; ++i) {
        double ang = 0.0;
        const common::Vec2 at = toScreen(core::text::samplePathBaseline(probe, ss[i], ang));
        const common::Vec2 ahead = toScreen(core::text::samplePathBaseline(probe, ss[i] + 2.0, ang));
        outPos[i] = at;
        outAngleRad[i] = std::atan2(ahead.y - at.y, ahead.x - at.x);
    }
    return true;
}

TextBoxControl VulkanCanvas::hitTextPathBrackets(common::Vec2 p) const {
    std::array<common::Vec2, 3> bp{};
    std::array<double, 3> ba{};
    if (!textPathBrackets(bp, ba))
        return TextBoxControl::None;
    constexpr double kBracketHitPx = 10.0;
    if ((p - bp[0]).length() <= kBracketHitPx)
        return TextBoxControl::PathStart;
    if ((p - bp[1]).length() <= kBracketHitPx)
        return TextBoxControl::PathEnd;
    if ((p - bp[2]).length() <= kBracketHitPx)
        return TextBoxControl::PathSlide;
    return TextBoxControl::None;
}

TextBoxControl VulkanCanvas::hitBentPointBar(common::Vec2 p) const {
    if (!isBentPointBlock())
        return TextBoxControl::None;
    std::vector<common::Vec2> bar;
    if (!textBottomBarScreen(bar) || bar.size() < 2)
        return TextBoxControl::None;
    if ((p - bar.back()).length() <= kHandleHitPx)
        return TextBoxControl::ResizeBR;  // size handle at the bar's right end
    for (std::size_t i = 0; i + 1 < bar.size(); ++i)
        if (distToSegment(p, bar[i], bar[i + 1]) <= kTextBoxEdgeBandPx)
            return TextBoxControl::Move;  // hovering along the bent bar moves the block
    return TextBoxControl::None;
}

bool VulkanCanvas::textRotateCorners(std::array<common::Vec2, 4>& out) const {
    const core::text::TextBlock* b = textEditBlock();
    core::Layer* l = textEditLayer();
    if (b == nullptr || l == nullptr)
        return false;
    // ⚠ BENT IS TESTED BEFORE 3D, and the order is the whole fix ("rotate handles get put in very
    // weird spots when the text is 3d and bent... place them based on the line/box bend in their
    // corners", user 2026-07-29). The 3D branch below anchors the band to an AXIS-ALIGNED extent,
    // and an AABB around an ARCH parks its corners in empty space diagonally off the arc ends --
    // precisely the defect round 3 fixed for flat bent text, which an extruded block walked
    // straight back into because `extrude` was tested first and bend never got a look in. A bent
    // block's hotspots now come off the bent box's own corners whether or not it is extruded.
    const bool bent = b->writingMode == core::text::WritingMode::HorizontalTB &&
                      std::abs(b->bend) > 1e-4f && m_textShaped.bentArc.active;
    // A bent AREA block's warped corners ARE textEditBoxCorners now (bend warp + cap projection in
    // the one funnel), so the rotate hotspots, the drawn frame's corners and the box hit-test are
    // one expression rather than three that have to be kept in step.
    if (bent && isBentAreaBlock())
        return textEditBoxCorners(out);
    if (bent) {
        // POINT: the box is ALREADY the warped content bbox (textEditBoxCorners' note), so its flat
        // corners are carried through the arc here and nowhere else -- unchanged from round 3, and
        // now through the cap plane map too, so a bent 3D Point block's hotspots ride the visible
        // face like every other piece of chrome instead of an axis-aligned extent.
        const std::optional<common::Rect> box = textEditBoxLocal();
        if (!box)
            return textEditBoxCorners(out);
        const core::text::ShapedBlock::BentArc& arc = m_textShaped.bentArc;
        const common::Affine2D world = core::worldTransform(*l);
        const std::optional<core::text::ExtrudePlaneMap> pm = textPlaneMap();
        const auto warped = [&](double lx, double ly) {
            common::Vec2 p = arc.warp({lx, ly});
            if (pm)
                p = pm->project(p);
            return m_view.toScreen(world.apply(p));
        };
        out = {warped(box->x, box->y), warped(box->right(), box->y),
               warped(box->right(), box->bottom()), warped(box->x, box->bottom())};
        return true;
    }
    const auto* tl = l->as<core::TextLayer>();
    const std::optional<common::Rect> ink =
        tl != nullptr ? tl->cachedInkBounds() : std::nullopt;
    common::Rect ext;
    if (b->extrude && !m_textShaped.bounds.empty()) {
        // 3D: the edit box rides the visible cap, whose corners can sit well inside (or float far
        // off) the projected solid -- hovering just outside the SOLID found nothing while empty
        // space rotated ("the rotate handles are in the wrong places", user 2026-07-07). Anchor
        // the band to the RENDERED INK's own bounds when the cache has them (exact by
        // construction: the alpha bbox of the pixels on screen -- "still conform poorly", user
        // 2026-07-14 round 2, was the conservative bevel-padded box projection); the projected
        // extent stays the cold-cache fallback. Both live in layer space and map through the
        // world transform alone.
        ext = ink && !ink->empty()
                  ? *ink
                  : core::text::projectedExtrudeBounds(m_textShaped.bounds, *b->extrude);
    } else {
        return textEditBoxCorners(out); // flat text: the edit box itself, exactly as before
    }
    const common::Affine2D world = core::worldTransform(*l);
    const std::array<common::Vec2, 4> local{{{ext.x, ext.y},
                                             {ext.right(), ext.y},
                                             {ext.right(), ext.bottom()},
                                             {ext.x, ext.bottom()}}};
    for (std::size_t i = 0; i < 4; ++i)
        out[i] = m_view.toScreen(world.apply(local[i]));
    return true;
}

bool VulkanCanvas::hitTextRotate(common::Vec2 p) const {
    std::array<common::Vec2, 4> c{};
    if (!textRotateCorners(c))
        return false;
    // Inside the (convex, possibly flipped) quad: never rotate -- the Move-tool convention keeps
    // the rotate affordance strictly outside the box (same test as hitTextEditBox's interior).
    bool anyPos = false, anyNeg = false;
    for (int i = 0; i < 4; ++i) {
        const common::Vec2 a = c[i];
        const common::Vec2 b = c[(i + 1) % 4];
        const double cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        anyPos = anyPos || cross > 0.0;
        anyNeg = anyNeg || cross < 0.0;
    }
    if (!(anyPos && anyNeg))
        return false;
    for (int i = 0; i < 4; ++i)
        if ((p - c[i]).length() <= kRotateBandPx)
            return true;
    return false;
}

std::optional<common::Rect> VulkanCanvas::textEditScreenBounds() const {
    std::array<common::Vec2, 4> c{};
    if (!textSessionActive() || !textEditBoxCorners(c))
        return std::nullopt;
    common::Vec2 lo = c[0];
    common::Vec2 hi = c[0];
    for (const common::Vec2& p : c) {
        lo.x = std::min(lo.x, p.x);
        lo.y = std::min(lo.y, p.y);
        hi.x = std::max(hi.x, p.x);
        hi.y = std::max(hi.y, p.y);
    }
    return common::Rect::fromCorners(lo, hi);
}

std::vector<common::Vec2> VulkanCanvas::textAreaFramePolyline() const {
    std::array<common::Vec2, 4> c{};
    if (m_textCreating && m_textCreateDragged) {
        // An Area create-drag in flight: the box from the press point to the last drag point
        // (latched at event time in dragTypeTool -- this runs from the frame loop).
        const common::Rect box = common::Rect::fromCorners(m_textCreatePressDoc, m_textCreateDragDoc);
        c = {m_view.toScreen({box.x, box.y}), m_view.toScreen({box.right(), box.y}),
             m_view.toScreen({box.right(), box.bottom()}), m_view.toScreen({box.x, box.bottom()})};
        return {c[0], c[1], c[2], c[3], c[0]}; // plain box (the handle is the overlay's solid fill)
    }
    if (!textSessionActive() || !textEditBoxCorners(c))
        return {};
    const core::text::TextBlock* b = textEditBlock();
    // The bottom edge bows to conform to the bend (BL -> BR; a straight two-ish points when unbent).
    std::vector<common::Vec2> bar;
    (void)textBottomBarScreen(bar);
    if (b != nullptr && b->frame == core::text::TextFrame::Area) {
        // Area: the WHOLE frame conforms to the bend (user 2026-07-14 -- before this, only the
        // bottom edge bowed and the rest of the box stood straight around the arched text). The top
        // edge rides the frame arc itself, the bottom is the same arc offset by the frame height
        // (the bar), and the sides join the warped corners -- a RADIAL chord of concentric circles,
        // which is a straight segment, so the implicit joins are already right. Unbent (or with no
        // live arc) the top falls back to the straight TL -> TR it always was.
        std::vector<common::Vec2> poly;
        const std::optional<common::Rect> box = textEditBoxLocal();
        std::vector<common::Vec2> top;
        if (box && textFrameArcEdgeScreen(box->y, top))
            poly = std::move(top); // TL -> TR along the warped top edge
        else
            poly = {c[0], c[1]};   // straight top: flat frame, or nothing laid out yet
        for (auto it = bar.rbegin(); it != bar.rend(); ++it)
            poly.push_back(*it);      // TR -> BR (the right radial), then the bowed bottom to BL
        poly.push_back(poly.front()); // BL -> TL (the left radial) closes the frame
        return poly;
    }
    // Point text: just one edge -- a baseline underline (user 2026-06-29: a fully-invisible box
    // "reads cheap", but a full frame would mimic an Area block, and a "U" reads like a half-drawn
    // one). The edge is the glyphs' UNDER side: the bottom for horizontal; for a vertical block the
    // LEFT edge in BOTH column orders -- vertical glyphs turn 90 deg clockwise regardless of rl/lr
    // (CSS Writing Modes: the typographic mode is shared, so line-under = left either way), the same
    // side the spell squiggles use (user 2026-07-02: not the block-end side, which read wrong for
    // lr and disconnected from the handle). The resize handle moves to the BL corner in vertical
    // (textResizeCorner) so the baseline still runs into it, like BL->BR ends at the BR handle.
    if (b != nullptr && b->writingMode != core::text::WritingMode::HorizontalTB)
        return {c[0], c[3]}; // TL -> BL, the left edge (both vertical modes)
    return bar; // Point (horizontal): the baseline underline, bowed to conform to the bend
}

int VulkanCanvas::textBoxCursorState() const {
    std::array<common::Vec2, 4> corners{};
    if (!textEditBoxCorners(corners))
        return -1;
    // Bracket drags slide ALONG the path, so their cursor is the stock resize axis best matching
    // the local tangent (the same bucketing resizeCursorFor uses); the centre grip moves freely.
    const auto bracketCursor = [&](TextBoxControl ctl) {
        if (ctl == TextBoxControl::PathSlide)
            return 10;
        std::array<common::Vec2, 3> bp{};
        std::array<double, 3> ba{};
        if (!textPathBrackets(bp, ba))
            return 10;
        const double a = ba[ctl == TextBoxControl::PathStart ? 0 : 1];
        const double deg = std::fmod(a * 180.0 / kPi + 360.0, 180.0);
        if (deg < 22.5 || deg >= 157.5)
            return 12; // WE
        if (deg < 67.5)
            return 13; // NWSE
        if (deg < 112.5)
            return 11; // NS
        return 14;     // NESW
    };
    if (textBoxGestureActive()) // keep the grabbed control's cursor for the whole drag
        switch (m_textBoxCtl) {
        case TextBoxControl::Move: return 10;
        case TextBoxControl::Rotate: return 15;
        case TextBoxControl::Bend: return 11;  // NS (up/down): the bend drag is vertical
        case TextBoxControl::ResizeBR: return resizeCursorFor(corners, textResizeCorner());
        case TextBoxControl::PathStart:
        case TextBoxControl::PathEnd:
        case TextBoxControl::PathSlide: return bracketCursor(m_textBoxCtl);
        case TextBoxControl::None: break;
        }
    const common::Vec2 p = m_cursorLogical; // hover test: the tracked pointer (updateToolCursor)
    common::Vec2 bend{};
    if (textBendHandle(bend) && (p - bend).length() <= kTextBendHitPx)
        return 11;  // hovering the bend handle -> the NS up/down cursor
    if (const TextBoxControl pctl = hitTextPathBrackets(p); pctl != TextBoxControl::None)
        return bracketCursor(pctl);
    // Rotate is the one shared test (hitTextRotate: box corners flat, the solid's extent for 3D);
    // hitTextEditBox's own corner band is muted for a 3D block so it can't double-place the band.
    const core::text::TextBlock* eb = textEditBlock();
    const double rotBand = eb != nullptr && eb->extrude ? 0.0 : kRotateBandPx;
    if (isBentPointBlock() || isBentAreaBlock()) {  // Move/Resize ride the visible bent chrome
        switch (isBentPointBlock() ? hitBentPointBar(p) : hitBentAreaFrame(p)) {
        case TextBoxControl::ResizeBR: return resizeCursorFor(corners, textResizeCorner());
        case TextBoxControl::Move: return 10;
        default: break;
        }
        return hitTextRotate(p) ? 15 : -1;
    }
    switch (hitTextEditBox(p, corners, kHandleHitPx, rotBand, kTextBoxEdgeBandPx,
                           textResizeCorner())) {
    case TextBoxControl::ResizeBR: return resizeCursorFor(corners, textResizeCorner());
    case TextBoxControl::Rotate: return 15;
    case TextBoxControl::Move: return 10;
    case TextBoxControl::Bend:      // never returned by hitTextEditBox (the bend handle is hit
    case TextBoxControl::PathStart: // off-box, and the fit-to-path brackets are canvas-tested
    case TextBoxControl::PathEnd:   // state values only -- see the enum's note)
    case TextBoxControl::PathSlide:
    case TextBoxControl::None: break;
    }
    if (hitTextRotate(p))
        return 15;  // 3D: the band hugs the solid's extent corners instead of the box's
    return -1; // interior (caret) or off the box -> the caller picks the I-beam / create cursor
}

void VulkanCanvas::beginTextBoxGesture(TextBoxControl ctl, common::Vec2 docPt) {
    core::Layer* l = textEditLayer();
    const core::text::TextBlock* b = textEditBlock();
    const std::optional<common::Rect> box = textEditBoxLocal();
    if (l == nullptr || b == nullptr || !box)
        return;
    const std::optional<common::Affine2D> base = core::worldTransform(*l).inverse();
    const std::optional<common::Affine2D> parentInv = core::parentWorldTransform(*l).inverse();
    if (!base || !parentInv) // a singular transform: nothing sane to edit through it
        return;
    m_textBoxCtl = ctl;
    m_textBoxBase = core::worldTransform(*l);
    m_textBoxBaseInv = *base;
    m_textBoxParentInv = *parentInv;
    m_textBoxContent = *box;
    m_textBoxBaseBlock = *b;
    m_textBoxPressLocal = m_textBoxBaseInv.apply(docPt);  // scale anchor for a jump-free resize
    m_textBoxPressScreen = eventLogicalPoint();
    m_textBoxLatched = false;
    ++m_textEditCoalesce; // this gesture is its own undo step, distinct from typing before/after it
    m_textBoxCoalesce = m_textEditCoalesce;
    if (ctl == TextBoxControl::Bend) {
        // Bend edits the block per event (like resize), streaming next.bend. Capture the press point in
        // layer-local space and the box width, so the drag maps the handle (which rides the box's bottom
        // bar) 1:1 to the cursor: Δbend = 2·dUp / W lifts the bar apex by exactly dUp. A 3D block's
        // pointer lands on the projected solid -- unproject through the press-time plane map (kept for
        // the whole gesture; see m_textBendPlane) so the drag works in the flat baseline frame.
        m_textBendPlane = textPlaneMap();
        const common::Vec2 pressLocal = m_textBoxBaseInv.apply(docPt);
        m_textBendPressLocal =
            m_textBendPlane ? m_textBendPlane->unproject(pressLocal).value_or(pressLocal)
                            : pressLocal;
        m_textBendW = std::max(1.0, m_textBoxContent.w);
    } else if (ctl == TextBoxControl::PathStart || ctl == TextBoxControl::PathEnd ||
               ctl == TextBoxControl::PathSlide) {
        // Bracket drags stream next.pathFit edits per event (like bend). Capture the press point's
        // arc-distance as the continuity reference, and the 3D plane at press (see m_textBendPlane).
        if (!b->pathFit || b->pathFit->baked.empty()) {
            m_textBoxCtl = TextBoxControl::None;
            return;
        }
        m_textBendPlane = textPlaneMap();
        common::Vec2 local = m_textBoxBaseInv.apply(docPt);
        if (m_textBendPlane)
            local = m_textBendPlane->unproject(local).value_or(local);
        m_textPathPressS = core::vec::nearestArcDistance(b->pathFit->baked, local);
    } else if (ctl != TextBoxControl::ResizeBR) { // Move / Rotate write the transform via TransformGesture
        const TransformMode mode =
            ctl == TextBoxControl::Rotate ? TransformMode::Rotate : TransformMode::Move;
        if (!m_textBox.begin(mode, -1, docPt, m_textBoxBase, m_textBoxContent))
            m_textBoxCtl = TextBoxControl::None; // degenerate grab: drop it
    }
}

void VulkanCanvas::dragTextBox() {
    if (!textBoxGestureActive())
        return;
    if (!m_textBoxLatched) { // a click (with its jitter) must not push a zero-delta edit
        const common::Vec2 p = eventLogicalPoint();
        if ((p - m_textBoxPressScreen).length() < kMoveDragDeadZonePx)
            return;
        m_textBoxLatched = true;
    }
    const common::Vec2 docPt = eventDocPoint();
    const auto state = Fl::event_state();
    const bool shift = (state & FL_SHIFT) != 0;
    const bool alt = (state & FL_ALT) != 0;

    if (m_textBoxCtl == TextBoxControl::Bend) {
        // Baseline bend (§9): the vertical drag in layer space maps 1:1 to the arch apex. dUp is the
        // upward (−y) displacement from the press point; Δbend = 2·dUp / W lifts the apex by exactly
        // dUp. 3D: unproject through the press-time plane map into the flat baseline frame first.
        common::Vec2 local = m_textBoxBaseInv.apply(docPt);
        if (m_textBendPlane)
            local = m_textBendPlane->unproject(local).value_or(local);
        const double dUp = m_textBendPressLocal.y - local.y;
        core::text::TextBlock next = m_textBoxBaseBlock;
        next.bend = std::clamp(m_textBoxBaseBlock.bend + static_cast<float>(2.0 * dUp / m_textBendW),
                               -1.0f, 1.0f);
        if (m_typeHost.editText)
            m_typeHost.editText(m_textEditTarget, std::move(next), m_textBoxCoalesce);
        m_textShapedRev = static_cast<std::uint64_t>(-1); // the block changed: re-layout caret/box
    } else if (m_textBoxCtl == TextBoxControl::PathStart ||
               m_textBoxCtl == TextBoxControl::PathEnd ||
               m_textBoxCtl == TextBoxControl::PathSlide) {
        // Fit-to-path brackets (§9): the pointer maps to an arc-distance on the baked path
        // (nearestArcDistance); the grabbed bracket streams that into pathFit. On a closed path
        // the nearest distance is folded to the representative closest to the reference so a drag
        // across the seam stays continuous.
        if (!m_textBoxBaseBlock.pathFit || m_textBoxBaseBlock.pathFit->baked.empty())
            return;
        common::Vec2 local = m_textBoxBaseInv.apply(docPt);
        if (m_textBendPlane)
            local = m_textBendPlane->unproject(local).value_or(local);
        const core::text::PathFit& base = *m_textBoxBaseBlock.pathFit;
        const double total = core::vec::contourLength(base.baked);
        const bool wraps = base.baked.size() == 1 && base.baked.front().closed && total > 1e-9;
        const double sRaw = core::vec::nearestArcDistance(base.baked, local);
        const auto rep = [&](double v, double ref) {
            if (!wraps)
                return v;
            while (v - ref > total * 0.5) v -= total;
            while (ref - v > total * 0.5) v += total;
            return v;
        };
        constexpr double kMinSpan = 1.0; // the brackets never cross (a zero span has no layout)
        core::text::TextBlock next = m_textBoxBaseBlock;
        core::text::PathFit& fit = *next.pathFit;
        if (m_textBoxCtl == TextBoxControl::PathStart) {
            double v = rep(sRaw, base.s0);
            v = wraps ? std::min(v, fit.s1 - kMinSpan) : std::clamp(v, 0.0, fit.s1 - kMinSpan);
            fit.s0 = v;
        } else if (m_textBoxCtl == TextBoxControl::PathEnd) {
            double v = rep(sRaw, base.s1);
            v = wraps ? std::max(v, fit.s0 + kMinSpan)
                      : std::clamp(v, fit.s0 + kMinSpan, std::max(total, fit.s0 + kMinSpan));
            fit.s1 = v;
        } else { // PathSlide: drag the whole run along the path; crossing the path flips the side
            const double delta = rep(sRaw, m_textPathPressS) - m_textPathPressS;
            fit.s0 = base.s0 + delta;
            fit.s1 = base.s1 + delta;
            double ang = 0.0;
            core::text::PathFit probe = base;
            probe.flip = false; // side test against the path itself, not the currently ridden side
            const common::Vec2 on = core::text::samplePathBaseline(probe, sRaw, ang);
            const common::Vec2 n{-std::sin(ang), std::cos(ang)};
            const double side = (local.x - on.x) * n.x + (local.y - on.y) * n.y;
            if (std::abs(side) > 3.0) // a small dead zone so hand jitter never flips it
                fit.flip = side > 0.0;
        }
        if (m_typeHost.editText)
            m_typeHost.editText(m_textEditTarget, std::move(next), m_textBoxCoalesce);
        m_textShapedRev = static_cast<std::uint64_t>(-1); // the block changed: re-layout caret/box
    } else if (m_textBoxCtl == TextBoxControl::ResizeBR) {
        core::text::TextBlock next = m_textBoxBaseBlock;
        const common::Vec2 local = m_textBoxBaseInv.apply(docPt); // cursor in layer-local space
        if (next.frame == core::text::TextFrame::Area) {
            // Area: the top-left stays put, the box grows to the cursor; the text reflows (#).
            constexpr double kMinBox = 16.0;
            next.areaSize = {std::max(kMinBox, local.x), std::max(kMinBox, local.y)};
        } else {
            // Point: uniform font-size scale off the box diagonal, anchored at the corner OPPOSITE
            // the handle -- the type re-shapes crisply at the new size (Type sizes), not a transform
            // stretch. Horizontal: BR handle, TL anchor. Vertical: the handle sits at BL (joined to
            // the left-edge side baseline), so the anchor is TR (textResizeCorner).
            const bool blHandle = next.frame == core::text::TextFrame::Point &&
                                  next.writingMode != core::text::WritingMode::HorizontalTB;
            const common::Vec2 anchor =
                blHandle ? common::Vec2{m_textBoxContent.right(), m_textBoxContent.y}
                         : m_textBoxContent.topLeft();
            // Scale relative to the PRESS point (where the handle was grabbed), not a fixed box corner:
            // a bent Point block's size handle rides the bent bar's end, not the flat corner, so this
            // starts the scale at 1.0 with no jump. For a flat block the press point is the corner.
            const double d0 = (m_textBoxPressLocal - anchor).length();
            const double d1 = (local - anchor).length();
            if (d0 > 1e-6)
                core::text::scaleTextSizes(next, static_cast<float>(d1 / d0));
        }
        if (m_typeHost.editText)
            m_typeHost.editText(m_textEditTarget, std::move(next), m_textBoxCoalesce);
        m_textShapedRev = static_cast<std::uint64_t>(-1); // the block changed: re-layout caret/box
    } else if (m_typeHost.transformText) { // Move / Rotate: write the layer transform
        const common::Affine2D world = m_textBox.transformFor(docPt, shift, alt);
        m_typeHost.transformText(m_textEditTarget, m_textBoxParentInv * world, m_textBoxCoalesce);
    }
    requestHostFrame();
    updateToolCursor(m_pointerInside); // keep the rotate cursor reoriented as the box turns
}

void VulkanCanvas::endTextBoxGesture() {
    const bool wasBlockEdit = textBlockEditGestureActive();
    m_textBox.cancel();
    m_textBoxCtl = TextBoxControl::None;
    m_textBendPlane.reset(); // the press-time 3D plane dies with the gesture
    m_textShapedRev = static_cast<std::uint64_t>(-1); // re-layout for the committed state
    ++m_textEditCoalesce; // the next edit (typing) starts a fresh undo step
    // A block-editing gesture rendered DRAFT (half-res) frames while it ran; the release must land
    // one crisp pass or the soft last frame stays on screen (the draft cache can't read as current,
    // but nothing else would trigger the recomposite that shows the re-render).
    if (wasBlockEdit && m_typeHost.recomposite)
        m_typeHost.recomposite();
    requestHostFrame();
}

bool VulkanCanvas::textBlockEditGestureActive() const {
    // The gestures that EDIT THE BLOCK per event -- and so re-shape + re-raster it every frame
    // (draft quality while live). Move/Rotate write the layer transform instead: their frames
    // re-raster nothing, and drafting them would only soften a cache that is not being replaced.
    switch (m_textBoxCtl) {
    case TextBoxControl::ResizeBR:
    case TextBoxControl::Bend:
    case TextBoxControl::PathStart:
    case TextBoxControl::PathEnd:
    case TextBoxControl::PathSlide:
        return textBoxGestureActive();
    default:
        return false;
    }
}

void VulkanCanvas::syncTextOverlay() {
    if (!m_renderer)
        return;
    // The Area-box wrapping frame, drawn as the poly-lasso smooth inverted line. It reuses the lasso
    // polyline channel; only when the Type tool is active (the lasso tool then can't be) do we own
    // that channel -- syncLassoOverlay cleared it to {} just before, so we overwrite with the frame
    // (or {} for Point / hover). Otherwise we leave the lasso tool's path untouched (#1).
    if (typeToolActive())
        m_renderer->setLassoPolyline(textAreaFramePolyline());
    notifyTextSelectionIfChanged(); // re-sync the bar/panel to the current selection (incl. session end)
    core::Layer* l = textEditLayer();
    const core::text::TextBlock* b = textEditBlock();
    if (l == nullptr || b == nullptr) {
        m_textMisspelled.clear();            // no session -> no squiggles (host repopulates on re-edit)
        m_renderer->setSpellSquiggles({});
        if (textSessionActive()) // the layer vanished (e.g. an undo of its creation): leave editing
            commitTextEdit();
        else
            m_renderer->setTextOverlay(false, {}, {}, {});
        m_renderer->setTextBendHandle(false, {}, {});
        m_renderer->setTextRotateDots(false, {}, 0.0f); // no session, no rotate affordance
        return;
    }
    // The rotate-hotspot dots (user 2026-07-14): the Type box's rotate band is invisible, and for
    // 3D text it anchors to the solid's projected extent -- a quad that can float far off the
    // visible letters, which is exactly when the handles became impossible to find. The dots fade
    // in with the MAX of the quad's own wackiness (shear/foreshortening) and its mismatch against
    // the visible edit box; a flat, unwarped box scores 0 on both and shows nothing new.
    {
        std::array<common::Vec2, 4> rc{};
        float dotAlpha = 0.0f;
        if (textRotateCorners(rc)) {
            double wacky = transformQuadWackiness(rc);
            std::array<common::Vec2, 4> bc{};
            if (textEditBoxCorners(bc))
                wacky = std::max(wacky, transformQuadMismatch(rc, bc));
            // ⚠ A BENT block is wacky BY CONSTRUCTION -- its box is an arch, and the rotate quad
            // (a plain rectangle around it) scores 0 on shape while the mismatch term can read ~0
            // too: a bent Point block's edit box is the warped content bounds, so quad and box
            // coincide and the dots never showed ("they don't appear at all -- or very briefly
            // then immediately disappear", user 2026-07-14: the flash was the gesture's stale
            // press-time box making the mismatch nonzero only DURING the drag). The bend itself
            // is the honest signal: full strength by |bend| = 0.5, where the arch is unmissable.
            if (b->writingMode == core::text::WritingMode::HorizontalTB)
                wacky = std::max(wacky, std::clamp(std::abs(b->bend) / 0.5, 0.0, 1.0));
            dotAlpha = static_cast<float>(rotateDotOpacity(wacky));
        }
        m_renderer->setTextRotateDots(dotAlpha > 0.0f, rc, dotAlpha);
    }
    // Clamp the caret/selection to the (possibly externally undone) block.
    const std::size_t n = b->utf8.size();
    m_textSel.anchor = std::min(m_textSel.anchor, n);
    m_textSel.focus = std::min(m_textSel.focus, n);

    const core::text::ShapedBlock& sh = ensureTextShaped();
    const common::Affine2D world = core::worldTransform(*l);
    // 3D (S30-d round 2): every piece of editing chrome projects through the front-cap plane map
    // first, so caret/selection/squiggles hug the solid instead of floating on the flat layout.
    const std::optional<core::text::ExtrudePlaneMap> pm = textPlaneMap();
    const auto toScreen = [&](common::Vec2 local) {
        if (pm) local = pm->project(local);
        return m_view.toScreen(world.apply(local));
    };

    const core::text::CaretGeometry cg = core::text::caretGeometry(sh, *b, m_textSel.focus);
    std::vector<std::array<common::Vec2, 4>> quads;
    if (!m_textSel.empty()) {
        // selectionQuads gives ORIENTED quads: axis-aligned per line for flat/vertical text (identical
        // to the old selectionRects path), one turned quad PER GLYPH for a bent baseline so the
        // highlight rides the arch (§9). Each corner is projected individually below.
        const auto sq = core::text::selectionQuads(sh, *b, m_textSel.lo(), m_textSel.hi());
        quads.reserve(sq.size());
        for (const std::array<common::Vec2, 4>& lq : sq) {
            std::array<common::Vec2, 4> q{toScreen(lq[0]), toScreen(lq[1]), toScreen(lq[2]),
                                          toScreen(lq[3])};
            // The shader's quadInside needs one winding. The 3D projection flips handedness when
            // the visible cap is the (mirrored) back face -- the highlight simply vanished there
            // (round 3); a mirroring layer transform would do the same. Re-order to the flat
            // convention (positive shoelace in y-down screen space) whenever it flipped.
            const double area2 = (q[1].x - q[0].x) * (q[3].y - q[0].y) -
                                 (q[3].x - q[0].x) * (q[1].y - q[0].y);
            if (area2 < 0.0)
                std::swap(q[1], q[3]);
            quads.push_back(q);
        }
    }
    // The bottom-right resize handle as the LAST overlay quad -- the renderer fills it SOLID in the box
    // colour (the same kBoxColor the bounding-box line uses), so it reads as a real handle, not a
    // translucent wash. It is box-aligned (rotates with the box). (user 2026-06-29)
    std::array<common::Vec2, 4> boxCorners{};
    int handleCount = 0;
    if (textEditBoxCorners(boxCorners)) {
        std::vector<common::Vec2> bar;
        if (isBentPointBlock() && textBottomBarScreen(bar) && bar.size() >= 2) {
            // Size handle rides the bent bar's right end, so it sits ON the visible line (not the flat
            // box corner below/beside it). A plain axis-aligned solid square.
            const common::Vec2 e = bar.back();
            const double hs = kTextHandleHalfPx;
            quads.push_back({{{e.x - hs, e.y - hs}, {e.x + hs, e.y - hs}, {e.x + hs, e.y + hs},
                              {e.x - hs, e.y + hs}}});
        } else {
            quads.push_back(textHandleQuad(boxCorners, kTextHandleHalfPx, textResizeCorner()));
        }
        ++handleCount;
    }
    // The fit-to-path range brackets (§9): start/end bars ACROSS the path plus a centre diamond
    // (slide/flip), drawn solid through the same handle channel as the resize square.
    {
        std::array<common::Vec2, 3> bp{};
        std::array<double, 3> ba{};
        if (textPathBrackets(bp, ba)) {
            const auto pushOriented = [&](common::Vec2 c, double ang, double halfAlong,
                                          double halfPerp) {
                const common::Vec2 t{std::cos(ang), std::sin(ang)};
                const common::Vec2 nrm{-t.y, t.x};
                std::array<common::Vec2, 4> q{
                    c - t * halfAlong - nrm * halfPerp, c + t * halfAlong - nrm * halfPerp,
                    c + t * halfAlong + nrm * halfPerp, c - t * halfAlong + nrm * halfPerp};
                const double area2 = (q[1].x - q[0].x) * (q[3].y - q[0].y) -
                                     (q[3].x - q[0].x) * (q[1].y - q[0].y);
                if (area2 < 0.0) // quadInside wants one winding, like the sel quads above
                    std::swap(q[1], q[3]);
                quads.push_back(q);
                ++handleCount;
            };
            pushOriented(bp[0], ba[0] + kPi * 0.5, 9.0, 1.7); // start: a bar across the path
            pushOriented(bp[1], ba[1] + kPi * 0.5, 9.0, 1.7); // end
            pushOriented(bp[2], ba[2] + kPi * 0.25, 5.0, 5.0); // centre: a slide/flip diamond
        }
    }
    // The baseline bend handle (§9) is drawn by the present pass (a rounded box-blue pill + ↕ glyph on
    // a stem) rather than as solid quads, so it can be two-tone. Set its pill centre + the bar apex the
    // stem reaches; horizontal, non-path-fitted blocks only (textBendHandle gates that -- a 3D block
    // keeps its handle, riding the projected bar). ⚠ The apex comes OUT of textBendHandle now: it
    // used to be re-derived here as pill - (0, kTextBendDrop), which silently assumed the drop was
    // screen-vertical and drew a bolt-upright stem to a point that was not on the bar at all once
    // the block (or the view) turned.
    common::Vec2 bendPill{};
    common::Vec2 bendApex{};
    if (textBendHandle(bendPill, &bendApex))
        m_renderer->setTextBendHandle(true, bendPill, bendApex);
    else
        m_renderer->setTextBendHandle(false, {}, {});
    m_renderer->setTextOverlay(m_textBlinkOn, toScreen(cg.top), toScreen(cg.bottom), quads, handleCount);

    // Spell-check squiggles (deferred §2): map each misspelled byte range to per-visual-line underline
    // segments along the line's "under" edge (the same selectionRects mapping the highlight uses), in
    // screen px -- the BOTTOM edge for horizontal text; for a vertical column the LEFT edge (glyphs
    // rotate 90 deg CW in vertical modes, so their under side faces left -- the CSS line-under side).
    // Ranges are clamped to the current block -- they lag edits by the worker's debounce, so a
    // just-typed block may briefly carry a range past its end.
    const bool textVertical = b->writingMode != core::text::WritingMode::HorizontalTB;
    std::vector<std::array<common::Vec2, 2>> squiggles;
    for (const core::text::MisspelledRange& mr : m_textMisspelled) {
        const std::size_t lo = std::min(mr.begin, n);
        const std::size_t hi = std::min(mr.end, n);
        if (lo >= hi)
            continue;
        if (textVertical) {  // vertical: the under side faces LEFT -- one segment down each column
            for (const common::Rect& r : core::text::selectionRects(sh, *b, lo, hi))
                squiggles.push_back({toScreen({r.x, r.y}), toScreen({r.x, r.bottom()})});
        } else {  // horizontal: the BOTTOM edge of each selection quad (BL->BR) -- per glyph when bent,
                  // so the squiggle rides the arch instead of running flat under it (§9).
            for (const std::array<common::Vec2, 4>& q : core::text::selectionQuads(sh, *b, lo, hi))
                squiggles.push_back({toScreen(q[3]), toScreen(q[2])});
        }
    }
    m_renderer->setSpellSquiggles(squiggles);
}

void VulkanCanvas::draw() {
    // FLTK calls draw() on expose; route it to Vulkan rather than FLTK's own drawing.
    //
    // ⚠ The SECOND frame origin, and the only one outside the host's paced loop: an expose has to
    // present, or the window shows whatever was in the swapchain when it was uncovered. It is
    // therefore deliberately NOT coalesced to the refresh interval -- and it does not need to be,
    // because FLTK raises it on map/resize/uncover, never per input event. (Which also means the
    // canvas FPS readout, which counts the host loop's frames, does not count these.)
    renderFrame();
}

void VulkanCanvas::resize(int X, int Y, int W, int H) {
    Fl_Window::resize(X, Y, W, H);
    m_view.setViewportSize({static_cast<double>(W), static_cast<double>(H)});
    if (!m_renderer)
        return;
    platform::NativeSurfaceHandle handle;
    std::string err;
    if (platform::nativeSurfaceHandle(this, handle, err)) {
        const int scale = handle.scale > 0 ? handle.scale : 1;
        // One line per CHANGE, not per resize. The overlay chrome's widths all key off this
        // number (canvas_present.comp's uiScale, via pc.ants.w), so when the ants or the reticle
        // look wrong on a HiDPI display, this says in one glance whether the scale reached the
        // renderer at all or the arithmetic downstream is at fault.
        if (scale != m_contentScale) {
            uiLog().info("canvas content scale: {}x ({}x{} logical -> {}x{} physical)", scale, W, H,
                         handle.pixelWidth, handle.pixelHeight);
            // Every cached cursor bitmap was rasterized AT THE OLD SCALE (cursorBuildScale() reads
            // m_contentScale), so dragging the window between a 1x and a 2x output left the pointer
            // showing art built for the other one -- that is the "sometimes" in the offset-cursor
            // report. Drop the lot; the next updateToolCursor rebuilds exactly the one on show, and
            // the -2 sentinel is what stops its value-dedup from deciding there is nothing to do.
            for (auto& img : m_cursorImages)
                img.reset();
            for (auto& img : m_panCursorImages)
                img.reset();
            m_rotateCursorImage.reset();
            m_rotateCursorBucket = -1;
            m_textCursorImage.reset();
            m_textCursorBucket = -1;
            m_fitTextCursorImage.reset();
            m_fitTextCursorScale = 0;
            for (auto& img : m_zoomCursorImages)
                img.reset();
            m_zoomCursorScale = 0.0;
            m_moveCursor.reset();
            m_cursorState = -2;
        }
        m_contentScale = scale;
        m_renderer->notifyResize(handle.pixelWidth, handle.pixelHeight);
    } else {
        m_renderer->notifyResize(W, H);
    }
}

void VulkanCanvas::hide() {
    // Tear the renderer down before FLTK destroys the native window it presents to, then the
    // Wayland subsurface (after the VkSurfaceKHR, before FLTK frees the parent surface).
    if (m_renderer) {
        m_renderer->waitIdle();
        if (m_onRendererShutdown)
            m_onRendererShutdown(); // ... and this is the path that actually runs on quit
        m_renderer.reset();
    }
#if !defined(__APPLE__) && !defined(_WIN32)
    m_subsurface.reset();
#endif
    m_initFailed = false; // allow re-init if shown again
    Fl_Window::hide();
}

// ---- S8 viewport interaction --------------------------------------------------------------
//
// View state lives here (per canvas) and is pushed to the renderer each frame. Input:
//   - mouse wheel            -> zoom around the cursor
//   - Space + drag / MMB-drag -> pan
//   - R + drag               -> rotate about the canvas centre (Shift snaps to 5 deg)
//   - double-tap R           -> reset rotation
// Zoom/Fit/100% are also reachable from the View menu (which works regardless of focus).

double VulkanCanvas::angleFromCenter(int x, int y) const {
    const common::Vec2 c = m_view.viewportSize() * 0.5;
    return std::atan2(static_cast<double>(y) - c.y, static_cast<double>(x) - c.x);
}

void VulkanCanvas::beginRotate(int x, int y) {
    m_rotating = true;
    m_rotateGrabAngle = angleFromCenter(x, y);
    m_rotateBaseRotation = m_view.rotation();
}

void VulkanCanvas::updateRotate(int x, int y, bool snap) {
    if (!m_rotating)
        beginRotate(x, y);
    double target = m_rotateBaseRotation + (angleFromCenter(x, y) - m_rotateGrabAngle);
    if (snap)
        target = std::round(target / kRotateSnapRad) * kRotateSnapRad;
    m_view.setRotation(target);
    m_rotatedSincePress = true;
    notifyViewChanged();
    requestHostFrame();
}

void VulkanCanvas::zoomAtCenter(double factor) {
    m_view.zoomAround(m_view.viewportSize() * 0.5, factor);
    notifyViewChanged();
    requestHostFrame();
}

void VulkanCanvas::zoomIn() {
    zoomAtCenter(1.25);
}
void VulkanCanvas::zoomOut() {
    zoomAtCenter(0.8);
}
void VulkanCanvas::fitToWindow() {
    m_view.fit();
    notifyViewChanged();
    requestHostFrame();
}
void VulkanCanvas::actualPixels() {
    m_view.actualPixels();
    notifyViewChanged();
    requestHostFrame();
}

bool VulkanCanvas::keyPhysicallyHeld(int key) const {
    if (m_heldKeyQuery)
        return m_heldKeyQuery(key); // the headless test's oracle (setHeldKeyQuery)
    // Fl::event_key(int) goes to the screen driver, which opens the display to answer. With no
    // mapped window there is no window system to ask, so the honest answer is "not held" -- which
    // also keeps the historical, believe-the-KEYUP behaviour for a canvas that was never shown.
    if (Fl::first_window() == nullptr)
        return false;
    return Fl::event_key(key) != 0;
}

void VulkanCanvas::resyncGestureModifierKeys() {
    // wl_keyboard_leave clears the compositor's held-key set and delivers FL_UNFOCUS -- never a
    // KEYUP. So a menu popup, a portal file dialog or the compositor handing focus elsewhere while
    // Space or R is held leaves our flag set for ever: the canvas stays in pan/rotate mode, the pan
    // cursor stays up, and every later click pans instead of painting. The pointer is the one thing
    // still arriving in that state, so it is where the truth gets re-read.
    if (m_spaceDown && !keyPhysicallyHeld(' '))
        m_spaceDown = false;
    if (m_rotateDown && !keyPhysicallyHeld('r')) {
        m_rotateDown = false;
        m_rotating = false;
    }
}

int VulkanCanvas::onKeyDown() {
    // While a text-edit session is live, the canvas owns the keystrokes (typing, caret nav, edit);
    // only keys onTextKey leaves unclaimed (modifiers, Ctrl/Cmd accelerators) fall through (§6).
    if (textSessionActive()) {
        if (const int consumed = onTextKey())
            return consumed;
    }
    switch (Fl::event_key()) {
    case ' ':
        m_spaceDown = true;
        updateToolCursor(m_pointerInside); // pan mode: back to the arrow
        return 1;
    case FL_Shift_L:
    case FL_Shift_R:
    case FL_Control_L:
    case FL_Control_R:
    case FL_Alt_L:
    case FL_Alt_R:
        updateToolCursor(m_pointerInside); // the op badge follows the modifiers live
        return 0;                          // not consumed: they stay ordinary modifiers
    case FL_Escape:
        if (m_recomposeReview) { // drop the Recompose preview, back to the crop suggestion
            if (m_cropHost.reviewCancel)
                m_cropHost.reviewCancel();
            return 1;
        }
        if (m_imageOpDrag.active()) {
            // Abandon the preview's handle drag: the press-time rect is reported back, so the
            // panel's fields return to what they were. A SECOND Escape then closes the panel
            // itself (the main window's popover dismissal, which this falls through to) -- the
            // crop tool's own two-stage convention.
            cancelImageOpDrag();
            return 1;
        }
        if (m_selMove.dragging()) {
            cancelSelectionMove(); // S16-i: the selection snaps back to where it was grabbed
            return 1;
        }
        if (m_gesture.active()) {
            cancelSelectionGesture(); // abandon the in-flight marquee/lasso (S14)
            return 1;
        }
        if (m_shapeDragging) {
            cancelShapeGesture(); // drop the in-flight shape drag + its preview (S26)
            return 1;
        }
        if (m_pen.active()) {
            // S28: Escape ENDS the path rather than discarding it -- Illustrator's rule, and the
            // one PLAN S28 names ("Enter/Esc/double-click/tool-switch finishes an open path"). What
            // was drawn lands as one undo step; Backspace is the way to take nodes back, and a
            // plain Undo takes the whole path back.
            commitPenPath();
            return 1;
        }
        if (m_anchorDragging) { // abandon the anchor reposition: the pivot snaps back to pre-drag
            m_transformPivotLocal = m_anchorDragPrev;
            m_anchorDragging = false;
            m_anchorDragPrev.reset();
            requestHostFrame();
            return 1;
        }
        if (m_layerMarqueeActive) {
            cancelLayerMarquee(); // S15-f: abandon the band; nothing gathered, the ants come back
            return 1;
        }
        if (m_transform.active()) {
            endMoveGesture(/*restoreBase=*/true); // S15: snap back to the pre-drag transform
            updateToolCursor(m_pointerInside);
            return 1;
        }
        if (m_chipDrawing) { // abandon the in-flight Ctrl-drag chip: nothing is marked
            finishChipDraw(/*commit=*/false);
            requestHostFrame();
            return 1;
        }
        if (m_cropRotating) { // abandon the in-flight rotate drag: the pre-drag box returns
            m_cropRotating = false;
            setCropAngle(m_cropAngle0);
            setCropRect(m_cropRotateBase); // the shrink-to-fit scaled it; restore
            updateToolCursor(m_pointerInside);
            requestHostFrame();
            return 1;
        }
        if (m_crop.active()) { // abandon the in-flight crop drag: the pre-drag rect returns
            const common::Rect base = m_crop.base();
            const bool wasDraw = m_crop.mode() == CropMode::Draw;
            m_crop.cancel();
            if (m_cropDrawFromEmpty)
                resetCropTool(); // DrawToBegin: a from-empty draw cancels back to no staged rect
            else {
                setCropRect(base);
                if (wasDraw)
                    setCropAngle(m_cropAngle0); // a fresh draw dropped the rotation: restore it
            }
            m_cropDrawFromEmpty = false;
            updateToolCursor(m_pointerInside);
            requestHostFrame();
            return 1;
        }
        if (cropToolActive() && m_cropRect) { // staged rect, no drag: back to the full canvas
            resetCropTool();
            requestHostFrame();
            return 1;
        }
        // S35-b: Esc puts the lattice back where the layer's stored warp left it -- it does NOT drop
        // the session, exactly as Esc on the Crop tool re-frames rather than leaving the tool.
        if (warpToolActive() && warpSessionActive()) {
            cancelWarp();
            return 1;
        }
        return 0; // not ours: the main window uses Escape to dismiss popovers
    case FL_BackSpace:
    case FL_Delete:
        // S28. While a path is being authored, Backspace/Delete takes the last node back (and the
        // whole gesture with it, once the last one goes). With a committed path bound for editing,
        // it removes the SELECTED node as its own undo step. Claimed ONLY in those two states, so
        // the main window's Delete-the-layer shortcut keeps working everywhere else.
        if (m_pen.active()) {
            m_pen.backspace();
            requestHostFrame();
            return 1;
        }
        if (penToolActive() && penDeleteSelectedNode())
            return 1;
        return 0;
    case FL_Enter:
    case FL_KP_Enter:
        if (m_pen.active()) {
            commitPenPath(); // S28: Enter ends an open path (the poly-lasso precedent)
            return 1;
        }
        if (m_gesture.phase() == SelectionGesture::Phase::Placing) {
            finishSelectionGesture(); // Enter closes the polygonal lasso
            return 1;
        }
        if (m_recomposeReview) { // Enter lands the reviewed Recompose as ONE undo step
            if (m_cropHost.reviewApply)
                m_cropHost.reviewApply();
            return 1;
        }
        if (cropToolActive()) {
            applyCropNow(); // Enter commits the crop (the poly-lasso precedent)
            return 1;
        }
        if (warpToolActive() && warpSessionActive()) {
            commitWarp(); // Enter applies the warp -- the crop's own convention (S35-b)
            return 1;
        }
        return 0;
    case FL_Left:
    case FL_Right:
    case FL_Up:
    case FL_Down: {
        // S16-i: nudge the selection OUTLINE while a marquee/lasso tool is active -- 1 document
        // pixel, or 10 with Shift. Document axes, not screen: the mask is integer doc pixels, so a
        // rotated view would need the coverage resampled to honour a screen-up arrow. Not ours
        // without a selection tool + an active selection (the arrows stay FLTK navigation keys).
        if (!activeSelectionKind() || m_gesture.active() || m_selMove.dragging())
            return 0;
        if (Fl::event_state() & (FL_ALT | FL_CTRL | FL_META))
            return 0; // leave the modified arrows to menus / future gestures
        if (baseSelection().isEmpty())
            return 0;
        const long step = (Fl::event_state() & FL_SHIFT) != 0 ? 10 : 1;
        // Read the whole arrow-key STATE, not just the key that fired this event: holding Left and
        // adding Down must nudge DIAGONALLY, and the window system only auto-repeats the key pressed
        // last -- so an event-key-only reading walks straight down while both are held. The key that
        // fired is always counted, in case a backend's held-key query is unreliable.
        const int fired = Fl::event_key();
        const auto held = [fired](int k) { return k == fired || Fl::event_key(k) != 0; };
        long dx = 0;
        long dy = 0;
        if (held(FL_Left))
            dx -= step;
        if (held(FL_Right))
            dx += step;
        if (held(FL_Up))
            dy -= step;
        if (held(FL_Down))
            dy += step;
        if (dx == 0 && dy == 0)
            return 1; // opposing arrows cancel: consume the key, but never push a no-op undo step
        nudgeSelection(dx, dy);
        return 1;
    }
    case 'r': {
        if (Fl::event_state() & (FL_ALT | FL_CTRL | FL_META))
            return 0; // Alt+R is the Filter menu mnemonic; only *bare* R is the rotate gesture
        // A PRESS EDGE, and nothing else, opens the double-tap window. `m_rotateDown` is only ever
        // cleared by a KEYUP the window system agreed with (onKeyUp) or by the pointer resync, so
        // this is now genuinely "R came up and went down again" -- not "some event that looked like
        // it did". The old guard's comment said "ignore auto-repeat" and assumed the X11 shape
        // (repeats arrive as bare KEYDOWNs with no intervening KEYUP); on Wayland an
        // up/down pair for a physically-held key would slip straight through it, and two such pairs
        // inside kDoubleTapSeconds -- 7 apart at FLTK's 20 Hz synthetic repeat -- read as a double
        // tap and reset the rotation mid-drag. That is the reported defect.
        if (!m_rotateDown) {
            const double now = nowSeconds();
            if (now - m_lastRDownTime < kDoubleTapSeconds && !m_rotatedSincePress) {
                m_view.resetRotation();        // double-tap R resets the rotation
                m_dialResetUntil = now + 0.55; // ... and flash the dial with a "Reset" label
                notifyViewChanged();
                requestHostFrame();
            }
            m_lastRDownTime = now;
            m_rotatedSincePress = false;
            m_rotateDown = true;
            updateToolCursor(m_pointerInside); // rotate mode: back to the arrow
        }
        return 1;
    }
    default:
        return 0; // let menu shortcuts and others through
    }
}

int VulkanCanvas::onKeyUp() {
    switch (Fl::event_key()) {
    case ' ':
        // Refuse a KEYUP for a key the window system still holds down (see the m_spaceDown note).
        // Consumed either way: the event WAS ours, we simply do not believe it.
        if (keyPhysicallyHeld(' '))
            return 1;
        m_spaceDown = false;
        // ⚠ m_panning is deliberately NOT cleared here. A pan is a POINTER gesture: it begins on
        // FL_PUSH and ends on FL_RELEASE, exactly like every other drag in this file. Ending it on
        // a key event made the drag hostage to the key stream -- one spurious/duplicated Space
        // KEYUP mid-drag killed the motion while a following repeat re-set m_spaceDown, which is
        // the reported "pan moves a little, then stops, with the pan cursor still showing":
        // m_spaceDown true (cursor 16/17 stands) and m_panning false (FL_DRAG does nothing).
        // Letting go of Space mid-drag now finishes the pan you started, which is also what every
        // other editor does.
        updateToolCursor(m_pointerInside);
        return 1;
    case 'r':
        if (keyPhysicallyHeld('r'))
            return 1; // same refusal: a physically-held key did not just come up
        m_rotateDown = false;
        m_rotating = false;
        updateToolCursor(m_pointerInside);
        return 1;
    case FL_Shift_L:
    case FL_Shift_R:
    case FL_Control_L:
    case FL_Control_R:
    case FL_Alt_L:
    case FL_Alt_R:
        updateToolCursor(m_pointerInside);
        return 0;
    default:
        return 0;
    }
}

// ---- the documentless idle state (the empty-state idle pass) --------------------------------
namespace {
constexpr double kIdleSpeed = 0.8;      // field clock rate (the chosen temperament)
constexpr double kIdleAmp = 0.7;        // ... its amplitude
constexpr double kIdlePitchPx = 32.0;   // dot lattice pitch, logical px
constexpr double kIdleQuietPadPx = 26.0; // quiet-zone margin beyond the invitation, logical px
// The wave phase wraps where every emitter's speed multiplier (1.5, 1.07, 3.2 -- all n/100)
// completes whole cycles, so the seam is invisible; wrapping in double keeps the shader's
// float phase exact over long sessions (the ants-crawl convention).
constexpr double kIdlePhaseWrap = 200.0 * kPi;
} // namespace

void VulkanCanvas::setIdleEnabled(bool on) {
    if (on == m_idleEnabled)
        return;
    m_idleEnabled = on;
    m_idleFade.setEnabled(on, nowSeconds());
    if (!on) {
        cursor(FL_CURSOR_DEFAULT); // the whole-surface "open" hand must not outlive the state
        // ⚠ Every cursor() call made OUTSIDE updateToolCursor has to invalidate the cached state,
        // or the next updateToolCursor dedups against a shape the window no longer wears and
        // returns without setting anything -- the OS pointer then keeps the arrow over a brush
        // tool (which hides it) until some unrelated state change happens by. -2 is the same
        // "unknown, re-send unconditionally" sentinel FL_ENTER uses.
        m_cursorState = -2;
    }
    requestHostFrame();
}

void VulkanCanvas::setIdleDropHot(bool hot) {
    if (hot && !m_idleEnabled)
        return; // a stray chrome-drag mirror while a document is open must not bloom
    // Tracked separately from m_idleFade because the fade is a TIMELINE (it keeps a nonzero value
    // while easing out) while this is the question handle() asks on every event: "is a bloom lit that
    // nobody has cleared?". Set even when the fade coalesces the request away, so the self-healing
    // clear at the top of handle() cannot be defeated by setHot()'s own no-op guard.
    m_dropHot = hot;
    m_idleFade.setHot(hot, nowSeconds());
    requestHostFrame();
}

void VulkanCanvas::updateIdleField() {
    const double now = nowSeconds();
    render::WindowRenderer::IdleField f;
    f.active = m_idleFade.active(now);
    if (f.active) {
        const Palette& pal = activePalette();
        const double scale = m_contentScale > 0.0 ? m_contentScale : 1.0;
        // Re-bake the invitation atlas when the theme or DPI moved under it (and on first use).
        // The bake needs FLTK font metrics, so it must never run inside a draw() -- this is the
        // frame tick, an event-loop callback (the Fl_Image_Surface rule).
        if (!m_idleAtlasBaked || m_idleAtlasDark != pal.dark || m_idleAtlasScale != scale ||
            m_idleAtlasAccent.r != pal.accent.r || m_idleAtlasAccent.g != pal.accent.g ||
            m_idleAtlasAccent.b != pal.accent.b) {
            const InvitationBake bake = bakeInvitationAtlas(pal, scale);
            if (m_renderer)
                m_renderer->setIdleAtlas(bake.atlas, InvitationBake::kRows);
            m_idleAtlasBaked = true;
            m_idleAtlasDark = pal.dark;
            m_idleAtlasScale = scale;
            m_idleAtlasAccent = pal.accent;
        }
        f.fade = static_cast<float>(m_idleFade.field.value(now));
        f.invAlpha = static_cast<float>(m_idleFade.invitation.value(now));
        f.hot = static_cast<float>(m_idleFade.hot.value(now));
        f.hover = static_cast<float>(m_idleFade.hover.value(now));
        f.timePhase = static_cast<float>(std::fmod(now * kIdleSpeed, kIdlePhaseWrap));
        f.pitch = static_cast<float>(kIdlePitchPx * scale);
        f.amp = static_cast<float>(kIdleAmp);
        f.quietPad = static_cast<float>(kIdleQuietPadPx * scale);
        // Ink sits between textMuted and accent -- the design round's tuned constants, keyed by
        // theme (they are derived shades, not palette entries).
        const common::Color8 ink = pal.dark ? common::Color8{159, 176, 232, 255}
                                            : common::Color8{86, 96, 124, 255};
        f.ink[0] = static_cast<float>(ink.r) / 255.0f;
        f.ink[1] = static_cast<float>(ink.g) / 255.0f;
        f.ink[2] = static_cast<float>(ink.b) / 255.0f;
        f.accent[0] = static_cast<float>(pal.accent.r) / 255.0f;
        f.accent[1] = static_cast<float>(pal.accent.g) / 255.0f;
        f.accent[2] = static_cast<float>(pal.accent.b) / 255.0f;
    }
    if (m_renderer)
        m_renderer->setIdleField(f);
}

int VulkanCanvas::handle(int event) {
    // Open the window in which Fl::event_x/y mean "on the canvas" (eventLogicalPoint()'s note):
    // FLTK translated the pair into our frame to make this call and restores it the instant we
    // return, so every coordinate this dispatch produces must be taken while the guard is alive.
    const PointerFrame frame(m_pointerFrameDepth, event);
    // ⚠ Self-healing clear for the drop bloom, because FL_DND_LEAVE CANNOT be relied on to arrive.
    // Fl::handle_ special-cases it (Fl.cxx): it calls `belowmouse(0)` and RETURNS 1 immediately, so
    // the leave is delivered only up whatever widget chain belowmouse happened to point at -- never
    // to a window's own handle() as an ordinary event. Since the FIRST leave nulls belowmouse, a
    // later one has nothing to walk, and a highlight cleared from a window-level handler is left
    // stuck on. That is the "drag on highlights the canvas, drag off does not" report, and it
    // reproduces on native Wayland and on Windows alike -- which is the tell that it is FLTK's
    // dispatch and not either backend.
    //
    // The reliable signal is the ABSENCE of a drag: FLTK delivers no ordinary pointer event while a
    // DND gesture is in flight, so the first one after the bloom is lit means the drag is over,
    // whatever route (or non-route) the leave took. Cheap -- one bool test on the hot path.
    if (m_dropHot && event != FL_DND_ENTER && event != FL_DND_DRAG && event != FL_DND_LEAVE &&
        event != FL_DND_RELEASE && event != FL_PASTE && event != FL_NO_EVENT)
        setIdleDropHot(false);
    // The XI2 ring fills at the device's ~200 Hz whenever the pen is over the canvas -- hovering,
    // dragging a lasso, anything. Only a brush press (pressSample) and a brush drag (drain) ever
    // consume it, so every OTHER event drops what buffered. Without this, SampleRing::overwritten()
    // would count samples nobody ever wanted instead of the stalls it exists to catch (§3.1). Cheap
    // and exact: on Wayland (sink-driven) and with no tablet at all, it is a no-op.
    switch (event) {
    case FL_PUSH:
        if (!strokeToolActive() || temporaryEyedropperActive() || cloneAnchorModifier())
            m_tablet.discardBuffered(); // a press that cannot start a stroke (incl. a Ctrl pick and
        break;                          // the clone stamp's source pick); pushBrushTool drains the
                                        // ring itself (pressSample)
    case FL_DRAG:
        if (!m_brushStroking && !m_brushPressPending)
            m_tablet.discardBuffered(); // a lasso/crop/transform drag: not a stroke, not our samples
        break;
    case FL_RELEASE:
        if (m_brushPressPending)
            break; // a tap: finishBrushStroke still has to drain the contact samples
        m_tablet.discardBuffered();
        break;
    default:
        m_tablet.discardBuffered();
        break;
    }
    // The documentless idle state owns the pointer + DND conversation outright: the whole
    // surface is the "open a file" button, and the invitation is a full drop target (its own
    // Fl_Window -- the payload arrives as a DIRECT FL_PASTE, no parent bubbling; the
    // EmptyStateView learnt both the hard way). Keyboard falls through to the normal path so
    // menu shortcuts keep working; coordinates are window-local already (sub-window rule), so
    // the release check is against [0, w) x [0, h), never event_inside.
    if (m_idleEnabled) {
        switch (event) {
        case FL_ENTER:
            m_idleFade.setHover(true, nowSeconds());
            cursor(FL_CURSOR_HAND);
            m_cursorState = -2; // set outside updateToolCursor: invalidate the dedup (setIdleEnabled)
            m_tablet.setToolCursor(FL_CURSOR_HAND); // ... and the PEN, which cursor() never reaches
            requestHostFrame();
            return 1;
        case FL_MOVE:
            return 1;
        case FL_LEAVE:
            m_idleFade.setHover(false, nowSeconds());
            cursor(FL_CURSOR_DEFAULT);
            m_cursorState = -2;
            m_tablet.setToolCursor(FL_CURSOR_DEFAULT);
            requestHostFrame();
            return 1;
        case FL_PUSH:
            dismissActivePopover(); // the app-wide chrome-click rule: a press on the work area
            dismissActiveMenu();    // closes whatever floats above it
            dismissActiveContextMenu();
            dismissActiveColorFlyout();
            return 1; // claim the whole press pair (PUSH + DRAG + RELEASE)
        case FL_DRAG:
            return 1;
        case FL_RELEASE: {
            const common::Vec2 up = eventLogicalPoint();
            if (Fl::event_button() == FL_LEFT_MOUSE && up.x >= 0.0 && up.x < w() && up.y >= 0.0 &&
                up.y < h() && m_onIdleOpen)
                m_onIdleOpen();
            return 1;
        }
        case FL_MOUSEWHEEL:
            return 1; // nothing to zoom; claiming it keeps the wheel off the chrome beneath
        case FL_DND_ENTER:
        case FL_DND_DRAG:
            setIdleDropHot(true);
            return 1;
        case FL_DND_LEAVE:
            setIdleDropHot(false);
            return 1;
        case FL_DND_RELEASE:
            setIdleDropHot(false);
            m_expectDropPaste = true; // the payload follows as a direct FL_PASTE
            return 1;
        case FL_PASTE: {
            if (!m_expectDropPaste)
                return 0; // some other paste routing -- not ours
            m_expectDropPaste = false;
            const char* dropped = Fl::event_text();
            const std::optional<std::string> path =
                firstLocalPathFromDndText(dropped != nullptr ? dropped : "");
            // openDocumentAtPath itself refuses while a background save needs quiescence, so a
            // drop accepted here can never swap a document out from under a save job.
            if (path && m_onIdleOpenPath)
                m_onIdleOpenPath(*path);
            return 1;
        }
        default:
            break; // keyboard and the rest take the normal path below
        }
    }
    switch (event) {
    case FL_ENTER:
        notifyCursor(true);
        // Force the re-entry to actually re-install our cursor. On Wayland `seat->default_cursor`
        // is APP-GLOBAL, so whatever another widget -- or a finished DND -- last set is what shows
        // when the pointer comes back over the canvas; without this the value-dedup at
        // updateToolCursor's top would see the state it left behind and do nothing. That is the
        // mechanism behind the standing "rotate cursor won't revert on canvas re-enter" item
        // (PLAN.md §12), whose WON'T-FIX rested entirely on X11 being the default backend. -2, not
        // -1: -1 is a legal want (the plain arrow), so only a value outside the encoding is a
        // sentinel the dedup cannot swallow. Cost on X11: one idempotent cursor() call per entry.
        m_cursorState = -2;
        resyncGestureModifierKeys(); // Space/R may have been released while we had no keyboard
        updateToolCursor(true);
        requestHostFrame(); // the reticle appears on entry, at input rate rather than heartbeat
        return 1; // opt in to move/wheel events
    case FL_MOVE:
        // FL_MOVE only ever arrives with no drag latched (FLTK turns motion into FL_DRAG the whole
        // time Fl::pushed() is set), so a pan flag still standing here belongs to a gesture whose
        // release we never saw -- a compositor focus change during the drag is enough. Drop it
        // before anything reads it, or the NEXT press pans instead of painting.
        m_panning = false;
        resyncGestureModifierKeys();
        if (m_gesture.phase() == SelectionGesture::Phase::Placing) {
            m_gesture.moveTo(eventDocPoint(), (Fl::event_state() & FL_SHIFT) != 0); // rubber band
            requestHostFrame();
        }
        movePenTool();      // S28: the pen's rubber-band segment follows the pointer (no-op idle)
        notifyCursor(true); // live position + colour readout (S13-b)
        // After notifyCursor, which is what refreshes m_cursorLogical, and before the cursor is
        // resolved -- the warp handle's hover decides that cursor (S35-b).
        updateWarpHover();
        updateToolCursor(true);
        // Plain motion kicks the frame loop too, and not only for the rubber band above. The brush
        // reticle, the pen's hover ring and every other pointer-following overlay are drawn by the
        // PRESENT PASS, so they move exactly as often as a frame is produced -- without this they
        // ride whatever the idle heartbeat happens to be and the reticle stutters behind the
        // cursor. The host paces the kick (it never runs ahead of the display), so this asks for
        // "a frame per motion event, capped at the panel's rate" rather than for a frame storm.
        requestHostFrame();
        return 1;
    case FL_LEAVE:
        m_pen.clearHover(); // S28: no pointer, no rubber band (it must not freeze off-canvas)
        m_penHover = PenHit{}; // ... and no hovered knob, and no closing ring, for the same reason
        m_penHasHover = false;
        notifyCursor(false); // the status bar clears its cursor readout
        updateToolCursor(false);
        return 1;
    case FL_PUSH:
        dismissActivePopover(); // a click on the work area closes the colour picker (it is its own
                                // sub-window stacked above us, so any press we see is outside it)
        dismissActiveMenu();    // ... and any open menu-bar menu, for the same reason
        dismissActiveContextMenu(); // ... and any open right-click menu (we only see presses outside it)
        dismissActiveColorFlyout(); // ... and the panels' colour bubble (same reasoning)
        take_focus();           // grab the keyboard so Space/R reach us
        // A press is a pointer event delivered to the canvas, so its coordinates are ours: track
        // it before the dispatch below (a click can land without a preceding FL_MOVE, and
        // updateToolCursor at the end of this case reads the tracked value, never the event).
        m_cursorLogical = eventLogicalPoint();
        // Ask the window system what Space/R are actually doing before the press decides which
        // gesture it starts: a KEYUP we never received must not make this press pan (see
        // resyncGestureModifierKeys).
        resyncGestureModifierKeys();
        if (m_spaceDown || Fl::event_button() == FL_MIDDLE_MOUSE) {
            m_panning = true;
            m_lastMouseX = static_cast<int>(std::lround(m_cursorLogical.x));
            m_lastMouseY = static_cast<int>(std::lround(m_cursorLogical.y));
        } else if (m_rotateDown) {
            beginRotate(static_cast<int>(std::lround(m_cursorLogical.x)),
                        static_cast<int>(std::lround(m_cursorLogical.y)));
        } else if (Fl::event_button() == FL_LEFT_MOUSE && !m_inpaintBusy) {
            // Editing gestures are blocked while an inpaint runs (the reticle shows the padlock);
            // navigation above (pan/rotate) is left live.
            if (m_recomposeReview)
                pushReviewDrag(); // the review is modal: only placement drags, no tool gestures
            else if (imageOpPreviewShowing()) {
                // The Image menu's live preview (Image Size / Canvas Size / Rotate) is staged, so
                // the canvas is in a modal-ish state that belongs to the PANEL: its own handles may
                // claim this press, and nothing else may have it. Gating HERE -- at the one
                // dispatch every tool passes through -- covers every tool at once instead of tool
                // by tool, and it leaves the Crop tool's claim on the shared overlay channel
                // untouched, because the Crop tool holding that channel is exactly what makes
                // imageOpPreviewShowing() false.
                pushImageOpPreview();
            } else if (pushDofHandles()) {
                // The active DofBlur layer's focus-band gizmo claimed the press (S33): its handles
                // work over ANY tool (the first layer-bound, tool-independent chrome), so the grab
                // outranks every tool gesture below. Dragged in FL_DRAG, cleared on FL_RELEASE.
            } else if (moveToolActive() && pushGuideGesture()) {
                // Move tool: a press on a document guide grabs it to drag (View -> Guides), before
                // the layer-move gesture below. Dragged in FL_DRAG, committed on FL_RELEASE.
            } else if (const auto kind = activeSelectionKind())
                pushSelectionGesture(*kind); // marquee/lasso tools (S14)
            else if (magicWandToolActive())
                pushMagicWand(); // one colour-flood click -> the host commits (S17)
            else if (bucketFillToolActive())
                pushBucketFill(); // one colour-flood click -> the host fills + commits (S21)
            else if (eyedropperToolActive())
                pushEyedropper(); // sample the colour under the cursor into the foreground (S24)
            else if (selectBrushToolActive())
                pushSelectBrush(); // begin a paint-to-select coverage stroke (S18)
            else if (edgeBrushToolActive())
                pushEdgeBrush(); // begin an edge-brush seed stroke (L1; the grow runs on release)
            else if (redEyeToolActive())
                pushRedEye(); // begin an eye-retouch scope stroke (S38-b; corrects on release)
            else if (moveToolActive())
                pushMoveTool(); // click-select / grab the transform controls (S15)
            else if (cropToolActive())
                pushCropTool(); // grab the crop rect's controls / draw a fresh rect (S16)
            else if (warpToolActive())
                pushWarpTool(); // grab a lattice handle (Alt: the whole lattice) -- S35-b
            else if (cloneAnchorModifier())
                pushCloneAnchor(); // Ctrl (⌘) + the clone stamp: the press picks a SOURCE (S38)
            else if (temporaryEyedropperActive())
                pushEyedropper(); // Ctrl + a brush tool: the press samples instead of stroking (S24)
            else if (strokeToolActive())
                pushBrushTool(); // begin a brush / inpaint / clone stroke (S19-a / S39 / S38)
            else if (shapeToolActive())
                pushShapeTool(); // anchor a shape drag (S26)
            else if (penToolActive())
                pushPenTool(); // place/close a Bezier node, or grab a bound path's node/handle (S28)
            else if (gradientToolActive())
                pushGradientTool(); // grab a gradient handle / anchor a new gradient drag (S22)
            else if (typeToolActive())
                pushTypeTool();  // place the caret / select-to-edit / anchor a create gesture (S29-b)
            else if (zoomToolActive())
                clickZoom(/*out=*/false); // left-click zooms IN about the point clicked
            requestHostFrame();  // new gesture preview / handles should show this frame
        } else if (Fl::event_button() == FL_RIGHT_MOUSE &&
                   (eyedropperToolActive() || temporaryEyedropperActive()) && !m_inpaintBusy) {
            pushEyedropper();  // right-click samples into the BACKGROUND swatch (S24)
            requestHostFrame();
        } else if (Fl::event_button() == FL_RIGHT_MOUSE && zoomToolActive() && !m_inpaintBusy) {
            m_zoomOutPressed = true; // the cursor shows "-" for as long as the button is held
            clickZoom(/*out=*/true); // the Zoom tool's other button: out, about the point clicked
        } else if (Fl::event_button() == FL_RIGHT_MOUSE && typeToolActive() &&
                   textSessionActive()) {
            // Right-click in a live text session: a themed Cut/Copy/Paste/Select-All menu (#9). With
            // nothing selected, place the caret at the click first so Paste lands where you clicked.
            if (m_textSel.empty()) {
                if (const core::text::TextBlock* b = textEditBlock())
                    m_textSel.collapseTo(core::text::hitTest(ensureTextShaped(), *b,
                                                             textDocToLocal(eventDocPoint())));
            }
            showTextContextMenu();
        }
        updateToolCursor(true); // a pan grab drops to the arrow; a gesture latches its badge
        return 1;               // consume so we keep receiving drag/release
    case FL_DRAG:
        if (m_panning) {
            const common::Vec2 at = eventLogicalPoint();
            const int x = static_cast<int>(std::lround(at.x));
            const int y = static_cast<int>(std::lround(at.y));
            m_view.panByScreen(
                {static_cast<double>(x - m_lastMouseX), static_cast<double>(y - m_lastMouseY)});
            m_lastMouseX = x;
            m_lastMouseY = y;
            requestHostFrame();
        } else if (m_rotating || m_rotateDown) {
            // m_rotating first, for the same reason the pan branch above is latched on m_panning:
            // once the gesture has begun it belongs to the pointer and runs to FL_RELEASE. Letting
            // go of R half way through a rotate drag finishes the rotate, it does not abandon it.
            const common::Vec2 at = eventLogicalPoint();
            updateRotate(static_cast<int>(std::lround(at.x)), static_cast<int>(std::lround(at.y)),
                         (Fl::event_state() & FL_SHIFT) != 0);
        } else if (imageOpPreviewShowing()) {
            // The preview's modal gate, the drag half (see the FL_PUSH branch): restage from the
            // grabbed handle, and swallow the drag whether or not one was grabbed. Placed high --
            // above every tool's own branch -- because two of them (the eyedropper's, the
            // temporary-eyedropper's) fire on the TOOL being active rather than on a latched
            // gesture, so a swallowed press alone would not have kept them quiet.
            dragImageOpPreview();
        } else if (m_selMove.dragging()) {
            if (m_selMove.dragTo(eventDocPoint())) { // whole-pixel steps only
                m_selMoveDirty = true;              // rebuilt once, in renderFrame
                requestHostFrame();
            }
        } else if (m_gesture.phase() == SelectionGesture::Phase::Dragging) {
            // Drag-time modifiers shape the marquee (press-time ones chose the boolean op).
            const auto state = Fl::event_state();
            m_gesture.dragTo(eventDocPoint(), (state & FL_SHIFT) != 0, (state & FL_ALT) != 0);
            requestHostFrame();
        } else if (m_gesture.phase() == SelectionGesture::Phase::Placing) {
            m_gesture.moveTo(eventDocPoint(),
                             (Fl::event_state() & FL_SHIFT) != 0); // press-and-slide
            requestHostFrame();
        } else if (m_dofDrag.active) {
            dragDofHandle(); // stream the DoF focus-band geometry edit (S33; one undo step)
        } else if (m_guideDrag.active) {
            dragGuideMove(); // track the grabbed guide to the cursor (View -> Guides; preview only)
        } else if (m_anchorDragging) {
            dragMoveAnchor(); // reposition the transform anchor (pivot); no transform is pushed (S15+)
        } else if (m_transform.active()) {
            dragMoveTool(); // stream the (coalesced) transform (S15)
        } else if (m_layerMarqueeActive) {
            dragLayerMarquee(); // a Move drag begun on empty canvas rubber-bands layers (S15-f)
        } else if (m_reviewDrag >= 0) {
            dragReviewPlacement(); // nudge a Recompose placement (plan §1.4)
        } else if (m_chipDrawing) {
            dragChipDraw(); // grow the Ctrl-drag keep-region chip (S16-f, fork F-d)
        } else if (m_crop.active() || m_cropRotating) {
            dragCropTool(); // update the staged crop rect / spin it (S16, S16-f rotate)
        } else if (m_warpDragging) {
            dragWarpTool(); // record the lattice drag for flushWarpDrag's frame tick (S35-b)
        } else if (m_brushStroking || m_brushPressPending) {
            dragBrushTool(); // stamp dabs along the segment + live preview (S19-a); a pending press
                             // begins here, from the first sample the device actually produced
        } else if (m_maskStrokeActive) {
            dragSelectBrush(); // extend the paint-to-select coverage stroke (S18)
        } else if (m_edgeStrokeActive) {
            dragEdgeBrush(); // extend the edge-brush seed trail (L1; raw trail only, never a grow)
        } else if (m_redEyeStrokeActive) {
            dragRedEye(); // extend the eye-retouch scope (S38-b; raw trail only, no correction yet)
        } else if (eyedropperToolActive() || temporaryEyedropperActive()) {
            dragEyedropper(); // keep sampling as the pointer drags (S24); the loupe follows via the
                              // per-frame heartbeat (syncLoupe reads the live cursor position)
        } else if (m_shapeBox.active() || m_lineHandle >= 0) {
            dragShapeBox(); // resize / move / rotate the selected shape, or drag a line gizmo (S26)
        } else if (m_shapeDragging) {
            dragShapeTool(); // refresh the in-flight shape outline (S26)
        } else if (penGestureActive()) {
            dragPenTool(); // pull the live handles, or move a bound path's node/handle (S28)
        } else if (m_gradientDragging || m_gradientHandle >= 0) {
            dragGradientTool(); // lay/refresh the gradient, or re-drag one of its handles (S22)
        } else if (m_textSelecting || m_textCreating || textBoxGestureActive()) {
            dragTypeTool(); // extend the selection, size an Area box, or drag the edit box (S29-b)
        }
        notifyCursor(true); // pan/rotate moves the document under the pointer
        return 1;
    case FL_RELEASE:
        m_panning = false;
        m_rotating = false;
        if (imageOpPreviewShowing()) {
            // The preview's modal gate, the release half: settle any handle drag and claim the
            // event, so the release never reaches a tool that never got the press.
            finishImageOpDrag();
            updateToolCursor(true);
            return 1;
        }
        m_reviewDrag = -1; // a placement drag ends where it was left (apply is Enter/bar Apply)
        if (m_guideDrag.active)
            finishGuideDrag(); // a grabbed guide lands (moved, or deleted if dragged off-canvas)
        if (m_selMove.dragging())
            finishSelectionMove(); // S16-i: the translated mask lands as one "Move Selection" step
        if (m_gesture.phase() == SelectionGesture::Phase::Dragging)
            finishSelectionGesture(); // marquee / free lasso commits on release (poly = on close)
        if (m_anchorDragging) { // the transform-anchor reposition ends; the new pivot stands (S15+)
            m_anchorDragging = false;
            m_anchorDragPrev.reset();
            requestHostFrame();
        }
        m_zoomOutPressed = false; // the Zoom tool's "-" cursor is held-only (see updateToolCursor)
        finishLayerMarquee(); // S15-f: the band gathers what it swept, then gives the ants back
        endMoveGesture(/*restoreBase=*/false); // the streamed transform stands as the commit
        finishDofGesture(); // a DoF handle drag ends: the last streamed geometry stands (S33)
        // A crop-tool click (press + release within the slop, see dragCropTool) on a keep-region
        // chip toggles it, and the ONE suggestion re-runs either way (an input changed -- never
        // a different-answer re-roll, plan §1): ON = protect it again, OFF = actively ignore it
        // (its importance is masked out of the search, so the box adapts AWAY from ignored
        // content; user 2026-07-02). A dragged gesture never toggles, and only the FIRST click
        // of a multi-click sequence does -- the repeats are swallowed so a fast double-click
        // neither double-toggles nor collides with double-click-apply (see pushCropTool).
        // A Ctrl-press that never cleared the slop is the SAME click, so it toggles too; one
        // that did latch commits its hand-drawn USER chip instead (fork F-d). A user chip has no
        // detector behind it, so clicking it REMOVES it outright rather than parking it disabled.
        {
            const bool chipClick = (m_crop.active() && !m_cropDragMoved) ||
                                   (m_chipDrawing && !m_chipDrawLatched);
            if (m_chipDrawing)
                finishChipDraw(true);
            if (chipClick && Fl::event_clicks() == 0 && smartResizeOn()) {
                if (SmartChip* chip = smartChipAt(eventDocPoint())) {
                    if (chip->user)
                        m_smartChips.erase(m_smartChips.begin() +
                                           (chip - m_smartChips.data()));
                    else
                        chip->enabled = !chip->enabled;
                    m_chipClickConsumed = true; // arm the dbl-click swallow (removed chips too)
                    m_crop.cancel();
                    applySmartCropSuggestion();
                }
            }
        }
        m_crop.cancel(); // the staged rect stays as the drag left it (apply is Enter/dbl-click)
        m_cropRotating = false; // a rotate drag ends where it was left, same convention
        if (m_warpDragging)
            finishWarpDrag(); // the lattice stays as the drag left it; Apply is Enter / the bar (S35-b)
        if (m_brushStroking || m_brushPressPending)
            finishBrushStroke(); // the brush stroke lands as one undoable command (S19-a); a tap
                                 // that never dragged drains its contact samples in there
        if (m_maskStrokeActive)
            finishSelectBrush(); // the paint-to-select stroke lands as one SetSelectionCommand (S18)
        if (m_edgeStrokeActive)
            finishEdgeBrush(); // the seed trail grows to edges ONCE and lands as one command (L1)
        if (m_redEyeStrokeActive)
            finishRedEye(); // the scope is corrected ONCE and lands as one command (S38-b)
        endShapeBoxGesture();    // a selected shape's resize/transform commits (S26-b §7.1; no-op idle)
        if (m_shapeDragging)
            finishShapeTool();   // the shape lands as a new VectorLayer (S26)
        finishPenDrag();         // a pen handle pull / node drag settles (S28; no-op when idle)
        if (m_gradientDragging || m_gradientHandle >= 0)
            finishGradientTool(); // the gradient lands as a new VectorLayer, or a handle drag settles (S22)
        if (textBoxGestureActive())
            endTextBoxGesture(); // a move/resize/rotate of the edit box commits (the last drag stands)
        if (m_textSelecting || m_textCreating)
            finishTypeTool();    // create the Point/Area block + enter editing, or end a selection (S29-b)
        updateToolCursor(true);  // back to the live-modifier badge (or from pan to crosshair)
        return 1;
    case FL_MOUSEWHEEL: {
        const int dy = Fl::event_dy();
        if (dy != 0) {
            const double factor = std::pow(kZoomClickStep, -dy); // wheel up (dy<0) zooms in
            m_view.zoomAround(eventLogicalPoint(), factor);
            notifyViewChanged();
            notifyCursor(true); // same screen point, new document point
            requestHostFrame();
        }
        return 1;
    }
    case FL_FOCUS:
    case FL_UNFOCUS:
        return 1;
    // S50: a file dragged over the canvas becomes a magic layer on release. The canvas is its own
    // Fl_Window, so these arrive here directly rather than at the main window; refusing the ENTER
    // (return 0) is what makes the drag source say "not here" over an app with no document open.
    case FL_DND_ENTER:
    case FL_DND_DRAG:
        return m_onFilesDropped ? 1 : 0;
    case FL_DND_LEAVE:
        return 1;
    case FL_DND_RELEASE:
        if (!m_onFilesDropped)
            return 0;
        m_expectDropPaste = true; // the payload follows as a DIRECT FL_PASTE (no parent bubbling)
        return 1;
    case FL_PASTE:
        if (m_expectDropPaste) { // the accepted drop's URI list, not a clipboard paste
            m_expectDropPaste = false;
            const char* dropped = Fl::event_text();
            const std::vector<std::string> paths =
                localPathsFromDndText(dropped != nullptr ? dropped : "");
            if (!paths.empty() && m_onFilesDropped)
                m_onFilesDropped(paths);
            return 1;
        }
        // Clipboard text arriving from Fl::paste (Ctrl-V / the right-click Paste) -> insert at the
        // caret while a session is live (#9). Normalize CRLF/CR -> LF so paragraphs split cleanly.
        if (textSessionActive()) {
            const char* t = Fl::event_text();
            const int len = Fl::event_length();
            if (t != nullptr && len > 0) {
                std::string clean;
                clean.reserve(static_cast<std::size_t>(len));
                for (int i = 0; i < len; ++i) {
                    if (t[i] == '\r') {
                        clean.push_back('\n');
                        if (i + 1 < len && t[i + 1] == '\n')
                            ++i; // collapse a CRLF pair into one '\n'
                    } else {
                        clean.push_back(t[i]);
                    }
                }
                insertTextAtCaret(clean);
            }
            return 1;
        }
        return 0;
    case FL_KEYDOWN:
        return onKeyDown();
    case FL_KEYUP:
        return onKeyUp();
    default:
        break;
    }
    return Fl_Window::handle(event);
}

} // namespace mosaic::ui
