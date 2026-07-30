#include "ui/fill_dialog.hpp"

#include "common/i18n.hpp"
#include "render/region_fill.hpp"
#include "ui/color_flyout.hpp"
#include "ui/gradient_flyout.hpp"
#include "ui/paint_chip.hpp"
#include "ui/pattern_flyout.hpp"
#include "ui/preview_pane.hpp"
#include "ui/scrub_slider.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace mosaic::ui {

namespace {

constexpr int kWinW = 540;
constexpr int kWinH = 352;
constexpr int kMargin = 24;
constexpr int kCtrlW = 290; // left controls column width
constexpr int kPreviewX = 346;
constexpr int kPreviewY = 76;
constexpr int kPreviewSize = 170;
constexpr int kCtxY = 104; // the contextual band (swatch / paint chip / note)
constexpr int kCtxH = 28;
constexpr int kGradTypeW = 84; // Linear/Radial/Conic dropdown, left of the gradient paint chip
constexpr int kGradGap = 6;
constexpr int kDialSide = 34; // the gradient direction dial (right of the paint chip)

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// The accent-filled primary button is the shared ui::FilledButton (widgets.hpp) -- promoted from
// the identical file-local copies here / Texture Generator / Export / Layer Effects.

// The themed checkbox is the shared mosaic::ui::CheckBox (widgets.hpp) -- one settled design. Its
// erase-ground defaults to windowBg, which this dialog uses, so the CheckBox(...) call sites are
// the same.

// The colour-preview chip with the fill colour + its hex is the shared ui::SwatchChip
// (widgets.hpp) -- passive for the solid Contents, interactive ("Edit…" + chevron -> the colour
// flyout) for "Color…". Extracted so the Type/3D panels' colour lines read identically.

// The live preview pane is the shared ui::PreviewPane (preview_pane.hpp): it fills its whole rect
// with the transparency checkerboard and frames the (alpha-carrying) fill result over it,
// aspect-fit
// -- see recomputePreview() + app_window's fillCompositePreview() for the region choice.

void addLabel(int X, int Y, int W, const char* text, bool muted = false) {
    auto* b = new Fl_Box(X, Y, W, 18, text);
    b->box(FL_NO_BOX);
    b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    b->labelfont(muted ? FL_HELVETICA : FL_HELVETICA_BOLD);
    b->labelsize(muted ? 12 : 13);
    b->labelcolor(toFl(muted ? activePalette().textMuted : activePalette().text));
}

} // namespace

