#include "core/texture/city_catalog.hpp"
#include "core/texture/sky_almanac.hpp"
#include "core/texture/texture_params.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <string>

// S55 night overhaul phase 5: the master-clock engine (sky_almanac.cpp) that turns a date + place
// into the whole sky's coherent state and can stamp it onto SkyParams. Base sanity here; the
// phase-5 agent adds the rigorous ephemeris pins.
using namespace mosaic::core;

TEST_CASE("almanac: daylight vs night follows the sun, exactly one band is set") {
    // New York-ish. Local noon is ~17:00 UTC (lon -74 -> UTC+ ~5h behind); local midnight ~05:00 UTC.
    const auto noon = texture::computeSkyAlmanac({2026, 6, 21, 17.0}, 40.7, -74.0);
    CHECK(noon.sunElevationDeg > 0.0);
    CHECK(noon.isDaylight);
    CHECK(std::string(noon.skyStateText) == "Daylight");

    const auto midnight = texture::computeSkyAlmanac({2026, 6, 21, 5.0}, 40.7, -74.0);
    CHECK(midnight.sunElevationDeg < 0.0);
    CHECK_FALSE(midnight.isDaylight);

    // Exactly one classification band is ever set (they partition the elevation line).
    for (double hour = 0.0; hour < 24.0; hour += 1.0) {
        const auto a = texture::computeSkyAlmanac({2026, 3, 20, hour}, 51.5, -0.13);  // London
        const int bands = int(a.isDaylight) + int(a.isCivilTwilight) + int(a.isNauticalTwilight) +
                          int(a.isAstroTwilight) + int(a.isNight);
        CAPTURE(hour);
        CHECK(bands == 1);
    }
}

TEST_CASE("almanac: the moon phase reads out, and the name matches the fraction") {
    // Sweep a synodic month: the fraction must reach near-new and near-full, and the NAME must
    // agree with the fraction/waxing state at every step.
    double lo = 1.0, hi = 0.0;
    for (int q = 0; q < 30 * 2; ++q) {
        const int day = 1 + q / 2;
        const double hour = (q % 2) * 12.0;
        const auto a = texture::computeSkyAlmanac({2026, 3, day, hour}, 40.0, -74.0);
        lo = std::min(lo, a.moonIlluminatedFraction);
        hi = std::max(hi, a.moonIlluminatedFraction);
        CHECK(a.moonIlluminatedFraction >= 0.0);
        CHECK(a.moonIlluminatedFraction <= 1.0);
        // Name/fraction consistency: a "Full" reading must be bright, a "New" must be dark.
        if (a.moonPhaseName == texture::MoonPhaseName::Full)
            CHECK(a.moonIlluminatedFraction > 0.9);
        if (a.moonPhaseName == texture::MoonPhaseName::New)
            CHECK(a.moonIlluminatedFraction < 0.1);
        CHECK(std::string(texture::moonPhaseNameText(a.moonPhaseName)).size() > 0);
    }
    CHECK(lo < 0.05);  // passes through new
    CHECK(hi > 0.95);  // passes through full
}

TEST_CASE("almanac: nearest city resolves to a real catalogue entry") {
    const auto a = texture::computeSkyAlmanac({2026, 1, 1, 0.0}, 48.85, 2.35);  // Paris coords
    CHECK(a.nearestCityIndex < texture::cityCatalogCount());
    CHECK(a.nearestCityDistanceKm >= 0.0);
    CHECK(a.nearestCityDistanceKm < 500.0);  // Paris is in the catalogue -> very close
}

// --- Phase-5 additive: rise / set / transit, pinned against real ephemeris facts ----------------
// Reference values are published almanac data (facts, not copyrightable): timeanddate.com sun
// tables, the March 2026 total-lunar-eclipse timings, and the derivable solstice/polar geometry.

