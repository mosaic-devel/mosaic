#include "ui/color_flyout.hpp"

#include "common/i18n.hpp"
#include "ui/color_models.hpp"   // rgbToHsv / hsvToRgb
#include "ui/color_surfaces.hpp" // SvField, HueStrip, ColorWheel
#include "ui/theme.hpp"
#include "ui/widgets.hpp" // Dropdown, TextInput, SwatchButton

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace mosaic::ui {
namespace {

// Content geometry (the surface combo, SV field + hue strip, hex + current/foreground swatches). The
// bubble geometry (kTri/kTriH/kPad/kContentX) + the triangle/gap/shape/positioning live in BubbleFlyout.
constexpr int kTri = BubbleFlyout::kTri;
constexpr int kPad = BubbleFlyout::kPad;
constexpr int kContentX = BubbleFlyout::kContentX;
constexpr int kBodyW = 190;
constexpr int kWinW = kTri + kBodyW;
constexpr int kContentW = kBodyW - 2 * kPad; // 170
constexpr int kComboY = kPad;
constexpr int kComboH = 26;
constexpr int kSurfaceY = kComboY + kComboH + 6;
constexpr int kSurfaceSide = 150;
constexpr int kStripW = 14;
constexpr int kStripGap = 6;
constexpr int kFieldW = kContentW - kStripW - kStripGap; // 150
constexpr int kHexY = kSurfaceY + kSurfaceSide + 8;
constexpr int kHexH = 26;
constexpr int kHexInputW = 100;
constexpr int kSwatchGap = 6;
constexpr int kSwatchW = 30; // the current colour swatch
constexpr int kFgGap = 4;
constexpr int kFgW = kContentW - kHexInputW - kSwatchGap - kSwatchW - kFgGap; // 30 (foreground swatch)
constexpr int kWinH = kHexY + kHexH + kPad;

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

// The flyout currently shown (at most one across the app -- FillDialog's and the main window's
// never overlap: the dialog is modal).
ColorFlyout* g_activeFlyout = nullptr;

// Parse "#RRGGBB" or "RRGGBB" (case-insensitive, optional leading '#'/whitespace). nullopt on
// anything else; alpha is forced opaque (the flyout is opaque-only, like the picker).
std::optional<common::Color8> parseHex(const char* s) {
    if (s == nullptr)
        return std::nullopt;
    while (*s == ' ' || *s == '\t')
        ++s;
    if (*s == '#')
        ++s;
    char digits[7];
    int n = 0;
    for (; *s != '\0' && n < 6; ++s) {
        if (!std::isxdigit(static_cast<unsigned char>(*s)))
            return std::nullopt;
        digits[n++] = *s;
    }
    if (n != 6 || *s != '\0')
        return std::nullopt;
    digits[6] = '\0';
    const long v = std::strtol(digits, nullptr, 16);
    return common::Color8{static_cast<std::uint8_t>((v >> 16) & 0xFF),
                          static_cast<std::uint8_t>((v >> 8) & 0xFF),
                          static_cast<std::uint8_t>(v & 0xFF), 255};
}

} // namespace

// BubbleFlyout is a borderless child sub-window (built before the host is shown; the DropdownPopup /
// Popover rule) and owns the triangle/gap/shape/positioning.
ColorFlyout::ColorFlyout() : BubbleFlyout(kWinW, kWinH) {
    begin();

    m_surfaceCombo = new Dropdown(kContentX, kComboY, kContentW, kComboH);
    m_surfaceCombo->add(_("Field"));
    m_surfaceCombo->add(_("HSL Wheel"));
    m_surfaceCombo->add(_("SV Wheel"));
    m_surfaceCombo->value(0);
    m_surfaceCombo->callback(
        [](Fl_Widget* w, void* v) {
            static_cast<ColorFlyout*>(v)->selectSurface(
                static_cast<Surface>(static_cast<Dropdown*>(w)->value()));
        },
        this);

    m_field = new SvField(kContentX, kSurfaceY, kFieldW, kSurfaceSide);
    m_field->callback([](Fl_Widget* w, void* v) { static_cast<ColorFlyout*>(v)->onSurfaceEdited(w); },
                      this);
    m_strip = new HueStrip(kContentX + kFieldW + kStripGap, kSurfaceY, kStripW, kSurfaceSide);
    m_strip->callback([](Fl_Widget* w, void* v) { static_cast<ColorFlyout*>(v)->onSurfaceEdited(w); },
                      this);
    // The wheels span the full content width so the ring centres (the SV field + strip together fill
    // that same width); a square wheel widget would otherwise leave an uneven right margin.
    m_wheelTri = new ColorWheel(kContentX, kSurfaceY, kContentW, kSurfaceSide,
                                ColorWheel::Style::Triangle);
    m_wheelTri->callback([](Fl_Widget* w, void* v) { static_cast<ColorFlyout*>(v)->onSurfaceEdited(w); },
                         this);
    m_wheelSq = new ColorWheel(kContentX, kSurfaceY, kContentW, kSurfaceSide,
                               ColorWheel::Style::Square);
    m_wheelSq->callback([](Fl_Widget* w, void* v) { static_cast<ColorFlyout*>(v)->onSurfaceEdited(w); },
                        this);

    auto* hex = new HexField(kContentX, kHexY, kHexInputW, kHexH);
    hex->input()->when(FL_WHEN_CHANGED);
    hex->input()->callback([](Fl_Widget*, void* v) { static_cast<ColorFlyout*>(v)->onHexEdited(); },
                           this);
    m_hex = hex->input();

    // A small foreground swatch after the current-colour swatch: it shows the foreground colour, and a
    // click reuses it -- replacing the full-width "Use foreground" button (its own tiny affordance,
    // matching the gradient flyout). Hidden until setUseForeground provides the getter.
    const int fgX = kContentX + kHexInputW + kSwatchGap + kSwatchW + kFgGap;
    auto* fg = new SwatchButton(fgX, kHexY, kFgW, kHexH);
    fg->setColorGetter([this] { return m_useFg ? m_useFg() : common::Color8{0, 0, 0, 255}; });
    fg->setOnClick([this] {
        if (!m_useFg) return;
        const common::Color8 c = m_useFg();
        const Hsv hsv = rgbToHsv(c);
        if (hsv.s > 0.0F && hsv.v > 0.0F) m_h = hsv.h; // keep the working hue on a grey
        m_s = hsv.s;
        m_v = hsv.v;
        pushToSurfaces();
        emitPick();
    });
    fg->tooltip(_("Use the foreground colour"));
    fg->hide();
    m_fgSwatch = fg;

    end();
    selectSurface(Surface::Field);
}

ColorFlyout::~ColorFlyout() {
    if (g_activeFlyout == this)
        g_activeFlyout = nullptr;
}

void ColorFlyout::setUseForeground(std::function<common::Color8()> get) {
    // Just reveal + drive the small foreground swatch (built in the ctor); no full-width button, so
    // the bubble does not grow.
    m_useFg = std::move(get);
    if (m_fgSwatch == nullptr) return;
    if (m_useFg)
        m_fgSwatch->show();
    else
        m_fgSwatch->hide();
    m_fgSwatch->redraw();
}

void ColorFlyout::selectSurface(Surface s) {
    m_surface = s;
    if (m_surfaceCombo->value() != static_cast<int>(s))
        m_surfaceCombo->value(static_cast<int>(s));
    const bool field = s == Surface::Field;
    if (field) {
        m_field->show();
        m_strip->show();
    } else {
        m_field->hide();
        m_strip->hide();
    }
    (s == Surface::WheelTriangle ? m_wheelTri->show() : m_wheelTri->hide());
    (s == Surface::WheelSquare ? m_wheelSq->show() : m_wheelSq->hide());
    pushToSurfaces();
    redraw();
}

void ColorFlyout::pushToSurfaces() {
    m_field->set(m_h, m_s, m_v);
    m_strip->set(m_h);
    m_wheelTri->set(m_h, m_s, m_v);
    m_wheelSq->set(m_h, m_s, m_v);
    if (!m_editingHex && m_hex != nullptr) {
        const common::Color8 c = currentColor();
        char buf[8]; // no '#': HexField draws the '#' as a fixed prefix glyph
        std::snprintf(buf, sizeof(buf), "%02X%02X%02X", c.r, c.g, c.b);
        if (std::strcmp(m_hex->value(), buf) != 0)
            m_hex->value(buf);
    }
    redraw(); // the swatch is painted in draw()
}

void ColorFlyout::onSurfaceEdited(Fl_Widget* who) {
    if (who == m_field) {
        m_s = m_field->sat();
        m_v = m_field->val();
    } else if (who == m_strip) {
        m_h = m_strip->hue();
    } else if (who == m_wheelTri) {
        m_h = m_wheelTri->hue();
        m_s = m_wheelTri->sat();
        m_v = m_wheelTri->val();
    } else if (who == m_wheelSq) {
        m_h = m_wheelSq->hue();
        m_s = m_wheelSq->sat();
        m_v = m_wheelSq->val();
    }
    pushToSurfaces(); // keep the other surfaces + hex + swatch in step
    emitPick();
}

void ColorFlyout::onHexEdited() {
    const auto c = parseHex(m_hex->value());
    if (!c)
        return; // mid-type / invalid: leave the surfaces alone until it parses
    const Hsv hsv = rgbToHsv(*c);
    // Keep the working hue when the typed colour is a grey (hue is meaningless there, like the picker).
    if (hsv.s > 0.0F && hsv.v > 0.0F)
        m_h = hsv.h;
    m_s = hsv.s;
    m_v = hsv.v;
    m_editingHex = true; // don't rewrite the field text under the user's cursor
    pushToSurfaces();
    m_editingHex = false;
    emitPick();
}

common::Color8 ColorFlyout::currentColor() const { return hsvToRgb({m_h, m_s, m_v}); }

void ColorFlyout::emitPick() {
    if (m_onPick)
        m_onPick(currentColor());
}

void ColorFlyout::openFor(const Fl_Widget* anchor, common::Color8 initial) {
    const Hsv hsv = rgbToHsv(initial);
    m_h = hsv.h;
    m_s = hsv.s;
    m_v = hsv.v;
    pushToSurfaces();
    placeBubble(anchor); // gap + flip + clamp + content shift + shape (shared with GradientFlyout)
    show();
    g_activeFlyout = this;
}

void ColorFlyout::moveContent(int delta) {
    const auto move = [delta](Fl_Widget* wgt) {
        if (wgt != nullptr) wgt->position(wgt->x() + delta, wgt->y());
    };
    move(m_surfaceCombo);
    move(m_field);
    move(m_strip);
    move(m_wheelTri);
    move(m_wheelSq);
    move(m_hex != nullptr ? m_hex->parent() : nullptr); // the HexField group (moves its input)
    move(m_fgSwatch);
}

void ColorFlyout::hide() {
    m_anchor = nullptr;
    if (g_activeFlyout == this)
        g_activeFlyout = nullptr;
    Fl_Double_Window::hide();
}

ColorFlyout* activeColorFlyout() { return g_activeFlyout; }

void dismissActiveColorFlyoutOnOutsideClick(int hostX, int hostY) {
    if (g_activeFlyout != nullptr && !g_activeFlyout->spansHostPoint(hostX, hostY))
        g_activeFlyout->hide();
}

void dismissActiveColorFlyout() {
    if (g_activeFlyout != nullptr)
        g_activeFlyout->hide();
}

void ColorFlyout::drawContent() {
    const Palette& p = activePalette();
    // Repaint the bubble chrome (and the swatch, below) ONLY on a full redraw. On a partial child
    // redraw (FL_DAMAGE_CHILD — a focused/edited hex field), repainting the whole body would overwrite
    // the HexField's own box with panelBg while the field only redraws its inner input, leaving panelBg
    // bleeding through its frame (user-reported). Skipping it keeps the prior chrome in the backbuffer.
    const bool full = (damage() & FL_DAMAGE_ALL) != 0;
    if (full) drawBubbleChrome(); // the plain panel / comic-book triangle (shared with GradientFlyout)

    draw_children();

    // The live swatch (right of the hex field): the chosen colour over a chip with a frame. Only on a
    // full redraw — a partial child redraw leaves the prior swatch in the backbuffer (a colour change
    // calls redraw() on the whole flyout, which is a full redraw).
    if (full) {
        const common::Color8 c = currentColor();
        const int sx = contentLeft() + kHexInputW + kSwatchGap;
        fl_color(toFl(c));
        fl_rectf(sx, kHexY, kSwatchW, kHexH);
        fl_color(toFl(p.border));
        fl_rect(sx, kHexY, kSwatchW, kHexH);
    }
}

} // namespace mosaic::ui
