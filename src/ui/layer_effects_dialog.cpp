#include "ui/layer_effects_dialog.hpp"

#include "common/i18n.hpp"
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
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>
#include <variant>
#include <vector>

namespace mosaic::ui {

namespace {

constexpr int kWinW = 760;
constexpr int kWinH = 580;
constexpr int kFooterH = 56;
constexpr int kRailW = 216;
constexpr int kPreviewY = 12; // preview pinned to the top of the content area (no header above it)
constexpr int kPreviewH = 168;
constexpr int kRowH = 28;
constexpr int kRowGap = 6;
constexpr int kStepH = 22;              // catalogue -[n]+ stepper height
constexpr int kStepW = 2 * kStepH + 20; // width = two SQUARE (h x h) cells + a count column
constexpr int kStrokeMax = 8;           // sane cap on concentric strokes
constexpr int kLabelW = 68;             // control caption column
constexpr int kFieldW = 200; // control width (narrow, so the colour flyout opens into free space)

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

common::Color8 lighten(common::Color8 c, int d) {
    auto up = [d](std::uint8_t v) {
        return static_cast<std::uint8_t>(std::clamp(static_cast<int>(v) + d, 0, 255));
    };
    return {up(c.r), up(c.g), up(c.b), c.a};
}

// The accent-filled primary button is the shared ui::FilledButton (widgets.hpp). The local
// lighten() above stays: the hover-cell tint below still derives from it.

// The live preview pane is the shared ui::PreviewPane (preview_pane.hpp): it fills its whole rect
// with the transparency checkerboard and frames the (alpha-carrying) effect neighbourhood over it,
// aspect-fit -- see recomputePreview() + app_window's layerEffectsPreview() for the region choice.

// The net-new "− [n] +" stepper (a stackable effect's instance count). Left/right cells decrement/
// increment; the centre shows the count. Fires onDelta(±1), clamped by the host to [lo, hi].
class Stepper : public Fl_Widget {
public:
    Stepper(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}
    void setCount(int n) {
        m_count = n;
        redraw();
    }
    [[nodiscard]] int count() const { return m_count; }
    void setRange(int lo, int hi) {
        m_lo = lo;
        m_hi = hi;
    }
    void setOnDelta(std::function<void(int)> f) { m_onDelta = std::move(f); }
    void setGroundColor(common::Color8 c) { m_ground = c; }

private:
    [[nodiscard]] int cell() const { return h(); } // SQUARE -/+ buttons; the count fills the middle
    [[nodiscard]] bool canDec() const { return m_count > m_lo; }
    [[nodiscard]] bool canInc() const { return m_count < m_hi; }

protected:
    void draw() override {
        const Palette& p = activePalette();
        const bool on =
            active_r(); // a deactivated (unimplemented-effect) stepper reads muted + inert
        const int c = cell();
        fl_color(toFl(m_ground)); // erase the whole widget first (the centre gap would otherwise
        fl_rectf(x(), y(), w(), h()); // over-paint the count and bold it up, [[mosaic-ui-gotchas]])
        const auto btn = [&](int bx, bool cellOn, bool hover) {
            fl_color(toFl(on && cellOn && hover ? lighten(p.controlBg, 12) : p.controlBg));
            fl_rectf(bx, y(), c, h());
            fl_color(toFl(p.border));
            fl_rect(bx, y(), c, h());
        };
        btn(x(), canDec(), m_hover == 1);
        btn(x() + w() - c, canInc(), m_hover == 2);
        const int midY = y() + h() / 2;
        fl_color(toFl(on && canDec() ? p.text : p.textMuted));
        fl_line(x() + c / 2 - 4, midY, x() + c / 2 + 4, midY);
        fl_color(toFl(on && canInc() ? p.text : p.textMuted));
        const int px = x() + w() - c / 2;
        fl_line(px - 4, midY, px + 4, midY);
        fl_line(px, midY - 4, px, midY + 4);
        fl_color(toFl(on ? p.text : p.textMuted));
        fl_font(FL_HELVETICA, 12);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", m_count);
        fl_draw(buf, x() + c, y(), w() - 2 * c, h(), FL_ALIGN_CENTER);
    }
    int handle(int e) override {
        if (!active_r())
            return Fl_Widget::handle(e); // deactivated: no hover, no clicks
        const int c = cell();
        const int lx = Fl::event_x() - x();
        const int region = lx < c ? 1 : (lx >= w() - c ? 2 : 0);
        switch (e) {
        case FL_ENTER:
        case FL_MOVE:
            if (m_hover != region) {
                m_hover = region;
                redraw();
            }
            return 1;
        case FL_LEAVE:
            m_hover = 0;
            redraw();
            return 1;
        case FL_PUSH:
            return 1; // claim so we get the release
        case FL_RELEASE:
            if (region == 1 && canDec() && m_onDelta)
                m_onDelta(-1);
            if (region == 2 && canInc() && m_onDelta)
                m_onDelta(+1);
            return 1;
        default:
            return Fl_Widget::handle(e);
        }
    }

private:
    int m_count = 0, m_lo = 0, m_hi = kStrokeMax, m_hover = 0;
    common::Color8 m_ground = activePalette().panelBg;
    std::function<void(int)> m_onDelta;
};

// A collapsible instance-panel header (the type panel's DisclosureButton look): a disclosure
// triangle
// + a bold caption + a hairline. Clicking toggles the panel; the FlatButton base fires the
// callback.
class InstanceHeader : public FlatButton {
public:
    InstanceHeader(int X, int Y, int W, int H, const char* text) : FlatButton(X, Y, W, H, text) {}
    void setOpen(bool o) {
        m_open = o;
        redraw();
    }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        fl_color(toFl(pal.windowBg));
        fl_rectf(x(), y(), w(), h());
        const int cx = x() + 6, cyc = y() + h() / 2;
        fl_color(toFl(pal.text));
        fl_begin_polygon();
        if (m_open) {
            fl_vertex(cx - 4, cyc - 2);
            fl_vertex(cx + 4, cyc - 2);
            fl_vertex(cx, cyc + 3);
        } else {
            fl_vertex(cx - 2, cyc - 4);
            fl_vertex(cx - 2, cyc + 4);
            fl_vertex(cx + 3, cyc);
        }
        fl_end_polygon();
        fl_font(FL_HELVETICA_BOLD, 12);
        fl_draw(label(), x() + 18, y(), w() - 18, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        fl_color(toFl(pal.border));
        fl_line(x(), y() + h() - 1, x() + w() - 1, y() + h() - 1);
    }

private:
    bool m_open = true;
};

// The fixed effect catalogue (canonical order). LE-b implements Stroke; the rest are shown disabled
// until their render tier lands (overlays LE-c/-d, shadows/glows LE-e, bevel/satin LE-f).
enum class FxKind {
    Stroke,
    ColorOverlay,
    GradientOverlay,
    PatternOverlay,
    DropShadow,
    InnerShadow,
    OuterGlow,
    InnerGlow,
    Bevel,
    Satin
};
struct CatalogEntry {
    const char* name;
    FxKind kind;
    bool stackable;
    bool implemented;
};
constexpr std::array<CatalogEntry, 10> kCatalog{{
    {"Stroke", FxKind::Stroke, true, true},
    {"Color Overlay", FxKind::ColorOverlay, false, true},
    {"Gradient Overlay", FxKind::GradientOverlay, false, true},
    {"Pattern Overlay", FxKind::PatternOverlay, false, true},
    // ---- LE-e: the shadow/glow tier is now rendered (layer_effects_render.cpp), so un-grey it.
    {"Drop Shadow", FxKind::DropShadow, true, true},
    {"Inner Shadow", FxKind::InnerShadow, true, true},
    {"Outer Glow", FxKind::OuterGlow, false, true},
    {"Inner Glow", FxKind::InnerGlow, false, true},
    // ---- LE-f: the shading tier renders now (bevel/satin). ----
    {"Bevel & Emboss", FxKind::Bevel, false, true},
    {"Satin", FxKind::Satin, false, true},
}};

const char* kAlignNames[3] = {"Inside", "Center", "Outside"};
// ---- LE-f: Bevel style dropdown labels (order matches core::BevelEffect::Style). ----
const char* kBevelStyleNames[4] = {"Outer Bevel", "Inner Bevel", "Emboss", "Pillow Emboss"};

// A control's binding: the closure to run when it fires (FLTK callbacks are C thunks + a void*).
struct Binding {
    std::function<void()> fn;
};
void controlThunk(Fl_Widget*, void* b) {
    if (auto* bb = static_cast<Binding*>(b))
        bb->fn();
}

} // namespace

struct LayerEffectsDialog::Ui {
    PreviewPane* preview = nullptr;
    Fl_Group* fillPanel = nullptr; // Fill-opacity, floating over the preview corner
    std::array<CheckBox*, kCatalog.size()> catToggles{};
    std::array<Stepper*, kCatalog.size()> catSteppers{};
    ScrollView* scroll = nullptr;
    Fl_Group* stack = nullptr; // scroll content (rebuilt on count change)
    ScrubSlider* fillOpacity = nullptr;
    std::unique_ptr<Binding> fillBinding;   // persistent (the fill slider lives outside the stack)
    std::vector<bool> collapsed;            // per-stroke collapse state (persists across rebuilds)
    SwatchChip* colorOverlayChip = nullptr; // colour overlay's solid chip (when the panel shows)
    PaintChip* gradOverlayChip = nullptr;   // gradient overlay's gradient chip
    PaintChip* patOverlayChip = nullptr;    // pattern overlay's pattern chip
    bool colorOverlayCollapsed = false;
    bool gradOverlayCollapsed = false;
    bool patOverlayCollapsed = false;
    // ---- LE-e: per-instance collapse state for the stackable shadow panels + the single glows.
    std::vector<bool> dropCollapsed;  // per drop-shadow instance (persists across rebuilds)
    std::vector<bool> innerCollapsed; // per inner-shadow instance
    bool outerGlowCollapsed = false;
    bool innerGlowCollapsed = false;
    bool bevelCollapsed = false; // LE-f: Bevel & Emboss panel collapse state
    bool satinCollapsed = false; // LE-f: Satin panel collapse state
    std::vector<std::unique_ptr<Binding>> stackBindings; // cleared each rebuildStack
    ColorFlyout* colorFlyout = nullptr;
    GradientFlyout* gradientFlyout = nullptr;
    PatternFlyout* patternFlyout = nullptr;
};

LayerEffectsDialog::LayerEffectsDialog(LayerEffectsHost host)
    : Fl_Double_Window(kWinW, kWinH, _("Layer Effects")), m_host(std::move(host)),
      m_ui(std::make_unique<Ui>()) {
    color(toFl(activePalette().windowBg));
    begin(); // keep `this` the current group through build() + the sub-window creation below
    build();

    // Modal dialog: its own DropdownPopup / ContextMenu / ColorFlyout child sub-windows, created
    // here before show() (a sub-window added to an already-shown parent is promoted to a stray
    // top-level). Order: the flyout FIRST so the DropdownPopup stacks above it (the Fill/Settings
    // convention).
    auto* flyout = new ColorFlyout();
    flyout->hide();
    flyout->setUseForeground(
        [this] { return m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255}; });
    flyout->setOnPick([this](common::Color8 c) {
        if (m_onColorPick)
            m_onColorPick(c);
    });
    m_ui->colorFlyout = flyout;
    // The gradient flyout (its sibling), created BEFORE the DropdownPopup so its spread combo's
    // list stacks above it. Edits route to the active m_onGradientChange sink (the paint control
    // opens it).
    auto* gflyout = new GradientFlyout();
    gflyout->hide();
    gflyout->setUseForeground(
        [this] { return m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255}; });
    gflyout->setOnChange([this](const core::vec::Gradient& g) {
        if (m_onGradientChange)
            m_onGradientChange(g);
    });
    m_ui->gradientFlyout = gflyout;
    // The pattern flyout (its sibling), also created before the DropdownPopup. Edits route to the
    // active m_onPatternChange sink (the paint control opens it).
    auto* pflyout = new PatternFlyout();
    pflyout->hide();
    pflyout->setUseForeground(
        [this] { return m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255}; });
    pflyout->setOnChange([this](const core::vec::ProceduralPattern& pp) {
        if (m_onPatternChange)
            m_onPatternChange(pp);
    });
    m_ui->patternFlyout = pflyout;
    (new DropdownPopup())->hide();
    (new ContextMenu())->hide();
    // The ScrubSlider precision-ruler HUD. It MUST be a child of this modal (its own top-level):
    // ScrubSlider::updateRuler bails unless slider.top_window() == ruler.top_window(), so the main
    // window's shared ruler would never show here (the recurring "ruler doesn't appear in a new
    // window" bug). Created before show() like the other sub-windows; handed to each slider.
    m_ruler = new ScrubRuler();
    m_ruler->hide();
    pflyout->setRuler(m_ruler); // the pattern flyout's Scale/Weight scrub sliders share this HUD
    if (m_ui->fillOpacity)
        m_ui->fillOpacity->setRuler(m_ruler); // and the Fill-opacity slider

    end();
    size_range(kWinW, kWinH, kWinW, kWinH);
    set_modal();
    callback([](Fl_Widget*, void* v) { static_cast<LayerEffectsDialog*>(v)->doCancel(); }, this);
}

