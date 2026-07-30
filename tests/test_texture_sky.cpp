#include "core/texture/atmosphere.hpp"
#include "core/texture/cloud_volume.hpp"
#include "core/texture/moon_elevation.hpp"
#include "core/texture/moon_texture.hpp"
#include "core/texture/sky_camera.hpp"
#include "core/texture/sky_render.hpp"
#include "core/texture/solar.hpp"
#include "core/texture/star_catalog.hpp"
#include "core/texture/texture_params.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>

// S55-b: the Hosek-Wilkie sky subsystem (docs/texture-generator.md §4) -- the clean-room
// NOAA/Meeus solar solver against published astronomical facts, the §4.5 perspective camera's
// geometry, and property tests on the renderer (solar reddening, alpha carry, deck behaviour).
// The byte-golden for the full render lives with the other generators in test_texture_layer.cpp.
namespace {

using namespace mosaic;
namespace texture = core::texture;

constexpr double kRadToDeg = 180.0 / std::numbers::pi;

double rayElevationDeg(const texture::SkyVec3& d) {
    return std::asin(d.z) * kRadToDeg;
}
double rayAzimuthDeg(const texture::SkyVec3& d) {
    const double az = std::atan2(d.x, d.y) * kRadToDeg;
    return az < 0.0 ? az + 360.0 : az;
}

double pixelLuma(const common::ImageF& img, std::uint32_t x, std::uint32_t y) {
    const auto c = img.at(x, y);
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
}

double rowLuma(const common::ImageF& img, std::uint32_t y) {
    double sum = 0.0;
    for (std::uint32_t x = 0; x < img.width; ++x) sum += pixelLuma(img, x, y);
    return sum / img.width;
}

}  // namespace

// ---- solar (clean-room NOAA/Meeus) -----------------------------------------------------------

TEST_CASE("solar: Julian Day reproduces Meeus's own worked example") {
    // Meeus, Astronomical Algorithms, example 7.a: 1957 October 4.81 = JD 2436116.31.
    CHECK(texture::julianDay({1957, 10, 4, 0.81 * 24.0}) == doctest::Approx(2436116.31).epsilon(1e-9));
    // J2000.0 epoch: 2000 January 1.5 = JD 2451545.0.
    CHECK(texture::julianDay({2000, 1, 1, 12.0}) == doctest::Approx(2451545.0));
}

TEST_CASE("solar: declination hits the tropics at the solstices and zero at the equinox") {
    CHECK(texture::solarDeclinationDeg({2026, 6, 21, 12.0}) == doctest::Approx(23.44).epsilon(0.002));
    CHECK(texture::solarDeclinationDeg({2026, 12, 21, 12.0}) == doctest::Approx(-23.44).epsilon(0.002));
    // 2026 March equinox is ~14:46 UTC: declination crosses zero within a few hundredths.
    CHECK(std::abs(texture::solarDeclinationDeg({2026, 3, 20, 15.0})) < 0.05);
}

TEST_CASE("solar: equation of time shows the November/February extremes") {
    CHECK(texture::equationOfTimeMinutes({2026, 11, 3, 12.0}) == doctest::Approx(16.5).epsilon(0.02));
    CHECK(texture::equationOfTimeMinutes({2026, 2, 12, 12.0}) == doctest::Approx(-14.2).epsilon(0.02));
    // Early September sits near the zero crossing.
    CHECK(std::abs(texture::equationOfTimeMinutes({2026, 9, 1, 12.0})) < 0.5);
}

TEST_CASE("solar: positions land where the sky says they should") {
    // Equator, equinox, solar noon (clock noon + ~7.5 min of equation-of-time): overhead.
    const auto overhead = texture::sunPosition({2026, 3, 20, 12.0 + 7.5 / 60.0}, 0.0, 0.0);
    CHECK(overhead.elevationDeg > 89.5);

    // 40°N summer-solstice noon: elevation 90 - (40 - 23.44), azimuth due south.
    const auto noon40 = texture::sunPosition({2026, 6, 21, 12.0 + 2.0 / 60.0}, 40.0, 0.0);
    CHECK(noon40.elevationDeg == doctest::Approx(73.4).epsilon(0.005));
    CHECK(noon40.azimuthDeg == doctest::Approx(180.0).epsilon(0.005));

    // London mid-summer morning: sun climbing in the east.
    const auto london = texture::sunPosition({2026, 6, 21, 8.0}, 51.5, -0.13);
    CHECK(london.elevationDeg == doctest::Approx(36.3).epsilon(0.01));
    CHECK(london.azimuthDeg == doctest::Approx(97.5).epsilon(0.01));

    // Sydney (southern hemisphere) winter noon: the sun sits to the NORTH.
    const auto sydney = texture::sunPosition({2026, 6, 21, 2.0}, -33.87, 151.21);
    CHECK(sydney.elevationDeg == doctest::Approx(32.7).epsilon(0.01));
    CHECK((sydney.azimuthDeg > 350.0 || sydney.azimuthDeg < 10.0));

    // Equinox sunset at the equator: due west, and refraction holds the apparent disc ~0.5°
    // above the geometric horizon.
    const auto sunset = texture::sunPosition({2026, 3, 20, 18.0 + 7.0 / 60.0}, 0.0, 0.0);
    CHECK(sunset.azimuthDeg == doctest::Approx(270.0).epsilon(0.005));
    CHECK(sunset.elevationDeg > 0.3);
    CHECK(sunset.elevationDeg < 0.8);
}

// ---- the §4.5 perspective camera --------------------------------------------------------------

