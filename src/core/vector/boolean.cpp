#include "core/vector/boolean.hpp"

#include "core/vector/flatten.hpp"
#include "core/vector/hit.hpp"     // fillRuleOf: a child's own rule feeds the operand predicate
#include "core/vector/to_path.hpp" // pathFromGeometry / transformedPath: the exact rebase

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>

// See boolean.hpp for the prior-art lineage and the contract. The pipeline, in order:
//
//   1. SNAP-ROUND every operand's flattened points onto a fixed integer lattice. From here on the
//      whole kernel is integer arithmetic and every predicate is exact -- which is the point:
//      coincident edges become bit-identical, and "does this vertex lie on that edge" has an
//      answer rather than a tolerance.
//   2. REFINE the edge set until three invariants hold: (I1) no two fragments properly cross,
//      (I2) no vertex lies in the interior of a fragment, (I3) two collinear fragments either
//      coincide exactly or share at most an endpoint. Each round splits at the offending points
//      and re-snaps; snapping can create new work, so the rounds repeat until clean.
//   3. MERGE fragments that share the same undirected endpoint pair into ONE representative
//      carrying a per-operand net winding delta. This is where coincident edges are resolved --
//      exactly, by arithmetic, not by a tie-break.
//   4. CLASSIFY each representative: evaluate every operand's winding number infinitesimally off
//      each side and keep the fragment only when the op's predicate DISAGREES across it. The
//      infinitesimal is symbolic (a sign, not a number), so there is still no epsilon anywhere.
//   5. LINK the surviving directed fragments into rings, then normalize (containment depth ->
//      outer/hole -> forced orientation -> canonical start vertex).
//
// Complexity is O(E^2) in the flattened edge count, with an x-order prune on the pair sweep and a
// y-range reject in the winding scan. That is the right trade for editor-scale shapes (a boolean
// of two flattened primitives is a few hundred edges); a full Bentley-Ottmann status structure is
// the optimization if a boolean of two very dense paths at extreme zoom ever shows up in a
// profile. Nothing about the RESULT would change.
namespace mosaic::core::vec {
namespace {

using common::Affine2D;
using common::Vec2;

using Int = std::int64_t;

// ---------------------------------------------------------------------------------------------
// The lattice, and the exact integer predicates it buys
// ---------------------------------------------------------------------------------------------
// Nominal resolution: 1/1024 of a layer-local unit. flatten() subdivides to `tolerancePx` DEVICE
// pixels, so the lattice is finer than the flattening tolerance by ~8x up to about 3200% zoom and
// stays finer than it up to ~25600% -- i.e. the boolean never becomes the coarse step in the
// pipeline at any zoom a user can reach. See docs/vector-model.md §9.
constexpr Int kLatticePerUnit = 1024;

// Every stored coordinate is DOUBLED (see toLattice), so this is the bound on the pre-doubling
// lattice value: coordinates stay within +-2^27 and every predicate below is a difference of
// products of magnitude <= 2^56, exact in int64 with 6 bits to spare.
constexpr Int kMaxLattice = Int{1} << 26;

// Snap-and-resplit converges in one or two rounds in practice. The budget is headroom for the
// cascade a ROUNDED crossing can start: routing both chains through one lattice vertex moves each
// of them by up to half a cell, which can put a fragment on the far side of something else.
constexpr int kMaxRefineRounds = 12;
constexpr std::size_t kMaxEdges = std::size_t{1} << 17;  // refinement stops growing past this

struct IPt {
    Int x = 0;
    Int y = 0;

