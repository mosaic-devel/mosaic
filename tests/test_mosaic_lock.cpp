#include "io/mosaic/lock.hpp"

#include "io/mosaic/journal.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

// The §2.10 advisory lock: an OS advisory lock on a recovery-dir file (never the user's document).
// flock treats two open descriptions of one file independently even within a process, so the
// busy/reacquire behaviour is exercisable in-process (a second Mosaic is a second description).
namespace {

using namespace mosaic::io::native;
namespace fs = std::filesystem;

fs::path lockDir() {
    const char* env = std::getenv("TMPDIR");
    const fs::path dir = fs::path(env != nullptr ? env : "/tmp") / "mosaic_lock";
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("advisory lock: exclusive, busy for a second holder, reacquirable after release") {
    const std::string p = (lockDir() / "doc.lock").string();
    std::error_code ec;
    fs::remove(p, ec);

    std::optional<AdvisoryLock> a;
    CHECK(AdvisoryLock::tryAcquire(p, a) == AdvisoryLock::Status::Acquired);
    REQUIRE(a.has_value());

    // A live holder has it -> a second attempt is Busy (the flow-6 signal), not an error.
    std::optional<AdvisoryLock> b;
    CHECK(AdvisoryLock::tryAcquire(p, b) == AdvisoryLock::Status::Busy);
    CHECK_FALSE(b.has_value());

    // Releasing it (a clean close, or the OS on a crash) lets the next open acquire cleanly --
    // the stale-lock case that folds into ordinary journal-backed recovery.
    a->release();
    std::optional<AdvisoryLock> c;
    CHECK(AdvisoryLock::tryAcquire(p, c) == AdvisoryLock::Status::Acquired);
}

TEST_CASE("advisory lock: destruction releases the lock") {
    const std::string p = (lockDir() / "doc2.lock").string();
    std::error_code ec;
    fs::remove(p, ec);
    {
        std::optional<AdvisoryLock> a;
        REQUIRE(AdvisoryLock::tryAcquire(p, a) == AdvisoryLock::Status::Acquired);
    } // ~AdvisoryLock releases here
    std::optional<AdvisoryLock> b;
    CHECK(AdvisoryLock::tryAcquire(p, b) == AdvisoryLock::Status::Acquired);
}

TEST_CASE("advisory lock: path is the journal key + .lock") {
    const std::string j = recoveryJournalPath("uuid-x", "/a/b.mosaic");
    CHECK(recoveryLockPath("uuid-x", "/a/b.mosaic") == j + ".lock");
}
