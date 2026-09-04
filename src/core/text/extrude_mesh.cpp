#include "core/text/extrude_mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <earcut.hpp>
#include <limits>
#include <utility>

// Mesh generation for extruded text (docs/type-tool.md §10.2). Technique lineage: classic outline
// extrusion with beveled edges as shipped by WordArt (1991) / Adobe Dimensions / Xara3D --
// decades-old public technique; cap triangulation is textbook ear clipping (vendored mapbox
// earcut, ISC).
//
// Conventions (see geometry3d.hpp / extrude.hpp): model space is the layer's 2D frame -- x right,
// y DOWN -- with +z toward the viewer; the front cap sits at z = +depth/2. Ring orientation is
// FORCED before meshing (outers to positive shoelace area in y-down space -- visually clockwise --
// holes negative), so one outward-normal formula, (dy, -dx) per edge, serves every ring.
namespace mosaic::core::text {
namespace {

using common::Rect;

constexpr double kDegenerateArea = 1e-9;  // rings flatter than this are dropped
constexpr double kSmoothCos = 0.75;       // adjacent-edge cos above this smooths the shared vertex
constexpr double kMiterLimit = 2.0;       // bisector inset scale clamp (sharp corners don't spike)

// One prepared ring: closed, oriented, deduplicated polygon in design space.
struct Ring {
    std::vector<Vec2> pts;
    double area = 0.0;  // signed shoelace area (y-down: >0 = visually clockwise)
    bool hole = false;
    // Per-EDGE-ENDPOINT outward normals: edge i runs pts[i] -> pts[i+1]; its endpoints may share
    // the neighbouring edge's normal (smooth curve) or keep their own (sharp corner). Two entries
    // per edge: [2i] = the normal at pts[i], [2i+1] = at pts[i+1].
    std::vector<Vec2> edgeNormals;
    // Per-VERTEX miter-limited bisector direction (outward) with its scale folded in -- the cap
    // inset moves a vertex by -miter[i] * insetDistance.
    std::vector<Vec2> miter;
};

double shoelace(const std::vector<Vec2>& p) {
    double a = 0.0;
    for (std::size_t i = 0, n = p.size(); i < n; ++i) {
        const Vec2& p0 = p[i];
        const Vec2& p1 = p[(i + 1) % n];
        a += p0.x * p1.y - p1.x * p0.y;
    }
    return a * 0.5;
}

// Even-odd point-in-polygon (ray toward +x).
bool pointInRing(Vec2 pt, const std::vector<Vec2>& p) {
    bool in = false;
    for (std::size_t i = 0, n = p.size(); i < n; ++i) {
        const Vec2& a = p[i];
        const Vec2& b = p[(i + 1) % n];
        if ((a.y > pt.y) != (b.y > pt.y)) {
            const double x = a.x + (pt.y - a.y) / (b.y - a.y) * (b.x - a.x);
            if (x > pt.x) in = !in;
        }
    }
    return in;
}

Vec2 edgeOutwardNormal(Vec2 a, Vec2 b) {
    const Vec2 d{b.x - a.x, b.y - a.y};
    const double l = std::sqrt(d.x * d.x + d.y * d.y);
    if (l <= 0.0) return {0.0, 0.0};
    // For the forced orientation (outer = visually clockwise in y-down), the solid's interior
    // lies on the (-dy, dx) side of an edge, so outward is (dy, -dx).
    return {d.y / l, -d.x / l};
}

// Prepare a raw contour: dedupe consecutive duplicates (incl. an explicit closing point), check
// degeneracy, compute area. Orientation/normals are filled in later (after hole classification).
bool prepareRing(const std::vector<Vec2>& raw, Ring& out) {
    out.pts.clear();
    for (const Vec2& p : raw) {
        if (out.pts.empty() || (out.pts.back() - p).length() > 1e-12) out.pts.push_back(p);
    }
    if (out.pts.size() >= 2 && (out.pts.front() - out.pts.back()).length() <= 1e-12)
        out.pts.pop_back();  // drop the explicit closing point
    if (out.pts.size() < 3) return false;
    out.area = shoelace(out.pts);
    return std::abs(out.area) > kDegenerateArea;
}

// Fill edgeNormals + miter for a ring whose orientation is already forced: per vertex, decide
// smooth (flattened-curve shallow turn -> the bisector) vs sharp (a real corner -> each edge
// keeps its own normal there), then hand each edge its two endpoint normals.
void buildRingNormals(Ring& r) {
    const std::size_t n = r.pts.size();
    std::vector<Vec2> en(n);  // plain per-edge outward normals
    for (std::size_t i = 0; i < n; ++i) en[i] = edgeOutwardNormal(r.pts[i], r.pts[(i + 1) % n]);

    std::vector<Vec2> bisector(n);
    std::vector<bool> smooth(n);
    r.miter.assign(n, Vec2{});
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2 prev = en[(i + n - 1) % n];  // edge arriving at vertex i
        const Vec2 next = en[i];                // edge leaving vertex i
        Vec2 bis{prev.x + next.x, prev.y + next.y};
        const double bl = bis.length();
        bis = bl > 1e-12 ? Vec2{bis.x / bl, bis.y / bl} : next;
        bisector[i] = bis;
        smooth[i] = prev.dot(next) >= kSmoothCos;
        // Miter: the bisector scaled by 1/cos(half angle) keeps the inset distance along both
        // edges; the clamp stops near-reversal corners from spiking across thin strokes.
        const double halfCos = std::max(bis.dot(next), 1.0 / kMiterLimit);
        r.miter[i] = bis * (1.0 / halfCos);
    }
    r.edgeNormals.assign(2 * n, Vec2{});
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        r.edgeNormals[2 * i + 0] = smooth[i] ? bisector[i] : en[i];
        r.edgeNormals[2 * i + 1] = smooth[j] ? bisector[j] : en[i];
    }
}

