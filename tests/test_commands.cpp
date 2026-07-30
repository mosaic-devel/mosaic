#include "core/command.hpp"
#include "core/commands.hpp"

#include <doctest/doctest.h>

#include <memory>

#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/object.hpp"

using namespace mosaic;
using core::AddLayerCommand;
using core::BlendMode;
using core::CompositeCommand;
using core::Document;
using core::LayerId;
using core::MoveLayerCommand;
using core::RemoveLayerCommand;
using core::SetBlendModeCommand;
using core::SetLayerPixelsCommand;
using core::SetNameCommand;
using core::SetOpacityCommand;
using core::SetTransformCommand;
using core::SetVisibleCommand;

namespace {
template <class T, class... Args>
void push(Document& doc, Args&&... args) {
    doc.commands().push(std::make_unique<T>(std::forward<Args>(args)...));
}
}  // namespace

TEST_CASE("AddLayerCommand adds, undoes and redoes, restoring identity") {
    Document doc(16, 16);
    auto layer = doc.makeRaster("A");
    const LayerId id = layer->id();

    doc.commands().push(std::make_unique<AddLayerCommand>(doc.root().id(), 0, std::move(layer)));
    CHECK(doc.find(id) != nullptr);
    CHECK(doc.root().childCount() == 1);
    CHECK(doc.dirty());

    doc.commands().undo();
    CHECK(doc.find(id) == nullptr);
    CHECK(doc.root().empty());

    doc.commands().redo();
    CHECK(doc.find(id) != nullptr);  // same layer object/id comes back
    CHECK(doc.root().child(0).id() == id);
}

TEST_CASE("RemoveLayerCommand restores the layer at its original place") {
    Document doc(16, 16);
    doc.root().addOnTop(doc.makeRaster("A"));
    const LayerId b = doc.root().addOnTop(doc.makeRaster("B")).id();
    doc.root().addOnTop(doc.makeRaster("C"));
    REQUIRE(doc.root().childCount() == 3);  // [A, B, C] bottom->top, B at index 1

    push<RemoveLayerCommand>(doc, b);
    CHECK(doc.find(b) == nullptr);
    CHECK(doc.root().childCount() == 2);

    doc.commands().undo();
    REQUIRE(doc.root().childCount() == 3);
    CHECK(doc.root().child(1).id() == b);  // back at index 1
}

TEST_CASE("MoveLayerCommand reorders within a parent") {
    Document doc(16, 16);
    const LayerId a = doc.root().addOnTop(doc.makeRaster("A")).id();
    doc.root().addOnTop(doc.makeRaster("B"));
    doc.root().addOnTop(doc.makeRaster("C"));  // [A, B, C]

    // Move A to the top. After removing A the list is [B, C]; index 2 appends -> [B, C, A].
    push<MoveLayerCommand>(doc, a, doc.root().id(), 2);
    CHECK(doc.root().child(2).id() == a);
    CHECK(doc.root().child(0).name() == "B");

    doc.commands().undo();
    CHECK(doc.root().child(0).id() == a);  // back to the bottom
    CHECK(doc.root().child(0).name() == "A");
}

TEST_CASE("MoveLayerCommand reparents into a group and back") {
    Document doc(16, 16);
    auto* group = doc.root().addOnTop(doc.makeGroup("G")).as<core::GroupLayer>();
    REQUIRE(group != nullptr);
    const LayerId g = group->id();
    const LayerId x = doc.root().addOnTop(doc.makeRaster("X")).id();  // root: [G, X]

    push<MoveLayerCommand>(doc, x, g, 0);  // move X into G
    CHECK(doc.root().childCount() == 1);
    CHECK(doc.groupById(g)->childCount() == 1);
    CHECK(doc.locate(x)->parent->id() == g);

    doc.commands().undo();
    CHECK(doc.root().childCount() == 2);
    CHECK(doc.groupById(g)->empty());
    CHECK(doc.root().child(1).id() == x);  // restored to its original index
}

TEST_CASE("scalar property commands apply and reverse") {
    Document doc(16, 16);
    core::Layer& l = doc.root().addOnTop(doc.makeRaster("L"));
    const LayerId id = l.id();

    push<SetNameCommand>(doc, id, std::string("renamed"));
    CHECK(l.name() == "renamed");
    doc.commands().undo();
    CHECK(l.name() == "L");

    push<SetVisibleCommand>(doc, id, false);
    CHECK_FALSE(l.visible());
    doc.commands().undo();
    CHECK(l.visible());

    push<SetBlendModeCommand>(doc, id, BlendMode::Screen);
    CHECK(l.blendMode() == BlendMode::Screen);
    doc.commands().undo();
    CHECK(l.blendMode() == BlendMode::Normal);
}

