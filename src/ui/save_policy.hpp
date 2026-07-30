#pragma once

#include "io/mosaic/compaction.hpp"

#include <cmath>
#include <cstdint>
#include <string>

// mosaic/ui/save_policy -- the PROACTIVE early-fold decision (S48 Build 2 ruling): a plain
// Ctrl+S may voluntarily perform the history-preserving full write to realize a beneficial
// journal->cas switch EARLY, instead of waiting for the parity-debt threshold to force one.
// Pure so it is headlessly testable, like recovery_flow's classifier; MainWindow::saveDocument
// gathers the inputs and carries out the choice.
//
// Every gate is a ruling, none optional:
//   benefit  -- BOTH the whole-history reuse fraction past switch-up (the same signal and
//               threshold the fold itself will apply -- a proactive fold that the fold's own
//               hysteresis would then decline must never fire) AND an absolute floor of
//               projected duplicate bytes, so a tiny file's high ratio cannot trigger a fold
//               that saves less than it costs to write;
//   stall    -- a document-size cap: fold cost is O(file), measured at ~20 MB/s end to end
//               (open + fold + parity), so the cap bounds the worst proactive stall at a few
//               hundred milliseconds; a larger file simply waits for the passive compaction
//               fold, which the parity-debt threshold guarantees eventually fires;
//   throttle -- never two proactive attempts without a meaningful change in the churn signal
//               in between (a fold that could not be built must not retry every Ctrl+S).
//
// cas->journal is deliberately NEVER proactive: below switch-down the two encodings are near
// the same size, so switching down early realizes nothing -- it rides the next passive fold.
namespace mosaic::ui {

inline constexpr std::uint64_t kProactiveFoldMaxBytes = 8ull * 1024 * 1024;
inline constexpr std::uint64_t kProactiveMinSavingsBytes = 1ull * 1024 * 1024;
inline constexpr double kProactiveChurnDelta = 0.05;

struct ProactiveFoldInputs {
    std::string mode;                // the file's history encoding as of the anchor
    std::uint64_t fileSize = 0;      // the file on disk, per the commit tip
    double churnFraction = 0.0;      // the live whole-history signal (ChurnTracker::fraction)
    std::uint64_t projectedSavings = 0; // duplicate bytes cas would stop storing (pre-compression)
    double lastAttemptChurn = -1.0;  // the signal at the last proactive attempt; < 0 = never
};

[[nodiscard]] inline bool proactiveFoldWanted(const ProactiveFoldInputs& in) {
    if (in.mode == io::native::kModeCas)
        return false; // only the up-switch has a benefit to realize early
    if (in.fileSize > kProactiveFoldMaxBytes)
        return false;
    if (in.churnFraction < io::native::kSwitchUp)
        return false;
    if (in.projectedSavings < kProactiveMinSavingsBytes)
        return false;
    if (in.lastAttemptChurn >= 0.0 &&
        std::abs(in.churnFraction - in.lastAttemptChurn) < kProactiveChurnDelta)
        return false;
    return true;
}

} // namespace mosaic::ui
