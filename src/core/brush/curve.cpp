#include "core/brush/curve.hpp"

#include "core/brush/math_util.hpp"
#include "core/brush/parse_util.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

using detail::appendNumber;
using detail::clamp01;
using detail::parseDouble;
using detail::trim;

// Solve the symmetric tridiagonal system whose sub- and super-diagonals are both `off` (size n-1),
// whose diagonal is `diag` (size n) and whose right-hand side is `rhs` (size n). Thomas algorithm.
// Used for the interior second derivatives of a natural cubic spline, where the matrix is
// diagonally dominant (diag[i] = 2*(h[i]+h[i+1]) > off[i-1] + off[i] = h[i] + h[i+1]), so no
// pivoting is needed and the forward sweep cannot divide by zero.
[[nodiscard]] std::vector<double> solveTridiagonal(const std::vector<double>& off,
                                                   const std::vector<double>& diag,
                                                   const std::vector<double>& rhs) {
    const std::size_t n = diag.size();
    std::vector<double> x(n, 0.0);
    if (n == 0)
        return x;
    if (n == 1) {
        x[0] = rhs[0] / diag[0];
        return x;
    }

    std::vector<double> alpha(n, 0.0);
    std::vector<double> beta(n, 0.0);
    alpha[1] = -off[0] / diag[0];
    beta[1] = rhs[0] / diag[0];
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const double denom = off[i - 1] * alpha[i] + diag[i];
        alpha[i + 1] = -off[i] / denom;
        beta[i + 1] = (rhs[i] - off[i - 1] * beta[i]) / denom;
    }
    x[n - 1] = (rhs[n - 1] - off[n - 2] * beta[n - 1]) / (diag[n - 1] + off[n - 2] * alpha[n - 1]);
    for (std::size_t i = n - 1; i-- > 0;)
        x[i] = alpha[i + 1] * x[i + 1] + beta[i + 1];
    return x;
}

} // namespace

Curve::Curve() : Curve(std::vector<CurvePoint>{{0.0, 0.0, false}, {1.0, 1.0, false}}) {}

