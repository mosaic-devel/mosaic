#include "core/commands.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/document.hpp"

namespace mosaic::core {

// ---- CompositeCommand -----------------------------------------------------------------------
void CompositeCommand::apply(Document& doc) {
    for (auto& child : m_children) child->apply(doc);
}

void CompositeCommand::undo(Document& doc) {
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) (*it)->undo(doc);
}

// ---- LoadedStateCommand ---------------------------------------------------------------------
void LoadedStateCommand::swapTree(Document& doc) {
    GroupLayer& root = doc.root();
    // Take the document's current tree out, preserving bottom->top order.
    std::vector<std::unique_ptr<Layer>> current;
    current.reserve(root.childCount());
    while (root.childCount() > 0)
        current.insert(current.begin(), root.removeAt(root.childCount() - 1));
    // Put our held snapshot in (same order), and keep what we removed for the inverse call.
    for (auto& layer : m_layers)
        root.addOnTop(std::move(layer));
    m_layers = std::move(current);
}

// ---- AddLayerCommand ------------------------------------------------------------------------
AddLayerCommand::AddLayerCommand(LayerId parentId, std::size_t index, std::unique_ptr<Layer> layer)
    : m_parentId(parentId), m_index(index),
      m_layerId(layer ? layer->id() : kInvalidLayerId), m_held(std::move(layer)) {}

void AddLayerCommand::apply(Document& doc) {
    GroupLayer* parent = doc.groupById(m_parentId);
    if (parent == nullptr || m_held == nullptr) return;
    parent->insert(m_index, std::move(m_held));  // m_held becomes null
}

void AddLayerCommand::undo(Document& doc) {
    GroupLayer* parent = doc.groupById(m_parentId);
    if (parent == nullptr) return;
    m_held = parent->removeAt(m_index);  // take ownership back for a possible redo
}

// ---- RemoveLayerCommand ---------------------------------------------------------------------
void RemoveLayerCommand::apply(Document& doc) {
    const auto loc = doc.locate(m_layerId);
    if (!loc) return;
    m_parentId = loc->parent->id();
    m_index = loc->index;
    m_held = loc->parent->removeAt(m_index);
}

void RemoveLayerCommand::undo(Document& doc) {
    GroupLayer* parent = doc.groupById(m_parentId);
    if (parent == nullptr || m_held == nullptr) return;
    parent->insert(m_index, std::move(m_held));
}

ReplaceLayerCommand::ReplaceLayerCommand(LayerId targetId, std::unique_ptr<Layer> replacement,
                                         std::string label)
    : m_targetId(targetId), m_replacementId(replacement ? replacement->id() : kInvalidLayerId),
      m_label(std::move(label)), m_incoming(std::move(replacement)) {}

void ReplaceLayerCommand::apply(Document& doc) {
    if (m_incoming == nullptr) return;  // already applied and not undone, or nothing to insert
    const auto loc = doc.locate(m_targetId);
    if (!loc) return;
    m_parentId = loc->parent->id();
    m_index = loc->index;
    m_outgoing = loc->parent->removeAt(m_index);
    loc->parent->insert(m_index, std::move(m_incoming));  // same parent, same index
}

void ReplaceLayerCommand::undo(Document& doc) {
    GroupLayer* parent = doc.groupById(m_parentId);
    if (parent == nullptr || m_outgoing == nullptr) return;
    // Take the replacement back out (it may have been re-indexed by nothing -- we never move it)
    // and restore the original where it stood. Redo re-applies with both held again.
    if (const auto loc = doc.locate(m_replacementId))
        m_incoming = loc->parent->removeAt(loc->index);
    parent->insert(m_index, std::move(m_outgoing));
}

// ---- MoveLayerCommand -----------------------------------------------------------------------
MoveLayerCommand::MoveLayerCommand(LayerId layerId, LayerId newParentId, std::size_t newIndex)
    : m_layerId(layerId), m_newParentId(newParentId), m_newIndex(newIndex) {}

void MoveLayerCommand::apply(Document& doc) {
    const auto loc = doc.locate(m_layerId);
    if (!loc) return;
    if (!m_captured) {
        m_oldParentId = loc->parent->id();
        m_oldIndex = loc->index;
        m_captured = true;
    }
    std::unique_ptr<Layer> held = loc->parent->removeAt(loc->index);
    GroupLayer* dest = doc.groupById(m_newParentId);
    if (dest == nullptr) {
        loc->parent->insert(loc->index, std::move(held));  // unknown destination: leave as-is
        return;
    }
    dest->insert(m_newIndex, std::move(held));
}

void MoveLayerCommand::undo(Document& doc) {
    GroupLayer* dest = doc.groupById(m_newParentId);
    GroupLayer* origin = doc.groupById(m_oldParentId);
    if (dest == nullptr || origin == nullptr) return;
    std::unique_ptr<Layer> held = dest->removeAt(m_newIndex);
    if (held == nullptr) return;
    origin->insert(m_oldIndex, std::move(held));
}

