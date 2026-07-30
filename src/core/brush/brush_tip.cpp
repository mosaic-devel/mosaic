#include "core/brush/brush_tip.hpp"

#include <atomic>
#include <utility>

namespace mosaic::core::brush {

std::uint64_t nextTipId() noexcept {
    // Starts at 1: 0 is the id a default-constructed DabRequest carries, and a real tip must never
    // collide with it.
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const BrushTip> makeTip(MaskGeneratorParams generator) {
    auto tip = std::make_shared<BrushTip>();
    tip->shape = std::move(generator);
    tip->id = nextTipId();
    return tip;
}

std::shared_ptr<const BrushTip> makeTip(std::shared_ptr<const BitmapTip> bitmap) {
    auto tip = std::make_shared<BrushTip>();
    tip->shape = std::move(bitmap);
    tip->id = nextTipId();
    return tip;
}

DabShape tipDabShape(const BrushTip& tip, int frame, double diameter, double ratio, double angleRad,
                     bool mirrorH, bool mirrorV) noexcept {
    if (const BitmapTip* bmp = tip.bitmap(); bmp != nullptr)
        return bitmapDabShape(*bmp, frame, diameter, ratio, angleRad, mirrorH, mirrorV);

    // A procedural tip fills the dab's ellipse exactly -- the generator IS the shape, so its extents
    // are the dab's. (`shapeOf(gen, ...)` would return this same pair from a constructed generator;
    // deriving it here means the engine does not build one just to ask how big it is.)
    DabShape s;
    s.width = diameter;
    s.height = diameter * ratio;
    s.angleRad = angleRad;
    s.mirrorH = mirrorH;
    s.mirrorV = mirrorV;
    return s;
}

DabMask renderTipMask(const BrushTip& tip, int frame, const DabShape& shape, double softness,
                      double subX, double subY) {
    if (const BitmapTip* bmp = tip.bitmap(); bmp != nullptr)
        return renderDabMask(*bmp, frame, shape, subX, subY);

    const MaskGeneratorParams* authored = tip.generator();
    if (authored == nullptr)
        return {};

    // The generator the DAB asks for: the authored falloff, at the dab's own geometry. The ratio is
    // derived from the shape's two extents rather than carried alongside them, because the shape is
    // what the cache's KEY decodes to -- a ratio taken from the request could name a mask the key
    // does not, and the cache would stop being transparent. (The same idiom the dab-cache tests use.)
    MaskGeneratorParams p = *authored;
    p.diameter = shape.width;
    p.ratio = shape.width > 0.0 ? shape.height / shape.width : 0.0;
    p.softness = softness;

    const MaskGenerator gen(p);
    return renderDabMask(gen, shape.angleRad, shape.mirrorH, shape.mirrorV, subX, subY);
}

} // namespace mosaic::core::brush
