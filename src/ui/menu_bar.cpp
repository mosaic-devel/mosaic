#include "ui/menu_bar.hpp"

#include "platform/native_window.hpp" // raiseNativeWindowToTop: the pop-up's z-order on Windows
#include "ui/keymap.hpp" // the text-editor fence names actions; the keymap knows their chords
#include "ui/theme.hpp"
#include "ui/widgets.hpp" // Marquee (the ticker's horizontal overflow scroll)

#ifdef __APPLE__
#  include "ui/sys_menu_macos.hpp" // applyMacMenuBadges: the badges the NSMenu mirror cannot carry
#endif

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace mosaic::ui {
namespace {

Fl_Color toFl(common::Color8 c) { return fl_rgb_color(c.r, c.g, c.b); }

common::Color8 blend8(common::Color8 a, common::Color8 b, double t) {
    const auto m = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(x + (y - x) * t));
    };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255};
}

constexpr int kRowH = 26;       // height of one menu item row
constexpr int kPad = 4;         // top/bottom inset around the rows
constexpr int kHInset = 12;     // left text inset
constexpr int kRightInset = 12; // right inset (after the shortcut / arrow)
constexpr int kGap = 28;        // min gap between a label and its right-aligned shortcut
constexpr int kArrowW = 16;     // width reserved for a submenu arrow -- or a toggle / radio on-mark
constexpr int kDivGap = 7;      // extra height a divider adds below its row
constexpr int kMinW = 168;      // a menu is at least this wide
constexpr int kFontPx = 13;
constexpr int kBadgeW = 34;     // width reserved for an item badge (fx / texture / align) right of a row
// The toggle / radio ON-MARK's opaque blit: a kMarkBlit-square patch centred kMarkCenterInset from
// the pop-up's right edge. It is painted AFTER the row text, so whatever it covers is GONE -- which
// is why menuRowGutters() reports its left edge and the shortcut stops before it.
constexpr int kMarkCenterInset = 14;
constexpr int kMarkBlit = 13;
constexpr int kBarInset = 6;    // left inset before the first title
constexpr int kTitlePad = 11;   // horizontal padding on each side of a title label

// The right-gutter on-marks (ui::itemMark). A checked FL_MENU_TOGGLE is the plain accent dot the
// layer rows use; a selected FL_MENU_RADIO is a distinctly smaller dot held inside a thin accent
// ring, which is what makes a radio group read as a radio group rather than a column of checks.
constexpr double kToggleDotR = 4.0;      // the checked-toggle dot
constexpr double kRadioDotR = 2.4;       // the selected-radio dot (air between it and the ring)
constexpr double kRadioRingR = 5.0;      // the ring's centre-line radius
constexpr double kRadioRingStroke = 1.2; // ... and its thickness (thin: a ring, not a second disc)

// Motivational ticker (docs/motivational-ticker.md). It sits in the bar's empty region past the
// titles: a `gap` after the last title, `rightInset` from the bar's right edge, ~3/4 of the leftover
// (tickerRegion()), hidden when that leftover is below `minW`. The line slides in/out vertically over
// kTickerSlideDurS, eased, at ~50 fps.
constexpr int kTickerGap = 28;
constexpr int kTickerRightInset = 12;
constexpr int kTickerMinW = 180;
constexpr int kTickerFontPx = 12;          // muted, a touch smaller than the titles: reads as chrome
constexpr double kTickerSlideTickS = 0.02; // ~50 fps while sliding in / out
constexpr double kTickerSlideDurS = 0.35;  // enter + exit slide duration

// Smoothstep easing (0->1), so the slide eases in and settles instead of moving linearly.
float tickerSmoothstep(float p) {
    p = std::clamp(p, 0.0F, 1.0F);
    return p * p * (3.0F - 2.0F * p);
}

// The label without FLTK's '&' mnemonic markers (a literal '&' is written '&&').
std::string stripAmp(const char* label) {
    std::string out;
    for (const char* p = label; p != nullptr && *p != '\0'; ++p) {
        if (*p == '&') {
            if (*(p + 1) == '&') { // "&&" -> a literal '&'
                out.push_back('&');
                ++p;
            }
            continue; // a lone '&' just marks the next char as the mnemonic
        }
        out.push_back(*p);
    }
    return out;
}

// Draw an FLTK '&'-mnemonic label with its text left edge at `textX` and BASELINE at `baseY` (the
// caller sets fl_font + fl_color). Underlines the mnemonic char iff `underline`.
void drawLabel(const char* raw, int textX, int baseY, bool underline) {
    std::string s;
    int mnem = -1; // index in `s` of the mnemonic char
    for (const char* p = raw; p != nullptr && *p != '\0'; ++p) {
        if (*p == '&') {
            if (*(p + 1) == '&') { // "&&" -> literal '&'
                s.push_back('&');
                ++p;
            } else if (mnem < 0 && *(p + 1) != '\0') {
                mnem = static_cast<int>(s.size());
            }
            continue;
        }
        s.push_back(*p);
    }
    fl_draw(s.c_str(), textX, baseY);
    if (underline && mnem >= 0) {
        const int ux = textX + static_cast<int>(fl_width(s.c_str(), mnem));
        const int uw = static_cast<int>(fl_width(s.c_str() + mnem, 1));
        fl_line(ux, baseY + 1, ux + uw - 1, baseY + 1);
    }
}

// The mnemonic char of an FLTK label, lowercased ('\0' if none). For keyboard item selection.
char mnemonicOf(const char* raw) {
    for (const char* p = raw; p != nullptr && *p != '\0'; ++p)
        if (*p == '&' && *(p + 1) != '\0' && *(p + 1) != '&')
            return static_cast<char>(std::tolower(static_cast<unsigned char>(*(p + 1))));
    return '\0';
}

// The badge pictograms, as whole-pixel rectangles in their own design box (see ui/menu_bar.hpp).
//
// The Align set is the classic alignment glyph: a 2px anchor line down (or across) one edge, and
// two bars -- one long, one short -- hugging it, in a 12x12 box. The Texture set is the layer
// dock's chip pattern: 4px cells filled on alternate squares of a 4x3 grid, so a 16x12 box.
constexpr BadgeRect kAlignLeft[]     = {{0, 0, 2, 12}, {2, 2, 10, 3}, {2, 7, 6, 3}};
constexpr BadgeRect kAlignHCenter[]  = {{5, 0, 2, 12}, {1, 2, 10, 3}, {3, 7, 6, 3}};
constexpr BadgeRect kAlignRight[]    = {{10, 0, 2, 12}, {0, 2, 10, 3}, {4, 7, 6, 3}};
constexpr BadgeRect kAlignTop[]      = {{0, 0, 12, 2}, {2, 2, 3, 10}, {7, 2, 3, 6}};
constexpr BadgeRect kAlignVMiddle[]  = {{0, 5, 12, 2}, {2, 1, 3, 10}, {7, 3, 3, 6}};
constexpr BadgeRect kAlignBottom[]   = {{0, 10, 12, 2}, {2, 0, 3, 10}, {7, 4, 3, 6}};
constexpr BadgeRect kTexture[]       = {{0, 0, 4, 4},  {8, 0, 4, 4},   // row 0: cols 0, 2
                                        {4, 4, 4, 4},  {12, 4, 4, 4},  // row 1: cols 1, 3
                                        {0, 8, 4, 4},  {8, 8, 4, 4}};  // row 2: cols 0, 2

