#include "ui/keymap.hpp"
#include "ui/menu_bar.hpp" // textEditorGuardedActions / textEditorGuardedShortcuts: the fence
#include "ui/tool.hpp"

#include <doctest/doctest.h>

#include <FL/Enumerations.H>
#include <FL/platform_types.h> // FL_COMMAND == fl_command_modifier()

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>

// ---- Remappable shortcuts (PLAN S51-b, docs/keybindings.md) -----------------------------------
//
// THE GOLDEN TEST is "the harvested defaults reproduce today's behaviour exactly". The shipped
// keymap is the bindings this app already had -- NOT Photoshop's (the PLAN line saying otherwise is
// superseded, user 2026-07-29) -- so every default is asserted against the literal it was harvested
// from: FL_COMMAND + 'n' for File ▸ New, FL_SHIFT + (FL_F + 5) for Edit ▸ Fill…, and so on. If
// somebody "aligns" a binding with another editor, this file goes red and names the row.
//
// Everything here is display-free: the chords, their text form, the conflict rules and the FLTK
// bridge are plain data and pure functions. Only chordDisplayText() would need a screen, and it is
// deliberately not exercised.

using mosaic::ui::Action;
using mosaic::ui::ActionCategory;
using mosaic::ui::ActionDef;
using mosaic::ui::ActionPhase;
using mosaic::ui::chordFromText;
using mosaic::ui::chordToText;
using mosaic::ui::defaultActions;
using mosaic::ui::fromFlShortcut;
using mosaic::ui::functionKey;
using mosaic::ui::isReservedChord;
using mosaic::ui::KeyChord;
using mosaic::ui::Keymap;
using mosaic::ui::kModAlt;
using mosaic::ui::kModCtrl;
using mosaic::ui::kModNone;
using mosaic::ui::kModShift;
using mosaic::ui::toFlShortcut;
using mosaic::ui::ToolId;
using mosaic::ui::ToolManager;

namespace {

// The chord `id` ships with, as an FLTK shortcut code -- what a menu item would be added with.
int defaultAccel(const char* id) {
    for (const ActionDef& d : defaultActions())
        if (d.id == id)
            return toFlShortcut(d.chord);
    return -1; // never a legal shortcut, so a missing row fails loudly rather than matching 0
}

// A keymap with the tool letters registered the way MainWindow registers them: straight out of the
// tool registry, so this test cannot drift from tool.cpp's kToolDefs either.
Keymap keymapWithTools() {
    Keymap keys;
    ToolManager tools;
    for (const auto& tool : tools.tools()) {
        if (tool->shortcut().empty())
            continue;
        // The id shape MainWindow uses is iconKeyFor()-based; here the tool's own letter is what
        // matters, and a per-tool unique id keeps the registration honest without pulling icon_pack
        // into a keymap test.
        (void)keys.registerTool("tool." + std::to_string(static_cast<int>(tool->id())),
                                tool->name(), tool->id(), tool->shortcut()[0]);
    }
    return keys;
}

} // namespace

// ---- The harvest ------------------------------------------------------------------------------