namespace {
constexpr double kMin = 1.0 / 60.0;  // one minute, in fractional hours
// |a - b| on a 24h clock (so 23.98 and 0.02 are ~2.4 min apart, not ~24h).
double clockGapHours(double a, double b) {
    double d = std::fmod(std::abs(a - b), 24.0);
    return std::min(d, 24.0 - d);
}
}  // namespace

TEST_CASE("almanac: sunrise/sunset match published London & New York times to the minute") {
    // timeanddate.com, London (51.5074 N, 0.1278 W), 2026-06-21 (summer solstice):
    //   sunrise 04:44 BST = 03:44 UTC, sunset 21:22 BST = 20:22 UTC.
    const auto lon = texture::sunRiseSetTimes({2026, 6, 21, 0.0}, 51.5074, -0.1278);
    REQUIRE(lon.rise.valid);
    REQUIRE(lon.set.valid);
    CHECK(clockGapHours(lon.rise.hourUtc, 3.0 + 44.0 * kMin) < 6.0 * kMin);   // 03:44 UTC
    CHECK(clockGapHours(lon.set.hourUtc, 20.0 + 22.0 * kMin) < 6.0 * kMin);   // 20:22 UTC
    CHECK(lon.transit.valid);
    CHECK(lon.transit.hourUtc > lon.rise.hourUtc);
    CHECK(lon.transit.hourUtc < lon.set.hourUtc);

    // New York (40.7128 N, 74.0060 W), 2026-06-21: sunrise 05:25 EDT = 09:25 UTC.
    const auto ny = texture::sunRiseSetTimes({2026, 6, 21, 0.0}, 40.7128, -74.0060);
    REQUIRE(ny.rise.valid);
    CHECK(clockGapHours(ny.rise.hourUtc, 9.0 + 25.0 * kMin) < 8.0 * kMin);    // 09:25 UTC
}

TEST_CASE("almanac: June-solstice transit altitude is 90 - |lat - 23.44| at every latitude") {
    // The solar-noon altitude on the June solstice is a geometric identity: 90 - |lat - decl|, and
    // the June declination is ~+23.44 deg (the obliquity). Our transit altitude is the airless
    // (geometric) maximum -> it must reproduce this to a fraction of a degree at any latitude.
    for (const double lat : {-23.44, 0.0, 23.44, 40.7128, 51.5074, 66.5}) {
        const auto s = texture::sunRiseSetTimes({2026, 6, 21, 0.0}, lat, 0.0);
        const double expected = 90.0 - std::abs(lat - 23.44);
        CAPTURE(lat);
        CHECK(std::abs(s.transitAltitudeDeg - expected) < 0.2);
    }
}

TEST_CASE("almanac: twilight bands nest correctly around sunrise/sunset (London equinox)") {
    // On the equinox all three bands exist. Dawn order (earliest first): astro < nautical < civil
    // < sunrise; dusk order (earliest first): sunset < civil < nautical < astro.
    const texture::UtcTime d{2026, 3, 20, 0.0};
    const double lat = 51.5074, lon = -0.1278;
    const auto sun = texture::sunRiseSetTimes(d, lat, lon);
    const auto civ = texture::sunTwilightTimes(d, lat, lon, texture::TwilightKind::Civil);
    const auto naut = texture::sunTwilightTimes(d, lat, lon, texture::TwilightKind::Nautical);
    const auto astro = texture::sunTwilightTimes(d, lat, lon, texture::TwilightKind::Astronomical);
    REQUIRE(sun.rise.valid);
    REQUIRE(civ.rise.valid);
    REQUIRE(naut.rise.valid);
    REQUIRE(astro.rise.valid);
    // Morning: light arrives astro -> nautical -> civil -> sunrise.
    CHECK(astro.rise.hourUtc < naut.rise.hourUtc);
    CHECK(naut.rise.hourUtc < civ.rise.hourUtc);
    CHECK(civ.rise.hourUtc < sun.rise.hourUtc);
    // Evening: dark falls sunset -> civil -> nautical -> astro.
    CHECK(sun.set.hourUtc < civ.set.hourUtc);
    CHECK(civ.set.hourUtc < naut.set.hourUtc);
    CHECK(naut.set.hourUtc < astro.set.hourUtc);
    // The twilights share the sun's transit.
    CHECK(civ.transit.hourUtc == doctest::Approx(sun.transit.hourUtc));
}

