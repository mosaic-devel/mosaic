#include "ui/loaded_history.hpp"

#include "core/commands.hpp" // LoadedStateCommand (whole-tree fallback)
#include "core/document.hpp"
#include "core/layer.hpp"
#include "io/mosaic/blob.hpp"  // cas-mode reference resolution (spec 3.9)
#include "io/mosaic/docio.hpp" // documentFromReport, StateChunk, kMaskSurfaceBit
#include "io/mosaic/format.hpp"
#include "io/mosaic/records.hpp" // parseHistRecord
#include "ui/loaded_delta_command.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

namespace mosaic::ui {
namespace {

namespace nio = io::native;

using ContentKey = std::pair<nio::ChunkTag, std::array<std::uint8_t, 16>>;
using VersionKey = std::tuple<nio::ChunkTag, std::array<std::uint8_t, 16>, std::uint64_t>;

// The raw owner id a chunk key carries (LE64 at byte 0; a mask surface sets bit 63).
std::uint64_t rawOwner(const nio::ChunkKey& k) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(k.bytes[static_cast<std::size_t>(i)]) << (8 * i);
    return v;
}

// Every frame an opened file offers, wherever it lives: current content, the checkpoint's retained
// history (post-compaction), or the committed append region.
std::vector<const nio::RecoveredChunk*> allFrames(const nio::OpenReport& r) {
    std::vector<const nio::RecoveredChunk*> out;
    out.reserve(r.base.chunks.size() + r.base.retained.size() + r.committed.size());
    for (const std::vector<nio::RecoveredChunk>* list : {&r.base.chunks, &r.base.retained,
                                                         &r.committed})
        for (const nio::RecoveredChunk& c : *list)
            out.push_back(&c);
    return out;
}

// The FULL layer tree of the state below `generation`: seed values (every key's newest frame
// below the oldest saved state) advanced through each resolved state older than the target --
// the whole-tree fallback for a structural/mask step. Composing through the STATES, not by a
// raw generation filter over frames, is what makes this work for cas-encoded history too, where
// a superseded value has no per-(KEY, generation) frame of its own. nullopt on failure.
std::optional<std::vector<std::unique_ptr<core::Layer>>>
belowTreeSnapshot(const nio::OpenReport& report, const std::vector<LoadedState>& states,
                  std::uint64_t generation) {
    // Seed: newest frame per key below the oldest state (identical to buildLoadedHistory's walk
    // seed), then apply each older state's after-images in order.
    struct Value {
        std::uint8_t flags = 0;
        std::uint64_t generation = 0;
        std::span<const std::uint8_t> payload;
    };
    const std::uint64_t firstState = states.empty() ? generation : states.front().generation;
    std::map<ContentKey, Value> composed;
    for (const nio::RecoveredChunk* c : allFrames(report)) {
        if (c->type == nio::kTypeHist || c->type == nio::kTypeBlob ||
            c->generation >= firstState)
            continue;
        Value& v = composed[{c->type, c->key.bytes}];
        if (v.payload.data() == nullptr || c->generation > v.generation)
            v = {c->flags, c->generation, c->payload};
    }
    for (const LoadedState& st : states) {
        if (st.generation >= generation)
            break;
        for (const LoadedChunk& c : st.chunks)
            composed[{c.type, c.key.bytes}] = {c.flags, st.generation, c.payload};
    }

    nio::OpenReport synth = report;
    synth.base.chunks.clear();
    synth.base.retained.clear();
    synth.committed.clear();
    synth.commits.clear();
    for (const auto& [key, v] : composed) {
        nio::RecoveredChunk c;
        c.type = key.first;
        c.key.bytes = key.second;
        c.generation = v.generation;
        c.flags = v.flags;
        c.payload.assign(v.payload.begin(), v.payload.end());
        synth.base.chunks.push_back(std::move(c));
    }
    auto snapshot = nio::documentFromReport(synth);
    if (!snapshot.has_value())
        return std::nullopt;
    std::vector<std::unique_ptr<core::Layer>> below;
    core::GroupLayer& sroot = snapshot->document->root();
    below.reserve(sroot.childCount());
    while (sroot.childCount() > 0)
        below.insert(below.begin(), sroot.removeAt(sroot.childCount() - 1));
    return below;
}

} // namespace

