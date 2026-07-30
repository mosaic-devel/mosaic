// Windows implementation of core/text/spell_checker.hpp backed by the OS Spell Checking API
// (ISpellChecker, Windows 8+), used INSTEAD of spell_checker.cpp on Windows (see
// core/CMakeLists.txt).
//
// Like the macOS/NSSpellChecker backend, this avoids cross-building enchant + glib and hands the
// user the dictionaries their OS already has -- including the ones they install through Settings ▸
// Time & Language, which Mosaic then picks up with no bundled wordlists at all. The public
// behaviour matches the enchant backend exactly (a missing dictionary NEVER paints a squiggle; mock
// dictionaries and the session ignore list behave identically), so the Type tool and its tests see
// one interface.
//
// ─────────────────────────────────────────────────────────────────────────────────────────────────
// THREADING / COM APARTMENTS -- the part that is easy to get wrong, so it is spelled out.
//
// SpellCheckWorker (text/spell_worker.cpp, decision D1) runs scans on its OWN std::thread, and the
// UI thread ALSO reaches into the very same SpellChecker for suggest()/addToUserDict()/ignore()/
// hasDictionary(), serialized by the worker's checkerMx. So TWO threads touch this object, and one
// of them (the UI thread) is already an STA: FLTK calls OleInitialize() on the main thread for
// drag-and-drop, and OleInitialize is CoInitializeEx(COINIT_APARTMENTTHREADED).
//
// A raw COM interface pointer is only valid inside the apartment it was created in. Handing the
// worker's pointer to an STA main thread (or vice versa) is precisely the bug class the macOS file
// warns about for NSSpellChecker, and it does not fail loudly -- it fails as an occasional
// RPC_E_WRONG_THREAD or a corrupted call. Marshalling through the global interface table would fix
// it, at the price of a proxy and a message pump the worker thread does not have.
//
// So: EVERY COM object here lives in THREAD-LOCAL storage. Each thread initializes COM itself, on
// first use, and creates its own factory and its own per-language ISpellChecker. No pointer ever
// crosses an apartment boundary, and there is nothing to marshal. The cost is one extra factory and
// one extra per-language checker for the (rare, tiny) UI-thread calls -- the in-proc spell service
// itself is shared, so this duplicates handles, not dictionaries.
//
// The apartment we ASK for is the MTA (COINIT_MULTITHREADED), which matters only on a thread that
// has no COM yet -- in practice the worker. MTA is correct there because the worker blocks on a
// condition variable and never pumps a message loop: an STA on a thread that does not pump is a
// deadlock waiting for the first cross-apartment call. On the UI thread CoInitializeEx returns
// RPC_E_CHANGED_MODE (FLTK's STA wins); that is a SUCCESS for our purposes -- COM is usable, the
// thread simply keeps its STA, and the per-thread objects created in it are STA-local and correct.
//
// ⚠ RPC_E_CHANGED_MODE must NOT be followed by CoUninitialize: we did not initialize anything, and
// balancing a call we never made would tear down FLTK's apartment.
// ─────────────────────────────────────────────────────────────────────────────────────────────────

#include "core/text/spell_checker.hpp"

#include <spellcheck.h> // pulls in windows.h + ole2.h itself (COM_NO_WINDOWS_H is not set)

#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/text/language.hpp"

