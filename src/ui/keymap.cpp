#include "ui/keymap.hpp"

#include "common/i18n.hpp"

#include <FL/Enumerations.H>
#include <FL/fl_draw.H> // fl_shortcut_label (the platform's own spelling of a chord)
#include <FL/platform_types.h> // FL_COMMAND == fl_command_modifier()

#include <algorithm>
#include <cctype>
#include <utility>

namespace mosaic::ui {
namespace {

// ---- Key names (the canonical text form) -----------------------------------------------------
// The named keys, spelled the way they are written on a keyboard. Deliberately ASCII and
// unlocalized: this table feeds settings.json, and a chord spelled "Entrée" in a German user's file
// would be unreadable to the next version of the parser. The F keys are handled arithmetically
// (kKeyF1 is contiguous through F12), so they are not listed.
struct NamedKey {
    int key;
    std::string_view name;
};
constexpr NamedKey kNamedKeys[] = {
    {kKeyEscape, "Escape"},   {kKeyTab, "Tab"},         {kKeyReturn, "Return"},
    {kKeyBackspace, "Backspace"}, {kKeyDelete, "Delete"}, {kKeyInsert, "Insert"},
    {kKeyHome, "Home"},       {kKeyEnd, "End"},         {kKeyPageUp, "PageUp"},
    {kKeyPageDown, "PageDown"}, {kKeyLeft, "Left"},     {kKeyRight, "Right"},
    {kKeyUp, "Up"},           {kKeyDown, "Down"},
    // ' ' is printable, but a chord that renders as "Ctrl+ " is unreadable and unparseable by eye,
    // so the space key gets a name of its own in BOTH directions.
    {' ', "Space"},
};

char lowerAscii(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
char upperAscii(char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }

bool equalNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) { return lowerAscii(x) == lowerAscii(y); });
}

bool startsWithNoCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && equalNoCase(text.substr(0, prefix.size()), prefix);
}

// The key's canonical name, or "" when the model cannot spell it (which makes chordToText return the
// empty string, so the round trip closes on "unbound" rather than on garbage).
std::string keyName(int key) {
    for (const NamedKey& n : kNamedKeys)
        if (n.key == key)
            return std::string(n.name);
    if (key >= kKeyF1 && key <= functionKey(12))
        return "F" + std::to_string(key - kKeyF1 + 1);
    if (isPrintableKey(key))
        return std::string(1, upperAscii(static_cast<char>(key)));
    return {};
}

// The inverse: a key name (or a single printable character) back to a model key. kKeyNone = no.
int keyFromName(std::string_view name) {
    if (name.empty())
        return kKeyNone;
    for (const NamedKey& n : kNamedKeys)
        if (equalNoCase(name, n.name))
            return n.key;
    if ((name[0] == 'F' || name[0] == 'f') && name.size() >= 2 && name.size() <= 3) {
        int n = 0;
        for (const char c : name.substr(1)) {
            if (c < '0' || c > '9')
                return kKeyNone;
            n = n * 10 + (c - '0');
        }
        if (n >= 1 && n <= 12)
            return functionKey(n);
        return kKeyNone;
    }
    if (name.size() == 1 && isPrintableKey(static_cast<unsigned char>(name[0])))
        return lowerAscii(name[0]);
    return kKeyNone;
}

// ---- The FLTK key bridge ---------------------------------------------------------------------
int flKeyFor(int key) {
    if (isPrintableKey(key))
        return key; // FLTK stores an ASCII shortcut key as the character itself
    if (key >= kKeyF1 && key <= functionKey(12))
        return FL_F + (key - kKeyF1 + 1);
    switch (key) {
    case kKeyEscape:   return FL_Escape;
    case kKeyTab:      return FL_Tab;
    case kKeyReturn:   return FL_Enter;
    case kKeyBackspace: return FL_BackSpace;
    case kKeyDelete:   return FL_Delete;
    case kKeyInsert:   return FL_Insert;
    case kKeyHome:     return FL_Home;
    case kKeyEnd:      return FL_End;
    case kKeyPageUp:   return FL_Page_Up;
    case kKeyPageDown: return FL_Page_Down;
    case kKeyLeft:     return FL_Left;
    case kKeyRight:    return FL_Right;
    case kKeyUp:       return FL_Up;
    case kKeyDown:     return FL_Down;
    default:           return 0;
    }
}

