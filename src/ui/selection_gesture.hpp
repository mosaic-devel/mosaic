#pragma once

#include "common/geometry.hpp"
#include "core/selection.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// The S14 marquee/lasso gesture engine: pure state + mask building for the four selection tools
// (rectangle/ellipse marquee, free/polygonal lasso), deliberately FLTK-free so it is unit-tested
// headlessly (tests/test_selection_gesture.cpp). VulkanCanvas owns one and feeds it pointer
// events in *document* coordinates; the live preview goes straight to the canvas mask
// (frame-coalesced, never the command stack -- the S13 part-1 coalescing decision) and the host
// pushes the single SetSelectionCommand with what finish() returns.
namespace mosaic::ui {

// Press-time modifiers choose the boolean op (drag-time modifiers shape the marquee instead,
// Photoshop's disambiguation): Shift = Add, Ctrl = Subtract, Shift+Ctrl or Alt = Intersect.
[[nodiscard]] core::SelectOp selectOpForModifiers(bool shift, bool ctrl, bool alt);

// The axis-aligned marquee rect of a drag: `square` (Shift held during the drag) constrains it
// to a square / circle bound; `fromCenter` (Alt during the drag) grows it around the anchor.
[[nodiscard]] common::Rect marqueeRect(common::Vec2 anchor, common::Vec2 cursor, bool square,
                                       bool fromCenter);

// Centripetal Catmull-Rom smoothing of an OPEN polyline: the curve passes through every input point
// but, being centripetal (knots ~ sqrt(chord)), never loops or overshoots between unevenly spaced
// samples the way uniform Catmull-Rom does. Emits a point roughly every `sampleSpacing` doc px ALONG
// the curve, so long (fast-drag) segments are subdivided enough to stay smooth. Inputs with fewer
// than 3 points pass through unchanged. Used for the freehand-lasso smoothing toggle. Pure; tested.
[[nodiscard]] std::vector<common::Vec2> catmullRomSmooth(const std::vector<common::Vec2>& pts,
                                                         double sampleSpacing);

// Laplacian (neighbour-average) smoothing of an open polyline: nudges each interior point a fraction
// `lambda` (0..1) toward the midpoint of its neighbours, `iterations` times, endpoints pinned. Pulls
// the hand-tremor jitter OUT of a freehand path before the spline fit, so the smoothed curve reads
// clean instead of waving through every jittered sample (an interpolating spline alone traces them).
// Inputs with fewer than 3 points pass through unchanged. Pure; unit-tested.
[[nodiscard]] std::vector<common::Vec2> laplacianSmooth(const std::vector<common::Vec2>& pts,
                                                        int iterations, double lambda);

class SelectionGesture {
public:
    enum class Kind : std::uint8_t { Rect, Ellipse, FreeLasso, PolyLasso };
    enum class Phase : std::uint8_t {
        Idle,
        Dragging, // rect/ellipse sizing, or the free lasso following the pointer
        Placing,  // polygonal lasso between clicks (rubber-band to the cursor)
    };

    [[nodiscard]] Phase phase() const noexcept { return m_phase; }
    [[nodiscard]] Kind kind() const noexcept { return m_kind; }
    [[nodiscard]] bool active() const noexcept { return m_phase != Phase::Idle; }
    [[nodiscard]] core::SelectOp op() const noexcept { return m_op; }
    [[nodiscard]] const std::vector<common::Vec2>& points() const noexcept { return m_points; }
    // The live corner / rubber-band end (doc px) -- the polygonal lasso's open segment to the cursor.
    [[nodiscard]] common::Vec2 cursor() const noexcept { return m_cursor; }

