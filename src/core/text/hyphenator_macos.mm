// macOS implementation of core/text/hyphenator.hpp backed by the system hyphenator
// (CoreFoundation's CFStringGetHyphenationLocationBeforeIndex), used INSTEAD of hyphenator.cpp on
// Apple (see core/CMakeLists.txt).
//
// This avoids cross-building libhyphen AND bundling Liang-pattern .dic dictionaries for macOS: the
// OS ships its own hyphenation data (the engine TextKit uses), keyed by locale, with no
// /usr/share/hyphen equivalent to install. The public behaviour matches the libhyphen backend -- the
// SAME UTF-8 byte-offset break-point convention feeds the shaper (shaping.cpp), and a language with
// no OS hyphenation yields no break points, so text simply wraps at spaces exactly as a missing .dic
// does on Linux.
//
// UTF-16 <-> UTF-8 (Mac-side UNVERIFIED -- there is no Mac here to run it): CoreFoundation indexes
// strings in UTF-16 code units, but Hyphenator returns UTF-8 BYTE offsets into `word`. We decode the
// (well-formed) UTF-8 word once into a UTF-16-unit -> byte-offset table and map every CFIndex break
// through it, so the offsets the shaper receives are byte-for-byte the libhyphen convention. The
// decode is independent of CoreFoundation's own UTF-16 buffer, but the two agree for any well-formed
// UTF-8 (CFStringCreateWithBytes performs no normalization). See hyphenationPoints for the details.
//
// Liang's algorithm dates to 1983; CoreFoundation is a system framework (covered by the
// GPLv3 System Library exception), so nothing here changes Mosaic's licensing posture.

#include "core/text/hyphenator.hpp"

#import <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/text/language.hpp"

namespace mosaic::core::text {
namespace {

// Lowercase a BCP-47/locale string and unify the separator ("en_US"/"EN-us" -> "en-us"), matching
// the libhyphen backend and spell_checker_macos.mm so every language key normalizes identically.
std::string normLang(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '_') out += '-';
        else out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// A CoreFoundation locale identifier for a normalized tag: lowercase language, uppercase region,
// underscore separator ("en-us" -> "en_US", "de" -> "de") -- the shape CFLocaleCreate expects.
std::string cfLocaleId(std::string_view norm) {
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

// Create a CFLocale from a locale-identifier string (caller CFReleases the result). NULL if `id` is
// empty or cannot be encoded.
CFLocaleRef makeLocale(const std::string& id) {
    if (id.empty()) return nullptr;
    CFStringRef s = CFStringCreateWithBytes(kCFAllocatorDefault,
                                            reinterpret_cast<const UInt8*>(id.data()),
                                            static_cast<CFIndex>(id.size()), kCFStringEncodingUTF8,
                                            false);
    if (!s) return nullptr;
    CFLocaleRef loc = CFLocaleCreate(kCFAllocatorDefault, s);
    CFRelease(s);
    return loc;
}

}  // namespace

struct Hyphenator::Impl {
    // A resolved CFLocale for which the OS has hyphenation, cached per normalized tag (a NULL value
    // caches "no hyphenation for this tag"). Owned -- every non-null entry is CFReleased in ~Impl.
    std::unordered_map<std::string, CFLocaleRef> locales;

    // Test hook: primary subtags that had a dictionary injected via loadDictionaryData. CoreFoundation
    // cannot hyphenate a synthetic pattern dictionary, so this only makes hasDictionary() report the
    // language available; see loadDictionaryData / hyphenationPoints for why break points cannot follow.
    std::unordered_set<std::string> injected;

    ~Impl() {
        for (auto& [k, l] : locales)
            if (l) CFRelease(l);
    }

