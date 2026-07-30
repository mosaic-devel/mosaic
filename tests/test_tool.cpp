#include <doctest/doctest.h>

#include "ui/tool.hpp"

#include <algorithm>
#include <optional>

using mosaic::ui::Tool;
using mosaic::ui::ToolId;
using mosaic::ui::ToolManager;
using mosaic::ui::toolForShortcut;

TEST_CASE("ToolManager registers the built-in tools and starts on Move") {
    ToolManager m;
    CHECK(m.tools().size() == 28); // 19 slots; marquee/lasso/shape/select-brush/eye/warp have variants
    CHECK(m.active() == ToolId::Move);
    REQUIRE(m.activeTool() != nullptr);
    CHECK(m.activeTool()->id() == ToolId::Move);

    // Every registered tool has a name and a non-empty icon; ids are unique.
    for (const auto& t : m.tools()) {
        CHECK_FALSE(t->name().empty());
        CHECK_FALSE(t->icon().empty());
        CHECK(m.find(t->id()) == t.get());
    }
}

TEST_CASE("ToolManager::setActive switches the tool and fires onChange only on a real change") {
    ToolManager m;
    int changes = 0;
    m.setOnChange([&] { ++changes; });

    m.setActive(ToolId::Brush);
    CHECK(m.active() == ToolId::Brush);
    CHECK(changes == 1);

    m.setActive(ToolId::Brush); // already active -> no notification
    CHECK(changes == 1);

    m.setActive(ToolId::Eyedropper);
    CHECK(m.active() == ToolId::Eyedropper);
    CHECK(changes == 2);
}

TEST_CASE("ToolManager::previous tracks the prior tool, ignoring same-tool re-selects (S16-p)") {
    ToolManager m;
    CHECK(m.active() == ToolId::Move);
    CHECK(m.previous() == ToolId::Move); // initial: equal to the starting tool

    m.setActive(ToolId::Brush);
    CHECK(m.previous() == ToolId::Move); // came from Move

    m.setActive(ToolId::Brush); // re-selecting the active tool is a no-op -> previous unchanged
    CHECK(m.previous() == ToolId::Move);

    m.setActive(ToolId::Crop);
    CHECK(m.previous() == ToolId::Brush); // came from Brush -> the S16-p "return to previous" target
}

TEST_CASE("Tool::tooltip combines name and shortcut") {
    ToolManager m;
    const Tool* brush = m.find(ToolId::Brush);
    REQUIRE(brush != nullptr);
    CHECK(brush->shortcut() == "B");
    CHECK(brush->tooltip() == brush->name() + " (B)");
}

TEST_CASE("Tools publish a non-empty option set with well-formed sliders") {
    using mosaic::ui::ToolOption;
    using mosaic::ui::ToolOptionKind;
    ToolManager m;
    for (const auto& t : m.tools())
        CHECK_FALSE(t->options().empty()); // every tool has at least one option in S11-b

    const Tool* brush = m.find(ToolId::Brush);
    REQUIRE(brush != nullptr);
    // Size / Hardness / Flow / Opacity / Smoothing.
    REQUIRE(brush->options().size() == 5);
    const auto brushOpt = [&](std::string_view id) -> const ToolOption* {
        for (const ToolOption& o : brush->options())
            if (o.id == id)
                return &o;
        return nullptr;
    };
    const ToolOption* size = brushOpt("size");
    REQUIRE(size != nullptr);
    CHECK(size->kind == ToolOptionKind::Slider);
    CHECK(size->min <= size->value);
    CHECK(size->value <= size->max);
    CHECK(size->step > 0.0);

    // ⚠ THE PRESET IS NOT A TOOL OPTION. It was, briefly, as a stand-in Fl_Choice of 117 names --
    // which was never the design and, worse, was unusable. It lives in the right dock now, as a
    // filterable grid of thumbnails (docs/brushes.md §8.2, ui::BrushPresetPanel), and the tool's
    // option list must stay clear of it: an option would put it back on the context bar.
    CHECK(brushOpt("preset") == nullptr);

    // A Choice option's value indexes into its choices.
    const Tool* grad = m.find(ToolId::Gradient);
    REQUIRE(grad != nullptr);
    const ToolOption& type = grad->options().front();
    CHECK(type.kind == ToolOptionKind::Choice);
    REQUIRE_FALSE(type.choices.empty());
    CHECK(static_cast<std::size_t>(type.value) < type.choices.size());

    // The Crop tool carries the Smart Resize toggle (S16-f), OFF by default -- the canvas
    // wiring looks it up by this id, so pin the contract here.
    const Tool* crop = m.find(ToolId::Crop);
    REQUIRE(crop != nullptr);
    const auto& copts = crop->options();
    const auto smart = std::find_if(copts.begin(), copts.end(),
                                    [](const ToolOption& o) { return o.id == "smartResize"; });
    REQUIRE(smart != copts.end());
    CHECK(smart->kind == ToolOptionKind::Toggle);
    CHECK(smart->value == 0.0);
}

