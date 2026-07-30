#include "ui/ask_or_tell_dialog.hpp"

#include "common/image_svg.hpp"
#include "platform/system_sound.hpp"
#include "ui/widgets.hpp"

#include <FL/Fl.H>

#include <assets/icon_corrupt_file_svg.hpp>
#include <assets/icon_info_svg.hpp>
#include <assets/icon_question_svg.hpp>
#include <assets/icon_restore_svg.hpp>
#include <assets/icon_warning_svg.hpp>
#include <chrono>
#include <cstdlib>
#include <doctest/doctest.h>
#include <string>

using namespace mosaic;
using ui::AskOrTellDialog;
using Stage = AskOrTellDialog::Stage;

// ---- Pure stage semantics ------------------------------------------------------------------

TEST_CASE("buttonCount clamps to the three-button maximum") {
    Stage s;
    CHECK(AskOrTellDialog::buttonCount(s) == 0);
    s.buttons = {"A"};
    CHECK(AskOrTellDialog::buttonCount(s) == 1);
    s.buttons = {"A", "B", "C"};
    CHECK(AskOrTellDialog::buttonCount(s) == 3);
    s.buttons = {"A", "B", "C", "D", "E"}; // extras beyond kMaxButtons are ignored
    CHECK(AskOrTellDialog::buttonCount(s) == AskOrTellDialog::kMaxButtons);
}

TEST_CASE("the default (Enter) button is the rightmost; none when the stage has no buttons") {
    Stage s;
    CHECK(AskOrTellDialog::defaultButtonIndex(s) == -1);
    s.buttons = {"OK"};
    CHECK(AskOrTellDialog::defaultButtonIndex(s) == 0);
    s.buttons = {"Cancel", "Skip", "Start"};
    CHECK(AskOrTellDialog::defaultButtonIndex(s) == 2);
}

TEST_CASE("cancel resolution: auto = leftmost, explicit index wins, kCancelNone disables") {
    Stage s; // no buttons: a program-driven stage never cancels
    CHECK(AskOrTellDialog::cancelIndexFor(s) == -1);

    s.buttons = {"OK"}; // a single button doubles as the dismiss
    CHECK(AskOrTellDialog::cancelIndexFor(s) == 0);

    s.buttons = {"Cancel", "Apply"};
    CHECK(AskOrTellDialog::cancelIndexFor(s) == 0);

    // The "Don't save / Cancel / Save" shape: the safe choice is NOT leftmost -- explicit index.
    s.buttons = {"Don't save", "Cancel", "Save"};
    s.cancelButton = 1;
    CHECK(AskOrTellDialog::cancelIndexFor(s) == 1);

    s.cancelButton = AskOrTellDialog::kCancelNone; // forced choice: Escape/WM-close inert
    CHECK(AskOrTellDialog::cancelIndexFor(s) == -1);

    s.cancelButton = 7; // out of range falls back to the auto rule
    CHECK(AskOrTellDialog::cancelIndexFor(s) == 0);
}

// ---- ProgressBar ----------------------------------------------------------------------------

TEST_CASE("indeterminateSpan slides the segment in from the left and out past the right") {
    int x0 = -1;
    int x1 = -1;
    constexpr int kTrack = 300;

    ui::ProgressBar::indeterminateSpan(0.0, kTrack, x0, x1);
    CHECK(x0 == 0);
    CHECK(x1 == 0); // fully off-left: nothing lit yet

    ui::ProgressBar::indeterminateSpan(0.05, kTrack, x0, x1);
    CHECK(x0 == 0);     // still entering: anchored to the left edge
    CHECK(x1 > 0);      // ...but a leading sliver is lit
    CHECK(x1 < kTrack);

    ui::ProgressBar::indeterminateSpan(0.5, kTrack, x0, x1);
    CHECK(x0 > 0); // mid-flight: a detached segment
    CHECK(x1 > x0);
    CHECK(x1 <= kTrack);

    ui::ProgressBar::indeterminateSpan(0.98, kTrack, x0, x1);
    CHECK(x1 == kTrack); // leaving: pinned to the right edge
    CHECK(x0 > 0);

    // The leading edge is monotonic over the cycle, so the sweep never jumps backwards.
    int prevLead = 0;
    for (double phase = 0.0; phase <= 1.0; phase += 0.05) {
        ui::ProgressBar::indeterminateSpan(phase, kTrack, x0, x1);
        CHECK(x1 >= prevLead - kTrack); // x1 is clipped; the raw lead only grows
        CHECK(x0 >= 0);
        CHECK(x1 >= x0);
        CHECK(x1 <= kTrack);
        prevLead = x1;
    }
}

