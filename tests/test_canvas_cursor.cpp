#include "ui/vulkan_canvas.hpp"

#include <doctest/doctest.h>

#include <FL/Enumerations.H>
#include <FL/Fl.H>

// The canvas's tracked pointer (the GPU brush-reticle / eyedropper-loupe anchor) vs. KEYBOARD
// events. An Fl_Window needs no display to exist or to take an event, and Fl::e_x / e_y /
// e_keysym are plain public fields (the test_brush_preset_panel precedent), so the event plumbing
// runs headlessly. The invariant pinned here: pointer events delivered to the canvas write the
// tracked position in CANVAS-relative coordinates; keyboard events -- which FLTK delivers against
// the TOP-LEVEL window, so their Fl::event_x/y carry the canvas's origin inside the window baked
// in -- must never move it.
//
// The regression (S24 follow-up): updateToolCursor() used to re-read Fl::event_x/y, and it runs
// on the modifier keydown/keyup fan-out too -- so holding ANY modifier with a brush tool active
// jumped the reticle by exactly the canvas's offset inside the main window.

namespace {

using mosaic::ui::VulkanCanvas;
using mosaic::common::Vec2;

// The house idiom: the handle() override is protected, the base's is public.
int send(Fl_Widget& w, int event) { return w.handle(event); }

// Deliver a pointer event the way FLTK does to a child sub-window, and leave Fl::e_x/e_y where
// FLTK leaves them afterwards. Both halves matter:
//   in  -- Fl_Group::send() subtracts the child window's x()/y() before calling handle()
//          (Fl_Group.cxx:104-108; Fl.cxx send_event() does the same arithmetic for the
//          pushed()-routed FL_DRAG / FL_RELEASE, which never go through the group at all),
//   out -- and it RESTORES the pair the instant handle() returns. So every reader that runs
//          later (the frame loop, a zero-delay timeout, a host callback, a key handler) sees
//          the position in the TOP-LEVEL window's frame, with the canvas's origin -- the chrome
//          above and left of it -- still in the number.
int sendFromWindow(VulkanCanvas& canvas, int event, int winX, int winY) {
    Fl::e_x = winX - canvas.x();
    Fl::e_y = winY - canvas.y();
    const int ret = send(canvas, event); // protected override, public through the base
    Fl::e_x = winX; // FLTK's restore -- window coordinates again, from here on
    Fl::e_y = winY;
    return ret;
}

} // namespace

TEST_CASE("modifier key events never move the tracked pointer (the reticle-offset bug)") {
    // The canvas sits offset inside the main window, like the real layout (toolbar to the left,
    // menu bar above). That offset is exactly what the buggy path baked into the reticle.
    constexpr int kOriginX = 120;
    constexpr int kOriginY = 80;
    VulkanCanvas canvas(kOriginX, kOriginY, 640, 480);

    // The pointer enters and moves at canvas-relative (30, 40).
    Fl::e_x = 30;
    Fl::e_y = 40;
    send(canvas, FL_ENTER);
    send(canvas, FL_MOVE);
    CHECK(canvas.pointerLogical() == mosaic::common::Vec2{30.0, 40.0});

    // A modifier keydown arrives. Key events are delivered against the top-level window, so the
    // event coordinates carry the canvas origin -- the exact offset the bug showed on screen.
    Fl::e_x = 30 + kOriginX;
    Fl::e_y = 40 + kOriginY;
    Fl::e_keysym = FL_Shift_L;
    send(canvas, FL_KEYDOWN); // the canvas's own key handler (canvas holds focus)
    CHECK(canvas.pointerLogical() == mosaic::common::Vec2{30.0, 40.0});

    // The main window's modifier fan-out (AppWindow::handle -> modifiersChanged) with the same
    // key event still current: likewise must not move it.
    canvas.modifiersChanged();
    CHECK(canvas.pointerLogical() == mosaic::common::Vec2{30.0, 40.0});

    // ... and the release's keyup, via both routes.
    send(canvas, FL_KEYUP);
    canvas.modifiersChanged();
    CHECK(canvas.pointerLogical() == mosaic::common::Vec2{30.0, 40.0});

    // A real pointer move still tracks (canvas-relative, as delivered to the canvas).
    Fl::e_x = 55;
    Fl::e_y = 66;
    send(canvas, FL_MOVE);
    CHECK(canvas.pointerLogical() == mosaic::common::Vec2{55.0, 66.0});

    // A press tracks too -- FL_PUSH seeds the tracked pointer before its tool dispatch.
    Fl::e_x = 200;
    Fl::e_y = 100;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    send(canvas, FL_PUSH);
    CHECK(canvas.pointerLogical() == mosaic::common::Vec2{200.0, 100.0});
    send(canvas, FL_RELEASE);
}

