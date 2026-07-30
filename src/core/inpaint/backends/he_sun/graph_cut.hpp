#pragma once

// Generic graph-cut optimization for the He & Sun graph-completion solver (PLAN S37-c, §7.5).
//
// Foundation: a from-scratch max-flow / min-cut — our own code.
//
// ⚠ LICENCE CORRECTION (2026-07-12) — this header used to justify the from-scratch solver as
// avoiding "the GCO/Kolmogorov-maxflow research-license issue". THAT WAS FACTUALLY WRONG and it
// steered a real engineering decision. Kolmogorov's `maxflow-v3.0x` ships under the **GPLv3** (its
// own graph.h says so verbatim; the tarball carries GPL.TXT) — fully compatible with Mosaic. The
// research-only licence belongs to **GCO**, a DIFFERENT library ("...can be used and distributed
// for research purposes only... Commercial use ... is not permited"), which is correctly avoided.
// ⚠ THE TRAP WORTH KEEPING: gco-v3.0 BUNDLES a copy of the same BK sources (maxflow.cpp/graph.cpp)
// under GCO's research-only terms. Same files, GPLv3 from Kolmogorov's tarball, research-only from
// inside GCO's. PROVENANCE decides the licence, not the filename.
// Our own solver stays — but for engineering reasons now, and nobody should re-derive a false
// constraint from this comment again.
//
// α-EXPANSION PLAN (next S37-c step, builds directly on MaxFlowGraph):
//   for each candidate label α, over the variable (hole) pixels:
//     - t-links: source/sink capacities = data cost of keeping the current label vs switching to α
//       (E_d(x,a) = 0 if x+s_a is known, +inf otherwise — the He & Sun validity term);
//     - n-links: pairwise seam-coherence cost E_s between 4-neighbours, truncated so each expansion
//       move stays submodular (standard Boykov–Veksler–Zabih construction);
//     - solve maxflow(); relabel pixels by inSourceSet(); accept the move if total energy E(L) =
//       Σ E_d(L(x)) + Σ E_s(L(x),L(x')) decreased; sweep all labels to convergence.
// Then Poisson-blend the seams and rescale offsets by the working-region scale
// (extractWorkingRegion).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <utility>
#include <vector>

namespace mosaic::core::inpaint {

// A two-terminal flow network for one expansion move. Regular nodes are 0..numNodes-1; the source
// and sink terminals are internal. Build with addTermWeights (t-links) + addEdge (n-links), then
// call maxflow(); afterwards inSourceSet(node) reports the min-cut side.
class MaxFlowGraph {
public:
    explicit MaxFlowGraph(int numNodes);

    // Terminal capacities for a node: capSource = source->node, capSink = node->sink.
    void addTermWeights(int node, double capSource, double capSink);

    // An undirected edge between two regular nodes, with a capacity in each direction.
    void addEdge(int a, int b, double cap, double revCap);

    // Computes the maximum flow (== min-cut value) and returns it.
    double maxflow();

    // Valid after maxflow(): true if `node` lies on the source side of the min cut.
    [[nodiscard]] bool inSourceSet(int node) const;

    // Byte-inert instrumentation. The α-expansion move loop was measured to spend 97-98 % of its
    // time inside maxflow(), and that time is still SUPERLINEAR in the node count (~n^1.6 measured
    // over a 16x node ladder) even though push-relabel was supposed to have removed the
    // superlinearity. These counters say which term is responsible; `gapScanned` in particular
    // separates "the gap heuristic fired often" from "the gap heuristic costs O(n) every time it
    // fires", which want opposite fixes. Nothing reads them unless a caller asks.
    struct Stats {
        long relabels = 0;
        long gaps = 0;        // gap-heuristic invocations
        long gapLifted = 0;  // nodes a gap actually raised
        long gapScanned = 0; // height buckets a gap walked (was: the whole node set, per gap)
        long globalRelabels = 0;
        long pushes = 0;
        long discharges = 0;
    };
    [[nodiscard]] const Stats& stats() const {
        return m_stats;
    }

private:
    struct Edge {
        int to;
        double cap;
    };

