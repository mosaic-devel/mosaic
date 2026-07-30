#pragma once

#include <optional>
#include <string_view>

class Fl_Window; // forward-declared: this header stays FLTK-free

// A tiny themed, modal "N px" numeric prompt (S18, docs/research-select-brush.md §9-C): the entry UI
// for the Select menu's Grow / Shrink / Feather / Smooth ops. One outlined value field + OK / Cancel,
// blocking until dismissed with the entered value. Requires a display.
namespace mosaic::ui {

// Show the prompt (`title` the headline, `prompt` the field caption, e.g. "Grow by:"), seeded at
// `initial` and clamped to [min, max], centred over `host` (the pointer's screen when null).
// Returns the entered integer pixel amount, or nullopt if the user cancelled. Enter confirms,
// Esc cancels.
[[nodiscard]] std::optional<int> showSelectModifyDialog(std::string_view title,
                                                        std::string_view prompt, int initial,
                                                        int min, int max,
                                                        Fl_Window* host = nullptr);

} // namespace mosaic::ui
