#include "ui/widgets.hpp"

#include "common/charconv_compat.hpp"
#include "common/i18n.hpp"
#include "core/blend_mode.hpp"
#include "platform/native_window.hpp" // raiseNativeWindowToTop: overlay z-order on Windows
#include "ui/theme.hpp"

#include <FL/Enumerations.H> // fl_rgb_color
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mosaic::ui {
namespace {
Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}
// Linear blend a->b by t in [0,1] (opaque result). Used to derive a grey that contrasts with a
// near-white surface by stepping toward the text colour.
common::Color8 mix(common::Color8 a, common::Color8 b, float t) {
    const auto ch = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(x + (y - x) * t));
    };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), 255};
}
// Uniform channel lift (clamped). The FilledButton hover derives from the accent this way instead
// of a second palette token (the Fill/Texture/Export dialogs' local FilledButtons set the pattern).
common::Color8 lighten(common::Color8 c, int d) {
    const auto up = [d](std::uint8_t v) {
        return static_cast<std::uint8_t>(std::clamp(static_cast<int>(v) + d, 0, 255));
    };
    return {up(c.r), up(c.g), up(c.b), c.a};
}
} // namespace

// ---- Anti-aliased arcs, rings and discs (see the header for why these exist at all) ------------
namespace {
// Signed distance from the offset (dx, dy) to the rx/ry ellipse about the origin, positive OUTSIDE.
// EXACT for a circle (it collapses to hypot - r); for an ellipse it is the gradient-normalised
// first-order estimate f/|grad f| of the implicit form hypot(dx/rx, dy/ry) - 1. That estimate is
// accurate where |f| is small, i.e. exactly along the curve where a THIN ring or a fill edge lives
// -- the error only grows far from it, where the coverage ramp has already saturated to 0 or 1.
double ellipseDistance(double dx, double dy, double rx, double ry) {
    if (rx <= 0.0 || ry <= 0.0)
        return 1.0e9; // degenerate: no coverage anywhere
    const double m = std::hypot(dx / rx, dy / ry);
    const double gm = std::hypot(dx / (rx * rx), dy / (ry * ry));
    if (gm < 1e-12)
        return -std::min(rx, ry); // dead centre: deepest point inside
    return (m - 1.0) * m / gm;
}

// Distance from (dx, dy) to the nearer straight edge of the wedge [a0, a1], positive INSIDE it.
// fl_arc measures degrees counter-clockwise from 3 o'clock on a y-DOWN surface, hence atan2(-dy,dx).
// The perpendicular distance to an edge through the centre is r*sin(angle past it); the argument is
// clamped to +-90 deg so a point deep inside the wedge cannot fold back across the far edge.
double wedgeDistance(double dx, double dy, double a0, double a1) {
    constexpr double kDeg = std::numbers::pi / 180.0;
    const double deg = std::atan2(-dy, dx) / kDeg;
    const double mid = (a0 + a1) * 0.5;
    double delta = std::fmod(deg - mid, 360.0);
    if (delta > 180.0)
        delta -= 360.0;
    else if (delta < -180.0)
        delta += 360.0;
    const double past = ((a1 - a0) * 0.5 - std::abs(delta)) * kDeg;
    return std::hypot(dx, dy) *
           std::sin(std::clamp(past, -std::numbers::pi / 2.0, std::numbers::pi / 2.0));
}

// The pixel rect where `arcs` can lay down any coverage at all. Wedge angles are ignored on purpose
// -- a conservative box costs a handful of blank pixels, whereas a tight one that got the wedge
// wrong would clip a ramp. Returns false when there is nothing to draw.
bool arcsBounds(const std::vector<AAArc>& arcs, int& ox, int& oy, int& pw, int& ph) {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    bool any = false;
    for (const AAArc& a : arcs) {
        if (a.rx <= 0.0 || a.ry <= 0.0)
            continue;
        // Outer reach: the centreline, half the stroke, and the half pixel the linear ramp adds.
        const double half = a.stroke > 0.0 ? a.stroke * 0.5 : 0.0;
        const double ex = a.rx + half + 0.5;
        const double ey = a.ry + half + 0.5;
        if (!any) {
            x0 = a.cx - ex;
            x1 = a.cx + ex;
            y0 = a.cy - ey;
            y1 = a.cy + ey;
            any = true;
            continue;
        }
        x0 = std::min(x0, a.cx - ex);
        x1 = std::max(x1, a.cx + ex);
        y0 = std::min(y0, a.cy - ey);
        y1 = std::max(y1, a.cy + ey);
    }
    if (!any)
        return false;
    // x0/x1 bracket the SAMPLE coordinates that can carry coverage, and pixel i is sampled at
    // i + 0.5, so the patch is the pixels whose centre falls strictly inside (x0, x1). Rounding it
    // this tightly matters: the sloppier floor(x0)..ceil(x1) box would blit a blank column one pixel
    // past a rim-deflected puck, i.e. one pixel outside the widget that owns the draw.
    ox = static_cast<int>(std::floor(x0 + 0.5));
    oy = static_cast<int>(std::floor(y0 + 0.5));
    pw = static_cast<int>(std::ceil(x1 - 0.5)) - ox;
    ph = static_cast<int>(std::ceil(y1 - 0.5)) - oy;
    return pw > 0 && ph > 0;
}
} // namespace

// fl_arc's box is the bounding box of the COMPLETE ellipse, and FLTK's own Cairo driver puts the
// centre at (x + w/2 - 0.5, y + h/2 - 0.5) in FLTK coordinates with semi-axes (w-1)/2 and (h-1)/2.
// FLTK integer coordinates address pixel CENTRES, so +0.5 lands the centre in this rasterizer's
// corner-origin space. (The GDI and Xlib drivers each round that differently by up to a pixel;
// Cairo is the reference because Cairo is what the marks were visually tuned against.)
AAArc aaArcFromBox(int x, int y, int w, int h, double a0, double a1, double stroke,
                   common::Color8 c) {
    return {x + w / 2.0, y + h / 2.0, (w - 1) / 2.0, (h - 1) / 2.0, stroke, c, a0, a1};
}

AAArc aaPieFromBox(int x, int y, int w, int h, double a0, double a1, common::Color8 c) {
    return {x + w / 2.0, y + h / 2.0, w / 2.0, h / 2.0, 0.0, c, a0, a1};
}

AAArc aaCircle(double x, double y, double r, double stroke, common::Color8 c) {
    return {x + 0.5, y + 0.5, r, r, stroke, c, 0.0, 360.0};
}

double aaArcCoverage(const AAArc& a, double sx, double sy) {
    const double dx = sx - a.cx;
    const double dy = sy - a.cy;
    const double d = ellipseDistance(dx, dy, a.rx, a.ry);
    // A fill ramps across its ONE edge; a stroke ramps across both edges of the ring band.
    double cov = a.stroke <= 0.0 ? std::clamp(0.5 - d, 0.0, 1.0)
                                 : std::clamp(a.stroke * 0.5 + 0.5 - std::abs(d), 0.0, 1.0);
    if (cov <= 0.0)
        return 0.0;
    const double sweep = a.a1 - a.a0;
    if (sweep <= 0.0)
        return 0.0; // fl_pie draws nothing for an empty sweep; fl_arc's single point is not a mark
    if (sweep < 360.0)
        cov *= std::clamp(0.5 + wedgeDistance(dx, dy, a.a0, a.a1), 0.0, 1.0);
    return cov;
}

void drawAAArcs(int originX, int originY, int w, int h,
                const std::function<common::Color8(int x, int y)>& under,
                const std::vector<AAArc>& arcs) {
    if (w <= 0 || h <= 0 || arcs.empty())
        return;
    std::vector<unsigned char> buf(static_cast<std::size_t>(w) * h * 3);
    std::size_t o = 0;
    for (int py = 0; py < h; ++py) {
        const double sy = originY + py + 0.5; // coverage is measured at pixel centres
        for (int px = 0; px < w; ++px) {
            const common::Color8 u = under(originX + px, originY + py);
            double r = u.r;
            double g = u.g;
            double b = u.b;
            const double sx = originX + px + 0.5;
            for (const AAArc& a : arcs) {
                const double cov = aaArcCoverage(a, sx, sy);
                if (cov <= 0.0)
                    continue;
                r += (a.color.r - r) * cov;
                g += (a.color.g - g) * cov;
                b += (a.color.b - b) * cov;
            }
            buf[o++] = static_cast<unsigned char>(std::lround(r));
            buf[o++] = static_cast<unsigned char>(std::lround(g));
            buf[o++] = static_cast<unsigned char>(std::lround(b));
        }
    }
    fl_draw_image(buf.data(), originX, originY, w, h, 3, 0);
}

void drawAAArcs(common::Color8 ground, const std::vector<AAArc>& arcs) {
    drawAAArcs([ground](int, int) { return ground; }, arcs);
}

void drawAAArcs(const std::function<common::Color8(int x, int y)>& under,
                const std::vector<AAArc>& arcs) {
    int ox = 0;
    int oy = 0;
    int pw = 0;
    int ph = 0;
    if (!arcsBounds(arcs, ox, oy, pw, ph))
        return;
    drawAAArcs(ox, oy, pw, ph, under, arcs);
}

Panel::Panel(int X, int Y, int W, int H, const char* label) : Fl_Group(X, Y, W, H, label) {
    const Palette& p = activePalette();
    box(MOSAIC_FLAT_BOX); // fill only; the hairline edges are drawn per m_edges (see header)
    color(toFl(p.panelBg));
    labelcolor(toFl(p.text));
}

void Panel::reapplyTheme() {
    const Palette& p = activePalette();
    color(toFl(p.panelBg));
    labelcolor(toFl(p.text));
    redraw();
}

void Panel::draw() {
    Fl_Group::draw();
    if (m_edges == EdgesNone)
        return;
    fl_color(toFl(activePalette().border));
    if ((m_edges & EdgeTop) != 0)
        fl_xyline(x(), y(), x() + w() - 1);
    if ((m_edges & EdgeBottom) != 0)
        fl_xyline(x(), y() + h() - 1, x() + w() - 1);
    if ((m_edges & EdgeLeft) != 0)
        fl_yxline(x(), y(), y() + h() - 1);
    if ((m_edges & EdgeRight) != 0)
        fl_yxline(x() + w() - 1, y(), y() + h() - 1);
}

FlatButton::FlatButton(int X, int Y, int W, int H, const char* label)
    : Fl_Button(X, Y, W, H, label) {
    box(MOSAIC_BUTTON_UP_BOX);
    down_box(MOSAIC_BUTTON_DOWN_BOX);
    // Semantic FLTK colours, NOT baked palette RGB: applyTheme() updates the global colour map, so
    // FL_BACKGROUND2_COLOR (= palette controlBg) and FL_FOREGROUND_COLOR (= text) follow a runtime
    // re-theme for free -- otherwise the rest fill stays stale until a hover and the label stays the
    // old text colour (white-on-white after dark->light). See the fltk-draw / re-theme notes.
    color(FL_BACKGROUND2_COLOR);
    selection_color(toFl(activePalette().controlActive));
    labelcolor(FL_FOREGROUND_COLOR);
    clear_visible_focus(); // pro look: no dotted focus rectangle
    m_themeSub = ThemeSubscription([this] { reapplyTheme(); });
}

void FlatButton::reapplyTheme() {
    color(FL_BACKGROUND2_COLOR); // un-freeze any concrete hover colour back to the semantic rest fill
    // The PRESSED / toggled-down fill. Fl_Button::draw() paints the down box with selection_color(),
    // and controlActive has no semantic FLTK slot to ride (FL_SELECTION_COLOR is the accent), so the
    // constructor bakes a concrete RGB -- which then survives a re-theme. That is the "buttons keep
    // their pressed state from the old theme" staleness: every toggle that sits down (B/I/U/S, an
    // open Style.../3D... button, a tool's option toggles) kept the previous palette's slab while
    // everything around it switched. Re-read it here, in the BASE, so the fix reaches every subclass.
    selection_color(toFl(activePalette().controlActive));
    redraw();
}

int FlatButton::handle(int event) {
    switch (event) {
    case FL_ENTER:
        color(toFl(activePalette().controlHover)); // no semantic for hover; read it live
        redraw();
        return 1; // claim ENTER so FLTK also delivers LEAVE
    case FL_LEAVE:
        color(FL_BACKGROUND2_COLOR); // back to the semantic rest colour (keeps following re-themes)
        redraw();
        return 1;
    default:
        return Fl_Button::handle(event);
    }
}

// ---- FilledButton: the accent-filled primary action (see widgets.hpp) ---------------------------
FilledButton::FilledButton(int X, int Y, int W, int H, const char* label)
    : FlatButton(X, Y, W, H, label) {
    applyFill(false);
}

void FilledButton::applyFill(bool hover) {
    const Palette& p = activePalette();
    color(toFl(hover ? lighten(p.accent, 16) : p.accent));
    labelcolor(toFl(p.onAccent));
}

void FilledButton::reapplyTheme() {
    FlatButton::reapplyTheme(); // FIRST: re-reads the shared pressed fill (and the semantic rest one)
    applyFill(false);           // then ours -- the accent is a palette RGB, not a semantic slot
    redraw();
}

int FilledButton::handle(int event) {
    switch (event) {
    case FL_ENTER:
        applyFill(true);
        redraw();
        return 1;
    case FL_LEAVE:
        applyFill(false);
        redraw();
        return 1;
    default:
        return Fl_Button::handle(event); // NOT FlatButton::handle (it resets to controlBg/hover)
    }
}

// ---- GlyphButton: a styled B/I/U/S / align / check toggle (see widgets.hpp) ----------------------
Fl_Color GlyphButton::glyphColor() const {
    const Palette& p = activePalette();
    return toFl(active_r() ? p.text : p.textMuted); // active_r(): an ancestor's deactivate() counts too
}

void GlyphButton::draw() {
    Fl_Button::draw(); // flat box + (toggled) down-box; the label is empty -- we draw the glyph
    fl_color(glyphColor());
    switch (m_kind) {
    case Kind::Bold: drawLetter("B", FL_HELVETICA_BOLD, Deco::None); break;
    case Kind::Italic: drawLetter("I", FL_HELVETICA_ITALIC, Deco::None); break;
    case Kind::Underline: drawLetter("U", FL_HELVETICA, Deco::Underline); break;
    case Kind::Strike: drawLetter("S", FL_HELVETICA, Deco::Strike); break;
    case Kind::Check: drawCheck(); break;
    default: drawAlign(); break;
    }
}

