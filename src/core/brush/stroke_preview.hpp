#pragma once

// The stroke preview: one brush, one representative stroke, rendered into a plain CPU image.
//
// This is the picture the preset dock's card mode shows (docs/brushes.md §8.2), the picture §8.1's
// chip shows, and the picture §8.3's editor re-renders on every settings change. ONE renderer for
// all three: a preview that disagreed with another preview of the same brush would be worse than no
// preview at all.
//
// It is deliberately in `core`, not `ui`: it drives the REAL `BrushEngine` over a `common::Image`
// and touches no FLTK, no Vulkan and no document. There is no second engine here and there must
// never be one -- a preview drawn by a lookalike would be a drawing of a brush we do not have.

#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/brush/stroke_state.hpp"

#include <vector>

namespace mosaic::core::brush {

// The canvas the preview stroke is laid on, and the paint it is laid in.
//
// The DEFAULTS below are the light theme's: black ink on white paper. The dock passes its own (the
// dark theme's paper is the panel's ground and its ink the muted text colour, and an eraser gets a
// slab of paint to bite) -- see `ui::presetStrokeStyle`. `core` never learns what a palette is; the
// style is simply a parameter, and choosing it is the UI's job.
//
// ⚠ THE PAPER MUST BE OPAQUE, whoever picks it, and that is not a styling choice. **Three shipped
// presets are ERASERS** (`CompositeOp=erase` -> `StrokeMode::Erase`), and an eraser lays NOTHING on an
// empty canvas -- it can only take away. On a transparent canvas all three render a blank card and
// read as broken imports. Opaque paper is what turns a carve into something you can see.
//
// ⚠⚠ AND FIVE PRESETS CANNOT MARK WHITE PAPER WITH BLACK INK AT ALL -- see `fallbackPaper`.
struct StrokePreviewStyle {
    common::Color8 paper{255, 255, 255, 255}; // OPAQUE: see above
    common::Color8 ink{0, 0, 0, 255};

    // ⚠⚠ THE PAPER *AND INK* A BRUSH THAT CANNOT MARK THE FIRST PAIR FALLS BACK TO. Measured, not
    // guessed: **five shipped presets leave white paper EXACTLY as they found it when painting
    // black.** `l)_Adjust_Lighten`, `_Dodge`, `_Color`, `_Overlay_Burn` and `y)_Texture_Starfield`
    // all paint through a BLEND MODE, and Lighten / ColorDodge / Color / Overlay / Screen are every
    // one of them **the identity for black over white**. Those five are not broken -- they work
    // perfectly against a canvas that physically cannot show them -- and they would sit in the dock
    // as five blank cards looking like five bugs.
    //
    // ⚠ AND SWAPPING THE PAPER ALONE DOES NOT FIX IT. **Black is darker than every paper in every
    // channel**, so `Lighten` is the identity against ANY of them: `max(x, 0) == x`. For a blend mode
    // to be able to move a pixel at all, the ink must be BRIGHTER than the paper in some channel and
    // DARKER in another -- which is what a saturated ink on a mid-grey paper guarantees, and nothing
    // else here does. So the fallback swaps BOTH.
    //
    // The trigger is the RESULT, never a list of preset names: a hard-coded list of the five would be
    // a sixth bug waiting for the next blend mode to ship.
    common::Color8 fallbackPaper{138, 138, 144, 255};
    common::Color8 fallbackInk{206, 66, 74, 255};

