#pragma once

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/layer.hpp"

#include <vector>

// The S35 artistic / stylize filter family (docs/filters-stylize.md): sharpen, unsharp mask,
// add noise, denoise, pixelate, emboss, oil paint (Kuwahara 1976), wave/ripple and vignette,
// each shipped as a non-destructive adjustment-layer kind on the S32 schema + S33 spatial
// framework.
//
// The whole family lives behind the THREE entry points below, so the compositor gains one
// branch rather than nine: `applyAdjustment` asks isStylizeKind() before its own scalar/spatial
// split, `blurAdjustmentReach` forwards to stylizeAdjustmentReach() for the region/group-buffer
// machinery, and everything else -- schema, docio, editor, dock -- treats the new kinds as
// ordinary adjustment kinds. Keeping the kernels out of compositor.cpp is deliberate: that file
// is already 3k lines and the stylize kernels share nothing with the blur gathers.
namespace mosaic::render {

// True for the S35 stylize kinds. The compositor's applyAdjustment() branches on this before
// its own scalar/spatial split.
[[nodiscard]] bool isStylizeKind(core::AdjustmentKind kind);

// Applies `adj` to the working buffer in place. Owns its own opacity * mask * clip-coverage
// blend, exactly like applyBlurAdjustment() does (premultiplied lerp, amt==1 fast path kept
// byte-equal to the raw kernel output, alpha handling stated in a comment). `pre` maps the
// layer's PARENT space onto the buffer; `maskDomain` is the parent-space rect the layer mask
// spans; `liveDrag` may draft-subsample heavy gathers mid-gesture.
void applyStylizeAdjustment(common::ImageF& acc, const core::AdjustmentLayer& adj,
                            const std::vector<float>* coverage, const common::Affine2D& pre,
                            const common::Rect& maskDomain, bool liveDrag);

// The kernel support radius (application-space px) this kind reads or spreads, for the
// region-recomposite machinery (docs/blur-filters.md §5). 0 for per-pixel kinds.
[[nodiscard]] double stylizeAdjustmentReach(const core::AdjustmentLayer& adj,
                                            const common::Rect& domain);

}  // namespace mosaic::render