int modelKeyFor(int flKey) {
    if (isPrintableKey(flKey))
        return lowerAscii(static_cast<char>(flKey));
    if (flKey > FL_F && flKey <= FL_F + 12)
        return functionKey(flKey - FL_F);
    switch (flKey) {
    case FL_Escape:    return kKeyEscape;
    case FL_Tab:       return kKeyTab;
    case FL_Enter:     return kKeyReturn;
    case FL_KP_Enter:  return kKeyReturn; // the numeric keypad's Enter is the same key to a user
    case FL_BackSpace: return kKeyBackspace;
    case FL_Delete:    return kKeyDelete;
    case FL_Insert:    return kKeyInsert;
    case FL_Home:      return kKeyHome;
    case FL_End:       return kKeyEnd;
    case FL_Page_Up:   return kKeyPageUp;
    case FL_Page_Down: return kKeyPageDown;
    case FL_Left:      return kKeyLeft;
    case FL_Right:     return kKeyRight;
    case FL_Up:        return kKeyUp;
    case FL_Down:      return kKeyDown;
    default:           return kKeyNone;
    }
}

} // namespace

// ---- Chord text ------------------------------------------------------------------------------

std::string chordToText(KeyChord chord) {
    if (chord.empty())
        return {};
    const std::string key = keyName(chord.key);
    if (key.empty())
        return {}; // unspellable => treated as unbound, so text and chord can never disagree
    std::string out;
    // Ctrl, Alt, Shift -- fl_shortcut_label()'s own order, so one chord is never spelled two ways.
    if ((chord.mods & kModCtrl) != 0)
        out += "Ctrl+";
    if ((chord.mods & kModAlt) != 0)
        out += "Alt+";
    if ((chord.mods & kModShift) != 0)
        out += "Shift+";
    out += key;
    return out;
}

std::optional<KeyChord> chordFromText(std::string_view text) {
    KeyChord chord;
    if (text.empty())
        return chord; // "" is the stored form of a deliberate unbind
    // Modifier prefixes, consumed longest-first and case-insensitively. "Cmd"/"Command"/"Control"
    // are accepted as ALIASES of the one command bit -- a hand-edited file (or a chord copied off a
    // macOS screenshot) should not silently lose its modifier. Only chordToText()'s spelling is ever
    // written back out.
    struct Prefix {
        std::string_view name;
        unsigned bit;
    };
    static constexpr Prefix kPrefixes[] = {
        {"command+", kModCtrl}, {"control+", kModCtrl}, {"shift+", kModShift},
        {"ctrl+", kModCtrl},    {"cmd+", kModCtrl},     {"alt+", kModAlt},
    };
    for (bool matched = true; matched;) {
        matched = false;
        for (const Prefix& p : kPrefixes) {
            if (!startsWithNoCase(text, p.name))
                continue;
            chord.mods |= p.bit;
            text.remove_prefix(p.name.size());
            matched = true;
            break;
        }
    }
    const int key = keyFromName(text);
    if (key == kKeyNone)
        return std::nullopt;
    chord.key = key;
    return chord;
}

bool isReservedChord(KeyChord chord) {
    switch (chord.key) {
    case kKeyEscape:
    case kKeyTab:
        // With ANY modifiers. Escape is the capture's own cancel, every popover's dismissal and
        // every dialog's Cancel; Tab is FLTK's focus navigation, in every dialog and every panel.
        return true;
    case kKeyReturn:
    case ' ':
        // Only BARE: a plain Return is a dialog's default button and a plain Space activates the
        // focused control (and is the canvas pan gesture). Ctrl+Return is nobody's default button.
        return chord.mods == kModNone;
    default:
        return false;
    }
}

// ---- The harvested defaults -------------------------------------------------------------------

