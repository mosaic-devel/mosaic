#include "render/warp.hpp"

#include <algorithm>
#include <cmath>

#include "common/thread_pool.hpp" // common::parallelFor -- the band split every pixel loop uses
#include "render/resample.hpp"    // kernelRadius / kernelWeight / bilinearPremul: THE kernel bank

namespace mosaic::render {

namespace {

// The subdivision each quality spends per patch edge. 12 is where a Catmull-Rom patch stops showing
// its own facets at 1:1; 4 is what a 60 Hz handle drag can afford on a large layer and is still
// visibly a curve rather than a chord.
constexpr int kPatchStepsFinal = 12;
constexpr int kPatchStepsDraft = 4;

// The output-extent ceiling, the same number the brush's bounded auto-grow uses
// (core::kMaxLayerCells): a deformation that would allocate more than this is refused instead of
// trying and dying. A warp can grow its own extent without limit -- drag one handle far enough and
// the bbox is a screenful of mostly-empty pixels -- so the cap is a real guard, not a formality.
constexpr double kMaxWarpCells = 268435456.0; // 1 << 28 px, as core/layer_grow.hpp

// A control point with one ring of phantoms extrapolated outside the lattice: P[-1] = 2*P[0] - P[1]
// (and the mirror at the far edge). Separable and recursive, so the four corner phantoms fall out of
// the same two rules and no case is written twice.
[[nodiscard]] common::Vec2 ctrl(const core::WarpGrid& g, int c, int r) {
    if (c < 0) return ctrl(g, 0, r) * 2.0 - ctrl(g, 1, r);
    if (c >= g.cols) return ctrl(g, g.cols - 1, r) * 2.0 - ctrl(g, g.cols - 2, r);
    if (r < 0) return ctrl(g, c, 0) * 2.0 - ctrl(g, c, 1);
    if (r >= g.rows) return ctrl(g, c, g.rows - 1) * 2.0 - ctrl(g, c, g.rows - 2);
    return g.points[static_cast<std::size_t>(r) * static_cast<std::size_t>(g.cols)
                    + static_cast<std::size_t>(c)];
}

// Uniform Catmull-Rom on the span between p1 and p2, t in [0,1]; t=0 gives p1 and t=1 gives p2.
// The same basis core/brush/stroke_path.cpp interpolates a stroke with, written for four points at a
// time because a surface needs it twice (once per axis).
[[nodiscard]] common::Vec2 crSpan(common::Vec2 p0, common::Vec2 p1, common::Vec2 p2,
                                  common::Vec2 p3, double t) {
    const double t2 = t * t;
    const double t3 = t2 * t;
    return (p1 * 2.0 + (p2 - p0) * t + (p0 * 2.0 - p1 * 5.0 + p2 * 4.0 - p3) * t2
            + (p1 * 3.0 - p0 - p2 * 3.0 + p3) * t3)
           * 0.5;
}

// Every vertex of the subdivided surface, computed ONCE for the whole lattice: dest position and
// source position side by side. Adjacent patches read the SAME entry for a shared boundary vertex,
// which is what keeps hairline seams out of the result (see the header note).
struct FineGrid {
    int nu = 0;
    int nv = 0;
    std::vector<common::Vec2> dst;
    std::vector<common::Vec2> src;

