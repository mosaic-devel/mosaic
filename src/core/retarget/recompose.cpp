// Smart Recompose core — see recompose.hpp for the pipeline, the guardrails and the lineage.

#include "core/retarget/recompose.hpp"

#include "core/retarget/smart_crop.hpp" // background window choice (same single-map search)

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mosaic::core::retarget {
namespace {

// A pairwise separation constraint: region `a` must precede region `b` along `axis` (0 = x,
// 1 = y) with at least `gap` between their facing edges. Derived from the SOURCE arrangement:
// the axis that separated two regions in the original picture is the axis that keeps them apart
// in the output ("maintain relative spatial relationships", Setlur 2005).
struct Separation {
    std::size_t a, b;
    int axis;
    double gap;
};

double axisGap(double a0, double a1, double b0, double b1) {
    if (b0 > a1)
        return b0 - a1;
    if (a0 > b1)
        return a0 - b1;
    return 0.0;
}

} // namespace

std::vector<common::Rect> solvePlacements(const std::vector<common::Rect>& rects, double srcW,
                                          double srcH, double targetW, double targetH,
                                          double minGap, int maxSweeps) {
    std::vector<common::Rect> out;
    if (rects.empty() || srcW <= 0.0 || srcH <= 0.0 || targetW <= 0.0 || targetH <= 0.0)
        return out;
    // Rigid: a region larger than the frame can never fit.
    for (const common::Rect& r : rects)
        if (r.w > targetW || r.h > targetH)
            return out;

    // Seed: proportional centers, sizes unchanged.
    out.reserve(rects.size());
    for (const common::Rect& r : rects) {
        const common::Vec2 c = r.center();
        out.push_back({c.x / srcW * targetW - r.w * 0.5, c.y / srcH * targetH - r.h * 0.5, r.w,
                       r.h});
    }

    // Separation constraints from the source arrangement: for each disjoint pair, the axis with
    // the larger NORMALIZED source gap separates them (ties -> x, deterministic); overlapping
    // source rects impose none (they travel together).
    std::vector<Separation> seps;
    for (std::size_t i = 0; i < rects.size(); ++i)
        for (std::size_t j = i + 1; j < rects.size(); ++j) {
            const double gx = axisGap(rects[i].x, rects[i].right(), rects[j].x, rects[j].right());
            const double gy =
                axisGap(rects[i].y, rects[i].bottom(), rects[j].y, rects[j].bottom());
            if (gx <= 0.0 && gy <= 0.0)
                continue;
            const int axis = (gx / srcW >= gy / srcH) ? 0 : 1;
            const bool iFirst = axis == 0 ? rects[i].x <= rects[j].x : rects[i].y <= rects[j].y;
            seps.push_back({iFirst ? i : j, iFirst ? j : i, axis, minGap});
        }

    // Symmetric constraint projection (Gauss-Seidel style), fixed order -> deterministic. Each
    // sweep: clamp every rect into the frame, then push each violated pair apart evenly.
    const auto pos = [&](std::size_t k, int axis) -> double& {
        return axis == 0 ? out[k].x : out[k].y;
    };
    const auto len = [&](std::size_t k, int axis) {
        return axis == 0 ? out[k].w : out[k].h;
    };
    bool satisfied = false;
    for (int sweep = 0; sweep < maxSweeps && !satisfied; ++sweep) {
        for (common::Rect& r : out) {
            r.x = std::clamp(r.x, 0.0, targetW - r.w);
            r.y = std::clamp(r.y, 0.0, targetH - r.h);
        }
        satisfied = true;
        for (const Separation& s : seps) {
            const double need = pos(s.a, s.axis) + len(s.a, s.axis) + s.gap;
            const double have = pos(s.b, s.axis);
            if (have + 1e-9 < need) {
                const double push = (need - have) * 0.5;
                pos(s.a, s.axis) -= push;
                pos(s.b, s.axis) += push;
                satisfied = false;
            }
        }
    }
    if (!satisfied) {
        // One last clamp + verify: the projection may have converged on the final sweep.
        for (common::Rect& r : out) {
            r.x = std::clamp(r.x, 0.0, targetW - r.w);
            r.y = std::clamp(r.y, 0.0, targetH - r.h);
        }
        for (const Separation& s : seps)
            if (pos(s.b, s.axis) + 1e-6 < pos(s.a, s.axis) + len(s.a, s.axis) + s.gap)
                return {}; // rigid placement infeasible at this target size
    }
    return out;
}

namespace {

// Integer padded cut box for a region: the snug rect grown by marginFrac of its own size per
// side, clamped to the document. Also reports the achieved per-side pad (clamping can shrink
// it), which drives the feather ramp.
struct CutBox {
    long x0, y0, x1, y1;         // half-open, doc space
    double padL, padT, padR, padB; // achieved pad per side (>= 0)
};

CutBox cutBoxFor(const common::Rect& snug, double marginFrac, std::uint32_t docW,
                 std::uint32_t docH) {
    const double mx = snug.w * marginFrac;
    const double my = snug.h * marginFrac;
    CutBox c{};
    c.x0 = std::max(0L, static_cast<long>(std::floor(snug.x - mx)));
    c.y0 = std::max(0L, static_cast<long>(std::floor(snug.y - my)));
    c.x1 = std::min(static_cast<long>(docW), static_cast<long>(std::ceil(snug.right() + mx)));
    c.y1 = std::min(static_cast<long>(docH), static_cast<long>(std::ceil(snug.bottom() + my)));
    c.padL = snug.x - c.x0;
    c.padT = snug.y - c.y0;
    c.padR = c.x1 - snug.right();
    c.padB = c.y1 - snug.bottom();
    return c;
}

// The feather ramp at piece-local (rx, ry): 1 inside the snug core, falling to 0 at the padded
// edge (a zero pad side stays 1 — the content ran to the document edge there).
double featherAlphaAt(const RecomposePiece& piece, std::uint32_t rx, std::uint32_t ry) {
    const double pw = static_cast<double>(piece.image.width);
    const double ph = static_cast<double>(piece.image.height);
    const double dl = rx + 0.5, dt = ry + 0.5;
    const double dr = pw - rx - 0.5, db = ph - ry - 0.5;
    double a = 1.0;
    if (piece.padL > 0.0)
        a = std::min(a, dl / piece.padL);
    if (piece.padT > 0.0)
        a = std::min(a, dt / piece.padT);
    if (piece.padR > 0.0)
        a = std::min(a, dr / piece.padR);
    if (piece.padB > 0.0)
        a = std::min(a, db / piece.padB);
    return std::clamp(a, 0.0, 1.0);
}

// Composite a cut piece over `canvas` with its top-left at (ox, oy), alpha-feathered across the
// pad band so the loose cut melts into the healed background: full opacity inside the snug core,
// ramping to 0 at the padded edge. The piece pixels themselves are never resampled — rigidity is
// the point. (The alpha mix alone leaves the band CONTENT as source surroundings, which reads as
// a pasted square wherever the destination background differs — blendPieceBand below re-solves
// the band when seam blending is on.)
void featherBlit(common::Image& canvas, const RecomposePiece& piece, long ox, long oy) {
    const common::Image& region = piece.image;
    for (std::uint32_t ry = 0; ry < region.height; ++ry) {
        const long cy = oy + static_cast<long>(ry);
        if (cy < 0 || cy >= static_cast<long>(canvas.height))
            continue;
        for (std::uint32_t rx = 0; rx < region.width; ++rx) {
            const long cx = ox + static_cast<long>(rx);
            if (cx < 0 || cx >= static_cast<long>(canvas.width))
                continue;
            double a = featherAlphaAt(piece, rx, ry);
            const std::size_t rp = (static_cast<std::size_t>(ry) * region.width + rx) * 4;
            a *= region.rgba[rp + 3] / 255.0; // respect the source pixel's own alpha
            if (a <= 0.0)
                continue;
            const std::size_t cp =
                (static_cast<std::size_t>(cy) * canvas.width + static_cast<std::size_t>(cx)) * 4;
            for (int ch = 0; ch < 3; ++ch)
                canvas.rgba[cp + ch] = static_cast<std::uint8_t>(std::lround(
                    canvas.rgba[cp + ch] + (region.rgba[rp + ch] - canvas.rgba[cp + ch]) * a));
            canvas.rgba[cp + 3] = static_cast<std::uint8_t>(
                std::max<double>(canvas.rgba[cp + 3], std::lround(a * 255.0)));
        }
    }
}

// Residual-seam blend (plan §2 step 5, wired 2026-07-02 after the user's "pasted square"
// report): re-solve the pad band's COLOURS in the gradient domain. The snug core stays hard
// (rigid — never touched); the band pixels become a Poisson solve with Dirichlet boundaries at
// the core edge and at the surrounding background, guided by a feather-weighted MIX of the
// piece's own gradients (near the core: carries the subject's fringe and immediate context) and
// the destination background's gradients (near the outer edge: the band bends into what is
// actually there now). This removes the pasted-square sheen — the band's low frequencies adapt
// to the destination — while structure fades across the band instead of stopping at a wall.
//
// Lineage: plain Poisson editing with mixed guidance (Pérez, Gangnet & Blake, SIGGRAPH 2003),
// solved by fixed-sweep red-black Gauss-Seidel — and deliberately NOT by a quadtree-accelerated
// solver, which this file does not adopt however tempting the speedup. Deterministic: fixed scan
// order, fixed sweep count.
void blendPieceBand(common::Image& canvas, const common::Image& bg, const RecomposePiece& piece,
                    long ox, long oy) {
    const common::Image& region = piece.image;
    const long W = static_cast<long>(canvas.width);
    const long H = static_cast<long>(canvas.height);
    const long coreX0 = ox + static_cast<long>(std::lround(piece.padL));
    const long coreY0 = oy + static_cast<long>(std::lround(piece.padT));
    const long coreX1 = ox + static_cast<long>(region.width) -
                        static_cast<long>(std::lround(piece.padR));
    const long coreY1 = oy + static_cast<long>(region.height) -
                        static_cast<long>(std::lround(piece.padB));
    // Collect the band: inside the piece rect and canvas, outside the hard core.
    struct BandPx {
        int x, y;
    };
    std::vector<BandPx> band;
    std::vector<int> idxOf(static_cast<std::size_t>(W) * static_cast<std::size_t>(H), -1);
    for (long cy = std::max(0L, oy); cy < std::min(H, oy + static_cast<long>(region.height));
         ++cy) {
        for (long cx = std::max(0L, ox); cx < std::min(W, ox + static_cast<long>(region.width));
             ++cx) {
            if (cx >= coreX0 && cx < coreX1 && cy >= coreY0 && cy < coreY1)
                continue; // the rigid core is untouchable
            idxOf[static_cast<std::size_t>(cy) * static_cast<std::size_t>(W) + cx] =
                static_cast<int>(band.size());
            band.push_back({static_cast<int>(cx), static_cast<int>(cy)});
        }
    }
    if (band.empty())
        return;
    // Working copies in float; u initialized from the feathered composite (a good first guess,
    // so the fixed sweep budget converges comfortably across a thin band).
    std::vector<float> u(band.size() * 3);
    for (std::size_t i = 0; i < band.size(); ++i) {
        const std::size_t cp =
            (static_cast<std::size_t>(band[i].y) * static_cast<std::size_t>(W) + band[i].x) * 4;
        for (int ch = 0; ch < 3; ++ch)
            u[i * 3 + ch] = canvas.rgba[cp + ch];
    }
    const auto pieceAt = [&](long cx, long cy, int ch) -> double {
        const long rx = cx - ox, ry = cy - oy;
        return region.rgba[(static_cast<std::size_t>(ry) * region.width +
                            static_cast<std::size_t>(rx)) *
                               4 +
                           static_cast<std::size_t>(ch)];
    };
    const auto insidePiece = [&](long cx, long cy) {
        return cx >= ox && cy >= oy && cx < ox + static_cast<long>(region.width) &&
               cy < oy + static_cast<long>(region.height);
    };
    const auto bgAt = [&](long cx, long cy, int ch) -> double {
        return bg.rgba[(static_cast<std::size_t>(cy) * bg.width + static_cast<std::size_t>(cx)) *
                           4 +
                       static_cast<std::size_t>(ch)];
    };
    constexpr int kBandSweeps = 150; // fixed budget: thin band + warm start converge well within
    const int neigh[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int sweep = 0; sweep < kBandSweeps; ++sweep) {
        for (int color = 0; color < 2; ++color) { // red-black: parallel-safe, deterministic
            for (std::size_t i = 0; i < band.size(); ++i) {
                const long x = band[i].x, y = band[i].y;
                if (((x + y) & 1L) != color)
                    continue;
                const double ap = insidePiece(x, y)
                                      ? featherAlphaAt(piece, static_cast<std::uint32_t>(x - ox),
                                                       static_cast<std::uint32_t>(y - oy))
                                      : 0.0;
                double sum[3] = {0, 0, 0};
                int n = 0;
                for (const auto& d : neigh) {
                    const long nx = x + d[0], ny = y + d[1];
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H)
                        continue;
                    ++n;
                    const int ni =
                        idxOf[static_cast<std::size_t>(ny) * static_cast<std::size_t>(W) + nx];
                    const bool nBand = ni >= 0;
                    const bool nPiece = insidePiece(nx, ny);
                    for (int ch = 0; ch < 3; ++ch) {
                        // Neighbour value: unknown (band) or Dirichlet (core / background,
                        // whatever the canvas holds there right now).
                        const double fv =
                            nBand ? u[static_cast<std::size_t>(ni) * 3 +
                                      static_cast<std::size_t>(ch)]
                                  : canvas.rgba[(static_cast<std::size_t>(ny) *
                                                     static_cast<std::size_t>(W) +
                                                 nx) *
                                                    4 +
                                                static_cast<std::size_t>(ch)];
                        // Mixed guidance: piece gradient near the core, background gradient
                        // near the outer edge (weighted by the feather ramp).
                        double g = 0.0;
                        if (ap > 0.0 && nPiece)
                            g += ap * (pieceAt(x, y, ch) - pieceAt(nx, ny, ch));
                        if (ap < 1.0)
                            g += (1.0 - ap) * (bgAt(x, y, ch) - bgAt(nx, ny, ch));
                        sum[static_cast<std::size_t>(ch)] += fv + g;
                    }
                }
                if (n > 0)
                    for (int ch = 0; ch < 3; ++ch)
                        u[i * 3 + static_cast<std::size_t>(ch)] = static_cast<float>(
                            std::clamp(sum[static_cast<std::size_t>(ch)] / n, 0.0, 255.0));
            }
        }
    }
    for (std::size_t i = 0; i < band.size(); ++i) {
        const std::size_t cp =
            (static_cast<std::size_t>(band[i].y) * static_cast<std::size_t>(W) + band[i].x) * 4;
        for (int ch = 0; ch < 3; ++ch)
            canvas.rgba[cp + ch] =
                static_cast<std::uint8_t>(std::lround(u[i * 3 + static_cast<std::size_t>(ch)]));
    }
}

} // namespace

