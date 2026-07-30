#include "core/inpaint/outpaint.hpp"
#include "core/inpaint/backends/he_sun/graph_completion.hpp"
#include "core/inpaint/backends/he_sun/graph_cut.hpp"
#include "core/inpaint/backends/he_sun/offset_statistics.hpp"
#include "core/inpaint/backends/he_sun/working_region.hpp"
#include "core/inpaint/backends/script/script_backend.hpp"
#include "core/inpaint/inpaint_engine.hpp"
#include "core/selection.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <doctest/doctest.h>

// Inpainting engine: pluggable-backend registry/dispatch + the cleared diffusion (PdeBackend)
// filler (PLAN S37-b). Verified with analytic invariants (a Laplace fill of a constant field is
// that constant), matching Mosaic's deterministic-headless verification division — the He & Sun
// offset-statistics backend and its golden-image diffs arrive with S37-c.
namespace {

using mosaic::common::ColorF;
using mosaic::common::ImageF;
using mosaic::common::Rect;
using mosaic::core::Selection;
namespace inpaint = mosaic::core::inpaint;

ImageF constantImage(std::uint32_t w, std::uint32_t h, ColorF c) {
    ImageF img(w, h);
    img.fill(c);
    return img;
}

// 24x8 image: period-6 vertical stripes plus a small y-gradient (so only a horizontal shift by a
// multiple of 6 reproduces it exactly). Used by the offset-statistics / graph-completion tests.
ImageF periodicImage() {
    ImageF im(24, 8);
    const float base[6] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 24; ++x) {
            const float v = 0.5f * base[x % 6] + 0.5f * (static_cast<float>(y) / 7.0f);
            im.set(x, y, {v, v, v, 1.0f});
        }
    }
    return im;
}

// Run a request through the cleared diffusion backend specifically (the default is now
// offset-stats).
inpaint::InpaintResult runPde(const inpaint::InpaintRequest& req) {
    inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    eng.setActiveBackend("pde");
    return eng.run(req);
}

// Per-pixel approximate image equality (Poisson's Gauss-Seidel relaxation drifts a few ULPs even
// when it reproduces the source, so reconstruction can't be checked bit-exactly).
bool imagesApproxEqual(const ImageF& a, const ImageF& b, float eps) {
    if (a.width != b.width || a.height != b.height) {
        return false;
    }
    for (std::uint32_t y = 0; y < a.height; ++y) {
        for (std::uint32_t x = 0; x < a.width; ++x) {
            const ColorF ca = a.at(x, y);
            const ColorF cb = b.at(x, y);
            if (std::fabs(ca.r - cb.r) > eps || std::fabs(ca.g - cb.g) > eps ||
                std::fabs(ca.b - cb.b) > eps || std::fabs(ca.a - cb.a) > eps) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TEST_CASE("default engine registers the built-in backends with pde active") {
    const inpaint::InpaintEngine engine = inpaint::makeDefaultEngine();
    const auto ids = engine.backendIds();
    CHECK(std::find(ids.begin(), ids.end(), "pde") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "offset-stats") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "script") != ids.end());
    CHECK(engine.activeBackend() == "offset-stats"); // He & Sun is the default
}

TEST_CASE("engine dispatch: backend selection and the no-backend case") {
    inpaint::InpaintEngine engine; // empty
    CHECK(engine.empty());

    const ImageF img = constantImage(4, 4, {0.2f, 0.4f, 0.6f, 1.0f});
    const Selection none;
    const inpaint::InpaintRequest req{img, none, {}};

    const inpaint::InpaintResult miss = engine.run(req);
    CHECK_FALSE(miss.ok); // nothing registered

    inpaint::InpaintEngine def = inpaint::makeDefaultEngine();
    CHECK(def.setActiveBackend("script"));
    CHECK(def.activeBackend() == "script");
    CHECK_FALSE(def.setActiveBackend("does-not-exist"));
    CHECK(def.activeBackend() == "script"); // unchanged on a bad id
}

TEST_CASE("offset-stats: the request cancel token aborts the run (S39-b responsiveness)") {
    const ImageF im = periodicImage();
    const Selection hole = Selection::rectangle(24, 8, Rect{9, 0, 6, 8});
    inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    REQUIRE(eng.setActiveBackend("offset-stats"));

    // A token already raised: the first cancel poll bails, so the run declines as "cancelled"
    // rather than producing a fill. This is the mechanism the status-bar cancel X drives from the
    // UI thread.
    std::atomic<bool> cancel{true};
    const inpaint::InpaintRequest cancelled{im, hole, {}, &cancel};
    const inpaint::InpaintResult r1 = eng.run(cancelled);
    CHECK_FALSE(r1.ok);
    CHECK(r1.detail == "cancelled"); // declined as cancelled, not as a fill or another failure
}

TEST_CASE("diffusion fill of a constant field reproduces the constant (harmonic invariant)") {
    const ColorF bg{0.2f, 0.4f, 0.6f, 1.0f};
    const ImageF img = constantImage(8, 8, bg);
    const Selection hole = Selection::rectangle(8, 8, Rect{3, 3, 2, 2}); // 2x2 interior hole

    const inpaint::InpaintRequest req{img, hole, {}};
    const inpaint::InpaintResult res = runPde(req);
    REQUIRE(res.ok);

    // Laplace fill with a constant Dirichlet boundary is exactly that constant.
    for (std::uint32_t y = 3; y < 5; ++y) {
        for (std::uint32_t x = 3; x < 5; ++x) {
            const ColorF c = res.image.at(x, y);
            CHECK(c.r == doctest::Approx(bg.r).epsilon(0.001));
            CHECK(c.g == doctest::Approx(bg.g).epsilon(0.001));
            CHECK(c.b == doctest::Approx(bg.b).epsilon(0.001));
            CHECK(c.a == doctest::Approx(bg.a).epsilon(0.001));
        }
    }
    // Known pixels are untouched.
    CHECK(res.image.at(0, 0).r == doctest::Approx(bg.r));
}

TEST_CASE("diffusion fill interpolates between two boundary values (monotone, in range)") {
    // Left column black, right column white, a hole spanning the middle. The harmonic fill must
    // stay within [0,1] and increase left-to-right across the hole row.
    ImageF img(6, 1);
    for (std::uint32_t x = 0; x < 6; ++x) {
        img.set(x, 0, {x < 3 ? 0.0f : 1.0f, 0.0f, 0.0f, 1.0f});
    }
    const Selection hole = Selection::rectangle(6, 1, Rect{2, 0, 2, 1}); // x in {2,3}

    const inpaint::InpaintRequest req{img, hole, {}};
    const inpaint::InpaintResult res = runPde(req);
    REQUIRE(res.ok);
    const float a = res.image.at(2, 0).r;
    const float b = res.image.at(3, 0).r;
    CHECK(a >= 0.0f);
    CHECK(b <= 1.0f);
    CHECK(a < b); // monotone toward the white side
}

TEST_CASE("degenerate inputs: empty mask is a no-op, whole-image hole declines") {
    const ColorF bg{0.5f, 0.5f, 0.5f, 1.0f};
    const ImageF img = constantImage(4, 4, bg);

    const Selection none; // no coverage
    const inpaint::InpaintResult noop = runPde({img, none, {}});
    CHECK(noop.ok);
    CHECK(noop.image == img); // unchanged

    const Selection all = Selection::rectangle(4, 4, Rect{0, 0, 4, 4});
    const inpaint::InpaintResult full = runPde({img, all, {}});
    CHECK_FALSE(full.ok); // no boundary data to diffuse from
}

TEST_CASE("offset statistics: a periodic image's dominant offset is its tiling period") {
    // Stripes with period 6 in x; a small y-gradient so a pure VERTICAL shift mismatches (only a
    // horizontal shift by a multiple of 6 reproduces the image exactly). The dominant offset must
    // therefore be horizontal with |u| == 6.
    ImageF im(24, 8);
    const float base[6] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 24; ++x) {
            const float val = 0.5f * base[x % 6] + 0.5f * (static_cast<float>(y) / 7.0f);
            im.set(x, y, {val, val, val, 1.0f});
        }
    }
    inpaint::Params p;
    p.patchSize = 4;
    p.K = 8;
    const Selection none; // whole image is known
    const std::vector<inpaint::Offset> offs = inpaint::computeDominantOffsets(im, none, p);
    REQUIRE_FALSE(offs.empty());
    CHECK(offs.front().v == 0);
    CHECK(std::abs(offs.front().u) == 6);
}

