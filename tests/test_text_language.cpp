// Language-attribute plumbing tests (docs/type-deferred-features.md §0): BCP-47 normalization,
// primary-subtag reduction, the inheritance resolver, and locale detection. Pure/deterministic
// except detectSystemLanguage, which we pin via LC_ALL (highest POSIX precedence).
#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

#include "core/text/language.hpp"

using namespace mosaic::core::text;

TEST_CASE("normalizeBcp47 canonicalizes raw locale strings") {
    CHECK(normalizeBcp47("en_US.UTF-8") == "en-US");
    CHECK(normalizeBcp47("de_DE@euro") == "de-DE");
    CHECK(normalizeBcp47("fr_FR") == "fr-FR");
    CHECK(normalizeBcp47("ja") == "ja");
    CHECK(normalizeBcp47("EN-us") == "en-US");   // case-folded per subtag
    CHECK(normalizeBcp47("pt_br.iso88591") == "pt-BR");
}

TEST_CASE("normalizeBcp47 rejects the neutral locales") {
    CHECK(normalizeBcp47("") == "");
    CHECK(normalizeBcp47("C") == "");
    CHECK(normalizeBcp47("POSIX") == "");
    CHECK(normalizeBcp47(".UTF-8") == "");
}

TEST_CASE("primaryLanguageSubtag strips the region") {
    CHECK(primaryLanguageSubtag("en-US") == "en");
    CHECK(primaryLanguageSubtag("de") == "de");
    CHECK(primaryLanguageSubtag("pt_BR") == "pt");
    CHECK(primaryLanguageSubtag("") == "");
}

TEST_CASE("resolveLanguage walks the inheritance chain") {
    CHECK(resolveLanguage("fr", "en-US", "en") == "fr");     // paragraph wins
    CHECK(resolveLanguage("", "en-US", "en") == "en-US");    // then document default
    CHECK(resolveLanguage("", "", "en") == "en");            // then app default
    CHECK(resolveLanguage("", "", "") == "");                // all empty -> empty (caller falls back)
}

TEST_CASE("detectSystemLanguage reads the environment locale") {
    // Save and pin LC_ALL (it overrides LC_MESSAGES/LANG), then restore.
    const char* saved = std::getenv("LC_ALL");
    const std::string savedStr = saved ? saved : "";

    setenv("LC_ALL", "de_DE.UTF-8", 1);
    CHECK(detectSystemLanguage() == "de-DE");

    setenv("LC_ALL", "C", 1);
    CHECK(detectSystemLanguage() == "");  // C/POSIX -> caller falls back

    if (saved) setenv("LC_ALL", savedStr.c_str(), 1);
    else unsetenv("LC_ALL");
}
