#include "core/vector/pattern.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

// Procedural pattern evaluation (docs/layer-effects.md §7). Each kind is a pure signed-distance
// field in unit-cell space (+ = inside the fg feature); the field is sampled at the layer point
// scaled by 1/scale and rotated by -angle, converted to a 1px linear-ramp coverage, and the fg is
// composited over bg. All maths is textbook (grids/lattices/value-noise) -- no ML anywhere.
// The herringbone/parquet brick lattice was found by a seamless-tiling search.
namespace mosaic::core::vec {
namespace {

using common::ColorF;
using common::Vec2;

constexpr double kSqrt1_2 = 0.70710678118654752440;

double fracv(double x) { return x - std::floor(x); }
double distToInt(double x) { return 0.5 - std::abs(fracv(x) - 0.5); }  // 0..0.5

// integer parity that is correct for negative coordinates (`% 2` is not).
bool evenParity(double a, double b) {
    return ((static_cast<std::int64_t>(std::floor(a)) + static_cast<std::int64_t>(std::floor(b))) &
            1) == 0;
}
bool oddInt(double v) { return (static_cast<std::int64_t>(v) & 1) != 0; }

Vec2 rotate(Vec2 p, double deg) {
    const double a = deg * M_PI / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    return {p.x * c - p.y * s, p.x * s + p.y * c};
}

// Signed distance to the vertical-stripe band [0,duty) within each unit period (+ = inside).
double stripeSD(double x, double duty) {
    const double m = fracv(x);
    const double d = std::min({m, 1.0 - m, std::abs(m - duty)});
    return (m < duty) ? d : -d;
}

// Signed distance into an axis-aligned rect (+ = inside).
double insideRect(Vec2 p, double x0, double y0, double w, double h) {
    return std::min({p.x - x0, x0 + w - p.x, p.y - y0, y0 + h - p.y});
}

// Herringbone brick tiling: H bricks 2x1 + V bricks 1x2 on the integer lattice a*V1 + b*V2, the V
// brick offset by S. (V1,V2,S) were found by a seamless-tiling search so mortar never gaps.
constexpr double kV1x = 1, kV1y = -3, kV2x = 1, kV2y = 1, kSx = -1, kSy = 0;
// inverse of [[V1x V2x];[V1y V2y]] (det = 4), to estimate the lattice cell of a point.
constexpr double kMi00 = 0.25, kMi01 = -0.25, kMi10 = 0.75, kMi11 = 0.25;

double herringboneSD(Vec2 u, double mortar) {
    const double a0 = std::round(kMi00 * u.x + kMi01 * u.y);
    const double b0 = std::round(kMi10 * u.x + kMi11 * u.y);
    double best = -1e30;
    // +/-2 window: a 2-unit brick's origin corner can sit up to ~2 lattice steps from the sampled
    // point, so a 3x3 search shaves far corners off some bricks. 5x5 reaches every brick that can
    // contain the point (LE-d chipped-corner fix).
    for (int da = -2; da <= 2; ++da)
        for (int db = -2; db <= 2; ++db) {
            const double a = a0 + da, b = b0 + db;
            const double cx = a * kV1x + b * kV2x, cy = a * kV1y + b * kV2y;
            best = std::max(best, insideRect(u, cx, cy, 2, 1));
            best = std::max(best, insideRect(u, cx + kSx, cy + kSy, 1, 2));
        }
    return best - mortar;
}

// Value noise (classic Perlin lineage) for the Grain kind.
double hash2(double i, double j) {
    const double h = std::sin(i * 127.1 + j * 311.7) * 43758.5453;
    return h - std::floor(h);
}
double valueNoise(double x, double y) {
    const double ix = std::floor(x), iy = std::floor(y);
    const double fx = x - ix, fy = y - iy;
    const double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const double a = hash2(ix, iy), b = hash2(ix + 1, iy);
    const double c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1);
    return a * (1 - sx) * (1 - sy) + b * sx * (1 - sy) + c * (1 - sx) * sy + d * sx * sy;
}

// Min |distance-to-ring| over the offset-row circle lattice (Chainmail / interlocking rings).
double chainmailRingDist(Vec2 u) {
    constexpr double rowSp = 0.5, R = 0.5;
    const double j0 = std::round(u.y / rowSp);
    double best = 1e30;
    for (int dj = -1; dj <= 1; ++dj) {
        const double j = j0 + dj;
        const double cy = j * rowSp;
        const double off = oddInt(j) ? 0.5 : 0.0;
        const double i0 = std::round(u.x - off);
        for (int di = -1; di <= 1; ++di) {
            const double cx = i0 + di + off;
            best = std::min(best, std::abs(std::hypot(u.x - cx, u.y - cy) - R));
        }
    }
    return best;
}

// Signed distance to an UPRIGHT 5-point star (Inigo Quilez sdStar5): r = outer radius, rf = inner/
// outer ratio; negative inside. A friendly chunky star (rf ~ 0.52), not a spiky pentagram.
double sdStar5(double px, double py, double r, double rf) {
    constexpr double k1x = 0.809016994, k1y = -0.587785252;
    constexpr double k2x = -0.809016994, k2y = -0.587785252;
    px = std::abs(px);
    const double d1 = std::max(px * k1x + py * k1y, 0.0);
    px -= 2 * d1 * k1x;
    py -= 2 * d1 * k1y;
    const double d2 = std::max(px * k2x + py * k2y, 0.0);
    px -= 2 * d2 * k2x;
    py -= 2 * d2 * k2y;
    px = std::abs(px);
    py -= r;
    const double bax = rf * (-k1y);
    const double bay = rf * k1x - 1.0;
    const double h = std::clamp((px * bax + py * bay) / (bax * bax + bay * bay), 0.0, r);
    return std::hypot(px - bax * h, py - bay * h) * ((py * bax - px * bay) < 0.0 ? -1.0 : 1.0);
}

// The fg-colour of a staggered cell's row offset (hearts / stars: alternate rows shift half a cell).
double staggerOffset(double y) { return oddInt(std::floor(y)) ? 0.5 : 0.0; }

// The unit-cell "range" a motif-on-lattice kind maps its cell to: a larger range shrinks the fixed-
// shape motif within its cell, opening the gap between neighbours. `spacing` in [0,1] drives it; the
// per-kind base is calibrated so spacing == 0.25 (the default) reproduces the original packed look.
double motifRange(double base, double spacing) { return base + std::clamp(spacing, 0.0, 1.0) * 2.0; }

// Signed distance (unit space, + = inside fg) for each procedural kind. `w` (weight) in [0,1] is the
// thickness/radius/duty knob; `spc` (spacing) in [0,1] only bites on the motif kinds (patternUsesSpacing).
double kindSD(ProceduralPattern::Kind kind, Vec2 u, double w, double spc) {
    using K = ProceduralPattern::Kind;
    switch (kind) {
        case K::Dots: {
            const double r = w * 0.5;
            return r - std::hypot(fracv(u.x) - 0.5, fracv(u.y) - 0.5);
        }
        case K::Grid:
            return w * 0.25 - std::min(distToInt(u.x), distToInt(u.y));
        case K::Lines:
            return stripeSD(u.x, w);
        case K::Hatch:
            return stripeSD(rotate(u, 45).x, w);
        case K::CrossHatch:
            return std::max(stripeSD(rotate(u, 45).x, w), stripeSD(rotate(u, -45).x, w));
        case K::Checker: {
            // The first FULL square begins at u==0 (the content top-left corner) so the checker starts
            // on a whole square there, not mid-cell. samplePattern nudges the phase a hair inward so the
            // shape's top/left silhouette edge clears the cell boundary's AA feather (no 1px top/left
            // seam) -- keeping the corner square visually full while avoiding the user-reported edge.
            const double db =
                std::min(0.5 - std::abs(fracv(u.x) - 0.5), 0.5 - std::abs(fracv(u.y) - 0.5));
            return evenParity(u.x, u.y) ? db : -db;
        }
        case K::Herringbone:
            return herringboneSD(rotate(u, -45.0), 0.06 + w * 0.10);
        case K::Parquet:
            return herringboneSD(u, 0.06 + w * 0.10);
        case K::Basketweave: {
            // Checkerboard of horizontal / vertical bar blocks (clean seams -- the diagonal-band
            // variant left fg-tinted slivers where the two orientations met).
            constexpr double L = 2.0;
            const double t = 0.04 + w * 0.10;
            const bool horiz = evenParity(u.x / L, u.y / L);
            const double dh = std::min(distToInt(u.y), distToInt(u.x / L) * L);
            const double dv = std::min(distToInt(u.x), distToInt(u.y / L) * L);
            return (horiz ? dh : dv) - t;
        }
        case K::Chevron: {
            // Down-pointing arrowhead bands with VERTICAL lines running through/between them.
            const double m2 = u.x - 2.0 * std::floor(u.x / 2.0);  // mod 2
            const double band = stripeSD(u.y - std::abs(m2 - 1.0), w);
            const double vl = (0.05 + w * 0.05) - distToInt(u.x);
            return std::max(band, vl);
        }
        case K::Zigzag: {
            const double m2 = u.x - 2.0 * std::floor(u.x / 2.0);
            return stripeSD(u.y + std::abs(m2 - 1.0), w);
        }
        case K::Chainmail:
            return (0.03 + w * 0.11) - chainmailRingDist(u);
        case K::Halftone: {
            const Vec2 r = rotate(u, 45);
            const double rad = std::sqrt(std::max(w, 0.0)) * 0.707;
            return rad - std::hypot(fracv(r.x) - 0.5, fracv(r.y) - 0.5);
        }
        case K::Grain:
            return (valueNoise(u.x * 2.5, u.y * 2.5) - (1.0 - w)) * 1.5;
        case K::Bricks: {
            constexpr double rowH = 0.5;
            const double off = oddInt(std::floor(u.y / rowH)) ? 0.5 : 0.0;
            const double t = 0.04 + w * 0.10;
            return std::min(distToInt(u.x + off), distToInt(u.y / rowH) * rowH) - t;
        }
        case K::Triangles: {
            // True equilateral tessellation: shear a unit lattice by half the row height so the
            // anti-diagonal split yields up/down triangles (fg = up), not right-triangles on a square
            // grid (which reads as a hatched grid).
            constexpr double kRowH = 0.86602540378;  // sqrt(3)/2
            const double yy = u.y / kRowH;
            const double xx = u.x - 0.5 * yy;
            const double fx = xx - std::floor(xx), fy = yy - std::floor(yy);
            const bool upper = (fx + fy) > 1.0;
            const double diag = std::abs(fx + fy - 1.0) * kSqrt1_2;
            const double de = upper ? std::min(diag, std::min(1.0 - fx, 1.0 - fy))
                                    : std::min(diag, std::min(fx, fy));
            return (upper ? de : -de) * 0.87;  // 0.87 ~ sheared-edge screen scale (AA ~1px)
        }
        case K::Sawtooth: {
            // Right triangles, EVERY row identical (the "/" diagonal, fg = lower-left).
            const double ly = u.y - std::floor(u.y);
            const double fx = u.x - std::floor(u.x);
            return (1.0 - fx - ly) * kSqrt1_2;
        }
        case K::Harlequin: {
            // 2-tone diamonds as a checker in sheared coords -> seamless (no base edge to leak).
            const double sv = u.y / 0.62;  // taller-than-wide diamonds
            const double a = u.x + sv, b = u.x - sv;
            const double edge = std::min(distToInt(a), distToInt(b));
            return (evenParity(a, b) ? edge : -edge) * 1.4;
        }
        case K::Honeycomb: {
            // Filled FLAT-TOP hexagons, each with its own thin gap (walls) -- no vertex hexagrams.
            constexpr double s = 0.62, sqrt3 = 1.7320508;
            const double e1x = 0.0, e1y = sqrt3 * s, e2x = 1.5 * s, e2y = sqrt3 * s / 2;
            const double det = e1x * e2y - e1y * e2x;
            const double m0 = std::round((u.x * e2y - u.y * e2x) / det);
            const double n0 = std::round((-u.x * e1y + u.y * e1x) / det);
            double best = 1e30, bcx = 0, bcy = 0;
            for (int dm = -1; dm <= 1; ++dm)
                for (int dn = -1; dn <= 1; ++dn) {
                    const double m = m0 + dm, n = n0 + dn;
                    const double cx = m * e1x + n * e2x, cy = m * e1y + n * e2y;
                    const double d = std::hypot(u.x - cx, u.y - cy);
                    if (d < best) { best = d; bcx = cx; bcy = cy; }
                }
            const double px = std::abs(u.x - bcx), py = std::abs(u.y - bcy);
            const double hd = std::max(py, 0.8660254 * px + 0.5 * py);
            return (s * sqrt3 / 2 - (0.02 + w * 0.07)) - hd;
        }
        case K::Hearts: {
            // Staggered hearts via the normalised implicit heart (crisp, not the blurry raw cubic).
            const double rng = motifRange(2.2, spc);  // spc 0.25 -> 2.7 (original)
            const double hx = (fracv(u.x + staggerOffset(u.y)) - 0.5) * rng;
            const double hy = -((fracv(u.y) - 0.5) * rng) + 0.35;  // flip so the point is at the bottom
            const double q = hx * hx + hy * hy - 1.0;
            const double f = q * q * q - hx * hx * hy * hy * hy;
            const double gx = 6 * hx * q * q - 2 * hx * hy * hy * hy;
            const double gy = 6 * hy * q * q - 3 * hx * hx * hy * hy;
            return (-f / (std::hypot(gx, gy) + 1e-6)) / rng * 2.2;
        }
        case K::StarAnise: {
            // The 5-ray burst (angle-lerped radius -> curved rays, the "star anise" look).
            const double rng = motifRange(1.7, spc);  // spc 0.25 -> 2.2 (original)
            constexpr double Ro = 0.9, Ri = 0.38, wedge = 2.0 * M_PI / 5.0;
            const double hx = (fracv(u.x + staggerOffset(u.y)) - 0.5) * rng;
            const double hy = (fracv(u.y) - 0.5) * rng;
            const double a = std::atan2(hy, hx), r = std::hypot(hx, hy);
            double an = a - wedge * std::floor(a / wedge);  // mod
            an = std::abs(an - M_PI / 5.0);
            const double tt = an / (M_PI / 5.0);
            return ((Ro * (1 - tt) + Ri * tt) - r) * 0.8;
        }
        case K::Stars: {
            const double rng = motifRange(1.9, spc);  // spc 0.25 -> 2.4 (original)
            const double hx = (fracv(u.x + staggerOffset(u.y)) - 0.5) * rng;
            const double hy = (fracv(u.y) - 0.5) * rng;
            return (-sdStar5(hx, -hy, 0.95, 0.52)) / rng * 2.4;  // -hy -> point up
        }
        case K::Waves:
            return stripeSD(u.y + 0.3 * std::sin(u.x * M_PI), w);
        case K::Crosses: {
            const double cx = fracv(u.x) - 0.5, cy = fracv(u.y) - 0.5;
            const double arm = 0.08 + w * 0.10, ln = 0.36;
            return std::max(std::min(arm - std::abs(cy), ln - std::abs(cx)),
                            std::min(arm - std::abs(cx), ln - std::abs(cy)));
        }
        case K::Rings: {
            const double cx = fracv(u.x) - 0.5, cy = fracv(u.y) - 0.5;
            return (0.04 + w * 0.09) - std::abs(std::hypot(cx, cy) - 0.36);
        }
    }
    return -1e30;
}

// fg (coverage-modulated) source-over bg, straight alpha.
ColorF fgOverBg(const ColorF& fg, const ColorF& bg, float cov) {
    const float srcA = fg.a * cov;
    const float outA = srcA + bg.a * (1.0f - srcA);
    if (outA <= 1e-8f) return {0, 0, 0, 0};
    const float k = 1.0f / outA;
    const float ib = bg.a * (1.0f - srcA);
    return {(fg.r * srcA + bg.r * ib) * k, (fg.g * srcA + bg.g * ib) * k,
            (fg.b * srcA + bg.b * ib) * k, outA};
}

// ---- ImagePattern tile sampling (LE-d2) -------------------------------------------------------
// A bitmap tile is a fixed-resolution image sampled with SEAMLESS WRAP, so it tiles edge-to-edge with
// no store and no I/O (samplePattern stays pure). `sx,sy` are continuous coordinates in native tile
// pixels (pixel i spans [i,i+1), centre at i+0.5); they wrap modulo the tile size so any coordinate
// (incl. negative) maps into the tile. `antialias` picks the filter: bilinear (soft) when on, nearest-
// neighbour (crisp/pixelated) when off -- the same document-wide AA choice the procedural kinds honour.
// Bilinear interpolates in PREMULTIPLIED alpha so a transparent texel never bleeds its RGB across an
// edge (correct for from-selection tiles that carry alpha); the result is returned straight-alpha.

// One tile texel as straight-alpha float, with wrap-around indexing (correct for negatives).
ColorF tileTexel(const common::Image& t, int x, int y) {
    const int tw = static_cast<int>(t.width), th = static_cast<int>(t.height);
    int wx = x % tw;
    if (wx < 0) wx += tw;
    int wy = y % th;
    if (wy < 0) wy += th;
    const std::size_t o = (static_cast<std::size_t>(wy) * tw + wx) * 4;
    return {t.rgba[o] / 255.0f, t.rgba[o + 1] / 255.0f, t.rgba[o + 2] / 255.0f, t.rgba[o + 3] / 255.0f};
}

ColorF sampleImageTile(const common::Image& t, double sx, double sy, bool antialias) {
    if (t.width == 0 || t.height == 0) return {0, 0, 0, 0};
    if (!antialias) {  // nearest-neighbour: pixel i covers [i,i+1)
        return tileTexel(t, static_cast<int>(std::floor(sx)), static_cast<int>(std::floor(sy)));
    }
    // Bilinear about texel centres (i+0.5), interpolating premultiplied alpha.
    const double fx = sx - 0.5, fy = sy - 0.5;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float wx = static_cast<float>(fx - x0), wy = static_cast<float>(fy - y0);
    const auto pm = [](ColorF c) { return ColorF{c.r * c.a, c.g * c.a, c.b * c.a, c.a}; };
    const ColorF c00 = pm(tileTexel(t, x0, y0)), c10 = pm(tileTexel(t, x0 + 1, y0));
    const ColorF c01 = pm(tileTexel(t, x0, y0 + 1)), c11 = pm(tileTexel(t, x0 + 1, y0 + 1));
    const auto lerp = [](float a, float b, float w) { return a + (b - a) * w; };
    const auto lerpC = [&](const ColorF& a, const ColorF& b, float w) {
        return ColorF{lerp(a.r, b.r, w), lerp(a.g, b.g, w), lerp(a.b, b.b, w), lerp(a.a, b.a, w)};
    };
    const ColorF top = lerpC(c00, c10, wx), bot = lerpC(c01, c11, wx);
    const ColorF pr = lerpC(top, bot, wy);  // premultiplied result
    if (pr.a <= 1e-8f) return {0, 0, 0, 0};
    const float inv = 1.0f / pr.a;
    return {pr.r * inv, pr.g * inv, pr.b * inv, std::clamp(pr.a, 0.0f, 1.0f)};
}

}  // namespace

