#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "common/geometry.hpp"

// Alignment & distribution (Arrange menu): the pure geometry that lines up a set of layer bounding
// boxes. FLTK-free; the app maps each selected layer to its document-space AABB, gets the per-box
// translation from here, and lands the result as one SetTransformsCommand.
namespace mosaic::core {

enum class AlignEdge { Left, HCenter, Right, Top, VMiddle, Bottom };
enum class DistributeAxis { Horizontal, Vertical };

// Per-box translation (dx, dy) that aligns each box's chosen edge/centre to `reference` -- a fixed
// rect that never moves. The app passes the document canvas rect here for the single-layer
// align-to-canvas path (aligning one box to its own union would be a no-op).
[[nodiscard]] inline std::vector<common::Vec2>
alignTranslations(const std::vector<common::Rect>& boxes, AlignEdge edge,
                  const common::Rect& reference) {
    std::vector<common::Vec2> out(boxes.size(), common::Vec2{0.0, 0.0});
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const common::Rect& b = boxes[i];
        double dx = 0.0, dy = 0.0;
        switch (edge) {
        case AlignEdge::Left: dx = reference.x - b.x; break;
        case AlignEdge::HCenter: dx = reference.center().x - b.center().x; break;
        case AlignEdge::Right: dx = reference.right() - b.right(); break;
        case AlignEdge::Top: dy = reference.y - b.y; break;
        case AlignEdge::VMiddle: dy = reference.center().y - b.center().y; break;
        case AlignEdge::Bottom: dy = reference.bottom() - b.bottom(); break;
        }
        out[i] = {dx, dy};
    }
    return out;
}

// Per-box translation (dx, dy) that aligns each box's chosen edge/centre to the selection's union
// bounding box -- the Photoshop/Illustrator "align to selection" convention (the extreme box on
// that side stays put; the rest move to meet it). Boxes with no translation get {0, 0}.
[[nodiscard]] inline std::vector<common::Vec2>
alignTranslations(const std::vector<common::Rect>& boxes, AlignEdge edge) {
    if (boxes.empty())
        return {};
    common::Rect uni = boxes.front();
    for (const common::Rect& b : boxes)
        uni = uni.united(b);
    return alignTranslations(boxes, edge, uni);
}

// Per-box translation that distributes the boxes so the GAPS between adjacent boxes (along the axis)
// are equal -- the "distribute spacing" convention. The two extreme boxes stay put. Needs at least 3
// boxes (fewer -> all zero). Order is by each box's min coordinate along the axis.
[[nodiscard]] inline std::vector<common::Vec2>
distributeTranslations(const std::vector<common::Rect>& boxes, DistributeAxis axis) {
    const std::size_t n = boxes.size();
    std::vector<common::Vec2> out(n, common::Vec2{0.0, 0.0});
    if (n < 3)
        return out;
    const bool horiz = axis == DistributeAxis::Horizontal;
    const auto lo = [&](std::size_t i) { return horiz ? boxes[i].x : boxes[i].y; };
    const auto sz = [&](std::size_t i) { return horiz ? boxes[i].w : boxes[i].h; };

    std::vector<std::size_t> idx(n);
    for (std::size_t i = 0; i < n; ++i)
        idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return lo(a) < lo(b); });

    const double spanLo = lo(idx.front());
    const double spanHi = lo(idx.back()) + sz(idx.back());
    double totalSize = 0.0;
    for (const std::size_t i : idx)
        totalSize += sz(i);
    const double gap = (spanHi - spanLo - totalSize) / static_cast<double>(n - 1);

    double cursor = spanLo;
    for (const std::size_t i : idx) {
        const double delta = cursor - lo(i);
        out[i] = horiz ? common::Vec2{delta, 0.0} : common::Vec2{0.0, delta};
        cursor += sz(i) + gap;
    }
    return out;
}

} // namespace mosaic::core