    [[nodiscard]] std::size_t at(int i, int j) const {
        return static_cast<std::size_t>(j) * static_cast<std::size_t>(nu)
               + static_cast<std::size_t>(i);
    }
};

[[nodiscard]] FineGrid buildFineGrid(const core::WarpGrid& from, const core::WarpGrid& to,
                                     int steps) {
    FineGrid f;
    f.nu = (to.cols - 1) * steps + 1;
    f.nv = (to.rows - 1) * steps + 1;
    const std::size_t n = static_cast<std::size_t>(f.nu) * static_cast<std::size_t>(f.nv);
    f.dst.resize(n);
    f.src.resize(n);
    const double inv = 1.0 / static_cast<double>(steps);
    for (int j = 0; j < f.nv; ++j) {
        const double v = static_cast<double>(j) * inv;
        for (int i = 0; i < f.nu; ++i) {
            const double u = static_cast<double>(i) * inv;
            const std::size_t k = f.at(i, j);
            f.dst[k] = warpSurfacePoint(to, u, v);
            f.src[k] = warpSurfacePoint(from, u, v);
        }
    }
    return f;
}

// One premultiplied kernel sample of `fetch` at source point (px, py). This is convolveInto's inner
// accumulation, per point instead of per affine row: the same kernelWeight taps, the same
// premultiplied accumulation in double, the same weight-sum normalisation, the same
// un-premultiply-once at the end. `rx`/`ry` are the (already capped) tap radii and
// `invSclX`/`invSclY` divide the footprint widening back out before the kernel is evaluated.
//
// It cannot simply CALL convolveInto because a warp has no single inverse affine to hand it -- the
// map varies per triangle (mesh) or per pixel (perspective). The kernels themselves are shared,
// which is the part that must never fork.
template <typename Fetch>
void sampleKernelPremul(Fetch&& fetch, double px, double py, ResampleFilter filter, double rx,
                        double ry, double invSclX, double invSclY, double out[4]) {
    const long sx0 = static_cast<long>(std::ceil(px - 0.5 - rx));
    const long sx1 = static_cast<long>(std::floor(px - 0.5 + rx));
    const long sy0 = static_cast<long>(std::ceil(py - 0.5 - ry));
    const long sy1 = static_cast<long>(std::floor(py - 0.5 + ry));
    double pr = 0, pg = 0, pb = 0, pa = 0, wsum = 0;
    for (long sy = sy0; sy <= sy1; ++sy) {
        const double wy = kernelWeight(filter, ((static_cast<double>(sy) + 0.5) - py) * invSclY);
        if (wy == 0.0) continue;
        for (long sx = sx0; sx <= sx1; ++sx) {
            const double wgt =
                wy * kernelWeight(filter, ((static_cast<double>(sx) + 0.5) - px) * invSclX);
            if (wgt == 0.0) continue;
            float c[4];
            fetch(sx, sy, c);
            const double aw = static_cast<double>(c[3]) * wgt;
            pr += static_cast<double>(c[0]) * aw; // premultiplied accumulation
            pg += static_cast<double>(c[1]) * aw;
            pb += static_cast<double>(c[2]) * aw;
            pa += aw;
            wsum += wgt;
        }
    }
    out[0] = out[1] = out[2] = out[3] = 0.0;
    if (wsum <= 0.0) return; // nothing covered -> leave transparent
    out[3] = std::clamp(pa / wsum, 0.0, 1.0);
    if (pa > 1e-8) { // un-premultiply (rgb is meaningless at ~zero coverage)
        out[0] = pr / pa;
        out[1] = pg / pa;
        out[2] = pb / pa;
    }
}

// The average of n x n bilinear sub-samples of one destination pixel, `mapSub` carrying a sub-pixel
// offset within it to a source point (and answering false where that point has no pre-image). The
// Supersample kernel's shape, shared by both engines because only the mapping differs.
template <typename Fetch, typename MapSub>
void supersamplePixel(Fetch&& fetch, int n, MapSub&& mapSub, double out[4]) {
    const double step = 1.0 / static_cast<double>(n);
    double acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            common::Vec2 s{};
            if (!mapSub((static_cast<double>(i) + 0.5) * step, (static_cast<double>(j) + 0.5) * step,
                        s))
                continue;
            double sub[4];
            bilinearPremul(fetch, s.x, s.y, sub);
            acc[0] += sub[0];
            acc[1] += sub[1];
            acc[2] += sub[2];
            acc[3] += sub[3];
        }
    }
    const double norm = 1.0 / (static_cast<double>(n) * static_cast<double>(n));
    out[0] = out[1] = out[2] = 0.0;
    out[3] = std::clamp(acc[3] * norm, 0.0, 1.0);
    if (acc[3] > 1e-8) { // un-premultiply
        out[0] = acc[0] / acc[3];
        out[1] = acc[1] / acc[3];
        out[2] = acc[2] / acc[3];
    }
}

// The supersample rate for a given local minification, capped like supersampleInto's own.
[[nodiscard]] int supersampleRate(double sclX, double sclY) noexcept {
    return std::clamp(static_cast<int>(std::ceil(std::max(sclX, sclY))) + 1, 2, 8);
}

void storePixel(common::ImageF& dst, std::uint32_t x, std::uint32_t y, const double c[4]) {
    dst.set(x, y,
            common::ColorF{static_cast<float>(c[0]), static_cast<float>(c[1]),
                           static_cast<float>(c[2]), static_cast<float>(c[3])});
}

