#include "ui/layer_panel.hpp"

#include "core/command.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/layer_effects.hpp"          // LayerEffects: the effects-revision cache key
#include "core/texture/texture_params.hpp" // Generator / defaultTextureParams (texture-badge path)
#include "core/vector/object.hpp"          // vec::Object / Path / ParametricShape (the path badge)
#include "ui/widgets.hpp" // ellipsizeToWidth

#include <FL/Fl.H> // Fl::e_number: the opacity callback branches on the live event
#include <FL/fl_draw.H>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace mosaic;

TEST_CASE("layerThumbnail renders an opaque, box-sized thumbnail of a raster layer") {
    core::Document doc(100, 100);
    auto raster = doc.makeRaster("R", 100, 100);
    raster->image().fill(common::Color8{220, 30, 30, 255}); // opaque red

    const common::Image thumb = ui::layerThumbnail(*raster, 40, 100, 100);
    CHECK(thumb.width == 40);
    CHECK(thumb.height == 40);

    bool allOpaque = true;
    for (std::size_t i = 3; i < thumb.rgba.size(); i += 4)
        if (thumb.rgba[i] != 255)
            allOpaque = false;
    CHECK(allOpaque);

    // A square raster fills the whole box, so the centre is the (opaque) red.
    const std::size_t c = ((20u * 40) + 20) * 4;
    CHECK(thumb.rgba[c + 0] > 180);
    CHECK(thumb.rgba[c + 1] < 90);
    CHECK(thumb.rgba[c + 2] < 90);
}

TEST_CASE("layerThumbnail shows the checkerboard through transparency") {
    core::Document doc(100, 100);
    auto raster = doc.makeRaster("R", 100, 100); // zero-filled => fully transparent

    const common::Image thumb = ui::layerThumbnail(*raster, 40, 100, 100);
    const std::size_t c = ((20u * 40) + 20) * 4;
    const int v = thumb.rgba[c];
    CHECK((v == 150 || v == 205));            // a neutral checker gray
    CHECK(thumb.rgba[c + 0] == thumb.rgba[c + 1]);
    CHECK(thumb.rgba[c + 1] == thumb.rgba[c + 2]);
    CHECK(thumb.rgba[c + 3] == 255);
}

// The thumbnail is a portrait of the OBJECT, not of the canvas (user, 2026-07-09): a small shape
// in the corner of a big document used to render as an invisible speck. It now frames the layer's
// content bounds, so WHERE on the canvas the content sits cannot change the picture.
TEST_CASE("layerThumbnail frames the layer's content, wherever it sits on the canvas") {
    const auto redPatch = [](core::Document& doc, double tx, double ty) {
        auto raster = doc.makeRaster("R", 40, 40);
        for (std::uint32_t y = 0; y < 10; ++y)
            for (std::uint32_t x = 0; x < 10; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y) * 40 + x) * 4;
                raster->image().rgba[p] = 220;
                raster->image().rgba[p + 3] = 255;
            }
        raster->setTransform(common::Affine2D::translation(tx, ty));
        return raster;
    };
    core::Document docA(40, 40);
    core::Document docB(40, 40);
    const auto a = redPatch(docA, 0, 0);
    const auto b = redPatch(docB, 25, 25); // same content, opposite corner of the canvas

    const common::Image ta = ui::layerThumbnail(*a, 40, 40, 40);
    const common::Image tb = ui::layerThumbnail(*b, 40, 40, 40);
    CHECK(ta.rgba == tb.rgba); // framing follows the content, not the canvas position

    // And the content fills the box: the centre is the patch, not empty canvas.
    const std::size_t c = ((20u * 40) + 20) * 4;
    CHECK(ta.rgba[c] == 220);
}

// A layer whose content is a tiny speck must not be magnified into a full-bleed slab.
TEST_CASE("layerThumbnail caps magnification for a one-pixel layer") {
    core::Document doc(64, 64);
    auto raster = doc.makeRaster("R", 64, 64);
    const std::size_t p = (static_cast<std::size_t>(32) * 64 + 32) * 4;
    raster->image().rgba[p] = 220;
    raster->image().rgba[p + 3] = 255;

    const common::Image thumb = ui::layerThumbnail(*raster, 32, 64, 64);
    int red = 0;
    for (std::size_t i = 0; i + 3 < thumb.rgba.size(); i += 4)
        if (thumb.rgba[i] == 220 && thumb.rgba[i + 3] == 255 && thumb.rgba[i + 1] == 0)
            ++red;
    CHECK(red > 0);                        // the speck is visible ...
    CHECK(red < 32 * 32 / 4);              // ... but nowhere near filling the box (4x cap)
}

TEST_CASE("contentRevision advances when pixels are invalidated") {
    core::Document doc(8, 8);
    auto raster = doc.makeRaster("R");
    const std::uint64_t before = raster->contentRevision();
    raster->invalidateContentBounds();
    CHECK(raster->contentRevision() == before + 1); // the thumbnail cache keys off this
}

TEST_CASE("moveIndexFor adjusts the destination index for the dragged layer's removal") {
    // Reparent (different parent): no removal shift in the destination, so the index is unchanged.
    CHECK(ui::moveIndexFor(0, /*sameParent=*/false, 5) == 0);
    CHECK(ui::moveIndexFor(3, /*sameParent=*/false, 0) == 3);

    // Same parent: removing the dragged layer shifts everything ABOVE its old slot down by one, so
    // a target past the old slot drops by one; a target at/below it is unaffected.
    CHECK(ui::moveIndexFor(5, /*sameParent=*/true, 2) == 4); // target above old slot -> shift down
    CHECK(ui::moveIndexFor(2, /*sameParent=*/true, 5) == 2); // target below old slot -> unchanged
    CHECK(ui::moveIndexFor(2, /*sameParent=*/true, 2) == 2); // exactly at the old slot -> no-op move
}

TEST_CASE("Group Layers wraps a layer in a new group at its original stack slot") {
    // Mirrors LayerPanel::groupActive(): Add an empty group just above the target, then Move the
    // target into it -- as one CompositeCommand, so it is a single undo step.
    core::Document doc(64, 64);
    core::GroupLayer& root = doc.root();
    const core::LayerId a = root.addOnTop(doc.makeRaster("A", 64, 64)).id(); // bottom
    const core::LayerId b = root.addOnTop(doc.makeRaster("B", 64, 64)).id(); // top
    REQUIRE(root.childCount() == 2);

    const auto loc = doc.locate(a);
    REQUIRE(loc.has_value());
    auto group = doc.makeGroup("Group");
    const core::LayerId gid = group->id();
    auto composite = std::make_unique<core::CompositeCommand>("Group Layers");
    composite->add(std::make_unique<core::AddLayerCommand>(loc->parent->id(), loc->index + 1,
                                                           std::move(group)));
    composite->add(std::make_unique<core::MoveLayerCommand>(a, gid, 0));
    doc.commands().push(std::move(composite));

    // The group now sits where A was (bottom), holding A; B is untouched on top.
    REQUIRE(root.childCount() == 2);
    CHECK(root.child(0).id() == gid);
    CHECK(root.child(1).id() == b);
    auto* g = root.child(0).as<core::GroupLayer>();
    REQUIRE(g != nullptr);
    REQUIRE(g->childCount() == 1);
    CHECK(g->child(0).id() == a);

    // One undo step restores the flat two-layer stack.
    doc.commands().undo();
    REQUIRE(root.childCount() == 2);
    CHECK(root.child(0).id() == a);
    CHECK(root.child(1).id() == b);
    CHECK(doc.find(gid) == nullptr);
}

TEST_CASE("layerThumbnail falls back to a flat placeholder for kinds without pixels") {
    core::Document doc(100, 100);
    auto vec = doc.makeVector("V");

    const common::Image thumb = ui::layerThumbnail(*vec, 32, 100, 100);
    CHECK(thumb.width == 32);
    CHECK(thumb.height == 32);
    CHECK(thumb.rgba[0] == 60); // the neutral letterbox colour, fully opaque
    CHECK(thumb.rgba[1] == 64);
    CHECK(thumb.rgba[2] == 82);
    CHECK(thumb.rgba[3] == 255);
}

