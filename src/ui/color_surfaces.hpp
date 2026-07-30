#pragma once

#include "common/image.hpp"    // Color8
#include "ui/color_models.hpp" // hsvToRgb
#include "ui/theme.hpp"        // Palette, activePalette, drawAAPrims

#include <FL/Fl.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

// The three Affinity-style picking surfaces, factored out of the colour picker (S12) so the compact
// Fill-dialog colour flyout (S39 follow-up) can reuse the exact same widgets at a smaller size: the
// SV field + hue strip, the hue wheel with a rotating HSL triangle, and the hue wheel with an
// inscribed SV square. Each is a plain Fl_Widget that edits a working HSV, reports edits through the
// ordinary FLTK callback (the host reads sat()/val()/hue() and rebuilds its colour), and is fully
// theme-aware. Bodies are inline so two TUs (the picker, the flyout) can share them with no separate
// translation unit; the tiny shared helpers live in `sf::` to avoid clashing with the per-file
// anonymous-namespace toFl/clamp01 helpers elsewhere.
namespace mosaic::ui::sf {
inline Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }
inline float clamp01(float v) { return std::clamp(v, 0.0F, 1.0F); }
inline constexpr float kDegToRad = 3.14159265F / 180.0F;
inline constexpr float kRadToDeg = 180.0F / 3.14159265F;
} // namespace mosaic::ui::sf

