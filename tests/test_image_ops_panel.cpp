// The S53 Image-menu operations panel (Image Size / Canvas Size / Rotate Arbitrary). Like the
// morphology and 3D panels it is a child sub-window, and constructing + rebuilding it WITHOUT
// show() is safe headlessly -- which is the point: `configure()` is deliberately split out of
// openFor() so the whole model (unit conversion, the proportions lock, the anchor grid, the
// Request round trip) pins here without a display.
//
// The controls are driven exactly as a user would: set the widget's value, fire its callback, and
// assert on what came out of the host funnel (the tests/test_adjustment_panel.cpp pattern).
#include "ui/image_ops_panel.hpp"

#include "ui/new_document_dialog.hpp" // SizeUnit, kMaxCanvasDimension
#include "ui/paint_chip.hpp"          // PaintChip (the Gradient/Pattern fills' contextual chip)
#include "ui/scrub_slider.hpp"        // ScrubSlider (the width/height scrub gesture)
#include "ui/widgets.hpp"

#include <doctest/doctest.h>

#include <FL/Fl_Group.H>

#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace mosaic;
using ui::ImageOpsPanel;

namespace {

// Every generated control the panel could carry, gathered by TYPE rather than by index: the row
// set differs per mode, so a positional census would silently test the wrong widget the moment a
// row moves.
struct Census {
    std::vector<ui::NumberField*> numbers; // width, height, angle -- in layout order
    std::vector<ui::Dropdown*> dropdowns;  // units, resample, fill, gradient-type -- layout order
    std::vector<ui::ScrubSlider*> scrubs;  // width, height
    std::vector<ui::Dial*> dials;          // rotate angle, gradient direction
    std::vector<ui::CheckBox*> checks;
    std::vector<ui::AnchorGrid*> grids;
    std::vector<ui::SwatchChip*> swatches;
    std::vector<ui::PaintChip*> paintChips;
};

void collect(const Fl_Group* g, Census& out) {
    for (int i = 0; i < g->children(); ++i) {
        Fl_Widget* c = g->child(i);
        if (auto* n = dynamic_cast<ui::NumberField*>(c)) out.numbers.push_back(n);
        if (auto* d = dynamic_cast<ui::Dropdown*>(c)) out.dropdowns.push_back(d);
        if (auto* s = dynamic_cast<ui::ScrubSlider*>(c)) out.scrubs.push_back(s);
        if (auto* dl = dynamic_cast<ui::Dial*>(c)) out.dials.push_back(dl);
        if (auto* cb = dynamic_cast<ui::CheckBox*>(c)) out.checks.push_back(cb);
        if (auto* ag = dynamic_cast<ui::AnchorGrid*>(c)) out.grids.push_back(ag);
        if (auto* sw = dynamic_cast<ui::SwatchChip*>(c)) out.swatches.push_back(sw);
        if (auto* pc = dynamic_cast<ui::PaintChip*>(c)) out.paintChips.push_back(pc);
        if (const auto* sub = dynamic_cast<const Fl_Group*>(c)) collect(sub, out);
    }
}

Census censusOf(const Fl_Group* g) {
    Census c;
    collect(g, c);
    return c;
}

// How many real rows a dropdown carries (Fl_Menu_::size() counts the terminating null too).
int itemCount(const ui::Dropdown* d) {
    int n = 0;
    for (int i = 0; i < d->size(); ++i)
        if (d->text(i) != nullptr)
            ++n;
    return n;
}

// Type into a NumberField the way a keystroke does: the field's `when(FL_WHEN_CHANGED)` means the
// callback is what carries the edit into the model, so the two always travel together.
void typeInto(ui::NumberField* f, const char* text) {
    REQUIRE(f != nullptr);
    f->value(text);
    f->do_callback();
}

void pick(ui::Dropdown* d, int index) {
    REQUIRE(d != nullptr);
    d->value(index);
    d->do_callback();
}

} // namespace

