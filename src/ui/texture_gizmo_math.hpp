#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>

#include "core/texture/sky_camera.hpp"
#include "core/texture/texture_params.hpp"

// Pure screen<->parameter mappings for the Texture Generator's preview gizmos (S55-f;
// docs/texture-generator.md §7.3). FLTK-free so every mapping is unit-testable: the dialog's
// preview widget is a thin event shell over these. All frame coordinates are image pixels
// (+y down) of the SAME frame the render camera was built from -- in Fit mode that is the proxy,
// in 1:1 mode the document frame (the cameras are frame-relative, so both agree).
namespace mosaic::ui::texgizmo {

struct Pt {
    double x = 0.0;
    double y = 0.0;
};

inline constexpr double kDeg = std::numbers::pi / 180.0;

// ---- Sky --------------------------------------------------------------------------------------

// Where a compass az/el direction sits in the frame, or nullopt when it is behind the camera
// plane (drag falls back to the inset dome). Inverse of SkyCamera::rayAt's projection; serves
// the sun AND the moon (S55-f night follow-up).
[[nodiscard]] inline std::optional<Pt> skyAzElScreen(const core::texture::SkyParams& s,
                                                     double azimuthDeg, double elevationDeg,
                                                     double w, double h) {
    if (w <= 0.0 || h <= 0.0) return std::nullopt;
    const auto cam = core::texture::SkyCamera::fromParams(s, static_cast<std::uint32_t>(w),
                                                          static_cast<std::uint32_t>(h));
    const auto d = core::texture::directionFromAzEl(azimuthDeg, elevationDeg);
    const double f = core::texture::skyDot(d, cam.forward);
    if (f <= 1e-6) return std::nullopt;
    const double nx = core::texture::skyDot(d, cam.right) / (f * cam.halfTanX);
    const double ny = core::texture::skyDot(d, cam.up) / (f * cam.halfTanY);
    return Pt{(nx + 1.0) * 0.5 * w, (1.0 + cam.shiftY - ny) * 0.5 * h};
}

// The az/el a frame position aims at (the in-frame sun/moon drag): the pixel's world ray.
inline void skyAzElFromScreen(const core::texture::SkyParams& s, double w, double h, double px,
                              double py, double& azimuthDeg, double& elevationDeg) {
    if (w <= 0.0 || h <= 0.0) return;
    const auto cam = core::texture::SkyCamera::fromParams(s, static_cast<std::uint32_t>(w),
                                                          static_cast<std::uint32_t>(h));
    const auto r = cam.rayAt(px, py);
    double az = std::atan2(r.x, r.y) / kDeg;
    if (az < 0.0) az += 360.0;
    azimuthDeg = az;
    elevationDeg = std::asin(std::clamp(r.z, -1.0, 1.0)) / kDeg;
}

[[nodiscard]] inline std::optional<Pt> skySunScreen(const core::texture::SkyParams& s, double w,
                                                    double h) {
    return skyAzElScreen(s, s.sunAzimuthDeg, s.sunElevationDeg, w, h);
}

// Aim the sun at a frame position. Elevation clamps to the slider range (down to -30: night).
inline void skySunFromScreen(core::texture::SkyParams& s, double w, double h, double px,
                             double py) {
    double az = s.sunAzimuthDeg, el = s.sunElevationDeg;
    skyAzElFromScreen(s, w, h, px, py, az, el);
    s.sunAzimuthDeg = az;
    s.sunElevationDeg = std::clamp(el, -30.0, 90.0);
}

// The horizon's frame row at column px (roll-aware): solves rayAt(px, py).z == 0 for py.
[[nodiscard]] inline double skyHorizonRowAt(const core::texture::SkyParams& s, double w, double h,
                                            double px) {
    const auto cam = core::texture::SkyCamera::fromParams(s, static_cast<std::uint32_t>(w),
                                                          static_cast<std::uint32_t>(h));
    const double nx = 2.0 * px / w - 1.0;
    double denom = cam.halfTanY * cam.up.z;
    if (std::abs(denom) < 1e-9) denom = denom < 0.0 ? -1e-9 : 1e-9;  // roll ~90: no usable horizon
    const double ny = -(cam.forward.z + nx * cam.halfTanX * cam.right.z) / denom;
    return (1.0 + cam.shiftY - ny) * 0.5 * h;
}

// The pitch that puts the CENTRE-column horizon at frame row py (the horizon-bar drag): from
// rayAt's centre-column z term, tan(pitch) = -ny * halfTanY * cos(roll).
[[nodiscard]] inline double skyPitchForHorizonRow(const core::texture::SkyParams& s, double w,
                                                  double h, double py) {
    const double halfTanX = std::tan(std::clamp(s.fovDeg, 10.0, 150.0) * 0.5 * kDeg);
    const double halfTanY = halfTanX * (w > 0.0 ? h / w : 1.0);
    const double ny = 1.0 + 2.0 * s.shiftY - 2.0 * py / (h > 0.0 ? h : 1.0);
    return std::atan(-ny * halfTanY * std::cos(s.rollDeg * kDeg)) / kDeg;
}

// ---- Grass ------------------------------------------------------------------------------------

[[nodiscard]] inline double grassHorizonRow(const core::texture::GrassParams& g, double w,
                                            double h) {
    const double halfTanX = std::tan(std::clamp(g.fovDeg, 20.0, 120.0) * 0.5 * kDeg);
    const double halfTanY = halfTanX * (w > 0.0 ? h / w : 1.0);
    const double ny = std::tan(std::clamp(g.pitchDeg, 0.5, 80.0) * kDeg) / halfTanY;
    return (1.0 - ny) * 0.5 * h;
}

[[nodiscard]] inline double grassPitchForHorizonRow(const core::texture::GrassParams& g, double w,
                                                    double h, double py) {
    const double halfTanX = std::tan(std::clamp(g.fovDeg, 20.0, 120.0) * 0.5 * kDeg);
    const double halfTanY = halfTanX * (w > 0.0 ? h / w : 1.0);
    const double ny = 1.0 - 2.0 * py / (h > 0.0 ? h : 1.0);
    return std::atan(ny * halfTanY) / kDeg;
}

// ---- Preview framing (continuous scroll-zoom + drag-pan) --------------------------------------

// The proxy render frame + the visible window for a continuously zoomed/panned preview. `zoom` is
// SCREEN pixels per DOCUMENT pixel (1 = 1:1); the render frame is the whole document scaled by it
// (the cameras are frame-relative, so the composition stays faithful at any resolution, §8.2), and
// the visible window is the pane-sized crop at the doc-space pan offset. A frame smaller than the
// pane needs no window (the pane centres it). Pure + unit-tested: BOTH the proxy request and the
// gizmo mapping read this one result, so the handles never drift off the rendered pixels.
struct PreviewView {
    std::uint32_t frameW = 0, frameH = 0;  // the camera frame (document x zoom)
    long winX = 0, winY = 0;               // window origin within the frame (pan, frame pixels)
    std::uint32_t viewW = 0, viewH = 0;    // visible frame pixels (== frame when it fits the pane)
};
[[nodiscard]] inline PreviewView previewView(std::uint32_t docW, std::uint32_t docH,
                                             std::uint32_t paneW, std::uint32_t paneH, double zoom,
                                             double panDocX, double panDocY) {
    PreviewView v;
    if (docW == 0 || docH == 0 || paneW == 0 || paneH == 0 || zoom <= 0.0) return v;
    v.frameW = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(docW * zoom)));
    v.frameH = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(docH * zoom)));
    v.viewW = std::min(paneW, v.frameW);
    v.viewH = std::min(paneH, v.frameH);
    const long maxX = static_cast<long>(v.frameW) - static_cast<long>(v.viewW);
    const long maxY = static_cast<long>(v.frameH) - static_cast<long>(v.viewH);
    v.winX = std::clamp<long>(std::lround(panDocX * zoom), 0, std::max(0L, maxX));
    v.winY = std::clamp<long>(std::lround(panDocY * zoom), 0, std::max(0L, maxY));
    return v;
}