TEST_CASE("Warp tools (S35-b): one slot, two variants, and Rows/Columns lie only under Mesh") {
    using mosaic::ui::ToolGroup;
    using mosaic::ui::ToolOption;
    using mosaic::ui::ToolOptionKind;
    using mosaic::ui::ToolSlot;
    ToolManager m;

    const Tool* mesh = m.find(ToolId::MeshWarp);
    const Tool* persp = m.find(ToolId::PerspectiveWarp);
    REQUIRE(mesh != nullptr);
    REQUIRE(persp != nullptr);
    CHECK(mesh->group() == ToolGroup::SelectTransform); // geometry, not paint -- it sits after Crop
    CHECK(persp->group() == ToolGroup::SelectTransform);
    CHECK(mesh->slot() == ToolSlot::Warp);
    CHECK(persp->slot() == ToolSlot::Warp);
    CHECK(mesh->shortcut() == "Q");
    CHECK(persp->shortcut() == "Q"); // variants share their slot's letter
    CHECK_FALSE(mesh->icon().empty());
    CHECK_FALSE(persp->icon().empty());

    const auto opt = [](const Tool* t, const char* id) -> const ToolOption* {
        for (const ToolOption& o : t->options())
            if (o.id == id)
                return &o;
        return nullptr;
    };
    // Rows / Columns exist on BOTH -- so the bar's layout does not jump when the variant changes --
    // but they are DEACTIVATED under Perspective, where one homography has four corners and no
    // interior control points. A control that lies about what it does is worse than a greyed one.
    for (const Tool* t : {mesh, persp}) {
        for (const char* id : {"rows", "cols"}) {
            const ToolOption* o = opt(t, id);
            REQUIRE_MESSAGE(o != nullptr, id);
            CHECK(o->kind == ToolOptionKind::Number);
            CHECK(o->min == 2.0);
            CHECK(o->max == 12.0); // ui::kWarpMaxNodes -- the overlay lane's vertex budget
            CHECK(o->value == 4.0);
        }
    }
    CHECK(opt(mesh, "rows")->enabled);
    CHECK(opt(mesh, "cols")->enabled);
    CHECK_FALSE(opt(persp, "rows")->enabled);
    CHECK_FALSE(opt(persp, "cols")->enabled);

    // Quality is the Move tool's Anti-aliasing list VERBATIM -- one kernel order for the whole app.
    const ToolOption* quality = opt(mesh, "quality");
    const ToolOption* aa = opt(m.find(ToolId::Move), "aa");
    REQUIRE(quality != nullptr);
    REQUIRE(aa != nullptr);
    CHECK(quality->kind == ToolOptionKind::Choice);
    CHECK(quality->choices == aa->choices);
    CHECK(quality->value == 0.0); // Auto

    CHECK(opt(mesh, "grid")->kind == ToolOptionKind::Toggle);
    CHECK(opt(mesh, "grid")->value == 1.0); // the lattice shows by default

    // The Crop tool's action precedent verbatim: an affirmative Apply, a NEUTRAL Cancel (red is
    // reserved for genuinely destructive actions).
    const ToolOption* apply = opt(mesh, "apply");
    const ToolOption* cancel = opt(mesh, "cancel");
    REQUIRE(apply != nullptr);
    REQUIRE(cancel != nullptr);
    CHECK(apply->kind == ToolOptionKind::Button);
    CHECK(apply->accent == mosaic::ui::ToolAccent::Affirmative);
    CHECK(cancel->accent == mosaic::ui::ToolAccent::None);
}

