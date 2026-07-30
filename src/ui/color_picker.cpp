#include "ui/color_picker.hpp"

#include "common/i18n.hpp"
#include "core/color_management.hpp"
#include "ui/color_models.hpp"
#include "ui/color_state.hpp"
#include "ui/color_surfaces.hpp" // SvField, HueStrip, ColorWheel (shared with the Fill colour flyout)
#include "ui/theme.hpp"
#include "ui/widgets.hpp" // Dropdown, Slider

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace mosaic::ui {
namespace {

// Layout (all popover-local). The three picking surfaces share one square-ish region, so swapping
// them never resizes the popover.
constexpr int kPickerW = 232;
constexpr int kPad = 12;
// The bubble lives in Popover; the picker just reserves its left margin when the backend supports it.
// Evaluated as a local (NOT a file-scope static — bubbleSupported() reads FLTK_BACKEND, which main()
// only pins in preferWaylandBackendIfUnset, after static init).
[[nodiscard]] int bodyLeftMargin() { return Popover::bubbleSupported() ? Popover::kBubbleTri : 0; }
constexpr int kPreviewH = 22;
constexpr int kFieldY = 42;
// Shared picking-surface region: as tall as the content is wide, so the wheels span the full
// content width (the triangle was "small-ish" at 160) with the Field's own margins, and the field
// simply matches the height -- switching surfaces never resizes the popover (review feedback).
constexpr int kSurfH = kPickerW - 2 * kPad;
constexpr int kStripW = 16;
constexpr int kGap = 6;
// Field width is whatever right-aligns the hue strip with the popover content edge (review
// feedback: slightly rectangular beats a square field + dead space on the right).
constexpr int kFieldW = kPickerW - 2 * kPad - kStripW - kGap;
constexpr int kComboY = kFieldY + kSurfH + 8;
constexpr int kComboW = 86;        // model combo (short entries)
constexpr int kSurfaceComboW = 100; // surface combo ("HSL Wheel" was clipped at 86)
constexpr int kRowsY = kComboY + 30;
constexpr int kRowH = 22;
constexpr int kPitch = 26;
constexpr int kLabelW = 16;
constexpr int kReadoutW = 34;
constexpr int kHexH = 24;
// The hex + indicator rows sit under 3 slider rows normally, 4 in CMYK mode; layoutRows() moves
// them and resizes the popover live (the proven setBaseSize + reanchor machinery).
constexpr int kHexYFor3 = kRowsY + 3 * kPitch + 4;
constexpr int kHexYFor4 = kRowsY + 4 * kPitch + 4;
constexpr int kWarnW = 36;   // the ⚠ gamut chip over the preview's right end
constexpr int kChip = 14;    // swatch chip side
constexpr int kSwatchH = 2 * kChip + 6; // palette row + recents row
constexpr int swatchYFor(int hexY) {
    return hexY + kHexH + 6;
}
constexpr int pickerHFor(int hexY) {
    return swatchYFor(hexY) + kSwatchH + 8;
}
constexpr int kPickerH = pickerHFor(kHexYFor3);

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// Per-model slider configuration. H is degrees; S/L/V percent; Lab in CIELAB units (a/b signed).
struct RowSpec {
    const char* name;
    double min;
    double max;
};
constexpr RowSpec kRgbRows[3] = {{"R", 0, 255}, {"G", 0, 255}, {"B", 0, 255}};
constexpr RowSpec kHslRows[3] = {{"H", 0, 360}, {"S", 0, 100}, {"L", 0, 100}};
constexpr RowSpec kHsvRows[3] = {{"H", 0, 360}, {"S", 0, 100}, {"V", 0, 100}};
constexpr RowSpec kLabRows[3] = {{"L", 0, 100}, {"a", -128, 127}, {"b", -128, 127}};
constexpr RowSpec kCmykRows[4] = {{"C", 0, 100}, {"M", 0, 100}, {"Y", 0, 100}, {"K", 0, 100}};

} // namespace

// Two compact chip rows: a fixed starter palette on top, the most recent colours below
// (persisted via settings; recorded when the picker closes -- a drag is one colour decision,
// not thirty). Clicking a chip makes it the foreground; empty recent slots draw hollow.
class SwatchGrid : public Fl_Widget {
public:
    static constexpr int kPerRow = 12;

    SwatchGrid(int X, int Y, int W) : Fl_Widget(X, Y, W, kSwatchH) {}

    void setOnPick(std::function<void(common::Color8)> cb) { m_onPick = std::move(cb); }
    void setRecents(std::vector<common::Color8> r) {
        m_recents = std::move(r);
        if (m_recents.size() > kPerRow)
            m_recents.resize(kPerRow);
        redraw();
    }
    [[nodiscard]] const std::vector<common::Color8>& recents() const { return m_recents; }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        for (int row = 0; row < 2; ++row) {
            const int cy = y() + row * (kChip + 6);
            for (int col = 0; col < kPerRow; ++col) {
                const int cx = chipX(col);
                const common::Color8* c = colorAt(row, col);
                fl_color(c != nullptr ? toFl(*c) : toFl(pal.controlBg));
                fl_rectf(cx, cy, kChip, kChip);
                fl_color(toFl(pal.border));
                fl_rect(cx, cy, kChip, kChip);
            }
        }
    }

    int handle(int event) override {
        if (event != FL_PUSH)
            return Fl_Widget::handle(event);
        const int row = (Fl::event_y() - y()) / (kChip + 6);
        const int col = colAt(Fl::event_x());
        if (col < 0 || row < 0 || row > 1)
            return 1;
        if (const common::Color8* c = colorAt(row, col); c != nullptr && m_onPick)
            m_onPick(*c);
        return 1;
    }

private:
    // Chips are spread so the first chip's left edge and the last chip's right edge align
    // exactly with the picker's other rows (the fixed-pitch layout left the grid a few px short
    // of the right edge -- user feedback, 2026-06).
    [[nodiscard]] int chipX(int col) const {
        return x() + (w() - kChip) * col / (kPerRow - 1);
    }
    // The chip column containing screen x `ex`, or -1 (clicks in the inter-chip gaps miss).
    [[nodiscard]] int colAt(int ex) const {
        for (int col = 0; col < kPerRow; ++col) {
            if (ex >= chipX(col) && ex < chipX(col) + kChip)
                return col;
        }
        return -1;
    }

    [[nodiscard]] const common::Color8* colorAt(int row, int col) const {
        static constexpr common::Color8 kPalette[kPerRow] = {
            {0, 0, 0, 255},      {255, 255, 255, 255}, {128, 128, 128, 255},
            {230, 57, 70, 255},  {243, 146, 55, 255},  {255, 209, 102, 255},
            {106, 176, 76, 255}, {78, 205, 196, 255},  {69, 123, 157, 255},
            {155, 89, 182, 255}, {214, 93, 177, 255},  {141, 110, 99, 255}};
        if (row == 0)
            return &kPalette[col];
        return static_cast<std::size_t>(col) < m_recents.size()
                   ? &m_recents[static_cast<std::size_t>(col)]
                   : nullptr;
    }

    std::vector<common::Color8> m_recents;
    std::function<void(common::Color8)> m_onPick;
};

