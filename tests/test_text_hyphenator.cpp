// Hyphenator tests (docs/type-deferred-features.md §1). Deterministic without any installed system
// dictionary: a tiny synthetic libhyphen .dic is injected via loadDictionaryData, so break points
// are reproducible on every machine. (The real /usr/share/hyphen dicts are exercised visually.)
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <vector>

#include "core/text/hyphenator.hpp"

using namespace mosaic::core::text;

namespace {
// A minimal libhyphen dictionary: first line is the charset, then one Liang pattern allowing a
// break between any two 'o's (odd value => break opportunity).
constexpr const char* kOoDict = "UTF-8\no1o\n";
}  // namespace

TEST_CASE("no dictionary yields no points and hasDictionary is false") {
    Hyphenator h;
    CHECK(h.hasDictionary("zz") == false);
    CHECK(h.hyphenationPoints("hello", "zz").empty());
}

TEST_CASE("an injected dictionary hyphenates deterministically") {
    Hyphenator h;
    REQUIRE(h.loadDictionaryData("xx", kOoDict));
    CHECK(h.hasDictionary("xx"));
    CHECK(h.hasDictionary("xx-YY"));  // matches on the primary subtag

    const auto pts = h.hyphenationPoints("oooooo", "xx", /*lhmin=*/2, /*rhmin=*/2);  // 6 bytes
    REQUIRE(!pts.empty());
    std::size_t prev = 0;
    for (std::size_t p : pts) {
        CHECK(p >= 2);     // lhmin: >= 2 chars kept on the left
        CHECK(p <= 4);     // rhmin: n - 2 = 4, so >= 2 chars kept on the right
        CHECK(p > prev);   // sorted and unique
        prev = p;
    }
    CHECK(std::find(pts.begin(), pts.end(), std::size_t{3}) != pts.end());  // the middle always fits
}

TEST_CASE("words shorter than a breakable length never hyphenate") {
    Hyphenator h;
    REQUIRE(h.loadDictionaryData("xx", kOoDict));
    CHECK(h.hyphenationPoints("oo", "xx").empty());
    CHECK(h.hyphenationPoints("ooo", "xx").empty());
}

TEST_CASE("ASCII case is folded so offsets still align") {
    Hyphenator h;
    REQUIRE(h.loadDictionaryData("xx", kOoDict));
    const auto lower = h.hyphenationPoints("oooooo", "xx", 2, 2);
    const auto upper = h.hyphenationPoints("OOOOOO", "xx", 2, 2);
    CHECK(upper == lower);
}

TEST_CASE("reloading a language replaces its dictionary without leaking") {
    Hyphenator h;
    REQUIRE(h.loadDictionaryData("xx", kOoDict));
    REQUIRE(h.loadDictionaryData("xx", kOoDict));  // second load frees the first (asan-checked)
    CHECK(h.hasDictionary("xx"));
}
