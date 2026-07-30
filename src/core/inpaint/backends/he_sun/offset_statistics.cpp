#include "core/inpaint/backends/he_sun/offset_statistics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "common/thread_pool.hpp"

namespace mosaic::core::inpaint {

namespace {

[[nodiscard]] bool pixelHole(const Selection& m, std::uint32_t x, std::uint32_t y) {
    return !m.isEmpty() && x < m.width() && y < m.height() && m.at(x, y) > 0;
}

// A patch (top-left (x,y), size ps) is "known" only if none of its pixels lie in the hole.
[[nodiscard]] bool patchKnown(const Selection& m, std::uint32_t x, std::uint32_t y, int ps) {
    if (m.isEmpty()) {
        return true;
    }
    for (int dy = 0; dy < ps; ++dy) {
        for (int dx = 0; dx < ps; ++dx) {
            if (pixelHole(m, x + static_cast<std::uint32_t>(dx),
                          y + static_cast<std::uint32_t>(dy))) {
                return false;
            }
        }
    }
    return true;
}

// Split [0, count) into hardware-thread bands and run fn(lo, hi) on each; bands write disjoint
// slots so any thread count gives identical results. (Used by refineOffsetsFullRes; the NNF has
// its own chunked variant for progress ticks.) The band arithmetic is unchanged since S39;
// S60-b only runs the bands on the shared pool instead of spawning threads per call.
template <class Fn> void parallelFor(std::size_t count, Fn&& fn) {
    const std::size_t hw = common::hardwareThreads();
    const std::size_t bands =
        std::max<std::size_t>(1, std::min<std::size_t>(hw, std::max<std::size_t>(1, count / 4)));
    if (bands <= 1) {
        fn(static_cast<std::size_t>(0), count);
        return;
    }
    const std::size_t step = (count + bands - 1) / bands;
    common::parallelBands((count + step - 1) / step, [&](std::size_t b) {
        const std::size_t lo = b * step;
        fn(lo, std::min(count, lo + step));
    });
}

// Hand-rolled balanced KD-tree over D-dimensional patch descriptors, for the offset NNF.
// The KD-tree is a 1975 classic (Bentley), and this is our own implementation (no nanoflann / no
// third-party code). Exact k-NN with a reordered index array (implicit nodes); points stay in
// original order so a point can be used as its own query by index.
class KdTree {
public:
    KdTree(std::vector<float> pts, int dim) : m_pts(std::move(pts)), m_dim(dim) {
        const int n =
            m_dim > 0 ? static_cast<int>(m_pts.size() / static_cast<std::size_t>(m_dim)) : 0;
        m_idx.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            m_idx[static_cast<std::size_t>(i)] = i;
        }
        if (n > 0) {
            build(0, n, 0);
        }
    }

    [[nodiscard]] const float* point(int idx) const {
        return &m_pts[static_cast<std::size_t>(idx) * static_cast<std::size_t>(m_dim)];
    }

    // The k nearest point indices (with squared distance), sorted ascending by distance.
    void knn(const float* q, int k, std::vector<std::pair<float, int>>& out) const {
        out.clear();
        if (m_idx.empty() || k <= 0) {
            return;
        }
        std::vector<std::pair<float, int>> heap;
        heap.reserve(static_cast<std::size_t>(k) + 1);
        search(0, static_cast<int>(m_idx.size()), 0, q, k, heap);
        std::sort(heap.begin(), heap.end());
        out = std::move(heap);
    }

private:
    [[nodiscard]] float coord(int idx, int d) const {
        return m_pts[static_cast<std::size_t>(idx) * static_cast<std::size_t>(m_dim) +
                     static_cast<std::size_t>(d)];
    }
    [[nodiscard]] float dist2(const float* q, int idx) const {
        const float* p = point(idx);
        float s = 0.0f;
        for (int i = 0; i < m_dim; ++i) {
            const float d = q[i] - p[i];
            s += d * d;
        }
        return s;
    }
    // Partial-distance search (Bei & Gray, 1985 — public domain): accumulate the squared distance
    // but bail the instant it reaches `bound` (the current k-th-best). High-dimensional descriptors
    // (3·8·8 = 192) make most candidates exceed the bound after only a handful of dims, so this skips
    // the bulk of each comparison. EXACT: the returned value is only used to test `< bound`, and once
    // s >= bound the true distance can't change that test — so the k nearest neighbours are identical
    // to the full-distance result.
    [[nodiscard]] float dist2Bounded(const float* q, int idx, float bound) const {
        const float* p = point(idx);
        float s = 0.0f;
        for (int i = 0; i < m_dim; ++i) {
            const float d = q[i] - p[i];
            s += d * d;
            if (s >= bound) {
                return s; // already too far — its exact distance is irrelevant
            }
        }
        return s;
    }
    void build(int lo, int hi, int depth) {
        if (hi - lo <= 1) {
            return;
        }
        const int d = depth % m_dim;
        const int mid = (lo + hi) / 2;
        std::nth_element(m_idx.begin() + lo, m_idx.begin() + mid, m_idx.begin() + hi,
                         [&](int a, int b) { return coord(a, d) < coord(b, d); });
        build(lo, mid, depth + 1);
        build(mid + 1, hi, depth + 1);
    }
    void consider(const float* q, int idx, int k, std::vector<std::pair<float, int>>& heap) const {
        if (static_cast<int>(heap.size()) < k) {
            // Heap not yet full: no bound to prune against, take the full distance.
            heap.emplace_back(dist2(q, idx), idx);
            std::push_heap(heap.begin(), heap.end());
        } else {
            // Heap full: front() is the current k-th-best (max-heap), our prune bound.
            const float bound = heap.front().first;
            const float d2 = dist2Bounded(q, idx, bound);
            if (d2 < bound) {
                std::pop_heap(heap.begin(), heap.end());
                heap.back() = {d2, idx};
                std::push_heap(heap.begin(), heap.end());
            }
        }
    }
    void search(int lo, int hi, int depth, const float* q, int k,
                std::vector<std::pair<float, int>>& heap) const {
        if (hi <= lo) {
            return;
        }
        if (hi - lo == 1) {
            consider(q, m_idx[static_cast<std::size_t>(lo)], k, heap);
            return;
        }
        const int d = depth % m_dim;
        const int mid = (lo + hi) / 2;
        const int pivot = m_idx[static_cast<std::size_t>(mid)];
        consider(q, pivot, k, heap);
        const float diff = q[d] - coord(pivot, d);
        const int nearLo = diff <= 0 ? lo : mid + 1;
        const int nearHi = diff <= 0 ? mid : hi;
        const int farLo = diff <= 0 ? mid + 1 : lo;
        const int farHi = diff <= 0 ? hi : mid;
        search(nearLo, nearHi, depth + 1, q, k, heap);
        if (static_cast<int>(heap.size()) < k || diff * diff < heap.front().first) {
            search(farLo, farHi, depth + 1, q, k, heap);
        }
    }