TEST_CASE("applyOffsetLabels: filling a periodic image with its period offset reconstructs it") {
    // period-6 stripes (+ y gradient); a hole spanning x in [6,12) filled by copying from x-6
    // (a known, one-period-away region) must reproduce the original by periodicity.
    ImageF im(24, 8);
    const float base[6] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 24; ++x) {
            const float v = 0.5f * base[x % 6] + 0.5f * (static_cast<float>(y) / 7.0f);
            im.set(x, y, {v, v, v, 1.0f});
        }
    }
    const Selection hole = Selection::rectangle(24, 8, Rect{6, 0, 6, 8}); // x in [6,12)
    const std::vector<inpaint::Offset> offsets{{-6, 0}};
    const std::vector<int> labels(6 * 8, 0); // every hole pixel uses offset (-6,0)
    const ImageF out = inpaint::applyOffsetLabels(im, hole, offsets, labels);
    CHECK(out == im); // periodicity: x-6 reproduces x
}

TEST_CASE("offset statistics: degenerate inputs return empty") {
    const ImageF tiny = constantImage(2, 2, {0.3f, 0.3f, 0.3f, 1.0f});
    inpaint::Params p;
    p.patchSize = 4; // larger than the image
    const Selection none;
    CHECK(inpaint::computeDominantOffsets(tiny, none, p).empty());
}

TEST_CASE("working region: full 3x margin with adaptiveSmallRegion off") {
    const ImageF img = constantImage(100, 100, {0.5f, 0.5f, 0.5f, 1.0f});
    const Selection hole = Selection::rectangle(100, 100, Rect{10, 10, 4, 4});
    inpaint::Params p;
    p.adaptiveSmallRegion = false;
    const inpaint::WorkingRegion wr = inpaint::extractWorkingRegion(img, hole, p);
    CHECK(wr.regionW == 12); // 3 * 4
    CHECK(wr.regionH == 12);
    CHECK(wr.originX == 6); // centred on bbox centre (12,12), so 12-6
    CHECK(wr.originY == 6);
    CHECK(wr.scale == 1); // 12 px fits the 800px budget
    CHECK(wr.image.width == 12);
    CHECK(wr.image.height == 12);
}

TEST_CASE("working region: a small hole samples a tight neighbourhood with low-effort on (default)") {
    const ImageF img = constantImage(100, 100, {0.5f, 0.5f, 0.5f, 1.0f});
    const Selection hole = Selection::rectangle(100, 100, Rect{10, 10, 4, 4});
    // Default Params (adaptiveSmallRegion == true): a tiny hole (4% of the image) uses a margin near
    // the 1.4x floor, far smaller than the 3x above -> much less to analyse.
    const inpaint::WorkingRegion wr = inpaint::extractWorkingRegion(img, hole, {});
    CHECK(wr.regionW < 12);   // tighter than the full 3x margin
    CHECK(wr.regionW >= 4);   // never smaller than the hole itself
    CHECK(wr.regionH == wr.regionW);
    // A hole filling a third+ of the image grows back to the full 3x (full context for big removals).
    const Selection big = Selection::rectangle(100, 100, Rect{10, 10, 40, 40});
    const inpaint::WorkingRegion bw = inpaint::extractWorkingRegion(img, big, {});
    CHECK(bw.regionW == 100); // 40 * 3 = 120, clamped to the 100px image
}

TEST_CASE("workingRegionRect: globalSearchRegion makes the search domain INVARIANT to the selection") {
    // ⚠ The default window's SIZE is derived from the selection's SIZE and it SURROUNDS the
    // selection. `globalSearchRegion` makes the domain the whole image so that it stops depending
    // on the hole at all -- a load-bearing property, not a tuning knob. This test is the guardrail:
    // a bounded search window is the most obvious speedup for a slow matcher, so someone WILL
    // reintroduce one, and it must not silently become the only behaviour.
    inpaint::Params p;
    p.globalSearchRegion = true;
    const Rect small = inpaint::workingRegionRect(100, 80, Rect{10, 10, 4, 4}, p);
    const Rect large = inpaint::workingRegionRect(100, 80, Rect{10, 10, 40, 40}, p);
    const Rect none = inpaint::workingRegionRect(100, 80, std::nullopt, p);
    for (const Rect& r : {small, large, none}) {
        CHECK(r.x == 0);
        CHECK(r.y == 0);
        CHECK(r.w == 100); // the WHOLE image, whatever the hole is
        CHECK(r.h == 80);
    }
    // ...and it overrides the adaptive margin, which is otherwise a function of the hole's size.
    p.adaptiveSmallRegion = true;
    CHECK(inpaint::workingRegionRect(100, 80, Rect{10, 10, 4, 4}, p).w == 100);
}

TEST_CASE("workingRegionRect: an edge-touching hole shifts inward, keeping its size") {
    inpaint::Params p;
    p.adaptiveSmallRegion = false; // full 3x for a predictable size
    // 4x4 hole in the top-left corner -> a 12x12 region that can't extend past the origin: it shifts
    // so the lost left/top margin is recovered on the right/bottom (the in-bounds compensation).
    const Rect tl = inpaint::workingRegionRect(100, 100, Rect{0, 0, 4, 4}, p);
    CHECK(tl.w == 12);
    CHECK(tl.h == 12);
    CHECK(tl.x == 0);
    CHECK(tl.y == 0);
    // Right-edge hole: the window stays within [0, 100].
    const Rect re = inpaint::workingRegionRect(100, 100, Rect{96, 48, 4, 4}, p);
    CHECK(re.w == 12);
    CHECK(re.x + re.w <= 100.0);
    // No hole bounds (whole-image fill) -> the whole image.
    const Rect whole = inpaint::workingRegionRect(50, 40, std::nullopt, p);
    CHECK(whole.x == 0);
    CHECK(whole.y == 0);
    CHECK(whole.w == 50);
    CHECK(whole.h == 40);
}

TEST_CASE("working region: box-average downsample when the region exceeds the budget") {
    const ColorF bg{0.2f, 0.6f, 0.4f, 1.0f};
    const ImageF img = constantImage(8, 8, bg);
    inpaint::Params p;
    p.maxRegionW = 4; // force a 2x downsample of the whole-image region
    p.maxRegionH = 4;
    const Selection none; // whole image is the region
    const inpaint::WorkingRegion wr = inpaint::extractWorkingRegion(img, none, p);
    CHECK(wr.scale == 2);
    CHECK(wr.image.width == 4);
    CHECK(wr.image.height == 4);
    const ColorF c = wr.image.at(0, 0); // average of a constant block == the constant
    CHECK(c.r == doctest::Approx(bg.r));
    CHECK(c.g == doctest::Approx(bg.g));
    CHECK(c.b == doctest::Approx(bg.b));
}

TEST_CASE("max-flow: independent terminal links sum to the per-node min") {
    inpaint::MaxFlowGraph g(2);
    g.addTermWeights(0, 3.0, 5.0); // min(3,5) = 3
    g.addTermWeights(1, 4.0, 2.0); // min(4,2) = 2
    CHECK(g.maxflow() == doctest::Approx(5.0));
}

TEST_CASE("max-flow: an n-link bottlenecks a source-sink chain") {
    inpaint::MaxFlowGraph g(2);
    g.addTermWeights(0, 10.0, 0.0); // node 0 tied to the source
    g.addTermWeights(1, 0.0, 10.0); // node 1 tied to the sink
    g.addEdge(0, 1, 3.0, 3.0);      // the only path crosses this edge
    CHECK(g.maxflow() == doctest::Approx(3.0));
}

