#include "ui/image_ops_panel.hpp"

#include "common/i18n.hpp"
#include "ui/gizmo_canvas.hpp"     // the software SDF-coverage rasterizer the anchor grid draws with
#include "ui/gradient_flyout.hpp"  // GradientFlyout + defaultGradient / directedGradientTransform
#include "ui/paint_chip.hpp"       // PaintChip + kGradTypeNames
#include "ui/pattern_flyout.hpp"   // PatternFlyout + defaultProceduralPattern
#include "ui/scrub_slider.hpp"     // ScrubSlider + ScrubRuler (the size rows' scrub gesture)
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Valuator.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mosaic::ui {

namespace {

using common::Vec2;

constexpr int kPanelW = 316;
constexpr int kPad = 14;
constexpr int kContentW = kPanelW - 2 * kPad; // 288: the full-width band
constexpr int kHeaderH = 42;
constexpr int kRowH = 26;
constexpr int kRowGap = 8;
constexpr int kFooterH = 44;
// The panel's three-column grid, shared by every row so the modes read as one control set:
//   A = the caption, B = the GESTURE (a scrub slider / a dial), C = the exact-entry field.
// A dropdown spans B+C (kWideW); the contextual paint band spans the whole content width.
constexpr int kCapW = 70;
constexpr int kColBX = kPad + kCapW + 6;          // 90
constexpr int kFieldW = 88;
constexpr int kFieldX = kPanelW - kPad - kFieldW; // 214
constexpr int kScrubW = kFieldX - 6 - kColBX;     // 118
constexpr int kWideW = kPanelW - kPad - kColBX;   // 212
constexpr int kDialSide = 34;
constexpr int kAngleRowH = 36; // the rotation dial's row (taller than a plain field row)
constexpr int kCtxH = 34;      // the Fill choice's contextual band (chip 28 high, dial 34)
constexpr int kChipH = 28;
constexpr int kGradTypeW = 84; // Linear/Radial/Conic, left of the gradient paint chip
constexpr int kGradGap = 6;
constexpr int kGridSide = 96; // the 3x3 anchor control's footprint (3 cells of 30 + the focus ring)
constexpr int kBtnW = 80;
constexpr int kBtnH = 28;

// Margin reserved around the 3x3 grid for the keyboard-focus frame (drawn 2px out, 1px thick).
constexpr int kFocusRing = 3;

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// A control's binding: the closure to run when it fires (FLTK callbacks are C thunks + a void*),
// mirroring the adjustment / morphology panels' pattern.
struct Binding {
    std::function<void()> fn;
};
void controlThunk(Fl_Widget*, void* b) {
    if (auto* bb = static_cast<Binding*>(b))
        bb->fn();
}

// The Resample-quality list, in the SAME order as MainWindow::currentResampleFilter's map, so the
// dropdown index is the enum's index and a reordering of either list cannot silently mis-map.
constexpr render::ResampleFilter kFilters[] = {
    render::ResampleFilter::Auto,     render::ResampleFilter::Nearest,
    render::ResampleFilter::Bilinear, render::ResampleFilter::Bicubic,
    render::ResampleFilter::Mitchell, render::ResampleFilter::Lanczos2,
    render::ResampleFilter::Lanczos3, render::ResampleFilter::Area,
    render::ResampleFilter::Gaussian, render::ResampleFilter::Supersample};

int filterIndex(render::ResampleFilter f) {
    for (int i = 0; i < static_cast<int>(std::size(kFilters)); ++i)
        if (kFilters[i] == f)
            return i;
    return 0;
}

// ---- the anchor control's anti-aliased chrome ---------------------------------------------------
// Everything below rasterizes from a signed distance with a 1 px feather (ui::GizmoCanvas), which is
// what makes a 45-degree arrow a real diagonal instead of a staircase of rectangles.

// A 1 px hairline pinned to an EXACT pixel column / row. The stroke's capsule has radius 0.5, so a
// centre line at k + 0.5 covers pixel k fully and neither neighbour at all; the half-pixel inset at
// each end stops the round caps bleeding a half-lit pixel past the grid. `px`/`py` are pixel
// indices; the span runs from `from` up to (not including) `to`.
void hairlineV(GizmoCanvas& gc, double px, double from, double to, common::Color8 c) {
    gc.stroke({px + 0.5, from + 0.5}, {px + 0.5, to - 0.5}, 1.0, c, 1.0F);
}
void hairlineH(GizmoCanvas& gc, double from, double to, double py, common::Color8 c) {
    gc.stroke({from + 0.5, py + 0.5}, {to - 0.5, py + 0.5}, 1.0, c, 1.0F);
}

// One radiating arrow: an anti-aliased shaft with a two-stroke chevron head, centred on (cx, cy)
// and pointing along the UNIT vector (ux, uy). A chevron rather than a filled triangle because two
// capsules read crisper at this size than a rasterized polygon, and they share the shaft's weight.
void drawArrow(GizmoCanvas& gc, double cx, double cy, double ux, double uy, double len,
               double stroke, common::Color8 c) {
    const Vec2 tip{cx + ux * len * 0.5, cy + uy * len * 0.5};
    const Vec2 tail{cx - ux * len * 0.5, cy - uy * len * 0.5};
    gc.stroke(tail, tip, stroke, c, 1.0F);
    const double head = len * 0.44;
    const double perpX = -uy; // the shaft's perpendicular
    const double perpY = ux;
    const double bx = tip.x - ux * head;
    const double by = tip.y - uy * head;
    gc.stroke(tip, {bx + perpX * head * 0.75, by + perpY * head * 0.75}, stroke, c, 1.0F);
    gc.stroke(tip, {bx - perpX * head * 0.75, by - perpY * head * 0.75}, stroke, c, 1.0F);
}

// Which of the three placements along one axis a dragged preview rect implies: `offset` is where
// the OLD canvas's edge sits inside the new one and `slack` is how much room there is to place it,
// so the answer is whichever of flush-start / centred / flush-end the offset is nearest.
int anchorAxisFor(long offset, long slack) {
    if (slack <= 0)
        return 1; // no room to place: the axis is centred by definition
    const long mid = slack / 2;
    const long dStart = std::labs(offset);
    const long dMid = std::labs(offset - mid);
    const long dEnd = std::labs(offset - slack);
    if (dStart <= dMid && dStart <= dEnd)
        return 0;
    if (dEnd <= dMid)
        return 2;
    return 1;
}

} // namespace

// ================================================================================================
// AnchorGrid
// ================================================================================================

AnchorGrid::AnchorGrid(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {
    box(FL_NO_BOX); // we paint the whole footprint ourselves (including the erase)
}

render::CanvasAnchor AnchorGrid::anchorFor(int col, int row) {
    const int c = std::clamp(col, 0, 2);
    const int r = std::clamp(row, 0, 2);
    return static_cast<render::CanvasAnchor>(r * 3 + c);
}

void AnchorGrid::setValue(render::CanvasAnchor a) {
    if (a == m_anchor)
        return;
    m_anchor = a;
    redraw();
}

void AnchorGrid::moveBy(int dx, int dy) {
    // Clamped, never wrapping: a keypress that jumped from "top-left" to "top-right" would move
    // the picture across the whole canvas with nothing on screen to explain it.
    const int c = std::clamp(columnOf(m_anchor) + dx, 0, 2);
    const int r = std::clamp(rowOf(m_anchor) + dy, 0, 2);
    const render::CanvasAnchor next = anchorFor(c, r);
    if (next == m_anchor)
        return; // already at the edge: no change, and therefore no preview churn
    m_anchor = next;
    redraw();
    do_callback();
}

// The cell pitch, in WHOLE pixels, with kFocusRing px reserved on every side so the focus frame
// drawn just outside the grid still lands inside the widget's own rect (drawing past it would
// smear over the panel).
int AnchorGrid::cellSize() const {
    return std::max(1, (std::min(w(), h()) - 2 * kFocusRing) / 3);
}
int AnchorGrid::gridX() const {
    return x() + (w() - cellSize() * 3) / 2;
}
int AnchorGrid::gridY() const {
    return y() + (h() - cellSize() * 3) / 2;
}

int AnchorGrid::cellAt(int ex, int ey, int& col, int& row) const {
    col = -1;
    row = -1;
    const int cell = cellSize();
    const int c = (ex - gridX()) / cell;
    const int r = (ey - gridY()) / cell;
    if (ex < gridX() || ey < gridY() || c < 0 || c > 2 || r < 0 || r > 2)
        return 0;
    col = c;
    row = r;
    return 1;
}

void AnchorGrid::draw() {
    const Palette& pal = activePalette();
    const bool on = active_r() != 0;
    const int cell = cellSize();
    const int side = cell * 3;
    // Widget-LOCAL space: the GizmoCanvas's origin is this widget's corner, and its ground doubles
    // as the erase FLTK's partial redraws never do for us (the standing draw()-must-erase trap).
    const double lx = gridX() - x();
    const double ly = gridY() - y();
    const int selCol = columnOf(m_anchor);
    const int selRow = rowOf(m_anchor);
    GizmoCanvas gc(w(), h(), m_hasGround ? m_ground : pal.panelBg);

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const bool sel = c == selCol && r == selRow;
            const bool hov = on && c == m_hoverCol && r == m_hoverRow;
            common::Color8 cellColor = pal.controlBg;
            if (sel)
                cellColor = on ? pal.accent : pal.controlActive;
            else if (hov)
                cellColor = pal.controlHover;
            // Cell edges land on exact pixel boundaries, so the fills tile seamlessly and the
            // hairlines below sit on top of them rather than between two soft edges.
            gc.fillSquare({lx + c * cell + cell * 0.5, ly + r * cell + cell * 0.5}, cell * 0.5,
                          cellColor, 1.0F);
        }
    }

    // The grid hairlines: the ONE thing here that stays pixel-pinned. A blurred grid is exactly
    // what this control must not look like, and a 0.5 px-wide capsule centred on a pixel centre is
    // both crisp and consistent with the coverage renderer drawing everything else.
    for (int i = 0; i <= 3; ++i) {
        const int off = i == 3 ? -1 : 0; // the last line sits INSIDE the grid's right/bottom edge
        hairlineV(gc, lx + i * cell + off, ly, ly + side, pal.border);
        hairlineH(gc, lx, lx + side, ly + i * cell + off, pal.border);
    }

    // The picture's own mark, in the selected cell: "the image sits HERE".
    gc.fillSquare({lx + selCol * cell + cell * 0.5, ly + selRow * cell + cell * 0.5}, cell * 0.17,
                  on ? pal.onAccent : pal.textMuted, 1.0F);

    // The eight arrows radiate OUT of the selected cell, one into each neighbouring cell that
    // exists -- so the control reads as "the picture sits here, the canvas grows that way".
    const common::Color8 ink = on ? pal.text : pal.textMuted;
    const double len = cell * 0.46;
    const double weight = std::max(1.4, cell / 18.0);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0)
                continue;
            const int c = selCol + dx;
            const int r = selRow + dy;
            if (c < 0 || c > 2 || r < 0 || r > 2)
                continue;
            const double norm = (dx != 0 && dy != 0) ? 0.7071067811865476 : 1.0;
            drawArrow(gc, lx + c * cell + cell * 0.5, ly + r * cell + cell * 0.5, dx * norm,
                      dy * norm, len, weight, ink);
        }
    }

    // Keyboard focus: a 1px accent frame just outside the grid, so arrow-key navigation is
    // discoverable without a dotted FLTK focus rectangle (Fl::visible_focus is off app-wide).
    if (Fl::focus() == this && on) {
        hairlineH(gc, lx - 2, lx + side + 2, ly - 2, pal.accent);
        hairlineH(gc, lx - 2, lx + side + 2, ly + side + 1, pal.accent);
        hairlineV(gc, lx - 2, ly - 2, ly + side + 2, pal.accent);
        hairlineV(gc, lx + side + 1, ly - 2, ly + side + 2, pal.accent);
    }

    Fl_RGB_Image blit(gc.data(), w(), h(), 4);
    blit.draw(x(), y());
}

