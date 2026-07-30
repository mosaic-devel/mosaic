#pragma once

#include "core/document.hpp"
#include "io/mosaic/save.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>

// mosaic/docio -- the document<->container bridge (S48 Build 1, document-model slice): a
// core::Document serializes into typed chunks (spec 2.2) and back.
//
//   MFST  one JSON manifest: canvas/color/uuid/title, the id allocator, the surface table, and
//         the full layer tree with every piece of chrome. The kind-specific payloads that are
//         JSON-friendly (adjustment params) ride inline; pixel and per-layer payloads go to
//         their own chunks below.
//   TILE  sparse 64px tiles of every SURFACE: raster layer content (owner id = the layer id),
//         magic-layer sources (same), and layer masks (owner id = layer id | kMaskSurfaceBit).
//         rgba8 tiles are Paeth-filtered (kFlagFiltered); a8 mask tiles are stored raw. An
//         absent tile means "transparent" for rgba8 and "fully visible" (255) for a8 -- so a
//         mostly-empty layer costs almost nothing, and a state's dirty set later (H2) stays
//         O(edit), never O(canvas).
//   VECT  one per vector/text layer, keyed vectorKey(layerId): the vec::Object or
//         text::TextBlock as JSON (docjson.hpp spellings).
//
// NOT persisted, by design: selection (session state), text render caches and reflection
// environments (recomputed), cached content bounds (recomputed). Undo history (HIST chunks) is
// the H2 slice -- this bridge carries the CURRENT content, and the container layers underneath
// already know how to retain history around it.
namespace mosaic::io::native {

inline constexpr std::uint32_t kTileSize = 64; // spec 2.2, Round 10-measured

// TILE owner-id space: a layer's mask surface shares the layer's u64 id with bit 63 set. Layer
// ids are minted monotonically from 1, so the bit is unreachable by real ids for any realistic
// document lifetime; ImagePattern tiles ride inline in their paint JSON instead (small,
// immutable), so layers and masks are the only two owner families.
inline constexpr std::uint64_t kMaskSurfaceBit = std::uint64_t{1} << 63;

// A fresh 128-bit random identity, hex-formatted (8-4-4-4-12). Minted once per document (at
// first save or creation) and stable forever after -- the journal and lock key on it.
[[nodiscard]] std::string mintDocumentUuid();

// Serialize the document's current content. The document must carry a non-empty uuid (the
// caller mints one first; this function never invents identity silently). CheckpointInput is
// what buildCheckpoint (file.hpp) consumes for the full write.
//
// `preview`, when non-null and non-empty, is the document's COMPOSITE, supplied by the app --
// io must not depend on render, so the compositor's output arrives as a parameter (S48-b). It
// becomes an ordinary PRVW chunk (preview.hpp: 256px longest edge, Paeth, Profile::Max), so
// diffDocumentStates decides when a Save actually writes one: an unchanged downscale
// serializes byte-identically and costs nothing.
[[nodiscard]] std::optional<CheckpointInput>
buildDocumentCheckpoint(const core::Document& doc, std::string* error = nullptr,
                        const common::Image* preview = nullptr);

struct DocumentReadResult {
    std::unique_ptr<core::Document> document;
    std::string uuid;
    // Honesty counters: content the container recovered but this layer had to reject (a tile
    // whose payload size disagrees with its grid slot, an unparseable VECT payload). The
    // affected regions read transparent/default rather than wrong; the caller reports, never
    // hides.
    std::size_t rejectedChunks = 0;
};

// Rebuild a document from an opened file's recovered chunks (openDocument, save.hpp). Strict on
// structure (a malformed manifest fails the read with `error`), tolerant on content (damaged
// tiles are counted in rejectedChunks and read as their surface default). Layer ids, the id
// allocator, and the uuid survive round-trips exactly -- tile keys and the undo model depend
// on it.
[[nodiscard]] std::optional<DocumentReadResult> documentFromReport(const OpenReport& report,
                                                                   std::string* error = nullptr);

// Apply a set of content deltas onto an EXISTING document, IN PLACE -- the per-key LiveUndoModel
// step (spec 3.5): each TILE patches its 64px cell of the owning pixel surface (an EMPTY payload
// clears the cell to the surface default -- the "this key was blank at the target state" case),
// each VECT replaces its layer's geometry, and the touched layers' content revisions bump so the
// compositor re-reads them. Deliberately narrow: MFST and mask-surface tiles are NOT handled here
// (a step that changes document structure or a mask is walked by the whole-tree LoadedStateCommand
// instead), so this only ever resolves raster/magic content surfaces and vector/text objects.
// Damaged or unresolvable chunks are counted in `rejected` (never applied wrong), never fatal --
// loadCommittedHistory guarantees the delta it hands over is resolvable, so a non-zero count is a
// reader/writer drift bug worth surfacing in tests.
void applyChunksToDocument(core::Document& doc, std::span<const StateChunk> chunks,
                           std::size_t* rejected = nullptr);

} // namespace mosaic::io::native
