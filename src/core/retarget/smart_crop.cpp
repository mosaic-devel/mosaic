// Smart Resize crop-window search — see smart_crop.hpp for the objective, the guardrails and
// the lineage credits.

#include "core/retarget/smart_crop.hpp"

#include "core/retarget/keep_regions.hpp" // shared ImportanceBlob / findImportanceBlobs

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mosaic::core::retarget {
namespace {

// Summed-area table of one field: sat(x, y) = Σ field over [0..x) × [0..y), so any window sum
// is four lookups. Double precision keeps the sums exact enough to compare windows on maps of
// this size (≤ ~10^5 cells of [0, ~1.5] floats).
class Sat {
public:
    Sat() = default;
    Sat(const std::vector<float>& field, std::uint32_t w, std::uint32_t h) : m_w(w + 1) {
        m_t.assign(static_cast<std::size_t>(w + 1) * (h + 1), 0.0);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
                m_t[idx(x + 1, y + 1)] = field[static_cast<std::size_t>(y) * w + x] +
                                         m_t[idx(x, y + 1)] + m_t[idx(x + 1, y)] - m_t[idx(x, y)];
    }
    // Sum over [x0, x1) × [y0, y1); caller guarantees x0 <= x1, y0 <= y1 within bounds.
    [[nodiscard]] double sum(std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                             std::uint32_t y1) const {
        return m_t[idx(x1, y1)] - m_t[idx(x0, y1)] - m_t[idx(x1, y0)] + m_t[idx(x0, y0)];
    }

private:
    [[nodiscard]] std::size_t idx(std::uint32_t x, std::uint32_t y) const {
        return static_cast<std::size_t>(y) * m_w + x;
    }
    std::uint32_t m_w = 1;
    std::vector<double> m_t;
};

// An axis-aligned box in integer map space (half-open), the candidate-window currency.
struct MapBox {
    std::uint32_t x0, y0, x1, y1;
};

// Doc-space rect -> conservative half-open map box (floor/ceil so any doc overlap counts).
// False for a rect that lands on no map cell.
bool docRectToMapBox(const ImportanceMap& m, const common::Rect& r, MapBox& out) {
    if (r.empty())
        return false;
    const double sx = static_cast<double>(m.width) / m.sourceW;
    const double sy = static_cast<double>(m.height) / m.sourceH;
    const auto x0 = static_cast<std::uint32_t>(
        std::clamp(std::floor(r.x * sx), 0.0, static_cast<double>(m.width)));
    const auto y0 = static_cast<std::uint32_t>(
        std::clamp(std::floor(r.y * sy), 0.0, static_cast<double>(m.height)));
    const auto x1 = static_cast<std::uint32_t>(
        std::clamp(std::ceil(r.right() * sx), 0.0, static_cast<double>(m.width)));
    const auto y1 = static_cast<std::uint32_t>(
        std::clamp(std::ceil(r.bottom() * sy), 0.0, static_cast<double>(m.height)));
    if (x1 <= x0 || y1 <= y0)
        return false;
    out = {x0, y0, x1, y1};
    return true;
}

// "Off" chips are ACTIVELY ignored (SmartCropOptions::excludeRects): zero their cells so the
// content stops attracting the window — not merely unprotected (user 2026-07-02). Returns `map`
// untouched (no copy) when nothing masks.
const ImportanceMap& maskExclusions(const ImportanceMap& map,
                                    const std::vector<common::Rect>& excludes,
                                    ImportanceMap& storage) {
    bool any = false;
    for (const common::Rect& r : excludes) {
        MapBox b{};
        if (!docRectToMapBox(map, r, b))
            continue;
        if (!any) {
            storage = map;
            any = true;
        }
        for (std::uint32_t y = b.y0; y < b.y1; ++y)
            for (std::uint32_t x = b.x0; x < b.x1; ++x)
                storage.w[static_cast<std::size_t>(y) * storage.width + x] = 0.0f;
    }
    return any ? storage : map;
}

// Fraction of `r`'s area inside `win` (both half-open map boxes): 1 = fully kept, 0 = fully
// outside, in between = sliced.
double containedFraction(const MapBox& r, const MapBox& win) {
    const double area = static_cast<double>(r.x1 - r.x0) * (r.y1 - r.y0);
    if (area <= 0.0)
        return 1.0;
    const double ox = std::max(0.0, static_cast<double>(std::min(r.x1, win.x1)) -
                                        static_cast<double>(std::max(r.x0, win.x0)));
    const double oy = std::max(0.0, static_cast<double>(std::min(r.y1, win.y1)) -
                                        static_cast<double>(std::max(r.y0, win.y0)));
    return ox * oy / area;
}

// The full window objective (smart_crop.hpp). All terms are normalized fractions so the λs
// compose predictably.
struct Scorer {
    const ImportanceMap& map;
    const SmartCropOptions& opts;
    Sat mass;   // SAT of W
    Sat momX;   // SAT of W(x,y) * (x + 0.5)  — for the retained-mass centroid
    Sat momY;   // SAT of W(x,y) * (y + 0.5)
    double total = 0.0;
    std::vector<ImportanceBlob> blobs; // shared finder (keep_regions.hpp)
    std::vector<MapBox> protects;      // protectRects mapped into map space

    explicit Scorer(const ImportanceMap& m, const SmartCropOptions& o) : map(m), opts(o) {
        std::vector<float> mx(m.w.size()), my(m.w.size());
        for (std::uint32_t y = 0; y < m.height; ++y)
            for (std::uint32_t x = 0; x < m.width; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * m.width + x;
                mx[i] = m.w[i] * (static_cast<float>(x) + 0.5f);
                my[i] = m.w[i] * (static_cast<float>(y) + 0.5f);
            }
        mass = Sat(m.w, m.width, m.height);
        momX = Sat(mx, m.width, m.height);
        momY = Sat(my, m.width, m.height);
        total = mass.sum(0, 0, m.width, m.height);
        float peak = 0.0f;
        for (const float v : m.w)
            peak = std::max(peak, v);
        blobs = findImportanceBlobs(m, opts.blobThreshold * peak, opts.blobMinMassFrac, total);
        for (const common::Rect& r : opts.protectRects) {
            MapBox b{};
            if (docRectToMapBox(m, r, b))
                protects.push_back(b);
        }
    }

    [[nodiscard]] double score(const MapBox& win) const {
        if (total <= 0.0)
            return 0.0;
        const double kept = mass.sum(win.x0, win.y0, win.x1, win.y1);
        double s = kept / total;
        // Boundary band: mass in the outer ~4% ring of the window (≥ 1 map px).
        const std::uint32_t bw = win.x1 - win.x0, bh = win.y1 - win.y0;
        const std::uint32_t band =
            std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::min(bw, bh) * 0.04));
        if (bw > 2 * band && bh > 2 * band) {
            const double inner = mass.sum(win.x0 + band, win.y0 + band, win.x1 - band,
                                          win.y1 - band);
            s -= opts.lambdaCut * (kept - inner) / total;
        }
        // Blob wholeness: a partially clipped blob costs by its mass and how badly it is cut
        // (0 at fully-in/out, peaking at half-in) — the benchmark's "no half-objects".
        for (const ImportanceBlob& b : blobs) {
            const double f = containedFraction({b.x0, b.y0, b.x1, b.y1}, win);
            if (f > 0.0 && f < 1.0)
                s -= opts.lambdaBlob * b.massFrac * 2.0 * std::min(f, 1.0 - f);
        }
        // Protect rects: ANY slicing costs almost-hard, scaled so "barely nicked" still ranks
        // above "cut in half" when no clean placement exists at all.
        for (const MapBox& p : protects) {
            const double f = containedFraction(p, win);
            if (f > 0.0 && f < 1.0)
                s -= opts.lambdaProtect * (0.5 + std::min(f, 1.0 - f));
        }
        // Composition: the retained-mass centroid, in window-normalized coords, near a thirds
        // point or the centre (centre included so symmetric content is not pushed off-axis).
        if (kept > 0.0) {
            const double cx = momX.sum(win.x0, win.y0, win.x1, win.y1) / kept;
            const double cy = momY.sum(win.x0, win.y0, win.x1, win.y1) / kept;
            const double u = (cx - win.x0) / bw;
            const double v = (cy - win.y0) / bh;
            constexpr double kAnchors[5][2] = {
                {1.0 / 3, 1.0 / 3}, {2.0 / 3, 1.0 / 3}, {1.0 / 3, 2.0 / 3}, {2.0 / 3, 2.0 / 3},
                {0.5, 0.5}};
            double best = 1e9;
            for (const auto& a : kAnchors)
                best = std::min(best, std::hypot(u - a[0], v - a[1]));
            s += opts.lambdaComp * std::clamp(1.0 - best / 0.5, 0.0, 1.0);
        }
        return s;
    }
};

} // namespace

