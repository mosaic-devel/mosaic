#include "ui/pattern_flyout.hpp"

#include "common/i18n.hpp"
#include "ui/color_models.hpp"    // rgbToHsv / hsvToRgb
#include "ui/color_surfaces.hpp"  // SvField, HueStrip
#include "ui/scrub_slider.hpp"    // ScrubSlider, ScrubRuler
#include "ui/theme.hpp"
#include "ui/widgets.hpp"  // Slider, Dial, CheckBox, ScrollView, SwatchButton, HexField

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

namespace mosaic::ui {
namespace {

using K = core::vec::ProceduralPattern::Kind;

constexpr int kTri = BubbleFlyout::kTri;
constexpr int kPad = BubbleFlyout::kPad;
constexpr int kContentX = BubbleFlyout::kContentX;
constexpr int kColGap = 12;  // between the two panes

// Left pane -- colour editor.
constexpr int kLeftW = 148;
constexpr int kHueW = 14;
constexpr int kHueGap = 6;
constexpr int kFieldW = kLeftW - kHueW - kHueGap;  // 128

// Right pane -- pattern specifics (scrollable).
constexpr int kScrollBarW = 15;
constexpr int kRightW = 188;                        // pane width incl. scrollbar gutter
constexpr int kRightInner = kRightW - kScrollBarW;  // 173 content width
constexpr int kCols = 5, kRows = 5, kCellGap = 4;
constexpr int kCellW = (kRightInner - (kCols - 1) * kCellGap) / kCols;  // 31
constexpr int kCellH = kCellW;
constexpr int kGridW = kCols * kCellW + (kCols - 1) * kCellGap;                 // 171
constexpr int kGridRows = kRows * kCellH + (kRows - 1) * kCellGap;              // 171
constexpr int kNameH = 16;
constexpr double kPreviewScale = 9.0;  // feature size in preview px

constexpr int kBodyW = kPad + kLeftW + kColGap + kRightW + kPad;
constexpr int kWinW = kTri + kBodyW;
constexpr int kWinH = 344;

// Left-pane vertical layout.
constexpr int kTargetY = kPad;  // 10
constexpr int kTargetH = 22;
constexpr int kSwatchW = 28;
constexpr int kCapW = 20;       // "Fg" / "Bg" label width
constexpr int kLabelGap = 4;    // between a label and its own swatch (so they don't collide)
constexpr int kGroupGap = 10;   // between the fg group and the bg group
constexpr int kFgSwatchDX = kCapW + kLabelGap;                  // fg swatch x, from content-left
constexpr int kBgLabelDX = kFgSwatchDX + kSwatchW + kGroupGap;  // bg label x
constexpr int kBgSwatchDX = kBgLabelDX + kCapW + kLabelGap;     // bg swatch x
constexpr int kHexH = 26;
constexpr int kHexY = kWinH - kPad - kHexH;   // hex + swatch pinned to the bottom
constexpr int kAlphaH = 22;
constexpr int kAlphaY = kHexY - 8 - kAlphaH;  // alpha above the hex
constexpr int kSurfY = kTargetY + kTargetH + 10;
constexpr int kSurfH = kAlphaY - 8 - kSurfY;  // sat/val field fills the middle
constexpr int kAlphaLabelW = 16;
constexpr int kHexInputW = 88;
constexpr int kHexSwatchGap = 6;
constexpr int kHexSwatchW = kLeftW - kHexInputW - kHexSwatchGap;  // 54

// Right-pane control rows (relative to the scroll content top).
constexpr int kRowH = 22;
constexpr int kRowGap = 8;
constexpr int kCtlLabelW = 52;
constexpr int kCtlGap = 6;
constexpr int kDialSide = 34;
constexpr int kAnchorH = 20;

constexpr int kCheck = 6;  // checkerboard cell (transparency)

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

// Paint `c` (its alpha honoured) over the transparency checkerboard in the rect.
void drawSwatchChecker(int rx, int ry, int rw, int rh, common::ColorF c) {
    const float a = std::clamp(c.a, 0.0f, 1.0f);
    for (int yy = 0; yy < rh; ++yy)
        for (int xx = 0; xx < rw; ++xx) {
            const bool dk = ((xx / kCheck) + (yy / kCheck)) & 1;
            const float bg = dk ? 205.0f : 255.0f;
            const auto ch = [&](float v) {
                return static_cast<std::uint8_t>(
                    std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f * a + bg * (1.0f - a)));
            };
            fl_color(fl_rgb_color(ch(c.r), ch(c.g), ch(c.b)));
            fl_point(rx + xx, ry + yy);
        }
}

// Render a procedural pattern of `kind` (with `base`'s colours/angle/weight/spacing, at a preview
// scale) into an RGB buffer composited over a checker, and blit it at (rx,ry). `antialias` follows the
// document AA setting so the preview reads like the canvas will (crisp docs show crisp previews).
void drawPatternCell(int rx, int ry, int rw, int rh, const core::vec::ProceduralPattern& base, K kind,
                     bool antialias) {
    core::vec::ProceduralPattern p = base;
    p.kind = kind;
    p.scale = static_cast<float>(kPreviewScale);
    const core::vec::Pattern pv = p;
    std::vector<unsigned char> buf(static_cast<std::size_t>(rw) * rh * 3);
    for (int yy = 0; yy < rh; ++yy)
        for (int xx = 0; xx < rw; ++xx) {
            const common::ColorF c = core::vec::samplePattern(pv, {xx + 0.5, yy + 0.5}, antialias);
            const bool dk = ((xx / kCheck) + (yy / kCheck)) & 1;
            const float bg = dk ? 205.0f : 255.0f;
            const float a = std::clamp(c.a, 0.0f, 1.0f);
            const auto over = [&](float v) {
                return static_cast<unsigned char>(
                    std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f * a + bg * (1.0f - a)));
            };
            const std::size_t o = (static_cast<std::size_t>(yy) * rw + xx) * 3;
            buf[o] = over(c.r);
            buf[o + 1] = over(c.g);
            buf[o + 2] = over(c.b);
        }
    Fl_RGB_Image img(buf.data(), rw, rh, 3);
    img.draw(rx, ry);
}

