#pragma once

#include <cstdint>
#include <memory>
#include <variant>

#include "common/geometry.hpp"
#include "common/image.hpp"

// Pattern paint (LE-d; docs/layer-effects.md §7). The third real Paint alternative after Solid and
// Gradient. Two honest kinds:
//   - ProceduralPattern: resolution-independent, colour-editable, seamless by construction. A pure
//     function of the sample point, so it shares the vec::sampleAt evaluator with solids/gradients
//     (no external tile store) -- this is the LE-d flagship, fully implemented here.
//   - ImagePattern: a fixed-resolution bitmap tile (GIMP tiles / import / make-from-selection). The
//     tile is held self-contained (a shared, immutable common::Image) so samplePattern stays pure --
//     no external store, no I/O. LE-d2 implements the sampler (bilinear/nearest, seamless wrap in
//     layer-px space); a null/empty tile still reads transparent. See docs/le-d2-image-patterns.md.
// Procedural kinds evaluate in LAYER-PIXEL space (not normalised): `scale` is the feature size in
// px, so the pattern tiles the same regardless of the shape's size (unlike a gradient, which spans
// its box). The effect renderer therefore passes real box-relative px for a pattern.
namespace mosaic::core::vec {

using common::ColorF;
using common::Vec2;

struct ProceduralPattern {
    // The procedural library (docs/layer-effects.md §7.1), in flyout-grid order (5 x 5). Distinct
    // weaves: Herringbone (true diagonal bricks), Parquet (axis-aligned single-brick interlock),
    // Basketweave (blocked bars). Chainmail = interlocking rings, Honeycomb = flat-top hex cells,
    // Harlequin = 2-tone diamonds, Triangles = equilateral tessellation, Sawtooth = per-row right
    // triangles, Star Anise = 5-ray burst, Stars = upright 5-point.
    enum class Kind {
        Dots, Grid, Lines, Hatch, CrossHatch,
        Checker, Herringbone, Parquet, Basketweave, Chevron,
        Zigzag, Chainmail, Halftone, Grain, Bricks,
        Triangles, Sawtooth, Harlequin, Honeycomb, Waves,
        Stars, StarAnise, Hearts, Crosses, Rings,
    };
    static constexpr int kKindCount = 25;

    Kind kind = Kind::Dots;
    ColorF fg{0.0f, 0.0f, 0.0f, 1.0f};
    ColorF bg{0.0f, 0.0f, 0.0f, 0.0f};  // transparent by default: the pattern reads over content
    float scale = 32.0f;                // feature size in layer px (the tiling period)
    float angleDeg = 0.0f;
    float offset = 0.0f;   // phase shift along BOTH tile axes, 0..1 of one tile (1 == a full tile in
                           // x and y, i.e. back to the start). Nudges the tiling under a fixed shape.
    float weight = 0.5f;   // per-kind primary knob (dot radius / line duty / mortar thickness), 0..1
    float spacing = 0.25f; // distance BETWEEN elements for motif-on-lattice kinds whose feature has
                           // a fixed shape (Hearts / Stars / Star Anise): 0 packs them tight, 1
                           // opens the gaps. Inert (and hidden by the UI) for kinds where `weight`
                           // is the size knob or which tessellate gaplessly (patternUsesSpacing()).
    // Anchor the pattern's phase to the CANVAS (document space) rather than the layer's content: the
    // static-texture "reveal" look where moving/rotating the layer slides its shape over a pattern
    // that stays put (Photoshop's Pattern Overlay "Link with Layer" OFF). The effect renderer reads
    // this to pick a fixed document origin over the content-box origin (layer_effects_render.cpp).
    // false = anchored to the layer content (tiles move with the shape).
    bool anchorToCanvas = false;
    bool operator==(const ProceduralPattern&) const = default;
};

struct ImagePattern {
    // The tile is a shared, immutable bitmap held self-contained so samplePattern stays pure (no
    // registry / no I/O). Built by make-from-selection or import (see makeImagePattern below), or a
    // curated GIMP tile in a later increment. A null or empty tile reads transparent.
    std::shared_ptr<const common::Image> tile;
    float scale = 1.0f;      // multiplier of native tile px: layer px `p` samples native px `p/scale`
    float angleDeg = 0.0f;   // rotation of the tiling (about the layer/anchor origin), like procedural
    Vec2 offset{0.0, 0.0};   // phase shift, in FRACTIONS OF ONE TILE per axis (0..1 == a full tile ==
                             // back to start); nudges which part of the tile sits at the origin.
    bool operator==(const ImagePattern&) const = default;  // shared_ptr identity + fields
};

using Pattern = std::variant<ProceduralPattern, ImagePattern>;

// Human-readable kind name (UI + tests). Stable ASCII; never localised through here.
[[nodiscard]] const char* patternKindName(ProceduralPattern::Kind kind);

// Which per-kind control the flyout should surface. `weight` is the primary knob for kinds that have
// a thickness/radius/duty; a handful of motif-on-lattice kinds ignore it and instead take `spacing`
// (the gap between elements); gapless tessellations (Checker / Triangles / Sawtooth / Harlequin)
// take neither. Exactly one of these is true for the motif kinds; both false for the tessellations.
[[nodiscard]] bool patternUsesWeight(ProceduralPattern::Kind kind);
[[nodiscard]] bool patternUsesSpacing(ProceduralPattern::Kind kind);

// Evaluate a pattern at a point in LAYER-PIXEL space. Procedural kinds tile with period `scale`
// (rotated by `angleDeg`), the fg feature composited over bg. `antialias` (the document-wide setting,
// the Move tool's AA combobox) picks the edge: true = a 1px linear-ramp AA edge, false = a hard 0/1
// threshold (crisp/aliased). Grain (value noise) never anti-aliases regardless -- it always reads
// crisp. An ImagePattern samples its tile with seamless wrap in layer-px space (`scale` multiplies the
// native tile px, `angleDeg` rotates the tiling, `offset` shifts the phase); `antialias` picks bilinear
// (soft, AA on) vs nearest-neighbour (crisp/pixelated, AA off) filtering, and a null/empty tile reads
// transparent. Pure -- shared by the vector rasteriser (raster.cpp) and the layer-effects renderer.
[[nodiscard]] ColorF samplePattern(const Pattern& pattern, Vec2 layerPx, bool antialias = true);

// Build an ImagePattern tile from a source image (make-from-selection / import; LE-d2). Pure, no I/O.
//  - The whole-image overload takes `tile` by value and holds it self-contained (import an already-
//    loaded bitmap, or a from-selection crop the caller has already extracted).
//  - The region overload crops [x,y,w,h] out of `src` (copyRegion semantics: out-of-bounds reads
//    transparent) -- the "make pattern from the current selection's bounding box" path.
// An empty result (zero-area region / empty src) yields a null-tile ImagePattern (reads transparent).
// scale/angleDeg/offset are left at their defaults for the caller / UI to set.
[[nodiscard]] ImagePattern makeImagePattern(common::Image tile);
[[nodiscard]] ImagePattern makeImagePattern(const common::Image& src, long x, long y,
                                            std::uint32_t w, std::uint32_t h);

}  // namespace mosaic::core::vec
