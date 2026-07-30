#include "core/document.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

#include "core/command.hpp"
#include "core/vector/flatten.hpp"
#include "core/vector/hit.hpp"

namespace mosaic::core {
namespace {

// Depth-first search of a group's subtree (the group itself is not matched). Returns the layer
// and, via out-params, its parent group and index when found.
Layer* findInGroup(GroupLayer& group, LayerId id, GroupLayer** parentOut, std::size_t* indexOut) {
    const auto& kids = group.children();
    for (std::size_t i = 0; i < kids.size(); ++i) {
        Layer* child = kids[i].get();
        if (child->id() == id) {
            if (parentOut) *parentOut = &group;
            if (indexOut) *indexOut = i;
            return child;
        }
        if (auto* sub = child->as<GroupLayer>()) {
            if (Layer* found = findInGroup(*sub, id, parentOut, indexOut)) return found;
        }
    }
    return nullptr;
}

std::size_t countInGroup(const GroupLayer& group) {
    std::size_t n = 0;
    for (const auto& child : group.children()) {
        ++n;
        if (const auto* sub = child->as<GroupLayer>()) n += countInGroup(*sub);
    }
    return n;
}

}  // namespace

std::string_view colorSpaceName(ColorSpace cs) {
    switch (cs) {
        case ColorSpace::SRGB: return "sRGB";
        case ColorSpace::LinearSRGB: return "Linear sRGB";
        case ColorSpace::DisplayP3: return "Display P3";
        case ColorSpace::AdobeRGB: return "Adobe RGB";
        case ColorSpace::Rec2020: return "Rec. 2020";
    }
    return "sRGB";
}

std::string_view precisionName(Precision p) {
    switch (p) {
        case Precision::U8: return "8-bit integer";
        case Precision::U16: return "16-bit integer";
        case Precision::F16: return "16-bit float";
        case Precision::F32: return "32-bit float";
    }
    return "16-bit float";
}

Document::Document(std::uint32_t width, std::uint32_t height, ColorSpace colorSpace,
                   Precision precision)
    : m_width(width), m_height(height), m_colorSpace(colorSpace), m_precision(precision) {
    m_root = std::make_unique<GroupLayer>(mintLayerId(), "Root");
    m_commands = std::make_unique<CommandStack>(*this);
}

Document::~Document() = default;

bool Document::dirty() const noexcept { return !m_commands->isSaved(); }

Layer* Document::find(LayerId id) noexcept {
    return findInGroup(*m_root, id, nullptr, nullptr);
}

const Layer* Document::find(LayerId id) const noexcept {
    // Reuse the non-const search on a non-const view of the tree; the result is re-constified.
    return findInGroup(*m_root, id, nullptr, nullptr);
}

std::optional<Document::Location> Document::locate(LayerId id) noexcept {
    GroupLayer* parent = nullptr;
    std::size_t index = 0;
    if (findInGroup(*m_root, id, &parent, &index) != nullptr) {
        return Location{parent, index};
    }
    return std::nullopt;
}

GroupLayer* Document::groupById(LayerId id) noexcept {
    if (id == m_root->id()) return m_root.get();
    Layer* layer = find(id);
    return layer ? layer->as<GroupLayer>() : nullptr;
}

std::size_t Document::layerCount() const noexcept {
    return countInGroup(*m_root);
}

std::unique_ptr<GroupLayer> Document::makeGroup(std::string name) {
    return std::make_unique<GroupLayer>(mintLayerId(), std::move(name));
}

std::unique_ptr<RasterLayer> Document::makeRaster(std::string name, std::uint32_t w,
                                                  std::uint32_t h) {
    return std::make_unique<RasterLayer>(mintLayerId(), std::move(name), w, h);
}

std::unique_ptr<RasterLayer> Document::makeRaster(std::string name) {
    return makeRaster(std::move(name), m_width, m_height);
}

std::unique_ptr<VectorLayer> Document::makeVector(std::string name) {
    return std::make_unique<VectorLayer>(mintLayerId(), std::move(name));
}

std::unique_ptr<TextLayer> Document::makeText(std::string name, std::string text) {
    return std::make_unique<TextLayer>(mintLayerId(), std::move(name), std::move(text));
}

std::unique_ptr<AdjustmentLayer> Document::makeAdjustment(std::string name, AdjustmentKind kind) {
    return std::make_unique<AdjustmentLayer>(mintLayerId(), std::move(name), kind);
}

std::unique_ptr<MagicLayer> Document::makeMagic(std::string name, common::Image source) {
    return std::make_unique<MagicLayer>(mintLayerId(), std::move(name), std::move(source));
}

std::unique_ptr<TextureLayer> Document::makeTexture(std::string name,
                                                    texture::TextureParams params) {
    return std::make_unique<TextureLayer>(mintLayerId(), std::move(name), std::move(params));
}

std::unique_ptr<Layer> Document::duplicateLayer(const Layer& src) {
    std::unique_ptr<Layer> copy;
    switch (src.kind()) {
        case LayerKind::Group: {
            const auto& srcGroup = static_cast<const GroupLayer&>(src);
            auto group = makeGroup(src.name());
            for (const auto& child : srcGroup.children()) {
                group->addOnTop(duplicateLayer(*child));  // recurse: fresh ids throughout
            }
            group->setExpanded(srcGroup.expanded());
            copy = std::move(group);
            break;
        }
        case LayerKind::Raster: {
            const auto& srcRaster = static_cast<const RasterLayer&>(src);
            auto raster = makeRaster(src.name(), srcRaster.image().width, srcRaster.image().height);
            raster->image() = srcRaster.image();  // copy pixels
            copy = std::move(raster);
            break;
        }
        case LayerKind::Vector:
            copy = makeVector(src.name());
            break;
        case LayerKind::Text: {
            const auto& srcText = static_cast<const TextLayer&>(src);
            auto text = makeText(src.name());
            text->setBlock(srcText.block());  // copy the full rich block (runs/paragraphs/frame/AA)
            copy = std::move(text);
            break;
        }
        case LayerKind::Adjustment: {
            const auto& srcAdj = static_cast<const AdjustmentLayer&>(src);
            auto adj = makeAdjustment(src.name(), srcAdj.adjustmentKind());
            adj->params() = srcAdj.params();
            copy = std::move(adj);
            break;
        }
        case LayerKind::Magic: {
            const auto& srcMagic = static_cast<const MagicLayer&>(src);
            copy = makeMagic(src.name(), srcMagic.source());
            break;
        }
        case LayerKind::Texture: {
            // The params ARE the content; the pixel cache regenerates on the copy's first
            // composite (same params + seed => the same pixels, §8.3 determinism).
            const auto& srcTex = static_cast<const TextureLayer&>(src);
            copy = makeTexture(src.name(), srcTex.params());
            break;
        }
    }

    // Shared chrome (factories already set the name).
    copy->setVisible(src.visible());
    copy->setOpacity(src.opacity());
    copy->setBlendMode(src.blendMode());
    copy->setClipToBelow(src.clipToBelow());
    copy->setLocked(src.locked());
    copy->setPastedMarker(src.pastedMarker()); // a copy of an unorganized paste is one too
    copy->setTransform(src.transform());
    if (src.hasMask()) {
        copy->setMask(*src.mask());
    }
    return copy;
}

namespace {

// `parentT` accumulates the ancestor transforms down the walk, so a leaf inside transformed
// groups is sampled where the compositor actually shows it.
Layer* topmostLayerAtImpl(GroupLayer& group, common::Vec2 docPt,
                          const common::Affine2D& parentT) {
    for (std::size_t i = group.childCount(); i-- > 0;) {
        Layer& child = group.child(i);
        if (!child.visible())
            continue;
        if (auto* sub = child.as<GroupLayer>()) {
            if (Layer* hit = topmostLayerAtImpl(*sub, docPt, parentT * sub->transform()))
                return hit;
            continue;
        }
        const std::optional<common::Affine2D> inv = (parentT * child.transform()).inverse();
        if (!inv)
            continue; // singular: shows nothing, hits nothing
        // A vector layer is hit by its geometry (so the Move tool can select + move shapes), not by
        // sampling pixels -- it has none.
        if (const auto* vl = child.as<VectorLayer>()) {
            if (vl->hasObject() && vec::hitTest(*vl->object(), inv->apply(docPt)))
                return &child;
            continue;
        }
        // A text layer is hit by its laid-out content box (so Move can select + transform it, like a
        // vector by geometry) -- it has no pixels to sample. The box is nullopt until the renderer has
        // measured it (an unrendered / empty block hits nothing).
        if (const auto* tl = child.as<TextLayer>()) {
            const std::optional<common::Rect> box = tl->contentBounds();
            if (box && box->contains(inv->apply(docPt)))
                return &child;
            continue;
        }
        // A texture layer is parametric like text: Move selects + transforms it as a unit, so it
        // hits by its rendered extent (nullopt until the first refresh -- hits nothing).
        if (const auto* xl = child.as<TextureLayer>()) {
            const std::optional<common::Rect> box = xl->contentBounds();
            if (box && box->contains(inv->apply(docPt)))
                return &child;
            continue;
        }
        const common::Image* src = nullptr;
        if (const auto* raster = child.as<RasterLayer>())
            src = &raster->image();
        else if (const auto* magic = child.as<MagicLayer>())
            src = &magic->source();
        if (src == nullptr || src->empty())
            continue;
        const common::Vec2 p = inv->apply(docPt);
        const long sx = static_cast<long>(std::floor(p.x));
        const long sy = static_cast<long>(std::floor(p.y));
        if (sx < 0 || sy < 0 || sx >= static_cast<long>(src->width) ||
            sy >= static_cast<long>(src->height))
            continue;
        if (src->rgba[(static_cast<std::size_t>(sy) * src->width + sx) * 4 + 3] > 0)
            return &child;
    }
    return nullptr;
}

}  // namespace

Layer* topmostLayerAt(GroupLayer& root, common::Vec2 docPt) {
    return topmostLayerAtImpl(root, docPt, root.transform());
}

namespace {
VectorLayer* topmostVectorLayerAtImpl(GroupLayer& group, common::Vec2 docPt,
                                      const common::Affine2D& parentT, double pickRadiusDoc) {
    for (std::size_t i = group.childCount(); i-- > 0;) {
        Layer& child = group.child(i);
        if (!child.visible())
            continue;
        const common::Affine2D world = parentT * child.transform();
        if (auto* sub = child.as<GroupLayer>()) {
            if (VectorLayer* hit = topmostVectorLayerAtImpl(*sub, docPt, world, pickRadiusDoc))
                return hit;
            continue;
        }
        auto* vl = child.as<VectorLayer>();
        if (vl == nullptr || !vl->hasObject())
            continue;
        const std::optional<common::Affine2D> inv = world.inverse();
        if (!inv)
            continue; // singular: shows nothing, hits nothing
        const common::Vec2 pLocal = inv->apply(docPt);
        const double pickLocal = inv->applyVector({pickRadiusDoc, 0}).length(); // doc px -> local
        if (vec::hitTest(*vl->object(), pLocal, pickLocal))
            return vl;
    }
    return nullptr;
}
}  // namespace

VectorLayer* topmostVectorLayerAt(GroupLayer& root, common::Vec2 docPt, double pickRadiusDoc) {
    return topmostVectorLayerAtImpl(root, docPt, root.transform(), pickRadiusDoc);
}

namespace {
TextLayer* topmostTextLayerAtImpl(GroupLayer& group, common::Vec2 docPt,
                                  const common::Affine2D& parentT, double padDoc) {
    for (std::size_t i = group.childCount(); i-- > 0;) {
        Layer& child = group.child(i);
        if (!child.visible())
            continue;
        const common::Affine2D world = parentT * child.transform();
        if (auto* sub = child.as<GroupLayer>()) {
            if (TextLayer* hit = topmostTextLayerAtImpl(*sub, docPt, world, padDoc))
                return hit;
            continue;
        }
        auto* tl = child.as<TextLayer>();
        const std::optional<common::Rect> box = tl != nullptr ? tl->contentBounds() : std::nullopt;
        if (!box)
            continue; // not a text layer, or its box has not been measured (empty / unrendered)
        const std::optional<common::Affine2D> inv = world.inverse();
        if (!inv)
            continue;
        const common::Vec2 pLocal = inv->apply(docPt);
        const double padLocal = inv->applyVector({padDoc, 0}).length(); // doc px -> local
        const common::Rect padded{box->x - padLocal, box->y - padLocal, box->w + 2 * padLocal,
                                  box->h + 2 * padLocal};
        if (padded.contains(pLocal))
            return tl;
    }
    return nullptr;
}
}  // namespace

TextLayer* topmostTextLayerAt(GroupLayer& root, common::Vec2 docPt, double padDoc) {
    return topmostTextLayerAtImpl(root, docPt, root.transform(), padDoc);
}

namespace {
VectorLayer* topmostVectorSpineAtImpl(GroupLayer& group, common::Vec2 docPt,
                                      const common::Affine2D& parentT, double padDoc) {
    for (std::size_t i = group.childCount(); i-- > 0;) {
        Layer& child = group.child(i);
        if (!child.visible())
            continue;
        const common::Affine2D world = parentT * child.transform();
        if (auto* sub = child.as<GroupLayer>()) {
            if (VectorLayer* hit = topmostVectorSpineAtImpl(*sub, docPt, world, padDoc))
                return hit;
            continue;
        }
        auto* vl = child.as<VectorLayer>();
        if (vl == nullptr || !vl->hasObject())
            continue;
        const std::optional<common::Affine2D> inv = world.inverse();
        if (!inv)
            continue;
        const common::Vec2 pLocal = inv->apply(docPt);
        const double padLocal = inv->applyVector({padDoc, 0}).length(); // doc px -> local
        double dist = 0.0;
        const vec::Contours cs = vec::flatten(vl->object()->geometry);
        (void)vec::nearestArcDistance(cs, pLocal, &dist);
        if (!cs.empty() && dist <= padLocal)
            return vl;
    }
    return nullptr;
}
}  // namespace

VectorLayer* topmostVectorSpineAt(GroupLayer& root, common::Vec2 docPt, double padDoc) {
    return topmostVectorSpineAtImpl(root, docPt, root.transform(), padDoc);
}

bool rebakeTextPathFit(Document& doc, TextLayer& tl) {
    text::TextBlock& block = tl.mutableBlock(); // only the DERIVED baked contours are touched
    if (!block.pathFit || block.pathFit->layer == kInvalidLayerId)
        return false;
    Layer* src = doc.find(static_cast<LayerId>(block.pathFit->layer));
    auto* vl = src != nullptr ? src->as<VectorLayer>() : nullptr;
    if (vl == nullptr || !vl->hasObject())
        return false; // source gone: keep the last baked path (the text holds its shape)
    const std::optional<common::Affine2D> textInv = worldTransform(tl).inverse();
    if (!textInv)
        return false;
    const common::Affine2D map = *textInv * worldTransform(*vl); // path-local -> text-local
    vec::Contours cs = vec::flatten(vl->object()->geometry);
    for (vec::Contour& c : cs)
        for (common::Vec2& p : c.points)
            p = map.apply(p);
    if (cs == block.pathFit->baked)
        return false;
    block.pathFit->baked = std::move(cs);
    return true;
}

Layer* moveClickTarget(Layer* hit, Layer* current) {
    if (hit == nullptr)
        return nullptr;
    // The hit's ancestor chain, hit-first, up to (excluding) the document root.
    std::vector<Layer*> chain;
    for (Layer* n = hit; n != nullptr && n->parent() != nullptr; n = n->parent())
        chain.push_back(n);
    if (chain.empty())
        return hit; // the root itself was passed: nothing sensible to resolve
    if (current != nullptr) {
        // Drill: the current target is the hit itself or one of its ancestors -> one level
        // deeper toward the hit (clicking an already-selected group enters it).
        for (std::size_t i = 0; i < chain.size(); ++i)
            if (chain[i] == current)
                return i == 0 ? chain[0] : chain[i - 1];
        // Scope: the click lands inside a group the current target also lives in -> select
        // the hit's node at that depth (sibling selection inside an "entered" group).
        for (Layer* scope = current->parent(); scope != nullptr; scope = scope->parent())
            for (std::size_t i = 1; i < chain.size(); ++i)
                if (chain[i] == scope)
                    return chain[i - 1];
    }
    return chain.back(); // outermost group containing the hit (the hit itself when ungrouped)
}


common::Affine2D placedImageTransform(std::uint32_t srcW, std::uint32_t srcH, std::uint32_t docW,
                                      std::uint32_t docH) {
    if (srcW == 0 || srcH == 0 || docW == 0 || docH == 0)
        return common::Affine2D::identity();
    const double sw = static_cast<double>(srcW);
    const double sh = static_cast<double>(srcH);
    const double dw = static_cast<double>(docW);
    const double dh = static_cast<double>(docH);
    const double scale = std::min({1.0, dw / sw, dh / sh}); // shrink to fit; never magnify
    // Scale about the source origin, then centre the scaled box in the document.
    return common::Affine2D::translation((dw - sw * scale) * 0.5, (dh - sh * scale) * 0.5) *
           common::Affine2D::scaling(scale, scale);
}

}  // namespace mosaic::core