TEST_CASE("each mode builds only the controls it needs, and seeds them from the document size") {
    ImageOpsPanel panel;

    // Canvas Size: width + height (each a scrub slider AND an exact-entry field), the unit and
    // fill combos, the proportions lock, the 3x3 anchor. No resample dropdown -- re-framing a
    // canvas never resamples anything. The gradient-type combo + direction dial are part of the
    // Fill choice's contextual band and exist (hidden) in every fillable mode.
    panel.configure(ImageOpsPanel::Mode::CanvasSize, 1920, 1080);
    CHECK(panel.mode() == ImageOpsPanel::Mode::CanvasSize);
    CHECK(panel.pixelWidth() == 1920);
    CHECK(panel.pixelHeight() == 1080);
    Census c = censusOf(&panel);
    CHECK(c.numbers.size() == 2);   // width, height -- typing never went away
    CHECK(c.scrubs.size() == 2);    // ... and each gained a scrub gesture beside it
    CHECK(c.dropdowns.size() == 3); // units, fill, gradient type
    CHECK(c.dials.size() == 1);     // the gradient direction dial (no angle in this mode)
    CHECK(c.checks.size() == 1);    // constrain proportions
    CHECK(c.swatches.size() == 1);  // the solid-fill colour chip
    CHECK(c.paintChips.size() == 1);
    REQUIRE(c.grids.size() == 1);
    CHECK(panel.anchorGrid() == c.grids[0]);

    // Image Size: the same size form plus the resample-quality combo, and NO anchor (the picture
    // scales in place, so there is nowhere to place it) and NO fill (nothing is ever exposed).
    panel.configure(ImageOpsPanel::Mode::ImageSize, 800, 600);
    CHECK(panel.pixelWidth() == 800);
    CHECK(panel.pixelHeight() == 600);
    c = censusOf(&panel);
    CHECK(c.numbers.size() == 2);
    CHECK(c.scrubs.size() == 2);
    CHECK(c.dropdowns.size() == 2); // units, resample
    CHECK(c.dials.empty());         // nothing to turn, and no fill band
    CHECK(c.checks.size() == 1);
    CHECK(c.swatches.empty());
    CHECK(c.paintChips.empty());
    CHECK(c.grids.empty());
    CHECK(panel.anchorGrid() == nullptr);

    // Rotate Arbitrary: the angle as a DIAL plus its exact field, the resample kernel and the
    // corner-wedge fill. No size form at all -- the new canvas is the rotated bounding box, not
    // something to type.
    panel.configure(ImageOpsPanel::Mode::RotateArbitrary, 800, 600);
    c = censusOf(&panel);
    CHECK(c.numbers.size() == 1);   // angle
    CHECK(c.scrubs.empty());        // ... and no size rows to scrub
    CHECK(c.dropdowns.size() == 3); // resample, fill, gradient type
    CHECK(c.dials.size() == 2);     // the angle dial + the gradient direction dial
    CHECK(c.checks.empty());
    CHECK(c.grids.empty());
}

TEST_CASE("the expansion Fill list is the Fill dialog's family, plus Transparent") {
    // The user's report: "Canvas Size Fill missing Inpaint and family from the Fill... dialog".
    // The list is now Edit->Fill...'s own Contents in its own order (Foreground / Background /
    // White / Black / 50% Gray / Color... / Gradient... / Pattern... / Inpaint) with Transparent
    // in front -- the one thing only an EXPANSION can mean.
    ImageOpsPanel panel;
    panel.configure(ImageOpsPanel::Mode::CanvasSize, 800, 600);
    Census c = censusOf(&panel);
    REQUIRE(c.dropdowns.size() == 3);
    CHECK(itemCount(c.dropdowns[1]) == 10); // ten rows: Transparent + the nine Fill contents

    // The enum IS the dropdown's index, so a row cannot silently move without the mapping moving.
    CHECK(static_cast<int>(ImageOpsPanel::FillMode::Transparent) == 0);
    CHECK(static_cast<int>(ImageOpsPanel::FillMode::Inpaint) == 9);

    // Rotate Arbitrary offers the same list MINUS Inpaint: healing a rotation's corner wedges
    // would need the composite re-rotated to seed the reconstruction, which the panel cannot ask
    // for mid-preview -- and an item that cannot deliver is worse than no item.
    panel.configure(ImageOpsPanel::Mode::RotateArbitrary, 800, 600);
    c = censusOf(&panel);
    REQUIRE(c.dropdowns.size() == 3);
    CHECK(itemCount(c.dropdowns[1]) == 9);
}

