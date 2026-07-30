#include "io/mosaic/compaction.hpp"

#include "io/mosaic/records.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace mosaic::io::native {
namespace {

using ContentKey = std::pair<ChunkTag, std::array<std::uint8_t, 16>>;
using VersionKey = std::tuple<ChunkTag, std::array<std::uint8_t, 16>, std::uint64_t>;

// Spec 2.7: parity stripes current tile/vector content; the manifest and preview ride uncovered
// (the manifest is replicated instead, spec 2.3). This must agree with what
// buildDocumentCheckpoint chooses, or a compaction full write and a Save As full write of the
// same document would disagree about what parity covers.
[[nodiscard]] bool parityCovered(const ChunkTag& t) noexcept {
    return t == kTypeTile || t == kTypeVector;
}

// A frame that survived the read is trustworthy -- its checksum verified on the way in -- so it
// may be spliced byte-verbatim while its bytes still exist (spec 3.3). A parity-rebuilt frame
// exists nowhere on disk (frameLen 0) and is re-encoded from that verified payload instead.
[[nodiscard]] FileChunk carry(std::span<const std::uint8_t> file, const RecoveredChunk& c,
                              bool parity, bool history, std::size_t& reEncoded) {
    if (c.frameLen > 0 && c.frameOffset + c.frameLen <= file.size())
        if (auto v = makeVerbatimChunk(file.subspan(c.frameOffset, c.frameLen), parity, history))
            return std::move(*v);
    ++reEncoded;
    FileChunk f;
    f.type = c.type;
    f.key = c.key;
    f.generation = c.generation;
    f.profile = Profile::Balanced;
    f.flags = c.flags; // kFlagFiltered rides along: the payload is filtered, not unfiltered
    f.parity = parity;
    f.history = history;
    f.payload = c.payload;
    return f;
}

[[nodiscard]] FileChunk freshChunk(ChunkTag type, const ChunkKey& key, std::uint64_t generation,
                                   std::uint8_t flags, std::vector<std::uint8_t> payload,
                                   bool parity, bool history) {
    FileChunk f;
    f.type = type;
    f.key = key;
    f.generation = generation;
    f.profile = Profile::Balanced;
    f.flags = flags;
    f.parity = parity;
    f.history = history;
    f.payload = std::move(payload);
    return f;
}

// One retained state, as read: the HIST frame it came from plus its parsed record.
struct ParsedState {
    const RecoveredChunk* frame = nullptr;
    HistRecord rec;
    bool fromRetained = false; // lives in the checkpoint's retained history (vs committed)
};

// One resolved after-image: what one state's dirty entry actually wrote. Content spans point
// into the open report's (or a blob frame's) payload and stay valid as long as it does.
struct Instance {
    std::size_t entry = 0; // index into the state's dirty list
    BlobHash hash{};
    std::uint8_t flags = 0;
    std::span<const std::uint8_t> content;
    const RecoveredChunk* frame = nullptr;     // the per-(KEY, generation) frame, when one exists
    const RecoveredChunk* blobFrame = nullptr; // the source BLOB frame, when hash-resolved to one
};

// Every retained state of an opened file, resolved to content. Shared by the fold and by the
// app's live churn signal (churnFromOpen) so the two can never disagree on what "the retained
// history" means.
struct HistoryResolution {
    std::map<std::uint64_t, ParsedState> states;
    std::vector<const RecoveredChunk*> opaqueHists; // HIST frames that would not parse
    std::map<BlobHash, const RecoveredChunk*> blobSources;
    std::map<std::uint64_t, std::vector<Instance>> instances; // per state, dirty-list order
    // Every (TYPE, KEY, generation) some state's dirty list names -- the after-images. A
    // superseded frame OUTSIDE this set is a seed: the base value below the oldest state, which
    // no reference can reach and which must therefore stay a frame in every mode.
    std::set<VersionKey> named;
    bool fullyResolved = true; // every non-PRVW entry of every parsed state reached its content
};

// The hash of a content payload, computed once per frame.
class FrameHasher {
public:
    [[nodiscard]] const BlobHash& of(const RecoveredChunk& c) {
        auto it = cache_.find(&c);
        if (it == cache_.end())
            it = cache_.emplace(&c, blobHashOf(c.payload)).first;
        return it->second;
    }

private:
    std::map<const RecoveredChunk*, BlobHash> cache_;
};

// Resolve every retained state's after-images. Hash references resolve against every surviving
// content frame -- current, retained, or committed -- exactly as the reader does: a reference
// that deduplicated against a frame at write time keeps resolving after appends supersede that
// frame, because the frame itself is still in the file.
[[nodiscard]] HistoryResolution resolveHistory(const OpenReport& open, FrameHasher& hasher) {
    HistoryResolution r;

    const auto offerState = [&](const RecoveredChunk& c, bool fromRetained) {
        auto rec = parseHistRecord(c.payload);
        if (!rec.has_value()) {
            r.opaqueHists.push_back(&c); // carried blind, never invented
            return;
        }
        const std::uint64_t state = rec->state;
        r.states[state] = ParsedState{&c, std::move(*rec), fromRetained};
    };
    for (const RecoveredChunk& c : open.base.retained) {
        if (c.type == kTypeHist) {
            offerState(c, true);
        } else if (c.type == kTypeBlob) {
            if (const auto content = blobContentOf(c.payload); content.has_value()) {
                BlobHash h{};
                std::copy(c.payload.begin(), c.payload.begin() + kBlobHashSize, h.begin());
                r.blobSources.emplace(h, &c);
            }
            // A blob whose stored hash disagrees with its content is unusable: references to it
            // stay unresolved below, which is the honest outcome.
        }
    }
    for (const RecoveredChunk& c : open.committed)
        if (c.type == kTypeHist)
            offerState(c, false);
    // Belt and braces: a HIST in base.chunks means a full-scan open collapsed the regions; such
    // an open has no separable history (loadedStates reads retained + committed only), and the
    // caller's fold declines before ever getting here (no directory implies no verified... a
    // verified root with a destroyed directory drops to full scan too, so check the flag).
    if (open.base.usedFullScan)
        return r;

    // The per-(KEY, generation) frame index, over every region a state's frame can live in, and
    // the hash index over the same frames (built lazily -- a journal-mode file with no
    // references never pays for hashing).
    std::map<VersionKey, const RecoveredChunk*> byVersion;
    std::vector<const RecoveredChunk*> contentFrames;
    const auto index = [&](const std::vector<RecoveredChunk>& list) {
        for (const RecoveredChunk& c : list)
            if (c.type != kTypeHist && c.type != kTypeBlob) {
                byVersion[{c.type, c.key.bytes, c.generation}] = &c;
                contentFrames.push_back(&c);
            }
    };
    index(open.base.chunks);
    index(open.base.retained);
    index(open.committed);
    std::map<BlobHash, const RecoveredChunk*> byHash;
    bool hashed = false;
    const auto frameByHash = [&](const BlobHash& h) -> const RecoveredChunk* {
        if (!hashed) {
            hashed = true;
            for (const RecoveredChunk* c : contentFrames)
                byHash[hasher.of(*c)] = c;
        }
        const auto it = byHash.find(h);
        return it == byHash.end() ? nullptr : it->second;
    };

    for (auto& [gen, st] : r.states) {
        std::vector<Instance>& out = r.instances[gen];
        for (std::size_t i = 0; i < st.rec.dirty.size(); ++i) {
            const DirtyKey& d = st.rec.dirty[i];
            // PRVW dirty entries resolve to nothing on purpose: a preview is a derived artifact
            // and compaction drops superseded copies (S48-b), so they are neither content nor
            // a resolution failure.
            if (d.type == kTypePreview)
                continue;
            r.named.insert({d.type, d.key.bytes, gen});
            Instance inst;
            inst.entry = i;
            const bool hasRef = i < st.rec.refs.size() && st.rec.refs[i].present;
            if (hasRef) {
                inst.hash = st.rec.refs[i].hash;
                inst.flags = st.rec.refs[i].flags;
                if (const auto b = r.blobSources.find(inst.hash); b != r.blobSources.end()) {
                    inst.blobFrame = b->second;
                    inst.content = std::span<const std::uint8_t>(b->second->payload)
                                       .subspan(kBlobHashSize);
                } else if (const RecoveredChunk* f = frameByHash(inst.hash); f != nullptr) {
                    inst.content = f->payload;
                } else {
                    r.fullyResolved = false;
                    continue;
                }
            } else {
                const auto f = byVersion.find({d.type, d.key.bytes, gen});
                if (f == byVersion.end()) {
                    r.fullyResolved = false;
                    continue;
                }
                inst.frame = f->second;
                inst.hash = hasher.of(*f->second);
                inst.flags = static_cast<std::uint8_t>(f->second->flags & ~kFlagLinked);
                inst.content = f->second->payload;
            }
            out.push_back(std::move(inst));
        }
    }
    return r;
}

} // namespace

