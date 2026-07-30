#pragma once

#include "common/geometry.hpp"

#include <array>
#include <cstdint>

// The Crop tool's rect math (PLAN S16): the staged crop rectangle is an axis-aligned rect in
// DOCUMENT space, and this module turns cursor motion into rect edits — drawing a fresh rect,
// moving it, or resizing it by the 8 handles (the S15 handle indexing: 0-3 corners TL,TR,BR,BL;
// 4-7 edge mids T,R,B,L) — with the aspect-ratio presets and Shift/Alt constraints.
// Deliberately FLTK-free (unit-tested in tests/test_crop_gesture.cpp); VulkanCanvas feeds it
// pointer events and hands the applied rect to the host, which lands it as
// render::buildCropCommand's single undo step.
//
// S16-f: the rect is NOT clamped to the document — reaching past an edge stages a canvas
// EXPANSION (buildCropCommand grows the canvas and fills the added area). Two things keep that
// workable: a snap band (`snapTol`, zoom-aware, passed by the canvas) pulls edges that land
// near a canvas edge exactly onto it, so plain crops stay effortless; and everything is bounded
// by a safety envelope (the canvas outset by kCropOutsetFactor x its larger dimension per
// side) so a runaway drag cannot stage an absurd canvas.
namespace mosaic::ui {

enum class CropMode : std::uint8_t { None, Draw, Move, Resize };

class CropGesture {
public:
    // Arm a gesture at `docPt` on `rect` (the staged crop rect; for Draw it is only the
    // Esc-restore base — the new rect anchors at the press point). False for a Resize with no
    // valid handle.
    bool begin(CropMode mode, int handle, common::Vec2 docPt, const common::Rect& rect);
    void cancel() noexcept { m_mode = CropMode::None; }
    [[nodiscard]] bool active() const noexcept { return m_mode != CropMode::None; }
    [[nodiscard]] CropMode mode() const noexcept { return m_mode; }
    [[nodiscard]] const common::Rect& base() const noexcept { return m_base; }

    // The crop rect for the cursor at `docPt`, kept at least ~1px and inside the safety
    // envelope (see the header note) — beyond the `docW`x`docH` document is allowed and stages
    // an expansion. Edges landing within `snapTol` document units of a canvas edge snap onto it
    // (0 disables). `ratio` is the constrained aspect (w/h; <= 0 = free). Modifiers follow the
    // marquee/Move conventions: Move locks to the dominant axis on Shift; Draw/Resize take
    // Shift = keep aspect (square for a fresh Draw, the rect's own for Resize) and Alt = resize
    // around the centre instead of the anchor.
    [[nodiscard]] common::Rect rectFor(common::Vec2 docPt, double ratio, bool shift, bool alt,
                                       double docW, double docH, double snapTol = 0.0) const;

private:
    CropMode m_mode = CropMode::None;
    int m_handle = -1;       // Resize only
    common::Rect m_base;     // the rect at press (Move/Resize work from it; Esc restores it)
    common::Vec2 m_startDoc; // press point, document space (Draw's anchor)
};

// ---- Crop rotation (S16-f rotate) -----------------------------------------------------------
// The staged crop box = an axis-aligned `rect` expressed in a ROTATED FRAME: the document plane
// rotated by `angle` radians about the fixed pivot `pivot`. With a FIXED pivot, motion composes
// exactly: moving the rect by d in frame coordinates moves the box by R(angle)·d in the document
// — which is the cursor's own motion mapped through docToCropFrame, so gestures stay
// cursor-following. (Re-anchoring the pivot per gesture would rotate the deltas twice.) The
// pivot is only REBASED between gestures (see the canvas), which changes the representation,
// never the box.
[[nodiscard]] common::Vec2 cropFrameToDoc(common::Vec2 p, double angle, common::Vec2 pivot);
[[nodiscard]] common::Vec2 docToCropFrame(common::Vec2 p, double angle, common::Vec2 pivot);
// The box's document-space corners, TL,TR,BR,BL of the frame rect mapped through the frame.
[[nodiscard]] std::array<common::Vec2, 4> cropBoxCorners(const common::Rect& r, double angle,
                                                         common::Vec2 pivot);

// The Ratio combo's "Custom" entry index (the last preset + 1): its aspect comes from the
// ratioW/ratioH Number fields, so the host computes it directly rather than via the table below.
inline constexpr int kCropRatioCustom = 6;

// Map the Crop tool's Ratio option (0 Free, 1 Original, 2 "1:1", 3 "4:3", 4 "16:9", 5 "3:2")
// to a w/h aspect (0 = unconstrained). `swap` flips to the portrait orientation. The "Custom"
// index (kCropRatioCustom) is NOT handled here -- it has no fixed ratio; the caller reads the
// ratioW/ratioH fields. Passing it returns 0 (free) as a safe fallback.
[[nodiscard]] double cropRatioForOptions(int choiceIndex, bool swap, double docW, double docH);

// The w/h aspect for the "Custom" Ratio entry, from the ratioW:ratioH fields. A non-positive
// component (either field empty/zero) yields 0 (free, unconstrained); `swap` flips orientation
// like the presets.
[[nodiscard]] double customCropRatio(double ratioW, double ratioH, bool swap);

// Re-fit a staged rect to a newly selected aspect `ratio`: keep its centre and area, then
// shift/shrink as needed to stay inside the document — or, for a rect already staging an
// expansion (any part outside the canvas), inside the safety envelope, so a ratio change never
// silently discards the staged outset. A no-op for a free ratio.
[[nodiscard]] common::Rect conformCropRect(const common::Rect& r, double ratio, double docW,
                                           double docH);

// The safety envelope's per-side outset in canvas units: how far beyond the canvas a staged
// rect (and so an expansion) may reach. Shared by the gesture, conform and snap so no stage
// can undo another's bound.
inline constexpr double kCropOutsetFactor = 2.0;

// The integer document-pixel rect a staged (double) rect crops to: edges rounded to the
// nearest pixel boundary, clamped to the safety envelope (NOT the document — beyond-canvas
// rects stage an expansion), at least 1x1 (w/h are 0 only when the document itself is empty).
struct CropPixels {
    long x = 0;
    long y = 0;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    friend bool operator==(const CropPixels&, const CropPixels&) = default;
};
[[nodiscard]] CropPixels snapCropRect(const common::Rect& r, std::uint32_t docW,
                                      std::uint32_t docH);

} // namespace mosaic::ui