int AnchorGrid::handle(int event) {
    switch (event) {
        case FL_ENTER:
            if (window() != nullptr)
                window()->cursor(FL_CURSOR_HAND); // it is a control, and it says so
            return 1;
        case FL_LEAVE:
            if (window() != nullptr)
                window()->cursor(FL_CURSOR_DEFAULT);
            if (m_hoverCol >= 0 || m_hoverRow >= 0) {
                m_hoverCol = -1;
                m_hoverRow = -1;
                redraw();
            }
            return 1;
        case FL_MOVE: {
            int c = -1;
            int r = -1;
            // A miss leaves c/r at -1, which is exactly the "nothing hovered" state we want.
            (void)cellAt(Fl::event_x(), Fl::event_y(), c, r);
            if (c != m_hoverCol || r != m_hoverRow) {
                m_hoverCol = c;
                m_hoverRow = r;
                redraw();
            }
            return 1;
        }
        case FL_PUSH: {
            int c = -1;
            int r = -1;
            if (cellAt(Fl::event_x(), Fl::event_y(), c, r) != 0) {
                take_focus(); // so the arrow keys can carry on from where the click landed
                const render::CanvasAnchor next = anchorFor(c, r);
                if (next != m_anchor) {
                    m_anchor = next;
                    redraw();
                    do_callback();
                }
            }
            return 1;
        }
        case FL_FOCUS:
        case FL_UNFOCUS: redraw(); return 1;
        case FL_KEYBOARD:
            switch (Fl::event_key()) {
                case FL_Left: moveBy(-1, 0); return 1;
                case FL_Right: moveBy(1, 0); return 1;
                case FL_Up: moveBy(0, -1); return 1;
                case FL_Down: moveBy(0, 1); return 1;
                default: break;
            }
            break;
        default: break;
    }
    return Fl_Widget::handle(event);
}