TEST_CASE("the paint fills ride the request as a Paint, not as a flat colour") {
    ImageOpsPanel panel;
    std::vector<ImageOpsPanel::Request> previews;
    panel.setOnPreview([&](const ImageOpsPanel::Request& r) { previews.push_back(r); });
    panel.setFillColorProviders([] { return common::Color8{10, 20, 30, 255}; },
                                [] { return common::Color8{200, 210, 220, 255}; });
    panel.configure(ImageOpsPanel::Mode::CanvasSize, 800, 600);
    Census c = censusOf(&panel);
    REQUIRE(c.dropdowns.size() == 3);

    // A solid content resolves to a CropFill right here in the panel ...
    pick(c.dropdowns[1], static_cast<int>(ImageOpsPanel::FillMode::Gray));
    REQUIRE(!previews.empty());
    REQUIRE(previews.back().fill.has_value());
    CHECK(previews.back().fill->color.r == 128);
    CHECK(std::holds_alternative<core::vec::SolidPaint>(previews.back().paint));

    // ... a gradient or a pattern does NOT: rasterizing one costs a whole new-canvas-sized image
    // and a preview fires on every keystroke, so the panel hands over the PAINT and the host
    // materializes it once, on Apply.
    pick(c.dropdowns[1], static_cast<int>(ImageOpsPanel::FillMode::Gradient));
    CHECK(previews.back().fillMode == ImageOpsPanel::FillMode::Gradient);
    CHECK_FALSE(previews.back().fill.has_value());
    CHECK(std::holds_alternative<core::vec::Gradient>(previews.back().paint));

    pick(c.dropdowns[1], static_cast<int>(ImageOpsPanel::FillMode::Pattern));
    CHECK(previews.back().fillMode == ImageOpsPanel::FillMode::Pattern);
    CHECK_FALSE(previews.back().fill.has_value());
    CHECK(std::holds_alternative<core::vec::Pattern>(previews.back().paint));

    // Inpaint is the async engine path: no colour, no paint to lay down, and the host routes on
    // fillMode alone.
    pick(c.dropdowns[1], static_cast<int>(ImageOpsPanel::FillMode::Inpaint));
    CHECK(previews.back().fillMode == ImageOpsPanel::FillMode::Inpaint);
    CHECK_FALSE(previews.back().fill.has_value());

    // "Color…" takes its colour from the host's flyout pick, not from a fixed table.
    pick(c.dropdowns[1], static_cast<int>(ImageOpsPanel::FillMode::Custom));
    panel.setCustomFillColor({7, 8, 9, 255});
    REQUIRE(previews.back().fill.has_value());
    CHECK(previews.back().fill->color.r == 7);
    CHECK(previews.back().fill->color.g == 8);
    CHECK(previews.back().fill->color.b == 9);
}