namespace mosaic::core::text {
namespace {

// ⚠ MinGW's <spellcheck.h> DECLARES these GUIDs (DEFINE_GUID without INITGUID expands to a bare
// `extern const GUID`) but MinGW's libuuid.a does NOT carry them -- verified with nm: of the GUIDs
// this file needs, only IID_IEnumString is in the archive, so linking `uuid` would still leave
// CLSID_SpellCheckerFactory and IID_ISpellCheckerFactory undefined. They are therefore spelled out
// locally, straight from the header's own DEFINE_GUID lines.
//
// The two alternatives were both rejected: `#define INITGUID` before the include would emit every
// GUID reachable from <windows.h> into this translation unit, and `__uuidof` is an MSVC extension
// that MinGW only emulates through a template (_mingw.h) whose availability differs between the GNU
// toolchain and the llvm-mingw clang we build the aarch64 target with.
constexpr GUID kClsidSpellCheckerFactory = {
    0x7ab36653, 0x1796, 0x484b, {0xbd, 0xfa, 0xe7, 0x4f, 0x1d, 0xb7, 0xc1, 0xdc}};
constexpr GUID kIidSpellCheckerFactory = {
    0x8e018a9d, 0x2415, 0x4677, {0xbf, 0x08, 0x79, 0x4e, 0xa6, 0x1f, 0x94, 0xbb}};

// Lowercase a BCP-47/locale string and unify the separator, so "en_US" and "EN-us" key alike.
// (Byte-identical to the enchant and macOS backends -- the MOCK dictionaries the deterministic
// tests install are keyed through this, so it must not drift.)
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

// UTF-8 -> UTF-16, the encoding every Win32 -W entry point and every COM string wants. Explicitly
// CP_UTF8 rather than CP_ACP: Mosaic's strings are UTF-8 by contract, and the active code page is
// still a legacy ANSI page on most machines (the same trap common/fs_path.hpp exists to close).
std::wstring toWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

// UTF-16 -> UTF-8, for the suggestion strings COM hands back.
std::string fromWide(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};  // n counts the terminator
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n - 1, nullptr, nullptr);
    return out;
}

// The per-thread COM state: an apartment, a factory, and the ISpellCheckers created in it. See the
// apartment discussion at the top of the file for why this is thread_local and not shared.
class ThreadCom {
public:
    ThreadCom() = default;
    ThreadCom(const ThreadCom&) = delete;
    ThreadCom& operator=(const ThreadCom&) = delete;

    // Runs at thread exit (or process exit for the main thread). Everything it touches was created
    // on THIS thread, so the releases are apartment-legal; CoUninitialize comes last, after the
    // last interface is gone. If the platform ever failed to run this destructor the consequence is
    // a leak at exit and nothing worse -- deliberately, because thread_local destructor support is
    // the one piece of this file that depends on the MinGW threading model rather than on Win32.
    ~ThreadCom() {
        for (auto& [tag, sc] : m_checkers)
            if (sc != nullptr) sc->Release();
        m_checkers.clear();
        if (m_factory != nullptr) {
            m_factory->Release();
            m_factory = nullptr;
        }
        if (m_owned) CoUninitialize();
    }

    // The factory for this thread, or null if COM or the spell service is unavailable. Attempted
    // exactly once: a machine without the feature (Windows Server core, a stripped image) must not
    // pay a failing CoCreateInstance per keystroke.
    ISpellCheckerFactory* factory() {
        if (m_tried) return m_factory;
        m_tried = true;

        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr == S_OK || hr == S_FALSE) {
            // S_OK: this thread's apartment is ours. S_FALSE: COM was already up on this thread in
            // the SAME mode -- the call still took a reference, so it still needs balancing.
            m_owned = true;
        } else if (hr != RPC_E_CHANGED_MODE) {
            return nullptr;  // COM is genuinely unusable; do NOT CoUninitialize a failed init
        }
        // RPC_E_CHANGED_MODE falls through with m_owned = false: the thread is already in the other
        // apartment kind (FLTK's STA on the UI thread), which is fine -- COM is usable, and the
        // objects we create below belong to that apartment.

        void* p = nullptr;
        if (SUCCEEDED(CoCreateInstance(kClsidSpellCheckerFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       kIidSpellCheckerFactory, &p))) {
            m_factory = static_cast<ISpellCheckerFactory*>(p);
        }
        return m_factory;
    }

    // The ISpellChecker for `language` (a BCP-47 tag as normalizeBcp47 spells it), or null when the
    // machine has no dictionary for it. Cached per resolved tag; a cached null means "asked, none",
    // so a paragraph in an uninstalled language costs one lookup, not one per word.
    ISpellChecker* checkerFor(const std::string& bcp47) {
        if (bcp47.empty()) return nullptr;
        if (auto it = m_resolved.find(bcp47); it != m_resolved.end()) {
            if (it->second.empty()) return nullptr;  // asked before, this machine has none
            const auto ck = m_checkers.find(it->second);
            return ck == m_checkers.end() ? nullptr : ck->second;
        }

        const std::string tag = resolveTag(bcp47);
        m_resolved.emplace(bcp47, tag);
        if (tag.empty()) return nullptr;

        auto it = m_checkers.find(tag);
        if (it == m_checkers.end()) {
            ISpellChecker* sc = nullptr;
            if (ISpellCheckerFactory* f = factory(); f != nullptr) {
                if (FAILED(f->CreateSpellChecker(toWide(tag).c_str(), &sc))) sc = nullptr;
            }
            it = m_checkers.emplace(tag, sc).first;
        }
        return it->second;
    }

    // Forward an Ignore to every checker this thread has already built. See SpellChecker::ignore()
    // for why this is a courtesy and not the mechanism.
    void ignoreInAll(const wchar_t* word) {
        for (auto& [tag, sc] : m_checkers)
            if (sc != nullptr) sc->Ignore(word);
    }

