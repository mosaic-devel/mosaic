// Paper / material texture generator -- procedural, CPU-first, ML-free. Built entirely on the
// public-domain noise kit (noise.hpp). Technique lineage, every step long published
// (docs/texture-generator.md §5):
//   - height-field "tooth" as surface capacity -- Curtis et al., "Computer-Generated Watercolor"
//     (SIGGRAPH 1997);
//   - spectral fibre -- sparse-convolution Gabor noise (Lagae et al., "Procedural Noise using
//     Sparse Gabor Convolution", SIGGRAPH 2009), reimplemented from the paper (noise.cpp);
//   - laid / chain / wove / felt structure -- public physical facts about mould/machine-made paper;
//   - height -> normal -- ⚠ SINGLE-PASS Sobel ONLY: no iterative smoothing of the height field
//     and no weighted multi-scale pyramid. That is a deliberate standing limit, not a shortcut
//     waiting to be upgraded;
//   - shading -- Oren-Nayar rough-diffuse (Oren & Nayar, SIGGRAPH 1994) over Lambert, plus an
//     optional Blinn-Phong (1977) coated sheen;
//   - print tooth -- ⚠ a GENERIC stochastic valley-ink dither (blue-noise-style, plain
//     void-and-cluster lineage). Deliberately never a hybrid AM/FM screening scheme;
//   - ⚠ this module generates a STATIC texture. It is not, and must not become, a
//     paint-deposition / grain-pickup engine that models a brush laying ink into the tooth.
// No ML. No third-party engine source.

#include "core/texture/paper_render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include "core/texture/noise.hpp"
#include "core/texture/parallel_rows.hpp"
#include "core/texture/render_support.hpp"

