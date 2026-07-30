#pragma once

#include <cstddef>
#include <cstdint>

#include "common/image.hpp"
#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"  // TextureWindow / TextureRenderProgress

// The S55-g follow-on materials (docs/texture-generator.md §1.1): wood, marble, stone, canvas and
// metal, all "data over §5" -- each is a height-field recipe + an albedo mapping over the SAME
// engine as paper (noise-kit height -> SINGLE-PASS Sobel normal -> Oren-Nayar raked light ->
// optional Blinn-Phong sheen -> tint), so a material is a recipe, not a new pipeline. All five
// render the 8-bit lane and carry paper's Scale semantics (features sized in document px at
// Scale 1 -- the registry's pixelScaledFeatures trait). Pure functions of (params, w, h): the
// §8.3 determinism contract, byte-exact under any TextureWindow crop.
namespace mosaic::core::texture {

[[nodiscard]] common::Image renderWood(const TextureParams& p, const WoodParams& wood,
                                       std::uint32_t w, std::uint32_t h,
                                       const TextureWindow& window = {},
                                       TextureRenderProgress* progress = nullptr);
[[nodiscard]] common::Image renderMarble(const TextureParams& p, const MarbleParams& marble,
                                         std::uint32_t w, std::uint32_t h,
                                         const TextureWindow& window = {},
                                         TextureRenderProgress* progress = nullptr);
[[nodiscard]] common::Image renderStone(const TextureParams& p, const StoneParams& stone,
                                        std::uint32_t w, std::uint32_t h,
                                        const TextureWindow& window = {},
                                        TextureRenderProgress* progress = nullptr);
[[nodiscard]] common::Image renderCanvas(const TextureParams& p, const CanvasParams& canvas,
                                         std::uint32_t w, std::uint32_t h,
                                         const TextureWindow& window = {},
                                         TextureRenderProgress* progress = nullptr);
[[nodiscard]] common::Image renderMetal(const TextureParams& p, const MetalParams& metal,
                                        std::uint32_t w, std::uint32_t h,
                                        const TextureWindow& window = {},
                                        TextureRenderProgress* progress = nullptr);

// The per-material preset libraries -- each entry a complete params value the dialog offers by
// name (sliders fine-tune from there; the paper/grass/sky preset pattern). Data only; ordering is
// stable (tests pin the counts + names).
struct WoodPreset {
    const char* name;
    WoodParams params;
};
[[nodiscard]] std::size_t woodPresetCount();
[[nodiscard]] const WoodPreset& woodPreset(std::size_t i);  // i < woodPresetCount()

struct MarblePreset {
    const char* name;
    MarbleParams params;
};
[[nodiscard]] std::size_t marblePresetCount();
[[nodiscard]] const MarblePreset& marblePreset(std::size_t i);

struct StonePreset {
    const char* name;
    StoneParams params;
};
[[nodiscard]] std::size_t stonePresetCount();
[[nodiscard]] const StonePreset& stonePreset(std::size_t i);

struct CanvasPreset {
    const char* name;
    CanvasParams params;
};
[[nodiscard]] std::size_t canvasPresetCount();
[[nodiscard]] const CanvasPreset& canvasPreset(std::size_t i);

struct MetalPreset {
    const char* name;
    MetalParams params;
};
[[nodiscard]] std::size_t metalPresetCount();
[[nodiscard]] const MetalPreset& metalPreset(std::size_t i);

}  // namespace mosaic::core::texture
