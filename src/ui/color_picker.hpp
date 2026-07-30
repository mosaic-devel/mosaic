#pragma once

#include "common/image.hpp" // Color8
#include "core/color_management.hpp" // ColorEngine, ColorSpace, Lab
#include "ui/popover.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Fl_Box;
class Fl_Button;
class Fl_Input;
class Fl_Int_Input;
class Fl_Group;
class Fl_RGB_Image;
class Fl_Widget;

// The colour-picker popover (PLAN S11-d stub, grown in S12-a). A compact, Affinity-style anchored
// popover (the shape the user settled on) editing the active *foreground* colour live:
//   - a colour preview;
//   - a switchable picking surface (review feedback, Affinity-style): the SV **field** + hue strip,
//     a hue **wheel with a rotating HSL triangle**, or a hue **wheel with an inscribed SV square**,
//     chosen by a surface combo (left of the model combo; each combo sits nearest what it controls);
//   - a colour-model combo -- HSL / RGB / HSV -- driving three relabelled slider rows, each row
//     with an editable numeric readout ("Hex" was cut from the combo in review: the hex field is
//     always present, so the entry was redundant);
//   - the redesigned hex input: a framed field with a fixed '#' prefix glyph and monospace
//     hex digits, paste-friendly (pasted text may include or omit the '#').
// Lab + CMYK + lcms2 colour management (indicator, gamut warning/snap, swatches/recents) are S12-b.
//
// Hue stability: the picker keeps its own working hue/sat/val (m_h/m_s/m_v) while the user edits,
// because round-tripping through RGB collapses hue at zero saturation/value (see color_models.hpp).
// Only changes that originate *outside* the picker re-derive them from the foreground RGB.
namespace mosaic::ui {

class ColorState;
class Dropdown;
class Slider;
class SvField;    // the saturation/value plane (defined in color_picker.cpp)
class HueStrip;   // the vertical hue gradient strip (defined in color_picker.cpp)
class ColorWheel; // hue ring + (rotating HSL triangle | inscribed SV square) (color_picker.cpp)
class SwatchGrid; // fixed starter palette + recent-colour chips (color_picker.cpp)

class ColorPicker : public Popover {
public:
    explicit ColorPicker(ColorState& colors);

    // Seed the controls from the current foreground colour, then position next to `anchor` and show.
    void showFor(const Fl_Widget* anchor);

    // The colour changed elsewhere (the swatch's X / reset / back-square swap): refresh the controls.
    void syncFromState();

    enum class Model { Hsl = 0, Rgb = 1, Hsv = 2, Lab = 3, Cmyk = 4 }; // combo order; CMYK is
                       // present only when the engine has a press profile (the vendored FOGRA39
                       // default, or an S12-c user profile) and uses a 4th slider row
    enum class Surface { Field = 0, WheelTriangle = 1, WheelSquare = 2 }; // combo order

    void setSurface(Surface s); // swap the picking surface (combo callback; settings-persisted)

    // Adopt a document's working colour space (S12-b): rebuilds the lcms2 engine and re-derives
    // the working Lab value. Called when the active document changes. A no-op while a
    // user-supplied working profile is active (S12-c: the settings profile wins).
    void setWorkingSpace(core::ColorSpace cs);

    // Same, but honouring a document-level ICC working profile (File -> New "Custom..."): a
    // non-empty `iccPath` loads over the enum's engine; a failed load (moved/deleted file, not
    // an RGB profile) falls back to the enum. The settings-level profile still wins over both.
    void setWorkingSpace(core::ColorSpace cs, const std::string& iccPath);

    // The active working-space name -- the built-in enum name or a loaded ICC profile's
    // description tag. Feeds the status bar's colour-space indicator (S13-b; the indicator's
    // interim S12-b home inside the picker moved there). The host re-queries this after
    // setWorkingSpace / applyProfileSettings.
    [[nodiscard]] std::string workingName() const;

    // Apply the settings-level ICC overrides (S12-c): an RGB working profile and/or a CMYK press
    // profile, each by path ("" = keep the built-in / vendored default). Returns whether each
    // requested load succeeded so the caller can log failures.
    struct ProfileLoad {
        bool workingOk = true;
        bool cmykOk = true;
    };
    ProfileLoad applyProfileSettings(const std::string& workingPath, const std::string& cmykPath);

    // Revert the CMYK profile to the vendored built-in default at runtime (Settings "Use default").
    void resetCmykToDefault();

    // Fired when the *user* switches the surface via the combo (not for programmatic setSurface):
    // the main window persists the choice to settings here.
    void setSurfaceChangedCallback(std::function<void(Surface)> cb) {
        m_onSurfaceChange = std::move(cb);
    }

    // Recents (S12-b): seeded from settings as "#RRGGBB" strings (unparseable entries dropped);
    // the callback fires with the updated list whenever closing the picker records a new one.
    void setRecentColors(const std::vector<std::string>& hex);
    void setRecentsChangedCallback(std::function<void(const std::vector<std::string>&)> cb) {
        m_onRecentsChange = std::move(cb);
    }

