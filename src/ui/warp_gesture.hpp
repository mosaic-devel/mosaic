#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "common/geometry.hpp"
#include "core/layer.hpp"     // core::WarpGrid / core::WarpKind
#include "render/render.hpp"  // render::ResampleFilter

// Mesh Warp / Perspective Warp gesture maths (PLAN S35-b, docs/warp-tools.md) -- the pure, FLTK-free
// half: the options bar turned into values, the handle hit-test, what a drag does to the lattice, and
// the on-canvas grid's geometry. Kept out of the canvas so it is unit-tested headlessly, exactly like
// crop_gesture / clone_stamp_gesture / shape_gesture; VulkanCanvas owns the pointer side and
// app_window the document side.
namespace mosaic::ui {

// A flat snapshot of the warp tool's options, read off the bar by the canvas so the maths never
// touches the ToolManager (the CloneStampOptions / ShapeOptions pattern).
struct WarpOptions {
    int cols = 4;
    int rows = 4;
    render::ResampleFilter quality = render::ResampleFilter::Auto;
    bool showGrid = true;
};

// The Rows / Columns bounds the bar publishes and the gesture clamps to. The ceiling is a real
// budget, not taste: the overlay draws every isoline through the shared content-keyed polyline lane,
// whose SSBO holds render::kLassoMaxVerts vertices, and a lattice past this cannot be drawn honestly
// at a useful subdivision. It is also well past what a hand-authored mesh is usable at.
inline constexpr int kWarpMinNodes = 2;
inline constexpr int kWarpMaxNodes = 12;

// The Quality choice's index -> the kernel. The order is the options-bar list's, which is the Move
// tool's "Anti-aliasing" list verbatim (MainWindow::currentResampleFilter's map) -- one order for the
// whole app, so a user who learns it once has learnt it everywhere.
[[nodiscard]] render::ResampleFilter warpQualityForChoice(int index) noexcept;
// ... and back, for seeding the bar from a value.
[[nodiscard]] int warpQualityChoiceIndex(render::ResampleFilter f) noexcept;

// How far, in SCREEN px, a press may miss a handle and still grab it. Screen px and not document px:
// a document-space radius would make the handles ungrabbable when zoomed out and absurdly sticky when
// zoomed in, which is the same coordinate-level mistake as reading a press in the wrong frame.
inline constexpr double kWarpHandleHitPx = 9.0;

// What a handle drag does. Shift constrains to whichever axis the drag has travelled further along
// (locked at the press, so a diagonal wobble cannot flip it mid-drag); Alt drags the WHOLE lattice
// rigidly, which is how a warped region is repositioned without touching its shape.
enum class WarpDragMode : std::uint8_t { Free, ConstrainAxis, MoveAll };

[[nodiscard]] WarpDragMode warpDragModeFor(bool shift, bool alt) noexcept;

// The index of the handle nearest `screenPt` within `hitPx`, or nullopt. `handles` are in the same
// space as `screenPt`. Nearest wins (not first), so two handles dragged on top of each other still
// resolve to the one under the cursor.
[[nodiscard]] std::optional<int> hitWarpHandle(const std::vector<common::Vec2>& handles,
                                               common::Vec2 screenPt,
                                               double hitPx = kWarpHandleHitPx);

// The grid's handles in the order the overlay draws them and `hitWarpHandle` indexes them:
//   * Mesh -- every node, row-major (so the index IS the lattice index).
//   * Perspective -- the four corners only, row-major (TL, TR, BL, BR): the interior of a homography
//     has no control points, and offering handles that move nothing is worse than offering none.
// Points are in the grid's own space; the canvas maps them to screen.
[[nodiscard]] std::vector<common::Vec2> warpHandlePoints(const core::WarpGrid& g);

// `base` with handle `index` moved so that it sits at `to`. `base` is the grid as it stood at the
// PRESS and `to` the cursor now -- so a drag is always computed from the press-time lattice and can
// never accumulate its own rounding. `pressAt` is where the drag started (the axis lock measures
// against it); MoveAll slides every node by the same delta.
//
// A `index` outside the handle set returns `base` untouched.
[[nodiscard]] core::WarpGrid warpDragged(const core::WarpGrid& base, int index,
                                         common::Vec2 pressAt, common::Vec2 to, WarpDragMode mode);

// The grid's drawn lines, in the grid's own space, as separate polylines:
//   * Mesh -- one per lattice row then one per lattice column, each sampled along the SAME
//     Catmull-Rom surface the pixels are, so the drawn grid bends the way the image does. Straight
//     chords between control points are the amateur tell.
//   * Perspective -- the four straight edges (one closed run) then the two diagonals.
// `budget` caps the total vertex count (render::kLassoMaxVerts): the subdivision is halved until the
// estimate fits, so a big lattice degrades to a coarser CURVE rather than being clipped mid-grid.
[[nodiscard]] std::vector<std::vector<common::Vec2>> warpGridLines(const core::WarpGrid& g,
                                                                   std::size_t budget);

// Which of `warpGridLines`'s runs are the OUTER BOUNDARY (the ones drawn a touch heavier). Mesh: the
// first and last row line, and the first and last column line. Perspective: the closed edge run.
[[nodiscard]] std::vector<std::size_t> warpBoundaryLines(const core::WarpGrid& g,
                                                         std::size_t lineCount);

// The subdivision `warpGridLines` would use for `g` under `budget`. Exposed for the tests, which
// pin that the vertex estimate stays inside the lane's SSBO.
[[nodiscard]] int warpLineSteps(const core::WarpGrid& g, std::size_t budget);

// One polyline doubled either side of its own normal by `offsetPx`, so it draws heavier than its
// neighbours on a lane that has exactly one line weight. Two runs come back (the + and - offsets);
// an under-2-point input comes back as itself.
//
// It is a trick, and an honest one: the shared overlay lane draws a fixed-thickness inverted line and
// the release overlay channel budget is 12, so a second weight cannot be bought with a new binding
// (which this file's own history says is also where a use-after-free lives). Two lines a fraction of
// a pixel apart read as one thicker line, which is exactly what the outer boundary needs.
[[nodiscard]] std::vector<std::vector<common::Vec2>> thickenPolyline(
    const std::vector<common::Vec2>& pts, double offsetPx);

} // namespace mosaic::ui
