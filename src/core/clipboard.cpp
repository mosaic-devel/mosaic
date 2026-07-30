#include "core/clipboard.hpp"

#include "core/layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace mosaic::core {
namespace {

// The pixel source a layer offers the clipboard, if any (mirrors selectionFromLayerPixels).
const common::Image* pixelSource(const Layer& layer) {
    if (const auto* raster = layer.as<RasterLayer>())
        return &raster->image();
    if (const auto* magic = layer.as<MagicLayer>())
        return &magic->source();
    return nullptr;
}

// The fragment's share of an 8-bit alpha under coverage `cov`, rounded to nearest. The residual
// takes `a - fragmentAlpha(a, cov)`, so the two halves sum back to `a` EXACTLY -- both used to
// truncate independently and under-summed by up to 2/255 before `over` ever got involved. The
// disjoint compositor reconstructs whatever the split hands it, so this is what makes cut + paste
// in place bit-exact rather than merely seamless.
[[nodiscard]] std::uint8_t fragmentAlpha(std::uint8_t a, int cov) noexcept {
    return static_cast<std::uint8_t>((a * cov + 127) / 255);
}

struct Region {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0; // exclusive
    int y1 = 0;
    [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
};

// The document-space pixel region a copy walks: the selection's tight bounds, or the layer's
// transformed extent when the selection is empty; always clamped to the document. `world` is
// the layer's full local->document transform (ancestors composed).
Region copyRegion(const common::Affine2D& world, const common::Image& src, const Selection& sel,
                  std::uint32_t docW, std::uint32_t docH) {
    common::Rect r;
    if (!sel.isEmpty()) {
        const std::optional<common::Rect> b = sel.bounds();
        if (!b)
            return {};
        r = *b;
    } else {
        r = world.mapBounds(
            {0.0, 0.0, static_cast<double>(src.width), static_cast<double>(src.height)});
    }
    Region out;
    out.x0 = std::clamp(static_cast<int>(std::floor(r.x)), 0, static_cast<int>(docW));
    out.y0 = std::clamp(static_cast<int>(std::floor(r.y)), 0, static_cast<int>(docH));
    out.x1 = std::clamp(static_cast<int>(std::ceil(r.right())), 0, static_cast<int>(docW));
    out.y1 = std::clamp(static_cast<int>(std::ceil(r.bottom())), 0, static_cast<int>(docH));
    return out;
}

} // namespace

std::optional<ClipboardContent> copyFromLayer(const Layer& layer, const Selection& sel,
                                              std::uint32_t docW, std::uint32_t docH) {
    const common::Image* src = pixelSource(layer);
    if (src == nullptr || src->empty())
        return std::nullopt; // groups/vector/text own no pixels to copy
    const common::Affine2D t = worldTransform(layer); // ancestors compose (transformed groups)
    const Region reg = copyRegion(t, *src, sel, docW, docH);
    if (reg.empty())
        return std::nullopt;

    const bool identity = t == common::Affine2D::identity();
    std::optional<common::Affine2D> inv;
    if (!identity) {
        inv = t.inverse();
        if (!inv)
            return std::nullopt; // a singular transform shows nothing (compositor semantics)
    }

    ClipboardContent out;
    out.docX = reg.x0;
    out.docY = reg.y0;
    out.sourceName = layer.name();
    if (sel.isEmpty()) // a whole-layer copy carries the layer's style (paste restores it)
        out.style = ClipboardContent::LayerStyle{layer.name(), layer.opacity(), layer.blendMode()};
    out.image = common::Image(static_cast<std::uint32_t>(reg.x1 - reg.x0),
                              static_cast<std::uint32_t>(reg.y1 - reg.y0));
    bool any = false;
    for (int y = reg.y0; y < reg.y1; ++y) {
        for (int x = reg.x0; x < reg.x1; ++x) {
            long sx = x;
            long sy = y;
            if (!identity) {
                const common::Vec2 p = inv->apply({x + 0.5, y + 0.5});
                sx = static_cast<long>(std::floor(p.x));
                sy = static_cast<long>(std::floor(p.y));
            }
            if (sx < 0 || sy < 0 || sx >= static_cast<long>(src->width) ||
                sy >= static_cast<long>(src->height))
                continue;
            const std::size_t sp = (static_cast<std::size_t>(sy) * src->width + sx) * 4;
            const int cov =
                sel.isEmpty() ? 255
                              : sel.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            const std::uint8_t a = fragmentAlpha(src->rgba[sp + 3], cov);
            const std::size_t dp =
                (static_cast<std::size_t>(y - reg.y0) * out.image.width + (x - reg.x0)) * 4;
            out.image.rgba[dp + 0] = src->rgba[sp + 0];
            out.image.rgba[dp + 1] = src->rgba[sp + 1];
            out.image.rgba[dp + 2] = src->rgba[sp + 2];
            out.image.rgba[dp + 3] = a;
            any = any || a > 0;
        }
    }
    if (!any)
        return std::nullopt; // nothing visible under the selection
    return out;
}

std::optional<ClipboardContent> copyMerged(const common::Image& composite, const Selection& sel) {
    if (composite.empty())
        return std::nullopt;
    int x0 = 0;
    int y0 = 0;
    int x1 = static_cast<int>(composite.width);
    int y1 = static_cast<int>(composite.height);
    if (!sel.isEmpty()) {
        const std::optional<common::Rect> b = sel.bounds();
        if (!b)
            return std::nullopt; // an active selection covering nothing
        x0 = std::max(0, static_cast<int>(std::floor(b->x)));
        y0 = std::max(0, static_cast<int>(std::floor(b->y)));
        x1 = std::min(x1, static_cast<int>(std::ceil(b->right())));
        y1 = std::min(y1, static_cast<int>(std::ceil(b->bottom())));
        if (x1 <= x0 || y1 <= y0)
            return std::nullopt;
    }
    ClipboardContent out;
    out.docX = x0;
    out.docY = y0;
    out.image =
        common::Image(static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t sp = (static_cast<std::size_t>(y) * composite.width + x) * 4;
            const std::size_t dp =
                (static_cast<std::size_t>(y - y0) * out.image.width + (x - x0)) * 4;
            const int cov =
                sel.isEmpty() ? 255
                              : sel.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            out.image.rgba[dp + 0] = composite.rgba[sp + 0];
            out.image.rgba[dp + 1] = composite.rgba[sp + 1];
            out.image.rgba[dp + 2] = composite.rgba[sp + 2];
            out.image.rgba[dp + 3] = static_cast<std::uint8_t>(composite.rgba[sp + 3] * cov / 255);
        }
    }
    return out;
}

