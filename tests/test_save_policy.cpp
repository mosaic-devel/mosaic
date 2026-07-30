#include "ui/save_policy.hpp"

#include <doctest/doctest.h>

// The proactive early-fold decision (S48 Build 2 ruling), pinned gate by gate: benefit needs
// BOTH the reuse fraction and the absolute-bytes floor; the size cap bounds the stall; the
// throttle needs the signal to actually move between attempts; and cas never folds proactively
// (there is no early benefit to a down-switch). Each case flips exactly one input off a passing
// baseline, so removing any single gate fails exactly one CHECK -- the mutation-test shape the
// PRVW rules established.
namespace {

using namespace mosaic::ui;
namespace nio = mosaic::io::native;

ProactiveFoldInputs passing() {
    ProactiveFoldInputs in;
    in.mode = nio::kModeJournal;
    in.fileSize = 2ull * 1024 * 1024;
    in.churnFraction = 0.50;
    in.projectedSavings = 4ull * 1024 * 1024;
    in.lastAttemptChurn = -1.0;
    return in;
}

} // namespace

TEST_CASE("save policy: the proactive fold fires only with every gate open") {
    CHECK(proactiveFoldWanted(passing()));

    // Benefit gate, half 1: the reuse fraction must clear the SAME switch-up threshold the
    // fold's own hysteresis applies -- a proactive fold the fold would then decline to switch
    // must never fire.
    {
        ProactiveFoldInputs in = passing();
        in.churnFraction = nio::kSwitchUp - 0.01;
        CHECK(!proactiveFoldWanted(in));
        in.churnFraction = nio::kSwitchUp;
        CHECK(proactiveFoldWanted(in));
    }
    // Benefit gate, half 2: the absolute floor -- a tiny file's high ratio saves less than the
    // fold costs to write.
    {
        ProactiveFoldInputs in = passing();
        in.projectedSavings = kProactiveMinSavingsBytes - 1;
        CHECK(!proactiveFoldWanted(in));
        in.projectedSavings = kProactiveMinSavingsBytes;
        CHECK(proactiveFoldWanted(in));
    }
    // The stall gate: fold cost is O(file); past the cap the passive compaction fold -- off the
    // Ctrl+S critical path by threshold -- realizes the switch instead.
    {
        ProactiveFoldInputs in = passing();
        in.fileSize = kProactiveFoldMaxBytes + 1;
        CHECK(!proactiveFoldWanted(in));
    }
    // The throttle: never two attempts without the signal meaningfully moving.
    {
        ProactiveFoldInputs in = passing();
        in.lastAttemptChurn = in.churnFraction;
        CHECK(!proactiveFoldWanted(in));
        in.lastAttemptChurn = in.churnFraction - kProactiveChurnDelta + 0.01;
        CHECK(!proactiveFoldWanted(in));
        in.lastAttemptChurn = in.churnFraction - kProactiveChurnDelta - 0.001;
        CHECK(proactiveFoldWanted(in));
        in.lastAttemptChurn = in.churnFraction + kProactiveChurnDelta + 0.001; // down counts too
        CHECK(proactiveFoldWanted(in));
    }
    // cas never folds proactively: only the up-switch has an early benefit.
    {
        ProactiveFoldInputs in = passing();
        in.mode = nio::kModeCas;
        CHECK(!proactiveFoldWanted(in));
    }
}
