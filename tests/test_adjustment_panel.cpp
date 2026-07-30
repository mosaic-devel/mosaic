// The S32 adjustment editor panel, built headlessly (no show(), the Type3d-panel pattern): the
// rows are GENERATED from the reflected kind's schema — with the pro layouts owning their
// scalars (Color Balance's wheels, Levels/Threshold's histogram handles) — edits stream through
// the host funnel with per-control field ids (the undo-coalescing key), reflecting a drifted
// bag (undo/redo) moves the controls without firing edits, and Reset re-seeds the schema
// defaults through the same funnel.
#include "ui/adjustment_panel.hpp"
#include "ui/curve_editor.hpp"
#include "ui/paint_chip.hpp"
#include "ui/tone_wheel.hpp"
#include "ui/scrub_slider.hpp"
#include "ui/widgets.hpp"

#include <doctest/doctest.h>

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Group.H>

#include <algorithm>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "common/image.hpp"
#include "core/document.hpp"

using namespace mosaic;

namespace {

struct Census {
    std::vector<ui::ScrubSlider*> sliders;
    std::vector<ui::CheckBox*> checks;
    std::vector<ui::Dropdown*> dropdowns;
    std::vector<ui::ToneWheel*> wheels;
    std::vector<ui::PaintChip*> chips;      // S34-a: Gradient Map's ramp preview
    std::vector<ui::SwatchChip*> swatches;  // S34-a: Photo Filter's Custom colour
};

void collect(const Fl_Group* g, Census& out) {
    for (int i = 0; i < g->children(); ++i) {
        Fl_Widget* c = g->child(i);
        if (auto* s = dynamic_cast<ui::ScrubSlider*>(c)) out.sliders.push_back(s);
        if (auto* cb = dynamic_cast<ui::CheckBox*>(c)) out.checks.push_back(cb);
        if (auto* dd = dynamic_cast<ui::Dropdown*>(c)) out.dropdowns.push_back(dd);
        if (auto* wh = dynamic_cast<ui::ToneWheel*>(c)) out.wheels.push_back(wh);
        if (auto* pc = dynamic_cast<ui::PaintChip*>(c)) out.chips.push_back(pc);
        if (auto* sw = dynamic_cast<ui::SwatchChip*>(c)) out.swatches.push_back(sw);
        if (const auto* sub = dynamic_cast<const Fl_Group*>(c)) collect(sub, out);
    }
}

Census censusOf(const Fl_Group* g) {
    Census c;
    collect(g, c);
    return c;
}

} // namespace

TEST_CASE("the adjustment panel generates its rows from the reflected kind's schema") {
    core::Document doc(1, 1);
    ui::AdjustmentPanel panel;

    // Color Balance: the colorist layout -- three tone-band wheels + the luminosity toggle;
    // no generic sliders (the wheels own the nine band scalars).
    auto cb = doc.makeAdjustment("cb", core::AdjustmentKind::ColorBalance);
    panel.reflect(*cb);
    CHECK(panel.target() == cb->id());
    Census c = censusOf(&panel);
    CHECK(c.wheels.size() == 3);
    CHECK(c.checks.size() == 1);
    CHECK(c.sliders.empty());

    // Exposure: 3 generic scalars; sliders seed at the schema defaults for an empty bag.
    auto exp = doc.makeAdjustment("exp", core::AdjustmentKind::Exposure);
    panel.reflect(*exp);
    CHECK(panel.target() == exp->id());
    c = censusOf(&panel);
    REQUIRE(c.sliders.size() == 3);
    CHECK(c.checks.empty());
    CHECK(c.wheels.empty());
    CHECK(c.sliders[0]->value() == 0.0); // exposure
    CHECK(c.sliders[2]->value() == 1.0); // gamma

    // An out-of-range bag shows CLAMPED values (the same schema read the compositor uses).
    exp->params()["exposure"] = 99.0;
    panel.reflect(*exp);
    CHECK(c.sliders[0]->value() == 5.0); // the declared max

    // Grayscale (S32 follow-up): a method dropdown + strength and grays sliders, seeded at
    // Luma / 100% / 256 (continuous).
    auto gs = doc.makeAdjustment("gs", core::AdjustmentKind::Grayscale);
    panel.reflect(*gs);
    c = censusOf(&panel);
    REQUIRE(c.dropdowns.size() == 1);
    REQUIRE(c.sliders.size() == 2);
    CHECK(c.dropdowns[0]->value() == static_cast<int>(core::GrayscaleMethod::Luma));
    CHECK(c.sliders[0]->value() == 100.0); // strength
    CHECK(c.sliders[1]->value() == 256.0); // grays (continuous)

    // Levels / Threshold: the histogram handles own every scalar -- no generic rows at all.
    auto lv = doc.makeAdjustment("lv", core::AdjustmentKind::Levels);
    panel.reflect(*lv);
    c = censusOf(&panel);
    CHECK(c.sliders.empty());
    auto th = doc.makeAdjustment("th", core::AdjustmentKind::Threshold);
    panel.reflect(*th);
    c = censusOf(&panel);
    CHECK(c.sliders.empty());
}

