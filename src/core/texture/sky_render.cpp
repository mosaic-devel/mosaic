// Sky texture generator (S55-b) -- procedural, CPU-first, ML-free. Technique lineage
// (docs/texture-generator.md §4/§9.2):
//   - Sky-dome radiance: Hosek & Wilkie, "An Analytic Model for Full Spectral Sky-Dome
//     Radiance" (SIGGRAPH 2012), via the authors' BSD-3 reference implementation vendored at
//     third_party/hosekwilkie/ (RGB lane).
//   - Solar position: clean-room NOAA/Meeus (solar.cpp; NEVER NREL spa.c -- §9.2).
//   - Sun tinting: Beer-Lambert (1729/1852) extinction over Kasten-Young (1989) relative air
//     mass with Rayleigh (1871) / Mie (1908) optical depths -- plain physics.
//   - Clouds: 2D layered Perlin-Worley fBm + domain warping (Perlin 1985 / Worley 1996 /
//     Perlin & Hoffert 1989) on §4.5 perspective-projected altitude planes.
//     ⚠ NO wavelet noise (noise.hpp's standing constraint). The volumetric lane is S55-c.
//   - Camera: pinhole perspective projection (ancient and plain).
// No ML. No engine source copied. Element composition per §3.4: enabled elements composite
// bottom-to-top in LINEAR radiometric space into a straight-alpha float buffer; disabled base
// elements leave genuine transparency. Display mapping per §4.4: exposure -> extended-Reinhard
// tonemap -> sRGB encode, kept in FLOAT (banding-free); values may exceed 1 at the solar disc
// (HDR headroom -- toImage8 clips them to white).
//
// ⚠ ONE DELIBERATE DEVIATION from the §4.4 order sketch: the sun disc/aureole composites
// BEFORE the cloud decks (the sketch drew it last). The catalogue itself demands it --
// "Altostratus: sun a diffuse disc" only happens with clouds OVER the sun -- and thin cirrus
// dimming the disc falls out of plain alpha compositing. Forward-scatter "silver lining" on the
// clouds carries the in-front glow instead.

#include "core/texture/sky_render.hpp"

#include <ArHosekSkyModel.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

#include "common/dither.hpp"
#include "core/texture/atmosphere.hpp"
#include "core/texture/cloud_volume.hpp"
#include "core/texture/lunar.hpp"
#include "core/texture/moon_elevation.hpp"
#include "core/texture/moon_texture.hpp"
#include "core/texture/noise.hpp"
#include "core/texture/parallel_rows.hpp"
#include "core/texture/render_support.hpp"
#include "core/texture/sky_camera.hpp"
#include "core/texture/star_catalog.hpp"

