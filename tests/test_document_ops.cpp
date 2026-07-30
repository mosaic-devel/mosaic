#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <utility>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "core/guides.hpp"
#include "core/layer.hpp"
#include "core/selection.hpp"
#include "render/compositor.hpp"
#include "render/document_ops.hpp"

using namespace mosaic;
using render::CanvasAnchor;
using render::DocOrient;

// ---------------------------------------------------------------------------------------------
// render/document_ops.hpp -- the S53-a Image-menu operations. They all run through the one shared
// engine (render::buildDocumentRemapCommand), so what is worth pinning here is the SEMANTICS each
// one chose: the anchor grid and its rounding rule, the byte-losslessness of the orientation ops,
// the guide rebase (which no crop used to do at all), and that every one is a single undo step.
// ---------------------------------------------------------------------------------------------
namespace {

// Composite on the deterministic CPU reference (the default options resample Nearest), which is
// what makes a byte-for-byte comparison of two orientations meaningful.
common::Image flatten(const core::Document& doc) {
    const render::CompositeResult r = render::composite(doc, {}, render::Backend::Cpu);
    REQUIRE(r.ok);
    return r.image;
}

common::Color8 px(const common::Image& img, std::uint32_t x, std::uint32_t y) {
    const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[p], img.rgba[p + 1], img.rgba[p + 2], img.rgba[p + 3]};
}

// A document whose single raster carries a DISTINCT colour per pixel, so an orientation can be
// checked cell by cell rather than by a summary statistic that a mirrored bug would still satisfy.
std::unique_ptr<core::Document> makePatternDoc(std::uint32_t w, std::uint32_t h) {
    auto doc = std::make_unique<core::Document>(w, h);
    auto* layer = doc->root().addOnTop(doc->makeRaster("L")).as<core::RasterLayer>();
    REQUIRE(layer != nullptr);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            layer->image().rgba[p] = static_cast<std::uint8_t>(20 + x * 30);
            layer->image().rgba[p + 1] = static_cast<std::uint8_t>(40 + y * 50);
            layer->image().rgba[p + 2] = 7;
            layer->image().rgba[p + 3] = 255;
        }
    }
    return doc;
}

// Push a command the test expects to exist, so a silently-refused op fails loudly here instead of
// dereferencing null inside the command stack.
void pushOp(core::Document& doc, std::unique_ptr<core::Command> cmd) {
    REQUIRE(static_cast<bool>(cmd));
    doc.commands().push(std::move(cmd));
}

// Where the old pixel (x, y) of a w x h document lands after `op` -- the mapping the header
// describes, written out independently of the affines document_ops.cpp builds.
std::pair<std::uint32_t, std::uint32_t> orientPixel(DocOrient op, std::uint32_t w, std::uint32_t h,
                                                    std::uint32_t x, std::uint32_t y) {
    switch (op) {
        case DocOrient::Rotate90CW: return {h - 1 - y, x};
        case DocOrient::Rotate90CCW: return {y, w - 1 - x};
        case DocOrient::Rotate180: return {w - 1 - x, h - 1 - y};
        case DocOrient::FlipHorizontal: return {w - 1 - x, y};
        case DocOrient::FlipVertical: return {x, h - 1 - y};
    }
    return {x, y};
}

constexpr DocOrient kAllOrients[] = {DocOrient::Rotate90CW, DocOrient::Rotate90CCW,
                                     DocOrient::Rotate180, DocOrient::FlipHorizontal,
                                     DocOrient::FlipVertical};

}  // namespace

TEST_CASE("canvasRectFor places the old canvas for every anchor when the canvas grows") {
    // 40x30 -> 60x50: the old canvas has 20 px of slack on each axis, so the column offsets are
    // {0, 10, 20} and the row offsets likewise.
    const auto at = [](CanvasAnchor a) { return render::canvasRectFor(40, 30, 60, 50, a); };
    CHECK(at(CanvasAnchor::TopLeft).x == 0);
    CHECK(at(CanvasAnchor::TopLeft).y == 0);
    CHECK(at(CanvasAnchor::Top).x == 10);
    CHECK(at(CanvasAnchor::Top).y == 0);
    CHECK(at(CanvasAnchor::TopRight).x == 20);
    CHECK(at(CanvasAnchor::TopRight).y == 0);
    CHECK(at(CanvasAnchor::Left).x == 0);
    CHECK(at(CanvasAnchor::Left).y == 10);
    CHECK(at(CanvasAnchor::Center).x == 10);
    CHECK(at(CanvasAnchor::Center).y == 10);
    CHECK(at(CanvasAnchor::Right).x == 20);
    CHECK(at(CanvasAnchor::Right).y == 10);
    CHECK(at(CanvasAnchor::BottomLeft).x == 0);
    CHECK(at(CanvasAnchor::BottomLeft).y == 20);
    CHECK(at(CanvasAnchor::Bottom).x == 10);
    CHECK(at(CanvasAnchor::Bottom).y == 20);
    CHECK(at(CanvasAnchor::BottomRight).x == 20);
    CHECK(at(CanvasAnchor::BottomRight).y == 20);
    // The rect is always the OLD canvas's footprint, so its size is the old size.
    CHECK(at(CanvasAnchor::Center).w == 40);
    CHECK(at(CanvasAnchor::Center).h == 30);
}

