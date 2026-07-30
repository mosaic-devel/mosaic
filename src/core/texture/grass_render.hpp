#pragma once

#include <cstddef>
#include <cstdint>

#include "common/image.hpp"
#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"  // TextureWindow / TextureRenderProgress

// The S55-e grass renderer (docs/texture-generator.md §6): a distance-graded HYBRID. A ground-plane
// homography (sky_camera.hpp's GrassCamera) recedes the lawn to a horizon; a procedural turf base
// carries the far field and the ground between blades; a single-class jittered (Poisson-disk-style)
// scatter roots quadratic Bezier blades whose count per IMAGE area falls with depth (the cost
// lever, so the horizon costs nothing); each blade is Kajiya-Kay tangent-lit with wrap-translucency
// and root AO; blades composite back-to-front (far->near) over the turf. Pure function of
// (params, w, h) -- the same determinism contract as the rest of the kit (§8.3): every random
// draw is hash-seeded on the integer scatter lattice, and the back-to-front composite is banded so
// any thread count is byte-identical to serial.
namespace mosaic::core::texture {

// Render the grass generator into the 8-bit lane. `p` carries seed + Scale (blade/clump feature
// size); `grass` the lawn knobs. Straight alpha: opaque where the turf base is enabled, else 255
// only under blade silhouettes (§3.4 -- a grass fringe over a transparent ground).
// `w`/`h` are the FULL frame (camera, blade instancing and LOD are all frame-based); `window`
// evaluates a byte-exact sub-rect and `progress` carries the per-row progress/cancel channel
// (both S55-f).
[[nodiscard]] common::Image renderGrass(const TextureParams& p, const GrassParams& grass,
                                        std::uint32_t w, std::uint32_t h,
                                        const TextureWindow& window = {},
                                        TextureRenderProgress* progress = nullptr);

// The §6 preset library -- each entry is a complete GrassParams value the S55-f dialog offers by
// name (sliders fine-tune from there, the paper/type3d preset pattern). Data only; ordering is
// stable (tests pin the count + names).
struct GrassPreset {
    const char* name;
    GrassParams params;
};
[[nodiscard]] std::size_t grassPresetCount();
[[nodiscard]] const GrassPreset& grassPreset(std::size_t i);  // i < grassPresetCount()

}  // namespace mosaic::core::texture
