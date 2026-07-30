#pragma once

#include <cstddef>

// The content of the "Cheesy motivational one-liners" annoyance (Settings -> Annoyances): 100
// all-caps English one-liners that drift diagonally UNDER the canvas when the toggle is on. They
// live in their OWN gettext domain ("motivate") so 100 jokes never pollute po/mosaic.pot (the main
// translator's catalog). Each is wrapped in MOTIVATE_(): an extraction-only marker -- identity at
// runtime, like N_ -- that the dedicated `pot-motivate` xgettext target scans (via
// --keyword=MOTIVATE_) into po/motivate/motivate.pot. The actual translation happens at the point
// of use, in randomMotivationalLine(), via i18n::dtr("motivate", line). See po/motivate/README.md.
#ifndef MOTIVATE_
#  define MOTIVATE_(s) (s)  // mark for extraction into the "motivate" catalog; no-op at runtime
#endif

namespace mosaic::ui {

// A uniformly-random one-liner, translated through the "motivate" domain (the English msgid when no
// catalog is installed). The returned pointer is owned by gettext / the static table and is stable.
[[nodiscard]] const char* randomMotivationalLine();

// The number of one-liners available (for tests / sanity checks).
[[nodiscard]] std::size_t motivationalLineCount();

}  // namespace mosaic::ui
