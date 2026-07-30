// S55-g follow-on material generators -- wood, marble, stone, canvas, metal -- procedural,
// CPU-first, ML-free, "data over §5": every material is a height-field recipe + albedo mapping
// over the SAME engine as paper_render.cpp. Technique lineage, every step long published
// (docs/texture-generator.md §5):
//   - solid-texture wood rings (distance-from-axis + turbulence perturbation, knots as local ring
//     systems around sparse cellular feature points) -- Peachey, "Solid Texturing of Complex
//     Surfaces" (SIGGRAPH 1985); Ebert et al., "Texturing & Modeling" (1994);
//   - marble veining (sin(k*x + turbulence) through a colour ramp) -- Perlin, "An Image
//     Synthesizer" (SIGGRAPH 1985), incl. the classic |perlin| octave-sum turbulence;
//   - cellular aggregate + F2-F1 crack network -- Worley, "A Cellular Texture Basis Function"
//     (SIGGRAPH 1996);
//   - woven warp/weft (two perpendicular sinusoid thread profiles + over/under parity) -- basic
//     trigonometry over public physical facts about plain weave;
//   - brushed-metal streaks -- anisotropically stretched fBm (the §5.1 grain-stretch precedent);
//   - spectral fibre -- sparse-convolution Gabor noise (Lagae et al. 2009), reimplemented in the
//     shared noise kit;
//   - height -> normal -- ⚠ SINGLE-PASS Sobel ONLY: no iterative smoothing of the height field
//     and no weighted multi-scale pyramid. That is a deliberate standing limit, not a shortcut
//     waiting to be upgraded (same constraint as paper_render.cpp);
//   - shading -- Oren-Nayar (1994) rough-diffuse over Lambert + optional Blinn-Phong (1977) sheen;
//     the metal "reflection" is a plain vertical albedo ramp, not an environment technique.
// No ML. No third-party engine source. ⚠ NO wavelet noise (noise.hpp's standing constraint).

#include "core/texture/material_render.hpp"

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

double mix(double a, double b, double t) noexcept {
    return a + (b - a) * t;
}

// Perlin's classic turbulence (1985): the octave sum of |perlin| -- the wood/marble domain
// perturbation. Local to the materials: the kit's fbm2 is the signed variant (§8.3 seeding
// discipline is the same -- each octave reads its own sub-seeded lattice).
double turb2(std::uint64_t seed, double x, double y, int octaves) {
    double sum = 0.0, amp = 0.5, freq = 1.0, norm = 0.0;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * std::abs(perlin2(subSeed(seed, static_cast<std::uint64_t>(o)), x * freq,
                                      y * freq));
        norm += amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return sum / norm;  // ~[0, 1)
}

// Element sub-seed tags (arbitrary, FROZEN: part of each material's golden contract).
constexpr std::uint64_t kSeedWoodWarp = 0x574F44'10;    // 'WOD'
constexpr std::uint64_t kSeedWoodDrift = 0x574F44'11;
constexpr std::uint64_t kSeedWoodKnotCell = 0x574F44'12;
constexpr std::uint64_t kSeedWoodKnotPick = 0x574F44'13;
constexpr std::uint64_t kSeedWoodFiber = 0x574F44'14;
constexpr std::uint64_t kSeedWoodTooth = 0x574F44'15;
constexpr std::uint64_t kSeedMarbleTurb = 0x4D4152'10;  // 'MAR'
constexpr std::uint64_t kSeedMarbleTurb2 = 0x4D4152'11;
constexpr std::uint64_t kSeedMarbleTooth = 0x4D4152'12;
constexpr std::uint64_t kSeedStoneCell = 0x53544F'10;   // 'STO'
constexpr std::uint64_t kSeedStoneRelief = 0x53544F'11;
constexpr std::uint64_t kSeedStoneTint = 0x53544F'12;
constexpr std::uint64_t kSeedCanvasWobU = 0x434156'10;  // 'CAV'
constexpr std::uint64_t kSeedCanvasWobV = 0x434156'11;
constexpr std::uint64_t kSeedCanvasThick = 0x434156'12;
constexpr std::uint64_t kSeedCanvasTint = 0x434156'13;
constexpr std::uint64_t kSeedCanvasFuzz = 0x434156'14;
constexpr std::uint64_t kSeedMetalStreak = 0x4D4554'10;  // 'MET'
constexpr std::uint64_t kSeedMetalMacro = 0x4D4554'11;

