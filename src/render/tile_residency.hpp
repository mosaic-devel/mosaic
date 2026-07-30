#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "core/tile_grid.hpp"

// Which tiles are kept on the GPU, and which one goes when the budget is full (S60-a,
// docs/s60-performance-plan.md §3.3).
//
// Deliberately Vulkan-FREE. Eviction is a policy question -- what is worth keeping, what may be
// dropped, and when the answer is "none of this fits, use the CPU path" -- and it is the same
// policy whatever the atlas layout underneath turns out to be. Keeping it separate means it is
// fully unit-testable without a device, and that the GPU plumbing can change (page atlas, image
// array, sparse binding) without touching a line of the decision.
//
// ---- PINNING is the load-bearing part --------------------------------------------------------
//
// Plain LRU is wrong here, and wrong in a way that looks fine until a big document arrives. One
// composite dispatch reads N tiles; if N exceeds capacity, an LRU that always evicts to make room
// will evict tiles THIS dispatch still has to read, then fault them straight back in -- the
// classic thrash, except it also produces wrong pixels if the eviction happens between the upload
// and the read. So a tile in use is PINNED, `admit` refuses rather than evicting a pinned tile,
// and the caller's answer to a refusal is to fall back to the CPU path for that composite. Slower
// is a fine outcome; corrupt or thrashing is not.

namespace mosaic::render {

// Hash for core::TileKey. Not in tile_grid.hpp because that header is the shared vocabulary and
// this is one consumer's indexing need. FNV-1a over the three fields; ids are dense in practice,
// so a cheap mix beats a strong one.
struct TileKeyHash {
    [[nodiscard]] std::size_t operator()(const core::TileKey& k) const noexcept {
        std::uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](std::uint64_t v) {
            for (int i = 0; i < 8; ++i) {
                h ^= (v >> (i * 8)) & 0xFFull;
                h *= 1099511628211ull;
            }
        };
        mix(k.layer);
        mix((static_cast<std::uint64_t>(k.tx) << 32) | k.ty);
        return static_cast<std::size_t>(h);
    }
};

// What `admit` did, so the caller knows whether to upload, and what to free first.
struct TileAdmission {
    bool ok = false;             // false => nothing changed; the caller falls back to the CPU path
    bool alreadyResident = false; // true => no upload needed, this was a cache hit
    std::vector<core::TileKey> evicted; // freed to make room, in eviction order
};

class TileResidency {
public:
    TileResidency() = default;
    // `budgetBytes` from atlasBudgetBytes(); `bytesPerTile` is one tile in the working format.
    TileResidency(std::uint64_t budgetBytes, std::uint64_t bytesPerTile) noexcept;

    // Re-budget (the document resized, or the memory snapshot moved). Tiles beyond the new
    // capacity are evicted LRU-first and returned. Pinned tiles are never evicted, so a shrink
    // mid-composite can leave residency ABOVE capacity -- `overBudget()` reports that honestly
    // rather than the class pretending it fits.
    std::vector<core::TileKey> reconfigure(std::uint64_t budgetBytes,
                                           std::uint64_t bytesPerTile) noexcept;

    [[nodiscard]] bool resident(const core::TileKey& k) const noexcept;
    // Mark a resident tile most-recently-used. False if it was not resident (no side effect).
    bool touch(const core::TileKey& k) noexcept;

    // Make `k` resident, evicting the least-recently-used UNPINNED tiles until it fits.
    // Refuses (ok = false) when capacity is zero, or when everything that would have to go is
    // pinned -- i.e. this composite needs more tiles at once than the budget can hold.
    TileAdmission admit(const core::TileKey& k) noexcept;

    // Pin/unpin. A pinned tile cannot be evicted. Pins NEST (a tile read by two layers in one
    // dispatch is pinned twice), so every pin needs its unpin. unpinAll ends a dispatch.
    void pin(const core::TileKey& k) noexcept;
    void unpin(const core::TileKey& k) noexcept;
    void unpinAll() noexcept;
    [[nodiscard]] std::size_t pinnedCount() const noexcept { return m_pinned; }

    void evict(const core::TileKey& k) noexcept; // explicit drop (the layer's pixels changed)
    void clear() noexcept;                       // a new document: everything goes

    [[nodiscard]] std::size_t count() const noexcept { return m_map.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] std::uint64_t residentBytes() const noexcept {
        return static_cast<std::uint64_t>(m_map.size()) * m_bytesPerTile;
    }
    // True when a shrink left more pinned tiles resident than capacity allows.
    [[nodiscard]] bool overBudget() const noexcept { return m_map.size() > m_capacity; }

    // Diagnostics. `misses` counts admits that had to upload; `refusals` counts admits that could
    // not fit at all -- a non-zero refusal count is the signal that the budget is too small for
    // this document, which is exactly what a user would otherwise report as "it got slow".
    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint64_t refusals = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return m_stats; }
    void resetStats() noexcept { m_stats = {}; }

    // Least- and most-recently-used keys, for tests and diagnostics. Both are core::kInvalidLayerId
    // keys when empty.
    [[nodiscard]] core::TileKey lruKey() const noexcept;
    [[nodiscard]] core::TileKey mruKey() const noexcept;

private:
    struct Entry {
        std::list<core::TileKey>::iterator order; // position in m_order (front = MRU)
        std::uint32_t pins = 0;
    };

    std::list<core::TileKey> m_order; // front = most recently used
    std::unordered_map<core::TileKey, Entry, TileKeyHash> m_map;
    std::uint64_t m_bytesPerTile = 0;
    std::size_t m_capacity = 0;
    std::size_t m_pinned = 0;
    Stats m_stats;
};

}  // namespace mosaic::render