    std::vector<float> m_pts;
    int m_dim;
    std::vector<int> m_idx;
};

} // namespace

std::vector<Offset> computeDominantOffsets(const common::ImageF& image, const Selection& holeMask,
                                           const Params& p, const std::atomic<bool>* cancel,
                                           const std::function<bool(float)>& progress) {
    std::vector<Offset> result;
    const int ps = std::max(1, p.patchSize);
    const std::uint32_t w = image.width;
    const std::uint32_t h = image.height;
    if (w < static_cast<std::uint32_t>(ps) || h < static_cast<std::uint32_t>(ps)) {
        return result;
    }
    const double tau = p.tauFraction * static_cast<double>(std::max(w, h));
    const double tau2 = tau * tau;
    const int dim = 3 * ps * ps;

    // Pass 1: positions of every fully-known patch (cheap — no descriptors yet).
    std::vector<std::pair<long, long>> allPos;
    for (std::uint32_t y = 0; y + static_cast<std::uint32_t>(ps) <= h; ++y) {
        for (std::uint32_t x = 0; x + static_cast<std::uint32_t>(ps) <= w; ++x) {
            if (patchKnown(holeMask, x, y, ps)) {
                allPos.emplace_back(static_cast<long>(x), static_cast<long>(y));
            }
        }
    }
    const long total = static_cast<long>(allPos.size());
    if (total < 2) {
        return result;
    }

    // DETERMINISTIC uniform decimation of the patch set to bound the cost. The exact k-NN is O(n^2)
    // in the patch count, so on a real photo the working region (up to ~480k patches) makes the NNF
    // run for hours. He & Sun observe the offset statistics are insensitive to the NNF, so a
    // uniform subset of the patches reproduces the dominant peaks at a cost that no longer grows
    // with image size. The subset is a fixed raster stride — NO randomness — so the result is fully
    // reproducible (Mosaic's golden-test determinism) and, critically, this is a pure
    // data-reduction step: every patch is still matched by an *independent, exact*
    // nearest-neighbour lookup. There is no propagation (a patch's offset is never seeded from a
    // neighbour's) and no offset perturbation — see the invariant at the top of
    // offset_statistics.hpp, which decimation must not weaken.
    const long budget = std::max<long>(2, p.nnfMaxPatches);
    const long stride = (total + budget - 1) / budget; // >= 1; 1 == use every patch (small regions)
    std::vector<std::pair<long, long>> pos;
    pos.reserve(static_cast<std::size_t>((total + stride - 1) / stride));
    for (long i = 0; i < total; i += stride) {
        pos.push_back(allPos[static_cast<std::size_t>(i)]);
    }
    const int n = static_cast<int>(pos.size());

    // Build descriptors for the (decimated) patch set only.
    std::vector<float> desc;
    desc.reserve(static_cast<std::size_t>(n) * static_cast<std::size_t>(dim));
    for (const auto& pxy : pos) {
        const auto px = static_cast<std::uint32_t>(pxy.first);
        const auto py = static_cast<std::uint32_t>(pxy.second);
        for (int dy = 0; dy < ps; ++dy) {
            for (int dx = 0; dx < ps; ++dx) {
                const common::ColorF c = image.at(px + static_cast<std::uint32_t>(dx),
                                                  py + static_cast<std::uint32_t>(dy));
                desc.push_back(c.r);
                desc.push_back(c.g);
                desc.push_back(c.b);
            }
        }
    }

    // KD-tree NNF: for each patch, the nearest OTHER patch (in descriptor space) whose spatial
    // offset exceeds tau; ties broken toward the smaller offset. The queries are independent and
    // read-only on the tree, so they run across hardware threads; each thread accumulates a local
    // offset histogram and they are merged afterwards. Merging is a commutative sum of counts, so
    // the dominant offsets are identical for any thread count (determinism preserved).
    const KdTree tree(std::move(desc), dim);
    constexpr int kKnn = 24;
    constexpr double kEps = 1e-9;

    const unsigned hw = common::hardwareThreads();
    const int nThreads = std::max(1, std::min<int>(static_cast<int>(hw), std::max(1, n / 512)));
    std::vector<std::map<std::pair<int, int>, long>> partial(static_cast<std::size_t>(nThreads));

    const auto worker = [&](int t, int lo, int hi) {
        std::map<std::pair<int, int>, long>& hist = partial[static_cast<std::size_t>(t)];
        std::vector<std::pair<float, int>> out;
        for (int i = lo; i < hi; ++i) {
            // Poll the cancel token every so often (an atomic load is cheap, but not per-query):
            // a cancelled stage just stops accumulating, and the caller discards the result.
            if (cancel != nullptr && (i & 0xFF) == 0 && cancel->load()) {
                return;
            }
            tree.knn(tree.point(i), kKnn, out);
            double bestD2 = std::numeric_limits<double>::infinity();
            double bestMag2 = std::numeric_limits<double>::infinity();
            Offset bestOff{};
            bool found = false;
            for (const auto& [d2, cand] : out) {
                if (cand == i) {
                    continue;
                }
                const long du = pos[static_cast<std::size_t>(cand)].first -
                                pos[static_cast<std::size_t>(i)].first;
                const long dv = pos[static_cast<std::size_t>(cand)].second -
                                pos[static_cast<std::size_t>(i)].second;
                const double mag2 = static_cast<double>(du) * du + static_cast<double>(dv) * dv;
                if (mag2 <= tau2) {
                    continue;
                }
                const double dd = static_cast<double>(d2);
                if (dd < bestD2 - kEps || (std::fabs(dd - bestD2) <= kEps && mag2 < bestMag2)) {
                    bestD2 = dd;
                    bestMag2 = mag2;
                    bestOff = {static_cast<int>(du), static_cast<int>(dv)};
                    found = true;
                }
            }
            if (found) {
                ++hist[{bestOff.u, bestOff.v}];
            }
        }
    };

    // Process the queries in chunks so the (multi-second) NNF can report incremental progress; each
    // chunk is split across the worker threads. The per-thread partial histograms accumulate across
    // chunks and are summed afterwards, so the result is IDENTICAL for any chunking/thread count —
    // each query independently reads the read-only tree, and the counts are a commutative sum
    // (determinism preserved). With no progress sink (headless/test path) a single chunk reproduces
    // the original band split exactly.
    const auto runRange = [&](int lo, int hi) {
        if (nThreads <= 1) {
            worker(0, lo, hi);
            return;
        }
        if (hi <= lo) {
            return; // an empty chunk had no bands to spawn either
        }
        const int step = (hi - lo + nThreads - 1) / nThreads;
        common::parallelBands(static_cast<std::size_t>((hi - lo + step - 1) / step),
                              [&](std::size_t b) {
                                  const int wlo = lo + static_cast<int>(b) * step;
                                  worker(static_cast<int>(b), wlo, std::min(hi, wlo + step));
                              });
    };
    const int nChunks = (progress && n > 0) ? std::min(n, 24) : 1;
    const int chunkSz = (n + nChunks - 1) / nChunks;
    for (int c = 0; c < nChunks; ++c) {
        const int clo = c * chunkSz;
        const int chi = std::min(n, clo + chunkSz);
        if (clo >= chi) {
            break;
        }
        runRange(clo, chi);
        if (cancel != nullptr && cancel->load()) {
            break;
        }
        if (progress && !progress(static_cast<float>(chi) / static_cast<float>(n))) {
            break;
        }
    }

    std::map<std::pair<int, int>, long> hist;
    for (const auto& pm : partial) {
        for (const auto& [k, v] : pm) {
            hist[k] += v;
        }
    }

    // Rank by frequency (desc), tie-break by smaller magnitude then lexicographic offset.
    std::vector<std::pair<std::pair<int, int>, long>> items(hist.begin(), hist.end());
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        const double ma = std::sqrt(static_cast<double>(a.first.first) * a.first.first +
                                    static_cast<double>(a.first.second) * a.first.second);
        const double mb = std::sqrt(static_cast<double>(b.first.first) * b.first.first +
                                    static_cast<double>(b.first.second) * b.first.second);
        if (ma != mb) {
            return ma < mb;
        }
        return a.first < b.first;
    });

    const int K = std::max(1, p.K);
    for (const auto& it : items) {
        if (static_cast<int>(result.size()) >= K) {
            break;
        }
        result.push_back({it.first.first, it.first.second});
    }
    return result;
}

