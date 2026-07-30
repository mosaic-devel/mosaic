#include "io/document_profile.hpp"

#include "io/exif_write.hpp"  // exifForExport -- the orientation-is-1 rule

#include "core/color_management.hpp" // documentIccProfile -- the profile bytes to embed
#include "core/layer.hpp"
#include "core/layer_effects.hpp"
#include "core/text/text_model.hpp"
#include "core/vector/object.hpp"
#include "core/vector/paint.hpp"

#include <memory>
#include <unordered_set>
#include <variant>

namespace mosaic::io {

namespace {

using core::Layer;
using core::LayerKind;

[[nodiscard]] bool isIdentity(const common::Affine2D& t) noexcept {
    return t.m00 == 1.0 && t.m01 == 0.0 && t.m02 == 0.0 && t.m10 == 0.0 && t.m11 == 1.0 &&
           t.m12 == 0.0;
}

// A gradient paint whose type is Conic, anywhere in a Paint variant.
[[nodiscard]] bool paintIsConic(const core::vec::Paint& paint) noexcept {
    const core::vec::Gradient* g = std::get_if<core::vec::Gradient>(&paint);
    return g != nullptr && g->type == core::vec::GradientType::Conic;
}

void probeVector(const core::VectorLayer& layer, DocumentProfile& p) {
    const core::vec::Object* obj = layer.object();
    if (obj == nullptr)
        return;
    p.hasVector = true;
    if (paintIsConic(obj->fill) || paintIsConic(obj->stroke.paint))
        p.usesConicGradient = true;
    if (obj->stroke.enabled) {
        if (obj->stroke.align != core::vec::StrokeAlign::Center)
            p.usesStrokeAlign = true;
        if (!obj->stroke.dashArray.empty())
            p.usesDashes = true;
    }
}

void probeText(const core::TextLayer& layer, DocumentProfile& p) {
    const core::text::TextBlock& block = layer.block();
    if (!block.empty())
        p.hasText = true;
    if (block.extrude.has_value())
        p.hasExtrude3d = true;
}

// A mask is "soft" when its coverage carries anything between fully hidden and fully shown --
// that is what a 1-bit-alpha target (GIF) cannot represent.
[[nodiscard]] bool maskIsSoft(const core::RasterMask& mask) noexcept {
    if (!mask.enabled)
        return false;
    for (const std::uint8_t v : mask.coverage)
        if (v != 0 && v != 255)
            return true;
    return false;
}

void probeLayer(const Layer& layer, DocumentProfile& p) {
    ++p.layerCount;

    if (layer.blendMode() != core::BlendMode::Normal || layer.opacity() < 1.0f)
        p.usesBlendModes = true;
    if (layer.hasEffects() && !layer.effects().empty())
        p.hasEffects = true;
    if (const core::RasterMask* m = layer.mask(); m != nullptr && maskIsSoft(*m))
        p.usesSoftMask = true;
    if (const auto& exif = layer.exif(); exif.has_value() && exif->hasAny())
        p.hasEXIF = true;

    switch (layer.kind()) {
    case LayerKind::Vector:
        if (const auto* v = layer.as<core::VectorLayer>())
            probeVector(*v, p);
        break;
    case LayerKind::Text:
        if (const auto* t = layer.as<core::TextLayer>())
            probeText(*t, p);
        break;
    case LayerKind::Adjustment:
        p.hasAdjustments = true;
        break;
    case LayerKind::Group:
    case LayerKind::Raster:
    case LayerKind::Magic:
    case LayerKind::Texture:
        break;
    }

    if (const auto* group = layer.as<core::GroupLayer>())
        for (const std::unique_ptr<Layer>& child : group->children())
            probeLayer(*child, p);
}

// Does this direct child of the root seal the canvas opaquely? (See the header for why alpha
// only ever grows, which is what makes one such layer sufficient.)
[[nodiscard]] bool sealsCanvas(const Layer& layer, std::uint32_t docW, std::uint32_t docH) noexcept {
    const auto* raster = layer.as<core::RasterLayer>();
    if (raster == nullptr)
        return false;
    if (!layer.visible() || layer.opacity() < 1.0f || layer.blendMode() != core::BlendMode::Normal)
        return false;
    if (layer.clipToBelow() || layer.hasMask())
        return false;
    if (layer.hasEffects() && !layer.effects().empty())
        return false;  // fill-opacity and inner effects both rewrite the layer's own alpha
    if (!isIdentity(layer.transform()))
        return false;

    const common::Image& img = raster->image();
    if (img.width < docW || img.height < docH || docW == 0 || docH == 0)
        return false;
    if (img.rgba.size() < img.pixelCount() * 4)
        return false;
    for (std::uint32_t y = 0; y < docH; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * img.width * 4;
        for (std::uint32_t x = 0; x < docW; ++x)
            if (img.rgba[row + static_cast<std::size_t>(x) * 4 + 3] != 255)
                return false;
    }
    return true;
}

// Narrow `best` towards the provenance layer (see documentExif's contract in the header):
// effectively-visible Raster/Magic layers carrying a record, earliest-minted id wins. `best` is a
// pointer to the winner so far, so one walk answers without allocating a candidate list.
void narrowExifProvenance(const Layer& layer, bool parentVisible, const Layer** best) {
    const bool visible = parentVisible && layer.visible();
    if (const auto* group = layer.as<core::GroupLayer>()) {
        // A group carries no metadata of its own, but its visibility gates every descendant's.
        for (const std::unique_ptr<Layer>& child : group->children())
            narrowExifProvenance(*child, visible, best);
        return;
    }
    if (!visible)
        return;
    // Raster and Magic only: those are the two kinds the open path stamps, and a text or
    // adjustment layer holding EXIF would mean something has gone wrong upstream, not that the
    // document descends from a photograph.
    if (layer.kind() != LayerKind::Raster && layer.kind() != LayerKind::Magic)
        return;
    const std::optional<common::ExifData>& exif = layer.exif();
    if (!exif.has_value() || !exif->hasAny())
        return;
    if (*best == nullptr || layer.id() < (*best)->id())
        *best = &layer;
}

} // namespace

std::optional<common::ExifData> documentExif(const core::Document& doc) {
    const Layer* best = nullptr;
    for (const std::unique_ptr<Layer>& child : doc.root().children())
        narrowExifProvenance(*child, /*parentVisible=*/true, &best);
    if (best == nullptr)
        return std::nullopt;
    return exifForExport(*best->exif());
}

EmbeddedMetadata documentMetadata(const core::Document& doc, bool keepMetadata) {
    EmbeddedMetadata out;
    if (!keepMetadata)
        return out;  // the privacy toggle: no EXIF, no profile, and no density either
    if (const std::optional<common::ExifData> exif = documentExif(doc))
        out.exif = buildExifPayload(*exif);  // already export-normalised by documentExif
    out.icc = core::documentIccProfile(doc);
    out.dpi = doc.dpi();
    return out;
}

bool imageHasTransparency(const common::Image& image) noexcept {
    if (image.empty() || image.rgba.size() < image.pixelCount() * 4)
        return false;
    for (std::size_t i = 3; i < image.pixelCount() * 4; i += 4)
        if (image.rgba[i] != 255)
            return true;
    return false;
}

int countDistinctColors(const common::Image& image, int limit) {
    if (image.empty() || image.rgba.size() < image.pixelCount() * 4)
        return 0;
    if (limit <= 0)
        return -1;
    std::unordered_set<std::uint32_t> seen;
    seen.reserve(static_cast<std::size_t>(limit) + 1u);
    const std::size_t n = image.pixelCount();
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t p = i * 4;
        const std::uint32_t packed = static_cast<std::uint32_t>(image.rgba[p]) |
                                     (static_cast<std::uint32_t>(image.rgba[p + 1]) << 8) |
                                     (static_cast<std::uint32_t>(image.rgba[p + 2]) << 16) |
                                     (static_cast<std::uint32_t>(image.rgba[p + 3]) << 24);
        seen.insert(packed);
        if (seen.size() > static_cast<std::size_t>(limit))
            return -1;
    }
    return static_cast<int>(seen.size());
}

