#pragma once

#include <vector>

#include "common/geometry.hpp"
#include "core/clone_stamp.hpp" // core::CloneSampleSource

// Stamp / Clone tool gesture maths (PLAN S38, docs/clone-stamp.md §3) -- the pure, FLTK-free half
// that turns the options bar into the values the stroke runs with, and builds the source marker's
// geometry. Kept out of the canvas so it is unit-tested headlessly, exactly like crop_gesture /
// red_eye_gesture / shape_gesture; VulkanCanvas owns the pointer side and app_window the document
// side (the sample-source snapshot).
namespace mosaic::ui {

// A flat snapshot of the clone tool's options, read off the bar by the canvas so the maths never
// touches the ToolManager (the ShapeOptions / RedEyeOptions pattern).
struct CloneStampOptions {
    double size = 40.0;     // tip diameter, document px
    double hardness = 80.0; // 0..100
    double flow = 100.0;    // 0..100
    double opacity = 100.0; // 0..100
    double spacing = 10.0;  // 0..200, percent of the tip's extents
    bool aligned = true;
    core::CloneSampleSource sample = core::CloneSampleSource::CurrentLayer;
};

// The "Sample" choice's index -> the sampling mode. The order is the options-bar list's, and this
// is the one place the two are tied together.
[[nodiscard]] core::CloneSampleSource cloneSampleForChoice(int index) noexcept;

// The source marker's outline, in whatever space `center` and `radius` are given in (the canvas
// hands over LOGICAL screen px, like every other overlay-line consumer). One polyline, drawn as a
// circle with an inscribed diamond: distinct at a glance from the brush's own size ring, and --
// deliberately -- it never retraces a segment, because a marker that draws a line twice reads as a
// different weight wherever it doubles.
//
// `radius` is clamped to something visible; a marker that shrinks with the zoom would vanish
// exactly when a user is working close in, so it is a FIXED screen size, like the transform anchor.
[[nodiscard]] std::vector<common::Vec2> cloneMarkerPolyline(common::Vec2 center, double radius);

// The marker's on-screen radius, logical px. Sized against the transform anchor and the pen's node
// dots so the three read as one family.
inline constexpr double kCloneMarkerRadius = 9.0;

} // namespace mosaic::ui