ColorPicker::ColorPicker(ColorState& colors)
    : Popover(kPickerW + bodyLeftMargin(), kPickerH), m_colors(colors),
      m_engine(std::make_unique<core::ColorEngine>(core::ColorSpace::SRGB)) {
    m_lab = m_engine->toLab(m_colors.foreground());
    begin();
    // All content is laid out in body-local coords (left edge at 0) inside this group; with the bubble
    // the group is shifted right by kBubbleTri to make room for the triangle margin (so the kPad-based
    // layout below is untouched, and layoutRows() re-pins the moved rows with the same offset). Without
    // the bubble (native Wayland) it stays put and the window is just kPickerW wide.
    auto* content = new Fl_Group(0, 0, kPickerW, kPickerH);
    content->box(FL_NO_BOX);
    content->begin();

    // Colour preview at the top: a flat fill with a hairline frame.
    m_preview = new Fl_Box(kPad, kPad, kPickerW - 2 * kPad, kPreviewH);
    m_preview->box(FL_BORDER_BOX);
    m_preview->color(toFl(m_colors.foreground()));

    // The out-of-gamut warning chip (§9 S12 semantics): a ⚠ over the preview's right end, filled
    // with the nearest in-gamut colour; clicking adopts it. Hidden while the colour is in gamut --
    // which is always, except while editing in Lab (the only S12 route to an impossible colour).
    m_gamutWarn = new Fl_Button(kPickerW - kPad - kWarnW, kPad, kWarnW, kPreviewH,
                                "\xE2\x9A\xA0"); // U+26A0 WARNING SIGN
    m_gamutWarn->box(FL_BORDER_BOX);
    m_gamutWarn->labelsize(12);
    m_gamutWarn->tooltip(_("Outside the working-space gamut — click to snap to the nearest color"));
    m_gamutWarn->callback([](Fl_Widget*, void* u) { static_cast<ColorPicker*>(u)->onGamutSnap(); },
                          this);
    m_gamutWarn->hide();

    // The picking surfaces -- one visible at a time (setSurface): the SV field + hue strip, and
    // the two wheels. All three share the same region, so swapping never resizes the popover.
    m_field = new SvField(kPad, kFieldY, kFieldW, kSurfH);
    m_field->callback([](Fl_Widget*, void* u) { static_cast<ColorPicker*>(u)->onFieldEdited(); },
                      this);
    m_strip = new HueStrip(kPad + kFieldW + kGap, kFieldY, kStripW, kSurfH);
    m_strip->callback([](Fl_Widget*, void* u) { static_cast<ColorPicker*>(u)->onHueEdited(); },
                      this);
    m_wheelTri = new ColorWheel(kPad, kFieldY, kPickerW - 2 * kPad, kSurfH,
                                ColorWheel::Style::Triangle);
    m_wheelTri->callback(
        [](Fl_Widget* w, void* u) { static_cast<ColorPicker*>(u)->onWheelEdited(w); }, this);
    m_wheelSq = new ColorWheel(kPad, kFieldY, kPickerW - 2 * kPad, kSurfH,
                               ColorWheel::Style::Square);
    m_wheelSq->callback(
        [](Fl_Widget* w, void* u) { static_cast<ColorPicker*>(u)->onWheelEdited(w); }, this);

    // The surface combo; sits on the left of the model combo (each combo nearest what it
    // controls: surface -> the picking surface above, model -> the slider rows below).
    m_surfaceCombo = new Dropdown(kPad, kComboY, kSurfaceComboW, kRowH);
    m_surfaceCombo->add("Field");
    m_surfaceCombo->add("HSL Wheel");
    m_surfaceCombo->add("SV Wheel");
    m_surfaceCombo->value(static_cast<int>(m_surface));
    m_surfaceCombo->callback(
        [](Fl_Widget*, void* u) { static_cast<ColorPicker*>(u)->onSurfaceChanged(); }, this);

    // The colour-model combo (Lab + CMYK join in S12-b with lcms2; "Hex" was cut in review --
    // the hex field below is permanent, so a Hex mode added nothing).
    m_modelCombo = new Dropdown(kPickerW - kPad - kComboW, kComboY, kComboW, kRowH);
    m_modelCombo->callback([](Fl_Widget*, void* u) { static_cast<ColorPicker*>(u)->onModelChanged(); },
                           this);
    refreshModelCombo();

    // Three channel rows (label + slider + numeric readout), relabelled per model.
    const int sx = kPad + kLabelW + 4;           // slider left
    const int rox = kPickerW - kPad - kReadoutW; // readout left
    const int sw = rox - 4 - sx;                 // slider width
    int rowY = kRowsY;
    for (int i = 0; i < 4; ++i) { // 4th row serves CMYK only; layoutRows() hides it otherwise
        const auto idx = static_cast<std::size_t>(i);
        auto* label = new Fl_Box(kPad, rowY, kLabelW, kRowH);
        label->box(FL_NO_BOX);
        label->labelfont(FL_HELVETICA);
        label->labelsize(12);
        label->labelcolor(FL_INACTIVE_COLOR); // = palette textMuted; follows a runtime re-theme
        label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        auto* slider = new Slider(sx, rowY, sw, kRowH);
        slider->step(1);
        slider->when(FL_WHEN_CHANGED);
        slider->callback([](Fl_Widget*, void* u) { static_cast<ColorPicker*>(u)->onChannelEdited(); },
                         this);

        // Editable numeric readout, styled like the hex field (review feedback: typing a value
        // should work everywhere a value is shown).
        auto* readout = new IntInput(rox, rowY + 1, kReadoutW, kRowH - 2); // themed right-click menu
        readout->box(MOSAIC_INPUT_BOX); // hairline frame + text padding (no kissing the outline)
        readout->color(FL_BACKGROUND2_COLOR); // semantic = controlBg/text; follow a runtime re-theme
        readout->textcolor(FL_FOREGROUND_COLOR);
        readout->cursor_color(FL_FOREGROUND_COLOR);
        readout->textfont(FL_SCREEN);
        readout->textsize(12);
        readout->when(FL_WHEN_CHANGED);
        readout->callback(
            [](Fl_Widget* w, void* u) { static_cast<ColorPicker*>(u)->onReadoutEdited(w); }, this);

        m_label[idx] = label;
        m_channel[idx] = slider;
        m_readout[idx] = readout;
        rowY += kPitch;
    }

    // Redesigned hex input (PLAN §9 S12; now the shared ui::HexField): one framed row whose '#' is a
    // fixed prefix glyph, monospace hex digits in the editable part. The value carries no '#'; pasting
    // a "#RRGGBB" still works (parseHexColor strips it).
    auto* hexRow = new HexField(kPad, kHexYFor3, kPickerW - 2 * kPad, kHexH);
    m_hexRow = hexRow;
    m_hex = hexRow->input();
    m_hex->when(FL_WHEN_CHANGED);
    m_hex->callback([](Fl_Widget*, void* u) { static_cast<ColorPicker*>(u)->onHexEdited(); }, this);

    // Starter palette + recent colours (S12-b): one click to re-pick either.
    m_swatches = new SwatchGrid(kPad, swatchYFor(kHexYFor3), kPickerW - 2 * kPad);
    m_swatches->setOnPick([this](common::Color8 c) {
        m_colors.setForeground(c); // an External change: the whole picker re-syncs to it
    });

    content->end();
    if (bodyLeftMargin() > 0)
        content->position(bodyLeftMargin(), 0); // shift the body right by the triangle margin
    end();
    enableBubble(); // Popover draws the bubble + balances the margins (host sets the insets)
    applyModelToSliders();
    layoutRows();
    setSurface(m_surface);
    syncFromState();
}

