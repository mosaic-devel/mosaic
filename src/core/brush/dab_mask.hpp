#pragma once

#include "core/brush/mask_generator.hpp"
#include "core/brush/math_util.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// One stamped dab, rasterized (docs/brushes.md §6.2). A `DabMask` is 8-bit coverage over a tight
// integer box; everything about *where* that box lands is a `DabPlacement`, and the two are computed
// from the same geometry so they cannot drift apart.
//
// The split that makes the dab cache possible: a dab's raster depends only on its SHAPE (extent,
// angle, mirror) and its sub-pixel PHASE -- never on its position. Two dabs 40 px apart with the same
// shape and phase share a mask byte for byte. So the pipeline is
//
//     centre + shape  ->  placeDab()  ->  { integer corner, quantized phase, mask dims }
//                                              |
//                            renderDabMask(tip, shape, phase) -> DabMask     (cacheable)
//
// and the caller blits the mask at the integer corner. The phase is quantized *by placeDab*, and the
// mask is rendered from the quantized phase, so a cache keyed on it is exactly transparent: a hit and
// a miss return the same bytes. (That is a test -- see tests/test_brush_dab_cache.cpp.)
//
// Coverage is 8-bit, not float. Every bitmap tip in every format is 8-bit to begin with, and the
// procedural generators already agree with their reference only to within quantization
// (docs/brushes.md §3.4), so a float mask would store precision that never existed. It also makes the
// cache four times denser.
//
// FLTK-, Vulkan- and platform-free.
namespace mosaic::core::brush {

// A dab wider or taller than this is refused (an empty mask) rather than allocated. Nothing in any
// preset format bounds `scale`, so a hostile file can ask for a 2^31 px dab; this is the backstop.
inline constexpr double kMaxDabExtent = 8192.0;

// Row-major 8-bit coverage. 255 is full paint, 0 is none -- the convention of docs/brushes.md §3.6.1,
// which is NOT the convention a `.png` tip stores its pixels in.
struct DabMask {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> coverage;

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }

    // Unchecked; `x < width && y < height` is the caller's contract.
    [[nodiscard]] std::uint8_t at(std::uint32_t x, std::uint32_t y) const noexcept {
        return coverage[static_cast<std::size_t>(y) * width + x];
    }
};

// A dab's footprint in document px, before rasterization. `width`/`height` are the tip's own extents
// *before* rotation -- for an elliptical tip, its two axes; for a bitmap tip, the size the source
// raster is resampled to. Mirroring flips the tip in its own frame, so it happens before rotation.
struct DabShape {
    double width = 24.0;
    double height = 24.0;
    double angleRad = 0.0; // rotates the tip within the document
    bool mirrorH = false;
    bool mirrorV = false;
};

// The axis-aligned box the rotated tip occupies. Continuous, unrounded, and always >= 0.
struct DabExtent {
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] bool empty() const noexcept { return !(width > 0.0) || !(height > 0.0); }
};
[[nodiscard]] DabExtent dabExtent(const DabShape& shape) noexcept;

// Where a dab centred at `(centerX, centerY)` puts its mask, once the sub-pixel phase is quantized to
// `subPixelSteps` bins per axis (1 disables sub-pixel placement entirely; 4 is the default, a max
// positional error of 1/8 px). `subX`/`subY` are the phase actually used, so a caller that renders
// with them and blits at `(x, y)` reproduces the requested centre to within that error.
struct DabPlacement {
    std::int32_t x = 0; // document coords of mask pixel (0,0)
    std::int32_t y = 0;
    double subX = 0.0; // [0,1), quantized
    double subY = 0.0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
};
[[nodiscard]] DabPlacement placeDab(const DabShape& shape, double centerX, double centerY,
                                    int subPixelSteps) noexcept;

// The shape a procedural generator paints at `angleRad`. A `MaskGenerator` already carries its final
// diameter, ratio and softness -- scaling it is a matter of constructing another one -- which is why
// the analytic `renderDabMask` below takes no size: taking one would let it disagree with `gen`.
[[nodiscard]] DabShape shapeOf(const MaskGenerator& gen, double angleRad, bool mirrorH = false,
                               bool mirrorV = false) noexcept;

// Rasterize a procedural tip at the given angle, mirror and sub-pixel phase. The result's dimensions
// are exactly those `placeDab(shapeOf(gen, angleRad, ...), ..., steps)` reports for the same phase.
[[nodiscard]] DabMask renderDabMask(const MaskGenerator& gen, double angleRad, bool mirrorH,
                                    bool mirrorV, double subX, double subY);

namespace detail {

// Mask dims for an extent offset by a sub-pixel phase: the tip spans [sub, sub + extent) along each
// axis, so it touches `ceil(sub + extent)` pixels. Shared by placeDab and both renderers, because a
// mask that is one pixel narrower than its placement is a buffer overrun in the blit.
[[nodiscard]] std::uint32_t maskSpan(double extent, double sub) noexcept;

// A phase outside [0,1) is not a phase. Both renderers push their argument through this before it
// reaches `maskSpan`, so a mask's dimensions are always consistent with the phase it was drawn at --
// even if a caller (or a dab cache with a differently-quantized key) hands over nonsense. The blit
// must still size itself from `DabMask::width`/`height`, never from the placement it came with.
[[nodiscard]] inline double normalizePhase(double s) noexcept {
    return (s >= 0.0 && s < 1.0) ? s : 0.0; // NaN takes the else branch
}

// Map a mask pixel's centre into the tip's own frame: undo the sub-pixel offset and the centring,
// then the rotation, then the mirror. `cosT`/`sinT` are of the dab's angle.
struct TipFrameMap {
    double halfW = 0.0; // of the ROTATED extent
    double halfH = 0.0;
    double subX = 0.0;
    double subY = 0.0;
    double cosT = 1.0;
    double sinT = 0.0;
    bool mirrorH = false;
    bool mirrorV = false;

    void operator()(std::uint32_t px, std::uint32_t py, double& tipX, double& tipY) const noexcept {
        const double lx = static_cast<double>(px) + 0.5 - subX - halfW;
        const double ly = static_cast<double>(py) + 0.5 - subY - halfH;
        double tx = lx * cosT + ly * sinT; // R(-angle) * local
        double ty = -lx * sinT + ly * cosT;
        if (mirrorH)
            tx = -tx;
        if (mirrorV)
            ty = -ty;
        tipX = tx;
        tipY = ty;
    }
};

// `ext` is passed in rather than recomputed: both renderers already have it, and it costs a sin, a
// cos and two rounds to rebuild once per dab.
[[nodiscard]] TipFrameMap tipFrameMap(const DabShape& shape, const DabExtent& ext, double subX,
                                      double subY) noexcept;

// Coverage in [0,1] -> the stored byte. Rounds to nearest, so 1.0 is 255 and 0.0 is 0.
[[nodiscard]] inline std::uint8_t quantizeCoverage(double c) noexcept {
    return static_cast<std::uint8_t>(clamp01(c) * 255.0 + 0.5);
}

} // namespace detail

} // namespace mosaic::core::brush
