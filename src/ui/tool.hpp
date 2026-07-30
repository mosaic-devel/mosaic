#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The tool framework (PLAN S11): a registry of the editor's tools and the single "active tool"
// selection that the left toolbar drives and the tool options bar + Properties tab (S11-b) read.
// Tools here are identity + presentation (icon, name, shortcut) only; on-canvas behaviour and the
// per-tool option sets arrive with each tool's own later session (S14 marquee/lasso, S15 move,
// S16 crop, S19 brush, S21 bucket, S22 gradient, S23 eraser, S24 eyedropper, S26 shape, S29 type).
// This module is intentionally FLTK-free so it can be unit-tested headlessly.
namespace mosaic::ui {

enum class ToolId : std::uint8_t {
    Move,
    RectMarquee,
    EllipseMarquee,
    Lasso,
    PolygonLasso,
    MagicWand,
    SelectBrush,
    EdgeBrush, // L1: the edge-aware select brush (a SelectBrush-slot variant)
    Crop,
    MeshWarp,        // S35-b: a Catmull-Rom lattice over the layer's own pixels
    PerspectiveWarp, // S35-b: one 4-corner homography -- the warp slot's second variant
    Brush,
    Eraser,
    InpaintBrush,
    CloneStamp,   // S38: Ctrl-pick a source, then stamp it through the brush tip
    RedEye,       // S38-b: flash red-eye (Tier 1) -- the eye-tool slot's first variant
    RedEyeSclera, // S38-b: sclera de-redding / vein suppression (Tier 2) -- its second
    BucketFill,
    Gradient,
    Eyedropper,
    RectShape,
    EllipseShape,
    PolygonShape,
    StarShape,
    LineShape,
    Pen, // S28: Bezier authoring + node editing on a vec::Path VectorLayer
    Text,
    Zoom,
};

// Coarse grouping for the left toolbar: the column draws a subtle divider where the group changes
// between consecutive tools (S11-c). Kept deliberately few -- a handful of clusters, not one per tool.
enum class ToolGroup : std::uint8_t {
    SelectTransform, // Move, marquee, lasso, magic wand, select brush, crop, warp
    PaintFill,       // Brush, eraser, bucket, gradient
    Sample,          // Eyedropper
    VectorText,      // Shape, type
    View,            // Zoom / navigation
};

// A toolbar *slot*: the unit the column shows as a single button. Most slots hold one tool; a few hold
// a set of **variants** that share the slot and are switched via the slot's flyout (S11-e) -- the
// marquee slot (rectangular / elliptical), the lasso slot (free / polygonal), the shape slot
// (rectangle / ellipse / line). A slot's button shows whichever variant is current and carries a small
// corner triangle when it holds more than one. (ToolGroup, above, is the coarser divider clustering;
// a slot never straddles a group.)
enum class ToolSlot : std::uint8_t {
    Move,
    Marquee,
    Lasso,
    MagicWand,
    SelectBrush,
    Crop,
    Warp, // S35-b: one slot, two variants (mesh lattice / 4-corner perspective)
    Brush,
    Eraser,
    Inpaint,
    CloneStamp, // S38: its own slot -- the clone stamp is not a variant of anything
    RedEye, // S38-b: one slot, two variants (flash red-eye / sclera de-redding)
    Bucket,
    Gradient,
    Eyedropper,
    Shape,
    Pen,
    Text,
    Zoom,
};

// A tool option: one tweakable setting a tool publishes, rendered by the tool options bar (S11-b)
// and the Properties tab (S11-c) -- two surfaces over the *same* value, kept in lockstep. The sets
// in S11 are placeholder-representative; each tool's real options arrive with its own later session.
enum class ToolOptionKind : std::uint8_t {
    Slider, // a numeric value in [min,max] by `step`, shown with an optional unit suffix
    Choice, // one of `choices` (value = the selected index)
    Toggle, // a boolean (value = 0 or 1)
    Button, // a momentary action (no stored value); clicking fires ToolManager::onAction(id)
    Number, // an outlined free-entry numeric field (value = the number; min/max/step clamp it)
    Font,   // a font-family picker: a Choice (value = index into `choices`) whose open list shows
            // each family rendered in its own face -- the options bar wires the preview (S29-c)
};

// A hint for an option's accent styling (currently Buttons): Affirmative = a solid accent-filled
// primary (e.g. crop Apply); Destructive = a solid danger-red fill (reserved for genuinely destructive
// actions); None = a plain neutral button (e.g. crop Cancel). Keeps tool.hpp free of any UI/colour type.
enum class ToolAccent : std::uint8_t { None, Affirmative, Destructive };

// A tool glyph's source: SVG text (the default pack and most packs) OR encoded PNG bytes (raster
// icon packs, S52). Exactly one side is set; consumers turn either into pixels through
// ui::renderIconSource (icon_pack.hpp) so vector and raster art travel the same pipeline.
struct IconSource {
    std::string svg;               // SVG source text ("" when the icon is raster)
    std::vector<std::uint8_t> png; // encoded PNG bytes (empty when the icon is vector)
    [[nodiscard]] bool empty() const noexcept { return svg.empty() && png.empty(); }
};

// How a Slider option maps its track position to its value (consumed by the options-bar ScrubSlider).
// Gamma/Log give the low end of the range most of the track, so small values (a 2 px brush on a
// 1..1000 range) are reachable without the precision gesture. Kept here, FLTK-free, as part of the
// option model; the widget translates it to ui::ScrubCurve.
enum class ResponseCurve : std::uint8_t { Linear, Gamma, Log };

// A Toggle option may render as a STYLED glyph (bold "B", italic "I", underlined "U", struck "S")
// rather than a plain text label -- bare "B/I/U/S" read as ambiguous. None = a normal text toggle.
// FLTK-free (part of the model); the options bar maps it to a ui::GlyphButton::Kind. (S29-c redesign)
enum class ToolGlyph : std::uint8_t { None, Bold, Italic, Underline, Strike };

struct ToolOption {
    std::string id;                   // stable key within the tool
    std::string label;                // shown to the user
    ToolOptionKind kind = ToolOptionKind::Slider;
    double value = 0.0;               // Slider: the value; Choice: selected index; Toggle: 0/1
    double min = 0.0;                 // Slider
    double max = 1.0;                 // Slider
    double step = 1.0;                // Slider
    std::string suffix;               // Slider readout unit, e.g. "px" / "%"
    std::vector<std::string> choices; // Choice labels
    std::vector<int> disabledChoices; // Choice: indices greyed + unpickable in the open list
                                      // (e.g. the crop Fill combo's Inpaint entry while the
                                      // staged crop is rotated: ⚠ INVARIANT -- content-aware
                                      // fill and crop rotation are mutually exclusive, so the
                                      // wedges a rotation leaves are never inpainted. Solid
                                      // fills stay available. Deliberate, not a gap.)
    std::string tooltip;              // hover help on the options-bar control (empty = none)
    ToolAccent accent = ToolAccent::None; // Button fill style (affirmative/destructive/neutral)
    bool primary = true;              // shown in the options bar (the hot subset); the full set lives
                                      // here for the tool's own panel (the "More…" bridge, S19+)
    bool joinPrev = false;            // bind tightly to the PREVIOUS option as one visual group: no
                                      // group gap before it, and its `label` is drawn as a centred,
                                      // enlarged SEPARATOR between the two controls (e.g. the crop
                                      // custom ratio's "W : H"), not as a left caption (S16-l)
    bool inlineFlow = false;          // Button only: flow inline with the left-fill controls (right
                                      // after the preceding option) instead of being right-anchored
                                      // with the action buttons (e.g. the Type bar's "Style…" / "3D…"
                                      // sit beside Size, not pinned to the bar's right edge -- §8.2)
    bool enabled = true;              // false -> the control is built but deactivated (greyed). Used
                                      // to RESERVE a control whose feature isn't wired yet (the Type
                                      // bar's "3D…" button placeholder, S29-c rev 1)
    ToolGlyph glyph = ToolGlyph::None; // Toggle only: draw a styled B/I/U/S glyph instead of a text label
    // --- Slider extras (the options-bar ScrubSlider; all default to "behaves as before") -------
    ResponseCurve curve = ResponseCurve::Linear; // track-position -> value mapping
    double curveK = 2.0;                          // Gamma exponent (>1 favours the low end)
    double snapStep = 0.0;                        // Shift-snap grid (<=0 -> the widget derives one)
    double defaultValue = 0.0;                    // middle-click / Ctrl-click reset target
    bool hasDefault = false;                      // false -> reset gesture is a no-op
};

// A tool: identity + presentation + its option set. On-canvas behaviour hooks are added by later
// sessions; this base carries what the toolbar + options surfaces need now and is the extension
// point tools subclass as they gain behaviour, so the registry type does not change later.
class Tool {
public:
    Tool(ToolId id, std::string name, std::string shortcut, std::string iconSvg, ToolGroup group,
         ToolSlot slot, std::vector<ToolOption> options = {})
        : m_id(id), m_name(std::move(name)), m_shortcut(std::move(shortcut)),
          m_icon{.svg = std::move(iconSvg)}, m_group(group), m_slot(slot),
          m_options(std::move(options)) {}
    virtual ~Tool() = default;

