#include "core/brush/mask_generator.hpp"

#include "core/brush/math_util.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSqrt2 = 1.41421356237309504880;

// The `soft` falloff's curve is resampled into a LUT and interpolated linearly. The reference sizes
// it by the tip's diameter; a fixed table is both simpler and steadier -- a 400 px tip and a 4 px
// tip should have the same profile, not different quantizations of one.
constexpr std::size_t kSoftLutSize = 512;

using detail::clamp01;

// The linear rim the antialiasing flag adds: coverage falls from its profile value to 0 across the
// last pixel. Expressed in coverage space, the reference's value-space ramp is exactly this product.
[[nodiscard]] double rimFade(double t) noexcept { return clamp01(1.0 - t); }

// Interior points of the softness curve have their y scaled by `softness`. A two-point curve has no
// interior, so a midpoint is created first -- otherwise softness would do nothing to a linear ramp,
// which is the default curve and therefore the common case.
[[nodiscard]] Curve softenCurve(const Curve& curve, double softness) {
    if (softness == 1.0)
        return curve;

    std::vector<CurvePoint> pts = curve.points();
    if (pts.size() == 2) {
        CurvePoint mid;
        mid.x = 0.5 * (pts[0].x + pts[1].x);
        mid.y = 0.5 * (pts[0].y + pts[1].y);
        pts.insert(pts.begin() + 1, mid);
    }
    for (std::size_t i = 1; i + 1 < pts.size(); ++i)
        pts[i].y = clamp01(pts[i].y * softness);
    return Curve(std::move(pts));
}

} // namespace

std::string_view maskFalloffName(MaskFalloff f) noexcept {
    switch (f) {
    case MaskFalloff::Soft:
        return "soft";
    case MaskFalloff::Gauss:
        return "gauss";
    case MaskFalloff::Default:
        break;
    }
    return "default";
}

std::optional<MaskFalloff> maskFalloffFromName(std::string_view name) noexcept {
    if (name == "default")
        return MaskFalloff::Default;
    if (name == "soft")
        return MaskFalloff::Soft;
    if (name == "gauss")
        return MaskFalloff::Gauss;
    return std::nullopt;
}

std::string_view maskShapeName(MaskShape s) noexcept {
    return s == MaskShape::Rect ? "rect" : "circle";
}

std::optional<MaskShape> maskShapeFromName(std::string_view name) noexcept {
    if (name == "circle")
        return MaskShape::Circle;
    if (name == "rect")
        return MaskShape::Rect;
    return std::nullopt;
}

MaskGenerator::MaskGenerator(MaskGeneratorParams params) : m_params(std::move(params)) {
    m_params.diameter = std::max(0.0, m_params.diameter);
    m_params.ratio = std::max(0.0, m_params.ratio);
    m_params.hFade = clamp01(m_params.hFade);
    m_params.vFade = clamp01(m_params.vFade);
    m_params.spikes = std::clamp(m_params.spikes, 2, 64);
    m_params.softness = std::clamp(m_params.softness, 0.0, 1.0);

    m_width = m_params.diameter;
    m_height = m_params.diameter * m_params.ratio;
    m_empty = !(m_width > 0.0) || !(m_height > 0.0);
    m_halfWidth = 0.5 * m_width;
    m_halfHeight = 0.5 * m_height;

    m_spikeCos = std::cos(-2.0 * kPi / m_params.spikes);
    m_spikeSin = std::sin(-2.0 * kPi / m_params.spikes);
    m_spikeAngle = kPi / m_params.spikes;

    if (m_empty)
        return;

    m_xCoef = 2.0 / m_width;
    m_yCoef = 2.0 / m_height;

    // A zero fade means "no shoulder": the coefficient collapses to 1 rather than to infinity, which
    // in a tip normalized to the unit disc puts the shoulder far outside it. That is what makes
    // hFade = 0 a hard edge instead of a division by zero.
    const double xFadeCoef = (m_params.hFade == 0.0) ? 1.0 : 2.0 / (m_params.hFade * m_width);
    const double yFadeCoef = (m_params.vFade == 0.0) ? 1.0 : 2.0 / (m_params.vFade * m_height);
    const double softnessCoef = 1.0 / std::max(0.01, m_params.softness);
    m_fadeX = xFadeCoef * softnessCoef;
    m_fadeY = yFadeCoef * softnessCoef;

    switch (m_params.falloff) {
    case MaskFalloff::Gauss: {
        if (m_params.shape == MaskShape::Circle) {
            // `fade` reads the RAW attributes, not the halved form -- one of the few places the two
            // conventions are visible side by side. It is pushed off 0 and 1, where erf's argument
            // and the normalizer both become undefined.
            double fade = 1.0 - (m_params.hFade + m_params.vFade) / 2.0;
            if (fade <= 0.0)
                fade = 1e-6;
            else if (fade >= 1.0)
                fade = 1.0 - 1e-6;

            m_gaussYCoef = 1.0 / m_params.ratio;
            m_gaussCenter = (2.5 * (6761.0 * fade - 10000.0)) / (kSqrt2 * 6761.0 * fade);
            m_gaussAlpha = 255.0 / (2.0 * std::erf(m_gaussCenter));
            m_gaussDist = kSqrt2 * 12500.0 / (6761.0 * fade * m_halfWidth);
            m_gaussRadius = m_halfWidth;
        } else {
            const double xFade = (1.0 - m_params.hFade / 2.0) * m_width * 0.1;
            const double yFade = (1.0 - m_params.vFade / 2.0) * m_height * 0.1;
            m_rectGaussXFade = 1.0 / (kSqrt2 * xFade);
            m_rectGaussYFade = 1.0 / (kSqrt2 * yFade);
            m_rectGaussHalfW = m_halfWidth - 2.5 * xFade;
            m_rectGaussHalfH = m_halfHeight - 2.5 * yFade;
            m_rectGaussAlpha = 255.0 / (4.0 * std::erf(m_rectGaussHalfW * m_rectGaussXFade) *
                                        std::erf(m_rectGaussHalfH * m_rectGaussYFade));
            if (!std::isfinite(m_rectGaussAlpha))
                m_rectGaussAlpha = 0.0; // erf(0) when the tip has no area to fade across
        }
        break;
    }
    case MaskFalloff::Soft: {
        m_softLut = softenCurve(m_params.softnessCurve, m_params.softness).toLut(kSoftLutSize);
        if (m_params.shape == MaskShape::Circle) {
            // The circle's antialiasing band is expressed in SQUARED normalized radius, because that
            // is the domain the curve is sampled on. `1 - coef` is the reference's `(1/c - 1)*c`.
            const double xf = std::max(0.0, 1.0 - m_xCoef);
            const double yf = std::max(0.0, 1.0 - m_yCoef);
            const double start = 0.5 * (xf + yf);
            m_softFadeStart = start * start;
        }
        break;
    }
    case MaskFalloff::Default:
        break;
    }
}

