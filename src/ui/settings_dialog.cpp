#include "ui/settings_dialog.hpp"

#include "common/i18n.hpp"
#include "common/languages.hpp"        // the UI-language picker's display names
#include "core/brush/stroke_state.hpp" // kMaxTiltDegrees -- the tilt full scale the test area draws against
#include "core/retarget/credits.hpp"
#include "ui/ask_or_tell_dialog.hpp"
#include "ui/curve_editor.hpp"
#include "ui/keymap.hpp" // Settings → Keybindings renders + interrogates the live keymap (S51-b)
#include "ui/line_style_preview.hpp"
#include "ui/scrub_slider.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Output.H>
#include <FL/filename.H> // fl_open_uri (the pack link line)
#include <FL/fl_draw.H>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mosaic::ui {
namespace {

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

// ---- layout metrics --------------------------------------------------------------------------
constexpr int kWinW = 640;
constexpr int kWinH = 460;
constexpr int kNavW = 160;     // left category rail
constexpr int kFooterH = 52;   // bottom strip holding Done
constexpr int kNavRowH = 34;
constexpr int kNavTop = 12;
constexpr int kMargin = 22;    // content inset inside a pane
constexpr int kRowH = 26;
// Settings -> Tablet. The rail is POSITIONAL (docs/tablet.md §8): names[] and the panes are pushed
// in parallel with no enum, so the Tablet pane's index is a fact both the pane and its readout timer
// have to agree on. 20 Hz is smooth to the eye for a live pressure bar and costs nothing.
constexpr int kTabletSection = 3;
constexpr double kTabletTickS = 0.05;
constexpr int kLabelW = 96;

// Vertical rhythm for stacked settings in a pane: a caption hugs the control it explains (kCaptionGap
// below that control) and the NEXT setting starts a clearer gap below the caption (kSettingGap), so a
// caption always reads as belonging to the control ABOVE it -- never the one below. Reuse these for
// every future stacked setting so the grouping stays unambiguous.
constexpr int kCaptionGap = 8;
constexpr int kSettingGap = 26;
constexpr int kCaptionH = 40; // a ~2-line caption at 12px in the content width
constexpr int kSubLabelH = 18; // a one-line 12px caption naming the control directly below it

constexpr int kContentX = kNavW;
constexpr int kContentY = 0;
constexpr int kContentW = kWinW - kNavW;
constexpr int kContentH = kWinH - kFooterH;
constexpr int kInnerX = kContentX + kMargin;
constexpr int kInnerW = kContentW - 2 * kMargin;

// The control column of a label:control row, and its width INSIDE a scrolling pane. kInnerW runs to
// within 6 px of a ScrollView's vertical scrollbar, which is fine for a pane that does not scroll and
// cramped for one that does -- so a scrolling pane lays its controls out against kCtlW and keeps the
// gutter clear. (Settings → Tablet shipped without this: its Reset button was laid out at
// kInnerX + kLabelW + 272, ending 2 px PAST the window's right edge and under the scrollbar.)
constexpr int kCtlX = kInnerX + kLabelW;
constexpr int kScrollGutter = 12;
constexpr int kCtlW = kInnerW - kLabelW - kScrollGutter;

// A left-rail category row: accent-filled when active, a soft highlight on hover, otherwise the
// panel ground. Left-aligned label. Clicking runs the supplied action (it selects the section).
class NavItem : public Fl_Widget {
public:
    NavItem(int X, int Y, int W, int H, const char* label, std::function<void()> onClick)
        : Fl_Widget(X, Y, W, H, label), m_onClick(std::move(onClick)) {}

    void setActive(bool a) {
        if (a != m_active) {
            m_active = a;
            redraw();
        }
    }

protected:
    int handle(int event) override {
        switch (event) {
        case FL_ENTER:
            m_hover = true;
            redraw();
            return 1;
        case FL_LEAVE:
            m_hover = false;
            redraw();
            return 1;
        case FL_PUSH:
            return 1; // claim the press; act on release if it stays inside
        case FL_RELEASE:
            if (Fl::event_inside(this) && m_onClick)
                m_onClick();
            return 1;
        default:
            return Fl_Widget::handle(event);
        }
    }

    void draw() override {
        const Palette& p = activePalette();
        const Fl_Color bg =
            m_active ? toFl(p.accent) : (m_hover ? toFl(p.controlHover) : toFl(p.panelBg));
        fl_color(bg);
        fl_rectf(x(), y(), w(), h());
        fl_color(m_active ? toFl(p.onAccent) : toFl(p.text));
        fl_font(FL_HELVETICA, 13);
        fl_draw(label(), x() + 14, y(), w() - 18, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }

private:
    std::function<void()> m_onClick;
    bool m_active = false;
    bool m_hover = false;
};

// A bold section heading at the top of a content pane.
Fl_Box* sectionTitle(int x, int y, const char* text) {
    auto* box = new Fl_Box(x, y, kInnerW, 26, text);
    box->labelfont(FL_HELVETICA_BOLD);
    box->labelsize(16);
    box->labelcolor(FL_FOREGROUND_COLOR); // = palette text; follows applyTheme()'s global re-theme
    box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    return box;
}

// A muted, wrapping caption (explanatory text under a heading or beside a control).
// The height `box` will ACTUALLY draw its label at, or 0 when there is no display to measure
// against. FLTK neither grows a wrapped box to fit its label nor clips it -- it simply draws the
// overflow outside the rect, over whatever is beneath. So the only honest height is the one the font
// gives back. Font metrics need a display, so headless we return 0 and the caller keeps its estimate.
int measuredLabelH(const Fl_Box& box) {
    if (box.label() == nullptr || *box.label() == '\0')
        return 0;
#if !defined(__APPLE__) && !defined(_WIN32)
    // "No display to measure against" is an X11/Wayland question only: those backends answer
    // nothing without a server, and the env vars are how you ask before FLTK is up. Cocoa and
    // Win32 always have a window server behind a GUI process, so the same test there is not a
    // headless probe -- it is a constant `true` (DISPLAY is simply never set), which pinned every
    // caption to the caller's ESTIMATE and stopped any of them growing to fit. Guarding it per
    // backend is what makes the measurement actually happen off Linux. (S57)
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr)
        return 0;
#endif
    fl_font(box.labelfont(), box.labelsize());
    int tw = (box.align() & FL_ALIGN_WRAP) != 0 ? box.w() : 0; // 0 = measure unwrapped
    int th = 0;
    fl_measure(box.label(), tw, th, 0);
    return th;
}

// `h` is the caller's estimate and the floor; a caption GROWS to fit text that needs more (a
// translation is routinely half again as long as the English it replaces). Callers that stack rows
// must advance by the returned box's h(), not by the h they passed in.
Fl_Box* caption(int x, int y, int w, int h, const char* text) {
    auto* box = new Fl_Box(x, y, w, h, text);
    box->labelsize(12);
    box->labelcolor(FL_INACTIVE_COLOR); // = palette textMuted; follows the global re-theme
    box->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    box->size(w, std::max(h, measuredLabelH(*box)));
    return box;
}

// A plain left-aligned field label.
Fl_Box* fieldLabel(int x, int y, int w, const char* text) {
    auto* box = new Fl_Box(x, y, w, kRowH, text);
    box->labelsize(13);
    box->labelcolor(FL_FOREGROUND_COLOR); // = palette text; follows the global re-theme
    box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    return box;
}

// The default-text-language choices (Settings → Text, deferred §2): the label shown and the BCP-47
// tag stored in Settings::textLanguage. Index 0 = "Follow system" (empty tag = the OS locale). Mirrors
// the Type panel's per-paragraph list; a language without an installed dictionary simply never flags.
struct TextLangItem {
    const char* label;
    const char* tag;
};
const TextLangItem kTextLanguages[] = {
    {"Follow system", ""},   {"English (US)", "en-US"}, {"English (UK)", "en-GB"},
    {"German", "de-DE"},     {"French", "fr-FR"},       {"Spanish", "es-ES"},
    {"Italian", "it-IT"},    {"Portuguese", "pt-PT"},   {"Dutch", "nl-NL"},
    {"Polish", "pl-PL"},     {"Russian", "ru-RU"},
};
// Text-language tag <-> dropdown index (0 = follow system for an empty/unknown tag).
int textLanguageIndex(const std::string& tag) {
    for (int i = 0; i < static_cast<int>(std::size(kTextLanguages)); ++i)
        if (tag == kTextLanguages[i].tag)
            return i;
    return 0;
}

// Units settings key <-> dropdown index. Index 0 = Automatic (follow the locale).
int unitsIndex(const std::string& key) {
    if (key == "metric")
        return 1;
    if (key == "imperial")
        return 2;
    return 0; // "auto" / "" / anything unknown
}
const char* unitsKey(int index) {
    switch (index) {
    case 1: return "metric";
    case 2: return "imperial";
    default: return "auto";
    }
}

// Crop initial-framing key <-> card index (S16-q): 0 = "Whole canvas" (default), 1 = "Inset"
// (a fixed 15% margin), 2 = "Draw to begin".
int cropFramingIndex(const std::string& key) {
    if (key == "inset")
        return 1;
    if (key == "draw")
        return 2;
    return 0; // anything unknown falls back to the whole-canvas default
}
const char* cropFramingKey(int index) {
    switch (index) {
    case 1: return "inset";
    case 2: return "draw";
    default: return "whole-canvas";
    }
}

// Multi-selection-edits key <-> card index (S15-e): 0 = "Disabled" (default), 1 = "All selected
// layers", 2 = "Active layer only".
int multiSelectIndex(const std::string& key) {
    if (key == "all")
        return 1;
    if (key == "active")
        return 2;
    return 0;
}
const char* multiSelectKey(int index) {
    switch (index) {
    case 1: return "all";
    case 2: return "active";
    default: return "disabled";
    }
}

// Overlay-line-style key <-> card index (Settings -> Appearance "Selection & reticle line"):
// 0 = "Classic", 1 = "Shadowed" (rim, the default), 2 = "Adaptive".
int lineStyleIndex(const std::string& key) {
    if (key == "classic")
        return 0;
    if (key == "adaptive")
        return 2;
    return 1; // anything unknown falls back to the shipped default, Shadowed/rim
}
const char* lineStyleKey(int index) {
    switch (index) {
    case 0: return "classic";
    case 2: return "adaptive";
    default: return "rim";
    }
}

// Feathered-selection-indicator key <-> card index (Settings -> Appearance "Feathered selection
// indicator"): 0 = "Bracketing ant pair" (A, the default), 1 = "True edge + soft band" (F).
int featherIndicatorIndex(const std::string& key) {
    if (key == "band")
        return 1;
    return 0; // "bracket" and anything unknown -> A, the default
}
const char* featherIndicatorKey(int index) {
    return index == 1 ? "band" : "bracket";
}

// ---- Appearance: theme-mode diagram cards (S51-a ②) -----------------------------------------
constexpr int kCardW = 132;
constexpr int kCardPreviewH = 92;
constexpr int kCardLabelH = 22;
constexpr int kCardGap = 14;

// The Appearance line-style cards ANIMATE: their photo-noise field drifts beneath a fixed stroke,
// the only way to show what a content-keyed line does. ~30fps while their pane is visible, a slow
// idle poll otherwise; ~4 px/s is an unhurried drift at card size (10 was distracting -- user).
constexpr double kLineCardTickS = 1.0 / 30.0;
constexpr double kLineCardIdleS = 0.25;
constexpr double kLineCardDriftPxPerS = 4.0;

// A tiny Mosaic mockup (menu strip + accent pip + left/right panels + canvas) drawn in palette `p`
// into (mx,my,mw,mh). Clipping by the caller can restrict it to a half (the System card = both).
void drawThemeMockup(int mx, int my, int mw, int mh, const Palette& p) {
    fl_color(toFl(p.canvasBg));
    fl_rectf(mx, my, mw, mh);
    const int titleH = 11;
    fl_color(toFl(p.windowBg)); // menu strip
    fl_rectf(mx, my, mw, titleH);
    fl_color(toFl(p.accent)); // an accent pip on the strip
    fl_rectf(mx + 4, my + 4, 12, 3);
    const int sideW = std::max(10, mw / 4);
    fl_color(toFl(p.panelBg)); // left toolbar + right dock
    fl_rectf(mx, my + titleH, sideW, mh - titleH);
    fl_rectf(mx + mw - sideW, my + titleH, sideW, mh - titleH);
    fl_color(toFl(p.controlBg)); // a few muted "rows" in the right dock
    for (int i = 0; i < 3 && my + titleH + 6 + i * 9 + 5 < my + mh; ++i)
        fl_rectf(mx + mw - sideW + 4, my + titleH + 6 + i * 9, sideW - 8, 5);
}

// A selectable diagram card: a mockup preview (drawn by `drawPreview`) above a centered label, with
// a hover wash and an accent ring when selected. Clicking runs onClick (which applies the option +
// re-selects the card). Generalizes the Appearance theme cards to any per-option setting -- crop
// initial-framing (S16-q), multi-selection edits (S15-e), ... -- the macOS-style picker the user
// wants instead of a combobox. The preview is clipped to its inset area so a mockup can't bleed out.
class OptionCard : public Fl_Widget {
public:
    // `bg` is the (hover-aware) colour the card painted behind the preview, so AA art can blend
    // against it instead of baking in a fixed background (e.g. the opacity-slider handle).
    using PreviewFn = std::function<void(int mx, int my, int mw, int mh, common::Color8 bg)>;
    OptionCard(int X, int Y, int W, int H, const char* label, PreviewFn drawPreview,
               std::function<void()> onClick)
        : Fl_Widget(X, Y, W, H, label), m_drawPreview(std::move(drawPreview)),
          m_onClick(std::move(onClick)) {}

    void setSelected(bool s) {
        if (s != m_selected) {
            m_selected = s;
            redraw();
        }
    }

protected:
    int handle(int event) override {
        switch (event) {
        case FL_ENTER: m_hover = true; redraw(); return 1;
        case FL_LEAVE: m_hover = false; redraw(); return 1;
        case FL_PUSH: return 1; // claim the press; act on release if it stays inside
        case FL_RELEASE:
            if (Fl::event_inside(this) && m_onClick)
                m_onClick();
            return 1;
        default: return Fl_Widget::handle(event);
        }
    }

    void draw() override {
        const Palette& cur = activePalette();
        const int previewH = h() - kCardLabelH;
        const common::Color8 bg = m_hover && !m_selected ? cur.controlHover : cur.controlBg;
        fl_color(toFl(bg));
        fl_rectf(x(), y(), w(), previewH);

        constexpr int m = 8;
        if (m_drawPreview) {
            fl_push_clip(x() + m, y() + m, w() - 2 * m, previewH - 2 * m);
            m_drawPreview(x() + m, y() + m, w() - 2 * m, previewH - 2 * m, bg);
            fl_pop_clip();
        }

        if (m_selected) { // 2px accent ring
            fl_color(toFl(cur.accent));
            fl_rect(x(), y(), w(), previewH);
            fl_rect(x() + 1, y() + 1, w() - 2, previewH - 2);
        } else {
            fl_color(toFl(cur.border));
            fl_rect(x(), y(), w(), previewH);
        }
        // ERASE the label strip before drawing the text: this widget sits on the transparent pane,
        // and a double-buffered window keeps prior pixels in undamaged sub-regions -- without this
        // fill the label composites over its previous frame and visibly thickens on every repaint.
        fl_color(toFl(cur.windowBg));
        fl_rectf(x(), y() + previewH, w(), kCardLabelH);
        fl_font(FL_HELVETICA, 12);
        fl_color(toFl(m_selected ? cur.accent : cur.text));
        fl_draw(label(), x(), y() + previewH, w(), kCardLabelH, FL_ALIGN_CENTER);
    }

private:
    PreviewFn m_drawPreview;
    std::function<void()> m_onClick;
    bool m_selected = false;
    bool m_hover = false;
};

// Select the card at `index` (its position in the group), clearing the others.
void selectCardAt(const std::vector<Fl_Widget*>& cards, int index) {
    for (int i = 0; i < static_cast<int>(cards.size()); ++i)
        static_cast<OptionCard*>(cards[static_cast<std::size_t>(i)])->setSelected(i == index);
}

// ---- Appearance → Icons: pack preview strips (S52) --------------------------------------------
// Six family-representative tools shown per pack -- one per toolbar colour family, so a pack's
// palette reads at a glance. Rendered once per (pack, size) through the host's renderIcon (which
// applies the per-icon default fallback and handles vector AND raster pack art) and blitted into
// the cards.
constexpr const char* kIconPreviewKeys[] = {"move", "lasso", "brush", "bucket_fill", "text",
                                            "zoom"};
constexpr int kIconPreviewCount = 6;

struct PreviewStrip {
    std::vector<common::Image> pixels; // backing stores; each Fl_RGB_Image references its entry
    std::vector<std::unique_ptr<Fl_RGB_Image>> imgs; // null where an icon failed to render
};

PreviewStrip buildPreviewStrip(const SettingsHost& host, const std::string& packId, int px) {
    PreviewStrip strip;
    if (!host.renderIcon)
        return strip;
    for (const char* key : kIconPreviewKeys) {
        strip.pixels.push_back(host.renderIcon(packId, key, px));
        common::Image& stored = strip.pixels.back();
        strip.imgs.push_back(stored.empty()
                                 ? nullptr
                                 : std::make_unique<Fl_RGB_Image>(stored.rgba.data(),
                                                                  static_cast<int>(stored.width),
                                                                  static_cast<int>(stored.height),
                                                                  4));
    }
    return strip;
}

// Blit a strip as a 3x2 grid centered in (bx,by,bw,bh).
void drawPreviewStrip(const PreviewStrip& strip, int px, int bx, int by, int bw, int bh) {
    constexpr int kCols = 3, kRows = 2;
    const int gapX = std::max(6, (bw - kCols * px) / (kCols + 1));
    const int gapY = std::max(4, (bh - kRows * px) / (kRows + 1));
    const int gridW = kCols * px + (kCols - 1) * gapX;
    const int gridH = kRows * px + (kRows - 1) * gapY;
    const int ox = bx + (bw - gridW) / 2;
    const int oy = by + (bh - gridH) / 2;
    for (int i = 0; i < kIconPreviewCount && i < static_cast<int>(strip.imgs.size()); ++i) {
        if (strip.imgs[static_cast<std::size_t>(i)] == nullptr)
            continue;
        const int cx = ox + (i % kCols) * (px + gapX);
        const int cy = oy + (i / kCols) * (px + gapY);
        strip.imgs[static_cast<std::size_t>(i)]->draw(cx, cy);
    }
}

// The pack link line: wrapped like the other identity lines, and CLICKABLE only when the text
// actually is a web link. "Link" is a claim by arbitrary third-party manifest text -- an e-mail,
// a social handle or a paragraph all land in this field -- so only http(s):// earns the hand
// cursor and fl_open_uri; anything else draws as plain wrapped text.
class PackLinkBox : public Fl_Box {
public:
    PackLinkBox(int X, int Y, int W, int H) : Fl_Box(X, Y, W, H, "") {
        box(FL_NO_BOX);
        labelfont(FL_HELVETICA);
        labelsize(12);
        align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE);
    }

    void setText(const std::string& text) {
        copy_label(text.c_str());
        m_url = looksLikeUrl(text) ? text : std::string();
        // The tooltip doubles as the clickability signal (with the hand cursor): plain text
        // gets neither.
        tooltip(m_url.empty() ? nullptr : _("Open in browser"));
    }

    [[nodiscard]] bool clickable() const { return !m_url.empty(); }

    static bool looksLikeUrl(const std::string& s) {
        return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
    }

    int handle(int event) override {
        if (!clickable())
            return Fl_Box::handle(event);
        switch (event) {
        case FL_ENTER: // claimed, so FL_LEAVE arrives and the cursor is always restored
            fl_cursor(FL_CURSOR_HAND);
            return 1;
        case FL_LEAVE:
            fl_cursor(FL_CURSOR_DEFAULT);
            return 1;
        case FL_PUSH: // open on push, the AboutWindow precedent
            fl_open_uri(m_url.c_str());
            return 1;
        default:
            return Fl_Box::handle(event);
        }
    }

private:
    std::string m_url; // empty = not a link, render-only
};

// The selected pack's detail panel: an inset preview card on the left; name (bold), artist, link
// and description stacked in ONE vertical scroll on the right. Every field is arbitrary-length
// third-party manifest text, so every field word-wraps, and a long name scrolls with the rest
// instead of pushing the block out of the panel (user call 2026-07-10; description-only before).
class IconPackDetailPanel : public Fl_Group {
public:
    IconPackDetailPanel(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
        box(FL_NO_BOX);
        m_scroll = new ScrollView(X + kPreviewW + 16, Y, W - kPreviewW - 16, H);
        m_scroll->type(Fl_Scroll::VERTICAL);
        m_scroll->box(FL_NO_BOX);
        m_scroll->color(FL_BACKGROUND_COLOR);
        m_scroll->begin();
        m_nameBox = makeLine(FL_HELVETICA_BOLD, 15);
        m_artistBox = makeLine(FL_HELVETICA, 12);
        m_linkBox = new PackLinkBox(X, Y, 1, 1);
        m_descBox = makeLine(FL_HELVETICA, 12);
        m_scroll->end();
        end();
    }