TEST_CASE("sky camera: centre ray follows pitch; azimuth grows to screen-right") {
    texture::SkyParams s;  // pitch 12, fov 62, no roll/shift
    s.pitchDeg = 0.0;
    auto cam = texture::SkyCamera::fromParams(s, 200, 150);
    const auto centre = cam.rayAt(100.0, 75.0);
    CHECK(rayElevationDeg(centre) == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(rayAzimuthDeg(centre) == doctest::Approx(180.0));

    // Pitch aims the principal ray.
    s.pitchDeg = 30.0;
    cam = texture::SkyCamera::fromParams(s, 200, 150);
    CHECK(rayElevationDeg(cam.rayAt(100.0, 75.0)) == doctest::Approx(30.0));

    // Right of centre = west of south (azimuth > 180): the S55-a left-to-right azimuth sense.
    CHECK(rayAzimuthDeg(cam.rayAt(160.0, 75.0)) > 180.0);
    CHECK(rayAzimuthDeg(cam.rayAt(40.0, 75.0)) < 180.0);

    // Wider FOV bends the corner rays farther from the principal ray.
    texture::SkyParams wide = s;
    wide.fovDeg = 100.0;
    const auto camWide = texture::SkyCamera::fromParams(wide, 200, 150);
    const double narrowOff = rayAzimuthDeg(cam.rayAt(0.5, 75.0));
    const double wideOff = rayAzimuthDeg(camWide.rayAt(0.5, 75.0));
    CHECK(std::abs(wideOff - 180.0) > std::abs(narrowOff - 180.0));
}

TEST_CASE("sky camera: roll tilts the horizon; tilt-shift slides it without pitching") {
    texture::SkyParams s;
    s.pitchDeg = 10.0;
    // No roll: a screen row is an iso-elevation line (up to projection symmetry).
    auto cam = texture::SkyCamera::fromParams(s, 200, 150);
    CHECK(rayElevationDeg(cam.rayAt(30.0, 40.0)) ==
          doctest::Approx(rayElevationDeg(cam.rayAt(170.0, 40.0))).epsilon(1e-9));

    // Positive roll: the horizon tilts clockwise (left end up on screen). At a fixed row that
    // reads as LOWER ray elevation on the left -- the left pixel sits closer to its (raised)
    // horizon than the right pixel does to its (dropped) one.
    s.rollDeg = 8.0;
    cam = texture::SkyCamera::fromParams(s, 200, 150);
    CHECK(rayElevationDeg(cam.rayAt(30.0, 40.0)) < rayElevationDeg(cam.rayAt(170.0, 40.0)));

    // Positive shiftY raises every ray (the horizon moves DOWN the frame) without touching the
    // horizontal direction.
    s.rollDeg = 0.0;
    s.shiftY = 0.2;
    const auto shifted = texture::SkyCamera::fromParams(s, 200, 150);
    texture::SkyParams unshifted = s;
    unshifted.shiftY = 0.0;
    CHECK(rayElevationDeg(shifted.rayAt(100.0, 75.0)) >
          rayElevationDeg(texture::SkyCamera::fromParams(unshifted, 200, 150).rayAt(100.0, 75.0)));
    CHECK(rayAzimuthDeg(shifted.rayAt(100.0, 75.0)) == doctest::Approx(180.0));
}

TEST_CASE("sky camera: directionFromAzEl round-trips") {
    const auto d = texture::directionFromAzEl(135.0, 40.0);
    CHECK(rayAzimuthDeg(d) == doctest::Approx(135.0));
    CHECK(rayElevationDeg(d) == doctest::Approx(40.0));
}

// ---- renderer properties ----------------------------------------------------------------------

namespace {

texture::TextureParams skyOnly(std::uint64_t seed = 7) {
    texture::TextureParams p = texture::defaultTextureParams(texture::Generator::Sky);
    p.seed = seed;
    return p;
}

texture::SkyParams& spec(texture::TextureParams& p) {
    return std::get<texture::SkyParams>(p.spec);
}

}  // namespace

TEST_CASE("sky render: deterministic; seed moves the clouds") {
    texture::TextureParams p = skyOnly();
    const auto a = texture::renderSkyTexture(p, spec(p), 48, 36);
    const auto b = texture::renderSkyTexture(p, spec(p), 48, 36);
    CHECK(a.rgba == b.rgba);

    texture::TextureParams reseeded = skyOnly(8);
    const auto c = texture::renderSkyTexture(reseeded, spec(reseeded), 48, 36);
    CHECK(a.rgba != c.rgba);
}

TEST_CASE("sky render: the low sun reddens through Beer-Lambert; the high sun stays white") {
    // Sun-only renders (dome/clouds/haze off): the disc is the only ink, so the brightest
    // pixel IS the disc. Beer-Lambert over ~15 air masses at 3° elevation must push R far
    // above B; at 30° (2 air masses) the HDR core clips toward white.
    texture::TextureParams p = skyOnly();
    auto& s = spec(p);
    s.enableDome = false;
    s.enableClouds = false;
    s.enableHaze = false;
    s.sunDiscScale = 3.0;  // fat disc so a 64x48 frame resolves it
    s.pitchDeg = 10.0;

    s.sunElevationDeg = 3.0;
    const auto low = texture::renderSkyTexture(p, s, 64, 48);
    s.sunElevationDeg = 30.0;
    const auto high = texture::renderSkyTexture(p, s, 64, 48);

    const auto brightest = [](const common::ImageF& img) {
        common::ColorF best{};
        for (std::uint32_t y = 0; y < img.height; ++y)
            for (std::uint32_t x = 0; x < img.width; ++x) {
                const auto c = img.at(x, y);
                if (c.a > best.a || (c.a == best.a && c.r > best.r)) best = c;
            }
        return best;
    };
    const auto lowC = brightest(low);
    const auto highC = brightest(high);
    REQUIRE(lowC.a > 0.5f);   // the disc is in frame
    REQUIRE(highC.a > 0.5f);
    CHECK(lowC.r - lowC.b > 0.2f);   // sunset-red disc
    CHECK(highC.r - highC.b < 0.1f); // near-white disc
    CHECK(highC.r > 0.9f);
}

TEST_CASE("sky render: alpha carry survives the camera (§3.4)") {
    texture::TextureParams p = skyOnly();
    auto& s = spec(p);

    // Dome on: opaque everywhere, including the below-horizon ground band.
    const auto full = texture::renderSkyTexture(p, s, 40, 30);
    for (std::uint32_t y = 0; y < full.height; ++y)
        for (std::uint32_t x = 0; x < full.width; ++x)
            CHECK(full.at(x, y).a == doctest::Approx(1.0f));

    // Everything off: genuine transparency.
    s.enableDome = s.enableSun = s.enableClouds = s.enableHaze = false;
    const auto none = texture::renderSkyTexture(p, s, 40, 30);
    for (std::uint32_t y = 0; y < none.height; ++y)
        for (std::uint32_t x = 0; x < none.width; ++x)
            CHECK(none.at(x, y).a == 0.0f);
}

TEST_CASE("sky render: dome shows blue zenith, bright horizon, dark ground band") {
    texture::TextureParams p = skyOnly();
    auto& s = spec(p);
    s.enableClouds = false;
    s.enableSun = false;
    s.sunAzimuthDeg = 0.0;  // sun behind the camera so the frame is away-from-sun sky
    s.pitchDeg = 20.0;
    const auto img = texture::renderSkyTexture(p, s, 48, 36);

    // Top of frame (high elevation): blue-dominant.
    const auto top = img.at(24, 1);
    CHECK(top.b > top.r);
    // The sky brightens from zenith toward the horizon (rows just above the horizon), then the
    // ground band below the horizon is darker than the sky at the horizon. With pitch 20 and
    // the default lens the horizon sits ~86% down the frame.
    const std::uint32_t horizonRow = 29;  // just above
    CHECK(rowLuma(img, horizonRow) > rowLuma(img, 2));
    CHECK(rowLuma(img, 35) < rowLuma(img, horizonRow));
}

TEST_CASE("sky render: cloud decks -- disabled decks are OFF, coverage drives ink, wind steers") {
    texture::TextureParams p = skyOnly();
    auto& s = spec(p);
    s.enableDome = false;
    s.enableSun = false;
    s.enableHaze = false;
    s.cloudCoverage = 0.55;

    // Disabling every deck is byte-identical to disabling the element.
    texture::SkyParams decksOff = s;
    for (auto& l : decksOff.cloudLayers) l.enabled = false;
    texture::SkyParams elementOff = s;
    elementOff.enableClouds = false;
    CHECK(texture::renderSkyTexture(p, decksOff, 48, 36).rgba ==
          texture::renderSkyTexture(p, elementOff, 48, 36).rgba);

    const auto coverageInk = [&](double cov) {
        texture::SkyParams v = s;
        v.cloudCoverage = cov;
        const auto img = texture::renderSkyTexture(p, v, 48, 36);
        double ink = 0.0;
        for (std::uint32_t y = 0; y < img.height; ++y)
            for (std::uint32_t x = 0; x < img.width; ++x) ink += img.at(x, y).a;
        return ink;
    };
    const double sparse = coverageInk(0.25);
    const double dense = coverageInk(0.85);
    CHECK(sparse > 0.0);          // some cloud
    CHECK(dense > sparse * 1.5);  // the master dial genuinely drives coverage

    // Wind direction shears the field: rotating it moves the pixels.
    texture::SkyParams windTurned = s;
    windTurned.windDirectionDeg = 115.0;
    CHECK(texture::renderSkyTexture(p, s, 48, 36).rgba !=
          texture::renderSkyTexture(p, windTurned, 48, 36).rgba);

    // Type matters: a cirrus-only sky and a cumulus-only sky are different pictures.
    texture::SkyParams cirrus = s;
    cirrus.cloudLayers = {{true, texture::CloudType::Cirrus, 1.0, 1.0, 0.0}};
    texture::SkyParams cumulus = s;
    cumulus.cloudLayers = {{true, texture::CloudType::Cumulus, 1.0, 1.0, 0.0}};
    CHECK(texture::renderSkyTexture(p, cirrus, 48, 36).rgba !=
          texture::renderSkyTexture(p, cumulus, 48, 36).rgba);
}

// ---- the volumetric cloud lane (S55-c) --------------------------------------------------------

TEST_CASE("volumetric lane: only the heap/tower types march") {
    CHECK(texture::cloudTypeIsVolumetric(texture::CloudType::Cumulus));
    CHECK(texture::cloudTypeIsVolumetric(texture::CloudType::Cumulonimbus));
    CHECK(!texture::cloudTypeIsVolumetric(texture::CloudType::Cirrus));
    CHECK(!texture::cloudTypeIsVolumetric(texture::CloudType::Stratus));
    CHECK(!texture::cloudTypeIsVolumetric(texture::CloudType::Altocumulus));
    CHECK(!texture::cloudTypeIsVolumetric(texture::CloudType::Stratocumulus));
}

TEST_CASE("volumetric march: misses a non-climbing ray or a clear field, marches a packed one") {
    const auto spec =
        texture::cloudVolumeSpec(texture::CloudType::Cumulus, 1300.0, 1900.0, 0.9, 1.0);
    const texture::CloudVolumeLight light{texture::directionFromAzEl(180.0, 45.0),
                                          {1.0, 1.0, 1.0}, {0.2, 0.2, 0.3}};

    // A downward or horizontal ray never enters the slab above the origin.
    CHECK(texture::marchCloudVolume(7, spec, light, {0.0, 0.2, -0.98}, 0.4, 0.5).coverage ==
          doctest::Approx(0.0));
    CHECK(texture::marchCloudVolume(7, spec, light, {0.0, 1.0, 0.0}, 0.4, 0.5).coverage ==
          doctest::Approx(0.0));

    // A clear field (coverage 0) writes nothing even straight up.
    const auto clearSpec =
        texture::cloudVolumeSpec(texture::CloudType::Cumulus, 1300.0, 1900.0, 0.0, 1.0);
    CHECK(texture::marchCloudVolume(7, clearSpec, light, {0.0, 0.0, 1.0}, 0.4, 0.5).coverage ==
          doctest::Approx(0.0));

    // A packed field (coverage 0.95) is near-opaque somewhere in a fan of near-vertical rays, and
    // every hit sits above the cloud base (the first-hit distance feeds aerial perspective).
    const auto packed =
        texture::cloudVolumeSpec(texture::CloudType::Cumulus, 1200.0, 1400.0, 0.95, 1.0);
    const texture::CloudVolumeLight top{texture::directionFromAzEl(180.0, 80.0),
                                        {1.1, 1.1, 1.05}, {0.18, 0.2, 0.28}};
    double maxCov = 0.0;
    bool sawHit = false;
    for (int i = 0; i < 48; ++i)
        for (int j = 0; j < 48; ++j) {
            const auto d = texture::skyNormalize({(i - 24) * 0.008, (j - 24) * 0.008, 1.0});
            const auto s = texture::marchCloudVolume(11, packed, top, d, 0.4, 0.5);
            maxCov = std::max(maxCov, s.coverage);
            if (s.coverage > 0.05) {
                sawHit = true;
                CHECK(s.firstHitM > 1000.0);  // the first dense sample is above the 1200 m base
            }
        }
    CHECK(sawHit);
    CHECK(maxCov > 0.8);
}

TEST_CASE("volumetric lane: the toggle re-shades the heap types but leaves the wisps untouched") {
    texture::TextureParams p = skyOnly();
    auto& s = spec(p);
    s.enableDome = false;
    s.enableSun = false;
    s.enableHaze = false;
    s.cloudCoverage = 0.6;

    // Cumulus: the volumetric field and the flat 2D projection are different pictures.
    texture::SkyParams cumVol = s;
    cumVol.cloudLayers = {{true, texture::CloudType::Cumulus, 1.0, 1.0, 0.0}};
    cumVol.volumetricClouds = true;
    texture::SkyParams cum2d = cumVol;
    cum2d.volumetricClouds = false;
    CHECK(texture::renderSkyTexture(p, cumVol, 48, 36).rgba !=
          texture::renderSkyTexture(p, cum2d, 48, 36).rgba);

    // Cirrus is a 2D-only type: flipping the volumetric master is inert for it.
    texture::SkyParams cirVol = s;
    cirVol.cloudLayers = {{true, texture::CloudType::Cirrus, 1.0, 1.0, 0.0}};
    cirVol.volumetricClouds = true;
    texture::SkyParams cir2d = cirVol;
    cir2d.volumetricClouds = false;
    CHECK(texture::renderSkyTexture(p, cirVol, 48, 36).rgba ==
          texture::renderSkyTexture(p, cir2d, 48, 36).rgba);
}

TEST_CASE("volumetric lane: deterministic; coverage drives ink; the sun steers the shading") {
    texture::TextureParams p = skyOnly();
    auto& s = spec(p);
    s.enableDome = false;
    s.enableSun = false;  // clouds are still lit -- the disc element toggle is not the physics
    s.enableHaze = false;
    s.cloudLayers = {{true, texture::CloudType::Cumulus, 1.0, 1.0, 0.0}};

    texture::SkyParams v = s;
    v.cloudCoverage = 0.6;
    CHECK(texture::renderSkyTexture(p, v, 48, 36).rgba ==
          texture::renderSkyTexture(p, v, 48, 36).rgba);  // pure function of the params

    const auto ink = [&](double cov) {
        texture::SkyParams q = s;
        q.cloudCoverage = cov;
        const auto img = texture::renderSkyTexture(p, q, 48, 36);
        double a = 0.0;
        for (std::uint32_t y = 0; y < img.height; ++y)
            for (std::uint32_t x = 0; x < img.width; ++x) a += img.at(x, y).a;
        return a;
    };
    CHECK(ink(0.7) > 0.0);
    CHECK(ink(0.7) > ink(0.3));  // the master dial genuinely drives the marched coverage

    // The same cloud field lit by a high vs a low sun is a different picture (self-shadow + the
    // sun-tinted lit face both move with the sun).
    texture::SkyParams sunHigh = v;
    sunHigh.sunElevationDeg = 70.0;
    texture::SkyParams sunLow = v;
    sunLow.sunElevationDeg = 6.0;
    CHECK(texture::renderSkyTexture(p, sunHigh, 48, 36).rgba !=
          texture::renderSkyTexture(p, sunLow, 48, 36).rgba);
}

// ---- the real star field (S55 night overhaul) ------------------------------------------------

TEST_CASE("star catalogue: the Yale BSC naked-eye subset, with a famous star where it belongs") {
    const std::size_t n = texture::starCatalogCount();
    CHECK(n > 8000);
    CHECK(n <= 9200);
    const texture::StarEntry* cat = texture::starCatalog();
    bool sawSirius = false;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& s = cat[i];
        CHECK(s.raDeg >= 0.0f);
        CHECK(s.raDeg < 360.0f);
        CHECK(s.decDeg >= -90.0f);
        CHECK(s.decDeg <= 90.0f);
        CHECK(s.mag <= 6.51f);        // naked-eye cutoff
        CHECK(s.kelvin >= 1500.0f);   // clamped colour-temperature range
        CHECK(s.kelvin <= 40000.0f);
        // Sirius: RA 06h45m ~= 101.29 deg, Dec ~= -16.72, the brightest star (V -1.46).
        if (std::abs(s.raDeg - 101.287f) < 0.02f && std::abs(s.decDeg + 16.716f) < 0.02f) {
            sawSirius = true;
            CHECK(s.mag == doctest::Approx(-1.46f));
        }
    }
    CHECK(sawSirius);
}

