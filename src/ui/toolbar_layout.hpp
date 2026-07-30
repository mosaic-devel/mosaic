#pragma once

#include <cstddef>
#include <vector>

// Pure layout maths for the left toolbar's vertical overflow (PLAN S16-o), split out so it can be
// unit-tested without FLTK. The toolbar shows tool *slots* top-to-bottom in a fixed natural order
// (most-used first); when the column is too short, the lowest (= least-used) slots move into an
// overflow popover, with the active tool always kept visible.
namespace mosaic::ui {

struct ToolbarSplit {
    std::vector<std::size_t> visible;  // slot indices kept in the column, in display order
    std::vector<std::size_t> overflow; // slot indices moved into the overflow popover, natural order
};

// Split `total` slots (indices 0..total-1, least-used last) given that only `fit` of them fit in the
// column. `fit` is clamped to [1, total]; if `fit >= total` nothing overflows. The active slot
// (`activeIndex`) is always kept visible: if it would land in the overflow it takes the LAST visible
// position and the slot it displaces moves to overflow -- so both lists stay in ascending natural
// order and at most one slot ever "jumps". `total == 0` yields empty lists.
[[nodiscard]] ToolbarSplit splitToolbarSlots(std::size_t total, std::size_t fit,
                                             std::size_t activeIndex);

} // namespace mosaic::ui
