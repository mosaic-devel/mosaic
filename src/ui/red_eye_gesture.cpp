#include "ui/red_eye_gesture.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace mosaic::ui {

namespace {

// A 0..100 options-bar percentage as a [0,1] fraction.
[[nodiscard]] double pct(double v) { return std::clamp(v, 0.0, 100.0) / 100.0; }

} // namespace

std::optional<core::RedEyeMode> redEyeModeFor(ToolId id) {
    switch (id) {
    case ToolId::RedEye:
        return core::RedEyeMode::Flash;
    case ToolId::RedEyeSclera:
        return core::RedEyeMode::Sclera;
    default:
        return std::nullopt;
    }
}

core::brush::MaskStrokeParams redEyeStrokeParams(const RedEyeOptions& o) {
    core::brush::MaskStrokeParams p;
    p.diameter = std::max(0.5, o.size);
    // Flash: HARD. The ring the user drags is a promise about what gets corrected, and a soft tip
    // quietly breaks it -- at 0.92 the coverage of a 70 px tip is already past half by r = 33.3 and
    // gone by r = 34.5, so the outer ~2 px of the ring corrects weakly or not at all. Line the ring
    // up with the glow, as the tool asks you to, and those 2 px are exactly the glow's edge: a red
    // rim, drawn by the difference between what the ring showed and what the scope covered.
    // MaskStroke still anti-aliases at hardness 1 (mask_stroke.hpp), so the boundary is not jagged.
    // Sclera: Spread owns the softness, and the default lands near 0.6.
    p.hardness = o.mode == core::RedEyeMode::Flash
                     ? 1.0
                     : std::clamp(1.0 - pct(o.spread), 0.05, 1.0);
    p.flow = 1.0;    // a scope, not paint: overlapping passes must not deepen the correction
    p.opacity = 1.0; // ... and the stroke must reach full coverage in one pass
    return p;
}

core::RedEyeParams redEyeParams(const RedEyeOptions& o) {
    core::RedEyeParams p;
    p.mode = o.mode;
    p.strength = pct(o.strength);
    p.darken = pct(o.darken);
    p.keepCatchlight = o.keepCatchlight;
    p.amount = pct(o.amount);
    p.vascularityFloor = pct(o.vascularity);
    p.suppressVeins = o.suppressVeins;
    p.protectCornerWarmth = o.protectCornerWarmth;
    // Derived, not exposed: ~12% of the tip diameter, so the vein scale follows the eye the user
    // is working at. Clamped so a 0.5 px tip still has a usable kernel and a 1000 px tip does not
    // turn the local-white estimate into a whole-region average.
    p.veinRadius = std::clamp(std::max(0.5, o.size) * 0.12, 1.0, 40.0);
    // Likewise for the flash mode's hysteresis reach: a glow's rim sits one or two pixels outside
    // its core, so this only has to be a small fraction of the tip -- and the correction is
    // insensitive to it (measured flat from 3 px to 30 px on the same pupil), because the ramps,
    // not the reach, decide what the core is allowed to vouch for.
    p.rimReach = std::clamp(std::max(0.5, o.size) * 0.10, 2.0, 24.0);
    return p;
}

core::Selection redEyeScope(const core::Selection& stroke,
                            const core::Selection& documentSelection) {
    if (stroke.isEmpty())
        return {};
    if (documentSelection.isEmpty())
        return stroke; // "no selection" means the whole document is editable (S13 semantics)
    if (documentSelection.width() != stroke.width() ||
        documentSelection.height() != stroke.height())
        return stroke; // mismatched grids: the stroke is the honest answer, never a wrong clip
    core::Selection clipped =
        core::Selection::combine(stroke, documentSelection, core::SelectOp::Intersect);
    if (!clipped.anySelected())
        return {}; // the user brushed entirely outside their own selection: nothing to do
    return clipped;
}

core::Selection redEyeScopeOnLayer(const core::Layer& layer, const core::Selection& docScope) {
    const auto* raster = layer.as<core::RasterLayer>();
    if (raster == nullptr || docScope.isEmpty())
        return {};
    const common::Image& img = raster->image();
    if (img.empty())
        return {};

    const common::Affine2D t = core::worldTransform(layer); // layer px -> document px
    core::Selection out(img.width, img.height);
    if (t == common::Affine2D::identity() && img.width == docScope.width() &&
        img.height == docScope.height()) {
        out.data() = docScope.data(); // 1:1 fast path: an untransformed, document-sized layer
        if (!out.anySelected())
            return {};
        return out;
    }
    for (std::uint32_t y = 0; y < img.height; ++y) {
        auto* row = out.data().data() + static_cast<std::size_t>(y) * img.width;
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const common::Vec2 p = t.apply({x + 0.5, y + 0.5});
            const auto dx = static_cast<long>(std::floor(p.x));
            const auto dy = static_cast<long>(std::floor(p.y));
            if (dx >= 0 && dy >= 0 && dx < static_cast<long>(docScope.width()) &&
                dy < static_cast<long>(docScope.height()))
                row[x] =
                    docScope.at(static_cast<std::uint32_t>(dx), static_cast<std::uint32_t>(dy));
        }
    }
    if (!out.anySelected())
        return {};
    return out;
}

} // namespace mosaic::ui
