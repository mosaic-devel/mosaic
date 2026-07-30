#include "core/texture/noise.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace mosaic::core::texture {

namespace {

// Quintic fade 6t^5 - 15t^4 + 10t^3 (Perlin 2002): C2-continuous across lattice cells, so
// derivative-based consumers (height->normal, analytic-derivative fBm later) see no creases.
double fade(double t) noexcept {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

double lerp(double a, double b, double t) noexcept {
    return a + (b - a) * t;
}

std::int64_t floorI(double v) noexcept {
    return static_cast<std::int64_t>(std::floor(v));
}

// Corner value in [0,1) for value noise.
double cornerValue(std::uint64_t seed, std::int64_t x, std::int64_t y, std::int64_t z = 0) {
    return hashToUnit(hashCoords(seed, x, y, z));
}

// A hashed unit gradient dotted with the corner offset (2D). A continuous random angle (rather
// than a small direction table) keeps the field isotropic -- no axis-aligned streaking.
double grad2(std::uint64_t seed, std::int64_t cx, std::int64_t cy, double dx, double dy) {
    const double a = hashToUnit(hashCoords(seed, cx, cy)) * 2.0 * std::numbers::pi;
    return std::cos(a) * dx + std::sin(a) * dy;
}

// The 12 cube-edge gradients of improved Perlin noise (Perlin 2002). Selected by hash, dotted
// with the corner offset.
double grad3(std::uint64_t seed, std::int64_t cx, std::int64_t cy, std::int64_t cz, double dx,
             double dy, double dz) {
    static constexpr double kGrad[12][3] = {
        {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
        {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};
    const double* g = kGrad[hashCoords(seed, cx, cy, cz) % 12];
    return g[0] * dx + g[1] * dy + g[2] * dz;
}

// Normalisation to [-1, 1]. The 2D bound is exact (unit gradients: max |interpolated dot| =
// sqrt(2)/2, at a cell centre); perlin3's 12-edge raw output was measured at |v| <= ~0.999 over
// 1.8x10^7 samples so it passes through unscaled; the simplex constant is calibrated the same
// way (empirical max 0.9998). Values may in principle poke a hair past +/-1 -- callers that
// need a hard range clamp. These are part of the determinism contract (header note): a retune
// is a golden-breaking change.
constexpr double kNorm2 = 1.4142135623730951;  // sqrt(2)
constexpr double kNorm3 = 1.0;                 // 12-edge dot products already span ~[-1, 1]
constexpr double kNormSimplex2 = 99.2;         // radial kernel (0.5 - d^2)^4, unit gradients

// Gabor: 3 impulses per lattice cell over the 3x3 neighbourhood, Gaussian bandwidth a = 1.3 so a
// kernel decays to <0.5% by the cell boundary (the 3x3 scan then loses no visible energy). Unlike
// the lattice noises this is a sum of random-weight kernels -- ~Gaussian by the CLT, so it has NO
// hard bound; kNormGabor sets the RMS to ~0.6 (typical |gabor2| < 1, with rare larger tails, max
// ~3.6 seen over 5.8x10^5 samples across frequency/anisotropy/orientation). Calibrated once and
// frozen, the same discipline as kNormSimplex2 -- part of the determinism contract.
constexpr int kGaborImpulses = 3;
constexpr double kGaborBandwidth = 1.3;  // Gaussian a (kernel width ~ 1 cell)
constexpr double kNormGabor = 1.40;

}  // namespace

// ---------------------------------------------------------------------------------------------
// Value noise
// ---------------------------------------------------------------------------------------------

double valueNoise2(std::uint64_t seed, double x, double y) {
    const std::int64_t ix = floorI(x), iy = floorI(y);
    const double fx = x - static_cast<double>(ix), fy = y - static_cast<double>(iy);
    const double u = fade(fx), v = fade(fy);
    const double a = cornerValue(seed, ix, iy), b = cornerValue(seed, ix + 1, iy);
    const double c = cornerValue(seed, ix, iy + 1), d = cornerValue(seed, ix + 1, iy + 1);
    return lerp(lerp(a, b, u), lerp(c, d, u), v) * 2.0 - 1.0;
}

double valueNoise3(std::uint64_t seed, double x, double y, double z) {
    const std::int64_t ix = floorI(x), iy = floorI(y), iz = floorI(z);
    const double fx = x - static_cast<double>(ix), fy = y - static_cast<double>(iy),
                 fz = z - static_cast<double>(iz);
    const double u = fade(fx), v = fade(fy), w = fade(fz);
    const auto c = [&](std::int64_t dx, std::int64_t dy, std::int64_t dz) {
        return cornerValue(seed, ix + dx, iy + dy, iz + dz);
    };
    const double lo = lerp(lerp(c(0, 0, 0), c(1, 0, 0), u), lerp(c(0, 1, 0), c(1, 1, 0), u), v);
    const double hi = lerp(lerp(c(0, 0, 1), c(1, 0, 1), u), lerp(c(0, 1, 1), c(1, 1, 1), u), v);
    return lerp(lo, hi, w) * 2.0 - 1.0;
}

// ---------------------------------------------------------------------------------------------
// Gradient (Perlin) noise
// ---------------------------------------------------------------------------------------------

double perlin2(std::uint64_t seed, double x, double y) {
    const std::int64_t ix = floorI(x), iy = floorI(y);
    const double fx = x - static_cast<double>(ix), fy = y - static_cast<double>(iy);
    const double u = fade(fx), v = fade(fy);
    const double n00 = grad2(seed, ix, iy, fx, fy);
    const double n10 = grad2(seed, ix + 1, iy, fx - 1.0, fy);
    const double n01 = grad2(seed, ix, iy + 1, fx, fy - 1.0);
    const double n11 = grad2(seed, ix + 1, iy + 1, fx - 1.0, fy - 1.0);
    return lerp(lerp(n00, n10, u), lerp(n01, n11, u), v) * kNorm2;
}

double perlin3(std::uint64_t seed, double x, double y, double z) {
    const std::int64_t ix = floorI(x), iy = floorI(y), iz = floorI(z);
    const double fx = x - static_cast<double>(ix), fy = y - static_cast<double>(iy),
                 fz = z - static_cast<double>(iz);
    const double u = fade(fx), v = fade(fy), w = fade(fz);
    const auto g = [&](std::int64_t dx, std::int64_t dy, std::int64_t dz) {
        return grad3(seed, ix + dx, iy + dy, iz + dz, fx - static_cast<double>(dx),
                     fy - static_cast<double>(dy), fz - static_cast<double>(dz));
    };
    const double lo = lerp(lerp(g(0, 0, 0), g(1, 0, 0), u), lerp(g(0, 1, 0), g(1, 1, 0), u), v);
    const double hi = lerp(lerp(g(0, 0, 1), g(1, 0, 1), u), lerp(g(0, 1, 1), g(1, 1, 1), u), v);
    return lerp(lo, hi, w) * kNorm3;
}

// ---------------------------------------------------------------------------------------------
// Simplex noise (2D) -- Gustavson's public-domain formulation of Perlin 2001
// ---------------------------------------------------------------------------------------------

double simplex2(std::uint64_t seed, double x, double y) {
    constexpr double F2 = 0.36602540378443865;  // (sqrt(3) - 1) / 2: square -> simplex skew
    constexpr double G2 = 0.21132486540518713;  // (3 - sqrt(3)) / 6: and back
    const double s = (x + y) * F2;
    const std::int64_t i = floorI(x + s), j = floorI(y + s);
    const double t = static_cast<double>(i + j) * G2;
    const double x0 = x - (static_cast<double>(i) - t), y0 = y - (static_cast<double>(j) - t);
    // Which of the two triangles of the skewed cell the point is in: upper (0,1) or lower (1,0).
    const std::int64_t i1 = x0 > y0 ? 1 : 0, j1 = x0 > y0 ? 0 : 1;
    const double x1 = x0 - static_cast<double>(i1) + G2, y1 = y0 - static_cast<double>(j1) + G2;
    const double x2 = x0 - 1.0 + 2.0 * G2, y2 = y0 - 1.0 + 2.0 * G2;
    const auto corner = [&](std::int64_t ci, std::int64_t cj, double dx, double dy) {
        const double tt = 0.5 - dx * dx - dy * dy;
        if (tt <= 0.0) return 0.0;
        const double t4 = (tt * tt) * (tt * tt);
        return t4 * grad2(seed, ci, cj, dx, dy);
    };
    const double n = corner(i, j, x0, y0) + corner(i + i1, j + j1, x1, y1) +
                     corner(i + 1, j + 1, x2, y2);
    return n * kNormSimplex2;
}

// ---------------------------------------------------------------------------------------------
// Gabor noise (sparse convolution) -- Lagae et al. 2009, reimplemented from the paper
// ---------------------------------------------------------------------------------------------

double gabor2(std::uint64_t seed, double x, double y, double frequency, double omegaRad,
              double anisotropy) {
    const std::int64_t ix = floorI(x), iy = floorI(y);
    const double aGauss = std::numbers::pi * kGaborBandwidth * kGaborBandwidth;
    const double twoPiF = 2.0 * std::numbers::pi * frequency;
    double sum = 0.0;
    for (std::int64_t dy = -1; dy <= 1; ++dy) {
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            const std::int64_t cx = ix + dx, cy = iy + dy;
            std::uint64_t h = hashCoords(seed, cx, cy);
            for (int k = 0; k < kGaborImpulses; ++k) {
                // Four decorrelated draws off the chained cell hash: position (x,y), weight, and a
                // random carrier angle blended toward omegaRad by the anisotropy.
                h = avalanche(h);
                const double px = static_cast<double>(cx) + hashToUnit(h);
                h = avalanche(h);
                const double py = static_cast<double>(cy) + hashToUnit(h);
                const double ex = x - px, ey = y - py;
                const double d2 = ex * ex + ey * ey;
                if (d2 > 2.25) continue;  // outside kernel support (a=1.3: envelope ~0)
                h = avalanche(h);
                const double w = hashToUnit(h) * 2.0 - 1.0;  // impulse weight in [-1, 1]
                h = avalanche(h);
                const double randOmega = hashToUnit(h) * 2.0 * std::numbers::pi;
                const double omega = randOmega + (omegaRad - randOmega) * anisotropy;
                const double proj = ex * std::cos(omega) + ey * std::sin(omega);
                sum += w * std::exp(-aGauss * d2) * std::cos(twoPiF * proj);
            }
        }
    }
    return sum * kNormGabor;
}

// ---------------------------------------------------------------------------------------------
// Worley / cellular noise
// ---------------------------------------------------------------------------------------------

namespace {

// One uniformly-jittered feature point per cell; both jitter channels come off the cell hash so
// the whole result is a pure function of (seed, cell).
void scanCell2(std::uint64_t seed, std::int64_t cx, std::int64_t cy, double x, double y,
               WorleyResult& r) {
    const std::uint64_t h = hashCoords(seed, cx, cy);
    const double px = static_cast<double>(cx) + hashToUnit(h);
    const double py = static_cast<double>(cy) + hashToUnit(avalanche(h));
    const double d = std::hypot(px - x, py - y);
    if (d < r.f1) {
        r.f2 = r.f1;
        r.f1 = d;
        r.cellId = h;
        r.nearest = {px, py};
    } else if (d < r.f2) {
        r.f2 = d;
    }
}

// The least distance from (x,y) to any point of cell (cx,cy)'s unit box -- the cheap rejection
// that makes the exact search affordable (no hash spent on a cell that cannot beat f2).
double cellBoxDist2(std::int64_t cx, std::int64_t cy, double x, double y) {
    const double dx = std::max({static_cast<double>(cx) - x, 0.0, x - static_cast<double>(cx + 1)});
    const double dy = std::max({static_cast<double>(cy) - y, 0.0, y - static_cast<double>(cy + 1)});
    return std::hypot(dx, dy);
}

}  // namespace

WorleyResult worley2(std::uint64_t seed, double x, double y) {
    const std::int64_t ix = floorI(x), iy = floorI(y);
    WorleyResult r;
    r.f1 = r.f2 = std::numeric_limits<double>::infinity();
    // The 3x3 core always scans; farther Chebyshev rings only while they could still beat f2
    // (a ring-d cell lies >= d-1 away). A plain 3x3 misses the true nearest in corner cases --
    // this stays exact for the same amortised cost (the box test rejects almost everything).
    for (std::int64_t dy = -1; dy <= 1; ++dy)
        for (std::int64_t dx = -1; dx <= 1; ++dx) scanCell2(seed, ix + dx, iy + dy, x, y, r);
    for (std::int64_t ring = 2; static_cast<double>(ring - 1) < r.f2 && ring <= 4; ++ring) {
        for (std::int64_t dy = -ring; dy <= ring; ++dy) {
            for (std::int64_t dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::llabs(dx), std::llabs(dy)) != ring) continue;  // ring only
                if (cellBoxDist2(ix + dx, iy + dy, x, y) >= r.f2) continue;
                scanCell2(seed, ix + dx, iy + dy, x, y, r);
            }
        }
    }
    return r;
}

