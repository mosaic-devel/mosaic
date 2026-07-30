// The selection-morphology corner panel (Grow/Shrink/Feather/Smooth live preview). Like the 3D
// popup test, the panel is a child sub-window; constructing + rebuilding it (via reapplyTheme)
// WITHOUT show() is safe headlessly, so the layout + the mode/amount state pin without a display.
#include "ui/select_morph_panel.hpp"

#include <doctest/doctest.h>

using mosaic::ui::SelectMorphMode;
using mosaic::ui::SelectMorphPanel;

// The panel builds in its ctor (static layout), at a fixed footprint that hugs its content.
TEST_CASE("the morphology panel builds at its fixed footprint with controls") {
    SelectMorphPanel panel;
    CHECK(panel.w() == 250);
    CHECK(panel.h() == 154); // header + 2 rows + footer (see select_morph_panel.cpp)
    CHECK(panel.children() > 0); // header, captions, dropdown, slider, Apply/Cancel
}

// configure() seeds the op + amount before a show; mode()/amount() read them back for the host's
// preview/apply. A theme rebuild preserves both (build() re-seeds the widgets from the members).
TEST_CASE("configure seeds the mode + amount and survives a theme rebuild") {
    SelectMorphPanel panel;
    panel.configure(SelectMorphMode::Feather, 12.0);
    CHECK(static_cast<int>(panel.mode()) == static_cast<int>(SelectMorphMode::Feather));
    CHECK(panel.amount() == doctest::Approx(12.0));

    panel.configure(SelectMorphMode::Shrink, 20.0);
    CHECK(static_cast<int>(panel.mode()) == static_cast<int>(SelectMorphMode::Shrink));
    CHECK(panel.amount() == doctest::Approx(20.0));

    panel.reapplyTheme(); // rebuild in a (possibly) new palette
    CHECK(panel.w() == 250);
    CHECK(static_cast<int>(panel.mode()) == static_cast<int>(SelectMorphMode::Shrink)); // op held
    CHECK(panel.amount() == doctest::Approx(20.0));
}
