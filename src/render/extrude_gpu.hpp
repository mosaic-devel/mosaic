#pragma once

#include <memory>
#include <string>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/text/extrude.hpp"
#include "core/text/extrude_mesh.hpp"
#include "core/text/extrude_overlay.hpp"  // ExtrudeOverlay (baked §12 overlay maps, S30-e)
#include "core/text/extrude_render.hpp"   // ExtrudeEnv (the canvas-reflection snapshot)

namespace mosaic::render {

// The Vulkan lane of the extruded-text renderer (docs/type-tool.md §10.5, S30-c): the compute
// rasterizer in shaders/extrude_raster.comp, dispatched twice (depth prepass, shade pass) over an
// offscreen supersampled tile, read back and composited through the SAME
// core::text::compositeSupersampledTile the CPU lane uses. The app injects render() as
// core::text::setExtrudeRenderOverride, so 3D text renders on the GPU wherever Vulkan exists and
// falls back to the CPU lane (return false) everywhere else -- the §10.5 dual-lane discipline.
//
// PERSISTENT: one context/pipeline for the object's lifetime, and the mesh's vertex/index/
// material-slot buffers are cached by content hash -- re-uploaded only when the solid's GEOMETRY
// changes; rotate/light/material edits re-dispatch with fresh per-render params only (the §10.5
// residency model). The readback is the price of today's CPU compositor and leaves with S60.
class ExtrudeGpu {
public:
    // nullptr (with `error` set) when no usable Vulkan device exists.
    static std::unique_ptr<ExtrudeGpu> create(bool enableValidation, std::string& error);
    ~ExtrudeGpu();
    ExtrudeGpu(const ExtrudeGpu&) = delete;
    ExtrudeGpu& operator=(const ExtrudeGpu&) = delete;

    // The core-hook contract: rasterize `mesh` over `dst`. false = fall back to the CPU lane.
    // `env` is the optional canvas-reflection snapshot (used only when params.reflectCanvas);
    // `overlay` the optional baked Layer-Effects overlay maps (S30-e §12) the front cap -- and,
    // with overlay->wrapSides, the walls/bevels -- shade with instead of the constant albedo.
    bool render(common::ImageF& dst, const core::text::ExtrudeMesh& mesh,
                const core::text::Extrude& params, const common::Affine2D& toPixel,
                bool antialias, const core::text::ExtrudeEnv* env = nullptr,
                const core::text::ExtrudeOverlay* overlay = nullptr);

private:
    ExtrudeGpu();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mosaic::render