// ---- SetNameCommand -------------------------------------------------------------------------
void SetNameCommand::apply(Document& doc) {
    Layer* layer = doc.find(m_id);
    if (layer == nullptr) return;
    auto* text = layer->as<TextLayer>();
    if (!m_captured) {
        m_old = layer->name();
        m_oldMarker = layer->pastedMarker();
        m_oldAutoName = text != nullptr && text->autoNamed();
        m_captured = true;
    }
    layer->setName(m_new);
    layer->setPastedMarker(false); // naming a pasted layer adopts it: the badge clears
    if (text != nullptr)
        text->setAutoNamed(false); // a hand-picked name stops tracking the text content
}

void SetNameCommand::undo(Document& doc) {
    if (Layer* layer = doc.find(m_id)) {
        layer->setName(m_old);
        layer->setPastedMarker(m_oldMarker);
        if (auto* text = layer->as<TextLayer>())
            text->setAutoNamed(m_oldAutoName);
    }
}

// ---- RenameDocumentCommand ------------------------------------------------------------------

void RenameDocumentCommand::apply(Document& doc) {
    if (!m_captured) {
        m_old = doc.title();
        m_captured = true;
    }
    doc.setTitle(m_new);
}

void RenameDocumentCommand::undo(Document& doc) {
    doc.setTitle(m_old);
}

// ---- SetVisibleCommand ----------------------------------------------------------------------
void SetVisibleCommand::apply(Document& doc) {
    Layer* layer = doc.find(m_id);
    if (layer == nullptr) return;
    if (!m_captured) {
        m_old = layer->visible();
        m_captured = true;
    }
    layer->setVisible(m_new);
}

void SetVisibleCommand::undo(Document& doc) {
    if (Layer* layer = doc.find(m_id)) layer->setVisible(m_old);
}

// ---- SetLockedCommand -----------------------------------------------------------------------
void SetLockedCommand::apply(Document& doc) {
    Layer* layer = doc.find(m_id);
    if (layer == nullptr) return;
    if (!m_captured) {
        m_old = layer->locked();
        m_captured = true;
    }
    layer->setLocked(m_new);
}

void SetLockedCommand::undo(Document& doc) {
    if (Layer* layer = doc.find(m_id)) layer->setLocked(m_old);
}

// ---- SetBlendModeCommand --------------------------------------------------------------------
void SetBlendModeCommand::apply(Document& doc) {
    Layer* layer = doc.find(m_id);
    if (layer == nullptr) return;
    if (!m_captured) {
        m_old = layer->blendMode();
        m_captured = true;
    }
    layer->setBlendMode(m_new);
}

void SetBlendModeCommand::undo(Document& doc) {
    if (Layer* layer = doc.find(m_id)) layer->setBlendMode(m_old);
}

// ---- SetClipToBelowCommand ------------------------------------------------------------------
void SetClipToBelowCommand::apply(Document& doc) {
    Layer* layer = doc.find(m_id);
    if (layer == nullptr) return;
    if (!m_captured) {
        m_old = layer->clipToBelow();
        m_captured = true;
    }
    layer->setClipToBelow(m_new);
}

void SetClipToBelowCommand::undo(Document& doc) {
    if (Layer* layer = doc.find(m_id)) layer->setClipToBelow(m_old);
}

// ---- SetOpacityCommand (coalescing) ---------------------------------------------------------
void SetOpacityCommand::apply(Document& doc) {
    Layer* layer = doc.find(m_id);
    if (layer == nullptr) return;
    if (!m_captured) {
        m_old = layer->opacity();
        m_captured = true;
    }
    layer->setOpacity(m_new);
}

void SetOpacityCommand::undo(Document& doc) {
    if (Layer* layer = doc.find(m_id)) layer->setOpacity(m_old);
}

bool SetOpacityCommand::tryMergeWith(const Command& next) {
    const auto* other = dynamic_cast<const SetOpacityCommand*>(&next);
    if (other == nullptr || other->m_id != m_id || m_coalesce == 0 ||
        other->m_coalesce != m_coalesce) {
        return false;
    }
    m_new = other->m_new;  // absorb the latest value; keep our captured m_old
    return true;
}

// ---- SetLayerEffectsCommand (coalescing) ----------------------------------------------------
namespace {
void assignEffects(Layer& layer, const std::optional<LayerEffects>& fx) {
    if (fx)
        layer.setEffects(*fx);
    else
        layer.clearEffects();
}
}  // namespace

void SetLayerEffectsCommand::apply(Document& doc) {
    Layer* layer = doc.find(m_id);
    if (layer == nullptr) return;
    if (!m_captured) {
        m_old = layer->hasEffects() ? std::optional<LayerEffects>(layer->effects()) : std::nullopt;
        m_captured = true;
    }
    assignEffects(*layer, m_new);
}

void SetLayerEffectsCommand::undo(Document& doc) {
    if (Layer* layer = doc.find(m_id)) assignEffects(*layer, m_old);
}

bool SetLayerEffectsCommand::tryMergeWith(const Command& next) {
    const auto* other = dynamic_cast<const SetLayerEffectsCommand*>(&next);
    if (other == nullptr || other->m_id != m_id || m_coalesce == 0 ||
        other->m_coalesce != m_coalesce) {
        return false;
    }
    m_new = other->m_new;  // absorb the latest value; keep our captured m_old
    return true;
}