void ColorPicker::layoutRows() {
    const bool four = m_model == Model::Cmyk;
    for (int i = 0; i < 4; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        const bool shown = i < (four ? 4 : 3);
        if (shown) {
            m_label[idx]->show();
            m_channel[idx]->show();
            m_readout[idx]->show();
        } else {
            m_label[idx]->hide();
            m_channel[idx]->hide();
            m_readout[idx]->hide();
        }
    }
    const int hexY = four ? kHexYFor4 : kHexYFor3;
    // The body is shifted right by the triangle margin, so re-pin the moved rows there too.
    const int dx = bodyLeftMargin();
    m_hexRow->position(dx + kPad, hexY); // children ride along (same-size Fl_Group::resize translates)
    m_swatches->position(dx + kPad, swatchYFor(hexY));
    setBaseSize(kPickerW + dx, pickerHFor(hexY));
    reanchor(); // keep a shown popover pinned to its anchor at the new size
}

void ColorPicker::setWorkingSpace(core::ColorSpace cs) {
    if (m_customWorking) // a settings-supplied .icc working space outranks the document enum
        return;
    // A still-loaded document profile must not survive a plain enum request (the early-out
    // below compares the CTOR enum, which a profile-loaded engine still reports).
    if (m_engine && m_engine->workingSpace() == cs && m_docProfilePath.empty())
        return;
    m_docProfilePath.clear();
    m_engine = std::make_unique<core::ColorEngine>(cs);
    refreshAfterEngineChange();
}

