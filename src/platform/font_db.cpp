#include "platform/font_db.hpp"

#include <fontconfig/fontconfig.h>

#include <cmath>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

// fontconfig-backed font resolution (docs/type-tool.md §4.2). fontconfig already implements the
// hard parts -- family/weight/slant/width matching, the system fallback cascade by Unicode
// coverage, and colour-font tagging -- so this is a thin, allocation-careful adapter to the
// abstract core::text::FontProvider. We never bake in a family name: even "the default" is a
// `sans-serif` query whose match is whatever the host configured.
namespace mosaic::platform {

using core::text::FontFace;
using core::text::FontRef;

namespace {

const FcChar8* fc(const char* s) { return reinterpret_cast<const FcChar8*>(s); }

// Pull a file+index FontFace out of a matched pattern (the file is required; index defaults to 0).
std::optional<FontFace> faceFromPattern(FcPattern* p) {
    FcChar8* file = nullptr;
    if (FcPatternGetString(p, FC_FILE, 0, &file) != FcResultMatch || !file) return std::nullopt;
    FontFace f;
    f.path = reinterpret_cast<const char*>(file);
    int index = 0;
    FcPatternGetInteger(p, FC_INDEX, 0, &index);  // absent => 0 (FreeType wants the raw FC index,
    f.index = index;                              // whose upper bits already encode the instance)
    return f;
}

// Apply the family-independent style hints (weight/slant/width) shared by resolve() & fallback.
void addStyleHints(FcPattern* pat, const FontRef& ref) {
    FcPatternAddInteger(pat, FC_WEIGHT,
                        FcWeightFromOpenType(static_cast<int>(std::lround(ref.weight))));
    FcPatternAddInteger(pat, FC_SLANT, ref.italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcPatternAddInteger(pat, FC_WIDTH, static_cast<int>(std::lround(ref.widthAxis)));
}

// A VARIABLE face is one design space in one file, so the weight/width the request asked for must
// travel as axis coordinates too -- a static family answers weight 700 with its real Bold file,
// but a variable match returns the same file for every weight and would otherwise always render
// its default instance (the shaper applies FontFace::variations to the face; R4 §3.4). Explicit
// per-axis settings in FontRef::variations win over the style-derived pair (map::emplace).
void foldVariableAxes(FcPattern* match, const FontRef& ref, FontFace& face) {
    FcBool variable = FcFalse;
    if (FcPatternGetBool(match, FC_VARIABLE, 0, &variable) != FcResultMatch || variable != FcTrue)
        return;
    face.variations.emplace("wght", ref.weight);
    face.variations.emplace("wdth", ref.widthAxis);
}

// Run the standard config+default substitutions, then match. Caller owns the returned pattern.
FcPattern* matchOf(FcConfig* cfg, FcPattern* pat) {
    FcConfigSubstitute(cfg, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res = FcResultNoMatch;
    return FcFontMatch(cfg, pat, &res);
}

// Family names listed by FcFontList over `query`, sorted + de-duplicated.
std::vector<std::string> listFamilies(FcConfig* cfg, FcPattern* query) {
    FcObjectSet* os = FcObjectSetBuild(FC_FAMILY, nullptr);
    FcFontSet* set = FcFontList(cfg, query, os);
    std::set<std::string> names;
    if (set) {
        for (int i = 0; i < set->nfont; ++i) {
            FcChar8* f = nullptr;
            if (FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &f) == FcResultMatch && f) {
                names.insert(reinterpret_cast<const char*>(f));
            }
        }
        FcFontSetDestroy(set);
    }
    FcObjectSetDestroy(os);
    return {names.begin(), names.end()};
}

// Emoji-ish codepoints for the preferred-emoji-family gate (R5) -- a pragmatic range set,
// consistent with tokenize.cpp's pictograph classification.
bool isEmojiCodepoint(char32_t cp) {
    return (cp >= 0x2600 && cp <= 0x27BF) ||   // miscellaneous symbols + dingbats
           (cp >= 0x2B00 && cp <= 0x2BFF) ||   // arrows/stars block (⭐ etc.)
           (cp >= 0x1F000 && cp <= 0x1FAFF);   // emoji & pictographic supplements
}

// Match `family` constrained to cover `cp`, verifying the match really IS that family and really
// covers the codepoint -- fontconfig always answers with its closest face, so both must be checked
// or the preference would silently substitute (which is exactly the normal cascade's job).
std::optional<FontFace> matchFamilyCovering(FcConfig* cfg, const std::string& family, char32_t cp,
                                            const FontRef& styleHints) {
    FcCharSet* cs = FcCharSetCreate();
    FcCharSetAddChar(cs, static_cast<FcChar32>(cp));
    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, fc(family.c_str()));
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    addStyleHints(pat, styleHints);
    std::optional<FontFace> face;
    if (FcPattern* match = matchOf(cfg, pat)) {
        FcChar8* fam = nullptr;
        FcCharSet* mcs = nullptr;
        const bool sameFamily =
            FcPatternGetString(match, FC_FAMILY, 0, &fam) == FcResultMatch && fam != nullptr &&
            FcStrCmpIgnoreCase(fam, fc(family.c_str())) == 0;
        const bool covers =
            FcPatternGetCharSet(match, FC_CHARSET, 0, &mcs) == FcResultMatch && mcs != nullptr &&
            FcCharSetHasChar(mcs, static_cast<FcChar32>(cp)) == FcTrue;
        if (sameFamily && covers) face = faceFromPattern(match);
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    FcCharSetDestroy(cs);
    return face;
}

}  // namespace

// A FontRef flattened into a map key: every field that changes what fontconfig would answer.
std::string refKey(const FontRef& ref) {
    std::string k = ref.family;
    k += '\x1f';
    k += std::to_string(ref.weight);
    k += ref.italic ? "|i|" : "|r|";
    k += std::to_string(ref.widthAxis);
    for (const auto& [tag, v] : ref.variations) {  // std::map: already in stable order
        k += '\x1f';
        k += tag;
        k += '=';
        k += std::to_string(v);
    }
    return k;
}

struct FontDB::Impl {
    FcConfig* cfg = nullptr;
    bool owned = false;
    std::string emojiFamily;  // Settings-preferred emoji family ("" = automatic) -- R5

