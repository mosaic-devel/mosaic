#pragma once

#include "common/geometry.hpp"

#include <vector>

// The stroke's PATH between two samples (docs/brushes.md §6.2).
//
// The dab walk used to lay dabs along a STRAIGHT CHORD from one sample to the next. A mouse delivers
// one position per motion event and the compositor coalesces those to the frame, so at 60 Hz a fast
// stroke is literally a 60-sided polygon -- which is exactly what users reported, and why a 160 Hz
// display made it "much less" bad. A tablet hides it by delivering ~200 samples/s regardless of the
// display; the mouse has nowhere to hide.
//
// So the path between two samples is a CURVE, and dabs are laid along it.
//
// ⚠⚠ THE CURVE IS **INTERPOLATING**, AND THAT IS NOT A DETAIL -- IT IS A HARD INVARIANT.
//
// A centripetal Catmull-Rom spline passes EXACTLY THROUGH every sample the user made. It decides
// where dabs land *between* two of the user's own points; it never decides *what those points should
// have been*. It has no anchor, no leash, no lag beyond the one lookahead sample a local fit needs,
// and it cannot steady a shaky hand -- a wobbly input produces a faithfully wobbly curve.
//
// The DEFERRED thing (docs/tablet.md §7, the rope / pulled-string stabilizer) is the opposite: it
// drags a virtual anchor behind the real cursor so the brush paints where the pointer NEVER WAS.
//
// **INTERPOLATE; DO NOT FILTER.** The moment anything in here starts *moving* the input points --
// damping them, averaging them, easing them toward each other before the fit -- it stops being this
// file and becomes that deferred stabilizer. Every point handed to this header must come out of it
// untouched, and that is a standing constraint on the file, not an accident of how it is written
// today. The lineage is Catmull & Rom (1974) and Schneider (Graphics Gems, 1990).
namespace mosaic::core::brush {

// Centripetal Catmull-Rom, evaluated on the span between p1 and p2. `u` in [0,1]; u=0 returns p1 and
// u=1 returns p2, EXACTLY -- the curve interpolates its control points, it does not approximate them.
// p0 and p3 are the neighbouring samples, and they only set the tangents.
//
// Centripetal (the exponent is 0.5, not 0 or 1) because mouse samples are wildly unevenly spaced:
// the uniform parameterization cusps and self-intersects on uneven input, which would put a loop in
// the stroke where the user drew none.
[[nodiscard]] common::Vec2 catmullRom(common::Vec2 p0, common::Vec2 p1, common::Vec2 p2,
                                      common::Vec2 p3, double u);

// The largest number of sub-chords one span is ever flattened into. A bound, not a target.
inline constexpr int kMaxFlattenSteps = 64;

// Flatten the p1->p2 span into INTERIOR points only -- p1 and p2 themselves are not emitted, because
// the walk already has them. `tol` is the greatest distance the polyline may sit from the true curve.
//
// ⚠ Emits NOTHING when the curve lies within `tol` of the straight chord, and that is load-bearing:
// it is what makes a straight stroke lay exactly the dabs it always did. The deviation is measured
// PERPENDICULAR to the chord, deliberately -- a straight-but-unevenly-sampled stroke produces a curve
// that lies exactly on the chord while being *parameterized* differently along it, and a naive
// "distance from p1 + chord*u" probe would read that harmless reparameterization as curvature and
// flatten a straight line into pieces for nothing.
void flattenCatmullRom(common::Vec2 p0, common::Vec2 p1, common::Vec2 p2, common::Vec2 p3,
                       double tol, std::vector<common::Vec2>& out);

} // namespace mosaic::core::brush
