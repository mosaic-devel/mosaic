#include "render/texture_gpu.hpp"

#include <ArHosekSkyModel.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

#include <vk_mem_alloc.h>

#include "core/texture/atmosphere.hpp"
#include "core/texture/cloud_volume.hpp"
#include "core/texture/lunar.hpp"
#include "core/texture/moon_elevation.hpp"
#include "core/texture/moon_texture.hpp"
#include "core/texture/noise.hpp"
#include "core/texture/render_support.hpp"
#include "core/texture/sky_camera.hpp"
#include "core/texture/star_catalog.hpp"
#include "render/gpu_policy.hpp"
#include "render/vulkan_context.hpp"
#include <shaders/texture_paper.comp.spv.hpp>
#include <shaders/texture_sky.comp.spv.hpp>

// The Vulkan compute lane of the Texture Generator (S55-h). Two halves live here:
//
//   1. The COOK -- everything the CPU renderer computes once per render (sky_render.cpp's and
//      paper_render.cpp's preambles) reproduced host-side in DOUBLE, partly by calling the same
//      public core code (SkyCamera, cookAtmosphere, cloudVolumeSpec, the lunar/star/solar
//      solvers, the Hosek-Wilkie state) and partly by TRANSCRIPTION of their file-local pieces
//      (the CloudTypeSpec table, sunTransmittance, the star-field builder, the moon frame).
//      ⚠ The transcribed constants are golden-pinned in their home TUs and may not drift here:
//      the parity tests (tests/test_texture_gpu.cpp) hold this copy to the CPU lane, so a retune
//      over there fails parity HERE, loudly, on any machine with a device.
//
//   2. The PLUMBING -- the GpuCompositor pattern (persistent context, VMA rgba32f storage image,
//      mapped staging readback) with the extrude lane's persistent-buffer discipline, plus row-
//      band dispatches so TextureRenderProgress cancel/progress work at dispatch granularity.
namespace mosaic::render {
namespace {

using core::texture::Atmosphere;
using core::texture::CloudType;
using core::texture::PaperParams;
using core::texture::ResolvedWindow;
using core::texture::Rgb;
using core::texture::SkyCamera;
using core::texture::SkyParams;
using core::texture::SkyVec3;
using core::texture::TextureParams;
using core::texture::TextureRenderProgress;
using core::texture::TextureRenderResult;
using core::texture::TextureWindow;

constexpr VkFormat kFloatFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr std::uint32_t kBandRows = 64;       // dispatch band height (cancel granularity)
constexpr std::uint32_t kMaxDecks = 8;        // more enabled decks -> the CPU lane serves
constexpr VkDeviceSize kMaxImageBytes = 512ull << 20;  // refuse absurd targets; CPU handles them

// Descriptor layout: kBindings slots, of which one (binding 1) is the storage-image target and
// the rest are storage buffers. Both counts are checked against the device's limits at init.
constexpr std::uint32_t kBindings = 7;
constexpr std::uint32_t kStorageBuffers = kBindings - 1;

constexpr double kPi = std::numbers::pi;
constexpr double kDegToRad = kPi / 180.0;

double clamp01(double v) noexcept {
    return std::clamp(v, 0.0, 1.0);
}

double smoothstepD(double e0, double e1, double v) noexcept {
    const double t = clamp01((v - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

SkyVec3 skyCross(SkyVec3 a, SkyVec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// ---------------------------------------------------------------------------------------------
// std430 mirrors of the shader parameter blocks (texture_sky.comp / texture_paper.comp
// binding 0). Field packing documented in the shaders; keep the three in lockstep.
// ---------------------------------------------------------------------------------------------

struct DeckGpu {
    float a[4];  // altitudeM, featureM, coverage, opacity
    float b[4];  // softness, worleyMix, baseGrey, warpAmp
    float c[4];  // fbm octaves, fbm gain (detail), erodeAmp, shearStretch
    float f[4];  // isVolumetric, pad, pad, pad
    std::uint32_t s0[4];  // seedBody, seedWarpChX
    std::uint32_t s1[4];  // seedWarpChY, seedErode
    std::uint32_t s2[4];  // seedWorley, vSeedBody
    std::uint32_t s3[4];  // vSeedWarpX, vSeedWarpY
    std::uint32_t s4[4];  // vSeedWarpZ, vSeedPuff
    std::uint32_t s5[4];  // vSeedErode, pad
    float v0[4];          // vBaseM, vThicknessM, vFeatureM, vCoverage
    float v1[4];          // vExtinction, vWorleyMix, vErosion, vWarp
    float v2[4];          // vAnvil, vBaseSoft, vTopSoft, vShearStretch
    float v3[4];          // vHgG, vPowder, vPrimarySteps, vLightSteps
    float v4[4];          // vBodyOctaves, pad, pad, pad
};
static_assert(sizeof(DeckGpu) == 240);

struct SkyHeadGpu {
    std::int32_t frame[4];   // frameW, frameH, winX, winY
    std::int32_t win[4];     // winW, winH, deckCount, starCount
    std::int32_t flags[4];   // enableDome, enableSun, enableHaze, moonOn
    std::int32_t flags2[4];  // twilightActive, starCols, starRows, atmo viewSteps
    float camRight[4];       // xyz, halfTanX
    float camUp[4];          // xyz, halfTanY
    float camForward[4];     // xyz, shiftY
    float camMisc[4];        // invW, invH, exposureScale, scatterScale
    float sunDir[4];         // xyz, sunDiscRad
    float sunHoriz[4];       // xyz, cos(windRad)
    float sunTint[4];        // rgb, sin(windRad)
    float discColor[4];      // rgb, haze01
    float litBase[4];        // rgb, visibilityM
    float ambBase[4];        // rgb, star grid cell
    float night[4];          // night01, twiW, starVis, nightFloor
    float moonDir[4];        // xyz, moonDiscRad
    float moonToObs[4];      // xyz, moonPhaseIllum
    float moonRight[4];      // xyz, moonUp01
    float moonUp[4];         // xyz, moonLibLonDeg
    float moonNorth[4];      // xyz, pad
    float moonLight[4];      // xyz, pad
    float atmo[4];           // betaMieScatter, betaMieExtinct, mieG, pad
    float flare0[4];         // sun screen x/y, frame centre x/y
    float flare1[4];         // diag, haloR, haloSigma, burstLen
    float flare2[4];         // burstRot, vis, on, pad
    float flareHue[4];       // the sun's unit-peak hue, pad
    float hwConfig[28];      // Hosek-Wilkie 9 coefficients per channel (R, G, B), padded
    float hwRad[4];          // Hosek-Wilkie radiances (R, G, B), padded
};
static_assert(sizeof(SkyHeadGpu) == 544);

struct PaperGpu {
    std::int32_t frame[4];  // frameW, frameH, winX, winY
    std::int32_t win[4];    // winW, winH, kind, flags (bit0 deckle, bit1 print)
    float grain[4];         // feature, stretch, cos(grainAngle), sin(grainAngle)
    float fiber[4];         // fiberFeature, fiberFreq, fiberOmega, fiberAmt
    float laid[4];          // laidPitch, chainPitch, laidDepth, feltFeature
    float light[4];         // lx, ly, lz, relief
    float oren[4];          // onA, onB, sheen, printGrain
    float halfv[4];         // Hx, Hy, Hz, printAmount
    float deckle[4];        // band, deckleFeature, deckleAmount, pad
    std::uint32_t seedA[4];  // toothSeed, fiberSeed
    std::uint32_t seedB[4];  // laidSeed, feltSeed
    std::uint32_t seedC[4];  // deckleSeed, printSeed
};
static_assert(sizeof(PaperGpu) == 192);

void putVec(float dst[4], SkyVec3 v, double w) {
    dst[0] = static_cast<float>(v.x);
    dst[1] = static_cast<float>(v.y);
    dst[2] = static_cast<float>(v.z);
    dst[3] = static_cast<float>(w);
}

void putRgb(float dst[4], Rgb c, double w) {
    dst[0] = static_cast<float>(c.r);
    dst[1] = static_cast<float>(c.g);
    dst[2] = static_cast<float>(c.b);
    dst[3] = static_cast<float>(w);
}

void putSeed(std::uint32_t dst[2], std::uint64_t seed) {
    dst[0] = static_cast<std::uint32_t>(seed);         // lo
    dst[1] = static_cast<std::uint32_t>(seed >> 32);   // hi
}

// ---------------------------------------------------------------------------------------------
// Sky cook -- transcribed from sky_render.cpp's per-render preamble (see the file header note).
// ---------------------------------------------------------------------------------------------

// Element sub-seed tags (sky_render.cpp kSeedClouds/kSeedFlare; FROZEN, golden contract).
constexpr std::uint64_t kSeedClouds = 0x534b59'01;
constexpr std::uint64_t kSeedFlare = 0x534b59'02;

// Display-mapping calibration (sky_render.cpp kBaseExposure/kScatterExposure/kAirglow/kTwiGlow).
constexpr double kBaseExposure = 0.032;
constexpr double kScatterExposure = 0.13;
constexpr double kAirglow = 0.00025;
constexpr double kTwiGlow = 0.0018;

double twilightWeight(double sunElevationDeg) noexcept {
    const double t = clamp01(sunElevationDeg / -6.0);
    return t * t * (3.0 - 2.0 * t);
}

// Kasten-Young air mass + Beer-Lambert sun tint (sky_render.cpp airMass/sunTransmittance).
double airMass(double zenithDeg) noexcept {
    const double z = std::clamp(zenithDeg, 0.0, 90.0);
    return 1.0 / (std::cos(z * kDegToRad) + 0.50572 * std::pow(96.07995 - z, -1.6364));
}

Rgb sunTransmittance(double sunElevationDeg, double turbidity) noexcept {
    const double m = airMass(90.0 - std::clamp(sunElevationDeg, 0.0, 90.0));
    const double haze01 = clamp01((turbidity - 1.0) / 9.0);
    const double tauMie = 0.06 * haze01 * 2.0;
    return {std::exp(-m * (0.064 + tauMie)), std::exp(-m * (0.099 + tauMie)),
            std::exp(-m * (0.194 + tauMie))};
}

// The §4.3 catalogue constants (sky_render.cpp cloudTypeSpec; golden-pinned THERE).
struct CloudTypeSpec {
    double altitudeM;
    double featureM;
    double covFactor;
    double softness;
    double worleyMix;
    double shear;
    double opacity;
    double baseGrey;
    double detail;
    int octaves;
    double warp;
};

const CloudTypeSpec& cloudTypeSpec(CloudType t) {
    static const std::array<CloudTypeSpec, core::texture::kCloudTypeCount> kSpecs{{
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

// Star-field tuning + colour mapping (sky_render.cpp; golden-pinned THERE).
constexpr double kStarAmp = 1.5;
constexpr double kStarCoreSigma = 0.72;
constexpr double kStarGlowSigma = 2.6;
constexpr double kStarGlowFrac = 0.30;

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
    constexpr double kDesat = 0.55;
    return {c.r + (1.0 - c.r) * kDesat, c.g + (1.0 - c.g) * kDesat, c.b + (1.0 - c.b) * kDesat};
}

// Selenographic mapping handedness (sky_render.cpp; verified against the real near side).
constexpr double kMoonAxisSign = -1.0;

struct SkyCook {
    bool supported = false;
    SkyHeadGpu head{};
    std::vector<DeckGpu> decks;
    std::vector<float> atmoTab;           // od + ms tables when twilightActive, else empty
    std::vector<float> stars;             // 8 floats per projected star
    std::vector<std::uint32_t> bins;      // CSR: binStart[cols*rows + 1] then star indices
    bool needsMoonTables = false;
};

SkyCook cookSky(const TextureParams& p, const SkyParams& sky, std::uint32_t w, std::uint32_t h,
                const ResolvedWindow& win) {
    SkyCook out;

    const SkyCamera cam = SkyCamera::fromParams(sky, w, h);
    const SkyVec3 sunDir =
        core::texture::directionFromAzEl(sky.sunAzimuthDeg, sky.sunElevationDeg);
    const SkyVec3 sunHoriz = core::texture::skyNormalize({sunDir.x, sunDir.y, 0.0});

    const double sunEl = sky.sunElevationDeg;
    const double night01 = clamp01(-sunEl / 12.0);
    const double twiW = twilightWeight(sunEl);
    const double starVis = clamp01((-sunEl - 7.0) / 9.0) * clamp01(sky.starsAmount);

    // ---- star field (sky_render.cpp buildStarField, host-side in double) --------------------
    struct ProjStar {
        double sx, sy;
        Rgb core, glow;
    };
    std::vector<ProjStar> projStars;
    double starCell = 8.0;
    int starCols = 1, starRows = 1;
    if (starVis > 0.0) {
        const core::texture::UtcTime clock{sky.obsYear, sky.obsMonth, sky.obsDay, sky.obsHourUtc};
        const double gmst = core::texture::greenwichMeanSiderealTimeDeg(clock);
        const core::texture::StarEntry* cat = core::texture::starCatalog();
        const std::size_t n = core::texture::starCatalogCount();
        double maxReach = kStarCoreSigma * 4.0;
        projStars.reserve(768);
        for (std::size_t i = 0; i < n; ++i) {
            const core::texture::StarEntry& s = cat[i];
            const core::texture::HorizontalCoord hz = core::texture::equatorialToHorizontal(
                {s.raDeg, s.decDeg}, gmst, sky.obsLatitudeDeg, sky.obsLongitudeDeg, false);
            if (hz.altitudeDeg < -1.0) continue;
            double px, py;
            if (!cam.project(core::texture::directionFromAzEl(hz.azimuthDeg, hz.altitudeDeg), px,
                             py))
                continue;
            const double ext = smoothstepD(-2.0, 12.0, hz.altitudeDeg);
            const double amp = kStarAmp * std::pow(10.0, -0.4 * s.mag) * starVis * ext;
            if (amp < 1e-4) continue;
            const Rgb col = kelvinToLinearRgb(s.kelvin);
            const double glowAmp = kStarGlowFrac * amp * smoothstepD(0.25, 1.2, amp);
            const double reach = 4.0 * (glowAmp > 1e-4 ? kStarGlowSigma : kStarCoreSigma);
            if (px < -reach || px > static_cast<double>(w) + reach || py < -reach ||
                py > static_cast<double>(h) + reach)
                continue;
            projStars.push_back({px, py,
                                 {amp * col.r, amp * col.g, amp * col.b},
                                 {glowAmp * col.r, glowAmp * col.g, glowAmp * col.b}});
            maxReach = std::max(maxReach, reach);
        }
        starCell = std::max(8.0, maxReach);
        starCols = static_cast<int>(std::floor(w / starCell)) + 2;
        starRows = static_cast<int>(std::floor(h / starCell)) + 2;
        std::vector<std::vector<int>> binLists(
            static_cast<std::size_t>(starCols) * static_cast<std::size_t>(starRows));
        for (int idx = 0; idx < static_cast<int>(projStars.size()); ++idx) {
            const int gx = std::clamp(
                static_cast<int>(std::floor(projStars[static_cast<std::size_t>(idx)].sx /
                                            starCell)),
                0, starCols - 1);
            const int gy = std::clamp(
                static_cast<int>(std::floor(projStars[static_cast<std::size_t>(idx)].sy /
                                            starCell)),
                0, starRows - 1);
            binLists[static_cast<std::size_t>(gy) * starCols + gx].push_back(idx);
        }
        // Flatten to CSR: per-bin star order preserved, so the GPU accumulates in the CPU's order.
        out.bins.resize(binLists.size() + 1, 0);
        for (std::size_t b = 0; b < binLists.size(); ++b)
            out.bins[b + 1] = out.bins[b] + static_cast<std::uint32_t>(binLists[b].size());
        for (const auto& list : binLists)
            for (const int idx : list) out.bins.push_back(static_cast<std::uint32_t>(idx));
        out.stars.reserve(projStars.size() * 8);
        for (const ProjStar& s : projStars) {
            out.stars.push_back(static_cast<float>(s.sx));
            out.stars.push_back(static_cast<float>(s.sy));
            out.stars.push_back(static_cast<float>(s.core.r));
            out.stars.push_back(static_cast<float>(s.core.g));
            out.stars.push_back(static_cast<float>(s.core.b));
            out.stars.push_back(static_cast<float>(s.glow.r));
            out.stars.push_back(static_cast<float>(s.glow.g));
            out.stars.push_back(static_cast<float>(s.glow.b));
        }
    }

    // ---- the moon frame (sky_render.cpp, verbatim transcription) ----------------------------
    const bool moonOn = sky.enableMoon;
    const SkyVec3 moonDir =
        core::texture::directionFromAzEl(sky.moonAzimuthDeg, sky.moonElevationDeg);
    const double moonDiscRad = 0.259 * kDegToRad * std::max(0.05, sky.moonScale);
    SkyVec3 moonLightDir = sunDir;
    double moonPhaseIllum = clamp01(0.5 - 0.5 * core::texture::skyDot(moonDir, sunDir));
    const double moonUp =
        clamp01(std::sin(std::max(0.0, sky.moonElevationDeg) * kDegToRad) * 4.0);
    const SkyVec3 moonToObs{-moonDir.x, -moonDir.y, -moonDir.z};
    SkyVec3 moonDiscRight{1.0, 0.0, 0.0};
    SkyVec3 moonDiscUp{0.0, 0.0, 1.0};
    SkyVec3 moonNorth{0.0, 0.0, 1.0};
    double moonLibLonDeg = 0.0;
    if (moonOn) {
        SkyVec3 rr = skyCross(moonDir, {0.0, 0.0, 1.0});
        if (rr.x * rr.x + rr.y * rr.y + rr.z * rr.z < 1e-12) rr = {1.0, 0.0, 0.0};
        moonDiscRight = core::texture::skyNormalize(rr);
        moonDiscUp = core::texture::skyNormalize(skyCross(moonDiscRight, moonDir));
        const core::texture::MoonPhysical mphys = core::texture::moonPhysical(
            core::texture::UtcTime{sky.obsYear, sky.obsMonth, sky.obsDay, sky.obsHourUtc});
        moonLibLonDeg = mphys.librationLonDeg;
        const SkyVec3 ncp = core::texture::directionFromAzEl(0.0, sky.obsLatitudeDeg);
        const double nd = core::texture::skyDot(ncp, moonDir);
        SkyVec3 ncpProj{ncp.x - nd * moonDir.x, ncp.y - nd * moonDir.y, ncp.z - nd * moonDir.z};
        if (ncpProj.x * ncpProj.x + ncpProj.y * ncpProj.y + ncpProj.z * ncpProj.z < 1e-12)
            ncpProj = moonDiscUp;
        ncpProj = core::texture::skyNormalize(ncpProj);
        const double phiNcp =
            std::atan2(core::texture::skyDot(ncpProj, moonDiscRight),
                       core::texture::skyDot(ncpProj, moonDiscUp));
        const double thetaN = phiNcp + kMoonAxisSign * mphys.axisPositionAngleDeg * kDegToRad;
        const double cN = std::cos(thetaN), sN = std::sin(thetaN);
        const SkyVec3 northInPlane{cN * moonDiscUp.x + sN * moonDiscRight.x,
                                   cN * moonDiscUp.y + sN * moonDiscRight.y,
                                   cN * moonDiscUp.z + sN * moonDiscRight.z};
        const double bRad = mphys.librationLatDeg * kDegToRad;
        const double sb = std::sin(bRad), cb = std::cos(bRad);
        moonNorth = core::texture::skyNormalize({sb * moonToObs.x + cb * northInPlane.x,
                                                 sb * moonToObs.y + cb * northInPlane.y,
                                                 sb * moonToObs.z + cb * northInPlane.z});
        if (sky.moonPhaseMode == 1 || sky.moonPhaseMode == 2) {
            double alpha;
            SkyVec3 brightDir;
            if (sky.moonPhaseMode == 2) {
                const core::texture::MoonPhase mp = core::texture::moonPhase(
                    core::texture::UtcTime{sky.obsYear, sky.obsMonth, sky.obsDay,
                                           sky.obsHourUtc});
                alpha = mp.phaseAngleDeg * kDegToRad;
                const double ba = phiNcp + kMoonAxisSign * mp.brightLimbAngleDeg * kDegToRad;
                brightDir = {std::cos(ba) * moonDiscUp.x + std::sin(ba) * moonDiscRight.x,
                             std::cos(ba) * moonDiscUp.y + std::sin(ba) * moonDiscRight.y,
                             std::cos(ba) * moonDiscUp.z + std::sin(ba) * moonDiscRight.z};
            } else {
                const double k = clamp01(sky.moonIlluminatedFraction);
                alpha = std::acos(std::clamp(2.0 * k - 1.0, -1.0, 1.0));
                const double sp = core::texture::skyDot(sunDir, moonDir);
                SkyVec3 proj{sunDir.x - sp * moonDir.x, sunDir.y - sp * moonDir.y,
                             sunDir.z - sp * moonDir.z};
                brightDir = proj.x * proj.x + proj.y * proj.y + proj.z * proj.z > 1e-10
                                ? core::texture::skyNormalize(proj)
                                : moonDiscRight;
            }
            const double ca2 = std::cos(alpha), sa2 = std::sin(alpha);
            moonLightDir = core::texture::skyNormalize({ca2 * moonToObs.x + sa2 * brightDir.x,
                                                        ca2 * moonToObs.y + sa2 * brightDir.y,
                                                        ca2 * moonToObs.z + sa2 * brightDir.z});
            moonPhaseIllum = clamp01(0.5 + 0.5 * core::texture::skyDot(moonLightDir, moonToObs));
        }
    }

    const double turbidity = std::clamp(sky.turbidity, 1.0, 10.0);
    const double haze01 = clamp01((turbidity - 1.0) / 9.0);
    const double albedo = clamp01(sky.groundAlbedo);
    const double exposureScale = kBaseExposure * std::exp2(sky.exposure);

    const bool twilightActive = twiW > 0.0;
    const double scatterScale = kScatterExposure * std::exp2(sky.exposure);
    const Atmosphere atmo = twilightActive ? core::texture::cookAtmosphere(sunDir, turbidity)
                                           : Atmosphere{};
    const double nightFloor =
        twiW * (kAirglow + kTwiGlow * smoothstepD(-16.0, -4.0, sunEl));
    const auto scatterDisplay = [&](const SkyVec3& dir) -> Rgb {
        const Rgb pr = atmo.radiance(dir);
        Rgb c{pr.r * scatterScale, pr.g * scatterScale, pr.b * scatterScale};
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

    // Hosek-Wilkie state: cooked by the vendored reference exactly as the CPU lane cooks it.
    const double solarElevRad = std::clamp(sky.sunElevationDeg, 0.0, 90.0) * kDegToRad;
    ArHosekSkyModelState* hw =
        arhosek_rgb_skymodelstate_alloc_init(turbidity, albedo, solarElevRad);
    for (int ch = 0; ch < 3; ++ch) {
        for (int i = 0; i < 9; ++i)
            out.head.hwConfig[ch * 9 + i] = static_cast<float>(hw->configs[ch][i]);
        out.head.hwRad[ch] = static_cast<float>(hw->radiances[ch]);
    }
    arhosekskymodelstate_free(hw);

    const Rgb sunTint = sunTransmittance(sky.sunElevationDeg, turbidity);
    const double sunDiscRad = 0.255 * kDegToRad * std::max(0.05, sky.sunDiscScale);
    const Rgb discColor{40.0 * sunTint.r, 40.0 * sunTint.g, 40.0 * sunTint.b};

    // Lens flare cook (sky_render.cpp cookLensFlare, verbatim transcription -- the per-pixel
    // ghost/halo/starburst evaluation lives in texture_sky.comp). flareOn false = the shader's
    // flare block never touches a byte, exactly the CPU's default-off path.
    bool flareOn = false;
    double fsx = 0.0, fsy = 0.0, fdiag = 1.0, fhaloR = 0.0, fhaloSigma = 1.0;
    double fburstLen = 1.0, fburstRot = 0.0, fvis = 0.0;
    Rgb fhue{1.0, 1.0, 1.0};
    if (sky.enableSun && sky.enableLensFlare) {
        const double strength = clamp01(sky.flareStrength);
        double px = 0.0, py = 0.0;
        if (strength > 0.0 && cam.project(sunDir, px, py)) {
            const double elFade = smoothstepD(-0.5, 2.0, sky.sunElevationDeg);
            const double fw = static_cast<double>(w), fh = static_cast<double>(h);
            const double diag = std::hypot(fw, fh);
            const double outX = std::max({0.0, -px, px - fw});
            const double outY = std::max({0.0, -py, py - fh});
            const double frameFade =
                1.0 - smoothstepD(0.0, 0.25 * diag, std::hypot(outX, outY));
            const double tintMean = (sunTint.r + sunTint.g + sunTint.b) / 3.0;
            const double hazeFade = 1.0 - 0.65 * haze01;
            const double vis = strength * elFade * frameFade * hazeFade * tintMean;
            if (vis > 1e-5) {
                flareOn = true;
                fvis = vis;
                fsx = px;
                fsy = py;
                fdiag = diag;
                const double axis = std::hypot(px - 0.5 * fw, py - 0.5 * fh);
                fhaloR = 0.45 * axis + 0.12 * diag;
                fhaloSigma = 0.045 * diag;
                fburstLen = 0.16 * diag;
                fburstRot = core::texture::hashToUnit(
                                core::texture::subSeed(p.seed, kSeedFlare)) *
                            (2.0 * kPi / 6.0);
                const double peak = std::max({sunTint.r, sunTint.g, sunTint.b, 1e-6});
                fhue = {sunTint.r / peak, sunTint.g / peak, sunTint.b / peak};
            }
        }
    }

    const double windRad = sky.windDirectionDeg * kDegToRad;
    const double windStrength = clamp01(sky.windStrength);
    const double visibilityM = 160000.0 / (1.0 + 2.2 * (turbidity - 1.0));

    // Deck shade colours + the twilight relight (sky_render.cpp, verbatim).
    const double sunUp =
        clamp01(std::sin(std::max(0.0, sky.sunElevationDeg) * kDegToRad) * 3.0);
    const double tintMax = std::max({sunTint.r, sunTint.g, sunTint.b, 1e-6});
    Rgb litBase = core::texture::mixRgb({1.12, 1.10, 1.06},
                                        {1.12 * sunTint.r / tintMax, 1.12 * sunTint.g / tintMax,
                                         1.12 * sunTint.b / tintMax},
                                        0.55);
    Rgb ambBase = core::texture::mixRgb({0.16, 0.17, 0.22}, {0.34, 0.40, 0.52}, sunUp);
    if (twilightActive) {
        const Rgb skyZen = scatterDisplay({0.0, 0.0, 1.0});
        const Rgb skyToward =
            scatterDisplay(core::texture::skyNormalize({sunHoriz.x, sunHoriz.y, 0.5}));
        const Rgb skyAway =
            scatterDisplay(core::texture::skyNormalize({-sunHoriz.x, -sunHoriz.y, 0.5}));
        const Rgb skyLight{0.5 * skyZen.r + 0.3 * skyToward.r + 0.2 * skyAway.r,
                           0.5 * skyZen.g + 0.3 * skyToward.g + 0.2 * skyAway.g,
                           0.5 * skyZen.b + 0.3 * skyToward.b + 0.2 * skyAway.b};
        const double nl = moonOn ? moonUp * moonPhaseIllum : 0.0;
        const Rgb moonKey{nl * 0.105, nl * 0.113, nl * 0.144};
        const Rgb litTwi{6.0 * skyLight.r + moonKey.r, 6.0 * skyLight.g + moonKey.g,
                         6.0 * skyLight.b + moonKey.b};
        const Rgb ambTwi{2.6 * skyLight.r + 0.40 * moonKey.r, 2.6 * skyLight.g + 0.44 * moonKey.g,
                         2.6 * skyLight.b + 0.62 * moonKey.b};
        litBase = core::texture::mixRgb(litBase, litTwi, twiW);
        ambBase = core::texture::mixRgb(ambBase, ambTwi, twiW);
    }

    // Cook the enabled decks, far to near (sky_render.cpp, verbatim).
    struct CookedDeck {
        CloudTypeSpec spec;
        CloudType type;
        double altitudeM;
        double featureM;
        double coverage;
        double scaleFactor;
        std::uint64_t seed;
    };
    std::vector<CookedDeck> cooked;
    if (sky.enableClouds) {
        cooked.reserve(sky.cloudLayers.size());
        const std::uint64_t cloudSeed = core::texture::subSeed(p.seed, kSeedClouds);
        for (std::size_t i = 0; i < sky.cloudLayers.size(); ++i) {
            const core::texture::CloudLayerParams& layer = sky.cloudLayers[i];
            if (!layer.enabled) continue;
            CookedDeck d;
            d.spec = cloudTypeSpec(layer.type);
            d.type = layer.type;
            d.altitudeM = layer.altitudeM > 0.0 ? layer.altitudeM : d.spec.altitudeM;
            d.scaleFactor = p.scale * sky.cloudScale * std::max(0.05, layer.scaleBias);
            d.featureM = std::max(10.0, d.spec.featureM * d.scaleFactor);
            d.coverage = clamp01(sky.cloudCoverage * d.spec.covFactor *
                                 std::max(0.0, layer.coverageBias));
            d.seed = core::texture::subSeed(cloudSeed, i + 1);
            cooked.push_back(d);
        }
        std::stable_sort(cooked.begin(), cooked.end(),
                         [](const CookedDeck& a, const CookedDeck& b) {
                             return a.altitudeM > b.altitudeM;
                         });
    }
    if (cooked.size() > kMaxDecks) return out;  // beyond the shader's table: CPU serves

    out.decks.reserve(cooked.size());
    for (const CookedDeck& d : cooked) {
        DeckGpu g{};
        g.a[0] = static_cast<float>(d.altitudeM);
        g.a[1] = static_cast<float>(d.featureM);
        g.a[2] = static_cast<float>(d.coverage);
        g.a[3] = static_cast<float>(d.spec.opacity);
        g.b[0] = static_cast<float>(d.spec.softness);
        g.b[1] = static_cast<float>(d.spec.worleyMix);
        g.b[2] = static_cast<float>(d.spec.baseGrey);
        g.b[3] = static_cast<float>(d.spec.warp);
        g.c[0] = static_cast<float>(d.spec.octaves);
        g.c[1] = static_cast<float>(d.spec.detail);
        g.c[2] = static_cast<float>(0.11 * d.spec.detail / 0.5);
        g.c[3] = static_cast<float>(1.0 + d.spec.shear * windStrength * 2.0);
        // 2D sub-stream seeds -- exact integer math shared with noise.hpp.
        const std::uint64_t warpSeed = core::texture::subSeed(d.seed, 0x77);
        putSeed(&g.s0[0], d.seed);
        putSeed(&g.s0[2], core::texture::subSeed(warpSeed, 0x57415250u));
        putSeed(&g.s1[0], core::texture::subSeed(warpSeed, 0x57415251u));
        putSeed(&g.s1[2], core::texture::subSeed(d.seed, 0xED0E));
        putSeed(&g.s2[0], core::texture::subSeed(d.seed, 0xCE11));
        const bool vol = sky.volumetricClouds && core::texture::cloudTypeIsVolumetric(d.type);
        g.f[0] = vol ? 1.0f : 0.0f;
        if (vol) {
            const core::texture::CloudVolumeSpec vs = core::texture::cloudVolumeSpec(
                d.type, d.altitudeM, d.featureM, d.coverage, d.scaleFactor);
            // Volumetric sub-stream seeds (cloud_volume.cpp's frozen tags).
            putSeed(&g.s2[2], core::texture::subSeed(d.seed, 0x564c'3d00));  // body
            putSeed(&g.s3[0], core::texture::subSeed(d.seed, 0x564c'5730));  // warp x
            putSeed(&g.s3[2], core::texture::subSeed(d.seed, 0x564c'5731));  // warp y
            putSeed(&g.s4[0], core::texture::subSeed(d.seed, 0x564c'5732));  // warp z
            putSeed(&g.s4[2], core::texture::subSeed(d.seed, 0x564c'ce11));  // puff
            putSeed(&g.s5[0], core::texture::subSeed(d.seed, 0x564c'ed0e));  // erode
            g.v0[0] = static_cast<float>(vs.baseM);
            g.v0[1] = static_cast<float>(vs.thicknessM);
            g.v0[2] = static_cast<float>(vs.featureM);
            g.v0[3] = static_cast<float>(vs.coverage);
            g.v1[0] = static_cast<float>(vs.extinction);
            g.v1[1] = static_cast<float>(vs.worleyMix);
            g.v1[2] = static_cast<float>(vs.erosion);
            g.v1[3] = static_cast<float>(vs.warp);
            g.v2[0] = static_cast<float>(vs.anvil);
            g.v2[1] = static_cast<float>(vs.baseSoft);
            g.v2[2] = static_cast<float>(vs.topSoft);
            g.v2[3] = static_cast<float>(1.0 + vs.shear * windStrength * 2.0);
            g.v3[0] = static_cast<float>(vs.hgG);
            g.v3[1] = static_cast<float>(vs.powder);
            g.v3[2] = static_cast<float>(vs.primarySteps);
            g.v3[3] = static_cast<float>(vs.lightSteps);
            g.v4[0] = static_cast<float>(vs.bodyOctaves);
        }
        out.decks.push_back(g);
    }

    // ---- assemble the header --------------------------------------------------------------
    SkyHeadGpu& hd = out.head;
    hd.frame[0] = static_cast<std::int32_t>(w);
    hd.frame[1] = static_cast<std::int32_t>(h);
    hd.frame[2] = static_cast<std::int32_t>(win.x);
    hd.frame[3] = static_cast<std::int32_t>(win.y);
    hd.win[0] = static_cast<std::int32_t>(win.w);
    hd.win[1] = static_cast<std::int32_t>(win.h);
    hd.win[2] = static_cast<std::int32_t>(out.decks.size());
    hd.win[3] = static_cast<std::int32_t>(projStars.size());
    hd.flags[0] = sky.enableDome ? 1 : 0;
    hd.flags[1] = sky.enableSun ? 1 : 0;
    hd.flags[2] = sky.enableHaze ? 1 : 0;
    hd.flags[3] = moonOn ? 1 : 0;
    hd.flags2[0] = twilightActive ? 1 : 0;
    hd.flags2[1] = starCols;
    hd.flags2[2] = starRows;
    hd.flags2[3] = twilightActive ? atmo.viewSteps : 0;
    putVec(hd.camRight, cam.right, cam.halfTanX);
    putVec(hd.camUp, cam.up, cam.halfTanY);
    putVec(hd.camForward, cam.forward, cam.shiftY);
    hd.camMisc[0] = static_cast<float>(cam.invW);
    hd.camMisc[1] = static_cast<float>(cam.invH);
    hd.camMisc[2] = static_cast<float>(exposureScale);
    hd.camMisc[3] = static_cast<float>(scatterScale);
    putVec(hd.sunDir, sunDir, sunDiscRad);
    putVec(hd.sunHoriz, sunHoriz, std::cos(windRad));
    putRgb(hd.sunTint, sunTint, std::sin(windRad));
    putRgb(hd.discColor, discColor, haze01);
    putRgb(hd.litBase, litBase, visibilityM);
    putRgb(hd.ambBase, ambBase, starCell);
    hd.night[0] = static_cast<float>(night01);
    hd.night[1] = static_cast<float>(twiW);
    hd.night[2] = static_cast<float>(starVis);
    hd.night[3] = static_cast<float>(nightFloor);
    putVec(hd.moonDir, moonDir, moonDiscRad);
    putVec(hd.moonToObs, moonToObs, moonPhaseIllum);
    putVec(hd.moonRight, moonDiscRight, moonUp);
    putVec(hd.moonUp, moonDiscUp, moonLibLonDeg);
    putVec(hd.moonNorth, moonNorth, 0.0);
    putVec(hd.moonLight, moonLightDir, 0.0);
    hd.atmo[0] = static_cast<float>(atmo.betaMieScatter);
    hd.atmo[1] = static_cast<float>(atmo.betaMieExtinct);
    hd.atmo[2] = static_cast<float>(atmo.mieG);
    hd.atmo[3] = 0.0f;
    hd.flare0[0] = static_cast<float>(fsx);
    hd.flare0[1] = static_cast<float>(fsy);
    hd.flare0[2] = static_cast<float>(0.5 * w);
    hd.flare0[3] = static_cast<float>(0.5 * h);
    hd.flare1[0] = static_cast<float>(fdiag);
    hd.flare1[1] = static_cast<float>(fhaloR);
    hd.flare1[2] = static_cast<float>(fhaloSigma);
    hd.flare1[3] = static_cast<float>(fburstLen);
    hd.flare2[0] = static_cast<float>(fburstRot);
    hd.flare2[1] = static_cast<float>(fvis);
    hd.flare2[2] = flareOn ? 1.0f : 0.0f;
    hd.flare2[3] = 0.0f;
    putRgb(hd.flareHue, fhue, 0.0);

    if (twilightActive) {
        out.atmoTab.reserve(atmo.odRayleigh.size() + atmo.odMie.size() + atmo.msTable.size());
        out.atmoTab.insert(out.atmoTab.end(), atmo.odRayleigh.begin(), atmo.odRayleigh.end());
        out.atmoTab.insert(out.atmoTab.end(), atmo.odMie.begin(), atmo.odMie.end());
        out.atmoTab.insert(out.atmoTab.end(), atmo.msTable.begin(), atmo.msTable.end());
    }
    out.needsMoonTables = moonOn;
    out.supported = true;
    return out;
}

// ---------------------------------------------------------------------------------------------
// Paper cook -- transcribed from renderPaper's preamble (paper_render.cpp).
// ---------------------------------------------------------------------------------------------

// Element sub-seed tags (paper_render.cpp; FROZEN, part of the paper golden contract).
constexpr std::uint64_t kSeedTooth = 0x504150'10;
constexpr std::uint64_t kSeedFiber = 0x504150'11;
constexpr std::uint64_t kSeedLaidJitter = 0x504150'12;
constexpr std::uint64_t kSeedFelt = 0x504150'13;
constexpr std::uint64_t kSeedDeckle = 0x504150'14;
constexpr std::uint64_t kSeedPrint = 0x504150'15;

PaperGpu cookPaper(const TextureParams& p, const PaperParams& paper, std::uint32_t w,
                   std::uint32_t h, const ResolvedWindow& win) {
    PaperGpu g{};
    const double scale = std::max(1e-3, p.scale);
    const double angle = paper.grainAngleDeg * kPi / 180.0;

    g.frame[0] = static_cast<std::int32_t>(w);
    g.frame[1] = static_cast<std::int32_t>(h);
    g.frame[2] = static_cast<std::int32_t>(win.x);
    g.frame[3] = static_cast<std::int32_t>(win.y);
    g.win[0] = static_cast<std::int32_t>(win.w);
    g.win[1] = static_cast<std::int32_t>(win.h);
    g.win[2] = static_cast<std::int32_t>(paper.kind);
    g.win[3] = (paper.deckleEdge ? 1 : 0) | (paper.printTooth ? 2 : 0);

    g.grain[0] = static_cast<float>(std::max(0.75, 4.5 * scale));
    g.grain[1] = static_cast<float>(1.0 + 3.0 * clamp01(paper.grainAnisotropy));
    g.grain[2] = static_cast<float>(std::cos(angle));
    g.grain[3] = static_cast<float>(std::sin(angle));
    g.fiber[0] = static_cast<float>(std::max(0.75, 2.0 * scale));
    g.fiber[1] = 0.6f;
    g.fiber[2] = static_cast<float>(angle + kPi / 2.0);
    g.fiber[3] = static_cast<float>(0.35 * clamp01(paper.fiber));
    g.laid[0] = static_cast<float>(std::max(1.0, paper.laidSpacing * scale));
    g.laid[1] = static_cast<float>(std::max(2.0, paper.chainSpacing * scale));
    g.laid[2] = static_cast<float>(clamp01(paper.laidDepth));
    g.laid[3] = static_cast<float>(std::max(2.0, 12.0 * scale));

    const double el = clamp01(paper.lightElevationDeg / 90.0) * kPi / 2.0;
    const double az = paper.lightAzimuthDeg * kPi / 180.0;
    const double lx = std::sin(az) * std::cos(el);
    const double ly = -std::cos(az) * std::cos(el);
    const double lz = std::sin(el);
    g.light[0] = static_cast<float>(lx);
    g.light[1] = static_cast<float>(ly);
    g.light[2] = static_cast<float>(lz);
    g.light[3] = static_cast<float>(2.6 * clamp01(paper.roughness));

    const double sigma = 0.7 * clamp01(paper.matte);
    const double sigma2 = sigma * sigma;
    g.oren[0] = static_cast<float>(1.0 - 0.5 * sigma2 / (sigma2 + 0.33));
    g.oren[1] = static_cast<float>(0.45 * sigma2 / (sigma2 + 0.09));
    g.oren[2] = static_cast<float>(clamp01(paper.sheen));
    g.oren[3] = static_cast<float>(std::max(1.0, 1.5 * scale));  // printGrain

    const double hxv = lx, hyv = ly, hzv = lz + 1.0;
    const double hlen = std::sqrt(hxv * hxv + hyv * hyv + hzv * hzv);
    g.halfv[0] = static_cast<float>(hxv / hlen);
    g.halfv[1] = static_cast<float>(hyv / hlen);
    g.halfv[2] = static_cast<float>(hzv / hlen);
    g.halfv[3] = static_cast<float>(paper.printAmount);

    const double band = std::max(1.0, paper.deckleInset * std::min(w, h));
    g.deckle[0] = static_cast<float>(band);
    g.deckle[1] = static_cast<float>(std::max(3.0, band * 0.5));
    g.deckle[2] = static_cast<float>(paper.deckleAmount);
    g.deckle[3] = 0.0f;

    putSeed(&g.seedA[0], core::texture::subSeed(p.seed, kSeedTooth));
    putSeed(&g.seedA[2], core::texture::subSeed(p.seed, kSeedFiber));
    putSeed(&g.seedB[0], core::texture::subSeed(p.seed, kSeedLaidJitter));
    putSeed(&g.seedB[2], core::texture::subSeed(p.seed, kSeedFelt));
    putSeed(&g.seedC[0], core::texture::subSeed(p.seed, kSeedDeckle));
    putSeed(&g.seedC[2], core::texture::subSeed(p.seed, kSeedPrint));
    return g;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The Vulkan plumbing (GpuCompositor's VMA pattern + banded dispatches).
// ---------------------------------------------------------------------------------------------

struct TextureGpu::Impl {
    std::shared_ptr<VulkanContext> ctx;  // the process-wide shared device (S60-alpha)
    VkCommandPool pool = VK_NULL_HANDLE; // OURS: pools are externally synchronized
    VmaAllocator allocator = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkShaderModule skyShader = VK_NULL_HANDLE;
    VkShaderModule paperShader = VK_NULL_HANDLE;
    VkPipeline skyPipeline = VK_NULL_HANDLE;
    VkPipeline paperPipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    // The rgba32f target + its readback staging, grown on demand.
    std::uint32_t imgW = 0, imgH = 0;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation imageAlloc = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    void* stagingPtr = nullptr;
    VkDeviceSize stagingCap = 0;

    // Host-visible SSBOs the shaders read directly (small; the moon tables upload once).
    struct Buf {
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        void* ptr = nullptr;
        VkDeviceSize cap = 0;
    };
    Buf params, atmo, stars, bins, moonAlb, moonElev;
    bool moonUploaded = false;

    void destroyBuf(Buf& b) {
        if (b.buf != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, b.buf, b.alloc);
        b = {};
    }

    bool ensureBuf(Buf& b, VkDeviceSize bytes) {
        if (b.cap >= bytes && b.buf != VK_NULL_HANDLE) return true;
        destroyBuf(b);
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = std::max<VkDeviceSize>(bytes, 64),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo aci{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(allocator, &bci, &aci, &b.buf, &b.alloc, &info) != VK_SUCCESS)
            return false;
        b.ptr = info.pMappedData;
        b.cap = bci.size;
        return true;
    }

    bool uploadBuf(Buf& b, const void* data, VkDeviceSize bytes) {
        if (!ensureBuf(b, bytes)) return false;
        if (bytes > 0 && data != nullptr) {
            std::memcpy(b.ptr, data, static_cast<std::size_t>(bytes));
            vmaFlushAllocation(allocator, b.alloc, 0, VK_WHOLE_SIZE);
        }
        return true;
    }

    void destroySized() {
        if (imageView) vkDestroyImageView(ctx->device(), imageView, nullptr);
        if (image) vmaDestroyImage(allocator, image, imageAlloc);
        imageView = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        imageAlloc = VK_NULL_HANDLE;
        imgW = imgH = 0;
    }

    bool ensureTarget(std::uint32_t w, std::uint32_t h) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4 * sizeof(float);
        if (imgW != w || imgH != h || image == VK_NULL_HANDLE) {
            ctx->waitIdle();
            destroySized();
            const VkImageCreateInfo ici{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = kFloatFormat,
                .extent = {w, h, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            const VmaAllocationCreateInfo iac{.usage = VMA_MEMORY_USAGE_AUTO};
            if (vmaCreateImage(allocator, &ici, &iac, &image, &imageAlloc, nullptr) != VK_SUCCESS)
                return false;
            const VkImageViewCreateInfo vci{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = kFloatFormat,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            if (vkCreateImageView(ctx->device(), &vci, nullptr, &imageView) != VK_SUCCESS)
                return false;
            imgW = w;
            imgH = h;
        }
        if (stagingCap < bytes || staging == VK_NULL_HANDLE) {
            if (staging) vmaDestroyBuffer(allocator, staging, stagingAlloc);
            staging = VK_NULL_HANDLE;
            const VkBufferCreateInfo bci{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = bytes,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            };
            const VmaAllocationCreateInfo aci{
                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO,
            };
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(allocator, &bci, &aci, &staging, &stagingAlloc, &info) !=
                VK_SUCCESS)
                return false;
            stagingPtr = info.pMappedData;
            stagingCap = bci.size;
        }
        return true;
    }

    bool submitAndWait() {
        const VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .commandBufferCount = 1,
                              .pCommandBuffers = &cmd};
        vkResetFences(ctx->device(), 1, &fence);
        if (ctx->submit(si, fence) != VK_SUCCESS) return false;
        return vkWaitForFences(ctx->device(), 1, &fence, VK_TRUE, 60'000'000'000ull) ==
               VK_SUCCESS;
    }

    ~Impl() {
        if (!ctx) return;
        const VkDevice dev = ctx->device();
        ctx->waitIdle();
        destroySized();
        if (staging) vmaDestroyBuffer(allocator, staging, stagingAlloc);
        for (Buf* b : {&params, &atmo, &stars, &bins, &moonAlb, &moonElev}) destroyBuf(*b);
        if (fence) vkDestroyFence(dev, fence, nullptr);
        if (cmd) vkFreeCommandBuffers(dev, pool, 1, &cmd);
        if (descPool) vkDestroyDescriptorPool(dev, descPool, nullptr);
        if (skyPipeline) vkDestroyPipeline(dev, skyPipeline, nullptr);
        if (paperPipeline) vkDestroyPipeline(dev, paperPipeline, nullptr);
        if (skyShader) vkDestroyShaderModule(dev, skyShader, nullptr);
        if (paperShader) vkDestroyShaderModule(dev, paperShader, nullptr);
        if (pipeLayout) vkDestroyPipelineLayout(dev, pipeLayout, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
        if (pool) vkDestroyCommandPool(dev, pool, nullptr);
        if (allocator) vmaDestroyAllocator(allocator);
    }

    bool init(bool enableValidation, std::string& error) {
        ctx = VulkanContext::shared(enableValidation, error);
        if (!ctx) return false;
        pool = ctx->createCommandPool(error);
        if (pool == VK_NULL_HANDLE) return false;
        const VkDevice dev = ctx->device();

        // rgba32f storage images are core-required, but verify and bail to the CPU path if
        // absent (the GpuCompositor precedent).
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(ctx->physicalDevice(), kFloatFormat, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            error = "device cannot use an rgba32f storage image";
            return false;
        }

        const VmaAllocatorCreateInfo aci{
            .physicalDevice = ctx->physicalDevice(),
            .device = dev,
            .instance = ctx->instance(),
            // The version VMA may use must not exceed what the instance was created with NOR
            // what the device supports -- caps().apiVersion is exactly min(instance, device).
            // A hard-coded 1.2 here would have VMA reach for entry points a 1.0 device lacks.
            .vulkanApiVersion = ctx->caps().apiVersion,
        };
        if (vmaCreateAllocator(&aci, &allocator) != VK_SUCCESS) {
            error = "vmaCreateAllocator failed";
            return false;
        }

        // Bindings: 0 params SSBO, 1 the rgba32f target, 2 atmosphere tables, 3 stars,
        // 4 star bins, 5 moon albedo, 6 moon elevation. Paper binds dummies for 2..6.
        // Vulkan 1.0 guarantees only FOUR storage buffers per stage and this lane binds SIX, so
        // ask before assuming; a refusal here hands the work to the CPU lane (S60-alpha).
        if (!ctx->caps().fitsStorageBuffers(kStorageBuffers) ||
            !ctx->caps().fitsStorageImages(1)) {
            error = "device allows only " +
                    std::to_string(ctx->caps().limits.maxPerStageDescriptorStorageBuffers) +
                    " storage buffers per stage; this lane needs " +
                    std::to_string(kStorageBuffers);
            return false;
        }
        VkDescriptorSetLayoutBinding bindings[kBindings]{};
        for (std::uint32_t i = 0; i < kBindings; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        const VkDescriptorSetLayoutCreateInfo slci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = kBindings,
            .pBindings = bindings,
        };
        if (vkCreateDescriptorSetLayout(dev, &slci, nullptr, &setLayout) != VK_SUCCESS) {
            error = "descriptor set layout creation failed";
            return false;
        }
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(std::uint32_t)};
        const VkPipelineLayoutCreateInfo plci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &setLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        };
        if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipeLayout) != VK_SUCCESS) {
            error = "pipeline layout creation failed";
            return false;
        }
        const auto makeShader = [&](const std::uint32_t* code, std::size_t size,
                                    VkShaderModule& mod) {
            const VkShaderModuleCreateInfo smci{
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = size,
                .pCode = code,
            };
            return vkCreateShaderModule(dev, &smci, nullptr, &mod) == VK_SUCCESS;
        };
        if (!makeShader(shaders::texture_sky_comp, shaders::texture_sky_comp_size, skyShader) ||
            !makeShader(shaders::texture_paper_comp, shaders::texture_paper_comp_size,
                        paperShader)) {
            error = "shader module creation failed";
            return false;
        }
        const auto makePipeline = [&](VkShaderModule mod, VkPipeline& pipe) {
            const VkComputePipelineCreateInfo cpci{
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                          .module = mod,
                          .pName = "main"},
                .layout = pipeLayout,
            };
            return vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe) ==
                   VK_SUCCESS;
        };
        if (!makePipeline(skyShader, skyPipeline) || !makePipeline(paperShader, paperPipeline)) {
            error = "compute pipeline creation failed";
            return false;
        }
        const VkDescriptorPoolSize poolSizes[2] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        };
        const VkDescriptorPoolCreateInfo dpci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1,
            .poolSizeCount = 2,
            .pPoolSizes = poolSizes,
        };
        if (vkCreateDescriptorPool(dev, &dpci, nullptr, &descPool) != VK_SUCCESS) {
            error = "descriptor pool creation failed";
            return false;
        }
        const VkDescriptorSetAllocateInfo dsai{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = descPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &setLayout,
        };
        if (vkAllocateDescriptorSets(dev, &dsai, &descSet) != VK_SUCCESS) {
            error = "descriptor set allocation failed";
            return false;
        }
        const VkCommandBufferAllocateInfo cbai{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(dev, &cbai, &cmd) != VK_SUCCESS) {
            error = "command buffer allocation failed";
            return false;
        }
        const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) {
            error = "fence creation failed";
            return false;
        }
        return true;
    }
};

TextureGpu::TextureGpu() : m_impl(std::make_unique<Impl>()) {}
TextureGpu::~TextureGpu() = default;

std::unique_ptr<TextureGpu> TextureGpu::create(bool enableValidation, std::string& error) {
    // CPU-only mode (render/gpu_policy.hpp, S60-b item 14). The generators' CPU lanes in
    // core/texture are the golden renders this one is regenerated against, so declining here
    // leaves the sky and the paper byte-identical and merely slower.
    if (!computeLaneAllowed("texture", error)) return nullptr;
    auto gpu = std::unique_ptr<TextureGpu>(new TextureGpu());
    if (!gpu->m_impl->init(enableValidation, error)) return nullptr;
    return gpu;
}

std::string TextureGpu::deviceName() const {
    return m_impl->ctx ? m_impl->ctx->deviceName() : std::string{};
}

bool TextureGpu::render(const TextureParams& params, std::uint32_t w, std::uint32_t h,
                        const TextureWindow& window, TextureRenderProgress* progress,
                        TextureRenderResult& out) {
    Impl& im = *m_impl;
    if (!im.ctx || w == 0 || h == 0) return false;

    const ResolvedWindow win = core::texture::resolveWindow(window, w, h);
    if (win.w == 0 || win.h == 0) return false;  // degenerate: the (trivial) CPU path serves
    const VkDeviceSize imageBytes = static_cast<VkDeviceSize>(win.w) * win.h * 4 * sizeof(float);
    // Three caps: OUR policy (kMaxImageBytes), the device's storage-buffer range, and the
    // device's max image dimension -- the target is a real VkImage, and Vulkan 1.0 guarantees
    // only 4096 (S60-alpha).
    if (imageBytes > kMaxImageBytes || !im.ctx->caps().fitsStorageBufferRange(imageBytes) ||
        !im.ctx->caps().fitsImage(win.w, win.h))
        return false;

    // ---- cook + upload the per-render data ------------------------------------------------
    const bool isSky = params.generator == core::texture::Generator::Sky &&
                       std::holds_alternative<SkyParams>(params.spec);
    const bool isPaper = params.generator == core::texture::Generator::Paper &&
                         std::holds_alternative<PaperParams>(params.spec);
    if (!isSky && !isPaper) return false;  // Grass (and anything newer) stays on the CPU lane

    SkyCook sky;
    PaperGpu paper{};
    if (isSky) {
        sky = cookSky(params, std::get<SkyParams>(params.spec), w, h, win);
        if (!sky.supported) return false;
        std::vector<std::uint8_t> blob(sizeof(SkyHeadGpu) + sky.decks.size() * sizeof(DeckGpu));
        std::memcpy(blob.data(), &sky.head, sizeof(SkyHeadGpu));
        if (!sky.decks.empty())
            std::memcpy(blob.data() + sizeof(SkyHeadGpu), sky.decks.data(),
                        sky.decks.size() * sizeof(DeckGpu));
        if (!im.uploadBuf(im.params, blob.data(), blob.size())) return false;
        if (!im.uploadBuf(im.atmo, sky.atmoTab.empty() ? nullptr : sky.atmoTab.data(),
                          std::max<VkDeviceSize>(sky.atmoTab.size() * sizeof(float), 4)))
            return false;
        if (!im.uploadBuf(im.stars, sky.stars.empty() ? nullptr : sky.stars.data(),
                          std::max<VkDeviceSize>(sky.stars.size() * sizeof(float), 4)))
            return false;
        if (!im.uploadBuf(im.bins, sky.bins.empty() ? nullptr : sky.bins.data(),
                          std::max<VkDeviceSize>(sky.bins.size() * sizeof(std::uint32_t), 4)))
            return false;
        if (sky.needsMoonTables && !im.moonUploaded) {
            // The static LRO/LOLA tables, packed for the shader's uint-word readers.
            const std::size_t texels = static_cast<std::size_t>(
                core::texture::kMoonTextureWidth * core::texture::kMoonTextureHeight);
            if (!im.uploadBuf(im.moonAlb, core::texture::moonAlbedo(), texels)) return false;
            if (!im.uploadBuf(im.moonElev, core::texture::moonElevation(),
                              texels * sizeof(std::int16_t)))
                return false;
            im.moonUploaded = true;
        } else if (!im.moonUploaded) {
            if (!im.ensureBuf(im.moonAlb, 4) || !im.ensureBuf(im.moonElev, 4)) return false;
        }
    } else {
        paper = cookPaper(params, std::get<PaperParams>(params.spec), w, h, win);
        if (!im.uploadBuf(im.params, &paper, sizeof(paper))) return false;
        // Keep every binding valid (never read by the paper kernel).
        if (!im.ensureBuf(im.atmo, 4) || !im.ensureBuf(im.stars, 4) ||
            !im.ensureBuf(im.bins, 4))
            return false;
        if (!im.moonUploaded &&
            (!im.ensureBuf(im.moonAlb, 4) || !im.ensureBuf(im.moonElev, 4)))
            return false;
    }

    if (!im.ensureTarget(win.w, win.h)) return false;

    // ---- descriptors -----------------------------------------------------------------------
    const VkDevice dev = im.ctx->device();
    const VkDescriptorImageInfo imgInfo{VK_NULL_HANDLE, im.imageView, VK_IMAGE_LAYOUT_GENERAL};
    const VkBuffer bufs[6] = {im.params.buf, im.atmo.buf,    im.stars.buf,
                              im.bins.buf,   im.moonAlb.buf, im.moonElev.buf};
    VkDescriptorBufferInfo bufInfos[6];
    VkWriteDescriptorSet writes[kBindings];
    for (std::uint32_t i = 0; i < 6; ++i) bufInfos[i] = {bufs[i], 0, VK_WHOLE_SIZE};
    const std::uint32_t bufBinding[6] = {0, 2, 3, 4, 5, 6};
    for (std::uint32_t i = 0; i < 6; ++i)
        writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = im.descSet,
                     .dstBinding = bufBinding[i],
                     .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     .pBufferInfo = &bufInfos[i]};
    writes[6] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = im.descSet,
                 .dstBinding = 1,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .pImageInfo = &imgInfo};
    vkUpdateDescriptorSets(dev, kBindings, writes, 0, nullptr);

    // ---- banded dispatches: progress + cancel at dispatch granularity ----------------------
    if (progress != nullptr)
        progress->rowsTotal.store(win.h, std::memory_order_relaxed);
    if (core::texture::renderCancelled(progress)) return true;  // empty result = cancelled

    const VkPipeline pipe = isSky ? im.skyPipeline : im.paperPipeline;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const std::uint32_t groupsX = (win.w + 7) / 8;
    bool first = true;
    for (std::uint32_t row = 0; row < win.h; row += kBandRows) {
        const std::uint32_t bandRows = std::min(kBandRows, win.h - row);
        vkResetCommandBuffer(im.cmd, 0);
        const VkCommandBufferBeginInfo cbi{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        if (vkBeginCommandBuffer(im.cmd, &cbi) != VK_SUCCESS) return false;
        if (first) {
            const VkImageMemoryBarrier toGeneral{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = im.image,
                .subresourceRange = range,
            };
            vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                 1, &toGeneral);
            first = false;
        }
        vkCmdBindPipeline(im.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(im.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.pipeLayout, 0, 1,
                                &im.descSet, 0, nullptr);
        vkCmdPushConstants(im.cmd, im.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(row),
                           &row);
        vkCmdDispatch(im.cmd, groupsX, (bandRows + 7) / 8, 1);
        if (vkEndCommandBuffer(im.cmd) != VK_SUCCESS) return false;
        if (!im.submitAndWait()) {
            if (progress != nullptr) progress->rowsDone.store(0, std::memory_order_relaxed);
            return false;  // device trouble: let the CPU lane take over cleanly
        }
        if (progress != nullptr)
            progress->rowsDone.fetch_add(bandRows, std::memory_order_relaxed);
        if (core::texture::renderCancelled(progress)) return true;  // empty = cancelled
    }

    // ---- readback ---------------------------------------------------------------------------
    vkResetCommandBuffer(im.cmd, 0);
    const VkCommandBufferBeginInfo cbi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(im.cmd, &cbi) != VK_SUCCESS) return false;
    const VkImageMemoryBarrier toSrc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = im.image,
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
    const VkBufferImageCopy copy{
        .bufferOffset = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {win.w, win.h, 1},
    };
    vkCmdCopyImageToBuffer(im.cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.staging, 1,
                           &copy);
    const VkBufferMemoryBarrier toHost{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = im.staging,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &toHost, 0, nullptr);
    if (vkEndCommandBuffer(im.cmd) != VK_SUCCESS) return false;
    if (!im.submitAndWait()) {
        if (progress != nullptr) progress->rowsDone.store(0, std::memory_order_relaxed);
        return false;
    }
    vmaInvalidateAllocation(im.allocator, im.stagingAlloc, 0, VK_WHOLE_SIZE);

    const float* data = static_cast<const float*>(im.stagingPtr);
    if (isSky) {
        common::ImageF img(win.w, win.h);
        std::memcpy(img.rgba.data(), data, static_cast<std::size_t>(imageBytes));
        out.imageF = std::move(img);
    } else {
        // Quantise (shade, alpha) with the CPU lane's own q() formula (paper_render.cpp) so
        // the byte mapping is shared, not re-derived.
        const PaperParams& pp = std::get<PaperParams>(params.spec);
        common::Image img(win.w, win.h);
        const std::size_t n = static_cast<std::size_t>(win.w) * win.h;
        for (std::size_t i = 0; i < n; ++i) {
            const double shade = data[i * 4 + 0];
            const double alpha = data[i * 4 + 1];
            const auto q = [&](float tintCh) {
                return static_cast<std::uint8_t>(
                    std::clamp(tintCh * shade * 255.0 + 0.5, 0.0, 255.0));
            };
            img.rgba[i * 4 + 0] = q(pp.tint.r);
            img.rgba[i * 4 + 1] = q(pp.tint.g);
            img.rgba[i * 4 + 2] = q(pp.tint.b);
            img.rgba[i * 4 + 3] =
                static_cast<std::uint8_t>(std::clamp(alpha * 255.0 + 0.5, 0.0, 255.0));
        }
        out.image8 = std::move(img);
    }
    return true;
}

}  // namespace mosaic::render