private:
    // Which tag this machine can actually check `bcp47` with: the full tag if it is supported, else
    // the bare primary subtag, else any supported tag sharing that subtag, else "" for none.
    //
    // The last step is what the macOS backend's available() does, and it is not redundant:
    // IsSupported() matches the tag it is GIVEN, and Windows reports its dictionaries as full tags
    // ("en-US", "de-DE"), so a paragraph tagged plain "de" on a machine with only de-DE installed
    // would otherwise read as "no dictionary" and silently lose its squiggles.
    std::string resolveTag(const std::string& bcp47) {
        ISpellCheckerFactory* f = factory();
        if (f == nullptr) return {};

        const std::string ll = primaryLanguageSubtag(bcp47);
        for (const std::string& candidate : {bcp47, ll}) {
            if (candidate.empty()) continue;
            BOOL supported = FALSE;
            if (SUCCEEDED(f->IsSupported(toWide(candidate).c_str(), &supported)) && supported)
                return candidate;
        }
        if (ll.empty()) return {};
        for (const std::string& have : supportedTags())
            if (primaryLanguageSubtag(have) == ll) return have;
        return {};
    }

    // get_SupportedLanguages(), drained once per thread into UTF-8. The list only changes when the
    // user installs a language pack, which takes a reboot-ish amount of ceremony; re-enumerating
    // per lookup would be the expensive way to learn nothing.
    const std::vector<std::string>& supportedTags() {
        if (m_supportedLoaded) return m_supported;
        m_supportedLoaded = true;
        if (ISpellCheckerFactory* f = factory(); f != nullptr) {
            IEnumString* e = nullptr;
            if (SUCCEEDED(f->get_SupportedLanguages(&e)) && e != nullptr) {
                LPOLESTR s = nullptr;
                while (e->Next(1, &s, nullptr) == S_OK && s != nullptr) {
                    m_supported.push_back(normalizeBcp47(fromWide(s)));
                    CoTaskMemFree(s);  // the enumerator hands ownership over, one string at a time
                    s = nullptr;
                }
                e->Release();
            }
        }
        return m_supported;
    }

    bool m_tried = false;
    bool m_owned = false;  // we called CoInitializeEx successfully -> we must CoUninitialize
    ISpellCheckerFactory* m_factory = nullptr;
    std::unordered_map<std::string, ISpellChecker*> m_checkers;  // by RESOLVED tag (null = none)
    std::unordered_map<std::string, std::string> m_resolved;     // requested tag -> resolved tag
    std::vector<std::string> m_supported;
    bool m_supportedLoaded = false;
};

// One per thread, constructed on first touch. Not a pointer-to-heap on purpose: the destructor is
// what balances CoInitializeEx and releases the interfaces in their own apartment.
thread_local ThreadCom t_com;

// Does the error enumeration for `word` contain anything? Any entry at all means misspelled -- the
// enumeration exists only to report errors, and Check() over a single bare word can produce at most
// one. A FAILED call yields false (i.e. "correct"), which is the header's hard invariant: a backend
// hiccup must never paint a squiggle.
bool hasSpellingError(ISpellChecker* sc, std::string_view word) {
    IEnumSpellingError* errors = nullptr;
    if (!SUCCEEDED(sc->Check(toWide(word).c_str(), &errors)) || errors == nullptr) return false;
    ISpellingError* first = nullptr;
    const bool any = (errors->Next(&first) == S_OK) && first != nullptr;
    if (first != nullptr) first->Release();
    errors->Release();
    return any;
}

}  // namespace

struct SpellChecker::Impl {
    struct Mock {
        std::unordered_set<std::string> misspelled;
        std::unordered_map<std::string, std::vector<std::string>> suggestions;
    };