RecomposeStaged prepareRecompose(const common::Image& src, double targetAspect,
                                 const std::vector<KeepRegion>& regions, const FillFn& fill,
                                 const RecomposeOptions& opts) {
    RecomposeStaged res;
    if (src.empty() || targetAspect <= 0.0 || !fill) {
        res.detail = "invalid input";
        return res;
    }
    if (regions.empty()) {
        res.detail = "no keep regions";
        return res;
    }
    // Target dims: the max-fit box for the aspect, like the crop tier (never upsampled).
    const double maxW =
        std::min(static_cast<double>(src.width), static_cast<double>(src.height) * targetAspect);
    const auto targetW = static_cast<std::uint32_t>(std::max(1L, std::lround(maxW)));
    const auto targetH =
        static_cast<std::uint32_t>(std::max(1L, std::lround(maxW / targetAspect)));

    // 1. Placement first (cheap) so an infeasible ask fails before any pixel work.
    std::vector<common::Rect> snug;
    snug.reserve(regions.size());
    for (const KeepRegion& r : regions)
        snug.push_back(r.rect);
    const double gap = opts.minGapFrac * std::min(targetW, targetH);
    const std::vector<common::Rect> placed =
        solvePlacements(snug, src.width, src.height, targetW, targetH, gap, opts.solverMaxSweeps);
    if (placed.empty()) {
        res.detail = "the marked regions do not fit at this size";
        return res;
    }

    // 2. Cut the padded regions, then heal the holes in a copy of the source.
    std::vector<common::Rect> holes;
    res.pieces.reserve(regions.size());
    holes.reserve(regions.size());
    for (const KeepRegion& r : regions) {
        const CutBox c = cutBoxFor(r.rect, opts.cutMarginFrac, src.width, src.height);
        res.pieces.push_back({common::copyRegion(src, c.x0, c.y0,
                                                 static_cast<std::uint32_t>(c.x1 - c.x0),
                                                 static_cast<std::uint32_t>(c.y1 - c.y0)),
                              c.padL, c.padT, c.padR, c.padB});
        holes.push_back({static_cast<double>(c.x0), static_cast<double>(c.y0),
                         static_cast<double>(c.x1 - c.x0), static_cast<double>(c.y1 - c.y0)});
    }
    common::Image bg = src;
    if (!fill(bg, holes)) {
        res.pieces.clear();
        res.detail = "hole fill failed";
        return res;
    }

    // 3. Crop the healed background to the target: the same single-map window search as the
    // crop tier, locked to the max fit (scaleSteps 0 -> the window IS targetW x targetH). The
    // healed holes are EXCLUDED from the window's importance (an input edit, like a toggled-off
    // chip): synthesized fill should never attract the frame — the window prefers showing
    // original pixels, which also keeps heal seams out of the output where geometry allows.
    SmartCropOptions winOpts;
    winOpts.scaleSteps = 0;
    winOpts.excludeRects = holes;
    const common::Rect win = chooseCropWindow(bg, targetAspect, winOpts);
    const long wx = std::clamp(static_cast<long>(std::lround(win.x)), 0L,
                               static_cast<long>(src.width) - static_cast<long>(targetW));
    const long wy = std::clamp(static_cast<long>(std::lround(win.y)), 0L,
                               static_cast<long>(src.height) - static_cast<long>(targetH));
    res.background = common::copyRegion(bg, wx, wy, targetW, targetH);
    res.placed = placed;
    res.targetW = targetW;
    res.targetH = targetH;
    res.ok = true;
    return res;
}

