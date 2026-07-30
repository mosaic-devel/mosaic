#pragma once

#include <cmath>
#include <vector>

#include "common/geometry.hpp"

// Snapping (View -> Snap): the pure geometry that pulls a moving box's edges + centres onto nearby
// reference lines -- document guides, the canvas edges + centre, and other layers' bounding boxes.
// FLTK-free; the canvas gathers the candidate lines (in document coordinates) and applies the offset
// this returns to the drag. The same match info drives the smart-guide overlay.
namespace mosaic::core {

// Candidate snap lines in document space: `vertical` are x = const lines (snap the box's left /
// centre-x / right to them); `horizontal` are y = const lines (top / centre-y / bottom).
struct SnapCandidates {
    std::vector<double> vertical;
    std::vector<double> horizontal;
};

struct SnapResult {
    double dx = 0.0; // the offset to add to the box so an anchor lands on a line
    double dy = 0.0;
    bool snappedX = false;
    bool snappedY = false;
    double lineX = 0.0; // the matched vertical line (valid when snappedX) -- for smart-guide display
    double lineY = 0.0; // the matched horizontal line (valid when snappedY)
};

// Snap the axis-aligned box `box` (document space) so one of its X anchors (left / centre / right)
// lands on the nearest vertical candidate within `threshold` doc px, and likewise one Y anchor onto
// a horizontal candidate. Each axis is resolved independently; the smallest correction wins.
[[nodiscard]] inline SnapResult snapBox(const common::Rect& box, const SnapCandidates& cand,
                                        double threshold) {
    SnapResult r;
    const double ax[3] = {box.x, box.center().x, box.right()};
    double bestX = threshold;
    for (const double line : cand.vertical)
        for (const double anchor : ax) {
            const double d = line - anchor;
            if (std::abs(d) < bestX) {
                bestX = std::abs(d);
                r.dx = d;
                r.snappedX = true;
                r.lineX = line;
            }
        }
    const double ay[3] = {box.y, box.center().y, box.bottom()};
    double bestY = threshold;
    for (const double line : cand.horizontal)
        for (const double anchor : ay) {
            const double d = line - anchor;
            if (std::abs(d) < bestY) {
                bestY = std::abs(d);
                r.dy = d;
                r.snappedY = true;
                r.lineY = line;
            }
        }
    return r;
}

} // namespace mosaic::core