namespace {

// The aspect-constrained window search (§4.3), over an already-exclusion-masked map.
common::Rect chooseAspectWindow(const ImportanceMap& map, double targetAspect,
                                const SmartCropOptions& opts) {
    const common::Rect full{0.0, 0.0, static_cast<double>(map.sourceW),
                            static_cast<double>(map.sourceH)};
    const Scorer scorer(map, opts);
    const double srcW = map.sourceW, srcH = map.sourceH;
    // Document-space window at the max fit for this aspect (uniform scale s shrinks it).
    const double maxW = std::min(srcW, srcH * targetAspect);
    const double maxH = maxW / targetAspect;
    const double toMapX = static_cast<double>(map.width) / srcW;
    const double toMapY = static_cast<double>(map.height) / srcH;

    struct Best {
        double score = -1e18;
        MapBox box{0, 0, 0, 0};
        double docW = 0.0, docH = 0.0;
    } best;

    const int scaleSteps = std::max(0, opts.scaleSteps);
    const double minScale = std::clamp(opts.minScale, 0.05, 1.0);
    // A candidate window's integer map box for doc-space size (w, h) at map position (x0, y0).
    const auto evaluate = [&](double docW, double docH, std::uint32_t x0, std::uint32_t y0) {
        const std::uint32_t bw = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(std::lround(docW * toMapX)));
        const std::uint32_t bh = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(std::lround(docH * toMapY)));
        if (x0 + bw > map.width || y0 + bh > map.height)
            return;
        const MapBox box{x0, y0, x0 + bw, y0 + bh};
        const double s = scorer.score(box);
        if (s > best.score) { // strictly greater: first-found in scan order wins ties
            best = {s, box, docW, docH};
        }
    };

    for (int si = 0; si <= scaleSteps; ++si) {
        const double s = scaleSteps == 0
                             ? 1.0
                             : 1.0 - (1.0 - minScale) * (static_cast<double>(si) / scaleSteps);
        const double docW = maxW * s, docH = maxH * s;
        const std::uint32_t bw = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(std::lround(docW * toMapX)));
        const std::uint32_t bh = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(std::lround(docH * toMapY)));
        if (bw > map.width || bh > map.height)
            continue;
        const std::uint32_t freeX = map.width - bw, freeY = map.height - bh;
        const int steps = std::max(1, opts.coarseSteps);
        for (int iy = 0; iy <= steps; ++iy) {
            const auto y0 = static_cast<std::uint32_t>(
                std::lround(static_cast<double>(freeY) * iy / steps));
            for (int ix = 0; ix <= steps; ++ix) {
                const auto x0 = static_cast<std::uint32_t>(
                    std::lround(static_cast<double>(freeX) * ix / steps));
                evaluate(docW, docH, x0, y0);
            }
        }
    }
    if (best.docW <= 0.0)
        return full;

    // Refine THE best candidate by axis-aligned hill-climbing at 1-map-px steps (deterministic
    // neighbour order; bounded). One window in, one window out — no alternatives are kept.
    bool improved = true;
    int guard = 4 * (static_cast<int>(std::max(map.width, map.height)) + 1);
    while (improved && guard-- > 0) {
        improved = false;
        const MapBox b = best.box;
        const std::uint32_t bw = b.x1 - b.x0, bh = b.y1 - b.y0;
        constexpr int kDx[4] = {-1, 1, 0, 0};
        constexpr int kDy[4] = {0, 0, -1, 1};
        for (int d = 0; d < 4; ++d) {
            const long nx = static_cast<long>(b.x0) + kDx[d];
            const long ny = static_cast<long>(b.y0) + kDy[d];
            if (nx < 0 || ny < 0 || nx + bw > map.width || ny + bh > map.height)
                continue;
            const MapBox cand{static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny),
                              static_cast<std::uint32_t>(nx) + bw,
                              static_cast<std::uint32_t>(ny) + bh};
            const double s = scorer.score(cand);
            if (s > best.score) {
                best.score = s;
                best.box = cand;
                improved = true;
                break; // re-evaluate neighbours of the new position
            }
        }
    }

    // Back to document space: exact doc-space size, position from the map-space offset (the
    // staged-rect snap rounds to whole pixels downstream).
    common::Rect out{best.box.x0 / toMapX, best.box.y0 / toMapY, best.docW, best.docH};
    out.x = std::clamp(out.x, 0.0, srcW - out.w);
    out.y = std::clamp(out.y, 0.0, srcH - out.h);
    return out;
}

