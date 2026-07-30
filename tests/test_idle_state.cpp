#include "ui/idle_fade.hpp"
#include "ui/idle_invitation.hpp"
#include "ui/vulkan_canvas.hpp"
#include "ui/widgets.hpp" // firstLocalPathFromDndText / localPathsFromDndText

#include <FL/Enumerations.H>
#include <FL/Fl.H>

#include <doctest/doctest.h>
#include <cmath>
#include <string>

using namespace mosaic;
using ui::firstLocalPathFromDndText;

namespace {
// The house idiom: the handle() override is protected, the base's is public.
int send(Fl_Widget& w, int event) {
    return w.handle(event);
}
} // namespace

// ---- DND payload parsing (pure) --------------------------------------------------------------

TEST_CASE("firstLocalPathFromDndText decodes a file URI") {
    const auto p = firstLocalPathFromDndText("file:///home/dev/pic.png\r\n");
    REQUIRE(p.has_value());
    CHECK(*p == "/home/dev/pic.png");
}

TEST_CASE("firstLocalPathFromDndText percent-decodes and takes the FIRST of several URIs") {
    const auto p =
        firstLocalPathFromDndText("file:///home/dev/two%20words%25.png\r\nfile:///second.png\r\n");
    REQUIRE(p.has_value());
    CHECK(*p == "/home/dev/two words%.png");
}

TEST_CASE("firstLocalPathFromDndText skips a localhost authority") {
    const auto p = firstLocalPathFromDndText("file://localhost/tmp/a.jpg");
    REQUIRE(p.has_value());
    CHECK(*p == "/tmp/a.jpg");
}

TEST_CASE("firstLocalPathFromDndText accepts a bare absolute path") {
    const auto p = firstLocalPathFromDndText("/tmp/plain path.png");
    REQUIRE(p.has_value());
    CHECK(*p == "/tmp/plain path.png");
}

TEST_CASE("firstLocalPathFromDndText rejects non-file payloads") {
    CHECK_FALSE(firstLocalPathFromDndText("https://example.com/a.png").has_value());
    CHECK_FALSE(firstLocalPathFromDndText("just some dragged text").has_value());
    CHECK_FALSE(firstLocalPathFromDndText("").has_value());
    CHECK_FALSE(firstLocalPathFromDndText("\r\n\r\n").has_value());
}

TEST_CASE("firstLocalPathFromDndText skips a leading non-file line for a later file URI") {
    const auto p = firstLocalPathFromDndText("https://example.com/x\nfile:///ok.png");
    REQUIRE(p.has_value());
    CHECK(*p == "/ok.png");
}

TEST_CASE("firstLocalPathFromDndText survives a malformed percent escape") {
    const auto p = firstLocalPathFromDndText("file:///bad%2escape%g1.png");
    REQUIRE(p.has_value());
    CHECK(*p == "/bad.scape%g1.png"); // %2e decodes to '.'; the broken %g1 passes through verbatim
}

TEST_CASE("localPathsFromDndText yields every local path a multi-file drop carries") {
    const auto all = ui::localPathsFromDndText(
        "file:///home/dev/a%20b.png\r\nfile:///home/dev/c.jpg\r\n/tmp/bare.png\r\n");
    REQUIRE(all.size() == 3);
    CHECK(all[0] == "/home/dev/a b.png"); // percent-decoded
    CHECK(all[1] == "/home/dev/c.jpg");
    CHECK(all[2] == "/tmp/bare.png"); // a bare absolute path drop

    // Non-file lines are skipped, not counted -- a URL drag next to a file drag yields one path.
    const auto mixed =
        ui::localPathsFromDndText("https://example.com/x.png\r\nfile:///tmp/real.png\r\n");
    REQUIRE(mixed.size() == 1);
    CHECK(mixed[0] == "/tmp/real.png");

    // A "localhost"-style authority is skipped, and blank lines never produce entries.
    const auto host = ui::localPathsFromDndText("\r\nfile://localhost/tmp/x.png\r\n\r\n");
    REQUIRE(host.size() == 1);
    CHECK(host[0] == "/tmp/x.png");

    CHECK(ui::localPathsFromDndText("").empty());
    CHECK(ui::localPathsFromDndText("just some text").empty());

    // firstLocalPathFromDndText stays the first of the same list.
    CHECK(ui::firstLocalPathFromDndText("file:///a.png\nfile:///b.png").value() == "/a.png");
}

// ---- The fade choreography (pure time arithmetic) --------------------------------------------