namespace mosaic::core::texture {

namespace {

using common::ColorF;

double clamp01(double v) noexcept {
    return std::clamp(v, 0.0, 1.0);
}

double smoothstep(double e0, double e1, double v) noexcept {
    const double t = clamp01((v - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}

double fract(double v) noexcept {
    return v - std::floor(v);
}

// Element sub-seed tags (arbitrary, FROZEN: part of the paper golden contract).
constexpr std::uint64_t kSeedTooth = 0x504150'10;
constexpr std::uint64_t kSeedFiber = 0x504150'11;
constexpr std::uint64_t kSeedLaidJitter = 0x504150'12;
constexpr std::uint64_t kSeedFelt = 0x504150'13;
constexpr std::uint64_t kSeedDeckle = 0x504150'14;
constexpr std::uint64_t kSeedPrint = 0x504150'15;

// The §5.1/§5.2 layered scalar height at one buffer texel. `px,py` are pixel-space coordinates;
// `ca,sa` rotate into the grain frame (u along grain, v across); the kind selects which line/felt
// structure rides the wove tooth. All contributors are pure functions of the seeds + coordinates.
struct HeightCtx {
    std::uint64_t toothSeed, fiberSeed, laidSeed, feltSeed;
    double feature, stretch, ca, sa;
    double fiberFeature, fiberFreq, fiberOmega, fiberAmt;
    PaperKind kind;
    double laidPitch, chainPitch, laidDepth;
    double feltFeature;
    FbmParams toothFbm, laidJitterFbm, feltFbm;
};

double heightAt(const HeightCtx& c, double px, double py) {
    // Grain frame: u runs ALONG the grain (fibre) axis, v ACROSS it.
    const double u = px * c.ca + py * c.sa;
    const double v = -px * c.sa + py * c.ca;

    // Base tooth: fBm micro-relief, compressed across the grain and stretched along it (§5.1
    // anisotropy) so even a wove sheet carries a faint machine direction.
    double h = fbm2(c.toothSeed, u / (c.feature * c.stretch), v / c.feature, c.toothFbm);

    // Spectral fibre: coherent Gabor streaks running along the grain (carrier ⟂ to the fibres).
    if (c.fiberAmt > 0.0) {
        const double f = gabor2(c.fiberSeed, px / c.fiberFeature, py / c.fiberFeature, c.fiberFreq,
                                c.fiberOmega, 1.0);
        h += c.fiberAmt * f;
    }

    // Laid kind: fine parallel "laid" corrugation (pitch across the grain) + sparse perpendicular
    // "chain" ridges (pitch along the grain), both wobbled by a low-frequency jitter so the ruling
    // reads hand-laid, not mechanical.
    if (c.kind == PaperKind::Laid && c.laidDepth > 0.0) {
        const double jit =
            fbm2(c.laidSeed, u / (6.0 * c.feature), v / (6.0 * c.feature), c.laidJitterFbm);
        const double laidCorr = std::sin(2.0 * std::numbers::pi * (v + 0.8 * c.laidPitch * jit) /
                                         c.laidPitch);
        const double cf = fract((u + 0.8 * c.chainPitch * jit) / c.chainPitch);
        const double cd = cf < 0.5 ? cf : 1.0 - cf;              // distance to nearest chain line
        const double chainLine = smoothstep(0.05, 0.0, cd);      // thin raised ridge, [0,1]
        h += c.laidDepth * (0.45 * laidCorr + 0.35 * (2.0 * chainLine - 1.0));
    }

    // Felt kind: the mould's felt side -- a coarse, cloudy low-frequency relief over the tooth.
    if (c.kind == PaperKind::Felt) {
        const double felt = fbm2(c.feltSeed, px / c.feltFeature, py / c.feltFeature, c.feltFbm);
        h += 0.6 * felt;
    }
    return h;
}

}  // namespace

common::Image renderPaper(const TextureParams& p, const PaperParams& paper, std::uint32_t w,
                          std::uint32_t h, const TextureWindow& window,
                          TextureRenderProgress* progress) {
    // Every height/noise term is a pure function of the FRAME coordinate and the deckle band
    // measures against the FULL frame, so the window is a byte-exact crop (§8.2).
    const ResolvedWindow win = resolveWindow(window, w, h);
    if (progress != nullptr)
        progress->rowsTotal.store(static_cast<std::uint64_t>(win.h) + 2 + win.h,
                                  std::memory_order_relaxed);
    const double scale = std::max(1e-3, p.scale);
    const double angle = paper.grainAngleDeg * std::numbers::pi / 180.0;

    HeightCtx c{};
    c.toothSeed = subSeed(p.seed, kSeedTooth);
    c.fiberSeed = subSeed(p.seed, kSeedFiber);
    c.laidSeed = subSeed(p.seed, kSeedLaidJitter);
    c.feltSeed = subSeed(p.seed, kSeedFelt);
    c.feature = std::max(0.75, 4.5 * scale);
    c.stretch = 1.0 + 3.0 * clamp01(paper.grainAnisotropy);
    c.ca = std::cos(angle);
    c.sa = std::sin(angle);
    c.fiberFeature = std::max(0.75, 2.0 * scale);
    c.fiberFreq = 0.6;                                     // cycles per fibre cell
    c.fiberOmega = angle + std::numbers::pi / 2.0;         // carrier ⟂ grain -> streaks along grain
    c.fiberAmt = 0.35 * clamp01(paper.fiber);
    c.kind = paper.kind;
    c.laidPitch = std::max(1.0, paper.laidSpacing * scale);
    c.chainPitch = std::max(2.0, paper.chainSpacing * scale);
    c.laidDepth = clamp01(paper.laidDepth);
    c.feltFeature = std::max(2.0, 12.0 * scale);
    c.toothFbm = FbmParams{5, 2.0, 0.55};
    c.laidJitterFbm = FbmParams{2, 2.0, 0.5};
    c.feltFbm = FbmParams{3, 2.2, 0.6};

    // Pass 1: the height field, one texel wider on every side of the WINDOW so the Sobel never
    // clamps at its edges (hAt(-1, ..) and hAt(winW, ..) are real evaluated samples). Buffer
    // coords are window-local; the height itself is sampled at the absolute frame coordinate.
    const std::uint32_t hw2 = win.w + 2, hh2 = win.h + 2;
    std::vector<float> height(static_cast<std::size_t>(hw2) * hh2);
    parallelRows(hh2, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            for (std::uint32_t x = 0; x < hw2; ++x) {
                // Buffer x -> frame pixel (win.x + x - 1), sampled at its centre.
                const double px = static_cast<double>(win.x) + static_cast<double>(x) - 0.5;
                const double py = static_cast<double>(win.y) + static_cast<double>(y) - 0.5;
                height[static_cast<std::size_t>(y) * hw2 + x] =
                    static_cast<float>(heightAt(c, px, py));
            }
            if (!progressRowTick(progress)) return;
        }
    });