TEST_CASE("SetLockedCommand applies, reverses and names itself for the History panel") {
    Document doc(16, 16);
    core::Layer& l = doc.root().addOnTop(doc.makeRaster("L"));
    const LayerId id = l.id();
    REQUIRE_FALSE(l.locked());

    push<core::SetLockedCommand>(doc, id, true);
    CHECK(l.locked());
    CHECK(doc.commands().nameAt(doc.commands().size() - 1) == "Lock Layer");
    doc.commands().undo();
    CHECK_FALSE(l.locked());

    doc.commands().redo();
    CHECK(l.locked());
    push<core::SetLockedCommand>(doc, id, false); // the label follows the direction, not the field
    CHECK_FALSE(l.locked());
    CHECK(doc.commands().nameAt(doc.commands().size() - 1) == "Unlock Layer");
    doc.commands().undo();
    CHECK(l.locked());
}

// A Text layer's row caption tracks its content until it is named by hand. Renaming IS that moment:
// without this, the panel would keep showing the first line of text and the rename would look like
// it silently failed.
TEST_CASE("SetNameCommand stops a Text layer auto-naming, and undo restores it") {
    Document doc(16, 16);
    auto text = doc.makeText("Text");
    const LayerId id = text->id();
    core::Layer& l = doc.root().addOnTop(std::move(text));
    auto* tl = l.as<core::TextLayer>();
    REQUIRE(tl != nullptr);
    REQUIRE(tl->autoNamed()); // fresh Text layers follow their content

    push<SetNameCommand>(doc, id, std::string("Title"));
    CHECK(l.name() == "Title");
    CHECK_FALSE(tl->autoNamed());

    doc.commands().undo();
    CHECK(l.name() == "Text");
    CHECK(tl->autoNamed());
}

// Renaming a pasted layer "adopts" it (the badge clears). That behaviour predates the rename UI but
// is what the inline editor now leans on to refresh the row in place, so pin it.
TEST_CASE("SetNameCommand clears the pasted marker, and undo restores it") {
    Document doc(16, 16);
    core::Layer& l = doc.root().addOnTop(doc.makeRaster("Pasted image"));
    l.setPastedMarker(true);
    const LayerId id = l.id();

    push<SetNameCommand>(doc, id, std::string("Sky"));
    CHECK_FALSE(l.pastedMarker());
    doc.commands().undo();
    CHECK(l.pastedMarker());
}

TEST_CASE("SetOpacityCommand coalesces a gesture into one undo step") {
    Document doc(16, 16);
    core::Layer& l = doc.root().addOnTop(doc.makeRaster("L"));
    const LayerId id = l.id();  // opacity starts at 1.0

    push<SetOpacityCommand>(doc, id, 0.8f, /*coalesceId=*/7);
    push<SetOpacityCommand>(doc, id, 0.5f, 7);
    push<SetOpacityCommand>(doc, id, 0.3f, 7);

    CHECK(l.opacity() == doctest::Approx(0.3f));
    CHECK(doc.commands().undoCount() == 1);  // merged

    doc.commands().undo();
    CHECK(l.opacity() == doctest::Approx(1.0f));  // reverts to the pre-gesture value
}

TEST_CASE("opacity edits in different/zero gestures do not coalesce") {
    Document doc(16, 16);
    const LayerId id = doc.root().addOnTop(doc.makeRaster("L")).id();

    push<SetOpacityCommand>(doc, id, 0.9f, 0);  // 0 = never coalesce
    push<SetOpacityCommand>(doc, id, 0.8f, 0);
    CHECK(doc.commands().undoCount() == 2);

    doc.commands().clear();
    push<SetOpacityCommand>(doc, id, 0.7f, 1);
    push<SetOpacityCommand>(doc, id, 0.6f, 2);  // different gesture id
    CHECK(doc.commands().undoCount() == 2);
}

TEST_CASE("SetTransformCommand coalesces too") {
    Document doc(16, 16);
    core::Layer& l = doc.root().addOnTop(doc.makeRaster("L"));
    const LayerId id = l.id();

    push<SetTransformCommand>(doc, id, common::Affine2D::translation(5, 0), 3);
    push<SetTransformCommand>(doc, id, common::Affine2D::translation(20, 0), 3);
    CHECK(doc.commands().undoCount() == 1);
    CHECK(l.transform().apply({0, 0}).x == doctest::Approx(20.0));

    doc.commands().undo();
    CHECK(l.transform().apply({0, 0}).x == doctest::Approx(0.0));  // identity restored
}

