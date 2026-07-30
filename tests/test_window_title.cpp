#include "ui/window_title.hpp"

#include <doctest/doctest.h>

#include <string>

// S18-d: the pure window-title formatter + unsaved-duration rendering (golden strings). The
// clean/dirty transitions themselves live on the command stack (test_commands.cpp).
namespace {
using mosaic::ui::formatUnsavedDuration;
using mosaic::ui::formatWindowTitle;
using mosaic::ui::UnsavedTitleFormat;

const std::string kDash = " \xE2\x80\x94 Mosaic";       // " — Mosaic"
const std::string kUnsaved = " \xE2\x80\xA2 unsaved";   // " • unsaved"
} // namespace

TEST_CASE("a clean document shows just its name and the app") {
    CHECK(formatWindowTitle("photo.png", /*dirty=*/false, -1, {}) == "photo.png" + kDash);
}

TEST_CASE("an empty name falls back to Untitled") {
    CHECK(formatWindowTitle("", /*dirty=*/false, -1, {}) == "Untitled" + kDash);
    CHECK(formatWindowTitle("", /*dirty=*/true, -1, {}) == "Untitled" + kUnsaved + kDash);
}

TEST_CASE("a fresh edit shows a bare unsaved, no duration yet") {
    UnsavedTitleFormat fmt; // showDuration on, threshold 300 s
    // Under the threshold: only the bare "• unsaved".
    CHECK(formatWindowTitle("a.png", true, 0, fmt) == "a.png" + kUnsaved + kDash);
    CHECK(formatWindowTitle("a.png", true, 299, fmt) == "a.png" + kUnsaved + kDash);
    // -1 means "unknown how long": also no duration.
    CHECK(formatWindowTitle("a.png", true, -1, fmt) == "a.png" + kUnsaved + kDash);
}

TEST_CASE("the duration joins once past the threshold, at minute granularity") {
    UnsavedTitleFormat fmt;
    CHECK(formatWindowTitle("a.png", true, 300, fmt) == "a.png" + kUnsaved + " for 5 min" + kDash);
    CHECK(formatWindowTitle("a.png", true, 12 * 60 + 45, fmt) ==
          "a.png" + kUnsaved + " for 12 min" + kDash); // seconds floored away
}

TEST_CASE("showDuration off keeps the title bare forever") {
    UnsavedTitleFormat fmt;
    fmt.showDuration = false;
    CHECK(formatWindowTitle("a.png", true, 9999, fmt) == "a.png" + kUnsaved + kDash);
}

TEST_CASE("includeSeconds ticks at second granularity") {
    UnsavedTitleFormat fmt;
    fmt.includeSeconds = true;
    CHECK(formatWindowTitle("a.png", true, 5 * 60 + 12, fmt) ==
          "a.png" + kUnsaved + " for 5 min 12 sec" + kDash);
}

TEST_CASE("displayDocumentName strips only the native extension") {
    using mosaic::ui::displayDocumentName;
    // ".mosaic" is redundant next to the trailing "— Mosaic" chrome; any case, once.
    CHECK(displayDocumentName("stuff.mosaic") == "stuff");
    CHECK(displayDocumentName("stuff.MOSAIC") == "stuff");
    CHECK(displayDocumentName("a.mosaic.mosaic") == "a.mosaic");
    // Foreign extensions carry information (an image-backed document) and stay.
    CHECK(displayDocumentName("photo.png") == "photo.png");
    CHECK(displayDocumentName("notmosaic") == "notmosaic");
    // The bare extension is not a strippable name -- never render an empty title.
    CHECK(displayDocumentName(".mosaic") == ".mosaic");
    CHECK(displayDocumentName("") == "");
}

TEST_CASE("formatUnsavedDuration renders minutes and optional seconds") {
    CHECK(formatUnsavedDuration(0, false) == "0 min");
    CHECK(formatUnsavedDuration(59, false) == "0 min");
    CHECK(formatUnsavedDuration(60, false) == "1 min");
    CHECK(formatUnsavedDuration(305, false) == "5 min");
    // With seconds: a full "M min S sec", or just seconds under a minute.
    CHECK(formatUnsavedDuration(305, true) == "5 min 5 sec");
    CHECK(formatUnsavedDuration(42, true) == "42 sec");
    CHECK(formatUnsavedDuration(-3, true) == "0 sec"); // clamped
    // A custom (0 s) threshold would show the duration immediately.
    CHECK(formatUnsavedDuration(0, false) == "0 min");
}