void ColorPicker::setWorkingSpace(core::ColorSpace cs, const std::string& iccPath) {
    if (iccPath.empty()) {
        setWorkingSpace(cs);
        return;
    }
    if (m_customWorking) // the settings-level override still wins (S12-c precedence)
        return;
    if (m_docProfilePath == iccPath)
        return; // already serving this document profile
    auto engine = std::make_unique<core::ColorEngine>(cs);
    if (engine->loadWorkingProfileFile(iccPath.c_str())) {
        m_engine = std::move(engine);
        m_docProfilePath = iccPath;
        refreshAfterEngineChange();
    } else { // the file moved / is not RGB: honest fallback to the document's enum
        m_docProfilePath.clear();
        m_engine = std::move(engine); // freshly built for `cs`, unmodified by the failed load
        refreshAfterEngineChange();
    }
}

std::string ColorPicker::workingName() const {
    return m_engine->workingName();
}

ColorPicker::ProfileLoad ColorPicker::applyProfileSettings(const std::string& workingPath,
                                                           const std::string& cmykPath) {
    ProfileLoad result;
    bool changed = false;
    if (!workingPath.empty()) {
        result.workingOk = m_engine->loadWorkingProfileFile(workingPath.c_str());
        if (result.workingOk) {
            m_customWorking = true;
            changed = true;
        }
    }
    if (!cmykPath.empty()) {
        result.cmykOk = m_engine->loadCmykProfileFile(cmykPath.c_str());
        changed = changed || result.cmykOk;
    }
    if (changed)
        refreshAfterEngineChange();
    return result;
}

