#include "ui/warp_gesture.hpp"

#include <algorithm>
#include <cmath>
#include <iterator> // std::size over the quality table

#include "render/warp.hpp" // warpIsolines / warpQuadLines: the overlay samples THE surface

namespace mosaic::ui {
namespace {

// The order the "Quality" choice publishes: the Move tool's "Anti-aliasing" list verbatim
// (MainWindow::currentResampleFilter's map). One order for the whole app.
constexpr render::ResampleFilter kQualityMap[] = {
    render::ResampleFilter::Auto,     render::ResampleFilter::Nearest,
    render::ResampleFilter::Bilinear, render::ResampleFilter::Bicubic,
    render::ResampleFilter::Mitchell, render::ResampleFilter::Lanczos2,
    render::ResampleFilter::Lanczos3, render::ResampleFilter::Area,
    render::ResampleFilter::Gaussian, render::ResampleFilter::Supersample,
};

// Catmull-Rom samples per patch edge for the DRAWN grid. Matched to the kernel's own final-quality
// subdivision so the line the user aims at is the line the pixels follow.
constexpr int kLineStepsMax = 12;

} // namespace

render::ResampleFilter warpQualityForChoice(int index) noexcept {
    if (index >= 0 && index < static_cast<int>(std::size(kQualityMap)))
        return kQualityMap[static_cast<std::size_t>(index)];
    return render::ResampleFilter::Auto;
}

int warpQualityChoiceIndex(render::ResampleFilter f) noexcept {
    for (std::size_t i = 0; i < std::size(kQualityMap); ++i)
        if (kQualityMap[i] == f) return static_cast<int>(i);
    return 0; // Auto
}

WarpDragMode warpDragModeFor(bool shift, bool alt) noexcept {
    // Alt wins: "move the whole thing" is a different gesture from "move this node", and a user
    // holding both is asking for the coarser one.
    if (alt) return WarpDragMode::MoveAll;
    return shift ? WarpDragMode::ConstrainAxis : WarpDragMode::Free;
}

std::optional<int> hitWarpHandle(const std::vector<common::Vec2>& handles, common::Vec2 screenPt,
                                 double hitPx) {
    std::optional<int> best;
    double bestD2 = hitPx * hitPx;
    for (std::size_t i = 0; i < handles.size(); ++i) {
        const double dx = handles[i].x - screenPt.x;
        const double dy = handles[i].y - screenPt.y;
        const double d2 = dx * dx + dy * dy;
        if (d2 <= bestD2) { // <= so a later handle at the SAME distance wins: the topmost drawn one
            bestD2 = d2;
            best = static_cast<int>(i);
        }
    }
    return best;
}

std::vector<common::Vec2> warpHandlePoints(const core::WarpGrid& g) {
    std::vector<common::Vec2> out;
    if (!g.valid()) return out;
    if (g.kind == core::WarpKind::Perspective) {
        out = {g.point(0, 0), g.point(1, 0), g.point(0, 1), g.point(1, 1)};
        return out;
    }
    out.reserve(g.points.size());
    for (int r = 0; r < g.rows; ++r)
        for (int c = 0; c < g.cols; ++c) out.push_back(g.point(c, r));
    return out;
}

core::WarpGrid warpDragged(const core::WarpGrid& base, int index, common::Vec2 pressAt,
                           common::Vec2 to, WarpDragMode mode) {
    core::WarpGrid out = base;
    if (!base.valid()) return out;
    const std::vector<common::Vec2> handles = warpHandlePoints(base);
    if (index < 0 || index >= static_cast<int>(handles.size())) return out;
    // The handle follows the CURSOR's DELTA from the press, never the cursor's absolute position: a
    // press that landed a few px off the handle must not teleport it under the pointer.
    common::Vec2 delta{to.x - pressAt.x, to.y - pressAt.y};
    if (mode == WarpDragMode::ConstrainAxis) {
        // Lock to whichever axis the drag has travelled further along, measured from the PRESS point
        // -- so the axis is decided by the gesture and re-decided as it grows, and a drag that turns
        // a corner follows the turn instead of staying stuck on its first pixel's answer.
        if (std::abs(delta.x) >= std::abs(delta.y))
            delta.y = 0.0;
        else
            delta.x = 0.0;
    }
    if (mode == WarpDragMode::MoveAll) {
        for (common::Vec2& p : out.points) p = p + delta;
        return out;
    }
    // Both handle sets are indexed row-major into `points` -- a Mesh handle index IS its lattice
    // index, and Perspective's four corners are the whole of a 2x2 lattice -- so one lookup serves.
    const std::size_t k = static_cast<std::size_t>(index);
    if (k < out.points.size()) out.points[k] = out.points[k] + delta;
    return out;
}

int warpLineSteps(const core::WarpGrid& g, std::size_t budget) {
    if (!g.valid()) return 1;
    if (g.kind == core::WarpKind::Perspective) return 1; // straight edges + diagonals: 7 vertices
    int steps = kLineStepsMax;
    while (steps > 1) {
        // rows lines of (cols-1)*steps+1 vertices, plus cols lines of (rows-1)*steps+1, plus one
        // break marker per run, plus the doubled outer boundary (4 of those runs drawn twice more).
        const std::size_t rowLen = static_cast<std::size_t>((g.cols - 1) * steps + 1);
        const std::size_t colLen = static_cast<std::size_t>((g.rows - 1) * steps + 1);
        const std::size_t runs = static_cast<std::size_t>(g.rows + g.cols);
        const std::size_t est = static_cast<std::size_t>(g.rows) * rowLen
                                + static_cast<std::size_t>(g.cols) * colLen + runs
                                + 2 * (2 * rowLen + 2 * colLen + 4);
        if (est <= budget) break;
        steps /= 2;
    }
    return steps;
}

std::vector<std::vector<common::Vec2>> warpGridLines(const core::WarpGrid& g, std::size_t budget) {
    if (!g.valid()) return {};
    if (g.kind == core::WarpKind::Perspective) return render::warpQuadLines(g);
    return render::warpIsolines(g, warpLineSteps(g, budget));
}

std::vector<std::size_t> warpBoundaryLines(const core::WarpGrid& g, std::size_t lineCount) {
    std::vector<std::size_t> out;
    if (!g.valid() || lineCount == 0) return out;
    if (g.kind == core::WarpKind::Perspective) {
        out.push_back(0); // warpQuadLines puts the closed edge run first
        return out;
    }
    // warpIsolines emits `rows` row lines then `cols` column lines: the outer ring is the first and
    // last of each.
    const std::size_t rows = static_cast<std::size_t>(g.rows);
    const std::size_t cols = static_cast<std::size_t>(g.cols);
    for (const std::size_t i : {std::size_t{0}, rows - 1, rows, rows + cols - 1})
        if (i < lineCount && std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    return out;
}

std::vector<std::vector<common::Vec2>> thickenPolyline(const std::vector<common::Vec2>& pts,
                                                       double offsetPx) {
    if (pts.size() < 2) return {pts};
    std::vector<std::vector<common::Vec2>> out(2);
    out[0].reserve(pts.size());
    out[1].reserve(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        // The local tangent: the segment on each side of the vertex, or the one segment at an end.
        const common::Vec2 a = pts[i == 0 ? 0 : i - 1];
        const common::Vec2 b = pts[i + 1 < pts.size() ? i + 1 : pts.size() - 1];
        common::Vec2 t = b - a;
        const double len = t.length();
        if (len < 1e-9) {
            out[0].push_back(pts[i]);
            out[1].push_back(pts[i]);
            continue;
        }
        t = t * (1.0 / len);
        const common::Vec2 n{-t.y, t.x};
        out[0].push_back(pts[i] + n * offsetPx);
        out[1].push_back(pts[i] - n * offsetPx);
    }
    return out;
}

} // namespace mosaic::ui
