#pragma once

#include "io/mosaic/salvage.hpp"
#include "io/mosaic/save.hpp"

#include <cstddef>
#include <cstdint>

// Open-time recovery classification (docs/askortell-dialog.md, "Recovery-family flows"): the pure
// decision of WHICH flow a damaged .mosaic open triggers, split out from the FLTK presentation in
// app_window.cpp so it can be exercised headlessly (tests/test_recovery_flow.cpp). The reader and
// salvage engine compute the facts; this maps them to the settled UX; app_window renders the face
// and carries out the choice. Journal flows (1/2) are not open-report classification -- they key
// on the recovery journal and arrive with that slice.
namespace mosaic::ui {

enum class RecoveryFlow {
    None,         // clean, or nothing worth telling the user
    Repaired,     // 3a: parity fixed everything, nothing lost -> status line, no dialog
    Damaged,      // 3b: checkpoint areas lost beyond parity -> tell
    Recover,      // 3c: committed-region damage with intact saves past it -> the one ASK
    TornTail,     // 3d: an unfinished save at the tail, nothing recoverable beyond it -> tell
    BadlyDamaged, // 3e: no clean fallback (roots/index gone; full-scan reassembly) -> tell
    DualWriter,   // 4:  two writers saved into one file, both intact -> ASK
};

// Classify one open. `checkpointLost` folds the container's lostEntries with the document layer's
// rejectedChunks (both are areas that came back unreadable). `salvage` is the result of
// salvageLinkedRegion over the committed region; pass nullptr when the caller has not probed
// (only a committed anomaly warrants the probe). The precedence is deliberate: a destroyed
// structure (3e) outranks a committed-region question, which outranks checkpoint loss (3b),
// which outranks a silent parity repair (3a).
[[nodiscard]] inline RecoveryFlow classifyRecoveryFlow(const io::native::OpenReport& report,
                                                       std::size_t checkpointLost,
                                                       const io::native::SalvageReport* salvage) {
    if (!report.base.rootFound && report.base.usedFullScan)
        return RecoveryFlow::BadlyDamaged;

    if (report.committedAnomaly) {
        if (salvage == nullptr)
            return RecoveryFlow::TornTail; // unprobed anomaly: the safe tell, never a false ask
        if (salvage->rootConflict)
            return RecoveryFlow::DualWriter;
        const io::native::SalvageLineage* prim = salvage->primary();
        const std::uint64_t conservativeLast = report.commits.empty() ? 0 : report.commits.back();
        const std::uint64_t newestRecovered =
            (prim == nullptr || prim->states.empty()) ? conservativeLast : prim->states.back();
        if (prim != nullptr && !prim->states.empty() && newestRecovered > conservativeLast)
            return RecoveryFlow::Recover; // genuinely more content past the gap than conservative
        return RecoveryFlow::TornTail;    // torn tail, or nothing salvage reached past it
    }

    if (checkpointLost > 0)
        return RecoveryFlow::Damaged;
    if (report.base.rsReconstructed > 0)
        return RecoveryFlow::Repaired;
    return RecoveryFlow::None;
}

} // namespace mosaic::ui
