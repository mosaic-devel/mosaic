#include "core/selection.hpp"

#include "core/layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace mosaic::core {

Selection::Selection(std::uint32_t width, std::uint32_t height)
    : m_width(width), m_height(height),
      m_data(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0) {}

Selection Selection::rectangle(std::uint32_t docW, std::uint32_t docH, common::Rect r) {
    Selection s(docW, docH);
    const int x0 = std::clamp(static_cast<int>(std::lround(r.x)), 0, static_cast<int>(docW));
    const int y0 = std::clamp(static_cast<int>(std::lround(r.y)), 0, static_cast<int>(docH));
    const int x1 = std::clamp(static_cast<int>(std::lround(r.right())), 0, static_cast<int>(docW));
    const int y1 = std::clamp(static_cast<int>(std::lround(r.bottom())), 0, static_cast<int>(docH));
    for (int y = y0; y < y1; ++y) {
        auto* row = s.m_data.data() + static_cast<std::size_t>(y) * docW;
        std::fill(row + x0, row + x1, std::uint8_t{255});
    }
    return s;
}

Selection Selection::polygon(std::uint32_t docW, std::uint32_t docH,
                             const std::vector<common::Vec2>& points) {
    Selection s(docW, docH);
    const std::size_t n = points.size();
    if (n < 3 || docW == 0 || docH == 0)
        return s;

    double minY = points[0].y;
    double maxY = points[0].y;
    for (const common::Vec2& p : points) {
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    const int y0 = std::clamp(static_cast<int>(std::floor(minY)), 0, static_cast<int>(docH));
    const int y1 = std::clamp(static_cast<int>(std::ceil(maxY)), 0, static_cast<int>(docH));

    // Scanline even-odd fill with kSubScan sub-scanlines per pixel row and exact fractional
    // horizontal span ends: vertical AA is 1/kSubScan-quantised, horizontal AA is analytic.
    // O(rows * kSubScan * edges) -- fine for marquee-scale polygons; an active-edge table is the
    // upgrade path if huge free-lasso paths ever make this noticeable (it runs once, on commit).
    constexpr int kSubScan = 8;
    std::vector<double> xs;          // edge crossings of one sub-scanline
    std::vector<double> acc(docW);   // coverage accumulator for one pixel row, 0..1
    for (int y = y0; y < y1; ++y) {
        std::fill(acc.begin(), acc.end(), 0.0);
        bool any = false;
        for (int k = 0; k < kSubScan; ++k) {
            const double sy = y + (k + 0.5) / kSubScan;
            xs.clear();
            for (std::size_t i = 0; i < n; ++i) {
                const common::Vec2 a = points[i];
                const common::Vec2 b = points[(i + 1) % n];
                if ((a.y <= sy) == (b.y <= sy))
                    continue; // half-open in y: a vertex on the line counts exactly once
                xs.push_back(a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x));
            }
            std::sort(xs.begin(), xs.end());
            for (std::size_t i = 0; i + 1 < xs.size(); i += 2) { // even-odd span pairs
                const double sx0 = std::max(xs[i], 0.0);
                const double sx1 = std::min(xs[i + 1], static_cast<double>(docW));
                if (sx1 <= sx0)
                    continue;
                any = true;
                const int ix0 = static_cast<int>(std::floor(sx0));
                const int ix1 = static_cast<int>(std::ceil(sx1));
                for (int x = ix0; x < ix1; ++x)
                    acc[x] += std::min(sx1, x + 1.0) - std::max(sx0, static_cast<double>(x));
            }
        }
        if (!any)
            continue;
        auto* row = s.m_data.data() + static_cast<std::size_t>(y) * docW;
        for (std::uint32_t x = 0; x < docW; ++x) {
            if (acc[x] > 0.0)
                row[x] = static_cast<std::uint8_t>(
                    std::lround(std::min(acc[x] / kSubScan, 1.0) * 255.0));
        }
    }
    return s;
}

Selection Selection::ellipse(std::uint32_t docW, std::uint32_t docH, common::Rect r) {
    if (r.empty())
        return Selection(docW, docH);
    constexpr double kPi = 3.14159265358979323846;
    const double rx = r.w * 0.5;
    const double ry = r.h * 0.5;
    const common::Vec2 c = r.center();
    // Tessellate into enough segments that the chord sagitta stays under ~1/16 px
    // (n >= pi*sqrt(8*rmax)), then reuse the polygon rasteriser (convex, so even-odd is exact).
    // Rounded up to a multiple of 4 so the vertex ring keeps the ellipse's 4-way symmetry.
    const double rmax = std::max(rx, ry);
    int segments = std::clamp(static_cast<int>(std::ceil(kPi * std::sqrt(8.0 * rmax))), 32, 4096);
    segments = (segments + 3) / 4 * 4;
    std::vector<common::Vec2> pts;
    pts.reserve(static_cast<std::size_t>(segments));
    for (int i = 0; i < segments; ++i) {
        const double a = 2.0 * kPi * i / segments;
        pts.push_back({c.x + rx * std::cos(a), c.y + ry * std::sin(a)});
    }
    return polygon(docW, docH, pts);
}

