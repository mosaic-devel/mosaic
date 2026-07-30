#pragma once

#include <FL/Fl_Widget.H>

#include <cstdint>

// The two-diagonal-square active-colour swatch (PLAN S11-d). The foreground square overlaps the
// background square (foreground on top, toward the top-left); a small swap glyph sits in the
// top-right corner and a black/white default-reset glyph in the bottom-left. Interactions:
//   - click the foreground square  -> open / toggle the colour picker popover (edits the foreground)
//   - click the background square   -> swap foreground <-> background (promote the back colour)
//   - click the swap glyph (or X)   -> swap foreground <-> background
//   - click the reset glyph (or D)  -> reset to black / white
// It lives at the bottom of the left toolbar. The painting / fill tools (S19+) read the foreground
// from the shared ColorState.
namespace mosaic::ui {

class ColorState;
class ColorPicker;

// Which part of the swatch a local point falls in (pure geometry, so it is unit-tested without FLTK).
enum class SwatchRegion : std::uint8_t { None, Foreground, Background, Swap, Reset };
[[nodiscard]] SwatchRegion swatchHitRegion(int localX, int localY, int w, int h);

class ColorSwatch : public Fl_Widget {
public:
    ColorSwatch(int X, int Y, int W, int H, ColorState& colors);
    ~ColorSwatch() override; // out-of-line (ColorPicker is incomplete here); does not own the picker

    // Hand the swatch the colour picker to toggle. The picker is a child sub-window of the main
    // window (so it can stack over the canvas without being a separate top-level), built and owned
    // there; the swatch only keeps this non-owning pointer to show/hide it anchored to itself.
    void attachPicker(ColorPicker* picker) { m_picker = picker; }

protected:
    void draw() override;
    int handle(int event) override;

private:
    void openPicker();      // open (or toggle closed) the picker for the foreground colour
    void onColorsChanged(); // ColorState observer: redraw + re-sync an open picker

    ColorState& m_colors;
    ColorPicker* m_picker = nullptr; // non-owning; set via attachPicker() (the main window owns it)
};

// Preferred footprint of the swatch (the toolbar pins it to the bottom of its column).
inline constexpr int kSwatchW = 34;
inline constexpr int kSwatchH = 40;

} // namespace mosaic::ui