// The event-FRAME rule (user report 2026-07-27: "the canvas handles/dragging ... dragging yields
// the statusbar+contextbar+toolbar coordinates rather than the canvas coordinates causing an
// offset"). FLTK translates Fl::event_x/y into a child sub-window's frame ONLY for the duration of
// its handle() call. Anything that reads the pair later -- the ~60 Hz frame loop, which is where
// every gizmo/overlay is placed, a zero-delay timeout, a host callback -- gets the position in the
// TOP-LEVEL window's frame, i.e. offset by exactly the chrome above and to the left of the canvas.
// The concrete casualty was the Gradient tool's axis gizmo: syncMoveOverlay() places it every frame
// from currentGradientDraft(), which read the raw pair, so the far handle sat a toolbar + options
// bar away from the cursor for the whole authoring drag.
//
// VulkanCanvas::eventLogicalPoint() is now the single answer, and this pins it. The fixture puts a
// REAL chrome offset in (app_window's layout: a 38 px tool rail, a 28 px menu bar + a 34 px options
// bar) so the corrected value and the buggy one are 38/62 logical px -- 19/31 document px at this
// zoom -- apart. A test that could not tell them apart would be worthless.
TEST_CASE("the canvas answers in its own frame once the event dispatch has ended") {
    constexpr int kChromeX = 38;      // the left tool rail
    constexpr int kChromeY = 28 + 34; // the menu bar + the tool options (context) bar
    VulkanCanvas canvas(kChromeX, kChromeY, 640, 480);

    // A deterministic view: 400x300 document, 2x zoom, no pan or rotation. Then
    //   doc = docCentre + (canvasLocal - viewCentre) / zoom
    //       = (200,150) + (canvasLocal - (320,240)) / 2
    const mosaic::common::Image image(400, 300);
    canvas.setDocumentImage(image, /*fitView=*/true);
    canvas.setViewState({/*zoom=*/2.0, /*rotation=*/0.0, /*pan=*/{0.0, 0.0}});

    // A press at window (338, 262) is canvas-local (300, 200) is document (190, 130).
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    Fl::e_state = FL_BUTTON1;
    Fl::e_clicks = 0;
    sendFromWindow(canvas, FL_PUSH, kChromeX + 300, kChromeY + 200);
    CHECK(canvas.pointerLogical() == Vec2{300.0, 200.0});
    CHECK(canvas.eventLogicalPoint() == Vec2{300.0, 200.0});

    // ... and that is still the answer AFTER the dispatch, with Fl::e_x/e_y back in the window's
    // frame -- this call is the frame loop, and it is the one the bug got wrong.
    const Vec2 doc = canvas.view().toDoc(canvas.eventLogicalPoint());
    CHECK(doc.x == doctest::Approx(190.0));
    CHECK(doc.y == doctest::Approx(130.0));

    // Fixture sanity: the offset really is visible. Mapping the WINDOW-frame pair (what the raw
    // Fl::event_x/y read returned from here) lands 19 x 31 document px away -- so the checks above
    // cannot pass by accident, and a regression that reinstates the raw read fails them.
    const Vec2 chromeBitten = canvas.view().toDoc({static_cast<double>(kChromeX + 300),
                                                   static_cast<double>(kChromeY + 200)});
    CHECK(chromeBitten.x == doctest::Approx(209.0));
    CHECK(chromeBitten.y == doctest::Approx(161.0));

    // A drag moves it, and the same rule holds after that dispatch ends.
    sendFromWindow(canvas, FL_DRAG, kChromeX + 360, kChromeY + 250);
    CHECK(canvas.eventLogicalPoint() == Vec2{360.0, 250.0});
    const Vec2 dragDoc = canvas.view().toDoc(canvas.eventLogicalPoint());
    CHECK(dragDoc.x == doctest::Approx(220.0));
    CHECK(dragDoc.y == doctest::Approx(155.0));

    // A KEYBOARD event must not open the frame either: FLTK routes it to the focus widget carrying
    // the last pointer position in the TOP-LEVEL window's coordinates, and the canvas must keep
    // answering with the tracked pointer (the S24 reticle bug, now structural rather than a rule).
    Fl::e_x = kChromeX + 360;
    Fl::e_y = kChromeY + 250;
    Fl::e_keysym = FL_Shift_L;
    send(canvas, FL_KEYDOWN);
    CHECK(canvas.eventLogicalPoint() == Vec2{360.0, 250.0});
    send(canvas, FL_KEYUP);
    CHECK(canvas.eventLogicalPoint() == Vec2{360.0, 250.0});

    Fl::e_state = 0; // all buttons up, so FLTK's own dispatch would clear pushed()
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    sendFromWindow(canvas, FL_RELEASE, kChromeX + 360, kChromeY + 250);
    CHECK(canvas.eventLogicalPoint() == Vec2{360.0, 250.0});
}