TEST_CASE("panel edits stream through the funnel; reflect() syncs drift without firing edits") {
    core::Document doc(1, 1);
    auto adj = doc.makeAdjustment("exp", core::AdjustmentKind::Exposure);
    adj->params()["exposure"] = 0.25;

    std::vector<std::string> fieldIds;
    std::map<std::string, double> bag = adj->params();
    ui::AdjustmentPanel panel;
    panel.setOnEdit([&](const std::string& id,
                        std::function<void(std::map<std::string, double>&)> mutate) {
        fieldIds.push_back(id);
        mutate(bag); // the host applies the mutation to the layer's bag (as a command)
    });
    panel.reflect(*adj);

    Census c = censusOf(&panel);
    REQUIRE(c.sliders.size() == 3);
    CHECK(c.sliders[0]->value() == 0.25);

    // A slider edit fires the funnel with its per-control field id and the mutated bag.
    c.sliders[0]->value(1.5);
    c.sliders[0]->do_callback();
    REQUIRE(fieldIds.size() == 1);
    CHECK(fieldIds.back() == "adjust:exposure");
    CHECK(bag.at("exposure") == 1.5);

    // Undo moved the bag under the panel: reflect() re-syncs the control, firing NO edit.
    adj->params()["exposure"] = -1.0;
    panel.reflect(*adj);
    CHECK(c.sliders[0]->value() == -1.0);
    CHECK(fieldIds.size() == 1);

    // Reset re-seeds the schema defaults through the funnel (its own coalesce key).
    bool resetFound = false;
    for (int i = 0; i < panel.children(); ++i) {
        if (auto* b = dynamic_cast<ui::FlatButton*>(panel.child(i))) {
            b->do_callback();
            resetFound = true;
            break;
        }
    }
    REQUIRE(resetFound);
    REQUIRE(fieldIds.size() == 2);
    CHECK(fieldIds.back() == "adjust:reset");
    CHECK(bag.at("exposure") == 0.0); // the schema default
}