// The shared raked-light shade (the §5.3 step, common to every material).
struct SurfaceShade {
    double relief;             // height -> surface-slope amplitude (paper: 2.6 x roughness)
    double matte;              // Oren-Nayar sigma proxy, 0 (Lambert) .. 1 (very matte)
    double sheen;              // Blinn-Phong strength, 0..1
    double sheenExp;           // Blinn-Phong exponent (48 = paper's tight coat; low = broad satin)
    double lightAzimuthDeg;    // raking light compass (0 = from the top of the frame)
    double lightElevationDeg;  // grazing angle
};

// The §5 engine over an arbitrary material recipe. `field(px, py, height, shade)` evaluates the
// scalar height and an albedo-driver scalar at the FRAME coordinate; `albedo(shade)` maps that
// scalar to a colour (ring ramp, vein ramp, luminance factor...). Two passes exactly like
// paper_render.cpp: heights one texel wider than the window on every side so the Sobel never
// clamps, then single-pass Sobel -> Oren-Nayar (+ optional sheen). Every term is a pure function
// of the frame coordinate, so a window is a byte-exact crop (§8.2); materials are opaque
// (alpha 255 everywhere).
template <class Field, class Albedo>
common::Image shadeMaterial(std::uint32_t w, std::uint32_t h, const TextureWindow& window,
                            TextureRenderProgress* progress, const SurfaceShade& ss, Field&& field,
                            Albedo&& albedo) {
    const ResolvedWindow win = resolveWindow(window, w, h);
    if (progress != nullptr)
        progress->rowsTotal.store(static_cast<std::uint64_t>(win.h) + 2 + win.h,
                                  std::memory_order_relaxed);

    // Pass 1: height + albedo-driver, window-local buffers over absolute frame coordinates.
    const std::uint32_t hw2 = win.w + 2, hh2 = win.h + 2;
    std::vector<float> height(static_cast<std::size_t>(hw2) * hh2);
    std::vector<float> driver(static_cast<std::size_t>(hw2) * hh2);
    parallelRows(hh2, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            for (std::uint32_t x = 0; x < hw2; ++x) {
                const double px = static_cast<double>(win.x) + static_cast<double>(x) - 0.5;
                const double py = static_cast<double>(win.y) + static_cast<double>(y) - 0.5;
                double hv = 0.0, sv = 0.0;
                field(px, py, hv, sv);
                const std::size_t o = static_cast<std::size_t>(y) * hw2 + x;
                height[o] = static_cast<float>(hv);
                driver[o] = static_cast<float>(sv);
            }
            if (!progressRowTick(progress)) return;
        }
    });

    // Pass 2: single-pass Sobel normal + Oren-Nayar raked-light shade (+ optional sheen); the
    // viewer looks straight at the sheet (V = +Z), the light rakes at a low elevation -- the
    // paper_render.cpp math verbatim.
    const double el = clamp01(ss.lightElevationDeg / 90.0) * std::numbers::pi / 2.0;
    const double az = ss.lightAzimuthDeg * std::numbers::pi / 180.0;
    const double lx = std::sin(az) * std::cos(el);
    const double ly = -std::cos(az) * std::cos(el);
    const double lz = std::sin(el);
    const double relief = ss.relief;

    const double sigma = 0.7 * clamp01(ss.matte);
    const double sigma2 = sigma * sigma;
    const double onA = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
    const double onB = 0.45 * sigma2 / (sigma2 + 0.09);
    const double sheen = clamp01(ss.sheen);
    const double hxv = lx, hyv = ly, hzv = lz + 1.0;
    const double hlen = std::sqrt(hxv * hxv + hyv * hyv + hzv * hzv);
    const double Hx = hxv / hlen, Hy = hyv / hlen, Hz = hzv / hlen;

    common::Image out(win.w, win.h);
    parallelRows(win.h, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            for (std::uint32_t x = 0; x < win.w; ++x) {
                const auto hAt = [&](std::int64_t hx, std::int64_t hy) {
                    return static_cast<double>(
                        height[static_cast<std::size_t>(hy + 1) * hw2 +
                               static_cast<std::size_t>(hx + 1)]);
                };
                const auto ix = static_cast<std::int64_t>(x), iy = static_cast<std::int64_t>(y);
                const double tl = hAt(ix - 1, iy - 1), tc = hAt(ix, iy - 1), tr = hAt(ix + 1, iy - 1);
                const double ml = hAt(ix - 1, iy), mr = hAt(ix + 1, iy);
                const double bl = hAt(ix - 1, iy + 1), bc = hAt(ix, iy + 1), br = hAt(ix + 1, iy + 1);
                const double dhx = ((tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl)) / 8.0;
                const double dhy = ((bl + 2.0 * bc + br) - (tl + 2.0 * tc + tr)) / 8.0;
                double nx = -relief * dhx, ny = -relief * dhy, nz = 1.0;
                const double nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
                nx /= nlen;
                ny /= nlen;
                nz /= nlen;

                const double ndl = std::max(0.0, nx * lx + ny * ly + nz * lz);
                const double ndv = std::max(1e-4, nz);
                const double thetaI = std::acos(std::clamp(ndl, 0.0, 1.0));
                const double thetaR = std::acos(std::clamp(ndv, 0.0, 1.0));
                const double alpha = std::max(thetaI, thetaR);
                const double beta = std::min(thetaI, thetaR);
                double lpx = lx - nx * ndl, lpy = ly - ny * ndl, lpz = lz - nz * ndl;
                double vpx = -nx * ndv, vpy = -ny * ndv, vpz = 1.0 - nz * ndv;
                const double ll = std::sqrt(lpx * lpx + lpy * lpy + lpz * lpz);
                const double vl = std::sqrt(vpx * vpx + vpy * vpy + vpz * vpz);
                double cosPhi = 0.0;
                if (ll > 1e-5 && vl > 1e-5)
                    cosPhi = (lpx * vpx + lpy * vpy + lpz * vpz) / (ll * vl);
                const double diffuse =
                    ndl * (onA + onB * std::max(0.0, cosPhi) * std::sin(alpha) * std::tan(beta));

                // The mostly-ambient matte response paper established; the diffuse term carves
                // the relief and the sheen adds the coated/polished highlight.
                double shade = 0.66 + 0.5 * diffuse;
                if (sheen > 0.0) {
                    const double ndh = std::max(0.0, nx * Hx + ny * Hy + nz * Hz);
                    shade += sheen * 0.6 * std::pow(ndh, ss.sheenExp);
                }

                const ColorF alb = albedo(static_cast<double>(
                    driver[static_cast<std::size_t>(iy + 1) * hw2 +
                           static_cast<std::size_t>(ix + 1)]));
                const auto q = [&](float ch) {
                    return static_cast<std::uint8_t>(
                        std::clamp(ch * shade * 255.0 + 0.5, 0.0, 255.0));
                };
                const std::size_t dp = (static_cast<std::size_t>(y) * win.w + x) * 4;
                out.rgba[dp + 0] = q(alb.r);
                out.rgba[dp + 1] = q(alb.g);
                out.rgba[dp + 2] = q(alb.b);
                out.rgba[dp + 3] = 255;
            }
            if (!progressRowTick(progress)) return;
        }
    });
    return out;
}