TEST_CASE("SetTextCommand coalesces a typing burst into one undo step") {
    Document doc(16, 16);
    core::Layer& l = doc.root().addOnTop(doc.makeText("T", "hi"));
    const LayerId id = l.id();
    auto* tl = l.as<core::TextLayer>();
    REQUIRE(tl != nullptr);

    // One keystroke = insert one char at the end of the CURRENT block (as the canvas does).
    auto typed = [&](const std::string& ch) {
        core::text::TextBlock b = tl->block();
        core::text::replaceText(b, b.utf8.size(), b.utf8.size(), ch);
        return b;
    };
    // Three keystrokes in one burst (same non-zero coalesce id) collapse to one step.
    push<core::SetTextCommand>(doc, id, typed("a"), std::string("Type"), 4);
    push<core::SetTextCommand>(doc, id, typed("b"), std::string("Type"), 4);
    push<core::SetTextCommand>(doc, id, typed("c"), std::string("Type"), 4);
    CHECK(tl->block().utf8 == "hiabc");
    CHECK(doc.commands().undoCount() == 1);

    doc.commands().undo();
    CHECK(tl->block().utf8 == "hi");  // reverts to the pre-burst text

    // A fresh gesture id is its own step.
    doc.commands().clear();
    push<core::SetTextCommand>(doc, id, typed("x"), std::string("Type"), 8);
    push<core::SetTextCommand>(doc, id, typed("y"), std::string("Type"), 9);
    CHECK(doc.commands().undoCount() == 2);
}

TEST_CASE("SetVectorObjectCommand coalesces a live edit into one undo step") {
    namespace vec = core::vec;
    Document doc(16, 16);
    auto vl = doc.makeVector("Shape");
    const LayerId id = vl->id();
    vec::Object base;  // a 10x10 rect, sharp
    base.geometry = vec::ParametricShape{vec::RectShape::uniform({10, 10}, 0)};
    vl->setObject(base);
    doc.root().addOnTop(std::move(vl));

    const auto withRadius = [&](double r) {
        vec::Object o = base;
        std::get<vec::RectShape>(std::get<vec::ParametricShape>(o.geometry)).cornerRadius = {r, r, r, r};
        return o;
    };
    // Two edits in one gesture (same id + non-zero coalesce id) collapse to a single step.
    push<core::SetVectorObjectCommand>(doc, id, withRadius(2.0), std::string("Edit Shape"), 5);
    push<core::SetVectorObjectCommand>(doc, id, withRadius(4.0), std::string("Edit Shape"), 5);
    CHECK(doc.commands().undoCount() == 1);

    const auto* vlp = doc.find(id)->as<core::VectorLayer>();
    REQUIRE(vlp != nullptr);
    CHECK(std::get<vec::RectShape>(std::get<vec::ParametricShape>(vlp->object()->geometry))
              .cornerRadius[0] == doctest::Approx(4.0));

    doc.commands().undo();  // one undo restores the pre-edit (sharp) object
    CHECK(std::get<vec::RectShape>(std::get<vec::ParametricShape>(vlp->object()->geometry))
              .cornerRadius[0] == doctest::Approx(0.0));

    // A different coalesce id (or 0) is a fresh step, not a continuation.
    push<core::SetVectorObjectCommand>(doc, id, withRadius(1.0), std::string("Edit Shape"), 6);
    push<core::SetVectorObjectCommand>(doc, id, withRadius(3.0), std::string("Edit Shape"), 7);
    CHECK(doc.commands().undoCount() == 2);
}

TEST_CASE("SetVectorObjectCommand sets object + transform atomically (the resize re-anchor)") {
    namespace vec = core::vec;
    Document doc(32, 32);
    auto vl = doc.makeVector("Shape");
    const LayerId id = vl->id();
    vec::Object base;  // a 10x10 rect placed at the origin
    base.geometry = vec::ParametricShape{vec::RectShape::uniform({10, 10}, 0)};
    vl->setObject(base);
    vl->setTransform(common::Affine2D::translation(5, 5));
    doc.root().addOnTop(std::move(vl));

    vec::Object bigger = base;  // a parametric resize: bigger object + a re-anchoring placement
    std::get<vec::RectShape>(std::get<vec::ParametricShape>(bigger.geometry)).size = {20, 20};
    const common::Affine2D placed = common::Affine2D::translation(10, 10);
    push<core::SetVectorObjectCommand>(doc, id, bigger, std::string("Edit Shape"), 0, placed);

    const auto* vlp = doc.find(id)->as<core::VectorLayer>();
    REQUIRE(vlp != nullptr);
    CHECK(std::get<vec::RectShape>(std::get<vec::ParametricShape>(vlp->object()->geometry)).size.x ==
          doctest::Approx(20.0));
    CHECK(vlp->transform().apply({0, 0}).x == doctest::Approx(10.0));  // placement set

    doc.commands().undo();  // ONE undo restores BOTH the object and the transform
    CHECK(std::get<vec::RectShape>(std::get<vec::ParametricShape>(vlp->object()->geometry)).size.x ==
          doctest::Approx(10.0));
    CHECK(vlp->transform().apply({0, 0}).x == doctest::Approx(5.0));  // original placement back
}