namespace mosaic::ui {

// The saturation/value plane at the current hue. The gradient is rendered into a cached RGB
// buffer (rebuilt only when the hue changes) and blitted; a contrast-picked ring marks s/v.
class SvField : public Fl_Widget {
public:
    SvField(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

    void set(float h, float s, float v) {
        m_h = h;
        m_s = sf::clamp01(s);
        m_v = sf::clamp01(v);
        redraw();
    }
    [[nodiscard]] float sat() const { return m_s; }
    [[nodiscard]] float val() const { return m_v; }

protected:
    void draw() override {
        if (!m_img || m_cachedHue != m_h)
            rebuild();
        m_img->draw(x(), y());
        const Palette& pal = activePalette();
        fl_color(sf::toFl(pal.border));
        fl_rect(x(), y(), w(), h());
        // Marker ring at (s, 1-v); pick black over light field regions, white over dark. Drawn
        // anti-aliased over a sampler of what's beneath (§12 chrome polish — fl_arc is jaggy).
        const int px = x() + static_cast<int>(std::lround(m_s * static_cast<float>(w() - 1)));
        const int py = y() + static_cast<int>(std::lround((1.0F - m_v) * static_cast<float>(h() - 1)));
        const common::Color8 ring =
            m_v > 0.65F && m_s < 0.65F ? common::Color8{0, 0, 0, 255} : common::Color8{255, 255, 255, 255};
        const auto under = [&](int ux, int uy) -> common::Color8 {
            const int ix = ux - x();
            const int iy = uy - y();
            if (ix < 0 || iy < 0 || ix >= w() || iy >= h())
                return pal.panelBg;
            if (ix == 0 || iy == 0 || ix == w() - 1 || iy == h() - 1)
                return pal.border; // the frame just drawn
            const std::size_t p = (static_cast<std::size_t>(iy) * w() + ix) * 3;
            return {m_pix[p], m_pix[p + 1], m_pix[p + 2], 255};
        };
        drawAAPrims(px - 5, py - 5, 12, 12, under, {{px + 0.5, py + 0.5, 4.0, 2.0, ring}});
    }

    int handle(int event) override {
        switch (event) {
        case FL_PUSH:
        case FL_DRAG: {
            m_s = sf::clamp01(static_cast<float>(Fl::event_x() - x()) / static_cast<float>(w() - 1));
            m_v = 1.0F - sf::clamp01(static_cast<float>(Fl::event_y() - y()) /
                                     static_cast<float>(h() - 1));
            redraw();
            do_callback();
            return 1;
        }
        case FL_RELEASE:
            return 1;
        default:
            return Fl_Widget::handle(event);
        }
    }

private:
    void rebuild() {
        const int W = w();
        const int H = h();
        m_pix.resize(static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * 3);
        std::size_t i = 0;
        for (int row = 0; row < H; ++row) {
            const float v = 1.0F - static_cast<float>(row) / static_cast<float>(H - 1);
            for (int col = 0; col < W; ++col) {
                const float s = static_cast<float>(col) / static_cast<float>(W - 1);
                const common::Color8 c = hsvToRgb({m_h, s, v});
                m_pix[i++] = c.r;
                m_pix[i++] = c.g;
                m_pix[i++] = c.b;
            }
        }
        m_img = std::make_unique<Fl_RGB_Image>(m_pix.data(), W, H, 3);
        m_cachedHue = m_h;
    }

    float m_h = 0.0F;
    float m_s = 0.0F;
    float m_v = 1.0F;
    float m_cachedHue = -1.0F;
    std::vector<unsigned char> m_pix;
    std::unique_ptr<Fl_RGB_Image> m_img;
};

// The vertical hue gradient strip (0 deg at the top through 360 at the bottom), with a two-tone
// marker line at the current hue. The gradient never changes, so its buffer is built once.
class HueStrip : public Fl_Widget {
public:
    HueStrip(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

    void set(float h) {
        m_h = h;
        redraw();
    }
    [[nodiscard]] float hue() const { return m_h; }

protected:
    void draw() override {
        if (!m_img)
            rebuild();
        m_img->draw(x(), y());
        const Palette& pal = activePalette();
        fl_color(sf::toFl(pal.border));
        fl_rect(x(), y(), w(), h());
        const int py =
            y() + static_cast<int>(std::lround(m_h / 360.0F * static_cast<float>(h() - 1)));
        fl_color(FL_WHITE);
        fl_line(x() + 1, py, x() + w() - 2, py);
        fl_color(FL_BLACK);
        fl_line(x() + 1, py + 1, x() + w() - 2, py + 1);
    }

    int handle(int event) override {
        switch (event) {
        case FL_PUSH:
        case FL_DRAG: {
            const float t =
                sf::clamp01(static_cast<float>(Fl::event_y() - y()) / static_cast<float>(h() - 1));
            m_h = std::min(t * 360.0F, 359.9F); // keep 360 == 0 from snapping the marker back to top
            redraw();
            do_callback();
            return 1;
        }
        case FL_RELEASE:
            return 1;
        default:
            return Fl_Widget::handle(event);
        }
    }

private:
    void rebuild() {
        const int W = w();
        const int H = h();
        m_pix.resize(static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * 3);
        std::size_t i = 0;
        for (int row = 0; row < H; ++row) {
            const common::Color8 c =
                hsvToRgb({static_cast<float>(row) / static_cast<float>(H - 1) * 360.0F, 1.0F, 1.0F});
            for (int col = 0; col < W; ++col) {
                m_pix[i++] = c.r;
                m_pix[i++] = c.g;
                m_pix[i++] = c.b;
            }
        }
        m_img = std::make_unique<Fl_RGB_Image>(m_pix.data(), W, H, 3);
    }

    float m_h = 0.0F;
    std::vector<unsigned char> m_pix;
    std::unique_ptr<Fl_RGB_Image> m_img;
};

// A hue ring with either a rotating HSL triangle or an inscribed SV square inside it — the two
// Affinity-style wheel surfaces from the review. The gradient buffer is rebuilt only when the hue
// changes (the triangle rotates with it; the square's tint follows it); the ring edges get a 1-px
// coverage blend against the panel background so they read round rather than stair-stepped.
// Triangle mapping (the classic GIMP/Krita one): a point's barycentric weights over the
// (pure-hue, white, black) corners give rgb = a*hue + b*white, i.e. v = a + b and s = a / (a + b).
class ColorWheel : public Fl_Widget {
public:
    enum class Style { Triangle, Square };

    ColorWheel(int X, int Y, int W, int H, Style style) : Fl_Widget(X, Y, W, H), m_style(style) {}

