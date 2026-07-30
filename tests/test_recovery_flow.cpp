#include "ui/recovery_flow.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

// The open-time recovery classifier (docs/askortell-dialog.md flows 3a-3e + 4), exercised on
// synthetic reader/salvage reports. app_window.cpp drives this exact function on real .mosaic
// opens; tools/corrupt_corpus.cpp's self-check + the scratch probe confirm the real corpus files
// land on the same flows this pins by construction. Journal flows (1/2) are not classified here.
namespace {

using namespace mosaic::io::native;
using mosaic::ui::classifyRecoveryFlow;
using mosaic::ui::RecoveryFlow;

// A healthy checkpoint open: a verified root, no anomaly, nothing lost.
OpenReport cleanOpen() {
    OpenReport r;
    r.base.rootFound = true;
    return r;
}

SalvageLineage seedLineage(std::vector<std::uint64_t> states) {
    SalvageLineage ln;
    ln.seedRooted = true; // primary() adopts the first seed-rooted chain
    ln.states = std::move(states);
    return ln;
}

SalvageReport salvageReaching(std::vector<std::uint64_t> states) {
    SalvageReport s;
    s.lineages.push_back(seedLineage(std::move(states)));
    return s;
}

} // namespace

TEST_CASE("recovery flow: a clean open needs no dialog") {
    CHECK(classifyRecoveryFlow(cleanOpen(), 0, nullptr) == RecoveryFlow::None);
}

TEST_CASE("recovery flow: 3a is a silent parity repair, 3b when the repair still lost something") {
    OpenReport r = cleanOpen();
    r.base.rsReconstructed = 3;
    CHECK(classifyRecoveryFlow(r, 0, nullptr) == RecoveryFlow::Repaired);
    // Loss outranks the silent repair: if anything is still unreadable it is a 3b tell, not 3a.
    CHECK(classifyRecoveryFlow(r, 2, nullptr) == RecoveryFlow::Damaged);
}

TEST_CASE("recovery flow: checkpoint areas lost beyond parity -> 3b") {
    OpenReport r = cleanOpen();
    r.base.lostEntries = 2;
    CHECK(classifyRecoveryFlow(r, 2, nullptr) == RecoveryFlow::Damaged);
}

TEST_CASE("recovery flow: a destroyed structure is 3e and outranks everything else") {
    OpenReport r;
    r.base.rootFound = false;
    r.base.usedFullScan = true;
    CHECK(classifyRecoveryFlow(r, 0, nullptr) == RecoveryFlow::BadlyDamaged);
    // Even a committed anomaly with recoverable saves cannot promote past a lost structure.
    r.committedAnomaly = true;
    SalvageReport sv = salvageReaching({2, 3});
    CHECK(classifyRecoveryFlow(r, 5, &sv) == RecoveryFlow::BadlyDamaged);
}

TEST_CASE("recovery flow: a committed anomaly with nothing past the tip is a 3d tell") {
    OpenReport r = cleanOpen();
    r.committedAnomaly = true;
    r.commits = {1};

    SUBCASE("salvage reaches only what conservative already had") {
        SalvageReport sv = salvageReaching({1});
        CHECK(classifyRecoveryFlow(r, 0, &sv) == RecoveryFlow::TornTail);
    }
    SUBCASE("an unprobed anomaly never becomes a false ask") {
        CHECK(classifyRecoveryFlow(r, 0, nullptr) == RecoveryFlow::TornTail);
    }
    SUBCASE("no seed-rooted lineage at all") {
        SalvageReport sv; // empty: primary() == nullptr
        REQUIRE(sv.primary() == nullptr);
        CHECK(classifyRecoveryFlow(r, 0, &sv) == RecoveryFlow::TornTail);
    }
}

TEST_CASE("recovery flow: saves recoverable past the gap make the one genuine 3c ask") {
    OpenReport r = cleanOpen();
    r.committedAnomaly = true;

    SUBCASE("conservative stopped at the checkpoint; salvage brings saves 2+3") {
        r.commits = {};
        SalvageReport sv = salvageReaching({2, 3});
        CHECK(classifyRecoveryFlow(r, 0, &sv) == RecoveryFlow::Recover);
    }
    SUBCASE("conservative reached save 1; salvage bridges to save 3 past a lost save 2") {
        r.commits = {1};
        SalvageReport sv = salvageReaching({1, 3});
        CHECK(classifyRecoveryFlow(r, 0, &sv) == RecoveryFlow::Recover);
    }
    SUBCASE("a committed anomaly outranks a co-occurring checkpoint loss") {
        r.commits = {};
        r.base.lostEntries = 4; // would be 3b on its own
        SalvageReport sv = salvageReaching({2, 3});
        CHECK(classifyRecoveryFlow(r, 4, &sv) == RecoveryFlow::Recover);
    }
}

TEST_CASE("recovery flow: two seed-rooted lineages are a 4 dual-writer conflict, outranking 3c") {
    OpenReport r = cleanOpen();
    r.committedAnomaly = true;
    r.commits = {1};
    SalvageReport sv = salvageReaching({2, 3}); // would be 3c on its own
    sv.lineages.push_back(seedLineage({1}));    // a second writer's chain
    sv.rootConflict = true;
    CHECK(classifyRecoveryFlow(r, 0, &sv) == RecoveryFlow::DualWriter);
}