FillDialog::FillDialog(FillHost host)
    : Fl_Double_Window(kWinW, kWinH, _("Fill")), m_host(std::move(host)) {
    const Palette& pal = activePalette();
    color(toFl(pal.windowBg));
    begin();

    // Header line ("Filling: …").
    auto* header = new Fl_Box(kMargin, 16, kCtrlW, 18, "");
    header->box(FL_NO_BOX);
    header->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    header->labelfont(FL_HELVETICA);
    header->labelsize(12);
    header->labelcolor(toFl(pal.textMuted));
    m_header = header;

    // Contents.
    addLabel(kMargin, 46, kCtrlW, _("Contents"));
    m_contents = new Dropdown(kMargin, 66, kCtrlW, 28);
    m_contents->add(_("Foreground Color"));
    m_contents->add(_("Background Color"));
    m_contents->add(_("White"));
    m_contents->add(_("Black"));
    m_contents->add(_("50% Gray"));
    m_contents->add(_("Color\xE2\x80\xA6"), 0, nullptr, nullptr,
                    FL_MENU_DIVIDER); // solids ↓ paints
    m_contents->add(_("Gradient\xE2\x80\xA6"));
    m_contents->add(_("Pattern\xE2\x80\xA6"), 0, nullptr, nullptr,
                    FL_MENU_DIVIDER); // paints ↓ Inpaint
    m_contents->add(_("Inpaint"));
    m_contents->value(0);
    m_contents->callback(
        [](Fl_Widget*, void* v) { static_cast<FillDialog*>(v)->onContentsChanged(); }, this);

    // Contextual area (all share this band; onContentsChanged() shows exactly one): a swatch chip
    // for solid contents, a gradient-type dropdown + paint chip for Gradient/Pattern, a note for
    // inpaint / no-target.
    auto* swatch = new SwatchChip(kMargin, kCtxY, kCtrlW, kCtxH);
    swatch->setOnClick([this] { openColorFlyout(); });
    m_swatch = swatch;
    // Gradient TYPE (Linear/Radial/Conic) — the parent-owned type, shown only for "Gradient…" (the
    // flyout edits stops + spread, never the type). Sits left of the paint chip.
    auto* gradType = new Dropdown(kMargin, kCtxY, kGradTypeW, kCtxH);
    for (const char* n : kGradTypeNames)
        gradType->add(n);
    gradType->value(0);
    gradType->callback(
        [](Fl_Widget*, void* v) { static_cast<FillDialog*>(v)->onGradTypeChanged(); }, this);
    gradType->hide();
    m_gradType = gradType;
    // The paint chip: previews the working gradient/pattern + opens the matching flyout. Placed
    // full width here; onContentsChanged() narrows it when the gradient-type dropdown shares the
    // row.
    auto* chip = new PaintChip(kMargin, kCtxY, kCtrlW, kCtxH);
    chip->setGroundColor(pal.windowBg);
    chip->setOnClick([this] {
        if (contents() == Contents::Gradient)
            openGradientFlyout();
        else if (contents() == Contents::Pattern)
            openPatternFlyout();
    });
    chip->hide();
    m_paintChip = chip;
    // Gradient DIRECTION dial (right of the paint chip; shown only for "Gradient…"). Rotates the
    // gradient's transform about the content-box centre -- the Gradient tool will drive this from
    // canvas handles instead, but the modal edits it here. Vertically centred on the 28px band.
    auto* dir = new Dial(kMargin + kCtrlW - kDialSide, kCtxY + (kCtxH - kDialSide) / 2, kDialSide,
                         kDialSide);
    dir->range(0, 360);
    dir->step(1);
    dir->setCellColor(pal.windowBg);
    dir->when(FL_WHEN_CHANGED);
    dir->tooltip(_("Gradient direction"));
    dir->callback([](Fl_Widget*, void* v) { static_cast<FillDialog*>(v)->onGradDirChanged(); },
                  this);
    dir->hide();
    m_gradDir = dir;
    auto* note = new Fl_Box(kMargin, 102, kCtrlW, 44, "");
    note->box(FL_NO_BOX);
    note->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    note->labelfont(FL_HELVETICA);
    note->labelsize(12);
    note->labelcolor(toFl(pal.textMuted));
    note->hide();
    m_note = note;

    // Blending.
    addLabel(kMargin, 158, kCtrlW, _("Blending"));
    addLabel(kMargin, 186, 56, _("Mode"), /*muted=*/true);
    m_mode = new Dropdown(kMargin + 60, 182, kCtrlW - 60, 28);
    addBlendModeItems(
        *m_mode); // all modes, grouped by family with dividers (shared with the layers panel)
    m_mode->value(0);

    addLabel(kMargin, 224, 64, _("Opacity"), /*muted=*/true);
    // A plain "dumb" Slider (+ a % readout box) -- the same control the Layers panel uses for layer
    // opacity: precision isn't needed for a one-shot fill, so no scrub/type-in.
    m_opacity = new Slider(kMargin + 64, 224, kCtrlW - 64 - 50, 22);
    m_opacity->range(0, 100);
    m_opacity->step(1);
    m_opacity->value(100);
    m_opacity->setCellColor(pal.windowBg);
    m_opacity->callback(
        [](Fl_Widget*, void* v) {
            auto* d = static_cast<FillDialog*>(v);
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d %%", int(d->m_opacity->value()));
            d->m_opacityReadout->copy_label(buf);
            d->m_opacityReadout->redraw();
            d->requestPreview(); // coalesced: a continuous drag re-composites at most once per
                                 // frame
        },
        this);
    auto* ro = new Fl_Box(kMargin + kCtrlW - 44, 224, 44, 22, "100 %");
    ro->box(FL_NO_BOX);
    ro->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
    ro->labelfont(FL_HELVETICA);
    ro->labelsize(12);
    ro->labelcolor(toFl(pal.text));
    m_opacityReadout = ro;

    auto* protect = new CheckBox(kMargin, 258, kCtrlW, 22, _("Protect alpha"),
                                 [this](bool) { requestPreview(); });
    protect->tooltip(_("Only recolors pixels the layer already has (opacity above zero); fully "
                       "transparent pixels are left untouched. With a transparent selection this "
                       "leaves the preview unchanged."));
    m_protectAlpha = protect;

    // Preview pane (right column) + the Inpaint "Preview" button directly under it.
    addLabel(kPreviewX, 46, kPreviewSize, _("Preview"));
    m_preview = new PreviewPane(kPreviewX, kPreviewY, kPreviewSize, kPreviewSize);
    auto* prev =
        new FlatButton(kPreviewX, kPreviewY + kPreviewSize + 8, kPreviewSize, 26, _("Preview"));
    prev->callback([](Fl_Widget*, void* v) { static_cast<FillDialog*>(v)->doPreview(); }, this);
    prev->hide(); // shown only for Inpaint
    m_previewBtn = prev;

    // Footer buttons.
    const int by = kWinH - 44;
    auto* fill = new FilledButton(kWinW - kMargin - 96, by, 96, 30, _("Fill"));
    fill->callback([](Fl_Widget*, void* v) { static_cast<FillDialog*>(v)->doFill(); }, this);
    m_fill = fill;
    auto* cancel = new FlatButton(kWinW - kMargin - 96 - 12 - 88, by, 88, 30, _("Cancel"));
    cancel->callback([](Fl_Widget*, void* v) { static_cast<FillDialog*>(v)->doCancel(); }, this);
    m_cancel = cancel;

    // This (modal) dialog is its own top-level, so its Dropdowns need their own themed list +
    // right-click menu sub-windows -- built here, before show(), so FLTK realizes them as real
    // sub-surfaces of the dialog (a sub-window added to an already-shown parent is promoted to a
    // stray top-level). Without these the Contents/Mode combos fall back to Fl_Choice's stock Motif
    // pulldown and never show the family dividers. (Mirrors SettingsDialog.) The colour flyout is
    // the same kind of pre-built child sub-window.
    //
    // ORDER MATTERS: the flyout is created FIRST so the DropdownPopup is stacked ABOVE it. The
    // flyout hosts its own surface combo whose themed list is this dialog's DropdownPopup; if the
    // popup were below the flyout it would open behind it and be unclickable (the picker works for
    // the same reason -- its popup is created after the picker Popover).
    auto* flyout = new ColorFlyout();
    flyout->hide();
    flyout->setOnPick([this](common::Color8 c) {
        m_customColour = c;
        static_cast<SwatchChip*>(m_swatch)->setColour(c);
        requestPreview();
    });
    flyout->setUseForeground(
        [this] { return m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255}; });
    m_colorFlyout = flyout;
    // The gradient + pattern flyouts, siblings of the colour flyout, created BEFORE the
    // DropdownPopup so their internal combos' lists stack above them (the same ordering rule).
    // Their edits route to the working gradient/pattern, which the paint chip + preview reflect.
    auto* gflyout = new GradientFlyout();
    gflyout->hide();
    gflyout->setUseForeground(
        [this] { return m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255}; });
    gflyout->setOnChange([this](const core::vec::Gradient& g) {
        // The flyout edits stops + spread; the modal owns type (dropdown) + direction (dial), so
        // keep those even if they changed while the flyout was open.
        m_customGradient.stops = g.stops;
        m_customGradient.spread = g.spread;
        refreshPaintChip();
        requestPreview();
    });
    m_gradientFlyout = gflyout;
    auto* pflyout = new PatternFlyout();
    pflyout->hide();
    pflyout->setUseForeground(
        [this] { return m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255}; });
    pflyout->setOnChange([this](const core::vec::ProceduralPattern& pp) {
        m_customPattern = pp;
        refreshPaintChip();
        requestPreview();
    });
    m_patternFlyout = pflyout;
    (new DropdownPopup())->hide();
    (new ContextMenu())->hide();
    // The precision-ruler HUD for the pattern flyout's scrub sliders. It MUST be a child of this
    // modal (ScrubSlider::updateRuler bails unless slider + ruler share a top_window), created
    // before show() like the other sub-windows.
    m_ruler = new ScrubRuler();
    m_ruler->hide();
    pflyout->setRuler(m_ruler);

    end();
    size_range(kWinW, kWinH, kWinW,
               kWinH); // fixed-layout dialog (also keeps child sub-windows fixed)
    set_modal(); // after end() + size_range, matching SettingsDialog (no taskbar entry; app-modal)
}

