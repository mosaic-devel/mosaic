#include "core/document.hpp"

#include <doctest/doctest.h>

#include "core/blend_mode.hpp"
#include "core/layer.hpp"

using namespace mosaic;
using core::AdjustmentKind;
using core::BlendMode;
using core::Document;
using core::GroupLayer;
using core::LayerKind;
using core::RasterLayer;

TEST_CASE("a new document has sensible defaults") {
    Document doc(800, 600);
    CHECK(doc.width() == 800);
    CHECK(doc.height() == 600);
    CHECK(doc.colorSpace() == core::ColorSpace::SRGB);
    CHECK(doc.precision() == core::Precision::F16);  // 16-bit float default (PLAN §3.6)
    CHECK(doc.dpi() == doctest::Approx(72.0));
    CHECK(doc.title() == "Untitled");
    CHECK_FALSE(doc.dirty());
    CHECK(doc.root().empty());
    CHECK(doc.layerCount() == 0);
}

TEST_CASE("minted layer ids are unique and monotonic") {
    Document doc(16, 16);
    const auto a = doc.makeRaster("a");
    const auto b = doc.makeRaster("b");
    CHECK(a->id() != b->id());
    CHECK(b->id() > a->id());
    CHECK(a->id() != core::kInvalidLayerId);
}

TEST_CASE("layers insert bottom-to-top and track their parent") {
    Document doc(16, 16);
    GroupLayer& root = doc.root();

    core::Layer& bottom = root.addOnTop(doc.makeRaster("bottom"));
    core::Layer& top = root.addOnTop(doc.makeRaster("top"));
    core::Layer& middle = root.insert(1, doc.makeRaster("middle"));

    REQUIRE(root.childCount() == 3);
    CHECK(root.child(0).name() == "bottom");  // index 0 == bottom of the stack
    CHECK(root.child(1).name() == "middle");
    CHECK(root.child(2).name() == "top");
    CHECK(bottom.parent() == &root);
    CHECK(top.parent() == &root);
    CHECK(middle.parent() == &root);
    CHECK(root.indexOf(middle.id()) == 1);
    CHECK(root.indexOf(core::kInvalidLayerId) == GroupLayer::npos);
}

TEST_CASE("find and locate reach nested layers") {
    Document doc(16, 16);
    GroupLayer& root = doc.root();
    auto* group = root.addOnTop(doc.makeGroup("grp")).as<GroupLayer>();
    REQUIRE(group != nullptr);
    core::Layer& nested = group->addOnTop(doc.makeRaster("nested"));
    const core::LayerId nestedId = nested.id();

    CHECK(doc.find(nestedId) == &nested);
    CHECK(doc.find(core::kInvalidLayerId) == nullptr);
    CHECK(doc.layerCount() == 2);  // group + nested

    const auto loc = doc.locate(nestedId);
    REQUIRE(loc.has_value());
    CHECK(loc->parent == group);
    CHECK(loc->index == 0);

    CHECK_FALSE(doc.locate(doc.root().id()).has_value());  // the root has no location
    CHECK_FALSE(doc.locate(core::kInvalidLayerId).has_value());
}

TEST_CASE("removeAt yields ownership and detaches; re-insert reattaches") {
    Document doc(16, 16);
    GroupLayer& root = doc.root();
    const core::LayerId id = root.addOnTop(doc.makeRaster("only")).id();
    REQUIRE(root.childCount() == 1);

    std::unique_ptr<core::Layer> taken = root.removeAt(0);
    REQUIRE(taken != nullptr);
    CHECK(taken->id() == id);
    CHECK(taken->parent() == nullptr);
    CHECK(root.empty());

    root.insert(0, std::move(taken));
    CHECK(root.childCount() == 1);
    CHECK(root.child(0).parent() == &root);

    CHECK(root.removeAt(5) == nullptr);  // out of range is a safe no-op
}

TEST_CASE("shared layer properties round-trip and opacity clamps") {
    Document doc(16, 16);
    core::Layer& l = doc.root().addOnTop(doc.makeRaster("x"));

    l.setName("renamed");
    l.setVisible(false);
    l.setBlendMode(BlendMode::Multiply);
    l.setClipToBelow(true);
    l.setLocked(true);
    l.setTransform(common::Affine2D::translation(5, 7));

    CHECK(l.name() == "renamed");
    CHECK_FALSE(l.visible());
    CHECK(l.blendMode() == BlendMode::Multiply);
    CHECK(l.clipToBelow());
    CHECK(l.locked());
    CHECK(l.transform().apply({0, 0}) == common::Vec2{5, 7});

    l.setOpacity(2.0f);
    CHECK(l.opacity() == doctest::Approx(1.0f));
    l.setOpacity(-1.0f);
    CHECK(l.opacity() == doctest::Approx(0.0f));
    l.setOpacity(0.5f);
    CHECK(l.opacity() == doctest::Approx(0.5f));
}