TEST_CASE("almanac: high-latitude midsummer has no astronomical night (band is 'always up')") {
    // London never reaches -18 deg around the solstice: astronomical twilight persists all night.
    const auto astro =
        texture::sunTwilightTimes({2026, 6, 21, 0.0}, 51.5074, -0.1278,
                                  texture::TwilightKind::Astronomical);
    CHECK_FALSE(astro.rise.valid);
    CHECK_FALSE(astro.set.valid);
    CHECK(astro.rise.alwaysUp);   // the sun stayed ABOVE -18 the whole day
    CHECK_FALSE(astro.rise.alwaysDown);
}

TEST_CASE("almanac: the polar day/night edge is reported, not silently wrong (Tromso)") {
    // Tromso, Norway (69.6492 N, 18.9553 E). Around the winter solstice the sun never rises; around
    // the summer solstice it never sets. The transit altitude still says how high it got.
    const double lat = 69.6492, lon = 18.9553;

    const auto dec = texture::sunRiseSetTimes({2026, 12, 21, 0.0}, lat, lon);
    CHECK_FALSE(dec.rise.valid);
    CHECK_FALSE(dec.set.valid);
    CHECK(dec.rise.alwaysDown);   // polar night: below the horizon all day
    CHECK_FALSE(dec.rise.alwaysUp);
    // Even at noon the sun is below the horizon: 90 - |69.6492 - (-23.44)| ~= -3.09 deg.
    CHECK(dec.transitAltitudeDeg < -0.833);
    CHECK(std::abs(dec.transitAltitudeDeg - (-3.09)) < 0.25);

    const auto jun = texture::sunRiseSetTimes({2026, 6, 21, 0.0}, lat, lon);
    CHECK_FALSE(jun.rise.valid);
    CHECK_FALSE(jun.set.valid);
    CHECK(jun.rise.alwaysUp);     // midnight sun: above the horizon all day
    CHECK(std::abs(jun.transitAltitudeDeg - (90.0 - std::abs(lat - 23.44))) < 0.2);
}

TEST_CASE("almanac: illuminated fraction pins to the 2026 full & new moon instants") {
    // 2026-03-03 total lunar eclipse -> the Moon is essentially exactly full; crest 11:38 UTC.
    const auto full = texture::computeSkyAlmanac({2026, 3, 3, 11.0 + 38.0 * kMin}, 40.0, -74.0);
    CHECK(full.moonIlluminatedFraction > 0.999);
    CHECK(full.moonPhaseName == texture::MoonPhaseName::Full);
    CHECK(std::string(texture::moonPhaseNameText(full.moonPhaseName)) == "Full Moon");

    // 2026-03-19 new moon at 01:23 UTC -> fraction ~0.
    const auto neu = texture::computeSkyAlmanac({2026, 3, 19, 1.0 + 23.0 * kMin}, 40.0, -74.0);
    CHECK(neu.moonIlluminatedFraction < 0.01);
    CHECK(neu.moonPhaseName == texture::MoonPhaseName::New);
}