TEST_CASE("a preview-handle drag drives the size and the anchor, and the panel keeps the numbers") {
    // The canvas reports the staged NEW canvas rect in CURRENT document pixels; the panel adopts
    // it, derives which of the nine placements it means, and re-fires ONE preview.
    ImageOpsPanel panel;
    std::vector<ImageOpsPanel::Request> previews;
    panel.setOnPreview([&](const ImageOpsPanel::Request& r) { previews.push_back(r); });
    panel.configure(ImageOpsPanel::Mode::CanvasSize, 1000, 500);
    panel.setConstrainProportions(false);

    // Grown 200 px to the right and 100 px down, with the old canvas flush at the new top-left.
    panel.applyPreviewDrag(0, 0, 1200, 600);
    CHECK(panel.pixelWidth() == 1200);
    CHECK(panel.pixelHeight() == 600);
    REQUIRE(!previews.empty());
    CHECK(previews.back().width == 1200);
    CHECK(previews.back().height == 600);
    CHECK(previews.back().anchor == render::CanvasAnchor::TopLeft);
    REQUIRE(panel.anchorGrid() != nullptr);
    CHECK(panel.anchorGrid()->value() == render::CanvasAnchor::TopLeft); // the widget followed

    // The same rect again is not a new edit: no anchor cell changed, so no preview churn.
    const std::size_t before = previews.size();
    panel.applyPreviewDrag(0, 0, 1200, 600);
    CHECK(previews.size() == before);

    // Grown symmetrically: the old canvas sits centred in the new one.
    panel.applyPreviewDrag(-100, -50, 1200, 600);
    CHECK(previews.back().anchor == render::CanvasAnchor::Center);
    // ... and flush at the far corner.
    panel.applyPreviewDrag(-200, -100, 1200, 600);
    CHECK(previews.back().anchor == render::CanvasAnchor::BottomRight);

    // Rotate has no size to drag out (its canvas is a function of the angle): the drag is ignored.
    panel.configure(ImageOpsPanel::Mode::RotateArbitrary, 400, 200);
    const std::size_t rotateBefore = previews.size();
    panel.applyPreviewDrag(0, 0, 4000, 2000);
    CHECK(previews.size() == rotateBefore);
    CHECK(panel.pixelWidth() == 400);
}

TEST_CASE("the Canvas Size request carries the typed size, the anchor and the chosen fill") {
    ImageOpsPanel panel;
    std::vector<ImageOpsPanel::Request> previews;
    std::vector<ImageOpsPanel::Request> applied;
    panel.setOnPreview([&](const ImageOpsPanel::Request& r) { previews.push_back(r); });
    panel.setOnApply([&](const ImageOpsPanel::Request& r) { applied.push_back(r); });
    panel.configure(ImageOpsPanel::Mode::CanvasSize, 1000, 500);
    CHECK(previews.empty()); // configure() seeds the controls; it does NOT fire the funnel

    Census c = censusOf(&panel);
    REQUIRE(c.numbers.size() == 2);
    REQUIRE(c.scrubs.size() == 2);
    REQUIRE(c.checks.size() == 1);
    REQUIRE(c.dropdowns.size() == 3);
    REQUIRE(c.grids.size() == 1);

    panel.setConstrainProportions(false); // unlock: the two axes now move independently

    typeInto(c.numbers[0], "1600");
    REQUIRE(!previews.empty());
    CHECK(previews.back().mode == ImageOpsPanel::Mode::CanvasSize);
    CHECK(previews.back().width == 1600);
    CHECK(previews.back().height == 500); // unlocked: the height did not follow
    // Typing moved the scrub slider too -- the two are one value with two gestures.
    CHECK(c.scrubs[0]->value() == doctest::Approx(1600.0));

    // Transparent leads the list and is spelled as NO CropFill at all (the engine's "add nothing").
    CHECK_FALSE(previews.back().fill.has_value());
    pick(c.dropdowns[1], static_cast<int>(ImageOpsPanel::FillMode::White));
    REQUIRE(previews.back().fill.has_value());
    CHECK(previews.back().fill->color.r == 255);
    CHECK(previews.back().fill->color.g == 255);
    CHECK(previews.back().fill->color.b == 255);
    CHECK(previews.back().fill->color.a == 255);
    CHECK_FALSE(previews.back().fill->layerName.empty());
    pick(c.dropdowns[1], static_cast<int>(ImageOpsPanel::FillMode::Black));
    REQUIRE(previews.back().fill.has_value());
    CHECK(previews.back().fill->color.r == 0);

    // Foreground / Background resolve through the host's providers, so the panel never has to
    // know where the colour state lives.
    panel.setFillColorProviders([] { return common::Color8{10, 20, 30, 255}; },
                                [] { return common::Color8{40, 50, 60, 255}; });
    pick(c.dropdowns[1], static_cast<int>(ImageOpsPanel::FillMode::Foreground));
    REQUIRE(previews.back().fill.has_value());
    CHECK(previews.back().fill->color.g == 20);
    pick(c.dropdowns[1], static_cast<int>(ImageOpsPanel::FillMode::Background));
    REQUIRE(previews.back().fill.has_value());
    CHECK(previews.back().fill->color.g == 50);

    // The anchor rides the request; a keyboard move fires the preview like a click would.
    CHECK(previews.back().anchor == render::CanvasAnchor::Center); // the seeded default
    panel.anchorGrid()->moveBy(-1, -1);
    CHECK(previews.back().anchor == render::CanvasAnchor::TopLeft);
    panel.anchorGrid()->moveBy(0, 1);
    CHECK(previews.back().anchor == render::CanvasAnchor::Left);

    // request() is what Apply hands the host -- the same value the last preview carried.
    const ImageOpsPanel::Request now = panel.request();
    CHECK(now.width == 1600);
    CHECK(now.height == 500);
    CHECK(now.anchor == render::CanvasAnchor::Left);

    // Apply fires the funnel once, with that same request.
    CHECK(applied.empty());
    for (int i = 0; i < panel.children(); ++i) {
        if (auto* b = dynamic_cast<ui::FilledButton*>(panel.child(i))) {
            b->do_callback();
            break;
        }
    }
    REQUIRE(applied.size() == 1);
    CHECK(applied.back().width == 1600);
    CHECK(applied.back().height == 500);
    CHECK(applied.back().anchor == render::CanvasAnchor::Left);
}

