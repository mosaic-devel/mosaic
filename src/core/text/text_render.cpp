// CPU text rasterization (docs/type-tool.md §5.2). See shaping.cpp for the full technique lineage:
// HarfBuzz shaping + FreeType outlines feed the S25 analytic vector rasterizer here. Per-run paint
// reuses vec::Paint (solid + gradient) through rasterizeObjectF, so text needs NO new fill code --
// it is a producer of the existing Contours seam (§5.1), not a new rendering primitive.
#include "core/text/text_render.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/text/extrude_mesh.hpp"
#include "core/text/extrude_overlay.hpp"
#include "core/text/extrude_render.hpp"
#include "core/vector/object.hpp"
#include "core/vector/raster.hpp"

namespace mosaic::core::text {
namespace {

// Source-over composite `src` onto `dst` (same dims; straight-alpha float RGBA).
void compositeOver(common::ImageF& dst, const common::ImageF& src) {
    const std::size_t n = dst.rgba.size();
    for (std::size_t p = 0; p < n; p += 4) {
        const float sa = src.rgba[p + 3];
        if (sa <= 0.0f) continue;
        const float da = dst.rgba[p + 3];
        const float outA = sa + da * (1.0f - sa);
        if (outA <= 0.0f) {
            dst.rgba[p + 0] = dst.rgba[p + 1] = dst.rgba[p + 2] = dst.rgba[p + 3] = 0.0f;
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            dst.rgba[p + c] =
                (src.rgba[p + c] * sa + dst.rgba[p + c] * da * (1.0f - sa)) / outA;
        }
        dst.rgba[p + 3] = outA;
    }
}

// Append the flattened layer-local `contours` to `path` as closed straight-polyline subpaths.
void appendContours(vec::Path& path, const vec::Contours& contours) {
    for (const vec::Contour& c : contours) {
        if (c.points.size() < 2) continue;
        vec::SubPath sp;
        sp.closed = true;  // glyph outlines are closed regions
        sp.nodes.reserve(c.points.size());
        for (const common::Vec2& pt : c.points) sp.nodes.push_back(vec::Node{pt, pt, pt});
        path.subpaths.push_back(std::move(sp));
    }
}

// Append an axis-aligned filled rectangle (a decoration bar) as a closed subpath in layer space.
void appendRect(vec::Path& path, double x0, double y0, double x1, double y1) {
    vec::SubPath sp;
    sp.closed = true;
    auto node = [](common::Vec2 p) { return vec::Node{p, p, p}; };
    sp.nodes = {node({x0, y0}), node({x1, y0}), node({x1, y1}), node({x0, y1})};
    path.subpaths.push_back(std::move(sp));
}

// Bilinear-sample a straight-alpha colour-glyph tile into `dst` through the layer->pixel transform.
// The tile holds `pixelScale` tile-px per layer unit; we inverse-map each covered target pixel back
// to layer space, then to tile space, so this handles scale/translation/rotation uniformly.
void blitColorTile(common::ImageF& dst, const TextShaper::ColorGlyphTile& tile,
                   const common::Affine2D& toPixel) {
    const std::uint32_t tw = tile.rgba.width, th = tile.rgba.height;
    if (tw == 0 || th == 0 || tile.pixelScale <= 0.0f) return;
    const double layerW = static_cast<double>(tw) / tile.pixelScale;
    const double layerH = static_cast<double>(th) / tile.pixelScale;
    const common::Rect layerRect{tile.origin.x, tile.origin.y, layerW, layerH};
    const auto inv = toPixel.inverse();
    if (!inv) return;

    const common::Rect pxBox = toPixel.mapBounds(layerRect);
    const long x0 = std::max<long>(0, static_cast<long>(std::floor(pxBox.x)));
    const long y0 = std::max<long>(0, static_cast<long>(std::floor(pxBox.y)));
    const long x1 = std::min<long>(dst.width, static_cast<long>(std::ceil(pxBox.right())));
    const long y1 = std::min<long>(dst.height, static_cast<long>(std::ceil(pxBox.bottom())));

    for (long py = y0; py < y1; ++py) {
        for (long px = x0; px < x1; ++px) {
            const common::Vec2 layer = inv->apply({px + 0.5, py + 0.5});
            const double tx = (layer.x - tile.origin.x) * tile.pixelScale - 0.5;
            const double ty = (layer.y - tile.origin.y) * tile.pixelScale - 0.5;
            if (tx < -0.5 || ty < -0.5 || tx > tw - 0.5 || ty > th - 0.5) continue;
            const long ix = static_cast<long>(std::floor(tx));
            const long iy = static_cast<long>(std::floor(ty));
            const double fx = tx - ix, fy = ty - iy;
            auto sample = [&](long sx, long sy, int ch) -> float {
                sx = std::clamp<long>(sx, 0, tw - 1);
                sy = std::clamp<long>(sy, 0, th - 1);
                return tile.rgba.rgba[(static_cast<std::size_t>(sy) * tw + sx) * 4 + ch] / 255.0f;
            };
            common::ColorF c;
            float* out = &c.r;
            for (int ch = 0; ch < 4; ++ch) {
                const float a = sample(ix, iy, ch) * (1 - fx) * (1 - fy) +
                                sample(ix + 1, iy, ch) * fx * (1 - fy) +
                                sample(ix, iy + 1, ch) * (1 - fx) * fy +
                                sample(ix + 1, iy + 1, ch) * fx * fy;
                out[ch] = a;
            }
            if (c.a <= 0.0f) continue;
            const std::size_t p = (static_cast<std::size_t>(py) * dst.width + px) * 4;
            const float da = dst.rgba[p + 3];
            const float outA = c.a + da * (1.0f - c.a);
            if (outA <= 0.0f) continue;
            dst.rgba[p + 0] = (c.r * c.a + dst.rgba[p + 0] * da * (1 - c.a)) / outA;
            dst.rgba[p + 1] = (c.g * c.a + dst.rgba[p + 1] * da * (1 - c.a)) / outA;
            dst.rgba[p + 2] = (c.b * c.a + dst.rgba[p + 2] * da * (1 - c.a)) / outA;
            dst.rgba[p + 3] = outA;
        }
    }
}

}  // namespace

common::ImageF renderTextF(TextShaper& shaper, const TextBlock& block, const FontProvider& fonts,
                           std::uint32_t width, std::uint32_t height,
                           const common::Affine2D& toPixel, double tolerancePx,
                           const ExtrudeEnv* env, const LayerEffects* effects) {
    common::ImageF result(width, height);  // transparent
    if (width == 0 || height == 0 || block.runs.empty()) return result;

    const ShapedBlock sb = shaper.layout(block, fonts);

    // Item 8 bakes the layer transform's scale into `toPixel`, so the glyph curves must be flattened
    // in DEVICE space: tolerate `tolerancePx` AFTER the bake, not at the (possibly tiny) point size.
    // Otherwise a Move-scaled small-point glyph shows facets ("low-poly") -- the curve was approximated
    // coarsely at its point size and then magnified. The scale is the larger basis-vector magnification
    // of toPixel's linear part (exact for an axis-aligned scale; an upper-bounded estimate otherwise);
    // clamped to >=1 so a shrink never coarsens below the base tolerance.
    const double bakeScale =
        std::max({1.0, std::hypot(toPixel.m00, toPixel.m10), std::hypot(toPixel.m01, toPixel.m11)});
    const double glyphTol = tolerancePx / bakeScale;

    // 3D (S30-c, docs/type-tool.md §10): an extruded block renders through the mesh lane instead
    // of the 2D fill -- into the same ImageF through the same toPixel bake, so it caches,
    // composites and thumbnails exactly like flat text. Colour glyphs (emoji) have no outlines to
    // extrude and are skipped for now; 2D decorations (underline bars) don't apply to a solid.
    if (block.extrude) {
        std::vector<GlyphSolidInput> solids;
        solids.reserve(sb.glyphs.size());
        for (const ShapedGlyph& g : sb.glyphs) {
            if (g.whitespace || g.colorGlyph) continue;
            solids.push_back({shaper.glyphContours(g, glyphTol), g.runIndex});
        }
        ExtrudeMesh mesh = buildExtrudeMesh(solids, *block.extrude);
        // The UVs' sampling domain (§12) is the ink bbox the mesh was BUILT over -- capture it
        // before the camera re-base below swaps designBounds for the shaped bounds.
        const common::Rect uvDomain = mesh.designBounds;
        // Re-base the camera on the SHAPED bounds instead of the tessellated-outline bbox: the
        // editing overlay (ExtrudePlaneMap) derives its pivot from the same sb.bounds, so caret/
        // selection chrome and the render agree exactly -- and the pivot stops depending on the
        // zoom-driven flattening tolerance. (The §12 UVs keep the ink-bbox domain baked above.)
        if (!sb.bounds.empty()) mesh.designBounds = sb.bounds;
        // Layer-Effects overlays mapped per face (§12, S30-e): bake them into design-space albedo
        // maps at the device texel density (bakeScale), pattern edges following the block's AA.
        ExtrudeOverlay overlay;
        if (effects != nullptr && extrudeOverlaysActive(*effects))
            overlay = buildExtrudeOverlay(*effects, mesh, *block.extrude, uvDomain, bakeScale,
                                          block.aa != AntiAlias::None);
        renderExtrudeMeshF(result, mesh, *block.extrude, toPixel,
                           block.aa != AntiAlias::None, env,
                           overlay.empty() ? nullptr : &overlay);
        return result;
    }

    // Accumulate each run's glyph outlines + decoration bars into one path (filled in run order
    // with that run's paint -- solid or gradient -- via the existing object rasterizer).
    std::vector<vec::Path> runPaths(block.runs.size());
    std::vector<TextShaper::ColorGlyphTile> colorTiles;

    for (const ShapedGlyph& g : sb.glyphs) {
        if (g.runIndex >= runPaths.size()) continue;
        if (g.colorGlyph) {
            if (auto tile = shaper.colorGlyphTile(g)) colorTiles.push_back(std::move(*tile));
            continue;
        }
        if (g.whitespace) continue;
        appendContours(runPaths[g.runIndex], shaper.glyphContours(g, glyphTol));
    }

    // Underline / strikethrough: one bar per maximal same-run span on each line.
    for (const ShapedLine& line : sb.lines) {
        std::size_t i = line.begin;
        while (i < line.end) {
            const std::size_t run = sb.glyphs[i].runIndex;
            std::size_t j = i;
            double x0 = sb.glyphs[i].pen.x, x1 = x0;
            while (j < line.end && sb.glyphs[j].runIndex == run) {
                x0 = std::min(x0, sb.glyphs[j].pen.x);
                x1 = std::max(x1, sb.glyphs[j].pen.x + sb.glyphs[j].advance);
                ++j;
            }
            if (run < block.runs.size() && x1 > x0) {
                const CharStyle& st = block.runs[run].style;
                // Underline/strike bars are axis-aligned rectangles spanning the run's flat pen extent,
                // so they only make sense on a straight baseline; a bent block (§9) skips them in v1
                // (per-glyph pens are warped, so a flat bar would cut across the arc). Following the arc
                // with a swept quad strip is a later refinement.
                if ((st.underline || st.strikethrough) && block.bend == 0.0f) {
                    const auto dm = shaper.decorationMetrics(sb.glyphs[i].face, st.sizePx);
                    if (st.underline) {
                        const double y = line.baselineY + dm.underlineOffset;
                        appendRect(runPaths[run], x0, y, x1, y + dm.underlineThickness);
                    }
                    if (st.strikethrough) {
                        const double y = line.baselineY - dm.strikeoutOffset;
                        appendRect(runPaths[run], x0, y - dm.strikeoutThickness * 0.5, x1,
                                   y + dm.strikeoutThickness * 0.5);
                    }
                }
            }
            i = j;
        }
    }

    const bool antialias = block.aa != AntiAlias::None;  // Subpixel degrades to Grayscale in S29-a
    for (std::size_t ri = 0; ri < block.runs.size(); ++ri) {
        if (runPaths[ri].subpaths.empty()) continue;
        if (std::holds_alternative<vec::NoPaint>(block.runs[ri].style.paint)) continue;
        vec::Object obj;
        obj.geometry = std::move(runPaths[ri]);
        obj.fill = block.runs[ri].style.paint;
        obj.stroke.enabled = false;
        const common::ImageF runImg =
            vec::rasterizeObjectF(obj, width, height, toPixel, tolerancePx, antialias);
        compositeOver(result, runImg);
    }

    for (const auto& tile : colorTiles) blitColorTile(result, tile, toPixel);
    return result;
}

common::Image renderText(TextShaper& shaper, const TextBlock& block, const FontProvider& fonts,
                         std::uint32_t width, std::uint32_t height,
                         const common::Affine2D& toPixel, double tolerancePx,
                         const ExtrudeEnv* env, const LayerEffects* effects) {
    return common::toImage8(
        renderTextF(shaper, block, fonts, width, height, toPixel, tolerancePx, env, effects));
}

std::optional<common::Rect> layoutBounds(TextShaper& shaper, const TextBlock& block,
                                         const FontProvider& fonts) {
    if (block.runs.empty()) return std::nullopt;
    const ShapedBlock sb = shaper.layout(block, fonts);
    if (sb.glyphs.empty() || sb.bounds.empty()) return std::nullopt;
    return sb.bounds;
}

common::Image renderFontSample(TextShaper& shaper, const FontProvider& fonts,
                               const std::string& family, const std::string& sample, float sizePx,
                               common::ColorF color, int maxW, int maxH, bool rightAlign) {
    if (maxW <= 0 || maxH <= 0) return {};
    CharStyle style;
    style.font.family = family;
    style.sizePx = sizePx;
    style.setSolidFill(color);
    const TextBlock block = makeBlock(sample, style);

    // Position the laid-out sample vertically centred, left- or right-aligned by a small inset. The
    // bounds are in layer-local em space, which == px here since sizePx is "px at base scale", so the
    // placement is a pure translation (no scale -- different families show their natural relative size).
    constexpr double kInset = 3.0;
    common::Affine2D toPixel = common::Affine2D::translation(kInset, kInset);
    if (const std::optional<common::Rect> b = layoutBounds(shaper, block, fonts)) {
        const double ty = (static_cast<double>(maxH) - b->h) * 0.5 - b->y; // vertical centre
        const double tx = rightAlign ? (static_cast<double>(maxW) - kInset - b->right())
                                     : (kInset - b->x);
        toPixel = common::Affine2D::translation(tx, ty);
    }
    return renderText(shaper, block, fonts, static_cast<std::uint32_t>(maxW),
                      static_cast<std::uint32_t>(maxH), toPixel);
}

}  // namespace mosaic::core::text