TEST_CASE("The options bar shows only the curated `primary` subset (brush trims to Size + Opacity)") {
    ToolManager m;

    // Every tool exposes at least one primary option, so the bar is never empty for an active tool.
    for (const auto& t : m.tools()) {
        const auto& opts = t->options();
        CHECK(std::any_of(opts.begin(), opts.end(), [](const auto& o) { return o.primary; }));
    }

    // The bar's hot set is Size + Opacity + Smoothing; Hardness + Flow are demoted to the (future
    // S19) brush editor -- and Hardness has since stopped meaning anything at all to a preset with a
    // real tip, which carries its own edge (brush_engine.hpp). Smoothing is on the BAR, not that
    // panel, and not on the Tablet page it was first built on: it steadies the POINTER, so it applies
    // to the mouse every bit as much as to the pen, and it belongs where the user is actually
    // painting (user call, 2026-07-11). The PRESET is on none of them: it is the dock's now (§8.2).
    const Tool* brush = m.find(ToolId::Brush);
    REQUIRE(brush != nullptr);
    REQUIRE(brush->options().size() == 5);
    int primaryCount = 0;
    for (const auto& o : brush->options()) {
        const bool hot = (o.id == "size" || o.id == "opacity" || o.id == "smoothing");
        CHECK(o.primary == hot); // exactly Size + Opacity + Smoothing are primary
        primaryCount += o.primary ? 1 : 0;
    }
    CHECK(primaryCount == 3); // Size, Opacity, Smoothing
}

TEST_CASE("Tools carry a toolbar group, with boundaries that drive the column dividers") {
    using mosaic::ui::ToolGroup;
    ToolManager m;
    CHECK(m.find(ToolId::Move)->group() == ToolGroup::SelectTransform);
    CHECK(m.find(ToolId::Crop)->group() == ToolGroup::SelectTransform);
    CHECK(m.find(ToolId::Brush)->group() == ToolGroup::PaintFill);
    CHECK(m.find(ToolId::Gradient)->group() == ToolGroup::PaintFill);
    CHECK(m.find(ToolId::Eyedropper)->group() == ToolGroup::Sample);
    CHECK(m.find(ToolId::Text)->group() == ToolGroup::VectorText);
    CHECK(m.find(ToolId::Zoom)->group() == ToolGroup::View);

    // Toolbar order has several group boundaries, so the column draws at least a few dividers.
    const auto& tools = m.tools();
    int boundaries = 0;
    for (std::size_t i = 1; i < tools.size(); ++i)
        boundaries += (tools[i]->group() != tools[i - 1]->group()) ? 1 : 0;
    CHECK(boundaries >= 3);
}

