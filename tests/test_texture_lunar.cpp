#include "core/texture/lunar.hpp"
#include "core/texture/sky_camera.hpp"
#include "core/texture/solar.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>

// S55 night overhaul: the clean-room Meeus lunar + celestial-frame solver (lunar.cpp), pinned
// against Meeus's OWN worked examples (the same discipline as the solar tests in
// test_texture_sky.cpp) plus geometric sanity properties. If a Table 47.A/47.B coefficient is
// mistranscribed, example 47.a/48.a stops reproducing -- that is the whole point of these pins.

using namespace mosaic::core;

TEST_CASE("lunar: Moon position reproduces Meeus example 47.a") {
    // Meeus, Astronomical Algorithms, example 47.a: 1992 April 12, 0h TD (JDE 2448724.5).
    // Book results: lambda = 133.162655 deg, beta = -3.229126 deg, Delta = 368409.7 km.
    const auto m = texture::moonGeocentric({1992, 4, 12, 0.0});
    CHECK(std::abs(m.eclLonDeg - 133.162655) < 1e-4);
    CHECK(std::abs(m.eclLatDeg - (-3.229126)) < 1e-4);
    CHECK(std::abs(m.distanceKm - 368409.7) < 1.0);
}

TEST_CASE("lunar: illuminated fraction & phase angle reproduce Meeus example 48.a") {
    // Example 48.a, same instant: phase angle i = 69.0756 deg, illuminated fraction k = 0.6786.
    const auto p = texture::moonPhase({1992, 4, 12, 0.0});
    CHECK(std::abs(p.phaseAngleDeg - 69.0756) < 0.02);
    CHECK(std::abs(p.illuminatedFraction - 0.6786) < 0.0005);
    // k and i are not independent -- 48.1 must hold exactly for our own outputs.
    CHECK(p.illuminatedFraction ==
          doctest::Approx((1.0 + std::cos(p.phaseAngleDeg * std::numbers::pi / 180.0)) / 2.0));
    CHECK(p.brightLimbAngleDeg >= 0.0);
    CHECK(p.brightLimbAngleDeg < 360.0);
    CHECK(p.ageDays >= 0.0);
    CHECK(p.ageDays < 30.0);  // synodic month sanity bound only
}

TEST_CASE("lunar: optical libration & axis position angle reproduce Meeus example 53.a") {
    // Example 53.a, 1992 April 12, 0h TD: optical libration l' = -1.206 deg, b' = +4.194 deg,
    // and the position angle of the axis P = 15.08 deg. We evaluate only the OPTICAL librations
    // (the physical terms ~0.02 deg are omitted, like nutation), so l/b land essentially exactly
    // and P within that tolerance of the book value.
    const auto mp = texture::moonPhysical({1992, 4, 12, 0.0});
    CHECK(std::abs(mp.librationLonDeg - (-1.206)) < 0.01);
    CHECK(std::abs(mp.librationLatDeg - 4.194) < 0.01);
    CHECK(std::abs(mp.axisPositionAngleDeg - 15.08) < 0.1);

    // The librations stay inside their physical envelope (~ +-8 deg optical) at any date, and P
    // inside its (~ +-27 deg) -- a mistranscribed node/obliquity term would blow past these.
    for (int m = 1; m <= 12; ++m) {
        const auto q = texture::moonPhysical({2026, m, 15, 0.0});
        CHECK(std::abs(q.librationLonDeg) < 12.0);
        CHECK(std::abs(q.librationLatDeg) < 12.0);
        CHECK(std::abs(q.axisPositionAngleDeg) < 35.0);
    }

    // moonObservation folds the same ch.53 values in for the dialog/renderer.
    const auto obs = texture::moonObservation({1992, 4, 12, 0.0}, 40.0, -74.0);
    CHECK(obs.librationLonDeg == doctest::Approx(mp.librationLonDeg));
    CHECK(obs.librationLatDeg == doctest::Approx(mp.librationLatDeg));
    CHECK(obs.axisPositionAngleDeg == doctest::Approx(mp.axisPositionAngleDeg));
}

