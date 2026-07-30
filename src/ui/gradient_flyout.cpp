#include "ui/gradient_flyout.hpp"

#include "common/dither.hpp"
#include "common/i18n.hpp"
#include "ui/color_models.hpp"   // rgbToHsv / hsvToRgb
#include "ui/color_surfaces.hpp" // SvField, HueStrip
#include "ui/theme.hpp"
#include "ui/widgets.hpp" // Dropdown, FlatButton, HexField, TextInput

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

namespace mosaic::ui {
namespace {

// Content layout (top to bottom): the ramp strip, the pin handles that point UP at it, a row of
// small stop tools (+ / - / a foreground swatch), the spread combo, the SV field + hue strip, the
// alpha slider, and the hex + selected-stop swatch. The bubble geometry (kTri/kTriH/kPad/kContentX)
// + the triangle/gap/shape/positioning all live in BubbleFlyout now.
constexpr int kTri = BubbleFlyout::kTri;
constexpr int kPad = BubbleFlyout::kPad;
constexpr int kContentX = BubbleFlyout::kContentX;
constexpr int kBodyW = 210;
constexpr int kWinW = kTri + kBodyW;
constexpr int kContentW = kBodyW - 2 * kPad; // 190

constexpr int kStripY = kPad;
constexpr int kStripH = 24;
constexpr int kPtrY = kStripY + kStripH;        // handle triangle tip (touching the strip's bottom)
constexpr int kPtrH = 6;                        // triangle height
constexpr int kPtrBaseY = kPtrY + kPtrH;        // triangle base line
constexpr int kHandleGap = 3;                   // gap between the pointer and the colour body
constexpr int kHBodyY = kPtrBaseY + kHandleGap; // handle body top
constexpr int kHBodyH = 12;                     // handle body height
constexpr int kHandleHalf = 6;                  // half-width of a handle body / triangle base
constexpr int kHandleW = 2 * kHandleHalf + 1; // odd width so body + triangle centre on hx, aligned
constexpr int kToolY = kHBodyY + kHBodyH + 8; // the + / - / foreground row
constexpr int kToolH = 22;
constexpr int kToolBtnW = 28;
constexpr int kSpreadY = kToolY + kToolH + 8;
constexpr int kSpreadH = 26;
constexpr int kSurfaceY = kSpreadY + kSpreadH + 8;
constexpr int kSurfaceSide = 140;
constexpr int kHueW = 14;
constexpr int kHueGap = 6;
constexpr int kFieldW = kContentW - kHueW - kHueGap; // 170
constexpr int kAlphaY = kSurfaceY + kSurfaceSide + 8;
constexpr int kAlphaH = 22;
constexpr int kAlphaLabelW = 44;
constexpr int kAlphaSlideX = kContentX + kAlphaLabelW + 6;
constexpr int kAlphaSlideW = kContentW - kAlphaLabelW - 6;
constexpr int kHexY = kAlphaY + kAlphaH + 8;
constexpr int kHexH = 26;
constexpr int kHexInputW = 108;
constexpr int kSwatchGap = 6;
constexpr int kSwatchW = kContentW - kHexInputW - kSwatchGap; // 76
constexpr int kWinH = kHexY + kHexH + kPad;

constexpr int kCheck = 6; // checkerboard cell (transparency)

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// Paint `c` (its alpha honoured) over the transparency checkerboard in the rect -- so any swatch
// that can be semi-transparent shows it, like the main colour picker.
void drawSwatchChecker(int rx, int ry, int rw, int rh, common::ColorF c) {
    const float a = std::clamp(c.a, 0.0f, 1.0f);
    for (int yy = 0; yy < rh; ++yy) {
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
}

GradientFlyout* g_active = nullptr;

// Rotate about the normalised content-box centre (0.5,0.5) -- the pivot every gradient type shares.
common::Affine2D rotateAboutCenter(double radians) {
    using A = common::Affine2D;
    return A::translation(0.5, 0.5) * A::rotation(radians) * A::translation(-0.5, -0.5);
}

// Colour of the stop ramp at parameter t in [0,1] (pad-clamped lerp; the strip shows the raw stops,
// spread/type are applied only on the canvas). Stops are kept sorted by offset.
common::ColorF rampAt(const std::vector<core::vec::GradientStop>& stops, double t) {
    if (stops.empty())
        return {0, 0, 0, 0};
    if (t <= stops.front().offset)
        return stops.front().color;
    if (t >= stops.back().offset)
        return stops.back().color;
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].offset) {
            const double span = stops[i].offset - stops[i - 1].offset;
            const double f = span > 1e-9 ? (t - stops[i - 1].offset) / span : 0.0;
            const common::ColorF& a = stops[i - 1].color;
            const common::ColorF& b = stops[i].color;
            const float ft = static_cast<float>(f);
            return {a.r + (b.r - a.r) * ft, a.g + (b.g - a.g) * ft, a.b + (b.b - a.b) * ft,
                    a.a + (b.a - a.a) * ft};
        }
    }
    return stops.back().color;
}

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