LayerEffectsDialog::~LayerEffectsDialog() {
    Fl::remove_timeout(previewTimer, this);
}

void LayerEffectsDialog::build() {
    const Palette& pal = activePalette();
    // NB: no begin()/end() here -- the ctor keeps `this` the current group so the sub-windows it
    // creates after build() are children of the dialog (not leaked stray top-levels).

    // ---- Left catalogue rail ----
    auto* rail = new Panel(0, 0, kRailW, kWinH - kFooterH);
    rail->borderEdges(Panel::EdgeRight);
    rail->begin();
    auto* railTitle = new Fl_Box(16, 12, kRailW - 24, 20, _("Effects"));
    railTitle->box(FL_NO_BOX);
    railTitle->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    railTitle->labelfont(FL_HELVETICA_BOLD);
    railTitle->labelsize(13);
    railTitle->labelcolor(toFl(pal.text));

    int ry = 44;
    for (std::size_t i = 0; i < kCatalog.size(); ++i) {
        const CatalogEntry& e = kCatalog[i];
        const int stepW = e.stackable ? kStepW : 0;
        auto* cb = new CheckBox(14, ry, kRailW - 28 - stepW - (stepW ? 6 : 0), kRowH, e.name);
        cb->setGroundColor(pal.panelBg);
        m_ui->catToggles[i] = cb;
        if (e.stackable) {
            auto* st = new Stepper(kRailW - 14 - stepW, ry + (kRowH - kStepH) / 2, stepW, kStepH);
            st->setRange(0, kStrokeMax);
            st->setGroundColor(pal.panelBg);
            m_ui->catSteppers[i] = st;
        }
        if (!e.implemented) {
            cb->deactivate(); // greyed until this effect's render tier lands
            cb->copy_tooltip(_("Coming in a later update"));
            if (m_ui->catSteppers[i])
                m_ui->catSteppers[i]->deactivate();
        }
        ry += kRowH + kRowGap;
    }
    // Stroke (stackable): wire its checkbox + stepper.
    if (m_ui->catToggles[0]) {
        m_ui->catToggles[0]->setOnToggle([this](bool on) {
            if (m_seeding)
                return;
            if (on && m_working.strokes.empty())
                m_working.strokes.emplace_back();
            else if (!on)
                m_working.strokes.clear();
            syncCatalog();
            rebuildStack();
            applyLive();
        });
    }
    if (m_ui->catSteppers[0]) {
        m_ui->catSteppers[0]->setOnDelta([this](int d) {
            if (m_seeding)
                return;
            int n = std::clamp(static_cast<int>(m_working.strokes.size()) + d, 0, kStrokeMax);
            m_working.strokes.resize(static_cast<std::size_t>(n));
            syncCatalog();
            rebuildStack();
            applyLive();
        });
    }
    // Colour + Gradient overlays (LE-c): enabling seeds a default paint so the effect draws at
    // once.
    if (m_ui->catToggles[1]) {
        m_ui->catToggles[1]->setOnToggle([this](bool on) {
            if (m_seeding)
                return;
            m_working.colorOverlay.enabled = on;
            if (on && std::holds_alternative<core::vec::NoPaint>(m_working.colorOverlay.paint)) {
                const common::Color8 fg =
                    m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255};
                m_working.colorOverlay.paint = core::vec::SolidPaint{common::toColorF(fg)};
            }
            rebuildStack();
            applyLive();
        });
    }
    if (m_ui->catToggles[2]) {
        m_ui->catToggles[2]->setOnToggle([this](bool on) {
            if (m_seeding)
                return;
            m_working.gradientOverlay.enabled = on;
            if (on &&
                !std::holds_alternative<core::vec::Gradient>(m_working.gradientOverlay.paint)) {
                const common::Color8 fg =
                    m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255};
                common::Color8 fade = fg;
                fade.a = 0;
                m_working.gradientOverlay.paint =
                    defaultGradient(core::vec::GradientType::Linear, fg, fade);
            }
            rebuildStack();
            applyLive();
        });
    }
    if (m_ui->catToggles[3]) {
        m_ui->catToggles[3]->setOnToggle([this](bool on) {
            if (m_seeding)
                return;
            m_working.patternOverlay.enabled = on;
            if (on && !std::holds_alternative<core::vec::Pattern>(m_working.patternOverlay.paint)) {
                const common::Color8 fg =
                    m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255};
                m_working.patternOverlay.paint = core::vec::Pattern{defaultProceduralPattern(fg)};
            }
            rebuildStack();
            applyLive();
        });
    }
    // ---- LE-e: shadows & glows. Drop/Inner Shadow are STACKABLE (mirror Stroke's checkbox+stepper
    // that add/remove vector entries); Outer/Inner Glow are SINGLE (mirror the overlay checkbox that
    // flips .enabled + seeds a paint). NB ShadowEffect defaults enabled=FALSE (unlike StrokeEffect),
    // so a freshly added instance must be flipped ON or it renders nothing (empty() stays true).
    const auto wireStackableShadow = [this](std::size_t idx, std::vector<core::ShadowEffect>& vec) {
        if (m_ui->catToggles[idx]) {
            m_ui->catToggles[idx]->setOnToggle([this, &vec](bool on) {
                if (m_seeding)
                    return;
                if (on && vec.empty())
                    vec.emplace_back().enabled = true;
                else if (!on)
                    vec.clear();
                syncCatalog();
                rebuildStack();
                applyLive();
            });
        }
        if (m_ui->catSteppers[idx]) {
            m_ui->catSteppers[idx]->setOnDelta([this, &vec](int d) {
                if (m_seeding)
                    return;
                const int old = static_cast<int>(vec.size());
                const int n = std::clamp(old + d, 0, kStrokeMax);
                vec.resize(static_cast<std::size_t>(n));
                for (int k = old; k < n; ++k)
                    vec[static_cast<std::size_t>(k)].enabled = true; // new instances draw at once
                syncCatalog();
                rebuildStack();
                applyLive();
            });
        }
    };
    wireStackableShadow(4, m_working.dropShadows);
    wireStackableShadow(5, m_working.innerShadows);
    // Outer + Inner Glow (single): enabling seeds a light default paint so a Screen-blended glow shows
    // at once (a black/foreground glow on Screen would be an invisible no-op).
    const auto wireGlow = [this](std::size_t idx, core::GlowEffect& glow) {
        if (!m_ui->catToggles[idx])
            return;
        m_ui->catToggles[idx]->setOnToggle([this, &glow](bool on) {
            if (m_seeding)
                return;
            glow.enabled = on;
            if (on && std::holds_alternative<core::vec::NoPaint>(glow.paint))
                glow.paint = core::vec::SolidPaint{common::toColorF({255, 255, 255, 255})};
            rebuildStack();
            applyLive();
        });
    };
    wireGlow(6, m_working.outerGlow);
    wireGlow(7, m_working.innerGlow);
    // ---- LE-f: Bevel & Emboss + Satin toggles (single effects; their model defaults already draw,
    // so enabling just flips `.enabled` -- the Colour/Gradient/Pattern-overlay checkbox pattern). ----
    if (m_ui->catToggles[8]) {
        m_ui->catToggles[8]->setOnToggle([this](bool on) {
            if (m_seeding)
                return;
            m_working.bevel.enabled = on;
            rebuildStack();
            applyLive();
        });
    }
    if (m_ui->catToggles[9]) {
        m_ui->catToggles[9]->setOnToggle([this](bool on) {
            if (m_seeding)
                return;
            m_working.satin.enabled = on;
            rebuildStack();
            applyLive();
        });
    }
    rail->end();

    // ---- Preview pane (pinned to the top of the content area; no header label above it) ----
    const int cx = kRailW + 16;
    const int cw = kWinW - cx - 16;
    m_ui->preview = new PreviewPane(cx, kPreviewY, cw, kPreviewH);

    // Fill-opacity lives in its own little panel floating over the preview's bottom-right corner (a
    // per-layer global, so it sits here rather than in the effect stack). Created AFTER the preview
    // so it draws on top; recomputePreview() co-redraws it so a preview refresh can't overpaint it.
    {
        constexpr int kFillPanelW = 156, kFillPanelH = 38, kFillMargin = 8, kFillCapW = 26;
        const int fpx = cx + cw - kFillPanelW - kFillMargin;
        const int fpy = kPreviewY + kPreviewH - kFillPanelH - kFillMargin;
        auto* fp = new Panel(fpx, fpy, kFillPanelW, kFillPanelH);
        fp->begin();
        const int inY = fpy + (kFillPanelH - kRowH) / 2;
        auto* fcap = new Fl_Box(fpx + 8, inY, kFillCapW, kRowH, _("Fill"));
        fcap->box(FL_NO_BOX);
        fcap->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        fcap->labelfont(FL_HELVETICA);
        fcap->labelsize(12);
        fcap->labelcolor(toFl(pal.textMuted));
        const int fsx = fpx + 8 + kFillCapW + 4;
        auto* fs = new ScrubSlider(fsx, inY, fpx + kFillPanelW - 8 - fsx, kRowH);
        fs->range(0, 100);
        fs->step(1);
        fs->setSuffix("%");
        fs->setCellColor(pal.panelBg);
        fs->value(m_working.fillOpacity * 100.0);
        fs->when(FL_WHEN_CHANGED);
        fs->copy_tooltip(_("Dims the layer's own pixels while its effects stay at full strength"));
        m_ui->fillBinding = std::make_unique<Binding>();
        m_ui->fillBinding->fn = [this, fs] {
            if (m_seeding)
                return;
            m_working.fillOpacity = static_cast<float>(fs->value() / 100.0);
            applyLive();
        };
        fs->callback(controlThunk, m_ui->fillBinding.get());
        m_ui->fillOpacity = fs;
        fp->end();
        m_ui->fillPanel = fp;
    }

    // ---- Instance stack (scroll) ---- (a slim gap below the preview)
    const int sy = kPreviewY + kPreviewH + 8;
    auto* sv = new ScrollView(kRailW, sy, kWinW - kRailW, (kWinH - kFooterH) - sy);
    sv->type(Fl_Scroll::VERTICAL_ALWAYS);
    sv->box(FL_FLAT_BOX);
    sv->color(toFl(pal.windowBg));
    sv->begin();
    auto* stack = new Fl_Group(kRailW, sy, kWinW - kRailW - 16, 200);
    stack->resizable(nullptr);
    stack->box(FL_NO_BOX);
    stack->end();
    sv->end();
    m_ui->scroll = sv;
    m_ui->stack = stack;

    // ---- Footer ---- buttons directly on the modal's windowBg (not a panelBg Panel) so the colour
    // flyout's fake-transparent corners blend here too; a top hairline separates it.
    auto* rule = new Fl_Box(0, kWinH - kFooterH, kWinW, 1);
    rule->box(FL_FLAT_BOX);
    rule->color(toFl(pal.border));
    const int by = kWinH - kFooterH + (kFooterH - 30) / 2;
    auto* ok = new FilledButton(kWinW - 16 - 96, by, 96, 30, _("OK"));
    ok->callback([](Fl_Widget*, void* v) { static_cast<LayerEffectsDialog*>(v)->doOk(); }, this);
    auto* cancel = new FlatButton(kWinW - 16 - 96 - 12 - 88, by, 88, 30, _("Cancel"));
    cancel->callback([](Fl_Widget*, void* v) { static_cast<LayerEffectsDialog*>(v)->doCancel(); },
                     this);
}

