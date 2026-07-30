#include "core/text/tokenize.hpp"

namespace mosaic::core::text {
namespace {

// ---------------------------------------------------------------------------------------------
// UTF-8 decode (advancing). Malformed bytes decode as U+FFFD and advance one byte, so arbitrary
// model text tokenizes without ever stalling. (A file-local twin of shaping.cpp's decoder; the two
// stay small and independent rather than dragging in a shared header just for this.)
// ---------------------------------------------------------------------------------------------
char32_t decodeUtf8(std::string_view s, std::size_t& i) {
    const std::size_t n = s.size();
    const auto c = static_cast<unsigned char>(s[i]);
    const auto cont = [&](std::size_t k) {
        return k < n && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80;
    };
    if (c < 0x80) { ++i; return c; }
    if ((c & 0xE0) == 0xC0 && cont(i + 1)) {
        const char32_t cp = (char32_t(c & 0x1F) << 6) | (s[i + 1] & 0x3F);
        i += 2; return cp;
    }
    if ((c & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2)) {
        const char32_t cp = (char32_t(c & 0x0F) << 12) | (char32_t(s[i + 1] & 0x3F) << 6) |
                            (s[i + 2] & 0x3F);
        i += 3; return cp;
    }
    if ((c & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
        const char32_t cp = (char32_t(c & 0x07) << 18) | (char32_t(s[i + 1] & 0x3F) << 12) |
                            (char32_t(s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
        i += 4; return cp;
    }
    ++i; return 0xFFFD;
}

// ---------------------------------------------------------------------------------------------
// Codepoint classification (UAX #29-inspired, common-script coverage; see the header).
// ---------------------------------------------------------------------------------------------
enum class Cp {
    Letter,   // alphabetic; part of a word
    Digit,    // decimal digit 0-9; part of a word
    Mid,      // an apostrophe that joins two letters (don't, l'ami)
    Extend,   // combining mark / joiner; continues the current word without changing it
    Cjk,      // ideographic / kana / hangul: a word boundary on both sides
    Space,    // whitespace (chunk separator)
    Other,    // punctuation, symbols, emoji: a word boundary
};

bool inRange(char32_t c, char32_t lo, char32_t hi) { return c >= lo && c <= hi; }

// Alphabetic across the scripts we cover. Latin ranges are complete enough for the bundled
// en/de/fr/es/it (and most European) dictionaries; the rest are the common non-Latin scripts.
bool isLetter(char32_t c) {
    if (inRange(c, 'A', 'Z') || inRange(c, 'a', 'z')) return true;
    if (c < 0x80) return false;
    // Latin-1 letters: À..ÿ minus × (0xD7) and ÷ (0xF7); plus ª µ º.
    if (c == 0xAA || c == 0xB5 || c == 0xBA) return true;
    if (inRange(c, 0xC0, 0xFF) && c != 0xD7 && c != 0xF7) return true;
    if (inRange(c, 0x100, 0x24F)) return true;             // Latin Extended-A/B
    if (inRange(c, 0x386, 0x3FF) && c != 0x387) return true;  // Greek (approx; 0x387 is ano teleia)
    if (inRange(c, 0x400, 0x4FF)) return true;             // Cyrillic
    if (inRange(c, 0x531, 0x58A)) return true;             // Armenian
    if (inRange(c, 0x5D0, 0x5EA) || inRange(c, 0x5EF, 0x5F2)) return true;  // Hebrew
    if (inRange(c, 0x620, 0x64A) || inRange(c, 0x66E, 0x6D3)) return true;  // Arabic
    if (inRange(c, 0x904, 0x939) || inRange(c, 0x958, 0x961)) return true;  // Devanagari
    return false;
}

// Uppercase within the ranges that have case (for the all-caps acronym flag). Only Latin/Greek/
// Cyrillic have a cased notion here; other scripts return false (they never mark allCaps).
bool isUpper(char32_t c) {
    if (inRange(c, 'A', 'Z')) return true;
    if (inRange(c, 0xC0, 0xDE) && c != 0xD7) return true;         // Latin-1 upper
    if (inRange(c, 0x391, 0x3A9)) return true;                    // Greek upper
    if (inRange(c, 0x400, 0x42F)) return true;                    // Cyrillic upper
    return false;
}
bool isLower(char32_t c) {
    if (inRange(c, 'a', 'z')) return true;
    if (c == 0xDF || (inRange(c, 0xE0, 0xFF) && c != 0xF7)) return true;  // Latin-1 lower
    if (inRange(c, 0x3B1, 0x3C9)) return true;                   // Greek lower
    if (inRange(c, 0x430, 0x44F)) return true;                   // Cyrillic lower
    return false;
}

bool isCjk(char32_t c) {
    return inRange(c, 0x1100, 0x11FF) ||  // Hangul Jamo
           inRange(c, 0x3040, 0x30FF) ||  // Hiragana + Katakana
           inRange(c, 0x31F0, 0x31FF) ||  // Katakana phonetic extensions
           inRange(c, 0x3400, 0x4DBF) ||  // CJK Ext-A
           inRange(c, 0x4E00, 0x9FFF) ||  // CJK Unified Ideographs
           inRange(c, 0xAC00, 0xD7A3) ||  // Hangul syllables
           inRange(c, 0xF900, 0xFAFF) ||  // CJK compatibility ideographs
           inRange(c, 0x20000, 0x2FA1F);  // CJK Ext-B..F + compat supplement
}

bool isSpace(char32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C ||
           c == 0xA0 ||                     // no-break space
           inRange(c, 0x2000, 0x200A) ||    // en/em/thin spaces
           c == 0x2028 || c == 0x2029 ||    // line / paragraph separators
           c == 0x202F || c == 0x205F ||    // narrow / medium math space
           c == 0x3000;                     // ideographic space
}

bool isExtend(char32_t c) {
    return inRange(c, 0x300, 0x36F) ||   // combining diacritical marks
           inRange(c, 0x483, 0x489) ||   // Cyrillic combining
           inRange(c, 0x591, 0x5BD) ||   // Hebrew points
           c == 0x5BF || c == 0x5C1 || c == 0x5C2 ||
           inRange(c, 0x64B, 0x65F) || c == 0x670 ||  // Arabic marks
           inRange(c, 0x6D6, 0x6DC) ||
           inRange(c, 0x900, 0x903) || inRange(c, 0x93A, 0x94F) ||  // Devanagari marks
           inRange(c, 0x951, 0x957) || inRange(c, 0x962, 0x963) ||
           c == 0x200C || c == 0x200D;   // ZWNJ / ZWJ
}

Cp classify(char32_t c) {
    if (isSpace(c)) return Cp::Space;
    if (c == U'\'' || c == 0x2019 /* ’ */ || c == 0x02BC /* ʼ */) return Cp::Mid;
    if (inRange(c, '0', '9')) return Cp::Digit;
    if (isLetter(c)) return Cp::Letter;
    if (isCjk(c)) return Cp::Cjk;
    if (isExtend(c)) return Cp::Extend;
    return Cp::Other;
}

// A whitespace-delimited chunk [begin,end) is a URL or e-mail if it carries a scheme/host marker.
// Conservative on purpose (only "://", a leading "www.", or an '@' between word chars), so ordinary
// prose punctuation ("end.Start", "a,b") is never mistaken for one.
bool looksLikeUrlOrEmail(std::string_view chunk) {
    if (chunk.find("://") != std::string_view::npos) return true;
    if (chunk.size() >= 4 && (chunk.substr(0, 4) == "www." || chunk.substr(0, 4) == "WWW."))
        return true;
    const std::size_t at = chunk.find('@');
    if (at != std::string_view::npos && at > 0 && at + 1 < chunk.size()) {
        // letters on both immediate sides -> an e-mail local@domain, not a stray '@'.
        std::size_t p = at - 1;
        const bool before = classify(static_cast<unsigned char>(chunk[p])) == Cp::Letter ||
                            classify(static_cast<unsigned char>(chunk[p])) == Cp::Digit;
        std::size_t q = at + 1;
        const char32_t after = decodeUtf8(chunk, q);
        const Cp ac = classify(after);
        if (before && (ac == Cp::Letter || ac == Cp::Digit)) return true;
    }
    return false;
}

}  // namespace

std::vector<WordToken> tokenizeWords(std::string_view utf8, const TokenizeOptions& opts) {
    std::vector<WordToken> out;
    const std::size_t n = utf8.size();

    std::size_t i = 0;
    while (i < n) {
        // Skip whitespace to the start of the next chunk.
        {
            std::size_t j = i;
            const char32_t cp = decodeUtf8(utf8, j);
            if (classify(cp) == Cp::Space) { i = j; continue; }
        }

        // Delimit the whitespace-free chunk [chunkBegin, chunkEnd).
        const std::size_t chunkBegin = i;
        std::size_t chunkEnd = i;
        while (chunkEnd < n) {
            std::size_t j = chunkEnd;
            const char32_t cp = decodeUtf8(utf8, j);
            if (classify(cp) == Cp::Space) break;
            chunkEnd = j;
        }

        const std::string_view chunk = utf8.substr(chunkBegin, chunkEnd - chunkBegin);
        if (opts.skipUrlsAndEmails && looksLikeUrlOrEmail(chunk)) {
            i = chunkEnd;
            continue;
        }

        // Tokenize within the chunk.
        std::size_t p = chunkBegin;
        while (p < chunkEnd) {
            std::size_t q = p;
            const char32_t cp = decodeUtf8(utf8, q);
            const Cp cls = classify(cp);

            if (cls == Cp::Cjk) {
                if (opts.cjk) out.push_back(WordToken{p, q, false, false, false, true});
                p = q;
                continue;
            }
            if (cls != Cp::Letter && cls != Cp::Digit) {  // Mid/Extend/Other with no word open
                p = q;
                continue;
            }

            // Grow a word: Letter/Digit, with a single interior apostrophe between two of them, and
            // combining marks absorbed. Track flags as we go.
            WordToken tok;
            tok.begin = p;
            std::size_t cur = p;
            bool sawCased = false;
            bool allUpper = true;
            char32_t prevCp = 0;
            while (cur < chunkEnd) {
                std::size_t next = cur;
                const char32_t c = decodeUtf8(utf8, next);
                const Cp cc = classify(c);
                if (cc == Cp::Letter || cc == Cp::Digit) {
                    if (cc == Cp::Letter) {
                        tok.hasLetter = true;
                        if (isUpper(c)) { sawCased = true; }
                        else if (isLower(c)) { sawCased = true; allUpper = false; }
                    } else {
                        tok.hasDigit = true;
                    }
                    prevCp = c;
                    cur = next;
                    continue;
                }
                if (cc == Cp::Extend) { cur = next; continue; }  // absorb marks, keep flags
                if (cc == Cp::Mid) {
                    // Join only if flanked by Letter/Digit on both sides; else it ends the word.
                    std::size_t after = next;
                    const char32_t nextCp = (next < chunkEnd) ? decodeUtf8(utf8, after) : 0;
                    const Cp nc = classify(nextCp);
                    const Cp pc = classify(prevCp);
                    const bool joinL = (pc == Cp::Letter || pc == Cp::Digit);
                    const bool joinR = (nc == Cp::Letter || nc == Cp::Digit);
                    if (joinL && joinR) { prevCp = c; cur = next; continue; }
                }
                break;  // Cjk / Other / unjoined Mid ends the word
            }
            tok.end = cur;
            tok.allCaps = sawCased && allUpper;

            const bool pureNumber = tok.hasDigit && !tok.hasLetter;
            if (pureNumber ? opts.numbers : true) out.push_back(tok);
            p = cur;
        }

        i = chunkEnd;
    }
    return out;
}

bool isVerticalUpright(char32_t cp) {
    // Ideographs, kana, and hangul are always upright (the tokenizer's own CJK set).
    if (isCjk(cp)) return true;
    // CJK punctuation / symbols and the compatibility & vertical presentation forms sit upright too.
    if (inRange(cp, 0x3000, 0x303F)) return true;   // CJK symbols and punctuation (incl. 、。「」)
    if (inRange(cp, 0x31C0, 0x31EF)) return true;   // CJK strokes
    if (inRange(cp, 0x3200, 0x33FF)) return true;   // enclosed CJK letters/months + compatibility
    if (inRange(cp, 0xFE30, 0xFE4F)) return true;   // CJK compatibility forms (vertical variants)
    if (inRange(cp, 0xFF01, 0xFF60)) return true;   // fullwidth ASCII forms
    if (inRange(cp, 0xFFE0, 0xFFE6)) return true;   // fullwidth signs (￡￥ etc.)
    // Emoji / pictographs stay upright (and, being colour glyphs, are never outline-rotated anyway).
    if (inRange(cp, 0x2600, 0x27BF)) return true;   // miscellaneous symbols + dingbats
    if (inRange(cp, 0x1F000, 0x1FAFF)) return true; // emoji & pictographic supplements
    return false;
}

}  // namespace mosaic::core::text