void ColorPicker::resetCmykToDefault() {
    if (m_engine && m_engine->loadDefaultCmykProfile())
        refreshAfterEngineChange();
}

void ColorPicker::refreshModelCombo() {
    m_modelCombo->clear();
    m_modelCombo->add("HSL");
    m_modelCombo->add("RGB");
    m_modelCombo->add("HSV");
    m_modelCombo->add("Lab");
    if (m_engine->hasCmyk()) // absent when no press profile is available (strippable default)
        m_modelCombo->add("CMYK");
    else if (m_model == Model::Cmyk)
        setModel(Model::Rgb); // the active model just lost its profile: fall back
    m_modelCombo->value(static_cast<int>(m_model));
}

void ColorPicker::refreshAfterEngineChange() {
    const common::Color8 fg = m_colors.foreground();
    m_lab = m_engine->toLab(fg);
    if (m_engine->hasCmyk())
        m_cmyk = m_engine->toCmyk(fg);
    m_outOfGamut = false;
    refreshModelCombo();
    applyModelToSliders();
    updateGamutUi();
    redraw();
}

void ColorPicker::hide() {
    // Record this picker session's final colour as a recent (newest first, deduped, one row max)
    // before dismissing; shown() guards teardown/no-op hides.
    if (shown() && m_swatches != nullptr) {
        const common::Color8 fg = m_colors.foreground();
        std::vector<common::Color8> r = m_swatches->recents();
        if (r.empty() || !(r.front() == fg)) {
            std::erase_if(r, [&](common::Color8 c) { return c == fg; });
            r.insert(r.begin(), fg);
            if (r.size() > SwatchGrid::kPerRow)
                r.resize(SwatchGrid::kPerRow);
            m_swatches->setRecents(r);
            if (m_onRecentsChange) {
                std::vector<std::string> hex;
                hex.reserve(r.size());
                for (common::Color8 c : r)
                    hex.push_back(hexString(c));
                m_onRecentsChange(hex);
            }
        }
    }
    Popover::hide();
}

void ColorPicker::setRecentColors(const std::vector<std::string>& hex) {
    std::vector<common::Color8> r;
    for (const std::string& h : hex)
        if (const std::optional<common::Color8> c = parseHexColor(h))
            r.push_back(*c);
    m_swatches->setRecents(std::move(r));
}

