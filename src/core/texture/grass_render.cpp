// Grass texture generator -- distance-graded hybrid (procedural turf base + depth-culled blade
// instancing). Technique lineage: Reeves & Blau, "Approximate/Probabilistic ... Structured Particle
// Systems" (SIGGRAPH 1985, still-image grass); Perbet & Cani (I3D 2001) + Boulanger et al. (IEEE CG&A
// 2009) blades-near/texture-far LOD; Jahrmann & Wimmer (I3D 2017) + Sucker Punch GDC 2021 (Bezier
// blades); Kajiya & Kay (SIGGRAPH 1989) / Lengyel et al. (I3D 2001) tangent-fiber lighting; wrap/
// thickness translucency (Half-Lambert; GPU Gems 3); obscurance AO (Zhukov 1998); single-class
// Poisson-disk (Cook 1986 / Bridson 2007) + Voronoi/Worley clumping; back-to-front Porter-Duff.
// Noise: classic Perlin (1985) + Worley (1996). No ML.
// ⚠ THREE STANDING CONSTRUCTION CONSTRAINTS -- deliberate, do not "improve" away: blade placement
// is SINGLE-CLASS Poisson-disk (never a multi-class / multi-species sampler); blades composite
// back-to-front with PLAIN painter's alpha (never a dedicated hair/fur compositing scheme); and
// nothing here drives generation from a reference image with a validation loop -- the parameters
// and the seed are the only input.
// Built entirely on the public-domain noise kit (noise.hpp); pure function of (params, w, h) per §8.3.

#include "core/texture/grass_render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "core/texture/noise.hpp"
#include "core/texture/parallel_rows.hpp"
#include "core/texture/render_support.hpp"
#include "core/texture/sky_camera.hpp"

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

double mixd(double a, double b, double t) noexcept {
    return a + (b - a) * t;
}

ColorF mixColor(ColorF a, ColorF b, double t) noexcept {
    const auto f = static_cast<float>(t);
    return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f,
            a.a + (b.a - a.a) * f};
}

// Element sub-seed tags (arbitrary, FROZEN: part of the grass golden contract). "GRS" = 0x475253.
constexpr std::uint64_t kSeedTurfClump = 0x475253'01;   // shared by turf tint AND blade clumping
constexpr std::uint64_t kSeedTurfJitter = 0x475253'02;
constexpr std::uint64_t kSeedTurfWear = 0x475253'03;
constexpr std::uint64_t kSeedScatter = 0x475253'04;     // per-cell root jitter
constexpr std::uint64_t kSeedBlade = 0x475253'05;       // per-blade height/facing/colour draws
constexpr std::uint64_t kSeedDensity = 0x475253'06;     // blade coverage field
constexpr std::uint64_t kSeedKeep = 0x475253'07;        // per-cell keep decision

// World-space constants (metres). Scale multiplies every feature size so the lawn zooms as one.
constexpr double kEyeHeight = 1.5;      // camera eye height
constexpr double kBladeH0 = 0.10;       // base blade length
constexpr double kBladeW0 = 0.004;      // base blade width
constexpr double kSpacing0 = 0.032;     // root grid pitch
constexpr double kClumpFeat0 = 0.5;     // tuft cell size
constexpr double kJitterFeat0 = 0.10;   // turf colour jitter
constexpr double kWearFeat0 = 2.2;      // low-frequency patch/wear
constexpr double kDensFeat0 = 0.9;      // blade density field
constexpr double kFadeStart0 = 7.0;     // depth (m) where blade density starts falling
constexpr double kFadeEnd0 = 22.0;      // depth (m) beyond which the far field is pure turf
constexpr double kVisibility0 = 16.0;   // aerial-perspective e-folding distance
constexpr double kMinBladePx = 0.9;     // LOD: blades shorter than this on screen collapse to turf
constexpr double kMinHalfPx = 0.5;      // width-correctness floor (thin blades -> 1px, alpha down)

// A small deterministic peeler: successive avalanche taps off one cell hash, so each blade attribute
// reads an independent uniform without a global stream (the §8.3 hashed-RNG discipline).
struct Peel {
    std::uint64_t s;
    double next() noexcept {
        s = avalanche(s);
        return hashToUnit(s);
    }
};

