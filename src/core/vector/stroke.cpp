#include "core/vector/stroke.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mosaic::core::vec {
namespace {

using common::Affine2D;
using common::Vec2;

Vec2 normalized(Vec2 v) {
    const double l = v.length();
    return l > 1e-12 ? v * (1.0 / l) : Vec2{0, 0};
}
Vec2 leftPerp(Vec2 d) { return {-d.y, d.x}; }

// Approximate isotropic scale of a transform (mean of the column lengths) -- used to size round
// cap/join tessellation in device pixels.
double deviceScale(const Affine2D& m) {
    return 0.5 * (std::hypot(m.m00, m.m10) + std::hypot(m.m01, m.m11));
}

// Segment count for a `sweep`-radian arc of device-space radius `r` to stay under `tol` px.
int arcSegs(double r, double sweep, double tol) {
    r = std::max(r, 1e-6);
    const double maxA = 2.0 * std::acos(std::clamp(1.0 - tol / r, -1.0, 1.0));
    const int n = maxA > 1e-6 ? static_cast<int>(std::ceil(std::abs(sweep) / maxA)) : 1;
    return std::max(n, 1);
}

// Force a consistent (positive-signed-area) winding so NonZero fill unions all pieces.
void ensurePositiveArea(Contour& c) {
    double a = 0;
    const auto& p = c.points;
    const std::size_t n = p.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2& u = p[i];
        const Vec2& v = p[(i + 1) % n];
        a += u.x * v.y - v.x * u.y;
    }
    if (a < 0) std::reverse(c.points.begin(), c.points.end());
}

Contour disc(Vec2 center, double r, int segs) {
    Contour c;
    c.closed = true;
    c.points.reserve(static_cast<std::size_t>(segs));
    for (int i = 0; i < segs; ++i) {
        const double t = 2.0 * M_PI * static_cast<double>(i) / segs;
        c.points.push_back({center.x + r * std::cos(t), center.y + r * std::sin(t)});
    }
    return c;
}

void pushPiece(Contours& out, std::vector<Vec2> pts) {
    Contour c;
    c.closed = true;
    c.points = std::move(pts);
    ensurePositiveArea(c);
    out.push_back(std::move(c));
}

// Fill the outer notch at vertex v between incoming dir d0 and outgoing dir d1.
void addJoin(Contours& out, Vec2 v, Vec2 d0, Vec2 d1, double hw, const Stroke& s, int discSegs) {
    const double cr = d0.x * d1.y - d0.y * d1.x;  // turn sign; ~0 == collinear, no gap
    if (std::abs(cr) < 1e-9) return;
    if (s.join == LineJoin::Round) {
        out.push_back(disc(v, hw, discSegs));
        return;
    }
    const Vec2 n0 = leftPerp(d0), n1 = leftPerp(d1);
    const double sign = (cr > 0) ? -1.0 : 1.0;  // the gap is on the outer side of the turn
    const Vec2 o0 = v + n0 * (hw * sign);
    const Vec2 o1 = v + n1 * (hw * sign);
    if (s.join == LineJoin::Bevel) {
        pushPiece(out, {v, o0, o1});
        return;
    }
    // Miter: intersect the two offset edges (o0 along d0, o1 along d1).
    const double det = -cr;  // = d0.x*(-d1.y) - (-d1.x)*d0.y
    if (std::abs(det) < 1e-9) {
        pushPiece(out, {v, o0, o1});
        return;
    }
    const Vec2 diff = o1 - o0;
    const double t0 = (-diff.x * d1.y + d1.x * diff.y) / det;
    const Vec2 M = o0 + d0 * t0;
    if ((M - v).length() > s.miterLimit * hw)  // limit exceeded -> fall back to bevel
        pushPiece(out, {v, o0, o1});
    else
        pushPiece(out, {v, o0, M, o1});
}