    void set(float h, float s, float v) {
        m_h = h;
        m_s = sf::clamp01(s);
        m_v = sf::clamp01(v);
        redraw();
    }
    [[nodiscard]] float hue() const { return m_h; }
    [[nodiscard]] float sat() const { return m_s; }
    [[nodiscard]] float val() const { return m_v; }

protected:
    void draw() override {
        if (!m_img || m_cachedHue != m_h)
            rebuild();
        m_img->draw(x(), y());
        // Markers ride an under-sampler of the cached wheel image, so their rings draw
        // anti-aliased and blit opaquely (§12 chrome polish — fl_arc is jaggy).
        const auto under = [&](int ux, int uy) -> common::Color8 {
            const int ix = ux - x();
            const int iy = uy - y();
            if (ix < 0 || iy < 0 || ix >= w() || iy >= h())
                return activePalette().panelBg;
            const std::size_t p = (static_cast<std::size_t>(iy) * w() + ix) * 3;
            return {m_pix[p], m_pix[p + 1], m_pix[p + 2], 255};
        };
        // Ring marker at the current hue (two-tone: white over black, readable on any hue).
        const float rad = m_h * sf::kDegToRad;
        const float rMid = (rInner() + rOuter()) * 0.5F;
        const int px = x() + static_cast<int>(std::lround(cx() + std::cos(rad) * rMid));
        const int py = y() + static_cast<int>(std::lround(cy() + std::sin(rad) * rMid));
        drawAAPrims(px - 6, py - 6, 13, 13, under,
                    {{px + 0.5, py + 0.5, 4.4, 1.4, {255, 255, 255, 255}},
                     {px + 0.5, py + 0.5, 3.2, 1.4, {0, 0, 0, 255}}});
        // Inner-shape marker at (s, v).
        float mx = 0.0F;
        float my = 0.0F;
        if (m_style == Style::Square) {
            const float half = innerHalf();
            mx = cx() - half + m_s * 2.0F * half;
            my = cy() + half - m_v * 2.0F * half;
        } else {
            float vx[3];
            float vy[3];
            triangleVertices(vx, vy);
            const float a = m_s * m_v;          // pure-hue corner weight
            const float b = m_v * (1.0F - m_s); // white corner weight
            const float c = 1.0F - m_v;         // black corner weight
            mx = a * vx[0] + b * vx[1] + c * vx[2];
            my = a * vy[0] + b * vy[1] + c * vy[2];
        }
        // Two-tone ring (white over black), like the hue marker, so the handle stays visible on any
        // field/triangle colour -- a single-tone ring vanished on light regions in light mode (user).
        const int ix = x() + static_cast<int>(std::lround(mx));
        const int iy = y() + static_cast<int>(std::lround(my));
        drawAAPrims(ix - 6, iy - 6, 13, 13, under,
                    {{ix + 0.5, iy + 0.5, 4.4, 1.4, {255, 255, 255, 255}},
                     {ix + 0.5, iy + 0.5, 3.2, 1.4, {0, 0, 0, 255}}});
    }

    int handle(int event) override {
        const float dx = static_cast<float>(Fl::event_x() - x()) - cx();
        const float dy = static_cast<float>(Fl::event_y() - y()) - cy();
        switch (event) {
        case FL_PUSH: {
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d >= rInner() - 2.0F && d <= rOuter() + 2.0F)
                m_drag = Drag::Ring;
            else if (d < rInner() && insideInner(dx, dy))
                m_drag = Drag::Inner;
            else
                return Fl_Widget::handle(event); // dead corner: the popover swallows it
            applyDrag(dx, dy);
            return 1;
        }
        case FL_DRAG:
            if (m_drag != Drag::None)
                applyDrag(dx, dy);
            return 1;
        case FL_RELEASE:
            m_drag = Drag::None;
            return 1;
        default:
            return Fl_Widget::handle(event);
        }
    }

private:
    enum class Drag { None, Ring, Inner };

    [[nodiscard]] float cx() const { return static_cast<float>(w()) * 0.5F; }
    [[nodiscard]] float cy() const { return static_cast<float>(h()) * 0.5F; }
    [[nodiscard]] float rOuter() const { return std::min(cx(), cy()) - 1.0F; }
    [[nodiscard]] float rInner() const { return rOuter() - 14.0F; }
    [[nodiscard]] float innerHalf() const { return (rInner() - 3.0F) * 0.70710678F; }

