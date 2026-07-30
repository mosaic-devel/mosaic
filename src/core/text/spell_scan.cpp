#include "core/text/spell_scan.hpp"

#include <algorithm>
#include <string>

#include "core/text/language.hpp"
#include "core/text/tokenize.hpp"

namespace mosaic::core::text {
namespace {

// Scan one paragraph's text (relative offsets) in `language`, appending misspelled ranges with
// `textBegin` added so the offsets are absolute into the block. The tokenizer already drops pure
// numbers, URLs/e-mails and CJK (its defaults); here we additionally skip alphanumeric tokens and
// (unless opts.checkAllCaps) all-caps acronyms, then ask the checker about the rest.
void scanParagraph(std::string_view text, std::size_t textBegin, std::string_view language,
                   SpellChecker& checker, const SpellScanOptions& opts,
                   std::vector<MisspelledRange>& out) {
    for (const WordToken& tok : tokenizeWords(text)) {
        if (!tok.hasLetter || tok.cjk || tok.hasDigit) continue;  // not a dictionary word
        if (tok.allCaps && !opts.checkAllCaps) continue;          // acronym skip (D4)
        const std::string_view word = text.substr(tok.begin, tok.length());
        if (!checker.correct(word, language))
            out.push_back({textBegin + tok.begin, textBegin + tok.end});
    }
}

}  // namespace

std::vector<MisspelledRange> scanBlockSpelling(const TextBlock& block, SpellChecker& checker,
                                               std::string_view documentDefault,
                                               std::string_view appDefault,
                                               const SpellScanOptions& opts, std::size_t paraFirst,
                                               std::size_t paraLast) {
    std::vector<MisspelledRange> out;
    const std::string& s = block.utf8;

    // Walk paragraphs ('\n'-delimited), scanning those whose index falls in [paraFirst, paraLast].
    std::size_t paraIdx = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i != s.size() && s[i] != '\n') continue;
        if (paraIdx >= paraFirst && paraIdx <= paraLast) {
            const std::string_view paraLangAttr =
                paraIdx < block.paragraphs.size() ? std::string_view{block.paragraphs[paraIdx].language}
                                                  : std::string_view{};
            const std::string lang = resolveLanguage(paraLangAttr, documentDefault, appDefault);
            // Skip the whole paragraph if there is no dictionary for its language (no point
            // tokenizing text we cannot check -- and it keeps unsupported languages un-squiggled).
            if (!lang.empty() && checker.hasDictionary(lang))
                scanParagraph(std::string_view{s}.substr(start, i - start), start, lang, checker,
                              opts, out);
        }
        ++paraIdx;
        start = i + 1;
        if (paraIdx > paraLast) break;  // done: later paragraphs are out of range
    }
    return out;
}

}  // namespace mosaic::core::text