    static constexpr int kPreviewW = 150; // the inset preview card
    static constexpr int kIconPx = 28;    // detail icons draw larger than the toolbar's 20

    void setPack(const SettingsHost::IconPackDesc& desc, const PreviewStrip* strip) {
        m_desc = desc;
        m_strip = strip;
        reflow();
        m_scroll->scrollbar.value(0);
        m_scroll->scroll_to(0, 0);
        redraw();
    }

protected:
    void draw() override {
        const Palette& p = activePalette();
        fl_color(toFl(p.windowBg)); // erase (transparent pane + double buffer; see OptionCard)
        fl_rectf(x(), y(), w(), h());

        // The preview card.
        fl_color(toFl(p.panelBg));
        fl_rectf(x(), y(), kPreviewW, h());
        fl_color(toFl(p.border));
        fl_rect(x(), y(), kPreviewW, h());
        if (m_strip != nullptr)
            drawPreviewStrip(*m_strip, kIconPx, x() + 6, y() + 6, kPreviewW - 12, h() - 12);

        // Theme the text at draw time so a runtime re-theme recolours it. Accent marks a REAL
        // (clickable) link; a non-link "link" field reads as ordinary text.
        m_nameBox->labelcolor(toFl(p.text));
        m_artistBox->labelcolor(toFl(p.text));
        m_linkBox->labelcolor(toFl(m_linkBox->clickable() ? p.accent : p.text));
        m_descBox->labelcolor(toFl(p.textMuted));
        draw_children();
    }

private:
    Fl_Box* makeLine(Fl_Font font, int size) {
        auto* b = new Fl_Box(m_scroll->x(), m_scroll->y(), 1, 1, "");
        b->box(FL_NO_BOX);
        b->labelfont(font);
        b->labelsize(size);
        b->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP | FL_ALIGN_INSIDE);
        return b;
    }

    // Word-wrapped height of `text` at the scroll's usable width, in the widget's own font.
    static int measureH(const Fl_Box* b, const std::string& text, int wrapW) {
        if (text.empty())
            return 0;
        fl_font(b->labelfont(), b->labelsize());
        int mw = wrapW;
        int mh = 0;
        fl_measure(text.c_str(), mw, mh);
        return mh;
    }

    // Stack the four fields top-down at their measured heights; empty fields collapse (hidden,
    // zero-height) without leaving holes. The description pads to the scroll's height so the
    // scroll geometry stays live even when everything fits.
    void reflow() {
        const int wrapW = m_scroll->w() - Fl::scrollbar_size() - 4;
        const int left = m_scroll->x();
        int ty = m_scroll->y();
        const auto place = [&](Fl_Box* b, const std::string& text, int gapAfter) {
            const int mh = measureH(b, text, wrapW);
            b->copy_label(text.c_str());
            b->resize(left, ty, wrapW, std::max(mh, 1));
            if (mh == 0)
                b->hide();
            else
                b->show();
            ty += mh + (mh > 0 ? gapAfter : 0);
        };
        place(m_nameBox, m_desc.name, 6);
        place(m_artistBox, m_desc.artist, 4);
        m_linkBox->setText(m_desc.link); // sets the label + the is-it-really-a-link state
        place(m_linkBox, m_desc.link, 10);
        const int descH = measureH(m_descBox, m_desc.description, wrapW) + 4;
        m_descBox->copy_label(m_desc.description.c_str());
        m_descBox->resize(left, ty, wrapW,
                          std::max(descH, m_scroll->h() - (ty - m_scroll->y())));
    }

    SettingsHost::IconPackDesc m_desc;
    const PreviewStrip* m_strip = nullptr;
    ScrollView* m_scroll = nullptr;
    Fl_Box* m_nameBox = nullptr;
    Fl_Box* m_artistBox = nullptr;
    PackLinkBox* m_linkBox = nullptr;
    Fl_Box* m_descBox = nullptr;
};

// ---- preview painters: each draws a mockup into a card's inset preview area -------------------

// Theme-mode index in creation order [System, Dark, Light] (so a card can be selected by mode).
int themeModeIndex(ThemeMode mode) {
    return mode == ThemeMode::System ? 0 : (mode == ThemeMode::Dark ? 1 : 2);
}

// Theme-mode preview: the Mosaic mockup drawn in that mode's palette. System = left half light,
// right half dark, to read as "follows the OS".
void drawThemeModePreview(int mx, int my, int mw, int mh, ThemeMode mode) {
    if (mode == ThemeMode::System) {
        const int half = mw / 2;
        fl_push_clip(mx, my, half, mh);
        drawThemeMockup(mx, my, mw, mh, lightPalette());
        fl_pop_clip();
        fl_push_clip(mx + half, my, mw - half, mh);
        drawThemeMockup(mx, my, mw, mh, darkPalette());
        fl_pop_clip();
    } else {
        drawThemeMockup(mx, my, mw, mh, mode == ThemeMode::Dark ? darkPalette() : lightPalette());
    }
}

// Overlay-line style preview: one frame of the drifting photo-noise scene rendered through the
// style's exact shader math (ui/line_style_preview) and blitted as an RGB image. `phase` is the
// shared background drift the dialog's animation tick advances; `originX` is the card's x-offset
// in its row, so sibling cards read as three windows onto ONE field (the pattern system's
// canvas-anchored behaviour) instead of three copies of the same motion.
void drawLineStylePreview(int mx, int my, int mw, int mh, int style, double phase,
                          double originX) {
    if (mw <= 1 || mh <= 1)
        return;
    std::vector<std::uint8_t> buf;
    renderLineStylePreview(buf, mw, mh, style, phase, originX);
    Fl_RGB_Image img(buf.data(), mw, mh, 3);
    img.draw(mx, my);
}

// Small filled squares at the 4 corners of (rx,ry,rw,rh) -- crop "handles". Corners only and subtle
// (accent fill): the app's full 8 white/black handles read too loud at card size (the user's call).
void drawCropHandles(int rx, int ry, int rw, int rh, Fl_Color c) {
    constexpr int s = 4;
    fl_color(c);
    fl_rectf(rx - s / 2, ry - s / 2, s, s);
    fl_rectf(rx + rw - s / 2, ry - s / 2, s, s);
    fl_rectf(rx - s / 2, ry + rh - s / 2, s, s);
    fl_rectf(rx + rw - s / 2, ry + rh - s / 2, s, s);
}

// Rule-of-thirds guides inside (rx,ry,rw,rh). Endpoints clamp to the rect's pixel extent (w-1/h-1)
// so the guides don't overhang the accent border by a pixel on the right / bottom.
void drawThirds(int rx, int ry, int rw, int rh, Fl_Color c) {
    fl_color(c);
    const int x1 = rx + rw / 3, x2 = rx + (2 * rw) / 3;
    const int y1 = ry + rh / 3, y2 = ry + (2 * rh) / 3;
    fl_line(x1, ry, x1, ry + rh - 1);
    fl_line(x2, ry, x2, ry + rh - 1);
    fl_line(rx, y1, rx + rw - 1, y1);
    fl_line(rx, y2, rx + rw - 1, y2);
}

// A tiny landscape "photo" so the crop overlay has something to frame. The colours are the scene's
// own (independent of the UI palette -- this represents image content, not chrome), so we can afford
// real, subtle tones: a daytime sky with a yellow sun on the light theme, a night sky with a crescent
// moon on the dark theme. The celestial body sits in the upper-left sky on both -- co-located, not on
// opposite sides, so it never collides with the crop box's corner handles.
void drawCanvasContent(int mx, int my, int mw, int mh, const Palette& p) {
    const bool night = p.dark;
    const common::Color8 sky =
        night ? common::Color8{60, 68, 96, 255} : common::Color8{166, 202, 232, 255};
    const common::Color8 ground =
        night ? common::Color8{46, 70, 56, 255} : common::Color8{150, 190, 120, 255};
    const int horizon = my + (mh * 3) / 5;
    fl_color(toFl(sky));
    fl_rectf(mx, my, mw, mh);
    fl_color(toFl(ground)); // a "ground" band along the bottom two-fifths
    fl_rectf(mx, horizon, mw, mh - (mh * 3) / 5);
    const int d = std::max(8, mh / 4);
    const int cxp = mx + mw / 6, cyp = my + mh / 6;
    // `under` reports both bands, not just the sky: at the smallest card size d floors at 8 px and
    // the body's patch can reach past the horizon, and an opaque patch erases what it covers.
    const auto under = [&](int, int uy) { return uy >= horizon ? ground : sky; };
    if (night) { // a crescent moon: a pale disc with a sky-coloured bite carved out
        // Disc and bite in ONE patch, in this order: the bite is a sky-coloured mark drawn ON the
        // disc, so composing them separately would let the bite's own patch cut the disc's rim.
        drawAAArcs(under, {aaPieFromBox(cxp, cyp, d, d, 0, 360, {228, 230, 238, 255}),
                           aaPieFromBox(cxp + d / 3, cyp - d / 6, d, d, 0, 360, sky)});
    } else { // a yellow sun
        drawAAArcs(under, {aaPieFromBox(cxp, cyp, d, d, 0, 360, {246, 206, 82, 255})});
    }
}

// The crop overlay (the staged-rect chrome): rule-of-thirds guides + an accent rect + the 8 handles
// -- the same set the app draws, so every framing card shows the real staged-rect look.
void drawCropFrame(int rx, int ry, int rw, int rh, const Palette& p) {
    // The rule-of-thirds guides must read with the SAME subtlety on both themes, but a single fixed
    // RGB can't manage that: the night scene is far darker than the day scene, so the light theme's
    // pale grey popped much brighter on dark (user-reported "lighter on dark than on light"), while
    // the dark palette's p.border was a near-black that vanished. So match the CONTRAST instead of the
    // colour -- light's pale grey lifts the day scene (lum ~180) by ~34; this mid cool-grey lifts the
    // night scene (lum ~65) by ~the same, so both look like an equally soft guide over their scene.
    const common::Color8 guide = p.dark ? common::Color8{96, 102, 118, 255} : lightPalette().border;
    drawThirds(rx, ry, rw, rh, toFl(guide));
    fl_color(toFl(p.accent));
    fl_rect(rx, ry, rw, rh);
    drawCropHandles(rx, ry, rw, rh, toFl(p.accent));
}

// Dim the whole canvas (a darker ground), then restore the bright canvas content inside (rx..rh) --
// the crop shield, exactly as the app dims everything outside the staged rect.
void drawDimmedCanvasWithRect(int mx, int my, int mw, int mh, int rx, int ry, int rw, int rh,
                              const Palette& p) {
    fl_color(fl_darker(toFl(p.canvasBg)));
    fl_rectf(mx, my, mw, mh);
    fl_push_clip(rx, ry, rw, rh);
    drawCanvasContent(mx, my, mw, mh, p);
    fl_pop_clip();
}

// A crosshair "cursor" centred at (cx,cy): a white core with a black outline on both themes (mirrors
// the app's cursor). Drawn as four 1px black copies offset 1px in each direction (a clean halo on
// every tip, including the left/top ones a thick-line approach left bare) under a 1px white cross.
void drawCrosshair(int cx, int cy) {
    constexpr int r = 5;
    const auto cross = [&](int ox, int oy) {
        fl_line(cx - r + ox, cy + oy, cx + r + ox, cy + oy);
        fl_line(cx + ox, cy - r + oy, cx + ox, cy + r + oy);
    };
    fl_color(FL_BLACK);
    cross(-1, 0);
    cross(1, 0);
    cross(0, -1);
    cross(0, 1);
    fl_color(FL_WHITE);
    cross(0, 0);
}

// Crop initial-framing preview (S16-q), mode = card index: 0 "Whole canvas" (rect fills the canvas,
// nothing dimmed); 1 "Inset" (a centred 15%-margin rect, the rest dimmed); 2 "Draw to begin" (a
// partial, in-progress rect with a crosshair at the dragged corner -- the same rect chrome the app
// shows once you start a drag, conveying that nothing is staged until then).
void drawCropFramingPreview(int mx, int my, int mw, int mh, int mode) {
    const Palette& p = activePalette();
    if (mode == 0) {
        drawCanvasContent(mx, my, mw, mh, p);
        drawCropFrame(mx + 2, my + 2, mw - 4, mh - 4, p);
    } else if (mode == 1) {
        const int ix = mw * 15 / 100, iy = mh * 15 / 100;
        const int rx = mx + ix, ry = my + iy, rw = mw - 2 * ix, rh = mh - 2 * iy;
        drawDimmedCanvasWithRect(mx, my, mw, mh, rx, ry, rw, rh, p);
        drawCropFrame(rx, ry, rw, rh, p);
    } else {
        const int rx = mx + mw / 6, ry = my + mh / 4, rw = mw / 2, rh = mh / 2;
        drawDimmedCanvasWithRect(mx, my, mw, mh, rx, ry, rw, rh, p);
        drawCropFrame(rx, ry, rw, rh, p);
        drawCrosshair(rx + rw, ry + rh);
    }
}

// One row of a mini layer stack: a thumbnail + a name bar, the active row tinted, plus a selection
// dot on the right (`dot`: 0 none, 1 accent = the edit lands here, 2 muted-grey = selected but not
// edited). Mirrors the real LayerRow vocabulary -- no left line, the dot is the sole indicator.
void drawMiniLayerRow(int rx, int ry, int rw, int rh, bool activeRow, int dot, const Palette& p) {
    const common::Color8 bg = activeRow ? p.controlActive : p.panelBg;
    fl_color(toFl(bg));
    fl_rectf(rx, ry, rw, rh);
    fl_color(fl_darker(toFl(p.canvasBg))); // thumbnail (darkened so it reads on the active row's tint)
    fl_rectf(rx + 3, ry + 3, rh - 6, rh - 6);
    fl_color(toFl(p.textMuted)); // a "name" bar
    fl_rectf(rx + rh, ry + rh / 2 - 1, (rw - rh) / 2, 2);
    fl_color(toFl(p.border)); // a row separator (reads on dark, unlike the old full outline)
    fl_line(rx, ry + rh - 1, rx + rw - 1, ry + rh - 1);
    if (dot != 0) {
        const common::Color8 c = dot == 1 ? p.accent : p.textMuted;
        const double r = dot == 1 ? 3.5 : 3.0; // active dot a touch larger (matches the real panel)
        const int dcx = rx + rw - 10;
        const int dcy = ry + rh / 2;
        drawAAPrims(dcx - 5, dcy - 5, 11, 11, [bg](int, int) { return bg; },
                    {{dcx + 0.0, dcy + 0.0, r, 0.0, c}});
    }
}

// The card's "Properties strip" -- the mini blend-mode combo + opacity slider in the layer-panel's
// flat style. This is what the S15-e setting actually governs, so the mode lives here: `grayed`
// makes the combo + opacity inert (chevron dropped, no accent fill, muted handle/readout), and
// `mixed` shows two blend-name segments with a comma between (a multi-mode selection, cf. E) instead
// of one. An "opacity %" readout line sits to the right of the slider.
void drawMiniStrip(int sx, int sy, int sw, const Palette& p, bool grayed, bool mixed,
                   common::Color8 cardBg) {
    constexpr int comboH = 14;
    fl_color(toFl(p.controlBg)); // blend-mode combo box
    fl_rectf(sx, sy, sw, comboH);
    // lift the outline on dark, where border-on-controlBg is nearly invisible (light is fine as-is)
    fl_color(p.dark ? fl_lighter(toFl(p.border)) : toFl(p.border));
    fl_rect(sx, sy, sw, comboH);
    // The disabled-state ink (the greyed combo text + the % readout): light's p.border reads as a
    // faint disabled mark on the white box, but on dark p.border is near-invisible on controlBg -- so
    // on dark lift it to a mid cool-grey whose contrast over the box matches light's (the user's call).
    const common::Color8 disabledInk = p.dark ? common::Color8{88, 95, 118, 255} : p.border;
    const int ly = sy + comboH / 2 - 1; // the blend-mode name(s)
    fl_color(toFl(grayed ? disabledInk : p.textMuted));
    if (mixed) {
        const int seg = sw / 4;
        fl_rectf(sx + 5, ly, seg, 2);
        fl_rectf(sx + 5 + seg + 2, ly, 2, 4);    // a comma right after the first mode name
        fl_rectf(sx + 5 + seg + 10, ly, seg, 2); // a clear space after the comma, then the next name
    } else {
        fl_rectf(sx + 5, ly, sw / 3, 2);
    }
    if (!grayed) { // dropdown chevron (dropped when disabled, to signal the combo is inert)
        fl_color(toFl(p.text));
        const int chx = sx + sw - 11;
        const int chy = sy + comboH / 2 - 2;
        fl_line(chx, chy, chx + 3, chy + 3);
        fl_line(chx + 3, chy + 3, chx + 6, chy);
    }
    const int trackY = sy + comboH + 9; // opacity slider + a "NN%" readout line at the right
    constexpr int readoutW = 14;
    const int trackW = sw - readoutW - 4;
    const int cy = trackY + 1;
    const int hx = sx + (trackW * 2) / 3;
    // filled portion: accent, or muted grey when disabled -- matching the real Slider, whose fill
    // goes textMuted (a grey on dark, a dark on light) rather than vanishing when it is inactive.
    const common::Color8 fillCol = grayed ? p.textMuted : p.accent;
    fl_color(toFl(p.border)); // a hairline frame, like the real Slider (an empty track is invisible)
    fl_rect(sx - 1, trackY - 1, trackW + 2, 5);
    fl_color(toFl(p.controlBg)); // track
    fl_rectf(sx, trackY, trackW, 3);
    fl_color(toFl(fillCol)); // filled portion
    fl_rectf(sx, trackY, hx - sx, 3);
    // The handle: an AA disc whose under-sampler reproduces the track (+ frame) and falls back to the
    // card background -- so it blends, with no opaque square showing the wrong colour on a hovered card.
    const auto under = [&](int ux, int uy) -> common::Color8 {
        if (ux >= sx - 1 && ux <= sx + trackW && uy >= trackY - 1 && uy <= trackY + 3) {
            if (ux == sx - 1 || ux == sx + trackW || uy == trackY - 1 || uy == trackY + 3)
                return p.border;
            return ux < hx ? fillCol : p.controlBg;
        }
        return cardBg;
    };
    drawAAPrims(hx - 6, cy - 6, 13, 13, under,
                {{hx + 0.0, cy + 0.0, 4.0, 0.0, grayed ? p.textMuted : p.text}});
    fl_color(toFl(grayed ? disabledInk : p.textMuted)); // the opacity % readout
    fl_rectf(sx + trackW + 4, trackY - 1, readoutW - 2, 2);
}

// Multi-selection-edits preview (S15-e), mode = card index. The Properties strip carries the mode
// (this setting governs the combo + opacity); the dots mark the selection. "All selected layers"
// edits every layer, so it gets an accent dot on ALL rows; the others use accent = active layer,
// grey = the rest. The bottom row runs past the card's clip so the stack reads as "and more".
// 0 "Disabled": combo + opacity greyed, mixed label. 1 "All": editable, mixed label, all-accent
// dots. 2 "Active layer only": editable, single label.
void drawMultiSelectPreview(int mx, int my, int mw, int mh, int mode, common::Color8 bg) {
    const Palette& p = activePalette();
    drawMiniStrip(mx, my, mw, p, /*grayed=*/mode == 0, /*mixed=*/mode != 2, /*cardBg=*/bg);
    constexpr int rh = 15;
    constexpr int gap = 2;
    for (int i = 0, ry = my + 36; ry < my + mh; ++i, ry += rh + gap) {
        const int dot = (mode == 1) ? 1 : (i == 0 ? 1 : 2); // All: every row accent; else active+grey
        drawMiniLayerRow(mx, ry, mw, rh, i == 0, dot, p);
    }
}

// Per-channel blend a -> b by t (0..1). Used by the feather-indicator preview to composite the
// translucent selection wash + soft band exactly in C++ (FLTK fl_rectf is opaque, so the illustration
// composites over a KNOWN flat canvas colour rather than relying on GPU alpha).
common::Color8 mixColor(common::Color8 a, common::Color8 b, float t) {
    const float u = 1.0f - t;
    return {static_cast<std::uint8_t>(a.r * u + b.r * t + 0.5f),
            static_cast<std::uint8_t>(a.g * u + b.g * t + 0.5f),
            static_cast<std::uint8_t>(a.b * u + b.b * t + 0.5f), 255};
}

