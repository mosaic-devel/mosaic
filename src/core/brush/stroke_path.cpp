#include "core/brush/stroke_path.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

using common::Vec2;

// CENTRIPETAL. alpha=0 is the uniform parameterization (which cusps and self-intersects on unevenly
// spaced points -- and mouse samples are nothing but unevenly spaced); alpha=1 is chordal (which
// overshoots). 0.5 is the one that provably does neither.
constexpr double kAlpha = 0.5;

// Knot gaps are divided by, and we DELIBERATELY duplicate the stroke's first and last points (a
// stroke has no sample before its first, and none after its last), which makes a gap of exactly
// zero. Flooring it is what turns that duplication into the natural "tangent along the chord" end
// condition instead of a division by zero.
constexpr double kKnotFloor = 1e-9;

[[nodiscard]] double nextKnot(double t, Vec2 a, Vec2 b) {
    return t + std::max(std::pow((b - a).length(), kAlpha), kKnotFloor);
}

// One step of the Barry-Goldman pyramid: the linear interpolation of a and b over the knot span
// [ta, tb], evaluated at t. The span is never zero -- nextKnot floors it.
[[nodiscard]] Vec2 knotLerp(Vec2 a, Vec2 b, double ta, double tb, double t) {
    const double w = (tb - t) / (tb - ta);
    return a * w + b * (1.0 - w);
}

// Perpendicular distance from `c` to the LINE through p1 and p2. See the header: measuring the
// distance to `p1 + chord*u` instead would mistake a reparameterization for a bend.
[[nodiscard]] double distanceToChord(Vec2 c, Vec2 p1, Vec2 p2) {
    const Vec2 chord = p2 - p1;
    const double len = chord.length();
    if (len < kKnotFloor)
        return (c - p1).length(); // degenerate chord: fall back to the point distance
    const Vec2 d = c - p1;
    return std::abs(chord.x * d.y - chord.y * d.x) / len; // |cross| / |chord|
}

} // namespace

Vec2 catmullRom(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, double u) {
    const double t0 = 0.0;
    const double t1 = nextKnot(t0, p0, p1);
    const double t2 = nextKnot(t1, p1, p2);
    const double t3 = nextKnot(t2, p2, p3);
    const double t = t1 + (t2 - t1) * u; // u in [0,1] over the p1..p2 span

    const Vec2 a1 = knotLerp(p0, p1, t0, t1, t);
    const Vec2 a2 = knotLerp(p1, p2, t1, t2, t);
    const Vec2 a3 = knotLerp(p2, p3, t2, t3, t);
    const Vec2 b1 = knotLerp(a1, a2, t0, t2, t);
    const Vec2 b2 = knotLerp(a2, a3, t1, t3, t);
    return knotLerp(b1, b2, t1, t2, t);
}

void flattenCatmullRom(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, double tol,
                       std::vector<Vec2>& out) {
    out.clear();
    if (!(tol > 0.0))
        return;

    // How far does the curve stray from the chord? Probe three interior parameters, not one: a
    // single midpoint probe reads EXACTLY ZERO on an S-shaped span (the curve crosses the chord at
    // its middle) and would flatten a genuine wiggle into a straight line.
    double dev = 0.0;
    for (const double u : {0.25, 0.5, 0.75})
        dev = std::max(dev, distanceToChord(catmullRom(p0, p1, p2, p3, u), p1, p2));

    if (dev <= tol)
        return; // it IS the chord: no interior points, and the walk stays bit-identical to the old one

    // How far the k-edge polyline actually sits from the curve: the worst gap between a sub-chord's
    // midpoint and the curve at the same parameter. This is MEASURED, not predicted.
    const auto polylineError = [&](int k) {
        double worst = 0.0;
        for (int i = 0; i < k; ++i) {
            const double ua = static_cast<double>(i) / k;
            const double ub = static_cast<double>(i + 1) / k;
            const Vec2 a = catmullRom(p0, p1, p2, p3, ua);
            const Vec2 b = catmullRom(p0, p1, p2, p3, ub);
            const Vec2 mid = catmullRom(p0, p1, p2, p3, 0.5 * (ua + ub));
            worst = std::max(worst, ((a + b) * 0.5 - mid).length());
        }
        return worst;
    };

    // Seed from the closed form (a cubic's flattening error falls off as ~1/k^2), then REFINE until
    // the polyline really is within tolerance.
    //
    // ⚠ The closed form alone is not enough, and assuming it was is a bug this caught: on a sharply
    // turning span it under-counted badly -- 0.42 px out against a 0.05 px tolerance. The header
    // promises `tol` as a BOUND on how far the polyline may sit from the curve, so the bound has to
    // be checked rather than estimated. The cap is what keeps the refinement finite.
    int k = std::clamp(static_cast<int>(std::ceil(std::sqrt(dev / tol))), 2, kMaxFlattenSteps);
    while (k < kMaxFlattenSteps && polylineError(k) > tol)
        k = std::min(k * 2, kMaxFlattenSteps);

    out.reserve(static_cast<std::size_t>(k - 1));
    for (int i = 1; i < k; ++i)
        out.push_back(catmullRom(p0, p1, p2, p3, static_cast<double>(i) / k));
}

} // namespace mosaic::core::brush