Selection Selection::combine(const Selection& a, const Selection& b, SelectOp op) {
    if (op == SelectOp::Replace || a.isEmpty()) {
        // Replace adopts b outright. For the other ops an empty `a` acts as a zero mask:
        // Add(0,b) = b; Subtract(0,b) = 0 = nothing; Intersect(0,b) = 0.
        switch (op) {
        case SelectOp::Replace:
        case SelectOp::Add:
            return b;
        case SelectOp::Subtract:
        case SelectOp::Intersect:
            return b.isEmpty() ? Selection{} : Selection(b.m_width, b.m_height);
        }
    }
    if (b.isEmpty())
        return op == SelectOp::Intersect ? Selection(a.m_width, a.m_height) : a;

    Selection out(a.m_width, a.m_height);
    const std::size_t n = a.m_data.size();
    for (std::size_t i = 0; i < n; ++i) {
        const int av = a.m_data[i];
        const int bv = b.m_data[i];
        int v = 0;
        switch (op) {
        case SelectOp::Add:
            v = std::max(av, bv);
            break;
        case SelectOp::Subtract:
            v = av * (255 - bv) / 255;
            break;
        case SelectOp::Intersect:
            v = std::min(av, bv);
            break;
        case SelectOp::Replace:
            v = bv; // unreachable (handled above); kept for switch completeness
            break;
        }
        out.m_data[i] = static_cast<std::uint8_t>(v);
    }
    return out;
}

std::uint8_t Selection::at(std::uint32_t x, std::uint32_t y) const noexcept {
    if (x >= m_width || y >= m_height || m_data.empty())
        return 0;
    return m_data[static_cast<std::size_t>(y) * m_width + x];
}

bool Selection::anySelected() const noexcept {
    return std::any_of(m_data.begin(), m_data.end(), [](std::uint8_t v) { return v > 0; });
}

Selection Selection::inverted() const {
    if (isEmpty())
        return {};
    Selection out(m_width, m_height);
    std::transform(m_data.begin(), m_data.end(), out.m_data.begin(),
                   [](std::uint8_t v) { return static_cast<std::uint8_t>(255 - v); });
    return out;
}

namespace {

// --- Select-menu morphology (S18, docs/research-select-brush.md §4) ---------------------------
// A tiny separable-blur + exact-EDT toolkit on the 8-bit coverage mask, kept here (not in
// `render`) so grow/shrink/feather/smooth stay pure `core` and the command is headless-testable.

constexpr double kInf = 1e20;

inline double clampCov(double c) noexcept { return c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c); }

// One-dimensional squared distance transform of the sampled function `f` (Felzenszwalb & Huttenlocher,
// "Distance Transforms of Sampled Functions", 2004 -- classical): the lower envelope of
// the parabolas rooted at each sample, written into `d`. Scratch `v`/`z` are the caller's to avoid
// per-row allocation. O(n).
void edt1d(const std::vector<double>& f, std::vector<double>& d, std::vector<int>& v,
           std::vector<double>& z, int n) {
    int k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    for (int q = 1; q < n; ++q) {
        double s = ((f[q] + static_cast<double>(q) * q) -
                    (f[v[k]] + static_cast<double>(v[k]) * v[k])) /
                   (2.0 * q - 2.0 * v[k]);
        while (s <= z[k]) {
            --k;
            s = ((f[q] + static_cast<double>(q) * q) -
                 (f[v[k]] + static_cast<double>(v[k]) * v[k])) /
                (2.0 * q - 2.0 * v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kInf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q)
            ++k;
        const double dx = q - v[k];
        d[q] = dx * dx + f[v[k]];
    }
}

// Exact Euclidean distance (px) from every pixel to the nearest `seed` pixel, separable 2-pass FH.
// `seed[i] != 0` marks a source; a pixel that is itself a seed reads 0. An empty seed set yields
// +inf everywhere. Distances beyond a caller-relevant band are still exact (we only threshold later).
std::vector<double> exactEdt(const std::vector<char>& seed, int w, int h) {
    std::vector<double> grid(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0, n = grid.size(); i < n; ++i)
        grid[i] = seed[i] ? 0.0 : kInf;

    const int maxDim = std::max(w, h);
    std::vector<double> f(maxDim), d(maxDim), z(maxDim + 1);
    std::vector<int> vv(maxDim);

    // Columns.
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y)
            f[y] = grid[static_cast<std::size_t>(y) * w + x];
        edt1d(f, d, vv, z, h);
        for (int y = 0; y < h; ++y)
            grid[static_cast<std::size_t>(y) * w + x] = d[y];
    }
    // Rows.
    for (int y = 0; y < h; ++y) {
        auto* row = grid.data() + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x)
            f[x] = row[x];
        edt1d(f, d, vv, z, w);
        for (int x = 0; x < w; ++x)
            row[x] = d[x];
    }
    for (double& v : grid)
        v = v >= kInf ? kInf : std::sqrt(v);
    return grid;
}

