#include "io/caps.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

// The S41 loss system (docs/export-system-plan.md §4): diff(DocumentProfile, FormatCaps,
// OptionValues) -> the exact set of warnings, severity by severity.
//
// Every assertion below pins the EXACT ordered set of LossCodes, never "some warning appeared":
// a diff() that warned about everything, or that lost the red-before-amber ordering, or that
// forgot a rule, all fail here. Each rule is also exercised against a format that DOES support
// the feature, so an implementation that always warns cannot pass.
//
// The capability rows are test-local on purpose. They are realistic (they are the §3.1 matrix)
// but they are NOT the shipping backends' caps -- tightening PngBackend::caps() must not force a
// rewrite of the loss goldens, and the backends' own rows are checked in test_export_registry.
using namespace mosaic::io;

namespace {

// The warnings as one "A|B|C" string: readable in a failure message, and exact.
std::string codeList(const std::vector<LossWarning>& warnings) {
    std::string s;
    for (const LossWarning& w : warnings) {
        if (!s.empty())
            s += '|';
        s += lossCodeName(w.code);
    }
    return s;
}

std::string codeList(const std::vector<LossWarning>& warnings, Severity sev) {
    std::string s;
    for (const LossWarning& w : warnings) {
        if (w.sev != sev)
            continue;
        if (!s.empty())
            s += '|';
        s += lossCodeName(w.code);
    }
    return s;
}

// ---- capability rows (§3.1) ----

FormatCaps capsPng() {
    FormatCaps c;
    c.channels = ChannelModel::Gray | ChannelModel::GrayAlpha | ChannelModel::Rgb |
                 ChannelModel::Rgba | ChannelModel::Indexed;
    c.maxBitDepth = 16;
    c.alpha = AlphaKind::Straight;
    c.icc = true;
    c.metadata = MetadataKind::Exif | MetadataKind::Xmp | MetadataKind::Text | MetadataKind::Dpi;
    c.lossless = true;
    c.lossy = false;
    return c;
}

FormatCaps capsJpeg() {
    FormatCaps c;
    c.channels = ChannelModel::Gray | ChannelModel::Rgb | ChannelModel::Cmyk;
    c.maxBitDepth = 8;
    c.alpha = AlphaKind::None;
    c.icc = true;
    c.metadata = MetadataKind::Exif | MetadataKind::Xmp | MetadataKind::Iptc | MetadataKind::Dpi;
    c.lossless = false;
    c.lossy = true;
    c.chromaSubsampling = true;
    return c;
}

FormatCaps capsGif() {
    FormatCaps c;
    c.channels = ChannelModel::Indexed;
    c.maxBitDepth = 8;
    c.alpha = AlphaKind::Binary;
    c.icc = false;
    c.metadata = MetadataKind::Text;
    c.lossless = true;
    c.lossy = false;
    c.animation = true;
    c.maxColors = 256;
    return c;
}

// Multi-page, 32-bit float capable, both lossy and lossless: the "carries almost everything"
// raster row, so a rule that fires here is a rule that fires unconditionally.
FormatCaps capsTiff() {
    FormatCaps c;
    c.channels = ChannelModel::Gray | ChannelModel::Rgb | ChannelModel::Rgba | ChannelModel::Cmyk;
    c.maxBitDepth = 32;
    c.floatPixels = true;
    c.alpha = AlphaKind::Either;
    c.icc = true;
    c.metadata = MetadataKind::Exif | MetadataKind::Xmp | MetadataKind::Iptc | MetadataKind::Dpi;
    c.lossless = true;
    c.lossy = true;
    c.layers = true;
    return c;
}

// QOI: the minimal lossless raster row -- alpha yes, everything else no.
FormatCaps capsQoi() {
    FormatCaps c;
    c.channels = ChannelModel::Rgb | ChannelModel::Rgba;
    c.maxBitDepth = 8;
    c.alpha = AlphaKind::Straight;
    c.icc = false;
    c.metadata = MetadataKind::None;
    c.lossless = true;
    c.lossy = false;
    return c;
}

FormatCaps capsSvg() {
    FormatCaps c;
    c.channels = ChannelModel::Rgba;
    c.maxBitDepth = 8;
    c.alpha = AlphaKind::Straight;
    c.icc = false;
    c.metadata = MetadataKind::Xmp;
    c.lossless = true;
    c.lossy = false;
    c.layers = true;
    c.vector = true;
    c.conicGradients = false;  // SVG 1.1 has linear + radial only
    c.strokeAlignment = false; // SVG strokes are centre-aligned only
    c.blendModes = true;       // mix-blend-mode
    return c;
}

// EPS/PostScript: vector, but it composites opaquely and keeps no stack.
FormatCaps capsEps() {
    FormatCaps c = capsSvg();
    c.layers = false;
    c.blendModes = false;
    c.alpha = AlphaKind::None;
    c.metadata = MetadataKind::None;
    return c;
}

// A document that loses nothing anywhere: one opaque layer, sRGB, 72 dpi, no metadata.
DocumentProfile plainDoc() {
    DocumentProfile p;
    p.layerCount = 1;
    p.distinctColors = 4;
    return p;
}

} // namespace