// A vertical marching-ant line at column `lx` (alternating black/white dashes), the settings-card
// stand-in for the canvas marquee -- static (no crawl) since the card is a still.
void drawAntDashes(int lx, int y0, int y1) {
    constexpr int dash = 4;
    for (int y = y0, i = 0; y < y1; y += dash, ++i) {
        fl_color((i & 1) == 0 ? FL_BLACK : FL_WHITE);
        fl_line(lx, y, lx, std::min(y + dash, y1) - 1);
    }
}

// Feathered-selection-indicator preview (Settings -> Appearance), mode = card index. A soft-edged
// "selection" fills the left side and feathers out across a band in the middle, over a flat canvas.
// 0 "Bracketing ant pair" (A): two ants at the band edges (~85% + ~15% coverage); the gap between
// them IS the feather. 1 "True edge + soft band" (F): one crisp 50% ant plus a faint falloff tint
// peaking mid-band (4c(1-c)) -- exactly what the shader draws.
void drawFeatherIndicatorPreview(int mx, int my, int mw, int mh, int mode) {
    const Palette& p = activePalette();
    const common::Color8 canvas = p.canvasBg;                 // the flat "document" the wash lies over
    const common::Color8 selBlue{47, 128, 237, 255};          // the selection hue (== shader kBoxColor)
    const int bandL = mx + (mw * 42) / 100;                   // ~85% coverage contour (inner ant, A)
    const int bandR = mx + (mw * 62) / 100;                   // ~15% coverage contour (outer ant, A)
    const int mid = (bandL + bandR) / 2;                      // ~50% coverage contour (the ant in F)
    const int span = std::max(1, bandR - bandL);
    for (int x = mx; x < mx + mw; ++x) {
        float cov = x <= bandL ? 1.0f : x >= bandR ? 0.0f : 1.0f - float(x - bandL) / float(span);
        common::Color8 c = mixColor(canvas, selBlue, 0.22f * cov); // the translucent selection wash
        if (mode == 1) {                                           // F: the soft band, over the wash
            const float emph = 4.0f * cov * (1.0f - cov);
            c = mixColor(c, common::Color8{255, 255, 255, 255}, 0.30f * emph);
        }
        fl_color(toFl(c));
        fl_line(x, my, x, my + mh - 1);
    }
    if (mode == 1) {
        drawAntDashes(mid, my, my + mh);
    } else {
        drawAntDashes(bandL, my, my + mh);
        drawAntDashes(bandR, my, my + mh);
    }
}

} // namespace

void drawPresetDisplayPreview(int mx, int my, int mw, int mh, int mode, common::Color8 bg) {
    const Palette& p = activePalette();
    fl_color(toFl(bg));
    fl_rectf(mx, my, mw, mh);

    if (mode == 0) { // Grid: 3 columns of square tiles
        constexpr int cols = 3;
        constexpr int gap = 4;
        const int tile = (mw - (cols - 1) * gap) / cols;
        for (int ty = my; ty + tile <= my + mh; ty += tile + gap)
            for (int col = 0; col < cols; ++col) {
                const int tx = mx + col * (tile + gap);
                fl_color(toFl(p.controlBg));
                fl_rectf(tx, ty, tile, tile);
                fl_color(toFl(p.border));
                fl_rect(tx, ty, tile, tile);
                fl_color(toFl(p.textMuted)); // the tip icon's blob
                const double cx = tx + tile / 2.0;
                const double cy = ty + tile / 2.0;
                drawAAPrims(tx + 1, ty + 1, tile - 2, tile - 2,
                            [&](int, int) { return p.controlBg; },
                            {{cx, cy, tile * 0.24, 0.0, p.textMuted}});
            }
        return;
    }

    // Cards: rows of [tip icon][a long strip with a stroke through it]. The stroke is the whole
    // point of the mode, so it is the thing the card has to show.
    constexpr int rowH = 22;
    constexpr int gap = 5;
    const int icon = rowH;
    for (int ry = my; ry + rowH <= my + mh; ry += rowH + gap) {
        fl_color(toFl(p.controlBg)); // the tip icon
        fl_rectf(mx, ry, icon, icon);
        fl_color(toFl(p.border));
        fl_rect(mx, ry, icon, icon);
        drawAAPrims(mx + 1, ry + 1, icon - 2, icon - 2, [&](int, int) { return p.controlBg; },
                    {{mx + icon / 2.0, ry + icon / 2.0, icon * 0.24, 0.0, p.textMuted}});

        const int sx = mx + icon + 4; // the strip
        const int sw = mx + mw - sx;
        if (sw <= 6)
            continue;
        fl_color(toFl(p.controlBg));
        fl_rectf(sx, ry, sw, icon);

        // A tapered S through it: thin at the left, thick at the right, which is what the real
        // preview's pressure ramp lays.
        //
        // ⚠ ANTI-ALIASED, and it has to be. The first cut stamped 1 px `fl_rectf` columns along the
        // curve and the result read as a staircase -- which is a poor advertisement for a mode whose
        // entire pitch is "look how the stroke is shaped". `fl_line` would be no better: FLTK does not
        // anti-alias it either (the same reason the curve editor and the sliders compose their own
        // coverage). So the stroke is laid as a run of AA discs over the card's own ground.
        //
        // ⚠⚠ AND IT IS INSET BY ITS OWN RADIUS, exactly as the real preview's path is. A curve laid
        // against the strip's full width runs from its left frame to its right one and reads as a
        // stroke that RAN OUT OF THE BOX -- which is what it does, and what the user reported. The
        // drawAAPrims blit clips it, so it does not paint over anything; it just gets guillotined at
        // the edge, which looks like the same bug and is.
        constexpr double kMaxR = 2.3; // the fat end of the taper
        const double inset = kMaxR + 2.0;
        const double cy = ry + icon / 2.0;
        const double amp = std::max(1.0, (icon / 2.0) - inset - kMaxR);
        const double x0 = sx + inset;
        const double x1 = sx + sw - 1 - inset;
        std::vector<AAPrim> dabs;
        if (x1 > x0) {
            const int steps = static_cast<int>(x1 - x0);
            dabs.reserve(static_cast<std::size_t>(std::max(0, steps)));
            for (int i = 0; i <= steps; ++i) {
                const double t = static_cast<double>(i) / std::max(1, steps);
                const double y = cy - std::sin(t * 6.2831853) * amp;
                dabs.push_back({x0 + i, y, 0.6 + t * (kMaxR - 0.6), 0.0, p.accent});
            }
        }
        drawAAPrims(sx + 1, ry + 1, sw - 2, icon - 2, [&](int, int) { return p.controlBg; }, dabs);

        fl_color(toFl(p.border));
        fl_rect(sx, ry, sw, icon); // the frame LAST: an opaque AA blit would eat its corners
    }
}

namespace {

// The themed checkbox is now the shared mosaic::ui::CheckBox (widgets.hpp) -- one settled design used
// by both Settings and the Type panel. Its erase-ground defaults to windowBg, which is what these panes
// use, so the CheckBox(...) call sites below are unchanged.

// A horizontal sub-tab strip for a content pane's second level (e.g. per-tool settings within
// Tools). Styled like the app's Layers|History dock tabs: bold labels, the active one full-strength
// with a 2px accent underline, the others muted; a click runs onSelect(index). Self-measuring so the
// tabs pack tightly regardless of label width.
class SubTabBar : public Fl_Widget {
public:
    SubTabBar(int X, int Y, int W, int H, std::vector<std::string> labels,
              std::function<void(int)> onSelect)
        : Fl_Widget(X, Y, W, H), m_labels(std::move(labels)), m_onSelect(std::move(onSelect)) {}

    void setActive(int index) {
        if (index != m_active) {
            m_active = index;
            redraw();
        }
    }

protected:
    int handle(int event) override {
        switch (event) {
        case FL_PUSH: return 1; // claim the press; select on release if it stays inside
        case FL_RELEASE:
            if (Fl::event_inside(this)) {
                const int i = tabAt(Fl::event_x());
                if (i >= 0 && m_onSelect)
                    m_onSelect(i);
            }
            return 1;
        default: return Fl_Widget::handle(event);
        }
    }

