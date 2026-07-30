#pragma once

#include <cstddef>
#include <limits>
#include <string_view>
#include <vector>

#include "core/text/spell_checker.hpp"
#include "core/text/text_model.hpp"

// The pure, FLTK-free heart of Type spell-checking (docs/spell-check-plan.md commit 2): given a
// TextBlock, produce the BYTE ranges of its misspelled words. This is the "range -> word" logic the
// plan keeps out of the UI so it can be unit-tested with a mock dictionary; the background worker
// (which runs this off the UI thread and marshals the result back on the frame tick -- decision D1)
// is the thin, manually-verified shell around it.
//
// It composes the pieces already shipped: tokenizeWords (core/text/tokenize.*) segments each
// paragraph into words and flags the ones to skip (pure numbers, URLs/e-mails and CJK are excluded
// by the tokenizer's defaults; this layer additionally skips words with digits and -- by default --
// all-caps acronyms, decision D4); resolveLanguage (core/text/language.*) picks each paragraph's
// dictionary from Paragraph::language with the document/app defaults behind it; and SpellChecker
// answers "is this spelled right". A paragraph whose resolved language has no dictionary is skipped
// wholesale, so unsupported languages never squiggle.
namespace mosaic::core::text {

// A misspelled word as a half-open BYTE range [begin, end) into TextBlock::utf8 -- the same
// byte-addressing runs, selection and shaped glyphs use, so the overlay (commit 3) maps a range
// straight onto screen segments. Sorted ascending, non-overlapping.
struct MisspelledRange {
    std::size_t begin = 0;
    std::size_t end = 0;
    [[nodiscard]] std::size_t length() const noexcept { return end - begin; }
    bool operator==(const MisspelledRange&) const = default;
};

struct SpellScanOptions {
    // Check all-caps words too (decision D4). Off by default: acronyms (NASA, HTTP) are usually not
    // in a dictionary and flagging them is noise; the Text-Settings "Check ALL-CAPS words" toggle
    // turns this on for the niche case of catching capitalised typos.
    bool checkAllCaps = false;
};

// Scan the paragraphs with index in [paraFirst, paraLast] (inclusive, clamped to the block) for
// misspellings, returning their byte ranges in ascending order. Defaults to the whole block; the
// worker passes the edited paragraph's index range (decision D3, edited-paragraph scope) so a
// keystroke rechecks only what changed. `documentDefault`/`appDefault` are the languages behind an
// empty Paragraph::language (see resolveLanguage). Deterministic for a given checker state.
[[nodiscard]] std::vector<MisspelledRange> scanBlockSpelling(
    const TextBlock& block, SpellChecker& checker, std::string_view documentDefault,
    std::string_view appDefault, const SpellScanOptions& opts = {}, std::size_t paraFirst = 0,
    std::size_t paraLast = std::numeric_limits<std::size_t>::max());

}  // namespace mosaic::core::text
