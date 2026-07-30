#pragma once

#include "core/brush/stroke_painter.hpp" // LineClip / LinePixel -- the shared thick-line rasterizer

#include <cstdint>
#include <vector>

// THE HATCHING ENGINE (docs/brushes.md §6.6g), transcribed from the reference's hatching paintop --
// the engine behind the shipped `y)_Screentone_Moire` preset.
//
// ⚠ IT IS NOT A `StrokePainter`, AND ESTABLISHING THAT WAS THE FIRST THING TO DO. §6.6's taxonomy
// puts it in group (c) -- "dab-based, but the dab's content is a canvas-aligned procedural pattern"
// -- and the source agrees: it derives from the reference's BRUSH-BASED paintop base and overrides
// `paintAt`, not `paintLine`. So it rides the dab walk, the spacing cadence and the dab cache
// exactly as a pixel brush does, and the only new thing it needs is what §6.6(c) predicted: a dab
// may source "procedural pattern clipped by the tip mask" rather than only a mask.
//
// The pattern: parallel lines at `angle`, `separation` apart, `thickness` wide, PHASE-LOCKED TO THE
// DOCUMENT (via `origin_x`/`origin_y`) rather than to the dab -- which is what makes two overlapping
// dabs continue one another's lines instead of each starting its own. A second pass at a differing
// angle is what produces the moiré.
//
// FLTK-, Vulkan- and platform-free.
namespace mosaic::core::brush {

// `Hatching/bool_*`, read in the reference reader's own priority order (the first true wins, and
// "no crosshatching" defaults TRUE, so a file that mentions none of them hatches once).
enum class CrosshatchingStyle : std::uint8_t {
    None,
    Perpendicular,
    MinusThenPlus,
    PlusThenMinus,
    Moire,
};

// The static `Hatching/*` block. `enabled` is the engine's gate, exactly as `SmudgeParams::enabled`
// is the smudge walk's.
struct HatchingParams {
    bool enabled = false;
    double angle = -60.0;     // Hatching/angle, DEGREES
    double separation = 6.0;  // Hatching/separation, px between line centres
    double thickness = 1.0;   // Hatching/thickness, px
    double originX = 50.0;    // Hatching/origin_x -- the document point the lattice is locked to
    double originY = 50.0;    // Hatching/origin_y
    CrosshatchingStyle style = CrosshatchingStyle::None;
    int separationIntervals = 2; // Hatching/separationintervals, used only by the Separation option
    bool antialias = false;          // Hatching/bool_antialias
    bool opaqueBackground = false;   // Hatching/bool_opaquebackground -- NOT transcribed; badged
    bool subpixelPrecision = false;  // Hatching/bool_subpixelprecision
};

// The four per-dab option values, evaluated by the engine beside the dab (brush_engine.cpp's
// resolveDab) exactly as the smudge quartet is, plus the three CHECKED gates the reference's own
// branching reads. All four values are size-like, WITH strength.
struct HatchingDabValues {
    double angle = 0.0;
    double crosshatching = 0.0;
    double separation = 0.0;
    double thickness = 1.0;
    bool angleChecked = false;
    bool crosshatchingChecked = false;
    bool separationChecked = false;
};

// The reference's `separationAsFunctionOfParameter`, transcribed: the [0,1] parameter is split into
// `numIntervals` equal buckets and the separation is doubled or halved by whole powers of two around
// the middle bucket. ⚠ Outside 2..7 intervals the reference returns the separation UNCHANGED (with a
// debug complaint), and that pass-through is reproduced rather than clamped. Free + pure.
[[nodiscard]] double hatchSeparationForParameter(double parameter, double separation,
                                                 int numIntervals);

// The reference's `spinAngle`, transcribed: fold `base + spin` into (-90, 90] degrees, keeping the
// SIGN of the unfolded sum rather than of the folded one. ⚠ It adds the preset's own angle itself,
// so a caller that has already added it adds it twice -- which the reference's own Angle-option call
// site does, deliberately or not, and which is reproduced. Free + pure.
[[nodiscard]] double hatchSpinAngle(double baseAngleDeg, double spinDeg);

// Rasterize one dab's worth of the lattice into `out` (row-major 8-bit, `width` x `height`), for a
// dab whose mask's top-left pixel is at document `(dabX, dabY)`. `out` is resized and cleared.
//
// The lattice's phase comes from the DOCUMENT origin, so this is a pure function of the dab's
// document position and the params -- two dabs a spacing apart continue the same lines.
void hatchStencil(const HatchingParams& params, const HatchingDabValues& values, double dabX,
                  double dabY, int width, int height, std::vector<std::uint8_t>& out);

} // namespace mosaic::core::brush