Worley3Result worley3(std::uint64_t seed, double x, double y, double z) {
    const std::int64_t ix = floorI(x), iy = floorI(y), iz = floorI(z);
    Worley3Result r;
    r.f1 = r.f2 = std::numeric_limits<double>::infinity();
    const auto scan = [&](std::int64_t cx, std::int64_t cy, std::int64_t cz) {
        const std::uint64_t h = hashCoords(seed, cx, cy, cz);
        const double px = static_cast<double>(cx) + hashToUnit(h);
        const double py = static_cast<double>(cy) + hashToUnit(avalanche(h));
        const double pz = static_cast<double>(cz) + hashToUnit(avalanche(avalanche(h)));
        const double d = std::sqrt((px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z));
        if (d < r.f1) {
            r.f2 = r.f1;
            r.f1 = d;
            r.cellId = h;
        } else if (d < r.f2) {
            r.f2 = d;
        }
    };
    const auto boxDist = [&](std::int64_t cx, std::int64_t cy, std::int64_t cz) {
        const double dx = std::max({static_cast<double>(cx) - x, 0.0,
                                    x - static_cast<double>(cx + 1)});
        const double dy = std::max({static_cast<double>(cy) - y, 0.0,
                                    y - static_cast<double>(cy + 1)});
        const double dz = std::max({static_cast<double>(cz) - z, 0.0,
                                    z - static_cast<double>(cz + 1)});
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    for (std::int64_t dz = -1; dz <= 1; ++dz)
        for (std::int64_t dy = -1; dy <= 1; ++dy)
            for (std::int64_t dx = -1; dx <= 1; ++dx) scan(ix + dx, iy + dy, iz + dz);
    for (std::int64_t ring = 2; static_cast<double>(ring - 1) < r.f2 && ring <= 4; ++ring) {
        for (std::int64_t dz = -ring; dz <= ring; ++dz) {
            for (std::int64_t dy = -ring; dy <= ring; ++dy) {
                for (std::int64_t dx = -ring; dx <= ring; ++dx) {
                    if (std::max({std::llabs(dx), std::llabs(dy), std::llabs(dz)}) != ring)
                        continue;
                    if (boxDist(ix + dx, iy + dy, iz + dz) >= r.f2) continue;
                    scan(ix + dx, iy + dy, iz + dz);
                }
            }
        }
    }
    return r;
}

// ---------------------------------------------------------------------------------------------
// Fractal composition
// ---------------------------------------------------------------------------------------------

namespace {

double basis2(NoiseBasis basis, std::uint64_t seed, double x, double y) {
    switch (basis) {
        case NoiseBasis::Value: return valueNoise2(seed, x, y);
        case NoiseBasis::Perlin: return perlin2(seed, x, y);
        case NoiseBasis::Simplex: return simplex2(seed, x, y);
    }
    return 0.0;
}

double basis3(NoiseBasis basis, std::uint64_t seed, double x, double y, double z) {
    switch (basis) {
        case NoiseBasis::Value: return valueNoise3(seed, x, y, z);
        case NoiseBasis::Perlin: return perlin3(seed, x, y, z);
        case NoiseBasis::Simplex: return perlin3(seed, x, y, z);  // no 3D simplex yet (header note)
    }
    return 0.0;
}

}  // namespace

double fbm2(std::uint64_t seed, double x, double y, const FbmParams& p, NoiseBasis basis) {
    double sum = 0.0, norm = 0.0, amp = 1.0, freq = 1.0;
    for (int o = 0; o < p.octaves; ++o) {
        sum += amp * basis2(basis, subSeed(seed, static_cast<std::uint64_t>(o)), x * freq,
                            y * freq);
        norm += amp;
        amp *= p.gain;
        freq *= p.lacunarity;
    }
    return norm > 0.0 ? sum / norm : 0.0;
}

double fbm3(std::uint64_t seed, double x, double y, double z, const FbmParams& p,
            NoiseBasis basis) {
    double sum = 0.0, norm = 0.0, amp = 1.0, freq = 1.0;
    for (int o = 0; o < p.octaves; ++o) {
        sum += amp * basis3(basis, subSeed(seed, static_cast<std::uint64_t>(o)), x * freq,
                            y * freq, z * freq);
        norm += amp;
        amp *= p.gain;
        freq *= p.lacunarity;
    }
    return norm > 0.0 ? sum / norm : 0.0;
}

Vec2 domainWarp2(std::uint64_t seed, Vec2 p, double amplitude, double frequency,
                 const FbmParams& fbm, NoiseBasis basis) {
    // Two decorrelated warp channels; sub-seed tags are arbitrary but frozen (golden contract).
    const double wx = fbm2(subSeed(seed, 0x57415250u), p.x * frequency, p.y * frequency, fbm,
                           basis);
    const double wy = fbm2(subSeed(seed, 0x57415251u), p.x * frequency, p.y * frequency, fbm,
                           basis);
    return {p.x + amplitude * wx, p.y + amplitude * wy};
}

}  // namespace mosaic::core::texture
