#include "core/brush/texture.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

// The reference's 8-bit arithmetic, spelled out. Every one of these TRUNCATES, and the truncation
// is the transcription: rounding them would move a byte on almost every textured pixel.
[[nodiscard]] constexpr int mul8(int a, int b) noexcept {
    return (a * b) / 255;
}
[[nodiscard]] constexpr int mul8(int a, int b, int c) noexcept {
    return (a * b * c) / (255 * 255);
}
[[nodiscard]] constexpr int inv8(int a) noexcept {
    return 255 - a;
}
// The reference's `unionShapeOpacity`.
[[nodiscard]] constexpr int union8(int a, int b) noexcept {
    return a + b - mul8(a, b);
}

// The reference's `qGray` weights, the same expression bitmap_tip.hpp's `luma` carries. Repeated
// rather than shared because the two are transcribed from different call sites and a future change
// to one must not silently move the other.
[[nodiscard]] constexpr int greyOf(int r, int g, int b) noexcept {
    return (r * 11 + g * 16 + b * 5) / 32;
}

// A separable TENT resample, used only for the pattern's `scale`.
//
// ⚠ THIS IS THE ONE DELIBERATE DEVIATION IN THE BAKE, and it is the same one the whole tip
// pipeline already carries: the reference resamples its pattern with its toolkit's smooth scaler,
// which is not transcribed, and Mosaic resamples rasters with its own filters everywhere (a bitmap
// tip is minified through a mip chain and a bilinear tap, and the 47 bitmap-tip presets are Exact
// on that basis). The kernel is a tent of radius max(1, 1/scale) source pixels per axis: at
// scale >= 1 that is exactly bilinear, and below it a triangle-filtered downscale, so neither
// regime aliases. It costs no fidelity badge for the same reason the tip raster does not.
void resampleAxis(const std::vector<std::uint8_t>& src, int srcW, int srcH, int dstW,
                  std::vector<std::uint8_t>& dst, bool horizontal) {
    const int outW = horizontal ? dstW : srcW;
    const int outH = horizontal ? srcH : dstW;
    dst.assign(static_cast<std::size_t>(outW) * static_cast<std::size_t>(outH) * 4u,
               std::uint8_t{0});

    const int n = horizontal ? srcW : srcH;
    const double scale = static_cast<double>(dstW) / static_cast<double>(n);
    const double radius = std::max(1.0, 1.0 / scale);

    // The stride between neighbouring taps in `src`, in RGBA units.
    const std::size_t tapStride = horizontal ? 4u : static_cast<std::size_t>(srcW) * 4u;
    const std::size_t lineStride = horizontal ? static_cast<std::size_t>(srcW) * 4u : 4u;
    const int lines = horizontal ? srcH : srcW;

    for (int o = 0; o < dstW; ++o) {
        const double center = (static_cast<double>(o) + 0.5) / scale - 0.5;
        const int first = static_cast<int>(std::floor(center - radius + 1e-9));
        const int last = static_cast<int>(std::ceil(center + radius - 1e-9));
        for (int line = 0; line < lines; ++line) {
            double acc[4] = {0.0, 0.0, 0.0, 0.0};
            double wsum = 0.0;
            for (int t = first; t <= last; ++t) {
                const double w = 1.0 - std::abs(static_cast<double>(t) - center) / radius;
                if (w <= 0.0)
                    continue;
                // Clamp to the edge rather than wrapping: the reference scales the image, not the
                // tiling, and a wrapped tap would smear the far edge into the near one.
                const int s = std::clamp(t, 0, n - 1);
                const std::size_t p = static_cast<std::size_t>(line) * lineStride +
                                      static_cast<std::size_t>(s) * tapStride;
                for (int c = 0; c < 4; ++c)
                    acc[c] += w * src[p + static_cast<std::size_t>(c)];
                wsum += w;
            }
            const std::size_t outLineStride =
                horizontal ? static_cast<std::size_t>(outW) * 4u : 4u;
            const std::size_t outTapStride = horizontal ? 4u : static_cast<std::size_t>(outW) * 4u;
            const std::size_t q = static_cast<std::size_t>(line) * outLineStride +
                                  static_cast<std::size_t>(o) * outTapStride;
            for (int c = 0; c < 4; ++c) {
                const double v = wsum > 0.0 ? acc[c] / wsum : 0.0;
                dst[q + static_cast<std::size_t>(c)] =
                    static_cast<std::uint8_t>(std::clamp(static_cast<int>(v + 0.5), 0, 255));
            }
        }
    }
}

} // namespace

