#include "io/caps.hpp"

#include "common/charconv_compat.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::io {

namespace {

void add(std::vector<LossWarning>& out, Severity sev, LossCode code, std::string feature,
         std::string consequence) {
    out.push_back(LossWarning{sev, code, std::move(feature), std::move(consequence)});
}

} // namespace

std::string_view lossCodeName(LossCode code) noexcept {
    switch (code) {
    case LossCode::AlphaDropped: return "AlphaDropped";
    case LossCode::AlphaReducedToBinary: return "AlphaReducedToBinary";
    case LossCode::LayersFlattened: return "LayersFlattened";
    case LossCode::VectorRasterized: return "VectorRasterized";
    case LossCode::TextRasterized: return "TextRasterized";
    case LossCode::EffectsBaked: return "EffectsBaked";
    case LossCode::Extrude3dBaked: return "Extrude3dBaked";
    case LossCode::AdjustmentsBaked: return "AdjustmentsBaked";
    case LossCode::BitDepthReduced: return "BitDepthReduced";
    case LossCode::HdrClipped: return "HdrClipped";
    case LossCode::ColorsQuantized: return "ColorsQuantized";
    case LossCode::LossyEncode: return "LossyEncode";
    case LossCode::ChromaSubsampled: return "ChromaSubsampled";
    case LossCode::IccDropped: return "IccDropped";
    case LossCode::ColorSpaceConverted: return "ColorSpaceConverted";
    case LossCode::ExifDropped: return "ExifDropped";
    case LossCode::XmpDropped: return "XmpDropped";
    case LossCode::DpiDropped: return "DpiDropped";
    case LossCode::ConicGradientRasterized: return "ConicGradientRasterized";
    case LossCode::StrokeAlignOutlined: return "StrokeAlignOutlined";
    case LossCode::BlendModesFlattened: return "BlendModesFlattened";
    }
    return "?";
}

bool encodeIsLossless(const FormatCaps& caps, const OptionValues& values) noexcept {
    if (!caps.lossless)
        return false;  // JPEG: no lossless mode exists, so no option setting can reach one
    if (!caps.lossy)
        return true;   // PNG, QOI, BMP: no lossy mode exists
    // Checked BEFORE the lossless flag on purpose: libwebp's near-lossless is a preprocessing
    // pass that runs INSIDE the lossless mode, so `lossless = true` plus `near-lossless < 100`
    // is a lossy encode wearing a lossless label.
    if (values.has(kOptNearLossless) && values.integer(kOptNearLossless, 100) < 100)
        return false;
    if (values.has(kOptLossless))
        return values.boolean(kOptLossless, false);
    if (values.has(kOptDistance))
        return values.number(kOptDistance, 1.0) <= 0.0;
    if (values.has(kOptQuality))
        return values.integer(kOptQuality, 0) >= 100;
    return true;  // a both-capable format with no knob supplied defaults to its lossless mode
}