TEST_CASE("max-flow: min-cut side query after solving") {
    inpaint::MaxFlowGraph g(2);
    g.addTermWeights(0, 10.0, 0.0);
    g.addTermWeights(1, 0.0, 10.0);
    g.addEdge(0, 1, 3.0, 3.0);
    g.maxflow();
    CHECK(g.inSourceSet(0));       // node 0 stays with the source
    CHECK_FALSE(g.inSourceSet(1)); // node 1 with the sink
}

// ⭐ THE INVARIANT THE WHOLE SOLVER RESTS ON, pinned against ground truth rather than against the
// previous implementation. α-expansion never reads the flow VALUE, only the cut, so the solver is
// free to find a different maximum flow — a different active-node order, a warm-started preflow, a
// cheaper gap heuristic — provided the CUT IT REPORTS DOES NOT MOVE. That is guaranteed by a
// theorem, not by luck: the nodes reachable from the source in the residual graph of ANY maximum
// flow are exactly the unique MINIMAL minimum cut (a residual-reachable node must lie on the source
// side of every minimum cut, or its augmenting path would cross that cut with spare capacity).
//
// This exhaustively checks that claim on random small graphs: brute-force every one of the 2^n
// source/sink assignments, take the minimum cut value, intersect the source sides of ALL cuts that
// achieve it, and require inSourceSet to report exactly that intersection. Any future change to the
// max-flow — including flow reuse across expansion moves — is correct if and only if it keeps this
// green, and a solver that merely reproduced today's bytes would not be proof of anything.
TEST_CASE("max-flow: the reported cut is the unique MINIMAL min cut, on random graphs") {
    constexpr int kN = 6;
    std::uint64_t rng = 0x9E3779B97F4A7C15ULL;
    const auto next = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    const auto cap = [&next]() { return static_cast<double>(next() % 11U); }; // 0..10, ties likely

    for (int trial = 0; trial < 300; ++trial) {
        std::vector<double> cs(kN), ct(kN);
        std::vector<std::array<double, 2>> ecap; // (i,j) pairs in a fixed order
        std::vector<std::pair<int, int>> epair;
        inpaint::MaxFlowGraph g(kN);
        for (int i = 0; i < kN; ++i) {
            cs[static_cast<std::size_t>(i)] = cap();
            ct[static_cast<std::size_t>(i)] = cap();
            g.addTermWeights(i, cs[static_cast<std::size_t>(i)], ct[static_cast<std::size_t>(i)]);
        }
        for (int i = 0; i < kN; ++i) {
            for (int j = i + 1; j < kN; ++j) {
                if (next() % 3U == 0U) {
                    continue; // sparsify
                }
                const double a = cap();
                const double b = cap();
                epair.emplace_back(i, j);
                ecap.push_back({a, b});
                g.addEdge(i, j, a, b);
            }
        }
        g.maxflow();

        // Brute force: bit k of `m` set == node k on the SINK side.
        const auto cutValue = [&](unsigned m) {
            double v = 0.0;
            for (int i = 0; i < kN; ++i) {
                const bool sinkSide = ((m >> static_cast<unsigned>(i)) & 1U) != 0U;
                v += sinkSide ? cs[static_cast<std::size_t>(i)] : ct[static_cast<std::size_t>(i)];
            }
            for (std::size_t e = 0; e < epair.size(); ++e) {
                const bool aSink = ((m >> static_cast<unsigned>(epair[e].first)) & 1U) != 0U;
                const bool bSink = ((m >> static_cast<unsigned>(epair[e].second)) & 1U) != 0U;
                if (!aSink && bSink) {
                    v += ecap[e][0];
                } else if (aSink && !bSink) {
                    v += ecap[e][1];
                }
            }
            return v;
        };
        double best = cutValue(0);
        for (unsigned m = 1; m < (1U << kN); ++m) {
            best = std::min(best, cutValue(m));
        }
        // Intersection of the source sides of every minimum cut.
        unsigned alwaysSource = (1U << kN) - 1U;
        for (unsigned m = 0; m < (1U << kN); ++m) {
            if (cutValue(m) <= best + 1e-9) {
                alwaysSource &= ~m; // nodes on the SINK side here are not always-source
            }
        }
        for (int i = 0; i < kN; ++i) {
            const bool expected = ((alwaysSource >> static_cast<unsigned>(i)) & 1U) != 0U;
            REQUIRE(g.inSourceSet(i) == expected);
        }
    }
}

TEST_CASE("alpha-expansion: no smoothness picks each node's argmin data label") {
    auto data = [](int n, int l) -> double {
        static const double d[2][2] = {{1.0, 5.0}, {7.0, 2.0}};
        return d[n][l];
    };
    auto smooth = [](int a, int b) { return a == b ? 0.0 : 1.0; };
    const std::vector<int> labels = inpaint::alphaExpansion(2, 2, data, smooth, {});
    REQUIRE(labels.size() == 2);
    CHECK(labels[0] == 0);
    CHECK(labels[1] == 1);
}

TEST_CASE("alpha-expansion: strong smoothness forces neighbours to agree") {
    auto data = [](int n, int l) -> double {
        return n == 0 ? (l == 0 ? 0.0 : 4.0) : (l == 1 ? 0.0 : 4.0);
    };
    auto smooth = [](int a, int b) { return a == b ? 0.0 : 1.0; };
    const std::vector<inpaint::GraphCutEdge> edges{{0, 1, 10.0}};
    const std::vector<int> L = inpaint::alphaExpansion(2, 2, data, smooth, edges);
    CHECK(L[0] == L[1]); // the weight-10 seam dominates the weight-4 data preference
    const double e = data(0, L[0]) + data(1, L[1]) + 10.0 * smooth(L[0], L[1]);
    CHECK(e == doctest::Approx(4.0)); // global optimum
}

TEST_CASE("alpha-expansion: a 3-node line settles to a single boundary") {
    auto data = [](int n, int l) -> double {
        if (n == 0)
            return l == 0 ? 0.0 : 10.0; // node 0 strongly label 0
        if (n == 2)
            return l == 1 ? 0.0 : 10.0; // node 2 strongly label 1
        return 0.0;                     // node 1 neutral
    };
    auto smooth = [](int a, int b) { return a == b ? 0.0 : 1.0; };
    const std::vector<inpaint::GraphCutEdge> edges{{0, 1, 5.0}, {1, 2, 5.0}};
    const std::vector<int> L = inpaint::alphaExpansion(3, 2, data, smooth, edges);
    CHECK(L[0] == 0);
    CHECK(L[2] == 1);
    const double e = data(0, L[0]) + data(1, L[1]) + data(2, L[2]) + 5.0 * smooth(L[0], L[1]) +
                     5.0 * smooth(L[1], L[2]);
    CHECK(e == doctest::Approx(5.0)); // exactly one boundary of weight 5
}

TEST_CASE("graphComplete: a periodic image's hole is reconstructed exactly") {
    const ImageF im = periodicImage();
    const Selection hole = Selection::rectangle(24, 8, Rect{9, 0, 6, 8}); // x in [9,15)
    inpaint::Params p;
    p.patchSize = 4;
    p.K = 8;
    const std::vector<inpaint::Offset> offs = inpaint::computeDominantOffsets(im, hole, p);
    REQUIRE_FALSE(offs.empty());
    const ImageF out = inpaint::graphComplete(im, hole, offs, p); // Poisson on by default
    CHECK(imagesApproxEqual(out, im, 1e-3f)); // period offset + identity-on-seamless Poisson
}

