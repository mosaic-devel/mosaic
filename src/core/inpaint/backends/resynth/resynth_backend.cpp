// Resynthesizer-style backend — see resynth_backend.hpp for the algorithm credits and the
// perturbation guardrail.

#include "core/inpaint/backends/resynth/resynth_backend.hpp"

#include "core/inpaint/backends/he_sun/working_region.hpp" // workingRegionRect (shared crop rule)
#include "core/inpaint/outpaint.hpp" // isOutpaintHole (S16-f: expansion rings synthesize banded)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "common/thread_pool.hpp"

namespace mosaic::core::inpaint {

namespace {

[[nodiscard]] bool pixelHole(const Selection& m, std::uint32_t x, std::uint32_t y) {
    return !m.isEmpty() && x < m.width() && y < m.height() && m.at(x, y) > 0;
}

// Fixed-seed xorshift32: sampling must be deterministic (identical inputs -> identical output),
// so the "random element" Harrison describes is a repeatable pseudo-random sequence, never
// std::rand / time seeding.
struct Rng {
    std::uint32_t s = 0x2545F491u;
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
};

// The Cauchy-based robust point metric of the thesis: cost(d) = log2(1 + (d/sigma)^2) per
// channel, evaluated through a PRECOMPUTED byte-difference table exactly as the plugin does
// (its diff_table): the synthesis compares 8-bit values, so |d| ∈ [0, 255]. Forgiving of a few
// outlier points in an otherwise good neighbourhood match — the property Harrison chose it for
// (the 2001 paper used weighted L1 for the same reason; the thesis form supersedes it).
struct CauchyMetric {
    float table[256];
    explicit CauchyMetric(double sigma) {
        const double s = std::max(1e-3, sigma) * 255.0; // sigma is given in [0,1] units
        const double invSigma2 = 1.0 / (s * s);
        for (int i = 0; i < 256; ++i) {
            table[static_cast<std::size_t>(i)] =
                static_cast<float>(std::log2(1.0 + static_cast<double>(i) * i * invSigma2));
        }
    }
    [[nodiscard]] float operator()(int d) const {
        return table[static_cast<std::size_t>(d < 0 ? -d : d)];
    }
};

// Rect-local synthesis buffers: one 32-bit word per cell, {r, g, b, flags}. The hot loop's reads
// were random accesses into the full-resolution FLOAT image (hundreds of MB) and the full-image
// selection mask — memory-bound, one cache miss per compared point. The plugin works on compact
// 8-bit buffers; so does this now (the OUTPUT still copies the winner's full float value — only
// the comparisons are 8-bit, which is what the source photos are anyway).
constexpr std::uint32_t kCellValid = 0x01000000u;  // corpus: known content (never hole)
constexpr std::uint32_t kCellFilled = 0x01000000u; // canvas: has a value (known or synthesized)

[[nodiscard]] inline std::uint32_t packCell(const common::ColorF& c, std::uint32_t flag) {
    const auto q = [](float v) {
        return static_cast<std::uint32_t>(
            std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | flag;
}

} // namespace

InpaintResult ResynthBackend::run(const InpaintRequest& request, const ProgressFn& progress) {
    InpaintResult res;
    res.image = request.image;
    if (request.image.empty()) {
        res.ok = false;
        res.detail = "empty image";
        return res;
    }
    ProgressReporter prog(progress, request.cancel);
    if (!prog.report(0.02f, "Analyzing")) {
        res.ok = false;
        res.detail = "cancelled";
        return res;
    }
    const std::optional<common::Rect> holeBounds = request.holeMask.bounds();
    if (!holeBounds) {
        res.detail = "no selection coverage";
        return res; // nothing to fill
    }
    const Params& p = request.params;
    const long W = static_cast<long>(request.image.width);
    const long H = static_cast<long>(request.image.height);

    std::optional<ScopedStage> setupStage;
    setupStage.emplace(&res.timings, "resynth-setup");
    // Donor/synthesis region: the same crop rule as the default backend (3x margin around the
    // hole, tighter for small holes) but at FULL resolution — per-pixel synthesis reads real
    // pixels, never a downsample.
    const common::Rect rr = workingRegionRect(request.image.width, request.image.height,
                                              holeBounds, p);
    const long rx0 = static_cast<long>(rr.x);
    const long ry0 = static_cast<long>(rr.y);
    const long rw = static_cast<long>(rr.w);
    const long rh = static_cast<long>(rr.h);
    if (rw <= 0 || rh <= 0) {
        res.ok = false;
        res.detail = "degenerate working region";
        return res;
    }
    const auto inRect = [&](long x, long y) {
        return x >= rx0 && y >= ry0 && x < rx0 + rw && y < ry0 + rh;
    };
    const auto isHole = [&](long x, long y) {
        return pixelHole(request.holeMask, static_cast<std::uint32_t>(x),
                         static_cast<std::uint32_t>(y));
    };

    // Donor list (known pixels in the rect), the fill-state grid, and the compact rect-local
    // buffers: `corpus` is the UNTOUCHED input (hole cells carry no valid flag — synthesis never
    // reads removed content), `canvas` is the evolving state the neighbourhoods compare against.
    std::vector<std::pair<int, int>> donors;
    std::vector<std::uint8_t> filled(static_cast<std::size_t>(rw) * static_cast<std::size_t>(rh));
    std::vector<std::uint32_t> corpus(filled.size(), 0);
    std::vector<std::uint32_t> canvas(filled.size(), 0);
    std::vector<std::pair<int, int>> targets;
    for (long y = ry0; y < ry0 + rh; ++y) {
        for (long x = rx0; x < rx0 + rw; ++x) {
            const std::size_t o = static_cast<std::size_t>(y - ry0) * static_cast<std::size_t>(rw) +
                                  static_cast<std::size_t>(x - rx0);
            if (isHole(x, y)) {
                targets.emplace_back(static_cast<int>(x), static_cast<int>(y));
            } else {
                filled[o] = 1;
                const std::uint32_t cell = packCell(
                    request.image.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)),
                    kCellValid);
                corpus[o] = cell;
                canvas[o] = cell; // kCellValid == kCellFilled for known cells
                donors.emplace_back(static_cast<int>(x), static_cast<int>(y));
            }
        }
    }
    if (targets.empty()) {
        res.detail = "no hole inside the working region";
        return res;
    }
    if (donors.size() < 16) {
        res.ok = false;
        res.detail = "not enough known pixels around the selection to synthesize from";
        return res;
    }

