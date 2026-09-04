#include "core/text/extrude_render.hpp"

#include "common/thread_pool.hpp"
#include "core/text/extrude_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
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

// Everything a shade needs that does NOT vary per fragment, resolved once per render.
//
// It is all loop-invariant and it was all being redone per covered sample: an exp2 and four
// clamps per fragment for the material terms, and -- the one that actually hurt -- a
// `(-light.direction).normalized()` per LIGHT per fragment, which is a sqrt and three divides to
// re-derive a constant. The albedo stays out of here on purpose: an overlay map (§12) replaces it
// per fragment, so the F0 split that depends on it is the one thing left inline.
struct ShadeLight {
    Vec3 l; // surface -> lamp, unit (the pre-normalized -direction)
    ColorF color;
    double intensity = 1.0;
};
struct ShadeConst {
    double metal = 0.0, rough = 0.5, kd = 1.0, shininess = 4.0, specGain = 1.0;
    std::vector<ShadeLight> lights;

    [[nodiscard]] static ShadeConst from(const Extrude& params, const Material& m) {
        ShadeConst c;
        c.metal = std::clamp(static_cast<double>(m.metalness), 0.0, 1.0);
        c.rough = std::clamp(static_cast<double>(m.roughness), 0.0, 1.0);
        c.kd = 1.0 - c.metal;                                 // a true metal has no diffuse lobe
        c.shininess = std::exp2(2.0 + 8.0 * (1.0 - c.rough)); // 4 .. 1024
        c.specGain = 0.25 + 0.75 * (1.0 - c.rough);           // rough surfaces spread it thin
        for (const Light& light : params.lights) {
            const Vec3 l = (-light.direction).normalized(); // surface -> lamp
            if (l == Vec3{})
                continue; // a zero direction is not a light (the old inline skip)
            c.lights.push_back({l, light.color, light.intensity});
        }
        return c;
    }
};

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
// `ar/ag/ab` is the fragment's albedo -- the material's, or the overlay map's sample where one
// applies -- passed separately from `sc` for exactly that reason.
Shaded shade(const Extrude& params, const ShadeConst& sc, float ar, float ag, float ab, Vec3 n,
             Vec3 viewDir, Vec3 p, const EnvContext& ec, bool capSurface) {
    if (!params.lightingEnabled) return {ar, ag, ab};  // flat self-lit faces

    const double metal = sc.metal;
    const double rough = sc.rough;
    const double kd = sc.kd;
    const double ksR = 0.06 + (ar - 0.06) * metal;      // dielectric 6% -> tinted metal spec (F0)
    const double ksG = 0.06 + (ag - 0.06) * metal;
    const double ksB = 0.06 + (ab - 0.06) * metal;
    const double shininess = sc.shininess;
    const double specGain = sc.specGain;

    double r = ar * params.ambient.r * kd, g = ag * params.ambient.g * kd,
           b = ab * params.ambient.b * kd;
    for (const ShadeLight& light : sc.lights) {
        const Vec3 l = light.l;
        const double ndl = std::max(0.0, n.dot(l));
        const double di = light.intensity;
        if (ndl > 0.0) {
            r += ar * kd * light.color.r * di * ndl;
            g += ag * kd * light.color.g * di * ndl;
            b += ab * kd * light.color.b * di * ndl;
            const Vec3 h = (l + viewDir).normalized();
            const double ndh = n.dot(h);
            // pow() is the single most expensive call in the shade; a back-facing half-vector
            // makes it pow(0, shininess) == 0, which is a pure waste of it.
            const double spec = ndh > 0.0 ? std::pow(ndh, shininess) * specGain * di : 0.0;
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
                        const ExtrudePalette& palette, const common::Affine2D& toPixel,
                        bool antialias, const ExtrudeEnv* env, const ExtrudeOverlay* overlay) {
    if (mesh.empty() || dst.width == 0 || dst.height == 0) return;
    if (overlay != nullptr && overlay->empty()) overlay = nullptr;  // nothing baked = no overlay
    // The Vulkan lane, when the app injected one and it can serve this render (§10.5); the CPU
    // rasterizer below stays the always-there fallback (headless tests, no-Vulkan machines).
    if (g_override && g_override(dst, mesh, params, palette, toPixel, antialias, env, overlay))
        return;
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

    // Per-RANGE shading setup, resolved once. This used to be redone for every range inside every
    // band -- a std::map lookup (materialForRun) and two overlay lookups each.
    struct RangeInfo {
        const Material* mat = nullptr;
        ColorF color; // the run's own colour -- the layer's, via the palette
        float alpha = 0.0f;
        const common::ImageF* capMap = nullptr;  // the front-cap design map (§12)
        const common::ImageF* wallMap = nullptr; // the unrolled wall map (§12 wrap)
        ShadeConst sc;
    };
    std::vector<RangeInfo> rinfo(mesh.ranges.size());
    for (std::size_t i = 0; i < mesh.ranges.size(); ++i) {
        RangeInfo& ri = rinfo[i];
        const std::size_t run = mesh.ranges[i].runIndex;
        ri.mat = &materialForRun(params, run);
        ri.color = palette.forRun(run);
        ri.alpha = std::clamp(ri.color.a, 0.0f, 1.0f);
        ri.capMap = overlay != nullptr ? overlay->mapForRun(run) : nullptr;
        ri.wallMap = overlay != nullptr ? overlay->wallMapForRun(run) : nullptr;
        ri.sc = ShadeConst::from(params, *ri.mat);
    }

    // ⚠ SETUP ONCE, BIN ONCE. The previous shape of this loop re-walked the WHOLE index buffer in
    // every band and redid each triangle's projection AABB, its material lookup and its cap/front/
    // overlay classification there -- work proportional to bands x triangles that answered the
    // same question every time. On a 41-lobe stroked rosette (40,854 triangles, 8 bands) that was
    // 327,000 re-derivations, and it was the majority of the render: the mesh is big, the bands
    // are few, and most triangles miss most bands.
    //
    // So: classify each triangle once into a screen-space record, then bucket it into the row
    // bands its AABB actually touches. A band then walks only the triangles that reach it.
    struct Tri {
        std::uint32_t v0 = 0, v1 = 0, v2 = 0;
        std::uint32_t range = 0;
        double inv = 0.0; // 1 / signed area
        // The three edge functions' dw/dx. They are constant over the whole triangle (only the
        // constant term moves with the scanline), which is what makes the per-row span below two
        // divides rather than a search.
        double a0 = 0.0, a1 = 0.0, a2 = 0.0;
        long x0 = 0, x1 = 0, y0 = 0, y1 = 0; // AABB, already clipped to the tile
        const common::ImageF* map = nullptr; // the overlay map this triangle samples, or none
        bool cap = false;
    };
    const bool wrap = overlay != nullptr && overlay->wrapSides;
    std::vector<Tri> tris;
    tris.reserve(mesh.indices.size() / 3);
    for (std::size_t r = 0; r < mesh.ranges.size(); ++r) {
        const ExtrudeMeshRange& range = mesh.ranges[r];
        if (rinfo[r].alpha <= 0.0f)
            continue; // fully transparent run: no ink, and no z either
        for (std::uint32_t k = range.firstIndex; k + 2 < range.firstIndex + range.indexCount;
             k += 3) {
            Tri t;
            t.v0 = mesh.indices[k];
            t.v1 = mesh.indices[k + 1];
            t.v2 = mesh.indices[k + 2];
            const TV& a = tv[t.v0];
            const TV& b = tv[t.v1];
            const TV& c = tv[t.v2];
            const double area = (b.px - a.px) * (c.py - a.py) - (c.px - a.px) * (b.py - a.py);
            if (std::abs(area) < 1e-12)
                continue; // degenerate on screen
            t.x0 = std::max(tx0, static_cast<long>(std::floor(std::min({a.px, b.px, c.px}))));
            t.x1 = std::min(tx1 - 1, static_cast<long>(std::ceil(std::max({a.px, b.px, c.px}))));
            if (t.x1 < t.x0)
                continue;
            t.y0 = std::max(ty0, static_cast<long>(std::floor(std::min({a.py, b.py, c.py}))));
            t.y1 = std::min(ty1 - 1, static_cast<long>(std::ceil(std::max({a.py, b.py, c.py}))));
            if (t.y1 < t.y0)
                continue; // entirely off the tile
            t.range = static_cast<std::uint32_t>(r);
            t.inv = 1.0 / area;
            t.a0 = (b.py - c.py) * t.inv;
            t.a1 = (c.py - a.py) * t.inv;
            t.a2 = (a.py - b.py) * t.inv;
            // Cap vs side is a per-TRIANGLE fact (the mesher never mixes them in one triangle).
            t.cap = mesh.vertices[t.v0].cap > 0.5f;
            // The FRONT cap carries the overlay design (§12). Wrap mode paints the WHOLE solid:
            // the back cap takes the design map too (mirrored from behind, like the back of a
            // painted sign), and walls/bevels take the UNROLLED wall map by their side coords.
            // Front = the cap whose model-space z is +depth/2.
            const bool front = t.cap && mesh.vertices[t.v0].position.z > 0.0;
            if (t.cap && (front || wrap))
                t.map = rinfo[r].capMap;
            else if (!t.cap && wrap)
                t.map = rinfo[r].wallMap;
            tris.push_back(t);
        }
    }
    if (tris.empty())
        return;

    // Row bands, finer than one-per-thread: the pool hands them out as workers free up, so a band
    // that lands on the dense middle of the solid does not stall a whole core the way an eighth of
    // the tile did.
    const std::size_t bandH =
        std::max<std::size_t>(32, (th + common::hardwareThreads() * 4 - 1) /
                                      std::max<std::size_t>(1, common::hardwareThreads() * 4));
    const std::size_t nBands = (th + bandH - 1) / bandH;
    std::vector<std::vector<std::uint32_t>> bins(nBands);
    for (std::uint32_t i = 0; i < tris.size(); ++i) {
        const std::size_t b0 = static_cast<std::size_t>(tris[i].y0 - ty0) / bandH;
        const std::size_t b1 = static_cast<std::size_t>(tris[i].y1 - ty0) / bandH;
        for (std::size_t b = b0; b <= b1 && b < nBands; ++b)
            bins[b].push_back(i);
    }

    // ⚠ A VISIBILITY pass, not a shading one -- the change that took the fragment cost off the
    // depth complexity. An extruded solid is 2-4 surfaces deep at any pixel (front cap, walls,
    // back cap), and this loop used to run the FULL fragment program -- two normalizations, a
    // perspective divide, an overlay sample and a whole Blinn-Phong shade with its pow() -- for
    // every one of those, then throw all but the nearest away. Now it resolves depth only, keeps
    // the winner's triangle and barycentrics, and the resolve pass below shades each surviving
    // sample exactly once.
    //
    // The barycentrics are STORED rather than re-derived from the sample position because the
    // resolve must shade the same point the depth test accepted, bit for bit; recomputing the
    // edge functions at a second call site invites the compiler to contract them differently.
    //
    // Banded over scanlines, and the z-buffer is why it has to be scanlines rather than triangles.
    // Every fragment resolves against `zbuf`, so two threads taking different TRIANGLES would race
    // on the same texel and the winner would depend on timing. Rows do not: a texel belongs to
    // exactly one band, each band walks its triangles in the SAME order the serial loop did, and
    // its z-test therefore resolves identically. Byte-identical, and deterministic, which a
    // triangle split would not be either.
    const std::size_t nSamples = tw * th;
    std::vector<double> zbuf(nSamples, -1.0); // stores invD; larger = closer; < 0 = empty
    // Written only where zbuf says a fragment landed, so they never need clearing.
    auto gtri = std::make_unique_for_overwrite<std::uint32_t[]>(nSamples);
    auto gw0 = std::make_unique_for_overwrite<double[]>(nSamples);
    auto gw1 = std::make_unique_for_overwrite<double[]>(nSamples);

    common::parallelBands(nBands, [&](std::size_t band) {
        const long bandLo = ty0 + static_cast<long>(band * bandH);
        const long bandHi = std::min(ty1, ty0 + static_cast<long>((band + 1) * bandH));
        for (const std::uint32_t ti : bins[band]) {
            const Tri& t = tris[ti];
            const TV& a = tv[t.v0];
            const TV& b = tv[t.v1];
            const TV& c = tv[t.v2];
            const long by0 = std::max(bandLo, t.y0);
            const long by1 = std::min(bandHi - 1, t.y1);
            const double inv = t.inv;
            for (long py = by0; py <= by1; ++py) {
                const double sy = py + 0.5;
                double* zrow = zbuf.data() + static_cast<std::size_t>(py - ty0) * tw;
                // ⚠ SOLVE for the covered span, do not SCAN for it. Each edge function is affine
                // in x along a scanline -- w_i(sx) = c_i + a_i * sx -- so the covered run is the
                // intersection of three half-lines, which is two divides.
                //
                // This is where an extruded solid's raster time went. Its triangles are slivers:
                // every wall quad, every bevel ring segment is a long thin diagonal whose
                // AXIS-ALIGNED box is mostly not the triangle. Measured on a 4-lobe stroked
                // rosette, walking those boxes tested 11.0M samples over an 838k-sample tile --
                // 13x the tile -- and 87.8% of the tests answered "outside". The barycentric
                // rejection was the rasteriser.
                //
                // The span only BOUNDS the loop; the exact sign test below is unchanged and still
                // decides coverage, and the bounds carry a one-sample guard on each side, so a
                // rounding difference between the solved root and the tested edge function costs
                // a redundant test rather than a dropped fragment. Coverage is bit-identical.
                const double ep = c.py - sy, eq = b.py - sy, er = a.py - sy;
                const double c0 = (b.px * ep - c.px * eq) * inv;
                const double c1 = (c.px * er - a.px * ep) * inv;
                const double c2 = 1.0 - c0 - c1;
                double lo = -std::numeric_limits<double>::infinity();
                double hi = std::numeric_limits<double>::infinity();
                bool none = false;
                const double ea[3] = {t.a0, t.a1, t.a2};
                const double ec3[3] = {c0, c1, c2};
                for (int e = 0; e < 3; ++e) {
                    if (ea[e] > 0.0)
                        lo = std::max(lo, -ec3[e] / ea[e]);
                    else if (ea[e] < 0.0)
                        hi = std::min(hi, -ec3[e] / ea[e]);
                    else if (ec3[e] < 0.0)
                        none = true; // constant and negative: the whole row is outside
                }
                if (none || lo > hi)
                    continue;
                long xs = t.x0, xe = t.x1;
                if (std::isfinite(lo))
                    xs = std::max(xs, static_cast<long>(std::ceil(lo - 0.5)) - 1);
                if (std::isfinite(hi))
                    xe = std::min(xe, static_cast<long>(std::floor(hi - 0.5)) + 1);
                for (long px = xs; px <= xe; ++px) {
                    const double sx = px + 0.5;
                    const double w0 = ((b.px - sx) * (c.py - sy) - (c.px - sx) * (b.py - sy)) * inv;
                    const double w1 = ((c.px - sx) * (a.py - sy) - (a.px - sx) * (c.py - sy)) * inv;
                    const double w2 = 1.0 - w0 - w1;
                    if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0)
                        continue;
                    // 1/depth is affine in screen space: interpolate it directly for the z-test.
                    const double invD = w0 * a.invD + w1 * b.invD + w2 * c.invD;
                    const std::size_t col = static_cast<std::size_t>(px - tx0);
                    if (invD <= zrow[col])
                        continue;
                    zrow[col] = invD;
                    const std::size_t at = static_cast<std::size_t>(py - ty0) * tw + col;
                    gtri[at] = ti;
                    gw0[at] = w0;
                    gw1[at] = w1;
                }
            }
        }
    });

    // Shade the survivors and resolve straight into `dst`. Fusing the shade, the box downsample
    // and the source-over is what lets the whole S-scaled RGBA colour tile go: on a full-canvas
    // solid at 1080p that buffer was 133 MB to allocate, zero and stream through, for numbers no
    // other pass ever read. The arithmetic below is compositeSupersampledTile's, unchanged --
    // that entry point stays for the Vulkan lane's readback, which has real texels to composite.
    const Vec3 eye{0.0, 0.0, cam.camDist};
    const long dx0 = tx0 / S, dy0 = ty0 / S;
    const long dx1 = (tx1 + S - 1) / S, dy1 = (ty1 + S - 1) / S;
    const float sampleN = static_cast<float>(S * S); // `taken` was always S*S
    // Banded finer than one-per-thread, and dynamically claimed, for the same reason the
    // visibility pass is: a solid is fattest across its middle, so eight equal slices of it hand
    // the middle threads roughly twice the covered samples the top and bottom ones get, and the
    // pass takes as long as its slowest slice. Destination rows are disjoint, so the split is free
    // to be as fine as it likes.
    const std::size_t rows = static_cast<std::size_t>(dy1 - dy0);
    const std::size_t rowsPerBand =
        std::max<std::size_t>(16, (rows + common::hardwareThreads() * 4 - 1) /
                                      std::max<std::size_t>(1, common::hardwareThreads() * 4));
    common::parallelBands((rows + rowsPerBand - 1) / rowsPerBand, [&](std::size_t band) {
        const std::size_t r0 = band * rowsPerBand;
        const std::size_t r1 = std::min(rows, r0 + rowsPerBand);
        for (long dy = dy0 + static_cast<long>(r0), dyEnd = dy0 + static_cast<long>(r1); dy < dyEnd;
             ++dy) {
            if (dy < 0 || dy >= static_cast<long>(dst.height))
                continue;
            for (long dx = dx0; dx < dx1; ++dx) {
                if (dx < 0 || dx >= static_cast<long>(dst.width))
                    continue;
                float ar = 0.0f, ag = 0.0f, ab = 0.0f, aa = 0.0f;
                for (int sy = 0; sy < S; ++sy) {
                    for (int sx = 0; sx < S; ++sx) {
                        const long qx = dx * S + sx, qy = dy * S + sy;
                        if (qx < tx0 || qx >= tx1 || qy < ty0 || qy >= ty1)
                            continue; // outside the tile = transparent sample
                        const std::size_t at = static_cast<std::size_t>(qy - ty0) * tw +
                                               static_cast<std::size_t>(qx - tx0);
                        if (zbuf[at] < 0.0)
                            continue; // nothing landed here = transparent sample
                        const Tri& t = tris[gtri[at]];
                        const RangeInfo& ri = rinfo[t.range];
                        const TV& a = tv[t.v0];
                        const TV& b = tv[t.v1];
                        const TV& c = tv[t.v2];
                        const double w0 = gw0[at], w1 = gw1[at];
                        const double w2 = 1.0 - w0 - w1;
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
                        float mr = ri.color.r, mg = ri.color.g, mb = ri.color.b;
                        if (t.map != nullptr) {
                            const bool side = !t.cap;
                            const double uu = side ? (a.su * q0 + b.su * q1 + c.su * q2) * qs
                                                   : (a.u * q0 + b.u * q1 + c.u * q2) * qs;
                            const double vv = side ? (a.sv * q0 + b.sv * q1 + c.sv * q2) * qs
                                                   : (a.v * q0 + b.v * q1 + c.v * q2) * qs;
                            const ColorF oc =
                                sampleEnv(*t.map, uu * t.map->width, vv * t.map->height);
                            mr = oc.r;
                            mg = oc.g;
                            mb = oc.b;
                        }
                        const Shaded s =
                            shade(params, ri.sc, mr, mg, mb, nn, viewDir, p, ec, t.cap);
                        const float sa = ri.alpha;
                        ar += s.r * sa; // premultiplied accumulation
                        ag += s.g * sa;
                        ab += s.b * sa;
                        aa += sa;
                    }
                }
                if (aa <= 0.0f)
                    continue;
                const float outA = aa / sampleN;
                const float pr = ar / sampleN, pg = ag / sampleN,
                            pb = ab / sampleN; // premultiplied average
                const std::size_t at =
                    (static_cast<std::size_t>(dy) * dst.width + static_cast<std::size_t>(dx)) * 4;
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
