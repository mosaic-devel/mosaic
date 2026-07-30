// Hyphenation *layout* tests (docs/type-deferred-features.md §1): the wrap-loop integration, as seen
// through TextShaper::layout. System fonts vary, so these assert STRUCTURE, not pixels. The actual
// break decisions come from the installed hyph_en_US.dic; where that is absent (a bare CI sandbox)
// the dictionary-dependent assertion is skipped, so the test passes everywhere. The break algorithm
// itself is pinned deterministically in test_text_hyphenator.cpp.
#include <doctest/doctest.h>

#include <cstddef>

#include "core/text/hyphenator.hpp"
#include "core/text/shaping.hpp"
#include "core/text/text_edit.hpp"
#include "core/text/text_model.hpp"
#include "platform/font_db.hpp"

using namespace mosaic::core::text;
using mosaic::common::ColorF;

namespace {

bool fontsAvailable(const mosaic::platform::FontDB& db) {
    if (db.families().empty()) return false;
    FontRef r;
    r.family = db.defaultFamily();
    return db.resolve(r).has_value();
}

CharStyle styleOf(float size, const std::string& family) {
    CharStyle s;
    s.setSolidFill(ColorF{0, 0, 0, 1});
    s.sizePx = size;
    s.font.family = family;
    return s;
}

ShapedBlock layoutParagraph(const mosaic::platform::FontDB& db, const std::string& text,
                            double boxW, bool hyphenate) {
    TextShaper shaper;
    shaper.setDefaultLanguage("en-US");
    TextBlock block = makeBlock(text, styleOf(24.0f, db.defaultFamily()), TextFrame::Area);
    block.areaSize = {boxW, 400.0};
    for (Paragraph& p : block.paragraphs) {
        p.align = Paragraph::Align::Justify;
        p.hyphenate = hyphenate;
    }
    return shaper.layout(block, db);
}

}  // namespace

TEST_CASE("hyphenation breaks a long word and only adds glyphs, never overflowing the box") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;

    // A single long word that cannot fit the narrow box on one line, so it MUST wrap. Without
    // hyphenation the wrap is an emergency character break (no hyphens); with it, the break points
    // come from the dictionary and a hyphen is drawn at each.
    const std::string text = "supercalifragilisticexpialidocious";
    const ShapedBlock off = layoutParagraph(db, text, 90.0, /*hyphenate=*/false);
    const ShapedBlock on = layoutParagraph(db, text, 90.0, /*hyphenate=*/true);

    REQUIRE(off.glyphs.size() > 0);
    // Same letters either way; hyphenation only ADDS hyphen glyphs.
    CHECK(on.glyphs.size() >= off.glyphs.size());
    // The auto-hyphen stays inside the frame (its advance is counted in the line width).
    CHECK(on.bounds.right() <= 90.0 + 2.0);

    // Where the dictionary is installed, hyphenation must actually fire: strictly more glyphs.
    Hyphenator probe;
    if (probe.hasDictionary("en-US")) {
        CHECK(on.glyphs.size() > off.glyphs.size());
    }
}

TEST_CASE("hyphenation is inert on Point text (no wrapping)") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;

    auto layoutPoint = [&](bool hyphenate) {
        TextShaper shaper;
        shaper.setDefaultLanguage("en-US");
        TextBlock block =
            makeBlock("hyphenation", styleOf(24.0f, db.defaultFamily()), TextFrame::Point);
        block.paragraphs.front().hyphenate = hyphenate;
        return shaper.layout(block, db);
    };
    const ShapedBlock off = layoutPoint(false);
    const ShapedBlock on = layoutPoint(true);
    // Point never wraps, so hyphenation is a no-op: one line, identical glyphs either way.
    REQUIRE(on.lines.size() == 1);
    CHECK(on.glyphs.size() == off.glyphs.size());
}

TEST_CASE("the synthetic hyphen does not corrupt caret / hit-testing byte mapping") {
    mosaic::platform::FontDB db;
    if (!fontsAvailable(db)) return;
    Hyphenator probe;
    if (!probe.hasDictionary("en-US")) return;  // needs real breaks to exercise the hyphen glyph

    TextShaper shaper;
    shaper.setDefaultLanguage("en-US");
    TextBlock block =
        makeBlock("supercalifragilisticexpialidocious", styleOf(24.0f, db.defaultFamily()),
                  TextFrame::Area);
    block.areaSize = {90.0, 400.0};
    block.paragraphs.front().hyphenate = true;
    const ShapedBlock sb = shaper.layout(block, db);
    const std::size_t n = block.utf8.size();
    REQUIRE(sb.lines.size() > 1);  // it wrapped, so at least one hyphen glyph is present

    // Hit-testing at every glyph pen returns an in-range byte offset (the hyphen never yields a
    // cluster past end-of-text or otherwise out of range).
    for (const ShapedGlyph& g : sb.glyphs) {
        const std::size_t b = hitTest(sb, block, g.pen);
        CHECK(b <= n);
    }
    // Selecting the whole block gives one rect per visual line, all inside the frame width.
    const auto rects = selectionRects(sb, block, 0, n);
    CHECK(rects.size() == sb.lines.size());
    for (const mosaic::common::Rect& r : rects) CHECK(r.right() <= 90.0 + 2.0);
}
