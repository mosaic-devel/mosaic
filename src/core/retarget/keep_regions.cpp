// Keep-region extraction — see keep_regions.hpp for the model and the design notes.

#include "core/retarget/keep_regions.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace mosaic::core::retarget {

std::vector<ImportanceBlob> findImportanceBlobs(const ImportanceMap& map, double absThreshold,
                                                double minMassFrac, double totalMass) {
    std::vector<ImportanceBlob> blobs;
    if (map.empty() || totalMass <= 0.0)
        return blobs;
    const std::uint32_t w = map.width, h = map.height;
    std::vector<std::uint8_t> seen(static_cast<std::size_t>(w) * h, 0);
    std::vector<std::uint32_t> stack;
    for (std::uint32_t sy = 0; sy < h; ++sy) {
        for (std::uint32_t sx = 0; sx < w; ++sx) {
            const std::size_t si = static_cast<std::size_t>(sy) * w + sx;
            if (seen[si] != 0 || map.w[si] < absThreshold)
                continue;
            ImportanceBlob b{sx, sy, sx + 1, sy + 1, 0.0, 0};
            stack.assign(1, static_cast<std::uint32_t>(si));
            seen[si] = 1;
            while (!stack.empty()) {
                const std::uint32_t i = stack.back();
                stack.pop_back();
                const std::uint32_t x = i % w, y = i / w;
                b.x0 = std::min(b.x0, x);
                b.y0 = std::min(b.y0, y);
                b.x1 = std::max(b.x1, x + 1);
                b.y1 = std::max(b.y1, y + 1);
                b.massFrac += map.w[i];
                ++b.cells;
                const auto push = [&](std::uint32_t nx, std::uint32_t ny) {
                    const std::size_t ni = static_cast<std::size_t>(ny) * w + nx;
                    if (seen[ni] == 0 && map.w[ni] >= absThreshold) {
                        seen[ni] = 1;
                        stack.push_back(static_cast<std::uint32_t>(ni));
                    }
                };
                if (x > 0)
                    push(x - 1, y);
                if (x + 1 < w)
                    push(x + 1, y);
                if (y > 0)
                    push(x, y - 1);
                if (y + 1 < h)
                    push(x, y + 1);
            }
            b.massFrac /= totalMass;
            if (b.massFrac >= minMassFrac)
                blobs.push_back(b);
        }
    }
    return blobs;
}

namespace {

// Whether two half-open map boxes overlap or sit within `gap` cells of each other on both axes
// (per-axis separation is 0 for overlapping boxes).
bool nearTouch(const ImportanceBlob& a, const ImportanceBlob& b, std::uint32_t gap) {
    const auto axisGap = [](std::uint32_t a0, std::uint32_t a1, std::uint32_t b0,
                            std::uint32_t b1) -> std::uint32_t {
        if (b0 > a1)
            return b0 - a1;
        if (a0 > b1)
            return a0 - b1;
        return 0;
    };
    return axisGap(a.x0, a.x1, b.x0, b.x1) <= gap && axisGap(a.y0, a.y1, b.y0, b.y1) <= gap;
}

} // namespace

