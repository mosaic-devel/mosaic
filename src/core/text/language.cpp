#include "core/text/language.hpp"

#include <cctype>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <windows.h> // GetUserDefaultLocaleName -- Windows has no POSIX locale environment
#endif

namespace mosaic::core::text {
namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
char upper(char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }

}  // namespace

std::string normalizeBcp47(std::string_view locale) {
    // Drop the ".encoding" and "@modifier" suffixes; take just "language[_territory]".
    std::size_t end = locale.size();
    for (std::size_t k = 0; k < locale.size(); ++k) {
        if (locale[k] == '.' || locale[k] == '@') { end = k; break; }
    }
    std::string_view core = locale.substr(0, end);
    if (core.empty() || core == "C" || core == "POSIX") return {};

    // Split on '_' or '-' into language + region (ignore any further subtags for v1).
    std::size_t sep = core.size();
    for (std::size_t k = 0; k < core.size(); ++k) {
        if (core[k] == '_' || core[k] == '-') { sep = k; break; }
    }
    std::string_view lang = core.substr(0, sep);
    std::string_view region = (sep < core.size()) ? core.substr(sep + 1) : std::string_view{};

    std::string out;
    out.reserve(lang.size() + 1 + region.size());
    for (char c : lang) out += lower(c);           // language subtag: lowercase
    if (!region.empty()) {
        out += '-';
        for (char c : region) out += upper(c);     // region subtag: uppercase
    }
    return out;
}

std::string primaryLanguageSubtag(std::string_view bcp47) {
    std::size_t sep = bcp47.size();
    for (std::size_t k = 0; k < bcp47.size(); ++k) {
        if (bcp47[k] == '-' || bcp47[k] == '_') { sep = k; break; }
    }
    std::string out;
    out.reserve(sep);
    for (std::size_t k = 0; k < sep; ++k) out += lower(bcp47[k]);
    return out;
}

std::string detectSystemLanguage() {
    // POSIX precedence for the message locale: LC_ALL overrides everything, then LC_MESSAGES, then
    // LANG. (LANGUAGE is a colon-list of gettext preferences; take its first entry if present.)
    const char* env = nullptr;
    if (const char* v = std::getenv("LC_ALL"); v != nullptr && *v != '\0') env = v;
    else if (const char* v2 = std::getenv("LC_MESSAGES"); v2 != nullptr && *v2 != '\0') env = v2;
    else if (const char* v3 = std::getenv("LANG"); v3 != nullptr && *v3 != '\0') env = v3;
    else if (const char* v4 = std::getenv("LANGUAGE"); v4 != nullptr && *v4 != '\0') env = v4;
#if defined(_WIN32)
    // Windows sets NONE of those variables, so the scan above finds nothing and this would answer
    // "" -- which silently collapses the whole language chain (spell-check dictionary AND
    // hyphenation dictionary, both routed through resolveLanguage) to the caller's "en" fallback.
    // A German user would get English squiggles on German text with nothing to explain it.
    // GetUserDefaultLocaleName already speaks BCP-47 ("de-DE"), which is the shape this function
    // returns, so normalizeBcp47 passes it through unchanged. This is the REGIONAL/UI language, the
    // closest Windows equivalent of LC_MESSAGES. (S57)
    if (env == nullptr) {
        wchar_t buf[LOCALE_NAME_MAX_LENGTH] = {};
        if (::GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH) > 0) {
            std::string tag;
            for (const wchar_t* p = buf; *p != L'\0'; ++p)
                tag.push_back(static_cast<char>(*p)); // a locale name is ASCII by definition
            return normalizeBcp47(tag);
        }
    }
#endif
    if (env == nullptr) return {};

    std::string_view s{env};
    if (const std::size_t colon = s.find(':'); colon != std::string_view::npos)
        s = s.substr(0, colon);  // LANGUAGE = "de:en" -> take "de"
    return normalizeBcp47(s);
}

std::string resolveLanguage(std::string_view paragraph, std::string_view documentDefault,
                            std::string_view appDefault) {
    if (!paragraph.empty()) return std::string{paragraph};
    if (!documentDefault.empty()) return std::string{documentDefault};
    return std::string{appDefault};
}

}  // namespace mosaic::core::text
