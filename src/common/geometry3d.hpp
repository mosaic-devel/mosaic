#pragma once

#include <cmath>

#include "common/geometry.hpp"  // Vec2 (screen/design-space projections)

// 3D math for the Type tool's extrude engine (docs/type-tool.md §10, S30-c) -- and nothing more.
// The same charter as geometry.hpp: exactly what the feature needs (a vector, a homogeneous
// column-vector transform, a gimbal-free orientation), header-only, double, constexpr-friendly;
// no general linear-algebra library. Conventions: RIGHT-handed, y-DOWN to match the 2D layer
// space (x right, y down, z toward the viewer is +z out of the canvas... see extrude_render for
// the camera's take), Mat4 is ROW-major storage applied to COLUMN vectors (m * v), angles in
// radians. Quaternions are (w, x, y, z), unit length for rotations.
namespace mosaic::common {

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr Vec3() = default;
    constexpr Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    friend constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
    friend constexpr Vec3 operator*(Vec3 v, double s) { return {v.x * s, v.y * s, v.z * s}; }
    friend constexpr Vec3 operator*(double s, Vec3 v) { return {v.x * s, v.y * s, v.z * s}; }
    friend constexpr bool operator==(Vec3, Vec3) = default;

    [[nodiscard]] constexpr double dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
    [[nodiscard]] constexpr Vec3 cross(Vec3 o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    [[nodiscard]] double length() const { return std::sqrt(x * x + y * y + z * z); }
    // The unit vector, or the zero vector unchanged (callers guard degenerate normals themselves;
    // returning zero keeps the failure visible instead of inventing a direction).
    [[nodiscard]] Vec3 normalized() const {
        const double l = length();
        return l > 0.0 ? Vec3{x / l, y / l, z / l} : Vec3{};
    }
};

struct Vec4 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 0.0;

    constexpr Vec4() = default;
    constexpr Vec4(double x_, double y_, double z_, double w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(Vec3 v, double w_) : x(v.x), y(v.y), z(v.z), w(w_) {}

    friend constexpr Vec4 operator+(Vec4 a, Vec4 b) {
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }
    friend constexpr Vec4 operator*(Vec4 v, double s) {
        return {v.x * s, v.y * s, v.z * s, v.w * s};
    }
    friend constexpr bool operator==(Vec4, Vec4) = default;

    // Perspective divide -> the Cartesian point. w == 0 (a direction) divides by 1 instead --
    // callers project points, and a zero w would otherwise poison the raster with infinities.
    [[nodiscard]] constexpr Vec3 dehomogenized() const {
        return w != 0.0 ? Vec3{x / w, y / w, z / w} : Vec3{x, y, z};
    }
};

// A 4x4 transform, row-major (m[r][c]), applied to column vectors: out = M * v. Compose left of
// the vector: (A * B) * v applies B first, then A -- matching the usual projection * view * model.
struct Mat4 {
    double m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    [[nodiscard]] static constexpr Mat4 identity() { return {}; }

    friend constexpr Mat4 operator*(const Mat4& a, const Mat4& b) {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                double s = 0.0;
                for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
                r.m[i][j] = s;
            }
        return r;
    }
    friend constexpr Vec4 operator*(const Mat4& a, Vec4 v) {
        Vec4 r;
        r.x = a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z + a.m[0][3] * v.w;
        r.y = a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z + a.m[1][3] * v.w;
        r.z = a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z + a.m[2][3] * v.w;
        r.w = a.m[3][0] * v.x + a.m[3][1] * v.y + a.m[3][2] * v.z + a.m[3][3] * v.w;
        return r;
    }
    friend constexpr bool operator==(const Mat4& a, const Mat4& b) {
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                if (a.m[i][j] != b.m[i][j]) return false;
        return true;
    }

    // Transform a POINT (w = 1, translation applies) / a DIRECTION (w = 0, translation ignored).
    [[nodiscard]] constexpr Vec3 transformPoint(Vec3 p) const {
        return (*this * Vec4{p, 1.0}).dehomogenized();
    }
    [[nodiscard]] constexpr Vec3 transformDirection(Vec3 d) const {
        const Vec4 r = *this * Vec4{d, 0.0};
        return {r.x, r.y, r.z};
    }

    [[nodiscard]] static constexpr Mat4 translation(Vec3 t) {
        Mat4 r;
        r.m[0][3] = t.x;
        r.m[1][3] = t.y;
        r.m[2][3] = t.z;
        return r;
    }
    [[nodiscard]] static constexpr Mat4 scale(Vec3 s) {
        Mat4 r;
        r.m[0][0] = s.x;
        r.m[1][1] = s.y;
        r.m[2][2] = s.z;
        return r;
    }

