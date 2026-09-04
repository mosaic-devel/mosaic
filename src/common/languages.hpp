#pragma once

#include <span>
#include <string_view>

// The UI languages Mosaic ships catalogs for, with the names the Settings language picker shows.
// The table itself is GENERATED from tools/i18n/languages.py (the single source of truth that also
// produces po/LINGUAS) by tools/i18n/gen_language_table.py -- see docs/i18n.md.
//
// This is the *shipped set*, not the *installed set*: a build or an install may carry fewer
// catalogs than the table lists (or none at all, running uninstalled). Ask
// common::i18n::installedLanguages() for what is actually present and intersect the two -- offering
// a language whose .mo is absent would silently do nothing.
namespace mosaic::common {

struct LanguageInfo {
    const char* code;    // gettext catalog selector: "de", "pt_BR", "ca@valencia"
    const char* english; // English name ("German") -- for a picker read in any language
    const char* endonym; // the language's own name ("Deutsch")
    bool rtl;            // written right-to-left
};

// Every shipped language, sorted by `code`.
[[nodiscard]] std::span<const LanguageInfo> knownLanguages();

// The row for `code`, or nullptr when it names no shipped language.
[[nodiscard]] const LanguageInfo* findLanguage(std::string_view code);

} // namespace mosaic::common
