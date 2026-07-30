#pragma once

#include "common/image.hpp"
#include "ui/theme.hpp"

#include <vector>

namespace mosaic::ui {

// The open-an-image invitation, baked for the canvas's documentless idle pass
// (canvas_idle.comp). The old EmptyStateView drew this live with FLTK; a Vulkan-rendered field
// that FADES cannot sit under an opaque FLTK sub-window, so the block becomes a texture: an
// atlas of THREE stacked rows -- idle / pointer-hover / drag-hot -- which the shader crossfades
// between (that is how the frame colour swap and the drag-over headline animate). Baked at
// device resolution and drawn 1:1, so the text is the platform rasterizer's own output, never
// scaled -- re-baked only on theme / DPI changes.
//
// Copy is the settled option A (2026-07-23) plus the drag-over headline swap:
//   "Open an image" / "click anywhere, or drop a file into this window"
//   / "File > New starts a blank canvas"; while a file hovers: "Drop it anywhere".
//
// The bake splits pure from FLTK-bound: the frame/tint/composite helpers below are plain pixel
// arithmetic (unit-tested headlessly); only the text coverage pass touches FLTK (fonts need a
// display, the offscreen surface is RGB-only -- the rasterizeOverlayTile conventions).

struct InvitationBake {
    common::Image atlas;   // rows stacked vertically, each rowW x rowH
    int rowW = 0;
    int rowH = 0;
    static constexpr int kRows = 3; // idle / hover / drag-hot
};

// -- pure helpers (no FLTK) -------------------------------------------------------------------

// Straight-alpha source-over composite of `src` onto `img` at (x, y), clipped.
void compositeOver(common::Image& img, const common::Image& src, int x, int y);

// Multiply `src`'s alpha by `factor` in place (the watermark treatment).
void fadeAlpha(common::Image& img, double factor);

// Tint rows [y0, y1) of a full-image coverage buffer (`covW` wide, one float per pixel, [0,1])
// into `img` at the same coordinates as `color`, source-over with alpha = coverage. The bake
// colours its text per line band this way (lines never overlap).
void tintCoverageBand(common::Image& img, const std::vector<float>& cov, int covW, int y0, int y1,
                      common::Color8 color);

// The dashed rounded invitation frame, anti-aliased, straight alpha, source-over into `img`:
// dashed straight runs + solid quarter-ring corners (the EmptyStateView look). `stroke` is the
// line thickness, `dashOn`/`dashOff` the dash pattern along the straight runs; all px.
void drawInvitationFrame(common::Image& img, double x, double y, double w, double h,
                         double radius, double stroke, double dashOn, double dashOff,
                         common::Color8 color);

// -- the bake (FLTK fonts; needs a display) ---------------------------------------------------

// Bake the three-row atlas for `pal` at `scale` (logical -> device px). Empty atlas on failure
// (no display / no fonts): the idle field then renders bare and the app stays usable.
[[nodiscard]] InvitationBake bakeInvitationAtlas(const Palette& pal, double scale);

} // namespace mosaic::ui