TEST_CASE("the return-to-blank bloom waits out its beat of stillness, then eases out to 1") {
    ui::IdleFadeState fade;
    fade.setEnabled(false, 0.0); // no-op: already disabled
    fade.setEnabled(true, 10.0);
    // During the 80 ms beat the field has not moved.
    CHECK(fade.field.value(10.0) == doctest::Approx(0.0));
    CHECK(fade.field.value(10.0 + ui::IdleFadeState::kFieldInDelay * 0.99) ==
          doctest::Approx(0.0));
    // Mid-bloom it is climbing; cubic-OUT front-loads, so the halfway point exceeds 1/2.
    const double mid =
        fade.field.value(10.0 + ui::IdleFadeState::kFieldInDelay + ui::IdleFadeState::kFieldInDur / 2);
    CHECK(mid > 0.5);
    CHECK(mid < 1.0);
    // Settled.
    CHECK(fade.field.value(11.0) == doctest::Approx(1.0));
    CHECK(fade.active(11.0));
}

TEST_CASE("a document's arrival settles the field cubic-in and ends the pass entirely") {
    ui::IdleFadeState fade;
    fade.setEnabled(true, 0.0);
    fade.setEnabled(false, 5.0);
    // Cubic-IN back-loads: at half the settle the field is still above 1/2.
    const double mid = fade.field.value(5.0 + ui::IdleFadeState::kFieldOutDur / 2);
    CHECK(mid > 0.5);
    CHECK(mid < 1.0);
    // The invitation's own (faster) fade finishes first.
    CHECK(fade.invitation.value(5.0 + ui::IdleFadeState::kInvOutDur + 0.01) ==
          doctest::Approx(0.0));
    // Once both timelines land, active() goes false -- the renderer skips the pass.
    CHECK(fade.field.value(5.5) == doctest::Approx(0.0));
    CHECK_FALSE(fade.active(5.5));
}

TEST_CASE("retargeting a tween mid-flight rebases on the current value, never jumps") {
    ui::IdleFadeState fade;
    fade.setHot(true, 0.0);
    const double partway = fade.hot.value(0.08); // mid-bloom
    CHECK(partway > 0.0);
    CHECK(partway < 1.0);
    fade.setHot(false, 0.08); // the drag leaves before the bloom lands
    CHECK(fade.hot.value(0.08) == doctest::Approx(partway)); // continuous at the handoff
    CHECK(fade.hot.value(1.0) == doctest::Approx(0.0));
}

TEST_CASE("a fade-out in flight keeps the pass active until it lands") {
    ui::IdleFadeState fade;
    fade.setEnabled(true, 0.0);
    fade.setEnabled(false, 5.0);
    CHECK(fade.active(5.05)); // the settle is still running -- the crossfade frame
    CHECK_FALSE(fade.enabled);
}

// ---- The invitation bake's pure pixel helpers ------------------------------------------------

TEST_CASE("drawInvitationFrame dashes the whole perimeter -- corners included") {
    common::Image img(200, 120);
    ui::drawInvitationFrame(img, 10, 10, 180, 100, 10.0, 1.5, 5.0, 4.0, {94, 126, 255, 255});
    const auto alphaAt = [&](int x, int y) {
        return img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3];
    };
    // The interior stays clear, and so does the far outside.
    CHECK(alphaAt(100, 60) == 0);
    CHECK(alphaAt(2, 2) == 0);
    // A dash pattern exists along the top run: some pixels ink and some do not.
    int inked = 0, blank = 0;
    for (int x = 25; x < 175; ++x)
        (alphaAt(x, 10) > 0 ? inked : blank) += 1;
    CHECK(inked > 0);
    CHECK(blank > 0);
    // The corners are dashed too (feedback 2026-07-23: solid arcs next to dashed runs read as
    // a mistake): walking the TL quarter-ring at the stroke radius crosses ink AND gaps -- the
    // arc is ~15.7 px against a ~9 px dash period, so both must appear.
    int arcInked = 0, arcBlank = 0;
    const double cx = 20.0, cy = 20.0, r = 10.0; // corner centre (x+radius, y+radius)
    for (int i = 0; i <= 45; ++i) {
        const double a = (3.14159265358979323846 / 2.0) * i / 45.0;
        const int px = static_cast<int>(std::lround(cx - r * std::cos(a)));
        const int py = static_cast<int>(std::lround(cy - r * std::sin(a)));
        (alphaAt(px, py) > 0 ? arcInked : arcBlank) += 1;
    }
    CHECK(arcInked > 0);
    CHECK(arcBlank > 0);
}

TEST_CASE("compositeOver + fadeAlpha behave as straight-alpha source-over") {
    common::Image dst(4, 4);
    dst.fill({100, 100, 100, 255});
    common::Image src(2, 2);
    src.fill({200, 200, 200, 255});
    ui::fadeAlpha(src, 0.5); // the watermark treatment
    CHECK(static_cast<int>(src.rgba[3]) == 128);
    ui::compositeOver(dst, src, 1, 1);
    // (1,1) is a 50/50 blend; (0,0) untouched.
    const std::uint8_t* p = &dst.rgba[(1 * 4 + 1) * 4];
    CHECK(static_cast<int>(p[0]) == doctest::Approx(150).epsilon(0.02));
    CHECK(static_cast<int>(dst.rgba[0]) == 100);
    // Clipping: compositing beyond the edge must not crash or wrap.
    ui::compositeOver(dst, src, 3, 3);
    ui::compositeOver(dst, src, -1, -1);
}