    // Symmetric perspective projection: vertical FOV `fovY` (radians), viewport aspect w/h, view-
    // space z mapped from [-near, -far] (right-handed, camera looking down -z) to NDC z [0, 1]
    // (the Vulkan convention; the CPU lane uses the same range so the two lanes share this
    // matrix). NDC x/y in [-1, 1].
    [[nodiscard]] static Mat4 perspective(double fovY, double aspect, double zNear, double zFar) {
        const double f = 1.0 / std::tan(fovY * 0.5);
        Mat4 r;
        r.m[0][0] = f / (aspect > 0.0 ? aspect : 1.0);
        r.m[1][1] = f;
        r.m[2][2] = zFar / (zNear - zFar);
        r.m[2][3] = zNear * zFar / (zNear - zFar);
        r.m[3][2] = -1.0;
        r.m[3][3] = 0.0;
        return r;
    }
    // Symmetric orthographic projection over view-space half-extents, z to [0, 1] as above --
    // the `perspective -> 0` limit the §10.3 slider bottoms out at (true ortho, no distortion).
    [[nodiscard]] static constexpr Mat4 orthographic(double halfW, double halfH, double zNear,
                                                     double zFar) {
        Mat4 r;
        r.m[0][0] = halfW > 0.0 ? 1.0 / halfW : 1.0;
        r.m[1][1] = halfH > 0.0 ? 1.0 / halfH : 1.0;
        r.m[2][2] = 1.0 / (zNear - zFar);
        r.m[2][3] = zNear / (zNear - zFar);
        return r;
    }
};

// A rotation quaternion (w + xi + yj + zk). Unit length by construction from axisAngle;
// renormalize after long compose chains (drag loops) with normalized().
struct Quat {
    double w = 1.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr Quat() = default;
    constexpr Quat(double w_, double x_, double y_, double z_) : w(w_), x(x_), y(y_), z(z_) {}

    [[nodiscard]] static constexpr Quat identity() { return {}; }

    [[nodiscard]] static Quat fromAxisAngle(Vec3 axis, double angleRad) {
        const Vec3 a = axis.normalized();
        const double h = angleRad * 0.5;
        const double s = std::sin(h);
        return {std::cos(h), a.x * s, a.y * s, a.z * s};
    }

    // Hamilton product: (a * b) rotates by b FIRST, then a -- matching Mat4 composition order.
    friend constexpr Quat operator*(Quat a, Quat b) {
        return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
                a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
    }
    friend constexpr bool operator==(Quat, Quat) = default;

    [[nodiscard]] double length() const { return std::sqrt(w * w + x * x + y * y + z * z); }
    [[nodiscard]] Quat normalized() const {
        const double l = length();
        return l > 0.0 ? Quat{w / l, x / l, y / l, z / l} : Quat{};
    }
    [[nodiscard]] constexpr Quat conjugate() const { return {w, -x, -y, -z}; }

    // Rotate a vector: q v q* (unit q assumed).
    [[nodiscard]] constexpr Vec3 rotate(Vec3 v) const {
        // t = 2 * (im x v); v' = v + w*t + im x t  (the standard expansion, fewer mults)
        const Vec3 im{x, y, z};
        const Vec3 t = im.cross(v) * 2.0;
        return v + t * w + im.cross(t);
    }

    // The equivalent rotation matrix (unit q assumed).
    [[nodiscard]] constexpr Mat4 toMat4() const {
        Mat4 r;
        const double xx = x * x, yy = y * y, zz = z * z;
        const double xy = x * y, xz = x * z, yz = y * z;
        const double wx = w * x, wy = w * y, wz = w * z;
        r.m[0][0] = 1.0 - 2.0 * (yy + zz);
        r.m[0][1] = 2.0 * (xy - wz);
        r.m[0][2] = 2.0 * (xz + wy);
        r.m[1][0] = 2.0 * (xy + wz);
        r.m[1][1] = 1.0 - 2.0 * (xx + zz);
        r.m[1][2] = 2.0 * (yz - wx);
        r.m[2][0] = 2.0 * (xz - wy);
        r.m[2][1] = 2.0 * (yz + wx);
        r.m[2][2] = 1.0 - 2.0 * (xx + yy);
        return r;
    }
};

}  // namespace mosaic::common