std::optional<std::vector<LoadedState>> loadedStates(const nio::OpenReport& report) {
    // ANY unreadable retained frame makes the whole walk untrustworthy, and the reader cannot say
    // which kind was lost:
    //   - a state's dirty chunk  -> that step has no "after" value
    //   - a state's HIST record  -> the state vanishes, and the step above it undoes to the value
    //                               below the GAP instead of to this state's
    //   - a frame no HIST names  -> the base state's value for a key some save later overwrote:
    //                               the seed below. Its absence reads as "this key was blank", so
    //                               undoing the first save that touched it would CLEAR the tile
    //                               rather than restore it.
    // That last one has no dirty-list to check it against, which is exactly why the rule is drawn
    // here and not per-key: a partial history moves the document to content no state ever held, and
    // a silently wrong undo is worse than no undo.
    if (report.base.lostHistoryEntries > 0)
        return std::nullopt;

    // HIST records are the authority on which states exist and what each of them dirtied. They are
    // read from the committed region and from retained history -- NEVER from base.chunks: a
    // full-scan open leaves HIST frames there while collapsing the superseded content away, so its
    // "states" would every one of them resolve to today's pixels.
    std::map<std::uint64_t, nio::HistRecord> records;
    bool anyRefs = false;
    bool anyOpaque = false;
    const auto collect = [&](const std::vector<nio::RecoveredChunk>& list) {
        for (const nio::RecoveredChunk& c : list) {
            if (c.type != nio::kTypeHist)
                continue;
            auto rec = nio::parseHistRecord(c.payload);
            if (!rec.has_value()) {
                // The frame VERIFIED but will not parse: a state we know exists and cannot read.
                // Same verdict as a lost record -- any unreadable history declines the WHOLE
                // walk, because a walk that skips the state moves the document through content
                // its neighbours never held (exactly the lostHistoryEntries rule above).
                anyOpaque = true;
                continue;
            }
            anyRefs = anyRefs || !rec->refs.empty();
            const std::uint64_t state = rec->state;
            records[state] = std::move(*rec);
        }
    };
    collect(report.base.retained);
    collect(report.committed);
    if (anyOpaque)
        return std::nullopt;
    if (records.empty())
        return std::vector<LoadedState>{}; // no history at all -- not the same as unreadable

    // A journal-mode (H2) entry's value is the frame that state wrote, wherever it ended up:
    // current content when nothing superseded it, retained history when something did.
    std::map<VersionKey, const nio::RecoveredChunk*> byVersion;
    for (const nio::RecoveredChunk* c : allFrames(report))
        if (c->type != nio::kTypeHist && c->type != nio::kTypeBlob)
            byVersion[{c->type, c->key.bytes, c->generation}] = c;

    // A cas-mode (H4) entry's value is content by hash (spec 3.9): a BLOB chunk, or -- when the
    // content is what some current frame holds -- that frame itself, which is exactly how the
    // writer deduplicates against current content. Keyed by COMPUTED hash, so a lying blob (a
    // stored head hash its content does not produce) resolves nothing rather than the wrong
    // bytes. Built only when some record actually carries references; a journal-mode file never
    // pays for it.
    struct Content {
        std::uint8_t flags = 0;
        std::span<const std::uint8_t> payload;
    };
    std::map<nio::BlobHash, Content> byHash;
    if (anyRefs) {
        for (const nio::RecoveredChunk* c : allFrames(report)) {
            if (c->type == nio::kTypeHist)
                continue;
            if (c->type == nio::kTypeBlob) {
                if (const auto content = nio::blobContentOf(c->payload); content.has_value()) {
                    nio::BlobHash h{};
                    std::copy(c->payload.begin(), c->payload.begin() + nio::kBlobHashSize,
                              h.begin());
                    byHash[h] = {static_cast<std::uint8_t>(c->flags & ~nio::kFlagLinked),
                                 *content};
                }
                continue;
            }
            byHash[nio::blobHashOf(c->payload)] = {
                static_cast<std::uint8_t>(c->flags & ~nio::kFlagLinked), c->payload};
        }
    }

    std::vector<LoadedState> states;
    states.reserve(records.size());
    for (const auto& [generation, rec] : records) {
        LoadedState st;
        st.generation = generation;
        for (std::size_t i = 0; i < rec.dirty.size(); ++i) {
            const nio::DirtyKey& d = rec.dirty[i];
            // S48-b hard rule: PRVW dirty keys are SKIPPED. A preview is a derived artifact, not
            // document content -- applyChunksToDocument must never receive one -- and compaction
            // DROPS superseded previews, so resolving one here would declare every compacted
            // file's history unreadable over a frame that was discarded on purpose.
            if (d.type == nio::kTypePreview)
                continue;
            const bool hasRef = i < rec.refs.size() && rec.refs[i].present;
            if (hasRef) {
                const auto it = byHash.find(rec.refs[i].hash);
                if (it == byHash.end())
                    return std::nullopt; // the referenced content did not survive: same verdict
                                         // as a missing frame -- partial history is not an option
                st.chunks.push_back({d.type, d.key, rec.refs[i].flags, it->second.payload});
            } else {
                const auto it = byVersion.find({d.type, d.key.bytes, generation});
                if (it == byVersion.end())
                    return std::nullopt; // a dirty frame did not survive: every step past it
                                         // would show content that state never held. A later
                                         // step's undo target IS this missing frame.
                st.chunks.push_back({d.type, d.key,
                                     static_cast<std::uint8_t>(it->second->flags &
                                                               ~nio::kFlagLinked),
                                     it->second->payload});
            }
        }
        states.push_back(std::move(st));
    }
    return states;
}