TEST_CASE("canvasRectFor places the old canvas for every anchor when the canvas shrinks") {
    // 40x30 -> 20x10: the old canvas overhangs by 20 px on each axis, so the offsets go negative
    // (the old content starts off the top/left edge and the remainder is cropped).
    const auto at = [](CanvasAnchor a) { return render::canvasRectFor(40, 30, 20, 10, a); };
    CHECK(at(CanvasAnchor::TopLeft).x == 0);
    CHECK(at(CanvasAnchor::TopLeft).y == 0);
    CHECK(at(CanvasAnchor::Top).x == -10);
    CHECK(at(CanvasAnchor::Top).y == 0);
    CHECK(at(CanvasAnchor::TopRight).x == -20);
    CHECK(at(CanvasAnchor::TopRight).y == 0);
    CHECK(at(CanvasAnchor::Left).x == 0);
    CHECK(at(CanvasAnchor::Left).y == -10);
    CHECK(at(CanvasAnchor::Center).x == -10);
    CHECK(at(CanvasAnchor::Center).y == -10);
    CHECK(at(CanvasAnchor::Right).x == -20);
    CHECK(at(CanvasAnchor::Right).y == -10);
    CHECK(at(CanvasAnchor::BottomLeft).x == 0);
    CHECK(at(CanvasAnchor::BottomLeft).y == -20);
    CHECK(at(CanvasAnchor::Bottom).x == -10);
    CHECK(at(CanvasAnchor::Bottom).y == -20);
    CHECK(at(CanvasAnchor::BottomRight).x == -20);
    CHECK(at(CanvasAnchor::BottomRight).y == -20);
    CHECK(at(CanvasAnchor::BottomRight).w == 40);
}

TEST_CASE("canvasRectFor centres an odd difference toward the right and bottom, both directions") {
    // The documented rule: truncate toward zero, so the odd pixel always lands on the right /
    // bottom whether the canvas is growing or shrinking. Growing 100 -> 101 adds the column on the
    // right (offset 0); shrinking 101 -> 100 takes it off the right (offset 0 as well).
    CHECK(render::canvasRectFor(100, 100, 101, 101, CanvasAnchor::Center).x == 0);
    CHECK(render::canvasRectFor(100, 100, 101, 101, CanvasAnchor::Center).y == 0);
    CHECK(render::canvasRectFor(101, 101, 100, 100, CanvasAnchor::Center).x == 0);
    CHECK(render::canvasRectFor(101, 101, 100, 100, CanvasAnchor::Center).y == 0);
    // With a difference of 3 the split is 1 / 2, again favouring the right and bottom.
    CHECK(render::canvasRectFor(100, 100, 103, 103, CanvasAnchor::Center).x == 1);
    CHECK(render::canvasRectFor(103, 103, 100, 100, CanvasAnchor::Center).x == -1);
}

TEST_CASE("every orientation op is byte-exact on a distinctive raster pattern") {
    for (const DocOrient op : kAllOrients) {
        CAPTURE(static_cast<int>(op));
        std::unique_ptr<core::Document> doc = makePatternDoc(5, 3);
        const common::Image before = flatten(*doc);
        const bool quarter = op == DocOrient::Rotate90CW || op == DocOrient::Rotate90CCW;
        pushOp(*doc, render::buildOrientCommand(*doc, op));
        CHECK(doc->width() == (quarter ? 3u : 5u));
        CHECK(doc->height() == (quarter ? 5u : 3u));
        const common::Image after = flatten(*doc);
        REQUIRE(after.width == doc->width());
        REQUIRE(after.height == doc->height());
        std::size_t mismatches = 0;
        for (std::uint32_t y = 0; y < 3; ++y) {
            for (std::uint32_t x = 0; x < 5; ++x) {
                const auto [nx, ny] = orientPixel(op, 5, 3, x, y);
                if (!(px(after, nx, ny) == px(before, x, y)))
                    ++mismatches;
            }
        }
        CHECK(mismatches == 0);
    }
}