    // Pass 2: single-pass Sobel normal + Oren-Nayar raked-light shade (+ optional sheen / deckle /
    // print tooth). Viewer looks straight at the page (V = +Z), the light rakes at a low elevation.
    const double el = clamp01(paper.lightElevationDeg / 90.0) * std::numbers::pi / 2.0;
    const double az = paper.lightAzimuthDeg * std::numbers::pi / 180.0;
    // Screen-space light: azimuth 0 = from the top of the frame, clockwise; +y is down.
    const double lx = std::sin(az) * std::cos(el);
    const double ly = -std::cos(az) * std::cos(el);
    const double lz = std::sin(el);
    const double relief = 2.6 * clamp01(paper.roughness);

    // Oren-Nayar A/B from the matte roughness (sigma up to ~0.7 rad -- a very rough diffuser).
    const double sigma = 0.7 * clamp01(paper.matte);
    const double sigma2 = sigma * sigma;
    const double onA = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
    const double onB = 0.45 * sigma2 / (sigma2 + 0.09);
    const double sheen = clamp01(paper.sheen);
    // Halfway vector for the coated sheen (L + V, V = +Z), normalised.
    const double hxv = lx, hyv = ly, hzv = lz + 1.0;
    const double hlen = std::sqrt(hxv * hxv + hyv * hyv + hzv * hzv);
    const double Hx = hxv / hlen, Hy = hyv / hlen, Hz = hzv / hlen;

    const bool deckle = paper.deckleEdge;
    const double band = std::max(1.0, paper.deckleInset * std::min(w, h));
    const double deckleFeature = std::max(3.0, band * 0.5);
    const std::uint64_t deckleSeed = subSeed(p.seed, kSeedDeckle);
    const std::uint64_t printSeed = subSeed(p.seed, kSeedPrint);
    const double printGrain = std::max(1.0, 1.5 * scale);  // Scale-relative speckle cell
    const FbmParams deckleFbm{4, 2.0, 0.5};