TEST_CASE("a color-balance wheel drag writes its band's key triple, mean preserved") {
    core::Document doc(1, 1);
    auto adj = doc.makeAdjustment("cb", core::AdjustmentKind::ColorBalance);

    std::vector<std::string> fieldIds;
    std::map<std::string, double> bag;
    ui::AdjustmentPanel panel;
    panel.setOnEdit([&](const std::string& id,
                        std::function<void(std::map<std::string, double>&)> mutate) {
        fieldIds.push_back(id);
        mutate(bag);
    });
    panel.reflect(*adj);
    Census c = censusOf(&panel);
    REQUIRE(c.wheels.size() == 3);

    // Drag the first (shadows) wheel fully toward red: cr rails at +100, the green/blue axes
    // take the complementary projection.
    c.wheels[0]->dragToValue(1.0, 0.0);
    REQUIRE(!fieldIds.empty());
    CHECK(fieldIds.back() == "adjust:wheel_shadows");
    CHECK(bag.at("shadows_cr") == doctest::Approx(100.0));
    CHECK(bag.at("shadows_mg") == doctest::Approx(-50.0));
    CHECK(bag.at("shadows_yb") == doctest::Approx(-50.0));
    CHECK(bag.count("midtones_cr") == 0); // the other bands are untouched

    // A hand-authored achromatic mean survives a wheel drag (the wheel edits chroma only).
    adj->params() = {{"midtones_cr", 30.0}, {"midtones_mg", 30.0}, {"midtones_yb", 30.0}};
    panel.reflect(*adj);
    c.wheels[1]->dragToValue(0.1, 0.0);
    const double sum = bag.at("midtones_cr") + bag.at("midtones_mg") + bag.at("midtones_yb");
    CHECK(sum / 3.0 == doctest::Approx(30.0)); // the mean is intact
    CHECK(bag.at("midtones_cr") > bag.at("midtones_mg")); // and the chroma moved toward red
}

// ---------------------------------------------------------------------------------------------
// S34: the Curves layout (channel picker + curve plot; the knots live in the same double bag)
// ---------------------------------------------------------------------------------------------

namespace {

// Push at a curve coordinate inside `e`'s plot. Mirrors tests/test_curve_editor.cpp: an
// Fl_Widget needs no display to handle an event, and Fl::e_* are public. e_number matters here
// because the panel's gesture counter reads Fl::event() to decide when a new undo step starts.
constexpr int kCurvePad = 8; // curve_editor.cpp's plot inset

void pressCurve(ui::CurveEditor& e, double cx, double cy) {
    Fl::e_number = FL_PUSH;
    Fl::e_x = e.x() + kCurvePad + static_cast<int>(cx * (e.w() - 2 * kCurvePad));
    Fl::e_y = e.y() + kCurvePad + static_cast<int>((1.0 - cy) * (e.h() - 2 * kCurvePad));
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    Fl::e_clicks = 0;
    static_cast<Fl_Widget&>(e).handle(FL_PUSH); // the override is protected, the base is public
}

ui::CurveEditor* curveOf(const Fl_Group* g) {
    for (int i = 0; i < g->children(); ++i)
        if (auto* ce = dynamic_cast<ui::CurveEditor*>(g->child(i))) return ce;
    return nullptr;
}

} // namespace

TEST_CASE("the Curves layout: a plot + a channel picker, and NO generic scalar rows") {
    core::Document doc(1, 1);
    ui::AdjustmentPanel panel;
    auto adj = doc.makeAdjustment("cv", core::AdjustmentKind::Curves);
    panel.reflect(*adj);

    const Census c = censusOf(&panel);
    CHECK(c.sliders.empty()); // Curves has no scalars at all
    CHECK(c.wheels.empty());
    REQUIRE(c.dropdowns.size() == 1);        // the channel picker
    CHECK(c.dropdowns[0]->value() == 0);     // seeded on the composite (RGB) curve
    ui::CurveEditor* ce = curveOf(&panel);
    REQUIRE(ce != nullptr);
    CHECK(ce->curve().isIdentity()); // an empty bag = four identity curves

    // A stored per-channel curve, with the bag pointing the editor at that channel: reflect()
    // must seed the PLOT from it, not just the dropdown.
    const core::brush::Curve lift(std::vector<core::brush::CurvePoint>{
        {0.0, 0.0, false}, {0.5, 0.8, false}, {1.0, 1.0, false}});
    core::setAdjustmentCurve(adj->params(), core::CurveChannel::Red, lift);
    adj->params()["channel"] = static_cast<double>(core::CurveChannel::Red);
    panel.reflect(*adj);
    CHECK(c.dropdowns[0]->value() == static_cast<int>(core::CurveChannel::Red));
    REQUIRE(ce->curve().points().size() == 3);
    CHECK(ce->curve().points()[1].y == doctest::Approx(0.8));
}

