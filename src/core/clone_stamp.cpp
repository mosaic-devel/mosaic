#include "core/clone_stamp.hpp"

#include "core/stroke_confinement.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core {
namespace {

[[nodiscard]] double clamp01(double v) noexcept { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

[[nodiscard]] std::uint8_t to8(double v) noexcept {
    return static_cast<std::uint8_t>(std::lround(clamp01(v) * 255.0));
}

// A whole number within a tolerance far below what a pixel could notice. The offset reaches here
// through two or three affine multiplies, so an exact `== std::round(v)` would reject shifts that
// are integers in every sense that matters.
[[nodiscard]] bool isWhole(double v) noexcept { return std::abs(v - std::round(v)) < 1e-9; }

} // namespace

void setCloneAnchor(CloneAnchorState& s, common::Vec2 anchorDoc) noexcept {
    s.hasAnchor = true;
    s.anchor = anchorDoc;
    // Re-picking a source is exactly the gesture that means "clone from HERE instead", so the
    // latched aligned offset cannot survive it -- keeping it would make the click do nothing.
    s.hasOffset = false;
    s.offset = {};
}

std::optional<common::Vec2> cloneStrokeOffset(CloneAnchorState& s, common::Vec2 strokeStartDoc,
                                              bool aligned) noexcept {
    if (!s.hasAnchor)
        return std::nullopt; // no source picked: there is nothing to clone, and nothing to invent
    if (aligned && s.hasOffset)
        return s.offset; // the offset the first stroke after the anchor established, kept
    const common::Vec2 offset{strokeStartDoc.x - s.anchor.x, strokeStartDoc.y - s.anchor.y};
    if (aligned) {
        s.hasOffset = true;
        s.offset = offset;
    } else {
        // Non-aligned: nothing is latched, so the NEXT stroke derives its own offset from its own
        // start and lands back on the anchor. Clearing here (rather than leaving a stale value) is
        // what makes toggling Aligned back on re-derive instead of resurrecting an old offset.
        s.hasOffset = false;
        s.offset = {};
    }
    return offset;
}

bool isWholePixelShift(const common::Affine2D& t) noexcept {
    return t.m00 == 1.0 && t.m01 == 0.0 && t.m10 == 0.0 && t.m11 == 1.0 && isWhole(t.m02) &&
           isWhole(t.m12);
}

common::Color8 sampleClone(const common::Image& img, double sx, double sy, bool bilinear) noexcept {
    if (img.empty())
        return {0, 0, 0, 0};
    const auto w = static_cast<long>(img.width);
    const auto h = static_cast<long>(img.height);
    const auto at = [&](long x, long y) -> const std::uint8_t* {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return nullptr;
        return img.rgba.data() + (static_cast<std::size_t>(y) * img.width + x) * 4;
    };
    if (!bilinear) {
        const std::uint8_t* p = at(static_cast<long>(std::floor(sx)), static_cast<long>(std::floor(sy)));
        if (p == nullptr)
            return {0, 0, 0, 0}; // off the source: nothing to copy
        return {p[0], p[1], p[2], p[3]};
    }
    // The 2x2 tap, in the pixel-centre convention: the sample at (i + 0.5, j + 0.5) is pixel
    // (i, j) exactly, so the fractional part is measured from the centre.
    const double fx = sx - 0.5;
    const double fy = sy - 0.5;
    const auto x0 = static_cast<long>(std::floor(fx));
    const auto y0 = static_cast<long>(std::floor(fy));
    const double tx = fx - static_cast<double>(x0);
    const double ty = fy - static_cast<double>(y0);
    const double wgt[4] = {(1.0 - tx) * (1.0 - ty), tx * (1.0 - ty), (1.0 - tx) * ty, tx * ty};
    const std::uint8_t* px[4] = {at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1)};
    // Colour is weighted by ALPHA as well as by the tap weight, and normalized by the accumulated
    // alpha-weight: sampling beside a transparent pixel must not drag that pixel's stale RGB into
    // the result (a transparent pixel's colour bytes are not a colour). Alpha itself is the plain
    // weighted mean, which is what makes a soft source edge stay soft.
    double a = 0.0;
    double aw = 0.0;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    for (int i = 0; i < 4; ++i) {
        if (px[i] == nullptr)
            continue; // off the source: contributes transparency, exactly like an empty pixel
        const double pa = px[i][3] / 255.0;
        a += wgt[i] * pa;
        const double cw = wgt[i] * pa;
        aw += cw;
        r += cw * px[i][0];
        g += cw * px[i][1];
        b += cw * px[i][2];
    }
    if (aw <= 0.0)
        return {0, 0, 0, to8(a)};
    // Division, never multiplication by a reciprocal -- the rule the engine's Colored normalization
    // and StrokeConfinement::at() are both pinned to: IEEE gives x/x == 1.0 exactly, so a run of
    // identical opaque taps reproduces its own colour byte for byte.
    return {to8(r / aw / 255.0), to8(g / aw / 255.0), to8(b / aw / 255.0), to8(a)};
}

