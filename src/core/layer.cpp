#include "core/layer.hpp"

#include "core/text/extrude_render.hpp" // projectedExtrudeBounds: a 3D object's reach
#include "core/vector/flatten.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace mosaic::core {

std::string_view layerKindName(LayerKind kind) {
    switch (kind) {
        case LayerKind::Group: return "Group";
        case LayerKind::Raster: return "Raster";
        case LayerKind::Vector: return "Vector";
        case LayerKind::Text: return "Text";
        case LayerKind::Adjustment: return "Adjustment";
        case LayerKind::Magic: return "Magic";
        case LayerKind::Texture: return "Texture";
    }
    return "Raster";
}

std::string_view adjustmentKindName(AdjustmentKind kind) {
    switch (kind) {
        case AdjustmentKind::BrightnessContrast: return "Brightness/Contrast";
        case AdjustmentKind::Levels: return "Levels";
        case AdjustmentKind::Curves: return "Curves";
        case AdjustmentKind::Exposure: return "Exposure";
        case AdjustmentKind::HueSaturation: return "Hue/Saturation";
        case AdjustmentKind::ColorBalance: return "Color Balance";
        case AdjustmentKind::Grayscale: return "Grayscale";
        case AdjustmentKind::Invert: return "Invert";
        case AdjustmentKind::Threshold: return "Threshold";
        case AdjustmentKind::Posterize: return "Posterize";
        case AdjustmentKind::PhotometricMatch: return "Photometric Match";
        case AdjustmentKind::GaussianBlur: return "Gaussian Blur";
        case AdjustmentKind::BoxBlur: return "Box Blur";
        case AdjustmentKind::MotionBlur: return "Motion Blur";
        case AdjustmentKind::RadialBlur: return "Radial Blur";
        case AdjustmentKind::SurfaceBlur: return "Surface Blur";
        case AdjustmentKind::LensBlur: return "Lens Blur";
        case AdjustmentKind::DofBlur: return "Depth of Field";
        case AdjustmentKind::ShadowsHighlights: return "Shadows/Highlights";
        case AdjustmentKind::Defringe: return "Defringe";
        case AdjustmentKind::MatteRemoval: return "Matte Removal";
        case AdjustmentKind::HazeRemoval: return "Haze Removal";
        case AdjustmentKind::Sharpen: return "Sharpen";
        case AdjustmentKind::UnsharpMask: return "Unsharp Mask";
        case AdjustmentKind::AddNoise: return "Add Noise";
        case AdjustmentKind::Denoise: return "Denoise";
        case AdjustmentKind::Pixelate: return "Pixelate";
        case AdjustmentKind::Emboss: return "Emboss";
        case AdjustmentKind::OilPaint: return "Oil Paint";
        case AdjustmentKind::Wave: return "Wave";
        case AdjustmentKind::Vignette: return "Vignette";
        case AdjustmentKind::GradientMap: return "Gradient Map";
        case AdjustmentKind::Vibrance: return "Vibrance";
        case AdjustmentKind::PhotoFilter: return "Photo Filter";
        case AdjustmentKind::HighPass: return "High Pass";
    }
    return "Brightness/Contrast";
}

std::uint64_t RasterLayer::alphaFingerprint() const {
    if (m_alphaHashRevision == m_contentRevision) return m_alphaHash;
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a offset basis
    for (std::size_t p = 3; p < m_image.rgba.size(); p += 4) {
        h ^= m_image.rgba[p];
        h *= 1099511628211ull;
    }
    m_alphaHash = h;
    m_alphaHashRevision = m_contentRevision;
    return h;
}

// ---- Warp (S35-b) ------------------------------------------------------------------------------

std::string_view warpKindName(WarpKind kind) {
    switch (kind) {
        case WarpKind::Mesh: return "Mesh";
        case WarpKind::Perspective: return "Perspective";
    }
    return "Mesh";
}

bool WarpGrid::valid() const {
    if (cols < 2 || rows < 2) return false;
    if (kind == WarpKind::Perspective && (cols != 2 || rows != 2)) return false;
    if (points.size() != static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows))
        return false;
    return source.w > 0.0 && source.h > 0.0;
}

