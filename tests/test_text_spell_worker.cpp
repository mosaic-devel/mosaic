// SpellCheckWorker tests (docs/spell-check-plan.md commit 2b): the background scan + epoch-cancel
// shell. Deterministic via a mock dictionary; results are polled with a generous timeout (a mock
// scan of a tiny block completes in microseconds, so this is not timing-sensitive in practice).
#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <thread>

#include "core/text/spell_worker.hpp"
#include "core/text/text_model.hpp"

using namespace mosaic::core::text;

namespace {
// Poll for a completed result whose epoch matches `wantEpoch`, up to ~2s. Ignores any staler result
// that arrives first (a superseded scan). Returns nullopt on timeout.
std::optional<SpellCheckWorker::Result> waitForEpoch(SpellCheckWorker& w, std::uint64_t wantEpoch) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto r = w.takeResult()) {
            if (r->epoch == wantEpoch) return r;
            continue;  // a superseded (older) result -- keep waiting for the one we asked about
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}
}  // namespace

TEST_CASE("worker scans a block and reports the misspelled ranges") {
    SpellCheckWorker w;
    w.loadMockDictionary("en", {"wrold", "teh"});
    w.request(makeBlock("the wrold is teh best"), "", "en", {}, /*epoch=*/1);
    auto r = waitForEpoch(w, 1);
    REQUIRE(r.has_value());
    REQUIRE(r->ranges.size() == 2);
    CHECK(r->ranges[0] == MisspelledRange{4, 9});
    CHECK(r->ranges[1] == MisspelledRange{13, 16});
}

TEST_CASE("the newest request wins (coalescing + epoch tag)") {
    SpellCheckWorker w;
    w.loadMockDictionary("en", {"wrold", "teh"});
    // Fire two back-to-back; the worker may run only the second. Whichever results arrive, the one
    // tagged epoch 2 must reflect the SECOND block ("teh" at [0,3)).
    w.request(makeBlock("wrold wrold"), "", "en", {}, 1);
    w.request(makeBlock("teh good"), "", "en", {}, 2);
    auto r = waitForEpoch(w, 2);
    REQUIRE(r.has_value());
    REQUIRE(r->ranges.size() == 1);
    CHECK(r->ranges[0] == MisspelledRange{0, 3});  // "teh"
}

TEST_CASE("no result before any request, and correct text yields an empty range list") {
    SpellCheckWorker w;
    w.loadMockDictionary("en", {"wrold"});
    CHECK_FALSE(w.takeResult().has_value());  // nothing queued yet
    w.request(makeBlock("the world is fine"), "", "en", {}, 7);
    auto r = waitForEpoch(w, 7);
    REQUIRE(r.has_value());
    CHECK(r->ranges.empty());
}

TEST_CASE("dictionary queries are served on the UI side while the worker lives") {
    SpellCheckWorker w;
    w.loadMockDictionary("en", {"wrold"}, {{"wrold", {"world", "word"}}});
    CHECK(w.hasDictionary("en"));
    CHECK(w.hasDictionary("en-US"));
    CHECK(w.suggest("wrold", "en") == std::vector<std::string>{"world", "word"});
    w.ignore("wrold");
    w.request(makeBlock("wrold"), "", "en", {}, 3);
    auto r = waitForEpoch(w, 3);
    REQUIRE(r.has_value());
    CHECK(r->ranges.empty());  // ignored -> no longer flagged
}
