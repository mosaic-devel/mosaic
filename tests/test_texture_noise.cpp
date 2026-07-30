#include "core/texture/noise.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// The noise kit's determinism contract (S55-a; docs/texture-generator.md §8.3): every primitive
// is a pure function of (seed, coordinates). These tests pin the CONTRACT -- determinism, range,
// smoothness at lattice seams (the classic value/gradient-noise bug site), seed independence, and
// Worley's exactness (the ring-expansion search must equal brute force) -- rather than pixel
// values; the renderer goldens in test_texture_layer.cpp pin the bytes.
namespace {

using namespace mosaic::core::texture;

// A modest deterministic point set spread over several cells (offsets are odd multipliers so
// points land at varied intra-cell positions, some very near lattice lines).
struct SamplePoint {
    double x, y, z;
};
std::vector<SamplePoint> samplePoints() {
    std::vector<SamplePoint> pts;
    for (int i = 0; i < 200; ++i) {
        pts.push_back({i * 0.137 - 12.5, i * 0.211 + 3.75, i * 0.0917 - 4.0});
    }
    return pts;
}

}  // namespace

TEST_CASE("hashCoords is deterministic, order-sensitive and seed-sensitive") {
    CHECK(hashCoords(1, 2, 3) == hashCoords(1, 2, 3));
    CHECK(hashCoords(1, 2, 3) != hashCoords(1, 3, 2));   // (x,y) never collides with (y,x)
    CHECK(hashCoords(1, 2, 3) != hashCoords(2, 2, 3));   // seed matters
    CHECK(hashCoords(0, 0, 0) != hashCoords(0, 0, 1));   // z arm participates
    CHECK(hashCoords(0, 0, 0, 0) != hashCoords(0, 0, 0, 1));
    // hashToUnit lands in [0, 1).
    for (std::uint64_t s = 0; s < 64; ++s) {
        const double u = hashToUnit(hashCoords(s, 17, -9));
        CHECK(u >= 0.0);
        CHECK(u < 1.0);
    }
    // subSeed decorrelates: different tags give different streams.
    CHECK(subSeed(7, 0) != subSeed(7, 1));
    CHECK(subSeed(7, 0) != subSeed(8, 0));
}

TEST_CASE("noise primitives are deterministic and bounded") {
    const auto pts = samplePoints();
    for (const std::uint64_t seed : {0ull, 1ull, 0xDEADBEEFull}) {
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (const auto& p : pts) {
            const double v2 = valueNoise2(seed, p.x, p.y);
            const double v3 = valueNoise3(seed, p.x, p.y, p.z);
            const double p2 = perlin2(seed, p.x, p.y);
            const double p3 = perlin3(seed, p.x, p.y, p.z);
            const double s2 = simplex2(seed, p.x, p.y);
            // Pure functions: a second evaluation is bit-identical.
            CHECK(v2 == valueNoise2(seed, p.x, p.y));
            CHECK(v3 == valueNoise3(seed, p.x, p.y, p.z));
            CHECK(p2 == perlin2(seed, p.x, p.y));
            CHECK(p3 == perlin3(seed, p.x, p.y, p.z));
            CHECK(s2 == simplex2(seed, p.x, p.y));
            // Normalised range (calibrated in noise.cpp; a hair of slack for the empirical ones).
            for (const double v : {v2, v3, p2, p3, s2}) {
                CHECK(v >= -1.05);
                CHECK(v <= 1.05);
            }
            lo = std::min({lo, v2, p2, s2});
            hi = std::max({hi, v2, p2, s2});
        }
        // The fields actually vary (not a constant function).
        CHECK(lo < -0.2);
        CHECK(hi > 0.2);
    }
}

TEST_CASE("noise fields differ between seeds") {
    const auto pts = samplePoints();
    int differs = 0;
    for (const auto& p : pts)
        if (perlin2(1, p.x, p.y) != perlin2(2, p.x, p.y)) ++differs;
    CHECK(differs > 150);  // essentially everywhere
}

TEST_CASE("lattice noises are continuous across cell boundaries") {
    // The classic seam bug: a mismatched corner hash on either side of an integer line. Sample a
    // hair on each side of many lattice lines; a smooth field moves O(eps), a seam jumps O(1).
    constexpr double eps = 1e-7;
    constexpr double tol = 1e-4;
    for (int i = -8; i <= 8; ++i) {
        const double y = i * 0.618 + 0.37;
        const double xL = static_cast<double>(i) - eps, xR = static_cast<double>(i) + eps;
        CHECK(std::fabs(valueNoise2(5, xL, y) - valueNoise2(5, xR, y)) < tol);
        CHECK(std::fabs(perlin2(5, xL, y) - perlin2(5, xR, y)) < tol);
        CHECK(std::fabs(valueNoise3(5, xL, y, 2.3) - valueNoise3(5, xR, y, 2.3)) < tol);
        CHECK(std::fabs(perlin3(5, xL, y, 2.3) - perlin3(5, xR, y, 2.3)) < tol);
        CHECK(std::fabs(simplex2(5, xL, y) - simplex2(5, xR, y)) < tol);
    }
}