// The Rotate / Flip family shares one motif: a solid "page" rect in a 12x12 box, as the Align set.
// Rotate90CW/CCW send a shaft out of the page's top corner and across the top, the arrowhead
// pointing the way the page turns; Rotate180 heads BOTH ends of that shaft (the page comes back
// end-for-end). FlipH / FlipV stand the solid page against its hollow mirror across a dashed axis.
constexpr BadgeRect kRotate90CW[] = {{0, 5, 7, 7}, // the page
                                     {0, 1, 2, 5}, {0, 1, 8, 2}, // riser out of it, then the shaft
                                     {8, 0, 2, 1}, {8, 1, 3, 1}, {8, 2, 4, 1},
                                     {8, 3, 3, 1}, {8, 4, 2, 1}}; // the head, pointing right
constexpr BadgeRect kRotate90CCW[] = {{5, 5, 7, 7}, // (kRotate90CW mirrored about the box's x axis)
                                      {10, 1, 2, 5}, {4, 1, 8, 2},
                                      {2, 0, 2, 1}, {1, 1, 3, 1}, {0, 2, 4, 1},
                                      {1, 3, 3, 1}, {2, 4, 2, 1}}; // the head, pointing left
constexpr BadgeRect kRotate180[] = {{3, 5, 6, 7}, {3, 1, 6, 2}, // the page, then the shaft over it
                                    {8, 0, 2, 1}, {8, 1, 3, 1}, {8, 2, 4, 1},
                                    {8, 3, 3, 1}, {8, 4, 2, 1}, // a head at the right end ...
                                    {2, 0, 2, 1}, {1, 1, 3, 1}, {0, 2, 4, 1},
                                    {1, 3, 3, 1}, {2, 4, 2, 1}}; // ... and one at the left
constexpr BadgeRect kFlipH[] = {{0, 1, 4, 10}, // the solid page
                                {5, 0, 2, 2}, {5, 3, 2, 2}, {5, 6, 2, 2}, {5, 9, 2, 3}, // the axis
                                {8, 1, 4, 1}, {8, 10, 4, 1}, // its hollow mirror twin
                                {8, 1, 1, 10}, {11, 1, 1, 10}};
constexpr BadgeRect kFlipV[] = {{1, 0, 10, 4}, // the solid page
                                {0, 5, 2, 2}, {3, 5, 2, 2}, {6, 5, 2, 2}, {9, 5, 3, 2}, // the axis
                                {1, 8, 10, 1}, {1, 11, 10, 1}, // its hollow mirror twin
                                {1, 8, 1, 4}, {10, 8, 1, 4}};

// The three sizing commands, also 12x12: Canvas Size is the content block sitting inside the canvas
// frame it is being re-cut from; Image Size is the image itself dragged out along the diagonal to a
// corner arrowhead; Trim is the printer's crop mark -- two overlapping corner rules.
constexpr BadgeRect kCanvasSize[] = {{0, 0, 12, 1}, {0, 11, 12, 1}, {0, 0, 1, 12}, {11, 0, 1, 12},
                                     {3, 3, 6, 6}}; // the canvas frame, then the content on it
constexpr BadgeRect kImageSize[] = {{0, 0, 6, 6}, // the image ...
                                    {6, 6, 2, 2}, {8, 8, 2, 2}, // ... dragged out on the diagonal
                                    {7, 10, 5, 2}, {10, 7, 2, 5}}; // ... to a corner arrowhead
constexpr BadgeRect kTrimContent[] = {{2, 0, 2, 10}, {2, 8, 10, 2}, // the lower-left crop rule
                                      {0, 2, 10, 2}, {8, 2, 2, 10}}; // the upper-right one

// The four boolean marks: two 8x8 squares overlapping in a 12x12 box, the parts that survive the
// operation inked and the rest left as a 1px outline -- the standard union / subtract / intersect /
// exclude vocabulary, and the menu-side of the same feature as the selection ops' + / - / x cursor
// alphabet (ui/cursors.cpp). A is the upper-left square, B the lower-right one.
constexpr BadgeRect kBoolUnion[] = {{0, 0, 8, 8}, {4, 4, 8, 8}}; // both shapes, solid
constexpr BadgeRect kBoolSubtract[] = {{0, 0, 8, 4}, {0, 4, 4, 4}, // A, less the overlap ...
                                       {4, 4, 8, 1}, {4, 11, 8, 1},
                                       {4, 4, 1, 8}, {11, 4, 1, 8}}; // ... and B, hollow
constexpr BadgeRect kBoolIntersect[] = {{4, 4, 4, 4}, // only the overlap is inked ...
                                        {0, 0, 8, 1}, {0, 7, 8, 1}, {0, 0, 1, 8}, {7, 0, 1, 8},
                                        {4, 4, 8, 1}, {4, 11, 8, 1}, {4, 4, 1, 8},
                                        {11, 4, 1, 8}}; // ... both shapes stay hollow around it
constexpr BadgeRect kBoolExclude[] = {{0, 0, 8, 4}, {0, 4, 4, 4}, // A, less the overlap ...
                                      {4, 8, 8, 4}, {8, 4, 4, 4}}; // ... plus B, less the overlap

// Type ▸ Orientation: three lines of running text, the last one short, laid the way the type runs.
constexpr BadgeRect kTypeHorizontal[] = {{0, 1, 12, 2}, {0, 5, 12, 2}, {0, 9, 8, 2}};
constexpr BadgeRect kTypeVertical[] = {{1, 0, 2, 12}, {5, 0, 2, 12}, {9, 0, 2, 8}};

// Fill a shape's rectangles with the current fl_color, its design box pinned at (gx, gy).
void drawBadgeRects(MenuBar::ItemBadge kind, int gx, int gy) {
    for (const BadgeRect& r : badgeShape(kind).rects)
        fl_rectf(gx + r.x, gy + r.y, r.w, r.h);
}

MenuBar* g_activeMenuBar = nullptr;

