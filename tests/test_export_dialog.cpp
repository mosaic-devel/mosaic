#include "ui/export_dialog.hpp"
#include "ui/export_preview.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <string>

// The Export modal's pure arithmetic: the interactive preview's zoom/pan transform and the three
// linked output-size controls. Both are GUI-free by construction (the widget only ever composes
// these functions), so the properties that actually matter -- "the point under the pointer stays
// under the pointer", "a size entry can never leave the legal range" -- are pinned here without a
// display. The widget gesture itself and the two drawn corner glyphs are the user's visual pass:
// an unshown Fl_Window renders black to an Fl_Image_Surface, so a whole modal cannot be
// screenshot-tested headlessly.
namespace {

using doctest::Approx;
using namespace mosaic::ui;

// Where a widget-local point lands in image space under `t` -- the inverse of the transform the
// preview draws with. Every anchoring assertion below is stated through it.
double toImageX(const PreviewTransform& t, double widgetX) {
    return (widgetX - t.offsetX) / t.scale;
}
double toImageY(const PreviewTransform& t, double widgetY) {
    return (widgetY - t.offsetY) / t.scale;
}

} // namespace

// ---- fit ---------------------------------------------------------------------------------------

TEST_CASE("previewFitScale takes the tighter axis") {
    // A 400x200 image in an 800x600 pane: width binds at 2.0, height would allow 3.0.
    CHECK(previewFitScale(400, 200, 800, 600) == Approx(2.0));
    CHECK(previewFitScale(400, 200, 800, 200) == Approx(1.0)); // now height binds
    CHECK(previewFitScale(1000, 1000, 500, 500) == Approx(0.5));
}

TEST_CASE("previewFitScale refuses a degenerate input rather than dividing by zero") {
    CHECK(previewFitScale(0, 200, 800, 600) == Approx(0.0));
    CHECK(previewFitScale(400, 0, 800, 600) == Approx(0.0));
    CHECK(previewFitScale(400, 200, 0, 600) == Approx(0.0));
    CHECK(previewFitScale(400, 200, 800, 0) == Approx(0.0));
}

TEST_CASE("previewFitTransform centres the fitted image in the pane") {
    const PreviewTransform t = previewFitTransform(400, 200, 800, 600);
    CHECK(t.scale == Approx(2.0));
    CHECK(t.offsetX == Approx(0.0));   // 800 - 400*2 = 0: the bound axis is flush
    CHECK(t.offsetY == Approx(100.0)); // (600 - 200*2) / 2
    // The image centre lands on the pane centre.
    CHECK(toImageX(t, 400.0) == Approx(200.0));
    CHECK(toImageY(t, 300.0) == Approx(100.0));
}

TEST_CASE("previewFitTransform on a degenerate input is the identity, not a NaN") {
    const PreviewTransform t = previewFitTransform(0, 0, 800, 600);
    CHECK(t.scale == Approx(1.0));
    CHECK(t.offsetX == Approx(0.0));
    CHECK(t.offsetY == Approx(0.0));
}

// ---- the zoom range ----------------------------------------------------------------------------

TEST_CASE("the zoom floor is the fit, unless the image is smaller than the pane") {
    // A big image (fit < 1): you may not zoom out past the resting fit -- there is nothing to see.
    CHECK(previewMinScale(0.25) == Approx(0.25));
    // A thumbnail (fit > 1): 100% must stay reachable, so the floor stops at 1.
    CHECK(previewMinScale(8.0) == Approx(1.0));
}

TEST_CASE("the zoom ceiling never lands below the resting fit") {
    CHECK(previewMaxScale(0.25) == Approx(kPreviewMaxZoom));
    CHECK(previewMaxScale(64.0) == Approx(64.0)); // a tiny image already fits above the ceiling
}

TEST_CASE("clampPreviewScale holds the range at both ends") {
    CHECK(clampPreviewScale(0.001, 0.25) == Approx(0.25));
    CHECK(clampPreviewScale(1000.0, 0.25) == Approx(kPreviewMaxZoom));
    CHECK(clampPreviewScale(2.0, 0.25) == Approx(2.0));
    // Nonsense in, resting scale out -- never a NaN into the draw path.
    CHECK(clampPreviewScale(0.0, 0.5) == Approx(0.5));
    CHECK(clampPreviewScale(-3.0, 0.5) == Approx(0.5));
    CHECK(clampPreviewScale(std::nan(""), 0.5) == Approx(0.5));
}

// ---- cursor-anchored zoom: THE contract --------------------------------------------------------

TEST_CASE("previewZoomAt keeps the image point under the cursor exactly where it was") {
    const PreviewTransform start{0.4, 33.0, -17.0};
    const double cursors[][2] = {{0.0, 0.0}, {123.0, 45.0}, {640.0, 480.0}, {-8.0, 900.0}};
    const double scales[] = {0.1, 0.4, 1.0, 3.75, 32.0};
    for (const auto& c : cursors) {
        const double imgX = toImageX(start, c[0]);
        const double imgY = toImageY(start, c[1]);
        for (double s : scales) {
            const PreviewTransform t = previewZoomAt(start, s, c[0], c[1]);
            CHECK(t.scale == Approx(s));
            CHECK(toImageX(t, c[0]) == Approx(imgX));
            CHECK(toImageY(t, c[1]) == Approx(imgY));
        }
    }
}

