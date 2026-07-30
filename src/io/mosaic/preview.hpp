#pragma once

#include "common/image.hpp"
#include "io/mosaic/save.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// mosaic/preview -- the PRVW chunk (S48-b): an embedded thumbnail of the document's composite,
// 256px longest edge (freedesktop "large", so 128 and 256 requests both DOWNSCALE rather than
// upscale), RGBA, Paeth-filtered, Profile::Max -- the first real consumer of the profile §2.4
// earmarked for written-once-read-many content.
//
// A preview is a DERIVED artifact, never document content. Three rules follow, none optional:
//   - buildLoadedHistory / loadedStates SKIP PRVW dirty keys (ui/loaded_history.cpp);
//   - applyChunksToDocument never receives one (it counts an arriving PRVW as rejected);
//   - compaction DROPS superseded previews instead of retaining them as undo states
//     (compaction.cpp) -- which is exactly why the history walk must skip them at the source.
//
// The COMPOSITE comes from above: io must not depend on render, so the app owns the compositor
// and passes the image down through buildDocumentCheckpoint (docio.hpp). Emitted as an ordinary
// chunk, diffDocumentStates decides when a Save actually writes one: an edit too small to alter
// the downscale serializes byte-identically and costs nothing.
namespace mosaic::io::native {

// Longest edge of a written preview. Matches ui::kFileThumbnailEdge by design, not by include.
inline constexpr std::uint32_t kPreviewEdge = 256;

// Payload spelling: LE32 width, LE32 height, then width*height*4 Paeth-filtered RGBA bytes.
inline constexpr std::size_t kPreviewHeaderSize = 8;

// Reader-side sanity cap on the declared dimensions (a hostile header must not be able to ask
// for gigabytes; same philosophy as codec.hpp's kMaxUncompressedLen). Generous: real writers
// emit kPreviewEdge.
inline constexpr std::uint32_t kMaxPreviewDim = 4096;

// Area-averaged downscale so `src` fits maxEdge on its longest edge, preserving aspect; never
// upscales (a small canvas ships at its own size). Alpha-weighted -- the accumulation is
// premultiplied, so the undefined RGB of fully transparent pixels cannot bleed into edges the
// way a straight per-channel mean would let it.
[[nodiscard]] common::Image downscalePreview(const common::Image& src, std::uint32_t maxEdge);

// The payload codec. Encode filters in place of returning raw pixels (the chunk rides
// kFlagFiltered, like rgba8 TILEs); decode validates the header against the payload size and
// never trusts either. nullopt on anything off-contract.
[[nodiscard]] std::vector<std::uint8_t> encodePreviewPayload(const common::Image& img);
[[nodiscard]] std::optional<common::Image> decodePreviewPayload(
    std::span<const std::uint8_t> payload);

// The PRVW chunk buildDocumentCheckpoint emits: `composite` (any size; downscaled here) becomes
// a singleton-key, Profile::Max, Paeth-filtered chunk. NOT parity-covered -- spec 2.7 stripes
// tile/vector content only, and a lost preview is a regenerate, never damage.
[[nodiscard]] FileChunk makePreviewChunk(const common::Image& composite);

// Seed a freshly built baseline with the newest PRVW an opened file already carries, so the
// next Save's diff compares the new composite's downscale against what is actually ON DISK --
// byte-identical means no preview is written, even across sessions. Flags are normalized (a
// committed-region frame carries kFlagLinked on the wire) so the compare cannot trip on
// transport bits. A preview-less file seeds nothing.
void seedPreviewFromReport(CheckpointInput& in, const OpenReport& report);

// The newest PRVW in a .mosaic file image, decoded -- highest generation wins, exactly the rule
// the container resolves every chunk by. A linear verified scan that decompresses ONLY preview
// frames: the New Document dialog's recents and the thumbnailer both call this once per file,
// so it must not pay for the document's tiles. nullopt when no readable preview exists (a
// pre-S48-b file, or damage took every copy).
[[nodiscard]] std::optional<common::Image> newestPreviewInFile(
    std::span<const std::uint8_t> file);

// newestPreviewInFile over a path. `error`, when non-null, says why there is nothing to show.
[[nodiscard]] std::optional<common::Image> readNewestPreview(const std::string& path,
                                                             std::string* error = nullptr);

} // namespace mosaic::io::native
