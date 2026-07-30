#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/text/spell_scan.hpp"
#include "core/text/text_model.hpp"

// Background spell-checker (docs/spell-check-plan.md commit 2, decision D1): the UI thread hands it a
// TextBlock snapshot to scan and later polls for the misspelled ranges, so a scan never blocks
// typing. This is the thin threading shell around the pure scanBlockSpelling (commit 2a); the UI
// wiring that feeds it snapshots and paints the result is manually verified.
//
// One worker thread COALESCES requests -- only the newest queued request is ever scanned -- and tags
// each result with the request's EPOCH. The UI bumps the epoch on every block change, so it can tell
// whether a returned result still matches the current text: it applies a result only when
// result.epoch equals its current epoch, and discards anything staler (the block moved on while the
// scan ran). Combined with coalescing, that is the epoch-cancel the plan calls for.
//
// The enchant SpellChecker is owned HERE and guarded by a mutex, so the UI's suggest / addToUserDict
// / ignore calls (the right-click menu, commit 4) serialize with the worker's scans on the one
// broker -- there is never cross-thread enchant access. Poll it from the UI thread (e.g. a re-armed
// FLTK timeout, as the inpaint job does); the worker never touches FLTK. FLTK-free and self-contained.
namespace mosaic::core::text {

class SpellCheckWorker {
public:
    SpellCheckWorker();   // spawns the worker thread
    ~SpellCheckWorker();  // signals + joins it
    SpellCheckWorker(const SpellCheckWorker&) = delete;
    SpellCheckWorker& operator=(const SpellCheckWorker&) = delete;
    SpellCheckWorker(SpellCheckWorker&&) = delete;
    SpellCheckWorker& operator=(SpellCheckWorker&&) = delete;

    // Queue a scan of `block` (copied, so the caller may keep editing) for paragraphs
    // [paraFirst, paraLast], tagged `epoch`. Supersedes any still-pending request. Thread-safe;
    // returns immediately without waiting for the scan.
    void request(TextBlock block, std::string documentDefault, std::string appDefault,
                 SpellScanOptions opts, std::uint64_t epoch, std::size_t paraFirst = 0,
                 std::size_t paraLast = std::numeric_limits<std::size_t>::max());

    struct Result {
        std::uint64_t epoch = 0;
        std::vector<MisspelledRange> ranges;
    };
    // The most recently COMPLETED scan not yet taken, or nullopt. Non-blocking. The UI keeps the
    // ranges only when result.epoch matches its current epoch (else a newer edit is already queued).
    [[nodiscard]] std::optional<Result> takeResult();

    // Dictionary queries for the right-click menu (commit 4), serialized with scans on the one
    // broker. Safe to call from the UI thread while the worker runs.
    [[nodiscard]] std::vector<std::string> suggest(std::string_view word, std::string_view language);
    void addToUserDict(std::string_view word, std::string_view language);
    void ignore(std::string_view word);
    [[nodiscard]] bool hasDictionary(std::string_view language);

    // Test hook: inject a mock dictionary (thread-safe wrapper over SpellChecker::loadMockDictionary),
    // so the worker's threading/epoch behaviour is testable without an installed system dictionary.
    void loadMockDictionary(
        std::string_view language, std::vector<std::string> misspelled,
        std::vector<std::pair<std::string, std::vector<std::string>>> suggestions = {});

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mosaic::core::text
