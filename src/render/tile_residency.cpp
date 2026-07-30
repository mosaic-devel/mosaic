#include "render/tile_residency.hpp"

#include <algorithm>

namespace mosaic::render {
namespace {

core::TileKey invalidKey() noexcept {
    return core::TileKey{core::kInvalidLayerId, 0, 0};
}

}  // namespace

TileResidency::TileResidency(std::uint64_t budgetBytes, std::uint64_t bytesPerTile) noexcept {
    reconfigure(budgetBytes, bytesPerTile);
}

std::vector<core::TileKey> TileResidency::reconfigure(std::uint64_t budgetBytes,
                                                      std::uint64_t bytesPerTile) noexcept {
    m_bytesPerTile = bytesPerTile;
    // A zero tile size would make capacity meaningless rather than infinite; treat it as "nothing
    // fits", which routes the caller to the CPU path instead of admitting unbounded tiles.
    m_capacity = bytesPerTile == 0 ? 0 : static_cast<std::size_t>(budgetBytes / bytesPerTile);

    // Shrink to fit, LRU first. Pinned tiles stay -- see overBudget().
    std::vector<core::TileKey> dropped;
    while (m_map.size() > m_capacity) {
        auto it = m_order.end();
        bool found = false;
        while (it != m_order.begin()) {
            --it;
            const auto mapIt = m_map.find(*it);
            if (mapIt != m_map.end() && mapIt->second.pins == 0) {
                found = true;
                break;
            }
        }
        if (!found)
            break; // everything left is pinned; report it via overBudget() rather than lie
        const core::TileKey victim = *it;
        dropped.push_back(victim);
        m_map.erase(victim);
        m_order.erase(it);
        ++m_stats.evictions;
    }
    return dropped;
}

bool TileResidency::resident(const core::TileKey& k) const noexcept {
    return m_map.find(k) != m_map.end();
}

bool TileResidency::touch(const core::TileKey& k) noexcept {
    const auto it = m_map.find(k);
    if (it == m_map.end())
        return false;
    // Splice rather than erase+insert: the iterator stored in the entry stays valid, which is the
    // whole reason the order list is a std::list.
    m_order.splice(m_order.begin(), m_order, it->second.order);
    return true;
}

TileAdmission TileResidency::admit(const core::TileKey& k) noexcept {
    TileAdmission out;
    if (touch(k)) { // already here: a hit, and no upload
        out.ok = true;
        out.alreadyResident = true;
        ++m_stats.hits;
        return out;
    }
    if (m_capacity == 0) { // the budget cannot hold even one tile
        ++m_stats.refusals;
        return out;
    }

    // Evict LRU-first until there is room. A PINNED tile is never a victim: it is being read by
    // the composite in flight, and dropping it would either thrash it straight back in or, worse,
    // free it between the upload and the read.
    while (m_map.size() >= m_capacity) {
        auto it = m_order.end();
        bool found = false;
        while (it != m_order.begin()) {
            --it;
            const auto mapIt = m_map.find(*it);
            if (mapIt != m_map.end() && mapIt->second.pins == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            // Everything resident is pinned: this composite needs more tiles AT ONCE than the
            // budget holds. Refuse cleanly and leave residency untouched -- the caller's answer is
            // the CPU path for this composite, which is slow but correct. Thrashing would be both
            // slow AND wrong.
            ++m_stats.refusals;
            return out;
        }
        const core::TileKey victim = *it;
        out.evicted.push_back(victim);
        m_map.erase(victim);
        m_order.erase(it);
        ++m_stats.evictions;
    }

    m_order.push_front(k);
    m_map.emplace(k, Entry{m_order.begin(), 0});
    ++m_stats.misses;
    out.ok = true;
    return out;
}

void TileResidency::pin(const core::TileKey& k) noexcept {
    const auto it = m_map.find(k);
    if (it == m_map.end())
        return; // pinning a tile that is not resident is a caller bug, but not a crash
    if (it->second.pins == 0)
        ++m_pinned;
    ++it->second.pins;
}

void TileResidency::unpin(const core::TileKey& k) noexcept {
    const auto it = m_map.find(k);
    if (it == m_map.end() || it->second.pins == 0)
        return;
    --it->second.pins;
    if (it->second.pins == 0)
        --m_pinned;
}

void TileResidency::unpinAll() noexcept {
    for (auto& [key, entry] : m_map)
        entry.pins = 0;
    m_pinned = 0;
}

void TileResidency::evict(const core::TileKey& k) noexcept {
    const auto it = m_map.find(k);
    if (it == m_map.end())
        return;
    // An explicit evict overrides a pin: the caller is saying these pixels are STALE (the layer
    // was edited), and serving a pinned stale tile would draw the wrong thing. Correctness beats
    // the thrash guard, which exists only to protect valid tiles.
    if (it->second.pins != 0)
        --m_pinned;
    m_order.erase(it->second.order);
    m_map.erase(it);
    ++m_stats.evictions;
}

void TileResidency::clear() noexcept {
    m_map.clear();
    m_order.clear();
    m_pinned = 0;
}

core::TileKey TileResidency::lruKey() const noexcept {
    return m_order.empty() ? invalidKey() : m_order.back();
}

core::TileKey TileResidency::mruKey() const noexcept {
    return m_order.empty() ? invalidKey() : m_order.front();
}

}  // namespace mosaic::render