FillDialog::~FillDialog() {
    Fl::remove_timeout(previewTimer, this);
}

void FillDialog::seed(const FillContext& ctx) {
    m_ctx = ctx;
    m_inpaintCacheValid = false; // the target is captured fresh; any prior preview is stale
    const common::Color8 fg =
        m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255};
    const common::Color8 bg =
        m_host.background ? m_host.background() : common::Color8{255, 255, 255, 255};
    m_customColour = fg;                                                         // "Color…" pick
    m_customGradient = defaultGradient(core::vec::GradientType::Linear, fg, bg); // FG→BG default
    m_customPattern = defaultProceduralPattern(fg);
    if (m_colorFlyout != nullptr)
        m_colorFlyout->hide();
    if (m_gradientFlyout != nullptr)
        m_gradientFlyout->hide();
    if (m_patternFlyout != nullptr)
        m_patternFlyout->hide();
    m_contents->value(0); // Foreground
    m_gradType->value(0); // Linear
    m_gradDir->value(0);  // 0° (the type default direction)
    m_mode->value(0);     // Normal
    m_opacity->value(100);
    m_opacityReadout->copy_label("100 %");
    static_cast<CheckBox*>(m_protectAlpha)->setChecked(false);

    if (m_header) {
        std::string h;
        if (!ctx.hasRasterTarget) {
            h = _("No raster layer is active.");
        } else {
            const char* what = ctx.selectionActive ? _("selection") : _("active layer");
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s: %s \xC2\xB7 %u\xC3\x97%u px", _("Filling"), what,
                          ctx.region.width, ctx.region.height);
            h = buf;
        }
        m_header->copy_label(h.c_str());
    }
    onContentsChanged();
}