TEST_CASE("the orientation ops compose to the identity in pairs") {
    struct Pair { DocOrient first; DocOrient second; };
    const Pair pairs[] = {{DocOrient::Rotate90CW, DocOrient::Rotate90CCW},
                          {DocOrient::Rotate90CCW, DocOrient::Rotate90CW},
                          {DocOrient::Rotate180, DocOrient::Rotate180},
                          {DocOrient::FlipHorizontal, DocOrient::FlipHorizontal},
                          {DocOrient::FlipVertical, DocOrient::FlipVertical}};
    for (const Pair& p : pairs) {
        CAPTURE(static_cast<int>(p.first));
        std::unique_ptr<core::Document> doc = makePatternDoc(5, 3);
        const common::Image before = flatten(*doc);
        pushOp(*doc, render::buildOrientCommand(*doc, p.first));
        pushOp(*doc, render::buildOrientCommand(*doc, p.second));
        CHECK(doc->width() == 5);
        CHECK(doc->height() == 3);
        CHECK(flatten(*doc).rgba == before.rgba);
        // ...and undoing both steps individually gets back there too.
        doc->commands().undo();
        doc->commands().undo();
        CHECK(doc->width() == 5);
        CHECK(flatten(*doc).rgba == before.rgba);
    }
}

TEST_CASE("an orientation op remaps the selection instead of clearing it") {
    core::Document doc(6, 4);
    doc.root().addOnTop(doc.makeRaster("L"));
    doc.setSelection(core::Selection::rectangle(6, 4, {0, 0, 2, 4})); // the left two columns
    pushOp(doc, render::buildOrientCommand(doc, DocOrient::FlipHorizontal));
    REQUIRE_FALSE(doc.selection().isEmpty());
    CHECK(doc.selection().width() == 6);
    CHECK(doc.selection().at(5, 0) == 255); // mirrored to the RIGHT two columns
    CHECK(doc.selection().at(4, 3) == 255);
    CHECK(doc.selection().at(0, 0) == 0);
    CHECK(doc.selection().at(3, 2) == 0);
}

TEST_CASE("a canvas resize carries the guides with the canvas") {
    core::Document doc(40, 30);
    doc.root().addOnTop(doc.makeRaster("L"));
    const std::uint64_t hid = doc.mintGuideId();
    const std::uint64_t vid = doc.mintGuideId();
    doc.addGuide({core::Guide::Orientation::Horizontal, 10.0, hid});
    doc.addGuide({core::Guide::Orientation::Vertical, 5.0, vid});
    // Grow 10 columns with the content centred: 5 are added on each side.
    pushOp(doc, render::buildCanvasResizeCommand(doc, 50, 30, CanvasAnchor::Center, std::nullopt));
    REQUIRE(doc.guides().size() == 2);
    REQUIRE(doc.findGuide(hid) != nullptr);
    REQUIRE(doc.findGuide(vid) != nullptr);
    CHECK(doc.findGuide(hid)->position == doctest::Approx(10.0)); // rows did not move
    CHECK(doc.findGuide(vid)->position == doctest::Approx(10.0)); // 5 columns added on the left
    CHECK(doc.findGuide(hid)->horizontal());
    doc.commands().undo();
    CHECK(doc.findGuide(vid)->position == doctest::Approx(5.0));
}

TEST_CASE("a quarter turn swaps each guide's axis and position") {
    core::Document doc(40, 30);
    doc.root().addOnTop(doc.makeRaster("L"));
    const std::uint64_t hid = doc.mintGuideId();
    const std::uint64_t vid = doc.mintGuideId();
    doc.addGuide({core::Guide::Orientation::Horizontal, 10.0, hid});
    doc.addGuide({core::Guide::Orientation::Vertical, 5.0, vid});
    pushOp(doc, render::buildOrientCommand(doc, DocOrient::Rotate90CW));
    CHECK(doc.width() == 30);
    CHECK(doc.height() == 40);
    REQUIRE(doc.findGuide(hid) != nullptr);
    REQUIRE(doc.findGuide(vid) != nullptr);
    // (x,y) -> (H - y, x): the row at y=10 becomes the column at x = 30 - 10 = 20, and the
    // column at x=5 becomes the row at y = 5.
    CHECK_FALSE(doc.findGuide(hid)->horizontal());
    CHECK(doc.findGuide(hid)->position == doctest::Approx(20.0));
    CHECK(doc.findGuide(vid)->horizontal());
    CHECK(doc.findGuide(vid)->position == doctest::Approx(5.0));
    doc.commands().undo();
    CHECK(doc.findGuide(hid)->horizontal());
    CHECK(doc.findGuide(hid)->position == doctest::Approx(10.0));
}

