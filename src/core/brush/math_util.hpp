#pragma once

#include <cmath>

// The two arithmetic primitives the brush module shares. Both were written three times over before
// this header existed, which is exactly how a clamp starts treating NaN differently in two files.
namespace mosaic::core::brush::detail {

// Saturate to [0,1]. NaN falls to 0 -- the comparisons are false, so the second ternary takes its
// else branch. That is deliberate: a NaN coverage or a NaN sensor reading must become "nothing",
// never propagate into a dab.
[[nodiscard]] inline double clamp01(double v) noexcept {
    return v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0;
}

// Wrap `x` into the half-open range [lo, hi). Angles are cyclic: an offset that pushes past a half
// turn must come back the other side rather than clamp there.
//
// The result is guaranteed finite, and strictly less than `hi`. Both guarantees need code: a
// non-finite `x` has no image under fmod, and a tiny negative remainder plus `range` can round up
// to exactly `range`, which would return `hi` and break the half-open contract callers rely on to
// index a table or compare against `lo`.
[[nodiscard]] inline double wrapValue(double x, double lo, double hi) noexcept {
    const double range = hi - lo;
    if (!(range > 0.0) || !std::isfinite(x))
        return lo;
    double v = std::fmod(x - lo, range);
    if (v < 0.0)
        v += range;
    if (!(v < range))
        v = 0.0;
    return v + lo;
}

} // namespace mosaic::core::brush::detail
