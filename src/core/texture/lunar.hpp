#pragma once

// Clean-room lunar + celestial-frame solver (S55 night overhaul; docs/texture-generator.md §4.2).
// The "set the moon & stars by time & place" companion to solar.{hpp,cpp}. Everything here is
// implemented from the PUBLISHED equations only -- Jean Meeus, "Astronomical Algorithms" (2nd ed.):
//   - ch. 12  Greenwich mean sidereal time (eq. 12.4)
//   - ch. 13  ecliptic <-> equatorial and equatorial -> horizontal transforms (eq. 13.3-13.6)
//   - ch. 14  parallactic angle (eq. 14.1)
//   - ch. 22  mean obliquity of the ecliptic (eq. 22.2)
//   - ch. 25  low-accuracy solar longitude/radius (for the phase geometry only)
//   - ch. 47  the Moon's geocentric position (the abridged ELP-2000/82 tables 47.A / 47.B)
//   - ch. 48  the Moon's illuminated fraction, phase angle, and bright-limb position angle
//   - ch. 53  the physical ephemeris: optical libration + the position angle of the axis
// The periodic-term tables in lunar.cpp are Meeus's Table 47.A / 47.B verbatim (published data,
// like a table of physical constants); the surrounding code is our own. No ML, no engine source.
//
// Pure functions of the inputs -- no clocks, no locale, no state, like solar.hpp. Accuracy target
// is a sky texture's, not an almanac's: the ELP truncation gives ~10" in longitude / 4" in
// latitude, and we deliberately omit nutation (~arc-seconds) and feed civil UTC as if it were
// dynamical time (dropping delta-T ~= 69 s in 2026, i.e. ~0.01 deg of lunar motion). All far
// inside the ~0.5 deg lunar disc. Reuses solar.hpp's UtcTime + julianDay.
#include "core/texture/solar.hpp"

namespace mosaic::core::texture {

// An equatorial position (right ascension / declination, degrees), mean equinox of date.
struct EquatorialCoord {
    double raDeg = 0.0;   // right ascension, [0, 360)
    double decDeg = 0.0;  // declination, [-90, 90]
};

// A position in the observer's sky. Azimuth is compass degrees CLOCKWISE FROM NORTH (0 N, 90 E,
// 180 S, 270 W) -- the SAME convention as solar.hpp's SunPosition and sky_camera's
// directionFromAzEl, so moon/stars drop straight into SkyParams alongside the sun.
struct HorizontalCoord {
    double azimuthDeg = 0.0;
    double altitudeDeg = 0.0;  // 0 = horizon, 90 = zenith, negative = below
};

// Greenwich MEAN sidereal time (degrees, [0, 360)) at the instant `t` -- Meeus eq. 12.4. This is
// the rotation of the celestial sphere: the local sidereal time at an observer is this value plus
// their east longitude, and a star's local hour angle is (local sidereal time - right ascension).
// Nutation (the mean->apparent correction, ~arc-seconds) is omitted. Exposed for the star field.
[[nodiscard]] double greenwichMeanSiderealTimeDeg(const UtcTime& t);

// Project an equatorial position into the observer's horizontal frame (Meeus eq. 13.5/13.6).
// `gmstDeg` is greenwichMeanSiderealTimeDeg(t); (latitude +N, longitude +E). With applyRefraction
// the returned altitude includes the standard-atmosphere bending near the horizon (as
// solar.cpp's sunPosition does); otherwise it is geometric. Used by the moon AND every star.
[[nodiscard]] HorizontalCoord equatorialToHorizontal(EquatorialCoord eq, double gmstDeg,
                                                     double latitudeDeg, double longitudeDeg,
                                                     bool applyRefraction = false);

// The Moon's geocentric position (Meeus ch. 47). Ecliptic longitude/latitude of date and the
// Earth-Moon centre distance, plus the derived equatorial coordinates (mean equinox of date).
struct MoonGeocentric {
    double eclLonDeg = 0.0;    // apparent geocentric ecliptic longitude (pre-nutation), [0, 360)
    double eclLatDeg = 0.0;    // ecliptic latitude
    double distanceKm = 0.0;   // Earth-Moon centre-to-centre distance
    EquatorialCoord equatorial{};
};
[[nodiscard]] MoonGeocentric moonGeocentric(const UtcTime& t);

// The Moon's phase (Meeus ch. 48), from the real sun-moon geometry at `t`.
struct MoonPhase {
    double illuminatedFraction = 0.0;  // 0 new .. 1 full (eq. 48.1)
    double phaseAngleDeg = 0.0;        // Sun-Moon-Earth angle (eq. 48.3), 0 full .. 180 new
    double brightLimbAngleDeg = 0.0;   // position angle of the midpoint of the bright limb,
                                       // measured from the north celestial pole toward east
                                       // (eq. 48.5), [0, 360)
    double ageDays = 0.0;              // synodic age (0 at new .. ~29.53), for phase-name readouts
};
[[nodiscard]] MoonPhase moonPhase(const UtcTime& t);

// The Moon's physical ephemeris (Meeus ch. 53): how the near side is turned toward Earth at `t`.
// These are geocentric, date-only quantities (no observer location) -- they orient the real LRO
// surface texture on the rendered disc. Only the OPTICAL librations are computed; the physical
// librations (~0.02 deg) and nutation are omitted, consistent with the arc-second simplifications
// elsewhere here (well inside the ~0.5 deg disc). Pinned against Meeus example 53.a.
struct MoonPhysical {
    double librationLonDeg = 0.0;  // optical libration in longitude l: the selenographic longitude
                                   // of the sub-Earth point (the surface point at the disc centre)
    double librationLatDeg = 0.0;  // optical libration in latitude b: its selenographic latitude
    double axisPositionAngleDeg =
        0.0;  // P: position angle of the Moon's north rotation axis, measured from the north
              // celestial pole toward the east (the on-sky tilt of the lunar meridian), [-180, 180]
};
[[nodiscard]] MoonPhysical moonPhysical(const UtcTime& t);

// The fully-observed Moon for the dialog and renderer: topocentric apparent az/el (parallax- and
// refraction-corrected), the phase, distance, and the parallactic angle that rotates the near-side
// texture upright against the sky. One call turns date + place into everything SkyParams' moon
// needs. (Topocentric parallax uses the dominant altitude term, Meeus ch. 40; the sub-degree
// azimuth cross-term is omitted -- invisible at the lunar disc's scale.)
struct MoonObservation {
    double azimuthDeg = 0.0;
    double altitudeDeg = 0.0;           // apparent (parallax + refraction corrected)
    double illuminatedFraction = 0.0;
    double phaseAngleDeg = 0.0;
    double brightLimbAngleDeg = 0.0;    // position angle of the bright limb (from N, toward E)
    double parallacticAngleDeg = 0.0;   // Meeus eq. 14.1: sky-vertical vs. celestial-north
    double librationLonDeg = 0.0;       // ch. 53 optical libration in longitude (sub-Earth lon)
    double librationLatDeg = 0.0;       // ch. 53 optical libration in latitude (sub-Earth lat)
    double axisPositionAngleDeg = 0.0;  // ch. 53 P: lunar north axis, from celestial N toward E
    double distanceKm = 0.0;
    double ageDays = 0.0;
};
[[nodiscard]] MoonObservation moonObservation(const UtcTime& t, double latitudeDeg,
                                              double longitudeDeg);

}  // namespace mosaic::core::texture