TEST_CASE("a faithful export produces no warnings at all") {
    const DocumentProfile doc = plainDoc();
    const std::vector<LossWarning> w = diff(doc, capsPng(), {});
    CHECK(codeList(w) == "");
    CHECK(w.empty());
    CHECK(worstSeverity(w) == Severity::Fine);
    // ...and the same document survives a format that carries strictly less.
    CHECK(codeList(diff(doc, capsQoi(), {})) == "");
}

TEST_CASE("alpha: dropped, reduced or carried, according to the format") {
    DocumentProfile doc = plainDoc();
    doc.hasAlpha = true;

    // PNG and QOI carry straight alpha -- nothing to say.
    CHECK(codeList(diff(doc, capsPng(), {})) == "");
    CHECK(codeList(diff(doc, capsQoi(), {})) == "");

    // JPEG has no alpha channel at all. (Its LossyEncode rides along: JPEG has no lossless mode.)
    CHECK(codeList(diff(doc, capsJpeg(), {})) == "AlphaDropped|LossyEncode");

    // GIF has alpha, but only one bit of it -- a different warning, not the same one.
    CHECK(codeList(diff(doc, capsGif(), {})) == "AlphaReducedToBinary");

    // Exactly one of the two alpha warnings ever fires.
    doc.hasAlpha = false;
    CHECK(codeList(diff(doc, capsJpeg(), {})) == "LossyEncode");
    CHECK(codeList(diff(doc, capsGif(), {})) == "");
}

TEST_CASE("layers only flatten when there is more than one, and not into a paged format") {
    DocumentProfile doc = plainDoc();
    CHECK(codeList(diff(doc, capsQoi(), {})) == ""); // one layer: nothing is flattened

    doc.layerCount = 4;
    doc.hasMultipleLayers = true;
    CHECK(codeList(diff(doc, capsQoi(), {})) == "LayersFlattened");
    CHECK(codeList(diff(doc, capsTiff(), {})) == ""); // multi-page: the stack survives

    // The warning teaches: the count is in the text.
    const std::vector<LossWarning> w = diff(doc, capsQoi(), {});
    REQUIRE(w.size() == 1);
    CHECK(w[0].sev == Severity::HardLoss);
    CHECK(w[0].feature == "Layers");
    CHECK(w[0].consequence.find("all 4 layers") != std::string::npos);
}