    common::Image out(win.w, win.h);
    parallelRows(win.h, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            // Frame coords for the absolute terms (print-grain cell, deckle distance); the Sobel
            // taps + output index stay window-local.
            const std::uint32_t fy = static_cast<std::uint32_t>(win.y) + y;
            for (std::uint32_t x = 0; x < win.w; ++x) {
                const std::uint32_t fx = static_cast<std::uint32_t>(win.x) + x;
                const auto hAt = [&](std::int64_t hx, std::int64_t hy) {
                    return static_cast<double>(
                        height[static_cast<std::size_t>(hy + 1) * hw2 +
                               static_cast<std::size_t>(hx + 1)]);
                };
                const auto ix = static_cast<std::int64_t>(x), iy = static_cast<std::int64_t>(y);
                // 3x3 Sobel slopes (divided by 8: the operator's central-difference normalisation).
                const double tl = hAt(ix - 1, iy - 1), tc = hAt(ix, iy - 1), tr = hAt(ix + 1, iy - 1);
                const double ml = hAt(ix - 1, iy), mc = hAt(ix, iy), mr = hAt(ix + 1, iy);
                const double bl = hAt(ix - 1, iy + 1), bc = hAt(ix, iy + 1), br = hAt(ix + 1, iy + 1);
                const double dhx = ((tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl)) / 8.0;
                const double dhy = ((bl + 2.0 * bc + br) - (tl + 2.0 * tc + tr)) / 8.0;
                // Normal of the height surface z = relief * h(x, y).
                double nx = -relief * dhx, ny = -relief * dhy, nz = 1.0;
                const double nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
                nx /= nlen;
                ny /= nlen;
                nz /= nlen;

                // Oren-Nayar rough-diffuse. NdotV = nz (V = +Z); the azimuth term uses the light and
                // view projected into the tangent plane.
                const double ndl = std::max(0.0, nx * lx + ny * ly + nz * lz);
                const double ndv = std::max(1e-4, nz);
                const double thetaI = std::acos(std::clamp(ndl, 0.0, 1.0));
                const double thetaR = std::acos(std::clamp(ndv, 0.0, 1.0));
                const double alpha = std::max(thetaI, thetaR);
                const double beta = std::min(thetaI, thetaR);
                // Tangent-plane azimuth difference between L and V.
                double lpx = lx - nx * ndl, lpy = ly - ny * ndl, lpz = lz - nz * ndl;
                double vpx = -nx * ndv, vpy = -ny * ndv, vpz = 1.0 - nz * ndv;
                const double ll = std::sqrt(lpx * lpx + lpy * lpy + lpz * lpz);
                const double vl = std::sqrt(vpx * vpx + vpy * vpy + vpz * vpz);
                double cosPhi = 0.0;
                if (ll > 1e-5 && vl > 1e-5)
                    cosPhi = (lpx * vpx + lpy * vpy + lpz * vpz) / (ll * vl);
                const double diffuse =
                    ndl * (onA + onB * std::max(0.0, cosPhi) * std::sin(alpha) * std::tan(beta));

                // Mostly-ambient matte response so the page reads paper-white; the diffuse term
                // carves the tooth relief.
                double shade = 0.66 + 0.5 * diffuse;
                if (sheen > 0.0) {
                    const double ndh = std::max(0.0, nx * Hx + ny * Hy + nz * Hz);
                    shade += sheen * 0.6 * std::pow(ndh, 48.0);  // soft coated-stock highlight
                }

                // Print tooth: a stochastic toner speckle that clusters in the tooth valleys. The
                // grain is a Scale-relative CELL (not per-pixel) so the speckle is resolution-
                // independent -- it reads as ink caught in the tooth, and responds to Scale.
                if (paper.printTooth && paper.printAmount > 0.0) {
                    const std::int64_t gx = static_cast<std::int64_t>(std::floor(fx / printGrain));
                    const std::int64_t gy = static_cast<std::int64_t>(std::floor(fy / printGrain));
                    const double valley = clamp01(0.5 - 0.6 * mc);
                    const double thresh = clamp01(paper.printAmount) * (0.12 + 0.88 * valley);
                    const double r = hashToUnit(hashCoords(printSeed, gx, gy));
                    if (r < thresh) shade *= (1.0 - 0.45 * clamp01(paper.printAmount));
                }

                // Deckle edge: an fBm-perturbed distance to the sheet border fades the torn fringe to
                // transparent (the §3.4 alpha carry). Default (deckle off) is fully opaque.
                double alphaOut = 1.0;
                if (deckle) {
                    const double edgeDist = std::min(
                        {static_cast<double>(fx), static_cast<double>(fy),
                         static_cast<double>(w - 1 - fx), static_cast<double>(h - 1 - fy)});
                    const double n = fbm2(deckleSeed, static_cast<double>(fx) / deckleFeature,
                                         static_cast<double>(fy) / deckleFeature, deckleFbm);
                    const double perturbed = edgeDist + clamp01(paper.deckleAmount) * band * n;
                    alphaOut = smoothstep(0.0, band * 0.5, perturbed);
                }

                const auto q = [&](float tintCh) {
                    return static_cast<std::uint8_t>(
                        std::clamp(tintCh * shade * 255.0 + 0.5, 0.0, 255.0));
                };
                const std::size_t dp = (static_cast<std::size_t>(y) * win.w + x) * 4;
                out.rgba[dp + 0] = q(paper.tint.r);
                out.rgba[dp + 1] = q(paper.tint.g);
                out.rgba[dp + 2] = q(paper.tint.b);
                out.rgba[dp + 3] =
                    static_cast<std::uint8_t>(std::clamp(alphaOut * 255.0 + 0.5, 0.0, 255.0));
            }
            if (!progressRowTick(progress)) return;
        }
    });
    return out;
}

