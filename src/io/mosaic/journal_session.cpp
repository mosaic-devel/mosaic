#include "io/mosaic/journal_session.hpp"

#include "common/fs_path.hpp"
#include "io/mosaic/records.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <utility>

namespace mosaic::io::native {
namespace {

[[nodiscard]] std::span<const std::uint8_t> stringBytes(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

} // namespace

JournalBinding bindingForTip(const std::string& uuid, const std::string& path,
                             const CommitTip& tip) {
    JournalBinding b;
    b.documentUuid = uuid;
    b.documentPath = path;
    b.commitId = tip.commitId;
    b.checksum = tip.checksum;
    b.checksumSize = tip.checksumSize;
    return b;
}

JournalBinding selfContainedBinding(const std::string& uuid, const std::string& path) {
    JournalBinding b;
    b.documentUuid = uuid;
    b.documentPath = path;
    b.commitId = 0;
    b.checksum = {}; // all-zero
    b.checksumSize = kLinkSize;
    return b;
}

std::vector<StateChunk> diffDocumentStates(const CheckpointInput& prev,
                                           const CheckpointInput& curr) {
    using Key = std::pair<ChunkTag, std::array<std::uint8_t, 16>>;
    std::map<Key, const FileChunk*> prevMap;
    for (const FileChunk& c : prev.chunks)
        prevMap[{c.type, c.key.bytes}] = &c;
    std::map<Key, const FileChunk*> currMap;
    for (const FileChunk& c : curr.chunks)
        currMap[{c.type, c.key.bytes}] = &c;

    std::vector<StateChunk> out;
    // Changed or newly present keys carry the current bytes.
    for (const FileChunk& c : curr.chunks) {
        const auto it = prevMap.find({c.type, c.key.bytes});
        if (it != prevMap.end() && it->second->flags == c.flags &&
            it->second->payload == c.payload)
            continue; // byte-identical: not dirty
        StateChunk s;
        s.type = c.type;
        s.key = c.key;
        s.payload = c.payload;
        s.flags = c.flags;
        out.push_back(std::move(s));
    }
    // A TILE that vanished (erased back to the surface default while its layer persists) needs an
    // explicit tombstone -- its committed painted generation would otherwise win the composition.
    for (const FileChunk& c : prev.chunks) {
        if (c.type != kTypeTile)
            continue;
        if (currMap.count({c.type, c.key.bytes}) != 0)
            continue;
        StateChunk s;
        s.type = kTypeTile;
        s.key = c.key;
        // rgba8 tiles are Paeth-filtered (transparent = 0); a8 masks are raw (fully visible = 255).
        // The blank tile is stored raw either way -- a predictor buys nothing on a constant tile.
        const std::uint8_t fill = (c.flags & kFlagFiltered) != 0 ? 0x00 : 0xFF;
        s.payload.assign(c.payload.size(), fill);
        s.flags = kFlagCritical;
        out.push_back(std::move(s));
    }
    return out;
}

std::optional<JournalSession> JournalSession::begin(const std::string& path,
                                                    const JournalBinding& binding,
                                                    std::optional<CheckpointInput> baseline,
                                                    std::uint64_t firstState, std::string* error,
                                                    std::uint64_t compactMinBytes) {
    // A crash mid-growth-compaction can leave a torn temp behind; it is dead weight (the rename
    // never happened, so the real journal replayed fine) and must not silt up the recovery dir.
    // The wide form on Windows for the usual reason: this path sits under %LOCALAPPDATA%, so an
    // account name outside the active code page would make the narrow remove miss the file and the
    // temp would silt up forever -- the exact thing this line exists to prevent.
#if defined(_WIN32)
    ::_wremove(common::pathFromUtf8(path + kJournalCompactSuffix).c_str());
#else
    std::remove((path + kJournalCompactSuffix).c_str());
#endif
    auto w = JournalWriter::create(path, binding, error);
    if (!w.has_value())
        return std::nullopt;
    JournalSession s;
    s.writer_ = std::move(*w);
    s.binding_ = binding;
    if (baseline.has_value())
        s.baseline_ = std::move(*baseline);
    s.nextState_ = firstState;
    s.firstState_ = firstState;
    s.compactMinBytes_ = compactMinBytes;
    return s;
}

bool JournalSession::autosave(const core::Document& doc, std::string* error) {
    if (!alive())
        return true; // a dead session no-ops until the next open re-creates one

    auto built = buildDocumentCheckpoint(doc, error);
    if (!built.has_value()) {
        dead_ = true; // cannot serialize the document -> stop journaling, do not spin
        return false;
    }
    std::vector<StateChunk> dirty = diffDocumentStates(baseline_, *built);
    if (dirty.empty())
        return true; // nothing new since the last durable state

    const std::uint64_t stateId = nextState_;
    SaveState st;
    st.stateId = stateId;
    st.chunks = std::move(dirty);

    JournalWriter& w = *writer_;
    const auto fail = [&](const char* what) {
        dead_ = true;
        if (error != nullptr)
            *error = what;
        return false;
    };
    for (const StateChunk& c : st.chunks)
        if (!w.append(c.type, c.key, stateId, c.payload, Profile::Fast, c.flags))
            return fail("journal content append failed");
    const std::string hist = histPayloadFor(st);
    if (!w.append(kTypeHist, histKey(stateId), stateId, stringBytes(hist), Profile::Store))
        return fail("journal HIST append failed");
    if (!w.sync())
        return fail("journal sync failed");

    baseline_ = std::move(*built);
    ++nextState_;
    // The live working set behind growth compaction: the newest value per dirtied key.
    for (StateChunk& c : st.chunks)
        lastWritten_[{c.type, c.key.bytes}] = std::move(c);
    // Best-effort: a compaction that cannot run leaves a big-but-correct journal; the session
    // stays alive and the durable state above is untouched either way.
    (void)maybeCompact(nullptr);
    return true;
}

bool JournalSession::maybeCompact(std::string* error) {
    const std::uint64_t size = writer_->sizeBytes();
    if (size < compactMinBytes_ || size < compactBackoffSize_)
        return true;
    // Worth it only when the journal is genuinely bigger than the session it protects: the
    // working-set estimate is uncompressed bytes while the file is LZ4-framed, so the comparison
    // errs toward compacting LATER, never toward a rewrite that saves nothing.
    std::uint64_t estimate = 0;
    for (const auto& [key, c] : lastWritten_)
        estimate += c.payload.size();
    if (static_cast<double>(size) <= kJournalCompactGain * static_cast<double>(estimate))
        return true;

    // One cumulative state at the FIRST autosaved id (it was consumed; later states keep their
    // ids, so nothing ever collides): every dirtied key at its newest value. Tombstones ride
    // along as the blank tiles diffDocumentStates already spelled them as.
    SaveState st;
    st.stateId = firstState_;
    for (const auto& [key, c] : lastWritten_)
        st.chunks.push_back(c);

    const std::string tmp = writer_->path() + kJournalCompactSuffix;
    auto fresh = JournalWriter::create(tmp, binding_, error);
    if (!fresh.has_value()) {
        compactBackoffSize_ = size * 2; // do not retry an expensive rewrite every autosave
        return false;
    }
    const auto fail = [&](const char* what) {
        fresh->discard(); // close + remove the temp; the live journal is untouched
        compactBackoffSize_ = size * 2;
        if (error != nullptr)
            *error = what;
        return false;
    };
    for (const StateChunk& c : st.chunks)
        if (!fresh->append(c.type, c.key, st.stateId, c.payload, Profile::Fast, c.flags))
            return fail("journal compaction append failed");
    const std::string hist = histPayloadFor(st);
    if (!fresh->append(kTypeHist, histKey(st.stateId), st.stateId, stringBytes(hist),
                       Profile::Store))
        return fail("journal compaction HIST append failed");
    if (!fresh->sync()) // durable BEFORE the rename: the swap must never install torn frames
        return fail("journal compaction sync failed");
    if (!writer_->replaceWith(std::move(*fresh))) {
        compactBackoffSize_ = size * 2;
        if (error != nullptr)
            *error = "could not swap the compacted journal in";
        return false;
    }
    compactBackoffSize_ = 0;
    return true;
}

void JournalSession::discard() {
    if (writer_.has_value())
        writer_->discard();
    writer_.reset();
    dead_ = true;
}

} // namespace mosaic::io::native