TEST_CASE("indeterminateSpan survives a degenerate track") {
    int x0 = -1;
    int x1 = -1;
    ui::ProgressBar::indeterminateSpan(0.5, 0, x0, x1);
    CHECK(x0 == 0);
    CHECK(x1 == 0);
}

TEST_CASE("ProgressBar clamps fractions and switches modes") {
    ui::ProgressBar bar(0, 0, 200, 8); // never shown: headless-safe
    CHECK(bar.fraction() == doctest::Approx(0.0));
    CHECK_FALSE(bar.indeterminate());

    bar.setFraction(1.7);
    CHECK(bar.fraction() == doctest::Approx(1.0));
    bar.setFraction(-0.3);
    CHECK(bar.fraction() == doctest::Approx(0.0));

    bar.setIndeterminate();
    CHECK(bar.indeterminate());
    bar.setFraction(0.4); // the first measured fraction leaves the sweep
    CHECK_FALSE(bar.indeterminate());
    CHECK(bar.fraction() == doctest::Approx(0.4));
}

// ---- Dialog wiring (built but never shown -- the test_layer_effects_dialog pattern) ----------

TEST_CASE("finish() records a programmatic result without a display") {
    AskOrTellDialog dlg;
    CHECK(dlg.result() == AskOrTellDialog::kNoChoice);
    dlg.finish(2);
    CHECK(dlg.result() == 2);
}

// A real show + in-place restyle, guarded like test_settings: skipped under ASan (FLTK/X11
// teardown leaks trip LeakSanitizer) and on a headless CI (no display to realize the window).
// A real long recovery body (multi-paragraph, wraps to many lines) must never leave text under
// the buttons: the body's bottom edge stays above the button row with padding. This is a DIALOG
// quality, so it is checked on generic long copy, not one flow's wording.
TEST_CASE("a long multi-paragraph body never lands under the buttons") {
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
    return;
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    AskOrTellDialog dlg;
    Stage s;
    s.icon = AskOrTellDialog::Icon::CorruptFile;
    s.title = "This file is damaged";
    s.message =
        "Part of \"05-damaged-mid-history.mosaic\" can't be read.\n\nMosaic can open it as it was "
        "at the last complete save, or open the recovered version: 1 newer save came back, and 11 "
        "areas that couldn't be read show older content instead. Affected: the Sky layer.\n\n"
        "Nothing is written to your file until you save.";
    s.buttons = {"Open recovered version", "Open last complete save"};
    dlg.present(s);
    for (int i = 0; i < 5; ++i)
        Fl::check();
    CHECK(dlg.shown());
    // The body must end at least a little above the buttons (never overlap), and the window must
    // be tall enough to contain the button row.
    CHECK(dlg.buttonsTop() >= dlg.bodyBottom());
    CHECK(dlg.buttonsTop() - dlg.bodyBottom() >= 8); // real padding, not a hairline
    CHECK(dlg.h() >= dlg.buttonsTop() + 20);         // the button row fits inside the window
    dlg.hide();
}

// A long single-line title used to truncate against the fixed 470px width -- the flow-6 face read
// "...already open in another Mosaic wi[ndow]". The headline now WRAPS and grows the window, just
// like the body. Dialog quality, pinned on the longest real recovery title (the one that clipped).
TEST_CASE("a long title wraps and grows the window instead of clipping") {
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
    return;
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    const auto pump = [] {
        for (int i = 0; i < 5; ++i)
            Fl::check();
    };

    AskOrTellDialog dlg;
    Stage shortT;
    shortT.icon = AskOrTellDialog::Icon::Restore;
    shortT.title = "Recover?";
    shortT.message = "One short body line.";
    shortT.buttons = {"Discard", "Restore"};
    dlg.present(shortT);
    pump();
    CHECK(dlg.shown());
    CHECK(dlg.titleFits()); // a one-line headline obviously fits its box...
    const int shortH = dlg.h();
    const int shortTitleBottom = dlg.titleBottom();

    // The flow-6 headline: the longest real recovery title, the one that used to clip.
    Stage longT = shortT;
    longT.title = "This document is already open in another Mosaic window";
    dlg.present(longT);
    pump();
    CHECK(dlg.shown());
    CHECK(dlg.titleFits());                      // ...and now the long one does too (no clip)
    CHECK(dlg.titleBottom() > shortTitleBottom); // it took more than one line
    CHECK(dlg.h() > shortH);                      // and the window grew to contain them
    // Layout order still holds: title above body above buttons, with real gaps.
    CHECK(dlg.bodyBottom() > dlg.titleBottom());   // the body starts below the wrapped title
    CHECK(dlg.buttonsTop() >= dlg.bodyBottom());   // the body never lands under the buttons
    CHECK(dlg.buttonsTop() - dlg.bodyBottom() >= 8);
    CHECK(dlg.h() >= dlg.buttonsTop() + 20); // the button row fits inside the window
    dlg.hide();
}

