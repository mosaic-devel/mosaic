#include "io/mosaic/chunk.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <vector>

// The .mosaic container's chunk framing (S48 Build 1, slice 1) -- the spec's section-7 framing
// battery: round-trips, header-covering checksums (a flipped KEY/GENERATION byte must fail like
// payload damage), explicit-link frames, magic-resync past corrupted length fields, torn tails,
// and garbage inputs. docs/mosaic-native-format.md sections 2.1-2.2, 2.8.
namespace {

using namespace mosaic::io::native;

std::vector<std::uint8_t> bytes(std::initializer_list<int> v) {
    std::vector<std::uint8_t> out;
    for (int b : v)
        out.push_back(static_cast<std::uint8_t>(b));
    return out;
}

std::vector<std::uint8_t> patternPayload(std::size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> p(n);
    for (std::size_t i = 0; i < n; ++i)
        p[i] = static_cast<std::uint8_t>(seed + i * 7);
    return p;
}

} // namespace

TEST_CASE("mosaic format: preamble round-trips and rejects imposters") {
    std::vector<std::uint8_t> buf;
    appendPreamble(buf, kDocTypeRasterVector);
    REQUIRE(buf.size() == kPreambleSize);

    const auto pre = parsePreamble(buf.data(), buf.size());
    REQUIRE(pre.has_value());
    CHECK(pre->version == kFormatVersion);
    CHECK(pre->documentType == kDocTypeRasterVector);

    // Reserved bytes are zero (they are the format's future, not garbage).
    for (std::size_t i = 10; i < kPreambleSize; ++i)
        CHECK(buf[i] == 0);

    // Too short, wrong magic, null -- all rejected.
    CHECK(!parsePreamble(buf.data(), kPreambleSize - 1).has_value());
    std::vector<std::uint8_t> wrong = buf;
    wrong[0] ^= 0xFF;
    CHECK(!parsePreamble(wrong.data(), wrong.size()).has_value());
    CHECK(!parsePreamble(nullptr, 0).has_value());

    // The preamble magic and the chunk magic are distinct sentinels.
    CHECK(kPreambleMagic != kChunkMagic);
}

TEST_CASE("mosaic format: chunk pack/parse round-trip, fast and strong checksums") {
    const auto payload = patternPayload(2000, 3);
    const std::uint64_t bigGeneration = (std::uint64_t{1} << 40) + 12345; // u64 earning its keep
    const ChunkKey key = tileKey(std::uint64_t{1} << 33, 70000, 12); // beyond u32/1000-tile limits

    std::vector<std::uint8_t> buf;
    appendPreamble(buf, kDocTypeRasterVector);
    appendChunk(buf, kTypeTile, key, bigGeneration, payload);
    const std::size_t rootAt = buf.size();
    appendChunk(buf, kTypeRoot, zeroKey(), 7, bytes({1, 2, 3}));

    const auto tile = parseChunkAt(buf, kPreambleSize);
    REQUIRE(tile.has_value());
    CHECK(tile->valid);
    CHECK(tile->type == kTypeTile);
    CHECK(tile->key == key);
    CHECK(tile->generation == bigGeneration);
    CHECK(tile->payloadLen == payload.size());
    CHECK(tile->checksumSize == kFastChecksumSize);
    const auto span = tile->payload(buf);
    CHECK(std::equal(span.begin(), span.end(), payload.begin(), payload.end()));

    // ROOT gets the strong (32-byte BLAKE3) suffix -- the importance-weighted check.
    const auto root = parseChunkAt(buf, rootAt);
    REQUIRE(root.has_value());
    CHECK(root->valid);
    CHECK(root->checksumSize == kStrongChecksumSize);
    CHECK(root->consumed == kHeaderSize + 3 + kStrongChecksumSize);

    // Key builders write distinct, order-sensitive identities.
    CHECK(tileKey(1, 2, 3) == tileKey(1, 2, 3));
    CHECK(tileKey(1, 2, 3) != tileKey(1, 3, 2));
    CHECK(vectorKey(9) == histKey(9)); // same bytes by design -- (TYPE, KEY) is the identity
}