// ---- SetTransformCommand (coalescing) -------------------------------------------------------
void SetTransformCommand::apply(Document& doc) {
    Layer* layer = doc.find(m_id);
    if (layer == nullptr) return;
    if (!m_captured) {
        m_old = layer->transform();
        m_captured = true;
    }
    layer->setTransform(m_new);
}

void SetTransformCommand::undo(Document& doc) {
    if (Layer* layer = doc.find(m_id)) layer->setTransform(m_old);
}

bool SetTransformCommand::tryMergeWith(const Command& next) {
    const auto* other = dynamic_cast<const SetTransformCommand*>(&next);
    if (other == nullptr || other->m_id != m_id || m_coalesce == 0 ||
        other->m_coalesce != m_coalesce) {
        return false;
    }
    m_new = other->m_new;
    return true;
}

// ---- SetTransformsCommand (multi-layer, coalescing) -----------------------------------------
SetTransformsCommand::SetTransformsCommand(std::vector<Entry> entries, std::uint64_t coalesceId)
    : m_coalesce(coalesceId) {
    m_items.reserve(entries.size());
    for (const Entry& e : entries)
        m_items.push_back({e.id, e.transform, common::Affine2D{}});
}

void SetTransformsCommand::apply(Document& doc) {
    for (Item& it : m_items) {
        Layer* layer = doc.find(it.id);
        if (layer == nullptr)
            continue; // a vanished layer applies/undoes as a no-op, keeping the rest symmetric
        if (!m_captured)
            it.old = layer->transform();
        layer->setTransform(it.neu);
    }
    m_captured = true;
}

void SetTransformsCommand::undo(Document& doc) {
    for (Item& it : m_items)
        if (Layer* layer = doc.find(it.id))
            layer->setTransform(it.old);
}

bool SetTransformsCommand::tryMergeWith(const Command& next) {
    const auto* other = dynamic_cast<const SetTransformsCommand*>(&next);
    if (other == nullptr || m_coalesce == 0 || other->m_coalesce != m_coalesce ||
        other->m_items.size() != m_items.size()) {
        return false;
    }
    for (std::size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].id != other->m_items[i].id)
            return false; // the selection changed: a new step, not a continuation
    for (std::size_t i = 0; i < m_items.size(); ++i)
        m_items[i].neu = other->m_items[i].neu;
    return true;
}

// ---- SetOpacitiesCommand (multi-layer, coalescing) ------------------------------------------
SetOpacitiesCommand::SetOpacitiesCommand(std::vector<Entry> entries, std::uint64_t coalesceId)
    : m_coalesce(coalesceId) {
    m_items.reserve(entries.size());
    for (const Entry& e : entries)
        m_items.push_back({e.id, e.opacity, 1.0f});
}

void SetOpacitiesCommand::apply(Document& doc) {
    for (Item& it : m_items) {
        Layer* layer = doc.find(it.id);
        if (layer == nullptr)
            continue; // a vanished layer applies/undoes as a no-op, keeping the rest symmetric
        if (!m_captured)
            it.old = layer->opacity();
        layer->setOpacity(it.neu);
    }
    m_captured = true;
}

void SetOpacitiesCommand::undo(Document& doc) {
    for (Item& it : m_items)
        if (Layer* layer = doc.find(it.id))
            layer->setOpacity(it.old);
}

bool SetOpacitiesCommand::tryMergeWith(const Command& next) {
    const auto* other = dynamic_cast<const SetOpacitiesCommand*>(&next);
    if (other == nullptr || m_coalesce == 0 || other->m_coalesce != m_coalesce ||
        other->m_items.size() != m_items.size()) {
        return false;
    }
    for (std::size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].id != other->m_items[i].id)
            return false; // the selection changed: a new step, not a continuation
    for (std::size_t i = 0; i < m_items.size(); ++i)
        m_items[i].neu = other->m_items[i].neu;
    return true;
}

SetLayerPixelsCommand::SetLayerPixelsCommand(LayerId id, common::Image next)
    : m_id(id), m_wholeLayer(true), m_new(std::move(next)) {}

SetLayerPixelsCommand::SetLayerPixelsCommand(LayerId id, common::Image regionPixels, long originX,
                                             long originY)
    : m_id(id), m_wholeLayer(false), m_x(originX), m_y(originY), m_new(std::move(regionPixels)) {}

void SetLayerPixelsCommand::apply(Document& doc) {
    Layer* l = doc.find(m_id);
    auto* raster = l != nullptr ? l->as<RasterLayer>() : nullptr;
    if (raster == nullptr)
        return; // the layer vanished or isn't raster: apply/undo stay symmetric no-ops
    if (m_wholeLayer) {
        // Replace the entire image -- may change its dimensions (e.g. crop's pixel bake), so this
        // cannot be a region patch.
        if (!m_captured) {
            m_old = raster->image();
            m_captured = true;
        }
        raster->image() = m_new;
    } else {
        // Patch only the stored region, leaving the rest of the layer (and its size) untouched.
        if (!m_captured) {
            m_old = common::copyRegion(raster->image(), m_x, m_y, m_new.width, m_new.height);
            m_captured = true;
        }
        common::blitRegion(raster->image(), m_new, m_x, m_y);
    }
    raster->invalidateContentBounds();
}

