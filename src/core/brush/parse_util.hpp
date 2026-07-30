#pragma once

#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

// Shared scanning and formatting for the brush preset formats.
//
// The `.kpp` family stores numbers as text in several places -- curve control points, sensor
// attributes -- and the two directions have to agree with each other, so they live together:
//
//   * The PARSERS are strict. `strtod`/`strtol` map "abc" to 0 and report failure only through a
//     pointer the caller must remember to check; every wrapper here demands the whole token was
//     consumed and hands back a bool. A preset is a third-party file, and an attribute of garbage
//     must fall back to its documented default rather than to a very deliberate-looking zero.
//   * The FORMATTER is `%g` at 6 significant digits, matching the producer we read. That is what
//     makes an imported curve round-trip byte-exactly (core/brush/curve.hpp), so the same routine
//     writes every other number in a preset too -- an `angleOffset="30"` must not come back as
//     `angleOffset="30.000000"`.
//
// ⚠ **Both directions must ignore LC_NUMERIC, and neither gets that for free.** Mosaic adopts the
// user's locale at startup (`std::setlocale(LC_ALL, "")`, common/i18n.cpp), and `strtod` and
// `snprintf("%g")` both honour it. The preset format is fixed and always writes '.'. Under any
// comma-decimal locale -- pl_PL, de_DE, fr_FR, most of Europe -- the unguarded versions:
//
//   * silently DROP curve control points, because "0.753769" stops strtod at the '.', fails the
//     full-consumption check, and takes the whole point down with it. A three-knot curve loads as
//     two, and a preset's response quietly flattens.
//   * write "0,753769" back out, where ',' is the curve format's own x/y separator -- so the
//     serialized curve is not merely imprecise, it is a different curve with different arity.
//
// The fix is to translate the separator rather than the locale (thread-safe, no `strtod_l`
// portability spread, no `from_chars` floating-point support to assume). Pinned by
// tests/test_brush_curve.cpp's locale cases, which are the only place this can be caught: a bare
// test binary never calls setlocale, so the bug is invisible outside the real app.
//
// Header-only and internal to core/brush; nothing here is part of the engine's public surface.
namespace mosaic::core::brush::detail {

// The interchange format's whitespace, not the locale's: std::isspace() is LC_CTYPE-dependent, and
// which bytes count as space must not vary by who is running the program.
[[nodiscard]] inline std::string_view trim(std::string_view s) {
    constexpr std::string_view kSpace = " \t\n\v\f\r";
    const std::size_t b = s.find_first_not_of(kSpace);
    if (b == std::string_view::npos)
        return {};
    return s.substr(b, s.find_last_not_of(kSpace) - b + 1);
}

// What LC_NUMERIC currently says a decimal point looks like. "." under C/POSIX and en_*.
[[nodiscard]] inline std::string localeDecimalPoint() {
    const std::lconv* lc = std::localeconv();
    if (lc == nullptr || lc->decimal_point == nullptr || lc->decimal_point[0] == '\0')
        return ".";
    return lc->decimal_point;
}

// std::from_chars for double is not available in every libstdc++ we build against; strtod on a
// NUL-terminated copy is the portable read. False on anything that is not a complete finite number
// -- including "nan" and "inf", which strtod accepts and which would poison whatever consumes them.
[[nodiscard]] inline bool parseDouble(std::string_view tok, double& out) {
    tok = trim(tok);
    if (tok.empty())
        return false;

    std::string s(tok);
    if (const std::string dp = localeDecimalPoint(); dp != ".") {
        // The token is in the format's convention. Reject the locale's separator outright -- a file
        // containing "22,5" is malformed, and must be malformed for every user rather than parsing
        // to 22.5 in Warsaw and to nothing in London. Then rewrite '.' into what strtod expects.
        if (s.find(dp) != std::string::npos)
            return false;
        if (const std::size_t dot = s.find('.'); dot != std::string::npos)
            s.replace(dot, 1, dp);
    }

    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v))
        return false;
    out = v;
    return true;
}

// False on a partial token or on a value too large for `long long` -- the caller's default stands.
// strtoll applies no locale grouping, so base-10 integers need no separator translation.
[[nodiscard]] inline bool parseLongLong(std::string_view tok, long long& out) {
    tok = trim(tok);
    if (tok.empty())
        return false;
    const std::string s(tok);
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size() || errno == ERANGE)
        return false;
    out = v;
    return true;
}

// The producers we read write booleans as 0/1 (their DOM takes a bool through an int overload), but
// the preset properties spell them "true"/"false". Accept both, and treat any nonzero integer as
// true. False when the token is neither, so the caller's documented default stands rather than a
// silent `false`.
//
// One implementation for the whole module: a sensor XML attribute and a preset property carrying the
// same bytes have to mean the same thing, and they were parsed by two hand-copied functions before.
[[nodiscard]] inline bool parseBool(std::string_view tok, bool& out) {
    tok = trim(tok);
    if (tok.empty())
        return false;
    if (long long n = 0; parseLongLong(tok, n)) {
        out = (n != 0);
        return true;
    }
    if (tok == "true" || tok == "TRUE" || tok == "True") {
        out = true;
        return true;
    }
    if (tok == "false" || tok == "FALSE" || tok == "False") {
        out = false;
        return true;
    }
    return false;
}

// Format as the interchange format's producer does: %g with 6 significant digits ("0", "1",
// "0.753769"). Anything else breaks byte-exact round-trip of imported presets.
inline void appendNumber(std::string& out, double v) {
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%g", v);
    if (n <= 0)
        return;

    std::string s(buf, static_cast<std::size_t>(n));
    // snprintf just wrote the user's decimal point. Put the format's back. (%g emits no grouping
    // separators, and an exponent form like "1e+300" has no separator at all.)
    if (const std::string dp = localeDecimalPoint(); dp != ".") {
        if (const std::size_t pos = s.find(dp); pos != std::string::npos)
            s.replace(pos, dp.size(), ".");
    }
    out += s;
}

[[nodiscard]] inline std::string formatNumber(double v) {
    std::string out;
    appendNumber(out, v);
    return out;
}

} // namespace mosaic::core::brush::detail