// One rendered menu entry, resolved from an Fl_Menu_Item once when the pop-up opens.
struct Entry {
    const Fl_Menu_Item* item = nullptr;
    std::string label;
    std::string shortcut; // "" if none
    int top = 0;          // y of the row within the pop-up
    bool divider = false; // a separator is drawn below this row
    bool submenu = false; // opens a nested pop-up
    bool inactive = false;
    // Width of the row's right gutter: the submenu triangle, or the on-mark an FL_MENU_TOGGLE /
    // FL_MENU_RADIO row wears (itemMark). Resolved from the item FLAGS, never from its value(), so
    // every member of a radio group keeps its label at the same x whichever member is selected.
    int markW = 0;
    // Resolved ONCE, in the width pass, and re-read by the draw pass. Asking the bar again at draw
    // time would be a second answer to the same question -- and the width pass's reservation and
    // the draw's placement disagreeing is exactly the bug menuRowGutters() exists to close.
    MenuBar::ItemBadge badge = MenuBar::ItemBadge::None;
};

} // namespace

std::span<const std::string_view> textEditorGuardedActions() {
    // The accelerators a live text editor keeps (menu_bar.hpp): every chord the S53 menus added,
    // plus the pre-existing layer-structure ones in the same class. All of them restructure the
    // DOCUMENT or the SELECTION, and none is something a user with a caret on screen is asking for
    // -- whereas Ctrl+S / Ctrl+Z / Ctrl+W deliberately stay global, and the editor already claims
    // Ctrl+C/X/V/A for itself. Copy Merged (edit.copy_merged) is deliberately absent for the same
    // reason: the Type session explicitly lets it through to the menu.
    //
    // Named by ACTION since S51-b, not by chord: the chords are the user's to move, and a fence
    // holding the old one would let the new one fire behind the caret. This list is the product
    // decision; the chord is whatever the keymap currently says.
    static constexpr std::string_view kGuarded[] = {
        "image.image_size",    // Image -> Image Size...
        "image.canvas_size",   // Image -> Canvas Size...
        "edit.paste_in_place", // Edit -> Paste in Place
        "select.reselect",     // Select -> Reselect
        "select.all_layers",   // Select -> Select All Layers
        "filter.last",         // Filter -> Last Filter
        "layer.send_backward", // Layer -> Send Backward
        "layer.bring_forward", // Layer -> Bring Forward
        "layer.new",           // Layer -> New Layer
        "layer.duplicate",     // Layer -> Duplicate Layer
        "layer.group",         // Layer -> Group Layers
        "layer.merge_down",    // Layer -> Merge Down
        "edit.fill",           // Edit -> Fill...
    };
    return kGuarded;
}

std::span<const int> textEditorGuardedShortcuts() {
    // The list above resolved through the keymap's harvested DEFAULTS -- so this function returns
    // exactly the chords it always did (FL_COMMAND+FL_ALT+'i', ... , FL_SHIFT+(FL_F+5)), in the same
    // order, and tests/test_menu_bar.cpp still pins them. The LIVE fence is the one MainWindow pushes
    // into setTextEditorGuardedShortcuts().
    //
    // A function-local static rather than a constexpr table: FL_COMMAND is a FUNCTION CALL
    // (fl_command_modifier()), so the platform picks Ctrl or Cmd at run time and none of these is
    // a constant expression. Built once, on first use.
    static const std::vector<int> kGuarded = [] {
        std::vector<int> out;
        out.reserve(textEditorGuardedActions().size());
        for (const std::string_view id : textEditorGuardedActions())
            for (const ActionDef& d : defaultActions())
                if (d.id == id) {
                    out.push_back(toFlShortcut(d.chord));
                    break;
                }
        return out;
    }();
    return std::span<const int>(kGuarded);
}

bool menuShortcutYieldsToTextEditor(int shortcut) {
    for (const int s : textEditorGuardedShortcuts())
        if (s == shortcut)
            return true;
    return false;
}

BadgeShape badgeShape(MenuBar::ItemBadge kind) {
    // No `default:` -- adding an ItemBadge without a pictogram here must fail to compile.
    switch (kind) {
    case MenuBar::ItemBadge::AlignLeft:      return {kAlignLeft, 12, 12};
    case MenuBar::ItemBadge::AlignHCenter:   return {kAlignHCenter, 12, 12};
    case MenuBar::ItemBadge::AlignRight:     return {kAlignRight, 12, 12};
    case MenuBar::ItemBadge::AlignTop:       return {kAlignTop, 12, 12};
    case MenuBar::ItemBadge::AlignVMiddle:   return {kAlignVMiddle, 12, 12};
    case MenuBar::ItemBadge::AlignBottom:    return {kAlignBottom, 12, 12};
    case MenuBar::ItemBadge::Texture:        return {kTexture, 16, 12};
    case MenuBar::ItemBadge::Rotate90CW:     return {kRotate90CW, 12, 12};
    case MenuBar::ItemBadge::Rotate90CCW:    return {kRotate90CCW, 12, 12};
    case MenuBar::ItemBadge::Rotate180:      return {kRotate180, 12, 12};
    case MenuBar::ItemBadge::FlipH:          return {kFlipH, 12, 12};
    case MenuBar::ItemBadge::FlipV:          return {kFlipV, 12, 12};
    case MenuBar::ItemBadge::CanvasSize:     return {kCanvasSize, 12, 12};
    case MenuBar::ItemBadge::ImageSize:      return {kImageSize, 12, 12};
    case MenuBar::ItemBadge::TrimContent:    return {kTrimContent, 12, 12};
    case MenuBar::ItemBadge::BoolUnion:      return {kBoolUnion, 12, 12};
    case MenuBar::ItemBadge::BoolSubtract:   return {kBoolSubtract, 12, 12};
    case MenuBar::ItemBadge::BoolIntersect:  return {kBoolIntersect, 12, 12};
    case MenuBar::ItemBadge::BoolExclude:    return {kBoolExclude, 12, 12};
    case MenuBar::ItemBadge::TypeVertical:   return {kTypeVertical, 12, 12};
    case MenuBar::ItemBadge::TypeHorizontal: return {kTypeHorizontal, 12, 12};
    case MenuBar::ItemBadge::Fx:             return {{}, 0, 0}; // set type, not a pictogram
    case MenuBar::ItemBadge::None:           break;
    }
    return {};
}

ItemMark itemMark(int flags, bool on) {
    ItemMark m;
    const bool radio = (flags & FL_MENU_RADIO) != 0;
    // FL_MENU_RADIO wins when both flags are set, matching FLTK's own Fl_Menu_Item::draw().
    const bool toggle = !radio && (flags & FL_MENU_TOGGLE) != 0;
    if (!radio && !toggle)
        return m; // a plain row: no mark, and no gutter reserved for one
    m.gutter = kArrowW; // from the FLAG, not `on`: a radio group's labels must not shift about
    if (!on)
        return m; // off draws nothing at all -- a half-lit mark would read as "disabled"
    m.dot = true;
    m.dotR = radio ? kRadioDotR : kToggleDotR;
    m.ring = radio;
    m.ringR = radio ? kRadioRingR : 0.0;
    m.ringStroke = radio ? kRadioRingStroke : 0.0;
    return m;
}

