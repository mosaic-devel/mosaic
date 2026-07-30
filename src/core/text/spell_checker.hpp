#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Spell-checking via enchant (docs/type-deferred-features.md §2, docs/spell-check-plan.md). Given a
// word and a BCP-47 language it answers "is this spelled right" and offers replacement suggestions;
// the Type tool draws a red squiggle under the misspellings (a canvas overlay, never baked into the
// text) and offers the suggestions in the right-click menu.
//
// enchant (which unifies hunspell / nuspell / aspell and finds the system dictionaries) is kept
// PRIVATE behind a PImpl, exactly as libhyphen is private to hyphenator.cpp and FreeType/HarfBuzz are
// private to shaping.cpp -- this header pulls in nothing but the standard library, so the model and
// the tests stay dependency-free. Dictionaries load lazily per language via one enchant broker and
// are cached; the broker is created only when a real dictionary is first needed.
//
// enchant is used under its LGPL-2.1+ license (GPLv3-compatible); the backend dictionaries are the
// system hunspell/aspell ones (see docs/third-party-licenses.md). The word segmentation that feeds
// this (which spans are words, which to skip) lives in core/text/tokenize.* -- shared with
// hyphenation -- and the per-paragraph language in core/text/language.*.
//
// NOT thread-safe: one instance is single-threaded. The incremental checker (deferred §2 commit 2)
// runs on a background worker that owns its OWN SpellChecker with its own broker, so enchant is never
// touched cross-thread.
namespace mosaic::core::text {

class SpellChecker {
public:
    SpellChecker();
    ~SpellChecker();
    SpellChecker(SpellChecker&&) noexcept;
    SpellChecker& operator=(SpellChecker&&) noexcept;
    SpellChecker(const SpellChecker&) = delete;
    SpellChecker& operator=(const SpellChecker&) = delete;

    // Whether `word` is spelled correctly in `language` (after primary-subtag fallback). `word` is
    // one plain UTF-8 word with no surrounding punctuation (as tokenizeWords yields). A word with no
    // dictionary available is treated as correct -- a missing dictionary must never paint spurious
    // squiggles. Words added via addToUserDict or ignore() also count as correct.
    [[nodiscard]] bool correct(std::string_view word, std::string_view language);

    // Ordered replacement suggestions for a (presumably misspelled) `word`, best first. Empty when
    // there is no dictionary or the backend offers none. The UI shows up to a handful at the top of
    // the right-click menu.
    [[nodiscard]] std::vector<std::string> suggest(std::string_view word, std::string_view language);

    // Add `word` to the user's PERSISTENT personal dictionary for `language` (enchant's own personal
    // wordlist -- decision D2); it is correct from now on, across sessions. No-op without a dictionary.
    void addToUserDict(std::string_view word, std::string_view language);

    // Ignore `word` for the rest of THIS session only (checker-wide, every language): correct()
    // returns true for it without touching any persistent dictionary. Case-sensitive as given, which
    // matches "Ignore All" acting on the exact spelling the user saw.
    void ignore(std::string_view word);

    // Whether a dictionary is available for `language` (after primary-subtag fallback). The checker
    // uses this to decide whether to spell-check a paragraph at all; tests gate real-dict assertions
    // on it so they pass on machines without the system dictionaries installed.
    [[nodiscard]] bool hasDictionary(std::string_view language);

    // Test hook: register an in-memory MOCK dictionary for `language`, bypassing enchant, so unit
    // tests are deterministic without any installed system dictionary (the spell-check twin of
    // Hyphenator::loadDictionaryData). `misspelled` are the words the mock treats as WRONG -- every
    // other alphabetic word is correct; `suggestions` maps a word to its ordered replacement list
    // returned by suggest(). Both are matched ASCII-case-insensitively. A mock dictionary is
    // consulted before the enchant backend for its primary subtag.
    void loadMockDictionary(
        std::string_view language, std::vector<std::string> misspelled,
        std::vector<std::pair<std::string, std::vector<std::string>>> suggestions = {});

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mosaic::core::text
