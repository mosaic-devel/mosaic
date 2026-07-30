#include "common/fs_path.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <string_view>

using namespace mosaic;
namespace fs = std::filesystem;

// These run on the LINUX host, where `fs::path` is byte-based and both helpers are the identity.
// That is not a weak test: the identity IS the Linux/macOS invariant worth pinning. The whole
// argument for introducing the helper this late (see fs_path.hpp) is that POSIX behaviour cannot
// move, so a regression that made pathFromUtf8 transcode -- e.g. someone "fixing" it to go through
// the wide API unconditionally -- would break Linux paths silently, and would break here loudly.
namespace {

// UTF-8 spellings written as explicit bytes rather than as source literals: the test must not
// depend on the compiler's input charset, and the byte sequence is the thing under test. The
// literals are split wherever a hex escape is followed by a hex digit -- "\x9Fe" would otherwise
// be read as ONE out-of-range escape rather than U+00DF followed by 'e'.
constexpr std::string_view kAccented =
    "\xC3\x9C" "bungen/Gr\xC3\xBC\xC3\x9F" "e.png";                                // Übungen/Grüße
constexpr std::string_view kCjk = "\xE5\x86\x99\xE7\x9C\x9F/\xE7\x8C\xAB.jpg";      // 写真/猫

}  // namespace

TEST_CASE("utf8 path round-trips ASCII") {
    const std::string in = "/home/artist/pictures/cat.png";
    CHECK(common::utf8FromPath(common::pathFromUtf8(in)) == in);
}

TEST_CASE("utf8 path round-trips non-ASCII") {
    for (const std::string_view in : {kAccented, kCjk}) {
        const fs::path p = common::pathFromUtf8(in);
        // The bytes must survive the crossing in BOTH directions. A code-page conversion (the
        // Windows failure this helper exists to prevent) shows up as a shorter or altered string.
        CHECK(common::utf8FromPath(p) == std::string(in));
    }
}

TEST_CASE("utf8 path round-trips the empty string") {
    // Empty is a real case, not a corner: configDir()/dataDir() return an empty path when nothing
    // resolves, and callers test .empty() rather than special-casing. It must not become ".".
    const fs::path p = common::pathFromUtf8("");
    CHECK(p.empty());
    CHECK(common::utf8FromPath(p).empty());
}

TEST_CASE("utf8 path is byte-identity on POSIX") {
#ifndef _WIN32
    // The invariant that makes this change safe on Linux and macOS: byte-for-byte the same result
    // as the fs::path(std::string) / path.string() calls the codebase already makes everywhere.
    for (const std::string_view in : {std::string_view("/tmp/plain"), kAccented, kCjk}) {
        CHECK(common::pathFromUtf8(in) == fs::path(std::string(in)));
        CHECK(common::utf8FromPath(fs::path(std::string(in))) == std::string(in));
    }
#endif
}

TEST_CASE("utf8 path composes with filesystem operations") {
    // The helper's real job is producing a path you can then build on; appending must not disturb
    // the encoded components.
    const fs::path dir = common::pathFromUtf8(kAccented).parent_path();
    const fs::path file = dir / common::pathFromUtf8("na\xC3\xAFve.icc");  // naïve.icc
    CHECK(common::utf8FromPath(file.filename()) == "na\xC3\xAFve.icc");
    CHECK(common::utf8FromPath(dir) == "\xC3\x9C" "bungen");
}
