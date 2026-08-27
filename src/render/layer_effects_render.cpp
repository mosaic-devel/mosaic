// Layer effects -- CPU reference lane. Technique lineage:
//   - Blend math: W3C Compositing & Blending L1 (render/blend.hpp).
//   - Blur: separable Gaussian + fast-box (Kovesi). SDF: Felzenszwalb-Huttenlocher 2004.
//   - Bevel: Blinn 1978 bump-mapping; SINGLE-PASS Sobel normals only.
//   - Effects as a concept: Photoshop 5.0 (1998). No ML.

#include "render/layer_effects_render.hpp"

#include "common/dither.hpp"
#include "common/profiler.hpp"
#include "common/thread_pool.hpp"
#include "core/layer_effects.hpp"
#include "render/blend.hpp"
#include "render/effect_primitives.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

namespace mosaic::render {

namespace {

using common::ColorF;
using common::ImageF;

// A pixel rectangle [x0,x1) x [y0,y1) within a buffer.
struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    [[nodiscard]] bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
};

// Band one ROI row loop through the shared pool.
//
// Every effect stage below walks INDEPENDENT rows: each iteration writes exactly one output slot
// per (x,y) -- `io.set(rx0+x, ry0+y)`, `below[y*rw+x]`, `alpha[y*rw+x]` -- and reads only inputs
// that no row writes (the coverage plane, the signed-distance field, an already-blurred field,
// the paint). There is no reduction and no carried state, so the band split cannot change a
// result for any thread count. That is the same argument effect_primitives.cpp carries, and it is
// what lets the layer-effects goldens stand unchanged.
//
// ⚠ Note what is NOT banded: the loops OVER EFFECTS (each concentric ring, each stacked shadow)
// stay sequential, because those composite over one another and the order IS the z-order. Only
// the pixel walk inside one effect is parallel.
template <class Fn> void forEachRow(int rh, Fn&& body) {
    common::parallelFor(static_cast<std::size_t>(rh), 16, [&](std::size_t y0, std::size_t y1) {
        for (int y = static_cast<int>(y0); y < static_cast<int>(y1); ++y)
            body(y);
    });
}

// Both coverage boxes of `io`, in ONE PARALLEL PASS.
//
// These are the first thing applyEffects does, and they used to be two SERIAL full-buffer scans.
// The effects buffer is the target buffer whenever the layer's footprint fits inside it (see
// renderLayer), so on a large document that is ~640 MB of float RGBA streamed twice on one thread
// before any effect has done any work -- while the effect work itself is ROI-clipped and banded.
// Everything else in the renderer goes through parallelFor; this did not.
//
//   `any`   -- alpha > 0: the full extent, 1px AA rim included. Drives the ROI and the
//              fill-opacity dim.
//   `solid` -- alpha > 0.5: the geometric silhouette. The paint PHASE anchor (gradient box origin /
//              pattern tile origin) uses this because it is STABLE: the AA rim adds ~1px, so
//              anchoring to `any` shifts the box origin when the Move-AA filter toggles or a live
//              drag swaps kernels (the "pattern snaps then snaps back" bug).
//
// Fusing them is safe because a texel with alpha <= 0 cannot be > 0.5 either, so the cheap test
// gates both. The band merge is min/max only, which is order-independent -- the result does not
// depend on the thread count, as the compositor's determinism rule requires.
struct AlphaBoxes {
    Box any;
    Box solid;
};

[[nodiscard]] AlphaBoxes alphaBoxes(const ImageF& io) {
    const int w = static_cast<int>(io.width), h = static_cast<int>(io.height);
    if (w <= 0 || h <= 0)
        return {};
    struct Acc {
        int minx, miny, maxx, maxy;
        void hit(int x, int y) noexcept {
            minx = std::min(minx, x);
            miny = std::min(miny, y);
            maxx = std::max(maxx, x);
            maxy = std::max(maxy, y);
        }
        void merge(const Acc& o) noexcept {
            minx = std::min(minx, o.minx);
            miny = std::min(miny, o.miny);
            maxx = std::max(maxx, o.maxx);
            maxy = std::max(maxy, o.maxy);
        }
        [[nodiscard]] Box box() const noexcept {
            return maxx < 0 ? Box{} : Box{minx, miny, maxx + 1, maxy + 1};
        }
    };
    const Acc empty{w, h, -1, -1};
    Acc any = empty, solid = empty;
    std::mutex merge;
    common::parallelFor(static_cast<std::size_t>(h), 32, [&](std::size_t y0, std::size_t y1) {
        Acc la = empty, ls = empty;
        for (std::size_t y = y0; y < y1; ++y) {
            const std::size_t row = y * static_cast<std::size_t>(w) * 4;
            const int yi = static_cast<int>(y);
            for (int x = 0; x < w; ++x) {
                const float a = io.rgba[row + static_cast<std::size_t>(x) * 4 + 3];
                if (a <= 0.0f)
                    continue;
                la.hit(x, yi);
                if (a > 0.5f)
                    ls.hit(x, yi);
            }
        }
        const std::lock_guard<std::mutex> lock(merge);
        any.merge(la);
        solid.merge(ls);
    });
    return {any.box(), solid.box()};
}

// Evaluate a Paint at buffer pixel (px,py). Gradients are keyed to the layer's content box,
// NORMALISED to [0,1]^2 so a gradient runs across the shape (its own transform + spread on top) --
// this is what makes a gradient stroke and a gradient overlay align. Patterns, by contrast, tile in
// LAYER PX with a fixed feature size, so they get real px (not normalised) -- otherwise `scale`
// would be meaningless. A pattern anchors its phase either at the content box origin (moves with the
// shape) or at the buffer/document origin (a static canvas texture the shape slides over -- the
// "Link with Layer" OFF look), per its anchorToCanvas flag. `antialias` (the document-wide AA
// setting) hardens/softens pattern edges. Solid/NoPaint ignore the point + the AA flag.
[[nodiscard]] ColorF paintAtNorm(const core::vec::Paint& paint, int px, int py, const Box& box,
                                 bool antialias, const std::optional<common::Affine2D>& bufferToLayer) {
    if (const auto* pat = std::get_if<core::vec::Pattern>(&paint)) {
        bool canvasAnchor = false;
        if (const auto* pp = std::get_if<core::vec::ProceduralPattern>(pat))
            canvasAnchor = pp->anchorToCanvas;
        if (canvasAnchor)  // static canvas texture: buffer space, unrotated (the shape reveals it)
            return core::vec::sampleAt(paint, {static_cast<double>(px), static_cast<double>(py)},
                                       antialias);
        if (bufferToLayer)  // glued to the layer: sample in layer-local space (rotates/scales with it)
            return core::vec::sampleAt(paint, bufferToLayer->apply({px + 0.5, py + 0.5}), antialias);
        return core::vec::sampleAt(paint, {static_cast<double>(px - box.x0),  // fallback: content box
                                           static_cast<double>(py - box.y0)}, antialias);
    }
    const double bw = std::max(1, box.x1 - box.x0);
    const double bh = std::max(1, box.y1 - box.y0);
    // The dither key is the DESTINATION pixel, not the normalized paint coordinate -- a gradient
    // overlay must dither on the same lattice the rasterizer uses, or the effect bands where the
    // layer does not.
    return core::vec::sampleAt(paint, {(px - box.x0) / bw, (py - box.y0) / bh}, true,
                               core::vec::SamplePixel{px, py, true});
}

// The constant a SOLID paint evaluates to, or nullopt for one that varies per pixel.
//
// `paintColorAt` returns `s->color` for a SolidPaint and reads neither the coordinate nor the
// dither key, so for that arm `paintAtNorm` is a constant function called once per texel -- plus
// two double divisions to build the normalised coordinate it then ignores. `applyStrokes` has
// cached this since it was written (see `Ring::solid` below); the overlays and the two glows had
// not, and they are the effects a GROUP tends to carry. The S60 fixture's Texture folder is an
// outer glow and a colour overlay over children that cover the canvas, so that was ~80 million
// calls through a variant dispatch to fetch the same four floats.
//
// Byte-identical: the hoisted value IS what every one of those calls returned.
[[nodiscard]] std::optional<ColorF> constantPaint(const core::vec::Paint& paint) {
    if (const auto* s = std::get_if<core::vec::SolidPaint>(&paint))
        return s->color;
    return std::nullopt;
}

// A resolved stroke ring: its signed-distance band [lo,hi] (px), paint, compositing, alignment.
// `solid` caches the constant colour; a gradient ring evaluates `paint` per pixel (paintAtNorm).
struct Ring {
    float lo = 0.0f, hi = 0.0f, opacity = 1.0f;
    core::vec::Paint paint;
    bool solid = true;
    ColorF solidColor;
    core::BlendMode blend = core::BlendMode::Normal;
    core::StrokeEffect::Align align = core::StrokeEffect::Align::Outside;
};

// Concentric strokes (doc §5.1): index 0 innermost, each pushed outward by the cumulative width.
// `sd` is the ROI-local signed distance field (px; negative inside).
//
// OUTSIDE strokes composite UNDER the layer content, INSIDE/CENTER strokes over it. That split is
// what stops the translucent seam the naive "all strokes on top" order leaves: the content's own
// anti-aliased rim lives just INSIDE the 0.5 contour, which an outside band (sd>=0) never covers, so
// the content edge would blend over transparency (a coverage dip) between the solid fill and the
// solid stroke. Painting outside strokes below and letting the content's rim blend over them removes
// it -- and crucially keeps text / raster / vector edges crisp (their AA is preserved, not fought).
void applyStrokes(ImageF& io, const std::vector<core::StrokeEffect>& strokes,
                  const std::vector<float>& sd, const std::vector<float>& alpha, const Box& content,
                  int rx0, int ry0, int rw, int rh, bool antialias,
                  const std::optional<common::Affine2D>& bufferToLayer) {
    using Align = core::StrokeEffect::Align;
    // A ring's colour at buffer pixel (px,py): the cached solid, or the gradient/pattern sampled
    // across the content box (paintAtNorm), so a gradient stroke aligns with a gradient overlay.
    const auto ringColor = [&](const Ring& r, int px, int py) -> ColorF {
        return r.solid ? r.solidColor : paintAtNorm(r.paint, px, py, content, antialias, bufferToLayer);
    };
    constexpr float kAa = 1.0f;         // 1px feather; LINEAR ramp (not smoothstep) for a clean edge
    constexpr float kInnerBack = 1.5f;  // how far the innermost OUTSIDE ring reaches under the content
                                        // rim, so the content's AA blends over it (no seam)

    std::vector<Ring> outside, over;  // below the content / over the content (inside + centre)
    float cum = 0.0f;                 // cumulative outward offset along the stack
    for (const core::StrokeEffect& st : strokes) {
        if (!st.enabled) continue;
        const float width = std::max(0.0f, st.width);
        Ring r;
        switch (st.align) {
            case Align::Outside: r.lo = cum;                r.hi = cum + width;        break;
            case Align::Inside:  r.lo = -cum - width;       r.hi = -cum;               break;
            case Align::Center:  r.lo = cum - width * 0.5f; r.hi = cum + width * 0.5f;  break;
        }
        cum += width;
        if (width <= 0.0f) continue;
        // NoPaint draws nothing but still occupies its ring in the stack (so an outer stroke keeps
        // its place). Solid + gradient both paint; a gradient evaluates per pixel below.
        if (std::holds_alternative<core::vec::NoPaint>(st.paint)) continue;
        r.paint = st.paint;
        r.solid = std::holds_alternative<core::vec::SolidPaint>(st.paint);
        if (r.solid) r.solidColor = std::get<core::vec::SolidPaint>(st.paint).color;
        r.opacity = st.opacity;
        r.blend = st.blend;
        r.align = st.align;
        (st.align == Align::Outside ? outside : over).push_back(r);
    }

    // BELOW: outside rings into a scratch buffer, drawn inner-on-top (outermost first) so each ring's
    // AA outer edge fades over the solid ring beneath it -- no ring-to-ring gap. The innermost reaches
    // kInnerBack under the content rim. Then the content is composited over the result.
    if (!outside.empty()) {
        std::vector<ColorF> below(static_cast<std::size_t>(rw) * rh, ColorF{0.0f, 0.0f, 0.0f, 0.0f});
        for (int k = static_cast<int>(outside.size()) - 1; k >= 0; --k) {
            const Ring& r = outside[static_cast<std::size_t>(k)];
            const float innerEdge = r.lo - (k == 0 ? kInnerBack : kAa);
            forEachRow(rh, [&](int y) {
                for (int x = 0; x < rw; ++x) {
                    const std::size_t idx = static_cast<std::size_t>(y) * rw + x;
                    const float d = sd[idx];
                    if (d < innerEdge - kAa || d > r.hi + kAa)
                        continue;
                    const float cov = std::clamp((d - innerEdge) / kAa + 0.5f, 0.0f, 1.0f) *
                                      std::clamp((r.hi - d) / kAa + 0.5f, 0.0f, 1.0f);
                    if (cov <= 0.0f)
                        continue;
                    ColorF src = ringColor(r, rx0 + x, ry0 + y);
                    src.a *= cov;
                    below[idx] = compositeOver(r.blend, below[idx], src, r.opacity);
                }
            });
        }
        forEachRow(rh, [&](int y) {
            for (int x = 0; x < rw; ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * rw + x;
                if (below[idx].a <= 0.0f)
                    continue;
                const std::uint32_t px = static_cast<std::uint32_t>(rx0 + x);
                const std::uint32_t py = static_cast<std::uint32_t>(ry0 + y);
                io.set(px, py,
                       compositeOver(core::BlendMode::Normal, below[idx], io.at(px, py), 1.0f));
            }
        });
    }

    // OVER: inside + centre strokes on top of the content.
    for (const Ring& r : over) {
        const bool inside = r.align == Align::Inside;
        forEachRow(rh, [&](int y) {
            for (int x = 0; x < rw; ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * rw + x;
                const float d = sd[idx];
                float cov;
                if (inside) {
                    // The inside stroke fills from its inner edge OUTWARD (AA only on the inner edge)
                    // and is then clipped to the shape's own coverage below, so the shape's
                    // anti-aliased rim becomes the stroke's outer edge -- the stroke colour reaches
                    // the silhouette instead of leaving a content-coloured rim.
                    if (d < r.lo - kAa || alpha[idx] <= 0.0f)
                        continue;
                    cov = std::clamp((d - r.lo) / kAa + 0.5f, 0.0f, 1.0f);
                } else {  // centre: a plain band (it covers both sides of the edge, so no rim)
                    if (d < r.lo - kAa || d > r.hi + kAa)
                        continue;
                    cov = std::clamp((d - r.lo) / kAa + 0.5f, 0.0f, 1.0f) *
                          std::clamp((r.hi - d) / kAa + 0.5f, 0.0f, 1.0f);
                }
                if (cov <= 0.0f)
                    continue;
                const std::uint32_t px = static_cast<std::uint32_t>(rx0 + x);
                const std::uint32_t py = static_cast<std::uint32_t>(ry0 + y);
                ColorF src = ringColor(r, rx0 + x, ry0 + y);
                src.a *= cov;
                ColorF res = compositeOver(r.blend, io.at(px, py), src, r.opacity);
                if (inside)
                    res.a = std::min(res.a, alpha[idx]); // never extend past the silhouette
                io.set(px, py, res);
            }
        });
    }
}

// A Colour / Gradient / Pattern overlay (doc §5.3): fill the layer's shape with the effect's paint,
// clipped to the ORIGINAL coverage (so fill-opacity can't shrink it), composited with the overlay's
// own blend + opacity on top of the layer's (fill-dimmed) pixels. Solid / gradient / (procedural)
// pattern paints all render via paintAtNorm; only NoPaint (an enabled-but-unset overlay) is a no-op.
// `content` is the box a gradient runs across / a pattern's phase anchor.
void applyOverlay(ImageF& io, const core::OverlayEffect& ov, const std::vector<float>& alpha,
                  const Box& content, int rx0, int ry0, int rw, int rh, bool antialias,
                  const std::optional<common::Affine2D>& bufferToLayer) {
    if (!ov.enabled || std::holds_alternative<core::vec::NoPaint>(ov.paint)) return;
    const std::optional<ColorF> flat = constantPaint(ov.paint);
    forEachRow(rh, [&](int y) {
        for (int x = 0; x < rw; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * rw + x;
            const float cov = alpha[idx];
            if (cov <= 0.0f)
                continue; // outside the shape
            ColorF src =
                flat ? *flat
                     : paintAtNorm(ov.paint, rx0 + x, ry0 + y, content, antialias, bufferToLayer);
            // Do NOT fade the overlay by coverage here: composite it over the layer at its OWN alpha,
            // then clamp the RESULT to the coverage below. Fading src by cov let the layer colour bleed
            // through the shape's AA rim, so the anti-aliased edge read in the LAYER's colour instead of
            // the pattern/overlay's (user-reported). The result-clamp still keeps it inside the shape.
            if (src.a <= 0.0f)
                continue; // a transparent overlay pixel (e.g. a pattern's bg) skips
            const auto px = static_cast<std::uint32_t>(rx0 + x);
            const auto py = static_cast<std::uint32_t>(ry0 + y);
            ColorF res = compositeOver(ov.blend, io.at(px, py), src, ov.opacity);
            res.a = std::min(res.a, cov);  // never paint outside the silhouette
            io.set(px, py, res);
        }
    });
}

// ---- LE-e: shadows & glows (doc §5.2, §5.4) -------------------------------------------------
// The blur tier. Every effect keys off ONE ROI signed-distance field of the shape (fx::sdf, neg
// inside); the size->sigma map, the (angle,distance)->offset map and the spread/choke->core dilation
// are shared here so drop/inner shadow + outer/inner glow stay consistent. Blur is the textbook
// separable Gaussian for small radii, the O(n) Kovesi 3-box approximation for large ones. No offset
// handles / no ML -- pure CPU float math, like the stroke + overlay lanes above.

// `size` -> Gaussian std-dev. PS-style, the shadow/glow "Size" reads as the blur RADIUS, so we map
// sigma = size/3: a Gaussian's visible reach is ~3*sigma, so 3*sigma == size -- EXACTLY the outward
// margin effectsOutwardReach() reserves (it adds `size`). Calibration of the exact look is a
// user-visual-pass detail; this keeps the blur inside the ROI the reach machinery sized for us.
[[nodiscard]] float sigmaForSize(float size) { return std::max(0.0f, size) * (1.0f / 3.0f); }

// Above this sigma the exact separable Gaussian's 2*(3*sigma)+1 taps get expensive; the 3-box
// almost-Gaussian (Kovesi) is O(n) in the radius and visually indistinguishable by the third pass.
constexpr float kLargeBlurSigma = 8.0f;

void blurCoverage(std::vector<float>& plane, int w, int h, float sigma) {
    if (sigma <= 0.0f) return;
    if (sigma >= kLargeBlurSigma)
        fx::boxBlurApprox(plane, w, h, sigma);
    else
        fx::gaussianBlur(plane, w, h, sigma);
}

// The shadow OFFSET (buffer px) for (angleDeg, distance). Photoshop convention: `angleDeg` is the
// LIGHT direction (math CCW from +x), the shadow falls OPPOSITE the light, and screen-y points DOWN.
//   dx = -distance * cos(angle),   dy = +distance * sin(angle)
// So the model default angle 120 deg (light from the upper-left) drops the shadow to (+dx,+dy) =
// lower-RIGHT. A drop-shadow field is sampled at (p - offset), so the shape's coverage lands shifted
// by +offset. PINNED by the "drop shadow inks below-and-right" golden.
struct Offset {
    float dx = 0.0f, dy = 0.0f;
};
[[nodiscard]] Offset shadowOffset(float angleDeg, float distance) {
    const float a = angleDeg * 3.14159265358979323846f / 180.0f;
    return {-distance * std::cos(a), distance * std::sin(a)};
}

// Bilinear sample of a rw*rh coverage `field` at ROI-local (fx,fy); anything off the ROI reads 0
// (the reach-padded ROI already contains every sample an in-bounds effect can reach).
[[nodiscard]] float sampleField(const std::vector<float>& field, int w, int h, float fx, float fy) {
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    const auto at = [&](int x, int y) -> float {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0.0f;
        return field[static_cast<std::size_t>(y) * w + x];
    };
    const float top = at(x0, y0) * (1.0f - tx) + at(x0 + 1, y0) * tx;
    const float bot = at(x0, y0 + 1) * (1.0f - tx) + at(x0 + 1, y0 + 1) * tx;
    return top * (1.0f - ty) + bot * ty;
}

// Shape coverage DILATED by `grow` px (grow>0 grows outward, <0 erodes inward) from the signed
// distance `sd` (neg inside): a clean 1px linear ramp at the shifted 0.5 contour. This is how
// `spread`/`choke` fatten (or thin) the solid core before the blur softens it.
[[nodiscard]] float dilatedCov(float sd, float grow) {
    return std::clamp(0.5f + grow - sd, 0.0f, 1.0f);
}

// Drop shadows -> the `below` scratch (transparent, ROI-sized), in VECTOR order (later over earlier).
// Each: dilate the shape core by `spread`, blur by sigma(size), offset by (angle,distance), colourise
// with color x opacity, composite with its own blend. `below` is later placed UNDER io (see
// applyEffects). NOTE (isolated-buffer lane): a shadow's blend acts against what is beneath it IN
// `below`, not against the live document backdrop -- true blend-against-backdrop is LE-g/S60 work.
void applyDropShadows(std::vector<ColorF>& below, const std::vector<core::ShadowEffect>& shadows,
                      const std::vector<float>& sd, int rw, int rh) {
    for (const core::ShadowEffect& sh : shadows) {
        if (!sh.enabled) continue;
        std::vector<float> field(sd.size());
        for (std::size_t i = 0; i < sd.size(); ++i) field[i] = dilatedCov(sd[i], sh.spread);
        blurCoverage(field, rw, rh, sigmaForSize(sh.size));
        const Offset o = shadowOffset(sh.angleDeg, sh.distance);
        forEachRow(rh, [&](int y) {
            for (int x = 0; x < rw; ++x) {
                const float f = sampleField(field, rw, rh, static_cast<float>(x) - o.dx,
                                            static_cast<float>(y) - o.dy);
                if (f <= 0.0f)
                    continue;
                const std::size_t idx = static_cast<std::size_t>(y) * rw + x;
                ColorF src{sh.color.r, sh.color.g, sh.color.b, sh.color.a * f};
                below[idx] = compositeOver(sh.blend, below[idx], src, sh.opacity);
            }
        });
    }
}

// Outer glow -> the `below` scratch, ABOVE the drop shadows (drawn after them). Symmetric (no offset):
// dilate the shape core by `choke`, blur by sigma(size), fill with `paint` (solid/gradient/pattern via
// paintAtNorm, like the overlays), composite with its own blend (Screen by default). The field is ~1
// deep inside (hidden once io is placed over it) and ramps to 0 outside -- that outside ramp is the halo.
void applyOuterGlow(std::vector<ColorF>& below, const core::GlowEffect& glow,
                    const std::vector<float>& sd, const Box& anchor, int rx0, int ry0, int rw, int rh,
                    bool antialias, const std::optional<common::Affine2D>& bufferToLayer) {
    if (!glow.enabled || std::holds_alternative<core::vec::NoPaint>(glow.paint)) return;
    std::vector<float> field(sd.size());
    for (std::size_t i = 0; i < sd.size(); ++i) field[i] = dilatedCov(sd[i], glow.choke);
    blurCoverage(field, rw, rh, sigmaForSize(glow.size));
    const std::optional<ColorF> flat = constantPaint(glow.paint);
    forEachRow(rh, [&](int y) {
        for (int x = 0; x < rw; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * rw + x;
            const float f = field[idx];
            if (f <= 0.0f)
                continue;
            ColorF src =
                flat ? *flat
                     : paintAtNorm(glow.paint, rx0 + x, ry0 + y, anchor, antialias, bufferToLayer);
            if (src.a <= 0.0f)
                continue;
            src.a *= f;
            below[idx] = compositeOver(glow.blend, below[idx], src, glow.opacity);
        }
    });
}

// Inner shadows: composite OVER io, CLIPPED to the captured `alpha` (exactly the applyOverlay clamp),
// so they never paint past the silhouette. The matte is the blurred COMPLEMENT (1 - shape, eroded by
// `spread`), offset by (angle,distance): sampled at (p - offset) it rides up the LIGHT-facing interior
// edges -- with the default angle the dark band hugs the top/left inner edges (the "recessed" look).
void applyInnerShadows(ImageF& io, const std::vector<core::ShadowEffect>& shadows,
                       const std::vector<float>& sd, const std::vector<float>& alpha, int rx0, int ry0,
                       int rw, int rh) {
    for (const core::ShadowEffect& sh : shadows) {
        if (!sh.enabled) continue;
        std::vector<float> field(sd.size());
        for (std::size_t i = 0; i < sd.size(); ++i) field[i] = 1.0f - dilatedCov(sd[i], -sh.spread);
        blurCoverage(field, rw, rh, sigmaForSize(sh.size));
        const Offset o = shadowOffset(sh.angleDeg, sh.distance);
        forEachRow(rh, [&](int y) {
            for (int x = 0; x < rw; ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * rw + x;
                const float cov = alpha[idx];
                if (cov <= 0.0f)
                    continue; // clipped to the shape
                const float f = sampleField(field, rw, rh, static_cast<float>(x) - o.dx,
                                            static_cast<float>(y) - o.dy);
                if (f <= 0.0f)
                    continue;
                const auto px = static_cast<std::uint32_t>(rx0 + x);
                const auto py = static_cast<std::uint32_t>(ry0 + y);
                ColorF src{sh.color.r, sh.color.g, sh.color.b, sh.color.a * f};
                ColorF res = compositeOver(sh.blend, io.at(px, py), src, sh.opacity);
                res.a = std::min(res.a, cov);
                io.set(px, py, res);
            }
        });
    }
}

// Inner glow: composite OVER io, CLIPPED to `alpha`. EDGE source -> the blurred complement (a bright
// band just inside every edge, choked by `choke`). CENTER source -> a distance ramp bright in the deep
// interior and fading out to the edges (choke pushes the bright core inward). Fill with `paint`.
void applyInnerGlow(ImageF& io, const core::GlowEffect& glow, const std::vector<float>& sd,
                    const std::vector<float>& alpha, const Box& anchor, int rx0, int ry0, int rw,
                    int rh, bool antialias, const std::optional<common::Affine2D>& bufferToLayer) {
    if (!glow.enabled || std::holds_alternative<core::vec::NoPaint>(glow.paint)) return;
    std::vector<float> field(sd.size());
    if (glow.source == core::GlowEffect::Source::Edge) {
        for (std::size_t i = 0; i < sd.size(); ++i) field[i] = 1.0f - dilatedCov(sd[i], -glow.choke);
        blurCoverage(field, rw, rh, sigmaForSize(glow.size));
    } else {  // Center: -sd is the interior depth (0 at the edge); a linear ramp over `size` px
        const float inv = 1.0f / std::max(1.0f, glow.size);
        for (std::size_t i = 0; i < sd.size(); ++i)
            field[i] = std::clamp((-sd[i] - glow.choke) * inv, 0.0f, 1.0f);
    }
    const std::optional<ColorF> flat = constantPaint(glow.paint);
    forEachRow(rh, [&](int y) {
        for (int x = 0; x < rw; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * rw + x;
            const float cov = alpha[idx];
            if (cov <= 0.0f)
                continue; // clipped to the shape
            const float f = field[idx];
            if (f <= 0.0f)
                continue;
            ColorF src =
                flat ? *flat
                     : paintAtNorm(glow.paint, rx0 + x, ry0 + y, anchor, antialias, bufferToLayer);
            if (src.a <= 0.0f)
                continue;
            src.a *= f;
            const auto px = static_cast<std::uint32_t>(rx0 + x);
            const auto py = static_cast<std::uint32_t>(ry0 + y);
            ColorF res = compositeOver(glow.blend, io.at(px, py), src, glow.opacity);
            res.a = std::min(res.a, cov);
            io.set(px, py, res);
        }
    });
}

// ============================================================================================= LE-f
// The SHADING tier -- Bevel & Emboss (doc §5.5) and Satin (doc §5.6). Both read the captured
// coverage `alpha` (ROI-local, rw*rh), modulate the layer's own pixels in `io`, and are CLIPPED to
// the silhouette (`res.a = min(res.a, cov)`, exactly like applyOverlay) so nothing paints outside.
// =====================================================================================================

constexpr float kDegToRad = 0.017453292519943295f;  // pi/180

// Map a UI radius (px) to a Gaussian sigma and blur `plane` in place. Small radii take the exact
// separable Gaussian; large radii take the 3-box fast-almost-Gaussian lane (Kovesi) so the cost stays
// linear regardless of radius. A radius ~ 2.5 sigma reads like the shadow/glow blur feel (the radius is
// roughly the visible extent of the kernel). `radiusPx <= 0` is a no-op.
void bevelSatinBlur(std::vector<float>& plane, int w, int h, float radiusPx) {
    if (radiusPx <= 0.0f) return;
    const float sigma = radiusPx / 2.5f;                     // size -> sigma calibration (doc §5)
    if (sigma > 4.0f) fx::boxBlurApprox(plane, w, h, sigma);  // large: the cheap 3-box approximation
    else fx::gaussianBlur(plane, w, h, sigma);                // small: the exact separable kernel
}

// SATIN (doc §5.6) -- the folded-fabric sheen. Take the shape's coverage A, blur it by `size`, then
// interfere two copies of it offset by +/- the (angle,distance) vector: `invert` picks the abs-
// difference of the copies (the default, sheen concentrated near the interference contour) vs their
// clamped sum. Because a Gaussian blur and a translation COMMUTE (both linear + shift-invariant),
// "blur each offset copy then interfere" is identical to "blur A once, then sample the blurred plane
// at p-/+offset" -- so we blur once and sample twice. The interference is the sheen's coverage; it is
// colourised with the satin colour and composited with the effect's blend + opacity, CLIPPED to A.
//
// Angle convention (shared with the bevel light azimuth below): Photoshop-style, 0 deg = +x (east),
// measured counter-clockwise, screen-y down -> a unit direction (cos a, -sin a); the two copies sit
// at +offset and -offset along it (a copy shifted by +v has value A(p - v), hence the p-/+offset sample).
void applySatin(ImageF& io, const core::SatinEffect& sa, const std::vector<float>& alpha, int rx0,
                int ry0, int rw, int rh) {
    if (!sa.enabled || sa.opacity <= 0.0f) return;
    std::vector<float> blurred = alpha;              // blur A once (see the commute note above)
    bevelSatinBlur(blurred, rw, rh, sa.size);
    const float a = sa.angleDeg * kDegToRad;
    const float ox = std::cos(a) * sa.distance;      // offset along the satin axis (px)
    const float oy = -std::sin(a) * sa.distance;     // screen-y down
    // Bilinear sample of the blurred coverage at (fxp,fyp), 0 outside the ROI (== no coverage there).
    const auto sample = [&](float fxp, float fyp) -> float {
        const int x0 = static_cast<int>(std::floor(fxp)), y0 = static_cast<int>(std::floor(fyp));
        const float tx = fxp - static_cast<float>(x0), ty = fyp - static_cast<float>(y0);
        const auto at = [&](int xx, int yy) -> float {
            if (xx < 0 || yy < 0 || xx >= rw || yy >= rh) return 0.0f;
            return blurred[static_cast<std::size_t>(yy) * rw + xx];
        };
        const float a00 = at(x0, y0), a10 = at(x0 + 1, y0);
        const float a01 = at(x0, y0 + 1), a11 = at(x0 + 1, y0 + 1);
        return (a00 * (1.0f - tx) + a10 * tx) * (1.0f - ty) + (a01 * (1.0f - tx) + a11 * tx) * ty;
    };
    forEachRow(rh, [&](int y) {
        for (int x = 0; x < rw; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * rw + x;
            const float cov = alpha[idx];
            if (cov <= 0.0f)
                continue; // clip to the silhouette
            const float a1 = sample(static_cast<float>(x) - ox, static_cast<float>(y) - oy);
            const float a2 = sample(static_cast<float>(x) + ox, static_cast<float>(y) + oy);
            const float s = sa.invert ? std::fabs(a1 - a2) : std::min(1.0f, a1 + a2);
            if (s <= 0.0f)
                continue;
            ColorF src = sa.color;
            src.a = sa.color.a * s;  // the sheen coverage modulates the satin colour's own alpha
            if (src.a <= 0.0f)
                continue;
            const auto px = static_cast<std::uint32_t>(rx0 + x);
            const auto py = static_cast<std::uint32_t>(ry0 + y);
            ColorF res = compositeOver(sa.blend, io.at(px, py), src, sa.opacity);
            res.a = std::min(res.a, cov);  // never paint outside the silhouette
            io.set(px, py, res);
        }
    });
}

// The per-style bevel HEIGHT profile from the signed distance `sd` (px; NEGATIVE inside, 0 at the
// edge, POSITIVE outside). `size` is the bevel band width. Only the GRADIENT of the field matters to
// the shading, so a flat plateau (gradient 0) leaves the surface un-shaded; the ramp is the bevel.
//   InnerBevel   -- rises inward: 0 at the edge -> 1 at depth `size` inside, plateau deeper. Its slope
//                   lives entirely INSIDE the silhouette (the common, fully-supported case).
//   OuterBevel   -- the mirror: plateau 1 inside, ramps 1->0 across the OUTSIDE band. **v1 LIMIT:** its
//                   slope is OUTSIDE the alpha, so the clip-to-silhouette below hides all but the ~1px
//                   edge -- the exterior lip that should extend BEYOND the alpha is a later refinement
//                   (it would need effectsOutwardReach() to grow the ROI + dropping the alpha clip for
//                   this style; deliberately NOT done here so the model/bounds stay untouched, per the
//                   LE-f brief). InnerBevel/Emboss/PillowEmboss are the everyday cases and work fully.
//   Emboss       -- straddles the edge: 1 inside -> 0.5 at the edge -> 0 outside, so both sides bevel;
//                   the inner half of the ramp shows within the silhouette.
//   PillowEmboss -- InnerBevel negated (the field dips inward), so highlight/shadow swap -> pressed-in.
[[nodiscard]] float bevelHeight(float sd, float size, core::BevelEffect::Style style) {
    const float t = sd / size;  // normalised signed distance (negative inside)
    using S = core::BevelEffect::Style;
    switch (style) {
        case S::InnerBevel:   return std::clamp(-t, 0.0f, 1.0f);
        case S::OuterBevel:   return std::clamp(1.0f - t, 0.0f, 1.0f);
        case S::Emboss:       return std::clamp(0.5f - 0.5f * t, 0.0f, 1.0f);
        case S::PillowEmboss: return -std::clamp(-t, 0.0f, 1.0f);
    }
    return 0.0f;
}

// BEVEL & EMBOSS (doc §5.5) -- shade the shape with a raked light over a height field derived from the
// alpha's signed-distance transform. h = bevelHeight(SDF(A), size, style); the surface normal comes
// from a SINGLE-PASS 3x3 Sobel of h (HARD RULE: single-pass ONLY -- never an iterative or pyramid
// height->normal estimation, however much smoother it would look) as the normal of z=h(x,y):
//   n = normalize(-depth*gx, -depth*gy, 1),  gx,gy the Sobel gradients, `depth` steepening the slope.
// A Blinn/Lambert raked light L at azimuth `angle`, elevation `altitude` gives the response n.L;
// measured against the flat-surface response (Lz), a POSITIVE deviation paints the highlight colour, a
// NEGATIVE one the shadow colour, each scaled by its opacity and CLIPPED to the silhouette. v1 is a
// shading modulation, like an overlay (see the OuterBevel exterior-lip note in bevelHeight).
// `ax`/`ay` anchor the dither below to the geometric silhouette (the `anchor` box): a key that is
// identical between a full composite and the footprint path, so region == full stays byte-exact,
// and that travels WITH the shape instead of crawling when the layer moves.
void applyBevel(ImageF& io, const core::BevelEffect& bv, const std::vector<float>& alpha,
                const std::vector<float>& sd, int rx0, int ry0, int rw, int rh, int ax, int ay) {
    if (!bv.enabled || bv.size <= 0.0f) return;
    // Height field from the alpha's SDF (px, negative inside), per style. The ANTI-ALIASED field
    // (3x supersampled, the stroke renderer's fix) is load-bearing here: the plain binary EDT is
    // faceted along the Voronoi creases of the discrete boundary samples on any angled or curved
    // edge, and the Sobel turns those facets into disconnected highlight/shadow BLOCKS (user
    // 2026-07-16: "rendered in blocks that do not connect -- most visible on 3D text and
    // circles"). The AA field's sub-pixel 0.5 crossing keeps the gradient direction continuous.
    std::vector<float> hgt(static_cast<std::size_t>(rw) * rh);
    for (std::size_t i = 0; i < hgt.size(); ++i) hgt[i] = bevelHeight(sd[i], bv.size, bv.style);
    // `soften` blurs the height field before the Sobel -> softer normals (still a single-pass
    // Sobel). A small floor is always on (the classic "smooth technique"): even the AA field
    // keeps a sub-texel ripple that the Sobel amplifies into shade wobble along angled edges;
    // half-pixel smoothing of the FIELD flattens it without touching how normals are derived.
    bevelSatinBlur(hgt, rw, rh, std::max(1.5f, bv.soften));

    // Raked light direction (azimuth `angle`, elevation `altitude`); screen-y down. altitude 90deg is
    // straight overhead (Lz=1, a flat surface reads neutral); 0deg grazes along the surface plane.
    const float az = bv.angleDeg * kDegToRad, alt = bv.altitudeDeg * kDegToRad;
    const float cAlt = std::cos(alt);
    const float Lx = cAlt * std::cos(az), Ly = -cAlt * std::sin(az), Lz = std::sin(alt);
    const float base = std::clamp(Lz, 0.0f, 1.0f);  // flat-surface (n=(0,0,1)) response == Lz

    const auto H = [&](int x, int y) -> float {  // clamp-extended height fetch (edge pixels)
        x = std::clamp(x, 0, rw - 1);
        y = std::clamp(y, 0, rh - 1);
        return hgt[static_cast<std::size_t>(y) * rw + x];
    };
    forEachRow(rh, [&](int y) {
        for (int x = 0; x < rw; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * rw + x;
            const float cov = alpha[idx];
            if (cov <= 0.0f)
                continue; // clip to the silhouette
            // SINGLE-PASS 3x3 Sobel of the height field (the ONLY normal-from-height allowed here).
            const float gx = (H(x + 1, y - 1) + 2.0f * H(x + 1, y) + H(x + 1, y + 1)) -
                             (H(x - 1, y - 1) + 2.0f * H(x - 1, y) + H(x - 1, y + 1));
            const float gy = (H(x - 1, y + 1) + 2.0f * H(x, y + 1) + H(x + 1, y + 1)) -
                             (H(x - 1, y - 1) + 2.0f * H(x, y - 1) + H(x + 1, y - 1));
            // Surface normal of z=h: normalize(-dh/dx, -dh/dy, 1); `depth` steepens the slope.
            float nx = -gx * bv.depth, ny = -gy * bv.depth, nz = 1.0f;
            const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            nx *= inv;
            ny *= inv;
            nz *= inv;
            const float lambert = std::clamp(nx * Lx + ny * Ly + nz * Lz, 0.0f, 1.0f);
            const float d = lambert - base;  // deviation from the flat-surface response
            if (d == 0.0f)
                continue; // plateau (gradient 0) -> neutral, leave the pixel be
            ColorF src;
            if (d > 0.0f) {  // tilted toward the light -> highlight
                src = bv.highlight;
                src.a = bv.highlight.a * std::clamp(d, 0.0f, 1.0f) * bv.highlightOpacity;
            } else {  // tilted away -> shadow
                src = bv.shadow;
                src.a = bv.shadow.a * std::clamp(-d, 0.0f, 1.0f) * bv.shadowOpacity;
            }
            if (src.a <= 0.0f)
                continue;
            const auto px = static_cast<std::uint32_t>(rx0 + x);
            const auto py = static_cast<std::uint32_t>(ry0 + y);
            // The strength is folded into src.a; composite Normal (the model carries no bevel blend).
            ColorF res = compositeOver(core::BlendMode::Normal, io.at(px, py), src, 1.0f);
            // ~1 LSB of TPDF dither before the eventual 8-bit quantisation: the shade ramp is a
            // shallow gradient, and its 1/255 steps otherwise band into visible iso-shade LINES
            // across the bevel (user 2026-07-16) -- the sky renderer's exact fix. Keys are
            // silhouette-anchored (see the function note) so patches and moves stay coherent.
            constexpr float kLsb = 1.0f / 255.0f;
            const auto dx = static_cast<std::uint32_t>(rx0 + x - ax);
            const auto dy = static_cast<std::uint32_t>(ry0 + y - ay);
            res.r = std::clamp(
                res.r + static_cast<float>(common::ditherTPDF(dx, dy, 0)) * kLsb, 0.0f, 1.0f);
            res.g = std::clamp(
                res.g + static_cast<float>(common::ditherTPDF(dx, dy, 1)) * kLsb, 0.0f, 1.0f);
            res.b = std::clamp(
                res.b + static_cast<float>(common::ditherTPDF(dx, dy, 2)) * kLsb, 0.0f, 1.0f);
            res.a = std::min(res.a, cov);  // never paint outside the silhouette
            io.set(px, py, res);
        }
    });
}

}  // namespace