    // The best OS-hyphenation CFLocale for `language`, or NULL if the OS has none. Tries the full tag
    // first (region-specific patterns when present, e.g. en_GB vs en_US), then the bare primary
    // subtag -- mirroring the libhyphen backend's dictFor() resolution. Cached per normalized tag;
    // the returned locale is owned by the cache, not by the caller.
    CFLocaleRef localeFor(std::string_view language) {
        const std::string norm = normLang(language);
        if (auto it = locales.find(norm); it != locales.end()) return it->second;

        CFLocaleRef chosen = nullptr;
        const std::string ids[2] = {cfLocaleId(norm), primaryLanguageSubtag(norm)};
        for (const std::string& id : ids) {
            if (id.empty()) continue;
            CFLocaleRef loc = makeLocale(id);
            if (!loc) continue;
            if (CFStringIsHyphenationAvailableForLocale(loc)) {
                chosen = loc;
                break;
            }
            CFRelease(loc);
        }
        locales.emplace(norm, chosen);  // caches NULL too (a negative result is worth remembering)
        return chosen;
    }
};

Hyphenator::Hyphenator() : m_impl(std::make_unique<Impl>()) {}
Hyphenator::~Hyphenator() = default;
Hyphenator::Hyphenator(Hyphenator&&) noexcept = default;
Hyphenator& Hyphenator::operator=(Hyphenator&&) noexcept = default;

bool Hyphenator::loadDictionaryData(std::string_view language, std::string_view dicData) {
    // No libhyphen on macOS, so a synthetic libhyphen .dic cannot drive CoreFoundation's hyphenator.
    // Keep the hook complete: record the language as available (so hasDictionary() stays consistent)
    // and accept any non-empty data. The deterministic break-point tests that rely on the injected
    // patterns are Linux-only -- the macOS preset sets MOSAIC_BUILD_TESTS=OFF, so no Mac build asserts
    // on injected break positions (which this backend cannot reproduce; see hyphenationPoints).
    const std::string ll = primaryLanguageSubtag(normLang(language));
    if (ll.empty() || dicData.empty()) return false;
    m_impl->injected.insert(ll);
    return true;
}

bool Hyphenator::hasDictionary(std::string_view language) {
    if (!m_impl->injected.empty() &&
        m_impl->injected.count(primaryLanguageSubtag(normLang(language))) != 0)
        return true;
    return m_impl->localeFor(language) != nullptr;
}

std::vector<std::size_t> Hyphenator::hyphenationPoints(std::string_view word,
                                                       std::string_view language, int lhmin,
                                                       int rhmin) {
    std::vector<std::size_t> out;
    CFLocaleRef locale = m_impl->localeFor(language);
    if (!locale) return out;  // no OS hyphenation for this language (an injected mock lands here too)

    // Same cheap short-circuit as the libhyphen backend: nothing this short (in bytes) can hold a
    // break, and it keeps the table-building below off the hot path for one/two-letter tokens.
    if (word.size() < 4) return out;

    // Decode the (well-formed) UTF-8 word ONCE into two parallel tables indexed by UTF-16 code unit
    // -- the unit CoreFoundation reports break positions in:
    //   u16ToByte[k] = the UTF-8 byte offset of the code point that owns UTF-16 unit k
    //   u16ToCp[k]   = the code-point index of that code point (for lhmin/rhmin, counted in chars)
    // Each table gets one entry per UTF-16 unit plus a final past-the-end entry, so a CFIndex break
    // `hp` (always on a code-point boundary) maps to byte offset u16ToByte[hp] and leaves u16ToCp[hp]
    // code points to its left. A supplementary character contributes two UTF-16 units that both map
    // to the same byte start / code-point index (a break inside a surrogate pair never occurs).
    std::vector<std::size_t> u16ToByte;
    std::vector<std::size_t> u16ToCp;
    u16ToByte.reserve(word.size() + 1);
    u16ToCp.reserve(word.size() + 1);
    std::size_t cp = 0;
    for (std::size_t i = 0; i < word.size();) {
        const unsigned char c = static_cast<unsigned char>(word[i]);
        std::size_t nbytes;
        std::uint32_t scalar;
        if (c < 0x80) {
            nbytes = 1;
            scalar = c;
        } else if ((c & 0xE0) == 0xC0) {
            nbytes = 2;
            scalar = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            nbytes = 3;
            scalar = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            nbytes = 4;
            scalar = c & 0x07u;
        } else {
            nbytes = 1;
            scalar = 0xFFFDu;  // stray continuation / invalid lead byte: treat as one 1-byte unit
        }
        std::size_t got = 1;
        for (; got < nbytes && i + got < word.size(); ++got) {
            const unsigned char cc = static_cast<unsigned char>(word[i + got]);
            if ((cc & 0xC0) != 0x80) break;  // not a continuation byte -> truncated sequence
            scalar = (scalar << 6) | (cc & 0x3Fu);
        }
        if (got != nbytes) {  // truncated at end of string: back off to a single replacement unit
            nbytes = 1;
            scalar = 0xFFFDu;
        }
        const std::size_t units = (scalar >= 0x10000u) ? 2u : 1u;  // supplementary -> surrogate pair
        for (std::size_t u = 0; u < units; ++u) {
            u16ToByte.push_back(i);
            u16ToCp.push_back(cp);
        }
        i += nbytes;
        ++cp;
    }
    u16ToByte.push_back(word.size());  // past-the-end boundary (a break "before end" is never emitted)
    u16ToCp.push_back(cp);
    const std::size_t totalCps = cp;

    CFStringRef str = CFStringCreateWithBytes(kCFAllocatorDefault,
                                              reinterpret_cast<const UInt8*>(word.data()),
                                              static_cast<CFIndex>(word.size()),
                                              kCFStringEncodingUTF8, false);
    if (!str) return out;  // malformed UTF-8 -> no hyphenation (shaped runs are always well-formed)
    const CFIndex u16len = CFStringGetLength(str);
    const CFRange full = CFRangeMake(0, u16len);

    // Walk break points from the end toward the start: the function returns the LAST hyphenation
    // point strictly before `loc`, so seeding the next query with that point enumerates every break
    // in descending order without gaps, and terminates because `hp` strictly decreases toward 0.
    // Convert each to a byte offset and (optionally) apply the caller's lhmin/rhmin, measured in code
    // points like libhyphen's own minima. lhmin/rhmin <= 0 defers entirely to the OS locale's own
    // minima (the "dictionary default" this backend has); the shaper always calls with the defaults.
    for (CFIndex loc = u16len; loc > 0;) {
        const CFIndex hp =
            CFStringGetHyphenationLocationBeforeIndex(str, loc, full, 0, locale, nullptr);
        if (hp == kCFNotFound || hp <= 0 || hp >= u16len) break;
        loc = hp;  // the next search is strictly before this break
        const std::size_t idx = static_cast<std::size_t>(hp);
        const std::size_t cpBefore = idx < u16ToCp.size() ? u16ToCp[idx] : totalCps;
        if (lhmin > 0 && cpBefore < static_cast<std::size_t>(lhmin)) continue;
        if (rhmin > 0 && (totalCps - cpBefore) < static_cast<std::size_t>(rhmin)) continue;
        out.push_back(idx < u16ToByte.size() ? u16ToByte[idx] : word.size());
    }
    CFRelease(str);

    std::sort(out.begin(), out.end());  // libhyphen returns ascending & unique -- match that shape
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace mosaic::core::text
