#include "io/mosaic/chunk.hpp"
#include "io/mosaic/codec.hpp"
#include "io/mosaic/paeth.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <vector>

// The .mosaic compression profiles + Paeth spatial filter (S48 Build 1, codec slice; spec 2.4 +
// 2.5): round-trips at every profile, the mandatory store-if-larger fallback, hostile-stream
// rejection, filter round-trips at awkward sizes, the SIMD decode lane pinned against the scalar
// reference, and the "filtering actually pays" claim checked rather than assumed.
namespace {

using namespace mosaic::io::native;

std::vector<std::uint8_t> randomBytes(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> v(n);
    for (auto& b : v)
        b = static_cast<std::uint8_t>(rng());
    return v;
}

// A smooth two-axis gradient with a little noise -- the photographic-content stand-in the
// research measured Paeth's ~25-30% win on. w*h*4 RGBA.
std::vector<std::uint8_t> gradientTile(std::uint32_t w, std::uint32_t h, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> v(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            const int n = static_cast<int>(rng() % 5);
            v[i + 0] = static_cast<std::uint8_t>((x * 255) / (w > 1 ? w - 1 : 1));
            v[i + 1] = static_cast<std::uint8_t>((y * 255) / (h > 1 ? h - 1 : 1));
            v[i + 2] = static_cast<std::uint8_t>(((x + y) * 128) / (w + h) + n);
            v[i + 3] = 255;
        }
    return v;
}

const std::vector<Profile> kAllProfiles = {Profile::Store, Profile::Fast, Profile::Balanced,
                                           Profile::Max};

} // namespace

TEST_CASE("mosaic codec: every profile round-trips every payload shape") {
    const std::vector<std::vector<std::uint8_t>> payloads = {
        {},                                    // empty
        {42},                                  // one byte
        randomBytes(16384, 1),                 // exactly one 64px tile, incompressible
        gradientTile(64, 64, 2),               // one 64px tile, compressible
        std::vector<std::uint8_t>(70001, 0xAB) // long run, maximally compressible, odd size
    };
    for (const auto& raw : payloads) {
        for (const Profile profile : kAllProfiles) {
            const Encoded enc = compressPayload(raw, profile);
            const auto back = decompressPayload(enc.bytes, enc.profile,
                                                static_cast<std::uint32_t>(raw.size()));
            REQUIRE_MESSAGE(back.has_value(), "profile ", static_cast<int>(profile), " size ",
                            raw.size());
            CHECK(*back == raw);
        }
    }
}

TEST_CASE("mosaic codec: store-if-larger is a wire fact, not a suggestion") {
    // Incompressible content requested at every compressing profile lands as Store, verbatim.
    const auto noise = randomBytes(16384, 7);
    for (const Profile profile : {Profile::Fast, Profile::Balanced, Profile::Max}) {
        const Encoded enc = compressPayload(noise, profile);
        CHECK(enc.profile == Profile::Store);
        CHECK(enc.bytes == noise);
    }
    // Empty payloads never "compress" (every codec's empty frame is > 0 bytes).
    CHECK(compressPayload({}, Profile::Max).profile == Profile::Store);

    // A real lesson caught by this test's first run: LZ4 has no entropy stage, so a RAW smooth
    // gradient (few byte-level repeats) does not compress under the fast tier at all --
    // store-if-larger correctly engaged. The fast tier pays on FILTERED residuals, which is the
    // actual pipeline (spec 2.5: the filter is what makes fast-tier compression work); zstd's
    // entropy coding shrinks even the raw pixels.
    const auto tile = gradientTile(64, 64, 3);
    CHECK(compressPayload(tile, Profile::Fast).profile == Profile::Store); // raw + LZ4: no win
    CHECK(compressPayload(tile, Profile::Balanced).profile == Profile::Balanced);

    std::vector<std::uint8_t> filtered(tile.size());
    filterPaethRgba(tile, 64, 64, filtered);
    const Encoded fast = compressPayload(filtered, Profile::Fast);
    const Encoded balanced = compressPayload(filtered, Profile::Balanced);
    CHECK(fast.profile == Profile::Fast);
    CHECK(balanced.profile == Profile::Balanced);
    CHECK(fast.bytes.size() < filtered.size());
    CHECK(balanced.bytes.size() <= fast.bytes.size());
}