void ColorPicker::onGamutSnap() {
    // The foreground already holds the clamped colour (Color8 forces commit-time clamping); the
    // snap just makes it the *working* Lab value too, retiring the warning.
    m_lab = m_engine->toLab(m_colors.foreground());
    m_outOfGamut = false;
    applyModelToSliders();
    updateGamutUi();
    redraw();
}

void ColorPicker::updateGamutUi() {
    if (m_outOfGamut) {
        m_gamutWarn->color(toFl(m_colors.foreground())); // = the nearest in-gamut colour
        m_gamutWarn->show();
    } else {
        m_gamutWarn->hide();
    }
}

void ColorPicker::setSurface(Surface s) {
    m_surface = s;
    m_surfaceCombo->value(static_cast<int>(s));
    const bool field = s == Surface::Field;
    field ? m_field->show() : m_field->hide();
    field ? m_strip->show() : m_strip->hide();
    s == Surface::WheelTriangle ? m_wheelTri->show() : m_wheelTri->hide();
    s == Surface::WheelSquare ? m_wheelSq->show() : m_wheelSq->hide();
    redraw();
}

void ColorPicker::onSurfaceChanged() {
    setSurface(static_cast<Surface>(m_surfaceCombo->value()));
    if (m_onSurfaceChange)
        m_onSurfaceChange(m_surface); // user-initiated: let the owner persist the choice
}

std::optional<ColorPicker::Surface> parsePickerSurface(std::string_view key) {
    if (key == "field")
        return ColorPicker::Surface::Field;
    if (key == "hsl-wheel")
        return ColorPicker::Surface::WheelTriangle;
    if (key == "sv-wheel")
        return ColorPicker::Surface::WheelSquare;
    return std::nullopt;
}

const char* pickerSurfaceKey(ColorPicker::Surface s) {
    switch (s) {
    case ColorPicker::Surface::WheelTriangle:
        return "hsl-wheel";
    case ColorPicker::Surface::WheelSquare:
        return "sv-wheel";
    case ColorPicker::Surface::Field:
        break;
    }
    return "field";
}

void ColorPicker::onWheelEdited(Fl_Widget* who) {
    const auto* wheel = static_cast<const ColorWheel*>(who);
    m_h = wheel->hue();
    m_s = wheel->sat();
    m_v = wheel->val();
    pushForeground(hsvToRgb({m_h, m_s, m_v}), Source::FieldOrStrip);
}

void ColorPicker::showFor(const Fl_Widget* anchor) {
    syncFromState();
    showAnchored(anchor); // window-relative placement next to the swatch (identical on every backend)
}

void ColorPicker::syncFromState() {
    const common::Color8 fg = m_colors.foreground();

    // Re-derive the working hue/sat/val only for changes born outside the picker's own controls
    // (swatch swap/reset, and complete hex entries -- a hex value *is* a full new colour). Keeping
    // them otherwise is what stops the hue collapsing through grey/black round-trips.
    if (m_source == Source::External || m_source == Source::Hex) {
        const Hsv t = rgbToHsv(fg);
        if (t.s > 0.0F && t.v > 0.0F)
            m_h = t.h;
        m_s = t.s;
        m_v = t.v;
    }

    // Working Lab value (S12-b): re-derived from the foreground -- in gamut by construction --
    // for every change except a Lab slider/readout edit, which set it (possibly out of gamut)
    // itself in onChannelEdited.
    if (!(m_source == Source::Sliders && m_model == Model::Lab)) {
        m_lab = m_engine->toLab(fg);
        m_outOfGamut = false;
    }
    // Same stability rule for the working CMYK (round-trips through a press profile drift).
    if (m_engine->hasCmyk() && !(m_source == Source::Sliders && m_model == Model::Cmyk))
        m_cmyk = m_engine->toCmyk(fg);
    updateGamutUi();

    if (m_source != Source::Sliders) {
        applyModelToSliders();
    } else { // the dragged slider / typed readout is authoritative; refresh the readouts only
        for (int i = 0; i < 4; ++i) {
            if (m_editingReadout == i)
                continue; // never rewrite the input under the user's caret
            const auto idx = static_cast<std::size_t>(i);
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d",
                          static_cast<int>(std::lround(m_channel[idx]->value())));
            m_readout[idx]->value(buf);
        }
    }

    m_field->set(m_h, m_s, m_v);
    m_strip->set(m_h);
    m_wheelTri->set(m_h, m_s, m_v);
    m_wheelSq->set(m_h, m_s, m_v);

    // Rewrite the hex field for any change *except* one the user is typing into it (which would
    // clobber the in-progress text + caret). The '#' lives in the prefix glyph, not the text.
    if (!m_editingHex)
        m_hex->value(hexString(fg).c_str() + 1);
    m_preview->color(toFl(fg));
    m_preview->redraw();
    redraw();

    m_source = Source::External;
}