    friend bool operator==(IPt, IPt) = default;
};

bool lexLess(IPt a, IPt b) { return a.x != b.x ? a.x < b.x : a.y < b.y; }

IPt sub(IPt a, IPt b) { return {a.x - b.x, a.y - b.y}; }
Int cross(IPt a, IPt b) { return a.x * b.y - a.y * b.x; }
Int dot(IPt a, IPt b) { return a.x * b.x + a.y * b.y; }

// Exact orientation of the triple: > 0, 0 or < 0. The lattice is what makes this exact -- see the
// header's predicate note (Shewchuk 1997 is the fallback lineage if the lattice is ever dropped).
Int orient2d(IPt a, IPt b, IPt c) { return cross(sub(b, a), sub(c, a)); }

int sgn(Int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

// Layer-local <-> lattice. Coordinates are stored DOUBLED so that the midpoint of any fragment is
// itself an exact integer -- the classification step in §4 evaluates windings at fragment
// midpoints, and a half-integer there would cost either a rational or a rounding decision.
struct Lattice {
    double scale = static_cast<double>(kLatticePerUnit);

    [[nodiscard]] IPt toLattice(Vec2 p) const {
        const double lim = static_cast<double>(kMaxLattice);
        const double sx = std::clamp(p.x * scale, -lim, lim);
        const double sy = std::clamp(p.y * scale, -lim, lim);
        return {2 * static_cast<Int>(std::llround(sx)), 2 * static_cast<Int>(std::llround(sy))};
    }

    // Round a doubled-lattice position to the nearest lattice point (an EVEN doubled coordinate).
    [[nodiscard]] IPt snap(double x, double y) const {
        const double lim = 2.0 * static_cast<double>(kMaxLattice);
        const double cx = std::clamp(x, -lim, lim);
        const double cy = std::clamp(y, -lim, lim);
        return {2 * static_cast<Int>(std::llround(cx * 0.5)),
                2 * static_cast<Int>(std::llround(cy * 0.5))};
    }

    [[nodiscard]] Vec2 toLocal(IPt p) const {
        const double inv = 1.0 / (2.0 * scale);
        return {static_cast<double>(p.x) * inv, static_cast<double>(p.y) * inv};
    }
};

// The finest power-of-two lattice that keeps `maxAbs` inside the exactness budget. Halving keeps
// the scale a power of two, so toLocal() is an exact inverse of toLattice()'s rounding grid.
double chooseScale(double maxAbs) {
    double s = static_cast<double>(kLatticePerUnit);
    const double lim = static_cast<double>(kMaxLattice);
    for (int i = 0; i < 40 && s > 1.0 && maxAbs * s > lim; ++i) s *= 0.5;
    return s;
}

// ---------------------------------------------------------------------------------------------
// §2 -- the arrangement
// ---------------------------------------------------------------------------------------------

struct Edge {
    IPt a, b;
    int operand = 0;
};

bool onSegment(IPt p, IPt a, IPt b) {
    if (orient2d(a, b, p) != 0) return false;
    return std::min(a.x, b.x) <= p.x && p.x <= std::max(a.x, b.x) && std::min(a.y, b.y) <= p.y &&
           p.y <= std::max(a.y, b.y);
}

// A point that must become a vertex of `e`: on it, but not already an end of it.
bool strictlyInside(IPt p, const Edge& e) {
    return !(p == e.a) && !(p == e.b) && onSegment(p, e.a, e.b);
}

// `p` pulled onto an end of `e` when it projects to, or past, that end. Exact -- the projection is
// an integer dot product, never a distance. This is the guard a SPLIT point needs, and it is a
// different question from onSegment(): a split has to lie within its edge's own span or the two
// fragments it makes would overlap and double back, but it does NOT have to lie exactly on the
// line, because a rounded crossing never does.
IPt anchorToEnd(IPt p, const Edge& e) {
    const IPt d = sub(e.b, e.a);
    if (dot(d, sub(p, e.a)) <= 0) return e.a;
    if (dot(d, sub(p, e.b)) >= 0) return e.b;
    return p;
}

// The snapped meeting point of two edges whose INTERIORS cross (sides strictly opposite both
// ways). Collinear and touching-at-a-vertex cases are deliberately not this function's business --
// they are covered by the strictlyInside sweep, which is exact and needs no intersection at all.
bool properCross(const Lattice& lat, const Edge& e, const Edge& f, IPt& out) {
    const Int d1 = orient2d(e.a, e.b, f.a);
    const Int d2 = orient2d(e.a, e.b, f.b);
    const Int d3 = orient2d(f.a, f.b, e.a);
    const Int d4 = orient2d(f.a, f.b, e.b);
    if (sgn(d1) * sgn(d2) >= 0 || sgn(d3) * sgn(d4) >= 0) return false;
    const IPt r = sub(e.b, e.a);
    const IPt s = sub(f.b, f.a);
    const Int denom = cross(r, s);
    if (denom == 0) return false;  // unreachable given the strict sign tests; cheap to be sure
    // The parameter is a ratio of exact integer determinants; only the DIVISION is floating point,
    // and its error is ~2^-25 lattice units against a rounding step of 1 -- the snap is unaffected.
    const double t = static_cast<double>(cross(sub(f.a, e.a), s)) / static_cast<double>(denom);
    out = lat.snap(static_cast<double>(e.a.x) + static_cast<double>(r.x) * t,
                   static_cast<double>(e.a.y) + static_cast<double>(r.y) * t);
    return true;
}

void pairSplits(const Lattice& lat, const Edge& e, const Edge& f, std::vector<IPt>& se,
                std::vector<IPt>& sf) {
    // (I2)+(I3): an endpoint of one landing inside the other becomes a vertex of that other. This
    // one rule covers every collinear overlap -- partial, contained or identical -- and drives it
    // to "coincide exactly, or share only an endpoint".
    if (strictlyInside(f.a, e)) se.push_back(f.a);
    if (strictlyInside(f.b, e)) se.push_back(f.b);
    if (strictlyInside(e.a, f)) sf.push_back(e.a);
    if (strictlyInside(e.b, f)) sf.push_back(e.b);
    // (I1): a genuine crossing splits both, at the SAME snapped point on each, so the two chains
    // meet at a shared vertex rather than merely near one.
    //
    // THE SNAP IS THE MECHANISM, not a rounding error to be validated away (Greene-Yao 1986 /
    // Hobby 1999). The crossing of two lattice segments is a rational point, so it is essentially
    // never a lattice point itself; rounding it displaces it off BOTH lines by up to half a cell,
    // and the two chains are then re-routed through that one vertex. What has to hold is that the
    // chains agree on the vertex -- never that the vertex stayed collinear with the edges it came
    // from. Asking for the latter (an onSegment test on `x`) rejects every crossing that is not
    // already lattice-exact, which leaves the two chains crossing with no vertex in common: the
    // arrangement stops being planar, the winding classification either side of a fragment stops
    // describing that fragment, and the surviving fragments no longer close into rings.
    IPt x{};
    if (properCross(lat, e, f, x)) {
        // The one thing rounding can break is the SPAN: a crossing within half a cell of an end can
        // round to or past it. Then that end is the honest shared vertex -- the chains already meet
        // there -- and the edge is left whole. `e` is settled last so its own split is never
        // outside its own span; the residue when both edges anchor to different ends of themselves
        // is a sub-cell spur on `f`, which is the documented limit (docs/vector-model.md §9.5).
        x = anchorToEnd(x, e);
        x = anchorToEnd(x, f);
        x = anchorToEnd(x, e);
        if (!(x == e.a) && !(x == e.b)) se.push_back(x);
        if (!(x == f.a) && !(x == f.b)) sf.push_back(x);
    }
}

// One refinement round. Returns true when something had to be split (so the caller runs again --
// the snap in properCross can move a point off the line it came from and create fresh work).
bool refine(const Lattice& lat, std::vector<Edge>& edges) {
    const std::size_t n = edges.size();
    if (n < 2) return false;
    std::vector<std::vector<IPt>> splits(n);

    // Bentley-Ottmann's ordering idea in its simplest form: sweep in x and stop the inner scan as
    // soon as an edge starts past the current edge's right end. No status structure, but it turns
    // the quadratic into "quadratic in what actually overlaps", which for real artwork is small.
    //
    // ⚠ The sweep walks a COMPACT ARRAY OF BOXES in x order, not an index permutation over
    // `edges`. Same pairs, same order, same splits -- but the inner scan is the hot loop of this
    // phase and the two forms cost very differently. Through a permutation, every candidate is a
    // random-access load of a 40-byte Edge somewhere else in the vector, and each one recomputes
    // the same four min/max bounds it recomputed the last time it was scanned. Normalizing a
    // stroke ribbon runs this 12 times over 62,000 edges and scans 2.7M candidates to find the
    // 218,000 that actually overlap -- 92% of the scan is a rejection, so the rejection is the
    // loop, and it wants to be a sequential read of precomputed bounds.
    struct Box {
        Int xLo, xHi, yLo, yHi;
        std::uint32_t idx;
    };
    std::vector<Box> boxes(n);
    for (std::size_t i = 0; i < n; ++i)
        boxes[i] = {std::min(edges[i].a.x, edges[i].b.x), std::max(edges[i].a.x, edges[i].b.x),
                    std::min(edges[i].a.y, edges[i].b.y), std::max(edges[i].a.y, edges[i].b.y),
                    static_cast<std::uint32_t>(i)};
    std::sort(boxes.begin(), boxes.end(), [](const Box& l, const Box& r) {
        return l.xLo != r.xLo ? l.xLo < r.xLo : l.idx < r.idx; // total order -> deterministic
    });
    for (std::size_t oi = 0; oi < n; ++oi) {
        const Box bi = boxes[oi];
        for (std::size_t oj = oi + 1; oj < n; ++oj) {
            const Box& bk = boxes[oj];
            if (bk.xLo > bi.xHi)
                break;
            if (bk.yLo > bi.yHi || bk.yHi < bi.yLo)
                continue;
            pairSplits(lat, edges[bi.idx], edges[bk.idx], splits[bi.idx], splits[bk.idx]);
        }
    }

    bool any = false;
    for (const auto& s : splits)
        if (!s.empty()) {
            any = true;
            break;
        }
    if (!any) return false;

    std::vector<Edge> next;
    next.reserve(n + n / 2 + 8);
    for (std::size_t i = 0; i < n; ++i) {
        const Edge& e = edges[i];
        std::vector<IPt>& s = splits[i];
        s.erase(std::remove_if(s.begin(), s.end(),
                               [&](IPt p) { return p == e.a || p == e.b; }),
                s.end());
        if (s.empty()) {
            next.push_back(e);
            continue;
        }
        // Order the new vertices ALONG a->b by their exact projection on the edge direction. A
        // rounded crossing is only NEAR the segment, so the dominant axis is not a total order
        // along it: for an edge that is nearly axis-aligned, half a cell of sideways slack is
        // enough to swap two points whose leading coordinate differs by less than that, and a
        // mis-ordered list emits fragments that overlap and double back. The dot product is exact
        // (magnitudes <= 2^57) and is the projection itself; lexLess only settles the tie between
        // two points that project to the same place, and settles it deterministically.
        const IPt d = sub(e.b, e.a);
        std::sort(s.begin(), s.end(), [&](IPt l, IPt r) {
            const Int pl = dot(d, sub(l, e.a));
            const Int pr = dot(d, sub(r, e.a));
            return pl != pr ? pl < pr : lexLess(l, r);
        });
        s.erase(std::unique(s.begin(), s.end()), s.end());
        IPt prev = e.a;
        for (const IPt p : s) {
            if (!(p == prev)) next.push_back(Edge{prev, p, e.operand});
            prev = p;
        }
        if (!(prev == e.b)) next.push_back(Edge{prev, e.b, e.operand});
    }
    edges.swap(next);
    return true;
}

// ---------------------------------------------------------------------------------------------
// §3 -- one representative per undirected fragment, carrying each operand's net winding delta
// ---------------------------------------------------------------------------------------------

struct Frag {
    IPt p, q;              // canonical: lexLess(p, q)
    std::vector<int> m;    // per operand: (#traversals p->q) - (#traversals q->p)
};

std::vector<Frag> buildFragments(const std::vector<Edge>& edges, std::size_t operandCount) {
    struct Keyed {
        IPt p, q;
        int operand;
        int dir;
    };
    std::vector<Keyed> keyed;
    keyed.reserve(edges.size());
    for (const Edge& e : edges) {
        if (e.a == e.b) continue;
        if (lexLess(e.a, e.b)) keyed.push_back({e.a, e.b, e.operand, 1});
        else keyed.push_back({e.b, e.a, e.operand, -1});
    }
    std::sort(keyed.begin(), keyed.end(), [](const Keyed& l, const Keyed& r) {
        if (!(l.p == r.p)) return lexLess(l.p, r.p);
        if (!(l.q == r.q)) return lexLess(l.q, r.q);
        return l.operand < r.operand;
    });

    std::vector<Frag> out;
    out.reserve(keyed.size());
    for (std::size_t i = 0; i < keyed.size();) {
        Frag f;
        f.p = keyed[i].p;
        f.q = keyed[i].q;
        f.m.assign(operandCount, 0);
        std::size_t j = i;
        while (j < keyed.size() && keyed[j].p == f.p && keyed[j].q == f.q) {
            const std::size_t k = static_cast<std::size_t>(keyed[j].operand);
            if (k < operandCount) f.m[k] += keyed[j].dir;
            ++j;
        }
        i = j;
        // A fragment every operand crosses a net zero times (a spur walked out and back, or two
        // opposite coincident edges of one operand) is not on any boundary. Drop it here so the
        // winding scan never sees it.
        const bool alive = std::any_of(f.m.begin(), f.m.end(), [](int v) { return v != 0; });
        if (alive) out.push_back(std::move(f));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// §4 -- classification: every operand's winding number infinitesimally off one side
// ---------------------------------------------------------------------------------------------

// `v <= M.y + eps*ny` for an infinitesimal eps > 0. Ties -- and ties are the whole reason this is
// symbolic -- are broken by the sign of the offset, which is exactly what "on this side" means.
bool leqEps(Int v, Int my, Int ny) {
    if (v != my) return v < my;
    return ny >= 0;
}

bool insideOp(BoolOp op, const std::vector<Int>& w, const std::vector<FillRule>& rules) {
    const std::size_t n = w.size();
    if (n == 0) return false;
    const auto in = [&](std::size_t k) {
        return rules[k] == FillRule::EvenOdd ? (w[k] % 2 != 0) : (w[k] != 0);
    };
    switch (op) {
        case BoolOp::Union:
            for (std::size_t k = 0; k < n; ++k)
                if (in(k)) return true;
            return false;
        case BoolOp::Intersect:
            for (std::size_t k = 0; k < n; ++k)
                if (!in(k)) return false;
            return true;
        case BoolOp::Subtract:
            if (!in(0)) return false;
            for (std::size_t k = 1; k < n; ++k)
                if (in(k)) return false;
            return true;
        case BoolOp::Exclude: {
            bool odd = false;
            for (std::size_t k = 0; k < n; ++k)
                if (in(k)) odd = !odd;
            return odd;
        }
    }
    return false;
}

// The §4 predicate for EVERY fragment, in one swept pass.
//
// The predicate itself is Sunday's winding number (vector/hit.cpp's rule) evaluated at M + eps*n
// for each operand, where M is a fragment midpoint -- so by invariants I1/I2 it lies on exactly
// one fragment and nothing else, which is what makes the symbolic evaluation well defined.
//
// ⚠ SWEPT IN Y, not all-pairs, and it answers both sides of a fragment in ONE scan. The direct
// direct form casts the two probes separately and each scans every other fragment: 2*F^2
// tests. A stroked shape's ribbon is where that bites -- the stroker emits one overlapping piece
// per segment and per join, so normalizing a 41-lobe rosette's outline arrives here with 4,762
// fragments, which is 45 million tests and measured 78 of the boolean's 95 ms, more than every
// other phase put together.
//
// Two facts collapse it, and neither changes an answer:
//
//   * A fragment contributes to a probe only when its y-interval STRADDLES the probe's y -- that
//     is exactly what the aLe != bLe test rejects on. So sweep the probes in increasing y and
//     keep an active list of the fragments whose interval is still open; the list IS the set the
//     old scan would have kept, and everything else it walked contributed nothing.
//   * The two probes share the midpoint and differ only in the normal, and the normals are
//     negatives of each other -- so one orient2d serves both, and only the symbolic tie-break
//     (the s == 0 case, and leqEps on an endpoint exactly at the probe's y) has to be asked twice.
//
// The winding numbers are integer sums over the same set of fragments, so they come out identical
// whatever order the sweep visits them in, and the classification with them.
enum : std::uint8_t { kSideNeither = 0, kSideRight = 1, kSideLeft = 2 };

std::vector<std::uint8_t> classifyFragments(BoolOp op, const std::vector<Frag>& frags,
                                            const std::vector<FillRule>& rules, std::size_t nOps) {
    const std::size_t F = frags.size();
    std::vector<std::uint8_t> out(F, kSideNeither);
    std::vector<Int> yLo(F), yHi(F), qy(F);
    for (std::size_t i = 0; i < F; ++i) {
        yLo[i] = std::min(frags[i].p.y, frags[i].q.y);
        yHi[i] = std::max(frags[i].p.y, frags[i].q.y);
        // The probe point is the fragment's midpoint; lattice coordinates are doubled, so the
        // halving is exact (the same midpoint the direct form probed at).
        qy[i] = (frags[i].p.y + frags[i].q.y) / 2;
    }
    std::vector<std::uint32_t> byLo(F), byQuery(F);
    for (std::size_t i = 0; i < F; ++i)
        byLo[i] = byQuery[i] = static_cast<std::uint32_t>(i);
    // Index ties break the sorts, so both orders are total and the sweep is reproducible.
    std::sort(byLo.begin(), byLo.end(), [&](std::uint32_t a, std::uint32_t b) {
        return yLo[a] != yLo[b] ? yLo[a] < yLo[b] : a < b;
    });
    std::sort(byQuery.begin(), byQuery.end(), [&](std::uint32_t a, std::uint32_t b) {
        return qy[a] != qy[b] ? qy[a] < qy[b] : a < b;
    });

    std::vector<std::uint32_t> active;
    active.reserve(F);
    std::vector<Int> wL(nOps, 0), wR(nOps, 0);
    std::size_t next = 0;
    for (const std::uint32_t qi : byQuery) {
        const Frag& f = frags[qi];
        const IPt m{(f.p.x + f.q.x) / 2, qy[qi]};
        const IPt d = sub(f.q, f.p);
        const IPt nL{d.y, -d.x};
        const IPt nR{-d.y, d.x};
        while (next < F && yLo[byLo[next]] <= m.y)
            active.push_back(byLo[next++]);
        std::fill(wL.begin(), wL.end(), Int{0});
        std::fill(wR.begin(), wR.end(), Int{0});
        // Retiring rides along on the scan: probes only ever move DOWN in y, so a fragment that
        // ends above this one can never straddle a later probe either.
        std::size_t live = 0;
        for (std::size_t r = 0; r < active.size(); ++r) {
            const std::uint32_t fi = active[r];
            if (yHi[fi] < m.y)
                continue; // closed behind the sweep: drop it for good
            active[live++] = fi;
            const Frag& g = frags[fi];
            const bool pLeL = leqEps(g.p.y, m.y, nL.y);
            const bool qLeL = leqEps(g.q.y, m.y, nL.y);
            const bool pLeR = leqEps(g.p.y, m.y, nR.y);
            const bool qLeR = leqEps(g.q.y, m.y, nR.y);
            if (pLeL == qLeL && pLeR == qLeR)
                continue; // the ray misses on both sides
            // orient2d(p, q, M + eps*n) == orient2d(p, q, M) + eps * cross(q - p, n); the second
            // term only ever decides when the first is zero, and the two normals are negatives,
            // so the tie-break is one cross product and its negation.
            const Int s0 = orient2d(g.p, g.q, m);
            Int sL = s0, sR = s0;
            if (s0 == 0) {
                sL = cross(sub(g.q, g.p), nL);
                sR = -sL;
            }
            if (pLeL != qLeL) {
                if (pLeL) {
                    if (sL > 0)
                        for (std::size_t i = 0; i < nOps; ++i)
                            wL[i] += g.m[i];
                } else if (sL < 0) {
                    for (std::size_t i = 0; i < nOps; ++i)
                        wL[i] -= g.m[i];
                }
            }
            if (pLeR != qLeR) {
                if (pLeR) {
                    if (sR > 0)
                        for (std::size_t i = 0; i < nOps; ++i)
                            wR[i] += g.m[i];
                } else if (sR < 0) {
                    for (std::size_t i = 0; i < nOps; ++i)
                        wR[i] -= g.m[i];
                }
            }
        }
        active.resize(live);
        const bool inL = insideOp(op, wL, rules);
        const bool inR = insideOp(op, wR, rules);
        if (inL == inR)
            continue; // both sides agree -> not a boundary of the result
        out[qi] = inR ? kSideRight : kSideLeft;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// §5 -- link the surviving boundary into rings
// ---------------------------------------------------------------------------------------------

struct DEdge {
    IPt a, b;
};

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

// Which sector of the full turn `d` falls in, measured CLOCKWISE (this is y-down screen space, so
// clockwise on screen is the +cross direction) from `ref`:
//   0 == exactly along ref (a reversal, angle 0)   1 == (0, pi)   2 == exactly pi   3 == (pi, 2pi)
int turnSector(IPt ref, IPt d) {
    const Int c = cross(ref, d);
    if (c > 0) return 1;
    if (c < 0) return 3;
    return dot(ref, d) > 0 ? 0 : 2;
}

// Is `x` further clockwise from `ref` than `y` is? Exact -- no angles are ever computed.
bool moreClockwise(IPt ref, IPt x, IPt y) {
    const int sx = turnSector(ref, x);
    const int sy = turnSector(ref, y);
    if (sx != sy) return sx > sy;
    if (sx == 0 || sx == 2) return false;  // identical directions; no preference
    return cross(y, x) > 0;                // within one half-turn, cross IS the comparator
}

// The result's interior lies on the (-dy, dx) side of every emitted fragment, i.e. on the walker's
// right. So at a pinch vertex the boundary is hugged by taking the turn FURTHEST clockwise from
// the way we came -- which resolves two lobes meeting at a point into two separate simple rings
// instead of one self-touching one. Under NonZero the filled region is identical either way; the
// decomposition (and hence the outer/hole normalization below) is not.
std::size_t pickNext(const std::vector<DEdge>& edges, const std::vector<std::size_t>& outgoing,
                     const std::vector<bool>& used, std::size_t cur) {
    const IPt ref = sub(edges[cur].a, edges[cur].b);  // back along the edge we arrived on
    std::size_t best = kNone;
    for (const std::size_t e : outgoing) {
        if (used[e]) continue;
        if (best == kNone) {
            best = e;
            continue;
        }
        if (moreClockwise(ref, sub(edges[e].b, edges[e].a), sub(edges[best].b, edges[best].a)))
            best = e;
    }
    return best;
}

// Exact orientation of a simple ring: its lexicographically smallest vertex is necessarily convex,
// so the turn there is the ring's sign. Positive == visually clockwise in y-down == the OUTER
// convention this file and core/text/extrude_mesh.cpp share.
bool ringIsPositive(const std::vector<IPt>& pts) {
    const std::size_t n = pts.size();
    std::size_t lo = 0;
    for (std::size_t i = 1; i < n; ++i)
        if (lexLess(pts[i], pts[lo])) lo = i;
    const IPt prev = pts[(lo + n - 1) % n];
    const IPt next = pts[(lo + 1) % n];
    const Int o = orient2d(prev, pts[lo], next);
    if (o != 0) return o > 0;
    // Degenerate (the extreme vertex's two edges are collinear): fall back to the shoelace sign,
    // accumulated in double relative to the first point so the magnitudes stay small.
    double area = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const IPt u = sub(pts[i], pts[0]);
        const IPt v = sub(pts[(i + 1) % n], pts[0]);
        area += static_cast<double>(u.x) * static_cast<double>(v.y) -
                static_cast<double>(v.x) * static_cast<double>(u.y);
    }
    return area >= 0.0;
}

// Merge runs of collinear vertices. A shared edge between two operands arrives here as two
// fragments meeting at a point that is no longer a corner of anything; so does every vertex the
// refinement pass had to introduce. Leaving them in would hand each downstream consumer -- stroke
// joins, node editing after a bake, SVG export -- corners that do not exist in the shape. The
// test is exact (orient2d == 0 plus "strictly between"), never an angle threshold, so a genuine
// hairpin is never mistaken for a straight run.
void dropCollinear(std::vector<IPt>& r) {
    bool changed = true;
    while (changed && r.size() > 3) {
        changed = false;
        for (std::size_t i = 0; i < r.size() && r.size() > 3;) {
            const std::size_t n = r.size();
            const IPt prev = r[(i + n - 1) % n];
            const IPt cur = r[i];
            const IPt next = r[(i + 1) % n];
            if (orient2d(prev, cur, next) == 0 && dot(sub(cur, prev), sub(next, cur)) > 0) {
                r.erase(r.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                continue;  // do not advance: the old `next` has slid into slot i
            }
            ++i;
        }
    }
}

// Even-odd containment, exact in the lattice. The probe is always a fragment MIDPOINT, which by
// I1/I2 is never on another ring's boundary -- so this has no tie cases to break.
bool pointInRing(IPt p, const std::vector<IPt>& ring) {
    bool in = false;
    const std::size_t n = ring.size();
    for (std::size_t i = 0; i < n; ++i) {
        const IPt a = ring[i];
        const IPt b = ring[(i + 1) % n];
        if ((a.y > p.y) != (b.y > p.y)) {
            const Int c = cross(sub(b, a), sub(p, a));
            if ((b.y > a.y) ? (c > 0) : (c < 0)) in = !in;
        }
    }
    return in;
}

// ---------------------------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------------------------

Contours resolve(BoolOp op, const std::vector<const Contours*>& operands,
                 const std::vector<FillRule>& rules) {
    const std::size_t nOps = operands.size();
    if (nOps == 0) return {};
    std::vector<FillRule> ruleset = rules;
    ruleset.resize(nOps, FillRule::NonZero);  // a short/absent rule list means NonZero

    // §1 -- pick the lattice from the input's magnitude, then snap.
    double maxAbs = 0.0;
    for (const Contours* cs : operands) {
        if (cs == nullptr) continue;
        for (const Contour& c : *cs)
            for (const Vec2& p : c.points) {
                if (!std::isfinite(p.x) || !std::isfinite(p.y)) continue;
                maxAbs = std::max(maxAbs, std::max(std::abs(p.x), std::abs(p.y)));
            }
    }
    const Lattice lat{chooseScale(maxAbs)};

    std::vector<Edge> edges;
    for (std::size_t k = 0; k < nOps; ++k) {
        const Contours* cs = operands[k];
        if (cs == nullptr) continue;
        for (const Contour& c : *cs) {
            // Every contour closes for the area test, open ones included -- the rule the scanline
            // rasterizer and hit.cpp's contains() already follow (and SVG's). An open operand is
            // never silently dropped.
            std::vector<IPt> ring;
            ring.reserve(c.points.size());
            bool bad = false;
            for (const Vec2& p : c.points) {
                if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                    bad = true;
                    break;
                }
                const IPt q = lat.toLattice(p);
                if (ring.empty() || !(ring.back() == q)) ring.push_back(q);
            }
            if (bad) continue;  // a non-finite point poisons the whole contour, not the operand
            if (ring.size() >= 2 && ring.front() == ring.back()) ring.pop_back();
            if (ring.size() < 3) continue;  // no area to contribute
            for (std::size_t i = 0; i < ring.size(); ++i)
                edges.push_back(Edge{ring[i], ring[(i + 1) % ring.size()],
                                     static_cast<int>(k)});
        }
    }
    if (edges.empty()) return {};

    // §2 -- refine to the three invariants.
    for (int round = 0; round < kMaxRefineRounds; ++round) {
        if (edges.size() > kMaxEdges) break;
        if (!refine(lat, edges)) break;
    }

    // §3 -- one representative per undirected fragment.
    const std::vector<Frag> frags = buildFragments(edges, nOps);
    if (frags.empty()) return {};

    // §4 -- keep the fragments the op's predicate straddles, oriented interior-on-the-right.
    std::vector<DEdge> kept;
    kept.reserve(frags.size());
    // Emitted in FRAGMENT order, whatever order the sweep classified them in: the ring assembly
    // below and the reproducibility it promises are stated over this sequence.
    const std::vector<std::uint8_t> side = classifyFragments(op, frags, ruleset, nOps);
    for (std::size_t i = 0; i < frags.size(); ++i) {
        if (side[i] == kSideNeither)
            continue;
        const Frag& f = frags[i];
        // Emit so the interior is on the (-dy, dx) side of the travel direction: that is the
        // winding whose ring comes out POSITIVE (visually clockwise, y-down) for an outer.
        if (side[i] == kSideRight)
            kept.push_back(DEdge{f.p, f.q});
        else kept.push_back(DEdge{f.q, f.p});
    }
    if (kept.empty()) return {};

    // §5 -- link into rings.
    std::vector<IPt> verts;
    verts.reserve(kept.size() * 2);
    for (const DEdge& e : kept) {
        verts.push_back(e.a);
        verts.push_back(e.b);
    }
    std::sort(verts.begin(), verts.end(), lexLess);
    verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
    const auto idOf = [&](IPt p) {
        return static_cast<std::size_t>(
            std::lower_bound(verts.begin(), verts.end(), p, lexLess) - verts.begin());
    };
    std::vector<std::vector<std::size_t>> outgoing(verts.size());
    for (std::size_t i = 0; i < kept.size(); ++i) outgoing[idOf(kept[i].a)].push_back(i);

    std::vector<bool> used(kept.size(), false);
    std::vector<std::vector<IPt>> rings;
    for (std::size_t start = 0; start < kept.size(); ++start) {
        if (used[start]) continue;
        std::vector<IPt> ring;
        std::size_t cur = start;
        for (std::size_t guard = 0; guard <= kept.size(); ++guard) {
            used[cur] = true;
            ring.push_back(kept[cur].a);
            const std::size_t nxt = pickNext(kept, outgoing[idOf(kept[cur].b)], used, cur);
            if (nxt == kNone) break;  // every exit taken -> the ring has closed
            cur = nxt;
        }
        if (ring.size() >= 3) rings.push_back(std::move(ring));
    }
    if (rings.empty()) return {};

    // Normalize: containment depth -> hole flag -> forced orientation. The probe is ring i's first
    // EDGE MIDPOINT rather than a vertex, because a vertex may be shared with another ring at a
    // pinch while a midpoint provably is not.
    std::vector<IPt> probe(rings.size());
    for (std::size_t i = 0; i < rings.size(); ++i)
        probe[i] = IPt{(rings[i][0].x + rings[i][1].x) / 2, (rings[i][0].y + rings[i][1].y) / 2};
    std::vector<int> depth(rings.size(), 0);
    for (std::size_t i = 0; i < rings.size(); ++i)
        for (std::size_t k = 0; k < rings.size(); ++k)
            if (k != i && pointInRing(probe[i], rings[k])) ++depth[i];

    Contours out;
    out.reserve(rings.size());
    for (std::size_t i = 0; i < rings.size(); ++i) {
        std::vector<IPt>& r = rings[i];
        const bool hole = (depth[i] % 2) == 1;
        // Outers positive, holes negative -- the convention core/text/extrude_mesh.cpp already
        // uses, and the one that makes the result read correctly under NonZero with no negotiation.
        if (ringIsPositive(r) == hole) std::reverse(r.begin(), r.end());
        // Only NOW, after the probes and the depths are settled on the linked rings, is it safe to
        // straighten the collinear runs -- a merged edge's midpoint is no longer a fragment
        // midpoint, and the containment probe above depends on it being one.
        dropCollinear(r);
        if (r.size() < 3) continue;
        // Canonical start vertex, so two runs on the same input are byte-identical.
        const std::size_t n = r.size();
        std::size_t lo = 0;
        for (std::size_t j = 1; j < n; ++j)
            if (lexLess(r[j], r[lo])) lo = j;
        Contour c;
        c.closed = true;
        c.points.reserve(n);
        for (std::size_t j = 0; j < n; ++j) c.points.push_back(lat.toLocal(r[(lo + j) % n]));
        out.push_back(std::move(c));
    }
    // Deterministic ring order. Each ring already starts at its own smallest vertex, but two rings
    // may share that vertex (a pinch), so the comparator runs the whole sequence -- a partial one
    // would leave std::sort free to pick either order and the output would stop being reproducible.
    std::sort(out.begin(), out.end(), [](const Contour& l, const Contour& r) {
        const std::size_t n = std::min(l.points.size(), r.points.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (l.points[i].x != r.points[i].x) return l.points[i].x < r.points[i].x;
            if (l.points[i].y != r.points[i].y) return l.points[i].y < r.points[i].y;
        }
        return l.points.size() < r.points.size();
    });
    return out;
}

std::vector<FillRule> padRules(std::vector<FillRule> rules, std::size_t n) {
    rules.resize(n, FillRule::NonZero);
    return rules;
}

// The flattening tolerance Layer > Combine Paths bakes at, in LAYER-LOCAL units.
//
// A live compound re-flattens every frame, so the fixed 0.25 default costs it nothing: the frame
// passes its own tolerance and device transform and the smoothness tracks zoom. A BAKE is
// permanent, so the same 0.25 would be wrong twice over -- far too coarse for a shape whose layer
// is scaled up (0.25 LOCAL units can be tens of document pixels), and gratuitously fine for one
// that is enormous in local units, where it buys thousands of nodes nobody asked for.
//
// So the tolerance is the FINER of two readings of the same requirement:
//   * DEVICE   -- 0.25 document pixels read back through the host's world scale, i.e. exactly the
//                 smoothness the live compound was drawing with at 100% zoom;
//   * RELATIVE -- 1/4000 of the operands' extent, which is what keeps a facet imperceptible once
//                 the shape is bigger on screen than an absolute pixel figure can speak about.
// The two agree at ~1000 document pixels of extent and each dominates on its own side of that.
//
// Then capped at 0.25 so a bake is never COARSER than the old fixed default, and finally floored
// at 1/100000 of the extent -- a node BUDGET (roughly 500 nodes for a full circle, whatever its
// size), which is deliberately allowed to beat the cap for enormous artwork: the kernel is O(E^2)
// and an "editable" path with ten thousand nodes in it is not an improvement on a compound.
double bakeToleranceFor(const BooleanCompound& compound, const Affine2D& hostWorld) {
    double extent = 0.0;
    for (const Object& child : compound.children)
        if (const auto box = contentBounds(child.geometry))
            extent = std::max(extent, std::max(box->w, box->h));
    // Max column norm: the bound on how far the transform can stretch one local unit. Floored so a
    // near-degenerate (but invertible) host transform cannot divide the tolerance to nothing.
    const double sx = std::hypot(hostWorld.m00, hostWorld.m10);
    const double sy = std::hypot(hostWorld.m01, hostWorld.m11);
    const double worldScale = std::max({sx, sy, 1e-6});
    double tol = std::min(0.25 / worldScale, extent * (1.0 / 4000.0));
    tol = std::min(tol, 0.25);
    return std::max({tol, extent * 1e-5, 1e-4});
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------

bool BooleanCompound::operator==(const BooleanCompound& other) const {
    return op == other.op && children == other.children;
}

Contours booleanContours(BoolOp op, const Contours& a, const Contours& b) {
    const std::vector<const Contours*> ops{&a, &b};
    return resolve(op, ops, std::vector<FillRule>(2, FillRule::NonZero));
}

Contours booleanContours(BoolOp op, const std::vector<Contours>& operands) {
    return booleanContours(op, operands, std::vector<FillRule>{});
}

Contours booleanContours(BoolOp op, const std::vector<Contours>& operands,
                         const std::vector<FillRule>& rules) {
    std::vector<const Contours*> ptrs;
    ptrs.reserve(operands.size());
    for (const Contours& cs : operands) ptrs.push_back(&cs);
    return resolve(op, ptrs, padRules(rules, operands.size()));
}

Contours normalizedContours(const Contours& cs, FillRule rule) {
    const std::vector<const Contours*> ops{&cs};
    return resolve(BoolOp::Union, ops, std::vector<FillRule>{rule});
}

Contours flattenCompound(const BooleanCompound& compound, double tolerancePx,
                         const Affine2D& toDevice) {
    std::vector<Contours> parts;
    std::vector<FillRule> rules;
    parts.reserve(compound.children.size());
    rules.reserve(compound.children.size());
    for (const Object& child : compound.children) {
        // Same tolerance, same device transform: a boolean must not flatten its operands more
        // coarsely than the caller asked, or the result's smoothness would stop tracking zoom.
        parts.push_back(flatten(child.geometry, tolerancePx, toDevice));
        rules.push_back(fillRuleOf(child.geometry));
    }
    return booleanContours(compound.op, parts, rules);
}

Path bakedBooleanPath(const BooleanCompound& compound, double tolerancePx) {
    const Contours cs = flattenCompound(compound, tolerancePx, Affine2D::identity());
    Path p;
    p.fillRule = FillRule::NonZero;  // the kernel normalizes to NonZero; nothing else is correct
    for (const Contour& c : cs) {
        if (c.points.size() < 3) continue;
        SubPath sp;
        sp.closed = true;  // the kernel emits closed rings only, whatever went in
        sp.nodes.reserve(c.points.size());
        for (const Vec2& v : c.points) {
            // Node hygiene, and it is the EDITOR's requirement rather than the kernel's: two
            // anchors at one point read as two draggable nodes stacked on each other, and a
            // zero-length segment has no direction for a handle to be smooth about. The kernel's
            // rings are already distinct lattice vertices and toLocal is injective on them, so
            // this never fires in practice -- but the guarantee belongs to the function that hands
            // out an editable path, not to the one that happened to fill it.
            if (!sp.nodes.empty() && sp.nodes.back().anchor == v) continue;
            // Handles == anchor: a polyline, exactly. Corner, because a polyline vertex IS one --
            // an editor dragging a handle there must break nothing, and Smooth would be a lie.
            sp.nodes.push_back(Node{v, v, v, Node::Type::Corner});
        }
        // The ring WRAPS, so the closing segment needs the same rule the interior ones got.
        while (sp.nodes.size() > 1 && sp.nodes.back().anchor == sp.nodes.front().anchor)
            sp.nodes.pop_back();
        if (sp.nodes.size() < 3) continue;  // nothing with area survived the dedup
        p.subpaths.push_back(std::move(sp));
    }
    return p;
}

Object rebasedObject(const Object& obj, const Affine2D& t) {
    Object out = obj;  // fill / stroke / paint order ride along untouched
    if (t == Affine2D::identity()) return out;
    if (const auto* compound = std::get_if<BooleanCompound>(&obj.geometry)) {
        BooleanCompound rebased;
        rebased.op = compound->op;
        rebased.children.reserve(compound->children.size());
        for (const Object& child : compound->children)
            rebased.children.push_back(rebasedObject(child, t));
        out.geometry = std::move(rebased);
        return out;
    }
    // A ParametricShape cannot carry an arbitrary transform (its parameters are its size, centred
    // on the local origin), so rebasing promotes it -- exactly, cubics being affine-invariant.
    out.geometry = transformedPath(pathFromGeometry(obj.geometry), t);
    return out;
}

std::optional<Object> makeLiveBooleanObject(
    BoolOp op, const std::vector<std::pair<Object, Affine2D>>& operandsInWorld,
    const Affine2D& hostWorld) {
    if (operandsInWorld.size() < 2) return std::nullopt;
    const std::optional<Affine2D> hostInv = hostWorld.inverse();
    if (!hostInv) return std::nullopt;
    BooleanCompound compound;
    compound.op = op;
    compound.children.reserve(operandsInWorld.size());
    for (const auto& [obj, world] : operandsInWorld)
        compound.children.push_back(rebasedObject(obj, *hostInv * world));
    Object out = operandsInWorld.front().first;  // the host's appearance wins
    out.geometry = std::move(compound);
    return out;
}

std::optional<Object> makeBooleanObject(
    BoolOp op, const std::vector<std::pair<Object, Affine2D>>& operandsInWorld,
    const Affine2D& hostWorld) {
    std::optional<Object> live = makeLiveBooleanObject(op, operandsInWorld, hostWorld);
    if (!live) return std::nullopt;
    // The menu COMMITS a path. A compound would be a layer no tool owns: penToolBinds() wants a
    // vec::Path and the Shape tool wants a ParametricShape, so a third alternative is selectable by
    // neither and wears the wrong badge. Baking is also what the object honestly is once the
    // command is on the undo stack -- a region computed from operands that no longer exist as
    // layers -- and it makes the result node-editable, ordinary to serialize and NonZero by
    // construction (fillRuleOf reads Path::fillRule, which bakedBooleanPath sets to what the
    // normalization actually produced).
    const BooleanCompound& compound = std::get<BooleanCompound>(live->geometry);
    Path baked = bakedBooleanPath(compound, bakeToleranceFor(compound, hostWorld));
    // An empty region is a real outcome (Subtract by something that covers the host, Exclude of two
    // identical shapes), but COMMITTING it would delete every consumed operand layer and leave the
    // host holding an invisible, unpickable path -- an undoable wipe that reads as a failure. Refuse
    // it; the caller already has the "these shapes cannot be combined" status for this.
    if (baked.subpaths.empty()) return std::nullopt;
    Object out = std::move(*live);  // `compound` is spent from here on; nothing below reads it
    out.geometry = std::move(baked);
    return out;
}

}  // namespace mosaic::core::vec