void SetLayerPixelsCommand::undo(Document& doc) {
    Layer* l = doc.find(m_id);
    auto* raster = l != nullptr ? l->as<RasterLayer>() : nullptr;
    if (raster == nullptr)
        return;
    if (m_wholeLayer)
        raster->image() = m_old;
    else
        common::blitRegion(raster->image(), m_old, m_x, m_y);
    raster->invalidateContentBounds();
}

namespace {

// The pixel buffer a warp reads and writes for `layer`, or null when the kind carries none it may
// own. Raster and Magic are the two kinds whose pixels ARE the layer; everything else either has no
// pixel grid at all or holds a cache it regenerates (see SetLayerWarpCommand's note).
[[nodiscard]] common::Image* warpablePixels(Layer* layer) {
    if (layer == nullptr) return nullptr;
    if (auto* raster = layer->as<RasterLayer>()) return &raster->image();
    if (auto* magic = layer->as<MagicLayer>()) return &magic->source();
    return nullptr;
}

// Bump the content bounds of whichever kind we just rewrote (the two have separate, non-virtual
// invalidators, so this cannot be one call on the base).
void invalidateWarped(Layer* layer) {
    if (layer == nullptr)
        return;
    if (auto* raster = layer->as<RasterLayer>())
        raster->invalidateContentBounds();
    else if (auto* magic = layer->as<MagicLayer>())
        magic->invalidateContentBounds();
}

} // namespace

void SetLayerWarpCommand::apply(Document& doc) {
    Layer* l = doc.find(m_id);
    common::Image* px = warpablePixels(l);
    if (px == nullptr)
        return; // the layer vanished or has no pixels of its own: apply/undo stay symmetric no-ops
    if (!m_captured) {
        m_old = *px;
        m_oldTransform = l->transform();
        m_oldGrid = l->hasWarp() ? std::optional<WarpGrid>(*l->warp()) : std::nullopt;
        m_captured = true;
    }
    *px = m_new;
    l->setTransform(m_newTransform);
    if (m_newGrid)
        l->setWarp(*m_newGrid);
    else
        l->clearWarp();
    invalidateWarped(l);
}

void SetLayerWarpCommand::undo(Document& doc) {
    Layer* l = doc.find(m_id);
    common::Image* px = warpablePixels(l);
    if (px == nullptr)
        return;
    *px = m_old;
    l->setTransform(m_oldTransform);
    if (m_oldGrid)
        l->setWarp(*m_oldGrid);
    else
        l->clearWarp();
    invalidateWarped(l);
}

std::optional<common::Rect> SetLayerWarpCommand::dirtyRegion(const Document& doc) const {
    (void)doc;
    return std::nullopt; // see the header: the extent AND the placement move, so recomposite all
}

void SetLayerMaskCommand::apply(Document& doc) {
    Layer* l = doc.find(m_id);
    if (l == nullptr)
        return; // the layer vanished: apply/undo stay symmetric no-ops
    if (!m_captured) {
        m_old = l->hasMask() ? std::optional<RasterMask>(*l->mask()) : std::nullopt;
        m_captured = true;
    }
    if (m_new)
        l->setMask(*m_new);
    else
        l->clearMask();
}

void SetLayerMaskCommand::undo(Document& doc) {
    Layer* l = doc.find(m_id);
    if (l == nullptr)
        return;
    if (m_old)
        l->setMask(*m_old);
    else
        l->clearMask();
}

void SetMaskEnabledCommand::apply(Document& doc) {
    Layer* l = doc.find(m_id);
    RasterMask* m = l != nullptr ? l->mask() : nullptr;
    if (m == nullptr)
        return; // no mask to flip: apply/undo stay symmetric no-ops
    if (!m_captured) {
        m_old = m->enabled;
        m_captured = true;
    }
    m->enabled = m_new;
    l->bumpMaskRevision();
}

void SetMaskEnabledCommand::undo(Document& doc) {
    Layer* l = doc.find(m_id);
    RasterMask* m = l != nullptr ? l->mask() : nullptr;
    if (m == nullptr)
        return;
    m->enabled = m_old;
    l->bumpMaskRevision();
}

void SetMaskLinkedCommand::apply(Document& doc) {
    Layer* l = doc.find(m_id);
    RasterMask* m = l != nullptr ? l->mask() : nullptr;
    if (m == nullptr)
        return; // no mask to flip: apply/undo stay symmetric no-ops
    if (!m_captured) {
        m_old = m->linked;
        m_oldToLocal = m->toLocal;
        m_captured = true;
    }
    if (m_new != m->linked) {
        // Rebase the sheet through the layer's own transform, so the mask does not MOVE when the
        // chain is clicked -- only what it does on the next transform changes (see the header).
        // A raster sheet's proportional source scale is deliberately not carried: it is the
        // identity for every mask these helpers build, and the flag never rescales the cells.
        const common::Affine2D t = l->transform();
        if (m_new) { // parent space -> the layer's own
            if (const std::optional<common::Affine2D> inv = t.inverse())
                m->toLocal = *inv * m->toLocal;
        } else { // the layer's own -> parent space
            m->toLocal = t * m->toLocal;
        }
    }
    m->linked = m_new;
    l->bumpMaskRevision();
}