GradientFlyout::GradientFlyout() : BubbleFlyout(kWinW, kWinH) {
    begin(); // BubbleFlyout set border(0) + the ground + the shape backend gate

    // Stop tools: small + / - (add / remove a stop) on the left, a foreground swatch on the right.
    m_addBtn = new FlatButton(kContentX, kToolY, kToolBtnW, kToolH, "+");
    m_addBtn->tooltip(_("Add a gradient stop"));
    m_addBtn->callback([](Fl_Widget*, void* v) { static_cast<GradientFlyout*>(v)->addStopInGap(); },
                       this);
    m_removeBtn = new FlatButton(kContentX + kToolBtnW + 4, kToolY, kToolBtnW, kToolH, "-");
    m_removeBtn->tooltip(_("Remove the selected gradient stop"));
    m_removeBtn->callback(
        [](Fl_Widget*, void* v) { static_cast<GradientFlyout*>(v)->removeSelected(); }, this);
    auto* fg = new SwatchButton(kContentX + kContentW - kToolBtnW, kToolY, kToolBtnW, kToolH);
    fg->setColorGetter([this] { return m_useFg ? m_useFg() : common::Color8{0, 0, 0, 255}; });
    fg->setOnClick([this] {
        if (!m_useFg)
            return;
        const common::Color8 c = m_useFg();
        const Hsv hsv = rgbToHsv(c);
        if (hsv.s > 0.0F && hsv.v > 0.0F)
            m_h = hsv.h;
        m_s = hsv.s;
        m_v = hsv.v;
        m_a = c.a / 255.0F;
        pushToSurfaces();
        writeSelectedColour();
    });
    fg->tooltip(_("Set the selected stop to the foreground colour"));
    m_fgSwatch = fg;

    m_spread = new Dropdown(kContentX, kSpreadY, kContentW, kSpreadH);
    m_spread->add(_("Pad"));
    m_spread->add(_("Repeat"));
    m_spread->add(_("Reflect"));
    m_spread->value(0);
    m_spread->callback(
        [](Fl_Widget* w, void* v) {
            auto* self = static_cast<GradientFlyout*>(v);
            self->m_grad.spread = static_cast<core::vec::SpreadMethod>(
                std::clamp(static_cast<Dropdown*>(w)->value(), 0, 2));
            self->emitChange();
        },
        this);

    m_field = new SvField(kContentX, kSurfaceY, kFieldW, kSurfaceSide);
    m_field->callback(
        [](Fl_Widget* w, void* v) { static_cast<GradientFlyout*>(v)->onSurfaceEdited(w); }, this);
    m_hueStrip = new HueStrip(kContentX + kFieldW + kHueGap, kSurfaceY, kHueW, kSurfaceSide);
    m_hueStrip->callback(
        [](Fl_Widget* w, void* v) { static_cast<GradientFlyout*>(v)->onSurfaceEdited(w); }, this);

    // Alpha of the selected stop -- the ONLY way to make a stop (semi-)transparent (the surfaces +
    // hex are opaque). A plain flat Slider (no ruler needed, unlike ScrubSlider).
    auto* al = new Fl_Box(kContentX, kAlphaY, kAlphaLabelW, kAlphaH, _("Alpha"));
    al->box(FL_NO_BOX);
    al->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    al->labelfont(FL_HELVETICA);
    al->labelsize(12);
    al->labelcolor(toFl(activePalette().textMuted));
    m_alphaLabel = al;
    m_alpha = new Slider(kAlphaSlideX, kAlphaY, kAlphaSlideW, kAlphaH);
    m_alpha->range(0, 100);
    m_alpha->step(1);
    m_alpha->setCellColor(activePalette().panelBg);
    m_alpha->when(FL_WHEN_CHANGED);
    m_alpha->callback(
        [](Fl_Widget* w, void* v) {
            auto* self = static_cast<GradientFlyout*>(v);
            if (self->m_syncing)
                return;
            self->m_a = static_cast<float>(static_cast<Slider*>(w)->value() / 100.0);
            self->writeSelectedColour();
        },
        this);

    auto* hex = new HexField(kContentX, kHexY, kHexInputW, kHexH);
    hex->input()->when(FL_WHEN_CHANGED);
    hex->input()->callback(
        [](Fl_Widget*, void* v) { static_cast<GradientFlyout*>(v)->onHexEdited(); }, this);
    m_hex = hex->input();

    end();
}