FillDialog::Contents FillDialog::contents() const {
    switch (m_contents->value()) {
    case 0:
        return Contents::Foreground;
    case 1:
        return Contents::Background;
    case 2:
        return Contents::White;
    case 3:
        return Contents::Black;
    case 4:
        return Contents::Gray;
    case 5:
        return Contents::Custom;
    case 6:
        return Contents::Gradient;
    case 7:
        return Contents::Pattern;
    default:
        return Contents::Inpaint;
    }
}

common::Color8 FillDialog::currentColour() const {
    switch (contents()) {
    case Contents::Foreground:
        return m_host.foreground ? m_host.foreground() : common::Color8{};
    case Contents::Background:
        return m_host.background ? m_host.background() : common::Color8{255, 255, 255, 255};
    case Contents::White:
        return {255, 255, 255, 255};
    case Contents::Black:
        return {0, 0, 0, 255};
    case Contents::Gray:
        return {128, 128, 128, 255};
    case Contents::Custom:
        return m_customColour;
    case Contents::Gradient: // not a flat colour; the paint chip/preview use currentPaint()
    case Contents::Pattern:
        return m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255};
    case Contents::Inpaint:
        return {0, 0, 0, 255};
    }
    return {0, 0, 0, 255};
}

core::vec::Paint FillDialog::currentPaint() const {
    switch (contents()) {
    case Contents::Gradient:
        return m_customGradient;
    case Contents::Pattern:
        return core::vec::Pattern{m_customPattern};
    default:
        return core::vec::SolidPaint{common::toColorF(currentColour())};
    }
}

core::BlendMode FillDialog::mode() const {
    return static_cast<core::BlendMode>(std::clamp(m_mode->value(), 0, core::kBlendModeCount - 1));
}
float FillDialog::opacityF() const {
    return float(m_opacity->value()) / 100.0f;
}
bool FillDialog::protectAlpha() const {
    return static_cast<CheckBox*>(m_protectAlpha)->checked();
}