    // ⚠ THE MEMOIZATION, and it is load-bearing, not tidiness. Shaping consults resolve() PER RUN
    // PER LAYOUT and fallbackFor() per uncovered codepoint -- and an FcFontMatch costs
    // milliseconds. Uncached, every keystroke, every bend-handle tick and every font-hover paid
    // fontconfig all over again ("bending text is extremely laggy", user 2026-07-14). The answers
    // are stable for the process' lifetime (this FontDB owns a config snapshot loaded at
    // construction); only setPreferredEmojiFamily changes fallback answers, and it drops that
    // cache. A miss is cached too (nullopt): an unresolvable family must not re-run the match per
    // layout. Guarded by a mutex: cheap when uncontended, and honest if a worker ever shapes.
    mutable std::mutex cacheMutex;
    mutable std::unordered_map<std::string, std::optional<FontFace>> resolveCache;
    mutable std::unordered_map<std::string, std::optional<FontFace>> fallbackCache;
    mutable std::uint64_t fcMatchCount = 0;  // EVENTS, never reset: see FontDB::fcMatches()
};

void FontDB::setPreferredEmojiFamily(std::string family) {
    if (family != m_impl->emojiFamily) {
        const std::scoped_lock lock(m_impl->cacheMutex);
        m_impl->fallbackCache.clear();  // the preferred family changes fallback answers
    }
    m_impl->emojiFamily = std::move(family);
}

std::uint64_t FontDB::fcMatches() const noexcept {
    const std::scoped_lock lock(m_impl->cacheMutex);
    return m_impl->fcMatchCount;
}

FontDB::FontDB() : m_impl(std::make_unique<Impl>()) {
    // Load an owned config (does NOT become the process "current" config, so we destroy it).
    m_impl->cfg = FcInitLoadConfigAndFonts();
    m_impl->owned = m_impl->cfg != nullptr;
    if (!m_impl->cfg) {  // last resort: share the current config (and do not destroy it)
        FcInit();
        m_impl->cfg = FcConfigGetCurrent();
        m_impl->owned = false;
    }
}

FontDB::~FontDB() {
    if (m_impl->owned && m_impl->cfg) FcConfigDestroy(m_impl->cfg);
    // Deliberately no FcFini(): other FontDBs / the rest of the process may still use fontconfig.
}

std::optional<FontFace> FontDB::resolve(const FontRef& ref) const {
    if (!m_impl->cfg) return std::nullopt;
    const std::string key = refKey(ref);
    {
        const std::scoped_lock lock(m_impl->cacheMutex);
        if (const auto it = m_impl->resolveCache.find(key); it != m_impl->resolveCache.end())
            return it->second;  // hit OR remembered miss: fontconfig is not consulted again
    }
    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, fc(ref.family.c_str()));
    addStyleHints(pat, ref);
    std::optional<FontFace> face;
    if (FcPattern* match = matchOf(m_impl->cfg, pat)) {
        face = faceFromPattern(match);
        if (face) {
            face->variations = ref.variations;  // explicit variable axes pass through
            foldVariableAxes(match, ref, *face);
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    {
        const std::scoped_lock lock(m_impl->cacheMutex);
        ++m_impl->fcMatchCount;
        m_impl->resolveCache.emplace(key, face);
    }
    return face;
}

std::optional<FontFace> FontDB::fallbackFor(char32_t codepoint, const FontRef& base) const {
    if (!m_impl->cfg) return std::nullopt;
    // Memoized like resolve(), keyed by codepoint + the base ref: a block with fallback glyphs
    // (CJK, emoji) used to pay one FcFontMatch PER SUCH CODEPOINT PER LAYOUT.
    const std::string key = std::to_string(static_cast<std::uint32_t>(codepoint)) + '\x1e' +
                            refKey(base);
    {
        const std::scoped_lock lock(m_impl->cacheMutex);
        if (const auto it = m_impl->fallbackCache.find(key); it != m_impl->fallbackCache.end())
            return it->second;
    }
    const auto remember = [&](std::optional<FontFace> face) {
        const std::scoped_lock lock(m_impl->cacheMutex);
        ++m_impl->fcMatchCount;
        m_impl->fallbackCache.emplace(key, face);
        return face;
    };
    // The Settings-preferred emoji family wins for emoji codepoints it actually covers (R5).
    // (No variable-axis folding: colour-emoji faces carry no meaningful wght/wdth design space.)
    if (!m_impl->emojiFamily.empty() && isEmojiCodepoint(codepoint)) {
        if (auto face = matchFamilyCovering(m_impl->cfg, m_impl->emojiFamily, codepoint, base))
            return remember(face);
    }
    FcCharSet* cs = FcCharSetCreate();
    FcCharSetAddChar(cs, static_cast<FcChar32>(codepoint));
    FcPattern* pat = FcPatternCreate();
    FcPatternAddCharSet(pat, FC_CHARSET, cs);  // "give me a face that covers this codepoint"
    addStyleHints(pat, base);
    std::optional<FontFace> face;
    if (FcPattern* match = matchOf(m_impl->cfg, pat)) {
        face = faceFromPattern(match);
        if (face) {
            face->variations = base.variations;
            foldVariableAxes(match, base, *face);
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    FcCharSetDestroy(cs);
    return remember(face);
}

std::string FontDB::defaultFamily() const {
    if (!m_impl->cfg) return {};
    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, fc("sans-serif"));  // resolve the generic alias, not a name
    std::string fam;
    if (FcPattern* match = matchOf(m_impl->cfg, pat)) {
        FcChar8* f = nullptr;
        if (FcPatternGetString(match, FC_FAMILY, 0, &f) == FcResultMatch && f) {
            fam = reinterpret_cast<const char*>(f);
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return fam;
}

std::vector<std::string> FontDB::families() const {
    if (!m_impl->cfg) return {};
    FcPattern* query = FcPatternCreate();  // empty query == every installed font
    std::vector<std::string> out = listFamilies(m_impl->cfg, query);
    FcPatternDestroy(query);
    return out;
}

std::vector<std::string> FontDB::emojiFamilies() const {
    if (!m_impl->cfg) return {};
    FcPattern* query = FcPatternCreate();
    FcPatternAddBool(query, FC_COLOR, FcTrue);  // only colour (COLR/CPAL or bitmap) faces
    std::vector<std::string> out = listFamilies(m_impl->cfg, query);
    FcPatternDestroy(query);
    return out;
}

std::string FontDB::sampleTextFor(const std::string& family) const {
    // Pick a preview string that PRINTS in `family`: Latin-capable faces (incl. Latin+Cyrillic etc.)
    // keep the canonical "Abg"; a dedicated non-Latin face gets a sample in the first script it
    // covers; a colour/emoji face gets emoji. Coverage comes from the matched pattern's FC_CHARSET /
    // FC_COLOR -- no need to open the face. (Samples are literal UTF-8; the file is built as UTF-8.)
    if (!m_impl->cfg) return "Abg";
    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, fc(family.c_str()));
    std::string sample = "Abg";
    if (FcPattern* match = matchOf(m_impl->cfg, pat)) {
        FcBool color = FcFalse;
        FcCharSet* cs = nullptr;
        if (FcPatternGetBool(match, FC_COLOR, 0, &color) == FcResultMatch && color == FcTrue) {
            sample = "\U0001F600\U0001F3A8\U0001F381";  // emoji: grin / palette / gift
        } else if (FcPatternGetCharSet(match, FC_CHARSET, 0, &cs) == FcResultMatch && cs != nullptr) {
            const auto has = [cs](char32_t c) {
                return FcCharSetHasChar(cs, static_cast<FcChar32>(c)) == FcTrue;
            };
            // CJK is checked BEFORE Latin: CJK fonts carry Latin too, but their identity is the Han
            // glyphs, and no Latin/European face covers U+6C38 -- so Han coverage means "a CJK font",
            // which should preview in Han (the user's "asian languages" case), not as "Abg".
            if (has(U'永')) {  // CJK Han  永安
                sample = "永安";
            } else if (has(U'A') && has(U'a') && has(U'g')) {
                sample = "Abg";  // Latin (or Latin + a European script) -- the common case
            } else {
                // First covered script wins (probe char -> sample). Ordered most-common first; the
                // Hangul/Kana entries only catch Han-less Korean/Japanese display faces (CJK fonts
                // already matched above).
                struct Script {
                    char32_t probe;
                    const char* text;
                };
                static const Script kScripts[] = {
                    {U'Б', "Абв"},         // Cyrillic   Абв
                    {U'Α', "Αβγ"},         // Greek      Αβγ
                    {U'가', "가나다"},         // Hangul     가나다
                    {U'あ', "あいう"},         // Hiragana   あいう
                    {U'ا', "أبج"},         // Arabic     أبج
                    {U'א', "אבג"},         // Hebrew     אבג
                    {U'अ', "अआइ"},         // Devanagari अआइ
                    {U'ก', "กขค"},         // Thai       กขค
                    {U'Ա', "Աբգ"},         // Armenian   Աբգ
                    {U'ა', "აბგ"},         // Georgian   აბგ
                };
                for (const Script& s : kScripts) {
                    if (has(s.probe)) {
                        sample = s.text;
                        break;
                    }
                }
            }
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return sample;
}

}  // namespace mosaic::platform