// Free-aspect SMART TRIM (the header contract; Suh 2003's thumbnail-crop formulation): pull each
// edge inward one map-cell strip at a time, always the cheapest (lowest-density) edge first,
// while a strip is BOTH cheap (density below trimCheapDensity x the map mean — a busy image
// stays full) AND affordable (retained mass stays >= trimKeepMass), and never into a protect
// rect. Deterministic: fixed edge order (left, top, right, bottom) breaks density ties.
common::Rect chooseTrimWindow(const ImportanceMap& map, const SmartCropOptions& opts) {
    const common::Rect full{0.0, 0.0, static_cast<double>(map.sourceW),
                            static_cast<double>(map.sourceH)};
    const Sat sat(map.w, map.width, map.height);
    const double total = sat.sum(0, 0, map.width, map.height);
    if (total <= 0.0)
        return full;
    std::vector<MapBox> protects;
    for (const common::Rect& r : opts.protectRects) {
        MapBox b{};
        if (docRectToMapBox(map, r, b))
            protects.push_back(b);
    }
    std::uint32_t x0 = 0, y0 = 0, x1 = map.width, y1 = map.height;
    double kept = total;
    const double cheap =
        opts.trimCheapDensity * (total / (static_cast<double>(map.width) * map.height));
    const double keepFloor = opts.trimKeepMass * total;
    // Never trim below a usable frame, whatever the map says.
    const std::uint32_t minW = std::max<std::uint32_t>(2, map.width / 8);
    const std::uint32_t minH = std::max<std::uint32_t>(2, map.height / 8);
    const auto hitsProtect = [&](std::uint32_t sx0, std::uint32_t sy0, std::uint32_t sx1,
                                 std::uint32_t sy1) {
        for (const MapBox& p : protects)
            if (sx0 < p.x1 && sx1 > p.x0 && sy0 < p.y1 && sy1 > p.y0)
                return true;
        return false;
    };
    while (true) {
        double bestDensity = 1e30;
        double bestMass = 0.0;
        int bestEdge = -1;
        const auto consider = [&](int edge, double mass, double area, bool room, bool blocked) {
            if (!room || blocked || kept - mass < keepFloor)
                return;
            const double d = mass / area;
            if (d <= cheap && d < bestDensity) {
                bestDensity = d;
                bestMass = mass;
                bestEdge = edge;
            }
        };
        consider(0, sat.sum(x0, y0, x0 + 1, y1), y1 - y0, x1 - x0 > minW,
                 hitsProtect(x0, y0, x0 + 1, y1)); // left strip
        consider(1, sat.sum(x0, y0, x1, y0 + 1), x1 - x0, y1 - y0 > minH,
                 hitsProtect(x0, y0, x1, y0 + 1)); // top strip
        consider(2, sat.sum(x1 - 1, y0, x1, y1), y1 - y0, x1 - x0 > minW,
                 hitsProtect(x1 - 1, y0, x1, y1)); // right strip
        consider(3, sat.sum(x0, y1 - 1, x1, y1), x1 - x0, y1 - y0 > minH,
                 hitsProtect(x0, y1 - 1, x1, y1)); // bottom strip
        if (bestEdge < 0)
            break;
        kept -= bestMass;
        if (bestEdge == 0)
            ++x0;
        else if (bestEdge == 1)
            ++y0;
        else if (bestEdge == 2)
            --x1;
        else
            --y1;
    }
    const double toDocX = static_cast<double>(map.sourceW) / map.width;
    const double toDocY = static_cast<double>(map.sourceH) / map.height;
    common::Rect out{x0 * toDocX, y0 * toDocY, (x1 - x0) * toDocX, (y1 - y0) * toDocY};
    out.x = std::clamp(out.x, 0.0, static_cast<double>(map.sourceW) - out.w);
    out.y = std::clamp(out.y, 0.0, static_cast<double>(map.sourceH) - out.h);
    return out;
}

} // namespace

common::Rect chooseCropWindow(const ImportanceMap& map, double targetAspect,
                              const SmartCropOptions& opts) {
    const common::Rect full{0.0, 0.0, static_cast<double>(map.sourceW),
                            static_cast<double>(map.sourceH)};
    if (map.empty() || map.sourceW == 0 || map.sourceH == 0)
        return full;
    ImportanceMap maskedStorage;
    const ImportanceMap& w = maskExclusions(map, opts.excludeRects, maskedStorage);
    if (targetAspect <= 0.0)
        return chooseTrimWindow(w, opts); // Free = smart trim (header contract)
    return chooseAspectWindow(w, targetAspect, opts);
}

common::Rect chooseCropWindow(const common::Image& src, double targetAspect,
                              const SmartCropOptions& opts, const ImportanceOptions& mapOpts) {
    return chooseCropWindow(buildImportanceMap(src, mapOpts), targetAspect, opts);
}

} // namespace mosaic::core::retarget