std::optional<common::Color8> parseHex(const char* s) {
    if (s == nullptr) return std::nullopt;
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == '#') ++s;
    char digits[7];
    int n = 0;
    for (; *s != '\0' && n < 6; ++s) {
        if (!std::isxdigit(static_cast<unsigned char>(*s))) return std::nullopt;
        digits[n++] = *s;
    }
    if (n != 6 || *s != '\0') return std::nullopt;
    digits[6] = '\0';
    const long v = std::strtol(digits, nullptr, 16);
    return common::Color8{static_cast<std::uint8_t>((v >> 16) & 0xFF),
                          static_cast<std::uint8_t>((v >> 8) & 0xFF),
                          static_cast<std::uint8_t>(v & 0xFF), 255};
}

PatternFlyout* g_active = nullptr;

}  // namespace

// The 5x5 grid of live kind previews + the selected-kind name, as a self-contained widget so the
// scrollable right pane can carry it. A click picks a kind (and, incidentally, consumes the press so
// it never falls through to the host). setPattern feeds it the current colours/angle so the previews
// stay live; the selected kind carries an accent frame.
class PatternGrid : public Fl_Widget {
public:
    PatternGrid(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}
    void setPattern(const core::vec::ProceduralPattern& p) {
        m_pat = p;
        redraw();
    }
    void setAntialias(bool aa) {
        if (aa == m_aa) return;
        m_aa = aa;
        redraw();
    }
    void setOnPick(std::function<void(int)> f) { m_onPick = std::move(f); }

