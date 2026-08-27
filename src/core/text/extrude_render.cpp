#include "core/text/extrude_render.hpp"

#include "common/thread_pool.hpp"
#include "core/text/extrude_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// Software 3D rasterization for extruded text (S30-c). Technique lineage: edge-function triangle
// rasterization with a z-buffer (Pineda 1988 / textbook), perspective-correct attribute
// interpolation (1/w-linear in screen space), Blinn-Phong shading (Blinn 1977) -- all classic,
// public technique, straight from the textbooks.
namespace mosaic::core::text {
namespace {

using common::ColorF;
using common::ImageF;

constexpr double kPi = 3.14159265358979323846;
constexpr double kOrthoFovDeg = 0.5;  // below this the §10.3 slider means "true ortho"

struct Shaded {
    float r, g, b;
};

// GLSL-identical smoothstep (both lanes must run the same curve for parity).
double smoothstep01(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// The procedural "studio" environment (feedback 2026-07-03: max-metal read as dark plastic --
// chrome needs something to MIRROR, not just a key light): a neutral vertical gradient -- bright
// zenith, mid horizon, dark floor -- sampled along the reflected view ray. Roughness widens the
// horizon band, which is the cheap analytic stand-in for a blurred reflection. Grey on purpose:
// the Fresnel term tints it by the metal's albedo. Classic fake chrome, no capture data.
double studioEnv(Vec3 r, double rough) {
    const double up = -r.y;  // design frame is y-DOWN: "up on screen" = negative y
    const double w = 0.08 + 0.6 * rough;
    const double t = smoothstep01(-w, w, up);
    const double sky = 0.65 + (1.25 - 0.65) * std::clamp(up, 0.0, 1.0);
    const double ground = 0.65 + (0.15 - 0.65) * std::clamp(-up, 0.0, 1.0);
    return ground + (sky - ground) * t;
}

// Per-render reflection context: the canvas plane the reflected rays can hit (view frame,
// pivot-relative -- the canvas does NOT rotate with the solid) + the app-provided snapshot.
struct EnvContext {
    const ExtrudeEnv* env = nullptr;  // null or empty image = studio only
    double zEnv = 0.0;                // the solid's back cap plane
    Vec2 centerXY;                    // pivot -> design-space offset (cam.center.xy)
};

// Bilinear, edge-clamped, straight-alpha sample of the env snapshot.
ColorF sampleEnv(const common::ImageF& img, double x, double y) {
    if (img.width == 0 || img.height == 0) return {};
    const double fx = std::clamp(x - 0.5, 0.0, static_cast<double>(img.width - 1));
    const double fy = std::clamp(y - 0.5, 0.0, static_cast<double>(img.height - 1));
    const std::size_t x0 = static_cast<std::size_t>(fx);
    const std::size_t y0 = static_cast<std::size_t>(fy);
    const std::size_t x1 = std::min<std::size_t>(x0 + 1, img.width - 1);
    const std::size_t y1 = std::min<std::size_t>(y0 + 1, img.height - 1);
    const float tx = static_cast<float>(fx - static_cast<double>(x0));
    const float ty = static_cast<float>(fy - static_cast<double>(y0));
    const auto px = [&](std::size_t xx, std::size_t yy) {
        return &img.rgba[(yy * img.width + xx) * 4];
    };
    const float* p00 = px(x0, y0);
    const float* p10 = px(x1, y0);
    const float* p01 = px(x0, y1);
    const float* p11 = px(x1, y1);
    ColorF out;
    float* o = &out.r;
    for (int c = 0; c < 4; ++c) {
        const float top = p00[c] + (p10[c] - p00[c]) * tx;
        const float bot = p01[c] + (p11[c] - p01[c]) * tx;
        o[c] = top + (bot - top) * ty;
    }
    return out;
}

// Blinn-Phong / PBR-lite (§10.3): albedo split into diffuse/specular by metalness, shininess from
// roughness, N unit and already facing the viewer. Lights' directions are the way the light
// TRAVELS, in the un-rotated design frame -- they stay put while the solid orbits (the way every
// 3D text tool behaves), which is exactly what shading in rotated (camera) space gives us. On top
// of the lights: a Fresnel-weighted environment reflection -- the studio gradient, or (when
// params.reflectCanvas and a snapshot exists) the canvas content behind the solid, hit by the
// reflected ray on the back-cap plane. `p` is the surface point in the same pivot-relative view
// frame as `viewDir`. `capSurface` marks the front/back face: with reflectSidesOnly set, caps
// keep the studio finish and only the extruded sides mirror the canvas (round 3 follow-up: a
// head-on face mirroring the artwork reads odd at some angles).
Shaded shade(const Extrude& params, const Material& m, Vec3 n, Vec3 viewDir, Vec3 p,
             const EnvContext& ec, bool capSurface) {
    const float ar = m.albedo.r, ag = m.albedo.g, ab = m.albedo.b;
    if (!params.lightingEnabled) return {ar, ag, ab};  // flat self-lit faces

    const double metal = std::clamp(static_cast<double>(m.metalness), 0.0, 1.0);
    const double rough = std::clamp(static_cast<double>(m.roughness), 0.0, 1.0);
    const double kd = 1.0 - metal;                      // a true metal has no diffuse lobe
    const double ksR = 0.06 + (ar - 0.06) * metal;      // dielectric 6% -> tinted metal spec (F0)
    const double ksG = 0.06 + (ag - 0.06) * metal;
    const double ksB = 0.06 + (ab - 0.06) * metal;
    const double shininess = std::exp2(2.0 + 8.0 * (1.0 - rough));  // 4 .. 1024
    const double specGain = 0.25 + 0.75 * (1.0 - rough);            // rough surfaces spread it thin

    double r = ar * params.ambient.r * kd, g = ag * params.ambient.g * kd,
           b = ab * params.ambient.b * kd;
    for (const Light& light : params.lights) {
        const Vec3 l = (-light.direction).normalized();  // surface -> lamp
        if (l == Vec3{}) continue;
        const double ndl = std::max(0.0, n.dot(l));
        const double di = light.intensity;
        if (ndl > 0.0) {
            r += ar * kd * light.color.r * di * ndl;
            g += ag * kd * light.color.g * di * ndl;
            b += ab * kd * light.color.b * di * ndl;
            const Vec3 h = (l + viewDir).normalized();
            const double spec = std::pow(std::max(0.0, n.dot(h)), shininess) * specGain * di;
            r += ksR * light.color.r * spec;
            g += ksG * light.color.g * spec;
            b += ksB * light.color.b * spec;
        }
    }

    // Environment reflection, Fresnel-Schlick weighted (F0 = the ks split above): the reflected
    // view ray samples the canvas snapshot where it hits the back-cap plane, else the studio.
    const double cosNV = std::max(0.0, n.dot(viewDir));
    const double f1 = 1.0 - cosNV;
    const double f5 = f1 * f1 * f1 * f1 * f1;
    const Vec3 refl = n * (2.0 * cosNV) - viewDir;  // surface -> environment
    const double sv = studioEnv(refl, rough);
    double er = sv, eg = sv, eb = sv;
    if (params.reflectCanvas && !(params.reflectSidesOnly && capSurface) && ec.env != nullptr &&
        ec.env->image != nullptr && !ec.env->image->rgba.empty() && refl.z < -1e-9) {
        const double t = (ec.zEnv - p.z) / refl.z;
        const Vec3 hit = p + refl * t;
        const Vec2 envPx =
            ec.env->layerToEnv.apply({hit.x + ec.centerXY.x, hit.y + ec.centerXY.y});
        const ColorF s = sampleEnv(*ec.env->image, envPx.x, envPx.y);
        const double a = std::clamp(static_cast<double>(s.a), 0.0, 1.0);
        er += (s.r - er) * a;  // transparent canvas falls through to the studio
        eg += (s.g - eg) * a;
        eb += (s.b - eb) * a;
    }
    const double envDim = 1.0 - 0.55 * rough;  // rough surfaces spread the mirror thin too
    r += (ksR + (1.0 - ksR) * f5) * er * envDim;
    g += (ksG + (1.0 - ksG) * f5) * eg * envDim;
    b += (ksB + (1.0 - ksB) * f5) * eb * envDim;

    return {static_cast<float>(std::min(r, 8.0)), static_cast<float>(std::min(g, 8.0)),
            static_cast<float>(std::min(b, 8.0))};
}

// The injected GPU lane (§10.5); empty until the app registers render::ExtrudeGpu.
ExtrudeRenderOverride g_override;

}  // namespace

void setExtrudeRenderOverride(ExtrudeRenderOverride fn) { g_override = std::move(fn); }

ExtrudeCamera ExtrudeCamera::from(const common::Rect& designBounds, const Extrude& params) {
    ExtrudeCamera cam;
    cam.center = {designBounds.x + designBounds.w * 0.5, designBounds.y + designBounds.h * 0.5,
                  0.0};
    const double fovDeg = std::clamp(static_cast<double>(params.perspective), 0.0, 130.0);
    cam.ortho = fovDeg < kOrthoFovDeg;
    const double radius = 0.5 * std::hypot(designBounds.w, designBounds.h) +
                          std::abs(static_cast<double>(params.depth));
    const double safeRadius = std::max(radius, 1.0);
    if (cam.ortho) {
        cam.camDist = safeRadius * 16.0;  // any large stand-off; projection ignores it
    } else {
        const double fovRad = fovDeg * kPi / 180.0;
        // Frame the solid's bounding radius in the FOV, but never let the eye enter the solid.
        cam.camDist = std::max(safeRadius / std::tan(fovRad * 0.5), safeRadius * 1.4);
    }
    return cam;
}

Vec2 ExtrudeCamera::project(Vec3 pr, double& depth) const {
    depth = camDist - pr.z;  // grows away from the eye; the z = 0 plane sits at camDist
    if (ortho) return {center.x + pr.x, center.y + pr.y};
    const double d = std::max(depth, camDist * 0.05);  // never divide across the eye plane
    const double s = camDist / d;                      // scale-true at z = 0 by construction
    return {center.x + pr.x * s, center.y + pr.y * s};
}

ExtrudePlaneMap ExtrudePlaneMap::from(const common::Rect& designBounds, const Extrude& params) {
    ExtrudePlaneMap m;
    m.cam = ExtrudeCamera::from(designBounds, params);
    m.orientation = params.orientation;
    // The chrome rides whichever cap FACES the viewer: past 90 degrees the front cap is hidden
    // behind the solid, and projecting onto it left the selection floating a depth behind the
    // visible (mirrored) back face -- "the selection is culled/blank when rotated around"
    // (round 3). The facing test is the rotated +z axis's z sign.
    const double facing = params.orientation.rotate({0.0, 0.0, 1.0}).z;
    m.zPlane = (facing >= 0.0 ? 0.5 : -0.5) * std::max(0.01, static_cast<double>(params.depth));
    return m;
}

Vec2 ExtrudePlaneMap::project(Vec2 p) const {
    double depth = 0.0;
    return cam.project(
        orientation.rotate({p.x - cam.center.x, p.y - cam.center.y, zPlane}), depth);
}

std::optional<Vec2> ExtrudePlaneMap::unproject(Vec2 q) const {
    const common::Quat inv = orientation.conjugate();
    Vec3 o, d;  // the eye ray through q, pivot-relative (matches ExtrudeCamera::project)
    if (cam.ortho) {
        o = {q.x - cam.center.x, q.y - cam.center.y, 0.0};
        d = {0.0, 0.0, -1.0};
    } else {
        o = {0.0, 0.0, cam.camDist};  // the eye; the ray passes the z = 0 plane at q
        d = Vec3{q.x - cam.center.x, q.y - cam.center.y, 0.0} - o;
    }
    // The front cap is the set of points whose UN-rotated pivot-relative z equals zPlane; the
    // constraint (inv.rotate(o + t*d)).z == zPlane is linear in t.
    const double z0 = inv.rotate(o).z;
    const double dz = inv.rotate(d).z;
    if (std::abs(dz) < 1e-9) return std::nullopt;  // the plane is edge-on to the ray
    const double t = (zPlane - z0) / dz;
    const Vec3 hit = inv.rotate(o + d * t);  // un-rotated: z == zPlane by construction
    return Vec2{hit.x + cam.center.x, hit.y + cam.center.y};
}

common::Rect projectedExtrudeBounds(const common::Rect& bounds2d, const Extrude& params) {
    if (bounds2d.empty()) return bounds2d;
    // Swell by the bevels' largest outward reach (the Convex bullnose pokes ~0.21 * size past the
    // outline; a full size is a safe conservative pad), then project the extruded box's corners.
    const double pad =
        std::max(static_cast<double>(params.bevelFront.size), static_cast<double>(params.bevelBack.size));
    const common::Rect b{bounds2d.x - pad, bounds2d.y - pad, bounds2d.w + 2.0 * pad,
                         bounds2d.h + 2.0 * pad};
    const ExtrudeCamera cam = ExtrudeCamera::from(b, params);
    const double hz = std::abs(static_cast<double>(params.depth)) * 0.5;
    double minX = std::numeric_limits<double>::infinity(), minY = minX;
    double maxX = -minX, maxY = -minY;
    for (const double cx : {b.x, b.right()})
        for (const double cy : {b.y, b.bottom()})
            for (const double cz : {-hz, hz}) {
                double depth = 0.0;
                const Vec2 p = cam.project(cam.rotate(params, {cx, cy, cz}), depth);
                minX = std::min(minX, p.x);
                minY = std::min(minY, p.y);
                maxX = std::max(maxX, p.x);
                maxY = std::max(maxY, p.y);
            }
    return common::Rect::fromCorners({minX, minY}, {maxX, maxY});
}

void renderExtrudeMeshF(common::ImageF& dst, const ExtrudeMesh& mesh, const Extrude& params,
                        const common::Affine2D& toPixel, bool antialias, const ExtrudeEnv* env,
                        const ExtrudeOverlay* overlay) {
    if (mesh.empty() || dst.width == 0 || dst.height == 0) return;
    if (overlay != nullptr && overlay->empty()) overlay = nullptr;  // nothing baked = no overlay
    // The Vulkan lane, when the app injected one and it can serve this render (§10.5); the CPU
    // rasterizer below stays the always-there fallback (headless tests, no-Vulkan machines).
    if (g_override && g_override(dst, mesh, params, toPixel, antialias, env, overlay)) return;
    const ExtrudeCamera cam = ExtrudeCamera::from(mesh.designBounds, params);
    EnvContext ec;
    ec.env = env;
    ec.zEnv = -0.5 * std::max(0.01, static_cast<double>(params.depth));
    ec.centerXY = {cam.center.x, cam.center.y};
    const int S = antialias ? 2 : 1;  // 2x2 supersampling; block.aa == None renders hard

    // Transform + project every vertex once. Positions/normals rotate into camera-facing space;
    // the projected design point then rides the SAME toPixel bake as the 2D path (device-space
    // crispness under any layer scale/rotation).
    struct TV {
        Vec3 view;    // rotated, pivot-relative (the eye sits at (0, 0, camDist))
        Vec3 normal;  // rotated
        double px, py;  // supersampled image pixels
        double invD;    // 1 / depth (affine in screen space -- the z-buffer key)
        double u, v;    // design-space UV (§12 -- the cap overlay map's sampling domain)
        double su, sv;  // unrolled side coords (§12 wrap -- the wall overlay map's domain)
    };
    std::vector<TV> tv(mesh.vertices.size());
    double minPx = std::numeric_limits<double>::infinity(), minPy = minPx;
    double maxPx = -minPx, maxPy = -minPy;
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const ExtrudeVertex& v = mesh.vertices[i];
        TV& t = tv[i];
        t.view = cam.rotate(params, v.position);
        t.normal = params.orientation.rotate(v.normal);
        t.u = v.uv.x;
        t.v = v.uv.y;
        t.su = v.side.x;
        t.sv = v.side.y;
        double depth = 0.0;
        const Vec2 design = cam.project(t.view, depth);
        const Vec2 dev = toPixel.apply(design);
        t.px = dev.x * S;
        t.py = dev.y * S;
        t.invD = 1.0 / std::max(depth, 1e-6);
        minPx = std::min(minPx, t.px);
        minPy = std::min(minPy, t.py);
        maxPx = std::max(maxPx, t.px);
        maxPy = std::max(maxPy, t.py);
    }

