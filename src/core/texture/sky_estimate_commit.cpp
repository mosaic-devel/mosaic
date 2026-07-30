#include "core/texture/sky_estimate_commit.hpp"

#include <utility>

#include "core/layer.hpp"
#include "core/texture/texture_layer_render.hpp"

namespace mosaic::core::texture {

std::unique_ptr<CompositeCommand> buildSkyConformCommand(Document& doc, LayerId photoLayerId,
                                                         SkyConformPlan plan) {
    const Layer* photo = doc.find(photoLayerId);
    if (photo == nullptr) return nullptr;
    const auto loc = doc.locate(photoLayerId);
    if (!loc || loc->parent == nullptr) return nullptr;
    if (plan.skySelection.isEmpty() || !plan.skySelection.anySelected()) return nullptr;
    if (!std::holds_alternative<SkyParams>(plan.skyParams.spec)) return nullptr;

    auto cmd = std::make_unique<CompositeCommand>(std::move(plan.label));

    // 1. The sky plate, BELOW the photo (children order bottom(0)->top, so the photo's own
    //    index is "just below it"; the photo shifts up by one when this applies). The bake is
    //    pre-installed through the cache's one write path so the first composite after the
    //    commit finds a CURRENT cache instead of re-rendering synchronously.
    auto sky = doc.makeTexture("Sky", plan.skyParams);
    applyBakedTextureCache(*sky, std::move(plan.baked), doc.width(), doc.height());
    cmd->add(std::make_unique<AddLayerCommand>(loc->parent->id(), loc->index, std::move(sky)));

    // 2. Mask the photo to its FOREGROUND (NOT-sky): the feathered selection complement,
    //    resampled onto the photo's own mask grid through its transform.
    RasterMask fgMask =
        maskFromSelection(*photo, plan.skySelection.inverted(), doc.width(), doc.height());
    cmd->add(std::make_unique<SetLayerMaskCommand>(photoLayerId, std::move(fgMask), "Mask sky"));

    // 3. The harmonization grade, clipped to the photo so it recolors ONLY the foreground
    //    (clip-to-below reads the photo's post-mask alpha), stacked directly above it. After
    //    step 1 applies, the photo sits at index + 1, so the adjustment lands at index + 2.
    auto adj = doc.makeAdjustment("Match sky", AdjustmentKind::PhotometricMatch);
    adj->params() = std::move(plan.matchParams);
    adj->setClipToBelow(true);
    cmd->add(std::make_unique<AddLayerCommand>(loc->parent->id(), loc->index + 2, std::move(adj)));

    return cmd;
}

}  // namespace mosaic::core::texture
