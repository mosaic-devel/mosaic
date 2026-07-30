#pragma once

#include "common/geometry.hpp"

#include <array>
#include <cstdint>
#include <optional>

// The Move tool's transform controls (PLAN S15): handle geometry, hit-testing, and the gesture
// math turning cursor motion into a layer transform. Modelled on Affinity Photo's Move tool —
// click-select, then drag the body to move, a handle to scale, or just outside a corner to
// rotate. Deliberately FLTK-free (unit-tested in tests/test_transform_gesture.cpp);
// VulkanCanvas feeds it pointer events and pushes the result as coalesced SetTransformCommands.
namespace mosaic::ui {

enum class TransformMode : std::uint8_t { None, Move, Scale, Rotate };

struct TransformHit {
    TransformMode mode = TransformMode::None;
    int handle = -1; // Scale only: 0-3 = corners TL,TR,BR,BL; 4-7 = edge mids T,R,B,L
};

// The 8 handle centres of a (possibly rotated) screen-space quad whose corners are given in
// TL,TR,BR,BL order: the corners themselves (0-3) then the edge midpoints (4-7: T,R,B,L).
[[nodiscard]] std::array<common::Vec2, 8>
transformHandleCenters(const std::array<common::Vec2, 4>& corners);

// ---- Rotate-affordance visibility (the fade-in dots, user 2026-07-14) -------------------------
// The rotate hotspots are invisible rings hugging the box corners: findable when the box is a
// plain rectangle, unfindable once a transform has dragged the corners somewhere unexpected -- a
// sheared Move box, or 3D text, whose rotate quad is a projected extent that can float far off the
// visible letters. The corners therefore grow faint DOTS that fade in as the quad gets "wackier",
// to a 50% ceiling (25% first shipped; the user asked for louder): enough to find, quiet enough to
// ignore. Pure maths; the canvas feeds the result to the renderer each frame.

// How far `c` (TL,TR,BR,BL screen corners) has drifted from a plain rectangle, in [0,1]. 0 for ANY
// rotated/scaled rectangle (corner angles are 90 whatever the rotation); rises with corner-angle
// deviation (shear, perspective foreshortening) and with degeneracy (a sliver-thin quad hides its
// corners along a line); 1 for a collapsed quad.
[[nodiscard]] double transformQuadWackiness(const std::array<common::Vec2, 4>& c);

// How far `quad`'s corners sit from `visible`'s, normalized by `quad`'s own diagonal, in [0,1]:
// the "the handles are not where the thing is" term. 0 when they coincide (a flat text box's
// rotate quad IS its edit box); rises as the rotate quad floats away from the chrome the user can
// actually see (3D text anchors its rotate band to the solid's projected extent, which can sit far
// off the visible cap -- the very case that made the handles impossible to find).
[[nodiscard]] double transformQuadMismatch(const std::array<common::Vec2, 4>& quad,
                                           const std::array<common::Vec2, 4>& visible);

// wackiness -> the dots' opacity: 0 at 0 (a plain box shows nothing new), ramping linearly to the
// 0.5 ceiling by wackiness 0.5 (the user's tuning: 25% first, then "make them go up to 50%").
[[nodiscard]] double rotateDotOpacity(double wackiness);

// Hit-test the controls at `screenPt`: handles first (within `handleRadius`), then the rotate
// band (within `rotateBand` of a corner, beyond its handle), then the body (inside the quad =
// Move). nullopt = missed everything (the click falls through to select/deselect).
[[nodiscard]] std::optional<TransformHit>
hitTransformControls(common::Vec2 screenPt, const std::array<common::Vec2, 4>& corners,
                     double handleRadius, double rotateBand);

// The Type-edit box has a DIFFERENT control split from the Move/Shape gizmo, because the box body
// is editable text: the box EDGE moves it (the interior is for the caret/selection, never Move),
// only ONE corner resizes (Area: the frame + reflow; Point: the font size), and a band just
// outside the corners rotates. So the typographic box and the geometric Move tool stay distinct
// (docs/type-tool.md §7): Move stretches, Type sizes. None = the interior (caret) or fully clear
// of the box. Mirrors hitTransformControls' corner order (TL,TR,BR,BL). `resizeCorner` picks which
// corner carries the handle: BR (2, the default) for horizontal text, BL (3) for vertical Point
// text so it stays joined to the left-edge side baseline (VulkanCanvas::textResizeCorner). The
// ResizeBR enum name keeps the common horizontal-case reading.
// Bend is the baseline warp handle (S30 §9), which sits off the box (a gap above the top-edge centre)
// and so is hit-tested separately by the canvas, not by hitTextEditBox -- it is only a state value here.
// PathStart/PathEnd/PathSlide are the fit-to-path range brackets (§9: the start/end arc-distance
// brackets and the centre slide+flip grip); like Bend they are canvas-tested state values only.
enum class TextBoxControl : std::uint8_t {
    None, Move, ResizeBR, Rotate, Bend, PathStart, PathEnd, PathSlide
};

[[nodiscard]] TextBoxControl
hitTextEditBox(common::Vec2 screenPt, const std::array<common::Vec2, 4>& corners,
               double handleRadius, double rotateBand, double edgeBand, int resizeCorner = 2);

class TransformGesture {
public:
    // Arm a gesture at `docPt`: `base` = the layer's transform at press, `content` the
    // layer-local rect the handles frame (the content bbox — NOT the image extent, which is
    // usually document-sized). False (and inactive) for degenerate input: an empty rect, a
    // singular base, or a rotate grab exactly on the pivot.
    //
    // `pivotLocal` is the transform's ANCHOR / reference point in layer-local (content) space (S15+):
    // the point rotation AND scaling turn around, which the user can drag off the box centre
    // (Photoshop-style). nullopt = the auto default: rotation pivots around the content centre and
    // scaling around the handle OPPOSITE the grabbed one (the Affinity default, unchanged). A value
    // makes BOTH rotation and scaling pivot around it (Alt still forces the centre for scale).
    bool begin(TransformMode mode, int handle, common::Vec2 docPt, const common::Affine2D& base,
               const common::Rect& content,
               std::optional<common::Vec2> pivotLocal = std::nullopt);
    void cancel() noexcept { m_mode = TransformMode::None; }
    [[nodiscard]] bool active() const noexcept { return m_mode != TransformMode::None; }
    [[nodiscard]] TransformMode mode() const noexcept { return m_mode; }
    [[nodiscard]] const common::Affine2D& base() const noexcept { return m_base; }

    // The layer transform for the cursor at `docPt`. Move: Shift locks to the dominant axis.
    // Scale: Shift = uniform (keep aspect), Alt = around the centre instead of the opposite
    // handle. Rotate: Shift snaps to 15-degree steps.
    [[nodiscard]] common::Affine2D transformFor(common::Vec2 docPt, bool shiftDown,
                                                bool altDown) const;

private:
    TransformMode m_mode = TransformMode::None;
    int m_handle = -1;
    common::Affine2D m_base;
    common::Affine2D m_baseInv;  // valid while active (begin() rejects singular bases)
    common::Rect m_content;      // the framed rect, layer-local space
    common::Vec2 m_startDoc;     // press point, document space
    common::Vec2 m_startLocal;   // press point, layer-local space
    common::Vec2 m_centerDoc;    // rotate/scale pivot (the anchor, or content centre), document space
    std::optional<common::Vec2> m_pivotLocal; // reference-point override, layer-local; nullopt = auto
    double m_startAngle = 0.0;   // cursor angle around the pivot at press
};

} // namespace mosaic::ui
