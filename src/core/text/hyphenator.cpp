#include "core/text/hyphenator.hpp"

#include <hyphen.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/fs_path.hpp" // the path<->UTF-8 crossing; the identity on POSIX
#include "core/text/language.hpp"

#if defined(_WIN32)
#include <fstream>
#include <iterator>

#include "common/settings.hpp" // installedDataDir(): the .dic files ship inside the app payload
#endif

#ifndef MOSAIC_HYPHEN_DIR
#define MOSAIC_HYPHEN_DIR "/usr/share/hyphen"
#endif

namespace mosaic::core::text {
namespace {

// Lowercase a BCP-47/locale string and unify the separator, so "en_US" and "EN-us" key alike.
std::string normLang(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '_') out += '-';
        else out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// The region subtag (uppercased) of a normalized tag, or "" -- "en-us" -> "US".
std::string regionOf(std::string_view norm) {
    const std::size_t dash = norm.find('-');
    if (dash == std::string_view::npos) return {};
    std::string r{norm.substr(dash + 1)};
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

bool isUtf8Boundary(std::string_view s, std::size_t i) {
    return i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80;
}

}  // namespace

struct Hyphenator::Impl {
    // Dictionaries loaded from disk, keyed by resolved absolute path (so en-GB and en-US, both
    // hyph_en_US.dic, share one load). langToPath caches the file-resolution per language (an empty
    // path means "resolved, no dictionary"). dataDicts holds test-injected in-memory dictionaries,
    // keyed by primary subtag, and is consulted first.
    std::unordered_map<std::string, HyphenDict*> fileDicts;
    std::unordered_map<std::string, std::string> langToPath;
    std::unordered_map<std::string, HyphenDict*> dataDicts;

    ~Impl() {
        for (auto& [k, d] : fileDicts)
            if (d) hnj_hyphen_free(d);
        for (auto& [k, d] : dataDicts)
            if (d) hnj_hyphen_free(d);
    }

