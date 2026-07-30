#pragma once

#include <cstdint>

#include "common/geometry.hpp"

// Deterministic procedural-noise kit -- the Texture Generator's shared primitives (S55-a;
// docs/texture-generator.md §8.3). Published lineage ONLY:
//   - value noise + fBm: classic Perlin lineage ("An Image Synthesizer", SIGGRAPH 1985);
//   - gradient ("Perlin") noise: Perlin 1985/2002 (the improved-gradient set);
//   - simplex noise: Perlin 2001; implemented from Gustavson, "Simplex noise demystified"
//     (public-domain spec), not from any tabled reference listing;
//   - Worley / cellular noise: Worley, "A Cellular Texture Basis Function" (SIGGRAPH 1996);
//   - domain warping: Perlin & Hoffert "Hypertexture" (SIGGRAPH 1989) / standard practice.
// Hash: SplitMix64/Murmur3-style finalizer avalanche (Vigna/Appleby, public domain constants).
// ⚠ INVARIANT -- NO WAVELET NOISE, here or in anything built on this kit. Band-limiting, when it
// comes, is analytic-derivative fBm over the primitives above. That is a deliberate standing
// limit on the kit's design space, not a gap waiting to be filled in.
//
// THE DETERMINISM CONTRACT (§8.3): every primitive is a pure function of (seed, coordinates) --
// lattice values are hash-seeded on the integer lattice, with no permutation tables, no global
// state and no stream order. Same seed + params + resolution => the same pixels at any tile
// order or thread count, CPU or GPU (the resynthesizer's hashed-RNG discipline). Changing any
// constant below is therefore a GOLDEN-BREAKING change to every generator built on the kit.
namespace mosaic::core::texture {

using common::Vec2;

// ---------------------------------------------------------------------------------------------
// Hashing (the seed plumbing everything else rides on)
// ---------------------------------------------------------------------------------------------

// Finalizer-style avalanche mix: every input bit flips ~half the output bits. The Murmur3/
// SplitMix64 finalizer constants (public domain).
[[nodiscard]] constexpr std::uint64_t avalanche(std::uint64_t z) noexcept {
    z ^= z >> 33;
    z *= 0xff51afd7ed558ccdULL;
    z ^= z >> 33;
    z *= 0xc4ceb9fe1a85ec53ULL;
    z ^= z >> 33;
    return z;
}

// Hash a lattice coordinate under a seed. Chained (not XOR-folded) so (x,y) and (y,x) never
// collide by construction; the z arm defaults inert for the 2D callers.
[[nodiscard]] constexpr std::uint64_t hashCoords(std::uint64_t seed, std::int64_t x,
                                                 std::int64_t y, std::int64_t z = 0) noexcept {
    std::uint64_t h = seed ^ 0x9e3779b97f4a7c15ULL;  // golden-ratio offset: seed 0 still mixes
    h = avalanche(h ^ static_cast<std::uint64_t>(x));
    h = avalanche(h ^ static_cast<std::uint64_t>(y));
    h = avalanche(h ^ static_cast<std::uint64_t>(z));
    return h;
}

// Top 53 bits -> a double in [0, 1). Uniform, and exactly representable.
[[nodiscard]] constexpr double hashToUnit(std::uint64_t h) noexcept {
    return static_cast<double>(h >> 11) * 0x1.0p-53;
}

// Derive an independent sub-stream seed (per octave, per warp axis, per generator element) so
// composed primitives never read correlated lattices. `tag` is any small caller-chosen constant.
[[nodiscard]] constexpr std::uint64_t subSeed(std::uint64_t seed, std::uint64_t tag) noexcept {
    return avalanche(seed ^ (tag * 0x9e3779b97f4a7c15ULL));
}

// ---------------------------------------------------------------------------------------------
// Noise primitives. All return approximately [-1, 1], zero-centred, smooth (C2 for the lattice
// noises via the quintic fade). Frequency is carried by the caller scaling the coordinates.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] double valueNoise2(std::uint64_t seed, double x, double y);
[[nodiscard]] double valueNoise3(std::uint64_t seed, double x, double y, double z);

[[nodiscard]] double perlin2(std::uint64_t seed, double x, double y);
[[nodiscard]] double perlin3(std::uint64_t seed, double x, double y, double z);

// 2D simplex-lattice gradient noise (fewer directional artifacts than the square lattice; the
// cloud/marble workhorse). 3D waits until a generator needs it (volumetric clouds ride perlin3).
[[nodiscard]] double simplex2(std::uint64_t seed, double x, double y);

// Sparse-convolution Gabor noise (Lagae, Lagae, Drettakis & Dutré, "Procedural Noise using Sparse
// Gabor Convolution", SIGGRAPH 2009) -- band-limited noise with an explicit orientation, the
// spectral-fibre primitive for the paper/material generators (§5.1). Reimplemented from the paper
// (its reference code carries no OSS licence). A hash-seeded set of Gabor kernels
// (Gaussian envelope × oriented cosine carrier) is summed over the 3x3 lattice neighbourhood:
//   - `frequency` = the carrier F0 in cycles per unit cell (the fibre pitch);
//   - `omegaRad`  = the carrier orientation (stripes run perpendicular to it);
//   - `anisotropy` in [0,1] blends each impulse's OWN random carrier direction (0 = isotropic
//     band-limited noise) toward the fixed `omegaRad` (1 = one coherent direction = fibre streaks).
// Deterministic (impulses are pure functions of (seed, cell)); returns approximately [-1, 1].
[[nodiscard]] double gabor2(std::uint64_t seed, double x, double y, double frequency,
                            double omegaRad, double anisotropy);

// Worley/cellular noise: one feature point per lattice cell, uniformly jittered. F1/F2 are the
// distances to the nearest / second-nearest feature (Euclidean, in lattice units); `cellId` is a
// stable hash identity of the F1 cell (clump tinting, per-cell facing); `nearest` its position.
// EXACT: the neighbourhood search expands by rings until no farther ring can beat the current F2
// (a plain 3x3 can miss the true nearest in corner cases -- the pattern-lattice lesson).
struct WorleyResult {
    double f1 = 0.0;
    double f2 = 0.0;
    std::uint64_t cellId = 0;
    Vec2 nearest{0.0, 0.0};
};
[[nodiscard]] WorleyResult worley2(std::uint64_t seed, double x, double y);

struct Worley3Result {
    double f1 = 0.0;
    double f2 = 0.0;
    std::uint64_t cellId = 0;
};
[[nodiscard]] Worley3Result worley3(std::uint64_t seed, double x, double y, double z);

// ---------------------------------------------------------------------------------------------
// Fractal composition
// ---------------------------------------------------------------------------------------------

enum class NoiseBasis { Value, Perlin, Simplex };

struct FbmParams {
    int octaves = 4;
    double lacunarity = 2.0;  // frequency multiplier per octave
    double gain = 0.5;        // amplitude multiplier per octave
    bool operator==(const FbmParams&) const = default;
};

// Fractional Brownian motion over a basis, normalised back to [-1, 1] (divides by the amplitude
// sum). Each octave reads its OWN sub-seeded lattice (subSeed(seed, octave)) so octaves are
// decorrelated instead of self-similar at the origin.
[[nodiscard]] double fbm2(std::uint64_t seed, double x, double y, const FbmParams& p,
                          NoiseBasis basis = NoiseBasis::Perlin);
[[nodiscard]] double fbm3(std::uint64_t seed, double x, double y, double z, const FbmParams& p,
                          NoiseBasis basis = NoiseBasis::Perlin);

// Domain warp: displace `p` by an independent 2-channel fBm field (the billowing/veining
// deformer). `frequency` scales p into the warp field; `amplitude` is the displacement in the
// caller's units. Returns the warped point for the caller to feed its own lookup.
[[nodiscard]] Vec2 domainWarp2(std::uint64_t seed, Vec2 p, double amplitude, double frequency,
                               const FbmParams& fbm, NoiseBasis basis = NoiseBasis::Perlin);

}  // namespace mosaic::core::texture