TEST_CASE("tintCoverageBand colours only its band") {
    common::Image img(4, 4);
    std::vector<float> cov(16, 1.0f);
    ui::tintCoverageBand(img, cov, 4, 1, 2, {255, 0, 0, 255});
    const auto alphaAt = [&](int x, int y) {
        return img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3];
    };
    CHECK(alphaAt(0, 0) == 0);
    CHECK(alphaAt(2, 1) == 255);
    CHECK(alphaAt(2, 2) == 0);
}

// ---- Idle input wiring (canvas built but never shown -- headless-safe) ------------------------
//
// The canvas is its own Fl_Window: DND arrives at it directly and the payload comes back as a
// DIRECT FL_PASTE (no parent bubbling) -- the contract the old EmptyStateView pinned, now owned
// by the canvas's idle intercept. Events are driven straight through handle() with the public
// event statics, the established headless pattern.

TEST_CASE("idle: a left release inside the canvas fires onIdleOpen; outside or right does not") {
    ui::VulkanCanvas canvas(0, 0, 400, 300);
    canvas.setIdleEnabled(true);
    int opened = 0;
    canvas.setOnIdleOpen([&opened] { ++opened; });

    Fl::e_x = 200;
    Fl::e_y = 150;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    CHECK(send(canvas, FL_PUSH) == 1); // claims the press so the release is delivered here
    CHECK(send(canvas, FL_RELEASE) == 1);
    CHECK(opened == 1);

    Fl::e_x = 500; // beyond w(): a release that slid off must not fire
    send(canvas, FL_RELEASE);
    CHECK(opened == 1);

    Fl::e_x = 200;
    Fl::e_keysym = FL_Button + FL_RIGHT_MOUSE;
    send(canvas, FL_RELEASE);
    CHECK(opened == 1);
}

TEST_CASE("idle: the canvas accepts a drop end-to-end -- DND events, then the direct FL_PASTE") {
    ui::VulkanCanvas canvas(0, 0, 400, 300);
    canvas.setIdleEnabled(true);
    std::string opened;
    canvas.setOnIdleOpenPath([&opened](const std::string& p) { opened = p; });

    CHECK(send(canvas, FL_DND_ENTER) == 1);
    CHECK(send(canvas, FL_DND_DRAG) == 1);
    CHECK(send(canvas, FL_DND_RELEASE) == 1);

    static char payload[] = "file:///tmp/dropped%20image.png\r\n";
    static char restore[] = "";
    Fl::e_text = payload;
    Fl::e_length = static_cast<int>(sizeof(payload) - 1);
    CHECK(send(canvas, FL_PASTE) == 1);
    CHECK(opened == "/tmp/dropped image.png");

    // A SECOND paste without a new drop must be ignored (the expect flag is one-shot).
    opened.clear();
    CHECK(send(canvas, FL_PASTE) == 0);
    CHECK(opened.empty());
    Fl::e_text = restore; // never leave a dangling payload pointer for later tests
    Fl::e_length = 0;
}

TEST_CASE("idle: a drag that leaves without dropping arms nothing") {
    ui::VulkanCanvas canvas(0, 0, 400, 300);
    canvas.setIdleEnabled(true);
    bool fired = false;
    canvas.setOnIdleOpenPath([&fired](const std::string&) { fired = true; });
    CHECK(send(canvas, FL_DND_ENTER) == 1);
    CHECK(send(canvas, FL_DND_LEAVE) == 1);
    static char payload[] = "file:///tmp/x.png";
    static char restore[] = "";
    Fl::e_text = payload;
    Fl::e_length = static_cast<int>(sizeof(payload) - 1);
    CHECK(send(canvas, FL_PASTE) == 0); // no release happened: the payload is not ours
    CHECK_FALSE(fired);
    Fl::e_text = restore;
    Fl::e_length = 0;
}

TEST_CASE("with idle DISABLED the documentless canvas refuses a drag outright") {
    ui::VulkanCanvas canvas(0, 0, 400, 300);
    // No idle, no file-drop host: the drag source must see "no drop target here" (S50 rule).
    CHECK(send(canvas, FL_DND_ENTER) == 0);
}

TEST_CASE("setIdleDropHot while a document is open (idle off) never blooms") {
    ui::VulkanCanvas canvas(0, 0, 400, 300);
    canvas.setIdleEnabled(true);
    canvas.setIdleEnabled(false);
    canvas.setIdleDropHot(true); // the window-level chrome mirror firing late
    // Headless proxy for "no bloom": the idle state stays inactive.
    CHECK_FALSE(canvas.idleActive());
}