void applyEffects(ImageF& io, const core::LayerEffects& fx, bool antialias,
                  const std::optional<common::Affine2D>& bufferToLayer) {
    if (fx.empty() || io.empty()) return;
    const int w = static_cast<int>(io.width), h = static_cast<int>(io.height);

    // Every effect attaches to the layer's coverage; nothing to do without any. One pass yields
    // both the extent and the phase anchor (see alphaBoxes).
    //
    // Scoped because it is the one pass here that is sized to the BUFFER rather than to the ROI:
    // everything below works inside `content` dilated by the reach, but finding `content` means
    // reading the whole alpha plane first. A group carrying effects over full-canvas children (the
    // S60 fixture's Texture folder) pays it at 39.8 MP.
    AlphaBoxes boxes;
    {
        MOSAIC_PERF_SCOPE("FX coverage scan", common::Lane::Cpu);
        boxes = alphaBoxes(io);
    }
    const Box content = boxes.any; // full extent: drives the ROI + the fill-opacity dim
    if (content.empty()) return;

    // The paint PHASE anchor (gradient box origin / pattern tile origin). Anchoring to `content`
    // (alpha>0) is UNSTABLE: the AA rim adds ~1px, so the box origin -- and thus the pattern's
    // position -- shifts when the Move-AA filter toggles (Nearest<->Auto) or a live drag swaps to a
    // cheaper kernel (the "pattern snaps then snaps back" bug). Anchor instead to the geometric
    // silhouette (alpha>=0.5), which those edge changes leave put. Fall back to content if the whole
    // shape is faint (all < 0.5).
    Box anchor = boxes.solid;
    if (anchor.empty()) anchor = content;

    // Region of interest = content dilated by the max outward reach (+AA margin), clamped to the
    // buffer. We only ever read signed distances <= reach, so the sub-region's border being wrong
    // beyond that is harmless -- and it spares an EDT of the whole (possibly 36 MP) canvas.
    float reach = core::effectsOutwardReach(fx);
    // Inner shadow/glow (LE-e) never paint OUTSIDE the shape (so they don't grow effectsBounds / the
    // buffer), but they SAMPLE the signed-distance / complement field up to this far outside it -- an
    // inner shadow reads the offset complement (distance+size+spread out), an inner glow the blurred
    // complement (size+choke out). The read ROI must be padded to cover that or the near-edge band
    // reads off the ROI as zero (the "inner shadow vanishes" bug). This grows the read window only.
    for (const core::ShadowEffect& sh : fx.innerShadows)
        if (sh.enabled) reach = std::max(reach, sh.distance + sh.size + sh.spread);
    if (fx.innerGlow.enabled && !std::holds_alternative<core::vec::NoPaint>(fx.innerGlow.paint))
        reach = std::max(reach, fx.innerGlow.size + fx.innerGlow.choke);
    const int pad = static_cast<int>(std::ceil(reach)) + 2;
    const int rx0 = std::max(0, content.x0 - pad);
    const int ry0 = std::max(0, content.y0 - pad);
    const int rx1 = std::min(w, content.x1 + pad);
    const int ry1 = std::min(h, content.y1 + pad);
    const int rw = rx1 - rx0, rh = ry1 - ry0;

    // Capture the ORIGINAL coverage before fill-opacity dims it, so effects key off the full
    // shape (a dimmed fill must not shrink the stroke/shadow edge).
    std::vector<float> alpha(static_cast<std::size_t>(rw) * rh);
    {
        MOSAIC_PERF_SCOPE("FX coverage capture", common::Lane::Cpu);
        forEachRow(rh, [&](int y) {
            for (int x = 0; x < rw; ++x)
                alpha[static_cast<std::size_t>(y) * rw + x] =
                    io.at(static_cast<std::uint32_t>(rx0 + x), static_cast<std::uint32_t>(ry0 + y))
                        .a;
        });
    }

    // MID -- the layer's own pixels, dimmed by fill-opacity (straight alpha; colour intact).
    // Overlays / satin / inner shadow+glow / bevel (all clipped to the alpha) land here in LE-c..f.
    if (fx.fillOpacity < 1.0f) {
        const float s = std::clamp(fx.fillOpacity, 0.0f, 1.0f);
        // Bounded by `content`, not the ROI, so it bands over its own row span.
        common::parallelFor(
            static_cast<std::size_t>(content.y1 - content.y0), 16,
            [&](std::size_t b0, std::size_t b1) {
                for (int y = content.y0 + static_cast<int>(b0),
                         yEnd = content.y0 + static_cast<int>(b1);
                     y < yEnd; ++y) {
                    for (int x = content.x0; x < content.x1; ++x) {
                        ColorF c =
                            io.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                        c.a *= s;
                        io.set(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), c);
                    }
                }
            });
    }

    // Shadow/glow tier (LE-e): the four effects all key off ONE ROI signed-distance field of the
    // captured coverage (neg inside), built once and shared. Only pay for it when a shadow/glow is on.
    bool anyDropShadow = false, anyInnerShadow = false;
    for (const core::ShadowEffect& sh : fx.dropShadows)
        if (sh.enabled) anyDropShadow = true;
    for (const core::ShadowEffect& sh : fx.innerShadows)
        if (sh.enabled) anyInnerShadow = true;
    const bool anyOuterGlow =
        fx.outerGlow.enabled && !std::holds_alternative<core::vec::NoPaint>(fx.outerGlow.paint);
    const bool anyInnerGlow =
        fx.innerGlow.enabled && !std::holds_alternative<core::vec::NoPaint>(fx.innerGlow.paint);
    std::vector<float> sdShadow;
    if (anyDropShadow || anyInnerShadow || anyOuterGlow || anyInnerGlow) {
        MOSAIC_PERF_SCOPE("FX signed-distance field", common::Lane::Cpu);
        sdShadow = fx::signedDistanceField(alpha, rw, rh);
    }

    // BELOW -- drop shadows / outer glow composite UNDER `io`: LE-e. Render them into a transparent
    // ROI-sized scratch (drop shadows in vector order, then the outer glow -- later over earlier),
    // then place io's own pixels OVER it so the effect shows through io's transparent + AA-rim areas.
    if (anyDropShadow || anyOuterGlow) {
        std::vector<ColorF> below;
        {
            MOSAIC_PERF_SCOPE("FX below buffer", common::Lane::Cpu);
            below.assign(static_cast<std::size_t>(rw) * rh, ColorF{0.0f, 0.0f, 0.0f, 0.0f});
        }
        {
            MOSAIC_PERF_SCOPE("FX drop shadows", common::Lane::Cpu);
            applyDropShadows(below, fx.dropShadows, sdShadow, rw, rh);
        }
        {
            MOSAIC_PERF_SCOPE("FX outer glow", common::Lane::Cpu);
            applyOuterGlow(below, fx.outerGlow, sdShadow, anchor, rx0, ry0, rw, rh, antialias,
                           bufferToLayer);
        }
        MOSAIC_PERF_SCOPE("FX below composite", common::Lane::Cpu);
        forEachRow(rh, [&](int y) {
            for (int x = 0; x < rw; ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * rw + x;
                if (below[idx].a <= 0.0f)
                    continue; // nothing under this pixel -> io unchanged
                const auto px = static_cast<std::uint32_t>(rx0 + x);
                const auto py = static_cast<std::uint32_t>(ry0 + y);
                io.set(px, py,
                       compositeOver(core::BlendMode::Normal, below[idx], io.at(px, py), 1.0f));
            }
        });
    }

    // MID overlays (doc §5.3): colour -> gradient -> pattern, in the fixed z-order, each clipped to
    // the shape's ORIGINAL coverage and composited with its own blend + opacity on top of the layer's
    // (fill-dimmed) pixels. Independent -- all three can coexist (the PS/Affinity composable model).
    {
        MOSAIC_PERF_SCOPE("FX overlays", common::Lane::Cpu);
        applyOverlay(io, fx.colorOverlay, alpha, anchor, rx0, ry0, rw, rh, antialias,
                     bufferToLayer);
        applyOverlay(io, fx.gradientOverlay, alpha, anchor, rx0, ry0, rw, rh, antialias,
                     bufferToLayer);
        applyOverlay(io, fx.patternOverlay, alpha, anchor, rx0, ry0, rw, rh, antialias,
                     bufferToLayer);
    }

    // SATIN (LE-f) -- after pattern overlay, before inner shadow. The folded-fabric sheen: the shape's
    // coverage interfered with an offset copy of itself, clipped to the silhouette (doc §5.6).
    {
        MOSAIC_PERF_SCOPE("FX satin", common::Lane::Cpu);
        applySatin(io, fx.satin, alpha, rx0, ry0, rw, rh);
    }

    // INNER shadow/glow (LE-e) -- after overlays/satin, before bevel. These composite OVER io,
    // CLIPPED to the captured `alpha` (like the overlays), so they only ever ink inside the silhouette.
    if (anyInnerShadow) {
        MOSAIC_PERF_SCOPE("FX inner shadows", common::Lane::Cpu);
        applyInnerShadows(io, fx.innerShadows, sdShadow, alpha, rx0, ry0, rw, rh);
    }
    if (anyInnerGlow) {
        MOSAIC_PERF_SCOPE("FX inner glow", common::Lane::Cpu);
        applyInnerGlow(io, fx.innerGlow, sdShadow, alpha, anchor, rx0, ry0, rw, rh, antialias,
                       bufferToLayer);
    }

    // ---- The 3x SUPERSAMPLED field, built at most ONCE ---------------------------------------
    //
    // Both remaining tiers want it, and they want the SAME one: `signedDistanceFieldAA(alpha, rw,
    // rh, 3)` is a pure function of arguments neither of them changes. The bevel built its own
    // inside applyBevel and the stroke pass built another, so a layer carrying both -- which is
    // what a styled headline is -- paid for it twice.
    //
    // It is the most expensive single thing in the stack, because the 3x supersample is 9x the
    // texels and `signedDistanceField` runs TWO exact Euclidean transforms over them: on a
    // headline's 3958x1034 ROI that is a pair of EDTs across 36.8 MP, done twice over.
    //
    // Lazy, so a layer with neither tier still builds nothing, and a layer with one builds one.
    std::vector<float> sdAA;
    bool sdAABuilt = false;
    const auto supersampledSdf = [&]() -> const std::vector<float>& {
        if (!sdAABuilt) {
            MOSAIC_PERF_SCOPE("FX signed-distance field (3x)", common::Lane::Cpu);
            sdAA = fx::signedDistanceFieldAA(alpha, rw, rh, 3);
            sdAABuilt = true;
        }
        return sdAA;
    };

    // BEVEL (LE-f) -- after inner glow, before stroke. Shades the silhouette with a raked light over a
    // height field built from the alpha's SDF (single-pass Sobel normals, doc §5.5).
    if (fx.bevel.enabled && fx.bevel.size > 0.0f) {
        MOSAIC_PERF_SCOPE("FX bevel", common::Lane::Cpu);
        applyBevel(io, fx.bevel, alpha, supersampledSdf(), rx0, ry0, rw, rh, anchor.x0, anchor.y0);
    }

    // ABOVE -- concentric strokes on top of everything.
    bool anyStroke = false;
    for (const core::StrokeEffect& st : fx.strokes)
        if (st.enabled && st.width > 0.0f) anyStroke = true;
    if (anyStroke) {
        // 3x supersampled so an outside stroke's outer edge is smooth along the content's own AA
        // (a binary-threshold field would make it bumpy). ROI-bounded, so the cost stays local.
        MOSAIC_PERF_SCOPE("FX strokes", common::Lane::Cpu);
        applyStrokes(io, fx.strokes, supersampledSdf(), alpha, anchor, rx0, ry0, rw, rh, antialias,
                     bufferToLayer);
    }
}

}  // namespace mosaic::render