    // Neighbourhood offset table: offsets sorted by distance (then y, x — deterministic). The
    // per-pixel comparison window collects the nearest already-filled points from it, and the
    // ordering below counts its leading entries as a cell's constraint.
    std::vector<std::pair<int, int>> offsetTable;
    {
        constexpr int kOffsetRadius = 12;
        for (int dy = -kOffsetRadius; dy <= kOffsetRadius; ++dy) {
            for (int dx = -kOffsetRadius; dx <= kOffsetRadius; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                if (dx * dx + dy * dy <= kOffsetRadius * kOffsetRadius) {
                    offsetTable.emplace_back(dx, dy);
                }
            }
        }
        std::sort(offsetTable.begin(), offsetTable.end(), [](const auto& a, const auto& b) {
            const int da = a.first * a.first + a.second * a.second;
            const int db = b.first * b.first + b.second * b.second;
            if (da != db) {
                return da < db;
            }
            if (a.second != b.second) {
                return a.second < b.second;
            }
            return a.first < b.first;
        });
    }

    // Outpaint banding: when the hole is a canvas-expansion ring (shared test with the
    // default engine — interior heals are untouched), each ring pixel's RANDOM donor draws are
    // restricted to a band at its own depth along the image edge it extends: a sky pixel
    // sources sky-height content, a grass pixel grass-height, and the horizon continues at the
    // horizon. Without the band, coherence chains seeded by whole-rect draws floated grass
    // shelves into the sky (the Broadway-tower repro). Per-pixel axis: 0 = the known content
    // lies along ±x (band donor.y near target.y), 1 = along ±y (band donor.x), 2 = unbanded
    // (interior pockets / no reachable known on either axis). Coherence candidates stay free —
    // they are local continuation by construction.
    const bool outpaint = isOutpaintHole(request.holeMask, request.image.width,
                                         request.image.height);
    long outpaintBand = std::clamp((rw + rh) / 20, 24L, 160L);
    if (const char* v = std::getenv("MOSAIC_INPAINT_OUTPAINT_BAND")) {
        outpaintBand = std::atol(v); // sweep override; <= 0 disables the banding
    }
    std::vector<std::uint8_t> bandAxis;
    if (outpaint && outpaintBand > 0) {
        bandAxis.assign(filled.size(), 2);
        const long kFar = rw + rh; // "no known found" sentinel distance
        std::vector<long> dh(filled.size(), kFar);
        std::vector<long> dv(filled.size(), kFar);
        for (long y = 0; y < rh; ++y) { // horizontal distance to known, both sweeps
            long since = kFar;
            for (long x = 0; x < rw; ++x) {
                const std::size_t o =
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(rw) +
                    static_cast<std::size_t>(x);
                since = filled[o] != 0 ? 0 : (since >= kFar ? kFar : since + 1);
                dh[o] = std::min(dh[o], since);
            }
            since = kFar;
            for (long x = rw - 1; x >= 0; --x) {
                const std::size_t o =
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(rw) +
                    static_cast<std::size_t>(x);
                since = filled[o] != 0 ? 0 : (since >= kFar ? kFar : since + 1);
                dh[o] = std::min(dh[o], since);
            }
        }
        for (long x = 0; x < rw; ++x) { // vertical distance to known, both sweeps
            long since = kFar;
            for (long y = 0; y < rh; ++y) {
                const std::size_t o =
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(rw) +
                    static_cast<std::size_t>(x);
                since = filled[o] != 0 ? 0 : (since >= kFar ? kFar : since + 1);
                dv[o] = std::min(dv[o], since);
            }
            since = kFar;
            for (long y = rh - 1; y >= 0; --y) {
                const std::size_t o =
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(rw) +
                    static_cast<std::size_t>(x);
                since = filled[o] != 0 ? 0 : (since >= kFar ? kFar : since + 1);
                dv[o] = std::min(dv[o], since);
            }
        }
        for (std::size_t o = 0; o < filled.size(); ++o) {
            if (filled[o] == 0 && (dh[o] < kFar || dv[o] < kFar)) {
                bandAxis[o] = dh[o] <= dv[o] ? 0 : 1;
            }
        }
    }

