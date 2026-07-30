#include "common/image.hpp"
#include "ui/canvas_view.hpp"
#include "ui/pen_gesture.hpp"
#include "ui/tool.hpp"
#include "ui/vulkan_canvas.hpp"

#include <doctest/doctest.h>

#include <FL/Enumerations.H>
#include <FL/Fl.H>

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// S28 -- the Pen tool's COORDINATE FRAME, pinned end to end.
//
// The hazard this file exists for: FLTK translates Fl::event_x/y into a child sub-window's own
// frame only for the duration of that window's handle() call (Fl_Group::send subtracts the child's
// x()/y() on the way in and restores the pair on the way out). So a tool driven from the MAIN
// WINDOW's handler, or one that reads the event pair from the frame loop, sees the position in the
// TOP-LEVEL window's frame -- offset by exactly the chrome above and to the left of the canvas (the
// menu bar + tool options bar, and the tool rail). A pen that mixed the two would drop its nodes a
// couple of centimetres from the cursor, and the drift would scale with the zoom.
//
// Every fixture below puts a REAL chrome offset in (app_window's layout: a 38 px rail, a 28 px menu
// bar + a 34 px options bar) and asserts the difference between the right answer and the wrong one,
// so a regression that reinstates a window-frame read cannot pass.
namespace {

using mosaic::common::Vec2;
using mosaic::ui::CanvasView;
using mosaic::ui::ShapeDraft;
using mosaic::ui::ToolId;
using mosaic::ui::ToolManager;
using mosaic::ui::VulkanCanvas;

namespace vec = mosaic::core::vec;

constexpr int kChromeX = 38;      // the left tool rail
constexpr int kChromeY = 28 + 34; // the menu bar + the tool options (context) bar

// The house idiom: the handle() override is protected, the base's is public.
int send(Fl_Widget& w, int event) { return w.handle(event); }

// Deliver a pointer event exactly as FLTK does to a child sub-window: translated into the child's
// frame for the call, restored to the top-level's frame afterwards (test_canvas_cursor's helper).
int sendFromWindow(VulkanCanvas& canvas, int event, int winX, int winY) {
    Fl::e_x = winX - canvas.x();
    Fl::e_y = winY - canvas.y();
    const int ret = send(canvas, event);
    Fl::e_x = winX; // FLTK's restore -- window coordinates again, from here on
    Fl::e_y = winY;
    return ret;
}

// A left-button click at CANVAS-LOCAL (x, y), delivered through the window like a real one.
void clickCanvasLocal(VulkanCanvas& canvas, double x, double y) {
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    Fl::e_state = 0;
    Fl::e_clicks = 0;
    Fl::e_is_click = 1;
    const int wx = kChromeX + static_cast<int>(x);
    const int wy = kChromeY + static_cast<int>(y);
    sendFromWindow(canvas, FL_PUSH, wx, wy);
    sendFromWindow(canvas, FL_RELEASE, wx, wy);
}

// Returns whether the canvas CLAIMED the key -- which matters as much as the effect: Backspace and
// Escape belong to the main window whenever the pen is not using them.
bool pressKeyClaimed(VulkanCanvas& canvas, int keysym) {
    Fl::e_keysym = keysym;
    Fl::e_state = 0;
    return send(canvas, FL_KEYDOWN) != 0;
}

void pressKey(VulkanCanvas& canvas, int keysym) {
    (void)pressKeyClaimed(canvas, keysym);
}

// A 400x300 document at 2x zoom, no pan, no rotation:
//     doc = (200,150) + (canvasLocal - (320,240)) / 2
void seedCanvas(VulkanCanvas& canvas) {
    const mosaic::common::Image image(400, 300);
    canvas.setDocumentImage(image, /*fitView=*/true);
    canvas.setViewState({/*zoom=*/2.0, /*rotation=*/0.0, /*pan=*/{0.0, 0.0}});
}

bool near(Vec2 a, Vec2 b, double eps = 1e-6) {
    return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps;
}

// The one subpath of a landed draft, in DOCUMENT space (placement applied), which is the only thing
// the user actually cares about: "the path is where I clicked".
std::vector<Vec2> draftAnchorsDoc(const ShapeDraft& draft) {
    std::vector<Vec2> out;
    const auto* path = std::get_if<vec::Path>(&draft.object.geometry);
    if (path == nullptr || path->subpaths.empty())
        return out;
    for (const vec::Node& n : path->subpaths[0].nodes)
        out.push_back(draft.placement.apply(n.anchor));
    return out;
}

} // namespace