bool documentIsProvablyOpaque(const core::Document& doc) noexcept {
    const core::GroupLayer& root = doc.root();
    for (const std::unique_ptr<Layer>& child : root.children())
        if (sealsCanvas(*child, doc.width(), doc.height()))
            return true;
    return false;
}

DocumentProfile profileDocument(const core::Document& doc) {
    DocumentProfile p;
    for (const std::unique_ptr<Layer>& child : doc.root().children())
        probeLayer(*child, p);
    p.hasMultipleLayers = p.layerCount > 1;

    p.precision = doc.precision();
    p.dpi = doc.dpi();
    p.hasICC = !doc.iccProfilePath().empty();
    p.hasNonSrgbSpace = doc.colorSpace() != core::ColorSpace::SRGB;
    // The export source is still the 8-bit flatten (§5 high-bit note): say so honestly rather
    // than promising the encoder depth the pipeline cannot deliver.
    p.sourceBitDepth = 8;
    p.sourceIsFloat = false;

    p.hasAlpha = !documentIsProvablyOpaque(doc);
    p.distinctColors = -1;  // uncounted without a flatten
    return p;
}

DocumentProfile profileDocument(const core::Document& doc, const common::Image& flattened,
                                int colorCountLimit) {
    DocumentProfile p = profileDocument(doc);
    p.hasAlpha = imageHasTransparency(flattened);
    p.distinctColors = countDistinctColors(flattened, colorCountLimit);
    return p;
}

} // namespace mosaic::io