common::Vec2 WarpGrid::latticePoint(int col, int row) const {
    // The uniform lattice over `source`: node (0,0) is its top-left corner and (cols-1, rows-1) its
    // bottom-right, so the outer ring of handles sits exactly ON the framed rect's edges.
    const double fx = cols > 1 ? static_cast<double>(col) / static_cast<double>(cols - 1) : 0.0;
    const double fy = rows > 1 ? static_cast<double>(row) / static_cast<double>(rows - 1) : 0.0;
    return {source.x + source.w * fx, source.y + source.h * fy};
}

common::Vec2 WarpGrid::point(int col, int row) const {
    const std::size_t i =
        static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(col);
    return i < points.size() ? points[i] : common::Vec2{};
}

bool WarpGrid::identity() const {
    if (!valid()) return false;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            if (!(point(c, r) == latticePoint(c, r))) return false;
    return true;
}

WarpGrid identityWarpGrid(const common::Rect& source, int cols, int rows, WarpKind kind) {
    WarpGrid g;
    g.kind = kind;
    // Perspective is one homography over four corners; a "3x4 perspective" is not a thing this
    // engine has, so the request is corrected here rather than refused three call sites later.
    g.cols = kind == WarpKind::Perspective ? 2 : std::max(2, cols);
    g.rows = kind == WarpKind::Perspective ? 2 : std::max(2, rows);
    g.source = source;
    g.points.resize(static_cast<std::size_t>(g.cols) * static_cast<std::size_t>(g.rows));
    for (int r = 0; r < g.rows; ++r)
        for (int c = 0; c < g.cols; ++c)
            g.points[static_cast<std::size_t>(r) * static_cast<std::size_t>(g.cols)
                     + static_cast<std::size_t>(c)] = g.latticePoint(c, r);
    return g;
}

WarpGrid identityLike(const WarpGrid& g) {
    return identityWarpGrid(g.source, g.cols, g.rows, g.kind);
}

WarpGrid translatedWarpGrid(const WarpGrid& g, common::Vec2 delta) {
    WarpGrid out = g;
    out.source.x += delta.x;
    out.source.y += delta.y;
    for (common::Vec2& p : out.points) p = p + delta;
    return out;
}

common::Affine2D parentWorldTransform(const Layer& layer) {
    common::Affine2D t = common::Affine2D::identity();
    for (const GroupLayer* p = layer.parent(); p != nullptr; p = p->parent())
        t = p->transform() * t;
    return t;
}

common::Affine2D worldTransform(const Layer& layer) {
    return parentWorldTransform(layer) * layer.transform();
}

namespace {

// The kinds whose mask sheet is their SOURCE IMAGE rather than a captured document window: their
// mask px are image px, and a mask at a different resolution stretches proportionally over it
// (what the compositor's fused fold does at the source pixel). Every mask this app builds is sized
// to its source, so the scale is the identity in practice -- it exists for hand-built masks and
// for documents whose mask resolution was reduced.
[[nodiscard]] common::Affine2D maskSourceScale(const Layer& layer, const RasterMask& mask) {
    std::uint32_t w = 0, h = 0;
    if (const auto* raster = layer.as<RasterLayer>()) {
        w = raster->image().width;
        h = raster->image().height;
    } else if (const auto* magic = layer.as<MagicLayer>()) {
        w = magic->source().width;
        h = magic->source().height;
    } else {
        return common::Affine2D::identity();
    }
    if (w == 0 || h == 0 || mask.width == 0 || mask.height == 0 ||
        (w == mask.width && h == mask.height))
        return common::Affine2D::identity();
    return common::Affine2D::scaling(static_cast<double>(w) / mask.width,
                                     static_cast<double>(h) / mask.height);
}

} // namespace

common::Affine2D maskPlacement(const Layer& layer, const RasterMask& mask) {
    // Unlinked: the sheet sits in the PARENT's space, 1 mask px per parent unit -- the source
    // scale belongs to the layer's own grid, which the unlinked sheet has stepped out of.
    if (!mask.linked)
        return mask.toLocal;
    return mask.toLocal * maskSourceScale(layer, mask);
}

common::Affine2D maskToDocument(const Layer& layer, const RasterMask& mask) {
    return (mask.linked ? worldTransform(layer) : parentWorldTransform(layer)) *
           maskPlacement(layer, mask);
}

