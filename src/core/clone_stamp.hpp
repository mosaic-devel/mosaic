#pragma once

#include "common/geometry.hpp"
#include "common/image.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

// The Stamp / Clone tool's pure maths (PLAN S38, docs/clone-stamp.md). FLTK-free, render-free and
// document-free, so it is unit-tested headlessly; ui/clone_stamp_gesture.hpp turns the options bar
// into these values and ui::VulkanCanvas drives the stroke.
//
// ⚠⚠ A CLONE STAMP COPIES PIXELS. That is the whole of the model and it is a compliance posture as
// much as a design one (docs/clone-stamp.md §7): the deposit is a plain source-over of the SOURCE
// pixel onto the PRE-STROKE destination pixel through the brush tip's alpha. There is no
// gradient-domain / Poisson step, no texture synthesis, no statistic of the destination, and
// nothing anywhere that "makes the patch match where it lands". Healing is S39's problem and it
// lives behind its own fences; if a future reader is tempted to add a "blend the seam" pass here,
// the answer is no.
namespace mosaic::core {

struct StrokeConfinement; // core/stroke_confinement.hpp -- the active selection on the stroke's grid

// Which pixels a clone stroke reads (docs/clone-stamp.md §4).
enum class CloneSampleSource : std::uint8_t {
    // The active raster layer's own PRE-STROKE pixels, in its own grid. The default, and the only
    // mode whose source needs no compositing at all.
    CurrentLayer,
    // The composited backdrop of the active layer and everything that draws BELOW it -- a
    // document-space snapshot taken once, at the press.
    CurrentAndBelow,
    // The whole document's composite, likewise document-space and likewise taken once.
    AllLayers,
};

// The tool's persistent source state (docs/clone-stamp.md §3). It outlives a stroke -- that is the
// point of "aligned" -- and belongs to the tool, not to the document.
struct CloneAnchorState {
    bool hasAnchor = false;
    common::Vec2 anchor{}; // the Ctrl-clicked SOURCE point, document px
    // The latched aligned offset: `firstStrokePoint - anchor`, in document px, fixed by the first
    // stroke laid after the anchor was set and kept by every stroke after it. Meaningless in
    // non-aligned mode, which re-derives the offset per stroke.
    bool hasOffset = false;
    common::Vec2 offset{};
};

// Set (or move) the source anchor. This DROPS any latched aligned offset, so the next stroke
// re-derives it: re-picking a source is the one gesture whose whole purpose is to say "clone from
// here instead", and keeping the old offset would silently ignore the click.
void setCloneAnchor(CloneAnchorState& s, common::Vec2 anchorDoc) noexcept;

// The offset a stroke beginning at `strokeStartDoc` clones with -- `target - source` in document
// px, so a target point `p` reads the source at `p - offset`. Advances `s` for the next stroke.
//
//   * ALIGNED (the default): the offset is latched by the FIRST stroke after the anchor is set and
//     every later stroke keeps it, so a subject painted over several strokes stays in one piece --
//     the source travels with the brush across strokes as well as within one.
//   * NON-ALIGNED: every stroke re-derives `strokeStart - anchor`, so each stroke starts stamping
//     at the anchor itself. Repeated stamps of the same thing, deliberately.
//
// nullopt when no anchor has ever been set: there is nothing to clone, and the caller must refuse
// the stroke rather than invent a source.
[[nodiscard]] std::optional<common::Vec2> cloneStrokeOffset(CloneAnchorState& s,
                                                            common::Vec2 strokeStartDoc,
                                                            bool aligned) noexcept;

// True when `t` carries every pixel CENTRE onto another pixel centre by a whole-pixel shift: the
// linear part is exactly the identity and both translations are whole numbers (within a tolerance
// far tighter than a pixel, so an offset that arrived through a couple of affine multiplies still
// counts).
//
// ⚠ This is what keeps a clone stamp an exact COPY. The overwhelmingly common case -- an
// untransformed layer, sampling itself, at a whole-pixel offset -- must move bytes, not resample
// them: a bilinear read at exactly integer coordinates is *algebraically* the same pixel but
// numerically a four-tap round trip through floats, and running a clone over the same region twice
// would then soften it a little each time. So the sampler branches on this once per stroke.
[[nodiscard]] bool isWholePixelShift(const common::Affine2D& t) noexcept;

// Read `img` at SOURCE-PIXEL coordinates, in the pixel-centre convention: integer pixel (i, j)
// has its centre at (i + 0.5, j + 0.5). Outside the image the answer is fully transparent -- a
// clone whose source hangs off the edge deposits nothing there, which is the honest reading of
// "there are no pixels to copy" and matches what an empty layer region already looks like.
//
// `bilinear` false reads the pixel the coordinate falls inside (a floor), which is exact for the
// whole-pixel-shift case above. `bilinear` true reads the usual 2x2 tap on STRAIGHT alpha,
// weighting colour by the neighbours' alpha so a sample beside a transparent pixel does not drag
// that pixel's stale RGB into the result.
[[nodiscard]] common::Color8 sampleClone(const common::Image& img, double sx, double sy,
                                         bool bilinear) noexcept;

// Everything one clone composite pass needs. All the buffers are borrowed; nothing is owned here.
struct CloneStampInput {
    // The live target pixels, written in place.
    common::Image* target = nullptr;
    // The target's PRE-STROKE pixels, same dimensions as `target`.
    //
    // ⚠⚠ THE COMPOSITE READS THIS, NEVER `target` (docs/brushes.md §6.6b, the DabSource rule). A
    // dab that lands where an earlier dab of the same stroke already stamped must still deposit
    // onto pristine pixels, or the mark stops being a function of (params, samples): it would
    // depend on how often composite() ran, which is a frame-rate question -- and that breaks
    // goldens, undo replay and the incremental-refresh contract in one go. Reading the snapshot
    // also makes this pass IDEMPOTENT per pixel, which is what lets the canvas re-run it over a
    // rectangle it has already written.
    const common::Image* base = nullptr;
    // The pixels the stroke deposits: the layer's own pre-stroke image, or a document-space
    // composite snapshot, per CloneSampleSource.
    const common::Image* source = nullptr;
    // Target-layer pixel coordinates -> `source` pixel coordinates, with the clone offset already
    // folded in. For the CurrentLayer mode this is a pure translation; for the backdrop modes it
    // additionally carries the layer's world transform, so a rotated or scaled layer clones from
    // the document the user is actually looking at.
    common::Affine2D targetToSource;
    bool bilinear = false; // !isWholePixelShift(targetToSource)