// Every recovery-family headline (docs/askortell-dialog.md flows 1/2/4/6 + the damage faces) must
// render fully at the fixed dialog width -- no title truncates, however long. Restyled in place on
// one window, the way a staged recovery flow drives it.
TEST_CASE("every recovery-flow title renders fully") {
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
    return;
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    const char* titles[] = {
        "Unsaved changes found",                                  // flow 1 (crash restore)
        "Unsaved untitled document found",                        // flow 1 (untitled)
        "Old unsaved changes no longer match this file",          // flow 2 (orphan journal)
        "This document is already open in another Mosaic window", // flow 6 (advisory lock)
        "Two programs saved into this file",                      // flow 4 (dual-writer)
        "This file is badly damaged",                             // flow 3b/3e
        "This file is damaged",                                   // flow 3c/3d
    };
    AskOrTellDialog dlg;
    for (const char* t : titles) {
        CAPTURE(t);
        Stage s;
        s.icon = AskOrTellDialog::Icon::Restore;
        s.title = t;
        s.message = "Body copy for the recovery face.";
        s.buttons = {"Cancel", "OK"};
        dlg.present(s);
        for (int i = 0; i < 5; ++i)
            Fl::check();
        CHECK(dlg.shown());
        CHECK(dlg.titleFits());                      // the headline is not clipped
        CHECK(dlg.bodyBottom() > dlg.titleBottom()); // body sits below it, no overlap
    }
    dlg.hide();
}

TEST_CASE("present() lays out, restyles in place, and run() honours a made choice") {
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
    return;
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    AskOrTellDialog dlg;
    Stage ask;
    ask.icon = AskOrTellDialog::Icon::Question;
    ask.title = "Recover?";
    ask.message = "A question long enough to wrap across the text column at least once, so the "
                  "measured body height exceeds a single line and the window grows to fit.";
    ask.buttons = {"Cancel", "Recover"};
    dlg.present(ask);
    for (int i = 0; i < 5; ++i)
        Fl::check();
    CHECK(dlg.shown());
    const int askH = dlg.h();

    // Escape resolves to the cancel target (auto = leftmost) through the key path.
    Fl::e_keysym = FL_Escape;
    CHECK(dlg.handle(FL_KEYDOWN) == 1);
    CHECK(dlg.result() == 0);

    // Restyle in place: a progress face (no buttons, one body line) reflows the same window.
    Stage prog;
    prog.icon = AskOrTellDialog::Icon::CorruptFile;
    prog.title = "Recovering";
    prog.message = "Short.";
    prog.progress = true;
    dlg.present(prog);
    for (int i = 0; i < 5; ++i)
        Fl::check();
    CHECK(dlg.shown());
    CHECK(dlg.h() != askH);                            // fewer lines + no button row
    CHECK(dlg.result() == AskOrTellDialog::kNoChoice); // present() reset the earlier choice
    dlg.setProgressFraction(0.5);

    // Enter on a buttonless stage is inert; a programmatic finish() ends run() immediately.
    Fl::e_keysym = FL_Enter;
    CHECK(dlg.handle(FL_KEYDOWN) == 1);
    CHECK(dlg.result() == AskOrTellDialog::kNoChoice);
    dlg.finish(1);
    CHECK(dlg.run() == 1);

    dlg.hide();
    for (int i = 0; i < 5; ++i)
        Fl::check();
}

