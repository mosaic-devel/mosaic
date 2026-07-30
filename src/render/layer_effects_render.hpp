#pragma once

#include "common/geometry.hpp"  // common::Affine2D
#include "common/image.hpp"

#include <optional>

namespace mosaic::core {
struct LayerEffects;
}

namespace mosaic::render {

// Apply a layer's non-destructive effects to its isolated straight-alpha RGBA buffer IN PLACE,
// in the render seam between renderLayer() and walkStep() (docs/layer-effects.md §4). The buffer
// is in the compositor's target space; effect sizes are interpreted as buffer px (== layer px
// for an untransformed layer; a transform-aware "Scale Effects" is LE-g). No-op when fx.empty(),
// so untouched layers stay byte-identical and pay nothing.
//
// This is the permanent CPU reference lane (doc §5, §8); a Vulkan-compute sibling is added at S60
// behind a parity-tested render override, exactly like the extrude renderer. LE-a renders
// fill-opacity + concentric Stroke; the shadow/glow/overlay/bevel/satin tiers land in LE-c..f.
//
// `antialias` is the document-wide AA setting (the Move tool's AA combobox: false only for the
// Nearest kernel). It only affects PATTERN edges (solid/gradient paints ignore it); the compositor
// passes `filter != Nearest`, matching how vector-shape edges read the same combobox.
//
// `bufferToLayer` maps a buffer pixel back to the layer's LOCAL space (the inverse of the layer's
// placement). A LAYER-anchored procedural pattern (anchorToCanvas == false) is evaluated there, so it
// is glued to the layer -- it rotates / translates / scales WITH it, with no position shift. A
// CANVAS-anchored pattern ignores it (stays put in buffer space). nullopt -> the pattern falls back to
// the content-box origin (the headless tests' path); solids/gradients ignore it entirely.
void applyEffects(common::ImageF& io, const core::LayerEffects& fx, bool antialias = true,
                  const std::optional<common::Affine2D>& bufferToLayer = std::nullopt);

}  // namespace mosaic::render
