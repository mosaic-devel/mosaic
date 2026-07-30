#pragma once

#include <cstddef>
#include <map>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/layer_effects.hpp"
#include "core/text/extrude.hpp"
#include "core/text/extrude_mesh.hpp"

// Layer-Effects overlays on 3D text (docs/type-tool.md §12, S30-e). The colour / gradient /
// pattern overlays are evaluated in the glyph's UNEXTRUDED 2D design space -- never over the
// rendered 3D pixel rectangle -- and baked into per-material "overlay albedo" maps: each texel is
// the overlay stack composited (its own blend mode + opacity, canonical colour -> gradient ->
// pattern order) over that material's constant albedo. Both render lanes then sample ONE map by
// the mesh's per-vertex design-space UVs -- the front cap always, the walls/bevels only when
// Extrude::overlayWrapSides asks for it -- and shade with the sampled colour, so the design sits
// ON the surface and is lit with it. Baking on the CPU keeps the blend-mode math in exactly one
// place and makes the Vulkan lane's sampling parity-trivial (same bilinear formula, same texels).
//
// Sampling semantics mirror the 2D effect renderer (layer_effects_render's paintAtNorm): a
// gradient spans the design domain normalized to [0,1]^2; a pattern tiles in real design px with
// its fixed feature size, phase-anchored at the domain's top-left corner. A pattern's
// anchorToCanvas has no meaning on a solid's surface (the face is a rotated 3D plane, not the
// canvas) and is treated as layer-anchored here.
namespace mosaic::core::text {

// The baked maps for one render. `uvDomain` is the design rect the mesh's UVs are normalized
// over (the ink bbox at mesh build time -- capture it BEFORE renderTextF re-bases designBounds).
//
// Wrap mode (S30-e feedback 2026-07-16) carries a SECOND map per material: the walls/bevels
// sample it by their UNROLLED side coordinates (ExtrudeVertex::side -- outline arc length x
// depth), NOT the flat design UV. Sampling the design map on a wall repeats one outline point's
// colour down the whole depth, which stretches any pattern into ruler lines ("turns dots pattern
// into stretched out lines"); the unrolled domain is the label-wrapped-around-the-solid surface a
// pattern tiles undistorted in. Per texel: patterns evaluate at real unrolled design px; colours
// are constant; gradients take the design-space continuation (the colour of the outline point the
// texel's s maps back to, via the mesh's SideStations) -- so a gradient still flows around the
// solid like paint, only textures unroll. In wrap mode the BACK CAP joins too (the design map at
// its UV; it reads mirrored from behind, like the back of a painted sign).
struct ExtrudeOverlay {
    bool wrapSides = false;
    std::vector<common::ImageF> maps;             // deduped by resolved material albedo
    std::vector<common::ImageF> wallMaps;         // wrap mode only; parallel to `maps`
    std::map<std::size_t, std::size_t> runToMap;  // mesh run index -> maps[]/wallMaps[] slot

    [[nodiscard]] const common::ImageF* mapForRun(std::size_t run) const {
        const auto it = runToMap.find(run);
        return it != runToMap.end() ? &maps[it->second] : nullptr;
    }
    [[nodiscard]] const common::ImageF* wallMapForRun(std::size_t run) const {
        const auto it = runToMap.find(run);
        return it != runToMap.end() && it->second < wallMaps.size() ? &wallMaps[it->second]
                                                                    : nullptr;
    }
    [[nodiscard]] bool empty() const noexcept { return maps.empty(); }
};

// Any of the three overlays enabled with a paint that draws? (NoPaint overlays are z-order
// placeholders, exactly as in the 2D renderer.)
[[nodiscard]] bool extrudeOverlaysActive(const LayerEffects& fx);

// Build the per-material maps for `mesh` (one per distinct resolved albedo across its ranges).
// `pixelScale` is the design-unit -> device-pixel magnification the render bakes (texel density
// matches the output, so the map never softens the result); `antialias` is the document-wide AA
// setting pattern edges follow, exactly like the 2D path. Returns an empty overlay when no
// overlay draws.
[[nodiscard]] ExtrudeOverlay buildExtrudeOverlay(const LayerEffects& fx, const ExtrudeMesh& mesh,
                                                 const Extrude& params,
                                                 const common::Rect& uvDomain, double pixelScale,
                                                 bool antialias);

}  // namespace mosaic::core::text