TEST_CASE("pen: the authored path lands where the pointer was, in the CANVAS's frame") {
    VulkanCanvas canvas(kChromeX, kChromeY, 640, 480);
    ToolManager tools;
    tools.setActive(ToolId::Pen);
    canvas.setToolManager(&tools);
    seedCanvas(canvas);

    std::optional<ShapeDraft> landed;
    int commits = 0;
    int cancels = 0;
    VulkanCanvas::PenToolHost host;
    host.spawnPath = [&](const ShapeDraft& d) { landed = d; };
    host.commitPath = [&](const std::string&) { ++commits; };
    host.cancelPath = [&] { ++cancels; };
    canvas.setPenToolHost(host);

    const std::array<Vec2, 3> local{Vec2{300, 200}, Vec2{400, 200}, Vec2{400, 300}};
    std::array<Vec2, 3> wantDoc{};
    for (std::size_t i = 0; i < local.size(); ++i) {
        wantDoc[i] = canvas.view().toDoc(local[i]);
        clickCanvasLocal(canvas, local[i].x, local[i].y);
    }
    CHECK(commits == 0); // nothing reaches the document until the path is finished

    pressKey(canvas, FL_Enter); // Enter ends an open path
    CHECK(commits == 1);
    CHECK(cancels == 0);
    REQUIRE(landed.has_value());

    const std::vector<Vec2> got = draftAnchorsDoc(*landed);
    REQUIRE(got.size() == 3);
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(near(got[i], wantDoc[i]));

    // Fixture sanity -- the two frames really are far apart, so the checks above cannot pass by
    // accident. Reading the press in the WINDOW's frame would have put every node 19 x 31 document
    // px away (the chrome divided by the zoom), which is the exact bug being guarded against.
    const Vec2 wrong = canvas.view().toDoc({local[0].x + kChromeX, local[0].y + kChromeY});
    CHECK(std::abs(wrong.x - wantDoc[0].x) == doctest::Approx(kChromeX / 2.0));
    CHECK(std::abs(wrong.y - wantDoc[0].y) == doctest::Approx(kChromeY / 2.0));
    CHECK((wrong - wantDoc[0]).length() > 10.0);

    // A bare click authors CORNER nodes (handles collapsed onto the anchor) and an OPEN subpath.
    const auto* path = std::get_if<vec::Path>(&landed->object.geometry);
    REQUIRE(path != nullptr);
    REQUIRE(path->subpaths.size() == 1);
    CHECK_FALSE(path->subpaths[0].closed);
    for (const vec::Node& n : path->subpaths[0].nodes) {
        CHECK(n.inHandle == n.anchor);
        CHECK(n.outHandle == n.anchor);
    }
}

