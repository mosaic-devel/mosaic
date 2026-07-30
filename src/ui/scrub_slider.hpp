#pragma once

#include "common/image.hpp" // Color8
#include "ui/popover.hpp"    // ScrubRuler is a Popover (reuses the comic-book bubble + sub-window)
#include "ui/theme.hpp"      // ThemeSubscription

#include <FL/Fl_Valuator.H>

#include <functional>
#include <string>

// A wide, floating-point, Krita-style value slider tailored for the tool options bar, plus its
// companion "precision ruler" HUD. Unlike the project-wide ui::Slider (a plain click-to-position
// track used in dialogs and the Settings panes) this one is built for fine sub-pixel authoring:
//
//   * the numeric value is drawn ON the bar (no separate readout box) and a click-without-drag
//     opens an inline type-in;
//   * a drag scrubs *relatively* from the current value (no jump-to-click);
//   * pulling the cursor away from the bar mid-drag engages a precision mode that divides the
//     sensitivity down (the farther away, the finer) and floats a curved ruler whose ticks spread
//     apart to show the gain -- the arc bows toward the direction the cursor went;
//   * Shift snaps to a coarse grid (snapStep); the mouse wheel nudges by step (Shift = coarse);
//   * a middle-click (or Ctrl+click) resets to a configured default;
//   * an optional non-linear response curve gives the low end of the range most of the track, so
//     small values (a 2 px brush on a 1..1000 range) are reachable without the precision gesture.
//
// The value math is split into the GUI-free scrub_detail:: namespace so it is unit-testable without
// an X server (ui/scrub_slider tests). The widget itself only wires those onto Fl_Valuator's
// value()/range()/step()/callback() machinery -- a drop-in for ui::Slider in the options bar.
namespace mosaic::ui {

// Maps the normalized track position [0,1] to/from a value across the range.
enum class ScrubCurve {
    Linear, // value is uniform across the track
    Gamma,  // value = mn + (mx-mn)*t^k -- k>1 gives the low end most of the track
    Log     // value = mn*(mx/mn)^t -- geometric; requires mn>0 (falls back to Linear otherwise)
};

// ---- Pure value model (GUI-free; unit-tested in tests/test_scrub_slider.cpp) ------------------
namespace scrub_detail {

// Normalized track fraction t in [0,1] -> value in [mn,mx] under the response curve.
[[nodiscard]] double curveToValue(double t, ScrubCurve curve, double k, double mn, double mx);

// Inverse of curveToValue: value -> normalized track fraction in [0,1].
[[nodiscard]] double valueToTrack(double v, ScrubCurve curve, double k, double mn, double mx);

// Local sensitivity at value v under the curve: how many value units one track pixel covers there
// (central difference over the curve, floored above zero so a Gamma low end never fully stalls).
[[nodiscard]] double unitsPerPixel(double v, ScrubCurve curve, double k, double mn, double mx,
                                   int trackPx);

// Precision factor in [0,1]: 0 while |dy| is within the deadzone, ramping linearly to 1 over the
// next `rampPx` pixels and clamped there. `dyAbs` is the cursor's vertical distance from the bar.
[[nodiscard]] double precision01(int dyAbs, int deadzonePx, int rampPx);

// Sensitivity divisor for a precision factor: 1 (no precision) easing up to `maxDiv` (finest). The
// integrated drag uses unitsPerPixel / gainDivisor, so a bigger divisor == slower == finer.
[[nodiscard]] double gainDivisor(double p01, double maxDiv);

// Round v to the nearest multiple of `snap` (snap <= 0 -> v unchanged).
[[nodiscard]] double snapTo(double v, double snap);

// Round v to the nearest multiple of `step` and clamp to [mn,mx] (step <= 0 -> just clamp).
[[nodiscard]] double quantize(double v, double step, double mn, double mx);

// Format like the options-bar readout: an integer when step >= 1, else two decimals, plus suffix.
[[nodiscard]] std::string format(double v, double step, const std::string& suffix);

} // namespace scrub_detail

class ScrubRuler; // the precision HUD, below

class ScrubSlider : public Fl_Valuator {
public:
    ScrubSlider(int X, int Y, int W, int H, const char* label = nullptr);
    ~ScrubSlider() override;

    // Readout unit shown after the value on the bar / in the ruler (e.g. "px"); empty = none.
    void setSuffix(std::string s);
    // Non-linear track response (see ScrubCurve). k is the Gamma exponent (ignored for Linear/Log).
    void setResponseCurve(ScrubCurve c, double k = 2.0);
    // Coarse grid the Shift modifier snaps to while dragging; <= 0 derives a sensible grid from the
    // range and step.
    void setSnapStep(double s);
    // Value a middle-click / Ctrl-click resets to. Unset -> reset is disabled.
    void setDefaultValue(double v);
    // The ground this slider clears its cell to (parity with ui::Slider::setCellColor): set when the
    // slider sits on a non-panelBg ground so its cell blends in.
    void setCellColor(common::Color8 c);
    // The shared precision-ruler overlay (owned by the main window, injected via the options bar).
    // Non-owning; null disables the ruler (the precision drag still works, just without the HUD).
    void setRuler(ScrubRuler* r) { m_ruler = r; }
    // Optional value-ramp strip (S32 pro controls): a thin per-pixel colored band along the
    // bar's bottom edge sampling `fn(t)` for t in [0,1] across the track -- the Hue slider's
    // spectrum, a saturation gray-to-vivid ramp, a lightness black-to-white ramp. Unset (the
    // default) draws nothing and the slider renders exactly as before.
    void setTrackFill(std::function<common::Color8(double)> fn) { m_trackFill = std::move(fn); }

protected:
    void draw() override;
    int handle(int event) override;

private:
    // Lets the main window commit our in-place edit when a click lands outside us (chrome / another
    // control), without exposing the edit internals.
    friend void commitActiveScrubEditOnOutsideClick(int winX, int winY);
    [[nodiscard]] int trackX0() const;
    [[nodiscard]] int trackW() const;
    [[nodiscard]] double effectiveSnap() const;
    [[nodiscard]] common::Color8 cellColor() const;
    [[nodiscard]] double trackFractionAt(int eventX) const; // [0,1] for a cursor x over the track