// A truncated normalized Gaussian kernel for `sigma` px (radius = ceil(3*sigma), clamped so a tiny
// sigma still blurs at least its immediate neighbours).
std::vector<double> gaussianKernel(double sigma) {
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0 * sigma)));
    std::vector<double> k(static_cast<std::size_t>(2 * radius + 1));
    const double twoSigmaSq = 2.0 * sigma * sigma;
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double v = std::exp(-(i * i) / twoSigmaSq);
        k[static_cast<std::size_t>(i + radius)] = v;
        sum += v;
    }
    for (double& v : k)
        v /= sum;
    return k;
}

// One box-blur pass of half-width `r` along the rows, edge-clamped, via a sliding running sum: each
// output reads the window sum in O(1), so the pass is O(w*h) INDEPENDENT of r. The window centred at
// x spans [x-r, x+r] with out-of-range indices replicated from the nearest edge (the same border
// policy as the exact kernel: a selection touching the document edge feathers inward). Input and
// output must be distinct buffers.
void boxBlurRows(const std::vector<double>& src, std::vector<double>& dst, int w, int h, int r) {
    const double norm = 1.0 / (2 * r + 1);
    for (int y = 0; y < h; ++y) {
        const auto base = static_cast<std::size_t>(y) * w;
        // Seed the window at x=0: indices -r..0 all clamp to column 0 (r+1 copies), then 1..r.
        double sum = src[base] * (r + 1);
        for (int i = 1; i <= r; ++i)
            sum += src[base + static_cast<std::size_t>(std::min(i, w - 1))];
        for (int x = 0; x < w; ++x) {
            dst[base + static_cast<std::size_t>(x)] = sum * norm;
            const int add = std::min(x + r + 1, w - 1); // pixel entering on the right
            const int sub = std::max(x - r, 0);         // pixel leaving on the left
            sum += src[base + static_cast<std::size_t>(add)] -
                   src[base + static_cast<std::size_t>(sub)];
        }
    }
}

// The column twin of boxBlurRows (stride w). Same running-sum, same edge-clamp, O(w*h).
void boxBlurCols(const std::vector<double>& src, std::vector<double>& dst, int w, int h, int r) {
    const double norm = 1.0 / (2 * r + 1);
    for (int x = 0; x < w; ++x) {
        const auto at = [&](int y) { return static_cast<std::size_t>(y) * w + x; };
        double sum = src[at(0)] * (r + 1);
        for (int i = 1; i <= r; ++i)
            sum += src[at(std::min(i, h - 1))];
        for (int y = 0; y < h; ++y) {
            dst[at(y)] = sum * norm;
            const int add = std::min(y + r + 1, h - 1);
            const int sub = std::max(y - r, 0);
            sum += src[at(add)] - src[at(sub)];
        }
    }
}

// A three-pass box-blur approximation of a Gaussian of standard deviation `sigma` (the classic
// central-limit approximation): a box of half-width r has variance (r^2 + r)/3, so three of them
// sum to r^2 + r; solving r^2 + r = sigma^2 for r matches the target variance. Cost is O(w*h) with
// NO dependence on the radius -- the whole point: a 1000 px feather costs the same as a 10 px one.
std::vector<double> gaussianBlurBox(const std::vector<double>& src, int w, int h, double sigma) {
    int r = static_cast<int>(std::lround((-1.0 + std::sqrt(1.0 + 4.0 * sigma * sigma)) / 2.0));
    r = std::clamp(r, 1, std::max(w, h)); // a box wider than the plane already averages all of it
    std::vector<double> a = src;           // ping
    std::vector<double> b(src.size());     // pong
    for (int pass = 0; pass < 3; ++pass) {
        boxBlurRows(a, b, w, h, r); // a -> b
        boxBlurCols(b, a, w, h, r); // b -> a (result lands back in `a` after each full pass)
    }
    return a;
}

// Feather/Smooth's Gaussian. The exact truncated kernel below is O(w*h*sigma): with the amount
// slider's large radii (sigma up to 1000 -> a ~6000-tap kernel) that is 10^11+ multiply-adds on a
// normal-sized selection and freezes the UI for minutes -- the "amount slider hangs the program"
// report. Above a small sigma we hand off to the O(w*h) box approximation (visually identical for a
// feather halo). Small sigma keeps the exact kernel: box widths that small approximate a Gaussian
// poorly, and it is the ONLY path the morphology unit tests exercise (radii <= 6), so their output
// stays byte-for-byte unchanged.
constexpr double kExactGaussianSigmaMax = 6.0;

// Separable Gaussian blur of a float coverage plane [0,1], clamping at the edges (so a selection
// touching the document border feathers inward rather than darkening against a phantom zero band).
std::vector<double> gaussianBlur(const std::vector<double>& src, int w, int h, double sigma) {
    if (sigma > kExactGaussianSigmaMax)
        return gaussianBlurBox(src, w, h, sigma);
    const std::vector<double> k = gaussianKernel(sigma);
    const int radius = static_cast<int>(k.size() / 2);
    std::vector<double> tmp(src.size(), 0.0);
    std::vector<double> out(src.size(), 0.0);
    // Horizontal.
    for (int y = 0; y < h; ++y) {
        const auto* srow = src.data() + static_cast<std::size_t>(y) * w;
        auto* trow = tmp.data() + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            double acc = 0.0;
            for (int i = -radius; i <= radius; ++i) {
                const int sx = std::clamp(x + i, 0, w - 1);
                acc += srow[sx] * k[static_cast<std::size_t>(i + radius)];
            }
            trow[x] = acc;
        }
    }
    // Vertical.
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
            double acc = 0.0;
            for (int i = -radius; i <= radius; ++i) {
                const int sy = std::clamp(y + i, 0, h - 1);
                acc +=
                    tmp[static_cast<std::size_t>(sy) * w + x] * k[static_cast<std::size_t>(i + radius)];
            }
            out[static_cast<std::size_t>(y) * w + x] = acc;
        }
    }
    return out;
}

} // namespace