TEST_CASE("raster masks attach, detach and round-trip") {
    Document doc(16, 16);
    core::Layer& l = doc.root().addOnTop(doc.makeRaster("x"));
    CHECK_FALSE(l.hasMask());

    l.setMask(core::RasterMask(4, 4, /*fill=*/128));
    REQUIRE(l.hasMask());
    CHECK(l.mask()->width == 4);
    CHECK(l.mask()->coverage.size() == 16);
    CHECK(l.mask()->coverage[0] == 128);
    CHECK(l.mask()->enabled);
    CHECK(l.mask()->linked);

    const core::RasterMask detached = l.takeMask();
    CHECK_FALSE(l.hasMask());
    CHECK(detached.width == 4);

    l.setMask(detached);
    l.clearMask();
    CHECK_FALSE(l.hasMask());
}

TEST_CASE("every layer kind constructs with the right kind and payload") {
    Document doc(32, 24);

    auto raster = doc.makeRaster("r", 10, 8);
    CHECK(raster->kind() == LayerKind::Raster);
    CHECK(raster->image().width == 10);
    CHECK(raster->image().height == 8);
    CHECK(doc.makeRaster("canvas-sized")->image().width == 32);  // defaults to canvas size

    auto group = doc.makeGroup("g");
    CHECK(group->kind() == LayerKind::Group);
    CHECK(group->expanded());

    auto vec = doc.makeVector("v");
    CHECK(vec->kind() == LayerKind::Vector);

    auto text = doc.makeText("t", "hello");
    CHECK(text->kind() == LayerKind::Text);
    CHECK(text->text() == "hello");

    auto adj = doc.makeAdjustment("a", AdjustmentKind::Grayscale);
    CHECK(adj->kind() == LayerKind::Adjustment);
    CHECK(adj->adjustmentKind() == AdjustmentKind::Grayscale);
    adj->params()["amount"] = 0.5;
    CHECK(adj->params().at("amount") == doctest::Approx(0.5));

    common::Image src(6, 6);
    auto magic = doc.makeMagic("m", src);
    CHECK(magic->kind() == LayerKind::Magic);
    CHECK(magic->source().width == 6);

    // The base-class down-cast helper respects the real type.
    core::Layer& asBase = *raster;
    CHECK(asBase.as<RasterLayer>() != nullptr);
    CHECK(asBase.as<GroupLayer>() == nullptr);
}

TEST_CASE("enum name helpers are populated") {
    CHECK(core::layerKindName(LayerKind::Magic) == "Magic");
    CHECK(core::blendModeName(BlendMode::ColorDodge) == "Color Dodge");
    CHECK(core::adjustmentKindName(AdjustmentKind::HueSaturation) == "Hue/Saturation");
    CHECK(core::colorSpaceName(core::ColorSpace::DisplayP3) == "Display P3");
    CHECK(core::precisionName(core::Precision::F16) == "16-bit float");
}

TEST_CASE("duplicateLayer deep-copies a subtree with fresh ids") {
    Document doc(64, 48);

    auto group = doc.makeGroup("Group");
    group->setOpacity(0.5f);
    group->setBlendMode(BlendMode::Multiply);
    auto* groupPtr = group->as<GroupLayer>();

    auto raster = doc.makeRaster("Child", 8, 8);
    raster->image().fill(common::Color8{10, 20, 30, 200});
    raster->setVisible(false);
    raster->setMask(core::RasterMask(8, 8, 128));
    const core::LayerId origRasterId = raster->id();
    groupPtr->addOnTop(std::move(raster));
    const core::LayerId origGroupId = group->id();
    doc.root().addOnTop(std::move(group));

    auto clone = doc.duplicateLayer(*doc.find(origGroupId));
    REQUIRE(clone != nullptr);

    // Fresh ids, identical structure.
    CHECK(clone->id() != origGroupId);
    CHECK(clone->kind() == LayerKind::Group);
    CHECK(clone->opacity() == doctest::Approx(0.5f));
    CHECK(clone->blendMode() == BlendMode::Multiply);
    auto* cloneGroup = clone->as<GroupLayer>();
    REQUIRE(cloneGroup->childCount() == 1);

    const auto& cloneChild = cloneGroup->child(0);
    CHECK(cloneChild.id() != origRasterId); // descendants are re-minted too
    CHECK(cloneChild.name() == "Child");
    CHECK(cloneChild.visible() == false);
    REQUIRE(cloneChild.hasMask());
    CHECK(cloneChild.mask()->coverage[0] == 128);

    const auto* cloneRaster = cloneChild.as<RasterLayer>();
    REQUIRE(cloneRaster != nullptr);
    CHECK(cloneRaster->image().width == 8);
    CHECK(cloneRaster->image().rgba[3] == 200); // pixels copied

    // The clone is detached, so it does not change the document until inserted.
    CHECK(doc.layerCount() == 2); // original group + its child only
    CHECK(doc.find(clone->id()) == nullptr);
}