std::vector<Offset> computeBoundaryOffsets(const common::ImageF& image, const Selection& holeMask,
                                           const Params& p, const std::atomic<bool>* cancel) {
    std::vector<Offset> result;
    const int budget = p.boundaryOffsets;
    const int ps = std::max(1, p.patchSize);
    const std::uint32_t w = image.width;
    const std::uint32_t h = image.height;
    if (budget <= 0 || holeMask.isEmpty() || w < static_cast<std::uint32_t>(ps) ||
        h < static_cast<std::uint32_t>(ps)) {
        return result;
    }
    const double tau = p.tauFraction * static_cast<double>(std::max(w, h));
    const double tau2 = tau * tau;
    const int dim = 3 * ps * ps;

    // Summed-area table of the hole mask: ring membership (patch box grown by kRingDist touches
    // the hole) in O(1) per patch.
    std::vector<std::uint32_t> sat(static_cast<std::size_t>(w + 1) * (h + 1), 0);
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint32_t rowSum = 0;
        for (std::uint32_t x = 0; x < w; ++x) {
            rowSum += pixelHole(holeMask, x, y) ? 1u : 0u;
            sat[static_cast<std::size_t>(y + 1) * (w + 1) + (x + 1)] =
                sat[static_cast<std::size_t>(y) * (w + 1) + (x + 1)] + rowSum;
        }
    }
    constexpr int kRingDist = 2; // how far from the hole a patch still counts as "adjacent"
    const auto holeIn = [&](long x0, long y0, long x1, long y1) -> bool {
        x0 = std::max(0L, x0);
        y0 = std::max(0L, y0);
        x1 = std::min(static_cast<long>(w), x1);
        y1 = std::min(static_cast<long>(h), y1);
        if (x0 >= x1 || y0 >= y1) {
            return false;
        }
        const auto at = [&](long x, long y) {
            return sat[static_cast<std::size_t>(y) * (w + 1) + static_cast<std::size_t>(x)];
        };
        return at(x1, y1) - at(x0, y1) - at(x1, y0) + at(x0, y0) > 0;
    };

    // Fully-known patches (the search set, decimated exactly like the dominant-offset NNF) and,
    // among them, the RING patches (the queries).
    std::vector<std::pair<long, long>> allPos;
    std::vector<std::pair<long, long>> ringPos;
    for (std::uint32_t y = 0; y + static_cast<std::uint32_t>(ps) <= h; ++y) {
        for (std::uint32_t x = 0; x + static_cast<std::uint32_t>(ps) <= w; ++x) {
            if (!patchKnown(holeMask, x, y, ps)) {
                continue;
            }
            allPos.emplace_back(static_cast<long>(x), static_cast<long>(y));
            if (holeIn(static_cast<long>(x) - kRingDist, static_cast<long>(y) - kRingDist,
                       static_cast<long>(x) + ps + kRingDist,
                       static_cast<long>(y) + ps + kRingDist)) {
                ringPos.emplace_back(static_cast<long>(x), static_cast<long>(y));
            }
        }
    }
    if (allPos.size() < 2 || ringPos.empty()) {
        return result;
    }
    {
        const long total = static_cast<long>(allPos.size());
        const long searchBudget = std::max<long>(2, p.nnfMaxPatches);
        const long stride = (total + searchBudget - 1) / searchBudget;
        if (stride > 1) {
            std::vector<std::pair<long, long>> dec;
            dec.reserve(static_cast<std::size_t>((total + stride - 1) / stride));
            for (long i = 0; i < total; i += stride) {
                dec.push_back(allPos[static_cast<std::size_t>(i)]);
            }
            allPos.swap(dec);
        }
    }
    constexpr long kQueryBudget = 384; // ring queries: a vote per few boundary patches is plenty
    {
        const long total = static_cast<long>(ringPos.size());
        const long stride = (total + kQueryBudget - 1) / kQueryBudget;
        if (stride > 1) {
            std::vector<std::pair<long, long>> dec;
            dec.reserve(static_cast<std::size_t>((total + stride - 1) / stride));
            for (long i = 0; i < total; i += stride) {
                dec.push_back(ringPos[static_cast<std::size_t>(i)]);
            }
            ringPos.swap(dec);
        }
    }

    const auto descriptorOf = [&](const std::pair<long, long>& pxy, float* out) {
        const auto px = static_cast<std::uint32_t>(pxy.first);
        const auto py = static_cast<std::uint32_t>(pxy.second);
        std::size_t i = 0;
        for (int dy = 0; dy < ps; ++dy) {
            for (int dx = 0; dx < ps; ++dx) {
                const common::ColorF c = image.at(px + static_cast<std::uint32_t>(dx),
                                                  py + static_cast<std::uint32_t>(dy));
                out[i++] = c.r;
                out[i++] = c.g;
                out[i++] = c.b;
            }
        }
    };
    std::vector<float> desc(allPos.size() * static_cast<std::size_t>(dim));
    for (std::size_t i = 0; i < allPos.size(); ++i) {
        descriptorOf(allPos[i], &desc[i * static_cast<std::size_t>(dim)]);
    }
    const KdTree tree(std::move(desc), dim);

    // Each ring query votes its best non-local match; commutative count merge across threads
    // (identical to the dominant-offset NNF's determinism argument).
    constexpr int kKnn = 16;
    const unsigned hw = common::hardwareThreads();
    const int nThreads = std::max(
        1, std::min<int>(static_cast<int>(hw), std::max(1, static_cast<int>(ringPos.size()) / 32)));
    std::vector<std::map<std::pair<int, int>, long>> partial(static_cast<std::size_t>(nThreads));
    std::atomic<bool> bailed{false};
    const auto worker = [&](int t, std::size_t lo, std::size_t hi) {
        std::map<std::pair<int, int>, long>& hist = partial[static_cast<std::size_t>(t)];
        std::vector<float> q(static_cast<std::size_t>(dim));
        std::vector<std::pair<float, int>> out;
        for (std::size_t i = lo; i < hi; ++i) {
            if (cancel != nullptr && (i & 0x1F) == 0 && cancel->load()) {
                bailed.store(true);
                return;
            }
            descriptorOf(ringPos[i], q.data());
            tree.knn(q.data(), kKnn, out);
            for (const auto& [d2, cand] : out) {
                const long du = allPos[static_cast<std::size_t>(cand)].first - ringPos[i].first;
                const long dv = allPos[static_cast<std::size_t>(cand)].second - ringPos[i].second;
                const double mag2 = static_cast<double>(du) * du + static_cast<double>(dv) * dv;
                if (mag2 <= tau2) {
                    continue; // too local (incl. the query itself, if it is in the search set)
                }
                ++hist[{static_cast<int>(du), static_cast<int>(dv)}];
                break; // the nearest non-local match is this query's one vote
            }
        }
    };
    {
        const std::size_t count = ringPos.size();
        if (nThreads <= 1) {
            worker(0, 0, count);
        } else if (count > 0) { // an empty ring had no bands to spawn either
            const std::size_t step = (count + static_cast<std::size_t>(nThreads) - 1) /
                                     static_cast<std::size_t>(nThreads);
            common::parallelBands((count + step - 1) / step, [&](std::size_t b) {
                const std::size_t lo = b * step;
                worker(static_cast<int>(b), lo, std::min(count, lo + step));
            });
        }
    }
    if (bailed.load()) {
        return result;
    }
    std::map<std::pair<int, int>, long> hist;
    for (const auto& pm : partial) {
        for (const auto& [k, v] : pm) {
            hist[k] += v;
        }
    }

    // Same ranking as the dominant offsets: count desc, then smaller magnitude, then lexicographic.
    std::vector<std::pair<std::pair<int, int>, long>> items(hist.begin(), hist.end());
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        const double ma = std::sqrt(static_cast<double>(a.first.first) * a.first.first +
                                    static_cast<double>(a.first.second) * a.first.second);
        const double mb = std::sqrt(static_cast<double>(b.first.first) * b.first.first +
                                    static_cast<double>(b.first.second) * b.first.second);
        if (ma != mb) {
            return ma < mb;
        }
        return a.first < b.first;
    });
    for (const auto& it : items) {
        if (static_cast<int>(result.size()) >= budget) {
            break;
        }
        result.push_back({it.first.first, it.first.second});
    }
    return result;
}