const char* patternKindName(ProceduralPattern::Kind kind) {
    using K = ProceduralPattern::Kind;
    switch (kind) {
        case K::Dots: return "Dots";
        case K::Grid: return "Grid";
        case K::Lines: return "Lines";
        case K::Hatch: return "Hatch";
        case K::CrossHatch: return "Cross-hatch";
        case K::Checker: return "Checker";
        case K::Herringbone: return "Herringbone";
        case K::Parquet: return "Parquet";
        case K::Basketweave: return "Basketweave";
        case K::Chevron: return "Chevron";
        case K::Zigzag: return "Zigzag";
        case K::Chainmail: return "Chainmail";
        case K::Halftone: return "Halftone";
        case K::Grain: return "Grain";
        case K::Bricks: return "Bricks";
        case K::Triangles: return "Triangles";
        case K::Sawtooth: return "Sawtooth";
        case K::Harlequin: return "Harlequin";
        case K::Honeycomb: return "Honeycomb";
        case K::Hearts: return "Hearts";
        case K::StarAnise: return "Star Anise";
        case K::Stars: return "Stars";
        case K::Waves: return "Waves";
        case K::Crosses: return "Crosses";
        case K::Rings: return "Rings";
    }
    return "Dots";
}

ColorF samplePattern(const Pattern& pattern, Vec2 layerPx, bool antialias) {
    if (const auto* ip = std::get_if<ImagePattern>(&pattern)) {
        // LE-d2: sample the held bitmap tile with seamless wrap. Null/empty -> transparent.
        if (!ip->tile || ip->tile->empty()) return {0, 0, 0, 0};
        const double scale = std::max(1e-3, static_cast<double>(ip->scale));
        const Vec2 r = rotate(layerPx, -static_cast<double>(ip->angleDeg));
        // Layer px -> native tile px (scale multiplies native px), then a phase shift in whole tiles.
        const double sx = r.x / scale + static_cast<double>(ip->offset.x) * ip->tile->width;
        const double sy = r.y / scale + static_cast<double>(ip->offset.y) * ip->tile->height;
        return sampleImageTile(*ip->tile, sx, sy, antialias);
    }
    const ProceduralPattern& p = std::get<ProceduralPattern>(pattern);
    const double scale = std::max(1e-3, static_cast<double>(p.scale));
    const Vec2 r = rotate(layerPx, -static_cast<double>(p.angleDeg));
    const double off = std::clamp(static_cast<double>(p.offset), 0.0, 1.0);  // phase, in tiles
    // Checker only: shift the phase ~0.75px into the first cell so the shape's top/left silhouette edge
    // (u==0 = the content corner) samples at FULL coverage rather than on a cell boundary's 1px AA
    // ramp, which would otherwise read as a faint seam along the top/left. Sub-pixel, so the corner
    // square still looks full; clamped below half a cell for tiny scales. Other kinds (thin lines/grids)
    // don't need it -- only the checker's large 2-colour cells make an edge-aligned boundary a visible seam.
    const double edgeNudge =
        p.kind == ProceduralPattern::Kind::Checker ? std::min(0.15, 0.75 / scale) : 0.0;
    const Vec2 u{r.x / scale + off + edgeNudge, r.y / scale + off + edgeNudge};
    const double sdUnit = kindSD(p.kind, u, std::clamp(static_cast<double>(p.weight), 0.0, 1.0),
                                 static_cast<double>(p.spacing));
    // Every kind (Grain included) honours the document-wide AA setting: crisp = a hard 0/1 threshold
    // at the contour, AA = the 1px linear coverage ramp centred on it.
    const float cov = !antialias ? (sdUnit >= 0.0 ? 1.0f : 0.0f)
                                 : static_cast<float>(std::clamp(sdUnit * scale + 0.5, 0.0, 1.0));
    return fgOverBg(p.fg, p.bg, cov);
}

bool patternUsesWeight(ProceduralPattern::Kind kind) {
    using K = ProceduralPattern::Kind;
    switch (kind) {
        // Gapless tessellations + fixed-shape motifs have no thickness knob.
        case K::Checker:
        case K::Triangles:
        case K::Sawtooth:
        case K::Harlequin:
        case K::Hearts:
        case K::StarAnise:
        case K::Stars: return false;
        default: return true;
    }
}

bool patternUsesSpacing(ProceduralPattern::Kind kind) {
    using K = ProceduralPattern::Kind;
    // Only the fixed-shape motifs on a lattice have a meaningful inter-element gap.
    return kind == K::Hearts || kind == K::StarAnise || kind == K::Stars;
}

ImagePattern makeImagePattern(common::Image tile) {
    ImagePattern p;
    if (!tile.empty()) p.tile = std::make_shared<const common::Image>(std::move(tile));
    return p;  // empty source -> null tile (reads transparent)
}

ImagePattern makeImagePattern(const common::Image& src, long x, long y, std::uint32_t w,
                              std::uint32_t h) {
    if (w == 0 || h == 0 || src.empty()) return {};  // null tile
    return makeImagePattern(common::copyRegion(src, x, y, w, h));
}

}  // namespace mosaic::core::vec
