// Volumetric cloud texture generator -- procedural, CPU-first, ML-free. Clean-room from PUBLISHED
// sources only (no engine code copied): Schneider & Vos "Real-Time Volumetric Cloudscapes of
// Horizon Zero Dawn" (SIGGRAPH 2015 / GPU Pro 7 / 2017 Nubis); Haggstrom MSc thesis 2018;
// Hillaire "Physically Based Sky, Atmosphere and Cloud Rendering" (SIGGRAPH 2016); Bauer "RDR2"
// (SIGGRAPH 2019). Physics: Beer-Lambert (1729/1852), Henyey-Greenstein phase (1941),
// Rayleigh(1871)/Mie(1908). Noise: classic Perlin (1985) + Worley (1996).
// No ML. No third-party engine source.
//
// ⚠ TWO STANDING CONSTRUCTION CONSTRAINTS -- deliberate, load-bearing, do not "improve" away:
//   1. the density field is IMPLICIT and procedural, evaluated per march sample. It is never
//      tessellated into a plurality of polygons, nor baked into any explicit geometry;
//   2. compositing is a single-medium Beer-Lambert march whose accumulated transmittance IS the
//      alpha. The deck is never mixed into the frame as a separately-rendered volume blended by
//      ray-intersection proportion against a globally-characterized scene image.
//
// The field is evaluated in WORLD METRES on the slab between the deck's base and top altitudes;
// the camera sits at the origin (ground) looking up (sky_camera.hpp). Each march sample is one
// density evaluation plus a short light-march toward the sun; the accumulated transmittance is
// the coverage (alpha) and the in-scattered radiance is composited by sky_render.cpp exactly like
// a 2D deck (same aerial-perspective fade, same straight-alpha over).

#include "core/texture/cloud_volume.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::texture {