    static std::filesystem::path resolveDictPath(const std::string& ll, const std::string& region) {
        namespace fs = std::filesystem;
        std::string llUpper = ll;
        for (char& c : llUpper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        // One directory's worth of the search, unchanged from when it was the body of the loop
        // below: the three preferred file names, then any hyph_<ll>_*.dic at all. Lifted into a
        // lambda so the Windows payload directory can reuse it verbatim rather than duplicating the
        // name rules.
        const auto findIn = [&](const fs::path& base) -> fs::path {
            std::error_code ec;
            if (!fs::is_directory(base, ec)) return {};
            std::array<std::string, 3> names = {
                region.empty() ? std::string{} : "hyph_" + ll + "_" + region + ".dic",
                "hyph_" + ll + "_" + llUpper + ".dic",  // de -> hyph_de_DE
                "hyph_" + ll + ".dic",
            };
            for (const std::string& n : names) {
                if (n.empty()) continue;
                const fs::path p = base / n;
                if (fs::is_regular_file(p, ec)) return p;  // follows symlinks (regional aliases)
            }
            // Last resort: any hyph_<ll>_*.dic in the dir.
            const std::string prefix = "hyph_" + ll + "_";
            for (const auto& entry : fs::directory_iterator(base, ec)) {
                // utf8FromPath, not .string(): a stray file whose name the active Windows code page
                // cannot spell would make .string() THROW out of a directory scan. The
                // prefix/suffix this compares against are ASCII, so UTF-8 answers identically --
                // and on POSIX the call is literally the .string() it replaces.
                const std::string fn = common::utf8FromPath(entry.path().filename());
                if (fn.size() > 4 && fn.compare(0, prefix.size(), prefix) == 0 &&
                    fn.compare(fn.size() - 4, 4, ".dic") == 0)
                    return entry.path();
            }
            return {};
        };

#if defined(_WIN32)
        // Windows has no system hyphenation API and no system dictionary directory, so the .dic
        // files ship INSIDE the application payload and are located relative to mosaic.exe --
        // installedDataDir()/"hyphen", the same executable-relative resolution the brush set, the
        // document templates and the ICC profiles use. Tried FIRST, and it is the only entry that
        // can ever match here: MOSAIC_HYPHEN_DIR is baked from the Linux cross-build host and
        // neither Unix path exists on a PC. They stay in the order below so there is one search
        // order to reason about rather than two.
        if (const fs::path data = common::installedDataDir(); !data.empty()) {
            if (const fs::path hit = findIn(data / "hyphen"); !hit.empty()) return hit;
        }
#endif
        static const std::array<const char*, 2> dirs = {MOSAIC_HYPHEN_DIR,
                                                        "/usr/share/myspell/dicts"};
        for (const char* dir : dirs) {
            if (const fs::path hit = findIn(fs::path{dir}); !hit.empty()) return hit;
        }
        return {};
    }

    // Load one dictionary FILE. Split out for the sake of the Windows branch, which cannot go
    // through libhyphen's own loader: hnj_hyphen_load() fopen()s the name it is handed, the
    // MSVCRT's fopen takes ANSI bytes, and libhyphen offers no wide variant -- so a payload sitting
    // under a profile name the active code page cannot spell would be unopenable, which on Linux
    // never came up because /usr/share/hyphen is ASCII by construction. Read the bytes here instead
    // (ifstream takes a std::filesystem::path, i.e. the native UTF-16 name) and hand them to the
    // same in-memory loader the loadDictionaryData test hook uses.
    static HyphenDict* loadDictFile(const std::string& utf8Path) {
#if defined(_WIN32)
        std::ifstream in(common::pathFromUtf8(utf8Path), std::ios::binary);
        if (!in) return nullptr;
        const std::string data((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        if (data.empty()) return nullptr;
        return hnj_hyphen_load_data(data.data(), data.size());
#else
        return hnj_hyphen_load(utf8Path.c_str());
#endif
    }

    HyphenDict* dictFor(std::string_view language) {
        const std::string norm = normLang(language);
        const std::string ll = primaryLanguageSubtag(norm);
        if (ll.empty()) return nullptr;

        // Test-injected data dictionaries win (keyed by primary subtag).
        if (auto it = dataDicts.find(ll); it != dataDicts.end()) return it->second;

        // Resolve the file path (cached), then load (cached by path).
        auto pit = langToPath.find(norm);
        if (pit == langToPath.end()) {
            const std::filesystem::path p = resolveDictPath(ll, regionOf(norm));
            // utf8FromPath rather than p.string(): the map's value is the cache key AND the name
            // the loader reopens, and on Windows .string() transcodes to the active code page
            // (lossy, and it throws outright when the page cannot spell the name). The identity on
            // POSIX, so the Linux cache holds exactly the bytes it always did (common/fs_path.hpp).
            pit = langToPath.emplace(norm, common::utf8FromPath(p)).first;
        }
        if (pit->second.empty()) return nullptr;  // no dictionary for this language

        auto dit = fileDicts.find(pit->second);
        if (dit == fileDicts.end())
            dit = fileDicts.emplace(pit->second, loadDictFile(pit->second)).first;
        return dit->second;
    }
};

Hyphenator::Hyphenator() : m_impl(std::make_unique<Impl>()) {}
Hyphenator::~Hyphenator() = default;
Hyphenator::Hyphenator(Hyphenator&&) noexcept = default;
Hyphenator& Hyphenator::operator=(Hyphenator&&) noexcept = default;

bool Hyphenator::loadDictionaryData(std::string_view language, std::string_view dicData) {
    const std::string ll = primaryLanguageSubtag(normLang(language));
    if (ll.empty()) return false;
    HyphenDict* d = hnj_hyphen_load_data(dicData.data(), dicData.size());
    if (!d) return false;
    if (auto it = m_impl->dataDicts.find(ll); it != m_impl->dataDicts.end() && it->second)
        hnj_hyphen_free(it->second);
    m_impl->dataDicts[ll] = d;
    return true;
}

bool Hyphenator::hasDictionary(std::string_view language) {
    return m_impl->dictFor(language) != nullptr;
}

std::vector<std::size_t> Hyphenator::hyphenationPoints(std::string_view word,
                                                       std::string_view language, int lhmin,
                                                       int rhmin) {
    std::vector<std::size_t> out;
    HyphenDict* dict = m_impl->dictFor(language);
    if (!dict) return out;

    const int n = static_cast<int>(word.size());
    // A break needs at least lhmin + rhmin characters; nothing shorter than that can hyphenate.
    if (n < 4) return out;

    // libhyphen matches case-sensitively against lowercase patterns; ASCII-lowercase byte-for-byte
    // so break offsets still index the original word (non-ASCII case is left as-is -- acceptable;
    // all-caps words are usually skipped by the caller anyway).
    std::string w(word);
    for (char& c : w)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');

    const int lh = lhmin > 0 ? lhmin : (dict->lhmin > 0 ? dict->lhmin : 2);
    const int rh = rhmin > 0 ? rhmin : (dict->rhmin > 0 ? dict->rhmin : 3);
    const int clh = dict->clhmin > 0 ? dict->clhmin : lh;
    const int crh = dict->crhmin > 0 ? dict->crhmin : rh;

    // rep/pos/cut must be the ADDRESSES of null pointers, not null themselves: libhyphen 0.4.0
    // dereferences them and lazily allocates (word_size-length) arrays only when a NON-STANDARD
    // replacement fires (e.g. German "Zuk-ker"). Standard hyphenation leaves them null; we still
    // free whatever was allocated. (Passing plain null here segfaults inside hyphenate3.)
    std::vector<char> hyphens(static_cast<std::size_t>(n) + 5, 0);
    char** rep = nullptr;
    int* pos = nullptr;
    int* cut = nullptr;
    const int rc = hnj_hyphen_hyphenate3(dict, w.c_str(), n, hyphens.data(), nullptr, &rep, &pos,
                                         &cut, lh, rh, clh, crh);
    if (rc == 0) {
        for (int i = 0; i + 1 < n; ++i) {
            if (hyphens[i] & 1) {
                const std::size_t tail = static_cast<std::size_t>(i) + 1;
                if (isUtf8Boundary(word, tail)) out.push_back(tail);
            }
        }
    }
    if (rep) {
        for (int i = 0; i < n; ++i)
            if (rep[i]) std::free(rep[i]);
        std::free(rep);
    }
    std::free(pos);
    std::free(cut);
    return out;
}

}  // namespace mosaic::core::text
