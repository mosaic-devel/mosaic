#include "core/brush/tip_outline.hpp"

#include "core/brush/dab_mask.hpp"
#include "core/brush/mask_generator.hpp"

#include <cmath>
#include <cstdint>

namespace mosaic::core::brush {
namespace detail {

namespace {
// Big enough to stand in for "no seed in this row/column" without ever being an infinity: an INF in
// the parabola hull turns the intersection into a NaN, and a NaN silently poisons the whole scan.
constexpr double kFar = 1.0e12;
} // namespace

void edt1d(const std::vector<double>& f, std::vector<double>& out, std::vector<int>& v,
           std::vector<double>& z) noexcept {
    const int n = static_cast<int>(f.size());
    if (n <= 0)
        return;
    out.assign(static_cast<std::size_t>(n), 0.0);
    v.assign(static_cast<std::size_t>(n), 0);
    z.assign(static_cast<std::size_t>(n) + 1, 0.0);

    // The lower envelope of the parabolas y = (x - p)^2 + f[p], built left to right. `v` holds the
    // parabolas still on the hull and `z` the x where each takes over from its predecessor.
    const auto meet = [&f](int q, int p) {
        return ((f[static_cast<std::size_t>(q)] + static_cast<double>(q) * q) -
                (f[static_cast<std::size_t>(p)] + static_cast<double>(p) * p)) /
               (2.0 * static_cast<double>(q) - 2.0 * static_cast<double>(p));
    };
    int k = 0; // index of the rightmost parabola on the hull
    v[0] = 0;
    z[0] = -kFar;
    z[1] = kFar;
    for (int q = 1; q < n; ++q) {
        // Pop parabolas off the hull until the new one's crossing lies to the RIGHT of the last
        // boundary. `z[0] == -kFar` and every f is finite, so the crossing is always > z[0]: k cannot
        // walk off the bottom. (This is exactly why `f` uses a large finite value and never an
        // infinity -- an INF here makes the crossing a NaN, and a NaN compares false forever.)
        double s = meet(q, v[static_cast<std::size_t>(k)]);
        while (s <= z[static_cast<std::size_t>(k)]) {
            --k;
            s = meet(q, v[static_cast<std::size_t>(k)]);
        }
        ++k;
        v[static_cast<std::size_t>(k)] = q;
        z[static_cast<std::size_t>(k)] = s;
        z[static_cast<std::size_t>(k) + 1] = kFar;
    }

    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[static_cast<std::size_t>(k) + 1] < static_cast<double>(q))
            ++k;
        const int p = v[static_cast<std::size_t>(k)];
        const double dq = static_cast<double>(q) - static_cast<double>(p);
        out[static_cast<std::size_t>(q)] = dq * dq + f[static_cast<std::size_t>(p)];
    }
}

std::vector<double> squaredEdt(const std::vector<std::uint8_t>& seed, int w, int h) {
    std::vector<double> grid(static_cast<std::size_t>(w) * h, 0.0);
    if (w <= 0 || h <= 0 || seed.size() != grid.size())
        return grid;

    for (std::size_t i = 0; i < grid.size(); ++i)
        grid[i] = seed[i] != 0 ? 0.0 : kFar;

    std::vector<double> f;
    std::vector<double> out;
    std::vector<int> v;
    std::vector<double> z;

    // Columns first, then rows -- the transform is separable, which is what makes it O(w*h).
    f.resize(static_cast<std::size_t>(h));
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y)
            f[static_cast<std::size_t>(y)] = grid[static_cast<std::size_t>(y) * w + x];
        edt1d(f, out, v, z);
        for (int y = 0; y < h; ++y)
            grid[static_cast<std::size_t>(y) * w + x] = out[static_cast<std::size_t>(y)];
    }
    f.resize(static_cast<std::size_t>(w));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x)
            f[static_cast<std::size_t>(x)] = grid[static_cast<std::size_t>(y) * w + x];
        edt1d(f, out, v, z);
        for (int x = 0; x < w; ++x)
            grid[static_cast<std::size_t>(y) * w + x] = out[static_cast<std::size_t>(x)];
    }
    return grid;
}

} // namespace detail

bool tipNeedsSdf(const BrushTip* tip) noexcept {
    // ⚠ FIRST, and not merely for speed: a NULL tip is the engine's built-in analytic circle -- the
    // one every golden and every bit-exact antialiasing check in the suite was laid by. It never
    // traces.
    if (tip == nullptr)
        return false;
    if (const MaskGeneratorParams* gen = tip->generator(); gen != nullptr) {
        // Only the plain circle is an ellipse. A `rect` is a rectangle and a spiked generator is a
        // star; the shader's closed form knows neither, so both must trace or the ring lies.
        return !(gen->shape == MaskShape::Circle && gen->spikes <= 2);
    }
    // A bitmap tip ALWAYS traces -- it is the whole reason this exists. (A tip holding neither a
    // generator nor a bitmap paints nothing; the ellipse is as good an answer as any.)
    return tip->bitmap() != nullptr;
}

