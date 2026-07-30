#include "ui/line_style_preview.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::ui {
namespace {

float sstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ---- the background: the design page's 4th torture-strip tile ("photo noise") -----------------
// The exact GLSL formulas from the artifact bench the styles were judged on: a warm fbm field
// (dark loam -> cream) with fine value-noise grain, drifting at an angle. A real-image stand-in
// -- muted tones, soft structure, luma that wanders across the key range without the eye-biting
// extremes of a synthetic black->white sweep.

// The bench's hash, in double internally: a long-open dialog drifts the domain far enough that a
// float fract() here would quantise and band the noise.
float hash21(float x, float y) {
    double px = x * 123.34, py = y * 345.45;
    px -= std::floor(px);
    py -= std::floor(py);
    const double add = px * (px + 34.345) + py * (py + 34.345);
    px += add;
    py += add;
    const double v = px * py;
    return static_cast<float>(v - std::floor(v));
}

float vnoise(float x, float y) {
    const float ix = std::floor(x), iy = std::floor(y);
    const float fx = x - ix, fy = y - iy;
    const float ux = fx * fx * (3.0f - 2.0f * fx);
    const float uy = fy * fy * (3.0f - 2.0f * fy);
    const float a = hash21(ix, iy), b = hash21(ix + 1.0f, iy);
    const float c = hash21(ix, iy + 1.0f), d = hash21(ix + 1.0f, iy + 1.0f);
    const float top = a + (b - a) * ux;
    return top + ((c + (d - c) * ux) - top) * uy;
}

float fbm(float x, float y) {
    float v = 0.0f, a = 0.5f;
    for (int i = 0; i < 4; ++i) {
        v += a * vnoise(x, y);
        x *= 2.03f;
        y *= 2.03f;
        a *= 0.5f;
    }
    return v;
}

float segDist(float px, float py, float ax, float ay, float bx, float by) {
    const float abx = bx - ax, aby = by - ay;
    const float apx = px - ax, apy = py - ay;
    const float len2 = std::max(abx * abx + aby * aby, 1e-6f);
    const float t = std::clamp((apx * abx + apy * aby) / len2, 0.0f, 1.0f);
    const float dx = apx - abx * t, dy = apy - aby * t;
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

float lineStyleShade(int style, float k, float d, float bg) {
    if (style == 0) {
        // Classic: the hard luminance key on the app's clamp(2.5 - 3d) thin-line profile.
        const float cov = std::clamp(2.5f - 3.0f * d, 0.0f, 1.0f);
        const float line = k < 0.5f ? 1.0f : 0.0f;
        return bg + (line - bg) * cov;
    }
    // The styled lines: plateau core + gaussian rim (canvas_present.comp's styledLine).
    const float covP = std::clamp(2.0f - 1.6f * d, 0.0f, 1.0f);
    const float rim = std::exp(-d * d / 5.12f);
    float c = bg;
    if (style == 1) {  // Shadowed (design 'R'): white core, dark rim keyed to content lightness
        const float a = sstep(0.25f, 0.75f, k);
        c *= 1.0f - rim * 0.65f * a;
        return c + (1.0f - c) * covP;
    }
    // Adaptive (design 'P3d'): white -> graphite dwell -> black; a faint rim shadow escorts the
    // ramp's one unavoidable tone == content crossing.
    const float tone = 1.0f - 0.58f * sstep(0.55f, 0.70f, k) - 0.42f * sstep(0.84f, 0.97f, k);
    const float def = 1.0f - sstep(0.12f, 0.35f, std::abs(tone - k));
    c *= 1.0f - rim * 0.35f * def;
    return c + (tone - c) * covP;
}

void renderLineStylePreview(std::vector<std::uint8_t>& rgb, int w, int h, int style, double phase,
                            double originX) {
    if (w <= 1 || h <= 1) {
        rgb.clear();
        return;
    }
    const std::size_t size = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    rgb.resize(size * 3);
    // Pass 1: the drifting photo-noise field + its luma plane. ~20 hash evaluations per pixel,
    // a couple of ms per tick across all three cards -- paid only while the Appearance pane is
    // actually on screen (the dialog's tick gates on that).
    std::vector<float> bg(size * 3);
    std::vector<float> lum(size);
    const float phx = static_cast<float>(phase) + static_cast<float>(originX);
    const float phy = static_cast<float>(phase) * 0.30f;  // the bench's angle: (drift, 0.30*drift)
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float qx = static_cast<float>(x) + phx;
            const float qy = static_cast<float>(y) + phy;
            // The bench tile's field at 1.5x frequency: the card is ~half the bench tile's size,
            // so the bench's feature scale reads oversized here.
            const float s = sstep(0.30f, 0.72f, fbm(qx * 0.03f, qy * 0.03f));
            const float grain = (vnoise(qx * 0.4f, qy * 0.4f) - 0.5f) * 0.07f;
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x;
            bg[i * 3 + 0] = std::clamp(0.13f + (0.92f - 0.13f) * s + grain, 0.0f, 1.0f);
            bg[i * 3 + 1] = std::clamp(0.11f + (0.87f - 0.11f) * s + grain, 0.0f, 1.0f);
            bg[i * 3 + 2] = std::clamp(0.10f + (0.78f - 0.10f) * s + grain, 0.0f, 1.0f);
            lum[i] = 0.299f * bg[i * 3] + 0.587f * bg[i * 3 + 1] + 0.114f * bg[i * 3 + 2];
        }
    }
    // The 9-tap ring key the real shader blurs with (blurKeyLum's 7px ring, diagonals rounded to
    // the pixel grid), clamped at the card edges. Only evaluated near a stroke.
    const auto blurKey = [&](int x, int y) {
        static constexpr int kTap[9][2] = {{0, 0}, {7, 0},  {-7, 0}, {0, 7},  {0, -7},
                                           {5, 5}, {-5, 5}, {5, -5}, {-5, -5}};
        float s = 0.0f;
        for (const auto& t : kTap) {
            const int tx = std::clamp(x + t[0], 0, w - 1);
            const int ty = std::clamp(y + t[1], 0, h - 1);
            s += lum[static_cast<std::size_t>(ty) * static_cast<std::size_t>(w) + tx];
        }
        return s / 9.0f;
    };
    // Pass 2: the fixed chrome -- a sine "lasso" stroke and a reticle ring -- composited with the
    // style's exact colour pick. The stroke is x-monotone with slope <= ~1, so a pixel's nearest
    // segment is its own column's or a close neighbour (the profiles die within ~6px).
    constexpr int kSegs = 24;
    float sx[kSegs + 1], sy[kSegs + 1];
    for (int i = 0; i <= kSegs; ++i) {
        const float t = static_cast<float>(i) / kSegs;
        sx[i] = t * static_cast<float>(w - 1);
        sy[i] = static_cast<float>(h) * (0.62f + 0.16f * std::sin(t * 9.4f + 0.6f));
    }
    const float cx = static_cast<float>(w) * 0.68f;  // the ring plays the brush reticle
    const float cy = static_cast<float>(h) * 0.28f;
    const float R = static_cast<float>(h) * 0.17f;
    const float segW = static_cast<float>(w - 1) / kSegs;
    const auto strokeDist = [&](float fx, float fy, int seg) {
        float d = 1e9f;
        for (int j = std::max(seg - 2, 0); j <= std::min(seg + 2, kSegs - 1); ++j)
            d = std::min(d, segDist(fx, fy, sx[j], sy[j], sx[j + 1], sy[j + 1]));
        const float dx = fx - cx, dy = fy - cy;
        return std::min(d, std::abs(std::sqrt(dx * dx + dy * dy) - R));
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x;
            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            const int seg = static_cast<int>(fx / segW);
            const float d0 = strokeDist(fx, fy, seg);
            float r = bg[i * 3], g = bg[i * 3 + 1], b = bg[i * 3 + 2];
            if (d0 < 6.0f) {  // inside a stroke corridor: shade like the shader would
                // Classic keys on the exact pixel like the app; the styled lines on the blur ring.
                const float k = style == 0 ? lum[i] : blurKey(x, y);
                if (d0 < 3.0f) {
                    // 3x3-supersample the profile region, mirroring the shader's AA: the core
                    // profiles transition in well under a pixel, so a single sample reads jagged
                    // at card size.
                    float ar = 0.0f, ag = 0.0f, ab = 0.0f;
                    for (int ssy = 0; ssy < 3; ++ssy)
                        for (int ssx = 0; ssx < 3; ++ssx) {
                            const float ds =
                                strokeDist(fx + (ssx + 0.5f) / 3.0f - 0.5f,
                                           fy + (ssy + 0.5f) / 3.0f - 0.5f, seg);
                            ar += lineStyleShade(style, k, ds, r);
                            ag += lineStyleShade(style, k, ds, g);
                            ab += lineStyleShade(style, k, ds, b);
                        }
                    r = ar / 9.0f;
                    g = ag / 9.0f;
                    b = ab / 9.0f;
                } else {
                    r = lineStyleShade(style, k, d0, r);
                    g = lineStyleShade(style, k, d0, g);
                    b = lineStyleShade(style, k, d0, b);
                }
            }
            rgb[i * 3 + 0] =
                static_cast<std::uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            rgb[i * 3 + 1] =
                static_cast<std::uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
            rgb[i * 3 + 2] =
                static_cast<std::uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
}

}  // namespace mosaic::ui
