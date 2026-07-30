#include <doctest/doctest.h>

#include <array>
#include <clocale>  // setlocale: the downgrade regression inspects the applied LC_MESSAGES
#include <cstdlib>  // setenv/unsetenv: the MOSAIC_LANG cases drive init() through the environment
#include <cstring>
#include <optional>
#include <string>

#include "common/i18n.hpp"  // defines _() and N_(); include after doctest

using namespace mosaic;

TEST_CASE("tr returns the msgid unchanged when no catalog matches") {
    // No catalog is installed in the test environment, so English text passes through.
    const char* in = "A string that has no translation catalog entry.";
    CHECK(std::strcmp(common::i18n::tr(in), in) == 0);
}

TEST_CASE("tr tolerates a null msgid") {
    CHECK(common::i18n::tr(nullptr) == nullptr);
}

TEST_CASE("the _() and N_() macros translate / mark correctly") {
    CHECK(std::strcmp(_("Open..."), "Open...") == 0);
    // N_() only marks for extraction; it must not alter the string.
    const char* marked = N_("Save As...");
    CHECK(std::strcmp(marked, "Save As...") == 0);
}

TEST_CASE("dtr falls back to the msgid for an unbound domain") {
    const char* in = "SOME MOTIVATIONAL SHOUTING";
    CHECK(std::strcmp(common::i18n::dtr("motivate", in), in) == 0);
    CHECK(common::i18n::dtr("motivate", nullptr) == nullptr);
    CHECK(std::strcmp(common::i18n::dtr(nullptr, in), in) == 0);
}

// $MOSAIC_LANG (S54). These mutate the process environment and re-run init(), so each case sets
// every variable it depends on rather than inheriting from the case before it -- doctest gives no
// ordering guarantee, and an order-dependent environment test fails only on someone else's machine.

// init() calls setlocale(LC_ALL, "") -- correct for an app adopting the user's locale, but in a
// test binary it also moves LC_NUMERIC for every OTHER test in the process. On a machine with, say,
// LC_NUMERIC=pl_PL.UTF-8 that swaps the decimal point to ',' and quietly breaks a dozen unrelated
// formatting tests. Snapshot the whole locale and the variables these cases touch, and put them
// back, so running the i18n tests cannot change the meaning of anything downstream.
namespace {
class LocaleGuard {
public:
    LocaleGuard() {
        const char* loc = std::setlocale(LC_ALL, nullptr);
        m_locale = loc != nullptr ? loc : "C";
        for (auto& [name, value] : m_env) {
            const char* v = std::getenv(name);
            value = v != nullptr ? std::optional<std::string>(v) : std::nullopt;
        }
    }
    ~LocaleGuard() {
        for (const auto& [name, value] : m_env) {
            if (value) {
                ::setenv(name, value->c_str(), 1);
            } else {
                ::unsetenv(name);
            }
        }
        std::setlocale(LC_ALL, m_locale.c_str());
    }
    LocaleGuard(const LocaleGuard&) = delete;
    LocaleGuard& operator=(const LocaleGuard&) = delete;

private:
    std::string m_locale;
    std::array<std::pair<const char*, std::optional<std::string>>, 4> m_env{
        {{"MOSAIC_LANG", {}}, {"LANGUAGE", {}}, {"LC_ALL", {}}, {"LC_MESSAGES", {}}}};
};
}  // namespace
TEST_CASE("no MOSAIC_LANG leaves the language selection to the system locale") {
    const LocaleGuard guard;
    ::unsetenv("MOSAIC_LANG");
    common::i18n::init();
    CHECK(std::strcmp(common::i18n::activeLanguageOverride(), "") == 0);
}

// MOSAIC_HAVE_GETTEXT is PRIVATE to mosaic_common, so a test cannot see it. It does not need to:
// without gettext the override is never applied, and activeLanguageOverride() stays empty. That
// is the honest observable, so gate on it rather than on a macro the test cannot read.
static bool overrideIsLive() {
    return common::i18n::activeLanguageOverride()[0] != '\0';
}

TEST_CASE("MOSAIC_LANG redirects message lookup without a generated system locale") {
    const LocaleGuard guard;
    ::setenv("MOSAIC_LANG", "de", 1);
    ::setenv("LC_ALL", "C", 1);  // the case that silently defeats a bare $LANGUAGE
    common::i18n::init();

    if (overrideIsLive()) {
        CHECK(std::strcmp(common::i18n::activeLanguageOverride(), "de") == 0);
        // $LANGUAGE carries the request...
        const char* language = std::getenv("LANGUAGE");
        REQUIRE(language != nullptr);
        CHECK(std::strcmp(language, "de") == 0);
        // ...and LC_ALL must be gone, since it outranks LC_MESSAGES in gettext's category lookup
        // and would pin the resolved locale to "C" -- where $LANGUAGE is deliberately ignored.
        CHECK(std::getenv("LC_ALL") == nullptr);
        const char* messages = std::getenv("LC_MESSAGES");
        REQUIRE(messages != nullptr);
        CHECK(std::strcmp(messages, "C") != 0);
    }
    // With no catalog installed the text is still English -- the override selects a language, it
    // does not invent one. True either way, so it is checked unconditionally.
    CHECK(std::strcmp(_("Open..."), "Open...") == 0);
}

TEST_CASE("MOSAIC_LANG accepts a colon-separated fallback list") {
    const LocaleGuard guard;
    ::setenv("MOSAIC_LANG", "ca@valencia:ca", 1);
    common::i18n::init();
    if (overrideIsLive()) {
        CHECK(std::strcmp(common::i18n::activeLanguageOverride(), "ca@valencia:ca") == 0);
        CHECK(std::strcmp(std::getenv("LANGUAGE"), "ca@valencia:ca") == 0);
        // Only the first entry names a locale; the list itself is $LANGUAGE's business.
        CHECK(std::strcmp(std::getenv("LC_MESSAGES"), "ca@valencia") == 0);
    }
}

// Regression: an early cut of the override re-applied LC_MESSAGES unconditionally, and happily
// accepted "C.UTF-8" as the fallback. That both failed to lift glibc's $LANGUAGE guard (which
// excludes C.UTF-8 exactly as it excludes "C") and DOWNGRADED users who already had a working
// locale -- turning a no-op into a regression. Setting a language must never make the message
// category worse than it was.
TEST_CASE("MOSAIC_LANG never downgrades an already-usable message locale") {
    const LocaleGuard guard;
    ::unsetenv("MOSAIC_LANG");
    ::unsetenv("LC_ALL");
    ::setenv("LC_MESSAGES", "en_US.UTF-8", 1);
    if (std::setlocale(LC_MESSAGES, "en_US.UTF-8") == nullptr) {
        return;  // this machine has no en_US.UTF-8; nothing to protect
    }

    ::setenv("MOSAIC_LANG", "zz", 1);  // deliberately not a real locale anywhere
    common::i18n::init();

    const char* applied = std::setlocale(LC_MESSAGES, nullptr);
    REQUIRE(applied != nullptr);
    CHECK(std::strcmp(applied, "C") != 0);
    CHECK(std::strcmp(applied, "C.UTF-8") != 0);
    CHECK(std::strcmp(applied, "POSIX") != 0);
}

TEST_CASE("an empty MOSAIC_LANG is treated as absent, not as a language named \"\"") {
    const LocaleGuard guard;
    ::setenv("MOSAIC_LANG", "", 1);
    common::i18n::init();
    CHECK(std::strcmp(common::i18n::activeLanguageOverride(), "") == 0);
}