void LayerEffectsDialog::rebuildStack() {
    const Palette& pal = activePalette();
    Fl_Group* stack = m_ui->stack;
    stack->clear();
    // Fl_Group::clear() resets resizable_ to `this`, so a later size() would PROPORTIONALLY scale
    // every child (the "controls get thicker / heights jump around" bug). Pin it null again.
    stack->resizable(nullptr);
    m_ui->stackBindings.clear();
    m_ui->colorOverlayChip = nullptr; // recreated below only when the panel is open
    m_ui->gradOverlayChip = nullptr;
    m_ui->patOverlayChip = nullptr;
    m_ui->collapsed.resize(m_working.strokes.size(), false);
    // ---- LE-e: keep the shadow instances' collapse state sized to their vectors.
    m_ui->dropCollapsed.resize(m_working.dropShadows.size(), false);
    m_ui->innerCollapsed.resize(m_working.innerShadows.size(), false);

    const int cw = stack->w();
    const int left = 12;
    const int labelW = kLabelW;
    const int fieldW = kFieldW;
    // Right-align the controls to the stack's right edge (label left, control right -- the
    // two-column form look). The colour flyout auto-flips to open LEFT of its (right-aligned) chip,
    // into the gap.
    const int fieldLeft = stack->x() + cw - 12 - fieldW;
    stack->begin();
    int cy = stack->y() + 10;

    const auto bind = [&](std::function<void()> fn) {
        auto b = std::make_unique<Binding>();
        b->fn = std::move(fn);
        Binding* raw = b.get();
        m_ui->stackBindings.push_back(std::move(b));
        return raw;
    };
    const auto caption = [&](int rowY, const char* text) {
        auto* c = new Fl_Box(stack->x() + left, rowY, labelW, kRowH, nullptr);
        c->copy_label(text);
        c->box(FL_NO_BOX);
        c->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        c->labelfont(FL_HELVETICA);
        c->labelsize(12);
        c->labelcolor(toFl(pal.textMuted));
    };
    const auto slider = [&](int rowY, double mn, double mx, double step, const char* suffix,
                            double val, std::function<void(double)> onChange) {
        auto* s = new ScrubSlider(fieldLeft, rowY, fieldW, kRowH);
        s->range(mn, mx);
        s->step(step);
        s->setSuffix(suffix);
        s->setCellColor(pal.windowBg);
        s->setRuler(m_ruler); // this modal's own ruler (shares the sliders' top_window)
        s->value(val);
        s->when(FL_WHEN_CHANGED);
        s->callback(controlThunk, bind([this, s, cb = std::move(onChange)] {
                        if (m_seeding)
                            return;
                        cb(s->value());
                        applyLive();
                    }));
        return s;
    };
    // A collapsible instance-panel header; the caller advances cy and emits its rows when `open`.
    const auto panelHeader = [&](int rowY, const char* title, bool open,
                                 std::function<void()> onToggle) {
        auto* head = new InstanceHeader(stack->x() + 2, rowY, cw - 4, kRowH, nullptr);
        head->copy_label(title);
        head->setOpen(open);
        head->callback(controlThunk, bind(std::move(onToggle)));
    };
    // A "Blend" row (caption + all-modes dropdown) shared by strokes + overlays.
    const auto blendRow = [&](int rowY, core::BlendMode cur,
                              std::function<void(core::BlendMode)> setter) {
        caption(rowY, _("Blend"));
        auto* d = new Dropdown(fieldLeft, rowY, fieldW, kRowH);
        addBlendModeItems(*d);
        d->value(std::clamp(static_cast<int>(cur), 0, core::kBlendModeCount - 1));
        d->callback(controlThunk, bind([this, d, setter = std::move(setter)] {
                        if (m_seeding)
                            return;
                        setter(static_cast<core::BlendMode>(
                            std::clamp(d->value(), 0, core::kBlendModeCount - 1)));
                        applyLive();
                    }));
    };

    // ---- LE-e helpers: an angle DIAL row (mirrors the Gradient-Overlay Direction dial), a SwatchChip
    // COLOUR row, and the shadow/glow panel builders. Angle edits the raw degrees the render lane pins
    // (default 120 -> lower-right); the dial-needle-to-light mapping is a visual-pass cosmetic.
    const auto angleRow = [&](int rowY, double curDeg, std::function<void(double)> onChange) {
        caption(rowY, _("Angle"));
        auto* dial = new Dial(fieldLeft, rowY, kRowH, kRowH);
        dial->range(0, 360);
        dial->step(1);
        dial->setCellColor(pal.windowBg);
        dial->when(FL_WHEN_CHANGED);
        dial->value(curDeg);
        auto* val = new Fl_Box(fieldLeft + kRowH + 8, rowY, fieldW - kRowH - 8, kRowH, nullptr);
        val->box(FL_NO_BOX);
        val->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        val->labelfont(FL_HELVETICA);
        val->labelsize(12);
        val->labelcolor(toFl(pal.text));
        const auto writeVal = [](Fl_Box* b, double deg) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d\xC2\xB0", static_cast<int>(std::lround(deg)));
            b->copy_label(buf);
        };
        writeVal(val, dial->value());
        dial->callback(controlThunk, bind([this, dial, val, writeVal, onChange = std::move(onChange)] {
                           if (m_seeding)
                               return;
                           onChange(dial->value());
                           writeVal(val, dial->value());
                           val->redraw();
                           applyLive();
                       }));
    };
    // A SwatchChip colour row (caption + interactive chip -> ColorFlyout). `get`/`set` read/write the
    // owning effect's ColorF (captured by a stable pointer, so the callbacks survive to the next rebuild).
    const auto colorRow = [&](int rowY, const char* label, common::Color8 cur,
                              std::function<common::Color8()> get, std::function<void(common::Color8)> set) {
        caption(rowY, label);
        auto* chip = new SwatchChip(fieldLeft, rowY, fieldW, kRowH);
        chip->setGroundColor(pal.windowBg);
        chip->setInteractive(true);
        chip->setColour(cur);
        chip->setOnClick([this, chip, get = std::move(get), set = std::move(set)] {
            openColorFlyout(chip, get(), [this, chip, set](common::Color8 c) {
                set(c);
                chip->setColour(c);
                applyLive();
            });
        });
    };
    // A drop/inner shadow instance panel (Blend, Color, Opacity, Angle, Distance, Spread, Size). `vec`
    // is a stable pointer to the owning m_working vector (dropShadows / innerShadows), indexed fresh in
    // every setter so a vector realloc between rebuilds cannot dangle (bindings are cleared each rebuild).
    const auto emitShadowPanel = [&](std::vector<core::ShadowEffect>* vec, std::vector<bool>* collapsed,
                                     std::size_t i, const char* prefix) {
        const bool open = i < collapsed->size() ? !(*collapsed)[i] : true;
        char title[32];
        std::snprintf(title, sizeof(title), "%s %zu", prefix, i + 1);
        auto* head = new InstanceHeader(stack->x() + 2, cy, cw - 4, kRowH, nullptr);
        head->copy_label(title);
        head->setOpen(open);
        head->callback(controlThunk, bind([this, collapsed, i] {
                           if (i < collapsed->size())
                               (*collapsed)[i] = !(*collapsed)[i];
                           rebuildStack();
                       }));
        cy += kRowH + 4;
        if (!open) {
            cy += kRowGap;
            return;
        }
        core::ShadowEffect& sh = (*vec)[i];
        blendRow(cy, sh.blend, [vec, i](core::BlendMode b) { (*vec)[i].blend = b; });
        cy += kRowH + kRowGap;
        colorRow(cy, _("Color"), common::toColor8(sh.color),
                 [vec, i] { return common::toColor8((*vec)[i].color); },
                 [vec, i](common::Color8 c) { (*vec)[i].color = common::toColorF(c); });
        cy += kRowH + kRowGap;
        caption(cy, _("Opacity"));
        slider(cy, 0, 100, 1, "%", sh.opacity * 100.0,
               [vec, i](double v) { (*vec)[i].opacity = float(v / 100.0); });
        cy += kRowH + kRowGap;
        angleRow(cy, sh.angleDeg, [vec, i](double d) { (*vec)[i].angleDeg = float(d); });
        cy += kRowH + kRowGap;
        caption(cy, _("Distance"));
        slider(cy, 0, 100, 0.5, "px", sh.distance, [vec, i](double v) { (*vec)[i].distance = float(v); });
        cy += kRowH + kRowGap;
        caption(cy, _("Spread"));
        slider(cy, 0, 100, 0.5, "px", sh.spread, [vec, i](double v) { (*vec)[i].spread = float(v); });
        cy += kRowH + kRowGap;
        caption(cy, _("Size"));
        slider(cy, 0, 250, 0.5, "px", sh.size, [vec, i](double v) { (*vec)[i].size = float(v); });
        cy += kRowH + kRowGap;
    };
    // The single-glow rows (Blend, Colour, Opacity, Choke, Size [+ Inner Glow: Source]). `glow` is a
    // stable pointer to m_working.outerGlow / innerGlow. The colour edits the paint as a SolidPaint.
    const auto emitGlowRows = [&](core::GlowEffect* glow, bool inner) {
        blendRow(cy, glow->blend, [glow](core::BlendMode b) { glow->blend = b; });
        cy += kRowH + kRowGap;
        const auto solidColor = [glow] {
            const auto* s = std::get_if<core::vec::SolidPaint>(&glow->paint);
            return common::toColor8(s ? s->color : common::ColorF{1, 1, 1, 1});
        };
        colorRow(cy, _("Color"), solidColor(), solidColor, [glow](common::Color8 c) {
            glow->paint = core::vec::SolidPaint{common::toColorF(c)};
        });
        cy += kRowH + kRowGap;
        caption(cy, _("Opacity"));
        slider(cy, 0, 100, 1, "%", glow->opacity * 100.0,
               [glow](double v) { glow->opacity = float(v / 100.0); });
        cy += kRowH + kRowGap;
        caption(cy, _("Choke"));
        slider(cy, 0, 100, 0.5, "px", glow->choke, [glow](double v) { glow->choke = float(v); });
        cy += kRowH + kRowGap;
        caption(cy, _("Size"));
        slider(cy, 0, 250, 0.5, "px", glow->size, [glow](double v) { glow->size = float(v); });
        cy += kRowH + kRowGap;
        if (inner) {
            caption(cy, _("Source"));
            auto* d = new Dropdown(fieldLeft, cy, fieldW, kRowH);
            d->add(_("Edge"));
            d->add(_("Center"));
            d->value(static_cast<int>(glow->source));
            d->callback(controlThunk, bind([this, glow, d] {
                            if (m_seeding)
                                return;
                            glow->source =
                                static_cast<core::GlowEffect::Source>(std::clamp(d->value(), 0, 1));
                            applyLive();
                        }));
            cy += kRowH + kRowGap;
        }
    };

    // (Fill-opacity is no longer here -- it lives in its own panel over the preview, built once.)

    // One panel per stroke instance.
    for (std::size_t i = 0; i < m_working.strokes.size(); ++i) {
        const bool open = !m_ui->collapsed[i];
        char title[24];
        std::snprintf(title, sizeof(title), "%s %zu", _("Stroke"), i + 1);
        auto* head = new InstanceHeader(stack->x() + 2, cy, cw - 4, kRowH, nullptr);
        head->copy_label(title);
        head->setOpen(open);
        head->callback(controlThunk, bind([this, i] {
                           if (i < m_ui->collapsed.size())
                               m_ui->collapsed[i] = !m_ui->collapsed[i];
                           rebuildStack();
                       }));
        cy += kRowH + 4; // header hairline + a little breathing room below it
        if (!open) {
            cy += kRowGap;
            continue;
        }

        core::StrokeEffect& st = m_working.strokes[i];

        caption(cy, _("Width"));
        slider(cy, 0, 100, 0.5, "px", st.width,
               [this, i](double v) { m_working.strokes[i].width = float(v); });
        cy += kRowH + kRowGap;

        caption(cy, _("Position"));
        {
            auto* d = new Dropdown(fieldLeft, cy, fieldW, kRowH);
            for (const char* n : kAlignNames)
                d->add(n);
            d->value(static_cast<int>(st.align));
            d->callback(controlThunk, bind([this, i, d] {
                            if (m_seeding)
                                return;
                            m_working.strokes[i].align = static_cast<core::StrokeEffect::Align>(
                                std::clamp(d->value(), 0, 2));
                            applyLive();
                        }));
        }
        cy += kRowH + kRowGap;

        // Paint control: a kind (Solid/Linear/Radial/Conic) dropdown + a chip that opens the colour
        // or gradient flyout. The kind is the paint's TYPE (parent-owned); switching it converts
        // the paint.
        caption(cy, _("Paint"));
        {
            const int kindW = 84, gap = 6;
            auto* kind = new Dropdown(fieldLeft, cy, kindW, kRowH);
            for (const char* n : kPaintKindNames)
                kind->add(n);
            kind->value(paintKindIndex(st.paint));
            auto* chip = new PaintChip(fieldLeft + kindW + gap, cy, fieldW - kindW - gap, kRowH);
            chip->setGroundColor(pal.windowBg);
            chip->setPaint(st.paint);
            kind->callback(controlThunk, bind([this, i, kind, chip] {
                               if (m_seeding)
                                   return;
                               const common::Color8 fg = m_host.foreground
                                                             ? m_host.foreground()
                                                             : common::Color8{0, 0, 0, 255};
                               setPaintKind(m_working.strokes[i].paint, kind->value(),
                                            paintSeedColor(m_working.strokes[i].paint, fg));
                               chip->setPaint(m_working.strokes[i].paint);
                               applyLive();
                           }));
            chip->setOnClick([this, i, chip] {
                core::vec::Paint& pt = m_working.strokes[i].paint;
                if (auto* g = std::get_if<core::vec::Gradient>(&pt)) {
                    openGradientFlyout(chip, *g, [this, i, chip](const core::vec::Gradient& ng) {
                        m_working.strokes[i].paint = ng;
                        chip->setPaint(m_working.strokes[i].paint);
                        applyLive();
                    });
                } else if (auto* pat = std::get_if<core::vec::Pattern>(&pt)) {
                    const common::Color8 fg =
                        m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255};
                    core::vec::ProceduralPattern cur = defaultProceduralPattern(fg);
                    if (const auto* pp = std::get_if<core::vec::ProceduralPattern>(pat))
                        cur = *pp;
                    openPatternFlyout(chip, cur,
                                      [this, i, chip](const core::vec::ProceduralPattern& np) {
                                          m_working.strokes[i].paint = core::vec::Pattern{np};
                                          chip->setPaint(m_working.strokes[i].paint);
                                          applyLive();
                                      });
                } else {
                    const auto* s = std::get_if<core::vec::SolidPaint>(&pt);
                    const common::Color8 cur =
                        s ? common::toColor8(s->color) : common::Color8{0, 0, 0, 255};
                    openColorFlyout(chip, cur, [this, i, chip](common::Color8 c) {
                        m_working.strokes[i].paint = core::vec::SolidPaint{common::toColorF(c)};
                        chip->setPaint(m_working.strokes[i].paint);
                        applyLive();
                    });
                }
            });
        }
        cy += kRowH + kRowGap;

        blendRow(cy, st.blend, [this, i](core::BlendMode b) { m_working.strokes[i].blend = b; });
        cy += kRowH + kRowGap;

        caption(cy, _("Opacity"));
        slider(cy, 0, 100, 1, "%", st.opacity * 100.0,
               [this, i](double v) { m_working.strokes[i].opacity = float(v / 100.0); });
        cy += kRowH + kRowGap; // the next InstanceHeader's hairline separates the panels
    }

    // Colour Overlay panel (solid paint only).
    if (m_working.colorOverlay.enabled) {
        const bool open = !m_ui->colorOverlayCollapsed;
        panelHeader(cy, _("Color Overlay"), open, [this] {
            m_ui->colorOverlayCollapsed = !m_ui->colorOverlayCollapsed;
            rebuildStack();
        });
        cy += kRowH + 4; // header hairline + a little breathing room below it
        if (open) {
            core::OverlayEffect& ov = m_working.colorOverlay;
            caption(cy, _("Color"));
            auto* chip = new SwatchChip(fieldLeft, cy, fieldW, kRowH);
            chip->setGroundColor(pal.windowBg);
            chip->setInteractive(true);
            const auto* solid = std::get_if<core::vec::SolidPaint>(&ov.paint);
            chip->setColour(common::toColor8(solid ? solid->color : common::ColorF{0, 0, 0, 1}));
            chip->setOnClick([this, chip] {
                const auto* s = std::get_if<core::vec::SolidPaint>(&m_working.colorOverlay.paint);
                const common::Color8 cur =
                    s ? common::toColor8(s->color) : common::Color8{0, 0, 0, 255};
                openColorFlyout(chip, cur, [this, chip](common::Color8 c) {
                    m_working.colorOverlay.paint = core::vec::SolidPaint{common::toColorF(c)};
                    chip->setColour(c);
                    applyLive();
                });
            });
            m_ui->colorOverlayChip = chip;
            cy += kRowH + kRowGap;
            blendRow(cy, ov.blend, [this](core::BlendMode b) { m_working.colorOverlay.blend = b; });
            cy += kRowH + kRowGap;
            caption(cy, _("Opacity"));
            slider(cy, 0, 100, 1, "%", ov.opacity * 100.0,
                   [this](double v) { m_working.colorOverlay.opacity = float(v / 100.0); });
            cy += kRowH + kRowGap;
        }
    }

    // Gradient Overlay panel (a gradient paint; its TYPE is chosen here, the parent control).
    if (m_working.gradientOverlay.enabled) {
        const bool open = !m_ui->gradOverlayCollapsed;
        panelHeader(cy, _("Gradient Overlay"), open, [this] {
            m_ui->gradOverlayCollapsed = !m_ui->gradOverlayCollapsed;
            rebuildStack();
        });
        cy += kRowH + 4; // header hairline + a little breathing room below it
        if (open) {
            core::OverlayEffect& ov = m_working.gradientOverlay;
            caption(cy, _("Type"));
            {
                auto* d = new Dropdown(fieldLeft, cy, fieldW, kRowH);
                for (const char* n : kGradTypeNames)
                    d->add(n);
                const auto* g0 = std::get_if<core::vec::Gradient>(&ov.paint);
                d->value(g0 ? static_cast<int>(g0->type) : 0);
                d->callback(
                    controlThunk, bind([this, d] {
                        if (m_seeding)
                            return;
                        const auto t =
                            static_cast<core::vec::GradientType>(std::clamp(d->value(), 0, 2));
                        if (auto* g = std::get_if<core::vec::Gradient>(
                                &m_working.gradientOverlay.paint)) {
                            const double dir = gradientDirectionDeg(*g); // carry direction across
                            g->type = t;
                            g->transform = directedGradientTransform(t, dir);
                        }
                        if (m_ui->gradOverlayChip)
                            m_ui->gradOverlayChip->setPaint(m_working.gradientOverlay.paint);
                        applyLive();
                    }));
            }
            cy += kRowH + kRowGap;
            caption(cy, _("Gradient"));
            {
                auto* chip = new PaintChip(fieldLeft, cy, fieldW, kRowH);
                chip->setGroundColor(pal.windowBg);
                chip->setPaint(ov.paint);
                chip->setOnClick([this, chip] {
                    const auto* g =
                        std::get_if<core::vec::Gradient>(&m_working.gradientOverlay.paint);
                    core::vec::Gradient cur =
                        g ? *g
                          : defaultGradient(core::vec::GradientType::Linear, {0, 0, 0, 255},
                                            {255, 255, 255, 255});
                    openGradientFlyout(chip, cur, [this, chip](const core::vec::Gradient& ng) {
                        // The flyout edits stops + spread; the Type dropdown + Direction dial own
                        // the type + transform, so keep those.
                        if (auto* g = std::get_if<core::vec::Gradient>(
                                &m_working.gradientOverlay.paint)) {
                            g->stops = ng.stops;
                            g->spread = ng.spread;
                        } else {
                            m_working.gradientOverlay.paint = ng;
                        }
                        chip->setPaint(m_working.gradientOverlay.paint);
                        applyLive();
                    });
                });
                m_ui->gradOverlayChip = chip;
            }
            cy += kRowH + kRowGap;
            // Direction dial (after the gradient selector): rotates the gradient's transform about
            // the content-box centre -- turns a linear axis / a conic sweep start (no-op for a
            // radial).
            caption(cy, _("Direction"));
            {
                auto* dial = new Dial(fieldLeft, cy, kRowH, kRowH);
                dial->range(0, 360);
                dial->step(1);
                dial->setCellColor(pal.windowBg);
                dial->when(FL_WHEN_CHANGED);
                const auto* g0 = std::get_if<core::vec::Gradient>(&ov.paint);
                dial->value(g0 != nullptr ? gradientDirectionDeg(*g0) : 0.0);
                auto* val =
                    new Fl_Box(fieldLeft + kRowH + 8, cy, fieldW - kRowH - 8, kRowH, nullptr);
                val->box(FL_NO_BOX);
                val->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
                val->labelfont(FL_HELVETICA);
                val->labelsize(12);
                val->labelcolor(toFl(pal.text));
                const auto writeVal = [](Fl_Box* b, double deg) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d\xC2\xB0",
                                  static_cast<int>(std::lround(deg)));
                    b->copy_label(buf);
                };
                writeVal(val, dial->value());
                dial->callback(
                    controlThunk, bind([this, dial, val, writeVal] {
                        if (m_seeding)
                            return;
                        if (auto* g =
                                std::get_if<core::vec::Gradient>(&m_working.gradientOverlay.paint))
                            g->transform = directedGradientTransform(g->type, dial->value());
                        writeVal(val, dial->value());
                        val->redraw();
                        if (m_ui->gradOverlayChip)
                            m_ui->gradOverlayChip->setPaint(m_working.gradientOverlay.paint);
                        applyLive();
                    }));
            }
            cy += kRowH + kRowGap;
            blendRow(cy, ov.blend,
                     [this](core::BlendMode b) { m_working.gradientOverlay.blend = b; });
            cy += kRowH + kRowGap;
            caption(cy, _("Opacity"));
            slider(cy, 0, 100, 1, "%", ov.opacity * 100.0,
                   [this](double v) { m_working.gradientOverlay.opacity = float(v / 100.0); });
            cy += kRowH + kRowGap;
        }
    }

    // Pattern Overlay panel (a pattern paint; its kind + colours are chosen in the pattern flyout).
    if (m_working.patternOverlay.enabled) {
        const bool open = !m_ui->patOverlayCollapsed;
        panelHeader(cy, _("Pattern Overlay"), open, [this] {
            m_ui->patOverlayCollapsed = !m_ui->patOverlayCollapsed;
            rebuildStack();
        });
        cy += kRowH + 4; // header hairline + a little breathing room below it
        if (open) {
            core::OverlayEffect& ov = m_working.patternOverlay;
            caption(cy, _("Pattern"));
            {
                auto* chip = new PaintChip(fieldLeft, cy, fieldW, kRowH);
                chip->setGroundColor(pal.windowBg);
                chip->setPaint(ov.paint);
                chip->setOnClick([this, chip] {
                    const common::Color8 fg =
                        m_host.foreground ? m_host.foreground() : common::Color8{0, 0, 0, 255};
                    core::vec::ProceduralPattern cur = defaultProceduralPattern(fg);
                    if (const auto* pat =
                            std::get_if<core::vec::Pattern>(&m_working.patternOverlay.paint))
                        if (const auto* pp = std::get_if<core::vec::ProceduralPattern>(pat))
                            cur = *pp;
                    openPatternFlyout(chip, cur,
                                      [this, chip](const core::vec::ProceduralPattern& np) {
                                          m_working.patternOverlay.paint = core::vec::Pattern{np};
                                          chip->setPaint(m_working.patternOverlay.paint);
                                          applyLive();
                                      });
                });
                m_ui->patOverlayChip = chip;
            }
            cy += kRowH + kRowGap;
            blendRow(cy, ov.blend,
                     [this](core::BlendMode b) { m_working.patternOverlay.blend = b; });
            cy += kRowH + kRowGap;
            caption(cy, _("Opacity"));
            slider(cy, 0, 100, 1, "%", ov.opacity * 100.0,
                   [this](double v) { m_working.patternOverlay.opacity = float(v / 100.0); });
            cy += kRowH + kRowGap;
        }
    }

    // ---- LE-e panels, in the catalogue's canonical order (after the overlays): a Drop Shadow panel
    // per instance, an Inner Shadow panel per instance, then the single Outer Glow + Inner Glow panels.
    for (std::size_t i = 0; i < m_working.dropShadows.size(); ++i)
        emitShadowPanel(&m_working.dropShadows, &m_ui->dropCollapsed, i, _("Drop Shadow"));
    for (std::size_t i = 0; i < m_working.innerShadows.size(); ++i)
        emitShadowPanel(&m_working.innerShadows, &m_ui->innerCollapsed, i, _("Inner Shadow"));
    if (m_working.outerGlow.enabled) {
        const bool open = !m_ui->outerGlowCollapsed;
        panelHeader(cy, _("Outer Glow"), open, [this] {
            m_ui->outerGlowCollapsed = !m_ui->outerGlowCollapsed;
            rebuildStack();
        });
        cy += kRowH + 4;
        if (open)
            emitGlowRows(&m_working.outerGlow, /*inner=*/false);
    }
    if (m_working.innerGlow.enabled) {
        const bool open = !m_ui->innerGlowCollapsed;
        panelHeader(cy, _("Inner Glow"), open, [this] {
            m_ui->innerGlowCollapsed = !m_ui->innerGlowCollapsed;
            rebuildStack();
        });
        cy += kRowH + 4;
        if (open)
            emitGlowRows(&m_working.innerGlow, /*inner=*/true);
    }

    // ---- LE-f: Bevel & Emboss panel (single). Style + steepness/size/soften + light angle/altitude
    // + highlight and shadow colours & opacities. Every edit routes through the shared slider/blend
    // funnel (one undo step per drag); the colour rows mirror the Colour-Overlay SwatchChip line. ----
    if (m_working.bevel.enabled) {
        const bool open = !m_ui->bevelCollapsed;
        panelHeader(cy, _("Bevel & Emboss"), open, [this] {
            m_ui->bevelCollapsed = !m_ui->bevelCollapsed;
            rebuildStack();
        });
        cy += kRowH + 4; // header hairline + a little breathing room below it
        if (open) {
            core::BevelEffect& bv = m_working.bevel;
            caption(cy, _("Style"));
            {
                auto* d = new Dropdown(fieldLeft, cy, fieldW, kRowH);
                for (const char* n : kBevelStyleNames)
                    d->add(n);
                d->value(static_cast<int>(bv.style));
                d->callback(controlThunk, bind([this, d] {
                                if (m_seeding)
                                    return;
                                m_working.bevel.style = static_cast<core::BevelEffect::Style>(
                                    std::clamp(d->value(), 0, 3));
                                applyLive();
                            }));
            }
            cy += kRowH + kRowGap;
            caption(cy, _("Depth"));
            slider(cy, 0, 500, 1, "%", bv.depth * 100.0,
                   [this](double v) { m_working.bevel.depth = float(v / 100.0); });
            cy += kRowH + kRowGap;
            caption(cy, _("Size"));
            slider(cy, 0, 250, 0.5, "px", bv.size,
                   [this](double v) { m_working.bevel.size = float(v); });
            cy += kRowH + kRowGap;
            caption(cy, _("Soften"));
            slider(cy, 0, 40, 0.5, "px", bv.soften,
                   [this](double v) { m_working.bevel.soften = float(v); });
            cy += kRowH + kRowGap;
            angleRow(cy, bv.angleDeg, [this](double v) { m_working.bevel.angleDeg = float(v); });
            cy += kRowH + kRowGap;
            caption(cy, _("Altitude"));
            slider(cy, 0, 90, 1, "\xC2\xB0", bv.altitudeDeg,
                   [this](double v) { m_working.bevel.altitudeDeg = float(v); });
            cy += kRowH + kRowGap;
            caption(cy, _("Highlight"));
            {
                auto* chip = new SwatchChip(fieldLeft, cy, fieldW, kRowH);
                chip->setGroundColor(pal.windowBg);
                chip->setInteractive(true);
                chip->setColour(common::toColor8(bv.highlight));
                chip->setOnClick([this, chip] {
                    openColorFlyout(chip, common::toColor8(m_working.bevel.highlight),
                                    [this, chip](common::Color8 c) {
                                        m_working.bevel.highlight = common::toColorF(c);
                                        chip->setColour(c);
                                        applyLive();
                                    });
                });
            }
            cy += kRowH + kRowGap;
            caption(cy, _("Hl. Opacity"));
            slider(cy, 0, 100, 1, "%", bv.highlightOpacity * 100.0,
                   [this](double v) { m_working.bevel.highlightOpacity = float(v / 100.0); });
            cy += kRowH + kRowGap;
            caption(cy, _("Shadow"));
            {
                auto* chip = new SwatchChip(fieldLeft, cy, fieldW, kRowH);
                chip->setGroundColor(pal.windowBg);
                chip->setInteractive(true);
                chip->setColour(common::toColor8(bv.shadow));
                chip->setOnClick([this, chip] {
                    openColorFlyout(chip, common::toColor8(m_working.bevel.shadow),
                                    [this, chip](common::Color8 c) {
                                        m_working.bevel.shadow = common::toColorF(c);
                                        chip->setColour(c);
                                        applyLive();
                                    });
                });
            }
            cy += kRowH + kRowGap;
            caption(cy, _("Sh. Opacity"));
            slider(cy, 0, 100, 1, "%", bv.shadowOpacity * 100.0,
                   [this](double v) { m_working.bevel.shadowOpacity = float(v / 100.0); });
            cy += kRowH + kRowGap;
        }
    }

    // ---- LE-f: Satin panel (single). Colour + blend/opacity + the (angle,distance) offset + size
    // (the sheen blur) + Invert (the abs-difference vs sum of the two offset copies). ----
    if (m_working.satin.enabled) {
        const bool open = !m_ui->satinCollapsed;
        panelHeader(cy, _("Satin"), open, [this] {
            m_ui->satinCollapsed = !m_ui->satinCollapsed;
            rebuildStack();
        });
        cy += kRowH + 4; // header hairline + a little breathing room below it
        if (open) {
            core::SatinEffect& sa = m_working.satin;
            caption(cy, _("Color"));
            {
                auto* chip = new SwatchChip(fieldLeft, cy, fieldW, kRowH);
                chip->setGroundColor(pal.windowBg);
                chip->setInteractive(true);
                chip->setColour(common::toColor8(sa.color));
                chip->setOnClick([this, chip] {
                    openColorFlyout(chip, common::toColor8(m_working.satin.color),
                                    [this, chip](common::Color8 c) {
                                        m_working.satin.color = common::toColorF(c);
                                        chip->setColour(c);
                                        applyLive();
                                    });
                });
            }
            cy += kRowH + kRowGap;
            blendRow(cy, sa.blend, [this](core::BlendMode b) { m_working.satin.blend = b; });
            cy += kRowH + kRowGap;
            caption(cy, _("Opacity"));
            slider(cy, 0, 100, 1, "%", sa.opacity * 100.0,
                   [this](double v) { m_working.satin.opacity = float(v / 100.0); });
            cy += kRowH + kRowGap;
            angleRow(cy, sa.angleDeg, [this](double v) { m_working.satin.angleDeg = float(v); });
            cy += kRowH + kRowGap;
            caption(cy, _("Distance"));
            slider(cy, 0, 250, 0.5, "px", sa.distance,
                   [this](double v) { m_working.satin.distance = float(v); });
            cy += kRowH + kRowGap;
            caption(cy, _("Size"));
            slider(cy, 0, 250, 0.5, "px", sa.size,
                   [this](double v) { m_working.satin.size = float(v); });
            cy += kRowH + kRowGap;
            caption(cy, _("Invert"));
            {
                // A bare CheckBox (own callback, not the Binding thunk) aligned under the field column.
                auto* cb = new CheckBox(fieldLeft, cy, fieldW, kRowH, nullptr);
                cb->setGroundColor(pal.windowBg);
                cb->setChecked(sa.invert);
                cb->setOnToggle([this](bool on) {
                    if (m_seeding)
                        return;
                    m_working.satin.invert = on;
                    applyLive();
                });
            }
            cy += kRowH + kRowGap;
        }
    }

    if (m_working.strokes.empty() && !m_working.colorOverlay.enabled &&
        !m_working.gradientOverlay.enabled && !m_working.patternOverlay.enabled &&
        m_working.dropShadows.empty() && m_working.innerShadows.empty() &&
        !m_working.outerGlow.enabled && !m_working.innerGlow.enabled &&
        !m_working.bevel.enabled && !m_working.satin.enabled) {
        auto* hint = new Fl_Box(stack->x() + left, cy, cw - 2 * left, kRowH * 2,
                                _("Enable an effect from the list to edit it here."));
        hint->box(FL_NO_BOX);
        hint->align(FL_ALIGN_CENTER | FL_ALIGN_WRAP);
        hint->labelfont(FL_HELVETICA);
        hint->labelsize(12);
        hint->labelcolor(toFl(pal.textMuted));
        cy += kRowH * 2;
    }

    stack->size(stack->w(), std::max(m_ui->scroll->h(), cy - stack->y() + 8));
    stack->end();
    m_ui->scroll->redraw();
}