Selection Selection::offsetBy(double delta) const {
    if (isEmpty())
        return {};
    const int w = static_cast<int>(m_width);
    const int h = static_cast<int>(m_height);
    const std::size_t n = m_data.size();

    // Binarize at the 0.5 iso-contour (coverage >= 128 = inside), then two exact EDTs: distance to
    // the nearest inside pixel (for outside pixels) and to the nearest outside pixel (for inside
    // ones). The signed distance places the boundary between adjacent inside/outside centres.
    std::vector<char> inside(n), outside(n);
    for (std::size_t i = 0; i < n; ++i) {
        const bool in = m_data[i] >= kAntsCoverageThreshold;
        inside[i] = in ? 1 : 0;
        outside[i] = in ? 0 : 1;
    }
    const std::vector<double> distToInside = exactEdt(inside, w, h);   // >0 on outside pixels
    const std::vector<double> distToOutside = exactEdt(outside, w, h); // >0 on inside pixels

    Selection out(m_width, m_height);
    for (std::size_t i = 0; i < n; ++i) {
        // sd < 0 inside, > 0 outside, |sd| ~ distance to the 0.5 contour (the -0.5 lands the
        // zero-crossing midway between the nearest inside and outside pixel centres).
        const double sd = inside[i] ? (0.5 - distToOutside[i]) : (distToInside[i] - 0.5);
        // New boundary at sd == delta; a 1-px linear ramp across it keeps a clean AA edge.
        const double cov = clampCov(0.5 + delta - sd);
        out.m_data[i] = static_cast<std::uint8_t>(std::lround(cov * 255.0));
    }
    if (!out.anySelected())
        return {}; // shrank (or grew a fringe) to nothing -> "no selection"
    return out;
}

Selection Selection::grown(int px) const {
    if (isEmpty() || px <= 0)
        return *this;
    return offsetBy(static_cast<double>(px));
}

Selection Selection::shrunk(int px) const {
    if (isEmpty() || px <= 0)
        return *this;
    return offsetBy(-static_cast<double>(px));
}

Selection Selection::feathered(double radius) const {
    if (isEmpty() || radius <= 0.0)
        return *this;
    const int w = static_cast<int>(m_width);
    const int h = static_cast<int>(m_height);
    std::vector<double> cov(m_data.size());
    for (std::size_t i = 0, n = m_data.size(); i < n; ++i)
        cov[i] = m_data[i] / 255.0;
    const std::vector<double> blurred = gaussianBlur(cov, w, h, radius);
    Selection out(m_width, m_height);
    for (std::size_t i = 0, n = out.m_data.size(); i < n; ++i)
        out.m_data[i] = static_cast<std::uint8_t>(std::lround(clampCov(blurred[i]) * 255.0));
    if (!out.anySelected())
        return {};
    return out;
}

Selection Selection::smoothed(double radius) const {
    if (isEmpty() || radius <= 0.0)
        return *this;
    const int w = static_cast<int>(m_width);
    const int h = static_cast<int>(m_height);
    std::vector<double> cov(m_data.size());
    for (std::size_t i = 0, n = m_data.size(); i < n; ++i)
        cov[i] = m_data[i] / 255.0;
    // Blur by the radius: features (islands / necks) smaller than it wash below the 0.5 crossing and
    // vanish, and jagged edges round off (§4.4). The blur leaves a wide, soft transition; a linear
    // remap about 0.5 pulls it back to a ~1-px AA edge at the crossing -- the §4.2 ramp derived
    // straight from the blurred field's slope (~0.4/sigma per px), so the crossing stays anti-aliased
    // rather than re-quantized to binary.
    const std::vector<double> blurred = gaussianBlur(cov, w, h, radius);
    const double gain = 2.5 * radius; // sigma / 0.4: restores a ~1-px edge independent of radius
    Selection out(m_width, m_height);
    for (std::size_t i = 0, n = out.m_data.size(); i < n; ++i) {
        const double c = clampCov(0.5 + (blurred[i] - 0.5) * gain);
        out.m_data[i] = static_cast<std::uint8_t>(std::lround(c * 255.0));
    }
    if (!out.anySelected())
        return {};
    return out;
}

