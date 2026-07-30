#include "core/texture/city_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mosaic::core::texture {
namespace {

// ~50 major world cities, geographically diverse across every continent and both hemispheres, and
// spread across the full longitude range (Auckland +174.8 .. Honolulu -157.9 .. Anchorage -149.9)
// so the great-circle lookup is exercised near the antimeridian. Coordinates are city-centre
// latitude (+N) / longitude (+E) to ~4 decimals -- ample for "which city is nearest" and for
// seeding a solar calculation (the sun solver wants a place, not a survey mark). Names are ASCII
// transliterations (Sao Paulo, Reykjavik) so they render on any host font (the no-tofu rule).
const CityEntry kCities[] = {
    // North & Central America
    {"New York", "United States", 40.7128f, -74.0060f},
    {"Los Angeles", "United States", 34.0522f, -118.2437f},
    {"Chicago", "United States", 41.8781f, -87.6298f},
    {"Honolulu", "United States", 21.3069f, -157.8583f},
    {"Anchorage", "United States", 61.2181f, -149.9003f},
    {"Toronto", "Canada", 43.6532f, -79.3832f},
    {"Vancouver", "Canada", 49.2827f, -123.1207f},
    {"Mexico City", "Mexico", 19.4326f, -99.1332f},
    // South America
    {"Bogota", "Colombia", 4.7110f, -74.0721f},
    {"Lima", "Peru", -12.0464f, -77.0428f},
    {"Santiago", "Chile", -33.4489f, -70.6693f},
    {"Buenos Aires", "Argentina", -34.6037f, -58.3816f},
    {"Sao Paulo", "Brazil", -23.5505f, -46.6333f},
    {"Rio de Janeiro", "Brazil", -22.9068f, -43.1729f},
    // Europe
    {"Reykjavik", "Iceland", 64.1466f, -21.9426f},
    {"Dublin", "Ireland", 53.3498f, -6.2603f},
    {"London", "United Kingdom", 51.5074f, -0.1278f},
    {"Lisbon", "Portugal", 38.7223f, -9.1393f},
    {"Madrid", "Spain", 40.4168f, -3.7038f},
    {"Paris", "France", 48.8566f, 2.3522f},
    {"Berlin", "Germany", 52.5200f, 13.4050f},
    {"Rome", "Italy", 41.9028f, 12.4964f},
    {"Stockholm", "Sweden", 59.3293f, 18.0686f},
    {"Athens", "Greece", 37.9838f, 23.7275f},
    {"Istanbul", "Turkey", 41.0082f, 28.9784f},
    {"Moscow", "Russia", 55.7558f, 37.6173f},
    // Africa
    {"Casablanca", "Morocco", 33.5731f, -7.5898f},
    {"Lagos", "Nigeria", 6.5244f, 3.3792f},
    {"Cairo", "Egypt", 30.0444f, 31.2357f},
    {"Nairobi", "Kenya", -1.2921f, 36.8219f},
    {"Addis Ababa", "Ethiopia", 9.0300f, 38.7469f},
    {"Johannesburg", "South Africa", -26.2041f, 28.0473f},
    {"Cape Town", "South Africa", -33.9249f, 18.4241f},
    // Middle East & Central/South Asia
    {"Tehran", "Iran", 35.6892f, 51.3890f},
    {"Dubai", "United Arab Emirates", 25.2048f, 55.2708f},
    {"Karachi", "Pakistan", 24.8607f, 67.0011f},
    {"Mumbai", "India", 19.0760f, 72.8777f},
    {"Delhi", "India", 28.6139f, 77.2090f},
    {"Dhaka", "Bangladesh", 23.8103f, 90.4125f},
    // East & Southeast Asia
    {"Bangkok", "Thailand", 13.7563f, 100.5018f},
    {"Singapore", "Singapore", 1.3521f, 103.8198f},
    {"Jakarta", "Indonesia", -6.2088f, 106.8456f},
    {"Hong Kong", "China", 22.3193f, 114.1694f},
    {"Beijing", "China", 39.9042f, 116.4074f},
    {"Shanghai", "China", 31.2304f, 121.4737f},
    {"Manila", "Philippines", 14.5995f, 120.9842f},
    {"Seoul", "South Korea", 37.5665f, 126.9780f},
    {"Tokyo", "Japan", 35.6762f, 139.6503f},
    // Oceania
    {"Perth", "Australia", -31.9505f, 115.8605f},
    {"Melbourne", "Australia", -37.8136f, 144.9631f},
    {"Sydney", "Australia", -33.8688f, 151.2093f},
    {"Auckland", "New Zealand", -36.8485f, 174.7633f},
};

constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kEarthRadiusKm = 6371.0088;  // IUGG mean radius

}  // namespace

const CityEntry* cityCatalog() noexcept { return kCities; }
std::size_t cityCatalogCount() noexcept { return sizeof(kCities) / sizeof(kCities[0]); }

const CityEntry& cityAt(std::size_t index) noexcept {
    const std::size_t n = cityCatalogCount();
    return kCities[index < n ? index : n - 1];
}

double greatCircleDistanceKm(double lat1Deg, double lon1Deg, double lat2Deg,
                             double lon2Deg) noexcept {
    const double phi1 = lat1Deg * kDegToRad;
    const double phi2 = lat2Deg * kDegToRad;
    const double dPhi = (lat2Deg - lat1Deg) * kDegToRad;
    // Longitude delta: the cos-of-half-delta term is 2*pi-periodic, so an antimeridian-spanning
    // pair (e.g. +179 and -179) folds to the true 2-degree gap with no explicit wrap arithmetic.
    const double dLambda = (lon2Deg - lon1Deg) * kDegToRad;
    const double sinDPhi = std::sin(dPhi * 0.5);
    const double sinDLam = std::sin(dLambda * 0.5);
    const double a =
        sinDPhi * sinDPhi + std::cos(phi1) * std::cos(phi2) * sinDLam * sinDLam;
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
    return kEarthRadiusKm * c;
}

NearestCity nearestCity(double latDeg, double lonDeg) noexcept {
    NearestCity best;
    best.index = 0;
    best.distanceKm = greatCircleDistanceKm(latDeg, lonDeg, kCities[0].latitudeDeg,
                                            kCities[0].longitudeDeg);
    const std::size_t n = cityCatalogCount();
    for (std::size_t i = 1; i < n; ++i) {
        const double d = greatCircleDistanceKm(latDeg, lonDeg, kCities[i].latitudeDeg,
                                               kCities[i].longitudeDeg);
        if (d < best.distanceKm) {
            best.distanceKm = d;
            best.index = i;
        }
    }
    return best;
}

}  // namespace mosaic::core::texture