TEST_CASE("layerThumbnail renders a group's composited subtree (no more flat placeholder)") {
    core::Document doc(40, 40);
    auto* group = doc.root().addOnTop(doc.makeGroup("G")).as<core::GroupLayer>();
    auto child = doc.makeRaster("C", 40, 40);
    for (std::uint32_t y = 0; y < 10; ++y)
        for (std::uint32_t x = 0; x < 10; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 40 + x) * 4;
            child->image().rgba[p] = 220;
            child->image().rgba[p + 3] = 255;
        }
    child->setTransform(common::Affine2D::translation(20, 20));
    group->addOnTop(std::move(child));

    const common::Image thumb = ui::layerThumbnail(*group, 40, 40, 40);
    // The group's subtree is composited and then framed on its content, so the child fills the box
    // rather than sitting as a small square in the canvas's lower-right quadrant.
    const std::size_t c = ((20u * 40) + 20) * 4;
    CHECK(thumb.rgba[c] == 220);
    CHECK(thumb.rgba[c + 3] == 255);
}

TEST_CASE("thumbnailSelectOp: Ctrl/Alt on top of the Shift trigger choose the boolean op") {
    using mosaic::core::SelectOp;
    using mosaic::ui::thumbnailSelectOp;
    CHECK(thumbnailSelectOp(false, false) == SelectOp::Replace);
    CHECK(thumbnailSelectOp(true, false) == SelectOp::Add);
    CHECK(thumbnailSelectOp(false, true) == SelectOp::Subtract);
    CHECK(thumbnailSelectOp(true, true) == SelectOp::Intersect);
}

TEST_CASE("child thumbnails follow the parent group's transform (S15.z)") {
    core::Document doc(40, 40);
    auto* group = doc.root().addOnTop(doc.makeGroup("G")).as<core::GroupLayer>();
    auto child = doc.makeRaster("C", 40, 40);
    for (std::uint32_t y = 0; y < 10; ++y)
        for (std::uint32_t x = 0; x < 10; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 40 + x) * 4;
            child->image().rgba[p] = 220;
            child->image().rgba[p + 3] = 255;
        }
    const core::Layer* c = &group->addOnTop(std::move(child));
    group->setTransform(common::Affine2D::translation(20, 20));

    // The child's own thumbnail is framed on the child's content carried through the GROUP's
    // transform. The content is opaque red either way, so what this pins is that the world
    // transform still participates: a singular/ignored transform would frame nothing and the
    // centre would come back as checkerboard.
    const common::Image thumb = ui::layerThumbnail(*c, 40, 40, 40);
    const std::size_t mid = ((20u * 40) + 20) * 4;
    CHECK(thumb.rgba[mid] == 220);
    CHECK(thumb.rgba[mid + 3] == 255);
}

// ---- locking (S16-g) ---------------------------------------------------------------------------
// The lock's contract: it forbids STRUCTURAL edits (delete, group, reorder) while leaving the layer
// selectable and its visibility/opacity/blend editable. Pixel and transform edits are refused
// elsewhere (the brush guard and VulkanCanvas::beginMoveGesture). Constructing the panel needs FLTK
// but no display -- nothing is show()n here.
TEST_CASE("a locked layer refuses delete and group, and pushes no command") {
    core::Document doc(16, 16);
    const core::LayerId id = doc.root().addOnTop(doc.makeRaster("L")).id();

    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    panel.setActive(id);
    CHECK_FALSE(panel.activeLayerLocked());

    panel.toggleLocked(id);
    CHECK(doc.find(id)->locked());
    CHECK(panel.activeLayerLocked());
    const std::size_t stackAfterLock = doc.commands().size();

    panel.deleteActive();
    CHECK(doc.find(id) != nullptr);                     // still there
    CHECK(doc.commands().size() == stackAfterLock);     // and no no-op undo step was recorded

    panel.groupActive();
    CHECK(doc.root().childCount() == 1);                // not wrapped in a group
    CHECK(doc.commands().size() == stackAfterLock);

    // Visibility is deliberately NOT locked out: you can always hide a locked layer.
    panel.toggleVisible(id);
    CHECK_FALSE(doc.find(id)->visible());
    CHECK(doc.commands().size() == stackAfterLock + 1);

    panel.toggleLocked(id); // unlocking restores the structural edits
    CHECK_FALSE(panel.activeLayerLocked());
    panel.deleteActive();
    CHECK(doc.find(id) == nullptr);
}

// The texture badge is the fx badge's sibling (S55): clicking it must open the Texture Generator
// modal for THAT layer. openTextureFor mirrors openEffectsFor -- it activates the layer first (so the
// generator dialog opens in EDIT mode, §3.3) and then fires the host callback with the same id.
TEST_CASE("openTextureFor activates the texture layer, then fires the open-texture callback") {
    core::Document doc(32, 32);
    const core::LayerId raster = doc.root().addOnTop(doc.makeRaster("R")).id();
    const core::LayerId tex =
        doc.root()
            .addOnTop(doc.makeTexture(
                "T", core::texture::defaultTextureParams(core::texture::Generator::Sky)))
            .id();

    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    panel.setActive(raster);
    REQUIRE(panel.activeLayer() == raster);

    core::LayerId opened = core::kInvalidLayerId;
    int calls = 0;
    panel.setOnOpenTexture([&](core::LayerId id) {
        opened = id;
        ++calls;
    });

    panel.openTextureFor(tex);
    CHECK(panel.activeLayer() == tex); // activated first (edit-mode precondition)
    CHECK(calls == 1);
    CHECK(opened == tex); // and the host is asked to open the generator for exactly that layer
}

// A silently-ignored click on an enabled-looking button reads as a broken button. The bottom strip's
// Delete/Group must GREY on a locked layer, matching the context menu (which greys the same items).
TEST_CASE("the bottom strip's Delete and Group grey out for a locked layer") {
    core::Document doc(16, 16);
    const core::LayerId id = doc.root().addOnTop(doc.makeRaster("L")).id();
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    panel.setActive(id);

    const auto findButtons = [&] { // the strip's buttons, in construction order: add, group, delete
        std::vector<const Fl_Widget*> buttons;
        for (int i = 0; i < panel.children(); ++i)
            if (dynamic_cast<const ui::IconButton*>(panel.child(i)) != nullptr)
                buttons.push_back(panel.child(i));
        return buttons;
    };
    const std::vector<const Fl_Widget*> buttons = findButtons();
    REQUIRE(buttons.size() == 3);
    const Fl_Widget* add = buttons[0];
    const Fl_Widget* group = buttons[1];
    const Fl_Widget* del = buttons[2];

    CHECK(add->active() != 0);
    CHECK(group->active() != 0);
    CHECK(del->active() != 0);

    panel.toggleLocked(id);
    CHECK(add->active() != 0);       // Add makes a NEW layer; the lock does not touch it
    CHECK(group->active() == 0);
    CHECK(del->active() == 0);

    panel.toggleLocked(id);
    CHECK(group->active() != 0);
    CHECK(del->active() != 0);
}

// With no document (the empty state) there is nothing to add to, group, or delete.
TEST_CASE("the bottom strip greys out entirely without a document") {
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(nullptr);
    for (int i = 0; i < panel.children(); ++i)
        if (dynamic_cast<const ui::IconButton*>(panel.child(i)) != nullptr)
            CHECK(panel.child(i)->active() == 0);
}

TEST_CASE("toggleLocked is one undoable step per click") {
    core::Document doc(16, 16);
    const core::LayerId id = doc.root().addOnTop(doc.makeRaster("L")).id();
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    panel.setActive(id);

    panel.toggleLocked(id);
    REQUIRE(doc.find(id)->locked());
    doc.commands().undo();
    CHECK_FALSE(doc.find(id)->locked());
    doc.commands().redo();
    CHECK(doc.find(id)->locked());
}

// The editor is a child of the PANEL, not of the row list -- rebuildRows() clears the scroll, and an
// editor living there would be freed mid-commit. Renaming with nothing in flight must be inert.
TEST_CASE("commitRename and cancelRename are no-ops when nothing is being renamed") {
    core::Document doc(16, 16);
    const core::LayerId id = doc.root().addOnTop(doc.makeRaster("L")).id();
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    panel.setActive(id);
    const std::size_t before = doc.commands().size();

    CHECK_FALSE(panel.renaming());
    panel.commitRename();
    panel.cancelRename();
    CHECK_FALSE(panel.renaming());
    CHECK(doc.commands().size() == before);
    CHECK(doc.find(id)->name() == "L");
}