TEST_CASE("almanac: moonrise/moonset/transit are physical for a full moon (up all night)") {
    // A full moon rises around sunset, transits near local solar midnight, and sets around sunrise.
    // London, 2026-03-03 (the eclipse full moon); lon ~= 0 so solar midnight ~= 00:00 UTC.
    const auto m = texture::moonRiseSetTimes({2026, 3, 3, 0.0}, 51.5074, -0.1278);
    REQUIRE(m.rise.valid);
    REQUIRE(m.set.valid);
    REQUIRE(m.transit.valid);
    // Rises in the evening (after ~15:00 UTC), sets the next morning (before ~10:00 UTC): the Moon
    // is above the horizon straight through the night.
    CHECK(m.rise.hourUtc > 15.0);
    CHECK(m.set.hourUtc < 10.0);
    // Transit is near local midnight (00:00 UTC either edge of the day).
    CHECK(clockGapHours(m.transit.hourUtc, 0.0) < 0.8);
    // And it climbs meaningfully above the horizon at culmination (moon dec ~ +7 deg, lat 51.5).
    CHECK(m.transitAltitudeDeg > 25.0);
    CHECK(m.transitAltitudeDeg < 60.0);
}

TEST_CASE("almanac: sunEventsAtAltitude is the general engine behind rise/set and twilight") {
    const texture::UtcTime d{2026, 9, 22, 0.0};  // near equinox
    const double lat = 34.05, lon = -118.24;      // Los Angeles
    // The standard sunrise/set == events at -0.8333 deg.
    const auto std0 = texture::sunEventsAtAltitude(d, lat, lon, -0.8333);
    const auto sun = texture::sunRiseSetTimes(d, lat, lon);
    CHECK(std0.rise.hourUtc == doctest::Approx(sun.rise.hourUtc));
    CHECK(std0.set.hourUtc == doctest::Approx(sun.set.hourUtc));
    // Civil twilight == events at -6 deg.
    const auto six = texture::sunEventsAtAltitude(d, lat, lon, -6.0);
    const auto civ = texture::sunTwilightTimes(d, lat, lon, texture::TwilightKind::Civil);
    CHECK(six.rise.hourUtc == doctest::Approx(civ.rise.hourUtc));
    CHECK(six.set.hourUtc == doctest::Approx(civ.set.hourUtc));
    // A higher target sun altitude is reached LATER in the morning and LEFT EARLIER in the evening.
    const auto high = texture::sunEventsAtAltitude(d, lat, lon, 10.0);
    CHECK(high.rise.hourUtc > sun.rise.hourUtc);
    CHECK(high.set.hourUtc < sun.set.hourUtc);
}

TEST_CASE("almanac: computeSkyAlmanac exposes the same rise/set the standalone solvers do") {
    const texture::UtcTime t{2026, 6, 21, 12.0};
    const double lat = 51.5074, lon = -0.1278;
    const auto a = texture::computeSkyAlmanac(t, lat, lon);
    const auto sun = texture::sunRiseSetTimes(t, lat, lon);
    const auto moon = texture::moonRiseSetTimes(t, lat, lon);
    const auto civ = texture::sunTwilightTimes(t, lat, lon, texture::TwilightKind::Civil);
    const auto naut = texture::sunTwilightTimes(t, lat, lon, texture::TwilightKind::Nautical);
    const auto astro = texture::sunTwilightTimes(t, lat, lon, texture::TwilightKind::Astronomical);
    CHECK(a.sunDayTimes.rise.hourUtc == doctest::Approx(sun.rise.hourUtc));
    CHECK(a.sunDayTimes.set.hourUtc == doctest::Approx(sun.set.hourUtc));
    CHECK(a.sunDayTimes.transitAltitudeDeg == doctest::Approx(sun.transitAltitudeDeg));
    CHECK(a.moonDayTimes.transit.hourUtc == doctest::Approx(moon.transit.hourUtc));
    CHECK(a.civilTwilightTimes.rise.valid == civ.rise.valid);
    CHECK(a.nauticalTwilightTimes.set.alwaysUp == naut.set.alwaysUp);
    CHECK(a.astronomicalTwilightTimes.rise.alwaysUp == astro.rise.alwaysUp);
}