void LayerEffectsDialog::syncCatalog() {
    m_seeding = true;
    if (m_ui->catToggles[0])
        m_ui->catToggles[0]->setChecked(!m_working.strokes.empty());
    if (m_ui->catSteppers[0])
        m_ui->catSteppers[0]->setCount(static_cast<int>(m_working.strokes.size()));
    if (m_ui->catToggles[1])
        m_ui->catToggles[1]->setChecked(m_working.colorOverlay.enabled);
    if (m_ui->catToggles[2])
        m_ui->catToggles[2]->setChecked(m_working.gradientOverlay.enabled);
    if (m_ui->catToggles[3])
        m_ui->catToggles[3]->setChecked(m_working.patternOverlay.enabled);
    // ---- LE-e: reflect the shadow/glow enable + instance-count state into the rail.
    if (m_ui->catToggles[4])
        m_ui->catToggles[4]->setChecked(!m_working.dropShadows.empty());
    if (m_ui->catSteppers[4])
        m_ui->catSteppers[4]->setCount(static_cast<int>(m_working.dropShadows.size()));
    if (m_ui->catToggles[5])
        m_ui->catToggles[5]->setChecked(!m_working.innerShadows.empty());
    if (m_ui->catSteppers[5])
        m_ui->catSteppers[5]->setCount(static_cast<int>(m_working.innerShadows.size()));
    if (m_ui->catToggles[6])
        m_ui->catToggles[6]->setChecked(m_working.outerGlow.enabled);
    if (m_ui->catToggles[7])
        m_ui->catToggles[7]->setChecked(m_working.innerGlow.enabled);
    // ---- LE-f: mirror the shading-tier enable state into its catalogue rows. ----
    if (m_ui->catToggles[8])
        m_ui->catToggles[8]->setChecked(m_working.bevel.enabled);
    if (m_ui->catToggles[9])
        m_ui->catToggles[9]->setChecked(m_working.satin.enabled);
    if (m_ui->fillOpacity)
        m_ui->fillOpacity->value(m_working.fillOpacity * 100.0);
    m_seeding = false;
}

