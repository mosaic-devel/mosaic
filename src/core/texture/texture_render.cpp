// Texture Generator CPU reference renderers -- procedural, ML-free, built entirely on the
// public-domain noise kit (see noise.hpp's lineage header). Element composition follows
// docs/texture-generator.md §3.4/§4.4: each generator composites its enabled elements
// bottom-to-top into a straight-alpha buffer, so a disabled base element (sky dome off) yields a
// genuinely TRANSPARENT layer -- that is the "create a transparent layer" affordance.
// Sky: the real S55-b renderer (Hosek-Wilkie + camera + cloud catalogue) in sky_render.cpp.
// Paper: the real S55-d renderer (fibre/grain/tooth kinds -> single-pass Sobel normal -> Oren-Nayar
// raked light + deckle + print tooth) in paper_render.cpp. Grass: the real S55-e distance-graded
// hybrid (ground-plane homography -> turf base -> depth-culled Bezier blades -> Kajiya-Kay/wrap/AO
// -> back-to-front composite) in grass_render.cpp. This TU is the GENERATOR REGISTRY (S55-g,
// GeneratorTraits) + dispatch; every renderer lives in its own translation unit and every
// cross-generator consumer walks the table below instead of switching on Generator.

#include "core/texture/texture_render.hpp"

#include <type_traits>

#include "core/texture/grass_render.hpp"
#include "core/texture/material_render.hpp"
#include "core/texture/paper_render.hpp"
#include "core/texture/render_support.hpp"
#include "core/texture/sky_render.hpp"

