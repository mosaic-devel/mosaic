#include "ui/recovery_journal.hpp"

#include <doctest/doctest.h>

// The crash-restore classifier (docs/askortell-dialog.md flows 1 + 2), exercised on synthetic
// binding/replay results. app_window.cpp drives this on real journals located at open time (and at
// app start for untitled docs); the tombstone/replay mechanics are pinned in
// test_mosaic_journal_session.cpp. This file only pins the fact -> face mapping.
namespace {

using mosaic::io::native::JournalBindingStatus;
using mosaic::io::native::JournalReplay;
using mosaic::ui::classifyJournalRecovery;
using mosaic::ui::JournalRecovery;

JournalReplay replayWith(std::size_t states, bool anomaly) {
    JournalReplay r;
    r.binding = JournalBindingStatus::Ok;
    for (std::size_t i = 0; i < states; ++i)
        r.states.push_back(i + 1);
    r.anomaly = anomaly;
    return r;
}

} // namespace

TEST_CASE("crash-restore: a bound journal with unsaved states offers flow 1") {
    const auto d = classifyJournalRecovery(JournalBindingStatus::Ok, replayWith(3, false));
    CHECK(d.kind == JournalRecovery::Restore);
    CHECK(d.changeCount == 3);
    CHECK_FALSE(d.tornTail);
}

TEST_CASE("crash-restore: a torn tail is flagged but still restores what survived") {
    const auto d = classifyJournalRecovery(JournalBindingStatus::Ok, replayWith(2, true));
    CHECK(d.kind == JournalRecovery::Restore);
    CHECK(d.changeCount == 2);
    CHECK(d.tornTail);
}

TEST_CASE("crash-restore: a bound but empty journal offers nothing (discard it)") {
    const auto d = classifyJournalRecovery(JournalBindingStatus::Ok, replayWith(0, false));
    CHECK(d.kind == JournalRecovery::None);
}

TEST_CASE("crash-restore: a journal bound to a different commit is an orphan (flow 2)") {
    CHECK(classifyJournalRecovery(JournalBindingStatus::WrongSeed, {}).kind ==
          JournalRecovery::Orphan);
    CHECK(classifyJournalRecovery(JournalBindingStatus::WrongBinding, {}).kind ==
          JournalRecovery::Orphan);
}

TEST_CASE("crash-restore: no valid header means nothing to offer") {
    CHECK(classifyJournalRecovery(JournalBindingStatus::NoHeader, {}).kind == JournalRecovery::None);
}