    [[nodiscard]] ToolId id() const noexcept { return m_id; }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] const std::string& shortcut() const noexcept { return m_shortcut; } // e.g. "B"
    [[nodiscard]] const IconSource& icon() const noexcept { return m_icon; }
    // Restyle the tool's glyph (the icon-pack switch, S52). Consumers cache rendered pixels, so
    // the caller must also refresh them (LeftToolbar::reloadIcons; flyout rows re-render on open).
    void setIcon(IconSource icon) { m_icon = std::move(icon); }
    [[nodiscard]] ToolGroup group() const noexcept { return m_group; } // toolbar divider cluster
    [[nodiscard]] ToolSlot slot() const noexcept { return m_slot; }    // toolbar button slot

    // The tool's options. The mutable overload lets the options surfaces write edited values back
    // (the single source of truth), so a tool keeps its settings while it stays registered.
    [[nodiscard]] const std::vector<ToolOption>& options() const noexcept { return m_options; }
    [[nodiscard]] std::vector<ToolOption>& options() noexcept { return m_options; }

    // Toolbar-button tooltip: "Name (S)" (or just "Name" when the tool has no shortcut).
    [[nodiscard]] std::string tooltip() const;

private:
    ToolId m_id;
    std::string m_name;
    std::string m_shortcut;
    IconSource m_icon;
    ToolGroup m_group;
    ToolSlot m_slot;
    std::vector<ToolOption> m_options;
};

