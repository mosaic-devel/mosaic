#pragma once

#include <functional>
#include <optional>

#include "common/geometry.hpp"
#include "common/geometry3d.hpp"
#include "common/image.hpp"
#include "core/text/extrude.hpp"
#include "core/text/extrude_mesh.hpp"

// The CPU render lane for extruded text (docs/type-tool.md §10.3/§10.5, S30-c): a z-buffered
// software rasterizer with toggle-gated Blinn-Phong shading, drawing the solid into the same
// ImageF the 2D text path fills -- so 3D text rides the existing layer cache/compositor with zero
// new machinery. This is the bring-up/test lane; the Vulkan pass renders through the SAME camera
// derivation (ExtrudeCamera) so the two lanes stay parity-testable. Blinn-Phong (1977) and the
// pinhole camera are textbook public-domain technique.
namespace mosaic::core::text {

using common::Vec2;
using common::Vec3;

// The camera both lanes derive from the extrude params: a pinhole at `camDist` along +z over the
// solid's pivot (the design-bounds centre at z = 0), FOV from Extrude::perspective. The z = 0
// plane projects SCALE-TRUE -- at identity orientation the solid's mid-depth plane lands exactly
// where the flat 2D text would, so enabling 3D never relocates the block. perspective -> 0 is
// true orthographic.
struct ExtrudeCamera {
    Vec3 center;         // rotation pivot (design-bounds centre, z = 0)
    double camDist = 0;  // eye distance from the pivot along +z (unused when ortho)
    bool ortho = false;

    [[nodiscard]] static ExtrudeCamera from(const common::Rect& designBounds,
                                            const Extrude& params);

    // Rotate a design-space point/normal into camera-facing space (pivot-relative).
    [[nodiscard]] Vec3 rotate(const Extrude& params, Vec3 p) const {
        return params.orientation.rotate(p - center);
    }
    // Project a pivot-relative point onto the design plane; `depth` (out) grows away from the eye.
    [[nodiscard]] Vec2 project(Vec3 pr, double& depth) const;
};

// A conservative design-space AABB of the rendered solid: the 2D bounds swelled by the bevels'
// possible outward bulge, extruded to a box, rotated and projected through the same camera. Used
// to size the layer's pixel cache so a rotated/deep solid is never clipped to the flat layout box.
[[nodiscard]] common::Rect projectedExtrudeBounds(const common::Rect& bounds2d,
                                                  const Extrude& params);

// The front-cap plane map (S30-d round 2): where a FLAT layer-local point lands once the block is
// extruded -- the glyphs the user reads live on the front cap (z = +depth/2), rotated + projected
// through the same ExtrudeCamera the render lanes use. The editing chrome (caret, selection,
// squiggles, the edit box) projects its geometry through this so it hugs the 3D text; hit-testing
// runs the inverse (an eye-ray/plane intersection). Perspective makes it a homography, so there
// is no affine shortcut. Build it from the SAME designBounds the render used (renderTextF bases
// both on the shaped block's bounds) or the pivots diverge.
struct ExtrudePlaneMap {
    ExtrudeCamera cam;
    common::Quat orientation;
    double zPlane = 0.0;  // the front cap

    [[nodiscard]] static ExtrudePlaneMap from(const common::Rect& designBounds,
                                              const Extrude& params);
    [[nodiscard]] Vec2 project(Vec2 designPt) const;
    // nullopt when the plane is edge-on to the eye ray (no meaningful design point).
    [[nodiscard]] std::optional<Vec2> unproject(Vec2 projectedPt) const;
};

// The reflection environment (feedback 2026-07-03): a snapshot of the document composited BELOW
// the text layer, in the layer's local frame. When Extrude::reflectCanvas is set and a snapshot
// is provided, metal reflections intersect each reflected view ray with the canvas plane at the
// solid's back cap and sample it (bilinear, edge-clamped, straight alpha; transparent texels fall
// through to the procedural studio). The app builds it (core cannot composite documents).
struct ExtrudeEnv {
    const common::ImageF* image = nullptr;  // null/empty = studio only
    common::Affine2D layerToEnv;            // layer-local design point -> env image pixel
};

struct ExtrudeOverlay;  // extrude_overlay.hpp: baked Layer-Effects overlay maps (§12, S30-e)

// Rasterize `mesh` over `dst` (straight-alpha linear float RGBA, composited source-over).
// `toPixel` is the same design-space -> image-pixel transform the 2D path bakes (device-space
// crispness for free); `antialias` supersamples 2x2 (block.aa != None). Materials resolve per
// §10.4: the range's run override or the shared default. `env` is the optional canvas-reflection
// snapshot (used only when params.reflectCanvas); `overlay` the optional baked Layer-Effects
// overlay maps -- front-cap fragments (and, with overlay->wrapSides, walls/bevels) shade with the
// map sampled at their design-space UV instead of the constant albedo. `palette` is the colour
// each run paints with -- the layer's OWN colour (§10.4), supplied per render rather than stored,
// so 3D colour and 2D colour are one thing. Renders through the injected override (the Vulkan
// lane) when one is set and succeeds; the CPU rasterizer is the always-there fallback.
void renderExtrudeMeshF(common::ImageF& dst, const ExtrudeMesh& mesh, const Extrude& params,
                        const ExtrudePalette& palette, const common::Affine2D& toPixel,
                        bool antialias, const ExtrudeEnv* env = nullptr,
                        const ExtrudeOverlay* overlay = nullptr);

// The GPU lane's injection seam (§10.5): core stays Vulkan-free, so the app registers the Vulkan
// pass here at startup (render::ExtrudeGpu). Return true = rendered; false = fall back to the CPU
// lane (device lost, no Vulkan, etc.). Set once before rendering starts; not thread-guarded.
using ExtrudeRenderOverride =
    std::function<bool(common::ImageF& dst, const ExtrudeMesh& mesh, const Extrude& params,
                       const ExtrudePalette& palette, const common::Affine2D& toPixel,
                       bool antialias, const ExtrudeEnv* env, const ExtrudeOverlay* overlay)>;
void setExtrudeRenderOverride(ExtrudeRenderOverride fn);

// Shared by both lanes: box-downsample a supersampled straight-alpha float RGBA tile (origin
// tx0/ty0 in S-scaled pixels, tw x th samples) and source-over it onto `dst`. Exposed so the
// Vulkan lane's readback composites bit-comparably with the CPU lane.
void compositeSupersampledTile(common::ImageF& dst, const float* tile, long tx0, long ty0,
                               long tw, long th, int S);

}  // namespace mosaic::core::text