TEST_CASE("SetTransformsCommand moves several layers as one coalescing step") {
    Document doc(16, 16);
    core::Layer& a = doc.root().addOnTop(doc.makeRaster("A"));
    core::Layer& b = doc.root().addOnTop(doc.makeRaster("B"));
    const LayerId ia = a.id();
    const LayerId ib = b.id();

    using Entry = core::SetTransformsCommand::Entry;
    auto move = [&](double dx, std::uint64_t coalesce) {
        std::vector<Entry> e{{ia, common::Affine2D::translation(dx, 0)},
                             {ib, common::Affine2D::translation(dx, 0)}};
        doc.commands().push(std::make_unique<core::SetTransformsCommand>(std::move(e), coalesce));
    };

    move(5, 7);
    move(20, 7); // same ids + coalesce -> absorbed
    CHECK(doc.commands().undoCount() == 1);
    CHECK(a.transform().apply({0, 0}).x == doctest::Approx(20.0));
    CHECK(b.transform().apply({0, 0}).x == doctest::Approx(20.0));

    doc.commands().undo(); // both restore to identity in one step
    CHECK(a.transform().apply({0, 0}).x == doctest::Approx(0.0));
    CHECK(b.transform().apply({0, 0}).x == doctest::Approx(0.0));

    // A different coalesce id is a fresh undo step, not a continuation.
    move(3, 7);
    move(4, 9);
    CHECK(doc.commands().undoCount() == 2);
}

TEST_CASE("SetOpacitiesCommand sets several layers as one coalescing step") {
    Document doc(16, 16);
    core::Layer& a = doc.root().addOnTop(doc.makeRaster("A"));
    core::Layer& b = doc.root().addOnTop(doc.makeRaster("B"));
    a.setOpacity(0.4f); // distinct starting opacities -> undo must restore each
    b.setOpacity(0.9f);
    const LayerId ia = a.id();
    const LayerId ib = b.id();

    using Entry = core::SetOpacitiesCommand::Entry;
    auto set = [&](float v, std::uint64_t coalesce) {
        std::vector<Entry> e{{ia, v}, {ib, v}};
        doc.commands().push(std::make_unique<core::SetOpacitiesCommand>(std::move(e), coalesce));
    };

    set(0.6f, 7);
    set(0.3f, 7); // same ids + coalesce -> absorbed into one step
    CHECK(doc.commands().undoCount() == 1);
    CHECK(a.opacity() == doctest::Approx(0.3f));
    CHECK(b.opacity() == doctest::Approx(0.3f));

    doc.commands().undo(); // each restores to its own original, in one step
    CHECK(a.opacity() == doctest::Approx(0.4f));
    CHECK(b.opacity() == doctest::Approx(0.9f));

    // A different coalesce id is a fresh undo step, not a continuation.
    set(0.5f, 7);
    set(0.2f, 8);
    CHECK(doc.commands().undoCount() == 2);
}

TEST_CASE("CompositeCommand is a single undo step") {
    Document doc(16, 16);
    auto layer = doc.makeRaster("A");
    const LayerId id = layer->id();

    auto composite = std::make_unique<CompositeCommand>("Add & Name");
    composite->add(std::make_unique<AddLayerCommand>(doc.root().id(), 0, std::move(layer)));
    composite->add(std::make_unique<SetNameCommand>(id, std::string("named")));
    doc.commands().push(std::move(composite));

    CHECK(doc.find(id) != nullptr);
    CHECK(doc.find(id)->name() == "named");
    CHECK(doc.commands().undoCount() == 1);

    doc.commands().undo();  // both sub-edits reverse together
    CHECK(doc.find(id) == nullptr);
}

TEST_CASE("CommandStack tracks undo/redo availability, labels and clears redo") {
    Document doc(16, 16);
    const LayerId id = doc.root().addOnTop(doc.makeRaster("L")).id();
    auto& cs = doc.commands();

    CHECK_FALSE(cs.canUndo());
    CHECK_FALSE(cs.canRedo());

    push<SetNameCommand>(doc, id, std::string("one"));
    CHECK(cs.canUndo());
    CHECK(cs.undoName() == "Rename Layer");

    cs.undo();
    CHECK(cs.canRedo());
    CHECK(cs.redoName() == "Rename Layer");

    // A fresh edit after an undo discards the redo branch.
    push<SetVisibleCommand>(doc, id, false);
    CHECK_FALSE(cs.canRedo());

    cs.clear();
    CHECK_FALSE(cs.canUndo());
    CHECK_FALSE(cs.canRedo());
}

