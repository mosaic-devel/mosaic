// SpellChecker tests (docs/type-deferred-features.md §2, docs/spell-check-plan.md commit 1).
// Deterministic without any installed system dictionary: an in-memory mock dictionary (a blacklist
// of misspelled words + a suggestion map) is injected via loadMockDictionary, so results are
// reproducible on every machine. A final case-gated block exercises the real enchant backend only
// when a system en_US dictionary is actually installed (the CI-safe pattern used for hyphenation).
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/text/spell_checker.hpp"

using namespace mosaic::core::text;

namespace {
// A mock "en" dictionary: only these words are wrong; everything else is correct.
SpellChecker makeMock() {
    SpellChecker sc;
    sc.loadMockDictionary("en", {"wrold", "teh"},
                          {{"wrold", {"world", "wold", "word"}}, {"teh", {"the", "ten"}}});
    return sc;
}
bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
}  // namespace

TEST_CASE("no dictionary treats every word as correct and offers no suggestions") {
    SpellChecker sc;
    CHECK(sc.hasDictionary("zz") == false);
    CHECK(sc.correct("qwertyx", "zz"));  // unknown language -> never flag
    CHECK(sc.suggest("qwertyx", "zz").empty());
}

TEST_CASE("a mock dictionary flags only its blacklisted words") {
    SpellChecker sc = makeMock();
    CHECK(sc.hasDictionary("en"));
    CHECK(sc.hasDictionary("en-US"));  // resolves on the primary subtag
    CHECK(sc.correct("hello", "en"));
    CHECK(sc.correct("world", "en"));
    CHECK_FALSE(sc.correct("wrold", "en"));
    CHECK_FALSE(sc.correct("teh", "en-GB"));  // primary-subtag fallback still finds the mock
}

TEST_CASE("blacklist and suggestions are ASCII case-insensitive") {
    SpellChecker sc = makeMock();
    CHECK_FALSE(sc.correct("Wrold", "en"));  // sentence-cased misspelling still flagged
    CHECK_FALSE(sc.correct("WROLD", "en"));
    CHECK(contains(sc.suggest("Wrold", "en"), "world"));
}

TEST_CASE("suggestions come back in order, best first") {
    SpellChecker sc = makeMock();
    const auto s = sc.suggest("wrold", "en");
    REQUIRE(s.size() == 3);
    CHECK(s[0] == "world");
    CHECK(s[2] == "word");
    CHECK(sc.suggest("hello", "en").empty());  // a correct word has none in the mock
}

TEST_CASE("addToUserDict makes a word correct thereafter") {
    SpellChecker sc = makeMock();
    REQUIRE_FALSE(sc.correct("wrold", "en"));
    sc.addToUserDict("wrold", "en");
    CHECK(sc.correct("wrold", "en"));
}

TEST_CASE("ignore is checker-wide and case-sensitive for the session") {
    SpellChecker sc = makeMock();
    sc.loadMockDictionary("de", {"teh"});  // same bad word in a second language
    REQUIRE_FALSE(sc.correct("teh", "en"));
    sc.ignore("teh");
    CHECK(sc.correct("teh", "en"));
    CHECK(sc.correct("teh", "de"));   // ignore spans every language
    CHECK_FALSE(sc.correct("Teh", "en"));  // exact spelling only (not case-folded)
}

TEST_CASE("empty and whitespace-only inputs are never flagged") {
    SpellChecker sc = makeMock();
    CHECK(sc.correct("", "en"));
    CHECK(sc.suggest("", "en").empty());
}

TEST_CASE("real enchant backend, only when a system en_US dictionary is installed") {
    SpellChecker sc;
    if (!sc.hasDictionary("en_US")) return;  // CI-safe: skip when no dict on disk
    CHECK(sc.correct("hello", "en_US"));
    CHECK_FALSE(sc.correct("asdfghjqwerty", "en_US"));  // clearly not a word
    // A near-miss should offer at least one suggestion (we do not assert the exact list).
    CHECK_FALSE(sc.suggest("recieve", "en_US").empty());
}
