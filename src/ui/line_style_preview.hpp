#pragma once

#include <cstdint>
#include <vector>

// Settings -> Appearance "Selection & reticle line": the animated card previews. Pure CPU pixel
// math (no FLTK) mirroring the present shader's overlay-line styles exactly -- the design bench's
// "photo noise" tile (a warm fbm field + fine grain, a real-image stand-in) drifting at an angle
// beneath a wavy "lasso" stroke and a brush-reticle ring, coloured through the chosen style's
// pick. The dialog blits the buffer via Fl_RGB_Image on every animation tick; keeping the math
// here (instead of inline in the dialog) makes the shader mirror testable.
namespace mosaic::ui {

// The composited value (0..1) of one pixel channel: `style` is the shader's lineStyle (0 Classic,
// 1 Shadowed/rim, 2 Adaptive), `k` the content-luma key under the stroke (the exact pixel for
// Classic, the ~7px blurred neighbourhood for the styled lines -- mirroring the shader), `d` the
// distance in px from the stroke's centre-line, `bg` the background value the stroke composites
// over. All the key/coverage math is scalar in `k` and `d`, and the composite is linear in `bg`,
// so colour callers apply it per channel with one shared `k`.
[[nodiscard]] float lineStyleShade(int style, float k, float d, float bg);

// Render one frame of a card preview into `rgb` (packed RGB, w*h*3, resized here): the photo-noise
// field drifted `phase` px along (1, 0.30) -- the bench's angled scroll -- under a sine "lasso"
// line and a reticle ring drawn with the style's exact profiles and keying (3x3-supersampled near
// the strokes, like the shader). `originX` shifts the field's window: sibling cards pass their
// x-offset in the row so all three read as windows onto ONE drifting background (the pattern
// system's canvas-anchored behaviour) instead of three copies of the same motion.
void renderLineStylePreview(std::vector<std::uint8_t>& rgb, int w, int h, int style, double phase,
                            double originX = 0.0);

}  // namespace mosaic::ui
