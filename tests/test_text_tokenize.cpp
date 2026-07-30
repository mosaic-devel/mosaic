// Word tokenizer tests (docs/type-deferred-features.md §0): the shared segmentation under
// hyphenation and spell-checking. Deterministic, no font/FLTK machinery -- byte ranges over UTF-8.
#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "core/text/tokenize.hpp"

using namespace mosaic::core::text;

namespace {

// The tokenized substrings (for readable expectations), and a raw-token fetch for flag checks.
std::vector<std::string> words(std::string_view s, TokenizeOptions opts = {}) {
    std::vector<std::string> out;
    for (const WordToken& t : tokenizeWords(s, opts))
        out.emplace_back(s.substr(t.begin, t.end - t.begin));
    return out;
}

}  // namespace

TEST_CASE("plain ASCII words split on spaces and punctuation") {
    CHECK(words("hello world") == std::vector<std::string>{"hello", "world"});
    CHECK(words("The quick, brown fox.") ==
          std::vector<std::string>{"The", "quick", "brown", "fox"});
    CHECK(words("") == std::vector<std::string>{});
    CHECK(words("   \t\n  ") == std::vector<std::string>{});
}

TEST_CASE("byte ranges point back into the source") {
    const std::string s = "ab cd";
    const auto toks = tokenizeWords(s);
    REQUIRE(toks.size() == 2);
    CHECK(toks[0].begin == 0);
    CHECK(toks[0].end == 2);
    CHECK(toks[1].begin == 3);
    CHECK(toks[1].end == 5);
}

TEST_CASE("interior apostrophe joins, edge apostrophe does not") {
    CHECK(words("don't can't it's") == std::vector<std::string>{"don't", "can't", "it's"});
    CHECK(words("girls' 'tis") == std::vector<std::string>{"girls", "tis"});
    // curly apostrophe (U+2019) joins the same way
    CHECK(words("don’t") == std::vector<std::string>{"don’t"});
}

TEST_CASE("hyphens break words (UAX#29 does not join across a hyphen)") {
    CHECK(words("e-mail well-known") ==
          std::vector<std::string>{"e", "mail", "well", "known"});
}

TEST_CASE("numbers are skipped by default, opt-in emits them") {
    CHECK(words("I have 3 cats and 42 dogs") ==
          std::vector<std::string>{"I", "have", "cats", "and", "dogs"});
    TokenizeOptions withNums;
    withNums.numbers = true;
    CHECK(words("box 42", withNums) == std::vector<std::string>{"box", "42"});
    // alphanumeric stays one word regardless (it has a letter)
    CHECK(words("iPhone7 3rd") == std::vector<std::string>{"iPhone7", "3rd"});
}

TEST_CASE("URLs and e-mails are skipped wholesale") {
    CHECK(words("see https://example.com/path now") ==
          std::vector<std::string>{"see", "now"});
    CHECK(words("mail me at bob@example.com please") ==
          std::vector<std::string>{"mail", "me", "at", "please"});
    CHECK(words("visit www.mosaic.io today") == std::vector<std::string>{"visit", "today"});
    // With detection off, the chunk tokenizes into its inner words.
    TokenizeOptions noSkip;
    noSkip.skipUrlsAndEmails = false;
    const auto w = words("bob@example.com", noSkip);
    CHECK(w == std::vector<std::string>{"bob", "example", "com"});
}

TEST_CASE("all-caps flag marks acronyms; mixed case does not") {
    auto flag = [](std::string_view s, std::size_t idx) {
        return tokenizeWords(s).at(idx).allCaps;
    };
    CHECK(flag("NASA", 0) == true);
    CHECK(flag("Nasa", 0) == false);
    CHECK(flag("nasa", 0) == false);
    // a digit-only alphanumeric with no cased letter is never allCaps
    CHECK(tokenizeWords("HTTP2").at(0).allCaps == true);   // cased letters all upper
    CHECK(tokenizeWords("hello").at(0).allCaps == false);
}

TEST_CASE("hasLetter / hasDigit flags") {
    const auto t = tokenizeWords("abc123");
    REQUIRE(t.size() == 1);
    CHECK(t[0].hasLetter == true);
    CHECK(t[0].hasDigit == true);
}

TEST_CASE("accented Latin stays a single word (dictionary scripts)") {
    // French, German, Spanish, Italian sample words must not fracture on their accents.
    CHECK(words("café naïve") == std::vector<std::string>{"café", "naïve"});
    CHECK(words("Straße Grüße") ==
          std::vector<std::string>{"Straße", "Grüße"});
    CHECK(words("niño año") == std::vector<std::string>{"niño", "año"});
    // combining-mark form (e + U+0301) is absorbed into the word too
    CHECK(words("café") == std::vector<std::string>{"café"});
}

TEST_CASE("Greek and Cyrillic are letters") {
    CHECK(words("καλά μέρα")  // "καλά μέρα"
          == std::vector<std::string>{"καλά", "μέρα"});
    CHECK(words("Привет")  // "Привет"
          == std::vector<std::string>{"Привет"});
}

TEST_CASE("CJK is skipped by default (no spell/hyphen), opt-in emits per-character") {
    // "日本語" -- three ideographs, no spaces.
    CHECK(words("日本語") == std::vector<std::string>{});
    TokenizeOptions withCjk;
    withCjk.cjk = true;
    const auto t = tokenizeWords("日本語", withCjk);
    REQUIRE(t.size() == 3);
    CHECK(t[0].cjk == true);
    CHECK(t[0].hasLetter == false);
    // Latin words next to CJK still tokenize normally.
    CHECK(words("hi 日 bye") == std::vector<std::string>{"hi", "bye"});
}

TEST_CASE("emoji and symbols are boundaries, never words") {
    CHECK(words("hi \U0001F600 there") == std::vector<std::string>{"hi", "there"});
    CHECK(words("a+b=c") == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("tokens never overlap and stay ordered") {
    const std::string s = "one, two-three; four's five";
    const auto toks = tokenizeWords(s);
    std::size_t prevEnd = 0;
    for (const WordToken& t : toks) {
        CHECK(t.begin >= prevEnd);
        CHECK(t.end > t.begin);
        CHECK(t.end <= s.size());
        prevEnd = t.end;
    }
}