TEST_CASE("constrain proportions keeps the seeded aspect, and re-arming adopts what is on screen") {
    ImageOpsPanel panel;
    panel.configure(ImageOpsPanel::Mode::ImageSize, 1000, 500); // 2:1
    Census c = censusOf(&panel);
    REQUIRE(c.numbers.size() == 2);
    REQUIRE(c.checks.size() == 1);
    CHECK(panel.constrainProportions()); // locked by default

    typeInto(c.numbers[0], "600");
    CHECK(panel.pixelWidth() == 600);
    CHECK(panel.pixelHeight() == 300); // 600 / 2

    typeInto(c.numbers[1], "800"); // editing the height drives the width the other way
    CHECK(panel.pixelHeight() == 800);
    CHECK(panel.pixelWidth() == 1600);

    // Unlocked, the axes are independent again -- and the checkbox follows the model.
    panel.setConstrainProportions(false);
    CHECK_FALSE(panel.constrainProportions());
    CHECK_FALSE(c.checks[0]->checked());
    typeInto(c.numbers[0], "300");
    CHECK(panel.pixelWidth() == 300);
    CHECK(panel.pixelHeight() == 800);

    // Re-arming the lock adopts whatever ratio is showing NOW (300x800), not the document's.
    panel.setConstrainProportions(true);
    CHECK(c.checks[0]->checked());
    typeInto(c.numbers[0], "600");
    CHECK(panel.pixelWidth() == 600);
    CHECK(panel.pixelHeight() == 1600);
}

TEST_CASE("sizes clamp to a usable canvas: never zero, never past the maximum dimension") {
    ImageOpsPanel panel;
    panel.configure(ImageOpsPanel::Mode::CanvasSize, 100, 100);
    Census c = censusOf(&panel);
    REQUIRE(c.numbers.size() == 2);
    panel.setConstrainProportions(false);

    typeInto(c.numbers[0], "0");
    CHECK(panel.pixelWidth() == 1); // a zero-wide canvas is not a canvas
    typeInto(c.numbers[0], "-40");
    CHECK(panel.pixelWidth() == 1);
    typeInto(c.numbers[0], "999999");
    CHECK(panel.pixelWidth() == ui::kMaxCanvasDimension);

    // The arithmetic NumberField already speaks "1024*2"; the panel just has to not undo it.
    typeInto(c.numbers[0], "1024*2");
    CHECK(panel.pixelWidth() == 2048);
}

