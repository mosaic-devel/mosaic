#pragma once

#include "core/selection.hpp"

#include <cstdint>

// Outpaint (canvas-expansion) detection, shared by the inpaint backends (S16-f follow-up,
// docs/inpainting-research.md §3.7.8). Filling OUTWARD — a hole that is a border ring or strip
// added around the picture — is a different problem from filling a hole inside it: the engines'
// default behaviour imports whole distant structures (offset statistics duplicated the Broadway
// tower wholesale; per-pixel synthesis floated grass shelves into the sky). Backends gate their
// outpaint-specific tuning on this test so INTERIOR heals keep their exact historical
// behaviour, byte for byte. The gate benefits the manual workflow too (expand transparent, then
// heal the ring by hand) — it reads only the hole's geometry, not who created it.
namespace mosaic::core::inpaint {

// Fraction of the image frame's perimeter pixels (the outermost row/column ring) covered by the
// hole. A canvas-expansion ring scores ~1, a one-sided expansion strip ~0.25-0.5, an interior
// heal 0, and an ordinary heal that merely touches an edge stays well below the gate.
[[nodiscard]] double holeFrameFraction(const Selection& holeMask, std::uint32_t width,
                                       std::uint32_t height);

// The gate: treat the fill as an outpaint when at least this share of the frame is hole. A full
// one-side strip on a 3:2 canvas is ~0.2 of the perimeter, so 0.25 requires more than one side
// (or one side plus corners) — ordinary edge-touching heals never trip it.
inline constexpr double kOutpaintFrameFraction = 0.25;

[[nodiscard]] inline bool isOutpaintHole(const Selection& holeMask, std::uint32_t width,
                                         std::uint32_t height) {
    return holeFrameFraction(holeMask, width, height) >= kOutpaintFrameFraction;
}

} // namespace mosaic::core::inpaint
