#pragma once

#include <cstdint>
#include <vector>

#include "core/texture/sky_camera.hpp"  // SkyVec3, Rgb, skyDot, skyNormalize

// Physical single-scattering atmosphere (S55 night overhaul, phase 4; docs/texture-generator.md
// §4). A clean-room CPU integrator of the radiative-transfer single-scattering integral -- the
// physics that makes a real twilight go warm-horizon -> Belt-of-Venus arch -> deep-blue nautical
// -> dark night as the sun sinks below the horizon, ALL emergent from scattering geometry rather
// than a hand-authored gradient. It replaces the frozen-Hosek-Wilkie-plus-hand-gradient twilight
// of the S55-f night for the sub-horizon sun; the >= 0 daytime dome stays Hosek-Wilkie (sky_render
// gates the handoff so the day path is byte-identical).
//
// Technique lineage: the single-scattering radiative-transfer integral itself is plain physics --
// in-scattered sunlight = Rayleigh (Rayleigh 1871) + Mie (Mie 1908, Cornette-Shanks 1992 phase)
// weighted by Beer-Lambert (1729/1852) transmittance from each sample to the sun and back to the
// camera, over exponential density profiles. The spherical-shell integration is Nishita et al.
// 1993 ("Display of the Earth Taking into Account Atmospheric Scattering", SIGGRAPH '93 -- the
// foundational paper); the tabulated optical-depth-to-space table is the standard acceleration
// used by Preetham 1999 / O'Neil 2005 / Bruneton 2008. REIMPLEMENTED FROM THE PAPERS' EQUATIONS
// -- no engine source copied (Bruneton's reference is BSD, Hillaire's MIT; we borrow the physics,
// not their code). No ML.
namespace mosaic::core::texture {

// One cooked atmosphere for a render: the sun direction + turbidity fixed, a transmittance-to-space
// table baked, ready to evaluate the in-scattered sky radiance for any view ray cheaply. Pure and
// stateless once cooked (a function of the view ray only), so it is parallelism- and window-crop-
// exact like every other sky element. Build once per render; evaluate per pixel.
struct Atmosphere {
    // Baked transmittance-to-space table: optical depth (grey, per species) from a point at radius
    // r looking outward along a ray of zenith-cosine mu, out to the top of the atmosphere. Indexed
    // [ir * kMuSteps + imu]; a ground-occluded ray stores kBlockedOd (-> transmittance 0, i.e. the
    // point is in Earth's shadow -- the mechanism that raises the twilight wedge). Two scalars per
    // cell (Rayleigh / Mie optical depth); the per-channel transmittance recombines them with the
    // wavelength-dependent scattering coefficients at eval time.
    static constexpr int kRSteps = 32;    // radial (altitude) samples
    static constexpr int kMuSteps = 256;  // zenith-cosine samples
    std::vector<float> odRayleigh;        // size kRSteps*kMuSteps
    std::vector<float> odMie;

    // Baked isotropic multiple-scattering table (Hillaire 2020's Psi_ms, reimplemented from the
    // paper's equations -- his reference implementation is MIT, and we take only the physics):
    // for a point at radius r under a sun of zenith-cosine muS, the radiance re-scattered toward
    // it by the REST of the atmosphere, summed over all orders as the geometric series
    // L2 / (1 - f_ms) (energy-conserving: f_ms < 1 is the fraction re-scattered per order, so no
    // energy is invented). This is what keeps a real twilight alive after direct sunlight has
    // left most of the shell -- the blue-hour ambient and the smooth anti-solar gradient single
    // scattering cannot carry. Indexed [ir * kMsMuSteps + imu], 3 floats (RGB) per cell.
    static constexpr int kMsRSteps = 16;
    static constexpr int kMsMuSteps = 64;
    std::vector<float> msTable;           // size kMsRSteps*kMsMuSteps*3

    SkyVec3 sunDir{0.0, 0.0, 1.0};
    double betaMieScatter = 0.0;   // effective Mie scattering coefficient (turbidity-scaled)
    double betaMieExtinct = 0.0;   // Mie extinction (scattering + absorption)
    double mieG = 0.76;            // Cornette-Shanks asymmetry
    int viewSteps = 32;            // per-ray march segments

    // In-scattered sky radiance (Rayleigh + Mie single scattering) for a unit view ray from the
    // ground camera, in physical-ish linear radiance (a fixed solar intensity folded in; the caller
    // applies its own display exposure). Finite and non-negative for any input. dir need not be
    // normalised.
    [[nodiscard]] Rgb radiance(SkyVec3 viewDir) const noexcept;
};

// Cook the atmosphere for `sunDir` (unit; its z is sin(sun elevation), negative below the horizon)
// and `turbidity` (1 crystalline .. 10 murky -- scales the Mie term). Cheap: one fixed-size table
// bake. Call once per render, then evaluate radiance() per pixel.
[[nodiscard]] Atmosphere cookAtmosphere(SkyVec3 sunDir, double turbidity) noexcept;

}  // namespace mosaic::core::texture