GradientFlyout::~GradientFlyout() {
    if (g_active == this)
        g_active = nullptr;
}

void GradientFlyout::setUseForeground(std::function<common::Color8()> get) {
    // Stores the getter the foreground swatch (built in the ctor) reads + applies on click; no
    // button.
    m_useFg = std::move(get);
    if (m_fgSwatch != nullptr)
        m_fgSwatch->redraw();
}

int GradientFlyout::handleXFor(double offset) const {
    return contentLeft() +
           static_cast<int>(std::lround(std::clamp(offset, 0.0, 1.0) * (kContentW - 1)));
}

int GradientFlyout::hitHandle(int localX) const {
    int best = -1;
    int bestDist = kHandleHalf + 3;
    for (std::size_t i = 0; i < m_grad.stops.size(); ++i) {
        const int d = std::abs(localX - handleXFor(m_grad.stops[i].offset));
        if (d < bestDist) {
            bestDist = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void GradientFlyout::seedSelectionColour() {
    if (m_grad.stops.empty())
        return;
    m_selected = std::clamp(m_selected, 0, static_cast<int>(m_grad.stops.size()) - 1);
    const common::ColorF& col = m_grad.stops[static_cast<std::size_t>(m_selected)].color;
    const Hsv hsv = rgbToHsv(common::toColor8(col));
    if (hsv.s > 0.0F && hsv.v > 0.0F)
        m_h = hsv.h;
    m_s = hsv.s;
    m_v = hsv.v;
    m_a = std::clamp(col.a, 0.0F, 1.0F);
    pushToSurfaces();
}

void GradientFlyout::pushToSurfaces() {
    m_syncing = true;
    m_field->set(m_h, m_s, m_v);
    m_hueStrip->set(m_h);
    if (m_alpha != nullptr)
        m_alpha->value(std::lround(m_a * 100.0F));
    if (!m_editingHex && m_hex != nullptr) {
        const common::Color8 c = hsvToRgb({m_h, m_s, m_v});
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02X%02X%02X", c.r, c.g, c.b);
        if (std::strcmp(m_hex->value(), buf) != 0)
            m_hex->value(buf);
    }
    m_syncing = false;
    redraw(); // the strip + handles + swatch are painted in draw()
}

void GradientFlyout::onSurfaceEdited(Fl_Widget* who) {
    if (m_syncing)
        return;
    if (who == m_field) {
        m_s = m_field->sat();
        m_v = m_field->val();
    } else if (who == m_hueStrip) {
        m_h = m_hueStrip->hue();
    }
    pushToSurfaces();
    writeSelectedColour();
}

void GradientFlyout::onHexEdited() {
    if (m_syncing)
        return;
    const auto c = parseHex(m_hex->value());
    if (!c)
        return;
    const Hsv hsv = rgbToHsv(*c);
    if (hsv.s > 0.0F && hsv.v > 0.0F)
        m_h = hsv.h;
    m_s = hsv.s;
    m_v = hsv.v;
    m_editingHex = true;
    pushToSurfaces();
    m_editingHex = false;
    writeSelectedColour();
}

void GradientFlyout::writeSelectedColour() {
    if (m_grad.stops.empty())
        return;
    m_selected = std::clamp(m_selected, 0, static_cast<int>(m_grad.stops.size()) - 1);
    common::ColorF col = common::toColorF(hsvToRgb({m_h, m_s, m_v})); // opaque rgb
    col.a = std::clamp(m_a, 0.0F, 1.0F);                              // + the stop's alpha
    m_grad.stops[static_cast<std::size_t>(m_selected)].color = col;
    redraw();
    emitChange();
}

void GradientFlyout::emitChange() {
    if (m_onChange)
        m_onChange(m_grad);
}

void GradientFlyout::addStopAt(double offset) {
    offset = std::clamp(offset, 0.0, 1.0);
    core::vec::GradientStop s{offset, rampAt(m_grad.stops, offset)};
    // Insert keeping the vector sorted by offset; select the new stop.
    auto it =
        std::upper_bound(m_grad.stops.begin(), m_grad.stops.end(), offset,
                         [](double o, const core::vec::GradientStop& st) { return o < st.offset; });
    it = m_grad.stops.insert(it, s);
    m_selected = static_cast<int>(it - m_grad.stops.begin());
    seedSelectionColour();
    redraw();
    emitChange();
}

void GradientFlyout::addStopInGap() {
    if (m_grad.stops.size() < 2) {
        addStopAt(0.5);
        return;
    }
    // Split the widest gap between consecutive stops (a predictable "+" that stays visible).
    std::size_t gap = 0;
    double widest = -1.0;
    for (std::size_t i = 1; i < m_grad.stops.size(); ++i) {
        const double g = m_grad.stops[i].offset - m_grad.stops[i - 1].offset;
        if (g > widest) {
            widest = g;
            gap = i;
        }
    }
    addStopAt(0.5 * (m_grad.stops[gap - 1].offset + m_grad.stops[gap].offset));
}

void GradientFlyout::removeSelected() {
    if (m_grad.stops.size() <= 2)
        return; // a gradient needs at least two stops
    m_selected = std::clamp(m_selected, 0, static_cast<int>(m_grad.stops.size()) - 1);
    m_grad.stops.erase(m_grad.stops.begin() + m_selected);
    m_selected = std::min(m_selected, static_cast<int>(m_grad.stops.size()) - 1);
    seedSelectionColour();
    redraw();
    emitChange();
}

void GradientFlyout::openFor(const Fl_Widget* anchor, const core::vec::Gradient& initial) {
    m_anchor = anchor;
    m_grad = initial;
    if (m_grad.stops.size() < 2)
        m_grad.stops = {{0.0, {0, 0, 0, 1}}, {1.0, {1, 1, 1, 1}}};
    std::sort(m_grad.stops.begin(), m_grad.stops.end(),
              [](const core::vec::GradientStop& a, const core::vec::GradientStop& b) {
                  return a.offset < b.offset;
              });
    m_selected = 0;
    m_spread->value(std::clamp(static_cast<int>(m_grad.spread), 0, 2));
    seedSelectionColour();

    placeBubble(anchor); // gap + flip + clamp + content shift + shape (shared with ColorFlyout)
    show();
    g_active = this;
}

void GradientFlyout::moveContent(int delta) {
    const auto move = [delta](Fl_Widget* wgt) {
        if (wgt != nullptr)
            wgt->position(wgt->x() + delta, wgt->y());
    };
    move(m_addBtn);
    move(m_removeBtn);
    move(m_fgSwatch);
    move(m_spread);
    move(m_field);
    move(m_hueStrip);
    move(m_alphaLabel);
    move(m_alpha);
    move(m_hex != nullptr ? m_hex->parent() : nullptr);
}

void GradientFlyout::hide() {
    m_anchor = nullptr;
    if (g_active == this)
        g_active = nullptr;
    Fl_Double_Window::hide();
}

GradientFlyout* activeGradientFlyout() {
    return g_active;
}
void dismissActiveGradientFlyoutOnOutsideClick(int hostX, int hostY) {
    if (g_active != nullptr && !g_active->spansHostPoint(hostX, hostY))
        g_active->hide();
}
void dismissActiveGradientFlyout() {
    if (g_active != nullptr)
        g_active->hide();
}

void GradientFlyout::drawContent() {
    const Palette& p = activePalette();
    const bool full = (damage() & FL_DAMAGE_ALL) != 0;
    if (full)
        drawBubbleChrome(); // the plain panel / comic-book triangle (shared with ColorFlyout)

    draw_children();

    if (full) {
        const int sl = contentLeft();
        // The stop ramp over a checkerboard (so stop alpha reads), built into an RGB buffer +
        // blitted.
        std::vector<unsigned char> buf(static_cast<std::size_t>(kContentW) * kStripH * 3);
        for (int yy = 0; yy < kStripH; ++yy) {
            for (int xx = 0; xx < kContentW; ++xx) {
                const double t = kContentW > 1 ? double(xx) / (kContentW - 1) : 0.0;
                const common::ColorF c = rampAt(m_grad.stops, t);
                const bool dark = ((xx / kCheck) + (yy / kCheck)) & 1;
                const float bg = dark ? 205.0f : 255.0f;
                const float a = std::clamp(c.a, 0.0f, 1.0f);
                // ~1 LSB of TPDF dither at the 8-bit quantisation: a shallow two-stop ramp
                // otherwise bands into visible steps across the strip (the sky renderer's fix,
                // user-requested here 2026-07-16).
                const auto over = [&](float ch, int chIdx) {
                    return static_cast<unsigned char>(std::lround(std::clamp(
                        std::clamp(ch, 0.0f, 1.0f) * 255.0f * a + bg * (1.0f - a) +
                            static_cast<float>(common::ditherTPDF(
                                static_cast<std::uint32_t>(xx), static_cast<std::uint32_t>(yy),
                                chIdx)),
                        0.0f, 255.0f)));
                };
                const std::size_t o = (static_cast<std::size_t>(yy) * kContentW + xx) * 3;
                buf[o] = over(c.r, 0);
                buf[o + 1] = over(c.g, 1);
                buf[o + 2] = over(c.b, 2);
            }
        }
        Fl_RGB_Image ramp(buf.data(), kContentW, kStripH, 3);
        ramp.draw(sl, kStripY);
        fl_color(toFl(p.border));
        fl_rect(sl, kStripY, kContentW, kStripH);

        // Pin handles: a triangle pointing UP at the strip + a colour body below it (over a checker
        // so a transparent stop reads). Accent frame = the selected stop; it draws last, on top.
        const auto drawHandle = [&](std::size_t i) {
            const int hx = handleXFor(m_grad.stops[i].offset);
            const bool sel = static_cast<int>(i) == m_selected;
            const common::ColorF col = m_grad.stops[i].color;
            // Pointer triangle (tip on the strip, base above the gap): a plain filled pointer, NO
            // outline -- an outlined triangle over the outlined body read as a "house". Filled
            // OPAQUE so it reads regardless of the stop's alpha. Base spans [hx-half, hx+half] ==
            // body width.
            fl_color(toFl(common::toColor8({col.r, col.g, col.b, 1.0f})));
            fl_begin_polygon();
            fl_vertex(hx, kPtrY);
            fl_vertex(hx - kHandleHalf, kPtrBaseY);
            fl_vertex(hx + kHandleHalf, kPtrBaseY);
            fl_end_polygon();
            // Colour body (over a checker for alpha), centred on hx; its frame carries the
            // selection.
            drawSwatchChecker(hx - kHandleHalf, kHBodyY, kHandleW, kHBodyH, col);
            fl_color(toFl(sel ? p.accent : p.border));
            fl_rect(hx - kHandleHalf, kHBodyY, kHandleW, kHBodyH);
        };
        for (std::size_t i = 0; i < m_grad.stops.size(); ++i)
            if (static_cast<int>(i) != m_selected)
                drawHandle(i);
        if (m_selected >= 0 && m_selected < static_cast<int>(m_grad.stops.size()))
            drawHandle(static_cast<std::size_t>(m_selected));

        // The selected stop's swatch (right of the hex field), over a checker so its alpha reads.
        const int sx = contentLeft() + kHexInputW + kSwatchGap;
        const common::Color8 sc = hsvToRgb({m_h, m_s, m_v});
        drawSwatchChecker(sx, kHexY, kSwatchW, kHexH,
                          common::ColorF{sc.r / 255.0f, sc.g / 255.0f, sc.b / 255.0f, m_a});
        fl_color(toFl(p.border));
        fl_rect(sx, kHexY, kSwatchW, kHexH);
    }
}

int GradientFlyout::handle(int event) {
    // Stop strip + pin handles: our own hit-testing, BEFORE the base routes to children.
    // Coordinates are window-local here (a sub-window's handle() gets event_x/y relative to itself
    // -- the DropdownPopup convention; subtracting x()/y() was the "clicks pass through" bug).
    const int lx = Fl::event_x();
    const int ly = Fl::event_y();
    const bool inBand = ly >= kStripY && ly < kHBodyY + kHBodyH &&
                        lx >= contentLeft() - kHandleHalf &&
                        lx <= contentLeft() + kContentW + kHandleHalf;
    switch (event) {
    case FL_PUSH:
        if (inBand) {
            const int hit = hitHandle(lx);
            if (hit >=
                0) { // select + start dragging that stop (clicks elsewhere in the band do nothing)
                m_selected = hit;
                m_dragStop = hit;
                seedSelectionColour();
                redraw();
                return 1;
            }
        }
        break;
    case FL_DRAG:
        if (m_dragStop >= 0 && m_dragStop < static_cast<int>(m_grad.stops.size())) {
            double t =
                std::clamp(double(lx - contentLeft()) / std::max(1, kContentW - 1), 0.0, 1.0);
            m_grad.stops[static_cast<std::size_t>(m_dragStop)].offset = t;
            // Keep sorted, tracking the dragged index.
            int i = m_dragStop;
            while (i > 0 && m_grad.stops[i].offset < m_grad.stops[i - 1].offset) {
                std::swap(m_grad.stops[i], m_grad.stops[i - 1]);
                --i;
            }
            while (i + 1 < static_cast<int>(m_grad.stops.size()) &&
                   m_grad.stops[i].offset > m_grad.stops[i + 1].offset) {
                std::swap(m_grad.stops[i], m_grad.stops[i + 1]);
                ++i;
            }
            m_dragStop = i;
            m_selected = i;
            redraw();
            emitChange();
            return 1;
        }
        break;
    case FL_RELEASE:
        if (m_dragStop >= 0) {
            m_dragStop = -1;
            return 1;
        }
        break;
    default:
        break;
    }
    return BubbleFlyout::handle(event); // Esc + default routing
}

common::Affine2D defaultGradientTransform(core::vec::GradientType type) {
    using common::Affine2D;
    switch (type) {
    case core::vec::GradientType::Linear:
        return Affine2D::identity(); // unit x 0->1 across the normalised content box
    case core::vec::GradientType::Radial:
        return Affine2D::trs({0.5, 0.5}, 0.0, {0.5, 0.5}); // centred, radius to the edge
    case core::vec::GradientType::Conic:
        return Affine2D::translation(0.5, 0.5); // centred
    }
    return Affine2D::identity();
}

core::vec::Gradient defaultGradient(core::vec::GradientType type, common::Color8 a,
                                    common::Color8 b) {
    core::vec::Gradient g;
    g.type = type;
    g.stops = {{0.0, common::toColorF(a)}, {1.0, common::toColorF(b)}};
    g.spread = core::vec::SpreadMethod::Pad;
    g.transform = defaultGradientTransform(type);
    return g;
}

common::Affine2D directedGradientTransform(core::vec::GradientType type, double deg) {
    return rotateAboutCenter(deg * M_PI / 180.0) * defaultGradientTransform(type);
}

double gradientDirectionDeg(const core::vec::Gradient& g) {
    const std::optional<common::Affine2D> dInv = defaultGradientTransform(g.type).inverse();
    if (!dInv)
        return 0.0;
    const common::Affine2D m = g.transform * *dInv; // the rotation left of the type default
    double deg = std::atan2(m.m10, m.m00) * 180.0 / M_PI;
    if (deg < 0.0)
        deg += 360.0;
    return deg;
}

} // namespace mosaic::ui