void LayerEffectsDialog::seed(std::string label, std::optional<core::LayerEffects> initial) {
    m_label = std::move(label);
    m_original = initial;
    m_working = initial.value_or(core::LayerEffects{});
    m_onColorPick = nullptr;
    m_onGradientChange = nullptr;
    m_onPatternChange = nullptr;
    m_ui->colorOverlayCollapsed = false;
    m_ui->gradOverlayCollapsed = false;
    m_ui->patOverlayCollapsed = false;
    // ---- LE-e: reset the shadow/glow collapse state to the seeded stack.
    m_ui->outerGlowCollapsed = false;
    m_ui->innerGlowCollapsed = false;
    m_ui->dropCollapsed.assign(m_working.dropShadows.size(), false);
    m_ui->innerCollapsed.assign(m_working.innerShadows.size(), false);
    m_ui->bevelCollapsed = false; // LE-f
    m_ui->satinCollapsed = false; // LE-f
    m_ui->collapsed.assign(m_working.strokes.size(), false);
    m_seeding = true;
    syncCatalog();
    rebuildStack();
    m_seeding = false;
    applyLive(); // set the layer to its current effects live + preview
}

std::optional<core::LayerEffects> LayerEffectsDialog::currentEffects() const {
    if (m_working.empty())
        return std::nullopt;
    return m_working;
}