    static constexpr int kGridTop = kNameH + 4;          // the name rides on TOP; cells below it
    static constexpr int kHeight = kGridTop + kGridRows;

protected:
    void draw() override {
        const Palette& p = activePalette();
        fl_color(toFl(p.panelBg));  // erase our cell first ([[mosaic-ui-gotchas]])
        fl_rectf(x(), y(), w(), h());
        fl_color(toFl(p.text));
        fl_font(FL_HELVETICA_BOLD, 12);
        fl_draw(core::vec::patternKindName(m_pat.kind), x(), y(), w(), kNameH,
                FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
        for (int r = 0; r < kRows; ++r)
            for (int c = 0; c < kCols; ++c) {
                const int idx = r * kCols + c;
                if (idx >= core::vec::ProceduralPattern::kKindCount) continue;
                const int cx = x() + c * (kCellW + kCellGap);
                const int cy = y() + kGridTop + r * (kCellH + kCellGap);
                drawPatternCell(cx, cy, kCellW, kCellH, m_pat, static_cast<K>(idx), m_aa);
                const bool sel = idx == static_cast<int>(m_pat.kind);
                fl_color(toFl(sel ? p.accent : p.border));
                if (sel) fl_rect(cx - 1, cy - 1, kCellW + 2, kCellH + 2);
                fl_rect(cx, cy, kCellW, kCellH);
            }
    }
    int handle(int e) override {
        if (e == FL_PUSH) {
            const int lx = Fl::event_x(), ly = Fl::event_y();
            for (int r = 0; r < kRows; ++r)
                for (int c = 0; c < kCols; ++c) {
                    const int idx = r * kCols + c;
                    if (idx >= core::vec::ProceduralPattern::kKindCount) continue;
                    const int cx = x() + c * (kCellW + kCellGap);
                    const int cy = y() + kGridTop + r * (kCellH + kCellGap);
                    if (lx >= cx && lx < cx + kCellW && ly >= cy && ly < cy + kCellH) {
                        if (idx != static_cast<int>(m_pat.kind) && m_onPick) m_onPick(idx);
                        return 1;
                    }
                }
            return 1;  // a press anywhere on the grid stays here (no fall-through to the host)
        }
        return Fl_Widget::handle(e);
    }

private:
    core::vec::ProceduralPattern m_pat;
    bool m_aa = true;
    std::function<void(int)> m_onPick;
};

PatternFlyout::PatternFlyout() : BubbleFlyout(kWinW, kWinH) {
    begin();

    const Palette& pal = activePalette();
    const auto mkCaption = [&](int cx, int cy, int cw, int ch, const char* text) {
        auto* b = new Fl_Box(cx, cy, cw, ch, text);
        b->box(FL_NO_BOX);
        b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        b->labelfont(FL_HELVETICA);
        b->labelsize(12);
        b->labelcolor(toFl(pal.textMuted));
        return b;
    };

    // ---- Right pane: pattern specifics, inside a vertical scroll ----
    const int rightX = kContentX + kLeftW + kColGap;
    auto* sv = new ScrollView(rightX, kPad, kRightW, kWinH - 2 * kPad);
    sv->type(Fl_Scroll::VERTICAL);
    sv->color(toFl(pal.panelBg));
    sv->box(FL_FLAT_BOX);
    sv->begin();
    const int rx = rightX;                          // content-left inside the scroll
    const int slX = rx + kCtlLabelW + kCtlGap;
    const int slW = kRightInner - kCtlLabelW - kCtlGap;
    int ry = kPad;

    m_scaleLabel = mkCaption(rx, ry, kCtlLabelW, kRowH, "Scale");
    m_scale = new ScrubSlider(slX, ry, slW, kRowH);
    m_scale->range(4, 256);
    m_scale->step(1);
    m_scale->setSuffix("px");
    m_scale->setCellColor(pal.panelBg);
    m_scale->when(FL_WHEN_CHANGED);
    m_scale->callback(
        [](Fl_Widget* w, void* v) {
            auto* self = static_cast<PatternFlyout*>(v);
            if (self->m_syncing) return;
            self->m_pat.scale = static_cast<float>(static_cast<ScrubSlider*>(w)->value());
            self->emitChange();
        },
        this);
    ry += kRowH + kRowGap;

    m_offsetLabel = mkCaption(rx, ry, kCtlLabelW, kRowH, "Offset");
    m_offset = new ScrubSlider(slX, ry, slW, kRowH);
    m_offset->range(0, 100);
    m_offset->step(1);
    m_offset->setSuffix("%");
    m_offset->setCellColor(pal.panelBg);
    m_offset->when(FL_WHEN_CHANGED);
    m_offset->callback(
        [](Fl_Widget* w, void* v) {
            auto* self = static_cast<PatternFlyout*>(v);
            if (self->m_syncing) return;
            self->m_pat.offset = static_cast<float>(static_cast<ScrubSlider*>(w)->value() / 100.0);
            if (self->m_grid) self->m_grid->setPattern(self->m_pat);
            self->emitChange();
        },
        this);
    ry += kRowH + kRowGap;

    // Weight / Distance (relabelled + hidden per kind; when hidden, the rows below realign up).
    m_weightLabel = mkCaption(rx, ry, kCtlLabelW, kRowH, "Weight");
    m_weight = new ScrubSlider(slX, ry, slW, kRowH);
    m_weight->range(0, 100);
    m_weight->step(1);
    m_weight->setSuffix("%");
    m_weight->setCellColor(pal.panelBg);
    m_weight->when(FL_WHEN_CHANGED);
    m_weight->callback(
        [](Fl_Widget* w, void* v) {
            auto* self = static_cast<PatternFlyout*>(v);
            if (self->m_syncing) return;
            const float val = static_cast<float>(static_cast<ScrubSlider*>(w)->value() / 100.0);
            if (core::vec::patternUsesSpacing(self->m_pat.kind))
                self->m_pat.spacing = val;
            else
                self->m_pat.weight = val;
            if (self->m_grid) self->m_grid->setPattern(self->m_pat);
            self->emitChange();
        },
        this);
    ry += kRowH + kRowGap;

    m_angleLabel = mkCaption(rx, ry, kCtlLabelW, kDialSide, "Angle");
    m_angle = new Dial(slX, ry, kDialSide, kDialSide);
    // Degrees readout to the RIGHT of the dial (its own box, so a triple-digit value never slides
    // under the dial the way it did when the caption carried the number).
    const int avX = slX + kDialSide + 8;
    m_angleValue = mkCaption(avX, ry, rx + kRightInner - avX, kDialSide, "0\xC2\xB0");
    m_angle->range(0, 360);
    m_angle->step(1);
    m_angle->setCellColor(pal.panelBg);
    m_angle->when(FL_WHEN_CHANGED);
    m_angle->callback(
        [](Fl_Widget* w, void* v) {
            auto* self = static_cast<PatternFlyout*>(v);
            if (self->m_syncing) return;
            self->m_pat.angleDeg = static_cast<float>(static_cast<Dial*>(w)->value());
            self->refreshSliderLabels();
            if (self->m_grid) self->m_grid->setPattern(self->m_pat);
            self->emitChange();
        },
        this);
    ry += kDialSide + 6;

    m_anchorCheck = new CheckBox(rx, ry, kRightInner, kAnchorH, _("Anchor to canvas"));
    m_anchorCheck->setGroundColor(pal.panelBg);
    m_anchorCheck->tooltip(
        _("Keep the pattern fixed to the canvas: moving or rotating the layer slides "
          "its shape over the pattern instead of dragging the pattern along"));
    m_anchorCheck->setOnToggle([this](bool on) {
        if (m_syncing) return;
        m_pat.anchorToCanvas = on;
        emitChange();
    });
    ry += kAnchorH + kRowGap;

    m_grid = new PatternGrid(rx, ry, kGridW, PatternGrid::kHeight);
    m_grid->setOnPick([this](int idx) {
        m_pat.kind = static_cast<K>(idx);
        updateKindControls();
        if (m_grid) m_grid->setPattern(m_pat);
        emitChange();
    });
    ry += PatternGrid::kHeight + kRowGap;

    m_ld2 = new Fl_Box(rx, ry, kRightInner, 44,
                       _("Image tiles\n(coming soon)"));
    m_ld2->box(FL_BORDER_FRAME);
    m_ld2->color(toFl(pal.border));
    m_ld2->labelfont(FL_HELVETICA);
    m_ld2->labelsize(11);
    m_ld2->labelcolor(toFl(pal.textMuted));
    m_ld2->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);

