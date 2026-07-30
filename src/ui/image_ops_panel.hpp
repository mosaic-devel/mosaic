#pragma once

#include "common/image.hpp"        // common::Color8 (the fill-colour providers)
#include "common/geometry.hpp"     // common::Rect (corner placement region)
#include "core/vector/paint.hpp"   // core::vec::Paint / Gradient / ProceduralPattern (the paint fills)
#include "render/document_ops.hpp" // CanvasAnchor, and (transitively) ResampleFilter + CropFill
#include "ui/new_document_dialog.hpp" // SizeUnit, unitToPixels/pixelsToUnit, kMaxCanvasDimension
#include "ui/popover.hpp"

#include <FL/Fl_Widget.H>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace mosaic::ui {

class GradientFlyout;
class PatternFlyout;
class ScrubRuler;

// The Canvas Size dialog's nine-point anchor control: a 3x3 grid of cells where the SELECTED cell
// is where the existing picture sits inside the new canvas, and the eight arrows radiating out of
// it show which way the canvas grows around it. Bespoke (not nine buttons) because the arrows have
// to move with the selection, which no stock widget does.
//
// EVERYTHING IS COVERAGE-RASTERIZED (ui::GizmoCanvas), not whole-pixel rect tables. The rect-table
// discipline is right for a 12 px menu badge, where a glyph is a handful of pixels and any
// softening reads as blur; at 30 px cells the same table reads as pixel art (user, 2026-07-28).
// So the arrows are proper anti-aliased shafts + chevron heads, the cell fills carry soft edges,
// and only the grid HAIRLINES are pinned to exact pixel columns -- a half-pixel line is the one
// thing that would look cheap here. Same reasoning, and the same renderer, as the 3D gizmo
// (ui/gizmo_canvas.hpp).
//
// Keyboard: the arrow keys walk the selection (clamped at the edges, never wrapping -- wrapping
// from "top-left" to "top-right" on one keypress would be a silent, invisible jump). The key
// handling is a thin adapter over moveBy(), which is public and pure so the navigation can be
// unit-tested headlessly.
class AnchorGrid : public Fl_Widget {
public:
    AnchorGrid(int X, int Y, int W, int H);

    [[nodiscard]] render::CanvasAnchor value() const noexcept { return m_anchor; }
    // Set without firing the callback (the host seeding state); no-op when unchanged.
    void setValue(render::CanvasAnchor a);
    // Walk the selection by whole cells and FIRE the callback when it actually moved. dx is
    // columns (+1 = right), dy is rows (+1 = down); both are clamped into the grid.
    void moveBy(int dx, int dy);

    // The row-major mapping the engine's CanvasAnchor already documents (`int(a) % 3` = column,
    // `int(a) / 3` = row), spelled out here so the widget and the tests share ONE definition.
    [[nodiscard]] static render::CanvasAnchor anchorFor(int col, int row);
    [[nodiscard]] static int columnOf(render::CanvasAnchor a) { return static_cast<int>(a) % 3; }
    [[nodiscard]] static int rowOf(render::CanvasAnchor a) { return static_cast<int>(a) / 3; }

    // The surface this widget erases against (a panel, not the window ground) -- the CheckBox
    // convention.
    void setGroundColor(common::Color8 c) {
        m_ground = c;
        m_hasGround = true;
    }

protected:
    void draw() override;
    int handle(int event) override;

private:
    // Geometry of the grid: the cell pitch and the grid's top-left, both whole pixels, so every
    // hairline lands on an exact pixel boundary regardless of the widget's size.
    [[nodiscard]] int cellSize() const;
    [[nodiscard]] int gridX() const;
    [[nodiscard]] int gridY() const;
    [[nodiscard]] int cellAt(int ex, int ey, int& col, int& row) const; // 1 = inside the grid

    render::CanvasAnchor m_anchor = render::CanvasAnchor::Center;
    int m_hoverCol = -1;
    int m_hoverRow = -1;
    common::Color8 m_ground{};
    bool m_hasGround = false;
};

