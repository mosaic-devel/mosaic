#include "common/image.hpp"
#include "ui/color_surfaces.hpp" // toFl
#include "ui/theme.hpp"
#include "ui/widgets.hpp" // FlatButton / FilledButton / GlyphButton (the re-theme staleness case)

#include <cstdlib>
#include <doctest/doctest.h>
#include <initializer_list>

using namespace mosaic;

namespace {
// Rec.601 luma, good enough for contrast sanity checks.
int luma(common::Color8 c) {
    return (c.r * 299 + c.g * 587 + c.b * 114) / 1000;
}
} // namespace

TEST_CASE("dark and light palettes have the expected polarity") {
    const ui::Palette d = ui::darkPalette();
    const ui::Palette l = ui::lightPalette();
    CHECK(d.dark);
    CHECK_FALSE(l.dark);
    // Dark theme: dark surfaces under light text; light theme: the reverse.
    CHECK(luma(d.windowBg) < luma(d.text));
    CHECK(luma(l.windowBg) > luma(l.text));
    // The two themes' window grounds are markedly different.
    CHECK(luma(l.windowBg) - luma(d.windowBg) > 100);
}

TEST_CASE("text stays readable against its surfaces") {
    for (const ui::Palette p : {ui::darkPalette(), ui::lightPalette()}) {
        CHECK(std::abs(luma(p.text) - luma(p.windowBg)) > 90);
        CHECK(std::abs(luma(p.text) - luma(p.panelBg)) > 90);
        CHECK(std::abs(luma(p.text) - luma(p.controlBg)) > 80);
    }
}

TEST_CASE("resolvePalette honors explicit modes") {
    // Explicit modes are deterministic and never consult the OS (no subprocess in tests).
    CHECK(ui::resolvePalette(ui::ThemeMode::Dark).dark);
    CHECK_FALSE(ui::resolvePalette(ui::ThemeMode::Light).dark);
}

TEST_CASE("theme observers fire on re-theme and the active palette updates") {
    ui::applyTheme(ui::darkPalette()); // ensure we're past the first-apply gate
    int hits = 0;
    {
        ui::ThemeSubscription sub([&] { ++hits; });
        ui::applyTheme(ui::lightPalette());
        CHECK(hits == 1);
        CHECK_FALSE(ui::activePalette().dark); // applyTheme swapped the active palette before firing
        ui::applyTheme(ui::darkPalette());
        CHECK(hits == 2);
        CHECK(ui::activePalette().dark);
    }
    // After the subscription is destroyed the callback must not fire again (no use-after-free).
    ui::applyTheme(ui::lightPalette());
    CHECK(hits == 2);
}

TEST_CASE("a button's PRESSED fill follows a re-theme (the stuck-pressed-state staleness)") {
    // Fl_Button::draw() paints the down box with selection_color(), and FlatButton's constructor
    // bakes a concrete controlActive RGB into it (controlActive has no semantic FLTK slot to ride --
    // FL_SELECTION_COLOR is the accent). Without a refresh in reapplyTheme() that RGB survives a
    // theme switch, so every toggle sitting DOWN keeps the previous palette's slab while everything
    // around it changes. The base owns the refresh; the check runs over the reachable subclasses
    // because an override that forgets to chain re-opens the bug for its own subtree.
    const ui::Palette dark = ui::darkPalette();
    const ui::Palette light = ui::lightPalette();
    REQUIRE(dark.controlActive.r != light.controlActive.r); // the two must differ or this proves nothing

    ui::applyTheme(dark);
    ui::FlatButton flat(0, 0, 40, 20);
    ui::FilledButton filled(0, 0, 40, 20);
    ui::GlyphButton glyph(0, 0, 40, 20, ui::GlyphButton::Kind::Bold);
    CHECK(flat.selection_color() == ui::sf::toFl(dark.controlActive));
    CHECK(filled.selection_color() == ui::sf::toFl(dark.controlActive));
    CHECK(glyph.selection_color() == ui::sf::toFl(dark.controlActive));

    ui::applyTheme(light); // fires every observer, including each button's
    CHECK(flat.selection_color() == ui::sf::toFl(light.controlActive));
    CHECK(filled.selection_color() == ui::sf::toFl(light.controlActive));
    CHECK(glyph.selection_color() == ui::sf::toFl(light.controlActive));
    // FilledButton overrides reapplyTheme: chaining must not cost it its accent rest fill.
    CHECK(filled.color() == ui::sf::toFl(light.accent));

    // And back. (A stale value would coincidentally MATCH here, so this leg does not detect the
    // pressed-fill bug on its own -- the dark->light leg above is the one that does. It is kept
    // because filled.color() below is direction-sensitive: the two palettes' accents differ.)
    ui::applyTheme(dark);
    CHECK(flat.selection_color() == ui::sf::toFl(dark.controlActive));
    CHECK(filled.selection_color() == ui::sf::toFl(dark.controlActive));
    CHECK(glyph.selection_color() == ui::sf::toFl(dark.controlActive));
    CHECK(filled.color() == ui::sf::toFl(dark.accent));
}

TEST_CASE("renderAAPrims: disc and ring coverage over the under-sampler") {
    const common::Color8 ground{10, 20, 30, 255};
    const common::Color8 white{255, 255, 255, 255};
    const auto under = [&](int, int) { return ground; };
    const auto at = [](const common::Image& img, int x, int y) {
        const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
        return common::Color8{img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
    };

    // A filled disc: solid at the centre, untouched ground outside, AA in between.
    const common::Image disc =
        ui::renderAAPrims(0, 0, 15, 15, under, {{7.5, 7.5, 5.0, 0.0, white}});
    CHECK(disc.width == 15);
    CHECK(at(disc, 7, 7).r == 255);  // centre: pure fill
    CHECK(at(disc, 0, 0).r == 10);   // far corner: pure ground
    CHECK(at(disc, 7, 2).r > 10);    // edge pixel (dist ~5): blended
    CHECK(at(disc, 7, 2).r < 255);
    CHECK(at(disc, 7, 7).a == 255);  // opaque output (blitted, not composited)

    // A stroked ring: the centre keeps the ground, the rim takes the stroke colour.
    const common::Image ring =
        ui::renderAAPrims(0, 0, 15, 15, under, {{7.5, 7.5, 5.0, 2.0, white}});
    CHECK(at(ring, 7, 7).r == 10);  // hole: untouched
    CHECK(at(ring, 7, 2).r == 255); // on the rim (dist 5.0): full stroke
    CHECK(at(ring, 0, 0).r == 10);  // outside: untouched

    // The origin offsets the sampling space: same ring shifted.
    const common::Image off =
        ui::renderAAPrims(100, 200, 15, 15, under, {{107.5, 207.5, 5.0, 2.0, white}});
    CHECK(at(off, 7, 2).r == 255);

    CHECK(ui::renderAAPrims(0, 0, 0, 5, under, {}).empty()); // degenerate patch
}