TEST_CASE("star field: real stars appear at night, ride the observer clock, and ignore the seed") {
    constexpr std::uint32_t kW = 200, kH = 140;
    const auto maxCh = [](const common::ImageF& img) {
        float m = 0.0f;
        for (std::size_t i = 0; i < img.rgba.size(); i += 4)
            m = std::max({m, img.rgba[i], img.rgba[i + 1], img.rgba[i + 2]});
        return m;
    };
    texture::TextureParams p = skyOnly(42);
    auto& s = spec(p);
    s.enableClouds = false;
    s.enableSun = false;
    s.enableMoon = false;
    s.sunElevationDeg = -35.0;
    s.starsAmount = 1.0;
    s.fovDeg = 90.0;
    s.pitchDeg = 45.0;  // look up into the winter sky (default clock 2000-01-01 04:00 UTC, 40N)

    const auto starry = texture::renderSkyTexture(p, s, kW, kH);
    s.starsAmount = 0.0;
    const auto bare = texture::renderSkyTexture(p, s, kW, kH);
    s.starsAmount = 1.0;
    CHECK(maxCh(starry) > maxCh(bare) + 0.2f);  // bright catalogue stars punch through

    // The star field is the CATALOGUE, not noise: the document seed cannot move it.
    texture::TextureParams p2 = skyOnly(999);
    p2.spec = s;
    const auto starry2 = texture::renderSkyTexture(p2, spec(p2), kW, kH);
    CHECK(starry.rgba == starry2.rgba);

    // The observer clock orients it: a different hour rotates the sky, changing the pixels.
    s.obsHourUtc = 10.0;  // six hours later -> ~90 deg of sky rotation
    const auto rotated = texture::renderSkyTexture(p, s, kW, kH);
    CHECK(rotated.rgba != starry.rgba);
    s.obsHourUtc = 4.0;

    // A different latitude also changes which stars are up.
    s.obsLatitudeDeg = -35.0;
    const auto southern = texture::renderSkyTexture(p, s, kW, kH);
    CHECK(southern.rgba != starry.rgba);
}