namespace mosaic::core::texture {

namespace {

// The injected GPU lane (S55-h; the extrude_render.cpp g_override precedent). Empty until the
// app registers render::TextureGpu -- never set by the test binary, so goldens stay CPU-pinned.
TextureRenderOverride g_renderOverride;

// ---- registry adapters ---------------------------------------------------------------------
// Every generator follows one shape: its params struct is a variant arm, its renderer takes
// (params, spec, w, h, window, progress) and returns the lane its cache uses, and its preset
// library is a table of {name, complete params}. These templates type-erase that shape into a
// GeneratorTraits row, so a row is one line per generator rather than five hand-written thunks.

template <class Spec>
void seedSpecOf(TextureParams& p) {
    p.spec = Spec{};
}

template <class Spec, auto RenderFn>
TextureRenderResult renderArm(const TextureParams& p, std::uint32_t w, std::uint32_t h,
                              const TextureWindow& window, TextureRenderProgress* progress) {
    // A mismatched variant arm (never constructed in practice) renders the spec's defaults --
    // the same tolerance the pre-registry switch carried.
    const auto* spec = std::get_if<Spec>(&p.spec);
    TextureRenderResult r;
    if constexpr (std::is_same_v<decltype(RenderFn(p, *spec, w, h, window, progress)),
                                 common::ImageF>)
        r.imageF = RenderFn(p, spec != nullptr ? *spec : Spec{}, w, h, window, progress);
    else
        r.image8 = RenderFn(p, spec != nullptr ? *spec : Spec{}, w, h, window, progress);
    return r;
}

template <auto PresetFn>  // const XxxPreset& (*)(std::size_t)
const char* presetNameOf(std::size_t i) {
    return PresetFn(i).name;
}

template <auto PresetFn>
void applyPresetOf(TextureParams& p, std::size_t i) {
    p.spec = PresetFn(i).params;
}

template <class Spec, auto PresetFn, auto CountFn>
int matchPresetOf(const TextureParams& p) {
    const auto* s = std::get_if<Spec>(&p.spec);
    if (s == nullptr) return 0;
    for (std::size_t i = 0; i < CountFn(); ++i)
        if (PresetFn(i).params == *s) return static_cast<int>(i) + 1;
    return 0;
}

// ---- the registry ----------------------------------------------------------------------------
// One row per generator, in enum order (static_assert-pinned). Tokens are docio/CLI surface and
// FROZEN once shipped.
constexpr GeneratorTraits kGenerators[kGeneratorCount] = {
    {Generator::Sky, "Sky", "sky", /*pixelScaledFeatures=*/false, &seedSpecOf<SkyParams>,
     &renderArm<SkyParams, renderSkyTexture>, &skyPresetCount, &presetNameOf<skyPreset>,
     &applyPresetOf<skyPreset>, &matchPresetOf<SkyParams, skyPreset, skyPresetCount>},
    {Generator::Paper, "Paper", "paper", /*pixelScaledFeatures=*/true, &seedSpecOf<PaperParams>,
     &renderArm<PaperParams, renderPaper>, &paperPresetCount, &presetNameOf<paperPreset>,
     &applyPresetOf<paperPreset>, &matchPresetOf<PaperParams, paperPreset, paperPresetCount>},
    {Generator::Grass, "Grass", "grass", /*pixelScaledFeatures=*/false, &seedSpecOf<GrassParams>,
     &renderArm<GrassParams, renderGrass>, &grassPresetCount, &presetNameOf<grassPreset>,
     &applyPresetOf<grassPreset>, &matchPresetOf<GrassParams, grassPreset, grassPresetCount>},
    // -- the S55-g materials: §5-engine recipes in material_render.cpp, paper Scale semantics --
    {Generator::Wood, "Wood", "wood", /*pixelScaledFeatures=*/true, &seedSpecOf<WoodParams>,
     &renderArm<WoodParams, renderWood>, &woodPresetCount, &presetNameOf<woodPreset>,
     &applyPresetOf<woodPreset>, &matchPresetOf<WoodParams, woodPreset, woodPresetCount>},
    {Generator::Marble, "Marble", "marble", /*pixelScaledFeatures=*/true,
     &seedSpecOf<MarbleParams>, &renderArm<MarbleParams, renderMarble>, &marblePresetCount,
     &presetNameOf<marblePreset>, &applyPresetOf<marblePreset>,
     &matchPresetOf<MarbleParams, marblePreset, marblePresetCount>},
    {Generator::Stone, "Stone", "stone", /*pixelScaledFeatures=*/true, &seedSpecOf<StoneParams>,
     &renderArm<StoneParams, renderStone>, &stonePresetCount, &presetNameOf<stonePreset>,
     &applyPresetOf<stonePreset>, &matchPresetOf<StoneParams, stonePreset, stonePresetCount>},
    {Generator::Canvas, "Canvas", "canvas", /*pixelScaledFeatures=*/true,
     &seedSpecOf<CanvasParams>, &renderArm<CanvasParams, renderCanvas>, &canvasPresetCount,
     &presetNameOf<canvasPreset>, &applyPresetOf<canvasPreset>,
     &matchPresetOf<CanvasParams, canvasPreset, canvasPresetCount>},
    {Generator::Metal, "Metal", "metal", /*pixelScaledFeatures=*/true, &seedSpecOf<MetalParams>,
     &renderArm<MetalParams, renderMetal>, &metalPresetCount, &presetNameOf<metalPreset>,
     &applyPresetOf<metalPreset>, &matchPresetOf<MetalParams, metalPreset, metalPresetCount>},
};

static_assert(
    [] {
        for (std::size_t i = 0; i < static_cast<std::size_t>(kGeneratorCount); ++i)
            if (kGenerators[i].id != static_cast<Generator>(i)) return false;
        return true;
    }(),
    "registry rows must follow the Generator enum order");

}  // namespace

void setTextureRenderOverride(TextureRenderOverride fn) {
    g_renderOverride = std::move(fn);
}

const GeneratorTraits& generatorTraits(Generator g) {
    const auto i = static_cast<std::size_t>(g);
    return kGenerators[i < static_cast<std::size_t>(kGeneratorCount) ? i : 0];
}

const char* generatorName(Generator g) {
    const auto i = static_cast<std::size_t>(g);
    return i < static_cast<std::size_t>(kGeneratorCount) ? kGenerators[i].name : "Texture";
}

const char* paperKindName(PaperKind k) {
    switch (k) {
        case PaperKind::Wove: return "Wove";
        case PaperKind::Laid: return "Laid";
        case PaperKind::Felt: return "Felt";
    }
    return "Paper";
}

const char* cloudTypeName(CloudType t) {
    switch (t) {
        case CloudType::Cirrus: return "Cirrus";
        case CloudType::Cirrocumulus: return "Cirrocumulus";
        case CloudType::Cirrostratus: return "Cirrostratus";
        case CloudType::Altocumulus: return "Altocumulus";
        case CloudType::Altostratus: return "Altostratus";
        case CloudType::Stratocumulus: return "Stratocumulus";
        case CloudType::Stratus: return "Stratus";
        case CloudType::Nimbostratus: return "Nimbostratus";
        case CloudType::Cumulus: return "Cumulus";
        case CloudType::Cumulonimbus: return "Cumulonimbus";
    }
    return "Cloud";
}

TextureParams defaultTextureParams(Generator g) {
    TextureParams p;
    p.generator = g;
    generatorTraits(g).seedSpec(p);
    return p;
}

TextureRenderResult renderTexture(const TextureParams& params, std::uint32_t w, std::uint32_t h,
                                  const TextureWindow& window, TextureRenderProgress* progress) {
    if (w == 0 || h == 0) return TextureRenderResult{};
    const auto i = static_cast<std::size_t>(params.generator);
    if (i >= static_cast<std::size_t>(kGeneratorCount)) return TextureRenderResult{};
    // The GPU lane serves first when installed and able (S55-h); any refusal -- unsupported
    // generator/feature, no device, a device error -- falls through to the registry's CPU
    // renderer below with a fresh result (a refusing override must not leak partial pixels).
    if (g_renderOverride) {
        TextureRenderResult viaGpu;
        if (g_renderOverride(params, w, h, window, progress, viaGpu))
            return renderCancelled(progress) ? TextureRenderResult{} : std::move(viaGpu);
    }
    TextureRenderResult r = kGenerators[i].render(params, w, h, window, progress);
    // A cancelled render never hands back partial pixels -- the contract is all-or-nothing so a
    // caller can treat "has an arm" as "complete".
    if (renderCancelled(progress)) return TextureRenderResult{};
    return r;
}

}  // namespace mosaic::core::texture