// The right-gutter chain (menu_bar.hpp). Public + pure so the arithmetic exists exactly once and is
// pinned by tests/test_menu_geometry.cpp rather than by three hand-written copies in draw().
MenuRowGutters menuRowGutters(int popupW, int markW, bool badged) {
    MenuRowGutters g;
    g.markCenterX = popupW - kMarkCenterInset;
    g.markLeft = g.markCenterX - kMarkBlit / 2;
    // The badge sits inboard of whatever gutter the row reserved for a mark or a submenu arrow.
    g.badgeRight = popupW - kRightInset - markW;
    // ... and the shortcut text inboard of the badge, when there is one.
    g.textRight = g.badgeRight - (badged ? kBadgeW : 0);
    return g;
}

// A flat, themed menu pop-up: a child sub-window rendering the direct children of one submenu item.
// Modeless (the host dismisses it on outside-click / Escape). Mouse-first.
class MenuPopup : public Fl_Double_Window {
public:
    MenuPopup() : Fl_Double_Window(0, 0, kMinW, kRowH) {
        border(0);
        color(toFl(activePalette().panelBg));
        end();
    }

    // Render `parent`'s direct children, placed at (hostX, hostY) in the top-level's coords. `sub`
    // is the pop-up to use for a nested submenu row (nullptr = no deeper nesting). `keyboard` opens
    // in keyboard mode (item mnemonic underlines shown); a mouse-opened menu stays clean until a key
    // is pressed (see handleKey) -- matches modern menus: mnemonics only when you're driving by key.
    void openFor(MenuBar* bar, const Fl_Menu_Item* parent, int hostX, int hostY, MenuPopup* sub,
                 bool keyboard) {
        m_bar = bar;
        m_sub = sub;
        m_keyboardMode = keyboard;
        m_hover = -1;
        m_entries.clear();

        fl_font(FL_HELVETICA, kFontPx);
        int contentW = 0;
        // Lay out `parent`'s DIRECT children. Walk by raw pointer (tracking submenu nesting via a
        // depth counter -- FL_SUBMENU opens a level, a null label closes it) rather than next(): we
        // must SEE the hidden rows, not skip past them, so a divider that sat on a hidden document-
        // only item survives on the last visible row instead of vanishing with that item. A visible
        // row still lays out normally; a hidden row is dropped but leaves its divider pending.
        bool pendingDivider = false; // a skipped hidden row carried a divider -> attach to last visible
        int depth = 0;
        for (const Fl_Menu_Item* c = parent + 1; c->label() != nullptr || depth > 0; ++c) {
            if (c->label() == nullptr) { // closes a nested submenu we stepped into
                --depth;
                continue;
            }
            const bool directChild = depth == 0;
            if (directChild && !c->visible()) {
                // Not laid out, but keep its group separator alive: a divider migrates to the last
                // VISIBLE row of the group rather than disappearing with the hidden item.
                if ((c->flags & FL_MENU_DIVIDER) != 0)
                    pendingDivider = true;
            } else if (directChild) {
                if (pendingDivider) { // a preceding hidden row's separator lands on the last visible row
                    if (!m_entries.empty())
                        m_entries.back().divider = true;
                    pendingDivider = false;
                }
                Entry e;
                e.item = c;
                e.label = stripAmp(c->label());
                e.submenu = c->submenu() != 0;
                e.inactive = (c->flags & FL_MENU_INACTIVE) != 0;
                e.divider = (c->flags & FL_MENU_DIVIDER) != 0;
                // The right gutter: a submenu triangle, or the on-mark an FL_MENU_TOGGLE /
                // FL_MENU_RADIO row wears. itemMark reserves it from the FLAGS (not from value()),
                // so a radio group's labels sit on one x whichever member is currently selected.
                e.markW = e.submenu ? kArrowW : itemMark(c->flags, /*on=*/false).gutter;
                if (!e.submenu && c->shortcut() != 0)
                    e.shortcut = fl_shortcut_label(c->shortcut());
                int w = static_cast<int>(fl_width(e.label.c_str()));
                if (!e.shortcut.empty())
                    w += kGap + static_cast<int>(fl_width(e.shortcut.c_str()));
                w += e.markW;
                e.badge = bar != nullptr ? bar->itemBadgeFor(c) : MenuBar::ItemBadge::None;
                if (e.badge != MenuBar::ItemBadge::None)
                    w += kBadgeW; // reserve room for the right-gutter badge (fx / texture)
                contentW = std::max(contentW, w);
                m_entries.push_back(std::move(e));
            }
            if ((c->flags & FL_SUBMENU) != 0)
                ++depth; // descend past a nested submenu's own children (they are not this menu's rows)
        }

        int y = kPad;
        for (Entry& e : m_entries) {
            e.top = y;
            y += kRowH + (e.divider ? kDivGap : 0);
        }
        const int pw = std::max(kMinW, kHInset + contentW + kRightInset);
        const int ph = y + kPad;

        int px = hostX;
        int py = hostY;
        if (const Fl_Window* parentWin = window()) { // keep fully inside the top-level window
            px = std::clamp(px, 0, std::max(0, parentWin->w() - pw));
            py = std::clamp(py, 0, std::max(0, parentWin->h() - ph));
        }
        m_selfSizing = true; // the resize below is OURS -- let it through the swallow in resize()
        resize(px, py, pw, ph);
        m_selfSizing = false;
        show();
        // Assert the z-order every time, not just the first: on Windows the Vulkan canvas is a
        // sibling sub-window deliberately sunk to HWND_BOTTOM, and a pop-up shown afterwards is
        // NOT guaranteed to come out above it -- the first submenu of a menu opened behind the
        // canvas while the next one hovered, its window by then already created, opened in front.
        // A no-op off Windows, where menus are not siblings of the canvas at all.
        platform::raiseNativeWindowToTop(this);
        redraw();     // refill when reused for a title switch (same window, new contents)
        take_focus(); // so the open menu accepts keyboard navigation (after a mnemonic opens it)
    }

