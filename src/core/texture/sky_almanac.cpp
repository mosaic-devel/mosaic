// The night-sky master-clock engine (see sky_almanac.hpp). Pure glue over solar.hpp (Sun),
// lunar.hpp (Moon position/phase) and city_catalog.hpp (nearest city). No new astronomy -- the
// clean-room ephemerides already exist; this composes them into the one call the dialog needs.

#include "core/texture/sky_almanac.hpp"

#include "core/texture/city_catalog.hpp"
#include "core/texture/lunar.hpp"
#include "core/texture/texture_params.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

namespace mosaic::core::texture {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kSolarStandardAltDeg = -0.8333;  // refraction 34' + Sun semidiameter 16' (Meeus 15)
constexpr double kEarthRadiusKm = 6378.14;        // Meeus's equatorial radius, for lunar parallax

double deg2rad(double d) noexcept { return d * kPi / 180.0; }
double rad2deg(double r) noexcept { return r * 180.0 / kPi; }

// The Sun's GEOMETRIC (airless) altitude at `t`, seen from (latDeg +N, lonDeg +E). Built from the
// PUBLIC solar drivers (declination + equation of time) so we reuse the pinned solver and never add
// refraction -- the standard event altitudes (-0.833 / -6 / -12 / -18) are defined against the
// airless altitude. Mirrors solar.cpp's own true-solar-time -> hour-angle -> altitude chain.
double sunGeometricAltitudeDeg(const UtcTime& t, double latDeg, double lonDeg) {
    const double dec = solarDeclinationDeg(t);
    const double eot = equationOfTimeMinutes(t);
    const double clockMin = t.hour * 60.0;
    const double trueSolarMin = std::fmod(clockMin + eot + 4.0 * lonDeg + 1440.0 * 4.0, 1440.0);
    double hourAngle = trueSolarMin / 4.0 - 180.0;  // degrees; 0 at solar transit
    if (hourAngle < -180.0) hourAngle += 360.0;
    const double lat = deg2rad(latDeg), d = deg2rad(dec), h = deg2rad(hourAngle);
    const double sinAlt =
        std::sin(lat) * std::sin(d) + std::cos(lat) * std::cos(d) * std::cos(h);
    return rad2deg(std::asin(std::clamp(sinAlt, -1.0, 1.0)));
}

// The Moon's GEOCENTRIC geometric altitude at `t` (no parallax, no refraction). Meeus ch. 15 folds
// both into the lunar event altitude h0, so rise/set is a crossing of THIS altitude past h0.
double moonGeocentricAltitudeDeg(const UtcTime& t, double latDeg, double lonDeg) {
    const MoonGeocentric m = moonGeocentric(t);
    const double gmst = greenwichMeanSiderealTimeDeg(t);
    return equatorialToHorizontal(m.equatorial, gmst, latDeg, lonDeg, false).altitudeDeg;
}

// The Moon's rise/set event altitude at `t` (Meeus ch. 15): h0 = 0.7275*pi - 0.5667 deg, where pi
// is the Moon's horizontal parallax at that instant's distance. ~+0.125 deg for a mean-distance Moon.
double moonEventAltitudeDeg(const UtcTime& t) {
    const double dist = moonGeocentric(t).distanceKm;
    const double piDeg = rad2deg(std::asin(std::clamp(kEarthRadiusKm / dist, -1.0, 1.0)));
    return 0.7275 * piDeg - 0.5667;
}

// Bisect a sign change of f over [a, b] (f(a), f(b) straddle zero). Robust to either direction.
double bisectCrossing(const std::function<double(double)>& f, double a, double b) {
    double fa = f(a);
    for (int k = 0; k < 32; ++k) {  // 32 halvings of a 5-min bracket -> sub-microsecond in hours
        const double mid = 0.5 * (a + b);
        const double fm = f(mid);
        if ((fa < 0.0) == (fm < 0.0)) {
            a = mid;
            fa = fm;
        } else {
            b = mid;
        }
    }
    return 0.5 * (a + b);
}

// Golden-section maximiser of alt over [a, b] -> the argmax hour (for upper culmination).
double refineMaxHour(const std::function<double(double)>& alt, double a, double b) {
    constexpr double g = 0.6180339887498949;  // (sqrt5 - 1) / 2
    double c = b - g * (b - a), d = a + g * (b - a);
    double fc = alt(c), fd = alt(d);
    for (int k = 0; k < 48; ++k) {
        if (fc > fd) {
            b = d;
            d = c;
            fd = fc;
            c = b - g * (b - a);
            fc = alt(c);
        } else {
            a = c;
            c = d;
            fc = fd;
            d = a + g * (b - a);
            fd = alt(d);
        }
    }
    return 0.5 * (a + b);
}

// The core solver, shared by the Sun and Moon paths. `excess(hour)` returns (geometric altitude -
// event altitude): its ascending zero is a rise, its descending zero a set. `rawAlt(hour)` is the
// plain geometric altitude, maximised for the transit. Searches the UTC day [0, 24) at a 5-minute
// grid and refines each hit. Polar days (no rise AND no set) are flagged up/down from the sign the
// excess holds all day.
RiseSetTimes solveDayEvents(const std::function<double(double)>& excess,
                            const std::function<double(double)>& rawAlt) {
    RiseSetTimes r;
    constexpr int kSamples = 288;             // 5-minute cadence across the day
    constexpr double kStep = 24.0 / kSamples;

    double prevHour = 0.0;
    double prevExc = excess(0.0);
    bool anyAbove = prevExc >= 0.0;
    bool anyBelow = prevExc < 0.0;

    // Track the running max for the transit as we sweep (so we needn't re-sample).
    double bestHour = 0.0, bestAlt = rawAlt(0.0);

    for (int i = 1; i <= kSamples; ++i) {
        const double hour = i * kStep;  // i == kSamples -> 24.0 == next day's 0h, closing the day
        const double exc = excess(hour);
        anyAbove = anyAbove || exc >= 0.0;
        anyBelow = anyBelow || exc < 0.0;

        if (prevExc < 0.0 && exc >= 0.0 && !r.rise.valid) {
            r.rise.valid = true;
            r.rise.hourUtc = std::min(bisectCrossing(excess, prevHour, hour), 24.0 - 1e-9);
        }
        if (prevExc >= 0.0 && exc < 0.0 && !r.set.valid) {
            r.set.valid = true;
            r.set.hourUtc = std::min(bisectCrossing(excess, prevHour, hour), 24.0 - 1e-9);
        }

        const double a = rawAlt(hour);
        if (a > bestAlt) {
            bestAlt = a;
            bestHour = hour;
        }
        prevHour = hour;
        prevExc = exc;
    }

    // Upper culmination: refine around the best grid sample (always valid).
    const double lo = std::max(0.0, bestHour - kStep);
    const double hi = std::min(24.0, bestHour + kStep);
    r.transit.valid = true;
    r.transit.hourUtc = std::min(refineMaxHour(rawAlt, lo, hi), 24.0 - 1e-9);
    r.transitAltitudeDeg = rawAlt(r.transit.hourUtc);

    // Polar disambiguation: only when NEITHER a rise nor a set fell in the window (a lone event is a
    // UTC-window edge, not a polar day -- leave its partner un-flagged).
    if (!r.rise.valid && !r.set.valid) {
        if (anyAbove && !anyBelow) {
            r.rise.alwaysUp = r.set.alwaysUp = true;   // up all day: never rises/sets (circumpolar)
        } else if (anyBelow && !anyAbove) {
            r.rise.alwaysDown = r.set.alwaysDown = true;  // down all day: never rises (polar night)
        }
    }
    return r;
}

// Name the phase from the illuminated fraction + waxing/waning. The quarter/full/new points get a
// narrow band around them so a near-exact quarter reads "First Quarter" rather than "Waxing
// Crescent"; everything else is crescent/gibbous by side.
MoonPhaseName phaseName(double illuminatedFraction, bool waxing) noexcept {
    const double f = illuminatedFraction;
    if (f <= 0.04) return MoonPhaseName::New;
    if (f >= 0.96) return MoonPhaseName::Full;
    if (waxing) {
        if (f < 0.46) return MoonPhaseName::WaxingCrescent;
        if (f <= 0.54) return MoonPhaseName::FirstQuarter;
        return MoonPhaseName::WaxingGibbous;
    }
    if (f > 0.54) return MoonPhaseName::WaningGibbous;
    if (f >= 0.46) return MoonPhaseName::LastQuarter;
    return MoonPhaseName::WaningCrescent;
}

}  // namespace