TEST_CASE("lunar: Greenwich mean sidereal time reproduces Meeus examples 12.a / 12.b") {
    // Example 12.a: 1987 April 10 at 0h UT -> theta0 = 197.693195 deg.
    CHECK(std::abs(texture::greenwichMeanSiderealTimeDeg({1987, 4, 10, 0.0}) - 197.693195) < 1e-4);
    // Example 12.b: same date at 19h21m00s UT -> theta0 = 128.7378735 deg.
    const double h = 19.0 + 21.0 / 60.0;
    CHECK(std::abs(texture::greenwichMeanSiderealTimeDeg({1987, 4, 10, h}) - 128.7378735) < 1e-3);
}

TEST_CASE("lunar: equatorial->horizontal geometry lands where the sky says") {
    // A star on the observer's meridian (H = 0 -> RA = local sidereal time) culminates due south
    // at altitude 90 - (lat - dec). Put gmst = 0, longitude 0, latitude 45, RA 0.
    const auto onEq = texture::equatorialToHorizontal({0.0, 0.0}, 0.0, 45.0, 0.0);
    CHECK(onEq.altitudeDeg == doctest::Approx(45.0).epsilon(1e-9));   // 90 - 45
    CHECK(onEq.azimuthDeg == doctest::Approx(180.0).epsilon(1e-6));   // due south

    // A star whose declination equals the latitude, on the meridian, passes through the zenith.
    const auto zenith = texture::equatorialToHorizontal({0.0, 45.0}, 0.0, 45.0, 0.0);
    CHECK(zenith.altitudeDeg == doctest::Approx(90.0).epsilon(1e-6));

    // An equatorial star three hours before the meridian (H = -45 deg -> RA = 45) rises in the
    // east from the equator.
    const auto rising = texture::equatorialToHorizontal({45.0, 0.0}, 0.0, 0.0, 0.0);
    CHECK(rising.azimuthDeg > 45.0);
    CHECK(rising.azimuthDeg < 135.0);  // eastern half
}

TEST_CASE("lunar: full observation is self-consistent and location-aware") {
    // The topocentric moon should sit within ~1.1 deg (parallax + refraction) of the geocentric
    // horizontal position for the same instant/place, and never NaN.
    const texture::UtcTime t{2026, 7, 15, 3.0};
    const auto obs = texture::moonObservation(t, 40.0, -74.0);  // New York-ish
    const auto geoEq = texture::moonGeocentric(t).equatorial;
    const auto geoHz = texture::equatorialToHorizontal(
        geoEq, texture::greenwichMeanSiderealTimeDeg(t), 40.0, -74.0);
    CHECK(std::isfinite(obs.altitudeDeg));
    CHECK(std::isfinite(obs.azimuthDeg));
    CHECK(obs.azimuthDeg == doctest::Approx(geoHz.azimuthDeg).epsilon(1e-9));  // azimuth unshifted
    CHECK(obs.altitudeDeg <= geoHz.altitudeDeg + 0.05);  // parallax pulls it DOWN (refraction < it)
    CHECK(geoHz.altitudeDeg - obs.altitudeDeg < 1.2);    // by at most ~1 deg
    CHECK(obs.illuminatedFraction >= 0.0);
    CHECK(obs.illuminatedFraction <= 1.0);

    // Moving the observer to the opposite hemisphere changes the altitude materially (the whole
    // point of "tied to location"): the same moment cannot be equally high everywhere.
    const auto south = texture::moonObservation(t, -40.0, -74.0);
    CHECK(std::abs(south.altitudeDeg - obs.altitudeDeg) > 5.0);
}

TEST_CASE("lunar: the phase sweeps from new to full across a synodic month") {
    // Physically guaranteed within any ~29.5-day window: the illuminated fraction must reach near
    // 0 (new) and near 1 (full). Sweep a month at 6h steps so we don't depend on a memorised date.
    double lo = 1.0, hi = 0.0;
    for (int q = 0; q < 30 * 4; ++q) {
        const double hour = (q % 4) * 6.0;
        const int day = 1 + q / 4;
        const auto p = texture::moonPhase({2026, 3, day, hour});
        lo = std::min(lo, p.illuminatedFraction);
        hi = std::max(hi, p.illuminatedFraction);
    }
    CHECK(lo < 0.02);  // passes through new moon
    CHECK(hi > 0.98);  // passes through full moon
}
