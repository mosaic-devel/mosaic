// FontDB (fontconfig backend) smoke tests (docs/type-tool.md §4.2). Font sets vary by machine, so
// these assert STRUCTURE, not specific families: the default resolves to a real file, a face has a
// readable path, fallback finds coverage for a common codepoint. Everything degrades to a pass on
// a machine with no fonts installed (CI sandboxes), so the suite stays green there.
#include <doctest/doctest.h>

#include <filesystem>

#include "core/text/shaping.hpp"
#include "core/text/text_model.hpp"
#include "platform/font_db.hpp"

using mosaic::platform::FontDB;
using mosaic::core::text::FontRef;

TEST_CASE("FontDB resolves the OS default family to a real font file") {
    FontDB db;
    const auto fams = db.families();
    if (fams.empty()) {
        WARN_MESSAGE(false, "no fonts installed -- skipping FontDB structural checks");
        return;
    }

    const std::string def = db.defaultFamily();
    CHECK_FALSE(def.empty());  // sans-serif must resolve to *something* named

    FontRef ref;
    ref.family = def;
    const auto face = db.resolve(ref);
    REQUIRE(face.has_value());
    CHECK_FALSE(face->path.empty());
    CHECK(std::filesystem::exists(face->path));
    CHECK(face->index >= 0);
}

TEST_CASE("FontDB resolve honours weight/italic and always yields a face") {
    FontDB db;
    if (db.families().empty()) return;

    FontRef bold;
    bold.family = db.defaultFamily();
    bold.weight = 700;
    bold.italic = true;
    const auto face = db.resolve(bold);  // may map to a synthesised style, but never nullopt here
    REQUIRE(face.has_value());
    CHECK(std::filesystem::exists(face->path));
}

TEST_CASE("FontDB fallbackFor finds coverage for a basic codepoint") {
    FontDB db;
    if (db.families().empty()) return;

    FontRef base;
    base.family = db.defaultFamily();
    const auto face = db.fallbackFor(U'A', base);  // ASCII 'A' is covered by essentially any font
    REQUIRE(face.has_value());
    CHECK(std::filesystem::exists(face->path));
}

TEST_CASE("FontDB enumerates families (sorted, unique) and emoji faces") {
    FontDB db;
    const auto fams = db.families();
    if (fams.empty()) return;

    CHECK(std::is_sorted(fams.begin(), fams.end()));
    CHECK(std::adjacent_find(fams.begin(), fams.end()) == fams.end());  // de-duplicated

    // emojiFamilies() is a subset of the colour-capable fonts; may legitimately be empty.
    const auto emoji = db.emojiFamilies();
    for (const auto& e : emoji) {
        CHECK(std::find(fams.begin(), fams.end(), e) != fams.end());
    }
}

TEST_CASE("FontDB sampleTextFor returns a coverage-appropriate, non-empty preview") {
    FontDB db;
    const auto fams = db.families();
    if (fams.empty()) return;

    // The OS sans default covers Latin, so it gets the canonical Latin sample.
    CHECK(db.sampleTextFor(db.defaultFamily()) == "Abg");
    // Never empty for any installed family (the closest match always resolves to something).
    for (std::size_t i = 0; i < fams.size() && i < 50; ++i)
        CHECK_FALSE(db.sampleTextFor(fams[i]).empty());
}

TEST_CASE("preferred emoji family steers emoji fallback and never breaks coverage (R5)") {
    FontDB db;
    if (db.families().empty()) return;
    const auto emoji = db.emojiFamilies();
    if (emoji.empty()) return;  // no colour-emoji font installed: nothing to prefer

    FontRef base;
    base.family = db.defaultFamily();
    constexpr char32_t kGrin = U'\U0001F600';

    // Find an emoji family that actually covers the probe (colour families can be sparse).
    for (const std::string& fam : emoji) {
        db.setPreferredEmojiFamily(fam);
        const auto face = db.fallbackFor(kGrin, base);
        db.setPreferredEmojiFamily("");
        if (!face) continue;
        // The preferred family's own resolve must map to the same file when it covers the glyph.
        FontRef pref;
        pref.family = fam;
        const auto direct = db.resolve(pref);
        if (direct && direct->path == face->path) {
            CHECK(true);  // preference honoured
            // And a NON-emoji codepoint ignores the preference entirely.
            db.setPreferredEmojiFamily(fam);
            const auto latin = db.fallbackFor(U'A', base);
            db.setPreferredEmojiFamily("");
            REQUIRE(latin.has_value());
            CHECK(latin->path != direct->path);
            return;
        }
    }
    // No installed emoji family both covers the probe and resolves to itself -- nothing to assert.
}