// ---- CheckBox: the settled themed checkbox (see widgets.hpp) -------------------------------------
int CheckBox::handle(int event) {
    switch (event) {
    case FL_ENTER: m_hover = true; redraw(); return 1;
    case FL_LEAVE: m_hover = false; redraw(); return 1;
    case FL_PUSH: return 1; // claim the press; toggle on release if it stays inside
    case FL_RELEASE:
        if (Fl::event_inside(this) && active_r()) { // a greyed checkbox (e.g. Hyphenate in vertical)
            m_checked = !m_checked;                 // must not toggle
            redraw();
            if (m_onToggle)
                m_onToggle(m_checked);
        }
        return 1;
    default: return Fl_Widget::handle(event);
    }
}

void CheckBox::draw() {
    const Palette& p = activePalette();
    const bool enabled = active_r(); // a deactivated host (e.g. Hyphenate in vertical) reads muted
    fl_color(toFl(m_hasGround ? m_ground : p.windowBg)); // erase first (double buffer keeps stale px)
    fl_rectf(x(), y(), w(), h());
    constexpr int kBox = 18;
    const int bx = x();
    const int by = y() + (h() - kBox) / 2;
    fl_color(toFl(m_checked ? (enabled ? p.accent : p.controlActive)
                            : (enabled && m_hover ? p.controlHover : p.controlBg)));
    fl_rectf(bx, by, kBox, kBox);
    fl_color(toFl(p.border));
    fl_rect(bx, by, kBox, kBox);
    if (m_checked) { // a tick (no glyph -- host-font rule); one CONNECTED path so the round join
        fl_color(toFl(enabled ? p.onAccent : p.textMuted)); // fills the bottom vertex
        fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, 2);
        fl_begin_line();
        fl_vertex(bx + 4, by + 9);
        fl_vertex(bx + 8, by + 13);
        fl_vertex(bx + 14, by + 5);
        fl_end_line();
        fl_line_style(0);
    }
    fl_color(toFl(enabled ? p.text : p.textMuted));
    fl_font(FL_HELVETICA, 13);
    fl_draw(label(), bx + kBox + 10, y(), w() - kBox - 10, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
}

int SwatchChip::handle(int event) {
    switch (event) {
    case FL_ENTER:
    case FL_MOVE: // claim hover so the tooltip surfaces even in passive mode (the HoverBox rule)
        if (m_interactive && active_r() && !m_hover) {
            m_hover = true;
            if (window() != nullptr)
                window()->cursor(FL_CURSOR_HAND);
            redraw();
        }
        return 1;
    case FL_LEAVE:
        if (m_hover) {
            m_hover = false;
            if (window() != nullptr)
                window()->cursor(FL_CURSOR_DEFAULT);
            redraw();
        }
        return 1;
    case FL_PUSH:
        return m_interactive && active_r() ? 1 : 0;
    case FL_RELEASE:
        if (m_interactive && active_r() && Fl::event_inside(this) && m_onClick)
            m_onClick();
        return 1;
    default:
        return Fl_Widget::handle(event);
    }
}

