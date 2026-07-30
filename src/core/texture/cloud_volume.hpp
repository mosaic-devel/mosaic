#pragma once

#include <cstdint>

#include "core/texture/noise.hpp"
#include "core/texture/sky_camera.hpp"
#include "core/texture/texture_params.hpp"

// The S55-c volumetric cloud lane (docs/texture-generator.md §4.3 lane B / §9.1). An IMPLICIT
// procedural 3D density field (Perlin-Worley fBm + domain warp, shaped by a per-type height
// gradient and coverage remap) is ray-marched with single-medium Beer-Lambert transmittance, a
// Henyey-Greenstein phase function, a powder/dual-lobe term and a secondary light-march for
// self-shadowing -- the published Nubis/Hillaire technique, clean-room, no ML, no engine code.
// It replaces the flat 2D projection for the heap/tower types (Cumulus, Cumulonimbus) while the
// sheet/wisp types stay on sky_render.cpp's fast 2D lane. See cloud_volume.cpp for the full
// lineage header and the two standing construction constraints it records.
//
// Purity contract (§8.3): every function here is a pure function of (seed, geometry, ray) built
// only on the hash-seeded noise kit -- same inputs give the same pixels at any thread count.
namespace mosaic::core::texture {

// Which catalogue types render through the volumetric lane. Cumulus/Cumulonimbus are the rounded
// heaps and towers whose lit-top/shaded-base shading only the marched field can carry; every
// other type (sheets, veils, wisps) reads fine -- and far cheaper -- on the 2D lane.
[[nodiscard]] bool cloudTypeIsVolumetric(CloudType t) noexcept;

// A cloud slab cooked for the marcher: the world geometry (base altitude + vertical extent +
// horizontal feature size), the effective coverage (already master-dial * per-deck bias), and the
// per-type shaping/lighting/quality knobs. Golden-pinned constants live in cloudVolumeSpec().
struct CloudVolumeSpec {
    double baseM = 1300.0;       // cloud base altitude (metres above the camera at the origin)
    double thicknessM = 1800.0;  // vertical extent, base -> top
    double featureM = 1900.0;    // horizontal feature size (the low-frequency base cells)
    double coverage = 0.4;       // 0 (clear) .. 1 (packed): carves where the field is cloud
    double extinction = 0.0035;  // per-metre optical density at full field value
    double worleyMix = 0.6;      // 0 pure fBm billow .. 1 cellular (Worley) cauliflower
    double erosion = 0.45;       // high-frequency Worley edge nibbling (wispy detail)
    double warp = 0.55;          // domain-warp amplitude (billowing deformation)
    double anvil = 0.0;          // 0 rounded top .. 1 spread anvil (Cumulonimbus)
    double baseSoft = 0.18;      // height gradient: soft rounded base
    double topSoft = 0.45;       // height gradient: where the top taper begins
    double shear = 0.4;          // wind elongation along the wind axis
    double hgG = 0.35;           // Henyey-Greenstein forward asymmetry
    double powder = 0.7;         // 0 plain Beer .. 1 full powder-sugar dark-edge term
    int primarySteps = 40;       // ray-march samples through the slab
    int lightSteps = 6;          // secondary light-march samples toward the sun
    int bodyOctaves = 5;         // fBm octaves of the density body
};

// Build the cooked spec for a type: the per-type golden table, with the deck's runtime base
// altitude, horizontal feature size and coverage folded in. `scaleFactor` (the Scale * cloudScale
// * per-deck bias the 2D lane already applied to featureM) also stretches the vertical extent so a
// scaled-up cloud stays proportioned instead of pancaking.
[[nodiscard]] CloudVolumeSpec cloudVolumeSpec(CloudType type, double baseM, double featureM,
                                              double coverage, double scaleFactor);

// The shared lighting context for a render (built once, per pixel is the cheap march).
struct CloudVolumeLight {
    SkyVec3 sunDir;   // unit world direction TOWARD the sun
    Rgb litColor;     // display-linear sunlit-face radiance (sky_render's litBase)
    Rgb ambColor;     // display-linear shadowed-face / skylight fill (sky_render's ambBase)
};

// One deck's contribution along a camera ray from the ground origin.
struct CloudVolumeSample {
    Rgb scatter{};           // in-scattered radiance (display-linear, straight -- pre-tonemap)
    double coverage = 0.0;   // 1 - transmittance through the slab (the alpha to composite)
    double firstHitM = 0.0;  // slant distance to the first dense sample (aerial-perspective fade)
};

// March the slab for a single unit camera ray. Returns coverage 0 when the ray misses the slab or
// the field is clear along it. `windRad`/`windStrength` match the 2D lane's shear convention.
// `jitter01` in [0, 1) offsets every march sample by that fraction of a step (0.5 = the plain
// midpoint rule): a caller that feeds each ray a deterministic per-pixel value (sky_render keys it
// to the FRAME pixel, like the display dither) decorrelates neighbouring rays' sample lattices, so
// fixed-step quadrature aliasing of the density field -- coherent "wavy strips" across the cloud
// bodies -- breaks up into imperceptible fine grain. Same march, same cost, still a pure function
// of its inputs.
[[nodiscard]] CloudVolumeSample marchCloudVolume(std::uint64_t seed, const CloudVolumeSpec& spec,
                                                 const CloudVolumeLight& light,
                                                 const SkyVec3& rayDir, double windRad,
                                                 double windStrength, double jitter01 = 0.5);

}  // namespace mosaic::core::texture