ColorF mixColor(ColorF a, ColorF b, double t) {
    const auto tf = static_cast<float>(clamp01(t));
    return {a.r + (b.r - a.r) * tf, a.g + (b.g - a.g) * tf, a.b + (b.b - a.b) * tf, 1.0f};
}

ColorF scaleColor(ColorF c, double s) {
    const auto sf = static_cast<float>(std::max(0.0, s));
    return {c.r * sf, c.g * sf, c.b * sf, 1.0f};
}

}  // namespace

// ---- wood --------------------------------------------------------------------------------------

common::Image renderWood(const TextureParams& p, const WoodParams& wood, std::uint32_t w,
                         std::uint32_t h, const TextureWindow& window,
                         TextureRenderProgress* progress) {
    const double scale = std::max(1e-3, p.scale);
    const double angle = wood.grainAngleDeg * std::numbers::pi / 180.0;
    const double ca = std::cos(angle), sa = std::sin(angle);
    const double pitch = std::max(2.0, wood.ringSpacing * scale);
    const double knotCell = 7.0 * pitch;
    const double fiberFeature = std::max(0.75, 2.0 * scale);
    const double toothFeature = std::max(0.75, 1.8 * scale);
    const double waviness = clamp01(wood.waviness);
    const double knots = clamp01(wood.knots);
    const double fiberAmt = 0.4 * clamp01(wood.fiber);
    const double contrast = clamp01(wood.ringContrast);
    const std::uint64_t warpSeed = subSeed(p.seed, kSeedWoodWarp);
    const std::uint64_t driftSeed = subSeed(p.seed, kSeedWoodDrift);
    const std::uint64_t knotCellSeed = subSeed(p.seed, kSeedWoodKnotCell);
    const std::uint64_t knotPickSeed = subSeed(p.seed, kSeedWoodKnotPick);
    const std::uint64_t fiberSeed = subSeed(p.seed, kSeedWoodFiber);
    const std::uint64_t toothSeed = subSeed(p.seed, kSeedWoodTooth);
    const FbmParams driftFbm{2, 2.0, 0.5};
    const FbmParams toothFbm{3, 2.0, 0.55};

    SurfaceShade ss{2.2 * clamp01(wood.roughness), wood.matte, wood.sheen, 32.0,
                    wood.lightAzimuthDeg, wood.lightElevationDeg};
    return shadeMaterial(
        w, h, window, progress, ss,
        [&](double px, double py, double& height, double& shade) {
            // Grain frame: u runs ALONG the grain (rings sweep along it), v ACROSS it -- the
            // plank face shows the ring bands across the grain (Peachey's distance-from-axis
            // field on a board-parallel cut).
            const double u = px * ca + py * sa;
            const double v = -px * sa + py * ca;
            const double warp = turb2(warpSeed, u / (4.0 * pitch), v / (4.0 * pitch), 4);
            double r = v + waviness * 2.2 * pitch * warp +
                       0.3 * pitch * fbm2(driftSeed, u / (8.0 * pitch), v / (8.0 * pitch),
                                          driftFbm);
            // Knots: sparse cellular feature points; near one, the ring field bends into a local
            // ring system around it (Ebert et al.'s knot recipe over Worley points) and the core
            // darkens. `knots` gates how many cells actually carry one.
            double knotDark = 0.0;
            if (knots > 0.0) {
                const WorleyResult k = worley2(knotCellSeed, u / knotCell, v / knotCell);
                if (hashToUnit(subSeed(knotPickSeed, k.cellId)) < 0.85 * knots) {
                    const double d = k.f1 * knotCell;  // px distance to the knot core
                    const double wgt = 1.0 - smoothstep(0.5 * pitch, 2.6 * pitch, d);
                    r = mix(r, d * 0.85, wgt);
                    knotDark = 1.0 - smoothstep(0.2 * pitch, 0.9 * pitch, d);
                }
            }
            const double s = 0.5 + 0.5 * std::sin(2.0 * std::numbers::pi * r / pitch);
            const double late = std::pow(s, 3.5);  // the narrow dense latewood band
            const double fibre =
                fiberAmt > 0.0
                    ? gabor2(fiberSeed, px / fiberFeature, py / fiberFeature, 0.6,
                             angle + std::numbers::pi / 2.0, 1.0)
                    : 0.0;
            const double tooth =
                fbm2(toothSeed, u / (3.0 * toothFeature), v / toothFeature, toothFbm);
            height = 0.45 * late + fiberAmt * fibre + 0.18 * tooth - 0.5 * knotDark;
            shade = clamp01(late * (0.25 + 0.75 * contrast) + 0.10 * std::abs(fibre) +
                            0.8 * knotDark);
        },
        [&](double s) { return mixColor(wood.earlyColor, wood.lateColor, s); });
}