// ---- text / 3D thumbnails (user report, 2026-07-09) ---------------------------------------------
// A TextLayer keeps its pixels in a cache the RENDERER fills. On document open the panel builds its
// rows before the first composite, so the layer has no cache yet and its thumbnail is the blank
// placeholder. Filling the cache bumps no content revision, so the old cache key never noticed when
// the pixels finally arrived and the placeholder stuck for ever -- most visibly on 3D text.
namespace {
// A text layer with a synthetic 8x8 opaque-red "rendered" cache, as the renderer would leave it.
void giveTextLayerCache(const core::TextLayer& tl) {
    common::Image img(8, 8);
    for (std::size_t p = 0; p + 3 < img.rgba.size(); p += 4) {
        img.rgba[p + 0] = 220;
        img.rgba[p + 3] = 255;
    }
    tl.setCachedImage(std::move(img), common::Affine2D::identity());
}
} // namespace

TEST_CASE("layerThumbnail draws a Text layer from its renderer cache") {
    core::Document doc(64, 64);
    auto* tl = doc.root().addOnTop(doc.makeText("T", "hi")).as<core::TextLayer>();
    REQUIRE(tl != nullptr);

    const common::Image blank = ui::layerThumbnail(*tl, 32, 64, 64);
    CHECK(blank.rgba[0] == 60); // no cache yet: the neutral placeholder ground

    giveTextLayerCache(*tl);
    const common::Image drawn = ui::layerThumbnail(*tl, 32, 64, 64);
    const std::size_t c = ((16u * 32) + 16) * 4;
    CHECK(drawn.rgba[c] == 220); // framed on the cache, so the glyphs fill the box
    CHECK(drawn.rgba[c + 3] == 255);
}

TEST_CASE("a Text layer's thumbnail refreshes when its renderer cache arrives") {
    core::Document doc(64, 64);
    auto* tl = doc.root().addOnTop(doc.makeText("T", "hi")).as<core::TextLayer>();
    REQUIRE(tl != nullptr);

    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc); // rows built BEFORE any render, exactly as on document open
    panel.refreshThumbnails();

    // The renderer runs and fills the cache. Nothing bumped contentRevision or the transform.
    giveTextLayerCache(*tl);
    panel.refreshThumbnails();

    // The panel's cached thumbnail must now be the rendered one, not the placeholder.
    bool rebuilt = false;
    const common::Image& thumb = panel.cachedThumbnail(*tl, &rebuilt);
    CHECK_FALSE(rebuilt); // the refresh above already rebuilt it; this call is a cache hit
    const std::size_t c = ((17u * 34) + 17) * 4; // kThumb == 34
    CHECK(thumb.rgba[c] == 220);
}

// ---- Name truncation vs the badges + active-layer dot -----------------------------------------
//
// fl_draw() does not clip to the box it is handed, so a long layer name used to run straight over
// the type badge and the active-layer dot. The row now ellipsizes against the room actually left,
// recomputed each draw because the dock is width-resizable.

TEST_CASE("ellipsizeToWidth fits text to the current font, cutting on codepoint boundaries") {
    fl_font(FL_HELVETICA, 13);

    // Anything that already fits comes back untouched -- no ellipsis, no copy of a suffix.
    const std::string shortName = "Layer 1";
    const int fits = static_cast<int>(fl_width(shortName.c_str())) + 4;
    CHECK(ui::ellipsizeToWidth(shortName, fits) == shortName);
    CHECK(ui::ellipsizeToWidth("", 100).empty());

    // A name too long for its column loses characters and gains the ellipsis, and the RESULT fits.
    const std::string longName = "A very long layer name that will not fit in the dock";
    const int narrow = 80;
    const std::string cut = ui::ellipsizeToWidth(longName, narrow);
    CHECK(cut != longName);
    CHECK(cut.size() < longName.size());
    CHECK(fl_width(cut.c_str()) <= static_cast<double>(narrow));
    CHECK(cut.rfind("\xE2\x80\xA6") == cut.size() - 3); // ends with U+2026

    // Narrowing the column further only ever shortens the result (the dock drag must not grow it).
    const std::string narrower = ui::ellipsizeToWidth(longName, 40);
    CHECK(fl_width(narrower.c_str()) <= 40.0);
    CHECK(narrower.size() <= cut.size());

    // No room at all: nothing is drawn rather than a stray mark.
    CHECK(ui::ellipsizeToWidth(longName, 0).empty());
    CHECK(ui::ellipsizeToWidth(longName, -20).empty());
}

TEST_CASE("ellipsizeToWidth never splits a multi-byte UTF-8 codepoint") {
    fl_font(FL_HELVETICA, 13);
    // Each of these is 2 bytes; a naive byte-count cut lands mid-sequence and yields mojibake.
    const std::string accented = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
    for (int w = 4; w < 60; ++w) {
        const std::string out = ui::ellipsizeToWidth(accented, w);
        // Every byte that starts a sequence must be followed by exactly its continuation bytes:
        // walking the string codepoint-wise must consume it exactly.
        std::size_t i = 0;
        while (i < out.size()) {
            const auto lead = static_cast<unsigned char>(out[i]);
            const std::size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
            REQUIRE(i + len <= out.size()); // no truncated sequence at the end
            for (std::size_t k = 1; k < len; ++k)
                CHECK((static_cast<unsigned char>(out[i + k]) & 0xC0) == 0x80);
            i += len;
        }
    }
}

// ---- Layer masks (S31): the dock's second thumbnail + mask ops ----------------------------------

TEST_CASE("maskThumbnail renders coverage as grayscale, aspect-fit over a dark ground") {
    core::RasterMask mask(40, 20, 0); // 2:1 grid in a square box -> letterboxed
    for (std::uint32_t y = 0; y < 20; ++y)
        for (std::uint32_t x = 20; x < 40; ++x) mask.coverage[y * 40 + x] = 255;

    const common::Image thumb = ui::maskThumbnail(mask, 34);
    CHECK(thumb.width == 34);
    CHECK(thumb.height == 34);
    const auto at = [&](int x, int y) {
        return thumb.rgba[(static_cast<std::size_t>(y) * 34 + x) * 4];
    };
    CHECK(at(8, 17) == 0);     // left half of the sheet: hidden = black
    CHECK(at(26, 17) == 255);  // right half: revealed = white
    CHECK(at(17, 2) == 34);    // letterbox bar above the 2:1 sheet: the dark ground
    // Opaque throughout (the dock blits it with fl_draw_image).
    bool allOpaque = true;
    for (std::size_t i = 3; i < thumb.rgba.size(); i += 4)
        if (thumb.rgba[i] != 255)
            allOpaque = false;
    CHECK(allOpaque);
}

TEST_CASE("mask edit target: click-to-aim, cleared by a row switch or the mask's deletion") {
    core::Document doc(16, 16);
    const core::LayerId a = doc.root().addOnTop(doc.makeRaster("A")).id();
    const core::LayerId b = doc.root().addOnTop(doc.makeRaster("B")).id();

    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    panel.setActive(a);
    CHECK_FALSE(panel.maskEditTarget()); // no mask yet -> nothing to aim at

    panel.targetMask(a); // still maskless: the aim must refuse
    CHECK_FALSE(panel.maskEditTarget());

    panel.addMaskTo(a); // adds a reveal-all mask AND aims at it (paint lands on the fresh mask)
    REQUIRE(doc.find(a)->hasMask());
    CHECK(doc.find(a)->mask()->coverage == std::vector<std::uint8_t>(16 * 16, 255));
    CHECK(panel.maskEditTarget());

    panel.targetPixels(a); // clicking the pixel thumb re-aims
    CHECK_FALSE(panel.maskEditTarget());
    panel.targetMask(a);
    CHECK(panel.maskEditTarget());

    panel.setActive(b); // switching rows re-aims at pixels
    CHECK_FALSE(panel.maskEditTarget());

    panel.targetMask(a);
    CHECK(panel.maskEditTarget());
    panel.deleteMask(a); // the mask under the aim is gone
    CHECK_FALSE(doc.find(a)->hasMask());
    CHECK_FALSE(panel.maskEditTarget());
}

TEST_CASE("addMaskTo seeds from the active selection (the Photoshop button semantics)") {
    core::Document doc(8, 8);
    const core::LayerId id = doc.root().addOnTop(doc.makeRaster("A")).id();
    doc.setSelection(core::Selection::rectangle(8, 8, {0, 0, 4, 8})); // left half

    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    panel.addMaskTo(id);
    REQUIRE(doc.find(id)->hasMask());
    const core::RasterMask* m = doc.find(id)->mask();
    CHECK(m->coverage[3] == 255);              // inside the selection
    CHECK(m->coverage[5] == 0);                // outside
    CHECK(doc.commands().undoName() == "Add Mask");
    doc.commands().undo();
    CHECK_FALSE(doc.find(id)->hasMask());
}

