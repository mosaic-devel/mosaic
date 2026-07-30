#pragma once

#include "common/image.hpp"
#include "ui/theme.hpp"   // ThemeSubscription (the window ground is a cached colour)
#include "ui/widgets.hpp" // ProgressBar

#include <FL/Fl_Double_Window.H>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Fl_Box;
class Fl_RGB_Image;

// A reusable themed ask/tell modal (docs/askortell-dialog.md): a colour icon top-left, a bold
// headline + wrapping body, up to three caller-labelled buttons -- the RIGHTMOST is the accent
// default and fires on Enter -- and an optional progress bar (determinate or indeterminate).
// One instance restyles IN PLACE via present(), so a staged flow (confirm -> progress -> summary)
// keeps a single window up with no flicker. Foreseen users: the HEVC drag-and-drop caveat and
// file-corruption recovery (neither built yet; this session ships only the dialog).
namespace mosaic::ui {

class AskOrTellDialog : public Fl_Double_Window {
public:
    // Which embedded stage icon to show (assets/icon_*.svg, rasterized at 48 px via nanosvg).
    // Restore is the crash-journal offer's face: the file is HEALTHY there (the journal is the
    // evidence, not damage), so CorruptFile would tell the user a lie -- see the recovery-flows
    // section of docs/askortell-dialog.md for the icon-to-flow mapping.
    enum class Icon { Info, Question, Warning, CorruptFile, Restore };

    static constexpr int kMaxButtons = 3;
    static constexpr int kCancelAuto = -1; // Stage::cancelButton: leftmost button (none when 0)
    static constexpr int kCancelNone = -2; // Stage::cancelButton: Escape/WM-close ignored
    static constexpr int kNoChoice = -1;   // result(): no button chosen since present()

    // One face of the dialog. present() applies a Stage wholesale -- icon, texts, buttons, and
    // whether the progress bar shows -- so a multi-stage caller swaps faces with plain data.
    struct Stage {
        Icon icon = Icon::Info;
        std::string title;   // short bold headline ("Recovery complete")
        std::string message; // body; wraps to the text column
        // 0..kMaxButtons labels, laid out left -> right; the LAST one is the accent-filled
        // default (Enter). Extra entries beyond kMaxButtons are ignored. An empty list makes a
        // program-driven stage: no way to dismiss, the caller presents onward or hides.
        std::vector<std::string> buttons;
        // The index Escape and the window-manager close fire, kCancelAuto (leftmost -- override
        // it when the leftmost button is not the safe choice, e.g. a "Don't save" on the left),
        // or kCancelNone for a forced choice.
        int cancelButton = kCancelAuto;
        bool progress = false; // show the progress bar (starts indeterminate on present())
    };

    AskOrTellDialog();

    // Apply `stage` and show (idempotent while already shown -- this is the restyle path too).
    // Sizes to the content, centred over `host` on first show (screen-centred when null), and
    // keeps its centre anchored when a later stage changes the height. Resets result()/the
    // pending choice, so a run() that follows waits for THIS stage's answer.
    void present(const Stage& stage, Fl_Window* host = nullptr);

    // Block (pumping Fl::wait) until a button fires, finish() is called, or the window is no
    // longer shown; returns the chosen button index or kNoChoice. The window STAYS SHOWN after a
    // choice so a staged caller can present() the next face; one-shot callers use ask().
    [[nodiscard]] int run();

    // present() + run() + hide(): the one-shot "ask a question / tell a thing" convenience.
    int ask(const Stage& stage, Fl_Window* host = nullptr);

    // Progress control for a Stage{.progress = true}: present() starts the bar indeterminate
    // (duration unknown yet); the first measured fraction switches it to a growing fill.
    void setProgressFraction(double f);
    void setProgressIndeterminate();

    // Non-blocking use (the debug exerciser, worker-driven flows): fired with the button index
    // on every click, from inside the button callback -- present()/hide() are safe in here.
    void setOnButton(std::function<void(int)> cb) { m_onButton = std::move(cb); }

    // Programmatic conclusion (e.g. a worker finished): records `result` and ends a pending
    // run(). Does not fire the onButton callback and does not hide the window.
    void finish(int result);

    [[nodiscard]] int result() const noexcept { return m_result; }

    // The stage currently applied (what present() last received) -- lets a staged caller ask
    // "which face is up?" instead of mirroring that state on the side.
    [[nodiscard]] const Stage& stage() const noexcept { return m_stage; }

    // ---- Pure stage semantics (unit-tested headlessly) ------------------------------------
    [[nodiscard]] static int buttonCount(const Stage& s);        // clamped to [0, kMaxButtons]
    [[nodiscard]] static int defaultButtonIndex(const Stage& s); // Enter's target; -1 when none
    [[nodiscard]] static int cancelIndexFor(const Stage& s); // Escape/WM-close target; -1 = none

    // ---- Layout introspection (the height-fit test) ---------------------------------------
    // The realized bottom of the wrapped body, and the top of the button row (window bottom
    // margin when buttonless). relayout() keeps buttonsTop() a fixed padding below bodyBottom()
    // so a long body never lands under a button; the test pins that invariant on real copy.
    [[nodiscard]] int bodyBottom() const;
    [[nodiscard]] int buttonsTop() const;
    // The realized bottom of the (possibly multi-line) headline, and whether that headline fits
    // its box unclipped. Like the body, a long title WRAPS and grows the window; the test pins
    // titleFits() on the longest real recovery titles so no headline ever truncates.
    [[nodiscard]] int titleBottom() const;
    [[nodiscard]] bool titleFits() const;

    int handle(int event) override; // Enter -> default button, Escape -> cancel target

protected:
    void draw() override; // base window + the stage icon blit

private:
    class StageButton;
    static void cbButton(Fl_Widget* w, void* self);
    static void cbClose(Fl_Widget* w, void* self); // WM close -> the cancel target
    void choose(int index);
    void chooseCancel();
    void relayout(); // size + place children for m_stage (needs a display: measures text)
    void updateIcon();

    Stage m_stage;
    int m_result = kNoChoice;
    bool m_choiceMade = false; // ends a pending run(); reset by present()
    std::function<void(int)> m_onButton;

    Fl_Box* m_title = nullptr;
    Fl_Box* m_message = nullptr;
    ProgressBar* m_progress = nullptr;
    std::array<StageButton*, kMaxButtons> m_buttons{}; // fixed pool: relabel/hide, never delete

    common::Image m_iconBuf;                 // owns the pixels m_iconImg references
    std::unique_ptr<Fl_RGB_Image> m_iconImg; // the current stage's 48 px icon raster
    Icon m_iconKind = Icon::Info;
    bool m_iconLoaded = false;

    ThemeSubscription m_themeSub; // the window ground is cached -> re-apply on a re-theme
};

#ifdef MOSAIC_DEBUG
// Debug builds only (Help menu): one exerciser per recovery flow, each a faithful rehearsal of
// exactly one settled face sequence (docs/askortell-dialog.md, recovery-family flows). They are
// deliberately SEPARATE -- the crash-restore flow (the file is fine, the app died) and the
// damaged-file flow (the file got hurt from outside) are different mechanisms with different
// faces, and a demo chaining one into the other would rehearse a product that does not exist.
void runAskOrTellCrashRestoreDemo(Fl_Window* host); // flow 1: one forced-choice face
void runAskOrTellFileRecoveryDemo(Fl_Window* host); // flow 3c: assess -> concrete-outcome ask
#endif

} // namespace mosaic::ui
