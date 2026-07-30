#include "ui/toolbar_layout.hpp"

#include <doctest/doctest.h>

#include <cstddef>

// The pure left-toolbar overflow split (ui::splitToolbarSlots): least-used (bottom) slots overflow
// first, the active tool is always kept visible, and both lists stay in ascending natural order.
namespace {

using mosaic::ui::splitToolbarSlots;
using mosaic::ui::ToolbarSplit;

std::vector<std::size_t> seq(std::size_t a, std::size_t b) { // [a, b)
    std::vector<std::size_t> v;
    for (std::size_t i = a; i < b; ++i)
        v.push_back(i);
    return v;
}

} // namespace

TEST_CASE("splitToolbarSlots: everything fits -> no overflow") {
    const ToolbarSplit s = splitToolbarSlots(12, 12, 0);
    CHECK(s.visible == seq(0, 12));
    CHECK(s.overflow.empty());
    // fit beyond total behaves the same.
    CHECK(splitToolbarSlots(5, 9, 2).overflow.empty());
}

TEST_CASE("splitToolbarSlots: active already visible -> plain prefix/suffix split") {
    const ToolbarSplit s = splitToolbarSlots(12, 8, 3); // active in the visible prefix
    CHECK(s.visible == seq(0, 8));
    CHECK(s.overflow == seq(8, 12));
}

TEST_CASE("splitToolbarSlots: active in overflow is pulled into the last visible slot") {
    const ToolbarSplit s = splitToolbarSlots(12, 8, 10); // active would have overflowed
    // visible = first 7 + the active (still ascending); the displaced slot 7 drops to overflow.
    CHECK(s.visible == std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 6, 10});
    CHECK(s.overflow == std::vector<std::size_t>{7, 8, 9, 11});
    CHECK(s.visible.size() == 8); // capacity preserved
}

TEST_CASE("splitToolbarSlots: tiny column keeps only the active tool") {
    const ToolbarSplit s = splitToolbarSlots(12, 1, 5);
    CHECK(s.visible == std::vector<std::size_t>{5});
    CHECK(s.overflow == std::vector<std::size_t>{0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11});
}

TEST_CASE("splitToolbarSlots: degenerate inputs") {
    CHECK(splitToolbarSlots(0, 4, 0).visible.empty()); // no slots
    const ToolbarSplit one = splitToolbarSlots(3, 0, 1); // fit clamps up to 1, active kept
    CHECK(one.visible == std::vector<std::size_t>{1});
    CHECK(one.overflow == std::vector<std::size_t>{0, 2});
}