Selection Selection::cropped(long offX, long offY, std::uint32_t newW,
                             std::uint32_t newH) const {
    if (isEmpty() || newW == 0 || newH == 0)
        return {};
    Selection out(newW, newH);
    // Copy the overlap of the window [off, off+new) with [0, old) row by row.
    const long x0 = std::max(0L, offX);
    const long x1 = std::min<long>(m_width, offX + static_cast<long>(newW));
    const long y0 = std::max(0L, offY);
    const long y1 = std::min<long>(m_height, offY + static_cast<long>(newH));
    for (long y = y0; y < y1; ++y) {
        const auto* src = m_data.data() + static_cast<std::size_t>(y) * m_width + x0;
        auto* dst = out.m_data.data() + static_cast<std::size_t>(y - offY) * newW + (x0 - offX);
        std::copy_n(src, static_cast<std::size_t>(x1 - x0), dst);
    }
    if (!out.anySelected())
        return {}; // the crop removed all coverage: back to "no selection"
    return out;
}

Selection Selection::remapped(const common::Affine2D& docToNew, std::uint32_t newW,
                              std::uint32_t newH) const {
    if (isEmpty() || newW == 0 || newH == 0)
        return {};
    const std::optional<common::Affine2D> inv = docToNew.inverse();
    if (!inv)
        return {}; // a singular remap shows nothing
    Selection out(newW, newH);
    for (std::uint32_t y = 0; y < newH; ++y) {
        auto* row = out.m_data.data() + static_cast<std::size_t>(y) * newW;
        for (std::uint32_t x = 0; x < newW; ++x) {
            const common::Vec2 p =
                inv->apply({static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5});
            const long sx = static_cast<long>(std::floor(p.x));
            const long sy = static_cast<long>(std::floor(p.y));
            if (sx < 0 || sy < 0 || sx >= static_cast<long>(m_width) ||
                sy >= static_cast<long>(m_height))
                continue; // outside the old mask: uncovered
            row[x] = m_data[static_cast<std::size_t>(sy) * m_width + static_cast<std::size_t>(sx)];
        }
    }
    if (!out.anySelected())
        return {}; // the remap carried no coverage onto the new canvas
    return out;
}

Selection Selection::scaled(std::uint32_t newW, std::uint32_t newH) const {
    if (isEmpty() || newW == 0 || newH == 0)
        return {};
    if (newW == m_width && newH == m_height)
        return *this;
    // Source units per destination pixel. The footprint half-width is half of that, floored at 0.5
    // so an ENLARGEMENT still spans one whole source pixel -- a unit-wide box average is exactly
    // linear interpolation, which is what keeps an upscaled AA edge smooth (header contract).
    const double sx = static_cast<double>(m_width) / static_cast<double>(newW);
    const double sy = static_cast<double>(m_height) / static_cast<double>(newH);
    const double hx = std::max(0.5, 0.5 * sx);
    const double hy = std::max(0.5, 0.5 * sy);
    Selection out(newW, newH);
    for (std::uint32_t y = 0; y < newH; ++y) {
        const double cy = (static_cast<double>(y) + 0.5) * sy;
        const double y0 = cy - hy, y1 = cy + hy;
        const long iy0 = static_cast<long>(std::floor(y0));
        const long iy1 = static_cast<long>(std::ceil(y1)) - 1;
        auto* row = out.m_data.data() + static_cast<std::size_t>(y) * newW;
        for (std::uint32_t x = 0; x < newW; ++x) {
            const double cx = (static_cast<double>(x) + 0.5) * sx;
            const double x0 = cx - hx, x1 = cx + hx;
            const long ix0 = static_cast<long>(std::floor(x0));
            const long ix1 = static_cast<long>(std::ceil(x1)) - 1;
            double acc = 0.0, wsum = 0.0;
            for (long ty = iy0; ty <= iy1; ++ty) {
                const double wy = std::min(y1, static_cast<double>(ty) + 1.0) -
                                  std::max(y0, static_cast<double>(ty));
                if (wy <= 0.0)
                    continue;
                // Edge taps clamp into the mask instead of reading zero (header contract).
                const long cyi = std::clamp<long>(ty, 0, static_cast<long>(m_height) - 1);
                for (long tx = ix0; tx <= ix1; ++tx) {
                    const double wx = std::min(x1, static_cast<double>(tx) + 1.0) -
                                      std::max(x0, static_cast<double>(tx));
                    if (wx <= 0.0)
                        continue;
                    const long cxi = std::clamp<long>(tx, 0, static_cast<long>(m_width) - 1);
                    const double w = wx * wy;
                    acc += w * static_cast<double>(
                                   m_data[static_cast<std::size_t>(cyi) * m_width +
                                          static_cast<std::size_t>(cxi)]);
                    wsum += w;
                }
            }
            if (wsum <= 0.0)
                continue;
            row[x] = static_cast<std::uint8_t>(std::clamp(acc / wsum + 0.5, 0.0, 255.0));
        }
    }
    if (!out.anySelected())
        return {}; // the scale erased every covered pixel
    return out;
}