TEST_CASE("topmostLayerAt: stacking order, alpha, transforms, visibility, groups (S15)") {
    Document doc(16, 16);

    auto bottom = doc.makeRaster("bottom"); // document-sized, opaque everywhere
    for (std::size_t p = 3; p < bottom->image().rgba.size(); p += 4)
        bottom->image().rgba[p] = 255;
    const auto bottomId = bottom->id();
    doc.root().addOnTop(std::move(bottom));

    auto top = doc.makeRaster("top", 4, 4); // small, translated to (6,6), opaque
    for (std::size_t p = 3; p < top->image().rgba.size(); p += 4)
        top->image().rgba[p] = 255;
    top->setTransform(common::Affine2D::translation(6, 6));
    const auto topId = top->id();

    auto group = doc.makeGroup("g"); // the top layer lives inside a group
    group->addOnTop(std::move(top));
    const auto groupId = group->id();
    doc.root().addOnTop(std::move(group));

    // Over both: the grouped top layer wins; beside it: the bottom layer.
    core::Layer* hit = core::topmostLayerAt(doc.root(), {7.0, 7.0});
    REQUIRE(hit != nullptr);
    CHECK(hit->id() == topId);
    hit = core::topmostLayerAt(doc.root(), {2.0, 2.0});
    REQUIRE(hit != nullptr);
    CHECK(hit->id() == bottomId);

    // Transparent pixels don't hit: punch a hole in the top layer at doc (6,6).
    static_cast<RasterLayer*>(doc.find(topId))->image().rgba[3] = 0;
    hit = core::topmostLayerAt(doc.root(), {6.5, 6.5});
    REQUIRE(hit != nullptr);
    CHECK(hit->id() == bottomId);

    // Hiding the group hides its subtree; hiding everything hits nothing.
    doc.find(groupId)->setVisible(false);
    hit = core::topmostLayerAt(doc.root(), {7.5, 7.5});
    REQUIRE(hit != nullptr);
    CHECK(hit->id() == bottomId);
    doc.find(bottomId)->setVisible(false);
    CHECK(core::topmostLayerAt(doc.root(), {7.5, 7.5}) == nullptr);
}

TEST_CASE("topmostTextLayerAt boxes the measured text content bounds (S29-b select-to-edit)") {
    Document doc(40, 40);
    auto* ta = doc.root().addOnTop(doc.makeText("A", "hi")).as<core::TextLayer>();
    REQUIRE(ta != nullptr);
    ta->setTransform(common::Affine2D::translation(5, 5)); // layer-local origin at doc (5,5)

    // The box is renderer-populated; until measured, the layer is never a hit.
    CHECK(core::topmostTextLayerAt(doc.root(), {8, 8}) == nullptr);
    ta->setCachedContentBounds(common::Rect{0, 0, 10, 8}); // layer-local content box

    CHECK(core::topmostTextLayerAt(doc.root(), {8, 8}) == ta);       // local (3,3): inside
    CHECK(core::topmostTextLayerAt(doc.root(), {20, 20}) == nullptr); // local (15,15): outside
    CHECK(core::topmostTextLayerAt(doc.root(), {16, 8}) == nullptr);  // local (11,3): just outside x
    CHECK(core::topmostTextLayerAt(doc.root(), {16, 8}, 2.0) == ta);  // padded by 2 -> caught

    ta->setVisible(false);
    CHECK(core::topmostTextLayerAt(doc.root(), {8, 8}) == nullptr); // hidden layers are skipped
}

