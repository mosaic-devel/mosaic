#include "core/stroke_confinement.hpp"

#include "core/layer_grow.hpp" // pixelBoxCovering -- the saturating double->integer corner
#include "core/selection.hpp"

#include <algorithm>
#include <optional>

namespace mosaic::core {

std::shared_ptr<const StrokeConfinement> makeStrokeConfinement(const Selection& sel,
                                                               const common::Affine2D& targetToDoc,
                                                               std::uint32_t targetW,
                                                               std::uint32_t targetH) {
    // NO SELECTION = everything editable. Returning nothing (rather than an all-255 field) is what
    // keeps an unconfined stroke byte-identical: the engine never looks at a null field, so not one
    // multiply, lookup or branch of confinement runs.
    if (sel.isEmpty())
        return nullptr;

    auto out = std::make_shared<StrokeConfinement>();
    if (targetW == 0 || targetH == 0)
        return out;
    // An ACTIVE selection covering nothing is not "no selection": the stroke must deposit nothing.
    // An empty window does exactly that -- at() reads 0 everywhere outside it, and there is no
    // inside.
    const std::optional<common::Rect> selBounds = sel.bounds();
    if (!selBounds)
        return out;
    // Only the WINDOW needs the inverse (a document-space bbox pulled back into the target grid);
    // the sampling itself runs FORWARD, target pixel centre -> document, exactly as
    // maskFromSelection and the compositor's leaf walk do. A singular placement has neither.
    const std::optional<common::Affine2D> docToTarget = targetToDoc.inverse();
    if (!docToTarget)
        return out; // singular placement: no pixel of this target sits in the document at all

    // The 1:1 fast path -- an untransformed layer on the document's own grid, which is what a
    // freshly created layer (and the mask of one) looks like. Identical answer, no per-pixel
    // transform: the same shortcut maskFromSelection takes, for the same reason. The window is the
    // selection's tight bounds, so a small marquee on a big canvas still costs its own box.
    const bool oneToOne = targetToDoc == common::Affine2D::identity() &&
                          targetW == sel.width() && targetH == sel.height();

    // The window: the selection's bounds pulled back into the target grid, dilated by a pixel (the
    // pull-back is the bounding box of a possibly rotated rect, and the sampling reads pixel
    // CENTRES), then clipped to the target. pixelBoxCovering saturates its corners before they
    // become integers, so an extreme placement cannot produce a corner the cast does not define.
    common::Rect want = *selBounds;
    if (!oneToOne)
        want = docToTarget->mapBounds(want);
    const PixelBox box = pixelBoxCovering(
        common::Rect{want.x - 1.0, want.y - 1.0, want.w + 2.0, want.h + 2.0});
    if (box.empty())
        return out;
    const long x0 = std::max<long>(box.x0, 0);
    const long y0 = std::max<long>(box.y0, 0);
    const long x1 = std::min<long>(box.x1, static_cast<long>(targetW));
    const long y1 = std::min<long>(box.y1, static_cast<long>(targetH));
    if (x1 <= x0 || y1 <= y0)
        return out; // the selection misses this target entirely

    out->x = static_cast<std::int32_t>(x0);
    out->y = static_cast<std::int32_t>(y0);
    out->w = static_cast<std::uint32_t>(x1 - x0);
    out->h = static_cast<std::uint32_t>(y1 - y0);
    out->v.assign(static_cast<std::size_t>(out->w) * out->h, 0);

    if (oneToOne) {
        for (std::uint32_t ry = 0; ry < out->h; ++ry) {
            std::uint8_t* row = out->v.data() + static_cast<std::size_t>(ry) * out->w;
            const auto sy = static_cast<std::uint32_t>(y0 + static_cast<long>(ry));
            for (std::uint32_t rx = 0; rx < out->w; ++rx)
                row[rx] = sel.at(static_cast<std::uint32_t>(x0 + static_cast<long>(rx)), sy);
        }
        return out;
    }

    const double sw = static_cast<double>(sel.width());
    const double sh = static_cast<double>(sel.height());
    for (std::uint32_t ry = 0; ry < out->h; ++ry) {
        std::uint8_t* row = out->v.data() + static_cast<std::size_t>(ry) * out->w;
        const double ty = static_cast<double>(y0 + static_cast<long>(ry)) + 0.5;
        for (std::uint32_t rx = 0; rx < out->w; ++rx) {
            const common::Vec2 p =
                targetToDoc.apply({static_cast<double>(x0 + static_cast<long>(rx)) + 0.5, ty});
            // The range test doubles as the cast's guard: it is written on the DOUBLES, so a NaN
            // (every comparison false) and an out-of-range magnitude both fall through to 0 instead
            // of reaching an integer conversion that has no defined result. For p >= 0 the
            // truncation IS the floor.
            if (p.x >= 0.0 && p.y >= 0.0 && p.x < sw && p.y < sh)
                row[rx] = sel.at(static_cast<std::uint32_t>(p.x), static_cast<std::uint32_t>(p.y));
        }
    }
    return out;
}

} // namespace mosaic::core