TEST_CASE("the unit combo re-expresses the same pixels and never resizes the document") {
    ImageOpsPanel panel;
    // 300 dpi so an inch is a round 300 px and the conversion is exact both ways.
    panel.configure(ImageOpsPanel::Mode::ImageSize, 600, 300, 300.0);
    Census c = censusOf(&panel);
    REQUIRE(c.numbers.size() == 2);
    REQUIRE(c.dropdowns.size() == 2);
    CHECK(panel.unit() == ui::SizeUnit::Pixels);

    pick(c.dropdowns[0], static_cast<int>(ui::SizeUnit::Inches));
    CHECK(panel.unit() == ui::SizeUnit::Inches);
    CHECK(panel.pixelWidth() == 600); // switching units resized nothing
    CHECK(panel.pixelHeight() == 300);

    // ... and typing in the new unit converts back through the dpi.
    typeInto(c.numbers[0], "4"); // 4 in at 300 dpi
    CHECK(panel.pixelWidth() == 1200);

    pick(c.dropdowns[0], static_cast<int>(ui::SizeUnit::Pixels));
    CHECK(panel.pixelWidth() == 1200);
}

TEST_CASE("the rotate mode reports its angle and resample kernel, and nothing else") {
    ImageOpsPanel panel;
    std::vector<ImageOpsPanel::Request> previews;
    panel.setOnPreview([&](const ImageOpsPanel::Request& r) { previews.push_back(r); });
    panel.configure(ImageOpsPanel::Mode::RotateArbitrary, 400, 200);
    Census c = censusOf(&panel);
    REQUIRE(c.numbers.size() == 1);
    REQUIRE(c.dropdowns.size() == 3);
    REQUIRE(c.dials.size() == 2); // [0] = the angle knob, [1] = the gradient direction

    typeInto(c.numbers[0], "37.5");
    REQUIRE(!previews.empty());
    CHECK(previews.back().mode == ImageOpsPanel::Mode::RotateArbitrary);
    CHECK(previews.back().angleDeg == doctest::Approx(37.5));
    CHECK(c.dials[0]->value() == doctest::Approx(37.5)); // the knob followed the typed value

    // ... and the knob drives the value the other way. A turn past the half-circle wraps into the
    // dial's range rather than sticking at an endpoint, which is the whole reason it is a dial.
    c.dials[0]->value(-135.0);
    c.dials[0]->do_callback();
    CHECK(previews.back().angleDeg == doctest::Approx(-135.0));

    // The kernel list leads with Auto (the Request's default) and then follows the ResampleFilter
    // enum's own order, so the index IS the enum's index.
    CHECK(previews.back().filter == render::ResampleFilter::Auto);
    pick(c.dropdowns[0], static_cast<int>(render::ResampleFilter::Lanczos3));
    CHECK(previews.back().filter == render::ResampleFilter::Lanczos3);

    // A negative angle is a legitimate turn the other way -- not something to clamp away.
    typeInto(c.numbers[0], "-90");
    CHECK(previews.back().angleDeg == doctest::Approx(-90.0));
}

TEST_CASE("the anchor grid maps cells to CanvasAnchor row-major, exactly as the engine reads it") {
    // The engine documents `int(a) % 3` as the column and `int(a) / 3` as the row; the widget and
    // the tests share ONE definition of that, so a drift here is a compile-time-visible drift.
    CHECK(ui::AnchorGrid::anchorFor(0, 0) == render::CanvasAnchor::TopLeft);
    CHECK(ui::AnchorGrid::anchorFor(1, 0) == render::CanvasAnchor::Top);
    CHECK(ui::AnchorGrid::anchorFor(2, 0) == render::CanvasAnchor::TopRight);
    CHECK(ui::AnchorGrid::anchorFor(0, 1) == render::CanvasAnchor::Left);
    CHECK(ui::AnchorGrid::anchorFor(1, 1) == render::CanvasAnchor::Center);
    CHECK(ui::AnchorGrid::anchorFor(2, 1) == render::CanvasAnchor::Right);
    CHECK(ui::AnchorGrid::anchorFor(0, 2) == render::CanvasAnchor::BottomLeft);
    CHECK(ui::AnchorGrid::anchorFor(1, 2) == render::CanvasAnchor::Bottom);
    CHECK(ui::AnchorGrid::anchorFor(2, 2) == render::CanvasAnchor::BottomRight);

    CHECK(ui::AnchorGrid::columnOf(render::CanvasAnchor::BottomLeft) == 0);
    CHECK(ui::AnchorGrid::rowOf(render::CanvasAnchor::BottomLeft) == 2);
    CHECK(ui::AnchorGrid::columnOf(render::CanvasAnchor::Right) == 2);
    CHECK(ui::AnchorGrid::rowOf(render::CanvasAnchor::Right) == 1);

    // Out-of-range cells clamp rather than wrapping into a neighbouring row.
    CHECK(ui::AnchorGrid::anchorFor(-1, -1) == render::CanvasAnchor::TopLeft);
    CHECK(ui::AnchorGrid::anchorFor(7, 7) == render::CanvasAnchor::BottomRight);
}