void SwatchChip::draw() {
    const Palette& p = activePalette();
    fl_color(toFl(m_hasGround ? m_ground : p.windowBg)); // erase own bg first (FLTK rule)
    fl_rectf(x(), y(), w(), h());
    const bool live = m_interactive && active_r();
    const int kSw = std::min(28, w()); // the chip; the readout + affordances sit to its right
    if (m_mixed) { // diagonal hatch over a neutral ground = several colours across the selection
        fl_color(toFl(p.controlBg));
        fl_rectf(x(), y(), kSw, h());
        fl_color(toFl(p.textMuted));
        fl_push_clip(x(), y(), kSw, h());
        for (int o = -h(); o < kSw; o += 4)
            fl_line(x() + o, y() + h(), x() + o + h(), y());
        fl_pop_clip();
    } else {
        fl_color(toFl(m_c));
        fl_rectf(x(), y(), kSw, h());
    }
    fl_color(toFl(live && m_hover ? p.accent : p.border));
    fl_rect(x(), y(), kSw, h());
    char hex[16];
    if (m_mixed)
        std::snprintf(hex, sizeof(hex), "%s", "\xE2\x80\x94"); // an em dash, like the sliders
    else
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", m_c.r, m_c.g, m_c.b);
    fl_color(toFl(p.textMuted));
    fl_font(FL_HELVETICA, 12);
    fl_draw(hex, x() + kSw + 10, y(), w() - kSw - 10, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    if (m_interactive) { // a hint + chevron so the chip reads as a button
        fl_color(toFl(live && m_hover ? p.text : p.textMuted));
        fl_draw(_("Edit\xE2\x80\xA6"), x(), y(), w() - 22, h(), FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        const int cx = x() + w() - 12;
        const int cy = y() + h() / 2;
        fl_begin_polygon();
        fl_vertex(cx - 4, cy - 2);
        fl_vertex(cx + 4, cy - 2);
        fl_vertex(cx, cy + 3);
        fl_end_polygon();
    }
}

int SwatchButton::handle(int event) {
    switch (event) {
    case FL_ENTER:
    case FL_MOVE:
        if (active_r() && !m_hover) {
            m_hover = true;
            if (window() != nullptr) window()->cursor(FL_CURSOR_HAND);
            redraw();
        }
        return 1;
    case FL_LEAVE:
        if (m_hover) {
            m_hover = false;
            if (window() != nullptr) window()->cursor(FL_CURSOR_DEFAULT);
            redraw();
        }
        return 1;
    case FL_PUSH:
        return active_r() ? 1 : 0;
    case FL_RELEASE:
        if (active_r() && Fl::event_inside(this) && m_onClick) m_onClick();
        return 1;
    default:
        return Fl_Widget::handle(event);
    }
}

void SwatchButton::draw() {
    const Palette& p = activePalette();
    const common::Color8 c = m_get ? m_get() : common::Color8{0, 0, 0, 255};
    const float a = c.a / 255.0f; // composite over a checker so a transparent colour reads
    for (int yy = 0; yy < h(); ++yy)
        for (int xx = 0; xx < w(); ++xx) {
            const bool dk = ((xx / 6) + (yy / 6)) & 1;
            const float bg = dk ? 205.0f : 255.0f;
            const auto ch = [&](std::uint8_t v) {
                return static_cast<std::uint8_t>(std::lround(v * a + bg * (1.0f - a)));
            };
            fl_color(fl_rgb_color(ch(c.r), ch(c.g), ch(c.b)));
            fl_point(x() + xx, y() + yy);
        }
    fl_color(toFl(m_hover ? p.accent : p.border));
    fl_rect(x(), y(), w(), h());
}

void GlyphButton::drawLetter(const char* s, int font, Deco deco) {
    fl_font(font, 13);
    fl_draw(s, x(), y(), w(), h(), FL_ALIGN_CENTER); // current font + colour, centred in the cell
    if (deco == Deco::None)
        return;
    const int cx = x() + w() / 2;
    const int cy = y() + h() / 2;
    const int half = static_cast<int>(std::ceil(fl_width(s))) / 2 + 1;
    const int baseline = cy + fl_height() / 2 - fl_descent();
    const int ly = deco == Deco::Underline ? baseline + 1 : cy; // just under / through the glyph
    fl_line_style(FL_SOLID | FL_CAP_ROUND, 1);
    fl_line(cx - half, ly, cx + half, ly);
    fl_line_style(0);
}

void GlyphButton::drawAlign() {
    // A small, centred alignment glyph: 4 short pixel-aligned rules (fl_rectf, not fl_line -- the 2px
    // round-capped lines read blurry). Compact: a near-full-width stack looked "huge".
    constexpr int gw = 12, th = 2, gap = 2, n = 4; // gw EVEN so centred bars sit pixel-symmetric
    const int lx = x() + (w() - gw) / 2;
    int yy = y() + (h() - (n * th + (n - 1) * gap)) / 2;
    static const float kRagged[4] = {1.0f, 0.55f, 0.85f, 0.5f}; // left / right: ragged one side
    static const float kCentre[4] = {1.0f, 0.5f, 0.83f, 0.67f}; // centre: varied lengths (even-snapped)
    const float* lens = m_kind == Kind::AlignCenter ? kCentre : kRagged;
    for (int i = 0; i < n; ++i) {
        const bool justify = m_kind == Kind::AlignJustify;
        const float frac = justify ? (i == n - 1 ? 0.6f : 1.0f) : lens[i]; // justify: short last line
        int len = std::max(2, static_cast<int>(std::lround(gw * frac)));
        int x0 = lx;
        if (m_kind == Kind::AlignRight) {
            x0 = lx + (gw - len);
        } else if (m_kind == Kind::AlignCenter) {
            len &= ~1;                 // even length -> symmetric centring in the even gw (no 1px lean)
            x0 = lx + (gw - len) / 2;
        }
        fl_rectf(x0, yy, len, th); // left + justify are flush-left
        yy += gap + th;
    }
}

void GlyphButton::drawCheck() {
    const int cx = x() + w() / 2;
    const int cy = y() + h() / 2;
    const int r = std::clamp(std::min(w(), h()) / 2 - 7, 3, 5);  // small, matches the align glyphs
    fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, 2);
    if (value() != 0) {
        fl_color(glyphColor()); // the tick greys with the control; the ✗ below is muted either way
        fl_begin_line();                       // ✓ : left-mid -> low vertex -> upper-right
        fl_vertex(cx - r, cy);
        fl_vertex(cx - r / 3, cy + r);
        fl_vertex(cx + r, cy - r);
        fl_end_line();
    } else {
        fl_color(toFl(activePalette().textMuted));
        fl_line(cx - r, cy - r, cx + r, cy + r);  // ✗
        fl_line(cx - r, cy + r, cx + r, cy - r);
    }
    fl_line_style(0);
}

void addBlendModeItems(Dropdown& dd) {
    // A divider after each blend family's last mode (indices follow core::BlendMode's order):
    // 0 Normal | 1-4 Darken | 5-8 Lighten | 9-14 Contrast | 15-18 Inversion | 19-22 Component.
    auto familyEnd = [](int i) { return i == 0 || i == 4 || i == 8 || i == 14 || i == 18; };
    for (int i = 0; i < core::kBlendModeCount; ++i) {
        const int flags = familyEnd(i) ? FL_MENU_DIVIDER : 0;
        dd.add(std::string(core::blendModeName(static_cast<core::BlendMode>(i))).c_str(), 0, nullptr,
               nullptr, flags);
    }
}

// ---- Dropdown themed pop-up ------------------------------------------------------------------

namespace {
constexpr int kDdRowH = 24; // height of one item row in the pop-up list
constexpr int kDdPad = 4;   // inset above/below the rows
constexpr int kDdPreviewW = 88; // preview-list right-hand cell (the in-face sample), hugs the dot
constexpr int kDdMinW = 90; // a narrow dropdown still gets a legible list
constexpr int kDdDivGap = 7; // extra height below a FL_MENU_DIVIDER row (matches the context menu)
constexpr int kDdScrollW = 9; // right-gutter width reserved for the scrollbar when the list scrolls
constexpr int kDdMargin = 6;  // inset a scrolling list keeps from the parent-window edges

// Registry of the per-host pop-ups (one per top-level window that hosts Dropdowns) + the one open
// now. The pop-up is a child sub-window of its host, so a Dropdown finds its list by top-level.
std::vector<DropdownPopup*> g_dropdownPopups;
DropdownPopup* g_activeDropdown = nullptr;

DropdownPopup* dropdownPopupFor(const Fl_Window* host) {
    for (DropdownPopup* p : g_dropdownPopups)
        if (p->window() == host)
            return p;
    return nullptr;
}
} // namespace

DropdownPopup::DropdownPopup() : Fl_Double_Window(0, 0, kDdMinW, kDdRowH) {
    border(0); // a sub-window has no decoration anyway; belt-and-braces (mirrors ui::Popover)
    color(toFl(activePalette().panelBg));
    end(); // rows are drawn directly; no child widgets
    g_dropdownPopups.push_back(this);
}

DropdownPopup::~DropdownPopup() {
    if (g_activeDropdown == this)
        g_activeDropdown = nullptr;
    g_dropdownPopups.erase(std::remove(g_dropdownPopups.begin(), g_dropdownPopups.end(), this),
                           g_dropdownPopups.end());
}

void DropdownPopup::show() {
    Fl_Double_Window::show();
    platform::raiseNativeWindowToTop(this);
}

void DropdownPopup::openFor(Dropdown* owner) {
    m_owner = owner;
    m_items.clear();
    m_dividers.clear();
    m_disabled.clear();
    for (const Fl_Menu_Item* it = owner->menu(); it != nullptr && it->label() != nullptr;
         it = it->next()) {
        m_items.emplace_back(it->label());
        m_dividers.push_back((it->flags & FL_MENU_DIVIDER) != 0); // group separator below this row
        m_disabled.push_back((it->flags & FL_MENU_INACTIVE) != 0); // greyed + unpickable row
    }
    if (m_items.empty())
        return;
    m_rowMarquees.clear(); // fresh list, fresh scroll states
    m_rowMarquees.resize(m_items.size());
    m_rowH = owner->rowHeight() > 0 ? owner->rowHeight() : kDdRowH; // taller rows for a preview list
    m_lastPreviewIdx = -1; // fresh open: nothing previewed yet
    m_value = owner->value();
    m_marked = owner->markedItems(); // dot every present mode (mixed selection); empty = just m_value
    m_hover = std::clamp(m_value, 0, static_cast<int>(m_items.size()) - 1);
    m_dragged = false;
    // Translate the owner's position into THIS pop-up's top-level coordinates. FLTK widget coords are
    // relative to the nearest enclosing Fl_Window, so a combo inside a sub-window (e.g. the colour-
    // picker popover) needs each enclosing sub-window's offset added. The pop-up is a child of the
    // top-level, a *sibling* of any such sub-window (drawn above it), never nested inside it.
    int ox = owner->x();
    int oy = owner->y();
    for (Fl_Window* w = owner->window(); w != nullptr && w != window(); w = w->window()) {
        ox += w->x();
        oy += w->y();
    }
    m_ownerX = ox;
    m_ownerY = oy;
    m_ownerW = owner->w();
    m_ownerH = owner->h();

    const int contentH = rowTop(static_cast<int>(m_items.size())) + kDdPad;
    const Fl_Window* parent = window();
    // Cap the list to the parent window (minus a small margin) so a list taller than the window
    // (e.g. the 23 blend modes) doesn't get clipped off-screen; the overflow scrolls internally.
    const int maxH = parent ? std::max(m_rowH + 2 * kDdPad, parent->h() - 2 * kDdMargin) : contentH;
    const bool scrolling = contentH > maxH;
    const int barW = scrolling ? kDdScrollW : 0;
    const int pw = std::max({owner->w(), kDdMinW, owner->listMinWidth()}) + barW;
    const int ph = scrolling ? maxH : contentH;
    m_maxScroll = std::max(0, contentH - ph);

    // Place over the combo with the selected row aligned to the closed control (so the value isn't
    // shown twice). When scrolling, anchor near the control and scroll the selected row to the top.
    int px = ox;
    int py = scrolling ? oy : oy - rowTop(m_hover);
    m_scroll = scrolling ? std::clamp(rowTop(m_hover) - kDdPad, 0, m_maxScroll) : 0;
    if (parent != nullptr) {
        px = std::clamp(px, 0, std::max(0, parent->w() - pw));
        const int lo = scrolling ? kDdMargin : 0;
        const int hi = std::max(lo, parent->h() - ph - (scrolling ? kDdMargin : 0));
        py = std::clamp(py, lo, hi);
    }
    m_selfResize = true; // the only sanctioned resize (resize() ignores parent-driven ones)
    resize(px, py, pw, ph);
    m_selfResize = false;
    show();
    g_activeDropdown = this;
}

void DropdownPopup::resize(int X, int Y, int W, int H) {
    // Fixed-size: ignore parent-window-driven resizes (those would stretch the open list). Only
    // openFor() resizes, with m_selfResize set.
    if (m_selfResize)
        Fl_Double_Window::resize(X, Y, W, H);
}

void DropdownPopup::setScroll(int s) {
    const int c = std::clamp(s, 0, m_maxScroll);
    if (c != m_scroll) {
        m_scroll = c;
        redraw();
    }
}

// Vertical scrollbar geometry (shared by paintScrollGrab + the drag in handle()). The grab is a
// pill in the right gutter; its length is the viewport's fraction of the content, its position the
// scroll fraction. trackTop/trackLen are the usable run inside the kDdPad insets.
void DropdownPopup::vScrollGeom(int& trackTop, int& trackLen, int& thumbY, int& thumbH) const {
    trackTop = kDdPad;
    trackLen = std::max(1, h() - 2 * kDdPad);
    const double contentH = static_cast<double>(h() + m_maxScroll); // viewport h() + overflow
    thumbH = static_cast<int>(std::lround(trackLen * (static_cast<double>(h()) / contentH)));
    thumbH = std::clamp(thumbH, 18, trackLen);
    const double frac = m_maxScroll > 0 ? static_cast<double>(m_scroll) / m_maxScroll : 0.0;
    thumbY = trackTop + static_cast<int>(std::lround((trackLen - thumbH) * frac));
}

void DropdownPopup::hide() {
    for (auto& m : m_rowMarquees) // no list, no scrolling rows
        if (m)
            m->stop();
    setPreviewHover(-1); // revert any live hover preview before the list closes (and before a commit's
                         // own callback runs -- commit() calls hide() first, see there)
    if (g_activeDropdown == this)
        g_activeDropdown = nullptr;
    Fl_Double_Window::hide();
}

bool DropdownPopup::spansHostPoint(int hostX, int hostY) const {
    const bool inPopup = hostX >= x() && hostX < x() + w() && hostY >= y() && hostY < y() + h();
    const bool inOwner = hostX >= m_ownerX && hostX < m_ownerX + m_ownerW && hostY >= m_ownerY &&
                         hostY < m_ownerY + m_ownerH;
    return inPopup || inOwner;
}

void DropdownPopup::draw() {
    const Palette& p = activePalette();
    fl_color(toFl(p.panelBg));
    fl_rectf(0, 0, w(), h());
    fl_font(FL_HELVETICA, 12);
    const int innerW = w() - (scrollable() ? kDdScrollW : 0); // rows stop before the scrollbar gutter
    fl_push_clip(1, 1, innerW - 2, h() - 2); // scrolled rows must not bleed over the frame
    const auto& preview = m_owner != nullptr ? m_owner->rowPreview()
                                             : std::function<Fl_RGB_Image*(int, int, int)>{};
    for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
        const int ry = rowTop(i) - m_scroll; // content -> viewport coords
        if (ry + m_rowH <= 1 || ry >= h() - 1)
            continue; // fully outside the viewport (also below a divider that scrolled away)
        // Decoupled cues so neither overpowers (user feedback): HOVER is a neutral grey wash
        // (controlHover, theme-aware in both modes); the current SELECTION is a small accent dot on
        // the right. Text stays the normal colour throughout, so hovering the selected row reads
        // clearly (no accent-on-accent low contrast, no harsh swap).
        const bool rowDisabled =
            i < static_cast<int>(m_disabled.size()) && m_disabled[static_cast<std::size_t>(i)];
        const common::Color8 rowBg =
            (i == m_hover && !rowDisabled) ? p.controlHover : p.panelBg; // no hover cue on a dead row
        fl_color(toFl(rowBg));
        fl_rectf(1, ry, innerW - 2, m_rowH);
        // With a preview list: the in-face PREVIEW occupies a fixed cell hugging the selection dot on
        // the RIGHT (its sample is right-aligned, so the ink ends at a consistent x next to the dot --
        // no mid-row float); the family NAME takes the rest of the row on the LEFT and is clipped if it
        // runs long (names are arbitrary). Without a preview the label spans the whole row, as before.
        const int dotGutter = 24; // reserved on the right for the selection dot
        const int nameX = 9;
        fl_color(toFl(rowDisabled ? mix(p.panelBg, p.text, 0.45F) : p.text)); // dim a dead row
        // Dot every marked row, else just the selected value. A mixed multi-selection (S15-e "All")
        // dots EVERY mode present across the selection; the plain case dots only m_value.
        // (Computed BEFORE the label: an undotted row's text may use the dot gutter — the
        // marquee turning around 24px early read as if a phantom dot sat there, user 2026-07-02.)
        const bool dotted =
            m_marked.empty() ? (i == m_value)
                             : std::find(m_marked.begin(), m_marked.end(), i) != m_marked.end();
        const int rightPad = dotted ? dotGutter : 8; // undotted rows run to a plain margin
        // Every overflowing row runs the shared marquee (the ScrollingLabel behaviour, not
        // hover-gated — user call). The greyed crop Fill entry "Inpaint (unavailable when
        // rotated)" is the motivating case.
        auto& rowMarquee = m_rowMarquees[static_cast<std::size_t>(i)];
        const auto drawName = [&](int nx, int nw) {
            if (fl_width(m_items[static_cast<std::size_t>(i)].c_str()) >
                static_cast<double>(nw)) {
                if (!rowMarquee)
                    rowMarquee = std::make_unique<Marquee>();
                rowMarquee->draw(this, m_items[static_cast<std::size_t>(i)].c_str(), nx, ry, nw,
                                 m_rowH);
            } else {
                fl_draw(m_items[static_cast<std::size_t>(i)].c_str(), nx, ry, nw, m_rowH,
                        FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            }
        };
        if (preview) {
            const int pvW = std::min(kDdPreviewW, innerW - dotGutter - nameX - 24); // leave name room
            const int pvX = innerW - dotGutter - pvW;
            drawName(nameX, std::max(10, pvX - nameX - 6));
            if (pvW > 8) {
                if (Fl_RGB_Image* img = preview(i, pvW, m_rowH))
                    img->draw(pvX, ry); // owner-cached; clipped to the row by the push_clip above
            }
        } else {
            drawName(nameX, innerW - rightPad - nameX);
        }
        if (dotted) { // selection marker: an AA accent disc (no Unicode glyph -- host-font rule)
            const int dcx = innerW - 12;
            const int dcy = ry + m_rowH / 2;
            const auto under = [rowBg](int, int) { return rowBg; };
            drawAAPrims(dcx - 5, dcy - 5, 11, 11, under, {{dcx + 0.5, dcy + 0.5, 3.0, 0.0, p.accent}});
        }
        if (i < static_cast<int>(m_dividers.size()) && m_dividers[static_cast<std::size_t>(i)]) {
            const int dy = ry + m_rowH + kDdDivGap / 2; // a hairline centred in the gap below the row
            fl_color(toFl(p.border));
            fl_line(7, dy, innerW - 7, dy);
        }
    }
    fl_pop_clip();
    if (scrollable())
        paintScrollGrab();
    fl_color(toFl(p.border)); // a crisp 1px frame on top
    fl_rect(0, 0, w(), h());
}

// The themed vertical scrollbar grab (mirrors ScrollView's pill look: an AA rounded rect, control
// greys at rest, brighter on hover, accent while dragged; light mode steps toward text so the grab
// stays visible on the near-white trough).
void DropdownPopup::paintScrollGrab() {
    const Palette& p = activePalette();
    const common::Color8 bg = p.panelBg;
    const int barX = w() - kDdScrollW;
    fl_color(toFl(bg)); // trough
    fl_rectf(barX, 1, kDdScrollW, h() - 2);

    int trackTop;
    int trackLen;
    int thumbY;
    int thumbH;
    vScrollGeom(trackTop, trackLen, thumbY, thumbH);

    common::Color8 grab;
    if (m_vDrag)
        grab = p.accent;
    else if (p.dark)
        grab = m_vHover ? p.controlHover : p.controlBg;
    else
        grab = mix(bg, p.text, m_vHover ? 0.42F : 0.30F);

    const int gx = barX + 2;
    const int gw = kDdScrollW - 4;
    const double cr = std::min(gw, thumbH) / 2.0;
    const int top = thumbY;
    const int bot = thumbY + thumbH;
    const int left = gx;
    const int right = gx + gw;
    const auto under = [&](int ux, int uy) -> common::Color8 {
        const bool inV = uy >= top + cr && uy <= bot - cr;
        const bool inH = ux >= left + cr && ux <= right - cr;
        return (inV || inH) ? grab : bg;
    };
    drawAAPrims(gx, thumbY, gw, thumbH, under,
                {{left + cr, top + cr, cr, 0.0, grab},
                 {right - cr, top + cr, cr, 0.0, grab},
                 {left + cr, bot - cr, cr, 0.0, grab},
                 {right - cr, bot - cr, cr, 0.0, grab}});
}

int DropdownPopup::rowTop(int index) const {
    int y = kDdPad;
    for (int i = 0; i < index; ++i) {
        y += m_rowH;
        if (i < static_cast<int>(m_dividers.size()) && m_dividers[static_cast<std::size_t>(i)])
            y += kDdDivGap; // the separator below this row pushes the next one down
    }
    return y;
}

int DropdownPopup::indexAt(int localX, int localY) const {
    const int innerW = w() - (scrollable() ? kDdScrollW : 0);
    if (localX < 0 || localX >= innerW) // a click in the scrollbar gutter is not a row
        return -1;
    const int cy = localY + m_scroll; // viewport -> content coords
    for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
        const int ry = rowTop(i);
        if (cy >= ry && cy < ry + m_rowH)
            return i; // the divider gap is a dead band (returns -1), which is fine
    }
    return -1;
}

void DropdownPopup::setPreviewHover(int index) {
    if (index == m_lastPreviewIdx)
        return;
    m_lastPreviewIdx = index;
    if (m_owner != nullptr && m_owner->hoverPreview())
        m_owner->hoverPreview()(index); // live-preview the hovered item (font picker); -1 reverts
}

void DropdownPopup::commit(int index) {
    Dropdown* owner = m_owner;
    if (index >= 0 && index < static_cast<int>(m_disabled.size()) &&
        m_disabled[static_cast<std::size_t>(index)])
        return; // a greyed row is not a choice; the list stays open
    const bool ok = index >= 0 && index < static_cast<int>(m_items.size());
    hide(); // close the list BEFORE running the callback (which may rebuild widgets)
    if (ok && owner != nullptr) {
        owner->value(index);
        owner->redraw();
        owner->do_callback();
    }
}

int DropdownPopup::handle(int event) {
    const bool overBar = scrollable() && Fl::event_x() >= w() - kDdScrollW;
    switch (event) {
    case FL_MOUSEWHEEL:
        if (scrollable() && Fl::event_dy() != 0) {
            setScroll(m_scroll + Fl::event_dy() * kDdRowH);
            return 1;
        }
        return 1; // swallow so the wheel doesn't scroll something behind the open list
    case FL_PUSH: {
        if (overBar) { // start a scrollbar drag (on the grab: in place; in the trough: jump + drag)
            int trackTop;
            int trackLen;
            int thumbY;
            int thumbH;
            vScrollGeom(trackTop, trackLen, thumbY, thumbH);
            const int ey = Fl::event_y();
            const bool onThumb = ey >= thumbY && ey < thumbY + thumbH;
            m_dragGrabOffset = onThumb ? (ey - thumbY) : (thumbH / 2);
            m_vDrag = true;
            const int span = std::max(1, trackLen - thumbH);
            const double frac = std::clamp(double(ey - m_dragGrabOffset - trackTop) / span, 0.0, 1.0);
            setScroll(static_cast<int>(std::lround(frac * m_maxScroll)));
            redraw();
            return 1;
        }
        const int idx = indexAt(Fl::event_x(), Fl::event_y());
        if (idx >= 0)
            commit(idx);
        return 1; // an outside press is dismissed by the host (dismissActiveDropdownPopup...)
    }
    case FL_RELEASE:
        if (m_vDrag) {
            m_vDrag = false;
            redraw();
            return 1;
        }
        if (m_dragged) { // press-drag-release onto an item selects it
            const int idx = indexAt(Fl::event_x(), Fl::event_y());
            if (idx >= 0)
                commit(idx);
        }
        return 1;
    case FL_DRAG:
        if (m_vDrag) {
            int trackTop;
            int trackLen;
            int thumbY;
            int thumbH;
            vScrollGeom(trackTop, trackLen, thumbY, thumbH);
            const int span = std::max(1, trackLen - thumbH);
            const double frac =
                std::clamp(double(Fl::event_y() - m_dragGrabOffset - trackTop) / span, 0.0, 1.0);
            setScroll(static_cast<int>(std::lround(frac * m_maxScroll)));
            return 1;
        }
        [[fallthrough]];
    case FL_MOVE: {
        const bool hov = overBar && !m_vDrag;
        if (hov != m_vHover) {
            m_vHover = hov;
            redraw();
        }
        if (event == FL_DRAG)
            m_dragged = true;
        const int idx = indexAt(Fl::event_x(), Fl::event_y());
        if (idx >= 0 && idx != m_hover) {
            m_hover = idx;
            redraw();
        }
        if (idx >= 0)
            setPreviewHover(idx); // live-preview the row under the cursor (no-op for a plain list)
        return 1;
    }
    case FL_LEAVE:
        setPreviewHover(-1); // cursor left the open list -> revert the live preview (list stays open)
        if (m_vHover) {
            m_vHover = false;
            redraw();
        }
        return Fl_Double_Window::handle(event);
    default:
        return Fl_Double_Window::handle(event);
    }
}

