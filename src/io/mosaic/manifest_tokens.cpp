#include "io/mosaic/manifest_tokens.hpp"

#include <array>
#include <utility>

// Deliberately dependency-free beyond the enums themselves -- see the header for why this is its
// own translation unit. Nothing may be added here that references a layer kind.
namespace mosaic::io::native::detail {
namespace {

template <typename E, std::size_t N>
const char* tokenOf(const std::array<std::pair<E, const char*>, N>& table, E v) {
    for (const auto& [e, s] : table)
        if (e == v)
            return s;
    return table[0].second; // unreachable for in-range enums; a safe spelling regardless
}

template <typename E, std::size_t N>
std::optional<E> enumOf(const std::array<std::pair<E, const char*>, N>& table,
                        const std::string& s) {
    for (const auto& [e, t] : table)
        if (s == t)
            return e;
    return std::nullopt;
}

constexpr std::array<std::pair<core::ColorSpace, const char*>, 5> kColorSpaceTokens{{
    {core::ColorSpace::SRGB, "srgb"},
    {core::ColorSpace::LinearSRGB, "linear_srgb"},
    {core::ColorSpace::DisplayP3, "display_p3"},
    {core::ColorSpace::AdobeRGB, "adobe_rgb"},
    {core::ColorSpace::Rec2020, "rec2020"},
}};

constexpr std::array<std::pair<core::Precision, const char*>, 4> kPrecisionTokens{{
    {core::Precision::U8, "u8"},
    {core::Precision::U16, "u16"},
    {core::Precision::F16, "f16"},
    {core::Precision::F32, "f32"},
}};

} // namespace

const char* colorSpaceToken(core::ColorSpace cs) {
    return tokenOf(kColorSpaceTokens, cs);
}

std::optional<core::ColorSpace> colorSpaceFromToken(const std::string& s) {
    return enumOf(kColorSpaceTokens, s);
}

const char* precisionToken(core::Precision p) {
    return tokenOf(kPrecisionTokens, p);
}

std::optional<core::Precision> precisionFromToken(const std::string& s) {
    return enumOf(kPrecisionTokens, s);
}

} // namespace mosaic::io::native::detail
