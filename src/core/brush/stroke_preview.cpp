#include "core/brush/stroke_preview.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace mosaic::core::brush {
namespace {

// The S-curve's control net, in fractions of the preview box (docs/brushes.md §8.3). The two control
// points sit OUTSIDE the box vertically (cy -+ h): that is what gives the curve its lean-in and
// lean-out without spending box height on them.
constexpr double kP0X = -0.45, kP0Y = 0.20;
constexpr double kC0X = 0.00, kC0Y = -1.00;
constexpr double kC1X = 0.00, kC1Y = 1.00;
constexpr double kP1X = 0.40, kP1Y = -0.20;

// Samples along the path. The engine interpolates a centripetal Catmull-Rom curve THROUGH the
// samples it is given and lays dabs along that, so this is not the dab count -- it is how finely the
// Bezier is described to the engine. 24 tracks the curve to well under a pixel at any preview size
// the UI uses, and the engine's own flattener refines from there.
constexpr int kSamples = 24;

// The pen accelerates through the stroke: 18 ms between the first pair of samples down to 4 ms
// between the last. A `speed` sensor reads distance over time, so a preview drawn at one constant
// cadence would show every speed-driven preset at a single speed -- which is to say, would not show
// what the option does at all.
constexpr double kFirstStepMs = 18.0;
constexpr double kLastStepMs = 4.0;

// The pen leans over as it goes: upright at the start, well tilted by the end. `declination` (the
// tilt ANGLE) is what most tilt-driven presets read, and it is the magnitude of this vector.
constexpr double kEndXTilt = 52.0;
constexpr double kEndYTilt = -22.0;

[[nodiscard]] double bezier(double t, double p0, double c0, double c1, double p1) {
    const double u = 1.0 - t;
    return (u * u * u * p0) + (3.0 * u * u * t * c0) + (3.0 * u * t * t * c1) + (t * t * t * p1);
}

} // namespace

std::vector<StrokeInput> strokePreviewPath(int width, int height, double inset) {
    std::vector<StrokeInput> path;
    if (width <= 0 || height <= 0)
        return path;

    // ⚠ The path runs through the dabs' CENTRES, so a curve laid out against the full box hangs half
    // a nib over each of its edges. Shrink the box by the radius -- but never past nothing: a brush
    // wider than its strip collapses the curve to a line down the middle and fills the strip, which
    // is a truthful picture of a brush wider than its strip.
    const double pad = std::max(0.0, inset);
    const double w = std::max(1.0, width - 2.0 * pad);
    const double h = std::max(1.0, height - 2.0 * pad);
    const double cx = width * 0.5;
    const double cy = height * 0.5;

    path.reserve(kSamples);
    double timeMs = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        const double t = static_cast<double>(i) / (kSamples - 1); // 0 .. 1 inclusive

        StrokeInput s;
        s.pos = {cx + w * bezier(t, kP0X, kC0X, kC1X, kP1X),
                 cy + h * bezier(t, kP0Y, kC0Y, kC1Y, kP1Y)};
        // The pressure ramp, and it reaches BOTH ends: 0 at the first sample, 1 at the last. An
        // inverted size curve (`z)_Stamp_Shoujo_Bubbles`) is only visible because of the 0 end, and
        // a taper is only visible because of the 1 end.
        s.pressure = t;
        s.xTilt = kEndXTilt * t;
        s.yTilt = kEndYTilt * t;
        s.timeUs = static_cast<std::uint64_t>(std::llround(timeMs * 1000.0));

        path.push_back(s);
        timeMs += kFirstStepMs + (kLastStepMs - kFirstStepMs) * t;
    }
    return path;
}

BrushParams previewCapped(BrushParams params, double maxDiameter) {
    if (maxDiameter <= 0.0 || params.diameter <= maxDiameter)
        return params; // under the ceiling: TRUE SCALE, untouched, to the bit

    // ⚠⚠ SCALE THE WHOLE BRUSH. The masking tip's size is a COEFFICIENT of the master size resolved
    // to an absolute at load, so it has to travel with the master or the authored ratio is broken.
    const double shrink = maxDiameter / params.diameter;
    params.diameter = maxDiameter;
    params.masking.diameter *= shrink;
    return params;
}

