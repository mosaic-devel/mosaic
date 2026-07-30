#include "core/inpaint/backends/he_sun/graph_cut.hpp"

#include <algorithm>
#include <limits>
#include <queue>

namespace mosaic::core::inpaint {

namespace {
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kEps = 1e-12;

// T-LINK MERGE + NORMALIZATION (off by default; see addTermWeights).
bool mergeTLinksEnabled() {
    static const bool on = std::getenv("MOSAIC_INPAINT_PR_TLINK") != nullptr;
    return on;
}

// Restores the pre-2026-07-21 gap heuristic — the one that walked the WHOLE node set on every gap.
// Kept only so the two forms can be A/B'd inside ONE binary, in one minute, on one machine. The
// claim that the bucketed form is a pure bookkeeping change decides whether a full-resolution cut
// is affordable at all, and "trust the commit message" is not a measurement. Both forms lift the
// same nodes to the same height n+1 and decrement m_count identically; this one just takes O(n) to
// find them. Neither is an early termination — the stopping condition is untouched by both.
bool legacyGapEnabled() {
    static const bool on = std::getenv("MOSAIC_INPAINT_PR_LEGACY_GAP") != nullptr;
    return on;
}
} // namespace

MaxFlowGraph::MaxFlowGraph(int numNodes)
    : m_n(numNodes + 2), m_source(numNodes), m_sink(numNodes + 1), m_numNodes(numNodes),
      m_mergeT(mergeTLinksEnabled()) {
    // A 4-neighbour grid move builds ~2 t-link arcs + ~4 n-link arcs per node. Reserving keeps this
    // to a handful of doublings on ONE flat vector instead of a per-node allocation storm.
    m_edges.reserve(static_cast<std::size_t>(numNodes) * 8);
    if (m_mergeT) {
        m_termS.assign(static_cast<std::size_t>(std::max(0, numNodes)), 0.0);
        m_termT.assign(static_cast<std::size_t>(std::max(0, numNodes)), 0.0);
    }
}

void MaxFlowGraph::addArc(int from, int to, double cap, double rev) {
    m_edges.push_back({to, cap});  // arc i     : from -> to
    m_edges.push_back({from, rev}); // arc i ^ 1 : to -> from (its `to` IS i's origin: see arcFrom)
}

void MaxFlowGraph::buildCsr() {
    const std::size_t m = m_edges.size();
    m_head.assign(static_cast<std::size_t>(m_n) + 1, 0);
    m_arcs.resize(m);
    // Count, prefix-sum, then scatter in ARC-INDEX order — which is exactly the order the arcs were
    // added, so every node's arc list keeps its original ordering and Dinic's traversal (and hence
    // the min cut, and hence the labeling) is unchanged.
    for (std::size_t i = 0; i < m; ++i) {
        ++m_head[static_cast<std::size_t>(arcFrom(i)) + 1];
    }
    for (std::size_t u = 0; u < static_cast<std::size_t>(m_n); ++u) {
        m_head[u + 1] += m_head[u];
    }
    std::vector<int> pos(m_head.begin(), m_head.end() - 1);
    for (std::size_t i = 0; i < m; ++i) {
        m_arcs[static_cast<std::size_t>(pos[static_cast<std::size_t>(arcFrom(i))]++)] =
            static_cast<int>(i);
    }
}

// α-expansion calls this MANY times for the same node: once for the pair of data costs, and once
// more for each incident n-link (the Kolmogorov-Zabih a1/a2 unary shares). Each call used to append
// its own arc pair, so a 4-neighbour grid ended up with about eight arc pairs per node where two
// would do -- half the graph, rebuilt once per (label x cycle), is redundant parallel t-links.
//
// With MOSAIC_INPAINT_PR_TLINK the capacities are accumulated per node instead and flushed to a
// single source arc and a single sink arc at solve time. Two exact facts make that safe:
//   - parallel arcs with the same endpoints are interchangeable with one arc of their summed
//     capacity, for the flow value AND for residual reachability (the merged residual is the sum of
//     the parallel residuals, so it is positive exactly when one of them is);
//   - subtracting min(capSource, capSink) from BOTH of a node's t-links subtracts a constant from
//     the energy of every labeling, because exactly one of the pair is cut whichever side the node
//     lands on. The set of minimum cuts is therefore unchanged.
// The pair is normalized after every call, so it is only ever the DIFFERENCE that is carried. That
// also removes the one real numerical hazard: the validity term is 1e9 and the seam terms are of
// order 1, so accumulating them into one sum would round the seam terms away, whereas 1e9 - 1e9
// cancels exactly and leaves the seam terms at full precision.
//
// ⚠ maxflow()'s RETURN VALUE changes under this gate (it is smaller by the sum of the subtracted
// constants). Nothing in the expansion loop reads it -- it wants the cut, not the value -- but a
// caller that wants the true min-cut VALUE must leave the gate off.
void MaxFlowGraph::addTermWeights(int node, double capSource, double capSink) {
    if (m_mergeT) {
        double& s = m_termS[static_cast<std::size_t>(node)];
        double& t = m_termT[static_cast<std::size_t>(node)];
        s += capSource;
        t += capSink;
        const double m = std::min(s, t);
        if (m > 0.0) {
            s -= m;
            t -= m;
        }
        return;
    }
    if (capSource > 0.0) {
        addArc(m_source, node, capSource, 0.0);
    }
    if (capSink > 0.0) {
        addArc(node, m_sink, capSink, 0.0);
    }
}

void MaxFlowGraph::addEdge(int a, int b, double cap, double revCap) {
    addArc(a, b, cap, revCap);
}

// Exact heights: the residual distance from each node to the SINK, by a reverse BFS. Arcs are paired,
// so the residual arc u->v exists iff m_edges[a ^ 1].cap > 0 for the arc a = v->u we can see from v.
// Called once up front and periodically thereafter — the single most important heuristic in
// push-relabel; without it the algorithm is quadratic in practice.
void MaxFlowGraph::globalRelabel() {
    const int n = m_n;
    std::fill(m_height.begin(), m_height.end(), n); // unreachable: can only return excess to source
    std::fill(m_count.begin(), m_count.end(), 0);
    std::vector<int> q(static_cast<std::size_t>(n));
    std::size_t qh = 0, qt = 0;
    m_height[static_cast<std::size_t>(m_sink)] = 0;
    q[qt++] = m_sink;
    while (qh < qt) {
        const int v = q[qh++];
        for (int k = m_head[static_cast<std::size_t>(v)]; k < m_head[static_cast<std::size_t>(v) + 1];
             ++k) {
            const int a = m_arcs[static_cast<std::size_t>(k)];
            const int u = m_edges[static_cast<std::size_t>(a)].to; // arc a: v -> u
            // residual arc u -> v is the REVERSE of a
            if (m_edges[static_cast<std::size_t>(a ^ 1)].cap > kEps &&
                m_height[static_cast<std::size_t>(u)] == n && u != m_sink) {
                m_height[static_cast<std::size_t>(u)] = m_height[static_cast<std::size_t>(v)] + 1;
                q[qt++] = u;
            }
        }
    }
    ++m_stats.globalRelabels;
    m_height[static_cast<std::size_t>(m_source)] = n;
    // Rebuild the height buckets in the same pass that rebuilds m_count: this is the one place
    // where every node's height is rewritten at once, so it is also the one place the bucket lists
    // have to be regenerated wholesale. Linking in DESCENDING node order leaves each list in
    // ASCENDING node order, so the gap heuristic below visits nodes in the same order the old
    // full-node-set scan did.
    std::fill(m_bktHead.begin(), m_bktHead.end(), -1);
    m_hiH = -1;
    for (int v = n - 1; v >= 0; --v) {
        const int h = m_height[static_cast<std::size_t>(v)];
        if (h < 2 * n) {
            ++m_count[static_cast<std::size_t>(h)];
        }
        if (h < n) {
            bktLink(v, h);
            if (h > m_hiH) {
                m_hiH = h;
            }
        }
    }
}

// After the flow: the nodes reachable from the source in the RESIDUAL graph. This set is the SOURCE
// SIDE of the minimum cut — and, crucially, it is the SAME set for every maximum flow (any node
// reachable in the residual must lie on the source side of EVERY min cut, or its augmenting path
// would cross that cut with spare capacity). That is why swapping the solver leaves the labeling
// BIT-IDENTICAL: the cut this reports does not depend on WHICH max flow was found.
void MaxFlowGraph::sourceReach() {
    m_level.assign(static_cast<std::size_t>(m_n), -1);
    std::vector<int> q(static_cast<std::size_t>(m_n));
    std::size_t qh = 0, qt = 0;
    m_level[static_cast<std::size_t>(m_source)] = 0;
    q[qt++] = m_source;
    while (qh < qt) {
        const int u = q[qh++];
        for (int k = m_head[static_cast<std::size_t>(u)]; k < m_head[static_cast<std::size_t>(u) + 1];
             ++k) {
            const Edge& e = m_edges[static_cast<std::size_t>(m_arcs[static_cast<std::size_t>(k)])];
            if (e.cap > kEps && m_level[static_cast<std::size_t>(e.to)] < 0) {
                m_level[static_cast<std::size_t>(e.to)] = m_level[static_cast<std::size_t>(u)] + 1;
                q[qt++] = e.to;
            }
        }
    }
}

// PUSH-RELABEL (Goldberg–Tarjan, JACM 1988) with GLOBAL RELABELING and the GAP heuristic, FIFO
// selection. Replaces the from-scratch Dinic, which was ~O(E·√V) here: Dinic rebuilds a BFS level
// graph EVERY phase and the phase count grows with the grid's diameter, so a full-resolution cut went
// superlinear (the per-node cost tripled from 5 k to 100 k nodes). Push-relabel keeps a
// height function INCREMENTALLY instead of rebuilding it, and never restarts a global search per
// phase.
//
// ⚠⚠ INVARIANT — DO NOT ADD AN EARLY EXIT. It is tempting: α-expansion needs only the CUT, not the
// flow value, so one could bail the moment source and sink are separated and no active node can
// still reach the sink. This solver deliberately does NOT do that. It runs the ordinary two-phase
// algorithm to completion (no active nodes at all, an exact max flow) and takes the cut afterwards.
// That costs a little time and it is a hard constraint on this file, not an oversight — do not
// "optimise" it away.
double MaxFlowGraph::maxflow() {
    m_stats = Stats{};
    if (m_mergeT) {
        // Flush the accumulated t-links. One pass, in node order, after every n-link is already in
        // place -- so the arc set is smaller but the n-link ordering inside each node's list is the
        // one the graph was built with.
        for (int v = 0; v < m_numNodes; ++v) {
            const double s = m_termS[static_cast<std::size_t>(v)];
            if (s > 0.0) {
                addArc(m_source, v, s, 0.0);
            }
            const double t = m_termT[static_cast<std::size_t>(v)];
            if (t > 0.0) {
                addArc(v, m_sink, t, 0.0);
            }
        }
    }
    buildCsr();
    const int n = m_n;
    m_excess.assign(static_cast<std::size_t>(n), 0.0);
    m_height.assign(static_cast<std::size_t>(n), 0);
    m_count.assign(static_cast<std::size_t>(2 * n + 1), 0);
    m_cur.resize(static_cast<std::size_t>(n));
    m_inQ.assign(static_cast<std::size_t>(n), 0);
    m_queue.clear();
    m_queue.reserve(static_cast<std::size_t>(n));
    m_bktHead.assign(static_cast<std::size_t>(n), -1);
    m_bktNext.assign(static_cast<std::size_t>(n), -1);
    m_bktPrev.assign(static_cast<std::size_t>(n), -1);
    m_hiH = -1;

    globalRelabel();
    std::copy(m_head.begin(), m_head.end() - 1, m_cur.begin());

    // ACTIVE-NODE SELECTION. FIFO is the shipped order; highest-label is the classical alternative
    // (Goldberg-Tarjan 1988; Cherkassky-Goldberg 1997 measured it as the better selection rule on
    // most families) and is now nearly free, because the gap fix above already threads every node
    // into a height bucket. Both rules compute an EXACT maximum flow to completion -- this is a
    // choice of which active node to discharge next, NOT a stopping rule, and nothing about the
    // termination condition changes (see the INVARIANT above the function).
    //
    // Selecting a different node next finds a DIFFERENT maximum flow, so this is gated. The cut it
    // reports is nevertheless the same one: the set of nodes reachable from the source in the
    // residual graph is identical for every maximum flow (it is the unique MINIMAL min cut -- any
    // residual-reachable node must be on the source side of every min cut, or its augmenting path
    // would cross that cut with spare capacity). That argument is what made the Dinic -> push-relabel
    // swap bit-identical, and it is what makes this one safe; it is verified against the shipped
    // order on the corpus rather than assumed.
    static const bool hiLabel = std::getenv("MOSAIC_INPAINT_PR_HIGHEST") != nullptr;
    if (hiLabel) {
        m_actHead.assign(static_cast<std::size_t>(2 * n + 2), -1);
        m_actNext.assign(static_cast<std::size_t>(n), -1);
    }
    int actHi = -1;
    const auto fileActive = [&](int v, int h) {
        m_inQ[static_cast<std::size_t>(v)] = 1;
        m_actNext[static_cast<std::size_t>(v)] = m_actHead[static_cast<std::size_t>(h)];
        m_actHead[static_cast<std::size_t>(h)] = v;
        if (h > actHi) {
            actHi = h;
        }
    };
    const auto activate = [&](int v) {
        if (!m_inQ[static_cast<std::size_t>(v)] && v != m_source && v != m_sink &&
            m_excess[static_cast<std::size_t>(v)] > kEps) {
            if (hiLabel) {
                fileActive(v, std::min(m_height[static_cast<std::size_t>(v)], 2 * n + 1));
            } else {
                m_inQ[static_cast<std::size_t>(v)] = 1;
                m_queue.push_back(v);
            }
        }
    };
    const auto push = [&](int u, int k) {
        const int a = m_arcs[static_cast<std::size_t>(k)];
        Edge& e = m_edges[static_cast<std::size_t>(a)];
        const double d = std::min(m_excess[static_cast<std::size_t>(u)], e.cap);
        e.cap -= d;
        m_edges[static_cast<std::size_t>(a ^ 1)].cap += d;
        m_excess[static_cast<std::size_t>(u)] -= d;
        m_excess[static_cast<std::size_t>(e.to)] += d;
        ++m_stats.pushes;
        activate(e.to);
    };

    // Saturate every arc out of the source (the initial preflow).
    for (int k = m_head[static_cast<std::size_t>(m_source)];
         k < m_head[static_cast<std::size_t>(m_source) + 1]; ++k) {
        const int a = m_arcs[static_cast<std::size_t>(k)];
        Edge& e = m_edges[static_cast<std::size_t>(a)];
        if (e.cap > kEps) {
            const double d = e.cap;
            e.cap = 0.0;
            m_edges[static_cast<std::size_t>(a ^ 1)].cap += d;
            m_excess[static_cast<std::size_t>(e.to)] += d;
            m_excess[static_cast<std::size_t>(m_source)] -= d;
            activate(e.to);
        }
    }

    const bool legacyGap = legacyGapEnabled(); // A/B harness only; see legacyGapEnabled()
    // Global relabel every n relabels — the standard cadence; it dominates the run's behaviour.
    const long relabelPeriod = std::max<long>(1, n);
    long relabels = 0;
    std::size_t qh = 0;
    for (;;) {
        int u = -1;
        if (hiLabel) {
            while (actHi >= 0 && m_actHead[static_cast<std::size_t>(actHi)] < 0) {
                --actHi;
            }
            if (actHi < 0) {
                break; // no active node anywhere: an exact maximum flow, both phases complete
            }
            u = m_actHead[static_cast<std::size_t>(actHi)];
            m_actHead[static_cast<std::size_t>(actHi)] =
                m_actNext[static_cast<std::size_t>(u)];
            m_inQ[static_cast<std::size_t>(u)] = 0;
            const int hu = std::min(m_height[static_cast<std::size_t>(u)], 2 * n + 1);
            if (hu != actHi) {
                // The gap heuristic lifted this node while it sat in a stale bucket. Re-file it at
                // its real height; the selection rule is a heuristic, so processing it one step out
                // of order costs nothing but a re-insert.
                fileActive(u, hu);
                continue;
            }
        } else {
            if (qh >= m_queue.size()) {
                break;
            }
            u = m_queue[qh++];
            m_inQ[static_cast<std::size_t>(u)] = 0;
        }
        if (u == m_source || u == m_sink) {
            continue;
        }
        ++m_stats.discharges;
        // Discharge u.
        while (m_excess[static_cast<std::size_t>(u)] > kEps) {
            if (m_cur[static_cast<std::size_t>(u)] >= m_head[static_cast<std::size_t>(u) + 1]) {
                // RELABEL: lift u to one above the lowest residual neighbour.
                const int oldH = m_height[static_cast<std::size_t>(u)];
                int nh = 2 * n;
                for (int k = m_head[static_cast<std::size_t>(u)];
                     k < m_head[static_cast<std::size_t>(u) + 1]; ++k) {
                    const Edge& e =
                        m_edges[static_cast<std::size_t>(m_arcs[static_cast<std::size_t>(k)])];
                    if (e.cap > kEps) {
                        nh = std::min(nh, m_height[static_cast<std::size_t>(e.to)] + 1);
                    }
                }
                if (oldH < 2 * n) {
                    --m_count[static_cast<std::size_t>(oldH)];
                }
                if (oldH < n) {
                    bktUnlink(u, oldH);
                }
                m_height[static_cast<std::size_t>(u)] = nh;
                if (nh < 2 * n) {
                    ++m_count[static_cast<std::size_t>(nh)];
                }
                if (nh < n) {
                    bktLink(u, nh);
                    if (nh > m_hiH) {
                        m_hiH = nh;
                    }
                }
                m_cur[static_cast<std::size_t>(u)] = m_head[static_cast<std::size_t>(u)];
                // GAP: if no node sits at height oldH any more, every node ABOVE it is cut off from
                // the sink — lift them all past n at once instead of relabelling them one by one.
                //
                // ⭐ THE GAP WAS THE SUPERLINEARITY (2026-07-21). This scan used to walk the WHOLE
                // node set on every gap, and gaps are frequent: measured on a 69 k-node full-res
                // cut it fired 339 407 times and visited 23.5 BILLION nodes to lift 494 172 —
                // roughly 70 % of the entire max-flow time, and an O(n) term per gap is exactly
                // what makes the solver scale as ~n^1.6 instead of ~n. Nodes are now threaded into
                // per-height buckets, and m_hiH bounds the highest occupied height below n, so a
                // gap touches only the heights that can hold a liftable node. m_hiH drops to
                // oldH-1 afterwards (every bucket above the gap is now empty and oldH itself is
                // empty by the gap condition), and it only ever rises again on a relabel — so the
                // scanning is paid for by the relabels, not by the node count.
                //
                // This is a BOOKKEEPING change, not an algorithm change: the same node set is
                // lifted to the same height n+1, m_count is decremented for the same heights, and
                // the bucket lists are kept in ascending node order so even the ORDER of the lifts
                // matches the old full-set scan. The resulting flow is bit-identical, not merely
                // cut-identical. It is standard Cherkassky-Goldberg implementation practice
                // (1997), no new mechanism, and in particular it is NOT an early termination:
                // nothing about the stopping condition changes.
                if (oldH < n && m_count[static_cast<std::size_t>(oldH)] == 0) {
                    ++m_stats.gaps;
                    if (legacyGap) {
                        m_stats.gapScanned += n;
                        for (int v = 0; v < n; ++v) {
                            const int hv = m_height[static_cast<std::size_t>(v)];
                            if (hv > oldH && hv < n) {
                                --m_count[static_cast<std::size_t>(hv)];
                                m_height[static_cast<std::size_t>(v)] = n + 1;
                                bktUnlink(v, hv);
                                ++m_stats.gapLifted;
                            }
                        }
                    } else {
                        for (int h = oldH + 1; h <= m_hiH; ++h) {
                            ++m_stats.gapScanned;
                            int v = m_bktHead[static_cast<std::size_t>(h)];
                            while (v >= 0) {
                                const int nx = m_bktNext[static_cast<std::size_t>(v)];
                                --m_count[static_cast<std::size_t>(h)];
                                m_height[static_cast<std::size_t>(v)] = n + 1;
                                ++m_stats.gapLifted;
                                v = nx;
                            }
                            m_bktHead[static_cast<std::size_t>(h)] = -1;
                        }
                    }
                    m_hiH = oldH - 1;
                }

                ++m_stats.relabels;
                if (++relabels % relabelPeriod == 0) {
                    globalRelabel();
                    std::copy(m_head.begin(), m_head.end() - 1, m_cur.begin());
                }
                if (m_height[static_cast<std::size_t>(u)] >= 2 * n) {
                    break; // stranded: its excess can go nowhere at all
                }
                continue;
            }
            const int k = m_cur[static_cast<std::size_t>(u)];
            const Edge& e = m_edges[static_cast<std::size_t>(m_arcs[static_cast<std::size_t>(k)])];
            if (e.cap > kEps && m_height[static_cast<std::size_t>(u)] ==
                                    m_height[static_cast<std::size_t>(e.to)] + 1) {
                push(u, k);
            } else {
                ++m_cur[static_cast<std::size_t>(u)];
            }
        }
        // Compact the FIFO so it cannot grow without bound on a long run.
        if (!hiLabel && qh > m_queue.size() / 2 && qh > 1024) {
            m_queue.erase(m_queue.begin(), m_queue.begin() + static_cast<long>(qh));
            qh = 0;
        }
    }

    // The cut. Residual reachability from the source — the same set for ANY max flow, which is what
    // keeps the labeling identical across a solver swap.
    sourceReach();
    return m_excess[static_cast<std::size_t>(m_sink)];
}

bool MaxFlowGraph::inSourceSet(int node) const {
    return !m_level.empty() && m_level[static_cast<std::size_t>(node)] >= 0;
}

std::vector<int> alphaExpansion(int numNodes, int numLabels,
                                const std::function<double(int, int)>& dataCost,
                                const std::function<double(int, int, int)>& pairwiseCost,
                                const std::vector<std::pair<int, int>>& edges, int maxCycles) {
    // Thin forwarder: the move loop lives in the header template so callers with hot, inlinable
    // cost functors (the offset solver) avoid std::function indirection. This overload keeps the
    // generic std::function entry point for tests and any non-perf-critical user.
    return alphaExpansionImpl(numNodes, numLabels, dataCost, pairwiseCost, edges, maxCycles);
}

std::vector<int> alphaExpansion(int numNodes, int numLabels,
                                const std::function<double(int, int)>& dataCost,
                                const std::function<double(int, int)>& smoothCost,
                                const std::vector<GraphCutEdge>& edges, int maxCycles) {
    // Adapt to the per-edge core: fold each edge's weight into a position-independent pairwise
    // cost.
    std::vector<std::pair<int, int>> pairs;
    pairs.reserve(edges.size());
    for (const auto& e : edges) {
        pairs.emplace_back(e.a, e.b);
    }
    const std::function<double(int, int, int)> pw = [&edges, &smoothCost](int i, int la, int lb) {
        return edges[static_cast<std::size_t>(i)].weight * smoothCost(la, lb);
    };
    return alphaExpansion(numNodes, numLabels, dataCost, pw, pairs, maxCycles);
}

} // namespace mosaic::core::inpaint
