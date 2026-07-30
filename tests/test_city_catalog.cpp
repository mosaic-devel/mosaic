#include "core/texture/city_catalog.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <numbers>

using namespace mosaic::core::texture;

namespace {

// Catalogue index of the city with this exact name, or -1.
int cityIndex(const char* name) {
    const std::size_t n = cityCatalogCount();
    for (std::size_t i = 0; i < n; ++i)
        if (std::strcmp(cityAt(i).name, name) == 0)
            return static_cast<int>(i);
    return -1;
}

constexpr double kEarthRadiusKm = 6371.0088;
constexpr double kHalfCircumferenceKm = std::numbers::pi * kEarthRadiusKm; // antipodal distance

} // namespace

TEST_CASE("city catalogue: curated, well-formed, and geographically diverse") {
    const std::size_t n = cityCatalogCount();
    CHECK(n >= 50);   // ~50 curated majors
    CHECK(n <= 80);   // curated, NOT exhaustive
    CHECK(cityCatalog() != nullptr);

    bool anyNorth = false, anySouth = false, anyFarEast = false, anyFarWest = false;
    for (std::size_t i = 0; i < n; ++i) {
        const CityEntry& c = cityAt(i);
        REQUIRE(c.name != nullptr);
        REQUIRE(c.country != nullptr);
        CHECK(std::strlen(c.name) > 0);
        CHECK(std::strlen(c.country) > 0);
        // ASCII only (no tofu on any host font) and no Fl_Choice metacharacters in the raw data.
        for (const char* p = c.name; *p != '\0'; ++p) {
            CHECK(static_cast<unsigned char>(*p) < 128);
            CHECK(*p != '/');
            CHECK(*p != '&');
        }
        CHECK(c.latitudeDeg >= -90.0f);
        CHECK(c.latitudeDeg <= 90.0f);
        CHECK(c.longitudeDeg >= -180.0f);
        CHECK(c.longitudeDeg <= 180.0f);
        anyNorth |= c.latitudeDeg > 20.0f;
        anySouth |= c.latitudeDeg < -20.0f;
        anyFarEast |= c.longitudeDeg > 100.0f;
        anyFarWest |= c.longitudeDeg < -100.0f;
    }
    CHECK(anyNorth); // both hemispheres...
    CHECK(anySouth);
    CHECK(anyFarEast); // ...and a wide longitude spread (dateline exercised)
    CHECK(anyFarWest);

    // cityAt clamps out-of-range indices to the last entry (never out of bounds).
    CHECK(&cityAt(n + 100) == &cityAt(n - 1));
}

TEST_CASE("greatCircleDistanceKm: identity, symmetry, antipode, dateline wrap") {
    // Zero distance to self.
    CHECK(greatCircleDistanceKm(48.85, 2.35, 48.85, 2.35) == doctest::Approx(0.0));

    // Symmetric.
    const double ab = greatCircleDistanceKm(40.71, -74.01, 35.68, 139.65);
    const double ba = greatCircleDistanceKm(35.68, 139.65, 40.71, -74.01);
    CHECK(ab == doctest::Approx(ba));

    // One degree along the equator is ~111.19 km.
    CHECK(greatCircleDistanceKm(0.0, 0.0, 0.0, 1.0) ==
          doctest::Approx(kEarthRadiusKm * std::numbers::pi / 180.0).epsilon(1e-6));

    // Antipodes are half the circumference apart.
    CHECK(greatCircleDistanceKm(0.0, 0.0, 0.0, 180.0) ==
          doctest::Approx(kHalfCircumferenceKm).epsilon(1e-9));
    CHECK(greatCircleDistanceKm(45.0, 45.0, -45.0, -135.0) ==
          doctest::Approx(kHalfCircumferenceKm).epsilon(1e-9));

    // THE dateline test: +179 and -179 are 2 degrees apart, not ~358. A naive coordinate delta
    // would report ~39,700 km; the great-circle value is ~222 km.
    const double wrap = greatCircleDistanceKm(0.0, 179.0, 0.0, -179.0);
    CHECK(wrap == doctest::Approx(2.0 * kEarthRadiusKm * std::numbers::pi / 180.0).epsilon(1e-6));
    CHECK(wrap < 300.0);
}

TEST_CASE("nearestCity: returns the true nearest by great-circle distance") {
    // A query at a city's own coordinates returns that city at ~0 km.
    const int tokyo = cityIndex("Tokyo");
    REQUIRE(tokyo >= 0);
    const CityEntry& t = cityAt(static_cast<std::size_t>(tokyo));
    const NearestCity atTokyo = nearestCity(t.latitudeDeg, t.longitudeDeg);
    CHECK(atTokyo.index == static_cast<std::size_t>(tokyo));
    CHECK(atTokyo.distanceKm < 1.0);

    // A point a short hop from New York still resolves to New York.
    const int ny = cityIndex("New York");
    REQUIRE(ny >= 0);
    const NearestCity nearNy = nearestCity(40.8, -73.9);
    CHECK(nearNy.index == static_cast<std::size_t>(ny));
    CHECK(nearNy.distanceKm < 30.0);

    // Returned distance is consistent with greatCircleDistanceKm to the chosen city.
    const CityEntry& chosen = cityAt(nearNy.index);
    CHECK(nearNy.distanceKm ==
          doctest::Approx(greatCircleDistanceKm(40.8, -73.9, chosen.latitudeDeg,
                                                chosen.longitudeDeg)));
}

TEST_CASE("nearestCity: great-circle beats a naive lat/lon metric across the dateline") {
    // From (0, +179): by great circle the nearest catalogue city is Honolulu (~3450 km), even though
    // its longitude (-157.86) is a naive 336 degrees away. A metric that failed to wrap longitude (or
    // ignored the sphere) would instead pick Auckland/Sydney/Tokyo. So this pins that the lookup is a
    // genuine great-circle search, not a coordinate box distance.
    const int honolulu = cityIndex("Honolulu");
    REQUIRE(honolulu >= 0);
    const NearestCity r = nearestCity(0.0, 179.0);
    CHECK(r.index == static_cast<std::size_t>(honolulu));

    // Sanity: Honolulu really is closer than the naive winner (Auckland) from this point.
    const CityEntry& h = cityAt(static_cast<std::size_t>(honolulu));
    const int auckland = cityIndex("Auckland");
    REQUIRE(auckland >= 0);
    const CityEntry& a = cityAt(static_cast<std::size_t>(auckland));
    CHECK(greatCircleDistanceKm(0.0, 179.0, h.latitudeDeg, h.longitudeDeg) <
          greatCircleDistanceKm(0.0, 179.0, a.latitudeDeg, a.longitudeDeg));
}