// The Image-menu operations panel (S53): a pinned corner popover -- the SelectMorphPanel's
// live-preview shape, not a modal -- serving Image Size, Canvas Size and Rotate Arbitrary. Every
// control change fires onPreview, which the host turns into a canvas overlay showing the staged
// canvas (NO command); Apply fires onApply once and the host lands exactly ONE command; Cancel/Esc
// close and the host drops the preview.
//
// The panel is pure UI: it owns no document and computes no pixels. It converts units, keeps the
// proportions lock honest and hands the host a fully-resolved Request in DOCUMENT PIXELS.
class ImageOpsPanel : public Popover {
public:
    enum class Mode { ImageSize, CanvasSize, RotateArbitrary };

    // What fills the area the old canvas did not cover (Canvas Size growth, and the corner wedges
    // of an arbitrary rotation). This is the Edit->Fill... dialog's own Contents family
    // (ui/fill_dialog.hpp) plus Transparent, which only an EXPANSION can mean -- the Fill dialog
    // fills existing pixels and has nothing to leave empty. The enum's order IS the dropdown's.
    //
    // Inpaint reconstructs the newly exposed margin from the picture beside it. That is a
    // one-sided reconstruction by construction (a grown margin has image on the inboard side and
    // nothing on the other three), so the engine extends the edge outward rather than closing a
    // hole; the host runs it asynchronously and lands the healed pixels through the same single
    // undo step as a solid fill. Canvas Size only -- see setFillMode's note in the .cpp.
    enum class FillMode {
        Transparent = 0,
        Foreground,
        Background,
        White,
        Black,
        Gray,
        Custom,   // "Color..."   -> the colour flyout
        Gradient, // "Gradient..." -> the gradient flyout (+ type dropdown + direction dial)
        Pattern,  // "Pattern..."  -> the pattern flyout
        Inpaint
    };

    struct Request {
        Mode mode = Mode::CanvasSize;
        std::uint32_t width = 0, height = 0;                          // document pixels
        render::CanvasAnchor anchor = render::CanvasAnchor::Center;   // CanvasSize only
        render::ResampleFilter filter = render::ResampleFilter::Auto; // ImageSize / Rotate only
        double angleDeg = 0.0;                                        // RotateArbitrary only
        // The raw fill choice, so the host can route the two kinds it must materialize itself:
        // Gradient/Pattern (rasterized from `paint` over the new canvas) and Inpaint (an async
        // engine run). Everything else is already resolved into `fill` below.
        FillMode fillMode = FillMode::Transparent;
        // The expansion fill, resolved for the SOLID kinds (nullopt = Transparent = add nothing).
        // Deliberately nullopt for Gradient/Pattern/Inpaint: those cost a full new-canvas-sized
        // image, and a preview fires on every keystroke -- the host materializes them on Apply.
        std::optional<render::CropFill> fill;
        // Gradient/Pattern only: the paint to rasterize across the new canvas (render::
        // computeFillPaint), exactly what FillDialog::currentPaint() hands its own fill path.
        core::vec::Paint paint;
    };

    ImageOpsPanel();
    ~ImageOpsPanel() override;

    // Corner placement (the Type/morphology panels' behaviour): pin to the canvas region's
    // bottom-right.
    void setPlacementProviders(std::function<common::Rect()> region);

    // The two colours the Fill combo can name. The panel resolves the choice into the CropFill it
    // hands back, so the host never has to re-derive it from an index.
    void setFillColorProviders(std::function<common::Color8()> foreground,
                               std::function<common::Color8()> background);

    // The shared precision-ruler HUD for the width/height scrub sliders (the morphology panel's
    // wiring). Non-owning; null leaves scrubbing working, just without the HUD.
    void setScrubRuler(ScrubRuler* r);

    // The host's gradient + pattern editors for the "Gradient..." / "Pattern..." fills. They must
    // be child sub-windows of the SAME top-level as this panel (the main window builds them before
    // show, like every other flyout); the panel drives openFor/onChange itself. Null = the paint
    // chip simply does not open anything.
    void setPaintFlyouts(GradientFlyout* gradient, PatternFlyout* pattern);

