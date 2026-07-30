#include "core/brush/mask_stroke.hpp"

#include "core/brush/brush_engine.hpp" // dabCoverage -- the shared analytic tip falloff

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {
// Chunky growth for the bounded coverage buffer, so a moving stroke reallocates rarely.
constexpr int kTile = 64;
inline int tileFloor(int v) { return (v >= 0 ? v / kTile : -((-v + kTile - 1) / kTile)) * kTile; }
inline int tileCeil(int v) { return tileFloor(v + kTile - 1); }
} // namespace

void MaskStroke::begin(std::uint32_t w, std::uint32_t h, const MaskStrokeParams& params,
                       common::Vec2 first) {
    m_w = w;
    m_h = h;
    m_params = params;
    m_params.diameter = std::max(params.diameter, 0.1);
    m_step = std::max(0.5, m_params.spacing * m_params.diameter);
    m_ox = m_oy = 0;
    m_cw = m_ch = 0;
    m_coverage.clear();
    m_last = first;
    m_carry = 0.0;
    m_active = true;
    stampDab(first); // the first dab lands immediately, under the press
}

void MaskStroke::extendTo(common::Vec2 sample) {
    if (!m_active)
        return;
    const common::Vec2 d = sample - m_last;
    const double segLen = d.length();
    if (segLen < 1e-9) {
        m_last = sample;
        return;
    }
    const common::Vec2 dir = {d.x / segLen, d.y / segLen};
    // The next dab is `m_step - m_carry` into this segment; then one every `m_step`. Whatever is left
    // after the last dab becomes the carry, so the pattern does not depend on segment granularity.
    double pos = m_step - m_carry;
    if (pos > segLen + 1e-9) {
        m_carry += segLen; // no dab this segment
        m_last = sample;
        return;
    }
    double lastPos = 0.0;
    for (; pos <= segLen + 1e-9; pos += m_step) {
        stampDab({m_last.x + dir.x * pos, m_last.y + dir.y * pos});
        lastPos = pos;
    }
    m_carry = segLen - lastPos;
    m_last = sample;
}

void MaskStroke::ensureCovers(int bx0, int by0, int bx1, int by1) {
    bx0 = std::clamp(bx0, 0, static_cast<int>(m_w));
    by0 = std::clamp(by0, 0, static_cast<int>(m_h));
    bx1 = std::clamp(bx1, 0, static_cast<int>(m_w));
    by1 = std::clamp(by1, 0, static_cast<int>(m_h));
    if (bx1 <= bx0 || by1 <= by0)
        return;

    if (m_cw == 0) { // first allocation
        m_ox = std::max(0, tileFloor(bx0));
        m_oy = std::max(0, tileFloor(by0));
        m_cw = static_cast<std::uint32_t>(std::min<int>(m_w, tileCeil(bx1)) - m_ox);
        m_ch = static_cast<std::uint32_t>(std::min<int>(m_h, tileCeil(by1)) - m_oy);
        m_coverage.assign(static_cast<std::size_t>(m_cw) * m_ch, 0.0f);
        return;
    }
    const int curX1 = m_ox + static_cast<int>(m_cw);
    const int curY1 = m_oy + static_cast<int>(m_ch);
    if (bx0 >= m_ox && by0 >= m_oy && bx1 <= curX1 && by1 <= curY1)
        return; // already covered

    const int nx0 = std::max(0, std::min(m_ox, tileFloor(bx0)));
    const int ny0 = std::max(0, std::min(m_oy, tileFloor(by0)));
    const int nx1 = std::min<int>(m_w, std::max(curX1, tileCeil(bx1)));
    const int ny1 = std::min<int>(m_h, std::max(curY1, tileCeil(by1)));
    const auto ncw = static_cast<std::uint32_t>(nx1 - nx0);
    const auto nch = static_cast<std::uint32_t>(ny1 - ny0);
    std::vector<float> next(static_cast<std::size_t>(ncw) * nch, 0.0f);
    for (std::uint32_t y = 0; y < m_ch; ++y) {
        const std::size_t src = static_cast<std::size_t>(y) * m_cw;
        const std::size_t dst = static_cast<std::size_t>(m_oy - ny0 + y) * ncw + (m_ox - nx0);
        std::copy_n(&m_coverage[src], m_cw, &next[dst]);
    }
    m_coverage.swap(next);
    m_ox = nx0;
    m_oy = ny0;
    m_cw = ncw;
    m_ch = nch;
}

void MaskStroke::stampDab(common::Vec2 center) {
    const double R = m_params.diameter * 0.5;
    const int bx0 = static_cast<int>(std::floor(center.x - R));
    const int by0 = static_cast<int>(std::floor(center.y - R));
    const int bx1 = static_cast<int>(std::ceil(center.x + R));
    const int by1 = static_cast<int>(std::ceil(center.y + R));
    ensureCovers(bx0, by0, bx1, by1);

    const int x0 = std::clamp(bx0, m_ox, m_ox + static_cast<int>(m_cw));
    const int y0 = std::clamp(by0, m_oy, m_oy + static_cast<int>(m_ch));
    const int x1 = std::clamp(bx1, m_ox, m_ox + static_cast<int>(m_cw));
    const int y1 = std::clamp(by1, m_oy, m_oy + static_cast<int>(m_ch));
    const double flow = std::clamp(m_params.flow, 0.0, 1.0);
    for (int y = y0; y < y1; ++y) {
        float* row = &m_coverage[static_cast<std::size_t>(y - m_oy) * m_cw + (x0 - m_ox)];
        for (int x = x0; x < x1; ++x, ++row) {
            const double dx = (x + 0.5) - center.x;
            const double dy = (y + 0.5) - center.y;
            const double cov = dabCoverage(std::sqrt(dx * dx + dy * dy), R, m_params.hardness);
            if (cov <= 0.0)
                continue;
            const double a = flow * cov;
            *row = static_cast<float>(*row + a * (1.0 - *row)); // flow-gated "over" toward 1
        }
    }
}

Selection MaskStroke::toSelection() const {
    if (m_coverage.empty() || m_w == 0 || m_h == 0)
        return {};
    Selection out(m_w, m_h);
    auto& mask = out.data();
    const double opacity = std::clamp(m_params.opacity, 0.0, 1.0);
    bool any = false;
    for (std::uint32_t y = 0; y < m_ch; ++y) {
        const std::size_t src = static_cast<std::size_t>(y) * m_cw;
        const std::size_t dst = static_cast<std::size_t>(m_oy + y) * m_w + m_ox;
        for (std::uint32_t x = 0; x < m_cw; ++x) {
            const double c = std::clamp(static_cast<double>(m_coverage[src + x]), 0.0, 1.0) * opacity;
            if (c <= 0.0)
                continue;
            const auto b = static_cast<std::uint8_t>(std::lround(c * 255.0));
            mask[dst + x] = b;
            if (b > 0)
                any = true;
        }
    }
    if (!any)
        return {}; // deposited nothing -> "no selection"
    return out;
}

} // namespace mosaic::core::brush
