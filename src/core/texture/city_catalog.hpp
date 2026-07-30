#pragma once

#include <cstddef>

// A curated table of major world cities + a nearest-city (great-circle) lookup. Pure logic (no
// FLTK, no clocks, no locale) so it is unit-testable headlessly. The Texture Generator's solar
// calculator (docs/texture-generator.md §4.2, core/texture/solar.hpp) drives sun position from
// (latitude +N, longitude +E); this lets the dialog offer a "pick a city" shortcut instead of
// making the user type coordinates, and reflect the nearest city when a coordinate is entered by
// hand or by dragging the sun gizmo.
//
// The list is deliberately CURATED (~50), not exhaustive: geographically diverse across every
// continent and both hemispheres, spanning the dateline so the great-circle lookup is exercised
// (a naive lat/lon distance breaks near +/-180). Accessor shape mirrors star_catalog.hpp /
// solar.hpp (count + entry-by-index), so the dialog can populate a dropdown by index.
namespace mosaic::core::texture {

struct CityEntry {
    const char* name;       // city name (ASCII, so it renders on any host font -- no tofu risk)
    const char* country;    // country / region (ASCII)
    float latitudeDeg;      // +N, degrees [-90, 90]
    float longitudeDeg;     // +E, degrees [-180, 180]
};

// The curated catalogue (stable order; index is a durable handle for the UI).
[[nodiscard]] const CityEntry* cityCatalog() noexcept;
[[nodiscard]] std::size_t cityCatalogCount() noexcept;
// Entry by index, clamped into [0, count) (never out of range).
[[nodiscard]] const CityEntry& cityAt(std::size_t index) noexcept;

// Great-circle (haversine) distance in kilometres between two (lat +N, lon +E) points. Handles the
// antimeridian wrap correctly (unlike a naive coordinate delta) and is symmetric. Pure.
[[nodiscard]] double greatCircleDistanceKm(double lat1Deg, double lon1Deg, double lat2Deg,
                                           double lon2Deg) noexcept;

struct NearestCity {
    std::size_t index = 0;       // index into cityCatalog()
    double distanceKm = 0.0;     // great-circle distance from the query point to that city
};

// The catalogue city closest to (latDeg +N, lonDeg +E) by great-circle distance. The catalogue is
// non-empty, so this always returns a valid entry. Pure.
[[nodiscard]] NearestCity nearestCity(double latDeg, double lonDeg) noexcept;

}  // namespace mosaic::core::texture
