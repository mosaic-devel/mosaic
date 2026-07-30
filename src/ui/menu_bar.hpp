#pragma once

#include <FL/Fl_Sys_Menu_Bar.H>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A themed menu bar (PLAN §12 menu-bar redesign). It keeps ALL of Fl_Menu_Bar's machinery -- the
// item data, the title row layout + drawing, title hit-testing, keyboard accelerators (Ctrl+N...),
// and mnemonics (Alt+F) -- and replaces ONLY the stock Motif pop-up menus with our flat, themed
// MenuPopup, by overriding the `play_menu()` hook (FLTK 1.4). The pop-up is a child sub-window of
// the top-level (like ui::Popover / ui::DropdownPopup): on Wayland a wl_subsurface, so it has no
// taskbar entry, is positioned relative to the parent, draws above the canvas, and never paints
// over another application's window. Build it where you'd build an Fl_Menu_Bar; it creates its
// pop-up sub-windows in the constructor, before the parent window is shown.
//
// ON macOS IT IS NONE OF THAT (S58-b). The base is Fl_Sys_Menu_Bar, which off macOS has no platform
// driver and IS Fl_Menu_Bar -- every Linux path below is byte-for-byte what it always was. On macOS
// the driver mirrors the item array into the screen-top system menu bar instead: the widget removes
// itself from its parent group in the base constructor (so it occupies no window row), draws
// nothing, and takes no events; the OS owns the menus, their ⌘-shortcuts, and their look. What the
// mirror cannot carry -- item badges, the application menu -- is added in ui/sys_menu_macos.mm.
namespace mosaic::ui {

class MenuPopup;
class Marquee;

class MenuBar : public Fl_Sys_Menu_Bar {
public:
    MenuBar(int X, int Y, int W, int H, const char* label = nullptr);
    ~MenuBar() override; // cancels the motivational-ticker slide timeout

    void dispatch(const Fl_Menu_Item* item); // a leaf was chosen: set value() + run its callback
    void closeMenu();                        // hide the pop-up(s) + clear the open-title state
    // True if (hostX, hostY) -- top-level-window coords -- lie within the open pop-up(s) or the bar
    // itself (the bar is spared so clicking a title toggles/switches rather than just dismissing).
    [[nodiscard]] bool spansHostPoint(int hostX, int hostY) const;

    // Reveal/hide the mnemonic underlines on the titles (driven by the Alt key from MainWindow).
    void setShowMnemonics(bool on);
    [[nodiscard]] bool showMnemonics() const { return m_showMnemonics; }

    // "A text editor legitimately owns the keyboard right now" -- a live Type session, an inline
    // rename editor, a focused text field. FLTK hands the focus widget FL_KEYBOARD first, but an
    // editor that DECLINES a chord (Fl_Input_ returns 0 for every Ctrl/Alt combination it has no
    // binding for; the Type session's onTextKey does the same) sees the event escalate to
    // FL_SHORTCUT, where an item accelerator fires behind the caret. The bar consults this before
    // letting the base handler match, and refuses the accelerators in
    // textEditorGuardedShortcuts() -- the document- and selection-mutating ones. Unset = the
    // historical behaviour (every accelerator always global).
    void setTextEditorActive(std::function<bool()> pred) { m_textEditorActive = std::move(pred); }

    // The chords that fence actually refuses, as live FLTK shortcut codes. Since S51-b the
    // accelerators are REMAPPABLE, so a fixed table would fence yesterday's chord and let the user's
    // new one fire behind the caret -- the exact bug the fence exists to stop. MainWindow resolves
    // textEditorGuardedActions() through the live ui::Keymap and pushes the answer here on every
    // remap (and once at start-up). An EMPTY list -- a host that never calls this, e.g. a test that
    // builds a bare MenuBar -- falls back to textEditorGuardedShortcuts(), the harvested defaults,
    // so the historical behaviour is what you get for free.
    void setTextEditorGuardedShortcuts(std::vector<int> chords) { m_guarded = std::move(chords); }

