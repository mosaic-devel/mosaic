// scanBlockSpelling tests (docs/spell-check-plan.md commit 2): the pure, FLTK-free "which byte
// ranges are misspelled" logic that the background worker runs off the UI thread. Deterministic via
// a mock dictionary -- no installed system dictionary, no enchant broker for the mocked languages.
#include <doctest/doctest.h>

#include <limits>
#include <string>
#include <vector>

#include "core/text/spell_checker.hpp"
#include "core/text/spell_scan.hpp"
#include "core/text/text_model.hpp"

using namespace mosaic::core::text;

namespace {
// A mock "en" checker flagging "wrold"/"teh"; extend per test.
SpellChecker enChecker() {
    SpellChecker sc;
    sc.loadMockDictionary("en", {"wrold", "teh"});
    return sc;
}
std::vector<MisspelledRange> ranges(std::initializer_list<std::pair<std::size_t, std::size_t>> l) {
    std::vector<MisspelledRange> v;
    for (auto [b, e] : l) v.push_back({b, e});
    return v;
}
}  // namespace

TEST_CASE("misspellings map to their exact byte ranges") {
    SpellChecker sc = enChecker();
    // "the wrold is teh best": wrold=[4,9), teh=[13,16).
    TextBlock block = makeBlock("the wrold is teh best");
    const auto out = scanBlockSpelling(block, sc, "", "en");
    CHECK(out == ranges({{4, 9}, {13, 16}}));
}

TEST_CASE("correct text yields no ranges") {
    SpellChecker sc = enChecker();
    TextBlock block = makeBlock("the world is the best");
    CHECK(scanBlockSpelling(block, sc, "", "en").empty());
}

TEST_CASE("each paragraph is checked in its own language") {
    SpellChecker sc = enChecker();
    sc.loadMockDictionary("fr", {});  // a real (empty) dictionary: flags nothing
    TextBlock block = makeBlock("wrold\nwrold");
    REQUIRE(block.paragraphs.size() == 2);
    block.paragraphs[0].language = "en";  // flags "wrold"
    block.paragraphs[1].language = "fr";  // does not
    const auto out = scanBlockSpelling(block, sc, "", "en");
    CHECK(out == ranges({{0, 5}}));  // only the English paragraph's "wrold"
}

TEST_CASE("a paragraph in a language with no dictionary is skipped entirely") {
    SpellChecker sc = enChecker();
    TextBlock block = makeBlock("wrold");
    block.paragraphs[0].language = "zz";  // no dictionary anywhere -> not scanned
    CHECK(scanBlockSpelling(block, sc, "", "en").empty());
}

TEST_CASE("all-caps acronyms are skipped by default, checked on request (D4)") {
    SpellChecker sc = enChecker();
    // "wrold NASA WROLD": wrold=[0,5), NASA=[6,10) (never bad), WROLD=[11,16) (all-caps misspelling).
    TextBlock block = makeBlock("wrold NASA WROLD");
    CHECK(scanBlockSpelling(block, sc, "", "en") == ranges({{0, 5}}));
    SpellScanOptions caps;
    caps.checkAllCaps = true;
    CHECK(scanBlockSpelling(block, sc, "", "en", caps) == ranges({{0, 5}, {11, 16}}));
}

TEST_CASE("alphanumeric words are not spell-checked") {
    SpellChecker sc = enChecker();
    // "wrold utf8 3rd teh": utf8=[6,10) and 3rd=[11,14) carry digits -> skipped; wrold+teh flagged.
    TextBlock block = makeBlock("wrold utf8 3rd teh");
    CHECK(scanBlockSpelling(block, sc, "", "en") == ranges({{0, 5}, {15, 18}}));
}

TEST_CASE("URLs are skipped, not word-by-word checked") {
    SpellChecker sc = enChecker();
    // The "teh" inside the URL must not flag; only the trailing standalone "teh" does.
    TextBlock block = makeBlock("see http://teh.example/teh then teh");
    const auto out = scanBlockSpelling(block, sc, "", "en");
    REQUIRE(out.size() == 1);
    CHECK(block.utf8.substr(out[0].begin, out[0].length()) == "teh");
    CHECK(out[0].begin == block.utf8.size() - 3);  // the last word
}

TEST_CASE("a paragraph range scopes the scan (D3 edited-paragraph)") {
    SpellChecker sc = enChecker();
    // three paragraphs, each one misspelled word: wrold=[0,5), teh=[6,9), wrold=[10,15).
    TextBlock block = makeBlock("wrold\nteh\nwrold");
    REQUIRE(block.paragraphs.size() == 3);
    CHECK(scanBlockSpelling(block, sc, "", "en") == ranges({{0, 5}, {6, 9}, {10, 15}}));
    // Only the middle paragraph:
    CHECK(scanBlockSpelling(block, sc, "", "en", {}, /*paraFirst=*/1, /*paraLast=*/1) ==
          ranges({{6, 9}}));
    // Paragraphs 1..2:
    CHECK(scanBlockSpelling(block, sc, "", "en", {}, 1, 2) == ranges({{6, 9}, {10, 15}}));
}

TEST_CASE("an empty block scans clean") {
    SpellChecker sc = enChecker();
    CHECK(scanBlockSpelling(makeBlock(""), sc, "", "en").empty());
}