    void hide() override; // records the session's final colour as a recent, then dismisses

    // The comic-book speech bubble (triangle at the swatch, balanced margins, shape()-cut corners on
    // every platform) lives in Popover now; the picker just reserves the kBubbleTri left margin (below)
    // and calls enableBubble(). The host sets the layout references via Popover::setBubbleInsets().

private:
    // Where the in-flight change came from, so syncFromState refreshes exactly the widgets that
    // did NOT originate it (and re-derives the working hue/sat/val only for external changes).
    enum class Source { External, Sliders, FieldOrStrip, Hex };

    void setModel(Model m);
    void applyModelToSliders(); // relabel + re-range + re-value the slider rows for m_model
    void layoutRows();          // show/hide the 4th row + move the hex/indicator rows + resize
    void refreshModelCombo();   // rebuild combo entries (CMYK present only with a press profile)
    void refreshAfterEngineChange(); // re-derive working values + indicator + combo + rows

    void onModelChanged();
    void onSurfaceChanged();              // the surface combo switched
    void onChannelEdited();               // a slider moved
    void onReadoutEdited(Fl_Widget* who); // a numeric readout was typed into
    void onWheelEdited(Fl_Widget* who);   // a wheel's ring or inner shape was dragged
    void onGamutSnap();                   // the warning was clicked: adopt the nearest in-gamut
    void updateGamutUi();                 // show/hide + repaint the warning chip
    void onFieldEdited();   // the SV field was clicked/dragged (new s, v)
    void onHueEdited();     // the hue strip was clicked/dragged (new h)
    void onHexEdited();     // the hex field changed

    void pushForeground(common::Color8 c, Source src); // write state; routes back via syncFromState

    ColorState& m_colors;
    Model m_model = Model::Rgb;
    Surface m_surface = Surface::Field; // default settled with the user: the most familiar surface
    Source m_source = Source::External;

    float m_h = 0.0F; // working HSV (see header comment); m_h also serves HSL (same hue)
    float m_s = 0.0F;
    float m_v = 0.0F;

    // The S12-b colour management. m_lab is the *unclamped* working Lab value: while the user
    // edits in Lab mode it may lie outside the working space (that is the gamut warning); what
    // lands in ColorState is always the clamped colour (Color8 forces it -- §9 S12 semantics).
    std::unique_ptr<core::ColorEngine> m_engine;
    bool m_customWorking = false; // a settings-supplied .icc working space is active (S12-c):
                                  // document colour-space changes no longer rebuild the engine
    std::string m_docProfilePath; // the DOCUMENT-level .icc the engine currently serves ("" =
                                  // plain enum engine); setWorkingSpace variants keep it honest
    core::Lab m_lab{};
    core::Cmyk m_cmyk{}; // working CMYK, same edit-stability rule as m_lab (re-derive on
                         // external change only -- CMYK round-trips drift)
    bool m_outOfGamut = false;

    SvField* m_field = nullptr;
    HueStrip* m_strip = nullptr;
    ColorWheel* m_wheelTri = nullptr; // hue ring + rotating HSL triangle
    ColorWheel* m_wheelSq = nullptr;  // hue ring + inscribed SV square
    Dropdown* m_surfaceCombo = nullptr;
    Dropdown* m_modelCombo = nullptr;
    std::array<Fl_Box*, 4> m_label{}; // 4th row exists for CMYK only (hidden otherwise)
    std::array<Slider*, 4> m_channel{};
    std::array<Fl_Int_Input*, 4> m_readout{}; // editable, hex-field-style
    int m_editingReadout = -1; // readout index currently being typed into (its text is left
                               // alone by syncFromState, exactly like m_editingHex), -1 = none
    Fl_Group* m_hexRow = nullptr; // frame + '#' prefix glyph + the input, moved as one in Hex mode
    Fl_Input* m_hex = nullptr;
    Fl_Box* m_preview = nullptr;
    Fl_Button* m_gamutWarn = nullptr; // the ⚠ chip over the preview's right end (hidden in gamut)
    SwatchGrid* m_swatches = nullptr; // starter palette + recents, under the hex row
    std::function<void(Surface)> m_onSurfaceChange; // see setSurfaceChangedCallback
    std::function<void(const std::vector<std::string>&)> m_onRecentsChange;
    bool m_editingHex = false; // true while a hex *keystroke* drives the change, so syncFromState
                               // leaves the field's text alone (slider-driven changes still rewrite it)
};

// Surface <-> Settings::pickerSurface key ("field" | "hsl-wheel" | "sv-wheel"). Pure, unit-tested.
[[nodiscard]] std::optional<ColorPicker::Surface> parsePickerSurface(std::string_view key);
[[nodiscard]] const char* pickerSurfaceKey(ColorPicker::Surface s);

} // namespace mosaic::ui