std::optional<common::Rect> Selection::bounds() const {
    std::uint32_t minX = m_width;
    std::uint32_t minY = m_height;
    std::uint32_t maxX = 0;
    std::uint32_t maxY = 0;
    bool any = false;
    for (std::uint32_t y = 0; y < m_height; ++y) {
        const auto* row = m_data.data() + static_cast<std::size_t>(y) * m_width;
        for (std::uint32_t x = 0; x < m_width; ++x) {
            if (row[x] > 0) {
                any = true;
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (!any)
        return std::nullopt;
    return common::Rect{static_cast<double>(minX), static_cast<double>(minY),
                        static_cast<double>(maxX - minX + 1), static_cast<double>(maxY - minY + 1)};
}

Selection Selection::translated(long dx, long dy) const {
    if (isEmpty())
        return {};
    // A shift by (dx,dy) is the same-sized window taken at (-dx,-dy) -- cropped() already copies
    // the overlap row by row and collapses a coverage-free result to "no selection". Short-circuit
    // a shift that clears the document outright so the offsets can never overflow the negation.
    if (std::abs(dx) >= static_cast<long>(m_width) || std::abs(dy) >= static_cast<long>(m_height))
        return {};
    return cropped(-dx, -dy, m_width, m_height);
}

namespace {

// --- Magic wand (S17) ------------------------------------------------------------------------
// Luma weights (ITU-R BT.601) for the colour channels, and a lighter colour-vs-alpha split so a
// click on a transparent area forms its own region without alpha swamping the colour match. Each
// group sums to 1, so wandColorDistance stays in [0,1] whether or not alpha is included.
constexpr double kWr = 0.30, kWg = 0.59, kWb = 0.11;
constexpr double kWandColorW = 0.80, kWandAlphaW = 0.20;

// Half-width of the anti-alias band, in the normalised metric's units (research §5). A tunable
// constant, not a magic threshold: the ramp math is unit-tested; the *feel* is owed a visual pass.
constexpr double kWandAaBand = 0.02;

inline common::Color8 pixelAt(const common::Image& img, int x, int y) noexcept {
    const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3]};
}

inline std::uint8_t coverageToByte(double c) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0, 1.0) * 255.0));
}

// The soft coverage a pixel at metric distance `d` earns for tolerance `T` (research §5): 1 solidly
// inside, a linear ramp through 0.5 at d==T, 0 solidly outside. With antialias off it is a hard step.
inline double wandCoverage(double d, double T, bool antialias) noexcept {
    if (!antialias)
        return d <= T ? 1.0 : 0.0;
    if (d <= T - kWandAaBand)
        return 1.0;
    if (d >= T + kWandAaBand)
        return 0.0;
    return (T + kWandAaBand - d) / (2.0 * kWandAaBand);
}

// 4-connected scanline (span) flood: fills whole row runs at once and only seeds the run-starts in
// the rows above/below, so the worklist tracks spans not pixels (research §4). `in` is the hard
// tolerance predicate over the whole image (precomputed once); `filled` is the visited/interior set.
void scanlineFlood(int seedX, int seedY, int w, int h, const std::vector<char>& in,
                   std::vector<char>& filled) {
    const auto idx = [w](int x, int y) { return static_cast<std::size_t>(y) * w + x; };
    std::vector<std::pair<int, int>> stack;
    stack.emplace_back(seedX, seedY);
    while (!stack.empty()) {
        const auto [px, py] = stack.back();
        stack.pop_back();
        if (filled[idx(px, py)] || !in[idx(px, py)])
            continue; // a redundant seed a prior span already swallowed
        int lx = px;
        while (lx > 0 && !filled[idx(lx - 1, py)] && in[idx(lx - 1, py)])
            --lx;
        int rx = px;
        while (rx < w - 1 && !filled[idx(rx + 1, py)] && in[idx(rx + 1, py)])
            ++rx;
        for (int x = lx; x <= rx; ++x)
            filled[idx(x, py)] = 1;
        for (const int ny : {py - 1, py + 1}) {
            if (ny < 0 || ny >= h)
                continue;
            int x = lx;
            while (x <= rx) {
                if (filled[idx(x, ny)] || !in[idx(x, ny)]) {
                    ++x;
                    continue;
                }
                stack.emplace_back(x, ny); // a new run start; skip to its end so we push it once
                ++x;
                while (x <= rx && !filled[idx(x, ny)] && in[idx(x, ny)])
                    ++x;
            }
        }
    }
}

} // namespace

double wandColorDistance(common::Color8 a, common::Color8 b, bool useAlpha) noexcept {
    const double dr = (static_cast<double>(a.r) - b.r) / 255.0;
    const double dg = (static_cast<double>(a.g) - b.g) / 255.0;
    const double db = (static_cast<double>(a.b) - b.b) / 255.0;
    const double colorSq = kWr * dr * dr + kWg * dg * dg + kWb * db * db; // in [0,1]
    if (!useAlpha)
        return std::sqrt(colorSq);
    const double da = (static_cast<double>(a.a) - b.a) / 255.0;
    return std::sqrt(kWandColorW * colorSq + kWandAlphaW * da * da); // in [0,1]
}