// ---- The Space / R gesture modifiers vs. a lying key stream (S59-b) -------------------------
//
// Both canvas gesture modifiers used to INFER "the key is held" from a press/release pairing --
// correct only on a backend that delivers exactly one KEYUP per KEYDOWN, in order. FLTK's Wayland
// backend delivers neither guarantee: it synthesises auto-repeat from a timer (a KEYDOWN every
// 50 ms, re-entering our handler mid-drag) and drops the whole held-key set on focus loss without
// sending a KEYUP at all. Two user-visible failures came out of that, and both are pinned here.
//
// The one input a test rig cannot otherwise produce is "a KEYUP for a key that is still physically
// down", which is exactly the event the fix arbitrates against the window system -- so the canvas
// exposes that oracle as a seam (setHeldKeyQuery) and the tests drive it.

TEST_CASE("a KEYUP for a still-held R cannot fire the double-tap rotation reset") {
    VulkanCanvas canvas(0, 0, 640, 480);
    bool rHeld = false;
    canvas.setHeldKeyQuery([&rHeld](int k) { return k == 'r' && rHeld; });

    Fl::e_state = 0;
    Fl::e_x = 300;
    Fl::e_y = 200;
    send(canvas, FL_ENTER);

    // R goes down and a drag rotates the view.
    rHeld = true;
    Fl::e_keysym = 'r';
    send(canvas, FL_KEYDOWN);
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    Fl::e_x = 300;
    Fl::e_y = 200;
    send(canvas, FL_PUSH);
    Fl::e_x = 380;
    Fl::e_y = 260;
    send(canvas, FL_DRAG);
    const double rotated = canvas.view().rotation();
    REQUIRE(rotated != 0.0); // fixture sanity: there IS a rotation to lose

    // Now the defect's shape: two up/down pairs for a key the user is still holding, close enough
    // together to sit inside the double-tap window. The first pair used to re-arm the tap timer
    // (clearing "rotated since press"), the second used to read as the second tap and zero the
    // rotation -- mid-gesture, with R never released.
    for (int pair = 0; pair < 2; ++pair) {
        Fl::e_keysym = 'r';
        send(canvas, FL_KEYUP);
        send(canvas, FL_KEYDOWN);
    }
    CHECK(canvas.view().rotation() == rotated);

    // A GENUINE release still works: the window system agrees the key is up, so the next press is a
    // real press edge -- the double tap is refused, not disabled.
    rHeld = false;
    Fl::e_keysym = 'r';
    send(canvas, FL_KEYUP);
    Fl::e_state = 0;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    send(canvas, FL_RELEASE);
    rHeld = true;
    Fl::e_keysym = 'r';
    send(canvas, FL_KEYDOWN); // 1st tap: nothing to reset yet (no drag since)
    rHeld = false;
    send(canvas, FL_KEYUP);
    rHeld = true;
    send(canvas, FL_KEYDOWN); // 2nd tap, inside the window, nothing rotated between
    CHECK(canvas.view().rotation() == 0.0);
}