TEST_CASE("moon: the real LRO disc -- seed-free albedo, a phase terminator, clock-driven face") {
    // A big centred disc in a deep-night sky (fov floors at 10 deg, so moonScale magnifies).
    constexpr std::uint32_t kW = 160, kH = 160;
    texture::TextureParams p = skyOnly(42);
    auto& s = spec(p);
    s.enableClouds = false;
    s.enableSun = false;
    s.enableMoon = true;
    s.starsAmount = 0.0;
    s.fovDeg = 10.0;
    s.moonScale = 12.0;
    s.pitchDeg = 45.0;
    s.moonAzimuthDeg = 180.0;  // camera faces 180 -> the disc is centred
    s.moonElevationDeg = 45.0;
    s.sunAzimuthDeg = 0.0;
    s.sunElevationDeg = -45.0;  // sun opposite & well down -> a full, brightly lit moon
    s.obsYear = 2026;
    s.obsMonth = 11;
    s.obsDay = 13;
    s.obsHourUtc = 0.0;
    s.obsLatitudeDeg = 40.0;
    s.obsLongitudeDeg = -74.0;

    const auto moon = texture::renderSkyTexture(p, s, kW, kH);

    // The disc is bright AND carries real light/dark structure (maria vs highlands), not a flat
    // ball: measure the luma range over the lit disc only.
    float lo = 1e9f, hi = -1e9f;
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < kW; ++x) {
            const double L = pixelLuma(moon, x, y);
            if (L > 0.25) {  // on the lit disc, not the near-black night dome
                lo = std::min(lo, static_cast<float>(L));
                hi = std::max(hi, static_cast<float>(L));
            }
        }
    CHECK(hi > 0.4f);          // the disc reads bright against the night
    CHECK(hi - lo > 0.08f);    // and the maria give it real contrast

    // The albedo is the LRO CATALOGUE texture, not seeded noise (the old fBm mare used the doc
    // seed): a different document seed cannot move a single pixel.
    texture::TextureParams p2 = skyOnly(999);
    p2.spec = s;
    const auto moon2 = texture::renderSkyTexture(p2, spec(p2), kW, kH);
    CHECK(moon.rgba == moon2.rgba);

    // The near side is turned by the observer clock (ch.53 libration + axis angle): a different
    // date rotates the disc (P swings ~20 deg Nov->Apr), so the pixels change.
    s.obsMonth = 4;
    const auto rotated = texture::renderSkyTexture(p, s, kW, kH);
    CHECK(rotated.rgba != moon.rgba);
    s.obsMonth = 11;

    // Phase: lighting the moon from opposite sides swaps which half is bright (a real terminator,
    // lit by the scene sun). Camera faces south (screen-right = west), so a west sun brightens the
    // right half and an east sun the left half.
    const auto halfBias = [&](const common::ImageF& im) {
        double l = 0.0, r = 0.0;
        int nl = 0, nr = 0;
        for (std::uint32_t y = 0; y < kH; ++y)
            for (std::uint32_t x = 0; x < kW; ++x) {
                const double L = pixelLuma(im, x, y);
                if (x < kW / 2) { l += L; ++nl; } else { r += L; ++nr; }
            }
        return l / nl - r / nr;  // >0 means the left (east-lit) half is brighter
    };
    s.sunAzimuthDeg = 270.0;
    s.sunElevationDeg = -8.0;  // sun to the west, just below the horizon
    const auto litWest = texture::renderSkyTexture(p, s, kW, kH);
    s.sunAzimuthDeg = 90.0;  // sun to the east
    const auto litEast = texture::renderSkyTexture(p, s, kW, kH);
    CHECK(halfBias(litWest) < 0.0);           // west sun -> right (west) half bright
    CHECK(halfBias(litEast) > 0.0);           // east sun -> left (east) half bright
}