TEST_CASE("graphComplete: Poisson blend interpolates a gradient the raw copy cannot") {
    // 20x1: left half 0, right half 1. Two offsets reach the 0 side and the 1 side; the graph cut
    // avoids a costly seam by copying flatly from one side, so the raw composite is uniform. Plain
    // Poisson (Laplace, flat guidance) then interpolates a monotonic ramp between the known
    // boundaries (x=7 -> 0, x=12 -> 1) — exactly the gradient-domain fill the copy can't produce.
    // 20 wide so the 4-cell hole stays under the outpaint gate (a 1px-tall image is all frame
    // perimeter): this test pins the INTERIOR blend, where the §3.7.8 boundary-crisp rule — which
    // would deliberately keep the copy's flat fill crisp — must stay off.
    ImageF im(20, 1);
    for (std::uint32_t x = 0; x < 20; ++x) {
        const float v = x < 10 ? 0.0f : 1.0f;
        im.set(x, 0, {v, 0.0f, 0.0f, 1.0f});
    }
    const Selection hole = Selection::rectangle(20, 1, Rect{8, 0, 4, 1}); // x in {8,9,10,11}
    REQUIRE_FALSE(inpaint::isOutpaintHole(hole, 20, 1));
    const std::vector<inpaint::Offset> offsets{{-4, 0}, {4, 0}};

    inpaint::Params p;
    p.poissonBlend = false;
    const ImageF raw = inpaint::graphComplete(im, hole, offsets, p);
    CHECK(std::fabs(raw.at(8, 0).r - raw.at(11, 0).r) < 1e-6f); // uniform copy (seam avoided)

    p.poissonBlend = true;
    const ImageF blended = inpaint::graphComplete(im, hole, offsets, p);
    CHECK(blended.at(8, 0).r > 0.01f); // strictly inside the [0,1] boundary
    CHECK(blended.at(11, 0).r < 0.99f);
    CHECK(blended.at(8, 0).r < blended.at(9, 0).r); // monotonic ramp from the Poisson solve
    CHECK(blended.at(9, 0).r < blended.at(10, 0).r);
    CHECK(blended.at(10, 0).r < blended.at(11, 0).r);
}

TEST_CASE("graphComplete: two-scale (coarse graph cut) still reconstructs the periodic hole") {
    const ImageF im = periodicImage();
    const Selection hole = Selection::rectangle(24, 8, Rect{9, 0, 6, 8});
    inpaint::Params p;
    p.patchSize = 4;
    p.K = 8;
    p.twoScaleFactor = 2; // force the coarse-then-full-res path
    const std::vector<inpaint::Offset> offs = inpaint::computeDominantOffsets(im, hole, p);
    REQUIRE_FALSE(offs.empty());
    const ImageF out = inpaint::graphComplete(im, hole, offs, p);
    CHECK(imagesApproxEqual(out, im, 1e-3f)); // coarse offset upsamples to the period; reconstructs
}

TEST_CASE("graphComplete: a straight horizon through the hole continues at its true height") {
    // The user-visible edge-alignment property (2026-07-02): a strong horizontal edge crossing
    // a hole must come out at the SAME row, not stepped or pushed to the hole boundary. Both
    // halves are period-6 striped, so horizontal shifts (which preserve the edge height) are
    // available as dominant offsets; the seam objective (colours & gradients + the E1 boundary
    // anchoring) must prefer them over anything that moves the horizon.
    ImageF im(36, 16);
    const float stripe[6] = {0.0f, 0.02f, 0.04f, 0.0f, 0.02f, 0.04f};
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 36; ++x) {
            const float base = y < 8 ? 0.15f : 0.85f; // sky above, sea below, edge at y=8
            const float v = base + stripe[x % 6];
            im.set(x, y, {v, v, v, 1.0f});
        }
    }
    const Selection hole = Selection::rectangle(36, 16, Rect{12, 4, 12, 8}); // crosses the edge
    inpaint::Params p;
    p.patchSize = 4;
    p.K = 8;
    const std::vector<inpaint::Offset> offs = inpaint::computeDominantOffsets(im, hole, p);
    REQUIRE_FALSE(offs.empty());
    const ImageF out = inpaint::graphComplete(im, hole, offs, p);
    // Allow the Poisson blend up to ±1 px of softness at the edge itself, but rows 6 and 9 must
    // be unambiguously sky and sea — a horizon shifted by 2+ px (the reported artifact) fails.
    for (std::uint32_t x = 12; x < 24; ++x) {
        CHECK(out.at(x, 6).r < 0.5f);
        CHECK(out.at(x, 9).r > 0.5f);
    }
    // And no lone step: the row just above the hole's top edge stayed sky, just below stayed sea
    // (the fill may not push the transition onto the hole boundary rings either).
    for (std::uint32_t x = 12; x < 24; ++x) {
        CHECK(out.at(x, 4).r < 0.5f);
        CHECK(out.at(x, 11).r > 0.5f);
    }
}

TEST_CASE("graphComplete: removed content never echoes — deep chains resolve to known pixels") {
    // The "removing the tower rebuilt a slimmer tower" bug (user 2026-07-02): with a hole wider
    // than any offset, deep-interior pixels get labels whose sources land back IN the hole, and
    // synthesis used to copy the ORIGINAL (removed) content there — the object came back as a
    // shifted echo. Chains must now resolve to known content: a loud red column hidden inside
    // the hole must leave NO trace, and the periodic background must reconstruct exactly.
    ImageF im(40, 8);
    const float base[4] = {0.1f, 0.3f, 0.5f, 0.7f};
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 40; ++x) {
            const float v = base[x % 4];
            im.set(x, y, {v, v, 0.2f, 1.0f});
        }
    ImageF expected = im; // the pure periodic field: what a perfect removal reconstructs
    for (std::uint32_t y = 1; y < 7; ++y) // the "object": a red block deep inside the hole
        for (std::uint32_t x = 18; x < 22; ++x)
            im.set(x, y, {1.0f, 0.0f, 0.0f, 1.0f});
    const Selection hole = Selection::rectangle(40, 8, Rect{10, 0, 20, 8}); // x in [10,30)
    const std::vector<inpaint::Offset> offsets{{-4, 0}, {4, 0}}; // shorter than the hole: chains
    inpaint::Params p;
    p.poissonBlend = false; // crisp copies so the assert is exact
    const ImageF out = inpaint::graphComplete(im, hole, offsets, p);
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 40; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            const ColorF c = out.at(x, y);
            CHECK(c.r < 0.9f); // no red echo anywhere
            // And the fill is the exact periodic continuation (offsets are period multiples).
            const ColorF e = expected.at(x, y);
            CHECK(c.r == doctest::Approx(e.r));
            CHECK(c.g == doctest::Approx(e.g));
        }
}

TEST_CASE("graphComplete: multigrid converges a wide hole's blend (no residual banding)") {
    // On a hole hundreds of pixels across, fixed-sweep relaxation leaves the low frequencies
    // unsolved — offset sheets keep slightly wrong DC levels (the user's "banded sky"). Fixture:
    // left half 0, right half 1, a 200x120 hole (over the 20k multigrid threshold) spanning the
    // boundary, and offsets long enough that BOTH are valid for every hole pixel (no forced
    // content). The cut places one seam; its 0|1 step must diffuse into a smooth ramp across
    // the hole — a ~200-px low-frequency profile a few dozen polish sweeps cannot build, so
    // this discriminates the multigrid's convergence.
    ImageF im(700, 160);
    for (std::uint32_t y = 0; y < 160; ++y)
        for (std::uint32_t x = 0; x < 700; ++x) {
            const float v = x < 350 ? 0.0f : 1.0f;
            im.set(x, y, {v, v, v, 1.0f});
        }
    const Selection hole = Selection::rectangle(700, 160, Rect{250, 20, 200, 120});
    const std::vector<inpaint::Offset> offsets{{-250, 0}, {250, 0}};
    inpaint::Params p; // defaults: Poisson on, 200 iterations (capped after the multigrid)
    const ImageF out = inpaint::graphComplete(im, hole, offsets, p);
    const std::uint32_t midY = 80;
    float prev = -1.0f;
    for (std::uint32_t x = 255; x < 445; x += 10) {
        const float v = out.at(x, midY).r;
        CHECK(v >= prev - 0.02f); // monotone (small tolerance for relaxation noise)
        prev = std::max(prev, v);
    }
    const float mid = out.at(350, midY).r;
    CHECK(mid > 0.25f); // the seam step spread across the hole, not parked in place
    CHECK(mid < 0.75f);
    CHECK(out.at(260, midY).r < 0.35f); // still anchored to the known sides
    CHECK(out.at(440, midY).r > 0.65f);
}