common::Affine2D newMaskToLocal(const Layer& layer, bool linked) {
    switch (layer.kind()) {
        case LayerKind::Group:
        case LayerKind::Vector:
        case LayerKind::Adjustment:
            break; // no pixel grid of their own: the sheet is a captured document window
        case LayerKind::Raster:
        case LayerKind::Magic:
        case LayerKind::Text:
        case LayerKind::Texture:
            // The sheet IS the layer's own sampling grid (its image, or the renderer's pixel
            // cache) and the compositor folds it at the source pixel, which no placement can
            // reach. Keeping the identity is what keeps those kinds byte-for-byte unchanged.
            return common::Affine2D::identity();
    }
    const common::Affine2D w = linked ? worldTransform(layer) : parentWorldTransform(layer);
    return w.inverse().value_or(common::Affine2D::identity());
}

namespace {

// Placement equality for the partition check. The linear part is unitless (1e-9 is far below any
// edit a user can make, and comfortably above the round-trip error of composing and inverting a
// chain of group transforms); the translation is in document pixels, where a thousandth of a pixel
// is already indistinguishable and no real edit lands that close.
[[nodiscard]] bool affineNear(const common::Affine2D& a, const common::Affine2D& b) noexcept {
    constexpr double kLin = 1e-9;
    constexpr double kTrans = 1e-3;
    return std::abs(a.m00 - b.m00) <= kLin && std::abs(a.m01 - b.m01) <= kLin &&
           std::abs(a.m10 - b.m10) <= kLin && std::abs(a.m11 - b.m11) <= kLin &&
           std::abs(a.m02 - b.m02) <= kTrans && std::abs(a.m12 - b.m12) <= kTrans;
}

// What BOTH halves must still be for the pair to describe one surface. Each clause is a way the
// alpha the compositor will actually see can stop being the A*m / A*(1-m) the split produced: an
// effect redraws the silhouette, a clip multiplies in an unrelated layer's coverage, a resampling
// placement lands the halves on grids that disagree, and a pixel edit replaces the alpha outright.
[[nodiscard]] bool partitionHalfEligible(const Layer& l) noexcept {
    const auto* raster = l.as<const RasterLayer>();
    if (raster == nullptr) return false;
    if (!l.visible() || l.opacity() <= 0.0f) return false;
    if (l.clipToBelow() || (l.hasEffects() && !l.effects().empty())) return false;
    // A blend mode is a request to interact with what lies BENEATH -- and beneath the fragment
    // lies its own hole. The reconstruction reassembles the alpha correctly whatever the mode
    // (alpha compositing is blend-independent), but the COLOUR then blends against the filled
    // hole in the feather band and against the real backdrop in the fully-cut core, and that
    // discontinuity draws a ring of its own: with Subtract, a dark one. Trading the alpha rim for
    // a colour ring is not a trade, so a blended half is honoured literally instead.
    if (l.blendMode() != BlendMode::Normal) return false;
    if (!integerGridPlacement(l)) return false;
    const std::optional<CoveragePartition>& p = l.partition();
    return p.has_value() && p->token != 0 && p->selfAlphaHash == raster->alphaFingerprint();
}

// The LOWER half additionally has to reach the composite at exactly the alpha it stores: it is the
// half being rewritten, so anything scaling it afterwards would scale the rewrite too and the two
// halves would no longer add up.
[[nodiscard]] bool partitionLowerEligible(const Layer& l) noexcept {
    return partitionHalfEligible(l) && l.opacity() >= 1.0f && !l.hasMask();
}

// The UPPER half may carry a linked mask -- the compositor folds it into the fragment's effective
// alpha exactly as it would when rendering it. An UNLINKED mask folds in parent space AFTER
// placement, which the per-pixel read cannot mirror, so that one does retire the pair.
[[nodiscard]] bool partitionUpperEligible(const Layer& l) noexcept {
    if (!partitionHalfEligible(l)) return false;
    const RasterMask* m = l.mask();
    return m == nullptr || !m->enabled || m->linked;
}

}  // namespace