TEST_CASE("CommandStack history view: chronological names, jumpTo, one observer fire (S16-b)") {
    Document doc(8, 8);
    int notified = 0;
    doc.commands().setOnChange([&notified] { ++notified; });

    const LayerId a = doc.root().addOnTop(doc.makeRaster("A")).id();
    push<SetVisibleCommand>(doc, a, false);
    push<SetNameCommand>(doc, a, "renamed");
    push<SetOpacityCommand>(doc, a, 0.5f);
    CHECK(notified == 3);
    CHECK(doc.commands().size() == 3);
    CHECK(doc.commands().position() == 3);
    CHECK(doc.commands().nameAt(0) == "Toggle Visibility");
    CHECK(doc.commands().nameAt(1) == "Rename Layer");
    CHECK(doc.commands().nameAt(2) == "Set Opacity");
    CHECK(doc.commands().nameAt(3) == "");

    // Names keep chronological order across the undo boundary (the redo vector is a stack).
    doc.commands().undo();
    doc.commands().undo();
    CHECK(notified == 5);
    CHECK(doc.commands().position() == 1);
    CHECK(doc.commands().nameAt(1) == "Rename Layer");
    CHECK(doc.commands().nameAt(2) == "Set Opacity");

    // jumpTo batches its walk into ONE notification and lands the full document state.
    doc.commands().jumpTo(3);
    CHECK(notified == 6);
    CHECK(doc.commands().position() == 3);
    CHECK(doc.find(a)->opacity() == doctest::Approx(0.5f));
    CHECK(doc.find(a)->name() == "renamed");

    doc.commands().jumpTo(0);
    CHECK(notified == 7);
    CHECK(doc.commands().position() == 0);
    CHECK(doc.find(a)->visible());
    CHECK(doc.find(a)->name() == "A");

    doc.commands().jumpTo(0); // already there: no walk, no notification
    CHECK(notified == 7);

    doc.commands().jumpTo(99); // clamps to the end
    CHECK(doc.commands().position() == 3);

    // A coalesced push that merges into the top changes no entry: no notification (gesture
    // pushes arrive per input event; the panel must not rebuild per event).
    notified = 0;
    push<SetOpacityCommand>(doc, a, 0.6f, std::uint64_t{7});
    push<SetOpacityCommand>(doc, a, 0.7f, std::uint64_t{7});
    CHECK(doc.commands().size() == 4);
    CHECK(notified == 1);
}

TEST_CASE("CommandStack timestamps: chronological per entry, travel through undo, merge refreshes") {
    using Clock = core::Command::Clock;
    Document doc(8, 8);
    const auto before = Clock::now();
    const LayerId a = doc.root().addOnTop(doc.makeRaster("A")).id();
    push<SetVisibleCommand>(doc, a, false);
    push<SetNameCommand>(doc, a, "renamed");
    const auto after = Clock::now();

    // Each entry is stamped at push, within [before, after], in non-decreasing chronological order.
    CHECK(doc.commands().timeAt(0) >= before);
    CHECK(doc.commands().timeAt(1) >= doc.commands().timeAt(0));
    CHECK(doc.commands().timeAt(1) <= after);
    CHECK(doc.commands().timeAt(2) == Clock::time_point{}); // out of range -> epoch (no time)

    // The stamp travels with the entry across the undo boundary (the redo vector reads back-to-front).
    const auto t1 = doc.commands().timeAt(1);
    doc.commands().undo();
    CHECK(doc.commands().timeAt(1) == t1);
    doc.commands().redo();
    CHECK(doc.commands().timeAt(1) == t1);

    // A coalesced gesture refreshes the surviving top entry's stamp to its latest push.
    push<SetOpacityCommand>(doc, a, 0.6f, std::uint64_t{42});
    const auto firstOpacity = doc.commands().timeAt(2);
    push<SetOpacityCommand>(doc, a, 0.7f, std::uint64_t{42}); // merges into the entry above
    CHECK(doc.commands().size() == 3);
    CHECK(doc.commands().timeAt(2) >= firstOpacity);
}

TEST_CASE("ResizeCanvasCommand resizes the canvas and restores the old size") {
    Document doc(40, 30);
    push<core::ResizeCanvasCommand>(doc, 16u, 12u);
    CHECK(doc.width() == 16);
    CHECK(doc.height() == 12);
    doc.commands().undo();
    CHECK(doc.width() == 40);
    CHECK(doc.height() == 30);
    doc.commands().redo();
    CHECK(doc.width() == 16);
    CHECK(doc.height() == 12);
}

TEST_CASE("contentBounds: tight alpha bbox, cached until the pixel command invalidates") {
    Document doc(16, 16);
    auto* raster = doc.root().addOnTop(doc.makeRaster("L")).as<core::RasterLayer>();
    REQUIRE(raster != nullptr);
    CHECK_FALSE(raster->contentBounds().has_value()); // fully transparent: no content

    // Paint two pixels through the command, which must refresh the (already queried) cache.
    common::Image px(16, 16);
    const auto set = [&px](std::uint32_t x, std::uint32_t y) {
        px.rgba[(static_cast<std::size_t>(y) * px.width + x) * 4 + 3] = 255;
    };
    set(3, 5);
    set(9, 11);
    push<SetLayerPixelsCommand>(doc, raster->id(), px);
    const std::optional<common::Rect> b = raster->contentBounds();
    REQUIRE(b.has_value());
    CHECK(*b == common::Rect{3, 5, 7, 7});

    doc.commands().undo(); // back to transparent: the cache must follow
    CHECK_FALSE(raster->contentBounds().has_value());
}