DropdownPopup* activeDropdownPopup() {
    return g_activeDropdown;
}

void dismissActiveDropdownPopup() {
    if (g_activeDropdown != nullptr)
        g_activeDropdown->hide();
}

void dismissActiveDropdownPopupOnOutsideClick(int hostX, int hostY) {
    if (g_activeDropdown != nullptr && !g_activeDropdown->spansHostPoint(hostX, hostY))
        g_activeDropdown->hide();
}

// ---- ContextMenu ---------------------------------------------------------------------------

namespace {
constexpr int kCmRowH = 24;      // height of one menu row
constexpr int kCmPad = 4;        // inset above/below the rows
constexpr int kCmHInset = 14;    // left text inset
constexpr int kCmRightInset = 16;// right inset after the label
constexpr int kCmDivGap = 7;     // extra height a divider row adds below itself
constexpr int kCmMinW = 150;     // a context menu is at least this wide

std::vector<ContextMenu*> g_contextMenus; // one per top-level that hosts text fields
ContextMenu* g_activeContextMenu = nullptr;

// Copy a field's current selection, or -- when nothing is selected -- its whole value, to the
// clipboard. Fl::copy puts arbitrary text on the clipboard without disturbing the field's selection.
void copyFieldText(Fl_Input_* in) {
    if (in->insert_position() != in->mark()) {
        in->copy(1); // a selection exists -> copy exactly it
    } else {
        const char* v = in->value();
        Fl::copy(v, static_cast<int>(std::strlen(v)), 1);
    }
}
} // namespace

ContextMenu::ContextMenu() : Fl_Double_Window(0, 0, kCmMinW, kCmRowH) {
    border(0); // a borderless sub-window (mirrors DropdownPopup / Popover)
    color(toFl(activePalette().panelBg));
    end(); // rows are drawn directly; no child widgets
    g_contextMenus.push_back(this);
}

ContextMenu::~ContextMenu() {
    if (g_activeContextMenu == this)
        g_activeContextMenu = nullptr;
    g_contextMenus.erase(std::remove(g_contextMenus.begin(), g_contextMenus.end(), this),
                         g_contextMenus.end());
}

void ContextMenu::show() {
    Fl_Double_Window::show();
    platform::raiseNativeWindowToTop(this);
}

void ContextMenu::openWith(int hostX, int hostY, std::vector<ContextAction> actions) {
    m_actions = std::move(actions);
    m_hover = -1;
    m_top.clear();
    fl_font(FL_HELVETICA, 13);
    int contentW = 0;
    int yy = kCmPad;
    for (const ContextAction& a : m_actions) {
        contentW = std::max(contentW, static_cast<int>(fl_width(a.label.c_str())));
        m_top.push_back(yy);
        yy += kCmRowH + (a.divider ? kCmDivGap : 0);
    }
    const int pw = std::max(kCmMinW, kCmHInset + contentW + kCmRightInset);
    const int ph = yy + kCmPad;
    int px = hostX;
    int py = hostY;
    if (const Fl_Window* parent = window()) { // keep fully inside the top-level window
        px = std::clamp(px, 0, std::max(0, parent->w() - pw));
        py = std::clamp(py, 0, std::max(0, parent->h() - ph));
    }
    resize(px, py, pw, ph);
    show();
    redraw();     // refill when reused (same window, fresh contents)
    take_focus(); // so Escape (handled below) reaches the open menu
    g_activeContextMenu = this;
}

void ContextMenu::hide() {
    if (g_activeContextMenu == this)
        g_activeContextMenu = nullptr;
    Fl_Double_Window::hide();
}

bool ContextMenu::spansHostPoint(int hostX, int hostY) const {
    return hostX >= x() && hostX < x() + w() && hostY >= y() && hostY < y() + h();
}

void ContextMenu::draw() {
    const Palette& p = activePalette();
    fl_color(toFl(p.panelBg));
    fl_rectf(0, 0, w(), h());
    fl_font(FL_HELVETICA, 13);
    for (int i = 0; i < static_cast<int>(m_actions.size()); ++i) {
        const ContextAction& a = m_actions[static_cast<std::size_t>(i)];
        const int top = m_top[static_cast<std::size_t>(i)];
        if (i == m_hover && a.enabled) {
            fl_color(toFl(p.controlHover));
            fl_rectf(1, top, w() - 2, kCmRowH);
        }
        fl_color(toFl(a.enabled ? p.text : p.textMuted));
        fl_draw(a.label.c_str(), kCmHInset, top, w() - kCmHInset - kCmRightInset, kCmRowH,
                FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        if (a.divider) {
            fl_color(toFl(p.border));
            const int dy = top + kCmRowH + kCmDivGap / 2;
            fl_xyline(kCmHInset, dy, w() - kCmHInset);
        }
    }
    fl_color(toFl(p.border)); // crisp 1px frame on top
    fl_rect(0, 0, w(), h());
}

int ContextMenu::rowAt(int localY) const {
    for (int i = 0; i < static_cast<int>(m_actions.size()); ++i) {
        const int t = m_top[static_cast<std::size_t>(i)];
        if (localY >= t && localY < t + kCmRowH)
            return i;
    }
    return -1;
}

void ContextMenu::commit(int row) {
    const bool valid = row >= 0 && row < static_cast<int>(m_actions.size());
    if (!valid || !m_actions[static_cast<std::size_t>(row)].enabled)
        return;
    // Move the action out and hide BEFORE running it -- the action mutates the field (and the menu
    // may be reused), so it must not see a stale open menu.
    std::function<void()> act = m_actions[static_cast<std::size_t>(row)].action;
    hide();
    if (act)
        act();
}

int ContextMenu::handle(int event) {
    switch (event) {
    case FL_PUSH: { // commit on release; press just tracks hover (a single click = press+release)
        const int i = rowAt(Fl::event_y());
        if (i != m_hover) {
            m_hover = i;
            redraw();
        }
        return 1;
    }
    case FL_RELEASE:
        commit(rowAt(Fl::event_y()));
        return 1;
    case FL_MOVE:
    case FL_DRAG: {
        const int i = rowAt(Fl::event_y());
        if (i != m_hover) {
            m_hover = i;
            redraw();
        }
        return 1;
    }
    case FL_FOCUS:
    case FL_UNFOCUS:
        return 1; // keep keyboard focus so Escape lands here
    case FL_KEYBOARD:
    case FL_SHORTCUT:
        if (Fl::event_key() == FL_Escape) {
            hide();
            return 1;
        }
        return 1; // swallow other keys while the menu is open
    default:
        return Fl_Double_Window::handle(event);
    }
}

ContextMenu* contextMenuFor(const Fl_Window* host) {
    for (ContextMenu* m : g_contextMenus)
        if (m->window() == host)
            return m;
    return nullptr;
}

ContextMenu* activeContextMenu() {
    return g_activeContextMenu;
}

void dismissActiveContextMenu() {
    if (g_activeContextMenu != nullptr)
        g_activeContextMenu->hide();
}

void dismissActiveContextMenuOnOutsideClick(int hostX, int hostY) {
    if (g_activeContextMenu != nullptr && !g_activeContextMenu->spansHostPoint(hostX, hostY))
        g_activeContextMenu->hide();
}

bool handleTextFieldEvent(Fl_Input_* field, int event, bool editable) {
    // While our context menu is open, swallow the drag/release of the very press that opened it: the
    // field stays the platform's "pushed" widget, so without this the right-button drag keeps
    // extending a text selection underneath the open menu (user-reported).
    if ((event == FL_DRAG || event == FL_RELEASE) && activeContextMenu() != nullptr)
        return true;
    if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE && field->active()) {
        Fl::focus(field); // address this field (a follow-up Ctrl+C works) + show its caret
        field->redraw();
        ContextMenu* menu = contextMenuFor(field->top_window());
        if (menu == nullptr)
            return false; // no themed host -> fall back to FLTK's stock right-click menu

        const bool hasSel = field->insert_position() != field->mark();
        const bool hasText = field->size() > 0;
        std::vector<ContextAction> actions;
        if (editable) {
            actions.push_back({_("Cut"), [field] { copyFieldText(field); field->cut(); }, hasSel});
            actions.push_back({_("Copy"), [field] { copyFieldText(field); }, hasText});
            actions.push_back({_("Paste"), [field] { Fl::paste(*field, 1); }, true});
            actions.push_back({_("Delete"), [field] { field->cut(); }, hasSel, /*divider=*/true});
        } else {
            actions.push_back({_("Copy"), [field] { copyFieldText(field); }, hasText});
        }
        actions.push_back({_("Select All"),
                           [field] {
                               field->insert_position(field->size(), 0);
                               field->redraw();
                           },
                           hasText});

        // Anchor at the cursor in the menu's top-level coords. Fl::event_x/y are relative to the
        // field's enclosing window, so add each sub-window offset up to the menu's top-level (a field
        // can sit inside a sub-window, e.g. the colour-picker popover).
        int hx = Fl::event_x();
        int hy = Fl::event_y();
        for (Fl_Window* w = field->window(); w != nullptr && w != menu->window(); w = w->window()) {
            hx += w->x();
            hy += w->y();
        }
        menu->openWith(hx, hy, std::move(actions));
        return true;
    }
    if (event == FL_KEYBOARD && (Fl::event_state() & (FL_COMMAND | FL_SHIFT)) == FL_COMMAND &&
        Fl::event_key() == 'c' && field->insert_position() == field->mark()) {
        // Plain Ctrl/Cmd+C (no Shift, so the app's Ctrl+Shift+C "Copy Merged" still passes through)
        // with an empty selection: copy the whole value -- FLTK's default copies only the selection,
        // so an un-selected field otherwise copies nothing (the user-reported case).
        const char* v = field->value();
        Fl::copy(v, static_cast<int>(std::strlen(v)), 1);
        return true;
    }
    return false;
}

// ---- caret blink --------------------------------------------------------------------------
// FLTK 1.4 draws the input cursor STEADY; a real text caret blinks (user 2026-07-22). One timer
// serves the single focused themed field: the "off" phase paints the cursor in the field's own
// ground colour (the caret rectangle becomes invisible), the "on" phase restores its real
// colour. The stored pointer is registered with Fl::watch_widget_pointer, so a field deleted
// while focused (a closing dialog) nulls it -- it can never dangle.
namespace {

constexpr double kCaretBlinkPeriodS = 0.53; // the common desktop cadence

Fl_Widget* g_caretField = nullptr; // watched: FLTK nulls it if the widget is deleted
Fl_Color g_caretColor = 0;         // the field's real cursor colour (restored on stop/reset)
bool g_caretHidden = false;

void caretBlinkTick(void*) {
    auto* field = static_cast<Fl_Input_*>(g_caretField);
    if (field == nullptr)
        return;
    if (Fl::focus() != field) { // focus left without the unfocus hook seeing it: stop cleanly
        field->cursor_color(g_caretColor);
        g_caretField = nullptr;
        return;
    }
    g_caretHidden = !g_caretHidden;
    field->cursor_color(g_caretHidden ? field->color() : g_caretColor);
    field->redraw();
    Fl::repeat_timeout(kCaretBlinkPeriodS, caretBlinkTick);
}

void stopCaretBlink() {
    Fl::remove_timeout(caretBlinkTick);
    if (g_caretField != nullptr) {
        static_cast<Fl_Input_*>(g_caretField)->cursor_color(g_caretColor);
        g_caretField->redraw();
        g_caretField = nullptr;
    }
}

// (Re)arm for `field`: the caret shows solid first and blinks from there -- also the reset used
// when a keystroke/click moves the caret, so it never blinks away mid-typing.
void startCaretBlink(Fl_Input_* field) {
    static bool watchInstalled = false;
    if (!watchInstalled) {
        Fl::watch_widget_pointer(g_caretField); // deletion nulls the pointer, never dangles
        watchInstalled = true;
    }
    if (g_caretField != field) {
        stopCaretBlink(); // restores the previous field and removes the pending tick
        g_caretField = field;
        g_caretColor = field->cursor_color();
    } else {
        Fl::remove_timeout(caretBlinkTick);
        field->cursor_color(g_caretColor);
        field->redraw();
    }
    g_caretHidden = false;
    Fl::add_timeout(kCaretBlinkPeriodS, caretBlinkTick);
}

} // namespace

void noteTextFieldFocusEvent(Fl_Input_* field, int event, int handled) {
    switch (event) {
    case FL_FOCUS:
        if (handled != 0)
            startCaretBlink(field);
        break;
    case FL_UNFOCUS:
    case FL_HIDE:
        if (field == g_caretField)
            stopCaretBlink();
        break;
    case FL_KEYBOARD:
    case FL_PUSH:
        if (handled != 0 && field == g_caretField)
            startCaretBlink(field); // the caret moved: hold it visible, restart the phase
        break;
    default:
        break;
    }
}

// ---- Dropdown ------------------------------------------------------------------------------

Dropdown::Dropdown(int X, int Y, int W, int H, const char* label) : Fl_Choice(X, Y, W, H, label) {
    const Palette& p = activePalette();
    box(MOSAIC_BUTTON_UP_BOX);
    color(toFl(p.controlBg));
    textcolor(toFl(p.text));
    textsize(12);
    labelcolor(toFl(p.text));
    selection_color(toFl(p.accent)); // highlighted row in the pop-up
    clear_visible_focus();
}