void MaskGenerator::fixRotation(double& x, double& y) const noexcept {
    if (m_params.spikes <= 2)
        return;
    double angle = std::atan2(y, x);
    // `y >= 0` on entry, so `angle` starts in [0, pi] and each step removes a full wedge. The bound
    // on `spikes` keeps this from ever being more than a handful of iterations.
    while (angle > m_spikeAngle) {
        const double sx = x;
        const double sy = y;
        x = m_spikeCos * sx - m_spikeSin * sy;
        y = m_spikeSin * sx + m_spikeCos * sy;
        angle -= 2.0 * m_spikeAngle;
    }
}

double MaskGenerator::circleDefault(double x, double y) const noexcept {
    const double nx = x * m_xCoef;
    const double ny = y * m_yCoef;
    const double n = nx * nx + ny * ny; // squared normalized radius
    if (n > 1.0)
        return 0.0;

    // The +1 px is what gives a hard tip (hFade = 0) an antialiased rim rather than a jagged step:
    // it pushes the shoulder's own coordinate one pixel out, so the last pixel always straddles it.
    double ax = std::fabs(x);
    double ay = std::fabs(y);
    if (m_params.antialiasEdges) {
        ax += 1.0;
        ay += 1.0;
    }

    const double fx = ax * m_fadeX;
    const double fy = ay * m_fadeY;
    const double nf = fx * fx + fy * fy;
    if (nf < 1.0)
        return 1.0; // inside the solid core

    // n == nf only where both reach the rim at once; the reference divides by zero there.
    const double denom = nf - n;
    if (!(denom > 0.0))
        return 0.0;
    return clamp01(1.0 - n * (nf - 1.0) / denom);
}

double MaskGenerator::rectDefault(double x, double y) const noexcept {
    double ax = std::fabs(x);
    double ay = std::fabs(y);

    const double nx = ax * m_xCoef;
    const double ny = ay * m_yCoef;
    if (nx > 1.0 || ny > 1.0)
        return 0.0;

    if (m_params.antialiasEdges) {
        ax += 1.0;
        ay += 1.0;
    }

    const double fx = ax * m_fadeX;
    const double fy = ay * m_fadeY;

    // Each axis fades independently; the tip takes whichever shoulder it has entered, and the deeper
    // of the two when it has entered both. `value` here is the reference's inverted sense.
    double value = 0.0;
    const double fxDen = fx - nx;
    const double fyDen = fy - ny;
    const double fxNorm = (fxDen > 0.0) ? nx * (fx - 1.0) / fxDen : 0.0;
    const double fyNorm = (fyDen > 0.0) ? ny * (fy - 1.0) / fyDen : 0.0;

    if (fx > 1.0)
        value = fxNorm;
    if (fxNorm < fyNorm && fy > 1.0)
        value = fyNorm;

    return clamp01(1.0 - value);
}

double MaskGenerator::gaussCircleProfile(double dist) const noexcept {
    const double d = dist * m_gaussDist;
    const double v = m_gaussAlpha * (std::erf(d + m_gaussCenter) - std::erf(d - m_gaussCenter));
    return clamp01(v / 255.0);
}