TEST_CASE("resynth backend: registered, default unchanged, exact on constant fields") {
    inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    const auto ids = eng.backendIds();
    CHECK(std::find(ids.begin(), ids.end(), "resynth") != ids.end());
    CHECK(eng.activeBackend() == "offset-stats"); // adding a choice must not change the default
    REQUIRE(eng.setActiveBackend("resynth"));

    // Constant field: every donor is the constant, so the fill is exactly the constant.
    const ColorF bg{0.3f, 0.5f, 0.7f, 1.0f};
    const ImageF img = constantImage(24, 24, bg);
    const Selection hole = Selection::rectangle(24, 24, Rect{9, 9, 6, 6});
    const inpaint::InpaintResult r = eng.run({img, hole, {}});
    REQUIRE(r.ok);
    for (std::uint32_t y = 0; y < 24; ++y)
        for (std::uint32_t x = 0; x < 24; ++x) {
            const ColorF c = r.image.at(x, y);
            CHECK(c.r == doctest::Approx(bg.r));
            CHECK(c.g == doctest::Approx(bg.g));
            CHECK(c.b == doctest::Approx(bg.b));
        }

    // A pre-raised cancel token declines instead of filling.
    std::atomic<bool> cancel{true};
    const inpaint::InpaintResult c = eng.run({img, hole, {}, &cancel});
    CHECK_FALSE(c.ok);
    CHECK(c.detail == "cancelled");
}

TEST_CASE("resynth backend: deterministic, copies only known content, never the removed pixels") {
    inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    REQUIRE(eng.setActiveBackend("resynth"));

    // Textured fixture with POISON inside the hole: pure red must never reappear (synthesis
    // reads the corpus only at known pixels), every filled pixel must be a verbatim copy of a
    // known pixel, and two runs must agree bit-for-bit (fixed-seed sampling).
    ImageF im(32, 16);
    const float base[5] = {0.1f, 0.3f, 0.5f, 0.7f, 0.9f};
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 32; ++x) {
            const float v = 0.7f * base[(x + 2 * y) % 5] + 0.3f * (static_cast<float>(y) / 15.0f);
            im.set(x, y, {v, v * 0.8f, v * 0.6f, 1.0f});
        }
    const Selection hole = Selection::rectangle(32, 16, Rect{12, 5, 8, 6});
    for (std::uint32_t y = 5; y < 11; ++y)
        for (std::uint32_t x = 12; x < 20; ++x)
            im.set(x, y, {1.0f, 0.0f, 0.0f, 1.0f}); // the removed content

    std::vector<std::array<float, 3>> known;
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 32; ++x)
            if (!(x >= 12 && x < 20 && y >= 5 && y < 11)) {
                const ColorF c = im.at(x, y);
                known.push_back({c.r, c.g, c.b});
            }

    const inpaint::InpaintResult r1 = eng.run({im, hole, {}});
    const inpaint::InpaintResult r2 = eng.run({im, hole, {}});
    REQUIRE(r1.ok);
    REQUIRE(r2.ok);
    CHECK(r1.image == r2.image); // determinism, bit-for-bit
    for (std::uint32_t y = 5; y < 11; ++y)
        for (std::uint32_t x = 12; x < 20; ++x) {
            const ColorF c = r1.image.at(x, y);
            CAPTURE(x);
            CAPTURE(y);
            CHECK(c.r < 0.99f); // no poison echo
            const bool isKnownValue =
                std::find(known.begin(), known.end(),
                          std::array<float, 3>{c.r, c.g, c.b}) != known.end();
            CHECK(isKnownValue); // verbatim copy of known content
        }
}

TEST_CASE("computeBoundaryOffsets: finds the continuation the frequency vote misses") {
    // The "treeline junction" limitation (docs §3.7.7): a structure ends at the hole and its one
    // continuation segment exists elsewhere, but the offset mapping it there is globally rare, so
    // dominant-offset voting never fields it. Fixture: period-5 stripes (dominant self-similarity
    // = horizontal multiples of 5), plus a distinctive pattern P touching the hole's left edge
    // whose ONLY copy sits at offset (+32, 0) — not a stripe-period multiple. Boundary matching
    // must surface (32, 0); the dominant vote (K = 2) must not (that is what makes the fixture
    // meaningful).
    ImageF im(64, 24);
    const float base[5] = {0.1f, 0.3f, 0.5f, 0.7f, 0.9f};
    for (std::uint32_t y = 0; y < 24; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            const float v = 0.8f * base[x % 5] + 0.2f * (static_cast<float>(y) / 23.0f);
            im.set(x, y, {v, v, v, 1.0f});
        }
    const auto pat = [](std::uint32_t lx, std::uint32_t y) {
        return 0.05f + 0.09f * static_cast<float>((3 * lx + 7 * y) % 11);
    };
    for (std::uint32_t y = 6; y < 18; ++y) {
        for (std::uint32_t x = 16; x < 24; ++x)
            im.set(x, y, {pat(x - 16, y), 0.6f, pat(x - 16, y), 1.0f}); // P, touching the hole
        for (std::uint32_t x = 48; x < 56; ++x)
            im.set(x, y, {pat(x - 48, y), 0.6f, pat(x - 48, y), 1.0f}); // its only copy: +32
    }
    const Selection hole = Selection::rectangle(64, 24, Rect{24, 0, 16, 24});
    inpaint::Params p;
    p.patchSize = 4;
    p.K = 2; // boundaryOffsets stays at its shipping default
    const std::vector<inpaint::Offset> dominant = inpaint::computeDominantOffsets(im, hole, p);
    CHECK(std::find(dominant.begin(), dominant.end(), inpaint::Offset{32, 0}) == dominant.end());
    const std::vector<inpaint::Offset> boundary = inpaint::computeBoundaryOffsets(im, hole, p);
    REQUIRE_FALSE(boundary.empty());
    CHECK(std::find(boundary.begin(), boundary.end(), inpaint::Offset{32, 0}) != boundary.end());

    // boundaryOffsets == 0 disables the pass entirely.
    p.boundaryOffsets = 0;
    CHECK(inpaint::computeBoundaryOffsets(im, hole, p).empty());
}