    sv->end();
    m_rightPane = sv;

    // ---- Left pane: colour editor ----
    m_fgLabel = mkCaption(kContentX, kTargetY, kCapW, kTargetH, "Fg");
    m_fgSwatch = new SwatchButton(kContentX + kFgSwatchDX, kTargetY, kSwatchW, kTargetH);
    m_fgSwatch->setColorGetter([this] { return common::toColor8(m_pat.fg); });
    m_fgSwatch->setOnClick([this] { setTarget(Target::Fg); });
    m_fgSwatch->tooltip(_("Foreground colour (click to edit)"));
    m_bgLabel = mkCaption(kContentX + kBgLabelDX, kTargetY, kCapW, kTargetH, "Bg");
    m_bgSwatch = new SwatchButton(kContentX + kBgSwatchDX, kTargetY, kSwatchW, kTargetH);
    m_bgSwatch->setColorGetter([this] { return common::toColor8(m_pat.bg); });
    m_bgSwatch->setOnClick([this] { setTarget(Target::Bg); });
    m_bgSwatch->tooltip(_("Background colour (click to edit)"));
    m_useFgSwatch = new SwatchButton(kContentX + kLeftW - kSwatchW, kTargetY, kSwatchW, kTargetH);
    m_useFgSwatch->setColorGetter(
        [this] { return m_useFg ? m_useFg() : common::Color8{0, 0, 0, 255}; });
    m_useFgSwatch->setOnClick([this] {
        if (!m_useFg) return;
        const common::Color8 c = m_useFg();
        const Hsv hsv = rgbToHsv(c);
        if (hsv.s > 0.0F && hsv.v > 0.0F) m_h = hsv.h;
        m_s = hsv.s;
        m_v = hsv.v;
        m_a = c.a / 255.0F;
        pushToSurfaces();
        writeTargetColour();
    });
    m_useFgSwatch->tooltip(_("Set the active colour to the app foreground"));