    // The "Color..." fill's swatch chip was clicked: the host opens its shared colour flyout at
    // `anchor` seeded with `current`, and routes the pick back through setCustomFillColor(). The
    // Type/3D panels' setOnEditColor contract, verbatim.
    void setOnEditFillColor(std::function<void(const Fl_Widget* anchor, common::Color8 current)> cb) {
        m_onEditFillColor = std::move(cb);
    }
    // A pick came back from that flyout (or the host is seeding the panel's custom colour).
    void setCustomFillColor(common::Color8 c);
    [[nodiscard]] common::Color8 customFillColor() const noexcept { return m_customColor; }

    // The host wires these. onPreview fires on every live control change (and once from openFor);
    // onApply / onCancel fire once from the footer buttons (onApply also from Enter).
    void setOnPreview(std::function<void(const Request&)> cb) { m_onPreview = std::move(cb); }
    void setOnApply(std::function<void(const Request&)> cb) { m_onApply = std::move(cb); }
    void setOnCancel(std::function<void()> cb) { m_onCancel = std::move(cb); }

    // Rebuild for `mode` and seed the size/angle/anchor state from the document, WITHOUT showing
    // and without firing onPreview. `dpi` is the document's resolution -- the physical units
    // (mm / in / pt) are meaningless without it. Split out of openFor so the panel's whole model
    // is drivable headlessly (the SelectMorphPanel::configure precedent).
    void configure(Mode mode, std::uint32_t docW, std::uint32_t docH, double dpi = 72.0);

    // configure() + corner-place + show + the first preview. What the host's menu items call.
    void openFor(Mode mode, const Fl_Widget* anchor, std::uint32_t docW, std::uint32_t docH,
                 double dpi = 72.0);

    [[nodiscard]] Mode mode() const noexcept { return m_mode; }

    // The canvas reported a live drag of the staged preview's handles: `x`/`y`/`w`/`h` are the new
    // canvas rect in CURRENT document pixels -- exactly the space the host stages the overlay in.
    // The panel stays the owner of the numbers: it adopts the dragged size, derives the anchor cell
    // the drag implies, pushes both into its controls and fires ONE preview. A no-op in Rotate mode
    // (there the rect is a function of the angle, not something to drag a size out of).
    void applyPreviewDrag(long x, long y, std::uint32_t w, std::uint32_t h);

    // The proportions lock, from outside the checkbox. ONE code path with the checkbox's own
    // toggle (which calls this): re-arming adopts the ratio currently on screen rather than
    // snapping back to the document's. Exists as a public entry because ui::CheckBox only flips on
    // a real FL_RELEASE inside its rect, so the headless tests cannot reach it any other way.
    void setConstrainProportions(bool on);
    // The current control state, resolved to document pixels (what onPreview/onApply carry).
    [[nodiscard]] Request request() const;

    // Read/drive the state directly -- the host seeds nothing else, but the headless tests do.
    [[nodiscard]] std::uint32_t pixelWidth() const noexcept { return m_pxW; }
    [[nodiscard]] std::uint32_t pixelHeight() const noexcept { return m_pxH; }
    [[nodiscard]] bool constrainProportions() const noexcept { return m_constrain; }
    [[nodiscard]] SizeUnit unit() const noexcept { return m_unit; }
    [[nodiscard]] FillMode fillMode() const noexcept { return m_fill; }
    [[nodiscard]] AnchorGrid* anchorGrid() const noexcept { return m_grid; }
    // Drive the Fill combo from outside the widget (the headless tests; the host never needs it).
    void setFillMode(FillMode m);
    // The paint the current Fill choice would lay down -- FillDialog::currentPaint()'s twin, and
    // what Request::paint carries for the Gradient/Pattern kinds.
    [[nodiscard]] core::vec::Paint currentPaint() const;
    // The flat colour the current Fill choice resolves to (meaningless for the paint kinds and for
    // Transparent/Inpaint, which resolve to no colour at all).
    [[nodiscard]] common::Color8 currentFillColor() const;

