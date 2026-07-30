#pragma once

#include "core/brush/bitmap_tip.hpp"
#include "core/brush/dab_mask.hpp"
#include "core/brush/mask_generator.hpp"

#include <cstdint>
#include <memory>
#include <variant>

// The tip a stroke stamps (docs/brushes.md §6.2's `BrushTip`): the six procedural generators, or a
// decoded bitmap tip. Everything under it -- MaskGenerator, BitmapTip, DabMask, DabMaskCache -- was
// built and tested in Arc A; this is the seam that hands one of them to the engine.
//
// Two things it deliberately is NOT:
//
//   * It carries no SIZE. A tip is a shape and a falloff; the dab supplies the diameter, the ratio,
//     the angle and the live softness, and every one of those can change per dab under the option
//     pipeline. A `MaskGeneratorParams` does carry a diameter -- it is the generator's own final
//     geometry -- so `renderTipMask` builds the generator the DAB asks for rather than reusing an
//     authored one. That is exactly what dab_mask.hpp's `renderDabMask(gen, ...)` means by taking no
//     size: the generator IS the size.
//
//   * It carries no colour. `BitmapTip` stores 8-bit COVERAGE planes and nothing else, whatever its
//     `application` says -- so an ImageStamp tip stamps its shape here and deposits the stroke's
//     colour, not the tip's pixels. Per-pixel dab colour is a `Colored` accumulator that does not
//     exist yet (the accumulator's colour hook is per DAB), and the importer records the loss.
//
// `id` identifies a tip's RASTER, for the dab cache (dab_cache.hpp): everything baked into a
// BitmapTip's coverage planes at construction -- the frames, the application, the three adjustments
// -- is invisible to a `DabKey`, so two different rasters must never share an id. `makeTip` hands
// out a fresh one per constructed tip, which is the safe direction: a needless miss, never a lie.
//
// FLTK-, Vulkan- and platform-free.
namespace mosaic::core::brush {

struct BrushTip {
    // Exactly one kind. A default-constructed BrushTip is the default procedural circle -- but the
    // engine's `BrushParams::tip` is a NULL pointer by default, which is a different thing: no tip
    // at all, i.e. the built-in analytic circle the engine has stamped since S19-a.
    std::variant<MaskGeneratorParams, std::shared_ptr<const BitmapTip>> shape;
    std::uint64_t id = 0;

    [[nodiscard]] bool isProcedural() const noexcept { return shape.index() == 0; }
    // Null unless this is a bitmap tip.
    [[nodiscard]] const BitmapTip* bitmap() const noexcept {
        const auto* p = std::get_if<std::shared_ptr<const BitmapTip>>(&shape);
        return p != nullptr ? p->get() : nullptr;
    }
    // The procedural generator's authored parameters. Its `diameter`, `ratio` and `softness` are
    // NOT what a dab is stamped at -- see the header comment.
    [[nodiscard]] const MaskGeneratorParams* generator() const noexcept {
        return std::get_if<MaskGeneratorParams>(&shape);
    }
};

// A fresh raster id, monotonically increasing across the process. Never reused, so a tip built after
// an edit can never be served the old tip's masks.
[[nodiscard]] std::uint64_t nextTipId() noexcept;

// The two constructors. Each mints a fresh `id`.
[[nodiscard]] std::shared_ptr<const BrushTip> makeTip(MaskGeneratorParams generator);
[[nodiscard]] std::shared_ptr<const BrushTip> makeTip(std::shared_ptr<const BitmapTip> bitmap);

// The dab this tip paints at `diameter` (its LONG axis, §3.5) squashed by `ratio`. A procedural tip
// fills the ellipse exactly; a bitmap tip preserves its frame's own aspect inside it, so its two
// extents are the frame's, scaled -- which is why the frame has to be known before the shape is.
[[nodiscard]] DabShape tipDabShape(const BrushTip& tip, int frame, double diameter, double ratio,
                                   double angleRad, bool mirrorH, bool mirrorV) noexcept;

// Rasterize the tip at exactly `shape` (which the caller took from the dab cache's KEY, never from
// its own request -- that is what makes the cache transparent) and the quantized sub-pixel phase.
//
// `softness` is the live Softness option's value, 1 = as authored. It reaches a procedural tip and
// is ignored by a bitmap one -- faithfully: the option scales a mask generator's fade coefficients,
// and there is nothing in a decoded raster for it to scale.
[[nodiscard]] DabMask renderTipMask(const BrushTip& tip, int frame, const DabShape& shape,
                                    double softness, double subX, double subY);

} // namespace mosaic::core::brush
