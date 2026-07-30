#include "ui/type_panel.hpp"

#include "ui/scrub_slider.hpp" // the precision value slider + shared ruler HUD (replaces ui::Slider)
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include "common/image.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <map>
#include <utility>

namespace mosaic::ui {
namespace {

namespace txt = core::text;

constexpr int kContentW = 300;  // panel body width (incl. the scrollbar gutter + border)
constexpr int kPad = 12;        // inset around the content (sides + bottom)
constexpr int kTopPad = 6;      // a tighter inset at the very top (rev 8)
constexpr int kRowH = 24;       // a control row
constexpr int kRowGap = 7;      // between rows
constexpr int kSectionGap = 12; // extra space above a (non-first) section header
constexpr int kHeaderH = 18;    // section header (title + hairline)
constexpr int kLabelW = 92;     // left caption (widened so "Space before" no longer overlaps -- rev 7)
constexpr int kScrollW = 15;    // reserved right gutter for the vertical scrollbar
constexpr int kGap = 6;         // gap between the segmented toggles in a row

// The font picker open list (mirrors the context bar so the two pickers look identical, §8).
constexpr int kFontRowH = 30;
constexpr int kFontListW = 300;

constexpr int kContentLeft = kPad;
constexpr int kFieldLeft = kPad + kLabelW;
constexpr int kFieldRight = kContentW - kPad - kScrollW;
constexpr int kFieldW = kFieldRight - kFieldLeft;
constexpr int kFullW = kFieldRight - kContentLeft; // a label-less full-width row

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }
common::Color8 to8(common::ColorF c) {
    const auto q = [](float v) {
        return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return {q(c.r), q(c.g), q(c.b), q(c.a)};
}

// Which control a callback targets (one thunk serves the whole panel). Sliders go through `sliders`,
// the toggles/dropdowns/chip through their own pointers.
enum class Role {
    Font, Size, Leading, Tracking, Baseline,
    Align, // idx = the Paragraph::Align value the button represents
    SpaceBefore, SpaceAfter, IndentFirst, IndentLeft, IndentRight,
    Direction,
    Language,  // idx into kLanguages -> the BCP-47 tag (index 0 = inherit the default)
    WritingMode, // block-level: horizontal / vertical-rl / vertical-lr (idx = WritingMode value)
    Orientation, // block-level: how Latin sits in a vertical block (idx = TextOrientation value)
    Aa,          // block-level: rasterization anti-aliasing (idx = AntiAlias value) -- R4, off the bar
    Axis,        // variable-font axis slider; idx = index into TypePanel::m_axes (R4 §3.4)
    Kerning,     // run-level pair-spacing source: metric / optical / none (idx = Kerning value) -- R4
};

// The paragraph-language picker's fixed choices (deferred §0/§1): the label shown and the BCP-47 tag
// written to Paragraph::language. Index 0 is "Default" (empty tag = inherit the document/app default,
// itself seeded from the OS locale). Feeds hyphenation now and spell-check later; more than the
// installed hyphenation dictionaries is fine (a language without patterns simply does not hyphenate).
struct LangItem {
    const char* label;
    const char* tag;
};
constexpr LangItem kLanguages[] = {
    {"Default", ""},         {"English (US)", "en-US"}, {"English (UK)", "en-GB"},
    {"German", "de-DE"},     {"French", "fr-FR"},       {"Spanish", "es-ES"},
    {"Italian", "it-IT"},    {"Portuguese", "pt-PT"},   {"Dutch", "nl-NL"},
    {"Polish", "pl-PL"},     {"Russian", "ru-RU"},
};

// The curated OpenType feature toggles (R4 §3.4) -- the CSS font-variant set, not a raw tag list.
// `tag2` groups a second tag under one toggle (Ligatures = liga+clig, the CSS "common ligatures"
// pair). `defaultOn` mirrors what HarfBuzz applies without any feature list, so an untouched style
// stays an empty CharStyle::features. Toggling a feature a face lacks is a harmless shaper no-op.
struct FeatureItem {
    const char* label;
    const char* tag;
    const char* tag2;  // nullptr = single tag
    bool defaultOn;
    const char* tip;
};
constexpr FeatureItem kFeatures[] = {
    {"Ligatures", "liga", "clig", true, "Common ligatures (fi, fl)"},
    {"Contextual", "calt", nullptr, true, "Contextual alternates"},
    {"Small caps", "smcp", nullptr, false, "Lowercase as small capitals"},
    {"Oldstyle nums", "onum", nullptr, false, "Oldstyle (text) figures"},
    {"Fractions", "frac", nullptr, false, "Set 1/2-style runs as diagonal fractions"},
    {"Disc. ligatures", "dlig", nullptr, false, "Discretionary (decorative) ligatures"},
};

// The coalesce id each control shares with the host (and, for the hot controls, with the context bar
// so a continuous size/font drag spanning both surfaces is one undo step).
const char* coalesceId(Role r) {
    switch (r) {
    case Role::Font: return "font";
    case Role::Size: return "size";
    case Role::Leading: return "leading";
    case Role::Tracking: return "tracking";
    case Role::Baseline: return "baselineShift";
    case Role::Align: return "align";
    case Role::SpaceBefore: return "spaceBefore";
    case Role::SpaceAfter: return "spaceAfter";
    case Role::IndentFirst: return "indentFirst";
    case Role::IndentLeft: return "indentLeft";
    case Role::IndentRight: return "indentRight";
    case Role::Direction: return "direction";
    case Role::Language: return "language";
    case Role::WritingMode: return "writingMode";
    case Role::Orientation: return "orientation";
    case Role::Aa: return "aa";
    case Role::Axis: return "axis"; // base only -- applyControl coalesces per-tag ("axis:wght")
    case Role::Kerning: return "kerning";
    }
    return "";
}

// A passive caption that still shows its tooltip. A default Fl_Box returns 0 from FL_ENTER/FL_MOVE, so
// it never becomes Fl::belowmouse() and Fl_Tooltip never surfaces its tip (the whole "no tooltips in
// the panel" report, rev 6) -- claiming those events registers it so its tip appears on hover.
class HoverBox : public Fl_Box {
public:
    HoverBox(int X, int Y, int W, int H) : Fl_Box(X, Y, W, H) {}

protected:
    int handle(int e) override {
        if (e == FL_ENTER || e == FL_MOVE)
            return 1; // become belowmouse so the tooltip shows; we draw/consume nothing else
        return Fl_Box::handle(e);
    }
};

// GlyphButton (B/I/U/S, alignment, check) moved to widgets.hpp (ui::GlyphButton) so the Type context
// bar and this panel share one styled toggle. Used below via `GlyphButton::Kind` (same ui namespace).

// The run-colour line is the shared ui::SwatchChip (widgets.hpp): chip + hex + "Edit…", a click
// opens the host's ColorFlyout (the Fill-dialog paradigm; the R2 "toolbar swatch only" deferral is
// closed). The hatched mixed state rides the shared widget's setMixed.

// The "Advanced typography" disclosure header: a full-width clickable FlatButton that draws a
// left disclosure triangle (right = closed, down = open) + a bold caption + a hairline, matching the
// section headers. Clicking flips the panel's Advanced section (onDisclosureCb -> toggleAdvanced()).
class DisclosureButton : public FlatButton {
public:
    DisclosureButton(int X, int Y, int W, int H, const char* text)
        : FlatButton(X, Y, W, H), m_text(text) {}
    void setOpen(bool o) {
        m_open = o;
        redraw();
    }

protected:
    void draw() override {
        // A clickable LABEL, not a button: no flat-box / hover chrome. Erase to the panel ground
        // (double-buffer keeps stale pixels otherwise), then draw the triangle + caption.
        const Palette& pal = activePalette();
        fl_color(toFl(pal.panelBg));
        fl_rectf(x(), y(), w(), h());
        const int cx = x() + 6;
        const int cyc = y() + h() / 2;
        fl_color(toFl(pal.text));
        fl_begin_polygon(); // a small solid triangle: pointing down when open, right when closed
        if (m_open) {
            fl_vertex(cx - 4, cyc - 2);
            fl_vertex(cx + 4, cyc - 2);
            fl_vertex(cx, cyc + 3);
        } else {
            fl_vertex(cx - 2, cyc - 4);
            fl_vertex(cx - 2, cyc + 4);
            fl_vertex(cx + 3, cyc);
        }
        fl_end_polygon();
        fl_font(FL_HELVETICA_BOLD, 12);
        fl_draw(m_text.c_str(), x() + 18, y(), w() - 18, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        if (m_open) { // the section rule only reads while the section is open (a bare label when closed)
            fl_color(toFl(pal.border));
            fl_line(x(), y() + h() - 1, x() + w() - 1, y() + h() - 1);
        }
    }

private:
    std::string m_text;
    bool m_open = false;
};

void onControlCb(Fl_Widget* w, void* u);    // the single control callback thunk (defined below)
void onDisclosureCb(Fl_Widget* w, void* u); // the Advanced disclosure toggle (defined below)

} // namespace

// A control's link back to the panel (FLTK callbacks are C thunks; the binding carries the rest).
struct Binding {
    TypePanel* self = nullptr;
    Role role = Role::Size;
    int idx = 0;
};

struct TypePanel::State {
    std::vector<std::unique_ptr<Binding>> bindings;
    ScrollView* scroll = nullptr;
    Fl_Group* content = nullptr;
    Dropdown* font = nullptr;
    Dropdown* direction = nullptr;
    Dropdown* language = nullptr;
    Dropdown* writingMode = nullptr;      // horizontal / vertical columns (block-level)
    Dropdown* orientation = nullptr;      // Latin orientation in a vertical block (greyed when horizontal)
    Dropdown* aa = nullptr;               // rasterization anti-aliasing (block-level; R4, off the bar)
    Dropdown* kerning = nullptr;          // pair-spacing source: metric / optical / none (R4)
    HoverBox* orientationCaption = nullptr; // its caption -- greyed together with the control
    CheckBox* hyphenate = nullptr;          // the settled themed checkbox (ui::CheckBox)
    DisclosureButton* disclosure = nullptr; // the "Advanced typography" header toggle
    Fl_Group* advanced = nullptr;           // the collapsible Advanced section (shown/hidden as one)
    std::array<FlatButton*, 4> align{};
    SwatchChip* colour = nullptr;
    std::map<Role, ScrubSlider*> sliders;
    std::vector<ScrubSlider*> axisSliders; // one per m_axes entry, in order (R4 §3.4)
    std::array<CheckBox*, std::size(kFeatures)> features{}; // OpenType toggles (R4 §3.4)
};

TypePanel::TypePanel() : Popover(kContentW, 400), m_state(std::make_unique<State>()) {
    setPinned(true); // survives clicks on the canvas/chrome; closed on tool switch / session end / re-click
}
TypePanel::~TypePanel() = default;

void TypePanel::setFontFamilies(std::vector<std::string> families) {
    m_families = std::move(families);
    if (m_state->content != nullptr)
        build(); // rebuild so the font dropdown carries the real list
}

void TypePanel::setScrubRuler(ScrubRuler* ruler) {
    m_scrubRuler = ruler;
    if (m_state->content != nullptr)
        build(); // hand the ruler to each slider as it (re)builds
}

void TypePanel::setPlacementProviders(std::function<common::Rect()> region,
                                      std::function<std::optional<common::Rect>()> avoid) {
    m_region = std::move(region);
    m_avoid = std::move(avoid);
    setCornerPlacement(Corner::BottomRight, m_region, m_avoid);
}

void TypePanel::reapplyTheme() {
    Popover::reapplyTheme();
    build(); // re-bake the controls' palette
}

void TypePanel::build() {
    const Palette& pal = activePalette();
    // Width-stable layout: rows reserve the scrollbar gutter whether or not the list actually scrolls.
    //
    // Normalize the window to its base footprint BEFORE laying out children: a HIDDEN popover is
    // group-stretched by every main-window resize (nothing re-pins it until it is shown), and
    // building design-coordinate children inside that stretched box bakes an inconsistent resize
    // baseline -- the show-time restore to the base size then squashes the scroll and shoves the
    // rows aside (user 2026-07-16: "style gets the inner controls pushed to the left", the
    // round-3 ghost finally reproduced).
    resizable(nullptr);
    Fl_Double_Window::resize(x(), y(), m_baseW, m_baseH);
    clear();            // delete the previous widgets first (no callbacks can fire after)...
    resizable(nullptr); // ...re-pin (clear() resets resizable to the group)...
    *m_state = State{}; // ...then drop their now-unreferenced bindings + stale pointers
    begin();
    auto* sv = new ScrollView(1, 1, kContentW - 2, h() - 2);
    // VERTICAL_ALWAYS, not VERTICAL: the rows reserve the scrollbar gutter (kFieldRight), so if the bar
    // came and went (e.g. collapsing Advanced makes the content fit) that gutter would flip between the
    // scrollbar and an empty strip -- the width/proportions "jumping" the user saw. Keeping the bar
    // always present holds the gutter occupied and the layout width stable.
    sv->type(Fl_Scroll::VERTICAL_ALWAYS);
    sv->box(FL_FLAT_BOX); // a solid panelBg ground so scrolling leaves no artifact
    sv->color(toFl(pal.panelBg));
    sv->begin();
    auto* content = new Fl_Group(1, 1, kContentW - 2 - kScrollW, 4000);
    // CRITICAL: an Fl_Group's default resizable() is the group itself, so the later size() that trims
    // this group from the provisional 4000px to the real content height would PROPORTIONALLY scale
    // every child (collapsing all rows ~0.13x, leaving them ~1px apart and overlapping). Pin it null so
    // size() only changes the group's own box -- the rows keep their absolute positions.
    content->resizable(nullptr);
    content->begin();

    m_state->scroll = sv;
    m_state->content = content;

    fl_font(FL_HELVETICA, 12);
    int cy = 1 + kTopPad;

    const auto bind = [&](Role role, int idx) {
        auto b = std::make_unique<Binding>();
        b->self = this;
        b->role = role;
        b->idx = idx;
        Binding* raw = b.get();
        m_state->bindings.push_back(std::move(b));
        return raw;
    };
    const auto caption = [&](int rowY, const char* text, const char* tip) -> HoverBox* {
        auto* b = new HoverBox(kContentLeft, rowY, kLabelW, kRowH);
        b->copy_label(text);
        b->box(FL_NO_BOX);
        b->labelfont(FL_HELVETICA);
        b->labelsize(12);
        b->labelcolor(toFl(pal.textMuted));
        b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        if (tip != nullptr)
            b->copy_tooltip(tip);
        return b;
    };
    const auto header = [&](const char* text, bool first) {
        cy += first ? 0 : kSectionGap; // the first header hugs the (already tightened) top inset
        auto* b = new Fl_Box(kContentLeft, cy, kFullW, kHeaderH);
        b->copy_label(text);
        b->box(FL_NO_BOX);
        b->labelfont(FL_HELVETICA_BOLD);
        b->labelsize(12);
        b->labelcolor(toFl(pal.text));
        b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        auto* rule = new Fl_Box(kContentLeft, cy + kHeaderH - 1, kFullW, 1);
        rule->box(FL_FLAT_BOX);
        rule->color(toFl(pal.border));
        cy += kHeaderH + kRowGap;
    };
    // A captioned precision slider (ScrubSlider: it draws its own value, so no separate readout box --
    // rev 5). Full field width; shares the bar's ruler HUD. Registers in the role-keyed slider map.
    const auto sliderRow = [&](const char* label, Role role, double min, double max, double step,
                               const char* suffix, const char* tip) {
        caption(cy, label, tip);
        auto* s = new ScrubSlider(kFieldLeft, cy, kFieldW, kRowH);
        s->range(min, max);
        s->step(step);
        s->setSuffix(suffix);
        s->setCellColor(pal.panelBg);
        s->setRuler(m_scrubRuler);
        s->when(FL_WHEN_CHANGED);
        s->callback(onControlCb, bind(role, 0));
        if (tip != nullptr)
            s->copy_tooltip(tip);
        m_state->sliders[role] = s;
        cy += kRowH + kRowGap;
    };
    // A segmented glyph toggle (B/I/U/S, alignment); FlatButton-derived, drawn in its own style (rev 3/4).
    const auto glyphBtn = [&](int X, int W, GlyphButton::Kind kind, Role role, int idx, const char* tip) {
        auto* t = new GlyphButton(X, cy, W, kRowH, kind);
        t->type(FL_TOGGLE_BUTTON);
        t->callback(onControlCb, bind(role, idx));
        if (tip != nullptr)
            t->copy_tooltip(tip);
        return t;
    };

    // A captioned dropdown row (Language / Writing mode / Orientation / Direction).
    const auto dropdownRow = [&](const char* label, Role role, const char* tip,
                                 std::initializer_list<const char*> items) {
        HoverBox* cap = caption(cy, label, tip);
        auto* d = new Dropdown(kFieldLeft, cy, kFieldW, kRowH);
        for (const char* it : items)
            d->add(it);
        d->callback(onControlCb, bind(role, 0));
        d->copy_tooltip(tip);
        cy += kRowH + kRowGap;
        return std::pair<Dropdown*, HoverBox*>{d, cap};
    };

    // ==== COMMON (always visible) =============================================================
    // ---- Character ----
    header("Character", /*first=*/true);

    caption(cy, "Font", "Font family");
    auto* font = new Dropdown(kFieldLeft, cy, kFieldW, kRowH);
    for (const std::string& fam : m_families)
        font->add(fam.c_str());
    font->callback(onControlCb, bind(Role::Font, 0));
    font->copy_tooltip("Font family");
    if (m_fontPreview) {
        font->setRowHeight(kFontRowH);
        font->setListMinWidth(kFontListW);
        font->setRowPreview([this](int i, int cw, int ch) -> Fl_RGB_Image* {
            if (i < 0 || i >= static_cast<int>(m_families.size()))
                return nullptr;
            return m_fontPreview(m_families[static_cast<std::size_t>(i)], cw, ch);
        });
        if (m_fontHover)
            font->setHoverPreview([this](int i) {
                m_fontHover(i >= 0 && i < static_cast<int>(m_families.size())
                                ? m_families[static_cast<std::size_t>(i)]
                                : std::string());
            });
    }
    m_state->font = font;
    cy += kRowH + kRowGap;

    sliderRow("Size", Role::Size, 6, 400, 0.5, "pt", "Font size"); // half-point steps (decimal-capable)

    // Variable-font axes (R4 §3.4): one precision slider per axis the SELECTED face exposes, right
    // under Size where weight/width belong. The set (m_axes) is swapped by reflect() when the
    // resolved face changes -- a full rebuild, so rows stay fixed-position between family switches.
    // A static face contributes none and this loop is empty.
    for (int i = 0; i < static_cast<int>(m_axes.size()); ++i) {
        const txt::VariableAxis& ax = m_axes[static_cast<std::size_t>(i)];
        const std::string tip = "Variable font axis '" + ax.tag + "'";
        caption(cy, ax.name.c_str(), tip.c_str());
        auto* s = new ScrubSlider(kFieldLeft, cy, kFieldW, kRowH);
        const double span = static_cast<double>(ax.max) - static_cast<double>(ax.min);
        s->range(ax.min, ax.max);
        s->step(span >= 100.0 ? 1.0 : span >= 20.0 ? 0.5 : 0.01);
        s->setCellColor(pal.panelBg);
        s->setRuler(m_scrubRuler);
        s->when(FL_WHEN_CHANGED);
        s->callback(onControlCb, bind(Role::Axis, i));
        s->copy_tooltip(tip.c_str());
        m_state->axisSliders.push_back(s);
        cy += kRowH + kRowGap;
    }

    // Bold / Italic / Underline / Strikethrough live on the Type CONTEXT BAR now (styled glyph toggles),
    // right before "Style…" (user 2026-07-01). Color stays here (the bar has no paint control yet).
    caption(cy, "Color", "Run color — click to edit");
    {
        auto* chip = new SwatchChip(kFieldLeft, cy, kFieldW, kRowH);
        chip->setGroundColor(pal.panelBg);
        chip->setInteractive(true);
        chip->copy_tooltip("Run color — click to edit");
        chip->setOnClick([this, chip] {
            if (!m_onEditColor)
                return;
            const common::Color8 c = chip->colour(); // the representative fill (mixed included)
            m_onEditColor(chip, {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f});
        });
        m_state->colour = chip;
    }
    cy += kRowH + kRowGap;

    // ---- Paragraph ----
    header("Paragraph", /*first=*/false);

    {
        caption(cy, "Align", "Paragraph alignment");
        using K = GlyphButton::Kind;
        const K kinds[4] = {K::AlignLeft, K::AlignCenter, K::AlignRight, K::AlignJustify};
        const char* tips[4] = {"Align left", "Align center", "Align right", "Justify"};
        const int n = 4;
        const int bw = (kFieldW - (n - 1) * kGap) / n;
        for (int i = 0; i < 4; ++i)
            m_state->align[static_cast<std::size_t>(i)] =
                glyphBtn(kFieldLeft + i * (bw + kGap), bw, kinds[i], Role::Align, i, tips[i]);
        cy += kRowH + kRowGap;
    }

    m_state->language =
        dropdownRow("Language", Role::Language, "Language for hyphenation and spell-check (BCP-47)",
                    {}).first;
    for (const LangItem& li : kLanguages)  // dropdownRow adds no items for Language; fill them here
        m_state->language->add(li.label);

    m_state->writingMode =
        dropdownRow("Writing mode", Role::WritingMode, "Lay the text horizontally, or in vertical columns",
                    {"Horizontal", "Vertical (RTL)", "Vertical (LTR)"})
            .first;

    // ==== ADVANCED (collapsible: leading/tracking/baseline/hyphenate/orientation/spacing/indents/dir) =
    cy += kSectionGap;
    auto* disc = new DisclosureButton(kContentLeft, cy, kFullW, kHeaderH + 2, "Advanced typography");
    disc->callback(onDisclosureCb, this);
    disc->setOpen(m_advancedOpen);
    disc->copy_tooltip("Show or hide the finer typography controls");
    m_state->disclosure = disc;
    cy += kHeaderH + 2 + kRowGap;

    const int advTop = cy;
    m_contentHCollapsed = advTop + kPad - 1; // content bottom when Advanced is closed

    auto* adv = new Fl_Group(1, advTop, kContentW - 2 - kScrollW, kRowH);
    adv->box(FL_NO_BOX);
    adv->resizable(nullptr); // a group's default resizable is itself -> pin so size() won't scale rows
    adv->begin();

    sliderRow("Leading", Role::Leading, 0.5, 4.0, 0.05, "", // ratio (multiple of em)
              "Line height as a multiple of the font size");
    sliderRow("Tracking", Role::Tracking, -200, 800, 1, "", "Letter spacing (1/1000 em)");
    sliderRow("Baseline", Role::Baseline, -100, 100, 0.5, "px",
              "Baseline shift: raise (+) or lower (−) the run");

    // Kerning source (R4 §13): the font's own pairs, shape-derived optical spacing, or none.
    m_state->kerning =
        dropdownRow("Kerning", Role::Kerning,
                    "Pair spacing: the font's kern data, optical (from the glyph shapes), or none",
                    {"Metric", "Optical", "None"})
            .first;

    // OpenType feature toggles (R4 §3.4): the curated kFeatures set as a two-column checkbox grid.
    // Each writes the selection's CharStyle::features through the style funnel (the core
    // setFeatureEnabled helper records only deviations from the shaper default).
    {
        const int colW = (kFullW - kGap) / 2;
        for (std::size_t i = 0; i < std::size(kFeatures); ++i) {
            const FeatureItem& item = kFeatures[i];
            const int col = static_cast<int>(i % 2);
            auto* cb = new CheckBox(
                kContentLeft + col * (colW + kGap), cy, colW, kRowH, item.label,
                [this, i](bool on) {
                    if (m_reflecting || !m_onStyleEdit)
                        return;
                    const FeatureItem& it = kFeatures[i];
                    const std::string tag = it.tag;
                    const std::string tag2 = it.tag2 != nullptr ? it.tag2 : "";
                    const bool defOn = it.defaultOn;
                    m_onStyleEdit(std::string("feat:") + it.tag,
                                  [tag, tag2, defOn, on](txt::CharStyle& s) {
                                      txt::setFeatureEnabled(s.features, tag, defOn, on);
                                      if (!tag2.empty())
                                          txt::setFeatureEnabled(s.features, tag2, defOn, on);
                                  });
                });
            cb->setGroundColor(pal.panelBg);
            cb->copy_tooltip(item.tip);
            m_state->features[i] = cb;
            if (col == 1 || i + 1 == std::size(kFeatures))
                cy += kRowH + kRowGap;
        }
    }

    // Hyphenate: the settled themed checkbox (ui::CheckBox), a full-width "[x] Hyphenate" row. Its
    // onToggle routes through the same paragraph funnel + coalesce id ("hyphenate") the old toggle used.
    {
        auto* hy = new CheckBox(kContentLeft, cy, kFullW, kRowH, "Hyphenate", [this](bool on) {
            if (m_reflecting || !m_onParagraphEdit)
                return;
            m_onParagraphEdit("hyphenate", [on](txt::Paragraph& p) { p.hyphenate = on; });
        });
        hy->setGroundColor(pal.panelBg); // erase to the panel ground (Settings uses the default windowBg)
        hy->copy_tooltip("Automatically hyphenate wrapped (Area) lines in the paragraph's language");
        m_state->hyphenate = hy;
    }
    cy += kRowH + kRowGap;

    {
        auto [d, cap] =
            dropdownRow("Orientation", Role::Orientation,
                        "How Latin text sits in vertical writing (mixed = rotated, upright)",
                        {"Mixed (rotate Latin)", "Upright"});
        m_state->orientation = d;
        m_state->orientationCaption = cap;
    }

    sliderRow("Space before", Role::SpaceBefore, 0, 300, 0.5, "px", "Space above the paragraph");
    sliderRow("Space after", Role::SpaceAfter, 0, 300, 0.5, "px", "Space below the paragraph");
    sliderRow("Indent 1st", Role::IndentFirst, -200, 300, 0.5, "px", "First-line indent");
    sliderRow("Indent left", Role::IndentLeft, 0, 300, 0.5, "px", "Left indent");
    sliderRow("Indent right", Role::IndentRight, 0, 300, 0.5, "px", "Right indent");

    m_state->direction = dropdownRow("Direction", Role::Direction,
                                     "Paragraph base writing direction (bidi)", {"Auto", "LTR", "RTL"})
                             .first;

    // Anti-alias (block-level, moved off the context bar in R4): how the whole object rasterizes.
    // Subpixel silently degrades to grayscale when its preconditions fail (rotation, transparency).
    m_state->aa = dropdownRow("Anti-alias", Role::Aa,
                              "Text rasterization: hard edges, grayscale smoothing, or LCD subpixel",
                              {"None", "Grayscale", "Subpixel"})
                      .first;

    adv->end();
    adv->size(adv->w(), cy - advTop);
    m_state->advanced = adv;
    m_contentHFull = cy + kPad - 1;

    if (!m_advancedOpen)
        adv->hide();
    m_contentH = m_advancedOpen ? m_contentHFull : m_contentHCollapsed;
    content->size(content->w(), m_contentH);
    content->end();
    sv->end();
    end();
    resizable(sv); // the scroll fills any height change (setBaseSize on open / disclosure toggle)
    redraw();
}

void TypePanel::reflect(const txt::CommonStyle& cs, const txt::CommonParagraph& cp,
                        txt::WritingMode wm, txt::TextOrientation orientation, txt::AntiAlias aa,
                        const std::vector<txt::VariableAxis>& axes) {
    if (m_state->content == nullptr)
        return;
    m_reflecting = true;
    // R4 §3.4: the axis-slider set follows the selection's resolved face. A different set (family
    // switch, or mixed -> none) rebuilds the panel; same set (the per-keystroke reflects, and any
    // reflect DURING an axis drag) leaves the rows alone, so drags are never yanked mid-scrub.
    if (axes != m_axes) {
        m_axes = axes;
        build();
        resizeToContent();
    }
    // Defensive (round 3, unreproduced once: "all controls shifted left, scrollbar at the very
    // left"): the panel never scrolls horizontally by design, so pin any stray x-scroll back.
    if (m_state->scroll != nullptr && m_state->scroll->xposition() != 0)
        m_state->scroll->scroll_to(0, m_state->scroll->yposition());

    const txt::CharStyle& s = cs.style;
    const txt::StyleAgreement& sa = cs.agree;
    const txt::Paragraph& p = cp.para;
    const txt::ParagraphAgreement& pa = cp.agree;

    // Font: select the family (or show "Mixed" when the selection spans several).
    if (m_state->font != nullptr) {
        if (!sa.family) {
            m_state->font->setOverrideText("Mixed");
        } else {
            m_state->font->setOverrideText("");
            const auto it = std::find(m_families.begin(), m_families.end(), s.font.family);
            if (it != m_families.end())
                m_state->font->value(static_cast<int>(std::distance(m_families.begin(), it)));
        }
    }

    // ScrubSliders draw their own value; a "mixed" field just shows its representative value (the
    // first touched run's), matching the context bar -- there is no separate readout to mark "—".
    const auto setSlider = [&](Role r, double v) {
        if (auto it = m_state->sliders.find(r); it != m_state->sliders.end()) {
            it->second->value(v);
            it->second->redraw();
        }
    };
    setSlider(Role::Size, s.sizePx);
    // Axis sliders: wght/wdth live on FontRef (they also steer static-face matching); any other
    // axis reads from the explicit variations map, falling back to the axis default.
    for (std::size_t i = 0; i < m_axes.size() && i < m_state->axisSliders.size(); ++i) {
        const txt::VariableAxis& ax = m_axes[i];
        double v = ax.def;
        if (ax.tag == "wght")
            v = s.font.weight;
        else if (ax.tag == "wdth")
            v = s.font.widthAxis;
        else if (const auto it = s.font.variations.find(ax.tag); it != s.font.variations.end())
            v = it->second;
        m_state->axisSliders[i]->value(v);
        m_state->axisSliders[i]->redraw();
    }
    setSlider(Role::Tracking, s.tracking);
    setSlider(Role::Baseline, s.baselineShift);
    setSlider(Role::Leading, p.leading);
    setSlider(Role::SpaceBefore, p.spaceBefore);
    setSlider(Role::SpaceAfter, p.spaceAfter);
    setSlider(Role::IndentFirst, p.indentFirst);
    setSlider(Role::IndentLeft, p.indentLeft);
    setSlider(Role::IndentRight, p.indentRight);

    const auto setToggle = [](FlatButton* b, bool on) {
        if (b != nullptr) {
            b->value(on ? 1 : 0);
            b->redraw();
        }
    };
    for (int i = 0; i < 4; ++i)
        setToggle(m_state->align[static_cast<std::size_t>(i)],
                  pa.align && static_cast<int>(p.align) == i);

    if (m_state->direction != nullptr) {
        if (!pa.direction)
            m_state->direction->setOverrideText("Mixed");
        else {
            m_state->direction->setOverrideText("");
            m_state->direction->value(static_cast<int>(p.direction));
        }
    }

    if (m_state->language != nullptr) {
        if (!pa.language) {
            m_state->language->setOverrideText("Mixed");
        } else {
            int idx = -1;
            for (int i = 0; i < static_cast<int>(std::size(kLanguages)); ++i)
                if (p.language == kLanguages[static_cast<std::size_t>(i)].tag) {
                    idx = i;
                    break;
                }
            if (idx >= 0) {  // a known tag (incl. "" -> "Default" at index 0)
                m_state->language->setOverrideText("");
                m_state->language->value(idx);
            } else {  // a tag we do not list: show it raw rather than mislabel
                m_state->language->setOverrideText(p.language);
            }
        }
    }
    if (m_state->kerning != nullptr) {
        if (!sa.kerning) {
            m_state->kerning->setOverrideText("Mixed");
        } else {
            m_state->kerning->setOverrideText("");
            m_state->kerning->value(static_cast<int>(s.kerning));
        }
    }

    // OpenType feature toggles: read each against its shaper default (a mixed selection shows the
    // first run's state -- the representative-value convention every other control uses).
    for (std::size_t i = 0; i < std::size(kFeatures); ++i)
        if (m_state->features[i] != nullptr)
            m_state->features[i]->setChecked(
                txt::featureEnabled(s.features, kFeatures[i].tag, kFeatures[i].defaultOn));

    if (m_state->hyphenate != nullptr) {
        m_state->hyphenate->setChecked(pa.hyphenate && p.hyphenate); // a mixed selection reads off
        // Hyphenation applies only to horizontally-wrapped lines: the vertical shaper never hyphenates
        // (columns break anywhere), so grey the checkbox in vertical to make that explicit.
        if (wm != txt::WritingMode::HorizontalTB)
            m_state->hyphenate->deactivate();
        else
            m_state->hyphenate->activate();
    }

    if (m_state->colour != nullptr) {
        m_state->colour->setColour(to8(s.solidFill()));
        m_state->colour->setMixed(!sa.paint);
    }

    // Writing mode is always active; Orientation applies only to vertical text, so grey it (and its
    // caption) out in horizontal rather than leave a mid-panel gap. Both values are set even while greyed
    // so the control is correct the moment it re-activates.
    if (m_state->writingMode != nullptr)
        m_state->writingMode->value(static_cast<int>(wm));
    if (m_state->orientation != nullptr && m_state->orientationCaption != nullptr) {
        m_state->orientation->value(static_cast<int>(orientation));
        if (wm != txt::WritingMode::HorizontalTB) {
            m_state->orientation->activate();
            m_state->orientationCaption->activate();
        } else {
            m_state->orientation->deactivate();
            m_state->orientationCaption->deactivate();
        }
    }
    if (m_state->aa != nullptr)
        m_state->aa->value(static_cast<int>(aa));

    m_reflecting = false;
}

void TypePanel::applyControl(int roleInt, int idx) {
    if (m_reflecting)
        return;
    const Role role = static_cast<Role>(roleInt);
    const std::string id = coalesceId(role);
    const auto sliderVal = [&](Role r) {
        const auto it = m_state->sliders.find(r);
        return it != m_state->sliders.end() ? it->second->value() : 0.0;
    };

    switch (role) {
    case Role::Font: {
        if (!m_onStyleEdit || m_state->font == nullptr)
            break;
        const int v = m_state->font->value();
        if (v < 0 || v >= static_cast<int>(m_families.size()))
            break;
        m_state->font->setOverrideText(""); // a pick resolves a previous "Mixed"
        const std::string fam = m_families[static_cast<std::size_t>(v)];
        m_onStyleEdit(id, [fam](txt::CharStyle& s) { s.font.family = fam; });
        break;
    }
    case Role::Size:
        if (m_onStyleEdit) {
            const auto v = static_cast<float>(sliderVal(role));
            m_onStyleEdit(id, [v](txt::CharStyle& s) { s.sizePx = v; });
        }
        break;
    case Role::Tracking:
        if (m_onStyleEdit) {
            const auto v = static_cast<float>(sliderVal(role));
            m_onStyleEdit(id, [v](txt::CharStyle& s) { s.tracking = v; });
        }
        break;
    case Role::Baseline:
        if (m_onStyleEdit) {
            const auto v = static_cast<float>(sliderVal(role));
            m_onStyleEdit(id, [v](txt::CharStyle& s) { s.baselineShift = v; });
        }
        break;
    case Role::Align:
        if (m_onParagraphEdit) {
            const auto a = static_cast<txt::Paragraph::Align>(idx);
            for (int i = 0; i < 4; ++i) // a segmented row: light only the picked button
                if (FlatButton* b = m_state->align[static_cast<std::size_t>(i)]) {
                    b->value(i == idx ? 1 : 0);
                    b->redraw();
                }
            m_onParagraphEdit(id, [a](txt::Paragraph& p) { p.align = a; });
        }
        break;
    case Role::Leading:
        if (m_onParagraphEdit) {
            const auto v = static_cast<float>(sliderVal(role));
            m_onParagraphEdit(id, [v](txt::Paragraph& p) {
                p.leading = v;
                p.leadingAbsolute = false; // the panel edits the ratio (multiple of em)
            });
        }
        break;
    case Role::SpaceBefore:
        if (m_onParagraphEdit) {
            const auto v = static_cast<float>(sliderVal(role));
            m_onParagraphEdit(id, [v](txt::Paragraph& p) { p.spaceBefore = v; });
        }
        break;
    case Role::SpaceAfter:
        if (m_onParagraphEdit) {
            const auto v = static_cast<float>(sliderVal(role));
            m_onParagraphEdit(id, [v](txt::Paragraph& p) { p.spaceAfter = v; });
        }
        break;
    case Role::IndentFirst:
        if (m_onParagraphEdit) {
            const auto v = static_cast<float>(sliderVal(role));
            m_onParagraphEdit(id, [v](txt::Paragraph& p) { p.indentFirst = v; });
        }
        break;
    case Role::IndentLeft:
        if (m_onParagraphEdit) {
            const auto v = static_cast<float>(sliderVal(role));
            m_onParagraphEdit(id, [v](txt::Paragraph& p) { p.indentLeft = v; });
        }
        break;
    case Role::IndentRight:
        if (m_onParagraphEdit) {
            const auto v = static_cast<float>(sliderVal(role));
            m_onParagraphEdit(id, [v](txt::Paragraph& p) { p.indentRight = v; });
        }
        break;
    case Role::Direction:
        if (m_onParagraphEdit && m_state->direction != nullptr) {
            m_state->direction->setOverrideText("");
            const auto d = static_cast<txt::Paragraph::Direction>(m_state->direction->value());
            m_onParagraphEdit(id, [d](txt::Paragraph& p) { p.direction = d; });
        }
        break;
    case Role::Language:
        if (m_onParagraphEdit && m_state->language != nullptr) {
            m_state->language->setOverrideText(""); // a pick resolves a previous "Mixed"
            const int v = m_state->language->value();
            if (v < 0 || v >= static_cast<int>(std::size(kLanguages)))
                break;
            const std::string tag = kLanguages[static_cast<std::size_t>(v)].tag;
            m_onParagraphEdit(id, [tag](txt::Paragraph& p) { p.language = tag; });
        }
        break;
    case Role::WritingMode:
        if (m_onBlockEdit && m_state->writingMode != nullptr) {
            const auto wm = static_cast<txt::WritingMode>(m_state->writingMode->value());
            m_onBlockEdit(id, [wm](txt::TextBlock& b) { b.writingMode = wm; });
        }
        break;
    case Role::Orientation:
        if (m_onBlockEdit && m_state->orientation != nullptr) {
            const auto o = static_cast<txt::TextOrientation>(m_state->orientation->value());
            m_onBlockEdit(id, [o](txt::TextBlock& b) { b.orientation = o; });
        }
        break;
    case Role::Aa:
        if (m_onBlockEdit && m_state->aa != nullptr) {
            const auto aa = static_cast<txt::AntiAlias>(m_state->aa->value());
            m_onBlockEdit(id, [aa](txt::TextBlock& b) { b.aa = aa; });
        }
        break;
    case Role::Kerning:
        if (m_onStyleEdit && m_state->kerning != nullptr) {
            m_state->kerning->setOverrideText(""); // a pick resolves a previous "Mixed"
            const auto k = static_cast<txt::Kerning>(m_state->kerning->value());
            m_onStyleEdit(id, [k](txt::CharStyle& s) { s.kerning = k; });
        }
        break;
    case Role::Axis: {
        if (!m_onStyleEdit || idx < 0 || idx >= static_cast<int>(m_axes.size()) ||
            idx >= static_cast<int>(m_state->axisSliders.size()))
            break;
        const txt::VariableAxis& ax = m_axes[static_cast<std::size_t>(idx)];
        const auto v =
            static_cast<float>(m_state->axisSliders[static_cast<std::size_t>(idx)]->value());
        const std::string axisId = "axis:" + ax.tag; // coalesce per axis, not across axes
        if (ax.tag == "wght") {
            m_onStyleEdit(axisId, [v](txt::CharStyle& s) { s.font.weight = v; });
        } else if (ax.tag == "wdth") {
            m_onStyleEdit(axisId, [v](txt::CharStyle& s) { s.font.widthAxis = v; });
        } else {
            const std::string tag = ax.tag;
            m_onStyleEdit(axisId, [tag, v](txt::CharStyle& s) { s.font.variations[tag] = v; });
        }
        break;
    }
    }
}

// Set the panel's footprint to fit m_contentH, clamped to the region (the canvas) so it never exceeds
// it -- the ScrollView takes any overflow, and the corner placement pins it within that same region.
// Reanchors when already shown (a live disclosure toggle); the caller shows it otherwise.
void TypePanel::resizeToContent() {
    int availH = 600;
    if (m_region) {
        const common::Rect reg = m_region();
        if (!reg.empty())
            availH = static_cast<int>(std::lround(reg.h)) - 24;
    }
    const int H = std::clamp(m_contentH + 2, 120, std::max(120, availH));
    setBaseSize(kContentW, H);
    if (shown())
        reanchor();
}

void TypePanel::toggle(const Fl_Widget* anchor) {
    if (m_state->content == nullptr)
        build();
    if (shownFor(anchor)) {
        hide();
        return;
    }
    resizeToContent(); // size for the current disclosure state (not shown yet -> no reanchor)
    showAnchored(anchor);
}

void TypePanel::toggleAdvanced() {
    m_advancedOpen = !m_advancedOpen;
    if (m_state->disclosure != nullptr)
        m_state->disclosure->setOpen(m_advancedOpen);
    if (m_state->advanced != nullptr) {
        if (m_advancedOpen)
            m_state->advanced->show();
        else
            m_state->advanced->hide();
    }
    m_contentH = m_advancedOpen ? m_contentHFull : m_contentHCollapsed;
    if (m_state->content != nullptr)
        m_state->content->size(m_state->content->w(), m_contentH);
    resizeToContent(); // grow/shrink the panel window to the new height + reanchor to the corner
    // Reset the scroll to the TOP so collapsing doesn't leave the view stranded in now-empty space.
    // Zero the scrollbar WIDGET value first (Fl_Scroll re-derives from it on the next draw), and
    // PRESERVE xposition() -- passing X=0 yanks the content flush-left (the Settings-dialog scroll-0
    // trap, resetScrollToTop). [[mosaic-ui-gotchas]]
    if (m_state->scroll != nullptr) {
        m_state->scroll->scrollbar.value(0);
        m_state->scroll->scroll_to(m_state->scroll->xposition(), 0);
        m_state->scroll->redraw();
    }
    redraw();
}

namespace {
void onControlCb(Fl_Widget* /*w*/, void* u) {
    auto* b = static_cast<Binding*>(u);
    if (b != nullptr && b->self != nullptr)
        b->self->applyControl(static_cast<int>(b->role), b->idx);
}
void onDisclosureCb(Fl_Widget* /*w*/, void* u) {
    if (auto* self = static_cast<TypePanel*>(u))
        self->toggleAdvanced();
}
} // namespace

} // namespace mosaic::ui