void LayerEffectsDialog::applyLive() {
    if (m_host.applyLive)
        m_host.applyLive(currentEffects());
    requestPreview();
}

void LayerEffectsDialog::requestPreview() {
    if (m_previewPending)
        return;
    m_previewPending = true;
    Fl::add_timeout(1.0 / 60.0, previewTimer, this);
}

void LayerEffectsDialog::previewTimer(void* self) {
    auto* d = static_cast<LayerEffectsDialog*>(self);
    d->m_previewPending = false;
    d->recomputePreview();
}

void LayerEffectsDialog::recomputePreview() {
    if (!m_host.renderPreview || m_ui->preview == nullptr)
        return;
    PreviewContent pc = m_host.renderPreview(m_ui->preview->w(), m_ui->preview->h());
    if (pc.image.empty()) {
        m_ui->preview->clearImage();
        m_ui->preview->setNote(_("Nothing to preview"));
    } else {
        m_ui->preview->setContent(pc);
    }
    // The Fill panel overlaps the preview: a preview refresh damages only the preview, so re-damage
    // the panel too (later sibling -> redraws on top, no overpaint).
    if (m_ui->fillPanel)
        m_ui->fillPanel->redraw();
}

void LayerEffectsDialog::applyPreviewAvoid(BubbleFlyout* flyout) {
    if (flyout == nullptr || m_ui->preview == nullptr)
        return;
    // Keep clear of the preview's ACTIVE content, NOT the whole pane -- so the flyout comes right
    // up to it (overlapping the checker surround is fine). The pane is wide (~3:1) and the content
    // is aspect-fit to its HEIGHT, so the real content is a CENTRED strip ~pane-height wide with
    // wide surround; protect just that centred band (only its left edge + y-span drive the leftward
    // shift).
    const int paneW = m_ui->preview->w();
    const int protW =
        std::min(paneW, m_ui->preview->h()); // centred content band (~pane height wide)
    const int inset = (paneW - protW) / 2;
    flyout->setAvoidRect(m_ui->preview->x() + inset, m_ui->preview->y(), protW, m_ui->preview->h());
}