TEST_CASE("refineOffsetsFullRes: a quantized offset snaps back to the true period") {
    // Offsets gathered on a downsampled working region are multiples of the region scale — up to
    // ±scale/2 off the true self-similarity, which misaligns structure carried across the hole.
    // Full-res refinement must snap a candidate back: period-5 stripes (plus a y-gradient so no
    // vertical shift is exact), a candidate (4,0) as if quantized by a scale-2 region, radius 2
    // -> the true (5,0).
    ImageF im(40, 12);
    const float base[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (std::uint32_t y = 0; y < 12; ++y)
        for (std::uint32_t x = 0; x < 40; ++x) {
            const float v = 0.7f * base[x % 5] + 0.3f * (static_cast<float>(y) / 11.0f);
            im.set(x, y, {v, v, v, 1.0f});
        }
    const Selection hole = Selection::rectangle(40, 12, Rect{16, 2, 8, 8});
    const std::vector<inpaint::Offset> quantized{{4, 0}};
    const std::vector<inpaint::Offset> refined =
        inpaint::refineOffsetsFullRes(im, hole, quantized, 2, 4);
    REQUIRE(refined.size() == 1);
    CHECK(refined[0].u == 5);
    CHECK(refined[0].v == 0);

    // radius 0 is a strict passthrough.
    const std::vector<inpaint::Offset> pass =
        inpaint::refineOffsetsFullRes(im, hole, quantized, 0, 4);
    CHECK(pass == quantized);

    // Two candidates snapping to the same true offset dedup to one (first wins, order kept).
    const std::vector<inpaint::Offset> both =
        inpaint::refineOffsetsFullRes(im, hole, {{4, 0}, {6, 0}}, 2, 4);
    REQUIRE(both.size() == 1);
    CHECK(both[0].u == 5);
}

TEST_CASE("resolveEffectiveOffsets: a cycled chain inherits a neighbour sheet, not diffusion") {
    // The "strips in the bottom right" bug (user 2026-07-02): where a hole meets the FRAME edge,
    // copy chains can loop (two sheets pointing into each other) and the failed pixels fell to
    // neighbour-fill diffusion — smeared streaks instead of texture. A failed chain must instead
    // inherit a resolved neighbour's effective offset when that offset is valid from its own
    // position. Hand-crafted 10x1 field: nodes at x=2..7; x=4 and x=6 point at each other
    // (2-cycle), everyone else resolves directly.
    const long W = 10, H = 1;
    const Selection hole = Selection::rectangle(10, 1, Rect{2, 0, 6, 1});
    std::vector<int> nodeOf(10, -1);
    for (int x = 2; x < 8; ++x)
        nodeOf[static_cast<std::size_t>(x)] = x - 2;
    const std::vector<inpaint::Offset> offsets{{-2, 0}, {2, 0}, {4, 0}}; // A, B, C
    //                        x=2 A  x=3 A  x=4 B  x=5 C  x=6 A  x=7 B
    const std::vector<int> labels{0, 0, 1, 2, 0, 1};
    // Chains: x=2 -> 0 known; x=3 -> 1 known; x=5 -> 9 known; x=7 -> 9 known.
    // x=4 -(B)-> x=6 -(A)-> x=4: a cycle. Inheritance: x=4 takes its W neighbour's (x=3, eff -2,
    // endpoint 2 = still in the hole -> invalid) then its E neighbour's (x=5, eff +4, endpoint
    // 8 = known, valid); x=6 takes x=5's +4 -> endpoint 10 out of bounds -> invalid, then x=7's
    // +2 -> endpoint 8 known, valid.
    const std::vector<inpaint::Offset> eff =
        inpaint::resolveEffectiveOffsets(hole, nodeOf, W, H, offsets, labels);
    REQUIRE(eff.size() == 6);
    constexpr int kNoSrc = std::numeric_limits<int>::min();
    for (const inpaint::Offset& o : eff)
        CHECK(o.u != kNoSrc); // nothing is left to diffusion
    CHECK(eff[0].u == -2);
    CHECK(eff[1].u == -2);
    CHECK(eff[3].u == 4);
    CHECK(eff[5].u == 2);
    CHECK(eff[2].u == 4); // the cycle pixels inherited their neighbours' sheets
    CHECK(eff[4].u == 2);
    for (const inpaint::Offset& o : eff)
        CHECK(o.v == 0);
}

TEST_CASE("graphComplete: reconstructs the periodic hole with Poisson blending disabled too") {
    const ImageF im = periodicImage();
    const Selection hole = Selection::rectangle(24, 8, Rect{9, 0, 6, 8});
    inpaint::Params p;
    p.patchSize = 4;
    p.K = 8;
    p.poissonBlend = false;
    const std::vector<inpaint::Offset> offs = inpaint::computeDominantOffsets(im, hole, p);
    REQUIRE_FALSE(offs.empty());
    CHECK(inpaint::graphComplete(im, hole, offs, p) == im);
}

TEST_CASE("offset-stats backend: registered, available, and reconstructs a periodic hole") {
    inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    const auto ids = eng.backendIds();
    CHECK(std::find(ids.begin(), ids.end(), "offset-stats") != ids.end());
    CHECK(eng.activeBackend() == "offset-stats"); // now the engine default

    const ImageF im = periodicImage();
    const Selection hole = Selection::rectangle(24, 8, Rect{9, 0, 6, 8});
    inpaint::Params p;
    p.patchSize = 4;
    p.K = 8;
    REQUIRE(eng.setActiveBackend("offset-stats"));
    const inpaint::InpaintResult r = eng.run({im, hole, p});
    REQUIRE(r.ok);
    CHECK(imagesApproxEqual(r.image, im, 1e-3f));
}

TEST_CASE("offset-stats backend: hole in a larger image (offsets gathered from the local region)") {
    // 48-wide period-6 image, hole in the middle: the working-region crop gathers offsets locally,
    // and the full-resolution fill still reconstructs it.
    ImageF im(48, 8);
    const float base[6] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 48; ++x) {
            const float v = 0.5f * base[x % 6] + 0.5f * (static_cast<float>(y) / 7.0f);
            im.set(x, y, {v, v, v, 1.0f});
        }
    }
    const Selection hole = Selection::rectangle(48, 8, Rect{18, 0, 6, 8});
    inpaint::Params p;
    p.patchSize = 4;
    p.K = 8;
    inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    REQUIRE(eng.setActiveBackend("offset-stats"));
    const inpaint::InpaintResult r = eng.run({im, hole, p});
    REQUIRE(r.ok);
    CHECK(imagesApproxEqual(r.image, im, 1e-3f));
}

TEST_CASE("script backend is inert until a provider is registered (S40 hook)") {
    inpaint::ScriptBackend sb;
    CHECK_FALSE(sb.hasProvider());

    const ImageF img = constantImage(4, 4, {0.1f, 0.1f, 0.1f, 1.0f});
    const Selection hole = Selection::rectangle(4, 4, Rect{1, 1, 2, 2});
    const inpaint::InpaintRequest req{img, hole, {}};

    const inpaint::InpaintResult before = sb.run(req, {});
    CHECK_FALSE(before.ok); // no provider yet

    sb.setProvider([](const inpaint::InpaintRequest& r, const inpaint::ProgressFn&) {
        inpaint::InpaintResult out;
        out.image = r.image;
        out.ok = true;
        out.detail = "stub-provider";
        return out;
    });
    CHECK(sb.hasProvider());
    const inpaint::InpaintResult after = sb.run(req, {});
    CHECK(after.ok);
    CHECK(after.detail == "stub-provider");
}

// ---- Settings → Inpainting: backend self-description + schema (the Settings category) --------

TEST_CASE("backend self-description: each built-in advertises a display name + a cost line") {
    const inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    for (const char* id : {"offset-stats", "pde"}) {
        const auto* b = eng.backend(id);
        REQUIRE(b != nullptr);
        const inpaint::BackendInfo info = b->info();
        CHECK_FALSE(info.displayName.empty());
        CHECK_FALSE(info.method.empty());
        CHECK_FALSE(info.summary.empty());
        CHECK_FALSE(info.cost.empty());
    }
}

TEST_CASE("availability: built-ins are selectable; the script backend hides until a provider lands") {
    const inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    CHECK(eng.backend("offset-stats")->available());
    CHECK(eng.backend("pde")->available());
    CHECK_FALSE(eng.backend("script")->available()); // no Lua provider yet (S40)

    inpaint::ScriptBackend sb;
    CHECK_FALSE(sb.available());
    sb.setProvider([](const inpaint::InpaintRequest& r, const inpaint::ProgressFn&) {
        return inpaint::InpaintResult{r.image, true, "stub"};
    });
    CHECK(sb.available());
}

TEST_CASE("offset-stats schema: presets exist and the default is one of them") {
    const inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    const inpaint::BackendSettingsSchema schema = eng.backend("offset-stats")->settingsSchema();
    CHECK_FALSE(schema.controls.empty());
    CHECK(schema.presets.size() == 3); // fast / balanced / best
    CHECK(schema.defaultPreset == "balanced");
    const bool defaultIsAPreset =
        std::any_of(schema.presets.begin(), schema.presets.end(),
                    [&](const inpaint::PresetSpec& p) { return p.id == schema.defaultPreset; });
    CHECK(defaultIsAPreset);
}