    m_field = new SvField(kContentX, kSurfY, kFieldW, kSurfH);
    m_field->callback([](Fl_Widget* w, void* v) { static_cast<PatternFlyout*>(v)->onSurfaceEdited(w); },
                      this);
    m_hueStrip = new HueStrip(kContentX + kFieldW + kHueGap, kSurfY, kHueW, kSurfH);
    m_hueStrip->callback(
        [](Fl_Widget* w, void* v) { static_cast<PatternFlyout*>(v)->onSurfaceEdited(w); }, this);

    m_alphaLabel = mkCaption(kContentX, kAlphaY, kAlphaLabelW, kAlphaH, _("A"));
    m_alpha = new Slider(kContentX + kAlphaLabelW + 6, kAlphaY, kLeftW - kAlphaLabelW - 6, kAlphaH);
    m_alpha->range(0, 100);
    m_alpha->step(1);
    m_alpha->setCellColor(pal.panelBg);
    m_alpha->when(FL_WHEN_CHANGED);
    m_alpha->callback(
        [](Fl_Widget* w, void* v) {
            auto* self = static_cast<PatternFlyout*>(v);
            if (self->m_syncing) return;
            self->m_a = static_cast<float>(static_cast<Slider*>(w)->value() / 100.0);
            self->writeTargetColour();
        },
        this);

    auto* hex = new HexField(kContentX, kHexY, kHexInputW, kHexH);
    hex->input()->when(FL_WHEN_CHANGED);
    hex->input()->callback([](Fl_Widget*, void* v) { static_cast<PatternFlyout*>(v)->onHexEdited(); },
                           this);
    m_hex = hex->input();

    end();
}

PatternFlyout::~PatternFlyout() {
    if (g_active == this) g_active = nullptr;
}

void PatternFlyout::setUseForeground(std::function<common::Color8()> get) {
    m_useFg = std::move(get);
    if (m_useFgSwatch != nullptr) m_useFgSwatch->redraw();
}

void PatternFlyout::setRuler(ScrubRuler* r) {
    m_ruler = r;
    if (m_scale != nullptr) m_scale->setRuler(r);
    if (m_weight != nullptr) m_weight->setRuler(r);
    if (m_offset != nullptr) m_offset->setRuler(r);
}

void PatternFlyout::setAntialias(bool aa) {
    m_previewAA = aa;
    if (m_grid != nullptr) m_grid->setAntialias(aa);
}

