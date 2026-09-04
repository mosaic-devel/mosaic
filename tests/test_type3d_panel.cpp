#include "core/text/extrude.hpp"
#include "ui/type3d_panel.hpp"
#include "ui/type_panel.hpp"

#include <FL/Fl_Group.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Window.H>
#include <doctest/doctest.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace {

struct Box {
    int x, y, w, h;
    bool operator==(const Box&) const = default;
};

// Every descendant widget's box, depth-first -- the layout fingerprint a resize round-trip must
// preserve exactly. An Fl_Scroll's own scrollbars are excluded: it repositions them lazily at
// draw time, so headlessly (never drawn) their boxes reflect construction order, not layout.
void collectBoxes(const Fl_Group* g, std::vector<Box>& out) {
    const auto* sc = dynamic_cast<const Fl_Scroll*>(g);
    for (int i = 0; i < g->children(); ++i) {
        const Fl_Widget* c = g->child(i);
        if (sc != nullptr && (c == &sc->scrollbar || c == &sc->hscrollbar)) continue;
        out.push_back({c->x(), c->y(), c->w(), c->h()});
        if (const auto* sub = dynamic_cast<const Fl_Group*>(c)) collectBoxes(sub, out);
    }
}

std::vector<Box> boxesOf(const Fl_Window& w) {
    std::vector<Box> out;
    collectBoxes(&w, out);
    return out;
}

}  // namespace

// Layout regression for the 3D popup (S30-d round 3: "massive padding at the bottom"). The panel
// is a child sub-window; building it (via reapplyTheme) without show() is safe headlessly, and
// the built height must hug the content -- the construction-time estimate must never leak into
// what the user sees.
TEST_CASE("the 3D popup's built height hugs its content") {
    mosaic::ui::Type3dPanel panel;
    panel.reapplyTheme(); // build() with the active palette
    CHECK(panel.w() == 300);
    // S30-e feedback: the content lives in a ScrollView now, so the panel window itself no longer
    // has to fit every row -- but the default footprint still hugs the content when space allows.
    CHECK(panel.h() <= 635);
    CHECK(panel.h() >= 400);
}

// The panel edits an `std::optional<Extrude>` and knows nothing about where it lives -- that is
// what lets ONE popup serve the Type tool (a TextBlock's) and the shape/pen tools (a
// vec::Object's; docs/vector-model.md §11). Drive the controls through the public thunk and assert
// the funnel is handed the optional itself.
//
// applyControl APPLIES the control's current state (it is the widget's callback, not a toggle),
// and reflect() is what puts the controls into a state -- so each case reflects the state the user
// would be looking at, then fires the control.
TEST_CASE("the 3D popup's edit funnel writes through an optional Extrude") {
    using Extrude = mosaic::core::text::Extrude;
    mosaic::ui::Type3dPanel panel;
    panel.reapplyTheme(); // build()

    // The subject is any optional<Extrude> the host cares to own. Here it is a local, which is the
    // whole point: the panel has no idea it is not a TextBlock's or a vec::Object's.
    std::optional<Extrude> subject;
    std::string lastId;
    int edits = 0;
    panel.setOnExtrudeEdit(
        [&](const std::string& id, const std::function<void(std::optional<Extrude>&)>& mut) {
            lastId = id;
            ++edits;
            mut(subject);
        });

    constexpr int kEnable = 0; // Role::Enable
    constexpr int kDepth = 1;  // Role::Depth

    // Enable OFF (the box reflects as unchecked) clears the optional.
    subject = Extrude{};
    panel.reflect(std::nullopt, /*hasSession=*/true);
    panel.applyControl(kEnable);
    CHECK(edits == 1);
    CHECK_FALSE(subject.has_value());
    CHECK(lastId == "extrude:on"); // every edit carries its coalescing id

    // Enable ON (checked) creates one where there was none.
    subject.reset();
    panel.reflect(Extrude{}, /*hasSession=*/true);
    panel.applyControl(kEnable);
    CHECK(edits == 2);
    CHECK(subject.has_value());

    // A VALUE edit with 3D off is a stale event -- dropped, never resurrecting the optional.
    subject.reset();
    panel.reflect(std::nullopt, /*hasSession=*/true);
    panel.applyControl(kDepth);
    CHECK_FALSE(subject.has_value());

    // ... and with 3D on it writes through to the value the controls were seeded from.
    Extrude seeded;
    seeded.depth = 37.5f;
    subject = Extrude{}; // deliberately NOT the seeded one: the write must come from the control
    panel.reflect(seeded, /*hasSession=*/true);
    panel.applyControl(kDepth);
    REQUIRE(subject.has_value());
    CHECK(subject->depth == doctest::Approx(37.5f));
    CHECK(lastId == "extrude:depth");
}

// S30-e feedback round ("style gets the inner controls pushed to the left while 3d shrinks them
// to about a quarter of the panel's size"): a shown popover is STRETCHED by the main window's
// group-resize on every interactive resize event, then place()/reanchor() snaps it back to its
// base size. With the popover's own resizable() left at the group default (Fl_Group::clear()
// RESETS it -- the classic trap), each stretch/restore cycle proportionally scales the children
// and integer rounding compounds the drift until the controls collapse. The round-trip must be
// the identity, cycle after cycle.
TEST_CASE("popover stretch/restore cycles leave the panel layouts untouched") {
    const auto roundTrips = [](Fl_Window& panel) {
        const int bw = panel.w(), bh = panel.h();
        const std::vector<Box> before = boxesOf(panel);
        for (int i = 0; i < 40; ++i) {
            panel.resize(panel.x(), panel.y(), bw + 137, bh + 91);  // the parent's group-stretch
            panel.resize(panel.x(), panel.y(), bw, bh);             // place()'s base-size restore
        }
        return before == boxesOf(panel);
    };
    mosaic::ui::Type3dPanel p3d;
    p3d.reapplyTheme();
    CHECK(roundTrips(p3d));

    mosaic::ui::TypePanel style;
    style.reapplyTheme();
    CHECK(roundTrips(style));
}

// The actual corruption path: a hidden panel is stretched by the main window's group-resize, and
// build() then runs INSIDE that stretched box (first open, or a theme change) -- the fresh
// design-coordinate children and the oversized window box become the resize baseline together,
// so the show-time restore to the base size scales the controls down. build() must normalize the
// window to its own footprint first, making "stretched, rebuilt, restored" land exactly where a
// clean build lands.
TEST_CASE("building while group-stretched still yields the design layout at base size") {
    const auto scenario = [](auto& panel, auto& reference) {
        reference.reapplyTheme();
        panel.reapplyTheme();
        panel.resize(0, 0, panel.w() * 4, panel.h() + 315);  // hidden + main window maximized
        panel.reapplyTheme();                                // rebuilt while stretched
        panel.resize(0, 0, reference.w(), reference.h());    // place()'s base-size restore on show
        return boxesOf(panel) == boxesOf(reference);
    };
    mosaic::ui::Type3dPanel p3d, p3dRef;
    CHECK(scenario(p3d, p3dRef));
    mosaic::ui::TypePanel style, styleRef;
    CHECK(scenario(style, styleRef));
}