// ---------------------------------------------------------------------------------------------
// S60-a: region-scoped SetLayerPixelsCommand + undo/redo dirty-region tracking.
// ---------------------------------------------------------------------------------------------

namespace {
common::Color8 at(const common::Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}
} // namespace

TEST_CASE("region SetLayerPixelsCommand patches only its rect, and undo/redo restore it") {
    Document doc(16, 16);
    auto* raster = doc.root().addOnTop(doc.makeRaster("L")).as<core::RasterLayer>();
    REQUIRE(raster != nullptr);
    raster->image().fill({10, 20, 30, 255}); // background A everywhere

    common::Image region(4, 4);
    region.fill({200, 100, 50, 255}); // B
    push<SetLayerPixelsCommand>(doc, raster->id(), region, /*ox=*/5, /*oy=*/6);

    const common::Image& img = raster->image();
    CHECK(at(img, 5, 6) == common::Color8{200, 100, 50, 255});   // inside the patch
    CHECK(at(img, 8, 9) == common::Color8{200, 100, 50, 255});   // inside (bottom-right corner)
    CHECK(at(img, 4, 6) == common::Color8{10, 20, 30, 255});     // just left of the patch: untouched
    CHECK(at(img, 9, 6) == common::Color8{10, 20, 30, 255});     // just right: untouched
    CHECK(at(img, 0, 0) == common::Color8{10, 20, 30, 255});     // far away: untouched

    doc.commands().undo();
    CHECK(at(raster->image(), 5, 6) == common::Color8{10, 20, 30, 255}); // region restored to A
    CHECK(at(raster->image(), 8, 9) == common::Color8{10, 20, 30, 255});

    doc.commands().redo();
    CHECK(at(raster->image(), 5, 6) == common::Color8{200, 100, 50, 255}); // B again
}

TEST_CASE("SetLayerPixelsCommand::dirtyRegion maps the layer-local rect to document space") {
    Document doc(64, 64);
    auto* raster = doc.root().addOnTop(doc.makeRaster("L")).as<core::RasterLayer>();
    REQUIRE(raster != nullptr);

    common::Image region(4, 4);
    region.fill({1, 2, 3, 255});

    SUBCASE("identity transform: doc rect == layer rect") {
        SetLayerPixelsCommand cmd(raster->id(), region, 5, 6);
        const auto r = cmd.dirtyRegion(doc);
        REQUIRE(r.has_value());
        CHECK(*r == common::Rect{5, 6, 4, 4});
    }
    SUBCASE("translated layer: rect shifts by the layer transform") {
        raster->setTransform(common::Affine2D::translation(10.0, 20.0));
        SetLayerPixelsCommand cmd(raster->id(), region, 5, 6);
        const auto r = cmd.dirtyRegion(doc);
        REQUIRE(r.has_value());
        CHECK(*r == common::Rect{15, 26, 4, 4});
    }

    // The whole-layer constructor reports the whole layer (origin 0, full size).
    SUBCASE("whole-layer constructor reports the whole layer") {
        common::Image whole(64, 64);
        SetLayerPixelsCommand cmd(raster->id(), whole);
        const auto r = cmd.dirtyRegion(doc);
        REQUIRE(r.has_value());
        CHECK(*r == common::Rect{0, 0, 64, 64});
    }
}

TEST_CASE("CommandStack::lastAffectedRegion follows undo/redo and is nullopt for non-pixel edits") {
    Document doc(32, 32);
    auto* raster = doc.root().addOnTop(doc.makeRaster("L")).as<core::RasterLayer>();
    REQUIRE(raster != nullptr);

    common::Image region(4, 4);
    region.fill({9, 9, 9, 255});
    push<SetLayerPixelsCommand>(doc, raster->id(), region, 7, 8);

    doc.commands().undo();
    REQUIRE(doc.commands().lastAffectedRegion().has_value());
    CHECK(*doc.commands().lastAffectedRegion() == common::Rect{7, 8, 4, 4});
    doc.commands().redo();
    REQUIRE(doc.commands().lastAffectedRegion().has_value());
    CHECK(*doc.commands().lastAffectedRegion() == common::Rect{7, 8, 4, 4});

    // A non-pixel edit reports no region -> the UI recomposites the whole document.
    push<SetVisibleCommand>(doc, raster->id(), false);
    doc.commands().undo();
    CHECK_FALSE(doc.commands().lastAffectedRegion().has_value());
}