// ================================================================================================
// ImageOpsPanel
// ================================================================================================

struct ImageOpsPanel::State {
    NumberField* width = nullptr;
    NumberField* height = nullptr;
    NumberField* angle = nullptr;
    ScrubSlider* widthScrub = nullptr;  // the size rows' scrub gesture (typing stays in the field)
    ScrubSlider* heightScrub = nullptr;
    Dial* angleDial = nullptr;          // rotation is cyclic: a knob, not a bare number
    Dropdown* unit = nullptr;
    Dropdown* filter = nullptr;
    Dropdown* fill = nullptr;
    // The Fill choice's contextual band (the Edit->Fill... dialog's own): exactly one of these is
    // shown at a time, chosen by syncFillContext().
    Dropdown* gradType = nullptr;
    Dial* gradDir = nullptr;
    SwatchChip* swatch = nullptr;
    PaintChip* paintChip = nullptr;
    Fl_Box* fillNote = nullptr;
    CheckBox* constrain = nullptr;
    std::vector<std::unique_ptr<Binding>> bindings;
};

ImageOpsPanel::ImageOpsPanel() : Popover(kPanelW, 100), m_state(std::make_unique<State>()) {
    setPinned(true); // survives canvas/chrome clicks, like the Type panel; Esc still closes
    end();           // Popover's ctor leaves the group open; build() manages its own begin/end
    build();
}

ImageOpsPanel::~ImageOpsPanel() = default;

void ImageOpsPanel::setPlacementProviders(std::function<common::Rect()> region) {
    setCornerPlacement(Corner::BottomRight, std::move(region));
}

void ImageOpsPanel::setFillColorProviders(std::function<common::Color8()> foreground,
                                          std::function<common::Color8()> background) {
    m_foreground = std::move(foreground);
    m_background = std::move(background);
}

void ImageOpsPanel::setScrubRuler(ScrubRuler* r) {
    m_ruler = r;
    // build() ran in the ctor, so push onto the live sliders too (the morphology panel's wiring).
    if (m_state->widthScrub != nullptr)
        m_state->widthScrub->setRuler(r);
    if (m_state->heightScrub != nullptr)
        m_state->heightScrub->setRuler(r);
}

void ImageOpsPanel::setPaintFlyouts(GradientFlyout* gradient, PatternFlyout* pattern) {
    m_gradientFlyout = gradient;
    m_patternFlyout = pattern;
}

void ImageOpsPanel::setCustomFillColor(common::Color8 c) {
    m_customColor = c;
    if (m_state->swatch != nullptr && m_fill == FillMode::Custom)
        m_state->swatch->setColour(c);
    if (m_fill == FillMode::Custom)
        firePreview();
}

std::uint32_t ImageOpsPanel::clampDimension(double px) const {
    if (!(px > 0.0)) // also catches NaN
        return 1;
    const double rounded = std::floor(px + 0.5);
    if (rounded > static_cast<double>(kMaxCanvasDimension))
        return kMaxCanvasDimension;
    return static_cast<std::uint32_t>(rounded);
}

void ImageOpsPanel::setConstrainProportions(bool on) {
    m_constrain = on;
    // Engaging the lock adopts whatever ratio is shown RIGHT NOW (the New Document dialog's
    // link-button rule) rather than snapping back to the document's -- otherwise a deliberate
    // re-shape would be silently undone by ticking the box.
    if (on && m_pxH > 0)
        m_aspect = static_cast<double>(m_pxW) / static_cast<double>(m_pxH);
    if (m_state->constrain != nullptr)
        m_state->constrain->setChecked(on); // no-op when this came FROM the checkbox
}

void ImageOpsPanel::applyConstraint(bool widthEdited) {
    if (!m_constrain || m_aspect <= 0.0)
        return;
    if (widthEdited)
        m_pxH = clampDimension(static_cast<double>(m_pxW) / m_aspect);
    else
        m_pxW = clampDimension(static_cast<double>(m_pxH) * m_aspect);
}

common::Color8 ImageOpsPanel::currentFillColor() const {
    switch (m_fill) {
        case FillMode::Foreground:
            return m_foreground ? m_foreground() : common::Color8{0, 0, 0, 255};
        case FillMode::Background:
            return m_background ? m_background() : common::Color8{255, 255, 255, 255};
        case FillMode::White: return {255, 255, 255, 255};
        case FillMode::Black: return {0, 0, 0, 255};
        case FillMode::Gray: return {128, 128, 128, 255};
        case FillMode::Custom: return m_customColor;
        case FillMode::Gradient: // not a flat colour; the chip / request use currentPaint()
        case FillMode::Pattern:
            return m_foreground ? m_foreground() : common::Color8{0, 0, 0, 255};
        case FillMode::Transparent:
        case FillMode::Inpaint: break; // no colour at all: nothing, and healed pixels
    }
    return {0, 0, 0, 255};
}

core::vec::Paint ImageOpsPanel::currentPaint() const {
    switch (m_fill) {
        case FillMode::Gradient: return m_customGradient;
        case FillMode::Pattern: return core::vec::Pattern{m_customPattern};
        default: return core::vec::SolidPaint{common::toColorF(currentFillColor())};
    }
}