    // The pop-up sizes ITSELF from its content (openFor computes pw/ph). It is a child sub-window of
    // the top-level, so Fl_Group::resize() would otherwise scale it along with the parent window on a
    // resize / maximise -- leaving a menu to open (or an open one to redraw) at a window-stretched
    // width. Swallow any resize we did not originate (m_selfSizing guards openFor's own call) so
    // submenus always render at their natural size regardless of the window's size.
    void resize(int X, int Y, int W, int H) override {
        if (!m_selfSizing)
            return;
        Fl_Double_Window::resize(X, Y, W, H);
    }

protected:
    void draw() override {
        const Palette& p = activePalette();
        fl_color(toFl(p.panelBg));
        fl_rectf(0, 0, w(), h());
        fl_font(FL_HELVETICA, kFontPx);
        // Item mnemonics underline only in keyboard mode (opened by key / a key was pressed) or
        // while Alt is held -- so a mouse-driven menu reads clean (see the Alt forwarding below).
        const bool underline = m_keyboardMode || (m_bar != nullptr && m_bar->showMnemonics());
        for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
            const Entry& e = m_entries[static_cast<std::size_t>(i)];
            if (i == m_hover && !e.inactive) {
                fl_color(toFl(p.controlHover));
                fl_rectf(1, e.top, w() - 2, kRowH);
            }
            fl_color(toFl(e.inactive ? p.textMuted : p.text));
            const int baseY = e.top + (kRowH + fl_height()) / 2 - fl_descent();
            drawLabel(e.item->label(), kHInset, baseY, underline);
            // The one right-gutter chain (menuRowGutters): mark, then badge, then the shortcut.
            const MenuRowGutters gut =
                menuRowGutters(w(), e.markW, e.badge != MenuBar::ItemBadge::None);
            if (!e.shortcut.empty()) {
                fl_color(toFl(p.textMuted));
                // Right-aligned into the box that STOPS before the reserved gutters. Aligning to
                // the pop-up's right inset instead is what let the on-mark's opaque blit repaint
                // the tail of "Ctrl+;" on View ▸ Show Guides.
                fl_draw(e.shortcut.c_str(), kHInset, e.top, gut.textRight - kHInset, kRowH,
                        FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
            }
            if (e.submenu) { // a small right-pointing triangle (no glyph -- host-font rule)
                const int ax = w() - kRightInset + 1;
                const int ay = e.top + kRowH / 2;
                fl_color(toFl(p.textMuted));
                fl_begin_polygon();
                fl_vertex(ax, ay - 4);
                fl_vertex(ax + 5, ay);
                fl_vertex(ax, ay + 4);
                fl_end_polygon();
            }
            // The right-gutter on-mark (itemMark). A checked FL_MENU_TOGGLE is the accent dot the
            // layer rows use (user choice 2026-06-17); a selected FL_MENU_RADIO is a smaller dot
            // held inside a thin accent ring, so a radio group never reads as a column of checks.
            // Either kind draws NOTHING when off -- the standard menu convention -- and since the
            // gutter was reserved from the flag, an unselected member is simply blank (not a dimmed
            // mark, which would read as disabled) with its label still aligned with the group's.
            const ItemMark mark = itemMark(e.item->flags, e.item->value() != 0);
            if (mark.dot) {
                const int dcx = gut.markCenterX;
                const int dcy = e.top + kRowH / 2;
                const double cx = static_cast<double>(dcx);
                const double cy = static_cast<double>(dcy);
                const common::Color8 bg = (i == m_hover && !e.inactive) ? p.controlHover : p.panelBg;
                std::vector<AAPrim> prims;
                if (mark.ring)
                    prims.push_back({cx, cy, mark.ringR, mark.ringStroke, p.accent});
                prims.push_back({cx, cy, mark.dotR, 0.0, p.accent});
                // OPAQUE: it repaints the row ground under it, so whatever the text pass left in
                // this square is lost. gut.markLeft is that square's left edge, and the shortcut
                // box above is cut to clear it.
                drawAAPrims(gut.markLeft, dcy - kMarkBlit / 2, kMarkBlit, kMarkBlit,
                            [&](int, int) { return bg; }, prims);
            }
            // Item badge (the layer-dock glyph, no chip outline) at the right of the row -- the
            // bold-italic "fx" on "Layer Effects…", the mini checkerboard on "Texture Generator…",
            // an alignment pictogram on each "Arrange ▸ Align …" -- as wayfinding. Drawn last so it
            // sits over the row's right gutter. A row that ALSO wears a submenu triangle or an
            // on-mark owns that gutter, so the badge steps left past it (gut.badgeRight -- the
            // width pass reserved both) rather than colliding: Type ▸ Orientation is a badged radio
            // group.
            const MenuBar::ItemBadge badge = e.badge; // resolved in the width pass (Entry::badge)
            if (badge == MenuBar::ItemBadge::Fx) {
                fl_font(FL_HELVETICA_BOLD_ITALIC, 12);
                const int fw = static_cast<int>(fl_width("fx")) + 4;
                const int gx = gut.badgeRight - fw + 2;
                fl_color(e.inactive ? toFl(p.textMuted) : toFl(blend8(p.text, p.textMuted, 0.15)));
                fl_draw("fx", gx, e.top, fw, kRowH, FL_ALIGN_CENTER);
                fl_font(FL_HELVETICA, kFontPx);  // restore the row font
            } else if (badge != MenuBar::ItemBadge::None) {
                // The pictogram badges (badgeShape): the layer dock's texture chip on Texture
                // Generator, the per-direction alignment glyph on each Arrange ▸ Align. Each is
                // right-aligned to the same right edge as the fx badge and centred in the row.
                const BadgeShape shape = badgeShape(badge);
                const int gx = gut.badgeRight + 2 - shape.w;
                const int gy = e.top + (kRowH - shape.h) / 2;
                fl_color(e.inactive ? toFl(p.textMuted) : toFl(blend8(p.text, p.textMuted, 0.15)));
                drawBadgeRects(badge, gx, gy);
            }
            if (e.divider) {
                fl_color(toFl(p.border));
                const int dy = e.top + kRowH + kDivGap / 2;
                fl_xyline(kHInset, dy, w() - kHInset);
            }
        }
        fl_color(toFl(p.border)); // crisp 1px frame on top
        fl_rect(0, 0, w(), h());
    }

