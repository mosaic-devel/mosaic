#include "ui/ask_or_tell_dialog.hpp"

#include "common/image_svg.hpp"
#include "platform/system_sound.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <assets/icon_corrupt_file_svg.hpp> // generated from assets/icon_corrupt_file.svg
#include <assets/icon_info_svg.hpp>         // generated from assets/icon_info.svg
#include <assets/icon_question_svg.hpp>     // generated from assets/icon_question.svg
#include <assets/icon_restore_svg.hpp>      // generated from assets/icon_restore.svg
#include <assets/icon_warning_svg.hpp>      // generated from assets/icon_warning.svg
#include <cmath>
#include <cstddef>
#include <string>

namespace mosaic::ui {
namespace {

// Which of the host's alert sounds a stage's FACE asks for. The icon is already the face's
// severity, so it -- not the button row -- is the right thing to key on. Restore rides the question
// sound: the crash-journal offer is a CHOICE about a healthy file, so an error tone would tell the
// same lie the CorruptFile icon would (see the Icon enum's own note).
platform::SystemSound soundFor(AskOrTellDialog::Icon icon) {
    switch (icon) {
    case AskOrTellDialog::Icon::Info: return platform::SystemSound::Information;
    case AskOrTellDialog::Icon::Question: return platform::SystemSound::Question;
    case AskOrTellDialog::Icon::Warning: return platform::SystemSound::Warning;
    case AskOrTellDialog::Icon::CorruptFile: return platform::SystemSound::Error;
    case AskOrTellDialog::Icon::Restore: return platform::SystemSound::Question;
    }
    return platform::SystemSound::Information;
}

constexpr int kWidth = 470;   // fixed dialog width (the error-dialog convention)
constexpr int kMargin = 16;   // outer padding
constexpr int kGap = 10;      // inter-element / inter-button gap
constexpr int kIconPx = 48;   // stage-icon raster size
constexpr int kTextGap = 14;  // icon column -> text column
constexpr int kTitleH = 22;   // single-line bold headline row
constexpr int kTitleToBody = 6;
constexpr int kBodyToButtons = 14;
constexpr int kMeasureSlack = 8; // measure the body this much narrower than the box wraps it, so a
                                 // wrap-boundary line the box would push down can only make us
                                 // RESERVE more -- never less -- than the box actually draws
constexpr int kBarH = 8;       // progress-bar height
constexpr int kButtonH = 28;   // the app-wide dialog button height
constexpr int kMinButtonW = 84;
constexpr int kButtonPad = 28; // label width -> button width breathing room

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

common::Color8 lighten(common::Color8 c, int d) {
    auto up = [d](std::uint8_t v) {
        return static_cast<std::uint8_t>(std::clamp(static_cast<int>(v) + d, 0, 255));
    };
    return {up(c.r), up(c.g), up(c.b), c.a};
}

struct IconSrc {
    const unsigned char* data;
    std::size_t size;
};

IconSrc iconSrc(AskOrTellDialog::Icon kind) {
    switch (kind) {
    case AskOrTellDialog::Icon::Question:
        return {assets::icon_question_svg, assets::icon_question_svg_size};
    case AskOrTellDialog::Icon::Warning:
        return {assets::icon_warning_svg, assets::icon_warning_svg_size};
    case AskOrTellDialog::Icon::CorruptFile:
        return {assets::icon_corrupt_file_svg, assets::icon_corrupt_file_svg_size};
    case AskOrTellDialog::Icon::Restore:
        return {assets::icon_restore_svg, assets::icon_restore_svg_size};
    case AskOrTellDialog::Icon::Info:
        break;
    }
    return {assets::icon_info_svg, assets::icon_info_svg_size};
}

} // namespace

// The pooled stage button: a FlatButton that can flip between the neutral look and the accent
// default (the local FilledButton pattern -- fill_dialog / layer_effects_dialog / tool_options --
// made toggleable, because one pooled widget plays both roles across stages).
class AskOrTellDialog::StageButton : public FlatButton {
public:
    StageButton(int X, int Y, int W, int H) : FlatButton(X, Y, W, H) {}

