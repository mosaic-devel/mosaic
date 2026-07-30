#pragma once

#include "common/image.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <FL/Enumerations.H> // Fl_Boxtype, Fl_Color

class Fl_RGB_Image;
class Fl_Window;

// Mosaic's theming engine. FLTK 1.4.5 has no subclassable scheme system yet (its `Fl_Scheme`
// is only a name registry -- the OOP constructor is "not yet implemented"), so instead of a
// scheme subclass we drive a **token palette** onto FLTK's global color map + a set of custom
// flat boxtypes + tooltip styling. Every custom widget and the Vulkan canvas read the same
// tokens, so the whole UI stays consistent and re-themes by swapping one Palette.
// See docs/theming.md.
namespace mosaic::ui {

// Semantic color tokens (not raw FLTK slots): name things by role so widgets never hardcode
// colors. Alpha is unused for UI chrome but kept for the canvas-clear handoff.
struct Palette {
    bool dark = true;
    common::Color8 windowBg;      // app window / menu-bar ground
    common::Color8 panelBg;       // docks / panels
    common::Color8 canvasBg;      // area behind the image (the Vulkan clear color)
    common::Color8 controlBg;     // buttons / inputs at rest
    common::Color8 controlHover;  // hovered control
    common::Color8 controlActive; // pressed / active control
    // A row/cell that belongs to a MULTI-SELECTION but is not the active one (the layer dock's
    // S15-c rows). The weakest step of the control ramp -- about half of the panelBg->controlHover
    // distance in each theme -- so a selection reads as a group without laying a slab under every
    // row. Never used for the active row: that one keeps controlActive.
    common::Color8 controlSelected;
    common::Color8 text;          // primary text / labels
    common::Color8 textMuted;     // secondary / inactive text
    common::Color8 accent;        // selection / focus / active tool (may follow the OS accent)
    common::Color8 onAccent;      // text / icons drawn on top of `accent`
    common::Color8 border;        // hairlines / panel edges
    common::Color8 tooltipBg;
    common::Color8 tooltipText;
};

enum class ThemeMode {
    System, // follow the OS light/dark preference + accent color
    Dark,
    Light,
};

// Stable, lowercase key for a mode ("system"/"dark"/"light") -- the value stored in settings.
[[nodiscard]] std::string_view themeModeKey(ThemeMode mode);

// Parse a settings key back to a mode (case-insensitive); std::nullopt for anything else, so
// callers can fall back to a default. Inverse of themeModeKey().
[[nodiscard]] std::optional<ThemeMode> parseThemeMode(std::string_view key);

[[nodiscard]] Palette darkPalette();
[[nodiscard]] Palette lightPalette();

// Resolve the effective palette for `mode`. `System` consults the host (platform/system_theme)
// for the light/dark preference and folds in the OS accent color when one is exposed.
[[nodiscard]] Palette resolvePalette(ThemeMode mode);

// Apply `pal` to FLTK globally: base scheme, color map (background/foreground/selection/...),
// custom boxtypes, and tooltip styling. Idempotent; call once at startup and again to
// re-theme. Registers the MOSAIC_* boxtypes on first call. On a *re-theme* (not the first call) it
// fires the theme observers and requests a global redraw, so the live UI updates immediately.
void applyTheme(const Palette& pal);

// The palette most recently passed to applyTheme() (defaults to darkPalette()).
[[nodiscard]] const Palette& activePalette();

// Runtime re-theming (S51-a ③). Widgets that read activePalette() live in their draw() re-theme for
// free on the global redraw applyTheme() requests; widgets that CACHE palette colours at construction
// (FLTK color()/labelcolor()/textcolor()/... on themselves or child widgets) must re-apply them. Such
// a widget registers a callback here (typically `[this]{ reapplyTheme(); }`) and removes it in its
// destructor. Callbacks fire on every applyTheme() after the first, in registration order, AFTER the
// active palette + colour map + boxtypes have updated. Returns a token for removeThemeObserver().
int addThemeObserver(std::function<void()> onThemeChanged);
void removeThemeObserver(int token);

// RAII wrapper around a theme observer: registers on construction, removes on destruction. Hold one
// as a widget member (`ThemeSubscription m_themeSub{[this]{ reapplyTheme(); }};`) so the callback is
// torn down with the widget -- never fired against a destroyed `this`.
class ThemeSubscription {
public:
    ThemeSubscription() = default;
    explicit ThemeSubscription(std::function<void()> cb) : m_token(addThemeObserver(std::move(cb))) {}
    ~ThemeSubscription() { reset(); }
    ThemeSubscription(ThemeSubscription&& o) noexcept : m_token(o.m_token) { o.m_token = 0; }
    ThemeSubscription& operator=(ThemeSubscription&& o) noexcept {
        if (this != &o) {
            reset();
            m_token = o.m_token;
            o.m_token = 0;
        }
        return *this;
    }
    ThemeSubscription(const ThemeSubscription&) = delete;
    ThemeSubscription& operator=(const ThemeSubscription&) = delete;
    void reset() {
        if (m_token != 0) {
            removeThemeObserver(m_token);
            m_token = 0;
        }
    }

private:
    int m_token = 0; // 0 = unregistered (valid tokens start at 1)
};

// ---- Anti-aliased dot/ring painting (§12 chrome polish: fl_pie / fl_arc are stair-stepped) --
// One filled disc (stroke <= 0) or a stroked ring of `stroke` thickness centred on radius `r`.
// Coordinates are in the caller's drawing space; coverage is computed against pixel centres.
struct AAPrim {
    double cx = 0.0;
    double cy = 0.0;
    double r = 0.0;
    double stroke = 0.0;
    common::Color8 color;
};

// Render `prims` (in order) over the patch with top-left (originX, originY); `under(x, y)`
// supplies the colour beneath each patch pixel in the same coordinate space, so the result can
// be blitted opaquely. Pure (no FLTK) → unit-tested; drawAAPrims() is the fl_draw_image blit.
[[nodiscard]] common::Image renderAAPrims(
    int originX, int originY, int w, int h,
    const std::function<common::Color8(int x, int y)>& under, const std::vector<AAPrim>& prims);

void drawAAPrims(int originX, int originY, int w, int h,
                 const std::function<common::Color8(int x, int y)>& under,
                 const std::vector<AAPrim>& prims);

// Draw an anti-aliased LEFT-pointing speech-bubble triangle into the `tri`-wide left margin of the
// current window: tip at (0, tipY), base on the body edge at x == tri, base height `triH`. The
// triangle fill is `bodyBg` (matching the body it joins); the corner margins blend to `marginBg`
// (so over a uniform ground they vanish); a ~1px slant outline in `border`. fl_polygon/fl_line are
// stair-stepped, hence the rendered coverage patch. Shared by the colour flyout + the picker bubble.
void drawBubbleTriangleLeft(int tri, int triH, int tipY, common::Color8 bodyBg,
                            common::Color8 marginBg, common::Color8 border);

// The UP-pointing sibling: tip at (tipX, 0), base on the body edge at y == tri, base width `triW`.
// For a popover anchored BELOW its anchor (e.g. an options-bar button) -- same fill/margin/border
// conventions as drawBubbleTriangleLeft, transposed.
void drawBubbleTriangleUp(int tri, int triW, int tipX, common::Color8 bodyBg,
                          common::Color8 marginBg, common::Color8 border);

// The RIGHT-pointing sibling: a `tri`-wide margin whose left edge is at window x `xLeft`; tip at
// (xLeft + tri, tipY), base on the body edge at x == xLeft. For a flyout that opens to the LEFT of a
// right-aligned anchor (its triangle points right, at the anchor). Same conventions as ...Left.
void drawBubbleTriangleRight(int xLeft, int tri, int triH, int tipY, common::Color8 bodyBg,
                             common::Color8 marginBg, common::Color8 border);

// Fill `buf` (resized to W*H*4 RGBA) with a speech-bubble WINDOW-SHAPE mask for Fl_Window::shape():
// opaque (a=255) over the body + the triangle, transparent (a=0) in the triangle-strip corners so the
// backdrop shows through (real transparency, not a faked marginBg fill). `tri`/`triH`/`tipY` match a
// drawBubbleTriangle{Left,Right} call: the triangle sits in the RIGHT margin (x in [W-tri, W)) when
// `rightSide`, else the LEFT margin (x in [0, tri)). shape() is gated on ui::Popover::bubbleSupported()
// -- true everywhere today, but the plain-panel fallback stays for a platform that cannot cut, so
// callers must still gate on it. NB the mask only takes effect if the window paints inside
// Fl_Window::draw()'s platform bracket and claimed its shape before mapping: see MOSAIC_CHROME_BOX
// and seedOpaqueWindowShape below.
void buildBubbleShapeMask(std::vector<unsigned char>& buf, int W, int H, int tri, int triH, int tipY,
                          bool rightSide);

// Custom flat boxtypes, valid after the first applyTheme(). Widgets use them via
// widget->box(MOSAIC_*). Before applyTheme() they alias sane FLTK built-ins.
extern Fl_Boxtype MOSAIC_FLAT_BOX;        // solid fill, no border (bars, backgrounds)
extern Fl_Boxtype MOSAIC_PANEL_BOX;       // solid fill + 1px border (docks/panels)
extern Fl_Boxtype MOSAIC_INPUT_BOX;       // like PANEL_BOX but with horizontal text padding (inputs)
extern Fl_Boxtype MOSAIC_BUTTON_UP_BOX;   // rounded control, at rest
extern Fl_Boxtype MOSAIC_BUTTON_DOWN_BOX; // rounded control, pressed/active

// ---- Shaped-window chrome ---------------------------------------------------------------------
// Fl_Window::shape() is applied by the platform driver ONLY inside the draw_begin()/draw_end()
// bracket that Fl_Window::draw() puts around its painting: native Wayland cuts the mask in
// draw_end() (a cairo CLEAR), macOS clips to it in draw_begin() (CGContextClipToMask). X11 is the
// odd one out -- XShapeCombineMask cuts the window server-side inside shape() itself, so an X11
// window keeps its shape no matter how it draws. A window that paints in its own draw() and never
// calls Fl_Window::draw() therefore loses its shape on every platform EXCEPT the one we develop on.
//
// MOSAIC_CHROME_BOX is the way back into that bracket: give the window this box, bind a painter for
// the duration of one draw with ScopedChromePainter, and call Fl_Window::draw() -- FLTK invokes the
// box between draw_begin() and its own child pass. See ui::Popover::draw() for the whole pattern.
extern Fl_Boxtype MOSAIC_CHROME_BOX;

// Give `win` a fully-opaque Fl_Window::shape() now, storing the mask in the caller's buffers (the
// image must outlive the call -- FLTK keeps the pointer). Call it while the window is still
// UNMAPPED, before anything the platform does at surface-creation time.
//
// Why: native Wayland declares a fully-opaque region for any window whose shape() is still null when
// its surface is created, and never clears it if a shape arrives later. A popover is a sub-window,
// so its surface is created when the HOST window is shown -- long before showAnchored() knows where
// the triangle points and builds the real mask. Without this seed the compositor has already been
// promised an opaque surface and ignores the alpha the mask punches, painting the cut region as raw
// cleared pixels: a black hole straight through to the desktop instead of the canvas behind.
//
// A fully-opaque mask cuts nothing, so it costs one allocation and changes no pixel anywhere.
void seedOpaqueWindowShape(Fl_Window& win, std::vector<unsigned char>& buf,
                           std::unique_ptr<Fl_RGB_Image>& img);

// Binds the painter MOSAIC_CHROME_BOX dispatches to, for one window draw. Nests and restores.
class ScopedChromePainter {
public:
    using Fn = void (*)(void*);
    ScopedChromePainter(Fn fn, void* userData) noexcept;
    ~ScopedChromePainter();
    ScopedChromePainter(const ScopedChromePainter&) = delete;
    ScopedChromePainter& operator=(const ScopedChromePainter&) = delete;
    ScopedChromePainter(ScopedChromePainter&&) = delete;
    ScopedChromePainter& operator=(ScopedChromePainter&&) = delete;

private:
    Fn m_prevFn;
    void* m_prevUserData;
};

} // namespace mosaic::ui