TEST_CASE("vector, text, effects and 3D rasterize into raster targets and survive vector ones") {
    DocumentProfile doc = plainDoc();
    doc.hasVector = true;
    doc.hasText = true;
    doc.hasEffects = true;
    doc.hasExtrude3d = true;
    doc.hasAdjustments = true;
    doc.layerCount = 5;
    doc.hasMultipleLayers = true;

    CHECK(codeList(diff(doc, capsPng(), {})) ==
          "LayersFlattened|VectorRasterized|TextRasterized|EffectsBaked|Extrude3dBaked|"
          "AdjustmentsBaked");

    // A paged raster format keeps the stack but still bakes the geometry and the effects.
    CHECK(codeList(diff(doc, capsTiff(), {})) ==
          "VectorRasterized|TextRasterized|EffectsBaked|Extrude3dBaked");

    // SVG keeps geometry, live text, a group tree and re-emitted effects: nothing is lost.
    CHECK(codeList(diff(doc, capsSvg(), {})) == "");
}

TEST_CASE("the vector-only rules stay silent against a raster target") {
    DocumentProfile doc = plainDoc();
    doc.usesConicGradient = true;
    doc.usesStrokeAlign = true;
    doc.usesBlendModes = true;
    doc.hasVector = true;

    // A PNG rasterises the conic gradient and the inside stroke exactly; only the geometry
    // itself is a loss. A diff that fired the vector rules everywhere would fail here.
    CHECK(codeList(diff(doc, capsPng(), {})) == "VectorRasterized");

    // SVG: no conic primitive, centre-only strokes -- but it does have mix-blend-mode.
    CHECK(codeList(diff(doc, capsSvg(), {})) ==
          "ConicGradientRasterized|StrokeAlignOutlined");
    CHECK(codeList(diff(doc, capsSvg(), {}), Severity::HardLoss) == "ConicGradientRasterized");
    CHECK(codeList(diff(doc, capsSvg(), {}), Severity::Lossy) == "StrokeAlignOutlined");

    // EPS composites opaquely and keeps no stack.
    CHECK(codeList(diff(doc, capsEps(), {})) ==
          "ConicGradientRasterized|BlendModesFlattened|StrokeAlignOutlined");
}

TEST_CASE("colour quantization keys off the counted colours, not the format alone") {
    DocumentProfile doc = plainDoc();

    doc.distinctColors = 12;
    CHECK(codeList(diff(doc, capsGif(), {})) == ""); // 12 colours fit a 256-entry palette
    doc.distinctColors = 256;
    CHECK(codeList(diff(doc, capsGif(), {})) == ""); // exactly the ceiling still fits
    doc.distinctColors = 257;
    CHECK(codeList(diff(doc, capsGif(), {})) == "ColorsQuantized");
    doc.distinctColors = -1; // uncounted / above the cap == treat as truecolour
    CHECK(codeList(diff(doc, capsGif(), {})) == "ColorsQuantized");

    // Truecolour formats never quantize, however many colours the flatten has.
    CHECK(codeList(diff(doc, capsPng(), {})) == "");
    CHECK(codeList(diff(doc, capsQoi(), {})) == "");
}

TEST_CASE("bit depth and HDR: one warning, chosen by what the source can actually deliver") {
    DocumentProfile doc = plainDoc();

    // Today's pipeline hands the encoder 8-bit pixels, so a default document -- whose declared
    // precision is F16! -- must NOT raise a permanent HDR banner (§5 high-bit note).
    doc.precision = mosaic::core::Precision::F16;
    CHECK(doc.sourceBitDepth == 8);
    CHECK_FALSE(doc.sourceIsFloat);
    CHECK(codeList(diff(doc, capsPng(), {})) == "");
    CHECK(codeList(diff(doc, capsQoi(), {})) == "");

    doc.sourceBitDepth = 16;
    CHECK(codeList(diff(doc, capsPng(), {})) == "");       // PNG carries 16-bit
    CHECK(codeList(diff(doc, capsQoi(), {})) == "BitDepthReduced"); // QOI is 8-bit only
    CHECK(codeList(diff(doc, capsTiff(), {})) == "");      // TIFF carries 32

    doc.sourceIsFloat = true;
    // Float beats depth: exactly one warning, and it is the HDR one.
    CHECK(codeList(diff(doc, capsPng(), {})) == "HdrClipped");
    CHECK(codeList(diff(doc, capsQoi(), {})) == "HdrClipped");
    CHECK(codeList(diff(doc, capsTiff(), {})) == ""); // float samples survive
}