std::size_t applyCloneStamp(const CloneStampInput& in, int x0, int y0, int x1, int y1) noexcept {
    if (in.target == nullptr || in.base == nullptr || in.source == nullptr ||
        in.coverage == nullptr || in.covW == 0 || in.covH == 0)
        return 0;
    common::Image& out = *in.target;
    const common::Image& base = *in.base;
    if (out.empty() || out.width != base.width || out.height != base.height)
        return 0; // the layer was resized under the stroke: deposit nothing rather than garbage

    // Clamp to the target AND to the coverage window: outside the window the stroke has no alpha,
    // so there is nothing to deposit and nothing to read.
    x0 = std::max({x0, 0, static_cast<int>(in.covX)});
    y0 = std::max({y0, 0, static_cast<int>(in.covY)});
    x1 = std::min({x1, static_cast<int>(out.width),
                   static_cast<int>(in.covX) + static_cast<int>(in.covW)});
    y1 = std::min({y1, static_cast<int>(out.height),
                   static_cast<int>(in.covY) + static_cast<int>(in.covH)});
    if (x0 >= x1 || y0 >= y1)
        return 0;

    const double cap = clamp01(in.opacity);
    std::size_t written = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t ci = static_cast<std::size_t>(y - in.covY) * in.covW +
                                   static_cast<std::size_t>(x - in.covX);
            double sa = static_cast<double>(in.coverage[ci]) * cap;
            if (sa <= 0.0)
                continue; // never stamped here -- the target stays exactly pristine
            if (in.confine != nullptr) {
                sa *= in.confine->at(x, y);
                if (sa <= 0.0)
                    continue; // outside the selection, exactly as the engine's composite skips it
            }
            // The source pixel, read at this target pixel's CENTRE mapped through the offset.
            const common::Vec2 s =
                in.targetToSource.apply({static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5});
            const common::Color8 src = sampleClone(*in.source, s.x, s.y, in.bilinear);

            const std::size_t p = (static_cast<std::size_t>(y) * out.width + x) * 4;
            const double br = base.rgba[p] / 255.0;
            const double bg = base.rgba[p + 1] / 255.0;
            const double bb = base.rgba[p + 2] / 255.0;
            const double ba = base.rgba[p + 3] / 255.0;
            // The source's OWN alpha rides the stroke's: cloning a transparent part of the source
            // deposits transparency, i.e. nothing, rather than a black hole.
            const double srcA = sa * (src.a / 255.0);
            const double oa = srcA + ba * (1.0 - srcA); // straight-alpha source-over
            if (oa <= 1e-6) {
                // Both sides empty. Keep the base's un-premultiplied colour bytes (the engine's own
                // erase convention) so painting back over the pixel later does not drag a black
                // fringe in from nowhere.
                out.rgba[p] = base.rgba[p];
                out.rgba[p + 1] = base.rgba[p + 1];
                out.rgba[p + 2] = base.rgba[p + 2];
                out.rgba[p + 3] = 0;
                ++written;
                continue;
            }
            const double inv = 1.0 / oa;
            out.rgba[p] = to8((src.r / 255.0 * srcA + br * ba * (1.0 - srcA)) * inv);
            out.rgba[p + 1] = to8((src.g / 255.0 * srcA + bg * ba * (1.0 - srcA)) * inv);
            out.rgba[p + 2] = to8((src.b / 255.0 * srcA + bb * ba * (1.0 - srcA)) * inv);
            out.rgba[p + 3] = to8(oa);
            ++written;
        }
    }
    return written;
}

} // namespace mosaic::core