std::optional<common::Image> imageWithSelectionCleared(const Layer& layer, const Selection& sel) {
    const auto* raster = layer.as<RasterLayer>();
    if (raster == nullptr || raster->image().empty())
        return std::nullopt; // only raster layers are destructively editable
    const common::Image& src = raster->image();
    const common::Affine2D t = worldTransform(layer); // ancestors compose (transformed groups)
    const bool identity = t == common::Affine2D::identity();

    common::Image out = src;
    bool changed = false;
    for (std::uint32_t y = 0; y < src.height; ++y) {
        for (std::uint32_t x = 0; x < src.width; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * src.width + x) * 4;
            if (src.rgba[p + 3] == 0)
                continue; // already clear
            int cov = 255; // empty selection: cut clears the whole layer
            if (!sel.isEmpty()) {
                common::Vec2 d{x + 0.5, y + 0.5};
                if (!identity)
                    d = t.apply(d); // the layer pixel's document position
                const long dx = static_cast<long>(std::floor(d.x));
                const long dy = static_cast<long>(std::floor(d.y));
                cov = (dx < 0 || dy < 0) ? 0
                                         : sel.at(static_cast<std::uint32_t>(dx),
                                                  static_cast<std::uint32_t>(dy));
            }
            if (cov == 0)
                continue;
            // The COMPLEMENT of what copyFromLayer lifted, not an independent (255-cov) scaling:
            // the pair must sum to the original alpha exactly for the two halves to tile.
            out.rgba[p + 3] =
                static_cast<std::uint8_t>(src.rgba[p + 3] - fragmentAlpha(src.rgba[p + 3], cov));
            changed = true;
        }
    }
    if (!changed)
        return std::nullopt; // the selection misses the layer: not worth an undo step
    return out;
}

bool partitionEligibleSource(const Layer& layer) {
    // The cut reads and rewrites the layer's raw image; a mask or an effect would sit between that
    // and the composited alpha, so the halves would no longer be the surface the split assumed.
    if (layer.as<RasterLayer>() == nullptr) return false;
    if (layer.hasMask() || (layer.hasEffects() && !layer.effects().empty())) return false;
    return integerGridPlacement(layer);
}

std::pair<int, int> pastePosition(std::uint32_t contentW, std::uint32_t contentH,
                                  std::optional<std::pair<int, int>> source, std::uint32_t docW,
                                  std::uint32_t docH) {
    if (source)
        return *source;
    return {(static_cast<int>(docW) - static_cast<int>(contentW)) / 2,
            (static_cast<int>(docH) - static_cast<int>(contentH)) / 2};
}

common::Image flattenedOverWhite(const common::Image& img) {
    common::Image out(img.width, img.height);
    for (std::size_t p = 0; p + 3 < img.rgba.size(); p += 4) {
        const int a = img.rgba[p + 3];
        out.rgba[p + 0] = static_cast<std::uint8_t>((img.rgba[p + 0] * a + 255 * (255 - a)) / 255);
        out.rgba[p + 1] = static_cast<std::uint8_t>((img.rgba[p + 1] * a + 255 * (255 - a)) / 255);
        out.rgba[p + 2] = static_cast<std::uint8_t>((img.rgba[p + 2] * a + 255 * (255 - a)) / 255);
        out.rgba[p + 3] = 255;
    }
    return out;
}

} // namespace mosaic::core