void Dropdown::draw() {
    const Palette& p = activePalette();
    const bool on = active_r(); // active_r(), not active(): also greys when an ANCESTOR is disabled
                                // (deactivate() on a parent group leaves each child's own flag set).
    draw_box(box(), toFl(on && m_hover ? p.controlHover : p.controlBg));

    // Chevron at the right edge.
    const int cx = x() + w() - 14;
    const int cy = y() + h() / 2;
    fl_color(toFl(on ? p.textMuted : p.border));
    fl_begin_polygon();
    fl_vertex(cx - 4, cy - 2);
    fl_vertex(cx + 4, cy - 2);
    fl_vertex(cx, cy + 3);
    fl_end_polygon();

    // The override text (a "mixed" state) if set, else the current selection's label; clipped
    // before the chevron.
    const Fl_Menu_Item* mv = mvalue();
    const char* text = !m_override.empty() ? m_override.c_str()
                                           : (mv != nullptr ? mv->label() : nullptr);
    if (text != nullptr) {
        fl_color(toFl(on ? p.text : p.textMuted));
        fl_font(textfont(), textsize());
        // The shared marquee: a label wider than the control scrolls (bounce + dwells) instead
        // of truncating — the app-wide long-label answer, never a per-control width hack.
        m_labelMarquee.draw(this, text, x() + 9, y(), w() - 9 - 20, h());
    }
}

void Dropdown::setOverrideText(std::string text) {
    if (m_override == text)
        return;
    m_override = std::move(text);
    redraw();
}

void Dropdown::setMarkedItems(std::vector<int> indices) {
    if (m_marked == indices)
        return;
    m_marked = std::move(indices);
    redraw(); // affects the open list only, but cheap and keeps a live re-open correct
}

int Dropdown::handle(int event) {
    switch (event) {
    case FL_ENTER:
        m_hover = true;
        redraw();
        return 1; // claim ENTER so FLTK also delivers LEAVE
    case FL_LEAVE:
        m_hover = false;
        redraw();
        return 1;
    case FL_PUSH:
        // Open OUR themed list (a child sub-window of this control's TOP-LEVEL) instead of
        // Fl_Choice's stock Motif pulldown. Falls back to the stock list when the top-level did not
        // create a DropdownPopup. Using top_window() (not window()) means a combo inside a sub-window
        // -- the colour picker -- uses the top-level's pop-up, placed over it as a sibling.
        if (active() && menu() != nullptr) {
            if (DropdownPopup* p = dropdownPopupFor(top_window())) {
                m_hover = false; // the pointer is leaving us for the pop-up
                if (p->shownFor(this))
                    p->hide(); // re-click toggles the list shut
                else
                    p->openFor(this);
                return 1;
            }
        }
        return Fl_Choice::handle(event);
    default:
        return Fl_Choice::handle(event);
    }
}

// ---- Slider --------------------------------------------------------------------------------

namespace {
constexpr int kHandleR = 6; // slider handle radius
constexpr int kTrackH = 4;  // slider track thickness
} // namespace

Slider::Slider(int X, int Y, int W, int H, const char* label) : Fl_Slider(X, Y, W, H, label) {
    type(FL_HORIZONTAL);
    box(FL_FLAT_BOX);
    color(toFl(activePalette().controlBg));
    selection_color(toFl(activePalette().accent));
    clear_visible_focus();
}

void Slider::draw() {
    const Palette& p = activePalette();
    const bool on = active_r(); // see Dropdown::draw: active_r() so an inactive ancestor greys us too
    // Clear our cell with a RAW fill, NOT draw_box(): FLTK's box path greys the colour through
    // fl_inactive() when the widget is deactivated (Fl::box_color), which turned the normally
    // invisible panelBg cell into a dark box on a disabled slider (user, dark mode). fl_rectf
    // never greys, so the disabled slider keeps the panel ground and only the fill/handle mute.
    const common::Color8 cell = m_cellColorSet ? m_cellColor : p.panelBg; // ground this slider clears to
    fl_color(toFl(cell));
    fl_rectf(x(), y(), w(), h());

    const int cy = y() + h() / 2;
    const int tx0 = x() + kHandleR;
    const int tw = std::max(1, w() - 2 * kHandleR);
    double t = (maximum() != minimum()) ? (value() - minimum()) / (maximum() - minimum()) : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const int hx = tx0 + static_cast<int>(std::lround(t * tw));

    const int ty0 = cy - kTrackH / 2;
    fl_color(toFl(p.border)); // hairline frame: an empty controlBg track is invisible on light panels
    fl_rect(tx0 - 1, ty0 - 1, tw + 2, kTrackH + 2);
    fl_color(toFl(p.controlBg)); // track
    fl_rectf(tx0, ty0, tw, kTrackH);
    const common::Color8 fillCol = on ? p.accent : p.textMuted;
    fl_color(toFl(fillCol)); // filled portion
    fl_rectf(tx0, ty0, hx - tx0, kTrackH);

    // The handle: an anti-aliased disc (fl_pie is stair-stepped — §12 chrome polish), a
    // panel-coloured rim lifting a text-coloured core off the track. The under-sampler
    // reproduces the geometry just drawn, so the patch blits opaquely.
    const auto under = [&](int ux, int uy) -> common::Color8 {
        if (ux >= tx0 - 1 && ux <= tx0 + tw && uy >= ty0 - 1 && uy <= ty0 + kTrackH) {
            if (ux == tx0 - 1 || ux == tx0 + tw || uy == ty0 - 1 || uy == ty0 + kTrackH)
                return p.border;
            return ux < hx ? fillCol : p.controlBg;
        }
        return cell;
    };
    drawAAPrims(hx - kHandleR - 1, cy - kHandleR - 1, 2 * kHandleR + 2, 2 * kHandleR + 2, under,
                {{hx + 0.0, cy + 0.0, kHandleR + 0.0, 0.0, cell},
                 {hx + 0.0, cy + 0.0, kHandleR - 1.0, 0.0, on ? p.text : p.textMuted}});
}

int Slider::handle(int event) {
    switch (event) {
    case FL_ENTER:
        m_hover = true;
        return 1;
    case FL_LEAVE:
        m_hover = false;
        return 1;
    case FL_PUSH:
    case FL_DRAG:
    case FL_RELEASE: {
        // Map the cursor to a value through our own geometry, then drive Fl_Valuator's machinery so
        // the callback/when() (and the opacity-coalescing) behave exactly as for a stock slider.
        const int tx0 = x() + kHandleR;
        const int tw = std::max(1, w() - 2 * kHandleR);
        double t = std::clamp(static_cast<double>(Fl::event_x() - tx0) / tw, 0.0, 1.0);
        const double v = minimum() + t * (maximum() - minimum());
        if (event == FL_PUSH)
            handle_push();
        handle_drag(v); // clamps to range + applies step + fires FL_WHEN_CHANGED
        if (event == FL_RELEASE)
            handle_release(); // fires FL_WHEN_RELEASE[_ALWAYS]
        redraw();
        return 1;
    }
    default:
        return Fl_Slider::handle(event);
    }
}

// ---- ProgressBar -----------------------------------------------------------------------------

namespace {
constexpr double kSweepTickS = 1.0 / 30.0; // indeterminate animation cadence
constexpr double kSweepS = 1.4;            // seconds per full sweep traverse
constexpr int kBarRadius = 4;              // end-cap rounding (clamped to h/2 in draw)
} // namespace

