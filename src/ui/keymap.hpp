#pragma once

#include "ui/tool.hpp" // ToolId -- the tool-selection actions are seeded from the tool registry

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Remappable keyboard shortcuts (PLAN S51-b, docs/keybindings.md).
//
// The shipped defaults are the bindings THIS APP ALREADY HAD, harvested verbatim from the three
// places they used to live -- buildMenu()'s inline accelerators, the text-editor fence in
// menu_bar.cpp, and the tool letters in tool.cpp's kToolDefs. They are deliberately NOT Photoshop's
// (the PLAN line that said so is superseded, user 2026-07-29): nothing the user already knows moves.
// tests/test_keymap.cpp pins every one of them against the literal it was harvested from, so a
// silent drift is a red test rather than a surprise.
//
// The model half of this header is FLTK-free on purpose -- the chords, their text form, the action
// table and the conflict rules are all plain data and pure functions, so they are unit-tested with
// no display. Only the three bridge functions at the bottom know FLTK exists.
namespace mosaic::ui {

// ---- Modifier bits ---------------------------------------------------------------------------
// OURS, not FLTK's. FL_COMMAND is not a constant -- it is fl_command_modifier(), a FUNCTION CALL,
// which is exactly why the fence table in menu_bar.cpp had to be a function-local static rather
// than a constexpr one. With our own bits the default table below IS constexpr, and the platform
// question gets asked in one place: toFlShortcut().
inline constexpr unsigned kModNone = 0u;
inline constexpr unsigned kModShift = 1u << 0;
// The COMMAND modifier: Ctrl on Linux/Windows, Cmd on macOS. It bridges to FL_COMMAND, which is what
// every accelerator this app has ever been written with uses -- so a harvested default reproduces
// today's behaviour on both platforms without a per-platform table. Spelled "Ctrl" in the canonical
// text form; that form is a SERIALIZATION, not a label (chordDisplayText() is what a human reads,
// and it says Cmd on macOS).
inline constexpr unsigned kModCtrl = 1u << 1;
inline constexpr unsigned kModAlt = 1u << 2;

// ---- Keys -----------------------------------------------------------------------------------
// A chord's key is either a printable ASCII character -- canonically LOWER-case for letters, so a
// chord is 'b' and never 'B'; the text form and the display label upper-case it -- or one of the
// named keys below. The named values are ours and sit above ASCII; toFlShortcut() maps them onto
// FLTK's 0xff.. keysyms.
enum : int {
    kKeyNone = 0,
    kKeyEscape = 0x101,
    kKeyTab,
    kKeyReturn,
    kKeyBackspace,
    kKeyDelete,
    kKeyInsert,
    kKeyHome,
    kKeyEnd,
    kKeyPageUp,
    kKeyPageDown,
    kKeyLeft,
    kKeyRight,
    kKeyUp,
    kKeyDown,
    kKeyF1 = 0x120, // contiguous through F12 on purpose -- see functionKey()
};
// The nth function key (1-based): functionKey(5) is F5. Contiguity is what lets the bridge map the
// whole family with one addition, the way FLTK's own FL_F + n does.
[[nodiscard]] constexpr int functionKey(int n) noexcept { return kKeyF1 + n - 1; }
// True for a key that types a character -- the distinction the conflict rules turn on, because it
// is the printable keys that the plain-letter dispatch phase and the menu accelerators fight over.
[[nodiscard]] constexpr bool isPrintableKey(int key) noexcept { return key >= 0x20 && key < 0x7f; }

struct KeyChord {
    int key = kKeyNone;
    unsigned mods = kModNone;