// Resolve ResampleFilter::Auto for a warp, and apply the draft downgrade.
//
// A warp has no single affine to hand chooseAutoFilter, so Auto is bucketed the same way from the
// OVERALL area change: an identity deformation is lossless (Nearest keeps pixel art crisp), a
// reduction box-averages (Area, no ringing), an enlarge or a bend takes the sharp kernel. Draft then
// pins everything except Nearest down to Bilinear -- a drag frame is replaced within milliseconds,
// and paying Lanczos3 for it is what makes a live preview stop being live.
[[nodiscard]] ResampleFilter resolveWarpFilter(ResampleFilter user, double srcArea, double dstArea,
                                               bool identity, WarpQuality quality) noexcept {
    ResampleFilter f = user;
    if (f == ResampleFilter::Auto) {
        if (identity)
            f = ResampleFilter::Nearest;
        else if (srcArea > 0.0 && dstArea < srcArea * (1.0 - 1e-6))
            f = ResampleFilter::Area;
        else
            f = ResampleFilter::Lanczos3;
    }
    if (quality == WarpQuality::Draft && f != ResampleFilter::Nearest)
        f = ResampleFilter::Bilinear;
    return f;
}

// The straight-alpha source reader every path samples through: transparent outside the image, which
// is the layer-placement edge policy (render::EdgeMode::Transparent) -- the area outside a layer
// genuinely is nothing, so a tap there must contribute transparent or the warp grows a halo of
// invented colour along every edge it stretches.
struct SourceFetch {
    const common::ImageF* img = nullptr;

    void operator()(long sx, long sy, float out[4]) const {
        if (sx < 0 || sy < 0 || sx >= static_cast<long>(img->width)
            || sy >= static_cast<long>(img->height)) {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            return;
        }
        const std::size_t p =
            (static_cast<std::size_t>(sy) * img->width + static_cast<std::size_t>(sx)) * 4;
        out[0] = img->rgba[p];
        out[1] = img->rgba[p + 1];
        out[2] = img->rgba[p + 2];
        out[3] = img->rgba[p + 3];
    }
};

// The bbox of the deformed content, rounded out to whole pixels. False when it is empty, past the
// cell cap, or not finite (a NaN in the lattice must not become an allocation).
[[nodiscard]] bool destExtent(const std::vector<common::Vec2>& pts, int& offX, int& offY,
                              std::uint32_t& outW, std::uint32_t& outH) {
    if (pts.empty()) return false;
    double lo = pts.front().x, hi = pts.front().x, top = pts.front().y, bot = pts.front().y;
    for (const common::Vec2& p : pts) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
        lo = std::min(lo, p.x);
        hi = std::max(hi, p.x);
        top = std::min(top, p.y);
        bot = std::max(bot, p.y);
    }
    offX = static_cast<int>(std::floor(lo));
    offY = static_cast<int>(std::floor(top));
    const double dw = std::ceil(hi) - static_cast<double>(offX);
    const double dh = std::ceil(bot) - static_cast<double>(offY);
    if (dw < 1.0 || dh < 1.0 || dw * dh > kMaxWarpCells) return false;
    outW = static_cast<std::uint32_t>(dw);
    outH = static_cast<std::uint32_t>(dh);
    return true;
}

// ---- The mesh engine --------------------------------------------------------------------------