// The ring's points moved INWARD by `inset` along the miter-limited bisectors (the cap outline
// when a bevel eats the edge). `safe` (when given) is the per-vertex travel bound from
// safeInsets() below: the inset saturates where the stroke runs out of room, so an oversized
// bevel steepens into a near-wall instead of folding the outline (the ">4px glitch" fix).
std::vector<Vec2> insetRing(const Ring& r, double inset, const std::vector<double>* safe) {
    std::vector<Vec2> out(r.pts.size());
    for (std::size_t i = 0; i < r.pts.size(); ++i) {
        double s = inset;
        if (safe != nullptr) s = std::clamp(s, -(*safe)[i], (*safe)[i]);
        out[i] = r.pts[i] - r.miter[i] * s;
    }
    return out;
}

// Per-vertex safe inset travel for one glyph's rings (2D only -- independent of depth/profile).
// Two ways a large inset folds the cap outline into self-intersection (earcut then emits garbage
// spanning the glyph -- the user-visible "letters glitch out / blow up"):
//   (a) an EDGE COLLAPSES: its endpoints' miters cross, flipping the edge;
//   (b) a stroke's two FACING WALLS cross: both cap sides travel inward and pass each other.
// Each vertex gets 90% of its tightest constraint (the sliver keeps the cap non-degenerate, the
// same margin the depth clamp leaves for the wall), capped at `request` -- geometry with room to
// spare is untouched. The Convex profile's outward bulge is bounded by the same figure (a bulge
// proportionate to the local stroke, never a glyph-swallowing balloon). O(n^2) clearance scan
// with a bbox reject at the current bound, so a small bevel on a big glyph stays near-linear.
std::vector<std::vector<double>> safeInsets(const std::vector<Ring>& rings, double request) {
    std::vector<std::vector<double>> safe(rings.size());
    const double unconstrained = request / 0.9 + 1.0;  // "no constraint found" sentinel
    for (std::size_t ri = 0; ri < rings.size(); ++ri)
        safe[ri].assign(rings[ri].pts.size(), unconstrained);

    // (a) Edge collapse: inset s moves edge (i, j) to e' = e - s * (miter_j - miter_i); its length
    // along its own direction hits zero at s = |e| / ((miter_j - miter_i) . e_hat).
    for (std::size_t ri = 0; ri < rings.size(); ++ri) {
        const Ring& r = rings[ri];
        const std::size_t n = r.pts.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const Vec2 e = r.pts[j] - r.pts[i];
            const double len = e.length();
            if (len <= 1e-12) continue;
            const Vec2 eh = e * (1.0 / len);
            const double shrink = (r.miter[j] - r.miter[i]).dot(eh);
            if (shrink > 1e-9) {
                const double s = len / shrink;
                safe[ri][i] = std::min(safe[ri][i], s);
                safe[ri][j] = std::min(safe[ri][j], s);
            }
        }
    }

    // (b) Facing-wall clearance: a vertex travelling inward meets the stroke's other wall (also
    // travelling) roughly halfway across the local width. Only an edge whose outward normal
    // OPPOSES the vertex's own and that lies on its INWARD side constrains it -- that pair of
    // tests keeps gap-facing walls free (an 'i' dot over its stem moves APART on inset; without
    // the inward-side test the dot gap would wrongly cap the bevel).
    //
    // ⚠ GRIDDED, because this is a BOUNDED-RADIUS query wearing an all-pairs loop's clothes. Every
    // vertex was tested against every edge of every ring in the solid -- and for a stroked shape
    // the solid is the normalized stroke ribbon, which has an order of magnitude more outline
    // points than the path it came from. Measured on a 41-lobe stroked rosette: 1,745 ribbon
    // points against their own 1,745 edges is 3.0M vertex/edge pairs, and it was 40 of the mesh
    // build's 42 ms.
    //
    // The radius is the point. `sv` starts at `unconstrained` and only ever falls, and an edge can
    // only tighten it through min(sv, dist * 0.5) -- so an edge farther than 2 * unconstrained
    // from a vertex cannot change that vertex's answer no matter what order it is visited in. That
    // is exactly what the `reach` test below already asserted per pair; hoisting its widest form
    // into a uniform grid answers it for whole neighbourhoods at once instead.
    //
    // The grid only decides WHICH pairs to look at. Every pair it yields runs the identical body,
    // the live `reach` test included, and the result is a min over a set the pruning provably
    // never removes a contributor from -- so the insets come out unchanged.
    const double reachMax = 2.0 * unconstrained;
    {
        struct GEdge {
            Vec2 a, c;
            std::uint32_t ring = 0, k = 0;
        };
        std::vector<GEdge> ge;
        double gx0 = std::numeric_limits<double>::infinity(), gy0 = gx0;
        double gx1 = -gx0, gy1 = -gx0;
        for (std::size_t ri = 0; ri < rings.size(); ++ri) {
            const std::size_t n = rings[ri].pts.size();
            for (std::size_t k = 0; k < n; ++k) {
                const Vec2 a = rings[ri].pts[k];
                const Vec2 c = rings[ri].pts[(k + 1) % n];
                ge.push_back({a, c, static_cast<std::uint32_t>(ri), static_cast<std::uint32_t>(k)});
                gx0 = std::min({gx0, a.x, c.x});
                gy0 = std::min({gy0, a.y, c.y});
                gx1 = std::max({gx1, a.x, c.x});
                gy1 = std::max({gy1, a.y, c.y});
            }
        }
        if (!ge.empty()) {
            // A cell at least as wide as the reach, so a query is a 3x3 neighbourhood; widened if
            // that would make the index bigger than the edge list it accelerates.
            constexpr int kMaxAxis = 512;
            const double cell =
                std::max({reachMax, (gx1 - gx0) / kMaxAxis, (gy1 - gy0) / kMaxAxis, 1e-9});
            const int nx = std::clamp(static_cast<int>((gx1 - gx0) / cell) + 1, 1, kMaxAxis);
            const int ny = std::clamp(static_cast<int>((gy1 - gy0) / cell) + 1, 1, kMaxAxis);
            const auto cx = [&](double x) {
                return std::clamp(static_cast<int>((x - gx0) / cell), 0, nx - 1);
            };
            const auto cy = [&](double y) {
                return std::clamp(static_cast<int>((y - gy0) / cell), 0, ny - 1);
            };
            // CSR: count the cells each edge's box touches, prefix-sum, then scatter.
            std::vector<std::uint32_t> starts(static_cast<std::size_t>(nx) * ny + 1, 0);
            for (const GEdge& e : ge)
                for (int iy = cy(std::min(e.a.y, e.c.y)); iy <= cy(std::max(e.a.y, e.c.y)); ++iy)
                    for (int ix = cx(std::min(e.a.x, e.c.x)); ix <= cx(std::max(e.a.x, e.c.x));
                         ++ix)
                        ++starts[static_cast<std::size_t>(iy) * nx + ix + 1];
            for (std::size_t i = 1; i < starts.size(); ++i)
                starts[i] += starts[i - 1];
            std::vector<std::uint32_t> items(starts.back());
            std::vector<std::uint32_t> fill(starts.begin(), starts.end() - 1);
            for (std::uint32_t i = 0; i < ge.size(); ++i) {
                const GEdge& e = ge[i];
                for (int iy = cy(std::min(e.a.y, e.c.y)); iy <= cy(std::max(e.a.y, e.c.y)); ++iy)
                    for (int ix = cx(std::min(e.a.x, e.c.x)); ix <= cx(std::max(e.a.x, e.c.x));
                         ++ix)
                        items[fill[static_cast<std::size_t>(iy) * nx + ix]++] = i;
            }

            for (std::size_t ri = 0; ri < rings.size(); ++ri) {
                const Ring& rv = rings[ri];
                for (std::size_t i = 0; i < rv.pts.size(); ++i) {
                    const Vec2 v = rv.pts[i];
                    const double ml = rv.miter[i].length();
                    if (ml <= 1e-12)
                        continue;
                    const Vec2 mHat = rv.miter[i] * (1.0 / ml);
                    double& sv = safe[ri][i];
                    const int ix0 = cx(v.x - reachMax), ix1 = cx(v.x + reachMax);
                    const int iy0 = cy(v.y - reachMax), iy1 = cy(v.y + reachMax);
                    for (int iy = iy0; iy <= iy1; ++iy) {
                        for (int ix = ix0; ix <= ix1; ++ix) {
                            const std::size_t c0 = static_cast<std::size_t>(iy) * nx + ix;
                            for (std::uint32_t s = starts[c0]; s < starts[c0 + 1]; ++s) {
                                const GEdge& e = ge[items[s]];
                                const std::size_t rk = e.ring;
                                const std::size_t n = rings[rk].pts.size();
                                const std::size_t k = e.k;
                                if (rk == ri && (k == i || (k + 1) % n == i))
                                    continue; // the two edges that own vertex i
                                const Vec2 a = e.a;
                                const Vec2 c = e.c;
                                const double reach =
                                    2.0 * sv; // farther walls cannot tighten the bound
                                if ((a.x < v.x - reach && c.x < v.x - reach) ||
                                    (a.x > v.x + reach && c.x > v.x + reach) ||
                                    (a.y < v.y - reach && c.y < v.y - reach) ||
                                    (a.y > v.y + reach && c.y > v.y + reach))
                                    continue;
                                if (mHat.dot(edgeOutwardNormal(a, c)) > -0.1)
                                    continue; // not a facing wall
                                const Vec2 ac = c - a;
                                const double len2 = ac.dot(ac);
                                const double t =
                                    len2 > 0.0 ? std::clamp((v - a).dot(ac) / len2, 0.0, 1.0) : 0.0;
                                const Vec2 q = a + ac * t;
                                const Vec2 d = q - v;
                                const double dist = d.length();
                                if (dist <= 1e-12) {
                                    sv = 0.0;
                                    continue;
                                }
                                if (d.dot(mHat) > -1e-12)
                                    continue; // on the outward side
                                sv = std::min(sv, dist * 0.5);
                            }
                        }
                    }
                }
            }
        }
    }

    for (auto& ringSafe : safe)
        for (double& s : ringSafe) s = std::min(request, 0.9 * s);
    return safe;
}