    // Freehand-lasso smoothing (Tools/Lasso setting): when on, the FreeLasso's hand-drawn path is
    // Catmull-Rom rounded before it becomes the preview line + committed mask. Off by default; only
    // the FreeLasso is affected (the polygonal lasso's straight segments are intentional). Set by the
    // canvas from the Settings toggle.
    void setSmoothing(bool on) noexcept {
        if (on != m_smooth) {
            m_smooth = on;
            m_previewDirty = true; // a live lasso rebuilds its preview next frame
        }
    }
    [[nodiscard]] bool smoothing() const noexcept { return m_smooth; }
    // The path that drives the lasso preview line + the committed polygon: the smoothed m_points for a
    // FreeLasso while smoothing is on, otherwise m_points verbatim. Callers add any open rubber-band
    // segment to the cursor themselves (PolyLasso, which is never smoothed).
    [[nodiscard]] std::vector<common::Vec2> pathPoints() const;

    // ---- rect / ellipse / free lasso (press-drag-release) ----
    // `shiftAtPress`/`altAtPress`: which shaping modifiers were already down at the press,
    // choosing the op. A modifier that chose the op is *spent* -- it does not also shape the
    // drag until it is released and pressed again mid-drag (else Shift-to-Add would force every
    // added marquee square). Photoshop's re-arm rule.
    void beginDrag(Kind kind, core::SelectOp op, common::Vec2 docPt, bool shiftAtPress = false,
                   bool altAtPress = false);
    // Rect/ellipse: move the live corner; `shiftDown` = square/circle, `altDown` = around the
    // anchor (both subject to the re-arm rule above). Free lasso: append the point (decimated
    // against the previous sample).
    void dragTo(common::Vec2 docPt, bool shiftDown, bool altDown);

    // ---- polygonal lasso (click - click - ... - close) ----
    void beginPoly(core::SelectOp op, common::Vec2 docPt);
    // Add / preview the next vertex. `shiftDown` constrains the new segment's angle to 5 degree
    // increments relative to the previous vertex (the polygon-tool convention), like dragTo's Shift.
    void addVertex(common::Vec2 docPt, bool shiftDown = false);
    void moveTo(common::Vec2 docPt, bool shiftDown = false); // rubber-band cursor (between clicks)
    // True when a click at docPt should close the polygon instead of adding a vertex: within
    // `closeRadius` (doc px -- the caller converts a screen tolerance through the zoom) of the
    // first vertex, or a double-click beside the just-placed last one.
    [[nodiscard]] bool shouldClose(common::Vec2 docPt, double closeRadius,
                                   bool isDoubleClick) const;

    // Commit: the gesture's shape combined onto `base` by the press-time op, and the gesture
    // reset to Idle. A degenerate shape (a plain click / fewer than 3 lasso points) deselects
    // for a Replace gesture on a non-empty base (the Photoshop click-away) and is nothing
    // otherwise; a result that selects no pixels lands as "no selection" rather than an
    // all-zero mask (which would actively block every edit). nullopt = nothing to push.
    [[nodiscard]] std::optional<core::Selection> finish(const core::Selection& base,
                                                        std::uint32_t docW, std::uint32_t docH);
    void cancel();

    // ---- live preview (canvas mask only, never the command stack) ----
    // Marquees preview the *combined* result (base op shape) so Subtract/Intersect read live;
    // lassos preview their path as a stroked mask over `base` -- the ants present pass renders
    // a thin region as a crawling dashed line, i.e. a rubber band for free. `strokeDocPx` is the
    // stroke thickness in document px: pass ~1/zoom so the band stays ~1 *screen* px when zoomed
    // out (the ants' bilinear >= 0.5 test would otherwise skip over a sub-pixel line).
    [[nodiscard]] core::Selection preview(const core::Selection& base, std::uint32_t docW,
                                          std::uint32_t docH, double strokeDocPx = 1.0) const;
    // Events mark the preview dirty; the canvas frame loop consumes the flag, so it is rebuilt
    // at most once per rendered frame (the same coalescing as the recomposite path).
    [[nodiscard]] bool previewDirty() const noexcept { return m_previewDirty; }
    void clearPreviewDirty() noexcept { m_previewDirty = false; }

private:
    [[nodiscard]] core::Selection shapeMask(std::uint32_t docW, std::uint32_t docH) const;
    [[nodiscard]] bool degenerate() const; // a plain click / too few points to enclose anything