void FillDialog::onContentsChanged() {
    const Contents c = contents();
    const bool inpaint = c == Contents::Inpaint;
    const bool paint = isPaintContents();
    // Switching away from a paint content closes any flyout that no longer matches.
    if (c != Contents::Custom && m_colorFlyout != nullptr && m_colorFlyout->shown())
        m_colorFlyout->hide();
    if (c != Contents::Gradient && m_gradientFlyout != nullptr && m_gradientFlyout->shown())
        m_gradientFlyout->hide();
    if (c != Contents::Pattern && m_patternFlyout != nullptr && m_patternFlyout->shown())
        m_patternFlyout->hide();
    // Blend controls are meaningless for a reconstruction.
    if (inpaint) {
        m_mode->deactivate();
        m_opacity->deactivate();
        m_protectAlpha->deactivate();
        m_swatch->hide();
        m_paintChip->hide();
        m_gradType->hide();
        m_gradDir->hide();
        m_note->copy_label(_("Reconstructs the selection from the surrounding image. Click Preview "
                             "to see the result; Fill commits it."));
        m_note->show();
        if (m_ctx.inpaintAvailable && m_ctx.selectionActive)
            m_previewBtn->show();
        else
            m_previewBtn->hide();
    } else {
        m_mode->activate();
        m_opacity->activate();
        m_protectAlpha->activate();
        m_note->hide();
        m_previewBtn->hide();
        if (paint) {
            // Gradient/Pattern: the paint chip replaces the swatch. Gradient additionally shows a
            // type dropdown (left) + a direction dial (right), narrowing the chip between them.
            m_swatch->hide();
            if (c == Contents::Gradient) {
                m_gradType->value(std::clamp(static_cast<int>(m_customGradient.type), 0, 2));
                m_gradType->show();
                m_gradDir->value(gradientDirectionDeg(m_customGradient));
                m_gradDir->show();
                m_paintChip->resize(kMargin + kGradTypeW + kGradGap, kCtxY,
                                    kCtrlW - kGradTypeW - kGradGap - kGradGap - kDialSide, kCtxH);
            } else {
                m_gradType->hide();
                m_gradDir->hide();
                m_paintChip->resize(kMargin, kCtxY, kCtrlW, kCtxH);
            }
            refreshPaintChip();
            m_paintChip->show();
        } else {
            m_gradType->hide();
            m_gradDir->hide();
            m_paintChip->hide();
            auto* chip = static_cast<SwatchChip*>(m_swatch);
            chip->setColour(currentColour());
            const bool custom = c == Contents::Custom;
            chip->setInteractive(custom); // only "Color…" opens the picker on click
            chip->tooltip(custom ? _("Click to choose a color") : nullptr);
            m_swatch->show();
        }
    }

    // Fill availability.
    bool canFill = m_ctx.hasRasterTarget;
    if (inpaint)
        canFill = canFill && m_ctx.inpaintAvailable && m_ctx.selectionActive;
    else
        canFill = canFill && !m_ctx.region.empty();
    if (canFill)
        m_fill->activate();
    else
        m_fill->deactivate();

    recomputePreview();
}

void FillDialog::requestPreview() {
    if (m_previewPending)
        return; // already queued; this drag tick collapses into the pending one
    m_previewPending = true;
    Fl::add_timeout(1.0 / 60.0, previewTimer,
                    this); // run once next frame, then re-arm on the next tick
}

void FillDialog::previewTimer(void* self) {
    auto* d = static_cast<FillDialog*>(self);
    d->m_previewPending = false;
    d->recomputePreview();
}

void FillDialog::openColorFlyout() {
    if (m_colorFlyout == nullptr || contents() != Contents::Custom)
        return;
    if (m_colorFlyout->shownForAnchor(m_swatch)) { // re-click toggles it shut
        m_colorFlyout->hide();
        return;
    }
    if (m_gradientFlyout != nullptr) // one bubble on screen at a time
        m_gradientFlyout->hide();
    if (m_patternFlyout != nullptr)
        m_patternFlyout->hide();
    m_colorFlyout->openFor(m_swatch, m_customColour);
}

void FillDialog::openGradientFlyout() {
    if (m_gradientFlyout == nullptr || contents() != Contents::Gradient)
        return;
    if (m_gradientFlyout->shownForAnchor(m_paintChip)) { // re-click toggles it shut
        m_gradientFlyout->hide();
        return;
    }
    if (m_colorFlyout != nullptr)
        m_colorFlyout->hide();
    if (m_patternFlyout != nullptr)
        m_patternFlyout->hide();
    m_gradientFlyout->openFor(m_paintChip, m_customGradient);
}