std::vector<Offset> outpaintShiftCandidates(const Selection& regionHoleMask, int pad) {
    std::vector<Offset> result;
    const long rwW = static_cast<long>(regionHoleMask.width());
    const long rwH = static_cast<long>(regionHoleMask.height());
    if (regionHoleMask.isEmpty() || rwW <= 0 || rwH <= 0) {
        return result;
    }
    const std::vector<std::uint8_t>& rm = regionHoleMask.data();
    // Max inward run of hole from one side of the region. Only scan lines that actually REACHED
    // known content count: a ring hole's corner-crossing lines are hole end to end and would
    // report the full extent, tripping the degenerate guard for every side (the first ladder was
    // silently inert because of exactly this).
    const auto holeRun = [&](bool vertical, bool fromEnd) {
        long depth = 0;
        const long outer = vertical ? rwW : rwH;
        const long inner = vertical ? rwH : rwW;
        for (long o = 0; o < outer; ++o) {
            long run = 0;
            for (long i = 0; i < inner; ++i) {
                const long ii = fromEnd ? inner - 1 - i : i;
                const long x = vertical ? o : ii;
                const long y = vertical ? ii : o;
                if (rm[static_cast<std::size_t>(y) * static_cast<std::size_t>(rwW) +
                       static_cast<std::size_t>(x)] == 0) {
                    break;
                }
                ++run;
            }
            if (run < inner) {
                depth = std::max(depth, run);
            }
        }
        return depth;
    };
    const auto addShift = [&](int u, int v) {
        const Offset o{u, v};
        if (std::find(result.begin(), result.end(), o) == result.end()) {
            result.push_back(o);
        }
    };
    const int padded = std::max(1, pad);
    // Geometric rungs spanning the WHOLE known extent: the strip must be able to source content
    // from the far side of any foreground object (a short ladder leaves strip-at-object-height
    // with only object-sourcing candidates — no weight can beat kInvalid, so reach is what
    // matters, not cost).
    const auto ladder = [&](long depth, bool vertical, int sign) {
        const long extent = vertical ? rwH : rwW;
        if (depth <= 0 || depth >= extent) {
            return;
        }
        long mag = depth + padded;
        for (int rung = 0; rung < 10 && mag < extent; ++rung) {
            addShift(vertical ? 0 : sign * static_cast<int>(mag),
                     vertical ? sign * static_cast<int>(mag) : 0);
            mag = static_cast<long>(static_cast<double>(mag) * 1.55) + padded;
        }
    };
    const long dLeft = holeRun(/*vertical=*/false, /*fromEnd=*/false);
    const long dRight = holeRun(false, true);
    const long dTop = holeRun(true, false);
    const long dBottom = holeRun(true, true);
    ladder(dLeft, false, +1);   // left strip: sources to the right
    ladder(dRight, false, -1);  // right strip: sources to the left
    ladder(dTop, true, +1);     // top strip: sources below
    ladder(dBottom, true, -1);  // bottom strip: sources above
    // Corner rungs: where two adjacent sides are both strips, the corner block's axis escapes
    // each land in the OTHER strip (still hole), so it has the fewest valid sources of any
    // outpaint pixel — the "faint far-corner fragment" of the first Broadway sweep. Pair the two
    // sides' ladders into diagonal inward shifts so the corner can slide the diagonally-adjacent
    // known content outward; both magnitudes must clear their own strip depth simultaneously.
    const auto cornerLadder = [&](long depthX, int signX, long depthY, int signY) {
        if (depthX <= 0 || depthX >= rwW || depthY <= 0 || depthY >= rwH) {
            return;
        }
        long magX = depthX + padded;
        long magY = depthY + padded;
        for (int rung = 0; rung < 6 && magX < rwW && magY < rwH; ++rung) {
            addShift(signX * static_cast<int>(magX), signY * static_cast<int>(magY));
            magX = static_cast<long>(static_cast<double>(magX) * 1.55) + padded;
            magY = static_cast<long>(static_cast<double>(magY) * 1.55) + padded;
        }
    };
    cornerLadder(dLeft, +1, dTop, +1);     // top-left corner: sources down-right
    cornerLadder(dRight, -1, dTop, +1);    // top-right corner: sources down-left
    cornerLadder(dLeft, +1, dBottom, -1);  // bottom-left corner: sources up-right
    cornerLadder(dRight, -1, dBottom, -1); // bottom-right corner: sources up-left
    return result;
}