    void setAccent(bool on) {
        m_accent = on;
        applyRest();
    }

protected:
    int handle(int event) override {
        if (!m_accent)
            return FlatButton::handle(event);
        switch (event) {
        case FL_ENTER:
            color(toFl(lighten(activePalette().accent, 16)));
            redraw();
            return 1;
        case FL_LEAVE:
            color(toFl(activePalette().accent));
            redraw();
            return 1;
        default:
            return Fl_Button::handle(event); // NOT FlatButton::handle (that resets to controlBg)
        }
    }

    // Chain to the base first (it owns the shared pressed fill), then re-resolve both of our looks
    // from the new palette -- the FlatButton::reapplyTheme contract.
    void reapplyTheme() override {
        FlatButton::reapplyTheme();
        applyRest();
    }

private:
    void applyRest() {
        if (m_accent) {
            color(toFl(activePalette().accent));
            labelcolor(toFl(activePalette().onAccent));
        } else {
            // Semantic slots, so the neutral look keeps following re-themes for free.
            color(FL_BACKGROUND2_COLOR);
            labelcolor(FL_FOREGROUND_COLOR);
        }
        redraw();
    }

    bool m_accent = false;
};

AskOrTellDialog::AskOrTellDialog() : Fl_Double_Window(kWidth, 160, "Mosaic") {
    color(toFl(activePalette().panelBg));
    begin();
    // Children live at placeholder geometry until the first present() lays them out (layout
    // measures text, so it must not run headless / before a display exists).
    m_title = new Fl_Box(0, 0, 10, kTitleH);
    m_title->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    m_title->labelfont(FL_HELVETICA_BOLD);
    m_title->labelsize(15);
    m_title->labelcolor(FL_FOREGROUND_COLOR);
    m_title->box(FL_NO_BOX);

    m_message = new Fl_Box(0, 0, 10, 10);
    m_message->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    m_message->labelcolor(FL_FOREGROUND_COLOR);
    m_message->box(FL_NO_BOX);

    m_progress = new ProgressBar(0, 0, 10, kBarH);
    m_progress->hide();

    for (auto*& b : m_buttons) {
        b = new StageButton(0, 0, 10, kButtonH);
        b->callback(cbButton, this);
        b->hide();
    }
    end();

    callback(cbClose, this); // window-manager close -> the stage's cancel target
    set_modal();             // creative-app convention: block the app, no taskbar entry
    m_themeSub = ThemeSubscription([this] {
        color(toFl(activePalette().panelBg)); // the ground is cached; children self-heal
        redraw();
    });
}

int AskOrTellDialog::buttonCount(const Stage& s) {
    return static_cast<int>(std::min<std::size_t>(s.buttons.size(), kMaxButtons));
}

int AskOrTellDialog::defaultButtonIndex(const Stage& s) {
    const int n = buttonCount(s);
    return n > 0 ? n - 1 : -1;
}

int AskOrTellDialog::cancelIndexFor(const Stage& s) {
    const int n = buttonCount(s);
    if (s.cancelButton == kCancelNone || n == 0)
        return -1;
    if (s.cancelButton >= 0 && s.cancelButton < n)
        return s.cancelButton;
    return 0; // kCancelAuto (an out-of-range explicit index falls back here too): the leftmost
}

int AskOrTellDialog::bodyBottom() const { return m_message->y() + m_message->h(); }

int AskOrTellDialog::titleBottom() const { return m_title->y() + m_title->h(); }

bool AskOrTellDialog::titleFits() const {
    // Re-measure the current headline at the box's ACTUAL width (what it wraps at when drawn):
    // if that height fits the reserved box, no line is clipped. relayout() reserves with
    // kMeasureSlack, so the honest draw width can only need the same or fewer lines.
    fl_font(m_title->labelfont(), m_title->labelsize());
    int w = m_title->w();
    int h = 0;
    fl_measure(m_stage.title.c_str(), w, h, 0);
    return h <= m_title->h();
}

int AskOrTellDialog::buttonsTop() const {
    for (auto* b : m_buttons)
        if (b != nullptr && b->visible())
            return b->y();
    return h() - kMargin; // buttonless: the window's own bottom margin
}

void AskOrTellDialog::present(const Stage& stage, Fl_Window* host) {
    m_stage = stage;
    m_result = kNoChoice;
    m_choiceMade = false;
    updateIcon();
    relayout();
    if (m_stage.progress)
        m_progress->setIndeterminate(); // duration unknown until the first measured fraction
    if (!visible()) {
        // Re-open path. Branch on visible(), not shown(): a backend may keep the native handle
        // alive across hide(), and such a half-alive window must be normalized (hidden for
        // real) before mapping again, or the fresh face can come up in the STALE surface --
        // the reopened-dialog-too-short class of bug (user report, 2026-07-06).
        if (shown())
            hide();
        centerWindowOver(*this, host); // over the host; multi-monitor-correct without one
        show();
        // The host's own alert sound, on the hidden -> shown transition ONLY. present() is also the
        // in-place restyle path, and a staged flow (confirm -> progress -> summary) drives it two or
        // three times on the SAME window; the sound announces "the app is interrupting you", which
        // happens once, so re-sounding each face would turn one interruption into a burst. A caller
        // that genuinely wants to punctuate a later stage calls platform::playSystemSound itself.
        platform::playSystemSound(soundFor(m_stage.icon));
    }
    redraw();
}

int AskOrTellDialog::run() {
    while (shown() && !m_choiceMade) {
        Fl::wait();
    }
    return m_result;
}

int AskOrTellDialog::ask(const Stage& stage, Fl_Window* host) {
    present(stage, host);
    const int r = run();
    hide();
    return r;
}

void AskOrTellDialog::setProgressFraction(double f) {
    m_progress->setFraction(f);
}

void AskOrTellDialog::setProgressIndeterminate() {
    m_progress->setIndeterminate();
}

void AskOrTellDialog::finish(int result) {
    m_result = result;
    m_choiceMade = true;
}

void AskOrTellDialog::choose(int index) {
    m_result = index;
    m_choiceMade = true;
    const auto cb = m_onButton; // copy: the callback may setOnButton() over itself
    if (cb)
        cb(index);
}

void AskOrTellDialog::chooseCancel() {
    const int idx = cancelIndexFor(m_stage);
    if (idx >= 0)
        choose(idx);
    // else: a program-driven or forced-choice stage -- Escape/WM-close deliberately inert.
}

void AskOrTellDialog::cbButton(Fl_Widget* w, void* self) {
    auto* dlg = static_cast<AskOrTellDialog*>(self);
    for (int i = 0; i < kMaxButtons; ++i) {
        if (dlg->m_buttons[static_cast<std::size_t>(i)] == w) {
            dlg->choose(i);
            return;
        }
    }
}

void AskOrTellDialog::cbClose(Fl_Widget*, void* self) {
    static_cast<AskOrTellDialog*>(self)->chooseCancel();
}

int AskOrTellDialog::handle(int event) {
    if (event == FL_KEYDOWN) {
        if (Fl::event_key() == FL_Escape) {
            chooseCancel();
            return 1; // consumed even when inert, so FLTK's default hide never fires
        }
        if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) {
            const int idx = defaultButtonIndex(m_stage);
            if (idx >= 0)
                choose(idx);
            return 1;
        }
    }
    return Fl_Double_Window::handle(event);
}

