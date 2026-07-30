// The layer dock's ROW BACKGROUND ramp (S15-c multi-selection).
//
// A row that is in the move-selection but is not the ACTIVE layer used to fall through to plain
// pal.panelBg -- pixel-identical to a row that is not selected at all -- so a multi-selection read
// only through its dots. It now carries pal.controlSelected. Two claims need pinning, and neither
// survives a "a row was drawn" test:
//
//   1. the tint is ORDERED and REAL in BOTH palettes (panelBg < controlSelected < controlHover <
//      controlActive, measured from panelBg), so it cannot end up visible in one theme and
//      invisible in the other -- this project has shipped a panel-coloured element on a
//      panel-coloured ground before; and
//   2. LayerRow::draw() actually FILLS with it. So the rows are shot and their pixels compared:
//      a case that compared two rows which were both really panelBg would pass while proving
//      nothing, hence every comparison below is a numeric distance with a floor under it.
//
// A LayerRow is a plain widget, not an Fl_Window, so it gives its draw() to an Fl_Image_Surface
// happily (an unshown Fl_Window would come back black).
#include "ui/layer_panel.hpp"

#include "common/image.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "ui/theme.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdlib>
#include <vector>

using namespace mosaic;

namespace {

// "The fills differ" has to be a NUMBER: two values that are both panelBg would compare equal and
// the case would be worthless either way.
[[nodiscard]] int channelDistance(common::Color8 a, common::Color8 b) {
    return std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)) +
           std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)) +
           std::abs(static_cast<int>(a.b) - static_cast<int>(b.b));
}

// How far a fill sits from the panel ground. The dark theme ramps UP from panelBg and the light
// theme ramps DOWN, so only the distance is comparable across the two.
[[nodiscard]] int fromPanel(const ui::Palette& pal, common::Color8 c) {
    return channelDistance(c, pal.panelBg);
}

// Every LayerRow in the panel's tree, whatever it is nested inside (they live in the ScrollView).
// Re-walked on demand rather than cached: a panel state change may rebuild the rows and leave a
// cached pointer dangling.
[[nodiscard]] std::vector<ui::LayerRow*> findRows(Fl_Group& group) {
    std::vector<ui::LayerRow*> out;
    for (int i = 0; i < group.children(); ++i) {
        Fl_Widget* child = group.child(i);
        if (auto* row = dynamic_cast<ui::LayerRow*>(child)) {
            out.push_back(row);
        } else if (auto* sub = dynamic_cast<Fl_Group*>(child)) {
            for (ui::LayerRow* nested : findRows(*sub))
                out.push_back(nested);
        }
    }
    return out;
}

// One real draw() of a row, off screen, sampled at (sx, sy). The surface is pre-filled MAGENTA: the
// row paints its whole rect, so a magenta sample means it painted nothing at all.
[[nodiscard]] common::Color8 shootRowFill(ui::LayerRow& row, int sx, int sy) {
    const int w = row.w();
    const int h = row.h();
    auto* surf = new Fl_Image_Surface(w, h);
    Fl_Surface_Device::push_current(surf);
    fl_color(fl_rgb_color(255, 0, 255));
    fl_rectf(0, 0, w, h);
    surf->draw(&row, 0, 0);
    Fl_Surface_Device::pop_current();

    Fl_RGB_Image* img = surf->image();
    const auto* px = reinterpret_cast<const unsigned char*>(img->data()[0]);
    const int d = img->d();
    const int iw = img->w();
    const unsigned char* p = px + (static_cast<std::size_t>(sy) * iw + sx) * d;
    const common::Color8 out{p[0], p[1], p[2], 255};
    delete img;
    delete surf;
    return out;
}

} // namespace