std::vector<std::unique_ptr<core::Command>>
buildLoadedHistory(const nio::OpenReport& report,
                   const std::function<std::string(std::uint64_t)>& label) {
    std::vector<std::unique_ptr<core::Command>> history;
    const auto resolved = loadedStates(report);
    if (!resolved.has_value() || resolved->empty())
        return history;
    const std::vector<LoadedState>& states = *resolved;

    // The value each key held BELOW the oldest saved state -- its newest frame older than that
    // state -- so a per-key step can read each touched key's undo target. On an un-compacted file
    // every checkpoint frame predates every save, making this exactly base.chunks; on a compacted
    // one the base state's frames are retained history, and its untouched keys are still current.
    struct Value {
        std::uint8_t flags = 0;
        std::uint64_t generation = 0;
        std::span<const std::uint8_t> payload;
    };
    const std::uint64_t firstState = states.front().generation;
    std::map<ContentKey, Value> current;
    for (const nio::RecoveredChunk* c : allFrames(report)) {
        if (c->type == nio::kTypeHist || c->type == nio::kTypeBlob ||
            c->generation >= firstState)
            continue;
        Value& v = current[{c->type, c->key.bytes}];
        if (v.payload.data() == nullptr || c->generation > v.generation)
            v = {c->flags, c->generation, c->payload};
    }

    history.reserve(states.size());
    for (const LoadedState& st : states) {
        // A re-emitted manifest or a mask-surface tile marks the step structural -> whole-tree
        // fallback (applyChunksToDocument patches content surfaces only).
        bool structural = false;
        for (const LoadedChunk& c : st.chunks)
            if (c.type == nio::kTypeManifest ||
                (c.type == nio::kTypeTile && (rawOwner(c.key) & nio::kMaskSurfaceBit) != 0))
                structural = true;

        if (structural) {
            auto below = belowTreeSnapshot(report, states, st.generation);
            if (!below.has_value())
                return {}; // history is a bonus: a reconstruction failure leaves the panel empty
            history.push_back(
                std::make_unique<core::LoadedStateCommand>(label(st.generation), std::move(*below)));
        } else {
            std::vector<nio::StateChunk> before, after;
            before.reserve(st.chunks.size());
            after.reserve(st.chunks.size());
            for (const LoadedChunk& c : st.chunks) {
                after.push_back({c.type, c.key,
                                 std::vector<std::uint8_t>(c.payload.begin(), c.payload.end()),
                                 c.flags});
                const auto it = current.find({c.type, c.key.bytes});
                if (it != current.end())
                    before.push_back({c.type, c.key,
                                      std::vector<std::uint8_t>(it->second.payload.begin(),
                                                                it->second.payload.end()),
                                      it->second.flags});
                else
                    // The key was blank below this save (a tile first painted here): an empty
                    // payload tells applyChunksToDocument to clear the cell to its default.
                    before.push_back({c.type, c.key, {}, 0});
            }
            history.push_back(std::make_unique<LoadedDeltaCommand>(label(st.generation),
                                                                   std::move(before),
                                                                   std::move(after)));
        }
        // Advance `current` so the next save's before-lookups see this save's values.
        for (const LoadedChunk& c : st.chunks)
            current[{c.type, c.key.bytes}] = {c.flags, st.generation, c.payload};
    }
    return history;
}

} // namespace mosaic::ui