TEST_CASE("paramsForPreset: offset-stats presets trade quality for speed") {
    const inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    const auto* b = eng.backend("offset-stats");
    REQUIRE(b != nullptr);

    const inpaint::Params fast = inpaint::paramsForPreset(*b, "fast");
    const inpaint::Params balanced = inpaint::paramsForPreset(*b, "balanced");
    const inpaint::Params best = inpaint::paramsForPreset(*b, "best");

    // Balanced mirrors the curated control defaults (= the Params defaults the engine ships with).
    CHECK(balanced.K == 60);
    CHECK(balanced.poissonIterations == 200);
    CHECK(balanced.graphCutMaxNodes == 6000);

    // Monotonic across presets: fast is cheaper, best is richer.
    CHECK(fast.K < balanced.K);
    CHECK(balanced.K < best.K);
    CHECK(fast.graphCutMaxNodes < best.graphCutMaxNodes);
    CHECK(fast.poissonIterations < best.poissonIterations);

    // The single "Working-region cap" knob drives both region axes.
    CHECK(best.maxRegionW == 1200);
    CHECK(best.maxRegionW == best.maxRegionH);
}

TEST_CASE("paramsForPreset: a preset-less backend (pde) falls back to its control defaults") {
    const inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    const auto* b = eng.backend("pde");
    REQUIRE(b != nullptr);
    CHECK(b->settingsSchema().presets.empty());
    // Any preset id (even a foreign one) resolves to the control defaults for a backend with none.
    const inpaint::Params p = inpaint::paramsForPreset(*b, "balanced");
    CHECK(p.pdeIterations == 800);
    CHECK(p.pdeEpsilon == doctest::Approx(1e-4));
}

TEST_CASE("applyParam ignores keys a backend does not own") {
    const inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    inpaint::Params p; // defaults
    const inpaint::Params before = p;
    eng.backend("pde")->applyParam(p, "K", 999);           // an offset-stats key
    eng.backend("offset-stats")->applyParam(p, "pdeIterations", 5); // a pde key
    CHECK(p.K == before.K);
    CHECK(p.pdeIterations == before.pdeIterations);
}

// ---------------------------------------------------------------------------------------------
// Outpaint gate (§3.7.8): the expansion-ring detection the engines' outpaint tuning hinges on.
// Interior heals must NEVER trip it — their output is contractually byte-identical to the
// pre-tuning engine.
// ---------------------------------------------------------------------------------------------
TEST_CASE("outpaint gate: rings trip it, interior and edge-touching heals do not") {
    using mosaic::core::Selection;
    using namespace mosaic::core::inpaint;
    // A full expansion ring: everything outside a centred 60x40 old canvas on a 100x80 new one.
    Selection ring(100, 80);
    for (std::uint32_t y = 0; y < 80; ++y)
        for (std::uint32_t x = 0; x < 100; ++x)
            if (x < 20 || x >= 80 || y < 20 || y >= 60)
                ring.data()[y * 100 + x] = 255;
    CHECK(holeFrameFraction(ring, 100, 80) == doctest::Approx(1.0));
    CHECK(isOutpaintHole(ring, 100, 80));

    // A one-sided strip (right 20 columns): more than a side of the frame -> outpaint.
    Selection strip(100, 80);
    for (std::uint32_t y = 0; y < 80; ++y)
        for (std::uint32_t x = 80; x < 100; ++x)
            strip.data()[y * 100 + x] = 255;
    CHECK(isOutpaintHole(strip, 100, 80));

    // An interior heal: never.
    Selection interior(100, 80);
    for (std::uint32_t y = 30; y < 50; ++y)
        for (std::uint32_t x = 40; x < 60; ++x)
            interior.data()[y * 100 + x] = 255;
    CHECK(holeFrameFraction(interior, 100, 80) == doctest::Approx(0.0));
    CHECK_FALSE(isOutpaintHole(interior, 100, 80));

    // A small heal that merely touches one edge: well under the gate.
    Selection touching(100, 80);
    for (std::uint32_t y = 0; y < 12; ++y)
        for (std::uint32_t x = 40; x < 60; ++x)
            touching.data()[y * 100 + x] = 255;
    CHECK(holeFrameFraction(touching, 100, 80) < 0.1);
    CHECK_FALSE(isOutpaintHole(touching, 100, 80));
}

// ---------------------------------------------------------------------------------------------
// Outpaint shift-candidate ladder (§3.7.8): axis rungs per strip side, diagonal rungs per strip
// corner, degenerate guards. Pure candidate generation — asserted directly.
// ---------------------------------------------------------------------------------------------
TEST_CASE("outpaint shift candidates: one-sided strip gets axis rungs only") {
    using namespace mosaic::core::inpaint;
    // Right 24-column strip on 100x80.
    Selection strip(100, 80);
    for (std::uint32_t y = 0; y < 80; ++y)
        for (std::uint32_t x = 76; x < 100; ++x)
            strip.data()[y * 100 + x] = 255;
    const std::vector<Offset> cands = outpaintShiftCandidates(strip, 8);
    REQUIRE_FALSE(cands.empty());
    // First rung clears the strip depth by pad: (-(24+8), 0); every rung is a leftward
    // axis-aligned shift (u < 0, v == 0) — a one-sided strip must produce no diagonals.
    CHECK(std::find(cands.begin(), cands.end(), Offset{-32, 0}) != cands.end());
    for (const Offset& o : cands) {
        CHECK(o.u < 0);
        CHECK(o.v == 0);
    }
}

TEST_CASE("outpaint shift candidates: two adjacent strips add diagonal corner rungs") {
    using namespace mosaic::core::inpaint;
    // Right 24-column strip + bottom 16-row strip on 100x80 (an expand right+down).
    Selection lShape(100, 80);
    for (std::uint32_t y = 0; y < 80; ++y)
        for (std::uint32_t x = 0; x < 100; ++x)
            if (x >= 76 || y >= 64)
                lShape.data()[y * 100 + x] = 255;
    const std::vector<Offset> cands = outpaintShiftCandidates(lShape, 8);
    // Axis rungs of both strips…
    CHECK(std::find(cands.begin(), cands.end(), Offset{-32, 0}) != cands.end());
    CHECK(std::find(cands.begin(), cands.end(), Offset{0, -24}) != cands.end());
    // …and the bottom-right corner's first diagonal rung pairing the two depths (24+8, 16+8).
    CHECK(std::find(cands.begin(), cands.end(), Offset{-32, -24}) != cands.end());
    // Only that corner exists: every diagonal points up-left, and no other sign pair appears.
    for (const Offset& o : cands) {
        if (o.u != 0 && o.v != 0) {
            CHECK(o.u < 0);
            CHECK(o.v < 0);
        }
    }
}

TEST_CASE("outpaint shift candidates: a full ring populates all four corners; degenerate holes "
          "produce nothing") {
    using namespace mosaic::core::inpaint;
    Selection ring(100, 80);
    for (std::uint32_t y = 0; y < 80; ++y)
        for (std::uint32_t x = 0; x < 100; ++x)
            if (x < 10 || x >= 90 || y < 10 || y >= 70)
                ring.data()[y * 100 + x] = 255;
    const std::vector<Offset> cands = outpaintShiftCandidates(ring, 8);
    bool signs[2][2] = {{false, false}, {false, false}}; // [u<0][v<0] among diagonal rungs
    for (const Offset& o : cands)
        if (o.u != 0 && o.v != 0)
            signs[o.u < 0 ? 1 : 0][o.v < 0 ? 1 : 0] = true;
    CHECK(signs[0][0]); // down-right (top-left corner)
    CHECK(signs[1][0]); // down-left  (top-right corner)
    CHECK(signs[0][1]); // up-right   (bottom-left corner)
    CHECK(signs[1][1]); // up-left    (bottom-right corner)

    // All-hole region: every scan line runs end to end, so no side reports a depth.
    Selection full(60, 40);
    for (std::uint32_t i = 0; i < 60 * 40; ++i)
        full.data()[i] = 255;
    CHECK(outpaintShiftCandidates(full, 8).empty());

    // No hole at all.
    const Selection empty(60, 40);
    CHECK(outpaintShiftCandidates(empty, 8).empty());
}