TEST_CASE("gradient noises vanish on the lattice, value noise does not") {
    // Gradient (Perlin) noise is zero AT every lattice point (the corner dot products are all
    // against a zero offset); value noise is the hashed corner value itself. This distinguishes
    // the two constructions -- a regression here means someone swapped a basis.
    CHECK(perlin2(9, 3.0, -7.0) == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(perlin3(9, 3.0, -7.0, 5.0) == doctest::Approx(0.0).epsilon(1e-12));
    int nonzero = 0;
    for (int i = 0; i < 16; ++i)
        if (std::fabs(valueNoise2(9, i, -i)) > 0.05) ++nonzero;
    CHECK(nonzero > 8);
}

namespace {

// Brute-force Worley reference: scan a window wide enough to be unarguably exact.
WorleyResult worley2Brute(std::uint64_t seed, double x, double y) {
    const auto fx = static_cast<std::int64_t>(std::floor(x));
    const auto fy = static_cast<std::int64_t>(std::floor(y));
    WorleyResult r;
    r.f1 = r.f2 = std::numeric_limits<double>::infinity();
    for (std::int64_t dy = -5; dy <= 5; ++dy) {
        for (std::int64_t dx = -5; dx <= 5; ++dx) {
            const std::uint64_t h = hashCoords(seed, fx + dx, fy + dy);
            const double px = static_cast<double>(fx + dx) + hashToUnit(h);
            const double py = static_cast<double>(fy + dy) + hashToUnit(avalanche(h));
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
    }
    return r;
}

}  // namespace

TEST_CASE("worley2 matches an exact brute-force reference") {
    const auto pts = samplePoints();
    for (const std::uint64_t seed : {0ull, 42ull}) {
        for (const auto& p : pts) {
            const WorleyResult fast = worley2(seed, p.x, p.y);
            const WorleyResult ref = worley2Brute(seed, p.x, p.y);
            CHECK(fast.f1 == ref.f1);
            CHECK(fast.f2 == ref.f2);
            CHECK(fast.cellId == ref.cellId);
            CHECK(fast.nearest.x == ref.nearest.x);
            CHECK(fast.nearest.y == ref.nearest.y);
            // Structural sanity.
            CHECK(fast.f1 >= 0.0);
            CHECK(fast.f1 <= fast.f2);
            CHECK(std::hypot(fast.nearest.x - p.x, fast.nearest.y - p.y) ==
                  doctest::Approx(fast.f1).epsilon(1e-12));
        }
    }
}

TEST_CASE("worley3 is deterministic and ordered") {
    const auto pts = samplePoints();
    for (const auto& p : pts) {
        const Worley3Result a = worley3(11, p.x, p.y, p.z);
        const Worley3Result b = worley3(11, p.x, p.y, p.z);
        CHECK(a.f1 == b.f1);
        CHECK(a.f2 == b.f2);
        CHECK(a.cellId == b.cellId);
        CHECK(a.f1 >= 0.0);
        CHECK(a.f1 <= a.f2);
        // One feature per unit cell: the nearest can never be farther than the cell diagonal
        // of the 3x3x3 core's worst case.
        CHECK(a.f1 <= std::sqrt(3.0) + 1e-9);
    }
}

TEST_CASE("fbm normalises, decorrelates octaves, and honours the basis") {
    const auto pts = samplePoints();
    const FbmParams p4{4, 2.0, 0.5};
    const FbmParams p1{1, 2.0, 0.5};
    for (const auto& pt : pts) {
        const double f = fbm2(3, pt.x, pt.y, p4);
        CHECK(f == fbm2(3, pt.x, pt.y, p4));  // deterministic
        CHECK(f >= -1.05);
        CHECK(f <= 1.05);
        // A single octave is exactly the sub-seeded basis (the composition contract).
        CHECK(fbm2(3, pt.x, pt.y, p1) == perlin2(subSeed(3, 0), pt.x, pt.y));
        CHECK(fbm2(3, pt.x, pt.y, p1, NoiseBasis::Value) ==
              valueNoise2(subSeed(3, 0), pt.x, pt.y));
        // 3D: the Simplex basis deliberately falls back to perlin3 until a 3D simplex exists.
        CHECK(fbm3(3, pt.x, pt.y, pt.z, p1, NoiseBasis::Simplex) ==
              fbm3(3, pt.x, pt.y, pt.z, p1, NoiseBasis::Perlin));
    }
    // Zero octaves cannot divide by zero.
    CHECK(fbm2(3, 1.0, 2.0, FbmParams{0, 2.0, 0.5}) == 0.0);
}

TEST_CASE("domain warp displaces by its amplitude and is inert at zero") {
    const FbmParams fp{3, 2.0, 0.5};
    const mosaic::common::Vec2 p{4.2, -1.7};
    const auto w0 = domainWarp2(5, p, 0.0, 1.0, fp);
    CHECK(w0.x == p.x);
    CHECK(w0.y == p.y);
    const auto w1 = domainWarp2(5, p, 2.0, 1.0, fp);
    CHECK(w1.x == domainWarp2(5, p, 2.0, 1.0, fp).x);  // deterministic
    CHECK(std::fabs(w1.x - p.x) <= 2.0 * 1.05);        // bounded by amplitude * |fbm|max
    CHECK(std::fabs(w1.y - p.y) <= 2.0 * 1.05);
    // The two warp channels are decorrelated: displacement is not the same in x and y.
    CHECK(w1.x - p.x != w1.y - p.y);
}