    void reapplyTheme() override; // re-theme + rebuild the controls in place
    // Closing the panel also closes any gradient/pattern bubble it opened -- their anchor chip is
    // about to stop being visible, and a stranded flyout is the classic orphan.
    void hide() override;

protected:
    int handle(int event) override; // Enter = Apply; the arrow keys drive the anchor grid

private:
    void build();               // (re)generate the controls for m_mode
    void pushStateToControls(); // reflect the members into the widgets (m_syncing-guarded)
    void firePreview();
    // Re-derive the free dimension from the locked aspect after `widthEdited` (or the height) moved.
    void applyConstraint(bool widthEdited);
    [[nodiscard]] std::uint32_t clampDimension(double px) const;
    // Push ONE size field's text (and its scrub slider) from the model, guarded. Only the PARTNER
    // field is ever rewritten while typing: re-writing the field being typed into would send its
    // caret to the end on every reformat.
    void pushSizeField(bool width);
    void pushSizeSlider(bool width); // range/step/suffix + value, in the CURRENT unit
    // Decimals shown in the size fields: none for pixels (a canvas is a whole number of them),
    // the full value for the physical units.
    [[nodiscard]] double sizeStep() const { return m_unit == SizeUnit::Pixels ? 1.0 : 0.0; }
    // Show exactly the contextual control the current Fill choice needs (swatch / paint chip +
    // gradient type + direction dial / note), the Fill dialog's onContentsChanged() band.
    void syncFillContext();
    void openFillGradientFlyout();
    void openFillPatternFlyout();
    void closeFillFlyouts();
    // Is Inpaint offered at all in this mode? Canvas Size only: a rotation's wedges would need the
    // composite RE-ROTATED to seed the heal, which the panel cannot ask the host for mid-preview.
    [[nodiscard]] bool inpaintOffered() const { return m_mode == Mode::CanvasSize; }

    struct State;
    std::unique_ptr<State> m_state;
    AnchorGrid* m_grid = nullptr; // CanvasSize only; null in the other modes

    Mode m_mode = Mode::CanvasSize;
    std::uint32_t m_pxW = 1, m_pxH = 1; // the edited size, always in document pixels
    std::uint32_t m_docW = 1, m_docH = 1; // the document's own size (the drag's anchor reference)
    double m_dpi = 72.0;
    SizeUnit m_unit = SizeUnit::Pixels;
    bool m_constrain = true;
    double m_aspect = 1.0; // docW/docH, captured at openFor: the proportions lock's reference
    render::CanvasAnchor m_anchor = render::CanvasAnchor::Center;
    render::ResampleFilter m_filter = render::ResampleFilter::Auto;
    double m_angleDeg = 0.0;
    FillMode m_fill = FillMode::Transparent;
    bool m_syncing = false; // a value-set during a rebuild/seed must not fire onPreview

    // The three "..." fills' working state, kept across Fill-combo switches exactly as the Fill
    // dialog keeps its own (so flipping to Black and back does not throw a built gradient away).
    common::Color8 m_customColor{0, 0, 0, 255};
    core::vec::Gradient m_customGradient;
    core::vec::ProceduralPattern m_customPattern;

    ScrubRuler* m_ruler = nullptr;             // shared precision HUD (non-owning)
    GradientFlyout* m_gradientFlyout = nullptr; // host-owned editors for the paint fills
    PatternFlyout* m_patternFlyout = nullptr;

    std::function<common::Color8()> m_foreground;
    std::function<common::Color8()> m_background;
    std::function<void(const Fl_Widget*, common::Color8)> m_onEditFillColor;
    std::function<void(const Request&)> m_onPreview;
    std::function<void(const Request&)> m_onApply;
    std::function<void()> m_onCancel;
};

} // namespace mosaic::ui