const char* actionCategoryName(ActionCategory category) {
    // N_(), not _(): these are marked for extraction HERE and translated where they are shown (the
    // settings pane wraps the return value in _()), so the model half stays gettext-free -- the same
    // split core/commands.hpp uses for undo-step names. No `default:`, so adding a category without
    // a name here must fail to compile (badgeShape's rule).
    switch (category) {
    case ActionCategory::Tools:  return N_("Tools");
    case ActionCategory::File:   return N_("File");
    case ActionCategory::Edit:   return N_("Edit");
    case ActionCategory::Image:  return N_("Image");
    case ActionCategory::Layer:  return N_("Layer");
    case ActionCategory::Select: return N_("Select");
    case ActionCategory::Filter: return N_("Filter");
    case ActionCategory::View:   return N_("View");
    case ActionCategory::Color:  return N_("Color");
    }
    return "";
}

std::span<const ActionDef> defaultActions() {
    // THE HARVEST. Every chord below is the one the app already shipped, copied from its site:
    // buildMenu()'s inline `menu->add(path, <chord>, cb, ...)` calls (app_window.cpp) for the menu
    // rows, and MainWindow::handle's unclaimed-key phase for the two colour keys. Nothing was
    // "aligned" with Photoshop, nothing was invented for an action that had no binding, and the
    // rows whose menu item carries no accelerator are simply absent -- a keymap row for an action
    // nobody can trigger from the keyboard today would be a new binding wearing a default's clothes.
    //
    // The tool letters are NOT here: they are seeded from ui/tool.cpp's kToolDefs through
    // Keymap::registerTool(), so a tool added there keeps its letter with no edit to this table.
    // Labels are N_(), not _(): marked for extraction here, translated in the Keymap constructor
    // (core/commands.hpp's undo-name split). The ids are NEVER translated -- they are the
    // persistence keys.
    static constexpr ActionDef kDefaults[] = {
        // --- File ---------------------------------------------------------------------------
        {"file.new", N_("New..."), ActionCategory::File, {'n', kModCtrl}},
        {"file.open", N_("Open..."), ActionCategory::File, {'o', kModCtrl}},
        {"file.save", N_("Save"), ActionCategory::File, {'s', kModCtrl}},
        {"file.save_as", N_("Save As..."), ActionCategory::File, {'s', kModCtrl | kModShift}},
        {"file.export_as", N_("Export As..."), ActionCategory::File, {'e', kModCtrl | kModShift}},
        {"file.quick_export_png", N_("Quick Export as PNG"), ActionCategory::File,
         {'e', kModCtrl | kModAlt}},
        {"file.close", N_("Close"), ActionCategory::File, {'w', kModCtrl}},
        // macOS puts Quit under the application's own name and FLTK builds that item itself, so
        // there is no File ▸ Quit row to re-point there; the action is still listed, because the
        // chord is real on every other platform.
        {"file.quit", N_("Quit"), ActionCategory::File, {'q', kModCtrl}},
        // --- Edit ---------------------------------------------------------------------------
        {"edit.undo", N_("Undo"), ActionCategory::Edit, {'z', kModCtrl}},
        {"edit.redo", N_("Redo"), ActionCategory::Edit, {'z', kModCtrl | kModShift}},
        {"edit.cut", N_("Cut"), ActionCategory::Edit, {'x', kModCtrl}},
        {"edit.copy", N_("Copy"), ActionCategory::Edit, {'c', kModCtrl}},
        {"edit.copy_merged", N_("Copy Merged"), ActionCategory::Edit, {'c', kModCtrl | kModShift}},
        {"edit.paste", N_("Paste"), ActionCategory::Edit, {'v', kModCtrl}},
        {"edit.paste_in_place", N_("Paste in Place"), ActionCategory::Edit,
         {'v', kModCtrl | kModShift}},
        // The one non-letter, non-Ctrl accelerator in the whole app: Shift+F5.
        {"edit.fill", N_("Fill..."), ActionCategory::Edit, {functionKey(5), kModShift}},
        // Also an application-menu item on macOS (see file.quit).
        {"edit.settings", N_("Settings..."), ActionCategory::Edit, {',', kModCtrl}},
        // --- Image --------------------------------------------------------------------------
        {"image.image_size", N_("Image Size..."), ActionCategory::Image, {'i', kModCtrl | kModAlt}},
        {"image.canvas_size", N_("Canvas Size..."), ActionCategory::Image,
         {'c', kModCtrl | kModAlt}},
        // --- Layer --------------------------------------------------------------------------
        {"layer.new", N_("New Layer"), ActionCategory::Layer, {'n', kModCtrl | kModShift}},
        {"layer.duplicate", N_("Duplicate Layer"), ActionCategory::Layer, {'j', kModCtrl}},
        {"layer.group", N_("Group Layers"), ActionCategory::Layer, {'g', kModCtrl}},
        {"layer.merge_down", N_("Merge Down"), ActionCategory::Layer, {'e', kModCtrl}},
        {"layer.bring_forward", N_("Bring Forward"), ActionCategory::Layer, {']', kModCtrl}},
        {"layer.send_backward", N_("Send Backward"), ActionCategory::Layer, {'[', kModCtrl}},
        // --- Select -------------------------------------------------------------------------
        {"select.all", N_("Select All"), ActionCategory::Select, {'a', kModCtrl}},
        {"select.deselect", N_("Deselect"), ActionCategory::Select, {'d', kModCtrl}},
        {"select.reselect", N_("Reselect"), ActionCategory::Select, {'d', kModCtrl | kModShift}},
        {"select.inverse", N_("Inverse"), ActionCategory::Select, {'i', kModCtrl | kModShift}},
        {"select.all_layers", N_("Select All Layers"), ActionCategory::Select,
         {'a', kModCtrl | kModAlt}},
        // --- Filter -------------------------------------------------------------------------
        {"filter.last", N_("Last Filter"), ActionCategory::Filter, {'f', kModCtrl}},
        // --- View ---------------------------------------------------------------------------
        {"view.zoom_in", N_("Zoom In"), ActionCategory::View, {'=', kModCtrl}},
        {"view.zoom_out", N_("Zoom Out"), ActionCategory::View, {'-', kModCtrl}},
        {"view.fit_on_screen", N_("Fit on Screen"), ActionCategory::View, {'0', kModCtrl}},
        {"view.rulers", N_("Rulers"), ActionCategory::View, {'r', kModCtrl}},
        {"view.show_guides", N_("Show Guides"), ActionCategory::View, {';', kModCtrl}},
        // --- Color --------------------------------------------------------------------------
        // The two BARE letters MainWindow::handle answers in the unclaimed-key phase, alongside the
        // tool letters. They are the only non-tool actions in that phase.
        {"color.swap", N_("Swap Foreground / Background"), ActionCategory::Color, {'x', kModNone}},
        {"color.reset", N_("Default Colors (black / white)"), ActionCategory::Color,
         {'d', kModNone}},
    };
    return kDefaults;
}