WarpResult warpMesh(const common::ImageF& src, const core::WarpGrid& from, const core::WarpGrid& to,
                    ResampleFilter filter, WarpQuality quality) {
    WarpResult out;
    const FineGrid fine = buildFineGrid(from, to, warpPatchSteps(quality));
    std::uint32_t dw = 0;
    std::uint32_t dh = 0;
    if (!destExtent(fine.dst, out.offX, out.offY, dw, dh)) return out;
    common::ImageF buf(dw, dh);
    const SourceFetch fetch{&src};
    const double kr = kernelRadius(filter);
    const double ox = static_cast<double>(out.offX);
    const double oy = static_cast<double>(out.offY);

    // The bands own disjoint DESTINATION ROWS, so two threads never write the same pixel however the
    // mesh folds over itself -- which is why the split is over the output and not over the mesh.
    // Each band walks every cell and skips the ones whose dest box misses its rows; a cell covers a
    // few pixels, so that skip is far cheaper than any spatial index would be to build.
    common::parallelFor(dh, 32, [&](std::size_t row0, std::size_t row1) {
        const double bandTop = static_cast<double>(row0);
        const double bandBot = static_cast<double>(row1);
        for (int j = 0; j + 1 < fine.nv; ++j) {
            for (int i = 0; i + 1 < fine.nu; ++i) {
                const common::Vec2 q00 = fine.dst[fine.at(i, j)];
                const common::Vec2 q10 = fine.dst[fine.at(i + 1, j)];
                const common::Vec2 q11 = fine.dst[fine.at(i + 1, j + 1)];
                const common::Vec2 q01 = fine.dst[fine.at(i, j + 1)];
                if (std::max({q00.y, q10.y, q11.y, q01.y}) - oy < bandTop
                    || std::min({q00.y, q10.y, q11.y, q01.y}) - oy >= bandBot)
                    continue;
                const common::Vec2 s00 = fine.src[fine.at(i, j)];
                const common::Vec2 s10 = fine.src[fine.at(i + 1, j)];
                const common::Vec2 s11 = fine.src[fine.at(i + 1, j + 1)];
                const common::Vec2 s01 = fine.src[fine.at(i, j + 1)];
                // Two triangles per cell, split on the 00-11 diagonal. A CELL is small enough that
                // an affine over each half follows the surface to well under a pixel -- the crease a
                // triangulation costs sits at the SUBDIVISION scale, not at the patch scale, which
                // is the whole reason the surface is subdivided before it is rasterised.
                const common::Vec2 triD[2][3] = {{q00, q10, q11}, {q00, q11, q01}};
                const common::Vec2 triS[2][3] = {{s00, s10, s11}, {s00, s11, s01}};
                for (int t = 0; t < 2; ++t) {
                    const common::Vec2* d = triD[t];
                    const common::Vec2* s = triS[t];
                    const common::Vec2 e1 = d[1] - d[0];
                    const common::Vec2 e2 = d[2] - d[0];
                    const double det = e1.x * e2.y - e1.y * e2.x;
                    if (std::abs(det) < 1e-12) continue; // a collapsed triangle covers nothing
                    const double invDet = 1.0 / det;
                    const common::Vec2 f1 = s[1] - s[0];
                    const common::Vec2 f2 = s[2] - s[0];
                    // The triangle's dest -> source map IS an affine (barycentric interpolation of
                    // three points), so its column lengths are the exact source-texels-per-dest-
                    // texel figures convolveInto derives from its own inverse. That is what makes
                    // the footprint widening correct rather than a guess, per triangle.
                    const double a00 = (f1.x * e2.y - f2.x * e1.y) * invDet;
                    const double a01 = (f2.x * e1.x - f1.x * e2.x) * invDet;
                    const double a10 = (f1.y * e2.y - f2.y * e1.y) * invDet;
                    const double a11 = (f2.y * e1.x - f1.y * e2.x) * invDet;
                    const double sclX = std::max(1.0, std::hypot(a00, a10));
                    const double sclY = std::max(1.0, std::hypot(a01, a11));
                    const double rx = std::min(kr * sclX, kMaxFootprintRadius);
                    const double ry = std::min(kr * sclY, kMaxFootprintRadius);
                    const double invSclX = 1.0 / sclX;
                    const double invSclY = 1.0 / sclY;
                    const long y0 = std::max<long>(
                        static_cast<long>(row0),
                        static_cast<long>(std::floor(std::min({d[0].y, d[1].y, d[2].y}) - oy)));
                    const long y1 = std::min<long>(
                        static_cast<long>(row1),
                        static_cast<long>(std::ceil(std::max({d[0].y, d[1].y, d[2].y}) - oy)) + 1);
                    const long x0 = std::max<long>(
                        0, static_cast<long>(std::floor(std::min({d[0].x, d[1].x, d[2].x}) - ox)));
                    const long x1 = std::min<long>(
                        static_cast<long>(dw),
                        static_cast<long>(std::ceil(std::max({d[0].x, d[1].x, d[2].x}) - ox)) + 1);
                    // Barycentric coordinates of the destination pixel centre, which double as the
                    // inside test AND as the source interpolation -- the inverse map, for free.
                    const auto bary = [&](double bx, double by, double& ba, double& bb) {
                        const double qx = bx - d[0].x;
                        const double qy = by - d[0].y;
                        ba = (qx * e2.y - qy * e2.x) * invDet;
                        bb = (e1.x * qy - e1.y * qx) * invDet;
                    };
                    for (long y = y0; y < y1; ++y) {
                        for (long x = x0; x < x1; ++x) {
                            double ba = 0.0;
                            double bb = 0.0;
                            bary(static_cast<double>(x) + 0.5 + ox,
                                 static_cast<double>(y) + 0.5 + oy, ba, bb);
                            // Inclusive on all three edges (a hair of tolerance): the two triangles
                            // sharing an edge both claim the pixels on it, so a shared edge can
                            // never fall between them and leave a one-pixel crack. The double write
                            // is harmless -- both sides agree about the source there to the last
                            // bit -- while a crack is a visible defect.
                            constexpr double kEdge = 1e-9;
                            if (ba < -kEdge || bb < -kEdge || ba + bb > 1.0 + kEdge) continue;
                            double c[4] = {0, 0, 0, 0};
                            if (filter == ResampleFilter::Nearest) {
                                float t4[4];
                                fetch(static_cast<long>(
                                          std::floor(s[0].x + f1.x * ba + f2.x * bb)),
                                      static_cast<long>(std::floor(s[0].y + f1.y * ba + f2.y * bb)),
                                      t4);
                                for (int k = 0; k < 4; ++k) c[k] = static_cast<double>(t4[k]);
                            } else if (filter == ResampleFilter::Supersample) {
                                supersamplePixel(
                                    fetch, supersampleRate(sclX, sclY),
                                    [&](double fx, double fy, common::Vec2& sp) {
                                        double sa = 0.0;
                                        double sb = 0.0;
                                        bary(static_cast<double>(x) + fx + ox,
                                             static_cast<double>(y) + fy + oy, sa, sb);
                                        sp = {s[0].x + f1.x * sa + f2.x * sb,
                                              s[0].y + f1.y * sa + f2.y * sb};
                                        return true;
                                    },
                                    c);
                            } else {
                                sampleKernelPremul(fetch, s[0].x + f1.x * ba + f2.x * bb,
                                                   s[0].y + f1.y * ba + f2.y * bb, filter, rx, ry,
                                                   invSclX, invSclY, c);
                            }
                            storePixel(buf, static_cast<std::uint32_t>(x),
                                       static_cast<std::uint32_t>(y), c);
                        }
                    }
                }
            }
        }
    });
    out.px = common::toImage8(buf);
    out.ok = true;
    return out;
}