void SetMaskLinkedCommand::undo(Document& doc) {
    Layer* l = doc.find(m_id);
    RasterMask* m = l != nullptr ? l->mask() : nullptr;
    if (m == nullptr)
        return;
    m->linked = m_old;
    m->toLocal = m_oldToLocal;
    l->bumpMaskRevision();
}

// ---- SetMaskPlacementCommand (mask sheets, multi-layer, coalescing) -------------------------
SetMaskPlacementCommand::SetMaskPlacementCommand(std::vector<Entry> entries,
                                                 std::uint64_t coalesceId)
    : m_coalesce(coalesceId) {
    m_items.reserve(entries.size());
    for (const Entry& e : entries)
        m_items.push_back({e.id, e.toLocal, common::Affine2D{}});
}

void SetMaskPlacementCommand::apply(Document& doc) {
    for (Item& it : m_items) {
        Layer* layer = doc.find(it.id);
        RasterMask* mask = layer != nullptr ? layer->mask() : nullptr;
        if (mask == nullptr)
            continue; // a vanished layer/mask applies+undoes as a no-op, keeping the rest symmetric
        if (!m_captured)
            it.old = mask->toLocal;
        mask->toLocal = it.neu;
        // The cells did not change, but where they LAND did: the panel's mask thumbnail and every
        // other consumer keyed on maskRevision() is reading a stale placement until this bumps.
        layer->bumpMaskRevision();
    }
    m_captured = true;
}

void SetMaskPlacementCommand::undo(Document& doc) {
    for (Item& it : m_items) {
        Layer* layer = doc.find(it.id);
        RasterMask* mask = layer != nullptr ? layer->mask() : nullptr;
        if (mask == nullptr)
            continue;
        mask->toLocal = it.old;
        layer->bumpMaskRevision();
    }
}

bool SetMaskPlacementCommand::tryMergeWith(const Command& next) {
    const auto* other = dynamic_cast<const SetMaskPlacementCommand*>(&next);
    if (other == nullptr || m_coalesce == 0 || other->m_coalesce != m_coalesce ||
        other->m_items.size() != m_items.size()) {
        return false;
    }
    for (std::size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].id != other->m_items[i].id)
            return false; // the set changed: a new step, not a continuation
    for (std::size_t i = 0; i < m_items.size(); ++i)
        m_items[i].neu = other->m_items[i].neu;
    return true;
}

// Copy `bytes` (the stored w x h region) into the mask's coverage, clipped to the mask.
void SetMaskPixelsCommand::blit(Document& doc, const std::vector<std::uint8_t>& bytes) const {
    Layer* l = doc.find(m_id);
    RasterMask* m = l != nullptr ? l->mask() : nullptr;
    if (m == nullptr || m->empty() || bytes.size() != static_cast<std::size_t>(m_w) * m_h)
        return;
    const long x0 = std::max(0L, m_x);
    const long y0 = std::max(0L, m_y);
    const long x1 = std::min(m_x + static_cast<long>(m_w), static_cast<long>(m->width));
    const long y1 = std::min(m_y + static_cast<long>(m_h), static_cast<long>(m->height));
    for (long y = y0; y < y1; ++y) {
        const std::uint8_t* srcRow =
            bytes.data() + static_cast<std::size_t>(y - m_y) * m_w + (x0 - m_x);
        std::uint8_t* dstRow =
            m->coverage.data() + static_cast<std::size_t>(y) * m->width + x0;
        std::copy(srcRow, srcRow + (x1 - x0), dstRow);
    }
    l->bumpMaskRevision();
}

void SetMaskPixelsCommand::apply(Document& doc) {
    if (!m_captured) {
        // Capture the region's old coverage (out-of-mask cells stay 0 -- they are never written
        // back, blit clips both ways).
        const Layer* l = doc.find(m_id);
        const RasterMask* m = l != nullptr ? l->mask() : nullptr;
        if (m == nullptr || m->empty())
            return; // nothing to patch: stay uncaptured so a later redo can't half-apply
        m_old.assign(static_cast<std::size_t>(m_w) * m_h, 0);
        const long x0 = std::max(0L, m_x);
        const long y0 = std::max(0L, m_y);
        const long x1 = std::min(m_x + static_cast<long>(m_w), static_cast<long>(m->width));
        const long y1 = std::min(m_y + static_cast<long>(m_h), static_cast<long>(m->height));
        for (long y = y0; y < y1; ++y) {
            const std::uint8_t* srcRow =
                m->coverage.data() + static_cast<std::size_t>(y) * m->width + x0;
            std::uint8_t* dstRow =
                m_old.data() + static_cast<std::size_t>(y - m_y) * m_w + (x0 - m_x);
            std::copy(srcRow, srcRow + (x1 - x0), dstRow);
        }
        m_captured = true;
    }
    blit(doc, m_new);
}