common::Image assembleRecompose(const RecomposeStaged& staged, bool blendSeams) {
    if (!staged.ok)
        return {};
    // 4. Composite the pieces at their (possibly nudged) placements — feathered, never
    // resampled — over a fresh copy of the healed background; with blendSeams, each piece's
    // pad band is then re-solved in the gradient domain (plan §2 step 5) so nothing reads as
    // a pasted square. The live nudge passes false (feather-only is drag-rate cheap) and the
    // final preview/apply pass true.
    common::Image out = staged.background;
    for (std::size_t i = 0; i < staged.pieces.size() && i < staged.placed.size(); ++i) {
        const RecomposePiece& p = staged.pieces[i];
        // The piece's top-left = snug placement minus the achieved pad.
        const long ox = static_cast<long>(std::lround(staged.placed[i].x - p.padL));
        const long oy = static_cast<long>(std::lround(staged.placed[i].y - p.padT));
        featherBlit(out, p, ox, oy);
        if (blendSeams)
            blendPieceBand(out, staged.background, p, ox, oy);
    }
    return out;
}

RecomposeResult recompose(const common::Image& src, double targetAspect,
                          const std::vector<KeepRegion>& regions, const FillFn& fill,
                          const RecomposeOptions& opts) {
    RecomposeResult res;
    RecomposeStaged staged = prepareRecompose(src, targetAspect, regions, fill, opts);
    if (!staged.ok) {
        res.detail = std::move(staged.detail);
        return res;
    }
    res.image = assembleRecompose(staged);
    res.placements.reserve(staged.placed.size());
    for (std::size_t i = 0; i < staged.placed.size(); ++i)
        res.placements.push_back({i, staged.placed[i]});
    res.ok = true;
    return res;
}

} // namespace mosaic::core::retarget