    int handle(int event) override {
        switch (event) {
        case FL_PUSH: { // press only tracks hover / opens a submenu -- commit is on RELEASE, so a
                        // single click (down+up over an item) fires the action exactly once
            const int i = entryAt(Fl::event_y());
            if (i != m_hover) {
                m_hover = i;
                redraw();
            }
            activate(i, /*commit=*/false);
            return 1;
        }
        case FL_RELEASE:
            activate(entryAt(Fl::event_y()), /*commit=*/true);
            return 1;
        case FL_MOVE:
        case FL_DRAG: {
            // Hover-switch fallback: where the bar does not get FL_MOVE under our sub-window (native
            // Wayland), the motion lands here instead; if the cursor is actually over a *different*
            // bar title, drive the switch from here. Harmless on X11 (the cursor over the bar never
            // reaches us, and hoverSwitchAt only acts when over the bar).
            if (m_bar != nullptr && m_bar->hoverSwitchAt(x() + Fl::event_x(), y() + Fl::event_y()))
                return 1; // the bar switched menus (this pop-up is now showing another title)
            const int i = entryAt(Fl::event_y());
            if (i != m_hover) {
                m_hover = i;
                redraw();
                activate(i, /*commit=*/false); // hover opens / closes the nested submenu
            }
            return 1;
        }
        case FL_FOCUS:
        case FL_UNFOCUS:
            return 1; // accept keyboard focus so the open menu is navigable
        case FL_KEYBOARD:
        case FL_SHORTCUT:
            return handleKey();
        default:
            return Fl_Double_Window::handle(event);
        }
    }

private:
    int handleKey() {
        if (!m_keyboardMode) { // first key turns on keyboard mode -> reveal the item mnemonics
            m_keyboardMode = true;
            redraw();
            if (m_sub != nullptr && m_sub->shown()) {
                m_sub->m_keyboardMode = true;
                m_sub->redraw();
            }
        }
        switch (Fl::event_key()) {
        case FL_Escape:
            if (m_bar != nullptr)
                m_bar->closeMenu();
            return 1;
        case FL_Up:
            moveHover(-1);
            return 1;
        case FL_Down:
            moveHover(+1);
            return 1;
        case FL_Right: // open a submenu (if the hovered row has one)
            activate(m_hover, /*commit=*/false);
            return 1;
        case FL_Left:
            if (m_bar != nullptr)
                m_bar->closeMenu();
            return 1;
        case FL_Enter:
        case FL_KP_Enter:
        case ' ':
            activate(m_hover, /*commit=*/true);
            return 1;
        default:
            break;
        }
        const char* t = Fl::event_text(); // a letter -> the item with that mnemonic
        if (t != nullptr && t[0] != '\0' && t[1] == '\0') {
            const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
            for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
                if (!m_entries[static_cast<std::size_t>(i)].inactive &&
                    mnemonicOf(m_entries[static_cast<std::size_t>(i)].item->label()) == ch) {
                    m_hover = i;
                    redraw();
                    activate(i, /*commit=*/true);
                    return 1;
                }
        }
        return 1; // swallow other keys while the menu is open
    }

    void moveHover(int dir) {
        const int n = static_cast<int>(m_entries.size());
        if (n == 0)
            return;
        int i = m_hover;
        for (int tries = 0; tries < n; ++tries) {
            i = (i < 0) ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
            if (!m_entries[static_cast<std::size_t>(i)].inactive) {
                m_hover = i;
                redraw();
                return;
            }
        }
    }

    [[nodiscard]] int entryAt(int localY) const {
        for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
            const int t = m_entries[static_cast<std::size_t>(i)].top;
            if (localY >= t && localY < t + kRowH)
                return i;
        }
        return -1;
    }

    // Act on entry `i`: open/close its nested submenu, and (when `commit`) pick a leaf.
    void activate(int i, bool commit) {
        const bool valid = i >= 0 && i < static_cast<int>(m_entries.size());
        const Entry* e = valid ? &m_entries[static_cast<std::size_t>(i)] : nullptr;
        if (e != nullptr && e->submenu && m_sub != nullptr) {
            // Open the nested pop-up to our right, aligned to the row (top-level coords: add our x/y).
            m_sub->openFor(m_bar, e->item, x() + w() - 1, y() + e->top, nullptr, m_keyboardMode);
        } else if (m_sub != nullptr && m_sub->shown()) {
            m_sub->hide(); // moved off the submenu row
        }
        if (commit && e != nullptr && !e->submenu && !e->inactive && m_bar != nullptr) {
            const Fl_Menu_Item* item = e->item;
            m_bar->closeMenu(); // close BEFORE the callback -- it may open a modal dialog that blocks
            m_bar->dispatch(item);
        }
    }

    MenuBar* m_bar = nullptr;
    MenuPopup* m_sub = nullptr;
    std::vector<Entry> m_entries;
    int m_hover = -1;
    bool m_keyboardMode = false; // draw item mnemonic underlines (opened by key / a key was pressed)
    bool m_selfSizing = false;   // true only across openFor's own resize (see resize() above)
};

MenuBar::MenuBar(int X, int Y, int W, int H, const char* label)
    : Fl_Sys_Menu_Bar(X, Y, W, H, label), m_tickerMarquee(std::make_unique<Marquee>()) {
#ifndef __APPLE__
    // The pop-up sub-windows must be created here, before the parent window is shown, so FLTK
    // realizes them as genuine sub-surfaces (not stray top-levels). They live as children of the
    // top-level (this bar's parent), hidden until a menu opens.
    //
    // Not on macOS: the system menu bar draws the menus, so these would be two sub-windows that
    // never open. (The base constructor has already removed `this` from the parent group there,
    // but Fl_Group::current() is still the top-level, so they WOULD be built as its children.)
    m_popup = new MenuPopup();
    m_popup->hide();
    m_sub = new MenuPopup();
    m_sub->hide();
#endif
}

void MenuBar::update() {
    Fl_Sys_Menu_Bar::update(); // rebuilds the macOS system menu; inert everywhere else
#ifdef __APPLE__
    // The rebuild above replaces every NSMenuItem, so the badges go with them. Re-attach -- but
    // not during buildMenu(), where each of ~200 add() calls lands here before the host has
    // installed its badge predicate and there is nothing to attach anyway.
    if (hasItemBadges())
        applyMacMenuBadges(*this);
#endif
}

MenuBar::~MenuBar() {
    if (m_tickerTimer)
        Fl::remove_timeout(tickerTimeout, this); // never leave a timeout pointing at a freed bar
}

int MenuBar::titleWidth(const Fl_Menu_Item* item) const {
    fl_font(textfont(), textsize());
    return 2 * kTitlePad + static_cast<int>(fl_width(stripAmp(item->label()).c_str()));
}

int MenuBar::titleX(const Fl_Menu_Item* item) const {
    int tx = x() + kBarInset;
    for (const Fl_Menu_Item* t = menu(); t != nullptr && t->label() != nullptr; t = t->next()) {
        if (t == item)
            break;
        if (!t->visible())
            continue; // a hidden title occupies no cell, so it adds no width before `item`
        tx += titleWidth(t);
    }
    return tx;
}

int MenuBar::usedWidth() const {
    int w = kBarInset; // the left inset before the first title
    for (const Fl_Menu_Item* t = menu(); t != nullptr && t->label() != nullptr; t = t->next()) {
        if (!t->visible())
            continue; // hidden titles take no cell (only File + Help stay while no document is open)
        w += titleWidth(t); // titleWidth sets fl_font itself
    }
    return w;
}

const Fl_Menu_Item* MenuBar::titleAt(int barX) const {
    int tx = x() + kBarInset;
    for (const Fl_Menu_Item* t = menu(); t != nullptr && t->label() != nullptr; t = t->next()) {
        if (!t->visible())
            continue; // skip hidden titles so hit-testing matches what draw() paints
        const int tw = titleWidth(t);
        if (barX >= tx && barX < tx + tw)
            return t;
        tx += tw;
    }
    return nullptr;
}