TEST_CASE("Toolbar slots collapse variant tools and remember the last-active variant (S11-e)") {
    using mosaic::ui::ToolSlot;
    ToolManager m;

    // 19 distinct slots, in toolbar order, starting at Move (S28 added the Pen's own slot; S38-b
    // the eye tool's; S38 the clone stamp's; S35-b the warp's).
    CHECK(m.slots().size() == 19);
    CHECK(m.slots().front() == ToolSlot::Move);
    CHECK(m.toolsInSlot(ToolSlot::MagicWand).size() == 1);   // S17: its own click-tool slot
    CHECK(m.toolsInSlot(ToolSlot::SelectBrush).size() == 2); // S18 + the L1 edge-brush variant
    CHECK(m.toolsInSlot(ToolSlot::RedEye).size() == 2);      // S38-b: flash red-eye + sclera
    CHECK(m.toolsInSlot(ToolSlot::CloneStamp).size() == 1);  // S38: its own stroke-tool slot
    CHECK(m.toolsInSlot(ToolSlot::Warp).size() == 2);        // S35-b: mesh + perspective

    // The marquee / lasso / shape slots hold multiple variants; the rest hold one tool each.
    CHECK(m.toolsInSlot(ToolSlot::Marquee).size() == 2); // rectangular + elliptical
    CHECK(m.toolsInSlot(ToolSlot::Lasso).size() == 2);   // free + polygonal
    CHECK(m.toolsInSlot(ToolSlot::Shape).size() == 5);   // rectangle + ellipse + polygon + star + line
    CHECK(m.toolsInSlot(ToolSlot::Brush).size() == 1);
    CHECK(m.toolsInSlot(ToolSlot::Inpaint).size() == 1); // S39 inpaint brush, its own slot
    CHECK(m.toolsInSlot(ToolSlot::Pen).size() == 1);     // S28 pen/path tool, its own slot
    CHECK(m.slotOf(ToolId::EllipseMarquee) == ToolSlot::Marquee);
    CHECK(m.slotOf(ToolId::LineShape) == ToolSlot::Shape);
    CHECK(m.slotOf(ToolId::InpaintBrush) == ToolSlot::Inpaint);

    // A slot shows its first variant until one is picked, then it remembers the choice -- even after
    // the active tool moves to another slot.
    CHECK(m.shownToolForSlot(ToolSlot::Marquee) == ToolId::RectMarquee);
    m.setActive(ToolId::EllipseMarquee);
    CHECK(m.shownToolForSlot(ToolSlot::Marquee) == ToolId::EllipseMarquee);
    m.setActive(ToolId::Brush);
    CHECK(m.shownToolForSlot(ToolSlot::Marquee) == ToolId::EllipseMarquee);
    CHECK(m.shownToolForSlot(ToolSlot::Brush) == ToolId::Brush);
}

TEST_CASE("Editing an option value persists on the tool and notifyOptionsChanged fires") {
    ToolManager m;
    int edits = 0;
    m.setOnOptionsChanged([&] { ++edits; });

    Tool* brush = m.find(ToolId::Brush);
    REQUIRE(brush != nullptr);
    brush->options()[0].value = 123.0; // a surface writes the edited value back
    m.notifyOptionsChanged();
    CHECK(edits == 1);
    CHECK(m.find(ToolId::Brush)->options()[0].value == doctest::Approx(123.0)); // persisted
}

TEST_CASE("toolForShortcut maps keys case-insensitively, ignoring unknown keys") {
    CHECK(toolForShortcut('v') == std::optional<ToolId>(ToolId::Move));
    CHECK(toolForShortcut('V') == std::optional<ToolId>(ToolId::Move));
    CHECK(toolForShortcut('b') == std::optional<ToolId>(ToolId::Brush));
    CHECK(toolForShortcut('j') == std::optional<ToolId>(ToolId::InpaintBrush)); // S39
    CHECK(toolForShortcut('t') == std::optional<ToolId>(ToolId::Text));
    CHECK(toolForShortcut('w') == std::optional<ToolId>(ToolId::MagicWand)); // S17
    CHECK(toolForShortcut('a') == std::optional<ToolId>(ToolId::SelectBrush)); // S18
    CHECK(toolForShortcut('s') == std::optional<ToolId>(ToolId::CloneStamp));  // S38
    // S35-b: Q is the warp slot's letter, and it resolves to the slot's FIRST variant -- the same
    // first-claim-wins rule that makes M the rectangular marquee.
    CHECK(toolForShortcut('q') == std::optional<ToolId>(ToolId::MeshWarp));
    CHECK(toolForShortcut('Q') == std::optional<ToolId>(ToolId::MeshWarp));
    CHECK(toolForShortcut('z') == std::optional<ToolId>(ToolId::Zoom));
    // H and O stay unclaimed on purpose: they are reserved for the tools whose names begin with
    // them (a Hand tool, the dodge/burn family) and whose art the icon pack already carries.
    CHECK_FALSE(toolForShortcut('h').has_value());
    CHECK_FALSE(toolForShortcut('o').has_value());
    CHECK_FALSE(toolForShortcut('1').has_value());
}

