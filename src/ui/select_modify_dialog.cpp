#include "ui/select_modify_dialog.hpp"

#include "common/i18n.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <string>

namespace mosaic::ui {
namespace {

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// The window subclass exists only to make Enter = OK, Esc = Cancel, and to hold the outcome the
// blocking loop reads back. Mirrors FillDialog::handle's key routing, trimmed to two keys.
class SelectModifyWindow : public Fl_Double_Window {
public:
    SelectModifyWindow(int W, int H, const char* title) : Fl_Double_Window(W, H, title) {}

    bool confirmed = false;
    NumberField* field = nullptr;

    void accept() {
        confirmed = true;
        hide();
    }
    void cancel() { hide(); }

protected:
    int handle(int event) override {
        if (event == FL_KEYBOARD) {
            const int key = Fl::event_key();
            if (key == FL_Escape) {
                cancel();
                return 1;
            }
            if (key == FL_Enter || key == FL_KP_Enter) {
                accept();
                return 1;
            }
        }
        return Fl_Double_Window::handle(event);
    }
};

} // namespace

std::optional<int> showSelectModifyDialog(std::string_view title, std::string_view prompt,
                                          int initial, int min, int max, Fl_Window* host) {
    const Palette& pal = activePalette();

    constexpr int kMargin = 16;
    constexpr int kWidth = 300;
    constexpr int kTitleH = 24;
    constexpr int kRowH = 28;
    constexpr int kButtonH = 28;
    constexpr int kGap = 14;
    const int height = kMargin + kTitleH + kGap + kRowH + kGap + kButtonH + kMargin;

    const std::string titleStr(title);
    const std::string promptStr(prompt);

    SelectModifyWindow win(kWidth, height, "Mosaic");
    win.color(toFl(pal.windowBg));
    win.begin();

    int y = kMargin;
    auto* titleBox = new Fl_Box(kMargin, y, kWidth - 2 * kMargin, kTitleH, titleStr.c_str());
    titleBox->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    titleBox->labelfont(FL_HELVETICA_BOLD);
    titleBox->labelsize(15);
    titleBox->labelcolor(toFl(pal.text));
    titleBox->box(FL_NO_BOX);
    y += kTitleH + kGap;

    // The prompt caption, then the value field + a "px" suffix.
    constexpr int kFieldW = 72;
    constexpr int kSuffixW = 24;
    const int captionW = kWidth - 2 * kMargin - kFieldW - kSuffixW - 8;
    auto* caption = new Fl_Box(kMargin, y, captionW, kRowH, promptStr.c_str());
    caption->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    caption->labelfont(FL_HELVETICA);
    caption->labelsize(13);
    caption->labelcolor(toFl(pal.text));
    caption->box(FL_NO_BOX);

    auto* field = new NumberField(kMargin + captionW + 4, y + (kRowH - 24) / 2, kFieldW, 24);
    field->value(formatFieldNumber(std::clamp(initial, min, max), 1.0).c_str());
    win.field = field;

    auto* suffix = new Fl_Box(kMargin + captionW + 4 + kFieldW + 4, y, kSuffixW, kRowH, _("px"));
    suffix->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    suffix->labelfont(FL_HELVETICA);
    suffix->labelsize(13);
    suffix->labelcolor(toFl(pal.textMuted));
    suffix->box(FL_NO_BOX);
    y += kRowH + kGap;

    // OK (right) + Cancel (left of it). Plain neutral buttons -- a numeric prompt is not a
    // destructive action, and the primary accent is reserved for genuinely weighty commits.
    constexpr int kOkW = 84;
    constexpr int kCancelW = 84;
    auto* ok = new FlatButton(kWidth - kMargin - kOkW, y, kOkW, kButtonH, _("OK"));
    ok->callback([](Fl_Widget* w, void*) { static_cast<SelectModifyWindow*>(w->window())->accept(); });
    auto* cancel = new FlatButton(kWidth - kMargin - kOkW - 8 - kCancelW, y, kCancelW, kButtonH,
                                  _("Cancel"));
    cancel->callback(
        [](Fl_Widget* w, void*) { static_cast<SelectModifyWindow*>(w->window())->cancel(); });

    win.end();
    win.set_modal();
    win.show();
    centerWindowOver(win, host); // over the app window; multi-monitor-correct without it
    field->take_focus();

    while (win.shown())
        Fl::wait();

    if (!win.confirmed)
        return std::nullopt;
    double v = static_cast<double>(initial);
    (void)parseFieldNumber(field->value(), v); // leaves v at initial on unparseable input
    return std::clamp(static_cast<int>(std::lround(v)), min, max);
}

} // namespace mosaic::ui
