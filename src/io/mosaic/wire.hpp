#pragma once

#include <cstdint>

// mosaic/wire -- explicit little-endian stores/loads shared by the framing (chunk.cpp) and
// container (file.cpp) layers. The on-disk layout must not depend on host endianness.
namespace mosaic::io::native::detail {

inline void storeLe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

inline void storeLe64(std::uint8_t* p, std::uint64_t v) noexcept {
    storeLe32(p, static_cast<std::uint32_t>(v));
    storeLe32(p + 4, static_cast<std::uint32_t>(v >> 32));
}

[[nodiscard]] inline std::uint32_t loadLe32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]] inline std::uint64_t loadLe64(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(loadLe32(p)) |
           (static_cast<std::uint64_t>(loadLe32(p + 4)) << 32);
}

} // namespace mosaic::io::native::detail
