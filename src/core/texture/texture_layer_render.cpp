#include "core/texture/texture_layer_render.hpp"

#include <optional>

#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/texture/texture_render.hpp"

namespace mosaic::core::texture {

namespace {

// The DOCUMENT-space extent of the layer's cached pixels (transform chain composed), padded 2 px
// for the compositor's resample footprint -- the text pass's dirty-rect contract.
common::Rect cachedDocBounds(const TextureLayer& t) {
    std::uint32_t cw = 0, ch = 0;
    if (const auto* img = t.cachedImage()) {
        cw = img->width;
        ch = img->height;
    } else if (const auto* imgF = t.cachedImageF()) {
        cw = imgF->width;
        ch = imgF->height;
    }
    if (cw == 0 || ch == 0) return {};
    const common::Rect local = t.cacheImageToLayer().mapBounds(
        {0.0, 0.0, static_cast<double>(cw), static_cast<double>(ch)});
    const common::Rect doc = worldTransform(t).mapBounds(local);
    return {doc.x - 2.0, doc.y - 2.0, doc.w + 4.0, doc.h + 4.0};
}

// Whichever cache lane is populated must match the document size -- a canvas resize (crop,
// Image > Canvas Size) re-renders even though the params revision never moved.
bool cacheMatchesSize(const TextureLayer& t, std::uint32_t w, std::uint32_t h) {
    if (const auto* img = t.cachedImage()) return img->width == w && img->height == h;
    if (const auto* imgF = t.cachedImageF()) return imgF->width == w && imgF->height == h;
    return false;
}

void refreshGroup(GroupLayer& group, std::uint32_t docW, std::uint32_t docH, bool& any,
                  common::Rect* dirty) {
    const auto accumulate = [&](const common::Rect& r) {
        if (dirty == nullptr || r.empty()) return;
        *dirty = dirty->empty() ? r : dirty->united(r);
    };
    for (const auto& child : group.children()) {
        if (auto* g = child->as<GroupLayer>()) {
            refreshGroup(*g, docW, docH, any, dirty);
        } else if (auto* t = child->as<TextureLayer>()) {
            // Read the OLD pixels' extent BEFORE the refresh replaces them: both the vacated
            // area and the newly-covered one need repainting.
            const common::Rect before = dirty != nullptr ? cachedDocBounds(*t) : common::Rect{};
            if (refreshTextureCache(*t, docW, docH)) {
                any = true;
                accumulate(before);
                accumulate(cachedDocBounds(*t));
            }
        }
    }
}

}  // namespace

void applyBakedTextureCache(TextureLayer& layer, TextureRenderResult baked, std::uint32_t docW,
                            std::uint32_t docH) {
    const bool rendered = baked.image8.has_value() || baked.imageF.has_value();
    layer.setCachedImage(std::move(baked.image8), std::move(baked.imageF),
                         common::Affine2D::identity());
    layer.setCachedContentBounds(
        rendered ? std::optional<common::Rect>(common::Rect{0.0, 0.0, static_cast<double>(docW),
                                                            static_cast<double>(docH)})
                 : std::nullopt);
}

bool refreshTextureCache(TextureLayer& layer, std::uint32_t docW, std::uint32_t docH) {
    if (layer.cacheCurrent() && cacheMatchesSize(layer, docW, docH))
        return false;  // already reflects these params at this document size
    applyBakedTextureCache(layer, renderTexture(layer.params(), docW, docH), docW, docH);
    return true;
}

bool refreshTextureCaches(Document& doc, common::Rect* dirtyDocOut) {
    bool any = false;
    if (dirtyDocOut != nullptr) *dirtyDocOut = {};
    refreshGroup(doc.root(), doc.width(), doc.height(), any, dirtyDocOut);
    return any;
}

}  // namespace mosaic::core::texture
