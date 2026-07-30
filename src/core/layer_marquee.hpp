#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "common/geometry.hpp"
#include "core/layer.hpp"

// The Move tool's empty-space drag marquee (S15-f): which layers a rubber-banded document rectangle
// gathers. Pure geometry plus one shallow tree walk -- FLTK- and Vulkan-free, so the maths is
// unit-tested headlessly (tests/test_layer_marquee.cpp) while VulkanCanvas keeps only the pointer
// plumbing and the overlay lane. The companion of core::topmostLayerAt (the click-pick): both agree
// that an invisible layer is untouchable and that a layer with no content extent is not a target,
// and both leave LOCKED layers selectable (locking refuses transforms, not selection -- the refusal
// lives in VulkanCanvas::beginMoveGesture).
namespace mosaic::core {

// Does the axis-aligned `rect` overlap the convex quad `quad` (either winding)? Separating-axis
// test over the rect's two axes plus the quad's four edge normals: a layer's content box becomes a
// general parallelogram once the layer (or an ancestor group) is rotated or sheared, and an
// AABB-vs-AABB test would then gather layers the user visibly never touched. Contact alone is a
// MISS (a zero-area overlap is not "touched"), which also makes a degenerate rect gather nothing.
[[nodiscard]] inline bool rectIntersectsQuad(const common::Rect& rect,
                                             const std::array<common::Vec2, 4>& quad) {
    if (rect.empty())
        return false;
    const std::array<common::Vec2, 4> box{
        common::Vec2{rect.x, rect.y}, common::Vec2{rect.right(), rect.y},
        common::Vec2{rect.right(), rect.bottom()}, common::Vec2{rect.x, rect.bottom()}};
    // Both shapes are convex, so overlap on EVERY candidate axis means they intersect.
    const auto separated = [&box, &quad](common::Vec2 axis) {
        if (axis.dot(axis) < 1e-18)
            return false; // a degenerate edge yields no axis: it separates nothing
        double aLo = box[0].dot(axis), aHi = aLo;
        double bLo = quad[0].dot(axis), bHi = bLo;
        for (std::size_t i = 1; i < 4; ++i) {
            const double a = box[i].dot(axis);
            aLo = std::min(aLo, a);
            aHi = std::max(aHi, a);
            const double b = quad[i].dot(axis);
            bLo = std::min(bLo, b);
            bHi = std::max(bHi, b);
        }
        return aHi <= bLo || bHi <= aLo;
    };
    if (separated({1.0, 0.0}) || separated({0.0, 1.0}))
        return false;
    for (std::size_t i = 0; i < 4; ++i) {
        const common::Vec2 e = quad[(i + 1) % 4] - quad[i];
        if (separated({-e.y, e.x})) // the edge's outward normal
            return false;
    }
    return true;
}

// `layer`'s content box mapped into DOCUMENT space (TL, TR, BR, BL), or nullopt when the layer has
// no content extent at all -- an adjustment layer, a fully transparent raster, a text/texture layer
// the renderer has not measured yet. A group's contentBounds() is the union of its visible
// children's boxes in the group's own frame, so the group maps as one unit here, matching how
// moveClickTarget treats an ungrouped click.
[[nodiscard]] inline std::optional<std::array<common::Vec2, 4>>
layerContentQuad(const Layer& layer) {
    const std::optional<common::Rect> box = layer.contentBounds();
    if (!box || box->empty())
        return std::nullopt;
    const common::Affine2D world = worldTransform(layer);
    return std::array<common::Vec2, 4>{
        world.apply(box->topLeft()), world.apply({box->right(), box->y}),
        world.apply({box->right(), box->bottom()}), world.apply({box->x, box->bottom()})};
}

// Every top-level unit (a direct child of `root`) whose content the document-space `rect` touches,
// in stack order (bottom first). Top-level ON PURPOSE: it is the marquee's reading of the same
// Affinity group model moveClickTarget applies to a click -- grouped content is one object, so a
// band across a group gathers the group, not its leaves. Invisible units are skipped, exactly as
// the click-pick skips them; locked ones are gathered (they select, they just refuse to transform).
[[nodiscard]] inline std::vector<LayerId> layersInMarquee(const GroupLayer& root,
                                                          const common::Rect& rect) {
    std::vector<LayerId> out;
    if (rect.empty())
        return out;
    for (std::size_t i = 0; i < root.childCount(); ++i) {
        const Layer& child = root.child(i);
        if (!child.visible())
            continue;
        const std::optional<std::array<common::Vec2, 4>> quad = layerContentQuad(child);
        if (quad && rectIntersectsQuad(rect, *quad))
            out.push_back(child.id());
    }
    return out;
}

} // namespace mosaic::core