TEST_CASE("toggleMaskEnabled / toggleMaskLinked push one undoable flag flip each") {
    core::Document doc(8, 8);
    const core::LayerId id = doc.root().addOnTop(doc.makeRaster("A")).id();
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    panel.addMaskTo(id);

    panel.toggleMaskEnabled(id);
    CHECK_FALSE(doc.find(id)->mask()->enabled);
    CHECK(doc.commands().undoName() == "Disable Mask");
    panel.toggleMaskEnabled(id);
    CHECK(doc.find(id)->mask()->enabled);
    CHECK(doc.commands().undoName() == "Enable Mask");

    panel.toggleMaskLinked(id);
    CHECK_FALSE(doc.find(id)->mask()->linked);
    CHECK(doc.commands().undoName() == "Unlink Mask");
    doc.commands().undo();
    CHECK(doc.find(id)->mask()->linked);
}

TEST_CASE("a locked layer refuses Add/Delete Mask but keeps the flag toggles live") {
    core::Document doc(8, 8);
    const core::LayerId id = doc.root().addOnTop(doc.makeRaster("A")).id();
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    panel.addMaskTo(id);
    panel.toggleLocked(id);
    const std::size_t stack = doc.commands().size();

    panel.deleteMask(id);
    CHECK(doc.find(id)->hasMask()); // refused, no no-op undo step
    CHECK(doc.commands().size() == stack);

    panel.toggleMaskEnabled(id); // visibility-like: stays live on a locked layer
    CHECK_FALSE(doc.find(id)->mask()->enabled);
    CHECK(doc.commands().size() == stack + 1);
}

TEST_CASE("shiftClickMaskThumbnail selects the mask's coverage") {
    core::Document doc(8, 8);
    const core::LayerId id = doc.root().addOnTop(doc.makeRaster("A")).id();
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    doc.setSelection(core::Selection::rectangle(8, 8, {0, 0, 2, 8}));
    panel.addMaskTo(id); // mask = the left two columns
    doc.setSelection(core::Selection{}); // drop the seed selection

    panel.shiftClickMaskThumbnail(id);
    CHECK(doc.selection().at(1, 4) == 255);
    CHECK(doc.selection().at(4, 4) == 0);
}

TEST_CASE("cachedMaskThumbnail re-renders exactly when the mask revision moves") {
    core::Document doc(8, 8);
    core::Layer& layer = doc.root().addOnTop(doc.makeRaster("A"));
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);

    bool rebuilt = false;
    const common::Image& none = panel.cachedMaskThumbnail(layer, &rebuilt);
    CHECK(none.empty()); // maskless: the empty answer
    CHECK_FALSE(rebuilt);

    layer.setMask(core::RasterMask(8, 8, 255));
    const common::Image& first = panel.cachedMaskThumbnail(layer, &rebuilt);
    CHECK_FALSE(first.empty());
    CHECK(rebuilt);
    (void)panel.cachedMaskThumbnail(layer, &rebuilt);
    CHECK_FALSE(rebuilt); // unchanged revision: served from the cache

    layer.mask()->coverage[0] = 0;
    layer.bumpMaskRevision(); // what the paint mirror / commands do after in-place edits
    (void)panel.cachedMaskThumbnail(layer, &rebuilt);
    CHECK(rebuilt);
}

// ---- Stale thumbnails (user report: "certain layer types get stale layer previews") -------------
//
// LayerRow::draw() blits an image the panel PUSHED into it; the row never consults the thumbnail
// cache. So a thumbnail is only ever as fresh as (a) the cache key noticing that its inputs moved
// and (b) somebody re-deriving it. Each case below pins one half of one of those two failures.
namespace {
// A group holding one child raster filled opaque white. The GROUP's thumbnail is a real composite
// of that child (render::compositeGroup), which is what makes it the witness for every "the child
// moved, the group's picture didn't" defect: nothing about the group itself is edited anywhere
// below, so a group thumbnail that refreshes can only have refreshed through its child.
struct GroupWithChild {
    core::GroupLayer* group = nullptr;
    core::RasterLayer* child = nullptr;
};

GroupWithChild groupWithOpaqueChild(core::Document& doc) {
    auto* group = doc.root().addOnTop(doc.makeGroup("G")).as<core::GroupLayer>();
    auto raster = doc.makeRaster("C", doc.width(), doc.height());
    raster->image().fill(common::Color8{255, 255, 255, 255}); // filled before any bounds query
    auto* child = group->addOnTop(std::move(raster)).as<core::RasterLayer>();
    return {group, child};
}

// A vector object whose geometry is an editable PATH (what the pen tool lands), and one whose
// geometry is a PARAMETRIC shape (what the shape tool lands). Same paint slot on both, so the badge
// tests can vary geometry and paint independently -- which is the whole precedence question.
core::vec::Object pathObject(core::vec::Paint fill) {
    core::vec::SubPath sub;
    sub.nodes.push_back(core::vec::Node{{0.0, 0.0}, {0.0, 0.0}, {6.0, 0.0}});
    sub.nodes.push_back(core::vec::Node{{10.0, 10.0}, {4.0, 10.0}, {10.0, 10.0}});
    core::vec::Object o;
    o.geometry = core::vec::Path{{sub}};
    o.fill = std::move(fill);
    return o;
}

core::vec::Object shapeObject(core::vec::Paint fill) {
    core::vec::Object o;
    o.geometry = core::vec::ParametricShape{core::vec::RectShape{{20.0, 20.0}}};
    o.fill = std::move(fill);
    return o;
}

core::vec::Paint solidRed() {
    return core::vec::SolidPaint{common::ColorF{1.0f, 0.0f, 0.0f, 1.0f}};
}

core::vec::Paint blackToWhiteRamp() {
    core::vec::Gradient g;
    g.stops = {core::vec::GradientStop{0.0, common::ColorF{0.0f, 0.0f, 0.0f, 1.0f}},
               core::vec::GradientStop{1.0, common::ColorF{1.0f, 1.0f, 1.0f, 1.0f}}};
    return g;
}
} // namespace

// Root cause B. Effects are not pixel CONTENT, so nothing bumps a contentRevision for them -- but
// the compositor renders every child THROUGH applyEffects, so a child's shadow/glow/overlay is
// visibly part of its group's thumbnail. Layer::effectsRevision() is what the key watches.
TEST_CASE("a child's layer effects re-derive its group's thumbnail") {
    core::Document doc(32, 32);
    const GroupWithChild g = groupWithOpaqueChild(doc);
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);

    bool rebuilt = false;
    const common::Image primed = panel.cachedThumbnail(*g.group, &rebuilt); // copy: kept to compare
    CHECK_FALSE(rebuilt); // setDocument's row build already rendered it

    // fillOpacity 0 drops the child's own pixels entirely (docs/layer-effects.md §1.9) -- the least
    // ambiguous difference an effect can make, and it counts as a live stack (empty() is false).
    core::LayerEffects fx;
    fx.fillOpacity = 0.0f;
    REQUIRE_FALSE(fx.empty());
    g.child->setEffects(fx);
    CHECK(g.child->contentRevision() == 0); // nothing about the CONTENT moved, which was the trap

    const common::Image& after = panel.cachedThumbnail(*g.group, &rebuilt);
    CHECK(rebuilt);
    CHECK(after.rgba != primed.rgba); // white group -> the checkerboard: really a different picture

    // Clearing them is the same event in reverse (the revision only ever advances).
    g.child->clearEffects();
    (void)panel.cachedThumbnail(*g.group, &rebuilt);
    CHECK(rebuilt);
}