TEST_CASE("a zoom in and back out about the same point restores the transform") {
    const PreviewTransform start{1.0, 12.0, 30.0};
    const PreviewTransform inward = previewZoomAt(start, 4.0, 200.0, 150.0);
    const PreviewTransform back = previewZoomAt(inward, 1.0, 200.0, 150.0);
    CHECK(back.scale == Approx(start.scale));
    CHECK(back.offsetX == Approx(start.offsetX));
    CHECK(back.offsetY == Approx(start.offsetY));
}

TEST_CASE("previewZoomAt leaves a degenerate transform alone") {
    const PreviewTransform bad{0.0, 5.0, 6.0};
    const PreviewTransform t = previewZoomAt(bad, 2.0, 10.0, 10.0);
    CHECK(t.scale == Approx(0.0));
    CHECK(t.offsetX == Approx(5.0));
    CHECK(t.offsetY == Approx(6.0));
}

TEST_CASE("the wheel zooms in on a scroll up and out on a scroll down") {
    // FLTK's dy is positive for a wheel-DOWN.
    CHECK(previewWheelScale(1.0, -1) == Approx(kPreviewZoomStep));
    CHECK(previewWheelScale(1.0, 1) == Approx(1.0 / kPreviewZoomStep));
    CHECK(previewWheelScale(1.0, 0) == Approx(1.0));
    // Detents compose multiplicatively, so a scroll up then down is a round trip.
    CHECK(previewWheelScale(previewWheelScale(2.5, -3), 3) == Approx(2.5));
}

// ---- pan clamping --------------------------------------------------------------------------------

TEST_CASE("an axis smaller than the pane is centred, with no pan to be had") {
    // 100x100 image at 1x in a 400x300 pane: both axes fit.
    const PreviewTransform dragged{1.0, -500.0, 900.0};
    const PreviewTransform t = clampPreviewPan(dragged, 100, 100, 400, 300);
    CHECK(t.offsetX == Approx(150.0)); // (400 - 100) / 2
    CHECK(t.offsetY == Approx(100.0)); // (300 - 100) / 2
}

TEST_CASE("a larger axis clamps so its edge never leaves the matching pane edge") {
    // 800x600 image at 1x in a 400x300 pane: the offset lives in [-400, 0] x [-300, 0].
    const PreviewTransform t0 = clampPreviewPan({1.0, 50.0, 20.0}, 800, 600, 400, 300);
    CHECK(t0.offsetX == Approx(0.0)); // dragged past the left edge -> flush
    CHECK(t0.offsetY == Approx(0.0));
    const PreviewTransform t1 = clampPreviewPan({1.0, -9000.0, -9000.0}, 800, 600, 400, 300);
    CHECK(t1.offsetX == Approx(-400.0));
    CHECK(t1.offsetY == Approx(-300.0));
    const PreviewTransform t2 = clampPreviewPan({1.0, -123.0, -45.0}, 800, 600, 400, 300);
    CHECK(t2.offsetX == Approx(-123.0)); // inside the range: untouched
    CHECK(t2.offsetY == Approx(-45.0));
}

TEST_CASE("the axes clamp independently") {
    // 800x100 image at 1x in a 400x300 pane: x pans, y centres.
    const PreviewTransform t = clampPreviewPan({1.0, -1000.0, 77.0}, 800, 100, 400, 300);
    CHECK(t.offsetX == Approx(-400.0));
    CHECK(t.offsetY == Approx(100.0));
}

TEST_CASE("the fit transform is already a fixed point of the pan clamp") {
    const PreviewTransform fit = previewFitTransform(1600, 900, 400, 300);
    const PreviewTransform t = clampPreviewPan(fit, 1600, 900, 400, 300);
    CHECK(t.offsetX == Approx(fit.offsetX));
    CHECK(t.offsetY == Approx(fit.offsetY));
}

TEST_CASE("zooming to 1:1 about the pane centre keeps the centre pixel, then clamps in bounds") {
    // The exact sequence ExportPreview::actualPixels() runs on a 2000x1500 image in a 640x480 pane.
    const int iw = 2000, ih = 1500, vw = 640, vh = 480;
    const PreviewTransform fit = previewFitTransform(iw, ih, vw, vh);
    const double centreImgX = toImageX(fit, vw * 0.5);
    const double centreImgY = toImageY(fit, vh * 0.5);
    const double target = clampPreviewScale(1.0, previewFitScale(iw, ih, vw, vh));
    CHECK(target == Approx(1.0));
    const PreviewTransform zoomed = previewZoomAt(fit, target, vw * 0.5, vh * 0.5);
    CHECK(toImageX(zoomed, vw * 0.5) == Approx(centreImgX));
    CHECK(toImageY(zoomed, vh * 0.5) == Approx(centreImgY));
    // The image is larger than the pane at 1:1, so the clamp leaves it where it is -- the centre
    // of a centred zoom can never be out of bounds.
    const PreviewTransform held = clampPreviewPan(zoomed, iw, ih, vw, vh);
    CHECK(held.offsetX == Approx(zoomed.offsetX));
    CHECK(held.offsetY == Approx(zoomed.offsetY));
    CHECK(held.offsetX <= 0.0);
    CHECK(held.offsetX >= vw - iw * 1.0);
}