TEST_CASE("FontDB memoizes: the SECOND identical resolve runs no fontconfig match") {
    // ⚠ The perf pin behind "bending text is extremely laggy" (user 2026-07-14): shaping consults
    // resolve() PER RUN PER LAYOUT, and an uncached FcFontMatch costs milliseconds -- so every
    // keystroke, bend tick and font-hover re-paid fontconfig. fcMatches() counts EVENTS (a cache
    // SIZE refills after a wrongly-dropped cache and a mutant sails through it).
    FontDB db;
    if (db.families().empty()) return;

    FontRef ref;
    ref.family = db.defaultFamily();
    const auto n0 = db.fcMatches();
    const auto a = db.resolve(ref);
    REQUIRE(a.has_value());
    const auto n1 = db.fcMatches();
    CHECK(n1 == n0 + 1); // the first ask pays one match
    const auto b = db.resolve(ref);
    REQUIRE(b.has_value());
    CHECK(db.fcMatches() == n1); // the second is FREE -- and identical
    CHECK(b->path == a->path);
    CHECK(b->index == a->index);

    // A different style is a different question: it pays its own match, once.
    FontRef bold = ref;
    bold.weight = 700;
    (void)db.resolve(bold);
    const auto n2 = db.fcMatches();
    CHECK(n2 == n1 + 1);
    (void)db.resolve(bold);
    CHECK(db.fcMatches() == n2);

    // fallbackFor memoizes the same way, per (codepoint, base).
    const auto f0 = db.fallbackFor(U'A', ref);
    const auto n3 = db.fcMatches();
    const auto f1 = db.fallbackFor(U'A', ref);
    CHECK(db.fcMatches() == n3); // free
    if (f0 && f1)
        CHECK(f0->path == f1->path);

    // A MISS is remembered too: an unresolvable family must not re-run the match per layout.
    // (fontconfig substitutes aggressively, so a garbage family usually still yields a face --
    // either way, the second ask must be free.)
    FontRef bogus;
    bogus.family = "no-such-family-anywhere-mosaic-test";
    (void)db.resolve(bogus);
    const auto n4 = db.fcMatches();
    (void)db.resolve(bogus);
    CHECK(db.fcMatches() == n4);

    // Changing the preferred emoji family DROPS the fallback cache (it changes answers) and
    // leaves the resolve cache alone.
    db.setPreferredEmojiFamily("Noto Color Emoji");
    (void)db.resolve(ref); // still free: the resolve cache survived
    CHECK(db.fcMatches() == n4);
    (void)db.fallbackFor(U'A', ref); // re-answers (one match): the fallback cache was dropped
    CHECK(db.fcMatches() == n4 + 1);
    db.setPreferredEmojiFamily("");
}

TEST_CASE("a whole re-layout after the first runs ZERO fontconfig matches") {
    // The end-to-end shape of the fix: the FIRST layout of a block warms the caches; every
    // re-layout after it (a keystroke, a bend tick, a caret move) is fontconfig-free.
    FontDB db;
    if (db.families().empty()) return;
    mosaic::core::text::TextShaper shaper;
    mosaic::core::text::TextBlock block = mosaic::core::text::makeBlock(
        "The quick brown fox", [&] {
            mosaic::core::text::CharStyle s;
            s.setSolidFill({0, 0, 0, 1});
            s.sizePx = 24.0f;
            s.font.family = db.defaultFamily();
            return s;
        }());
    (void)shaper.layout(block, db); // warm
    const auto warmed = db.fcMatches();
    block.bend = 0.5f; // a bend tick's re-layout
    (void)shaper.layout(block, db);
    block.bend = 0.6f;
    (void)shaper.layout(block, db);
    CHECK(db.fcMatches() == warmed);
}
