#pragma once

#include <string>
#include <string_view>

// The window-title string machinery (S18-d). Pure and FLTK-free so the formatting + the
// unsaved-duration rendering are golden-testable; MainWindow feeds it the document name, the dirty
// state (from the command stack's saved marker) and how long the document has been unsaved, and
// pushes the result to the window label only when it changes.
namespace mosaic::ui {

// The Annoyances settings that shape the "• unsaved" clause.
struct UnsavedTitleFormat {
    // "Show how long the document has been unsaved in the title" (on by default). Off = the bare
    // "• unsaved" with no duration, ever.
    bool showDuration = true;
    // "…include seconds" (off by default): tick the duration every second ("5 min 12 sec") instead
    // of once a minute ("5 min"). A per-second title re-render is motion in the eye-line, so it is
    // opt-in.
    bool includeSeconds = false;
    // The duration only JOINS the title once the document has been unsaved this long (default 5
    // min, at minute granularity below); a fresh edit shows a bare "• unsaved" first.
    int thresholdSeconds = 300;

    // The words. They are PASSED IN rather than looked up inside the formatter so it stays pure:
    // a function that reads the active locale could not be golden-tested without the tests
    // inheriting whatever catalog happened to be bound. MainWindow fills these from _(); the
    // English defaults are what the tests (and any caller that does not care) get.
    //
    // `unsaved` and `untitled` are whole words. `durationFor` is the connective before the
    // duration, and `minutes`/`seconds` are the unit abbreviations -- kept as separate pieces
    // rather than one format string because the caller assembles counts into them and a
    // translator reordering a "%d min %d sec" pattern would swap the two numbers' meanings.
    std::string_view untitled = "Untitled";
    std::string_view unsaved = "unsaved";
    std::string_view durationFor = "for";
    std::string_view minutes = "min";
    std::string_view seconds = "sec";
};

// Render the unsaved duration clause body (what follows "for"), e.g. "5 min" or, with seconds,
// "5 min 12 sec" / "42 sec". Exposed for unit testing. `seconds` is clamped at 0. The unit words
// default to English so existing callers and tests are unaffected.
[[nodiscard]] std::string formatUnsavedDuration(int seconds, bool includeSeconds,
                                                std::string_view minuteUnit = "min",
                                                std::string_view secondUnit = "sec");

// The document name as the title shows it: a trailing ".mosaic" (any case) is dropped — it is
// redundant next to the trailing "— Mosaic" and reads oddly ("stuff.mosaic — Mosaic"). Foreign
// extensions stay: "photo.png" tells the user this document is still backed by an image file.
[[nodiscard]] std::string displayDocumentName(std::string_view fileName);

// Build the whole window title:
//   clean            -> "<name> — Mosaic"
//   dirty, brief     -> "<name> • unsaved — Mosaic"
//   dirty, long      -> "<name> • unsaved for 12 min — Mosaic"
// The document name comes first, because taskbars and alt-tab truncate from the right. `dirty`
// gates the "• unsaved" clause; `unsavedSeconds` (< 0 when clean/unknown) plus `fmt` decide whether
// (and how) the "for <duration>" tail appears.
[[nodiscard]] std::string formatWindowTitle(std::string_view docName, bool dirty, int unsavedSeconds,
                                            const UnsavedTitleFormat& fmt);

} // namespace mosaic::ui