ImageOpsPanel::Request ImageOpsPanel::request() const {
    Request r;
    r.mode = m_mode;
    r.width = m_pxW;
    r.height = m_pxH;
    r.anchor = m_anchor;
    r.filter = m_filter;
    r.angleDeg = m_angleDeg;
    r.fillMode = m_fill;
    r.paint = currentPaint();
    // Transparent means "add nothing", which the engine spells as no CropFill at all -- not a
    // zero-alpha colour (that would still mint a fill layer for the fallback stack shape). The
    // paint kinds and Inpaint are left unresolved on purpose: both cost a whole new-canvas-sized
    // image, and a preview fires on every keystroke, so the HOST materializes them on Apply.
    switch (m_fill) {
        case FillMode::Transparent:
        case FillMode::Gradient:
        case FillMode::Pattern:
        case FillMode::Inpaint: break;
        default: {
            render::CropFill fill;
            fill.color = currentFillColor();
            fill.layerName = _("Canvas fill"); // the SAME msgid the Crop tool's expansion uses
            r.fill = std::move(fill);
            break;
        }
    }
    return r;
}

void ImageOpsPanel::firePreview() {
    if (m_syncing || !m_onPreview)
        return;
    m_onPreview(request());
}

void ImageOpsPanel::build() {
    const Palette& pal = activePalette();
    closeFillFlyouts(); // their anchor chip is about to be deleted out from under them
    clear();            // drop the previous controls (a mode switch or a theme rebuild)
    resizable(nullptr); // clear() re-arms proportional scaling; this layout is fixed (the LE rule)
    *m_state = State{}; // every cached pointer just died with the widgets
    m_grid = nullptr;

    const bool sizing = m_mode != Mode::RotateArbitrary;
    const bool canvasSize = m_mode == Mode::CanvasSize;
    const bool rotating = m_mode == Mode::RotateArbitrary;
    const bool resamples = m_mode == Mode::ImageSize || rotating;
    const bool fillable = canvasSize || rotating;

    int contentH = kHeaderH;
    if (sizing)
        contentH += 4 * (kRowH + kRowGap); // width, height, units, constrain
    if (rotating)
        contentH += kAngleRowH + kRowGap; // angle (field + dial)
    if (canvasSize)
        contentH += kGridSide + kRowGap; // the nine-point anchor
    if (resamples)
        contentH += kRowH + kRowGap; // resample quality
    if (fillable)
        contentH += kRowH + kRowGap + kCtxH + kRowGap; // the Fill combo + its contextual band
    contentH += kFooterH;
    setBaseSize(kPanelW, contentH);

    begin();

    const auto bind = [&](std::function<void()> fn) {
        auto b = std::make_unique<Binding>();
        b->fn = std::move(fn);
        Binding* raw = b.get();
        m_state->bindings.push_back(std::move(b));
        return raw;
    };
    const auto caption = [&](int cy, int rowH, const char* text) {
        auto* c = new Fl_Box(kPad, cy, kCapW, rowH, text);
        c->box(FL_NO_BOX);
        c->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        c->labelsize(12);
        c->labelcolor(toFl(pal.text));
    };

    // ---- Header ----
    const char* title = _("Canvas Size");
    if (m_mode == Mode::ImageSize)
        title = _("Image Size");
    else if (rotating)
        title = _("Rotate Canvas");
    auto* head = new Fl_Box(kPad, 12, kContentW, 20, title);
    head->box(FL_NO_BOX);
    head->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    head->labelfont(FL_HELVETICA_BOLD);
    head->labelsize(13);
    head->labelcolor(toFl(pal.text));

    int cy = kHeaderH;

    if (sizing) {
        // ---- Width / Height, in the current unit -------------------------------------------
        // Each axis carries BOTH gestures: a scrub slider (drag for the shape you want, pull away
        // for precision + the ruler HUD) and the exact-entry NumberField beside it, which is the
        // one that speaks "1024*2" and is locale-independent. Neither replaces the other.
        caption(cy, kRowH, _("Width"));
        auto* ws = new ScrubSlider(kColBX, cy, kScrubW, kRowH);
        ws->setCellColor(pal.panelBg);
        ws->setResponseCurve(ScrubCurve::Gamma, 2.5); // the small end gets most of the track
        ws->setRuler(m_ruler);
        ws->when(FL_WHEN_CHANGED);
        ws->callback(controlThunk, bind([this] {
                         if (m_syncing || m_state->widthScrub == nullptr)
                             return;
                         m_pxW = clampDimension(
                             unitToPixels(m_state->widthScrub->value(), m_unit, m_dpi));
                         applyConstraint(/*widthEdited=*/true);
                         pushSizeField(/*width=*/true); // a slider drag owns no caret to disturb
                         pushSizeField(/*width=*/false);
                         pushSizeSlider(/*width=*/false); // only the PARTNER slider, never this one
                         firePreview();
                     }));
        m_state->widthScrub = ws;
        auto* wf = new NumberField(kFieldX, cy, kFieldW, kRowH);
        wf->when(FL_WHEN_CHANGED); // live: every keystroke re-previews (the NDD field convention)
        wf->callback(controlThunk, bind([this] {
                         if (m_syncing || m_state->width == nullptr)
                             return;
                         double v = 0.0;
                         if (!parseFieldNumber(m_state->width->value(), v))
                             return; // mid-typing ("", "1024*"): leave the model alone
                         m_pxW = clampDimension(unitToPixels(v, m_unit, m_dpi));
                         applyConstraint(/*widthEdited=*/true);
                         // Only the PARTNER field is rewritten: re-writing the field being typed
                         // into would send its caret to the end on every reformat.
                         pushSizeField(/*width=*/false);
                         pushSizeSlider(/*width=*/true);
                         pushSizeSlider(/*width=*/false);
                         firePreview();
                     }));
        m_state->width = wf;
        cy += kRowH + kRowGap;

        caption(cy, kRowH, _("Height"));
        auto* hs = new ScrubSlider(kColBX, cy, kScrubW, kRowH);
        hs->setCellColor(pal.panelBg);
        hs->setResponseCurve(ScrubCurve::Gamma, 2.5);
        hs->setRuler(m_ruler);
        hs->when(FL_WHEN_CHANGED);
        hs->callback(controlThunk, bind([this] {
                         if (m_syncing || m_state->heightScrub == nullptr)
                             return;
                         m_pxH = clampDimension(
                             unitToPixels(m_state->heightScrub->value(), m_unit, m_dpi));
                         applyConstraint(/*widthEdited=*/false);
                         pushSizeField(/*width=*/true);
                         pushSizeField(/*width=*/false);
                         pushSizeSlider(/*width=*/true);
                         firePreview();
                     }));
        m_state->heightScrub = hs;
        auto* hf = new NumberField(kFieldX, cy, kFieldW, kRowH);
        hf->when(FL_WHEN_CHANGED);
        hf->callback(controlThunk, bind([this] {
                         if (m_syncing || m_state->height == nullptr)
                             return;
                         double v = 0.0;
                         if (!parseFieldNumber(m_state->height->value(), v))
                             return;
                         m_pxH = clampDimension(unitToPixels(v, m_unit, m_dpi));
                         applyConstraint(/*widthEdited=*/false);
                         pushSizeField(/*width=*/true);
                         pushSizeSlider(/*width=*/true);
                         pushSizeSlider(/*width=*/false);
                         firePreview();
                     }));
        m_state->height = hf;
        cy += kRowH + kRowGap;

        // ---- Units ---------------------------------------------------------------------------
        // The pixel size is the model; the unit only changes how it READS, so switching units
        // never resizes anything (and never fires a preview).
        caption(cy, kRowH, _("Units"));
        auto* unit = new Dropdown(kColBX, cy, kWideW, kRowH);
        for (int u = 0; u <= static_cast<int>(SizeUnit::Points); ++u)
            unit->add(std::string(sizeUnitName(static_cast<SizeUnit>(u))).c_str(), 0, nullptr,
                      nullptr, 0);
        unit->callback(controlThunk, bind([this] {
                           if (m_syncing || m_state->unit == nullptr)
                               return;
                           m_unit = static_cast<SizeUnit>(m_state->unit->value());
                           pushSizeField(/*width=*/true); // re-express the same pixels, new unit
                           pushSizeField(/*width=*/false);
                           pushSizeSlider(/*width=*/true); // ... and re-range the scrub gesture
                           pushSizeSlider(/*width=*/false);
                       }));
        m_state->unit = unit;
        cy += kRowH + kRowGap;

        // ---- Constrain proportions -------------------------------------------------------------
        auto* lock = new CheckBox(kPad, cy, kContentW, kRowH, _("Constrain Proportions"));
        lock->setGroundColor(pal.panelBg);
        lock->setOnToggle([this](bool on) {
            if (!m_syncing)
                setConstrainProportions(on);
        });
        m_state->constrain = lock;
        cy += kRowH + kRowGap;
    }

    if (rotating) {
        // ---- Angle: a DIAL plus the exact field ------------------------------------------------
        // An angle is cyclic, which a number field models badly -- "point it down-left" is a
        // gesture, not arithmetic (ui::Dial's own argument). The field keeps the exact value.
        caption(cy, kAngleRowH, _("Angle"));
        auto* dial = new Dial(kColBX + (kScrubW - kDialSide) / 2, cy + (kAngleRowH - kDialSide) / 2,
                              kDialSide, kDialSide);
        dial->range(-180.0, 180.0); // a full turn, so the knob wraps instead of sticking at an end
        dial->step(1.0);
        dial->setCellColor(pal.panelBg);
        dial->setDefaultValue(0.0); // middle / Ctrl click = back to square
        dial->when(FL_WHEN_CHANGED);
        dial->tooltip(_("Drag to turn the canvas; hold Shift to snap"));
        dial->callback(controlThunk, bind([this] {
                           if (m_syncing || m_state->angleDial == nullptr)
                               return;
                           m_angleDeg = m_state->angleDial->value();
                           if (m_state->angle != nullptr) {
                               const bool wasSyncing = m_syncing;
                               m_syncing = true;
                               m_state->angle->value(formatFieldNumber(m_angleDeg, 0.0).c_str());
                               m_syncing = wasSyncing;
                           }
                           firePreview();
                       }));
        m_state->angleDial = dial;
        auto* af = new NumberField(kFieldX, cy + (kAngleRowH - kRowH) / 2, kFieldW, kRowH);
        af->when(FL_WHEN_CHANGED);
        af->callback(controlThunk, bind([this] {
                         if (m_syncing || m_state->angle == nullptr)
                             return;
                         double v = 0.0;
                         if (!parseFieldNumber(m_state->angle->value(), v))
                             return;
                         m_angleDeg = v;
                         if (m_state->angleDial != nullptr) {
                             const bool wasSyncing = m_syncing;
                             m_syncing = true;
                             // The typed value is kept verbatim (450 deg IS a legal turn); only the
                             // NEEDLE is folded into the knob's range.
                             m_state->angleDial->value(wrapDialValue(m_angleDeg, -180.0, 180.0));
                             m_syncing = wasSyncing;
                         }
                         firePreview();
                     }));
        m_state->angle = af;
        cy += kAngleRowH + kRowGap;
    }

    if (canvasSize) {
        // ---- The nine-point anchor -------------------------------------------------------------
        caption(cy, kRowH, _("Anchor"));
        auto* grid = new AnchorGrid(kPanelW - kPad - kGridSide, cy, kGridSide, kGridSide);
        grid->setGroundColor(pal.panelBg);
        grid->callback(controlThunk, bind([this] {
                           if (m_syncing || m_grid == nullptr)
                               return;
                           m_anchor = m_grid->value();
                           firePreview();
                       }));
        m_grid = grid;
        cy += kGridSide + kRowGap;
    }

    if (resamples) {
        // ---- Resample quality ------------------------------------------------------------------
        caption(cy, kRowH, _("Resample"));
        auto* filter = new Dropdown(kColBX, cy, kWideW, kRowH);
        // Same labels + order as the Move tool's Anti-aliasing combo (tool.cpp), so one kernel
        // is named one way everywhere. "Auto" leads: it is the Request's default, and a list
        // that could not show its own default would be a lie the first time the panel opens.
        for (const char* label : {_("Auto"), _("Nearest"), _("Bilinear"), _("Bicubic"),
                                  _("Mitchell"), _("Lanczos 2"), _("Lanczos 3"), _("Area (box)"),
                                  _("Gaussian"), _("Supersample")})
            filter->add(label, 0, nullptr, nullptr, 0);
        filter->callback(controlThunk, bind([this] {
                             if (m_syncing || m_state->filter == nullptr)
                                 return;
                             const int i = m_state->filter->value();
                             if (i >= 0 && i < static_cast<int>(std::size(kFilters)))
                                 m_filter = kFilters[i];
                             firePreview();
                         }));
        m_state->filter = filter;
        cy += kRowH + kRowGap;
    }

    if (fillable) {
        // ---- Expansion fill --------------------------------------------------------------------
        // The Edit->Fill... dialog's own Contents family, in its order, with the SAME msgids (so
        // one fill is named one way everywhere), plus Transparent -- which only an expansion can
        // mean. The dividers group it exactly as the modal does: nothing / solids / paints /
        // reconstruction.
        caption(cy, kRowH, _("Fill"));
        auto* fill = new Dropdown(kColBX, cy, kWideW, kRowH);
        fill->add(_("Transparent"), 0, nullptr, nullptr, FL_MENU_DIVIDER); // nothing v solids
        fill->add(_("Foreground Color"), 0, nullptr, nullptr, 0);
        fill->add(_("Background Color"), 0, nullptr, nullptr, 0);
        fill->add(_("White"), 0, nullptr, nullptr, 0);
        fill->add(_("Black"), 0, nullptr, nullptr, 0);
        fill->add(_("50% Gray"), 0, nullptr, nullptr, 0);
        fill->add(_("Color\xE2\x80\xA6"), 0, nullptr, nullptr, FL_MENU_DIVIDER); // solids v paints
        fill->add(_("Gradient\xE2\x80\xA6"), 0, nullptr, nullptr, 0);
        fill->add(_("Pattern\xE2\x80\xA6"), 0, nullptr, nullptr,
                  inpaintOffered() ? FL_MENU_DIVIDER : 0);
        // Inpaint is a CANVAS SIZE fill only. A rotation's corner wedges would need the composite
        // re-rotated to seed the heal, which is not something the panel can ask for mid-preview --
        // and the Crop tool's rotated fills already exclude it for the same reason.
        if (inpaintOffered())
            fill->add(_("Inpaint"), 0, nullptr, nullptr, 0);
        fill->callback(controlThunk, bind([this] {
                           if (m_syncing || m_state->fill == nullptr)
                               return;
                           const int i = std::clamp(m_state->fill->value(), 0,
                                                    static_cast<int>(FillMode::Inpaint));
                           m_fill = static_cast<FillMode>(i);
                           syncFillContext();
                           firePreview();
                       }));
        m_state->fill = fill;
        cy += kRowH + kRowGap;

        // ---- The Fill choice's contextual band -------------------------------------------------
        // All of these share one band and syncFillContext() shows exactly one: a swatch chip for
        // the solid contents (interactive only for "Color..."), a type dropdown + paint chip +
        // direction dial for a gradient, the chip alone for a pattern, a muted note for the two
        // that have no colour at all. Lifted wholesale from FillDialog::onContentsChanged.
        const int ctxY = cy;
        const int chipY = ctxY + (kCtxH - kChipH) / 2;
        auto* swatch = new SwatchChip(kPad, chipY, kContentW, kChipH);
        swatch->setGroundColor(pal.panelBg);
        swatch->setOnClick([this] {
            if (m_fill == FillMode::Custom && m_onEditFillColor && m_state->swatch != nullptr)
                m_onEditFillColor(m_state->swatch, m_customColor);
        });
        m_state->swatch = swatch;

        auto* gradType = new Dropdown(kPad, chipY, kGradTypeW, kChipH);
        for (const char* n : kGradTypeNames)
            gradType->add(n, 0, nullptr, nullptr, 0);
        gradType->callback(controlThunk, bind([this] {
                               if (m_syncing || m_state->gradType == nullptr)
                                   return;
                               const auto t = static_cast<core::vec::GradientType>(
                                   std::clamp(m_state->gradType->value(), 0, 2));
                               m_customGradient.type = t;
                               // Re-place unit-space for the new type, keeping the dial's direction
                               // (type + transform are panel-owned; the flyout only edits stops).
                               m_customGradient.transform = directedGradientTransform(
                                   t, m_state->gradDir != nullptr ? m_state->gradDir->value() : 0.0);
                               if (m_state->paintChip != nullptr)
                                   m_state->paintChip->setPaint(currentPaint());
                               firePreview();
                           }));
        gradType->hide();
        m_state->gradType = gradType;

        auto* chip = new PaintChip(kPad, chipY, kContentW, kChipH);
        chip->setGroundColor(pal.panelBg);
        chip->setOnClick([this] {
            if (m_fill == FillMode::Gradient)
                openFillGradientFlyout();
            else if (m_fill == FillMode::Pattern)
                openFillPatternFlyout();
        });
        chip->hide();
        m_state->paintChip = chip;

        auto* dir = new Dial(kPad + kContentW - kDialSide, ctxY, kDialSide, kDialSide);
        dir->range(0, 360);
        dir->step(1);
        dir->setCellColor(pal.panelBg);
        dir->when(FL_WHEN_CHANGED);
        dir->tooltip(_("Gradient direction"));
        dir->callback(controlThunk, bind([this] {
                          if (m_syncing || m_state->gradDir == nullptr)
                              return;
                          m_customGradient.transform = directedGradientTransform(
                              m_customGradient.type, m_state->gradDir->value());
                          if (m_state->paintChip != nullptr)
                              m_state->paintChip->setPaint(currentPaint());
                          firePreview();
                      }));
        dir->hide();
        m_state->gradDir = dir;

        auto* note = new Fl_Box(kPad, ctxY, kContentW, kCtxH, "");
        note->box(FL_NO_BOX);
        note->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        note->labelfont(FL_HELVETICA);
        note->labelsize(11);
        note->labelcolor(toFl(pal.textMuted));
        note->hide();
        m_state->fillNote = note;
        cy += kCtxH + kRowGap;
    }

    // ---- Footer: Cancel + Apply (right-aligned) ----
    const int btnY = cy + (kFooterH - kBtnH) / 2 - 2;
    const int applyX = kPanelW - kPad - kBtnW;
    const int cancelX = applyX - 8 - kBtnW;
    auto* cancel = new FlatButton(cancelX, btnY, kBtnW, kBtnH, _("Cancel"));
    cancel->callback(controlThunk, bind([this] {
                         if (m_onCancel)
                             m_onCancel();
                     }));
    auto* apply = new FilledButton(applyX, btnY, kBtnW, kBtnH, _("Apply"));
    apply->callback(controlThunk, bind([this] {
                        if (m_onApply)
                            m_onApply(request());
                    }));

    end();
    pushStateToControls(); // seed every freshly-built widget from the model
}

