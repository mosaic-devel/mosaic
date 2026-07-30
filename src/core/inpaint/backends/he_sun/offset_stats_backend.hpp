#pragma once

// Default high-quality inpaint backend: the He & Sun offset-statistics graph-completion solver
// (PLAN S37-c). Gathers dominant patch offsets, then chooses one per hole pixel by
// α-expansion (validity + seam coherence) and copies the result. Clean-room from the paper: an
// exact KD-tree NNF (no propagation, no random search) over our own graph cut.
//
// Runs at full resolution for now; the working-region downsample + KD-tree NNF are the documented
// performance steps still to wire in before this replaces PdeBackend as the engine default.

#include "common/i18n.hpp" // N_(): mark for extraction (shown by the History panel)
#include "core/inpaint/inpaint_backend.hpp"

namespace mosaic::core::inpaint {

class OffsetStatisticsBackend final : public IInpaintBackend {
public:
    [[nodiscard]] std::string id() const override { return "offset-stats"; }
    [[nodiscard]] std::string name() const override { return N_("Offset statistics (He & Sun)"); }

    [[nodiscard]] InpaintResult run(const InpaintRequest& request,
                                    const ProgressFn& progress) override;

    [[nodiscard]] BackendInfo info() const override;
    [[nodiscard]] BackendSettingsSchema settingsSchema() const override;
    void applyParam(Params& params, const std::string& key, double value) const override;

    // The cropped working region this backend analyses -- drives the UI sample-area preview (S39).
    [[nodiscard]] std::optional<common::Rect>
    analysedRegion(std::uint32_t imageW, std::uint32_t imageH,
                   const std::optional<common::Rect>& holeBounds,
                   const Params& params) const override;
};

}  // namespace mosaic::core::inpaint