TEST_CASE("moon: phase control -- not full every night (manual + ephemeris)") {
    constexpr std::uint32_t kW = 140, kH = 140;
    const auto setup = [&]() {
        texture::TextureParams p = skyOnly(42);
        auto& s = spec(p);
        s.enableClouds = false;
        s.enableSun = false;
        s.enableMoon = true;
        s.starsAmount = 0.0;
        s.fovDeg = 10.0;
        s.moonScale = 12.0;
        s.pitchDeg = 45.0;
        s.moonAzimuthDeg = 180.0;
        s.moonElevationDeg = 45.0;
        s.sunAzimuthDeg = 0.0;
        s.sunElevationDeg = -45.0;  // sun opposite -> the LEGACY scene-sun mode would render full
        s.obsYear = 2026;
        s.obsMonth = 3;
        s.obsDay = 5;
        s.obsHourUtc = 3.0;
        s.obsLatitudeDeg = 40.0;
        s.obsLongitudeDeg = -74.0;
        return p;
    };
    // Total frame luma is a lit-area proxy (the night background is ~0): more of the disc lit ->
    // more luma. A thin crescent must light far less than a full moon at the SAME geometry.
    const auto litArea = [&](const common::ImageF& im) {
        double sum = 0.0;
        for (std::uint32_t y = 0; y < im.height; ++y)
            for (std::uint32_t x = 0; x < im.width; ++x) sum += pixelLuma(im, x, y);
        return sum;
    };

    texture::TextureParams p = setup();
    auto& s = spec(p);
    s.moonPhaseMode = 1;  // manual illuminated fraction
    s.moonIlluminatedFraction = 1.0;
    const double full = litArea(texture::renderSkyTexture(p, s, kW, kH));
    s.moonIlluminatedFraction = 0.5;
    const double half = litArea(texture::renderSkyTexture(p, s, kW, kH));
    s.moonIlluminatedFraction = 0.1;
    const double crescent = litArea(texture::renderSkyTexture(p, s, kW, kH));
    CHECK(full > half);
    CHECK(half > crescent);
    CHECK(full > crescent * 1.8);  // decisively NOT "full moon every day"

    // Ephemeris mode: the phase follows the observer clock's DATE, so two dates ~2 weeks apart
    // (opposite ends of the cycle) render materially different amounts of lit disc.
    s.moonPhaseMode = 2;
    s.obsDay = 5;
    const double date1 = litArea(texture::renderSkyTexture(p, s, kW, kH));
    s.obsDay = 20;
    const double date2 = litArea(texture::renderSkyTexture(p, s, kW, kH));
    CHECK(std::abs(date1 - date2) > 0.15 * std::max(date1, date2));
}

TEST_CASE("moon: LOLA relief -- real basin depths in the table, crags at the half-phase terminator") {
    // Data facts first. The elevation map is the real LOLA LDEM_4 grid resampled onto the albedo
    // map's own (u, v) convention: Mare Crisium (+59E, +17N) must read BOTH dark and ~3.5 km deep
    // at the same texel, and over the whole map the bright highlands sit high (a solidly positive
    // albedo-height correlation) -- the checks that pinned the two maps' shared orientation.
    const std::int16_t* elev = texture::moonElevation();
    const unsigned char* alb = texture::moonAlbedo();
    constexpr int W = texture::kMoonElevationWidth, H = texture::kMoonElevationHeight;
    static_assert(W == texture::kMoonTextureWidth && H == texture::kMoonTextureHeight);
    const std::size_t n = static_cast<std::size_t>(W) * H;
    double sumA = 0.0, sumE = 0.0;
    std::int16_t lo = 32767, hi = -32768;
    for (std::size_t i = 0; i < n; ++i) {
        sumA += alb[i];
        sumE += elev[i];
        lo = std::min(lo, elev[i]);
        hi = std::max(hi, elev[i]);
    }
    CHECK(lo < -8000);   // South Pole-Aitken floor
    CHECK(lo > -11000);  // ...and nothing beyond the real lunar range
    CHECK(hi > 9000);    // far-side highlands
    CHECK(hi < 11000);
    const double meanA = sumA / n, meanE = sumE / n;
    double cov = 0.0, varA = 0.0, varE = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double da = alb[i] - meanA, de = elev[i] - meanE;
        cov += da * de;
        varA += da * da;
        varE += de * de;
    }
    CHECK(cov / std::sqrt(varA * varE) > 0.25);  // dark maria low, bright highlands high
    const int cu = static_cast<int>((59.0 + 180.0) / 360.0 * W);   // Mare Crisium centre texel
    const int cv = static_cast<int>((90.0 - 17.0) / 180.0 * H);
    double crisiumE = 0.0, crisiumA = 0.0;
    for (int dy = -8; dy < 8; ++dy)
        for (int dx = -8; dx < 8; ++dx) {
            const std::size_t i = static_cast<std::size_t>(cv + dy) * W + (cu + dx);
            crisiumE += elev[i];
            crisiumA += alb[i];
        }
    CHECK(crisiumE / 256.0 < -2500.0);  // the real basin floor is ~3.5 km down
    CHECK(crisiumA / 256.0 < meanA);    // and it is a dark mare

    // Render property: relief shading grows with grazing incidence, so a half moon's lit disc
    // reads far craggier than the SAME geometry at full phase (where 2cosI/(cosI+cosE) -> 1 for
    // sphere and terrain normals alike -- what real photographs do). Normalized local contrast =
    // mean |adjacent-luma difference| over lit pixels / mean lit luma. The flat-normal renderer
    // measured half/full = 1.89 at this framing; with relief it is 2.33 -- pin the gap at 2.1.
    constexpr std::uint32_t kW = 320, kH = 320;
    texture::TextureParams p = skyOnly(42);
    auto& s = spec(p);
    s.enableClouds = false;
    s.enableSun = false;
    s.enableMoon = true;
    s.starsAmount = 0.0;
    s.fovDeg = 10.0;
    s.moonScale = 12.0;
    s.pitchDeg = 45.0;
    s.moonAzimuthDeg = 180.0;
    s.moonElevationDeg = 45.0;
    s.sunAzimuthDeg = 270.0;
    s.sunElevationDeg = -45.0;
    s.moonPhaseMode = 1;  // manual illuminated fraction
    s.obsYear = 2026;
    s.obsMonth = 11;
    s.obsDay = 13;
    s.obsHourUtc = 0.0;
    s.obsLatitudeDeg = 40.0;
    s.obsLongitudeDeg = -74.0;
    const auto litContrast = [](const common::ImageF& im) {
        double maxL = 0.0;
        for (std::uint32_t y = 0; y < im.height; ++y)
            for (std::uint32_t x = 0; x < im.width; ++x)
                maxL = std::max(maxL, pixelLuma(im, x, y));
        const double thresh = 0.25 * maxL;
        double dSum = 0.0, lSum = 0.0;
        std::size_t dN = 0, lN = 0;
        for (std::uint32_t y = 0; y < im.height; ++y)
            for (std::uint32_t x = 0; x < im.width; ++x) {
                const double L = pixelLuma(im, x, y);
                if (L <= thresh) continue;
                lSum += L;
                ++lN;
                if (x + 1 < im.width) {
                    const double R = pixelLuma(im, x + 1, y);
                    if (R > thresh) {
                        dSum += std::abs(R - L);
                        ++dN;
                    }
                }
            }
        REQUIRE(lN > 0);
        REQUIRE(dN > 0);
        return (dSum / dN) / (lSum / lN);
    };
    s.moonIlluminatedFraction = 0.5;
    const auto half = texture::renderSkyTexture(p, s, kW, kH);
    const auto half2 = texture::renderSkyTexture(p, s, kW, kH);
    CHECK(half.rgba == half2.rgba);  // the relief taps stay a pure function of the params
    s.moonIlluminatedFraction = 1.0;
    const auto full = texture::renderSkyTexture(p, s, kW, kH);
    CHECK(litContrast(half) > 2.1 * litContrast(full));
}