    void addArc(int from, int to, double cap, double rev);
    void buildCsr();      // one-shot, at the top of maxflow()
    void globalRelabel(); // exact heights = residual distance to the sink (reverse BFS)
    void sourceReach();   // residual reachability from the source -> m_level (the min cut)
    // Height-bucket bookkeeping for the gap heuristic (graph_cut.cpp). A node is in exactly one
    // bucket while its height is below m_n, and in none once it is lifted past it.
    void bktLink(int v, int h) {
        m_bktPrev[static_cast<std::size_t>(v)] = -1;
        const int head = m_bktHead[static_cast<std::size_t>(h)];
        m_bktNext[static_cast<std::size_t>(v)] = head;
        if (head >= 0) {
            m_bktPrev[static_cast<std::size_t>(head)] = v;
        }
        m_bktHead[static_cast<std::size_t>(h)] = v;
    }
    void bktUnlink(int v, int h) {
        const int p = m_bktPrev[static_cast<std::size_t>(v)];
        const int nx = m_bktNext[static_cast<std::size_t>(v)];
        if (p >= 0) {
            m_bktNext[static_cast<std::size_t>(p)] = nx;
        } else {
            m_bktHead[static_cast<std::size_t>(h)] = nx;
        }
        if (nx >= 0) {
            m_bktPrev[static_cast<std::size_t>(nx)] = p;
        }
    }

    // The origin of arc i, without storing it: arcs are added in pairs, so i^1 is i's reverse and
    // its `to` IS i's `from`.
    [[nodiscard]] int arcFrom(std::size_t i) const {
        return m_edges[i ^ 1].to;
    }

