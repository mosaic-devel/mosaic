#pragma once

#include "ui/theme.hpp" // ThemeSubscription (FlatButton self-heals on a runtime re-theme)

#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Float_Input.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Slider.H>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Fl_Input_;
class Fl_RGB_Image;

// Themed custom-widget base classes. They read the active palette (ui/theme) at construction
// so the whole UI stays visually consistent. This is the groundwork the docks (S10) and the
// tool buttons + options bar + Properties tab (S11) build on. The point of drawing our own:
// FLTK's stock controls (Fl_Choice's arrow, the slider grips) carry a 3-D/Motif flavour that
// clashes with the flat theme, so the interactive widgets override draw() to paint themselves.
namespace mosaic::ui {

// ---- Anti-aliased arcs, rings and discs (the fl_arc / fl_pie replacement) ----------------------
// FLTK only anti-aliases arcs on its CAIRO-backed drivers. Windows GDI's Arc()/Pie() and X11's
// XDrawArc/XFillArc are hard-edged, so every code-drawn circle in the app read stair-stepped on
// Windows and on Linux/X11 while looking smooth on native Wayland -- which is exactly why the layer
// dock's mask-link chain was reported as jagged from a Windows build and from nowhere else
// (2026-07-30). Building the Windows FLTK with FLTK_GRAPHICS_CAIRO=ON would fix one platform;
// rasterizing the marks ourselves fixes all of them, adds nothing to the payload, touches no text
// rendering, and reuses the SDF-coverage machinery ui::Dial, ui::GizmoCanvas and theme.hpp's AAPrim
// already ship. This is the arc-and-ellipse-capable superset of AAPrim: prefer it for new marks.
//
// Coverage is LINEAR in the distance to the edge -- clamp(halfWidth + 0.5 - |d|, 0, 1) -- never a
// smoothstep: a smoothstep plateaus either side of the centreline and leaves single-sample AA
// looking ropey (the lesson the overlay-line shader redesign paid for).
//
// ⚠ FLTK offers no readback, so nothing can be blended against the pixels already on the surface.
// A caller instead supplies `under(x, y)` -- the colour it KNOWS is beneath each patch pixel -- the
// arcs compose over that in a private RGB buffer, and the finished patch is blitted once, opaquely.
// Same arrangement as Dial::draw and drawAAPrims, and it has one consequence worth stating plainly:
// an opaque patch ERASES whatever is already inside its rect that `under` does not reproduce. Marks
// that overlap each other therefore belong in ONE call (they compose in list order), never in two.
struct AAArc {
    double cx = 0.0;     // centre in the caller's drawing space: an INTEGER is a pixel CORNER, so a
    double cy = 0.0;     // mark centred ON pixel n has c == n + 0.5 (the AAPrim convention)
    double rx = 0.0;     // semi-axes of the stroke CENTRELINE (of the fill itself when stroke <= 0);
    double ry = 0.0;     // rx != ry draws an ellipse -- the globe's meridian, the chain's rings
    double stroke = 0.0; // stroke width in px; <= 0 means a filled disc / pie wedge instead
    common::Color8 color{};
    double a0 = 0.0;   // fl_arc's own angles: degrees COUNTER-clockwise from 3 o'clock, a1 >= a0
    double a1 = 360.0; // a sweep of 360 deg or more is the whole ellipse (no wedge edges at all)
};

// The AA'd equivalent of fl_arc(x, y, w, h, a0, a1): same bounding box, same angle convention, so a
// converted call site keeps its literal numbers and "did the geometry move?" is answerable by diff.
// `stroke` is the pen width the fl_arc being replaced drew with -- 1 unless fl_line_style set one.
[[nodiscard]] AAArc aaArcFromBox(int x, int y, int w, int h, double a0, double a1, double stroke,
                                 common::Color8 c);

// The AA'd equivalent of fl_pie(x, y, w, h, a0, a1). NB a pie slice is a HALF PIXEL wider than the
// fl_arc outline of the same box (radius w/2 against (w-1)/2) -- FLTK documents that discrepancy,
// and reproducing it is what keeps a converted disc the size it was visually tuned at.
[[nodiscard]] AAArc aaPieFromBox(int x, int y, int w, int h, double a0, double a1,
                                 common::Color8 c);

// The AA'd equivalent of fl_circle(x, y, r) -- centre ON pixel (x, y), radius exactly r.
[[nodiscard]] AAArc aaCircle(double x, double y, double r, double stroke, common::Color8 c);

// `a`'s coverage in [0, 1] at the sample point (sx, sy), which is a pixel CENTRE (n + 0.5). Pure
// and FLTK-free: the whole rasterizer's geometry lives here, so it is unit-testable on its own.
[[nodiscard]] double aaArcCoverage(const AAArc& a, double sx, double sy);

// Compose `arcs` in list order over the ground `under` reports, for the w x h patch whose top-left
// pixel is (originX, originY), and blit the result once. Depth-3 RGB deliberately: fl_draw_image
// with depth 4 is read inconsistently across backends (the standing never-blit-RGBA rule).
void drawAAArcs(int originX, int originY, int w, int h,
                const std::function<common::Color8(int x, int y)>& under,
                const std::vector<AAArc>& arcs);

// Flat-ground convenience: the patch is sized to the arcs' own coverage bounds and cleared to
// `ground`. Most call sites know their marks sit on one solid colour, and this way nobody has to
// hand-compute a patch rect -- one that is a pixel too tight would guillotine the AA ramp.
void drawAAArcs(common::Color8 ground, const std::vector<AAArc>& arcs);

// Same auto-sized patch, but for a ground that is NOT one flat colour: a mark landing on a raster,
// a gradient, or a hairline drawn a moment earlier. `under` must answer for every pixel the patch
// covers, so the sampler is where a caller re-states the frame / crosshair / plot buffer it is
// painting on top of -- see ToneWheel::draw and CurveEditor::draw for both shapes of answer.
void drawAAArcs(const std::function<common::Color8(int x, int y)>& under,
                const std::vector<AAArc>& arcs);

// A themed container: solid panel fill + hairline border edges from the palette. Which edges
// draw is a bitmask, because for docked chrome **exactly one element must own each junction
// line** -- when two adjacent elements each draw their shared edge the hairline doubles to 2 px
// (user feedback, 2026-06: "visual agony"). Convention: the element draws only the edge(s)
// facing a *neighbouring region* it is responsible for separating (the options bar owns its
// top + bottom, the toolbar its right, the dock its left, the status bar its top); window-edge
// sides draw nothing (the WM frame is the border there). Free-standing panels keep EdgesAll.
class Panel : public Fl_Group {
public:
    enum Edge : unsigned {
        EdgesNone = 0,
        EdgeTop = 1,
        EdgeRight = 2,
        EdgeBottom = 4,
        EdgeLeft = 8,
        EdgesAll = EdgeTop | EdgeRight | EdgeBottom | EdgeLeft,
    };