TEST_CASE("Magic wand (S17): own slot, SelectTransform group, tolerance/contiguous/aa/source") {
    using mosaic::ui::ToolGroup;
    using mosaic::ui::ToolOptionKind;
    using mosaic::ui::ToolSlot;
    ToolManager m;

    Tool* wand = m.find(ToolId::MagicWand);
    REQUIRE(wand != nullptr);
    CHECK(wand->group() == ToolGroup::SelectTransform);
    CHECK(wand->slot() == ToolSlot::MagicWand); // OQ-1: a distinct click tool, not a marquee variant
    CHECK(wand->shortcut() == "W");
    CHECK_FALSE(wand->icon().empty());

    // The published options (§8): a 0-100 tolerance slider, Contiguous + Anti-alias on by default,
    // and the Eyedropper's Active-Layer / All-Layers Source LABELS (not its default -- see the
    // "Source defaults" case below).
    const auto opt = [&](const char* id) -> const mosaic::ui::ToolOption* {
        for (const auto& o : wand->options())
            if (o.id == id)
                return &o;
        return nullptr;
    };
    const auto* tol = opt("tolerance");
    REQUIRE(tol != nullptr);
    CHECK(tol->kind == ToolOptionKind::Slider);
    CHECK(tol->min == doctest::Approx(0.0));
    CHECK(tol->max == doctest::Approx(100.0));
    const auto* contig = opt("contiguous");
    REQUIRE(contig != nullptr);
    CHECK(contig->kind == ToolOptionKind::Toggle);
    CHECK(contig->value == doctest::Approx(1.0)); // default on (D4)
    const auto* aa = opt("antialias");
    REQUIRE(aa != nullptr);
    CHECK(aa->value == doctest::Approx(1.0)); // default on (D5)
    const auto* source = opt("source");
    REQUIRE(source != nullptr);
    CHECK(source->kind == ToolOptionKind::Choice);
    REQUIRE(source->choices.size() == 2);
    CHECK(source->value == doctest::Approx(0.0)); // Active Layer by default (D8)
}

TEST_CASE("Source defaults: All Layers for the Eyedropper ONLY, Active Layer for the two selection "
          "tools") {
    using mosaic::ui::ToolOption;
    using mosaic::ui::ToolOptionKind;
    ToolManager m;

    const auto source = [&](ToolId id) -> const ToolOption* {
        const Tool* t = m.find(id);
        if (t == nullptr)
            return nullptr; // no REQUIRE in here: the caller's REQUIRE reports which tool it was
        for (const ToolOption& o : t->options())
            if (o.id == "source")
                return &o;
        return nullptr;
    };
    const ToolOption* eye = source(ToolId::Eyedropper);
    const ToolOption* wand = source(ToolId::MagicWand);
    const ToolOption* edge = source(ToolId::EdgeBrush);
    REQUIRE(eye != nullptr);
    REQUIRE(wand != nullptr);
    REQUIRE(edge != nullptr);

    // All three offer the SAME two choices in the SAME order -- index 0 = Active Layer, 1 = All
    // Layers -- because they resolve through one pair of helpers in MainWindow
    // (activeLayerDocImage / wandMergedSource) that switches on that raw index. Compared tool to
    // tool rather than to literals so the assertion survives translation.
    CHECK(eye->kind == ToolOptionKind::Choice);
    REQUIRE(eye->choices.size() == 2);
    CHECK(wand->choices == eye->choices);
    CHECK(edge->choices == eye->choices);

    // The eyedropper is a WYSIWYG instrument and starts on All Layers: the loupe magnifies the
    // composite, so picking the active layer's own pixel instead disagrees with what the user is
    // aiming at. This ALSO governs the temporary Ctrl-loupe over a stroke tool, which reads the
    // Eyedropper's options whatever tool is active (MainWindow::eyedropperSample).
    CHECK(eye->value == 1.0); // All Layers -- exact index, no tolerance to hide a wrong one

    // The two selection tools deliberately do NOT follow. A flood / geodesic grow over merged
    // pixels selects a region the active layer does not contain; that has to be the user's act.
    // If a future change moves either of these, it is a behaviour change and this is the tripwire.
    CHECK(wand->value == 0.0);
    CHECK(edge->value == 0.0);

    // The sample-size neighbourhood is untouched by all of this: Point by default.
    const Tool* eyeTool = m.find(ToolId::Eyedropper);
    REQUIRE(eyeTool != nullptr);
    const auto sampleOpt =
        std::find_if(eyeTool->options().begin(), eyeTool->options().end(),
                     [](const ToolOption& o) { return o.id == "sample"; });
    REQUIRE(sampleOpt != eyeTool->options().end());
    CHECK(sampleOpt->kind == ToolOptionKind::Choice);
    CHECK(sampleOpt->value == 0.0);
    CHECK(sampleOpt->choices.size() == 4); // Point / 3x3 / 5x5 / 11x11
}