// ---- The perspective engine -------------------------------------------------------------------

WarpResult warpPerspective(const common::ImageF& src, const core::WarpGrid& from,
                           const core::WarpGrid& to, ResampleFilter filter) {
    WarpResult out;
    const std::array<common::Vec2, 4> qFrom = warpQuadCorners(from);
    const std::array<common::Vec2, 4> qTo = warpQuadCorners(to);
    if (!convexQuad(qFrom) || !convexQuad(qTo)) return out; // a fold-back is refused, not rendered
    const std::optional<Homography> h = solveHomography(qFrom, qTo);
    if (!h) return out;
    const std::optional<Homography> hi = h->inverse();
    if (!hi) return out;
    std::uint32_t dw = 0;
    std::uint32_t dh = 0;
    if (!destExtent(std::vector<common::Vec2>(qTo.begin(), qTo.end()), out.offX, out.offY, dw, dh))
        return out;
    common::ImageF buf(dw, dh);
    const SourceFetch fetch{&src};
    const double kr = kernelRadius(filter);
    const double ox = static_cast<double>(out.offX);
    const double oy = static_cast<double>(out.offY);
    // Which side of the map's horizon the picture is on: the homogeneous divisor's sign at the
    // quad's own centre. A destination pixel whose divisor carries the other sign is behind the
    // projection and has no honest pre-image; drawing it anyway is the mirrored ghost a naive
    // homography shows beyond the vanishing line.
    const common::Vec2 dstC{(qTo[0].x + qTo[1].x + qTo[2].x + qTo[3].x) * 0.25,
                            (qTo[0].y + qTo[1].y + qTo[2].y + qTo[3].y) * 0.25};
    double wRef = 0.0;
    (void)hi->apply(dstC, &wRef);
    const double refSign = wRef >= 0.0 ? 1.0 : -1.0;

    common::parallelFor(dh, 32, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            for (std::uint32_t x = 0; x < dw; ++x) {
                const common::Vec2 p{static_cast<double>(x) + 0.5 + ox,
                                     static_cast<double>(y) + 0.5 + oy};
                double w = 0.0;
                const common::Vec2 sp = hi->apply(p, &w);
                if (w * refSign <= 1e-12) continue; // past the horizon: no pre-image
                // A destination pixel whose pre-image lands outside the SOURCE quad is not part of
                // the picture being warped; mapping it anyway would drag unrelated content into the
                // bbox's corners on a re-edit, where the source quad is a sub-region of the image
                // rather than the whole of it. The one-pixel slack keeps the true boundary pixels
                // in, so the kernel's own footprint still supplies the soft edge on a first warp.
                if (!pointInQuad(qFrom, sp, 1.0)) continue;
                // The local Jacobian of the INVERSE map here -- differentiating (u/w, v/w) -- so the
                // tap footprint widens by the true local minification even though a projective map's
                // scale changes across the quad. One affine's constant columns would be wrong at
                // both ends of a strong perspective.
                const std::array<double, 9>& a = hi->m;
                const double u = a[0] * p.x + a[1] * p.y + a[2];
                const double v = a[3] * p.x + a[4] * p.y + a[5];
                const double iw2 = 1.0 / (w * w);
                const double sclX = std::max(
                    1.0, std::hypot((a[0] * w - u * a[6]) * iw2, (a[3] * w - v * a[6]) * iw2));
                const double sclY = std::max(
                    1.0, std::hypot((a[1] * w - u * a[7]) * iw2, (a[4] * w - v * a[7]) * iw2));
                double c[4] = {0, 0, 0, 0};
                if (filter == ResampleFilter::Nearest) {
                    float t4[4];
                    fetch(static_cast<long>(std::floor(sp.x)), static_cast<long>(std::floor(sp.y)),
                          t4);
                    for (int k = 0; k < 4; ++k) c[k] = static_cast<double>(t4[k]);
                } else if (filter == ResampleFilter::Supersample) {
                    supersamplePixel(
                        fetch, supersampleRate(sclX, sclY),
                        [&](double fx, double fy, common::Vec2& s) {
                            double sw = 0.0;
                            s = hi->apply({static_cast<double>(x) + fx + ox,
                                           static_cast<double>(y) + fy + oy},
                                          &sw);
                            return sw * refSign > 1e-12;
                        },
                        c);
                } else {
                    sampleKernelPremul(fetch, sp.x, sp.y, filter,
                                       std::min(kr * sclX, kMaxFootprintRadius),
                                       std::min(kr * sclY, kMaxFootprintRadius), 1.0 / sclX,
                                       1.0 / sclY, c);
                }
                storePixel(buf, x, y, c);
            }
        }
    });
    out.px = common::toImage8(buf);
    out.ok = true;
    return out;
}

} // namespace

