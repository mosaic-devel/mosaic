#include "ui/tool.hpp"

#include "ui/icon_pack.hpp"   // defaultIconSvg: tools are BORN with the default pack's art (S52)
#include "ui/warp_gesture.hpp" // kWarpMinNodes / kWarpMaxNodes: the bar clamps to what the gesture does

#include "common/i18n.hpp"

#include <array>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace mosaic::ui {
namespace {

struct ToolDef {
    ToolId id;
    const char* name;     // wrapped in _() at registration
    const char* shortcut; // single uppercase letter (display + hotkey), case-insensitive match
    ToolGroup group; // toolbar cluster; the column draws a divider where this changes (S11-c)
    ToolSlot slot;   // toolbar button slot; consecutive same-slot tools are flyout variants (S11-e)
};

// Order here is the toolbar order (top to bottom). Consecutive tools sharing a ToolSlot are variants
// of one slot (marquee rect/ellipse, lasso free/poly, shape rect/ellipse/line) and collapse to a
// single button with a flyout; the ToolGroup column drives the subtle dividers between clusters.
// Variants share their slot's shortcut letter (it selects the slot's current variant).
constexpr std::array<ToolDef, 28> kToolDefs{{
    {ToolId::Move, "Move", "V", ToolGroup::SelectTransform, ToolSlot::Move},
    {ToolId::RectMarquee, "Rectangular Marquee", "M", ToolGroup::SelectTransform,
     ToolSlot::Marquee},
    {ToolId::EllipseMarquee, "Elliptical Marquee", "M",
     ToolGroup::SelectTransform, ToolSlot::Marquee},
    {ToolId::Lasso, "Lasso", "L", ToolGroup::SelectTransform, ToolSlot::Lasso},
    {ToolId::PolygonLasso, "Polygonal Lasso", "L", ToolGroup::SelectTransform,
     ToolSlot::Lasso},
    {ToolId::MagicWand, "Magic Wand", "W", ToolGroup::SelectTransform, ToolSlot::MagicWand},
    {ToolId::SelectBrush, "Select Brush", "A", ToolGroup::SelectTransform, ToolSlot::SelectBrush},
    {ToolId::EdgeBrush, "Edge Select Brush", "A", ToolGroup::SelectTransform,
     ToolSlot::SelectBrush},
    {ToolId::Crop, "Crop", "C", ToolGroup::SelectTransform, ToolSlot::Crop},
    // S35-b: the two warps, one slot, right after Crop -- they are geometry tools, not paint, and
    // Crop is the geometry cluster's tail. "Q" for the shortcut, and it is genuinely free: W is the
    // Magic Wand's, A the Select Brush's, R is the canvas's own rotate gesture and P the Pen's, so
    // the word "warp" offers no letter of its own. H / O are deliberately LEFT free for the tools
    // whose names do start with them and whose art the icon pack already reserves (hand, dodge/burn).
    {ToolId::MeshWarp, "Mesh Warp", "Q", ToolGroup::SelectTransform, ToolSlot::Warp},
    {ToolId::PerspectiveWarp, "Perspective Warp", "Q", ToolGroup::SelectTransform, ToolSlot::Warp},
    {ToolId::Brush, "Brush", "B", ToolGroup::PaintFill, ToolSlot::Brush},
    {ToolId::Eraser, "Eraser", "E", ToolGroup::PaintFill, ToolSlot::Eraser},
    {ToolId::InpaintBrush, "Inpaint Brush", "J", ToolGroup::PaintFill,
     ToolSlot::Inpaint},
    // S38: the Stamp / Clone tool. "S" for Stamp, and it is genuinely free -- no other descriptor
    // claims it, and the canvas's own bare-key gestures are R (rotate) and Space (pan), never S.
    {ToolId::CloneStamp, "Clone Stamp", "S", ToolGroup::PaintFill, ToolSlot::CloneStamp},
    // S38-b: ONE eye tool, one slot, two shipping modes (docs/red-eye-tool.md §4). "Y" for eYe --
    // the note proposed "R", but bare R is the canvas's own rotate gesture (VulkanCanvas::onKeyDown
    // consumes it while the canvas has focus), so R was never actually free. The code wins.
    {ToolId::RedEye, "Red Eye", "Y", ToolGroup::PaintFill, ToolSlot::RedEye},
    {ToolId::RedEyeSclera, "De-redden Eye", "Y", ToolGroup::PaintFill, ToolSlot::RedEye},
    {ToolId::BucketFill, "Bucket Fill", "K", ToolGroup::PaintFill, ToolSlot::Bucket},
    {ToolId::Gradient, "Gradient", "G", ToolGroup::PaintFill, ToolSlot::Gradient},
    {ToolId::Eyedropper, "Eyedropper", "I", ToolGroup::Sample, ToolSlot::Eyedropper},
    {ToolId::RectShape, "Rectangle", "U", ToolGroup::VectorText, ToolSlot::Shape},
    {ToolId::EllipseShape, "Ellipse", "U", ToolGroup::VectorText, ToolSlot::Shape},
    {ToolId::PolygonShape, "Polygon", "U", ToolGroup::VectorText, ToolSlot::Shape},
    {ToolId::StarShape, "Star", "U", ToolGroup::VectorText, ToolSlot::Shape},
    {ToolId::LineShape, "Line", "U", ToolGroup::VectorText, ToolSlot::Shape},
    {ToolId::Pen, "Pen", "P", ToolGroup::VectorText, ToolSlot::Pen},
    {ToolId::Text, "Type", "T", ToolGroup::VectorText, ToolSlot::Text},
    {ToolId::Zoom, "Zoom", "Z", ToolGroup::View, ToolSlot::Zoom},
}};

// Small builders for the placeholder option sets below. The trailing `tip` is the options-bar
// hover help (empty = none).
ToolOption slider(std::string id, std::string label, double min, double max, double step,
                  double value, std::string suffix = "", std::string tip = "") {
    ToolOption o{std::move(id),  std::move(label), ToolOptionKind::Slider, value, min,
                 max,            step,             std::move(suffix),      {}};
    o.tooltip = std::move(tip);
    return o;
}
ToolOption choice(std::string id, std::string label, std::vector<std::string> choices, int sel = 0,
                  std::string tip = "") {
    ToolOption o{std::move(id), std::move(label), ToolOptionKind::Choice, static_cast<double>(sel),
                 0, 0, 1, "", std::move(choices)};
    o.tooltip = std::move(tip);
    return o;
}
ToolOption toggle(std::string id, std::string label, bool on, std::string tip = "") {
    ToolOption o{std::move(id), std::move(label), ToolOptionKind::Toggle, on ? 1.0 : 0.0,
                 0, 0, 1, "", {}};
    o.tooltip = std::move(tip);
    return o;
}
// A Toggle drawn as a styled glyph (bold "B", italic "I", underlined "U", struck "S") with no text
// label -- the Type bar's B/I/U/S. The options bar renders `glyph` as a compact square ui::GlyphButton.
// `joinPrev` binds it flush to the previous control (no group gap) so B/I/U/S read as one segmented set.
ToolOption styleToggle(std::string id, std::string tip, ToolGlyph glyph, bool joinPrev = false) {
    ToolOption o = toggle(std::move(id), "", false, std::move(tip));
    o.glyph = glyph;
    o.joinPrev = joinPrev;
    return o;
}
ToolOption button(std::string id, std::string label, ToolAccent accent, std::string tip = "") {
    ToolOption o{std::move(id), std::move(label), ToolOptionKind::Button, 0, 0, 0, 1, "", {}};
    o.tooltip = std::move(tip);
    o.accent = accent;
    return o;
}
ToolOption number(std::string id, std::string label, double min, double max, double step,
                  double value, std::string tip = "") {
    ToolOption o{std::move(id), std::move(label), ToolOptionKind::Number, value, min,
                 max,           step,             "",                     {}};
    o.tooltip = std::move(tip);
    return o;
}
// A floating-point brush-tip-diameter slider for the options-bar ScrubSlider: a Gamma response so
// the small end of the 0.5..1000 px range gets most of the track, 0.1 px steps for sub-pixel sizes,
// Shift-snaps to 0.5, and a middle-click / Ctrl-click resets to `value`. The tip tools share it.
ToolOption sizeSlider(std::string id, std::string label, double value, std::string tip) {
    ToolOption o =
        slider(std::move(id), std::move(label), 0.5, 1000, 0.1, value, _("px"), std::move(tip));
    o.curve = ResponseCurve::Gamma;
    o.curveK = 2.0;
    o.snapStep = 0.5;
    o.defaultValue = value;
    o.hasDefault = true;
    return o;
}
// Demote an option out of the (curated) options bar: it stays in the tool's full set -- reached via
// the tool's own panel / the "More…" bridge (S19+) -- but the bar shows only the `primary` hot subset.
ToolOption secondary(ToolOption o) {
    o.primary = false;
    return o;
}
// Bind an option tightly to the one before it (its label becomes the centred separator between the
// two -- the crop custom ratio's "W : H"). See ToolOption::joinPrev.
ToolOption join(ToolOption o) {
    o.joinPrev = true;
    return o;
}
// The shape tools' resize-vs-transform toggle (S26-b §7.1): a UI-only mode (NOT part of the shape's
// data, so select-to-edit never reflects it) that picks what a selection-box handle does. OFF (the
// default) resizes the shape's size parameters with the stroke width held fixed (the crisp
// Figma/Affinity default); ON scales the whole layer so the stroke scales with it (Illustrator's
// "Scale Strokes & Effects"). Since S26-c "the stroke" means a line's own weight and the layer's
// outline EFFECT -- both ride the layer transform, so the toggle reads the same as it always did.
// The id stays "transform"; only the user-facing label names the effect.
ToolOption shapeTransformToggle() {
    return toggle("transform", _("Scale stroke"), false,
                  _("Scale the stroke with the shape when dragging handles; off keeps the stroke "
                    "width fixed and resizes the shape"));
}
// The "Edit shape..." button that opens the shape-designer popover (S26-b §7.4) -- the "everything"
// surface (per-corner radii, arc sweep, star/polygon rounding, line dash). A neutral action button,
// right-anchored on the bar like crop's Apply; the host (notifyAction "designer") opens the popover.
ToolOption shapeDesignerButton() {
    return button("designer", _("Edit shape…"), ToolAccent::None,
                  _("Open the shape designer for the selected shape's full parameters"));
}
// The Type bar's "Style…" button (S29-c §8): opens the Type panel (Character + Paragraph). Unlike the
// shape designer it flows INLINE, right after the Size slider, rather than right-anchored -- the type
// controls read as one left-aligned cluster (Font · Size · Style… · 3D…).
ToolOption typePanelButton() {
    auto o = button("typePanel", _("Style…"), ToolAccent::None,
                    _("Open the Type panel: Character + Paragraph controls"));
    o.inlineFlow = true;
    return o;
}
// The Gradient bar's "Stops…" button (S22): opens the reusable GradientFlyout (stops + spread +
// blend curves) anchored to it. Flows inline after the Type dropdown, like the Type "Style…" button.
ToolOption gradientStopsButton() {
    auto o = button("stops", _("Stops…"), ToolAccent::None,
                    _("Edit the gradient's colour stops, blend curves, and spread"));
    o.inlineFlow = true;
    return o;
}
// The Type bar's "3D…" button: opens the 3D-text popup (S30-d; docs/type-tool.md §8.4) -- the live
// viewport with orbit/depth/bevel/light handles. Live now that the extrude engine landed (S30-c).
ToolOption type3dButton() {
    auto o = button("type3d", _("3D…"), ToolAccent::None, _("3D extrusion (depth, bevel, lighting)"));
    o.inlineFlow = true;
    return o;
}

// The *placeholder* option set a tool publishes for S11 (representative of each tool's eventual
// real options; the on-canvas behaviour that consumes them lands in each tool's own session).
std::vector<ToolOption> defaultOptionsFor(ToolId id) {
    switch (id) {
    case ToolId::Move:
        // "Anti-aliasing" governs how rotated / scaled / sub-pixel-placed layers are resampled by
        // the compositor (render::ResampleFilter; the order here matches app_window's index map).
        // Auto picks the best kernel per transform: Nearest for lossless (incl. pixel-doubling),
        // cheap Bilinear during a live drag, sharp Lanczos3 on a committed rotate/enlarge, and
        // box Area on a committed reduction. Whole-pixel translation stays lossless regardless.
        return {toggle("autoselect", _("Auto-Select"), true,
                       _("Click selects the topmost layer under the cursor")),
                choice("snap", _("Snap"), {_("None"), _("Edges"), _("Grid")}, 0,
                       _("Snap moved layers to edges or the grid")),
                choice("aa", _("Anti-aliasing"),
                       {_("Auto"), _("Nearest"), _("Bilinear"), _("Bicubic"), _("Mitchell"),
                        _("Lanczos 2"), _("Lanczos 3"), _("Area (box)"), _("Gaussian"),
                        _("Supersample")},
                       0, _("How rotated / scaled layers are resampled (Auto picks per transform)"))};
    case ToolId::RectMarquee:
    case ToolId::EllipseMarquee:
        return {choice("mode", _("Mode"), {_("New"), _("Add"), _("Subtract"), _("Intersect")}, 0,
                       _("How a new selection combines with the current one")),
                slider("feather", _("Feather"), 0, 250, 1, 0, _("px"),
                       _("Soften the selection edge by this radius"))};
    case ToolId::Lasso:
    case ToolId::PolygonLasso:
        return {slider("feather", _("Feather"), 0, 250, 1, 0, _("px"),
                       _("Soften the selection edge by this radius")),
                toggle("aa", _("Anti-alias"), true, _("Smooth the selection edge"))};
    case ToolId::MagicWand:
        // S17 (docs/research-selection.md §8): a stateless colour-tolerance flood. Tolerance is a
        // 0-100 threshold on the wand's metric (linear in T; the app maps it to WandParams [0,1]).
        // "Contiguous" floods the connected region vs. matches every pixel; "Source" offers the
        // Eyedropper's Active-Layer / All-Layers choice, with the same labels and the same meaning
        // (both resolve through MainWindow::activeLayerDocImage / wandMergedSource) but NOT its
        // default: the wand stays on Active Layer. A flood over merged pixels selects a region the
        // active layer does not contain, so choosing it has to be the user's act, not the tool's.
        return {slider("tolerance", _("Tolerance"), 0, 100, 1, 15, "",
                       _("How close in color a pixel must be to the clicked one to be selected")),
                toggle("contiguous", _("Contiguous"), true,
                       _("Select only the connected region, not every matching pixel")),
                toggle("antialias", _("Anti-alias"), true, _("Smooth the selection edge")),
                choice("source", _("Source"), {_("Active Layer"), _("All Layers")}, 0,
                       _("Sample from the active layer or the merged composite"))};
    case ToolId::SelectBrush:
        // S18 (docs/research-select-brush.md §3.1): a plain coverage painter -- painting the tip's
        // soft coverage straight into the selection mask, no image analysis. Size + Opacity are the
        // hot pair on the bar (mirroring the Brush); Hardness + Flow live in the model. The combine
        // op is NOT a bar control: no-modifier uses Settings::selectBrushAddByDefault (Add by
        // default) and Alt subtracts (§9-B), the press-time-modifier convention the marquee uses.
        return {sizeSlider("size", _("Size"), 30, _("Select brush tip diameter")),
                secondary(slider("hardness", _("Hardness"), 0, 100, 1, 80, _("%"),
                                 _("Edge softness of the selection tip"))),
                secondary(slider("flow", _("Flow"), 0, 100, 1, 100, _("%"),
                                 _("Selection coverage added per stroke pass"))),
                slider("opacity", _("Opacity"), 0, 100, 1, 100, _("%"),
                       _("Maximum coverage the stroke adds to the selection"))};
    case ToolId::EdgeBrush: {
        // L1: the edge-aware select brush (the S18 select brush's sibling). The stroke
        // supplies geometric seeds; on release the selection grows outward by an edge-weighted
        // geodesic distance transform and stops at image edges (core::edgeGrowSelection). The
        // knobs are deliberately two -- "how far" (Reach) and "how hard edges stop it" (Edge
        // Stop); the combine op is the select brush's (setting + Alt, §9-B), never a bar control.
        // Source mirrors the wand's Active-Layer / All-Layers choice exactly.
        ToolOption reach =
            slider("reach", _("Reach"), 1, 500, 1, 64, _("px"),
                   _("How far the selection grows out from the stroke before stopping"));
        reach.curve = ResponseCurve::Gamma; // small reaches get most of the track, like Size
        reach.curveK = 2.0;
        reach.defaultValue = 64;
        reach.hasDefault = true;
        return {sizeSlider("size", _("Size"), 30, _("Seed stroke tip diameter")),
                std::move(reach),
                slider("edgeStop", _("Edge Stop"), 0, 100, 1, 50, "",
                       _("How strongly image edges stop the growing selection (0 grows a plain "
                         "disc)")),
                choice("source", _("Source"), {_("Active Layer"), _("All Layers")}, 0,
                       _("Read edges from the active layer or the merged composite"))};
    }
    case ToolId::Crop: {
        // Real options (S16): the index order is cropRatioForOptions' contract (Custom = the last
        // index, kCropRatioCustom; its ratio comes from the ratioW/ratioH fields, not the preset
        // table); "Swap W:H" flips an orientation; "Delete Cropped Pixels" picks the destructive
        // bake over the canvas-bounds-only crop (render::buildCropCommand). The two Custom Number
        // fields start non-primary (hidden from the bar); the host flips them primary and triggers
        // a deferred rebuild when "Custom" is chosen (S16-e -- never a synchronous rebuild inside a
        // control's own callback, which would delete the live widget mid-edit).
        // Smart Recompose (plan §1.3): the second tier's button. Hidden unless Smart Resize is ON
        // (the host flips `primary`, same deferred-rebuild trick as the Custom fields) and greyed
        // until the marked regions cannot all fit a crop window at the chosen ratio (the canvas
        // recomputes the offer per frame). The USER invokes it — never an automatic switch; that
        // is a recorded guardrail on this tier, not a default anyone may flip.
        ToolOption recomposeBtn =
            button("recompose", _("Recompose"), ToolAccent::None,
                   _("Rebuild the picture instead of cropping: the marked regions move closer "
                     "together, the background is retargeted and the gaps are healed. Available "
                     "when the marked regions cannot all fit a crop at this ratio"));
        recomposeBtn.primary = false;
        recomposeBtn.enabled = false;
        return {choice("ratio", _("Ratio"),
                       {_("Free"), _("Original"), "1:1", "4:3", "16:9", "3:2", _("Custom")}, 0,
                       _("Constrain the crop to an aspect ratio")),
                secondary(number("ratioW", "", 0.01, 100000.0, 0.01, 4.0,
                                 _("Custom aspect ratio width (W:H)"))),
                join(secondary(number("ratioH", ":", 0.01, 100000.0, 0.01, 5.0,
                                      _("Custom aspect ratio height (W:H)")))),
                toggle("swap", _("Swap orientation"), false,
                       _("Swap the crop's width and height (portrait / landscape)")),
                // S16-f Smart Resize: content-aware placement of the staged rect. It only picks
                // the STARTING rect (one suggestion, always hand-editable afterwards); Apply is
                // the same crop command as a manual rect.
                toggle("smartResize", _("Smart Resize"), false,
                       _("Place the crop automatically to keep the most important parts of the "
                         "picture: with an aspect ratio it repositions the frame, with Free it "
                         "trims away the boring edges. Click a detected region to protect or "
                         "ignore it, Ctrl-drag to mark one yourself — then adjust the crop as "
                         "you like")),
                toggle("grid", _("Guides"), true,
                       _("Show rule-of-thirds guides over the crop")),
                toggle("delete", _("Delete Pixels"), true,
                       _("On: discard the pixels outside the crop. Off: keep them (resize the "
                         "canvas only)")),
                // S16-f expansion: the crop box may be dragged PAST the canvas edge; the added
                // area takes this fill. SECONDARY: the host flips it into the bar exactly while
                // the staged box expands or is rotated (when a fill is meaningful) — the bar was
                // "a mess" with it always present (user 2026-07-02). Inpaint = the S37 engine
                // heals the ring, run ONLY on an explicit Apply of this pre-chosen mode — never
                // a post-crop chooser presenting operation previews, and never triggered by a
                // rotation. Those two are recorded guardrails on the crop tool, not preferences.
                secondary(choice("fillMode", _("Fill"),
                                 {_("Transparent"), _("White"), _("Black"), _("Active color"),
                                  _("Background color"), _("Inpaint")},
                                 0,
                                 _("What fills the new area when the crop box reaches outside "
                                   "the canvas (drag a handle past the edge to expand). "
                                   "Transparent adds nothing; Inpaint continues the picture "
                                   "into the new area"))),
                recomposeBtn,
                button("apply", _("Apply"), ToolAccent::Affirmative, _("Apply the crop (Enter)")),
                // Cancel is NOT destructive (it just discards the staged crop) -> a quiet neutral
                // button. Red is reserved for genuinely destructive actions (user 2026-06-14).
                button("cancel", _("Cancel"), ToolAccent::None,
                       _("Reset the crop to the full canvas (Esc)"))};
    }
    case ToolId::MeshWarp:
    case ToolId::PerspectiveWarp: {
        // S35-b (docs/warp-tools.md §4). The Crop tool's precedent verbatim for the actions: an
        // affirmative Apply and a quiet neutral Cancel, right-anchored on the bar. Everything else is
        // the lattice's own shape plus how the pixels are resampled when it is applied.
        //
        // Rows / Columns are MEANINGLESS for Perspective -- one homography has four corners and no
        // interior control points -- so they are built and DEACTIVATED there rather than shown as
        // controls that lie about what they do. (The Type bar's reserved "3D…" is the precedent for
        // `enabled = false` on a control that exists but does nothing here.)
        const bool mesh = id == ToolId::MeshWarp;
        ToolOption rows = number("rows", _("Rows"), kWarpMinNodes, kWarpMaxNodes, 1, 4,
                                 _("Control points down the lattice"));
        ToolOption cols = number("cols", _("Columns"), kWarpMinNodes, kWarpMaxNodes, 1, 4,
                                 _("Control points across the lattice"));
        rows.enabled = mesh;
        cols.enabled = mesh;
        return {std::move(rows), std::move(cols),
                // The SAME kernel list, in the SAME order, as the Move tool's "Anti-aliasing"
                // (ui::warpQualityForChoice's table is the one place the two are tied together): a
                // user who has learnt the order once has learnt it everywhere.
                choice("quality", _("Quality"),
                       {_("Auto"), _("Nearest"), _("Bilinear"), _("Bicubic"), _("Mitchell"),
                        _("Lanczos 2"), _("Lanczos 3"), _("Area (box)"), _("Gaussian"),
                        _("Supersample")},
                       0,
                       _("How the pixels are resampled when the warp is applied (Auto picks per "
                         "deformation). A live drag always previews cheaply")),
                toggle("grid", _("Show grid"), true,
                       _("Draw the warp lattice over the layer; the handles stay grabbable either "
                         "way")),
                button("apply", _("Apply"), ToolAccent::Affirmative, _("Apply the warp (Enter)")),
                // Cancel discards the staged deformation -- not destructive, so a quiet neutral
                // button. Red is reserved for genuinely destructive actions (user 2026-06-14).
                button("cancel", _("Cancel"), ToolAccent::None,
                       _("Put the handles back where the layer's warp left them (Esc)"))};
    }
    case ToolId::Brush:
        // Bar shows the hot pair (Size + Opacity); Hardness/Flow live in the model for the S19
        // Brush Settings panel (reached via "More…") -- the bar is no longer a four-slider dump.
        //
        // The PRESET is not here. It was, briefly, as a stand-in Fl_Choice of 117 names -- which was
        // never the design (§8.1's chip) and, worse, was unusable: a hundred names in a pull-down
        // cannot be hunted through by eye, and a name is not what a brush looks like. Presets now
        // live in the right dock, in a filterable grid of their own thumbnails (§8.2,
        // ui::BrushPresetPanel), which is where the spec always put them.
        return {sizeSlider("size", _("Size"), 24, _("Brush tip diameter")),
                secondary(slider("hardness", _("Hardness"), 0, 100, 1, 80, _("%"),
                                 _("Edge softness of the brush tip"))),
                secondary(slider("flow", _("Flow"), 0, 100, 1, 100, _("%"),
                                 _("Paint build-up rate per stroke pass"))),
                slider("opacity", _("Opacity"), 0, 100, 1, 100, _("%"),
                       _("Maximum stroke opacity")),
                toggle("smoothing", _("Smoothing"), true,
                       _("Steadies the stroke by averaging recent pointer positions. A mouse reports "
                         "whole pixels, so it rattles without this. Off paints your input raw."))};
    case ToolId::Eraser:
        return {sizeSlider("size", _("Size"), 40, _("Eraser tip diameter")),
                secondary(slider("hardness", _("Hardness"), 0, 100, 1, 50, _("%"),
                                 _("Edge softness of the eraser tip"))),
                slider("opacity", _("Opacity"), 0, 100, 1, 100, _("%"),
                       _("How much each pass erases")),
                toggle("smoothing", _("Smoothing"), true,
                       _("Steadies the stroke by averaging recent pointer positions. A mouse reports "
                         "whole pixels, so it rattles without this. Off paints your input raw."))};
    case ToolId::InpaintBrush:
        // The Inpaint brush paints a mask (shown as a red overlay), not colour, so it carries only the
        // tip controls (S39); on release the brushed region is filled by the inpainting engine.
        return {sizeSlider("size", _("Size"), 40, _("Inpaint tip diameter")),
                secondary(slider("hardness", _("Hardness"), 0, 100, 1, 90, _("%"),
                                 _("Edge softness of the inpaint tip"))),
                toggle("smoothing", _("Smoothing"), true,
                       _("Steadies the stroke by averaging recent pointer positions. A mouse reports "
                         "whole pixels, so it rattles without this. Off paints your input raw."))};
    case ToolId::CloneStamp:
        // S38 (docs/clone-stamp.md §3/§4). A CLONE STAMP COPIES PIXELS -- there is no "match the
        // destination" control here and there never will be, so the bar is the tip's own controls
        // plus the two things that are genuinely about cloning: where the source is measured from
        // (Aligned) and which pixels count as the source (Sample).
        //
        // Size and Opacity are the hot pair, exactly as on the Brush; Hardness, Flow and Spacing sit
        // in the model for the tool's own panel. Aligned defaults ON, which is what makes painting a
        // subject out over several strokes keep it in one piece.
        return {sizeSlider("size", _("Size"), 40, _("Clone stamp tip diameter")),
                secondary(slider("hardness", _("Hardness"), 0, 100, 1, 80, _("%"),
                                 _("Edge softness of the clone tip"))),
                secondary(slider("flow", _("Flow"), 0, 100, 1, 100, _("%"),
                                 _("How much of the source each pass lays down"))),
                slider("opacity", _("Opacity"), 0, 100, 1, 100, _("%"),
                       _("Maximum strength of the stamped source")),
                toggle("aligned", _("Aligned"), true,
                       _("Keep the distance between the source and where you paint, so every "
                         "stroke carries on from the last. Off restarts each stroke at the same "
                         "source point")),
                // Title case, and "All Layers" reuses the wand's / eyedropper's own string: the
                // three Source-style pickers in the app must not read as three different features.
                // ⚠ "and", never "&": these labels go through Fl_Menu_::add, which reads `&` as a
                // mnemonic marker and `/` as a submenu separator (the standing Fl_Choice-parsing
                // trap). Escaping it as `&&` would work and would also put a doubled ampersand in
                // front of 74 translators for no gain.
                choice("sample", _("Sample"),
                       {_("Current Layer"), _("Current and Below"), _("All Layers")}, 0,
                       _("Which pixels get copied: the active layer alone, everything from the "
                         "active layer down, or the whole picture as you see it")),
                secondary(slider("spacing", _("Spacing"), 1, 200, 1, 10, _("%"),
                                 _("Distance between stamped dabs, as a percentage of the tip"))),
                toggle("smoothing", _("Smoothing"), true,
                       _("Steadies the stroke by averaging recent pointer positions. A mouse reports "
                         "whole pixels, so it rattles without this. Off paints your input raw."))};
    case ToolId::RedEye:
        // S38-b Tier 1 (docs/red-eye-tool.md §3.1/§4): kill the flash glow inside the pupil the
        // user pointed at. Size IS the scoped region -- the reticle ring shows exactly what a click
        // will correct, and nothing outside it is ever read as a candidate (no image
        // scan, no detector). Strength scales the whole masked transform; Darken says how far the
        // corrected pupil goes toward the dark-pupil target; "Keep catchlight" withholds that
        // darkening over the specular highlight so the eye keeps its spark (the chroma still
        // collapses there, so a red-tinted glare comes out white).
        return {sizeSlider("size", _("Size"), 40, _("Diameter of the region a click corrects")),
                slider("strength", _("Strength"), 0, 100, 1, 100, _("%"),
                       _("How strongly the red glow is removed")),
                slider("darken", _("Darken"), 0, 100, 1, 65, _("%"),
                       _("How far the corrected pupil goes toward a dark pupil")),
                toggle("catchlight", _("Keep catchlight"), true,
                       _("Leave the bright reflection in the eye alone, so the eye keeps its "
                         "spark"))};
    case ToolId::RedEyeSclera:
        // S38-b Tier 2 (docs/red-eye-tool.md §3.2/§4): paint over the bloodshot white of the eye.
        // "Keep veins" is the vascularity floor and it is a FIRST-CLASS control, not a nicety: the
        // amount can never drive the region past it, so the tool cannot produce a flat, dead-white
        // eye however hard it is pushed. Method picks harmonize-only vs harmonize + the
        // frequency-separation vein suppression. Corner warmth damps the effect on the warm canthus
        // tint (and on any eyelid a slipped brush caught).
        return {sizeSlider("size", _("Size"), 60, _("Eye retouch tip diameter")),
                slider("amount", _("Amount"), 0, 100, 1, 55, _("%"),
                       _("How far the redness is pulled toward the surrounding white")),
                slider("vascularity", _("Keep veins"), 0, 100, 1, 35, _("%"),
                       _("The share of the eye's own colour and vein detail that always survives. "
                         "The default under-corrects on purpose — a fully whitened eye looks "
                         "fake")),
                choice("method", _("Method"), {_("Harmonize"), _("Harmonize + veins")}, 1,
                       _("Harmonize evens out the redness; + veins also softens the vessels")),
                toggle("warmth", _("Corner warmth"), true,
                       _("Leave the warm tint in the corners of the eye alone")),
                secondary(slider("spread", _("Spread"), 0, 100, 1, 40, _("%"),
                                 _("Softness of the painted region's edge")))};
    case ToolId::BucketFill:
        return {slider("tolerance", _("Tolerance"), 0, 255, 1, 32, "",
                       _("How close in color a pixel must be to get filled")),
                toggle("contiguous", _("Contiguous"), true,
                       _("Fill only the connected region, not all matching pixels")),
                toggle("antialias", _("Anti-alias"), true,
                       _("Soften the filled region's edge with a one-pixel feather")),
                secondary(slider("opacity", _("Opacity"), 0, 100, 1, 100, _("%"),
                                 _("Fill opacity")))};
    case ToolId::Gradient:
        // Drag on the canvas to lay the gradient down; the Type picks the shape (Elliptical = a
        // radial with a squashed axis), "Stops…" opens the reusable flyout (stops/blend curves/
        // spread), Dithering trades a little texture for the loss of 8-bit banding, Opacity is the
        // created layer's opacity. The gradient lands as an editable, maskable vector layer --
        // re-select the tool on it to re-drag the handles. (S22)
        //
        // The Dithering order matches ui::gradientDitherFromChoice / core::vec::DitherKind: the
        // three kinds are each genuinely different in output (a tiled matrix, a high-frequency
        // noise tile, and flat white noise). Error diffusion is absent by design -- it is not a
        // point function of the pixel, so it cannot ride the shared paint sampler (docs §7).
        return {choice("type", _("Type"),
                       {_("Linear"), _("Radial"), _("Elliptical"), _("Conic")}, 0,
                       _("Gradient shape")),
                gradientStopsButton(),
                choice("dither", _("Dithering"),
                       {_("None"), _("Ordered"), _("Blue noise"), _("Noise")}, 0,
                       _("Break up 8-bit banding in a smooth ramp: Ordered is a fine repeating "
                         "matrix, Blue noise a pattern-free high-frequency tile, Noise plain "
                         "film grain")),
                slider("opacity", _("Opacity"), 0, 100, 1, 100, _("%"), _("Gradient layer opacity"))};
    case ToolId::Eyedropper:
        // ⚠ "Source" defaults to ALL LAYERS (index 1), and the eyedropper is the ONLY tool with
        // that default -- the Magic Wand and the Edge Select Brush keep Active Layer. The
        // eyedropper is a WYSIWYG instrument: the loupe magnifies the composite, the user aims at a
        // colour they can see on the canvas, and picking the active layer's own (possibly
        // transparent, possibly hidden-behind) pixel instead is a surprise every time the stack is
        // more than one layer deep. A selection tool is not that -- a wand that silently floods
        // merged pixels selects a region the active layer does not contain, which is a different
        // tool, not a friendlier one. (User call, 2026-07-29; it also governs the temporary
        // Ctrl-loupe, which borrows exactly these options -- MainWindow::eyedropperSample.)
        return {choice("sample", _("Sample"), {_("Point"), "3×3", "5×5", "11×11"}, 0,
                       _("Average the picked color over this neighborhood")),
                choice("source", _("Source"), {_("Active Layer"), _("All Layers")}, 1,
                       _("Sample from the active layer or the composite"))};
    case ToolId::RectShape:
        // S26-c: the shape tools author a FILLED shape in the active foreground colour and nothing
        // else -- the Paint (Fill/Outline/Both) picker and its Stroke-width slider are RETIRED. An
        // outline is a Layer Effects Stroke now, added from the ordinary Layer Effects UI exactly as
        // on any other layer; the Shape bar carries no route of its own to it. Only the shape's own
        // hot parameter is left here; buildShapeDraft() (shape_gesture) consumes it. The "Transform"
        // toggle (§7.1) picks what the selection-box handles do when a shape is selected.
        return {slider("radius", _("Corner"), 0, 2000, 1, 0, _("px"), _("Corner radius")),
                shapeTransformToggle(), shapeDesignerButton()};
    case ToolId::EllipseShape:
        return {shapeTransformToggle(), shapeDesignerButton()};
    case ToolId::PolygonShape:
        return {number("sides", _("Sides"), 3, 60, 1, 5, _("Number of polygon sides")),
                shapeTransformToggle(), shapeDesignerButton()};
    case ToolId::StarShape:
        return {number("points", _("Points"), 3, 60, 1, 5, _("Number of star points")),
                slider("inner", _("Inner"), 5, 95, 1, 50, _("%"),
                       _("Inner radius as a percentage of the outer")),
                shapeTransformToggle(), shapeDesignerButton()};
    case ToolId::LineShape:
        // THE LINE EXCEPTION (S26-c): a line has no interior, so a fill cannot express it -- its
        // stroke IS the shape, and Weight + Cap therefore stay tool options where every other kind
        // lost its stroke controls. The §7.5 paint modes (Hollow / Outlined) went with the rest:
        // "a line with a contrasting border" is precisely an outline, and an outline is a Stroke
        // layer effect now. (LineShape::paint / borderWidth stay in the model, so an older document
        // still renders.)
        return {slider("weight", _("Weight"), 1, 200, 1, 3, _("px"), _("Line thickness")),
                choice("cap", _("Cap"), {_("Butt"), _("Round"), _("Square")}, 0,
                       _("How the line ends are drawn")),
                shapeTransformToggle(), shapeDesignerButton()};
    case ToolId::Pen:
        // S28. The Pen is the ONE vector tool that still authors a stroke of its own: an open path
        // has no interior, so -- exactly like the Line shape (S26-c's single exception) -- a stroke
        // is the only way most pen paths can exist at all, and PLAN S28 names "custom stroke"
        // outright. The params are S27's set: width / cap / join / dash. `fill` is the second half
        // (a closed path is a region), and with BOTH switched off ui::penPaintedObject forces the
        // stroke back on rather than leaving an invisible layer behind. An outline AROUND the whole
        // path is still a Layer Effects Stroke, as for every other kind.
        return {toggle("fill", _("Fill"), true,
                       _("Fill the finished path with the foreground color")),
                toggle("stroke", _("Stroke"), true, _("Draw the path's outline")),
                slider("weight", _("Weight"), 0.1, 200, 0.1, 2, _("px"), _("Stroke width")),
                choice("cap", _("Cap"), {_("Butt"), _("Round"), _("Square")}, 1,
                       _("How open ends are drawn")),
                choice("join", _("Join"), {_("Miter"), _("Round"), _("Bevel")}, 1,
                       _("How corners are drawn")),
                choice("dash", _("Dash"), {_("Solid"), _("Dashed"), _("Dotted"), _("Dash-dot")}, 0,
                       _("Stroke dash pattern"))};
    case ToolId::Text:
        // S29-c §8.2 (rev 1/2) -- the bar keeps only the two HOT type controls (Font + Size); Bold,
        // Italic, Align and everything else moved into the Type panel (the "Style…" button). The
        // "font" choices are a placeholder seeded with the OS default; the app populates the real
        // family list from the FontDB once it exists (the tool registry is built before the FontDB).
        // These edit the CURRENT selection's style (or the new-text defaults when none) via the
        // selection funnel (VulkanCanvas::applySelectionStyle), the Type twin of shape select-to-edit.
        return {[] { auto o = choice("font", _("Font"), {_("Sans")}, 0, _("Font family"));
                     o.kind = ToolOptionKind::Font; // open list previews each family in its own face
                     return o; }(),
                slider("size", _("Size"), 6, 400, 0.5, 48, _("pt"), // half-point steps (matches the panel)
                       _("Font size")),
                // B/I/U/S styled glyph toggles, flowing inline right before "Style…" (user 2026-07-01);
                // they edit the selection (or the new-text defaults) exactly like Font/Size.
                styleToggle("bold", _("Bold weight"), ToolGlyph::Bold),
                styleToggle("italic", _("Italic / oblique"), ToolGlyph::Italic, /*joinPrev=*/true),
                styleToggle("underline", _("Underline"), ToolGlyph::Underline, /*joinPrev=*/true),
                styleToggle("strikethrough", _("Strikethrough"), ToolGlyph::Strike, /*joinPrev=*/true),
                // Writing mode + Latin orientation live in the Type panel (below Language), not the bar
                // -- a vertical-columns dropdown reads out of place inline with Font/Size (user 2026-07-01).
                // Anti-alias moved into the panel's Advanced section too (R4): a block-level render
                // property nobody flips mid-typing has no claim to bar space.
                typePanelButton(), // "Style…"
                type3dButton()};   // "3D…" -- reserved (greyed) beside it
    case ToolId::Zoom:
        return {choice("mode", _("Mode"), {_("In"), _("Out")}, 0,
                       _("Click to zoom in or out"))};
    }
    return {};
}

} // namespace