TEST_CASE("jumpTo unions stepped pixel regions, but a whole-doc step forces a full recomposite") {
    Document doc(40, 40);
    auto* raster = doc.root().addOnTop(doc.makeRaster("L")).as<core::RasterLayer>();
    REQUIRE(raster != nullptr);

    common::Image a(3, 3), b(3, 3);
    a.fill({1, 1, 1, 255});
    b.fill({2, 2, 2, 255});
    push<SetLayerPixelsCommand>(doc, raster->id(), a, 2, 2);   // entry 0
    push<SetLayerPixelsCommand>(doc, raster->id(), b, 10, 12); // entry 1

    doc.commands().jumpTo(0); // undo both: union of {2,2,3,3} and {10,12,3,3}
    REQUIRE(doc.commands().lastAffectedRegion().has_value());
    CHECK(*doc.commands().lastAffectedRegion() == common::Rect{2, 2, 11, 13});

    doc.commands().jumpTo(2); // redo both: same union
    REQUIRE(doc.commands().lastAffectedRegion().has_value());
    CHECK(*doc.commands().lastAffectedRegion() == common::Rect{2, 2, 11, 13});

    // Insert a whole-document edit between the pixel edits; jumping across it must go full.
    push<SetOpacityCommand>(doc, raster->id(), 0.5f); // entry 2, dirtyRegion == nullopt
    doc.commands().jumpTo(0);
    CHECK_FALSE(doc.commands().lastAffectedRegion().has_value());
}

// S18-d: dirty tracking is a saved-position marker on the command stack, not a boolean.
TEST_CASE("dirty follows the command-stack saved marker (undo back to clean)") {
    Document doc(8, 8);
    CHECK(doc.commands().isSaved()); // a fresh document sits at the saved position (0)
    CHECK_FALSE(doc.dirty());

    push<AddLayerCommand>(doc, doc.root().id(), 0, doc.makeRaster("A"));
    CHECK(doc.dirty());
    CHECK_FALSE(doc.commands().isSaved());

    doc.commands().undo(); // back to the saved position -> clean again
    CHECK_FALSE(doc.dirty());
    CHECK(doc.commands().isSaved());

    doc.commands().redo(); // forward off it -> dirty again
    CHECK(doc.dirty());
}

TEST_CASE("markSaved moves the clean point; editing past it dirties, undoing to it cleans") {
    Document doc(8, 8);
    push<AddLayerCommand>(doc, doc.root().id(), 0, doc.makeRaster("A"));
    push<SetNameCommand>(doc, doc.root().child(0).id(), "renamed");
    CHECK(doc.dirty());

    doc.commands().markSaved(); // a Save at position 2
    CHECK_FALSE(doc.dirty());

    push<SetOpacityCommand>(doc, doc.root().child(0).id(), 0.5f); // position 3
    CHECK(doc.dirty());

    doc.commands().undo(); // back to the saved position 2
    CHECK_FALSE(doc.dirty());
}

TEST_CASE("saving, then undoing past the save and editing, makes the clean point unreachable") {
    Document doc(8, 8);
    push<AddLayerCommand>(doc, doc.root().id(), 0, doc.makeRaster("A")); // pos 1
    push<SetNameCommand>(doc, doc.root().child(0).id(), "renamed");      // pos 2
    doc.commands().markSaved();                                         // clean at pos 2

    doc.commands().undo(); // pos 1: below the saved marker -> dirty
    CHECK(doc.dirty());

    // A new edit here clears the redo branch that held the saved position: it can never be reached
    // again, so the document stays dirty even after undoing back to pos 1.
    push<SetVisibleCommand>(doc, doc.root().child(0).id(), false); // pos 2 on a NEW branch
    CHECK(doc.dirty());
    doc.commands().undo(); // pos 1 again -- still dirty, the clean point is gone
    CHECK(doc.dirty());
    doc.commands().undo(); // pos 0 -- also dirty (the marker is unreachable)
    CHECK(doc.dirty());
}

// ---- Layer masks (S31) ---------------------------------------------------------------------

TEST_CASE("SetLayerMaskCommand adds, replaces, deletes and reverses (one step each)") {
    Document doc(4, 4);
    push<AddLayerCommand>(doc, doc.root().id(), 0, doc.makeRaster("A"));
    core::Layer& layer = doc.root().child(0);
    const LayerId id = layer.id();

    // Add.
    core::RasterMask reveal(4, 4, 255);
    push<core::SetLayerMaskCommand>(doc, id, reveal, "Add Mask");
    REQUIRE(layer.hasMask());
    CHECK(*layer.mask() == reveal);
    CHECK(doc.commands().undoName() == "Add Mask");

    // Replace (Mask from Selection over an existing mask).
    core::RasterMask half(4, 4, 0);
    for (std::uint32_t i = 0; i < 8; ++i) half.coverage[i] = 255;
    push<core::SetLayerMaskCommand>(doc, id, half, "Mask from Selection");
    CHECK(*layer.mask() == half);
    doc.commands().undo();
    CHECK(*layer.mask() == reveal); // back to the previous mask, not to none
    doc.commands().redo();
    CHECK(*layer.mask() == half);

    // Delete.
    push<core::SetLayerMaskCommand>(doc, id, std::nullopt, "Delete Mask");
    CHECK_FALSE(layer.hasMask());
    doc.commands().undo();
    REQUIRE(layer.hasMask());
    CHECK(*layer.mask() == half);
}