TEST_CASE("pen: the same clicks land in the same place at another zoom and pan") {
    // The offset bug scales with the zoom, so a fixture at a single zoom is not enough: the path
    // must land on the same DOCUMENT points whatever the view is doing.
    const std::array<CanvasView::ViewState, 3> views{
        CanvasView::ViewState{0.25, 0.0, {0.0, 0.0}},
        CanvasView::ViewState{2.0, 0.0, {-140.0, 90.0}},
        CanvasView::ViewState{8.0, 0.0, {310.0, -220.0}},
    };
    for (const CanvasView::ViewState& vs : views) {
        VulkanCanvas canvas(kChromeX, kChromeY, 640, 480);
        ToolManager tools;
        tools.setActive(ToolId::Pen);
        canvas.setToolManager(&tools);
        const mosaic::common::Image image(400, 300);
        canvas.setDocumentImage(image, /*fitView=*/true);
        canvas.setViewState(vs);

        std::optional<ShapeDraft> landed;
        VulkanCanvas::PenToolHost host;
        host.spawnPath = [&](const ShapeDraft& d) { landed = d; };
        host.commitPath = [&](const std::string&) {};
        canvas.setPenToolHost(host);

        const std::array<Vec2, 2> local{Vec2{120, 90}, Vec2{500, 380}};
        const Vec2 a = canvas.view().toDoc(local[0]);
        const Vec2 b = canvas.view().toDoc(local[1]);
        clickCanvasLocal(canvas, local[0].x, local[0].y);
        clickCanvasLocal(canvas, local[1].x, local[1].y);
        pressKey(canvas, FL_Enter);

        REQUIRE(landed.has_value());
        const std::vector<Vec2> got = draftAnchorsDoc(*landed);
        REQUIRE(got.size() == 2);
        CHECK(near(got[0], a, 1e-6));
        CHECK(near(got[1], b, 1e-6));

        // ... and the screen<->document round trip the whole thing rests on is exact both ways.
        CHECK(near(canvas.view().toScreen(a), local[0], 1e-6));
        CHECK(near(canvas.view().toDoc(canvas.view().toScreen(b)), b, 1e-6));
    }
}

TEST_CASE("pen: clicking the first node closes the path and commits it") {
    VulkanCanvas canvas(kChromeX, kChromeY, 640, 480);
    ToolManager tools;
    tools.setActive(ToolId::Pen);
    canvas.setToolManager(&tools);
    seedCanvas(canvas);

    std::optional<ShapeDraft> landed;
    int commits = 0;
    VulkanCanvas::PenToolHost host;
    host.spawnPath = [&](const ShapeDraft& d) { landed = d; };
    host.commitPath = [&](const std::string&) { ++commits; };
    canvas.setPenToolHost(host);

    clickCanvasLocal(canvas, 260, 180);
    clickCanvasLocal(canvas, 380, 180);
    clickCanvasLocal(canvas, 380, 300);
    CHECK(commits == 0);
    clickCanvasLocal(canvas, 260, 180); // back on the first node: closes, and the release commits
    CHECK(commits == 1);

    REQUIRE(landed.has_value());
    const auto* path = std::get_if<vec::Path>(&landed->object.geometry);
    REQUIRE(path != nullptr);
    REQUIRE(path->subpaths.size() == 1);
    CHECK(path->subpaths[0].closed);
    CHECK(path->subpaths[0].nodes.size() == 3); // closing adds no node
}

TEST_CASE("pen: a double-click finishes an open path; Escape finishes it too (Illustrator's rule)") {
    {
        VulkanCanvas canvas(kChromeX, kChromeY, 640, 480);
        ToolManager tools;
        tools.setActive(ToolId::Pen);
        canvas.setToolManager(&tools);
        seedCanvas(canvas);

        int commits = 0;
        std::optional<ShapeDraft> landed;
        VulkanCanvas::PenToolHost host;
        host.spawnPath = [&](const ShapeDraft& d) { landed = d; };
        host.commitPath = [&](const std::string&) { ++commits; };
        canvas.setPenToolHost(host);

        clickCanvasLocal(canvas, 300, 200);
        clickCanvasLocal(canvas, 380, 260);
        // The second click of a double-click: FLTK reports event_clicks() > 0.
        Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
        Fl::e_state = 0;
        Fl::e_clicks = 1;
        sendFromWindow(canvas, FL_PUSH, kChromeX + 380, kChromeY + 260);
        sendFromWindow(canvas, FL_RELEASE, kChromeX + 380, kChromeY + 260);
        Fl::e_clicks = 0;
        CHECK(commits == 1);
        REQUIRE(landed.has_value());
        CHECK(draftAnchorsDoc(*landed).size() == 2);
    }
    {
        VulkanCanvas canvas(kChromeX, kChromeY, 640, 480);
        ToolManager tools;
        tools.setActive(ToolId::Pen);
        canvas.setToolManager(&tools);
        seedCanvas(canvas);

        int commits = 0;
        VulkanCanvas::PenToolHost host;
        host.spawnPath = [&](const ShapeDraft&) {};
        host.commitPath = [&](const std::string&) { ++commits; };
        canvas.setPenToolHost(host);

        clickCanvasLocal(canvas, 300, 200);
        clickCanvasLocal(canvas, 380, 260);
        pressKey(canvas, FL_Escape); // ENDS the path; it does not discard it
        CHECK(commits == 1);

        // ... and with the path gone, Escape is no longer ours (the main window uses it to dismiss
        // popovers), so it must fall through.
        Fl::e_keysym = FL_Escape;
        Fl::e_state = 0;
        CHECK(send(canvas, FL_KEYDOWN) == 0);
    }
}