    // Which badge a pop-up item wears in its right gutter. The glyphs are the layer dock's chips
    // (one badge language): Fx = the bold-italic "fx", Texture = the mini checkerboard. The Align*
    // kinds are the classic anchor-line + two-bars alignment pictograms (Arrange ▸ Align ...), one
    // per direction, so each option is found by eye instead of by reading its label. The Rotate* /
    // Flip* kinds (Image ▸ Rotate / Flip) share one solid "page" rect so they read as one family;
    // CanvasSize / ImageSize / TrimContent mark the three sizing commands; the Bool* kinds are the
    // standard two-overlapping-shapes vocabulary -- the menu-side of the same feature as the
    // selection ops' + / - / x cursor alphabet (ui/cursors.cpp); TypeVertical / TypeHorizontal are
    // the Type ▸ Orientation pair (a radio group, so they share their row with the radio on-mark).
    enum class ItemBadge {
        None,
        Fx,
        Texture,
        AlignLeft,
        AlignHCenter,
        AlignRight,
        AlignTop,
        AlignVMiddle,
        AlignBottom,
        Rotate90CW,
        Rotate90CCW,
        Rotate180,
        FlipH,
        FlipV,
        CanvasSize,
        ImageSize,
        TrimContent,
        BoolUnion,
        BoolSubtract,
        BoolIntersect,
        BoolExclude,
        TypeVertical,
        TypeHorizontal
    };
    // Install the per-item badge predicate -- e.g. Fx for the "Layer Effects…" item, Texture for
    // "Texture Generator…". It is queried per item each time a pop-up renders (so it tracks the
    // current state); unset = no badges.
    void setItemBadge(std::function<ItemBadge(const Fl_Menu_Item*)> pred) {
        m_itemBadge = std::move(pred);
    }
    [[nodiscard]] ItemBadge itemBadgeFor(const Fl_Menu_Item* it) const {
        return (m_itemBadge && it != nullptr) ? m_itemBadge(it) : ItemBadge::None;
    }
    [[nodiscard]] bool hasItemBadges() const { return static_cast<bool>(m_itemBadge); }

    // Push item-array changes (visibility, check marks, inserted rows) to what the user sees.
    // Off macOS this is Fl_Sys_Menu_Bar's inert base -- the bar draws straight from the array, so
    // callers pair it with redraw(). On macOS the system menu is a SNAPSHOT that FLTK rebuilds
    // from scratch here, which is also why the badges have to be re-attached afterwards.
    void update() override;
    // If (hostX, hostY) -- top-level coords -- is over a *different* title than the open one, switch
    // to it and return true. An open pop-up calls this so hover-switch works even where the bar does
    // not receive FL_MOVE under the pop-up sub-window (native Wayland).
    bool hoverSwitchAt(int hostX, int hostY);

    // The pixel width the top-level titles consume, from the bar's left edge to the right edge of
    // the last title (the left inset + Σ of the title cell widths). The region past it is empty
    // chrome -- where MainWindow parks the motivational-ticker ScrollingLabel
    // (docs/motivational-ticker.md); feed it to tickerRegion() below.
    [[nodiscard]] int usedWidth() const;

    // The bar draws a "motivational one-liner" ticker in its own empty right region (past the titles;
    // docs/motivational-ticker.md): a line slides DOWN into the row, holds `holdSeconds`, then slides
    // UP and out, leaving the row empty between lines. A too-long line horizontally scrolls during the
    // hold. The bar OWNS + draws it itself (not a separate overlapping widget), so it can never smear
    // or go stale against the titles, and it never disturbs the bar's own event handling. The
    // MotivationTicker driver feeds lines here; the cadence lives there. clearTicker() stops + blanks
    // it (the Annoyances toggle went off).
    void showTickerLine(const std::string& text, double holdSeconds = 5.0);
    void clearTicker();

protected:
    // We draw the title row ourselves and intercept clicks, rather than override the play_menu()
    // hook -- FLTK 1.4.5's Fl_Menu_Bar::handle opens its pulldown inline (it does not route the
    // mouse path through play_menu), so a play_menu override never fires for a click. Drawing the
    // titles ourselves also keeps hit-testing self-consistent with what's drawn (no guessing FLTK's
    // internal title padding) and gives the flat themed look. Keyboard accelerators (Ctrl+N...) and
    // mnemonics still flow to Fl_Menu_Bar::handle(FL_SHORTCUT) untouched.
    void draw() override;
    int handle(int event) override;

private:
    // toggle / open / switch the pop-up for a title; `keyboard` true when a mnemonic/key opened it,
    // so the pop-up shows its item underlines (keyboard mode) rather than the clean mouse look.
    void openTitle(const Fl_Menu_Item* item, bool keyboard = false);
    [[nodiscard]] const Fl_Menu_Item* titleAt(int barX) const; // top-level title under an x (or null)
    [[nodiscard]] int titleWidth(const Fl_Menu_Item* item) const; // drawn width of a title cell
    [[nodiscard]] int titleX(const Fl_Menu_Item* item) const;     // left x of a title cell