    Panel(int X, int Y, int W, int H, const char* label = nullptr);

    void borderEdges(unsigned mask) {
        m_edges = mask;
        redraw();
    }

    // Re-read the palette into this panel's cached fill/label colours after a runtime theme change
    // (ui::applyTheme). The border + children that draw live re-theme on the global redraw; only the
    // cached fill needs this. Virtual so a themed subclass re-applies its own child colours too --
    // override and call Panel::reapplyTheme() first.
    virtual void reapplyTheme();

protected:
    void draw() override; // fill + children, then the owned hairline edges on top

private:
    unsigned m_edges = EdgesAll;
};

// A flat, rounded, theme-colored button with a hover highlight and no focus rectangle.
class FlatButton : public Fl_Button {
public:
    FlatButton(int X, int Y, int W, int H, const char* label = nullptr);

protected:
    int handle(int event) override;
    // Restore the rest AND pressed fills on a runtime re-theme. The label is a semantic colour
    // (follows for free), but two concrete palette RGBs are baked into this widget: FL_ENTER bakes a
    // hover colour into color() -- and if the widget is hidden while hovered (e.g. clicking "Done" to
    // close a dialog) no FL_LEAVE fires, so it freezes and survives reopen -- and the constructor
    // bakes controlActive into selection_color(), the fill Fl_Button::draw() paints the DOWN box
    // with. Both go stale across a theme switch; this re-reads them.
    // ⚠ CONTRACT for every subclass that overrides this to restore a fill of its own (FilledButton,
    // tool_options' TintedButton, the AskOrTell StageButton): call `FlatButton::reapplyTheme()`
    // FIRST, then re-apply yours. The base owns the pressed fill for the whole button family, so an
    // override that does not chain silently re-opens the staleness for its subtree.
    virtual void reapplyTheme();

private:
    ThemeSubscription m_themeSub; // fires reapplyTheme() on every applyTheme()
};

// The accent-filled primary button -- the one emphasized action of a dialog's button row ("Create",
// "Fill", "Export"). A FlatButton, so it keeps the rounded themed box matching the quiet Cancel
// beside it; only the fill (accent, hover-lightened) and label (onAccent) differ. Promoted from the
// identical file-local copies in the Fill/Texture/Export dialogs (they still carry theirs; migrate
// opportunistically). Uses the reapplyTheme() hook FlatButton reserved for exactly this subclass.
class FilledButton : public FlatButton {
public:
    FilledButton(int X, int Y, int W, int H, const char* label = nullptr);

protected:
    int handle(int event) override;
    void reapplyTheme() override; // restore the ACCENT fill, not FlatButton's controlBg

private:
    void applyFill(bool hover);
};

// A toggle whose glyph is drawn IN the style it represents -- "B" bold, "I" italic, "U" underlined,
// "S" struck, a stack of alignment rules, or a check/✗ -- because plain letters ("B I U S" / "L C R J")
// read as ambiguous. A FlatButton subclass, so it keeps the flat box + hover + toggle-down highlight;
// only the label is replaced by a hand-drawn glyph (host-font / no-Unicode-in-label rule,
// [[mosaic-ui-gotchas]]). Shared by the Type panel and the Type context bar's B/I/U/S so the two match.
class GlyphButton : public FlatButton {
public:
    enum class Kind {
        Bold, Italic, Underline, Strike,
        AlignLeft, AlignCenter, AlignRight, AlignJustify,
        Check, // a bare check (on) / ✗ (off) toggle
    };
    GlyphButton(int X, int Y, int W, int H, Kind kind) : FlatButton(X, Y, W, H), m_kind(kind) {}

protected:
    void draw() override;

private:
    enum class Deco { None, Underline, Strike };
    // The glyph's ink. Greyed exactly like a disabled Dropdown's text (pal.text -> pal.textMuted,
    // keyed on active_r()), so a deactivated B/I/U/S reads the same as a deactivated combobox
    // beside it on the context bar -- they used to diverge (the glyph kept full contrast).
    [[nodiscard]] Fl_Color glyphColor() const;
    void drawLetter(const char* s, int font, Deco deco);
    void drawAlign();
    void drawCheck();
    Kind m_kind;
};

// The settled themed checkbox (originally the Settings dialog's): a small square (accent fill + a drawn
// tick when checked, hover/rest fills otherwise) followed by a label; the WHOLE widget toggles on click
// and fires onToggle(newState). Greys via active_r(). Erase-ground defaults to the window ground; a
// panel host (a different surface colour) sets it via setGroundColor so the erase matches its backdrop.
class CheckBox : public Fl_Widget {
public:
    CheckBox(int X, int Y, int W, int H, const char* label, std::function<void(bool)> onToggle = {})
        : Fl_Widget(X, Y, W, H, label), m_onToggle(std::move(onToggle)) {}