bool integerGridPlacement(const Layer& layer) {
    const common::Affine2D t = worldTransform(layer);
    const auto isInt = [](double v) { return std::abs(v - std::round(v)) < 1e-9; };
    return t.m00 == 1.0 && t.m01 == 0.0 && t.m10 == 0.0 && t.m11 == 1.0 && isInt(t.m02) &&
           isInt(t.m12);
}

bool partitionPairLive(const Layer& lower, const Layer& upper) {
    if (&lower == &upper) return false;
    if (!partitionLowerEligible(lower) || !partitionUpperEligible(upper)) return false;
    const CoveragePartition& a = *lower.partition();
    const CoveragePartition& b = *upper.partition();
    // The two must name each other under one token: a half whose partner was deleted and whose id
    // was later reused must not re-pair with a stranger.
    if (a.token != b.token || a.partner != upper.id() || b.partner != lower.id()) return false;

    const common::Affine2D wl = worldTransform(lower);
    const common::Affine2D wu = worldTransform(upper);
    const std::optional<common::Affine2D> il = wl.inverse();
    const std::optional<common::Affine2D> iu = wu.inverse();
    if (!il || !iu) return false;
    return affineNear(*il * wu, a.relToPartner) && affineNear(*iu * wl, b.relToPartner);
}

namespace {

// Root -> `layer` (exclusive of `layer` itself), i.e. every group it hangs under, outermost first.
[[nodiscard]] std::vector<const GroupLayer*> ancestry(const Layer& layer) {
    std::vector<const GroupLayer*> chain;
    for (const GroupLayer* p = layer.parent(); p != nullptr; p = p->parent()) chain.push_back(p);
    std::reverse(chain.begin(), chain.end());
    return chain;
}

// A group that neither reshapes nor re-blends what it wraps, so its buffer composites with plain
// `over` and the reconstruction still holds across it. Opacity is handled by the caller: for
// `over` with Normal, a group's opacity is exactly a scaling of the alpha it contains, so on the
// fragment's side it can be folded into the effective alpha rather than disqualifying the pair.
[[nodiscard]] bool passThrough(const GroupLayer& g) noexcept {
    return g.visible() && g.opacity() > 0.0f && g.blendMode() == BlendMode::Normal &&
           !g.clipToBelow() && !g.hasMask() && !(g.hasEffects() && !g.effects().empty());
}

}  // namespace

LivePartition partitionReachable(const Layer& lower, const Layer& upper) {
    const std::vector<const GroupLayer*> a = ancestry(lower);
    const std::vector<const GroupLayer*> b = ancestry(upper);
    // The deepest group that contains both. Two layers in one document always share the root, so
    // `common` only stays 0 for a detached layer -- which is never reachable.
    std::size_t common = 0;
    while (common < a.size() && common < b.size() && a[common] == b[common]) ++common;
    if (common == 0) return {};
    const GroupLayer& meet = *a[common - 1];

    // Which child of `meet` each half descends through, and in what order they composite.
    const Layer* lowerBranch = common < a.size() ? static_cast<const Layer*>(a[common]) : &lower;
    const Layer* upperBranch = common < b.size() ? static_cast<const Layer*>(b[common]) : &upper;
    if (lowerBranch == upperBranch) return {};  // same branch: `common` was not the meet
    const std::size_t li = meet.indexOf(lowerBranch->id());
    const std::size_t ui = meet.indexOf(upperBranch->id());
    if (li == GroupLayer::npos || ui == GroupLayer::npos || li >= ui)
        return {};  // the fragment must composite AFTER the hole it fills

    // Every wrapper strictly between each half and the meet must pass its content through. On the
    // LOWER side that includes staying fully opaque -- it is the half being rewritten.
    for (std::size_t i = common; i < a.size(); ++i)
        if (!passThrough(*a[i]) || a[i]->opacity() < 1.0f) return {};
    float scale = upper.opacity();
    for (std::size_t i = common; i < b.size(); ++i) {
        if (!passThrough(*b[i])) return {};
        scale *= b[i]->opacity();
    }
    return LivePartition{&upper, scale};
}

namespace {

// Depth-first search for `id` under `group` (the partner lives somewhere in the same tree, but not
// necessarily anywhere near the layer that names it).
[[nodiscard]] const Layer* findById(const GroupLayer& group, LayerId id) {
    for (std::size_t i = 0; i < group.childCount(); ++i) {
        const Layer& c = group.child(i);
        if (c.id() == id) return &c;
        if (const auto* g = c.as<const GroupLayer>())
            if (const Layer* hit = findById(*g, id)) return hit;
    }
    return nullptr;
}

}  // namespace