Selection magicWandSelection(const common::Image& src, int seedX, int seedY,
                             const WandParams& params) {
    const int w = static_cast<int>(src.width);
    const int h = static_cast<int>(src.height);
    if (src.empty() || seedX < 0 || seedY < 0 || seedX >= w || seedY >= h)
        return {}; // no valid seed -> "no selection"

    Selection out(src.width, src.height);
    auto& mask = out.data();
    const common::Color8 seed = pixelAt(src, seedX, seedY);
    const double T = std::clamp(params.tolerance, 0.0, 1.0);
    const bool aa = params.antialias;
    const bool alpha = params.sampleAlpha;
    const auto idx = [w](int x, int y) { return static_cast<std::size_t>(y) * w + x; };
    const auto dist = [&](int x, int y) {
        return wandColorDistance(pixelAt(src, x, y), seed, alpha);
    };

    if (!params.contiguous) {
        // Global match: no connectivity -- apply the predicate/ramp straight to every pixel.
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double c = wandCoverage(dist(x, y), T, aa);
                if (c > 0.0)
                    mask[idx(x, y)] = coverageToByte(c);
            }
    } else {
        // Contiguous: a HARD flood (d <= T) gives the solid interior; the AA edge is a one-pixel
        // ramp so the soft band cannot bridge to a disconnected same-colour region (research §5.1).
        std::vector<char> in(static_cast<std::size_t>(w) * h, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                in[idx(x, y)] = dist(x, y) <= T ? 1 : 0;
        std::vector<char> filled(static_cast<std::size_t>(w) * h, 0);
        scanlineFlood(seedX, seedY, w, h, in, filled);

        if (!aa) {
            for (std::size_t i = 0, n = filled.size(); i < n; ++i)
                if (filled[i])
                    mask[i] = 255;
        } else {
            // Interior: soften each selected pixel to [128,255] (it stays inside the ants' >=128
            // set). Outer ring: a sub-128 fringe on the interior's unselected 4-neighbours -- never
            // >=128, so it neither shows ants nor leaks the flood into the next region.
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (!filled[idx(x, y)])
                        continue;
                    const double c = wandCoverage(dist(x, y), T, /*antialias=*/true);
                    mask[idx(x, y)] =
                        static_cast<std::uint8_t>(std::max<int>(128, coverageToByte(c)));
                }
            const int dx[4] = {-1, 1, 0, 0};
            const int dy[4] = {0, 0, -1, 1};
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (filled[idx(x, y)] || mask[idx(x, y)] != 0)
                        continue; // only unselected pixels not already fringed
                    bool touchesInterior = false;
                    for (int k = 0; k < 4; ++k) {
                        const int nx = x + dx[k], ny = y + dy[k];
                        if (nx >= 0 && ny >= 0 && nx < w && ny < h && filled[idx(nx, ny)]) {
                            touchesInterior = true;
                            break;
                        }
                    }
                    if (!touchesInterior)
                        continue;
                    const double c = wandCoverage(dist(x, y), T, /*antialias=*/true);
                    if (c > 0.0)
                        mask[idx(x, y)] =
                            static_cast<std::uint8_t>(std::min<int>(127, coverageToByte(c)));
                }
        }
    }

    if (!out.anySelected())
        return {}; // a coverage-free result collapses to "no selection" (research §1)
    return out;
}

std::optional<Selection> selectionFromLayerPixels(const Layer& layer, std::uint32_t docW,
                                                  std::uint32_t docH) {
    const common::Image* src = nullptr;
    if (const auto* raster = layer.as<RasterLayer>())
        src = &raster->image();
    else if (const auto* magic = layer.as<MagicLayer>())
        src = &magic->source();
    if (src == nullptr)
        return std::nullopt; // groups/vector/text/texture/adjustment own no pixels to select

    Selection s(docW, docH);
    if (src->empty())
        return s;
    const common::Affine2D t = worldTransform(layer); // ancestors compose (transformed groups)
    if (t == common::Affine2D::identity() && src->width == docW && src->height == docH) {
        // 1:1 fast path (the common case: a document-sized, untransformed layer).
        auto* out = s.data().data();
        for (std::size_t i = 0, n = s.data().size(); i < n; ++i)
            out[i] = src->rgba[i * 4 + 3];
        return s;
    }
    const auto inv = t.inverse();
    if (!inv)
        return s; // singular transform collapses to nothing, like the compositor's leaf walk
    for (std::uint32_t y = 0; y < docH; ++y) {
        auto* row = s.data().data() + static_cast<std::size_t>(y) * docW;
        for (std::uint32_t x = 0; x < docW; ++x) {
            const common::Vec2 p = inv->apply({x + 0.5, y + 0.5});
            const long sx = static_cast<long>(std::floor(p.x));
            const long sy = static_cast<long>(std::floor(p.y));
            if (sx >= 0 && sy >= 0 && sx < static_cast<long>(src->width) &&
                sy < static_cast<long>(src->height)) {
                row[x] = src->rgba[(static_cast<std::size_t>(sy) * src->width + sx) * 4 + 3];
            }
        }
    }
    return s;
}

// ---- Layer masks (S31) --------------------------------------------------------------------------

