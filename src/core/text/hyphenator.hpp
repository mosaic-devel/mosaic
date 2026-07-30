#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Automatic hyphenation via libhyphen (Liang's 1983 TeX algorithm + per-language pattern
// dictionaries; docs/type-deferred-features.md §1). Given a word and a BCP-47 language, it answers
// "where may this word break" -- the layout uses that to break a long word across an Area line and
// draw a trailing hyphen, so justified text fills lines evenly instead of opening rivers.
//
// libhyphen (and the FILE/dict machinery) is kept PRIVATE behind a PImpl, exactly as FreeType /
// HarfBuzz are private to shaping.cpp -- this header pulls in nothing but the standard library, so
// the model and the tests stay dependency-free. Dictionaries load lazily per language from the
// system hyphenation dir (MOSAIC_HYPHEN_DIR, default /usr/share/hyphen) and are cached.
//
// libhyphen is used under its LGPL-2.1 / MPL arms for GPLv3 compatibility (see
// docs/third-party-licenses.md).
namespace mosaic::core::text {

class Hyphenator {
public:
    Hyphenator();
    ~Hyphenator();
    Hyphenator(Hyphenator&&) noexcept;
    Hyphenator& operator=(Hyphenator&&) noexcept;
    Hyphenator(const Hyphenator&) = delete;
    Hyphenator& operator=(const Hyphenator&) = delete;

    // Byte offsets into `word` at which a break may be inserted -- each offset is where the TAIL
    // (the part pushed to the next line) begins, so a hyphen is drawn just before it. Empty when the
    // language has no dictionary, the word is too short, or it holds no break opportunity. `word` is
    // one plain UTF-8 word with no surrounding punctuation. lhmin/rhmin (min chars kept before/after
    // a break) override the dictionary's own values when > 0; <= 0 uses the dictionary default (or a
    // 2/3 fallback). Deterministic for a given dictionary; the dictionary is cached across calls.
    [[nodiscard]] std::vector<std::size_t> hyphenationPoints(std::string_view word,
                                                             std::string_view language,
                                                             int lhmin = 0, int rhmin = 0);

    // Whether a dictionary is available for `language` (after primary-subtag fallback). The Type
    // panel uses this to grey the Hyphenate toggle when the paragraph's language has no patterns.
    [[nodiscard]] bool hasDictionary(std::string_view language);

    // Test hook: register an in-memory libhyphen .dic for `language`, bypassing the filesystem, so
    // unit tests assert break points deterministically without relying on installed system dicts.
    // Returns false if the data does not parse as a dictionary.
    bool loadDictionaryData(std::string_view language, std::string_view dicData);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mosaic::core::text