    // Priority machinery for the LIVE-front pass-0 rounds below (the paper's "most constrained
    // empty location first", evaluated against the real fill state at run time): the constraint
    // of a cell = how many of its nearest kPrioWindow offsets are already filled. An earlier
    // design precomputed the whole order by simulating the fill and then batched it for the
    // worker pool — but batching reorders across the simulation's assumptions, and pixels ran
    // with far less context than the order promised (a camouflage of context-free blotches on
    // large holes). Eligibility is now a property of the ACTUAL state, so batching can never
    // starve a pixel of the context its priority claimed.
    constexpr int kPrioWindow = 12; // nearest offsets counted toward a cell's constraint
    const auto oAt = [&](long x, long y) {
        return static_cast<std::size_t>(y - ry0) * static_cast<std::size_t>(rw) +
               static_cast<std::size_t>(x - rx0);
    };
    const auto countFilled = [&](long x, long y) {
        int c = 0;
        for (int k = 0; k < kPrioWindow; ++k) {
            const long nx = x + offsetTable[static_cast<std::size_t>(k)].first;
            const long ny = y + offsetTable[static_cast<std::size_t>(k)].second;
            if (inRect(nx, ny) ? filled[oAt(nx, ny)] != 0
                               : (nx >= 0 && ny >= 0 && nx < W && ny < H && !isHole(nx, ny))) {
                ++c; // known content just outside the rect counts as context too
            }
        }
        return c;
    };
    // The recorded pass-0 fill sequence (refinement passes revisit its worst-matched half).
    std::vector<std::pair<int, int>> order;
    order.reserve(targets.size());

    const int neighbors = std::clamp(p.resynthNeighbors, 4, 64);
    const int tries = std::clamp(p.resynthTries, 8, 1000);
    const int passes = std::clamp(p.resynthPasses, 1, 6);
    const CauchyMetric metric(p.resynthSensitivity);
    // A comparison the corpus cannot answer (the source's neighbour lands outside the rect or in
    // the hole) costs as much as a full-range miss on every channel — candidates whose context
    // hangs off the edge of the donor region are discouraged, as in the plugin.
    const float kInvalidPoint = 3.0f * metric(255);
    // Early ACCEPT, as in the plugin ("patch searches stop upon finding a match meeting a
    // quality threshold"): once a candidate matches to within ~2/255 per compared channel,
    // searching further buys nothing perceptible — in smooth regions (sky) EVERY donor is a
    // near-tie, and without this the search scores all `tries` candidates in full there.
    const float kAcceptPerPoint = 3.0f * metric(2);

    // Source map: for each rect cell, the donor position its value came from (hole pixels only).
    std::vector<std::pair<int, int>> srcMap(filled.size(), {-1, -1});
    const common::ImageF& src = request.image; // corpus reads: the untouched input
    const auto rectIdx = [&](long x, long y) {
        return static_cast<std::size_t>(y - ry0) * static_cast<std::size_t>(rw) +
               static_cast<std::size_t>(x - rx0);
    };

    setupStage.reset(); // close "resynth-setup" before the timed synthesis loop