// ---- §5.5 preset library ---------------------------------------------------------------------
// Each preset is a complete PaperParams value; the S55-f dialog sets every knob from it, then the
// sliders fine-tune. Physical-fact starting points, tuned in the render pass.

namespace {

PaperParams presetBusinessCard() {
    PaperParams p;                          // bright, smooth, faintly coated cardstock
    p.tint = {0.97f, 0.97f, 0.95f, 1.0f};
    p.kind = PaperKind::Wove;
    p.roughness = 0.28;
    p.fiber = 0.2;
    p.grainAnisotropy = 0.15;
    p.matte = 0.5;
    p.sheen = 0.25;
    return p;
}

PaperParams presetKraft() {
    PaperParams p;                          // warm brown recycled wrapping
    p.tint = {0.72f, 0.55f, 0.36f, 1.0f};
    p.kind = PaperKind::Felt;
    p.roughness = 0.7;
    p.fiber = 0.7;
    p.grainAnisotropy = 0.3;
    p.matte = 0.85;
    return p;
}

PaperParams presetLaidBond() {
    PaperParams p;                          // antique writing stock, ruled
    p.tint = {0.94f, 0.92f, 0.85f, 1.0f};
    p.kind = PaperKind::Laid;
    p.roughness = 0.45;
    p.fiber = 0.5;
    p.laidSpacing = 5.0;
    p.chainSpacing = 90.0;
    p.laidDepth = 0.7;
    p.matte = 0.7;
    return p;
}

PaperParams presetColdPressWatercolour() {
    PaperParams p;                          // heavy cotton rag, deep tooth, deckled
    p.tint = {0.95f, 0.94f, 0.90f, 1.0f};
    p.kind = PaperKind::Felt;
    p.roughness = 0.9;
    p.fiber = 0.55;
    p.grainAnisotropy = 0.2;
    p.matte = 0.9;
    p.deckleEdge = true;
    p.deckleAmount = 0.6;
    p.deckleInset = 0.07;
    return p;
}

PaperParams presetNewsprint() {
    PaperParams p;                          // greyish, rough, cheap -- print tooth on
    p.tint = {0.85f, 0.84f, 0.79f, 1.0f};
    p.kind = PaperKind::Wove;
    p.roughness = 0.55;
    p.fiber = 0.65;
    p.grainAnisotropy = 0.5;
    p.matte = 0.85;
    p.printTooth = true;
    p.printAmount = 0.4;
    return p;
}

PaperParams presetVellum() {
    PaperParams p;                          // smooth translucent tracing stock
    p.tint = {0.96f, 0.95f, 0.92f, 1.0f};
    p.kind = PaperKind::Wove;
    p.roughness = 0.18;
    p.fiber = 0.1;
    p.grainAnisotropy = 0.1;
    p.matte = 0.45;
    p.sheen = 0.15;
    return p;
}

PaperParams presetLinenCanvas() {
    PaperParams p;                          // woven linen finish -- strong crossed grain
    p.tint = {0.93f, 0.90f, 0.83f, 1.0f};
    p.kind = PaperKind::Laid;
    p.roughness = 0.62;
    p.fiber = 0.6;
    p.laidSpacing = 4.0;
    p.chainSpacing = 4.0;                   // near-equal pitches -> a woven crosshatch
    p.laidDepth = 0.8;
    p.grainAnisotropy = 0.2;
    p.matte = 0.8;
    return p;
}

const std::array<PaperPreset, 7> kPaperPresets{{
    {"Business card", presetBusinessCard()},
    {"Kraft", presetKraft()},
    {"Laid bond", presetLaidBond()},
    {"Cold-press watercolour", presetColdPressWatercolour()},
    {"Newsprint", presetNewsprint()},
    {"Vellum", presetVellum()},
    {"Linen / canvas", presetLinenCanvas()},
}};

}  // namespace

std::size_t paperPresetCount() {
    return kPaperPresets.size();
}

const PaperPreset& paperPreset(std::size_t i) {
    return kPaperPresets[std::min(i, kPaperPresets.size() - 1)];
}

}  // namespace mosaic::core::texture