void ColorPicker::applyModelToSliders() {
    const common::Color8 fg = m_colors.foreground();
    const RowSpec* spec = kRgbRows;
    int rows = 3;
    double values[4] = {double(fg.r), double(fg.g), double(fg.b), 0.0};
    switch (m_model) {
    case Model::Hsl: {
        spec = kHslRows;
        const Hsl hsl = rgbToHsl(fg);
        values[0] = m_h; // shared hue: HSL hue == HSV hue, and m_h survives grey round-trips
        values[1] = double{hsl.s} * 100.0;
        values[2] = double{hsl.l} * 100.0;
        break;
    }
    case Model::Hsv:
        spec = kHsvRows;
        values[0] = m_h;
        values[1] = double{m_s} * 100.0;
        values[2] = double{m_v} * 100.0;
        break;
    case Model::Lab:
        spec = kLabRows;
        values[0] = double{m_lab.l}; // the working (possibly out-of-gamut) Lab, not a re-derive
        values[1] = double{m_lab.a};
        values[2] = double{m_lab.b};
        break;
    case Model::Cmyk:
        spec = kCmykRows;
        rows = 4;
        values[0] = double{m_cmyk.c}; // working CMYK, same stability rule as Lab
        values[1] = double{m_cmyk.m};
        values[2] = double{m_cmyk.y};
        values[3] = double{m_cmyk.k};
        break;
    case Model::Rgb:
        break;
    }
    for (int i = 0; i < rows; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        m_label[idx]->copy_label(spec[i].name);
        m_channel[idx]->range(spec[i].min, spec[i].max);
        m_channel[idx]->value(values[i]);
        if (m_editingReadout != i) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(values[i])));
            m_readout[idx]->value(buf);
        }
        m_channel[idx]->redraw();
    }
}

void ColorPicker::onModelChanged() {
    setModel(static_cast<Model>(m_modelCombo->value()));
}

void ColorPicker::setModel(Model m) {
    if (m == m_model)
        return;
    m_model = m;
    applyModelToSliders();
    layoutRows();
    redraw();
}

void ColorPicker::pushForeground(common::Color8 c, Source src) {
    // Writing the foreground notifies observers; the swatch's observer calls our syncFromState(),
    // which uses m_source to refresh exactly the widgets that did not originate the change.
    m_source = src;
    const bool changed = !(c == m_colors.foreground());
    m_colors.setForeground(c);
    if (!changed) {
        // A no-op push fires no observers, but the picker's *working* hue/sat/val did change --
        // every hue at s == 0 is the same grey (user-reported: hue drags / H edits on greys froze
        // the field, readouts, and sliders). Sync ourselves; m_source is still set, so the
        // originating widget is left alone exactly as for a real change.
        syncFromState();
    }
    m_source = Source::External;
}

