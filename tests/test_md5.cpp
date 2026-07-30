// MD5 (io/brush/md5): the RFC 1321 appendix's own seven vectors, plus the padding boundary
// cases the tail logic hinges on (55/56/64 bytes: the largest one-block remainder, the smallest
// two-block one, and an exact block). The corpus replay re-verifies every digest against the
// shipped bundle manifests, so these KATs only have to pin the algorithm, not the deployment.

#include "io/brush/md5.hpp"

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

using mosaic::io::brush::md5Hex;

namespace {
[[nodiscard]] std::string hexOf(std::string_view s) {
    return md5Hex(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}
} // namespace

TEST_CASE("md5: the RFC 1321 test suite") {
    CHECK(hexOf("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(hexOf("a") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(hexOf("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(hexOf("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(hexOf("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(hexOf("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ==
          "d174ab98d277d9f5a5611c2c9f419d9f");
    CHECK(hexOf("1234567890123456789012345678901234567890123456789012345678901234567890123456789"
                "0") == "57edf4a22be3c955ac49da2e2107b67a");
}

TEST_CASE("md5: the padding boundaries") {
    // 55 bytes: the longest message whose padding + length still fit one block.
    CHECK(hexOf(std::string(55, 'x')) == "04364420e25c512fd958a70738aa8f72");
    // 56 bytes: the first length forced onto a second padding block.
    CHECK(hexOf(std::string(56, 'x')) == "668a72d5ba17f08e62dabcafad6db14b");
    // 64 bytes: an exact block; the tail is padding alone.
    CHECK(hexOf(std::string(64, 'x')) == "c1bb4f81d892b2d57947682aeb252456");
}