TEST_CASE("harvested defaults: every menu accelerator is the chord the app already shipped") {
    // File. Each right-hand side is copied from buildMenu()'s own menu->add() call.
    CHECK(defaultAccel("file.new") == FL_COMMAND + 'n');
    CHECK(defaultAccel("file.open") == FL_COMMAND + 'o');
    CHECK(defaultAccel("file.save") == FL_COMMAND + 's');
    CHECK(defaultAccel("file.save_as") == FL_COMMAND + FL_SHIFT + 's');
    CHECK(defaultAccel("file.export_as") == FL_COMMAND + FL_SHIFT + 'e');
    CHECK(defaultAccel("file.quick_export_png") == FL_COMMAND + FL_ALT + 'e');
    CHECK(defaultAccel("file.close") == FL_COMMAND + 'w');
    CHECK(defaultAccel("file.quit") == FL_COMMAND + 'q');
    // Edit.
    CHECK(defaultAccel("edit.undo") == FL_COMMAND + 'z');
    CHECK(defaultAccel("edit.redo") == FL_COMMAND + FL_SHIFT + 'z');
    CHECK(defaultAccel("edit.cut") == FL_COMMAND + 'x');
    CHECK(defaultAccel("edit.copy") == FL_COMMAND + 'c');
    CHECK(defaultAccel("edit.copy_merged") == FL_COMMAND + FL_SHIFT + 'c');
    CHECK(defaultAccel("edit.paste") == FL_COMMAND + 'v');
    CHECK(defaultAccel("edit.paste_in_place") == FL_COMMAND + FL_SHIFT + 'v');
    CHECK(defaultAccel("edit.fill") == FL_SHIFT + (FL_F + 5));
    CHECK(defaultAccel("edit.settings") == FL_COMMAND + ',');
    // Image.
    CHECK(defaultAccel("image.image_size") == FL_COMMAND + FL_ALT + 'i');
    CHECK(defaultAccel("image.canvas_size") == FL_COMMAND + FL_ALT + 'c');
    // Layer.
    CHECK(defaultAccel("layer.new") == FL_COMMAND + FL_SHIFT + 'n');
    CHECK(defaultAccel("layer.duplicate") == FL_COMMAND + 'j');
    CHECK(defaultAccel("layer.group") == FL_COMMAND + 'g');
    CHECK(defaultAccel("layer.merge_down") == FL_COMMAND + 'e');
    CHECK(defaultAccel("layer.bring_forward") == FL_COMMAND + ']');
    CHECK(defaultAccel("layer.send_backward") == FL_COMMAND + '[');
    // Select.
    CHECK(defaultAccel("select.all") == FL_COMMAND + 'a');
    CHECK(defaultAccel("select.deselect") == FL_COMMAND + 'd');
    CHECK(defaultAccel("select.reselect") == FL_COMMAND + FL_SHIFT + 'd');
    CHECK(defaultAccel("select.inverse") == FL_COMMAND + FL_SHIFT + 'i');
    CHECK(defaultAccel("select.all_layers") == FL_COMMAND + FL_ALT + 'a');
    // Filter.
    CHECK(defaultAccel("filter.last") == FL_COMMAND + 'f');
    // View.
    CHECK(defaultAccel("view.zoom_in") == FL_COMMAND + '=');
    CHECK(defaultAccel("view.zoom_out") == FL_COMMAND + '-');
    CHECK(defaultAccel("view.fit_on_screen") == FL_COMMAND + '0');
    CHECK(defaultAccel("view.rulers") == FL_COMMAND + 'r');
    CHECK(defaultAccel("view.show_guides") == FL_COMMAND + ';');
}

TEST_CASE("harvested defaults: the two colour keys are the bare letters handle() answered") {
    // MainWindow::handle's unclaimed-key phase: X swaps the active colours, D resets them. Bare, and
    // matched case-insensitively -- which is why the chord carries no Shift bit.
    CHECK(defaultAccel("color.swap") == 'x');
    CHECK(defaultAccel("color.reset") == 'd');
}

TEST_CASE("harvested defaults: nothing was invented and nothing collides") {
    std::set<int> seen;
    for (const ActionDef& d : defaultActions()) {
        CAPTURE(d.id);
        CHECK_FALSE(d.chord.empty()); // a row with no chord would be a keymap entry for nothing
        // No two shipped actions may want the same chord: whichever one the dispatcher happened to
        // reach first would BE the binding, and the settings list would show a lie.
        CHECK(seen.insert(toFlShortcut(d.chord)).second);
    }
    // 38 rows: 8 File, 9 Edit, 2 Image, 6 Layer, 5 Select, 1 Filter, 5 View, 2 Color. The count is
    // pinned so a row silently ADDED (a Photoshop binding for a command that has none today) fails
    // here as well as in the table above.
    CHECK(defaultActions().size() == 38);
}