void PatternFlyout::refreshSliderLabels() {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d\xC2\xB0", static_cast<int>(std::lround(m_pat.angleDeg)));
    if (m_angleValue != nullptr) m_angleValue->copy_label(buf);  // caption stays "Angle"
}

void PatternFlyout::updateKindControls() {
    const bool useSpacing = core::vec::patternUsesSpacing(m_pat.kind);
    const bool useWeight = core::vec::patternUsesWeight(m_pat.kind);
    const bool showWeight = useSpacing || useWeight;  // false for gapless tessellations
    if (m_weight == nullptr || m_weightLabel == nullptr) return;

    // Realign: when the Weight/Distance row hides, pull the rows below it UP so there's no dead gap
    // (and push them back DOWN when it returns). A relative shift preserves any scroll/triangle offset.
    if (showWeight != m_weightShown) {
        const int dy = (kRowH + kRowGap) * (showWeight ? +1 : -1);
        for (Fl_Widget* wgt : {static_cast<Fl_Widget*>(m_angleLabel), static_cast<Fl_Widget*>(m_angle),
                               static_cast<Fl_Widget*>(m_angleValue),
                               static_cast<Fl_Widget*>(m_anchorCheck),
                               static_cast<Fl_Widget*>(m_grid), static_cast<Fl_Widget*>(m_ld2)})
            if (wgt != nullptr) wgt->position(wgt->x(), wgt->y() + dy);
        m_weightShown = showWeight;
        if (m_rightPane != nullptr) m_rightPane->redraw();
    }

    if (showWeight) {
        m_weightLabel->copy_label(useSpacing ? "Distance" : "Weight");
        m_syncing = true;
        m_weight->value((useSpacing ? m_pat.spacing : m_pat.weight) * 100.0);
        m_syncing = false;
        m_weightLabel->show();
        m_weight->show();
    } else {
        m_weightLabel->hide();
        m_weight->hide();
    }
    refreshSliderLabels();
}

void PatternFlyout::setTarget(Target t) {
    m_target = t;
    seedTargetColour();
    redraw();
}

void PatternFlyout::seedTargetColour() {
    const common::ColorF& col = (m_target == Target::Fg) ? m_pat.fg : m_pat.bg;
    const Hsv hsv = rgbToHsv(common::toColor8(col));
    if (hsv.s > 0.0F && hsv.v > 0.0F) m_h = hsv.h;
    m_s = hsv.s;
    m_v = hsv.v;
    m_a = std::clamp(col.a, 0.0F, 1.0F);
    pushToSurfaces();
}

void PatternFlyout::pushToSurfaces() {
    m_syncing = true;
    m_field->set(m_h, m_s, m_v);
    m_hueStrip->set(m_h);
    if (m_alpha != nullptr) m_alpha->value(std::lround(m_a * 100.0F));
    if (!m_editingHex && m_hex != nullptr) {
        const common::Color8 c = hsvToRgb({m_h, m_s, m_v});
        char b[8];
        std::snprintf(b, sizeof(b), "%02X%02X%02X", c.r, c.g, c.b);
        if (std::strcmp(m_hex->value(), b) != 0) m_hex->value(b);
    }
    m_syncing = false;
    redraw();
}

void PatternFlyout::onSurfaceEdited(Fl_Widget* who) {
    if (m_syncing) return;
    if (who == m_field) {
        m_s = m_field->sat();
        m_v = m_field->val();
    } else if (who == m_hueStrip) {
        m_h = m_hueStrip->hue();
    }
    pushToSurfaces();
    writeTargetColour();
}

void PatternFlyout::onHexEdited() {
    if (m_syncing) return;
    const auto c = parseHex(m_hex->value());
    if (!c) return;
    const Hsv hsv = rgbToHsv(*c);
    if (hsv.s > 0.0F && hsv.v > 0.0F) m_h = hsv.h;
    m_s = hsv.s;
    m_v = hsv.v;
    m_editingHex = true;
    pushToSurfaces();
    m_editingHex = false;
    writeTargetColour();
}

