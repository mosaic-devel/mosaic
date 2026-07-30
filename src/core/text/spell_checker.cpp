#include "core/text/spell_checker.hpp"

#include <enchant.h>

#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/text/language.hpp"

namespace mosaic::core::text {
namespace {

// Lowercase a BCP-47/locale string and unify the separator, so "en_US" and "EN-us" key alike.
// (Same normalization the Hyphenator uses.)
std::string normLang(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '_') out += '-';
        else out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// ASCII-lowercase a word for case-insensitive mock lookups. (The real enchant backend does its own
// case handling; this only affects the test mock.)
std::string asciiLower(std::string_view s) {
    std::string out{s};
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// enchant tags use an underscore separator with a lowercase language and uppercase region
// ("en_US", "de", ...). Build that from a normalized (dash-separated, all-lowercase) tag.
std::string enchantTag(const std::string& norm) {
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

}  // namespace

struct SpellChecker::Impl {
    // A test-injected mock dictionary: a blacklist of misspelled words + a suggestion map, all keyed
    // ASCII-lowercase. Consulted (by primary subtag) before the enchant backend.
    struct Mock {
        std::unordered_set<std::string> misspelled;
        std::unordered_map<std::string, std::vector<std::string>> suggestions;
    };

    // The enchant broker is created lazily on the first real-dictionary need, so a build that only
    // ever uses the mock (the unit tests) never spins one up. Dicts are cached by resolved tag; a
    // null value means "resolved, no dictionary" so we do not re-ask the broker every keystroke.
    EnchantBroker* broker = nullptr;
    std::unordered_map<std::string, EnchantDict*> dicts;
    std::unordered_map<std::string, Mock> mockDicts;  // keyed by primary subtag
    std::unordered_set<std::string> ignored;          // session-scoped, checker-wide (Ignore All)

    ~Impl() {
        if (broker) {
            for (auto& [tag, d] : dicts)
                if (d) enchant_broker_free_dict(broker, d);
            enchant_broker_free(broker);
        }
    }

    // The enchant dict for `language` (real backend only; mocks are handled by the callers), or null
    // if none is available. Tries the full tag, then the primary subtag ("en_US" -> "en"). Cached.
    EnchantDict* dictFor(std::string_view language) {
        const std::string norm = normLang(language);
        const std::string ll = primaryLanguageSubtag(norm);
        if (ll.empty()) return nullptr;

        if (auto it = dicts.find(norm); it != dicts.end()) return it->second;

        if (!broker) broker = enchant_broker_init();
        EnchantDict* d = nullptr;
        if (broker) {
            const std::string full = enchantTag(norm);
            d = enchant_broker_request_dict(broker, full.c_str());
            if (!d && ll != norm) d = enchant_broker_request_dict(broker, ll.c_str());
        }
        dicts.emplace(norm, d);
        return d;
    }

    Mock* mockFor(std::string_view language) {
        const std::string ll = primaryLanguageSubtag(normLang(language));
        if (ll.empty()) return nullptr;
        auto it = mockDicts.find(ll);
        return it == mockDicts.end() ? nullptr : &it->second;
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

    EnchantDict* d = m_impl->dictFor(language);
    if (!d) return true;  // no dictionary -> never flag
    // enchant_dict_check: 0 = correct, >0 = misspelled, <0 = error. Treat an error as correct so a
    // backend hiccup can never paint a squiggle.
    return enchant_dict_check(d, word.data(), static_cast<ssize_t>(word.size())) <= 0;
}

std::vector<std::string> SpellChecker::suggest(std::string_view word, std::string_view language) {
    std::vector<std::string> out;
    if (word.empty()) return out;

    if (Impl::Mock* mock = m_impl->mockFor(language)) {
        if (auto it = mock->suggestions.find(asciiLower(word)); it != mock->suggestions.end())
            out = it->second;
        return out;
    }

    EnchantDict* d = m_impl->dictFor(language);
    if (!d) return out;
    std::size_t n = 0;
    char** s = enchant_dict_suggest(d, word.data(), static_cast<ssize_t>(word.size()), &n);
    if (s) {
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            if (s[i]) out.emplace_back(s[i]);
        enchant_dict_free_string_list(d, s);
    }
    return out;
}

void SpellChecker::addToUserDict(std::string_view word, std::string_view language) {
    if (word.empty()) return;
    if (Impl::Mock* mock = m_impl->mockFor(language)) {
        mock->misspelled.erase(asciiLower(word));  // henceforth correct
        return;
    }
    if (EnchantDict* d = m_impl->dictFor(language))
        enchant_dict_add(d, word.data(), static_cast<ssize_t>(word.size()));
}

void SpellChecker::ignore(std::string_view word) {
    if (!word.empty()) m_impl->ignored.emplace(word);
}

bool SpellChecker::hasDictionary(std::string_view language) {
    if (m_impl->mockFor(language)) return true;
    return m_impl->dictFor(language) != nullptr;
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

}  // namespace mosaic::core::text