TEST_CASE("sky render: output is finite with bounded HDR headroom (incl. sub-horizon twilight)") {
    // Hostile-ish params across the WHOLE sun range: max turbidity, heavy overcast, wide lens, and
    // every twilight/night elevation the phase-4 integrator has to survive (day, the handoff band,
    // civil/nautical/astronomical twilight, deep night).
    for (double el : {35.0, 0.0, -1.0, -4.0, -6.0, -9.0, -12.0, -18.0, -30.0}) {
        texture::TextureParams p = skyOnly(99);
        auto& s = spec(p);
        s.turbidity = 10.0;
        s.sunElevationDeg = el;
        s.cloudCoverage = 1.0;
        s.fovDeg = 120.0;
        s.pitchDeg = 0.0;
        s.enableMoon = true;  // exercise the moon/relight paths too
        s.moonScale = 3.0;
        s.starsAmount = 1.0;
        const auto img = texture::renderSkyTexture(p, s, 48, 36);
        CAPTURE(el);
        for (std::uint32_t y = 0; y < img.height; ++y)
            for (std::uint32_t x = 0; x < img.width; ++x) {
                const auto c = img.at(x, y);
                CHECK(std::isfinite(c.r));
                CHECK(std::isfinite(c.g));
                CHECK(std::isfinite(c.b));
                CHECK(c.r >= 0.0f);
                CHECK(c.g >= 0.0f);
                CHECK(c.b >= 0.0f);
                CHECK(c.r < 8.0f);  // §4.4: HDR headroom exists but stays sane
                CHECK(c.g < 8.0f);
                CHECK(c.b < 8.0f);
                CHECK(c.a >= 0.0f);
                CHECK(c.a <= 1.0f);
            }
    }
}

// ---- lens flare (screen-space by construction: no lens model, no ghost LUT) --------------------

TEST_CASE("lens flare: the OFF path is byte-inert (the toggle gates every flare byte)") {
    // Default day render vs the same params with the flare machinery poked but GATED: the
    // strength knob with the toggle off, and the toggle on with zero strength, must both leave
    // the exact pre-flare bytes (the golden pin in test_texture_layer holds the default itself).
    texture::TextureParams p = skyOnly(42);
    auto& s = spec(p);
    const auto base = texture::renderSkyTexture(p, s, 64, 48);

    texture::SkyParams knobOnly = s;
    knobOnly.enableLensFlare = false;  // the default, restated
    knobOnly.flareStrength = 1.0;
    const auto knob = texture::renderSkyTexture(p, knobOnly, 64, 48);
    CHECK(base.rgba == knob.rgba);

    texture::SkyParams zeroStrength = s;
    zeroStrength.enableLensFlare = true;
    zeroStrength.flareStrength = 0.0;
    const auto zero = texture::renderSkyTexture(p, zeroStrength, 64, 48);
    CHECK(base.rgba == zero.rgba);
}