TEST_CASE("harvested defaults: the tool letters come from the tool registry, unchanged") {
    const Keymap keys = keymapWithTools();
    // Every letter kToolDefs advertises resolves to the tool ui::toolForShortcut resolves it to --
    // the keymap is SEEDED from that table, never a second copy of it.
    ToolManager tools;
    for (const auto& tool : tools.tools()) {
        if (tool->shortcut().empty())
            continue;
        const char letter = tool->shortcut()[0];
        CAPTURE(letter);
        const Action* action = keys.actionForDirectKey(letter);
        REQUIRE(action != nullptr);
        REQUIRE(action->tool.has_value());
        // A slot's variants share one letter and the FIRST claims it, exactly as toolForShortcut
        // does -- so pressing M lands on the rectangular marquee, not on whichever variant is last.
        CHECK(*action->tool == mosaic::ui::toolForShortcut(letter).value());
    }
    // Case-insensitive, like the phase that dispatches it: Shift+B has always picked the Brush.
    const Action* upper = keys.actionForDirectKey('B');
    const Action* lower = keys.actionForDirectKey('b');
    REQUIRE(upper != nullptr);
    CHECK(upper == lower);
    CHECK(*upper->tool == ToolId::Brush);
    // ... and the two colour keys still answer, alongside the tools, in the same phase.
    REQUIRE(keys.actionForDirectKey('x') != nullptr);
    CHECK(keys.actionForDirectKey('x')->id == "color.swap");
    REQUIRE(keys.actionForDirectKey('D') != nullptr);
    CHECK(keys.actionForDirectKey('D')->id == "color.reset");
}

TEST_CASE("the text-editor fence resolves through the keymap to the chords it always held") {
    // menu_bar.cpp's fence used to be a literal table; it now names ACTIONS and resolves them. The
    // resolved values must be byte-identical to the old table, in the old order.
    const std::span<const int> fence = mosaic::ui::textEditorGuardedShortcuts();
    REQUIRE(fence.size() == mosaic::ui::textEditorGuardedActions().size());
    const int expected[] = {
        FL_COMMAND + FL_ALT + 'i',   FL_COMMAND + FL_ALT + 'c', FL_COMMAND + FL_SHIFT + 'v',
        FL_COMMAND + FL_SHIFT + 'd', FL_COMMAND + FL_ALT + 'a', FL_COMMAND + 'f',
        FL_COMMAND + '[',            FL_COMMAND + ']',          FL_COMMAND + FL_SHIFT + 'n',
        FL_COMMAND + 'j',            FL_COMMAND + 'g',          FL_COMMAND + 'e',
        FL_SHIFT + (FL_F + 5),
    };
    REQUIRE(fence.size() == std::size(expected));
    for (std::size_t i = 0; i < fence.size(); ++i) {
        CAPTURE(i);
        CHECK(fence[i] == expected[i]);
    }
    // Every fenced action must actually exist in the table, or the fence quietly stops fencing.
    for (const std::string_view id : mosaic::ui::textEditorGuardedActions()) {
        CAPTURE(id);
        CHECK(defaultAccel(std::string(id).c_str()) != -1);
    }
}

// ---- Chord text: the round trip ---------------------------------------------------------------

TEST_CASE("chord text round-trips exactly") {
    const struct {
        KeyChord chord;
        const char* text;
    } cases[] = {
        {{'n', kModCtrl | kModShift}, "Ctrl+Shift+N"},
        {{'b', kModNone}, "B"},
        {{'e', kModCtrl | kModAlt}, "Ctrl+Alt+E"},
        {{'[', kModCtrl}, "Ctrl+["},
        {{']', kModCtrl}, "Ctrl+]"},
        {{',', kModCtrl}, "Ctrl+,"},
        {{'=', kModCtrl}, "Ctrl+="},
        {{'0', kModCtrl}, "Ctrl+0"},
        {{functionKey(5), kModShift}, "Shift+F5"},
        {{functionKey(12), kModNone}, "F12"},
        {{'+', kModCtrl}, "Ctrl++"}, // the awkward one: the key IS the separator
        {{' ', kModCtrl}, "Ctrl+Space"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.text);
        CHECK(chordToText(c.chord) == c.text);
        const std::optional<KeyChord> back = chordFromText(c.text);
        REQUIRE(back.has_value());
        CHECK(*back == c.chord);
        // ... and the text form is CANONICAL: parsing it and re-printing gives the same bytes, which
        // is the property settings.json depends on.
        CHECK(chordToText(*back) == c.text);
    }
}

