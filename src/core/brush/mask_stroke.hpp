#pragma once

#include "common/geometry.hpp"  // common::Vec2
#include "core/selection.hpp"   // the stroke's committed output

#include <cstdint>
#include <vector>

// The coverage-only brush stroke (S18, docs/research-select-brush.md §3.2b): the select brush's
// engine. It runs the same soft-round-tip spacing walk the paint brush uses -- dabCoverage stamped
// every (spacing * diameter) px along the path, flow-gated "over" accumulation building toward 1 --
// but into a bounded float coverage buffer with NO colour composite. The stroke's coverage *is* the
// selection contribution, and there is NO IMAGE ANALYSIS ANYWHERE IN IT -- no pixels read, no edges,
// no region growing. That is a standing constraint on this engine, not merely how it happens to be
// written today: keep it a pure coverage stamper.
//
// It is deliberately the analytic round tip only (Size + Hardness), not the full BrushEngine tip /
// dynamics / spacing-ellipse machinery -- a paint-to-select brush has none of those knobs. FLTK- and
// Vulkan-free, so it is unit-tested headlessly (canned sample stream -> coverage golden), matching
// the engine's test posture.
namespace mosaic::core::brush {

// The select brush's static knobs, read once at begin().
struct MaskStrokeParams {
    double diameter = 24.0; // tip diameter, document px (clamped to a small positive minimum)
    double hardness = 0.8;  // 0 = soft cone from the centre .. 1 = hard edge (still AA'd)
    double flow = 1.0;      // 0..1 coverage deposited per dab (overlapped dabs build toward 1)
    double opacity = 1.0;   // 0..1 cap the whole stroke's contribution can reach, however it crosses
                            // itself (the flow-vs-opacity split, applied when the mask is read out)
    double spacing = 0.10;  // dab interval as a fraction of the diameter (floored at 0.5 px)
};

class MaskStroke {
public:
    // Begin a stroke on a `w`x`h` document, stamping the first dab at `first`. The coverage buffer is
    // grown lazily over the stroke's footprint -- never a document-sized allocation.
    void begin(std::uint32_t w, std::uint32_t h, const MaskStrokeParams& params, common::Vec2 first);

    // Extend to `sample`, stamping dabs every (spacing * diameter) px along the straight segment from
    // the previous sample and carrying the sub-interval remainder across calls -- so the dab pattern
    // is independent of how finely the pointer is sampled.
    void extendTo(common::Vec2 sample);

    // End the stroke (no tail lookahead to flush: the walk stamps each segment as it arrives).
    void end() { m_active = false; }
    [[nodiscard]] bool active() const noexcept { return m_active; }

    // The accumulated per-pixel coverage [0,1] over the bounded working rect (row-major), building
    // toward 1 -- the raw stroke footprint before the opacity cap. Empty until the first dab. Map
    // index i -> document pixel (originX + i%coverageWidth, originY + i/coverageWidth).
    [[nodiscard]] const std::vector<float>& coverage() const noexcept { return m_coverage; }
    [[nodiscard]] std::uint32_t coverageWidth() const noexcept { return m_cw; }
    [[nodiscard]] std::uint32_t coverageHeight() const noexcept { return m_ch; }
    [[nodiscard]] std::int32_t coverageOriginX() const noexcept { return m_ox; }
    [[nodiscard]] std::int32_t coverageOriginY() const noexcept { return m_oy; }
    [[nodiscard]] std::uint32_t width() const noexcept { return m_w; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_h; }

    // The stroke's selection contribution: a document-sized Selection whose coverage is
    // clamp(rawCoverage, 0, 1) * opacity, in the mask's 8-bit AA semantics (§3.2 step 2). A stroke
    // that deposited nothing returns an empty Selection ("no selection"), never an active one.
    [[nodiscard]] Selection toSelection() const;

private:
    void stampDab(common::Vec2 center);
    // Grow the bounded working rect to cover the integer box [bx0,bx1) x [by0,by1) (clamped to the
    // document), tile-aligned so growth is chunky.
    void ensureCovers(int bx0, int by0, int bx1, int by1);

    std::uint32_t m_w = 0; // document dimensions
    std::uint32_t m_h = 0;
    MaskStrokeParams m_params;
    double m_step = 0.5; // spacing walk step in px (spacing * diameter, floored at 0.5)

    // The bounded working rect: coverage sized m_cw x m_ch at document-local origin (m_ox, m_oy).
    std::int32_t m_ox = 0;
    std::int32_t m_oy = 0;
    std::uint32_t m_cw = 0;
    std::uint32_t m_ch = 0;
    std::vector<float> m_coverage;

    common::Vec2 m_last{}; // the previous sample
    double m_carry = 0.0;  // distance travelled since the last dab (the spacing remainder)
    bool m_active = false;
};

} // namespace mosaic::core::brush