// moveBy() IS the arrow-key handler's whole body -- AnchorGrid::handle only maps FL_Left/Right/
// Up/Down onto it -- so driving it directly tests the navigation without synthesising key events.
TEST_CASE("arrow-key navigation walks the anchor grid and stops at the edges") {
    // Defensive: a bare widget joins whatever Fl_Group is "current", and an earlier test in the
    // binary may have left one open -- which would then delete this stack object.
    Fl_Group::current(nullptr);
    ui::AnchorGrid grid(0, 0, 96, 96);
    int fired = 0;
    // A raw Fl_Callback thunk: the widget only ever reports "the value changed", and the count is
    // what proves an edge press does NOT re-fire the host's live preview.
    grid.callback([](Fl_Widget*, void* n) { ++*static_cast<int*>(n); }, &fired);

    CHECK(grid.value() == render::CanvasAnchor::Center); // the honest default: no re-framing bias

    grid.moveBy(0, -1);
    CHECK(grid.value() == render::CanvasAnchor::Top);
    CHECK(fired == 1);
    grid.moveBy(-1, 0);
    CHECK(grid.value() == render::CanvasAnchor::TopLeft);
    CHECK(fired == 2);

    // Already at the top-left corner: both presses are no-ops, and neither fires.
    grid.moveBy(0, -1);
    grid.moveBy(-1, 0);
    CHECK(grid.value() == render::CanvasAnchor::TopLeft);
    CHECK(fired == 2);

    // A diagonal walk to the opposite corner, then the same clamp on the other side.
    grid.moveBy(1, 1);
    CHECK(grid.value() == render::CanvasAnchor::Center);
    grid.moveBy(1, 1);
    CHECK(grid.value() == render::CanvasAnchor::BottomRight);
    CHECK(fired == 4);
    grid.moveBy(1, 1);
    CHECK(grid.value() == render::CanvasAnchor::BottomRight);
    CHECK(fired == 4);

    // setValue is the host's seeding path: it moves the selection WITHOUT firing (otherwise
    // reflecting state into the widget would read as a user edit and re-preview).
    grid.setValue(render::CanvasAnchor::Left);
    CHECK(grid.value() == render::CanvasAnchor::Left);
    CHECK(fired == 4);
}

TEST_CASE("a theme rebuild regenerates the controls and keeps the panel's state") {
    ImageOpsPanel panel;
    panel.configure(ImageOpsPanel::Mode::CanvasSize, 640, 480);
    Census before = censusOf(&panel);
    REQUIRE(before.numbers.size() == 2);
    REQUIRE(before.checks.size() == 1);
    panel.setConstrainProportions(false);
    typeInto(before.numbers[0], "1024");
    REQUIRE(panel.anchorGrid() != nullptr);
    panel.anchorGrid()->moveBy(1, 1); // Center -> BottomRight, through the widget's own callback

    panel.reapplyTheme(); // rebuilds every widget in the (possibly) new palette

    CHECK(panel.mode() == ImageOpsPanel::Mode::CanvasSize);
    CHECK(panel.pixelWidth() == 1024);
    CHECK(panel.pixelHeight() == 480);
    CHECK_FALSE(panel.constrainProportions());
    // The old AnchorGrid died with the rebuild; the fresh one carries the same value.
    REQUIRE(panel.anchorGrid() != nullptr);
    CHECK(panel.anchorGrid()->value() == render::CanvasAnchor::BottomRight);
    const Census after = censusOf(&panel);
    CHECK(after.numbers.size() == 2);
    CHECK(after.grids.size() == 1);
}