std::string Tool::tooltip() const {
    if (m_shortcut.empty())
        return m_name;
    return m_name + " (" + m_shortcut + ")";
}

ToolManager::ToolManager() {
    m_tools.reserve(kToolDefs.size());
    for (const ToolDef& d : kToolDefs) {
        m_tools.push_back(std::make_unique<Tool>(d.id, _(d.name), d.shortcut,
                                                 std::string(defaultIconSvg(d.id)), d.group,
                                                 d.slot, defaultOptionsFor(d.id)));
        // First-seen slot order = toolbar order; the slot's first tool is its default shown variant.
        if (m_shownPerSlot.emplace(d.slot, d.id).second)
            m_slots.push_back(d.slot);
    }
    m_active = kToolDefs.front().id; // Move
    if (m_eraserSizeTie)
        seedEraserSize(); // the tie defaults on: the pair must not START split (24 vs 40)
}

namespace {
double* optionValue(Tool* tool, const char* id) {
    if (tool == nullptr)
        return nullptr;
    for (ToolOption& o : tool->options())
        if (o.id == id)
            return &o.value;
    return nullptr;
}
} // namespace

void ToolManager::seedEraserSize() {
    double* brush = optionValue(find(ToolId::Brush), "size");
    double* eraser = optionValue(find(ToolId::Eraser), "size");
    if (brush != nullptr && eraser != nullptr)
        *eraser = *brush;
}

