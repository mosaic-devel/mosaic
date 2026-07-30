#pragma once

#include <string>
#include <string_view>

// Per-paragraph language, the shared foundation under hyphenation and spell-checking
// (docs/type-deferred-features.md §0). Both features are language-specific -- hyphenation patterns
// and spell dictionaries are chosen per BCP-47 tag -- so the model carries a language per paragraph
// (Paragraph::language), empty meaning "inherit the document/app default", and that default is
// seeded from the OS locale. These free functions are the FLTK-free plumbing: detect the locale,
// normalize a raw locale string to a BCP-47 tag, resolve the inheritance chain, and reduce a tag to
// its primary subtag (dictionaries are usually keyed by language, sometimes by language+region).
namespace mosaic::core::text {

// Normalize a raw locale / language string to a BCP-47-ish tag: "en_US.UTF-8" -> "en-US",
// "de_DE@euro" -> "de-DE", "C"/"POSIX"/"" -> "". The language subtag is lowercased, the region
// subtag uppercased, the ".encoding" and "@modifier" suffixes dropped, and '_' becomes '-'.
// Deterministic and locale-independent (no setlocale side effects).
[[nodiscard]] std::string normalizeBcp47(std::string_view locale);

// The primary language subtag of a BCP-47 tag: "en-US" -> "en", "de" -> "de", "" -> "".
// (Lowercased.) What a language-keyed dictionary lookup falls back to when there is no
// region-specific dictionary.
[[nodiscard]] std::string primaryLanguageSubtag(std::string_view bcp47);

// The OS's preferred UI/text language as a BCP-47 tag ("en-US", "de", ...), read from the
// environment locale (LC_ALL / LC_MESSAGES / LANG on POSIX). Returns "" when it cannot be
// determined or resolves to the C/POSIX locale -- callers then fall back to their own default.
[[nodiscard]] std::string detectSystemLanguage();

// Resolve the language actually in force for a paragraph: the first non-empty of
// (paragraph tag, document default, app default). All three may be empty, in which case the result
// is empty and the caller applies its ultimate fallback (typically detectSystemLanguage() or "en").
[[nodiscard]] std::string resolveLanguage(std::string_view paragraph, std::string_view documentDefault,
                                          std::string_view appDefault);

}  // namespace mosaic::core::text