void LayerEffectsDialog::openColorFlyout(const Fl_Widget* anchor, common::Color8 current,
                                         std::function<void(common::Color8)> onPick) {
    if (m_ui->colorFlyout == nullptr)
        return;
    if (m_ui->colorFlyout->shownForAnchor(anchor)) { // a re-click toggles it shut
        m_ui->colorFlyout->hide();
        m_onColorPick = nullptr;
        return;
    }
    if (m_ui->gradientFlyout != nullptr)
        m_ui->gradientFlyout->hide(); // one bubble on screen
    if (m_ui->patternFlyout != nullptr)
        m_ui->patternFlyout->hide();
    m_onColorPick = std::move(onPick);
    applyPreviewAvoid(m_ui->colorFlyout);
    m_ui->colorFlyout->openFor(anchor, current);
}

void LayerEffectsDialog::openGradientFlyout(
    const Fl_Widget* anchor, const core::vec::Gradient& g,
    std::function<void(const core::vec::Gradient&)> onChange) {
    if (m_ui->gradientFlyout == nullptr)
        return;
    if (m_ui->gradientFlyout->shownForAnchor(anchor)) {
        m_ui->gradientFlyout->hide();
        m_onGradientChange = nullptr;
        return;
    }
    if (m_ui->colorFlyout != nullptr)
        m_ui->colorFlyout->hide();
    if (m_ui->patternFlyout != nullptr)
        m_ui->patternFlyout->hide();
    m_onGradientChange = std::move(onChange);
    applyPreviewAvoid(m_ui->gradientFlyout);
    m_ui->gradientFlyout->openFor(anchor, g);
}

