// THE ON-DISK CHUNK WALK (io/mosaic/tagscan.hpp) -- reading a few chunks WITHOUT reading the file.
//
// The container's ground-truth scan takes a span, so everything that wanted one chunk out of a
// document first pulled the document into memory: 302 MB read and every tile hashed to answer
// "how big is this canvas, and is there a thumbnail". tagscan walks the frame chain on disk,
// reading 46-byte headers and seeking past the payloads nobody asked for.
//
// That trade is only sound if it keeps the container's recovery posture, so these cases are about
// exactly that -- not about speed, which a test cannot honestly assert anyway:
//
//   1. It finds what the in-memory scan finds. Same newest-generation-wins rule, same answer.
//   2. IT SEES PAST THE DIRECTORY. Appended Save batches land after the base checkpoint's DIR, so
//      the newest manifest is frequently NOT the one the directory indexes. A reader that trusted
//      the directory would confidently return a stale canvas size; the frame walk cannot, because
//      it does not consult one.
//   3. A corrupted length field costs a resync, never a wrong answer. The walk skips a frame on
//      the strength of a length it has not verified -- that is the whole point -- so what happens
//      when that length lies is the question the design rests on.
//   4. A damaged copy of a wanted chunk is not returned, and an older intact replica still is.
//   5. A missing tag reads as nullopt, and so does a file that is not a container at all.

#include "io/mosaic/chunk.hpp"
#include "io/mosaic/file.hpp"
#include "io/mosaic/format.hpp"
#include "io/mosaic/tagscan.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace mosaic::io::native;

std::vector<std::uint8_t> pattern(std::size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> p(n);
    for (std::size_t i = 0; i < n; ++i)
        p[i] = static_cast<std::uint8_t>(seed + i * 7);
    return p;
}

// A checkpoint with a manifest, a preview and enough tiles that the walk has real frames to step
// over rather than a single header.
CheckpointInput sampleInput(std::uint64_t generation, std::uint8_t manifestSeed) {
    CheckpointInput in;
    in.documentUuid = "tagscan-uuid-0001";
    in.generation = generation;
    in.chunks.push_back({kTypeManifest, zeroKey(), generation, Profile::Balanced, kFlagCritical,
                         false, false, pattern(600, manifestSeed)});
    in.chunks.push_back({kTypePreview, zeroKey(), generation, Profile::Balanced, 0, false, false,
                         pattern(256, 2)});
    for (std::uint32_t i = 0; i < 8; ++i)
        in.chunks.push_back({kTypeTile, tileKey(7, i, 0), generation, Profile::Balanced,
                             kFlagCritical, true, false,
                             pattern(3000 + i * 13, static_cast<std::uint8_t>(3 + i))});
    return in;
}

// A scratch path that cleans up after itself.
struct TempFile {
    fs::path path;
    explicit TempFile(const std::string& leaf)
        : path(fs::temp_directory_path() / ("mosaic_tagscan_" + leaf)) {}
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
    void write(const std::vector<std::uint8_t>& bytes) const {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    [[nodiscard]] std::string str() const { return path.string(); }
};

// The same question asked of the in-memory scan, so the two can be compared directly.
std::optional<std::vector<std::uint8_t>> newestInMemory(const std::vector<std::uint8_t>& file,
                                                        const ChunkTag& tag) {
    std::optional<ChunkRecord> best;
    for (const ChunkRecord& rec : scanChunks(file)) {
        if (!rec.valid || rec.type != tag)
            continue;
        if (!best.has_value() || rec.generation >= best->generation)
            best = rec;
    }
    if (!best.has_value())
        return std::nullopt;
    return decodeChunkPayload(*best, file);
}

} // namespace

TEST_CASE("tagscan: the on-disk walk agrees with the in-memory scan") {
    const std::vector<std::uint8_t> bytes = buildCheckpoint(sampleInput(40, 1));
    const TempFile tmp("agree.mosaic");
    tmp.write(bytes);

    const std::array<ChunkTag, 2> tags{kTypeManifest, kTypePreview};
    const auto got = readNewestChunkPayloads(tmp.str(), tags);
    REQUIRE(got.size() == 2);

    const auto wantManifest = newestInMemory(bytes, kTypeManifest);
    const auto wantPreview = newestInMemory(bytes, kTypePreview);
    REQUIRE(wantManifest.has_value());
    REQUIRE(wantPreview.has_value());
    REQUIRE(got[0].has_value());
    REQUIRE(got[1].has_value());
    CHECK(*got[0] == *wantManifest);
    CHECK(*got[1] == *wantPreview);
    CHECK(*got[0] == pattern(600, 1)); // and it is the payload that went in
}

TEST_CASE("tagscan: an APPENDED manifest wins over the one the directory indexes") {
    // The case a directory-driven shortcut would get wrong, and the reason this walks frames. The
    // base checkpoint's DIR is written before the end-of-checkpoint root replicas; appended Save
    // batches land after them, so a document saved ten times carries a newest manifest the base
    // directory has never heard of. Reading `base_dir_offset` would answer with a stale canvas
    // size and look completely healthy doing it.
    std::vector<std::uint8_t> bytes = buildCheckpoint(sampleInput(40, 1));
    const std::size_t baseLen = bytes.size();

    // A newer manifest, appended exactly as a later Save would append one.
    const std::vector<std::uint8_t> newer = pattern(600, 99);
    appendChunk(bytes, kTypeManifest, zeroKey(), /*generation=*/41, newer, Profile::Balanced);
    REQUIRE(bytes.size() > baseLen);

    const TempFile tmp("appended.mosaic");
    tmp.write(bytes);

    const std::array<ChunkTag, 1> tags{kTypeManifest};
    const auto got = readNewestChunkPayloads(tmp.str(), tags);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].has_value());
    CHECK(*got[0] == newer);                 // the appended one
    CHECK_FALSE(*got[0] == pattern(600, 1)); // not the checkpoint's
}

