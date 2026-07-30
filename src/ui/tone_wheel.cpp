#include "ui/tone_wheel.hpp"

#include "ui/theme.hpp"
#include "ui/widgets.hpp" // drawAAArcs: the rim ring + puck, anti-aliased (fl_arc is not)

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace mosaic::ui {

namespace {

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// Hue (turns, red at 0, y-up counter-clockwise) + saturation -> RGB, HSV-style at full value.
// The disc reads as "which way does the image shift": vivid rim, neutral centre.
void hueToRgb(double hue, double sat, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    const double h = (hue - std::floor(hue)) * 6.0;
    const int i = static_cast<int>(h);
    const double f = h - i;
    const auto mix = [&](double v) {
        return static_cast<std::uint8_t>(std::lround(255.0 * (1.0 - sat * (1.0 - v))));
    };
    const std::uint8_t hi = 255;
    const std::uint8_t lo = mix(0.0);
    const std::uint8_t up = mix(f);
    const std::uint8_t dn = mix(1.0 - f);
    switch (i) {
        case 0: r = hi; g = up; b = lo; break;
        case 1: r = dn; g = hi; b = lo; break;
        case 2: r = lo; g = hi; b = up; break;
        case 3: r = lo; g = dn; b = hi; break;
        case 4: r = up; g = lo; b = hi; break;
        default: r = hi; g = lo; b = dn; break;
    }
}

} // namespace

common::Color8 wheelHue(double turns, double sat) {
    std::uint8_t r = 0, g = 0, b = 0;
    hueToRgb(turns, std::clamp(sat, 0.0, 1.0), r, g, b);
    return {r, g, b, 255};
}

ToneWheel::ToneWheel(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

int ToneWheel::radius() const {
    return std::max(8, std::min(w(), h()) / 2 - 2);
}

void ToneWheel::setValue(double x, double y) {
    const double len = std::hypot(x, y);
    if (len > 1.0) {
        x /= len;
        y /= len;
    }
    if (x == m_x && y == m_y)
        return;
    m_x = x;
    m_y = y;
    redraw();
}

void ToneWheel::rebuildDisc() {
    const int r = radius();
    const int d = 2 * r;
    m_disc.assign(static_cast<std::size_t>(d) * d * 3, 0);
    for (int py = 0; py < d; ++py) {
        for (int px = 0; px < d; ++px) {
            const double dx = (px + 0.5 - r);
            const double dy = (py + 0.5 - r);
            const double dist = std::hypot(dx, dy) / r; // 0 centre .. 1 rim
            std::uint8_t cr = m_ground.r, cg = m_ground.g, cb = m_ground.b;
            if (dist < 1.0) {
                // Screen y grows down; the plane is y-up (+y toward green), so negate dy.
                const double hue = std::atan2(-dy, dx) / (2.0 * std::numbers::pi);
                hueToRgb(hue, std::min(1.0, dist), cr, cg, cb);
            }
            // 1px AA ramp at the rim: blend the disc colour over the ground by coverage.
            const double cov = std::clamp((1.0 - dist) * r, 0.0, 1.0);
            const std::size_t p = (static_cast<std::size_t>(py) * d + px) * 3;
            m_disc[p + 0] =
                static_cast<std::uint8_t>(std::lround(cr * cov + m_ground.r * (1.0 - cov)));
            m_disc[p + 1] =
                static_cast<std::uint8_t>(std::lround(cg * cov + m_ground.g * (1.0 - cov)));
            m_disc[p + 2] =
                static_cast<std::uint8_t>(std::lround(cb * cov + m_ground.b * (1.0 - cov)));
        }
    }
    m_discFor = r;
    m_discGround = m_ground;
}

void ToneWheel::draw() {
    const Palette& pal = activePalette();
    // Erase the whole box first ([[mosaic-ui-gotchas]]), then blit the cached disc.
    fl_color(toFl(m_ground));
    fl_rectf(x(), y(), w(), h());
    const int r = radius();
    if (m_discFor != r || !(m_discGround.r == m_ground.r && m_discGround.g == m_ground.g &&
                            m_discGround.b == m_ground.b))
        rebuildDisc();
    const int cx = x() + w() / 2;
    const int cy = y() + h() / 2;
    fl_draw_image(m_disc.data(), cx - r, cy - r, 2 * r, 2 * r, 3, 0);
    // Crosshair ticks at the neutral centre (visible while the puck is elsewhere). These now go down
    // BEFORE the rim ring rather than after it -- the ring is at the rim and the ticks span 3 px
    // about the centre, so for any radius this widget can have (>= 8) they cannot touch, and the
    // swap changes no pixel. It buys the one thing that matters: the ring and the puck can then
    // share a patch whose `under` restates the ticks it covers.
    fl_color(toFl(pal.textMuted));
    fl_line(cx - 3, cy, cx + 3, cy);
    fl_line(cx, cy - 3, cx, cy + 3);
    // The puck: screen y grows down, the value plane is y-up.
    const int ux = cx + static_cast<int>(std::lround(m_x * (r - 3)));
    const int uy = cy - static_cast<int>(std::lround(m_y * (r - 3)));
    // The hairline rim ring (so the disc reads on any ground) and the three puck discs compose in
    // ONE anti-aliased patch. They have to: at full deflection the puck reaches the rim, and a
    // separate opaque puck patch would bite a notch out of the ring it overlapped. `under` is what
    // the widget has already painted -- the cached hue disc where the patch is over it, the tick
    // crosshair where it crosses that, and the cell ground beyond the disc.
    const int d = 2 * r;
    const auto under = [&](int px, int py) -> common::Color8 {
        if ((py == cy && px >= cx - 3 && px <= cx + 3) ||
            (px == cx && py >= cy - 3 && py <= cy + 3))
            return pal.textMuted;
        const int lx = px - (cx - r);
        const int ly = py - (cy - r);
        if (lx < 0 || ly < 0 || lx >= d || ly >= d)
            return m_ground;
        const std::size_t o = (static_cast<std::size_t>(ly) * d + lx) * 3;
        return {m_disc[o], m_disc[o + 1], m_disc[o + 2], 255};
    };
    drawAAArcs(under, {aaArcFromBox(cx - r, cy - r, d, d, 0.0, 360.0, 1.0, pal.border),
                       aaPieFromBox(ux - 4, uy - 4, 9, 9, 0.0, 360.0, {0, 0, 0, 255}),
                       aaPieFromBox(ux - 3, uy - 3, 7, 7, 0.0, 360.0, {255, 255, 255, 255}),
                       aaPieFromBox(ux - 1, uy - 1, 3, 3, 0.0, 360.0, {0, 0, 0, 255})});
}

void ToneWheel::dragToValue(double x, double y) {
    const double len = std::hypot(x, y);
    if (len > 1.0) {
        x /= len;
        y /= len;
    }
    if (x == m_x && y == m_y)
        return;
    m_x = x;
    m_y = y;
    redraw();
    if (m_onChange)
        m_onChange(m_x, m_y);
}

void ToneWheel::dragTo(int ex, int ey) {
    const int r = radius();
    dragToValue((ex - (x() + w() / 2.0)) / std::max(1, r - 3),
                -(ey - (y() + h() / 2.0)) / std::max(1, r - 3));
}

int ToneWheel::handle(int event) {
    switch (event) {
        // Claim the whole PUSH/DRAG/RELEASE gesture ([[mosaic-ui-gotchas]]: an unclaimed release
        // falls to the child under the pointer).
        case FL_PUSH:
            if (Fl::event_button() != FL_LEFT_MOUSE)
                return 0;
            if (Fl::event_clicks() > 0) { // double-click recentres (the neutral shift)
                m_x = 0.0;
                m_y = 0.0;
                redraw();
                if (m_onChange)
                    m_onChange(0.0, 0.0);
                return 1;
            }
            dragTo(Fl::event_x(), Fl::event_y());
            return 1;
        case FL_DRAG:
            dragTo(Fl::event_x(), Fl::event_y());
            return 1;
        case FL_RELEASE:
            return 1;
        default:
            return Fl_Widget::handle(event);
    }
}

} // namespace mosaic::ui