void ImageOpsPanel::pushSizeField(bool width) {
    NumberField* f = width ? m_state->width : m_state->height;
    if (f == nullptr)
        return;
    const auto px = static_cast<double>(width ? m_pxW : m_pxH);
    const bool wasSyncing = m_syncing;
    m_syncing = true;
    f->value(formatFieldNumber(pixelsToUnit(px, m_unit, m_dpi), sizeStep()).c_str());
    m_syncing = wasSyncing;
}

void ImageOpsPanel::pushSizeSlider(bool width) {
    ScrubSlider* s = width ? m_state->widthScrub : m_state->heightScrub;
    if (s == nullptr)
        return;
    const bool wasSyncing = m_syncing;
    m_syncing = true;
    // The scrub gesture speaks whatever unit the fields do, so its range + readout are re-derived
    // on every unit change. A whole-pixel step in Pixels; continuous (step 0) for the physical
    // units, where a 1/100 grid would quantise a millimetre into visible jumps.
    const double mn = pixelsToUnit(1.0, m_unit, m_dpi);
    const double mx = pixelsToUnit(static_cast<double>(kMaxCanvasDimension), m_unit, m_dpi);
    s->range(mn, mx);
    s->step(m_unit == SizeUnit::Pixels ? 1.0 : 0.0);
    s->setSuffix(std::string(sizeUnitAbbrev(m_unit)));
    s->value(pixelsToUnit(static_cast<double>(width ? m_pxW : m_pxH), m_unit, m_dpi));
    m_syncing = wasSyncing;
}