void ColorPicker::onChannelEdited() {
    const double v0 = m_channel[0]->value();
    const double v1 = m_channel[1]->value();
    const double v2 = m_channel[2]->value();
    const double v3 = m_channel[3]->value(); // meaningful in CMYK mode only
    switch (m_model) {
    case Model::Rgb: {
        const common::Color8 c{static_cast<std::uint8_t>(std::lround(v0)),
                               static_cast<std::uint8_t>(std::lround(v1)),
                               static_cast<std::uint8_t>(std::lround(v2)), 255};
        const Hsv t = rgbToHsv(c);
        if (t.s > 0.0F && t.v > 0.0F)
            m_h = t.h;
        m_s = t.s;
        m_v = t.v;
        pushForeground(c, Source::Sliders);
        break;
    }
    case Model::Hsl: {
        m_h = static_cast<float>(v0);
        const common::Color8 c = hslToRgb(
            {m_h, static_cast<float>(v1) / 100.0F, static_cast<float>(v2) / 100.0F});
        const Hsv t = rgbToHsv(c);
        m_s = t.s;
        m_v = t.v;
        pushForeground(c, Source::Sliders);
        break;
    }
    case Model::Hsv:
        m_h = static_cast<float>(v0);
        m_s = static_cast<float>(v1) / 100.0F;
        m_v = static_cast<float>(v2) / 100.0F;
        pushForeground(hsvToRgb({m_h, m_s, m_v}), Source::Sliders);
        break;
    case Model::Lab: {
        m_lab = {static_cast<float>(v0), static_cast<float>(v1), static_cast<float>(v2)};
        m_outOfGamut = !core::ColorEngine::inGamut(m_engine->toRgbUnclamped(m_lab));
        const common::Color8 c = m_engine->toRgbClamped(m_lab); // commit-time clamp (§9 S12)
        const Hsv t = rgbToHsv(c);
        if (t.s > 0.0F && t.v > 0.0F)
            m_h = t.h;
        m_s = t.s;
        m_v = t.v;
        pushForeground(c, Source::Sliders);
        break;
    }
    case Model::Cmyk: {
        m_cmyk = {static_cast<float>(v0), static_cast<float>(v1), static_cast<float>(v2),
                  static_cast<float>(v3)};
        const common::Color8 c = m_engine->cmykToRgb(m_cmyk);
        const Hsv t = rgbToHsv(c);
        if (t.s > 0.0F && t.v > 0.0F)
            m_h = t.h;
        m_s = t.s;
        m_v = t.v;
        pushForeground(c, Source::Sliders);
        break;
    }
    }
}

void ColorPicker::onReadoutEdited(Fl_Widget* who) {
    int hit = -1;
    for (int i = 0; i < 4; ++i)
        if (m_readout[static_cast<std::size_t>(i)] == who)
            hit = i;
    if (hit < 0)
        return;
    const auto idx = static_cast<std::size_t>(hit);
    const char* text = m_readout[idx]->value();
    if (text == nullptr || *text == '\0')
        return; // mid-edit emptiness: don't snap the channel to 0 under the user's caret
    const double raw = std::atof(text);
    const double v = std::clamp(raw, m_channel[idx]->minimum(), m_channel[idx]->maximum());
    if (raw != v) {
        // Out-of-range entry snaps back immediately (e.g. typing 1000 in RGB becomes 255) -- the
        // one case where rewriting under the caret is the *expected* behaviour (user-reported).
        char buf[8];
        const int len = std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(v)));
        m_readout[idx]->value(buf);
        m_readout[idx]->insert_position(len);
    }
    m_channel[idx]->value(v);
    m_editingReadout = hit;
    onChannelEdited(); // reads all three sliders and pushes, exactly like a slider drag
    m_editingReadout = -1;
}

void ColorPicker::onFieldEdited() {
    m_s = m_field->sat();
    m_v = m_field->val();
    pushForeground(hsvToRgb({m_h, m_s, m_v}), Source::FieldOrStrip);
}

void ColorPicker::onHueEdited() {
    m_h = m_strip->hue();
    pushForeground(hsvToRgb({m_h, m_s, m_v}), Source::FieldOrStrip);
}

void ColorPicker::onHexEdited() {
    const char* text = m_hex->value();
    if (const std::optional<common::Color8> c = parseHexColor(text != nullptr ? text : "")) {
        // Mark the change as hex-originated so the resulting syncFromState leaves the field's text
        // (and caret) untouched; only a complete, valid hex applies -- partial text is ignored.
        m_editingHex = true;
        pushForeground(*c, Source::Hex);
        m_editingHex = false;
    }
}

} // namespace mosaic::ui