namespace {

// One stroke, onto one paper. Returns the image, and whether the brush moved a single pixel of it.
[[nodiscard]] std::pair<common::Image, bool> layStroke(const BrushParams& params, int width,
                                                       int height, common::Color8 paper,
                                                       common::Color8 ink) {
    common::Image img(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    img.fill(paper); // OPAQUE: an eraser can only take away, so it needs something to take.

    // ⚠ A SMUDGE can only move paint around, so it needs paint that VARIES -- the eraser's rule,
    // one step further: on a uniform paper every patch equals every other and the six pure
    // blenders (colour rate 0) render a blank card that reads as a broken import. So a smudge
    // preset's paper wears vertical INK bars for the stroke to drag: a smear streaks them, a
    // dulling blender mixes them into the paper, a wet paint adds its own ink beside them --
    // each family showing exactly the thing it does. The trigger is the ENGINE MODE, never a
    // preset-name list, exactly like the eraser's opaque paper.
    const bool smudge = params.smudge.enabled;
    if (smudge) {
        const int pitch = std::max(8, height / 2);       // bar spacing scales with the strip
        const int barW = std::max(2, height / 10);       // ...and so does the bar
        for (int x0 = pitch / 2; x0 < width; x0 += pitch) {
            for (int y = 0; y < height; ++y) {
                for (int x = x0; x < std::min(width, x0 + barW); ++x) {
                    const std::size_t p =
                        (static_cast<std::size_t>(y) * width + x) * 4;
                    img.rgba[p] = ink.r;
                    img.rgba[p + 1] = ink.g;
                    img.rgba[p + 2] = ink.b;
                    img.rgba[p + 3] = ink.a;
                }
            }
        }
    }
    // With bars on the paper, "did it mark" must compare against the PRE-STROKE image, not the
    // paper colour -- the bars themselves are not a mark.
    std::vector<std::uint8_t> before;
    if (smudge)
        before = img.rgba;

    // The inset is the brush's RADIUS: the path runs through dab CENTRES, so without it the stroke
    // hangs half a nib over every edge of its box.
    const std::vector<StrokeInput> path = strokePreviewPath(width, height, params.diameter * 0.5);
    if (path.size() < 2)
        return {std::move(img), false};

    BrushEngine engine;
    engine.begin(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), img, params,
                 BrushDynamics{}, path.front());
    for (std::size_t i = 1; i < path.size(); ++i)
        engine.extendTo(path[i]);
    engine.flush(); // the walk lags one sample: without this the last span never lands
    engine.composite();
    engine.end();

    bool marked = false;
    if (smudge) {
        marked = img.rgba != before;
    } else {
        for (std::size_t i = 0; i + 3 < img.rgba.size() && !marked; i += 4)
            marked = img.rgba[i] != paper.r || img.rgba[i + 1] != paper.g ||
                     img.rgba[i + 2] != paper.b || img.rgba[i + 3] != paper.a;
    }
    return {std::move(img), marked};
}

} // namespace

common::Image renderStrokePreview(BrushParams params, int width, int height,
                                  const StrokePreviewStyle& style) {
    return renderStrokePreviewResolved(std::move(params), width, height, style).image;
}

StrokePreviewRender renderStrokePreviewResolved(BrushParams params, int width, int height,
                                                const StrokePreviewStyle& style) {
    StrokePreviewRender out;
    out.paper = style.paper;
    out.ink = style.ink;
    if (width <= 0 || height <= 0)
        return out;

    params.color = style.ink;
    // A preview must be the SAME picture every time it is drawn -- a card that reshuffled its own
    // `fuzzy` dabs on every repaint would shimmer. The seed is a parameter precisely so that it can
    // be pinned, and 0 is what every golden pins it to.
    params.seed = 0;
    // ⚠ AND THE PER-STROKE DERIVATION IS SWITCHED OFF WITH IT (§6.6i). A LIVE stroke must draw fresh
    // randomness every time (`ui::BrushPresetStore` turns it on, which is what makes two taps of a
    // random hose stamp two different cells) -- and a preview is the exact opposite requirement, so
    // the params that arrive from the store carry the flag and must have it cleared here. Pinning
    // `seed` alone would NOT be enough: the derivation folds the preview path's own first sample in.
    params.seedFromFirstSample = false;
    params = previewCapped(std::move(params), style.maxDiameter);

    auto [img, marked] = layStroke(params, width, height, style.paper, style.ink);
    if (marked) {
        out.image = std::move(img);
        return out;
    }

    // ⚠⚠ IT LEFT THE PAPER EXACTLY AS IT FOUND IT -- so the CANVAS is the problem, not the brush.
    // Five shipped presets paint through a blend mode that is the IDENTITY for black over white
    // (Lighten, ColorDodge, Color, Overlay, Screen). They work perfectly; white under black is simply
    // a pair they cannot move.
    //
    // ⚠ AND IT IS THE PAIR, NOT THE PAPER. Black is darker than every paper in every channel, so
    // `Lighten` is the identity against any of them (`max(x, 0) == x`). A blend mode can only move a
    // pixel if the ink is brighter than the paper somewhere and darker somewhere else -- so the
    // fallback swaps the INK as well, to a saturated one on a mid-grey.
    //
    // ⚠ The trigger is the RESULT, never a list of preset names: a hard-coded list of the five would
    // be a sixth bug waiting for the next blend mode to ship. (And a brush that marks NEITHER pair --
    // `g)_Dry_Bristles_Eroded`, which the masking-tip gap very nearly deletes -- comes back near-blank
    // either way, which is the truth about it.)
    params.color = style.fallbackInk;
    auto [fallback, movedGrey] =
        layStroke(params, width, height, style.fallbackPaper, style.fallbackInk);
    (void)movedGrey;
    out.image = std::move(fallback);
    out.paper = style.fallbackPaper;
    out.ink = style.fallbackInk;
    out.usedFallback = true;
    return out;
}

} // namespace mosaic::core::brush