TEST_CASE("tagscan: a corrupted length field costs a resync, not the answer") {
    // The walk skips a frame on the strength of a length it has NOT verified. When that length
    // lies, the next header lands on bytes that are not MAGIC -- and the walk must hunt forward
    // for the next real frame rather than give up or return something wrong.
    std::vector<std::uint8_t> bytes = buildCheckpoint(sampleInput(40, 1));

    // Find the first TILE frame and corrupt its PAYLOAD LENGTH so the walk is told to step to a
    // place no frame begins. The manifest and preview sit before it in file order, so this also
    // proves the walk keeps going rather than stopping at the damage.
    std::size_t victim = 0;
    for (const ChunkRecord& rec : scanChunks(bytes)) {
        if (rec.valid && rec.type == kTypeTile) {
            victim = rec.offset;
            break;
        }
    }
    REQUIRE(victim != 0);
    // A length that stays inside the file (so it is a plausible frame, not an obvious overrun)
    // but points into the middle of the following payload.
    bytes[victim + kOffPayloadLen] = static_cast<std::uint8_t>(bytes[victim + kOffPayloadLen] + 3);

    const TempFile tmp("badlen.mosaic");
    tmp.write(bytes);

    // Ask for a tag that appears AFTER the damaged frame, so the answer can only come from a
    // successful resync. buildCheckpoint replicates the manifest late in the file (spec 2.3).
    const std::array<ChunkTag, 2> tags{kTypeManifest, kTypePreview};
    const auto got = readNewestChunkPayloads(tmp.str(), tags);
    REQUIRE(got.size() == 2);
    REQUIRE(got[0].has_value());
    CHECK(*got[0] == pattern(600, 1));
}

TEST_CASE("tagscan: a damaged wanted frame is refused, and an older intact one still answers") {
    // Validity is the checksum's word, not the header's. A frame whose payload has been altered
    // must not be returned even though its header parses perfectly -- and if an older generation
    // of the same tag survives, that is the honest answer.
    std::vector<std::uint8_t> bytes = buildCheckpoint(sampleInput(40, 1));
    const std::vector<std::uint8_t> newer = pattern(600, 99);
    appendChunk(bytes, kTypeManifest, zeroKey(), /*generation=*/41, newer, Profile::Balanced);

    // Corrupt the appended (newest) manifest's payload, leaving its header intact.
    std::size_t newestOffset = 0;
    std::uint64_t newestGen = 0;
    std::size_t payloadAt = 0;
    for (const ChunkRecord& rec : scanChunks(bytes)) {
        if (rec.valid && rec.type == kTypeManifest && rec.generation >= newestGen) {
            newestGen = rec.generation;
            newestOffset = rec.offset;
            payloadAt = rec.payloadOffset;
        }
    }
    REQUIRE(newestOffset != 0);
    REQUIRE(newestGen == 41);
    bytes[payloadAt] = static_cast<std::uint8_t>(bytes[payloadAt] ^ 0xFF);

    const TempFile tmp("damaged.mosaic");
    tmp.write(bytes);

    const std::array<ChunkTag, 1> tags{kTypeManifest};
    const auto got = readNewestChunkPayloads(tmp.str(), tags);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].has_value());
    CHECK(*got[0] == pattern(600, 1)); // the checkpoint's intact copy, not the damaged newer one
    CHECK_FALSE(*got[0] == newer);
}

TEST_CASE("tagscan: a tag the file does not carry, and a file that is not a container") {
    SUBCASE("missing tag") {
        CheckpointInput in = sampleInput(40, 1);
        // Drop the preview: a document saved before previews existed, or written by a tool that
        // emits none (torture_doc, until it learned to).
        std::erase_if(in.chunks, [](const FileChunk& c) { return c.type == kTypePreview; });
        const TempFile tmp("nopreview.mosaic");
        tmp.write(buildCheckpoint(in));

        const std::array<ChunkTag, 2> tags{kTypeManifest, kTypePreview};
        const auto got = readNewestChunkPayloads(tmp.str(), tags);
        REQUIRE(got.size() == 2);
        CHECK(got[0].has_value());       // the manifest is there
        CHECK_FALSE(got[1].has_value()); // the preview is not, and that is not an error
    }
    SUBCASE("not a container") {
        const TempFile tmp("garbage.bin");
        tmp.write(pattern(50000, 5));
        const std::array<ChunkTag, 1> tags{kTypeManifest};
        const auto got = readNewestChunkPayloads(tmp.str(), tags);
        REQUIRE(got.size() == 1);
        CHECK_FALSE(got[0].has_value());
    }
    SUBCASE("no such file") {
        const std::array<ChunkTag, 1> tags{kTypeManifest};
        const auto got = readNewestChunkPayloads("/nonexistent/mosaic/tagscan/file", tags);
        REQUIRE(got.size() == 1);
        CHECK_FALSE(got[0].has_value());
    }
}