// One bevel profile station: the ring inset by `inset`, at depth-offset `zOff` from the cap plane
// toward the wall, with normals blending u (outward 2D) at blend=0 to +/-z at blend=1.
struct ProfileStation {
    double inset = 0.0;   // inward distance from the outline
    double zBack = 0.0;   // distance BACK from the cap plane (0 = the cap edge)
    double uWeight = 1.0; // normal = normalize(u * uWeight + zAxis * zWeight)
    double zWeight = 0.0;
};

// The profile stations from the WALL end (largest zBack) to the CAP edge (zBack 0), excluding the
// wall junction itself (inset 0, zBack = size) which the caller emits as the wall's edge.
std::vector<ProfileStation> profileStations(const Bevel& bevel) {
    std::vector<ProfileStation> st;
    const double s = bevel.size;
    if (s <= 0.0) return st;
    const int segs = std::max(1, bevel.profile == Bevel::Profile::Flat ? 1 : bevel.segments);
    st.reserve(static_cast<std::size_t>(segs) + 1);
    constexpr double kHalfPi = 1.5707963267948966;
    for (int k = 0; k <= segs; ++k) {
        const double t = static_cast<double>(k) / segs;  // 0 = wall end, 1 = cap edge
        ProfileStation p;
        switch (bevel.profile) {
        case Bevel::Profile::Flat:
            p.inset = s * t;
            p.zBack = s * (1.0 - t);
            p.uWeight = 1.0;  // constant 45-degree chamfer normal
            p.zWeight = 1.0;
            break;
        case Bevel::Profile::Round: {  // quarter circle bulging outward (normals u -> z)
            const double phi = t * kHalfPi;
            p.inset = s * (1.0 - std::cos(phi));
            p.zBack = s * (1.0 - std::sin(phi));
            p.uWeight = std::cos(phi);
            p.zWeight = std::sin(phi);
            break;
        }
        case Bevel::Profile::Convex: {  // bullnose: a semicircle over the chamfer chord
            // Chord from the wall end (inset 0, zBack s) to the cap edge (inset s, zBack 0); the
            // semicircle bulges OUTWARD past the chord -- its apex passes exactly through where
            // the sharp corner (inset 0, zBack 0) would have been. Rotate the start point about
            // the chord midpoint; the surface normal opposes the radial (the inset axis points
            // inward and zBack points backward, both against their 3D counterparts).
            const double phi = t * 3.14159265358979323846;
            const double cs = std::cos(phi), sn = std::sin(phi);
            const double ax = -0.5 * s, az = 0.5 * s;  // start, relative to the chord midpoint
            const double rx = ax * cs - az * sn;
            const double rz = ax * sn + az * cs;
            p.inset = 0.5 * s + rx;  // negative mid-arc: the bulge pokes outside the outline
            p.zBack = 0.5 * s + rz;
            const double rl = std::sqrt(rx * rx + rz * rz);
            p.uWeight = rl > 1e-12 ? -rx / rl : 1.0;
            p.zWeight = rl > 1e-12 ? -rz / rl : 0.0;
            break;
        }
        case Bevel::Profile::Concave: {  // cove: quarter circle cut inward (normals z -> u)
            const double phi = t * kHalfPi;
            p.inset = s * std::sin(phi);
            p.zBack = s * std::cos(phi);
            p.uWeight = std::sin(phi);
            p.zWeight = std::cos(phi);
            break;
        }
        }
        st.push_back(p);
    }
    return st;
}