    void pushAndDrag(double v); // route a value through Fl_Valuator (clamp/step/when/callback)
    void updateRuler();         // push the live gesture state to the shared overlay

    // --- in-place value editing (no pop-up widget: the slider draws + edits its own value text) ---
    void beginEdit();   // enter edit mode, seed the buffer from the current value, select all
    void commitEdit();  // parse the buffer, write the value, leave edit mode
    void cancelEdit();  // discard the buffer, leave edit mode
    void endEditMode(); // shared teardown (stop the caret blink, drop focus, redraw)
    [[nodiscard]] bool hasSelection() const { return m_selAnchor != m_caret; }
    void deleteSelection();           // remove the selected span (no-op if none); caret -> span start
    void editInsert(const char* s);   // insert numeric chars at the caret (replacing any selection)
    void editSelectAll();
    void editClipboard(bool cut);     // copy (or cut) the selection / whole buffer to the clipboard
    void caretFromX(int eventX);      // place the caret at the click x within the editable text
    void openValueContextMenu();      // the themed right-click menu, acting on the edit buffer
    static void blinkCb(void* self);

    std::string m_suffix;
    ScrubCurve m_curve = ScrubCurve::Linear;
    double m_curveK = 2.0;
    double m_snapStep = 0.0;
    double m_default = 0.0;
    bool m_hasDefault = false;
    common::Color8 m_cellColor{};
    bool m_cellColorSet = false;

    bool m_hover = false;
    // --- live drag state ---
    bool m_dragging = false;  // a button is down on us
    bool m_moved = false;     // moved far enough to count as a scrub (else a click opens the editor)
    bool m_precision = false; // crossed the deadzone this drag -> the ruler is up
    int m_pushX = 0;
    int m_pushY = 0;          // FL_PUSH position (window coords); dy is measured from here
    int m_lastX = 0;          // last drag x, for the per-frame delta
    double m_accum = 0.0;     // continuous (un-stepped) value the drag integrates into
    double m_pxPerUnit = 0.0; // current screen pixels per value unit (ruler tick spacing)

    // --- edit-mode state ---
    bool m_editing = false;
    std::string m_editBuf;   // the digits being typed (no suffix)
    int m_caret = 0;         // caret index into m_editBuf
    int m_selAnchor = 0;     // selection anchor; the span is [min(anchor,caret), max(anchor,caret)]
    bool m_caretOn = true;   // caret blink phase

    ScrubRuler* m_ruler = nullptr; // shared overlay, non-owning
    std::function<common::Color8(double)> m_trackFill; // optional value-ramp strip (unset = off)
    ThemeSubscription m_themeSub;
};

// State the active ScrubSlider hands the ruler each frame (value/screen units; the ruler is
// otherwise stateless presentation).
struct RulerState {
    double value = 0.0;
    double minVal = 0.0;
    double maxVal = 1.0;
    double step = 1.0;        // the slider's value step (>=1 -> integer slider: no fractional ticks)
    double snapStep = 0.0;
    double pxPerUnit = 0.0;   // screen pixels per value unit (tick spacing basis) at the current gain
    double precision01 = 0.0; // 0..1 depth into precision mode
    bool snapping = false;    // Shift held -> emphasise the snap-grid ticks
    std::string text;         // the formatted value, shown large
};

// The precision ruler: a comic-book speech bubble (a ui::Popover, so it reuses the backend-gated
// triangle + shape() transparency + child-sub-window machinery) that pops UP-pointing from the middle
// of the active slider during a precision drag. It is NOT bound to the cursor -- it stays anchored to
// the slider, growing a little and drifting ever so slightly toward the cursor as precision deepens.
// One instance is shared by every options-bar slider (created by the main window before show()).
class ScrubRuler : public Popover {
public:
    ScrubRuler();

    // Show/update the ruler. It is placed ONCE (centred under `slider`, a sibling child of the main
    // window) when it first appears for a drag and then only its content updates -- Xwayland does not
    // re-realise a shaped sub-window's geometry after show(), so resizing/moving it mid-drag breaks the
    // visible window + triangle. Hence a fixed size, and no grow/nudge.
    void present(const Fl_Widget* slider, const RulerState& st);

    void hide() override;

protected:
    void drawContent() override; // the bubble chrome (Popover::drawContent) + the tick fan in the body

private:
    RulerState m_st;
    double m_smoothPxPerUnit = 0.0; // eased toward st.pxPerUnit to calm the tick-LOD snapping
    double m_lastTickUnit = 0.0;    // hysteresis: the labelled-major unit only changes when clearly
                                    // out of band, so tiny vertical jitter can't flicker the labels
    bool m_live = false;            // currently presenting (drives the smoothing reset)
};

// Commit the slider whose value is being edited in place if (winX, winY) -- main-window coordinates --
// is outside it. The main window calls this on every FL_PUSH it sees (like the popover dismissals), so
// clicking any chrome / another control closes the field. A click inside the field, or on its own
// right-click menu (a separate sub-window the main window never sees), is spared.
void commitActiveScrubEditOnOutsideClick(int winX, int winY);

} // namespace mosaic::ui