void strokePolyline(Contours& out, const std::vector<Vec2>& pts, bool closed, const Stroke& s,
                    double devScale, double tol) {
    const double hw = s.width * 0.5;
    if (hw <= 0) return;
    const std::size_t n = pts.size();
    const int discSegs = std::max(8, arcSegs(hw * devScale, 2.0 * M_PI, tol));
    if (n == 1) {
        if (s.cap == LineCap::Round) out.push_back(disc(pts[0], hw, discSegs));  // a dot
        return;
    }
    if (n < 2) return;

    const std::size_t segCount = closed ? n : n - 1;
    for (std::size_t i = 0; i < segCount; ++i) {
        Vec2 a = pts[i], b = pts[(i + 1) % n];
        const Vec2 d = normalized(b - a);
        if (d.x == 0 && d.y == 0) continue;
        const Vec2 nrm = leftPerp(d) * hw;
        if (!closed && s.cap == LineCap::Square) {  // extend the true open ends by hw
            if (i == 0) a = a - d * hw;
            if (i + 1 == segCount) b = b + d * hw;
        }
        pushPiece(out, {a + nrm, b + nrm, b - nrm, a - nrm});
    }

    const std::size_t firstJoin = closed ? 0 : 1;
    const std::size_t lastJoin = closed ? n : n - 1;  // open: interior verts 1..n-2
    for (std::size_t i = firstJoin; i < lastJoin; ++i) {
        const Vec2 v = pts[i];
        const Vec2 d0 = normalized(v - pts[(i + n - 1) % n]);
        const Vec2 d1 = normalized(pts[(i + 1) % n] - v);
        if ((d0.x == 0 && d0.y == 0) || (d1.x == 0 && d1.y == 0)) continue;
        addJoin(out, v, d0, d1, hw, s, discSegs);
    }

    if (!closed && s.cap == LineCap::Round) {  // round caps == a disc at each end
        out.push_back(disc(pts.front(), hw, discSegs));
        out.push_back(disc(pts.back(), hw, discSegs));
    }
}

// Split a polyline into its "on" dash runs (open polylines), walking by arc length.
std::vector<std::vector<Vec2>> applyDashes(const std::vector<Vec2>& pts, bool closed,
                                           const std::vector<double>& dash, double offset) {
    std::vector<std::vector<Vec2>> runs;
    std::vector<Vec2> P = pts;
    if (closed && !pts.empty()) P.push_back(pts.front());
    if (P.size() < 2) return runs;
    double period = 0;
    for (double d : dash) period += std::max(0.0, d);
    if (period <= 1e-9) {
        runs.push_back(P);  // degenerate dash -> solid
        return runs;
    }

    std::size_t di = 0;
    bool on = true;
    double rem = std::max(0.0, dash[0]);
    double o = std::fmod(offset, period);
    if (o < 0) o += period;
    while (o > 1e-12) {  // advance the pattern cursor by the offset
        if (o >= rem) {
            o -= rem;
            di = (di + 1) % dash.size();
            on = !on;
            rem = std::max(0.0, dash[di]);
        } else {
            rem -= o;
            o = 0;
        }
    }

    std::vector<Vec2> cur;
    auto emit = [&] {
        if (cur.size() >= 2) runs.push_back(cur);
        cur.clear();
    };
    for (std::size_t i = 0; i + 1 < P.size(); ++i) {
        const Vec2 a = P[i], b = P[i + 1];
        const double segLen = (b - a).length();
        if (segLen <= 1e-12) continue;
        double pos = 0;
        if (on && cur.empty()) cur.push_back(a);
        while (pos < segLen - 1e-12) {
            const double step = std::min(rem, segLen - pos);
            pos += step;
            rem -= step;
            const Vec2 pt = a + (b - a) * (pos / segLen);
            if (on) cur.push_back(pt);
            if (rem <= 1e-12) {  // toggle on/off
                if (on) emit();
                di = (di + 1) % dash.size();
                on = !on;
                rem = std::max(0.0, dash[di]);
                if (on) {
                    cur.clear();
                    cur.push_back(pt);
                }
            }
        }
    }
    if (on) emit();
    return runs;
}

}  // namespace

Contours strokeOutline(const Contours& contours, const Stroke& stroke, double tol,
                       const Affine2D& toDevice) {
    Contours out;
    if (stroke.width <= 0) return out;
    const double devScale = deviceScale(toDevice);
    for (const auto& c : contours) {
        if (c.points.empty()) continue;
        if (stroke.dashArray.empty()) {
            strokePolyline(out, c.points, c.closed, stroke, devScale, tol);
        } else {
            for (auto& run : applyDashes(c.points, c.closed, stroke.dashArray, stroke.dashOffset))
                strokePolyline(out, run, false, stroke, devScale, tol);
        }
    }
    return out;
}

}  // namespace mosaic::core::vec