struct Builder {
    ExtrudeMesh mesh;
    // The current ring's unrolled-side bookkeeping (§12 wrap mode): `sideBase` is where this ring
    // starts on the concatenated axis, `sidePrefix[i]` the arc length from the ring's first point
    // to pts[i] (RAW design units -- the post-pass normalizes over the mesh total). The BASE
    // outline's lengths parameterize every station of the ring (wall + bevels), so the profile
    // strips stack vertically in the side domain.
    double sideBase = 0.0;
    std::vector<double> sidePrefix;
    double sideTotal = 0.0;  // the current ring's full perimeter

    std::uint32_t addVertex(Vec3 pos, Vec3 n, bool cap = false, Vec2 side = {}) {
        mesh.vertices.push_back(
            {pos, n.normalized(), Vec2{}, side, cap ? 1.0f : 0.0f});  // UVs are a post-pass
        return static_cast<std::uint32_t>(mesh.vertices.size() - 1);
    }
    void addTri(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        mesh.indices.push_back(a);
        mesh.indices.push_back(b);
        mesh.indices.push_back(c);
    }

    // Start a ring's side band: compute its prefix lengths and record its return-map stations
    // (s -> design point; the closing knot repeats the first point at the full perimeter).
    void beginRingSide(const Ring& r) {
        const std::size_t n = r.pts.size();
        sidePrefix.assign(n + 1, 0.0);
        for (std::size_t i = 0; i < n; ++i)
            sidePrefix[i + 1] = sidePrefix[i] + (r.pts[(i + 1) % n] - r.pts[i]).length();
        sideTotal = sidePrefix[n];
        for (std::size_t i = 0; i <= n; ++i)
            mesh.sideStations.push_back({static_cast<float>(sideBase + sidePrefix[i]),
                                         r.pts[i % n]});  // raw s; normalized in the post-pass
    }
    void endRingSide() { sideBase += sideTotal; }