void ToolManager::setBrushSmoothingEnabled(bool on) {
    for (const ToolId id : {ToolId::Brush, ToolId::Eraser, ToolId::InpaintBrush, ToolId::CloneStamp})
        if (double* v = optionValue(find(id), "smoothing"); v != nullptr)
            *v = on ? 1.0 : 0.0;
}

int ToolManager::syncBrushSmoothing() {
    const double* src = optionValue(activeTool(), "smoothing");
    if (src == nullptr)
        return -1; // not a brush-family tool; nothing to sync
    const bool on = *src > 0.5;
    setBrushSmoothingEnabled(on);
    return on ? 1 : 0;
}

void ToolManager::setEraserSizeTie(bool on) {
    if (m_eraserSizeTie == on)
        return;
    m_eraserSizeTie = on;
    if (!on)
        return; // untied: each keeps its current value and they drift independently from here
    seedEraserSize();
    notifyOptionsChanged(); // the eraser's bar may be visible right now: let it re-read
}

void ToolManager::syncEraserSizeTie() {
    if (!m_eraserSizeTie)
        return;
    const bool fromBrush = m_active == ToolId::Brush;
    if (!fromBrush && m_active != ToolId::Eraser)
        return;
    double* src = optionValue(find(fromBrush ? ToolId::Brush : ToolId::Eraser), "size");
    double* dst = optionValue(find(fromBrush ? ToolId::Eraser : ToolId::Brush), "size");
    if (src != nullptr && dst != nullptr)
        *dst = *src;
}