TEST_CASE("encodeIsLossless follows the shared option vocabulary") {
    OptionValues v;

    // A format with no lossy mode is always lossless, whatever the bag says.
    CHECK(encodeIsLossless(capsPng(), v));
    v.set(kOptQuality, intValue(10));
    CHECK(encodeIsLossless(capsPng(), v));

    // A format with no lossless mode never is -- not even at quality 100.
    v.clear();
    v.set(kOptQuality, intValue(100));
    CHECK_FALSE(encodeIsLossless(capsJpeg(), v));

    // A both-capable format: the explicit flag wins, then distance, then quality.
    const FormatCaps both = capsTiff();
    v.clear();
    CHECK(encodeIsLossless(both, v)); // no knob supplied => its lossless mode
    v.set(kOptLossless, boolValue(true));
    v.set(kOptDistance, realValue(3.0));
    v.set(kOptQuality, intValue(40));
    CHECK(encodeIsLossless(both, v)); // the flag outranks both numbers
    v.set(kOptLossless, boolValue(false));
    CHECK_FALSE(encodeIsLossless(both, v));
    v.erase(kOptLossless);
    CHECK_FALSE(encodeIsLossless(both, v)); // distance 3 outranks quality
    v.set(kOptDistance, realValue(0.0));
    CHECK(encodeIsLossless(both, v));
    v.erase(kOptDistance);
    CHECK_FALSE(encodeIsLossless(both, v)); // quality 40
    v.set(kOptQuality, intValue(100));
    CHECK(encodeIsLossless(both, v));
}

TEST_CASE("lossy encoding and chroma subsampling are separate amber warnings") {
    const DocumentProfile doc = plainDoc();

    OptionValues v;
    v.set(kOptQuality, intValue(85));
    v.set(kOptSubsampling, textValue("4:2:0"));
    CHECK(codeList(diff(doc, capsJpeg(), v)) == "LossyEncode|ChromaSubsampled");
    CHECK(worstSeverity(diff(doc, capsJpeg(), v)) == Severity::Lossy);

    // 4:4:4 keeps full chroma: the subsampling warning goes, the encode one stays.
    v.set(kOptSubsampling, textValue(std::string(kSubsampling444)));
    CHECK(codeList(diff(doc, capsJpeg(), v)) == "LossyEncode");

    // A format that does not subsample ignores a stray subsampling key entirely.
    v.set(kOptSubsampling, textValue("4:2:0"));
    CHECK(codeList(diff(doc, capsPng(), v)) == "");

    // A both-capable format in its lossless mode is clean; flipping the flag turns it amber.
    OptionValues t;
    t.set(kOptLossless, boolValue(true));
    CHECK(codeList(diff(doc, capsTiff(), t)) == "");
    t.set(kOptLossless, boolValue(false));
    CHECK(codeList(diff(doc, capsTiff(), t)) == "LossyEncode");

    // The quality is echoed into the text so the banner can be read without the panel.
    OptionValues q;
    q.set(kOptQuality, intValue(72));
    const std::vector<LossWarning> w = diff(doc, capsJpeg(), q);
    REQUIRE(w.size() == 1);
    CHECK(w[0].consequence.find("quality 72") != std::string::npos);
}