void AskOrTellDialog::relayout() {
    const int n = buttonCount(m_stage);
    const int textX = kMargin + kIconPx + kTextGap;
    const int textW = kWidth - textX - kMargin;

    int yPos = kMargin;
    // Measure the title exactly like the body below (its OWN bold font, a hair under the box
    // width via kMeasureSlack), so a long headline WRAPS across lines and grows the window
    // instead of truncating against the fixed width. Reserving a touch tall is the safe
    // direction (the box wraps at its full width -> needs the same or fewer lines). Title height
    // scaling is a dialog quality, not a per-flow copy concern -- pinned in the tests.
    fl_font(m_title->labelfont(), m_title->labelsize());
    int tw = textW - kMeasureSlack;
    int th = 0;
    fl_measure(m_stage.title.c_str(), tw, th, 0);
    th = std::max(th, kTitleH); // never shorter than the single-line headline row
    m_title->copy_label(m_stage.title.c_str());
    m_title->resize(textX, yPos, textW, th);
    yPos += th + kTitleToBody;

    // Measure the wrapped body with the box's OWN font (so a themed/scaled label can't be
    // mismeasured), at a hair under the box width. The box wraps at its full width, so it can only
    // ever need the SAME or FEWER lines than we reserve here -- the text can never spill below its
    // box onto the buttons. Reserving a touch tall is the safe direction; the extra is breathing
    // room, which the recovery flows want anyway. (Height scaling is a dialog quality, not a
    // per-flow concern -- tests/test_ask_or_tell_dialog.cpp pins it on real long copy.)
    fl_font(m_message->labelfont(), m_message->labelsize());
    int mw = textW - kMeasureSlack;
    int mh = 0;
    fl_measure(m_stage.message.c_str(), mw, mh, 0);
    mh = std::max(mh, fl_height());
    m_message->copy_label(m_stage.message.c_str());
    m_message->resize(textX, yPos, textW, mh);
    yPos += mh;

    if (m_stage.progress) {
        yPos += kGap;
        m_progress->resize(textX, yPos, textW, kBarH);
        m_progress->show();
        yPos += kBarH;
    } else {
        m_progress->hide();
    }

    yPos = std::max(yPos, kMargin + kIconPx); // the text column never undercuts the icon
    yPos += kBodyToButtons;

    if (n > 0) {
        // Right-aligned row, laid left -> right; each button sized to its label.
        std::array<int, kMaxButtons> bw{};
        int total = 0;
        for (int i = 0; i < n; ++i) {
            const auto& label = m_stage.buttons[static_cast<std::size_t>(i)];
            bw[static_cast<std::size_t>(i)] =
                std::max(kMinButtonW,
                         static_cast<int>(std::lround(fl_width(label.c_str()))) + kButtonPad);
            total += bw[static_cast<std::size_t>(i)] + (i > 0 ? kGap : 0);
        }
        int bx = kWidth - kMargin - total;
        for (int i = 0; i < n; ++i) {
            auto* b = m_buttons[static_cast<std::size_t>(i)];
            b->resize(bx, yPos, bw[static_cast<std::size_t>(i)], kButtonH);
            b->copy_label(m_stage.buttons[static_cast<std::size_t>(i)].c_str());
            b->setAccent(i == n - 1); // the rightmost is the accent default
            b->show();
            bx += bw[static_cast<std::size_t>(i)] + kGap;
        }
        yPos += kButtonH;
    }
    for (int i = n; i < kMaxButtons; ++i)
        m_buttons[static_cast<std::size_t>(i)]->hide();

    const int newH = yPos + kMargin;
    // Pin the WM size hints (min == max) to THIS face on every relayout, before the resize. A
    // fixed-size window otherwise carries hints from whenever it was last mapped, and a window
    // manager is free to clamp or restore a remembered size at (re)map time -- the stale-height
    // reopen class. With the hints always tracking the current face, the WM has no other legal
    // size to impose.
    size_range(kWidth, newH, kWidth, newH);
    if (shown()) {
        // A staged restyle keeps the dialog's centre anchored instead of its top-left, so the
        // window appears to breathe in place rather than grow downward.
        resize(x() + (w() - kWidth) / 2, y() + (h() - newH) / 2, kWidth, newH);
    } else {
        size(kWidth, newH);
    }
}

