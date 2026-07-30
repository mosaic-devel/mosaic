#pragma once

#include "io/mosaic/format.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

// mosaic/naming -- the textual spellings of chunk identities used inside the format's own JSON
// payloads (DIR entries, HIST dirty lists, JHDR bindings). Shared by the container (file.cpp)
// and the write/recovery paths (save.cpp, journal.cpp, salvage.cpp); one implementation, so the
// spellings cannot drift apart between writer and reader.
namespace mosaic::io::native::detail {

[[nodiscard]] inline std::string tagToString(const ChunkTag& t) {
    return std::string(reinterpret_cast<const char*>(t.data()), t.size());
}

[[nodiscard]] inline std::optional<ChunkTag> tagFromString(const std::string& s) {
    if (s.size() != 4)
        return std::nullopt;
    ChunkTag t;
    std::copy(s.begin(), s.end(), reinterpret_cast<char*>(t.data()));
    return t;
}

[[nodiscard]] inline std::string bytesToHex(std::span<const std::uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2] = kHex[bytes[i] >> 4];
        out[i * 2 + 1] = kHex[bytes[i] & 0xF];
    }
    return out;
}

[[nodiscard]] inline int hexNibble(char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

[[nodiscard]] inline std::string keyToHex(const ChunkKey& k) {
    return bytesToHex(k.bytes);
}

[[nodiscard]] inline std::optional<ChunkKey> keyFromHex(const std::string& s) {
    if (s.size() != 32)
        return std::nullopt;
    ChunkKey k;
    for (std::size_t i = 0; i < 16; ++i) {
        const int hi = hexNibble(s[i * 2]);
        const int lo = hexNibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        k.bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return k;
}

} // namespace mosaic::io::native::detail