// ---- Keymap ----------------------------------------------------------------------------------

Keymap::Keymap() {
    // Reserve past the tool letters too: find() hands out Action pointers, and a registerTool()
    // reallocation would dangle them. Registration all happens at start-up, before anything holds
    // one, and the reserve makes that cheap rather than merely true.
    m_actions.reserve(defaultActions().size() + 32);
    for (const ActionDef& d : defaultActions()) {
        Action a;
        a.id = std::string(d.id);
        a.label = _(d.label);
        a.category = d.category;
        // The colour keys ride the same unclaimed-key phase as the tool letters; everything else is
        // a menu item accelerator. Derived rather than stored: the category IS the phase here, and a
        // second field to keep in step would be a second thing to get wrong.
        a.phase = d.category == ActionCategory::Color ? ActionPhase::DirectKey
                                                     : ActionPhase::MenuAccel;
        a.defaultChord = d.chord;
        m_actions.push_back(std::move(a));
    }
}

bool Keymap::registerTool(std::string_view id, std::string label, ToolId tool, char letter) {
    if (letter == '\0')
        return true; // a tool with no letter registers nothing, and that is not a failure
    const int key = lowerAscii(letter);
    if (!isPrintableKey(key))
        return false;
    // First claim wins -- toolForShortcut()'s rule, which is why the marquee / lasso / shape
    // variants collapse onto their slot's single letter (and why pressing M has always selected the
    // rectangular marquee). A letter already held by ANOTHER TOOL, or an id already registered, is
    // therefore the ordinary variant case and no kind of failure.
    for (const Action& a : m_actions) {
        if (a.phase != ActionPhase::DirectKey || a.defaultChord.key != key)
            continue;
        // ... but a letter held by a NON-tool action -- today X (swap colours) and D (default
        // colours) -- means a new tool advertises a letter that does something else entirely. Two
        // actions sharing one plain letter would make the dispatch ORDER the binding, so the tool
        // gets no keymap row and the caller is told, loudly.
        return a.tool.has_value();
    }
    if (find(id) != nullptr)
        return true; // same id from a second variant: nothing to add, nothing wrong
    Action a;
    a.id = std::string(id);
    a.label = std::move(label);
    a.category = ActionCategory::Tools;
    a.phase = ActionPhase::DirectKey;
    a.defaultChord = KeyChord{key, kModNone};
    a.tool = tool;
    m_actions.push_back(std::move(a));
    return true;
}