std::string chooseHistoryMode(const std::string& currentMode, double churnFraction) {
    if (currentMode == kModeCas)
        return churnFraction < kSwitchDown ? kModeJournal : kModeCas;
    return churnFraction >= kSwitchUp ? kModeCas : kModeJournal;
}

std::size_t ChurnTracker::HashKey::operator()(const BlobHash& h) const noexcept {
    std::size_t v = 0; // the key already IS a cryptographic hash: any 8 bytes of it are uniform
    std::memcpy(&v, h.data(), sizeof v);
    return v;
}

void ChurnTracker::add(const BlobHash& hash, std::uint64_t contentBytes) {
    totalBytes += contentBytes;
    if (seen_.insert(hash).second)
        uniqueBytes += contentBytes;
}

std::optional<ChurnTracker> churnFromOpen(const OpenReport& open) {
    if (open.base.unsupportedVersion || !open.base.rootFound || open.base.usedFullScan)
        return std::nullopt;
    FrameHasher hasher;
    const HistoryResolution history = resolveHistory(open, hasher);
    if (!history.fullyResolved)
        return std::nullopt; // no signal is better than a wrong one
    ChurnTracker tracker;
    for (const auto& [gen, list] : history.instances)
        for (const Instance& inst : list)
            tracker.add(inst.hash, inst.content.size());
    return tracker;
}