// ---- marble ------------------------------------------------------------------------------------

common::Image renderMarble(const TextureParams& p, const MarbleParams& marble, std::uint32_t w,
                           std::uint32_t h, const TextureWindow& window,
                           TextureRenderProgress* progress) {
    const double scale = std::max(1e-3, p.scale);
    const double angle = marble.veinAngleDeg * std::numbers::pi / 180.0;
    const double ca = std::cos(angle), sa = std::sin(angle);
    // The secondary vein system runs 31 degrees off the primary at a finer pitch -- real marble
    // carries more than one fracture family.
    const double angle2 = angle + 31.0 * std::numbers::pi / 180.0;
    const double ca2 = std::cos(angle2), sa2 = std::sin(angle2);
    const double pitch = std::max(4.0, marble.veinSpacing * scale);
    const double pitch2 = pitch * 0.43;
    const double toothFeature = std::max(0.75, 2.0 * scale);
    const double turbAmt = 9.0 * clamp01(marble.turbulence);
    const double contrast = clamp01(marble.contrast);
    const std::uint64_t turbSeed = subSeed(p.seed, kSeedMarbleTurb);
    const std::uint64_t turbSeed2 = subSeed(p.seed, kSeedMarbleTurb2);
    const std::uint64_t toothSeed = subSeed(p.seed, kSeedMarbleTooth);
    const FbmParams toothFbm{3, 2.0, 0.5};

    SurfaceShade ss{2.0 * clamp01(marble.roughness), marble.matte, marble.sheen, 64.0,
                    marble.lightAzimuthDeg, marble.lightElevationDeg};
    return shadeMaterial(
        w, h, window, progress, ss,
        [&](double px, double py, double& height, double& shade) {
            // Perlin 1985 marble: a sinusoid across the vein axis, phase-shifted by turbulence;
            // the vein is the sharpened band where the sinusoid crosses zero.
            const double v1 = -px * sa + py * ca;
            const double t1 = turb2(turbSeed, px / (1.5 * pitch), py / (1.5 * pitch), 5);
            const double band1 =
                std::sin(2.0 * std::numbers::pi * v1 / pitch + turbAmt * t1);
            const double vein1 = std::pow(1.0 - std::abs(band1), 5.0);
            const double v2 = -px * sa2 + py * ca2;
            const double t2 = turb2(turbSeed2, px / (1.5 * pitch2), py / (1.5 * pitch2), 4);
            const double band2 =
                std::sin(2.0 * std::numbers::pi * v2 / pitch2 + turbAmt * 0.8 * t2);
            const double vein2 = std::pow(1.0 - std::abs(band2), 6.0);
            const double veins = clamp01(vein1 + 0.5 * vein2);
            const double tooth =
                fbm2(toothSeed, px / toothFeature, py / toothFeature, toothFbm);
            height = -0.6 * veins + 0.12 * tooth;  // veins sit a hair below the polish
            shade = clamp01(veins * (0.35 + 0.65 * contrast) + 0.06 * t1);
        },
        [&](double s) { return mixColor(marble.baseColor, marble.veinColor, s); });
}