std::shared_ptr<const TexturePattern> bakeTexturePattern(const std::uint8_t* rgba, std::uint32_t w,
                                                         std::uint32_t h,
                                                         const TextureBake& bake) {
    if (rgba == nullptr || w == 0 || h == 0)
        return nullptr;
    if (static_cast<std::uint64_t>(w) * h > kMaxTexturePixels)
        return nullptr;

    std::vector<std::uint8_t> pixels(rgba, rgba + static_cast<std::size_t>(w) * h * 4u);
    int pw = static_cast<int>(w);
    int ph = static_cast<int>(h);

    // ⚠ EXACTLY 1.0 (and exactly 0.0) RESAMPLE NOTHING -- the reference's own `qFuzzyCompare` pair.
    // Four shipped presets author scale 1, and their mask is the image's luminance byte for byte.
    const double scale = bake.scale;
    if (std::isfinite(scale) && std::abs(scale - 1.0) > 1e-12 && std::abs(scale) > 1e-12) {
        // The reference maps the image's rect through the scale and then floors the result at
        // 2 x 2 -- a pattern scaled to nothing is not a pattern.
        const int tw = std::max(2, static_cast<int>(std::lround(pw * scale)));
        const int th = std::max(2, static_cast<int>(std::lround(ph * scale)));
        if (static_cast<std::uint64_t>(tw) * th > kMaxTexturePixels)
            return nullptr;
        std::vector<std::uint8_t> tmp;
        resampleAxis(pixels, pw, ph, tw, tmp, /*horizontal=*/true);
        pixels.swap(tmp);
        resampleAxis(pixels, tw, ph, th, tmp, /*horizontal=*/false);
        pixels.swap(tmp);
        pw = tw;
        ph = th;
    }

    auto out = std::make_shared<TexturePattern>();
    out->width = static_cast<std::uint32_t>(pw);
    out->height = static_cast<std::uint32_t>(ph);
    out->mask.resize(static_cast<std::size_t>(pw) * ph);

    const auto neutral = static_cast<float>(bake.neutralPoint);
    const auto brightness = static_cast<float>(bake.brightness);
    const auto contrast = static_cast<float>(bake.contrast);
    const float cutLeft = static_cast<float>(bake.cutoffLeft) / 255.0f;
    const float cutRight = static_cast<float>(bake.cutoffRight) / 255.0f;

    for (std::size_t i = 0, n = out->mask.size(); i < n; ++i) {
        const std::uint8_t* p = pixels.data() + i * 4u;
        const float alpha = static_cast<float>(p[3]) / 255.0f;
        const int grey = greyOf(p[0], p[1], p[2]);

        // A transparent pixel reads WHITE, not black: the reference composites the pattern over
        // white before measuring it, and white is the identity under Multiply.
        float v = static_cast<float>(grey) / 255.0f * alpha + (1.0f - alpha);
        v = v - brightness;
        v = ((v - 0.5f) * contrast) + 0.5f;
        // The clamp sits BEFORE the invert, as the reference's does. (It happens to agree with
        // clamping after, because the second bound below catches either order -- so this is the
        // transcription's shape rather than an observable step, and no test claims otherwise.)
        if (v > 1.0f)
            v = 1.0f;
        else if (v < 0.0f)
            v = 0.0f;
        if (bake.invert)
            v = 1.0f - v;
        v = std::clamp(v, 0.0f, 1.0f);

        // The neutral point re-centres the mask with two straight segments rather than one, so
        // neither half clips: [0, neutral] maps onto [0, 0.5] and [neutral, 1] onto [0.5, 1].
        float adjusted = 0.0f;
        if (neutral == 1.0f || (neutral != 0.0f && v <= neutral))
            adjusted = v / (2.0f * neutral);
        else
            adjusted = 0.5f + (v - neutral) / (2.0f - 2.0f * neutral);

        if (bake.cutoffPolicy == 1 && (adjusted < cutLeft || adjusted > cutRight))
            adjusted = 0.0f;
        else if (bake.cutoffPolicy == 2 && (adjusted < cutLeft || adjusted > cutRight))
            adjusted = 1.0f;

        // Round to nearest, 8-bit. ⚠ The clamp is on the FLOAT and not on the cast's result: a
        // preset is third-party input, so a NaN or an enormous brightness must land on a byte
        // rather than on undefined behaviour. For every sane parameter set `adjusted` is already
        // in [0,1] and the guard costs nothing.
        const float scaled = adjusted * 255.0f + 0.5f;
        out->mask[i] = static_cast<std::uint8_t>(
            static_cast<int>(std::isfinite(scaled) ? std::clamp(scaled, 0.0f, 255.0f) : 0.0f));
    }
    return out;
}

std::uint8_t textureValueAt(const TexturePattern& pattern, int docX, int docY, int offX,
                            int offY) noexcept {
    if (pattern.empty())
        return 255;
    const auto w = static_cast<int>(pattern.width);
    const auto h = static_cast<int>(pattern.height);
    // A MATHEMATICAL modulo. C's `%` truncates toward zero, so a document coordinate left of the
    // origin would read a negative index -- and the grain must tile across the origin, not fold at
    // it (a stroke that crosses x = 0 must not see its texture jump).
    int x = (docX - offX) % w;
    if (x < 0)
        x += w;
    int y = (docY - offY) % h;
    if (y < 0)
        y += h;
    return pattern.mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                        static_cast<std::size_t>(x)];
}

int textureStrength8(double strength) noexcept {
    if (!std::isfinite(strength))
        return 0;
    const double v = strength * 255.0;
    return static_cast<int>(std::clamp(v, 0.0, 255.0) + 0.5);
}

std::uint8_t textureComposite(TexturingMode mode, std::uint8_t src, std::uint8_t dst,
                              int strength8, bool soft) noexcept {
    const int s = static_cast<int>(src);
    const int d = static_cast<int>(dst);
    const int k = std::clamp(strength8, 0, 255);
    int r = d;
    switch (mode) {
    case TexturingMode::Multiply:
        r = soft ? mul8(union8(s, inv8(k)), d) : mul8(s, d, k);
        break;
    case TexturingMode::Subtract:
        r = soft ? std::max(0, d - mul8(s, k)) : std::max(0, d - (s + inv8(k)));
        break;
    }
    return static_cast<std::uint8_t>(std::clamp(r, 0, 255));
}

} // namespace mosaic::core::brush