    // --- Motivational ticker (drawn in the bar's empty right region; docs/motivational-ticker.md) ---
    // The vertical slide life-cycle: Idle = nothing; a line cycles In -> Hold -> Out -> Idle.
    enum class TickerPhase { Idle, In, Hold, Out };
    static void tickerTimeout(void* self);
    void tickerAdvance();                            // the slide state machine, one tick
    [[nodiscard]] float tickerSlideOffsetY() const;  // vertical px offset for the phase (0 = at rest)
    [[nodiscard]] bool tickerRect(int& tx, int& tw) const; // the ticker's sub-rect (false = too narrow)
    void drawTicker();                               // paint the current line (called from draw())

    MenuPopup* m_popup = nullptr;               // the 1st-level pop-up
    MenuPopup* m_sub = nullptr;                 // the one nested submenu level (Filter -> Blur)
    const Fl_Menu_Item* m_openTitle = nullptr;  // the title whose menu is open (toggle / highlight)
    const Fl_Menu_Item* m_hoverTitle = nullptr; // the title under the pointer (hover highlight)
    bool m_showMnemonics = false;               // draw the title underlines (Alt held)
    std::function<ItemBadge(const Fl_Menu_Item*)> m_itemBadge; // the per-item badge-kind predicate
    std::function<bool()> m_textEditorActive;   // see setTextEditorActive
    std::vector<int> m_guarded;                 // live fence; empty = the defaults (S51-b)

    std::string m_tickerText;                   // the current one-liner ("" = nothing showing)
    std::unique_ptr<Marquee> m_tickerMarquee;   // horizontal overflow scroll during the hold
    TickerPhase m_tickerPhase = TickerPhase::Idle;
    double m_tickerElapsed = 0.0;               // seconds into the active In / Out slide
    double m_tickerHold = 0.0;                  // how long the line holds fully visible
    bool m_tickerTimer = false;                 // a tickerTimeout is pending
};

// --- Item-badge pictograms -----------------------------------------------------------------
// The badges are built from whole-pixel rectangles at integer coordinates (the texture-chip rule),
// which is what keeps them crisp at any screen scale. The table lives here rather than inside the
// pop-up's drawing code because macOS draws the SAME badges into NSImages for the system menu
// (ui/sys_menu_macos.mm) -- two renderers, one badge language, no chance of drift.
struct BadgeRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};
struct BadgeShape {
    std::span<const BadgeRect> rects; // empty for None (nothing to draw) and Fx (it is set type)
    int w = 0;                        // the design box the rects are positioned in
    int h = 0;
};
[[nodiscard]] BadgeShape badgeShape(MenuBar::ItemBadge kind);
// One past the last ItemBadge value. badgeShape()'s switch has no `default:`, so the compiler is
// the real guard that every kind is covered; this is for whatever must SIZE ITSELF to the set --
// the tests' sweeps, and the macOS badge-image cache (ui/sys_menu_macos.mm), which silently drops
// any kind past its own bound. Size that cache from here, never from a second hand-written count.
inline constexpr int kItemBadgeCount = 23;

// --- Toggle / radio on-marks ---------------------------------------------------------------
// The mark a pop-up row wears in its right gutter for an FL_MENU_TOGGLE / FL_MENU_RADIO item, and
// the gutter width it costs. A checked toggle is a single filled accent dot (the mark the layer
// rows use); a SELECTED radio is a smaller dot inside a thin accent ring, so a radio group never
// reads like a column of check marks. Either kind draws NOTHING in its off state -- the standard
// menu convention -- but `gutter` comes from the item FLAGS, never from its value, so every member
// of a radio group keeps its label at the same x and an unselected member reads as "not chosen"
// rather than as a dimmed, disabled-looking mark. FL_MENU_RADIO wins if both flags are set, as in
// FLTK's own item drawing. Pure (flags in, geometry out) → the three states are unit-tested.
struct ItemMark {
    int gutter = 0;          // px the row reserves right of its label (0 = it wears no mark at all)
    bool dot = false;        // draw the filled centre dot
    double dotR = 0.0;       // ... at this radius
    bool ring = false;       // draw the thin ring around it (the radio's tell)
    double ringR = 0.0;      // the ring's centre-line radius
    double ringStroke = 0.0; // ... and its thickness
};
[[nodiscard]] ItemMark itemMark(int flags, bool on);

// Layout math for the menu-bar motivational ticker (docs/motivational-ticker.md). Given the bar's
// pixel geometry + the width its titles consume (MenuBar::usedWidth()), compute a right-anchored
// strip covering ~3/4 of the leftover empty region: a `gap` kept clear after the titles, a
// `rightInset` at the bar's right edge. Returns false -- hide the ticker -- when the leftover is
// narrower than `minW` (a short window; D3). Pure (no FLTK state), so the layout is unit-tested
// headless.
struct TickerRegion {
    int x = 0;
    int w = 0;
};
[[nodiscard]] inline bool tickerRegion(int barX, int barW, int usedWidth, int gap, int rightInset,
                                       int minW, TickerRegion& out) {
    const int leftover = barW - usedWidth - gap - rightInset;
    if (leftover < minW || leftover <= 0)
        return false;
    const int width = leftover * 3 / 4;
    if (width <= 0)
        return false;
    out.w = width;
    out.x = barX + barW - rightInset - width; // right-anchored to the bar's right inset
    return true;
}

