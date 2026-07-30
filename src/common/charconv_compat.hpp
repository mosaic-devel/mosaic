#pragma once

// std::from_chars for FLOATING POINT is not implemented in libc++ (the macOS/osxcross toolchain,
// S58) -- only the integral overloads exist there. libstdc++ (Linux) has both. This wrapper keeps
// one call shape across both: on libstdc++ it is exactly std::from_chars (bit-identical behaviour),
// on libc++ it falls back to a locale-correct strtod. Callers pass '.'-decimal input (they have
// already normalised any ',' separator), matching from_chars's locale independence.
//
// gToString() is the WRITE side of the same contract, and it exists because the asymmetry bites:
// i18n::init() moves LC_NUMERIC to the user's locale, so `snprintf("%g")` emits "1234,5" across
// most of Europe while fromChars() only ever accepts '.'. Anything that round-trips through a
// stored token must use both halves of this header, not printf. (Found in S54: custom-size
// recents were written with %g and read with fromChars, so on a comma locale every saved size
// silently failed to parse back.)

#include <charconv>
#include <cerrno>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>

namespace mosaic::common {

#if defined(_LIBCPP_VERSION)
inline std::from_chars_result fromChars(const char* first, const char* last, double& value) {
    std::string s(first, last);
    // strtod honours LC_NUMERIC; the input is '.'-decimal, so if the active locale's point differs,
    // rewrite it (the same guard core/brush/parse_util.hpp uses for its own strtod read).
    const std::lconv* lc = std::localeconv();
    const char* dp = (lc != nullptr && lc->decimal_point != nullptr && lc->decimal_point[0] != '\0')
                         ? lc->decimal_point
                         : ".";
    if (dp[0] != '.') {
        if (const std::size_t pos = s.find('.'); pos != std::string::npos)
            s.replace(pos, 1, dp);
    }
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    std::from_chars_result r{};
    r.ptr = first + (end - s.c_str());
    if (end == s.c_str())
        r.ec = std::errc::invalid_argument;
    else if (errno == ERANGE)
        r.ec = std::errc::result_out_of_range;
    else {
        r.ec = std::errc{};
        value = v;
    }
    return r;
}
#else
inline std::from_chars_result fromChars(const char* first, const char* last, double& value) {
    return std::from_chars(first, last, value);
}
#endif

// Format `value` like printf's "%.<significant>g" -- shortest general form, trailing zeros
// trimmed -- but ALWAYS with '.' as the decimal point, whatever LC_NUMERIC says. This is the
// string form fromChars() reads back, so it is what any persisted token must be written with.
inline std::string gToString(double value, int significant = 10) {
#if defined(_LIBCPP_VERSION)
    // libc++ has no floating-point std::to_chars either; go through snprintf and undo the locale.
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "%.*g", significant, value);
    std::string s(buf, n > 0 ? static_cast<std::size_t>(n) : 0u);
    const std::lconv* lc = std::localeconv();
    const char* dp = (lc != nullptr && lc->decimal_point != nullptr && lc->decimal_point[0] != '\0')
                         ? lc->decimal_point
                         : ".";
    if (dp[0] != '.') {
        if (const std::size_t pos = s.find(dp); pos != std::string::npos)
            s.replace(pos, std::char_traits<char>::length(dp), ".");
    }
    return s;
#else
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, value, std::chars_format::general,
                                 significant);
    if (r.ec != std::errc{})
        return "0";  // cannot happen for a finite double in 64 bytes; never emit garbage
    return std::string(buf, r.ptr);
#endif
}

} // namespace mosaic::common