Tool* ToolManager::find(ToolId id) const {
    for (const std::unique_ptr<Tool>& t : m_tools)
        if (t->id() == id)
            return t.get();
    return nullptr;
}

std::vector<Tool*> ToolManager::toolsInSlot(ToolSlot slot) const {
    std::vector<Tool*> out;
    for (const std::unique_ptr<Tool>& t : m_tools)
        if (t->slot() == slot)
            out.push_back(t.get());
    return out;
}

ToolSlot ToolManager::slotOf(ToolId id) const {
    if (const Tool* t = find(id))
        return t->slot();
    if (const Tool* a = find(m_active)) // missing id: fall back to the active tool's slot
        return a->slot();
    return ToolSlot::Move; // unreachable -- m_active is always a registered tool
}

ToolId ToolManager::shownToolForSlot(ToolSlot slot) const {
    const auto it = m_shownPerSlot.find(slot);
    return it != m_shownPerSlot.end() ? it->second : m_active;
}

void ToolManager::setActive(ToolId id) {
    if (id == m_active || find(id) == nullptr)
        return;
    m_previous = m_active; // remember where we came from (the early return skips same-tool re-selects)
    m_active = id;
    m_shownPerSlot[slotOf(id)] = id; // the slot now shows (and remembers) this variant
    if (m_onChange)
        m_onChange();
}

void ToolManager::applyIcons(const std::function<IconSource(ToolId)>& iconFor) {
    if (!iconFor)
        return;
    for (const std::unique_ptr<Tool>& tool : m_tools) {
        IconSource icon = iconFor(tool->id());
        if (!icon.empty())
            tool->setIcon(std::move(icon));
    }
}

std::optional<ToolId> toolForShortcut(char key) {
    const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(key)));
    for (const ToolDef& d : kToolDefs)
        if (d.shortcut[0] == up)
            return d.id;
    return std::nullopt;
}

} // namespace mosaic::ui