    // A band between two stations of one ring: quad per edge, split vertices (band-local normals).
    // ringA/ringB are the station outlines; each edge takes its endpoint normals from the ring's
    // edgeNormals (smooth/sharp already decided) tilted by the station's u/z weights. Side
    // coordinates: s from the BASE ring's prefix lengths (the closing edge's second endpoint takes
    // the full perimeter, not 0, so the seam carries its whole length), t from each station's z
    // (raw z here; the post-pass maps it to the 0..1 front->back range).
    void emitBand(const Ring& r, const std::vector<Vec2>& ringA, double zA, double uwA, double zwA,
                  const std::vector<Vec2>& ringB, double zB, double uwB, double zwB, double zSign) {
        const std::size_t n = r.pts.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const Vec2 n0 = r.edgeNormals[2 * i + 0];
            const Vec2 n1 = r.edgeNormals[2 * i + 1];
            const auto tilt = [&](Vec2 u2, double uw, double zw) {
                return Vec3{u2.x * uw, u2.y * uw, zSign * zw};
            };
            const double s0 = sideBase + sidePrefix[i];
            const double s1 = sideBase + sidePrefix[i + 1];  // == full perimeter on the last edge
            const std::uint32_t a0 = addVertex({ringA[i].x, ringA[i].y, zA}, tilt(n0, uwA, zwA),
                                               false, {s0, zA});
            const std::uint32_t a1 = addVertex({ringA[j].x, ringA[j].y, zA}, tilt(n1, uwA, zwA),
                                               false, {s1, zA});
            const std::uint32_t b0 = addVertex({ringB[i].x, ringB[i].y, zB}, tilt(n0, uwB, zwB),
                                               false, {s0, zB});
            const std::uint32_t b1 = addVertex({ringB[j].x, ringB[j].y, zB}, tilt(n1, uwB, zwB),
                                               false, {s1, zB});
            addTri(a0, a1, b1);
            addTri(a0, b1, b0);
        }
    }
};

}  // namespace