TEST_CASE("almanac: utcHourToLocal wraps a UTC hour onto the local clock") {
    CHECK(texture::utcHourToLocal(3.744, 1.0) == doctest::Approx(4.744));    // London BST (+1)
    CHECK(texture::utcHourToLocal(9.417, -4.0) == doctest::Approx(5.417));   // New York EDT (-4)
    CHECK(texture::utcHourToLocal(1.0, -5.5) == doctest::Approx(19.5));      // wrap past midnight
    CHECK(texture::utcHourToLocal(23.0, 2.5) == doctest::Approx(1.5));       // wrap past 24
    CHECK(texture::utcHourToLocal(6.0, 5.5) == doctest::Approx(11.5));       // India (+5.5)
    // Result is always in [0, 24).
    for (double off = -12.0; off <= 14.0; off += 0.5) {
        const double h = texture::utcHourToLocal(13.37, off);
        CHECK(h >= 0.0);
        CHECK(h < 24.0);
    }
}

TEST_CASE("almanac: phase-name boundaries agree with the illuminated fraction & side") {
    // Deliverable 2: pin the phase-name boundary logic so a refinement can't silently drift.
    // Sweep a full synodic month and assert every emitted name is consistent with the fraction and
    // the waxing/waning side it must describe (crescent < ~half < gibbous; quarters bracket 0.5).
    for (int q = 0; q < 30 * 4; ++q) {
        const int day = 1 + q / 4;
        const double hour = (q % 4) * 6.0;
        const auto a = texture::computeSkyAlmanac({2026, 5, day, hour}, 40.0, -74.0);
        const double f = a.moonIlluminatedFraction;
        CAPTURE(day);
        CAPTURE(hour);
        CAPTURE(f);
        switch (a.moonPhaseName) {
            case texture::MoonPhaseName::New:            CHECK(f < 0.10); break;
            case texture::MoonPhaseName::Full:           CHECK(f > 0.90); break;
            case texture::MoonPhaseName::FirstQuarter:
            case texture::MoonPhaseName::LastQuarter:    CHECK(std::abs(f - 0.5) < 0.10); break;
            case texture::MoonPhaseName::WaxingCrescent:
                CHECK(f < 0.55);
                CHECK(a.moonWaxing);
                break;
            case texture::MoonPhaseName::WaningCrescent:
                CHECK(f < 0.55);
                CHECK_FALSE(a.moonWaxing);
                break;
            case texture::MoonPhaseName::WaxingGibbous:
                CHECK(f > 0.45);
                CHECK(a.moonWaxing);
                break;
            case texture::MoonPhaseName::WaningGibbous:
                CHECK(f > 0.45);
                CHECK_FALSE(a.moonWaxing);
                break;
        }
    }
}

TEST_CASE("almanac: applyMasterClock stamps a coherent sky onto SkyParams") {
    texture::SkyParams sky;  // defaults: daytime, moon off
    const double origFov = sky.fovDeg, origTurb = sky.turbidity;
    texture::applyMasterClock(sky, {2026, 10, 5, 2.0}, 34.05, -118.24);  // LA, ~night

    const auto a = texture::computeSkyAlmanac({2026, 10, 5, 2.0}, 34.05, -118.24);
    CHECK(sky.enableMoon);
    CHECK(sky.moonPhaseMode == 2);
    CHECK(sky.sunAzimuthDeg == doctest::Approx(a.sunAzimuthDeg));
    CHECK(sky.sunElevationDeg == doctest::Approx(a.sunElevationDeg));
    CHECK(sky.moonAzimuthDeg == doctest::Approx(a.moonAzimuthDeg));
    CHECK(sky.moonElevationDeg == doctest::Approx(a.moonElevationDeg));
    CHECK(sky.obsYear == 2026);
    CHECK(sky.obsMonth == 10);
    CHECK(sky.obsLatitudeDeg == doctest::Approx(34.05));
    // Artistic fields are untouched -- only the celestial bodies move.
    CHECK(sky.fovDeg == doctest::Approx(origFov));
    CHECK(sky.turbidity == doctest::Approx(origTurb));
}