TEST_CASE("lens flare: deterministic; energy lands on the ghost train along the sun-centre axis") {
    // A clear sky with the sun up-left of centre: the ghost train must brighten the pixels at
    // C + t*(S - C) (S from the same projection the renderer uses), and nothing may darken --
    // the flare is strictly additive light over an opaque dome.
    constexpr std::uint32_t kW = 128, kH = 96;
    texture::TextureParams p = skyOnly(11);
    auto& s = spec(p);
    s.enableClouds = false;
    s.sunAzimuthDeg = 168.0;  // left of the frame centre
    s.sunElevationDeg = 38.0;
    const auto off = texture::renderSkyTexture(p, s, kW, kH);
    s.enableLensFlare = true;
    s.flareStrength = 1.0;
    const auto on = texture::renderSkyTexture(p, s, kW, kH);
    const auto on2 = texture::renderSkyTexture(p, s, kW, kH);
    CHECK(on.rgba == on2.rgba);  // pure function of the params (row-parallel included)
    CHECK(on.rgba != off.rgba);

    // Nothing darkens: additive-only light (identical dither keys on both renders).
    for (std::size_t i = 0; i < on.rgba.size(); ++i) CHECK(on.rgba[i] >= off.rgba[i] - 1e-6f);

    // The sun's screen position, exactly as the renderer projects it.
    const texture::SkyCamera cam = texture::SkyCamera::fromParams(s, kW, kH);
    double sx = 0.0, sy = 0.0;
    REQUIRE(cam.project(texture::directionFromAzEl(s.sunAzimuthDeg, s.sunElevationDeg), sx, sy));
    const double cx = kW / 2.0, cy = kH / 2.0;
    // Sample the centre pixel of a few ghosts of the frozen train (t values from the renderer's
    // recipe); each must be measurably brighter with the flare on.
    for (const double t : {0.62, 0.21, -0.32, -1.05}) {
        const double gx = cx + t * (sx - cx), gy = cy + t * (sy - cy);
        const auto ix = static_cast<std::uint32_t>(gx);
        const auto iy = static_cast<std::uint32_t>(gy);
        REQUIRE(ix < kW);
        REQUIRE(iy < kH);
        CAPTURE(t);
        CHECK(pixelLuma(on, ix, iy) > pixelLuma(off, ix, iy) + 0.005);
    }

    // The strength knob is monotonic in total energy.
    const auto meanLuma = [](const common::ImageF& img) {
        double sum = 0.0;
        for (std::uint32_t y = 0; y < img.height; ++y)
            for (std::uint32_t x = 0; x < img.width; ++x) sum += pixelLuma(img, x, y);
        return sum / (static_cast<double>(img.width) * img.height);
    };
    s.flareStrength = 0.4;
    const auto half = texture::renderSkyTexture(p, s, kW, kH);
    CHECK(meanLuma(off) < meanLuma(half));
    CHECK(meanLuma(half) < meanLuma(on));
}

TEST_CASE("lens flare: sun-bound visibility -- night kills it, behind-camera kills it, "
          "half-out-of-frame fades it") {
    texture::TextureParams p = skyOnly(5);
    auto& s = spec(p);
    s.enableClouds = false;

    SUBCASE("a sub-horizon sun casts no flare (the moon never does)") {
        s.sunElevationDeg = -20.0;
        s.enableMoon = true;
        s.moonElevationDeg = 40.0;
        s.starsAmount = 0.8;
        const auto off = texture::renderSkyTexture(p, s, 64, 48);
        s.enableLensFlare = true;
        s.flareStrength = 1.0;
        const auto on = texture::renderSkyTexture(p, s, 64, 48);
        CHECK(off.rgba == on.rgba);  // byte-identical: the flare ties to the SUN alone
    }
    SUBCASE("a sun behind the image plane casts no flare") {
        s.sunAzimuthDeg = 20.0;  // the camera faces 180 (south); 20 is behind it
        s.sunElevationDeg = 10.0;
        const auto off = texture::renderSkyTexture(p, s, 64, 48);
        s.enableLensFlare = true;
        s.flareStrength = 1.0;
        const auto on = texture::renderSkyTexture(p, s, 64, 48);
        CHECK(off.rgba == on.rgba);
    }
    SUBCASE("a sun just off the frame edge still flares (the Sega-style fade is smooth)") {
        s.sunAzimuthDeg = 216.0;  // ~5 degrees past the default 62-degree lens's right edge
        s.sunElevationDeg = 20.0;
        const auto off = texture::renderSkyTexture(p, s, 96, 64);
        s.enableLensFlare = true;
        s.flareStrength = 1.0;
        const auto on = texture::renderSkyTexture(p, s, 96, 64);
        CHECK(on.rgba != off.rgba);  // faded, not dead
        double gain = 0.0;
        for (std::size_t i = 0; i < on.rgba.size(); ++i)
            gain += static_cast<double>(on.rgba[i]) - off.rgba[i];
        CHECK(gain > 0.0);
    }
}

TEST_CASE("lens flare: full strength stays finite inside the HDR headroom") {
    for (const double turb : {1.0, 10.0}) {
        texture::TextureParams p = skyOnly(3);
        auto& s = spec(p);
        s.turbidity = turb;
        s.sunElevationDeg = 40.0;
        s.enableLensFlare = true;
        s.flareStrength = 1.0;
        const auto img = texture::renderSkyTexture(p, s, 48, 36);
        CAPTURE(turb);
        for (std::uint32_t y = 0; y < img.height; ++y)
            for (std::uint32_t x = 0; x < img.width; ++x) {
                const auto c = img.at(x, y);
                CHECK(std::isfinite(c.r));
                CHECK(std::isfinite(c.g));
                CHECK(std::isfinite(c.b));
                CHECK(c.r >= 0.0f);
                CHECK(c.r < 8.0f);
                CHECK(c.g < 8.0f);
                CHECK(c.b < 8.0f);
                CHECK(c.a >= 0.0f);
                CHECK(c.a <= 1.0f);
            }
    }
}

// ---- the physical single-scattering atmosphere (S55 night overhaul, phase 4) ------------------

namespace {
// Total radiance over a fixed fan of view directions -- a proxy for "how much light the sky throws".
double skyBrightness(const texture::Atmosphere& a) {
    double sum = 0.0;
    for (double el : {5.0, 20.0, 45.0, 80.0})
        for (double az : {0.0, 90.0, 180.0, 270.0}) {
            const texture::Rgb c = a.radiance(texture::directionFromAzEl(az, el));
            sum += c.r + c.g + c.b;
        }
    return sum;
}
}  // namespace

TEST_CASE("atmosphere: single-scattering integrator -- finite, blue zenith, reddened low sun") {
    // A daytime sun: the integrator itself is a valid atmosphere at any elevation (the renderer
    // only GATES its use to sub-horizon suns; the physics is testable directly).
    const auto atmo = texture::cookAtmosphere(texture::directionFromAzEl(180.0, 20.0), 2.5);

    // Finite and non-negative everywhere on the dome.
    for (double el : {0.0, 10.0, 45.0, 89.0})
        for (double az : {0.0, 45.0, 135.0, 180.0, 225.0, 315.0}) {
            const texture::Rgb c = atmo.radiance(texture::directionFromAzEl(az, el));
            CHECK(std::isfinite(c.r));
            CHECK(std::isfinite(c.g));
            CHECK(std::isfinite(c.b));
            CHECK(c.r >= 0.0);
            CHECK(c.g >= 0.0);
            CHECK(c.b >= 0.0);
        }

    // Rayleigh scattering makes the zenith BLUE (b > r); the horizon toward the sun is REDDENED by
    // the long air path (its r/b ratio beats the zenith's).
    const texture::Rgb zenith = atmo.radiance({0.0, 0.0, 1.0});
    CHECK(zenith.b > zenith.r);
    const texture::Rgb sunHoriz = atmo.radiance(texture::directionFromAzEl(180.0, 1.0));
    CHECK(sunHoriz.r / (sunHoriz.b + 1e-9) > zenith.r / (zenith.b + 1e-9));
}