// Owns the built-in tool set and the active-tool selection, and notifies observers (the toolbar,
// and the S11-b options surfaces) when the active tool changes. FLTK-free → unit-tested.
class ToolManager {
public:
    ToolManager(); // registers the built-in tools; the Move tool starts active

    [[nodiscard]] const std::vector<std::unique_ptr<Tool>>& tools() const noexcept {
        return m_tools;
    }
    [[nodiscard]] ToolId active() const noexcept { return m_active; }
    [[nodiscard]] Tool* activeTool() const { return find(m_active); }
    // The tool active immediately before the current one -- for "restore previous" flows like S16-p's
    // post-crop tool switch. Equal to the initial tool until the first change; updated on every actual
    // change (re-selecting the active tool is a no-op, so it never records the same tool as previous).
    [[nodiscard]] ToolId previous() const noexcept { return m_previous; }
    [[nodiscard]] Tool* find(ToolId id) const;

    // ---- Toolbar slots / variants (S11-e) ----
    // The distinct slots in toolbar order (first-seen order of the registered tools).
    [[nodiscard]] const std::vector<ToolSlot>& slots() const noexcept { return m_slots; }
    // The tools registered in `slot`, in toolbar order (one entry for a single-tool slot).
    [[nodiscard]] std::vector<Tool*> toolsInSlot(ToolSlot slot) const;
    // Which slot a tool belongs to (the active slot's, for a missing id).
    [[nodiscard]] ToolSlot slotOf(ToolId id) const;
    // The tool a slot's button currently represents -- its last-active variant (the slot's first tool
    // until one is picked). setActive() updates it, so a slot "remembers" which variant was chosen.
    [[nodiscard]] ToolId shownToolForSlot(ToolSlot slot) const;