TEST_CASE("SetMaskEnabled/SetMaskLinked flip the flags, reverse, and bump the mask revision") {
    Document doc(4, 4);
    push<AddLayerCommand>(doc, doc.root().id(), 0, doc.makeRaster("A"));
    core::Layer& layer = doc.root().child(0);
    const LayerId id = layer.id();
    push<core::SetLayerMaskCommand>(doc, id, core::RasterMask(4, 4, 255));

    const std::uint64_t rev0 = layer.maskRevision();
    push<core::SetMaskEnabledCommand>(doc, id, false);
    CHECK_FALSE(layer.mask()->enabled);
    CHECK(layer.maskRevision() > rev0); // the panel's mask-thumb cache key moved
    CHECK(doc.commands().undoName() == "Disable Mask");
    doc.commands().undo();
    CHECK(layer.mask()->enabled);

    push<core::SetMaskLinkedCommand>(doc, id, false);
    CHECK_FALSE(layer.mask()->linked);
    CHECK(doc.commands().undoName() == "Unlink Mask");
    doc.commands().undo();
    CHECK(layer.mask()->linked);
}

TEST_CASE("mask flag commands are symmetric no-ops on a maskless layer") {
    Document doc(4, 4);
    push<AddLayerCommand>(doc, doc.root().id(), 0, doc.makeRaster("A"));
    const LayerId id = doc.root().child(0).id();
    push<core::SetMaskEnabledCommand>(doc, id, false);
    push<core::SetMaskLinkedCommand>(doc, id, false);
    CHECK_FALSE(doc.root().child(0).hasMask());
    doc.commands().undo();
    doc.commands().undo();
    CHECK_FALSE(doc.root().child(0).hasMask());
}

TEST_CASE("SetMaskPixelsCommand patches a region, clips to the mask, and undoes byte-exact") {
    Document doc(8, 8);
    push<AddLayerCommand>(doc, doc.root().id(), 0, doc.makeRaster("A"));
    core::Layer& layer = doc.root().child(0);
    const LayerId id = layer.id();
    push<core::SetLayerMaskCommand>(doc, id, core::RasterMask(8, 8, 255));
    const std::vector<std::uint8_t> before = layer.mask()->coverage;

    // A 3x2 patch of zeros at (6, 3): one column (x=8) falls off the mask and is clipped.
    push<core::SetMaskPixelsCommand>(doc, id, std::vector<std::uint8_t>(3 * 2, 0), 3u, 2u, 6L, 3L);
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        return layer.mask()->coverage[static_cast<std::size_t>(y) * 8 + x];
    };
    CHECK(at(6, 3) == 0);
    CHECK(at(7, 4) == 0);
    CHECK(at(5, 3) == 255); // outside the patch
    CHECK(at(6, 5) == 255);
    CHECK(doc.commands().undoName() == "Paint Mask");

    doc.commands().undo();
    CHECK(layer.mask()->coverage == before);
    doc.commands().redo();
    CHECK(at(7, 3) == 0);
}

TEST_CASE("SetMaskPixelsCommand::dirtyRegion maps mask px through the fold transform") {
    Document doc(16, 16);
    push<AddLayerCommand>(doc, doc.root().id(), 0, doc.makeRaster("A"));
    core::Layer& layer = doc.root().child(0);
    const LayerId id = layer.id();
    push<core::SetLayerMaskCommand>(doc, id, core::RasterMask(16, 16, 255));
    layer.setTransform(mosaic::common::Affine2D::translation(4, 0));

    // Linked: the mask rides the layer transform -> the doc rect is shifted by +4 in x.
    core::SetMaskPixelsCommand probe(id, std::vector<std::uint8_t>(4, 0), 2u, 2u, 1L, 1L);
    const auto linked = probe.dirtyRegion(doc);
    REQUIRE(linked.has_value());
    CHECK(linked->x == doctest::Approx(5.0));
    CHECK(linked->y == doctest::Approx(1.0));
    CHECK(linked->w == doctest::Approx(2.0));

    layer.mask()->linked = false;
    const auto unlinked = probe.dirtyRegion(doc);
    REQUIRE(unlinked.has_value());
    CHECK(unlinked->x == doctest::Approx(1.0)); // parent space: the transform does not apply
    CHECK(unlinked->w == doctest::Approx(2.0));
}