    void setChecked(bool c) {
        if (c != m_checked) {
            m_checked = c;
            redraw();
        }
    }
    [[nodiscard]] bool checked() const { return m_checked; }
    void setOnToggle(std::function<void(bool)> f) { m_onToggle = std::move(f); }
    void setGroundColor(common::Color8 c) { m_ground = c; m_hasGround = true; redraw(); }

protected:
    int handle(int event) override;
    void draw() override;

private:
    std::function<void(bool)> m_onToggle;
    bool m_checked = false;
    bool m_hover = false;
    common::Color8 m_ground{};   // the erase colour when m_hasGround; else activePalette().windowBg
    bool m_hasGround = false;
};

// The settled colour line (originally the Fill dialog's): a small colour chip + its hex readout.
// Passive by default (a live preview); setInteractive(true) makes it a button -- hover highlight,
// an "Edit…" hint + chevron -- and a click fires onClick (the host opens a ColorFlyout anchored
// here). setMixed paints a diagonal hatch + an em-dash readout ("several colours across the
// selection", the Type panel's case). Erase-ground defaults to the window ground; a panel host
// sets setGroundColor so the erase matches its backdrop (the CheckBox convention).
class SwatchChip : public Fl_Widget {
public:
    SwatchChip(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

    void setColour(common::Color8 c) {
        m_c = c;
        redraw();
    }
    [[nodiscard]] common::Color8 colour() const { return m_c; }
    void setMixed(bool m) {
        if (m != m_mixed) {
            m_mixed = m;
            redraw();
        }
    }
    void setInteractive(bool on) {
        if (on != m_interactive) {
            m_interactive = on;
            redraw();
        }
    }
    void setOnClick(std::function<void()> cb) { m_onClick = std::move(cb); }
    void setGroundColor(common::Color8 c) {
        m_ground = c;
        m_hasGround = true;
        redraw();
    }

protected:
    int handle(int event) override;
    void draw() override;

private:
    common::Color8 m_c{0, 0, 0, 255};
    common::Color8 m_ground{}; // the erase colour when m_hasGround; else activePalette().windowBg
    bool m_hasGround = false;
    bool m_mixed = false;
    bool m_interactive = false;
    bool m_hover = false;
    std::function<void()> m_onClick;
};

// A tiny clickable colour swatch showing the colour a getter returns, composited over a transparency
// checkerboard (so a semi-transparent colour reads), with a hover-accent frame; a click fires onClick.
// The flyouts' compact "set to foreground" affordance -- it shows the foreground colour, so it reads
// as "click this colour to apply it" (replacing a full-width Use-foreground button).
class SwatchButton : public Fl_Widget {
public:
    SwatchButton(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}
    void setColorGetter(std::function<common::Color8()> g) {
        m_get = std::move(g);
        redraw();
    }
    void setOnClick(std::function<void()> f) { m_onClick = std::move(f); }

protected:
    void draw() override;
    int handle(int event) override;

private:
    std::function<common::Color8()> m_get;
    std::function<void()> m_onClick;
    bool m_hover = false;
};

// The ONE scrolling-label behaviour, embeddable in any custom-drawn widget (the app-wide answer
// to a too-long label — never grow the control, never invent a per-control truncation): text
// that fits draws flush to the chosen edge; text that overflows its cell scrolls horizontally
// (bouncing, with end dwells) inside it. Not a widget — own one as a member and call draw()
// from the owner's draw() with the font and colour already set; it schedules its own animation
// ticks (redrawing the owner) only while the text actually overflows and the owner is visible,
// and auto-restarts from the resting position whenever the text changes. ScrollingLabel wraps
// it as a standalone widget; the Dropdown (closed control) and the open list's hovered row
// embed it directly.
class Marquee {
public:
    ~Marquee() { stop(); }

    // Draw `text` clipped to the cell (x,y,w,h), scrolling if it overflows. `host` is the
    // owning widget (redrawn per tick; ticking stops while it is not visible_r()).
    void draw(Fl_Widget* host, const char* text, int x, int y, int w, int h,
              bool rightAlignWhenFits = false);
    void reset(); // back to the resting position (e.g. the hovered row changed)
    void stop();  // cancel the pending tick (owner hiding; the dtor calls it too)

    // Seconds for one full traverse of an overflow of `overflowPx`, dwells included; 0 when
    // nothing overflows. (The status bar holds transient messages at least this long.)
    [[nodiscard]] static double oneScrollSeconds(double overflowPx);

private:
    static void tick(void* self);

    Fl_Widget* m_host = nullptr;
    std::string m_lastText; // text-change detection -> auto reset
    float m_offset = 0.0F;  // scroll position px (0 = text left edge at the cell's left)
    float m_dir = 1.0F;
    int m_pauseTicks = 0; // remaining ticks to dwell at an end before reversing
    float m_maxOff = 0.0F;
    bool m_timer = false;
};

// A flat, theme-colored dropdown: our rounded box + a custom chevron + a hover highlight, no
// focus ring. The pop-up list inherits the globally themed menu colors (ui::applyTheme). Use it
// exactly like Fl_Choice (add()/value()/...). Replaces a bare Fl_Choice so the closed control
// matches the flat buttons instead of showing FLTK's stock up/down arrow box.
class Dropdown : public Fl_Choice {
public:
    Dropdown(int X, int Y, int W, int H, const char* label = nullptr);

    // Show `text` in the closed control instead of the selected item's label (cleared by an empty
    // string). Used for a "mixed" state — e.g. the layer panel showing "Normal, Multiply" when a
    // multi-selection spans several blend modes. Does not change value(); purely cosmetic.
    void setOverrideText(std::string text);

    // Mark several item indices with a dot in the OPEN list, for a "mixed" multi-selection — e.g.
    // the blend dropdown dotting every mode present across the selected layers (S15-e "All"), so the
    // list shows what the set currently spans. Empty (the default) = only the selected value() is
    // dotted, the normal single-select look. Purely cosmetic; does not change value().
    void setMarkedItems(std::vector<int> indices);
    [[nodiscard]] const std::vector<int>& markedItems() const noexcept { return m_marked; }

    // ---- Optional rich open list (the font picker, docs/type-tool.md §8) -----------------------
    // Taller rows (0 = the default compact height) so a per-row preview has room.
    void setRowHeight(int h) { m_rowHeight = h; }
    [[nodiscard]] int rowHeight() const noexcept { return m_rowHeight; }
    // A minimum width for the OPEN list (it may be wider than the closed control); 0 = no minimum.
    void setListMinWidth(int w) { m_listMinWidth = w; }
    [[nodiscard]] int listMinWidth() const noexcept { return m_listMinWidth; }
    // Per-row preview image: given an item index and the preview cell size in px, return a cached image
    // to blit in the row (the family rendered in its own face), or null for none. The CALLER owns the
    // returned image (lazy-render + cache it); the list never deletes it. null provider = a plain list.
    void setRowPreview(std::function<Fl_RGB_Image*(int index, int cellW, int cellH)> cb) {
        m_rowPreview = std::move(cb);
    }
    [[nodiscard]] const std::function<Fl_RGB_Image*(int, int, int)>& rowPreview() const noexcept {
        return m_rowPreview;
    }
    // Live "hover preview": invoked with the hovered item index as the cursor moves over the OPEN
    // list (and -1 when the cursor leaves it or the list closes without committing). The font picker
    // uses it to apply the hovered family to the edited text transiently, reverting on -1 (S29-c §8).
    void setHoverPreview(std::function<void(int index)> cb) { m_hoverPreview = std::move(cb); }
    [[nodiscard]] const std::function<void(int)>& hoverPreview() const noexcept {
        return m_hoverPreview;
    }

protected:
    void draw() override;
    int handle(int event) override;

private:
    bool m_hover = false;
    std::string m_override;
    Marquee m_labelMarquee;    // the closed control's label scrolls instead of truncating
    std::vector<int> m_marked; // indices dotted in the open list (mixed multi-selection); see above
    int m_rowHeight = 0;       // 0 = default compact row; >0 = taller rows (preview list)
    int m_listMinWidth = 0;    // 0 = list width tracks the control; >0 = a wider minimum
    std::function<Fl_RGB_Image*(int, int, int)> m_rowPreview; // per-row preview image (null = none)
    std::function<void(int)> m_hoverPreview;                  // live hover preview (null = none)
};

// Populate a Dropdown with every blend mode, grouped by family with FL_MENU_DIVIDER separators
// (Normal | Darken | Lighten | Contrast | Inversion | Component). Shared by the layer panel and the
// Fill dialog so the two blend lists read identically.
void addBlendModeItems(Dropdown& dd);

// A flat, theme-colored horizontal slider: a thin track with an accent fill up to a round handle.
// Custom-drawn *and* custom-hit (it maps the cursor to a value through its own geometry, then uses
// Fl_Valuator's push/drag/release so callback + coalescing semantics are unchanged), because
// Fl_Slider's built-in styles look out of place next to the flat widgets. Use it like Fl_Slider
// (range()/value()/step()/when()/callback()).
class Slider : public Fl_Slider {
public:
    Slider(int X, int Y, int W, int H, const char* label = nullptr);