void SetMaskPixelsCommand::undo(Document& doc) {
    if (m_captured)
        blit(doc, m_old);
}

std::optional<common::Rect> SetMaskPixelsCommand::dirtyRegion(const Document& doc) const {
    const Layer* l = doc.find(m_id);
    const RasterMask* m = l != nullptr ? l->mask() : nullptr;
    if (m == nullptr || m->empty())
        return std::nullopt; // layer/mask gone: fall back to a full recomposite
    // Mask px -> document, through the sheet's own placement and the transform the compositor
    // folds it under (the layer's when linked, the parent chain's when not): core::maskToDocument
    // is that map, and using it here is what keeps the scoped recomposite over the pixels the
    // stroke actually changed on a transformed layer.
    const common::Affine2D fwd = maskToDocument(*l, *m);
    const common::Rect r = fwd.mapBounds(common::Rect{static_cast<double>(m_x),
                                                      static_cast<double>(m_y),
                                                      static_cast<double>(m_w),
                                                      static_cast<double>(m_h)});
    const double fx = std::floor(r.x), fy = std::floor(r.y);
    return common::Rect{fx, fy, std::ceil(r.right()) - fx, std::ceil(r.bottom()) - fy};
}

std::optional<common::Rect> SetLayerPixelsCommand::dirtyRegion(const Document& doc) const {
    const Layer* l = doc.find(m_id);
    if (l == nullptr)
        return std::nullopt; // layer gone: fall back to a full recomposite
    // Map the stored layer-local rect to its document-space bounding box (the layer may be
    // transformed). Floor/ceil to whole document pixels so the scoped recomposite covers it fully.
    const common::Affine2D fwd = worldTransform(*l);
    const double x0 = static_cast<double>(m_x), y0 = static_cast<double>(m_y);
    const double x1 = x0 + static_cast<double>(m_new.width);
    const double y1 = y0 + static_cast<double>(m_new.height);
    const common::Vec2 c[4] = {fwd.apply({x0, y0}), fwd.apply({x1, y0}), fwd.apply({x1, y1}),
                               fwd.apply({x0, y1})};
    double minx = c[0].x, miny = c[0].y, maxx = c[0].x, maxy = c[0].y;
    for (int i = 1; i < 4; ++i) {
        minx = std::min(minx, c[i].x);
        miny = std::min(miny, c[i].y);
        maxx = std::max(maxx, c[i].x);
        maxy = std::max(maxy, c[i].y);
    }
    const double fx = std::floor(minx), fy = std::floor(miny);
    return common::Rect{fx, fy, std::ceil(maxx) - fx, std::ceil(maxy) - fy};
}

std::optional<LayerPixelEdit> SetLayerPixelsCommand::dirtyLayerPixels(const Document& doc) const {
    const Layer* l = doc.find(m_id);
    if (l == nullptr || l->as<RasterLayer>() == nullptr)
        return std::nullopt; // the layer vanished or is not raster: no claim to make
    LayerPixelEdit out;
    out.layer = m_id;
    if (m_wholeLayer)
        return out; // empty rect == the whole image, which is what a resize-capable replace is
    out.rect = common::Rect{static_cast<double>(m_x), static_cast<double>(m_y),
                            static_cast<double>(m_new.width), static_cast<double>(m_new.height)};
    return out;
}

// ---- GrowAndPaintLayerCommand ----------------------------------------------------------------
GrowAndPaintLayerCommand::GrowAndPaintLayerCommand(LayerId id, std::uint32_t newW,
                                                   std::uint32_t newH, long offsetX, long offsetY,
                                                   common::Image regionPixels, long regionX,
                                                   long regionY)
    : m_id(id), m_w(newW), m_h(newH), m_offX(offsetX), m_offY(offsetY),
      m_new(std::move(regionPixels)), m_rx(regionX), m_ry(regionY) {}

