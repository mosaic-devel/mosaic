#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

// 2D geometry shared across modules (the document model's layer transforms, the canvas
// viewport's pan/zoom/rotate in S8, hit-testing, dirty rectangles). Deliberately small: a
// point/vector, an axis-aligned rectangle, and a 2D affine transform -- exactly what a raster+
// vector editor needs, no general linear-algebra library. Header-only and constexpr-friendly.
namespace mosaic::common {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;

    constexpr Vec2() = default;
    constexpr Vec2(double x_, double y_) : x(x_), y(y_) {}

    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Vec2 operator-(Vec2 a) { return {-a.x, -a.y}; }
    friend constexpr Vec2 operator*(Vec2 v, double s) { return {v.x * s, v.y * s}; }
    friend constexpr Vec2 operator*(double s, Vec2 v) { return {v.x * s, v.y * s}; }
    friend constexpr bool operator==(Vec2, Vec2) = default;

    [[nodiscard]] constexpr double dot(Vec2 o) const { return x * o.x + y * o.y; }
    [[nodiscard]] double length() const { return std::sqrt(x * x + y * y); }
};

// Axis-aligned rectangle (x,y = top-left in a y-down image space; w,h >= 0 when non-empty).
struct Rect {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;

    [[nodiscard]] constexpr double right() const { return x + w; }
    [[nodiscard]] constexpr double bottom() const { return y + h; }
    [[nodiscard]] constexpr bool empty() const { return w <= 0.0 || h <= 0.0; }
    [[nodiscard]] constexpr Vec2 topLeft() const { return {x, y}; }
    [[nodiscard]] constexpr Vec2 center() const { return {x + w * 0.5, y + h * 0.5}; }

    [[nodiscard]] constexpr bool contains(Vec2 p) const {
        return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
    }

    // The smallest rect covering both (treating an empty rect as "nothing").
    [[nodiscard]] constexpr Rect united(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        const double l = x < o.x ? x : o.x;
        const double t = y < o.y ? y : o.y;
        const double r = right() > o.right() ? right() : o.right();
        const double b = bottom() > o.bottom() ? bottom() : o.bottom();
        return {l, t, r - l, b - t};
    }

    // The overlap of the two rects, or an empty rect if they do not intersect.
    [[nodiscard]] constexpr Rect intersected(const Rect& o) const {
        const double l = x > o.x ? x : o.x;
        const double t = y > o.y ? y : o.y;
        const double r = right() < o.right() ? right() : o.right();
        const double b = bottom() < o.bottom() ? bottom() : o.bottom();
        if (r <= l || b <= t) return {};
        return {l, t, r - l, b - t};
    }

    [[nodiscard]] constexpr bool intersects(const Rect& o) const {
        return !intersected(o).empty();
    }

    [[nodiscard]] static constexpr Rect fromCorners(Vec2 a, Vec2 b) {
        const double l = a.x < b.x ? a.x : b.x;
        const double t = a.y < b.y ? a.y : b.y;
        const double r = a.x > b.x ? a.x : b.x;
        const double btm = a.y > b.y ? a.y : b.y;
        return {l, t, r - l, btm - t};
    }

    friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

// A 2D affine transform stored as the top two rows of a 3x3 matrix:
//     | m00 m01 m02 |   maps (x,y) -> (m00*x + m01*y + m02,
//     | m10 m11 m12 |                  m10*x + m11*y + m12)
//     |  0   0   1  |
// Composition follows matrix order: (A * B).apply(p) == A.apply(B.apply(p)).
struct Affine2D {
    double m00 = 1.0, m01 = 0.0, m02 = 0.0;
    double m10 = 0.0, m11 = 1.0, m12 = 0.0;

    [[nodiscard]] static constexpr Affine2D identity() { return {}; }
    [[nodiscard]] static constexpr Affine2D translation(double tx, double ty) {
        return {1, 0, tx, 0, 1, ty};
    }
    [[nodiscard]] static constexpr Affine2D scaling(double sx, double sy) {
        return {sx, 0, 0, 0, sy, 0};
    }
    [[nodiscard]] static Affine2D rotation(double radians) {
        const double c = std::cos(radians);
        const double s = std::sin(radians);
        return {c, -s, 0, s, c, 0};
    }
    // Translate by `t`, then rotate by `radians`, then scale by `s`, applied to a point in
    // the order scale -> rotate -> translate (the usual TRS layout an editor exposes).
    [[nodiscard]] static Affine2D trs(Vec2 t, double radians, Vec2 s) {
        return translation(t.x, t.y) * rotation(radians) * scaling(s.x, s.y);
    }

    [[nodiscard]] constexpr Vec2 apply(Vec2 p) const {
        return {m00 * p.x + m01 * p.y + m02, m10 * p.x + m11 * p.y + m12};
    }
    // Transform a direction/extent (ignores translation).
    [[nodiscard]] constexpr Vec2 applyVector(Vec2 v) const {
        return {m00 * v.x + m01 * v.y, m10 * v.x + m11 * v.y};
    }

    [[nodiscard]] constexpr double determinant() const { return m00 * m11 - m01 * m10; }

    [[nodiscard]] constexpr std::optional<Affine2D> inverse() const {
        const double det = determinant();
        if (det > -1e-12 && det < 1e-12) return std::nullopt;  // singular
        const double inv = 1.0 / det;
        const double i00 = m11 * inv;
        const double i01 = -m01 * inv;
        const double i10 = -m10 * inv;
        const double i11 = m00 * inv;
        return Affine2D{i00, i01, -(i00 * m02 + i01 * m12),
                        i10, i11, -(i10 * m02 + i11 * m12)};
    }

    // The axis-aligned bounding box of `r` after transformation (all four corners mapped).
    [[nodiscard]] constexpr Rect mapBounds(const Rect& r) const {
        const Vec2 c0 = apply({r.x, r.y});
        const Vec2 c1 = apply({r.right(), r.y});
        const Vec2 c2 = apply({r.x, r.bottom()});
        const Vec2 c3 = apply({r.right(), r.bottom()});
        const double l = std::min(std::min(c0.x, c1.x), std::min(c2.x, c3.x));
        const double t = std::min(std::min(c0.y, c1.y), std::min(c2.y, c3.y));
        const double rr = std::max(std::max(c0.x, c1.x), std::max(c2.x, c3.x));
        const double bb = std::max(std::max(c0.y, c1.y), std::max(c2.y, c3.y));
        return {l, t, rr - l, bb - t};
    }

    friend constexpr Affine2D operator*(const Affine2D& a, const Affine2D& b) {
        return {a.m00 * b.m00 + a.m01 * b.m10, a.m00 * b.m01 + a.m01 * b.m11,
                a.m00 * b.m02 + a.m01 * b.m12 + a.m02,
                a.m10 * b.m00 + a.m11 * b.m10, a.m10 * b.m01 + a.m11 * b.m11,
                a.m10 * b.m02 + a.m11 * b.m12 + a.m12};
    }

    friend constexpr bool operator==(const Affine2D&, const Affine2D&) = default;
};

}  // namespace mosaic::common