std::vector<Offset> refineOffsetsFullRes(const common::ImageF& image, const Selection& holeMask,
                                         const std::vector<Offset>& offsets, int radius,
                                         int patchSize, const std::atomic<bool>* cancel) {
    if (radius <= 0 || offsets.empty() || image.empty()) {
        return offsets;
    }
    const long W = static_cast<long>(image.width);
    const long H = static_cast<long>(image.height);
    const int ps = std::max(1, patchSize);
    if (W < ps || H < ps) {
        return offsets;
    }

    // Hole bbox grown by one patch: outside it every patch is fully known, inside it a summed-
    // area table over the hole mask answers patch-known in O(1). The SAT covers only this box, so
    // memory stays proportional to the hole, not the image (a 36-MP photo must not cost a
    // full-image table).
    long bx0 = 0, by0 = 0, bx1 = 0, by1 = 0; // grown hole bbox, half-open; empty => no hole
    std::vector<std::uint32_t> sat;          // (bw+1)*(bh+1), row-major
    long bw = 0, bh = 0;
    {
        long hx0 = W, hy0 = H, hx1 = -1, hy1 = -1;
        if (!holeMask.isEmpty()) {
            for (std::uint32_t y = 0; y < holeMask.height(); ++y) {
                for (std::uint32_t x = 0; x < holeMask.width(); ++x) {
                    if (holeMask.at(x, y) > 0) {
                        hx0 = std::min(hx0, static_cast<long>(x));
                        hy0 = std::min(hy0, static_cast<long>(y));
                        hx1 = std::max(hx1, static_cast<long>(x));
                        hy1 = std::max(hy1, static_cast<long>(y));
                    }
                }
            }
        }
        if (hx1 < hx0) {
            return offsets; // no hole: nothing to refine against (callers don't do this)
        }
        bx0 = std::max(0L, hx0 - ps);
        by0 = std::max(0L, hy0 - ps);
        bx1 = std::min(W, hx1 + 1 + ps);
        by1 = std::min(H, hy1 + 1 + ps);
        bw = bx1 - bx0;
        bh = by1 - by0;
        sat.assign(static_cast<std::size_t>(bw + 1) * static_cast<std::size_t>(bh + 1), 0);
        for (long y = 0; y < bh; ++y) {
            std::uint32_t rowSum = 0;
            for (long x = 0; x < bw; ++x) {
                rowSum += pixelHole(holeMask, static_cast<std::uint32_t>(bx0 + x),
                                    static_cast<std::uint32_t>(by0 + y))
                              ? 1u
                              : 0u;
                sat[static_cast<std::size_t>(y + 1) * static_cast<std::size_t>(bw + 1) +
                    static_cast<std::size_t>(x + 1)] =
                    sat[static_cast<std::size_t>(y) * static_cast<std::size_t>(bw + 1) +
                        static_cast<std::size_t>(x + 1)] +
                    rowSum;
            }
        }
    }
    // Is the ps x ps patch at (x, y) fully known and in bounds?
    const auto patchOk = [&](long x, long y) -> bool {
        if (x < 0 || y < 0 || x + ps > W || y + ps > H) {
            return false;
        }
        const long ix0 = std::max(x, bx0), iy0 = std::max(y, by0);
        const long ix1 = std::min(x + ps, bx1), iy1 = std::min(y + ps, by1);
        if (ix0 >= ix1 || iy0 >= iy1) {
            return true; // entirely outside the grown bbox: known by construction
        }
        const auto at = [&](long xx, long yy) {
            return sat[static_cast<std::size_t>(yy - by0) * static_cast<std::size_t>(bw + 1) +
                       static_cast<std::size_t>(xx - bx0)];
        };
        return at(ix1, iy1) - at(ix0, iy1) - at(ix1, iy0) + at(ix0, iy0) == 0;
    };

    // Deterministic sample grid: known patches near the hole (the grown bbox expanded by another
    // patch ring, where the fill's context lives), strided to a fixed budget.
    std::vector<std::pair<long, long>> samples;
    {
        const long sx0 = std::max(0L, bx0 - 2L * ps), sy0 = std::max(0L, by0 - 2L * ps);
        const long sx1 = std::min(W - ps, bx1 + 2L * ps), sy1 = std::min(H - ps, by1 + 2L * ps);
        constexpr long kSampleBudget = 1200;
        const double area = static_cast<double>(std::max(0L, sx1 - sx0)) *
                            static_cast<double>(std::max(0L, sy1 - sy0));
        const long stride = std::max(
            1L, static_cast<long>(std::ceil(std::sqrt(area / static_cast<double>(kSampleBudget)))));
        for (long y = sy0; y <= sy1 - 1; y += stride) {
            for (long x = sx0; x <= sx1 - 1; x += stride) {
                if (patchOk(x, y)) {
                    samples.emplace_back(x, y);
                }
            }
        }
    }
    if (samples.size() < 24) {
        return offsets; // not enough anchored context to score candidates honestly
    }

    // Patch SSD between P(a) and P(b), subsampled every 2 px (exact and identical for every
    // candidate — a pure constant-factor cost reduction, not an approximation of the argmin's
    // inputs relative to each other).
    const auto patchSSD = [&](long ax, long ay, long bx, long by) -> double {
        double s = 0.0;
        for (int dy = 0; dy < ps; dy += 2) {
            for (int dx = 0; dx < ps; dx += 2) {
                const common::ColorF a = image.at(static_cast<std::uint32_t>(ax + dx),
                                                  static_cast<std::uint32_t>(ay + dy));
                const common::ColorF b = image.at(static_cast<std::uint32_t>(bx + dx),
                                                  static_cast<std::uint32_t>(by + dy));
                const double dr = static_cast<double>(a.r) - b.r;
                const double dg = static_cast<double>(a.g) - b.g;
                const double db = static_cast<double>(a.b) - b.b;
                s += dr * dr + dg * dg + db * db;
            }
        }
        return s;
    };

    std::vector<Offset> refined(offsets.size());
    std::atomic<bool> bailed{false};
    const auto refineOne = [&](std::size_t i) {
        const Offset base = offsets[i];
        double bestCost = std::numeric_limits<double>::infinity();
        long bestMag2 = std::numeric_limits<long>::max();
        Offset best = base;
        for (int dv = -radius; dv <= radius; ++dv) {
            for (int du = -radius; du <= radius; ++du) {
                const Offset cand{base.u + du, base.v + dv};
                double sum = 0.0;
                int count = 0;
                for (const auto& [x, y] : samples) {
                    const long tx = x + cand.u;
                    const long ty = y + cand.v;
                    if (!patchOk(tx, ty)) {
                        continue;
                    }
                    sum += patchSSD(x, y, tx, ty);
                    ++count;
                }
                if (count < 24) {
                    continue; // too few anchors for a fair comparison
                }
                const double cost = sum / count;
                const long mag2 = static_cast<long>(du) * du + static_cast<long>(dv) * dv;
                constexpr double kEps = 1e-12;
                if (cost < bestCost - kEps ||
                    (cost <= bestCost + kEps &&
                     (mag2 < bestMag2 ||
                      (mag2 == bestMag2 && (dv < best.v - base.v ||
                                            (dv == best.v - base.v && du < best.u - base.u)))))) {
                    bestCost = cost;
                    bestMag2 = mag2;
                    best = cand;
                }
            }
        }
        refined[i] = best;
    };
    parallelFor(offsets.size(), [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            if (cancel != nullptr && cancel->load()) {
                bailed.store(true);
                return;
            }
            refineOne(i);
        }
    });
    if (bailed.load()) {
        return offsets; // cancelled: hand back the unrefined set; the caller aborts soon anyway
    }

    // Dedup, preserving the input's frequency order (two coarse offsets may snap to one).
    std::vector<Offset> out;
    out.reserve(refined.size());
    for (const Offset& o : refined) {
        bool dup = false;
        for (const Offset& q : out) {
            if (q == o) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out.push_back(o);
        }
    }
    return out;
}

