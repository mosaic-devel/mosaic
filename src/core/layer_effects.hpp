#pragma once

#include <algorithm>
#include <vector>

#include "common/image.hpp"       // common::ColorF
#include "core/blend_mode.hpp"    // core::BlendMode
#include "core/vector/paint.hpp"  // core::vec::Paint (solid / gradient / pattern-later)

// Layer effects -- non-destructive per-layer styles (stroke, colour/gradient/pattern overlay,
// drop/inner shadow, outer/inner glow, bevel & emboss, satin), the model behind the
// `Layer ▸ Layer Effects…` modal. Design + technique lineage: docs/layer-effects.md.
//
// THIS HEADER IS THE DATA MODEL ONLY (LE-a, doc §3). It is FLTK-free and render-free, so every
// module -- core (this struct on the Layer base), render (applyEffects), ui (the modal), io
// (.mosaic serialization, S48/LE-g) -- shares one definition. The CPU render seam that turns
// these parameters into pixels is render/layer_effects_render.{hpp,cpp}; the modal editor is
// LE-b. Effects attach to the `Layer` base (std::optional<LayerEffects>, layer.hpp) beside the
// RasterMask, so every kind -- raster, vector, text, group, 3D text -- inherits them with one
// code path, the same choice the base makes for opacity/blendMode/mask.
//
// The full catalogue + fields are defined here (the model is stable from LE-a); LE-a only
// *renders* Stroke + fill-opacity. The remaining effects render in later sessions (overlays
// LE-c/-d, shadows/glows LE-e, bevel/satin LE-f) -- the model does not change when they land.
namespace mosaic::core {

using common::ColorF;
using vec::Paint;  // NoPaint | SolidPaint | Gradient (+ Pattern in LE-d)

// ---------------------------------------------------------------------------------------------
// The effect catalogue (docs/layer-effects.md §5). All fills reuse vec::Paint so solid /
// gradient / pattern work uniformly in strokes and overlays.
// ---------------------------------------------------------------------------------------------

// Stroke -- STACKABLE, concentric. Offset the alpha edge by ±width from its signed-distance
// field, fill the band with `paint`. Inside clips to the shape, Outside extends beyond it (grows
// effectsBounds), Center straddles. Stacked strokes render index-0 innermost, each pushed
// outward by the cumulative width (the white/black/white ring look). Rendered in LE-a.
struct StrokeEffect {
    enum class Align { Inside, Center, Outside };
    float width = 3.0f;  // px in layer space
    Align align = Align::Outside;
    // Default to a solid black paint (a freshly added stroke should draw something); the UI
    // (LE-b) exposes the paint. NoPaint is a legal "invisible" state.
    Paint paint = vec::SolidPaint{ColorF{0.0f, 0.0f, 0.0f, 1.0f}};
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.0f;
    bool enabled = true;
    bool operator==(const StrokeEffect&) const = default;
};

// Colour / Gradient / Pattern overlay share this shape -- the kind of `paint` IS the kind of
// overlay. Independent, each composited with its own blend + opacity (doc §5.3). Rendered LE-c/-d.
struct OverlayEffect {
    Paint paint;  // NoPaint until the UI assigns one
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.0f;
    bool enabled = false;
    bool operator==(const OverlayEffect&) const = default;
};

// Drop + Inner shadow (STACKABLE). Drop: blur the alpha by `size`, choke by `spread`, offset by
// (angle,distance), colourise, place BELOW the layer. Inner: blur (1-alpha), offset inward, clip
// to the alpha, place above. Global-Light aware in LE-g. Rendered LE-e.
struct ShadowEffect {
    ColorF color{0.0f, 0.0f, 0.0f, 1.0f};
    float opacity = 0.75f;
    BlendMode blend = BlendMode::Multiply;
    float angleDeg = 120.0f;
    float distance = 6.0f;
    float spread = 0.0f;  // choke
    float size = 6.0f;    // blur radius
    bool enabled = false;
    bool operator==(const ShadowEffect&) const = default;
};

// Outer + Inner glow (single each). Outer: blur the alpha outward by `size`, colourise, place
// below/around. Inner: blur inward from the edge (or centre), clip to the alpha. Rendered LE-e.
struct GlowEffect {
    Paint paint;
    float opacity = 0.75f;
    BlendMode blend = BlendMode::Screen;
    float choke = 0.0f;
    float size = 8.0f;
    enum class Source { Edge, Center } source = Source::Edge;  // inner glow only
    bool enabled = false;
    bool operator==(const GlowEffect&) const = default;
};

// Bevel & Emboss (single). Height field h = clamp(SDF(alpha)/size); normal map from a
// SINGLE-PASS Sobel (never iterative / pyramid); raked Blinn light -> highlight/shadow. Rendered
// LE-f.
struct BevelEffect {
    enum class Style { OuterBevel, InnerBevel, Emboss, PillowEmboss };
    Style style = Style::InnerBevel;
    float depth = 1.0f;
    float size = 5.0f;
    float soften = 0.0f;
    float angleDeg = 120.0f;
    float altitudeDeg = 30.0f;
    ColorF highlight{1.0f, 1.0f, 1.0f, 1.0f};
    float highlightOpacity = 0.75f;
    ColorF shadow{0.0f, 0.0f, 0.0f, 1.0f};
    float shadowOpacity = 0.75f;
    bool enabled = false;
    bool operator==(const BevelEffect&) const = default;
};

// Satin (single). Duplicate the alpha, offset by (angle,distance), blur by `size`, interfere
// with itself, clip to the alpha, colourise -- the folded-fabric sheen. Rendered LE-f.
struct SatinEffect {
    ColorF color{0.0f, 0.0f, 0.0f, 1.0f};
    float opacity = 0.5f;
    BlendMode blend = BlendMode::Multiply;
    float angleDeg = 19.0f;
    float distance = 11.0f;
    float size = 14.0f;
    bool invert = true;
    bool enabled = false;
    bool operator==(const SatinEffect&) const = default;
};

// The per-layer effect stack. Stacking is WITHIN a type (the vectors); the canonical render
// z-order is fixed (not user-reorderable): drop shadow -> outer glow -> layer fill -> colour ->
// gradient -> pattern overlay -> satin -> inner shadow -> inner glow -> bevel -> stroke
// (doc §4). applyEffects walks the fields in that order.
struct LayerEffects {
    float fillOpacity = 1.0f;  // dims the layer's OWN pixels only, effects stay full (doc §1.9)
    std::vector<ShadowEffect> dropShadows;  // stackable
    GlowEffect outerGlow;
    OverlayEffect colorOverlay;
    OverlayEffect gradientOverlay;
    OverlayEffect patternOverlay;
    SatinEffect satin;
    std::vector<ShadowEffect> innerShadows;  // stackable
    GlowEffect innerGlow;
    BevelEffect bevel;
    std::vector<StrokeEffect> strokes;  // stackable, concentric