void ImageOpsPanel::syncFillContext() {
    if (m_state->fill == nullptr || m_state->swatch == nullptr)
        return; // not a fillable mode: there is no band to sync
    const bool gradient = m_fill == FillMode::Gradient;
    const bool pattern = m_fill == FillMode::Pattern;
    // Switching away from a paint closes the editor that no longer matches (the Fill dialog's rule).
    if (!gradient && m_gradientFlyout != nullptr && m_gradientFlyout->shown())
        m_gradientFlyout->hide();
    if (!pattern && m_patternFlyout != nullptr && m_patternFlyout->shown())
        m_patternFlyout->hide();

    SwatchChip* swatch = m_state->swatch;
    PaintChip* chip = m_state->paintChip;
    Dropdown* type = m_state->gradType;
    Dial* dial = m_state->gradDir;
    Fl_Box* note = m_state->fillNote;

    if (gradient || pattern) {
        swatch->hide();
        note->hide();
        if (gradient) {
            type->value(std::clamp(static_cast<int>(m_customGradient.type), 0, 2));
            type->show();
            dial->value(gradientDirectionDeg(m_customGradient));
            dial->show();
            chip->resize(kPad + kGradTypeW + kGradGap, chip->y(),
                         kContentW - kGradTypeW - kGradGap - kGradGap - kDialSide, chip->h());
        } else {
            type->hide();
            dial->hide();
            chip->resize(kPad, chip->y(), kContentW, chip->h());
        }
        chip->setPaint(currentPaint());
        chip->show();
    } else if (m_fill == FillMode::Transparent || m_fill == FillMode::Inpaint) {
        swatch->hide();
        chip->hide();
        type->hide();
        dial->hide();
        note->copy_label(m_fill == FillMode::Inpaint
                             ? _("Rebuilds the new margin from the picture beside it. Runs in the "
                                 "background when you Apply.")
                             : _("The area the canvas gains is left empty."));
        note->show();
    } else {
        chip->hide();
        type->hide();
        dial->hide();
        note->hide();
        swatch->setColour(currentFillColor());
        const bool custom = m_fill == FillMode::Custom;
        swatch->setInteractive(custom); // only "Color..." opens the picker on click
        swatch->tooltip(custom ? _("Click to choose a color") : nullptr);
        swatch->show();
    }
    redraw();
}