namespace {

double clamp01(double v) noexcept {
    return std::clamp(v, 0.0, 1.0);
}

double smoothstep(double e0, double e1, double v) noexcept {
    if (e1 <= e0) return v >= e1 ? 1.0 : 0.0;
    const double t = clamp01((v - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

// Linear remap of v from [lo, hi] onto [a, b], clamped -- the Nubis density-shaping workhorse
// (carving coverage, nibbling edges). Degenerate ranges collapse to the low end.
double remap(double v, double lo, double hi, double a, double b) noexcept {
    if (hi <= lo) return a;
    return a + clamp01((v - lo) / (hi - lo)) * (b - a);
}

struct V3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

// Sub-seed tags (arbitrary, FROZEN: part of the golden contract). Distinct from the 2D lane's
// tags so a deck's two lanes never read correlated lattices (a deck only ever renders one lane,
// but keeping the streams independent makes the choice invisible to the field).
constexpr std::uint64_t kSeedWarpX = 0x564c'5730;  // "VL" warp x
constexpr std::uint64_t kSeedWarpY = 0x564c'5731;
constexpr std::uint64_t kSeedWarpZ = 0x564c'5732;
constexpr std::uint64_t kSeedBody = 0x564c'3d00;   // "VL" 3D body
constexpr std::uint64_t kSeedPuff = 0x564c'ce11;
constexpr std::uint64_t kSeedErode = 0x564c'ed0e;

// Single-scatter gain: real clouds are lit far more than one Beer-Lambert bounce predicts (the
// missing multiple scattering). A modest constant restores the punchy sunlit crown without a
// second march -- the standard real-time cheat (Nubis/Hillaire). Golden-pinned, tunable.
constexpr double kSunGain = 2.6;

// Slant step boost cap: primarySteps is sized for a vertical ray, but the in-slab path (and so
// dt) stretches as 1/dir.z toward the horizon, and a fixed count undersamples the density field
// there -- the source of the "wavy strips" banding (verified by experiment: 8x primary steps
// removed it; 8x LIGHT steps changed nothing). Scaling the count with the slab slant keeps dt
// constant in WORLD METRES until the cap; jitter (marchCloudVolume's jitter01) carries the rays
// past it. Cap 3 restores an 8x-reference-quality horizon at ~2.8x worst-case cost (a pitch-6
// frame; steep frames pay far less). Golden-pinned, like the step counts themselves.
constexpr double kSlantStepCap = 3.0;

// Height gradient: a rounded soft base, a tapered top, and -- for Cumulonimbus -- an anvil lobe
// that keeps/spreads density near the very top instead of tapering it. h is the fraction of the
// slab (0 base, 1 top).
double heightGradient(double h, const CloudVolumeSpec& s) noexcept {
    const double base = smoothstep(0.0, s.baseSoft, h);
    const double top = 1.0 - smoothstep(s.topSoft, 1.0, h);
    double g = base * top;
    if (s.anvil > 0.0) {
        const double anvilLobe =
            s.anvil * smoothstep(0.55, 0.75, h) * (1.0 - smoothstep(0.92, 1.0, h));
        g = std::max(g, anvilLobe * base);
    }
    return clamp01(g);
}

// The implicit density field at a world point (metres). Pure function of (seed, coords). `cheap`
// drops the two Worley terms and halves the octaves for the light-march (occlusion tolerates a
// coarse field -- the standard low-detail shadow sample). Returns ~[0, 1].
double cloudDensity(std::uint64_t seed, const CloudVolumeSpec& s, double wxM, double wyM,
                    double wzM, double windRad, double windStrength, bool cheap) {
    const double h = (wzM - s.baseM) / s.thicknessM;
    if (h < 0.0 || h > 1.0) return 0.0;  // outside the slab (the light march exits the top here)

    // Wind frame: rotate horizontal coords and stretch ALONG the wind (streaked heaps).
    const double ca = std::cos(windRad), sa = std::sin(windRad);
    const double along = wxM * sa + wyM * ca;
    const double across = wxM * ca - wyM * sa;
    const double stretch = 1.0 + s.shear * windStrength * 2.0;
    const V3 p{along / (s.featureM * stretch), across / s.featureM, wzM / s.featureM};

    // Domain warp (three decorrelated fBm channels) -- billowing deformation of the lookup.
    const FbmParams warpFbm{3, 2.0, 0.5};
    const double freq = 1.9;
    const auto warpCh = [&](std::uint64_t tag) {
        return fbm3(subSeed(seed, tag), p.x * freq, p.y * freq, p.z * freq, warpFbm);
    };
    const V3 w{p.x + s.warp * warpCh(kSeedWarpX), p.y + s.warp * warpCh(kSeedWarpY),
               p.z + s.warp * warpCh(kSeedWarpZ)};

    const int oct = cheap ? std::max(2, s.bodyOctaves / 2) : s.bodyOctaves;
    const double billow = 0.5 + 0.5 * fbm3(subSeed(seed, kSeedBody), w.x, w.y, w.z,
                                           FbmParams{oct, 2.0, 0.5});
    double baseShape = billow;
    if (!cheap && s.worleyMix > 0.0) {
        // Inverted F1 makes each Worley cell a rounded puff centre (cauliflower). Frequency BELOW
        // the fBm body so puffs read as broad heaps, not confetti.
        const double f1 = worley3(subSeed(seed, kSeedPuff), w.x * 0.9, w.y * 0.9, w.z * 0.9).f1;
        const double puff = clamp01(1.0 - f1 * 0.95);
        baseShape = clamp01(billow * (1.0 - s.worleyMix) + puff * s.worleyMix);
    }

    // Carve coverage (only the densest field survives at low coverage), then the height gradient.
    double d = remap(baseShape, 1.0 - s.coverage, 1.0, 0.0, 1.0) * heightGradient(h, s);
    if (d <= 0.0) return 0.0;

    // High-frequency Worley erosion nibbles wispy detail into the surface (edges + interior grain).
    if (!cheap && s.erosion > 0.0) {
        const double e = worley3(subSeed(seed, kSeedErode), w.x * 3.4, w.y * 3.4, w.z * 3.4).f1;
        d = remap(d, clamp01(e) * s.erosion, 1.0, 0.0, 1.0);
    }
    return clamp01(d);
}

// Henyey-Greenstein phase (1941), without the 1/4pi normaliser (the real-time convention -- the
// scene calibrates brightness through exposure/tonemap). g in (-1, 1); the denominator base is
// 1 + g^2 - 2 g cos, strictly positive for |g| < 1.
double henyeyGreenstein(double cosT, double g) noexcept {
    const double gg = g * g;
    const double denom = std::max(1e-4, 1.0 + gg - 2.0 * g * cosT);
    return (1.0 - gg) / (denom * std::sqrt(denom));
}

// Dual-lobe phase: a strong forward lobe (the silver lining on sunward edges) with a gentler back
// lobe so away-from-sun crowns still catch a rounded highlight instead of going flat.
double phaseFn(double cosT, double g) noexcept {
    return std::max(henyeyGreenstein(cosT, g), 0.7 * henyeyGreenstein(cosT, -0.15));
}

}  // namespace

bool cloudTypeIsVolumetric(CloudType t) noexcept {
    return t == CloudType::Cumulus || t == CloudType::Cumulonimbus;
}

CloudVolumeSpec cloudVolumeSpec(CloudType type, double baseM, double featureM, double coverage,
                                double scaleFactor) {
    CloudVolumeSpec s;
    s.baseM = baseM;
    s.featureM = std::max(10.0, featureM);
    s.coverage = clamp01(coverage);
    const double sf = std::max(0.05, scaleFactor);
    // Per-type shaping (public meteorological character; the numbers are artistic calibration,
    // golden-pinned like every other constant in the suite). thicknessM scales with the same
    // factor as featureM so a scaled cloud keeps its proportions.
    switch (type) {
        case CloudType::Cumulonimbus:
            s.thicknessM = 7000.0 * sf;
            s.extinction = 0.0042;
            s.worleyMix = 0.55;
            s.erosion = 0.40;
            s.warp = 0.55;
            s.anvil = 0.9;
            s.baseSoft = 0.12;
            s.topSoft = 0.70;
            s.shear = 0.5;
            s.hgG = 0.32;
            s.powder = 0.75;
            s.primarySteps = 56;
            s.lightSteps = 6;
            s.bodyOctaves = 5;
            break;
        case CloudType::Cumulus:
        default:
            s.thicknessM = 1800.0 * sf;
            s.extinction = 0.0038;
            s.worleyMix = 0.62;
            s.erosion = 0.45;
            s.warp = 0.50;
            s.anvil = 0.0;
            s.baseSoft = 0.18;
            s.topSoft = 0.45;
            s.shear = 0.35;
            s.hgG = 0.35;
            s.powder = 0.70;
            s.primarySteps = 40;
            s.lightSteps = 6;
            s.bodyOctaves = 5;
            break;
    }
    return s;
}

CloudVolumeSample marchCloudVolume(std::uint64_t seed, const CloudVolumeSpec& spec,
                                   const CloudVolumeLight& light, const SkyVec3& rayDir,
                                   double windRad, double windStrength, double jitter01) {
    CloudVolumeSample out;
    const double topM = spec.baseM + spec.thicknessM;
    // The ray leaves the ground origin; it enters the slab only if it climbs (dir.z > 0).
    if (rayDir.z <= 1e-3 || spec.coverage <= 1e-4 || spec.thicknessM <= 0.0) return out;

    const double tEnter = spec.baseM / rayDir.z;   // metres to the cloud base
    const double tExit = topM / rayDir.z;          // metres to the cloud top
    // (tExit - tEnter) / thickness == 1/dir.z: the slant factor the in-slab path stretches by.
    const double slant = (tExit - tEnter) / spec.thicknessM;
    const int steps = std::max(
        8, static_cast<int>(std::lround(spec.primarySteps * std::min(kSlantStepCap, slant))));
    const double dt = (tExit - tEnter) / steps;    // metres of ray per sample

    const double cosT = skyDot(rayDir, light.sunDir);
    const double phase = phaseFn(cosT, spec.hgG);
    const double lightStepM = spec.thicknessM / std::max(1, spec.lightSteps);

    double transmittance = 1.0;
    Rgb scatter{};
    bool hit = false;

    for (int i = 0; i < steps; ++i) {
        const double t = tEnter + (static_cast<double>(i) + jitter01) * dt;
        const double wx = rayDir.x * t, wy = rayDir.y * t, wz = rayDir.z * t;
        const double dens = cloudDensity(seed, spec, wx, wy, wz, windRad, windStrength, false);
        if (dens <= 1e-3) continue;
        if (!hit) {
            hit = true;
            out.firstHitM = t;
        }
        const double stepTau = dens * spec.extinction * dt;

        // Light march toward the sun: sum optical depth through the (coarse) field for self-shadow.
        double tauL = 0.0;
        for (int j = 0; j < spec.lightSteps; ++j) {
            const double ls = (static_cast<double>(j) + 0.5) * lightStepM;
            const double lx = wx + light.sunDir.x * ls;
            const double ly = wy + light.sunDir.y * ls;
            const double lz = wz + light.sunDir.z * ls;
            tauL += cloudDensity(seed, spec, lx, ly, lz, windRad, windStrength, true) *
                    spec.extinction * lightStepM;
        }
        // Beer-Lambert attenuation toward the sun, blended with the powder-sugar term (restores
        // the dark-edge look near the light): powder=0 -> plain Beer, 1 -> full 2*beer*(1-e^-2t).
        const double beer = std::exp(-tauL);
        const double beerPowder = 2.0 * beer * (1.0 - std::exp(-2.0 * tauL));
        const double lightEnergy = beer + (beerPowder - beer) * spec.powder;

        // Sunlit in-scatter (gained for the missing multiple scattering) + skylight ambient that
        // fills the tops more than the shadowed bases.
        const double h = clamp01((wz - spec.baseM) / spec.thicknessM);
        const double ambFactor = 0.15 + 0.85 * h;
        const double sun = kSunGain * lightEnergy * phase;
        Rgb radiance{light.litColor.r * sun + light.ambColor.r * ambFactor,
                     light.litColor.g * sun + light.ambColor.g * ambFactor,
                     light.litColor.b * sun + light.ambColor.b * ambFactor};

        // Energy-conserving integration: the fraction scattered in this step is (1 - e^-tau).
        const double stepTrans = std::exp(-stepTau);
        const double integ = transmittance * (1.0 - stepTrans);
        scatter.r += integ * radiance.r;
        scatter.g += integ * radiance.g;
        scatter.b += integ * radiance.b;
        transmittance *= stepTrans;
        if (transmittance < 0.01) break;
    }

    if (!hit) return out;
    out.scatter = scatter;
    out.coverage = clamp01(1.0 - transmittance);
    return out;
}

}  // namespace mosaic::core::texture