    {
        ScopedStage synthStage(&res.timings, "resynth-synthesize");
        struct NbPoint {
            int dx, dy;
            std::uint32_t value; // packed canvas cell (8-bit r|g|b)
            bool holeNb;
            std::pair<int, int> nbSrc;
        };
        struct Scratch {
            std::vector<NbPoint> nb;
            std::vector<std::pair<int, int>> cands;
        };
        // Best cost of each hole pixel's last synthesis (mean per compared point), for the
        // shrinking refinement passes.
        std::vector<float> costMap(filled.size(), 0.0f);

        // Per-(pixel, pass) seeded generator: the random draws must not be a shared sequence,
        // or the result would depend on processing interleaving. With hashed seeds the output
        // is bit-for-bit identical for ANY thread count, including one.
        const auto pixelRng = [](int x, int y, int pass) {
            std::uint32_t h = static_cast<std::uint32_t>(x) * 0x9E3779B1u;
            h ^= (static_cast<std::uint32_t>(y) + 0x85EBCA6Bu) * 0xC2B2AE35u;
            h ^= static_cast<std::uint32_t>(pass + 1) * 0x27D4EB2Fu;
            h ^= h >> 16;
            h *= 0x85EBCA6Bu;
            h ^= h >> 13;
            return Rng{h != 0 ? h : 1u};
        };

        // One pixel's synthesis against the CURRENT state. Safe to run concurrently for pixels
        // more than kOffsetRadius apart in Chebyshev distance: every read (values, fill state,
        // source map) is within that radius, and all writes are the pixel's own cells.
        const auto synthOne = [&](int tx, int ty, int pass, Scratch& s) {
            s.nb.clear();
            for (const auto& [dx, dy] : offsetTable) {
                if (static_cast<int>(s.nb.size()) >= neighbors) {
                    break;
                }
                const long nx = tx + dx;
                const long ny = ty + dy;
                if (!inRect(nx, ny)) {
                    continue;
                }
                const std::size_t no = rectIdx(nx, ny);
                const std::uint32_t cell = canvas[no];
                if ((cell & kCellFilled) == 0) {
                    continue;
                }
                const bool holeNb = (corpus[no] & kCellValid) == 0; // filled but not known
                s.nb.push_back({dx, dy, cell, holeNb,
                                holeNb ? srcMap[no] : std::pair<int, int>{static_cast<int>(nx),
                                                                          static_cast<int>(ny)}});
            }

            // Candidates: neighbour COHERENCE (each nearby point proposes the donor that would
            // continue its own source patch — Harrison's thesis search, Ashikhmin-style) plus
            // UNIFORM random donor draws. Never a perturbation of an existing mapping (the
            // header's guardrail). The current source competes first, so ties keep it.
            s.cands.clear();
            const std::pair<int, int> cur = srcMap[rectIdx(tx, ty)];
            if (cur.first >= 0) {
                s.cands.push_back(cur);
            }
            constexpr int kCoherenceFrom = 8;
            int coh = 0;
            for (const NbPoint& q : s.nb) {
                if (coh >= kCoherenceFrom) {
                    break;
                }
                if (q.nbSrc.first < 0) {
                    continue;
                }
                const long cx = static_cast<long>(q.nbSrc.first) - q.dx;
                const long cy = static_cast<long>(q.nbSrc.second) - q.dy;
                if (inRect(cx, cy) && (corpus[rectIdx(cx, cy)] & kCellValid) != 0) {
                    s.cands.emplace_back(static_cast<int>(cx), static_cast<int>(cy));
                    ++coh;
                }
            }
            // Uniform donor draws by rejection over the rect (the donor-position ARRAY is huge
            // and random reads of it were pure cache misses; testing the corpus valid bit costs
            // one read the scoring would do anyway). Deterministic: fixed per-pixel sequence.
            // Outpaint banding (see the setup above): a ring pixel draws inside the band
            // at its own depth along the edge it extends; interior heals draw the whole rect.
            Rng rng2 = pixelRng(tx, ty, pass);
            const auto uw = static_cast<std::uint32_t>(rw);
            const auto uh = static_cast<std::uint32_t>(rh);
            const std::uint8_t axis =
                bandAxis.empty() ? std::uint8_t{2} : bandAxis[rectIdx(tx, ty)];
            const auto uBand = static_cast<std::uint32_t>(2 * outpaintBand + 1);
            int drawn = 0;
            for (int attempts = 0; drawn < tries && attempts < 4 * tries; ++attempts) {
                long lx;
                long ly;
                if (axis == 0) { // known lies along ±x: donors at a similar HEIGHT
                    lx = static_cast<long>(rng2.next() % uw);
                    ly = std::clamp((ty - ry0) + static_cast<long>(rng2.next() % uBand) -
                                        outpaintBand,
                                    0L, rh - 1);
                } else if (axis == 1) { // known lies along ±y: donors at a similar COLUMN
                    lx = std::clamp((tx - rx0) + static_cast<long>(rng2.next() % uBand) -
                                        outpaintBand,
                                    0L, rw - 1);
                    ly = static_cast<long>(rng2.next() % uh);
                } else {
                    lx = static_cast<long>(rng2.next() % uw);
                    ly = static_cast<long>(rng2.next() % uh);
                }
                if ((corpus[static_cast<std::size_t>(ly) * static_cast<std::size_t>(rw) +
                            static_cast<std::size_t>(lx)] &
                     kCellValid) != 0) {
                    s.cands.emplace_back(static_cast<int>(rx0 + lx), static_cast<int>(ry0 + ly));
                    ++drawn;
                }
            }

            // Score with exact early termination (a candidate is abandoned the moment its
            // partial sum reaches the best so far — pure speedup, identical argmin) and the
            // plugin's early ACCEPT (stop the whole search at a good-enough match).
            const float acceptCost = kAcceptPerPoint * static_cast<float>(s.nb.size());
            float bestCost = std::numeric_limits<float>::infinity();
            std::pair<int, int> best{-1, -1};
            for (const auto& c : s.cands) {
                float cost = 0.0f;
                for (const NbPoint& q : s.nb) {
                    const long sx = c.first + q.dx;
                    const long sy = c.second + q.dy;
                    std::uint32_t cell = 0;
                    if (inRect(sx, sy)) {
                        cell = corpus[rectIdx(sx, sy)];
                    }
                    if ((cell & kCellValid) == 0) {
                        cost += kInvalidPoint;
                    } else {
                        cost += metric(static_cast<int>(cell & 0xFF) -
                                       static_cast<int>(q.value & 0xFF)) +
                                metric(static_cast<int>((cell >> 8) & 0xFF) -
                                       static_cast<int>((q.value >> 8) & 0xFF)) +
                                metric(static_cast<int>((cell >> 16) & 0xFF) -
                                       static_cast<int>((q.value >> 16) & 0xFF));
                    }
                    if (cost >= bestCost) {
                        break;
                    }
                }
                if (cost < bestCost) {
                    bestCost = cost;
                    best = c;
                    if (bestCost <= acceptCost) {
                        break; // good enough — the plugin's search cutoff
                    }
                }
            }
            if (best.first >= 0) {
                const std::size_t o = rectIdx(tx, ty);
                srcMap[o] = best;
                costMap[o] =
                    bestCost / static_cast<float>(std::max<std::size_t>(1, s.nb.size()));
                canvas[o] = (corpus[rectIdx(best.first, best.second)] & 0x00FFFFFFu) |
                            kCellFilled;
                // The OUTPUT keeps full float fidelity: copy the winner's original value.
                const common::ColorF v = src.at(static_cast<std::uint32_t>(best.first),
                                                static_cast<std::uint32_t>(best.second));
                const float a = res.image.at(static_cast<std::uint32_t>(tx),
                                             static_cast<std::uint32_t>(ty)).a;
                res.image.set(static_cast<std::uint32_t>(tx), static_cast<std::uint32_t>(ty),
                              {v.r, v.g, v.b, a});
                filled[o] = 1;
            }
        };

        // Wave-parallel synthesis on the shared pool (common/thread_pool.hpp): one band per
        // participant, all draining the same chunked cursor, so the dynamic load balancing is
        // exactly what this loop's own condition-variable pool did -- minus the threads it spawned
        // for every inpaint run. Wave members are more than kOffsetRadius apart by construction,
        // so the result stays independent of thread count and interleaving either way.
        const unsigned hw = common::hardwareThreads();
        const int workers = static_cast<int>(std::min(hw, 16u)) - 1; // main thread participates
        struct Wave {
            const std::vector<std::pair<int, int>>* px = nullptr;
            int pass = 0;
        };
        Wave wave;
        std::atomic<std::size_t> cursor{0};
        const auto drain = [&](Scratch& s) {
            constexpr std::size_t kChunk = 24; // claim work in chunks: one shared-line RMW per
                                               // ~24 pixels instead of per pixel
            for (;;) {
                const std::size_t lo = cursor.fetch_add(kChunk);
                if (lo >= wave.px->size()) {
                    break;
                }
                const std::size_t hi = std::min(wave.px->size(), lo + kChunk);
                for (std::size_t i = lo; i < hi; ++i) {
                    const auto& [x, y] = (*wave.px)[i];
                    synthOne(x, y, wave.pass, s);
                }
            }
        };
        // One scratch buffer per participant, reused across every wave: a hole is thousands of
        // waves deep, so allocating the neighbour/candidate vectors per wave would cost more than
        // the synthesis. Band index == participant index, and a band runs once per wave, so no
        // two threads ever share a slot.
        std::vector<Scratch> scratch(static_cast<std::size_t>(std::max(0, workers)) + 1);
        for (Scratch& s : scratch) {
            s.nb.reserve(static_cast<std::size_t>(neighbors));
        }
        const auto runWave = [&](const std::vector<std::pair<int, int>>& w, int pass) {
            wave.px = &w;
            wave.pass = pass;
            cursor.store(0);
            // Small waves (the tail of every front) are cheaper on the submitting thread than a
            // pool round-trip.
            constexpr std::size_t kMainOnlyBelow = 64;
            if (workers <= 0 || w.size() < kMainOnlyBelow) {
                drain(scratch[0]);
                return;
            }
            common::parallelBands(static_cast<std::size_t>(workers) + 1,
                                  [&](std::size_t i) { drain(scratch[i]); });
        };

        // Wave partition: split a work list into order-respecting subsets whose members are
        // pairwise MORE than kOffsetRadius apart, via a coarse occupancy grid (cell = radius+1,
        // so a cell holds at most one member and only the 3x3 surrounding cells need an exact
        // check). Deterministic — no dependence on thread count or timing.
        constexpr int kWaveCell = 13; // kOffsetRadius + 1
        const long cw = (rw + kWaveCell - 1) / kWaveCell;
        const long ch = (rh + kWaveCell - 1) / kWaveCell;
        std::vector<std::int32_t> cellOcc(static_cast<std::size_t>(cw) *
                                              static_cast<std::size_t>(ch),
                                          -1); // packed member coords, -1 = empty
        std::vector<std::size_t> touched;
        const auto buildWave = [&](std::vector<std::pair<int, int>>& work,
                                   std::vector<std::pair<int, int>>& waveOut,
                                   std::vector<std::pair<int, int>>& rest) {
            waveOut.clear();
            rest.clear();
            for (const std::size_t t : touched) {
                cellOcc[t] = -1;
            }
            touched.clear();
            for (const auto& [x, y] : work) {
                const long cx = (x - rx0) / kWaveCell;
                const long cy = (y - ry0) / kWaveCell;
                bool conflict = false;
                for (long oy = std::max(0L, cy - 1); oy <= std::min(ch - 1, cy + 1) && !conflict;
                     ++oy) {
                    for (long ox = std::max(0L, cx - 1); ox <= std::min(cw - 1, cx + 1); ++ox) {
                        const std::int32_t occ =
                            cellOcc[static_cast<std::size_t>(oy) * static_cast<std::size_t>(cw) +
                                    static_cast<std::size_t>(ox)];
                        if (occ < 0) {
                            continue;
                        }
                        const int mx = occ % 65536;
                        const int my = occ / 65536;
                        if (std::abs(mx - (x - rx0)) <= kWaveCell - 1 &&
                            std::abs(my - (y - ry0)) <= kWaveCell - 1) {
                            conflict = true;
                            break;
                        }
                    }
                }
                if (conflict) {
                    rest.emplace_back(x, y);
                } else {
                    const std::size_t cell =
                        static_cast<std::size_t>(cy) * static_cast<std::size_t>(cw) +
                        static_cast<std::size_t>(cx);
                    cellOcc[cell] =
                        static_cast<std::int32_t>((y - ry0) * 65536 + (x - rx0));
                    touched.push_back(cell);
                    waveOut.emplace_back(x, y);
                }
            }
        };

        const std::size_t estTotal =
            targets.size() +
            (static_cast<std::size_t>(std::max(0, passes - 1)) * targets.size()) / 2;
        std::size_t done = 0;
        std::size_t nextPreview = std::max<std::size_t>(1, estTotal / 12);
        std::vector<std::pair<int, int>> waveBuf;
        std::vector<std::pair<int, int>> restBuf;
        const auto tick = [&]() -> bool { // progress + cancellation between waves
            if (prog.cancelled()) {
                return false;
            }
            if (done >= nextPreview) {
                nextPreview = done + std::max<std::size_t>(1, estTotal / 12);
                // estTotal is an estimate (refinement can revisit more than half on cost ties) —
                // never report past 0.98.
                const float frac = 0.05f + 0.93f * (static_cast<float>(done) /
                                                    static_cast<float>(
                                                        std::max<std::size_t>(1, estTotal)));
                return prog.report(std::min(0.98f, frac), "Synthesizing", &res.image);
            }
            return true;
        };

        // PASS 0 — live-front rounds. Each round takes the unfilled cells whose ACTUAL filled-
        // neighbour count is within a small slack of the current maximum (the real front, the
        // paper's most-constrained-first rule evaluated against live state), partitions them
        // into spaced waves and runs those on the pool. A pixel can never run with less context
        // than its class promises, no matter how the waves interleave. The frontier set keeps
        // each round's scan proportional to the front, not the hole.
        {
            std::vector<std::pair<int, int>> frontier;
            std::vector<std::uint8_t> inFrontier(filled.size(), 0);
            for (const auto& [x, y] : targets) {
                if (countFilled(x, y) > 0) {
                    frontier.emplace_back(x, y);
                    inFrontier[oAt(x, y)] = 1;
                }
            }
            std::size_t filledCount = 0;
            std::vector<std::pair<int, int>> eligible;
            std::vector<std::pair<int, int>> keep;
            while (filledCount < targets.size()) {
                if (frontier.empty()) {
                    // Disconnected pocket (pathological mask): seed the first unfilled target.
                    for (const auto& t : targets) {
                        if (filled[oAt(t.first, t.second)] == 0) {
                            frontier.push_back(t);
                            inFrontier[oAt(t.first, t.second)] = 1;
                            break;
                        }
                    }
                }
                // Split the frontier into this round's eligible class and the rest.
                int maxC = 0;
                for (const auto& [x, y] : frontier) {
                    if (filled[oAt(x, y)] == 0) {
                        maxC = std::max(maxC, countFilled(x, y));
                    }
                }
                // Class width: batches without letting anything run context-starved. maxC == 0
                // (a seeded context-free pocket) must admit the 0-count members or no round
                // could ever run them.
                const int floorC = maxC > 0 ? std::max(1, maxC - 2) : 0;
                eligible.clear();
                keep.clear();
                for (const auto& t : frontier) {
                    const std::size_t o = oAt(t.first, t.second);
                    if (filled[o] != 0) {
                        inFrontier[o] = 0; // synthesized in an earlier round
                        continue;
                    }
                    if (countFilled(t.first, t.second) >= floorC) {
                        eligible.push_back(t);
                    } else {
                        keep.push_back(t);
                    }
                }
                frontier.swap(keep);
                // Spaced waves over the eligible class; deferred members retry immediately
                // (their context only grew).
                while (!eligible.empty()) {
                    if (!tick()) {
                        res.ok = false;
                        res.detail = "cancelled";
                        return res;
                    }
                    buildWave(eligible, waveBuf, restBuf);
                    if (waveBuf.empty()) {
                        break; // defensive: cannot happen (first item always fits a wave)
                    }
                    runWave(waveBuf, 0);
                    done += waveBuf.size();
                    filledCount += waveBuf.size();
                    for (const auto& [x, y] : waveBuf) {
                        order.emplace_back(x, y); // the recorded fill sequence
                        // Newly reachable neighbours join the frontier.
                        for (int k = 0; k < kPrioWindow; ++k) {
                            const long nx = x - offsetTable[static_cast<std::size_t>(k)].first;
                            const long ny = y - offsetTable[static_cast<std::size_t>(k)].second;
                            if (inRect(nx, ny) && isHole(nx, ny) && filled[oAt(nx, ny)] == 0 &&
                                inFrontier[oAt(nx, ny)] == 0) {
                                inFrontier[oAt(nx, ny)] = 1;
                                frontier.emplace_back(static_cast<int>(nx),
                                                      static_cast<int>(ny));
                            }
                        }
                    }
                    eligible.swap(restBuf);
                }
            }
        }

        // REFINEMENT passes revisit the WORST-matched half of the fill (the plugin's shrinking
        // passes) with complete surroundings. Context is complete here, so plain segment-wise
        // spaced waves over the recorded sequence are safe.
        for (int pass = 1; pass < passes; ++pass) {
            std::vector<float> cs;
            cs.reserve(order.size());
            for (const auto& [x, y] : order) {
                cs.push_back(costMap[rectIdx(x, y)]);
            }
            auto mid = cs.begin() + static_cast<std::ptrdiff_t>(cs.size() / 2);
            std::nth_element(cs.begin(), mid, cs.end(), std::greater<float>());
            const float thr = *mid;
            std::vector<std::pair<int, int>> work;
            for (const auto& t : order) {
                if (costMap[rectIdx(t.first, t.second)] >= thr) {
                    work.push_back(t);
                }
            }
            constexpr std::size_t kSegment = 262144;
            std::size_t segStart = 0;
            while (segStart < work.size()) {
                std::vector<std::pair<int, int>> segment(
                    work.begin() + static_cast<std::ptrdiff_t>(segStart),
                    work.begin() + static_cast<std::ptrdiff_t>(
                                       std::min(work.size(), segStart + kSegment)));
                segStart += segment.size();
                while (!segment.empty()) {
                    if (!tick()) {
                        res.ok = false;
                        res.detail = "cancelled";
                        return res;
                    }
                    buildWave(segment, waveBuf, restBuf);
                    if (waveBuf.empty()) {
                        break;
                    }
                    runWave(waveBuf, pass);
                    done += waveBuf.size();
                    segment.swap(restBuf);
                }
            }
        }
    }
    res.detail = "Resynthesizer texture synthesis (Harrison 2001/2005)";
    prog.report(1.0f, "Synthesizing", &res.image);
    return res;
}

BackendInfo ResynthBackend::info() const {
    BackendInfo bi;
    bi.displayName = "Resynthesizer";
    bi.method = "Best-fit per-pixel texture synthesis";
    bi.authors = "Paul Harrison, 2001-2005 (the GIMP Resynthesizer; maintained by Lloyd Konneker)";
    bi.paper = "A Non-hierarchical Procedure for Re-synthesis of Complex Textures (WSCG 2001); "
               "Image Texture Tools (PhD thesis, Monash University, 2005)";
    bi.summary =
        "Grows the fill one pixel at a time, always choosing the donor pixel whose surroundings "
        "best match what is already there — the classic Resynthesizer known from GIMP's "
        "heal-selection plugin. Strongest on organic, irregular texture (grass, foliage, gravel, "
        "sand, clouds); the default Offset-statistics engine remains better at continuing "
        "repeating structure and long edges.";
    bi.deviations = {
        "Clean-room C++ from the publications — no plugin code ported, none of the GIMP "
        "adapters.",
        "Deterministic: the random element is a fixed-seed sequence, so identical inputs give "
        "identical output.",
        "Donor pool bounded to the hole's neighbourhood (the shared working-region rule), at "
        "full resolution.",
        "Most-constrained-first fill order (live filled-neighbour counts) approximates the "
        "paper's entropy prioritization for the hole-filling case.",
        "Comparisons run on 8-bit values as in the plugin; the output copies the winning "
        "donor's full-precision value.",
    };
    bi.augmentations = {
        "Multi-threaded: the front is filled in spaced waves across CPU cores, with identical "
        "output for any thread count.",
        "Exact early-termination candidate scoring (identical result, large speedup).",
        "Live preview streaming and cancellation while the fill grows.",
    };
    bi.cost = "Seconds on typical selections; grows with hole area; multi-threaded CPU";
    return bi;
}

BackendSettingsSchema ResynthBackend::settingsSchema() const {
    BackendSettingsSchema s;
    s.controls = {
        {"resynthNeighbors", "Neighbourhood size",
         "How many nearby known pixels each candidate is compared against. Larger keeps more "
         "context (better structure) but is slower.",
         ParamControl::Kind::Int, 8, 64, 1, {}, 30, /*advanced*/ false},
        {"resynthTries", "Search tries",
         "Random donor candidates examined per pixel, in addition to the coherent guesses. More "
         "finds better matches at some cost.",
         ParamControl::Kind::Int, 20, 500, 10, {}, 200, /*advanced*/ false},
        {"resynthPasses", "Refinement passes",
         "After the first fill, whole-hole refinement passes reconsider every pixel with its "
         "completed surroundings.",
         ParamControl::Kind::Int, 1, 4, 1, {}, 2, /*advanced*/ false},
        {"resynthSensitivity", "Outlier tolerance",
         "Width of the robust match metric: higher forgives more mismatched pixels in an "
         "otherwise good patch (Harrison's sensitivity parameter).",
         ParamControl::Kind::Real, 0.02, 0.5, 0.01, {}, 0.12, /*advanced*/ true},
    };
    s.presets = {
        {"fast", "Fast", {{"resynthNeighbors", 16}, {"resynthTries", 80}, {"resynthPasses", 1}}},
        {"balanced",
         "Balanced",
         {{"resynthNeighbors", 30}, {"resynthTries", 200}, {"resynthPasses", 2}}},
        {"best",
         "Best",
         {{"resynthNeighbors", 48}, {"resynthTries", 400}, {"resynthPasses", 3}}},
    };
    s.defaultPreset = "balanced";
    return s;
}

void ResynthBackend::applyParam(Params& params, const std::string& key, double value) const {
    const auto asInt = [&] { return static_cast<int>(std::lround(value)); };
    if (key == "resynthNeighbors") {
        params.resynthNeighbors = asInt();
    } else if (key == "resynthTries") {
        params.resynthTries = asInt();
    } else if (key == "resynthPasses") {
        params.resynthPasses = asInt();
    } else if (key == "resynthSensitivity") {
        params.resynthSensitivity = value;
    }
}

std::optional<common::Rect>
ResynthBackend::analysedRegion(std::uint32_t imageW, std::uint32_t imageH,
                               const std::optional<common::Rect>& holeBounds,
                               const Params& params) const {
    if (imageW == 0 || imageH == 0) {
        return std::nullopt;
    }
    return workingRegionRect(imageW, imageH, holeBounds, params);
}

} // namespace mosaic::core::inpaint