TipSdf buildTipSdf(const BrushTip& tip, int frame, double ratio, int res) {
    TipSdf sdf;
    if (res <= 1 || res > kTipSdfRes)
        res = kTipSdfRes;
    if (!(ratio > 0.0) || !std::isfinite(ratio))
        ratio = 1.0; // a preset is an untrusted file; a ratio of 0 paints nothing at all

    // The tip's TRUE envelope, in its own frame: at the dab's squash (see the header -- a spiked tip
    // has no star without it) and at angle 0, because the dab's rotation IS applied when the field is
    // sampled.
    DabShape shape = tipDabShape(tip, frame, static_cast<double>(res), ratio,
                                 /*angleRad=*/0.0, /*mirrorH=*/false, /*mirrorV=*/false);
    if (!(shape.width > 0.0) || !(shape.height > 0.0))
        return sdf;

    // `res` sizes the tip's LONG axis, whichever it is. A `ratio` above 1 makes a procedural tip
    // taller than `diameter`, and a grid sized from the width alone would then overrun the storage
    // the renderer sized for `res`.
    const double longAxis = std::fmax(shape.width, shape.height);
    const double fit = static_cast<double>(res) / longAxis;
    shape.width *= fit;
    shape.height *= fit;
    if (!(shape.width > 0.0) || !(shape.height > 0.0))
        return sdf;

    // Phase 0, deliberately. A sub-pixel phase splits a one-pixel feature across two cells at half
    // coverage each; the silhouette read back out would then be a blurred, one-cell-fat copy of the
    // tip's. The cost is that the box's continuous extent need not be a whole number of cells, which
    // is why `boxW`/`boxH` are carried separately from `w`/`h` rather than assumed equal to them.
    const DabMask mask = renderTipMask(tip, frame, shape, /*softness=*/1.0, /*subX=*/0.0,
                                       /*subY=*/0.0);
    if (mask.empty())
        return sdf;

    const int mw = static_cast<int>(mask.width);
    const int mh = static_cast<int>(mask.height);
    if (mw <= 0 || mh <= 0 || mw > kTipSdfRes || mh > kTipSdfRes)
        return sdf; // a tip that will not fit the renderer's grid falls back to the ellipse

    const int pad = kTipSdfPad; // background all round (see the header: the rect tip, and the band)
    const int w = mw + 2 * pad;
    const int h = mh + 2 * pad;

    // ⚠ The silhouette is `coverage != 0`. Any non-zero coverage is inside -- NOT a half-coverage
    // threshold, which on a soft tip would trace a ring well inside the tip's real extent and
    // understate the brush's size.
    std::vector<std::uint8_t> inside(static_cast<std::size_t>(w) * h, 0);
    std::vector<std::uint8_t> outside(static_cast<std::size_t>(w) * h, 1);
    std::size_t insideCount = 0;
    for (int y = 0; y < mh; ++y) {
        for (int x = 0; x < mw; ++x) {
            if (mask.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) == 0)
                continue;
            const std::size_t i = static_cast<std::size_t>(y + pad) * w + (x + pad);
            inside[i] = 1;
            outside[i] = 0;
            ++insideCount;
        }
    }
    if (insideCount == 0)
        return sdf; // the tip paints nothing: no outline to trace

    // Two exact transforms: how far each cell is from the tip, and how far it is from the background.
    const std::vector<double> toInside = detail::squaredEdt(inside, w, h);
    const std::vector<double> toOutside = detail::squaredEdt(outside, w, h);

    sdf.w = w;
    sdf.h = h;
    sdf.pad = pad;
    sdf.boxW = shape.width;
    sdf.boxH = shape.height;
    sdf.d.resize(static_cast<std::size_t>(w) * h, 0.0F);

    // The nearest opposite-class cell CENTRE is at least one cell away, so a bare EDT reads 1.0 at
    // the outline instead of the half cell that is actually there. Subtracting the half cell puts the
    // zero crossing on the boundary BETWEEN the two centres -- where the silhouette's edge is -- and
    // leaves the field with a unit gradient everywhere, which is what the shader's gradient
    // correction assumes.
    const double inv = 1.0 / shape.width; // normalize: a distance of 1.0 is the tip's whole width
    for (std::size_t i = 0; i < sdf.d.size(); ++i) {
        const double dist = inside[i] != 0 ? -(std::sqrt(toOutside[i]) - 0.5)
                                           : (std::sqrt(toInside[i]) - 0.5);
        sdf.d[i] = static_cast<float>(dist * inv);
    }
    return sdf;
}

} // namespace mosaic::core::brush