    [[nodiscard]] constexpr bool empty() const noexcept { return key == kKeyNone; }
    friend constexpr bool operator==(const KeyChord&, const KeyChord&) = default;
};

// The canonical, platform-independent text form and its exact inverse: "Ctrl+Shift+N", "B",
// "Ctrl+Alt+E", "Ctrl+[", "Shift+F5". Modifier order is Ctrl, Alt, Shift -- the order
// fl_shortcut_label() prints -- so the settings list and a menu row never spell one chord two ways.
// This is the ON-DISK form (it goes into settings.json), so it is never localized and must
// round-trip byte-for-byte. An empty chord is "", and "" parses back to an empty chord: that pair
// is how "the user deliberately unbound this action" is stored.
[[nodiscard]] std::string chordToText(KeyChord chord);
[[nodiscard]] std::optional<KeyChord> chordFromText(std::string_view text);

// Chords no remap may claim. Escape and Tab are load-bearing everywhere (Escape cancels the capture
// itself, dismisses every popover and is every dialog's Cancel; Tab is FLTK's focus navigation), and
// a bare Return or Space is a dialog's default button / the focused control's activation -- Space is
// also the canvas pan gesture. Handing any of them to an action would take the app apart from the
// inside, so capture refuses them rather than letting the user find out.
[[nodiscard]] bool isReservedChord(KeyChord chord);

// ---- Actions ---------------------------------------------------------------------------------
// Grouping for the settings list. Only categories that actually HOLD a binding today exist here --
// Type and Arrange have no shortcut at all, and an empty group in the list is a promise of nothing.
enum class ActionCategory : std::uint8_t {
    Tools, // the toolbar letters (V, M, B ...) -- first, because they are hit the most
    File,
    Edit,
    Image,
    Layer,
    Select,
    Filter,
    View,
    Color, // X swaps the active colours, D resets them
};
inline constexpr int kActionCategoryCount = 9;
// UNTRANSLATED group name; the settings pane wraps it in _(). Untranslated here so the model half
// stays free of gettext and the same string can key a test.
[[nodiscard]] const char* actionCategoryName(ActionCategory category);

// Which dispatch phase matches an action's chord -- and the whole reason the conflict rules below
// are more than "is this chord taken already".
//
// MenuAccel: an Fl_Menu_Item accelerator. FLTK matches these GLOBALLY and EARLY -- the focus widget
// gets FL_KEYBOARD first, but the instant it declines the chord the same keystroke returns as
// FL_SHORTCUT and the menu bar fires the item (the S53 audit that produced the text-editor fence).
// DirectKey: MainWindow::handle's "unclaimed key" phase -- the tool letters and the two colour
// keys. It runs only for chords carrying no Ctrl/Alt/Cmd, so a focused widget keeps first crack at
// every plain letter, and it reads the key CASE-INSENSITIVELY (Shift+B has always picked the Brush
// exactly like b does). Both facts are load-bearing; check() enforces them.
enum class ActionPhase : std::uint8_t { MenuAccel, DirectKey };

// One row of the compile-time default table -- the harvest itself. Kept as a POD of borrowed
// strings so the table is constexpr; Keymap turns it into the runtime Action list, translating the
// labels on the way past.
struct ActionDef {
    std::string_view id;      // stable + untranslated; this is the persistence key ("file.open")
    const char* label;        // untranslated English; the UI wraps it in _()
    ActionCategory category;
    KeyChord chord;           // the binding the app ALREADY had, verbatim (empty = unbound)
};
// The harvested table, in display order. The golden test walks it.
[[nodiscard]] std::span<const ActionDef> defaultActions();

// A registered action at run time.
struct Action {
    std::string id;
    std::string label; // translated
    ActionCategory category = ActionCategory::File;
    ActionPhase phase = ActionPhase::MenuAccel;
    KeyChord defaultChord;
    // Set on the tool-selection actions: which tool the chord activates. Absent for everything the
    // menu bar or the colour state owns.
    std::optional<ToolId> tool;
};

// The live keymap: the harvested defaults plus the user's SPARSE overrides.
//
// Sparse is the point. Only what the user actually remapped is stored, so improving a default later
// still reaches every user who never touched that action -- a full snapshot would freeze today's
// table into their settings file forever.
class Keymap {
public:
    Keymap(); // the harvested defaults; tool actions arrive via registerTool()

    // Register one tool-selection action, seeded FROM THE TOOL REGISTRY (ui/tool.cpp's kToolDefs,
    // walked through ToolManager) rather than restated here -- so a tool added to that table keeps
    // its letter with no edit to this file. `letter` is the tool's own shortcut(); the FIRST tool to
    // claim a letter wins and later ones are ignored, which is precisely toolForShortcut()'s rule
    // (the marquee / lasso / shape variants share their slot's letter, and pressing M has always
    // selected the rectangular marquee). A tool with no letter registers nothing.
    //
    // Returns false ONLY when the letter is already held by a non-tool action -- i.e. a new tool
    // picked X or D, the two colour keys. The caller logs that, because the alternative is a tool
    // whose advertised letter quietly does something else. A letter (or id) already held by another
    // TOOL is the ordinary variant case and returns true. Call every registerTool() before anything
    // holds an Action* from find(): a later one may reallocate the list.
    [[nodiscard]] bool registerTool(std::string_view id, std::string label, ToolId tool,
                                    char letter);

    [[nodiscard]] std::span<const Action> actions() const noexcept { return m_actions; }
    [[nodiscard]] const Action* find(std::string_view id) const;

