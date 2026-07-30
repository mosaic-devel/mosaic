// Clean-room NOAA/Meeus solar position (see solar.hpp's lineage note). Every formula below is
// from the published NOAA Solar Calculator equation set / Meeus ch. 22/25/28; the only
// implementation choices are plumbing (degree<->radian helpers, wrap conventions).

#include "core/texture/solar.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mosaic::core::texture {

namespace {

constexpr double kPi = std::numbers::pi;

double deg2rad(double d) noexcept {
    return d * kPi / 180.0;
}
double rad2deg(double r) noexcept {
    return r * 180.0 / kPi;
}
// Wrap into [0, 360).
double wrap360(double d) noexcept {
    d = std::fmod(d, 360.0);
    return d < 0.0 ? d + 360.0 : d;
}

// The geometry of one moment: cooked once from the Julian century, evaluated by the public
// functions. All angles degrees unless suffixed.
struct SunGeometry {
    double declinationDeg = 0.0;
    double eqOfTimeMin = 0.0;
};

SunGeometry sunGeometry(double julianCentury) {
    const double T = julianCentury;

    // Geometric mean longitude / mean anomaly of the sun, eccentricity of Earth's orbit
    // (Meeus 25.2-25.4).
    const double meanLon = wrap360(280.46646 + T * (36000.76983 + T * 0.0003032));
    const double meanAnom = 357.52911 + T * (35999.05029 - 0.0001537 * T);
    const double eccent = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);

    // Equation of centre -> true longitude (Meeus ch. 25).
    const double mRad = deg2rad(meanAnom);
    const double eqCentre = std::sin(mRad) * (1.914602 - T * (0.004817 + 0.000014 * T)) +
                            std::sin(2.0 * mRad) * (0.019993 - 0.000101 * T) +
                            std::sin(3.0 * mRad) * 0.000289;
    const double trueLon = meanLon + eqCentre;

    // Apparent longitude: nutation + aberration via the lunar ascending node (Meeus 25.8).
    const double omega = 125.04 - 1934.136 * T;
    const double apparentLon = trueLon - 0.00569 - 0.00478 * std::sin(deg2rad(omega));

    // Mean obliquity of the ecliptic (Meeus 22.2) + the same node correction (25.8).
    const double meanObliq =
        23.0 + (26.0 + (21.448 - T * (46.8150 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
    const double obliq = meanObliq + 0.00256 * std::cos(deg2rad(omega));

    SunGeometry g;
    g.declinationDeg =
        rad2deg(std::asin(std::sin(deg2rad(obliq)) * std::sin(deg2rad(apparentLon))));

    // Equation of time (Meeus 28.3), in minutes of time.
    const double y = std::pow(std::tan(deg2rad(obliq) / 2.0), 2.0);
    const double l0Rad = deg2rad(meanLon);
    const double eot = y * std::sin(2.0 * l0Rad) - 2.0 * eccent * std::sin(mRad) +
                       4.0 * eccent * y * std::sin(mRad) * std::cos(2.0 * l0Rad) -
                       0.5 * y * y * std::sin(4.0 * l0Rad) -
                       1.25 * eccent * eccent * std::sin(2.0 * mRad);
    g.eqOfTimeMin = 4.0 * rad2deg(eot);
    return g;
}

// NOAA atmospheric refraction correction (degrees), added to the geometric elevation. Piecewise
// over elevation bands; the standard-atmosphere fit the NOAA calculator uses.
double refractionDeg(double elevDeg) {
    if (elevDeg > 85.0) return 0.0;
    const double tanE = std::tan(deg2rad(elevDeg));
    double sec = 0.0;  // arc-seconds
    if (elevDeg > 5.0) {
        sec = 58.1 / tanE - 0.07 / std::pow(tanE, 3.0) + 0.000086 / std::pow(tanE, 5.0);
    } else if (elevDeg > -0.575) {
        sec = 1735.0 +
              elevDeg * (-518.2 + elevDeg * (103.4 + elevDeg * (-12.79 + elevDeg * 0.711)));
    } else {
        sec = -20.774 / tanE;
    }
    return sec / 3600.0;
}

double julianCentury(const UtcTime& t) {
    return (julianDay(t) - 2451545.0) / 36525.0;
}

}  // namespace

double julianDay(const UtcTime& t) {
    // Meeus 7.1, Gregorian calendar. Month/year shift folds Jan/Feb into the preceding year so
    // the leap day sits at the end of the counting year.
    int y = t.year;
    int m = t.month;
    if (m <= 2) {
        y -= 1;
        m += 12;
    }
    const int a = y / 100;
    const int b = 2 - a + a / 4;
    return std::floor(365.25 * (y + 4716)) + std::floor(30.6001 * (m + 1)) +
           static_cast<double>(t.day) + b - 1524.5 + t.hour / 24.0;
}

double solarDeclinationDeg(const UtcTime& t) {
    return sunGeometry(julianCentury(t)).declinationDeg;
}

double equationOfTimeMinutes(const UtcTime& t) {
    return sunGeometry(julianCentury(t)).eqOfTimeMin;
}

SunPosition sunPosition(const UtcTime& t, double latitudeDeg, double longitudeDeg) {
    const SunGeometry g = sunGeometry(julianCentury(t));

    // True solar time at the observer's meridian (minutes): clock UTC + equation of time +
    // 4 min per degree of east longitude. Hour angle: 0 at solar noon, +afternoon.
    const double clockMin = t.hour * 60.0;
    const double trueSolarMin =
        std::fmod(clockMin + g.eqOfTimeMin + 4.0 * longitudeDeg + 1440.0 * 4.0, 1440.0);
    double hourAngle = trueSolarMin / 4.0 - 180.0;
    if (hourAngle < -180.0) hourAngle += 360.0;

    const double latRad = deg2rad(latitudeDeg);
    const double decRad = deg2rad(g.declinationDeg);
    const double haRad = deg2rad(hourAngle);

    // Zenith angle, then azimuth from the spherical triangle (NOAA convention: clockwise from
    // true north; the acos form is folded by the hour angle's sign).
    const double cosZen = std::sin(latRad) * std::sin(decRad) +
                          std::cos(latRad) * std::cos(decRad) * std::cos(haRad);
    const double zenRad = std::acos(std::clamp(cosZen, -1.0, 1.0));
    const double elevDeg = 90.0 - rad2deg(zenRad);

    double azimuthDeg = 0.0;
    const double sinZen = std::sin(zenRad);
    if (std::abs(sinZen) > 1e-9) {
        const double cosAz = std::clamp(
            (std::sin(latRad) * cosZen - std::sin(decRad)) / (std::cos(latRad) * sinZen), -1.0,
            1.0);
        const double azFromSouth = rad2deg(std::acos(cosAz));
        azimuthDeg = hourAngle > 0.0 ? wrap360(azFromSouth + 180.0) : wrap360(540.0 - azFromSouth);
    } else {
        // Sun at the zenith/nadir: azimuth is degenerate; point it at the noon meridian.
        azimuthDeg = latitudeDeg >= g.declinationDeg ? 180.0 : 0.0;
    }

    SunPosition pos;
    pos.azimuthDeg = azimuthDeg;
    pos.elevationDeg = elevDeg + refractionDeg(elevDeg);
    return pos;
}

}  // namespace mosaic::core::texture
