// Clean-room lunar + celestial-frame solver (see lunar.hpp's lineage note). Every formula below
// is from Meeus, "Astronomical Algorithms" (2nd ed.), ch. 12/13/14/22/25/47/48; the periodic-term
// tables kTermsLR / kTermsB are Meeus's Table 47.A / 47.B (published data). The only implementation
// choices are plumbing (degree<->radian helpers, wrap conventions, the compass-azimuth mapping).

#include "core/texture/lunar.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace mosaic::core::texture {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kAuKm = 149597870.7;         // astronomical unit, km
constexpr double kEarthRadiusKm = 6378.14;    // Meeus's equatorial radius, for lunar parallax
constexpr double kSynodicMonthDays = 29.530588853;

double deg2rad(double d) noexcept { return d * kPi / 180.0; }
double rad2deg(double r) noexcept { return r * 180.0 / kPi; }
double wrap360(double d) noexcept {
    d = std::fmod(d, 360.0);
    return d < 0.0 ? d + 360.0 : d;
}

double julianCentury(const UtcTime& t) { return (julianDay(t) - 2451545.0) / 36525.0; }

// Meeus 22.2 mean obliquity of the ecliptic (degrees).
double meanObliquityDeg(double T) {
    return 23.0 + (26.0 + (21.448 - T * (46.8150 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
}

// NOAA/Meeus standard-atmosphere refraction (degrees) added to a geometric elevation -- the same
// piecewise fit solar.cpp uses, kept local so the two solvers don't couple internals.
double refractionDeg(double elevDeg) {
    if (elevDeg > 85.0) return 0.0;
    const double tanE = std::tan(deg2rad(elevDeg));
    double sec;  // arc-seconds
    if (elevDeg > 5.0)
        sec = 58.1 / tanE - 0.07 / std::pow(tanE, 3.0) + 0.000086 / std::pow(tanE, 5.0);
    else if (elevDeg > -0.575)
        sec = 1735.0 +
              elevDeg * (-518.2 + elevDeg * (103.4 + elevDeg * (-12.79 + elevDeg * 0.711)));
    else
        sec = -20.774 / tanE;
    return sec / 3600.0;
}

// ecliptic (lon,lat) of date -> equatorial (RA,Dec) -- Meeus 13.3/13.4. All degrees.
EquatorialCoord eclToEq(double lonDeg, double latDeg, double obliqDeg) {
    const double l = deg2rad(lonDeg), b = deg2rad(latDeg), e = deg2rad(obliqDeg);
    const double ra =
        std::atan2(std::sin(l) * std::cos(e) - std::tan(b) * std::sin(e), std::cos(l));
    const double dec =
        std::asin(std::sin(b) * std::cos(e) + std::cos(b) * std::sin(e) * std::sin(l));
    return {wrap360(rad2deg(ra)), rad2deg(dec)};
}

// Low-accuracy Sun (Meeus ch. 25): apparent geocentric ecliptic longitude (deg) + radius (km),
// used ONLY for the phase geometry. The daylight dome keeps its own solver (solar.cpp); this is a
// deliberately small, self-contained copy so lunar.cpp needs nothing from solar's internals.
struct SunLR {
    double lonDeg;
    double distanceKm;
};
SunLR sunLonR(double T) {
    const double l0 = wrap360(280.46646 + T * (36000.76983 + T * 0.0003032));
    const double m = 357.52911 + T * (35999.05029 - 0.0001537 * T);
    const double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
    const double mRad = deg2rad(m);
    const double c = std::sin(mRad) * (1.914602 - T * (0.004817 + 0.000014 * T)) +
                     std::sin(2.0 * mRad) * (0.019993 - 0.000101 * T) +
                     std::sin(3.0 * mRad) * 0.000289;
    const double trueLon = l0 + c;
    const double omega = 125.04 - 1934.136 * T;
    const double apparentLon = trueLon - 0.00569 - 0.00478 * std::sin(deg2rad(omega));
    const double v = m + c;
    const double rAu = 1.000001018 * (1.0 - e * e) / (1.0 + e * std::cos(deg2rad(v)));
    return {wrap360(apparentLon), rAu * kAuKm};
}

// Meeus Table 47.A: longitude Sigma_l (units 1e-6 deg) and distance Sigma_r (units 1e-3 km).
// Columns: D, M, M', F multipliers, then the two coefficients.
struct TermLR {
    int d, m, mp, f;
    double sl, sr;
};
constexpr std::array<TermLR, 60> kTermsLR{{
    {0, 0, 1, 0, 6288774, -20905355},   {2, 0, -1, 0, 1274027, -3699111},
    {2, 0, 0, 0, 658314, -2955968},     {0, 0, 2, 0, 213618, -569925},
    {0, 1, 0, 0, -185116, 48888},       {0, 0, 0, 2, -114332, -3149},
    {2, 0, -2, 0, 58793, 246158},       {2, -1, -1, 0, 57066, -152138},
    {2, 0, 1, 0, 53322, -170733},       {2, -1, 0, 0, 45758, -204586},
    {0, 1, -1, 0, -40923, -129620},     {1, 0, 0, 0, -34720, 108743},
    {0, 1, 1, 0, -30383, 104755},       {2, 0, 0, -2, 15327, 10321},
    {0, 0, 1, 2, -12528, 0},            {0, 0, 1, -2, 10980, 79661},
    {4, 0, -1, 0, 10675, -34782},       {0, 0, 3, 0, 10034, -23210},
    {4, 0, -2, 0, 8548, -21636},        {2, 1, -1, 0, -7888, 24208},
    {2, 1, 0, 0, -6766, 30824},         {1, 0, -1, 0, -5163, -8379},
    {1, 1, 0, 0, 4987, -16675},         {2, -1, 1, 0, 4036, -12831},
    {2, 0, 2, 0, 3994, -10445},         {4, 0, 0, 0, 3861, -11650},
    {2, 0, -3, 0, 3665, 14403},         {0, 1, -2, 0, -2689, -7003},
    {2, 0, -1, 2, -2602, 0},            {2, -1, -2, 0, 2390, 10056},
    {1, 0, 1, 0, -2348, 6322},          {2, -2, 0, 0, 2236, -9884},
    {0, 1, 2, 0, -2120, 5751},          {0, 2, 0, 0, -2069, 0},
    {2, -2, -1, 0, 2048, -4950},        {2, 0, 1, -2, -1773, 4130},
    {2, 0, 0, 2, -1595, 0},             {4, -1, -1, 0, 1215, -3958},
    {0, 0, 2, 2, -1110, 0},             {3, 0, -1, 0, -892, 3258},
    {2, 1, 1, 0, -810, 2616},           {4, -1, -2, 0, 759, -1897},
    {0, 2, -1, 0, -713, -2117},         {2, 2, -1, 0, -700, 2354},
    {2, 1, -2, 0, 691, 0},              {2, -1, 0, -2, 596, 0},
    {4, 0, 1, 0, 549, -1423},           {0, 0, 4, 0, 537, -1117},
    {4, -1, 0, 0, 520, -1571},          {1, 0, -2, 0, -487, -1739},
    {2, 1, 0, -2, -399, 0},             {0, 0, 2, -2, -381, -4421},
    {1, 1, 1, 0, 351, 0},               {3, 0, -2, 0, -340, 0},
    {4, 0, -3, 0, 330, 0},              {2, -1, 2, 0, 327, 0},
    {0, 2, 1, 0, -323, 1165},           {1, 1, -1, 0, 299, 0},
    {2, 0, 3, 0, 294, 0},               {2, 0, -1, -2, 0, 8752},
}};

// Meeus Table 47.B: latitude Sigma_b (units 1e-6 deg). Columns: D, M, M', F, coefficient.
struct TermB {
    int d, m, mp, f;
    double sb;
};
constexpr std::array<TermB, 60> kTermsB{{
    {0, 0, 0, 1, 5128122},  {0, 0, 1, 1, 280602},   {0, 0, 1, -1, 277693},
    {2, 0, 0, -1, 173237},  {2, 0, -1, 1, 55413},   {2, 0, -1, -1, 46271},
    {2, 0, 0, 1, 32573},    {0, 0, 2, 1, 17198},    {2, 0, 1, -1, 9266},
    {0, 0, 2, -1, 8822},    {2, -1, 0, -1, 8216},   {2, 0, -2, -1, 4324},
    {2, 0, 1, 1, 4200},     {2, 1, 0, -1, -3359},   {2, -1, -1, 1, 2463},
    {2, -1, 0, 1, 2211},    {2, -1, -1, -1, 2065},  {0, 1, -1, -1, -1870},
    {4, 0, -1, -1, 1828},   {0, 1, 0, 1, -1794},    {0, 0, 0, 3, -1749},
    {0, 1, -1, 1, -1565},   {1, 0, 0, 1, -1491},    {0, 1, 1, 1, -1475},
    {0, 1, 1, -1, -1410},   {0, 1, 0, -1, -1344},   {1, 0, 0, -1, -1335},
    {0, 0, 3, 1, 1107},     {4, 0, 0, -1, 1021},    {4, 0, -1, 1, 833},
    {0, 0, 1, -3, 777},     {4, 0, -2, 1, 671},     {2, 0, 0, -3, 607},
    {2, 0, 2, -1, 596},     {2, -1, 1, -1, 491},    {2, 0, -2, 1, -451},
    {0, 0, 3, -1, 439},     {2, 0, 2, 1, 422},      {2, 0, -3, -1, 421},
    {2, 1, -1, 1, -366},    {2, 1, 0, 1, -351},     {4, 0, 0, 1, 331},
    {2, -1, 1, 1, 315},     {2, -2, 0, -1, 302},    {0, 0, 1, 3, -283},
    {2, 1, 1, -1, -229},    {1, 1, 0, -1, 223},     {1, 1, 0, 1, 223},
    {0, 1, -2, -1, -220},   {2, 1, -1, -1, -220},   {1, 0, 1, 1, -185},
    {2, -1, -2, -1, 181},   {0, 1, 2, 1, -177},     {4, 0, -2, -1, 176},
    {4, -1, -1, -1, 166},   {1, 0, 1, -1, -164},    {4, 0, 1, -1, 132},
    {1, 0, -1, -1, -119},   {4, -1, 0, -1, 115},    {2, -2, 0, 1, 107},
}};

// The eccentricity power E^|m| for the M multiplier (Meeus: terms in the Sun's mean anomaly are
// scaled by E = 1 - 0.002516T - ... to the |M| power, correcting for the changing solar orbit).
double ePow(int m, double E, double E2) {
    const int a = m < 0 ? -m : m;
    return a == 0 ? 1.0 : (a == 1 ? E : E2);
}

MoonGeocentric moonGeocentricT(double T) {
    const double T2 = T * T, T3 = T2 * T, T4 = T3 * T;
    // Mean arguments (Meeus 47.1-47.5), degrees.
    const double Lp = wrap360(218.3164477 + 481267.88123421 * T - 0.0015786 * T2 +
                              T3 / 538841.0 - T4 / 65194000.0);  // mean longitude
    const double D = 297.8501921 + 445267.1114034 * T - 0.0018819 * T2 + T3 / 545868.0 -
                     T4 / 113065000.0;  // mean elongation
    const double M = 357.5291092 + 35999.0502909 * T - 0.0001536 * T2 +
                     T3 / 24490000.0;  // Sun's mean anomaly
    const double Mp = 134.9633964 + 477198.8675055 * T + 0.0087414 * T2 + T3 / 69699.0 -
                      T4 / 14712000.0;  // Moon's mean anomaly
    const double F = 93.2720950 + 483202.0175233 * T - 0.0036539 * T2 - T3 / 3526000.0 +
                     T4 / 863310000.0;  // argument of latitude
    const double A1 = 119.75 + 131.849 * T;
    const double A2 = 53.09 + 479264.290 * T;
    const double A3 = 313.45 + 481266.484 * T;
    const double E = 1.0 - 0.002516 * T - 0.0000074 * T2;
    const double E2 = E * E;

    // Additive terms (Venus, Jupiter and the flattening of the Earth -- Meeus p. 338).
    double suml = 3958.0 * std::sin(deg2rad(A1)) + 1962.0 * std::sin(deg2rad(Lp - F)) +
                  318.0 * std::sin(deg2rad(A2));
    double sumr = 0.0;
    double sumb = -2235.0 * std::sin(deg2rad(Lp)) + 382.0 * std::sin(deg2rad(A3)) +
                  175.0 * std::sin(deg2rad(A1 - F)) + 175.0 * std::sin(deg2rad(A1 + F)) +
                  127.0 * std::sin(deg2rad(Lp - Mp)) - 115.0 * std::sin(deg2rad(Lp + Mp));

    for (const TermLR& r : kTermsLR) {
        const double arg = deg2rad(r.d * D + r.m * M + r.mp * Mp + r.f * F);
        const double e = ePow(r.m, E, E2);
        suml += r.sl * std::sin(arg) * e;
        sumr += r.sr * std::cos(arg) * e;
    }
    for (const TermB& r : kTermsB) {
        const double arg = deg2rad(r.d * D + r.m * M + r.mp * Mp + r.f * F);
        sumb += r.sb * std::sin(arg) * ePow(r.m, E, E2);
    }

    MoonGeocentric g;
    g.eclLonDeg = wrap360(Lp + suml * 1e-6);
    g.eclLatDeg = sumb * 1e-6;
    g.distanceKm = 385000.56 + sumr * 1e-3;
    g.equatorial = eclToEq(g.eclLonDeg, g.eclLatDeg, meanObliquityDeg(T));
    return g;
}

MoonPhase moonPhaseT(double T) {
    const MoonGeocentric moon = moonGeocentricT(T);
    const SunLR sun = sunLonR(T);
    const double obliq = meanObliquityDeg(T);
    const EquatorialCoord sunEq = eclToEq(sun.lonDeg, 0.0, obliq);

    const double a0 = deg2rad(sunEq.raDeg), d0 = deg2rad(sunEq.decDeg);
    const double a = deg2rad(moon.equatorial.raDeg), d = deg2rad(moon.equatorial.decDeg);
    // Geocentric elongation psi (48.2) -> phase angle i (48.3) -> illuminated fraction k (48.1).
    const double cosPsi =
        std::sin(d0) * std::sin(d) + std::cos(d0) * std::cos(d) * std::cos(a0 - a);
    const double psi = std::acos(std::clamp(cosPsi, -1.0, 1.0));
    const double i = std::atan2(sun.distanceKm * std::sin(psi),
                                moon.distanceKm - sun.distanceKm * std::cos(psi));

    MoonPhase p;
    p.phaseAngleDeg = rad2deg(i);
    p.illuminatedFraction = (1.0 + std::cos(i)) / 2.0;
    // Position angle of the bright limb's midpoint (48.5), from the north celestial pole toward E.
    const double chi =
        std::atan2(std::cos(d0) * std::sin(a0 - a),
                   std::sin(d0) * std::cos(d) - std::cos(d0) * std::sin(d) * std::cos(a0 - a));
    p.brightLimbAngleDeg = wrap360(rad2deg(chi));
    // Synodic age from the ecliptic elongation (0 at new, ~14.77 at full, waxing through waning).
    p.ageDays = wrap360(moon.eclLonDeg - sun.lonDeg) / 360.0 * kSynodicMonthDays;
    return p;
}

// Meeus ch. 53 optical libration + position angle of the axis. Only the optical librations are
// evaluated (physical librations ~0.02 deg and nutation are omitted, like everywhere else here);
// the constant I is the inclination of the mean lunar equator to the ecliptic. Pinned to example
// 53.a (1992 Apr 12): l ~= -1.206 deg, b ~= +4.194 deg, P ~= 15.08 deg.
MoonPhysical moonPhysicalT(double T) {
    const double T2 = T * T, T3 = T2 * T, T4 = T3 * T;
    const MoonGeocentric moon = moonGeocentricT(T);
    const double lambda = moon.eclLonDeg;          // apparent longitude (no nutation, per above)
    const double beta = deg2rad(moon.eclLatDeg);   // ecliptic latitude
    const double eps = deg2rad(meanObliquityDeg(T));
    const double alpha = deg2rad(moon.equatorial.raDeg);
    constexpr double I = 1.54242;                  // deg, inclination of the mean lunar equator
    const double Irad = deg2rad(I);
    // Longitude of the mean ascending node of the lunar orbit (Meeus, same series as ch. 47's node).
    const double Omega = 125.0445479 - 1934.1362891 * T + 0.0020754 * T2 + T3 / 467441.0 -
                         T4 / 60616000.0;
    // Moon's argument of latitude (Meeus 47.5), needed for l = A - F.
    const double F = 93.2720950 + 483202.0175233 * T - 0.0036539 * T2 - T3 / 3526000.0 +
                     T4 / 863310000.0;

    // --- Optical librations (53.*). W = lambda - dPsi - Omega; dPsi = 0 here. ---
    const double W = deg2rad(lambda - Omega);
    const double A = std::atan2(std::sin(W) * std::cos(beta) * std::cos(Irad) -
                                    std::sin(beta) * std::sin(Irad),
                                std::cos(W) * std::cos(beta));
    double lLon = rad2deg(A) - F;
    lLon = std::fmod(lLon, 360.0);
    if (lLon > 180.0) lLon -= 360.0;
    if (lLon < -180.0) lLon += 360.0;
    const double bLat = std::asin(-std::sin(W) * std::cos(beta) * std::sin(Irad) -
                                  std::sin(beta) * std::cos(Irad));

    // --- Position angle of the axis P (53.*). V = Omega (dPsi, sigma = 0); rho = 0. ---
    const double V = deg2rad(Omega);
    const double X = std::sin(Irad) * std::sin(V);
    const double Y = std::sin(Irad) * std::cos(V) * std::cos(eps) - std::cos(Irad) * std::sin(eps);
    const double omega = std::atan2(X, Y);
    const double P = std::asin(std::clamp(std::sqrt(X * X + Y * Y) * std::cos(alpha - omega) /
                                              std::cos(bLat),
                                          -1.0, 1.0));

    MoonPhysical mp;
    mp.librationLonDeg = lLon;
    mp.librationLatDeg = rad2deg(bLat);
    mp.axisPositionAngleDeg = rad2deg(P);
    return mp;
}

}  // namespace

double greenwichMeanSiderealTimeDeg(const UtcTime& t) {
    const double jd = julianDay(t);
    const double T = (jd - 2451545.0) / 36525.0;
    // Meeus 12.4 (degrees), valid for any instant (the fractional-day term is folded into JD).
    const double theta = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * T * T -
                         T * T * T / 38710000.0;
    return wrap360(theta);
}

HorizontalCoord equatorialToHorizontal(EquatorialCoord eq, double gmstDeg, double latitudeDeg,
                                       double longitudeDeg, bool applyRefraction) {
    // Local hour angle H = local sidereal time - RA; local sidereal = Greenwich + east longitude.
    const double H = deg2rad(wrap360(gmstDeg + longitudeDeg - eq.raDeg));
    const double lat = deg2rad(latitudeDeg), dec = deg2rad(eq.decDeg);
    const double sinAlt =
        std::sin(lat) * std::sin(dec) + std::cos(lat) * std::cos(dec) * std::cos(H);
    double alt = rad2deg(std::asin(std::clamp(sinAlt, -1.0, 1.0)));
    // Meeus 13.5 gives azimuth from SOUTH toward WEST; +180 -> compass clockwise from north.
    const double az = std::atan2(std::sin(H),
                                 std::cos(H) * std::sin(lat) - std::tan(dec) * std::cos(lat));
    if (applyRefraction) alt += refractionDeg(alt);
    return {wrap360(rad2deg(az) + 180.0), alt};
}

MoonGeocentric moonGeocentric(const UtcTime& t) { return moonGeocentricT(julianCentury(t)); }

MoonPhase moonPhase(const UtcTime& t) { return moonPhaseT(julianCentury(t)); }

MoonPhysical moonPhysical(const UtcTime& t) { return moonPhysicalT(julianCentury(t)); }

MoonObservation moonObservation(const UtcTime& t, double latitudeDeg, double longitudeDeg) {
    const double T = julianCentury(t);
    const MoonGeocentric moon = moonGeocentricT(T);
    const double gmst = greenwichMeanSiderealTimeDeg(t);
    const HorizontalCoord geo =
        equatorialToHorizontal(moon.equatorial, gmst, latitudeDeg, longitudeDeg, false);

    // Topocentric parallax: the nearby Moon rides up to ~1 deg LOWER than the geocentric altitude
    // (dominant altitude term, Meeus ch. 40 -- the sub-arc-minute azimuth cross-term is dropped).
    const double piRad = std::asin(std::clamp(kEarthRadiusKm / moon.distanceKm, -1.0, 1.0));
    double alt = geo.altitudeDeg - rad2deg(piRad * std::cos(deg2rad(geo.altitudeDeg)));
    alt += refractionDeg(alt);

    const MoonPhase ph = moonPhaseT(T);
    const MoonPhysical phys = moonPhysicalT(T);
    // Parallactic angle (14.1): the tilt between the sky's vertical and celestial north, used to
    // set the near-side texture upright in the observer's frame.
    const double H = deg2rad(wrap360(gmst + longitudeDeg - moon.equatorial.raDeg));
    const double lat = deg2rad(latitudeDeg), dec = deg2rad(moon.equatorial.decDeg);
    const double q = std::atan2(std::sin(H),
                                std::tan(lat) * std::cos(dec) - std::sin(dec) * std::cos(H));

    MoonObservation o;
    o.azimuthDeg = geo.azimuthDeg;
    o.altitudeDeg = alt;
    o.illuminatedFraction = ph.illuminatedFraction;
    o.phaseAngleDeg = ph.phaseAngleDeg;
    o.brightLimbAngleDeg = ph.brightLimbAngleDeg;
    o.parallacticAngleDeg = rad2deg(q);
    o.librationLonDeg = phys.librationLonDeg;
    o.librationLatDeg = phys.librationLatDeg;
    o.axisPositionAngleDeg = phys.axisPositionAngleDeg;
    o.distanceKm = moon.distanceKm;
    o.ageDays = ph.ageDays;
    return o;
}

}  // namespace mosaic::core::texture
