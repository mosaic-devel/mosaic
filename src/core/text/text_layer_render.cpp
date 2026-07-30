#include "core/text/text_layer_render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/text/extrude_render.hpp"  // projectedExtrudeBounds (3D cache extent, S30-c)
#include "core/text/shaping.hpp"         // BentArc::sectorContains + bentSectorBounds (bent clip)
#include "core/text/text_render.hpp"

namespace mosaic::core::text {
namespace {

// Soft cap on a text cache image so a pathological size (huge font * long line, or a big stretch)
// can't allocate gigabytes. Far beyond any reasonable on-canvas block; the GPU-resident path lifts
// the ceiling. When a baked stretch would exceed it, the bake is scaled down to fit (the residual
// magnification then resamples, as before) -- crisp up to an 8192px glyph cache, graceful past it.
constexpr long kMaxCacheDim = 8192;

// The 2x2 linear part of an affine (scale/rotation/shear), translation dropped.
common::Affine2D linearPart(const common::Affine2D& a) {
    return {a.m00, a.m01, 0.0, a.m10, a.m11, 0.0};
}

// Equal linear parts within a small tolerance? Avoids a re-raster on sub-pixel transform jitter.
bool linearClose(const common::Affine2D& a, const common::Affine2D& b) {
    constexpr double kEps = 1e-4;
    return std::abs(a.m00 - b.m00) < kEps && std::abs(a.m01 - b.m01) < kEps &&
           std::abs(a.m10 - b.m10) < kEps && std::abs(a.m11 - b.m11) < kEps;
}

// The DOCUMENT-space bounds of a text layer's currently cached pixels (empty when uncached): the
// area a cache swap visibly changes on screen, padded 2 doc px for the compositor's resample
// filter footprint (a rotated/scaled layer's bilinear taps read past the mapped edge).
common::Rect cachedDocBounds(const TextLayer& t) {
    const common::Image* img = t.cachedImage();
    if (img == nullptr || img->rgba.empty())
        return {};
    const common::Rect local = t.cacheImageToLayer().mapBounds(
        {0.0, 0.0, static_cast<double>(img->width), static_cast<double>(img->height)});
    const common::Rect doc = worldTransform(t).mapBounds(local);
    return {doc.x - 2.0, doc.y - 2.0, doc.w + 4.0, doc.h + 4.0};
}

void refreshGroup(GroupLayer& group, TextShaper& shaper, const FontProvider& fonts, LayerId editing,
                  bool liveDrag, bool& any, common::Rect* dirty, bool draftEditing) {
    const auto accumulate = [&](const common::Rect& r) {
        if (dirty == nullptr || r.empty())
            return;
        *dirty = dirty->empty() ? r : dirty->united(r);
    };
    for (const auto& child : group.children()) {
        if (auto* g = child->as<GroupLayer>()) {
            refreshGroup(*g, shaper, fonts, editing, liveDrag, any, dirty, draftEditing);
        } else if (auto* t = child->as<TextLayer>()) {
            // The OLD pixels' extent is read BEFORE the refresh replaces them: both the vacated
            // area and the newly-covered one need repainting.
            const common::Rect before = dirty != nullptr ? cachedDocBounds(*t) : common::Rect{};
            if (refreshTextCache(*t, shaper, fonts, /*clipToArea=*/t->id() != editing, liveDrag,
                                 /*draft=*/draftEditing && t->id() == editing)) {
                any = true;
                accumulate(before);
                accumulate(cachedDocBounds(*t));
            }
        }
    }
}

}  // namespace

bool refreshTextCache(TextLayer& layer, TextShaper& shaper, const FontProvider& fonts,
                      bool clipToArea, bool freezeTransform, bool draft) {
    const TextBlock& block = layer.block();
    // An Area block's pixels are clamped to its frame box so overset text disappears (#3); the block
    // being edited passes clipToArea=false so its overflow stays visible while you type. An EXTRUDED
    // block is never box-clipped -- the rotated solid legitimately pokes past the flat frame (S30-c).
    const bool isArea =
        block.frame == TextFrame::Area && block.areaSize.x > 0.0 && block.areaSize.y > 0.0;
    const bool clip = clipToArea && isArea && !block.extrude;

    // Item 8: bake the layer transform's linear part (scale/rotation/shear) into the rasterization so
    // the contours are sampled at device res -- a stretched layer stays crisp instead of upsampling a
    // 1px/unit bitmap. During a live transform drag we KEEP the already-baked linear (freezeTransform)
    // so the gesture costs no re-raster per frame; the commit (not frozen) re-bakes the settled value.
    // `requested` is the cache-validity key (the transform asked for); `bake` is what we actually
    // rasterize through -- equal unless the soft cap forces a downscale (handled below).
    const common::Affine2D requested =
        freezeTransform ? layer.cacheLinear() : linearPart(layer.transform());
    // The Layer-Effects overlays an EXTRUDED render bakes per face (S30-e, §12) join the validity
    // key: an effects edit re-renders the solid without any block edit. Flat blocks key on the
    // defaults -- their overlays are applied by the 2D effect pass, never baked here.
    const bool bakesOverlays = block.extrude.has_value() && layer.hasEffects();
    const std::array<OverlayEffect, 3> overlayKey =
        bakesOverlays ? std::array<OverlayEffect, 3>{layer.effects().colorOverlay,
                                                     layer.effects().gradientOverlay,
                                                     layer.effects().patternOverlay}
                      : std::array<OverlayEffect, 3>{};
    if (layer.cacheCurrent() && layer.cacheClipped() == clip &&
        linearClose(layer.cacheLinear(), requested) && layer.cachedOverlays() == overlayKey)
        return false;  // already up to date for this clip state + baked transform + overlays
    layer.setCachedOverlays(overlayKey);  // every path below re-renders and re-caches
    common::Affine2D bake = requested;
    // Draft (a live drag / hover preview): rasterize at HALF resolution -- a quarter of the raster
    // cost -- and let the compositor's residual magnification upsample, exactly like the
    // kMaxCacheDim soft cap below. The halved bake is stored as cacheLinear, so it can never pass
    // linearClose against `requested` again: the first non-draft refresh re-renders crisp with no
    // extra state to forget.
    if (draft)
        bake = common::Affine2D::scaling(0.5, 0.5) * bake;

    // The object's extent: an Area block is its frame box (so the whole box is clickable + the move
    // gizmo frames it, even where it's empty / overset); a Point block is the tight text bounds. The
    // content box stays in LAYER-LOCAL units (the move gizmo / hit-test apply the layer transform).
    const std::optional<common::Rect> areaBox =
        isArea ? std::optional<common::Rect>(common::Rect{0.0, 0.0, block.areaSize.x, block.areaSize.y})
               : std::nullopt;
    std::optional<common::Rect> natural = layoutBounds(shaper, block, fonts);
    const std::optional<common::Rect> contentBox = isArea ? areaBox : natural;
    // An extruded solid can project past the flat layout box (rotation, depth, perspective):
    // grow the cache extent by the conservative projected bounds so it is never clipped. The
    // CONTENT box above stays the flat frame -- the 2D box gizmo places the text; the solid may
    // legitimately overhang it (§10.3: the gizmo orients the solid, the 2D transform places it).
    if (natural && block.extrude)
        natural = natural->united(projectedExtrudeBounds(*natural, *block.extrude));

    if (!natural || natural->empty()) {  // empty text: no pixels (a bare caret draws as overlay)
        layer.setCachedContentBounds(contentBox);
        layer.setCachedInkBounds(std::nullopt); // no pixels, no ink
        layer.setCachedImage(std::nullopt, common::Affine2D::identity(), clip, requested);
        return true;
    }

    // The image grid lives in BAKED space (the layout run through `bake`): its AABB sets the extent,
    // so glyph contours are rasterized at the final scale. If that would blow the soft cap, shrink the
    // bake uniformly to fit (the leftover magnification resamples in the compositor, as it used to).
    constexpr double kPad = 2.0;
    common::Rect bn = bake.mapBounds(*natural);
    const double maxDim = std::max(bn.w, bn.h) + 2.0 * kPad;
    if (maxDim > static_cast<double>(kMaxCacheDim)) {
        const double f = static_cast<double>(kMaxCacheDim) / maxDim;
        bake = common::Affine2D::scaling(f, f) * bake;
        bn = bake.mapBounds(*natural);
    }

    // Pad a couple of pixels so glyph AA / side-bearing overshoot past the metrics box is not clipped,
    // and snap the origin to whole pixels (the image grid).
    long ox = static_cast<long>(std::floor(bn.x - kPad));
    long oy = static_cast<long>(std::floor(bn.y - kPad));
    long x1 = static_cast<long>(std::ceil(bn.right() + kPad));
    long y1 = static_cast<long>(std::ceil(bn.bottom() + kPad));
    // ⚠ A BENT Area frame is not its flat rect (user 2026-07-14: "the Area text clipping mask does
    // not conform to bend"): applyBend warps the whole box into an annular sector whose arch rises
    // past the flat top, so a rect clamp sheared the arch flat. The clip becomes the sector itself:
    // its bounds clamp the image extent here, and a per-pixel mask below cuts the overset against
    // the box the user actually SEES. The frame arc is derived from the BLOCK alone -- {0, 0,
    // areaSize.x, bend*sweep}, the same frame-driven arc applyBend lays the text on -- so no shaped
    // block is needed. Unbent keeps the exact rect clamp (and no mask): byte-identical.
    const bool bentClip = clip && block.writingMode == WritingMode::HorizontalTB &&
                          std::abs(block.bend) > 1e-4f;
    const ShapedBlock::BentArc frameArc{
        0.0f, 0.0f, static_cast<float>(block.areaSize.x),
        bentClip ? static_cast<float>(block.bend * kBendMaxSweep) : 0.0f, bentClip};
    if (clip) {  // clamp the rendered image to the box edges so overset glyphs are cut (round-4 #3);
        // the box is mapped through `bake` too (its AABB under rotation/shear -- exact for a scale).
        const common::Rect ab = bake.mapBounds(
            bentClip ? bentSectorBounds(frameArc, block.areaSize.y) : *areaBox);
        ox = std::max(ox, static_cast<long>(std::floor(ab.x)));
        oy = std::max(oy, static_cast<long>(std::floor(ab.y)));
        x1 = std::min(x1, static_cast<long>(std::ceil(ab.right())));
        y1 = std::min(y1, static_cast<long>(std::ceil(ab.bottom())));
    }
    if (x1 <= ox || y1 <= oy) {  // the box clipped away every glyph: no pixels, but keep the box bounds
        layer.setCachedContentBounds(contentBox);
        layer.setCachedInkBounds(std::nullopt); // no pixels, no ink
        layer.setCachedImage(std::nullopt, common::Affine2D::identity(), clip, requested);
        return true;
    }
    const long w = std::clamp(x1 - ox, 1L, kMaxCacheDim);
    const long h = std::clamp(y1 - oy, 1L, kMaxCacheDim);

    // layer-local -> image-px: apply the baked linear, then shift to the grid origin. Its inverse maps
    // image-px -> layer-local (the cache's contract), so `transform * cacheImageToLayer` collapses to
    // the residual translation -- the compositor never re-applies the (already-baked) scale/rotation.
    const common::Affine2D toPixel =
        common::Affine2D::translation(static_cast<double>(-ox), static_cast<double>(-oy)) * bake;
    const std::optional<common::Affine2D> imageToLayer = toPixel.inverse();
    if (!imageToLayer) {  // a singular bake (a degenerate 0-area scale): nothing to show
        layer.setCachedContentBounds(contentBox);
        layer.setCachedInkBounds(std::nullopt); // no pixels, no ink
        layer.setCachedImage(std::nullopt, common::Affine2D::identity(), clip, requested);
        return true;
    }
    // The 3D canvas-reflection snapshot, when the block wants it and the app has built one.
    ExtrudeEnv env;
    const ExtrudeEnv* envPtr = nullptr;
    if (block.extrude && block.extrude->reflectCanvas && layer.reflectionEnv() != nullptr) {
        env.image = layer.reflectionEnv();
        env.layerToEnv = layer.reflectionEnvTransform();
        envPtr = &env;
    }
    common::Image img = renderText(shaper, block, fonts, static_cast<std::uint32_t>(w),
                                   static_cast<std::uint32_t>(h), toPixel, 0.25, envPtr,
                                   bakesOverlays ? &layer.effects() : nullptr);
    if (bentClip) {
        // The per-pixel half of the bent clip: zero everything outside the warped sector. Each
        // image pixel unbakes to layer space and takes BentArc::sectorContains -- pointAt's exact
        // inverse, so the cut line IS the frame the chrome draws. The edge is a hard cut, exactly
        // as the rect clamp's edge always was.
        for (std::uint32_t py = 0; py < img.height; ++py) {
            std::uint8_t* row = &img.rgba[static_cast<std::size_t>(py) * img.width * 4];
            for (std::uint32_t px = 0; px < img.width; ++px) {
                const common::Vec2 p = imageToLayer->apply(
                    {static_cast<double>(px) + 0.5, static_cast<double>(py) + 0.5});
                if (!frameArc.sectorContains({p.x, p.y}, block.areaSize.y))
                    std::memset(row + static_cast<std::size_t>(px) * 4, 0, 4);
            }
        }
    }
    // Measure the INK's bbox while the pixels are in hand: the alpha > 0 extent, mapped back to
    // layer space -- the tightest honest "where is this text on screen" (the rotate affordance
    // anchors to it; see TextLayer::cachedInkBounds). One row-major pass over pixels the render
    // just wrote; the row scans stop at the first/last hits so it costs a fraction of the raster.
    {
        long iy0 = -1, iy1 = -1;
        long ix0 = static_cast<long>(img.width), ix1 = -1;
        for (std::uint32_t py = 0; py < img.height; ++py) {
            const std::uint8_t* row = &img.rgba[static_cast<std::size_t>(py) * img.width * 4];
            long first = -1, last = -1;
            for (std::uint32_t px = 0; px < img.width; ++px)
                if (row[px * 4 + 3] != 0) { first = static_cast<long>(px); break; }
            if (first < 0)
                continue;
            for (std::uint32_t px = img.width; px-- > 0;)
                if (row[px * 4 + 3] != 0) { last = static_cast<long>(px); break; }
            if (iy0 < 0)
                iy0 = static_cast<long>(py);
            iy1 = static_cast<long>(py);
            ix0 = std::min(ix0, first);
            ix1 = std::max(ix1, last);
        }
        if (iy0 >= 0) {
            layer.setCachedInkBounds(imageToLayer->mapBounds(
                {static_cast<double>(ix0), static_cast<double>(iy0),
                 static_cast<double>(ix1 - ix0 + 1), static_cast<double>(iy1 - iy0 + 1)}));
        } else {
            layer.setCachedInkBounds(std::nullopt); // rendered, but every pixel is transparent
        }
    }
    // ⚠ A DRAFT stores its halved bake as the validity key -- it must NEVER read as current
    // against `requested`, or it would stay soft forever. A soft-CAP downscale stores `requested`
    // deliberately (the cap is permanent for that size; re-rendering it changes nothing).
    layer.setCachedImage(std::move(img), *imageToLayer, clip, draft ? bake : requested);
    layer.setCachedContentBounds(contentBox);
    return true;
}

bool refreshTextCaches(Document& doc, TextShaper& shaper, const FontProvider& fonts, LayerId editing,
                       bool liveDrag, common::Rect* dirtyDocOut, bool draftEditing) {
    bool any = false;
    if (dirtyDocOut != nullptr)
        *dirtyDocOut = {};
    refreshGroup(doc.root(), shaper, fonts, editing, liveDrag, any, dirtyDocOut, draftEditing);
    return any;
}

}  // namespace mosaic::core::text