ExtrudeMesh buildExtrudeMesh(const std::vector<GlyphSolidInput>& glyphs, const Extrude& params) {
    Builder b;

    // Bevel sizes clamped so front + back never swallow the solid (leave >= 10% real wall).
    const double depth = std::max(0.01, static_cast<double>(params.depth));
    double sFront = std::max(0.0, static_cast<double>(params.bevelFront.size));
    double sBack = std::max(0.0, static_cast<double>(params.bevelBack.size));
    if (const double total = sFront + sBack; total > 0.9 * depth) {
        const double k = 0.9 * depth / total;
        sFront *= k;
        sBack *= k;
    }
    const double zF = depth * 0.5;   // front cap plane (+z toward the viewer)
    const double zB = -depth * 0.5;  // back cap plane

    // Design bounds over every input outline (the §12 UV domain).
    double minX = std::numeric_limits<double>::infinity(), minY = minX;
    double maxX = -minX, maxY = -minY;

    for (const GlyphSolidInput& glyph : glyphs) {
        const std::uint32_t rangeStart = static_cast<std::uint32_t>(b.mesh.indices.size());

        // --- Prepare + classify this glyph's rings (containment depth -> outer/hole) ----------
        std::vector<Ring> rings;
        for (const vec::Contour& c : glyph.contours) {
            if (!c.closed) continue;  // an open contour has no solid to extrude
            Ring r;
            if (prepareRing(c.points, r)) rings.push_back(std::move(r));
        }
        if (rings.empty()) continue;
        for (const Ring& r : rings)
            for (const Vec2& p : r.pts) {
                minX = std::min(minX, p.x);
                minY = std::min(minY, p.y);
                maxX = std::max(maxX, p.x);
                maxY = std::max(maxY, p.y);
            }

        std::vector<int> depthOf(rings.size(), 0);
        for (std::size_t i = 0; i < rings.size(); ++i)
            for (std::size_t k = 0; k < rings.size(); ++k)
                if (k != i && pointInRing(rings[i].pts.front(), rings[k].pts)) ++depthOf[i];

        for (std::size_t i = 0; i < rings.size(); ++i) {
            Ring& r = rings[i];
            r.hole = (depthOf[i] % 2) == 1;
            // Force orientation: outers visually clockwise (area > 0 in y-down), holes the
            // opposite -- then one outward-normal formula serves every ring.
            const bool wantPositive = !r.hole;
            if ((r.area > 0.0) != wantPositive) {
                std::reverse(r.pts.begin(), r.pts.end());
                r.area = -r.area;
            }
            buildRingNormals(r);
        }

        // The per-vertex bevel travel bounds (see safeInsets): computed once per glyph, shared by
        // caps and bevel stations so their outlines stay bit-identical (watertightness).
        const double maxInset = std::max(sFront, sBack);
        std::vector<std::vector<double>> safe;
        if (maxInset > 0.0) safe = safeInsets(rings, maxInset);
        const auto safeOf = [&](const Ring* r) -> const std::vector<double>* {
            if (safe.empty()) return nullptr;
            return &safe[static_cast<std::size_t>(r - rings.data())];
        };

        // --- Caps: earcut per outer-with-its-holes group --------------------------------------
        for (std::size_t i = 0; i < rings.size(); ++i) {
            if (rings[i].hole) continue;
            // The group: this outer + every hole whose immediate parent it is (depth + 1 and
            // contained). Build the (possibly inset) cap outlines front and back.
            std::vector<const Ring*> group{&rings[i]};
            for (std::size_t k = 0; k < rings.size(); ++k)
                if (rings[k].hole && depthOf[k] == depthOf[i] + 1 &&
                    pointInRing(rings[k].pts.front(), rings[i].pts))
                    group.push_back(&rings[k]);

            const auto capOf = [&](double inset, double z, double nz) {
                using EPoint = std::array<double, 2>;
                std::vector<std::vector<EPoint>> poly;
                std::vector<std::uint32_t> vtx;  // mesh vertex per earcut point, flattened order
                poly.reserve(group.size());
                for (const Ring* r : group) {
                    std::vector<EPoint> ringPts;
                    const std::vector<Vec2> pts =
                        inset > 0.0 ? insetRing(*r, inset, safeOf(r)) : r->pts;
                    ringPts.reserve(pts.size());
                    for (const Vec2& p : pts) {
                        ringPts.push_back({p.x, p.y});
                        vtx.push_back(b.addVertex({p.x, p.y, z}, {0.0, 0.0, nz}, /*cap=*/true));
                    }
                    poly.push_back(std::move(ringPts));
                }
                const std::vector<std::uint32_t> tri = mapbox::earcut<std::uint32_t>(poly);
                for (std::size_t t = 0; t + 2 < tri.size(); t += 3) {
                    // earcut winds for a y-up frame; our y is down. Emit as-is for the front cap
                    // and swapped for the back -- the analytic normals carry the meaning either
                    // way, the order only keeps a consistent convention for later GPU culling.
                    if (nz > 0.0)
                        b.addTri(vtx[tri[t]], vtx[tri[t + 1]], vtx[tri[t + 2]]);
                    else
                        b.addTri(vtx[tri[t]], vtx[tri[t + 2]], vtx[tri[t + 1]]);
                }
            };
            capOf(sFront, zF, 1.0);
            capOf(sBack, zB, -1.0);
        }

        // --- Walls + bevels per ring -----------------------------------------------------------
        for (const Ring& r : rings) {
            b.beginRingSide(r);  // side-domain bookkeeping (§12 wrap): prefix lengths + stations
            const std::vector<Vec2>& outline = r.pts;
            // The wall band spans the un-beveled middle.
            const double wallTop = zF - sFront;   // toward the viewer
            const double wallBot = zB + sBack;
            b.emitBand(r, outline, wallBot, 1.0, 0.0, outline, wallTop, 1.0, 0.0, 1.0);

            // Front bevel: stations run wall end -> cap edge; band k connects k to k+1.
            const auto emitBevel = [&](const Bevel& bevel, double s, double capZ, double zSign) {
                if (s <= 0.0) return;
                Bevel scaled = bevel;
                scaled.size = static_cast<float>(s);
                const std::vector<ProfileStation> st = profileStations(scaled);
                for (std::size_t k = 0; k + 1 < st.size(); ++k) {
                    const ProfileStation& a = st[k];
                    const ProfileStation& c = st[k + 1];
                    // A negative inset (the Convex bulge) moves OUTWARD -- insetRing handles both
                    // (the travel bound is symmetric: bulges stay proportionate to the stroke).
                    const std::vector<Vec2> ringA =
                        a.inset != 0.0 ? insetRing(r, a.inset, safeOf(&r)) : r.pts;
                    const std::vector<Vec2> ringC =
                        c.inset != 0.0 ? insetRing(r, c.inset, safeOf(&r)) : r.pts;
                    b.emitBand(r, ringA, capZ - zSign * a.zBack, a.uWeight, a.zWeight, ringC,
                               capZ - zSign * c.zBack, c.uWeight, c.zWeight, zSign);
                }
            };
            emitBevel(params.bevelFront, sFront, zF, 1.0);
            emitBevel(params.bevelBack, sBack, zB, -1.0);
            b.endRingSide();
        }

        // --- The §10.4 material range for this glyph ------------------------------------------
        const std::uint32_t count =
            static_cast<std::uint32_t>(b.mesh.indices.size()) - rangeStart;
        if (count > 0) {
            if (!b.mesh.ranges.empty() && b.mesh.ranges.back().runIndex == glyph.runIndex &&
                b.mesh.ranges.back().firstIndex + b.mesh.ranges.back().indexCount == rangeStart) {
                b.mesh.ranges.back().indexCount += count;  // merge with the previous glyph's run
            } else {
                b.mesh.ranges.push_back({glyph.runIndex, rangeStart, count});
            }
        }
    }

    // --- Design-space UVs over the whole block (§12) -------------------------------------------
    if (!b.mesh.vertices.empty() && maxX > minX && maxY > minY) {
        b.mesh.designBounds = Rect{minX, minY, maxX - minX, maxY - minY};
        for (ExtrudeVertex& v : b.mesh.vertices)
            v.uv = {(v.position.x - minX) / (maxX - minX),
                    (v.position.y - minY) / (maxY - minY)};
    }
    // --- The unrolled side domain (§12 wrap mode): normalize s over the concatenated length and
    // map each side vertex's raw z (stashed in side.y by emitBand) to the 0..1 front->back range.
    b.mesh.sideLength = b.sideBase;
    if (b.mesh.sideLength > 0.0) {
        for (ExtrudeVertex& v : b.mesh.vertices) {
            if (v.cap > 0.5f) continue;  // caps carry no side coords
            v.side.x /= b.mesh.sideLength;
            v.side.y = (zF - v.side.y) / depth;
        }
        for (SideStation& st : b.mesh.sideStations)
            st.s = static_cast<float>(st.s / b.mesh.sideLength);
    } else {
        b.mesh.sideStations.clear();
    }
    return std::move(b.mesh);
}

}  // namespace mosaic::core::text
