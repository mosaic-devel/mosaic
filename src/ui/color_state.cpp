#include "ui/color_state.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace mosaic::ui {

void ColorState::setForeground(common::Color8 c) {
    if (c == m_fg)
        return;
    m_fg = c;
    notify();
}

void ColorState::setBackground(common::Color8 c) {
    if (c == m_bg)
        return;
    m_bg = c;
    notify();
}

void ColorState::swap() {
    std::swap(m_fg, m_bg);
    notify();
}

void ColorState::reset() {
    m_fg = {0, 0, 0, 255};
    m_bg = {255, 255, 255, 255};
    notify();
}

void ColorState::notify() {
    for (const std::function<void()>& obs : m_observers)
        if (obs)
            obs();
}

namespace {

// Hex digit value, or -1 if not a hex digit.
int hexValue(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::uint8_t pair(int hi, int lo) {
    return static_cast<std::uint8_t>(hi * 16 + lo);
}

} // namespace

std::optional<common::Color8> parseHexColor(std::string_view text) {
    // Trim surrounding whitespace, then an optional single leading '#'.
    std::size_t b = 0;
    std::size_t e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b])) != 0)
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1])) != 0)
        --e;
    std::string_view s = text.substr(b, e - b);
    if (!s.empty() && s.front() == '#')
        s.remove_prefix(1);

    int v[6] = {};
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i >= 6)
            return std::nullopt; // too long
        v[i] = hexValue(s[i]);
        if (v[i] < 0)
            return std::nullopt; // non-hex digit
    }

    if (s.size() == 3) // "#RGB" shorthand: each nibble doubled (F -> FF)
        return common::Color8{pair(v[0], v[0]), pair(v[1], v[1]), pair(v[2], v[2]), 255};
    if (s.size() == 6)
        return common::Color8{pair(v[0], v[1]), pair(v[2], v[3]), pair(v[4], v[5]), 255};
    return std::nullopt;
}

std::string hexString(common::Color8 c) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

} // namespace mosaic::ui