void ImageOpsPanel::openFillGradientFlyout() {
    if (m_gradientFlyout == nullptr || m_state->paintChip == nullptr)
        return;
    if (m_gradientFlyout->shownForAnchor(m_state->paintChip)) { // a re-click toggles it shut
        m_gradientFlyout->hide();
        return;
    }
    if (m_patternFlyout != nullptr) // one bubble on screen at a time
        m_patternFlyout->hide();
    // Re-pointed on every open, so sharing the host's editor with another opener is safe.
    m_gradientFlyout->setOnChange([this](const core::vec::Gradient& g) {
        // The flyout edits stops + spread; the panel owns the type (dropdown) and the direction
        // (dial), so those survive an edit made while the bubble was open.
        m_customGradient.stops = g.stops;
        m_customGradient.spread = g.spread;
        if (m_state->paintChip != nullptr)
            m_state->paintChip->setPaint(currentPaint());
        firePreview();
    });
    m_gradientFlyout->openFor(m_state->paintChip, m_customGradient);
}

void ImageOpsPanel::openFillPatternFlyout() {
    if (m_patternFlyout == nullptr || m_state->paintChip == nullptr)
        return;
    if (m_patternFlyout->shownForAnchor(m_state->paintChip)) {
        m_patternFlyout->hide();
        return;
    }
    if (m_gradientFlyout != nullptr)
        m_gradientFlyout->hide();
    m_patternFlyout->setOnChange([this](const core::vec::ProceduralPattern& pp) {
        m_customPattern = pp;
        if (m_state->paintChip != nullptr)
            m_state->paintChip->setPaint(currentPaint());
        firePreview();
    });
    m_patternFlyout->openFor(m_state->paintChip, m_customPattern);
}

void ImageOpsPanel::closeFillFlyouts() {
    if (m_gradientFlyout != nullptr && m_gradientFlyout->shown())
        m_gradientFlyout->hide();
    if (m_patternFlyout != nullptr && m_patternFlyout->shown())
        m_patternFlyout->hide();
}

void ImageOpsPanel::hide() {
    closeFillFlyouts(); // a bubble anchored to a chip that is about to vanish
    Popover::hide();
}

void ImageOpsPanel::setFillMode(FillMode m) {
    if (m == FillMode::Inpaint && !inpaintOffered())
        m = FillMode::Transparent; // never leave the choice on a row this mode does not offer
    m_fill = m;
    if (m_state->fill != nullptr) {
        const bool wasSyncing = m_syncing;
        m_syncing = true;
        m_state->fill->value(static_cast<int>(m_fill));
        m_syncing = wasSyncing;
    }
    syncFillContext();
}