TEST_CASE("mosaic format: the checksum covers every header field, not just the payload") {
    const auto payload = patternPayload(64, 11);
    std::vector<std::uint8_t> clean;
    appendChunk(clean, kTypeTile, tileKey(5, 1, 2), 42, payload);

    // Flip every single byte after MAGIC in turn -- type, flags, profile, all 16 KEY bytes, all
    // 8 GENERATION bytes, both lengths, every payload byte region representative, and the stored
    // checksum itself. Each flip must kill verification. This is precisely the class of bug a
    // payload-only checksum misses (spec 2.2: a flipped KEY would otherwise attribute an intact
    // payload to the WRONG tile with no error anywhere).
    for (std::size_t pos = 8; pos < clean.size(); ++pos) {
        std::vector<std::uint8_t> mutated = clean;
        mutated[pos] ^= 0x01;
        const auto rec = parseChunkAt(mutated, 0);
        if (!rec.has_value())
            continue; // never happens: MAGIC untouched
        CHECK_MESSAGE(!rec->valid, "flipped byte at offset ", pos, " must fail verification");
    }

    // Flipping MAGIC bytes means "no chunk here" rather than "invalid chunk".
    std::vector<std::uint8_t> noMagic = clean;
    noMagic[3] ^= 0xFF;
    CHECK(!parseChunkAt(noMagic, 0).has_value());
}

TEST_CASE("mosaic format: explicit-link frames bind order without poisoning successors") {
    const std::array<std::uint8_t, kLinkSize> seed = {9, 9, 9, 9, 9, 9, 9, 9};
    std::vector<std::uint8_t> buf;
    appendChunk(buf, kTypeTile, tileKey(1, 0, 0), 10, patternPayload(100, 1), Profile::Store, kFlagCritical, &seed);
    const auto a = parseChunkAt(buf, 0);
    REQUIRE(a.has_value());
    REQUIRE(a->valid);
    CHECK(a->linked());
    CHECK(a->link == seed);

    const auto aLink = a->linkValue();
    const std::size_t bAt = buf.size();
    appendChunk(buf, kTypeHist, histKey(10), 10, patternPayload(40, 2), Profile::Store, kFlagCritical, &aLink);
    const auto b = parseChunkAt(buf, bAt);
    REQUIRE(b.has_value());
    REQUIRE(b->valid);
    CHECK(b->link == a->linkValue()); // chain validation = compare embedded link vs actual checksum

    // A flipped LINK byte fails B's own checksum (LINK is inside the checked region)...
    std::vector<std::uint8_t> mutated = buf;
    mutated[bAt + kHeaderSize] ^= 0xFF;
    CHECK(!parseChunkAt(mutated, bAt)->valid);

    // ...but damaging A leaves B independently verifiable -- the whole point of explicit-link
    // over a cumulative chain (Round 11: a destroyed frame localizes to a gap; the reader
    // detects it by LINK mismatch, not by losing the ability to verify everything after).
    std::vector<std::uint8_t> aDamaged = buf;
    aDamaged[kHeaderSize + kLinkSize + 5] ^= 0xFF; // inside A's payload
    CHECK(!parseChunkAt(aDamaged, 0)->valid);
    CHECK(parseChunkAt(aDamaged, bAt)->valid);
}

TEST_CASE("mosaic format: scan resyncs past corrupted length fields and torn tails") {
    std::vector<std::uint8_t> buf;
    std::vector<std::size_t> offsets;
    for (int i = 0; i < 20; ++i) {
        offsets.push_back(buf.size());
        appendChunk(buf, kTypeTile, tileKey(1, static_cast<std::uint32_t>(i), 0),
                    static_cast<std::uint64_t>(100 + i), patternPayload(300 + i * 11, static_cast<std::uint8_t>(i)));
    }

    // Smash chunk 5's PAYLOAD_LEN into nonsense -- the classic PNG killer. The scan must lose
    // exactly that one chunk and resync to the other 19 (EBML's answer, spec 2.2 MAGIC bullet).
    std::vector<std::uint8_t> smashed = buf;
    smashed[offsets[5] + kOffPayloadLen] = 0xFF;
    smashed[offsets[5] + kOffPayloadLen + 1] = 0xFF;
    smashed[offsets[5] + kOffPayloadLen + 2] = 0xFF;
    const auto recs = scanChunks(smashed);
    std::size_t validCount = 0;
    for (const auto& r : recs)
        validCount += r.valid ? 1 : 0;
    CHECK(validCount == 19);

    // Torn tail: cut the final chunk in half -- 19 valid, the last incomplete, no crash.
    std::vector<std::uint8_t> torn(buf.begin(),
                                   buf.begin() + static_cast<std::ptrdiff_t>(offsets[19] + 60));
    const auto tornRecs = scanChunks(torn);
    std::size_t tornValid = 0;
    bool sawIncomplete = false;
    for (const auto& r : tornRecs) {
        tornValid += r.valid ? 1 : 0;
        sawIncomplete = sawIncomplete || (!r.complete && r.offset == offsets[19]);
    }
    CHECK(tornValid == 19);
    CHECK(sawIncomplete);
}