TEST_CASE("group contentBounds unions visible children through their transforms") {
    Document doc(32, 32);
    auto* group = doc.root().addOnTop(doc.makeGroup("G")).as<GroupLayer>();
    REQUIRE(group != nullptr);
    CHECK_FALSE(group->contentBounds().has_value()); // empty group: no content

    const auto paint = [](RasterLayer& r, std::uint32_t x, std::uint32_t y) {
        r.image().rgba[(static_cast<std::size_t>(y) * r.image().width + x) * 4 + 3] = 255;
    };
    auto a = doc.makeRaster("A", 8, 8);
    paint(*a, 1, 1); // content (1,1)..(1,1)
    auto b = doc.makeRaster("B", 8, 8);
    paint(*b, 2, 3); // content (2,3), translated by (10, 10) -> (12, 13)
    b->setTransform(common::Affine2D::translation(10, 10));
    group->addOnTop(std::move(a));
    auto* bRaw = group->addOnTop(std::move(b)).as<RasterLayer>();

    const auto bounds = group->contentBounds();
    REQUIRE(bounds.has_value());
    CHECK(*bounds == common::Rect{1, 1, 12, 13}); // (1,1) .. (13,14) exclusive

    bRaw->setVisible(false); // hidden children don't count
    const auto onlyA = group->contentBounds();
    REQUIRE(onlyA.has_value());
    CHECK(*onlyA == common::Rect{1, 1, 1, 1});
}

TEST_CASE("moveClickTarget: outermost group first, then click-to-drill (Affinity model)") {
    // root > G > H > L, plus sibling M inside H and ungrouped X at top level.
    Document doc(8, 8);
    auto* g = doc.root().addOnTop(doc.makeGroup("G")).as<GroupLayer>();
    auto* h = g->addOnTop(doc.makeGroup("H")).as<GroupLayer>();
    auto* l = &h->addOnTop(doc.makeRaster("L", 8, 8));
    auto* m = &h->addOnTop(doc.makeRaster("M", 8, 8));
    auto* x = &doc.root().addOnTop(doc.makeRaster("X", 8, 8));

    // No current target: the outermost group wins; ungrouped layers select themselves.
    CHECK(core::moveClickTarget(l, nullptr) == g);
    CHECK(core::moveClickTarget(x, nullptr) == x);

    // Drill one level per click while the target is an ancestor of (or is) the hit.
    CHECK(core::moveClickTarget(l, g) == h);
    CHECK(core::moveClickTarget(l, h) == l);
    CHECK(core::moveClickTarget(l, l) == l); // already there: stays

    // Inside an entered group, clicking a sibling selects it at that depth.
    CHECK(core::moveClickTarget(m, l) == m);

    // Clicking outside every shared group exits to the outermost rule.
    CHECK(core::moveClickTarget(x, l) == x);
    CHECK(core::moveClickTarget(l, x) == g);
}

TEST_CASE("worldTransform composes ancestor group transforms (S15.z)") {
    Document doc(16, 16);
    auto* g = doc.root().addOnTop(doc.makeGroup("G")).as<GroupLayer>();
    auto* h = g->addOnTop(doc.makeGroup("H")).as<GroupLayer>();
    auto* l = &h->addOnTop(doc.makeRaster("L", 4, 4));
    g->setTransform(common::Affine2D::translation(10, 0));
    h->setTransform(common::Affine2D::scaling(2, 2));
    l->setTransform(common::Affine2D::translation(1, 1));

    // L's local (0,0) -> +1,+1 (own) -> x2 (H) -> +10 x (G) = (12, 2).
    const common::Vec2 p = core::worldTransform(*l).apply({0, 0});
    CHECK(p == common::Vec2{12, 2});
    CHECK(core::parentWorldTransform(*l).apply({0, 0}) == common::Vec2{10, 0});
    // Top-level layers: parent chain is just the (identity) root.
    auto* x = &doc.root().addOnTop(doc.makeRaster("X", 4, 4));
    CHECK(core::worldTransform(*x) == x->transform());
}

TEST_CASE("topmostLayerAt samples nested layers through ancestor transforms (S15.z)") {
    Document doc(16, 16);
    auto* g = doc.root().addOnTop(doc.makeGroup("G")).as<GroupLayer>();
    auto child = doc.makeRaster("C", 4, 4);
    child->image().fill({200, 0, 0, 255}); // opaque 4x4 at the group origin
    core::Layer* c = &g->addOnTop(std::move(child));
    g->setTransform(common::Affine2D::translation(8, 8)); // shown at (8,8)..(12,12)

    CHECK(core::topmostLayerAt(doc.root(), {9.0, 9.0}) == c);  // where it is SHOWN
    CHECK(core::topmostLayerAt(doc.root(), {1.0, 1.0}) == nullptr); // not where it "was"
}
