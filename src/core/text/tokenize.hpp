#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

// Shared word tokenizer for the Type tool's language features (docs/type-deferred-features.md §0).
// Both hyphenation and spell-checking need to segment a paragraph's UTF-8 into WORDS: hyphenation
// asks "where can this word break", spell-check asks "is this word spelled right". They must agree
// on what a word is, so the segmentation lives here once -- FLTK-free, FreeType-free, deterministic,
// and unit-tested -- rather than being re-derived in each feature.
//
// The rules are a pragmatic subset of Unicode UAX #29 (word boundaries): a word is a maximal run of
// LETTERS and DIGITS, with a single interior apostrophe joining two letters (so "don't", "it's" and
// "l'ami" are one token each). Hyphens, slashes, and other punctuation break words (UAX #29 does not
// join across a hyphen either). CJK/ideographic text has a boundary between every character (it is
// not spell-checked or hyphenated), so those are handled separately. URLs and e-mails are detected
// per whitespace-delimited chunk and excluded wholesale (a URL is never spell-checked word-by-word).
//
// Coverage is by codepoint range for the common scripts (Latin incl. the accented ranges the
// bundled en/de/fr/es/it dictionaries need, Greek, Cyrillic, Hebrew, Arabic, Devanagari, CJK); it is
// deliberately not the full Unicode database (no ICU dependency). Extending a script is adding a
// range to the classifier tables in tokenize.cpp -- callers do not change.
namespace mosaic::core::text {

// One word token: a half-open BYTE range [begin, end) into the source UTF-8, plus classification
// flags. Byte ranges (not codepoint indices) so a caller maps a token straight back onto style runs
// and shaped glyphs, which are all byte-addressed in this codebase.
struct WordToken {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool hasLetter = false;  // contains >= 1 alphabetic codepoint (vs. a pure number)
    bool hasDigit = false;   // contains >= 1 decimal digit (e.g. "iPhone7", "3rd")
    bool allCaps = false;    // every CASED letter is uppercase and there is >=1 cased letter (an
                             // acronym candidate spell-check may choose to skip). Scripts without
                             // case (CJK, Arabic, Hebrew, Devanagari) never set this.
    bool cjk = false;        // an ideographic/CJK token (no hyphenation, no spell-check applies)

    [[nodiscard]] std::size_t length() const noexcept { return end - begin; }
    bool operator==(const WordToken&) const = default;
};

struct TokenizeOptions {
    // Emit pure-number tokens (all digits, no letters). Off by default: neither spell-check nor
    // hyphenation wants them, and it keeps "3.14" / "2026" out of the stream.
    bool numbers = false;
    // Emit CJK/ideographic tokens (one per character). Off by default: they are neither hyphenated
    // (they wrap anywhere) nor spell-checked. A future CJK feature can turn them on.
    bool cjk = false;
    // Detect URLs (contain "://", start with "www.") and e-mails (contain "@") in a whitespace
    // chunk and skip the whole chunk, so "see http://x.io/p" never yields "http"/"x"/"io"/"p".
    bool skipUrlsAndEmails = true;
};

// Segment `utf8` into word tokens per the rules above. The returned tokens are in text order, do not
// overlap, and each spans a valid UTF-8 boundary range. Deterministic and allocation-cheap; safe to
// call per edited paragraph on a keystroke (spell-check) or per Area line (hyphenation).
[[nodiscard]] std::vector<WordToken> tokenizeWords(std::string_view utf8,
                                                   const TokenizeOptions& opts = {});

// True if codepoint `cp` is set UPRIGHT in a vertical writing mode: CJK ideographs / kana / hangul,
// CJK punctuation and fullwidth forms, and emoji / pictographs. False means it is a "sideways" glyph
// (Latin, digits, most punctuation) that the vertical shaper rotates 90 degrees clockwise when the
// block's text-orientation is `mixed` (docs/type-vertical-writing-mode.md B3). This is the run-
// segmentation policy for the vertical shaper -- a pragmatic subset of the Unicode Vertical_Orientation
// property (no ICU), consistent with the tokenizer's own CJK classification.
[[nodiscard]] bool isVerticalUpright(char32_t cp);

}  // namespace mosaic::core::text
