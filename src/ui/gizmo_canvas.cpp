#include "ui/gizmo_canvas.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::ui {

using common::Vec2;

GizmoCanvas::GizmoCanvas(int w, int h, common::Color8 ground)
    : m_w(w), m_h(h), m_px(4u * static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {
    for (std::size_t i = 0; i < m_px.size(); i += 4) {
        m_px[i + 0] = ground.r;
        m_px[i + 1] = ground.g;
        m_px[i + 2] = ground.b;
        m_px[i + 3] = 255;
    }
}

void GizmoCanvas::blitImage(const common::Image& img, int ox, int oy) {
    for (std::uint32_t sy = 0; sy < img.height; ++sy) {
        const int dy = oy + static_cast<int>(sy);
        if (dy < 0 || dy >= m_h) continue;
        for (std::uint32_t sx = 0; sx < img.width; ++sx) {
            const int dx = ox + static_cast<int>(sx);
            if (dx < 0 || dx >= m_w) continue;
            const std::size_t s = (static_cast<std::size_t>(sy) * img.width + sx) * 4;
            const float a = img.rgba[s + 3] / 255.0f;
            if (a <= 0.0f) continue;
            std::uint8_t* d = &m_px[(static_cast<std::size_t>(dy) * m_w + dx) * 4];
            for (int c = 0; c < 3; ++c)
                d[c] = static_cast<std::uint8_t>(
                    std::lround(img.rgba[s + c] * a + d[c] * (1.0f - a)));
        }
    }
}

void GizmoCanvas::checker(int x0, int y0, int x1, int y1, common::Color8 a, common::Color8 b,
                          int cell) {
    const int ix0 = std::max(0, x0), iy0 = std::max(0, y0);
    const int ix1 = std::min(m_w, x1), iy1 = std::min(m_h, y1);
    const int c = std::max(1, cell);
    for (int py = iy0; py < iy1; ++py)
        for (int px = ix0; px < ix1; ++px) {
            const bool odd = (((px - x0) / c) + ((py - y0) / c)) & 1;
            const common::Color8 col = odd ? b : a;
            std::uint8_t* d = &m_px[(static_cast<std::size_t>(py) * m_w + px) * 4];
            d[0] = col.r;
            d[1] = col.g;
            d[2] = col.b;
            d[3] = 255;
        }
}

void GizmoCanvas::stroke(Vec2 a, Vec2 b, double width, common::Color8 c, float alpha) {
    const double r = width * 0.5;
    forBox(std::min(a.x, b.x) - r - 1, std::min(a.y, b.y) - r - 1, std::max(a.x, b.x) + r + 1,
           std::max(a.y, b.y) + r + 1,
           [&](double px, double py) { return r - segDist({px, py}, a, b); }, c, alpha);
}

void GizmoCanvas::fillDisc(Vec2 ctr, double r, common::Color8 c, float alpha) {
    forBox(ctr.x - r - 1, ctr.y - r - 1, ctr.x + r + 1, ctr.y + r + 1,
           [&](double px, double py) { return r - (Vec2{px, py} - ctr).length(); }, c, alpha);
}

void GizmoCanvas::strokeCircle(Vec2 ctr, double r, double width, common::Color8 c, float alpha) {
    const double hw = width * 0.5;
    forBox(ctr.x - r - hw - 1, ctr.y - r - hw - 1, ctr.x + r + hw + 1, ctr.y + r + hw + 1,
           [&](double px, double py) { return hw - std::abs((Vec2{px, py} - ctr).length() - r); },
           c, alpha);
}

void GizmoCanvas::fillSquare(Vec2 ctr, double half, common::Color8 c, float alpha) {
    forBox(ctr.x - half - 1, ctr.y - half - 1, ctr.x + half + 1, ctr.y + half + 1,
           [&](double px, double py) {
               return half - std::max(std::abs(px - ctr.x), std::abs(py - ctr.y));
           },
           c, alpha);
}

void GizmoCanvas::fillDiamond(Vec2 ctr, double r, common::Color8 c, float alpha) {
    forBox(ctr.x - r - 1, ctr.y - r - 1, ctr.x + r + 1, ctr.y + r + 1,
           [&](double px, double py) {
               return (r - (std::abs(px - ctr.x) + std::abs(py - ctr.y))) * 0.7071 + 0.29;
           },
           c, alpha);
}

double GizmoCanvas::segDist(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const double len2 = ab.dot(ab);
    const double t = len2 > 0.0 ? std::clamp((p - a).dot(ab) / len2, 0.0, 1.0) : 0.0;
    return (p - (a + ab * t)).length();
}

template <typename SdCov>
void GizmoCanvas::forBox(double x0, double y0, double x1, double y1, SdCov inside,
                         common::Color8 c, float alpha) {
    const int ix0 = std::max(0, static_cast<int>(std::floor(x0)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(y0)));
    const int ix1 = std::min(m_w - 1, static_cast<int>(std::ceil(x1)));
    const int iy1 = std::min(m_h - 1, static_cast<int>(std::ceil(y1)));
    for (int py = iy0; py <= iy1; ++py)
        for (int px = ix0; px <= ix1; ++px) {
            const float cov =
                static_cast<float>(std::clamp(inside(px + 0.5, py + 0.5) + 0.5, 0.0, 1.0));
            if (cov <= 0.0f) continue;
            const float a = cov * alpha;
            std::uint8_t* d = &m_px[(static_cast<std::size_t>(py) * m_w + px) * 4];
            for (int ch = 0; ch < 3; ++ch) {
                const std::uint8_t src = ch == 0 ? c.r : ch == 1 ? c.g : c.b;
                d[ch] = static_cast<std::uint8_t>(std::lround(src * a + d[ch] * (1.0f - a)));
            }
        }
}

} // namespace mosaic::ui
