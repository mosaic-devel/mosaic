#pragma once

#include "core/brush/parse_util.hpp"

#include <pugixml.hpp>

#include <algorithm>

// pugixml attribute readers with parse_util's strictness -- shared by the sensor-fragment parser
// (core/brush/sensors.cpp) and the preset/tip XML layer (io/brush/preset_xml.cpp), because the
// garbage-handling rule must not drift between them:
//
// pugixml's as_int/as_double follow strtol/strtod, which map "abc" to 0 rather than reporting
// failure -- so an attribute of garbage would read as a very deliberate-looking zero. The strict
// parsers in parse_util.hpp demand the whole token; the caller's documented default stands
// otherwise. (And parseDouble owns the LC_NUMERIC translation, so these are safe under any locale.)
//
// Header-only and internal, like parse_util.hpp; pugixml stays out of every public header.
namespace mosaic::core::brush::detail {

[[nodiscard]] inline bool boolAttribute(pugi::xml_attribute a, bool fallback) {
    if (!a)
        return fallback;
    bool out = false;
    return parseBool(a.value(), out) ? out : fallback;
}

[[nodiscard]] inline int intAttribute(pugi::xml_attribute a, int fallback, int lo, int hi) {
    if (!a)
        return fallback;
    long long v = 0;
    if (!parseLongLong(a.value(), v))
        return fallback;
    return static_cast<int>(std::clamp<long long>(v, lo, hi));
}

[[nodiscard]] inline double doubleAttribute(pugi::xml_attribute a, double fallback) {
    if (!a)
        return fallback;
    double v = 0.0;
    return parseDouble(a.value(), v) ? v : fallback;
}

} // namespace mosaic::core::brush::detail
