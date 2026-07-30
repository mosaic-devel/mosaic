#pragma once

#include <cstddef>
#include <cstdint>

#include "common/image.hpp"
#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"  // TextureWindow / TextureRenderProgress

// The S55-d paper / material renderer (docs/texture-generator.md §5). A layered scalar height
// field (tooth + spectral fibre + the laid/chain/wove/felt structure) is turned into a normal map
// by a SINGLE-PASS Sobel and shaded by an Oren-Nayar raked light, then tinted; an optional deckle
// edge writes the transparent fringe (§3.4 alpha carry) and an optional blue-noise print tooth
// speckles the valleys. Pure function of (params, w, h) -- the same determinism contract as the
// rest of the kit (§8.3). This same pipeline is the engine for the S55-g follow-on materials
// (wood/marble/stone/canvas/metal): they differ only in the height field + tint, so a new material
// is a data addition, not new code.
namespace mosaic::core::texture {

// Render the paper generator into the 8-bit lane. `p` carries seed + Scale (feature size); `paper`
// the material knobs. Straight alpha: 255 everywhere unless the deckle edge is enabled.
// `w`/`h` are the FULL frame (the deckle band measures against them); `window` evaluates a
// byte-exact sub-rect and `progress` carries the per-row progress/cancel channel (both S55-f).
[[nodiscard]] common::Image renderPaper(const TextureParams& p, const PaperParams& paper,
                                        std::uint32_t w, std::uint32_t h,
                                        const TextureWindow& window = {},
                                        TextureRenderProgress* progress = nullptr);

// The §5.5 preset library -- each entry is a complete PaperParams value the S55-f dialog offers by
// name (sliders fine-tune from there, the type3d_panel preset pattern). Data only; ordering is
// stable (docio/UI index nothing off it, but tests pin the count + names).
struct PaperPreset {
    const char* name;
    PaperParams params;
};
[[nodiscard]] std::size_t paperPresetCount();
[[nodiscard]] const PaperPreset& paperPreset(std::size_t i);  // i < paperPresetCount()

}  // namespace mosaic::core::texture