namespace {

// The mask grid for `layer` (see the header note): raster/magic mask their source image, every
// other kind masks the document window.
void maskDimsFor(const Layer& layer, std::uint32_t docW, std::uint32_t docH, std::uint32_t& w,
                 std::uint32_t& h) {
    w = docW;
    h = docH;
    if (const auto* raster = layer.as<RasterLayer>()) {
        w = raster->image().width;
        h = raster->image().height;
    } else if (const auto* magic = layer.as<MagicLayer>()) {
        w = magic->source().width;
        h = magic->source().height;
    }
}

// A fresh, correctly PLACED sheet for `layer` (the RasterMask grid contract, core/layer.hpp):
// sized by maskDimsFor and pinned by core::newMaskToLocal. It keeps the LINKAGE of the mask
// already on the layer, because the flag picks which space `toLocal` maps into: building coverage
// on the linked grid and then flagging the result unlinked -- which is what "Add to Mask" on an
// unlinked mask used to do -- places every byte of it in the wrong space.
[[nodiscard]] RasterMask freshMask(const Layer& layer, std::uint32_t docW, std::uint32_t docH,
                                   std::uint8_t fill) {
    std::uint32_t w = 0, h = 0;
    maskDimsFor(layer, docW, docH, w, h);
    RasterMask mask(w, h, fill);
    const RasterMask* existing = layer.mask();
    if (existing != nullptr && existing->width == w && existing->height == h) {
        // Rebuilding over a mask that is already there (the Select menu's Add to / Subtract from
        // Mask): keep ITS sheet, so the new coverage lands cell-for-cell on the old and the two
        // combine byte-wise. Re-capturing would put them in different spaces the moment the layer
        // had been moved since -- the combine would then mix two sheets that no longer align.
        mask.linked = existing->linked;
        mask.toLocal = existing->toLocal;
        return mask;
    }
    mask.linked = existing == nullptr || existing->linked;
    mask.toLocal = newMaskToLocal(layer, mask.linked);
    return mask;
}

} // namespace

RasterMask revealAllMask(const Layer& layer, std::uint32_t docW, std::uint32_t docH) {
    return freshMask(layer, docW, docH, 255);
}

RasterMask maskFromSelection(const Layer& layer, const Selection& sel, std::uint32_t docW,
                             std::uint32_t docH) {
    if (sel.isEmpty())
        return revealAllMask(layer, docW, docH); // no selection = everything (S13 semantics)
    RasterMask mask = freshMask(layer, docW, docH, 0);
    const std::uint32_t w = mask.width, h = mask.height;
    if (mask.empty())
        return mask;
    // Back-map the document-space selection onto the sheet through the placement freshMask just
    // captured, so the mask reveals exactly the DOC pixels the selection covered whatever the
    // layer's transform. For a document-window sheet that placement is the world transform's own
    // inverse, so the map below is the identity and the mask IS the selection -- the transform
    // rides in `toLocal` instead of skewing the coverage, which is what keeps a shape layer's mask
    // on the shape (see the grid contract) and its thumbnail readable.
    const common::Affine2D t = maskToDocument(layer, mask); // mask px -> document
    if (t == common::Affine2D::identity() && w == sel.width() && h == sel.height()) {
        mask.coverage = sel.data(); // 1:1 fast path (every doc-window sheet, and an untouched raster)
        return mask;
    }
    for (std::uint32_t y = 0; y < h; ++y) {
        auto* row = mask.coverage.data() + static_cast<std::size_t>(y) * w;
        for (std::uint32_t x = 0; x < w; ++x) {
            const common::Vec2 p = t.apply({x + 0.5, y + 0.5});
            const long dx = static_cast<long>(std::floor(p.x));
            const long dy = static_cast<long>(std::floor(p.y));
            if (dx >= 0 && dy >= 0 && dx < static_cast<long>(sel.width()) &&
                dy < static_cast<long>(sel.height()))
                row[x] = sel.at(static_cast<std::uint32_t>(dx), static_cast<std::uint32_t>(dy));
        }
    }
    return mask;
}

std::optional<Selection> selectionFromLayerMask(const Layer& layer, std::uint32_t docW,
                                                std::uint32_t docH) {
    const RasterMask* mask = layer.mask();
    if (mask == nullptr || mask->empty())
        return std::nullopt;
    Selection s(docW, docH);
    // Where the compositor folds the mask: the sheet's own placement (layer-local when linked, the
    // parent's space when not) carried out to document space -- core::maskToDocument is the one
    // definition of that map, shared with the fold and with mask painting.
    const common::Affine2D t = maskToDocument(layer, *mask);
    if (t == common::Affine2D::identity() && mask->width == docW && mask->height == docH) {
        s.data() = mask->coverage; // 1:1 fast path
    } else {
        const auto inv = t.inverse();
        if (!inv)
            return Selection{}; // singular transform shows nothing, like the compositor
        for (std::uint32_t y = 0; y < docH; ++y) {
            auto* row = s.data().data() + static_cast<std::size_t>(y) * docW;
            for (std::uint32_t x = 0; x < docW; ++x) {
                const common::Vec2 p = inv->apply({x + 0.5, y + 0.5});
                const long mx = static_cast<long>(std::floor(p.x));
                const long my = static_cast<long>(std::floor(p.y));
                if (mx >= 0 && my >= 0 && mx < static_cast<long>(mask->width) &&
                    my < static_cast<long>(mask->height))
                    row[x] =
                        mask->coverage[static_cast<std::size_t>(my) * mask->width + mx];
            }
        }
    }
    if (!s.anySelected())
        return Selection{}; // coverage-free collapses to "no selection" (research §1)
    return s;
}

} // namespace mosaic::core