int warpPatchSteps(WarpQuality q) noexcept {
    return q == WarpQuality::Draft ? kPatchStepsDraft : kPatchStepsFinal;
}

std::array<common::Vec2, 4> warpQuadCorners(const core::WarpGrid& g) {
    if (g.points.size() < 4) return {};
    // A 2x2 lattice is stored row-major, so its raw order is TL, TR, BL, BR -- the swap to the
    // CYCLIC order is deliberate, and everything downstream (the convexity test, the homography
    // solve, the diagonals) reads it that way.
    return {g.points[0], g.points[1], g.points[3], g.points[2]};
}

common::Vec2 warpSurfacePoint(const core::WarpGrid& g, double u, double v) {
    if (!g.valid()) return {};
    u = std::clamp(u, 0.0, static_cast<double>(g.cols - 1));
    v = std::clamp(v, 0.0, static_cast<double>(g.rows - 1));
    const int cu = std::clamp(static_cast<int>(std::floor(u)), 0, g.cols - 2);
    const int cv = std::clamp(static_cast<int>(std::floor(v)), 0, g.rows - 2);
    const double tu = u - static_cast<double>(cu);
    const double tv = v - static_cast<double>(cv);
    common::Vec2 col[4];
    for (int k = 0; k < 4; ++k) {
        const int r = cv - 1 + k;
        col[static_cast<std::size_t>(k)] = crSpan(ctrl(g, cu - 1, r), ctrl(g, cu, r),
                                                  ctrl(g, cu + 1, r), ctrl(g, cu + 2, r), tu);
    }
    return crSpan(col[0], col[1], col[2], col[3], tv);
}