// The distant lawn / atmospheric mist the near sward fades into (a desaturated pale green-grey) and
// the sky above the horizon grades up from.
constexpr ColorF kHaze{0.63f, 0.67f, 0.60f, 1.0f};
constexpr ColorF kSkyTop{0.78f, 0.83f, 0.86f, 1.0f};

// One projected blade ready to rasterise. Screen control points of the quadratic Bezier, their
// screen half-widths (root/mid/tip taper already applied), the two lit endpoint colours (root ->
// tip, shade folded in; AO + aerial fade applied per-pixel in the raster), and the sort/cull data.
struct Blade {
    double p0x, p0y, p1x, p1y, p2x, p2y;
    double hw0, hw1, hw2;
    ColorF cRoot, cTip;
    double aoFloor;   // root-darkening floor (u=0), ramps to 1 at the tip
    double fade;      // aerial-perspective mix toward kHaze at this blade's depth
    double camZ;      // root depth: the back-to-front sort key
    std::uint64_t seq;  // generation index: the total-order tiebreak (deterministic sort)
    double bx0, by0, bx1, by1;  // screen AABB (already padded for AA)
};

Vec2 rotate2(Vec2 v, double ang) noexcept {
    const double c = std::cos(ang), s = std::sin(ang);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

// Distance from point (px,py) to segment (ax,ay)-(bx,by); also returns the clamped param `t`.
double segDist(double px, double py, double ax, double ay, double bx, double by,
               double& t) noexcept {
    const double dx = bx - ax, dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    t = len2 > 1e-12 ? clamp01(((px - ax) * dx + (py - ay) * dy) / len2) : 0.0;
    const double cx = ax + dx * t, cy = ay + dy * t;
    const double ex = px - cx, ey = py - cy;
    return std::sqrt(ex * ex + ey * ey);
}

// ---------------------------------------------------------------------------------------------
// Turf base pass -- the ground colour between and behind blades, sampled in GROUND space so clumps
// foreshorten with depth, and faded into the horizon haze (§6.1 step 2).
// ---------------------------------------------------------------------------------------------

struct TurfCtx {
    std::uint64_t clumpSeed, jitterSeed, wearSeed;
    double clumpFeat, jitterFeat, wearFeat, visibility;
    double patchiness;
    ColorF soil, base;
    FbmParams jitterFbm, wearFbm;
};

// Returns the turf colour + fade at a ground point; `fadeOut` is the aerial mix already computed.
ColorF turfAt(const TurfCtx& c, double gx, double gy, double dist, double& fadeOut) {
    const WorleyResult cell = worley2(c.clumpSeed, gx / c.clumpFeat, gy / c.clumpFeat);
    const double clumpTint = (hashToUnit(cell.cellId) - 0.5) * 0.12;
    const double seam = 1.0 - 0.08 * (1.0 - smoothstep(0.0, 0.12, cell.f2 - cell.f1));
    const double n = fbm2(c.jitterSeed, gx / c.jitterFeat, gy / c.jitterFeat, c.jitterFbm);
    const double wear = 0.5 + 0.5 * fbm2(c.wearSeed, gx / c.wearFeat, gy / c.wearFeat, c.wearFbm);
    const double mixT = clamp01(0.42 + 0.42 * n + clumpTint);
    ColorF col = mixColor(c.soil, c.base, mixT);
    const double darken = seam * (1.0 - c.patchiness * 0.4 * wear);
    col = {static_cast<float>(col.r * darken), static_cast<float>(col.g * darken),
           static_cast<float>(col.b * darken), 1.0f};
    fadeOut = 1.0 - std::exp(-dist / c.visibility);
    return mixColor(col, kHaze, fadeOut);
}

}  // namespace

