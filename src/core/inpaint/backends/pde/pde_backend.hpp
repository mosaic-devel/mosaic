#pragma once

// Classical inpainting backend for Mosaic (PLAN S37-b).
//
// Algorithm: harmonic (Laplace-equation) diffusion — fill the hole with the smooth function whose
// Laplacian is zero inside the hole and whose boundary equals the surrounding known pixels, solved
// by Gauss-Seidel relaxation. This is the simplest, oldest member of the PDE-inpainting family
// (Laplace's equation), generalizing the foundational Bertalmío and Telea PDE methods. A sharper,
// faster Telea Fast-Marching kernel is a planned drop-in within this same backend.
//
// ⚠ INVARIANT — this backend implements the harmonic formulation ONLY. Any future kernel here is
// limited to the classical published PDE schemes named above (Bertalmío, Telea); a later,
// non-classical PDE inpainting scheme must NOT be substituted in. That is a deliberate hard
// constraint on this file, not an oversight.

#include "common/i18n.hpp" // N_(): mark for extraction (shown by the History panel)
#include "core/inpaint/inpaint_backend.hpp"

namespace mosaic::core::inpaint {

class PdeBackend final : public IInpaintBackend {
public:
    [[nodiscard]] std::string id() const override { return "pde"; }
    [[nodiscard]] std::string name() const override { return N_("Diffusion (PDE)"); }

    [[nodiscard]] InpaintResult run(const InpaintRequest& request,
                                    const ProgressFn& progress) override;

    [[nodiscard]] BackendInfo info() const override;
    [[nodiscard]] BackendSettingsSchema settingsSchema() const override;
    void applyParam(Params& params, const std::string& key, double value) const override;
};

}  // namespace mosaic::core::inpaint