    // Override the colour the slider clears its own cell + handle rim with. Sliders default to the
    // panel ground (they normally sit on a panelBg dock); set this when a slider sits on a different
    // ground (e.g. the Settings pane's windowBg) so the cell blends in instead of showing a panel box.
    void setCellColor(common::Color8 c) {
        m_cellColor = c;
        m_cellColorSet = true;
        redraw();
    }

protected:
    void draw() override;
    int handle(int event) override;

private:
    bool m_hover = false;
    common::Color8 m_cellColor{};
    bool m_cellColorSet = false;
};

// A themed horizontal progress bar: a rounded controlBg track with an accent fill, sharing the
// ui::Slider track language. Two modes: DETERMINATE (setFraction; the fill grows left->right) and
// INDETERMINATE (setIndeterminate; an accent segment sweeps the track while progress is not yet
// measurable). The sweep runs on the shared timer convention (repeat_timeout while visible,
// removed in the dtor) and re-arms itself from draw() after the bar was hidden. First consumer:
// the AskOrTellDialog's operation stages (docs/askortell-dialog.md); the S41 export flow is the
// expected next one. Ground defaults to the panel fill; a host on a different surface sets
// setCellColor (the ui::Slider convention).
class ProgressBar : public Fl_Widget {
public:
    ProgressBar(int X, int Y, int W, int H);
    ~ProgressBar() override;

    void setFraction(double f); // clamps to [0, 1]; leaves indeterminate mode
    void setIndeterminate();    // sweep until a fraction arrives
    [[nodiscard]] double fraction() const noexcept { return m_fraction; }
    [[nodiscard]] bool indeterminate() const noexcept { return m_indet; }

    void setCellColor(common::Color8 c) {
        m_cellColor = c;
        m_cellColorSet = true;
        redraw();
    }

    // The lit span [x0, x1) of the indeterminate sweep within a trackW-wide track, for a sweep
    // position `phase` in [0, 1): the segment slides in from fully off the left edge and out past
    // the right one, clipped to the track (empty at both extremes). Pure -- unit-tested.
    static void indeterminateSpan(double phase, int trackW, int& x0, int& x1);

protected:
    void draw() override;

private:
    static void tick(void* self);
    void arm(); // schedule the sweep tick if the animation should run (idempotent)

    double m_fraction = 0.0;
    double m_phase = 0.0; // sweep position, wraps in [0, 1)
    bool m_indet = false;
    bool m_timer = false; // a tick is scheduled
    common::Color8 m_cellColor{};
    bool m_cellColorSet = false;
};

// A circular ROTARY KNOB for authoring an angle -- the reusable base for any control that reads
// better as a dial than a slider (the pattern flyout's Angle, the shape designer's arc/tail
// directions; future rotate/orientation knobs). An angle is CYCLIC, which a linear degree slider
// models badly: its endpoints are arbitrary, it cannot express wrap-around, and "point the tail
// down-left" becomes a numeric puzzle instead of a gesture. This is a plain Fl_Valuator
// (value()/range()/step()/callback()/when() exactly like ui::Slider), so a host drives it
// identically.
//
// Interaction: drag anywhere on the knob to point the needle at the cursor -- the value is the
// screen angle measured CLOCKWISE FROM 12 o'clock, quantised to step() and wrapped into the range,
// so dragging round and round never sticks at an endpoint. Shift snaps to setSnapIncrement()
// (15 deg by default -- the Illustrator/Photoshop constrain grid). The wheel and the arrow keys
// nudge by step (Shift = x10, and the arrows respect the snap grid); Home/End go to the reset value
// and its opposite; a middle / Ctrl click resets to setDefaultValue() (the range minimum when none
// was set). The knob shows a hand cursor, and setShowReadout() prints the live value in its face.
// It renders with software SDF coverage (a clean anti-aliased face + rim + needle) because FLTK's
// fl_pie / fl_arc are not anti-aliased -- matching the AA the rest of the chrome now carries.
// A host whose stored angle is NOT in the "clockwise from 12" convention (e.g. the blur filters'
// math angle, 0 = +x measured toward +y) can setZeroOffset() so the needle still points the true
// direction while value() stays byte-identical to what the host serializes (see setZeroOffset).
class Dial : public Fl_Valuator {
public:
    Dial(int X, int Y, int W, int H, const char* label = nullptr);

    // The ground the dial clears its (square) cell to, for a knob that sits on a non-panelBg host
    // (parity with ui::Slider::setCellColor / ScrubSlider::setCellColor).
    void setCellColor(common::Color8 c) {
        m_cellColor = c;
        m_cellColorSet = true;
        redraw();
    }

    // Remap the needle for a host that stores its angle in a convention other than the dial's native
    // "clockwise from 12 o'clock": `screenDegAtZero` is the screen angle (still clockwise from 12)
    // the needle points at when value()==0. The rotation SENSE is shared -- +value turns the needle
    // clockwise, which is also how the math angle sweeps +x->+y on screen (y-down) -- so an offset
    // alone reconciles the blur convention (offset 90: value 0 == +x == 3 o'clock). value() itself is
    // untouched, so a host reads/writes it in its own units and the stored bytes are unchanged.
    // Default 0 keeps value() == the screen angle (every pre-existing caller renders identically).
    void setZeroOffset(double screenDegAtZero) {
        m_zeroOffset = screenDegAtZero;
        redraw();
    }

    // The value a middle-click / Ctrl-click resets to (parity with ScrubSlider::setDefaultValue).
    // Unset -> the historical reset-to-range-minimum. Set it when the range minimum is not the rest
    // value (the blur angle's range is [-180,180] but its neutral is 0, not -180).
    void setDefaultValue(double v) {
        m_default = v;
        m_hasDefault = true;
    }