TEST_CASE("atmosphere: the sky darkens monotonically as the sun sinks (Earth's shadow rises)") {
    // As the sun drops below the horizon the whole visible atmosphere moves into Earth's shadow, so
    // less and less sunlight is available to scatter in -- the physical mechanism that carries the
    // day->night transition without any hand-authored gradient.
    double prev = 1e18, b6 = 0.0, bDusk = 0.0, bNight = 0.0;
    for (double el : {6.0, 0.0, -3.0, -6.0, -9.0, -12.0, -18.0}) {
        const auto atmo = texture::cookAtmosphere(texture::directionFromAzEl(180.0, el), 2.5);
        const double b = skyBrightness(atmo);
        CAPTURE(el);
        CHECK(b <= prev + 1e-12);  // never brightens as the sun sinks (it bottoms out at ~0)
        CHECK(b >= 0.0);
        prev = b;
        if (el == 6.0) b6 = b;
        if (el == -3.0) bDusk = b;
        if (el == -18.0) bNight = b;
    }
    // The meaningful twilight range strictly dims; deep-astronomical night is essentially black
    // (single scattering has nothing left to scatter -- Earth's shadow covers the whole atmosphere).
    CHECK(bDusk > 0.0);
    CHECK(b6 > bDusk);
    CHECK(bNight < 0.05 * bDusk);
}

TEST_CASE("atmosphere: multiple scattering keeps the anti-solar twilight alive and blue") {
    // Single scattering alone left the anti-solar civil-twilight sky essentially BLACK (every
    // sample there sits in Earth's shadow, so the direct term is zero); the baked isotropic
    // multiple-scattering table (Psi_ms) is what lights it -- softly, and blue-shifted, because
    // the light arriving there has scattered off the still-sunlit atmosphere. Measured: the
    // single-scatter-only integrator gave (0.0007, 0.00004, 0.0) here; with Psi_ms the blue
    // channel alone sits orders of magnitude above that floor.
    const auto atmo = texture::cookAtmosphere(texture::directionFromAzEl(180.0, -3.0), 2.5);
    CHECK(!atmo.msTable.empty());
    for (float v : atmo.msTable) {
        CHECK(std::isfinite(v));
        CHECK(v >= 0.0f);
    }
    const texture::Rgb away = atmo.radiance(texture::directionFromAzEl(0.0, 5.0));
    CHECK(away.b > 0.005);          // not the dead-black single-scatter sky
    CHECK(away.b > 0.8 * away.r);   // and blue-balanced, not a warm leak
}

TEST_CASE("atmosphere: a below-horizon sun leaves a warm glow toward it, dark away from it") {
    // Civil twilight: the sunset point still glows warm (the light reaching the low atmosphere there
    // is heavily reddened), while the anti-solar horizon is darker and cooler.
    const auto atmo = texture::cookAtmosphere(texture::directionFromAzEl(180.0, -3.0), 3.0);
    const texture::Rgb toward = atmo.radiance(texture::directionFromAzEl(180.0, 3.0));
    const texture::Rgb away = atmo.radiance(texture::directionFromAzEl(0.0, 3.0));
    CHECK(toward.r + toward.g + toward.b > away.r + away.g + away.b);   // brighter toward the sun
    CHECK(toward.r / (toward.b + 1e-9) > away.r / (away.b + 1e-9));     // and warmer

    // An empty (never-cooked) atmosphere is a safe zero (the renderer relies on this for the day
    // gate -- it default-constructs the table and never evaluates it above the horizon).
    const texture::Atmosphere empty;
    const texture::Rgb z = empty.radiance({0.0, 0.0, 1.0});
    CHECK(z.r == 0.0);
    CHECK(z.g == 0.0);
    CHECK(z.b == 0.0);
}

TEST_CASE("physical twilight: gated to the sub-horizon sun; the day path is inert to night inputs") {
    // Above the horizon the renderer must NEVER route through the integrator or the night machinery:
    // the observer clock, star amount and moon knobs (all night-only) cannot move a single daytime
    // byte. (The golden pin in test_texture_layer holds the exact day bytes; this guards the gate.)
    // (The moon is a deliberately day-visible element -- like a real daytime moon -- so it is NOT a
    // "night input"; only the integrator, star field and twilight relight are gated by elevation.)
    texture::TextureParams p = skyOnly(42);
    auto& s = spec(p);
    s.sunElevationDeg = 5.0;  // daytime, just above the 0..-6 handoff
    s.enableMoon = false;
    const auto day = texture::renderSkyTexture(p, s, 64, 48);
    s.obsYear = 1987;
    s.obsHourUtc = 21.0;
    s.obsLatitudeDeg = -33.0;
    s.starsAmount = 1.0;
    const auto day2 = texture::renderSkyTexture(p, s, 64, 48);
    CHECK(day.rgba == day2.rgba);  // byte-identical: the day dome is pure Hosek-Wilkie

    // Below the horizon the integrator IS engaged: turbidity (an atmosphere input) now genuinely
    // repaints the twilight sky, and the picture differs from the gated daytime one.
    texture::SkyParams tw = s;
    tw.sunElevationDeg = -4.0;
    tw.enableMoon = false;
    tw.enableClouds = false;
    tw.starsAmount = 0.0;
    tw.turbidity = 2.0;
    tw.turbidity = 2.0;
    const auto clear = texture::renderSkyTexture(p, tw, 64, 48);
    tw.turbidity = 8.0;
    const auto hazy = texture::renderSkyTexture(p, tw, 64, 48);
    CHECK(clear.rgba != hazy.rgba);  // the scattering integrator responds to turbidity
    CHECK(clear.rgba != day.rgba);   // and the twilight sky is not the gated daytime one
}

TEST_CASE("physical twilight: deterministic, and the sky reddens-then-darkens as the sun sets") {
    texture::TextureParams p = skyOnly(7);
    auto& s = spec(p);
    s.enableClouds = false;
    s.enableSun = false;
    s.enableMoon = false;
    s.starsAmount = 0.0;
    s.pitchDeg = 10.0;
    s.fovDeg = 80.0;

    const auto meanLuma = [](const common::ImageF& img) {
        double sum = 0.0;
        for (std::uint32_t y = 0; y < img.height; ++y)
            for (std::uint32_t x = 0; x < img.width; ++x) sum += pixelLuma(img, x, y);
        return sum / (static_cast<double>(img.width) * img.height);
    };

    // Deterministic (pure function of the params -- the integrator carries no per-thread state).
    s.sunElevationDeg = -4.0;
    const auto a = texture::renderSkyTexture(p, s, 96, 64);
    const auto b = texture::renderSkyTexture(p, s, 96, 64);
    CHECK(a.rgba == b.rgba);

    // Monotonic darkening from the handoff down into the night, all emergent from the integrator.
    double prev = 1e9;
    for (double el : {-1.0, -3.0, -5.0, -8.0, -12.0, -20.0}) {
        s.sunElevationDeg = el;
        const double m = meanLuma(texture::renderSkyTexture(p, s, 96, 64));
        CAPTURE(el);
        CHECK(m < prev);
        prev = m;
    }
}