    // Make `id` the active tool, firing onChange. No-op (no notification) if it is already active or
    // not a registered tool. Also records `id` as its slot's shown variant.
    void setActive(ToolId id);

    // Restyle every tool's glyph from `iconFor` (the icon-pack switch, S52): an empty return keeps
    // the tool's current art. Fires no observer -- the caller owns refreshing the icon consumers.
    void applyIcons(const std::function<IconSource(ToolId)>& iconFor);

    // Observer fired after the active tool *changes* (rebuild the toolbar + the options surfaces).
    void setOnChange(std::function<void()> cb) { m_onChange = std::move(cb); }

    // Observer fired when an option *value* is edited on one surface, so the twin surface (and, in
    // later sessions, the canvas) can re-sync. Surfaces call notifyOptionsChanged() after writing.
    void setOnOptionsChanged(std::function<void()> cb) { m_onOptionsChanged = std::move(cb); }
    void notifyOptionsChanged() {
        syncEraserSizeTie(); // before observers read: the mirrored twin must already be current
        if (m_onOptionsChanged)
            m_onOptionsChanged();
    }

    // The eraser-size tie (S19 §8.4, Settings::eraserSizeFollowsBrush, default true): while on,
    // the Brush's and the Eraser's "size" options are ONE value -- an edit to whichever of the two
    // is active mirrors to the other, and enabling (or constructing) seeds the eraser from the
    // brush. It lives in the manager rather than in an options surface so every edit path is
    // covered and the bar can never show a size a stroke won't use.
    void setEraserSizeTie(bool on);

    // Brush smoothing (core/brush/stroke_smoother.hpp): ON or OFF, no dial. It steadies the POINTER,
    // not the tip, so it is one preference wearing four hats -- the Brush, the Eraser, the Inpaint
    // brush and the Clone stamp each show it on their bar, and they must not drift apart.
    // setBrushSmoothingEnabled() seeds all four from the persisted setting; syncBrushSmoothing()
    // copies whichever bar the user just clicked onto the others and reports the state now in force
    // (-1 = the active tool has no such option, i.e. it is not a brush-family tool; 0 = off; 1 = on).
    void setBrushSmoothingEnabled(bool on);
    int syncBrushSmoothing();
    [[nodiscard]] bool eraserSizeTie() const noexcept { return m_eraserSizeTie; }

    // Observer fired when a momentary Button option is clicked (the options bar calls it with the
    // option's id, e.g. "apply" / "cancel"); the host maps it to a tool action.
    void setOnAction(std::function<void(const std::string&)> cb) { m_onAction = std::move(cb); }
    void notifyAction(const std::string& optionId) {
        if (m_onAction)
            m_onAction(optionId);
    }

private:
    // Mirror the shared size across the Brush/Eraser pair while the tie is on. The options
    // surfaces edit the ACTIVE tool, so the active side is the fresh value; when neither of the
    // pair is active there is nothing new to mirror (enable-time seeding covers edits made while
    // the tie was off).
    void syncEraserSizeTie();
    void seedEraserSize(); // eraser.size = brush.size (the brush is the source at tie time)

    std::vector<std::unique_ptr<Tool>> m_tools;
    std::vector<ToolSlot> m_slots;             // distinct slots, toolbar order
    std::map<ToolSlot, ToolId> m_shownPerSlot; // current shown variant per slot
    bool m_eraserSizeTie = true; // matches Settings::eraserSizeFollowsBrush's default
    ToolId m_active = ToolId::Move;
    ToolId m_previous = ToolId::Move; // the tool active before m_active (S16-p restore-previous)
    std::function<void()> m_onChange;
    std::function<void()> m_onOptionsChanged;
    std::function<void(const std::string&)> m_onAction;
};

// Map a single character (case-insensitive) to the tool it selects, for the no-modifier keyboard
// shortcuts. Pure → unit-tested; the FLTK key plumbing lives in the main window.
[[nodiscard]] std::optional<ToolId> toolForShortcut(char key);

} // namespace mosaic::ui