TEST_CASE("a pan drag survives the Space key, however the key stream behaves") {
    VulkanCanvas canvas(0, 0, 640, 480);
    bool spaceHeld = false;
    canvas.setHeldKeyQuery([&spaceHeld](int k) { return k == ' ' && spaceHeld; });

    Fl::e_state = 0;
    Fl::e_x = 300;
    Fl::e_y = 200;
    send(canvas, FL_ENTER);

    spaceHeld = true;
    Fl::e_keysym = ' ';
    send(canvas, FL_KEYDOWN);
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    Fl::e_x = 300;
    Fl::e_y = 200;
    send(canvas, FL_PUSH);
    Fl::e_x = 320;
    Fl::e_y = 200;
    send(canvas, FL_DRAG);
    const double afterFirst = canvas.viewState().pan.x;
    REQUIRE(afterFirst != 0.0);

    // A KEYUP for a Space the user is still holding -- refused, and in any case a pan is a POINTER
    // gesture now, so it could not have ended the drag either way. This is the reported "pan moves
    // a little, then stops while the pan cursor is still showing".
    Fl::e_keysym = ' ';
    send(canvas, FL_KEYUP);
    Fl::e_x = 340;
    Fl::e_y = 200;
    send(canvas, FL_DRAG);
    CHECK(canvas.viewState().pan.x == doctest::Approx(afterFirst + 20.0));

    // ... and even a GENUINE release mid-drag finishes the pan you started, rather than dropping
    // the document where your hand happened to be.
    spaceHeld = false;
    Fl::e_keysym = ' ';
    send(canvas, FL_KEYUP);
    Fl::e_x = 360;
    Fl::e_y = 200;
    send(canvas, FL_DRAG);
    CHECK(canvas.viewState().pan.x == doctest::Approx(afterFirst + 40.0));

    // The release ends it, and the next plain move must not resume panning.
    Fl::e_state = 0;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    send(canvas, FL_RELEASE);
    const double settled = canvas.viewState().pan.x;
    Fl::e_x = 500;
    Fl::e_y = 200;
    send(canvas, FL_MOVE);
    CHECK(canvas.viewState().pan.x == settled);
}

TEST_CASE("a lost KEYUP cannot strand the canvas in pan mode") {
    // wl_keyboard_leave clears the compositor's held-key set and delivers FL_UNFOCUS -- never a
    // KEYUP. Before the pointer resync, a popup or a portal dialog opening while Space was held
    // left m_spaceDown set for ever: every later click panned instead of reaching the tool.
    VulkanCanvas canvas(0, 0, 640, 480);
    bool spaceHeld = true;
    canvas.setHeldKeyQuery([&spaceHeld](int k) { return k == ' ' && spaceHeld; });

    Fl::e_state = 0;
    Fl::e_x = 300;
    Fl::e_y = 200;
    send(canvas, FL_ENTER);
    Fl::e_keysym = ' ';
    send(canvas, FL_KEYDOWN);

    spaceHeld = false; // focus went elsewhere and came back; no KEYUP was ever delivered
    Fl::e_x = 310;
    Fl::e_y = 200;
    send(canvas, FL_MOVE);

    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    send(canvas, FL_PUSH);
    Fl::e_x = 380;
    Fl::e_y = 200;
    send(canvas, FL_DRAG);
    CHECK(canvas.viewState().pan.x == 0.0); // the press started no pan
}