std::vector<LossWarning> diff(const DocumentProfile& doc, const FormatCaps& caps,
                              const OptionValues& values, const MetadataRequest& want) {
    std::vector<LossWarning> out;

    // ---- pixels the file cannot represent -------------------------------------------------
    if (doc.hasAlpha) {
        if (caps.alpha == AlphaKind::None)
            add(out, Severity::HardLoss, LossCode::AlphaDropped, "Transparency",
                "this format has no alpha channel; transparent areas are filled with the matte "
                "colour");
        else if (caps.alpha == AlphaKind::Binary)
            add(out, Severity::HardLoss, LossCode::AlphaReducedToBinary, "Partial transparency",
                "this format stores pixels as either fully transparent or fully opaque; soft and "
                "anti-aliased edges become hard");
    }

    // ---- structure that survives only as flat pixels ---------------------------------------
    if (doc.hasMultipleLayers && !caps.layers)
        add(out, Severity::HardLoss, LossCode::LayersFlattened, "Layers",
            "all " + std::to_string(doc.layerCount) +
                " layers are flattened into one image; the stack cannot be recovered from the "
                "exported file");
    if (doc.hasVector && !caps.vector)
        add(out, Severity::HardLoss, LossCode::VectorRasterized, "Vector shapes",
            "resolution-independent geometry is rasterised at the export size");
    if (doc.hasText && !caps.vector)
        add(out, Severity::HardLoss, LossCode::TextRasterized, "Live text",
            "text is rasterised into pixels and can no longer be edited, restyled or re-flowed");
    // Effects and 3D text gate on `vector`, not `layers`, exactly as §4's table does: a
    // vector target re-emits them as geometry, a raster target -- layered or not, because a
    // TIFF page is still flat pixels -- bakes them.
    if (doc.hasEffects && !caps.vector)
        add(out, Severity::HardLoss, LossCode::EffectsBaked, "Layer effects",
            "strokes, shadows, glows, bevels and overlays are baked into the pixels");
    if (doc.hasExtrude3d && !caps.vector)
        add(out, Severity::HardLoss, LossCode::Extrude3dBaked, "3D text",
            "the extruded solid is baked into flat pixels; its depth, lighting and orientation "
            "are no longer adjustable");
    if (doc.hasAdjustments && !caps.layers)
        add(out, Severity::HardLoss, LossCode::AdjustmentsBaked, "Adjustment layers",
            "adjustments are baked into the pixels and can no longer be re-tuned");

    // ---- depth & palette --------------------------------------------------------------------
    if (!caps.floatPixels) {
        if (doc.sourceIsFloat)
            add(out, Severity::HardLoss, LossCode::HdrClipped, "High dynamic range",
                "floating-point pixels are clipped to the 0-1 range and quantised to " +
                    std::to_string(caps.maxBitDepth) + "-bit integers");
        else if (doc.sourceBitDepth > caps.maxBitDepth)
            add(out, Severity::HardLoss, LossCode::BitDepthReduced, "Bit depth",
                std::to_string(doc.sourceBitDepth) + "-bit channels are quantised to " +
                    std::to_string(caps.maxBitDepth) + " bits");
    }
    if (caps.maxColors >= 0 && (doc.distinctColors < 0 || doc.distinctColors > caps.maxColors))
        add(out, Severity::HardLoss, LossCode::ColorsQuantized, "Colours",
            (doc.distinctColors < 0 ? std::string("the image is truecolour")
                                    : "the image uses " + std::to_string(doc.distinctColors) +
                                          " colours") +
                " and is quantised to a palette of at most " + std::to_string(caps.maxColors));

    // ---- vector targets ---------------------------------------------------------------------
    // Only meaningful when the target keeps geometry at all: a raster export rasterises a conic
    // gradient exactly, so it is not a loss there.
    if (caps.vector) {
        if (doc.usesConicGradient && !caps.conicGradients)
            add(out, Severity::HardLoss, LossCode::ConicGradientRasterized, "Conic gradients",
                "this format has no conic gradient primitive; those fills are rasterised into an "
                "embedded bitmap");
        if (doc.usesBlendModes && !caps.blendModes)
            add(out, Severity::HardLoss, LossCode::BlendModesFlattened, "Blend modes and opacity",
                "this format composites opaquely; blended and semi-transparent layers are "
                "flattened into the artwork below them");
        // Amber, not red: outlining reproduces the stroke exactly, it only stops being a stroke.
        if (doc.usesStrokeAlign && !caps.strokeAlignment)
            add(out, Severity::Lossy, LossCode::StrokeAlignOutlined, "Inside/outside strokes",
                "this format strokes on the path centre only; those strokes are outlined into "
                "fills, which looks identical but is no longer an editable stroke");
    }

    // ---- generation loss --------------------------------------------------------------------
    const bool lossless = encodeIsLossless(caps, values);
    if (!lossless) {
        std::string how = "the pixels are re-encoded lossily; fine detail is discarded and "
                          "re-exporting an export compounds the loss";
        if (values.has(kOptQuality))
            how += " (quality " + std::to_string(values.integer(kOptQuality, 0)) + ")";
        else if (values.has(kOptDistance))
            how += " (distance " + common::gToString(values.number(kOptDistance, 0.0), 4) + ")";
        add(out, Severity::Lossy, LossCode::LossyEncode, "Encoding", std::move(how));
    }
    // A lossless encode cannot be subsampling anything, whatever the (hidden, remembered) ratio
    // in the bag says -- AVIF forces 4:4:4 in its lossless mode, and an option that is invisible
    // in the panel must not raise a warning about a thing the file does not do.
    if (caps.chromaSubsampling && !lossless && values.has(kOptSubsampling)) {
        const std::string ratio = values.text(kOptSubsampling, std::string(kSubsampling444));
        if (std::string_view(ratio) != kSubsampling444)
            add(out, Severity::Lossy, LossCode::ChromaSubsampled, "Chroma subsampling",
                "colour is stored at reduced resolution (" + ratio +
                    "); hard edges and fine coloured text soften");
    }

    // ---- colour management & metadata --------------------------------------------------------
    //
    // Each of these asks the same two-part question: is there something to carry, and CAN the
    // target carry it? The MetadataRequest gates the whole group rather than each line, because
    // a user who turned the row off has already been told what that means by the row itself --
    // see the rule on MetadataRequest (io/caps.hpp).
    if (want.embedIcc) {
        if (doc.hasICC && !caps.icc)
            add(out, Severity::Lossy, LossCode::IccDropped, "ICC colour profile",
                "this format cannot embed a profile; the file will be interpreted as sRGB");
        if (doc.hasNonSrgbSpace && !caps.icc)
            add(out, Severity::Lossy, LossCode::ColorSpaceConverted, "Working colour space",
                "this format carries no colour profile, so colours outside sRGB shift when the "
                "file is opened elsewhere");
    }
    if (want.keepMetadata) {
        if (doc.hasEXIF && !has(caps.metadata, MetadataKind::Exif))
            add(out, Severity::Lossy, LossCode::ExifDropped, "Camera metadata",
                "EXIF (camera, lens, date, GPS) is not written to this format");
        if (doc.hasXMP && !has(caps.metadata, MetadataKind::Xmp))
            add(out, Severity::Lossy, LossCode::XmpDropped, "XMP metadata",
                "XMP is not written to this format");
        if (std::abs(doc.dpi - 72.0) > 1e-9 && !has(caps.metadata, MetadataKind::Dpi))
            add(out, Severity::Lossy, LossCode::DpiDropped, "Print resolution",
                "the " + common::gToString(doc.dpi, 6) +
                    " dpi print resolution is not recorded; the file describes pixels only");
    }

    // Red before amber, otherwise declaration order (which is LossCode order, so the set is a
    // stable golden).
    std::stable_partition(out.begin(), out.end(),
                          [](const LossWarning& w) { return w.sev == Severity::HardLoss; });
    return out;
}

Severity worstSeverity(const std::vector<LossWarning>& warnings) noexcept {
    Severity worst = Severity::Fine;
    for (const LossWarning& w : warnings)
        if (w.sev > worst)
            worst = w.sev;
    return worst;
}

} // namespace mosaic::io