TEST_CASE("chord text: an empty chord is the stored form of a deliberate unbind") {
    CHECK(chordToText(KeyChord{}).empty());
    const std::optional<KeyChord> empty = chordFromText("");
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("chord text: garbage does not parse, and aliases do") {
    CHECK_FALSE(chordFromText("Ctrl+").has_value());   // a modifier with nothing to modify
    CHECK_FALSE(chordFromText("Ctrl").has_value());    // ... not even a separator
    CHECK_FALSE(chordFromText("F13").has_value());     // past the family we can express
    CHECK_FALSE(chordFromText("Wiggle").has_value());  // not a key name
    // Case-insensitive, and Cmd / Command / Control all mean the one command modifier -- generous on
    // the way IN so a hand-edited file keeps working; chordToText's spelling is what goes back out.
    for (const char* alias : {"ctrl+shift+n", "CTRL+SHIFT+N", "Cmd+Shift+N", "Command+Shift+N",
                              "Control+Shift+N"}) {
        CAPTURE(alias);
        const std::optional<KeyChord> parsed = chordFromText(alias);
        REQUIRE(parsed.has_value());
        CHECK(chordToText(*parsed) == "Ctrl+Shift+N");
    }
}

TEST_CASE("the FLTK bridge round-trips every shipped default") {
    for (const ActionDef& d : defaultActions()) {
        CAPTURE(d.id);
        CHECK(fromFlShortcut(toFlShortcut(d.chord)) == d.chord);
    }
    CHECK(toFlShortcut(KeyChord{}) == 0);       // unbound == "no accelerator" to FLTK
    CHECK(fromFlShortcut(0).empty());
    CHECK(fromFlShortcut(FL_Escape) == KeyChord{mosaic::ui::kKeyEscape, kModNone});
}

// ---- Reserved chords --------------------------------------------------------------------------

TEST_CASE("reserved chords are refused with or without modifiers, as appropriate") {
    using namespace mosaic::ui;
    // Escape and Tab whatever is held: the capture's own cancel, and FLTK's focus navigation.
    CHECK(isReservedChord({kKeyEscape, kModNone}));
    CHECK(isReservedChord({kKeyEscape, kModCtrl | kModShift}));
    CHECK(isReservedChord({kKeyTab, kModNone}));
    CHECK(isReservedChord({kKeyTab, kModAlt}));
    // Return and Space only BARE -- a plain Return is a dialog's default button, a plain Space
    // activates the focused control (and pans the canvas). Ctrl+Return is nobody's default button.
    CHECK(isReservedChord({kKeyReturn, kModNone}));
    CHECK_FALSE(isReservedChord({kKeyReturn, kModCtrl}));
    CHECK(isReservedChord({' ', kModNone}));
    CHECK_FALSE(isReservedChord({' ', kModCtrl}));
    CHECK_FALSE(isReservedChord({'b', kModNone}));
}

// ---- Conflict rules ---------------------------------------------------------------------------

TEST_CASE("conflicts: a chord already bound names the other action rather than stealing it") {
    Keymap keys = keymapWithTools();
    const Keymap::ChordCheck taken = keys.check("file.new", {'o', kModCtrl});
    CHECK(taken.conflict == Keymap::Conflict::Taken);
    CHECK(taken.otherId == "file.open");
    // Without permission it changes NOTHING -- not the new action, not the old one.
    CHECK_FALSE(keys.rebind("file.new", {'o', kModCtrl}));
    CHECK(keys.accel("file.new") == FL_COMMAND + 'n');
    CHECK(keys.accel("file.open") == FL_COMMAND + 'o');
    // With permission the chord moves and the loser is left EXPLICITLY unbound, so its default
    // cannot creep back in and re-create the collision.
    CHECK(keys.rebind("file.new", {'o', kModCtrl}, /*steal=*/true));
    CHECK(keys.accel("file.new") == FL_COMMAND + 'o');
    CHECK(keys.accel("file.open") == 0);
    CHECK(keys.isRemapped("file.open"));
}

TEST_CASE("conflicts: a menu accelerator may not wear a bare letter") {
    Keymap keys = keymapWithTools();
    // THE FLTK collision: item accelerators are matched before the unclaimed-key phase, so a bare
    // (or Shift-only) letter on a menu row would swallow the tool key wearing it. Refused, and the
    // victim is named.
    const Keymap::ChordCheck bare = keys.check("file.new", {'b', kModNone});
    CHECK(bare.conflict == Keymap::Conflict::MenuNeedsCtrl);
    CHECK_FALSE(bare.otherId.empty()); // 'b' is the Brush
    const Keymap::ChordCheck shifted = keys.check("file.new", {'b', kModShift});
    CHECK(shifted.conflict == Keymap::Conflict::MenuNeedsCtrl);
    // A letter no tool holds is refused just the same: the NEXT tool to claim it would die as
    // silently, and there would be nothing on screen to explain it.
    CHECK(keys.check("file.new", {'\'', kModNone}).conflict == Keymap::Conflict::MenuNeedsCtrl);
    // Alt alone is enough (Alt+letter is a menu mnemonic's territory, but it is a real modifier);
    // and Shift + an F-key is untouched, which is what keeps Edit ▸ Fill…'s own default legal.
    CHECK(keys.check("file.new", {'b', kModAlt}).conflict == Keymap::Conflict::None);
    CHECK(keys.check("file.new", {functionKey(9), kModShift}).conflict == Keymap::Conflict::None);
    CHECK(keys.check("file.new", {functionKey(9), kModNone}).conflict == Keymap::Conflict::None);
}

TEST_CASE("conflicts: a tool or colour key must be a single bare key") {
    Keymap keys = keymapWithTools();
    const Action* brush = keys.actionForDirectKey('b');
    REQUIRE(brush != nullptr);
    CHECK(brush->phase == ActionPhase::DirectKey);
    // The unclaimed-key phase only runs for chords with no Ctrl/Alt/Cmd, so storing one would be a
    // setting that lies about what it does.
    CHECK(keys.check(brush->id, {'k', kModCtrl}).conflict == Keymap::Conflict::ModifierOnDirectKey);
    CHECK(keys.check(brush->id, {'k', kModAlt}).conflict == Keymap::Conflict::ModifierOnDirectKey);
    // It also cannot tell Shift+B from b, so Shift is refused rather than silently dropped.
    CHECK(keys.check(brush->id, {'k', kModShift}).conflict == Keymap::Conflict::ModifierOnDirectKey);
    CHECK(keys.check(brush->id, {functionKey(4), kModNone}).conflict ==
          Keymap::Conflict::ModifierOnDirectKey);
    // A free bare letter is fine, and moving the Brush frees its old one.
    CHECK(keys.check(brush->id, {'\'', kModNone}).conflict == Keymap::Conflict::None);
    const std::string brushId = brush->id;
    CHECK(keys.rebind(brushId, {'\'', kModNone}));
    CHECK(keys.actionForDirectKey('b') == nullptr);
    REQUIRE(keys.actionForDirectKey('\'') != nullptr);
    CHECK(keys.actionForDirectKey('\'')->id == brushId);
    // Ctrl+B is a legal MENU chord either way -- the Ctrl is precisely what stops it colliding with
    // the plain-letter phase -- and it stays legal now that B is free.
    CHECK(keys.check("file.new", {'b', kModCtrl}).conflict == Keymap::Conflict::None);
}

TEST_CASE("conflicts: reserved keys and unknown ids are refused") {
    Keymap keys = keymapWithTools();
    CHECK(keys.check("file.new", {mosaic::ui::kKeyEscape, kModCtrl}).conflict ==
          Keymap::Conflict::Reserved);
    CHECK(keys.check("no.such.action", {'y', kModCtrl}).conflict == Keymap::Conflict::Reserved);
    CHECK_FALSE(keys.rebind("no.such.action", {'y', kModCtrl}));
    // Unbinding is always allowed -- including for an action that has never been remapped.
    CHECK(keys.rebind("file.new", KeyChord{}));
    CHECK(keys.accel("file.new") == 0);
}

// ---- Overrides: sparse, persisted, forgiving ---------------------------------------------------

TEST_CASE("overrides are sparse: only what the user actually moved is stored") {
    Keymap keys = keymapWithTools();
    CHECK(keys.overrides().empty());
    REQUIRE(keys.rebind("file.new", {'y', kModCtrl}));
    const std::map<std::string, std::string> over = keys.overrides();
    CHECK(over.size() == 1);
    REQUIRE(over.count("file.new") == 1);
    CHECK(over.at("file.new") == "Ctrl+Y");
    // Setting a binding BACK to its default drops the override rather than recording it, so a later
    // change to that default still reaches this user.
    REQUIRE(keys.rebind("file.new", {'n', kModCtrl}));
    CHECK(keys.overrides().empty());
    CHECK_FALSE(keys.isRemapped("file.new"));
}

TEST_CASE("overrides: an explicit unbind survives the round trip as an empty string") {
    Keymap keys = keymapWithTools();
    REQUIRE(keys.rebind("file.open", KeyChord{}));
    const std::map<std::string, std::string> over = keys.overrides();
    REQUIRE(over.count("file.open") == 1);
    CHECK(over.at("file.open").empty()); // "" == deliberately unbound, distinct from absent

    Keymap fresh = keymapWithTools();
    fresh.setOverrides(over);
    CHECK(fresh.accel("file.open") == 0);
    CHECK(fresh.isRemapped("file.open"));
    CHECK(fresh.overrides() == over);
}

TEST_CASE("overrides: a hand-edited file leaves the keymap odd, never broken") {
    Keymap keys = keymapWithTools();
    keys.setOverrides({
        {"file.new", "Ctrl+Alt+Y"},   // good
        {"no.such.action", "Ctrl+Y"}, // an action this build does not have
        {"file.save", "Wiggle+Q"},    // unparseable
    });
    CHECK(keys.accel("file.new") == FL_COMMAND + FL_ALT + 'y');
    CHECK(keys.accel("file.save") == FL_COMMAND + 's'); // the default stayed in force
    const std::map<std::string, std::string> kept = keys.overrides();
    CHECK(kept.size() == 1);
    CHECK(kept.count("file.new") == 1);
}

TEST_CASE("reset and resetAll come back to the harvested defaults") {
    Keymap keys = keymapWithTools();
    REQUIRE(keys.rebind("file.new", {'y', kModCtrl}));
    REQUIRE(keys.rebind("file.open", {'u', kModCtrl}));
    keys.reset("file.new");
    CHECK(keys.accel("file.new") == FL_COMMAND + 'n');
    CHECK(keys.accel("file.open") == FL_COMMAND + 'u');
    keys.resetAll();
    CHECK(keys.overrides().empty());
    for (const ActionDef& d : defaultActions()) {
        CAPTURE(d.id);
        CHECK(keys.accel(d.id) == toFlShortcut(d.chord));
    }
}

TEST_CASE("the change hook fires exactly when a binding actually moved") {
    Keymap keys = keymapWithTools();
    int changes = 0;
    keys.setOnChanged([&changes] { ++changes; });
    CHECK(keys.rebind("file.new", {'y', kModCtrl}));
    CHECK(changes == 1);
    CHECK_FALSE(keys.rebind("file.new", {'o', kModCtrl})); // refused: Ctrl+O is File ▸ Open
    CHECK(changes == 1);
    keys.reset("file.open"); // never overridden: nothing to undo
    CHECK(changes == 1);
    keys.reset("file.new");
    CHECK(changes == 2);
    keys.resetAll(); // already clean
    CHECK(changes == 2);
    keys.setOverrides({}); // still clean
    CHECK(changes == 2);
    keys.setOverrides({{"file.save", "Ctrl+Alt+S"}});
    CHECK(changes == 3);
}

// ---- Categories -------------------------------------------------------------------------------

TEST_CASE("every action category has a name, and every action lands in one") {
    for (int i = 0; i < mosaic::ui::kActionCategoryCount; ++i) {
        CAPTURE(i);
        const char* name = mosaic::ui::actionCategoryName(static_cast<ActionCategory>(i));
        REQUIRE(name != nullptr);
        CHECK(*name != '\0'); // the settings list groups by these -- an empty heading is a bug
    }
    const Keymap keys = keymapWithTools();
    for (const Action& a : keys.actions()) {
        CAPTURE(a.id);
        CHECK(static_cast<int>(a.category) < mosaic::ui::kActionCategoryCount);
        CHECK_FALSE(a.label.empty());
        // The phase is derived, not stored twice: everything in Tools / Color rides the
        // unclaimed-key phase, everything else is a menu accelerator.
        const bool direct =
            a.category == ActionCategory::Tools || a.category == ActionCategory::Color;
        CHECK(a.phase == (direct ? ActionPhase::DirectKey : ActionPhase::MenuAccel));
        CHECK(a.tool.has_value() == (a.category == ActionCategory::Tools));
    }
}

TEST_CASE("no two live bindings collide once the tools are registered") {
    const Keymap keys = keymapWithTools();
    // The whole live set, tools included: the dispatcher would otherwise pick a winner by accident.
    std::set<int> seen;
    for (const Action& a : keys.actions()) {
        CAPTURE(a.id);
        const int accel = keys.accel(a.id);
        if (accel == 0)
            continue; // unbound by default is fine; two actions sharing a chord is not
        CHECK(seen.insert(accel).second);
    }
}
