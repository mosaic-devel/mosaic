#include "ui/canvas_view.hpp"
#include "ui/tab_strip.hpp"

#include <doctest/doctest.h>

#include <numeric>
#include <vector>

// The S49 tab strip's pure half: how N tabs share the canvas column's width, and the view state a
// tab remembers while it is off screen. The FLTK event plumbing (hover, the close X, wheel scroll)
// is exercised by the --gui-frames smoke run instead.

using mosaic::ui::fitTabWidths;
using mosaic::ui::kTabMaxWidth;
using mosaic::ui::kTabMinWidth;

namespace {
long sum(const std::vector<int>& v) { return std::accumulate(v.begin(), v.end(), 0L); }
} // namespace

TEST_CASE("fitTabWidths leaves tabs alone when they already fit") {
    const std::vector<int> natural{120, 160, 100};
    CHECK(fitTabWidths(natural, 1000) == natural);
    CHECK(fitTabWidths(natural, sum(natural)) == natural); // exactly full is still a fit
    CHECK(fitTabWidths({}, 500).empty());
}

TEST_CASE("fitTabWidths shrinks proportionally when the strip overflows") {
    const std::vector<int> natural{200, 200, 200, 200}; // 800 wanted
    const std::vector<int> got = fitTabWidths(natural, 600);
    CHECK(got.size() == 4);
    for (const int w : got) {
        CHECK(w < 200);
        CHECK(w >= kTabMinWidth);
    }
    CHECK(sum(got) <= 600);
    // Equal inputs stay equal: no tab is arbitrarily favoured.
    for (std::size_t i = 1; i < got.size(); ++i)
        CHECK(got[i] == got[0]);
}

TEST_CASE("fitTabWidths never shrinks a tab past the legibility floor -- the strip scrolls instead") {
    // Twenty tabs in a 400px column: proportional scaling alone would give each 20px.
    const std::vector<int> natural(20, kTabMaxWidth);
    const std::vector<int> got = fitTabWidths(natural, 400);
    for (const int w : got)
        CHECK(w == kTabMinWidth);
    CHECK(sum(got) > 400); // deliberately overflows: the caller scrolls the excess
}

TEST_CASE("fitTabWidths handles a degenerate column without dividing by zero") {
    const std::vector<int> natural{150, 150};
    for (const int avail : {0, -10}) {
        const std::vector<int> got = fitTabWidths(natural, avail);
        CHECK(got == natural); // nothing sensible to shrink into; the caller clips
    }
}

TEST_CASE("a tab's view state round-trips exactly, and clamps only the zoom") {
    mosaic::ui::CanvasView view;
    view.setDocumentSize({800, 600});
    view.setViewportSize({400, 300});
    view.fit();
    view.rotateBy(0.4);
    view.panByScreen({17.0, -9.0});

    const mosaic::ui::CanvasView::ViewState saved = view.state();

    // Something else happens to the view while the tab is off screen...
    view.reset();
    view.setViewportSize({1000, 1000}); // ... including a window resize
    CHECK(view.state().rotation == doctest::Approx(0.0));

    view.setState(saved); // ... and switching back restores exactly where it was left
    CHECK(view.state().zoom == doctest::Approx(saved.zoom));
    CHECK(view.state().rotation == doctest::Approx(saved.rotation));
    CHECK(view.state().pan.x == doctest::Approx(saved.pan.x));
    CHECK(view.state().pan.y == doctest::Approx(saved.pan.y));

    // The zoom is the one field with a legal range; pan and rotation are free.
    view.setState({1e9, 100.0, {1e6, -1e6}});
    CHECK(view.zoom() == doctest::Approx(mosaic::ui::CanvasView::kMaxZoom));
    view.setState({0.0, 0.0, {0.0, 0.0}});
    CHECK(view.zoom() == doctest::Approx(mosaic::ui::CanvasView::kMinZoom));
}
