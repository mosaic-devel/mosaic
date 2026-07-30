#pragma once

#include <optional>

#include "core/brush/mask_stroke.hpp" // MaskStrokeParams -- the scope stroke's tip
#include "core/layer.hpp"             // core::Layer (the scope's landing grid)
#include "core/red_eye.hpp"           // core::RedEyeMode / RedEyeParams
#include "core/selection.hpp"
#include "ui/tool.hpp"

// Eye-tool gesture math (S38-b, docs/red-eye-tool.md §4) -- the pure, FLTK-free core that turns the
// active tool + its options-bar snapshot into (a) the tip the scope stroke paints and (b) the
// core::RedEyeParams the correction runs with, plus the scoping rule the gesture commits under.
// Kept free of FLTK and the canvas so it is unit-tested headlessly, exactly like crop_gesture /
// pen_gesture / shape_gesture; VulkanCanvas owns the pointer side and app_window the document side.
//
// BOTH modes are brush-scoped strokes. §4 offers "drag an ellipse (or click-brush)" for the flash
// mode and a brush for the sclera mode; one stroke lane serves both, with the flash mode's **Size**
// control being "the scoped region" §4 names -- a click with the size ring showing exactly what
// will be corrected. That keeps red_eye.hpp's invariant 1 identical in both modes (the USER
// supplies the region, nothing is ever detected) and means one gesture, one reticle, one undo
// step, whichever mode is live.
namespace mosaic::ui {

// A flat snapshot of the active eye tool's options, read off the options bar by the canvas so the
// math never touches the ToolManager (the ShapeOptions pattern).
struct RedEyeOptions {
    core::RedEyeMode mode = core::RedEyeMode::Flash;
    double size = 40.0;     // scope tip diameter, document px
    double spread = 40.0;   // 0..100 softness of the scope's edge (0 = hard, 100 = fully soft)
    // Flash
    double strength = 100.0; // 0..100
    double darken = 65.0;    // 0..100
    bool keepCatchlight = true;
    // Sclera
    double amount = 55.0;      // 0..100
    double vascularity = 35.0; // 0..100 -- the floor that always survives
    bool suppressVeins = true;
    bool protectCornerWarmth = true;
};

// The eye-tool mode a ToolId selects (nullopt for every other tool). The one place the two
// registered variants map onto the two shipping tiers.
[[nodiscard]] std::optional<core::RedEyeMode> redEyeModeFor(ToolId id);

// The tip the scope stroke paints. The flash mode paints a near-hard tip -- a pupil's glow has a
// crisp boundary and a soft scope would drag the correction onto the iris -- while the sclera mode
// honours Spread, because a soft scope edge is what makes a de-redded patch blend into the rest of
// the white. Full flow/opacity in both: the stroke is a SCOPE, not paint, so crossing it twice
// must not deepen the correction (that job belongs to Strength / Amount).
[[nodiscard]] core::brush::MaskStrokeParams redEyeStrokeParams(const RedEyeOptions& o);

// The options-bar snapshot as core parameters. Percentages become [0,1]; the vein-suppression
// radius is derived from the tip size rather than exposed as its own control -- a bigger brush is a
// bigger eye is a thicker vessel, and §4's control list deliberately has no radius in it.
[[nodiscard]] core::RedEyeParams redEyeParams(const RedEyeOptions& o);

// The effective scope of a finished gesture (§2.4): the painted stroke, intersected with the
// document's own selection when there is one. An empty document selection means "everything is
// editable", so the stroke stands alone; an active selection clips the correction to it exactly as
// it clips a brush stroke. Returns an empty Selection when nothing survives -- the caller then
// lands no command at all.
[[nodiscard]] core::Selection redEyeScope(const core::Selection& stroke,
                                          const core::Selection& documentSelection);

// A document-space scope carried onto a RASTER layer's own pixel grid, which is the grid the
// correction and its region-scoped SetLayerPixelsCommand both work in. Per layer pixel, read the
// coverage at the document point it lands on through the layer's world transform (nearest) -- the
// same map core::selectionFromLayerPixels uses in the other direction and the compositor's leaf
// walk samples with, so a rotated or scaled layer is retouched exactly where the user painted.
// Returns an empty Selection for a non-raster layer, an empty layer, an empty scope, or a singular
// transform -- in every case the caller lands no command.
[[nodiscard]] core::Selection redEyeScopeOnLayer(const core::Layer& layer,
                                                 const core::Selection& docScope);

} // namespace mosaic::ui