// Root cause C. AdjustmentLayer has no contentRevision() of its own -- it inherits the base's
// constant 0 -- so a nested Levels/Curves was invisible to the group's subtree key: drag its
// sliders and the group's thumbnail froze for ever, since nothing about the group would ever drift
// again. The params bag is folded into the key instead of being counted by the layer, because
// params() hands out a mutable reference nobody can be trusted to announce.
TEST_CASE("a nested adjustment layer's params re-derive its group's thumbnail") {
    core::Document doc(32, 32);
    const GroupWithChild g = groupWithOpaqueChild(doc);
    auto* adj = g.group->addOnTop(doc.makeAdjustment("Levels", core::AdjustmentKind::Levels))
                    .as<core::AdjustmentLayer>();
    REQUIRE(adj != nullptr);
    CHECK(adj->contentRevision() == 0); // ... and it stays 0 for every edit below

    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    bool rebuilt = false;
    (void)panel.cachedThumbnail(*g.group, &rebuilt);
    CHECK_FALSE(rebuilt);

    adj->params()["out_white"] = 0.25; // a slider drag, straight into the bag
    (void)panel.cachedThumbnail(*g.group, &rebuilt);
    CHECK(rebuilt);

    adj->params()["out_white"] = 0.75; // a second drag: the VALUE alone must move the key
    (void)panel.cachedThumbnail(*g.group, &rebuilt);
    CHECK(rebuilt);

    adj->setAdjustmentKind(core::AdjustmentKind::Curves); // ... as must the kind
    (void)panel.cachedThumbnail(*g.group, &rebuilt);
    CHECK(rebuilt);
}

// Root cause A -- and note that the assertion is INVERTED from the ones above. Here the panel does
// the edit, so by the time the test asks for the thumbnail the panel must ALREADY have re-derived
// it: a cache HIT is the pass. A miss would mean the row is still holding (and re-blitting) the
// picture it had before the click, which is exactly what the user reported.
TEST_CASE("toggling a child's visibility leaves no stale thumbnail behind") {
    core::Document doc(32, 32);
    const GroupWithChild g = groupWithOpaqueChild(doc);
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    const common::Image primed = panel.cachedThumbnail(*g.group);

    panel.toggleVisible(g.child->id()); // the eye cell, on a row INSIDE the group
    REQUIRE_FALSE(g.child->visible());

    bool rebuilt = false;
    const common::Image& after = panel.cachedThumbnail(*g.group, &rebuilt);
    CHECK_FALSE(rebuilt);              // the panel already did the work
    CHECK(after.rgba != primed.rgba);  // and the work was real: the group lost its only content
}

TEST_CASE("the blend dropdown leaves no stale thumbnail behind") {
    core::Document doc(32, 32);
    const GroupWithChild g = groupWithOpaqueChild(doc);
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    panel.setActive(g.child->id()); // syncProperties pushes the child's mode (Normal) to the strip

    // Drift the model behind the panel's back so the dropdown (still Normal) and the layer really
    // disagree: the callback then CHANGES the mode instead of re-asserting the one it already has,
    // which is the only way to reach a genuine drift without the (private) slider widget.
    g.child->setBlendMode(core::BlendMode::Multiply);
    (void)panel.cachedThumbnail(*g.group);

    panel.onBlendChanged();
    REQUIRE(g.child->blendMode() == core::BlendMode::Normal); // the strip's value won

    bool rebuilt = false;
    (void)panel.cachedThumbnail(*g.group, &rebuilt);
    CHECK_FALSE(rebuilt);

    // Control: the key really is watching the child's blend mode. Flip it behind the panel's back
    // and the very next call rebuilds -- so the hit above was the panel working, not the key being
    // blind to blend modes in the first place.
    g.child->setBlendMode(core::BlendMode::Screen);
    (void)panel.cachedThumbnail(*g.group, &rebuilt);
    CHECK(rebuilt);
}

TEST_CASE("toggling a child's mask flags leaves no stale thumbnail behind") {
    core::Document doc(32, 32);
    const GroupWithChild g = groupWithOpaqueChild(doc);
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    panel.addMaskTo(g.child->id()); // reveal-all (no selection); rebuilds the rows as it goes
    REQUIRE(g.child->hasMask());
    (void)panel.cachedThumbnail(*g.group);

    bool rebuilt = false;
    panel.toggleMaskEnabled(g.child->id()); // the mask stops folding: a different composite
    REQUIRE_FALSE(g.child->mask()->enabled);
    (void)panel.cachedThumbnail(*g.group, &rebuilt);
    CHECK_FALSE(rebuilt);

    panel.toggleMaskLinked(g.child->id()); // linkage moves where the sheet folds, same story
    REQUIRE_FALSE(g.child->mask()->linked);
    (void)panel.cachedThumbnail(*g.group, &rebuilt);
    CHECK_FALSE(rebuilt);
}

// The opacity slider is the one path that deliberately does NOT re-derive per event: a drag fires
// its callback per tick, and re-running compositeGroup that often would stall the gesture. It
// settles instead, exactly as the host settles the text and adjustment previews (app_window's
// m_textThumbDirty / m_adjustThumbDirty timers).
TEST_CASE("the opacity slider re-derives on the gesture's release, not on every drag tick") {
    core::Document doc(32, 32);
    const GroupWithChild g = groupWithOpaqueChild(doc);
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    panel.setActive(g.child->id());
    (void)panel.cachedThumbnail(*g.group);

    // Something the group's thumbnail depends on drifts behind the panel's back, standing in for
    // the per-tick opacity commands themselves (the slider widget is private, so driving the
    // callback can only ever re-assert the value it already holds).
    g.child->setBlendMode(core::BlendMode::Multiply);

    const int liveEvent = Fl::e_number; // FLTK's current event -- the callback branches on it
    Fl::e_number = FL_DRAG;
    panel.onOpacityChanged(); // mid-drag: the canvas recomposites, the dock deliberately does not
    bool rebuilt = false;
    (void)panel.cachedThumbnail(*g.group, &rebuilt);
    CHECK(rebuilt); // still stale, on purpose

    g.child->setBlendMode(core::BlendMode::Screen); // drift again (the call above just re-derived)
    Fl::e_number = FL_RELEASE;
    panel.onOpacityChanged(); // the gesture settles: NOW the panel brings every row current
    (void)panel.cachedThumbnail(*g.group, &rebuilt);
    CHECK_FALSE(rebuilt);
    Fl::e_number = liveEvent;
}

// Root cause D. A crop / canvas resize moves no layer revision and no world transform, yet it
// re-frames everything derived at document resolution -- a group's composite, an adjustment's scope
// preview (whose aspect ratio comes straight off the doc rect), a vector layer's rasterization, and
// any layer with no contentBounds, which frames the doc rect as its fallback.
TEST_CASE("a canvas resize re-derives the thumbnails that are framed on the document rect") {
    core::Document doc(64, 64);
    core::Layer& shape = doc.root().addOnTop(doc.makeVector("V"));
    auto* vl = shape.as<core::VectorLayer>();
    REQUIRE(vl != nullptr);
    vl->setObject(shapeObject(solidRed()));
    vl->setTransform(common::Affine2D::translation(32.0, 32.0));

    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    bool rebuilt = false;
    (void)panel.cachedThumbnail(shape, &rebuilt);
    CHECK_FALSE(rebuilt);

    const std::uint64_t rev = vl->contentRevision();
    doc.setCanvasSize(128, 128);
    CHECK(vl->contentRevision() == rev); // the layer is untouched: the DOC size is the only signal
    (void)panel.cachedThumbnail(shape, &rebuilt);
    CHECK(rebuilt);

    // And it settles: a second query at the new size is a hit, not a permanent rebuild.
    (void)panel.cachedThumbnail(shape, &rebuilt);
    CHECK_FALSE(rebuilt);
}

// Root cause E. A text layer's pixels live in a cache the RENDERER fills, which bumps no revision
// anywhere in the document. The leaf key has always carried that cache; the SUBTREE key did not, so
// a group containing text kept the picture it had while the text was still a blank placeholder.
TEST_CASE("a group re-derives when a descendant text layer's renderer cache arrives or resizes") {
    core::Document doc(64, 64);
    auto* group = doc.root().addOnTop(doc.makeGroup("G")).as<core::GroupLayer>();
    REQUIRE(group != nullptr);
    auto* tl = group->addOnTop(doc.makeText("T", "hi")).as<core::TextLayer>();
    REQUIRE(tl != nullptr);

    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc); // rows built BEFORE any render, exactly as on document open
    bool rebuilt = false;
    (void)panel.cachedThumbnail(*group, &rebuilt);
    CHECK_FALSE(rebuilt);

    giveTextLayerCache(*tl); // the renderer runs: an 8x8 cache appears out of nowhere
    (void)panel.cachedThumbnail(*group, &rebuilt);
    CHECK(rebuilt);
    (void)panel.cachedThumbnail(*group, &rebuilt);
    REQUIRE_FALSE(rebuilt); // settled again

    // A re-render at a DIFFERENT SIZE is the other half of the signal. (The cache's ADDRESS is
    // stable inside the layer object, so it distinguishes nothing here -- only presence and
    // dimensions are real, which is why those are what the fold carries.)
    common::Image bigger(16, 16);
    for (std::size_t p = 0; p + 3 < bigger.rgba.size(); p += 4) {
        bigger.rgba[p + 0] = 220;
        bigger.rgba[p + 3] = 255;
    }
    tl->setCachedImage(std::move(bigger), common::Affine2D::identity());
    (void)panel.cachedThumbnail(*group, &rebuilt);
    CHECK(rebuilt);
}