    // Print the live value (+ the degree sign) in the middle of the face, and draw the needle as a
    // rim TICK rather than a hub-to-rim spoke so the digits have room. Opt-in because the knob has
    // to be big enough to read -- a 24 px row dial (the adjustment/effects panels) has none, and
    // those hosts already caption the number beside the knob. Off = the historical needle look.
    void setShowReadout(bool on) {
        m_showReadout = on;
        redraw();
    }

    // The grid Shift snaps a drag to, and the coarse step Shift+arrow keys walk. Degrees; <= 0
    // disables snapping. Defaults to kDefaultSnapDeg.
    void setSnapIncrement(double deg) { m_snapDeg = deg; }

    // The conventional angle-constrain grid (Illustrator / Photoshop / Figma all use 15 deg).
    static constexpr double kDefaultSnapDeg = 15.0;

    // The screen angle (degrees clockwise from 12 o'clock) a cursor at (dx, dy) from the knob
    // centre points at, folded into [0, 360), with `snap` applied when it is > 0. Pure, so the
    // dial's whole angle mapping -- wrap-around and snapping included -- is unit-testable without
    // a widget (tests/test_shape_gesture.cpp).
    [[nodiscard]] static double screenAngleAt(double dx, double dy, double snap = 0.0);

    // `v` folded into [minimum(), maximum()) when the range spans a full turn, else clamped to it.
    // Exposed (and static-friendly via the free wrapDialValue below) for the same reason.
    [[nodiscard]] double clampWrap(double v) const;

protected:
    void draw() override;
    int handle(int event) override;

private:
    [[nodiscard]] common::Color8 cellColor() const;
    void pointNeedleAt(int eventX, int eventY); // value <- cursor angle about the knob centre
    void nudge(double delta);                   // wheel / arrow key: value += delta, wrapped
    [[nodiscard]] double resetValue() const;

    bool m_hover = false;
    bool m_drag = false;
    bool m_wantFocus = false;  // set at PUSH so FL_FOCUS accepts: click-to-focus without a Tab stop
    bool m_showReadout = false;
    common::Color8 m_cellColor{};
    bool m_cellColorSet = false;
    double m_zeroOffset = 0.0; // screen deg (cw from 12) the needle shows at value()==0; 0 == native
    double m_default = 0.0;    // reset target (see setDefaultValue); minimum() when m_hasDefault is false
    bool m_hasDefault = false;
    double m_snapDeg = kDefaultSnapDeg;
};

// Dial::clampWrap's math as a free function: `v` folded into [mn, mx) when the range spans a full
// turn (>= 359.9 deg), else clamped into it. Pure -- the dial's wrap-around contract is pinned here
// so a test can assert it without constructing a widget.
[[nodiscard]] double wrapDialValue(double v, double mn, double mx);

// A themed scroll container: an Fl_Scroll that behaves exactly like the stock one (geometry,
// scrollbar visibility, value, mouse-wheel, child clipping are all inherited) but repaints the
// stock FLTK scrollbar in the flat theme -- a panel-coloured trough (the stock 3-D knob and arrow
// buttons are overpainted) with an anti-aliased rounded grab in control colours, brightening on
// hover and going accent while dragged, matching ui::Slider. The base bars are kept fully
// functional and made to paint a flat panelBg rectangle (via MOSAIC_FLAT_BOX), so our grab is the
// only chrome that shows. Use it anywhere an Fl_Scroll is used (the docks, the Settings sub-panes).
class ScrollView : public Fl_Scroll {
public:
    ScrollView(int X, int Y, int W, int H, const char* label = nullptr);

    // Re-apply the neutral colours the stock bars paint beneath our overlay after a runtime theme
    // change; the grab reads the palette live in draw(). Fired automatically via m_themeSub.
    void reapplyTheme();

    // The width the vertical scrollbar WILL occupy once `contentHeight` px of rows are laid out --
    // 0 when they fit. Ask this from a LAYOUT pass; do NOT read `scrollbar.visible()` from a row's
    // draw(). Fl_Scroll only decides that flag inside its own draw(), so before the first paint
    // after a show() or a resize it still holds the previous answer, and any row that insets its
    // right-hand furniture (an age caption, a selection dot) by it lands in the wrong place for one
    // frame -- the "labels jump on tab entry" flicker. Same rule Fl_Scroll uses, asked early.
    [[nodiscard]] int scrollbarGutter(int contentHeight) const;

protected:
    void draw() override;            // base draw (children + neutralised bars), then our grab on top
    int handle(int event) override; // tracks pointer-over-bar for the hover state

private:
    void neutralizeBar(Fl_Scrollbar& sb); // make a stock bar paint a flat trough-coloured rectangle
    // The trough colour = this widget's own color() (what the caller painted behind the scroll: the
    // dock's panelBg, the Settings pane's windowBg), so the bar always blends into its background.
    [[nodiscard]] common::Color8 troughColor() const;
    void paintGrab(const Fl_Scrollbar& sb, bool vertical, bool hover, bool drag);
    // Thumb pixel length + grab rectangle for a bar at its current value. The grab spans the FULL
    // trough -- we draw no arrow buttons, so it reaches both ends with no floating gutters (the
    // stock bar's arrow-reserved track is bypassed; we drive scrolling ourselves, see handle()).
    void trackSpan(const Fl_Scrollbar& sb, bool vertical, int& start, int& len) const;
    [[nodiscard]] int thumbLen(const Fl_Scrollbar& sb, bool vertical) const;
    void grabRect(const Fl_Scrollbar& sb, bool vertical, int& gx, int& gy, int& gw, int& gh) const;
    [[nodiscard]] bool inBar(const Fl_Scrollbar& sb) const; // is the current event over this bar?
    bool startBarDrag(Fl_Scrollbar& sb, bool vertical);     // begin a thumb/trough drag on FL_PUSH
    void dragBarTo(Fl_Scrollbar& sb, bool vertical);        // scroll so the grab tracks the cursor