// ---- stone -------------------------------------------------------------------------------------

common::Image renderStone(const TextureParams& p, const StoneParams& stone, std::uint32_t w,
                          std::uint32_t h, const TextureWindow& window,
                          TextureRenderProgress* progress) {
    const double scale = std::max(1e-3, p.scale);
    const double cell = std::max(3.0, stone.cellSize * scale);
    const double crackDepth = clamp01(stone.crackDepth);
    const double rough = clamp01(stone.roughness);
    const double variation = clamp01(stone.variation);
    const std::uint64_t cellSeed = subSeed(p.seed, kSeedStoneCell);
    const std::uint64_t reliefSeed = subSeed(p.seed, kSeedStoneRelief);
    const std::uint64_t tintSeed = subSeed(p.seed, kSeedStoneTint);
    const FbmParams reliefFbm{4, 2.0, 0.55};

    SurfaceShade ss{2.4, stone.matte, stone.sheen, 48.0, stone.lightAzimuthDeg,
                    stone.lightElevationDeg};
    return shadeMaterial(
        w, h, window, progress, ss,
        [&](double px, double py, double& height, double& shade) {
            // Worley aggregate: F2-F1 falls to zero on the cell boundary -- that IS the crack
            // network; each cell domes up away from it and carries its own decorrelated fBm
            // relief (the discontinuity at the border hides inside the crack channel).
            const WorleyResult wr = worley2(cellSeed, px / cell, py / cell);
            const double edge = wr.f2 - wr.f1;
            const double crack = 1.0 - smoothstep(0.0, 0.22, edge);
            const double dome = smoothstep(0.0, 0.6, edge);
            const double rel = fbm2(subSeed(reliefSeed, wr.cellId), px / (0.3 * cell),
                                    py / (0.3 * cell), reliefFbm);
            height = -1.3 * crackDepth * crack + 0.55 * dome + 0.5 * rough * rel;
            // Albedo driver = a per-cell luminance factor, darkened into the cracks.
            const double cellTint =
                (hashToUnit(subSeed(tintSeed, wr.cellId)) - 0.5) * 2.0 * variation;
            shade = (1.0 + 0.35 * cellTint) * (1.0 - 0.5 * crackDepth * crack);
        },
        [&](double s) { return scaleColor(stone.baseColor, s); });
}

// ---- canvas ------------------------------------------------------------------------------------