void FillDialog::openPatternFlyout() {
    if (m_patternFlyout == nullptr || contents() != Contents::Pattern)
        return;
    if (m_patternFlyout->shownForAnchor(m_paintChip)) { // re-click toggles it shut
        m_patternFlyout->hide();
        return;
    }
    if (m_colorFlyout != nullptr)
        m_colorFlyout->hide();
    if (m_gradientFlyout != nullptr)
        m_gradientFlyout->hide();
    m_patternFlyout->setAntialias(m_host.antialias ? m_host.antialias() : true);
    m_patternFlyout->openFor(m_paintChip, m_customPattern);
}

void FillDialog::onGradTypeChanged() {
    if (m_gradType == nullptr)
        return;
    const auto t = static_cast<core::vec::GradientType>(std::clamp(m_gradType->value(), 0, 2));
    m_customGradient.type = t;
    // Re-place unit-space for the new type, keeping the dial's current direction. type + transform
    // are parent-owned, so an open editor need not be reseeded (its emit only contributes
    // stops/spread).
    const double deg = m_gradDir != nullptr ? m_gradDir->value() : 0.0;
    m_customGradient.transform = directedGradientTransform(t, deg);
    refreshPaintChip();
    requestPreview();
}

void FillDialog::onGradDirChanged() {
    if (m_gradDir == nullptr)
        return;
    m_customGradient.transform =
        directedGradientTransform(m_customGradient.type, m_gradDir->value());
    refreshPaintChip();
    requestPreview();
}

void FillDialog::refreshPaintChip() {
    if (m_paintChip == nullptr)
        return;
    if (contents() == Contents::Gradient)
        m_paintChip->setPaint(m_customGradient);
    else if (contents() == Contents::Pattern)
        m_paintChip->setPaint(core::vec::Pattern{m_customPattern});
}

void FillDialog::recomputePreview() {
    if (m_previewing)
        return; // the pane is showing live Preview progress; don't fight it (also fences the pump)
    auto* pane = static_cast<PreviewPane*>(m_preview);
    if (!m_ctx.hasRasterTarget || m_ctx.region.empty()) {
        pane->clearImage();
        pane->setNote(m_ctx.hasRasterTarget ? _("Nothing to fill.")
                                            : _("No raster layer is active."));
        return;
    }
    if (!m_host.compositePreview) {
        pane->clearImage();
        return;
    }
    common::Image regionPixels;
    if (isInpaint()) {
        // Show the cached inpaint result if one exists, else the area unchanged (in-document
        // context).
        regionPixels = m_inpaintCacheValid ? m_inpaintCache : m_ctx.region;
    } else if (isPaintContents()) {
        regionPixels = render::computeFillPaint(
            m_ctx.region, m_ctx.coverage, currentPaint(), m_ctx.originX, m_ctx.originY, mode(),
            opacityF(), protectAlpha(), m_host.antialias ? m_host.antialias() : true);
    } else {
        regionPixels = render::computeFill(m_ctx.region, m_ctx.coverage, currentColour(), mode(),
                                           opacityF(), protectAlpha());
    }
    // Composite the candidate pixels in document context (other layers + this layer's
    // blend/opacity), framed to the pane so it fills over the checker + off-canvas backdrop.
    pane->setContent(
        m_host.compositePreview(regionPixels, m_ctx.originX, m_ctx.originY, pane->w(), pane->h()));
}

void FillDialog::doPreview() {
    if (!isInpaint() || !m_host.runInpaintRegion || m_ctx.region.empty() || m_previewing)
        return;
    m_previewing = true; // fences off the buttons + recompute while the host pumps the event loop
    m_previewCancel = false;
    auto* pane = static_cast<PreviewPane*>(m_preview);
    pane->clearImage();
    pane->setNote(_("Reconstructing\xE2\x80\xA6 0%"));
    cursor(FL_CURSOR_WAIT); // a synchronous run on explicit click; the status bar shows progress
    // Each engine tick the host repaints; we mirror the percentage into the pane and report whether
    // Escape has asked to stop.
    const auto onProgress = [this, pane](float frac, const std::string& stage) -> bool {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s %d%%", stage.c_str(),
                      int(std::lround(std::clamp(frac, 0.0f, 1.0f) * 100.0f)));
        pane->clearImage();
        pane->setNote(buf);
        return !m_previewCancel;
    };
    auto result = m_host.runInpaintRegion(m_ctx.originX, m_ctx.originY, m_ctx.region.width,
                                          m_ctx.region.height, onProgress);
    cursor(FL_CURSOR_DEFAULT);
    m_previewing = false;
    if (result) {
        m_inpaintCache = std::move(*result);
        m_inpaintCacheValid = true;
    }
    recomputePreview(); // shows the cached result, or restores the contextual note
}