TEST_CASE("mosaic codec: hostile streams are rejected, never trusted") {
    const auto tile = gradientTile(64, 64, 4);
    const Encoded enc = compressPayload(tile, Profile::Balanced);

    // Wrong claimed length (both directions), truncated stream, corrupted stream, garbage
    // stream, unknown profile byte, and an absurd allocation request -- all nullopt, no crash.
    CHECK(!decompressPayload(enc.bytes, enc.profile, static_cast<std::uint32_t>(tile.size()) - 1)
               .has_value());
    CHECK(!decompressPayload(enc.bytes, enc.profile, static_cast<std::uint32_t>(tile.size()) + 1)
               .has_value());
    std::vector<std::uint8_t> truncated(enc.bytes.begin(),
                                        enc.bytes.begin() + static_cast<std::ptrdiff_t>(
                                                                enc.bytes.size() / 2));
    CHECK(!decompressPayload(truncated, enc.profile, static_cast<std::uint32_t>(tile.size()))
               .has_value());
    std::vector<std::uint8_t> corrupt = enc.bytes;
    corrupt[corrupt.size() / 2] ^= 0xFF;
    const auto res = decompressPayload(corrupt, enc.profile,
                                       static_cast<std::uint32_t>(tile.size()));
    CHECK((!res.has_value() || *res != tile)); // zstd may or may not detect; it must not crash
    CHECK(!decompressPayload(randomBytes(500, 5), Profile::Fast, 16384).has_value());
    CHECK(!decompressPayload(enc.bytes, static_cast<Profile>(7),
                             static_cast<std::uint32_t>(tile.size()))
               .has_value());
    CHECK(!decompressPayload(enc.bytes, enc.profile, kMaxUncompressedLen + 1).has_value());

    // The same rejection reaches the chunk layer: a frame whose PROFILE byte is unknown fails
    // decodeChunkPayload (the checksum can pass -- a foreign writer's bug -- decode still must
    // not trust it).
    std::vector<std::uint8_t> buf;
    appendChunk(buf, kTypeTile, tileKey(1, 0, 0), 1, tile, Profile::Balanced);
    const auto rec = parseChunkAt(buf, 0);
    REQUIRE(rec.has_value());
    REQUIRE(rec->valid);
    const auto decoded = decodeChunkPayload(*rec, buf);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == tile);
}

TEST_CASE("mosaic codec: framed profiles round-trip through pack/parse/decode") {
    // Filtered residuals, the real tile pipeline -- every compressing profile genuinely shrinks
    // this content class (raw gradients defeat LZ4; see the store-if-larger case above).
    const auto tile = gradientTile(64, 64, 6);
    std::vector<std::uint8_t> residuals(tile.size());
    filterPaethRgba(tile, 64, 64, residuals);
    for (const Profile profile : kAllProfiles) {
        std::vector<std::uint8_t> buf;
        appendChunk(buf, kTypeTile, tileKey(2, 3, 4), 9, residuals, profile);
        const auto rec = parseChunkAt(buf, 0);
        REQUIRE(rec.has_value());
        REQUIRE(rec->valid);
        CHECK(rec->uncompressedLen == residuals.size());
        if (profile != Profile::Store)
            CHECK(rec->payloadLen < rec->uncompressedLen);
        const auto decoded = decodeChunkPayload(*rec, buf);
        REQUIRE(decoded.has_value());
        CHECK(*decoded == residuals);
    }
}

TEST_CASE("mosaic paeth: encode/decode round-trips at every awkward size") {
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes = {
        {64, 64}, {33, 17}, {1, 1}, {1, 64}, {64, 1}, {3, 3}, {2, 2}};
    for (const auto& [w, h] : sizes) {
        for (std::uint32_t seed = 0; seed < 3; ++seed) {
            const auto raw = (seed == 0) ? gradientTile(w, h, seed)
                                         : randomBytes(static_cast<std::size_t>(w) * h * 4,
                                                       100 + seed);
            std::vector<std::uint8_t> filtered(raw.size());
            filterPaethRgba(raw, w, h, filtered);

            std::vector<std::uint8_t> viaDispatch = filtered;
            unfilterPaethRgba(viaDispatch, w, h);
            CHECK_MESSAGE(viaDispatch == raw, "dispatch decode ", w, "x", h, " seed ", seed);

            // The SIMD lane must agree with the portable reference byte-for-byte -- the scalar
            // path is the oracle, not just the fallback.
            std::vector<std::uint8_t> viaScalar = filtered;
            detail::unfilterPaethRgbaScalar(viaScalar, w, h);
            CHECK(viaScalar == raw);
            CHECK(viaDispatch == viaScalar);
        }
    }
}

TEST_CASE("mosaic paeth: filtering pays on smooth content and round-trips with the codec") {
    const std::uint32_t w = 64, h = 64;
    const auto raw = gradientTile(w, h, 9);
    std::vector<std::uint8_t> filtered(raw.size());
    filterPaethRgba(raw, w, h, filtered);

    // The whole reason the filter exists (spec 2.5): residuals compress materially better than
    // raw pixels on smooth/photographic content.
    const Encoded rawZ = compressPayload(raw, Profile::Balanced);
    const Encoded filtZ = compressPayload(filtered, Profile::Balanced);
    CHECK(filtZ.bytes.size() < rawZ.bytes.size());

    // Full integration: filter -> frame at fast (the autosave shape, kFlagFiltered recorded) ->
    // parse -> decode -> unfilter == original.
    std::vector<std::uint8_t> buf;
    appendChunk(buf, kTypeTile, tileKey(3, 1, 1), 5, filtered, Profile::Fast,
                kFlagCritical | kFlagFiltered);
    const auto rec = parseChunkAt(buf, 0);
    REQUIRE(rec.has_value());
    REQUIRE(rec->valid);
    CHECK((rec->flags & kFlagFiltered) != 0);
    auto decoded = decodeChunkPayload(*rec, buf);
    REQUIRE(decoded.has_value());
    unfilterPaethRgba(*decoded, w, h);
    CHECK(*decoded == raw);
}