void addStateToChurn(ChurnTracker& tracker, std::span<const StateChunk> chunks) {
    for (const StateChunk& c : chunks) {
        if (c.type == kTypePreview)
            continue; // derived, dropped by compaction: never part of the retained history
        tracker.add(blobHashOf(c.payload), c.payload.size());
    }
}

std::optional<CompactionResult>
buildCompactedCheckpoint(std::span<const std::uint8_t> file, const OpenReport& open,
                         std::span<const StateChunk> newState, std::string* error,
                         const CompactionOptions& options) {
    const auto fail = [&](const char* what) -> std::optional<CompactionResult> {
        if (error != nullptr)
            *error = what;
        return std::nullopt;
    };
    if (open.base.unsupportedVersion)
        return fail("this file was written by a newer Mosaic: folding a container this build does "
                    "not understand would rewrite it as something it is not");
    if (!open.base.rootFound)
        return fail("this file has no verified root: there is no history to fold and no identity "
                    "to carry");
    if (open.base.documentUuid.empty())
        return fail("this file carries no document identity");
    // A file whose retained history is unreadable (a rotted frame or HIST -- unrepairable,
    // history carries no parity by design) must NOT be folded: the fold would emit a clean
    // checkpoint with no loss counters, and the reopened walk would step over the gap into
    // content some state never held -- the exact silent-wrong-undo the lostHistoryEntries rule
    // exists to decline. Refuse; the caller appends instead, and the damage verdict stands.
    if (open.base.lostHistoryEntries > 0)
        return fail("this file's save history is damaged: folding it would hide the damage");

    // Only STATES consume generation ids (spec 2.2). A compaction that commits an edit takes the
    // next one; a pure parity refresh advances nothing -- reusing an id would tie "highest
    // generation wins" and silently resolve to stale content (Round 12, A5).
    const std::uint64_t generation =
        newState.empty() ? open.tip.commitId : open.tip.commitId + 1;

    // 1. Current content: highest generation wins per (TYPE, KEY) across the checkpoint's content
    //    and every committed batch. Everything a key superseded falls out as history candidate.
    //    base.retained never re-enters the running -- committed generations always exceed the
    //    checkpoint's, by construction.
    std::map<ContentKey, const RecoveredChunk*> current;
    std::vector<const RecoveredChunk*> retirees;
    // HIST/BLOB frames sitting in base.chunks mean a full-scan open collapsed the regions (a
    // verified root over a destroyed directory). Such an open has no separable history, so they
    // ride through verbatim, unread -- exactly what the pre-Build-2 fold did with them.
    std::vector<const RecoveredChunk*> stray;
    // S48-b hard rule: a SUPERSEDED preview is DROPPED, never retained as an undo state. A
    // thumbnail is derived from content -- every folded PRVW would be dead bytes no reader may
    // resolve (loadedStates skips PRVW dirty keys to match), growing each fold for nothing.
    // The NEWEST preview stays, as current content.
    const auto retire = [&](const RecoveredChunk* c) {
        if (c->type != kTypePreview)
            retirees.push_back(c);
    };
    const auto offer = [&](const RecoveredChunk& c, bool fromBase) {
        if (c.type == kTypeHist || c.type == kTypeBlob) {
            // Committed-region HISTs are states (the resolution walk below reads them); the same
            // types in base.chunks are the full-scan strays described above.
            if (fromBase)
                stray.push_back(&c);
            return;
        }
        const RecoveredChunk*& slot = current[{c.type, c.key.bytes}];
        if (slot == nullptr)
            slot = &c;
        else if (c.generation >= slot->generation)
            retire(std::exchange(slot, &c));
        else
            retire(&c);
    };
    for (const RecoveredChunk& c : open.base.chunks)
        offer(c, /*fromBase=*/true);
    for (const RecoveredChunk& c : open.committed)
        offer(c, /*fromBase=*/false);
    for (const RecoveredChunk& c : open.base.retained)
        if (c.type != kTypeHist && c.type != kTypeBlob)
            retire(&c); // history a previous compaction already folded in

    // 2. The Save's own edit supersedes whatever those keys held.
    for (const StateChunk& s : newState)
        if (const auto it = current.find({s.type, s.key.bytes}); it != current.end()) {
            retire(it->second);
            current.erase(it);
        }

    // 3. Resolve the retained states, and measure the whole-history churn (spec 3.9: the window
    //    IS the retention horizon -- everything retained, never a recent slice). currentByHash is
    //    the cas encoder's dedup target set: the FINAL current content, post-override.
    FrameHasher hasher;
    std::map<BlobHash, std::span<const std::uint8_t>> currentByHash;
    for (const auto& [key, c] : current)
        currentByHash[hasher.of(*c)] = c->payload;
    for (const StateChunk& s : newState)
        if (s.type != kTypePreview)
            currentByHash[blobHashOf(s.payload)] = s.payload;

    const HistoryResolution history = resolveHistory(open, hasher);

    ChurnTracker churn;
    for (const auto& [gen, list] : history.instances)
        for (const Instance& inst : list)
            churn.add(inst.hash, inst.content.size());
    addStateToChurn(churn, newState); // the state being committed is retained history too

    // 4. Pick the encoding (spec 3.9). A history that did not fully resolve -- a damaged file --
    //    is preserved AS IS instead: every surviving frame, record, and blob carried verbatim in
    //    the file's own mode, no re-spelling, no switch. The walk's verdict on such a file is
    //    unchanged by the fold, which is the most a fold may promise about damage.
    const std::string fileMode = open.base.mode == kModeCas ? kModeCas : kModeJournal;
    const bool preserveAsIs = !history.fullyResolved;
    std::string mode;
    if (preserveAsIs)
        mode = fileMode;
    else if (!options.forceMode.empty())
        mode = options.forceMode == kModeCas ? kModeCas : kModeJournal;
    else if (churn.totalBytes == 0)
        mode = kModeJournal; // nothing retained: nothing to deduplicate
    else
        mode = chooseHistoryMode(fileMode, churn.fraction());
    const bool cas = mode == kModeCas;

    // 5. Assemble. Current content first (parity-covered, carry/fresh as before)...
    std::size_t reEncoded = 0;
    std::vector<FileChunk> chunks;
    for (const auto& [key, c] : current)
        chunks.push_back(carry(file, *c, parityCovered(c->type), /*history=*/false, reEncoded));
    for (const StateChunk& s : newState)
        chunks.push_back(freshChunk(s.type, s.key, generation, s.flags, s.payload,
                                    parityCovered(s.type), /*history=*/false));
    const std::size_t currentCount = chunks.size();

    // ...then the retained history behind it, in the chosen encoding.
    std::size_t blobCount = 0;
    std::vector<FileChunk> retained;

    if (preserveAsIs) {
        for (const RecoveredChunk* c : retirees)
            retained.push_back(carry(file, *c, /*parity=*/false, /*history=*/true, reEncoded));
        for (const auto& [hash, c] : history.blobSources)
            retained.push_back(carry(file, *c, /*parity=*/false, /*history=*/true, reEncoded));
        blobCount = history.blobSources.size();
        for (const auto& [gen, st] : history.states)
            retained.push_back(carry(file, *st.frame, /*parity=*/false, /*history=*/true,
                                     reEncoded));
    } else if (cas) {
        // Seeds -- frames below the oldest state, which no dirty list names (the value the first
        // save touching a key undoes TO) -- stay per-key frames in either mode: there is exactly
        // one per key, so content-addressing them buys nothing and costs a reference spelling.
        for (const RecoveredChunk* c : retirees)
            if (history.named.count({c->type, c->key.bytes, c->generation}) == 0)
                retained.push_back(carry(file, *c, /*parity=*/false, /*history=*/true, reEncoded));
        // Unique content once, oldest referencing state first. Content equal to a CURRENT frame's
        // is not stored at all -- the reference resolves to the parity-covered current chunk, and
        // duplicating it as history would un-deduplicate exactly what cas mode exists to share.
        std::set<BlobHash> emitted;
        for (const auto& [gen, list] : history.instances) {
            for (const Instance& inst : list) {
                if (currentByHash.count(inst.hash) != 0 || !emitted.insert(inst.hash).second)
                    continue;
                if (inst.blobFrame != nullptr) {
                    retained.push_back(carry(file, *inst.blobFrame, /*parity=*/false,
                                             /*history=*/true, reEncoded));
                } else {
                    retained.push_back(freshChunk(
                        kTypeBlob, blobKeyOf(inst.hash), gen, inst.flags,
                        makeBlobPayload(inst.hash, inst.content), /*parity=*/false,
                        /*history=*/true));
                }
                ++blobCount;
            }
        }
        // HIST records: already-cas records splice verbatim (encode-once); journal-spelled ones
        // gain their references here -- the one re-spelling a switch pays -- with every field
        // this layer does not own preserved.
        for (const auto& [gen, st] : history.states) {
            const bool alreadyCas = st.fromRetained && !st.rec.refs.empty();
            if (alreadyCas) {
                retained.push_back(carry(file, *st.frame, /*parity=*/false, /*history=*/true,
                                         reEncoded));
                continue;
            }
            HistRecord rec = st.rec;
            rec.refs.assign(rec.dirty.size(), BlobRef{});
            const auto instList = history.instances.find(gen);
            if (instList != history.instances.end())
                for (const Instance& inst : instList->second) {
                    rec.refs[inst.entry].present = true;
                    rec.refs[inst.entry].hash = inst.hash;
                    rec.refs[inst.entry].flags = inst.flags;
                }
            const std::string payload = histRecordJson(rec, histExtrasJson(st.frame->payload));
            retained.push_back(freshChunk(kTypeHist, histKey(gen), gen, kFlagCritical,
                                          std::vector<std::uint8_t>(payload.begin(),
                                                                    payload.end()),
                                          /*parity=*/false, /*history=*/true));
        }
    } else {
        // Journal (H2) encoding: every superseded frame keeps (or regains) its own per-(KEY,
        // generation) frame. Frames still on disk splice verbatim; a reference whose frame no
        // longer exists anywhere (cas -> journal switch) is materialized from its content.
        std::set<VersionKey> materialized;
        for (const RecoveredChunk* c : retirees) {
            retained.push_back(carry(file, *c, /*parity=*/false, /*history=*/true, reEncoded));
            materialized.insert({c->type, c->key.bytes, c->generation});
        }
        for (const auto& [gen, st] : history.states) {
            const auto instList = history.instances.find(gen);
            if (instList != history.instances.end())
                for (const Instance& inst : instList->second) {
                    if (inst.frame != nullptr)
                        continue; // its frame exists (current content or a retiree carried above)
                    const DirtyKey& d = st.rec.dirty[inst.entry];
                    if (currentByHash.count(inst.hash) != 0) {
                        // The reference points at current content. When that content IS this
                        // state's own frame (the newest state per key) the walk resolves it as
                        // current; only a genuine cross-key duplicate needs its own frame back.
                        const auto cur = current.find({d.type, d.key.bytes});
                        if (cur != current.end() && cur->second->generation == gen)
                            continue;
                    }
                    if (!materialized.insert({d.type, d.key.bytes, gen}).second)
                        continue;
                    retained.push_back(freshChunk(
                        d.type, d.key, gen, inst.flags,
                        std::vector<std::uint8_t>(inst.content.begin(), inst.content.end()),
                        /*parity=*/false, /*history=*/true));
                }
            const bool spelledCas = !st.rec.refs.empty();
            if (!spelledCas) {
                retained.push_back(carry(file, *st.frame, /*parity=*/false, /*history=*/true,
                                         reEncoded));
                continue;
            }
            HistRecord rec = st.rec;
            rec.refs.clear(); // the journal spelling: frames carry the content, not references
            const std::string payload = histRecordJson(rec, histExtrasJson(st.frame->payload));
            retained.push_back(freshChunk(kTypeHist, histKey(gen), gen, kFlagCritical,
                                          std::vector<std::uint8_t>(payload.begin(),
                                                                    payload.end()),
                                          /*parity=*/false, /*history=*/true));
        }
    }
    // HIST frames that would not parse, and full-scan strays, ride through verbatim in every
    // mode: this layer cannot re-spell what it cannot read, and dropping them would erase states
    // the panel may yet decline honestly.
    for (const RecoveredChunk* c : history.opaqueHists)
        retained.push_back(carry(file, *c, /*parity=*/false, /*history=*/true, reEncoded));
    for (const RecoveredChunk* c : stray)
        retained.push_back(carry(file, *c, /*parity=*/false, /*history=*/true, reEncoded));

    // ...and the HIST record for the state this very Save is committing. Without it the edit has
    // no dirty list on disk, and a reopen's history walk would step straight over it.
    if (!newState.empty()) {
        HistRecord rec;
        rec.state = generation;
        rec.parent = generation == 0 ? 0 : generation - 1; // linear undo (spec 3.2)
        for (const StateChunk& s : newState) {
            rec.dirty.push_back({s.type, s.key});
            BlobRef ref;
            if (cas && s.type != kTypePreview) {
                ref.present = true;
                ref.hash = blobHashOf(s.payload);
                ref.flags = static_cast<std::uint8_t>(s.flags & ~kFlagLinked);
            }
            rec.refs.push_back(ref);
        }
        if (!cas)
            rec.refs.clear();
        const std::string hist = histRecordJson(rec);
        retained.push_back(freshChunk(kTypeHist, histKey(generation), generation, kFlagCritical,
                                      std::vector<std::uint8_t>(hist.begin(), hist.end()),
                                      /*parity=*/false, /*history=*/true));
    }

    // Oldest state first, deterministic within one: copy-through keeps each frame at the
    // encoding the Save that committed it chose, so no state is ever compressed twice.
    std::stable_sort(retained.begin(), retained.end(),
                     [](const FileChunk& a, const FileChunk& b) {
                         return std::tie(a.generation, a.type, a.key.bytes) <
                                std::tie(b.generation, b.type, b.key.bytes);
                     });
    for (FileChunk& c : retained)
        chunks.push_back(std::move(c));

    CheckpointInput in;
    in.documentType = open.base.documentType;
    in.documentUuid = open.base.documentUuid;
    in.generation = generation;
    in.mode = mode;
    in.chunks = std::move(chunks);

    CompactionResult out;
    out.generation = generation;
    out.mode = mode;
    out.churnFraction = churn.fraction();
    out.currentChunks = currentCount;
    out.retainedChunks = in.chunks.size() - currentCount;
    out.blobChunks = blobCount;
    out.reEncodedChunks = reEncoded;
    out.bytes = buildCheckpoint(in, options.progress);
    return out;
}

} // namespace mosaic::io::native