    // The stroke's accumulated coverage, over the engine's bounded working rect (its
    // coverage()/coverageOrigin*/coverage{Width,Height} accessors, verbatim).
    const float* coverage = nullptr;
    std::int32_t covX = 0;
    std::int32_t covY = 0;
    std::uint32_t covW = 0;
    std::uint32_t covH = 0;

    // The stroke's ceiling, [0,1] -- BrushEngine's `m_cap` for a stroke whose colour is opaque,
    // which the clone stroke's always is. Coverage times this is exactly the alpha the engine's own
    // composite() would have laid, which is why the two agree pixel for pixel about WHERE the
    // stroke is even though they disagree about what it deposits.
    double opacity = 1.0;
    // The active selection on the target's grid, or null for "no selection" (S13 semantics: the
    // whole document is editable). Applied exactly as the engine applies it -- a coverage multiply,
    // never a clip -- so a feathered selection takes its proportion of the stamp.
    const StrokeConfinement* confine = nullptr;
};

// Composite the clone over the integer half-open rectangle [x0,x1) x [y0,y1) of the target:
//
//     alpha = coverage * opacity * confine        (skipped where that is 0)
//     out   = over(source(targetToSource * p), base(p), alpha)
//
// straight-alpha source-over, the same expression BrushEngine::composite() runs. The rectangle is
// clamped here to the target, the coverage window and nothing else; a caller that hands over the
// rect the engine just composited therefore rewrites exactly the pixels the engine just wrote, and
// no others -- which is what makes the engine's own restore() put a cancelled clone stroke back
// byte for byte.
//
// Returns the number of pixels written (0 for a malformed input), which the tests read.
std::size_t applyCloneStamp(const CloneStampInput& in, int x0, int y0, int x1, int y1) noexcept;

} // namespace mosaic::core