common::Image renderCanvas(const TextureParams& p, const CanvasParams& canvas, std::uint32_t w,
                           std::uint32_t h, const TextureWindow& window,
                           TextureRenderProgress* progress) {
    const double scale = std::max(1e-3, p.scale);
    const double angle = canvas.weaveAngleDeg * std::numbers::pi / 180.0;
    const double ca = std::cos(angle), sa = std::sin(angle);
    const double P = std::max(2.0, canvas.threadPitch * scale);
    const double irr = clamp01(canvas.irregularity);
    const double depth = clamp01(canvas.weaveDepth);
    const double fuzzAmt = clamp01(canvas.fuzz);
    const double fuzzFeature = std::max(0.75, 1.5 * scale);
    const std::uint64_t wobSeedU = subSeed(p.seed, kSeedCanvasWobU);
    const std::uint64_t wobSeedV = subSeed(p.seed, kSeedCanvasWobV);
    const std::uint64_t thickSeed = subSeed(p.seed, kSeedCanvasThick);
    const std::uint64_t tintSeed = subSeed(p.seed, kSeedCanvasTint);
    const std::uint64_t fuzzSeed = subSeed(p.seed, kSeedCanvasFuzz);
    const FbmParams wobFbm{3, 2.0, 0.5};
    const FbmParams fuzzFbm{3, 2.0, 0.55};

    SurfaceShade ss{2.2, canvas.matte, canvas.sheen, 48.0, canvas.lightAzimuthDeg,
                    canvas.lightElevationDeg};
    return shadeMaterial(
        w, h, window, progress, ss,
        [&](double px, double py, double& height, double& shade) {
            // Two perpendicular thread systems (the laid/chain machinery generalized to a true
            // weave): each thread is a sinusoid ridge, the over/under checker parity decides
            // which system rides on top in each cell, and a low-frequency wobble plus per-thread
            // thickness jitter keep it cloth, not graph paper.
            const double u = px * ca + py * sa;
            const double v = -px * sa + py * ca;
            const double uw =
                u + irr * 0.35 * P * fbm2(wobSeedU, u / (8.0 * P), v / (8.0 * P), wobFbm);
            const double vw =
                v + irr * 0.35 * P * fbm2(wobSeedV, u / (8.0 * P), v / (8.0 * P), wobFbm);
            const double fu = uw / P - std::floor(uw / P);
            const double fv = vw / P - std::floor(vw / P);
            const auto iu = static_cast<std::int64_t>(std::floor(uw / P));
            const auto iv = static_cast<std::int64_t>(std::floor(vw / P));
            // u-running threads are indexed by their v row (iv), v-running by iu.
            const double thU = 1.0 - 0.3 * irr * hashToUnit(hashCoords(thickSeed, iv, 1));
            const double thV = 1.0 - 0.3 * irr * hashToUnit(hashCoords(thickSeed, iu, 2));
            const double profU = std::sin(std::numbers::pi * fv) * thU;
            const double profV = std::sin(std::numbers::pi * fu) * thV;
            const bool parity = ((iu + iv) & 1) == 0;
            const double hU = profU * (parity ? 1.0 : 0.62);
            const double hV = profV * (parity ? 0.62 : 1.0);
            const double weave = std::max(hU, hV);
            const double fuzz =
                fbm2(fuzzSeed, px / fuzzFeature, py / fuzzFeature, fuzzFbm);
            height = depth * weave + fuzzAmt * 0.25 * fuzz;
            // Albedo driver: gaps between threads read darker; the top thread carries a faint
            // per-thread tint so long threads read as continuous fibres.
            const bool topIsU = hU >= hV;
            const double threadTint =
                0.94 + 0.12 * hashToUnit(hashCoords(tintSeed, topIsU ? iv : iu, topIsU ? 1 : 2));
            shade = threadTint * (0.72 + 0.28 * weave);
        },
        [&](double s) { return scaleColor(canvas.tint, s); });
}

// ---- metal -------------------------------------------------------------------------------------

common::Image renderMetal(const TextureParams& p, const MetalParams& metal, std::uint32_t w,
                          std::uint32_t h, const TextureWindow& window,
                          TextureRenderProgress* progress) {
    const double scale = std::max(1e-3, p.scale);
    const double angle = metal.brushAngleDeg * std::numbers::pi / 180.0;
    const double ca = std::cos(angle), sa = std::sin(angle);
    const double feature = std::max(0.75, 2.2 * scale);
    constexpr double kStretch = 42.0;  // "strongly stretched": streaks run along the brush axis
    const double rough = clamp01(metal.roughness);
    const double grad = clamp01(metal.gradient);
    const std::uint64_t streakSeed = subSeed(p.seed, kSeedMetalStreak);
    const std::uint64_t macroSeed = subSeed(p.seed, kSeedMetalMacro);
    const FbmParams streakFbm{5, 2.0, 0.6};
    const FbmParams macroFbm{2, 2.0, 0.5};
    const double invH = h > 1 ? 1.0 / static_cast<double>(h - 1) : 0.0;

    SurfaceShade ss{2.0 * rough, metal.matte, metal.sheen, 24.0, metal.lightAzimuthDeg,
                    metal.lightElevationDeg};
    return shadeMaterial(
        w, h, window, progress, ss,
        [&](double px, double py, double& height, double& shade) {
            // Brushed streaks: fBm stretched hard along the brush axis (the §5.1 anisotropic
            // grain stretch, turned up), plus a coarser macro layer so long brush marks vary.
            const double u = px * ca + py * sa;
            const double v = -px * sa + py * ca;
            const double streak =
                fbm2(streakSeed, u / (feature * kStretch), v / feature, streakFbm);
            const double macro =
                fbm2(macroSeed, u / (feature * kStretch * 4.0), v / (feature * 6.0), macroFbm);
            height = rough * (0.7 * streak + 0.3 * macro);
            // Albedo driver: the vertical reflection ramp (top brighter, as a sheet reflecting
            // sky over ground reads) + a faint per-streak sparkle. A plain colour ramp -- no
            // environment machinery.
            const double ny = clamp01(py * invH);
            shade = 1.0 + grad * (0.5 - ny) + 0.08 * streak;
        },
        [&](double s) { return scaleColor(metal.tint, s); });
}