void FillDialog::doFill() {
    if (m_previewing) { // a Preview is running: treat the click as "stop previewing", don't fill
                        // yet
        m_previewCancel = true;
        return;
    }
    if (!m_ctx.hasRasterTarget) {
        hide();
        return;
    }
    if (isInpaint()) {
        if (m_inpaintCacheValid) { // a Preview already ran: commit the cached result, no re-inpaint
            common::Image cached = m_inpaintCache;
            hide();
            if (m_host.commitFill)
                m_host.commitFill(std::move(cached), m_ctx.originX, m_ctx.originY);
            return;
        }
        hide(); // no preview cached: hand off to the async path (previews on the un-dimmed canvas)
        if (m_host.runInpaintFill)
            m_host.runInpaintFill();
        return;
    }
    if (m_ctx.region.empty()) {
        hide();
        return;
    }
    common::Image filled =
        isPaintContents()
            ? render::computeFillPaint(m_ctx.region, m_ctx.coverage, currentPaint(), m_ctx.originX,
                                       m_ctx.originY, mode(), opacityF(), protectAlpha(),
                                       m_host.antialias ? m_host.antialias() : true)
            : render::computeFill(m_ctx.region, m_ctx.coverage, currentColour(), mode(), opacityF(),
                                  protectAlpha());
    hide();
    if (m_host.commitFill)
        m_host.commitFill(std::move(filled), m_ctx.originX, m_ctx.originY);
}

void FillDialog::doCancel() {
    if (m_previewing) { // don't tear the dialog down mid-run; cancel the Preview instead
        m_previewCancel = true;
        return;
    }
    hide();
}

int FillDialog::handle(int event) {
    if (m_previewing) {
        // A Preview is running (the host is pumping the loop). Esc requests cancellation; swallow
        // all other keystrokes so nothing re-enters mid-run.
        if (event == FL_KEYBOARD) {
            if (Fl::event_key() == FL_Escape)
                m_previewCancel = true;
            return 1;
        }
    }
    // A press anywhere outside an open themed list / context menu / colour flyout closes it (the
    // modeless pop-ups are children of this dialog; mirrors SettingsDialog::handle).
    if (event == FL_PUSH) {
        dismissActiveDropdownPopupOnOutsideClick(Fl::event_x(), Fl::event_y());
        dismissActiveContextMenuOnOutsideClick(Fl::event_x(), Fl::event_y());
        if (m_colorFlyout != nullptr && m_colorFlyout->shown() &&
            !m_colorFlyout->spansHostPoint(Fl::event_x(), Fl::event_y()))
            m_colorFlyout->hide();
        if (m_gradientFlyout != nullptr && m_gradientFlyout->shown() &&
            !m_gradientFlyout->spansHostPoint(Fl::event_x(), Fl::event_y()))
            m_gradientFlyout->hide();
        if (m_patternFlyout != nullptr && m_patternFlyout->shown() &&
            !m_patternFlyout->spansHostPoint(Fl::event_x(), Fl::event_y()))
            m_patternFlyout->hide();
    }
    if (event == FL_KEYBOARD) {
        const int key = Fl::event_key();
        if (key == FL_Escape) {
            // Escape closes an open list / menu / flyout first, only then cancels the dialog.
            if (activeContextMenu() != nullptr) {
                dismissActiveContextMenu();
                return 1;
            }
            if (activeDropdownPopup() != nullptr) {
                dismissActiveDropdownPopup();
                return 1;
            }
            if (m_colorFlyout != nullptr && m_colorFlyout->shown()) {
                m_colorFlyout->hide();
                return 1;
            }
            if (m_gradientFlyout != nullptr && m_gradientFlyout->shown()) {
                m_gradientFlyout->hide();
                return 1;
            }
            if (m_patternFlyout != nullptr && m_patternFlyout->shown()) {
                m_patternFlyout->hide();
                return 1;
            }
            doCancel();
            return 1;
        }
        if ((key == FL_Enter || key == FL_KP_Enter) && m_fill->active()) {
            doFill();
            return 1;
        }
    }
    return Fl_Double_Window::handle(event);
}

} // namespace mosaic::ui