// ---- The vector-PATH type badge (S28) ----------------------------------------------------------
//
// vec::Geometry is a variant and there is no separate path layer KIND by design, so a pen path, a
// star and a gradient are all one VectorLayer -- and until this badge, a path and a star wore the
// identical square+circle mark in the dock.

TEST_CASE("the type badge tells a pen path from a parametric shape and from a gradient layer") {
    core::Document doc(64, 64);
    const auto vectorWith = [&](core::vec::Object o) -> core::Layer& {
        core::Layer& layer = doc.root().addOnTop(doc.makeVector("V"));
        layer.as<core::VectorLayer>()->setObject(std::move(o));
        return layer;
    };

    CHECK(ui::typeBadgeFor(vectorWith(pathObject(solidRed()))) ==
          ui::LayerRow::TypeBadge::VectorPath);
    CHECK(ui::typeBadgeFor(vectorWith(shapeObject(solidRed()))) ==
          ui::LayerRow::TypeBadge::VectorShape);
    CHECK(ui::typeBadgeFor(vectorWith(shapeObject(blackToWhiteRamp()))) ==
          ui::LayerRow::TypeBadge::Gradient);

    // PRECEDENCE: geometry outranks paint. A path filled with a gradient is still a path -- the
    // ramp chip means "gradient layer", the full-bleed-rect idiom (docs/vector-model.md §1), not
    // "has a gradient in it somewhere"; and the fill is the half of the answer the row's own
    // thumbnail is already showing, where the geometry is not.
    CHECK(ui::typeBadgeFor(vectorWith(pathObject(blackToWhiteRamp()))) ==
          ui::LayerRow::TypeBadge::VectorPath);

    // Every other kind keeps the mark it had.
    CHECK(ui::typeBadgeFor(doc.root().addOnTop(doc.makeRaster("R"))) ==
          ui::LayerRow::TypeBadge::None);
    CHECK(ui::typeBadgeFor(doc.root().addOnTop(
              doc.makeAdjustment("A", core::AdjustmentKind::Invert))) ==
          ui::LayerRow::TypeBadge::Adjustment);
    CHECK(ui::typeBadgeFor(doc.root().addOnTop(doc.makeText("T", "hi"))) ==
          ui::LayerRow::TypeBadge::TextPoint);

    // A pre-existing gap, pinned so that closing it is a deliberate act: a vector layer with no
    // object at all shows no badge whatsoever, not even the generic shapes mark.
    CHECK(ui::typeBadgeFor(doc.root().addOnTop(doc.makeVector("empty"))) ==
          ui::LayerRow::TypeBadge::None);
}

TEST_CASE("typeBadgeWidth claims exactly the px each badge's draw branch consumes") {
    using Badge = ui::LayerRow::TypeBadge;
    // Transcribed from the branch chain at the bottom of LayerRow::draw(): each number is the
    // rightmost x that branch inks, relative to its gx, plus one. The layer NAME is ellipsized
    // against these, and fl_draw() does not clip -- so a badge that draws wider than it claims gets
    // a name run underneath it. Change the art, change both, and change this table.
    CHECK(ui::typeBadgeWidth(Badge::None, false) == 0);
    CHECK(ui::typeBadgeWidth(Badge::VectorShape, false) == 11); // fl_arc(gx+3, .., 8 wide)
    CHECK(ui::typeBadgeWidth(Badge::VectorPath, false) == 11);  // fl_rectf(gx+8, .., 3 wide)
    CHECK(ui::typeBadgeWidth(Badge::Gradient, false) == 11);    // the bw=11 framed ramp
    CHECK(ui::typeBadgeWidth(Badge::TextPoint, false) == 8);    // the 8-wide serif crossbar
    CHECK(ui::typeBadgeWidth(Badge::TextArea, false) == 14);    // ... + fl_rectf(gx+10, .., 4 wide)
    CHECK(ui::typeBadgeWidth(Badge::Magic, false) == 9);        // the page's right edge at gx+8
    CHECK(ui::typeBadgeWidth(Badge::Texture, false) == 20);     // kTexBadgeW, the framed chip
    CHECK(ui::typeBadgeWidth(Badge::Adjustment, false) == 10);  // the d=10 circle

    // The paste marker REPLACES the type badge -- they are one slot, not two -- so it answers for
    // every kind. (Which is also why a pasted vector layer shows no type mark at all.)
    CHECK(ui::typeBadgeWidth(Badge::None, true) == 9);
    CHECK(ui::typeBadgeWidth(Badge::VectorPath, true) == 9);
    CHECK(ui::typeBadgeWidth(Badge::Texture, true) == 9);
}

// ---- The row multi-selection grammar (user report, 2026-07-28) ----------------------------------
//
// The panel already MIRRORED the Move tool's multi-selection (setMoveSelection, highlighted rows,
// the gated blend/opacity strip) but had no way to EDIT it: every press on a row was a plain
// "make this the active layer". These pin the standard list vocabulary the rows now speak, and the
// fact that a set the panel builds itself is announced to the host exactly once.

namespace {
using RowClick = ui::LayerPanel::RowClick;

// Four flat top-level layers, bottom-to-top A, B, C, D -- so the DISPLAYED rows (top of the stack
// first) read D, C, B, A. Every range below is expressed in that displayed order, because that is
// the order a Shift sweep walks.
struct FourRows {
    core::LayerId a = core::kInvalidLayerId;
    core::LayerId b = core::kInvalidLayerId;
    core::LayerId c = core::kInvalidLayerId;
    core::LayerId d = core::kInvalidLayerId;
};

FourRows fourTopLevelLayers(core::Document& doc) {
    FourRows r;
    r.a = doc.root().addOnTop(doc.makeRaster("A")).id();
    r.b = doc.root().addOnTop(doc.makeRaster("B")).id();
    r.c = doc.root().addOnTop(doc.makeRaster("C")).id();
    r.d = doc.root().addOnTop(doc.makeRaster("D")).id();
    return r;
}
} // namespace

TEST_CASE("rowClickFor resolves the row modifiers: plain replaces, Ctrl toggles, Shift extends") {
    CHECK(ui::rowClickFor(/*shift=*/false, /*command=*/false) == RowClick::Replace);
    CHECK(ui::rowClickFor(/*shift=*/false, /*command=*/true) == RowClick::Toggle);
    CHECK(ui::rowClickFor(/*shift=*/true, /*command=*/false) == RowClick::Extend);
    // Both down: Shift wins. "Extend the toggled set" is a gesture no editor agrees on, and
    // extending is the half people actually reach for.
    CHECK(ui::rowClickFor(/*shift=*/true, /*command=*/true) == RowClick::Extend);
}

TEST_CASE("a plain row click replaces the panel's selection and becomes the anchor") {
    core::Document doc(16, 16);
    const FourRows r = fourTopLevelLayers(doc);
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);

    panel.selectRow(r.b, RowClick::Replace);
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.b}));
    CHECK(panel.activeLayer() == r.b);
    CHECK(panel.selectionAnchor() == r.b);
    CHECK_FALSE(panel.multiSelectActive()); // one row is a selection of one, not a multi-selection

    // A plain click on top of a set COLLAPSES it -- that is the whole of "replaces", and it is what
    // makes the modifier grammar reversible without a menu.
    panel.setMoveSelection({r.a, r.b, r.c});
    REQUIRE(panel.multiSelectActive());
    panel.selectRow(r.d, RowClick::Replace);
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.d}));
    CHECK(panel.activeLayer() == r.d);
    CHECK(panel.selectionAnchor() == r.d);
}