const Action* Keymap::find(std::string_view id) const {
    for (const Action& a : m_actions)
        if (a.id == id)
            return &a;
    return nullptr;
}

KeyChord Keymap::chordOf(const Action& action) const {
    const auto it = m_over.find(action.id);
    return it != m_over.end() ? it->second : action.defaultChord;
}

KeyChord Keymap::chord(std::string_view id) const {
    const Action* a = find(id);
    return a != nullptr ? chordOf(*a) : KeyChord{};
}

bool Keymap::isRemapped(std::string_view id) const { return m_over.find(id) != m_over.end(); }

int Keymap::accel(std::string_view id) const { return toFlShortcut(chord(id)); }

const Action* Keymap::actionForChord(KeyChord chord) const {
    if (chord.empty())
        return nullptr;
    for (const Action& a : m_actions) {
        const KeyChord bound = chordOf(a);
        if (bound.empty())
            continue;
        if (a.phase == ActionPhase::DirectKey) {
            // The unclaimed-key phase is only reached with no Ctrl/Alt/Cmd, and it reads the typed
            // character -- so it cannot tell Shift+B from b. Matching that here is what makes
            // "already bound" mean the same thing to the dialog as it does to the dispatcher.
            if (chord.mods == kModNone && bound.key == chord.key)
                return &a;
        } else if (bound == chord) {
            return &a;
        }
    }
    return nullptr;
}

const Action* Keymap::actionForDirectKey(char typed) const {
    const int key = lowerAscii(typed);
    if (!isPrintableKey(key))
        return nullptr;
    return actionForChord(KeyChord{key, kModNone});
}

Keymap::ChordCheck Keymap::check(std::string_view id, KeyChord chord) const {
    ChordCheck out;
    const Action* self = find(id);
    if (self == nullptr) {
        out.conflict = Conflict::Reserved; // an id we do not know: refuse, never invent a binding
        return out;
    }
    if (chord.empty())
        return out; // "unbind this action" is always allowed
    if (isReservedChord(chord)) {
        out.conflict = Conflict::Reserved;
        return out;
    }
    if (self->phase == ActionPhase::DirectKey) {
        // A tool / colour key is matched in the unclaimed-key phase, which only runs for chords
        // carrying no Ctrl/Alt/Cmd. Accepting one anyway would store a binding the dispatcher can
        // never see -- a setting that lies.
        if (chord.mods != kModNone || !isPrintableKey(chord.key)) {
            out.conflict = Conflict::ModifierOnDirectKey;
            return out;
        }
    } else if (isPrintableKey(chord.key) && (chord.mods & (kModCtrl | kModAlt)) == 0) {
        // THE FLTK collision. A menu item accelerator is matched before the unclaimed-key phase, so
        // a bare (or Shift-only) letter on a menu row would eat that letter: the tool key wearing it
        // simply stops working, with nothing on screen to explain why. Refused outright rather than
        // only when a tool holds the letter today -- the next tool to claim it would die just as
        // silently. Shift+F5 is untouched: F5 is not a printable key.
        out.conflict = Conflict::MenuNeedsCtrl;
        if (const Action* victim = actionForDirectKey(static_cast<char>(chord.key)))
            out.otherId = victim->id;
        return out;
    }
    if (const Action* other = actionForChord(chord); other != nullptr && other->id != self->id) {
        out.conflict = Conflict::Taken;
        out.otherId = other->id;
    }
    return out;
}