void LayerEffectsDialog::openPatternFlyout(
    const Fl_Widget* anchor, const core::vec::ProceduralPattern& pat,
    std::function<void(const core::vec::ProceduralPattern&)> onChange) {
    if (m_ui->patternFlyout == nullptr)
        return;
    if (m_ui->patternFlyout->shownForAnchor(anchor)) {
        m_ui->patternFlyout->hide();
        m_onPatternChange = nullptr;
        return;
    }
    if (m_ui->colorFlyout != nullptr)
        m_ui->colorFlyout->hide();
    if (m_ui->gradientFlyout != nullptr)
        m_ui->gradientFlyout->hide();
    m_onPatternChange = std::move(onChange);
    m_ui->patternFlyout->setAntialias(m_host.antialias ? m_host.antialias() : true);
    applyPreviewAvoid(m_ui->patternFlyout);
    m_ui->patternFlyout->openFor(anchor, pat);
}

void LayerEffectsDialog::doOk() {
    if (m_host.commit)
        m_host.commit(currentEffects());
    hide();
}

void LayerEffectsDialog::doCancel() {
    if (m_host.applyLive)
        m_host.applyLive(m_original); // revert the live layer
    hide();
}

void LayerEffectsDialog::reapplyTheme() {
    color(toFl(activePalette().windowBg));
    redraw();
}

int LayerEffectsDialog::handle(int event) {
    if (event == FL_PUSH) {
        dismissActiveDropdownPopupOnOutsideClick(Fl::event_x(), Fl::event_y());
        dismissActiveContextMenuOnOutsideClick(Fl::event_x(), Fl::event_y());
        dismissActiveColorFlyoutOnOutsideClick(Fl::event_x(), Fl::event_y());
        dismissActiveGradientFlyoutOnOutsideClick(Fl::event_x(), Fl::event_y());
        dismissActivePatternFlyoutOnOutsideClick(Fl::event_x(), Fl::event_y());
    }
    if (event == FL_MOUSEWHEEL) {
        // Scrolling the instance stack moves a chip out from under its (anchored) flyout, so the
        // triangle would point at empty space -- close it (a scrollbar DRAG already dismisses via
        // the FL_PUSH outside-click above). BUT a wheel OVER the flyout is meant to scroll the
        // flyout's own content (the pattern flyout has a scrollable pane), so only dismiss when the
        // wheel is OUTSIDE it. Fall through so the wheel still scrolls the stack.
        const int ex = Fl::event_x(), ey = Fl::event_y();
        const auto closeIfOutside = [&](BubbleFlyout* f) {
            if (f != nullptr && f->shown() && !f->spansHostPoint(ex, ey))
                f->hide();
        };
        closeIfOutside(m_ui->colorFlyout);
        closeIfOutside(m_ui->gradientFlyout);
        closeIfOutside(m_ui->patternFlyout);
    }
    if (event == FL_KEYDOWN) {
        if (Fl::event_key() == FL_Escape) {
            // Escape closes an open flyout first, only then cancels the dialog.
            if (m_ui->colorFlyout != nullptr && m_ui->colorFlyout->shown()) {
                m_ui->colorFlyout->hide();
                return 1;
            }
            if (m_ui->gradientFlyout != nullptr && m_ui->gradientFlyout->shown()) {
                m_ui->gradientFlyout->hide();
                return 1;
            }
            if (m_ui->patternFlyout != nullptr && m_ui->patternFlyout->shown()) {
                m_ui->patternFlyout->hide();
                return 1;
            }
            doCancel();
            return 1;
        }
        if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) {
            doOk();
            return 1;
        }
    }
    return Fl_Double_Window::handle(event);
}

} // namespace mosaic::ui