TEST_CASE("a guide pushed off the shrinking canvas is dropped, and undo brings it back") {
    core::Document doc(40, 30);
    doc.root().addOnTop(doc.makeRaster("L"));
    const std::uint64_t kept = doc.mintGuideId();
    const std::uint64_t lost = doc.mintGuideId();
    doc.addGuide({core::Guide::Orientation::Vertical, 4.0, kept});
    doc.addGuide({core::Guide::Orientation::Vertical, 35.0, lost});
    pushOp(doc, render::buildCanvasResizeCommand(doc, 20, 30, CanvasAnchor::TopLeft, std::nullopt));
    CHECK(doc.guides().size() == 1);
    CHECK(doc.findGuide(kept) != nullptr);
    CHECK(doc.findGuide(lost) == nullptr);
    doc.commands().undo();
    CHECK(doc.guides().size() == 2);
    REQUIRE(doc.findGuide(lost) != nullptr);
    CHECK(doc.findGuide(lost)->position == doctest::Approx(35.0));
}

TEST_CASE("undo and redo of a canvas resize restore the size, transforms, guides and selection") {
    core::Document doc(40, 30);
    auto* layer = doc.root().addOnTop(doc.makeRaster("L")).as<core::RasterLayer>();
    REQUIRE(layer != nullptr);
    layer->image().fill({10, 20, 30, 255});
    const std::uint64_t gid = doc.mintGuideId();
    doc.addGuide({core::Guide::Orientation::Vertical, 8.0, gid});
    doc.setSelection(core::Selection::rectangle(40, 30, {4, 4, 10, 10}));
    const core::Selection selBefore = doc.selection();

    // Anchor Right on the middle row: 20 columns are added on the LEFT, nothing vertically.
    pushOp(doc, render::buildCanvasResizeCommand(doc, 60, 30, CanvasAnchor::Right, std::nullopt));
    CHECK(doc.commands().undoCount() == 1); // exactly one undo step for the whole operation
    CHECK(doc.width() == 60);
    CHECK(doc.height() == 30);
    CHECK(layer->transform() == common::Affine2D::translation(20, 0));
    CHECK(doc.findGuide(gid)->position == doctest::Approx(28.0));
    CHECK(doc.selection().width() == 60);
    CHECK(doc.selection().at(24, 4) == 255);

    doc.commands().undo();
    CHECK(doc.width() == 40);
    CHECK(layer->transform() == common::Affine2D::identity());
    CHECK(doc.findGuide(gid)->position == doctest::Approx(8.0));
    CHECK(doc.selection() == selBefore);

    doc.commands().redo();
    CHECK(doc.width() == 60);
    CHECK(layer->transform() == common::Affine2D::translation(20, 0));
    CHECK(doc.findGuide(gid)->position == doctest::Approx(28.0));
    CHECK(doc.selection().width() == 60);
}

TEST_CASE("image resize scales the document, resamples the plain raster and scales the selection") {
    core::Document doc(40, 30);
    auto* layer = doc.root().addOnTop(doc.makeRaster("L")).as<core::RasterLayer>();
    REQUIRE(layer != nullptr);
    layer->image().fill({200, 100, 50, 255});
    doc.setSelection(core::Selection::rectangle(40, 30, {0, 0, 40, 30}));

    pushOp(doc, render::buildImageResizeCommand(doc, 20, 15, render::ResampleFilter::Area));
    CHECK(doc.width() == 20);
    CHECK(doc.height() == 15);
    // A plain unmasked raster at identity is PHYSICALLY resampled and lands back at identity.
    CHECK(layer->image().width == 20);
    CHECK(layer->image().height == 15);
    CHECK(layer->transform() == common::Affine2D::identity());
    CHECK(px(flatten(doc), 10, 7) == common::Color8{200, 100, 50, 255});
    REQUIRE_FALSE(doc.selection().isEmpty());
    CHECK(doc.selection().width() == 20);
    CHECK(doc.selection().height() == 15);
    CHECK(doc.selection().at(10, 7) == 255);

    doc.commands().undo();
    CHECK(doc.width() == 40);
    CHECK(layer->image().width == 40);
    CHECK(doc.selection().width() == 40);
}