    int m_n; // total nodes incl. terminals
    int m_source;
    int m_sink;
    std::vector<Edge> m_edges; // paired: index i and i^1 are forward/back
    // Adjacency as a flat CSR (m_arcs[m_head[u] .. m_head[u+1]) = u's arc indices, in the order they
    // were added). This WAS a vector<vector<int>>: one heap allocation per node, growing by
    // push_back — and α-expansion rebuilds the whole graph once per (label × cycle), so a 2.4 M-node
    // hole meant ~2.4 M little vectors reallocated ~190 times over. That allocator traffic, not
    // Dinic, was what made the full-resolution cut superlinear. The CSR is two flat
    // arrays. Filling it in ARC-INDEX order preserves each node's arc ordering exactly, so Dinic
    // walks the same arcs in the same order and the min-cut — and therefore the labeling — is
    // BIT-IDENTICAL.
    std::vector<int> m_head;  // size m_n + 1
    std::vector<int> m_arcs;  // size m_edges.size()
    std::vector<int> m_level; // after maxflow: >= 0 exactly for the min cut's SOURCE side
    // Push-relabel state (replaced Dinic).
    std::vector<double> m_excess;
    std::vector<int> m_height;
    std::vector<int> m_cur;   // current-arc pointers (absolute indices into m_arcs)
    std::vector<int> m_count; // nodes at each height, for the gap heuristic
    std::vector<int> m_queue; // FIFO of active nodes
    std::vector<char> m_inQ;
    // Height buckets: m_bktHead[h] is the first node whose height is h (h < m_n), threaded through
    // m_bktNext / m_bktPrev. m_hiH is an UPPER BOUND on the largest occupied height below m_n; it
    // is what makes the gap heuristic's "lift everything above the gap" step cost the number of
    // nodes it actually lifts instead of a full pass over the node set.
    std::vector<int> m_bktHead;
    std::vector<int> m_bktNext;
    std::vector<int> m_bktPrev;
    int m_hiH = -1;
    // ACTIVE-node buckets, used only by the optional highest-label selection rule. Separate from
    // the height buckets above: those hold every node, these hold only the ones carrying excess.
    std::vector<int> m_actHead;
    std::vector<int> m_actNext;
    // Optional t-link accumulation (see addTermWeights). One source and one sink capacity per
    // node, kept in normalized form (at most one of the pair is nonzero), flushed to arcs once.
    std::vector<double> m_termS;
    std::vector<double> m_termT;
    int m_numNodes;
    bool m_mergeT;
    Stats m_stats;
};

// One n-link (pairwise) edge of the labeling problem: nodes a,b with a nonnegative weight.
struct GraphCutEdge {
    int a;
    int b;
    double weight;
};

// Multi-label energy minimization by α-expansion, built on MaxFlowGraph. Minimizes
//   E(L) = Σ_p dataCost(p, L[p]) + Σ_edges weight * smoothCost(L[a], L[b])
// for a metric smoothCost (triangle inequality; smoothCost(x,x) == 0). Each expansion move's
// pairwise terms use the Kolmogorov–Zabih binary representation (no auxiliary nodes); a non-metric
// term is truncated to keep the move submodular. Sweeps labels until a full cycle stops improving
// (at most maxCycles). Returns the final per-node labeling (initialized to the per-node argmin of
// the data cost). This is the engine the He & Sun graph-completion solver drives with offset
// labels (data = patch validity, smooth = seam coherence).
[[nodiscard]] std::vector<int>
alphaExpansion(int numNodes, int numLabels,
               const std::function<double(int node, int label)>& dataCost,
               const std::function<double(int labelA, int labelB)>& smoothCost,
               const std::vector<GraphCutEdge>& edges, int maxCycles = 12);

// Per-edge variant: pairwiseCost(edgeIndex, labelA, labelB) lets the smoothness term depend on the
// specific edge — e.g. He & Sun's position-dependent seam cost, where the penalty depends on the
// actual pixels the two neighbours would copy under each offset, not just the label pair. `edges`
// are node-index pairs. Same Kolmogorov–Zabih representation and submodular-truncation semantics.
[[nodiscard]] std::vector<int>
alphaExpansion(int numNodes, int numLabels,
               const std::function<double(int node, int label)>& dataCost,
               const std::function<double(int edgeIndex, int labelA, int labelB)>& pairwiseCost,
               const std::vector<std::pair<int, int>>& edges, int maxCycles = 12);

// Core α-expansion move loop, templated on the cost callables so they INLINE. The std::function
// overloads above forward here; the offset-statistics solver instead passes raw lambdas over its
// precomputed cost arrays, because at K≈60 labels the move loop evaluates the costs hundreds of
// times per node/edge and std::function indirection dominated the runtime. Identical energy model:
//   dataCost(node, label) -> double          (e.g. +inf for an invalid offset)
//   pairwiseCost(edgeIndex, labelA, labelB) -> double  (0 when labelA == labelB)
// Each move builds the Kolmogorov–Zabih binary graph, solves a Dinic min-cut, and accepts the
// relabeling only if the total energy strictly decreases; a non-submodular pairwise term is
// truncated to keep the move solvable. Returns the final per-node labeling.
template <class DataCost, class PairwiseCost>
[[nodiscard]] std::vector<int>
alphaExpansionImpl(int numNodes, int numLabels, DataCost dataCost, PairwiseCost pairwiseCost,
                   const std::vector<std::pair<int, int>>& edges, int maxCycles = 12,
                   int* outCycles = nullptr, const std::atomic<bool>* cancel = nullptr,
                   const std::function<bool(float)>& progress = {}) {
    std::vector<int> f(static_cast<std::size_t>(std::max(0, numNodes)), 0);
    if (numNodes <= 0 || numLabels <= 0) {
        return f;
    }
    const auto energyOf = [&](const std::vector<int>& g) -> double {
        double e = 0.0;
        for (int p = 0; p < numNodes; ++p) {
            e += dataCost(p, g[static_cast<std::size_t>(p)]);
        }
        for (std::size_t i = 0; i < edges.size(); ++i) {
            e += pairwiseCost(static_cast<int>(i), g[static_cast<std::size_t>(edges[i].first)],
                              g[static_cast<std::size_t>(edges[i].second)]);
        }
        return e;
    };

    // Initialize each node to the cheapest data-cost label.
    for (int p = 0; p < numNodes; ++p) {
        int best = 0;
        double bestCost = dataCost(p, 0);
        for (int l = 1; l < numLabels; ++l) {
            const double c = dataCost(p, l);
            if (c < bestCost) {
                bestCost = c;
                best = l;
            }
        }
        f[static_cast<std::size_t>(p)] = best;
    }
    double curE = energyOf(f);

    // α-expansion converges fast: the first cycle removes the bulk of the energy and later cycles
    // find only negligible improvements (measured: cycle 2 is visually identical to cycle 4). Stop
    // once a full cycle improves the energy by less than this fraction of the FIRST cycle's gain —
    // adaptive to each problem's scale (robust to the constant +inf terms of stuck-invalid pixels,
    // which cancel in the per-cycle delta), so we don't burn K max-flows per wasted cycle.
    constexpr double kCycleConvergeFrac = 0.02;
    double firstReduction = 0.0;

    // Byte-inert move-loop profile. The stage timer only ever showed one "solve" number, so nobody
    // could tell whether an expansion move spends its time BUILDING its graph, pushing flow, or
    // re-evaluating the energy of the candidate labeling. Those three want completely different
    // fixes, so they get three counters. Its own env var, not MOSAIC_INPAINT_TIMING: the banded
    // seam re-cuts drive this same template once per seam PAIR, so folding it into the general
    // timing flag would bury the stage lines under thousands of band reports. Read once per
    // process, so the per-call cost is a load.
    static const bool xpDbg = std::getenv("MOSAIC_INPAINT_XP_TIMING") != nullptr;
    double msBuild = 0.0, msFlow = 0.0, msApply = 0.0, msEnergy = 0.0;
    long moves = 0;
    long stRelabel = 0, stGaps = 0, stGapScan = 0, stGapLift = 0, stGlobal = 0, stPush = 0,
         stDischarge = 0;
    using XpClock = std::chrono::steady_clock;
    const auto clk = []() { return xpDbg ? XpClock::now() : XpClock::time_point{}; };
    const auto since = [](XpClock::time_point t0) {
        return xpDbg ? std::chrono::duration<double, std::milli>(XpClock::now() - t0).count() : 0.0;
    };

    for (int cycle = 0; cycle < maxCycles; ++cycle) {
        if (cancel != nullptr && cancel->load()) {
            break; // cancelled: stop refining; the caller discards the (partial) labeling
        }
        const double cycleStartE = curE;
        bool improved = false;
        for (int alpha = 0; alpha < numLabels; ++alpha) {
            const auto tB0 = clk();
            MaxFlowGraph g(numNodes);
            for (int p = 0; p < numNodes; ++p) {
                g.addTermWeights(p, dataCost(p, alpha),
                                 dataCost(p, f[static_cast<std::size_t>(p)]));
            }
            for (std::size_t i = 0; i < edges.size(); ++i) {
                const int ei = static_cast<int>(i);
                const int p = edges[i].first;
                const int q = edges[i].second;
                const int fp = f[static_cast<std::size_t>(p)];
                const int fq = f[static_cast<std::size_t>(q)];
                const double e00 = pairwiseCost(ei, fp, fq);
                const double e01 = pairwiseCost(ei, fp, alpha);
                const double e10 = pairwiseCost(ei, alpha, fq);
                const double e11 = pairwiseCost(ei, alpha, alpha);
                const double a1 = e10 - e00;
                const double a2 = e11 - e10;
                double beta = e01 + e10 - e00 - e11;
                if (beta < 0.0) {
                    beta = 0.0; // truncate a non-submodular term to keep the move solvable
                }
                if (a1 >= 0.0) {
                    g.addTermWeights(p, a1, 0.0);
                } else {
                    g.addTermWeights(p, 0.0, -a1);
                }
                if (a2 >= 0.0) {
                    g.addTermWeights(q, a2, 0.0);
                } else {
                    g.addTermWeights(q, 0.0, -a2);
                }
                if (beta > 0.0) {
                    g.addEdge(p, q, beta, 0.0);
                }
            }
            msBuild += since(tB0);
            const auto tF0 = clk();
            g.maxflow();
            msFlow += since(tF0);
            ++moves;
            if (xpDbg) {
                const MaxFlowGraph::Stats& s = g.stats();
                stRelabel += s.relabels;
                stGaps += s.gaps;
                stGapScan += s.gapScanned;
                stGapLift += s.gapLifted;
                stGlobal += s.globalRelabels;
                stPush += s.pushes;
                stDischarge += s.discharges;
            }
            const auto tA0 = clk();
            std::vector<int> nf = f;
            for (int p = 0; p < numNodes; ++p) {
                if (!g.inSourceSet(p)) {
                    nf[static_cast<std::size_t>(p)] = alpha;
                }
            }
            msApply += since(tA0);
            const auto tE0 = clk();
            const double ne = energyOf(nf);
            msEnergy += since(tE0);
            if (ne < curE - 1e-9) {
                f = std::move(nf);
                curE = ne;
                improved = true;
            }
            // Per-label progress tick (the move loop is the expensive "Solving" stage). progress()
            // returning false is a cancel request — return the best labeling found so far.
            if (progress) {
                const float frac = static_cast<float>(cycle * numLabels + alpha + 1) /
                                   static_cast<float>(std::max(1, maxCycles) * numLabels);
                if (!progress(frac)) {
                    if (outCycles != nullptr) {
                        *outCycles = cycle + 1;
                    }
                    return f;
                }
            }
        }
        if (outCycles != nullptr) {
            *outCycles = cycle + 1;
        }
        if (!improved) {
            break;
        }
        const double reduction = cycleStartE - curE;
        if (cycle == 0) {
            firstReduction = reduction;
        } else if (reduction < kCycleConvergeFrac * firstReduction) {
            break; // diminishing returns — further cycles won't change the result visibly
        }
    }
    if (xpDbg) {
        std::fprintf(stderr,
                     "  [expansion] n=%d K=%d E=%zu moves=%ld build=%.0fms flow=%.0fms "
                     "apply=%.0fms energy=%.0fms\n"
                     "  [expansion] relabels=%ld gaps=%ld gapScanned=%ld gapLifted=%ld "
                     "globalRelabels=%ld pushes=%ld discharges=%ld\n",
                     numNodes, numLabels, edges.size(), moves, msBuild, msFlow, msApply, msEnergy,
                     stRelabel, stGaps, stGapScan, stGapLift, stGlobal, stPush, stDischarge);
    }
    return f;
}

} // namespace mosaic::core::inpaint