    // The supersampled tile: the mesh's pixel AABB clipped to the destination.
    const long tx0 = std::max(0L, static_cast<long>(std::floor(minPx)) - 1);
    const long ty0 = std::max(0L, static_cast<long>(std::floor(minPy)) - 1);
    const long tx1 = std::min(static_cast<long>(dst.width) * S,
                              static_cast<long>(std::ceil(maxPx)) + 1);
    const long ty1 = std::min(static_cast<long>(dst.height) * S,
                              static_cast<long>(std::ceil(maxPy)) + 1);
    if (tx1 <= tx0 || ty1 <= ty0) return;
    const std::size_t tw = static_cast<std::size_t>(tx1 - tx0);
    const std::size_t th = static_cast<std::size_t>(ty1 - ty0);
    std::vector<float> color(tw * th * 4, 0.0f);
    std::vector<double> zbuf(tw * th, -1.0);  // stores invD; larger = closer; -1 = empty

    const Vec3 eye{0.0, 0.0, cam.camDist};
    // ⚠ BANDED OVER SCANLINES, and the z-buffer is why it has to be scanlines rather than
    // triangles. Every fragment below resolves against `zbuf`, so two threads taking different
    // TRIANGLES would race on the same texel and the winner would depend on timing. Rows do not:
    // a texel belongs to exactly one band, each band walks the ranges and the triangles inside
    // them in the SAME order the serial loop did, and its z-test therefore resolves identically.
    // Byte-identical, and deterministic, which a triangle split would not be either.
    //
    // This is the whole of the 3D type cost on a machine with no compute lane: a 200 px extruded
    // headline on a 1080p canvas spends 56 of its 66 ms right here, shading 2x2 supersampled
    // fragments one at a time. The per-band re-walk of the triangle list is an overlap test each,
    // which against a full Blinn-Phong shade per covered fragment is nothing.
    common::parallelFor(th, 32, [&](std::size_t band0, std::size_t band1) {
        const long bandLo = ty0 + static_cast<long>(band0);
        const long bandHi = ty0 + static_cast<long>(band1); // exclusive
        for (const ExtrudeMeshRange& range : mesh.ranges) {
            const Material& mat = materialForRun(params, range.runIndex);
            const float alpha = std::clamp(mat.albedo.a, 0.0f, 1.0f);
            if (alpha <= 0.0f)
                continue;
            const common::ImageF* ovMap =
                overlay != nullptr ? overlay->mapForRun(range.runIndex) : nullptr;
            const common::ImageF* wallMap =
                overlay != nullptr ? overlay->wallMapForRun(range.runIndex) : nullptr;
            for (std::uint32_t k = range.firstIndex; k + 2 < range.firstIndex + range.indexCount;
                 k += 3) {
                const TV& a = tv[mesh.indices[k]];
                const TV& b = tv[mesh.indices[k + 1]];
                const TV& c = tv[mesh.indices[k + 2]];
                // Cap vs side is a per-TRIANGLE fact (the mesher never mixes them in one triangle).
                const bool capTri = mesh.vertices[mesh.indices[k]].cap > 0.5f;
                // The FRONT cap carries the overlay design (§12). Wrap mode paints the WHOLE solid:
                // the back cap takes the design map too (mirrored from behind, like the back of a
                // painted sign), and walls/bevels take the UNROLLED wall map by their side coords.
                // Front = the cap whose model-space z is +depth/2.
                const bool frontTri = capTri && mesh.vertices[mesh.indices[k]].position.z > 0.0;
                const bool wrap = overlay != nullptr && overlay->wrapSides;
                const common::ImageF* triMap = nullptr;
                if (capTri && (frontTri || wrap))
                    triMap = ovMap;
                else if (!capTri && wrap)
                    triMap = wallMap;
                const double area = (b.px - a.px) * (c.py - a.py) - (c.px - a.px) * (b.py - a.py);
                if (std::abs(area) < 1e-12)
                    continue; // degenerate on screen
                const double inv = 1.0 / area;
                const long bx0 =
                    std::max(tx0, static_cast<long>(std::floor(std::min({a.px, b.px, c.px}))));
                const long by0 =
                    std::max(bandLo, static_cast<long>(std::floor(std::min({a.py, b.py, c.py}))));
                const long bx1 =
                    std::min(tx1 - 1, static_cast<long>(std::ceil(std::max({a.px, b.px, c.px}))));
                const long by1 = std::min(
                    bandHi - 1, static_cast<long>(std::ceil(std::max({a.py, b.py, c.py}))));
                if (by1 < by0)
                    continue; // this triangle does not reach this band
                for (long py = by0; py <= by1; ++py) {
                    for (long px = bx0; px <= bx1; ++px) {
                        const double sx = px + 0.5, sy = py + 0.5;
                        const double w0 =
                            ((b.px - sx) * (c.py - sy) - (c.px - sx) * (b.py - sy)) * inv;
                        const double w1 =
                            ((c.px - sx) * (a.py - sy) - (a.px - sx) * (c.py - sy)) * inv;
                        const double w2 = 1.0 - w0 - w1;
                        if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0)
                            continue;
                        // 1/depth is affine in screen space: interpolate it directly for the
                        // z-test.
                        const double invD = w0 * a.invD + w1 * b.invD + w2 * c.invD;
                        const std::size_t at = static_cast<std::size_t>(py - ty0) * tw +
                                               static_cast<std::size_t>(px - tx0);
                        if (invD <= zbuf[at])
                            continue;
                        zbuf[at] = invD;
                        // Perspective-correct attribute interpolation: attr/d is affine, so weight
                        // the barycentrics by each vertex's 1/d and renormalize.
                        const double q0 = w0 * a.invD, q1 = w1 * b.invD, q2 = w2 * c.invD;
                        const double qs = 1.0 / (q0 + q1 + q2);
                        const Vec3 n = (a.normal * q0 + b.normal * q1 + c.normal * q2) * qs;
                        const Vec3 p = (a.view * q0 + b.view * q1 + c.view * q2) * qs;
                        Vec3 nn = n.normalized();
                        const Vec3 viewDir =
                            cam.ortho ? Vec3{0.0, 0.0, 1.0} : (eye - p).normalized();
                        if (nn.dot(viewDir) < 0.0)
                            nn = -nn; // two-sided (no culling, no winding woes)
                        // §12: the overlay map replaces the albedo for this fragment (rgb only --
                        // coverage stays the material's), then shading proceeds unchanged, so the
                        // design is lit WITH the surface it sits on. Caps sample by design UV,
                        // walls/bevels (wrap mode) by their unrolled side coords.
                        Material tinted;
                        const Material* fragMat = &mat;
                        if (triMap != nullptr) {
                            const bool side = !capTri;
                            const double uu = side ? (a.su * q0 + b.su * q1 + c.su * q2) * qs
                                                   : (a.u * q0 + b.u * q1 + c.u * q2) * qs;
                            const double vv = side ? (a.sv * q0 + b.sv * q1 + c.sv * q2) * qs
                                                   : (a.v * q0 + b.v * q1 + c.v * q2) * qs;
                            const ColorF oc =
                                sampleEnv(*triMap, uu * triMap->width, vv * triMap->height);
                            tinted = mat;
                            tinted.albedo.r = oc.r;
                            tinted.albedo.g = oc.g;
                            tinted.albedo.b = oc.b;
                            fragMat = &tinted;
                        }
                        const Shaded s = shade(params, *fragMat, nn, viewDir, p, ec, capTri);
                        color[at * 4 + 0] = s.r;
                        color[at * 4 + 1] = s.g;
                        color[at * 4 + 2] = s.b;
                        color[at * 4 + 3] = alpha;
                    }
                }
            }
        }
    });

