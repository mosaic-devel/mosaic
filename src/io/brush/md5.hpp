#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// MD5 (RFC 1321) -- here as a RESOURCE KEY, not a security primitive: the preset format's
// `md5sum` attribute and the bundle manifest both identify a tip file by the MD5 of its bytes,
// and the library's resolution order (md5 index first, filename fallback -- §3.5, the upstream
// bestMatch rule) needs the same digests to reproduce the same matches. Nothing here signs,
// verifies or protects anything.
//
// Implemented from the RFC's algorithm description; the KAT suite pins the RFC's own seven test
// vectors, and the corpus replay cross-checks every digest against the shipped manifests' claims.
namespace mosaic::io::brush {

[[nodiscard]] std::array<std::uint8_t, 16> md5(const std::uint8_t* data, std::size_t size);

// The wire spelling: 32 lowercase hex digits, as every manifest and preset writes it.
[[nodiscard]] std::string md5Hex(const std::uint8_t* data, std::size_t size);

} // namespace mosaic::io::brush
