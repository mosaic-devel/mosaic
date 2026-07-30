#pragma once

#include "io/document_profile.hpp"
#include "io/options_schema.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// io/caps -- the loss-warning system (Export & I/O plan §4), the core of S41.
//
//   FormatCaps        what a BACKEND can carry (not what the container theoretically could:
//                     caps describe what OUR encoder actually writes, or diff() would lie)
//   DocumentProfile   what the document actually uses (io/document_profile.hpp)
//   diff()            the pure function between them + the chosen encoder options
//
// diff() is FLTK-free, allocation-only, and depends on nothing but its three arguments -- it is
// the thing the Export modal's live banner calls on every format switch and every slider tick,
// and it is exhaustively golden-tested (tests/test_export_loss.cpp).
namespace mosaic::io {

// ---------------------------------------------------------------------------------------------
// FormatCaps
// ---------------------------------------------------------------------------------------------

// The channel models a format can store, as a bitmask (a format usually supports several and
// the encoder picks; PNG is G, GA, RGB, RGBA and Indexed).
enum class ChannelModel : std::uint16_t {
    None = 0,
    Gray = 1u << 0,
    GrayAlpha = 1u << 1,
    Rgb = 1u << 2,
    Rgba = 1u << 3,
    Indexed = 1u << 4,
    Cmyk = 1u << 5,
    Lab = 1u << 6,
};

[[nodiscard]] constexpr ChannelModel operator|(ChannelModel a, ChannelModel b) noexcept {
    return static_cast<ChannelModel>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr ChannelModel operator&(ChannelModel a, ChannelModel b) noexcept {
    return static_cast<ChannelModel>(static_cast<std::uint16_t>(a) & static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr bool has(ChannelModel set, ChannelModel bit) noexcept {
    return (set & bit) != ChannelModel::None;
}

// How a format records transparency -- the WIDEST kind it supports.
enum class AlphaKind : std::uint8_t {
    None,           // JPEG, PNM (non-PAM), Radiance HDR: transparency cannot be carried at all
    Binary,         // GIF: one fully-transparent palette index, no partial coverage
    Straight,       // PNG, WebP, QOI, PAM: unassociated alpha, our pipeline's own convention
    Premultiplied,  // OpenEXR's associated-alpha convention
    Either,         // JXL / AVIF / TIFF / TGA: the file records which of the two it holds
};

// Metadata containers a format can carry, as a bitmask. `Dpi` is the physical-density record
// (PNG pHYs, JFIF density, TIFF resolution), not an EXIF tag.
enum class MetadataKind : std::uint16_t {
    None = 0,
    Exif = 1u << 0,
    Xmp = 1u << 1,
    Iptc = 1u << 2,
    Text = 1u << 3,
    Dpi = 1u << 4,
};

[[nodiscard]] constexpr MetadataKind operator|(MetadataKind a, MetadataKind b) noexcept {
    return static_cast<MetadataKind>(static_cast<std::uint16_t>(a) |
                                     static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr MetadataKind operator&(MetadataKind a, MetadataKind b) noexcept {
    return static_cast<MetadataKind>(static_cast<std::uint16_t>(a) &
                                     static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr bool has(MetadataKind set, MetadataKind bit) noexcept {
    return (set & bit) != MetadataKind::None;
}

// Pure data, straight off the §3.1 capability matrix -- one row per format.
//
// The rule that keeps this honest: a field states what THIS BACKEND WRITES TODAY, never what the
// specification permits. PNG's container has APNG and 16-bit; our encoder writes neither, so our
// PNG caps say animation=false, maxBitDepth=8. Otherwise the loss banner would promise depth the
// file will not receive -- and the banner is the one place in the app that must never be wrong.
struct FormatCaps {
    ChannelModel channels = ChannelModel::Rgba;
    int maxBitDepth = 8;         // integer bits per channel the encoder writes
    bool floatPixels = false;    // 16/32-bit float samples (the HDR tier's gate)
    AlphaKind alpha = AlphaKind::None;
    bool icc = false;            // an ICC profile can be embedded
    MetadataKind metadata = MetadataKind::None;
    bool lossless = true;        // has a mode that writes the pixels exactly
    bool lossy = false;          // has a lossy mode
    bool animation = false;      // multi-frame playback
    int maxColors = -1;          // palette ceiling; -1 = truecolour
    bool layers = false;         // a layer/page stack survives (multi-page TIFF, PDF, .mosaic)
    bool chromaSubsampling = false;  // the encoder can subsample chroma

    // Vector targets only (SVG / PDF / EPS). `vector` gates the three below: against a raster
    // target, geometry is rasterised exactly and none of these is a loss.
    bool vector = false;
    bool conicGradients = false;  // a conic/sweep gradient primitive exists (SVG 1.1: no)
    bool strokeAlignment = false; // inside/outside strokes exist (SVG: centre-only)
    bool blendModes = false;      // per-object blend modes survive (EPS/PS: no)

    bool operator==(const FormatCaps&) const = default;
};

// ---------------------------------------------------------------------------------------------
// What the user asked the export to CARRY
// ---------------------------------------------------------------------------------------------

// The third input diff() needs, and the reason it needs one: §6's Metadata toggle and Colour row
// are the user telling us to leave something out. Dropping EXIF the user asked to strip is not a
// loss -- it is the request being honoured -- and a banner that warned about it would teach the
// user that the banner is noise. So the rule, stated once and applied consistently:
//
//   THE BANNER REPORTS WHAT THE FORMAT CANNOT CARRY, NEVER WHAT THE USER CHOSE TO OMIT.
//
// Which is also why this struct can only ever REMOVE warnings: its defaults are "keep everything",
// so a caller that has no opinion gets exactly the pre-M5 answer and every golden still holds.
struct MetadataRequest {
    bool keepMetadata = true;  // §6's Metadata toggle: EXIF / XMP / the density record ride along
    bool embedIcc = true;      // §6's Colour row: a profile is embedded

    bool operator==(const MetadataRequest&) const = default;
};

// ---------------------------------------------------------------------------------------------
// Loss warnings
// ---------------------------------------------------------------------------------------------

// §4's three-level scale, and the rule that decides which one a warning gets:
//
//   HardLoss  the PICTURE or its EDITABILITY is destroyed -- pixels the file cannot represent
//             (alpha, depth, colour count) or structure that only survives as flat pixels
//             (layers, vector geometry, live text, effects). Red banner.
//   Lossy     the file is written with generation loss, or side-car information that is not
//             pixels is dropped (ICC, EXIF, DPI). Amber banner.
//   Fine      nothing was lost. NO WARNING EVER CARRIES THIS -- diff() returns an EMPTY vector
//             and worstSeverity() answers Fine, which is the green check. Keeping Fine out of
//             the list is what lets a golden test assert an exact set instead of filtering.
enum class Severity : std::uint8_t { Fine, Lossy, HardLoss };

// A stable identity for each warning, so the UI can translate (and a test can assert) without
// matching on prose. Never renumber: presets and goldens key on the name, not the value.
enum class LossCode : std::uint16_t {
    AlphaDropped,
    AlphaReducedToBinary,
    LayersFlattened,
    VectorRasterized,
    TextRasterized,
    EffectsBaked,
    Extrude3dBaked,
    AdjustmentsBaked,
    BitDepthReduced,
    HdrClipped,
    ColorsQuantized,
    LossyEncode,
    ChromaSubsampled,
    IccDropped,
    ColorSpaceConverted,
    ExifDropped,
    XmpDropped,
    DpiDropped,
    ConicGradientRasterized,
    StrokeAlignOutlined,
    BlendModesFlattened,
};

[[nodiscard]] std::string_view lossCodeName(LossCode code) noexcept;

// `feature` and `consequence` are ENGLISH SOURCE STRINGS, deliberately not translated here:
// the io module is FLTK- and gettext-free, and translating inside diff() would make the goldens
// locale-dependent. The M3 banner translates by `code` and can format its own text; the strings
// are the fallback and the test oracle.
struct LossWarning {
    Severity sev = Severity::Fine;
    LossCode code = LossCode::AlphaDropped;
    std::string feature;      // "Transparency"
    std::string consequence;  // "the transparent areas are filled with the matte colour"

    bool operator==(const LossWarning&) const = default;
};

// Everything the target format will not carry, given what the document uses and how the encoder
// is configured. Deterministic order (declaration order of LossCode above): HardLoss entries
// first, then Lossy. An empty result means the export is faithful -- the green check.
//
// `values` should already have been through OptionsSchema::coerce(); diff() reads only the
// well-known keys (options_schema.hpp) and tolerates their absence. `want` defaults to "carry
// everything", which is the answer every caller wanted before the Metadata/Colour rows existed.
[[nodiscard]] std::vector<LossWarning> diff(const DocumentProfile& doc, const FormatCaps& caps,
                                            const OptionValues& values,
                                            const MetadataRequest& want = {});

// The banner colour: the worst severity present, or Fine when the list is empty.
[[nodiscard]] Severity worstSeverity(const std::vector<LossWarning>& warnings) noexcept;

// Whether the encode `values` select a mode that writes the pixels exactly. The shared
// vocabulary of options_schema.hpp is the whole mechanism: an explicit `lossless` bool wins,
// then a `distance` of 0, then a `quality` of 100; with no knob at all the format's own
// lossless-ness decides. A format with NO lossless mode (JPEG) is never lossless -- not even at
// quality 100, which is still DCT-quantised. (§4's "Lossy = quality<100" is the amber TRIGGER;
// this is the underlying fact, and it is why a JPEG export is amber at every quality.)
[[nodiscard]] bool encodeIsLossless(const FormatCaps& caps, const OptionValues& values) noexcept;

} // namespace mosaic::io
