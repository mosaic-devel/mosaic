#include "io/mosaic/salvage.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace mosaic::io::native {
namespace {

using Link = std::array<std::uint8_t, kLinkSize>;

// One standalone-verified linked frame, plus whether damage was skipped to reach it.
struct RawFrame {
    ChunkRecord rec;
    std::vector<std::uint8_t> payload;
    bool gapBefore = false;
};

// A lineage under construction: the chain's frames in link order, the running tip, and the
// highest generation seen (the monotonic-ids guard for gap bridging).
struct ChainBuild {
    Link root{};
    bool seedRooted = false;
    bool bridgedGap = false;
    Link tip{};
    std::uint64_t lastGeneration = 0;
    std::vector<std::size_t> frames;
};

using Identity = std::pair<ChunkTag, std::array<std::uint8_t, 16>>;

} // namespace

const SalvageLineage* SalvageReport::primary() const noexcept {
    for (const SalvageLineage& ln : lineages)
        if (ln.seedRooted)
            return &ln;
    return nullptr;
}

SalvageReport salvageLinkedRegion(std::span<const std::uint8_t> buf, std::size_t start,
                                  const Link& seed) {
    SalvageReport report;

    // Pass 1: collect every standalone-verifiable linked frame, magic-resyncing past damage.
    // Every intact frame verifies independently (the explicit-link property, spec 2.2) --
    // damage localizes to a recorded gap instead of poisoning everything after it (B3b is the
    // cumulative-chain dead end this design replaced).
    std::vector<RawFrame> frames;
    bool pendingGap = false;
    const auto markGap = [&] {
        if (!pendingGap) {
            pendingGap = true;
            ++report.gaps;
        }
    };
    std::size_t offset = std::min(start, buf.size());
    while (offset < buf.size()) {
        const auto it = std::search(buf.begin() + static_cast<std::ptrdiff_t>(offset), buf.end(),
                                    kChunkMagic.begin(), kChunkMagic.end());
        if (it == buf.end()) {
            markGap(); // trailing non-frame bytes
            break;
        }
        const std::size_t at = static_cast<std::size_t>(it - buf.begin());
        if (at != offset)
            markGap(); // skipped garbage to reach this magic
        const auto rec = parseChunkAt(buf, at);
        if (!rec.has_value() || !rec->valid) {
            markGap();
            offset = at + 1; // resync -- corrupted length fields cannot derail the hunt
            continue;
        }
        if (rec->type == kTypeRoot) {
            offset = at + rec->consumed; // end-of-checkpoint replicas: not chain members
            continue;
        }
        if (!rec->linked()) {
            markGap(); // an unlinked frame in a linked region: no chain membership to verify
            offset = at + rec->consumed;
            continue;
        }
        auto payload = decodeChunkPayload(*rec, buf);
        if (!payload.has_value()) {
            markGap();
            offset = at + rec->consumed;
            continue;
        }
        frames.push_back({*rec, std::move(*payload), pendingGap});
        pendingGap = false;
        offset = at + rec->consumed;
    }

    // Every checksum a surviving frame (or the seed) owns: the "resolvable" set. A chain root
    // pointing at one of these is a genuine fork; a root pointing at nothing is an orphan.
    std::set<Link> known;
    known.insert(seed);
    for (const RawFrame& f : frames)
        known.insert(f.rec.linkValue());

    // Pass 2: partition into link-chains (Round 13). Following links, not file order, is what
    // recovers both racers of a frame-interleaved dual write completely and separately (D5b).
    std::vector<ChainBuild> chains;
    std::map<Link, std::size_t> tips; // current chain tip -> chain index
    std::ptrdiff_t previousChain = -1;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const RawFrame& f = frames[i];
        std::size_t chainIdx;
        const auto it = tips.find(f.rec.link);
        if (it != tips.end()) {
            chainIdx = it->second;
            tips.erase(it);
        } else if (known.count(f.rec.link) == 0 && f.gapBefore && previousChain >= 0 &&
                   f.rec.generation >=
                       chains[static_cast<std::size_t>(previousChain)].lastGeneration) {
            // Orphan root right after damage, generations still monotonic: the same writer's
            // chain continuing past a destroyed frame (spec 2.8: skip-and-continue, WITH
            // flags). A root that resolves to a known checksum never takes this path -- that
            // is a fork, and merging it is exactly the D2 blending hazard.
            chainIdx = static_cast<std::size_t>(previousChain);
            tips.erase(chains[chainIdx].tip);
            chains[chainIdx].bridgedGap = true;
        } else {
            ChainBuild fresh;
            fresh.root = f.rec.link;
            fresh.seedRooted = f.rec.link == seed;
            chains.push_back(std::move(fresh));
            chainIdx = chains.size() - 1;
        }
        ChainBuild& chain = chains[chainIdx];
        chain.frames.push_back(i);
        chain.tip = f.rec.linkValue();
        chain.lastGeneration = std::max(chain.lastGeneration, f.rec.generation);
        tips[chain.tip] = chainIdx;
        previousChain = static_cast<std::ptrdiff_t>(chainIdx);
    }

    // Pass 3: apply each lineage state-granularly. A state counts only when its HIST and every
    // dirty chunk verify; anything less flags the exact keys -- or degrades to declared-
    // imprecise when the HIST (the authority on the dirty set) is itself gone (B4/B4b).
    for (ChainBuild& chain : chains) {
        SalvageLineage lineage;
        lineage.root = chain.root;
        lineage.seedRooted = chain.seedRooted;
        lineage.bridgedGap = chain.bridgedGap;

        std::map<Identity, RecoveredChunk> pending;
        std::set<Identity> flagged;
        bool haveExpected = false;
        std::uint64_t expectedState = 0;
        const auto flag = [&](const ChunkTag& t, const ChunkKey& k) {
            if (flagged.insert({t, k.bytes}).second)
                lineage.flagged.push_back({t, k});
        };

        for (const std::size_t idx : chain.frames) {
            RawFrame& f = frames[idx];
            if (f.rec.type == kTypeCommit)
                continue; // a chain member, but carries no content
            if (f.rec.type != kTypeHist) {
                pending[{f.rec.type, f.rec.key.bytes}] = RecoveredChunk{
                    f.rec.type, f.rec.key, f.rec.generation, f.rec.flags, std::move(f.payload)};
                continue;
            }
            const auto hist = parseHistRecord(f.payload);
            if (!hist.has_value()) {
                lineage.precise = false; // a dirty list we cannot read exactly
                continue;
            }
            if (haveExpected && hist->state != expectedState) {
                // Whole states' HIST frames are gone between here and the last applied state:
                // their dirty lists are unknown. Flag what their surviving frames reveal and
                // say so -- a lower bound, never fake precision (B4b).
                lineage.precise = false;
                for (auto it = pending.begin(); it != pending.end();) {
                    if (it->second.generation < hist->state) {
                        flag(it->second.type, it->second.key);
                        it = pending.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (const DirtyKey& d : hist->dirty) {
                const auto it = pending.find({d.type, d.key.bytes});
                if (it != pending.end() && it->second.generation == hist->state) {
                    lineage.chunks.push_back(std::move(it->second));
                    pending.erase(it);
                } else {
                    flag(d.type, d.key); // this state's content is lost; older content shows
                }
            }
            lineage.chunks.push_back(RecoveredChunk{f.rec.type, f.rec.key, f.rec.generation,
                                                    f.rec.flags, std::move(f.payload)});
            lineage.states.push_back(hist->state);
            haveExpected = true;
            expectedState = hist->state + 1;
        }
        // Leftover pending frames are a torn tail's half-state: neither applied nor flagged --
        // the same conservative treatment replay gives them.
        report.lineages.push_back(std::move(lineage));
    }

    std::size_t seedRooted = 0;
    for (const SalvageLineage& ln : report.lineages)
        seedRooted += ln.seedRooted ? 1 : 0;
    report.rootConflict = seedRooted > 1;
    return report;
}

JournalSalvage salvageJournal(std::span<const std::uint8_t> buf,
                              const JournalBinding& expected) {
    JournalSalvage out;
    std::size_t headerEnd = 0;
    Link headerLink{};
    out.binding = verifyJournalHeader(buf, expected, &headerEnd, &headerLink);
    if (out.binding != JournalBindingStatus::Ok)
        return out; // a journal bound elsewhere is rejected, never salvaged into this document
    out.report = salvageLinkedRegion(buf, headerEnd, headerLink);
    return out;
}

} // namespace mosaic::io::native