std::vector<KeepRegion> extractKeepRegions(const ImportanceMap& map,
                                           const KeepRegionOptions& opts,
                                           const std::vector<common::Rect>& faceRects) {
    std::vector<KeepRegion> out;
    if (map.empty() || map.sourceW == 0 || map.sourceH == 0)
        return out;

    double total = 0.0;
    float peak = 0.0f;
    for (const float v : map.w) {
        total += v;
        peak = std::max(peak, v);
    }
    std::vector<ImportanceBlob> blobs =
        findImportanceBlobs(map, opts.blobThreshold * peak, opts.blobMinMassFrac, total);
    // Cell floor: the mass floor is set low enough for a small genuine subject, so lone texture
    // speckles (1–3 cells of grass glint) are filtered by SIZE instead.
    std::erase_if(blobs, [&](const ImportanceBlob& b) { return b.cells < opts.blobMinCells; });

    // Merge near-touching blobs to a fixpoint (chips for two halves of one subject are noise).
    // Deterministic: fixed scan order, always folding the later blob into the earlier one.
    bool merged = true;
    while (merged) {
        merged = false;
        for (std::size_t i = 0; i < blobs.size() && !merged; ++i)
            for (std::size_t j = i + 1; j < blobs.size() && !merged; ++j)
                if (nearTouch(blobs[i], blobs[j], opts.mergeGapCells)) {
                    blobs[i].x0 = std::min(blobs[i].x0, blobs[j].x0);
                    blobs[i].y0 = std::min(blobs[i].y0, blobs[j].y0);
                    blobs[i].x1 = std::max(blobs[i].x1, blobs[j].x1);
                    blobs[i].y1 = std::max(blobs[i].y1, blobs[j].y1);
                    blobs[i].massFrac += blobs[j].massFrac;
                    blobs[i].cells += blobs[j].cells;
                    blobs.erase(blobs.begin() + static_cast<std::ptrdiff_t>(j));
                    merged = true;
                }
    }

    // Heaviest first; stable so equal-mass blobs keep scan order (determinism).
    std::stable_sort(blobs.begin(), blobs.end(), [](const ImportanceBlob& a,
                                                    const ImportanceBlob& b) {
        return a.massFrac > b.massFrac;
    });
    if (opts.maxRegions >= 0 && blobs.size() > static_cast<std::size_t>(opts.maxRegions))
        blobs.resize(static_cast<std::size_t>(opts.maxRegions));

    // Support expansion (the "castle spire" fix): grow each kept blob by a BOUNDED GEODESIC
    // DILATION into the support level. The strict threshold clips a faint extremity of an
    // object (a turret cap fading into sky); whatever the object connects to WITHIN REACH at
    // the support level is part of it — the crop tier must protect it and the recompose tier
    // must cut it. The reach bound is the discriminator a bbox union cannot provide: on the
    // beach photo the turret caps connect (at support level) to a cloud bank spanning half the
    // sky — a protrusion sits NEAR the strict mass, a flood extends far, so walking at most
    // `reach` cells admits the caps and stops the clouds. Still the ONE fused map — this is a
    // second read of the same W, never a second, differently-typed saliency map (the standing
    // one-map guardrail). Deterministic: FIFO BFS seeded in scan order.
    if (opts.supportThresholdFrac > 0.0 && opts.supportThresholdFrac < 1.0 && !blobs.empty()) {
        const double thr =
            static_cast<double>(peak) * opts.blobThreshold * opts.supportThresholdFrac;
        const std::uint32_t w = map.width, h = map.height;
        std::vector<std::uint32_t> dist(map.w.size());
        std::vector<std::uint32_t> queue;
        for (ImportanceBlob& b : blobs) {
            // Hand-set reach (the standing guardrail: every weight and prior in this tier is a
            // hand-set constant from photographic convention, never learned from a corpus of
            // images): half the blob's larger side, clamped to a sane band — enough for a real
            // protrusion, never a crawl across the picture.
            const std::uint32_t maxDim = std::max(b.x1 - b.x0, b.y1 - b.y0);
            const auto reach = static_cast<std::uint32_t>(
                std::clamp<long>(std::lround(0.5 * maxDim), 4, 16));
            constexpr std::uint32_t kUnseen = 0xffffffffu;
            dist.assign(dist.size(), kUnseen);
            queue.clear();
            // Seeds: every support-level cell inside the blob's current box (includes all its
            // strict cells). BFS layers outward, extending the box as it goes.
            for (std::uint32_t y = b.y0; y < b.y1; ++y)
                for (std::uint32_t x = b.x0; x < b.x1; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * w + x;
                    if (map.w[i] >= thr) {
                        dist[i] = 0;
                        queue.push_back(static_cast<std::uint32_t>(i));
                    }
                }
            for (std::size_t qi = 0; qi < queue.size(); ++qi) {
                const std::uint32_t i = queue[qi];
                const std::uint32_t d = dist[i];
                if (d >= reach)
                    continue; // admitted, but its neighbours are beyond reach
                const std::uint32_t x = i % w, y = i / w;
                const auto push = [&](std::uint32_t nx, std::uint32_t ny) {
                    const std::size_t ni = static_cast<std::size_t>(ny) * w + nx;
                    if (dist[ni] == kUnseen && map.w[ni] >= thr) {
                        dist[ni] = d + 1;
                        queue.push_back(static_cast<std::uint32_t>(ni));
                    }
                };
                if (x > 0)
                    push(x - 1, y);
                if (x + 1 < w)
                    push(x + 1, y);
                if (y > 0)
                    push(x, y - 1);
                if (y + 1 < h)
                    push(x, y + 1);
            }
            for (const std::uint32_t i : queue) { // every visited cell (dist <= reach) joins
                const std::uint32_t x = i % w, y = i / w;
                b.x0 = std::min(b.x0, x);
                b.y0 = std::min(b.y0, y);
                b.x1 = std::max(b.x1, x + 1);
                b.y1 = std::max(b.y1, y + 1);
            }
        }
    }

    // Map space -> snug doc-space rects (floor/ceil so the box never under-covers the blob).
    const double sx = static_cast<double>(map.sourceW) / map.width;
    const double sy = static_cast<double>(map.sourceH) / map.height;
    out.reserve(blobs.size() + faceRects.size());
    for (const ImportanceBlob& b : blobs) {
        const double x0 = std::floor(b.x0 * sx);
        const double y0 = std::floor(b.y0 * sy);
        const double x1 = std::min(std::ceil(b.x1 * sx), static_cast<double>(map.sourceW));
        const double y1 = std::min(std::ceil(b.y1 * sy), static_cast<double>(map.sourceH));
        out.push_back({{x0, y0, x1 - x0, y1 - y0}, b.massFrac, KeepRegion::Source::Auto});
    }

    // Dormant face hook (F1 dropped 2026-07-02): faces append as their own regions — never
    // merged into blobs, never capped. Clamped to the document; empty leftovers dropped.
    const common::Rect doc{0.0, 0.0, static_cast<double>(map.sourceW),
                           static_cast<double>(map.sourceH)};
    for (const common::Rect& f : faceRects) {
        const common::Rect c = f.intersected(doc);
        if (!c.empty())
            out.push_back({c, 0.0, KeepRegion::Source::Face});
    }
    return out;
}

} // namespace mosaic::core::retarget