namespace mosaic::core::texture {

namespace {

using common::ColorF;

constexpr double kPi = std::numbers::pi;
constexpr double kDegToRad = kPi / 180.0;

double clamp01(double v) noexcept {
    return std::clamp(v, 0.0, 1.0);
}

double smoothstep(double e0, double e1, double v) noexcept {
    const double t = clamp01((v - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

constexpr double kRadToDeg = 180.0 / kPi;

SkyVec3 skyCross(SkyVec3 a, SkyVec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Bilinear sample of the LRO albedo map (moon_texture.hpp) at equirectangular (u, v): u wraps in
// selenographic longitude, v clamps at the poles. Returns a display-space albedo in [0, 1].
double sampleMoonAlbedo(double u, double v) noexcept {
    const unsigned char* tex = moonAlbedo();
    constexpr int W = kMoonTextureWidth, H = kMoonTextureHeight;
    u -= std::floor(u);  // wrap longitude into [0, 1)
    const double fu = u * W - 0.5;
    const double fv = clamp01(v) * H - 0.5;
    int x0 = static_cast<int>(std::floor(fu));
    int y0 = static_cast<int>(std::floor(fv));
    const double tx = fu - x0, ty = fv - y0;
    const auto wrapX = [](int x) { x %= W; return x < 0 ? x + W : x; };
    const int x0w = wrapX(x0), x1w = wrapX(x0 + 1);
    const int y0c = std::clamp(y0, 0, H - 1), y1c = std::clamp(y0 + 1, 0, H - 1);
    const auto at = [&](int x, int y) {
        return tex[static_cast<std::size_t>(y) * W + x] / 255.0;
    };
    const double top = at(x0w, y0c) * (1.0 - tx) + at(x1w, y0c) * tx;
    const double bot = at(x0w, y1c) * (1.0 - tx) + at(x1w, y1c) * tx;
    return top * (1.0 - ty) + bot * ty;
}

// Bilinear sample of the LOLA elevation map (moon_elevation.hpp) at the SAME equirectangular
// (u, v) as the albedo -- the two maps share one grid. Returns metres above the reference sphere.
double sampleMoonElevation(double u, double v) noexcept {
    const std::int16_t* tex = moonElevation();
    constexpr int W = kMoonElevationWidth, H = kMoonElevationHeight;
    u -= std::floor(u);  // wrap longitude into [0, 1)
    const double fu = u * W - 0.5;
    const double fv = clamp01(v) * H - 0.5;
    int x0 = static_cast<int>(std::floor(fu));
    int y0 = static_cast<int>(std::floor(fv));
    const double tx = fu - x0, ty = fv - y0;
    const auto wrapX = [](int x) { x %= W; return x < 0 ? x + W : x; };
    const int x0w = wrapX(x0), x1w = wrapX(x0 + 1);
    const int y0c = std::clamp(y0, 0, H - 1), y1c = std::clamp(y0 + 1, 0, H - 1);
    const auto at = [&](int x, int y) {
        return static_cast<double>(tex[static_cast<std::size_t>(y) * W + x]);
    };
    const double top = at(x0w, y0c) * (1.0 - tx) + at(x1w, y0c) * tx;
    const double bot = at(x0w, y1c) * (1.0 - tx) + at(x1w, y1c) * tx;
    return top * (1.0 - ty) + bot * ty;
}

// The Moon's mean radius (metres) -- converts elevation-map gradients to metres-per-metre slopes.
constexpr double kMoonRadiusM = 1737.4e3;

// Rgb + mixRgb are the shared §4.4 radiance primitives (sky_camera.hpp), used here and by the
// volumetric marcher (cloud_volume.cpp).

// Straight-alpha source-over in the linear working space (§3.4).
ColorF over(ColorF src, ColorF dst) noexcept {
    const float a = src.a + dst.a * (1.0f - src.a);
    if (a <= 0.0f) return {0.0f, 0.0f, 0.0f, 0.0f};
    const auto blend = [&](float s, float d) {
        return (s * src.a + d * dst.a * (1.0f - src.a)) / a;
    };
    return {blend(src.r, dst.r), blend(src.g, dst.g), blend(src.b, dst.b), a};
}

ColorF overRgb(Rgb src, double srcA, ColorF dst) noexcept {
    return over({static_cast<float>(src.r), static_cast<float>(src.g),
                 static_cast<float>(src.b), static_cast<float>(srcA)},
                dst);
}

// Element sub-seed tags (arbitrary, FROZEN: part of the golden contract). kSeedClouds carries
// over from the S55-a baseline; decks derive per-layer streams off it by index.
constexpr std::uint64_t kSeedClouds = 0x534b59'01;  // "SKY" 1
constexpr std::uint64_t kSeedFlare = 0x534b59'02;   // "SKY" 2 -- the starburst's spoke phase

// Selenographic mapping handedness, verified by rendering against the real near-side face (the
// LRO albedo texture's own equirectangular convention + the Meeus ch. 53 axis sign): kMoonEastSign
// orients longitude across the disc so Mare Crisium lands on the correct limb; kMoonAxisSign is the
// on-disc rotation sense of the axis position angle P.
constexpr double kMoonEastSign = -1.0;
constexpr double kMoonAxisSign = -1.0;

// ---------------------------------------------------------------------------------------------
// Display mapping (§4.4): calibrated exposure -> extended Reinhard -> sRGB encode.
// ---------------------------------------------------------------------------------------------

// Maps Hosek-Wilkie RGB radiance into display-linear units, calibrated LOW enough that the
// visible sky sits on the tonemap's near-linear range -- Reinhard compresses channel ratios as
// radiance climbs, and a washed-out desaturated blue was exactly the first render's defect.
// exposure is user EV on top.
constexpr double kBaseExposure = 0.032;

// Extended Reinhard with a fixed white point: radiances at/above kTonemapWhite map to >= 1
// (the solar disc), the sky body keeps its gradient. Public-domain math (Reinhard 2002).
constexpr double kTonemapWhite = 6.0;

// The physical single-scattering integrator (atmosphere.cpp) returns radiance in its own units;
// this places it in the same display-linear space as the Hosek-Wilkie dome so the sun-0 handoff has
// no brightness cliff. Calibrated by matching the integrator's sun-on-the-horizon dome to the HW
// el-0 dome (the frozen sunset image) in the render harness. exposure EV rides on top, as for HW.
constexpr double kScatterExposure = 0.13;
// Deep-sky floor, cool blue (added under the integrator). kAirglow is the faint constant true-night
// glow (airglow + integrated starlight) so a dark sky never crushes to pure black and the dither has
// something to work on. kTwiGlow is a residual twilight glow -- it fills the nautical-twilight sky
// and fades out by astronomical night (smoothstep -16..-4 deg) so true night is genuinely dark, not
// a flat floor. It once stood in for ALL the multiple scattering the single-scatter integrator
// omitted; the integrator now carries a real isotropic multiple-scattering term (atmosphere.cpp's
// Psi_ms table), which gives civil twilight its directional, blue-shifted ambient -- the flat
// residue here covers only what that term still cannot reach (ozone, transport beyond the
// second-order closure, the deep -8..-16 band whose light is all higher-order).
constexpr double kAirglow = 0.00025;
constexpr double kTwiGlow = 0.0018;
// The sub-horizon twilight handoff: sun elevation >= 0 is pure Hosek-Wilkie (this is 0 there, so the
// DAY PATH STAYS BYTE-IDENTICAL); the integrator fades fully in by -6 deg. Reused for the dome
// blend and the cloud relighting so both cross over together.
double twilightWeight(double sunElevationDeg) noexcept {
    const double t = clamp01(sunElevationDeg / -6.0);  // 0 at >=0, 1 at <=-6
    return t * t * (3.0 - 2.0 * t);
}

double tonemap(double linear) noexcept {
    const double x = std::max(0.0, linear);
    return x * (1.0 + x / (kTonemapWhite * kTonemapWhite)) / (1.0 + x);
}

// The standard sRGB opto-electronic transfer (IEC 61966-2-1). Inputs may exceed 1 (HDR
// headroom at the disc) -- the curve simply continues; toImage8 clips later.
float srgbEncode(double linear) noexcept {
    if (linear <= 0.0031308) return static_cast<float>(std::max(0.0, linear) * 12.92);
    return static_cast<float>(1.055 * std::pow(linear, 1.0 / 2.4) - 0.055);
}

// Deterministic TPDF dither, keyed on the FRAME pixel and channel -- so it stays parallelism-
// and window-crop-exact like every other element. Promoted to common/dither.hpp (byte-identical
// formula, the sky golden holds) so the layer-effects ramps + gradient editor share it.
using common::ditherTPDF;

// Uniform [0,1) hash of the FRAME pixel -- the volumetric marcher's per-ray march offset
// (stratified jitter breaking the fixed-step banding; see marchCloudVolume). The same splitmix64
// finaliser as ditherTPDF on its own channel tag (3), so it is equally deterministic,
// parallelism- and window-crop-exact.
double marchJitter01(std::uint32_t px, std::uint32_t py) noexcept {
    std::uint64_t v = ((static_cast<std::uint64_t>(px) << 32) ^
                       (static_cast<std::uint64_t>(py) << 3) ^ 3ull) *
                          2 +
                      1;
    v ^= v >> 30;
    v *= 0xbf58476d1ce4e5b9ULL;
    v ^= v >> 27;
    v *= 0x94d049bb133111ebULL;
    v ^= v >> 31;
    return static_cast<double>(v >> 11) * (1.0 / 9007199254740992.0);
}

// ---------------------------------------------------------------------------------------------
// Atmosphere pieces
// ---------------------------------------------------------------------------------------------

// Kasten-Young (1989) relative optical air mass for a zenith angle in degrees.
double airMass(double zenithDeg) noexcept {
    const double z = std::clamp(zenithDeg, 0.0, 90.0);
    return 1.0 / (std::cos(z * kDegToRad) + 0.50572 * std::pow(96.07995 - z, -1.6364));
}

// Rayleigh optical depth at the sRGB primaries (~612/549/465 nm, Bodhaine-style lambda^-4
// fit) + a grey Mie term growing with turbidity. Beer-Lambert transmittance over the air mass
// reddens the low sun exactly the way a real sunset does.
Rgb sunTransmittance(double sunElevationDeg, double turbidity) noexcept {
    const double m = airMass(90.0 - std::clamp(sunElevationDeg, 0.0, 90.0));
    const double haze01 = clamp01((turbidity - 1.0) / 9.0);
    const double tauMie = 0.06 * haze01 * 2.0;
    return {std::exp(-m * (0.064 + tauMie)), std::exp(-m * (0.099 + tauMie)),
            std::exp(-m * (0.194 + tauMie))};
}

// ---------------------------------------------------------------------------------------------
// The §4.3 cloud type catalogue: per-type constants over the shared knobs.
// ---------------------------------------------------------------------------------------------

struct CloudTypeSpec {
    double altitudeM;    // natural deck altitude
    double featureM;     // natural feature size at scale 1
    double covFactor;    // how much of the master coverage dial this type takes
    double softness;     // smoothstep width past the coverage threshold (edge hardness)
    double worleyMix;    // 0 = pure fBm billow .. 1 = cellular (Worley) structure
    double shear;        // wind elongation multiplier (cirrus streaks)
    double opacity;      // peak alpha (veils stay translucent)
    double baseGrey;     // how dark a dense core shades, 0..1 (1 = stays white)
    double detail;       // fBm gain (wispy high-frequency energy)
    int octaves;
    double warp;         // domain-warp amplitude: heaps billow low, wisps swirl high
};

const CloudTypeSpec& cloudTypeSpec(CloudType t) {
    // Altitudes/character from the §4.3 table (public meteorological facts); numbers are
    // artistic calibration, golden-pinned like every other constant here.
    static const std::array<CloudTypeSpec, kCloudTypeCount> kSpecs{{
        // covFactor > 1 (the sheet types) pushes the master dial toward full coverage fast --
        // a "stratus at 0.5" must already read as a sheet, not a 50/50 patchwork.
        // altM   featM  cov    soft  worley shear opac  grey  detail oct  warp
        {9000.0, 3500.0, 0.75, 0.30, 0.10, 6.0, 0.60, 0.95, 0.62, 5, 0.85},   // Cirrus
        {7500.0, 1100.0, 0.60, 0.24, 0.75, 1.6, 0.55, 0.92, 0.50, 4, 0.40},   // Cirrocumulus
        {8000.0, 14000.0, 1.20, 0.45, 0.05, 2.5, 0.50, 0.96, 0.45, 3, 0.55},  // Cirrostratus
        {4000.0, 1700.0, 0.75, 0.22, 0.65, 1.4, 0.80, 0.80, 0.52, 4, 0.45},   // Altocumulus
        {3500.0, 12000.0, 1.35, 0.50, 0.10, 1.5, 0.85, 0.72, 0.42, 3, 0.50},  // Altostratus
        {1600.0, 1400.0, 0.85, 0.18, 0.55, 1.2, 0.92, 0.62, 0.55, 5, 0.45},   // Stratocumulus
        {600.0, 9000.0, 1.50, 0.55, 0.05, 1.1, 0.96, 0.75, 0.40, 3, 0.50},    // Stratus
        {900.0, 11000.0, 1.50, 0.50, 0.10, 1.2, 1.00, 0.45, 0.42, 3, 0.50},   // Nimbostratus
        {1300.0, 1900.0, 0.90, 0.16, 0.60, 1.0, 0.96, 0.58, 0.55, 5, 0.32},   // Cumulus
        {1100.0, 3000.0, 1.10, 0.16, 0.70, 1.1, 1.00, 0.38, 0.58, 5, 0.40},   // Cumulonimbus
    }};
    return kSpecs[static_cast<std::size_t>(t)];
}

// One deck cooked for the render loop.
struct CookedDeck {
    CloudTypeSpec spec{};
    CloudType type = CloudType::Cumulus;
    double altitudeM = 0.0;
    double featureM = 1.0;
    double coverage = 0.0;     // effective 0..1 after master dial + biases
    double scaleFactor = 1.0;  // Scale * cloudScale * per-deck bias (stretches the volumetric slab)
    std::uint64_t seed = 0;
    FbmParams fbm{};
    FbmParams warpFbm{3, 2.0, 0.5};
};

// The deck's density field at a world point on its altitude plane (metres). Pure function of
// (seed, coords): wind shear elongates along the wind axis, domain warp billows, fBm + an
// optional Worley cellular term shape the body. Returns ~[0, 1].
double deckDensity(const CookedDeck& d, double wxM, double wyM, double windRad,
                   double windStrength) {
    // Rotate into the wind frame and stretch ALONG the wind: streaks follow the shear axis.
    const double ca = std::cos(windRad), sa = std::sin(windRad);
    const double along = (wxM * sa + wyM * ca);  // wind blows toward windRad (compass)
    const double across = (wxM * ca - wyM * sa);
    const double stretch = 1.0 + d.spec.shear * windStrength * 2.0;
    const double u = along / (d.featureM * stretch);
    const double v = across / d.featureM;

    const Vec2 warped = domainWarp2(subSeed(d.seed, 0x77), {u, v}, d.spec.warp,
                                    /*frequency=*/1.9, d.warpFbm);
    const double body = 0.5 + 0.5 * fbm2(d.seed, warped.x, warped.y, d.fbm);
    // High-frequency erosion: wobbles the coverage edge and grains the interior, so masses
    // read as vapour with structure instead of smooth warped blobs (weighted by detail --
    // veils stay smooth).
    const double erode = fbm2(subSeed(d.seed, 0xED0E), warped.x * 4.3, warped.y * 4.3,
                              FbmParams{2, 2.0, 0.6}) *
                         0.11 * d.spec.detail / 0.5;
    if (d.spec.worleyMix <= 0.0) return clamp01(body + erode);

    // Cellular structure: inverted F1 makes each Worley cell a puff/roll centre (frequency
    // BELOW the fBm body so puffs read broad and merge into masses, not confetti).
    const WorleyResult cell = worley2(subSeed(d.seed, 0xCE11), warped.x * 0.9, warped.y * 0.9);
    const double puff = clamp01(1.0 - cell.f1 * 0.95);
    return clamp01(body * (1.0 - d.spec.worleyMix) + puff * d.spec.worleyMix + erode);
}

// ---------------------------------------------------------------------------------------------
// The night star field (S55 night overhaul). REPLACES the S55-f hash-lattice (which had no real
// positions -> no constellations, and whose lattice + gaussian tails read as streaks). Now the
// Yale Bright Star Catalogue is projected by each star's TRUE position: RA/Dec -> alt/az via the
// observer's sidereal clock (lunar.hpp) -> world ray -> screen point (SkyCamera::project). Each
// star splats a tight gaussian PSF (a crisp point at any resolution) plus a faint broad glow on
// the brightest, tinted by its colour temperature and scaled by its magnitude. The projected list
// + a screen-space bin grid are built ONCE per render and only read per pixel, so the field stays
// a pure function of (frame pixel) -- parallelism- and window-crop-safe like every other element.
// ---------------------------------------------------------------------------------------------

// Colour temperature (K) -> normalised linear-sRGB tint. Tanner Helland's Planckian-locus
// approximation (sRGB 0..255) -> linear -> normalised to unit peak (magnitude carries brightness,
// K carries only hue) -> desaturated toward white, since to the eye all but the brightest stars
// read nearly white.
Rgb kelvinToLinearRgb(double kelvin) {
    const double t = std::clamp(kelvin, 1500.0, 40000.0) / 100.0;
    double r, g, b;
    r = t <= 66.0 ? 255.0 : 329.698727 * std::pow(t - 60.0, -0.1332047);
    g = t <= 66.0 ? 99.4708025 * std::log(t) - 161.1195682
                  : 288.1221695 * std::pow(t - 60.0, -0.0755148);
    b = t >= 66.0 ? 255.0 : (t <= 19.0 ? 0.0 : 138.5177312 * std::log(t - 10.0) - 305.0447927);
    const auto toLin = [](double c) {
        c = clamp01(c / 255.0);
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    Rgb c{toLin(r), toLin(g), toLin(b)};
    const double peak = std::max({c.r, c.g, c.b, 1e-6});
    c = {c.r / peak, c.g / peak, c.b / peak};
    constexpr double kDesat = 0.55;  // 0 = full colour, 1 = white
    return {c.r + (1.0 - c.r) * kDesat, c.g + (1.0 - c.g) * kDesat, c.b + (1.0 - c.b) * kDesat};
}

// Star-field tuning (display-linear amplitudes, calibrated so a magnitude-0 star reads as a crisp
// point that just blooms and magnitude 6.5 sits near the visibility floor).
constexpr double kStarAmp = 1.5;        // central linear radiance of a magnitude-0 star
constexpr double kStarCoreSigma = 0.72;  // px: the point-spread core
constexpr double kStarGlowSigma = 2.6;   // px: the broad bloom on bright stars
constexpr double kStarGlowFrac = 0.30;   // bloom strength relative to the core

struct ProjStar {
    double sx, sy;                  // frame pixel centre
    Rgb core;                       // core radiance (colour * amplitude)
    Rgb glow;                       // broad-bloom radiance (bright stars only)
    double reach;                   // px radius worth sampling
};

struct StarField {
    std::vector<ProjStar> stars;
    int cols = 0, rows = 0;
    double cell = 8.0;
    std::vector<std::vector<int>> bins;

    Rgb sampleAt(double fx, double fy) const {
        Rgb sum{};
        if (stars.empty()) return sum;
        const int cx = std::clamp(static_cast<int>(std::floor(fx / cell)), 0, cols - 1);
        const int cy = std::clamp(static_cast<int>(std::floor(fy / cell)), 0, rows - 1);
        for (int gy = cy - 1; gy <= cy + 1; ++gy)
            for (int gx = cx - 1; gx <= cx + 1; ++gx) {
                if (gx < 0 || gy < 0 || gx >= cols || gy >= rows) continue;
                for (const int idx : bins[static_cast<std::size_t>(gy) * cols + gx]) {
                    const ProjStar& s = stars[static_cast<std::size_t>(idx)];
                    const double dx = fx - s.sx, dy = fy - s.sy, d2 = dx * dx + dy * dy;
                    const double core =
                        std::exp(-d2 / (2.0 * kStarCoreSigma * kStarCoreSigma));
                    sum.r += s.core.r * core;
                    sum.g += s.core.g * core;
                    sum.b += s.core.b * core;
                    if (s.glow.r > 0.0 || s.glow.g > 0.0 || s.glow.b > 0.0) {
                        const double gl =
                            std::exp(-d2 / (2.0 * kStarGlowSigma * kStarGlowSigma));
                        sum.r += s.glow.r * gl;
                        sum.g += s.glow.g * gl;
                        sum.b += s.glow.b * gl;
                    }
                }
            }
        return sum;
    }
};

// Project the catalogue into the frame for this render. `starVis` folds the twilight fade and the
// user's starsAmount; empty when it is daytime (so the day path never touches a star).
StarField buildStarField(const SkyCamera& cam, const SkyParams& sky, std::uint32_t w,
                         std::uint32_t h, double starVis) {
    StarField sf;
    if (starVis <= 0.0) return sf;
    const UtcTime clock{sky.obsYear, sky.obsMonth, sky.obsDay, sky.obsHourUtc};
    const double gmst = greenwichMeanSiderealTimeDeg(clock);
    const StarEntry* cat = starCatalog();
    const std::size_t n = starCatalogCount();
    double maxReach = kStarCoreSigma * 4.0;
    sf.stars.reserve(768);
    for (std::size_t i = 0; i < n; ++i) {
        const StarEntry& s = cat[i];
        const HorizontalCoord hz = equatorialToHorizontal(
            {s.raDeg, s.decDeg}, gmst, sky.obsLatitudeDeg, sky.obsLongitudeDeg, false);
        if (hz.altitudeDeg < -1.0) continue;  // below the horizon
        double px, py;
        if (!cam.project(directionFromAzEl(hz.azimuthDeg, hz.altitudeDeg), px, py)) continue;
        // Magnitude -> linear flux; atmospheric extinction dims the low stars.
        const double ext = smoothstep(-2.0, 12.0, hz.altitudeDeg);
        const double amp =
            kStarAmp * std::pow(10.0, -0.4 * s.mag) * starVis * ext;
        if (amp < 1e-4) continue;
        const Rgb col = kelvinToLinearRgb(s.kelvin);
        // Only genuinely bright stars carry the visible bloom (a soft threshold on amplitude).
        const double glowAmp = kStarGlowFrac * amp * smoothstep(0.25, 1.2, amp);
        const double reach = 4.0 * (glowAmp > 1e-4 ? kStarGlowSigma : kStarCoreSigma);
        if (px < -reach || px > static_cast<double>(w) + reach || py < -reach ||
            py > static_cast<double>(h) + reach)
            continue;
        ProjStar ps;
        ps.sx = px;
        ps.sy = py;
        ps.core = {amp * col.r, amp * col.g, amp * col.b};
        ps.glow = {glowAmp * col.r, glowAmp * col.g, glowAmp * col.b};
        ps.reach = reach;
        sf.stars.push_back(ps);
        maxReach = std::max(maxReach, reach);
    }
    // A uniform screen-space bin grid whose cell spans the largest reach, so each pixel only needs
    // its own bin plus the eight neighbours.
    sf.cell = std::max(8.0, maxReach);
    sf.cols = static_cast<int>(std::floor(w / sf.cell)) + 2;
    sf.rows = static_cast<int>(std::floor(h / sf.cell)) + 2;
    sf.bins.assign(static_cast<std::size_t>(sf.cols) * sf.rows, {});
    for (int idx = 0; idx < static_cast<int>(sf.stars.size()); ++idx) {
        const int gx = std::clamp(static_cast<int>(std::floor(sf.stars[idx].sx / sf.cell)), 0,
                                  sf.cols - 1);
        const int gy = std::clamp(static_cast<int>(std::floor(sf.stars[idx].sy / sf.cell)), 0,
                                  sf.rows - 1);
        sf.bins[static_cast<std::size_t>(gy) * sf.cols + gx].push_back(idx);
    }
    return sf;
}

// ---------------------------------------------------------------------------------------------
// Lens flare (screen-space). Technique lineage:
//   - The classic SCREEN-SPACE flare: aperture-shaped ghost sprites, a halo and a starburst
//     placed analytically along the line through the sun's projected position and the frame
//     centre, intensity faded as the sun leaves the view. Published art: Kilgard's OpenGL
//     lens-flare tutorial (1999-2000); King, "2D Lens Flare", Game Programming Gems 1 (2000).
//   - ⚠ Hullin et al., "Physically-Based Real-Time Lens Flare Rendering" (SIGGRAPH 2011) is
//     BACKGROUND ONLY and deliberately NOT built. This flare has no lens-prescription
//     simulation, no paraxial/matrix optics, no modelling of reflections between lens
//     elements and no precomputed angle-indexed ghost look-up tables -- a standing limit on
//     the element, not an unfinished feature.
// Every term below is a pure analytic function of the FRAME pixel + constants cooked once from
// the full frame -- no image-space post pass -- so the flare stays row-parallel and window-crop
// byte-exact like every other element, and deterministic per seed.
// ---------------------------------------------------------------------------------------------

// One ghost of the fixed train: position t along C + t*(S - C) (S = the sun's screen position,
// C = the frame centre; t = 1 sits ON the sun, t < 0 past the centre on the far side), aperture
// radius as a fraction of the frame diagonal, centre energy relative to the master, and a
// coating-style tint. The train is FROZEN once a flare render is golden-pinned.
struct FlareGhost {
    double t;
    double size;
    double energy;
    Rgb tint;
};

constexpr std::array<FlareGhost, 7> kFlareGhosts{{
    {0.62, 0.024, 0.34, {1.00, 0.86, 0.58}},   // warm coating ghost near the sun
    {0.44, 0.040, 0.16, {0.70, 0.85, 1.00}},   // pale blue, wider
    {0.21, 0.015, 0.44, {1.00, 0.78, 0.52}},   // small hot amber short of the centre
    {-0.08, 0.056, 0.11, {0.58, 0.80, 1.00}},  // broad cool ghost just past the centre
    {-0.32, 0.028, 0.22, {0.62, 1.00, 0.80}},  // green coating reflection
    {-0.60, 0.078, 0.09, {0.86, 0.62, 1.00}},  // wide faint violet
    {-1.05, 0.048, 0.15, {1.00, 0.70, 0.78}},  // far rose ghost opposite the sun
}};

constexpr int kFlareSpikes = 6;         // hexagonal iris -> six starburst streaks
constexpr double kFlareFringe = 0.055;  // per-channel radius spread (chromatic fringing)

// The cooked per-render flare state; `on == false` means the pixel loop never runs a flare term
// (the enableLensFlare=false path is BYTE-INERT -- the golden pins rely on it).
struct FlareSetup {
    bool on = false;
    double sx = 0.0, sy = 0.0;  // the sun's screen position (frame px)
    double cx = 0.0, cy = 0.0;  // the frame centre
    double diag = 1.0;          // frame diagonal (px) -- the sprite size unit
    double haloR = 0.0;         // halo ring radius (px)
    double haloSigma = 1.0;     // halo ring thickness (px)
    double burstLen = 1.0;      // starburst e-folding length (px)
    double burstRot = 0.0;      // spoke rotation (radians; frozen per seed)
    double vis = 0.0;           // master energy: strength x elevation x frame x haze x tint
    Rgb hue{1.0, 1.0, 1.0};     // the sun's Beer-Lambert hue, normalised to unit peak
};

FlareSetup cookLensFlare(const SkyCamera& cam, const SkyParams& sky, std::uint64_t seed,
                         std::uint32_t w, std::uint32_t h, const SkyVec3& sunDir,
                         const Rgb& sunTint, double haze01) {
    FlareSetup f;
    if (!sky.enableSun || !sky.enableLensFlare) return f;
    const double strength = clamp01(sky.flareStrength);
    if (strength <= 0.0) return f;
    double px = 0.0, py = 0.0;
    if (!cam.project(sunDir, px, py)) return f;  // sun behind the image plane: no flare
    // The flare belongs to the SUN alone: it dies with the disc through sunset. (The moon casts
    // none -- real lens flare off moonlight is negligible and would read as a defect.)
    const double elFade = smoothstep(-0.5, 2.0, sky.sunElevationDeg);
    // Off-frame fade (the classic Kilgard falloff pattern): full strength while the sun is
    // inside the frame, dying smoothly once it has left by a quarter-diagonal.
    const double fw = static_cast<double>(w), fh = static_cast<double>(h);
    const double diag = std::hypot(fw, fh);
    const double outX = std::max({0.0, -px, px - fw});
    const double outY = std::max({0.0, -py, py - fh});
    const double frameFade = 1.0 - smoothstep(0.0, 0.25 * diag, std::hypot(outX, outY));
    // The glare block's own sun signals, inherited (NO new occlusion machinery): the Beer-
    // Lambert transmittance dims the whole flare as the low sun reddens, and haze mutes the
    // DISTINCT ghost train (the veiling glare a hazy sky adds is the aureole's business).
    const double tintMean = (sunTint.r + sunTint.g + sunTint.b) / 3.0;
    const double hazeFade = 1.0 - 0.65 * haze01;
    f.vis = strength * elFade * frameFade * hazeFade * tintMean;
    if (f.vis <= 1e-5) return f;
    f.on = true;
    f.sx = px;
    f.sy = py;
    f.cx = 0.5 * fw;
    f.cy = 0.5 * fh;
    f.diag = diag;
    // The halo ring sits around the frame centre, its radius riding the sun-centre distance so
    // it sweeps outward as the sun moves off-axis (the floor keeps a centred sun haloed).
    const double axis = std::hypot(px - f.cx, py - f.cy);
    f.haloR = 0.45 * axis + 0.12 * diag;
    f.haloSigma = 0.045 * diag;
    f.burstLen = 0.16 * diag;
    // The starburst's spoke phase is the flare's one random draw -- frozen per seed.
    f.burstRot = hashToUnit(subSeed(seed, kSeedFlare)) * (2.0 * kPi / kFlareSpikes);
    // Ghosts carry the sun's HUE, not its dimming (the dimming already rode into vis above).
    const double peak = std::max({sunTint.r, sunTint.g, sunTint.b, 1e-6});
    f.hue = {sunTint.r / peak, sunTint.g / peak, sunTint.b / peak};
    return f;
}

// A soft hexagonal-aperture sprite: ~1 inside, easing out at the polygon edge, with a gentle
// rim brightening (the way a real aperture ghost photographs). `rot` turns the iris; the radius
// profile is the classic regular-n-gon polar form (vertices at `radius`, edges at its apothem).
double flareNgon(double dx, double dy, double radius, double rot) noexcept {
    const double r = std::hypot(dx, dy);
    if (r <= 1e-9) return 0.68;
    constexpr double seg = 2.0 * kPi / kFlareSpikes;
    double th = std::atan2(dy, dx) - rot;
    th -= seg * std::floor(th / seg);
    const double polyR = std::cos(0.5 * seg) / std::cos(th - 0.5 * seg);
    const double d = r / (radius * polyR);
    const double body = 1.0 - smoothstep(0.82, 1.0, d);
    return body * (0.68 + 0.32 * smoothstep(0.30, 0.92, d));
}

// The flare radiance for ONE frame pixel: the fixed ghost train + the centre halo + the
// starburst, each analytic -- chromatic fringing evaluates the channels at slightly different
// radii (short wavelengths focus tighter, so blue rings land inside red).
Rgb lensFlareAt(const FlareSetup& f, double fx, double fy) noexcept {
    const double ax = f.sx - f.cx, ay = f.sy - f.cy;  // the centre -> sun axis
    Rgb sum{};
    for (const FlareGhost& g : kFlareGhosts) {
        const double gx = f.cx + g.t * ax, gy = f.cy + g.t * ay;
        const double radius = g.size * f.diag;
        const double reach = radius * (1.0 + kFlareFringe);
        const double dx = fx - gx, dy = fy - gy;
        if (std::abs(dx) > reach || std::abs(dy) > reach) continue;  // the common case
        const double rot = f.burstRot + g.t;  // each ghost's iris sits at its own angle
        const double e = g.energy * f.vis;
        sum.r += e * g.tint.r * f.hue.r * flareNgon(dx, dy, reach, rot);
        sum.g += e * g.tint.g * f.hue.g * flareNgon(dx, dy, radius, rot);
        sum.b += e * g.tint.b * f.hue.b * flareNgon(dx, dy, radius * (1.0 - kFlareFringe), rot);
    }
    {
        // The wide halo ring around the frame centre, dispersion-fringed: red lands outside.
        const double r = std::hypot(fx - f.cx, fy - f.cy);
        const double s2 = 2.0 * f.haloSigma * f.haloSigma;
        const double e = 0.085 * f.vis;
        const double dR = r - f.haloR * 1.045;
        const double dG = r - f.haloR;
        const double dB = r - f.haloR * 0.955;
        sum.r += e * f.hue.r * std::exp(-dR * dR / s2);
        sum.g += e * f.hue.g * std::exp(-dG * dG / s2);
        sum.b += e * f.hue.b * std::exp(-dB * dB / s2);
    }
    {
        // The starburst: thin streaks through the sun (a high power of the spoke cosine keeps
        // them needle-thin), radially exponential, spoke phase frozen per seed.
        const double dx = fx - f.sx, dy = fy - f.sy;
        const double r = std::hypot(dx, dy);
        if (r > 1e-9 && r < 4.0 * f.burstLen) {
            const double phi = std::atan2(dy, dx);
            const double lobe =
                std::pow(0.5 + 0.5 * std::cos(kFlareSpikes * (phi - f.burstRot)), 26.0);
            const double e = 0.60 * f.vis * lobe * std::exp(-r / f.burstLen);
            sum.r += e * f.hue.r;
            sum.g += e * f.hue.g;
            sum.b += e * f.hue.b;
        }
    }
    return sum;
}

}  // namespace

common::ImageF renderSkyTexture(const TextureParams& p, const SkyParams& sky, std::uint32_t w,
                                std::uint32_t h, const TextureWindow& window,
                                TextureRenderProgress* progress) {
    if (w == 0 || h == 0) return common::ImageF(w, h);
    // The camera (and every projected element) is built from the FULL frame; the window merely
    // selects which of its pixels get evaluated -- a byte-exact crop (§8.2).
    const ResolvedWindow win = resolveWindow(window, w, h);
    common::ImageF out(win.w, win.h);
    if (win.w == 0 || win.h == 0) return out;
    if (progress != nullptr) progress->rowsTotal.store(win.h, std::memory_order_relaxed);

    const SkyCamera cam = SkyCamera::fromParams(sky, w, h);
    const SkyVec3 sunDir = directionFromAzEl(sky.sunAzimuthDeg, sky.sunElevationDeg);
    const SkyVec3 sunHoriz = skyNormalize({sunDir.x, sunDir.y, 0.0});

    // ---- Night factors (S55-f night + phase-4 physical twilight) -----------------------------
    // Everything nocturnal keys off how far the sun sits BELOW the horizon; at elevation >= 0
    // every factor here is 0 and the day path stays byte-identical. night01 saturates by -12 deg
    // (past nautical twilight) and drives moonlight/earthshine strength; stars need the sky darker
    // still. twiW is the physical-twilight handoff: 0 for a daytime sun (pure Hosek-Wilkie), 1 by
    // -6 deg (full single-scattering integrator), a smooth crossfade between -- so the day->night
    // transition is REAL scattering geometry, not a crossfade to a hand-authored gradient.
    const double sunEl = sky.sunElevationDeg;
    const double night01 = clamp01(-sunEl / 12.0);
    const double twiW = twilightWeight(sunEl);
    const double starVis = clamp01((-sunEl - 7.0) / 9.0) * clamp01(sky.starsAmount);
    // The real star field: project the Yale BSC once, then splat per pixel (empty by day).
    const StarField starField = buildStarField(cam, sky, w, h, starVis);
    // The moon (an element like the sun); its PHASE falls out of the real sun-moon geometry --
    // sunDir carries the below-horizon elevation, so a midnight sun lights the far side and the
    // moon renders full when opposite. Visible faintly by day too (it really is).
    const bool moonOn = sky.enableMoon;
    const SkyVec3 moonDir = directionFromAzEl(sky.moonAzimuthDeg, sky.moonElevationDeg);
    const double moonDiscRad = 0.259 * kDegToRad * std::max(0.05, sky.moonScale);
    // The Moon's illumination direction (uniform over the disc, the Sun being far away). Default is
    // the legacy scene-sun lighting (phase falls out of geometry); the phase-control modes below
    // override it so the disc is not "full every night". moonPhaseIllum (1 full .. 0 new) drives the
    // halo/cloud-lift and is recomputed from whatever lights the disc.
    SkyVec3 moonLightDir = sunDir;
    double moonPhaseIllum = clamp01(0.5 - 0.5 * skyDot(moonDir, sunDir));
    const double moonUp =
        clamp01(std::sin(std::max(0.0, sky.moonElevationDeg) * kDegToRad) * 4.0);
    // The real Moon (S55 night overhaul, phase 3): the LRO albedo texture sphere-mapped onto the
    // disc, its near side turned toward us by the Meeus ch. 53 physical ephemeris (optical libration
    // + the axis position angle) off the observer clock, phase-lit by the scene sun. The frame is
    // built once here; each on-disc ray reads a selenographic (lon, lat) from it below.
    const SkyVec3 moonToObs{-moonDir.x, -moonDir.y, -moonDir.z};  // disc-centre surface normal
    SkyVec3 moonDiscRight{1.0, 0.0, 0.0};
    SkyVec3 moonDiscUp{0.0, 0.0, 1.0};
    SkyVec3 moonNorth{0.0, 0.0, 1.0};  // lunar north pole, world space
    double moonLibLonDeg = 0.0;
    if (moonOn) {
        // Disc frame: discUp toward the observer's zenith at the Moon's position, discRight east.
        SkyVec3 rr = skyCross(moonDir, {0.0, 0.0, 1.0});
        if (rr.x * rr.x + rr.y * rr.y + rr.z * rr.z < 1e-12) rr = {1.0, 0.0, 0.0};  // Moon at zenith
        moonDiscRight = skyNormalize(rr);
        moonDiscUp = skyNormalize(skyCross(moonDiscRight, moonDir));
        const MoonPhysical mphys =
            moonPhysical(UtcTime{sky.obsYear, sky.obsMonth, sky.obsDay, sky.obsHourUtc});
        moonLibLonDeg = mphys.librationLonDeg;
        // Celestial north projected onto the disc gives the parallactic tilt for this position;
        // rotate by the axis position angle P for lunar north, then tip it out of the disc plane by
        // the libration in latitude (b > 0 => the north pole leans toward the observer).
        const SkyVec3 ncp = directionFromAzEl(0.0, sky.obsLatitudeDeg);
        const double nd = skyDot(ncp, moonDir);
        SkyVec3 ncpProj{ncp.x - nd * moonDir.x, ncp.y - nd * moonDir.y, ncp.z - nd * moonDir.z};
        if (ncpProj.x * ncpProj.x + ncpProj.y * ncpProj.y + ncpProj.z * ncpProj.z < 1e-12)
            ncpProj = moonDiscUp;
        ncpProj = skyNormalize(ncpProj);
        const double phiNcp =
            std::atan2(skyDot(ncpProj, moonDiscRight), skyDot(ncpProj, moonDiscUp));
        const double thetaN = phiNcp + kMoonAxisSign * mphys.axisPositionAngleDeg * kDegToRad;
        const double cN = std::cos(thetaN), sN = std::sin(thetaN);
        const SkyVec3 northInPlane{cN * moonDiscUp.x + sN * moonDiscRight.x,
                                   cN * moonDiscUp.y + sN * moonDiscRight.y,
                                   cN * moonDiscUp.z + sN * moonDiscRight.z};
        const double bRad = mphys.librationLatDeg * kDegToRad;
        const double sb = std::sin(bRad), cb = std::cos(bRad);
        moonNorth = skyNormalize({sb * moonToObs.x + cb * northInPlane.x,
                                  sb * moonToObs.y + cb * northInPlane.y,
                                  sb * moonToObs.z + cb * northInPlane.z});

        // Phase control (user 2026-07-15): synthesise the illumination direction from a phase angle
        // and a bright-limb direction so the moon is not always full. alpha is the phase angle
        // (0 full .. pi new); brightDir is the in-disc unit toward the lit limb. L reconstructed as
        // cos(alpha)*toObs + sin(alpha)*brightDir -> alpha 0 lights the whole near side (full),
        // alpha pi/2 a half moon, alpha pi the far side (new). Mode 0 keeps L = the scene sun.
        if (sky.moonPhaseMode == 1 || sky.moonPhaseMode == 2) {
            double alpha;
            SkyVec3 brightDir;
            if (sky.moonPhaseMode == 2) {
                // Ephemeris: the real phase for the observer clock. The bright limb sits at position
                // angle chi from celestial north toward the east (screen-left, the kMoonAxisSign sense).
                const MoonPhase mp =
                    moonPhase(UtcTime{sky.obsYear, sky.obsMonth, sky.obsDay, sky.obsHourUtc});
                alpha = mp.phaseAngleDeg * kDegToRad;
                const double ba = phiNcp + kMoonAxisSign * mp.brightLimbAngleDeg * kDegToRad;
                brightDir = {std::cos(ba) * moonDiscUp.x + std::sin(ba) * moonDiscRight.x,
                             std::cos(ba) * moonDiscUp.y + std::sin(ba) * moonDiscRight.y,
                             std::cos(ba) * moonDiscUp.z + std::sin(ba) * moonDiscRight.z};
            } else {
                // Manual: the illuminated fraction sets the terminator; the lit limb faces the
                // scene sun's azimuth (drag the sun and the crescent points at it).
                const double k = clamp01(sky.moonIlluminatedFraction);
                alpha = std::acos(std::clamp(2.0 * k - 1.0, -1.0, 1.0));
                const double sp = skyDot(sunDir, moonDir);
                SkyVec3 proj{sunDir.x - sp * moonDir.x, sunDir.y - sp * moonDir.y,
                             sunDir.z - sp * moonDir.z};
                brightDir = proj.x * proj.x + proj.y * proj.y + proj.z * proj.z > 1e-10
                                ? skyNormalize(proj)
                                : moonDiscRight;
            }
            const double ca2 = std::cos(alpha), sa2 = std::sin(alpha);
            moonLightDir = skyNormalize({ca2 * moonToObs.x + sa2 * brightDir.x,
                                         ca2 * moonToObs.y + sa2 * brightDir.y,
                                         ca2 * moonToObs.z + sa2 * brightDir.z});
            moonPhaseIllum = clamp01(0.5 + 0.5 * skyDot(moonLightDir, moonToObs));
        }
    }

    const double turbidity = std::clamp(sky.turbidity, 1.0, 10.0);
    const double haze01 = clamp01((turbidity - 1.0) / 9.0);
    const double albedo = clamp01(sky.groundAlbedo);
    const double exposureScale = kBaseExposure * std::exp2(sky.exposure);

    // Physical single-scattering twilight (phase 4): cooked ONLY when the sun is at/below the -6..0
    // handoff (twiW > 0). A pure daytime render never constructs it, so the day path is untouched.
    // scatterScale maps the integrator's radiance into the HW display-linear space (+ user EV).
    const bool twilightActive = twiW > 0.0;
    const double scatterScale = kScatterExposure * std::exp2(sky.exposure);
    const Atmosphere atmo = twilightActive ? cookAtmosphere(sunDir, turbidity) : Atmosphere{};
    // The deep-sky floor for this render: airglow + a twilight residual that fades out toward
    // astronomical night, all ramped in by twiW so the day path never sees it.
    const double nightFloor =
        twiW * (kAirglow + kTwiGlow * smoothstep(-16.0, -4.0, sunEl));
    // The integrator radiance in display-linear units, plus the deep-sky floor and (when a bright
    // moon is up) a subtle cool moon-skylight lift. Pure function of the ray.
    const auto scatterDisplay = [&](const SkyVec3& dir) -> Rgb {
        const Rgb p = atmo.radiance(dir);
        Rgb c{p.r * scatterScale, p.g * scatterScale, p.b * scatterScale};
        c.r += nightFloor * 0.55;
        c.g += nightFloor * 0.72;
        c.b += nightFloor * 1.00;
        if (moonOn) {
            const double lift = 0.010 * moonUp * moonPhaseIllum * twiW;
            c.r += lift * 0.75;
            c.g += lift * 0.85;
            c.b += lift * 1.15;
        }
        return c;
    };

    // The Hosek-Wilkie state cooks once per render (the model is valid for sun AT/above the
    // horizon; the artistic elevation clamps into range). Everything per-pixel is the cheap
    // 9-parameter evaluation.
    const double solarElevRad = std::clamp(sky.sunElevationDeg, 0.0, 90.0) * kDegToRad;
    using HwState = std::unique_ptr<ArHosekSkyModelState, void (*)(ArHosekSkyModelState*)>;
    HwState hw(arhosek_rgb_skymodelstate_alloc_init(turbidity, albedo, solarElevRad),
               &arhosekskymodelstate_free);

    // Day dome radiance for a UNIT direction at/above the horizon, in display-linear units.
    const auto dayRadiance = [&](const SkyVec3& dir) -> Rgb {
        const double cosTheta = std::clamp(dir.z, 0.0, 1.0);
        const double theta = std::acos(cosTheta);
        const double gamma = std::acos(std::clamp(skyDot(dir, sunDir), -1.0, 1.0));
        Rgb c;
        c.r = arhosek_tristim_skymodel_radiance(hw.get(), theta, gamma, 0) * exposureScale;
        c.g = arhosek_tristim_skymodel_radiance(hw.get(), theta, gamma, 1) * exposureScale;
        c.b = arhosek_tristim_skymodel_radiance(hw.get(), theta, gamma, 2) * exposureScale;
        return c;
    };

    // Twilight/night dome (S55 phase 4 -- PHYSICAL single scattering): at sun elevation >= 0 this
    // returns the Hosek-Wilkie day dome untouched (the day path stays BYTE-IDENTICAL). Below the
    // horizon it crossfades that (frozen at the HW el-0 validity floor -- the sunset image) into the
    // clean-room Nishita integrator over the 0..-6 handoff. Everything the eye reads at twilight --
    // the warm horizon band, the Belt of Venus / anti-twilight arch, the deep-blue nautical band,
    // the darkening from the horizon up as Earth's shadow rises -- is EMERGENT scattering geometry
    // (atmosphere.cpp), not an authored gradient. Stars ride the (now dark) dome as before.
    const auto domeRadiance = [&](const SkyVec3& dir) -> Rgb {
        const Rgb day = dayRadiance(dir);
        if (!twilightActive) return day;  // sun at/above the horizon: pure Hosek-Wilkie
        return mixRgb(day, scatterDisplay(dir), twiW);
    };

    // Sun tint (Beer-Lambert over air mass): computed regardless of enableSun -- clouds are lit
    // by the sun even when the disc element itself is toggled off.
    const Rgb sunTint = sunTransmittance(sky.sunElevationDeg, turbidity);
    const double sunDiscRad = 0.255 * kDegToRad * std::max(0.05, sky.sunDiscScale);
    // Disc radiance: far above the tonemap white point, so a high sun clips to white while a
    // low one keeps the Beer-Lambert reddening (the tint scales all three channels).
    const Rgb discColor{40.0 * sunTint.r, 40.0 * sunTint.g, 40.0 * sunTint.b};

    // Lens flare cook (screen-space; the lineage block above cookLensFlare). Off, sub-horizon,
    // behind-the-camera or fully faded => flare.on stays false and the pixel loop below never
    // touches a byte -- the default-off path is exactly the pre-flare renderer.
    const FlareSetup flare = cookLensFlare(cam, sky, p.seed, w, h, sunDir, sunTint, haze01);

    // Cook the enabled decks, far to near (altitude DESCENDING -- painter's order for an
    // upward-looking camera; ties keep declaration order).
    std::vector<CookedDeck> decks;
    if (sky.enableClouds) {
        decks.reserve(sky.cloudLayers.size());
        const std::uint64_t cloudSeed = subSeed(p.seed, kSeedClouds);
        for (std::size_t i = 0; i < sky.cloudLayers.size(); ++i) {
            const CloudLayerParams& layer = sky.cloudLayers[i];
            if (!layer.enabled) continue;
            CookedDeck d;
            d.spec = cloudTypeSpec(layer.type);
            d.type = layer.type;
            d.altitudeM = layer.altitudeM > 0.0 ? layer.altitudeM : d.spec.altitudeM;
            d.scaleFactor = p.scale * sky.cloudScale * std::max(0.05, layer.scaleBias);
            d.featureM = std::max(10.0, d.spec.featureM * d.scaleFactor);
            d.coverage = clamp01(sky.cloudCoverage * d.spec.covFactor *
                                 std::max(0.0, layer.coverageBias));
            d.seed = subSeed(cloudSeed, i + 1);
            d.fbm = FbmParams{d.spec.octaves, 2.0, d.spec.detail};
            decks.push_back(d);
        }
        std::stable_sort(decks.begin(), decks.end(), [](const CookedDeck& a, const CookedDeck& b) {
            return a.altitudeM > b.altitudeM;
        });
    }

    const double windRad = sky.windDirectionDeg * kDegToRad;
    const double windStrength = clamp01(sky.windStrength);
    // Aerial-perspective visibility: Koschmieder-style, shrinking with turbidity. Distant
    // decks fade into the horizon haze instead of piling into a wall of cloud.
    const double visibilityM = 160000.0 / (1.0 + 2.2 * (turbidity - 1.0));

    // Shade colors for the decks (display-linear): lit faces carry the sun tint, shaded cores
    // fall toward a cool ambient that darkens as the sun drops.
    const double sunUp = clamp01(std::sin(std::max(0.0, sky.sunElevationDeg) * kDegToRad) * 3.0);
    // Lit faces take the sun's HUE, not its dimming (real lit cumulus is white at midday --
    // normalising the transmittance keeps golden-hour warmth without midday beige).
    const double tintMax = std::max({sunTint.r, sunTint.g, sunTint.b, 1e-6});
    Rgb litBase = mixRgb({1.12, 1.10, 1.06},
                         {1.12 * sunTint.r / tintMax, 1.12 * sunTint.g / tintMax,
                          1.12 * sunTint.b / tintMax},
                         0.55);
    Rgb ambBase = mixRgb({0.16, 0.17, 0.22}, {0.34, 0.40, 0.52}, sunUp);
    if (twilightActive) {
        // Relight the decks from the SCATTERED twilight, not a constant (phase 4, requirement 4).
        // A hemispheric skylight sampled from the integrator -- warm toward the sunset, deep blue
        // away, dark once night falls -- becomes the clouds' ambient/key, plus a cool moonlight key
        // when a bright moon is up. The direct-sun warm term (litBase/ambBase above) fades out as
        // the sun sinks past -6, so twilight clouds go from sunset-underlit to moon/sky-lit
        // silhouettes coherently. Feeds the volumetric lane too (volLight below).
        const Rgb skyZen = scatterDisplay({0.0, 0.0, 1.0});
        const Rgb skyToward = scatterDisplay(skyNormalize({sunHoriz.x, sunHoriz.y, 0.5}));
        const Rgb skyAway = scatterDisplay(skyNormalize({-sunHoriz.x, -sunHoriz.y, 0.5}));
        const Rgb skyLight{0.5 * skyZen.r + 0.3 * skyToward.r + 0.2 * skyAway.r,
                           0.5 * skyZen.g + 0.3 * skyToward.g + 0.2 * skyAway.g,
                           0.5 * skyZen.b + 0.3 * skyToward.b + 0.2 * skyAway.b};
        const double nl = moonOn ? moonUp * moonPhaseIllum : 0.0;
        const Rgb moonKey{nl * 0.105, nl * 0.113, nl * 0.144};  // cool moonlight on the tops
        const Rgb litTwi{6.0 * skyLight.r + moonKey.r, 6.0 * skyLight.g + moonKey.g,
                         6.0 * skyLight.b + moonKey.b};
        const Rgb ambTwi{2.6 * skyLight.r + 0.40 * moonKey.r, 2.6 * skyLight.g + 0.44 * moonKey.g,
                         2.6 * skyLight.b + 0.62 * moonKey.b};
        litBase = mixRgb(litBase, litTwi, twiW);
        ambBase = mixRgb(ambBase, ambTwi, twiW);
    }

    // Volumetric lane (S55-c): the heap/tower types march an implicit 3D density field instead of
    // the 2D projection. The lit/ambient radiances above are its sunlit-face / skylight colours.
    const bool volumetric = sky.volumetricClouds;
    const CloudVolumeLight volLight{sunDir, litBase, ambBase};

    parallelRows(win.h, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            const double fy = static_cast<double>(win.y) + y;  // frame row of this window row
            for (std::uint32_t x = 0; x < win.w; ++x) {
                const double fx = static_cast<double>(win.x) + x;
                const SkyVec3 dir = cam.rayAt(fx + 0.5, fy + 0.5);
                const double elev = std::asin(std::clamp(dir.z, -1.0, 1.0));
                ColorF acc{0.0f, 0.0f, 0.0f, 0.0f};

                // Horizon-direction radiance: the haze tint for this azimuth, shared by the
                // below-horizon band, the haze element and the decks' aerial perspective.
                const SkyVec3 horizDir =
                    skyNormalize({dir.x, dir.y, 0.035});  // graze just above the horizon
                Rgb horizonRgb{0.0, 0.0, 0.0};
                bool horizonCooked = false;
                const auto horizon = [&]() -> const Rgb& {
                    if (!horizonCooked) {
                        horizonRgb = domeRadiance(horizDir);
                        horizonCooked = true;
                    }
                    return horizonRgb;
                };

                if (sky.enableDome) {
                    if (dir.z >= 0.0) {
                        const Rgb c = domeRadiance(dir);
                        acc = {static_cast<float>(c.r), static_cast<float>(c.g),
                               static_cast<float>(c.b), 1.0f};
                    } else {
                        // Below the horizon: the §4.5 ground band -- the horizon's own haze
                        // colour receding into murk (reads as distant ground/sea; keeps a
                        // dome-on layer opaque so default skies composite like S55-a).
                        const double depth = -elev / kDegToRad;  // degrees below horizon
                        const double fade = 0.10 + 0.90 * std::exp(-depth / 2.6);
                        const Rgb hz = horizon();
                        acc = {static_cast<float>(hz.r * fade * 0.80),
                               static_cast<float>(hz.g * fade * 0.83),
                               static_cast<float>(hz.b * fade * 0.90), 1.0f};
                    }
                }

                if (starVis > 0.0 && sky.enableDome && dir.z >= 0.0) {
                    // Stars ride the dome element (they ARE the sky background): additive PSF
                    // radiance from the projected catalogue (per-star magnitude, colour and
                    // horizon extinction are already baked into buildStarField).
                    const Rgb s = starField.sampleAt(fx + 0.5, fy + 0.5);
                    acc.r += static_cast<float>(s.r);
                    acc.g += static_cast<float>(s.g);
                    acc.b += static_cast<float>(s.b);
                }

                if (sky.enableHaze && acc.a > 0.0f && dir.z >= 0.0) {
                    // The haze ELEMENT: an artistic aerial-perspective boost over what the
                    // dome already carries -- pulls low-elevation sky toward the horizon tint.
                    // Modulates existing coverage only (nothing beneath -> nothing, §3.4).
                    const double amt =
                        haze01 * std::pow(1.0 - clamp01(std::sin(elev) / 0.35), 2.5) * 0.32;
                    if (amt > 1e-4) {
                        const Rgb hz = horizon();
                        acc.r += static_cast<float>((hz.r - acc.r) * amt);
                        acc.g += static_cast<float>((hz.g - acc.g) * amt);
                        acc.b += static_cast<float>((hz.b - acc.b) * amt);
                    }
                }

                if (sky.enableSun && dir.z >= 0.0) {
                    // Disc + aureole, clipped at the world horizon (a setting sun shows its
                    // top half, like a sea horizon). BEHIND the decks (see the header
                    // deviation note): thick sheets swallow it, veils blur it, cirrus dims
                    // it -- plain alpha.
                    const double gamma =
                        std::acos(std::clamp(skyDot(dir, sunDir), -1.0, 1.0));
                    const double dNorm = gamma / sunDiscRad;
                    // S55-f follow-up ("the sun is a little unconvincing"): the physically-sized
                    // disc read as a pinprick because the glare died at its limb. Photographic
                    // glare instead: a softer limb, a WIDE aureole that carries HDR radiance
                    // several disc-radii out (eyes and cameras both bloom the sun), and a
                    // slightly broader scatter glow. Deliberate golden re-bless.
                    const double disc = 1.0 - smoothstep(0.86, 1.22, dNorm);
                    const double aur =
                        std::exp(-(dNorm * dNorm) / (2.0 * 30.25)) * (0.42 + 0.55 * haze01);
                    const double glow =
                        std::exp(-gamma / (0.13 + 0.30 * haze01)) * (0.12 + 0.34 * haze01);
                    const double cov = clamp01(disc + aur + glow);
                    if (cov > 1e-4) {
                        const double t = clamp01(disc + 0.58 * aur);
                        // Disc core is HDR-bright; the halo relaxes to the tinted glow.
                        const Rgb c = mixRgb({1.35 * sunTint.r + 0.10, 1.32 * sunTint.g + 0.10,
                                              1.25 * sunTint.b + 0.10},
                                             discColor, t);
                        acc = overRgb(c, cov, acc);
                    }
                }

                if (moonOn && dir.z >= 0.0) {
                    // The moon: the real LRO albedo sphere-mapped onto the disc (S55 phase 3),
                    // lit by moonLightDir (scene sun / manual / ephemeris phase), with earthshine
                    // on the dark side. Same slot as the sun: behind the cloud decks (§4.4).
                    const double gammaM =
                        std::acos(std::clamp(skyDot(dir, moonDir), -1.0, 1.0));
                    const double dN = gammaM / moonDiscRad;
                    if (dN < 7.0 && night01 > 0.0) {
                        // A soft moonlight halo so the night disc reads as a glowing body, not
                        // a sticker; scaled by phase (a crescent barely glows).
                        const double halo = std::exp(-(dN * dN) / (2.0 * 6.25)) * 0.055 *
                                            night01 * (0.25 + 0.75 * moonPhaseIllum);
                        if (halo > 1e-4) {
                            const Rgb c{halo + acc.r, halo * 1.02 + acc.g, halo * 1.12 + acc.b};
                            acc = overRgb(c, clamp01(halo * 2.5), acc);
                        }
                    }
                    // The emitted disc colour + coverage for ONE ray (returns false past the limb).
                    const auto discEmit = [&](const SkyVec3& sdir, Rgb& emit, double& cov) -> bool {
                        const double gm =
                            std::acos(std::clamp(skyDot(sdir, moonDir), -1.0, 1.0));
                        const double dn = gm / moonDiscRad;
                        if (dn >= 1.03) return false;
                        cov = 1.0 - smoothstep(0.975, 1.03, dn);
                        if (cov <= 0.0) return false;
                        // Disc coordinates in the observer-vertical frame, then the outward sphere
                        // normal of this surface point (z toward the observer = -moonDir).
                        const double a = skyDot(sdir, moonDiscRight) / moonDiscRad;
                        const double bb = skyDot(sdir, moonDiscUp) / moonDiscRad;
                        const double r2 = std::min(1.0, a * a + bb * bb);
                        const double zc = std::sqrt(1.0 - r2);
                        const SkyVec3 n = skyNormalize(
                            {a * moonDiscRight.x + bb * moonDiscUp.x - zc * moonDir.x,
                             a * moonDiscRight.y + bb * moonDiscUp.y - zc * moonDir.y,
                             a * moonDiscRight.z + bb * moonDiscUp.z - zc * moonDir.z});
                        // Selenographic (lon, lat): latitude off the lunar north pole; longitude as
                        // the signed angle about it from the sub-Earth meridian (+ libration).
                        const double lat = std::asin(std::clamp(skyDot(n, moonNorth), -1.0, 1.0));
                        const double wN = skyDot(moonToObs, moonNorth), nN = skyDot(n, moonNorth);
                        const SkyVec3 wEq{moonToObs.x - wN * moonNorth.x,
                                          moonToObs.y - wN * moonNorth.y,
                                          moonToObs.z - wN * moonNorth.z};
                        const SkyVec3 nEq{n.x - nN * moonNorth.x, n.y - nN * moonNorth.y,
                                          n.z - nN * moonNorth.z};
                        const double relLon =
                            std::atan2(skyDot(moonNorth, skyCross(wEq, nEq)), skyDot(wEq, nEq));
                        const double lonDeg = moonLibLonDeg + relLon * kRadToDeg;
                        const double u = 0.5 + kMoonEastSign * lonDeg / 360.0;
                        const double v = 0.5 - lat * kRadToDeg / 180.0;
                        const double albedo = sampleMoonAlbedo(u, v);
                        // Terrain relief (the LOLA elevation map): differentiate the height
                        // through the SAME (u, v) mapping the albedo reads -- the chain rule
                        // carries kMoonEastSign, so whatever orientation the maps share, the
                        // slope lands along the true local east -- and tilt the shading normal
                        // by the metres-per-metre slopes. Physically scaled (no gain): crater
                        // rims and maria edges shadow hard exactly at the terminator (grazing
                        // incidence) and flatten out by full phase, as real photographs do.
                        SkyVec3 nS = n;
                        {
                            constexpr double dU = 1.0 / kMoonElevationWidth;
                            constexpr double dV = 1.0 / kMoonElevationHeight;
                            const double dhdu = (sampleMoonElevation(u + dU, v) -
                                                 sampleMoonElevation(u - dU, v)) /
                                                (2.0 * dU);
                            const double dhdv = (sampleMoonElevation(u, v + dV) -
                                                 sampleMoonElevation(u, v - dV)) /
                                                (2.0 * dV);
                            // u = 0.5 + sign*lon/360, v = 0.5 - lat/180 (deg) -> per-radian.
                            const double dhdLon = dhdu * kMoonEastSign / (2.0 * kPi);
                            const double dhdLat = -dhdv / kPi;
                            const SkyVec3 eastRaw = skyCross(moonNorth, n);
                            const double el2 = eastRaw.x * eastRaw.x + eastRaw.y * eastRaw.y +
                                               eastRaw.z * eastRaw.z;
                            if (el2 > 1e-12) {  // undefined at the poles: leave the sphere normal
                                const SkyVec3 east = skyNormalize(eastRaw);
                                const SkyVec3 north = skyCross(n, east);  // unit by construction
                                const double cosLat = std::max(0.05, std::cos(lat));
                                // Foreshortening kills resolvable relief at the limb (and the
                                // map gradient degenerates there): fade the tilt out over the
                                // outer ~5% of the radius so the limb stays a clean arc.
                                const double limbFade = smoothstep(0.05, 0.30, zc);
                                const double se =
                                    limbFade * dhdLon / (kMoonRadiusM * cosLat);
                                const double sn = limbFade * dhdLat / kMoonRadiusM;
                                nS = skyNormalize({n.x - se * east.x - sn * north.x,
                                                   n.y - se * east.y - sn * north.y,
                                                   n.z - se * east.z - sn * north.z});
                            }
                        }
                        // Lommel-Seeliger reflectance -- the Moon's near-uniform full disc plus a
                        // real terminator -- lit by moonLightDir.
                        const double cosI = skyDot(nS, moonLightDir);
                        const double cosE = skyDot(nS, moonToObs);
                        const double illumination =
                            cosI > 0.0 ? clamp01(2.0 * cosI / (cosI + cosE + 1e-3)) : 0.0;
                        const double earthshine = 0.02 * night01;
                        const double kMoon = 0.35 + 3.05 * night01;  // bright at night, pale by day
                        const double lum = albedo * (kMoon * illumination + earthshine);
                        emit = {lum * 0.99, lum * 0.985, lum * 0.94};
                        return true;
                    };
                    if (dN < 1.4) {
                        // 4x rotated-grid supersampling: at one sample per pixel the sharp disc
                        // edge, the terminator and the minified surface texture read as an aliased
                        // "pixelated sticker" (the user's 1:1 complaint). Only disc-adjacent pixels
                        // pay; the smooth halo above stays single-sampled.
                        static constexpr double kRG[4][2] = {{0.125, 0.375}, {0.375, -0.125},
                                                             {-0.125, -0.375}, {-0.375, 0.125}};
                        Rgb premul{0.0, 0.0, 0.0};
                        double covSum = 0.0;
                        for (const auto& o : kRG) {
                            Rgb emit;
                            double cov = 0.0;
                            if (discEmit(cam.rayAt(fx + 0.5 + o[0], fy + 0.5 + o[1]), emit, cov)) {
                                premul.r += emit.r * cov;
                                premul.g += emit.g * cov;
                                premul.b += emit.b * cov;
                                covSum += cov;
                            }
                        }
                        if (covSum > 1e-4) {
                            // The atmosphere scatters IN FRONT of the moon, so the disc ADDS to the
                            // sky (a crescent's dark side vanishes into blue by day, not a black
                            // hole). Reconstruct the additive straight colour from the coverage-
                            // weighted mean emission and composite once with the mean coverage.
                            const Rgb c{premul.r / covSum + acc.r, premul.g / covSum + acc.g,
                                        premul.b / covSum + acc.b};
                            acc = overRgb(c, covSum / 4.0, acc);
                        }
                    }
                }

                if (!decks.empty() && dir.z > 1e-4) {
                    for (const CookedDeck& d : decks) {
                        if (d.coverage <= 1e-4) continue;

                        if (volumetric && cloudTypeIsVolumetric(d.type)) {
                            // Cheap horizon cull before the (expensive) march: a deck whose base
                            // is already past the visibility depth fades to nothing anyway.
                            const double tEnterGuess = d.altitudeM / dir.z;
                            if (std::exp(-tEnterGuess / visibilityM) <= 1e-3) continue;
                            const CloudVolumeSpec vs = cloudVolumeSpec(
                                d.type, d.altitudeM, d.featureM, d.coverage, d.scaleFactor);
                            const CloudVolumeSample s = marchCloudVolume(
                                d.seed, vs, volLight, dir, windRad, windStrength,
                                marchJitter01(static_cast<std::uint32_t>(win.x) + x,
                                              static_cast<std::uint32_t>(win.y) + y));
                            if (s.coverage <= 1e-4) continue;
                            const double fade = std::exp(-s.firstHitM / visibilityM);
                            if (fade <= 1e-3) continue;
                            // scatter is premultiplied in-scatter; overRgb wants the straight
                            // colour + alpha, so divide the coverage back out (coverage > 1e-4).
                            Rgb c{s.scatter.r / s.coverage, s.scatter.g / s.coverage,
                                  s.scatter.b / s.coverage};
                            double cov = s.coverage * fade;
                            if (acc.a > 0.0f) {
                                const Rgb hz = horizon();
                                c = mixRgb(hz, c, 0.25 + 0.75 * fade);
                                cov = s.coverage * (0.25 + 0.75 * fade);
                            }
                            acc = overRgb(c, cov, acc);
                            continue;
                        }

                        const double t = d.altitudeM / dir.z;  // metres to the deck plane
                        const double wxM = dir.x * t, wyM = dir.y * t;
                        const double n01 = deckDensity(d, wxM, wyM, windRad, windStrength);
                        const double thr = 1.0 - d.coverage;
                        const double covRaw =
                            smoothstep(thr, thr + d.spec.softness, n01) * d.spec.opacity;
                        if (covRaw <= 1e-4) continue;

                        // Aerial perspective: transmittance along the slant path.
                        const double fade = std::exp(-t / visibilityM);
                        if (fade <= 1e-3) continue;

                        // Shading: density past the threshold darkens the core; the flank
                        // facing the sun catches light (one extra sample toward the sun);
                        // forward scatter silver-lines everything near the sun.
                        const double thick = smoothstep(thr, 1.0, n01);
                        const double nLit =
                            deckDensity(d, wxM + sunHoriz.x * d.featureM * 0.35,
                                        wyM + sunHoriz.y * d.featureM * 0.35, windRad,
                                        windStrength);
                        const double edgeLit = std::clamp((n01 - nLit) * 2.8, -0.45, 0.9);
                        const double gamma =
                            std::acos(std::clamp(skyDot(dir, sunDir), -1.0, 1.0));
                        const double forward = std::exp(-gamma / 0.45);

                        double light = 0.72 + 0.5 * edgeLit - (1.0 - d.spec.baseGrey) * thick;
                        light = clamp01(light) * (0.85 + 0.45 * forward);
                        Rgb c = mixRgb(ambBase, litBase, clamp01(light));
                        // Distant decks dissolve into the horizon haze (tint only while a
                        // dome exists beneath; over transparency they just fade out). The
                        // tint eases in gently so mid-distance clouds keep their own colour.
                        double cov = covRaw * fade;
                        if (acc.a > 0.0f) {
                            const Rgb hz = horizon();
                            c = mixRgb(hz, c, 0.25 + 0.75 * fade);
                            cov = covRaw * (0.25 + 0.75 * fade);
                        }
                        acc = overRgb(c, cov, acc);
                    }
                }

                if (flare.on) {
                    // Lens flare (screen-space): lens light, so it rides OVER the decks --
                    // a cloud cannot occlude a reflection born inside the lens. Additive over an
                    // opaque dome; over transparency it carries its own coverage (the moon-disc
                    // additive-reconstruction idiom above).
                    const Rgb fl = lensFlareAt(flare, fx + 0.5, fy + 0.5);
                    const double fmax = std::max({fl.r, fl.g, fl.b});
                    if (fmax > 1e-5) {
                        const double cov = clamp01(3.0 * fmax);
                        const Rgb c{fl.r / cov + acc.r, fl.g / cov + acc.g, fl.b / cov + acc.b};
                        acc = overRgb(c, cov, acc);
                    }
                }

                // Display mapping (§4.4): tonemap + sRGB-encode the linear radiances, keep
                // float precision (and any >1 disc headroom) in the cache. A ~1-LSB TPDF dither
                // (keyed to the frame pixel) breaks 8-bit banding on the shallow gradients when the
                // cache is later quantised for display/bake.
                const std::uint32_t pxi = static_cast<std::uint32_t>(win.x) + x;
                const std::uint32_t pyi = static_cast<std::uint32_t>(win.y) + y;
                constexpr double kDitherLsb = 1.0 / 255.0;
                out.set(x, y,
                        {static_cast<float>(std::max(
                             0.0, srgbEncode(tonemap(acc.r)) + ditherTPDF(pxi, pyi, 0) * kDitherLsb)),
                         static_cast<float>(std::max(
                             0.0, srgbEncode(tonemap(acc.g)) + ditherTPDF(pxi, pyi, 1) * kDitherLsb)),
                         static_cast<float>(std::max(
                             0.0, srgbEncode(tonemap(acc.b)) + ditherTPDF(pxi, pyi, 2) * kDitherLsb)),
                         acc.a});
            }
            if (!progressRowTick(progress)) return;
        }
    });
    return out;
}

// ---- §7.4 preset library (S55-f) ---------------------------------------------------------------
// Each preset is a complete SkyParams value; the dialog sets every knob from it, then the sliders
// fine-tune. Contrails / mammatus / lenticular-style looks are DECK RECIPES over the §4.3 type
// catalogue (per texture_params.hpp: presets over types, not new types). "Golden hour" carries the
// S55-b render-pass find (turbidity 4.5, sun 6 degrees -- genuinely beautiful).

namespace {

SkyParams presetClearDay() {
    SkyParams s;  // crystalline blue, not a cloud in sight
    s.turbidity = 1.8;
    s.cloudCoverage = 0.0;
    s.cloudLayers.clear();
    return s;
}

SkyParams presetFairWeather() {
    SkyParams s;  // the default two-deck §4.5 parallax demo, slightly fuller cumulus
    s.cloudCoverage = 0.45;
    return s;
}

SkyParams presetGoldenHour() {
    SkyParams s;  // the S55-b hero: low warm sun through a hazy sky
    s.sunElevationDeg = 6.0;
    s.turbidity = 4.5;
    s.cloudCoverage = 0.35;
    s.cloudLayers = {CloudLayerParams{true, CloudType::Altocumulus, 1.0, 1.0, 0.0},
                     CloudLayerParams{true, CloudType::Cirrus, 0.8, 1.0, 0.0}};
    return s;
}

SkyParams presetHighCirrus() {
    SkyParams s;  // wind-swept high wisps over a clear dome
    s.turbidity = 2.0;
    s.cloudCoverage = 0.5;
    s.windStrength = 0.85;
    s.cloudLayers = {CloudLayerParams{true, CloudType::Cirrus, 1.0, 1.0, 0.0},
                     CloudLayerParams{true, CloudType::Cirrocumulus, 0.6, 1.0, 0.0}};
    return s;
}

SkyParams presetSunsetStreaks() {
    SkyParams s;  // contrail-style sheared streaks catching a setting sun
    s.sunElevationDeg = 3.0;
    s.turbidity = 5.5;
    s.cloudCoverage = 0.45;
    s.windDirectionDeg = 70.0;
    s.windStrength = 1.0;
    s.cloudLayers = {CloudLayerParams{true, CloudType::Cirrus, 1.2, 0.8, 0.0},
                     CloudLayerParams{true, CloudType::Cirrostratus, 0.6, 1.0, 0.0}};
    return s;
}

SkyParams presetOvercast() {
    SkyParams s;  // a flat grey lid; the sun survives only as a bright patch
    s.turbidity = 6.0;
    s.cloudCoverage = 0.85;
    s.sunElevationDeg = 38.0;
    s.cloudLayers = {CloudLayerParams{true, CloudType::Stratus, 1.0, 1.0, 0.0},
                     CloudLayerParams{true, CloudType::Altostratus, 0.8, 1.0, 0.0}};
    return s;
}

SkyParams presetStormBrewing() {
    SkyParams s;  // a cumulonimbus tower under a mid-level sheet -- the S55-c showcase
    s.turbidity = 3.5;
    s.cloudCoverage = 0.6;
    s.sunElevationDeg = 22.0;
    s.windStrength = 0.7;
    s.cloudLayers = {CloudLayerParams{true, CloudType::Cumulonimbus, 1.1, 1.0, 0.0},
                     CloudLayerParams{true, CloudType::Altocumulus, 0.7, 1.0, 0.0}};
    return s;
}

SkyParams presetMoonlitNight() {
    SkyParams s;  // deep night: a near-full moon (a sliver of terminator keeps it reading 3D)
    s.sunAzimuthDeg = 15.0;  // behind the camera -> the afterglow arch stays out of frame
    s.sunElevationDeg = -30.0;
    s.turbidity = 2.0;
    s.cloudCoverage = 0.25;
    s.enableMoon = true;
    s.moonAzimuthDeg = 200.0;
    s.moonElevationDeg = 30.0;  // inside the default framing
    s.moonScale = 2.2;          // the postcard telephoto moon (1.0 = the honest 0.26 deg speck)
    s.moonPhaseMode = 1;
    s.moonIlluminatedFraction = 0.92;
    s.starsAmount = 0.85;
    s.windStrength = 0.4;
    s.cloudLayers = {CloudLayerParams{true, CloudType::Cirrus, 0.9, 1.0, 0.0}};
    return s;
}

SkyParams presetCloudyMoonlit() {
    SkyParams s;  // broken stratocumulus with a gibbous moon catching the cloud tops
    s.sunAzimuthDeg = 20.0;
    s.sunElevationDeg = -28.0;
    s.turbidity = 2.4;
    s.cloudCoverage = 0.55;
    s.enableMoon = true;
    s.moonAzimuthDeg = 190.0;
    s.moonElevationDeg = 42.0;
    s.moonScale = 2.0;
    s.moonPhaseMode = 1;
    s.moonIlluminatedFraction = 0.85;
    s.starsAmount = 0.55;
    s.windStrength = 0.5;
    s.cloudLayers = {CloudLayerParams{true, CloudType::Stratocumulus, 1.0, 1.0, 0.0},
                     CloudLayerParams{true, CloudType::Cirrus, 0.6, 1.2, 0.0}};
    return s;
}

SkyParams presetCrescentClouds() {
    SkyParams s;  // a thin crescent low over scattered cumulus, stars between the gaps
    s.sunAzimuthDeg = 235.0;  // sun just below WSW -> the crescent leans toward it
    s.sunElevationDeg = -16.0;
    s.turbidity = 2.2;
    s.cloudCoverage = 0.4;
    s.enableMoon = true;
    s.moonAzimuthDeg = 210.0;
    s.moonElevationDeg = 26.0;
    s.moonScale = 2.4;
    s.moonPhaseMode = 1;
    s.moonIlluminatedFraction = 0.2;
    s.starsAmount = 0.8;
    s.windStrength = 0.45;
    s.cloudLayers = {CloudLayerParams{true, CloudType::Cumulus, 0.8, 1.0, 0.0},
                     CloudLayerParams{true, CloudType::Altocumulus, 0.5, 1.0, 0.0}};
    return s;
}

SkyParams presetOvercastNight() {
    SkyParams s;  // a flat overcast lid; the near-full moon survives only as a diffuse glow
    s.sunAzimuthDeg = 0.0;
    s.sunElevationDeg = -25.0;
    s.turbidity = 3.0;
    s.cloudCoverage = 0.85;
    s.enableMoon = true;
    s.moonAzimuthDeg = 185.0;
    s.moonElevationDeg = 46.0;
    s.moonScale = 2.0;
    s.moonPhaseMode = 1;
    s.moonIlluminatedFraction = 0.95;
    s.starsAmount = 0.22;
    s.windStrength = 0.5;
    s.cloudLayers = {CloudLayerParams{true, CloudType::Stratus, 1.0, 1.0, 0.0},
                     CloudLayerParams{true, CloudType::Altostratus, 0.8, 1.0, 0.0}};
    return s;
}

SkyParams presetStormyNight() {
    SkyParams s;  // a cumulonimbus tower under a mid-level sheet, moon breaking through
    s.sunAzimuthDeg = 30.0;
    s.sunElevationDeg = -22.0;
    s.turbidity = 3.5;
    s.cloudCoverage = 0.6;
    s.enableMoon = true;
    s.moonAzimuthDeg = 205.0;
    s.moonElevationDeg = 36.0;
    s.moonScale = 2.2;
    s.moonPhaseMode = 1;
    s.moonIlluminatedFraction = 0.7;
    s.starsAmount = 0.4;
    s.windStrength = 0.7;
    s.cloudLayers = {CloudLayerParams{true, CloudType::Cumulonimbus, 1.0, 1.0, 0.0},
                     CloudLayerParams{true, CloudType::Altocumulus, 0.6, 1.0, 0.0}};
    return s;
}

SkyParams presetMistyMoonrise() {
    SkyParams s;  // a big low moon behind a low stratus band, the horizon framed near the bottom
    s.sunAzimuthDeg = 200.0;
    s.sunElevationDeg = -14.0;
    s.turbidity = 3.2;
    s.cloudCoverage = 0.5;
    s.pitchDeg = 8.0;  // drop the horizon so the rising moon sits low in the frame
    s.enableMoon = true;
    s.moonAzimuthDeg = 180.0;
    s.moonElevationDeg = 11.0;
    s.moonScale = 3.0;
    s.moonPhaseMode = 1;
    s.moonIlluminatedFraction = 0.6;
    s.starsAmount = 0.5;
    s.windStrength = 0.3;
    s.cloudLayers = {CloudLayerParams{true, CloudType::Stratus, 0.7, 1.2, 0.0},
                     CloudLayerParams{true, CloudType::Stratocumulus, 0.5, 1.0, 0.0}};
    return s;
}

SkyParams presetStarryCirrus() {
    SkyParams s;  // an almost-clear night: bright stars, a wisp of cirrus, a slim crescent
    s.sunAzimuthDeg = 10.0;
    s.sunElevationDeg = -32.0;
    s.turbidity = 1.9;
    s.cloudCoverage = 0.35;
    s.enableMoon = true;
    s.moonAzimuthDeg = 215.0;
    s.moonElevationDeg = 44.0;
    s.moonScale = 1.8;
    s.moonPhaseMode = 1;
    s.moonIlluminatedFraction = 0.32;  // slim, so the star field dominates
    s.starsAmount = 1.0;
    s.windStrength = 0.7;
    s.cloudLayers = {CloudLayerParams{true, CloudType::Cirrus, 0.7, 1.0, 0.0}};
    return s;
}

const std::array<SkyPreset, 14> kSkyPresets{{
    {"Clear day", presetClearDay()},
    {"Fair-weather cumulus", presetFairWeather()},
    {"Golden hour", presetGoldenHour()},
    {"High cirrus", presetHighCirrus()},
    {"Sunset streaks", presetSunsetStreaks()},
    {"Overcast", presetOvercast()},
    {"Storm brewing", presetStormBrewing()},
    {"Moonlit night", presetMoonlitNight()},
    {"Cloudy moonlit night", presetCloudyMoonlit()},
    {"Crescent & scattered cloud", presetCrescentClouds()},
    {"Overcast night", presetOvercastNight()},
    {"Stormy night", presetStormyNight()},
    {"Misty moonrise", presetMistyMoonrise()},
    {"Starry high cirrus", presetStarryCirrus()},
}};

}  // namespace

std::size_t skyPresetCount() {
    return kSkyPresets.size();
}

const SkyPreset& skyPreset(std::size_t i) {
    return kSkyPresets[std::min(i, kSkyPresets.size() - 1)];
}

}  // namespace mosaic::core::texture
