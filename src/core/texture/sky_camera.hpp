#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

#include "core/texture/texture_params.hpp"

// The §4.5 perspective camera (S55-b): a plain pinhole projection -- ancient, unencumbered --
// that turns each output pixel into a WORLD ray, so clouds foreshorten to a real vanishing
// point and the horizon sits (and tilts) where the target photograph's does. Header-only; the
// grass generator's ground-plane homography (S55-e) shares this machinery per §6.1.
//
// World frame: +X east, +Y north, +Z up. Compass azimuth is clockwise from north (0 N, 90 E,
// 180 S). The camera faces azimuth 180 (south), so sunAzimuthDeg = 180 lands at the frame
// centre -- the S55-a convention, preserved. Screen x grows with azimuth (east of south is
// screen-left, west screen-right).
namespace mosaic::core::texture {

struct SkyVec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

[[nodiscard]] inline SkyVec3 skyNormalize(SkyVec3 v) noexcept {
    const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len <= 0.0) return {0.0, 0.0, 1.0};
    return {v.x / len, v.y / len, v.z / len};
}

[[nodiscard]] inline double skyDot(SkyVec3 a, SkyVec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// A linear-radiometric RGB triple in the §4.4 display-linear working space, shared by the sky
// dome, the 2D cloud lane (sky_render.cpp) and the volumetric marcher (cloud_volume.cpp) so all
// three read ONE definition. Not the image pixel type (that is common::ColorF with alpha) --
// this is the pre-tonemap radiance the compositing helpers combine.
struct Rgb {
    double r = 0.0, g = 0.0, b = 0.0;
};

[[nodiscard]] inline Rgb mixRgb(Rgb a, Rgb b, double t) noexcept {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// Unit direction for a compass azimuth + elevation (degrees).
[[nodiscard]] inline SkyVec3 directionFromAzEl(double azimuthDeg, double elevationDeg) noexcept {
    const double az = azimuthDeg * std::numbers::pi / 180.0;
    const double el = elevationDeg * std::numbers::pi / 180.0;
    return {std::sin(az) * std::cos(el), std::cos(az) * std::cos(el), std::sin(el)};
}

// The cooked camera basis for one render: build once, then rayAt() per pixel.
struct SkyCamera {
    SkyVec3 right;    // screen +x
    SkyVec3 up;       // screen +y (upward on screen)
    SkyVec3 forward;  // through the principal point
    double halfTanX = 0.6;  // tan(horizontal fov / 2)
    double halfTanY = 0.45;
    double shiftY = 0.0;  // tilt-shift, NDC units of half-height
    double invW = 0.0, invH = 0.0;

    static SkyCamera fromParams(const SkyParams& s, std::uint32_t w, std::uint32_t h) {
        constexpr double kDegToRad = std::numbers::pi / 180.0;
        SkyCamera c;
        const double pitch = s.pitchDeg * kDegToRad;
        const double roll = s.rollDeg * kDegToRad;
        // Facing south (azimuth 180), pitched up: forward tilts toward +Z, up tips back with
        // it. Facing south, screen-right is WEST (-X) so azimuth grows to the right of 180.
        const SkyVec3 f{0.0, -std::cos(pitch), std::sin(pitch)};
        const SkyVec3 r0{-1.0, 0.0, 0.0};
        const SkyVec3 u0{0.0, std::sin(pitch), std::cos(pitch)};
        // Roll about the view axis: positive roll tilts the horizon clockwise on screen.
        c.right = {r0.x * std::cos(roll) + u0.x * std::sin(roll),
                   r0.y * std::cos(roll) + u0.y * std::sin(roll),
                   r0.z * std::cos(roll) + u0.z * std::sin(roll)};
        c.up = {u0.x * std::cos(roll) - r0.x * std::sin(roll),
                u0.y * std::cos(roll) - r0.y * std::sin(roll),
                u0.z * std::cos(roll) - r0.z * std::sin(roll)};
        c.forward = f;
        // fovDeg is the HORIZONTAL field of view; vertical follows the frame's aspect. A lens
        // shift (§4.5 tilt-shift) offsets the principal point so the horizon can sit off-centre
        // without pitching the rays -- positive shiftY moves the horizon down the frame.
        c.halfTanX = std::tan(std::clamp(s.fovDeg, 10.0, 150.0) * 0.5 * kDegToRad);
        c.halfTanY = c.halfTanX * (w > 0 ? static_cast<double>(h) / static_cast<double>(w) : 1.0);
        c.shiftY = 2.0 * s.shiftY;
        c.invW = w > 0 ? 1.0 / static_cast<double>(w) : 0.0;
        c.invH = h > 0 ? 1.0 / static_cast<double>(h) : 0.0;
        return c;
    }

    // The unit world ray through pixel centre (px, py) (image coords, +y down).
    [[nodiscard]] SkyVec3 rayAt(double px, double py) const noexcept {
        const double nx = 2.0 * px * invW - 1.0;
        const double ny = 1.0 - 2.0 * py * invH + shiftY;
        return skyNormalize({forward.x + nx * halfTanX * right.x + ny * halfTanY * up.x,
                             forward.y + nx * halfTanX * right.y + ny * halfTanY * up.y,
                             forward.z + nx * halfTanX * right.z + ny * halfTanY * up.z});
    }

    // The exact inverse of rayAt: project a unit world direction back to a pixel centre. Returns
    // false when the direction is behind the image plane (a star out of the frustum). Used to
    // place catalogue stars (S55 night overhaul); the basis is orthonormal so a world dir
    // decomposes as (forward, right, up) components directly.
    [[nodiscard]] bool project(SkyVec3 d, double& px, double& py) const noexcept {
        const double cf = skyDot(d, forward);
        if (cf <= 1e-6 || invW <= 0.0 || invH <= 0.0) return false;
        const double nx = (skyDot(d, right) / cf) / halfTanX;
        const double ny = (skyDot(d, up) / cf) / halfTanY;
        px = (nx + 1.0) * 0.5 / invW;
        py = (1.0 + shiftY - ny) * 0.5 / invH;
        return true;
    }
};

// The §6.1 grass ground-plane camera -- the SAME pinhole machinery as the sky camera, but placed
// at eye height ABOVE a ground plane z = 0 and pitched DOWN, so each pixel ray meets the lawn at a
// real foreshortened point and the sward recedes to a horizon. `project` maps a world point (blade
// control point) back to the image; `groundAt` inverts a pixel ray onto the plane. Depth is the
// distance ALONG the view axis (camZ), monotonic with range -- the back-to-front sort key and the
// per-blade screen-size / aerial-fade driver. World frame is the shared one (+X right, +Y into the
// scene, +Z up); the compass conventions of the sky are irrelevant here (a lawn has no sun azimuth
// binding), so the grass camera faces +Y with screen-x growing east.
struct GrassCamera {
    SkyVec3 eye{0.0, 0.0, 1.5};  // metres above the ground plane (eye level)
    SkyVec3 right{1.0, 0.0, 0.0};
    SkyVec3 up{0.0, 0.0, 1.0};
    SkyVec3 forward{0.0, 1.0, 0.0};
    double halfTanX = 0.52;
    double halfTanY = 0.39;
    double focalX = 100.0;  // image-plane pixels per unit of tan-angle (world-size -> screen-size)
    double focalY = 100.0;
    double w = 1.0, h = 1.0;
    double invW = 0.0, invH = 0.0;

    static GrassCamera fromParams(const GrassParams& g, std::uint32_t iw, std::uint32_t ih,
                                  double eyeHeight) {
        constexpr double kDegToRad = std::numbers::pi / 180.0;
        GrassCamera c;
        c.eye = {0.0, 0.0, eyeHeight};
        const double p = std::clamp(g.pitchDeg, 0.5, 80.0) * kDegToRad;  // down-tilt from level
        // Face +Y, tilt the view axis down toward the ground; up tips back to stay orthonormal.
        c.forward = {0.0, std::cos(p), -std::sin(p)};
        c.right = {1.0, 0.0, 0.0};
        c.up = {0.0, std::sin(p), std::cos(p)};  // = right x forward, +Z at zero pitch
        c.halfTanX = std::tan(std::clamp(g.fovDeg, 20.0, 120.0) * 0.5 * kDegToRad);
        c.w = iw > 0 ? static_cast<double>(iw) : 1.0;
        c.h = ih > 0 ? static_cast<double>(ih) : 1.0;
        c.halfTanY = c.halfTanX * c.h / c.w;
        c.focalX = 0.5 * c.w / c.halfTanX;
        c.focalY = 0.5 * c.h / c.halfTanY;  // == focalX; both kept for clarity at call sites
        c.invW = 1.0 / c.w;
        c.invH = 1.0 / c.h;
        return c;
    }

    // The unit world ray through pixel centre (px, py) (image coords, +y down).
    [[nodiscard]] SkyVec3 rayAt(double px, double py) const noexcept {
        const double nx = 2.0 * px * invW - 1.0;
        const double ny = 1.0 - 2.0 * py * invH;
        return skyNormalize({forward.x + nx * halfTanX * right.x + ny * halfTanY * up.x,
                             forward.y + nx * halfTanX * right.y + ny * halfTanY * up.y,
                             forward.z + nx * halfTanX * right.z + ny * halfTanY * up.z});
    }

    // Intersect the pixel ray with the ground plane z = 0. Returns false when the ray points at or
    // above the horizon (no lawn there). On success `ground` is the world hit and `dist` the range.
    [[nodiscard]] bool groundAt(double px, double py, SkyVec3& ground, double& dist) const noexcept {
        const SkyVec3 r = rayAt(px, py);
        if (r.z >= -1e-6) return false;  // level or upward: the sky half
        const double t = -eye.z / r.z;   // eye.z > 0, r.z < 0  => t > 0
        ground = {eye.x + r.x * t, eye.y + r.y * t, 0.0};
        dist = t;
        return true;
    }

    // Project a world point to image coords. Returns false if behind the image plane. `sx,sy` are
    // pixel coordinates (+y down) and `camZ` the along-axis depth (the size / sort driver).
    [[nodiscard]] bool project(SkyVec3 world, double& sx, double& sy, double& camZ) const noexcept {
        const SkyVec3 v{world.x - eye.x, world.y - eye.y, world.z - eye.z};
        camZ = skyDot(v, forward);
        if (camZ <= 1e-4) return false;
        const double xc = skyDot(v, right) / camZ;
        const double yc = skyDot(v, up) / camZ;
        sx = (xc / halfTanX + 1.0) * 0.5 * w;
        sy = (1.0 - yc / halfTanY) * 0.5 * h;
        return true;
    }
};

}  // namespace mosaic::core::texture