common::Image renderGrass(const TextureParams& p, const GrassParams& grass, std::uint32_t w,
                          std::uint32_t h, const TextureWindow& window,
                          TextureRenderProgress* progress) {
    if (w == 0 || h == 0) return common::Image(w, h);
    // Camera, blade instancing and the LOD cull all work in FULL-frame coordinates; the window
    // selects which frame pixels get evaluated (turf) / rastered (blades), so it is a byte-exact
    // crop (§8.2) -- every pixel sees the same blade sequence it would in the full render.
    const ResolvedWindow win = resolveWindow(window, w, h);
    common::Image out(win.w, win.h);
    if (win.w == 0 || win.h == 0) return out;
    const bool bladePass = grass.enableBlades && grass.density > 0.0;
    if (progress != nullptr)
        progress->rowsTotal.store(static_cast<std::uint64_t>(win.h) * (bladePass ? 3 : 2),
                                  std::memory_order_relaxed);

    const double scale = std::max(1e-3, p.scale);
    const GrassCamera cam = GrassCamera::fromParams(grass, w, h, kEyeHeight);

    // Feature sizes (all scale together).
    const double clumpFeat = std::max(0.05, kClumpFeat0 * scale * std::max(0.1, grass.clumpScale));
    const double jitterFeat = std::max(0.01, kJitterFeat0 * scale);
    const double wearFeat = std::max(0.05, kWearFeat0 * scale);
    const double densFeat = std::max(0.05, kDensFeat0 * scale);
    const double visibility = std::max(1.0, kVisibility0 * scale);
    const double fadeStart = kFadeStart0 * scale;
    const double fadeEnd = kFadeEnd0 * scale;

    const std::uint64_t clumpSeed = subSeed(p.seed, kSeedTurfClump);
    const std::uint64_t jitterSeed = subSeed(p.seed, kSeedTurfJitter);
    const std::uint64_t wearSeed = subSeed(p.seed, kSeedTurfWear);
    const std::uint64_t scatterSeed = subSeed(p.seed, kSeedScatter);
    const std::uint64_t bladeSeed = subSeed(p.seed, kSeedBlade);
    const std::uint64_t densSeed = subSeed(p.seed, kSeedDensity);
    const std::uint64_t keepSeed = subSeed(p.seed, kSeedKeep);
    const double patchiness = clamp01(grass.patchiness);

    // Premultiplied straight-alpha float working buffer (banding-free composite; unpremultiplied to
    // 8-bit at the end). Turf writes it opaque; blades alpha-over it far->near. Window-sized;
    // indexed by window-local coords throughout.
    std::vector<float> buf(static_cast<std::size_t>(win.w) * win.h * 4, 0.0f);

    // ---- Pass 1: turf base + sky band ---------------------------------------------------------
    TurfCtx tc{};
    tc.clumpSeed = clumpSeed;
    tc.jitterSeed = jitterSeed;
    tc.wearSeed = wearSeed;
    tc.clumpFeat = clumpFeat;
    tc.jitterFeat = jitterFeat;
    tc.wearFeat = wearFeat;
    tc.visibility = visibility;
    tc.patchiness = patchiness;
    tc.soil = grass.soilColor;
    tc.base = grass.baseColor;
    tc.jitterFbm = FbmParams{4, 2.0, 0.5};
    tc.wearFbm = FbmParams{3, 2.0, 0.5};
    const bool turf = grass.enableTurf;

    parallelRows(win.h, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            for (std::uint32_t x = 0; x < win.w; ++x) {
                const std::size_t dp = (static_cast<std::size_t>(y) * win.w + x) * 4;
                const double px = static_cast<double>(win.x) + x + 0.5;
                const double py = static_cast<double>(win.y) + y + 0.5;
                SkyVec3 g;
                double dist = 0.0;
                ColorF col;
                if (cam.groundAt(px, py, g, dist)) {
                    double fade = 0.0;
                    col = turfAt(tc, g.x, g.y, dist, fade);
                } else {
                    // Above the horizon: a soft mist->sky gradient by how far up the ray points.
                    const SkyVec3 r = cam.rayAt(px, py);
                    col = mixColor(kHaze, kSkyTop, smoothstep(0.0, 0.32, r.z));
                }
                if (turf) {
                    buf[dp + 0] = col.r;  // opaque -> premultiplied == straight
                    buf[dp + 1] = col.g;
                    buf[dp + 2] = col.b;
                    buf[dp + 3] = 1.0f;
                }  // else leave transparent (§3.4): only blades will write coverage
            }
            if (!progressRowTick(progress)) return;
        }
    });

    // ---- Pass 2: blade instancing (deterministic generation, back-to-front banded raster) ------
    if (bladePass && !renderCancelled(progress)) {
        const SkyVec3 lightDir = directionFromAzEl(grass.lightAzimuthDeg, grass.lightElevationDeg);
        const double windRad = grass.windDirectionDeg * std::numbers::pi / 180.0;
        const Vec2 windDir2 = rotate2(Vec2{0.0, 1.0}, windRad);  // ground lean direction (unit)
        const double s = std::max(1e-3, kSpacing0 * scale);
        const double H0 = kBladeH0 * std::max(0.0, grass.bladeHeight) * scale;
        const double W0 = kBladeW0 * std::max(0.05, grass.bladeWidth) * scale;
        const double dryAmt = clamp01(grass.dryAmount);
        const FbmParams densFbm{3, 2.0, 0.5};
        const FbmParams wearFbmB{3, 2.0, 0.5};

        // Ground-Y range covering [near, fadeEnd] in depth. camZ = y*cos(pitch)+eye*sin(pitch).
        const double cz0 = cam.forward.y;            // == cos(pitch)
        const double czBias = kEyeHeight * cam.up.y;  // == eye * sin(pitch)
        const double yEnd = (fadeEnd - czBias) / std::max(1e-3, cz0);
        const double yStart = 0.02;
        const std::int64_t j0 = static_cast<std::int64_t>(std::floor(yStart / s));
        const std::int64_t j1 = static_cast<std::int64_t>(std::ceil(yEnd / s));

        std::vector<Blade> blades;
        blades.reserve(4096);
        std::uint64_t seq = 0;
        // Defensive cap: bounded by the fixed world reach (independent of resolution), but never let
        // a pathological Scale blow memory -- log-free hard stop, honestly surfaced by the count.
        constexpr std::size_t kMaxBlades = 6'000'000;

        for (std::int64_t j = j0; j <= j1 && blades.size() < kMaxBlades; ++j) {
            if (renderCancelled(progress)) break;  // single-threaded stage: cheap per-row check
            const double Y0 = (static_cast<double>(j) + 0.5) * s;
            const double camZrow = Y0 * cz0 + czBias;
            if (camZrow <= 0.05) continue;
            const double depthFade = smoothstep(fadeEnd, fadeStart, camZrow);  // 1 near, 0 far
            if (depthFade <= 0.0) continue;
            const double xHalf = camZrow * cam.halfTanX * 1.35 + 2.0 * s;
            const std::int64_t i0 = static_cast<std::int64_t>(std::floor(-xHalf / s));
            const std::int64_t i1 = static_cast<std::int64_t>(std::ceil(xHalf / s));
            for (std::int64_t i = i0; i <= i1; ++i) {
                if (blades.size() >= kMaxBlades) break;
                // Root jitter (single-class Poisson-style stratified sample).
                Peel jit{hashCoords(scatterSeed, i, j)};
                const double rx = (static_cast<double>(i) + 0.5) * s + (jit.next() - 0.5) * s * 0.9;
                const double ry = (static_cast<double>(j) + 0.5) * s + (jit.next() - 0.5) * s * 0.9;

                // Keep decision: density field * user density * depth fade, thinned by wear/patches.
                const double densF = 0.5 + 0.5 * fbm2(densSeed, rx / densFeat, ry / densFeat, densFbm);
                const double wearF =
                    0.5 + 0.5 * fbm2(wearSeed, rx / wearFeat, ry / wearFeat, wearFbmB);
                double keepP = clamp01(grass.density) * (0.30 + 0.70 * densF) * depthFade;
                keepP *= (1.0 - patchiness * 0.6 * wearF);
                if (hashToUnit(hashCoords(keepSeed, i, j)) > keepP) continue;

                // Root projection + LOD cull.
                double p0x, p0y, z0;
                if (!cam.project({rx, ry, 0.0}, p0x, p0y, z0)) continue;

                // Clump identity (shared with the turf field): coherent facing + height per tuft.
                const WorleyResult cell = worley2(clumpSeed, rx / clumpFeat, ry / clumpFeat);
                Peel cp{avalanche(cell.cellId ^ 0x9e3779b97f4a7c15ULL)};
                const double clumpFacing = (cp.next() - 0.5) * 0.8;   // rad, coherent per tuft
                const double clumpHeight = 0.78 + 0.5 * cp.next();

                Peel bp{hashCoords(bladeSeed, i, j)};
                const double worldH = H0 * clumpHeight * (0.7 + 0.6 * bp.next());
                if (worldH <= 1e-4) continue;
                const double bladeScreenH = worldH * cam.focalY / z0;
                if (bladeScreenH < kMinBladePx) continue;  // sub-pixel -> the turf carries it

                // Facing: the global wind lean, rotated by the tuft's coherent bias + a blade jitter.
                const double facing = clumpFacing + (bp.next() - 0.5) * 0.5;
                const Vec2 lean2 = rotate2(windDir2, facing);
                const double leanAmt = clamp01(grass.windStrength) * (0.55 + 0.5 * bp.next());
                const SkyVec3 leanW{lean2.x * leanAmt * worldH, lean2.y * leanAmt * worldH, 0.0};

                const SkyVec3 root{rx, ry, 0.0};
                const SkyVec3 tip{root.x + leanW.x, root.y + leanW.y, root.z + worldH};
                // Mid control: raised + eased back against the lean for a convex, drooping arc.
                const double bendMag = clamp01(grass.curvature) * worldH;
                const SkyVec3 mid{0.5 * (root.x + tip.x) - lean2.x * bendMag * 0.22,
                                  0.5 * (root.y + tip.y) - lean2.y * bendMag * 0.22,
                                  0.5 * (root.z + tip.z) + worldH * (0.16 + 0.14 * clamp01(grass.curvature))};

                double p1x, p1y, z1, p2x, p2y, z2;
                if (!cam.project(mid, p1x, p1y, z1) || !cam.project(tip, p2x, p2y, z2)) continue;

                // Screen half-widths (root/mid/tip taper).
                const double hw0 = 0.5 * W0 * 1.0 * cam.focalX / z0;
                const double hw1 = 0.5 * W0 * 0.62 * cam.focalX / z1;
                const double hw2 = 0.5 * W0 * 0.18 * cam.focalX / z2;

                // ---- Shading (constant per blade; AO + fade applied per-pixel) -----------------
                SkyVec3 T = skyNormalize({tip.x - root.x, tip.y - root.y, tip.z - root.z});
                const SkyVec3 V = skyNormalize({cam.eye.x - root.x, cam.eye.y - root.y,
                                                cam.eye.z - root.z});
                const SkyVec3 Hh = skyNormalize({lightDir.x + V.x, lightDir.y + V.y, lightDir.z + V.z});
                const double TdL = skyDot(T, lightDir);
                const double TdH = skyDot(T, Hh);
                const double sinTL = std::sqrt(std::max(0.0, 1.0 - TdL * TdL));
                const double sinTH = std::sqrt(std::max(0.0, 1.0 - TdH * TdH));
                // Broad face toward the camera (view minus its tangent component) for a wrap diffuse.
                const double VdT = skyDot(V, T);
                SkyVec3 faceN = skyNormalize({V.x - T.x * VdT, V.y - T.y * VdT, V.z - T.z * VdT});
                const double ndl = skyDot(faceN, lightDir);
                const double wrap = clamp01(ndl * 0.5 + 0.5);            // half-Lambert
                const double kkSpec = std::pow(sinTH, 24.0);            // anisotropic tip sheen
                const double occ = 1.0 - 0.22 * clamp01(densF);         // clump self-shadow (Reeves)
                double shade = (0.30 + 0.55 * wrap + 0.18 * sinTL) * occ;
                shade = std::clamp(shade + 0.45 * kkSpec, 0.0, 1.6);

                // Backlit translucency: light coming through the blade toward the camera glows.
                const double back = clamp01(-skyDot(V, lightDir));
                const double trans = std::pow(back, 2.2) * 0.7;

                // Colour: base->tip gradient, per-blade jitter, occasional dry/straw blade.
                const double tintJ = 0.86 + 0.26 * bp.next();
                ColorF baseC = grass.baseColor, tipC = grass.tipColor;
                if (bp.next() < dryAmt) {
                    baseC = mixColor(baseC, grass.dryColor, 0.75);
                    tipC = mixColor(tipC, grass.dryColor, 0.85);
                }
                const double m = shade * tintJ;
                const ColorF glow = grass.tipColor;  // backlit leaf glow, concentrated at the tip
                const auto lit = [&](ColorF c, double g) {
                    return ColorF{static_cast<float>(clamp01(c.r * m + glow.r * g)),
                                  static_cast<float>(clamp01(c.g * m + glow.g * g)),
                                  static_cast<float>(clamp01(c.b * m + glow.b * g)), 1.0f};
                };
                Blade b{};
                b.p0x = p0x; b.p0y = p0y; b.p1x = p1x; b.p1y = p1y; b.p2x = p2x; b.p2y = p2y;
                b.hw0 = hw0; b.hw1 = hw1; b.hw2 = hw2;
                b.cRoot = lit(baseC, 0.0);
                b.cTip = lit(tipC, trans * 0.6);  // tip carries the backlit glow
                b.aoFloor = 0.42;
                b.fade = 1.0 - std::exp(-z0 / visibility);
                b.camZ = z0;
                b.seq = seq++;
                const double maxHw = std::max({hw0, hw1, hw2}) + 1.5;
                b.bx0 = std::min({p0x, p1x, p2x}) - maxHw;
                b.by0 = std::min({p0y, p1y, p2y}) - maxHw;
                b.bx1 = std::max({p0x, p1x, p2x}) + maxHw;
                b.by1 = std::max({p0y, p1y, p2y}) + maxHw;
                blades.push_back(b);
            }
        }

        // Back-to-front: far (large camZ) first, near last -> near composites on top. Total order
        // (camZ, seq) keeps the sort deterministic across thread counts / optimisation levels.
        std::sort(blades.begin(), blades.end(), [](const Blade& a, const Blade& b) {
            if (a.camZ != b.camZ) return a.camZ > b.camZ;
            return a.seq < b.seq;
        });

        // Banded raster: disjoint row bands, each replays ALL blades in the shared far->near order,
        // clipped to its rows. A blade touching two bands writes disjoint pixels, so the banded
        // result is byte-identical to a single-threaded painter's pass. Bands cover the WINDOW's
        // rows; all clipping happens in frame coordinates (a pixel's value depends only on its
        // frame position and the shared blade order, so the window stays a byte-exact crop).
        parallelRows(win.h, [&](std::size_t row0, std::size_t row1) {
            bool cancelled = false;
            const long fRow0 = win.y + static_cast<long>(row0);  // frame rows of this band
            const long fRow1 = win.y + static_cast<long>(row1);
            const auto rTop = static_cast<double>(fRow0);
            const auto rBot = static_cast<double>(fRow1);
            std::size_t bladesDone = 0;
            for (const Blade& b : blades) {
                // Progress/cancel by share of blades replayed (rows are useless here: every band
                // walks the whole list). Ticks map onto this band's row budget.
                if (progress != nullptr && ++bladesDone % 4096 == 0 &&
                    progress->cancel.load(std::memory_order_relaxed)) {
                    cancelled = true;
                    break;
                }
                if (b.by1 < rTop || b.by0 >= rBot) continue;
                const int xlo = std::max(static_cast<int>(win.x), static_cast<int>(std::floor(b.bx0)));
                const int xhi = std::min(static_cast<int>(win.x) + static_cast<int>(win.w) - 1,
                                         static_cast<int>(std::ceil(b.bx1)));
                const int ylo = std::max(static_cast<int>(fRow0), static_cast<int>(std::floor(b.by0)));
                const int yhi =
                    std::min(static_cast<int>(fRow1) - 1, static_cast<int>(std::ceil(b.by1)));
                if (xlo > xhi || ylo > yhi) continue;

                // Sample the screen Bezier into a short polyline (u, position, half-width).
                const double segLen = std::hypot(b.p2x - b.p0x, b.p2y - b.p0y) +
                                      std::hypot(b.p1x - b.p0x, b.p1y - b.p0y);
                const int N = std::clamp(static_cast<int>(std::lround(segLen / 2.5)), 5, 22);
                std::array<double, 23> sx{}, sy{}, su{}, shw{};
                for (int k = 0; k < N; ++k) {
                    const double u = static_cast<double>(k) / (N - 1);
                    const double a0 = (1 - u) * (1 - u), a1 = 2 * (1 - u) * u, a2 = u * u;
                    sx[k] = a0 * b.p0x + a1 * b.p1x + a2 * b.p2x;
                    sy[k] = a0 * b.p0y + a1 * b.p1y + a2 * b.p2y;
                    su[k] = u;
                    shw[k] = a0 * b.hw0 + a1 * b.hw1 + a2 * b.hw2;
                }
                for (int y = ylo; y <= yhi; ++y) {
                    for (int x = xlo; x <= xhi; ++x) {
                        const double px = static_cast<double>(x) + 0.5;
                        const double py = static_cast<double>(y) + 0.5;
                        double best = 1e18, bestU = 0.0, bestHw = 0.0;
                        for (int k = 0; k + 1 < N; ++k) {
                            double t;
                            const double d = segDist(px, py, sx[k], sy[k], sx[k + 1], sy[k + 1], t);
                            if (d < best) {
                                best = d;
                                bestU = mixd(su[k], su[k + 1], t);
                                bestHw = mixd(shw[k], shw[k + 1], t);
                            }
                        }
                        const double hwTrue = bestHw;
                        const double hwEff = std::max(hwTrue, kMinHalfPx);
                        const double alphaW = hwTrue < kMinHalfPx ? hwTrue / kMinHalfPx : 1.0;
                        double cov = smoothstep(hwEff + 0.5, hwEff - 0.5, best) * alphaW;
                        if (cov <= 0.0) continue;
                        cov = clamp01(cov);

                        const double ao = mixd(b.aoFloor, 1.0, std::pow(bestU, 1.1));
                        ColorF c = mixColor(b.cRoot, b.cTip, bestU);
                        c = {static_cast<float>(c.r * ao), static_cast<float>(c.g * ao),
                             static_cast<float>(c.b * ao), 1.0f};
                        c = mixColor(c, kHaze, b.fade);  // aerial perspective

                        const std::size_t dp =
                            (static_cast<std::size_t>(y - win.y) * win.w +
                             static_cast<std::size_t>(x - win.x)) *
                            4;
                        const double om = 1.0 - cov;
                        buf[dp + 0] = static_cast<float>(c.r * cov + buf[dp + 0] * om);
                        buf[dp + 1] = static_cast<float>(c.g * cov + buf[dp + 1] * om);
                        buf[dp + 2] = static_cast<float>(c.b * cov + buf[dp + 2] * om);
                        buf[dp + 3] = static_cast<float>(cov + buf[dp + 3] * om);
                    }
                }
            }
            // Row ticks land as one band-sized chunk: the raster iterates blades outer, rows
            // inner, so per-row ticks do not exist here.
            if (progress != nullptr && !cancelled)
                progress->rowsDone.fetch_add(row1 - row0, std::memory_order_relaxed);
        });
    }

    // ---- Unpremultiply -> 8-bit straight alpha ------------------------------------------------
    parallelRows(win.h, [&](std::size_t row0, std::size_t row1) {
        for (std::uint32_t y = static_cast<std::uint32_t>(row0); y < row1; ++y) {
            for (std::uint32_t x = 0; x < win.w; ++x) {
                const std::size_t dp = (static_cast<std::size_t>(y) * win.w + x) * 4;
                const double a = buf[dp + 3];
                const double inv = a > 1e-6 ? 1.0 / a : 0.0;
                const auto q = [&](double ch) {
                    return static_cast<std::uint8_t>(std::clamp(ch * inv * 255.0 + 0.5, 0.0, 255.0));
                };
                out.rgba[dp + 0] = q(buf[dp + 0]);
                out.rgba[dp + 1] = q(buf[dp + 1]);
                out.rgba[dp + 2] = q(buf[dp + 2]);
                out.rgba[dp + 3] =
                    static_cast<std::uint8_t>(std::clamp(a * 255.0 + 0.5, 0.0, 255.0));
            }
            if (!progressRowTick(progress)) return;
        }
    });
    return out;
}

