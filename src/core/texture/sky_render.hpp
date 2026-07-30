#pragma once

#include <cstddef>
#include <cstdint>

#include "common/image.hpp"
#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"  // TextureWindow / TextureRenderProgress

// The S55-b CPU sky renderer (docs/texture-generator.md §4): Hosek-Wilkie dome + solar-tinted
// sun disc/aureole + Rayleigh/Mie aerial perspective + the §4.3 2D cloud catalogue on §4.5
// projected altitude planes, composited per §3.4 into a straight-alpha float image. Same purity
// contract as every generator: a function of (params, w, h) only.
namespace mosaic::core::texture {

// `w`/`h` are the FULL frame (the camera is built from them); `window` evaluates a byte-exact
// sub-rect of it and `progress` carries the per-row progress/cancel channel (both S55-f, see
// texture_render.hpp).
[[nodiscard]] common::ImageF renderSkyTexture(const TextureParams& params, const SkyParams& sky,
                                              std::uint32_t w, std::uint32_t h,
                                              const TextureWindow& window = {},
                                              TextureRenderProgress* progress = nullptr);

// The §7.4 sky preset library (S55-f) -- each entry is a complete SkyParams value the dialog
// offers by name (sliders fine-tune from there; the paper/grass/type3d preset pattern). Data
// only; ordering is stable (tests pin the count + names).
struct SkyPreset {
    const char* name;
    SkyParams params;
};
[[nodiscard]] std::size_t skyPresetCount();
[[nodiscard]] const SkyPreset& skyPreset(std::size_t i);  // i < skyPresetCount()

}  // namespace mosaic::core::texture
