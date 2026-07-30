#include "core/brush/tablet_policy.hpp"

#include "core/brush/math_util.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

using detail::clamp01;

constexpr double kPi = 3.14159265358979323846;

// The LUT resolution docs/tablet.md §7 specifies. 256 steps over [0,1] with linear interpolation
// between them is below any device's pressure quantization (the coarsest common axis is 10 bits).
constexpr std::size_t kPressureLutSize = 256;

} // namespace

double TabletPolicy::applyPressure(double raw) const noexcept {
    const double p = clamp01(raw);

    // 1. Range remap. A degenerate span is a threshold, not a division by zero.
    const double span = m_rawMax - m_rawMin;
    const double remapped = span > 0.0 ? clamp01((p - m_rawMin) / span) : (p >= m_rawMax ? 1.0 : 0.0);

    // 2. Response curve; identity short-circuits (file comment).
    if (m_lut.empty())
        return remapped;
    return static_cast<double>(evalLut(m_lut, remapped));
}

void TabletPolicy::applyTilt(double& xTilt, double& yTilt) const noexcept {
    if (m_tiltOffsetDegrees == 0.0)
        return;
    // ascension is atan2(-xTilt, yTilt) (stroke_state.cpp); this rotation adds exactly +offset to
    // that bearing: with u = -x, v = y it is the plain 2D rotation u' = u*cos + v*sin,
    // v' = v*cos - u*sin, translated back through x = -u.
    const double x = xTilt;
    const double y = yTilt;
    xTilt = x * m_tiltCos - y * m_tiltSin;
    yTilt = x * m_tiltSin + y * m_tiltCos;
}

StrokeInput TabletPolicy::apply(StrokeInput in) const noexcept {
    in.pressure = applyPressure(in.pressure);
    applyTilt(in.xTilt, in.yTilt);
    return in;
}

void TabletPolicy::setPressureCurve(const Curve& curve) {
    m_curve = curve;
    if (curve.isIdentity())
        m_lut.clear();
    else
        m_lut = curve.toLut(kPressureLutSize);
}

void TabletPolicy::setPressureRange(double rawMin, double rawMax) noexcept {
    rawMin = clamp01(rawMin);
    rawMax = clamp01(rawMax);
    if (rawMax < rawMin)
        std::swap(rawMin, rawMax);
    m_rawMin = rawMin;
    m_rawMax = rawMax;
}

void TabletPolicy::setTiltOffsetDegrees(double degrees) noexcept {
    m_tiltOffsetDegrees = degrees;
    const double radians = degrees * (kPi / 180.0);
    m_tiltCos = std::cos(radians);
    m_tiltSin = std::sin(radians);
}

bool TabletPolicy::isIdentity() const noexcept {
    return m_lut.empty() && m_rawMin == 0.0 && m_rawMax == 1.0 && m_tiltOffsetDegrees == 0.0;
}

} // namespace mosaic::core::brush