    // Vertices in widget-local coordinates: [0] = pure hue (tracks the ring), [1] = white,
    // [2] = black, equally spaced on the circle just inside the ring.
    void triangleVertices(float (&vx)[3], float (&vy)[3]) const {
        const float r = rInner() - 3.0F;
        for (int k = 0; k < 3; ++k) {
            const float ang = (m_h + 120.0F * static_cast<float>(k)) * sf::kDegToRad;
            vx[k] = cx() + std::cos(ang) * r;
            vy[k] = cy() + std::sin(ang) * r;
        }
    }

    static void bary(float px, float py, const float (&vx)[3], const float (&vy)[3], float& a,
                     float& b, float& c) {
        const float den =
            (vy[1] - vy[2]) * (vx[0] - vx[2]) + (vx[2] - vx[1]) * (vy[0] - vy[2]);
        a = ((vy[1] - vy[2]) * (px - vx[2]) + (vx[2] - vx[1]) * (py - vy[2])) / den;
        b = ((vy[2] - vy[0]) * (px - vx[2]) + (vx[0] - vx[2]) * (py - vy[2])) / den;
        c = 1.0F - a - b;
    }

    [[nodiscard]] bool insideInner(float dx, float dy) const {
        if (m_style == Style::Square) {
            const float half = innerHalf();
            return std::abs(dx) <= half && std::abs(dy) <= half;
        }
        float vx[3];
        float vy[3];
        triangleVertices(vx, vy);
        float a = 0.0F;
        float b = 0.0F;
        float c = 0.0F;
        bary(dx + cx(), dy + cy(), vx, vy, a, b, c);
        return a >= -0.001F && b >= -0.001F && c >= -0.001F;
    }

    void applyDrag(float dx, float dy) {
        if (m_drag == Drag::Ring) {
            float deg = std::atan2(dy, dx) * sf::kRadToDeg;
            if (deg < 0.0F)
                deg += 360.0F;
            m_h = std::min(deg, 359.9F);
        } else if (m_style == Style::Square) {
            const float half = innerHalf();
            m_s = sf::clamp01((dx + half) / (2.0F * half));
            m_v = sf::clamp01((half - dy) / (2.0F * half));
        } else {
            float vx[3];
            float vy[3];
            triangleVertices(vx, vy);
            float a = 0.0F;
            float b = 0.0F;
            float c = 0.0F;
            bary(dx + cx(), dy + cy(), vx, vy, a, b, c);
            // A drag may wander outside the triangle: clamp the weights and renormalise so the
            // marker slides along the nearest edge instead of escaping.
            a = std::max(a, 0.0F);
            b = std::max(b, 0.0F);
            c = std::max(c, 0.0F);
            const float sum = a + b + c;
            a /= sum;
            b /= sum;
            m_v = sf::clamp01(a + b);
            if (a + b > 0.0001F)
                m_s = sf::clamp01(a / (a + b)); // s is undefined at v = 0: keep the last one
        }
        redraw();
        do_callback();
    }

