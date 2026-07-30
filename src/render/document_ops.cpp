#include "render/document_ops.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "common/geometry.hpp"
#include "core/commands.hpp"
#include "core/layer.hpp"

namespace mosaic::render {
namespace {

// Union of every VISIBLE layer's content box, mapped into DOCUMENT space (Trim to Content). A
// group contributes through its children rather than its own contentBounds() so the ancestor
// chain's transforms compose the same way the compositor's walk composes them, and an invisible
// group prunes its whole subtree. Kinds with no measured content (an adjustment layer, an
// unrendered text/texture layer) report nullopt and simply do not contribute.
void accumulateContentBounds(const core::Layer& layer, const common::Affine2D& parentToDoc,
                             std::optional<common::Rect>& acc) {
    if (!layer.visible())
        return;
    const common::Affine2D toDoc = parentToDoc * layer.transform();
    if (const auto* group = layer.as<core::GroupLayer>()) {
        for (const std::unique_ptr<core::Layer>& child : group->children())
            accumulateContentBounds(*child, toDoc, acc);
        return;
    }
    const std::optional<common::Rect> cb = layer.contentBounds();
    if (!cb || cb->empty())
        return;
    const common::Rect box = toDoc.mapBounds(*cb);
    acc = acc ? acc->united(box) : box;
}

}  // namespace

CanvasRect canvasRectFor(std::uint32_t oldW, std::uint32_t oldH, std::uint32_t newW,
                         std::uint32_t newH, CanvasAnchor anchor) {
    const long dw = static_cast<long>(newW) - static_cast<long>(oldW);
    const long dh = static_cast<long>(newH) - static_cast<long>(oldH);
    const int col = static_cast<int>(anchor) % 3;  // 0 = left, 1 = centre, 2 = right
    const int row = static_cast<int>(anchor) / 3;  // 0 = top,  1 = middle, 2 = bottom
    // Integer division truncates TOWARD ZERO, which is the documented rounding rule: the odd pixel
    // of an odd difference always lands on the right / bottom, growing and shrinking alike.
    const long x = col == 0 ? 0L : (col == 1 ? dw / 2 : dw);
    const long y = row == 0 ? 0L : (row == 1 ? dh / 2 : dh);
    return {x, y, oldW, oldH};
}

std::unique_ptr<core::Command> buildCanvasResizeCommand(core::Document& doc, std::uint32_t newW,
                                                        std::uint32_t newH, CanvasAnchor anchor,
                                                        const std::optional<CropFill>& fill) {
    if (newW == 0 || newH == 0)
        return nullptr;
    if (newW == doc.width() && newH == doc.height())
        return nullptr; // a no-op re-frame is not an undo step
    const CanvasRect r = canvasRectFor(doc.width(), doc.height(), newW, newH, anchor);
    // The old canvas's top-left lands at (r.x, r.y) in the new canvas, so the remap is exactly
    // that whole-pixel translation -- which is what keeps the engine's lossless extend-in-place
    // Background path and the exact selection crop available here.
    return buildDocumentRemapCommand(
        doc, newW, newH,
        common::Affine2D::translation(static_cast<double>(r.x), static_cast<double>(r.y)),
        /*deletePixels=*/false, fill, "Canvas Size");
}

std::unique_ptr<core::Command> buildImageResizeCommand(core::Document& doc, std::uint32_t newW,
                                                       std::uint32_t newH, ResampleFilter filter) {
    if (newW == 0 || newH == 0 || doc.width() == 0 || doc.height() == 0)
        return nullptr;
    if (newW == doc.width() && newH == doc.height())
        return nullptr;
    const common::Affine2D scale =
        common::Affine2D::scaling(static_cast<double>(newW) / static_cast<double>(doc.width()),
                                  static_cast<double>(newH) / static_cast<double>(doc.height()));
    // deletePixels is what makes `filter` mean something: it bakes the plain unmasked rasters at
    // the new resolution through that kernel (and leaves everything else resolution-independent on
    // the scaled transform) -- see the header's split.
    return buildDocumentRemapCommand(doc, newW, newH, scale, /*deletePixels=*/true, std::nullopt,
                                     "Image Size", resolveFilter(filter, scale));
}

std::unique_ptr<core::Command> buildOrientCommand(core::Document& doc, DocOrient op) {
    if (doc.width() == 0 || doc.height() == 0)
        return nullptr;
    const double w = static_cast<double>(doc.width());
    const double h = static_cast<double>(doc.height());
    std::uint32_t newW = doc.width();
    std::uint32_t newH = doc.height();
    common::Affine2D remap;  // {m00, m01, m02, m10, m11, m12}
    const char* label = "";
    switch (op) {
        case DocOrient::Rotate90CW:
            // (x,y) -> (H - y, x): the old top-left pixel becomes the new top-right one.
            newW = doc.height();
            newH = doc.width();
            remap = common::Affine2D{0.0, -1.0, h, 1.0, 0.0, 0.0};
            label = "Rotate 90 CW";
            break;
        case DocOrient::Rotate90CCW:
            // (x,y) -> (y, W - x): the old top-left pixel becomes the new bottom-left one.
            newW = doc.height();
            newH = doc.width();
            remap = common::Affine2D{0.0, 1.0, 0.0, -1.0, 0.0, w};
            label = "Rotate 90 CCW";
            break;
        case DocOrient::Rotate180:
            remap = common::Affine2D{-1.0, 0.0, w, 0.0, -1.0, h};
            label = "Rotate 180";
            break;
        case DocOrient::FlipHorizontal:
            remap = common::Affine2D{-1.0, 0.0, w, 0.0, 1.0, 0.0};
            label = "Flip Horizontal";
            break;
        case DocOrient::FlipVertical:
            remap = common::Affine2D{1.0, 0.0, 0.0, 0.0, -1.0, h};
            label = "Flip Vertical";
            break;
    }
    // Every matrix above is a signed axis permutation with an integer translation, so the placement
    // is a lossless grid one: no bake (deletePixels stays false -- nothing would be gained by
    // rewriting the pixels) and no fill (the turned canvas covers itself exactly).
    return buildDocumentRemapCommand(doc, newW, newH, remap, /*deletePixels=*/false, std::nullopt,
                                     label);
}

std::unique_ptr<core::Command> buildRotateDocumentCommand(core::Document& doc, double angleRad,
                                                          ResampleFilter filter,
                                                          const std::optional<CropFill>& fill) {
    if (doc.width() == 0 || doc.height() == 0)
        return nullptr;
    const double turns = angleRad / (2.0 * std::numbers::pi);
    if (std::abs(turns - std::round(turns)) < 1e-12)
        return nullptr; // a whole number of turns leaves the document exactly as it is
    const double w = static_cast<double>(doc.width());
    const double h = static_cast<double>(doc.height());
    const common::Affine2D rot = common::Affine2D::translation(w * 0.5, h * 0.5) *
                                 common::Affine2D::rotation(angleRad) *
                                 common::Affine2D::translation(-w * 0.5, -h * 0.5);
    const common::Rect bb = rot.mapBounds(common::Rect{0.0, 0.0, w, h});
    // The canvas grows to the rotated bounding box, rounded out to whole pixels; the sub-pixel
    // slack between ceil(bb) and bb sits at the right / bottom edge (the epsilon keeps an exact
    // integer box from gaining a spurious extra column).
    const auto newW = static_cast<std::uint32_t>(std::ceil(bb.w - 1e-9));
    const auto newH = static_cast<std::uint32_t>(std::ceil(bb.h - 1e-9));
    if (newW == 0 || newH == 0)
        return nullptr;
    const common::Affine2D remap = common::Affine2D::translation(-bb.x, -bb.y) * rot;
    return buildDocumentRemapCommand(doc, newW, newH, remap, /*deletePixels=*/true, fill,
                                     "Rotate Canvas", resolveFilter(filter, remap));
}

std::unique_ptr<core::Command> buildTrimToContentCommand(core::Document& doc) {
    if (doc.width() == 0 || doc.height() == 0)
        return nullptr;
    std::optional<common::Rect> box;
    for (const std::unique_ptr<core::Layer>& child : doc.root().children())
        accumulateContentBounds(*child, common::Affine2D::identity(), box);
    if (!box)
        return nullptr; // nothing visible has content: there is nothing to trim to
    // Whole pixels, clamped to the canvas -- Trim only ever shrinks (header contract).
    const long x0 = std::max<long>(0, static_cast<long>(std::floor(box->x)));
    const long y0 = std::max<long>(0, static_cast<long>(std::floor(box->y)));
    const long x1 = std::min<long>(doc.width(), static_cast<long>(std::ceil(box->right())));
    const long y1 = std::min<long>(doc.height(), static_cast<long>(std::ceil(box->bottom())));
    if (x1 <= x0 || y1 <= y0)
        return nullptr; // the content lies entirely off-canvas: refuse rather than empty the doc
    if (x0 == 0 && y0 == 0 && x1 == static_cast<long>(doc.width()) &&
        y1 == static_cast<long>(doc.height()))
        return nullptr; // already tight
    return buildDocumentRemapCommand(
        doc, static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0),
        common::Affine2D::translation(static_cast<double>(-x0), static_cast<double>(-y0)),
        /*deletePixels=*/false, std::nullopt, "Trim to Content");
}

}  // namespace mosaic::render