    Phase m_phase = Phase::Idle;
    Kind m_kind = Kind::Rect;
    core::SelectOp m_op = core::SelectOp::Replace;
    common::Vec2 m_anchor;              // press point (rect/ellipse)
    common::Vec2 m_cursor;              // current corner / rubber-band end
    bool m_constrain = false;           // Shift during the drag: square / circle
    bool m_fromCenter = false;          // Alt during the drag: grow around the anchor
    bool m_shiftArmed = true;           // false while a press-time Shift is still "spent"
    bool m_altArmed = true;             // false while a press-time Alt is still "spent"
    std::vector<common::Vec2> m_points; // lasso path / polygon vertices
    bool m_previewDirty = false;
    bool m_smooth = false;              // Tools/Lasso: Catmull-Rom-round the freehand path (see above)
};

// S16-i: moving an existing selection's OUTLINE -- the mask, not the pixels underneath (that is the
// Move tool). With a marquee/lasso tool active, a MODIFIER-FREE press inside the ants grabs the
// selection and the drag translates it; arrow keys nudge it by whole document pixels. The press-time
// modifiers keep their S14 meaning (they choose a boolean op), so holding one starts a new marquee
// *inside* the old selection instead -- Photoshop's disambiguation, and the reason the grab needs no
// modifier of its own.
//
// The gesture keeps a copy of the press-time mask and always translates THAT by the accumulated
// offset, never the previous result: translation clips at the document edge (the mask is
// document-sized), so re-translating a result would erode it. Dragging out of the canvas and back is
// therefore lossless within one gesture, while the committed mask stops honestly at the edge.
//
// A move never combines: it replaces the mask with its own translation. Pure state, FLTK-free,
// unit-tested in tests/test_selection_gesture.cpp.
class SelectionMoveGesture {
public:
    // Grab `base` (must be non-empty) at document point `anchor` -- a pointer drag.
    void begin(core::Selection base, common::Vec2 anchor);
    // Grab `base` for a keyboard nudge session (no anchor; drive it with nudge(), not dragTo()).
    void beginNudge(core::Selection base);

    [[nodiscard]] bool active() const noexcept { return m_active; }
    [[nodiscard]] bool dragging() const noexcept { return m_active && m_hasAnchor; }
    // Whole document pixels the mask has travelled from its press-time place.
    [[nodiscard]] long offsetX() const noexcept { return m_dx; }
    [[nodiscard]] long offsetY() const noexcept { return m_dy; }
    [[nodiscard]] bool moved() const noexcept { return m_dx != 0 || m_dy != 0; }

    // Pointer reached `docPt`: re-derive the integer offset from the anchor (round to nearest, so
    // the mask lands on whole pixels and the ants never shimmer). True when the offset CHANGED --
    // sub-pixel motion costs no mask rebuild.
    bool dragTo(common::Vec2 docPt);
    // Keyboard nudge, accumulating onto the session's offset.
    void nudge(long dx, long dy);

    // The press-time base translated by the accumulated offset: both the live preview and, at
    // finish(), the committed mask. Empty when the move cleared the document.
    [[nodiscard]] core::Selection current() const;

    // End the gesture and hand back what to commit. nullopt when nothing moved -- a plain click
    // inside the ants must not push an undo step.
    [[nodiscard]] std::optional<core::Selection> finish();
    void cancel();

private:
    bool m_active = false;
    bool m_hasAnchor = false; // pointer drag (true) vs keyboard nudge session (false)
    core::Selection m_base;
    common::Vec2 m_anchor{0.0, 0.0};
    long m_dx = 0;
    long m_dy = 0;
};

} // namespace mosaic::ui