    void rebuild() {
        const Palette& pal = activePalette();
        const int W = w();
        const int H = h();
        m_pix.resize(static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * 3);
        const common::Color8 hueRgb = hsvToRgb({m_h, 1.0F, 1.0F});
        const float bgR = static_cast<float>(pal.panelBg.r);
        const float bgG = static_cast<float>(pal.panelBg.g);
        const float bgB = static_cast<float>(pal.panelBg.b);
        float vx[3];
        float vy[3];
        triangleVertices(vx, vy);
        const float half = innerHalf();
        std::size_t i = 0;
        for (int row = 0; row < H; ++row) {
            for (int col = 0; col < W; ++col) {
                const float px = static_cast<float>(col) + 0.5F;
                const float py = static_cast<float>(row) + 0.5F;
                const float dx = px - cx();
                const float dy = py - cy();
                const float d = std::sqrt(dx * dx + dy * dy);
                float r = bgR;
                float g = bgG;
                float b = bgB;
                // The ring, with a 1-px coverage blend on both edges (cheap anti-aliasing).
                const float cov =
                    std::min(sf::clamp01(rOuter() - d + 0.5F), sf::clamp01(d - rInner() + 0.5F));
                if (cov > 0.0F) {
                    float deg = std::atan2(dy, dx) * sf::kRadToDeg;
                    if (deg < 0.0F)
                        deg += 360.0F;
                    const common::Color8 rc = hsvToRgb({deg, 1.0F, 1.0F});
                    r += (static_cast<float>(rc.r) - r) * cov;
                    g += (static_cast<float>(rc.g) - g) * cov;
                    b += (static_cast<float>(rc.b) - b) * cov;
                } else if (d < rInner()) {
                    if (m_style == Style::Square) {
                        if (std::abs(dx) <= half && std::abs(dy) <= half) {
                            const float s = (dx + half) / (2.0F * half);
                            const float v = (half - dy) / (2.0F * half);
                            const common::Color8 c8 = hsvToRgb({m_h, s, v});
                            r = static_cast<float>(c8.r);
                            g = static_cast<float>(c8.g);
                            b = static_cast<float>(c8.b);
                        }
                    } else {
                        float a = 0.0F;
                        float bb = 0.0F;
                        float c = 0.0F;
                        bary(px, py, vx, vy, a, bb, c);
                        // ~1-px edge anti-aliasing (user-reported jaggies): one barycentric unit
                        // spans the triangle height (1.5 x the vertex radius), so the smallest
                        // weight rescales to an approximate pixel distance from the nearest edge.
                        const float aa =
                            sf::clamp01(std::min({a, bb, c}) * 1.5F * (rInner() - 3.0F) + 0.5F);
                        if (aa > 0.0F) {
                            a = std::max(a, 0.0F);
                            bb = std::max(bb, 0.0F);
                            const float tr = a * static_cast<float>(hueRgb.r) + bb * 255.0F;
                            const float tg = a * static_cast<float>(hueRgb.g) + bb * 255.0F;
                            const float tb = a * static_cast<float>(hueRgb.b) + bb * 255.0F;
                            r += (tr - r) * aa;
                            g += (tg - g) * aa;
                            b += (tb - b) * aa;
                        }
                    }
                }
                // Hairline outlines (light-mode user feedback, 2026-06): both ring edges and
                // the inner shape's boundary — white regions otherwise dissolve into a light
                // panel. Same 1-px coverage blend as the shapes themselves.
                float oc = std::max(sf::clamp01(1.0F - std::abs(d - rOuter())),
                                    sf::clamp01(1.0F - std::abs(d - rInner())));
                if (m_style == Style::Square) {
                    const float sd = std::max(std::abs(dx), std::abs(dy)) - half;
                    oc = std::max(oc, sf::clamp01(1.0F - std::abs(sd)));
                } else {
                    float ea = 0.0F;
                    float eb = 0.0F;
                    float ec = 0.0F;
                    bary(px, py, vx, vy, ea, eb, ec);
                    // min barycentric weight ~ signed pixel distance to the nearest edge (the
                    // same rescale the edge AA above uses); negative far outside => no line.
                    const float edgePx = std::min({ea, eb, ec}) * 1.5F * (rInner() - 3.0F);
                    oc = std::max(oc, sf::clamp01(1.0F - std::abs(edgePx)));
                }
                if (oc > 0.0F) {
                    r += (static_cast<float>(pal.border.r) - r) * oc;
                    g += (static_cast<float>(pal.border.g) - g) * oc;
                    b += (static_cast<float>(pal.border.b) - b) * oc;
                }
                m_pix[i++] = static_cast<unsigned char>(std::lround(std::clamp(r, 0.0F, 255.0F)));
                m_pix[i++] = static_cast<unsigned char>(std::lround(std::clamp(g, 0.0F, 255.0F)));
                m_pix[i++] = static_cast<unsigned char>(std::lround(std::clamp(b, 0.0F, 255.0F)));
            }
        }
        m_img = std::make_unique<Fl_RGB_Image>(m_pix.data(), W, H, 3);
        m_cachedHue = m_h;
    }

    Style m_style;
    Drag m_drag = Drag::None;
    float m_h = 0.0F;
    float m_s = 0.0F;
    float m_v = 1.0F;
    float m_cachedHue = -1.0F;
    std::vector<unsigned char> m_pix;
    std::unique_ptr<Fl_RGB_Image> m_img;
};

} // namespace mosaic::ui