    compositeSupersampledTile(dst, color.data(), tx0, ty0, static_cast<long>(tw),
                              static_cast<long>(th), S);
}

void compositeSupersampledTile(common::ImageF& dst, const float* tile, long tx0, long ty0,
                               long tw, long th, int S) {
    if (tile == nullptr || tw <= 0 || th <= 0 || S <= 0) return;
    const long tx1 = tx0 + tw, ty1 = ty0 + th;
    const long dx0 = tx0 / S, dy0 = ty0 / S;
    const long dx1 = (tx1 + S - 1) / S, dy1 = (ty1 + S - 1) / S;
    // Banded like the rasteriser above, and for the same reason it is safe: one destination row is
    // resolved from its own S source rows and nothing else, so the bands touch disjoint texels.
    common::parallelFor(
        static_cast<std::size_t>(dy1 - dy0), 32, [&](std::size_t band0, std::size_t band1) {
            for (long dy = dy0 + static_cast<long>(band0), dyEnd = dy0 + static_cast<long>(band1);
                 dy < dyEnd; ++dy) {
                for (long dx = dx0; dx < dx1; ++dx) {
                    float ar = 0.0f, ag = 0.0f, ab = 0.0f, aa = 0.0f;
                    int taken = 0;
                    for (int sy = 0; sy < S; ++sy) {
                        for (int sx = 0; sx < S; ++sx) {
                            const long qx = dx * S + sx, qy = dy * S + sy;
                            if (qx < tx0 || qx >= tx1 || qy < ty0 || qy >= ty1) {
                                ++taken; // outside the tile = transparent sample
                                continue;
                            }
                            const std::size_t at = (static_cast<std::size_t>(qy - ty0) * tw +
                                                    static_cast<std::size_t>(qx - tx0)) *
                                                   4;
                            const float sa = tile[at + 3];
                            ar += tile[at + 0] * sa; // premultiplied accumulation
                            ag += tile[at + 1] * sa;
                            ab += tile[at + 2] * sa;
                            aa += sa;
                            ++taken;
                        }
                    }
                    if (aa <= 0.0f || taken == 0)
                        continue;
                    if (dx < 0 || dy < 0 || dx >= static_cast<long>(dst.width) ||
                        dy >= static_cast<long>(dst.height))
                        continue;
                    const float n = static_cast<float>(taken);
                    const float outA = aa / n;
                    const float pr = ar / n, pg = ag / n, pb = ab / n; // premultiplied average
                    const std::size_t at =
                        (static_cast<std::size_t>(dy) * dst.width + static_cast<std::size_t>(dx)) *
                        4;
                    const float da = dst.rgba[at + 3];
                    const float ra = outA + da * (1.0f - outA);
                    if (ra <= 0.0f)
                        continue;
                    dst.rgba[at + 0] = (pr + dst.rgba[at + 0] * da * (1.0f - outA)) / ra;
                    dst.rgba[at + 1] = (pg + dst.rgba[at + 1] * da * (1.0f - outA)) / ra;
                    dst.rgba[at + 2] = (pb + dst.rgba[at + 2] * da * (1.0f - outA)) / ra;
                    dst.rgba[at + 3] = ra;
                }
            }
        });
}

}  // namespace mosaic::core::text