    bool m_vHover = false;
    bool m_hHover = false;
    bool m_vDrag = false;
    bool m_hDrag = false;
    int m_dragOffset = 0; // px from the cursor to the grab's leading edge during a drag
    ThemeSubscription m_themeSub; // fires reapplyTheme() on every applyTheme()
};

// A selectable gallery card -- preview art over a title line and an optional muted subtitle (the
// New Document dialog's preset / template / recent tiles). The preview is either an owned opaque
// RGBA image blitted 1:1 centred (the CALLER pre-fits it to the preview cell -- the card never
// scales) or a draw callback for procedural placeholder art (the paper-proportioned preset
// rectangle); the callback also receives the hover-aware ground so AA art can blend against it.
// Follows the settings OptionCard's discipline: the whole PUSH/RELEASE pair is claimed
// ([[mosaic-ui-gotchas]]), and the label strip is erased before text (a double-buffered window
// keeps prior pixels in undamaged regions -- unerased labels thicken on every repaint). A single
// click fires onSelect; a double click onActivate (the dialog's Create/Open shortcut).
class GalleryCard : public Fl_Widget {
public:
    using PreviewFn = std::function<void(int px, int py, int pw, int ph, common::Color8 bg)>;

    static constexpr int kTitleH = 20;    // title strip under the preview
    static constexpr int kSubtitleH = 15; // optional second strip (muted detail line)

    GalleryCard(int X, int Y, int W, int H, std::string title, std::string subtitle = {});

    // Opaque RGBA, already fitted to the preview cell. Packed to depth-3 RGB internally: RGBA
    // fl_draw_image blits misread on some backends ([[mosaic-ui-gotchas]] "NEVER depth 4").
    void setThumbnail(const common::Image& thumb);
    void setPreviewFn(PreviewFn fn);
    void setSelected(bool s);
    [[nodiscard]] bool selected() const noexcept { return m_selected; }
    void setOnSelect(std::function<void()> f) { m_onSelect = std::move(f); }
    void setOnActivate(std::function<void()> f) { m_onActivate = std::move(f); }
    // Right-click (fired at PUSH, the menu convention); the pair is swallowed so a right-click
    // never also selects. Unset = right-clicks do nothing.
    void setOnContextMenu(std::function<void()> f) { m_onContextMenu = std::move(f); }
    // Erase-ground for the label strips; defaults to the window ground (the CheckBox convention).
    void setGroundColor(common::Color8 c);

    // The preview cell height: h() minus the strips this card actually shows. The caller sizes
    // thumbnails against this (and the card width) before setThumbnail().
    [[nodiscard]] int previewHeight() const;

protected:
    int handle(int event) override;
    void draw() override;

private:
    // One label strip: centred while the text fits, scrolling (Marquee) once it overflows --
    // a long path/file name must stay inside the card (outside pixels are never erased).
    void drawStrip(Marquee& m, const std::string& text, int ty, int th);

    std::string m_title;
    std::string m_subtitle;
    std::vector<std::uint8_t> m_thumbRgb; // packed depth-3 RGB (see setThumbnail)
    int m_thumbW = 0;
    int m_thumbH = 0;
    PreviewFn m_previewFn;
    std::function<void()> m_onSelect;
    std::function<void()> m_onActivate;
    std::function<void()> m_onContextMenu;
    bool m_selected = false;
    bool m_hover = false;
    bool m_pushWasDouble = false; // Fl::event_clicks() at PUSH, acted on at RELEASE
    bool m_pushWasRight = false;  // a right-click pair belongs to the context menu, not select
    common::Color8 m_ground{};
    bool m_hasGround = false;
    Marquee m_titleMarquee;    // see drawStrip: overflowing lines scroll instead of
    Marquee m_subtitleMarquee; // centring past the card edges
};

// A single-line label that scrolls horizontally (bouncing, with end pauses) when its text is
// wider than the widget, and draws right-aligned when it fits. Built for the picker's
// colour-space indicator, whose ICC profile descriptions can be arbitrarily long (user-reported
// truncation); the S13-b status bar reuses it. The full text always goes to the tooltip.
// Style via labelcolor()/labelsize(). A thin widget shell over the shared Marquee above.
class ScrollingLabel : public Fl_Widget {
public:
    // When the text fits, it draws flush to this edge; while it overflows both edges scroll the
    // same way. Right is the colour-space indicator's historical look; Left suits a left-flowing
    // status message.
    enum class Align { Left, Right };

    ScrollingLabel(int X, int Y, int W, int H);

    void setText(const std::string& text);
    void setAlign(Align a) { m_align = a; }

    // Seconds to scroll the current text once from the start to the far end, including the start
    // and end dwell pauses; 0 when the text fits (no scrolling needed). A transient message holds
    // on screen at least this long so it can be read through at least once.
    [[nodiscard]] double oneScrollSeconds() const;

protected:
    void draw() override;

private:
    std::string m_text;
    Marquee m_marquee;
    Align m_align = Align::Right;
};

// The themed open-list for a Dropdown -- our flat replacement for Fl_Choice's stock Motif pulldown.
// Like ui::Popover it is a **child sub-window** of its host top-level window (on X11 a nested child,
// on native Wayland a wl_subsurface), so it has no taskbar entry, is positioned relative to its
// parent (Wayland blocks app-set top-level coords), draws above the Vulkan canvas, and is never
// orphaned. Each top-level window that hosts Dropdowns (the main window, the Settings dialog)
// creates ONE of these in its constructor *before it is shown* (a sub-window added to an
// already-realized parent is promoted to a top-level -- the very bug we are avoiding); a Dropdown
// finds its host's pop-up by top-level window. Hosts that don't create one (e.g. the colour-picker
// sub-window) leave their Dropdowns on Fl_Choice's stock list.
class DropdownPopup : public Fl_Double_Window {
public:
    DropdownPopup();
    ~DropdownPopup() override;

    void openFor(Dropdown* owner); // populate from `owner`, place over it, show
    [[nodiscard]] bool shownFor(Dropdown* owner) { return m_owner == owner && shown(); }
    // Whether (hostX, hostY) -- coords relative to the parent top-level -- lie within the pop-up or
    // its owner control (the owner is spared so a re-click can toggle the list shut).
    [[nodiscard]] bool spansHostPoint(int hostX, int hostY) const;
    void hide() override;
    // The pop-up is fixed-size: only openFor() resizes it (guarded by m_selfResize). A parent-window
    // resize must NOT stretch the open list, so external resize() calls are ignored.
    void resize(int X, int Y, int W, int H) override;

protected:
    void draw() override;
    int handle(int event) override;

private:
    void commit(int index);
    [[nodiscard]] int indexAt(int localX, int localY) const;
    void setPreviewHover(int index); // fire the owner's hoverPreview when the previewed row changes

    [[nodiscard]] int rowTop(int index) const; // y of row `index` (accounts for divider gaps)

    // A list taller than the clamped pop-up scrolls internally (the 23 blend modes vs a short window).
    // These centralise the vertical-scrollbar geometry shared by draw() and the drag in handle().
    [[nodiscard]] bool scrollable() const { return m_maxScroll > 0; }
    void vScrollGeom(int& trackTop, int& trackLen, int& thumbY, int& thumbH) const;
    void setScroll(int s); // clamp into [0, m_maxScroll] + redraw
    void paintScrollGrab(); // the themed grab (ScrollView look), drawn in the right gutter