TEST_CASE("fit after a zoom returns exactly the resting transform") {
    const PreviewTransform fit = previewFitTransform(1200, 800, 500, 400);
    PreviewTransform t = previewZoomAt(fit, 6.0, 10.0, 390.0);
    t = clampPreviewPan(t, 1200, 800, 500, 400);
    const PreviewTransform reset = previewFitTransform(1200, 800, 500, 400);
    CHECK(reset.scale == Approx(fit.scale));
    CHECK(reset.offsetX == Approx(fit.offsetX));
    CHECK(reset.offsetY == Approx(fit.offsetY));
}

// ---- the info line's byte count ------------------------------------------------------------------

TEST_CASE("humanFileSize switches units at the binary boundaries") {
    CHECK(humanFileSize(0) == "0 B");
    CHECK(humanFileSize(864) == "864 B");
    CHECK(humanFileSize(1023) == "1023 B");
    CHECK(humanFileSize(1024) == "1.0 KB");
    CHECK(humanFileSize(1024 * 1024 - 1) == "1024.0 KB");
    CHECK(humanFileSize(1024 * 1024) == "1.00 MB");
    CHECK(humanFileSize(3 * 1024 * 1024 / 2) == "1.50 MB");
}

// ---- the three linked size controls ---------------------------------------------------------------

TEST_CASE("a percentage scales both axes and rounds to whole pixels") {
    CHECK(exportSizeFromScale(1920, 1080, 100.0) == ExportPixelSize{1920, 1080});
    CHECK(exportSizeFromScale(1920, 1080, 50.0) == ExportPixelSize{960, 540});
    CHECK(exportSizeFromScale(1920, 1080, 25.0) == ExportPixelSize{480, 270});
    CHECK(exportSizeFromScale(1920, 1080, 200.0) == ExportPixelSize{3840, 2160});
    CHECK(exportSizeFromScale(101, 51, 33.0) == ExportPixelSize{33, 17}); // 33.33 -> 33, 16.83 -> 17
}

TEST_CASE("no size entry can leave the legal range") {
    // A percentage so small it would round to zero still yields a 1px picture.
    CHECK(exportSizeFromScale(100, 100, 0.0001) == ExportPixelSize{1, 1});
    // ... and one large enough to overflow the encoders is capped, not truncated.
    const ExportPixelSize huge = exportSizeFromScale(4000, 4000, 100000.0);
    CHECK(huge.w == kMaxExportDim);
    CHECK(huge.h == kMaxExportDim);
    // Direct field entries clamp the same way, including hostile ones.
    CHECK(exportSizeFromWidth(100, 100, -50.0, 100, false).w == 1);
    CHECK(exportSizeFromWidth(100, 100, 1e12, 100, false).w == kMaxExportDim);
    CHECK(exportSizeFromHeight(100, 100, 0.0, 100, false).h == 1);
    CHECK(exportSizeFromHeight(100, 100, std::nan(""), 100, false).h == 1);
    // An empty source cannot produce a zero output.
    CHECK(exportSizeFromScale(0, 0, 100.0) == ExportPixelSize{1, 1});
}

TEST_CASE("the aspect lock drives the other field from the SOURCE ratio, not the current one") {
    // Base 1600x900. Height is currently something the user set by hand while unlocked.
    CHECK(exportSizeFromWidth(1600, 900, 800.0, 123, true) == ExportPixelSize{800, 450});
    CHECK(exportSizeFromHeight(1600, 900, 450.0, 123, true) == ExportPixelSize{800, 450});
    // Unlocked, the other axis is left exactly as it was.
    CHECK(exportSizeFromWidth(1600, 900, 800.0, 123, false) == ExportPixelSize{800, 123});
    CHECK(exportSizeFromHeight(1600, 900, 450.0, 123, false) == ExportPixelSize{123, 450});
}

TEST_CASE("a locked edit round-trips through the percentage readout") {
    const ExportPixelSize s = exportSizeFromWidth(1920, 1080, 960.0, 1080, true);
    CHECK(exportScalePercent(1920, s.w) == Approx(50.0));
    CHECK(exportSizeFromScale(1920, 1080, exportScalePercent(1920, s.w)) == s);
}

TEST_CASE("exportScalePercent answers 100 for an empty source rather than dividing by zero") {
    CHECK(exportScalePercent(0, 512) == Approx(100.0));
    CHECK(exportScalePercent(512, 512) == Approx(100.0));
    CHECK(exportScalePercent(512, 128) == Approx(25.0));
}