TEST_CASE("pen: Backspace takes the last node back and never claims the key when idle") {
    VulkanCanvas canvas(kChromeX, kChromeY, 640, 480);
    ToolManager tools;
    tools.setActive(ToolId::Pen);
    canvas.setToolManager(&tools);
    seedCanvas(canvas);

    std::optional<ShapeDraft> landed;
    int commits = 0;
    VulkanCanvas::PenToolHost host;
    host.spawnPath = [&](const ShapeDraft& d) { landed = d; };
    host.commitPath = [&](const std::string&) { ++commits; };
    canvas.setPenToolHost(host);

    // Idle: Backspace belongs to the main window (Delete Layer), so the canvas must decline it.
    Fl::e_keysym = FL_BackSpace;
    Fl::e_state = 0;
    CHECK(send(canvas, FL_KEYDOWN) == 0);

    clickCanvasLocal(canvas, 300, 200);
    clickCanvasLocal(canvas, 400, 200);
    clickCanvasLocal(canvas, 400, 300);
    CHECK(pressKeyClaimed(canvas, FL_BackSpace)); // drops the (400,300) node
    pressKey(canvas, FL_Enter);

    REQUIRE(landed.has_value());
    const std::vector<Vec2> got = draftAnchorsDoc(*landed);
    REQUIRE(got.size() == 2);
    CHECK(near(got[1], canvas.view().toDoc({400, 200})));
    CHECK(commits == 1);
}

TEST_CASE("pen: a tool switch FINISHES the open path rather than throwing it away") {
    VulkanCanvas canvas(kChromeX, kChromeY, 640, 480);
    ToolManager tools;
    tools.setActive(ToolId::Pen);
    canvas.setToolManager(&tools);
    seedCanvas(canvas);

    std::optional<ShapeDraft> landed;
    int commits = 0;
    int cancels = 0;
    VulkanCanvas::PenToolHost host;
    host.spawnPath = [&](const ShapeDraft& d) { landed = d; };
    host.commitPath = [&](const std::string&) { ++commits; };
    host.cancelPath = [&] { ++cancels; };
    canvas.setPenToolHost(host);

    clickCanvasLocal(canvas, 280, 200);
    clickCanvasLocal(canvas, 420, 320);
    // MainWindow::onToolChanged calls this on every tool switch (and nothing else does).
    canvas.commitPenPath();
    CHECK(commits == 1);
    CHECK(cancels == 0);
    REQUIRE(landed.has_value());
    CHECK(draftAnchorsDoc(*landed).size() == 2);

    // A second call is a no-op: the path is already gone.
    canvas.commitPenPath();
    CHECK(commits == 1);

    // A document swap DROPS a half-drawn path instead (cancelCanvasInteractions).
    clickCanvasLocal(canvas, 300, 210);
    canvas.cancelPenGesture();
    CHECK(commits == 1);
    CHECK(cancels == 1);
}