    // The chord in force for `id`: the user's override when there is one, else the default.
    [[nodiscard]] KeyChord chord(std::string_view id) const;
    [[nodiscard]] bool isRemapped(std::string_view id) const;
    // toFlShortcut(chord(id)) -- what a menu item is added with. 0 for an unbound (or unknown)
    // action, which is exactly what "no accelerator" means to FLTK.
    [[nodiscard]] int accel(std::string_view id) const;

    // Which action holds `chord` right now, or nullptr. A DirectKey holder matches
    // case-insensitively and only when the chord carries no modifiers, mirroring how the
    // unclaimed-key phase actually reads the keyboard.
    [[nodiscard]] const Action* actionForChord(KeyChord chord) const;
    // The action the unclaimed-key phase should run for one typed character (the tool letters, X and
    // D). Case-insensitive, no modifiers: the exact rule MainWindow::handle has always used, now
    // asked of the keymap instead of hard-coded into it.
    [[nodiscard]] const Action* actionForDirectKey(char typed) const;

    // Why a capture was refused. Every one of these is a real explanation the dialog can show --
    // none is "that didn't work".
    enum class Conflict : std::uint8_t {
        None,
        Reserved,            // isReservedChord: Escape / Tab / plain Return / plain Space
        ModifierOnDirectKey, // a tool or colour key must be a BARE printable key (see ActionPhase)
        MenuNeedsCtrl,       // a menu accelerator on a bare letter would swallow the tool keys
        Taken,               // another action already holds this exact chord (offer to reassign)
    };
    struct ChordCheck {
        Conflict conflict = Conflict::None;
        // Taken: the action already holding the chord. MenuNeedsCtrl: the plain-key action that
        // would be swallowed, when there is one (empty when the letter is merely free today --
        // the rule still holds, because the next tool to claim that letter would die silently).
        std::string otherId;
        [[nodiscard]] bool ok() const noexcept { return conflict == Conflict::None; }
    };
    // Would binding `chord` to `id` be accepted? Pure -- the dialog asks before it applies, and the
    // test asks without a dialog. An empty chord ("unbind me") is always accepted.
    [[nodiscard]] ChordCheck check(std::string_view id, KeyChord chord) const;

    // Bind `chord` to `id`, or unbind it with an empty chord. Refuses (changing nothing, returning
    // false) whenever check() does -- except that `steal` additionally accepts a Taken chord by
    // UNBINDING the other action first, which is what the dialog's "Reassign" answer means. Nothing
    // is ever taken silently: the caller has to pass steal.
    bool rebind(std::string_view id, KeyChord chord, bool steal = false);
    void reset(std::string_view id); // drop the override; back to the harvested default
    void resetAll();

    // The sparse override set, id -> canonical chord text, for persistence. A value of "" is a
    // deliberate unbind (distinct from "not overridden", which is simply absent).
    [[nodiscard]] std::map<std::string, std::string> overrides() const;
    // Replace the override set. Entries whose id is unknown, or whose text does not parse, are
    // dropped -- a hand-edited settings file can leave the keymap odd, never broken.
    void setOverrides(const std::map<std::string, std::string>& over);

    // Fired after any change that actually moved a binding, so the host can re-apply the menu
    // accelerators (and re-mirror them into the macOS system menu bar).
    void setOnChanged(std::function<void()> cb) { m_onChanged = std::move(cb); }

private:
    // The chord in force for an action we already have in hand -- one map lookup, no linear search.
    // chord(id) is the same answer reached the long way round.
    [[nodiscard]] KeyChord chordOf(const Action& action) const;
    void notifyChanged() const;

    std::vector<Action> m_actions;
    std::map<std::string, KeyChord, std::less<>> m_over; // sparse: only what the user remapped
    std::function<void()> m_onChanged;
};

// ---- FLTK bridge -----------------------------------------------------------------------------
// The only functions here that know FLTK exists. An Fl_Shortcut is the key code OR'd with
// FL_SHIFT / FL_CTRL / FL_ALT / FL_META -- the same `int` a menu item is added with. kModCtrl maps
// onto FL_COMMAND (a function call, per the note at the top), so none of these can be constexpr.
[[nodiscard]] int toFlShortcut(KeyChord chord);
[[nodiscard]] KeyChord fromFlShortcut(int shortcut);
// What a HUMAN reads for this chord on THIS platform, via fl_shortcut_label(): the Cmd/Shift glyphs
// on macOS, "Ctrl+Shift+N" elsewhere. Never persisted -- chordToText() is the on-disk form -- and
// never parsed back, because a localized modifier name is not an identifier.
[[nodiscard]] std::string chordDisplayText(KeyChord chord);

} // namespace mosaic::ui