void MenuBar::draw() {
#ifndef __APPLE__
    const Palette& p = activePalette();
    // active_r() (not active()): the bar is greyed when an inpaint run deactivates the chrome. While
    // disabled the titles read muted and no hover/open fill is painted (no interaction reaches us).
    const bool enabled = active_r();
    fl_color(toFl(p.windowBg)); // the bar ground (menu sits on the window header)
    fl_rectf(x(), y(), w(), h());
    fl_font(textfont(), textsize());
    int tx = x() + kBarInset;
    for (const Fl_Menu_Item* t = menu(); t != nullptr && t->label() != nullptr; t = t->next()) {
        if (!t->visible())
            continue; // a hidden title (document-only menus while no document is open) takes no cell
        const int tw = titleWidth(t);
        const bool open = enabled && t == m_openTitle;
        if (open) {
            fl_color(toFl(p.accent));
            fl_rectf(tx, y(), tw, h());
        } else if (enabled && t == m_hoverTitle) {
            fl_color(toFl(p.controlHover));
            fl_rectf(tx, y(), tw, h());
        }
        fl_color(toFl(!enabled ? p.textMuted : (open ? p.onAccent : p.text)));
        const int textW = static_cast<int>(fl_width(stripAmp(t->label()).c_str()));
        const int textX = tx + (tw - textW) / 2;
        const int baseY = y() + (h() + fl_height()) / 2 - fl_descent();
        drawLabel(t->label(), textX, baseY, m_showMnemonics); // underline mnemonics only when Alt held
        tx += tw;
    }
    drawTicker(); // the motivational one-liner, in the empty region past the titles (on top of ground)
#endif // the macOS bar draws nothing: the menus are the OS's, and it owns no window row
}

// ---- Motivational ticker ------------------------------------------------------------------------
// Owned + drawn by the bar itself (docs/motivational-ticker.md): no separate overlapping widget, so
// it never smears against the titles, never goes stale between its own redraws, and never disturbs
// the bar's event handling. A line slides DOWN into the row, holds, then slides UP and out.

bool MenuBar::tickerRect(int& tx, int& tw) const {
    TickerRegion r;
    if (!tickerRegion(x(), w(), usedWidth(), kTickerGap, kTickerRightInset, kTickerMinW, r))
        return false; // short window: no room for the ticker (the titles keep the whole row)
    tx = r.x;
    tw = r.w;
    return true;
}

float MenuBar::tickerSlideOffsetY() const {
    // The line enters from ABOVE the row and exits back UP; 0 at rest, -h() when fully clear above.
    const float H = static_cast<float>(h());
    const float span = static_cast<float>(kTickerSlideDurS);
    const float p = span > 0.0F ? static_cast<float>(m_tickerElapsed) / span : 1.0F;
    switch (m_tickerPhase) {
    case TickerPhase::In:
        return -(1.0F - tickerSmoothstep(p)) * H; // -H (above) -> 0 (resting)
    case TickerPhase::Out:
        return -tickerSmoothstep(p) * H; // 0 (resting) -> -H (gone above)
    case TickerPhase::Hold:
    case TickerPhase::Idle:
    default:
        return 0.0F;
    }
}

void MenuBar::drawTicker() {
    if (m_tickerText.empty() || m_tickerPhase == TickerPhase::Idle)
        return; // nothing to present -> the row's empty region stays plain ground
    int tx = 0;
    int tw = 0;
    if (!tickerRect(tx, tw))
        return;
    fl_font(FL_HELVETICA, kTickerFontPx);
    fl_color(toFl(activePalette().textMuted)); // dim inactive-chrome tone: reads as chrome, not accent
    const int dy = static_cast<int>(std::lround(tickerSlideOffsetY()));
    // Clip to the FIXED ticker rect (the bar row) so the sliding line enters/leaves cleanly and never
    // overdraws the titles; the Marquee scrolls it horizontally if the line overflows the region.
    fl_push_clip(tx, y(), tw, h());
    m_tickerMarquee->draw(this, m_tickerText.c_str(), tx, y() + dy, tw, h(),
                          /*rightAlignWhenFits=*/true);
    fl_pop_clip();
}

void MenuBar::showTickerLine(const std::string& text, double holdSeconds) {
    if (m_tickerTimer) {
        Fl::remove_timeout(tickerTimeout, this);
        m_tickerTimer = false;
    }
    m_tickerText = text;
    m_tickerMarquee->reset();
    m_tickerHold = holdSeconds;
    if (text.empty()) { // nothing to present -> leave the row empty
        m_tickerPhase = TickerPhase::Idle;
        m_tickerElapsed = 0.0;
        redraw();
        return;
    }
    m_tickerPhase = TickerPhase::In; // start above the row, slide down into place (tickerAdvance drives it)
    m_tickerElapsed = 0.0;
    Fl::add_timeout(kTickerSlideTickS, tickerTimeout, this);
    m_tickerTimer = true;
    redraw();
}

void MenuBar::clearTicker() {
    if (m_tickerTimer) {
        Fl::remove_timeout(tickerTimeout, this);
        m_tickerTimer = false;
    }
    m_tickerPhase = TickerPhase::Idle;
    m_tickerElapsed = 0.0;
    m_tickerText.clear();
    m_tickerMarquee->stop();
    redraw();
}

void MenuBar::tickerTimeout(void* self) { static_cast<MenuBar*>(self)->tickerAdvance(); }

void MenuBar::tickerAdvance() {
    m_tickerTimer = false;
    if (!visible_r()) { // the bar is hidden: drop the line rather than slide to nowhere
        m_tickerPhase = TickerPhase::Idle;
        m_tickerElapsed = 0.0;
        m_tickerText.clear();
        m_tickerMarquee->stop();
        return;
    }
    switch (m_tickerPhase) {
    case TickerPhase::In:
        m_tickerElapsed += kTickerSlideTickS;
        if (m_tickerElapsed >= kTickerSlideDurS) { // settled -> hold fully visible
            m_tickerPhase = TickerPhase::Hold;
            m_tickerElapsed = 0.0;
            Fl::add_timeout(m_tickerHold, tickerTimeout, this); // one wake at the end of the hold
            m_tickerTimer = true;
        } else {
            Fl::add_timeout(kTickerSlideTickS, tickerTimeout, this);
            m_tickerTimer = true;
        }
        break;
    case TickerPhase::Hold: // the hold elapsed -> slide back up and out
        m_tickerPhase = TickerPhase::Out;
        m_tickerElapsed = 0.0;
        Fl::add_timeout(kTickerSlideTickS, tickerTimeout, this);
        m_tickerTimer = true;
        break;
    case TickerPhase::Out:
        m_tickerElapsed += kTickerSlideTickS;
        if (m_tickerElapsed >= kTickerSlideDurS) { // gone: the row stays empty until the next line
            m_tickerPhase = TickerPhase::Idle;
            m_tickerElapsed = 0.0;
            m_tickerText.clear();
            m_tickerMarquee->stop();
        } else {
            Fl::add_timeout(kTickerSlideTickS, tickerTimeout, this);
            m_tickerTimer = true;
        }
        break;
    case TickerPhase::Idle:
        break;
    }
    redraw();
}