TEST_CASE("Curves panel: the channel picker re-seeds the plot and streams one edit") {
    core::Document doc(1, 1);
    auto adj = doc.makeAdjustment("cv", core::AdjustmentKind::Curves);
    const core::brush::Curve lift(std::vector<core::brush::CurvePoint>{
        {0.0, 0.0, false}, {0.5, 0.8, false}, {1.0, 1.0, false}});
    core::setAdjustmentCurve(adj->params(), core::CurveChannel::Red, lift);

    std::vector<std::string> fieldIds;
    std::map<std::string, double> bag = adj->params();
    ui::AdjustmentPanel panel;
    panel.setOnEdit([&](const std::string& id,
                        std::function<void(std::map<std::string, double>&)> mutate) {
        fieldIds.push_back(id);
        mutate(bag);
    });
    panel.reflect(*adj);
    const Census c = censusOf(&panel);
    REQUIRE(c.dropdowns.size() == 1);
    ui::CurveEditor* ce = curveOf(&panel);
    REQUIRE(ce != nullptr);
    CHECK(ce->curve().isIdentity()); // the bag says channel 0 (composite), which is untouched

    c.dropdowns[0]->value(static_cast<int>(core::CurveChannel::Red));
    c.dropdowns[0]->do_callback();
    REQUIRE(fieldIds.size() == 1);
    CHECK(fieldIds.back() == "adjust:channel");
    CHECK(bag.at("channel") == static_cast<double>(core::CurveChannel::Red));
    // ... and the plot now holds the RED curve, without a reflect() in between.
    REQUIRE(ce->curve().points().size() == 3);
    CHECK(ce->curve().points()[1].y == doctest::Approx(0.8));
}

TEST_CASE("Curves panel: a plot edit writes knots, one undo step per gesture; Reset erases") {
    core::Document doc(1, 1);
    auto adj = doc.makeAdjustment("cv", core::AdjustmentKind::Curves);
    core::seedAdjustmentDefaults(*adj);

    std::vector<std::string> fieldIds;
    std::map<std::string, double> bag = adj->params();
    ui::AdjustmentPanel panel;
    panel.setOnEdit([&](const std::string& id,
                        std::function<void(std::map<std::string, double>&)> mutate) {
        fieldIds.push_back(id);
        mutate(bag);
    });
    panel.reflect(*adj);
    ui::CurveEditor* ce = curveOf(&panel);
    REQUIRE(ce != nullptr);

    // Click empty plot space: the editor adds a point and commits, which streams the whole
    // curve through the funnel as indexed knots in the ordinary double bag.
    pressCurve(*ce, 0.5, 0.75);
    REQUIRE(fieldIds.size() == 1);
    CHECK(bag.count("curve_rgb_n") == 1);
    CHECK(bag.at("curve_rgb_n") == 3.0);
    CHECK(bag.count("curve_r_n") == 0); // only the shown channel was written
    const core::brush::Curve stored = core::adjustmentCurve(bag, core::CurveChannel::Composite);
    CHECK_FALSE(stored.isIdentity());

    // A SECOND gesture must not merge into the first one's undo step.
    const std::string first = fieldIds.back();
    pressCurve(*ce, 0.25, 0.4);
    REQUIRE(fieldIds.size() == 2);
    CHECK(fieldIds.back() != first);
    CHECK(bag.at("curve_rgb_n") == 4.0);
    // ... but a drag WITHIN a gesture keeps the id, so a point drag is one step.
    Fl::e_number = FL_DRAG;
    Fl::e_x = ce->x() + ce->w() / 2;
    Fl::e_y = ce->y() + ce->h() / 3;
    static_cast<Fl_Widget&>(*ce).handle(FL_DRAG);
    REQUIRE(fieldIds.size() == 3);
    CHECK(fieldIds.back() == fieldIds[1]);

    // Reset erases the knots (they are not schema rows, so re-seeding cannot restore them).
    bool resetFound = false;
    for (int i = 0; i < panel.children(); ++i) {
        if (auto* b = dynamic_cast<ui::FlatButton*>(panel.child(i))) {
            b->do_callback();
            resetFound = true;
            break;
        }
    }
    REQUIRE(resetFound);
    CHECK(fieldIds.back() == "adjust:reset");
    CHECK(bag.count("curve_rgb_n") == 0);
    CHECK(bag.at("channel") == 0.0);
    CHECK(core::adjustmentCurve(bag, core::CurveChannel::Composite).isIdentity());
}