TEST_CASE("Ctrl-click toggles a row in and out of the selection, and moves the anchor either way") {
    core::Document doc(16, 16);
    const FourRows r = fourTopLevelLayers(doc);
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);

    panel.selectRow(r.b, RowClick::Replace);
    panel.selectRow(r.d, RowClick::Toggle); // absent -> appended, so it is the new primary
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.b, r.d}));
    CHECK(panel.multiSelectActive());
    CHECK(panel.activeLayer() == r.d);
    CHECK(panel.selectionAnchor() == r.d);

    panel.selectRow(r.a, RowClick::Toggle);
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.b, r.d, r.a}));

    // Present -> removed. The ACTIVE slot then follows what is left on top of the set, exactly as
    // VulkanCanvas::toggleMoveTarget hands it to m_moveTargets.back(); the two surfaces agree.
    panel.selectRow(r.d, RowClick::Toggle);
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.b, r.a}));
    CHECK(panel.activeLayer() == r.a);
    CHECK(panel.selectionAnchor() == r.d); // the toggled row anchors whether it joined or left

    // Emptying the set leaves the clicked row ACTIVE but unselected: the single active layer is a
    // separate, always-present thing, not the one-element case of the selection.
    panel.selectRow(r.b, RowClick::Toggle);
    panel.selectRow(r.a, RowClick::Toggle);
    CHECK(panel.moveSelection().empty());
    CHECK(panel.activeLayer() == r.a);
}

TEST_CASE("Shift-click extends the range from the anchor, and the anchor does not move") {
    core::Document doc(16, 16);
    const FourRows r = fourTopLevelLayers(doc);
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);

    // Displayed rows, top of the stack first: D, C, B, A.
    panel.selectRow(r.b, RowClick::Replace); // anchor = B (displayed index 2)
    panel.selectRow(r.d, RowClick::Extend);  // sweep 2 -> 0, so the CLICKED row lands last
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.b, r.c, r.d}));
    CHECK(panel.activeLayer() == r.d);
    CHECK(panel.selectionAnchor() == r.b);

    // A SECOND Shift-click re-sweeps from the SAME anchor rather than growing the last range --
    // the behaviour every file manager and every layer stack has.
    panel.selectRow(r.c, RowClick::Extend);
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.b, r.c}));
    CHECK(panel.selectionAnchor() == r.b);

    // Sweeping the other way past the anchor is the same walk in the other direction.
    panel.selectRow(r.a, RowClick::Extend);
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.b, r.a}));
    CHECK(panel.activeLayer() == r.a);

    // Shift-clicking the anchor itself is a one-row range, never an empty one.
    panel.selectRow(r.b, RowClick::Extend);
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.b}));
    CHECK_FALSE(panel.multiSelectActive());
}

TEST_CASE("a Shift-extend with no anchor yet sweeps from the active row") {
    core::Document doc(16, 16);
    const FourRows r = fourTopLevelLayers(doc);
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc); // selects the top of the stack (D) and clears the anchor
    REQUIRE(panel.activeLayer() == r.d);
    REQUIRE(panel.selectionAnchor() == core::kInvalidLayerId);

    panel.selectRow(r.b, RowClick::Extend); // D (index 0) -> B (index 2)
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.d, r.c, r.b}));
}

TEST_CASE("rowPressed reads the live modifiers (Fl::e_state) into the row grammar") {
    core::Document doc(16, 16);
    const FourRows r = fourTopLevelLayers(doc);
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);

    const int live = Fl::e_state;
    Fl::e_state = 0;
    panel.rowPressed(r.b);
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.b}));

    Fl::e_state = FL_COMMAND; // Ctrl on X11/Windows, Cmd on macOS (fl_command_modifier())
    panel.rowPressed(r.d);
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.b, r.d}));

    Fl::e_state = FL_SHIFT; // anchor is D (index 0) -> sweep down to A (index 3)
    panel.rowPressed(r.a);
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.d, r.c, r.b, r.a}));

    Fl::e_state = FL_SHIFT | FL_COMMAND; // Shift wins: still a sweep from D
    panel.rowPressed(r.c);
    CHECK((panel.moveSelection() == std::vector<core::LayerId>{r.d, r.c}));

    Fl::e_state = live;
}

TEST_CASE("the panel announces the sets IT builds, and never echoes the host's own push back") {
    core::Document doc(16, 16);
    const FourRows r = fourTopLevelLayers(doc);
    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);

    std::vector<std::vector<core::LayerId>> pushed;
    panel.setOnSelectionChanged(
        [&](const std::vector<core::LayerId>& sel) { pushed.push_back(sel); });

    panel.selectRow(r.b, RowClick::Replace);
    REQUIRE(pushed.size() == 1);
    CHECK((pushed.back() == std::vector<core::LayerId>{r.b}));

    panel.selectRow(r.b, RowClick::Replace); // the same pick again is not a change
    CHECK(pushed.size() == 1);

    panel.selectRow(r.d, RowClick::Toggle);
    REQUIRE(pushed.size() == 2);
    CHECK((pushed.back() == std::vector<core::LayerId>{r.b, r.d}));

    // The host pushing the CANVAS's set IN must not come straight back out: that is the loop the
    // callback's contract exists to forbid.
    panel.setMoveSelection({r.a, r.c});
    CHECK(pushed.size() == 2);
}

// ---- "Shift-click the thumbnail selects the layer's pixels", for EVERY kind ---------------------
//
// The gesture worked for Raster/Magic (core::selectionFromLayerPixels) and, since the group
// thumbnail was built, for a Group -- and silently did nothing for text, vector, texture and
// adjustment layers, even though the compositor already produces a picture for each of those rows.
// Each kind now routes through that same picture: no second rasterizer, and no kind left out.

namespace {
// An 8x8 fully opaque block -- the smallest thing that reads unambiguously as "these pixels".
common::Image opaqueBlock(std::uint32_t side) {
    common::Image img(side, side);
    img.fill(common::Color8{220, 40, 40, 255});
    return img;
}

// Every panel gesture below reads the live modifier set for its boolean op (thumbnailSelectOp), so
// a stray FL_CTRL left over from another case would silently turn Replace into Add.
struct ClearedModifiers {
    int saved = Fl::e_state;
    ClearedModifiers() { Fl::e_state = 0; }
    ~ClearedModifiers() { Fl::e_state = saved; }
};
} // namespace