bool Keymap::rebind(std::string_view id, KeyChord chord, bool steal) {
    const ChordCheck v = check(id, chord);
    if (!v.ok() && !(steal && v.conflict == Conflict::Taken))
        return false;
    const Action* self = find(id);
    if (self == nullptr)
        return false;
    if (v.conflict == Conflict::Taken) {
        // An EXPLICIT empty override on the loser, not an erase: erasing would let its harvested
        // default creep back in and re-create the collision the reassign just resolved.
        m_over[v.otherId] = KeyChord{};
    }
    if (chord == self->defaultChord) {
        if (const auto it = m_over.find(id); it != m_over.end())
            m_over.erase(it); // back at the default: the override stops existing (sparse, §4)
    } else {
        m_over[std::string(id)] = chord;
    }
    notifyChanged();
    return true;
}

void Keymap::reset(std::string_view id) {
    const auto it = m_over.find(id);
    if (it == m_over.end())
        return;
    m_over.erase(it);
    notifyChanged();
}

void Keymap::resetAll() {
    if (m_over.empty())
        return;
    m_over.clear();
    notifyChanged();
}

std::map<std::string, std::string> Keymap::overrides() const {
    std::map<std::string, std::string> out;
    for (const auto& [id, chord] : m_over)
        out.emplace(id, chordToText(chord)); // "" for a deliberate unbind
    return out;
}

void Keymap::setOverrides(const std::map<std::string, std::string>& over) {
    std::map<std::string, KeyChord, std::less<>> parsed;
    for (const auto& [id, text] : over) {
        if (find(id) == nullptr)
            continue; // an action this build does not have (an older/newer settings file)
        const std::optional<KeyChord> chord = chordFromText(text);
        if (!chord.has_value())
            continue; // unparseable: leave the default in force rather than guess
        parsed.emplace(id, *chord);
    }
    if (parsed == m_over)
        return;
    m_over = std::move(parsed);
    notifyChanged();
}

void Keymap::notifyChanged() const {
    if (m_onChanged)
        m_onChanged();
}

// ---- FLTK bridge -----------------------------------------------------------------------------

int toFlShortcut(KeyChord chord) {
    if (chord.empty())
        return 0;
    const int key = flKeyFor(chord.key);
    if (key == 0)
        return 0;
    int shortcut = key;
    // FL_COMMAND is fl_command_modifier() -- Ctrl here, Cmd on macOS. Asked exactly once, here.
    if ((chord.mods & kModCtrl) != 0)
        shortcut |= FL_COMMAND;
    if ((chord.mods & kModAlt) != 0)
        shortcut |= FL_ALT;
    if ((chord.mods & kModShift) != 0)
        shortcut |= FL_SHIFT;
    return shortcut;
}

KeyChord fromFlShortcut(int shortcut) {
    KeyChord chord;
    if (shortcut == 0)
        return chord;
    const auto bits = static_cast<unsigned>(shortcut);
    chord.key = modelKeyFor(static_cast<int>(bits & FL_KEY_MASK));
    if (chord.key == kKeyNone)
        return {}; // a key the model cannot express: report "unbound" rather than half a chord
    if ((bits & FL_SHIFT) != 0)
        chord.mods |= kModShift;
    if ((bits & FL_ALT) != 0)
        chord.mods |= kModAlt;
    // FL_CTRL and FL_META both fold onto the one command bit. FL_COMMAND is whichever of the two
    // this platform uses; a chord carrying the other one is nothing this app binds, and folding it
    // keeps the round trip closed instead of dropping a modifier on the floor.
    if ((bits & (FL_CTRL | FL_META)) != 0)
        chord.mods |= kModCtrl;
    return chord;
}

std::string chordDisplayText(KeyChord chord) {
    const int shortcut = toFlShortcut(chord);
    if (shortcut == 0)
        return {};
    const char* label = fl_shortcut_label(static_cast<unsigned int>(shortcut));
    return label != nullptr ? std::string(label) : std::string();
}

} // namespace mosaic::ui