    void draw() override {
        const Palette& p = activePalette();
        fl_color(toFl(p.windowBg)); // erase first (transparent pane + double buffer; see OptionCard)
        fl_rectf(x(), y(), w(), h());
        fl_font(FL_HELVETICA_BOLD, 13);
        int tx = x();
        for (int i = 0; i < static_cast<int>(m_labels.size()); ++i) {
            const int tw = tabWidth(i);
            const bool active = i == m_active;
            fl_color(toFl(active ? p.text : p.textMuted));
            fl_draw(m_labels[static_cast<std::size_t>(i)].c_str(), tx, y(), tw, h(),
                    FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            if (active) {
                fl_color(toFl(p.accent));
                fl_line(tx, y() + h() - 2, tx + tw - kTabGap, y() + h() - 2);
            }
            tx += tw;
        }
    }

private:
    static constexpr int kTabGap = 22; // trailing space after each label before the next tab
    int tabWidth(int i) const {
        fl_font(FL_HELVETICA_BOLD, 13);
        int w = 0, h = 0;
        fl_measure(m_labels[static_cast<std::size_t>(i)].c_str(), w, h, 0);
        return w + kTabGap;
    }
    int tabAt(int ex) const {
        int tx = x();
        for (int i = 0; i < static_cast<int>(m_labels.size()); ++i) {
            const int tw = tabWidth(i);
            if (ex >= tx && ex < tx + tw)
                return i;
            tx += tw;
        }
        return -1;
    }
    std::vector<std::string> m_labels;
    std::function<void(int)> m_onSelect;
    int m_active = 0;
};

// ---- The engine / feature "spec sheet" -------------------------------------------------------
// Draws a method header, an "authors · cost" sub-line, a divider, the summary, titled bullet
// lists, and small footnote lines. Fed either from an inpaint BackendInfo (Inpainting → Engine)
// or a retarget RetargetInfo ("About Smart Resize" on Tools → Crop) — same sheet, same look.
// Self-measuring -- the setters resize the widget to its content height so the enclosing
// ScrollView can scroll a long description. Pure-drawn, like the other settings widgets.
class SpecPanel : public Fl_Widget {
public:
    SpecPanel(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

    // Generic content: what layout() actually draws.
    struct Content {
        std::string method;  // bold header
        std::string authors; // muted sub-line
        std::string cost;    // muted sub-line
        std::string summary;
        std::vector<std::pair<std::string, std::vector<std::string>>> sections;
        std::vector<std::string> footnotes; // small muted lines at the end
    };

    void setInfo(const core::inpaint::BackendInfo& info) {
        Content c;
        c.method = info.method;
        c.authors = info.authors;
        c.cost = info.cost;
        c.summary = info.summary;
        if (!info.deviations.empty())
            c.sections.emplace_back(_("Differs from the paper"), info.deviations);
        if (!info.augmentations.empty())
            c.sections.emplace_back(_("Mosaic adds"), info.augmentations);
        if (!info.paper.empty())
            c.footnotes.push_back(std::string(_("Paper: ")) + info.paper);
        setContent(std::move(c));
    }

    void setInfo(const core::retarget::RetargetInfo& info) {
        Content c;
        c.method = info.method;
        c.authors = info.lineage;
        c.cost = info.cost;
        c.summary = info.summary;
        for (const auto& s : info.sections)
            c.sections.emplace_back(s.title, s.items);
        c.footnotes = info.footnotes;
        setContent(std::move(c));
    }

    void setContent(Content content) {
        m_content = std::move(content);
        const int need = layout(x(), y(), false); // may be 0 before there's a GC; draw() corrects it
        if (need > 0)
            size(w(), need); // fit content; the ScrollView re-reads on redraw
        if (parent() != nullptr)
            parent()->redraw();
        redraw();
    }

protected:
    void draw() override {
        // Self-correct the height once a real GC exists (text measurement can return 0 before show).
        const int need = layout(x(), y(), false);
        if (need > 0 && need != h()) {
            size(w(), need);
            if (parent() != nullptr)
                parent()->redraw();
        }
        const Palette& p = activePalette();
        fl_color(toFl(p.windowBg)); // erase (transparent pane + double buffer; see OptionCard)
        fl_rectf(x(), y(), w(), h());
        layout(x(), y(), true);
    }

private:
    // Lay the content out from (ox, oy); paint when `draw`, else only measure. Returns the height.
    int layout(int ox, int oy, bool draw) const {
        const Palette& p = activePalette();
        int yy = oy;
        const int ww = w();
        const auto block = [&](const std::string& s, Fl_Font font, int size, common::Color8 col,
                               int gapAfter) {
            if (s.empty())
                return;
            fl_font(font, size);
            int tw = ww;
            int th = 0;
            fl_measure(s.c_str(), tw, th, 0); // tw = wrap width in; th = wrapped height out
            if (draw) {
                fl_color(toFl(col));
                fl_draw(s.c_str(), ox, yy, ww, th, FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
            }
            yy += th + gapAfter;
        };
        block(m_content.method, FL_HELVETICA_BOLD, 14, p.text, 2);
        block(m_content.authors, FL_HELVETICA, 12, p.textMuted, 2); // authors/year on their own line
        block(m_content.cost, FL_HELVETICA, 12, p.textMuted, 8);    // cost / time on the next line
        if (draw) {
            fl_color(toFl(p.border));
            fl_line(ox, yy, ox + ww, yy);
        }
        yy += 10;
        block(m_content.summary, FL_HELVETICA, 12, p.text, 10);
        for (const auto& [title, items] : m_content.sections) {
            if (items.empty())
                continue;
            block(title, FL_HELVETICA_BOLD, 12, p.text, 2);
            for (const auto& it : items)
                block("\xe2\x80\xa2  " + it, FL_HELVETICA, 12, p.textMuted, 2); // "• " bullet
            yy += 6;
        }
        for (const auto& fn : m_content.footnotes)
            block(fn, FL_HELVETICA, 11, p.textMuted, 2);
        return (yy - oy) + 16; // bottom breathing room so the last line never kisses the edge
    }

    Content m_content;
};

// Format a control's current value for its readout box.
std::string formatParamValue(const core::inpaint::ParamControl& c, double v) {
    char buf[32];
    if (c.kind == core::inpaint::ParamControl::Kind::Int)
        std::snprintf(buf, sizeof buf, "%d", static_cast<int>(std::lround(v)));
    else
        std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// Reset a VERTICAL Fl_Scroll to the TOP, leaving the horizontal layout untouched. scroll_to() alone
// is not enough vertically: Fl_Scroll re-derives its position from the scrollbar WIDGET value on the
// next draw, so a stale value would snap freshly rebuilt content back down — zero the vertical
// scrollbar first. But the X must be PRESERVED: these scrolls inset their content one kMargin from
// the rail, so the natural xposition() is that margin. Passing X=0 (as this used to) yanked the
// content flush-left and made the controls kiss the rail; xposition() keeps the inset.
void resetScrollToTop(Fl_Scroll* sc) {
    if (sc == nullptr)
        return;
    sc->scrollbar.value(0);
    sc->scroll_to(sc->xposition(), 0);
}

// Does `schema` define a preset with this id?
bool presetExists(const core::inpaint::BackendSettingsSchema& schema, const std::string& id) {
    for (const auto& p : schema.presets)
        if (p.id == id)
            return true;
    return false;
}

} // namespace

// Icons sub-pane: rasterized pack preview strips, one per (pack, pixel size), built on first use
// and kept for the dialog's lifetime (the pack list is fixed once the dialog is constructed).
struct SettingsDialog::IconPreviewCache {
    std::map<std::string, PreviewStrip> strips; // "<packId>/<px>"
    const PreviewStrip& get(const SettingsHost& host, const std::string& packId, int px) {
        const std::string key = packId + "/" + std::to_string(px);
        auto it = strips.find(key);
        if (it == strips.end())
            it = strips.emplace(key, buildPreviewStrip(host, packId, px)).first;
        return it->second;
    }
};

// Settings -> Tablet's TEST AREA (docs/tablet.md §8): a live readout of the last sample the wiring
// saw -- pressure as a filled bar, the tilt lean as a dot in a circle, rotation, and the resolved
// sample rate. §8 calls it "the single most useful control on the page", and the reason is that it
// answers "is my tablet working" without the user having to paint anything and then judge the
// result. It reads POST-policy values on purpose: what it shows is what the brush engine will get,
// so a mis-set pressure range or a curve that flattens the top of the stroke is visible HERE.
class TabletTestArea : public Fl_Widget {
public:
    TabletTestArea(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

    void setReading(const SettingsHost::TabletReading& r) {
        m_r = r;
        redraw();
    }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        fl_color(toFl(pal.controlBg));
        fl_rectf(x(), y(), w(), h());
        fl_color(toFl(pal.border));
        fl_rect(x(), y(), w(), h());

        if (!m_r.valid) {
            fl_color(toFl(pal.textMuted));
            fl_font(FL_HELVETICA, 12);
            fl_draw(_("Touch your pen to the tablet — pressure, tilt and rate appear here."),
                    x() + 12, y(), w() - 24, h(), FL_ALIGN_CENTER | FL_ALIGN_WRAP);
            return;
        }

        const int pad = 14;
        int cy = y() + pad;

        // Pressure: a bar, because a bar is the one readout whose SHAPE is the answer -- you press
        // harder and it grows, and a nib that never reaches the end tells you to set the range.
        fl_color(toFl(pal.textMuted));
        fl_font(FL_HELVETICA, 11);
        fl_draw(_("Pressure"), x() + pad, cy, 70, 16, FL_ALIGN_LEFT);
        const int barX = x() + pad + 74;
        const int barW = w() - (barX - x()) - pad - 56;
        fl_color(toFl(pal.windowBg));
        fl_rectf(barX, cy + 3, barW, 11);
        fl_color(toFl(pal.accent));
        fl_rectf(barX, cy + 3, static_cast<int>(std::lround(barW * std::clamp(m_r.pressure, 0.0, 1.0))),
                 11);
        fl_color(toFl(pal.text));
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3f", m_r.pressure);
        fl_draw(buf, barX + barW + 8, cy, 48, 16, FL_ALIGN_LEFT);
        cy += 26;

        // Tilt: a dot in a circle -- the lean, drawn as the lean. Numbers alone do not tell you the
        // pen is reading tilt BACKWARDS, and a dot that moves the wrong way does, instantly.
        const int dialR = 26;
        const int dcx = x() + pad + dialR;
        const int dcy = cy + dialR;
        const double tx = std::clamp(m_r.xTilt / core::brush::kMaxTiltDegrees, -1.0, 1.0);
        const double ty = std::clamp(m_r.yTilt / core::brush::kMaxTiltDegrees, -1.0, 1.0);
        const int dx = dcx + static_cast<int>(std::lround(tx * (dialR - 4)));
        const int dy = dcy + static_cast<int>(std::lround(ty * (dialR - 4)));
        // Ring and dot in ONE patch over the test area's ground: at full lean the dot's rim reaches
        // the ring, and a separate opaque dot patch would bite a notch out of it.
        drawAAArcs(pal.controlBg, {aaCircle(dcx, dcy, dialR, 1.0, pal.border),
                                   aaPieFromBox(dx - 3, dy - 3, 6, 6, 0, 360, pal.accent)});

        fl_color(toFl(pal.text));
        fl_font(FL_HELVETICA, 11);
        const int tX = dcx + dialR + 16;
        std::snprintf(buf, sizeof(buf), "%s  %+.1f°, %+.1f°", _("Tilt"), m_r.xTilt, m_r.yTilt);
        fl_draw(buf, tX, cy + 4, w() - (tX - x()) - pad, 16, FL_ALIGN_LEFT);
        std::snprintf(buf, sizeof(buf), "%s  %+.1f°", _("Rotation"), m_r.rotation);
        fl_draw(buf, tX, cy + 22, w() - (tX - x()) - pad, 16, FL_ALIGN_LEFT);
        // The RATE is the diagnostic that catches a tablet reporting at 60 Hz because it fell back
        // to a generic driver -- a stroke drawn from 60 samples a second goes polygonal on a flick.
        std::snprintf(buf, sizeof(buf), "%s  %.0f Hz", _("Sample rate"), m_r.rateHz);
        fl_draw(buf, tX, cy + 40, w() - (tX - x()) - pad, 16, FL_ALIGN_LEFT);
    }

private:
    SettingsHost::TabletReading m_r;
};

// ---- Keybindings pane (S51-b, docs/keybindings.md) --------------------------------------------
namespace {
constexpr int kKeyRowH = 26;
constexpr int kKeyHeaderH = 26;
constexpr int kKeyResetW = 56;
// The search row sits low enough that a translated caption can grow to three lines without landing
// on it (caption() GROWS to fit -- kCaptionH's rationale), and the list starts a settled gap below.
constexpr int kKeySearchY = 104;
constexpr int kKeyListTop = kKeySearchY + kRowH + 14; // first content row inside the scroll
constexpr int kKeyListW = kInnerW - kScrollGutter;    // the scrolling pane's own content width
constexpr int kKeyRowW = kKeyListW - kKeyResetW - 6;  // ... less the per-row Reset button's column

// Lower-case an ASCII string for the search match. Deliberately ASCII-only: it filters labels the
// user can see, and a locale-aware fold would need ICU to be worth anything.
std::string lowerAsciiCopy(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

// ONE action's list row: the label on the left, its chord right-aligned. Built on NavItem's shape --
// hover wash, claim the whole PUSH/RELEASE pair ([[mosaic-ui-gotchas]]), act on a release that stayed
// inside -- because that is already what a clickable row in this dialog looks like. Clicking arms
// capture: the chord is replaced by "Press a key…" in the accent and the row wears an accent ring, so
// which row is listening is never in doubt. A remapped row draws its chord in the accent too, which is
// the same "this is not the default" language the rest of the app uses.
class SettingsDialog::KeyRow : public Fl_Widget {
public:
    KeyRow(int X, int Y, int W, int H, std::function<void()> onClick)
        : Fl_Widget(X, Y, W, H), m_onClick(std::move(onClick)) {}

    void setContent(std::string label, std::string chord, bool remapped) {
        m_label = std::move(label);
        m_chord = std::move(chord);
        m_remapped = remapped;
        redraw();
    }
    void setCapturing(bool on) {
        if (on != m_capturing) {
            m_capturing = on;
            redraw();
        }
    }

protected:
    int handle(int event) override {
        switch (event) {
        case FL_ENTER: m_hover = true; redraw(); return 1;
        case FL_LEAVE: m_hover = false; redraw(); return 1;
        case FL_PUSH: return 1; // claim the press; act on release if it stays inside
        case FL_RELEASE:
            if (Fl::event_inside(this) && m_onClick)
                m_onClick();
            return 1;
        default: return Fl_Widget::handle(event);
        }
    }

    void draw() override {
        const Palette& p = activePalette();
        // Fill first, always: this row sits on the transparent pane inside a double-buffered window,
        // so without an erase the label composites over its previous frame and thickens (the
        // OptionCard label-strip lesson).
        const common::Color8 bg = m_capturing ? p.controlActive
                                              : (m_hover ? p.controlHover : p.windowBg);
        fl_color(toFl(bg));
        fl_rectf(x(), y(), w(), h());
        if (m_capturing) {
            fl_color(toFl(p.accent));
            fl_rect(x(), y(), w(), h());
        }
        constexpr int pad = 8;
        fl_font(FL_HELVETICA, 12);
        const std::string chord = m_capturing ? std::string(_("Press a key…")) : m_chord;
        const int chordW = static_cast<int>(fl_width(chord.c_str())) + 2;
        fl_color(toFl(m_capturing || m_remapped ? p.accent : p.textMuted));
        fl_draw(chord.c_str(), x() + w() - pad - chordW, y(), chordW, h(),
                FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        fl_color(toFl(p.text));
        // CLIPPED, and never a negative width: a long translated label must stop at the chord rather
        // than draw over it (FLTK neither wraps nor clips a label by itself).
        const int labelW = std::max(0, w() - 3 * pad - chordW);
        fl_draw(m_label.c_str(), x() + pad, y(), labelW, h(),
                FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    }

private:
    std::function<void()> m_onClick;
    std::string m_label;
    std::string m_chord;
    bool m_remapped = false;
    bool m_capturing = false;
    bool m_hover = false;
};

SettingsDialog::SettingsDialog(SettingsHost host)
    : Fl_Double_Window(kWinW, kWinH, _("Settings")), m_host(std::move(host)),
      m_iconPreviews(std::make_unique<IconPreviewCache>()) {
    const Palette& pal = activePalette();
    color(toFl(pal.windowBg));

    begin();

    // ---- left category rail --------------------------------------------------------------
    auto* rail = new Panel(0, 0, kNavW, kContentH);
    m_rail = rail;
    rail->color(toFl(pal.panelBg));
    rail->borderEdges(Panel::EdgeRight); // owns the seam against the content pane
    rail->end();

    // ⚠ POSITIONAL: names[] and the content panes below are pushed in parallel with no enum, so a
    // row inserted here must have its pane inserted at the SAME place. "Rendering" (S60-b) sits
    // AFTER Tablet deliberately -- kTabletSection above pins index 3, and a row added ahead of it
    // would silently re-point the tablet readout timer at somebody else's pane. It pairs with
    // Color Management because the two are the "how the pixels get made" categories.
    // "Keybindings" (S51-b) goes in BEFORE Annoyances and AFTER Tablet, for both reasons the note
    // above gives: kTabletSection pins index 3, so nothing may be inserted ahead of it, and
    // Annoyances stays last because it is the joke drawer.
    const char* names[] = {_("General"),          _("Appearance"), _("Tools"),       _("Tablet"),
                           _("Color Management"), _("Rendering"),  _("Inpainting"),  _("Text"),
                           _("Keybindings"),      _("Annoyances")};
    constexpr int kNavCount = 10;
    int navY = kNavTop;
    for (int i = 0; i < kNavCount; ++i) {
        auto* item = new NavItem(0, navY, kNavW, kNavRowH, names[i], [this, i] { selectSection(i); });
        rail->add(item);
        m_nav.push_back(item);
        navY += kNavRowH;
    }

    // ---- content panes (parallel to the rail; all share one rect, one shown) -------------
    // General -------------------------------------------------------------------------------
    {
        auto* pane = new Fl_Group(kContentX, kContentY, kContentW, kContentH);
        pane->box(FL_NO_BOX);
        pane->begin();
        sectionTitle(kInnerX, 18, _("General"));
        fieldLabel(kInnerX, 64, kLabelW, _("Units"));
        m_units = new Dropdown(kInnerX + kLabelW, 64, 200, kRowH);
        m_units->add(_("Automatic (system)"));
        m_units->add(_("Metric"));
        m_units->add(_("Imperial"));
        m_units->callback([](Fl_Widget*, void* v) { static_cast<SettingsDialog*>(v)->onUnitsChanged(); },
                          this);
        caption(kInnerX, 64 + kRowH + 8, kInnerW, 36,
                _("Measurement system for rulers and size readouts. Automatic follows your "
                  "system locale."));
        // UI language (docs/i18n.md). The list is the languages a catalog is actually INSTALLED
        // for -- offering one whose .mo is absent would silently do nothing -- so it is empty on an
        // uninstalled build with no $MOSAIC_LOCALEDIR, and the control is then greyed rather than
        // pretending. "System default" and "English" are always offered: the first is the empty
        // setting, the second needs no catalog (the msgids ARE the English) and is the only way
        // back to English on a non-English system.
        const int ly = 64 + kRowH + 8 + 36 + 14;
        fieldLabel(kInnerX, ly, kLabelW, _("Language"));
        m_language = new Dropdown(kInnerX + kLabelW, ly, 200, kRowH);
        m_languageCodes.emplace_back(""); // System default
        m_language->add(_("System default"));
        m_languageCodes.emplace_back("en");
        m_language->add("English");
        {
            std::vector<const common::LanguageInfo*> installed;
            for (const std::string& code : common::i18n::installedLanguages())
                if (const common::LanguageInfo* li = common::findLanguage(code))
                    installed.push_back(li);
            // By ENGLISH name, not by endonym: a list sorted by endonym is sorted by an alphabet
            // the reader may not have, and the endonym is what they scan for anyway (it is first
            // in the label). Sorting the codes would put "cs" between "ca" and "da" for no reason
            // a user can see.
            std::sort(installed.begin(), installed.end(),
                      [](const common::LanguageInfo* a, const common::LanguageInfo* b) {
                          return std::string_view(a->english) < std::string_view(b->english);
                      });
            for (const common::LanguageInfo* li : installed) {
                m_languageCodes.emplace_back(li->code);
                // "Deutsch (German)" -- the endonym leads, because that is the word a speaker of
                // the language recognises; the English name disambiguates for everyone else. No
                // parenthetical when the two are the same word (Afrikaans, Esperanto, ...).
                std::string label = li->endonym;
                if (label != li->english)
                    label += " (" + std::string(li->english) + ")";
                m_language->add(label.c_str());
            }
            if (installed.empty())
                m_language->deactivate(); // no catalogs: nothing to switch TO
        }
        m_language->callback(
            [](Fl_Widget*, void* v) { static_cast<SettingsDialog*>(v)->onLanguageChanged(); },
            this);
        caption(kInnerX, ly + kRowH + 8, kInnerW, 44,
                _("The language Mosaic's own menus and dialogs are written in. It applies the next "
                  "time Mosaic starts -- the window is built in the language it launched with."));
        // The export-format breadth switch (docs/export-system-plan.md §0/§3). It gates only the
        // EXOTIC tier, which arrives at M7 -- until then it is honestly inert, and the caption does
        // not pretend otherwise. The curated set is always offered and this cannot hide it.
        const int gy = ly + kRowH + 8 + 44 + 14;
        m_showAllExportFormats = new CheckBox(
            kInnerX, gy, kInnerW, 22, _("Show all export formats"), [this](bool on) {
                if (m_host.setShowAllExportFormats)
                    m_host.setShowAllExportFormats(on);
            });
        caption(kInnerX, gy + 22 + kCaptionGap, kInnerW, 44,
                _("The Export dialog normally offers the formats people actually export to. With "
                  "this on it also lists the specialised ones -- the formats a particular tool or "
                  "era wants. Nothing is removed either way."));
        pane->end();
        m_panes.push_back(pane);
    }
    // Appearance: General | Icons sub-tabs (S52). General keeps the theme + overlay-line cards
    // (in a scroll -- the two card rows outgrow the pane once the tab strip claims its row; the
    // crop sub-pane's pattern); Icons is the tool icon-pack browser: the selected pack's preview
    // + credits on top, a scrollable card grid of every installed pack below.
    {
        constexpr int kSubTop = 86; // y of the first row inside a sub-pane (the Tools pane rhythm)
        auto* pane = new Fl_Group(kContentX, kContentY, kContentW, kContentH);
        pane->box(FL_NO_BOX);
        pane->begin();
        sectionTitle(kInnerX, 18, _("Appearance"));
        m_appearanceTabs = new SubTabBar(kInnerX, 52, kInnerW, 26, {_("General"), _("Icons")},
                                         [this](int i) { selectAppearanceTab(i); });

        // -- General sub-pane -------------------------------------------------------------------
        {
            auto* sp = new ScrollView(kContentX, kSubTop, kContentW, kContentH - kSubTop);
            sp->type(Fl_Scroll::VERTICAL);
            sp->box(FL_NO_BOX);
            sp->color(FL_BACKGROUND_COLOR);
            sp->begin();
            caption(kInnerX, kSubTop, kInnerW, 36,
                    _("Choose how Mosaic looks. \"System\" follows your desktop's light / dark "
                      "preference and changes are applied immediately."));
            const struct {
                const char* label;
                ThemeMode mode;
            } cards[] = {
                {_("System"), ThemeMode::System},
                {_("Dark"), ThemeMode::Dark},
                {_("Light"), ThemeMode::Light},
            };
            const int themeY = kSubTop + 44;
            int cx = kInnerX;
            for (const auto& c : cards) {
                const ThemeMode mode = c.mode;
                auto* card = new OptionCard(
                    cx, themeY, kCardW, kCardPreviewH + kCardLabelH, c.label,
                    [mode](int mx, int my, int mw, int mh, common::Color8) {
                        drawThemeModePreview(mx, my, mw, mh, mode);
                    },
                    [this, mode] {
                        selectCardAt(m_themeCards, themeModeIndex(mode));
                        if (m_host.setThemeMode)
                            m_host.setThemeMode(std::string(themeModeKey(mode)));
                    });
                m_themeCards.push_back(card);
                cx += kCardW + kCardGap;
            }
            // Selection & reticle line (2026-07-07 design rounds): three ANIMATED cards, each
            // running its style live over a drifting gradient -- the scene the styles were judged
            // on; a static swatch cannot show what a content-keyed line does. lineCardTick
            // advances the shared phase while the dialog is on screen.
            const int lineY = themeY + kCardPreviewH + kCardLabelH + 16;
            fieldLabel(kInnerX, lineY, kInnerW, _("Selection & reticle line"));
            caption(kInnerX, lineY + 22, kInnerW, 36,
                    _("How the lasso, brush reticle and text-frame line keeps itself readable over "
                      "the image. The image drifts beneath each line."));
            const char* lineLabels[] = {_("Classic"), _("Shadowed"), _("Adaptive")};
            cx = kInnerX;
            for (int i = 0; i < 3; ++i) {
                auto* card = new OptionCard(
                    cx, lineY + 64, kCardW, kCardPreviewH + kCardLabelH, lineLabels[i],
                    [this, i](int mx, int my, int mw, int mh, common::Color8) {
                        drawLineStylePreview(mx, my, mw, mh, i, m_linePhase,
                                             i * (kCardW + kCardGap));
                    },
                    [this, i] {
                        selectCardAt(m_lineStyleCards, i);
                        if (m_host.setOverlayLineStyle)
                            m_host.setOverlayLineStyle(lineStyleKey(i));
                    });
                m_lineStyleCards.push_back(card);
                cx += kCardW + kCardGap;
            }
            // Feathered selection indicator (A / F): how a soft-edged selection conveys its feather
            // width + location, which a single 50% marching ant cannot. Two static cards (unlike the
            // animated line cards -- a still already shows the two contours vs. the band); the
            // preview composites over a flat canvas so its translucency reads exactly.
            const int featherY = lineY + 64 + kCardPreviewH + kCardLabelH + 16;
            fieldLabel(kInnerX, featherY, kInnerW, _("Feathered selection indicator"));
            // The description wraps to several lines; place the cards below the caption's ACTUAL
            // (grown) height plus the standard caption gap so the text never crowds them -- the
            // caption() contract is to stack by the returned box's h(), not the estimate passed in.
            Fl_Box* featherCap =
                caption(kInnerX, featherY + 22, kInnerW, 58,
                        _("How a soft-edged (feathered) selection shows its softness, which a single "
                          "marching ant cannot. \"Bracketing ant pair\" fences the feather between two "
                          "ants; \"True edge + soft band\" keeps the crisp edge and adds a faint band."));
            const int featherCardsY = featherCap->y() + featherCap->h() + kCaptionGap;
            const char* featherLabels[] = {_("Bracketing ant pair"), _("True edge + soft band")};
            cx = kInnerX;
            for (int i = 0; i < 2; ++i) {
                auto* card = new OptionCard(
                    cx, featherCardsY, kCardW, kCardPreviewH + kCardLabelH, featherLabels[i],
                    [i](int mx, int my, int mw, int mh, common::Color8) {
                        drawFeatherIndicatorPreview(mx, my, mw, mh, i);
                    },
                    [this, i] {
                        selectCardAt(m_featherCards, i);
                        if (m_host.setFeatherIndicator)
                            m_host.setFeatherIndicator(featherIndicatorKey(i));
                    });
                m_featherCards.push_back(card);
                cx += kCardW + kCardGap;
            }
            // Bottom breathing room below the last Appearance card (matches the tool sub-panes).
            new Fl_Box(kInnerX, featherCardsY + kCardPreviewH + kCardLabelH, 1, kMargin);
            sp->end();
            m_appearancePanes.push_back(sp);
        }

        // -- Icons sub-pane (the pack browser, S52) -----------------------------------------------
        {
            auto* sp = new Fl_Group(kContentX, kSubTop, kContentW, kContentH - kSubTop);
            sp->box(FL_NO_BOX);
            sp->begin();
            auto* detail = new IconPackDetailPanel(kInnerX, kSubTop, kInnerW, 120);
            m_iconPackDetail = detail;
            const int gridTop = kSubTop + 120 + 14;
            auto* grid = new ScrollView(kContentX, gridTop, kContentW, kContentH - gridTop);
            grid->type(Fl_Scroll::VERTICAL);
            grid->box(FL_NO_BOX);
            grid->color(FL_BACKGROUND_COLOR);
            grid->begin();
            int cx = kInnerX;
            int cy = gridTop;
            for (int i = 0; i < static_cast<int>(m_host.iconPacks.size()); ++i) {
                const std::string packId = m_host.iconPacks[static_cast<std::size_t>(i)].id;
                auto* card = new OptionCard(
                    cx, cy, kCardW, kCardPreviewH + kCardLabelH, nullptr,
                    [this, packId](int mx, int my, int mw, int mh, common::Color8) {
                        drawPreviewStrip(m_iconPreviews->get(m_host, packId, 20), 20, mx, my, mw,
                                         mh);
                    },
                    [this, i] { selectIconPack(i, /*fromUser=*/true); });
                // Pack names are runtime strings: copy_label owns a copy (a bare label() would
                // dangle when the host vector reallocates).
                card->copy_label(m_host.iconPacks[static_cast<std::size_t>(i)].name.c_str());
                m_iconPackCards.push_back(card);
                if (i % 3 == 2) {
                    cx = kInnerX;
                    cy += kCardPreviewH + kCardLabelH + kCardGap;
                } else {
                    cx += kCardW + kCardGap;
                }
            }
            grid->end();
            sp->end();
            m_appearancePanes.push_back(sp);
        }
        selectAppearanceTab(0);
        pane->end();
        m_panes.push_back(pane);
    }
    // Tools: a horizontal sub-tab strip (one tab per tool that HAS settings) over per-tool sub-panes
    // -- cohesive with the app's Layers|History dock tabs, and keeping each tool's settings on their
    // own short pane so nothing gets buried in one long scroll as more tools gain settings.
    {
        constexpr int kToolTop = 86; // y of the first setting inside a sub-pane (below title + tabs)
        auto* pane = new Fl_Group(kContentX, kContentY, kContentW, kContentH);
        pane->box(FL_NO_BOX);
        pane->begin();
        sectionTitle(kInnerX, 18, _("Tools"));
        auto* tabs = new SubTabBar(kInnerX, 52, kInnerW, 26,
                                   {_("Crop"), _("Move"), _("Lasso"), _("Brush"), _("Eraser")},
                                   [this](int i) { selectToolTab(i); });
        m_toolTabs = tabs;

        // -- Crop sub-pane ------------------------------------------------------------------
        {
            auto* sp = new ScrollView(kContentX, kToolTop, kContentW, kContentH - kToolTop);
            sp->type(Fl_Scroll::VERTICAL); // grows past the pane -> a themed vertical scrollbar
            sp->box(FL_NO_BOX);
            sp->color(FL_BACKGROUND_COLOR); // = windowBg; the scroll's themed trough blends into it
            sp->begin();
            int y = kToolTop;
            fieldLabel(kInnerX, y, kInnerW, _("Crop framing"));
            y += kRowH;
            const char* framingLabels[] = {_("Whole canvas"), _("Inset"), _("Draw to begin")};
            int cx = kInnerX;
            for (int i = 0; i < 3; ++i) {
                auto* card = new OptionCard(
                    cx, y, kCardW, kCardPreviewH + kCardLabelH, framingLabels[i],
                    [i](int mx, int my, int mw, int mh, common::Color8) {
                        drawCropFramingPreview(mx, my, mw, mh, i);
                    },
                    [this, i] {
                        selectCardAt(m_cropCards, i);
                        if (m_host.setCropInitialFraming)
                            m_host.setCropInitialFraming(cropFramingKey(i));
                    });
                m_cropCards.push_back(card);
                cx += kCardW + kCardGap;
            }
            y += kCardPreviewH + kCardLabelH + kCaptionGap;
            caption(kInnerX, y, kInnerW, 44,
                    _("What the Crop tool frames when you pick it. \"Whole canvas\" (the default) "
                      "selects the entire image; \"Inset\" leaves a 15% margin; \"Draw to begin\" "
                      "stages nothing until you drag out a rectangle."));
            y += 44 + kSettingGap;
            m_cropSwitchTool =
                new CheckBox(kInnerX, y, kInnerW, 22,
                             _("Switch to the previous tool after applying a crop"), [this](bool on) {
                                 if (m_host.setCropSwitchToolAfterApply)
                                     m_host.setCropSwitchToolAfterApply(on);
                             });
            y += 22 + kCaptionGap;
            caption(kInnerX, y, kInnerW, 34,
                    _("Off (the default, like most editors) keeps the Crop tool active after you "
                      "apply; on returns you to the tool you were using before."));
            y += 34 + 18; // a tighter gap so the second toggle fits (no caption; the label says it)
            m_cropClearOnLeave =
                new CheckBox(kInnerX, y, kInnerW, 22,
                             _("Clear the crop selection when leaving the Crop tool"), [this](bool on) {
                                 if (m_host.setCropClearSelectionOnLeave)
                                     m_host.setCropClearSelectionOnLeave(on);
                             });
            y += 22 + kSettingGap;
            // "About Smart Resize" — the crediting sheet (docs/smart-resize-research.md §7: the
            // attribution is a user-visible feature, not a source-comment afterthought). Smart
            // Resize is one Crop-bar toggle with no settings pane of its own, so its record lives
            // at the very bottom of the Crop tab. Self-measuring; its own bottom margin is the
            // pane's breathing room, so no trailing spacer Fl_Box is needed here.
            fieldLabel(kInnerX, y, kInnerW, _("About Smart Resize"));
            y += kRowH + 2;
            auto* about = new SpecPanel(kInnerX, y, kInnerW - 6, 60);
            about->setInfo(core::retarget::retargetInfo());
            sp->end();
            m_toolPanes.push_back(sp);
        }

        // -- Move sub-pane: multi-selection edits (S15-e) -----------------------------------
        {
            auto* sp = new ScrollView(kContentX, kToolTop, kContentW, kContentH - kToolTop);
            sp->type(Fl_Scroll::VERTICAL);
            sp->box(FL_NO_BOX);
            sp->color(FL_BACKGROUND_COLOR); // = windowBg (matches the Crop pane)
            sp->begin();
            int y = kToolTop;
            fieldLabel(kInnerX, y, kInnerW, _("Multi-selection edits"));
            y += kRowH;
            const char* msLabels[] = {_("Disabled"), _("All selected layers"), _("Active layer only")};
            int cx = kInnerX;
            for (int i = 0; i < 3; ++i) {
                auto* card = new OptionCard(
                    cx, y, kCardW, kCardPreviewH + kCardLabelH, msLabels[i],
                    [i](int mx, int my, int mw, int mh, common::Color8 bg) {
                        drawMultiSelectPreview(mx, my, mw, mh, i, bg);
                    },
                    [this, i] {
                        selectCardAt(m_multiSelectCards, i);
                        if (m_host.setMultiSelectionEdits)
                            m_host.setMultiSelectionEdits(multiSelectKey(i));
                    });
                m_multiSelectCards.push_back(card);
                cx += kCardW + kCardGap;
            }
            y += kCardPreviewH + kCardLabelH + kCaptionGap;
            caption(kInnerX, y, kInnerW, 76,
                    _("What a blend or opacity change does while several layers are selected "
                      "(shift-click them with the Move tool). \"Disabled\" edits one layer at a time; "
                      "\"All selected layers\" applies to every selected layer in one step; \"Active "
                      "layer only\" applies just to the active layer."));
            new Fl_Box(kInnerX, y + 76, 1, kMargin); // bottom breathing room (matches the Crop pane)
            sp->end();
            m_toolPanes.push_back(sp);
        }

        // -- Lasso sub-pane: freehand smoothing (F) -----------------------------------------
        {
            auto* sp = new ScrollView(kContentX, kToolTop, kContentW, kContentH - kToolTop);
            sp->type(Fl_Scroll::VERTICAL);
            sp->box(FL_NO_BOX);
            sp->color(FL_BACKGROUND_COLOR); // = windowBg (matches the other sub-panes)
            sp->begin();
            int y = kToolTop;
            fieldLabel(kInnerX, y, kInnerW, _("Freehand lasso"));
            y += kRowH;
            m_lassoSmooth =
                new CheckBox(kInnerX, y, kInnerW, 22, _("Smooth the freehand lasso"), [this](bool on) {
                    if (m_host.setLassoSmooth)
                        m_host.setLassoSmooth(on);
                });
            y += 22 + kCaptionGap;
            caption(kInnerX, y, kInnerW, 56,
                    _("On (the default) smooths the freehand lasso into a clean curve -- for both the "
                      "live preview and the committed selection -- which actually follows your stroke "
                      "more faithfully than the raw path, whose decimated samples read as a jagged "
                      "staircase. Off uses that raw path. The polygonal lasso is never smoothed."));
            new Fl_Box(kInnerX, y + 56, 1, kMargin); // bottom breathing room (matches the other panes)
            sp->end();
            m_toolPanes.push_back(sp);
        }

        // -- Brush sub-pane: how the preset dock draws the library (docs/brushes.md §8.2) ----
        {
            auto* sp = new ScrollView(kContentX, kToolTop, kContentW, kContentH - kToolTop);
            sp->type(Fl_Scroll::VERTICAL);
            sp->box(FL_NO_BOX);
            sp->color(FL_BACKGROUND_COLOR); // = windowBg (matches the other sub-panes)
            sp->begin();
            int y = kToolTop;
            fieldLabel(kInnerX, y, kInnerW, _("Preset dock"));
            y += kRowH;
            const char* dispLabels[] = {_("Grid"), _("Cards")};
            int cx = kInnerX;
            for (int i = 0; i < 2; ++i) {
                auto* card = new OptionCard(
                    cx, y, kCardW, kCardPreviewH + kCardLabelH, dispLabels[i],
                    [i](int mx, int my, int mw, int mh, common::Color8 bg) {
                        drawPresetDisplayPreview(mx, my, mw, mh, i, bg);
                    },
                    [this, i] {
                        selectCardAt(m_brushDisplayCards, i);
                        if (m_host.setBrushPresetDisplay)
                            m_host.setBrushPresetDisplay(i == 0 ? "grid" : "cards");
                    });
                m_brushDisplayCards.push_back(card);
                cx += kCardW + kCardGap;
            }
            y += kCardPreviewH + kCardLabelH + kCaptionGap;
            caption(kInnerX, y, kInnerW, 76,
                    _("How the brush-preset dock lists the library. \"Cards\" (the default) gives each "
                      "preset a row carrying its tip and a stroke of that very brush, painted by the "
                      "real engine -- so you can see the mark it makes before you make it. \"Grid\" "
                      "packs the tips into a denser grid of tiles, for reaching a brush you already "
                      "know by sight."));
            new Fl_Box(kInnerX, y + 76, 1, kMargin); // bottom breathing room (matches the other panes)
            sp->end();
            m_toolPanes.push_back(sp);
        }

        // -- Eraser sub-pane ----------------------------------------------------------------
        {
            auto* sp = new ScrollView(kContentX, kToolTop, kContentW, kContentH - kToolTop);
            sp->type(Fl_Scroll::VERTICAL);
            sp->box(FL_NO_BOX);
            sp->color(FL_BACKGROUND_COLOR); // = windowBg (matches the other sub-panes)
            sp->begin();
            int y = kToolTop;
            fieldLabel(kInnerX, y, kInnerW, _("Size"));
            y += kRowH;
            m_eraserSizeTie = new CheckBox(kInnerX, y, kInnerW, 22,
                                           _("Eraser size follows the brush"), [this](bool on) {
                                               if (m_host.setEraserSizeFollowsBrush)
                                                   m_host.setEraserSizeFollowsBrush(on);
                                           });
            y += 22 + kCaptionGap;
            caption(kInnerX, y, kInnerW, 56,
                    _("On (the default) keeps one shared tip size: resizing the brush resizes the "
                      "eraser and the other way round, so switching tools never changes the mark "
                      "you make. Off gives the eraser its own independent size."));
            y += 56 + kMargin;

            // The preset tie (docs/brushes.md §8.4). It is a genuine preference and not a
            // strictly-better default -- Photoshop ties the pair, Krita does not -- so it earns a
            // toggle where a settled answer would not.
            fieldLabel(kInnerX, y, kInnerW, _("Preset"));
            y += kRowH;
            m_eraserPresetTie =
                new CheckBox(kInnerX, y, kInnerW, 22, _("Eraser preset follows the brush"),
                             [this](bool on) {
                                 if (m_host.setEraserPresetFollowsBrush)
                                     m_host.setEraserPresetFollowsBrush(on);
                             });
            y += 22 + kCaptionGap;
            caption(kInnerX, y, kInnerW, 72,
                    _("Off (the default) gives the eraser its own preset, kept apart from the "
                      "brush's, so reaching for one never re-points the other. Note that the two "
                      "preset lists do not overlap -- a preset belongs to the eraser because it "
                      "erases, and each tool is shown only its own -- so \"follow the brush\" can "
                      "at most follow it to a nib the eraser is never offered."));
            new Fl_Box(kInnerX, y + 72, 1, kMargin); // bottom breathing room (matches the other panes)
            sp->end();
            m_toolPanes.push_back(sp);
        }

        pane->end();
        m_panes.push_back(pane);
        selectToolTab(0);
    }
    // Tablet (docs/tablet.md §8) -------------------------------------------------------------
    // The global input policy (§7) -- what every raw sample is put through at ingest, before any
    // preset's dynamics see it -- plus the diagnostics that answer "is my tablet working". The
    // stabilizer row §8 lists is NOT here: it is not built, and a control that does nothing is
    // worse than no control. Nor is the API selector (Windows only).
    {
        auto* pane = new Fl_Group(kContentX, kContentY, kContentW, kContentH);
        pane->box(FL_NO_BOX);
        pane->begin();
        sectionTitle(kInnerX, 18, _("Tablet"));

        auto* scroll = new ScrollView(kContentX, 52, kContentW, kContentH - 52);
        scroll->type(Fl_Scroll::VERTICAL);
        scroll->box(FL_NO_BOX);
        scroll->color(FL_BACKGROUND_COLOR);
        scroll->begin();
        int y = 56;

        const Palette& pane_pal = activePalette();
        // Every control on this page sits on the pane's windowBg, not the panelBg a slider/dial
        // clears its cell to by default -- without this each one paints its own visibly darker
        // rectangle and the page reads as a stack of little boxes.
        const auto ground = [&pane_pal](auto* w) {
            w->setCellColor(pane_pal.windowBg);
            return w;
        };
        // Two half-width controls in the control column, with a gap.
        constexpr int kHalfW = (kCtlW - 12) / 2;
        constexpr int kHalfX2 = kCtlX + kHalfW + 12;

        // -- detected devices: diagnostic, not configurable -----------------------------------
        {
            std::vector<std::string> lines;
            if (m_host.tabletBackend.empty()) {
                lines.emplace_back(_("No tablet backend is running — pressure is unavailable, and "
                                     "strokes paint at full pressure."));
            } else {
                lines.emplace_back(std::string(_("Backend: ")) + m_host.tabletBackend);
                if (m_host.tabletDevices.empty()) {
                    lines.emplace_back(
                        _("No tablet devices detected. Plug one in and reopen Settings."));
                }
                for (const SettingsHost::TabletDeviceRow& d : m_host.tabletDevices) {
                    std::string line = "• " + d.name;
                    if (!d.tool.empty())
                        line += "  (" + d.tool + ")";
                    // An empty valuator list is the WHOLE diagnosis for "my pen has no pressure":
                    // say so, rather than leaving a blank the user has to interpret.
                    line += "  — " + (d.valuators.empty() ? std::string(_("no valuators reported"))
                                                          : d.valuators);
                    lines.push_back(std::move(line));
                }
            }
            // A wrapped Fl_Box does not grow to fit its label: a box sized for ONE row per device
            // spills its overflow across whatever is drawn below it (the shipped pane ran the device
            // list straight through the Test area's title). Size it for the rows the text will
            // actually take, from a deliberately PESSIMISTIC 7 px/char at 12 px Helvetica -- real
            // glyphs average nearer 6, so this over-counts and the box ends up roomy, never short.
            constexpr int kRowLead = 16;
            const int perLine = std::max(20, kCtlW / 7);
            int rows = 0;
            std::string summary;
            for (const std::string& l : lines) {
                rows += std::max<int>(1, (static_cast<int>(l.size()) + perLine - 1) / perLine);
                if (!summary.empty())
                    summary += "\n";
                summary += l;
            }
            fieldLabel(kInnerX, y, kLabelW, _("Devices"));
            auto* box = new Fl_Box(kCtlX, y, kCtlW, std::max(36, kRowLead * rows + 8));
            box->box(FL_NO_BOX);
            box->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
            box->labelsize(12);
            box->labelcolor(toFl(pane_pal.textMuted));
            box->copy_label(summary.c_str());
            y += box->h() + kSettingGap;
        }

        // -- the TEST AREA. §8: the single most useful control on the page ---------------------
        fieldLabel(kInnerX, y, kLabelW, _("Test area"));
        m_tabletTestArea = new TabletTestArea(kCtlX, y, kCtlW, 108);
        y += 108 + kCaptionGap;
        y += caption(kCtlX, y, kCtlW, 54,
                     _("Live, and already through the settings below — what you see here is what "
                       "the brush will get. Hover the pen; you do not have to press."))
                ->h() +
             kSettingGap;

        // -- pressure response curve (the §8.3 widget) -----------------------------------------
        // Editor on the left, Reset beside it, and the explanation UNDER both across the full
        // column: squeezed into the strip beside a 260 px editor it had ~68 px to wrap into.
        fieldLabel(kInnerX, y, kLabelW, _("Pressure curve"));
        {
            constexpr int kCurveW = 240;
            constexpr int kCurveH = 180;
            m_tabletCurve = new CurveEditor(kCtlX, y, kCurveW, kCurveH);
            m_tabletCurve->setCellColor(pane_pal.windowBg);
            m_tabletCurve->onChanged([this](const core::brush::Curve& c) {
                if (m_host.setTabletPressureCurve)
                    m_host.setTabletPressureCurve(c.toString());
            });
            auto* reset = new FlatButton(kCtlX + kCurveW + 12, y, kCtlW - kCurveW - 12, kRowH,
                                         _("Reset"));
            reset->callback(
                [](Fl_Widget*, void* v) { static_cast<SettingsDialog*>(v)->m_tabletCurve->reset(); },
                this);
            y += kCurveH + kCaptionGap;
            y += caption(kCtlX, y, kCtlW, 90,
                         _("How hard you press (left to right) maps to how much paint comes out "
                           "(bottom to top). Drag a point to bend it; click to add one, right-click "
                           "to remove it, double-click to make it a sharp corner. A line from corner "
                           "to corner is no change at all."))
                    ->h() +
                 kSettingGap;
        }

        // -- pressure range: the worn-nib clamp ------------------------------------------------
        fieldLabel(kInnerX, y, kLabelW, _("Pressure range"));
        {
            // Two unitless 0..1 sliders side by side are indistinguishable without these.
            const int lh = std::max(caption(kCtlX, y, kHalfW, kSubLabelH, _("Lightest"))->h(),
                                    caption(kHalfX2, y, kHalfW, kSubLabelH, _("Heaviest"))->h());
            const int sy = y + lh + 2;
            auto* lo = ground(new ScrubSlider(kCtlX, sy, kHalfW, kRowH));
            lo->range(0.0, 1.0);
            lo->step(0.01);
            lo->setDefaultValue(0.0);
            lo->callback([](Fl_Widget*, void* v) { static_cast<SettingsDialog*>(v)->onTabletRange(); },
                        this);
            m_tabletPressureMin = lo;
            auto* hi = ground(new ScrubSlider(kHalfX2, sy, kHalfW, kRowH));
            hi->range(0.0, 1.0);
            hi->step(0.01);
            hi->setDefaultValue(1.0);
            hi->callback([](Fl_Widget*, void* v) { static_cast<SettingsDialog*>(v)->onTabletRange(); },
                        this);
            m_tabletPressureMax = hi;
            y = sy + kRowH + kCaptionGap;
            y += caption(kCtlX, y, kCtlW, 54,
                         _("Stretch the part of the range your pen actually reaches out to the full "
                           "stroke. A worn nib that never bottoms out is what this is for."))
                    ->h() +
                 kSettingGap;
        }

        // -- tilt direction offset -------------------------------------------------------------
        fieldLabel(kInnerX, y, kLabelW, _("Tilt offset"));
        {
            auto* dial = ground(new Dial(kCtlX, y, 48, 48));
            dial->range(-180.0, 180.0);
            dial->step(1.0);
            dial->callback(
                [](Fl_Widget* w, void* v) {
                    auto* d = static_cast<SettingsDialog*>(v);
                    if (d->m_host.setTabletTiltOffset)
                        d->m_host.setTabletTiltOffset(static_cast<Fl_Valuator*>(w)->value());
                },
                this);
            m_tabletTiltOffset = dial;
            const int ch = caption(kCtlX + 60, y, kCtlW - 60, 54,
                                   _("For a pen you hold turned. Shifts which way the tablet thinks "
                                     "you are leaning, without changing how far."))
                               ->h();
            y += std::max(56, ch + 2) + kSettingGap; // the dial is 48 tall; the caption may be more
        }

        // -- speed smoothing -------------------------------------------------------------------
        fieldLabel(kInnerX, y, kLabelW, _("Speed"));
        {
            const int lh = std::max(caption(kCtlX, y, kHalfW, kSubLabelH, _("Counts as full"))->h(),
                                    caption(kHalfX2, y, kHalfW, kSubLabelH, _("Averaged over"))->h());
            const int sy = y + lh + 2;
            auto* mx = ground(new ScrubSlider(kCtlX, sy, kHalfW, kRowH));
            mx->range(0.1, 20.0);
            mx->step(0.1);
            mx->setSuffix("px/ms");
            mx->setDefaultValue(3.0);
            mx->callback([](Fl_Widget*, void* v) { static_cast<SettingsDialog*>(v)->onTabletSpeed(); },
                        this);
            m_tabletSpeedMax = mx;
            auto* win = ground(new ScrubSlider(kHalfX2, sy, kHalfW, kRowH));
            win->range(1.0, 250.0);
            win->step(1.0);
            win->setSuffix("ms");
            win->setDefaultValue(30.0);
            win->callback([](Fl_Widget*, void* v) { static_cast<SettingsDialog*>(v)->onTabletSpeed(); },
                         this);
            m_tabletSpeedWindow = win;
            y = sy + kRowH + kCaptionGap;
            y += caption(kCtlX, y, kCtlW, 54,
                         _("Calibrates brushes that respond to how fast you draw: the speed that "
                           "counts as \"full\", and how long a window it is averaged over."))
                    ->h() +
                 kMargin;
        }


        new Fl_Box(kInnerX, y, 1, kMargin); // bottom breathing room
        scroll->end();
        pane->end();
        m_panes.push_back(pane);
    }
    // Color Management ----------------------------------------------------------------------
    {
        auto* pane = new Fl_Group(kContentX, kContentY, kContentW, kContentH);
        pane->box(FL_NO_BOX);
        pane->begin();
        sectionTitle(kInnerX, 18, _("Color Management"));
        // Name the built-in default so "Use default" / the field below aren't a mystery. Falls back
        // to a generic line if no profile is compiled in (a build with it stripped). copy_label so
        // the box owns the dynamic string (the helper stores the pointer without copying).
        const std::string cmykCaption =
            m_host.defaultCmykName.empty()
                ? std::string(_("Mosaic includes a default CMYK profile, though some distributions "
                                "may omit it. Choose a CMYK .icc here to use your own instead; "
                                "\"Use default\" restores it."))
                : std::string(_("Mosaic's built-in CMYK profile is ")) + m_host.defaultCmykName +
                      _(" (a FOGRA39 press profile); some distributions may omit it. Choose a CMYK "
                        ".icc here to use your own instead; \"Use default\" restores the built-in.");
        caption(kInnerX, 52, kInnerW, 64, "x")->copy_label(cmykCaption.c_str());
        fieldLabel(kInnerX, 116, kInnerW, _("CMYK profile"));
        m_cmykField = new TextOutput(kInnerX, 144, kInnerW, kRowH);
        m_cmykField->box(MOSAIC_INPUT_BOX); // a real outlined field with text padding, like inputs
        m_cmykField->color(toFl(pal.controlBg));
        m_cmykField->textcolor(toFl(pal.text));
        m_cmykField->textsize(13);
        // It must stay focusable (NO clear_visible_focus): a field that can't take keyboard focus
        // never receives FL_KEYBOARD, so Ctrl+C is dead (confirmed). A focused Fl_Output would draw an
        // insertion caret, which is meaningless on a read-only field -- hide it by matching the cursor
        // colour to the field ground (the selection highlight, which IS useful, still shows).
        m_cmykField->cursor_color(toFl(pal.controlBg));
        auto* browse = new FlatButton(kInnerX, 182, 92, 28, _("Browse..."));
        browse->callback([](Fl_Widget*, void* v) { static_cast<SettingsDialog*>(v)->browseCmyk(); },
                         this);
        auto* clear = new FlatButton(kInnerX + 100, 182, 92, 28, _("Use default"));
        clear->callback([](Fl_Widget*, void* v) { static_cast<SettingsDialog*>(v)->clearCmyk(); },
                        this);
        pane->end();
        m_panes.push_back(pane);
    }

    // Rendering (S60-b item 14, docs/s60-performance-plan.md §6) ------------------------------
    // One setting, and it stays one setting. Per [[mosaic-no-toggle-for-strictly-better]] there is
    // no toggle for individual GPU tiers: if a capability is present and faster, Mosaic uses it and
    // does not ask. This is the exception that rule allows for, because it is not a quality dial --
    // on a driver that hangs or corrupts, the CPU lanes are the only way to get the app to run at
    // all, and nothing can tell "this driver is wrong" from "this driver is slow" by probing it.
    //
    // §6.3's read-only capability readout (device, Vulkan version, which tier fired, which lane is
    // serving what) belongs on this pane and is S60-f's; it is deliberately not stubbed here.
    {
        auto* pane = new Fl_Group(kContentX, kContentY, kContentW, kContentH);
        pane->box(FL_NO_BOX);
        pane->begin();
        sectionTitle(kInnerX, 18, _("Rendering"));
        caption(kInnerX, 52, kInnerW, 56,
                _("Mosaic uses your graphics card for the heavy image work -- compositing, blurs, "
                  "texture generation, 3D text -- and falls back to the processor on its own "
                  "whenever the card cannot do a particular job."));
        const int ry = 52 + 56 + 10;
        m_renderingCpuOnly = new CheckBox(
            kInnerX, ry, kInnerW, 22, _("Do all image processing on the processor"),
            [this](bool on) {
                if (m_host.setRenderingMode)
                    m_host.setRenderingMode(on ? "cpu-only" : "auto");
            });
        caption(kInnerX, ry + 22 + kCaptionGap, kInnerW, 92,
                _("Turn this on if your graphics driver misbehaves -- wrong colours, freezes, "
                  "crashes. Mosaic then builds nothing on the card and uses the processor paths "
                  "instead: the same picture, produced more slowly. The window itself is still "
                  "drawn by the card either way.\n"
                  "Takes effect the next time you start Mosaic. Starting with --cpu does the same "
                  "thing for one run without changing this setting."));
        pane->end();
        m_panes.push_back(pane);
    }

    // Inpainting: an Engine tab (backend selector + spec sheet) over a Backend Settings tab (the
    // chosen backend's quality preset + tunable controls, generated from its schema). Cohesive with
    // the Tools pane's sub-tab shape; both tabs are rebuilt from data when the backend changes.
    {
        constexpr int kInpTop = 86; // y of the first row inside a sub-pane (below title + tabs)
        auto* pane = new Fl_Group(kContentX, kContentY, kContentW, kContentH);
        pane->box(FL_NO_BOX);
        pane->begin();
        m_inpaintPane = pane;
        sectionTitle(kInnerX, 18, _("Inpainting"));
        auto* tabs = new SubTabBar(kInnerX, 52, kInnerW, 26, {_("Engine"), _("Backend settings")},
                                   [this](int i) { selectInpaintTab(i); });
        m_inpaintTabs = tabs;

        // -- Engine sub-pane: backend selector + spec sheet ---------------------------------
        {
            auto* sp = new Fl_Group(kContentX, kInpTop, kContentW, kContentH - kInpTop);
            sp->box(FL_NO_BOX);
            sp->begin();
            fieldLabel(kInnerX, kInpTop, kLabelW, _("Backend"));
            m_inpaintBackendChoice =
                new Dropdown(kInnerX + kLabelW, kInpTop, kInnerW - kLabelW, kRowH);
            for (const auto& d : m_host.inpaintBackends)
                m_inpaintBackendChoice->add(d.info.displayName.c_str());
            m_inpaintBackendChoice->callback(
                [](Fl_Widget* w, void* v) {
                    static_cast<SettingsDialog*>(v)->selectInpaintBackend(
                        static_cast<Dropdown*>(w)->value(), /*fromSeed=*/false);
                },
                this);
            const int specY = kInpTop + kRowH + 14;
            auto* scroll = new ScrollView(kContentX, specY, kContentW, kContentH - specY);
            scroll->type(Fl_Scroll::VERTICAL);
            scroll->box(FL_NO_BOX);
            scroll->color(FL_BACKGROUND_COLOR); // = windowBg; the themed trough blends in
            scroll->begin();
            m_inpaintSpec = new SpecPanel(kInnerX, specY, kInnerW - 6, kContentH - specY);
            scroll->end();
            sp->end();
            m_inpaintTabPanes.push_back(sp);
        }
        // -- Backend Settings sub-pane: dynamic, rebuilt per backend ------------------------
        {
            auto* scroll = new ScrollView(kContentX, kInpTop, kContentW, kContentH - kInpTop);
            scroll->type(Fl_Scroll::VERTICAL);
            scroll->box(FL_NO_BOX);
            scroll->color(FL_BACKGROUND_COLOR);
            scroll->end();
            m_inpaintSettingsScroll = scroll;
            m_inpaintTabPanes.push_back(scroll);
        }
        pane->end();
        m_panes.push_back(pane);

        // Initial population (seed() re-selects with the persisted backend + preset before show()).
        if (!m_host.inpaintBackends.empty())
            selectInpaintBackend(0, /*fromSeed=*/true);
        selectInpaintTab(0);
    }

    // Text ----------------------------------------------------------------------------------
    // The Type tool's language features (deferred §2): live spell-checking, and the default text
    // language that feeds both spell-check and hyphenation. (Custom-dictionary management + the R5
    // emoji-font picker will join this pane later.)
    {
        auto* pane = new Fl_Group(kContentX, kContentY, kContentW, kContentH);
        pane->box(FL_NO_BOX);
        pane->begin();
        sectionTitle(kInnerX, 18, _("Text"));
        m_spellCheck = new CheckBox(kInnerX, 60, kInnerW, 22, _("Check spelling"), [this](bool on) {
            if (m_host.setSpellCheck)
                m_host.setSpellCheck(on);
        });
        caption(kInnerX, 60 + 22 + kCaptionGap, kInnerW, 40,
                _("Underline misspelled words with a red wavy line while you edit text. The underline "
                  "is only drawn on-canvas -- never part of the image or an export."));
        m_spellCheckAllCaps =
            new CheckBox(kInnerX, 148, kInnerW, 22, _("Check ALL-CAPS words"), [this](bool on) {
                if (m_host.setSpellCheckAllCaps)
                    m_host.setSpellCheckAllCaps(on);
            });
        caption(kInnerX, 148 + 22 + kCaptionGap, kInnerW, 34,
                _("Off by default, so acronyms like NASA or HTTP are not flagged."));
        fieldLabel(kInnerX, 216, kLabelW, _("Language"));
        m_textLanguage = new Dropdown(kInnerX + kLabelW, 216, 200, kRowH);
        for (const TextLangItem& li : kTextLanguages)
            m_textLanguage->add(li.label);
        m_textLanguage->callback(
            [](Fl_Widget*, void* v) { static_cast<SettingsDialog*>(v)->onTextLanguageChanged(); },
            this);
        caption(kInnerX, 216 + kRowH + 8, kInnerW, 44,
                _("The default language for spell-check and hyphenation, used where a paragraph has no "
                  "language of its own. \"Follow system\" uses your OS locale."));
        // Emoji font (Type R5): which installed colour-emoji family renders emoji in text. Index 0 =
        // Automatic (the OS font cascade picks); the rest mirror FontDB::emojiFamilies() in order.
        {
            const int ey = 216 + kRowH + 8 + 44 + 12;
            fieldLabel(kInnerX, ey, kLabelW, _("Emoji font"));
            m_emojiFont = new Dropdown(kInnerX + kLabelW, ey, 200, kRowH);
            m_emojiFont->add(_("Automatic"));
            for (const std::string& fam : m_host.emojiFamilies)
                m_emojiFont->add(fam.c_str());
            m_emojiFont->callback(
                [](Fl_Widget*, void* v) { static_cast<SettingsDialog*>(v)->onEmojiFontChanged(); },
                this);
            caption(kInnerX, ey + kRowH + 8, kInnerW, 44,
                    _("The color font used when text needs an emoji glyph. An emoji the chosen "
                      "family lacks still falls back automatically, so nothing turns into boxes."));
        }
        pane->end();
        m_panes.push_back(pane);
    }

    // Keybindings ---------------------------------------------------------------------------
    buildKeybindingsPane();

    // Annoyances ----------------------------------------------------------------------------
    // The fabled category, finally inhabited: a home for things sure to bug someone eventually (the
    // "unsaved since…" nag and friends will live here too). The name + contents explain themselves,
    // so the pane needs no preamble -- just the settings, each with its own (here, unserious) note.
    {
        auto* pane = new Fl_Group(kContentX, kContentY, kContentW, kContentH);
        pane->box(FL_NO_BOX);
        pane->begin();
        sectionTitle(kInnerX, 18, _("Annoyances"));
        int uy = 64;
        // The one-liners ride in the menu bar's empty right region, which macOS does not have --
        // the menus are in the system menu bar at the top of the screen there (S58-b). So the row is
        // not built on macOS rather than left as a toggle that cannot do anything; everything
        // below simply starts where it would have.
#ifndef __APPLE__
        m_motivationalLines = new CheckBox(
            kInnerX, uy, kInnerW, 22, _("Cheesy motivational one-liners"), [this](bool on) {
                if (m_host.setMotivationalLines)
                    m_host.setMotivationalLines(on);
            });
        caption(kInnerX, uy + 22 + kCaptionGap, kInnerW, 64,
                _("Mosaic believes in you. With this on, a line of all-caps encouragement slides "
                  "down into the empty right end of the menu bar every few minutes, holds there for "
                  "a breath, then slips back out of sight. Off by default, because some hearts are "
                  "busy."));
        uy += 22 + kCaptionGap + 64 + 14;
#endif

        // The unsaved-state window title (S18-d). A "• unsaved" always shows; these two control the
        // running duration that follows it.
        m_showUnsavedDuration = new CheckBox(
            kInnerX, uy, kInnerW, 22,
            _("Show how long the document has been unsaved in the title"), [this](bool on) {
                if (m_host.setShowUnsavedDuration)
                    m_host.setShowUnsavedDuration(on);
            });
        caption(kInnerX, uy + 22 + kCaptionGap, kInnerW, 44,
                _("After a few unsaved minutes the title starts counting -- a quiet nudge toward "
                  "Ctrl+S. The bare \"unsaved\" mark shows either way; this just adds the tally."));
        uy += 22 + kCaptionGap + 44 + 14;
        m_unsavedIncludeSeconds = new CheckBox(
            kInnerX + 24, uy, kInnerW - 24, 22, _("Include the seconds"), [this](bool on) {
                if (m_host.setUnsavedIncludeSeconds)
                    m_host.setUnsavedIncludeSeconds(on);
            });
        caption(kInnerX + 24, uy + 22 + kCaptionGap, kInnerW - 24, 44,
                _("Tick the tally every second instead of every minute. Off by default: a title "
                  "that re-renders each second is motion in the corner of the eye, and some screen "
                  "readers read it aloud each time."));
        pane->end();
        m_panes.push_back(pane);
    }

    // ---- footer: Done (right-anchored) ---------------------------------------------------
    auto* footer = new Panel(0, kContentH, kWinW, kFooterH);
    m_footer = footer;
    footer->color(toFl(pal.windowBg));
    footer->borderEdges(Panel::EdgeTop);
    footer->end();
    auto* done = new FlatButton(kWinW - kMargin - 90, kContentH + (kFooterH - 28) / 2, 90, 28,
                                _("Done"));
    done->callback([](Fl_Widget* w, void*) { w->window()->hide(); });
    footer->add(done);

    // This (modal) dialog hosts its own Dropdowns, so it needs its own themed list sub-window --
    // built here, before show(), so FLTK realizes it as a real sub-surface of the dialog.
    (new DropdownPopup())->hide();
    // ...and its own themed text-field right-click menu (the CMYK output), on the same rules.
    (new ContextMenu())->hide();

    end();
    size_range(kWinW, kWinH, kWinW, kWinH); // fixed-layout dialog: no WM resize for now
    set_modal(); // creative-app convention (Affinity/Photoshop/Krita): block the app, no taskbar entry

    // Escape / window-close closes the dialog; MainWindow reuses the (hidden) instance on re-open.
    callback([](Fl_Widget* w, void*) { w->hide(); });

    selectSection(0);
}

SettingsDialog::~SettingsDialog() {
    Fl::remove_timeout(lineCardTick, this);
    Fl::remove_timeout(tabletTick, this); // never fire a readout poll at a dead dialog
}

void SettingsDialog::show() {
    Fl_Double_Window::show();
    Fl::remove_timeout(lineCardTick, this); // never double-arm on a re-show
    Fl::add_timeout(kLineCardTickS, lineCardTick, this);
    // Ask the tablet backend to read the pen over THIS window too -- tablet event delivery is
    // per-window, so until it does, the test area cannot see a pen hovering the dialog it is in
    // (docs/tablet.md §8). AFTER Fl_Double_Window::show(): the window has no native handle until it
    // is mapped, and a re-shown dialog gets a NEW one.
    if (m_host.tabletWatchWindow)
        m_host.tabletWatchWindow(this);
    armTabletTick();
}

void SettingsDialog::hide() {
    endKeyCapture(); // a hidden dialog must not come back still listening for a chord
    Fl::remove_timeout(lineCardTick, this);
    m_tabletTicking = false;
    Fl::remove_timeout(tabletTick, this); // a hidden dialog polls nothing
    // BEFORE Fl_Double_Window::hide(), which destroys the native window: a backend still holding it
    // would name a dead window on its next device re-enumeration (on X11 that is a BadWindow, and
    // Xlib's default error handler ends the process).
    if (m_host.tabletUnwatchWindow)
        m_host.tabletUnwatchWindow(this);
    Fl_Double_Window::hide();
}

// The Appearance line-style cards' animation: drift the shared background phase and repaint the
// three cards, but only while their pane is the visible one -- parked on any other pane the tick
// idles at a slow poll. hide() removes the timeout; the shown() check backstops a queued tick
// racing the removal.
void SettingsDialog::lineCardTick(void* v) {
    auto* dlg = static_cast<SettingsDialog*>(v);
    if (!dlg->shown())
        return;
    const bool visible =
        dlg->m_section == 1 && dlg->m_appearanceTab == 0 && !dlg->m_lineStyleCards.empty();
    if (visible) {
        dlg->m_linePhase += kLineCardDriftPxPerS * kLineCardTickS;
        if (dlg->m_linePhase > 1e6) // wrap far beyond any card width: keeps the float exact
            dlg->m_linePhase = 0.0;
        for (Fl_Widget* card : dlg->m_lineStyleCards)
            card->redraw();
    }
    Fl::add_timeout(visible ? kLineCardTickS : kLineCardIdleS, lineCardTick, dlg);
}

int SettingsDialog::handle(int event) {
    // Keybindings capture (S51-b): while a row is listening, this window owns every keystroke.
    // beginKeyCapture parked focus here, so FL_KEYBOARD arrives before the search field sees it, and
    // returning 1 unconditionally means no accelerator, mnemonic or Tab navigation can fire from the
    // chord the user is trying to ASSIGN.
    if (m_keyCapture >= 0 && (event == FL_KEYBOARD || event == FL_SHORTCUT)) {
        switch (Fl::event_key()) {
        case FL_Escape:
            endKeyCapture(); // the documented way out; also why Escape is never capturable
            return 1;
        case FL_Shift_L:
        case FL_Shift_R:
        case FL_Control_L:
        case FL_Control_R:
        case FL_Alt_L:
        case FL_Alt_R:
        case FL_Meta_L:
        case FL_Meta_R:
        case FL_Caps_Lock:
        case FL_Num_Lock:
        case FL_Scroll_Lock:
            return 1; // a modifier on its own is not a chord: keep listening
        default:
            break;
        }
        captureFromEvent();
        return 1;
    }
    if (event == FL_PUSH) {
        dismissActiveDropdownPopupOnOutsideClick(Fl::event_x(), Fl::event_y());
        dismissActiveContextMenuOnOutsideClick(Fl::event_x(), Fl::event_y());
        // Clicking anywhere abandons a pending capture. A click that lands on another ROW then
        // re-arms it from the base dispatch below, which is exactly "move the capture".
        endKeyCapture();
    }
    // Escape closes an open Dropdown list / context menu first (not the whole dialog); only an Escape
    // with neither open falls through to the window callback that hides the dialog.
    if ((event == FL_KEYBOARD || event == FL_SHORTCUT) && Fl::event_key() == FL_Escape) {
        if (activeContextMenu() != nullptr) {
            dismissActiveContextMenu();
            return 1;
        }
        if (activeDropdownPopup() != nullptr) {
            dismissActiveDropdownPopup();
            return 1;
        }
    }
    return Fl_Double_Window::handle(event);
}

void SettingsDialog::selectSection(int index) {
    m_section = index;
    for (int i = 0; i < static_cast<int>(m_panes.size()); ++i) {
        static_cast<NavItem*>(m_nav[static_cast<std::size_t>(i)])->setActive(i == index);
        if (i == index)
            m_panes[static_cast<std::size_t>(i)]->show();
        else
            m_panes[static_cast<std::size_t>(i)]->hide();
    }
    armTabletTick(); // the test area only polls while it is the page you are looking at
}

// --- Settings -> Tablet (docs/tablet.md §8) ---------------------------------------------------

void SettingsDialog::onTabletRange() {
    if (m_tabletPressureMin == nullptr || m_tabletPressureMax == nullptr ||
        !m_host.setTabletPressureRange)
        return;
    // Handed over as the user set them, inverted pairs and all: TabletPolicy::setPressureRange
    // clamps and swaps, and an equal pair is a threshold rather than a division by a zero span. The
    // dialog does not get to have its own opinion about a value the ingest path already defines.
    m_host.setTabletPressureRange(static_cast<Fl_Valuator*>(m_tabletPressureMin)->value(),
                                  static_cast<Fl_Valuator*>(m_tabletPressureMax)->value());
}

void SettingsDialog::onTabletSpeed() {
    if (m_tabletSpeedMax == nullptr || m_tabletSpeedWindow == nullptr || !m_host.setTabletSpeed)
        return;
    m_host.setTabletSpeed(static_cast<Fl_Valuator*>(m_tabletSpeedMax)->value(),
                          static_cast<Fl_Valuator*>(m_tabletSpeedWindow)->value());
}


void SettingsDialog::tabletTick(void* v) {
    auto* d = static_cast<SettingsDialog*>(v);
    d->refreshTabletTest();
    if (d->m_tabletTicking)
        Fl::repeat_timeout(kTabletTickS, tabletTick, v);
}

void SettingsDialog::armTabletTick() {
    // The Tablet pane is the one at kTabletSection (the rail is positional -- see the constructor).
    const bool want = shown() != 0 && m_section == kTabletSection && m_tabletTestArea != nullptr;
    if (want == m_tabletTicking)
        return;
    m_tabletTicking = want;
    Fl::remove_timeout(tabletTick, this);
    if (want) {
        refreshTabletTest(); // show the current state at once, not one tick from now
        Fl::add_timeout(kTabletTickS, tabletTick, this);
    }
}

void SettingsDialog::selectTabletSectionForTest() { selectSection(kTabletSection); }

void SettingsDialog::refreshTabletTest() {
    if (m_tabletTestArea == nullptr)
        return;
    static_cast<TabletTestArea*>(m_tabletTestArea)
        ->setReading(m_host.tabletReading ? m_host.tabletReading() : SettingsHost::TabletReading{});
}

void SettingsDialog::selectToolTab(int index) {
    m_toolTab = index;
    if (m_toolTabs != nullptr)
        static_cast<SubTabBar*>(m_toolTabs)->setActive(index);
    for (int i = 0; i < static_cast<int>(m_toolPanes.size()); ++i) {
        if (i == index)
            m_toolPanes[static_cast<std::size_t>(i)]->show();
        else
            m_toolPanes[static_cast<std::size_t>(i)]->hide();
    }
}

void SettingsDialog::selectAppearanceTab(int index) {
    m_appearanceTab = index;
    if (m_appearanceTabs != nullptr)
        static_cast<SubTabBar*>(m_appearanceTabs)->setActive(index);
    for (int i = 0; i < static_cast<int>(m_appearancePanes.size()); ++i) {
        if (i == index)
            m_appearancePanes[static_cast<std::size_t>(i)]->show();
        else
            m_appearancePanes[static_cast<std::size_t>(i)]->hide();
    }
}

void SettingsDialog::selectIconPack(int index, bool fromUser) {
    if (index < 0 || index >= static_cast<int>(m_host.iconPacks.size()))
        return;
    selectCardAt(m_iconPackCards, index);
    const SettingsHost::IconPackDesc& desc = m_host.iconPacks[static_cast<std::size_t>(index)];
    if (m_iconPackDetail != nullptr)
        static_cast<IconPackDetailPanel*>(m_iconPackDetail)
            ->setPack(desc, &m_iconPreviews->get(m_host, desc.id, IconPackDetailPanel::kIconPx));
    if (fromUser && m_host.setIconPack)
        m_host.setIconPack(desc.id);
}

// ---- Inpainting pane -------------------------------------------------------------------------

const SettingsHost::InpaintBackendDesc* SettingsDialog::currentInpaintBackend() const {
    if (m_inpaintDescIdx < 0 || m_inpaintDescIdx >= static_cast<int>(m_host.inpaintBackends.size()))
        return nullptr;
    return &m_host.inpaintBackends[static_cast<std::size_t>(m_inpaintDescIdx)];
}

void SettingsDialog::selectInpaintTab(int index) {
    m_inpaintTab = index;
    if (m_inpaintTabs != nullptr)
        static_cast<SubTabBar*>(m_inpaintTabs)->setActive(index);
    for (int i = 0; i < static_cast<int>(m_inpaintTabPanes.size()); ++i) {
        if (i == index)
            m_inpaintTabPanes[static_cast<std::size_t>(i)]->show();
        else
            m_inpaintTabPanes[static_cast<std::size_t>(i)]->hide();
    }
}

void SettingsDialog::selectInpaintBackend(int comboIndex, bool fromSeed) {
    if (comboIndex < 0 || comboIndex >= static_cast<int>(m_host.inpaintBackends.size()))
        return;
    m_inpaintDescIdx = comboIndex;
    if (m_inpaintBackendChoice != nullptr)
        m_inpaintBackendChoice->value(comboIndex);
    const auto& d = m_host.inpaintBackends[static_cast<std::size_t>(comboIndex)];
    if (m_inpaintSpec != nullptr) {
        static_cast<SpecPanel*>(m_inpaintSpec)->setInfo(d.info);
        // Full reset (not just scroll_to) so the new description starts at the top-left, not where the
        // previous backend's left off — see resetScrollToTop.
        resetScrollToTop(static_cast<Fl_Scroll*>(m_inpaintSpec->parent()));
    }

    if (fromSeed) {
        // Honour the persisted preset / custom overrides; fall back to the default only if the saved
        // preset isn't one this backend offers (and isn't the "custom" sentinel).
        if (m_inpaintPresetId != "custom" && !presetExists(d.schema, m_inpaintPresetId))
            m_inpaintPresetId = d.schema.defaultPreset;
    } else {
        // A live switch: the host resets the new backend to its default preset (no overrides) and
        // persists; mirror that here so the controls position to the default.
        if (m_host.setInpaintBackend)
            m_host.setInpaintBackend(d.id);
        m_inpaintPresetId = d.schema.defaultPreset;
        m_inpaintOverrides.clear();
    }
    rebuildInpaintBackendSettings(); // builds the controls and positions them to the current state
}

void SettingsDialog::rebuildInpaintBackendSettings() {
    if (m_inpaintSettingsScroll == nullptr)
        return;
    Fl_Group* const prevGroup = Fl_Group::current(); // restore so this is safe mid-construction too

    // Tear down the previous backend's widgets (never the ScrollView's own scrollbars). Immediate
    // delete, not Fl::delete_widget: rebuild only runs from the backend selector's callback or from
    // seed() — never from inside one of these widgets' own callbacks — so deleting now is safe, and
    // it avoids the deferred-delete leaving stale controls drawn over the new ones for a frame on
    // re-open (which read as controls shifted/overlapping).
    for (auto* w : m_inpaintDynamic) {
        m_inpaintSettingsScroll->remove(w);
        delete w; // NOLINT(cppcoreguidelines-owning-memory)
    }
    m_inpaintDynamic.clear();
    m_inpaintCtrlSpecs.clear();
    m_inpaintCtrlWidgets.clear();
    m_inpaintCtrlReadouts.clear();
    m_inpaintPresetChoice = nullptr;

    const auto* d = currentInpaintBackend();
    if (d == nullptr) {
        Fl_Group::current(prevGroup);
        return;
    }

    // Reset the scroll to the top-left BEFORE creating the new controls. Fl_Scroll positions its
    // children relative to its scroll offset, so building them while it is still scrolled (from the
    // previous backend) would offset the fresh controls. With the offset cleared first, each
    // control's absolute coordinate maps straight to its content position.
    resetScrollToTop(m_inpaintSettingsScroll);

    const int cw = kInnerW - 6; // leave room for the vertical scrollbar
    int y = 86;                  // top of the sub-pane (matches the ScrollView's y)
    m_inpaintSettingsScroll->begin();

    // Name the backend these settings belong to (the selector is on the other tab).
    auto* title = fieldLabel(kInnerX, y, cw, d->info.displayName.c_str());
    title->labelfont(FL_HELVETICA_BOLD);
    title->labelsize(14);
    m_inpaintDynamic.push_back(title);
    y += kRowH + 6;

    if (d->schema.controls.empty()) {
        auto* cap = caption(kInnerX, y, cw, 40,
                            _("This backend has no adjustable settings — it is configured "
                              "elsewhere (a script provides it)."));
        m_inpaintDynamic.push_back(cap);
        m_inpaintSettingsScroll->end();
        Fl_Group::current(prevGroup);
        redraw();
        return;
    }

    // Quality preset row (only if the backend defines presets).
    if (!d->schema.presets.empty()) {
        auto* lbl = fieldLabel(kInnerX, y, kLabelW, _("Quality"));
        m_inpaintDynamic.push_back(lbl);
        auto* choice = new Dropdown(kInnerX + kLabelW, y, cw - kLabelW, kRowH);
        for (const auto& p : d->schema.presets)
            choice->add(p.label.c_str());
        choice->callback(
            [](Fl_Widget* w, void* v) {
                static_cast<SettingsDialog*>(v)->onInpaintPresetChosen(
                    static_cast<Dropdown*>(w)->value());
            },
            this);
        m_inpaintPresetChoice = choice;
        m_inpaintDynamic.push_back(choice);
        y += kRowH + kSettingGap;
    }

    // Controls: the up-front ones first, then an "Advanced" subheading + the advanced ones.
    bool advancedHeaderDone = false;
    for (int phase = 0; phase < 2; ++phase) {
        const bool advanced = phase == 1;
        for (const auto& c : d->schema.controls) {
            if (c.advanced != advanced)
                continue;
            if (advanced && !advancedHeaderDone) {
                auto* hdr = fieldLabel(kInnerX, y, cw, _("Advanced"));
                hdr->labelfont(FL_HELVETICA_BOLD);
                m_inpaintDynamic.push_back(hdr);
                y += kRowH;
                advancedHeaderDone = true;
            }
            addInpaintControl(c, y);
        }
    }

    y += 6;
    auto* reset = new FlatButton(kInnerX, y, 132, 28, _("Reset to defaults"));
    reset->callback(
        [](Fl_Widget*, void* v) { static_cast<SettingsDialog*>(v)->resetInpaintParams(); }, this);
    m_inpaintDynamic.push_back(reset);
    y += 28;
    m_inpaintDynamic.push_back(new Fl_Box(kInnerX, y, 1, kMargin)); // bottom breathing room

    m_inpaintSettingsScroll->end();
    positionInpaintControls();              // reflect the active preset / custom values
    resetScrollToTop(m_inpaintSettingsScroll); // back to top-left (clears stale scrollbar offsets)
    Fl_Group::current(prevGroup);
    redraw();
}

void SettingsDialog::addInpaintControl(const core::inpaint::ParamControl& c, int& y) {
    using Kind = core::inpaint::ParamControl::Kind;
    const int cw = kInnerW - 6;
    Fl_Widget* primary = nullptr;
    Fl_Widget* readout = nullptr;

    if (c.kind == Kind::Bool) {
        auto* cb = new CheckBox(kInnerX, y, cw, 22, c.label.c_str(), nullptr);
        // Route through the uniform handler (so "Custom" + the host push happen), by widget identity.
        cb->setOnToggle([this, cb](bool) { onInpaintControlChanged(cb); });
        m_inpaintDynamic.push_back(cb);
        primary = cb;
        y += 22;
    } else if (c.kind == Kind::Choice) {
        auto* lbl = fieldLabel(kInnerX, y, cw, c.label.c_str());
        m_inpaintDynamic.push_back(lbl);
        y += kRowH;
        auto* dd = new Dropdown(kInnerX, y, cw, kRowH);
        for (const auto& opt : c.choices)
            dd->add(opt.c_str());
        dd->callback(
            [](Fl_Widget* w, void* v) { static_cast<SettingsDialog*>(v)->onInpaintControlChanged(w); },
            this);
        m_inpaintDynamic.push_back(dd);
        primary = dd;
        y += kRowH;
    } else { // Int / Real: label, then a slider + a numeric readout
        auto* lbl = fieldLabel(kInnerX, y, cw, c.label.c_str());
        m_inpaintDynamic.push_back(lbl);
        y += kRowH;
        constexpr int kReadW = 64;
        auto* sl = new Slider(kInnerX, y, cw - kReadW - 8, 20);
        sl->setCellColor(activePalette().windowBg); // blend into the pane (not the slider's panelBg default)
        sl->range(c.min, c.max);
        if (c.step > 0)
            sl->step(c.step);
        sl->when(FL_WHEN_CHANGED);
        sl->callback(
            [](Fl_Widget* w, void* v) { static_cast<SettingsDialog*>(v)->onInpaintControlChanged(w); },
            this);
        m_inpaintDynamic.push_back(sl);
        auto* ro = new Fl_Box(kInnerX + cw - kReadW, y, kReadW, 20);
        ro->box(FL_FLAT_BOX);            // erase its own area (double-buffered window; see OptionCard)
        ro->color(FL_BACKGROUND_COLOR);  // = windowBg
        ro->labelfont(FL_HELVETICA);
        ro->labelsize(12);
        ro->labelcolor(FL_FOREGROUND_COLOR);
        ro->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        m_inpaintDynamic.push_back(ro);
        primary = sl;
        readout = ro;
        y += 20;
    }

    if (!c.help.empty()) {
        constexpr int kCapH = 34;
        auto* cap = caption(kInnerX, y, cw, kCapH, c.help.c_str());
        m_inpaintDynamic.push_back(cap);
        y += kCapH;
    }
    y += 12; // inter-control gap

    m_inpaintCtrlSpecs.push_back(c);
    m_inpaintCtrlWidgets.push_back(primary);
    m_inpaintCtrlReadouts.push_back(readout);
}

void SettingsDialog::onInpaintPresetChosen(int presetIndex) {
    const auto* d = currentInpaintBackend();
    if (d == nullptr || presetIndex < 0 ||
        presetIndex >= static_cast<int>(d->schema.presets.size()))
        return;
    applyInpaintPreset(d->schema.presets[static_cast<std::size_t>(presetIndex)].id,
                       /*fromUser=*/true);
}

void SettingsDialog::onInpaintControlChanged(Fl_Widget* control) {
    int index = -1;
    for (int i = 0; i < static_cast<int>(m_inpaintCtrlWidgets.size()); ++i)
        if (m_inpaintCtrlWidgets[static_cast<std::size_t>(i)] == control) {
            index = i;
            break;
        }
    if (index < 0)
        return;
    const auto& c = m_inpaintCtrlSpecs[static_cast<std::size_t>(index)];
    const double v = readInpaintControlValue(index);
    updateInpaintReadout(index, v);
    // The values now diverge from any named preset: become "custom" and remember the override.
    m_inpaintPresetId = "custom";
    m_inpaintOverrides[c.key] = v;
    if (m_inpaintPresetChoice != nullptr)
        static_cast<Dropdown*>(m_inpaintPresetChoice)->setOverrideText(_("Custom"));
    if (m_host.setInpaintParam)
        m_host.setInpaintParam(c.key, v);
}

// Set every control to the values implied by the active state (a named preset, or the custom
// overrides) and reflect that state in the preset chip.
void SettingsDialog::positionInpaintControls() {
    const auto* d = currentInpaintBackend();
    if (d == nullptr)
        return;
    const bool custom = m_inpaintPresetId == "custom";

    if (m_inpaintPresetChoice != nullptr) {
        auto* dd = static_cast<Dropdown*>(m_inpaintPresetChoice);
        if (custom) {
            dd->setOverrideText(_("Custom"));
        } else {
            dd->setOverrideText("");
            for (int i = 0; i < static_cast<int>(d->schema.presets.size()); ++i)
                if (d->schema.presets[static_cast<std::size_t>(i)].id == m_inpaintPresetId) {
                    dd->value(i);
                    break;
                }
        }
    }

    const core::inpaint::PresetSpec* preset = nullptr;
    if (!custom)
        for (const auto& p : d->schema.presets)
            if (p.id == m_inpaintPresetId) {
                preset = &p;
                break;
            }
    for (int i = 0; i < static_cast<int>(m_inpaintCtrlSpecs.size()); ++i) {
        const auto& c = m_inpaintCtrlSpecs[static_cast<std::size_t>(i)];
        double v = c.defaultValue;
        if (custom) {
            const auto it = m_inpaintOverrides.find(c.key);
            if (it != m_inpaintOverrides.end())
                v = it->second;
        } else if (preset != nullptr) {
            for (const auto& [key, presetValue] : preset->values)
                if (key == c.key) {
                    v = presetValue;
                    break;
                }
        }
        setInpaintControlValue(i, v);
    }
}

void SettingsDialog::applyInpaintPreset(const std::string& presetId, bool fromUser) {
    m_inpaintPresetId = presetId;
    m_inpaintOverrides.clear(); // a named preset (or a reset) replaces any hand-tuned values
    positionInpaintControls();
    if (fromUser && m_host.setInpaintPreset)
        m_host.setInpaintPreset(presetId);
}

void SettingsDialog::resetInpaintParams() {
    // Back to the backend's default preset (a preset-less backend has "" => its control defaults).
    const auto* d = currentInpaintBackend();
    if (d != nullptr)
        applyInpaintPreset(d->schema.defaultPreset, /*fromUser=*/true);
}

void SettingsDialog::setInpaintControlValue(int index, double value) {
    if (index < 0 || index >= static_cast<int>(m_inpaintCtrlSpecs.size()))
        return;
    using Kind = core::inpaint::ParamControl::Kind;
    const auto& c = m_inpaintCtrlSpecs[static_cast<std::size_t>(index)];
    Fl_Widget* w = m_inpaintCtrlWidgets[static_cast<std::size_t>(index)];
    if (w == nullptr)
        return;
    if (c.kind == Kind::Bool)
        static_cast<CheckBox*>(w)->setChecked(value != 0.0);
    else if (c.kind == Kind::Choice)
        static_cast<Dropdown*>(w)->value(static_cast<int>(std::lround(value)));
    else
        static_cast<Slider*>(w)->value(value);
    updateInpaintReadout(index, value);
}

double SettingsDialog::readInpaintControlValue(int index) const {
    if (index < 0 || index >= static_cast<int>(m_inpaintCtrlSpecs.size()))
        return 0.0;
    using Kind = core::inpaint::ParamControl::Kind;
    const auto& c = m_inpaintCtrlSpecs[static_cast<std::size_t>(index)];
    Fl_Widget* w = m_inpaintCtrlWidgets[static_cast<std::size_t>(index)];
    if (w == nullptr)
        return c.defaultValue;
    if (c.kind == Kind::Bool)
        return static_cast<CheckBox*>(w)->checked() ? 1.0 : 0.0;
    if (c.kind == Kind::Choice)
        return static_cast<Dropdown*>(w)->value();
    return static_cast<Slider*>(w)->value();
}

void SettingsDialog::updateInpaintReadout(int index, double value) {
    if (index < 0 || index >= static_cast<int>(m_inpaintCtrlReadouts.size()))
        return;
    Fl_Widget* ro = m_inpaintCtrlReadouts[static_cast<std::size_t>(index)];
    if (ro == nullptr)
        return;
    ro->copy_label(formatParamValue(m_inpaintCtrlSpecs[static_cast<std::size_t>(index)], value).c_str());
}

void SettingsDialog::setInpaintEngineBusy(bool busy) {
    if (m_inpaintPane == nullptr)
        return;
    if (busy)
        m_inpaintPane->deactivate();
    else
        m_inpaintPane->activate();
    m_inpaintPane->redraw();
}

void SettingsDialog::seed(const common::Settings& s) {
    if (m_units != nullptr)
        m_units->value(unitsIndex(s.units));
    // Tablet (§8). Programmatic value()/setCurve() do NOT fire the per-control callbacks, so seeding
    // never writes back -- which matters here, because a re-open would otherwise persist a curve the
    // user never touched.
    if (m_tabletCurve != nullptr)
        m_tabletCurve->setCurve(core::brush::Curve::fromString(s.tabletPressureCurve));
    if (m_tabletPressureMin != nullptr)
        static_cast<Fl_Valuator*>(m_tabletPressureMin)->value(s.tabletPressureMin);
    if (m_tabletPressureMax != nullptr)
        static_cast<Fl_Valuator*>(m_tabletPressureMax)->value(s.tabletPressureMax);
    if (m_tabletTiltOffset != nullptr)
        static_cast<Fl_Valuator*>(m_tabletTiltOffset)->value(s.tabletTiltOffsetDegrees);
    if (m_tabletSpeedMax != nullptr)
        static_cast<Fl_Valuator*>(m_tabletSpeedMax)->value(s.tabletSpeedMax);
    if (m_tabletSpeedWindow != nullptr)
        static_cast<Fl_Valuator*>(m_tabletSpeedWindow)->value(s.tabletSpeedWindowMs);
    selectCardAt(m_themeCards, themeModeIndex(parseThemeMode(s.theme).value_or(ThemeMode::Dark)));
    if (!m_lineStyleCards.empty())
        selectCardAt(m_lineStyleCards, lineStyleIndex(s.overlayLineStyle));
    if (!m_featherCards.empty())
        selectCardAt(m_featherCards, featherIndicatorIndex(s.featherIndicator));
    if (!m_iconPackCards.empty()) {
        int packIdx = 0; // an id that no longer resolves seeds as the default pack
        for (int i = 0; i < static_cast<int>(m_host.iconPacks.size()); ++i)
            if (m_host.iconPacks[static_cast<std::size_t>(i)].id == s.iconPack) {
                packIdx = i;
                break;
            }
        selectIconPack(packIdx, /*fromUser=*/false);
    }
    if (!m_cropCards.empty())
        selectCardAt(m_cropCards, cropFramingIndex(s.cropInitialFraming));
    if (!m_multiSelectCards.empty())
        selectCardAt(m_multiSelectCards, multiSelectIndex(s.multiSelectionEdits));
    if (!m_brushDisplayCards.empty()) // anything that is not "grid" is the default, Cards
        selectCardAt(m_brushDisplayCards, s.brushPresetDisplay == "grid" ? 0 : 1);
    if (m_cropSwitchTool != nullptr)
        static_cast<CheckBox*>(m_cropSwitchTool)->setChecked(s.cropSwitchToolAfterApply);
    if (m_cropClearOnLeave != nullptr)
        static_cast<CheckBox*>(m_cropClearOnLeave)->setChecked(s.cropClearSelectionOnLeave);
    if (m_lassoSmooth != nullptr)
        static_cast<CheckBox*>(m_lassoSmooth)->setChecked(s.lassoSmooth);
    if (m_eraserSizeTie != nullptr)
        static_cast<CheckBox*>(m_eraserSizeTie)->setChecked(s.eraserSizeFollowsBrush);
    if (m_eraserPresetTie != nullptr)
        static_cast<CheckBox*>(m_eraserPresetTie)->setChecked(s.eraserPresetFollowsBrush);
    if (m_motivationalLines != nullptr)
        static_cast<CheckBox*>(m_motivationalLines)->setChecked(s.motivationalLines);
    if (m_showUnsavedDuration != nullptr)
        static_cast<CheckBox*>(m_showUnsavedDuration)->setChecked(s.showUnsavedDuration);
    if (m_unsavedIncludeSeconds != nullptr)
        static_cast<CheckBox*>(m_unsavedIncludeSeconds)->setChecked(s.unsavedIncludeSeconds);
    // Rendering: anything that is not "cpu-only" -- including the "" a settings file written before
    // the field existed reads back as -- is the default, "auto".
    if (m_renderingCpuOnly != nullptr)
        static_cast<CheckBox*>(m_renderingCpuOnly)->setChecked(s.renderingMode == "cpu-only");
    if (m_showAllExportFormats != nullptr)
        static_cast<CheckBox*>(m_showAllExportFormats)->setChecked(s.showAllExportFormats);
    if (m_spellCheck != nullptr)
        static_cast<CheckBox*>(m_spellCheck)->setChecked(s.spellCheck);
    if (m_spellCheckAllCaps != nullptr)
        static_cast<CheckBox*>(m_spellCheckAllCaps)->setChecked(s.spellCheckAllCaps);
    if (m_textLanguage != nullptr)
        m_textLanguage->value(textLanguageIndex(s.textLanguage));
    if (m_language != nullptr) {
        // 0 (System default) for "" AND for a saved language this build has no catalog for -- the
        // dropdown must never show a row that is not in it, and "System default" is what the app
        // is actually doing in that case.
        const auto it = std::find(m_languageCodes.begin(), m_languageCodes.end(), s.language);
        m_language->value(
            it == m_languageCodes.end() ? 0 : static_cast<int>(it - m_languageCodes.begin()));
    }
    if (m_emojiFont != nullptr) {
        int idx = 0;  // Automatic for "" or a family no longer installed
        for (int i = 0; i < static_cast<int>(m_host.emojiFamilies.size()); ++i)
            if (s.emojiFont == m_host.emojiFamilies[static_cast<std::size_t>(i)]) {
                idx = i + 1;  // +1: index 0 is Automatic
                break;
            }
        m_emojiFont->value(idx);
    }
    // Inpainting: select the persisted backend (1:1 with the combobox order) and remember the desired
    // preset + any custom overrides so selectInpaintBackend() positions the controls to them.
    if (m_inpaintBackendChoice != nullptr && !m_host.inpaintBackends.empty()) {
        m_inpaintPresetId = s.inpaintPreset;
        m_inpaintOverrides = s.inpaintParams;
        int combo = 0;
        for (int i = 0; i < static_cast<int>(m_host.inpaintBackends.size()); ++i)
            if (m_host.inpaintBackends[static_cast<std::size_t>(i)].id == s.inpaintBackend) {
                combo = i;
                break;
            }
        selectInpaintBackend(combo, /*fromSeed=*/true);
    }
    // Keybindings (S51-b): the rows read the LIVE keymap, not `s` -- the host has already applied
    // whatever was persisted, and the keymap is what the app is actually dispatching. Seeding is
    // therefore a re-read, which also picks up a remap made in a previous session of this dialog.
    refreshKeyRows();
    m_cmyk = s.cmykProfile;
    updateCmykDisplay();
}

void SettingsDialog::reapplyTheme() {
    const Palette& pal = activePalette();
    color(toFl(pal.windowBg)); // the content-pane ground
    if (m_rail != nullptr)
        m_rail->reapplyTheme(); // panelBg rail
    if (m_footer != nullptr) {
        m_footer->color(toFl(pal.windowBg)); // windowBg footer (overrides Panel's default panelBg)
        m_footer->redraw();
    }
    if (m_cmykField != nullptr) {
        m_cmykField->color(toFl(pal.controlBg));
        m_cmykField->textcolor(toFl(pal.text));
        m_cmykField->cursor_color(toFl(pal.controlBg)); // keep the read-only caret hidden after re-theme
    }
    if (m_keySearch != nullptr) { // Keybindings: the filter box bakes its colours, like the CMYK one
        m_keySearch->color(toFl(pal.controlBg));
        m_keySearch->textcolor(toFl(pal.text));
        m_keySearch->cursor_color(toFl(pal.text));
    }
    // Nav rows, captions/labels (semantic colours) and the theme cards draw live -> redraw repaints.
    redraw();
}

// ---- Keybindings (S51-b, docs/keybindings.md) -------------------------------------------------

void SettingsDialog::buildKeybindingsPane() {
    auto* pane = new Fl_Group(kContentX, kContentY, kContentW, kContentH);
    pane->box(FL_NO_BOX);
    pane->begin();
    sectionTitle(kInnerX, 18, _("Keybindings"));
    caption(kInnerX, 46, kInnerW, 48,
            _("Click a shortcut, then press the new keys. Escape cancels. Changes apply at once."));
    auto* search = new TextInput(kInnerX, kKeySearchY, kInnerW - 100 - 8, kRowH);
    search->box(MOSAIC_INPUT_BOX);
    search->color(toFl(activePalette().controlBg));
    search->textcolor(toFl(activePalette().text));
    search->cursor_color(toFl(activePalette().text));
    search->textsize(13);
    search->when(FL_WHEN_CHANGED); // filter as you type; the list only re-stacks, never rebuilds
    search->callback([](Fl_Widget*, void* v) { static_cast<SettingsDialog*>(v)->relayoutKeyRows(); },
                     this);
    // FLTK has no placeholder text; the tooltip carries what the box filters on.
    search->copy_tooltip(_("Filter by command, group or shortcut"));
    m_keySearch = search;
    auto* resetAll = new FlatButton(kInnerX + kInnerW - 100, kKeySearchY, 100, kRowH,
                                    _("Reset all"));
    resetAll->callback(
        [](Fl_Widget*, void* v) {
            auto* dlg = static_cast<SettingsDialog*>(v);
            dlg->endKeyCapture();
            if (dlg->m_host.resetAllKeyChords)
                dlg->m_host.resetAllKeyChords();
            dlg->refreshKeyRows();
        },
        this);

    auto* scroll = new ScrollView(kContentX, kKeyListTop, kContentW, kContentH - kKeyListTop);
    scroll->type(Fl_Scroll::VERTICAL);
    scroll->box(FL_NO_BOX);
    scroll->color(FL_BACKGROUND_COLOR);
    scroll->begin();
    m_keyScroll = scroll;
    // Every row is created ONCE, grouped by category in the keymap's own category order; the search
    // box only hides and re-stacks them (relayoutKeyRows). Rebuilding per keystroke would delete
    // widgets from inside the search field's own callback, and Fl_Scroll already excludes hidden
    // children from its content bounds, so hiding is both cheaper and safer.
    m_keyHeaders.assign(kActionCategoryCount, nullptr);
    int y = kKeyListTop;
    for (int c = 0; c < kActionCategoryCount; ++c) {
        const auto category = static_cast<ActionCategory>(c);
        auto* header = fieldLabel(kInnerX, y, kKeyListW, _(actionCategoryName(category)));
        header->labelfont(FL_HELVETICA_BOLD);
        m_keyHeaders[static_cast<std::size_t>(c)] = header;
        y += kKeyHeaderH;
        if (m_host.keymap == nullptr)
            continue;
        for (const Action& action : m_host.keymap->actions()) {
            if (action.category != category)
                continue;
            const int row = static_cast<int>(m_keyRows.size());
            auto* item = new KeyRow(kInnerX, y, kKeyRowW, kKeyRowH,
                                    [this, row] { beginKeyCapture(row); });
            auto* reset = new FlatButton(kInnerX + kKeyListW - kKeyResetW, y + 2, kKeyResetW,
                                         kKeyRowH - 4, _("Reset"));
            reset->labelsize(11);
            // Located by WIDGET rather than carried in the callback's void* -- the same way
            // onInpaintControlChanged finds which control fired, and the only way to keep `this`.
            reset->callback(
                [](Fl_Widget* w, void* v) {
                    auto* dlg = static_cast<SettingsDialog*>(v);
                    for (std::size_t i = 0; i < dlg->m_keyRowResets.size(); ++i)
                        if (dlg->m_keyRowResets[i] == w) {
                            dlg->resetKeyRow(static_cast<int>(i));
                            return;
                        }
                },
                this);
            reset->hide(); // shown only while the row carries an override (refreshKeyRows)
            m_keyRows.push_back(item);
            m_keyRowResets.push_back(reset);
            m_keyRowIds.push_back(action.id);
            y += kKeyRowH;
        }
    }
    // Bottom breathing room (the inpaint pane's trick), but KEPT: it also caps the scroll's content
    // height, and Fl_Scroll takes that from its children's bounding box -- so a filter that hides
    // most of the list only shrinks the scrollbar if this moves up with the last visible row.
    m_keyBottomPad = new Fl_Box(kInnerX, y, 1, kMargin);
    scroll->end();
    pane->end();
    m_panes.push_back(pane);
    refreshKeyRows();
    relayoutKeyRows();
}

void SettingsDialog::relayoutKeyRows() {
    if (m_keyScroll == nullptr)
        return;
    endKeyCapture(); // a row about to move (or vanish) must not still be listening
    const std::string query =
        lowerAsciiCopy(m_keySearch != nullptr && m_keySearch->value() != nullptr
                           ? m_keySearch->value()
                           : "");
    int y = kKeyListTop;
    for (int c = 0; c < kActionCategoryCount; ++c) {
        const auto category = static_cast<ActionCategory>(c);
        const std::string categoryName = lowerAsciiCopy(_(actionCategoryName(category)));
        Fl_Widget* header = m_keyHeaders[static_cast<std::size_t>(c)];
        const int headerY = y;
        y += kKeyHeaderH; // reserved; rolled back below when the whole group filtered out
        bool anyShown = false;
        for (std::size_t i = 0; i < m_keyRows.size(); ++i) {
            const Action* action =
                m_host.keymap != nullptr ? m_host.keymap->find(m_keyRowIds[i]) : nullptr;
            if (action == nullptr || action->category != category)
                continue;
            // Matched against everything visible on the row plus the group it sits in, so both
            // "brush" and "layer" find what a user would expect, and so does "ctrl+j".
            const bool matched =
                query.empty() || categoryName.find(query) != std::string::npos ||
                lowerAsciiCopy(action->label).find(query) != std::string::npos ||
                lowerAsciiCopy(chordDisplayText(m_host.keymap->chord(action->id)))
                        .find(query) != std::string::npos;
            if (!matched) {
                m_keyRows[i]->hide();
                m_keyRowResets[i]->hide();
                continue;
            }
            m_keyRows[i]->position(kInnerX, y);
            m_keyRows[i]->show();
            m_keyRowResets[i]->position(kInnerX + kKeyListW - kKeyResetW, y + 2);
            if (m_host.keymap->isRemapped(action->id))
                m_keyRowResets[i]->show();
            else
                m_keyRowResets[i]->hide();
            y += kKeyRowH;
            anyShown = true;
        }
        if (header == nullptr)
            continue;
        if (anyShown) {
            header->position(kInnerX, headerY);
            header->show();
        } else {
            header->hide();
            y = headerY; // no group heading with nothing under it
        }
    }
    if (m_keyBottomPad != nullptr)
        m_keyBottomPad->position(kInnerX, y); // the content's new bottom, so the bar sizes to it
    // Children were just placed at ABSOLUTE content coordinates, so the scroll offset has to be
    // zero for them to land where the arithmetic says (rebuildInpaintBackendSettings' rule).
    resetScrollToTop(m_keyScroll);
    m_keyScroll->redraw();
}

void SettingsDialog::refreshKeyRows() {
    if (m_host.keymap == nullptr)
        return;
    for (std::size_t i = 0; i < m_keyRows.size(); ++i) {
        const Action* action = m_host.keymap->find(m_keyRowIds[i]);
        if (action == nullptr)
            continue;
        // The PLATFORM's spelling, not the on-disk one: macOS shows ⌘ in the system menu bar, so a
        // list that said "Ctrl" beside it would be describing a different keyboard.
        std::string shown = chordDisplayText(m_host.keymap->chord(action->id));
        const bool remapped = m_host.keymap->isRemapped(action->id);
        if (shown.empty())
            shown = _("Not set");
        static_cast<KeyRow*>(m_keyRows[i])->setContent(action->label, shown, remapped);
        // Row positions are NOT touched here -- only relayout places rows, and only with the scroll
        // parked at the top. So a remap deep in the list never yanks the view back up.
        if (remapped && m_keyRows[i]->visible())
            m_keyRowResets[i]->show();
        else
            m_keyRowResets[i]->hide();
    }
    if (m_keyScroll != nullptr)
        m_keyScroll->redraw();
}

void SettingsDialog::beginKeyCapture(int row) {
    if (row < 0 || row >= static_cast<int>(m_keyRows.size()))
        return;
    endKeyCapture();
    m_keyCapture = row;
    static_cast<KeyRow*>(m_keyRows[static_cast<std::size_t>(row)])->setCapturing(true);
    // Park focus on the WINDOW so the next keystroke arrives in handle() instead of in the search
    // field: FLTK offers FL_KEYBOARD to Fl::focus() first, and a window is a legitimate focus target
    // -- the same mechanism the app-wide chrome-click unfocus uses (fileDialogInputGuard).
    Fl::focus(this);
}

void SettingsDialog::endKeyCapture() {
    if (m_keyCapture < 0)
        return;
    const int row = m_keyCapture;
    m_keyCapture = -1; // cleared FIRST: setCapturing redraws, and nothing should see a half state
    if (row < static_cast<int>(m_keyRows.size()))
        static_cast<KeyRow*>(m_keyRows[static_cast<std::size_t>(row)])->setCapturing(false);
}

void SettingsDialog::captureFromEvent() {
    const int row = m_keyCapture;
    // Leave capture BEFORE anything below can pump an event loop of its own: AskOrTellDialog::ask()
    // runs a nested Fl::wait() loop, and a dialog still in capture would answer that loop's own
    // keystrokes (the Enter that dismisses the ask) as a second chord.
    endKeyCapture();
    if (row < 0 || row >= static_cast<int>(m_keyRowIds.size()) || m_host.keymap == nullptr)
        return;
    const std::string id = m_keyRowIds[static_cast<std::size_t>(row)];
    const Action* self = m_host.keymap->find(id);
    if (self == nullptr)
        return;

    // Only the three modifiers the model can express, and the command one is asked for by NAME
    // (FL_COMMAND = Ctrl here, Cmd on macOS) rather than folded from whichever of FL_CTRL / FL_META
    // happened to be down. Folding would make a Super-key press on Linux come back spelled "Ctrl+K"
    // -- a chord the list would then be lying about. Held alone, an unsupported modifier simply does
    // not appear in the chord, and the rules below explain what is missing.
    int shortcut = Fl::event_key();
    const int state = Fl::event_state();
    if ((state & FL_SHIFT) != 0)
        shortcut |= FL_SHIFT;
    if ((state & FL_ALT) != 0)
        shortcut |= FL_ALT;
    if ((state & FL_COMMAND) != 0)
        shortcut |= FL_COMMAND;
    const KeyChord chord = fromFlShortcut(shortcut);

    const auto tell = [this](const char* title, const std::string& body) {
        AskOrTellDialog dlg;
        dlg.ask({AskOrTellDialog::Icon::Warning, title, body, {_("OK")}}, this);
    };
    const auto nameOf = [this](const std::string& other) {
        const Action* a = m_host.keymap->find(other);
        return a != nullptr ? a->label : other;
    };

    const Keymap::ChordCheck verdict = m_host.keymap->check(id, chord);
    switch (verdict.conflict) {
    case Keymap::Conflict::None:
        if (m_host.setKeyChord)
            m_host.setKeyChord(id, chordToText(chord), /*steal=*/false);
        refreshKeyRows();
        return;
    case Keymap::Conflict::Reserved:
        tell(_("That key is reserved"),
             _("Escape, Tab, plain Return and plain Space are not available. Mosaic uses them to "
               "cancel, to move between controls and to activate whatever is focused -- including "
               "in this dialog, where Escape is how you get out of a capture."));
        return;
    case Keymap::Conflict::ModifierOnDirectKey:
        tell(_("Use a single key"),
             _("Tool and color shortcuts are read in the phase that only runs for unmodified keys, "
               "so whatever you are typing into keeps first claim on the keyboard. A combination "
               "with Ctrl, Alt or Cmd would never reach them. Press one key on its own."));
        return;
    case Keymap::Conflict::MenuNeedsCtrl: {
        std::string body =
            _("A menu shortcut has to include Ctrl or Alt. Menu shortcuts are matched before the "
              "single-key tool and color shortcuts, so a bare letter on a menu command would "
              "swallow that letter everywhere else.");
        if (!verdict.otherId.empty()) {
            body += "\n\n";
            body += _("Right now that letter belongs to: ");
            body += nameOf(verdict.otherId);
            body += ".";
        }
        tell(_("That shortcut needs a modifier"), body);
        return;
    }
    case Keymap::Conflict::Taken: {
        AskOrTellDialog dlg;
        std::string body = _("This shortcut currently belongs to: ");
        body += nameOf(verdict.otherId);
        body += ".\n\n";
        body += _("Reassigning it leaves that command with no shortcut at all. Its own default is "
                  "still one click away, on its row's Reset.");
        // Rightmost button is the accent default (Enter) -- so the destructive answer is the
        // deliberate one and Escape lands on Cancel (the leftmost, kCancelAuto).
        const int answer = dlg.ask({AskOrTellDialog::Icon::Question,
                                    std::string(_("Already used: ")) + chordDisplayText(chord),
                                    body,
                                    {_("Cancel"), _("Reassign")}},
                                   this);
        if (answer == 1 && m_host.setKeyChord)
            m_host.setKeyChord(id, chordToText(chord), /*steal=*/true);
        refreshKeyRows();
        return;
    }
    }
}

void SettingsDialog::resetKeyRow(int row) {
    if (row < 0 || row >= static_cast<int>(m_keyRowIds.size()))
        return;
    endKeyCapture();
    if (m_host.resetKeyChord)
        m_host.resetKeyChord(m_keyRowIds[static_cast<std::size_t>(row)]);
    refreshKeyRows();
}

void SettingsDialog::onUnitsChanged() {
    if (m_units != nullptr && m_host.setUnits)
        m_host.setUnits(unitsKey(m_units->value()));
}

void SettingsDialog::onLanguageChanged() {
    if (m_language == nullptr || !m_host.setLanguage)
        return;
    const int i = m_language->value();
    if (i >= 0 && i < static_cast<int>(m_languageCodes.size()))
        m_host.setLanguage(m_languageCodes[static_cast<std::size_t>(i)]);
}

void SettingsDialog::onTextLanguageChanged() {
    if (m_textLanguage != nullptr && m_host.setTextLanguage) {
        const int i = m_textLanguage->value();
        if (i >= 0 && i < static_cast<int>(std::size(kTextLanguages)))
            m_host.setTextLanguage(kTextLanguages[i].tag);
    }
}

void SettingsDialog::onEmojiFontChanged() {
    if (m_emojiFont == nullptr || !m_host.setEmojiFont)
        return;
    const int i = m_emojiFont->value();
    if (i <= 0 || i > static_cast<int>(m_host.emojiFamilies.size()))
        m_host.setEmojiFont("");  // Automatic
    else
        m_host.setEmojiFont(m_host.emojiFamilies[static_cast<std::size_t>(i - 1)]);
}

void SettingsDialog::browseCmyk() {
    Fl_Native_File_Chooser chooser;
    chooser.title(_("Choose a CMYK ICC profile"));
    chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
    chooser.filter(_("ICC profiles\t*.{icc,icm}"));
    if (chooser.show() != 0) // 1 = cancel, -1 = error
        return;
    const char* picked = chooser.filename();
    if (picked == nullptr || picked[0] == '\0')
        return;
    // Apply first; only adopt + persist the path if it actually loaded as a CMYK profile, so a
    // wrong file (e.g. an RGB .icc) is never saved and can't haunt the next launch with a warning.
    if (m_host.setCmykProfile && !m_host.setCmykProfile(picked)) {
        // The app's own themed "tell" (docs/askortell-dialog.md), never FLTK's stock fl_alert --
        // system message boxes ignore the theme and the icon language.
        AskOrTellDialog dlg;
        dlg.ask({AskOrTellDialog::Icon::Warning, _("Not a CMYK profile"),
                 _("That file is not a CMYK ICC profile, so it was not applied."),
                 {_("OK")}},
                this);
        return; // keep the previous value + display
    }
    m_cmyk = picked;
    updateCmykDisplay();
}

void SettingsDialog::clearCmyk() {
    if (m_cmyk.empty())
        return;
    m_cmyk.clear();
    updateCmykDisplay();
    if (m_host.setCmykProfile)
        m_host.setCmykProfile(m_cmyk); // "" reverts to the built-in default (always succeeds)
}

void SettingsDialog::updateCmykDisplay() {
    if (m_cmykField == nullptr)
        return;
    if (m_cmyk.empty()) {
        if (m_host.defaultCmykName.empty()) {
            m_cmykField->value(_("Default (built-in)"));
            m_cmykField->tooltip(nullptr);
        } else {
            const std::string label = std::string(_("Default: ")) + m_host.defaultCmykName;
            m_cmykField->value(label.c_str());
            m_cmykField->copy_tooltip(_("Mosaic's vendored default CMYK press profile."));
        }
    } else {
        // Prefer the profile's embedded description (like the default shows its name); fall back to
        // the filename if it can't be read. The full path is always on hover.
        std::string shown = m_host.cmykProfileName ? m_host.cmykProfileName(m_cmyk) : std::string();
        if (shown.empty())
            shown = std::filesystem::path(m_cmyk).filename().string();
        m_cmykField->value(shown.c_str());
        m_cmykField->copy_tooltip(m_cmyk.c_str());
    }
}

} // namespace mosaic::ui