    bool operator==(const LayerEffects&) const = default;

    // No enabled effect and fill-opacity == 1 -> the layer takes today's exact
    // renderLayer->walkStep path, byte-identical. A HARD requirement: untouched layers must not
    // change or pay any cost (the compositor short-circuits on this, doc §8).
    [[nodiscard]] bool empty() const {
        if (fillOpacity != 1.0f) return false;
        for (const StrokeEffect& s : strokes)
            if (s.enabled) return false;
        for (const ShadowEffect& s : dropShadows)
            if (s.enabled) return false;
        for (const ShadowEffect& s : innerShadows)
            if (s.enabled) return false;
        return !(outerGlow.enabled || innerGlow.enabled || colorOverlay.enabled ||
                 gradientOverlay.enabled || patternOverlay.enabled || satin.enabled ||
                 bevel.enabled);
    }
};

// The maximum distance (px in layer space) any effect paints OUTSIDE the layer's alpha edge.
// effectsBounds() (Layer) dilates contentBounds by this, and the compositor grows a group's
// isolated buffer by it, so an outside stroke / drop shadow / outer glow is not clipped
// (doc §4). Inner effects (inner shadow/glow, bevel, overlays clipped to the alpha) add none.
[[nodiscard]] inline float effectsOutwardReach(const LayerEffects& fx) {
    float reach = 0.0f;
    // Concentric outside/centre strokes stack outward, so their reach is the sum of widths.
    float strokeOut = 0.0f;
    for (const StrokeEffect& s : fx.strokes) {
        if (!s.enabled || s.align == StrokeEffect::Align::Inside) continue;
        strokeOut += s.width;
    }
    reach = std::max(reach, strokeOut);
    for (const ShadowEffect& sh : fx.dropShadows)
        if (sh.enabled) reach = std::max(reach, sh.distance + sh.size + sh.spread);
    if (fx.outerGlow.enabled) reach = std::max(reach, fx.outerGlow.size + fx.outerGlow.choke);
    return reach;
}

}  // namespace mosaic::core