const char* moonPhaseNameText(MoonPhaseName p) noexcept {
    switch (p) {
        case MoonPhaseName::New: return "New Moon";
        case MoonPhaseName::WaxingCrescent: return "Waxing Crescent";
        case MoonPhaseName::FirstQuarter: return "First Quarter";
        case MoonPhaseName::WaxingGibbous: return "Waxing Gibbous";
        case MoonPhaseName::Full: return "Full Moon";
        case MoonPhaseName::WaningGibbous: return "Waning Gibbous";
        case MoonPhaseName::LastQuarter: return "Last Quarter";
        case MoonPhaseName::WaningCrescent: return "Waning Crescent";
    }
    return "New Moon";
}

SkyAlmanac computeSkyAlmanac(const UtcTime& t, double latitudeDeg, double longitudeDeg) {
    SkyAlmanac a;
    const SunPosition sun = sunPosition(t, latitudeDeg, longitudeDeg);
    a.sunAzimuthDeg = sun.azimuthDeg;
    a.sunElevationDeg = sun.elevationDeg;

    const MoonObservation moon = moonObservation(t, latitudeDeg, longitudeDeg);
    a.moonAzimuthDeg = moon.azimuthDeg;
    a.moonElevationDeg = moon.altitudeDeg;
    a.moonIlluminatedFraction = moon.illuminatedFraction;
    a.moonAgeDays = moon.ageDays;
    a.moonWaxing = moon.ageDays < 29.530588853 / 2.0;
    a.moonPhaseName = phaseName(moon.illuminatedFraction, a.moonWaxing);

    const double e = sun.elevationDeg;
    a.isDaylight = e >= -0.833;
    a.isCivilTwilight = e < -0.833 && e >= -6.0;
    a.isNauticalTwilight = e < -6.0 && e >= -12.0;
    a.isAstroTwilight = e < -12.0 && e >= -18.0;
    a.isNight = e < -18.0;
    a.skyStateText = a.isDaylight            ? "Daylight"
                     : a.isCivilTwilight     ? "Civil twilight"
                     : a.isNauticalTwilight  ? "Nautical twilight"
                     : a.isAstroTwilight     ? "Astronomical twilight"
                                             : "Night";

    const NearestCity nc = nearestCity(latitudeDeg, longitudeDeg);
    a.nearestCityIndex = nc.index;
    a.nearestCityDistanceKm = nc.distanceKm;

    // Rise/set/transit for the UTC day containing `t` (the info panel reads these).
    a.sunDayTimes = sunRiseSetTimes(t, latitudeDeg, longitudeDeg);
    a.moonDayTimes = moonRiseSetTimes(t, latitudeDeg, longitudeDeg);
    a.civilTwilightTimes = sunTwilightTimes(t, latitudeDeg, longitudeDeg, TwilightKind::Civil);
    a.nauticalTwilightTimes = sunTwilightTimes(t, latitudeDeg, longitudeDeg, TwilightKind::Nautical);
    a.astronomicalTwilightTimes =
        sunTwilightTimes(t, latitudeDeg, longitudeDeg, TwilightKind::Astronomical);
    return a;
}