ProgressBar::ProgressBar(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

ProgressBar::~ProgressBar() {
    Fl::remove_timeout(tick, this);
}

void ProgressBar::setFraction(double f) {
    f = std::clamp(f, 0.0, 1.0);
    if (!m_indet && f == m_fraction)
        return;
    m_indet = false; // the pending tick (if any) lapses on its next fire
    m_fraction = f;
    redraw();
}

void ProgressBar::setIndeterminate() {
    if (!m_indet) {
        m_indet = true;
        m_phase = 0.0;
        redraw();
    }
    arm();
}

void ProgressBar::arm() {
    if (!m_timer && m_indet) {
        m_timer = true;
        Fl::add_timeout(kSweepTickS, tick, this);
    }
}

void ProgressBar::tick(void* self) {
    auto* bar = static_cast<ProgressBar*>(self);
    // Let the chain lapse while the bar is determinate or not on screen; draw() re-arms when the
    // sweep becomes visible again (the Marquee convention: animate only what is actually shown).
    if (!bar->m_indet || !bar->visible_r()) {
        bar->m_timer = false;
        return;
    }
    bar->m_phase += kSweepTickS / kSweepS;
    if (bar->m_phase >= 1.0)
        bar->m_phase -= 1.0;
    bar->redraw();
    Fl::repeat_timeout(kSweepTickS, tick, self);
}

void ProgressBar::indeterminateSpan(double phase, int trackW, int& x0, int& x1) {
    // Segment ~30% of the track (at least 24 px); its leading edge travels trackW + seg px per
    // cycle, so the segment enters from off-left and leaves off-right with a brief dark beat.
    const int seg = std::max(24, trackW * 3 / 10);
    const int lead = static_cast<int>(std::lround(phase * (trackW + seg)));
    x0 = std::clamp(lead - seg, 0, std::max(0, trackW));
    x1 = std::clamp(lead, x0, std::max(0, trackW));
}

void ProgressBar::draw() {
    const Palette& pal = activePalette();
    const common::Color8 ground = m_cellColorSet ? m_cellColor : pal.panelBg;
    fl_rectf(x(), y(), w(), h(), toFl(ground)); // erase the cell first (draw() owns its ground)

    const int rad = std::min(kBarRadius, h() / 2);
    fl_color(toFl(pal.controlBg));
    fl_rounded_rectf(x(), y(), w(), h(), rad);

    int fx0 = 0;
    int fx1 = 0;
    if (m_indet) {
        indeterminateSpan(m_phase, w(), fx0, fx1);
    } else {
        fx1 = static_cast<int>(std::lround(m_fraction * w()));
    }
    if (fx1 > fx0) {
        // Clip the full rounded track shape to the lit span: the fill keeps the track's rounded
        // caps at either end and a square edge mid-track (the ui::Slider fill look).
        fl_push_clip(x() + fx0, y(), fx1 - fx0, h());
        fl_color(toFl(pal.accent));
        fl_rounded_rectf(x(), y(), w(), h(), rad);
        fl_pop_clip();
    }
    fl_color(toFl(pal.border));
    fl_rounded_rect(x(), y(), w(), h(), rad);

    if (m_indet)
        arm(); // freshly (re)shown while sweeping: restart the lapsed tick chain
}

// ---- Dial ----------------------------------------------------------------------------------

namespace {
// Distance from point (px,py) to the segment a->b (needle capsule; fl_line is stair-stepped).
double distToSeg(double px, double py, double ax, double ay, double bx, double by) {
    const double vx = bx - ax, vy = by - ay;
    const double len2 = vx * vx + vy * vy;
    double t = len2 > 1e-9 ? ((px - ax) * vx + (py - ay) * vy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(px - (ax + t * vx), py - (ay + t * vy));
}
common::Color8 overCol(common::Color8 base, common::Color8 top, double cov) {
    const double a = std::clamp(cov, 0.0, 1.0);
    const auto ch = [&](std::uint8_t b, std::uint8_t t) {
        return static_cast<std::uint8_t>(std::lround(t * a + b * (1.0 - a)));
    };
    return {ch(base.r, top.r), ch(base.g, top.g), ch(base.b, top.b), 255};
}
} // namespace

Dial::Dial(int X, int Y, int W, int H, const char* label) : Fl_Valuator(X, Y, W, H, label) {
    range(0, 360);
    step(1);
    // visible_focus stays ON (the Fl_Widget default): it is what lets take_focus() deliver the
    // arrow-key nudge. FL_FOCUS below still refuses focus unless we asked for it at PUSH, so the
    // knob is not a Tab-stop at rest -- the ScrubSlider's in-place-edit arrangement.
}

common::Color8 Dial::cellColor() const {
    return m_cellColorSet ? m_cellColor : activePalette().panelBg;
}

double wrapDialValue(double v, double mn, double mx) {
    const double span = mx - mn;
    if (span >= 359.9) { // a full turn: wrap so 360 == 0 and negatives fold in
        double t = std::fmod(v - mn, span);
        if (t < 0.0) t += span;
        return mn + t;
    }
    return std::clamp(v, std::min(mn, mx), std::max(mn, mx));
}

double Dial::clampWrap(double v) const { return wrapDialValue(v, minimum(), maximum()); }

double Dial::screenAngleAt(double dx, double dy, double snap) {
    double deg = std::atan2(dx, -dy) * 180.0 / M_PI; // 0 = up (12 o'clock), growing clockwise
    if (snap > 0.0) deg = std::round(deg / snap) * snap;
    deg = std::fmod(deg, 360.0);
    if (deg < 0.0) deg += 360.0;
    // ⚠ The half-open range is not free: a tiny negative (-1e-14, which snapping and atan2 both
    // produce at the seam) plus 360 ROUNDS TO exactly 360.0 in double, so the fold above can hand
    // back the one value the range excludes. 360 and 0 are the same needle; return the canonical one.
    if (deg >= 360.0) deg = 0.0;
    return deg;
}

double Dial::resetValue() const { return m_hasDefault ? clampWrap(m_default) : minimum(); }

void Dial::draw() {
    const Palette& p = activePalette();
    const bool on = active_r();
    const common::Color8 ground = cellColor();

    const int W = w(), H = h();
    const double lcx = W / 2.0, lcy = H / 2.0;      // widget-local centre
    const double R = std::min(W, H) / 2.0 - 2.0;    // knob radius (2px breathing room)
    const double rimHalf = 0.9;                      // rim half-thickness
    const double needleHalf = std::max(1.0, R * 0.09);
    const double hubR = std::max(2.0, R * 0.16);

    const common::Color8 faceC = p.controlBg;
    // Only the RIM picks up the accent on hover/drag; the needle keeps its resting colour (turning
    // both read too busy, user feedback).
    const bool live = on && (m_hover || m_drag || Fl::focus() == this);
    const common::Color8 rimC = live ? p.accent : p.border;
    const common::Color8 needleC = on ? p.text : p.textMuted;

    // Needle: value degrees clockwise from 12 o'clock (0 = up, 90 = right), plus any zero offset the
    // host set to remap a foreign angle convention onto the true screen direction (setZeroOffset).
    const double a = (m_zeroOffset + clampWrap(value())) * M_PI / 180.0;
    // With a readout in the face the needle becomes a RIM TICK (an outer stub) so the digits are not
    // struck through; without one it is the full hub-to-rim spoke the pattern flyout ships.
    const double n0 = m_showReadout ? 0.62 : 0.0;
    const double sx = lcx + R * n0 * std::sin(a);
    const double sy = lcy - R * n0 * std::cos(a);
    const double nx = lcx + R * 0.80 * std::sin(a);
    const double ny = lcy - R * 0.80 * std::cos(a);

    std::vector<unsigned char> buf(static_cast<std::size_t>(W) * H * 3);
    for (int by = 0; by < H; ++by)
        for (int bx = 0; bx < W; ++bx) {
            const double fx = bx + 0.5, fy = by + 0.5;
            const double d = std::hypot(fx - lcx, fy - lcy);
            common::Color8 c = ground;
            c = overCol(c, faceC, R - d + 0.5);                       // filled face
            c = overCol(c, rimC, rimHalf - std::abs(d - R) + 0.5);    // outer rim
            c = overCol(c, needleC,                                    // needle capsule / rim tick
                        needleHalf - distToSeg(fx, fy, sx, sy, nx, ny) + 0.5);
            if (!m_showReadout)
                c = overCol(c, needleC, hubR - d + 0.5);              // centre hub
            const std::size_t o = (static_cast<std::size_t>(by) * W + bx) * 3;
            buf[o] = c.r;
            buf[o + 1] = c.g;
            buf[o + 2] = c.b;
        }
    Fl_RGB_Image img(buf.data(), W, H, 3);
    img.draw(x(), y());

    if (m_showReadout) {
        // The live angle in the face. fl_draw's glyphs are the host font's -- already AA'd -- so it
        // goes on TOP of the composed knob rather than into the coverage buffer.
        const std::string text = formatFieldNumber(clampWrap(value()), step()) + "\xC2\xB0";
        fl_font(FL_HELVETICA, static_cast<int>(std::clamp(R * 0.46, 9.0, 13.0)));
        fl_color(toFl(needleC));
        fl_draw(text.c_str(), x(), y(), W, H, FL_ALIGN_CENTER);
    }
}

void Dial::pointNeedleAt(int eventX, int eventY) {
    const double dx = eventX - (x() + w() / 2.0);
    const double dy = eventY - (y() + h() / 2.0);
    if (std::abs(dx) < 1e-6 && std::abs(dy) < 1e-6) return;  // dead centre: keep the current value
    const bool shift = (Fl::event_state() & FL_SHIFT) != 0;
    const double deg = screenAngleAt(dx, dy, shift ? m_snapDeg : 0.0);
    // The angle is recomputed from the CURSOR every event (never integrated), which is what makes
    // dragging round and round wrap instead of piling up against an endpoint.
    // Back out the zero offset so we drive value() in the host's convention (offset 0 == a no-op).
    // Fl_Valuator::round() applies step(); qualified so it can never bind to ::round from <cmath>.
    handle_drag(clampWrap(Fl_Valuator::round(deg - m_zeroOffset)));
    redraw();
}

void Dial::nudge(double delta) {
    if (delta == 0.0) return;
    handle_push();
    handle_drag(clampWrap(Fl_Valuator::round(value() + delta)));
    handle_release();
    redraw();
}

int Dial::handle(int event) {
    switch (event) {
    case FL_ENTER:
    case FL_MOVE:
        // The knob is grabbable across its whole face, so the whole cell shows the hand (set on the
        // hover TRANSITION only -- the SwatchChip/SwatchButton convention).
        if (!m_hover) {
            m_hover = true;
            if (window() != nullptr)
                window()->cursor(active_r() ? FL_CURSOR_HAND : FL_CURSOR_DEFAULT);
            redraw();
        }
        return 1;
    case FL_LEAVE:
        if (m_hover) {
            m_hover = false;
            if (window() != nullptr) window()->cursor(FL_CURSOR_DEFAULT);
            redraw();
        }
        return 1;
    case FL_PUSH:
        // Middle / Ctrl click resets to the configured default, or the range minimum when none was
        // set (parity with the scrub slider; a symmetric range like [-180,180] needs 0, not the min).
        if (Fl::event_button() == FL_MIDDLE_MOUSE || (Fl::event_state() & FL_CTRL)) {
            handle_push();
            handle_drag(resetValue());
            handle_release();
            redraw();
            return 1;
        }
        m_wantFocus = true; // let FL_FOCUS through for this take_focus() only...
        take_focus();       // ...so the arrow keys nudge the knob the user just grabbed
        m_wantFocus = false; // ...and a Tab landing here is still refused
        m_drag = true;
        handle_push();
        pointNeedleAt(Fl::event_x(), Fl::event_y());
        redraw();
        return 1;
    case FL_DRAG:
        pointNeedleAt(Fl::event_x(), Fl::event_y());
        return 1;
    case FL_RELEASE:
        m_drag = false;
        handle_release();
        redraw();
        return 1;

    // Click-to-focus without becoming a Tab-stop: Fl_Group's navigation calls take_focus(), whose
    // FL_FOCUS we refuse unless our own PUSH asked for it.
    case FL_FOCUS:
        return m_wantFocus ? 1 : 0;
    case FL_UNFOCUS:
        m_wantFocus = false;
        redraw();
        return 1;

    case FL_MOUSEWHEEL: {
        if (!Fl::event_inside(this)) return 0;  // don't hijack scrolling (scroll_slider lesson)
        const double s = step() > 0 ? step() : 1.0;
        nudge(-Fl::event_dy() * s * ((Fl::event_state() & FL_SHIFT) ? 10.0 : 1.0));
        return 1;
    }
    case FL_KEYBOARD: {
        if (Fl::focus() != this || !active_r()) return 0;
        const double s = step() > 0 ? step() : 1.0;
        const bool shift = (Fl::event_state() & FL_SHIFT) != 0;
        // Shift walks the snap grid (the same 15 deg a Shift-drag lands on), so the modifier means
        // one thing on this control however you drive it.
        const double coarse = m_snapDeg > 0.0 ? m_snapDeg : s * 10.0;
        const double d = shift ? coarse : s;
        switch (Fl::event_key()) {
        case FL_Up:
        case FL_Right:
            nudge(d);
            return 1;
        case FL_Down:
        case FL_Left:
            nudge(-d);
            return 1;
        case FL_Home:
            nudge(clampWrap(resetValue()) - value());
            return 1;
        case FL_End: // the opposite pointing -- the other half of "flip it round"
            nudge(clampWrap(resetValue() + 180.0) - value());
            return 1;
        default:
            return 0;
        }
    }
    default:
        return Fl_Valuator::handle(event);
    }
}

// ---- ScrollView ----------------------------------------------------------------------------

namespace {
constexpr int kGrabPad = 3; // inset of the grab from the trough's long edges
} // namespace

ScrollView::ScrollView(int X, int Y, int W, int H, const char* label)
    : Fl_Scroll(X, Y, W, H, label) {
    neutralizeBar(scrollbar);
    neutralizeBar(hscrollbar);
    m_themeSub = ThemeSubscription([this] { reapplyTheme(); });
}

int ScrollView::scrollbarGutter(int contentHeight) const {
    const int size = scrollbar_size() != 0 ? scrollbar_size() : Fl::scrollbar_size();
    return contentHeight > h() ? size : 0;
}

// Make a stock scrollbar paint nothing but a flat trough-coloured rectangle: MOSAIC_FLAT_BOX fills
// with color() (no border, no fallback to FL_UP_BOX the way FL_NO_BOX would), and the same boxtype
// for the knob/arrow-squares + trough-coloured selection/label hides the stock 3-D knob and arrow
// glyphs. The bar stays fully interactive; only its pixels are blanked so paintGrab() owns the look.
void ScrollView::neutralizeBar(Fl_Scrollbar& sb) {
    sb.box(MOSAIC_FLAT_BOX);
    sb.slider(MOSAIC_FLAT_BOX);
    sb.color(color());
    sb.selection_color(color());
    sb.labelcolor(color()); // the arrow triangles draw in labelcolor() -> invisible
}

common::Color8 ScrollView::troughColor() const {
    common::Color8 c{};
    Fl::get_color(color(), c.r, c.g, c.b); // resolves both semantic (windowBg) and concrete colours
    c.a = 255;
    return c;
}

void ScrollView::reapplyTheme() {
    neutralizeBar(scrollbar);
    neutralizeBar(hscrollbar);
    redraw();
}

// The grab's travel track along the bar's long axis: the trough minus a small end inset at each
// end, so the grab keeps a little gap from the bar's ends (never kissing the panel beyond it). The
// same span feeds both painting and hit-testing, so the visible grab and the drag stay in lockstep.
void ScrollView::trackSpan(const Fl_Scrollbar& sb, bool vertical, int& start, int& len) const {
    const int along = vertical ? sb.h() : sb.w();
    start = (vertical ? sb.y() : sb.x()) + kGrabPad;
    len = std::max(1, along - 2 * kGrabPad);
}

// Thumb pixel length: the fractional slider_size() over the track, floored to a comfortably
// grabbable minimum and capped at the track length.
int ScrollView::thumbLen(const Fl_Scrollbar& sb, bool vertical) const {
    int start;
    int len;
    trackSpan(sb, vertical, start, len);
    const int across = vertical ? sb.w() : sb.h();
    const int S = static_cast<int>(std::lround(sb.slider_size() * len));
    const int minT = std::min(len, std::max(across + 6, 24));
    return std::clamp(S, minT, len);
}

// The grab rectangle (the visible pill) at the bar's current value, inset from all four edges.
void ScrollView::grabRect(const Fl_Scrollbar& sb, bool vertical, int& gx, int& gy, int& gw,
                          int& gh) const {
    int start;
    int len;
    trackSpan(sb, vertical, start, len);
    const int S = thumbLen(sb, vertical);
    const double range = sb.maximum() - sb.minimum();
    double val = range > 0 ? (sb.value() - sb.minimum()) / range : 0.0;
    val = std::clamp(val, 0.0, 1.0);
    const int pos = static_cast<int>(std::lround(val * (len - S)));
    if (vertical) {
        gx = sb.x() + kGrabPad;
        gw = sb.w() - 2 * kGrabPad;
        gy = start + pos;
        gh = S;
    } else {
        gy = sb.y() + kGrabPad;
        gh = sb.h() - 2 * kGrabPad;
        gx = start + pos;
        gw = S;
    }
}

// Paint the themed grab over a (neutralised) stock bar. The grab is an anti-aliased rounded
// rectangle (a vertical pill for a tall thumb, collapsing to a horizontal pill / disc when short)
// built from a single AA patch: the four corners are discs, the cross-shaped interior is filled by
// the under-sampler. fl_pie/fl_arc are stair-stepped, hence drawAAPrims (see Slider + §12 polish).
void ScrollView::paintGrab(const Fl_Scrollbar& sb, bool vertical, bool hover, bool drag) {
    if (!sb.visible())
        return;
    const Palette& p = activePalette();
    const common::Color8 bg = troughColor();
    // Clear the WHOLE bar rect ourselves: the stock bar only repaints its middle track on a partial
    // (non-FL_DAMAGE_ALL) redraw, so a grab that reaches into the former arrow-button end zones would
    // otherwise leave stale pixels there when it moves (paired with the full-redraw-on-scroll below).
    fl_color(toFl(bg));
    fl_rectf(sb.x(), sb.y(), sb.w(), sb.h());

    int gx;
    int gy;
    int gw;
    int gh;
    grabRect(sb, vertical, gx, gy, gw, gh);
    if (gw <= 0 || gh <= 0)
        return;

    // Grab colour. Dark mode: the control* greys sit a step lighter than the trough -> visible (the
    // approved look). Light mode: those greys are near-white (controlBg is pure white) and vanish on
    // the near-white trough, so step toward the text colour for a clearly visible mid-grey instead.
    common::Color8 grab;
    if (drag)
        grab = p.accent;
    else if (p.dark)
        grab = hover ? p.controlHover : p.controlBg;
    else
        grab = mix(bg, p.text, hover ? 0.42F : 0.30F);
    const double cr = std::min(gw, gh) / 2.0; // corner radius -> a true pill at the short axis
    const int top = gy;
    const int bot = gy + gh;
    const int left = gx;
    const int right = gx + gw;
    const auto under = [&](int ux, int uy) -> common::Color8 {
        const bool inV = uy >= top + cr && uy <= bot - cr;    // middle rows: full width
        const bool inH = ux >= left + cr && ux <= right - cr; // middle cols: full height
        return (inV || inH) ? grab : bg;
    };
    drawAAPrims(gx, gy, gw, gh, under,
                {{left + cr, top + cr, cr, 0.0, grab},
                 {right - cr, top + cr, cr, 0.0, grab},
                 {left + cr, bot - cr, cr, 0.0, grab},
                 {right - cr, bot - cr, cr, 0.0, grab}});
}

void ScrollView::draw() {
    Fl_Scroll::draw(); // children (clipped) + the neutralised bars painting flat panelBg rects
    paintGrab(scrollbar, true, m_vHover, m_vDrag);
    paintGrab(hscrollbar, false, m_hHover, m_hDrag);
}

bool ScrollView::inBar(const Fl_Scrollbar& sb) const {
    return sb.visible() && Fl::event_x() >= sb.x() && Fl::event_x() < sb.x() + sb.w() &&
           Fl::event_y() >= sb.y() && Fl::event_y() < sb.y() + sb.h();
}

// Begin a drag on FL_PUSH if the cursor is over `sb`. A press on the grab drags it in place; a
// press in the trough jumps the grab to the cursor (centred) and then drags -- both run through
// dragBarTo(). Returns true when the press is consumed (so handle() does not forward it to the
// base bar, whose arrow-reserved hit-testing we are deliberately bypassing).
bool ScrollView::startBarDrag(Fl_Scrollbar& sb, bool vertical) {
    if (!inBar(sb))
        return false;
    int gx;
    int gy;
    int gw;
    int gh;
    grabRect(sb, vertical, gx, gy, gw, gh);
    const int evAlong = vertical ? Fl::event_y() : Fl::event_x();
    const int grabStart = vertical ? gy : gx;
    const int S = vertical ? gh : gw;
    const bool onGrab = evAlong >= grabStart && evAlong < grabStart + S;
    m_dragOffset = onGrab ? (evAlong - grabStart) : (S / 2); // trough press: grab by its centre
    if (vertical)
        m_vDrag = true;
    else
        m_hDrag = true;
    dragBarTo(sb, vertical);
    return true;
}

void ScrollView::dragBarTo(Fl_Scrollbar& sb, bool vertical) {
    int start;
    int len;
    trackSpan(sb, vertical, start, len);
    const int S = thumbLen(sb, vertical);
    const int travel = len - S;
    const int evAlong = vertical ? Fl::event_y() : Fl::event_x();
    const int grabStart = std::clamp(evAlong - m_dragOffset, start, start + travel);
    const double val = travel > 0 ? static_cast<double>(grabStart - start) / travel : 0.0;
    const int target =
        static_cast<int>(std::lround(sb.minimum() + val * (sb.maximum() - sb.minimum())));
    if (vertical)
        scroll_to(xposition(), target);
    else
        scroll_to(target, yposition());
    redraw();
}

int ScrollView::handle(int event) {
    switch (event) {
    case FL_PUSH:
        // Bar presses are ours (full-length grab + accent); everything else falls to the base.
        if (startBarDrag(scrollbar, true) || startBarDrag(hscrollbar, false))
            return 1;
        break;
    case FL_DRAG:
        if (m_vDrag) {
            dragBarTo(scrollbar, true);
            return 1;
        }
        if (m_hDrag) {
            dragBarTo(hscrollbar, false);
            return 1;
        }
        break;
    case FL_RELEASE:
        if (m_vDrag || m_hDrag) {
            m_vDrag = false;
            m_hDrag = false;
            redraw(); // drop the accent
            return 1;
        }
        break;
    case FL_ENTER:
    case FL_MOVE: {
        const bool vH = inBar(scrollbar);
        const bool hH = inBar(hscrollbar);
        if (vH != m_vHover || hH != m_hHover) {
            m_vHover = vH;
            m_hHover = hH;
            redraw();
        }
        break; // still let the base see the move (child hover, etc.)
    }
    case FL_LEAVE:
        if (m_vHover || m_hHover) {
            m_vHover = false;
            m_hHover = false;
            redraw();
        }
        break;
    default:
        break;
    }
    // Any base-driven scroll (mouse-wheel, keys) must repaint the whole bar, not the optimised
    // partial region -- otherwise the grab leaves stale pixels in the former arrow-button end zones.
    const int yp = yposition();
    const int xp = xposition();
    const int r = Fl_Scroll::handle(event);
    if (yposition() != yp || xposition() != xp)
        redraw();
    return r;
}

// ---- Marquee / ScrollingLabel -------------------------------------------------------------------

namespace {
constexpr double kScrollTickS = 0.04; // ~25 fps while scrolling
constexpr float kScrollStepPx = 1.2F;
constexpr int kEndPauseTicks = 30; // ~1.2 s dwell at either end
} // namespace

void Marquee::reset() {
    m_offset = 0.0F;
    m_dir = 1.0F;
    m_pauseTicks = kEndPauseTicks;
}

void Marquee::stop() {
    if (m_timer) {
        Fl::remove_timeout(tick, this);
        m_timer = false;
    }
}

double Marquee::oneScrollSeconds(double overflowPx) {
    if (overflowPx <= 0.0)
        return 0.0; // fits: no scroll
    const double scrollS = (overflowPx / kScrollStepPx) * kScrollTickS; // ticks to cross * period
    const double pauseS = kEndPauseTicks * kScrollTickS;
    return 2.0 * pauseS + scrollS; // start dwell + the traverse + the end dwell
}

void Marquee::tick(void* self) {
    auto* m = static_cast<Marquee*>(self);
    m->m_timer = false;
    if (m->m_host == nullptr || !m->m_host->visible_r())
        return; // surface hidden: stop ticking; the next draw() restarts the scroll
    if (m->m_pauseTicks > 0) {
        --m->m_pauseTicks;
    } else {
        m->m_offset += m->m_dir * kScrollStepPx;
        if (m->m_offset <= 0.0F || m->m_offset >= m->m_maxOff) {
            m->m_offset = std::clamp(m->m_offset, 0.0F, std::max(m->m_maxOff, 0.0F));
            m->m_dir = -m->m_dir;
            m->m_pauseTicks = kEndPauseTicks;
        }
    }
    m->m_host->redraw();
    Fl::add_timeout(kScrollTickS, tick, self);
    m->m_timer = true;
}

void Marquee::draw(Fl_Widget* host, const char* text, int x, int y, int w, int h,
                   bool rightAlignWhenFits) {
    m_host = host;
    if (text == nullptr)
        text = "";
    if (m_lastText != text) { // fresh text starts at rest (also the hovered-row switch)
        m_lastText = text;
        reset();
    }
    const float textW = static_cast<float>(fl_width(text));
    m_maxOff = textW - static_cast<float>(w);
    const int baseline = y + (h + fl_height()) / 2 - fl_descent();
    if (m_maxOff <= 0.0F) {
        stop(); // fits: a plain label flush to the chosen edge
        fl_draw(text, rightAlignWhenFits ? x + w - static_cast<int>(textW) : x, baseline);
        return;
    }
    fl_push_clip(x, y, w, h);
    fl_draw(text, x - static_cast<int>(m_offset), baseline);
    fl_pop_clip();
    if (!m_timer && host->visible_r()) {
        Fl::add_timeout(kScrollTickS, tick, this);
        m_timer = true;
    }
}

// ---- GalleryCard: preview art + title (+ subtitle) tile (see widgets.hpp) -----------------------
GalleryCard::GalleryCard(int X, int Y, int W, int H, std::string title, std::string subtitle)
    : Fl_Widget(X, Y, W, H), m_title(std::move(title)), m_subtitle(std::move(subtitle)) {
    copy_tooltip(m_subtitle.empty() ? m_title.c_str()
                                    : (m_title + "\n" + m_subtitle).c_str()); // full text, always
}

void GalleryCard::setThumbnail(const common::Image& thumb) {
    // Pack straight RGB: fl_draw_image with depth 4 misreads channels on some backends (the
    // pinned magenta-tint trap); the sources are opaque (checker pre-composited), so alpha drops.
    m_thumbW = static_cast<int>(thumb.width);
    m_thumbH = static_cast<int>(thumb.height);
    m_thumbRgb.assign(static_cast<std::size_t>(m_thumbW) * m_thumbH * 3, 0);
    for (std::size_t p = 0; p < static_cast<std::size_t>(m_thumbW) * m_thumbH; ++p) {
        m_thumbRgb[p * 3 + 0] = thumb.rgba[p * 4 + 0];
        m_thumbRgb[p * 3 + 1] = thumb.rgba[p * 4 + 1];
        m_thumbRgb[p * 3 + 2] = thumb.rgba[p * 4 + 2];
    }
    m_previewFn = nullptr;
    redraw();
}

void GalleryCard::setPreviewFn(PreviewFn fn) {
    m_previewFn = std::move(fn);
    m_thumbRgb.clear();
    m_thumbW = m_thumbH = 0;
    redraw();
}

void GalleryCard::setSelected(bool s) {
    if (s != m_selected) {
        m_selected = s;
        redraw();
    }
}

void GalleryCard::setGroundColor(common::Color8 c) {
    m_ground = c;
    m_hasGround = true;
    redraw();
}

int GalleryCard::previewHeight() const {
    return h() - kTitleH - (m_subtitle.empty() ? 0 : kSubtitleH);
}

int GalleryCard::handle(int event) {
    switch (event) {
    case FL_ENTER:
        m_hover = true;
        redraw();
        return 1;
    case FL_LEAVE:
        m_hover = false;
        redraw();
        return 1;
    case FL_PUSH:
        // Claim the whole pair; double-click state is read HERE (event_clicks() is a press-time
        // counter) and acted on at RELEASE so a drag-off still cancels cleanly. A right-click
        // opens the card's context menu (at PUSH, the menu convention) and never selects.
        m_pushWasRight = Fl::event_button() == FL_RIGHT_MOUSE;
        if (m_pushWasRight) {
            if (m_onContextMenu)
                m_onContextMenu();
            return 1;
        }
        m_pushWasDouble = Fl::event_clicks() != 0;
        return 1;
    case FL_RELEASE:
        if (m_pushWasRight)
            return 1; // the context menu owns this pair
        if (Fl::event_inside(this)) {
            if (m_onSelect)
                m_onSelect(); // a double click selects too -- activation implies selection
            if (m_pushWasDouble && m_onActivate)
                m_onActivate();
        }
        return 1;
    default:
        return Fl_Widget::handle(event);
    }
}

void GalleryCard::draw() {
    const Palette& p = activePalette();
    const int ph = previewHeight();
    const common::Color8 bg = m_hover && !m_selected ? p.controlHover : p.controlBg;

    // Preview cell: ground, then the art (image blit or placeholder callback), clipped.
    fl_color(toFl(bg));
    fl_rectf(x(), y(), w(), ph);
    if (!m_thumbRgb.empty()) {
        const int tx = x() + (w() - m_thumbW) / 2;
        const int ty = y() + (ph - m_thumbH) / 2;
        fl_push_clip(x() + 1, y() + 1, w() - 2, ph - 2);
        fl_draw_image(m_thumbRgb.data(), tx, ty, m_thumbW, m_thumbH, 3, 0); // packed RGB
        fl_pop_clip();
    } else if (m_previewFn) {
        constexpr int m = 8;
        fl_push_clip(x() + m, y() + m, w() - 2 * m, ph - 2 * m);
        m_previewFn(x() + m, y() + m, w() - 2 * m, ph - 2 * m, bg);
        fl_pop_clip();
    }
    if (m_selected) { // 2px accent ring (the OptionCard language)
        fl_color(toFl(p.accent));
        fl_rect(x(), y(), w(), ph);
        fl_rect(x() + 1, y() + 1, w() - 2, ph - 2);
    } else {
        fl_color(toFl(p.border));
        fl_rect(x(), y(), w(), ph);
    }

    // Label strips: ERASE first (double-buffer keeps prior pixels; unerased text thickens).
    const common::Color8 ground = m_hasGround ? m_ground : p.windowBg;
    fl_color(toFl(ground));
    fl_rectf(x(), y() + ph, w(), h() - ph);
    // A line longer than its strip (a deep location path, a long file name) SCROLLS inside it
    // instead of centring past the card edges: text drawn outside the widget lands where no
    // draw() ever erases, so every repaint boldened it further (user 2026-07-22).
    fl_font(FL_HELVETICA, 12);
    fl_color(toFl(m_selected ? p.accent : p.text));
    drawStrip(m_titleMarquee, m_title, y() + ph, kTitleH);
    if (!m_subtitle.empty()) {
        fl_font(FL_HELVETICA, 10);
        fl_color(toFl(p.textMuted));
        drawStrip(m_subtitleMarquee, m_subtitle, y() + ph + kTitleH, kSubtitleH);
    }
}

void GalleryCard::drawStrip(Marquee& m, const std::string& text, int ty, int th) {
    const int tx = x() + 2;
    const int tw = w() - 4;
    // draw_symbols=0: titles are arbitrary FILENAMES -- a leading '@' must not become glyph art.
    if (fl_width(text.c_str()) <= static_cast<double>(tw)) {
        fl_draw(text.c_str(), tx, ty, tw, th, FL_ALIGN_CENTER, nullptr, 0);
        return;
    }
    m.draw(this, text.c_str(), tx, ty, tw, th); // clips to the strip; ticks while overflowing
}

ScrollingLabel::ScrollingLabel(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {
    labelfont(FL_HELVETICA);
    labelsize(11);
    color(toFl(activePalette().panelBg)); // the erase colour; callers on other surfaces override
}

void ScrollingLabel::setText(const std::string& text) {
    m_text = text;
    copy_tooltip(text.c_str()); // the full text is always reachable, scrolled or not
    m_marquee.reset();
    redraw();
}

double ScrollingLabel::oneScrollSeconds() const {
    fl_font(labelfont(), labelsize());
    return Marquee::oneScrollSeconds(static_cast<double>(fl_width(m_text.c_str())) -
                                     static_cast<double>(w()));
}

void ScrollingLabel::draw() {
    // Erase first: an unboxed widget redraw paints over its own previous frame, so a scrolling
    // text would smear into a trail (user-reported, memorably, as "a won game of Solitaire").
    fl_color(color());
    fl_rectf(x(), y(), w(), h());
    fl_font(labelfont(), labelsize());
    fl_color(labelcolor());
    fl_push_clip(x(), y(), w(), h());
    m_marquee.draw(this, m_text.c_str(), x(), y(), w(), h(),
                   /*rightAlignWhenFits=*/m_align == Align::Right);
    fl_pop_clip();
}

// ---- HexField ------------------------------------------------------------------------------

HexField::HexField(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    box(MOSAIC_PANEL_BOX);
    color(FL_BACKGROUND2_COLOR); // = controlBg; follows a runtime re-theme
    begin();
    auto* prefix = new Fl_Box(X + 8, Y + 1, 14, H - 2, "#");
    prefix->box(FL_NO_BOX);
    prefix->labelfont(FL_SCREEN);
    prefix->labelsize(12);
    prefix->labelcolor(FL_INACTIVE_COLOR); // = textMuted; follows a runtime re-theme
    prefix->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    m_input = new TextInput(X + 24, Y + 2, W - 32, H - 4);
    m_input->box(FL_FLAT_BOX);
    m_input->color(FL_BACKGROUND2_COLOR); // semantic = controlBg/text; follow a runtime re-theme
    m_input->textcolor(FL_FOREGROUND_COLOR);
    m_input->cursor_color(FL_FOREGROUND_COLOR);
    m_input->textfont(FL_SCREEN); // fixed-width hex glyphs
    m_input->textsize(12);
    end();
}

std::string ellipsizeToWidth(const std::string& text, int maxWidth) {
    if (maxWidth <= 0)
        return {};
    if (text.empty() || fl_width(text.c_str()) <= static_cast<double>(maxWidth))
        return text;
    static constexpr const char* kEllipsis = "\xE2\x80\xA6"; // U+2026, as in the "Open as layer…" menus
    const double ellipsisW = fl_width(kEllipsis);
    if (ellipsisW > static_cast<double>(maxWidth))
        return {};
    // Walk back one CODEPOINT at a time (never mid-sequence) until the prefix plus the ellipsis fits.
    std::size_t cut = text.size();
    while (cut > 0) {
        --cut;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut; // step off a UTF-8 continuation byte onto the codepoint's lead byte
        const std::string head = text.substr(0, cut);
        if (fl_width(head.c_str()) + ellipsisW <= static_cast<double>(maxWidth))
            return head + kEllipsis;
    }
    return kEllipsis; // room for the mark, but not for a single glyph before it
}

namespace {

int hexVal(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// RFC-3986 percent-decoding, lenient: a malformed escape passes through verbatim.
std::string percentDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexVal(s[i + 1]);
            const int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

} // namespace

void centerWindowOver(Fl_Window& win, Fl_Window* host) {
    // The pointer's screen is where the user IS -- every dialog-raising flow (a menu click, a
    // file drop, a launch) has the pointer at the user's locus, and a modal anywhere else just
    // reads as "nothing happened".
    int sx = 0;
    int sy = 0;
    int sw = 0;
    int sh = 0;
    Fl::screen_work_area(sx, sy, sw, sh); // the work area of the screen holding the pointer
    if (host != nullptr && host->shown() != 0) {
        const int px = host->x() + (host->w() - win.w()) / 2;
        const int py = host->y() + (host->h() - win.h()) / 2;
        // Trust the host-centred spot only when it lands on the pointer's screen. A JUST-SHOWN
        // host still reports 0,0 -- x()/y() are real only once the WM has placed it and the
        // ConfigureNotify has landed -- so a dialog raised during startup (a file argument that
        // fails to open, a crash-restore offer) would centre off phantom coordinates and open on
        // the wrong display (verified live 2026-07-16: settled coords are correct, pre-placement
        // coords are 0,0). A stale host and a host parked on another monitor both resolve the
        // same way: show the dialog where the user actually is.
        const int cx = px + win.w() / 2;
        const int cy = py + win.h() / 2;
        if (cx >= sx && cx < sx + sw && cy >= sy && cy < sy + sh) {
            win.position(px, py);
            return;
        }
    }
    win.position(sx + (sw - win.w()) / 2, sy + (sh - win.h()) / 2);
}

std::vector<std::string> localPathsFromDndText(std::string_view text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos) ? text.substr(pos)
                                                               : text.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (line.empty())
            continue;

        constexpr std::string_view kFileScheme = "file://";
        if (line.substr(0, kFileScheme.size()) == kFileScheme) {
            std::string_view rest = line.substr(kFileScheme.size());
            if (!rest.empty() && rest.front() != '/') {
                // file://host/path -- skip the authority (in practice "localhost")
                const std::size_t slash = rest.find('/');
                if (slash == std::string_view::npos)
                    continue;
                rest = rest.substr(slash);
            }
            std::string path = percentDecode(rest);
            if (!path.empty())
                out.push_back(std::move(path));
            continue;
        }
        if (line.find("://") != std::string_view::npos)
            continue; // some other scheme (http, ...) -- not a local file
        if (line.front() == '/')
            out.emplace_back(line); // a bare absolute path drop
    }
    return out;
}

std::optional<std::string> firstLocalPathFromDndText(std::string_view text) {
    std::vector<std::string> all = localPathsFromDndText(text);
    if (all.empty())
        return std::nullopt;
    return std::move(all.front());
}

// ---- NumberField -------------------------------------------------------------------------------

NumberField::NumberField(int X, int Y, int W, int H, const char* label)
    : FloatInput(X, Y, W, H, label) {
    box(MOSAIC_INPUT_BOX);                 // hairline frame + text padding (no kissing the outline)
    color(FL_BACKGROUND2_COLOR);           // semantic = controlBg/text; follows a runtime re-theme
    textcolor(FL_FOREGROUND_COLOR);
    cursor_color(FL_FOREGROUND_COLOR);
    textsize(12);
    // FL_NORMAL_INPUT, not the float type: Fl_Input's per-keystroke float filter would reject the
    // arithmetic set ("1024*2" could not even be typed). handle() below owns the filtering.
    type(FL_NORMAL_INPUT);
}

int NumberField::handle(int event) {
    if (event == FL_KEYBOARD && Fl::event_length() == 1 &&
        (Fl::event_state() & (FL_CTRL | FL_ALT | FL_META)) == 0) {
        const char c = Fl::event_text()[0];
        // A typed ',' inserts '.' so a comma-locale user can enter their separator (S16-l).
        if (c == ',') {
            replace(insert_position(), mark(), ".", 1); // insert like a normal keystroke
            return 1;
        }
        // Digits and the arithmetic set type straight in; any other printable is swallowed so
        // the field still reads as numeric (the float filter this class turned off in the ctor).
        if (std::isprint(static_cast<unsigned char>(c)) != 0 &&
            std::strchr("0123456789.eE+-*/() ", c) == nullptr)
            return 1;
    }
    if (event == FL_UNFOCUS)
        commitExpression();
    return FloatInput::handle(event);
}

void NumberField::commitExpression() {
    const char* t = value();
    if (t == nullptr || *t == '\0')
        return;
    const std::optional<double> v = evaluateFieldExpression(t);
    if (!v.has_value())
        return; // malformed/mid-typing text stays as typed; the caller's parse falls back
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.10g", *v); // enough digits that pixel sizes never round
    if (std::strcmp(buf, t) != 0) {
        value(buf);
        do_callback(); // exactly like a typed edit, so live summaries/links re-derive
    }
}

std::string formatFieldNumber(double value, double step) {
    // The INTEGER arm can call std::to_chars directly -- libc++ only ever gated the
    // floating-point overloads. The fractional arm must not: libc++ marks to_chars(double)
    // unavailable below macOS 13.3, so calling it is a hard compile error at the project's 11.0
    // deployment floor. common::gToString is the house wrapper for exactly this (it drops to
    // snprintf on libc++), and it is what the rest of this file already effectively uses -- the
    // NumberField commit path a few lines up formats with "%.10g", which is gToString's default.
    if (step >= 1.0) {
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof buf, std::lround(value));
        return r.ec == std::errc() ? std::string(buf, r.ptr) : std::string();
    }
    return common::gToString(value);
}

namespace {

// Recursive-descent evaluator behind evaluateFieldExpression: expr := term (('+'|'-') term)*;
// term := factor (('*'|'/') factor)*; factor := number | '(' expr ')' | ('+'|'-') factor.
struct ExprParser {
    std::string_view s;
    std::size_t i = 0;
    bool ok = true;

    void skipWs() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
    }
    bool eat(char c) {
        skipWs();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    double number() {
        skipWs();
        std::string digits; // collected so a comma decimal can become a dot for strtod
        bool any = false;
        while (i < s.size() &&
               (std::isdigit(static_cast<unsigned char>(s[i])) != 0 || s[i] == '.' ||
                s[i] == ',')) {
            digits += s[i] == ',' ? '.' : s[i];
            any = true;
            ++i;
        }
        if (!any) {
            ok = false;
            return 0.0;
        }
        // fromChars, NOT strtod: `digits` has already been normalised to a '.' decimal, but strtod
        // reads the ACTIVE LC_NUMERIC -- which i18n::init() moves to the user's locale. On any
        // comma locale (most of Europe) strtod("8.5") stops at the '.', returns 8, and the
        // whole-string check below then rejected the input: every decimal typed into a number
        // field was refused. fromChars is locale-independent and '.'-only, which is exactly the
        // contract the normalisation above was written for. (S54)
        double v = 0.0;
        const auto r = mosaic::common::fromChars(digits.data(), digits.data() + digits.size(), v);
        if (r.ec != std::errc() || r.ptr != digits.data() + digits.size())
            ok = false; // "1.2.3" and friends
        return v;
    }
    double factor() {
        if (eat('-'))
            return -factor();
        if (eat('+'))
            return factor();
        if (eat('(')) {
            const double v = expr();
            if (!eat(')'))
                ok = false;
            return v;
        }
        return number();
    }
    double term() {
        double v = factor();
        while (ok) {
            if (eat('*')) {
                v *= factor();
            } else if (eat('/')) {
                const double d = factor();
                if (d == 0.0)
                    ok = false;
                else
                    v /= d;
            } else {
                break;
            }
        }
        return v;
    }
    double expr() {
        double v = term();
        while (ok) {
            if (eat('+'))
                v += term();
            else if (eat('-'))
                v -= term();
            else
                break;
        }
        return v;
    }
};

} // namespace

std::optional<double> evaluateFieldExpression(std::string_view text) {
    ExprParser p{text};
    const double v = p.expr();
    p.skipWs();
    if (!p.ok || p.i != p.s.size() || !std::isfinite(v))
        return std::nullopt;
    return v;
}

bool parseFieldNumber(const char* text, double& out) {
    if (text == nullptr)
        return false;
    // Arithmetic first (a plain number evaluates to itself), so every NumberField caller reads
    // "2*3" as 6. The historical prefix parse stays as the fallback ("12abc" -> 12).
    if (const std::optional<double> v = evaluateFieldExpression(text)) {
        out = *v;
        return true;
    }
    std::string s(text);
    std::replace(s.begin(), s.end(), ',', '.');
    std::string_view sv(s);
    if (!sv.empty() && sv.front() == '+')
        sv.remove_prefix(1);
    return mosaic::common::fromChars(sv.data(), sv.data() + sv.size(), out).ec == std::errc();
}

// ---- TimeDrum ------------------------------------------------------------------------------------

TimeDrum::TimeDrum(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

void TimeDrum::setValue(double hours) {
    double h = std::fmod(hours, 24.0);
    if (h < 0.0)
        h += 24.0;
    const int totalMin = static_cast<int>(std::lround(h * 60.0 / kMinuteStep)) * kMinuteStep;
    m_hour = (totalMin / 60) % 24;
    m_minute = totalMin % 60;
    redraw();
}

double TimeDrum::value() const {
    return m_hour + m_minute / 60.0;
}

void TimeDrum::spin(int drum, int delta) {
    if (drum == 0) {
        m_hour = ((m_hour + delta) % 24 + 24) % 24;
    } else {
        int m = m_minute + delta * kMinuteStep;
        // Minutes carry into the hour like the mechanical thing.
        while (m < 0) {
            m += 60;
            m_hour = (m_hour + 23) % 24;
        }
        while (m >= 60) {
            m -= 60;
            m_hour = (m_hour + 1) % 24;
        }
        m_minute = m;
    }
    redraw();
    do_callback();
}

int TimeDrum::drumAt(int eventX) const {
    const int lx = eventX - x();
    if (lx < 0 || lx >= w())
        return -1;
    return lx < w() / 2 ? 0 : 1;
}

void TimeDrum::draw() {
    const Palette& pal = activePalette();
    fl_color(fl_rgb_color(pal.controlBg.r, pal.controlBg.g, pal.controlBg.b));
    fl_rectf(x(), y(), w(), h());

    const int cellH = h() / 3; // three visible rows: the neighbours peek above and below
    const int midY = y() + h() / 2;
    const int halfW = w() / 2;
    char buf[8];

    const auto row = [&](int drum, int offset, int value) {
        std::snprintf(buf, sizeof(buf), "%02d", value);
        const bool centre = offset == 0;
        fl_font(FL_HELVETICA, centre ? 14 : 11);
        const common::Color8 c = centre ? pal.text : pal.textMuted;
        fl_color(fl_rgb_color(c.r, c.g, c.b));
        const int rx = x() + drum * halfW;
        fl_draw(buf, rx, midY + offset * cellH - cellH / 2, halfW, cellH, FL_ALIGN_CENTER);
    };
    row(0, -1, (m_hour + 23) % 24);
    row(0, 0, m_hour);
    row(0, +1, (m_hour + 1) % 24);
    const int mSteps = 60 / kMinuteStep;
    const int mIdx = m_minute / kMinuteStep;
    row(1, -1, ((mIdx + mSteps - 1) % mSteps) * kMinuteStep);
    row(1, 0, m_minute);
    row(1, +1, ((mIdx + 1) % mSteps) * kMinuteStep);

    // The colon between the drums, centre row only (it does not spin).
    fl_font(FL_HELVETICA, 14);
    fl_color(fl_rgb_color(pal.text.r, pal.text.g, pal.text.b));
    fl_draw(":", x(), midY - cellH / 2, w(), cellH, FL_ALIGN_CENTER);

    // The drum window: hairlines bracketing the centre band, a soft hover wash behind the
    // hovered drum, and the widget outline.
    if (m_hoverDrum >= 0 && active_r()) {
        const common::Color8 hcol = pal.controlHover;
        fl_color(fl_rgb_color(hcol.r, hcol.g, hcol.b));
        fl_rect(x() + m_hoverDrum * halfW + 2, midY - cellH / 2, halfW - 4, cellH);
    }
    fl_color(fl_rgb_color(pal.border.r, pal.border.g, pal.border.b));
    fl_line(x() + 2, midY - cellH / 2, x() + w() - 3, midY - cellH / 2);
    fl_line(x() + 2, midY + cellH / 2, x() + w() - 3, midY + cellH / 2);
    fl_rect(x(), y(), w(), h());
}

int TimeDrum::handle(int event) {
    constexpr int kStepPx = 12; // drag px per step: slow enough to land on a value
    switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
        const int over = drumAt(Fl::event_x());
        if (over != m_hoverDrum) {
            m_hoverDrum = over;
            redraw();
        }
        return 1;
    }
    case FL_LEAVE:
        if (m_hoverDrum != -1) {
            m_hoverDrum = -1;
            redraw();
        }
        return 1;
    case FL_PUSH:
        m_dragDrum = drumAt(Fl::event_x());
        m_lastY = Fl::event_y();
        m_accum = 0;
        return m_dragDrum >= 0 ? 1 : 0;
    case FL_DRAG: {
        if (m_dragDrum < 0)
            return 1;
        m_accum += m_lastY - Fl::event_y(); // drag UP = the drum rolls forward
        m_lastY = Fl::event_y();
        while (m_accum >= kStepPx) {
            m_accum -= kStepPx;
            spin(m_dragDrum, +1);
        }
        while (m_accum <= -kStepPx) {
            m_accum += kStepPx;
            spin(m_dragDrum, -1);
        }
        return 1;
    }
    case FL_RELEASE:
        m_dragDrum = -1;
        return 1;
    case FL_MOUSEWHEEL: {
        if (!Fl::event_inside(this))
            return 0; // never hijack a wheel meant for a sibling (the ScrubSlider lesson)
        const int over = drumAt(Fl::event_x());
        if (over < 0)
            return 0;
        spin(over, Fl::event_dy() < 0 ? +1 : -1);
        return 1;
    }
    default:
        return Fl_Widget::handle(event);
    }
}

} // namespace mosaic::ui