// ---- §6 preset library ------------------------------------------------------------------------
// Each preset is a complete GrassParams value; the S55-f dialog sets every knob from it, then the
// sliders fine-tune. Physical-fact starting points, tuned in the render pass.

namespace {

GrassParams presetLawn() {
    GrassParams g;  // tidy, dense, mown -- the default well-kept sward
    g.density = 0.9;
    g.bladeHeight = 0.85;
    g.curvature = 0.35;
    g.windStrength = 0.25;
    g.patchiness = 0.35;
    g.dryAmount = 0.06;
    return g;
}

GrassParams presetMeadow() {
    GrassParams g;  // taller, leaning, patchy, a few straws
    g.baseColor = {0.17f, 0.31f, 0.10f, 1.0f};
    g.tipColor = {0.52f, 0.68f, 0.28f, 1.0f};
    g.density = 0.8;
    g.bladeHeight = 1.35;
    g.curvature = 0.6;
    g.windStrength = 0.45;
    g.windDirectionDeg = 40.0;
    g.patchiness = 0.55;
    g.dryAmount = 0.18;
    g.clumpScale = 1.3;
    return g;
}

GrassParams presetDrySavanna() {
    GrassParams g;  // sparse, tall, sun-bleached straw
    g.baseColor = {0.34f, 0.31f, 0.13f, 1.0f};
    g.tipColor = {0.68f, 0.62f, 0.30f, 1.0f};
    g.soilColor = {0.22f, 0.18f, 0.10f, 1.0f};
    g.dryColor = {0.72f, 0.64f, 0.30f, 1.0f};
    g.density = 0.55;
    g.bladeHeight = 1.5;
    g.curvature = 0.7;
    g.windStrength = 0.55;
    g.patchiness = 0.75;
    g.dryAmount = 0.6;
    g.clumpScale = 1.6;
    return g;
}

GrassParams presetPuttingGreen() {
    GrassParams g;  // very short, dense, uniform, upright
    g.baseColor = {0.13f, 0.32f, 0.11f, 1.0f};
    g.tipColor = {0.34f, 0.58f, 0.20f, 1.0f};
    g.density = 1.0;
    g.bladeHeight = 0.45;
    g.bladeWidth = 0.85;
    g.curvature = 0.15;
    g.windStrength = 0.1;
    g.patchiness = 0.12;
    g.dryAmount = 0.02;
    g.clumpScale = 0.7;
    return g;
}

GrassParams presetWildOvergrown() {
    GrassParams g;  // tall, strongly clumped, drooping, wind-blown
    g.baseColor = {0.15f, 0.29f, 0.09f, 1.0f};
    g.tipColor = {0.50f, 0.66f, 0.26f, 1.0f};
    g.density = 0.85;
    g.bladeHeight = 1.9;
    g.bladeWidth = 1.2;
    g.curvature = 0.85;
    g.windStrength = 0.6;
    g.windDirectionDeg = 55.0;
    g.patchiness = 0.6;
    g.dryAmount = 0.22;
    g.clumpScale = 1.8;
    return g;
}

GrassParams presetMoss() {
    GrassParams g;  // very short, very dense, dark and soft
    g.baseColor = {0.10f, 0.24f, 0.08f, 1.0f};
    g.tipColor = {0.26f, 0.46f, 0.16f, 1.0f};
    g.soilColor = {0.07f, 0.10f, 0.05f, 1.0f};
    g.density = 1.0;
    g.bladeHeight = 0.3;
    g.bladeWidth = 0.7;
    g.curvature = 0.5;
    g.windStrength = 0.15;
    g.patchiness = 0.25;
    g.dryAmount = 0.03;
    g.clumpScale = 0.5;
    return g;
}

const std::array<GrassPreset, 6> kGrassPresets{{
    {"Lawn", presetLawn()},
    {"Meadow", presetMeadow()},
    {"Dry / savanna", presetDrySavanna()},
    {"Putting green", presetPuttingGreen()},
    {"Wild / overgrown", presetWildOvergrown()},
    {"Moss", presetMoss()},
}};

}  // namespace

std::size_t grassPresetCount() {
    return kGrassPresets.size();
}

const GrassPreset& grassPreset(std::size_t i) {
    return kGrassPresets[std::min(i, kGrassPresets.size() - 1)];
}

}  // namespace mosaic::core::texture