void applyMasterClock(SkyParams& sky, const UtcTime& t, double latitudeDeg, double longitudeDeg) {
    const SunPosition sun = sunPosition(t, latitudeDeg, longitudeDeg);
    sky.sunAzimuthDeg = sun.azimuthDeg;
    sky.sunElevationDeg = sun.elevationDeg;

    const MoonObservation moon = moonObservation(t, latitudeDeg, longitudeDeg);
    sky.moonAzimuthDeg = moon.azimuthDeg;
    sky.moonElevationDeg = moon.altitudeDeg;
    sky.enableMoon = true;
    sky.moonPhaseMode = 2;  // ephemeris phase, from the same observer clock recorded below

    sky.obsYear = t.year;
    sky.obsMonth = t.month;
    sky.obsDay = t.day;
    sky.obsHourUtc = t.hour;
    sky.obsLatitudeDeg = latitudeDeg;
    sky.obsLongitudeDeg = longitudeDeg;
}

RiseSetTimes sunEventsAtAltitude(const UtcTime& date, double latitudeDeg, double longitudeDeg,
                                 double targetAltDeg) {
    const int y = date.year, mo = date.month, dy = date.day;
    const auto rawAlt = [=](double hour) {
        return sunGeometricAltitudeDeg({y, mo, dy, hour}, latitudeDeg, longitudeDeg);
    };
    const auto excess = [=](double hour) { return rawAlt(hour) - targetAltDeg; };
    return solveDayEvents(excess, rawAlt);
}

RiseSetTimes sunRiseSetTimes(const UtcTime& date, double latitudeDeg, double longitudeDeg) {
    return sunEventsAtAltitude(date, latitudeDeg, longitudeDeg, kSolarStandardAltDeg);
}

RiseSetTimes sunTwilightTimes(const UtcTime& date, double latitudeDeg, double longitudeDeg,
                              TwilightKind kind) {
    const double depression = kind == TwilightKind::Civil       ? -6.0
                              : kind == TwilightKind::Nautical   ? -12.0
                                                                 : -18.0;
    return sunEventsAtAltitude(date, latitudeDeg, longitudeDeg, depression);
}

RiseSetTimes moonRiseSetTimes(const UtcTime& date, double latitudeDeg, double longitudeDeg) {
    const int y = date.year, mo = date.month, dy = date.day;
    const auto rawAlt = [=](double hour) {
        return moonGeocentricAltitudeDeg({y, mo, dy, hour}, latitudeDeg, longitudeDeg);
    };
    // Event altitude varies with the Moon's distance through the day, so subtract it per instant.
    const auto excess = [=](double hour) {
        const UtcTime t{y, mo, dy, hour};
        return moonGeocentricAltitudeDeg(t, latitudeDeg, longitudeDeg) - moonEventAltitudeDeg(t);
    };
    return solveDayEvents(excess, rawAlt);
}

double utcHourToLocal(double hourUtc, double utcOffsetHours) noexcept {
    double h = std::fmod(hourUtc + utcOffsetHours, 24.0);
    if (h < 0.0) h += 24.0;
    return h;
}

}  // namespace mosaic::core::texture