// Regression (user-reported 2026-07-06): re-opening the dialog after a staged flow came back
// with the LAST face's height instead of the newly presented one. present() must yield the same
// height for the same stage no matter what was shown (and hidden) in between.
TEST_CASE("re-presenting a stage after hide restores that stage's height") {
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
    return;
#endif
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;

    // Fl::check() alone can outrun the compositor: on Wayland the configure that acknowledges a
    // map/resize arrives asynchronously, so give the loop real time to receive it.
    const auto pump = [] {
        for (int i = 0; i < 10; ++i)
            Fl::wait(0.03);
    };

    Fl_Double_Window parent(640, 480, "host");
    parent.show();
    pump();

    AskOrTellDialog dlg;
    Stage confirm;
    confirm.icon = AskOrTellDialog::Icon::Question;
    confirm.title = "Recover imaginary.mosaic?";
    confirm.message = "Do you want to recover this file? This body is deliberately long enough "
                      "to wrap across several lines so the confirm face is clearly taller than "
                      "the short summary face below.";
    confirm.buttons = {"Do not", "Recover"};

    Stage summary;
    summary.icon = AskOrTellDialog::Icon::Info;
    summary.title = "Recovery complete";
    summary.message = "Short.";
    summary.buttons = {"Close"};

    Stage progress;
    progress.icon = AskOrTellDialog::Icon::CorruptFile;
    progress.title = "Recovering";
    progress.message = "One line while the bar runs.";
    progress.buttons = {"Abort"};
    progress.progress = true;

    // Open 1: the demo's shape -- confirm, then a progress face, then close from the (shorter)
    // summary face.
    dlg.present(confirm, &parent);
    pump();
    const int confirmH = dlg.h();
    dlg.present(progress, &parent);
    pump();
    dlg.setProgressFraction(0.5);
    pump();
    dlg.present(summary, &parent);
    pump();
    const int summaryH = dlg.h();
    CHECK(summaryH < confirmH); // precondition: the flow really ends on a shorter face
    dlg.hide();
    pump();

    // Open 2: the same confirm stage must come back at the same height, not the summary's.
    dlg.present(confirm, &parent);
    const int preMapH = dlg.h();
    CHECK(preMapH == confirmH);
    pump(); // mapping/configure must not clobber the height either
    CHECK(dlg.h() == confirmH);

    dlg.hide();
    pump();
    parent.hide();
    pump();
}

#ifdef MOSAIC_DEBUG
// Opt-in driver for the REAL Help-menu demos (debug builds only): walks the file-recovery flow
// end to end, then reopens the SAME window as the crash-restore offer -- two flows, one
// persistent dialog -- with real timers, logging face/height/visibility at each step, and
// optionally screenshotting the reopened dialog (what the COMPOSITOR shows, not client state).
// Run with MOSAIC_DEMO_DIAG=1 [MOSAIC_DEMO_SHOT=/path/shot.png]; skipped otherwise -- it maps
// windows for several seconds, too intrusive for the default suite.
TEST_CASE("DIAG: drive both Help-menu demos through one window and log heights") {
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
    return;
#endif
    if (std::getenv("MOSAIC_DEMO_DIAG") == nullptr)
        return;
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return;
    using Clock = std::chrono::steady_clock;
    // Fl::wait(t) returns early on ANY event (the sweep redraws at 30 Hz), so pump against
    // wall-clock deadlines, not iteration counts.
    const auto pumpFor = [](double s) {
        const auto until = Clock::now() + std::chrono::duration<double>(s);
        while (Clock::now() < until)
            Fl::wait(0.05);
    };
    Fl_Double_Window host(800, 600, "diag-host");
    host.show();
    pumpFor(0.4);

    mosaic::ui::runAskOrTellFileRecoveryDemo(&host);
    auto* dlg = static_cast<mosaic::ui::AskOrTellDialog*>(Fl::first_window());
    REQUIRE(static_cast<Fl_Window*>(dlg) != &host);
    const auto waitForTitle = [&](const char* want, double capS) {
        const auto until = Clock::now() + std::chrono::duration<double>(capS);
        while (Clock::now() < until && dlg->stage().title != want)
            Fl::wait(0.05);
        return dlg->stage().title == want;
    };
    const auto report = [&](const char* tag) {
        std::printf("DIAG %s: title='%s' h=%d w=%d shown=%d visible=%d result=%d\n", tag,
                    dlg->stage().title.c_str(), dlg->h(), dlg->w(), dlg->shown(), dlg->visible(),
                    dlg->result());
    };
    pumpFor(0.5);
    report("open1 checking (holds until proceed)");
    const int checking1 = dlg->h();
    CHECK(waitForTitle("Checking imaginary.mosaic", 3.0));

    Fl::e_keysym = FL_Enter;
    dlg->handle(FL_KEYDOWN); // default button: "Proceed to next stage" -> determinate fill
    pumpFor(0.3);
    report("after proceed (determinate)");
    CHECK(waitForTitle("This file is damaged", 15.0)); // the fill runs ~3.4 s, wall-clock
    pumpFor(0.3);
    report("damage ask");

    Fl::e_keysym = FL_Enter;
    dlg->handle(FL_KEYDOWN); // default = "Open last complete save" (the conservative choice)
    pumpFor(0.5);
    report("after conservative open");

    mosaic::ui::runAskOrTellCrashRestoreDemo(&host);
    report("open2 restore offer pre-settle");
    pumpFor(1.0);
    report("open2 restore offer settled");
    std::printf("DIAG open1 checking h was %d\n", checking1);
    if (const char* shot = std::getenv("MOSAIC_DEMO_SHOT")) {
        std::string cmd = "spectacle -b -n -a -o ";
        cmd += shot;
        [[maybe_unused]] const int rc = std::system(cmd.c_str()); // active window, blocking-ish
        pumpFor(1.5);
    }
    // The restore offer is a FORCED choice (kCancelNone, docs/askortell-dialog.md flow 1):
    // Escape must be inert -- a keyboard slip can never discard recovered work.
    Fl::e_keysym = FL_Escape;
    dlg->handle(FL_KEYDOWN);
    pumpFor(0.3);
    CHECK(dlg->visible());
    CHECK(dlg->stage().title == "Unsaved changes found");
    dlg->hide(); // the test dismisses programmatically; a user must pick a button
    pumpFor(0.3);
    host.hide();
    pumpFor(0.3);
}
#endif // MOSAIC_DEBUG