    // The largest dab diameter the preview will draw, in px. 0 = the brush's true size, whatever it
    // is.
    //
    // §8.3 rules that the preview renders at TRUE BRUSH SCALE and lets a big brush overflow its box,
    // "rather than clamping size into a 3-25 px window that makes large brushes lie" -- and that
    // ruling stands: this is a CEILING, not the reference's window, and every brush under it draws
    // at exactly the size it will paint at. What the ceiling buys is cost, not looks: a preset's
    // authored diameter runs up to 1000 px, and a 1000 px tip rasterizes a MEGAPIXEL MASK PER DAB
    // into a card 60 px tall that a solid blob and a bigger solid blob look identical in. The
    // ceiling is a real loss of honesty at the very top of the range, and it is the only one here.
    double maxDiameter = 0.0;
};

// The stroke the preview draws, sampled along its path (docs/brushes.md §8.3).
//
// A single cubic Bezier S-curve, pressure ramped 0 -> 1 along it. One rise and one fall shows the
// taper, the thin end, the thick end and both curvature directions exactly once; a multi-period sine
// spends most of its arc length repeating itself and crowds the thick end.
//
// ⚠ IT MUST BE A CURVE, AND THE PRESSURE MUST VISIT BOTH ENDS. Both halves of that have already
// cost this codebase a bug:
//   - A STRAIGHT stroke cannot show a per-dab HEADING: on a straight span the curve's local tangent
//     and a chord direction are bit-identical, so a heading-following nib (14 of them) is
//     indistinguishable from one that ignores the stroke. A straight preview would draw 14 presets
//     wrong and look right doing it.
//   - `z)_Stamp_Shoujo_Bubbles` carries an INVERTED size curve (`0,1;1,0;`): press harder, paint
//     SMALLER, to nothing at all at full pressure. A ramp that only ever pressed HARD renders that
//     working preset as an empty card.
// Speed and tilt ramp along the path too, so that speed- and tilt-driven presets preview truthfully
// rather than previewing as though the pen were held still and upright.
//
// ⚠ `inset` is the brush's own RADIUS, and passing it is what keeps the stroke INSIDE its box. The
// path is a curve through the CENTRES of the dabs, so a path laid out against the full box hangs half
// a nib over every edge of it -- which is exactly what a 40 px brush did to a 46 px strip. The curve
// is therefore laid inside a box shrunk by the radius on all four sides. (The box is never shrunk
// past nothing: a brush wider than its strip simply fills it, which is the honest picture of a brush
// wider than its strip.)
[[nodiscard]] std::vector<StrokeInput> strokePreviewPath(int width, int height, double inset = 0.0);

// `params` with the style's diameter ceiling applied -- THE WHOLE BRUSH SCALED, not just the nib.
//
// ⚠⚠ THE MASKING BRUSH IS PART OF THE BRUSH. Its tip's size is authored as a COEFFICIENT of the
// master size (`MaskingBrush/UseMasterSize`, §6.2) and only resolved to an absolute at load, so a
// ceiling that shrinks the primary and leaves the mask where it was does not draw the same brush
// smaller -- it draws a DIFFERENT brush, wearing a mask several times too big for it.
// `g)_Dry_Bristles_Eroded` is a 120 px nib under a 120 px SUBTRACT mask, authored 1:1; cap the nib to
// 40 and leave the mask at 120 and the mask stops eroding the stroke and starts deleting it.
//
// Pure, and separated out precisely so the rule can be asserted as a RATIO rather than inferred from
// a pixel count -- which is a measure of the mask's ferocity, not of its faithfulness.
[[nodiscard]] BrushParams previewCapped(BrushParams params, double maxDiameter);

// `params` painted along `strokePreviewPath` into a fresh `width` x `height` image.
//
// ⚠ `params` is taken BY VALUE and its colour, seed and diameter are the preview's own: the caller
// hands in the preset exactly as `presetBrushParams()` built it (which mints the tip's raster id --
// so build it ONCE per preset and keep it, never once per preview).
//
// The stroke is flushed before it is composited: the dab walk lags the sample stream by one sample,
// so anything that reads a stroke's PIXELS without flushing first sees a stroke with its last span
// missing.
[[nodiscard]] common::Image renderStrokePreview(BrushParams params, int width, int height,
                                                const StrokePreviewStyle& style = {});

// The same render, plus WHICH PAIR IT LANDED ON.
//
// ⚠ THE STYLE THAT WAS ASKED FOR IS NOT ALWAYS THE STYLE THAT WAS USED. A brush that cannot mark the
// requested pair is re-laid on `fallbackPaper` under `fallbackInk` (see above), and a caller that
// then draws anything else onto the same surface -- the editor's scratchpad is exactly that caller
// (docs/brushes.md §8.3) -- has to draw it in the ink the picture actually came back in, or the one
// surface contradicts itself in precisely the five cases the fallback exists for.
struct StrokePreviewRender {
    common::Image image;
    common::Color8 paper{255, 255, 255, 255}; // what the image was actually laid on
    common::Color8 ink{0, 0, 0, 255};         // ... and what it was laid in
    bool usedFallback = false;                // the first pair could not be marked
};
[[nodiscard]] StrokePreviewRender renderStrokePreviewResolved(BrushParams params, int width,
                                                              int height,
                                                              const StrokePreviewStyle& style = {});

} // namespace mosaic::core::brush