Curve::Curve(std::vector<CurvePoint> points) : m_points(std::move(points)) {
    if (m_points.empty()) {
        m_points = {{0.0, 0.0, false}, {1.0, 1.0, false}};
    } else {
        std::stable_sort(m_points.begin(), m_points.end(),
                         [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
        // A zero-width interval has no spline; keep the first point at each x.
        m_points.erase(std::unique(m_points.begin(), m_points.end(),
                                   [](const CurvePoint& a, const CurvePoint& b) {
                                       return !(a.x < b.x) && !(b.x < a.x);
                                   }),
                       m_points.end());
    }
    rebuild();
}

void Curve::rebuild() {
    m_intervals.clear();
    const std::size_t n = m_points.size();
    if (n < 2)
        return;
    m_intervals.resize(n - 1);

    // Walk maximal segments bounded by the endpoints and by any interior corner. Each segment is an
    // independent natural cubic spline; a corner belongs to both the segment that ends at it and the
    // one that starts there (position is continuous, the derivatives are not).
    std::size_t begin = 0;
    while (begin + 1 < n) {
        std::size_t end = begin + 1;
        while (end + 1 < n && !m_points[end].corner)
            ++end;

        const std::size_t count = end - begin + 1; // points in this segment (>= 2)
        const std::size_t intervals = count - 1;

        std::vector<double> h(intervals);
        for (std::size_t i = 0; i < intervals; ++i)
            h[i] = m_points[begin + i + 1].x - m_points[begin + i].x;

        // Second derivatives at the segment's knots; zero at both ends (the natural condition).
        std::vector<double> c(count, 0.0);
        if (intervals > 1) {
            const std::size_t m = intervals - 1; // interior knots
            std::vector<double> diag(m);
            std::vector<double> rhs(m);
            std::vector<double> off(m > 0 ? m - 1 : 0);
            for (std::size_t i = 0; i < m; ++i) {
                diag[i] = 2.0 * (h[i] + h[i + 1]);
                const double dyR = m_points[begin + i + 2].y - m_points[begin + i + 1].y;
                const double dyL = m_points[begin + i + 1].y - m_points[begin + i].y;
                rhs[i] = 6.0 * (dyR / h[i + 1] - dyL / h[i]);
            }
            for (std::size_t i = 0; i + 1 < m; ++i)
                off[i] = h[i + 1];
            const std::vector<double> interior = solveTridiagonal(off, diag, rhs);
            for (std::size_t i = 0; i < m; ++i)
                c[i + 1] = interior[i];
        }

        for (std::size_t i = 0; i < intervals; ++i) {
            Cubic& s = m_intervals[begin + i];
            s.a = m_points[begin + i].y;
            s.c = c[i];
            s.d = (c[i + 1] - c[i]) / h[i];
            s.b = -0.5 * c[i] * h[i] - (1.0 / 6.0) * s.d * h[i] * h[i] +
                  (m_points[begin + i + 1].y - m_points[begin + i].y) / h[i];
        }

        begin = end;
    }
}

double Curve::eval(double x) const {
    if (m_points.size() == 1)
        return clamp01(m_points.front().y);
    if (m_intervals.empty())
        return 0.0;

    const double lo = m_points.front().x;
    const double hi = m_points.back().x;
    if (x < lo)
        x = lo;
    if (x > hi)
        x = hi;

    // Last interval whose left knot is <= x.
    std::size_t i = 0;
    while (i + 1 < m_intervals.size() && x >= m_points[i + 1].x)
        ++i;

    const double t = x - m_points[i].x;
    const Cubic& s = m_intervals[i];
    const double y = s.a + s.b * t + 0.5 * s.c * t * t + (1.0 / 6.0) * s.d * t * t * t;
    return clamp01(y);
}

std::vector<float> Curve::toLut(std::size_t size) const {
    if (size < 2)
        size = 2;
    std::vector<float> lut(size);
    const double step = 1.0 / static_cast<double>(size - 1);
    for (std::size_t i = 0; i < size; ++i)
        lut[i] = static_cast<float>(eval(static_cast<double>(i) * step));
    return lut;
}

bool Curve::isIdentity() const noexcept {
    return m_points.size() == 2 && m_points[0].x == 0.0 && m_points[0].y == 0.0 &&
           m_points[1].x == 1.0 && m_points[1].y == 1.0;
}

Curve Curve::fromString(std::string_view s) {
    std::vector<CurvePoint> pts;
    while (!s.empty()) {
        const std::size_t semi = s.find(';');
        std::string_view entry = (semi == std::string_view::npos) ? s : s.substr(0, semi);
        s = (semi == std::string_view::npos) ? std::string_view{} : s.substr(semi + 1);

        entry = trim(entry);
        if (entry.empty())
            continue; // the trailing ';' of a well-formed string, or a stray one

        // First two comma-separated tokens are x and y; later tokens are flags, and any we do not
        // recognize are ignored rather than rejected (the format reserves room for more).
        const std::size_t c1 = entry.find(',');
        if (c1 == std::string_view::npos)
            continue;
        const std::size_t c2 = entry.find(',', c1 + 1);
        const std::string_view xs = entry.substr(0, c1);
        const std::string_view ys = (c2 == std::string_view::npos)
                                        ? entry.substr(c1 + 1)
                                        : entry.substr(c1 + 1, c2 - c1 - 1);

        CurvePoint p;
        if (!parseDouble(xs, p.x) || !parseDouble(ys, p.y))
            continue;

        std::string_view rest = (c2 == std::string_view::npos) ? std::string_view{}
                                                               : entry.substr(c2 + 1);
        while (!rest.empty()) {
            const std::size_t comma = rest.find(',');
            const std::string_view flag =
                trim((comma == std::string_view::npos) ? rest : rest.substr(0, comma));
            if (flag == "is_corner")
                p.corner = true;
            if (comma == std::string_view::npos)
                break;
            rest = rest.substr(comma + 1);
        }
        pts.push_back(p);
    }
    return Curve(std::move(pts));
}

std::string Curve::toString() const {
    std::string out;
    out.reserve(m_points.size() * 16);
    for (const CurvePoint& p : m_points) {
        appendNumber(out, p.x);
        out.push_back(',');
        appendNumber(out, p.y);
        if (p.corner)
            out.append(",is_corner");
        out.push_back(';');
    }
    return out;
}

float evalLut(const std::vector<float>& lut, double x) {
    if (lut.empty())
        return 0.0f;
    if (lut.size() == 1)
        return lut[0];
    const double t = clamp01(x) * static_cast<double>(lut.size() - 1);
    const double f = std::floor(t);
    auto i = static_cast<std::size_t>(f);
    if (i + 1 >= lut.size())
        return lut.back();
    const auto frac = static_cast<float>(t - f);
    return lut[i] + (lut[i + 1] - lut[i]) * frac;
}

} // namespace mosaic::core::brush