void AskOrTellDialog::updateIcon() {
    if (m_iconLoaded && m_iconKind == m_stage.icon)
        return;
    const IconSrc src = iconSrc(m_stage.icon);
    std::string err;
    m_iconImg.reset();
    m_iconBuf = common::rasterizeSvg(src.data, src.size, kIconPx, kIconPx, &err);
    if (!m_iconBuf.empty()) {
        m_iconImg = std::make_unique<Fl_RGB_Image>(m_iconBuf.rgba.data(),
                                                   static_cast<int>(m_iconBuf.width),
                                                   static_cast<int>(m_iconBuf.height), 4);
    }
    // An embedded icon failing to rasterize is a build defect (covered by tests); the dialog
    // stays usable with an empty icon well rather than aborting the caller's flow.
    m_iconKind = m_stage.icon;
    m_iconLoaded = true;
}

void AskOrTellDialog::draw() {
    Fl_Double_Window::draw();
    if (m_iconImg)
        m_iconImg->draw(kMargin, kMargin); // RGBA blit over the fresh ground
}

#ifdef MOSAIC_DEBUG
// ---- Help-menu exercisers (debug builds only; docs/askortell-dialog.md) ----------------------
// One demo per recovery flow, each wearing that flow's SETTLED faces verbatim so the visual
// pass rehearses the real product. Deliberately NOT _()-wrapped: dev-only testers must not
// burden the translation template.
namespace {

enum class DemoStage { Idle, RestoreOffer, Checking, Applying, DamageAsk, Aborted };
DemoStage g_demoStage = DemoStage::Idle; // stale timers check this and lapse
double g_demoFraction = 0.0;

// Reused across opens; intentionally never freed (a static-destruction-order dodge -- the
// exerciser may outlive nothing, but FLTK teardown at exit must not race a static dtor).
AskOrTellDialog* g_demoDialog = nullptr;

AskOrTellDialog* demoDialog() {
    if (g_demoDialog == nullptr)
        g_demoDialog = new AskOrTellDialog();
    return g_demoDialog;
}

// Flow 3c's ask, AFTER assessment: one choice between two concrete outcomes, the conservative
// one keeping the accent default and Escape (spec 2.8: the conservative stop is the default).
void demoShowDamageAsk(AskOrTellDialog* dlg) {
    g_demoStage = DemoStage::DamageAsk;
    AskOrTellDialog::Stage s;
    s.icon = AskOrTellDialog::Icon::CorruptFile;
    s.title = "This file is damaged";
    s.message = "Part of \"imaginary.mosaic\" can't be read.\n\n"
                "Mosaic can open it as it was at the last complete save (yesterday at 18:06), "
                "or open the recovered version: 2 of the 3 newer saves came back intact, and "
                "the areas of the lost one show older content.\n\n"
                "Nothing is written to your file until you save.";
    s.buttons = {"Open recovered version", "Open last complete save"};
    s.cancelButton = 1; // Escape = the conservative choice, not the leftmost
    dlg->present(s);
}

void demoShowAborted(AskOrTellDialog* dlg) {
    g_demoStage = DemoStage::Aborted;
    AskOrTellDialog::Stage s;
    s.icon = AskOrTellDialog::Icon::Warning;
    s.title = "Recovery aborted";
    s.message = "The fake recovery was cancelled mid-flight. In the real flow Mosaic falls "
                "back to opening the last complete save.";
    s.buttons = {"Close"};
    dlg->present(s);
}

void demoFillTick(void* p) {
    auto* dlg = static_cast<AskOrTellDialog*>(p);
    if (g_demoStage != DemoStage::Applying || !dlg->shown())
        return; // aborted / dismissed: let the chain lapse
    g_demoFraction += 0.015; // 0 -> 1 in ~3.3 s at 20 Hz
    if (g_demoFraction >= 1.0) {
        demoShowDamageAsk(dlg);
        return;
    }
    dlg->setProgressFraction(g_demoFraction);
    Fl::repeat_timeout(1.0 / 20.0, demoFillTick, p);
}

void demoStartApplying(AskOrTellDialog* dlg) {
    g_demoStage = DemoStage::Applying;
    AskOrTellDialog::Stage s;
    s.icon = AskOrTellDialog::Icon::CorruptFile;
    // Same title as the sweep face: the real probe is ONE face whose bar flips indeterminate ->
    // determinate once the damage extent is mapped; only the demo's affordances differ.
    s.title = "Checking imaginary.mosaic";
    s.message = "Damage mapped -- the bar is determinate now. The real face has no buttons; "
                "Abort here just exercises the aborted face.";
    s.buttons = {"Abort"};
    s.progress = true;
    dlg->present(s); // restyle in place; present() restarts the bar indeterminate...
    g_demoFraction = 0.0;
    dlg->setProgressFraction(0.0); // ...and the first fraction flips it to a growing fill
    Fl::add_timeout(1.0 / 20.0, demoFillTick, dlg);
}

} // namespace