void GrowAndPaintLayerCommand::apply(Document& doc) {
    Layer* l = doc.find(m_id);
    auto* raster = l != nullptr ? l->as<RasterLayer>() : nullptr;
    if (raster == nullptr)
        return; // the layer vanished or isn't raster: apply/undo stay symmetric no-ops
    if (!m_captured) {
        m_old = raster->image();
        m_oldMask = raster->hasMask() ? std::optional<RasterMask>(*raster->mask()) : std::nullopt;
        m_oldTransform = raster->transform();
        m_captured = true;
    }
    if (m_w == 0 || m_h == 0)
        return; // a degenerate grid would throw the layer's pixels away; refuse it
    // Re-home the pre-growth pixels. copyRegion reads outside its source as transparent, so the old
    // image lands byte-exact at (m_offX, m_offY) and the grown band arrives empty -- exactly what a
    // layer that never had those pixels looks like.
    raster->image() = common::copyRegion(m_old, -m_offX, -m_offY, m_w, m_h);
    // The mask rides the raster's own grid 1 px per image px, so it has to move with it or it would
    // slide across the content. Only when the two SHARED a grid to begin with: they always do when
    // the canvas raised this command (it refuses to grow otherwise), and a mismatch here means
    // something else resized the layer, in which case leaving the mask alone is the honest answer.
    if (m_oldMask && m_oldMask->width == m_old.width && m_oldMask->height == m_old.height) {
        RasterMask grown(m_w, m_h, 255); // the new band REVEALS -- see the header
        grown.enabled = m_oldMask->enabled;
        grown.linked = m_oldMask->linked;
        for (std::uint32_t row = 0; row < m_oldMask->height; ++row) {
            const long dy = m_offY + static_cast<long>(row);
            if (dy < 0 || dy >= static_cast<long>(m_h))
                continue;
            for (std::uint32_t col = 0; col < m_oldMask->width; ++col) {
                const long dx = m_offX + static_cast<long>(col);
                if (dx < 0 || dx >= static_cast<long>(m_w))
                    continue;
                grown.coverage[static_cast<std::size_t>(dy) * m_w + static_cast<std::size_t>(dx)] =
                    m_oldMask->coverage[static_cast<std::size_t>(row) * m_oldMask->width + col];
            }
        }
        raster->setMask(std::move(grown)); // setMask bumps the mask revision itself
    }
    // ... and the placement absorbs the shift, so the existing content does not move a pixel in
    // document space: a point that sat at layer-local p now sits at p + offset, and
    // `old * translate(-offset)` maps that back to exactly where it was.
    raster->setTransform(m_oldTransform * common::Affine2D::translation(
                                              -static_cast<double>(m_offX),
                                              -static_cast<double>(m_offY)));
    common::blitRegion(raster->image(), m_new, m_rx, m_ry);
    raster->invalidateContentBounds();
}

void GrowAndPaintLayerCommand::undo(Document& doc) {
    Layer* l = doc.find(m_id);
    auto* raster = l != nullptr ? l->as<RasterLayer>() : nullptr;
    if (raster == nullptr || !m_captured)
        return;
    // Restore what was captured rather than un-growing by arithmetic: a round trip is then byte-
    // exact by construction, whatever the growth did.
    raster->image() = m_old;
    if (m_oldMask)
        raster->setMask(*m_oldMask);
    raster->setTransform(m_oldTransform);
    raster->invalidateContentBounds();
}

std::optional<common::Rect> GrowAndPaintLayerCommand::dirtyRegion(const Document&) const {
    return std::nullopt; // see the header: the extent changed, so the whole document is the region
}

void SetVectorObjectCommand::set(Document& doc, const std::optional<vec::Object>& obj) const {
    Layer* layer = doc.find(m_id);
    auto* vl = layer ? layer->as<VectorLayer>() : nullptr;
    if (vl == nullptr) return;
    if (obj)
        vl->setObject(*obj);
    else
        vl->clearObject();
}

void SetVectorObjectCommand::apply(Document& doc) {
    Layer* layer = doc.find(m_id);
    if (!m_captured) {
        const auto* vl = layer ? layer->as<VectorLayer>() : nullptr;
        m_old = (vl != nullptr && vl->hasObject()) ? std::optional<vec::Object>(*vl->object())
                                                   : std::nullopt;
        if (m_newXform && layer != nullptr)
            m_oldXform = layer->transform(); // capture for an atomic resize (object + placement)
        m_captured = true;
    }
    set(doc, m_new);
    if (m_newXform && layer != nullptr)
        layer->setTransform(*m_newXform); // the resize re-anchor (S26-b §7.1)
}

void SetVectorObjectCommand::undo(Document& doc) {
    set(doc, m_old);
    if (m_oldXform) {
        if (Layer* layer = doc.find(m_id))
            layer->setTransform(*m_oldXform);
    }
}

bool SetVectorObjectCommand::tryMergeWith(const Command& next) {
    const auto* other = dynamic_cast<const SetVectorObjectCommand*>(&next);
    if (other == nullptr || other->m_id != m_id || m_coalesce == 0 ||
        other->m_coalesce != m_coalesce) {
        return false;
    }
    m_new = other->m_new; // keep our captured m_old; absorb the newer target object
    if (other->m_newXform)
        m_newXform = other->m_newXform; // ... and the newer placement (resize re-anchor)
    return true;
}

// ---- SetTextCommand (coalescing) ------------------------------------------------------------
void SetTextCommand::apply(Document& doc) {
    Layer* layer = doc.find(m_id);
    auto* tl = layer ? layer->as<TextLayer>() : nullptr;
    if (tl == nullptr) return;
    if (!m_captured) {
        m_old = tl->block();
        m_captured = true;
    }
    tl->setBlock(m_new);
}

void SetTextCommand::undo(Document& doc) {
    Layer* layer = doc.find(m_id);
    if (auto* tl = layer ? layer->as<TextLayer>() : nullptr) tl->setBlock(m_old);
}

bool SetTextCommand::tryMergeWith(const Command& next) {
    const auto* other = dynamic_cast<const SetTextCommand*>(&next);
    if (other == nullptr || other->m_id != m_id || m_coalesce == 0 ||
        other->m_coalesce != m_coalesce) {
        return false;
    }
    m_new = other->m_new;  // keep our captured m_old; absorb the newer block
    return true;
}

