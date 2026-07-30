// Smart Recompose — the inpaint-engine FillFn adapter; see inpaint_fill.hpp.

#include "core/retarget/inpaint_fill.hpp"

#include "core/selection.hpp"

#include <utility>

namespace mosaic::core::retarget {

FillFn makeInpaintFill(const inpaint::InpaintEngine& engine, inpaint::Params params,
                       const std::atomic<bool>* cancel, FillProgressFn progress) {
    return [&engine, params, cancel, progress = std::move(progress)](
               common::Image& img, const std::vector<common::Rect>& holes) -> bool {
        if (holes.empty())
            return true; // nothing to heal
        if (img.empty())
            return false;
        // One union mask = one engine run: the backends already handle disjoint hole regions
        // (a multi-blob inpaint stroke is the same shape of request).
        Selection mask;
        for (const common::Rect& h : holes)
            mask = Selection::combine(mask, Selection::rectangle(img.width, img.height, h),
                                      SelectOp::Add);
        if (!mask.anySelected())
            return true; // holes clamped away entirely: nothing to do
        const common::ImageF input = common::toFloat(img);
        const inpaint::InpaintRequest req{input, mask, params, cancel};
        inpaint::ProgressFn fn;
        if (progress || cancel != nullptr) {
            fn = [&progress, cancel](const inpaint::InpaintProgress& p) -> bool {
                if (progress)
                    progress(p.fraction, p.stage);
                return cancel == nullptr || !cancel->load();
            };
        }
        const inpaint::InpaintResult r = engine.run(req, fn);
        // A cancelled run counts as a failed fill regardless of what the engine salvaged — the
        // caller (prepareRecompose) must never build on a half-healed background.
        if (!r.ok || (cancel != nullptr && cancel->load()))
            return false;
        img = common::toImage8(r.image);
        return true;
    };
}

} // namespace mosaic::core::retarget
