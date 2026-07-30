#pragma once

// Clean-room solar-position solver (S55-b; docs/texture-generator.md §4.2) -- the "set the sun
// by time & place" half of the sun-positioning story. Implemented from the PUBLISHED equations
// only: the NOAA Solar Calculator equation set (gml.noaa.gov/grad/solcalc), which derives from
// Meeus, "Astronomical Algorithms" (2nd ed., ch. 22/25/28). ⚠ NEVER port NREL's spa.c here --
// its header is noncommercial/no-redistribute, doubly GPL-incompatible (§9.2); a sky needs
// arc-minute accuracy, not SPA's ±0.0003°, and this solver delivers a few hundredths of a
// degree. Self-owned, GPLv3, no dependency.
//
// Pure functions of the inputs -- no clocks, no locale, no state. The dialog (S55-f) binds this
// to the sun gizmo: date/time/lat/long in, azimuth/elevation out, feeding SkyParams'
// sunAzimuthDeg/sunElevationDeg.
namespace mosaic::core::texture {

// A civil UTC timestamp (proleptic Gregorian). The caller owns any local-time -> UTC offset;
// keeping the solver in UTC sidesteps every timezone/DST question.
struct UtcTime {
    int year = 2026;
    int month = 6;       // 1..12
    int day = 21;        // 1..31
    double hour = 12.0;  // fractional hours, 0..24
};

struct SunPosition {
    double azimuthDeg = 0.0;    // clockwise from true north: 0 N, 90 E, 180 S, 270 W
    double elevationDeg = 0.0;  // apparent (refraction-corrected) elevation; negative = below horizon
};

// Days since the epoch J2000.0 (2000-01-01 12:00 TT), as a Julian Day number. Exposed for tests.
[[nodiscard]] double julianDay(const UtcTime& t);

// The sun's position seen from (latitudeDeg +N, longitudeDeg +E) at `t`. Accuracy ~0.01° in
// elevation over 1900..2100 (plus the inherent ~0.1° wobble of real refraction vs the standard
// atmosphere the correction assumes) -- far inside the arc-minute budget.
[[nodiscard]] SunPosition sunPosition(const UtcTime& t, double latitudeDeg, double longitudeDeg);

// The two scalar drivers, exposed for tests and the dialog's readouts: solar declination and
// the equation of time (true solar minus mean solar time, minutes) for the moment `t`.
[[nodiscard]] double solarDeclinationDeg(const UtcTime& t);
[[nodiscard]] double equationOfTimeMinutes(const UtcTime& t);

}  // namespace mosaic::core::texture