// Flow 1, the crash-restore offer: ONE face. The file is healthy; the app died; the journal
// holds the unsaved work. Both answers end the flow -- in the real product the next thing the
// user sees is the restored canvas and a status-bar line, never another dialog face. Escape is
// deliberately inert (forced choice): a keyboard slip must not discard recovered work.
void runAskOrTellCrashRestoreDemo(Fl_Window* host) {
    AskOrTellDialog* dlg = demoDialog();
    dlg->setOnButton([dlg](int) {
        g_demoStage = DemoStage::Idle;
        dlg->hide();
    });
    g_demoStage = DemoStage::RestoreOffer;
    AskOrTellDialog::Stage s;
    s.icon = AskOrTellDialog::Icon::Restore;
    s.title = "Unsaved changes found";
    s.message = "Mosaic didn't close cleanly the last time this document was open. Your unsaved "
                "work -- 14 changes, the last from today at 14:32 -- was kept safe.\n\n"
                "Restore picks up exactly where you left off. Nothing is written to your file "
                "until you save.";
    s.buttons = {"Discard changes", "Restore"};
    s.cancelButton = AskOrTellDialog::kCancelNone;
    dlg->present(s, host);
}

// Flow 3c, the damaged-file flow: assess first (both progress modes, held for inspection), then
// the concrete-outcome ask. This is the file-got-hurt-outside-Mosaic scenario -- a different
// mechanism from the crash restore above, and the demos stay separate on purpose.
void runAskOrTellFileRecoveryDemo(Fl_Window* host) {
    AskOrTellDialog* dlg = demoDialog();
    dlg->setOnButton([dlg](int i) {
        switch (g_demoStage) {
        case DemoStage::Checking:
            if (i == 1)
                demoStartApplying(dlg); // "Proceed to next stage"
            else
                demoShowAborted(dlg); // "Abort"
            break;
        case DemoStage::Applying:
            demoShowAborted(dlg); // the only button on that face is Abort
            break;
        case DemoStage::DamageAsk:
        case DemoStage::Aborted:
        case DemoStage::RestoreOffer:
        case DemoStage::Idle:
            g_demoStage = DemoStage::Idle;
            dlg->hide(); // either open choice (and Close) just dismisses the demo
            break;
        }
    });
    g_demoStage = DemoStage::Checking;
    AskOrTellDialog::Stage s;
    s.icon = AskOrTellDialog::Icon::CorruptFile;
    s.title = "Checking imaginary.mosaic";
    // No timer here: nothing is actually being recovered, so this face HOLDS -- the sweep can
    // be watched for as long as needed -- and only the button advances the flow.
    s.message = "Reading every intact part of a file that never existed. The bar sweeps "
                "(indeterminate) while the damage extent is still unknown.";
    s.buttons = {"Abort", "Proceed to next stage"};
    s.progress = true;
    dlg->present(s, host);
}
#endif // MOSAIC_DEBUG

} // namespace mosaic::ui