TEST_CASE("colour management and metadata are amber, and only when the format cannot hold them") {
    DocumentProfile doc = plainDoc();
    doc.hasICC = true;
    doc.hasNonSrgbSpace = true;
    doc.hasEXIF = true;
    doc.hasXMP = true;
    doc.dpi = 300.0;

    CHECK(codeList(diff(doc, capsPng(), {})) == ""); // PNG carries profile, EXIF, XMP and pHYs
    CHECK(codeList(diff(doc, capsQoi(), {})) ==
          "IccDropped|ColorSpaceConverted|ExifDropped|XmpDropped|DpiDropped");
    CHECK(worstSeverity(diff(doc, capsQoi(), {})) == Severity::Lossy);

    // GIF holds a text comment only: no profile, no EXIF, no XMP, no density. Its 4 colours
    // still fit the palette, so nothing red joins in.
    CHECK(codeList(diff(doc, capsGif(), {})) ==
          "IccDropped|ColorSpaceConverted|ExifDropped|XmpDropped|DpiDropped");

    // 72 dpi is the "no opinion" default and never warns.
    doc.dpi = 72.0;
    CHECK(codeList(diff(doc, capsQoi(), {})) ==
          "IccDropped|ColorSpaceConverted|ExifDropped|XmpDropped");
}

TEST_CASE("red sorts before amber, and the order within a severity is stable") {
    DocumentProfile doc = plainDoc();
    doc.hasAlpha = true;
    doc.layerCount = 3;
    doc.hasMultipleLayers = true;
    doc.hasText = true;
    doc.hasICC = true;
    doc.hasEXIF = true;
    doc.dpi = 600.0;
    doc.distinctColors = -1; // truecolour

    OptionValues v;
    v.set(kOptQuality, intValue(60));
    v.set(kOptSubsampling, textValue("4:2:0"));

    const std::vector<LossWarning> w = diff(doc, capsGif(), v);
    // GIF: red first (binary alpha, flatten, rasterised text, quantized), then amber (no ICC,
    // no EXIF, no density). GIF is lossless and does not subsample, so neither of the encode
    // warnings appears despite both keys being present.
    CHECK(codeList(w) == "AlphaReducedToBinary|LayersFlattened|TextRasterized|ColorsQuantized|"
                         "IccDropped|ExifDropped|DpiDropped");
    CHECK(worstSeverity(w) == Severity::HardLoss);

    // The partition is stable: every red one precedes every amber one, and no entry is Fine.
    bool seenAmber = false;
    for (const LossWarning& one : w) {
        CHECK(one.sev != Severity::Fine);
        if (one.sev == Severity::Lossy)
            seenAmber = true;
        else
            CHECK_FALSE(seenAmber);
        CHECK_FALSE(one.feature.empty());
        CHECK_FALSE(one.consequence.empty());
    }
    CHECK(seenAmber);
}

TEST_CASE("every LossCode has a distinct stable name") {
    // A duplicated or missing case in lossCodeName() would silently corrupt every golden above.
    const LossCode all[] = {
        LossCode::AlphaDropped,     LossCode::AlphaReducedToBinary, LossCode::LayersFlattened,
        LossCode::VectorRasterized, LossCode::TextRasterized,       LossCode::EffectsBaked,
        LossCode::Extrude3dBaked,   LossCode::AdjustmentsBaked,     LossCode::BitDepthReduced,
        LossCode::HdrClipped,       LossCode::ColorsQuantized,      LossCode::LossyEncode,
        LossCode::ChromaSubsampled, LossCode::IccDropped,           LossCode::ColorSpaceConverted,
        LossCode::ExifDropped,      LossCode::XmpDropped,           LossCode::DpiDropped,
        LossCode::ConicGradientRasterized, LossCode::StrokeAlignOutlined,
        LossCode::BlendModesFlattened};
    std::vector<std::string> names;
    for (const LossCode c : all) {
        const std::string n(lossCodeName(c));
        CHECK(n != "?");
        for (const std::string& seen : names)
            CHECK(seen != n);
        names.push_back(n);
    }
    CHECK(names.size() == 21);
}