TEST_CASE("layerPixelCoverage answers for every layer kind, through the compositor's own picture") {
    ClearedModifiers mods;

    SUBCASE("raster: its own alpha, as it always was") {
        core::Document doc(32, 32);
        auto raster = doc.makeRaster("R", 8, 8);
        raster->image().fill(common::Color8{220, 40, 40, 255});
        const core::LayerId id = doc.root().addOnTop(std::move(raster)).id();
        ui::LayerPanel panel(0, 0, 280, 600);
        panel.setDocument(&doc);

        const std::optional<core::Selection> sel = panel.layerPixelCoverage(id);
        REQUIRE(sel.has_value());
        CHECK(sel->at(4, 4) == 255);
        CHECK(sel->at(20, 20) == 0);
        CHECK(panel.layerHasSelectablePixels(id));
    }

    SUBCASE("magic: its preserved source, like a raster") {
        core::Document doc(32, 32);
        const core::LayerId id = doc.root().addOnTop(doc.makeMagic("M", opaqueBlock(8))).id();
        ui::LayerPanel panel(0, 0, 280, 600);
        panel.setDocument(&doc);

        const std::optional<core::Selection> sel = panel.layerPixelCoverage(id);
        REQUIRE(sel.has_value());
        CHECK(sel->at(4, 4) == 255);
        CHECK(sel->at(20, 20) == 0);
    }

    SUBCASE("group: the composited subtree (the arm the others were modelled on)") {
        core::Document doc(32, 32);
        const GroupWithChild g = groupWithOpaqueChild(doc); // a doc-sized opaque child
        ui::LayerPanel panel(0, 0, 280, 600);
        panel.setDocument(&doc);

        const std::optional<core::Selection> sel = panel.layerPixelCoverage(g.group->id());
        REQUIRE(sel.has_value());
        CHECK(sel->at(4, 4) == 255);
        CHECK(sel->at(28, 28) == 255);
        CHECK(panel.layerHasSelectablePixels(g.group->id()));
    }

    SUBCASE("vector: the rasterized shape coverage, not the whole layer box") {
        core::Document doc(32, 32);
        auto vec = doc.makeVector("V");
        vec->setObject(shapeObject(solidRed())); // a 20x20 rect, centred on the local origin
        vec->setTransform(common::Affine2D::translation(16.0, 16.0)); // -> doc [6,26]^2
        const core::LayerId id = doc.root().addOnTop(std::move(vec)).id();
        ui::LayerPanel panel(0, 0, 280, 600);
        panel.setDocument(&doc);

        const std::optional<core::Selection> sel = panel.layerPixelCoverage(id);
        REQUIRE(sel.has_value());
        CHECK(sel->at(16, 16) == 255); // inside the shape
        CHECK(sel->at(2, 2) == 0);     // outside it, but well inside the layer's canvas window
        CHECK(panel.layerHasSelectablePixels(id));
    }

    SUBCASE("text: the renderer's pixel cache") {
        core::Document doc(32, 32);
        auto* tl = doc.root().addOnTop(doc.makeText("T", "hi")).as<core::TextLayer>();
        REQUIRE(tl != nullptr);
        ui::LayerPanel panel(0, 0, 280, 600);
        panel.setDocument(&doc);

        // Unrendered, the layer genuinely has no glyphs yet -- and says so.
        CHECK_FALSE(panel.layerHasSelectablePixels(tl->id()));
        giveTextLayerCache(*tl); // an 8x8 opaque cache, placed 1:1 in layer space
        CHECK(panel.layerHasSelectablePixels(tl->id()));

        const std::optional<core::Selection> sel = panel.layerPixelCoverage(tl->id());
        REQUIRE(sel.has_value());
        CHECK(sel->at(4, 4) == 255);
        CHECK(sel->at(20, 20) == 0);
    }

    SUBCASE("texture: the generator's pixel cache") {
        core::Document doc(32, 32);
        auto* xl = doc.root()
                       .addOnTop(doc.makeTexture(
                           "X", core::texture::defaultTextureParams(core::texture::Generator::Paper)))
                       .as<core::TextureLayer>();
        REQUIRE(xl != nullptr);
        ui::LayerPanel panel(0, 0, 280, 600);
        panel.setDocument(&doc);
        CHECK_FALSE(panel.layerHasSelectablePixels(xl->id())); // no cache rendered yet

        xl->setCachedImage(opaqueBlock(8), std::nullopt, common::Affine2D::identity());
        CHECK(panel.layerHasSelectablePixels(xl->id()));

        const std::optional<core::Selection> sel = panel.layerPixelCoverage(xl->id());
        REQUIRE(sel.has_value());
        CHECK(sel->at(4, 4) == 255);
        CHECK(sel->at(20, 20) == 0);
    }

    SUBCASE("adjustment: the scope it grades -- the same picture its own row shows") {
        core::Document doc(32, 32);
        auto raster = doc.makeRaster("R", 8, 8); // the only thing beneath the adjustment
        raster->image().fill(common::Color8{220, 40, 40, 255});
        doc.root().addOnTop(std::move(raster));
        const core::LayerId adj =
            doc.root().addOnTop(doc.makeAdjustment("Levels", core::AdjustmentKind::Invert)).id();
        ui::LayerPanel panel(0, 0, 280, 600);
        panel.setDocument(&doc);

        const std::optional<core::Selection> sel = panel.layerPixelCoverage(adj);
        REQUIRE(sel.has_value());
        CHECK(sel->at(4, 4) == 255);   // the backdrop it acts on
        CHECK(sel->at(20, 20) == 0);   // where it acts on nothing
    }
}

TEST_CASE("shift-clicking a thumbnail lands one selection command for the non-raster kinds too") {
    ClearedModifiers mods;
    core::Document doc(32, 32);
    auto vec = doc.makeVector("V");
    vec->setObject(shapeObject(solidRed()));
    vec->setTransform(common::Affine2D::translation(16.0, 16.0));
    const core::LayerId id = doc.root().addOnTop(std::move(vec)).id();

    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    const std::size_t before = doc.commands().size();

    panel.shiftClickThumbnail(id);
    CHECK(doc.commands().size() == before + 1); // exactly one undoable step
    CHECK(panel.activeLayer() == id);           // ... and the gesture selected the row it acted on
    CHECK(doc.selection().at(16, 16) == 255);
    CHECK(doc.selection().at(2, 2) == 0);

    doc.commands().undo();
    CHECK(doc.selection().isEmpty());
}

TEST_CASE("an empty layer refuses the pixel selection OUT LOUD instead of clearing it") {
    ClearedModifiers mods;
    core::Document doc(32, 32);
    // Three ways to have nothing: a vector with no object, text nobody has rendered, and an
    // adjustment with no backdrop beneath it.
    const core::LayerId emptyVector = doc.root().addOnTop(doc.makeVector("V")).id();
    const core::LayerId emptyText = doc.root().addOnTop(doc.makeText("T", "hi")).id();
    const core::LayerId emptyAdjust =
        doc.root().addOnTop(doc.makeAdjustment("Levels", core::AdjustmentKind::Invert)).id();

    ui::LayerPanel panel(0, 0, 280, 600);
    panel.setDocument(&doc);
    std::vector<std::string> said;
    panel.setOnStatus([&](std::string m) { said.push_back(std::move(m)); });

    // A pre-existing selection is the thing at risk: a "successful" empty result would silently
    // wipe it, which is exactly the class of quiet failure this refusal exists to prevent.
    doc.setSelection(core::Selection::rectangle(32, 32, {0, 0, 8, 8}));
    const std::size_t before = doc.commands().size();

    for (const core::LayerId id : {emptyVector, emptyText, emptyAdjust}) {
        const std::optional<core::Selection> sel = panel.layerPixelCoverage(id);
        REQUIRE(sel.has_value());       // the kind is wired; it simply covers nothing
        CHECK_FALSE(sel->anySelected());
        panel.shiftClickThumbnail(id);
    }
    CHECK(said.size() == 3);                        // every refusal was narrated
    CHECK(doc.commands().size() == before);         // no no-op undo step
    CHECK(doc.selection().at(4, 4) == 255);         // and the standing selection survived intact

    // The cheap affordance oracle agrees with two of the three for free (the adjustment's scope is
    // a whole composite away, so its row promises and the click above is what answers).
    CHECK_FALSE(panel.layerHasSelectablePixels(emptyVector));
    CHECK_FALSE(panel.layerHasSelectablePixels(emptyText));
    CHECK(panel.layerHasSelectablePixels(emptyAdjust));
}

// The two families are deliberately asymmetric about the layer MASK, and the asymmetry predates
// this work: Raster/Magic keep the S13 contract (their own alpha; clicking the MASK thumbnail is
// the separate S31 gesture), while every kind routed through render::rasterizeLayer gets the
// rasterizer's contract -- transform, mask and effects baked -- exactly as the group arm always
// did through render::compositeGroup. Pinned so that changing it is a decision, not a drift.
TEST_CASE("a raster's pixel selection ignores its mask; a baked kind's honours its effects") {
    ClearedModifiers mods;

    SUBCASE("raster: the mask is the OTHER thumbnail's gesture, so it is not folded here") {
        core::Document doc(16, 16);
        auto raster = doc.makeRaster("R", 16, 16);
        raster->image().fill(common::Color8{220, 40, 40, 255});
        const core::LayerId id = doc.root().addOnTop(std::move(raster)).id();

        ui::LayerPanel panel(0, 0, 280, 600);
        panel.setDocument(&doc);
        doc.setSelection(core::Selection::rectangle(16, 16, {0, 0, 8, 16})); // left half
        panel.addMaskTo(id); // ... seeds a mask that hides the right half
        doc.setSelection(core::Selection{});

        const std::optional<core::Selection> sel = panel.layerPixelCoverage(id);
        REQUIRE(sel.has_value());
        CHECK(sel->at(4, 8) == 255);
        CHECK(sel->at(12, 8) == 255); // masked away on the canvas, still selectable here
    }

    SUBCASE("vector: effects are baked, so a zero fill-opacity really does leave nothing") {
        core::Document doc(32, 32);
        auto vec = doc.makeVector("V");
        vec->setObject(shapeObject(solidRed()));
        vec->setTransform(common::Affine2D::translation(16.0, 16.0));
        core::Layer& layer = doc.root().addOnTop(std::move(vec));

        ui::LayerPanel panel(0, 0, 280, 600);
        panel.setDocument(&doc);
        const std::optional<core::Selection> primed = panel.layerPixelCoverage(layer.id());
        REQUIRE(primed.has_value());
        REQUIRE(primed->anySelected());

        // fillOpacity 0 drops the layer's own pixels entirely (docs/layer-effects.md §1.9) -- the
        // least ambiguous difference an effect can make, and it counts as a live stack.
        core::LayerEffects fx;
        fx.fillOpacity = 0.0f;
        REQUIRE_FALSE(fx.empty());
        layer.setEffects(fx);

        const std::optional<core::Selection> sel = panel.layerPixelCoverage(layer.id());
        REQUIRE(sel.has_value());
        CHECK_FALSE(sel->anySelected());
    }
}
