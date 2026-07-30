#pragma once

// Resynthesizer-style texture synthesis backend for Mosaic — an OPTIONAL engine choice beside
// the He & Sun default (user request 2026-07-02: "more choices"; the default stays offset-stats).
//
// Algorithm & credits: Paul Harrison's best-fit per-pixel texture re-synthesis —
//   - P. Harrison, "A Non-hierarchical Procedure for Re-synthesis of Complex Textures",
//     WSCG 2001 (constraint-ordered per-pixel best-fit growth, robust weighted match metric);
//   - P. Harrison, "Image Texture Tools", PhD thesis, Monash University, 2005 (the practical
//     refinements: neighbour-coherence candidate search, Cauchy-based difference metric,
//     multi-pass refinement);
//   - lineage of D. Garber (1981) and A. Efros & T. Leung (1999), whom Harrison refines;
//   - the algorithm is best known as the GIMP "Resynthesizer" plugin (Harrison, 2000s),
//     maintained today by Lloyd Konneker (github.com/bootchk/resynthesizer).
// This implementation is CLEAN-ROOM C++ from the publications above (no plugin code ported —
// the plugin is GPL and GPLv3-compatible, but Mosaic implements from the papers; the GIMP
// adapters are irrelevant here anyway). Full crediting also appears in the Settings → Inpainting
// engine sheet (BackendInfo below).
//
// Lineage: the method is published 2001/2005 academic work, and it sits in the pre-2006
// texture-synthesis generation (Efros & Leung 1999, Wei & Levoy 2000, Ashikhmin 2001). Harrison's
// coherence search (2001) is the lineage for this backend's candidate scheme.
//
// ⚠ GUARDRAIL for future edits: this backend's random candidates are UNIFORM draws over the donor
// region, never perturbations of an existing mapping. Do NOT add perturb-around-current-best
// sampling to the candidate search — that is a hard constraint on this file, not an oversight.
//
// Character: strongest on organic, irregular texture (grass, foliage, gravel, sand, clouds)
// where the offset-statistics default wants repeating structure. Deterministic: all sampling
// uses a fixed-seed PRNG, so identical inputs give identical output (Mosaic's golden rule).

#include "common/i18n.hpp" // N_(): mark for extraction (shown by the History panel)
#include "core/inpaint/inpaint_backend.hpp"

namespace mosaic::core::inpaint {

class ResynthBackend final : public IInpaintBackend {
public:
    [[nodiscard]] std::string id() const override { return "resynth"; }
    [[nodiscard]] std::string name() const override { return N_("Resynthesizer"); }

    [[nodiscard]] InpaintResult run(const InpaintRequest& request,
                                    const ProgressFn& progress) override;

    [[nodiscard]] BackendInfo info() const override;
    [[nodiscard]] BackendSettingsSchema settingsSchema() const override;
    void applyParam(Params& params, const std::string& key, double value) const override;

    [[nodiscard]] std::optional<common::Rect>
    analysedRegion(std::uint32_t imageW, std::uint32_t imageH,
                   const std::optional<common::Rect>& holeBounds,
                   const Params& params) const override;
};

} // namespace mosaic::core::inpaint