void ImageOpsPanel::pushStateToControls() {
    const bool wasSyncing = m_syncing;
    m_syncing = true;
    pushSizeField(/*width=*/true);
    pushSizeField(/*width=*/false);
    pushSizeSlider(/*width=*/true);
    pushSizeSlider(/*width=*/false);
    if (m_state->angle != nullptr)
        m_state->angle->value(formatFieldNumber(m_angleDeg, 0.0).c_str());
    if (m_state->angleDial != nullptr)
        m_state->angleDial->value(wrapDialValue(m_angleDeg, -180.0, 180.0));
    if (m_state->unit != nullptr)
        m_state->unit->value(static_cast<int>(m_unit));
    if (m_state->filter != nullptr)
        m_state->filter->value(filterIndex(m_filter));
    if (m_state->fill != nullptr)
        m_state->fill->value(static_cast<int>(m_fill));
    if (m_state->constrain != nullptr)
        m_state->constrain->setChecked(m_constrain);
    if (m_grid != nullptr)
        m_grid->setValue(m_anchor);
    syncFillContext();
    m_syncing = wasSyncing;
}

void ImageOpsPanel::applyPreviewDrag(long x, long y, std::uint32_t w, std::uint32_t h) {
    if (m_mode == Mode::RotateArbitrary)
        return; // there the staged rect is a FUNCTION of the angle, not a size to drag out
    const std::uint32_t nw = clampDimension(static_cast<double>(w));
    const std::uint32_t nh = clampDimension(static_cast<double>(h));
    render::CanvasAnchor nextAnchor = m_anchor;
    if (m_mode == Mode::CanvasSize) {
        // (x, y) is the staged NEW canvas's top-left in CURRENT document pixels, so the OLD canvas
        // sits at (-x, -y) inside it -- which is exactly what the nine-point anchor names.
        nextAnchor = AnchorGrid::anchorFor(
            anchorAxisFor(-x, static_cast<long>(nw) - static_cast<long>(m_docW)),
            anchorAxisFor(-y, static_cast<long>(nh) - static_cast<long>(m_docH)));
    }
    if (nw == m_pxW && nh == m_pxH && nextAnchor == m_anchor)
        return; // the drag has not left the cell it was already in: no preview churn
    m_pxW = nw;
    m_pxH = nh;
    m_anchor = nextAnchor;
    // A handle drag is authoritative on BOTH axes; with the lock engaged it ADOPTS the dragged
    // ratio rather than snapping an axis back (setConstrainProportions' rule, from the other side).
    if (m_constrain && m_pxH > 0)
        m_aspect = static_cast<double>(m_pxW) / static_cast<double>(m_pxH);
    pushStateToControls(); // the panel still owns the numbers; this is what puts them on screen
    firePreview();
}

void ImageOpsPanel::configure(Mode mode, std::uint32_t docW, std::uint32_t docH, double dpi) {
    m_mode = mode;
    m_dpi = dpi > 0.0 ? dpi : 72.0;
    m_docW = std::max<std::uint32_t>(1, docW);
    m_docH = std::max<std::uint32_t>(1, docH);
    m_pxW = m_docW;
    m_pxH = m_docH;
    m_aspect = static_cast<double>(m_pxW) / static_cast<double>(m_pxH);
    m_angleDeg = 0.0;
    m_anchor = render::CanvasAnchor::Center;
    if (m_fill == FillMode::Inpaint && !inpaintOffered())
        m_fill = FillMode::Transparent; // Rotate does not offer it (see build())
    // Seed the three "..." fills from the live colours, exactly as FillDialog::seed does on every
    // open: a session starts from the colours you are working in, not from the last session's.
    const common::Color8 fg = m_foreground ? m_foreground() : common::Color8{0, 0, 0, 255};
    const common::Color8 bg = m_background ? m_background() : common::Color8{255, 255, 255, 255};
    m_customColor = fg;
    m_customGradient = defaultGradient(core::vec::GradientType::Linear, fg, bg);
    m_customPattern = defaultProceduralPattern(fg);
    build(); // the control set differs per mode, so every open regenerates it
}

void ImageOpsPanel::openFor(Mode mode, const Fl_Widget* anchor, std::uint32_t docW,
                            std::uint32_t docH, double dpi) {
    configure(mode, docW, docH, dpi);
    showAnchored(anchor); // corner placement drives geometry; the anchor is for bookkeeping
    firePreview();        // seed the canvas overlay with the identity request
}

void ImageOpsPanel::reapplyTheme() {
    Popover::reapplyTheme();
    build(); // rebuild the controls in the new palette (build() re-seeds them from the model)
}

int ImageOpsPanel::handle(int event) {
    if (event == FL_KEYBOARD || event == FL_SHORTCUT) {
        const int k = Fl::event_key();
        // Enter commits. Delegate to the base FIRST so a focused child -- a number field's
        // in-place edit, or a scrub slider's type-in -- consumes its own Enter; only when nothing
        // did do we Apply. Esc falls through to Popover::handle, which hides us; the host drops the
        // preview when it sees the panel closed (updateImageOps).
        if (k == FL_Enter || k == FL_KP_Enter) {
            if (Popover::handle(event))
                return 1;
            if (m_onApply)
                m_onApply(request());
            return 1;
        }
        // Arrow keys walk the anchor grid whenever the caret is NOT in a text field (where they
        // must keep moving the caret) and no valuator holds focus (the dial and the scrub sliders
        // nudge their own value with them). When the grid itself holds focus FLTK delivers the key
        // straight to it and this never runs.
        if (m_grid != nullptr && Fl::focus() != m_grid &&
            dynamic_cast<Fl_Input_*>(Fl::focus()) == nullptr &&
            dynamic_cast<Fl_Valuator*>(Fl::focus()) == nullptr) {
            switch (k) {
                case FL_Left: m_grid->moveBy(-1, 0); return 1;
                case FL_Right: m_grid->moveBy(1, 0); return 1;
                case FL_Up: m_grid->moveBy(0, -1); return 1;
                case FL_Down: m_grid->moveBy(0, 1); return 1;
                default: break;
            }
        }
    }
    return Popover::handle(event);
}

} // namespace mosaic::ui