    Dropdown* m_owner = nullptr;
    std::vector<std::string> m_items;
    std::vector<bool> m_dividers;
    std::vector<bool> m_disabled; // FL_MENU_INACTIVE rows: dim + unpickable
    // Every visible OVERFLOWING row scrolls (the ScrollingLabel precedent — user call after the
    // hover-only first cut), each with its own marquee state; lazily created per row.
    std::vector<std::unique_ptr<Marquee>> m_rowMarquees; // FL_MENU_DIVIDER per item: draw a separator hairline below it
    std::vector<int> m_marked; // indices to dot (mixed multi-selection); empty = dot m_value only
    int m_value = -1;
    int m_hover = -1;
    bool m_dragged = false;
    int m_ownerX = 0; // owner control rect (parent coords) for spansHostPoint()
    int m_ownerY = 0;
    int m_ownerW = 0;
    int m_ownerH = 0;
    bool m_selfResize = false; // true only inside openFor()'s resize() (see resize())
    int m_scroll = 0;          // vertical content offset in px (0 = first row at the top)
    int m_maxScroll = 0;       // contentHeight - viewport; 0 when the whole list fits
    int m_rowH = 24;           // per-open row height (kDdRowH, or the owner's taller preview rows)
    int m_lastPreviewIdx = -1; // last index sent to the owner's hoverPreview (so we fire only on change)
    bool m_vHover = false;     // pointer over the scrollbar gutter
    bool m_vDrag = false;      // dragging the scrollbar grab
    int m_dragGrabOffset = 0;  // px from the cursor to the grab's top edge during a drag
};

// The Dropdown pop-up currently open (at most one), or nullptr. Hosts dismiss it on an outside click
// or focus loss, mirroring the ui::Popover dismissal helpers (the open list is modeless).
[[nodiscard]] DropdownPopup* activeDropdownPopup();
void dismissActiveDropdownPopup();
// Dismiss the open list if (hostX, hostY) -- relative to the parent top-level -- is outside both it
// and its owner control. Hosts call this from their handle() on every FL_PUSH.
void dismissActiveDropdownPopupOnOutsideClick(int hostX, int hostY);

// ---- Context menu (themed right-click menu for text fields) ---------------------------------

// One row of a ContextMenu: a label + the action it runs when picked. A disabled row is shown muted
// and ignores clicks; `divider` draws a separator hairline below the row.
struct ContextAction {
    std::string label;
    std::function<void()> action;
    bool enabled = true;
    bool divider = false;
};

// A flat, themed pop-up menu -- our replacement for FLTK's stock Motif right-click menu on text
// fields (Cut / Copy / Paste / Select All). Like ui::DropdownPopup it is a **child sub-window** of
// its host top-level (a wl_subsurface on native Wayland): no taskbar entry, positioned relative to
// the parent, drawn above the Vulkan canvas, never painting over another application. Each top-level
// that hosts text fields creates ONE in its constructor *before it is shown* (a sub-window added to
// an already-realized parent is promoted to a stray top-level -- the bug we avoid); a field finds its
// menu by top_window(). Fields whose top-level created none fall back to FLTK's stock menu.
class ContextMenu : public Fl_Double_Window {
public:
    ContextMenu();
    ~ContextMenu() override;

    // Show `actions` with the menu's top-left at (hostX, hostY) in this menu's top-level coords
    // (clamped to stay fully inside the parent window).
    void openWith(int hostX, int hostY, std::vector<ContextAction> actions);
    void hide() override;
    // Whether (hostX, hostY) -- parent-top-level coords -- lies within the open menu.
    [[nodiscard]] bool spansHostPoint(int hostX, int hostY) const;

protected:
    void draw() override;
    int handle(int event) override;

private:
    [[nodiscard]] int rowAt(int localY) const;
    void commit(int row);

