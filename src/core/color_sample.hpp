#pragma once

#include "common/image.hpp" // Color8, Image

#include <optional>

// Eyedropper colour sampling (S24). A pure, headless-testable helper: given an image and a pixel,
// return the colour the eyedropper should pick, optionally averaged over a neighbourhood. The UI
// (ui::VulkanCanvas + the app's EyedropperHost) resolves WHICH image to read -- the active layer or
// the merged composite, in document space -- and hands it here; this file owns only the arithmetic.
namespace mosaic::core {

// The neighbourhood the sample averages over. Point picks the single pixel; the others average a
// square window centred on it. Mirrors the Eyedropper tool's "Sample" choice
// (Point / 3x3 / 5x5 / 11x11), whose radii are 0 / 1 / 2 / 5.
enum class SampleSize { Point, Avg3, Avg5, Avg11 };

// The half-window (radius) in pixels for a SampleSize: 0, 1, 2, 5. A window of side (2r+1).
[[nodiscard]] int sampleRadius(SampleSize size) noexcept;

// Map the Eyedropper "Sample" option index (0..3) onto a SampleSize; anything out of range is
// Point.
[[nodiscard]] SampleSize sampleSizeFromIndex(int index) noexcept;

// Average the colour of `img` over the (2r+1)x(2r+1) square window centred on (cx, cy), r being the
// size's radius. Only in-bounds pixels contribute -- the window is clipped at the image edge, so a
// sample near a corner averages the pixels that actually exist. Each channel (R,G,B,A) is averaged
// independently in straight (non-premultiplied) 8-bit space: the value the eye reads on screen,
// which is the colour-picker convention (Photoshop/Krita average the displayed pixel values, not a
// premultiplied blend). Returns nullopt when (cx, cy) is itself outside the image, or the image is
// empty -- there is nothing under the cursor to sample.
[[nodiscard]] std::optional<common::Color8> sampleColor(const common::Image& img, int cx, int cy,
                                                        SampleSize size);

} // namespace mosaic::core
