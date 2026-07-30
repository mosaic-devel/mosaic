#pragma once

#include "io/mosaic/format.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// mosaic/records -- the JSON payload schemas of the HIST and CMIT frames (spec 3.2, 2.6). One
// writer and one parser, shared by the Save path, the journal, replay, and salvage: the dirty
// list is the authority on which keys a lost state touched (honest salvage depends on it), so
// its spelling must not be able to drift between producers and consumers.
namespace mosaic::io::native {

// One entry of a HIST frame's dirty list: the (TYPE, KEY) identity of a chunk the state wrote.
struct DirtyKey {
    ChunkTag type{};
    ChunkKey key{};
    friend bool operator==(const DirtyKey&, const DirtyKey&) = default;
};

// A cas-mode (H4, spec 3.9) content reference riding on one dirty entry: the full BLAKE3 hash
// of the content the state wrote for that key, plus the frame flags it carried (kFlagFiltered
// -- the flags say how to INTERPRET content, and a blob shared across entries serves each
// reference with that reference's own flags). `present` false = this entry carries no
// reference: an H2 (journal-mode) record, or a PRVW entry, whose superseded content compaction
// drops rather than retains.
struct BlobRef {
    bool present = false;
    std::array<std::uint8_t, 32> hash{};
    std::uint8_t flags = 0;
    friend bool operator==(const BlobRef&, const BlobRef&) = default;
};

// The canonical HIST fields. `parent` is stored even though it is always literally state-1
// under linear undo -- a free redundant consistency check (spec 3.2). Document-layer fields
// (op/params/manifest_snapshot) merge in through `extraJson` and ride along untouched.
// `refs`, when non-empty, is dirty.size() entries -- the cas-mode spelling; each ref is
// written INSIDE its own dirty entry on the wire, so the two lists cannot skew apart.
struct HistRecord {
    std::uint64_t state = 0;
    std::uint64_t parent = 0;
    std::vector<DirtyKey> dirty;
    std::vector<BlobRef> refs;
};

[[nodiscard]] std::string histRecordJson(const HistRecord& rec,
                                         const std::string& extraJson = {});
[[nodiscard]] std::optional<HistRecord> parseHistRecord(std::span<const std::uint8_t> payload);

// The non-canonical fields of an existing HIST payload (op/params/manifest_snapshot -- whatever
// the document layer wrote), as a JSON object string ready for histRecordJson's extraJson.
// Empty when there are none. This is what lets a mode switch re-spell a record's dirty list
// (adding or dropping refs) without losing fields this layer does not own.
[[nodiscard]] std::string histExtrasJson(std::span<const std::uint8_t> payload);

// The CMIT frame closing one File->Save batch (spec 2.6): the saved state id plus the batch's
// full state list, letting replay cross-check the batch it just buffered.
struct CmitRecord {
    std::uint64_t savedState = 0;
    std::vector<std::uint64_t> batchStates;
};

[[nodiscard]] std::string cmitRecordJson(const CmitRecord& rec);
[[nodiscard]] std::optional<CmitRecord> parseCmitRecord(std::span<const std::uint8_t> payload);

} // namespace mosaic::io::native
