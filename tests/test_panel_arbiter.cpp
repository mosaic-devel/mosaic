// The unified corner-panel decision core (S32 round 5, docs/adjustment-layers.md §5): pure
// logic, so every panel-fight class that used to need interactive repro pins headlessly --
// explicit beats conditional, explicit-over-explicit replaces (the shared-corner exclusivity),
// closing promotes the queued conditional, Esc suppresses exactly the dismissed context until
// it changes, and a dead context (the text session ending) evaporates the explicit request.
#include "ui/panel_arbiter.hpp"

#include <doctest/doctest.h>

using mosaic::ui::PanelArbiter;

namespace {
constexpr int kStyle = 1;
constexpr int kType3d = 2;
constexpr int kAdjust = 3;
} // namespace

TEST_CASE("explicit requests outrank conditions; closing promotes the queue") {
    bool session = true;
    std::uint64_t adjLayer = 7; // the active adjustment layer's id (0 = none)
    PanelArbiter a;
    a.addExplicit(kStyle, [&] { return session; });
    a.addExplicit(kType3d, [&] { return session; });
    a.addConditional(kAdjust, [&] { return adjLayer; });

    // The condition alone shows the adjustment panel.
    CHECK(a.resolve() == kAdjust);

    // A button click wins over the condition...
    a.toggle(kStyle);
    CHECK(a.resolve() == kStyle);
    // ...toggling the OTHER explicit panel replaces it (Style/3D share the corner)...
    a.toggle(kType3d);
    CHECK(a.resolve() == kType3d);
    // ...and re-toggling closes it, which PROMOTES the queued conditional panel.
    a.toggle(kType3d);
    CHECK(a.resolve() == kAdjust);

    // With no condition either, nothing shows.
    adjLayer = 0;
    CHECK(a.resolve() == std::nullopt);
}

TEST_CASE("an explicit request evaporates when its context dies") {
    bool session = true;
    std::uint64_t adjLayer = 7;
    PanelArbiter a;
    a.addExplicit(kStyle, [&] { return session; });
    a.addConditional(kAdjust, [&] { return adjLayer; });

    a.toggle(kStyle);
    CHECK(a.resolve() == kStyle);
    session = false; // the text session ended
    CHECK(a.resolve() == kAdjust); // the request is gone, the queue takes over
    session = true;  // a NEW session does not resurrect the old request
    CHECK(a.resolve() == kAdjust);
}

TEST_CASE("dismissing a conditional panel suppresses exactly that context") {
    std::uint64_t adjLayer = 7;
    PanelArbiter a;
    a.addConditional(kAdjust, [&] { return adjLayer; });

    CHECK(a.resolve() == kAdjust);
    a.hidden(kAdjust); // the user Esc-closed it
    CHECK(a.resolve() == std::nullopt); // stays away for THIS layer...
    CHECK(a.resolve() == std::nullopt); // ...frame after frame

    adjLayer = 9; // ...but selecting ANOTHER adjustment layer re-arms it
    CHECK(a.resolve() == kAdjust);

    a.hidden(kAdjust);
    adjLayer = 0; // the condition lapsing (a non-adjustment selected) also re-arms...
    CHECK(a.resolve() == std::nullopt);
    adjLayer = 9; // ...so coming BACK to the same layer reopens (the seen-latch behavior)
    CHECK(a.resolve() == kAdjust);
}

TEST_CASE("hidden() on an explicit panel clears the request (theme close, bar rebuild)") {
    PanelArbiter a;
    a.addExplicit(kStyle);
    a.toggle(kStyle);
    CHECK(a.resolve() == kStyle);
    a.hidden(kStyle);
    CHECK(a.resolve() == std::nullopt);
}

TEST_CASE("conditional precedence picks the strongest claim") {
    std::uint64_t low = 1, high = 1;
    PanelArbiter a;
    a.addConditional(kAdjust, [&] { return low; }, /*precedence=*/0);
    a.addConditional(kType3d, [&] { return high; }, /*precedence=*/1);
    CHECK(a.resolve() == kType3d);
    high = 0;
    CHECK(a.resolve() == kAdjust);
}
