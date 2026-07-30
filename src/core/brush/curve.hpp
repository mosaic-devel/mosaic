#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// The brush-dynamics response curve (docs/brushes.md §3.3) -- the shared foundation under every
// CurveOption: a sensor produces a value in [0,1], this maps it to the option's [0,1] output.
//
// The curve is a NATURAL cubic spline through its control points (second derivative zero at both
// ends), evaluated in x. A point flagged `corner` additionally forces the second derivative to zero
// on BOTH sides of itself, which severs C1/C2 continuity there -- so the curve is equivalently a
// chain of independent natural splines joined at the corner points. That equivalence is what lets us
// solve it with a tridiagonal pass per segment instead of one big linear system.
//
// The serialized form is the interchange format used by the presets we import ("x,y;x,y;", with an
// optional third token `is_corner` per point). Imported curves must round-trip byte-exactly, so
// toString() reproduces its 6-significant-digit `%g` formatting, and fromString() tolerates the
// legacy two-token form, a missing trailing ';', and unknown extra tokens.
//
// FLTK- and Vulkan-free: unit-tested headlessly, and shared by the engine, the brush editor's curve
// widget, and the tablet layer's global pressure curve (docs/tablet.md §7).
namespace mosaic::core::brush {

struct CurvePoint {
    double x = 0.0;
    double y = 0.0;
    // A corner breaks the spline: the curve arrives and leaves with zero second derivative, so the
    // two sides are independent. Two adjacent corners (or a corner next to an endpoint) therefore
    // bound a 2-point segment, which is a straight line. Ignored on the first/last point.
    bool corner = false;
};

// The identity curve, and the default of every option that has not been edited.
inline constexpr std::string_view kIdentityCurve = "0,0;1,1;";

class Curve {
public:
    // Identity: (0,0) -> (1,1).
    Curve();

    // Takes control points in any order; they are sorted by x and points sharing an x with an
    // earlier point are dropped (a zero-width interval has no spline). An empty list yields the
    // identity curve; a single point yields the constant function at its y.
    explicit Curve(std::vector<CurvePoint> points);

    // Parses "x,y;x,y;" -- see the file comment for the tolerated variations. A string that yields
    // no usable point (empty, or entirely malformed) returns the identity curve rather than failing,
    // because a preset with a corrupt curve should still load with that option behaving sanely.
    [[nodiscard]] static Curve fromString(std::string_view s);

    // Round-trips fromString() byte-exactly for any string that fromString() accepted verbatim.
    [[nodiscard]] std::string toString() const;

    // Evaluate at `x`. `x` outside the control points' domain is clamped to it (the curve is
    // extended flat, not extrapolated -- a cubic run past its last knot diverges fast). The result
    // is clamped to [0,1]: a natural spline routinely overshoots between widely separated knots, and
    // an option strength outside [0,1] is meaningless.
    [[nodiscard]] double eval(double x) const;

    // `size` samples of eval() over [0,1] inclusive, for the per-stroke lookup tables the dab
    // pipeline and the tablet pressure path use instead of re-evaluating the spline per sample.
    // `size` < 2 is treated as 2.
    [[nodiscard]] std::vector<float> toLut(std::size_t size) const;

    // True when this is the identity curve (two default endpoints). The dab pipeline skips the
    // lookup entirely for these, which is the overwhelmingly common case in real presets.
    [[nodiscard]] bool isIdentity() const noexcept;

    [[nodiscard]] const std::vector<CurvePoint>& points() const noexcept { return m_points; }

private:
    void rebuild();

    // One cubic per interval, in the interval-local coordinate t = x - x[i]:
    //     s(t) = a + b*t + (c/2)*t^2 + (d/6)*t^3
    // Local coordinates rather than absolute x: the absolute-x form loses precision badly once the
    // knots are far from the origin, and costs nothing here.
    struct Cubic {
        double a = 0.0;
        double b = 0.0;
        double c = 0.0;
        double d = 0.0;
    };

    std::vector<CurvePoint> m_points;
    std::vector<Cubic> m_intervals; // size == m_points.size() - 1 (empty for a single point)
};

// Linear interpolation into a LUT built by Curve::toLut(), for `x` in [0,1] (clamped). The dab
// pipeline's hot path: one multiply, one floor, one lerp.
[[nodiscard]] float evalLut(const std::vector<float>& lut, double x);

} // namespace mosaic::core::brush