    // Everything the Impl itself owns is plain data, shared across the threads that reach this
    // object under the worker's checkerMx. The COM handles are deliberately NOT here -- see the
    // apartment note at the top -- so there is nothing thread-affine in the object's lifetime and
    // the move constructor the header promises stays trivial.
    std::unordered_map<std::string, Mock> mockDicts;  // keyed by primary subtag
    std::unordered_set<std::string> ignored;          // session-scoped, checker-wide (Ignore All)

    Mock* mockFor(std::string_view language) {
        const std::string ll = primaryLanguageSubtag(normLang(language));
        if (ll.empty()) return nullptr;
        auto it = mockDicts.find(ll);
        return it == mockDicts.end() ? nullptr : &it->second;
    }

    // The calling thread's checker for `language`. normalizeBcp47 (core/text/language.cpp) is
    // already exactly the shape this API documents -- "en-US", lowercase language, uppercase
    // region, no ".UTF-8" or "@euro" tail -- so there is no per-backend tag mapping here, unlike
    // enchant's "en_US" and NSSpellChecker's "en_US".
    static ISpellChecker* checkerFor(std::string_view language) {
        return t_com.checkerFor(normalizeBcp47(language));
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

    ISpellChecker* sc = Impl::checkerFor(language);
    if (sc == nullptr) return true;  // no dictionary -> never flag
    return !hasSpellingError(sc, word);
}

std::vector<std::string> SpellChecker::suggest(std::string_view word, std::string_view language) {
    std::vector<std::string> out;
    if (word.empty()) return out;

    if (Impl::Mock* mock = m_impl->mockFor(language)) {
        if (auto it = mock->suggestions.find(asciiLower(word)); it != mock->suggestions.end())
            out = it->second;
        return out;
    }

    ISpellChecker* sc = Impl::checkerFor(language);
    if (sc == nullptr) return out;

    IEnumString* e = nullptr;
    // Suggest() reports S_FALSE when the word is spelled correctly (and then enumerates nothing),
    // so the SUCCEEDED() test must not be mistaken for "there are suggestions".
    if (!SUCCEEDED(sc->Suggest(toWide(word).c_str(), &e)) || e == nullptr) return out;
    LPOLESTR s = nullptr;
    while (e->Next(1, &s, nullptr) == S_OK && s != nullptr) {
        out.push_back(fromWide(s));
        CoTaskMemFree(s);  // each string is ours to free, exactly as the enumeration hands it over
        s = nullptr;
    }
    e->Release();
    return out;
}

void SpellChecker::addToUserDict(std::string_view word, std::string_view language) {
    if (word.empty()) return;
    if (Impl::Mock* mock = m_impl->mockFor(language)) {
        mock->misspelled.erase(asciiLower(word));  // henceforth correct
        return;
    }
    // ISpellChecker::Add writes to the USER's own dictionary under their profile, which the OS
    // spell service owns and reloads. That is the property enchant's personal wordlist gives on
    // Linux (decision D2: learned words persist across sessions) WITHOUT Mosaic writing a file
    // anywhere -- exactly what the project's "never write a user's file without an explicit
    // Save/Export" rule wants, and the reason this backend has no wordlist of its own to manage or
    // migrate.
    if (ISpellChecker* sc = Impl::checkerFor(language); sc != nullptr)
        sc->Add(toWide(word).c_str());
}

void SpellChecker::ignore(std::string_view word) {
    if (word.empty()) return;
    // The session set is the AUTHORITY, identical to the enchant and macOS backends: the header's
    // contract is that Ignore All is checker-wide and language-agnostic, whereas
    // ISpellChecker::Ignore is scoped to one checker instance for one language (and, here, to one
    // thread's instance).
    m_impl->ignored.emplace(word);
    // Still forwarded to the checkers this thread already holds, so the OS agrees with us wherever
    // it is consulted directly (Suggest, for one, will stop proposing a replacement for a word it
    // has been told to ignore). Only the calling thread's instances exist to be told -- which is
    // harmless precisely because the set above is what correct() actually reads.
    t_com.ignoreInAll(toWide(word).c_str());
}

bool SpellChecker::hasDictionary(std::string_view language) {
    if (m_impl->mockFor(language)) return true;
    return Impl::checkerFor(language) != nullptr;
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
