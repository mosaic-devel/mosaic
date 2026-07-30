#include "io/brush/md5.hpp"

#include <bit>
#include <cstring>

namespace mosaic::io::brush {
namespace {

// The RFC's T table: T[i] = floor(2^32 * |sin(i + 1)|), spelled out because sin() is not
// constexpr-portable and the values are load-bearing constants, not derived data.
constexpr std::uint32_t kT[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
    0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
    0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
    0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
    0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
    0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
    0xeb86d391,
};

// Per-round left-rotation amounts, four rounds of four.
constexpr int kS[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, //
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, //
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, //
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

struct State {
    std::uint32_t a = 0x67452301;
    std::uint32_t b = 0xefcdab89;
    std::uint32_t c = 0x98badcfe;
    std::uint32_t d = 0x10325476;
};

void processBlock(State& s, const std::uint8_t block[64]) {
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = static_cast<std::uint32_t>(block[i * 4]) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
    }

    std::uint32_t a = s.a, b = s.b, c = s.c, d = s.d;
    for (int i = 0; i < 64; ++i) {
        std::uint32_t f = 0;
        int g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        const std::uint32_t rotated = a + f + kT[i] + m[g];
        a = d;
        d = c;
        c = b;
        b += std::rotl(rotated, kS[i]);
    }
    s.a += a;
    s.b += b;
    s.c += c;
    s.d += d;
}

} // namespace

std::array<std::uint8_t, 16> md5(const std::uint8_t* data, std::size_t size) {
    State s;
    std::size_t pos = 0;
    for (; size - pos >= 64; pos += 64)
        processBlock(s, data + pos);

    // Padding: 0x80, zeros, then the ORIGINAL length in bits as a 64-bit little-endian tail.
    // The remainder is under 64 bytes, so padding spans one block or two.
    std::uint8_t tail[128] = {};
    const std::size_t rem = size - pos;
    if (rem != 0)
        std::memcpy(tail, data + pos, rem);
    tail[rem] = 0x80;
    const std::size_t blocks = rem + 1 + 8 > 64 ? 2 : 1;
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8;
    for (int i = 0; i < 8; ++i)
        tail[blocks * 64 - 8 + i] = static_cast<std::uint8_t>(bits >> (i * 8));
    processBlock(s, tail);
    if (blocks == 2)
        processBlock(s, tail + 64);

    std::array<std::uint8_t, 16> out{};
    const std::uint32_t words[4] = {s.a, s.b, s.c, s.d};
    for (int i = 0; i < 16; ++i)
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(words[i / 4] >> ((i % 4) * 8));
    return out;
}

std::string md5Hex(const std::uint8_t* data, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    const std::array<std::uint8_t, 16> digest = md5(data, size);
    std::string out(32, '0');
    for (int i = 0; i < 16; ++i) {
        out[static_cast<std::size_t>(i * 2)] = kHex[digest[static_cast<std::size_t>(i)] >> 4];
        out[static_cast<std::size_t>(i * 2 + 1)] = kHex[digest[static_cast<std::size_t>(i)] & 15];
    }
    return out;
}

} // namespace mosaic::io::brush