TEST_CASE("trim to content shrinks the canvas to the visible alpha box") {
    core::Document doc(40, 30);
    auto* layer = doc.root().addOnTop(doc.makeRaster("L")).as<core::RasterLayer>();
    REQUIRE(layer != nullptr);
    for (std::uint32_t y = 5; y < 15; ++y) {
        for (std::uint32_t x = 10; x < 30; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 40 + x) * 4;
            layer->image().rgba[p] = 200;
            layer->image().rgba[p + 3] = 255;
        }
    }
    pushOp(doc, render::buildTrimToContentCommand(doc));
    CHECK(doc.width() == 20);
    CHECK(doc.height() == 10);
    CHECK(layer->transform() == common::Affine2D::translation(-10, -5));
    const common::Image out = flatten(doc);
    CHECK(px(out, 0, 0).a == 255);
    CHECK(px(out, 19, 9).a == 255);

    doc.commands().undo();
    CHECK(doc.width() == 40);
    CHECK(doc.height() == 30);
    CHECK(layer->transform() == common::Affine2D::identity());
}

TEST_CASE("a request that changes nothing returns nullptr instead of an empty undo step") {
    core::Document doc(40, 30);
    auto* layer = doc.root().addOnTop(doc.makeRaster("L")).as<core::RasterLayer>();
    REQUIRE(layer != nullptr);
    CHECK(!render::buildCanvasResizeCommand(doc, 40, 30, CanvasAnchor::Center, std::nullopt));
    CHECK(!render::buildCanvasResizeCommand(doc, 0, 30, CanvasAnchor::Center, std::nullopt));
    CHECK(!render::buildImageResizeCommand(doc, 40, 30, render::ResampleFilter::Lanczos3));
    CHECK(!render::buildImageResizeCommand(doc, 20, 0, render::ResampleFilter::Lanczos3));
    CHECK(!render::buildRotateDocumentCommand(doc, 0.0, render::ResampleFilter::Lanczos3,
                                              std::nullopt));
    CHECK(!render::buildRotateDocumentCommand(doc, 2.0 * std::numbers::pi,
                                              render::ResampleFilter::Lanczos3, std::nullopt));
    // Nothing has alpha yet: there is no content box to trim to.
    CHECK(!render::buildTrimToContentCommand(doc));
    // Content filling the canvas is already tight.
    layer->image().fill({0, 0, 0, 255});
    layer->invalidateContentBounds();
    CHECK(!render::buildTrimToContentCommand(doc));
}

TEST_CASE("an arbitrary document rotation grows the canvas to the rotated bounding box") {
    core::Document doc(40, 30);
    auto* layer = doc.root().addOnTop(doc.makeRaster("L")).as<core::RasterLayer>();
    REQUIRE(layer != nullptr);
    layer->image().fill({200, 30, 30, 255});
    doc.setSelection(core::Selection::rectangle(40, 30, {4, 4, 10, 10}));
    const std::uint64_t gid = doc.mintGuideId();
    doc.addGuide({core::Guide::Orientation::Vertical, 8.0, gid});

    const double angle = std::numbers::pi / 4.0;
    pushOp(doc, render::buildRotateDocumentCommand(doc, angle, render::ResampleFilter::Bilinear,
                                                   render::CropFill{{255, 255, 255, 255},
                                                                    "Canvas fill"}));
    // The rotated bounding box of a 40x30 rectangle at 45 degrees is (40+30)/sqrt(2) on a side.
    const auto expected = static_cast<std::uint32_t>(std::ceil(70.0 / std::numbers::sqrt2 - 1e-9));
    CHECK(doc.width() == expected);
    CHECK(doc.height() == expected);
    // The corners the old canvas never covered took the fill colour; the centre is still content.
    const common::Image out = flatten(doc);
    CHECK(px(out, 0, 0) == common::Color8{255, 255, 255, 255});
    CHECK(px(out, doc.width() / 2, doc.height() / 2).r > 100);
    // Neither the selection nor a guide survives an arbitrary rotation (see the header).
    CHECK(doc.selection().isEmpty());
    CHECK(doc.guides().empty());

    doc.commands().undo();
    CHECK(doc.width() == 40);
    CHECK(doc.height() == 30);
    CHECK_FALSE(doc.selection().isEmpty());
    CHECK(doc.guides().size() == 1);
}
