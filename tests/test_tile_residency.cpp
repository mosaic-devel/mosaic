#include "render/tile_residency.hpp"

#include <doctest/doctest.h>

#include <vector>

using namespace mosaic;
using render::TileResidency;

namespace {

core::TileKey key(std::uint64_t layer, std::uint32_t tx, std::uint32_t ty = 0) {
    return core::TileKey{layer, tx, ty};
}

// A residency sized to hold exactly `tiles` tiles.
TileResidency sized(std::size_t tiles, std::uint64_t bytesPerTile = 1024) {
    return TileResidency{bytesPerTile * tiles, bytesPerTile};
}

}  // namespace

TEST_CASE("capacity is the byte budget divided by the tile cost") {
    CHECK(sized(4).capacity() == 4);
    CHECK(TileResidency(10'000, 1024).capacity() == 9); // partial tiles do not count
    // A budget smaller than one tile holds NOTHING -- not "one anyway". Admitting a tile that does
    // not fit is how a budget becomes a suggestion.
    CHECK(TileResidency(512, 1024).capacity() == 0);
    // A zero tile cost is meaningless, not infinite: refuse rather than admit without bound.
    CHECK(TileResidency(1 << 20, 0).capacity() == 0);
}

TEST_CASE("a hit does not upload; a miss does") {
    TileResidency r = sized(4);
    const auto first = r.admit(key(1, 0));
    CHECK(first.ok);
    CHECK_FALSE(first.alreadyResident); // a miss: the caller must upload
    const auto second = r.admit(key(1, 0));
    CHECK(second.ok);
    CHECK(second.alreadyResident);      // a hit: no upload
    CHECK(r.count() == 1);
    CHECK(r.stats().hits == 1);
    CHECK(r.stats().misses == 1);
}

TEST_CASE("eviction is least-recently-used, and touching reorders") {
    TileResidency r = sized(3);
    r.admit(key(1, 0));
    r.admit(key(1, 1));
    r.admit(key(1, 2));
    CHECK(r.lruKey() == key(1, 0));

    // Touch the oldest: it must stop being the victim.
    CHECK(r.touch(key(1, 0)));
    CHECK(r.mruKey() == key(1, 0));
    CHECK(r.lruKey() == key(1, 1));

    const auto adm = r.admit(key(1, 3));
    REQUIRE(adm.ok);
    REQUIRE(adm.evicted.size() == 1);
    CHECK(adm.evicted[0] == key(1, 1)); // ... and the newly-oldest goes instead
    CHECK_FALSE(r.resident(key(1, 1)));
    CHECK(r.resident(key(1, 0)));
}

TEST_CASE("touching a tile that is not resident does nothing and says so") {
    TileResidency r = sized(2);
    r.admit(key(1, 0));
    CHECK_FALSE(r.touch(key(9, 9)));
    CHECK(r.count() == 1);
    CHECK(r.mruKey() == key(1, 0)); // order untouched
}

TEST_CASE("a pinned tile is never the victim") {
    TileResidency r = sized(2);
    r.admit(key(1, 0));
    r.admit(key(1, 1));
    r.pin(key(1, 0)); // the LRU one, so plain LRU would pick exactly this
    CHECK(r.pinnedCount() == 1);

    const auto adm = r.admit(key(1, 2));
    REQUIRE(adm.ok);
    REQUIRE(adm.evicted.size() == 1);
    CHECK(adm.evicted[0] == key(1, 1)); // the unpinned one goes
    CHECK(r.resident(key(1, 0)));
}

TEST_CASE("a composite needing more tiles at once than fit is REFUSED, not thrashed") {
    // The failure plain LRU hides: dispatch reads 3 tiles, budget holds 2. An LRU that always
    // evicts would drop a tile this dispatch still has to read -- thrash at best, and a free
    // between upload and read at worst. Refusing sends the caller to the CPU path: slow, correct.
    TileResidency r = sized(2);
    REQUIRE(r.admit(key(1, 0)).ok);
    r.pin(key(1, 0));
    REQUIRE(r.admit(key(1, 1)).ok);
    r.pin(key(1, 1));

    const auto adm = r.admit(key(1, 2));
    CHECK_FALSE(adm.ok);
    CHECK(adm.evicted.empty());   // and residency is left exactly as it was
    CHECK(r.count() == 2);
    CHECK(r.resident(key(1, 0)));
    CHECK(r.resident(key(1, 1)));
    CHECK(r.stats().refusals == 1);

    // Once the dispatch ends, the same admit succeeds.
    r.unpinAll();
    CHECK(r.pinnedCount() == 0);
    CHECK(r.admit(key(1, 2)).ok);
}

TEST_CASE("a zero-capacity residency refuses everything, cleanly") {
    TileResidency r = sized(0);
    const auto adm = r.admit(key(1, 0));
    CHECK_FALSE(adm.ok);
    CHECK(r.count() == 0);
    CHECK(r.stats().refusals == 1);
    CHECK(r.lruKey().layer == core::kInvalidLayerId); // empty is reported, not fabricated
    CHECK(r.mruKey().layer == core::kInvalidLayerId);
}

TEST_CASE("pins nest -- one tile read by two layers needs two unpins") {
    TileResidency r = sized(2);
    r.admit(key(1, 0));
    r.admit(key(1, 1));
    r.pin(key(1, 0));
    r.pin(key(1, 0)); // read twice in one dispatch
    CHECK(r.pinnedCount() == 1);

    r.unpin(key(1, 0));
    CHECK(r.pinnedCount() == 1); // still pinned: one reader remains
    const auto blocked = r.admit(key(1, 2));
    CHECK(blocked.ok);
    CHECK(blocked.evicted[0] == key(1, 1)); // the unpinned one still went

    r.unpin(key(1, 0));
    CHECK(r.pinnedCount() == 0);
    // Over-unpinning must not underflow the counter into a huge number.
    r.unpin(key(1, 0));
    CHECK(r.pinnedCount() == 0);
}

TEST_CASE("pinning a non-resident tile is a no-op, not a crash") {
    TileResidency r = sized(2);
    r.pin(key(7, 7));
    r.unpin(key(7, 7));
    CHECK(r.pinnedCount() == 0);
    CHECK(r.count() == 0);
}

TEST_CASE("an explicit evict overrides a pin -- stale pixels beat the thrash guard") {
    // The pin protects VALID tiles from being dropped mid-dispatch. When the caller says the
    // layer was edited, the tile is wrong, and serving it would draw the wrong thing.
    TileResidency r = sized(2);
    r.admit(key(1, 0));
    r.pin(key(1, 0));
    r.evict(key(1, 0));
    CHECK_FALSE(r.resident(key(1, 0)));
    CHECK(r.pinnedCount() == 0); // the pin count follows the tile out
    CHECK(r.count() == 0);
}

TEST_CASE("tiles are keyed by layer as well as position") {
    // Two layers' tile (0,0) are different tiles; collapsing them would composite one layer's
    // pixels into another's.
    TileResidency r = sized(4);
    r.admit(key(1, 0, 0));
    r.admit(key(2, 0, 0));
    CHECK(r.count() == 2);
    CHECK(r.resident(key(1, 0, 0)));
    CHECK(r.resident(key(2, 0, 0)));
    CHECK_FALSE(r.resident(key(3, 0, 0)));
}

TEST_CASE("reconfigure shrinks LRU-first and reports what went") {
    TileResidency r = sized(4);
    for (std::uint32_t i = 0; i < 4; ++i)
        r.admit(key(1, i));
    const auto dropped = r.reconfigure(2 * 1024, 1024);
    CHECK(r.capacity() == 2);
    CHECK(r.count() == 2);
    REQUIRE(dropped.size() == 2);
    CHECK(dropped[0] == key(1, 0)); // oldest first
    CHECK(dropped[1] == key(1, 1));
    CHECK(r.resident(key(1, 2)));
    CHECK(r.resident(key(1, 3)));
}

TEST_CASE("a shrink cannot evict pinned tiles, and says it is over budget") {
    // Honesty over pretence: reporting a fit that does not exist would have the caller allocate
    // against memory it does not have.
    TileResidency r = sized(4);
    for (std::uint32_t i = 0; i < 4; ++i) {
        r.admit(key(1, i));
        r.pin(key(1, i));
    }
    const auto dropped = r.reconfigure(1024, 1024);
    CHECK(dropped.empty());
    CHECK(r.capacity() == 1);
    CHECK(r.count() == 4);
    CHECK(r.overBudget());

    r.unpinAll();
    CHECK(r.reconfigure(1024, 1024).size() == 3);
    CHECK_FALSE(r.overBudget());
}

TEST_CASE("clear drops everything including pins") {
    TileResidency r = sized(4);
    r.admit(key(1, 0));
    r.pin(key(1, 0));
    r.clear();
    CHECK(r.count() == 0);
    CHECK(r.pinnedCount() == 0);
    CHECK(r.admit(key(1, 0)).ok); // and the next document starts clean
}

TEST_CASE("refusals are counted, because that is the 'it got slow' signal") {
    TileResidency r = sized(1);
    r.admit(key(1, 0));
    r.pin(key(1, 0));
    for (int i = 0; i < 5; ++i)
        CHECK_FALSE(r.admit(key(1, static_cast<std::uint32_t>(i + 1))).ok);
    CHECK(r.stats().refusals == 5);
    r.resetStats();
    CHECK(r.stats().refusals == 0);
}

TEST_CASE("a long churn stays consistent and bounded") {
    // The order list and the map must not drift apart over thousands of operations -- that drift
    // is how an LRU starts returning tiles it already evicted.
    TileResidency r = sized(16);
    for (std::uint32_t i = 0; i < 4000; ++i) {
        r.admit(key(1, i % 64, i % 7));
        if (i % 3 == 0)
            r.touch(key(1, (i / 2) % 64, ((i / 2) % 7)));
    }
    CHECK(r.count() <= r.capacity());
    CHECK(r.residentBytes() <= 16ull * 1024ull);
    CHECK_FALSE(r.overBudget());
    CHECK(r.count() > 0);
    // The order list and the map must not have drifted apart. Evicting everything one tile at a
    // time is the check: each evict must find its victim in BOTH structures, and the set must
    // drain to exactly empty -- a duplicate or orphaned entry shows up here as a count that never
    // reaches zero, or as an evict that finds nothing.
    const std::size_t before = r.count();
    std::size_t drained = 0;
    while (r.count() > 0) {
        const core::TileKey lru = r.lruKey();
        REQUIRE(lru.layer != core::kInvalidLayerId);
        CHECK(r.resident(lru));
        r.evict(lru);
        CHECK_FALSE(r.resident(lru));
        ++drained;
        REQUIRE(drained <= before); // a stuck evict would spin forever otherwise
    }
    CHECK(drained == before);
    CHECK(r.lruKey().layer == core::kInvalidLayerId);
    CHECK(r.mruKey().layer == core::kInvalidLayerId);
}