std::vector<std::vector<common::Vec2>> warpIsolines(const core::WarpGrid& g, int stepsPerEdge) {
    std::vector<std::vector<common::Vec2>> out;
    if (!g.valid()) return out;
    const int steps = std::max(1, stepsPerEdge);
    const int nu = (g.cols - 1) * steps + 1;
    const int nv = (g.rows - 1) * steps + 1;
    const double inv = 1.0 / static_cast<double>(steps);
    out.reserve(static_cast<std::size_t>(g.rows) + static_cast<std::size_t>(g.cols));
    for (int r = 0; r < g.rows; ++r) { // one line along each lattice ROW
        std::vector<common::Vec2> line;
        line.reserve(static_cast<std::size_t>(nu));
        for (int i = 0; i < nu; ++i)
            line.push_back(
                warpSurfacePoint(g, static_cast<double>(i) * inv, static_cast<double>(r)));
        out.push_back(std::move(line));
    }
    for (int c = 0; c < g.cols; ++c) { // ... and one along each lattice COLUMN
        std::vector<common::Vec2> line;
        line.reserve(static_cast<std::size_t>(nv));
        for (int j = 0; j < nv; ++j)
            line.push_back(
                warpSurfacePoint(g, static_cast<double>(c), static_cast<double>(j) * inv));
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<std::vector<common::Vec2>> warpQuadLines(const core::WarpGrid& g) {
    std::vector<std::vector<common::Vec2>> out;
    if (!g.valid() || g.cols != 2 || g.rows != 2) return out;
    const std::array<common::Vec2, 4> q = warpQuadCorners(g);
    out.push_back({q[0], q[1], q[2], q[3], q[0]});
    out.push_back({q[0], q[2]}); // the two diagonals: the perspective read, drawn faint
    out.push_back({q[1], q[3]});
    return out;
}

common::Vec2 Homography::apply(common::Vec2 p, double* outW) const {
    const double w = m[6] * p.x + m[7] * p.y + m[8];
    if (outW != nullptr) *outW = w;
    if (w == 0.0) return {};
    const double iw = 1.0 / w;
    return {(m[0] * p.x + m[1] * p.y + m[2]) * iw, (m[3] * p.x + m[4] * p.y + m[5]) * iw};
}

std::optional<Homography> Homography::inverse() const {
    // The 3x3 adjugate over the determinant. Re-normalising m[8] to 1 afterwards is deliberately
    // NOT done: apply() divides by w, so any non-zero scale of the matrix is the same map, and
    // normalising would fail on the perfectly legal case where that entry comes out near zero.
    const double c00 = m[4] * m[8] - m[5] * m[7];
    const double c01 = m[5] * m[6] - m[3] * m[8];
    const double c02 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (std::abs(det) < 1e-15) return std::nullopt;
    const double id = 1.0 / det;
    Homography r;
    r.m[0] = c00 * id;
    r.m[1] = (m[2] * m[7] - m[1] * m[8]) * id;
    r.m[2] = (m[1] * m[5] - m[2] * m[4]) * id;
    r.m[3] = c01 * id;
    r.m[4] = (m[0] * m[8] - m[2] * m[6]) * id;
    r.m[5] = (m[2] * m[3] - m[0] * m[5]) * id;
    r.m[6] = c02 * id;
    r.m[7] = (m[1] * m[6] - m[0] * m[7]) * id;
    r.m[8] = (m[0] * m[4] - m[1] * m[3]) * id;
    return r;
}

std::optional<Homography> solveHomography(const std::array<common::Vec2, 4>& from,
                                          const std::array<common::Vec2, 4>& to) {
    // Each correspondence (x,y) -> (X,Y) gives two linear equations in the eight free coefficients
    // (m[8] is fixed at 1):
    //     m0 x + m1 y + m2                 - m6 x X - m7 y X = X
    //                     m3 x + m4 y + m5 - m6 x Y - m7 y Y = Y
    // Eight equations, eight unknowns; solved directly rather than by SVD because with exactly four
    // points the system is square -- there is no over-determination to least-squares away.
    double a[8][9]{};
    for (std::size_t i = 0; i < 4; ++i) {
        const double x = from[i].x;
        const double y = from[i].y;
        const double X = to[i].x;
        const double Y = to[i].y;
        double* r0 = a[2 * i];
        double* r1 = a[2 * i + 1];
        r0[0] = x;
        r0[1] = y;
        r0[2] = 1.0;
        r0[6] = -x * X;
        r0[7] = -y * X;
        r0[8] = X;
        r1[3] = x;
        r1[4] = y;
        r1[5] = 1.0;
        r1[6] = -x * Y;
        r1[7] = -y * Y;
        r1[8] = Y;
    }
    // Gaussian elimination with PARTIAL PIVOTING, which is not decoration here: the rows above mix
    // plain coordinates with coordinate PRODUCTS, so on a large canvas the columns differ by four
    // orders of magnitude and an unpivoted elimination loses the small ones.
    for (int col = 0; col < 8; ++col) {
        int piv = col;
        for (int r = col + 1; r < 8; ++r)
            if (std::abs(a[r][col]) > std::abs(a[piv][col])) piv = r;
        if (std::abs(a[piv][col]) < 1e-12) return std::nullopt; // singular: refuse
        if (piv != col)
            for (int k = col; k < 9; ++k) std::swap(a[col][k], a[piv][k]);
        const double inv = 1.0 / a[col][col];
        for (int r = 0; r < 8; ++r) {
            if (r == col) continue;
            const double f = a[r][col] * inv;
            if (f == 0.0) continue;
            for (int k = col; k < 9; ++k) a[r][k] -= f * a[col][k];
        }
    }
    Homography h;
    for (std::size_t i = 0; i < 8; ++i) h.m[i] = a[i][8] / a[i][i];
    h.m[8] = 1.0;
    return h;
}

bool convexQuad(const std::array<common::Vec2, 4>& q) {
    double sign = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
        const common::Vec2 a = q[i];
        const common::Vec2 b = q[(i + 1) % 4];
        const common::Vec2 c = q[(i + 2) % 4];
        const double cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
        if (std::abs(cross) < 1e-9) return false; // a collinear corner: no area, no map
        const double s = cross > 0.0 ? 1.0 : -1.0;
        if (sign == 0.0)
            sign = s;
        else if (s != sign)
            return false; // a reflex corner => the quad crosses itself
    }
    return true;
}

bool pointInQuad(const std::array<common::Vec2, 4>& q, common::Vec2 p, double slack) {
    // Signed distance to each edge, with `slack` px of outward tolerance. The quad is assumed convex
    // and in cyclic order (every caller checks that first), so "inside" is one consistent sign --
    // counted rather than latched, because a point sitting ON an edge has no sign to latch.
    int neg = 0;
    int pos = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const common::Vec2 a = q[i];
        const common::Vec2 e = q[(i + 1) % 4] - a;
        const double len = e.length();
        if (len < 1e-12) return false;
        const double d = ((p.x - a.x) * e.y - (p.y - a.y) * e.x) / len;
        if (d > slack) ++pos;
        if (d < -slack) ++neg;
    }
    return neg == 0 || pos == 0;
}

