// macOS implementation of core/text/spell_checker.hpp backed by the system spellchecker
// (NSSpellChecker), used INSTEAD of spell_checker.cpp on Apple (see core/CMakeLists.txt).
//
// This avoids cross-building enchant + glib for macOS and gives the user the OS spellchecker and
// dictionaries for free -- no bundled wordlists. The public behaviour matches the enchant backend
// exactly (a missing dictionary never paints a squiggle; mock dictionaries and the session ignore
// list are honoured identically), so the Type tool and its tests see one interface.
//
// THREADING (Mac-side verification item): Mosaic's SpellCheckWorker owns its OWN SpellChecker and
// drives it from a background thread. NSSpellChecker is documented as main-thread-preferred; if a
// real Mac shows trouble here, marshal correct()/suggest() onto the main queue. Kept direct for v1.

#include "core/text/spell_checker.hpp"

#import <AppKit/AppKit.h>

#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/text/language.hpp"

namespace mosaic::core::text {
namespace {

// Lowercase a BCP-47/locale string and unify the separator ("en_US"/"EN-us" -> "en-us").
std::string normLang(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '_') out += '-';
        else out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string asciiLower(std::string_view s) {
    std::string out{s};
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// macOS/NSSpellChecker language ids look like "en", "en_GB", "de_DE": lowercase language, uppercase
// region, underscore separator. Build that from a normalized (dash-separated, lowercase) tag.
std::string macTag(const std::string& norm) {
    std::string out;
    out.reserve(norm.size());
    bool afterDash = false;
    for (char c : norm) {
        if (c == '-') {
            out += '_';
            afterDash = true;
        } else {
            out += afterDash ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        }
    }
    return out;
}

NSString* nsstr(std::string_view s) {
    return [[NSString alloc] initWithBytes:s.data() length:(NSUInteger)s.size()
                                  encoding:NSUTF8StringEncoding];
}

} // namespace

struct SpellChecker::Impl {
    struct Mock {
        std::unordered_set<std::string> misspelled;
        std::unordered_map<std::string, std::vector<std::string>> suggestions;
    };

    std::unordered_map<std::string, Mock> mockDicts;     // keyed by primary subtag
    std::unordered_set<std::string> ignored;             // session-scoped, checker-wide (Ignore All)
    std::unordered_map<std::string, bool> dictAvail;     // cache of hasDictionary() by primary subtag

    NSSpellChecker* checker() { return [NSSpellChecker sharedSpellChecker]; }

    Mock* mockFor(std::string_view language) {
        const std::string ll = primaryLanguageSubtag(normLang(language));
        if (ll.empty()) return nullptr;
        auto it = mockDicts.find(ll);
        return it == mockDicts.end() ? nullptr : &it->second;
    }

    // True if the system has any dictionary whose primary subtag matches `language` (so words in
    // that language can be checked even if only a regional variant, e.g. en_GB, is installed).
    bool available(std::string_view language) {
        const std::string ll = primaryLanguageSubtag(normLang(language));
        if (ll.empty()) return false;
        if (auto it = dictAvail.find(ll); it != dictAvail.end()) return it->second;
        bool found = false;
        for (NSString* lang in [checker() availableLanguages]) {
            std::string a = normLang([lang UTF8String]);
            if (primaryLanguageSubtag(a) == ll) { found = true; break; }
        }
        dictAvail.emplace(ll, found);
        return found;
    }

    // The best NSSpellChecker language string for `language`: the full "en_US" if the system has it,
    // else the bare primary subtag "en" (NSSpellChecker accepts either).
    std::string langArg(std::string_view language) {
        const std::string norm = normLang(language);
        const std::string full = macTag(norm);
        for (NSString* lang in [checker() availableLanguages])
            if (std::string([lang UTF8String]) == full) return full;
        return primaryLanguageSubtag(norm);
    }
};

SpellChecker::SpellChecker() : m_impl(std::make_unique<Impl>()) {}
SpellChecker::~SpellChecker() = default;
SpellChecker::SpellChecker(SpellChecker&&) noexcept = default;
SpellChecker& SpellChecker::operator=(SpellChecker&&) noexcept = default;

bool SpellChecker::correct(std::string_view word, std::string_view language) {
    if (word.empty()) return true;
    if (m_impl->ignored.count(std::string{word})) return true;

    if (Impl::Mock* mock = m_impl->mockFor(language))
        return mock->misspelled.count(asciiLower(word)) == 0;

    if (!m_impl->available(language)) return true; // no dictionary -> never flag

    @autoreleasepool {
        NSString* w = nsstr(word);
        NSString* lang = nsstr(m_impl->langArg(language));
        NSRange r = [m_impl->checker() checkSpellingOfString:w
                                                  startingAt:0
                                                    language:lang
                                                        wrap:NO
                                      inSpellDocumentWithTag:0
                                                   wordCount:NULL];
        return r.location == NSNotFound; // no misspelling found
    }
}

std::vector<std::string> SpellChecker::suggest(std::string_view word, std::string_view language) {
    std::vector<std::string> out;
    if (word.empty()) return out;

    if (Impl::Mock* mock = m_impl->mockFor(language)) {
        if (auto it = mock->suggestions.find(asciiLower(word)); it != mock->suggestions.end())
            out = it->second;
        return out;
    }

    if (!m_impl->available(language)) return out;

    @autoreleasepool {
        NSString* w = nsstr(word);
        NSString* lang = nsstr(m_impl->langArg(language));
        NSArray<NSString*>* guesses = [m_impl->checker() guessesForWordRange:NSMakeRange(0, [w length])
                                                                    inString:w
                                                                    language:lang
                                                      inSpellDocumentWithTag:0];
        out.reserve([guesses count]);
        for (NSString* g in guesses)
            out.emplace_back([g UTF8String]);
    }
    return out;
}

void SpellChecker::addToUserDict(std::string_view word, std::string_view language) {
    if (word.empty()) return;
    if (Impl::Mock* mock = m_impl->mockFor(language)) {
        mock->misspelled.erase(asciiLower(word)); // henceforth correct
        return;
    }
    @autoreleasepool {
        [m_impl->checker() learnWord:nsstr(word)]; // NSSpellChecker's learn is system-wide
    }
}

void SpellChecker::ignore(std::string_view word) {
    if (!word.empty()) m_impl->ignored.emplace(word);
}

bool SpellChecker::hasDictionary(std::string_view language) {
    if (m_impl->mockFor(language)) return true;
    return m_impl->available(language);
}

void SpellChecker::loadMockDictionary(
    std::string_view language, std::vector<std::string> misspelled,
    std::vector<std::pair<std::string, std::vector<std::string>>> suggestions) {
    const std::string ll = primaryLanguageSubtag(normLang(language));
    if (ll.empty()) return;
    Impl::Mock mock;
    for (const std::string& w : misspelled) mock.misspelled.insert(asciiLower(w));
    for (auto& [k, v] : suggestions) mock.suggestions.emplace(asciiLower(k), std::move(v));
    m_impl->mockDicts[ll] = std::move(mock);
}

} // namespace mosaic::core::text