// ---- preset libraries ----------------------------------------------------------------------
// Each preset is a complete params value; the dialog sets every knob from it, then the sliders
// fine-tune (the paper §5.5 pattern). Physical-fact starting points, tuned in the render pass.

namespace {

WoodParams woodOak() {
    WoodParams v;  // the defaults ARE the oak plank
    return v;
}

WoodParams woodWalnut() {
    WoodParams v;  // dark, tight, calm rings; satin finish
    v.earlyColor = {0.42f, 0.29f, 0.19f, 1.0f};
    v.lateColor = {0.22f, 0.13f, 0.08f, 1.0f};
    v.ringSpacing = 16.0;
    v.ringContrast = 0.5;
    v.waviness = 0.3;
    v.knots = 0.1;
    v.sheen = 0.3;
    return v;
}

WoodParams woodKnottyPine() {
    WoodParams v;  // pale, wide-ringed, knot-happy
    v.earlyColor = {0.88f, 0.74f, 0.52f, 1.0f};
    v.lateColor = {0.62f, 0.44f, 0.26f, 1.0f};
    v.ringSpacing = 34.0;
    v.ringContrast = 0.75;
    v.waviness = 0.55;
    v.knots = 0.7;
    v.fiber = 0.6;
    return v;
}

WoodParams woodDriftwood() {
    WoodParams v;  // weathered grey, deep open grain, no finish
    v.earlyColor = {0.68f, 0.66f, 0.62f, 1.0f};
    v.lateColor = {0.38f, 0.37f, 0.35f, 1.0f};
    v.ringSpacing = 20.0;
    v.ringContrast = 0.8;
    v.waviness = 0.6;
    v.knots = 0.35;
    v.fiber = 0.85;
    v.roughness = 0.8;
    v.matte = 0.9;
    v.sheen = 0.0;
    return v;
}

MarbleParams marbleCarrara() {
    MarbleParams v;  // the defaults ARE Carrara: white ground, soft grey veins
    return v;
}

MarbleParams marbleNero() {
    MarbleParams v;  // black ground, bright veins
    v.baseColor = {0.10f, 0.10f, 0.12f, 1.0f};
    v.veinColor = {0.82f, 0.80f, 0.76f, 1.0f};
    v.veinSpacing = 90.0;
    v.turbulence = 0.75;
    v.contrast = 0.85;
    v.sheen = 0.7;
    return v;
}

MarbleParams marbleRosa() {
    MarbleParams v;  // warm pink ground, wine-dark veining
    v.baseColor = {0.88f, 0.78f, 0.74f, 1.0f};
    v.veinColor = {0.55f, 0.33f, 0.34f, 1.0f};
    v.veinSpacing = 52.0;
    v.turbulence = 0.55;
    v.contrast = 0.55;
    return v;
}

StoneParams stoneGranite() {
    StoneParams v;  // fine speckled aggregate, shallow joints
    v.cellSize = 9.0;
    v.crackDepth = 0.25;
    v.roughness = 0.5;
    v.variation = 0.7;
    return v;
}

StoneParams stoneConcrete() {
    StoneParams v;  // near-uniform grey, hairline cracks
    v.baseColor = {0.66f, 0.65f, 0.63f, 1.0f};
    v.cellSize = 120.0;
    v.crackDepth = 0.35;
    v.roughness = 0.35;
    v.variation = 0.12;
    return v;
}

StoneParams stoneCobbles() {
    StoneParams v;  // the defaults writ large: rounded setts with deep dark joints
    v.cellSize = 90.0;
    v.crackDepth = 0.9;
    v.roughness = 0.65;
    v.variation = 0.5;
    return v;
}

StoneParams stoneSandstone() {
    StoneParams v;  // warm, soft-edged blocks
    v.baseColor = {0.76f, 0.64f, 0.46f, 1.0f};
    v.cellSize = 60.0;
    v.crackDepth = 0.5;
    v.roughness = 0.75;
    v.variation = 0.3;
    return v;
}

CanvasParams canvasCottonDuck() {
    CanvasParams v;  // the defaults ARE cotton duck
    return v;
}

CanvasParams canvasCoarseLinen() {
    CanvasParams v;  // wide irregular weave, raw tone
    v.tint = {0.78f, 0.72f, 0.60f, 1.0f};
    v.threadPitch = 12.0;
    v.irregularity = 0.6;
    v.weaveDepth = 0.85;
    v.fuzz = 0.45;
    return v;
}

CanvasParams canvasFinePortrait() {
    CanvasParams v;  // tight smooth weave for detail work
    v.tint = {0.90f, 0.87f, 0.80f, 1.0f};
    v.threadPitch = 4.0;
    v.irregularity = 0.15;
    v.weaveDepth = 0.5;
    v.fuzz = 0.1;
    return v;
}

CanvasParams canvasPrimed() {
    CanvasParams v;  // gesso-white, weave softened under the ground
    v.tint = {0.94f, 0.93f, 0.90f, 1.0f};
    v.weaveDepth = 0.45;
    v.irregularity = 0.25;
    v.fuzz = 0.1;
    v.sheen = 0.15;
    return v;
}

MetalParams metalBrushedSteel() {
    MetalParams v;  // the defaults ARE brushed steel
    return v;
}

MetalParams metalAluminium() {
    MetalParams v;  // lighter, flatter, finer brush
    v.tint = {0.80f, 0.81f, 0.83f, 1.0f};
    v.roughness = 0.25;
    v.sheen = 0.6;
    v.gradient = 0.25;
    return v;
}

MetalParams metalBrass() {
    MetalParams v;  // warm polished yellow metal
    v.tint = {0.78f, 0.62f, 0.31f, 1.0f};
    v.roughness = 0.2;
    v.sheen = 0.9;
    v.gradient = 0.5;
    v.matte = 0.15;
    return v;
}

MetalParams metalGunmetal() {
    MetalParams v;  // dark blued steel, coarse brush
    v.tint = {0.35f, 0.37f, 0.41f, 1.0f};
    v.roughness = 0.55;
    v.sheen = 0.55;
    v.gradient = 0.3;
    v.brushAngleDeg = 90.0;
    return v;
}

const std::array<WoodPreset, 4> kWoodPresets{{
    {"Oak plank", woodOak()},
    {"Walnut", woodWalnut()},
    {"Knotty pine", woodKnottyPine()},
    {"Driftwood", woodDriftwood()},
}};

const std::array<MarblePreset, 3> kMarblePresets{{
    {"Carrara", marbleCarrara()},
    {"Nero", marbleNero()},
    {"Rosa", marbleRosa()},
}};

const std::array<StonePreset, 4> kStonePresets{{
    {"Granite", stoneGranite()},
    {"Concrete", stoneConcrete()},
    {"Cobbles", stoneCobbles()},
    {"Sandstone", stoneSandstone()},
}};

const std::array<CanvasPreset, 4> kCanvasPresets{{
    {"Cotton duck", canvasCottonDuck()},
    {"Coarse linen", canvasCoarseLinen()},
    {"Fine portrait linen", canvasFinePortrait()},
    {"Primed canvas", canvasPrimed()},
}};

const std::array<MetalPreset, 4> kMetalPresets{{
    {"Brushed steel", metalBrushedSteel()},
    {"Aluminium", metalAluminium()},
    {"Brass", metalBrass()},
    {"Gunmetal", metalGunmetal()},
}};

}  // namespace

std::size_t woodPresetCount() {
    return kWoodPresets.size();
}
const WoodPreset& woodPreset(std::size_t i) {
    return kWoodPresets[std::min(i, kWoodPresets.size() - 1)];
}

std::size_t marblePresetCount() {
    return kMarblePresets.size();
}
const MarblePreset& marblePreset(std::size_t i) {
    return kMarblePresets[std::min(i, kMarblePresets.size() - 1)];
}

std::size_t stonePresetCount() {
    return kStonePresets.size();
}
const StonePreset& stonePreset(std::size_t i) {
    return kStonePresets[std::min(i, kStonePresets.size() - 1)];
}

std::size_t canvasPresetCount() {
    return kCanvasPresets.size();
}
const CanvasPreset& canvasPreset(std::size_t i) {
    return kCanvasPresets[std::min(i, kCanvasPresets.size() - 1)];
}

std::size_t metalPresetCount() {
    return kMetalPresets.size();
}
const MetalPreset& metalPreset(std::size_t i) {
    return kMetalPresets[std::min(i, kMetalPresets.size() - 1)];
}

}  // namespace mosaic::core::texture