WarpResult warpImage(const common::Image& src, const core::WarpGrid& from, const core::WarpGrid& to,
                     ResampleFilter filter, WarpQuality quality) {
    WarpResult out;
    if (src.empty() || !from.valid() || !to.valid()) return out;
    if (from.kind != to.kind || from.cols != to.cols || from.rows != to.rows) return out;
    // Auto's buckets need the overall area change; the two lattices' bounding boxes stand in for it
    // (an exact deformed area would cost a second surface evaluation to learn something a kernel
    // choice cannot use that precisely).
    const auto latticeArea = [](const core::WarpGrid& g) {
        double lo = g.points[0].x, hi = g.points[0].x, top = g.points[0].y, bot = g.points[0].y;
        for (const common::Vec2& p : g.points) {
            lo = std::min(lo, p.x);
            hi = std::max(hi, p.x);
            top = std::min(top, p.y);
            bot = std::max(bot, p.y);
        }
        return (hi - lo) * (bot - top);
    };
    const ResampleFilter f = resolveWarpFilter(filter, latticeArea(from), latticeArea(to),
                                               from.points == to.points, quality);
    const common::ImageF srcF = common::toFloat(src);
    return to.kind == core::WarpKind::Perspective ? warpPerspective(srcF, from, to, f)
                                                 : warpMesh(srcF, from, to, f, quality);
}

WarpResult warpImage(const common::Image& src, const core::WarpGrid& to, ResampleFilter filter,
                     WarpQuality quality) {
    return warpImage(src, core::identityLike(to), to, filter, quality);
}

} // namespace mosaic::render