void MenuBar::setShowMnemonics(bool on) {
    if (on == m_showMnemonics)
        return;
    m_showMnemonics = on;
    redraw();
    // A mouse-opened menu is in mouse mode (no underlines); honouring Alt here lets the user reveal
    // the item mnemonics on demand without committing to keyboard nav. The pop-ups read showMnemonics().
    if (m_popup != nullptr && m_popup->shown())
        m_popup->redraw();
    if (m_sub != nullptr && m_sub->shown())
        m_sub->redraw();
}

bool MenuBar::hoverSwitchAt(int hostX, int hostY) {
    if (hostX < x() || hostX >= x() + w() || hostY < y() || hostY >= y() + h())
        return false; // not over the bar
    const Fl_Menu_Item* t = titleAt(hostX);
    if (t == nullptr || t == m_openTitle)
        return false;
    openTitle(t);
    return true;
}

void MenuBar::openTitle(const Fl_Menu_Item* item, bool keyboard) {
    const bool isOpen = m_popup != nullptr && m_popup->shown();
    if (item == nullptr || (isOpen && item == m_openTitle)) { // re-select the open title -> shut
        closeMenu();
        redraw();
        return;
    }
    // Switching titles must NOT hide+show the pop-up sub-window: on native Wayland a hide followed by
    // a show of the subsurface in the same iteration leaves it un-mapped -- the title goes accent-
    // "open" but no pop-up appears. Reuse the already-shown window instead (openFor repositions +
    // refills it); only the nested submenu is closed. A fresh open (nothing shown) maps it via show().
    if (m_sub != nullptr)
        m_sub->hide();
    m_openTitle = item;
    g_activeMenuBar = this;
    m_popup->openFor(this, item, titleX(item), y() + h(), m_sub, keyboard);
    redraw(); // highlight the open title
}

int MenuBar::handle(int event) {
#ifdef __APPLE__
    (void)event;
    // The widget is not in the window's group and never shown, so nothing routes here anyway. The
    // system menu bar owns the pointer; FLTK matches ⌘-shortcuts against the item array itself
    // (its own FL_SHORTCUT system handler), not through this widget.
    return 0;
#else
    // Deactivated (an inpaint run locks the chrome): take no interactive input. Crucially this drops
    // FL_SHORTCUT, so the item accelerators (Ctrl+N...) the base handler would otherwise fire stay
    // dead too. active_r() also covers an ancestor being disabled.
    if (!active_r() &&
        (event == FL_PUSH || event == FL_RELEASE || event == FL_SHORTCUT || event == FL_MOVE))
        return 0;
    switch (event) {
    case FL_PUSH:
        if (Fl::event_inside(this)) {
            openTitle(titleAt(Fl::event_x())); // our themed pop-up, not Fl_Menu_Bar's stock pulldown
            return 1;
        }
        break;
    case FL_RELEASE:
        if (Fl::event_inside(this))
            return 1; // matched our FL_PUSH; item selection happens in the pop-up's own handle()
        break;
    case FL_SHORTCUT:
        // A title mnemonic (Alt+F) opens OUR themed pop-up, not Fl_Menu_Bar's stock pulldown. Item
        // accelerators (Ctrl+N...) match no title here and fall through to the base handler below.
        for (const Fl_Menu_Item* t = menu(); t != nullptr && t->label() != nullptr; t = t->next()) {
            if (!t->visible())
                continue; // a hidden title's mnemonic must not open it (document-only menus)
            if (Fl_Widget::test_shortcut(t->label(), /*require_alt=*/true)) {
                openTitle(t, /*keyboard=*/true); // Alt+letter -> open in keyboard mode
                return 1;
            }
        }
        // A live text editor owns the keyboard: the document/selection accelerators must not fire
        // behind its caret (menu_bar.hpp explains why FLTK gets them here at all). CLAIMED, not
        // passed on -- Fl::handle_ retries FL_SHORTCUT once more with the letter's case toggled, so
        // merely declining would fire the item on the second pass.
        if (m_textEditorActive && m_textEditorActive()) {
            // The LIVE fence when the host installed one (it resolves the guarded ACTIONS through
            // the current keymap, so a remapped chord is the one fenced), else the defaults.
            const std::span<const int> fence =
                m_guarded.empty() ? textEditorGuardedShortcuts() : std::span<const int>(m_guarded);
            for (const int sc : fence)
                if (sc != 0 && Fl::test_shortcut(static_cast<Fl_Shortcut>(sc)) != 0)
                    return 1;
        }
        break;
    case FL_MOVE: {
        const Fl_Menu_Item* t = Fl::event_inside(this) ? titleAt(Fl::event_x()) : nullptr;
        if (t != m_hoverTitle) {
            m_hoverTitle = t;
            redraw();
        }
        // Hover-switch: with a menu already open, moving onto another title opens that one.
        if (m_openTitle != nullptr && t != nullptr && t != m_openTitle)
            openTitle(t);
        if (Fl::event_inside(this))
            return 1;
        break;
    }
    case FL_LEAVE:
        if (m_hoverTitle != nullptr) {
            m_hoverTitle = nullptr;
            redraw();
        }
        break;
    default:
        break;
    }
    return Fl_Menu_Bar::handle(event); // keeps accelerators (Ctrl+N...) + mnemonics
#endif
}

void MenuBar::dispatch(const Fl_Menu_Item* item) {
    if (item != nullptr)
        picked(item); // sets value() + runs the item's (or the menu's) callback, FLTK's own dispatch
}

void MenuBar::closeMenu() {
    if (m_sub != nullptr)
        m_sub->hide();
    if (m_popup != nullptr)
        m_popup->hide();
    if (m_openTitle != nullptr) {
        m_openTitle = nullptr;
        redraw(); // drop the open title's accent fill immediately (don't wait for a mouse-over)
    }
    if (g_activeMenuBar == this)
        g_activeMenuBar = nullptr;
}

bool MenuBar::spansHostPoint(int hostX, int hostY) const {
    const auto in = [&](const Fl_Widget* w) {
        return w != nullptr && hostX >= w->x() && hostX < w->x() + w->w() && hostY >= w->y() &&
               hostY < w->y() + w->h();
    };
    const bool inBar = hostX >= x() && hostX < x() + w() && hostY >= y() && hostY < y() + h();
    const bool inPopup = m_popup != nullptr && m_popup->shown() && in(m_popup);
    const bool inSub = m_sub != nullptr && m_sub->shown() && in(m_sub);
    return inBar || inPopup || inSub;
}

void dismissActiveMenu() {
    if (g_activeMenuBar != nullptr)
        g_activeMenuBar->closeMenu();
}

void dismissActiveMenuOnOutsideClick(int hostX, int hostY) {
    if (g_activeMenuBar != nullptr && !g_activeMenuBar->spansHostPoint(hostX, hostY))
        g_activeMenuBar->closeMenu();
}

} // namespace mosaic::ui