TEST_CASE("mosaic format: magic-lookalike bytes inside a valid payload do not derail the scan") {
    // A payload that contains the chunk magic verbatim -- compressed tile data can and will
    // produce such runs eventually; a valid chunk is consumed wholesale so it never matters.
    std::vector<std::uint8_t> payload = patternPayload(64, 5);
    payload.insert(payload.begin() + 20, kChunkMagic.begin(), kChunkMagic.end());

    std::vector<std::uint8_t> buf;
    appendChunk(buf, kTypeTile, tileKey(1, 0, 0), 1, payload);
    appendChunk(buf, kTypeTile, tileKey(1, 1, 0), 1, patternPayload(64, 6));
    const auto recs = scanChunks(buf);
    REQUIRE(recs.size() == 2);
    CHECK(recs[0].valid);
    CHECK(recs[1].valid);

    // Even when the FIRST chunk is damaged (so the scan cannot consume it wholesale and must
    // resync byte-by-byte THROUGH the embedded magic), the lookalike is rejected by checksum and
    // the second chunk is still found -- the spec's "the checksum check, not the magic match, is
    // the actual defense".
    std::vector<std::uint8_t> damaged = buf;
    damaged[kOffPayloadLen] = 0xFF;
    damaged[kOffPayloadLen + 1] = 0xFF;
    const auto recs2 = scanChunks(damaged);
    std::size_t valid2 = 0;
    for (const auto& r : recs2)
        valid2 += r.valid ? 1 : 0;
    CHECK(valid2 == 1);
}

TEST_CASE("mosaic format: known-answer checksums pin the on-disk format forever") {
    // Reference digests generated against the upstream xxHash 0.8.3 / BLAKE3 1.8.4 system
    // libraries before the implementations were vendored (third_party/xxhash, third_party/
    // blake3). These bytes ARE the format: any implementation swap, flag change, or "harmless"
    // refactor that alters them breaks every .mosaic file ever written. If this test fails, the
    // code is wrong -- the constants are not to be regenerated.
    std::vector<std::uint8_t> payload;
    for (int i = 0; i < 256; ++i)
        payload.push_back(static_cast<std::uint8_t>(i));

    std::vector<std::uint8_t> buf;
    appendChunk(buf, kTypeTile, tileKey(0x0123456789ABCDEFull, 7, 9), 0x00000002DEADBEEFull,
                payload);
    const auto tile = parseChunkAt(buf, 0);
    REQUIRE(tile.has_value());
    REQUIRE(tile->valid);
    const auto expectedTile = bytes({0x0C, 0x48, 0x25, 0x63, 0xA0, 0x83, 0x8E, 0x45});
    CHECK(std::equal(expectedTile.begin(), expectedTile.end(), tile->checksum.begin()));

    const std::size_t rootAt = buf.size();
    appendChunk(buf, kTypeRoot, zeroKey(), 42, payload);
    const auto root = parseChunkAt(buf, rootAt);
    REQUIRE(root.has_value());
    REQUIRE(root->valid);
    const auto expectedRoot = bytes({0x93, 0xDD, 0x11, 0xA8, 0x4D, 0xD3, 0xDC, 0x1F,
                                     0xC5, 0x3C, 0x75, 0x8F, 0x42, 0xF7, 0xE8, 0x49,
                                     0x83, 0x57, 0x11, 0x57, 0x79, 0xB5, 0x28, 0x2C,
                                     0xBA, 0x91, 0xCA, 0xEB, 0x50, 0xD5, 0x7F, 0x9B});
    CHECK(std::equal(expectedRoot.begin(), expectedRoot.end(), root->checksum.begin()));

    const std::array<std::uint8_t, kLinkSize> link = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::size_t linkedAt = buf.size();
    appendChunk(buf, kTypeHist, histKey(3), 3, payload, Profile::Store, kFlagCritical, &link);
    const auto linked = parseChunkAt(buf, linkedAt);
    REQUIRE(linked.has_value());
    REQUIRE(linked->valid);
    const auto expectedLinked = bytes({0x34, 0x26, 0xBC, 0x9D, 0xFB, 0xF2, 0x56, 0xC9});
    CHECK(std::equal(expectedLinked.begin(), expectedLinked.end(), linked->checksum.begin()));
}

TEST_CASE("mosaic format: garbage in, no crash out") {
    CHECK(scanChunks({}).empty());

    const std::vector<std::uint8_t> zeros(4096, 0);
    CHECK(scanChunks(zeros).empty());

    std::mt19937 rng(20260707);
    std::vector<std::uint8_t> noise(1 << 16);
    for (auto& b : noise)
        b = static_cast<std::uint8_t>(rng());
    for (const auto& r : scanChunks(noise))
        CHECK(!r.valid); // random bytes may contain magic-lookalikes; none may verify
}