// ---- Icon assets ------------------------------------------------------------------------------

namespace {
common::Image rasterIcon(const unsigned char* data, std::size_t size, std::string& err) {
    return common::rasterizeSvg(data, size, 48, 48, &err);
}

int opaqueishPixels(const common::Image& img) {
    int n = 0;
    for (std::size_t i = 3; i < img.rgba.size(); i += 4) {
        if (img.rgba[i] > 128)
            ++n;
    }
    return n;
}
} // namespace

TEST_CASE("the five dialog icons rasterize through nanosvg with real coverage") {
    struct Src {
        const char* name;
        const unsigned char* data;
        std::size_t size;
    };
    const Src icons[] = {
        {"info", assets::icon_info_svg, assets::icon_info_svg_size},
        {"question", assets::icon_question_svg, assets::icon_question_svg_size},
        {"warning", assets::icon_warning_svg, assets::icon_warning_svg_size},
        {"corrupt_file", assets::icon_corrupt_file_svg, assets::icon_corrupt_file_svg_size},
        {"restore", assets::icon_restore_svg, assets::icon_restore_svg_size},
    };
    std::vector<std::vector<unsigned char>> pixels;
    for (const Src& src : icons) {
        CAPTURE(src.name);
        std::string err;
        const common::Image img = rasterIcon(src.data, src.size, err);
        REQUIRE_MESSAGE(!img.empty(), err);
        CHECK(img.width == 48);
        CHECK(img.height == 48);
        // A badge glyph fills a solid chunk of its 48x48 cell; a broken gradient/path would
        // rasterize (nanosvg is lenient) but with next to no coverage.
        CHECK(opaqueishPixels(img) > 48 * 48 / 10);
        pixels.push_back(img.rgba);
    }
    // The five are a family, not copies: every pair must differ. Restore and CorruptFile share
    // the page silhouette on purpose (both are document-fate icons); their badges must still
    // make them clearly distinct pixels.
    for (std::size_t a = 0; a < pixels.size(); ++a) {
        for (std::size_t b = a + 1; b < pixels.size(); ++b) {
            CHECK(pixels[a] != pixels[b]);
        }
    }
}

// ---- Alert sounds (platform/system_sound.hpp) -------------------------------------------------

TEST_CASE("a test run never plays alert sounds") {
    // present() asks the host for its alert sound, and this file drives present() thirteen times.
    // The subsystem is therefore OFF until an application arms it, and main() is its only caller --
    // otherwise `ctest` would fire a dozen alerts through the developer's speakers on every run.
    // Asserted rather than assumed, so wiring enableSystemSounds() into library code fails here
    // instead of being discovered by ear.
    CHECK_FALSE(platform::systemSoundsEnabled());

    // And presenting a face does not arm it as a side effect.
    AskOrTellDialog dlg;
    dlg.present({AskOrTellDialog::Icon::Warning, "Quiet", "No sound may escape a test.", {"OK"}});
    CHECK_FALSE(platform::systemSoundsEnabled());
    dlg.hide();
}