void PatternFlyout::writeTargetColour() {
    common::ColorF col = common::toColorF(hsvToRgb({m_h, m_s, m_v}));  // opaque rgb
    col.a = std::clamp(m_a, 0.0F, 1.0F);                              // + alpha
    (m_target == Target::Fg ? m_pat.fg : m_pat.bg) = col;
    if (m_grid != nullptr) m_grid->setPattern(m_pat);  // previews reflect the new colour
    redraw();                                          // swatches + active-target frame
    emitChange();
}

void PatternFlyout::emitChange() {
    if (m_onChange) m_onChange(m_pat);
}

void PatternFlyout::openFor(const Fl_Widget* anchor, const core::vec::ProceduralPattern& initial) {
    m_anchor = anchor;
    m_pat = initial;
    m_target = Target::Fg;
    m_syncing = true;
    if (m_scale != nullptr) m_scale->value(std::clamp<double>(m_pat.scale, 4, 256));
    if (m_angle != nullptr) m_angle->value(std::clamp<double>(m_pat.angleDeg, 0, 360));
    if (m_offset != nullptr) m_offset->value(std::clamp<double>(m_pat.offset * 100.0, 0, 100));
    m_syncing = false;
    if (m_anchorCheck != nullptr) m_anchorCheck->setChecked(m_pat.anchorToCanvas);
    if (m_rightPane != nullptr) static_cast<Fl_Scroll*>(m_rightPane)->scroll_to(0, 0);  // show the top
    updateKindControls();
    if (m_grid != nullptr) m_grid->setPattern(m_pat);
    seedTargetColour();

    placeBubble(anchor);
    show();
    g_active = this;
}

void PatternFlyout::moveContent(int delta) {
    const auto move = [delta](Fl_Widget* wgt) {
        if (wgt != nullptr) wgt->position(wgt->x() + delta, wgt->y());
    };
    move(m_rightPane);  // the scroll carries its children (controls + grid + placeholder)
    move(m_fgLabel);
    move(m_fgSwatch);
    move(m_bgLabel);
    move(m_bgSwatch);
    move(m_useFgSwatch);
    move(m_field);
    move(m_hueStrip);
    move(m_alphaLabel);
    move(m_alpha);
    move(m_hex != nullptr ? m_hex->parent() : nullptr);
}

void PatternFlyout::hide() {
    m_anchor = nullptr;
    if (g_active == this) g_active = nullptr;
    Fl_Double_Window::hide();
}

void PatternFlyout::drawContent() {
    const Palette& p = activePalette();
    const bool full = (damage() & FL_DAMAGE_ALL) != 0;
    if (full) drawBubbleChrome();

    draw_children();

    if (full) {
        const int gx = contentLeft();
        // Active colour target frame (around the fg or bg swatch).
        const int ax = gx + (m_target == Target::Fg ? kFgSwatchDX : kBgSwatchDX);
        fl_color(toFl(p.accent));
        fl_rect(ax - 2, kTargetY - 2, kSwatchW + 4, kTargetH + 4);

        // The active colour's swatch (right of the hex field), over a checker so alpha reads.
        const int sx = gx + kHexInputW + kHexSwatchGap;
        const common::Color8 sc = hsvToRgb({m_h, m_s, m_v});
        drawSwatchChecker(sx, kHexY, kHexSwatchW, kHexH,
                          common::ColorF{sc.r / 255.0f, sc.g / 255.0f, sc.b / 255.0f, m_a});
        fl_color(toFl(p.border));
        fl_rect(sx, kHexY, kHexSwatchW, kHexH);
    }
}

PatternFlyout* activePatternFlyout() { return g_active; }
void dismissActivePatternFlyoutOnOutsideClick(int hostX, int hostY) {
    if (g_active != nullptr && !g_active->spansHostPoint(hostX, hostY)) g_active->hide();
}
void dismissActivePatternFlyout() {
    if (g_active != nullptr) g_active->hide();
}

core::vec::ProceduralPattern defaultProceduralPattern(common::Color8 fg) {
    core::vec::ProceduralPattern p;
    p.kind = K::Dots;
    p.fg = common::toColorF(fg);
    p.bg = common::ColorF{0, 0, 0, 0};  // transparent -- the pattern reads over content
    p.scale = 32.0f;
    p.angleDeg = 0.0f;
    p.weight = 0.5f;
    return p;
}

}  // namespace mosaic::ui