common::ImageF applyOffsetLabels(const common::ImageF& image, const Selection& holeMask,
                                 const std::vector<Offset>& offsets,
                                 const std::vector<int>& labels) {
    common::ImageF out = image; // known pixels stay as-is
    if (image.empty() || offsets.empty()) {
        return out;
    }
    const long w = static_cast<long>(image.width);
    const long h = static_cast<long>(image.height);
    const auto isHole = [&](long x, long y) {
        return pixelHole(holeMask, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    };

    // A hole pixel is filled by copying its label's source whenever that source is IN BOUNDS (the
    // He & Sun composite is a stack of shifted copies of the image). A source that lands in the hole
    // is the graph cut's best effort for a deep-interior pixel and copies existing texture — left
    // as-is. Only an OUT-OF-BOUNDS source (or a negative no-source label) cannot be copied; those
    // pixels are neighbour-filled below so no off-image/removed content is ever left behind. (The
    // caller's correctLabelsForValidity has already redirected most out-of-bounds labels, so this is
    // normally just the rare pixel no in-bounds offset can reach.)
    std::vector<std::uint8_t> needsFill(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                                        0);
    std::vector<std::pair<long, long>> unfilled;
    std::size_t node = 0;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            if (!isHole(static_cast<long>(x), static_cast<long>(y))) {
                continue; // not a hole pixel
            }
            const int label = (node < labels.size()) ? labels[node] : -1;
            ++node;
            bool copied = false;
            if (label >= 0 && static_cast<std::size_t>(label) < offsets.size()) {
                const long sx = static_cast<long>(x) + offsets[static_cast<std::size_t>(label)].u;
                const long sy = static_cast<long>(y) + offsets[static_cast<std::size_t>(label)].v;
                if (sx >= 0 && sy >= 0 && sx < w && sy < h) {
                    out.set(x, y, image.at(static_cast<std::uint32_t>(sx),
                                           static_cast<std::uint32_t>(sy)));
                    copied = true;
                }
            }
            if (!copied) {
                needsFill[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x] = 1;
                unfilled.emplace_back(static_cast<long>(x), static_cast<long>(y));
            }
        }
    }

    // Neighbour-fill the leftovers by iterative averaging over already-filled (known or
    // previously-filled) 4-neighbours — a tiny bounded diffusion. Each sweep reads only the
    // fill-state from BEFORE the sweep (committed after), so it is order-independent and fully
    // deterministic regardless of thread/scan order (preserves golden-image reproducibility).
    if (!unfilled.empty()) {
        const int neigh[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (int sweep = 0; sweep < 64 && !unfilled.empty(); ++sweep) {
            std::vector<std::pair<long, long>> still;
            std::vector<std::pair<long, long>> justFilled;
            for (const auto& [x, y] : unfilled) {
                double r = 0, g = 0, b = 0;
                int n = 0;
                for (const auto& d : neigh) {
                    const long nx = x + d[0];
                    const long ny = y + d[1];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                        continue;
                    }
                    if (needsFill[static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) + nx]) {
                        continue; // neighbour not yet filled (as of this sweep's start)
                    }
                    const common::ColorF c = out.at(static_cast<std::uint32_t>(nx),
                                                    static_cast<std::uint32_t>(ny));
                    r += c.r;
                    g += c.g;
                    b += c.b;
                    ++n;
                }
                if (n > 0) {
                    const common::ColorF old =
                        out.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                    const double inv = 1.0 / n;
                    out.set(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                            {static_cast<float>(r * inv), static_cast<float>(g * inv),
                             static_cast<float>(b * inv), old.a});
                    justFilled.emplace_back(x, y);
                } else {
                    still.emplace_back(x, y);
                }
            }
            for (const auto& [x, y] : justFilled) {
                needsFill[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x] = 0;
            }
            if (justFilled.empty()) {
                break; // nothing reachable this sweep — avoid spinning
            }
            unfilled.swap(still);
        }
    }
    return out;
}

} // namespace mosaic::core::inpaint
