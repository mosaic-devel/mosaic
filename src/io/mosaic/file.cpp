#include "io/mosaic/file.hpp"

#include "io/mosaic/codec.hpp"
#include "io/mosaic/naming.hpp"
#include "io/mosaic/reedsolomon.hpp"
#include "io/mosaic/wire.hpp"

#include "common/fs_path.hpp" // pathFromUtf8: the target path is UTF-8, a Win32 path is UTF-16

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#else
#include <atomic>
// See save.cpp: the toolchain defines NOMINMAX, and redefining it is a -Werror diagnostic. This one
// matters more than most -- buildCheckpoint below is built on std::min/std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mosaic::io::native {
namespace {

using nlohmann::json;

using detail::keyFromHex;
using detail::keyToHex;
using detail::tagFromString;
using detail::tagToString;

// Build one chunk into a standalone buffer (the root is placed, not appended).
[[nodiscard]] std::vector<std::uint8_t> buildChunk(ChunkTag type, const ChunkKey& key,
                                                   std::uint64_t generation,
                                                   std::span<const std::uint8_t> payload,
                                                   Profile profile) {
    std::vector<std::uint8_t> out;
    appendChunk(out, type, key, generation, payload, profile);
    return out;
}

[[nodiscard]] std::span<const std::uint8_t> jsonBytes(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

// A verified root candidate plus its decoded payload and stored checksum (the checksum is the
// committed-region chain seed and journal binding target, spec 2.6 -- replicas are byte-
// identical, so whichever replica verified carries the same value).
struct RootInfo {
    std::uint64_t generation = 0;
    json payload;
    std::array<std::uint8_t, kStrongChecksumSize> checksum{};
    std::uint8_t checksumSize = 0;
};

[[nodiscard]] std::optional<RootInfo> decodeRoot(const ChunkRecord& rec,
                                                 std::span<const std::uint8_t> file) {
    if (!rec.valid || rec.type != kTypeRoot)
        return std::nullopt;
    const auto payload = decodeChunkPayload(rec, file);
    if (!payload.has_value())
        return std::nullopt;
    json j = json::parse(payload->begin(), payload->end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
        return std::nullopt;
    RootInfo info{rec.generation, std::move(j), rec.checksum, rec.checksumSize};
    return info;
}

// The recovery ladder's first two rungs: the slot (following an RPTR indirection), then a
// bounded tail-window scan for the end-of-checkpoint replicas. Highest generation wins across
// every candidate that verifies.
[[nodiscard]] std::optional<RootInfo> findBestRoot(std::span<const std::uint8_t> file) {
    std::optional<RootInfo> best;
    const auto offer = [&](std::optional<RootInfo> cand) {
        if (cand.has_value() && (!best.has_value() || cand->generation > best->generation))
            best = std::move(cand);
    };

    // Rung 1: the slot. Either the root itself or an RPTR holding the real root's offset.
    if (const auto slot = parseChunkAt(file, kPreambleSize); slot.has_value() && slot->valid) {
        if (slot->type == kTypeRoot) {
            offer(decodeRoot(*slot, file));
        } else if (slot->type == kTypeRootPtr) {
            if (const auto ptr = decodeChunkPayload(*slot, file);
                ptr.has_value() && ptr->size() == 8) {
                const std::uint64_t target = detail::loadLe64(ptr->data());
                if (target < file.size())
                    if (const auto real = parseChunkAt(file, static_cast<std::size_t>(target)))
                        offer(decodeRoot(*real, file));
            }
        }
    }

    // Rung 2: the end replicas. Appended Save batches land after them (spec 2.3), so scan a
    // bounded tail window rather than assuming EOF; resync tolerates landing mid-chunk.
    const std::size_t window = std::min<std::size_t>(file.size(), kRootSlotSize * 4);
    for (const ChunkRecord& rec : scanChunks(file, file.size() - window))
        if (rec.valid && rec.type == kTypeRoot)
            offer(decodeRoot(rec, file));

    return best;
}

void fullScanRecover(std::span<const std::uint8_t> file, ReadReport& out) {
    out.usedFullScan = true;
    // Content types only -- ROOT/DIR/RPTR are accelerators, PRTY is reconstructed from rather
    // than returned, and journal-only types never appear in the main file.
    static constexpr ChunkTag kContent[] = {kTypeManifest, kTypePreview, kTypeTile,
                                            kTypeVector,   kTypeHist,    kTypeBlob};
    std::map<std::pair<std::string, std::array<std::uint8_t, 16>>, RecoveredChunk> best;
    for (const ChunkRecord& rec : scanChunks(file)) {
        if (!rec.valid)
            continue;
        if (std::find(std::begin(kContent), std::end(kContent), rec.type) == std::end(kContent))
            continue;
        const auto mapKey = std::make_pair(tagToString(rec.type), rec.key.bytes);
        const auto it = best.find(mapKey);
        if (it != best.end() && it->second.generation >= rec.generation)
            continue;
        auto payload = decodeChunkPayload(rec, file);
        if (!payload.has_value())
            continue;
        best[mapKey] = RecoveredChunk{rec.type,  rec.key,           rec.generation, rec.flags,
                                      std::move(*payload), rec.offset, rec.consumed};
    }
    for (auto& [k, v] : best)
        out.chunks.push_back(std::move(v));
}

} // namespace

const RecoveredChunk* ReadReport::find(const ChunkTag& t, const ChunkKey& k) const noexcept {
    for (const RecoveredChunk& c : chunks)
        if (c.type == t && c.key == k)
            return &c;
    return nullptr;
}

std::optional<FileChunk> makeVerbatimChunk(std::span<const std::uint8_t> frame, bool parity,
                                           bool history) {
    assert(!(parity && history) && "history-region parity is out of the build plan (spec 3.8)");
    const auto rec = parseChunkAt(frame, 0);
    if (!rec.has_value() || !rec->valid || rec->consumed != frame.size())
        return std::nullopt;
    // Full verification, not just the checksum: an undecodable payload must be caught at Save
    // time too (verify-then-copy), or a foreign writer's bug would ride into every future file.
    if (!decodeChunkPayload(*rec, frame).has_value())
        return std::nullopt;
    FileChunk c;
    c.type = rec->type;
    c.key = rec->key;
    c.generation = rec->generation;
    c.profile = static_cast<Profile>(rec->profile);
    c.flags = rec->flags;
    c.parity = parity;
    c.history = history;
    c.verbatim.assign(frame.begin(), frame.end());
    return c;
}

std::vector<std::uint8_t> buildCheckpoint(const CheckpointInput& in,
                                          const BuildProgressFn& progress) {
    std::vector<std::uint8_t> out;
    appendPreamble(out, in.documentType, in.formatVersion);

    const std::size_t slotOffset = out.size();
    out.resize(slotOffset + kRootSlotSize, 0);

    // Progress accounting (async save): content chunks carry the compression cost, parity the
    // rest -- an honest-enough split for a status bar, not a stopwatch.
    const std::size_t totalChunks = std::max<std::size_t>(in.chunks.size(), 1);
    std::size_t doneChunks = 0;
    const auto report = [&](double fraction) {
        if (progress)
            progress(fraction);
    };

    // Content chunks, recording each frame's offset for the directory and, for parity-covered
    // chunks, the frame extent for striping. Verbatim chunks (copy-through, spec 3.3) splice
    // their pre-framed bytes untouched -- makeVerbatimChunk already verified them, and the
    // checksum's coverage is position-independent, so the bytes stay valid at any offset.
    json entries = json::array();
    std::vector<std::pair<std::size_t, std::size_t>> eligible; // frame offset, frame length
    for (const FileChunk& c : in.chunks) {
        const std::size_t off = out.size();
        json entry{{"t", tagToString(c.type)}, {"k", keyToHex(c.key)}, {"o", off}};
        if (c.history)
            entry["h"] = true; // retained history, not current content (spec 3.3)
        entries.push_back(std::move(entry));
        if (!c.verbatim.empty())
            out.insert(out.end(), c.verbatim.begin(), c.verbatim.end());
        else
            appendChunk(out, c.type, c.key, c.generation, c.payload, c.profile, c.flags);
        if (c.parity)
            eligible.emplace_back(off, out.size() - off);
        ++doneChunks;
        report(0.75 * static_cast<double>(doneChunks) / static_cast<double>(totalChunks));
    }

    // Reed-Solomon parity (spec 2.7): stripes of k eligible frames in file order; shard = the
    // whole framed chunk, zero-padded to the stripe's longest member (the padding waste Round 11
    // measured at +11.5% over the m/k floor at 64px). A trailing group of one gets no parity --
    // m "parity" shards of a single frame would just be copies. PRTY chunks are ancillary and
    // live outside the directory; the root's rs_params map is their index.
    json stripes = json::array();
    for (std::size_t s = 0; s < eligible.size(); s += kRsDataShards) {
        const std::size_t n = std::min(kRsDataShards, eligible.size() - s);
        if (n < 2)
            continue;
        const std::size_t m = std::min(kRsParityShards, n);
        std::size_t shardLen = 0;
        for (std::size_t j = 0; j < n; ++j)
            shardLen = std::max(shardLen, eligible[s + j].second);
        std::vector<std::vector<std::uint8_t>> data(n, std::vector<std::uint8_t>(shardLen, 0));
        json dOffsets = json::array();
        for (std::size_t j = 0; j < n; ++j) {
            const auto [off, len] = eligible[s + j];
            std::copy(out.begin() + static_cast<std::ptrdiff_t>(off),
                      out.begin() + static_cast<std::ptrdiff_t>(off + len), data[j].begin());
            dOffsets.push_back(off);
        }
        const ReedSolomon rs(n, m);
        const auto parity = rs.encodeParity(data);
        json pOffsets = json::array();
        const std::size_t stripeIdx = s / kRsDataShards;
        for (std::size_t row = 0; row < parity.size(); ++row) {
            pOffsets.push_back(out.size());
            appendChunk(out, kTypeParity, parityKey((stripeIdx << 8) | row), in.generation,
                        parity[row], Profile::Store, 0 /* ancillary */);
        }
        stripes.push_back(json{{"l", shardLen}, {"d", std::move(dOffsets)},
                               {"p", std::move(pOffsets)}});
        report(0.75 + 0.25 * (static_cast<double>(s + n) /
                              static_cast<double>(std::max<std::size_t>(eligible.size(), 1))));
    }
    report(1.0);

    // The manifest replica (spec 2.3). The manifest is the one chunk without which NOTHING opens:
    // lose its few hundred bytes and a thousand intact frames are worthless, because nothing says
    // how big the canvas is or which layer a tile belongs to. The root already carries three
    // replicas for exactly this reason. Parity is the wrong tool here -- a stripe pads every shard
    // to its longest member, so a large manifest (a document with many layers) would inflate its
    // whole stripe -- and it only survives m losses anyway. A second copy costs the manifest's own
    // size, full stop, and it goes AFTER the parity chunks so that no single burst of contiguous
    // damage can take both. Readers treat two frames sharing (TYPE, KEY, GENERATION) as replicas of
    // one chunk: whichever verifies answers.
    for (const FileChunk& c : in.chunks) {
        if (c.type != kTypeManifest || c.history)
            continue;
        entries.push_back(json{{"t", tagToString(c.type)}, {"k", keyToHex(c.key)}, {"o", out.size()}});
        if (!c.verbatim.empty())
            out.insert(out.end(), c.verbatim.begin(), c.verbatim.end());
        else
            appendChunk(out, c.type, c.key, c.generation, c.payload, c.profile, c.flags);
        break; // one manifest, one replica
    }

    const std::size_t dirOffset = out.size();
    const std::string dirJson = json{{"entries", std::move(entries)}}.dump();
    appendChunk(out, kTypeDir, zeroKey(), in.generation, jsonBytes(dirJson), Profile::Balanced);

    // wal_start_offset points at the first end-of-checkpoint root replica: appended Save
    // batches land after the replicas, and the committed-region replay skips ROOT chunks, so
    // the value needs no self-referential total-length arithmetic.
    const std::size_t walStart = out.size();
    json rootJson{{"generation", in.generation},
                  {"document_uuid", in.documentUuid},
                  {"document_type", in.documentType},
                  // The container version, again -- here under a BLAKE3 checksum. The preamble's
                  // copy is a bare byte, so this is the one a reader may act on (§2.1).
                  {"format_version", in.formatVersion},
                  {"base_dir_offset", dirOffset},
                  {"wal_start_offset", walStart},
                  {"mode", in.mode.empty() ? kModeJournal : in.mode}};
    rootJson["rs_params"] = stripes.empty()
                                ? json(nullptr)
                                : json{{"k", kRsDataShards}, {"m", kRsParityShards},
                                       {"stripes", std::move(stripes)}};
    const std::string rootStr = rootJson.dump();
    const std::vector<std::uint8_t> rootChunk =
        buildChunk(kTypeRoot, zeroKey(), in.generation, jsonBytes(rootStr), Profile::Store);

    if (rootChunk.size() <= kRootSlotSize) {
        std::copy(rootChunk.begin(), rootChunk.end(),
                  out.begin() + static_cast<std::ptrdiff_t>(slotOffset));
    } else {
        // Overflow (a huge rs_params map, Round 10): the slot gets a tiny always-fits pointer
        // to the first end replica; every reader rung finds the real root regardless.
        std::array<std::uint8_t, 8> target{};
        detail::storeLe64(target.data(), walStart);
        const std::vector<std::uint8_t> ptr =
            buildChunk(kTypeRootPtr, zeroKey(), in.generation, target, Profile::Store);
        std::copy(ptr.begin(), ptr.end(), out.begin() + static_cast<std::ptrdiff_t>(slotOffset));
    }
    out.insert(out.end(), rootChunk.begin(), rootChunk.end());
    out.insert(out.end(), rootChunk.begin(), rootChunk.end()); // the commit marker's own redundancy
    return out;
}

ReadReport readCheckpoint(std::span<const std::uint8_t> file) {
    ReadReport report;
    const auto pre = parsePreamble(file.data(), file.size());
    if (pre.has_value())
        report.documentType = pre->documentType; // convenience only, never load-bearing

    const auto root = findBestRoot(file);
    if (!root.has_value()) {
        // No chunk in this file verified. If the preamble ALSO claims a version this build does
        // not know, those two facts agree on one explanation: a newer container whose framing we
        // cannot parse. Refuse honestly rather than reporting a perfectly good file as destroyed.
        //
        // Both facts are required. The preamble carries no checksum, so a single rotted byte there
        // must never be enough on its own -- and it never is, because a v1 file with a corrupted
        // version byte still has three checksummed root replicas that answer above.
        if (pre.has_value() && pre->version > kFormatVersion) {
            report.unsupportedVersion = true;
            report.formatVersion = pre->version;
            return report;
        }
        fullScanRecover(file, report);
        return report;
    }
    report.rootFound = true;
    report.rootChecksum = root->checksum;
    report.rootChecksumSize = root->checksumSize;
    const json& r = root->payload;

    // A root verified, so the framing IS ours, whatever the preamble byte says. The root's own
    // claim rides under a BLAKE3 checksum; the preamble's does not. The root wins -- which is
    // exactly what makes a flipped preamble byte a non-event rather than a locked door.
    report.formatVersion = static_cast<std::uint8_t>(
        r.value("format_version", static_cast<unsigned>(kFormatVersion)));
    if (report.formatVersion > kFormatVersion) {
        report.unsupportedVersion = true;
        return report; // do not touch the directory: its meaning may have changed too
    }
    report.generation = r.value("generation", std::uint64_t{0});
    report.documentUuid = r.value("document_uuid", std::string{});
    report.documentType = static_cast<std::uint8_t>(r.value("document_type", 0));
    report.walStartOffset = r.value("wal_start_offset", std::uint64_t{0});
    report.mode = r.value("mode", std::string{kModeJournal});
    if (r.contains("rs_params") && !r["rs_params"].is_null())
        report.rsParamsJson = r["rs_params"].dump();

    // Fast path: the directory. Any structural failure here drops to the full scan -- the
    // directory is an accelerator, never the sole path in.
    const std::uint64_t dirOffset = r.value("base_dir_offset", std::uint64_t{0});
    const auto dirRec = (dirOffset < file.size())
                            ? parseChunkAt(file, static_cast<std::size_t>(dirOffset))
                            : std::nullopt;
    const auto dirPayload =
        (dirRec.has_value() && dirRec->valid && dirRec->type == kTypeDir)
            ? decodeChunkPayload(*dirRec, file)
            : std::nullopt;
    json dir = dirPayload.has_value()
                   ? json::parse(dirPayload->begin(), dirPayload->end(), nullptr, false)
                   : json(json::value_t::discarded);
    if (dir.is_discarded() || !dir.is_object() || !dir["entries"].is_array()) {
        fullScanRecover(file, report);
        return report;
    }

    struct LostEntry {
        ChunkTag tag;
        ChunkKey key;
        std::size_t off;
        bool history;
    };
    std::vector<LostEntry> lost;
    // An entry that failed and cannot be counted until the whole directory has been read: a lost
    // frame whose (TYPE, KEY) still has a surviving CURRENT sibling was a replica, and losing a
    // replica of a chunk that answered anyway is not a loss to report to anyone.
    std::vector<LostEntry> unresolved;
    // Survivors in directory order, each tagged with the directory's "h" flag. They are split into
    // current content and retained history only after the parity pass, so a reconstructed frame
    // takes the same route its directory entry declared.
    std::vector<RecoveredChunk> found;
    std::vector<bool> isHistory;
    const auto keep = [&](RecoveredChunk c, bool history) {
        found.push_back(std::move(c));
        isHistory.push_back(history);
    };
    for (const json& e : dir["entries"]) {
        const auto tag = tagFromString(e.value("t", std::string{}));
        const auto key = keyFromHex(e.value("k", std::string{}));
        const std::uint64_t off = e.value("o", std::uint64_t{0});
        const bool flagged = e.value("h", false); // retained history (spec 3.3)
        if (!tag.has_value() || !key.has_value()) {
            // Unmappable entry: no identity to reconstruct toward, and none to match a sibling
            // against either. Its own flag is all we have.
            ++(flagged ? report.lostHistoryEntries : report.lostEntries);
            continue;
        }
        // A HIST record is history whatever the flag says: it describes an undo state, never the
        // document. Belt and braces against a writer that forgets to set "h".
        const bool history = flagged || *tag == kTypeHist;
        const auto rec = (off < file.size()) ? parseChunkAt(file, static_cast<std::size_t>(off))
                                             : std::nullopt;
        if (!rec.has_value() || !rec->valid || rec->type != *tag || !(rec->key == *key)) {
            lost.push_back({*tag, *key, static_cast<std::size_t>(off), history});
            continue;
        }
        auto payload = decodeChunkPayload(*rec, file);
        if (!payload.has_value()) {
            // Checksum-valid but undecodable (a foreign writer's bug): parity would reproduce
            // the same undecodable bytes, so reconstruction cannot help. Honest loss -- unless a
            // replica of the same chunk answers, which the tally below decides.
            unresolved.push_back({*tag, *key, static_cast<std::size_t>(off), history});
            continue;
        }
        keep(RecoveredChunk{rec->type, rec->key, rec->generation, rec->flags, std::move(*payload),
                            rec->offset, rec->consumed},
             history);
    }

    // Reed-Solomon pass (spec 2.7): only the cheap known-erasure decode -- the directory told us
    // exactly which frames are damaged. Every reconstruction is re-verified (checksum + decode +
    // identity match) before being trusted; anything beyond the parity budget stays honestly lost.
    json rs = report.rsParamsJson.empty()
                  ? json(nullptr)
                  : json::parse(report.rsParamsJson, nullptr, /*allow_exceptions=*/false);
    std::map<std::size_t, std::optional<std::vector<std::vector<std::uint8_t>>>> stripeCache;
    for (const LostEntry& le : lost) {
        std::optional<std::vector<std::uint8_t>> recovered;
        if (rs.is_object() && rs["stripes"].is_array()) {
            for (std::size_t si = 0; si < rs["stripes"].size() && !recovered.has_value(); ++si) {
                const json& stripe = rs["stripes"][si];
                if (!stripe["d"].is_array() || !stripe["p"].is_array())
                    continue;
                std::ptrdiff_t myShard = -1;
                for (std::size_t j = 0; j < stripe["d"].size(); ++j)
                    if (stripe["d"][j].get<std::uint64_t>() == le.off)
                        myShard = static_cast<std::ptrdiff_t>(j);
                if (myShard < 0)
                    continue;
                auto cached = stripeCache.find(si);
                if (cached == stripeCache.end()) {
                    const std::size_t l = stripe.value("l", std::uint64_t{0});
                    const std::size_t n = stripe["d"].size();
                    const std::size_t m = stripe["p"].size();
                    std::vector<std::optional<std::vector<std::uint8_t>>> shards(n + m);
                    const auto gather = [&](std::uint64_t off, bool isParity)
                        -> std::optional<std::vector<std::uint8_t>> {
                        if (off >= file.size() || l == 0 || l > kMaxUncompressedLen)
                            return std::nullopt;
                        const auto r = parseChunkAt(file, static_cast<std::size_t>(off));
                        if (!r.has_value() || !r->valid)
                            return std::nullopt;
                        if (isParity) {
                            if (r->type != kTypeParity)
                                return std::nullopt;
                            auto payload = decodeChunkPayload(*r, file);
                            if (!payload.has_value() || payload->size() != l)
                                return std::nullopt;
                            return payload;
                        }
                        if (r->consumed > l)
                            return std::nullopt;
                        std::vector<std::uint8_t> shard(l, 0);
                        std::copy(file.begin() + static_cast<std::ptrdiff_t>(off),
                                  file.begin() + static_cast<std::ptrdiff_t>(off + r->consumed),
                                  shard.begin());
                        return shard;
                    };
                    for (std::size_t j = 0; j < n; ++j)
                        shards[j] = gather(stripe["d"][j].get<std::uint64_t>(), false);
                    for (std::size_t j = 0; j < m; ++j)
                        shards[n + j] = gather(stripe["p"][j].get<std::uint64_t>(), true);
                    const ReedSolomon coder(n, m);
                    cached = stripeCache.emplace(si, coder.reconstruct(shards)).first;
                }
                if (cached->second.has_value())
                    recovered = (*cached->second)[static_cast<std::size_t>(myShard)];
            }
        }
        // Re-verify before trusting: the reconstructed bytes must parse as a valid frame whose
        // identity matches the directory entry AND decode cleanly -- exact-and-verified or
        // declined, never silently wrong.
        bool accepted = false;
        if (recovered.has_value()) {
            const auto rec = parseChunkAt(*recovered, 0);
            if (rec.has_value() && rec->valid && rec->type == le.tag && rec->key == le.key) {
                auto payload = decodeChunkPayload(*rec, *recovered);
                if (payload.has_value()) {
                    // frameOffset/frameLen stay 0: these bytes were rebuilt in memory and exist
                    // nowhere in the file, so a later compaction re-encodes rather than copies
                    // them -- which is how the repair becomes permanent.
                    keep(RecoveredChunk{rec->type, rec->key, rec->generation, rec->flags,
                                        std::move(*payload)},
                         le.history);
                    ++report.rsReconstructed;
                    accepted = true;
                }
            }
        }
        if (!accepted)
            unresolved.push_back(le);
    }

    // Split survivors: current content (unique per (TYPE, KEY), highest generation wins -- the
    // invariant documentFromReport and the history walk both rely on) from retained history, and
    // from REPLICAS. Two frames sharing (TYPE, KEY, GENERATION) are the same logical chunk by the
    // format's own rule -- generation is what versions a key -- so the second copy is dropped, not
    // mistaken for history. (Getting that wrong would file a manifest replica under retained
    // history, where damaging it would decline the whole undo walk.) The directory's "h" flag
    // decides history; the generation collapse behind it is a safety net, so a file whose directory
    // under-declares its history can still never present two frames of one key as current content.
    enum class Role : std::uint8_t { Current, History, Replica };
    std::vector<Role> role(found.size(), Role::Current);
    for (std::size_t i = 0; i < found.size(); ++i)
        if (isHistory[i])
            role[i] = Role::History;
    std::map<std::pair<std::string, std::array<std::uint8_t, 16>>, std::size_t> winner;
    for (std::size_t i = 0; i < found.size(); ++i) {
        if (role[i] != Role::Current)
            continue;
        const auto mapKey = std::make_pair(tagToString(found[i].type), found[i].key.bytes);
        const auto it = winner.find(mapKey);
        if (it == winner.end())
            winner.emplace(mapKey, i);
        else if (found[i].generation == found[it->second].generation)
            role[i] = Role::Replica; // the same chunk, written twice: one answer is enough
        else if (found[i].generation > found[it->second].generation) {
            role[it->second] = Role::History; // the older frame is history, not current content
            it->second = i;
        } else {
            role[i] = Role::History;
        }
    }

    // Now the tally. A lost CURRENT frame whose (TYPE, KEY) has a surviving sibling was a replica
    // of a chunk that answered anyway -- reporting that as damage would raise the "this file is
    // damaged" face over a document that lost precisely nothing. When every copy of a chunk is
    // gone it is counted ONCE, by identity: losing both halves of a replicated manifest is one
    // chunk lost, not two areas. Retained history is never replicated, so it counts as it falls.
    std::set<std::pair<std::string, std::array<std::uint8_t, 16>>> countedLost;
    for (const LostEntry& u : unresolved) {
        if (u.history) {
            ++report.lostHistoryEntries;
            continue;
        }
        const auto id = std::make_pair(tagToString(u.tag), u.key.bytes);
        if (winner.count(id) == 0 && countedLost.insert(id).second)
            ++report.lostEntries;
    }

    for (std::size_t i = 0; i < found.size(); ++i) {
        if (role[i] == Role::Replica)
            continue;
        (role[i] == Role::History ? report.retained : report.chunks).push_back(std::move(found[i]));
    }
    return report;
}

bool writeFileAtomic(const std::string& path, std::span<const std::uint8_t> bytes,
                     std::string* error) {
    const auto fail = [&](const std::string& what) {
        if (error)
            *error = what;
        return false;
    };
    namespace fs = std::filesystem;
    // pathFromUtf8, not fs::path(path): identical on POSIX (the bytes ARE the path), and the only
    // correct decode on Windows, where fs::path is wchar_t-based and would otherwise read these
    // UTF-8 bytes in the active code page.
    const fs::path target = common::pathFromUtf8(path);

#ifndef _WIN32
    fs::path dir = target.parent_path();
    if (dir.empty())
        dir = ".";
    std::string tmp = (dir / (target.filename().string() + ".tmp-XXXXXX")).string();
    const int fd = ::mkstemp(tmp.data());
    if (fd < 0)
        return fail("could not create a temporary file next to the target");
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ::ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (n < 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return fail("could not write the temporary file");
        }
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tmp.c_str());
        return fail("could not flush the temporary file to disk");
    }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return fail("could not move the finished file over the target");
    }
    // Parent-directory fsync: without it POSIX permits losing the rename itself on power
    // failure (spec 2.6). Best-effort -- some filesystems refuse directory fsync; the rename
    // already succeeded, so a refusal here must not fail the save.
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
#else
    std::error_code ec;
    // A UNIQUE temp name -- which is what mkstemp buys the POSIX branch, and what the fixed
    // "<target>.tmp" this replaced did not: two instances saving the same document (or one racing a
    // .tmp left behind by an earlier crash) would otherwise write into the same file and rename
    // each other's half over the user's document. It stays in the target's OWN directory because
    // MoveFileExW, like rename(2), is only atomic within a volume. The wide concatenation is
    // deliberate: target.string() would narrow through the active code page and then be re-decoded
    // by the same, which is a lossy round trip for any name outside it.
    static std::atomic<unsigned> seq{0};
    fs::path tmp = target;
    tmp += L".tmp-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
           std::to_wstring(::GetTickCount64()) + L"-" +
           std::to_wstring(seq.fetch_add(1, std::memory_order_relaxed));
    {
        const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return fail("could not create a temporary file next to the target");
        std::size_t written = 0;
        while (written < bytes.size()) {
            DWORD n = 0;
            const DWORD want = static_cast<DWORD>(
                std::min<std::size_t>(bytes.size() - written, 1u << 30));
            // Short writes loop, exactly as the write(2) loop above does; n == 0 on a reported
            // success is not a documented outcome for a non-zero request, and treating it as a
            // failure is what stops this loop spinning forever rather than finishing a short file.
            if (::WriteFile(h, bytes.data() + written, want, &n, nullptr) == 0 || n == 0) {
                ::CloseHandle(h);
                fs::remove(tmp, ec);
                return fail("could not write the temporary file");
            }
            written += n;
        }
        if (::FlushFileBuffers(h) == 0) { // fsync: durable BEFORE the rename, never after
            ::CloseHandle(h);
            fs::remove(tmp, ec);
            return fail("could not flush the temporary file to disk");
        }
        ::CloseHandle(h);
    }
    // MoveFileExW with MOVEFILE_REPLACE_EXISTING, and NOT ReplaceFileW -- the choice matters, so it
    // is written down. MOVEFILE_REPLACE_EXISTING is a single metadata operation that swaps the
    // directory entry, which is the property §2.6 depends on: a watching sync client never sees an
    // intermediate state and a crash mid-write leaves the previous good file completely untouched.
    // ReplaceFileW exists to PRESERVE the destination's ACL, attributes and alternate data streams,
    // but it does so as a documented multi-step sequence (rename the target aside, move the
    // replacement in, copy metadata back) with observable intermediate states, and it fails
    // outright when the target does not exist -- trading the load-bearing guarantee for a nicety.
    // POSIX rename() preserves nothing of the old file either, so MOVEFILE_REPLACE_EXISTING is also
    // the behavioural twin of the branch above. The cost, stated honestly: the new file carries the
    // TEMP's security descriptor (inherited from the parent directory), so a document with a
    // hand-edited ACL or an alternate stream loses it on save, just as a POSIX save discards a
    // file's mode bits. If that ever needs fixing the answer is to copy the ACL onto the temp
    // before the move, not to give up atomicity.
    //
    // MOVEFILE_WRITE_THROUGH is this branch's parent-directory fsync: it holds the return until the
    // rename's metadata reaches the device, which is the step POSIX permits losing on power failure
    // without the explicit dir fsync above.
    if (::MoveFileExW(tmp.c_str(), target.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        // Also the (Windows-only) failure mode worth naming: a replace is refused while ANY handle
        // holds the destination open without FILE_SHARE_DELETE -- a preview handler or an on-access
        // scanner can do that. The document is untouched, so reporting and letting the user retry
        // is the honest answer.
        fs::remove(tmp, ec);
        return fail("could not move the finished file over the target");
    }
    return true;
#endif
}

} // namespace mosaic::io::native
