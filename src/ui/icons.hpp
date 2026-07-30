#pragma once

#include "common/image.hpp"
#include "ui/widgets.hpp" // FlatButton

#include <cstdint>
#include <optional>

class Fl_RGB_Image;

// Panel-chrome icons (S16-g): the small monochrome marks the layer dock draws -- the add/group/
// delete buttons and each row's visibility + lock cells.
//
// These are REAL rasterized SVGs (assets/icon_*.svg, embedded at build time and rasterized through
// common::rasterizeSvg, the toolbar's nanosvg path), never font glyphs -- the host font may not
// carry a symbol and we cannot test every machine (see the no-Unicode-in-labels rule).
//
// They are deliberately NOT the toolbar's colourful, illustrative tool icons. Chrome icons carry
// one ink: the art is authored in pure white on transparent, and drawIcon() replaces the RGB with a
// palette colour while keeping the rasterizer's coverage in the alpha. One source therefore serves
// the light theme, the dark theme, hover, and the muted/disabled state -- and a re-theme costs
// nothing but a redraw. (The colourful, fully-illustrative set is the S52 icon session's job.)
//
// ⚠ They are authored on a 16x16 grid and MUST be drawn at kIconPx. Every straight edge sits on a
// half-integer (1px strokes) or an integer (2px strokes), so at scale 1.0 it covers exactly one
// pixel column. Rasterize them at any other size and nanosvg's uniform fit lands those edges on
// fractional boundaries -- the whole set goes soft, which is precisely what the first cut did
// (a 24-unit viewBox drawn at 16px) and what the user reported as "the small icons look blurry".
namespace mosaic::ui {

enum class Icon : std::uint8_t {
    Plus,        // New Layer
    Trash,       // Delete Layer
    GroupLayers, // Group Layers (a folder -- what the action makes)
    EyeOpen,     // layer visible
    EyeClosed,   // layer hidden (a lowered lid; see the asset comment for why not a slashed eye)
    LockOpen,    // layer unlocked
    LockClosed,  // layer locked
    Close,       // close a document tab (S49)
};

// The one size the chrome icons are authored for. Not a default anyone should override.
inline constexpr int kIconPx = 16;

// Draw `icon` centred on (cx, cy) as a kIconPx square, inked in `color`. Rasters are cached per
// (icon, colour); the palette offers a handful of inks, so the cache stays small. A failed
// rasterization logs once and draws nothing (never throws, never half-draws).
//
// The blit is 1:1 at (cx - 8, cy - 8) -- integral, so it always lands on the pixel grid. Crispness
// therefore hinges on the rasterization size alone, which is why kIconPx is not a parameter.
void drawIcon(Icon icon, int cx, int cy, common::Color8 color);

// A FlatButton whose label is one of the icons above: the flat box + hover + toggle-down highlight
// are the button's, the centred icon is ours. The ink follows active_r() exactly as Dropdown's text
// and GlyphButton's glyph do (pal.text -> pal.textMuted), so a disabled icon button greys with
// everything else. setInk() overrides that for a button wearing a non-palette fill (the New Layer
// button turns solid green as a clone drop-target, where the ink must read against the green).
class IconButton : public FlatButton {
public:
    IconButton(int X, int Y, int W, int H, Icon icon);

    void setIcon(Icon icon);
    void setInk(std::optional<common::Color8> ink); // std::nullopt -> follow the palette

protected:
    void draw() override;

private:
    Icon m_icon;
    std::optional<common::Color8> m_ink;
};

} // namespace mosaic::ui