TEST_CASE("Select brush (S18): own slot, SelectTransform group, shortcut A, size/hardness/flow/opacity") {
    using mosaic::ui::ToolGroup;
    using mosaic::ui::ToolOptionKind;
    using mosaic::ui::ToolSlot;
    ToolManager m;

    Tool* sb = m.find(ToolId::SelectBrush);
    REQUIRE(sb != nullptr);
    CHECK(sb->group() == ToolGroup::SelectTransform); // it produces a SetSelectionCommand, not paint
    CHECK(sb->slot() == ToolSlot::SelectBrush);       // §9-A: its own slot, not a wand flyout variant
    CHECK(sb->shortcut() == "A");
    CHECK_FALSE(sb->icon().empty());

    // Size / Hardness / Flow / Opacity; Size + Opacity are the bar's hot pair (like the Brush), the
    // combine op is a Settings field + Alt modifier (§9-B), not a bar control.
    const auto opt = [&](const char* id) -> const mosaic::ui::ToolOption* {
        for (const auto& o : sb->options())
            if (o.id == id)
                return &o;
        return nullptr;
    };
    for (const char* id : {"size", "hardness", "flow", "opacity"}) {
        const auto* o = opt(id);
        REQUIRE_MESSAGE(o != nullptr, id);
        CHECK(o->kind == ToolOptionKind::Slider);
    }
    CHECK(opt("size")->primary);
    CHECK(opt("opacity")->primary);
    CHECK_FALSE(opt("hardness")->primary);
    CHECK_FALSE(opt("flow")->primary);
    // Not an image-analysis tool: no tolerance / contiguous / source knobs (that is the wand's line).
    CHECK(opt("tolerance") == nullptr);
    CHECK(opt("source") == nullptr);
}

namespace {
double sizeOf(ToolManager& m, ToolId id) {
    Tool* t = m.find(id);
    REQUIRE(t != nullptr);
    for (mosaic::ui::ToolOption& o : t->options())
        if (o.id == "size")
            return o.value;
    FAIL("tool has no size option");
    return 0.0;
}

void setSize(ToolManager& m, ToolId id, double v) {
    Tool* t = m.find(id);
    REQUIRE(t != nullptr);
    for (mosaic::ui::ToolOption& o : t->options())
        if (o.id == "size")
            o.value = v;
    m.notifyOptionsChanged(); // what the options surfaces do after writing
}
} // namespace

TEST_CASE("the eraser-size tie starts on and seeds the pair from the Brush (S19 §8.4)") {
    // The registered defaults differ (Brush 24, Eraser 40); with the tie defaulting on, the pair
    // must not START split -- the eraser opens at the brush's 24.
    ToolManager m;
    CHECK(m.eraserSizeTie());
    CHECK(sizeOf(m, ToolId::Brush) == doctest::Approx(24.0));
    CHECK(sizeOf(m, ToolId::Eraser) == doctest::Approx(24.0));
}