double MaskGenerator::circleGauss(double x, double y) const noexcept {
    const double yn = y * m_gaussYCoef;
    const double dist = std::sqrt(x * x + yn * yn);
    if (dist > m_gaussRadius)
        return 0.0;

    if (m_params.antialiasEdges) {
        const double start = std::max(0.0, m_gaussRadius - 1.0);
        if (dist > start) {
            const double span = m_gaussRadius - start;
            if (!(span > 0.0))
                return 0.0;
            return gaussCircleProfile(start) * rimFade((dist - start) / span);
        }
    }
    return gaussCircleProfile(dist);
}

double MaskGenerator::rectGauss(double x, double y) const noexcept {
    const double ax = std::fabs(x);
    const double ay = std::fabs(y);
    if (ax > m_halfWidth || ay > m_halfHeight)
        return 0.0;

    const double gx = std::erf((m_rectGaussHalfW + ax) * m_rectGaussXFade) +
                      std::erf((m_rectGaussHalfW - ax) * m_rectGaussXFade);
    const double gy = std::erf((m_rectGaussHalfH + ay) * m_rectGaussYFade) +
                      std::erf((m_rectGaussHalfH - ay) * m_rectGaussYFade);
    double coverage = clamp01(m_rectGaussAlpha * gx * gy / 255.0);

    // The two axes' rims compose multiplicatively -- the reference reaches the same value by nesting
    // two "blend the rest of the way to transparent" steps.
    if (m_params.antialiasEdges) {
        const double sx = m_halfWidth - 1.0;
        const double sy = m_halfHeight - 1.0;
        if (ax > sx && m_halfWidth > sx)
            coverage *= rimFade((ax - sx) / (m_halfWidth - sx));
        if (ay > sy && m_halfHeight > sy)
            coverage *= rimFade((ay - sy) / (m_halfHeight - sy));
    }
    return coverage;
}

double MaskGenerator::softCurve(double t) const noexcept { return clamp01(evalLut(m_softLut, t)); }

double MaskGenerator::circleSoft(double x, double y) const noexcept {
    const double nx = x * m_xCoef;
    const double ny = y * m_yCoef;
    const double dist = nx * nx + ny * ny; // the curve's domain IS the squared radius
    if (dist > 1.0)
        return 0.0;

    if (m_params.antialiasEdges && dist > m_softFadeStart) {
        const double span = 1.0 - m_softFadeStart;
        if (!(span > 0.0))
            return 0.0;
        return softCurve(m_softFadeStart) * rimFade((dist - m_softFadeStart) / span);
    }
    return softCurve(dist);
}

// One axis of the rect's soft profile: the curve rising from the edge, times the curve falling
// towards the far edge. Symmetric about the centre by construction.
double MaskGenerator::softRectAxis(double t) const noexcept {
    return softCurve(t) * (1.0 - softCurve(1.0 - t));
}

double MaskGenerator::rectSoft(double x, double y) const noexcept {
    const double ax = std::fabs(x);
    const double ay = std::fabs(y);
    if (ax > m_halfWidth || ay > m_halfHeight)
        return 0.0;

    const double nx = ax / m_halfWidth;
    const double ny = ay / m_halfHeight;
    double coverage = clamp01(softRectAxis(nx) * softRectAxis(ny));

    if (m_params.antialiasEdges) {
        const double sx = m_halfWidth - 1.0;
        const double sy = m_halfHeight - 1.0;
        if (ax > sx && m_halfWidth > sx)
            coverage *= rimFade((ax - sx) / (m_halfWidth - sx));
        if (ay > sy && m_halfHeight > sy)
            coverage *= rimFade((ay - sy) / (m_halfHeight - sy));
    }
    return coverage;
}

double MaskGenerator::coverageAt(double x, double y) const noexcept {
    if (m_empty || !std::isfinite(x) || !std::isfinite(y))
        return 0.0;

    // The falloffs are all symmetric in y, so the spike fold only ever sees the upper half-plane.
    //
    // ⚠ Five of the six generators hand the spike fold a SIGNED x. The `default` rect alone folds x
    // into the right half-plane first, which mirrors its spikes relative to every other tip. That is
    // a quirk of the reference implementation, not a design; it is reproduced here because an
    // imported preset may depend on the shape it produces. It is unobservable in the shipped set --
    // all four presets that use spikes > 2 are circles, and no shipped preset pairs a rect with
    // anything but spikes = 2.
    const bool foldXFirst =
        m_params.shape == MaskShape::Rect && m_params.falloff == MaskFalloff::Default;
    double xr = foldXFirst ? std::fabs(x) : x;
    double yr = std::fabs(y);
    fixRotation(xr, yr);

    switch (m_params.falloff) {
    case MaskFalloff::Gauss:
        return m_params.shape == MaskShape::Circle ? circleGauss(xr, yr) : rectGauss(xr, yr);
    case MaskFalloff::Soft:
        return m_params.shape == MaskShape::Circle ? circleSoft(xr, yr) : rectSoft(xr, yr);
    case MaskFalloff::Default:
        break;
    }
    return m_params.shape == MaskShape::Circle ? circleDefault(xr, yr) : rectDefault(xr, yr);
}

} // namespace mosaic::core::brush