LivePartition livePartitionFor(const Layer& layer) {
    const std::optional<CoveragePartition>& p = layer.partition();
    if (!p.has_value() || p->partner == kInvalidLayerId) return {};
    const GroupLayer* root = layer.parent();
    if (root == nullptr) return {};
    while (root->parent() != nullptr) root = root->parent();
    const Layer* partner = findById(*root, p->partner);
    if (partner == nullptr) return {};
    if (!partitionPairLive(layer, *partner)) return {};
    return partitionReachable(layer, *partner);
}

void linkCoveragePartition(Layer& a, Layer& b) {
    // Monotonic per process. Tokens only ever need to distinguish partitions that coexist in one
    // document, and they are re-checked against the partner id, so wrap-around is not a concern.
    static std::uint64_t nextToken = 0;
    const common::Affine2D wa = worldTransform(a);
    const common::Affine2D wb = worldTransform(b);
    const std::optional<common::Affine2D> ia = wa.inverse();
    const std::optional<common::Affine2D> ib = wb.inverse();
    if (!ia || !ib) return;
    const auto* ra = a.as<const RasterLayer>();
    const auto* rb = b.as<const RasterLayer>();
    if (ra == nullptr || rb == nullptr) return;
    const std::uint64_t token = ++nextToken;
    a.setPartition(CoveragePartition{b.id(), token, ra->alphaFingerprint(), *ia * wb});
    b.setPartition(CoveragePartition{a.id(), token, rb->alphaFingerprint(), *ib * wa});
}

std::optional<common::Rect> GroupLayer::contentBounds() const {
    std::optional<common::Rect> acc;
    for (const auto& child : children()) {
        if (!child->visible())
            continue;
        const std::optional<common::Rect> cb = child->contentBounds();
        if (!cb)
            continue;
        const common::Rect mapped = child->transform().mapBounds(*cb);
        acc = acc ? acc->united(mapped) : mapped;
    }
    return acc;
}

std::optional<common::Rect> RasterLayer::contentBounds() const {
    if (!m_boundsValid) {
        m_contentBounds = common::alphaBounds(m_image);
        m_boundsValid = true;
    }
    return m_contentBounds;
}

std::optional<common::Rect> MagicLayer::contentBounds() const {
    if (!m_boundsValid) {
        m_contentBounds = common::alphaBounds(m_source);
        m_boundsValid = true;
    }
    return m_contentBounds;
}

std::optional<common::Rect> VectorLayer::contentBounds() const {
    if (!m_boundsValid) {
        m_contentBounds = m_object ? vec::contentBounds(*m_object) : std::nullopt;
        // An EXTRUDED object reaches outside its own outline: the solid has depth, it is rotated,
        // and its bevels bulge. Swell the flat box to the projected extent of the solid, exactly
        // as the text lane sizes its cache (text::projectedExtrudeBounds), or the Move gizmo would
        // frame the path while the render spills past it.
        if (m_contentBounds && m_object && m_object->extrude)
            m_contentBounds = text::projectedExtrudeBounds(*m_contentBounds, *m_object->extrude);
        m_boundsValid = true;
    }
    return m_contentBounds;
}

Layer& GroupLayer::insert(std::size_t index, std::unique_ptr<Layer> layer) {
    if (index > m_children.size()) index = m_children.size();
    layer->m_parent = this;
    Layer& ref = *layer;
    m_children.insert(m_children.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
    return ref;
}

std::unique_ptr<Layer> GroupLayer::removeAt(std::size_t index) {
    if (index >= m_children.size()) return nullptr;
    std::unique_ptr<Layer> layer = std::move(m_children[index]);
    m_children.erase(m_children.begin() + static_cast<std::ptrdiff_t>(index));
    layer->m_parent = nullptr;
    return layer;
}

std::size_t GroupLayer::indexOf(LayerId id) const noexcept {
    for (std::size_t i = 0; i < m_children.size(); ++i) {
        if (m_children[i]->id() == id) return i;
    }
    return npos;
}

}  // namespace mosaic::core
