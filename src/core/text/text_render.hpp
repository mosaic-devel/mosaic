#pragma once

#include <cstdint>
#include <optional>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/text/font_provider.hpp"
#include "core/text/shaping.hpp"
#include "core/text/text_model.hpp"

// CPU text rasterization -- the conservative lane (docs/type-tool.md §5.2):
// shape -> per-glyph Contours -> the S25 analytic vector rasterizer, per-run paint, with COLR/CPAL
// & embedded-bitmap colour glyphs composited on top. This is the S29-a floor; the GPU-resident
// Slug/MSDF path (§5.3) is a later backend swap that consumes the SAME ShapedBlock cache.
namespace mosaic::core {
struct LayerEffects;  // layer_effects.hpp: the per-layer effect stack (S30-e overlays)
}

namespace mosaic::core::text {

struct ExtrudeEnv;  // extrude_render.hpp: the 3D canvas-reflection snapshot (optional)

// Render `block` into a fresh straight-alpha FLOAT RGBA image (width x height), mapping layer-local
// em space to target pixels via `toPixel` (so the caller folds in the layer transform + zoom).
// `shaper` is reused across calls for its face cache; `fonts` resolves families/fallback. Honours
// each run's vec::Paint (solid/gradient), underline/strikethrough, colour glyphs, and block.aa
// (None hardens edges to 0/1; Subpixel degrades to Grayscale in S29-a). `tolerancePx` is the curve
// flattening tolerance in device pixels. `env` feeds the 3D lane's canvas reflections (ignored
// unless block.extrude && extrude->reflectCanvas). `effects` feeds the 3D lane's per-face overlay
// mapping (§12, S30-e): an EXTRUDED block bakes the layer's colour/gradient/pattern overlays onto
// its front face (walls too with Extrude::overlayWrapSides) -- ignored for flat text, whose
// overlays the 2D effect pass applies (layer_effects_render).
[[nodiscard]] common::ImageF renderTextF(TextShaper& shaper, const TextBlock& block,
                                         const FontProvider& fonts, std::uint32_t width,
                                         std::uint32_t height, const common::Affine2D& toPixel,
                                         double tolerancePx = 0.25,
                                         const ExtrudeEnv* env = nullptr,
                                         const LayerEffects* effects = nullptr);

// 8-bit RGBA convenience wrapper (toImage8 of renderTextF).
[[nodiscard]] common::Image renderText(TextShaper& shaper, const TextBlock& block,
                                       const FontProvider& fonts, std::uint32_t width,
                                       std::uint32_t height, const common::Affine2D& toPixel,
                                       double tolerancePx = 0.25,
                                       const ExtrudeEnv* env = nullptr,
                                       const LayerEffects* effects = nullptr);

// Tight layer-local bounds of the laid-out block (what TextLayer::contentBounds caches; the Move
// gizmo / thumbnails frame this). nullopt for empty text. Shapes the block (cheap; cacheable).
[[nodiscard]] std::optional<common::Rect> layoutBounds(TextShaper& shaper, const TextBlock& block,
                                                       const FontProvider& fonts);

// Render `sample` set in `family` (at `sizePx`, painted `color`) into a fresh `maxW`x`maxH` straight-
// alpha RGBA image, the text vertically centred with a small inset; glyphs past `maxW` are clipped.
// `rightAlign` pins the sample to the cell's RIGHT edge (so its ink ends at a consistent x regardless
// of width -- the font picker hugs the preview to the selection dot); false left-aligns it. The
// font-picker preview cell (docs/type-tool.md §8) -- it draws each family in its own face through the
// same renderer the canvas uses, so emoji/CJK/etc. preview correctly.
[[nodiscard]] common::Image renderFontSample(TextShaper& shaper, const FontProvider& fonts,
                                             const std::string& family, const std::string& sample,
                                             float sizePx, common::ColorF color, int maxW, int maxH,
                                             bool rightAlign = false);

}  // namespace mosaic::core::text