// ---------------------------------------------------------------------------------------------
// S34-a: the Curves backdrop histogram, and the two layouts with a host-owned editor bubble.
// ---------------------------------------------------------------------------------------------

namespace {

// A backdrop the four histograms cannot confuse: every pixel is a dark red, so the RED channel's
// mass sits high and the GREEN/BLUE channels' sits at zero.
common::Image redBackdrop() {
    common::Image img(8, 8);
    for (std::size_t p = 0; p < img.rgba.size(); p += 4) {
        img.rgba[p] = 200;
        img.rgba[p + 1] = 20;
        img.rgba[p + 2] = 20;
        img.rgba[p + 3] = 255;
    }
    return img;
}

// The bin index carrying the most mass -- "where this channel's pixels actually are".
std::size_t peakBin(const std::vector<float>& bins) {
    return static_cast<std::size_t>(
        std::max_element(bins.begin(), bins.end()) - bins.begin());
}

} // namespace

TEST_CASE("Curves plot: the backdrop histogram follows the channel picker") {
    core::Document doc(1, 1);
    auto adj = doc.makeAdjustment("cv", core::AdjustmentKind::Curves);

    std::map<std::string, double> bag = adj->params();
    ui::AdjustmentPanel panel;
    panel.setBackdropProvider([] { return redBackdrop(); });
    panel.setOnEdit([&](const std::string&,
                        std::function<void(std::map<std::string, double>&)> mutate) {
        mutate(bag);
    });
    panel.reflect(*adj);

    ui::CurveEditor* ce = curveOf(&panel);
    REQUIRE(ce != nullptr);
    // The composite curve shows the LUMA distribution -- the same 256-bin, alpha-weighted,
    // sqrt-normalized data path the Levels/Threshold strips have always drawn.
    REQUIRE(ce->histogram().size() == 256);
    const std::size_t lumaPeak = peakBin(ce->histogram());
    CHECK(lumaPeak == static_cast<std::size_t>(0.3 * 200 + 0.59 * 20 + 0.11 * 20));

    // Switching to Red re-seeds the plot's histogram IN THE SAME callback, so the distribution
    // under the curve is the one that curve edits.
    const Census c = censusOf(&panel);
    REQUIRE(c.dropdowns.size() == 1);
    c.dropdowns[0]->value(static_cast<int>(core::CurveChannel::Red));
    c.dropdowns[0]->do_callback();
    CHECK(peakBin(ce->histogram()) == 200);
    c.dropdowns[0]->value(static_cast<int>(core::CurveChannel::Blue));
    c.dropdowns[0]->do_callback();
    CHECK(peakBin(ce->histogram()) == 20);

    // No backdrop provider = no histogram at all, which is the widget's default state (the
    // tablet-settings consumer has no image to plot).
    ui::AdjustmentPanel bare;
    bare.reflect(*adj);
    ui::CurveEditor* plain = curveOf(&bare);
    REQUIRE(plain != nullptr);
    CHECK(plain->histogram().empty());
}