    std::vector<ContextAction> m_actions;
    std::vector<int> m_top; // y of each row within the pop-up
    int m_hover = -1;
};

// Find the ContextMenu hosted by `host` (a top-level window), or nullptr.
[[nodiscard]] ContextMenu* contextMenuFor(const Fl_Window* host);
[[nodiscard]] ContextMenu* activeContextMenu();
void dismissActiveContextMenu();
// Dismiss the open menu if (hostX, hostY) -- parent-top-level coords -- is outside it. Hosts call
// this from their handle() on every FL_PUSH (mirroring the Dropdown helper above).
void dismissActiveContextMenuOnOutsideClick(int hostX, int hostY);

// ---- Themed text fields ----------------------------------------------------------------------

// Shared event handling for the themed text fields below: a right-click opens our ContextMenu
// instead of FLTK's stock Motif menu, and Ctrl+C copies the *whole* value when nothing is selected
// (FLTK copies only the selection, so an un-selected field otherwise yields nothing -- the
// user-reported read-only-output case). Returns true when the event was fully consumed (the caller
// returns 1 without invoking the base). A right-click with no ContextMenu host returns false so the
// caller falls back to the stock menu. `editable` false omits Cut / Paste / Delete (read-only field).
bool handleTextFieldEvent(Fl_Input_* field, int event, bool editable);

// Caret-blink bookkeeping for the themed fields: FLTK 1.4 draws the input cursor STEADY, so the
// shared TextField wrapper reports focus traffic here (after the base, so focus acceptance is
// known) and one timer blinks the focused field's caret -- the "off" phase paints it in the
// field's own ground colour. A handled keystroke/click resets the phase to visible, so a moving
// caret never blinks away mid-typing.
void noteTextFieldFocusEvent(Fl_Input_* field, int event, int handled);

// Thin Fl_Input_ subclasses that route through handleTextFieldEvent. Drop-in replacements for the
// FLTK base input types -- construct exactly like the base. `Editable` is false for the read-only
// output so its menu is Copy + Select All only.
template <class Base, bool Editable>
class TextField : public Base {
public:
    TextField(int X, int Y, int W, int H, const char* label = nullptr) : Base(X, Y, W, H, label) {}

protected:
    int handle(int event) override {
        if (handleTextFieldEvent(this, event, Editable))
            return 1;
        const int handled = Base::handle(event);
        noteTextFieldFocusEvent(this, event, handled); // caret blink (FLTK's cursor is steady)
        return handled;
    }
};

using TextInput = TextField<Fl_Input, true>;       // a themed Fl_Input
using FloatInput = TextField<Fl_Float_Input, true>; // a themed Fl_Float_Input
using IntInput = TextField<Fl_Int_Input, true>;     // a themed Fl_Int_Input
using TextOutput = TextField<Fl_Output, false>;     // a themed, read-only Fl_Output

// The app's ONE outlined numeric value field (user 2026-07-15: the crop bar's fields and every
// dialog's must read identically -- each site had been hand-styling its own). A FloatInput that
// styles ITSELF: MOSAIC_INPUT_BOX hairline + semantic FLTK colours (so a runtime re-theme follows
// for free, the colour-picker readout pattern) at the standard 12px; and it accepts BOTH '.' and
// ',' as the decimal separator regardless of locale (S16-l, extracted from the crop bar's
// file-local NumberInput). Pair with formatFieldNumber/parseFieldNumber for locale-independent
// value round-trips.
class NumberField : public FloatInput {
public:
    NumberField(int X, int Y, int W, int H, const char* label = nullptr);

protected:
    int handle(int event) override; // ',' inserts '.'; arithmetic keys type in; unfocus evaluates

private:
    void commitExpression(); // "1024*2" -> "2048" on unfocus (no-op for plain/malformed text)
};

// Evaluate a value-field entry as simple arithmetic -- "1024*2", "(3+4)/2", "8.5-0.25" -- with
// + - * / and parentheses; a comma reads as a decimal point (the NumberField convention).
// nullopt for anything malformed/incomplete (a mid-typing "1024*"), non-finite results, or
// division by zero; a plain number evaluates to itself. Promoted from the New Document dialog
// (user 2026-07-22) so every NumberField shares one evaluator via parseFieldNumber.
[[nodiscard]] std::optional<double> evaluateFieldExpression(std::string_view text);

// Locale-independent display text for a NumberField (std::to_chars always uses '.'; step >= 1
// prints no decimals). Extracted from tool_options.cpp so every field formats identically.
[[nodiscard]] std::string formatFieldNumber(double value, double step = 0.0);
// Parse a NumberField's text to a double, accepting either separator and ignoring the process
// locale. Returns false (leaving *out untouched) on no parseable number.
[[nodiscard]] bool parseFieldNumber(const char* text, double& out);

// A drum/roller time-of-day picker (user 2026-07-15: a fractional "21.75 h" slider reads as
// nonsense -- time should LOOK like a clock). Two rolling drums -- hours 00-23 and minutes in
// 5-minute steps -- spun by a vertical drag or the mouse wheel, wrapping like the mechanical
// thing; the neighbouring values peek above and below the centre band the way a real drum shows
// its edges. value() is fractional hours in [0, 24) (what the solar solver takes); the callback
// fires on every step (FL_WHEN_CHANGED semantics).
class TimeDrum : public Fl_Widget {
public:
    TimeDrum(int X, int Y, int W, int H);

    void setValue(double hours);        // snapped onto the 5-minute grid, wrapped into [0, 24)
    [[nodiscard]] double value() const; // fractional hours

    static constexpr int kMinuteStep = 5;

protected:
    void draw() override;
    int handle(int event) override;

private:
    void spin(int drum, int delta); // step a drum (0 = hours, 1 = minutes), wrapping
    [[nodiscard]] int drumAt(int eventX) const; // -1 = neither

    int m_hour = 12;
    int m_minute = 0; // always a multiple of kMinuteStep
    int m_dragDrum = -1;
    int m_lastY = 0;
    int m_accum = 0; // drag px accumulated toward the next step
    int m_hoverDrum = -1;
};

// The redesigned hex-colour input (PLAN §9 S12): one framed row whose '#' is a fixed prefix glyph,
// with monospace hex digits in the editable part. The input's value carries no '#' (paste of a
// "#RRGGBB" still works -- callers strip it). Shared by the colour picker and the Fill colour flyout
// (S39) so the two read identically. Style is all semantic FLTK colours, so it follows a runtime
// re-theme for free. Drive it through input() (set/get the value, attach the change callback).
class HexField : public Fl_Group {
public:
    HexField(int X, int Y, int W, int H);
    [[nodiscard]] TextInput* input() { return m_input; } // the editable digits (no '#')

private:
    TextInput* m_input = nullptr;
};

// ---- Text fitting ----------------------------------------------------------------------------

// `text` shortened to fit `maxWidth` logical px in the CURRENT fl_font, with a trailing ellipsis
// when anything was dropped. Call it between fl_font() and fl_draw() -- it measures with fl_width(),
// so the font in effect is the one it fits to.
//
// This exists because fl_draw(str, x, y, w, h, align) does NOT clip to the box it is given: a name
// longer than its column simply runs on, over whatever furniture sits to the right (in the layer
// dock, the type badge and the active-layer dot). Cutting on codepoint boundaries keeps a truncated
// UTF-8 name from ending in a broken byte sequence. Returns "" when not even the ellipsis fits, so a
// vanishing column draws nothing rather than a lone stub.
[[nodiscard]] std::string ellipsizeToWidth(const std::string& text, int maxWidth);

// ---- Dialog placement --------------------------------------------------------------------------

// Centre `win` over `host` (the window the action came from) when that spot is on the screen the
// POINTER is on; otherwise centre in the pointer screen's work area. The pointer guard matters: a
// just-shown host reports 0,0 until the WM places it, so a startup dialog centred on it opens on
// the wrong display -- and the pointer is at the user's locus in every dialog-raising flow. The
// one sanctioned way to place a dialog: centring with Fl::w()/Fl::h() describes only the PRIMARY
// screen, which marooned dialogs on the left display no matter where the app was (user report
// 2026-07-16).
void centerWindowOver(Fl_Window& win, Fl_Window* host = nullptr);

// ---- Drag-and-drop payloads ------------------------------------------------------------------

// Every usable local file path in the FL_PASTE payload that follows an accepted FL_DND_RELEASE, in
// drop order: drops arrive as newline-separated URIs ("file:///home/a%20b.png\r\n...") on
// X11/Wayland, occasionally as a bare filesystem path, and a multi-file drag lists them all. Yields
// each file:// URI (percent-decoded, a "localhost"-style authority skipped) and each absolute path;
// lines carrying neither (an http:// URL, plain text) are skipped. Pure -- unit-tested.
//
// Three widgets accept file drops and each means something different by it: the empty state and the
// document tab strip open the files, the canvas places them as magic layers (S50).
[[nodiscard]] std::vector<std::string> localPathsFromDndText(std::string_view text);

// The first of those, or nullopt. For the callers that can only act on one file.
[[nodiscard]] std::optional<std::string> firstLocalPathFromDndText(std::string_view text);

} // namespace mosaic::ui