// --- Menu pop-up row geometry: the ONE right-gutter rule ---------------------------------------
// A pop-up row hangs up to three things off its right edge, and the width pass reserves room for
// all three: the toggle/radio ON-MARK (or a submenu triangle) in the outermost gutter, an optional
// item BADGE inboard of it, and the right-aligned SHORTCUT text inboard of that. Each occupant
// therefore starts from the previous one's left edge -- one chain, computed once, here.
//
// It exists because the arithmetic was written out by hand at each draw site and the shortcut's
// copy stopped at the pop-up's right inset: it ignored both the mark gutter and the badge gutter,
// and since the on-mark is painted AFTER the text as an opaque 13x13 blit, a row with an
// accelerator AND a toggle had the tail of its shortcut repainted away. View ▸ Show Guides
// ("Ctrl+;", an FL_MENU_TOGGLE) is the row it was reported on; Image ▸ Image Size… ("Ctrl+Alt+I",
// which wears a badge) is the same collision through the badge gutter. Derive, never re-type.
struct MenuRowGutters {
    int markLeft;    // left x of the on-mark's opaque blit (what the text must clear)
    int markCenterX; // centre x of the on-mark / the submenu triangle's gutter
    int badgeRight;  // x just past the right end of an item badge on this row
    int textRight;   // x just past the right end of the row's right-aligned shortcut text
};
// `popupW` is the pop-up's width, `markW` the gutter the row reserved for its mark/arrow (0 when it
// wears neither), `badged` true when the row carries an item badge.
[[nodiscard]] MenuRowGutters menuRowGutters(int popupW, int markW, bool badged);

// --- Accelerators a live text editor keeps (the S53 shortcut audit) -------------------------
// FLTK dispatches menu item accelerators GLOBALLY: the focus widget gets FL_KEYBOARD first, but
// the moment it declines the chord the same keystroke comes back around as FL_SHORTCUT and the
// menu fires it -- behind an active caret, with no visible connection to what the user was doing.
// Fl_Input_ declines every Ctrl/Alt combination it has no binding for, and the Type session's key
// handler explicitly passes modified chords through ("Other modified accelerators (Ctrl/Cmd-Z/S…)
// pass through to the menus"), so this is not hypothetical.
//
// The answer is NOT to fence every accelerator: Ctrl+S, Ctrl+Z, Ctrl+W and friends are meant to
// work with a caret on screen, and the editor already claims the ones it wants (Ctrl+C/X/V/A). It
// is this list -- the accelerators that restructure the DOCUMENT or the SELECTION out from under
// an editing session, which is every chord the S53 menus added plus the layer-structure ones in
// the same class. Pure data + a pure predicate, so the table is pinned by a unit test rather than
// by reading the .cpp.
//
// NOTE (macOS): the system menu bar owns its ⌘-shortcuts and FLTK matches them against the item
// array through its own handler, not through MenuBar::handle -- so the guard is a Linux/Windows
// behaviour. macOS text fields get the standard AppKit key-equivalent arbitration instead.
//
// S51-b turned the ID LIST below into the source of truth: the fence names ACTIONS, because the
// chords they resolve to are now the user's to move. textEditorGuardedShortcuts() is the same list
// resolved through the keymap's harvested DEFAULTS -- unchanged values, so what it has always meant
// still holds -- and the LIVE fence is whatever MainWindow pushed into
// MenuBar::setTextEditorGuardedShortcuts().
[[nodiscard]] std::span<const std::string_view> textEditorGuardedActions();
[[nodiscard]] std::span<const int> textEditorGuardedShortcuts();
// True when `shortcut` (an FLTK shortcut code, e.g. FL_COMMAND + FL_ALT + 'i' -- the same `int` a
// menu item is added with) is in that table.
[[nodiscard]] bool menuShortcutYieldsToTextEditor(int shortcut);

// At most one menu is open at a time. Hosts dismiss it on an outside click or Escape, mirroring the
// ui::Popover / ui::DropdownPopup dismissal helpers (the open menu is modeless).
void dismissActiveMenu();
void dismissActiveMenuOnOutsideClick(int hostX, int hostY);

} // namespace mosaic::ui