TEST_CASE("the Gradient Map layout: a ramp chip + the Reverse toggle, and Reset erases the stops") {
    core::Document doc(1, 1);
    auto adj = doc.makeAdjustment("gm", core::AdjustmentKind::GradientMap);
    core::seedAdjustmentDefaults(*adj);
    // A stored ramp the chip has to pick up (the gradient is NOT a schema row).
    core::vec::Gradient duotone = core::defaultGradientMap();
    duotone.stops = {{0.0, common::ColorF{0.0F, 0.0F, 0.4F, 1.0F}, 0.5},
                     {1.0, common::ColorF{1.0F, 0.8F, 0.2F, 1.0F}, 0.5}};
    core::setAdjustmentGradientMap(adj->params(), duotone);

    std::vector<std::string> fieldIds;
    std::map<std::string, double> bag = adj->params();
    ui::AdjustmentPanel panel;
    panel.setOnEdit([&](const std::string& id,
                        std::function<void(std::map<std::string, double>&)> mutate) {
        fieldIds.push_back(id);
        mutate(bag);
    });
    panel.reflect(*adj);

    const Census c = censusOf(&panel);
    REQUIRE(c.chips.size() == 1);  // the ramp preview: the editing surface is the host's bubble
    CHECK(c.checks.size() == 1);   // Reverse
    CHECK(c.sliders.empty());      // ... and nothing else: the ramp is the control
    const auto* shown = std::get_if<core::vec::Gradient>(&c.chips[0]->paint());
    REQUIRE(shown != nullptr);
    CHECK(shown->stops == duotone.stops); // reflect() seeded the chip from the bag

    // Reset ERASES the stops -- absent spells the default black-to-white ramp, so re-seeding the
    // schema alone could never get back to it (the Curves-knots rule, one kind over).
    bool resetFound = false;
    for (int i = 0; i < panel.children(); ++i) {
        if (auto* b = dynamic_cast<ui::FlatButton*>(panel.child(i))) {
            b->do_callback();
            resetFound = true;
            break;
        }
    }
    REQUIRE(resetFound);
    CHECK(fieldIds.back() == "adjust:reset");
    CHECK(bag.count("gm_n") == 0);
    CHECK(bag.at("reverse") == 0.0);
    CHECK(core::adjustmentGradientMap(bag).stops == core::defaultGradientMap().stops);
}

TEST_CASE("the Photo Filter layout: the Custom colour is ONE swatch, not three sliders") {
    core::Document doc(1, 1);
    auto adj = doc.makeAdjustment("pf", core::AdjustmentKind::PhotoFilter);
    core::seedAdjustmentDefaults(*adj);

    std::vector<std::string> fieldIds;
    std::map<std::string, double> bag = adj->params();
    ui::AdjustmentPanel panel;
    panel.setOnEdit([&](const std::string& id,
                        std::function<void(std::map<std::string, double>&)> mutate) {
        fieldIds.push_back(id);
        mutate(bag);
    });
    panel.reflect(*adj);

    Census c = censusOf(&panel);
    REQUIRE(c.dropdowns.size() == 1);       // the preset picker
    REQUIRE(c.swatches.size() == 1);        // ... and the colour it resolves to
    CHECK(c.sliders.size() == 1);           // Density ONLY -- the three color_* rows are the swatch
    CHECK(c.checks.size() == 1);            // Preserve luminosity
    // A named preset owns its colour: the swatch previews it and is not clickable.
    CHECK(c.swatches[0]->colour() ==
          core::photoFilterPresetColor(core::PhotoFilterPreset::Warming85));

    // Picking a colour IS choosing Custom: one edit moves the preset and the three rows together,
    // so the swatch never shows a colour the compositor is not using.
    panel.setPickedColor({10, 20, 30, 255});
    REQUIRE(fieldIds.size() == 1);
    CHECK(fieldIds.back() == "adjust:filter_color");
    CHECK(bag.at("filter") == static_cast<double>(core::PhotoFilterPreset::Custom));
    CHECK(bag.at("color_r") == 10.0);
    CHECK(bag.at("color_g") == 20.0);
    CHECK(bag.at("color_b") == 30.0);
    adj->params() = bag;
    panel.reflect(*adj);
    CHECK(c.swatches[0]->colour() == common::Color8{10, 20, 30, 255});

    // A kind without a swatch must not take a pick (the host router asks whichever panel is up).
    auto other = doc.makeAdjustment("exp", core::AdjustmentKind::Exposure);
    panel.reflect(*other);
    panel.setPickedColor({1, 2, 3, 255});
    CHECK(fieldIds.size() == 1);
}