// The ramp itself, with no FLTK in the way: the ORDER and the SIZE of each step, in both palettes.
TEST_CASE("layerRowBackground ramps panelBg -> controlSelected -> controlHover -> controlActive") {
    for (const ui::Palette& pal : {ui::darkPalette(), ui::lightPalette()}) {
        INFO((pal.dark ? "dark palette" : "light palette")); // parens: doctest's * binds before ?:

        // Precedence: active > hover > selected. Hover outranking selection is what keeps the
        // pointer's feedback alive on a row that is already in the set.
        CHECK(ui::layerRowBackground(pal, true, false, false, false) == pal.panelBg);
        CHECK(ui::layerRowBackground(pal, true, false, true, false) == pal.controlSelected);
        CHECK(ui::layerRowBackground(pal, true, false, false, true) == pal.controlHover);
        CHECK(ui::layerRowBackground(pal, true, false, true, true) == pal.controlHover);
        CHECK(ui::layerRowBackground(pal, true, true, true, true) == pal.controlActive);
        // The greyed panel (inpaint chrome lock) still paints the rest state only.
        CHECK(ui::layerRowBackground(pal, false, true, true, true) == pal.panelBg);

        // The step exists at all -- in BOTH themes. A tint that vanished into panelBg in the theme
        // the user happens to run is the whole failure mode this token was added to avoid.
        CHECK(fromPanel(pal, pal.controlSelected) >= 12);
        // ... and it is ordered: slighter than the pointer's fill, far slighter than the active.
        CHECK(fromPanel(pal, pal.controlSelected) < fromPanel(pal, pal.controlHover));
        CHECK(fromPanel(pal, pal.controlHover) < fromPanel(pal, pal.controlActive));
        // "Slight" spelled out: the selected fill sits CLOSER to the ground than to the active row.
        CHECK(fromPanel(pal, pal.controlSelected) <
              channelDistance(pal.controlSelected, pal.controlActive));
        // And the pointer's own step stays legible on top of it (hover on a selected row must not
        // be a 2-level nudge nobody can see).
        CHECK(channelDistance(pal.controlSelected, pal.controlHover) >= 12);
    }
}

// ... and the rows really paint it. Three layers, two of them selected, one of those two active:
// that is the exact configuration the fill was added for, and all three fills are read back out of
// a real draw() and compared against each other.
TEST_CASE("a selected-but-not-active layer row draws its own fill, in both palettes") {
#if defined(__SANITIZE_ADDRESS__) ||                                                               \
    (defined(__has_feature) && __has_feature(address_sanitizer)) // NOLINT
    return; // FLTK/X11 internals leak on teardown under LeakSanitizer; this is not a memory test
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    core::Document doc(16, 16);
    const core::LayerId bottom = doc.root().addOnTop(doc.makeRaster("bottom")).id();
    const core::LayerId middle = doc.root().addOnTop(doc.makeRaster("middle")).id();
    const core::LayerId top = doc.root().addOnTop(doc.makeRaster("top")).id();

    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    panel.setActive(top);
    panel.setMoveSelection({top, middle}); // `bottom` is the control: dotless, unselected
    REQUIRE(panel.multiSelectActive());
    REQUIRE(findRows(panel).size() == 3);

    // Sampled near the row's TOP-LEFT: the eye/lock glyphs and the thumbnail are vertically centred
    // and the selection dot hugs the right edge, so this pixel is nothing but the fill.
    const auto fillOf = [&panel](core::LayerId id) {
        ui::LayerRow* found = nullptr;
        for (ui::LayerRow* row : findRows(panel))
            if (row->layerId() == id)
                found = row;
        REQUIRE(found != nullptr);
        return shootRowFill(*found, 2, 2);
    };

    const ui::Palette saved = ui::activePalette();
    for (const ui::Palette& pal : {ui::darkPalette(), ui::lightPalette()}) {
        ui::applyTheme(pal);
        INFO((pal.dark ? "dark palette" : "light palette")); // parens: doctest's * binds before ?:

        const common::Color8 activeFill = fillOf(top);
        const common::Color8 selectedFill = fillOf(middle);
        const common::Color8 plainFill = fillOf(bottom);

        CHECK(plainFill == pal.panelBg);              // the unselected control row: bare ground
        CHECK(selectedFill == pal.controlSelected);   // the row this whole change exists for
        CHECK(activeFill == pal.controlActive);       // ... and the active row is untouched

        // The claims restated as distances, so a future palette edit that quietly collapsed two of
        // these into the same colour fails here rather than shipping.
        CHECK(channelDistance(selectedFill, plainFill) >= 12);
        CHECK(channelDistance(activeFill, selectedFill) >= 12);
        CHECK(channelDistance(activeFill, plainFill) >
              channelDistance(selectedFill, plainFill)); // the active row stays the loudest

        // S15-e "All selected layers": every selected dot goes accent. The FILL is a separate
        // channel and must not budge -- the tint says "in the set", the dot's ink says "the edit
        // lands here". (The dot is at the right edge; the sample never sees it either way.)
        panel.setMultiSelectionMode(ui::LayerPanel::MultiSelectMode::All);
        REQUIRE(panel.editsAllSelected());
        CHECK(fillOf(middle) == selectedFill);
        CHECK(fillOf(top) == activeFill);
        panel.setMultiSelectionMode(ui::LayerPanel::MultiSelectMode::Disabled);
    }
    ui::applyTheme(saved);
}
