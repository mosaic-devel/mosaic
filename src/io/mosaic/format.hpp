#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// mosaic/format -- byte-level layout for the .mosaic native container (S48 Build 1; the spec is
// docs/mosaic-native-format.md, sections 2.1-2.2, and this header follows it exactly). Everything
// is little-endian at fixed offsets. Framing logic lives in chunk.hpp; this is layout only.
namespace mosaic::io::native {

inline constexpr std::uint8_t kFormatVersion = 1;

// --- Preamble (spec 2.1): 16 bytes at offset 0 -- magic(8) + format-version(1) +
// documentType(1) + 6 reserved zero bytes. A convenience for instant type-sniffing, never
// load-bearing: recovery must work with the preamble destroyed.
//
// The magic steals PNG's transfer-corruption trap byte-for-byte in spirit: a high-bit lead byte
// (catches 7-bit strippers), "MOS", CRLF (catches line-ending translation), 0x1A (stops DOS
// `type`), LF (catches CRLF->LF rewriting).
inline constexpr std::array<std::uint8_t, 8> kPreambleMagic = {0x8C, 'M', 'O', 'S',
                                                               0x0D, 0x0A, 0x1A, 0x0A};
inline constexpr std::size_t kPreambleSize = 16;

// documentType codes: raster+vector is the only type actually built; vector-only is reserved for
// S30-b and not exercised until that document type exists.
inline constexpr std::uint8_t kDocTypeRasterVector = 0;
inline constexpr std::uint8_t kDocTypeVectorOnly = 1;

struct Preamble {
    std::uint8_t version = 0;
    std::uint8_t documentType = 0;
};

// `version` is a parameter so a test can forge a file from the future; real writers stamp
// kFormatVersion. It is ALSO written into the (checksummed) root -- see file.hpp -- because these
// 16 bytes carry no checksum of their own, and a version gate that trusted them alone would let a
// single rotted byte lock a user out of a perfectly good document.
void appendPreamble(std::vector<std::uint8_t>& out, std::uint8_t documentType,
                    std::uint8_t version = kFormatVersion);
[[nodiscard]] std::optional<Preamble> parsePreamble(const std::uint8_t* data,
                                                    std::size_t size) noexcept;

// --- Chunk framing (spec 2.2): fixed 46-byte header + optional 8-byte LINK + payload + checksum.
//
//   MAGIC(8) TYPE(4) FLAGS(1) PROFILE(1) KEY(16) GENERATION(8) UNCOMPRESSED_LEN(4) PAYLOAD_LEN(4)
//   | [LINK(8) -- iff FLAGS bit 1] | payload | checksum (8, or 32 for ROOT)
//
// The chunk magic is a distinct sentinel from the preamble's (a linear scan resyncing to it must
// not stop at byte 0), same trap construction, no degenerate all-0/all-1 runs.
inline constexpr std::array<std::uint8_t, 8> kChunkMagic = {0x9E, 'M', 'C', 'K',
                                                            0x0D, 0x0A, 0x1A, 0x0A};

inline constexpr std::size_t kHeaderSize = 46;
inline constexpr std::size_t kOffType = 8;
inline constexpr std::size_t kOffFlags = 12;
inline constexpr std::size_t kOffProfile = 13;
inline constexpr std::size_t kOffKey = 14;
inline constexpr std::size_t kOffGeneration = 30;
inline constexpr std::size_t kOffUncompressedLen = 38;
inline constexpr std::size_t kOffPayloadLen = 42;
inline constexpr std::size_t kLinkSize = 8;
inline constexpr std::size_t kFastChecksumSize = 8;    // xxh3-64, ordinary chunks
inline constexpr std::size_t kStrongChecksumSize = 32; // BLAKE3, ROOT chunks only

using ChunkTag = std::array<std::uint8_t, 4>;
inline constexpr ChunkTag kTypeManifest = {'M', 'F', 'S', 'T'};
inline constexpr ChunkTag kTypePreview = {'P', 'R', 'V', 'W'};
inline constexpr ChunkTag kTypeTile = {'T', 'I', 'L', 'E'};
inline constexpr ChunkTag kTypeVector = {'V', 'E', 'C', 'T'};
inline constexpr ChunkTag kTypeRoot = {'R', 'O', 'O', 'T'};
inline constexpr ChunkTag kTypeDir = {'D', 'I', 'R', ' '};
inline constexpr ChunkTag kTypeRootPtr = {'R', 'P', 'T', 'R'}; // root-slot overflow pointer
inline constexpr ChunkTag kTypeParity = {'P', 'R', 'T', 'Y'};  // Reed-Solomon parity
inline constexpr ChunkTag kTypeHist = {'H', 'I', 'S', 'T'};    // history-state commit record
inline constexpr ChunkTag kTypeBlob = {'B', 'L', 'O', 'B'};    // content-addressed (H4, Build 2)
inline constexpr ChunkTag kTypeJournalHeader = {'J', 'H', 'D', 'R'}; // recovery journal only
inline constexpr ChunkTag kTypeCommit = {'C', 'M', 'I', 'T'};  // closes one File->Save batch

// FLAGS bits. Linked = the 8-byte LINK field is present (journal frames and committed Save
// batches): the frame's checksum stands alone and the chain is validated by comparing LINK
// against the previous frame's actual checksum -- explicit-link, not a cumulative chain
// (spec 2.2, Round 11).
inline constexpr std::uint8_t kFlagCritical = 0x01;
inline constexpr std::uint8_t kFlagLinked = 0x02;
inline constexpr std::uint8_t kFlagFiltered = 0x04; // spatial predictor applied before compression
inline constexpr std::uint8_t kFlagDelta = 0x08;    // reserved for H3; unused unless it ever ships

// Compression profiles, recorded per chunk (spec 2.4). Build 1 slice 1 frames everything Store;
// LZ4/zstd arrive with the codec slice.
enum class Profile : std::uint8_t { Store = 0, Fast = 1, Balanced = 2, Max = 3 };

// --- KEY(16): the structured chunk identity (spec 2.2). Interpreted per TYPE; a chunk's logical
// identity is (TYPE, KEY), and GENERATION -- always the undo-state id that wrote the chunk --
// versions it ("highest generation wins"). Only states consume generation ids: demonstrated
// load-bearing for index-free recovery, not bookkeeping (Round 12, A5).
struct ChunkKey {
    std::array<std::uint8_t, 16> bytes{};
    friend bool operator==(const ChunkKey&, const ChunkKey&) = default;
};

[[nodiscard]] ChunkKey tileKey(std::uint64_t layerId, std::uint32_t tx, std::uint32_t ty) noexcept;
[[nodiscard]] ChunkKey vectorKey(std::uint64_t layerId) noexcept;
[[nodiscard]] ChunkKey histKey(std::uint64_t stateId) noexcept;
[[nodiscard]] ChunkKey parityKey(std::uint64_t stripeIndex) noexcept;
// MFST/PRVW/ROOT/DIR/RPTR/JHDR/CMIT are singletons: all-zero key, generation disambiguates.
[[nodiscard]] constexpr ChunkKey zeroKey() noexcept { return {}; }

} // namespace mosaic::io::native