// The fit zoom (screen px per doc px) that shows the WHOLE document without upscaling past 1:1 --
// the floor of the zoom range and the value used in fit mode.
[[nodiscard]] inline double previewFitZoom(std::uint32_t docW, std::uint32_t docH,
                                           std::uint32_t paneW, std::uint32_t paneH) {
    if (docW == 0 || docH == 0 || paneW == 0 || paneH == 0) return 1.0;
    return std::min({static_cast<double>(paneW) / docW, static_cast<double>(paneH) / docH, 1.0});
}

// The largest pan (document pixels) that keeps the visible window inside the frame on one axis;
// 0 when the frame fits the pane (nothing to pan -- the pane centres it).
[[nodiscard]] inline double previewMaxPanDoc(std::uint32_t docPx, std::uint32_t panePx,
                                             double zoom) {
    if (zoom <= 0.0) return 0.0;
    return std::max(0.0, static_cast<double>(docPx) - static_cast<double>(panePx) / zoom);
}

// ---- Shared insets ----------------------------------------------------------------------------

// The dome inset (sun / raking light): a disc whose CENTRE is straight up (elevation 90) and rim
// the horizon (elevation 0); the angle around is azimuth RELATIVE to `refAzimuthDeg` ("up" on the
// inset = the direction the camera faces -- 180 for the sky camera, 0 for grass/paper).
[[nodiscard]] inline Pt domeDot(double cx, double cy, double radius, double azimuthDeg,
                                double elevationDeg, double refAzimuthDeg) {
    const double a = (azimuthDeg - refAzimuthDeg) * kDeg;
    const double r = radius * (1.0 - std::clamp(elevationDeg, 0.0, 90.0) / 90.0);
    return {cx + std::sin(a) * r, cy - std::cos(a) * r};
}

inline void domeFromPoint(double cx, double cy, double radius, double px, double py,
                          double refAzimuthDeg, double& azimuthDeg, double& elevationDeg) {
    const double dx = px - cx, dy = py - cy;
    const double len = std::hypot(dx, dy);
    double az = refAzimuthDeg + std::atan2(dx, -dy) / kDeg;
    az = std::fmod(az, 360.0);
    if (az < 0.0) az += 360.0;
    azimuthDeg = az;
    elevationDeg = 90.0 * (1.0 - std::min(1.0, radius > 0.0 ? len / radius : 0.0));
}

// The compass inset (wind): direction around (same reference convention), strength = radius
// fraction.
[[nodiscard]] inline Pt compassDot(double cx, double cy, double radius, double directionDeg,
                                   double strength01, double refAzimuthDeg) {
    const double a = (directionDeg - refAzimuthDeg) * kDeg;
    const double r = radius * std::clamp(strength01, 0.0, 1.0);
    return {cx + std::sin(a) * r, cy - std::cos(a) * r};
}

inline void compassFromPoint(double cx, double cy, double radius, double px, double py,
                             double refAzimuthDeg, double& directionDeg, double& strength01) {
    const double dx = px - cx, dy = py - cy;
    double dir = refAzimuthDeg + std::atan2(dx, -dy) / kDeg;
    dir = std::fmod(dir, 360.0);
    if (dir < 0.0) dir += 360.0;
    directionDeg = dir;
    strength01 = std::clamp(radius > 0.0 ? std::hypot(dx, dy) / radius : 0.0, 0.0, 1.0);
}

}  // namespace mosaic::ui::texgizmo