// ---- SetTextureCommand (coalescing) ----------------------------------------------------------
void SetTextureCommand::apply(Document& doc) {
    Layer* layer = doc.find(m_id);
    auto* xl = layer ? layer->as<TextureLayer>() : nullptr;
    if (xl == nullptr) return;
    if (!m_captured) {
        m_old = xl->params();
        m_captured = true;
    }
    xl->setParams(m_new);
}

void SetTextureCommand::undo(Document& doc) {
    Layer* layer = doc.find(m_id);
    if (auto* xl = layer ? layer->as<TextureLayer>() : nullptr) xl->setParams(m_old);
}

bool SetTextureCommand::tryMergeWith(const Command& next) {
    const auto* other = dynamic_cast<const SetTextureCommand*>(&next);
    if (other == nullptr || other->m_id != m_id || m_coalesce == 0 ||
        other->m_coalesce != m_coalesce) {
        return false;
    }
    m_new = other->m_new;  // keep our captured m_old; absorb the newer params
    return true;
}

// ---- SetAdjustmentParamsCommand (coalescing) --------------------------------------------------
void SetAdjustmentParamsCommand::apply(Document& doc) {
    Layer* layer = doc.find(m_id);
    auto* adj = layer ? layer->as<AdjustmentLayer>() : nullptr;
    if (adj == nullptr) return;
    if (!m_captured) {
        m_old = adj->params();
        m_captured = true;
    }
    adj->params() = m_new;
}

void SetAdjustmentParamsCommand::undo(Document& doc) {
    Layer* layer = doc.find(m_id);
    if (auto* adj = layer ? layer->as<AdjustmentLayer>() : nullptr) adj->params() = m_old;
}

bool SetAdjustmentParamsCommand::tryMergeWith(const Command& next) {
    const auto* other = dynamic_cast<const SetAdjustmentParamsCommand*>(&next);
    if (other == nullptr || other->m_id != m_id || m_coalesce == 0 ||
        other->m_coalesce != m_coalesce) {
        return false;
    }
    m_new = other->m_new;  // keep our captured m_old; absorb the newer bag
    return true;
}

void ResizeCanvasCommand::apply(Document& doc) {
    if (!m_captured) {
        m_oldW = doc.width();
        m_oldH = doc.height();
        m_captured = true;
    }
    doc.setCanvasSize(m_newW, m_newH);
}

void ResizeCanvasCommand::undo(Document& doc) {
    doc.setCanvasSize(m_oldW, m_oldH);
}

void SetSelectionCommand::apply(Document& doc) {
    if (!m_captured) {
        m_old = doc.selection();
        m_captured = true;
    }
    doc.setSelection(m_new);
}

void SetSelectionCommand::undo(Document& doc) {
    doc.setSelection(m_old);
}

bool SetSelectionCommand::tryMergeWith(const Command& next) {
    const auto* other = dynamic_cast<const SetSelectionCommand*>(&next);
    if (other == nullptr || m_coalesce == 0 || other->m_coalesce != m_coalesce)
        return false;
    m_new = other->m_new; // absorb the latest mask; keep our captured m_old (and our label)
    return true;
}

// ---- Guide commands -------------------------------------------------------------------------

void AddGuideCommand::apply(Document& doc) {
    doc.addGuide(m_guide);
}

void AddGuideCommand::undo(Document& doc) {
    doc.removeGuide(m_guide.id);
}

void RemoveGuideCommand::apply(Document& doc) {
    if (!m_have)
        if (const Guide* g = doc.findGuide(m_id)) {
            m_captured = *g;
            m_have = true;
        }
    doc.removeGuide(m_id);
}

void RemoveGuideCommand::undo(Document& doc) {
    if (m_have)
        doc.addGuide(m_captured); // re-adds with the same id (order does not matter for guides)
}

void MoveGuideCommand::apply(Document& doc) {
    doc.setGuidePosition(m_id, m_new);
}

void MoveGuideCommand::undo(Document& doc) {
    doc.setGuidePosition(m_id, m_old);
}

bool MoveGuideCommand::tryMergeWith(const Command& next) {
    const auto* other = dynamic_cast<const MoveGuideCommand*>(&next);
    if (other == nullptr || m_coalesce == 0 || other->m_coalesce != m_coalesce ||
        other->m_id != m_id)
        return false;
    m_new = other->m_new; // absorb the latest position; keep our captured m_old
    return true;
}

void ClearGuidesCommand::apply(Document& doc) {
    if (!m_have) {
        m_captured = doc.guides();
        m_have = true;
    }
    doc.setGuides({});
}

void ClearGuidesCommand::undo(Document& doc) {
    doc.setGuides(m_captured);
}

void SetGuidesCommand::apply(Document& doc) {
    if (!m_captured) {
        m_old = doc.guides();
        m_captured = true;
    }
    doc.setGuides(m_new);
}

void SetGuidesCommand::undo(Document& doc) {
    doc.setGuides(m_old);
}

}  // namespace mosaic::core