TEST_CASE("outpaint ladder is WIRED: a strip only the ladder can source is filled by a ladder "
          "copy, not diffusion") {
    // Horizontal gradient: every column is constant, so all self-similarity is VERTICAL — the
    // frequency vote and the boundary vote can only produce u==0 offsets, and none of those can
    // source the right strip (their sources stay inside the strip). Only the §3.7.8 leftward
    // ladder rungs give the strip a valid source, so a strip pixel whose output equals a
    // ladder-shifted known pixel proves the candidates actually reached the solver. This
    // regression pins the 2026-07-11 wiring fix: the rungs were appended AFTER the region→full
    // rescale, so the whole ladder had been silently dead since it shipped.
    ImageF im(64, 48);
    for (std::uint32_t y = 0; y < 48; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            const float v = static_cast<float>(x) / 64.0f;
            im.set(x, y, {v, v, v, 1.0f});
        }
    }
    Selection strip(64, 48);
    for (std::uint32_t y = 0; y < 48; ++y)
        for (std::uint32_t x = 40; x < 64; ++x)
            strip.data()[y * 64 + x] = 255;
    REQUIRE(inpaint::isOutpaintHole(strip, 64, 48)); // the ladder is outpaint-gated
    inpaint::Params p;
    p.poissonBlend = false; // verbatim label copies, so provenance is exact
    inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    REQUIRE(eng.setActiveBackend("offset-stats"));
    const inpaint::InpaintResult r = eng.run({im, strip, p});
    REQUIRE(r.ok);
    // Ladder rungs for a 24-deep strip with pad=max(2, patchSize=8): u ∈ {-32, -57}. Without
    // them every strip pixel is no-source and neighbour-diffuses to ~the boundary value 39/64;
    // with them it is a verbatim copy of a known column. Sample mid-strip and far-edge pixels.
    std::size_t ladderCopies = 0;
    std::size_t checked = 0;
    for (std::uint32_t y = 4; y < 44; y += 8) {
        for (std::uint32_t x = 44; x < 64; x += 5) {
            ++checked;
            const float got = r.image.at(x, y).r;
            for (const int mag : {32, 57}) {
                if (static_cast<long>(x) - mag >= 0 &&
                    got == im.at(static_cast<std::uint32_t>(static_cast<long>(x) - mag), y).r) {
                    ++ladderCopies;
                    break;
                }
            }
        }
    }
    // Every sampled pixel must be an exact ladder copy (no blend ran, labels are verbatim).
    CHECK(ladderCopies == checked);
}

TEST_CASE("outpaint structure penalty: the deviation term taxes smooth blobs the tensor misses") {
    // 160x80; the hole is the right strip x in [120,160). Content: a hard vertical edge at x=20
    // anchors the robust (98th-percentile) scales; a 1px checkerboard patch (fine texture, both
    // terms should ignore it) around (50,40); a wide smooth cosine bump centred (90,40), radius
    // 12 — a "faint cloud": its interior is tensor-blind (radial orientations cancel) but its
    // band-pass energy is high. Everything sits outside the near-boundary guard margin (~12px
    // here), which is itself checked last.
    ImageF im(160, 80);
    for (std::uint32_t y = 0; y < 80; ++y) {
        for (std::uint32_t x = 0; x < 160; ++x) {
            float v = x < 20 ? 0.1f : 0.5f;
            if (x >= 40 && x < 60 && y >= 10 && y < 70) {
                v = ((x + y) & 1u) != 0 ? 0.7f : 0.3f; // fine checkerboard, mean 0.5
            }
            const double dx = static_cast<double>(x) - 90.0;
            const double dy = static_cast<double>(y) - 40.0;
            const double rr = std::sqrt(dx * dx + dy * dy);
            if (rr < 12.0) { // smooth bump, peak +0.25, C1 at the rim
                v += 0.25f * static_cast<float>(0.5 * (1.0 + std::cos(rr / 12.0 * 3.14159265)));
            }
            im.set(x, y, {v, v, v, 1.0f});
        }
    }
    Selection hole(160, 80);
    for (std::uint32_t y = 0; y < 80; ++y)
        for (std::uint32_t x = 120; x < 160; ++x)
            hole.data()[y * 160 + x] = 255;
    const inpaint::StructurePenalty base =
        inpaint::buildStructurePenalty(im, hole, 1.0, 0.05, 0.0); // anisotropy only
    const inpaint::StructurePenalty dev =
        inpaint::buildStructurePenalty(im, hole, 1.0, 0.05, 1.5); // + deviation term
    // The blob's interior is nearly free under anisotropy alone (that blindness IS the recorded
    // residual: cloud sheets stayed cheap)...
    CHECK(base.at(90, 40) <= 0.15);
    // ...and the deviation term is what makes it pay.
    CHECK(dev.at(90, 40) >= base.at(90, 40) + 0.15);
    // Fine texture gains nothing: the band-pass's first blur flattens a 1px pattern to its mean.
    CHECK(std::fabs(dev.at(50, 40) - base.at(50, 40)) <= 0.05);
    // max-combine can only raise the map, never lower it.
    for (long y = 0; y < 80; y += 7) {
        for (long x = 0; x < 160; x += 7) {
            CHECK(dev.at(x, y) >= base.at(x, y) - 1e-6);
        }
    }
    // The near-boundary guard covers the deviation term too: donor pixels hugging the old frame
    // edge stay free (the ring step edge is the band-pass's strongest response by far).
    CHECK(dev.at(118, 40) == doctest::Approx(0.0));
}

TEST_CASE("outpaint boundary-crisp guidance: a ring mismatch stays a content edge, not a smear") {
    // 120x60; the hole is the right strip x in [100,120) (100 of 360 perimeter cells -> outpaint).
    // Known content: period-10 vertical stripes, EXCEPT the three columns hugging the hole
    // (x in [97,100)) carry a +0.3 bright patch on rows [20,40) that no periodic donor has. Every
    // valid donor sheet therefore views an UNbrightened ring there: its view of a boundary edge
    // disagrees with the composite's own gradient by ~0.3 — the §3.7.8 boundary-crisp trigger.
    // With the rule ON (the opt-in Params::outpaintBoundaryCrisp flag — OFF by default after
    // user-observed regressions) the guidance keeps the composite's gradient and the strip's
    // first columns keep their donor values; with it OFF (the default = the historical
    // behaviour) the blend diffuses the +0.3 ring residual into the strip as a bright smear.
    ImageF im(120, 60);
    for (std::uint32_t y = 0; y < 60; ++y) {
        for (std::uint32_t x = 0; x < 120; ++x) {
            float v = 0.1f + 0.05f * static_cast<float>(x % 10);
            if (x >= 97 && x < 100 && y >= 20 && y < 40) {
                v += 0.3f;
            }
            im.set(x, y, {v, v, v, 1.0f});
        }
    }
    Selection strip(120, 60);
    for (std::uint32_t y = 0; y < 60; ++y)
        for (std::uint32_t x = 100; x < 120; ++x)
            strip.data()[y * 120 + x] = 255;
    REQUIRE(inpaint::isOutpaintHole(strip, 120, 60));
    inpaint::InpaintEngine eng = inpaint::makeDefaultEngine();
    REQUIRE(eng.setActiveBackend("offset-stats"));
    const auto meanNearRing = [&](const ImageF& img) {
        double sum = 0.0;
        int cnt = 0;
        for (std::uint32_t y = 24; y < 36; ++y) {
            for (std::uint32_t x = 100; x < 104; ++x) {
                sum += img.at(x, y).r;
                ++cnt;
            }
        }
        return sum / cnt;
    };
    inpaint::Params pOn;
    pOn.outpaintBoundaryCrisp = true; // the opt-in flag under test (off by default)
    const inpaint::InpaintResult on = eng.run({im, strip, pOn});
    REQUIRE(on.ok);
    const inpaint::InpaintResult off = eng.run({im, strip, inpaint::Params{}}); // default: off
    REQUIRE(off.ok);
    // The default (historical) guidance injects the ring residual and brightens the near-ring
    // strip cells; the opt-in crisp rule keeps them at their donor values.
    CHECK(meanNearRing(off.image) >= meanNearRing(on.image) + 0.05);
}