TEST_CASE("the eraser-size tie mirrors the active side's edits to the other, both ways") {
    ToolManager m;
    m.setActive(ToolId::Brush);
    setSize(m, ToolId::Brush, 60.0); // resize while painting...
    CHECK(sizeOf(m, ToolId::Eraser) == doctest::Approx(60.0)); // ... the eraser follows

    m.setActive(ToolId::Eraser);
    setSize(m, ToolId::Eraser, 12.0); // resize while erasing...
    CHECK(sizeOf(m, ToolId::Brush) == doctest::Approx(12.0)); // ... one shared value, not a master

    // An edit to an unrelated active tool leaves the pair alone.
    m.setActive(ToolId::InpaintBrush);
    setSize(m, ToolId::InpaintBrush, 99.0);
    CHECK(sizeOf(m, ToolId::Brush) == doctest::Approx(12.0));
    CHECK(sizeOf(m, ToolId::Eraser) == doctest::Approx(12.0));
}

TEST_CASE("untying frees the sizes; re-tying seeds the eraser from the brush and notifies") {
    ToolManager m;
    m.setEraserSizeTie(false);
    m.setActive(ToolId::Eraser);
    setSize(m, ToolId::Eraser, 200.0); // independent while untied
    CHECK(sizeOf(m, ToolId::Brush) == doctest::Approx(24.0));

    int notified = 0;
    m.setOnOptionsChanged([&] { ++notified; });
    m.setEraserSizeTie(true); // re-tie: the brush is the source of truth at enable time
    CHECK(sizeOf(m, ToolId::Eraser) == doctest::Approx(24.0));
    CHECK(notified == 1); // the eraser's bar may be showing the moved value: it must re-read
    m.setEraserSizeTie(true); // no-op: already on
    CHECK(notified == 1);
}

TEST_CASE("brush smoothing is ONE preference across the brush-family tools") {
    using mosaic::ui::ToolOption;
    // It steadies the POINTER, not the tip -- so it is not a per-tool taste like Size or Hardness, and
    // the Brush, the Eraser and the Inpaint brush must not drift apart. Whichever bar the user just
    // clicked wins, and the others follow.
    //
    // It is a TOGGLE, not a slider: there is no useful "a little bit of rattle" (user call). It stays
    // a toggle rather than becoming unconditional because smoothing DISCARDS the user's actual input,
    // so it is not strictly-better -- pixel-precise work has a real claim on raw data.
    ToolManager m;
    const auto smoothing = [&](ToolId id) {
        const Tool* t = m.find(id);
        REQUIRE(t != nullptr);
        for (const ToolOption& o : t->options())
            if (o.id == "smoothing") {
                CHECK(o.kind == mosaic::ui::ToolOptionKind::Toggle); // never a slider again
                return o.value > 0.5;
            }
        FAIL("brush-family tool has no smoothing option");
        return false;
    };

    CHECK(smoothing(ToolId::Brush)); // ON by default -- a raw mouse stroke rattles

    m.setBrushSmoothingEnabled(false);
    CHECK_FALSE(smoothing(ToolId::Brush));
    CHECK_FALSE(smoothing(ToolId::Eraser));
    CHECK_FALSE(smoothing(ToolId::InpaintBrush));

    SUBCASE("clicking it on the ERASER carries it to the Brush") {
        m.setActive(ToolId::Eraser);
        for (ToolOption& o : m.find(ToolId::Eraser)->options())
            if (o.id == "smoothing")
                o.value = 1.0; // the user clicks the Eraser's toggle
        CHECK(m.syncBrushSmoothing() == 1);
        CHECK(smoothing(ToolId::Brush));
        CHECK(smoothing(ToolId::InpaintBrush));
    }

    SUBCASE("a tool with no smoothing option syncs nothing, and says so") {
        m.setBrushSmoothingEnabled(true);
        m.setActive(ToolId::BucketFill);
        CHECK(m.syncBrushSmoothing() == -1);      // "not a brush-family tool"
        CHECK(smoothing(ToolId::Brush));          // ... and nothing was clobbered
    }
}
